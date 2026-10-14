#include "db.h"

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
#if IDYDB_SIZING_MODE == IDYDB_SIZING_MODE_TINY
constexpr unsigned max_position = 16;
#elif IDYDB_SIZING_MODE == IDYDB_SIZING_MODE_SMALL
constexpr unsigned max_position = 256;
#else
constexpr unsigned max_position = 65536;
#endif

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

struct TestFiles {
    std::filesystem::path directory = std::filesystem::temp_directory_path() /
        ("idydb-core-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TestFiles() { std::filesystem::create_directory(directory); }
    ~TestFiles() { std::error_code error; std::filesystem::remove_all(directory, error); }
    std::filesystem::path file(const char* name) const { return directory / name; }
};

struct Handle {
    idydb* value = nullptr;
    ~Handle() { idydb_close(&value); }
    void open(const std::filesystem::path& file, int flags = IDYDB_CREATE) {
        const int rc = idydb_open(file.string().c_str(), &value, flags);
        check(rc == IDYDB_SUCCESS, "open failed: " + std::string(idydb_errmsg(&value)));
    }
    void close() { check(idydb_close(&value) == IDYDB_DONE, "close failed"); }
};

void require_int(Handle& handle, unsigned long long column, unsigned long long row, int expected) {
    check(idydb_extract(&handle.value, column, row) == IDYDB_DONE, "extract integer failed");
    check(idydb_retrieved_type(&handle.value) == IDYDB_INTEGER, "integer type mismatch");
    check(idydb_retrieve_int(&handle.value) == expected, "integer value mismatch");
}

void require_text(Handle& handle, unsigned long long column, unsigned long long row, const std::string& expected) {
    check(idydb_extract(&handle.value, column, row) == IDYDB_DONE, "extract string failed");
    check(idydb_retrieved_type(&handle.value) == IDYDB_CHAR, "string type mismatch");
    check(idydb_retrieve_char(&handle.value) == expected, "string value mismatch");
}

void null_arguments() {
    idydb* missing = nullptr;
    unsigned short dims = 123;
    check(idydb_extract(nullptr, 1, 1) == IDYDB_ERROR, "null extract handler");
    check(idydb_extract(&missing, 1, 1) == IDYDB_ERROR, "unopened extract handler");
    check(idydb_insert_int(&missing, 1, 1, 5) == IDYDB_ERROR, "unopened insert handler");
    check(idydb_insert_char(nullptr, 1, 1, nullptr) == IDYDB_ERROR, "null string handler");
    check(idydb_insert_vector(nullptr, 1, 1, nullptr, 1) == IDYDB_ERROR, "null vector handler");
    check(idydb_delete(nullptr, 1, 1) == IDYDB_ERROR, "null delete handler");
    check(idydb_retrieved_type(nullptr) == IDYDB_NULL, "null retrieved type");
    check(idydb_retrieve_int(nullptr) == 0, "null retrieve integer");
    check(idydb_retrieve_float(&missing) == 0, "null retrieve float");
    check(!idydb_retrieve_bool(nullptr), "null retrieve bool");
    check(idydb_retrieve_char(nullptr) == nullptr, "null retrieve string");
    check(idydb_retrieve_vector(nullptr, &dims) == nullptr && dims == 0, "null retrieve vector");
    check(idydb_close(nullptr) == IDYDB_DONE, "null close handler");
}

void payload_limits_and_aliases(const TestFiles& files) {
    Handle handle;
    const auto path = files.file("payloads.db");
    handle.open(path);
    check(idydb_insert_const_char(&handle.value, 1, 1, "") == IDYDB_DONE, "store empty string");
    require_text(handle, 1, 1, "");
    const std::string longest(65532, 'x');
    check(idydb_insert_const_char(&handle.value, 1, 1, longest.c_str()) == IDYDB_DONE, "store maximum string");
    require_text(handle, 1, 1, longest);
    char* alias = idydb_retrieve_char(&handle.value);
    check(idydb_insert_char(&handle.value, 1, 2, alias) == IDYDB_DONE, "copy retrieved string");
    require_text(handle, 1, 2, longest);
    const std::string oversized(65533, 'y');
    check(idydb_insert_const_char(&handle.value, 1, 1, oversized.c_str()) == IDYDB_ERROR, "reject oversized string");
    require_text(handle, 1, 1, longest);
    check(idydb_insert_const_char(&handle.value, 1, 1, nullptr) == IDYDB_ERROR, "reject null string");
    std::vector<float> vector(16383, 0.25f);
    check(idydb_insert_vector(&handle.value, 2, 1, vector.data(), 16383) == IDYDB_DONE, "store maximum vector");
    check(idydb_insert_int(&handle.value, 3, 1, 901) == IDYDB_DONE, "store suffix record");
    check(idydb_extract(&handle.value, 2, 1) == IDYDB_DONE, "extract maximum vector");
    unsigned short dims = 0;
    const float* vector_alias = idydb_retrieve_vector(&handle.value, &dims);
    check(dims == 16383 && vector_alias && vector_alias[dims - 1] == 0.25f, "maximum vector payload");
    check(idydb_insert_vector(&handle.value, 2, 2, vector_alias, dims) == IDYDB_DONE, "copy retrieved vector");
    check(idydb_insert_vector(&handle.value, 2, 1, vector.data(), 16384) == IDYDB_ERROR, "reject oversized vector");
    check(idydb_insert_vector(&handle.value, 2, 1, nullptr, 1) == IDYDB_ERROR, "reject null vector");
    check(idydb_insert_vector(&handle.value, 2, 1, vector.data(), 0) == IDYDB_ERROR, "reject zero dimensions");
    check(idydb_insert_const_char(&handle.value, 1, 1, "small") == IDYDB_DONE, "shrink maximum string");
    check(idydb_delete(&handle.value, 1, 2) == IDYDB_DONE, "delete maximum string");
    check(idydb_insert_bool(&handle.value, 2, 1, true) == IDYDB_DONE, "shrink maximum vector to bool");
    check(idydb_delete(&handle.value, 2, 2) == IDYDB_DONE, "delete maximum vector");
    require_int(handle, 3, 1, 901);
    check(idydb_insert_int(&handle.value, 0, 1, 1) == IDYDB_RANGE, "reject zero column");
    check(idydb_insert_int(&handle.value, 1, max_position + 1, 1) == IDYDB_RANGE, "reject row overflow");
    handle.close();
    handle.open(path, IDYDB_READONLY);
    require_text(handle, 1, 1, "small");
    require_int(handle, 3, 1, 901);
    check(idydb_insert_int(&handle.value, 1, 1, 99) == IDYDB_READONLY, "readonly mutation");
    require_text(handle, 1, 1, "small");
}

struct Cell {
    int type;
    int integer = 0;
    bool boolean = false;
    std::string text;
};

void mutation_sequences(const TestFiles& files) {
    Handle handle;
    const auto path = files.file("mutations.db");
    handle.open(path);
    std::map<std::pair<unsigned, unsigned>, Cell> expected;
    std::mt19937 random(197701);
    for (unsigned iteration = 0; iteration < 500; ++iteration) {
        const unsigned column = random() % 12 + 1;
        const unsigned row = random() % (max_position < 20 ? max_position : 20) + 1;
        const auto key = std::make_pair(column, row);
        const unsigned operation = random() % 4;
        int rc = IDYDB_ERROR;
        if (operation == 0) {
            rc = idydb_delete(&handle.value, column, row);
            expected.erase(key);
        } else if (operation == 1) {
            const int value = static_cast<int>(random() % 100000);
            rc = idydb_insert_int(&handle.value, column, row, value);
            expected[key] = Cell{IDYDB_INTEGER, value, false, {}};
        } else if (operation == 2) {
            const bool value = (random() & 1) != 0;
            rc = idydb_insert_bool(&handle.value, column, row, value);
            expected[key] = Cell{IDYDB_BOOL, 0, value, {}};
        } else {
            const std::string value(random() % 2500, static_cast<char>('a' + random() % 26));
            rc = idydb_insert_const_char(&handle.value, column, row, value.c_str());
            expected[key] = Cell{IDYDB_CHAR, 0, false, value};
        }
        check(rc == IDYDB_DONE, "mutation failed at iteration " + std::to_string(iteration));
        for (const auto& entry : expected) {
            check(idydb_extract(&handle.value, entry.first.first, entry.first.second) == IDYDB_DONE,
                  "mutation sequence lost cell at iteration " + std::to_string(iteration));
            check(idydb_retrieved_type(&handle.value) == entry.second.type, "mutation sequence changed type");
            if (entry.second.type == IDYDB_INTEGER) check(idydb_retrieve_int(&handle.value) == entry.second.integer, "mutation sequence changed integer");
            else if (entry.second.type == IDYDB_BOOL) check(idydb_retrieve_bool(&handle.value) == entry.second.boolean, "mutation sequence changed bool");
            else check(idydb_retrieve_char(&handle.value) == entry.second.text, "mutation sequence changed text");
        }
        if (iteration % 50 == 49) {
            handle.close();
            handle.open(path, IDYDB_READONLY);
            for (const auto& entry : expected)
                check(idydb_extract(&handle.value, entry.first.first, entry.first.second) == IDYDB_DONE, "readonly reopen lost cell");
            handle.close();
            handle.open(path, 0);
        }
    }
    for (const auto& entry : expected)
        check(idydb_delete(&handle.value, entry.first.first, entry.first.second) == IDYDB_DONE, "delete final cell");
    handle.close();
    check(std::filesystem::file_size(path) == 0, "delete all cells did not empty file");
}

void append_short(std::vector<unsigned char>& bytes, unsigned short value) {
    const auto* pointer = reinterpret_cast<const unsigned char*>(&value);
    bytes.insert(bytes.end(), pointer, pointer + sizeof(value));
}

std::vector<unsigned char> header(unsigned char type, unsigned short count = 0, unsigned short row = 0) {
    std::vector<unsigned char> bytes;
    append_short(bytes, 0);
    append_short(bytes, count);
    append_short(bytes, row);
    bytes.push_back(type);
    return bytes;
}

void write_bytes(const std::filesystem::path& file, const std::vector<unsigned char>& bytes) {
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output.exceptions(std::ios::badbit | std::ios::failbit);
    output.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

void malformed_records(const TestFiles& files) {
    std::vector<std::vector<unsigned char>> cases;
    cases.push_back(std::vector<unsigned char>(6, 0));
    cases.push_back(header(1)); // Missing integer payload.
    cases.push_back(header(99));
    auto bytes = header(6); append_short(bytes, 0); cases.push_back(bytes);
    bytes = header(6); append_short(bytes, 16383); cases.push_back(bytes);
    bytes = header(3); append_short(bytes, 65535); bytes.push_back(0); cases.push_back(bytes);
    bytes = header(3); append_short(bytes, 1); bytes.push_back('a'); bytes.push_back('b'); cases.push_back(bytes);
    bytes = header(3); append_short(bytes, 2); bytes.push_back(0); bytes.push_back('b'); bytes.push_back(0); cases.push_back(bytes);
    bytes = header(4); bytes.push_back(0); cases.push_back(bytes); // Truncated next partition.
    cases.push_back(header(4, 1)); // Declares a missing second row.
    bytes = header(4, 1, 1); append_short(bytes, 0); bytes.push_back(4); cases.push_back(bytes);
    bytes = header(4, 1, 0); append_short(bytes, 0); bytes.push_back(4); cases.push_back(bytes);
    for (size_t index = 0; index < cases.size(); ++index) {
        const auto path = files.directory / ("corrupt-" + std::to_string(index) + ".db");
        write_bytes(path, cases[index]);
        for (const int flags : {0, IDYDB_READONLY}) {
            Handle handle;
            const int rc = idydb_open(path.string().c_str(), &handle.value, flags);
            check(rc == IDYDB_CORRUPT, "malformed file accepted: case " + std::to_string(index));
        }
        check(std::filesystem::file_size(path) == cases[index].size(), "malformed open modified file");
    }
}

void full_column(const TestFiles& files) {
    auto bytes = header(4, static_cast<unsigned short>(max_position - 1), 0);
    for (unsigned row = 1; row < max_position; ++row) {
        append_short(bytes, static_cast<unsigned short>(row));
        bytes.push_back(4);
    }
    const auto path = files.file("full-column.db");
    write_bytes(path, bytes);
    Handle handle;
    handle.open(path, IDYDB_READONLY);
    check(idydb_extract(&handle.value, 1, max_position) == IDYDB_DONE && idydb_retrieve_bool(&handle.value), "read last of full column");
    handle.close();
    handle.open(path, 0);
    check(idydb_insert_bool(&handle.value, 1, max_position, false) == IDYDB_DONE, "update full column");
    check(idydb_delete(&handle.value, 1, max_position) == IDYDB_DONE, "delete from full column");
    check(idydb_insert_bool(&handle.value, 1, max_position, false) == IDYDB_DONE, "restore full column");
    check(idydb_extract(&handle.value, 1, max_position) == IDYDB_DONE && !idydb_retrieve_bool(&handle.value), "verify restored last row");
    handle.close();
    handle.open(path, IDYDB_READONLY);
    check(idydb_extract(&handle.value, 1, max_position - 1) == IDYDB_DONE && idydb_retrieve_bool(&handle.value), "full-column count survived reopen");
}

void sparse_unsafe_columns(const TestFiles& files) {
#if IDYDB_SIZING_MODE == IDYDB_SIZING_MODE_BIG
    Handle handle;
    handle.open(files.file("unsafe.db"), IDYDB_CREATE | IDYDB_UNSAFE);
    check(idydb_insert_int(&handle.value, 70000, 1, 5) == IDYDB_RANGE, "reject unrepresentable initial gap");
    check(idydb_insert_int(&handle.value, 65536, 1, 10) == IDYDB_DONE, "create sparse initial column");
    check(idydb_insert_int(&handle.value, 131072, 1, 20) == IDYDB_DONE, "extend unsafe column range");
    require_int(handle, 131072, 1, 20);
    check(idydb_delete(&handle.value, 65536, 1) == IDYDB_RANGE, "reject unrepresentable merged gap");
    require_int(handle, 65536, 1, 10);
    check(idydb_delete(&handle.value, 65536, 2) == IDYDB_DONE, "missing unsafe row deletion is a noop");
    check(idydb_insert_int(&handle.value, 1, 1, 30) == IDYDB_DONE, "bridge initial sparse gap");
    check(idydb_insert_int(&handle.value, 65536, 2, 40) == IDYDB_DONE, "keep unsafe column after deletion");
    check(idydb_delete(&handle.value, 65536, 1) == IDYDB_DONE, "delete row without removing sparse column");
    require_int(handle, 65536, 2, 40);
#else
    (void)files;
#endif
}

void sizing_capacity(const TestFiles& files) {
#if IDYDB_SIZING_MODE == IDYDB_SIZING_MODE_TINY
    // Fill every supported cell with its largest payload. This previously
    // exceeded both the uint16 file offset and the max-size calculation.
    std::vector<unsigned char> bytes;
    bytes.reserve(16777792);
    for (unsigned column = 0; column < max_position; ++column) {
        append_short(bytes, 0);
        append_short(bytes, static_cast<unsigned short>(max_position - 1));
        for (unsigned row = 0; row < max_position; ++row) {
            append_short(bytes, static_cast<unsigned short>(row));
            bytes.push_back(3);
            append_short(bytes, 65532);
            bytes.insert(bytes.end(), 65532, 'z');
            bytes.push_back(0);
        }
    }
    check(bytes.size() == 16777792, "tiny capacity fixture size");
    const auto path = files.file("tiny-capacity.db");
    write_bytes(path, bytes);
    Handle handle;
    handle.open(path, IDYDB_READONLY);
    require_text(handle, max_position, max_position, std::string(65532, 'z'));
#elif IDYDB_SIZING_MODE == IDYDB_SIZING_MODE_SMALL
    // A sparse malformed file checks the 64-bit capacity boundary without
    // allocating four gigabytes. Size validation must allow the boundary,
    // then the record decoder must report the invalid first record.
    const auto path = files.file("small-capacity.db");
    { std::ofstream create(path, std::ios::binary); check(create.good(), "create sparse capacity fixture"); }
    std::filesystem::resize_file(path, UINT64_C(4295099392));
    Handle handle;
    check(idydb_open(path.string().c_str(), &handle.value, IDYDB_READONLY) == IDYDB_CORRUPT,
          "small mode truncated file size or underestimated capacity");
#else
    (void)files;
#endif
}
} // namespace

int main() {
    try {
        TestFiles files;
        null_arguments();
        payload_limits_and_aliases(files);
        mutation_sequences(files);
        malformed_records(files);
        full_column(files);
        sparse_unsafe_columns(files);
        sizing_capacity(files);
        std::cout << "Core regression tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Core regression test failed: " << error.what() << '\n';
        return 1;
    }
}
