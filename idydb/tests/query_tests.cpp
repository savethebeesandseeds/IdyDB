#include "db.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <string>
#include <vector>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#endif

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); \
    std::abort(); \
} } while (0)

struct TemporaryFile {
    std::string path;
    TemporaryFile() {
        static unsigned serial = 0;
        path = "idydb-query-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + "-" + std::to_string(serial++) + ".db";
#if defined(_WIN32)
        // The legacy Windows CRT lacks C11's fopen "x" mode. Reserve the
        // name atomically before adopting its binary descriptor as a stream.
        const int descriptor = _open(path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL |
            _O_BINARY | _O_NOINHERIT, _S_IREAD | _S_IWRITE);
        FILE* file = descriptor < 0 ? nullptr : _fdopen(descriptor, "wb");
        if (!file && descriptor >= 0) _close(descriptor);
#else
        FILE* file = std::fopen(path.c_str(), "wbx");
#endif
        CHECK(file);
        CHECK(std::fclose(file) == 0);
    }
    ~TemporaryFile() { std::remove(path.c_str()); }
    void write(const std::vector<unsigned char>& bytes) {
        FILE* file = std::fopen(path.c_str(), "wb");
        CHECK(file);
        CHECK(std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size());
        CHECK(std::fclose(file) == 0);
    }
};

template<typename T>
static void append(std::vector<unsigned char>& bytes, T value)
{
    const unsigned char* raw = reinterpret_cast<const unsigned char*>(&value);
    bytes.insert(bytes.end(), raw, raw + sizeof(value));
}

static std::vector<unsigned char> header(unsigned short count, unsigned short row, unsigned char type)
{
    std::vector<unsigned char> bytes;
    append<unsigned short>(bytes, 0); // column 1
    append<unsigned short>(bytes, count);
    append<unsigned short>(bytes, row);
    append<unsigned char>(bytes, type);
    return bytes;
}

static void test_invalid_arguments()
{
    const float query[] = {1, 0};
    idydb_knn_result result[] = {{7, 12}};
    idydb* absent = NULL;
    CHECK(idydb_knn_search_vector_column(NULL, 1, query, 2, 1, IDYDB_SIM_COSINE, result) == -1);
    CHECK(result[0].row == 0);
    CHECK(idydb_knn_search_vector_column(&absent, 1, query, 2, 1, IDYDB_SIM_COSINE, result) == -1);
    CHECK(idydb_column_next_row(NULL, 1) == 0);
    idydb_set_embedder(NULL, NULL, NULL);
    char* context = reinterpret_cast<char*>(1);
    CHECK(idydb_rag_query_context(NULL, 1, 2, query, 2, 1, IDYDB_SIM_COSINE, 0, &context) == IDYDB_ERROR);
    CHECK(context == NULL);
    char* texts[] = {reinterpret_cast<char*>(1)};
    CHECK(idydb_rag_query_topk(NULL, 1, 2, query, 2, 1, IDYDB_SIM_COSINE, result, texts) == -1);
    CHECK(texts[0] == NULL);
    CHECK(idydb_rag_upsert_text_auto_embed(NULL, 1, 2, 1, "text") == IDYDB_ERROR);
}

static void test_queries(int flags)
{
    TemporaryFile file;
    idydb* database = NULL;
    CHECK(idydb_open(file.path.c_str(), &database, IDYDB_CREATE) == IDYDB_SUCCESS);
    const float first[] = {1, 0}, second[] = {0.8f, 0.6f}, third[] = {0, 1};
    CHECK(idydb_insert_vector(&database, 2, 1, first, 2) == IDYDB_DONE); // highest result has no text
    CHECK(idydb_rag_upsert_text(&database, 1, 2, 2, "beta", second, 2) == IDYDB_DONE);
    CHECK(idydb_rag_upsert_text(&database, 1, 2, 3, "gamma", third, 2) == IDYDB_DONE);
    CHECK(idydb_insert_int(&database, 3, 2, 42) == IDYDB_DONE);
    CHECK(idydb_insert_const_char(&database, 4, 2, "group") == IDYDB_DONE);
    CHECK(idydb_close(&database) == IDYDB_DONE);
    CHECK(idydb_open(file.path.c_str(), &database, flags) == IDYDB_SUCCESS);

    idydb_knn_result results[4];
    CHECK(idydb_knn_search_vector_column(&database, 2, first, 2, 4, IDYDB_SIM_COSINE, results) == 3);
    CHECK(results[0].row == 1 && results[1].row == 2 && results[2].row == 3 && results[3].row == 0);
    CHECK(std::fabs(results[0].score - 1) < 0.00001f);
    CHECK(std::fabs(results[1].score - 0.8f) < 0.00001f);
    CHECK(idydb_knn_search_vector_column(&database, 2, first, 2, 2, IDYDB_SIM_L2, results) == 2);
    CHECK(results[0].row == 1 && results[1].row == 2);
    CHECK(idydb_column_next_row(&database, 1) == 4);
    CHECK(idydb_column_next_row(&database, 2) == 4);

    idydb_filter_term term = {};
    term.column = 3; term.type = IDYDB_NULL; term.op = IDYDB_FILTER_OP_EQ;
    idydb_filter filter = {&term, 1};
    CHECK(idydb_knn_search_vector_column_filtered(&database, 2, first, 2, 4, IDYDB_SIM_COSINE, &filter, results) == 2);
    CHECK(results[0].row == 1 && results[1].row == 3);
    term.op = IDYDB_FILTER_OP_NEQ;
    CHECK(idydb_knn_search_vector_column_filtered(&database, 2, first, 2, 4, IDYDB_SIM_COSINE, &filter, results) == 1);
    CHECK(results[0].row == 2);
    term.type = IDYDB_INTEGER; term.op = IDYDB_FILTER_OP_GTE; term.value.i = 42;
    CHECK(idydb_knn_search_vector_column_filtered(&database, 2, first, 2, 4, IDYDB_SIM_COSINE, &filter, results) == 1);
    CHECK(results[0].row == 2);
    term.column = 4; term.type = IDYDB_CHAR; term.op = IDYDB_FILTER_OP_EQ; term.value.s = "group";
    CHECK(idydb_knn_search_vector_column_filtered(&database, 2, first, 2, 4, IDYDB_SIM_COSINE, &filter, results) == 1);
    CHECK(results[0].row == 2);
    idydb_filter malformed = {NULL, 1};
    CHECK(idydb_knn_search_vector_column_filtered(&database, 2, first, 2, 4, IDYDB_SIM_COSINE, &malformed, results) == -1);
    CHECK(results[0].row == 0);
    CHECK(idydb_knn_search_vector_column(&database, 2, first, 2, 4, static_cast<idydb_similarity_metric>(99), results) == -1);
    const float invalid[] = {std::numeric_limits<float>::quiet_NaN(), 0};
    CHECK(idydb_knn_search_vector_column(&database, 2, invalid, 2, 4, IDYDB_SIM_COSINE, results) == -1);

    char* context = NULL;
    CHECK(idydb_rag_query_context(&database, 1, 2, first, 2, 4, IDYDB_SIM_COSINE, 0, &context) == IDYDB_DONE);
    CHECK(context && std::strcmp(context, "beta\n---\ngamma") == 0);
    idydb_free(context);
    CHECK(idydb_rag_query_context(&database, 1, 2, first, 2, 4, IDYDB_SIM_COSINE, 7, &context) == IDYDB_DONE);
    CHECK(context && std::strlen(context) == 7 && std::strncmp(context, "beta\n--", 7) == 0);
    idydb_free(context);
    CHECK(idydb_rag_query_context(&database, 1, 5, first, 2, 4, IDYDB_SIM_COSINE, 0, &context) == IDYDB_DONE);
    CHECK(context && context[0] == '\0');
    idydb_free(context);

    const idydb_column_row_sizing columns[] = {3, 4, 2};
    idydb_value metadata[12];
    char* texts[4];
    CHECK(idydb_rag_query_topk_with_metadata(&database, 1, 2, first, 2, 4, IDYDB_SIM_COSINE, NULL,
        columns, 3, results, texts, metadata) == 3);
    CHECK(texts[0] == NULL && texts[1] && std::strcmp(texts[1], "beta") == 0);
    CHECK(metadata[0].type == IDYDB_NULL && metadata[1].type == IDYDB_NULL);
    CHECK(metadata[3].type == IDYDB_INTEGER && metadata[3].as.i == 42);
    CHECK(metadata[4].type == IDYDB_CHAR && std::strcmp(metadata[4].as.s, "group") == 0);
    CHECK(metadata[5].type == IDYDB_VECTOR && metadata[5].as.vec.dims == 2);
    CHECK(std::fabs(metadata[5].as.vec.v[0] - second[0]) < 0.00001f);
    CHECK(metadata[9].type == IDYDB_NULL);
    for (unsigned i = 0; i < 4; ++i) idydb_free(texts[i]);
    idydb_values_free(metadata, 12);
    idydb_values_free(metadata, 12); // cleanup is safe to repeat
    CHECK(idydb_close(&database) == IDYDB_DONE);
}

static void test_upsert_preflight_and_large_vectors()
{
    TemporaryFile file;
    idydb* database = NULL;
    CHECK(idydb_open(file.path.c_str(), &database, IDYDB_CREATE) == IDYDB_SUCCESS);
    const float invalid[] = {std::numeric_limits<float>::infinity(), 0};
    CHECK(idydb_rag_upsert_text(&database, 1, 2, 1, "must not persist", invalid, 2) != IDYDB_DONE);
    CHECK(idydb_extract(&database, 1, 1) == IDYDB_NULL);
    const float valid[] = {1, 0};
    CHECK(idydb_rag_upsert_text(&database, 1, 1, 1, "must not persist", valid, 2) != IDYDB_DONE);
    CHECK(idydb_extract(&database, 1, 1) == IDYDB_NULL);
    CHECK(idydb_rag_upsert_text(&database, 1, 2, 1, "must not persist", valid, 16384) != IDYDB_DONE);
    CHECK(idydb_extract(&database, 1, 1) == IDYDB_NULL);
    const float maximum = std::numeric_limits<float>::max();
    const float positive[] = {maximum, maximum}, negative[] = {-maximum, -maximum};
    CHECK(idydb_insert_vector(&database, 2, 1, positive, 2) == IDYDB_DONE);
    CHECK(idydb_insert_vector(&database, 2, 2, negative, 2) == IDYDB_DONE);
    idydb_knn_result results[2];
    CHECK(idydb_knn_search_vector_column(&database, 2, positive, 2, 2, IDYDB_SIM_COSINE, results) == 2);
    CHECK(results[0].row == 1 && results[1].row == 2);
    CHECK(std::fabs(results[0].score - 1) < 0.00001f && std::fabs(results[1].score + 1) < 0.00001f);
    CHECK(idydb_knn_search_vector_column(&database, 2, positive, 2, 2, IDYDB_SIM_L2, results) == 2);
    CHECK(results[0].row == 1 && results[1].row == 2 && std::isfinite(results[1].score));
    CHECK(idydb_close(&database) == IDYDB_DONE);
}

static void test_boundary_ties()
{
    TemporaryFile file;
    idydb* database = NULL;
    CHECK(idydb_open(file.path.c_str(), &database, IDYDB_CREATE) == IDYDB_SUCCESS);
    const float low[] = {0, 1}, high[] = {1, 0};
    CHECK(idydb_insert_vector(&database, 2, 1, low, 2) == IDYDB_DONE);
    CHECK(idydb_insert_vector(&database, 2, 2, low, 2) == IDYDB_DONE);
    CHECK(idydb_insert_vector(&database, 2, 3, high, 2) == IDYDB_DONE);
    idydb_knn_result results[2];
    CHECK(idydb_knn_search_vector_column(&database, 2, high, 2, 2, IDYDB_SIM_COSINE, results) == 2);
    CHECK(results[0].row == 3 && results[1].row == 1);
    CHECK(results[0].score == 1 && results[1].score == 0);
    CHECK(idydb_close(&database) == IDYDB_DONE);
}

static void reject_corrupt(const std::vector<unsigned char>& bytes, int flags)
{
    TemporaryFile file;
    file.write(bytes);
    idydb* database = NULL;
    const int opened = idydb_open(file.path.c_str(), &database, flags);
    if (opened != IDYDB_SUCCESS) { if (database) idydb_close(&database); return; }
    const float query[] = {1, 0};
    idydb_knn_result results[2] = {{7, 12}, {8, 11}};
    CHECK(idydb_knn_search_vector_column(&database, 1, query, 2, 2, IDYDB_SIM_COSINE, results) == -1);
    CHECK(results[0].row == 0 && results[1].row == 0);
    CHECK(idydb_column_next_row(&database, 1) == 0);
    CHECK(idydb_close(&database) == IDYDB_DONE);
}

static void test_corrupt_scans(int flags)
{
    reject_corrupt(std::vector<unsigned char>(1, 0), flags); // partial partition header
    std::vector<unsigned char> bytes = header(0, 0, 1);
    append<unsigned char>(bytes, 42); // truncated INT payload
    reject_corrupt(bytes, flags);
    bytes = header(0, 0, 6);
    append<unsigned short>(bytes, 2);
    append<float>(bytes, 1); // second vector component missing
    reject_corrupt(bytes, flags);
    bytes = header(0, 0, 6);
    append<unsigned short>(bytes, 16384); // length previously overflowed the ushort advance
    reject_corrupt(bytes, flags);
    bytes = header(0, 0, 3);
    append<unsigned short>(bytes, 65535);
    reject_corrupt(bytes, flags);
    bytes = header(0, 0, 3);
    append<unsigned short>(bytes, 1);
    append<unsigned char>(bytes, 'x'); append<unsigned char>(bytes, 'y'); // missing string terminator
    reject_corrupt(bytes, flags);
    reject_corrupt(header(65535, 0, 4), flags); // missing remaining 65535 rows
    reject_corrupt(header(0, 0, 99), flags); // unknown tag
}

static void test_full_column()
{
#if IDYDB_SIZING_MODE == IDYDB_SIZING_MODE_BIG
    TemporaryFile file;
    std::vector<unsigned char> bytes = header(65535, 0, 4);
    for (unsigned row = 1; row < 65536; ++row) {
        append<unsigned short>(bytes, static_cast<unsigned short>(row));
        append<unsigned char>(bytes, 4);
    }
    file.write(bytes);
    idydb* database = NULL;
    CHECK(idydb_open(file.path.c_str(), &database, IDYDB_READONLY) == IDYDB_SUCCESS);
    CHECK(idydb_column_next_row(&database, 1) == 0);
    const float query[] = {1, 0};
    idydb_knn_result result;
    CHECK(idydb_knn_search_vector_column(&database, 2, query, 2, 1, IDYDB_SIM_COSINE, &result) == 0);
    CHECK(idydb_close(&database) == IDYDB_DONE);
#endif
}

int main()
{
    test_invalid_arguments();
    test_queries(0);
    test_queries(IDYDB_READONLY); // exercises mmap when enabled
    test_upsert_preflight_and_large_vectors();
    test_boundary_ties();
    test_corrupt_scans(0);
    test_corrupt_scans(IDYDB_READONLY);
    test_full_column();
    std::puts("Query, filter, metadata, RAG, and corrupt scan regressions passed.");
    return 0;
}
