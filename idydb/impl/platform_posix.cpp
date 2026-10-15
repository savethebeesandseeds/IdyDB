#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "platform.h"
#include "db.h"
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#include <openssl/rand.h>

namespace idydb_platform {
int seek(FILE* stream, std::int64_t offset, int whence) { return fseeko(stream, offset, whence); }
std::int64_t tell(FILE* stream) { return ftello(stream); }
int descriptor(FILE* stream) { return fileno(stream); }
int resize(FILE* stream, std::uint64_t size) {
    if (size > INT64_MAX) { errno = EFBIG; return -1; }
    return ftruncate(fileno(stream), static_cast<off_t>(size));
}
int sync(int fd) { return IDYDB_IO_FAULT(IDYDB_TEST_FILE_SYNC) ? -1 : fsync(fd); }
int lock(int fd, int operation) {
    if (!(operation & unlock) && IDYDB_IO_FAULT(IDYDB_TEST_LOCK)) return -1;
    int native = (operation & unlock) ? LOCK_UN : ((operation & exclusive) ? LOCK_EX : LOCK_SH);
    if (operation & nonblocking) native |= LOCK_NB;
    return flock(fd, native);
}
FILE* open_locked(const char* filename, int flags, int* status, int directory_fd) {
    const bool ro = (flags & IDYDB_READONLY) == IDYDB_READONLY;
    const bool create = !ro && (flags & IDYDB_CREATE) == IDYDB_CREATE;
    *status = IDYDB_ERROR;
    for (int attempt = 0; attempt < 3; ++attempt) {
        int mode = (ro ? O_RDONLY : O_RDWR) | (create ? O_CREAT : 0) | O_CLOEXEC;
        int fd = directory_fd >= 0 ? openat(directory_fd, filename, mode | O_NOFOLLOW | O_NONBLOCK, 0600) :
            open(filename, mode, 0600);
        if (fd < 0) { *status = errno == ENOENT ? IDYDB_NOT_FOUND : IDYDB_PERM; return nullptr; }
        struct stat metadata;
        if (directory_fd >= 0 && (fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode))) {
            close(fd); *status = IDYDB_CORRUPT; return nullptr;
        }
        FILE* stream = fdopen(fd, ro ? "rb" : "r+b");
        if (!stream) { close(fd); return nullptr; }
        if (lock(fd, nonblocking | (ro ? shared : exclusive)) != 0) {
            fclose(stream); *status = IDYDB_BUSY; return nullptr;
        }
        struct stat opened, current;
        int rc = directory_fd >= 0 ? fstatat(directory_fd, filename, &current, AT_SYMLINK_NOFOLLOW) :
            stat(filename, &current);
        if (fstat(fd, &opened) == 0 && rc == 0 && opened.st_dev == current.st_dev && opened.st_ino == current.st_ino) {
            *status = IDYDB_SUCCESS; return stream;
        }
        fclose(stream);
    }
    *status = IDYDB_BUSY;
    return nullptr;
}
char* canonical_path(FILE*, const char* filename) { return realpath(filename, nullptr); }
bool exists(const char* filename) { return access(filename, F_OK) == 0; }
FILE* secure_plain_stream(const char** kind) {
    if (kind) *kind = "unknown";
#if defined(__linux__) && defined(SYS_memfd_create)
    int fd = static_cast<int>(syscall(SYS_memfd_create, "idydb_plain", 0x0001U)); // MFD_CLOEXEC
    if (fd >= 0) {
        if (fchmod(fd, 0600) != 0) { close(fd); return nullptr; }
        FILE* stream = fdopen(fd, "w+b");
        if (!stream) close(fd);
        else if (kind) *kind = "memfd";
        return stream;
    }
#endif
    unsigned char nonce[16];
    if (RAND_bytes(nonce, sizeof nonce) != 1) return nullptr;
    char name[64];
    snprintf(name, sizeof name, "/idydb_%02x%02x%02x%02x%02x%02x%02x%02x",
        nonce[0], nonce[1], nonce[2], nonce[3], nonce[4], nonce[5], nonce[6], nonce[7]);
    int shared_fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    if (shared_fd < 0) return nullptr;
    if (shm_unlink(name) != 0) { close(shared_fd); return nullptr; }
    FILE* stream = fdopen(shared_fd, "w+b");
    if (!stream) close(shared_fd);
    else if (kind) *kind = "shm";
    return stream;
}
FILE* stage(FILE* backing, const char* filename, int directory_fd, char** out_path) {
    *out_path = nullptr;
    if (!filename) return nullptr;
    const size_t length = strlen(filename);
    char* path = static_cast<char*>(malloc(length + 40));
    if (!path) return nullptr;
    int fd = -1;
    if (directory_fd >= 0) {
        for (int attempt = 0; attempt < 8; ++attempt) {
            unsigned char nonce[8];
            if (RAND_bytes(nonce, sizeof nonce) != 1) break;
            snprintf(path, length + 40, ".idydb.tmp.%02x%02x%02x%02x%02x%02x%02x%02x",
                nonce[0], nonce[1], nonce[2], nonce[3], nonce[4], nonce[5], nonce[6], nonce[7]);
            fd = openat(directory_fd, path, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
            if (fd >= 0 || errno != EEXIST) break;
        }
    } else {
        // Keep the sibling component short even when the database basename is
        // already at the filesystem's NAME_MAX limit.
        const char* slash = strrchr(filename, '/');
        const size_t parent_length = slash ? static_cast<size_t>(slash - filename) + 1 : 0;
        memcpy(path, filename, parent_length);
        memcpy(path + parent_length, ".idydb.tmp.XXXXXX", sizeof ".idydb.tmp.XXXXXX");
        fd = mkstemp(path);
    }
    if (fd < 0) { free(path); return nullptr; }
    FILE* stream = nullptr;
    struct stat original;
    if (fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 && backing && fstat(fileno(backing), &original) == 0 &&
        fchown(fd, original.st_uid, original.st_gid) == 0 && fchmod(fd, original.st_mode & 0777) == 0)
        stream = fdopen(fd, "w+b");
    if (!stream) close(fd);
    else if (lock(fileno(stream), exclusive | nonblocking) == 0) { *out_path = path; return stream; }
    else fclose(stream);
    remove_stage(path, directory_fd);
    free(path);
    return nullptr;
}
void remove_stage(const char* path, int directory_fd) {
    if (directory_fd >= 0) unlinkat(directory_fd, path, 0);
    else unlink(path);
}
bool replace(FILE* backing, FILE*, const char* stage_path, const char* filename, int directory_fd) {
    if (directory_fd >= 0) {
        struct stat opened, current;
        if (fstat(fileno(backing), &opened) != 0 ||
            fstatat(directory_fd, filename, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
            !S_ISREG(current.st_mode) || opened.st_dev != current.st_dev || opened.st_ino != current.st_ino) {
            errno = ESTALE; return false;
        }
        return renameat(directory_fd, stage_path, directory_fd, filename) == 0;
    }
    return rename(stage_path, filename) == 0;
}
bool sync_directory(const char* filename, int directory_fd) {
    if (IDYDB_IO_FAULT(IDYDB_TEST_DIRECTORY_SYNC)) return false;
    if (directory_fd >= 0) return sync(directory_fd) == 0;
    char* parent = strdup(filename);
    if (!parent) return false;
    char* slash = strrchr(parent, '/');
    if (slash) { if (slash == parent) slash[1] = '\0'; else *slash = '\0'; }
    int fd = open(slash ? parent : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    free(parent);
    if (fd < 0) return false;
    bool ok = sync(fd) == 0;
    close(fd);
    return ok;
}
void* map_readonly(FILE* stream, std::uint64_t size, void** cookie) {
    *cookie = nullptr;
    void* view = mmap(nullptr, static_cast<size_t>(size), PROT_READ, MAP_PRIVATE, fileno(stream), 0);
    return view == MAP_FAILED ? nullptr : view;
}
void unmap(void* view, std::uint64_t size, void*) { if (view) munmap(view, static_cast<size_t>(size)); }
int duplicate_directory(int fd) {
    struct stat metadata;
    if (fd < 0 || fstat(fd, &metadata) != 0 || !S_ISDIR(metadata.st_mode)) { errno = EINVAL; return -1; }
    const int flags = fcntl(fd, F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY) { errno = EINVAL; return -1; }
#ifdef O_PATH
    if (flags & O_PATH) { errno = EINVAL; return -1; }
#endif
    return fcntl(fd, F_DUPFD_CLOEXEC, 0);
}
void close_directory(int fd) { if (fd >= 0) close(fd); }
}
