/* db.cpp */
#ifndef idydb_c
#define idydb_c

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/* ---------------- platform + libc ---------------- */
#include <errno.h>
#include <stdarg.h>
#include <ctype.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>
#include <limits.h>
#include <limits>

static_assert(CHAR_BIT == 8 && sizeof(short) == 2 && sizeof(int) == 4 && sizeof(float) == 4,
              "IdyDB requires 8-bit bytes, 16-bit shorts, and 32-bit ints/floats");
static_assert(std::numeric_limits<float>::is_iec559, "IdyDB requires IEEE-754 floats");

#include "platform.h"
#define fileno idydb_platform::descriptor
#define fseek idydb_platform::seek
#define ftell idydb_platform::tell
#define fsync idydb_platform::sync
#define flock idydb_platform::lock
#define LOCK_SH idydb_platform::shared
#define LOCK_EX idydb_platform::exclusive
#define LOCK_NB idydb_platform::nonblocking
#define LOCK_UN idydb_platform::unlock

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#include "db.h"

/* Fault injection is compiled only into a private regression-test library. */
#ifdef IDYDB_TESTING
#include "../tests/failure_hooks.h"
static idydb_test_hook idydb_fault_hook = NULL;
static void* idydb_fault_user = NULL;
extern "C" void idydb_test_set_hook(idydb_test_hook hook, void* user)
{
	idydb_fault_hook = hook;
	idydb_fault_user = user;
}
static int idydb_fault(int point)
{
	int failure = idydb_fault_hook ? idydb_fault_hook(point, idydb_fault_user) : 0;
	if (failure) errno = failure;
	return failure;
}
int idydb_platform::fault(int point) { return idydb_fault(point); }
static int idydb_fault_seek(FILE* stream, long long offset, int whence)
{
	if (idydb_fault(IDYDB_TEST_SEEK)) return -1;
	return fseek(stream, offset, whence);
}
static size_t idydb_fault_write(const void* data, size_t size, size_t count, FILE* stream)
{
	int failure = idydb_fault(IDYDB_TEST_WRITE);
	if (!failure) return fwrite(data, size, count, stream);
	if (!size || !count) return 0;
	size_t prefix = size * count / 2;
	if (prefix) (void)fwrite(data, 1, prefix, stream);
	errno = failure;
	return prefix / size;
}
static int idydb_fault_flush(FILE* stream)
{
	if (idydb_fault(IDYDB_TEST_FLUSH)) return EOF;
	return fflush(stream);
}
#undef fseek
#define fseek idydb_fault_seek
#define fwrite idydb_fault_write
#define fflush idydb_fault_flush
#define IDYDB_FAULT(point) idydb_fault(point)
#else
#define IDYDB_FAULT(point) 0
#endif

/* IdyDB database operations (internal) */
static int idydb_new(idydb **handler);
static void idydb_destroy(idydb **handler);
static int idydb_connection_setup(idydb **handler, const char *filename, int flags);
static int idydb_connection_setup_stream(idydb **handler, FILE* stream, int flags, bool stream_locked);
static char *idydb_get_err_message(idydb **handler);
static unsigned char idydb_read_at(idydb **handler, idydb_column_row_sizing column_position, idydb_column_row_sizing row_position);
static void idydb_clear_values(idydb **handler);
static unsigned char idydb_insert_at(idydb **handler, idydb_column_row_sizing column_position, idydb_column_row_sizing row_position);
static unsigned char idydb_insert_at_inplace(idydb **handler, idydb_column_row_sizing column_position, idydb_column_row_sizing row_position);
static unsigned char idydb_insert_value_int(idydb **handler, int set_value);
static unsigned char idydb_insert_value_float(idydb **handler, float set_value);
static unsigned char idydb_insert_value_char(idydb **handler, char *set_value);
static unsigned char idydb_insert_value_bool(idydb **handler, bool set_value);
static unsigned char idydb_insert_value_vector(idydb **handler, const float* data, unsigned short dims);
static void idydb_insert_reset(idydb **handler);
static int idydb_retrieve_value_int(idydb **handler);
static float idydb_retrieve_value_float(idydb **handler);
static char *idydb_retrieve_value_char(idydb **handler);
static bool idydb_retrieve_value_bool(idydb **handler);
static const float* idydb_retrieve_value_vector(idydb **handler, unsigned short* out_dims);
static unsigned char idydb_retrieve_value_type(idydb **handler);
static void idydb_error_state(idydb **handler, unsigned char error_id);
static unsigned char idydb_validate_structure(idydb **handler);

/* ----- constants and layout helpers ----- */
#define IDYDB_MAX_BUFFER_SIZE 1024
#define IDYDB_MAX_CHAR_LENGTH (0xFFFF - sizeof(short))   /* reader expects (stored_len + 1) <= IDYDB_MAX_CHAR_LENGTH */
#define IDYDB_MAX_VECTOR_DIM   16383
#define IDYDB_MAX_ERR_SIZE 100
#define IDYDB_SEGMENT_SIZE 3
#define IDYDB_PARTITION_SIZE 4
#define IDYDB_PARTITION_AND_SEGMENT (IDYDB_SEGMENT_SIZE + IDYDB_PARTITION_SIZE)
#define IDYDB_MMAP_MAX_SIZE 0x1400000

/* on-disk type tags for values that follow a segment header */
#define IDYDB_READ_INT         1
#define IDYDB_READ_FLOAT       2
#define IDYDB_READ_CHAR        3
#define IDYDB_READ_BOOL_TRUE   4
#define IDYDB_READ_BOOL_FALSE  5
#define IDYDB_READ_VECTOR      6

#define IDYDB_READ_AND_WRITE 0
#define IDYDB_READONLY_MMAPPED 2

#ifndef IDYDB_SIZING_MODE
#error No sizing mode type was defined to IDYDB_SIZING_MODE
#elif IDYDB_SIZING_MODE == IDYDB_SIZING_MODE_TINY
#define IDYDB_COLUMN_POSITION_MAX 0x000F
#define IDYDB_ROW_POSITION_MAX 0x000F
typedef unsigned int idydb_size_selection_type;
typedef uint64_t idydb_sizing_max;
#elif IDYDB_SIZING_MODE == IDYDB_SIZING_MODE_SMALL
#define IDYDB_COLUMN_POSITION_MAX 0x00FF
#define IDYDB_ROW_POSITION_MAX 0x00FF
typedef unsigned int idydb_size_selection_type;
typedef uint64_t idydb_sizing_max;
#elif IDYDB_SIZING_MODE == IDYDB_SIZING_MODE_BIG
#define IDYDB_COLUMN_POSITION_MAX 0xFFFF
#define IDYDB_ROW_POSITION_MAX 0xFFFF
#define IDYDB_ALLOW_UNSAFE
typedef unsigned long long idydb_size_selection_type;
typedef unsigned long long idydb_sizing_max;
#else
#error An invalid sizing mode was attributed to IDYDB_SIZING_MODE
#endif

#if IDYDB_MMAP_ALLOWED
#define IDYDB_MMAP_OK
#endif

/* ---------------- Encrypted-at-rest format ----------------
 * Header layout (little-endian ints):
 *  [0..7]   magic "IDYDBENC"
 *  [8..11]  version (u32) = 1
 *  [12..15] pbkdf2_iter (u32)
 *  [16..31] salt (16 bytes)
 *  [32..43] iv (12 bytes) (GCM nonce)
 *  [44..51] plaintext_len (u64)
 *  [52..67] tag (16 bytes) (GCM tag over AAD+ciphertext)
 *  [68..]   ciphertext (plaintext_len bytes)
 *
 * AAD = header bytes [0..51] (everything except the tag).
 */
#define IDYDB_ENC_MAGIC      "IDYDBENC"
#define IDYDB_ENC_MAGIC_LEN  8
#define IDYDB_ENC_VERSION    1u
#define IDYDB_ENC_SALT_LEN   16
#define IDYDB_ENC_IV_LEN     12
#define IDYDB_ENC_TAG_LEN    16
#define IDYDB_ENC_HDR_LEN    (IDYDB_ENC_MAGIC_LEN + 4 + 4 + IDYDB_ENC_SALT_LEN + IDYDB_ENC_IV_LEN + 8 + IDYDB_ENC_TAG_LEN)
#define IDYDB_ENC_AAD_LEN    (IDYDB_ENC_HDR_LEN - IDYDB_ENC_TAG_LEN)
#define IDYDB_ENC_KEY_LEN    32
#define IDYDB_ENC_DEFAULT_PBKDF2_ITER 200000u

/* Reasonable PBKDF2 bounds (DoS mitigation + sanity). Tune as you like. */
#define IDYDB_ENC_MIN_PBKDF2_ITER 10000u
#define IDYDB_ENC_MAX_PBKDF2_ITER 5000000u

static inline void idydb_u32_le_write(unsigned char* p, uint32_t v) {
	p[0] = (unsigned char)(v & 0xFFu);
	p[1] = (unsigned char)((v >> 8) & 0xFFu);
	p[2] = (unsigned char)((v >> 16) & 0xFFu);
	p[3] = (unsigned char)((v >> 24) & 0xFFu);
}
static inline uint32_t idydb_u32_le_read(const unsigned char* p) {
	return (uint32_t)p[0]
		| ((uint32_t)p[1] << 8)
		| ((uint32_t)p[2] << 16)
		| ((uint32_t)p[3] << 24);
}
static inline void idydb_u64_le_write(unsigned char* p, uint64_t v) {
	for (int i = 0; i < 8; ++i) p[i] = (unsigned char)((v >> (8 * i)) & 0xFFu);
}
static inline uint64_t idydb_u64_le_read(const unsigned char* p) {
	uint64_t v = 0;
	for (int i = 0; i < 8; ++i) v |= ((uint64_t)p[i] << (8 * i));
	return v;
}

static inline int idydb_crypto_iter_ok(uint32_t iter) {
	return iter >= IDYDB_ENC_MIN_PBKDF2_ITER && iter <= IDYDB_ENC_MAX_PBKDF2_ITER;
}

static int idydb_crypto_derive_key_pbkdf2(const char* passphrase,
                                         const unsigned char salt[IDYDB_ENC_SALT_LEN],
                                         uint32_t iter,
                                         unsigned char out_key[IDYDB_ENC_KEY_LEN])
{
	if (!passphrase || !out_key || iter == 0) return 0;
	if (!idydb_crypto_iter_ok(iter)) return 0;
	size_t passphrase_len = strlen(passphrase);
	if (passphrase_len > INT_MAX) return 0;

	int ok = PKCS5_PBKDF2_HMAC(passphrase,
	                          (int)passphrase_len,
	                          salt,
	                          IDYDB_ENC_SALT_LEN,
	                          (int)iter,
	                          EVP_sha256(),
	                          IDYDB_ENC_KEY_LEN,
	                          out_key);
	return ok == 1;
}

/* Working-storage policy belongs to the selected OS backend. */
static FILE* idydb_secure_plain_stream(const char** out_kind)
{
    return idydb_platform::secure_plain_stream(out_kind);
}

static int idydb_crypto_decrypt_locked_file_to_stream(FILE* in,
                                                      const char* passphrase,
                                                      FILE* out_plain,
                                                      unsigned char out_salt[IDYDB_ENC_SALT_LEN],
                                                      uint32_t* out_iter,
                                                      unsigned char out_key[IDYDB_ENC_KEY_LEN])
{
	if (!in || !passphrase || !out_plain || !out_salt || !out_iter || !out_key) return 0;

	/* Get total size for sanity checks. */
	if (fseek(in, 0L, SEEK_END) != 0) return 0;
	long long total_sz = ftell(in);
	if (total_sz < 0) return 0;
	if (fseek(in, 0L, SEEK_SET) != 0) return 0;

	if (total_sz < (long long)IDYDB_ENC_HDR_LEN) return 0;

	unsigned char hdr[IDYDB_ENC_HDR_LEN];
	size_t r = fread(hdr, 1, IDYDB_ENC_HDR_LEN, in);
	if (r != IDYDB_ENC_HDR_LEN) return 0;

	if (memcmp(hdr, IDYDB_ENC_MAGIC, IDYDB_ENC_MAGIC_LEN) != 0) return 0;
	uint32_t ver = idydb_u32_le_read(hdr + 8);
	if (ver != IDYDB_ENC_VERSION) return 0;

	uint32_t iter = idydb_u32_le_read(hdr + 12);
	if (!idydb_crypto_iter_ok(iter)) return 0;

	memcpy(out_salt, hdr + 16, IDYDB_ENC_SALT_LEN);

	unsigned char iv[IDYDB_ENC_IV_LEN];
	memcpy(iv, hdr + 32, IDYDB_ENC_IV_LEN);

	uint64_t plaintext_len = idydb_u64_le_read(hdr + 44);

	unsigned char tag[IDYDB_ENC_TAG_LEN];
	memcpy(tag, hdr + 52, IDYDB_ENC_TAG_LEN);

	/* Since GCM ciphertext length equals plaintext length, ensure file length matches. */
	uint64_t cipher_len = (uint64_t)(total_sz - (long long)IDYDB_ENC_HDR_LEN);
	if (cipher_len != plaintext_len) return 0;

	if (!idydb_crypto_derive_key_pbkdf2(passphrase, out_salt, iter, out_key)) return 0;
	*out_iter = iter;

	EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
	if (!ctx) return 0;

	int ok = 1;
	int len = 0;

	if (EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) ok = 0;
	if (ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IDYDB_ENC_IV_LEN, NULL) != 1) ok = 0;
	if (ok && EVP_DecryptInit_ex(ctx, NULL, NULL, out_key, iv) != 1) ok = 0;

	if (ok && EVP_DecryptUpdate(ctx, NULL, &len, hdr, (int)IDYDB_ENC_AAD_LEN) != 1) ok = 0;

	if (fseek(in, IDYDB_ENC_HDR_LEN, SEEK_SET) != 0) { EVP_CIPHER_CTX_free(ctx); return 0; }

	unsigned char inbuf[16 * 1024];
	unsigned char outbuf[16 * 1024 + 16];
	uint64_t written = 0;

	while (ok) {
		size_t n = fread(inbuf, 1, sizeof(inbuf), in);
		if (n == 0) break;
		int outl = 0;
		if (EVP_DecryptUpdate(ctx, outbuf, &outl, inbuf, (int)n) != 1) { ok = 0; break; }
		if (outl > 0) {
			if (fwrite(outbuf, 1, (size_t)outl, out_plain) != (size_t)outl) { ok = 0; break; }
			written += (uint64_t)outl;
		}
	}

	if (ok) {
		if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, IDYDB_ENC_TAG_LEN, tag) != 1) ok = 0;
		unsigned char finalbuf[16];
		int finallen = 0;
		int final_ok = EVP_DecryptFinal_ex(ctx, finalbuf, &finallen);
		if (final_ok != 1) ok = 0;
	}

	EVP_CIPHER_CTX_free(ctx);

	if (ok && (plaintext_len != written || ferror(in))) ok = 0;
	if (fflush(out_plain) != 0 || fseek(out_plain, 0L, SEEK_SET) != 0) ok = 0;
	return ok ? 1 : 0;
}

static int idydb_crypto_encrypt_stream_to_locked_file(FILE* plain,
                                                      FILE* out,
                                                      const unsigned char salt[IDYDB_ENC_SALT_LEN],
                                                      uint32_t iter,
                                                      const unsigned char key[IDYDB_ENC_KEY_LEN])
{
	if (!plain || !out || !salt || !key || iter == 0) return 0;
	if (!idydb_crypto_iter_ok(iter)) return 0;

	if (fflush(plain) != 0) return 0;
	clearerr(plain); /* Retry a transient read failure against the unchanged working image. */
	long long cur = ftell(plain);
	if (cur < 0) cur = 0;
	if (fseek(plain, 0L, SEEK_END) != 0) return 0;
	long long plen_long = ftell(plain);
	if (plen_long < 0) return 0;
	uint64_t plaintext_len = (uint64_t)plen_long;
	if (fseek(plain, 0L, SEEK_SET) != 0) return 0;

	unsigned char iv[IDYDB_ENC_IV_LEN];
	if (RAND_bytes(iv, IDYDB_ENC_IV_LEN) != 1) return 0;

	unsigned char hdr[IDYDB_ENC_HDR_LEN];
	memset(hdr, 0, sizeof(hdr));
	memcpy(hdr, IDYDB_ENC_MAGIC, IDYDB_ENC_MAGIC_LEN);
	idydb_u32_le_write(hdr + 8, IDYDB_ENC_VERSION);
	idydb_u32_le_write(hdr + 12, iter);
	memcpy(hdr + 16, salt, IDYDB_ENC_SALT_LEN);
	memcpy(hdr + 32, iv, IDYDB_ENC_IV_LEN);
	idydb_u64_le_write(hdr + 44, plaintext_len);
	/* tag placeholder at hdr+52..+67 remains zero */

	/* truncate & write placeholder header */
	if (fflush(out) != 0) return 0;
	if (idydb_platform::resize(out, 0) != 0) return 0;

	if (fseek(out, 0L, SEEK_SET) != 0) return 0;
	if (fwrite(hdr, 1, IDYDB_ENC_HDR_LEN, out) != IDYDB_ENC_HDR_LEN) return 0;

	EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
	if (!ctx) return 0;

	int ok = 1;
	int len = 0;

	if (EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), NULL, NULL, NULL) != 1) ok = 0;
	if (ok && EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_IVLEN, IDYDB_ENC_IV_LEN, NULL) != 1) ok = 0;
	if (ok && EVP_EncryptInit_ex(ctx, NULL, NULL, key, iv) != 1) ok = 0;

	if (ok && EVP_EncryptUpdate(ctx, NULL, &len, hdr, (int)IDYDB_ENC_AAD_LEN) != 1) ok = 0;

	unsigned char inbuf[16 * 1024];
	unsigned char outbuf[16 * 1024 + 16];
	uint64_t encrypted_bytes = 0;
	while (ok) {
		size_t n = fread(inbuf, 1, sizeof(inbuf), plain);
		if (n == 0) break;
		int outl = 0;
		if (EVP_EncryptUpdate(ctx, outbuf, &outl, inbuf, (int)n) != 1) { ok = 0; break; }
		if (outl > 0) {
			if (fwrite(outbuf, 1, (size_t)outl, out) != (size_t)outl) { ok = 0; break; }
			encrypted_bytes += (uint64_t)outl;
		}
	}

	if (ok) {
		int finallen = 0;
		unsigned char finalbuf[16];
		if (EVP_EncryptFinal_ex(ctx, finalbuf, &finallen) != 1) ok = 0;
	}

	unsigned char tag[IDYDB_ENC_TAG_LEN];
	if (ok) {
		if (EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, IDYDB_ENC_TAG_LEN, tag) != 1) ok = 0;
	}

	EVP_CIPHER_CTX_free(ctx);

	if (!ok || ferror(plain) || encrypted_bytes != plaintext_len) return 0;

	/* write tag into header */
	if (fseek(out, 52, SEEK_SET) != 0) return 0;
	if (fwrite(tag, 1, IDYDB_ENC_TAG_LEN, out) != IDYDB_ENC_TAG_LEN) return 0;

	if (fflush(out) != 0 || fsync(fileno(out)) != 0) return 0;

	return fseek(plain, cur, SEEK_SET) == 0;
}

/* --------------------------- Core object --------------------------- */

typedef struct idydb
{
	void *buffer;
	bool configured;
	FILE *file_descriptor;
	char err_message[IDYDB_MAX_ERR_SIZE];
	union value
	{
		int   int_value;
		float float_value;
		char  char_value[(IDYDB_MAX_CHAR_LENGTH + 1)];
		bool  bool_value;
	} value;
	float*         vector_value;
	unsigned short vector_dims;

	unsigned char value_type;
	bool value_retrieved;
	idydb_sizing_max size;
	unsigned char read_only;
#ifdef IDYDB_ALLOW_UNSAFE
	bool unsafe;
#endif
#ifdef IDYDB_MMAP_OK
	void* mapping_cookie; /* private backend-owned mapping handle */
#endif

	idydb_embed_fn embedder;
	void* embedder_user;

	/* encryption runtime state */
	bool encryption_enabled;
	bool dirty;
	bool mutation_staging; /* private clones may mutate in place, never the backing file */

	FILE* backing_descriptor; /* locked file handle for encrypted backing (or NULL) */
	char* backing_filename;
	int directory_fd; /* owned POSIX directory anchor, -1 for pathname opens */

	unsigned char enc_salt[IDYDB_ENC_SALT_LEN];
	uint32_t enc_iter;
	unsigned char enc_key[IDYDB_ENC_KEY_LEN];
	bool enc_key_set;

	/* debug: where plaintext lives when encrypted mode is enabled */
	const char* plain_storage_kind; /* "memfd" / "shm" / NULL */

} idydb;

#ifdef IDYDB_TESTING
/* Inspect the physical image through its owning handle: Win32 exclusive locks
 * prohibit opening a second handle even in the process that owns the lock. */
extern "C" int idydb_test_backing_snapshot(idydb** handler, unsigned char** bytes, size_t* length)
{
    if (!bytes || !length) return IDYDB_ERROR;
    *bytes = NULL;
    *length = 0;
    if (!handler || !*handler || !(*handler)->configured) return IDYDB_ERROR;
    FILE* stream = (*handler)->encryption_enabled ? (*handler)->backing_descriptor : (*handler)->file_descriptor;
    if (!stream) return IDYDB_ERROR;
    const int64_t position = idydb_platform::tell(stream);
    if (position < 0 || idydb_platform::seek(stream, 0, SEEK_END) != 0) return IDYDB_ERROR;
    const int64_t size = idydb_platform::tell(stream);
    if (size < 0 || (uint64_t)size > SIZE_MAX || idydb_platform::seek(stream, 0, SEEK_SET) != 0) {
        (void)idydb_platform::seek(stream, position, SEEK_SET);
        return IDYDB_ERROR;
    }
    unsigned char* copy = (unsigned char*)malloc(size ? (size_t)size : 1);
    const bool copied = copy && fread(copy, 1, (size_t)size, stream) == (size_t)size;
    const bool restored = idydb_platform::seek(stream, position, SEEK_SET) == 0;
    if (!copied || !restored) { free(copy); return IDYDB_ERROR; }
    *bytes = copy;
    *length = (size_t)size;
    return IDYDB_DONE;
}
#endif

/* All backing-file commits use a locked sibling file and atomic replacement. */
static FILE* idydb_stage_file(idydb* h, char** out_path)
{
    FILE* backing = h->encryption_enabled ? h->backing_descriptor : h->file_descriptor;
    return idydb_platform::stage(backing, h->backing_filename, h->directory_fd, out_path);
}

static void idydb_stage_cleanup(FILE* stream, char* path, int directory_fd)
{
    if (stream) fclose(stream);
    if (path) {
        idydb_platform::remove_stage(path, directory_fd);
        free(path);
    }
}

static int idydb_sync_directory(const char* filename, int directory_fd)
{
    return idydb_platform::sync_directory(filename, directory_fd);
}

/* Once renamed, adopt the new locked inode even when directory sync fails.
 * The caller can distinguish adoption because *staged is set to NULL. */
static int idydb_publish(idydb* h, FILE** staged, const char* path, idydb_sizing_max size)
{
    if (IDYDB_FAULT(IDYDB_TEST_BEFORE_REPLACE) || IDYDB_FAULT(IDYDB_TEST_REPLACE)) return 0;
    FILE** destination = h->encryption_enabled ? &h->backing_descriptor : &h->file_descriptor;
    if (!idydb_platform::replace(*destination, *staged, path, h->backing_filename, h->directory_fd)) return 0;
    FILE* previous = *destination;
    *destination = *staged;
    *staged = NULL;
    if (!h->encryption_enabled) h->size = size;
    fclose(previous);
    if (IDYDB_FAULT(IDYDB_TEST_AFTER_REPLACE)) return 0;
    return idydb_sync_directory(h->backing_filename, h->directory_fd);
}

static int idydb_crypto_commit(idydb* h)
{
    char* path = NULL;
    FILE* staged = idydb_stage_file(h, &path);
    if (!staged) return 0;
    int ok = idydb_crypto_encrypt_stream_to_locked_file(h->file_descriptor, staged,
        h->enc_salt, h->enc_iter, h->enc_key);
    if (ok) ok = idydb_publish(h, &staged, path, h->size);
    if (staged) idydb_stage_cleanup(staged, path, h->directory_fd);
    else free(path); /* replacement already owns the complete file */
    return ok;
}

/* ---------------- Verbose debug (compile-time) ---------------- */

#ifdef CUWACUNU_CAMAHJUCUNU_DB_VERBOSE_DEBUG

static const char* idydb_ro_str(unsigned char ro)
{
	switch (ro)
	{
		case IDYDB_READ_AND_WRITE:   return "rw";
		case IDYDB_READONLY:         return "ro";
		case IDYDB_READONLY_MMAPPED: return "ro(mmap)";
		default:                     return "unknown";
	}
}

static void db_debugf(idydb **handler, const char* fmt, ...)
{
	(void)handler; /* currently unused, but kept in signature for future context */
	fprintf(stdout, "[DB] ");

	va_list ap;
	va_start(ap, fmt);
	vfprintf(stdout, fmt, ap);
	va_end(ap);

	fputc('\n', stdout);
	fflush(stdout);
}

#define DB_DEBUGF(db, fmt, ...) do { db_debugf((db), (fmt), ##__VA_ARGS__); } while(0)
#define DB_DEBUG(db, msg_literal) do { DB_DEBUGF((db), "%s", (msg_literal)); } while(0)

/* --- formatting helpers --- */

static void idydb_dbg_sha256_8bytes_hex16(const void* data, size_t len, char out_hex16[17])
{
	/* out = 8 bytes of sha256 => 16 hex chars + '\0' */
	static const char* hexd = "0123456789abcdef";
	unsigned char digest[32];
	unsigned int out_len = 0;
	EVP_MD_CTX* ctx = EVP_MD_CTX_new();
	if (!ctx) { strncpy(out_hex16, "noctx", 17); out_hex16[16] = '\0'; return; }

	int ok = (EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1)
	      && (EVP_DigestUpdate(ctx, data, len) == 1)
	      && (EVP_DigestFinal_ex(ctx, digest, &out_len) == 1)
	      && (out_len == 32);

	EVP_MD_CTX_free(ctx);

	if (!ok) { strncpy(out_hex16, "shaerr", 17); out_hex16[16] = '\0'; return; }

	for (int i = 0; i < 8; ++i) {
		out_hex16[2*i + 0] = hexd[(digest[i] >> 4) & 0xF];
		out_hex16[2*i + 1] = hexd[(digest[i] >> 0) & 0xF];
	}
	out_hex16[16] = '\0';
}

static void idydb_dbg_escape_preview(const char* in, char* out, size_t out_cap, size_t max_in_chars)
{
	static const char* hexd = "0123456789abcdef";
	if (!out || out_cap == 0) return;
	out[0] = '\0';
	if (!in) { snprintf(out, out_cap, "<null>"); return; }

	size_t o = 0;
	for (size_t i = 0; in[i] != '\0' && i < max_in_chars; ++i)
	{
		unsigned char ch = (unsigned char)in[i];
		if (o + 2 >= out_cap) break;

		switch (ch)
		{
			case '\\': if (o + 2 < out_cap) { out[o++]='\\'; out[o++]='\\'; } break;
			case '"':  if (o + 2 < out_cap) { out[o++]='\\'; out[o++]='"';  } break;
			case '\n': if (o + 2 < out_cap) { out[o++]='\\'; out[o++]='n';  } break;
			case '\r': if (o + 2 < out_cap) { out[o++]='\\'; out[o++]='r';  } break;
			case '\t': if (o + 2 < out_cap) { out[o++]='\\'; out[o++]='t';  } break;
			default:
				if (isprint(ch)) {
					out[o++] = (char)ch;
				} else {
					/* \xHH */
					if (o + 4 >= out_cap) break;
					out[o++]='\\'; out[o++]='x';
					out[o++]=hexd[(ch >> 4) & 0xF];
					out[o++]=hexd[(ch >> 0) & 0xF];
				}
				break;
		}
		if (o >= out_cap) break;
	}
	out[(o < out_cap) ? o : (out_cap - 1)] = '\0';
}

static void idydb_dbg_format_value_from_handler(const idydb* h, char* out, size_t cap)
{
	if (!out || cap == 0) return;
	if (!h) { snprintf(out, cap, "<nohandler>"); return; }
	if (h->encryption_enabled) { snprintf(out, cap, "<encrypted value>"); return; }

	switch (h->value_type)
	{
		case IDYDB_NULL:
			snprintf(out, cap, "NULL");
			return;

		case IDYDB_INTEGER:
			snprintf(out, cap, "INT(%d)", h->value.int_value);
			return;

		case IDYDB_FLOAT:
			snprintf(out, cap, "FLOAT(%.9g)", (double)h->value.float_value);
			return;

		case IDYDB_BOOL:
			snprintf(out, cap, "BOOL(%s)", (h->value.bool_value ? "true" : "false"));
			return;

		case IDYDB_CHAR:
		{
			const char* s = h->value.char_value;
			size_t len = (s ? strlen(s) : 0);
			char prev[96];
			idydb_dbg_escape_preview(s, prev, sizeof(prev), 48);
			const char* ell = (len > 48 ? "…" : "");
			snprintf(out, cap, "CHAR(len=%zu,\"%s%s\")", len, prev, ell);
			return;
		}

		case IDYDB_VECTOR:
		{
			unsigned short d = h->vector_dims;
			if (!h->vector_value || d == 0) {
				snprintf(out, cap, "VEC(d=%u,<null>)", (unsigned)d);
				return;
			}
			char sha16[17];
			idydb_dbg_sha256_8bytes_hex16(h->vector_value, (size_t)d * sizeof(float), sha16);
			snprintf(out, cap, "VEC(d=%u,sha=%s)", (unsigned)d, sha16);
			return;
		}

		default:
			snprintf(out, cap, "TYPE(%u)", (unsigned)h->value_type);
			return;
	}
}

/* Reads the current stored value of a cell and formats it to out. */
static unsigned char idydb_dbg_peek_cell_repr(idydb **handler,
                                             idydb_column_row_sizing c,
                                             idydb_column_row_sizing r,
                                             char* out,
                                             size_t cap)
{
	if (!out || cap == 0) return 0xFF;
	out[0] = '\0';

	int rc = idydb_read_at(handler, c, r);
	if (rc == IDYDB_DONE) {
		unsigned char t = (*handler)->value_type;
		idydb_dbg_format_value_from_handler(*handler, out, cap);
		idydb_clear_values(handler);
		return t;
	}

	if (rc == IDYDB_NULL) {
		snprintf(out, cap, "NULL");
		idydb_clear_values(handler);
		return IDYDB_NULL;
	}

	snprintf(out, cap, "ERR(rc=%d,%s)", rc, idydb_get_err_message(handler));
	idydb_clear_values(handler);
	return 0xFF;
}

#else

#define DB_DEBUGF(db, fmt, ...) do { (void)(db); } while(0)
#define DB_DEBUG(db, msg_literal) do { (void)(db); (void)(msg_literal); } while(0)

#endif


/* ---------------- Error state ---------------- */
static void idydb_error_statef(idydb **handler, unsigned char error_id, const char* fmt, ...)
{
  idydb_error_state(handler, error_id);

  if (!handler || !*handler || !fmt) return;

  char buf[IDYDB_MAX_ERR_SIZE];
  buf[0] = '\0';

  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  // Only override if we produced something non-empty.
  if (buf[0] != '\0') {
    snprintf((*handler)->err_message, IDYDB_MAX_ERR_SIZE, "%s", buf);
  }
}
static void idydb_error_state(idydb **handler, unsigned char error_id)
{
	if (!handler || !*handler) return;
	const char *errors[] = {
		"\0",
		"The minimum buffer size has encroached beyond suitable definitions\0",
		"The maximum buffer size has encroached beyond suitable definitions\0",
		"The database handler has already been attributed to handle another database\0",
		"No database exists to be exclusively read\0",
		"Failed to open the database\0",
		"Exclusive rights to access the database could not be obtained\0",
		"The database attempted to access has a larger size than what this object can read\0",
		"The database handler has not been attributed to handle a database\0",
		"The database was opened in readonly mode\0",
		"Data insertion avoided due to unexpected tennant\0",
		"Data insertion avoided due to the length of a string being too large (or vector too large)\0",
#if IDYDB_SIZING_MODE == IDYDB_SIZING_MODE_TINY
		"The requested range was outside of the database's range (sizing mode parameter is: tiny)\0",
#elif IDYDB_SIZING_MODE == IDYDB_SIZING_MODE_SMALL
		"The requested range was outside of the database's range (sizing mode parameter is: small)\0",
#elif IDYDB_SIZING_MODE == IDYDB_SIZING_MODE_BIG
		"The requested range was outside of the database's range\0",
#endif
		"The database contracted a malformed structure declaration\0",
		"An error occurred in attempting to read data from the database\0",
		"An error occurred in attempting to write data to the database\0",
		"An error occurred in attempting to write data to an updating skip offset notation in the database\0",
		"Failed database truncation occurred\0",
		"An error occurred in attempting to retrieve data from the database\0",
		"Data retrieval avoided due to the length of a string being too large\0",
		"The database yielded an invalid datatype\0",
		"The requested range must have a valid starting range of at least 1\0",
		"The database declares ranges that exceed the current sizing mode parameter set\0",
		"Unable to enable unsafe mode due to compilation sizing mode parameter set\0",
		"Unable to allocate memory for the creation of the database handler\0",
		"An unknown error occurred\0",
		"Encryption requested but no passphrase supplied\0",
		"Database decryption failed (wrong passphrase, tampered file, or unsupported parameters)\0",
		"Database encryption writeback failed\0",
		"Failed to create secure in-memory plaintext working storage\0",
		"Encrypted READONLY open cannot migrate plaintext db; open writable once to migrate\0"
	};

	const unsigned char max_id = (unsigned char)(sizeof(errors) / sizeof(errors[0]) - 1);
	if (error_id > max_id) error_id = max_id;
	snprintf((*handler)->err_message, IDYDB_MAX_ERR_SIZE, "%s", errors[error_id]);
}

/* ---------------- core helpers ---------------- */

unsigned int idydb_version_check()
{
	return IDYDB_VERSION;
}

static void idydb_clear_values(idydb **handler)
{
	(*handler)->value.int_value = 0;
	(*handler)->value_type = IDYDB_NULL;
	(*handler)->value_retrieved = false;

	if ((*handler)->vector_value != NULL) {
		free((*handler)->vector_value);
		(*handler)->vector_value = NULL;
	}
	(*handler)->vector_dims = 0;
}

static int idydb_new(idydb **handler)
{
	*handler = (idydb *)malloc(sizeof(idydb));
	if (*handler == NULL)
		return IDYDB_ERROR;

	(*handler)->buffer = NULL; /* important */
	(*handler)->configured = (IDYDB_MAX_BUFFER_SIZE < 50 || IDYDB_MAX_BUFFER_SIZE > 1024);
	(*handler)->size = 0;
	(*handler)->read_only = IDYDB_READ_AND_WRITE;
#ifdef IDYDB_ALLOW_UNSAFE
	(*handler)->unsafe = false;
#endif
	(*handler)->value_type = 0;
	memset(&(*handler)->value, 0, sizeof((*handler)->value));
	(*handler)->value_retrieved = false;
	(*handler)->file_descriptor = NULL;
	(*handler)->vector_value = NULL;
	(*handler)->vector_dims = 0;
	(*handler)->embedder = NULL;
	(*handler)->embedder_user = NULL;

	(*handler)->encryption_enabled = false;
	(*handler)->dirty = false;
	(*handler)->mutation_staging = false;
	(*handler)->backing_descriptor = NULL;
	(*handler)->backing_filename = NULL;
	(*handler)->directory_fd = -1;
	memset((*handler)->enc_salt, 0, sizeof((*handler)->enc_salt));
	(*handler)->enc_iter = 0;
	memset((*handler)->enc_key, 0, sizeof((*handler)->enc_key));
	(*handler)->enc_key_set = false;
	(*handler)->plain_storage_kind = NULL;

#ifdef IDYDB_MMAP_OK
	(*handler)->mapping_cookie = NULL;
#endif

	if (IDYDB_MAX_BUFFER_SIZE < 50)
	{
		idydb_error_state(handler, 1);
		return IDYDB_ERROR;
	}
	else if (IDYDB_MAX_BUFFER_SIZE > 1024)
	{
		idydb_error_state(handler, 2);
		return IDYDB_ERROR;
	}
	else
		idydb_error_state(handler, 0);
	return IDYDB_SUCCESS;
}

static void idydb_destroy(idydb **handler)
{
	if (!handler || !*handler) return;

	(*handler)->configured = false;
	if ((*handler)->file_descriptor != NULL)
	{
			flock(fileno((*handler)->file_descriptor), LOCK_UN);
			fclose((*handler)->file_descriptor);
			(*handler)->file_descriptor = NULL;
	}

	if ((*handler)->backing_descriptor != NULL)
	{
		flock(fileno((*handler)->backing_descriptor), LOCK_UN);
		fclose((*handler)->backing_descriptor);
		(*handler)->backing_descriptor = NULL;
	}
	if ((*handler)->backing_filename != NULL)
	{
		free((*handler)->backing_filename);
		(*handler)->backing_filename = NULL;
	}
	if ((*handler)->directory_fd >= 0) {
		idydb_platform::close_directory((*handler)->directory_fd);
		(*handler)->directory_fd = -1;
	}
	{
		OPENSSL_cleanse((*handler)->enc_key, sizeof((*handler)->enc_key));
		(*handler)->enc_key_set = false;
		(*handler)->dirty = false;
		(*handler)->encryption_enabled = false;
	}

	/* buffer teardown (malloc vs mmap) */
#ifdef IDYDB_MMAP_OK
	if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
	{
		idydb_platform::unmap((*handler)->buffer, (*handler)->size, (*handler)->mapping_cookie);
		(*handler)->buffer = NULL;
		(*handler)->mapping_cookie = NULL;
	}
	else
#endif
	{
		if ((*handler)->buffer != NULL) free((*handler)->buffer);
		(*handler)->buffer = NULL;
	}

	if ((*handler)->vector_value != NULL) {
		free((*handler)->vector_value);
		(*handler)->vector_value = NULL;
	}
}

static inline idydb_sizing_max idydb_max_size()
{
	const uint64_t columns = (uint64_t)IDYDB_COLUMN_POSITION_MAX + 1;
	const uint64_t rows = (uint64_t)IDYDB_ROW_POSITION_MAX + 1;
	/* Maximum CHAR payload includes its NUL and uint16 length field. */
	const uint64_t cell_bytes = IDYDB_SEGMENT_SIZE + IDYDB_MAX_CHAR_LENGTH + sizeof(short);
	return (idydb_sizing_max)(columns * (IDYDB_PARTITION_SIZE + rows * cell_bytes));
}

static int idydb_connection_setup_stream(idydb **handler, FILE* stream, int flags, bool stream_locked)
{
	if ((*handler)->configured)
	{
		if (IDYDB_MAX_BUFFER_SIZE >= 50 && IDYDB_MAX_BUFFER_SIZE <= 1024)
			idydb_error_state(handler, 3);
		return IDYDB_ERROR;
	}

	(*handler)->size = 0;
	(*handler)->read_only = IDYDB_READ_AND_WRITE;
	if ((flags & IDYDB_READONLY) == IDYDB_READONLY)
		(*handler)->read_only = IDYDB_READONLY;

	(*handler)->file_descriptor = stream;
	if ((*handler)->file_descriptor == NULL)
	{
		idydb_error_state(handler, 5);
		return IDYDB_PERM;
	}

	if (fseek(stream, 0L, SEEK_END) != 0) return IDYDB_ERROR;
	long long stream_size = ftell(stream);
	if (stream_size < 0 || (uint64_t)stream_size > (uint64_t)((idydb_sizing_max)-1)) return IDYDB_RANGE;
	(*handler)->size = (idydb_sizing_max)stream_size;
	if (fseek(stream, 0L, SEEK_SET) != 0) return IDYDB_ERROR;

	(*handler)->configured = true;

	int op = LOCK_NB | (((flags & IDYDB_READONLY) == IDYDB_READONLY) ? LOCK_SH : LOCK_EX);
	if (!stream_locked && flock(fileno((*handler)->file_descriptor), op) != 0)
	{
		idydb_error_state(handler, 6);
		return IDYDB_BUSY;
	}

	if ((flags & IDYDB_UNSAFE) == IDYDB_UNSAFE)
#ifdef IDYDB_ALLOW_UNSAFE
		(*handler)->unsafe = true;
#else
	{
		idydb_error_state(handler, 23);
		return IDYDB_ERROR;
	}
#endif
	else if ((*handler)->size > idydb_max_size())
	{
		idydb_error_state(handler, 7);
		return IDYDB_RANGE;
	}

	unsigned char validation = idydb_validate_structure(handler);
	if (validation != IDYDB_DONE) return validation;
	(*handler)->buffer = malloc((sizeof(char) * IDYDB_MAX_BUFFER_SIZE));
	if ((*handler)->buffer == NULL)
	{
		idydb_error_state(handler, 24);
		return IDYDB_ERROR;
	}
	return IDYDB_SUCCESS;
}

/* Do not truncate on create. Verify pathname identity after acquiring the lock,
 * so a caller never writes an old inode after another handle replaces the file. */
static FILE* idydb_open_locked(const char* filename, int flags, int* status, int directory_fd = -1)
{
    return idydb_platform::open_locked(filename, flags, status, directory_fd);
}

static int idydb_connection_setup(idydb **handler, const char *filename, int flags)
{
    int status;
    FILE* stream = idydb_open_locked(filename, flags, &status);
    if (!stream) {
        idydb_error_state(handler, status == IDYDB_BUSY ? 6 : (status == IDYDB_NOT_FOUND ? 4 : 5));
        return status;
    }
    int rc = idydb_connection_setup_stream(handler, stream, flags, true);
    if (rc != IDYDB_SUCCESS) return rc;
    if ((*handler)->read_only == IDYDB_READ_AND_WRITE) {
    (*handler)->backing_filename = idydb_platform::canonical_path(stream, filename);
    if (!(*handler)->backing_filename) { idydb_error_state(handler, 24); return IDYDB_ERROR; }
    }
#ifdef IDYDB_MMAP_OK
    if ((*handler)->read_only == IDYDB_READONLY && (*handler)->size > 0
        && (*handler)->size <= IDYDB_MMAP_MAX_SIZE) {
        void* cookie = NULL;
        void* view = idydb_platform::map_readonly(stream, (*handler)->size, &cookie);
        if (view) {
            free((*handler)->buffer); (*handler)->buffer = view;
            (*handler)->mapping_cookie = cookie;
            (*handler)->read_only = IDYDB_READONLY_MMAPPED;
        }
    }
#endif
    return IDYDB_SUCCESS;
}

static char *idydb_get_err_message(idydb **handler)
{
	if (!handler || *handler == NULL)
		return (char *)"This handler failed to be setup\0";
	return (*handler)->err_message;
}

/* ---------------- Public open/close (runtime encryption) ---------------- */

static int idydb_open_internal(const char *filename, idydb **handler, const idydb_open_options* options)
{
	if (!filename || !handler || !options) return IDYDB_ERROR;

	if (idydb_new(handler) == IDYDB_ERROR)
	{
		*handler = NULL;
		return IDYDB_ERROR;
	}

	int flags = options->flags;

	if (!options->encrypted_at_rest)
	{
		int rc = idydb_connection_setup(handler, filename, flags);
		if (rc == IDYDB_SUCCESS) {
			(*handler)->encryption_enabled = false;
			(*handler)->dirty = false;
			idydb_clear_values(handler);
			DB_DEBUGF(handler, "opened PLAINTEXT db file=\"%s\" flags=0x%x", filename, flags);
		}
		return rc;
	}

	/* encrypted-at-rest path */
	if (!options->passphrase)
	{
		idydb_error_state(handler, 26);
		DB_DEBUGF(handler, "encrypted open refused: passphrase is NULL (file=\"%s\")", filename);
		return IDYDB_ERROR;
	}

	bool file_exists = idydb_platform::exists(filename);
	if (!file_exists && ((flags & IDYDB_CREATE) == 0))
	{
		idydb_error_state(handler, 4);
		return IDYDB_NOT_FOUND;
	}

	/* open + lock backing file */
	const bool ro = ((flags & IDYDB_READONLY) == IDYDB_READONLY);

	int backing_status;
	FILE* backing = idydb_open_locked(filename, flags, &backing_status);
	if (!backing) {
		idydb_error_state(handler, backing_status == IDYDB_BUSY ? 6 : (backing_status == IDYDB_NOT_FOUND ? 4 : 5));
		return backing_status;
	}

	(*handler)->encryption_enabled = true;
	(*handler)->backing_descriptor = backing;
	if (!ro) {
	(*handler)->backing_filename = idydb_platform::canonical_path(backing, filename);
	if (!(*handler)->backing_filename) { idydb_error_state(handler, 24); return IDYDB_ERROR; }
	}
	(*handler)->dirty = false;

	const char* kind = NULL;
	FILE* plain = idydb_secure_plain_stream(&kind);
	if (!plain)
	{
		idydb_error_state(handler, 29);
		DB_DEBUGF(handler, "failed to create secure in-memory plaintext working storage (backing=\"%s\")", filename);
		return IDYDB_ERROR;
	}
	(*handler)->plain_storage_kind = kind;

	DB_DEBUGF(handler, "opened ENCRYPTED-AT-REST db backing=\"%s\" ro=%s exists=%s working_plain=%s",
	          filename, (ro ? "yes" : "no"), (file_exists ? "yes" : "no"), (kind ? kind : "unknown"));

	/* detect encrypted header */
	if (fseek(backing, 0L, SEEK_END) != 0) { fclose(plain); return IDYDB_ERROR; }
	long long bsz = ftell(backing);
	if (bsz < 0 || fseek(backing, 0L, SEEK_SET) != 0) { fclose(plain); return IDYDB_ERROR; }

	bool is_enc = false;
	if (bsz >= (long)IDYDB_ENC_MAGIC_LEN)
	{
		unsigned char magic[IDYDB_ENC_MAGIC_LEN];
		size_t rr = fread(magic, 1, IDYDB_ENC_MAGIC_LEN, backing);
		if (rr != IDYDB_ENC_MAGIC_LEN || fseek(backing, 0L, SEEK_SET) != 0) {
			fclose(plain); return IDYDB_ERROR;
		}
		if (rr == IDYDB_ENC_MAGIC_LEN && memcmp(magic, IDYDB_ENC_MAGIC, IDYDB_ENC_MAGIC_LEN) == 0) is_enc = true;
	}

	/* IMPORTANT: encrypted-at-rest should not silently accept a plaintext backing in READONLY mode */
	if (!is_enc && ro && bsz > 0)
	{
		idydb_error_state(handler, 30);
		DB_DEBUGF(handler, "refusing encrypted READONLY open on PLAINTEXT backing; open writable once to migrate");
		fclose(plain);
		return IDYDB_ERROR;
	}

	if (is_enc)
	{
		uint32_t iter = 0;
		DB_DEBUGF(handler, "encrypted container detected; decrypting...");
		if (!idydb_crypto_decrypt_locked_file_to_stream(backing, options->passphrase, plain,
		                                                (*handler)->enc_salt, &iter, (*handler)->enc_key))
		{
			idydb_error_state(handler, 27);
			DB_DEBUGF(handler, "decrypt FAILED (wrong passphrase, tampered file, or unsupported params)");
			fclose(plain);
			return IDYDB_ERROR;
		}
		(*handler)->enc_iter = iter;
		(*handler)->enc_key_set = true;

		if (fseek(plain, 0L, SEEK_END) != 0) { fclose(plain); return IDYDB_ERROR; }
		[[maybe_unused]] long long psz = ftell(plain);
		if (psz < 0 || fseek(plain, 0L, SEEK_SET) != 0) { fclose(plain); return IDYDB_ERROR; }
		DB_DEBUGF(handler, "decrypt OK -> plaintext bytes=%lld pbkdf2_iter=%u", psz, iter);
	}
	else
	{
		/* plaintext backing (migration) */
		if (bsz > 0)
		{
			DB_DEBUGF(handler, "PLAINTEXT backing detected; copying into working plaintext stream (migration)");
			unsigned char buf[16 * 1024];
			uint64_t copied = 0;
			while (1) {
				size_t n = fread(buf, 1, sizeof(buf), backing);
				if (n == 0) break;
				if (fwrite(buf, 1, n, plain) != n) { idydb_error_state(handler, 26); fclose(plain); return IDYDB_ERROR; }
				copied += n;
			}
			if (ferror(backing) || copied != (uint64_t)bsz || fflush(plain) != 0 ||
			    fseek(plain, 0L, SEEK_SET) != 0 || fseek(backing, 0L, SEEK_SET) != 0) {
				fclose(plain); return IDYDB_ERROR;
			}
		}

		/* generate new key/salt for migration or new encrypted creation */
		uint32_t iter = (options->pbkdf2_iter == 0 ? IDYDB_ENC_DEFAULT_PBKDF2_ITER : (uint32_t)options->pbkdf2_iter);
		if (!idydb_crypto_iter_ok(iter)) {
			idydb_error_state(handler, 26);
			fclose(plain);
			return IDYDB_ERROR;
		}

		(*handler)->enc_iter = iter;

		if (RAND_bytes((*handler)->enc_salt, IDYDB_ENC_SALT_LEN) != 1)
		{
			idydb_error_state(handler, 26);
			fclose(plain);
			return IDYDB_ERROR;
		}
		if (!idydb_crypto_derive_key_pbkdf2(options->passphrase, (*handler)->enc_salt, (*handler)->enc_iter, (*handler)->enc_key))
		{
			idydb_error_state(handler, 26);
			fclose(plain);
			return IDYDB_ERROR;
		}
		(*handler)->enc_key_set = true;

		/* If readonly, do not writeback. If writeable, mark dirty to force encryption on close. */
		if (!ro) (*handler)->dirty = true;

		DB_DEBUGF(handler, "migration/new-encrypted setup: pbkdf2_iter=%u dirty=%s", (*handler)->enc_iter, ((*handler)->dirty ? "yes" : "no"));
	}

	/* Setup db handler to operate on plaintext stream */
	int setup_rc = idydb_connection_setup_stream(handler, plain, flags, false);
	if (setup_rc != IDYDB_SUCCESS)
	{
		return setup_rc;
	}

	idydb_clear_values(handler);
	DB_DEBUGF(handler, "ready: db opened against secure working plaintext stream kind=%s", (kind ? kind : "unknown"));
	return IDYDB_SUCCESS;
}

int idydb_open_with_options(const char *filename, idydb **handler, const idydb_open_options* options)
{
	if (!filename || !*filename || !handler || !options) return IDYDB_ERROR;
	if (*handler && (*handler)->configured) {
		idydb_error_state(handler, 3);
		return IDYDB_ERROR;
	}
	if (*handler) { idydb_destroy(handler); free(*handler); *handler = NULL; }
	int rc = idydb_open_internal(filename, handler, options);
	if (rc != IDYDB_SUCCESS) idydb_destroy(handler);
	return rc;
}

int idydb_open(const char *filename, idydb **handler, int flags)
{
	idydb_open_options opt;
	opt.flags = flags;
	opt.encrypted_at_rest = false;
	opt.passphrase = NULL;
	opt.pbkdf2_iter = 0;
	return idydb_open_with_options(filename, handler, &opt);
}

int idydb_open_at(int directory_fd, const char* filename, idydb** handler, int flags)
{
#if defined(_WIN32)
	(void)directory_fd; (void)filename; (void)flags;
	idydb_error_statef(handler, 8, "Directory-relative access is unsupported on Windows");
	return IDYDB_ERROR;
#else
	if (!handler || !filename || !*filename || strchr(filename, '/') ||
	    strcmp(filename, ".") == 0 || strcmp(filename, "..") == 0 ||
	    (flags & ~(IDYDB_CREATE | IDYDB_READONLY | IDYDB_UNSAFE)) != 0 ||
	    ((flags & IDYDB_CREATE) != 0 && (flags & IDYDB_CREATE) != IDYDB_CREATE)) return IDYDB_ERROR;
	if (*handler && (*handler)->configured) { idydb_error_state(handler, 3); return IDYDB_ERROR; }
	int owned_directory = idydb_platform::duplicate_directory(directory_fd);
	if (owned_directory < 0) return errno == EINVAL || errno == EBADF ? IDYDB_ERROR : IDYDB_PERM;
	if (*handler) { idydb_destroy(handler); free(*handler); *handler = NULL; }
	if (idydb_new(handler) != IDYDB_SUCCESS) { idydb_platform::close_directory(owned_directory); return IDYDB_ERROR; }
	(*handler)->directory_fd = owned_directory;
	(*handler)->backing_filename = (char*)malloc(strlen(filename) + 1);
	if (!(*handler)->backing_filename) {
		idydb_error_state(handler, 24); idydb_destroy(handler); return IDYDB_ERROR;
	}
	strcpy((*handler)->backing_filename, filename);
	int rc;
	FILE* stream = idydb_open_locked(filename, flags, &rc, owned_directory);
	if (!stream) {
		idydb_error_state(handler, rc == IDYDB_BUSY ? 6 : (rc == IDYDB_NOT_FOUND ? 4 : 5));
		idydb_destroy(handler);
		return rc;
	}
	rc = idydb_connection_setup_stream(handler, stream, flags, true);
	if (rc != IDYDB_SUCCESS) { idydb_destroy(handler); return rc; }
	idydb_clear_values(handler);
	return IDYDB_SUCCESS;
#endif
}

int idydb_open_encrypted(const char *filename, idydb **handler, int flags, const char* passphrase)
{
	idydb_open_options opt;
	opt.flags = flags;
	opt.encrypted_at_rest = true;
	opt.passphrase = passphrase;
	opt.pbkdf2_iter = 0;
	return idydb_open_with_options(filename, handler, &opt);
}

int idydb_flush(idydb **handler)
{
	if (!handler || !*handler || !(*handler)->configured) { idydb_error_state(handler, 8); return IDYDB_ERROR; }
	if ((*handler)->read_only != IDYDB_READ_AND_WRITE) return IDYDB_DONE;

	/* If encrypted-at-rest and dirty and writable, writeback ciphertext */
	if ((*handler)->encryption_enabled &&
	    (*handler)->enc_key_set &&
	    (*handler)->backing_descriptor != NULL &&
	    (*handler)->read_only == IDYDB_READ_AND_WRITE &&
	    (*handler)->dirty)
	{
		DB_DEBUGF(handler, "flush: encrypting writeback -> backing=\"%s\" pbkdf2_iter=%u",
		          ((*handler)->backing_filename ? (*handler)->backing_filename : "(unknown)"),
		          (*handler)->enc_iter);

		if (idydb_validate_structure(handler) != IDYDB_DONE || !idydb_crypto_commit(*handler))
		{
			idydb_error_state(handler, 28);
			DB_DEBUGF(handler, "flush: writeback or durability check failed; handle retained for retry");
			return IDYDB_ERROR;
		}
		DB_DEBUGF(handler, "flush: writeback OK");
		(*handler)->dirty = false;
	}
	else
	{
		DB_DEBUGF(handler, "flush: no writeback (enc=%s dirty=%s mode=%s)",
		          ((*handler)->encryption_enabled ? "yes" : "no"),
		          ((*handler)->dirty ? "yes" : "no"),
		          idydb_ro_str((*handler)->read_only));
	}

	if ((*handler)->configured && (*handler)->read_only == IDYDB_READ_AND_WRITE &&
	    (fflush((*handler)->file_descriptor) != 0 || fsync(fileno((*handler)->file_descriptor)) != 0)) {
		idydb_error_state(handler, 15);
		return IDYDB_ERROR;
	}
	/* A previous publish may already be visible while its directory sync failed.
	 * Retry that durability step even when there are no new plaintext changes. */
	if (!(*handler)->encryption_enabled && !idydb_sync_directory((*handler)->backing_filename, (*handler)->directory_fd)) {
		idydb_error_state(handler, 15);
		return IDYDB_ERROR;
	}
	return IDYDB_DONE;
}

int idydb_discard(idydb **handler)
{
	if (!handler || !*handler) return IDYDB_DONE;
	idydb_destroy(handler);
	free(*handler);
	*handler = NULL;
	return IDYDB_DONE;
}

int idydb_close(idydb **handler)
{
	if (!handler || !*handler) return IDYDB_DONE;
	if ((*handler)->configured) {
		int rc = idydb_flush(handler);
		if (rc != IDYDB_DONE) return rc;
	}
	return idydb_discard(handler);
}

/* ---------------- Public API thin wrappers ---------------- */

/* One bounded decoder is shared by extraction, queries and mutation validation.
 * Counts are wider than the on-disk count-minus-one field (65535 means 65536).
 */
struct idydb_scan_cursor {
	idydb_sizing_max offset;
	idydb_size_selection_type column;
	uint32_t rows_remaining;
	unsigned short last_row;
	bool have_last_row;
};

struct idydb_record {
	idydb_sizing_max offset;
	idydb_sizing_max data_offset;
	idydb_sizing_max payload_size;
	idydb_sizing_max next_offset;
	idydb_size_selection_type column;
	idydb_column_row_sizing row;
	unsigned char type;
	unsigned short field;
};

static unsigned char idydb_read_bytes_at(idydb **handler, idydb_sizing_max position, void* out, size_t count)
{
	if (!handler || !*handler || !(*handler)->configured || (!out && count)) return IDYDB_ERROR;
	if (position > (*handler)->size || count > (*handler)->size - position) {
		idydb_error_state(handler, 13);
		return IDYDB_CORRUPT;
	}
	if (!count) return IDYDB_DONE;
#ifdef IDYDB_MMAP_OK
	if ((*handler)->read_only == IDYDB_READONLY_MMAPPED) {
		memcpy(out, (const unsigned char*)(*handler)->buffer + position, count);
		return IDYDB_DONE;
	}
#endif
	if (fseek((*handler)->file_descriptor, position, SEEK_SET) != 0 ||
	    fread(out, 1, count, (*handler)->file_descriptor) != count) {
		idydb_error_state(handler, 14);
		return IDYDB_ERROR;
	}
	return IDYDB_DONE;
}

static unsigned char idydb_scan_next(idydb **handler, idydb_scan_cursor* cursor, idydb_record* record)
{
	if (!handler || !*handler || !(*handler)->configured || !cursor || !record) return IDYDB_ERROR;
	if (cursor->offset == (*handler)->size && cursor->rows_remaining == 0) return IDYDB_NULL;
	const bool partition = cursor->rows_remaining == 0;
	const size_t header_size = partition ? IDYDB_PARTITION_AND_SEGMENT : IDYDB_SEGMENT_SIZE;
	unsigned char header[IDYDB_PARTITION_AND_SEGMENT];
	unsigned char rc = idydb_read_bytes_at(handler, cursor->offset, header, header_size);
	if (rc != IDYDB_DONE) return rc;
	unsigned short stored_row = 0;
	if (partition) {
		unsigned short skip = 0, count = 0;
		memcpy(&skip, header, sizeof(skip));
		memcpy(&count, header + sizeof(short), sizeof(count));
		if (cursor->column > (idydb_size_selection_type)-1 - ((idydb_size_selection_type)skip + 1)) {
			idydb_error_state(handler, 13); return IDYDB_CORRUPT;
		}
		cursor->column += (idydb_size_selection_type)skip + 1;
		cursor->rows_remaining = (uint32_t)count + 1;
		cursor->have_last_row = false;
		if (cursor->rows_remaining > (uint32_t)IDYDB_ROW_POSITION_MAX + 1) {
			idydb_error_state(handler, 22); return IDYDB_RANGE;
		}
#ifdef IDYDB_ALLOW_UNSAFE
		if (!(*handler)->unsafe && cursor->column > (idydb_size_selection_type)IDYDB_COLUMN_POSITION_MAX + 1) {
#else
		if (cursor->column > (idydb_size_selection_type)IDYDB_COLUMN_POSITION_MAX + 1) {
#endif
			idydb_error_state(handler, 22); return IDYDB_RANGE;
		}
	}
	const size_t row_offset = partition ? IDYDB_PARTITION_SIZE : 0;
	memcpy(&stored_row, header + row_offset, sizeof(stored_row));
	if (
#if IDYDB_SIZING_MODE != IDYDB_SIZING_MODE_BIG
	    stored_row > IDYDB_ROW_POSITION_MAX ||
#endif
	    (cursor->have_last_row && stored_row <= cursor->last_row)) {
		idydb_error_state(handler, 13); return IDYDB_CORRUPT;
	}
	record->offset = cursor->offset;
	record->column = cursor->column;
	record->row = (idydb_column_row_sizing)stored_row + 1;
	record->type = header[row_offset + sizeof(short)];
	record->field = 0;
	record->data_offset = cursor->offset + header_size;
	switch (record->type) {
		case IDYDB_READ_INT: record->payload_size = sizeof(int); break;
		case IDYDB_READ_FLOAT: record->payload_size = sizeof(float); break;
		case IDYDB_READ_BOOL_TRUE:
		case IDYDB_READ_BOOL_FALSE: record->payload_size = 0; break;
		case IDYDB_READ_CHAR:
		case IDYDB_READ_VECTOR:
			rc = idydb_read_bytes_at(handler, record->data_offset, &record->field, sizeof(short));
			if (rc != IDYDB_DONE) return rc;
			record->data_offset += sizeof(short);
			if (record->type == IDYDB_READ_CHAR) {
				if (record->field >= IDYDB_MAX_CHAR_LENGTH) {
					idydb_error_state(handler, 19); return IDYDB_CORRUPT;
				}
				record->payload_size = (idydb_sizing_max)record->field + 1;
			} else {
				if (record->field == 0 || record->field > IDYDB_MAX_VECTOR_DIM) {
					idydb_error_state(handler, 19); return IDYDB_CORRUPT;
				}
				record->payload_size = (idydb_sizing_max)record->field * sizeof(float);
			}
			break;
		default: idydb_error_state(handler, 20); return IDYDB_CORRUPT;
	}
	if (record->data_offset > (*handler)->size || record->payload_size > (*handler)->size - record->data_offset) {
		idydb_error_state(handler, 13); return IDYDB_CORRUPT;
	}
	if (record->type == IDYDB_READ_CHAR) {
		unsigned char chunk[IDYDB_MAX_BUFFER_SIZE];
		idydb_sizing_max checked = 0;
		while (checked < record->payload_size) {
			const size_t count = (size_t)((record->payload_size - checked < sizeof(chunk)) ? record->payload_size - checked : sizeof(chunk));
			rc = idydb_read_bytes_at(handler, record->data_offset + checked, chunk, count);
			if (rc != IDYDB_DONE) return rc;
			for (size_t i = 0; i < count; ++i) {
				const bool last = checked + i + 1 == record->payload_size;
				if ((chunk[i] == 0) != last) { idydb_error_state(handler, 13); return IDYDB_CORRUPT; }
			}
			checked += count;
		}
	}
	record->next_offset = record->data_offset + record->payload_size;
	cursor->offset = record->next_offset;
	cursor->last_row = stored_row;
	cursor->have_last_row = true;
	--cursor->rows_remaining;
	return IDYDB_DONE;
}

static unsigned char idydb_validate_structure(idydb **handler)
{
	idydb_scan_cursor cursor = {};
	idydb_record record = {};
	unsigned char rc;
	while ((rc = idydb_scan_next(handler, &cursor, &record)) == IDYDB_DONE) {}
	return rc == IDYDB_NULL ? IDYDB_DONE : rc;
}

/* Unsafe mode can extend the column range, but each stored gap is still uint16.
 * Reject changes that would truncate a new or merged gap before moving bytes.
 */
static unsigned char idydb_validate_column_gap(idydb **handler, idydb_column_row_sizing column, idydb_column_row_sizing row)
{
	idydb_scan_cursor cursor = {};
	idydb_record record = {};
	idydb_size_selection_type previous = 0, next = 0;
	uint32_t rows = 0;
	bool found_row = false;
	unsigned char rc;
	while ((rc = idydb_scan_next(handler, &cursor, &record)) == IDYDB_DONE) {
		if (record.column < column) previous = record.column;
		else if (record.column == column) { ++rows; if (record.row == row) found_row = true; }
		else { next = record.column; break; }
	}
	if (rc != IDYDB_DONE && rc != IDYDB_NULL) return rc;
	const bool inserting_column = rows == 0 && (*handler)->value_type != IDYDB_NULL;
	const bool removing_column = rows == 1 && found_row && (*handler)->value_type == IDYDB_NULL;
	if ((inserting_column && column - previous - 1 > 0xFFFF) ||
	    (removing_column && next != 0 && next - previous - 1 > 0xFFFF)) {
		idydb_error_state(handler, 12);
		return IDYDB_RANGE;
	}
	return IDYDB_DONE;
}

char *idydb_errmsg(idydb **handler) { return idydb_get_err_message(handler); }
int idydb_extract(idydb **handler, idydb_column_row_sizing c, idydb_column_row_sizing r) { return idydb_read_at(handler, c, r); }
int idydb_retrieved_type(idydb **handler) { return idydb_retrieve_value_type(handler); }

/* ---------------- insert value staging ---------------- */

static unsigned char idydb_insert_value_int(idydb **handler, int set_value)
{
	if (!handler || !*handler) return IDYDB_ERROR;
	if (!(*handler)->configured) { idydb_error_state(handler, 8); return IDYDB_ERROR; }
	if ((*handler)->read_only != IDYDB_READ_AND_WRITE) { idydb_error_state(handler, 9); return IDYDB_READONLY; }
	if ((*handler)->value_type != IDYDB_NULL && !(*handler)->value_retrieved) { idydb_error_state(handler, 10); return IDYDB_ERROR; }
	idydb_clear_values(handler);
	(*handler)->value_type = IDYDB_INTEGER;
	(*handler)->value.int_value = set_value;
	return IDYDB_DONE;
}

static unsigned char idydb_insert_value_float(idydb **handler, float set_value)
{
	if (!handler || !*handler) return IDYDB_ERROR;
	if (!(*handler)->configured) { idydb_error_state(handler, 8); return IDYDB_ERROR; }
	if ((*handler)->read_only != IDYDB_READ_AND_WRITE) { idydb_error_state(handler, 9); return IDYDB_READONLY; }
	if ((*handler)->value_type != IDYDB_NULL && !(*handler)->value_retrieved) { idydb_error_state(handler, 10); return IDYDB_ERROR; }
	idydb_clear_values(handler);
	(*handler)->value_type = IDYDB_FLOAT;
	(*handler)->value.float_value = set_value;
	return IDYDB_DONE;
}

static unsigned char idydb_insert_value_char(idydb **handler, char *set_value)
{
	if (!handler || !*handler || !set_value) { idydb_error_state(handler, 8); return IDYDB_ERROR; }
	if (!(*handler)->configured) { idydb_error_state(handler, 8); return IDYDB_ERROR; }
	if ((*handler)->read_only != IDYDB_READ_AND_WRITE) { idydb_error_state(handler, 9); return IDYDB_READONLY; }
	if ((*handler)->value_type != IDYDB_NULL && !(*handler)->value_retrieved) { idydb_error_state(handler, 10); return IDYDB_ERROR; }
	/* The input may be the handler's most recently retrieved string. */
	const size_t length = strlen(set_value);
	char* copy = (char*)malloc(length + 1);
	if (!copy) { idydb_error_state(handler, 24); return IDYDB_ERROR; }
	memcpy(copy, set_value, length + 1);
	idydb_clear_values(handler);
	(*handler)->value_type = IDYDB_CHAR;
	memcpy((*handler)->value.char_value, copy, length + 1);
	free(copy);
	return IDYDB_DONE;
}

static unsigned char idydb_insert_value_bool(idydb **handler, bool set_value)
{
	if (!handler || !*handler) return IDYDB_ERROR;
	if (!(*handler)->configured) { idydb_error_state(handler, 8); return IDYDB_ERROR; }
	if ((*handler)->read_only != IDYDB_READ_AND_WRITE) { idydb_error_state(handler, 9); return IDYDB_READONLY; }
	if ((*handler)->value_type != IDYDB_NULL && !(*handler)->value_retrieved) { idydb_error_state(handler, 10); return IDYDB_ERROR; }
	idydb_clear_values(handler);
	(*handler)->value_type = IDYDB_BOOL;
	(*handler)->value.bool_value = set_value;
	return IDYDB_DONE;
}

static unsigned char idydb_insert_value_vector(idydb **handler, const float* data, unsigned short dims)
{
	if (!handler || !*handler || !data) { idydb_error_state(handler, 8); return IDYDB_ERROR; }
	if (!(*handler)->configured) { idydb_error_state(handler, 8); return IDYDB_ERROR; }
	if ((*handler)->read_only != IDYDB_READ_AND_WRITE) { idydb_error_state(handler, 9); return IDYDB_READONLY; }
	if ((*handler)->value_type != IDYDB_NULL && !(*handler)->value_retrieved) { idydb_error_state(handler, 10); return IDYDB_ERROR; }
	if (dims == 0 || dims > IDYDB_MAX_VECTOR_DIM) { idydb_error_state(handler, 11); return IDYDB_ERROR; }
	/* Copy before clearing: data may alias a previously retrieved vector. */
	float* copy = (float*)malloc(sizeof(float) * (size_t)dims);
	if (!copy) { idydb_error_state(handler, 24); return IDYDB_ERROR; }
	memcpy(copy, data, sizeof(float) * (size_t)dims);
	idydb_clear_values(handler);
	(*handler)->value_type  = IDYDB_VECTOR;
	(*handler)->vector_dims = dims;
	(*handler)->vector_value = copy;
	return IDYDB_DONE;
}

static void idydb_insert_reset(idydb **handler) { idydb_clear_values(handler); }

/* ---------------- Public inserts ---------------- */

int idydb_insert_int(idydb **handler, idydb_column_row_sizing c, idydb_column_row_sizing r, int value)
{
	unsigned char s = idydb_insert_value_int(handler, value);
	if (s != IDYDB_DONE) return s;
	return idydb_insert_at(handler, c, r);
}

int idydb_insert_float(idydb **handler, idydb_column_row_sizing c, idydb_column_row_sizing r, float value)
{
	unsigned char s = idydb_insert_value_float(handler, value);
	if (s != IDYDB_DONE) return s;
	return idydb_insert_at(handler, c, r);
}

int idydb_insert_char(idydb **handler, idydb_column_row_sizing c, idydb_column_row_sizing r, char *value)
{
	if (!handler || !*handler || !value) { idydb_error_state(handler, 8); return IDYDB_ERROR; }
	size_t value_length = strlen(value);
	/* reader requires stored_len+1 <= IDYDB_MAX_CHAR_LENGTH => stored_len <= IDYDB_MAX_CHAR_LENGTH-1 */
	if (value_length >= IDYDB_MAX_CHAR_LENGTH)
	{
		idydb_error_state(handler, 11);
		return IDYDB_ERROR;
	}
	unsigned char s = idydb_insert_value_char(handler, value);
	if (s != IDYDB_DONE) return s;
	return idydb_insert_at(handler, c, r);
}

int idydb_insert_const_char(idydb **handler, idydb_column_row_sizing c, idydb_column_row_sizing r, const char *value)
{
	if (!handler || !*handler || !value) { idydb_error_state(handler, 8); return IDYDB_ERROR; }
	size_t value_length = strlen(value);
	if (value_length >= IDYDB_MAX_CHAR_LENGTH)
	{
		idydb_error_state(handler, 11);
		return IDYDB_ERROR;
	}

	char* tmp = (char*)malloc((size_t)value_length + 1);
	if (!tmp) { idydb_error_state(handler, 24); return IDYDB_ERROR; }
	memcpy(tmp, value, (size_t)value_length + 1);
	int rc = idydb_insert_char(handler, c, r, tmp);
	free(tmp);
	return rc;
}

int idydb_insert_bool(idydb **handler, idydb_column_row_sizing c, idydb_column_row_sizing r, bool value)
{
	unsigned char s = idydb_insert_value_bool(handler, (value == true));
	if (s != IDYDB_DONE) return s;
	return idydb_insert_at(handler, c, r);
}

int idydb_insert_vector(idydb **handler, idydb_column_row_sizing c, idydb_column_row_sizing r, const float* data, unsigned short dims)
{
	unsigned char s = idydb_insert_value_vector(handler, data, dims);
	if (s != IDYDB_DONE) return s;
	return idydb_insert_at(handler, c, r);
}

int idydb_delete(idydb **handler, idydb_column_row_sizing c, idydb_column_row_sizing r)
{
	if (!handler || !*handler) return IDYDB_ERROR;
	idydb_insert_reset(handler);
	return idydb_insert_at(handler, c, r);
}

/* ---------------- retrieve staged value ---------------- */

static int idydb_retrieve_value_int(idydb **handler) { return (handler && *handler && (*handler)->value_type == IDYDB_INTEGER) ? (*handler)->value.int_value : 0; }
static float idydb_retrieve_value_float(idydb **handler) { return (handler && *handler && (*handler)->value_type == IDYDB_FLOAT) ? (*handler)->value.float_value : 0.0f; }
static char *idydb_retrieve_value_char(idydb **handler) { return (handler && *handler && (*handler)->value_type == IDYDB_CHAR) ? (*handler)->value.char_value : NULL; }
static bool idydb_retrieve_value_bool(idydb **handler) { return (handler && *handler && (*handler)->value_type == IDYDB_BOOL) ? (*handler)->value.bool_value : false; }

static const float* idydb_retrieve_value_vector(idydb **handler, unsigned short* out_dims)
{
	if (out_dims) *out_dims = 0;
	if (!handler || !*handler) return NULL;
	if ((*handler)->value_type == IDYDB_VECTOR) {
		if (out_dims) *out_dims = (*handler)->vector_dims;
		return (*handler)->vector_value;
	}
	if (out_dims) *out_dims = 0;
	return NULL;
}

static unsigned char idydb_retrieve_value_type(idydb **handler) { return (handler && *handler) ? (*handler)->value_type : IDYDB_NULL; }

int idydb_retrieve_int(idydb **handler) { return idydb_retrieve_value_int(handler); }
float idydb_retrieve_float(idydb **handler) { return idydb_retrieve_value_float(handler); }
char *idydb_retrieve_char(idydb **handler) { return idydb_retrieve_value_char(handler); }
bool idydb_retrieve_bool(idydb **handler) { return idydb_retrieve_value_bool(handler); }
const float* idydb_retrieve_vector(idydb **handler, unsigned short* out_dims) { return idydb_retrieve_value_vector(handler, out_dims); }

/* ---------------- read value at (column,row) ---------------- */
/* Decode bounded records before copying a selected payload. */

static unsigned char idydb_read_at(idydb **handler, idydb_column_row_sizing column_position, idydb_column_row_sizing row_position)
{
	if (!handler || !*handler || !(*handler)->configured) {
		idydb_error_state(handler, 8);
		return IDYDB_ERROR;
	}
	idydb_clear_values(handler);
	if (column_position == 0 || row_position == 0 ||
	    row_position - 1 > IDYDB_ROW_POSITION_MAX
#ifdef IDYDB_ALLOW_UNSAFE
	    || (!(*handler)->unsafe && column_position - 1 > IDYDB_COLUMN_POSITION_MAX)
#else
	    || column_position - 1 > IDYDB_COLUMN_POSITION_MAX
#endif
	) {
		idydb_error_state(handler, 12);
		return IDYDB_RANGE;
	}
	idydb_scan_cursor cursor = {};
	idydb_record record = {};
	unsigned char rc;
	while ((rc = idydb_scan_next(handler, &cursor, &record)) == IDYDB_DONE) {
		if (record.column > column_position) return IDYDB_NULL;
		if (record.column != column_position || record.row != row_position) continue;
		switch (record.type) {
			case IDYDB_READ_INT:
				rc = idydb_read_bytes_at(handler, record.data_offset, &(*handler)->value.int_value, sizeof(int));
				(*handler)->value_type = IDYDB_INTEGER;
				break;
			case IDYDB_READ_FLOAT:
				rc = idydb_read_bytes_at(handler, record.data_offset, &(*handler)->value.float_value, sizeof(float));
				(*handler)->value_type = IDYDB_FLOAT;
				break;
			case IDYDB_READ_CHAR:
				rc = idydb_read_bytes_at(handler, record.data_offset, (*handler)->value.char_value, (size_t)record.payload_size);
				(*handler)->value_type = IDYDB_CHAR;
				break;
			case IDYDB_READ_BOOL_TRUE:
			case IDYDB_READ_BOOL_FALSE:
				(*handler)->value.bool_value = record.type == IDYDB_READ_BOOL_TRUE;
				(*handler)->value_type = IDYDB_BOOL;
				rc = IDYDB_DONE;
				break;
			case IDYDB_READ_VECTOR:
				(*handler)->vector_value = (float*)malloc((size_t)record.payload_size);
				if (!(*handler)->vector_value) {
					idydb_error_state(handler, 24);
					return IDYDB_ERROR;
				}
				(*handler)->vector_dims = record.field;
				(*handler)->value_type = IDYDB_VECTOR;
				rc = idydb_read_bytes_at(handler, record.data_offset, (*handler)->vector_value, (size_t)record.payload_size);
				break;
			default: idydb_error_state(handler, 20); return IDYDB_CORRUPT;
		}
		if (rc != IDYDB_DONE) { idydb_clear_values(handler); return rc; }
		(*handler)->value_retrieved = true;
		return IDYDB_DONE;
	}
	return rc;
}

/* ---------------- insert value at (column,row) ---------------- */
/* This is your original insert logic, with:
 *  - CHAR payload sizing fixed (includes '\0')
 *  - length field written correctly
 *  - dirty flag set on success
 */

/* Mutation clones isolate every write, including partial I/O failures. */
static int idydb_mutation_begin(idydb** handler, idydb** staged, char** path)
{
    *staged = NULL;
    *path = NULL;
    if (!handler || !*handler || !(*handler)->configured) { idydb_error_state(handler, 8); return IDYDB_ERROR; }
    idydb* h = *handler;
    if (h->read_only != IDYDB_READ_AND_WRITE) { idydb_error_state(handler, 9); return IDYDB_READONLY; }
    if (idydb_new(staged) != IDYDB_SUCCESS) { idydb_error_state(handler, 24); return IDYDB_ERROR; }
    idydb* copy = *staged;
    copy->mutation_staging = true;
    copy->encryption_enabled = h->encryption_enabled;
#ifdef IDYDB_ALLOW_UNSAFE
    copy->unsafe = h->unsafe;
#endif
    copy->size = h->size;
    copy->read_only = IDYDB_READ_AND_WRITE;
    copy->file_descriptor = h->encryption_enabled ? idydb_secure_plain_stream(NULL) : idydb_stage_file(h, path);
    copy->buffer = malloc(IDYDB_MAX_BUFFER_SIZE);
    copy->configured = copy->file_descriptor && copy->buffer;
    if (!copy->configured) { idydb_error_state(handler, 24); return IDYDB_ERROR; }
    memcpy(&copy->value, &h->value, sizeof(h->value));
    copy->value_type = h->value_type;
    copy->value_retrieved = h->value_retrieved;
    if (h->vector_value && h->vector_dims) {
        copy->vector_value = (float*)malloc((size_t)h->vector_dims * sizeof(float));
        if (!copy->vector_value) { idydb_error_state(handler, 24); return IDYDB_ERROR; }
        copy->vector_dims = h->vector_dims;
        memcpy(copy->vector_value, h->vector_value, (size_t)h->vector_dims * sizeof(float));
    }
    FILE* source = h->file_descriptor;
    if (fflush(source) != 0 || fseek(source, 0, SEEK_END) != 0) {
        idydb_error_state(handler, 14); return IDYDB_ERROR;
    }
    clearerr(source);
    long long actual_size = ftell(source);
    if (actual_size < 0 || (uint64_t)actual_size != h->size || fseek(source, 0, SEEK_SET) != 0) {
        idydb_error_state(handler, 14); return IDYDB_ERROR;
    }
    unsigned char buffer[16 * 1024];
    idydb_sizing_max remaining = h->size;
    while (remaining) {
        size_t count = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
        if (IDYDB_FAULT(IDYDB_TEST_COPY_READ) || fread(buffer, 1, count, source) != count) {
            idydb_error_state(handler, 14); return IDYDB_ERROR;
        }
        int failure = IDYDB_FAULT(IDYDB_TEST_COPY_WRITE);
        if (failure) {
            if (count / 2) (void)fwrite(buffer, 1, count / 2, copy->file_descriptor);
            errno = failure;
            idydb_error_state(handler, 15); return IDYDB_ERROR;
        }
        if (fwrite(buffer, 1, count, copy->file_descriptor) != count) {
            idydb_error_state(handler, 15); return IDYDB_ERROR;
        }
        remaining -= count;
    }
    if (ferror(source) || fflush(copy->file_descriptor) != 0 ||
        fseek(copy->file_descriptor, 0, SEEK_SET) != 0) {
        idydb_error_state(handler, 15); return IDYDB_ERROR;
    }
    return IDYDB_DONE;
}

static void idydb_mutation_cleanup(idydb** staged, char* path, int directory_fd)
{
    idydb_discard(staged);
    if (path) idydb_stage_cleanup(NULL, path, directory_fd);
}

static int idydb_mutation_finish(idydb** handler, idydb** staged, char** path, int status)
{
    idydb* h = *handler;
    idydb* copy = *staged;
    if (status != IDYDB_DONE) {
        if (copy && copy->err_message[0]) snprintf(h->err_message, sizeof(h->err_message), "%s", copy->err_message);
        return status;
    }
    status = idydb_validate_structure(staged);
    if (status != IDYDB_DONE) {
        snprintf(h->err_message, sizeof(h->err_message), "%s", copy->err_message);
        return status;
    }
    if (fflush(copy->file_descriptor) != 0) { idydb_error_state(handler, 15); return IDYDB_ERROR; }
    if (h->encryption_enabled) {
        FILE* previous = h->file_descriptor;
        h->file_descriptor = copy->file_descriptor;
        copy->file_descriptor = NULL;
        h->size = copy->size;
        h->dirty = true;
        fclose(previous);
        return IDYDB_DONE;
    }
    if (fsync(fileno(copy->file_descriptor)) != 0) { idydb_error_state(handler, 15); return IDYDB_ERROR; }
    int committed = idydb_publish(h, &copy->file_descriptor, *path, copy->size);
    if (!copy->file_descriptor) {
        free(*path);
        *path = NULL; /* never unlink a file already adopted as the backing */
        h->dirty = false;
    }
    if (!committed) { idydb_error_state(handler, 15); return IDYDB_ERROR; }
    return IDYDB_DONE;
}

static unsigned char idydb_insert_at(idydb** handler, idydb_column_row_sizing column, idydb_column_row_sizing row)
{
    if (!handler || !*handler) return IDYDB_ERROR;
    if ((*handler)->mutation_staging) {
        if (IDYDB_FAULT(IDYDB_TEST_CELL_WRITE)) { idydb_error_state(handler, 15); return IDYDB_ERROR; }
        return idydb_insert_at_inplace(handler, column, row);
    }
    idydb* staged = NULL;
    char* path = NULL;
    int status = idydb_mutation_begin(handler, &staged, &path);
    if (status == IDYDB_DONE) {
        if (IDYDB_FAULT(IDYDB_TEST_CELL_WRITE)) { idydb_error_state(&staged, 15); status = IDYDB_ERROR; }
        else status = idydb_insert_at_inplace(&staged, column, row);
        status = idydb_mutation_finish(handler, &staged, &path, status);
    }
    idydb_mutation_cleanup(&staged, path, (*handler)->directory_fd);
    idydb_clear_values(handler);
    return (unsigned char)status;
}

static unsigned char idydb_insert_at_inplace(idydb **handler, idydb_column_row_sizing column_position, idydb_column_row_sizing row_position)
{
	if (!handler || !*handler) return IDYDB_ERROR;
	if (!(*handler)->configured)
	{
		idydb_error_state(handler, 8);
		return IDYDB_ERROR;
	}
	if ((*handler)->read_only != IDYDB_READ_AND_WRITE)
	{
		idydb_error_state(handler, 9);
		idydb_clear_values(handler);
		return IDYDB_READONLY;
	}
	const unsigned char validation = idydb_validate_structure(handler);
	if (validation != IDYDB_DONE) { idydb_clear_values(handler); return validation; }
#ifdef IDYDB_ALLOW_UNSAFE
	if (!(*handler)->unsafe)
	{
#endif
		if (column_position == 0 || (column_position - 1) > IDYDB_COLUMN_POSITION_MAX || row_position == 0 || (row_position - 1) > IDYDB_ROW_POSITION_MAX)
		{
			idydb_error_state(handler, 12);
			idydb_clear_values(handler);
			return IDYDB_RANGE;
		}
#ifdef IDYDB_ALLOW_UNSAFE
	}
	else
#endif
		if (column_position == 0 || row_position == 0)
	{
		idydb_error_state(handler, 21);
		idydb_clear_values(handler);
		return IDYDB_RANGE;
	}
	else if ((row_position - 1) > IDYDB_ROW_POSITION_MAX)
	{
		idydb_error_state(handler, 12);
		idydb_clear_values(handler);
		return IDYDB_RANGE;
	}
	const unsigned char gap_validation = idydb_validate_column_gap(handler, column_position, row_position);
	if (gap_validation != IDYDB_DONE) { idydb_clear_values(handler); return gap_validation; }

#ifdef CUWACUNU_CAMAHJUCUNU_DB_VERBOSE_DEBUG
	/* Keep original coords for debug */
	idydb_column_row_sizing dbg_col = column_position;
	idydb_column_row_sizing dbg_row = row_position;

	/* Capture before/after in a way that doesn't break the staged write. */
	idydb_sizing_max dbg_size_before = (*handler)->size;

	char dbg_before[256]; dbg_before[0] = '\0';
	char dbg_after[256];  dbg_after[0]  = '\0';
	bool dbg_have = true;

	/* Preserve staged payload so we can peek old cell value safely. */
	unsigned char dbg_stage_type = (*handler)->value_type;

	int   dbg_i = 0;
	float dbg_f = 0.0f;
	bool  dbg_b = false;

	char* dbg_sdup = NULL;

	float* dbg_vec_ptr = NULL;
	unsigned short dbg_vec_dims = 0;

	if (dbg_stage_type == IDYDB_INTEGER) dbg_i = (*handler)->value.int_value;
	else if (dbg_stage_type == IDYDB_FLOAT) dbg_f = (*handler)->value.float_value;
	else if (dbg_stage_type == IDYDB_BOOL) dbg_b = (*handler)->value.bool_value;
	else if (dbg_stage_type == IDYDB_CHAR)
	{
		size_t n = strlen((*handler)->value.char_value);
		dbg_sdup = (char*)malloc(n + 1);
		if (dbg_sdup) memcpy(dbg_sdup, (*handler)->value.char_value, n + 1);
		else dbg_have = false; /* can't safely peek without losing staged string */
	}
	else if (dbg_stage_type == IDYDB_VECTOR)
	{
		dbg_vec_ptr  = (*handler)->vector_value;
		dbg_vec_dims = (*handler)->vector_dims;

		/* Prevent idydb_clear_values() (inside idydb_read_at) from freeing the staged vector */
		(*handler)->vector_value = NULL;
		(*handler)->vector_dims  = 0;
	}

	if (dbg_have)
	{
		(void)idydb_dbg_peek_cell_repr(handler, dbg_col, dbg_row, dbg_before, sizeof(dbg_before));

		/* Restore staged payload (idydb_read_at clobbered handler state) */
		idydb_clear_values(handler);
		(*handler)->value_type = dbg_stage_type;
		(*handler)->value_retrieved = false;

		if (dbg_stage_type == IDYDB_INTEGER) (*handler)->value.int_value = dbg_i;
		else if (dbg_stage_type == IDYDB_FLOAT) (*handler)->value.float_value = dbg_f;
		else if (dbg_stage_type == IDYDB_BOOL) (*handler)->value.bool_value = dbg_b;
		else if (dbg_stage_type == IDYDB_CHAR && dbg_sdup)
		{
			strncpy((*handler)->value.char_value, dbg_sdup, sizeof((*handler)->value.char_value));
			(*handler)->value.char_value[sizeof((*handler)->value.char_value) - 1] = '\0';
			free(dbg_sdup);
			dbg_sdup = NULL;
		}
		else if (dbg_stage_type == IDYDB_VECTOR)
		{
			(*handler)->vector_value = dbg_vec_ptr;
			(*handler)->vector_dims  = dbg_vec_dims;
		}

		idydb_dbg_format_value_from_handler(*handler, dbg_after, sizeof(dbg_after));
	}
	else
	{
		/* can't peek safely; still log "after" */
		idydb_dbg_format_value_from_handler(*handler, dbg_after, sizeof(dbg_after));
		snprintf(dbg_before, sizeof(dbg_before), "<peek skipped: OOM>");

		if (dbg_sdup) free(dbg_sdup);

		if (dbg_stage_type == IDYDB_VECTOR) {
			(*handler)->vector_value = dbg_vec_ptr;
			(*handler)->vector_dims  = dbg_vec_dims;
		}
	}
#endif

	row_position -= 1;

	unsigned short input_size = 0;
	switch ((*handler)->value_type)
	{
	case IDYDB_INTEGER:
		input_size = sizeof(int);
		break;
	case IDYDB_FLOAT:
		input_size = sizeof(float);
		break;
	case IDYDB_BOOL:
		break;
	case IDYDB_CHAR:
	{
		/* FIX: payload includes '\0' */
		unsigned short len = (unsigned short)strlen((*handler)->value.char_value);
		input_size = (unsigned short)(len + 1); /* payload bytes */
		break;
	}
	case IDYDB_VECTOR:
		if ((*handler)->vector_dims == 0 || (*handler)->vector_dims > IDYDB_MAX_VECTOR_DIM) {
			idydb_clear_values(handler);
			idydb_error_state(handler, 11);
			return IDYDB_ERROR;
		}
		input_size = (unsigned short)((*handler)->vector_dims * sizeof(float));
		break;
	}

	const unsigned short input_size_default = input_size; /* payload bytes */
	if ((*handler)->value_type == IDYDB_CHAR || (*handler)->value_type == IDYDB_VECTOR)
		input_size += sizeof(short);

	idydb_sizing_max offset[6] = {0, 0, 0, 0, 0, 0};
	idydb_size_selection_type skip_offset[2] = {0, 0};
	unsigned short skip_amount[2] = {0, 0};
	unsigned short read_length[2] = {IDYDB_PARTITION_AND_SEGMENT, IDYDB_PARTITION_AND_SEGMENT};
	uint32_t row_count[3] = {0, 0, 0};
	unsigned short current_length[2] = {0, 0};
	unsigned char current_type = 0;

	/* ---------------- READ STRUCTURE (original) ---------------- */
	if ((*handler)->size > 0)
	{
		for (;;)
		{
			read_length[1] = read_length[0];
			offset[2] = offset[0];
			if (offset[0] >= (*handler)->size)
			{
				if ((offset[0] - current_length[0]) > (*handler)->size)
				{
					idydb_clear_values(handler);
					idydb_error_state(handler, 13);
					return IDYDB_CORRUPT;
				}
				else if (skip_offset[0] == column_position)
				{
					offset[0] = offset[3];
					offset[1] = (*handler)->size;
					row_count[0] = row_count[1];
					current_length[0] = 0;
				}
				else if (skip_offset[0] < column_position)
				{
					offset[0] = (*handler)->size;
					offset[1] = offset[0];
					row_count[0] = 0;
					current_length[0] = 0;
				}
				break;
			}
			if (fseek((*handler)->file_descriptor, offset[0], SEEK_SET) != 0)
			{
				idydb_clear_values(handler);
				idydb_error_state(handler, 14);
				return IDYDB_ERROR;
			}
			offset[0] += read_length[0];
			unsigned char skip_read_offset = 1;
			if (row_count[0] == 0)
			{
				offset[1] = offset[2];
				offset[5] = offset[3];
				offset[3] = offset[2];
				offset[4] = (offset[2] + IDYDB_PARTITION_SIZE);
				skip_amount[1] = skip_amount[0];
				if (fread(&skip_amount[0], 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short))
				{
					idydb_clear_values(handler);
					idydb_error_state(handler, 14);
					return IDYDB_ERROR;
				}
				skip_offset[1] = skip_offset[0];
				skip_offset[0] += skip_amount[0];
				if (skip_offset[0] > IDYDB_COLUMN_POSITION_MAX)
				{
#ifdef IDYDB_ALLOW_UNSAFE
					if (!(*handler)->unsafe)
					{
#endif
						idydb_clear_values(handler);
						idydb_error_state(handler, 22);
						return IDYDB_RANGE;
#ifdef IDYDB_ALLOW_UNSAFE
					}
#endif
				}
				skip_offset[0] += 1;
				unsigned short stored_count = 0;
				if (fread(&stored_count, 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short))
				{
					idydb_clear_values(handler);
					idydb_error_state(handler, 14);
					return IDYDB_ERROR;
				}
				row_count[0] = stored_count;
#if IDYDB_SIZING_MODE != IDYDB_SIZING_MODE_BIG
				if (row_count[0] > IDYDB_ROW_POSITION_MAX)
				{
					idydb_clear_values(handler);
					idydb_error_state(handler, 22);
					return IDYDB_RANGE;
				}
#endif
				row_count[2] = row_count[1];
				row_count[0] += 1;
				row_count[1] = row_count[0];
				if (row_count[0] > 1)
					read_length[0] = IDYDB_SEGMENT_SIZE;
			}
			else
			{
				if (skip_offset[0] != column_position &&
				    fseek((*handler)->file_descriptor, skip_read_offset++, SEEK_CUR) != 0)
				{
					idydb_clear_values(handler);
					idydb_error_state(handler, 14);
					return IDYDB_ERROR;
				}
				offset[1] += read_length[0];
			}
			current_length[0] = 0;
			if (skip_offset[0] == column_position)
			{
				skip_offset[1] = skip_offset[0];
				unsigned short position;
				if (fread(&position, 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short))
				{
					idydb_clear_values(handler);
					idydb_error_state(handler, 14);
					return IDYDB_ERROR;
				}
				if (position == row_position)
				{
					if (fread(&current_type, 1, 1, (*handler)->file_descriptor) != 1)
					{
						idydb_clear_values(handler);
						idydb_error_state(handler, 14);
						return IDYDB_ERROR;
					}
					current_length[1] = 1;
					switch (current_type)
					{
					case IDYDB_READ_INT:   current_length[0] = sizeof(int); break;
					case IDYDB_READ_FLOAT: current_length[0] = sizeof(float); break;
					case IDYDB_READ_CHAR:
						if (fread(&current_length[0], 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short))
						{
							idydb_error_state(handler, 14);
							return IDYDB_ERROR;
						}
						current_length[0] += (1 + sizeof(short));
						break;
					case IDYDB_READ_BOOL_TRUE:
					case IDYDB_READ_BOOL_FALSE:
						break;
					case IDYDB_READ_VECTOR:
					{
						unsigned short dims = 0;
						if (fread(&dims, 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short))
						{
							idydb_error_state(handler, 14);
							return IDYDB_ERROR;
						}
						if (dims == 0 || dims > IDYDB_MAX_VECTOR_DIM)
						{
							idydb_error_state(handler, 22);
							return IDYDB_RANGE;
						}
						current_length[0] = (unsigned short)(sizeof(short) + (dims * sizeof(float)));
						break;
					}
					default:
						idydb_clear_values(handler);
						idydb_error_state(handler, 20);
						return IDYDB_ERROR;
					}
					offset[0] = offset[3];
					offset[1] = offset[2];
					row_count[0] = row_count[1];
					break;
				}
#if IDYDB_SIZING_MODE != IDYDB_SIZING_MODE_BIG
				else if (position > IDYDB_ROW_POSITION_MAX)
				{
					idydb_clear_values(handler);
					idydb_error_state(handler, 22);
					return IDYDB_RANGE;
				}
#endif
				else if (row_count[0] >= 1 && position > row_position)
				{
					offset[0] = offset[3];
					offset[1] = offset[2];
					current_length[0] = 0;
					row_count[0] = row_count[1];
					break;
				}
				else if (row_position < position)
				{
					offset[0] = offset[2];
					current_length[0] = 0;
					row_count[0] = row_count[1];
					break;
				}
			}
			else if (skip_offset[0] > column_position)
			{
				skip_offset[0] = skip_offset[1];
				skip_amount[0] = skip_amount[1];
				offset[1] = offset[2];
				if (skip_offset[0] == column_position)
					offset[0] = offset[5];
				else
					offset[0] = offset[2];
				read_length[0] = read_length[1];
				current_length[0] = 0;
				if (skip_offset[1] == column_position)
					row_count[0] = row_count[2];
				else
					row_count[0] = 0;
				break;
			}
			else if (fseek((*handler)->file_descriptor, (3 - skip_read_offset), SEEK_CUR) != 0)
			{
				idydb_clear_values(handler);
				idydb_error_state(handler, 14);
				return IDYDB_ERROR;
			}

			if (fread(&current_type, 1, 1, (*handler)->file_descriptor) != 1)
			{
				idydb_clear_values(handler);
				idydb_error_state(handler, 14);
				return IDYDB_ERROR;
			}
			switch (current_type)
			{
			case IDYDB_READ_INT: current_length[0] = sizeof(int); break;
			case IDYDB_READ_FLOAT: current_length[0] = sizeof(float); break;
			case IDYDB_READ_CHAR:
				if (fread(&current_length[0], 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short))
				{
					idydb_clear_values(handler);
					idydb_error_state(handler, 14);
					return IDYDB_ERROR;
				}
				current_length[0] += (1 + sizeof(short));
				break;
			case IDYDB_READ_BOOL_TRUE:
			case IDYDB_READ_BOOL_FALSE:
				break;
			case IDYDB_READ_VECTOR:
			{
				unsigned short dims = 0;
				if (fread(&dims, 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short))
				{
					idydb_clear_values(handler);
					idydb_error_state(handler, 14);
					return IDYDB_ERROR;
				}
				current_length[0] = (unsigned short)(sizeof(short) + (dims * sizeof(float)));
				break;
			}
			default:
				idydb_clear_values(handler);
				idydb_error_state(handler, 20);
				return IDYDB_ERROR;
			}
			current_type = 0;
			if (row_count[0] == 1)
				read_length[0] = IDYDB_PARTITION_AND_SEGMENT;
			row_count[0] -= 1;
			offset[0] += current_length[0];
		}
	}

	if (current_length[1] == 0 && (input_size == 0 && (*handler)->value_type == IDYDB_NULL))
		return IDYDB_DONE;

	struct relinquish_excersion { unsigned short size; idydb_sizing_max position; bool use; };

	struct relinquish_excersion info_skip_offset = {0, 0, false};

	struct relinquish_excersion info_row_count = {
		(unsigned short)(row_count[0]),
		0,
		false
	};

	struct relinquish_excersion info_row_position = {(unsigned short)row_position, 0, false};

	/* FIX: CHAR stores strlen (payload-1); VECTOR stores dims */
	unsigned short char_len_field = 0;
	if ((*handler)->value_type == IDYDB_CHAR && input_size_default > 0) char_len_field = (unsigned short)(input_size_default - 1);

	struct relinquish_excersion info_input_size = {
		(unsigned short)(((*handler)->value_type == IDYDB_CHAR) ? char_len_field :
		                 ((*handler)->value_type == IDYDB_VECTOR ? (*handler)->vector_dims : 0)),
		0,
		false
	};

	struct relinquish_excersion info_input_type = {(unsigned short)(*handler)->value_type, 0, false};
	struct relinquish_excersion info_input_buffer = {0, 0, false};

	bool removal = false;

	if (input_size > current_length[0] || (current_length[1] == 0 && (*handler)->value_type != IDYDB_NULL))
	{
		unsigned short offset_sizing = (unsigned short)(input_size - current_length[0]);
		unsigned char additional_offset = 0;
		if (current_length[1] == 0)
		{
			if (row_count[0] == 0) additional_offset = IDYDB_PARTITION_AND_SEGMENT;
			else additional_offset = IDYDB_SEGMENT_SIZE;
		}
		if (offset[1] < (*handler)->size)
		{
			idydb_sizing_max buffer_delimitation_point = (offset[1]);
			idydb_sizing_max buffer_offset = (((*handler)->size - offset[1]) % IDYDB_MAX_BUFFER_SIZE);
			if (buffer_offset == 0) buffer_offset = IDYDB_MAX_BUFFER_SIZE;
			unsigned short buffer_size = (unsigned short)buffer_offset;
			for (;;)
			{
				memset((*handler)->buffer, 0, (sizeof(char) * IDYDB_MAX_BUFFER_SIZE));
				if (fseek((*handler)->file_descriptor, ((*handler)->size - buffer_offset), SEEK_SET) != 0)
				{
					idydb_clear_values(handler);
					idydb_error_state(handler, 14);
					return IDYDB_ERROR;
				}
				if (fread((*handler)->buffer, buffer_size, 1, (*handler)->file_descriptor) != 1)
				{
					idydb_clear_values(handler);
					idydb_error_state(handler, 14);
					return IDYDB_ERROR;
				}
				if (fseek((*handler)->file_descriptor, (((*handler)->size - buffer_offset) + offset_sizing + additional_offset), SEEK_SET) != 0)
				{
					idydb_clear_values(handler);
					idydb_error_state(handler, 15);
					return IDYDB_ERROR;
				}
				if (fwrite((*handler)->buffer, buffer_size, 1, (*handler)->file_descriptor) != 1)
				{
					idydb_clear_values(handler);
					idydb_error_state(handler, 15);
					return IDYDB_ERROR;
				}
				if (((*handler)->size - buffer_offset) <= buffer_delimitation_point) break;
				buffer_size = IDYDB_MAX_BUFFER_SIZE;
				buffer_offset += buffer_size;
			}
			if (fseek((*handler)->file_descriptor, (((*handler)->size - buffer_offset) + offset_sizing + additional_offset), SEEK_SET) != 0)
			{
				idydb_clear_values(handler);
				idydb_error_state(handler, 14);
				return IDYDB_ERROR;
			}
		}
		(*handler)->size += offset_sizing;
		if (current_length[1] == 0) (*handler)->size += additional_offset;
		else row_count[0] -= 1;
	}
	else if (input_size < current_length[0] || (*handler)->value_type == IDYDB_NULL)
	{
		unsigned short offset_sizing = (unsigned short)(current_length[0] - (current_length[0] - input_size));
		if (row_count[0] == 1)
		{
			offset[3] = offset[1];
			offset[1] = offset[4];
		}
		idydb_sizing_max deletion_point[2] = {
			(offset[1] + IDYDB_SEGMENT_SIZE + current_length[0]),
			(offset[1] + IDYDB_SEGMENT_SIZE + offset_sizing),
		};
		if (offset[0] == offset[1]) deletion_point[0] += IDYDB_PARTITION_SIZE;

		if (input_size == 0 && (*handler)->value_type == IDYDB_NULL)
		{
			if (row_count[0] > 1)
			{
				if (offset[0] == offset[1])
				{
					deletion_point[1] -= (((deletion_point[1] - offset[0])) );
					deletion_point[1] += IDYDB_PARTITION_SIZE;
				}
				else
					deletion_point[1] -= (IDYDB_SEGMENT_SIZE);
			}
			else if (offset[3] == offset[0])
			{
				if (offset[0] == offset[1])
				{
					deletion_point[0] += IDYDB_PARTITION_SIZE;
					deletion_point[1] += IDYDB_PARTITION_SIZE;
				}
				else if (row_count[0] == 1)
					deletion_point[1] -= IDYDB_PARTITION_AND_SEGMENT;
				else
					deletion_point[1] -= IDYDB_PARTITION_SIZE;
			}
			else
				deletion_point[1] -= (deletion_point[1] - (deletion_point[1] - offset[1]));
		}
		else if (offset[0] == offset[1])
			deletion_point[1] += IDYDB_PARTITION_SIZE;

		unsigned short buffer_size = IDYDB_MAX_BUFFER_SIZE;
		idydb_sizing_max buffer_offset = 0;
		bool writable = (deletion_point[0] != (*handler)->size);
		while (writable)
		{
			if ((deletion_point[0] + buffer_offset + buffer_size) >= (*handler)->size)
			{
				buffer_size = (unsigned short)((*handler)->size - (deletion_point[0] + buffer_offset));
				writable = false;
				if (buffer_size == 0) break;
			}
			memset((*handler)->buffer, 0, (sizeof(char) * IDYDB_MAX_BUFFER_SIZE));
			if (fseek((*handler)->file_descriptor, (deletion_point[0] + buffer_offset), SEEK_SET) != 0)
			{
				idydb_clear_values(handler);
				idydb_error_state(handler, 14);
				return IDYDB_ERROR;
			}
			if (fread((*handler)->buffer, buffer_size, 1, (*handler)->file_descriptor) != 1)
			{
				idydb_clear_values(handler);
				idydb_error_state(handler, 14);
				return IDYDB_ERROR;
			}
			if (fseek((*handler)->file_descriptor, (deletion_point[1] + buffer_offset), SEEK_SET) != 0)
			{
				idydb_clear_values(handler);
				idydb_error_state(handler, 15);
				return IDYDB_ERROR;
			}
			if (fwrite((*handler)->buffer, buffer_size, 1, (*handler)->file_descriptor) != 1)
			{
				idydb_clear_values(handler);
				idydb_error_state(handler, 15);
				return IDYDB_ERROR;
			}
			buffer_offset += buffer_size;
		}
		(*handler)->size -= (current_length[0] - offset_sizing);
		if (input_size == 0 && (*handler)->value_type == IDYDB_NULL)
		{
			if (row_count[0] > 1) (*handler)->size -= IDYDB_SEGMENT_SIZE;
			else (*handler)->size -= IDYDB_PARTITION_AND_SEGMENT;
		}
		if (fflush((*handler)->file_descriptor) != 0 ||
		    idydb_platform::resize((*handler)->file_descriptor, (*handler)->size) != 0)
		{
			idydb_clear_values(handler);
			idydb_error_state(handler, 17);
			return IDYDB_CORRUPT;
		}
		row_count[0] -= 1;
		if (row_count[0] == 0 && input_size == 0 && (*handler)->value_type == IDYDB_NULL)
			removal = true;
	}
	else
		row_count[0] -= 1;

	if (offset[0] == offset[1]) offset[1] += IDYDB_PARTITION_SIZE;

	info_skip_offset.position = offset[0];
	info_row_count.position = (offset[0] + 2);
	info_row_count.size = row_count[0];
	if ((*handler)->value_type == IDYDB_NULL) info_row_count.size -= 1;

	if (((input_size == 0 && (*handler)->value_type != IDYDB_NULL) ? true : (input_size != 0)) && !removal)
	{
		info_skip_offset.use = true;
		if (row_count[0] == 0)
		{
			if (current_length[0] != 0 || current_length[1] == 1)
				info_skip_offset.use = false;
			else if (offset[0] != 0)
				skip_amount[0] = (unsigned short)(column_position - (skip_offset[0] + 1));
			else
				skip_amount[0] = (unsigned short)(column_position - 1);
		}
		info_skip_offset.size = skip_amount[0];

		info_row_position.use = true;
		info_row_position.position = (offset[1]);

		info_input_type.use = true;
		info_input_type.position = (offset[1] + 2);

		if ((*handler)->value_type == IDYDB_CHAR || (*handler)->value_type == IDYDB_VECTOR)
		{
			info_input_size.use = true;
			info_input_size.position = (offset[1] + 3);
			info_input_buffer.position = (offset[1] + 5);
		}
		else
		{
			info_input_buffer.position = (offset[1] + 3);
		}

		info_input_buffer.use = ((*handler)->value_type != IDYDB_BOOL);
		info_row_count.use = true;

		if (current_length[0] == 0 && current_length[1] == 0 && row_count[0] == 0 && (current_length[0] != input_size || (*handler)->value_type == IDYDB_BOOL))
		{
			offset[0] += (IDYDB_PARTITION_AND_SEGMENT + input_size);
			if (offset[0] != (*handler)->size)
			{
				if (fseek((*handler)->file_descriptor, offset[0], SEEK_SET) != 0)
				{
					idydb_clear_values(handler);
					idydb_error_state(handler, 14);
					return IDYDB_ERROR;
				}
				if (fread(&skip_amount[1], sizeof(short), 1, (*handler)->file_descriptor) != 1)
				{
					idydb_clear_values(handler);
					idydb_error_state(handler, 14);
					return IDYDB_ERROR;
				}
				if (skip_amount[1] == 1) skip_amount[0] = 0;
				else skip_amount[0] = (unsigned short)(skip_amount[1] - (skip_amount[0] + 1));
				if (fseek((*handler)->file_descriptor, offset[0], SEEK_SET) != 0)
				{
					idydb_clear_values(handler);
					idydb_error_state(handler, 15);
					return IDYDB_ERROR;
				}
				if (fwrite(&skip_amount[0], sizeof(short), 1, (*handler)->file_descriptor) != 1)
				{
					idydb_clear_values(handler);
					idydb_error_state(handler, 16);
					return IDYDB_ERROR;
				}
			}
		}
	}
	else if (offset[0] != (*handler)->size)
	{
		if (row_count[0] == 0)
		{
			info_skip_offset.use = true;
			if (fseek((*handler)->file_descriptor, offset[0], SEEK_SET) != 0)
			{
				idydb_clear_values(handler);
				idydb_error_state(handler, 14);
				return IDYDB_ERROR;
			}
			skip_amount[1] = skip_amount[0];
			if (fread(&skip_amount[0], 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short))
			{
				idydb_clear_values(handler);
				idydb_error_state(handler, 14);
				return IDYDB_ERROR;
			}
			skip_amount[0] += (unsigned short)(skip_amount[1] + 1);
			info_skip_offset.size = skip_amount[0];
		}
		else
			info_row_count.use = true;
	}

	if (info_skip_offset.use)
	{
		if (fseek((*handler)->file_descriptor, info_skip_offset.position, SEEK_SET) != 0)
		{
			idydb_clear_values(handler);
			idydb_error_state(handler, 15);
			return IDYDB_ERROR;
		}
		if (fwrite(&info_skip_offset.size, sizeof(short), 1, (*handler)->file_descriptor) != 1)
		{
			idydb_clear_values(handler);
			idydb_error_state(handler, 15);
			return IDYDB_ERROR;
		}
	}
	if (info_row_count.use)
	{
		if (fseek((*handler)->file_descriptor, info_row_count.position, SEEK_SET) != 0)
		{
			idydb_clear_values(handler);
			idydb_error_state(handler, 15);
			return IDYDB_ERROR;
		}
		if (fwrite(&info_row_count.size, sizeof(short), 1, (*handler)->file_descriptor) != 1)
		{
			idydb_clear_values(handler);
			idydb_error_state(handler, 15);
			return IDYDB_ERROR;
		}
	}
	if (info_row_position.use)
	{
		if (fseek((*handler)->file_descriptor, info_row_position.position, SEEK_SET) != 0)
		{
			idydb_clear_values(handler);
			idydb_error_state(handler, 15);
			return IDYDB_ERROR;
		}
		if (fwrite(&info_row_position.size, sizeof(short), 1, (*handler)->file_descriptor) != 1)
		{
			idydb_clear_values(handler);
			idydb_error_state(handler, 15);
			return IDYDB_ERROR;
		}
	}
	if (info_input_size.use)
	{
		if (fseek((*handler)->file_descriptor, info_input_size.position, SEEK_SET) != 0)
		{
			idydb_clear_values(handler);
			idydb_error_state(handler, 15);
			return IDYDB_ERROR;
		}
		if (fwrite(&info_input_size.size, sizeof(short), 1, (*handler)->file_descriptor) != 1)
		{
			idydb_clear_values(handler);
			idydb_error_state(handler, 15);
			return IDYDB_ERROR;
		}
	}
	if (info_input_type.use)
	{
		if (fseek((*handler)->file_descriptor, info_input_type.position, SEEK_SET) != 0)
		{
			idydb_clear_values(handler);
			idydb_error_state(handler, 15);
			return IDYDB_ERROR;
		}
		unsigned short input_type = 0;
		switch (info_input_type.size)
		{
		case IDYDB_INTEGER: input_type = IDYDB_READ_INT; break;
		case IDYDB_FLOAT:   input_type = IDYDB_READ_FLOAT; break;
		case IDYDB_CHAR:    input_type = IDYDB_READ_CHAR; break;
		case IDYDB_BOOL:    input_type = ((*handler)->value.bool_value ? IDYDB_READ_BOOL_TRUE : IDYDB_READ_BOOL_FALSE); break;
		case IDYDB_VECTOR:  input_type = IDYDB_READ_VECTOR; break;
		}
		if (fwrite(&input_type, 1, 1, (*handler)->file_descriptor) != 1)
		{
			idydb_clear_values(handler);
			idydb_error_state(handler, 15);
			return IDYDB_ERROR;
		}
	}
	if (info_input_buffer.use)
	{
		if (fseek((*handler)->file_descriptor, info_input_buffer.position, SEEK_SET) != 0)
		{
			idydb_clear_values(handler);
			idydb_error_state(handler, 15);
			return IDYDB_ERROR;
		}
		switch (info_input_type.size)
		{
		case IDYDB_INTEGER:
			if (fwrite(&(*handler)->value.int_value, sizeof(int), 1, (*handler)->file_descriptor) != 1)
			{
				idydb_clear_values(handler);
				idydb_error_state(handler, 15);
				return IDYDB_ERROR;
			}
			break;
		case IDYDB_FLOAT:
			if (fwrite(&(*handler)->value.float_value, sizeof(float), 1, (*handler)->file_descriptor) != 1)
			{
				idydb_clear_values(handler);
				idydb_error_state(handler, 15);
				return IDYDB_ERROR;
			}
			break;
		case IDYDB_CHAR:
			/* FIX: write payload including '\0' */
			if (fwrite((*handler)->value.char_value, 1, input_size_default, (*handler)->file_descriptor) != input_size_default)
			{
				idydb_clear_values(handler);
				idydb_error_state(handler, 15);
				return IDYDB_ERROR;
			}
			break;
		case IDYDB_VECTOR:
			if (fwrite((*handler)->vector_value, sizeof(float), (*handler)->vector_dims, (*handler)->file_descriptor) != (*handler)->vector_dims)
			{
				idydb_clear_values(handler);
				idydb_error_state(handler, 15);
				return IDYDB_ERROR;
			}
			break;
		}
	}

	if (fflush((*handler)->file_descriptor) != 0) {
		idydb_clear_values(handler);
		idydb_error_state(handler, 15);
		return IDYDB_ERROR;
	}

	/* capture staged type before clear_values resets it */
#ifdef CUWACUNU_CAMAHJUCUNU_DB_VERBOSE_DEBUG
	idydb_sizing_max dbg_size_after = (*handler)->size;
	long long dbg_delta = (long long)dbg_size_after - (long long)dbg_size_before;

	const char* dbg_op = "UPDATE";
	if (strcmp(dbg_before, dbg_after) == 0) dbg_op = "NOOP";
	else if (strncmp(dbg_before, "NULL", 4) == 0 && strncmp(dbg_after, "NULL", 4) != 0) dbg_op = "INSERT";
	else if (strncmp(dbg_before, "NULL", 4) != 0 && strncmp(dbg_after, "NULL", 4) == 0) dbg_op = "DELETE";
#endif

	idydb_clear_values(handler);

	(*handler)->dirty = true;

#ifdef CUWACUNU_CAMAHJUCUNU_DB_VERBOSE_DEBUG
	DB_DEBUGF(handler,
	          "cell(%llu,%llu) %s: %s -> %s (Δ%+lldB, size=%llu)",
	          (unsigned long long)dbg_col,
	          (unsigned long long)dbg_row,
	          dbg_op,
	          dbg_before,
	          dbg_after,
	          dbg_delta,
	          (unsigned long long)dbg_size_after);
#else
	DB_DEBUG(handler, "database mutated");
#endif

	return IDYDB_DONE;
}

/* ---------------- Vector math helpers ---------------- */

/* Query scans use a single bounded reader for both FILE and mmap handles. */
static int idydb_query_column_ok(idydb **handler, idydb_column_row_sizing column)
{
	(void)handler; /* Only BIG mode consults the handler's unsafe flag. */
	if (column == 0) return 0;
#ifdef IDYDB_ALLOW_UNSAFE
	if ((*handler)->unsafe) return 1;
#endif
	return (column - 1) <= IDYDB_COLUMN_POSITION_MAX;
}

static int idydb_query_read(idydb **handler, idydb_sizing_max offset, void* out, size_t length)
{
	return idydb_read_bytes_at(handler, offset, out, length) == IDYDB_DONE;
}

typedef idydb_scan_cursor idydb_query_cursor;
typedef idydb_record idydb_query_cell;

/* Translate the shared scanner's status convention for count-returning queries. */
static int idydb_query_next(idydb **handler, idydb_query_cursor* cursor, idydb_query_cell* cell)
{
	const unsigned char rc = idydb_scan_next(handler, cursor, cell);
	if (rc == IDYDB_DONE) return 1;
	if (rc == IDYDB_NULL) return 0;
	return -1;
}

static int idydb_query_vector_ok(const float* vector, unsigned short dims)
{
	if (!vector || dims == 0 || dims > IDYDB_MAX_VECTOR_DIM) return 0;
	for (unsigned short i = 0; i < dims; ++i) if (!isfinite(vector[i])) return 0;
	return 1;
}

static void idydb_query_results_reset(idydb_knn_result* results, unsigned short k)
{
	if (!results) return;
	for (unsigned short i = 0; i < k; ++i) { results[i].row = 0; results[i].score = -INFINITY; }
}

/* ---------------- Filter helpers ---------------- */

void* idydb_alloc(size_t bytes) { return malloc(bytes); }

void idydb_free(void* p) { if (p) free(p); }

void idydb_value_free(idydb_value* v)
{
	if (!v) return;
	if (v->type == IDYDB_CHAR) {
		if (v->as.s) free(v->as.s);
		v->as.s = NULL;
	} else if (v->type == IDYDB_VECTOR) {
		if (v->as.vec.v) free(v->as.vec.v);
		v->as.vec.v = NULL;
		v->as.vec.dims = 0;
	}
	v->type = IDYDB_NULL;
}

void idydb_values_free(idydb_value* values, size_t count)
{
	if (!values) return;
	for (size_t i = 0; i < count; ++i) idydb_value_free(&values[i]);
}

static inline int idydb_filter_cmp_int(int a, idydb_filter_op op, int b)
{
	switch (op) {
		case IDYDB_FILTER_OP_EQ:  return a == b;
		case IDYDB_FILTER_OP_NEQ: return a != b;
		case IDYDB_FILTER_OP_GT:  return a >  b;
		case IDYDB_FILTER_OP_GTE: return a >= b;
		case IDYDB_FILTER_OP_LT:  return a <  b;
		case IDYDB_FILTER_OP_LTE: return a <= b;
		default: return 0;
	}
}

static inline int idydb_filter_cmp_float(float a, idydb_filter_op op, float b)
{
	switch (op) {
		case IDYDB_FILTER_OP_EQ:  return a == b;
		case IDYDB_FILTER_OP_NEQ: return a != b;
		case IDYDB_FILTER_OP_GT:  return a >  b;
		case IDYDB_FILTER_OP_GTE: return a >= b;
		case IDYDB_FILTER_OP_LT:  return a <  b;
		case IDYDB_FILTER_OP_LTE: return a <= b;
		default: return 0;
	}
}

static inline int idydb_filter_cmp_bool(bool a, idydb_filter_op op, bool b)
{
	switch (op) {
		case IDYDB_FILTER_OP_EQ:  return a == b;
		case IDYDB_FILTER_OP_NEQ: return a != b;
		default: return 0;
	}
}

static int idydb_filter_term_ok(idydb **handler, const idydb_filter_term* term)
{
	if (!term || !idydb_query_column_ok(handler, term->column)) return 0;
	if (term->op < IDYDB_FILTER_OP_EQ || term->op > IDYDB_FILTER_OP_IS_NOT_NULL) return 0;
	if (term->op == IDYDB_FILTER_OP_IS_NULL || term->op == IDYDB_FILTER_OP_IS_NOT_NULL) return 1;
	if (term->type == IDYDB_NULL) return term->op == IDYDB_FILTER_OP_EQ || term->op == IDYDB_FILTER_OP_NEQ;
	if (term->type == IDYDB_INTEGER) return 1;
	if (term->type == IDYDB_FLOAT) return isfinite(term->value.f);
	if (term->type == IDYDB_CHAR)
		return term->value.s && (term->op == IDYDB_FILTER_OP_EQ || term->op == IDYDB_FILTER_OP_NEQ);
	if (term->type == IDYDB_BOOL)
		return term->op == IDYDB_FILTER_OP_EQ || term->op == IDYDB_FILTER_OP_NEQ;
	return 0;
}

static int idydb_filter_build_term_mask(idydb **handler,
                                       const idydb_filter_term* term,
                                       unsigned char* term_mask,
                                       size_t mask_len)
{
	if (!idydb_filter_term_ok(handler, term)) {
		idydb_error_statef(handler, 25, "Invalid query filter term");
		return 0;
	}
	idydb_filter_op op = term->op;
	if (term->type == IDYDB_NULL && op == IDYDB_FILTER_OP_EQ) op = IDYDB_FILTER_OP_IS_NULL;
	if (term->type == IDYDB_NULL && op == IDYDB_FILTER_OP_NEQ) op = IDYDB_FILTER_OP_IS_NOT_NULL;

	/* Missing cells are the database's NULL representation. */
	memset(term_mask, op == IDYDB_FILTER_OP_IS_NULL ? 1 : 0, mask_len);
	term_mask[0] = 0;
	idydb_query_cursor cursor = {};
	idydb_query_cell cell;
	int rc;
	while ((rc = idydb_query_next(handler, &cursor, &cell)) > 0) {
		if (cell.column != term->column || (size_t)cell.row >= mask_len) continue;
		unsigned char* match = &term_mask[(size_t)cell.row];
		if (op == IDYDB_FILTER_OP_IS_NULL) { *match = 0; continue; }
		if (op == IDYDB_FILTER_OP_IS_NOT_NULL) { *match = 1; continue; }
		if (cell.type == IDYDB_READ_BOOL_TRUE || cell.type == IDYDB_READ_BOOL_FALSE) {
			if (term->type == IDYDB_BOOL)
				*match = (unsigned char)idydb_filter_cmp_bool(cell.type == IDYDB_READ_BOOL_TRUE, op, term->value.b);
		} else if (cell.type == IDYDB_READ_INT && term->type == IDYDB_INTEGER) {
			int value;
			if (!idydb_query_read(handler, cell.data_offset, &value, sizeof(value))) return 0;
			*match = (unsigned char)idydb_filter_cmp_int(value, op, term->value.i);
		} else if (cell.type == IDYDB_READ_FLOAT && term->type == IDYDB_FLOAT) {
			float value;
			if (!idydb_query_read(handler, cell.data_offset, &value, sizeof(value))) return 0;
			*match = (unsigned char)idydb_filter_cmp_float(value, op, term->value.f);
		} else if (cell.type == IDYDB_READ_CHAR && term->type == IDYDB_CHAR) {
			const size_t wanted_length = strlen(term->value.s);
			int equal = wanted_length == (size_t)cell.field;
			size_t position = 0;
			while (equal && position < wanted_length) {
				char buffer[1024];
				size_t length = wanted_length - position;
				if (length > sizeof(buffer)) length = sizeof(buffer);
				if (!idydb_query_read(handler, cell.data_offset + position, buffer, length)) return 0;
				equal = memcmp(buffer, term->value.s + position, length) == 0;
				position += length;
			}
			*match = (unsigned char)(op == IDYDB_FILTER_OP_EQ ? equal : !equal);
		}
	}
	return rc == 0;
}

static int idydb_filter_build_allowed_mask(idydb **handler,
                                          const idydb_filter* filter,
                                          unsigned char* allowed,
                                          size_t allowed_len)
{
	memset(allowed, 1, allowed_len);
	allowed[0] = 0;
	if (!filter || filter->nterms == 0) return 1;
	if (!filter->terms) {
		idydb_error_statef(handler, 25, "Filter has terms count but no terms");
		return 0;
	}
	unsigned char* temporary = (unsigned char*)malloc(allowed_len);
	if (!temporary) { idydb_error_state(handler, 24); return 0; }
	for (size_t term = 0; term < filter->nterms; ++term) {
		if (!idydb_filter_build_term_mask(handler, &filter->terms[term], temporary, allowed_len)) {
			free(temporary);
			return 0;
		}
		for (size_t row = 0; row < allowed_len; ++row)
			allowed[row] = (unsigned char)(allowed[row] && temporary[row]);
	}
	free(temporary);
	return 1;
}

/* ---------------- Column scanning for kNN ---------------- */

typedef struct {
	idydb_column_row_sizing row;
	double score;
} idydb_query_candidate;

static int idydb_query_candidate_compare(const void* left, const void* right)
{
	const idydb_query_candidate* a = (const idydb_query_candidate*)left;
	const idydb_query_candidate* b = (const idydb_query_candidate*)right;
	if (!a->row) return b->row ? 1 : 0;
	if (!b->row) return -1;
	if (a->score != b->score) return a->score > b->score ? -1 : 1;
	return a->row < b->row ? -1 : a->row > b->row;
}

static int idydb_knn_search_vector_column_internal(idydb **handler,
                                   idydb_column_row_sizing vector_column,
                                   const float* query,
                                   unsigned short dims,
                                   unsigned short k,
                                   idydb_similarity_metric metric,
                                   const unsigned char* allowed,
                                   size_t allowed_len,
                                   idydb_knn_result* out_results)
{
	idydb_query_results_reset(out_results, k);
	if (!handler || !*handler || !(*handler)->configured || !out_results || k == 0 ||
		!idydb_query_vector_ok(query, dims) || (metric != IDYDB_SIM_COSINE && metric != IDYDB_SIM_L2)) {
		idydb_error_statef(handler, 8, "Invalid vector query arguments");
		return -1;
	}
	if (!idydb_query_column_ok(handler, vector_column)) { idydb_error_state(handler, 12); return -1; }
	idydb_query_candidate* candidates = (idydb_query_candidate*)calloc(k, sizeof(idydb_query_candidate));
	if (!candidates) { idydb_error_state(handler, 24); return -1; }
	for (unsigned short i = 0; i < k; ++i) candidates[i].score = -INFINITY;

	/* Double accumulators keep valid, finite float inputs from overflowing during scoring. */
	double query_norm_squared = 0.0;
	if (metric == IDYDB_SIM_COSINE)
		for (unsigned short i = 0; i < dims; ++i) query_norm_squared += (double)query[i] * query[i];
	double query_norm = sqrt(query_norm_squared);
	if (query_norm == 0.0) query_norm = 1.0;

	idydb_query_cursor cursor = {};
	idydb_query_cell cell;
	int rc;
	while ((rc = idydb_query_next(handler, &cursor, &cell)) > 0) {
		if (cell.column != vector_column || cell.type != IDYDB_READ_VECTOR || cell.field != dims) continue;
		if (allowed && ((size_t)cell.row >= allowed_len || !allowed[(size_t)cell.row])) continue;

		double dot = 0.0, squared_distance = 0.0, norm_squared = 0.0;
		for (size_t start = 0; start < dims; start += 256) {
			float values[256];
			size_t length = dims - start;
			if (length > 256) length = 256;
			if (!idydb_query_read(handler, cell.data_offset + start * sizeof(float), values, length * sizeof(float))) {
				free(candidates);
				return -1;
			}
			for (size_t component = 0; component < length; ++component) {
				const float value = values[component];
				if (!isfinite(value)) {
					idydb_error_statef(handler, 13, "Stored vector has a nonfinite component");
					free(candidates);
					return -1;
				}
				if (metric == IDYDB_SIM_COSINE) {
					dot += (double)query[start + component] * value;
					norm_squared += (double)value * value;
				} else {
					const double difference = (double)query[start + component] - value;
					squared_distance += difference * difference;
				}
			}
		}
		double score;
		if (metric == IDYDB_SIM_COSINE) {
			double norm = sqrt(norm_squared);
			if (norm == 0.0) norm = 1.0;
			double similarity = dot / (query_norm * norm);
			if (similarity > 1.0) similarity = 1.0;
			if (similarity < -1.0) similarity = -1.0;
			score = similarity;
		} else {
			score = -sqrt(squared_distance);
		}
		unsigned short worst = 0;
		for (unsigned short i = 1; i < k; ++i) {
			if (!candidates[worst].row) break;
			if (!candidates[i].row || candidates[i].score < candidates[worst].score ||
				(candidates[i].score == candidates[worst].score && candidates[i].row > candidates[worst].row)) worst = i;
		}
		if (!candidates[worst].row || score > candidates[worst].score ||
			(score == candidates[worst].score && cell.row < candidates[worst].row)) {
			candidates[worst].row = cell.row;
			candidates[worst].score = score;
		}
	}
	if (rc < 0) { free(candidates); return -1; }
	qsort(candidates, k, sizeof(idydb_query_candidate), idydb_query_candidate_compare);
	int count = 0;
	for (unsigned short i = 0; i < k; ++i) {
		if (!candidates[i].row) break;
		out_results[i].row = candidates[i].row;
		/* Rank with double precision; clamp scores beyond the public float range. */
		out_results[i].score = candidates[i].score < -(double)3.402823466e38F ? -3.402823466e38F : (float)candidates[i].score;
		++count;
	}
	free(candidates);
	return count;
}

int idydb_knn_search_vector_column(idydb **handler,
                                   idydb_column_row_sizing vector_column,
                                   const float* query,
                                   unsigned short dims,
                                   unsigned short k,
                                   idydb_similarity_metric metric,
                                   idydb_knn_result* out_results)
{
	return idydb_knn_search_vector_column_internal(handler, vector_column, query, dims, k, metric, NULL, 0, out_results);
}

int idydb_knn_search_vector_column_filtered(idydb **handler,
                                            idydb_column_row_sizing vector_column,
                                            const float* query,
                                            unsigned short dims,
                                            unsigned short k,
                                            idydb_similarity_metric metric,
                                            const idydb_filter* filter,
                                            idydb_knn_result* out_results)
{
	idydb_query_results_reset(out_results, k);
	if (!handler || !*handler || !(*handler)->configured || !out_results || k == 0 ||
		!idydb_query_vector_ok(query, dims) || (metric != IDYDB_SIM_COSINE && metric != IDYDB_SIM_L2)) {
		idydb_error_statef(handler, 8, "Invalid vector query arguments");
		return -1;
	}
	const size_t allowed_len = 65538; /* disk rows are uint16, exposed as 1-based indices */
	unsigned char* allowed = NULL;
	if (filter && filter->nterms > 0) {
		allowed = (unsigned char*)malloc(allowed_len);
		if (!allowed) { idydb_error_state(handler, 24); return -1; }
		if (!idydb_filter_build_allowed_mask(handler, filter, allowed, allowed_len)) {
			free(allowed);
			return -1;
		}
	}
	const int count = idydb_knn_search_vector_column_internal(handler, vector_column, query, dims, k, metric, allowed, allowed_len, out_results);
	free(allowed);
	return count;
}

/* ---------------- Utility: next row index ---------------- */

idydb_column_row_sizing idydb_column_next_row(idydb **handler, idydb_column_row_sizing column)
{
	if (!handler || !*handler || !(*handler)->configured) { idydb_error_state(handler, 8); return 0; }
	if (!idydb_query_column_ok(handler, column)) { idydb_error_state(handler, 12); return 0; }
	idydb_query_cursor cursor = {};
	idydb_query_cell cell;
	idydb_column_row_sizing maximum = 0;
	int rc;
	while ((rc = idydb_query_next(handler, &cursor, &cell)) > 0)
		if (cell.column == column && cell.row > maximum) maximum = cell.row;
	if (rc < 0) return 0;
	if (maximum >= (idydb_column_row_sizing)IDYDB_ROW_POSITION_MAX + 1) {
		idydb_error_state(handler, 12);
		return 0;
	}
	return maximum + 1;
}

/* ---------------- RAG helpers ---------------- */

void idydb_set_embedder(idydb **handler, idydb_embed_fn fn, void* user)
{
	if (!handler || !*handler) return;
	(*handler)->embedder = fn;
	(*handler)->embedder_user = user;
}

int idydb_rag_upsert_text(idydb **handler,
                          idydb_column_row_sizing text_column,
                          idydb_column_row_sizing vector_column,
                          idydb_column_row_sizing row,
                          const char* text,
                          const float* embedding,
                          unsigned short dims)
{
	/* Both cells are built in one private snapshot, then committed together. */
	if (!handler || !*handler || !(*handler)->configured || !text || !idydb_query_vector_ok(embedding, dims)) {
		idydb_error_statef(handler, 8, "Invalid RAG upsert arguments");
		return IDYDB_ERROR;
	}
	if (!idydb_query_column_ok(handler, text_column) || !idydb_query_column_ok(handler, vector_column) ||
		text_column == vector_column || row == 0 || row > IDYDB_MAX_POSITION) {
		idydb_error_state(handler, 12);
		return IDYDB_RANGE;
	}
	if (strlen(text) >= IDYDB_MAX_CHAR_LENGTH) { idydb_error_state(handler, 11); return IDYDB_ERROR; }
	idydb* staged = NULL;
	char* path = NULL;
	int rc = idydb_mutation_begin(handler, &staged, &path);
	if (rc == IDYDB_DONE) {
		idydb_clear_values(&staged);
		rc = idydb_insert_const_char(&staged, text_column, row, text);
		if (rc == IDYDB_DONE) rc = idydb_insert_vector(&staged, vector_column, row, embedding, dims);
		rc = idydb_mutation_finish(handler, &staged, &path, rc);
	}
	idydb_mutation_cleanup(&staged, path, (*handler)->directory_fd);
	idydb_clear_values(handler);
	return rc;
}

int idydb_rag_upsert_text_auto_embed(idydb **handler,
                                     idydb_column_row_sizing text_column,
                                     idydb_column_row_sizing vector_column,
                                     idydb_column_row_sizing row,
                                     const char* text)
{
	if (!handler || !*handler || !(*handler)->configured || !(*handler)->embedder || !text) {
		idydb_error_state(handler, 8);
		return IDYDB_ERROR;
	}
	float* vector = NULL;
	unsigned short dims = 0;
	const int rc = (*handler)->embedder(text, &vector, &dims, (*handler)->embedder_user);
	if (rc != 0 || !idydb_query_vector_ok(vector, dims)) {
		free(vector);
		idydb_error_statef(handler, 25, "Embedder failed or returned an invalid vector");
		return IDYDB_ERROR;
	}
	const int status = idydb_rag_upsert_text(handler, text_column, vector_column, row, text, vector, dims);
	free(vector);
	return status;
}

/* Query APIs return a count in [0,k], or -1 on error. Their output arrays must
 * have room for k entries, and CHAR/VECTOR deep copies belong to the caller. */
static void idydb_query_texts_free(char** texts, unsigned short k)
{
	if (!texts) return;
	for (unsigned short i = 0; i < k; ++i) { free(texts[i]); texts[i] = NULL; }
}

int idydb_rag_query_topk(idydb **handler,
                         idydb_column_row_sizing text_column,
                         idydb_column_row_sizing vector_column,
                         const float* query_embedding,
                         unsigned short dims,
                         unsigned short k,
                         idydb_similarity_metric metric,
                         idydb_knn_result* out_results,
                         char** out_texts)
{
	return idydb_rag_query_topk_filtered(handler, text_column, vector_column, query_embedding, dims, k, metric,
		NULL, out_results, out_texts);
}

int idydb_rag_query_topk_filtered(idydb **handler,
                                  idydb_column_row_sizing text_column,
                                  idydb_column_row_sizing vector_column,
                                  const float* query_embedding,
                                  unsigned short dims,
                                  unsigned short k,
                                  idydb_similarity_metric metric,
                                  const idydb_filter* filter,
                                  idydb_knn_result* out_results,
                                  char** out_texts)
{
	idydb_query_results_reset(out_results, k);
	if (out_texts) for (unsigned short i = 0; i < k; ++i) out_texts[i] = NULL;
	if (!handler || !*handler || !(*handler)->configured || !out_results || !out_texts) {
		idydb_error_state(handler, 8);
		return -1;
	}
	if (!idydb_query_column_ok(handler, text_column)) { idydb_error_state(handler, 12); return -1; }
	const int count = idydb_knn_search_vector_column_filtered(handler, vector_column, query_embedding, dims, k, metric,
		filter, out_results);
	if (count <= 0) return count;
	for (int i = 0; i < count; ++i) {
		const int rc = idydb_extract(handler, text_column, out_results[i].row);
		if (rc == IDYDB_NULL) continue;
		if (rc != IDYDB_DONE) goto failure;
		if (idydb_retrieved_type(handler) != IDYDB_CHAR) { idydb_clear_values(handler); continue; }
		const char* text = idydb_retrieve_char(handler);
		if (!text) { idydb_error_state(handler, 18); goto failure; }
		const size_t length = strlen(text);
		out_texts[i] = (char*)malloc(length + 1);
		if (!out_texts[i]) { idydb_error_state(handler, 24); goto failure; }
		memcpy(out_texts[i], text, length + 1);
		idydb_clear_values(handler);
	}
	return count;
failure:
	idydb_clear_values(handler);
	idydb_query_texts_free(out_texts, k);
	idydb_query_results_reset(out_results, k);
	return -1;
}

int idydb_rag_query_topk_with_metadata(idydb **handler,
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
                                       idydb_value* out_meta)
{
	idydb_query_results_reset(out_results, k);
	if (out_texts) for (unsigned short i = 0; i < k; ++i) out_texts[i] = NULL;
	if (!handler || !*handler || !(*handler)->configured || !out_results || !out_texts || k == 0 ||
		(meta_columns_count && (!meta_columns || !out_meta || meta_columns_count > SIZE_MAX / k / sizeof(idydb_value)))) {
		idydb_error_state(handler, 8);
		return -1;
	}
	const size_t metadata_count = (size_t)k * meta_columns_count;
	if (metadata_count) {
		memset(out_meta, 0, metadata_count * sizeof(idydb_value));
		for (size_t i = 0; i < metadata_count; ++i) out_meta[i].type = IDYDB_NULL;
	}
	for (size_t j = 0; j < meta_columns_count; ++j)
		if (!idydb_query_column_ok(handler, meta_columns[j])) { idydb_error_state(handler, 12); return -1; }

	const int count = idydb_rag_query_topk_filtered(handler, text_column, vector_column, query_embedding, dims, k, metric,
		filter, out_results, out_texts);
	if (count <= 0 || !metadata_count) return count;
	for (int i = 0; i < count; ++i) {
		for (size_t j = 0; j < meta_columns_count; ++j) {
			idydb_value* value = &out_meta[(size_t)i * meta_columns_count + j];
			const int rc = idydb_extract(handler, meta_columns[j], out_results[i].row);
			if (rc == IDYDB_NULL) continue;
			if (rc != IDYDB_DONE) goto failure;
			value->type = (unsigned char)idydb_retrieved_type(handler);
			switch (value->type) {
				case IDYDB_INTEGER: value->as.i = idydb_retrieve_int(handler); break;
				case IDYDB_FLOAT: value->as.f = idydb_retrieve_float(handler); break;
				case IDYDB_BOOL: value->as.b = idydb_retrieve_bool(handler); break;
				case IDYDB_CHAR: {
					const char* text = idydb_retrieve_char(handler);
					if (!text) { idydb_error_state(handler, 18); goto failure; }
					const size_t length = strlen(text);
					value->as.s = (char*)malloc(length + 1);
					if (!value->as.s) { idydb_error_state(handler, 24); goto failure; }
					memcpy(value->as.s, text, length + 1);
					break;
				}
				case IDYDB_VECTOR: {
					unsigned short vector_dims = 0;
					const float* vector = idydb_retrieve_vector(handler, &vector_dims);
					if (!vector || vector_dims == 0) { idydb_error_state(handler, 18); goto failure; }
					value->as.vec.dims = vector_dims;
					value->as.vec.v = (float*)malloc((size_t)vector_dims * sizeof(float));
					if (!value->as.vec.v) { idydb_error_state(handler, 24); goto failure; }
					memcpy(value->as.vec.v, vector, (size_t)vector_dims * sizeof(float));
					break;
				}
				default: value->type = IDYDB_NULL; break;
			}
			idydb_clear_values(handler);
		}
	}
	return count;
failure:
	idydb_clear_values(handler);
	idydb_query_texts_free(out_texts, k);
	idydb_values_free(out_meta, metadata_count);
	idydb_query_results_reset(out_results, k);
	return -1;
}

int idydb_rag_query_context(idydb **handler,
                            idydb_column_row_sizing text_column,
                            idydb_column_row_sizing vector_column,
                            const float* query_embedding,
                            unsigned short dims,
                            unsigned short k,
                            idydb_similarity_metric metric,
                            size_t max_chars,
                            char** out_context)
{
	return idydb_rag_query_context_filtered(handler, text_column, vector_column, query_embedding, dims, k, metric,
		NULL, max_chars, out_context);
}

int idydb_rag_query_context_filtered(idydb **handler,
                                     idydb_column_row_sizing text_column,
                                     idydb_column_row_sizing vector_column,
                                     const float* query_embedding,
                                     unsigned short dims,
                                     unsigned short k,
                                     idydb_similarity_metric metric,
                                     const idydb_filter* filter,
                                     size_t max_chars,
                                     char** out_context)
{
	if (!out_context) { idydb_error_state(handler, 8); return IDYDB_ERROR; }
	*out_context = NULL;
	if (!handler || !*handler || !(*handler)->configured || k == 0) {
		idydb_error_state(handler, 8);
		return IDYDB_ERROR;
	}
	idydb_knn_result* results = (idydb_knn_result*)calloc(k, sizeof(idydb_knn_result));
	char** texts = (char**)calloc(k, sizeof(char*));
	if (!results || !texts) {
		free(results); free(texts);
		idydb_error_state(handler, 24);
		return IDYDB_ERROR;
	}
	const int count = idydb_rag_query_topk_filtered(handler, text_column, vector_column, query_embedding, dims, k, metric,
		filter, results, texts);
	if (count <= 0) {
		free(results); free(texts);
		if (count < 0) return IDYDB_ERROR;
		*out_context = (char*)malloc(1);
		if (!*out_context) { idydb_error_state(handler, 24); return IDYDB_ERROR; }
		(*out_context)[0] = '\0';
		return IDYDB_DONE;
	}

	const char separator[] = "\n---\n";
	const size_t separator_length = sizeof(separator) - 1;
	size_t total = 0;
	int text_count = 0;
	for (int i = 0; i < count; ++i) {
		if (!texts[i]) continue;
		const size_t length = strlen(texts[i]);
		const size_t padding = text_count++ ? separator_length : 0;
		if (length > SIZE_MAX - 1 - padding || total > SIZE_MAX - 1 - padding - length) {
			idydb_query_texts_free(texts, k); free(results); free(texts);
			idydb_error_state(handler, 24);
			return IDYDB_ERROR;
		}
		total += padding + length;
	}
	if (max_chars && total > max_chars) total = max_chars;
	char* context = (char*)malloc(total + 1);
	if (!context) {
		idydb_query_texts_free(texts, k); free(results); free(texts);
		idydb_error_state(handler, 24);
		return IDYDB_ERROR;
	}
	size_t written = 0;
	text_count = 0;
	for (int i = 0; i < count && written < total; ++i) {
		if (!texts[i]) continue;
		if (text_count++) {
			size_t length = separator_length;
			if (length > total - written) length = total - written;
			memcpy(context + written, separator, length);
			written += length;
		}
		size_t length = strlen(texts[i]);
		if (length > total - written) length = total - written;
		memcpy(context + written, texts[i], length);
		written += length;
	}
	context[written] = '\0';
	idydb_query_texts_free(texts, k);
	free(results); free(texts);
	*out_context = context;
	return IDYDB_DONE;
}
/* ---------------- undefines mirroring original ---------------- */

#undef IDYDB_MAX_BUFFER_SIZE
#undef IDYDB_MAX_CHAR_LENGTH
#undef IDYDB_MAX_VECTOR_DIM
#undef IDYDB_MAX_ERR_SIZE
#undef IDYDB_COLUMN_POSITION_MAX
#undef IDYDB_ROW_POSITION_MAX
#undef IDYDB_SEGMENT_SIZE
#undef IDYDB_PARTITION_SIZE
#undef IDYDB_PARTITION_AND_SEGMENT
#undef IDYDB_READ_INT
#undef IDYDB_READ_FLOAT
#undef IDYDB_READ_CHAR
#undef IDYDB_READ_BOOL_TRUE
#undef IDYDB_READ_BOOL_FALSE
#undef IDYDB_READ_VECTOR
#undef IDYDB_MMAP_OK
#undef IDYDB_ALLOW_UNSAFE
#undef IDYDB_MMAP_ALLOWED

#endif /* idydb_c */
