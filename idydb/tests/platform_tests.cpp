#include "db.h"
#include "test_process.h"

#include <chrono>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#if defined(_WIN32)
#include <aclapi.h>
#endif

namespace {
namespace fs = std::filesystem;
fs::path extended_path(const fs::path& path) {
#if defined(_WIN32)
    const std::wstring name = fs::absolute(path).native();
    if (name.compare(0, 4, L"\\\\?\\") == 0) return fs::path(name);
    if (name.compare(0, 2, L"\\\\") == 0) return fs::path(L"\\\\?\\UNC\\" + name.substr(2));
    return fs::path(L"\\\\?\\" + name);
#else
    return path;
#endif
}
void check(bool value, const std::string& message) {
    if (!value) throw std::runtime_error(message);
}
struct Handle {
    idydb* value = nullptr;
    ~Handle() { idydb_discard(&value); }
    int open(const fs::path& path, bool encrypted, int flags) {
        idydb_open_options options = {};
        options.flags = flags;
        options.encrypted_at_rest = encrypted;
        options.passphrase = encrypted ? "public-platform-test-passphrase" : nullptr;
        options.pbkdf2_iter = 10000;
        return idydb_open_with_options(path.u8string().c_str(), &value, &options);
    }
    void close() { check(idydb_close(&value) == IDYDB_DONE, "close failed"); }
};
void require_value(Handle& handle, int expected) {
    check(idydb_extract(&handle.value, 1, 1) == IDYDB_DONE &&
          idydb_retrieve_int(&handle.value) == expected, "stored value changed");
}
void create(const fs::path& path, bool encrypted, int value) {
    Handle handle;
    check(handle.open(path, encrypted, IDYDB_CREATE) == IDYDB_SUCCESS, "create failed: " + path.u8string());
    check(idydb_insert_int(&handle.value, 1, 1, value) == IDYDB_DONE, "insert failed");
    check(idydb_flush(&handle.value) == IDYDB_DONE, "flush failed");
    handle.close();
}
void filenames(const fs::path& directory) {
    const fs::path unicode = directory / fs::u8path(u8"العربية é space 🐝.db");
    for (bool encrypted : {false, true}) {
        if (fs::exists(unicode)) fs::remove(unicode);
        create(unicode, encrypted, -73);
        Handle reopened;
        check(reopened.open(unicode, encrypted, IDYDB_READONLY) == IDYDB_SUCCESS,
              "Unicode database cannot reopen");
        require_value(reopened, -73);
        reopened.close();
    }
    fs::path nested = directory;
    while (nested.u8string().size() < 320) nested /= "nested-directory-for-long-paths";
    // The fixture helper also uses an extended name, so this tests the
    // library's long-path handling independently of the test CRT's defaults.
    fs::create_directories(extended_path(nested));
    const fs::path long_path = nested / fs::u8path(u8"بيانات 🐝.db");
    create(long_path, false, 81);
    Handle reopened;
    check(reopened.open(long_path, false, 0) == IDYDB_SUCCESS, "long database path cannot reopen");
    require_value(reopened, 81);
    check(idydb_insert_int(&reopened.value, 1, 1, 82) == IDYDB_DONE, "long path atomic update failed");
    reopened.close();
}
void filename_components(const fs::path& directory) {
    for (size_t length : {218u, 219u, 244u, 245u, 255u}) {
        for (bool encrypted : {false, true}) {
            const fs::path path = directory / (std::string(length - 3, encrypted ? 'e' : 'p') + ".db");
            create(path, encrypted, 73);
            Handle handle;
            check(handle.open(path, encrypted, 0) == IDYDB_SUCCESS,
                  "long filename component cannot reopen: " + std::to_string(length));
            require_value(handle, 73);
            check(idydb_insert_int(&handle.value, 1, 1, 74) == IDYDB_DONE,
                  "long filename component cannot update: " + std::to_string(length));
            check(idydb_flush(&handle.value) == IDYDB_DONE,
                  "long filename component cannot flush: " + std::to_string(length));
            handle.close();
            check(handle.open(path, encrypted, 0) == IDYDB_SUCCESS,
                  "updated long filename component cannot reopen: " + std::to_string(length));
            require_value(handle, 74);
            check(idydb_delete(&handle.value, 1, 1) == IDYDB_DONE,
                  "long filename component cannot delete: " + std::to_string(length));
            check(idydb_flush(&handle.value) == IDYDB_DONE,
                  "deleted long filename component cannot flush: " + std::to_string(length));
            handle.close();
            check(handle.open(path, encrypted, IDYDB_READONLY) == IDYDB_SUCCESS,
                  "deleted long filename component cannot reopen: " + std::to_string(length));
            check(idydb_extract(&handle.value, 1, 1) == IDYDB_NULL,
                  "deleted long filename component retained its value: " + std::to_string(length));
            handle.close();
        }
    }
}
void locks(const fs::path& directory, const fs::path& executable) {
    for (bool encrypted : {false, true}) {
        const fs::path path = directory / (encrypted ? "locks-encrypted.db" : "locks-plain.db");
        create(path, encrypted, 73);
        auto probe = [&](int flags, int expected) {
            const int status = idydb_test::child(executable, {"--open-child", idydb_test::hex(path.u8string()),
                encrypted ? "1" : "0", std::to_string(flags), std::to_string(expected)});
            check(status == 0, "child lock expectation failed: " + std::to_string(status));
        };
        Handle writer;
        check(writer.open(path, encrypted, 0) == IDYDB_SUCCESS, "writer open failed");
        probe(0, IDYDB_BUSY);
        probe(IDYDB_READONLY, IDYDB_BUSY);
        writer.close();
        probe(0, IDYDB_SUCCESS);
        Handle reader;
        check(reader.open(path, encrypted, IDYDB_READONLY) == IDYDB_SUCCESS, "reader open failed");
        probe(IDYDB_READONLY, IDYDB_SUCCESS);
        probe(0, IDYDB_BUSY);
        reader.close();
        probe(0, IDYDB_SUCCESS);
    }
}
void reader_snapshot(const fs::path& directory) {
    const fs::path path = directory / "snapshot.db";
    const fs::path backup = directory / "snapshot-old.db";
    create(path, false, 1);
    Handle old_reader;
    check(old_reader.open(path, false, IDYDB_READONLY) == IDYDB_SUCCESS, "old reader open failed");
    fs::rename(path, backup);
    create(path, false, 2);
    require_value(old_reader, 1);
    Handle new_reader;
    check(new_reader.open(path, false, IDYDB_READONLY) == IDYDB_SUCCESS, "new reader open failed");
    require_value(new_reader, 2);
    new_reader.close();
    old_reader.close();
}
void allocator_boundary() {
    void* allocation = idydb_alloc(257);
    check(allocation != nullptr, "library allocator failed");
    std::memset(allocation, 0x5a, 257);
    idydb_free(allocation);
    idydb_free(nullptr);
}
#if defined(_WIN32)
void remove_windows_fixture(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        check(error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND,
              "cannot inspect fixture during cleanup: " + std::to_string(error));
        return;
    }
    const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (directory && !(attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        WIN32_FIND_DATAW entry;
        const std::wstring pattern = path + L"\\*";
        struct Search {
            HANDLE value;
            ~Search() { if (value != INVALID_HANDLE_VALUE) FindClose(value); }
        } search{FindFirstFileW(pattern.c_str(), &entry)};
        if (search.value == INVALID_HANDLE_VALUE) {
            check(GetLastError() == ERROR_FILE_NOT_FOUND, "cannot enumerate fixture directory");
        } else {
            for (;;) {
                if (std::wcscmp(entry.cFileName, L".") != 0 && std::wcscmp(entry.cFileName, L"..") != 0)
                    remove_windows_fixture(path + L"\\" + entry.cFileName);
                if (!FindNextFileW(search.value, &entry)) {
                    check(GetLastError() == ERROR_NO_MORE_FILES, "cannot continue fixture enumeration");
                    break;
                }
            }
        }
    }
    // A reparse point is removed as an entry; its target is never traversed.
    const BOOL removed = directory ? RemoveDirectoryW(path.c_str()) : DeleteFileW(path.c_str());
    check(removed != FALSE, "cannot remove fixture: " + std::to_string(GetLastError()));
}

std::string dacl(const fs::path& path) {
    PACL acl = nullptr;
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    check(GetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
              DACL_SECURITY_INFORMATION, nullptr, nullptr, &acl, nullptr, &descriptor) == ERROR_SUCCESS,
          "cannot inspect database DACL");
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    const bool inspected = GetSecurityDescriptorControl(descriptor, &control, &revision) != FALSE;
    std::string result;
    if (acl) result.assign(reinterpret_cast<const char*>(acl), acl->AclSize);
    result.append(reinterpret_cast<const char*>(&control), sizeof(control));
    LocalFree(descriptor);
    check(inspected, "cannot inspect DACL protection");
    return result;
}
void restrict_to_owner(const fs::path& path) {
    HANDLE token = nullptr;
    check(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != FALSE,
          "cannot inspect test user token");
    DWORD required = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    std::vector<unsigned char> storage(required);
    const BOOL found = GetTokenInformation(token, TokenUser, storage.data(), required, &required);
    CloseHandle(token);
    check(found != FALSE, "cannot inspect test user SID");
    EXPLICIT_ACCESSW access = {};
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode = SET_ACCESS;
    access.grfInheritance = NO_INHERITANCE;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName = static_cast<LPWSTR>(reinterpret_cast<void*>(
        reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid));
    PACL acl = nullptr;
    check(SetEntriesInAclW(1, &access, nullptr, &acl) == ERROR_SUCCESS, "cannot create private DACL");
    const DWORD status = SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
        DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, nullptr, nullptr, acl, nullptr);
    LocalFree(acl);
    check(status == ERROR_SUCCESS, "cannot set private database DACL");
}
void windows_security(const fs::path& directory) {
    for (bool encrypted : {false, true}) {
        const fs::path path = directory / (encrypted ? "private-encrypted.db" : "private-plain.db");
        create(path, encrypted, 1);
        restrict_to_owner(path);
        const std::string original = dacl(path);
        constexpr DWORD preserved_attributes = FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
            FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
        check(SetFileAttributesW(path.c_str(), preserved_attributes) != FALSE,
              "cannot set database basic attributes");
        const DWORD before_attributes = GetFileAttributesW(path.c_str());
        check(before_attributes != INVALID_FILE_ATTRIBUTES &&
              (before_attributes & preserved_attributes) == preserved_attributes,
              "database basic attribute fixture was not applied");
        Handle handle;
        check(handle.open(path, encrypted, 0) == IDYDB_SUCCESS, "private database open failed");
        check(idydb_insert_int(&handle.value, 1, 1, 2) == IDYDB_DONE, "private database update failed");
        check(idydb_flush(&handle.value) == IDYDB_DONE, "private database flush failed");
        handle.close();
        check(dacl(path) == original, "atomic replacement changed protected DACL");
        const DWORD after_attributes = GetFileAttributesW(path.c_str());
        check(after_attributes != INVALID_FILE_ATTRIBUTES &&
              (after_attributes & preserved_attributes) == preserved_attributes,
              "atomic replacement changed database basic attributes");
        check(handle.open(path, encrypted, IDYDB_READONLY) == IDYDB_SUCCESS, "private database reopen failed");
        require_value(handle, 2);
        handle.close();
    }
    const fs::path target = directory / "reparse-target.db";
    const fs::path link = directory / "reparse-link.db";
    create(target, false, 73);
    const std::wstring target_name = extended_path(target).native();
    const std::wstring link_name = extended_path(link).native();
    // Some MinGW standard-library builds stub create_symlink with ENOSYS.
    // Exercise Windows itself, using existing Developer Mode or token rights;
    // do not change settings or enable any privileges for the test.
    constexpr DWORD allow_unprivileged_create = 0x2;
    BOOL created = CreateSymbolicLinkW(link_name.c_str(), target_name.c_str(), allow_unprivileged_create);
    DWORD error = created ? ERROR_SUCCESS : GetLastError();
    if (!created && error == ERROR_INVALID_PARAMETER) {
        created = CreateSymbolicLinkW(link_name.c_str(), target_name.c_str(), 0);
        error = created ? ERROR_SUCCESS : GetLastError();
    }
    if (!created) {
        check(error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_ACCESS_DENIED ||
              error == ERROR_NOT_SUPPORTED || error == ERROR_INVALID_FUNCTION,
              "unexpected symbolic-link creation failure: " + std::to_string(error));
        std::cout << "Symbolic-link creation unavailable: Windows error " << error << '\n';
    } else {
        Handle handle;
        check(handle.open(link, false, 0) != IDYDB_SUCCESS, "writer followed a reparse point");
        idydb_discard(&handle.value);
        check(handle.open(link, false, IDYDB_READONLY) != IDYDB_SUCCESS,
              "reader followed a reparse point");
        check(DeleteFileW(link_name.c_str()) != FALSE, "cannot remove symbolic-link fixture");
    }
    const fs::path stream_path(target.native() + L":retained-metadata");
    { std::ofstream stream(stream_path, std::ios::binary); stream << "retain this alternate stream";
      check(stream.good(), "cannot create NTFS alternate-stream fixture"); }
    {
        Handle handle;
        check(handle.open(target, false, 0) == IDYDB_SUCCESS, "alternate-stream database open failed");
        check(idydb_insert_int(&handle.value, 1, 1, 99) != IDYDB_DONE,
              "replacement silently discarded an alternate data stream");
        require_value(handle, 73);
        handle.close();
        check(handle.open(stream_path, false, 0) != IDYDB_SUCCESS,
              "database API accepted a named alternate data stream");
    }
    { std::ifstream stream(stream_path, std::ios::binary); std::string content;
      std::getline(stream, content);
      check(content == "retain this alternate stream", "failed update changed alternate metadata"); }
    {
        Handle handle;
        const std::string malformed = directory.u8string() + "/invalid-" + char(0xff) + ".db";
        check(idydb_open(malformed.c_str(), &handle.value, IDYDB_CREATE) != IDYDB_SUCCESS,
              "database API accepted malformed UTF-8 pathname");
    }
}
#endif
}
int main(int argc, char** argv) {
    if (argc == 6 && std::string(argv[1]) == "--open-child") {
        try {
            Handle handle;
            const int actual = handle.open(fs::u8path(idydb_test::unhex(argv[2])),
                std::string(argv[3]) == "1", std::stoi(argv[4]));
            if (actual != std::stoi(argv[5])) {
                std::cerr << "child expected " << argv[5] << " got " << actual << '\n';
                return 10;
            }
            if (actual == IDYDB_SUCCESS) { require_value(handle, 73); handle.close(); }
            return 0;
        } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 11; }
    }
    const fs::path directory = fs::temp_directory_path() / ("idydb-platform-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    try {
        fs::create_directory(directory);
        filenames(directory);
        filename_components(directory);
        locks(directory, idydb_test::executable(argv[0]));
        reader_snapshot(directory);
        allocator_boundary();
#if defined(_WIN32)
        windows_security(directory);
#endif
#if defined(_WIN32)
        // MinGW remove_all can spin in filesystem traversal on long paths,
        // including when the root already carries an extended-length prefix.
        remove_windows_fixture(extended_path(directory).native());
#else
        fs::remove_all(directory);
#endif
        std::cout << "Platform filename, lock, child-process, snapshot and allocation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << " (fixtures: " << directory.u8string() << ")\n";
        return 1;
    }
}
