#include "db.h"
#include "failure_hooks.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct Handle {
    idydb* value = nullptr;
    ~Handle() { idydb_discard(&value); }
    void open_at(int directory, const char* name, int flags = 0) {
        const int status = idydb_open_at(directory, name, &value, flags);
        check(status == IDYDB_SUCCESS,
              "directory open failed: " + std::string(idydb_errmsg(&value)));
    }
    void close() {
        check(idydb_close(&value) == IDYDB_DONE, "directory handle close failed");
        check(value == nullptr, "successful close retained a directory handle");
    }
};

#if !defined(_WIN32)
struct Descriptor {
    int value = -1;
    explicit Descriptor(int descriptor) : value(descriptor) {
        check(value >= 0, "fixture descriptor open failed");
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    ~Descriptor() { if (value >= 0) ::close(value); }
    void close() {
        check(::close(value) == 0, "fixture descriptor close failed");
        value = -1;
    }
};

struct Files {
    fs::path directory;
    Files() {
        std::string pattern = (fs::temp_directory_path() / "idydb-directory-XXXXXX").string();
        std::vector<char> writable(pattern.begin(), pattern.end());
        writable.push_back('\0');
        char* created = mkdtemp(writable.data());
        check(created != nullptr, "cannot create unique directory fixtures");
        directory = created;
    }
    fs::path make_directory(const char* name) {
        const fs::path path = directory / name;
        check(fs::create_directory(path), "cannot create fixture directory");
        return path;
    }
    void require_no_staging(const fs::path& path) const {
        for (const auto& entry : fs::directory_iterator(path)) {
            check(entry.path().filename().string().find(".tmp.") == std::string::npos,
                  "staging file leaked: " + entry.path().string());
        }
    }
};

std::string bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    check(stream.is_open(), "cannot read fixture " + path.string());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

constexpr float old_vector[] = {1.0f, 0.0f};
constexpr float new_vector[] = {0.0f, 1.0f};
constexpr const char* old_text = "original pinned text";
constexpr const char* new_text = "new pinned text and embedding";

void require_pair(Handle& handle, const char* text, const float* vector) {
    check(idydb_extract(&handle.value, 1, 1) == IDYDB_DONE, "pinned text extraction failed");
    check(idydb_retrieved_type(&handle.value) == IDYDB_CHAR, "pinned text type changed");
    check(std::strcmp(idydb_retrieve_char(&handle.value), text) == 0, "pinned text changed");
    check(idydb_extract(&handle.value, 2, 1) == IDYDB_DONE, "pinned vector extraction failed");
    check(idydb_retrieved_type(&handle.value) == IDYDB_VECTOR, "pinned vector type changed");
    unsigned short dimensions = 0;
    const float* actual = idydb_retrieve_vector(&handle.value, &dimensions);
    check(actual && dimensions == 2 && actual[0] == vector[0] && actual[1] == vector[1],
          "pinned vector changed");
}

void upsert(Handle& handle, const char* text, const float* vector) {
    check(idydb_rag_upsert_text(&handle.value, 1, 2, 1, text, vector, 2) == IDYDB_DONE,
          "pinned RAG upsert failed");
}

void create_fixture(const fs::path& path, const char* text = old_text) {
    Handle handle;
    check(idydb_open(path.u8string().c_str(), &handle.value, IDYDB_CREATE) == IDYDB_SUCCESS,
          "fixture database creation failed");
    upsert(handle, text, old_vector);
    handle.close();
}

struct Injection {
    int stage;
    unsigned occurrence;
    unsigned seen = 0;
    bool fired = false;
    explicit Injection(int point, unsigned nth = 1) : stage(point), occurrence(nth) {
        idydb_test_set_hook(callback, this);
    }
    ~Injection() { reset(); }
    static int callback(int point, void* context) {
        auto& self = *static_cast<Injection*>(context);
        if (point != self.stage || ++self.seen != self.occurrence) return 0;
        self.fired = true;
        return EIO;
    }
    void reset() { idydb_test_set_hook(nullptr, nullptr); }
};

void reject(int directory, const char* name, int flags, const std::string& message) {
    Handle handle;
    check(idydb_open_at(directory, name, &handle.value, flags) != IDYDB_SUCCESS, message);
}

int require_owned_cloexec_directory(int caller_fd) {
#if defined(__linux__)
    struct stat original;
    check(fstat(caller_fd, &original) == 0, "caller directory descriptor was closed");
    unsigned matches = 0;
    int owned = -1;
    for (const auto& entry : fs::directory_iterator("/proc/self/fd")) {
        const int fd = std::stoi(entry.path().filename().string());
        if (fd == caller_fd) continue;
        struct stat candidate;
        if (fstat(fd, &candidate) != 0 || !S_ISDIR(candidate.st_mode) ||
            candidate.st_dev != original.st_dev || candidate.st_ino != original.st_ino) continue;
        ++matches;
        owned = fd;
        check((fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0,
              "owned directory descriptor is inherited across exec");
    }
    check(matches >= 1, "directory open did not retain its own directory descriptor");
    return owned;
#else
    check(fcntl(caller_fd, F_GETFD) >= 0, "caller directory descriptor was closed");
    return -1;
#endif
}

void test_create_reopen_and_lock(Files& files) {
    const fs::path path = files.make_directory("normal");
    Descriptor directory(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    Handle created;
    {
        Injection injection(IDYDB_TEST_LOCK, 2);
        created.open_at(directory.value, "test.db", IDYDB_CREATE);
        check(injection.seen == 1 && !injection.fired,
              "directory creation must acquire its file lock exactly once");
    }
    const int owned = require_owned_cloexec_directory(directory.value);
    upsert(created, old_text, old_vector);
    created.close();
    if (owned >= 0) check(fcntl(owned, F_GETFD) == -1 && errno == EBADF,
                          "successful close leaked its owned directory descriptor");

    for (int flags : {IDYDB_CREATE, 0, IDYDB_READONLY}) {
        Handle reopened;
        {
            Injection injection(IDYDB_TEST_LOCK, 2);
            reopened.open_at(directory.value, "test.db", flags);
            check(injection.seen == 1 && !injection.fired,
                  "directory reopen must acquire its file lock exactly once");
        }
        require_pair(reopened, old_text, old_vector);
        if (flags == IDYDB_READONLY) {
            check(idydb_insert_const_char(&reopened.value, 1, 1, new_text) == IDYDB_READONLY,
                  "directory read-only handle accepted a write");
            check(idydb_flush(&reopened.value) == IDYDB_DONE, "directory read-only flush failed");
        }
        reopened.close();
    }
    Handle writer;
    writer.open_at(directory.value, "test.db");
    Handle competing;
    check(idydb_open_at(directory.value, "test.db", &competing.value, 0) == IDYDB_BUSY,
          "directory open ignored an existing writer lock");
    writer.close();
    files.require_no_staging(path);
}

void test_rejected_entries_and_arguments(Files& files) {
    const fs::path path = files.make_directory("rejections");
    Descriptor directory(open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    for (const char* name : {"", ".", "..", "nested/name.db", "../outside.db", "/absolute.db"}) {
        reject(directory.value, name, IDYDB_CREATE, "directory open accepted unsafe basename");
    }
    reject(directory.value, nullptr, IDYDB_CREATE, "directory open accepted null basename");
    reject(-1, "missing.db", IDYDB_CREATE, "directory open accepted invalid descriptor");
    reject(directory.value, "unsupported.db", 0x10000, "directory open accepted unsupported flags");
#if defined(__linux__) && defined(O_PATH)
    Descriptor path_only(open(path.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
    const auto descriptor_count = [] {
        return static_cast<std::size_t>(std::distance(fs::directory_iterator("/proc/self/fd"),
                                                    fs::directory_iterator()));
    };
    const std::size_t before = descriptor_count();
    Handle rejected_path_only;
    check(idydb_open_at(path_only.value, "path-only.db", &rejected_path_only.value, IDYDB_CREATE) == IDYDB_ERROR,
          "directory open accepted an unsyncable O_PATH descriptor");
    check(rejected_path_only.value == nullptr, "O_PATH rejection allocated a handle");
    check(!fs::exists(path / "path-only.db"), "O_PATH rejection created a database");
    check(fcntl(path_only.value, F_GETFD) >= 0, "O_PATH rejection closed the caller's descriptor");
    check(descriptor_count() == before, "O_PATH rejection leaked a descriptor");
#endif
    check(idydb_open_at(directory.value, "missing.db", nullptr, IDYDB_CREATE) == IDYDB_ERROR,
          "directory open accepted null handler");
    for (int flags : {0, IDYDB_READONLY, IDYDB_CREATE | IDYDB_READONLY}) {
        Handle missing;
        check(idydb_open_at(directory.value, "missing.db", &missing.value, flags) == IDYDB_NOT_FOUND,
              "directory open returned wrong missing-file status");
        check(!fs::exists(path / "missing.db"), "non-creating open created a missing file");
    }
    check(fs::create_directory(path / "directory.db"), "nonregular fixture creation failed");
    reject(directory.value, "directory.db", IDYDB_CREATE, "directory open accepted a directory");
    check(mkfifo((path / "fifo.db").c_str(), 0600) == 0, "FIFO fixture creation failed");
    reject(directory.value, "fifo.db", 0, "directory open accepted a FIFO");

    const fs::path target = files.directory / "symlink-target.db";
    create_fixture(target);
    const std::string saved = bytes(target);
    fs::create_symlink(target, path / "symlink.db");
    for (int flags : {0, IDYDB_CREATE, IDYDB_READONLY}) {
        reject(directory.value, "symlink.db", flags, "directory open followed a symlink");
        check(bytes(target) == saved, "rejected symlink changed its target");
    }
    Descriptor regular(open(target.c_str(), O_RDONLY | O_CLOEXEC));
    reject(regular.value, "test.db", IDYDB_CREATE, "directory open accepted a regular-file descriptor");

    Handle existing;
    existing.open_at(directory.value, "existing.db", IDYDB_CREATE);
    idydb* original = existing.value;
    check(idydb_open_at(directory.value, "another.db", &existing.value, IDYDB_CREATE) == IDYDB_ERROR,
          "directory open accepted an already-populated handler");
    check(existing.value == original, "failed reopen replaced the existing handle");
    check(!fs::exists(path / "another.db"), "failed reopen created another database");
    existing.close();
    files.require_no_staging(path);
}

void test_pinned_parent_and_atomic_persistence(Files& files) {
    const fs::path source = files.make_directory("source");
    const fs::path replacement = files.make_directory("replacement");
    const fs::path pinned = files.directory / "pinned";
    create_fixture(source / "test.db");
    create_fixture(replacement / "test.db", "replacement directory sentinel");
    const std::string untouched = bytes(replacement / "test.db");
    Descriptor original(open(source.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    Handle handle;
    handle.open_at(original.value, "test.db");
    require_owned_cloexec_directory(original.value);
    original.close();
    fs::rename(source, pinned);
    fs::create_directory_symlink(replacement, source);
    const std::string saved = bytes(pinned / "test.db");

    for (int stage : {IDYDB_TEST_CELL_WRITE, IDYDB_TEST_REPLACE}) {
        Injection injection(stage, stage == IDYDB_TEST_CELL_WRITE ? 2u : 1u);
        check(idydb_rag_upsert_text(&handle.value, 1, 2, 1, new_text, new_vector, 2) != IDYDB_DONE,
              "pinned atomic upsert ignored failure");
        check(injection.fired, "pinned atomic upsert did not reach failure boundary");
        injection.reset();
        check(bytes(pinned / "test.db") == saved, "failed pinned upsert changed backing bytes");
        require_pair(handle, old_text, old_vector);
        check(bytes(replacement / "test.db") == untouched, "failed upsert followed replaced parent");
        files.require_no_staging(pinned);
        files.require_no_staging(replacement);
    }

    {
        Injection injection(IDYDB_TEST_DIRECTORY_SYNC);
        check(idydb_rag_upsert_text(&handle.value, 1, 2, 1, new_text, new_vector, 2) == IDYDB_ERROR,
              "pinned upsert hid directory-sync failure");
        check(injection.fired, "pinned upsert did not sync its directory");
    }
    require_pair(handle, new_text, new_vector);
    check(bytes(pinned / "test.db") != saved, "pinned replacement was not committed");
    check(bytes(replacement / "test.db") == untouched, "pinned replacement followed replaced parent");
    {
        Injection injection(IDYDB_TEST_DIRECTORY_SYNC);
        check(idydb_flush(&handle.value) == IDYDB_ERROR, "pinned flush skipped directory-sync retry");
        check(injection.fired, "pinned flush did not retry directory sync");
    }
    check(idydb_flush(&handle.value) == IDYDB_DONE, "pinned flush retry failed");
    check(idydb_insert_const_char(&handle.value, 3, 1, "standalone pinned text") == IDYDB_DONE,
          "pinned standalone text insertion failed");
    handle.close();

    Descriptor reopened_directory(open(pinned.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    Handle reopened;
    reopened.open_at(reopened_directory.value, "test.db", IDYDB_READONLY);
    require_pair(reopened, new_text, new_vector);
    check(idydb_extract(&reopened.value, 3, 1) == IDYDB_DONE &&
          std::strcmp(idydb_retrieve_char(&reopened.value), "standalone pinned text") == 0,
          "pinned standalone text was not persisted");
    reopened.close();
    check(bytes(replacement / "test.db") == untouched, "pinned close followed replaced parent");
    files.require_no_staging(pinned);
    files.require_no_staging(replacement);
}

#if defined(__linux__)
void test_unlinked_readonly_snapshot(Files& files) {
    const fs::path directory_path = files.make_directory("snapshot");
    const fs::path path = directory_path / "test.db";
    create_fixture(path);
    Descriptor directory(open(directory_path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    Descriptor snapshot(open(path.c_str(), O_RDONLY | O_CLOEXEC));
    Handle writer;
    writer.open_at(directory.value, "test.db");
    upsert(writer, new_text, new_vector);
    writer.close();
    struct stat old_inode;
    check(fstat(snapshot.value, &old_inode) == 0 && old_inode.st_nlink == 0,
          "atomic replacement did not unlink snapshot inode");
    const std::string descriptor_path = "/proc/self/fd/" + std::to_string(snapshot.value);
    Handle readonly;
    check(idydb_open(descriptor_path.c_str(), &readonly.value, IDYDB_READONLY) == IDYDB_SUCCESS,
          "normal read-only open rejected an unlinked descriptor snapshot");
    snapshot.close();
    require_pair(readonly, old_text, old_vector);
    check(idydb_insert_const_char(&readonly.value, 1, 1, new_text) == IDYDB_READONLY,
          "descriptor snapshot accepted a write");
    readonly.close();
    Handle current;
    current.open_at(directory.value, "test.db", IDYDB_READONLY);
    require_pair(current, new_text, new_vector);
    current.close();
    files.require_no_staging(directory_path);
}
#endif
#endif
} // namespace

int main() {
#if defined(_WIN32)
    Handle unsupported;
    if (idydb_open_at(-1, "unsupported.db", &unsupported.value, IDYDB_CREATE) != IDYDB_ERROR) return 1;
    std::cout << "POSIX directory tests skipped; Windows unsupported API status passed\n";
    return 0;
#else
    Files files;
    try {
        test_create_reopen_and_lock(files);
        test_rejected_entries_and_arguments(files);
        test_pinned_parent_and_atomic_persistence(files);
#if defined(__linux__)
        test_unlinked_readonly_snapshot(files);
#endif
        fs::remove_all(files.directory);
        std::cout << "Directory-anchored open, atomic persistence, and snapshot tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        idydb_test_set_hook(nullptr, nullptr);
        std::cerr << error.what() << " (fixtures: " << files.directory << ")\n";
        return 1;
    }
#endif
}
