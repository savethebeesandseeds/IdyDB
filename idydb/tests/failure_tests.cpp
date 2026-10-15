#include "db.h"
#include "failure_hooks.h"
#include "test_process.h"

#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
namespace fs = std::filesystem;
fs::path test_executable;

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

std::string bytes(const fs::path& path, idydb** live = nullptr) {
#if defined(_WIN32)
    // Windows byte-range locks deliberately reject reads through a second
    // handle. Inspect backing bytes through the writer's existing handle.
    if (live && *live) {
        unsigned char* data = nullptr;
        size_t size = 0;
        check(idydb_test_backing_snapshot(live, &data, &size) == IDYDB_DONE,
              "cannot inspect locked backing image");
        std::string result(reinterpret_cast<char*>(data), size);
        idydb_free(data);
        return result;
    }
#else
    (void)live;
#endif
    std::ifstream stream(path, std::ios::binary);
    check(stream.is_open(), "cannot read fixture " + path.string());
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

struct Files {
    fs::path directory = fs::temp_directory_path() / ("idydb-failure-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    unsigned serial = 0;
    Files() { fs::create_directory(directory); }
    fs::path next() { return directory / (std::to_string(serial++) + ".db"); }
    void require_no_staging() const {
        for (const auto& entry : fs::directory_iterator(directory)) {
            check(entry.path().filename().string().find(".tmp.") == std::string::npos,
                "staged commit file leaked: " + entry.path().string());
        }
    }
};

struct Handle {
    idydb* value = nullptr;
    ~Handle() { idydb_discard(&value); }
    void open(const fs::path& path, bool encrypted, int flags = 0) {
        idydb_open_options options = {};
        options.flags = flags;
        options.encrypted_at_rest = encrypted;
        options.passphrase = encrypted ? "failure-test-secret" : nullptr;
        options.pbkdf2_iter = 10000;
        const int status = idydb_open_with_options(path.u8string().c_str(), &value, &options);
        check(status == IDYDB_SUCCESS, "fixture open: " + std::string(idydb_errmsg(&value)));
    }
    void close() {
        check(idydb_close(&value) == IDYDB_DONE, "fixture close failed");
        check(value == nullptr, "successful close retained handle");
    }
    void discard() {
        check(idydb_discard(&value) == IDYDB_DONE, "discard failed");
        check(value == nullptr, "discard retained handle");
    }
};

constexpr float old_vector[] = {1.0f, 0.0f};
constexpr float new_vector[] = {0.0f, 1.0f};
const char* old_text = "original text";
const char* new_text = "replacement text with a different length";

void require_pair(Handle& handle, unsigned row, const char* text, const float* vector) {
    check(idydb_extract(&handle.value, 1, row) == IDYDB_DONE, "text extraction failed");
    check(idydb_retrieved_type(&handle.value) == IDYDB_CHAR, "text type changed");
    check(std::strcmp(idydb_retrieve_char(&handle.value), text) == 0, "text changed unexpectedly");
    check(idydb_extract(&handle.value, 2, row) == IDYDB_DONE, "vector extraction failed");
    check(idydb_retrieved_type(&handle.value) == IDYDB_VECTOR, "vector type changed");
    unsigned short dims = 0;
    const float* actual = idydb_retrieve_vector(&handle.value, &dims);
    check(actual && dims == 2 && actual[0] == vector[0] && actual[1] == vector[1],
          "vector changed unexpectedly");
}

void require_absent_pair(Handle& handle, unsigned row) {
    check(idydb_extract(&handle.value, 1, row) == IDYDB_NULL, "partial text insertion persisted");
    check(idydb_extract(&handle.value, 2, row) == IDYDB_NULL, "partial vector insertion persisted");
}

void require_marker(Handle& handle) {
    check(idydb_extract(&handle.value, 3, 1) == IDYDB_DONE, "unrelated cell disappeared");
    check(idydb_retrieved_type(&handle.value) == IDYDB_INTEGER &&
        idydb_retrieve_int(&handle.value) == 73, "unrelated cell changed");
}

void create_fixture(const fs::path& path, bool encrypted) {
    Handle handle;
    handle.open(path, encrypted, IDYDB_CREATE);
    check(idydb_rag_upsert_text(&handle.value, 1, 2, 1, old_text, old_vector, 2) == IDYDB_DONE,
          "fixture RAG insertion failed");
    check(idydb_insert_int(&handle.value, 3, 1, 73) == IDYDB_DONE,
          "fixture marker insertion failed");
    handle.close();
}

struct Injection {
    int stage;
    unsigned occurrence;
    int error;
    unsigned seen = 0;
    bool fired = false;
    unsigned cell_gate;
    unsigned writes_gate;
    unsigned cells = 0;
    unsigned writes = 0;
    Injection(int point, unsigned nth = 1, int err = EIO,
              unsigned after_cell = 0, unsigned after_writes = 0) :
        stage(point), occurrence(nth), error(err), cell_gate(after_cell), writes_gate(after_writes) {
        idydb_test_set_hook(callback, this);
    }
    ~Injection() { reset(); }
    static int callback(int point, void* context) {
        auto& self = *static_cast<Injection*>(context);
        if (point == IDYDB_TEST_CELL_WRITE) { ++self.cells; self.writes = 0; }
        if (point == IDYDB_TEST_WRITE && self.cells >= self.cell_gate) ++self.writes;
        if (self.cells < self.cell_gate || self.writes < self.writes_gate) return 0;
        if (point != self.stage || ++self.seen != self.occurrence) return 0;
        self.fired = true;
        return self.error;
    }
    void reset() { idydb_test_set_hook(nullptr, nullptr); }
    void require_fired(const std::string& operation) const {
        check(fired, operation + " did not exercise injected stage " + std::to_string(stage));
    }
};

struct Failure {
    int stage;
    unsigned occurrence;
    int error;
};

void test_mutation_failures(Files& files, bool encrypted) {
    const Failure failures[] = {
        {IDYDB_TEST_COPY_READ, 1, EIO},
        {IDYDB_TEST_COPY_WRITE, 1, EIO},
        {IDYDB_TEST_COPY_WRITE, 1, ENOSPC},
        {IDYDB_TEST_CELL_WRITE, 1, ENOSPC},
        {IDYDB_TEST_CELL_WRITE, 2, ENOSPC},
        {IDYDB_TEST_SEEK, 1, EIO},
        {IDYDB_TEST_FLUSH, 1, ENOSPC},
        {IDYDB_TEST_WRITE, 1, EIO},
        {IDYDB_TEST_WRITE, 1, ENOSPC}
    };
    for (const auto& failure : failures) {
        for (unsigned row : {1u, 2u}) {
            const fs::path path = files.next();
            create_fixture(path, encrypted);
            const std::string saved = bytes(path);
            Handle handle;
            handle.open(path, encrypted);
            {
                Injection injection(failure.stage, failure.occurrence, failure.error);
                check(idydb_rag_upsert_text(&handle.value, 1, 2, row, new_text, new_vector, 2) != IDYDB_DONE,
                      "RAG upsert accepted injected failure");
                injection.require_fired("RAG upsert");
                injection.reset();
            }
            check(bytes(path, &handle.value) == saved, "failed RAG upsert changed persisted bytes");
            require_pair(handle, 1, old_text, old_vector);
            require_absent_pair(handle, 2);
            require_marker(handle);
            files.require_no_staging();
            check(idydb_rag_upsert_text(&handle.value, 1, 2, row, new_text, new_vector, 2) == IDYDB_DONE,
                  "retry after failed RAG upsert failed");
            require_pair(handle, row, new_text, new_vector);
            require_marker(handle);
            handle.close();
            Handle reopened;
            reopened.open(path, encrypted, IDYDB_READONLY);
            require_pair(reopened, row, new_text, new_vector);
            require_marker(reopened);
            reopened.close();
        }
    }
}

void test_plain_commit_failures(Files& files) {
    for (const auto& failure : {Failure{IDYDB_TEST_FILE_SYNC, 1, EIO},
                               Failure{IDYDB_TEST_REPLACE, 1, EACCES}}) {
        const fs::path path = files.next();
        create_fixture(path, false);
        const std::string saved = bytes(path);
        Handle handle;
        handle.open(path, false);
        {
            Injection injection(failure.stage, failure.occurrence, failure.error);
            check(idydb_rag_upsert_text(&handle.value, 1, 2, 1, new_text, new_vector, 2) != IDYDB_DONE,
                  "plaintext commit accepted injected failure");
            injection.require_fired("plaintext commit");
            injection.reset();
        }
        check(bytes(path, &handle.value) == saved, "failed plaintext commit changed bytes");
        require_pair(handle, 1, old_text, old_vector);
        require_marker(handle);
        check(idydb_rag_upsert_text(&handle.value, 1, 2, 1, new_text, new_vector, 2) == IDYDB_DONE,
              "plaintext commit retry failed");
        handle.close();
        files.require_no_staging();
    }
}

void test_plain_failure_authoritative_path(Files& files) {
    // Inspect the pathname independently after releasing the failed writer.
    // A snapshot of its retained descriptor alone would miss a replacement
    // published to the pathname without adopting that replacement handle.
    for (int stage : {IDYDB_TEST_CELL_WRITE, IDYDB_TEST_FILE_SYNC, IDYDB_TEST_REPLACE}) {
        const fs::path path = files.next();
        create_fixture(path, false);
        const std::string saved = bytes(path);
        Handle handle;
        handle.open(path, false);
        {
            Injection injection(stage, stage == IDYDB_TEST_CELL_WRITE ? 2 : 1, EIO);
            check(idydb_rag_upsert_text(&handle.value, 1, 2, 1, new_text, new_vector, 2) != IDYDB_DONE,
                  "authoritative-path fixture accepted injected failure");
            injection.require_fired("authoritative-path pre-replacement failure");
            injection.reset();
        }
        handle.discard();
        check(bytes(path) == saved, "failed mutation changed authoritative pathname bytes");
        Handle reopened;
        reopened.open(path, false, IDYDB_READONLY);
        require_pair(reopened, 1, old_text, old_vector);
        require_absent_pair(reopened, 2);
        require_marker(reopened);
        reopened.close();
        files.require_no_staging();
    }
}

void test_staged_primitive_failures(Files& files, bool encrypted) {
    struct StagedFailure { int stage; int error; unsigned prior_writes; };
    const StagedFailure failures[] = {
        {IDYDB_TEST_WRITE, EIO, 0},
        {IDYDB_TEST_WRITE, ENOSPC, 0},
        {IDYDB_TEST_SEEK, EIO, 1},
        {IDYDB_TEST_FLUSH, ENOSPC, 0}
    };
    for (unsigned cell : {1u, 2u}) {
        for (bool shrinking : {false, true}) {
            for (const auto& failure : failures) {
                const fs::path path = files.next();
                create_fixture(path, encrypted);
                const char* initial = shrinking ? new_text : old_text;
                const char* replacement = shrinking ? "tiny" : new_text;
                if (shrinking) {
                    Handle setup;
                    setup.open(path, encrypted);
                    check(idydb_rag_upsert_text(&setup.value, 1, 2, 1, initial, old_vector, 2) == IDYDB_DONE,
                          "staged shrink fixture construction failed");
                    setup.close();
                }
                const std::string saved = bytes(path);
                Handle handle;
                handle.open(path, encrypted);
                {
                    Injection injection(failure.stage, 1, failure.error, cell, failure.prior_writes);
                    check(idydb_rag_upsert_text(&handle.value, 1, 2, 1, replacement, new_vector, 2) != IDYDB_DONE,
                          "staged primitive accepted injected failure");
                    injection.require_fired("staged primitive " + std::to_string(cell));
                    injection.reset();
                }
                check(bytes(path, &handle.value) == saved, "staged primitive failure changed backing image");
                require_pair(handle, 1, initial, old_vector);
                require_marker(handle);
                check(idydb_rag_upsert_text(&handle.value, 1, 2, 1, replacement, new_vector, 2) == IDYDB_DONE,
                      "retry after staged primitive failure failed");
                handle.close();
                Handle reopened;
                reopened.open(path, encrypted, IDYDB_READONLY);
                require_pair(reopened, 1, replacement, new_vector);
                require_marker(reopened);
                reopened.close();
                files.require_no_staging();
            }
        }
    }
}

void test_borrowed_rag_inputs(Files& files, bool encrypted) {
    for (bool vector_alias : {false, true}) {
        const fs::path path = files.next();
        create_fixture(path, encrypted);
        const std::string saved = bytes(path);
        Handle handle;
        handle.open(path, encrypted);
        const char* text = new_text;
        const float* embedding = new_vector;
        auto borrow = [&] {
            check(idydb_extract(&handle.value, vector_alias ? 2 : 1, 1) == IDYDB_DONE,
                  "borrowed RAG source extraction failed");
            if (vector_alias) {
                unsigned short dims = 0;
                embedding = idydb_retrieve_vector(&handle.value, &dims);
                check(embedding && dims == 2, "borrowed RAG source vector invalid");
            } else {
                text = idydb_retrieve_char(&handle.value);
                check(text != nullptr, "borrowed RAG source text invalid");
            }
        };
        borrow();
        {
            Injection injection(IDYDB_TEST_WRITE, 1, EIO, 2);
            check(idydb_rag_upsert_text(&handle.value, 1, 2, 2, text, embedding, 2) != IDYDB_DONE,
                  "borrowed-input RAG accepted embedding short write");
            injection.require_fired("borrowed-input RAG");
            injection.reset();
        }
        check(bytes(path, &handle.value) == saved, "borrowed-input RAG failure changed bytes");
        require_pair(handle, 1, old_text, old_vector);
        require_absent_pair(handle, 2);
        borrow();
        check(idydb_rag_upsert_text(&handle.value, 1, 2, 2, text, embedding, 2) == IDYDB_DONE,
              "borrowed-input RAG retry failed");
        handle.close();
        Handle reopened;
        reopened.open(path, encrypted, IDYDB_READONLY);
        require_pair(reopened, 1, old_text, old_vector);
        require_pair(reopened, 2, vector_alias ? new_text : old_text, vector_alias ? old_vector : new_vector);
        require_marker(reopened);
        reopened.close();
        files.require_no_staging();
    }
}

void test_failed_delete(Files& files) {
    const fs::path path = files.next();
    create_fixture(path, false);
    const std::string saved = bytes(path);
    Handle handle;
    handle.open(path, false);
    {
        Injection injection(IDYDB_TEST_WRITE, 1, ENOSPC);
        check(idydb_delete(&handle.value, 1, 1) != IDYDB_DONE, "delete accepted a short write");
        injection.require_fired("delete");
        injection.reset();
    }
    check(bytes(path, &handle.value) == saved, "failed delete changed bytes");
    require_pair(handle, 1, old_text, old_vector);
    require_marker(handle);
    check(idydb_delete(&handle.value, 1, 1) == IDYDB_DONE, "delete retry failed");
    check(idydb_extract(&handle.value, 1, 1) == IDYDB_NULL, "successful delete kept text");
    handle.close();
    files.require_no_staging();
}

void require_locked(const fs::path& path, bool encrypted) {
    idydb* competing = nullptr;
    idydb_open_options options = {};
    options.encrypted_at_rest = encrypted;
    options.passphrase = encrypted ? "failure-test-secret" : nullptr;
    options.pbkdf2_iter = 10000;
    const int status = idydb_open_with_options(path.u8string().c_str(), &competing, &options);
    check(idydb_discard(&competing) == IDYDB_DONE, "competing handle cleanup failed");
    check(status == IDYDB_BUSY, "failed persistence released the live writer lock");
}

void test_plain_open_lock_acquisition(Files& files) {
    const fs::path path = files.next();
    create_fixture(path, false);
    for (int flags : {0, IDYDB_READONLY}) {
        Handle handle;
        // A second acquisition fails even on platforms where relocking the
        // same descriptor would otherwise be allowed.
        Injection injection(IDYDB_TEST_LOCK, 2);
        handle.open(path, false, flags);
        check(injection.seen == 1 && !injection.fired,
              "plaintext open must acquire its file lock exactly once");
        injection.reset();
        require_pair(handle, 1, old_text, old_vector);
        require_marker(handle);
        handle.close();
    }
}

void test_encrypted_persistence_failures(Files& files) {
    const Failure failures[] = {
        {IDYDB_TEST_SEEK, 1, EIO},
        {IDYDB_TEST_FLUSH, 1, ENOSPC},
        {IDYDB_TEST_WRITE, 1, EIO},
        {IDYDB_TEST_WRITE, 1, ENOSPC},
        {IDYDB_TEST_FILE_SYNC, 1, EIO},
        {IDYDB_TEST_REPLACE, 1, EACCES}
    };
    for (const auto& failure : failures) {
        for (bool close : {false, true}) {
            const fs::path path = files.next();
            create_fixture(path, true);
            const std::string saved = bytes(path);
            Handle handle;
            handle.open(path, true);
            check(idydb_rag_upsert_text(&handle.value, 1, 2, 1, new_text, new_vector, 2) == IDYDB_DONE,
                  "pending encrypted upsert failed");
            check(bytes(path, &handle.value) == saved, "encrypted upsert persisted before flush");
            idydb* original = handle.value;
            {
                Injection injection(failure.stage, failure.occurrence, failure.error);
                const int status = close ? idydb_close(&handle.value) : idydb_flush(&handle.value);
                check(status != IDYDB_DONE, "persistence accepted injected failure");
                injection.require_fired(close ? "close" : "flush");
                injection.reset();
            }
            check(handle.value == original, "failed persistence discarded live handle");
            check(bytes(path, &handle.value) == saved, "failed encrypted persistence changed bytes");
            require_pair(handle, 1, new_text, new_vector);
            require_marker(handle);
            require_locked(path, true);
            files.require_no_staging();
            handle.discard();
            Handle reopened;
            reopened.open(path, true);
            require_pair(reopened, 1, old_text, old_vector);
            require_marker(reopened);
            check(idydb_rag_upsert_text(&reopened.value, 1, 2, 1, new_text, new_vector, 2) == IDYDB_DONE,
                  "writer after discard cannot mutate");
            check(idydb_flush(&reopened.value) == IDYDB_DONE, "writer after discard cannot persist");
            require_locked(path, true);
            reopened.close();
        }
    }
}

void test_plain_failed_close_cleanup(Files& files) {
    for (int stage : {IDYDB_TEST_FLUSH, IDYDB_TEST_FILE_SYNC}) {
        for (bool close : {false, true}) {
            const fs::path path = files.next();
            create_fixture(path, false);
            Handle handle;
            handle.open(path, false);
            check(idydb_rag_upsert_text(&handle.value, 1, 2, 1, new_text, new_vector, 2) == IDYDB_DONE,
                  "plaintext cleanup fixture mutation failed");
            const std::string committed = bytes(path, &handle.value);
            idydb* original = handle.value;
            {
                Injection injection(stage);
                check((close ? idydb_close(&handle.value) : idydb_flush(&handle.value)) != IDYDB_DONE,
                      "plaintext persistence accepted injected failure");
                injection.require_fired(close ? "plaintext close" : "plaintext flush");
                injection.reset();
            }
            check(handle.value == original, "failed plaintext persistence discarded live handle");
            require_pair(handle, 1, new_text, new_vector);
            require_locked(path, false);
            handle.discard();
            check(bytes(path) == committed, "discard rolled back a committed plaintext mutation");
            Handle reopened;
            reopened.open(path, false);
            require_pair(reopened, 1, new_text, new_vector);
            reopened.close();
        }
    }
}

void test_encrypted_retry(Files& files) {
    const fs::path path = files.next();
    create_fixture(path, true);
    const std::string saved = bytes(path);
    Handle handle;
    handle.open(path, true);
    check(idydb_rag_upsert_text(&handle.value, 1, 2, 1, new_text, new_vector, 2) == IDYDB_DONE,
          "encrypted retry fixture mutation failed");
    idydb* original = handle.value;
    {
        Injection injection(IDYDB_TEST_FILE_SYNC);
        check(idydb_close(&handle.value) != IDYDB_DONE, "encrypted close ignored sync failure");
        injection.require_fired("encrypted retry close");
        injection.reset();
    }
    check(handle.value == original && bytes(path, &handle.value) == saved, "encrypted failed close lost retry state");
    check(idydb_flush(&handle.value) == IDYDB_DONE, "encrypted flush retry failed");
    check(handle.value == original && bytes(path, &handle.value) != saved, "encrypted flush retry lost its handle or update");
    require_locked(path, true);
    check(idydb_rag_upsert_text(&handle.value, 1, 2, 2, new_text, new_vector, 2) == IDYDB_DONE,
          "encrypted mutation after retry failed");
    handle.close();
    Handle reopened;
    reopened.open(path, true, IDYDB_READONLY);
    require_pair(reopened, 1, new_text, new_vector);
    require_pair(reopened, 2, new_text, new_vector);
    require_marker(reopened);
    reopened.close();
    files.require_no_staging();
}

void test_post_replacement_failure(Files& files, bool encrypted) {
    const fs::path path = files.next();
    create_fixture(path, encrypted);
    const std::string saved = bytes(path);
    Handle handle;
    handle.open(path, encrypted);
    if (encrypted) {
        check(idydb_rag_upsert_text(&handle.value, 1, 2, 1, new_text, new_vector, 2) == IDYDB_DONE,
              "pending encrypted upsert failed");
    }
    {
        Injection injection(IDYDB_TEST_DIRECTORY_SYNC);
        const int status = encrypted ? idydb_flush(&handle.value) :
            idydb_rag_upsert_text(&handle.value, 1, 2, 1, new_text, new_vector, 2);
        check(status != IDYDB_DONE, "directory-sync failure was hidden");
        injection.require_fired("replacement directory sync");
        injection.reset();
    }
    check(bytes(path, &handle.value) != saved, "directory-sync failure did not occur after replacement");
    require_pair(handle, 1, new_text, new_vector);
    require_marker(handle);
    require_locked(path, encrypted);
    handle.discard();
    Handle reopened;
    reopened.open(path, encrypted, IDYDB_READONLY);
    require_pair(reopened, 1, new_text, new_vector);
    require_marker(reopened);
    reopened.close();
    files.require_no_staging();
}

void test_plain_directory_sync_retry(Files& files) {
    const fs::path path = files.next();
    create_fixture(path, false);
    const std::string saved = bytes(path);
    Handle handle;
    handle.open(path, false);
    idydb* original = handle.value;
    {
        Injection injection(IDYDB_TEST_DIRECTORY_SYNC);
        check(idydb_rag_upsert_text(&handle.value, 1, 2, 1, new_text, new_vector, 2) == IDYDB_ERROR,
              "plaintext commit ignored directory-sync failure");
        injection.require_fired("plaintext commit directory sync");
        injection.reset();
    }
    const std::string committed = bytes(path, &handle.value);
    check(committed != saved, "plaintext directory-sync failure occurred before replacement");
    check(handle.value == original, "post-replacement failure discarded plaintext handle");
    require_pair(handle, 1, new_text, new_vector);
    require_marker(handle);
    require_locked(path, false);

    // Flushing the replacement's data alone does not retry durability of its
    // directory entry. Both explicit persistence paths must retry that sync.
    for (bool close : {false, true}) {
        Injection injection(IDYDB_TEST_DIRECTORY_SYNC);
        check((close ? idydb_close(&handle.value) : idydb_flush(&handle.value)) == IDYDB_ERROR,
              "plaintext retry ignored repeated directory-sync failure");
        injection.require_fired(close ? "plaintext retry close directory sync" :
                                      "plaintext retry flush directory sync");
        injection.reset();
        check(handle.value == original, "directory-sync retry discarded plaintext handle");
        check(bytes(path, &handle.value) == committed, "directory-sync retry rewrote committed plaintext bytes");
        require_pair(handle, 1, new_text, new_vector);
        require_marker(handle);
        require_locked(path, false);
    }
    check(idydb_flush(&handle.value) == IDYDB_DONE, "plaintext directory-sync flush retry failed");
    check(handle.value == original, "successful plaintext flush changed its handle");
    require_locked(path, false);
    handle.close();
    check(bytes(path) == committed, "successful persistence retry changed plaintext bytes");
    Handle reopened;
    reopened.open(path, false, IDYDB_READONLY);
    require_pair(reopened, 1, new_text, new_vector);
    require_marker(reopened);
    reopened.close();
    files.require_no_staging();
}

void test_flush_and_discard(Files& files) {
    check(idydb_discard(nullptr) == IDYDB_DONE, "discard rejects null pointer");
    idydb* missing = nullptr;
    check(idydb_discard(&missing) == IDYDB_DONE, "discard rejects empty handle");
    for (bool encrypted : {false, true}) {
        const fs::path path = files.next();
        create_fixture(path, encrypted);
        const std::string saved = bytes(path);
        {
            Handle readonly;
            readonly.open(path, encrypted, IDYDB_READONLY);
            Injection injection(IDYDB_TEST_FLUSH);
            check(idydb_flush(&readonly.value) == IDYDB_DONE, "read-only flush failed");
            check(!injection.fired, "read-only flush attempted writeback");
            injection.reset();
            readonly.close();
        }
        check(bytes(path) == saved, "read-only flush changed bytes");
        {
            Handle handle;
            handle.open(path, encrypted);
            check(idydb_insert_int(&handle.value, 3, 1, 99) == IDYDB_DONE,
                  "discard fixture mutation failed");
            handle.discard();
        }
        Handle reopened;
        reopened.open(path, encrypted);
        check(idydb_extract(&reopened.value, 3, 1) == IDYDB_DONE, "discard fixture extraction failed");
        check(idydb_retrieve_int(&reopened.value) == (encrypted ? 73 : 99),
              "discard persistence boundary incorrect");
        reopened.close();
    }
}

struct Crash {
    int stage;
    static int callback(int point, void* context) {
        if (point == static_cast<Crash*>(context)->stage) idydb_test::crash_exit(91);
        return 0;
    }
};

void test_interrupted_commit(Files& files, bool encrypted, bool after) {
    const fs::path path = files.next();
    create_fixture(path, encrypted);
    const std::string saved = bytes(path);
    std::string expected_new;
    if (!encrypted) {
        const fs::path expected_path = files.next();
        create_fixture(expected_path, false);
        Handle expected;
        expected.open(expected_path, false);
        check(idydb_rag_upsert_text(&expected.value, 1, 2, 1, new_text, new_vector, 2) == IDYDB_DONE,
              "interruption expected-image construction failed");
        expected.close();
        expected_new = bytes(expected_path);
    }
    const int status = idydb_test::child(test_executable, {"--crash-child",
        idydb_test::hex(path.u8string()), encrypted ? "1" : "0", after ? "1" : "0"});
    check(status == 91, "commit did not reach crash boundary: " + std::to_string(status));
    check(after ? bytes(path) != saved : bytes(path) == saved, "interruption changed the wrong file image");
    if (after && !encrypted) check(bytes(path) == expected_new, "interruption exposed an incomplete new image");
    Handle reopened;
    reopened.open(path, encrypted, IDYDB_READONLY);
    require_pair(reopened, 1, after ? new_text : old_text, after ? new_vector : old_vector);
    require_marker(reopened);
    reopened.close();
    // A process interrupted before rename can leave its fully prepared temporary
    // file behind; the authoritative database must still open as a complete image.
    for (const auto& entry : fs::directory_iterator(files.directory)) {
        if (entry.path().filename().string().find(".tmp.") != std::string::npos) fs::remove(entry.path());
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc == 5 && std::string(argv[1]) == "--crash-child") {
        try {
            Handle handle;
            const bool encrypted = std::string(argv[3]) == "1";
            handle.open(fs::u8path(idydb_test::unhex(argv[2])), encrypted);
            Crash crash = {std::string(argv[4]) == "1" ? IDYDB_TEST_AFTER_REPLACE : IDYDB_TEST_BEFORE_REPLACE};
            idydb_test_set_hook(Crash::callback, &crash);
            const int status = idydb_rag_upsert_text(&handle.value, 1, 2, 1, new_text, new_vector, 2);
            if (encrypted && status == IDYDB_DONE) idydb_flush(&handle.value);
            idydb_test::crash_exit(92);
        } catch (...) { idydb_test::crash_exit(93); }
    }
    test_executable = idydb_test::executable(argv[0]);
    Files files;
    try {
        test_mutation_failures(files, false);
        test_mutation_failures(files, true);
        test_staged_primitive_failures(files, false);
        test_staged_primitive_failures(files, true);
        test_borrowed_rag_inputs(files, false);
        test_borrowed_rag_inputs(files, true);
        test_plain_commit_failures(files);
        test_plain_failure_authoritative_path(files);
        test_failed_delete(files);
        test_plain_open_lock_acquisition(files);
        test_encrypted_persistence_failures(files);
        test_plain_failed_close_cleanup(files);
        test_encrypted_retry(files);
        test_post_replacement_failure(files, false);
        test_post_replacement_failure(files, true);
        test_plain_directory_sync_retry(files);
        test_flush_and_discard(files);
        for (bool encrypted : {false, true}) {
            test_interrupted_commit(files, encrypted, false);
            test_interrupted_commit(files, encrypted, true);
        }
        files.require_no_staging();
        fs::remove_all(files.directory);
        std::cout << "Atomic mutation, persistence failure, discard, and interruption tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        idydb_test_set_hook(nullptr, nullptr);
        std::cerr << error.what() << " (fixtures: " << files.directory << ")\n";
        return 1;
    }
}
