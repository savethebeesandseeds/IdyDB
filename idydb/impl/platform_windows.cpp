#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif
#ifndef NTDDI_VERSION
#define NTDDI_VERSION 0x0A000002
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <io.h>
#include <fcntl.h>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <openssl/rand.h>

#include "db.h"
#include "platform.h"

namespace idydb_platform {
namespace {

// All names entering this backend are UTF-8, including the names returned by
// canonical_path. Wide, extended-length names never pass through the CRT's
// current ANSI code page.
bool ordinary_name(const wchar_t* name) {
    const bool drive = std::wcsncmp(name, L"\\\\?\\", 4) == 0 &&
        ((name[4] >= L'A' && name[4] <= L'Z') || (name[4] >= L'a' && name[4] <= L'z')) &&
        name[5] == L':' && name[6] == L'\\';
    const bool unc = _wcsnicmp(name, L"\\\\?\\UNC\\", 8) == 0;
    // A database is a regular default data stream, not a DOS device or an ADS.
    if ((!drive && !unc) || std::wcschr(name + (drive ? 6 : 8), L':')) {
        errno = ENOTSUP; return false;
    }
    return true;
}

wchar_t* wide_name(const char* name) {
    if (!name || !*name) { errno = EINVAL; return nullptr; }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, nullptr, 0);
    if (!count) { errno = EILSEQ; return nullptr; }
    wchar_t* result = static_cast<wchar_t*>(std::malloc(static_cast<size_t>(count) * sizeof(wchar_t)));
    if (!result) { errno = ENOMEM; return nullptr; }
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, result, count)) {
        std::free(result); errno = EILSEQ; return nullptr;
    }
    if (std::wcsncmp(result, L"\\\\?\\", 4) == 0) {
        if (ordinary_name(result)) return result;
        std::free(result); return nullptr;
    }
    if (std::wcsncmp(result, L"\\\\.\\", 4) == 0 || std::wcsncmp(result, L"\\??\\", 4) == 0) {
        std::free(result); errno = ENOTSUP; return nullptr;
    }
    // An extended-length absolute name removes dependence on the executable's
    // longPathAware manifest and the machine-wide LongPathsEnabled setting.
    DWORD capacity = GetFullPathNameW(result, 0, nullptr, nullptr);
    if (!capacity) { std::free(result); errno = EINVAL; return nullptr; }
    wchar_t* absolute = nullptr;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        if (capacity >= static_cast<DWORD>(INT_MAX)) { errno = ENAMETOOLONG; break; }
        absolute = static_cast<wchar_t*>(std::malloc((static_cast<size_t>(capacity) + 1) * sizeof(wchar_t)));
        if (!absolute) { errno = ENOMEM; break; }
        const DWORD length = GetFullPathNameW(result, capacity + 1, absolute, nullptr);
        if (length && length <= capacity) break;
        std::free(absolute); absolute = nullptr;
        if (!length) { errno = EINVAL; break; }
        capacity = length;
    }
    std::free(result);
    if (!absolute) return nullptr;
    const bool unc = absolute[0] == L'\\' && absolute[1] == L'\\';
    const size_t length = std::wcslen(absolute);
    const size_t prefix = unc ? 8 : 4;
    const size_t skip = unc ? 2 : 0;
    if (length > SIZE_MAX / sizeof(wchar_t) - prefix - 1) {
        std::free(absolute); errno = ENAMETOOLONG; return nullptr;
    }
    result = static_cast<wchar_t*>(std::malloc((prefix + length - skip + 1) * sizeof(wchar_t)));
    if (!result) { std::free(absolute); errno = ENOMEM; return nullptr; }
    std::memcpy(result, unc ? L"\\\\?\\UNC\\" : L"\\\\?\\", prefix * sizeof(wchar_t));
    std::memcpy(result + prefix, absolute + skip, (length - skip + 1) * sizeof(wchar_t));
    std::free(absolute);
    if (!ordinary_name(result)) { std::free(result); return nullptr; }
    return result;
}

char* utf8_name(const wchar_t* name) {
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name, -1, nullptr, 0, nullptr, nullptr);
    if (!count) { errno = EILSEQ; return nullptr; }
    char* result = static_cast<char*>(std::malloc(static_cast<size_t>(count)));
    if (!result) { errno = ENOMEM; return nullptr; }
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, name, -1, result, count, nullptr, nullptr)) {
        std::free(result); errno = EILSEQ; return nullptr;
    }
    return result;
}

int win_errno(DWORD error) {
    switch (error) {
        case ERROR_FILE_NOT_FOUND: case ERROR_PATH_NOT_FOUND: return ENOENT;
        case ERROR_SHARING_VIOLATION: case ERROR_LOCK_VIOLATION: return EBUSY;
        case ERROR_ACCESS_DENIED: case ERROR_PRIVILEGE_NOT_HELD: return EACCES;
        case ERROR_DISK_FULL: case ERROR_HANDLE_DISK_FULL: return ENOSPC;
        case ERROR_NOT_ENOUGH_MEMORY: case ERROR_OUTOFMEMORY: return ENOMEM;
        case ERROR_ALREADY_EXISTS: case ERROR_FILE_EXISTS: return EEXIST;
        case ERROR_INVALID_HANDLE: return EBADF;
        case ERROR_FILENAME_EXCED_RANGE: return ENAMETOOLONG;
        case ERROR_INVALID_NAME: case ERROR_INVALID_PARAMETER: return EINVAL;
        case ERROR_NOT_SUPPORTED: case ERROR_INVALID_FUNCTION: return ENOTSUP;
        default: return EIO;
    }
}

int fail_win(DWORD error) { errno = win_errno(error); return -1; }

int open_status(DWORD error) {
    errno = win_errno(error);
    if (errno == ENOENT) return IDYDB_NOT_FOUND;
    if (errno == EBUSY) return IDYDB_BUSY;
    if (errno == EACCES) return IDYDB_PERM;
    return IDYDB_ERROR;
}

HANDLE native_handle(int fd) {
    if (fd < 0) { errno = EBADF; return INVALID_HANDLE_VALUE; }
    const intptr_t value = _get_osfhandle(fd);
    if (value == -1) { errno = EBADF; return INVALID_HANDLE_VALUE; }
    return reinterpret_cast<HANDLE>(value);
}

bool regular(HANDLE handle, BY_HANDLE_FILE_INFORMATION* information) {
    if (GetFileType(handle) != FILE_TYPE_DISK) { errno = ENOTSUP; return false; }
    if (!GetFileInformationByHandle(handle, information)) { fail_win(GetLastError()); return false; }
    // Refuse final-component reparse points instead of reading one file and
    // subsequently replacing the link. Parent junctions are resolved by the
    // handle-derived canonical name before any sibling stage is constructed.
    if (information->dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        errno = ENOTSUP; return false;
    }
    return true;
}

bool same_file(HANDLE left, HANDLE right) {
    BY_HANDLE_FILE_INFORMATION a, b;
    return regular(left, &a) && regular(right, &b) &&
        a.dwVolumeSerialNumber == b.dwVolumeSerialNumber &&
        a.nFileIndexHigh == b.nFileIndexHigh && a.nFileIndexLow == b.nFileIndexLow;
}

FILE* own_stream(HANDLE handle, bool readonly) {
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(handle),
        (readonly ? _O_RDONLY : _O_RDWR) | _O_BINARY | _O_NOINHERIT);
    if (fd < 0) { CloseHandle(handle); return nullptr; }
    FILE* stream = _fdopen(fd, readonly ? "rb" : "r+b");
    if (!stream) _close(fd);
    return stream;
}

wchar_t* final_name(HANDLE handle) {
    const DWORD flags = FILE_NAME_NORMALIZED | VOLUME_NAME_DOS;
    DWORD size = GetFinalPathNameByHandleW(handle, nullptr, 0, flags);
    if (!size) { fail_win(GetLastError()); return nullptr; }
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        if (size >= static_cast<DWORD>(INT_MAX) || size > SIZE_MAX / sizeof(wchar_t) - 1) {
            errno = ENAMETOOLONG; return nullptr;
        }
        wchar_t* name = static_cast<wchar_t*>(std::malloc((static_cast<size_t>(size) + 1) * sizeof(wchar_t)));
        if (!name) { errno = ENOMEM; return nullptr; }
        const DWORD length = GetFinalPathNameByHandleW(handle, name, size + 1, flags);
        if (length && length <= size) return name;
        const DWORD error = GetLastError();
        std::free(name);
        if (!length) { fail_win(error); return nullptr; }
        size = length;
    }
    errno = EBUSY;
    return nullptr;
}

// CREATE_NEW and an unpredictable name avoid the create/close/reopen race of
// GetTempFileName. The descriptor and native handle are never inheritable.
bool random_suffix(wchar_t* output) {
    unsigned char bytes[16];
    if (RAND_bytes(bytes, sizeof(bytes)) != 1) { errno = EIO; return false; }
    const wchar_t digits[] = L"0123456789abcdef";
    for (unsigned i = 0; i < sizeof(bytes); ++i) {
        output[i * 2] = digits[bytes[i] >> 4];
        output[i * 2 + 1] = digits[bytes[i] & 15];
    }
    output[32] = L'\0';
    return true;
}

bool preserve_dacl(HANDLE handle, PSECURITY_DESCRIPTOR original) {
    BOOL present = FALSE, defaulted = FALSE;
    PACL acl = nullptr;
    SECURITY_DESCRIPTOR_CONTROL before = 0;
    DWORD revision = 0;
    if (!GetSecurityDescriptorDacl(original, &present, &acl, &defaulted) ||
        !GetSecurityDescriptorControl(original, &before, &revision)) {
        fail_win(GetLastError()); return false;
    }
    // File creation can map generic ACE masks and clear auto-inheritance
    // bookkeeping, even when SECURITY_ATTRIBUTES supplied a protected DACL.
    // Set the original ACL explicitly before any database bytes are copied.
    const SECURITY_INFORMATION protection = (before & SE_DACL_PROTECTED) ?
        PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION;
    DWORD status = SetSecurityInfo(handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | protection,
        nullptr, nullptr, present ? acl : nullptr, nullptr);
    if (status != ERROR_SUCCESS) { fail_win(status); return false; }
    PSECURITY_DESCRIPTOR actual = nullptr;
    PACL copied = nullptr;
    status = GetSecurityInfo(handle, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION,
        nullptr, nullptr, &copied, nullptr, &actual);
    if (status != ERROR_SUCCESS) { fail_win(status); return false; }
    SECURITY_DESCRIPTOR_CONTROL after = 0;
    const bool inspected = GetSecurityDescriptorControl(actual, &after, &revision) != FALSE;
    const SECURITY_DESCRIPTOR_CONTROL mask = SE_DACL_PRESENT | SE_DACL_PROTECTED |
        SE_DACL_AUTO_INHERITED | SE_DACL_AUTO_INHERIT_REQ;
    const bool same = inspected && (before & mask) == (after & mask) &&
        ((!acl && !copied) || (acl && copied && acl->AclSize == copied->AclSize &&
          std::memcmp(acl, copied, acl->AclSize) == 0));
    LocalFree(actual);
    if (!same) { errno = EACCES; return false; }
    return true;
}

struct private_security {
    HANDLE token = nullptr;
    TOKEN_USER* user = nullptr;
    ACL* acl = nullptr;
    SECURITY_DESCRIPTOR descriptor = {};
    SECURITY_ATTRIBUTES attributes = {};
    ~private_security() {
        std::free(acl); std::free(user);
        if (token) CloseHandle(token);
    }
    bool initialize() {
        if (!OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
            const DWORD error = GetLastError();
            if (error != ERROR_NO_TOKEN || !OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
                fail_win(error == ERROR_NO_TOKEN ? GetLastError() : error); return false;
            }
        }
        DWORD bytes = 0;
        GetTokenInformation(token, TokenUser, nullptr, 0, &bytes);
        if (!bytes) { fail_win(GetLastError()); return false; }
        user = static_cast<TOKEN_USER*>(std::malloc(bytes));
        if (!user) { errno = ENOMEM; return false; }
        if (!GetTokenInformation(token, TokenUser, user, bytes, &bytes)) { fail_win(GetLastError()); return false; }
        const DWORD acl_size = static_cast<DWORD>(sizeof(ACL) + sizeof(ACCESS_ALLOWED_ACE) +
            GetLengthSid(user->User.Sid) - sizeof(DWORD));
        acl = static_cast<ACL*>(std::malloc(acl_size));
        if (!acl) { errno = ENOMEM; return false; }
        if (!InitializeAcl(acl, acl_size, ACL_REVISION) ||
            !AddAccessAllowedAceEx(acl, ACL_REVISION, 0, FILE_ALL_ACCESS, user->User.Sid) ||
            !InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
            !SetSecurityDescriptorOwner(&descriptor, user->User.Sid, FALSE) ||
            !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) ||
            !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED)) {
            fail_win(GetLastError()); return false;
        }
        attributes.nLength = sizeof(attributes);
        attributes.lpSecurityDescriptor = &descriptor;
        attributes.bInheritHandle = FALSE;
        return true;
    }
};

wchar_t* temporary_directory() {
    // GetTempPath2 chooses the service-safe temp directory when available;
    // loading it dynamically retains compatibility with earlier Windows 10.
    using temp_function = DWORD (WINAPI*)(DWORD, LPWSTR);
    HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    temp_function get_temp = nullptr;
    const FARPROC address = kernel ? GetProcAddress(kernel, "GetTempPath2W") : nullptr;
    static_assert(sizeof(get_temp) == sizeof(address), "Win32 function pointer representation mismatch");
    std::memcpy(&get_temp, &address, sizeof(get_temp));
    if (!get_temp) get_temp = GetTempPathW;
    DWORD size = get_temp(0, nullptr);
    if (!size) { fail_win(GetLastError()); return nullptr; }
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        if (size > SIZE_MAX / sizeof(wchar_t) - 1) { errno = ENAMETOOLONG; return nullptr; }
        wchar_t* path = static_cast<wchar_t*>(std::malloc((static_cast<size_t>(size) + 1) * sizeof(wchar_t)));
        if (!path) { errno = ENOMEM; return nullptr; }
        const DWORD length = get_temp(size + 1, path);
        if (length && length <= size) return path;
        const DWORD error = GetLastError();
        std::free(path);
        if (!length) { fail_win(error); return nullptr; }
        size = length;
    }
    errno = EBUSY;
    return nullptr;
}

bool default_stream_only(const wchar_t* path) {
    WIN32_FIND_STREAM_DATA data;
    HANDLE search = FindFirstStreamW(path, FindStreamInfoStandard, &data, 0);
    if (search == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_HANDLE_EOF) return true;
        fail_win(error); return false;
    }
    bool okay = true;
    do {
        if (std::wcscmp(data.cStreamName, L"::$DATA") != 0) {
            okay = false; errno = ENOTSUP; break;
        }
    } while (FindNextStreamW(search, &data));
    const DWORD error = GetLastError();
    FindClose(search);
    if (okay && error != ERROR_HANDLE_EOF) { fail_win(error); return false; }
    return okay;
}

} // namespace

int seek(FILE* stream, std::int64_t offset, int whence) { return _fseeki64(stream, offset, whence); }
std::int64_t tell(FILE* stream) { return _ftelli64(stream); }
int descriptor(FILE* stream) { return stream ? _fileno(stream) : -1; }
int resize(FILE* stream, std::uint64_t size) {
    if (size > static_cast<std::uint64_t>(INT64_MAX)) { errno = EOVERFLOW; return -1; }
    const errno_t error = _chsize_s(descriptor(stream), static_cast<__int64>(size));
    if (error) { errno = error; return -1; }
    return 0;
}
int sync(int fd) {
    if (IDYDB_IO_FAULT(IDYDB_TEST_FILE_SYNC)) return -1;
    HANDLE handle = native_handle(fd);
    if (handle == INVALID_HANDLE_VALUE) return -1;
    return FlushFileBuffers(handle) ? 0 : fail_win(GetLastError());
}
int lock(int fd, int operation) {
    if (!(operation & unlock) && IDYDB_IO_FAULT(IDYDB_TEST_LOCK)) return -1;
    HANDLE handle = native_handle(fd);
    if (handle == INVALID_HANDLE_VALUE) return -1;
    OVERLAPPED overlapped = {};
    if (operation & unlock) {
        if (UnlockFileEx(handle, 0, MAXDWORD, MAXDWORD, &overlapped)) return 0;
        const DWORD error = GetLastError();
        return error == ERROR_NOT_LOCKED ? 0 : fail_win(error);
    }
    DWORD flags = 0;
    if (operation & exclusive) flags |= LOCKFILE_EXCLUSIVE_LOCK;
    if (operation & nonblocking) flags |= LOCKFILE_FAIL_IMMEDIATELY;
    return LockFileEx(handle, flags, 0, MAXDWORD, MAXDWORD, &overlapped) ? 0 : fail_win(GetLastError());
}

FILE* open_locked(const char* filename, int flags, int* status, int directory_fd) {
    if (!status) { errno = EINVAL; return nullptr; }
    *status = IDYDB_ERROR;
    if (directory_fd >= 0) { errno = ENOTSUP; return nullptr; }
    wchar_t* name = wide_name(filename);
    if (!name) return nullptr;
    const bool readonly = (flags & IDYDB_READONLY) == IDYDB_READONLY;
    const bool create = !readonly && (flags & IDYDB_CREATE) == IDYDB_CREATE;
    for (unsigned attempt = 0; attempt < 3; ++attempt) {
        HANDLE handle = CreateFileW(name, readonly ? GENERIC_READ : GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, create ? OPEN_ALWAYS : OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            *status = open_status(GetLastError()); std::free(name); return nullptr;
        }
        BY_HANDLE_FILE_INFORMATION information;
        if (!regular(handle, &information)) {
            CloseHandle(handle); std::free(name); *status = IDYDB_ERROR; return nullptr;
        }
        FILE* stream = own_stream(handle, readonly);
        if (!stream) { std::free(name); return nullptr; }
        if (lock(descriptor(stream), nonblocking | (readonly ? shared : exclusive)) != 0) {
            const int error = errno; std::fclose(stream); std::free(name); errno = error;
            *status = error == EBUSY || error == EWOULDBLOCK ? IDYDB_BUSY : IDYDB_ERROR;
            return nullptr;
        }
        HANDLE current = CreateFileW(name, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        const bool same = current != INVALID_HANDLE_VALUE && same_file(handle, current);
        if (current != INVALID_HANDLE_VALUE) CloseHandle(current);
        if (same) { std::free(name); *status = IDYDB_SUCCESS; return stream; }
        std::fclose(stream);
    }
    std::free(name); errno = EBUSY; *status = IDYDB_BUSY; return nullptr;
}

char* canonical_path(FILE* opened, const char* filename) {
    (void)filename;
    HANDLE handle = native_handle(descriptor(opened));
    if (handle == INVALID_HANDLE_VALUE) return nullptr;
    wchar_t* path = final_name(handle);
    if (!path) return nullptr;
    char* result = utf8_name(path);
    std::free(path);
    return result;
}

bool exists(const char* filename) {
    wchar_t* path = wide_name(filename);
    if (!path) return false;
    const DWORD attributes = GetFileAttributesW(path);
    const DWORD error = GetLastError();
    std::free(path);
    if (attributes == INVALID_FILE_ATTRIBUTES) { fail_win(error); return false; }
    return true;
}

FILE* secure_plain_stream(const char** kind) {
    if (kind) *kind = "unknown";
    private_security security;
    if (!security.initialize()) return nullptr;
    wchar_t* directory = temporary_directory();
    if (!directory) return nullptr;
    char* directory_utf8 = utf8_name(directory);
    std::free(directory);
    if (!directory_utf8) return nullptr;
    directory = wide_name(directory_utf8);
    std::free(directory_utf8);
    if (!directory) return nullptr;
    const size_t length = std::wcslen(directory);
    if (length > SIZE_MAX / sizeof(wchar_t) - 48) { std::free(directory); errno = ENAMETOOLONG; return nullptr; }
    wchar_t* path = static_cast<wchar_t*>(std::malloc((length + 48) * sizeof(wchar_t)));
    if (!path) { std::free(directory); errno = ENOMEM; return nullptr; }
    std::memcpy(path, directory, length * sizeof(wchar_t));
    std::free(directory);
    size_t suffix = length;
    if (suffix && path[suffix - 1] != L'\\' && path[suffix - 1] != L'/') path[suffix++] = L'\\';
    std::memcpy(path + suffix, L"idydb-", 6 * sizeof(wchar_t)); suffix += 6;
    FILE* stream = nullptr;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        if (!random_suffix(path + suffix)) break;
        std::memcpy(path + suffix + 32, L".tmp", 5 * sizeof(wchar_t));
        HANDLE handle = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | DELETE, 0, &security.attributes, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle != INVALID_HANDLE_VALUE) { stream = own_stream(handle, false); break; }
        const DWORD error = GetLastError();
        fail_win(error);
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) break;
    }
    std::free(path);
    if (stream && kind) *kind = "win32-private-temp-delete-on-close";
    // TEMPORARY is a cache hint, not a RAM-only guarantee. Plaintext can reach
    // disk; the DACL and delete-on-close protect access and lifecycle only.
    return stream;
}

FILE* stage(FILE* backing, const char* filename, int directory_fd, char** output) {
    if (!output) { errno = EINVAL; return nullptr; }
    *output = nullptr;
    if (directory_fd >= 0) { errno = ENOTSUP; return nullptr; }
    HANDLE original = native_handle(descriptor(backing));
    BY_HANDLE_FILE_INFORMATION information;
    if (original == INVALID_HANDLE_VALUE || !regular(original, &information)) return nullptr;
    // Copying only the default stream would silently strip these properties.
    // Refuse them until a deliberately tested preservation implementation exists.
    const DWORD unsupported = FILE_ATTRIBUTE_ENCRYPTED | FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE;
    if (information.dwFileAttributes & unsupported) {
        errno = ENOTSUP; return nullptr;
    }
    wchar_t* name = wide_name(filename);
    if (!name) return nullptr;
    if (!default_stream_only(name)) { std::free(name); return nullptr; }
    PSECURITY_DESCRIPTOR security = nullptr;
    const DWORD security_status = GetSecurityInfo(original, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &security);
    if (security_status != ERROR_SUCCESS) { std::free(name); fail_win(security_status); return nullptr; }
    SECURITY_ATTRIBUTES attributes = {sizeof(attributes), security, FALSE};
    const wchar_t* separator = std::wcsrchr(name, L'\\');
    if (!separator) { LocalFree(security); std::free(name); errno = EINVAL; return nullptr; }
    const size_t length = static_cast<size_t>(separator - name) + 1;
    // Keep the stage in the same directory without extending the database's
    // last component: NTFS accepts 255-character basenames, even when adding a
    // suffix to that valid basename would exceed the per-component limit.
    constexpr wchar_t prefix[] = L".idydb.tmp.";
    constexpr size_t prefix_length = sizeof(prefix) / sizeof(*prefix) - 1;
    if (length > SIZE_MAX / sizeof(wchar_t) - prefix_length - 33) {
        LocalFree(security); std::free(name); errno = ENAMETOOLONG; return nullptr;
    }
    wchar_t* path = static_cast<wchar_t*>(std::malloc((length + prefix_length + 33) * sizeof(wchar_t)));
    if (!path) { LocalFree(security); std::free(name); errno = ENOMEM; return nullptr; }
    std::memcpy(path, name, length * sizeof(wchar_t));
    std::memcpy(path + length, prefix, prefix_length * sizeof(wchar_t));
    std::free(name);
    FILE* stream = nullptr;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        if (!random_suffix(path + length + prefix_length)) break;
        const DWORD copied = information.dwFileAttributes &
            (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
        HANDLE handle = CreateFileW(path, GENERIC_READ | GENERIC_WRITE | DELETE | WRITE_DAC,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            &attributes, CREATE_NEW, (copied ? copied : FILE_ATTRIBUTE_NORMAL) | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError(); fail_win(error);
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) continue;
            break;
        }
        BY_HANDLE_FILE_INFORMATION created = {};
        if (!regular(handle, &created) || (created.dwFileAttributes & unsupported)) {
            const int error = (created.dwFileAttributes & unsupported) ? ENOTSUP : errno;
            CloseHandle(handle); DeleteFileW(path); errno = error; break;
        }
        // NTFS can ignore NOT_CONTENT_INDEXED in CreateFileW's creation flags.
        // Apply the selected attributes through the owned handle before copying
        // any database bytes, so a failure cannot publish altered metadata.
        FILE_BASIC_INFO basic = {};
        basic.FileAttributes = copied ? copied : FILE_ATTRIBUTE_NORMAL;
        if (!SetFileInformationByHandle(handle, FileBasicInfo, &basic, sizeof(basic))) {
            const DWORD error = GetLastError();
            CloseHandle(handle); DeleteFileW(path); fail_win(error); break;
        }
        if (!SetFileTime(handle, &information.ftCreationTime, nullptr, nullptr)) {
            const DWORD error = GetLastError();
            CloseHandle(handle); DeleteFileW(path); fail_win(error); break;
        }
        if (!preserve_dacl(handle, security)) {
            const int error = errno;
            CloseHandle(handle); DeleteFileW(path); errno = error; break;
        }
        stream = own_stream(handle, false);
        if (!stream || lock(descriptor(stream), exclusive | nonblocking) != 0) {
            const int error = errno;
            if (stream) std::fclose(stream);
            stream = nullptr; DeleteFileW(path); errno = error; break;
        }
        *output = utf8_name(path);
        if (!*output) {
            const int error = errno; std::fclose(stream); stream = nullptr; DeleteFileW(path); errno = error;
        }
        break;
    }
    LocalFree(security); std::free(path);
    return stream;
}

void remove_stage(const char* path, int directory_fd) {
    if (directory_fd >= 0 || !path) return;
    wchar_t* name = wide_name(path);
    if (!name) return;
    if (!DeleteFileW(name)) fail_win(GetLastError());
    std::free(name);
}

bool replace(FILE* current, FILE* replacement, const char* staged, const char* filename, int directory_fd) {
    (void)staged;
    if (directory_fd >= 0) { errno = ENOTSUP; return false; }
    wchar_t* destination = wide_name(filename);
    if (!destination) return false;
    HANDLE opened = native_handle(descriptor(current));
    HANDLE source = native_handle(descriptor(replacement));
    HANDLE path = CreateFileW(destination, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    bool okay = false;
    if (path != INVALID_HANDLE_VALUE) {
        okay = opened != INVALID_HANDLE_VALUE && source != INVALID_HANDLE_VALUE && same_file(opened, path);
        CloseHandle(path);
        if (!okay) errno = EBUSY;
    } else fail_win(GetLastError());
    if (okay) {
        const size_t name_bytes = std::wcslen(destination) * sizeof(wchar_t);
        const size_t allocation = sizeof(FILE_RENAME_INFO) + name_bytes;
        if (allocation > MAXDWORD) {
            errno = ENAMETOOLONG; okay = false;
        } else {
            FILE_RENAME_INFO* information = static_cast<FILE_RENAME_INFO*>(std::calloc(1, allocation));
            if (!information) { errno = ENOMEM; okay = false; }
            else {
                information->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS;
                information->RootDirectory = nullptr;
                information->FileNameLength = static_cast<DWORD>(name_bytes);
                std::memcpy(information->FileName, destination, name_bytes);
                // Renaming the owned stage handle prevents substitution of its
                // pathname. POSIX replacement semantics keep the old target
                // handle and its lock valid until common code adopts the new
                // locked file. Never close/unlock the target to make a rename
                // succeed, and never fall back to a delete-then-move sequence.
                if (!SetFileInformationByHandle(source, FileRenameInfoEx, information,
                                               static_cast<DWORD>(allocation))) {
                    fail_win(GetLastError()); okay = false;
                }
                std::free(information);
            }
        }
    }
    std::free(destination);
    return okay;
}

bool sync_directory(const char* filename, int directory_fd) {
    if (IDYDB_IO_FAULT(IDYDB_TEST_DIRECTORY_SYNC)) return false;
    if (directory_fd >= 0) { errno = ENOTSUP; return false; }
    wchar_t* name = wide_name(filename);
    if (!name) return false;
    HANDLE handle = CreateFileW(name, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    std::free(name);
    if (handle == INVALID_HANDLE_VALUE) { fail_win(GetLastError()); return false; }
    BY_HANDLE_FILE_INFORMATION information;
    bool okay = regular(handle, &information);
    if (okay && !FlushFileBuffers(handle)) { fail_win(GetLastError()); okay = false; }
    CloseHandle(handle);
    // Win32 has no equivalent promise here to POSIX fsync(parent-directory).
    // A successful Windows commit means the complete sibling was flushed,
    // handle-based FileRenameInfoEx(POSIX_SEMANTICS|REPLACE_IF_EXISTS)
    // succeeded, and the current
    // replacement file was flushed. Repeating flush/close retries this last
    // flush. This does not assert universal power-loss durability of NTFS name
    // metadata, remote filesystems, storage caches, or a POSIX directory sync.
    return okay;
}

void* map_readonly(FILE* stream, std::uint64_t size, void** cookie) {
    if (!cookie) { errno = EINVAL; return nullptr; }
    *cookie = nullptr;
    if (!size || size > SIZE_MAX) return nullptr;
    HANDLE handle = native_handle(descriptor(stream));
    if (handle == INVALID_HANDLE_VALUE) return nullptr;
    HANDLE mapping = CreateFileMappingW(handle, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (!mapping) { fail_win(GetLastError()); return nullptr; }
    void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, static_cast<SIZE_T>(size));
    if (!view) { const DWORD error = GetLastError(); CloseHandle(mapping); fail_win(error); return nullptr; }
    *cookie = mapping;
    return view;
}

void unmap(void* view, std::uint64_t size, void* cookie) {
    (void)size;
    if (view) UnmapViewOfFile(view);
    if (cookie) CloseHandle(static_cast<HANDLE>(cookie));
}

int duplicate_directory(int fd) { (void)fd; errno = ENOTSUP; return -1; }
void close_directory(int fd) { (void)fd; }

} // namespace idydb_platform
