/* db.h */
/* based on the original work of: https://github.com/bradley499/flitdb/tree/main */
/* also used in project idyicyanere */
#ifndef idydb_h
#define idydb_h

/* ---------------- Symbol visibility / DLL support ----------------
 * - Static builds: nothing special.
 * - Windows DLL:
 *     - Define IDYDB_BUILD_DLL when building the library
 *     - Define IDYDB_USE_DLL   when consuming the DLL
 */
#if defined(_WIN32) || defined(__CYGWIN__)
  #if defined(IDYDB_BUILD_DLL)
    #define IDYDB_API __declspec(dllexport)
  #elif defined(IDYDB_USE_DLL)
    #define IDYDB_API __declspec(dllimport)
  #else
    #define IDYDB_API
  #endif
#else
  #define IDYDB_API
#endif

#define CUWACUNU_CAMAHJUCUNU_DB_VERBOSE_DEBUG 1 // enable debug mode

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IDYDB_SUCCESS       0  // Successful operation
#define IDYDB_ERROR         1  // Unsuccessful operation
#define IDYDB_PERM          2  // Permission denied
#define IDYDB_BUSY          3  // The database file is locked
#define IDYDB_NOT_FOUND     4  // The database file is not found
#define IDYDB_CORRUPT       5  // The database file is malformed
#define IDYDB_RANGE         6  // The requested range is outside the range of the database
#define IDYDB_CREATE        7  // Create a database if not existent
#define IDYDB_READONLY      8  // Only allow the reading of the database
#define IDYDB_DONE          9  // The operation was completed successfully
#define IDYDB_NULL          10 // The operation resulted in a null lookup
#define IDYDB_INTEGER       11 // The value type of int
#define IDYDB_FLOAT         12 // The value type of float
#define IDYDB_CHAR          13 // The value type of char
#define IDYDB_BOOL          14 // The value type of bool
#define IDYDB_VECTOR        15 // The value type of vector<float>
#define IDYDB_UNSAFE        16 // Discard safety protocols to allow for larger database
#define IDYDB_VERSION  0x117ee // The current IdyDB version magic number

// Database sizing options
#define IDYDB_SIZING_MODE_TINY  1 // Handle databases up to 14.74 megabytes in size
#define IDYDB_SIZING_MODE_SMALL 2 // Handle databases up to 4.26 gigabytes in size
#define IDYDB_SIZING_MODE_BIG   3 // Handle databases up to 281.47 terabytes in size

// Database memory mapping
#define IDYDB_MMAP_ALLOWED 1 // Allows the database to memory map files - if possible (1 - allowed, 0 - disallowed)

// Database sizing selection
#define IDYDB_SIZING_MODE IDYDB_SIZING_MODE_BIG // The sizing mode for this compilation

#ifdef __cplusplus
#define idydb_extern extern "C" IDYDB_API
#else
#define idydb_extern extern IDYDB_API
#endif

typedef struct idydb idydb;

#if IDYDB_SIZING_MODE == IDYDB_SIZING_MODE_BIG
typedef unsigned long long idydb_column_row_sizing;
#else
typedef unsigned short idydb_column_row_sizing;
#endif

/* ---------------- Runtime open options (encryption is runtime-controlled) ---------------- */

typedef struct idydb_open_options {
    int flags;                  /* existing flags: IDYDB_CREATE, IDYDB_READONLY, IDYDB_UNSAFE */
    bool encrypted_at_rest;     /* true => encrypted backing file using OpenSSL AES-256-GCM */
    const char* passphrase;     /* required if encrypted_at_rest == true */
    unsigned int pbkdf2_iter;   /* 0 => default (recommended); otherwise custom */
} idydb_open_options;

/**
 * @brief Unified runtime open entrypoint
 */
idydb_extern int idydb_open_with_options(const char *filename, idydb **handler, const idydb_open_options* options);

/**
 * @brief Configures the IdyDB handler to point to and operate on a specific file (PLAINTEXT)
 */
idydb_extern int idydb_open(const char *filename, idydb **handler, int flags);

/**
 * @brief Open an encrypted-at-rest database (AES-256-GCM + PBKDF2-HMAC-SHA256 via OpenSSL).
 */
idydb_extern int idydb_open_encrypted(const char *filename, idydb **handler, int flags, const char* passphrase);

/**
 * @brief Close the connection to the database, and delete the IdyDB handler object
 */
idydb_extern int idydb_close(idydb **handler);

/**
 * @brief Return the current version of the IdyDB API
 */
idydb_extern unsigned int idydb_version_check();

/**
 * @brief Retrieve an error message from the IdyDB handler if once exists
 */
idydb_extern char* idydb_errmsg(idydb **handler);

/**
 * @brief Extract a value stored within the IdyDB handler
 */
idydb_extern int idydb_extract(idydb **handler, idydb_column_row_sizing column_position, idydb_column_row_sizing row_position);

/**
 * @brief Retrieve a numeric representation of what data type was retrieved:
 *        IDYDB_NULL, IDYDB_INTEGER, IDYDB_FLOAT, IDYDB_CHAR, IDYDB_BOOL, IDYDB_VECTOR
 */
idydb_extern int idydb_retrieved_type(idydb **handler);

idydb_extern int idydb_insert_int(idydb **handler, idydb_column_row_sizing column_position, idydb_column_row_sizing row_position, int value);
idydb_extern int idydb_insert_float(idydb **handler, idydb_column_row_sizing column_position, idydb_column_row_sizing row_position, float value);
idydb_extern int idydb_insert_char(idydb **handler, idydb_column_row_sizing column_position, idydb_column_row_sizing row_position, char* value);
idydb_extern int idydb_insert_const_char(idydb **handler, idydb_column_row_sizing column_position, idydb_column_row_sizing row_position, const char* value);
idydb_extern int idydb_insert_bool(idydb **handler, idydb_column_row_sizing column_position, idydb_column_row_sizing row_position, bool value);

/**
 * @brief Insert a vector<float> (embedding)
 *
 * Layout on disk: [type=6][uint16 dims][dims * float32 bytes]
 * @param dims must be <= 16383
 */
idydb_extern int idydb_insert_vector(idydb **handler, idydb_column_row_sizing column_position, idydb_column_row_sizing row_position, const float* data, unsigned short dims);

/**
 * @brief Delete a value stored within the IdyDB handler
 */
idydb_extern int idydb_delete(idydb **handler, idydb_column_row_sizing column_position, idydb_column_row_sizing row_position);

/**
 * @brief Retrieve primitives from the last extracted value
 */
idydb_extern int   idydb_retrieve_int(idydb **handler);
idydb_extern float idydb_retrieve_float(idydb **handler);
idydb_extern char* idydb_retrieve_char(idydb **handler);
idydb_extern bool  idydb_retrieve_bool(idydb **handler);

/**
 * @brief Retrieve a vector from the last extracted value.
 * Returns an internal pointer valid until the next extract/insert/clear; do not free it.
 * On success, *out_dims is set to the vector dimensionality.
 */
idydb_extern const float* idydb_retrieve_vector(idydb **handler, unsigned short* out_dims);

/* --------------------------- Vector DB + RAG extensions --------------------------- */

typedef enum { IDYDB_SIM_COSINE = 1, IDYDB_SIM_L2 = 2 } idydb_similarity_metric;

typedef struct {
    idydb_column_row_sizing row; // 1-based row index
    float score;                 // higher is better
} idydb_knn_result;

/* --------------------------- Query Filters + Metadata --------------------------- */

typedef enum {
    IDYDB_FILTER_OP_EQ         = 1,
    IDYDB_FILTER_OP_NEQ        = 2,
    IDYDB_FILTER_OP_GT         = 3,
    IDYDB_FILTER_OP_GTE        = 4,
    IDYDB_FILTER_OP_LT         = 5,
    IDYDB_FILTER_OP_LTE        = 6,
    IDYDB_FILTER_OP_IS_NULL    = 7,
    IDYDB_FILTER_OP_IS_NOT_NULL= 8
} idydb_filter_op;

typedef union {
    int         i;
    float       f;
    bool        b;
    const char* s;  /* not owned */
} idydb_filter_value;

typedef struct {
    idydb_column_row_sizing column;
    unsigned char           type;  /* IDYDB_INTEGER/FLOAT/CHAR/BOOL/NULL */
    idydb_filter_op         op;
    idydb_filter_value      value;
} idydb_filter_term;

typedef struct {
    const idydb_filter_term* terms; /* not owned */
    size_t                   nterms;
} idydb_filter;

/* Returned metadata values (deep-copies for CHAR/VECTOR). */
typedef struct {
    unsigned char type; /* IDYDB_NULL/INTEGER/FLOAT/CHAR/BOOL/VECTOR */
    union {
        int   i;
        float f;
        bool  b;
        char* s; /* malloc'd if type==IDYDB_CHAR */
        struct { float* v; unsigned short dims; } vec; /* malloc'd if type==IDYDB_VECTOR */
    } as;
} idydb_value;

/* Convenience free helpers for heap allocations returned by IdyDB APIs. */
idydb_extern void idydb_free(void* p);
idydb_extern void idydb_value_free(idydb_value* v);
idydb_extern void idydb_values_free(idydb_value* values, size_t count);

idydb_extern int idydb_knn_search_vector_column(idydb **handler,
                                                idydb_column_row_sizing vector_column,
                                                const float* query,
                                                unsigned short dims,
                                                unsigned short k,
                                                idydb_similarity_metric metric,
                                                idydb_knn_result* out_results);

/* Filtered kNN: only rows passing `filter` are considered. */
idydb_extern int idydb_knn_search_vector_column_filtered(idydb **handler,
                                                        idydb_column_row_sizing vector_column,
                                                        const float* query,
                                                        unsigned short dims,
                                                        unsigned short k,
                                                        idydb_similarity_metric metric,
                                                        const idydb_filter* filter,
                                                        idydb_knn_result* out_results);

 
idydb_extern idydb_column_row_sizing idydb_column_next_row(idydb **handler, idydb_column_row_sizing column);

idydb_extern int idydb_rag_upsert_text(idydb **handler,
                                       idydb_column_row_sizing text_column,
                                       idydb_column_row_sizing vector_column,
                                       idydb_column_row_sizing row,
                                       const char* text,
                                       const float* embedding,
                                       unsigned short dims);

typedef int (*idydb_embed_fn)(const char* text, float** out_vector, unsigned short* out_dims, void* user);

idydb_extern void idydb_set_embedder(idydb **handler, idydb_embed_fn fn, void* user);

idydb_extern int idydb_rag_upsert_text_auto_embed(idydb **handler,
                                                  idydb_column_row_sizing text_column,
                                                  idydb_column_row_sizing vector_column,
                                                  idydb_column_row_sizing row,
                                                  const char* text);

idydb_extern int idydb_rag_query_topk(idydb **handler,
                                      idydb_column_row_sizing text_column,
                                      idydb_column_row_sizing vector_column,
                                      const float* query_embedding,
                                      unsigned short dims,
                                      unsigned short k,
                                      idydb_similarity_metric metric,
                                      idydb_knn_result* out_results,
                                      char** out_texts);

idydb_extern int idydb_rag_query_topk_filtered(idydb **handler,
                                              idydb_column_row_sizing text_column,
                                              idydb_column_row_sizing vector_column,
                                              const float* query_embedding,
                                              unsigned short dims,
                                              unsigned short k,
                                              idydb_similarity_metric metric,
                                              const idydb_filter* filter,
                                              idydb_knn_result* out_results,
                                              char** out_texts);

/* TopK with structured metadata:
 * out_meta is a flat array of size (k * meta_columns_count).
 * Indexing: out_meta[i*meta_columns_count + j] corresponds to result i, meta column j.
 * Caller frees CHAR/VECTOR in out_meta via idydb_values_free(out_meta, k*meta_columns_count).
 */
idydb_extern int idydb_rag_query_topk_with_metadata(idydb **handler,
                                                   idydb_column_row_sizing text_column,
                                                   idydb_column_row_sizing vector_column,
                                                   const float* query_embedding,
                                                   unsigned short dims,
                                                   unsigned short k,
                                                   idydb_similarity_metric metric,
                                                   const idydb_filter* filter,
                                                   const idydb_column_row_sizing* meta_columns,
                                                   size_t meta_columns_count,
                                                   idydb_knn_result* out_results,
                                                   char** out_texts,
                                                   idydb_value* out_meta);

idydb_extern int idydb_rag_query_context(idydb **handler,
                                         idydb_column_row_sizing text_column,
                                         idydb_column_row_sizing vector_column,
                                         const float* query_embedding,
                                         unsigned short dims,
                                         unsigned short k,
                                         idydb_similarity_metric metric,
                                         size_t max_chars,
                                         char** out_context);

idydb_extern int idydb_rag_query_context_filtered(idydb **handler,
                                                 idydb_column_row_sizing text_column,
                                                 idydb_column_row_sizing vector_column,
                                                 const float* query_embedding,
                                                 unsigned short dims,
                                                 unsigned short k,
                                                 idydb_similarity_metric metric,
                                                 const idydb_filter* filter,
                                                 size_t max_chars,
                                                 char** out_context);

#undef idydb_extern

#ifdef __cplusplus
namespace db {

using ::idydb;
using ::idydb_column_row_sizing;
using ::idydb_open_options;

using ::idydb_open_with_options;
using ::idydb_open;
using ::idydb_open_encrypted;
using ::idydb_close;
using ::idydb_version_check;
using ::idydb_errmsg;
using ::idydb_extract;
using ::idydb_retrieved_type;

using ::idydb_insert_int;
using ::idydb_insert_float;
using ::idydb_insert_char;
using ::idydb_insert_const_char;
using ::idydb_insert_bool;
using ::idydb_insert_vector;

using ::idydb_delete;

using ::idydb_retrieve_int;
using ::idydb_retrieve_float;
using ::idydb_retrieve_char;
using ::idydb_retrieve_bool;
using ::idydb_retrieve_vector;

using ::idydb_similarity_metric;
using ::idydb_knn_result;
using ::idydb_knn_search_vector_column;
using ::idydb_knn_search_vector_column_filtered;
using ::idydb_column_next_row;

using ::idydb_rag_upsert_text;
using ::idydb_embed_fn;
using ::idydb_set_embedder;
using ::idydb_rag_upsert_text_auto_embed;
using ::idydb_rag_query_topk;
using ::idydb_rag_query_topk_filtered;
using ::idydb_rag_query_topk_with_metadata;
using ::idydb_rag_query_context;
using ::idydb_rag_query_context_filtered;

using ::idydb_filter_op;
using ::idydb_filter_value;
using ::idydb_filter_term;
using ::idydb_filter;
using ::idydb_value;
using ::idydb_free;
using ::idydb_value_free;
using ::idydb_values_free;

// C++ overload conveniences (header-only)
static inline int idydb_insert(idydb **handler, idydb_column_row_sizing c, idydb_column_row_sizing r, int v)
{ return ::idydb_insert_int(handler, c, r, v); }

static inline int idydb_insert(idydb **handler, idydb_column_row_sizing c, idydb_column_row_sizing r, float v)
{ return ::idydb_insert_float(handler, c, r, v); }

static inline int idydb_insert(idydb **handler, idydb_column_row_sizing c, idydb_column_row_sizing r, char* v)
{ return ::idydb_insert_char(handler, c, r, v); }

static inline int idydb_insert(idydb **handler, idydb_column_row_sizing c, idydb_column_row_sizing r, const char* v)
{ return ::idydb_insert_const_char(handler, c, r, v); }

static inline int idydb_insert(idydb **handler, idydb_column_row_sizing c, idydb_column_row_sizing r, bool v)
{ return ::idydb_insert_bool(handler, c, r, v); }

} // namespace db
#endif

#endif /* idydb_h */
