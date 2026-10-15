#include "db.h"
#include "failure_hooks.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
#define CHECK(expr) do { if (!(expr)) throw std::runtime_error(#expr); } while (0)
static std::string bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
static std::string locked_bytes(idydb** handle, const fs::path& path) {
#if defined(_WIN32)
    (void)path;
    unsigned char* data = nullptr;
    size_t size = 0;
    CHECK(idydb_test_backing_snapshot(handle, &data, &size) == IDYDB_DONE);
    std::string result(reinterpret_cast<char*>(data), size);
    idydb_free(data);
    return result;
#else
    (void)handle;
    return bytes(path);
#endif
}
static int encrypted(const fs::path& path, idydb** handle, int flags, const char* password) {
    idydb_open_options options = {};
    options.flags = flags;
    options.encrypted_at_rest = true;
    options.passphrase = password;
    options.pbkdf2_iter = 10000;
    return idydb_open_with_options(path.u8string().c_str(), handle, &options);
}
int main() {
    const fs::path root = fs::temp_directory_path() / ("idydb-persistence-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(root);
    idydb* handle = nullptr;
    try {
        auto path = root / "encrypted.db";
        CHECK(encrypted(path, &handle, IDYDB_CREATE, "secret") == IDYDB_SUCCESS);
        CHECK(idydb_insert_const_char(&handle, 1, 1, "private payload") == IDYDB_DONE);
        idydb* original = handle;
        CHECK(idydb_open(path.u8string().c_str(), &handle, IDYDB_CREATE) == IDYDB_ERROR);
        CHECK(handle == original);
        idydb* competing = nullptr;
        CHECK(encrypted(path, &competing, IDYDB_CREATE, "secret") == IDYDB_BUSY);
        CHECK(idydb_close(&competing) == IDYDB_DONE);
        CHECK(idydb_close(&handle) == IDYDB_DONE);
        std::string saved = bytes(path);
        CHECK(saved.substr(0, 8) == "IDYDBENC");
        CHECK(saved.find("private payload") == std::string::npos);
        CHECK(encrypted(path, &handle, IDYDB_READONLY, "wrong") == IDYDB_ERROR);
        CHECK(idydb_close(&handle) == IDYDB_DONE);
        CHECK(bytes(path) == saved);
        CHECK(encrypted(path, &handle, IDYDB_READONLY, "secret") == IDYDB_SUCCESS);
        CHECK(idydb_extract(&handle, 1, 1) == IDYDB_DONE);
        CHECK(std::string(idydb_retrieve_char(&handle)) == "private payload");
        CHECK(idydb_insert_int(&handle, 2, 1, 8) == IDYDB_READONLY);
        CHECK(idydb_close(&handle) == IDYDB_DONE);
        CHECK(bytes(path) == saved);

        // Force replacement failure after ciphertext has been staged. The old
        // backing bytes and the live working handle must remain recoverable.
        CHECK(encrypted(path, &handle, 0, "secret") == IDYDB_SUCCESS);
        CHECK(idydb_insert_const_char(&handle, 1, 1, "changed payload") == IDYDB_DONE);
        auto backup = root / "original.db";
        fs::rename(path, backup);
        fs::create_directory(path);
        CHECK(idydb_close(&handle) == IDYDB_ERROR);
        CHECK(handle != nullptr);
        CHECK(locked_bytes(&handle, backup) == saved);
        CHECK(idydb_extract(&handle, 1, 1) == IDYDB_DONE);
        CHECK(std::string(idydb_retrieve_char(&handle)) == "changed payload");
        fs::remove(path);
        fs::rename(backup, path);
        CHECK(idydb_close(&handle) == IDYDB_DONE);
        CHECK(encrypted(path, &handle, IDYDB_READONLY, "secret") == IDYDB_SUCCESS);
        CHECK(idydb_extract(&handle, 1, 1) == IDYDB_DONE);
        CHECK(std::string(idydb_retrieve_char(&handle)) == "changed payload");
        CHECK(idydb_close(&handle) == IDYDB_DONE);
        saved = bytes(path);
        saved.back() ^= 1;
        { std::ofstream stream(path, std::ios::binary | std::ios::trunc); stream.write(saved.data(), saved.size()); }
        CHECK(encrypted(path, &handle, IDYDB_READONLY, "secret") == IDYDB_ERROR);
        CHECK(idydb_close(&handle) == IDYDB_DONE);
        CHECK(bytes(path) == saved);

        auto plain = root / "migration.db";
        CHECK(idydb_open(plain.u8string().c_str(), &handle, IDYDB_CREATE) == IDYDB_SUCCESS);
        CHECK(idydb_insert_int(&handle, 1, 1, 42) == IDYDB_DONE);
        CHECK(idydb_close(&handle) == IDYDB_DONE);
        std::string plain_saved = bytes(plain);
        CHECK(encrypted(plain, &handle, IDYDB_READONLY, "secret") == IDYDB_ERROR);
        CHECK(idydb_close(&handle) == IDYDB_DONE);
        CHECK(bytes(plain) == plain_saved);
        CHECK(encrypted(plain, &handle, 0, "secret") == IDYDB_SUCCESS);
        CHECK(idydb_close(&handle) == IDYDB_DONE);
        CHECK(encrypted(plain, &handle, IDYDB_READONLY, "secret") == IDYDB_SUCCESS);
        CHECK(idydb_extract(&handle, 1, 1) == IDYDB_DONE);
        CHECK(idydb_retrieve_int(&handle) == 42);
        CHECK(idydb_close(&handle) == IDYDB_DONE);
        for (const auto& entry : fs::directory_iterator(root)) CHECK(entry.path().string().find(".tmp.") == std::string::npos);
        fs::remove_all(root);
        std::cout << "Persistence tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << " (fixtures: " << root << ")\n";
        return 1;
    }
}
