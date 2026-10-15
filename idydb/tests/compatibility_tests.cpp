#include "db.h"

#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>

#ifndef IDYDB_FIXTURE_DIR
#error IDYDB_FIXTURE_DIR must name the checked-in compatibility fixtures
#endif
static_assert(CHAR_BIT == 8 && sizeof(short) == 2 && sizeof(int) == 4,
              "The current native record format requires 8/16/32-bit byte/short/int");
static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559,
              "The current vector format requires IEEE-754 binary32");
static_assert(sizeof(unsigned long long) == 8, "The BIG API requires a 64-bit index type");

namespace {
namespace fs = std::filesystem;
constexpr const char* password = "public-idydb-v1-compatibility-fixture";
constexpr const char* text = u8"Portable العربية café 🐝\r\n\x1a";
constexpr float vector[] = {1.0f, -2.0f, 0.5f, 0.0f};
void check(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
struct Handle {
    idydb* value = nullptr;
    ~Handle() { idydb_discard(&value); }
    void open(const fs::path& path, bool encrypted, int flags) {
        idydb_open_options options = {};
        options.flags = flags;
        options.encrypted_at_rest = encrypted;
        options.passphrase = encrypted ? password : nullptr;
        options.pbkdf2_iter = 10000;
        const int status = idydb_open_with_options(path.u8string().c_str(), &value, &options);
        check(status == IDYDB_SUCCESS, "open compatibility image failed: " + path.u8string() +
              ": " + idydb_errmsg(&value));
    }
    void close() { check(idydb_close(&value) == IDYDB_DONE, "close compatibility image failed"); }
};
std::string bytes(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    check(stream.is_open(), "cannot read compatibility bytes");
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
int embed(const char*, float** output, unsigned short* dimensions, void*) {
    *dimensions = 4;
    *output = static_cast<float*>(idydb_alloc(sizeof(vector)));
    if (!*output) return 1;
    std::memcpy(*output, vector, sizeof(vector));
    return 0;
}
void write(const fs::path& path, bool encrypted) {
    check(!fs::exists(path), "refusing to overwrite compatibility image: " + path.u8string());
    Handle handle;
    handle.open(path, encrypted, IDYDB_CREATE);
    check(idydb_insert_int(&handle.value, 1, 1, INT_MIN) == IDYDB_DONE, "write INT_MIN");
    const int maximum_status = idydb_insert_int(&handle.value, 1, 2, INT_MAX);
    check(maximum_status == IDYDB_DONE, "write INT_MAX: " + std::string(idydb_errmsg(&handle.value)));
    check(idydb_insert_int(&handle.value, 1, 3, -1234567) == IDYDB_DONE, "write negative integer");
    check(idydb_insert_float(&handle.value, 2, 1, 1.5f) == IDYDB_DONE, "write float");
    check(idydb_insert_float(&handle.value, 2, 2, -0.25f) == IDYDB_DONE, "write negative float");
    check(idydb_insert_float(&handle.value, 2, 3, -0.0f) == IDYDB_DONE, "write signed zero");
    check(idydb_insert_const_char(&handle.value, 3, 1, text) == IDYDB_DONE, "write UTF-8 text");
    check(idydb_insert_const_char(&handle.value, 3, 2, "") == IDYDB_DONE, "write empty text");
    check(idydb_insert_bool(&handle.value, 4, 1, true) == IDYDB_DONE, "write true");
    check(idydb_insert_bool(&handle.value, 4, 2, false) == IDYDB_DONE, "write false");
    check(idydb_insert_vector(&handle.value, 5, 1, vector, 4) == IDYDB_DONE, "write vector");
    check(idydb_insert_int(&handle.value, 6, 1, 9) == IDYDB_DONE, "write deleted cell");
    check(idydb_delete(&handle.value, 6, 1) == IDYDB_DONE, "delete compatibility cell");
    idydb_set_embedder(&handle.value, embed, nullptr);
    check(idydb_rag_upsert_text_auto_embed(&handle.value, 7, 8, 1, text) == IDYDB_DONE,
          "library-allocated embedder vector failed");
    check(idydb_flush(&handle.value) == IDYDB_DONE, "flush compatibility image");
    handle.close();
}
void read(const fs::path& path, bool encrypted) {
    const std::string before = bytes(path);
    Handle handle;
    handle.open(path, encrypted, IDYDB_READONLY);
    for (unsigned row = 1; row <= 3; ++row) {
        const int expected[] = {INT_MIN, INT_MAX, -1234567};
        check(idydb_extract(&handle.value, 1, row) == IDYDB_DONE &&
              idydb_retrieved_type(&handle.value) == IDYDB_INTEGER &&
              idydb_retrieve_int(&handle.value) == expected[row - 1], "integer representation mismatch");
    }
    for (unsigned row = 1; row <= 3; ++row) {
        const float expected[] = {1.5f, -0.25f, -0.0f};
        check(idydb_extract(&handle.value, 2, row) == IDYDB_DONE &&
              idydb_retrieved_type(&handle.value) == IDYDB_FLOAT &&
              idydb_retrieve_float(&handle.value) == expected[row - 1], "float representation mismatch");
        if (row == 3) check(std::signbit(idydb_retrieve_float(&handle.value)), "signed zero lost");
    }
    for (unsigned row = 1; row <= 2; ++row) {
        check(idydb_extract(&handle.value, 3, row) == IDYDB_DONE &&
              idydb_retrieved_type(&handle.value) == IDYDB_CHAR &&
              std::strcmp(idydb_retrieve_char(&handle.value), row == 1 ? text : "") == 0,
              "text bytes or empty string changed");
        check(idydb_extract(&handle.value, 4, row) == IDYDB_DONE &&
              idydb_retrieved_type(&handle.value) == IDYDB_BOOL &&
              idydb_retrieve_bool(&handle.value) == (row == 1), "boolean representation mismatch");
    }
    for (unsigned column : {5u, 8u}) {
        check(idydb_extract(&handle.value, column, 1) == IDYDB_DONE, "vector extraction failed");
        unsigned short dimensions = 0;
        const float* actual = idydb_retrieve_vector(&handle.value, &dimensions);
        check(actual && dimensions == 4 && std::memcmp(actual, vector, sizeof(vector)) == 0,
              "vector representation mismatch");
    }
    check(idydb_extract(&handle.value, 6, 1) == IDYDB_NULL, "deleted cell returned");
    check(idydb_extract(&handle.value, 7, 1) == IDYDB_DONE &&
          std::strcmp(idydb_retrieve_char(&handle.value), text) == 0, "RAG text changed");
    idydb_knn_result hits[1] = {};
    check(idydb_knn_search_vector_column(&handle.value, 5, vector, 4, 1, IDYDB_SIM_COSINE, hits) == 1 &&
          hits[0].row == 1 && std::fabs(hits[0].score - 1.0f) < 1e-6f, "fixture query failed");
    handle.close();
    check(bytes(path) == before, "read-only compatibility check changed bytes");
}
void pair(const fs::path& directory, bool create) {
    if (create) {
        fs::create_directories(directory);
        write(directory / "plaintext.idydb", false);
        write(directory / "encrypted.idydb", true);
    }
    read(directory / "plaintext.idydb", false);
    read(directory / "encrypted.idydb", true);
}
}
int main(int argc, char** argv) {
    try {
        const std::uint16_t endian = 1;
        check(*reinterpret_cast<const unsigned char*>(&endian) == 1,
              "Current checked-in fixtures target little-endian platforms");
        if (argc == 3 && (std::string(argv[1]) == "--write" || std::string(argv[1]) == "--read")) {
            pair(fs::u8path(argv[2]), std::string(argv[1]) == "--write");
        } else if (argc == 3 && std::string(argv[1]) == "--generate-fixtures") {
            const fs::path directory = fs::u8path(argv[2]);
            fs::create_directories(directory);
            write(directory / "portable-v1.db", false);
            write(directory / "portable-v1.encrypted.db", true);
        } else {
            check(argc == 1, "Usage: compatibility_tests [--write|--read DIRECTORY]");
            const char* fixture_override = std::getenv("IDYDB_FIXTURE_DIR");
            const fs::path fixtures = fs::u8path(fixture_override ? fixture_override : IDYDB_FIXTURE_DIR);
            read(fixtures / "portable-v1.db", false);
            read(fixtures / "portable-v1.encrypted.db", true);
            const fs::path directory = fs::temp_directory_path() / ("idydb-compatibility-" +
                std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            pair(directory, true);
            check(bytes(directory / "plaintext.idydb") == bytes(fixtures / "portable-v1.db"),
                  "writer changed fixed plaintext record encoding");
            fs::remove_all(directory);
        }
        std::cout << "Plaintext and encrypted cross-platform compatibility checks passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
