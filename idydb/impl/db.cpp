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

#if defined(_WIN32)
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#ifndef F_OK
#define F_OK 0
#endif

/* POSIX-ish names used by the codebase */
#define access     _access
#define fileno     _fileno
#define fdopen     _fdopen
#define fsync      _commit
#define ftruncate  _chsize_s

/* 64-bit file offsets (needed for BIG mode on Windows) */
#define fseek _fseeki64
#define ftell _ftelli64

/* flock(2) compatibility (good enough for advisory lock usage in this project) */
#ifndef LOCK_SH
#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8
#endif

static int flock(int fd, int operation)
{
	HANDLE h = (HANDLE)_get_osfhandle(fd);
	if (h == INVALID_HANDLE_VALUE) { errno = EBADF; return -1; }

	OVERLAPPED ov;
	memset(&ov, 0, sizeof(ov));

	if (operation & LOCK_UN)
	{
		if (!UnlockFileEx(h, 0, MAXDWORD, MAXDWORD, &ov))
		{
			DWORD e = GetLastError();
			/* behave closer to POSIX: unlocking an unlocked file is not fatal here */
			if (e != ERROR_NOT_LOCKED) { errno = EINVAL; return -1; }
		}
		return 0;
	}

	DWORD flags = 0;
	if (operation & LOCK_EX) flags |= LOCKFILE_EXCLUSIVE_LOCK;
	if (operation & LOCK_NB) flags |= LOCKFILE_FAIL_IMMEDIATELY;

	if (!LockFileEx(h, flags, 0, MAXDWORD, MAXDWORD, &ov))
	{
		DWORD e = GetLastError();
		if (e == ERROR_LOCK_VIOLATION) errno = EWOULDBLOCK;
		else errno = EINVAL;
		return -1;
	}
	return 0;
}

#else /* POSIX */

#include <unistd.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>

#ifdef __linux__
#include <sys/syscall.h>
#endif

#endif /* _WIN32 */

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>

#include "db.h"

/* IdyDB database operations (internal) */
static int idydb_new(idydb **handler);
static void idydb_destroy(idydb **handler);
static int idydb_connection_setup(idydb **handler, const char *filename, int flags);
static int idydb_connection_setup_stream(idydb **handler, FILE* stream, int flags);
static char *idydb_get_err_message(idydb **handler);
static unsigned char idydb_read_at(idydb **handler, idydb_column_row_sizing column_position, idydb_column_row_sizing row_position);
static void idydb_clear_values(idydb **handler);
static unsigned char idydb_insert_at(idydb **handler, idydb_column_row_sizing column_position, idydb_column_row_sizing row_position);
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

union idydb_read_mmap_response;
static union idydb_read_mmap_response idydb_read_mmap(unsigned int position, unsigned char size, void *mmapped_char);

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
typedef unsigned short idydb_sizing_max;
#elif IDYDB_SIZING_MODE == IDYDB_SIZING_MODE_SMALL
#define IDYDB_COLUMN_POSITION_MAX 0x00FF
#define IDYDB_ROW_POSITION_MAX 0x00FF
typedef unsigned int idydb_size_selection_type;
typedef unsigned int idydb_sizing_max;
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

	int ok = PKCS5_PBKDF2_HMAC(passphrase,
	                          (int)strlen(passphrase),
	                          salt,
	                          IDYDB_ENC_SALT_LEN,
	                          (int)iter,
	                          EVP_sha256(),
	                          IDYDB_ENC_KEY_LEN,
	                          out_key);
	return ok == 1;
}

/* ---------------- Secure plaintext working storage ----------------
 * Goal: do NOT write plaintext to a filesystem-backed temp file.
 *
 * Linux: memfd_create (anonymous RAM-backed FD).
 * Fallback: shm_open + shm_unlink (usually tmpfs-backed; anonymous after unlink).
 */
#ifdef __linux__
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif
static int idydb_memfd_create_compat(const char* name, unsigned int flags)
{
#ifdef SYS_memfd_create
	return (int)syscall(SYS_memfd_create, name, flags);
#else
	errno = ENOSYS;
	return -1;
#endif
}
#endif

static FILE* idydb_secure_plain_stream(const char** out_kind)
{
    if (out_kind) *out_kind = "unknown";

#if defined(_WIN32)
    {
        /* Best-effort secure temp file on Windows */
        wchar_t tmpPath[MAX_PATH];
        DWORD n = GetTempPathW(MAX_PATH, tmpPath);
        if (n == 0 || n > MAX_PATH) return NULL;

        wchar_t tmpFile[MAX_PATH];
        if (GetTempFileNameW(tmpPath, L"idy", 0, tmpFile) == 0) return NULL;

        HANDLE h = CreateFileW(tmpFile,
                               GENERIC_READ | GENERIC_WRITE,
                               0, /* no sharing */
                               NULL,
                               CREATE_ALWAYS,
                               FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE,
                               NULL);
        if (h == INVALID_HANDLE_VALUE) {
            DeleteFileW(tmpFile);
            return NULL;
        }

        int fd = _open_osfhandle((intptr_t)h, _O_RDWR | _O_BINARY);
        if (fd < 0) {
            CloseHandle(h);
            return NULL;
        }

        FILE* f = _fdopen(fd, "w+b");
        if (!f) {
            _close(fd); /* closes underlying handle too */
            return NULL;
        }

        if (out_kind) *out_kind = "win32-temp-delete-on-close";
        return f;
    }
#else
  #ifdef __linux__
    {
        int fd = idydb_memfd_create_compat("idydb_plain", MFD_CLOEXEC);
        if (fd >= 0) {
            (void)fchmod(fd, 0600);
            FILE* f = fdopen(fd, "w+b");
            if (!f) { close(fd); return NULL; }
            if (out_kind) *out_kind = "memfd";
            return f;
        }
    }
  #endif

    /* POSIX fallback: shm_open + shm_unlink */
    {
        unsigned char rnd[16];
        if (RAND_bytes(rnd, sizeof(rnd)) != 1) return NULL;

        char name[64];
        snprintf(name, sizeof(name),
                 "/idydb_%02x%02x%02x%02x%02x%02x%02x%02x",
                 rnd[0], rnd[1], rnd[2], rnd[3], rnd[4], rnd[5], rnd[6], rnd[7]);

        int fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0) return NULL;

        (void)shm_unlink(name);

        FILE* f = fdopen(fd, "w+b");
        if (!f) { close(fd); return NULL; }
        if (out_kind) *out_kind = "shm";
        return f;
    }
#endif
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
	fseek(in, 0L, SEEK_END);
	long long total_sz = ftell(in);
	if (total_sz < 0) return 0;
	fseek(in, 0L, SEEK_SET);

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

	fseek(in, IDYDB_ENC_HDR_LEN, SEEK_SET);

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

	if (ok && plaintext_len != written) ok = 0;

	fflush(out_plain);
	fseek(out_plain, 0L, SEEK_SET);
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

	fflush(plain);
	long long cur = ftell(plain);
	if (cur < 0) cur = 0;
	fseek(plain, 0L, SEEK_END);
	long long plen_long = ftell(plain);
	if (plen_long < 0) return 0;
	uint64_t plaintext_len = (uint64_t)plen_long;
	fseek(plain, 0L, SEEK_SET);

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
	fflush(out);
	if (ftruncate(fileno(out), 0) != 0) return 0;

	fseek(out, 0L, SEEK_SET);
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

	while (ok) {
		size_t n = fread(inbuf, 1, sizeof(inbuf), plain);
		if (n == 0) break;
		int outl = 0;
		if (EVP_EncryptUpdate(ctx, outbuf, &outl, inbuf, (int)n) != 1) { ok = 0; break; }
		if (outl > 0) {
			if (fwrite(outbuf, 1, (size_t)outl, out) != (size_t)outl) { ok = 0; break; }
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

	if (!ok) return 0;

	/* write tag into header */
	fseek(out, 52, SEEK_SET);
	if (fwrite(tag, 1, IDYDB_ENC_TAG_LEN, out) != IDYDB_ENC_TAG_LEN) return 0;

	fflush(out);
	fsync(fileno(out));

	fseek(plain, cur, SEEK_SET);
	return 1;
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
#if defined(_WIN32)
	HANDLE win32_mmap_handle; /* CreateFileMapping handle if using MapViewOfFile */
#endif
#endif

	idydb_embed_fn embedder;
	void* embedder_user;

	/* encryption runtime state */
	bool encryption_enabled;
	bool dirty;

	FILE* backing_descriptor; /* locked file handle for encrypted backing (or NULL) */
	char* backing_filename;

	unsigned char enc_salt[IDYDB_ENC_SALT_LEN];
	uint32_t enc_iter;
	unsigned char enc_key[IDYDB_ENC_KEY_LEN];
	bool enc_key_set;

	/* debug: where plaintext lives when encrypted mode is enabled */
	const char* plain_storage_kind; /* "memfd" / "shm" / NULL */

} idydb;

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

/* ---------------- mmap helper ---------------- */

#ifdef IDYDB_MMAP_OK
union idydb_read_mmap_response
{
	int integer;
	short short_integer;
	float floating_point;
	unsigned char bytes[4];
};

static union idydb_read_mmap_response idydb_read_mmap(unsigned int position, unsigned char size, void *mmapped_char)
{
	union idydb_read_mmap_response value;
	memset(value.bytes, 0, sizeof(value.bytes));
	unsigned char byte_position = 0;
	for (unsigned int i = position; i < (size + position); i++)
		value.bytes[byte_position++] = (char)(((char *)mmapped_char)[i]);
	return value;
}
#endif

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
	(*handler)->value_retrieved = false;
	(*handler)->file_descriptor = NULL;
	(*handler)->vector_value = NULL;
	(*handler)->vector_dims = 0;
	(*handler)->embedder = NULL;
	(*handler)->embedder_user = NULL;

	(*handler)->encryption_enabled = false;
	(*handler)->dirty = false;
	(*handler)->backing_descriptor = NULL;
	(*handler)->backing_filename = NULL;
	memset((*handler)->enc_salt, 0, sizeof((*handler)->enc_salt));
	(*handler)->enc_iter = 0;
	memset((*handler)->enc_key, 0, sizeof((*handler)->enc_key));
	(*handler)->enc_key_set = false;
	(*handler)->plain_storage_kind = NULL;

#ifdef IDYDB_MMAP_OK
#if defined(_WIN32)
	(*handler)->win32_mmap_handle = NULL;
#endif
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

	if ((*handler)->configured)
	{
		(*handler)->configured = false;
		if ((*handler)->file_descriptor != NULL)
		{
			flock(fileno((*handler)->file_descriptor), LOCK_UN);
			fclose((*handler)->file_descriptor);
			(*handler)->file_descriptor = NULL;
		}
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
	if ((*handler)->enc_key_set)
	{
		OPENSSL_cleanse((*handler)->enc_key, sizeof((*handler)->enc_key));
		(*handler)->enc_key_set = false;
	}

	/* buffer teardown (malloc vs mmap) */
#ifdef IDYDB_MMAP_OK
	if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
	{
#if defined(_WIN32)
		if ((*handler)->buffer) {
			UnmapViewOfFile((*handler)->buffer);
			(*handler)->buffer = NULL;
		}
		if ((*handler)->win32_mmap_handle) {
			CloseHandle((*handler)->win32_mmap_handle);
			(*handler)->win32_mmap_handle = NULL;
		}
#else
		if ((*handler)->buffer != NULL && (*handler)->buffer != MAP_FAILED)
			munmap((*handler)->buffer, (size_t)(*handler)->size);
		(*handler)->buffer = NULL;
#endif
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

static inline const idydb_sizing_max idydb_max_size()
{
	idydb_sizing_max insertion_area[2] = {0, 0};
	insertion_area[0] = IDYDB_COLUMN_POSITION_MAX;
	insertion_area[0] *= IDYDB_ROW_POSITION_MAX;
	insertion_area[0] *= (IDYDB_MAX_CHAR_LENGTH - 1);
	if (IDYDB_ROW_POSITION_MAX > 1)
	{
		insertion_area[1] = IDYDB_COLUMN_POSITION_MAX;
		insertion_area[1] *= IDYDB_ROW_POSITION_MAX;
		insertion_area[1] *= IDYDB_SEGMENT_SIZE;
	}
	return (insertion_area[0] + insertion_area[1] + (IDYDB_COLUMN_POSITION_MAX * IDYDB_PARTITION_AND_SEGMENT));
}

static int idydb_connection_setup_stream(idydb **handler, FILE* stream, int flags)
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

	fseek((*handler)->file_descriptor, 0L, SEEK_END);
	(*handler)->size = (idydb_sizing_max)ftell((*handler)->file_descriptor);
	fseek((*handler)->file_descriptor, 0L, SEEK_SET);

	(*handler)->configured = true;

	int op = LOCK_NB | (((flags & IDYDB_READONLY) == IDYDB_READONLY) ? LOCK_SH : LOCK_EX);
	if (flock(fileno((*handler)->file_descriptor), op) != 0)
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

	(*handler)->buffer = malloc((sizeof(char) * IDYDB_MAX_BUFFER_SIZE));
	if ((*handler)->buffer == NULL)
	{
		idydb_error_state(handler, 24);
		return IDYDB_ERROR;
	}
	return IDYDB_SUCCESS;
}

static int idydb_connection_setup(idydb **handler, const char *filename, int flags)
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

	bool file_exists = true;
	if (access(filename, F_OK) != 0)
	{
		file_exists = false;
		if ((*handler)->read_only == IDYDB_READONLY)
		{
			if ((flags & IDYDB_CREATE) == 0)
			{
				idydb_error_state(handler, 4);
				return IDYDB_NOT_FOUND;
			}
		}
	}

	const char* mode =
		(((flags & IDYDB_READONLY) == IDYDB_READONLY)
			? "rb"
			: ((((flags & IDYDB_CREATE) == IDYDB_CREATE) && !file_exists) ? "w+b" : "r+b"));
	(*handler)->file_descriptor = fopen(filename, mode);

	if ((*handler)->file_descriptor == NULL)
	{
		idydb_error_state(handler, 5);
		return IDYDB_PERM;
	}
	else
	{
		fseek((*handler)->file_descriptor, 0L, SEEK_END);
		(*handler)->size = ftell((*handler)->file_descriptor);
		fseek((*handler)->file_descriptor, 0L, SEEK_SET);
	}

	(*handler)->configured = true;

	int op = LOCK_NB | (((flags & IDYDB_READONLY) == IDYDB_READONLY) ? LOCK_SH : LOCK_EX);
	if (flock(fileno((*handler)->file_descriptor), op) != 0)
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

#ifdef IDYDB_MMAP_OK
	if ((*handler)->read_only == IDYDB_READONLY)
	{
		if ((*handler)->size <= IDYDB_MMAP_MAX_SIZE && (*handler)->size > 0)
		{
#if defined(_WIN32)
			HANDLE hFile = (HANDLE)_get_osfhandle(fileno((*handler)->file_descriptor));
			if (hFile != INVALID_HANDLE_VALUE)
			{
				HANDLE hMap = CreateFileMappingW(hFile, NULL, PAGE_READONLY, 0, 0, NULL);
				if (hMap)
				{
					void* view = MapViewOfFile(hMap, FILE_MAP_READ, 0, 0, 0);
					if (view)
					{
						(*handler)->buffer = view;
						(*handler)->win32_mmap_handle = hMap;
						(*handler)->read_only = IDYDB_READONLY_MMAPPED;
					}
					else
					{
						CloseHandle(hMap);
						(*handler)->buffer = malloc((sizeof(char) * IDYDB_MAX_BUFFER_SIZE));
					}
				}
				else
				{
					(*handler)->buffer = malloc((sizeof(char) * IDYDB_MAX_BUFFER_SIZE));
				}
			}
			else
			{
				(*handler)->buffer = malloc((sizeof(char) * IDYDB_MAX_BUFFER_SIZE));
			}
#else
			(*handler)->buffer = mmap(NULL, (*handler)->size, PROT_READ, MAP_PRIVATE, fileno((*handler)->file_descriptor), 0);
			if ((*handler)->buffer != MAP_FAILED)
				(*handler)->read_only = IDYDB_READONLY_MMAPPED;
			else
				(*handler)->buffer = malloc((sizeof(char) * IDYDB_MAX_BUFFER_SIZE));
#endif
		}
		else
			(*handler)->buffer = malloc((sizeof(char) * IDYDB_MAX_BUFFER_SIZE));
	}
	else
#endif
		(*handler)->buffer = malloc((sizeof(char) * IDYDB_MAX_BUFFER_SIZE));

	if ((*handler)->buffer == NULL)
	{
		idydb_error_state(handler, 24);
		return IDYDB_ERROR;
	}

	return IDYDB_SUCCESS;
}

static char *idydb_get_err_message(idydb **handler)
{
	if (*handler == NULL)
		return (char *)"This handler failed to be setup\0";
	return (*handler)->err_message;
}

/* ---------------- Public open/close (runtime encryption) ---------------- */

int idydb_open_with_options(const char *filename, idydb **handler, const idydb_open_options* options)
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
		idydb_error_state(handler, 27);
		DB_DEBUGF(handler, "encrypted open refused: passphrase is NULL (file=\"%s\")", filename);
		return IDYDB_ERROR;
	}

	bool file_exists = (access(filename, F_OK) == 0);
	if (!file_exists && ((flags & IDYDB_CREATE) == 0))
	{
		idydb_error_state(handler, 4);
		return IDYDB_NOT_FOUND;
	}

	/* open + lock backing file */
	const bool ro = ((flags & IDYDB_READONLY) == IDYDB_READONLY);

	FILE* backing = fopen(filename,
		(ro ? "rb" : (file_exists ? "rb+" : "wb+")));

	if (!backing)
	{
		idydb_error_state(handler, 5);
		return IDYDB_PERM;
	}

	int op = LOCK_NB | (ro ? LOCK_SH : LOCK_EX);
	if (flock(fileno(backing), op) != 0)
	{
		fclose(backing);
		idydb_error_state(handler, 6);
		return IDYDB_BUSY;
	}

	(*handler)->encryption_enabled = true;
	(*handler)->backing_descriptor = backing;
	(*handler)->backing_filename = (char*)malloc(strlen(filename) + 1);
	if ((*handler)->backing_filename) strcpy((*handler)->backing_filename, filename);
	(*handler)->dirty = false;

	const char* kind = NULL;
	FILE* plain = idydb_secure_plain_stream(&kind);
	if (!plain)
	{
		idydb_error_state(handler, 30);
		DB_DEBUGF(handler, "failed to create secure in-memory plaintext working storage (backing=\"%s\")", filename);
		return IDYDB_ERROR;
	}
	(*handler)->plain_storage_kind = kind;

	DB_DEBUGF(handler, "opened ENCRYPTED-AT-REST db backing=\"%s\" ro=%s exists=%s working_plain=%s",
	          filename, (ro ? "yes" : "no"), (file_exists ? "yes" : "no"), (kind ? kind : "unknown"));

	/* detect encrypted header */
	fseek(backing, 0L, SEEK_END);
	long long bsz = ftell(backing);
	fseek(backing, 0L, SEEK_SET);

	bool is_enc = false;
	if (bsz >= (long)IDYDB_ENC_MAGIC_LEN)
	{
		unsigned char magic[IDYDB_ENC_MAGIC_LEN];
		size_t rr = fread(magic, 1, IDYDB_ENC_MAGIC_LEN, backing);
		fseek(backing, 0L, SEEK_SET);
		if (rr == IDYDB_ENC_MAGIC_LEN && memcmp(magic, IDYDB_ENC_MAGIC, IDYDB_ENC_MAGIC_LEN) == 0) is_enc = true;
	}

	/* IMPORTANT: encrypted-at-rest should not silently accept a plaintext backing in READONLY mode */
	if (!is_enc && ro && bsz > 0)
	{
		idydb_error_state(handler, 31);
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
			idydb_error_state(handler, 28);
			DB_DEBUGF(handler, "decrypt FAILED (wrong passphrase, tampered file, or unsupported params)");
			fclose(plain);
			return IDYDB_ERROR;
		}
		(*handler)->enc_iter = iter;
		(*handler)->enc_key_set = true;

		fseek(plain, 0L, SEEK_END);
		long long psz = ftell(plain);
		fseek(plain, 0L, SEEK_SET);
		DB_DEBUGF(handler, "decrypt OK -> plaintext bytes=%ld pbkdf2_iter=%u", psz, iter);
	}
	else
	{
		/* plaintext backing (migration) */
		if (bsz > 0)
		{
			DB_DEBUGF(handler, "PLAINTEXT backing detected; copying into working plaintext stream (migration)");
			unsigned char buf[16 * 1024];
			while (1) {
				size_t n = fread(buf, 1, sizeof(buf), backing);
				if (n == 0) break;
				if (fwrite(buf, 1, n, plain) != n) { idydb_error_state(handler, 26); fclose(plain); return IDYDB_ERROR; }
			}
			fflush(plain);
			fseek(plain, 0L, SEEK_SET);
			fseek(backing, 0L, SEEK_SET);
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
	int setup_rc = idydb_connection_setup_stream(handler, plain, flags);
	if (setup_rc != IDYDB_SUCCESS)
	{
		fclose(plain);
		return setup_rc;
	}

	idydb_clear_values(handler);
	DB_DEBUGF(handler, "ready: db opened against secure working plaintext stream kind=%s", (kind ? kind : "unknown"));
	return IDYDB_SUCCESS;
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

int idydb_open_encrypted(const char *filename, idydb **handler, int flags, const char* passphrase)
{
	idydb_open_options opt;
	opt.flags = flags;
	opt.encrypted_at_rest = true;
	opt.passphrase = passphrase;
	opt.pbkdf2_iter = 0;
	return idydb_open_with_options(filename, handler, &opt);
}

int idydb_close(idydb **handler)
{
	if (!handler || !*handler) return IDYDB_DONE;

	/* If encrypted-at-rest and dirty and writable, writeback ciphertext */
	if ((*handler)->encryption_enabled &&
	    (*handler)->enc_key_set &&
	    (*handler)->backing_descriptor != NULL &&
	    (*handler)->read_only == IDYDB_READ_AND_WRITE &&
	    (*handler)->dirty)
	{
		DB_DEBUGF(handler, "close: encrypting writeback -> backing=\"%s\" pbkdf2_iter=%u",
		          ((*handler)->backing_filename ? (*handler)->backing_filename : "(unknown)"),
		          (*handler)->enc_iter);

		if (!idydb_crypto_encrypt_stream_to_locked_file((*handler)->file_descriptor,
		                                                (*handler)->backing_descriptor,
		                                                (*handler)->enc_salt,
		                                                (*handler)->enc_iter,
		                                                (*handler)->enc_key))
		{
			idydb_error_state(handler, 29);
			DB_DEBUGF(handler, "close: writeback FAILED (backing not updated safely)");
			idydb_destroy(handler);
			free(*handler);
			*handler = NULL;
			return IDYDB_ERROR;
		}
		DB_DEBUGF(handler, "close: writeback OK");
	}
	else
	{
		DB_DEBUGF(handler, "close: no writeback (enc=%s dirty=%s mode=%s)",
		          ((*handler)->encryption_enabled ? "yes" : "no"),
		          ((*handler)->dirty ? "yes" : "no"),
		          idydb_ro_str((*handler)->read_only));
	}

	idydb_destroy(handler);
	free(*handler);
	*handler = NULL;
	return IDYDB_DONE;
}

/* ---------------- Public API thin wrappers ---------------- */

char *idydb_errmsg(idydb **handler) { return idydb_get_err_message(handler); }
int idydb_extract(idydb **handler, idydb_column_row_sizing c, idydb_column_row_sizing r) { return idydb_read_at(handler, c, r); }
int idydb_retrieved_type(idydb **handler) { return idydb_retrieve_value_type(handler); }

/* ---------------- insert value staging ---------------- */

static unsigned char idydb_insert_value_int(idydb **handler, int set_value)
{
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
	if (!(*handler)->configured) { idydb_error_state(handler, 8); return IDYDB_ERROR; }
	if ((*handler)->read_only != IDYDB_READ_AND_WRITE) { idydb_error_state(handler, 9); return IDYDB_READONLY; }
	if ((*handler)->value_type != IDYDB_NULL && !(*handler)->value_retrieved) { idydb_error_state(handler, 10); return IDYDB_ERROR; }
	idydb_clear_values(handler);
	(*handler)->value_type = IDYDB_CHAR;
	strncpy((*handler)->value.char_value, set_value, sizeof((*handler)->value.char_value));
	return IDYDB_DONE;
}

static unsigned char idydb_insert_value_bool(idydb **handler, bool set_value)
{
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
	if (!(*handler)->configured) { idydb_error_state(handler, 8); return IDYDB_ERROR; }
	if ((*handler)->read_only != IDYDB_READ_AND_WRITE) { idydb_error_state(handler, 9); return IDYDB_READONLY; }
	if ((*handler)->value_type != IDYDB_NULL && !(*handler)->value_retrieved) { idydb_error_state(handler, 10); return IDYDB_ERROR; }
	if (dims == 0 || dims > IDYDB_MAX_VECTOR_DIM) { idydb_error_state(handler, 11); return IDYDB_ERROR; }
	idydb_clear_values(handler);
	(*handler)->value_type  = IDYDB_VECTOR;
	(*handler)->vector_dims = dims;
	(*handler)->vector_value = (float*)malloc(sizeof(float) * (size_t)dims);
	if (!(*handler)->vector_value) { idydb_error_state(handler, 24); return IDYDB_ERROR; }
	memcpy((*handler)->vector_value, data, sizeof(float) * (size_t)dims);
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
	unsigned int value_length = (unsigned int)strlen(value);
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
	unsigned int value_length = (unsigned int)strlen(value);
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
	idydb_insert_reset(handler);
	return idydb_insert_at(handler, c, r);
}

/* ---------------- retrieve staged value ---------------- */

static int idydb_retrieve_value_int(idydb **handler) { return ((*handler)->value_type == IDYDB_INTEGER) ? (*handler)->value.int_value : 0; }
static float idydb_retrieve_value_float(idydb **handler) { return ((*handler)->value_type == IDYDB_FLOAT) ? (*handler)->value.float_value : 0.0f; }
static char *idydb_retrieve_value_char(idydb **handler) { return ((*handler)->value_type == IDYDB_CHAR) ? (*handler)->value.char_value : NULL; }
static bool idydb_retrieve_value_bool(idydb **handler) { return ((*handler)->value_type == IDYDB_BOOL) ? (*handler)->value.bool_value : false; }

static const float* idydb_retrieve_value_vector(idydb **handler, unsigned short* out_dims)
{
	if ((*handler)->value_type == IDYDB_VECTOR) {
		if (out_dims) *out_dims = (*handler)->vector_dims;
		return (*handler)->vector_value;
	}
	if (out_dims) *out_dims = 0;
	return NULL;
}

static unsigned char idydb_retrieve_value_type(idydb **handler) { return (*handler)->value_type; }

int idydb_retrieve_int(idydb **handler) { return idydb_retrieve_value_int(handler); }
float idydb_retrieve_float(idydb **handler) { return idydb_retrieve_value_float(handler); }
char *idydb_retrieve_char(idydb **handler) { return idydb_retrieve_value_char(handler); }
bool idydb_retrieve_bool(idydb **handler) { return idydb_retrieve_value_bool(handler); }
const float* idydb_retrieve_vector(idydb **handler, unsigned short* out_dims) { return idydb_retrieve_value_vector(handler, out_dims); }

/* ---------------- read value at (column,row) ---------------- */
/* This function is your original logic (unchanged). */

static unsigned char idydb_read_at(idydb **handler, idydb_column_row_sizing column_position, idydb_column_row_sizing row_position)
{
	if (!(*handler)->configured)
	{
		idydb_error_state(handler, 8);
		return IDYDB_ERROR;
	}
	idydb_clear_values(handler);
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
	row_position -= 1;
	bool store_response = false;
	idydb_sizing_max offset = 0;
	idydb_size_selection_type skip_offset = 0;
	unsigned char read_length = IDYDB_PARTITION_AND_SEGMENT;
	unsigned short row_count = 0;
	for (;;)
	{
		if ((offset + read_length) > (*handler)->size)
		{
			if ((offset + read_length) > ((*handler)->size + read_length))
			{
				idydb_error_state(handler, 13);
				return IDYDB_CORRUPT;
			}
			break;
		}
#ifdef IDYDB_MMAP_OK
		if ((*handler)->read_only != IDYDB_READONLY_MMAPPED)
		{
#endif
			fseek((*handler)->file_descriptor, offset, SEEK_SET);
			offset += read_length;
#ifdef IDYDB_MMAP_OK
		}
#endif
		if (read_length == IDYDB_PARTITION_AND_SEGMENT)
		{
			unsigned short skip_amount;
#ifdef IDYDB_MMAP_OK
			if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
				skip_amount = (unsigned short)idydb_read_mmap(offset, sizeof(short), (*handler)->buffer).integer;
			else
#endif
				if (fread(&skip_amount, 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short))
			{
				idydb_error_state(handler, 14);
				return IDYDB_ERROR;
			}
			skip_offset += skip_amount;
			if (skip_offset > IDYDB_COLUMN_POSITION_MAX)
			{
#ifdef IDYDB_ALLOW_UNSAFE
				if (!(*handler)->unsafe)
				{
#endif
					idydb_error_state(handler, 22);
					return IDYDB_RANGE;
#ifdef IDYDB_ALLOW_UNSAFE
				}
#endif
			}
			skip_offset += 1;
			if (skip_offset > column_position)
				return IDYDB_NULL;
#ifdef IDYDB_MMAP_OK
			if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
				row_count = (unsigned short)idydb_read_mmap((offset + sizeof(short)), sizeof(short), (*handler)->buffer).integer;
			else
#endif
				if (fread(&row_count, 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short))
			{
				idydb_error_state(handler, 14);
				return IDYDB_ERROR;
			}
#if IDYDB_SIZING_MODE != IDYDB_SIZING_MODE_BIG
			if (row_count > IDYDB_ROW_POSITION_MAX)
			{
				idydb_error_state(handler, 22);
				return IDYDB_RANGE;
			}
#endif
			row_count += 1;
		}
		unsigned char set_read_length = IDYDB_PARTITION_AND_SEGMENT;
		if (skip_offset == column_position)
		{
			unsigned short position;
#ifdef IDYDB_MMAP_OK
			if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
				position = (unsigned short)idydb_read_mmap((offset + ((sizeof(short) * 2) * (read_length == IDYDB_PARTITION_AND_SEGMENT))), sizeof(short), (*handler)->buffer).integer;
			else
#endif
				if (fread(&position, 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short))
			{
				idydb_error_state(handler, 14);
				return IDYDB_ERROR;
			}
			if (position == row_position)
			{
				store_response = true;
				row_count = 0;
			}
#if IDYDB_SIZING_MODE != IDYDB_SIZING_MODE_BIG
			else if (position > IDYDB_ROW_POSITION_MAX)
			{
				idydb_error_state(handler, 22);
				return IDYDB_RANGE;
			}
#endif
		}
		else
#ifdef IDYDB_MMAP_OK
			if ((*handler)->read_only != IDYDB_READONLY_MMAPPED)
#endif
			fseek((*handler)->file_descriptor, sizeof(short), SEEK_CUR);
		if (row_count > 1)
		{
			row_count -= 1;
			set_read_length = IDYDB_SEGMENT_SIZE;
		}
		unsigned short response_length;
		unsigned char data_type;
#ifdef IDYDB_MMAP_OK
		idydb_sizing_max offset_mmap_standard_diff = offset + (sizeof(short) * 3);
		if (read_length == IDYDB_SEGMENT_SIZE)
			offset_mmap_standard_diff = (offset + sizeof(short));
		if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
		{
			if (read_length == IDYDB_PARTITION_AND_SEGMENT)
				data_type = (unsigned short)idydb_read_mmap((offset + (sizeof(short) * 3)), 1, (*handler)->buffer).integer;
			else
				data_type = (unsigned short)idydb_read_mmap((offset + sizeof(short)), 1, (*handler)->buffer).integer;
			offset_mmap_standard_diff += 1;
		}
		else
#endif
			if (fread(&data_type, 1, 1, (*handler)->file_descriptor) != 1)
		{
			idydb_error_state(handler, 14);
			return IDYDB_ERROR;
		}
		(*handler)->value_retrieved = store_response;
		switch (data_type)
		{
		case IDYDB_READ_INT:
			if (store_response)
			{
				(*handler)->value_type = IDYDB_INTEGER;
#ifdef IDYDB_MMAP_OK
				if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
					(*handler)->value.int_value = idydb_read_mmap(offset_mmap_standard_diff, sizeof(int), (*handler)->buffer).integer;
				else
#endif
					if (fread(&(*handler)->value.int_value, 1, sizeof(int), (*handler)->file_descriptor) != sizeof(int))
				{
					idydb_error_state(handler, 18);
					return IDYDB_ERROR;
				}
				return IDYDB_DONE;
			}
			data_type = IDYDB_INTEGER;
			response_length = sizeof(int);
			break;
		case IDYDB_READ_FLOAT:
			if (store_response)
			{
				(*handler)->value_type = IDYDB_FLOAT;
#ifdef IDYDB_MMAP_OK
				if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
					(*handler)->value.float_value = idydb_read_mmap(offset_mmap_standard_diff, sizeof(float), (*handler)->buffer).floating_point;
				else
#endif
					if (fread(&(*handler)->value.float_value, 1, sizeof(float), (*handler)->file_descriptor) != sizeof(float))
				{
					idydb_error_state(handler, 18);
					return IDYDB_ERROR;
				}
				return IDYDB_DONE;
			}
			data_type = IDYDB_FLOAT;
			response_length = sizeof(float);
			break;
		case IDYDB_READ_CHAR:
			data_type = IDYDB_CHAR;
#ifdef IDYDB_MMAP_OK
			if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
				response_length = (unsigned short)idydb_read_mmap(offset_mmap_standard_diff, sizeof(short), (*handler)->buffer).integer;
			else
#endif
				if (fread(&response_length, 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short))
			{
				idydb_error_state(handler, 18);
				return IDYDB_ERROR;
			}
			response_length += 1;
			if (response_length > IDYDB_MAX_CHAR_LENGTH)
			{
				idydb_error_state(handler, 19);
				return IDYDB_ERROR;
			}
			if (store_response)
			{
				(*handler)->value_type = data_type;
				memset((*handler)->value.char_value, 0, sizeof((*handler)->value.char_value));
#ifdef IDYDB_MMAP_OK
				if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
				{
					idydb_sizing_max offset_diff = (offset_mmap_standard_diff + sizeof(short));
					for (idydb_sizing_max i = 0; i < response_length; i++)
						(*handler)->value.char_value[i] = ((char *)(*handler)->buffer)[(i + offset_diff)];
				}
				else
#endif
					if (fread((*handler)->value.char_value, response_length, sizeof(char), (*handler)->file_descriptor) != sizeof(char))
				{
					idydb_error_state(handler, 18);
					return IDYDB_ERROR;
				}
				if (strlen((*handler)->value.char_value) == 0)
				{
					idydb_error_state(handler, 14);
					return IDYDB_ERROR;
				}
				return IDYDB_DONE;
			}
			response_length += sizeof(short);
			break;
		case IDYDB_READ_BOOL_TRUE:
		case IDYDB_READ_BOOL_FALSE:
			if (store_response)
			{
				(*handler)->value_type = IDYDB_BOOL;
				(*handler)->value.bool_value = (data_type == IDYDB_READ_BOOL_TRUE);
				return IDYDB_DONE;
			}
			data_type = IDYDB_BOOL;
			response_length = 0;
			break;
		case IDYDB_READ_VECTOR:
		{
			data_type = IDYDB_VECTOR;
			unsigned short dims = 0;
#ifdef IDYDB_MMAP_OK
			if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
				dims = (unsigned short)idydb_read_mmap(offset_mmap_standard_diff, sizeof(short), (*handler)->buffer).integer;
			else
#endif
				if (fread(&dims, 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short))
			{
				idydb_error_state(handler, 18);
				return IDYDB_ERROR;
			}
			if (dims == 0 || dims > IDYDB_MAX_VECTOR_DIM)
			{
				idydb_error_state(handler, 19);
				return IDYDB_ERROR;
			}
			unsigned short bytes = (unsigned short)(dims * sizeof(float));
			if (store_response)
			{
				(*handler)->value_type  = data_type;
				(*handler)->vector_dims = dims;
				(*handler)->vector_value = (float*)malloc(sizeof(float) * (size_t)dims);
				if (!(*handler)->vector_value)
				{
					idydb_error_state(handler, 24);
					return IDYDB_ERROR;
				}
#ifdef IDYDB_MMAP_OK
				if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
				{
					idydb_sizing_max base = (offset_mmap_standard_diff + sizeof(short));
					for (unsigned short i = 0; i < dims; ++i)
					{
						union idydb_read_mmap_response f =
							idydb_read_mmap((unsigned int)(base + (i * sizeof(float))), (unsigned char)sizeof(float), (*handler)->buffer);
						(*handler)->vector_value[i] = f.floating_point;
					}
				}
				else
#endif
				{
					if (fread((*handler)->vector_value, sizeof(float), dims, (*handler)->file_descriptor) != dims)
					{
						idydb_error_state(handler, 18);
						return IDYDB_ERROR;
					}
				}
				return IDYDB_DONE;
			}
			response_length = bytes + sizeof(short);
			break;
		}
		default:
			(*handler)->value_retrieved = false;
			idydb_error_state(handler, 20);
			return IDYDB_CORRUPT;
		}
#ifdef IDYDB_MMAP_OK
		if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
			offset += read_length;
#endif
		read_length = set_read_length;
		offset += response_length;
	}
	return IDYDB_NULL;
}

/* ---------------- insert value at (column,row) ---------------- */
/* This is your original insert logic, with:
 *  - CHAR payload sizing fixed (includes '\0')
 *  - length field written correctly
 *  - dirty flag set on success
 */

static unsigned char idydb_insert_at(idydb **handler, idydb_column_row_sizing column_position, idydb_column_row_sizing row_position)
{
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

	/* Keep original coords for debug */
	idydb_column_row_sizing dbg_col = column_position;
	idydb_column_row_sizing dbg_row = row_position;

#ifdef CUWACUNU_CAMAHJUCUNU_DB_VERBOSE_DEBUG
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
		if (len == 0)
		{
			idydb_clear_values(handler);
			break;
		}
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
	unsigned short row_count[3] = {0, 0, 0};
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
			fseek((*handler)->file_descriptor, offset[0], SEEK_SET);
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
				if (fread(&row_count[0], 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short))
				{
					idydb_clear_values(handler);
					idydb_error_state(handler, 14);
					return IDYDB_ERROR;
				}
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
				if (skip_offset[0] != column_position)
					fseek((*handler)->file_descriptor, skip_read_offset++, SEEK_CUR);
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
			else
				fseek((*handler)->file_descriptor, (3 - skip_read_offset), SEEK_CUR);

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
				fseek((*handler)->file_descriptor, ((*handler)->size - buffer_offset), SEEK_SET);
				if (fread((*handler)->buffer, buffer_size, 1, (*handler)->file_descriptor) != 1)
				{
					idydb_clear_values(handler);
					idydb_error_state(handler, 14);
					return IDYDB_ERROR;
				}
				fseek((*handler)->file_descriptor, (((*handler)->size - buffer_offset) + offset_sizing + additional_offset), SEEK_SET);
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
			fseek((*handler)->file_descriptor, (((*handler)->size - buffer_offset) + offset_sizing + additional_offset), SEEK_SET);
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
			fseek((*handler)->file_descriptor, (deletion_point[0] + buffer_offset), SEEK_SET);
			if (fread((*handler)->buffer, buffer_size, 1, (*handler)->file_descriptor) != 1)
			{
				idydb_clear_values(handler);
				idydb_error_state(handler, 14);
				return IDYDB_ERROR;
			}
			fseek((*handler)->file_descriptor, (deletion_point[1] + buffer_offset), SEEK_SET);
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
		if (ftruncate(fileno((*handler)->file_descriptor), (*handler)->size) != 0)
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
				if (fread(&skip_amount[1], sizeof(short), 1, (*handler)->file_descriptor) != 1)
				{
					idydb_clear_values(handler);
					idydb_error_state(handler, 14);
					return IDYDB_ERROR;
				}
				if (skip_amount[1] == 1) skip_amount[0] = 0;
				else skip_amount[0] = (unsigned short)(skip_amount[1] - (skip_amount[0] + 1));
				fseek((*handler)->file_descriptor, offset[0], SEEK_SET);
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
			fseek((*handler)->file_descriptor, offset[0], SEEK_SET);
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
		fseek((*handler)->file_descriptor, info_skip_offset.position, SEEK_SET);
		if (fwrite(&info_skip_offset.size, sizeof(short), 1, (*handler)->file_descriptor) != 1)
		{
			idydb_clear_values(handler);
			idydb_error_state(handler, 15);
			return IDYDB_ERROR;
		}
	}
	if (info_row_count.use)
	{
		fseek((*handler)->file_descriptor, info_row_count.position, SEEK_SET);
		if (fwrite(&info_row_count.size, sizeof(short), 1, (*handler)->file_descriptor) != 1)
		{
			idydb_clear_values(handler);
			idydb_error_state(handler, 15);
			return IDYDB_ERROR;
		}
	}
	if (info_row_position.use)
	{
		fseek((*handler)->file_descriptor, info_row_position.position, SEEK_SET);
		if (fwrite(&info_row_position.size, sizeof(short), 1, (*handler)->file_descriptor) != 1)
		{
			idydb_clear_values(handler);
			idydb_error_state(handler, 15);
			return IDYDB_ERROR;
		}
	}
	if (info_input_size.use)
	{
		fseek((*handler)->file_descriptor, info_input_size.position, SEEK_SET);
		if (fwrite(&info_input_size.size, sizeof(short), 1, (*handler)->file_descriptor) != 1)
		{
			idydb_clear_values(handler);
			idydb_error_state(handler, 15);
			return IDYDB_ERROR;
		}
	}
	if (info_input_type.use)
	{
		fseek((*handler)->file_descriptor, info_input_type.position, SEEK_SET);
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
		fseek((*handler)->file_descriptor, info_input_buffer.position, SEEK_SET);
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

static inline float idydb_dot(const float* a, const float* b, unsigned short d) {
	float s = 0.0f;
	for (unsigned short i = 0; i < d; ++i) s += a[i] * b[i];
	return s;
}
static inline float idydb_norm(const float* a, unsigned short d) {
	return sqrtf(idydb_dot(a, a, d));
}

/* ---------------- Filter helpers ---------------- */

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

static int idydb_filter_build_term_mask(idydb **handler,
                                       const idydb_filter_term* term,
                                       unsigned char* term_mask,
                                       size_t mask_len)
{
	if (!handler || !*handler || !(*handler)->configured || !term || !term_mask || mask_len == 0) return 0;
	if (term->column == 0) return 0;

#ifdef IDYDB_ALLOW_UNSAFE
	if (!(*handler)->unsafe)
	{
#endif
		if ((term->column - 1) > IDYDB_COLUMN_POSITION_MAX) return 0;
#ifdef IDYDB_ALLOW_UNSAFE
	}
#endif

	/* Normalize NULL equality */
	idydb_filter_op op = term->op;
	unsigned char want_type = term->type;
	if (op == IDYDB_FILTER_OP_EQ  && want_type == IDYDB_NULL) op = IDYDB_FILTER_OP_IS_NULL;
	if (op == IDYDB_FILTER_OP_NEQ && want_type == IDYDB_NULL) op = IDYDB_FILTER_OP_IS_NOT_NULL;

	if (op == IDYDB_FILTER_OP_IS_NULL) memset(term_mask, 1, mask_len);
	else memset(term_mask, 0, mask_len);
	if (mask_len > 0) term_mask[0] = 0;

	idydb_sizing_max offset = 0;
	idydb_size_selection_type skip_offset = 0;
	unsigned char read_length = IDYDB_PARTITION_AND_SEGMENT;
	unsigned short row_count = 0;

	while ((offset + read_length) <= (*handler)->size)
	{
#ifdef IDYDB_MMAP_OK
		if ((*handler)->read_only != IDYDB_READONLY_MMAPPED)
		{
#endif
			fseek((*handler)->file_descriptor, offset, SEEK_SET);
			offset += read_length;
#ifdef IDYDB_MMAP_OK
		}
#endif

		if (read_length == IDYDB_PARTITION_AND_SEGMENT)
		{
			unsigned short skip_amount = 0;
#ifdef IDYDB_MMAP_OK
			if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
				skip_amount = (unsigned short)idydb_read_mmap(offset, sizeof(short), (*handler)->buffer).integer;
			else
#endif
				if (fread(&skip_amount, 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short)) return 0;

			skip_offset += skip_amount;
			skip_offset += 1;

#ifdef IDYDB_MMAP_OK
			if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
				row_count = (unsigned short)idydb_read_mmap((offset + sizeof(short)), sizeof(short), (*handler)->buffer).integer;
			else
#endif
				if (fread(&row_count, 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short)) return 0;
			row_count += 1;
		}

		unsigned char set_read_length = IDYDB_PARTITION_AND_SEGMENT;
		const bool in_target = (skip_offset == term->column);
		unsigned short row_pos = 0;

		if (in_target)
		{
#ifdef IDYDB_MMAP_OK
			if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
				row_pos = (unsigned short)idydb_read_mmap((offset + ((sizeof(short) * 2) * (read_length == IDYDB_PARTITION_AND_SEGMENT))), sizeof(short), (*handler)->buffer).integer;
			else
#endif
				if (fread(&row_pos, 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short)) return 0;
		}
		else
		{
#ifdef IDYDB_MMAP_OK
			if ((*handler)->read_only != IDYDB_READONLY_MMAPPED)
#endif
				fseek((*handler)->file_descriptor, sizeof(short), SEEK_CUR);
		}

		if (row_count > 1) { row_count -= 1; set_read_length = IDYDB_SEGMENT_SIZE; }

		unsigned char data_type;
#ifdef IDYDB_MMAP_OK
		idydb_sizing_max offset_mmap_standard_diff = offset + (sizeof(short) * 3);
		if (read_length == IDYDB_SEGMENT_SIZE) offset_mmap_standard_diff = (offset + sizeof(short));

		if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
		{
			if (read_length == IDYDB_PARTITION_AND_SEGMENT)
				data_type = (unsigned short)idydb_read_mmap((offset + (sizeof(short) * 3)), 1, (*handler)->buffer).integer;
			else
				data_type = (unsigned short)idydb_read_mmap((offset + sizeof(short)), 1, (*handler)->buffer).integer;
			offset_mmap_standard_diff += 1; /* now points to payload start */
		}
		else
#endif
			if (fread(&data_type, 1, 1, (*handler)->file_descriptor) != 1) return 0;

		unsigned short adv = 0;

		/* Helper: set mask for this row if in range */
		const idydb_column_row_sizing row_api = (idydb_column_row_sizing)(row_pos + 1);
		const int row_in_range = ((size_t)row_api < mask_len);

		switch (data_type)
		{
			case IDYDB_READ_BOOL_TRUE:
			case IDYDB_READ_BOOL_FALSE:
			{
				adv = 0;
				if (in_target && row_in_range)
				{
					if (op == IDYDB_FILTER_OP_IS_NULL) term_mask[row_api] = 0;
					else if (op == IDYDB_FILTER_OP_IS_NOT_NULL) term_mask[row_api] = 1;
					else if (want_type == IDYDB_BOOL) {
						bool v = (data_type == IDYDB_READ_BOOL_TRUE);
						term_mask[row_api] = (unsigned char)(idydb_filter_cmp_bool(v, op, term->value.b) ? 1 : 0);
					}
				}
				break;
			}

			case IDYDB_READ_INT:
			{
				adv = sizeof(int);
				if (in_target && row_in_range)
				{
					if (op == IDYDB_FILTER_OP_IS_NULL) term_mask[row_api] = 0;
					else if (op == IDYDB_FILTER_OP_IS_NOT_NULL) term_mask[row_api] = 1;
					else if (want_type == IDYDB_INTEGER) {
						int v = 0;
#ifdef IDYDB_MMAP_OK
						if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
							v = idydb_read_mmap(offset_mmap_standard_diff, sizeof(int), (*handler)->buffer).integer;
						else
#endif
							(void)fread(&v, 1, sizeof(int), (*handler)->file_descriptor);
						term_mask[row_api] = (unsigned char)(idydb_filter_cmp_int(v, op, term->value.i) ? 1 : 0);
					}
				}
				break;
			}

			case IDYDB_READ_FLOAT:
			{
				adv = sizeof(float);
				if (in_target && row_in_range)
				{
					if (op == IDYDB_FILTER_OP_IS_NULL) term_mask[row_api] = 0;
					else if (op == IDYDB_FILTER_OP_IS_NOT_NULL) term_mask[row_api] = 1;
					else if (want_type == IDYDB_FLOAT) {
						float v = 0.0f;
#ifdef IDYDB_MMAP_OK
						if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
							v = idydb_read_mmap(offset_mmap_standard_diff, sizeof(float), (*handler)->buffer).floating_point;
						else
#endif
							(void)fread(&v, 1, sizeof(float), (*handler)->file_descriptor);
						term_mask[row_api] = (unsigned char)(idydb_filter_cmp_float(v, op, term->value.f) ? 1 : 0);
					}
				}
				break;
			}

			case IDYDB_READ_CHAR:
			{
				unsigned short n = 0; /* stored length (no '\0') */
#ifdef IDYDB_MMAP_OK
				if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
					n = (unsigned short)idydb_read_mmap(offset_mmap_standard_diff, sizeof(short), (*handler)->buffer).integer;
				else
#endif
					(void)fread(&n, 1, sizeof(short), (*handler)->file_descriptor);
				adv = (unsigned short)(sizeof(short) + n + 1);

				if (in_target && row_in_range)
				{
					if (op == IDYDB_FILTER_OP_IS_NULL) term_mask[row_api] = 0;
					else if (op == IDYDB_FILTER_OP_IS_NOT_NULL) term_mask[row_api] = 1;
					else if (want_type == IDYDB_CHAR)
					{
						const char* want = (term->value.s ? term->value.s : "");
						const size_t want_len = strlen(want);
						int eq = 0;
						if (want_len == (size_t)n)
						{
#ifdef IDYDB_MMAP_OK
							if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
							{
								const char* p = (const char*)((char*)(*handler)->buffer + (offset_mmap_standard_diff + sizeof(short)));
								eq = (memcmp(p, want, want_len) == 0);
							}
							else
#endif
							{
								/* Stream compare; ok to stop early (next loop fseek()s anyway) */
								size_t pos = 0;
								eq = 1;
								char buf[1024];
								while (pos < want_len)
								{
									size_t chunk = want_len - pos;
									if (chunk > sizeof(buf)) chunk = sizeof(buf);
									if (fread(buf, 1, chunk, (*handler)->file_descriptor) != chunk) { eq = 0; break; }
									if (memcmp(buf, want + pos, chunk) != 0) { eq = 0; break; }
									pos += chunk;
								}
							}
						}
						if (op == IDYDB_FILTER_OP_EQ) term_mask[row_api] = (unsigned char)(eq ? 1 : 0);
						else if (op == IDYDB_FILTER_OP_NEQ) term_mask[row_api] = (unsigned char)(eq ? 0 : 1);
					}
				}
				break;
			}

			case IDYDB_READ_VECTOR:
			{
				unsigned short d = 0;
#ifdef IDYDB_MMAP_OK
				if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
					d = (unsigned short)idydb_read_mmap(offset_mmap_standard_diff, sizeof(short), (*handler)->buffer).integer;
				else
#endif
					(void)fread(&d, 1, sizeof(short), (*handler)->file_descriptor);
				adv = (unsigned short)(sizeof(short) + d * sizeof(float));

				if (in_target && row_in_range)
				{
					/* Only NULL-ness is supported for VECTOR in filters (keeps it sane). */
					if (op == IDYDB_FILTER_OP_IS_NULL) term_mask[row_api] = 0;
					else if (op == IDYDB_FILTER_OP_IS_NOT_NULL) term_mask[row_api] = 1;
				}
				break;
			}

			default:
				return 0;
		}

#ifdef IDYDB_MMAP_OK
		if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
			offset += read_length;
#endif
		read_length = set_read_length;
		offset += adv;
	}

	return 1;
}

static int idydb_filter_build_allowed_mask(idydb **handler,
                                          const idydb_filter* filter,
                                          unsigned char* allowed,
                                          size_t allowed_len)
{
	if (!allowed || allowed_len == 0) return 0;
	memset(allowed, 1, allowed_len);
	if (allowed_len > 0) allowed[0] = 0;

	if (!filter || filter->nterms == 0 || !filter->terms) return 1;

	unsigned char* tmp = (unsigned char*)malloc(allowed_len);
	if (!tmp) return 0;

	for (size_t t = 0; t < filter->nterms; ++t)
	{
		if (!idydb_filter_build_term_mask(handler, &filter->terms[t], tmp, allowed_len))
		{
			free(tmp);
			return 0;
		}
		for (size_t i = 0; i < allowed_len; ++i)
			allowed[i] = (unsigned char)((allowed[i] && tmp[i]) ? 1 : 0);
	}

	free(tmp);
	return 1;
}

/* ---------------- Column scanning for kNN ---------------- */

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
	if (!(*handler) || !(*handler)->configured || !query || dims == 0 || dims > IDYDB_MAX_VECTOR_DIM || k == 0 || !out_results)
	{
		idydb_error_state(handler, 8);
		return -1;
	}
#ifdef IDYDB_ALLOW_UNSAFE
	if (!(*handler)->unsafe)
	{
#endif
		if (vector_column == 0 || (vector_column - 1) > IDYDB_COLUMN_POSITION_MAX)
		{
			idydb_error_state(handler, 12);
			return -1;
		}
#ifdef IDYDB_ALLOW_UNSAFE
	}
#endif
	for (unsigned short i = 0; i < k; ++i) { out_results[i].row = 0; out_results[i].score = -INFINITY; }

	float query_norm = 1.0f;
	if (metric == IDYDB_SIM_COSINE)
	{
		query_norm = idydb_norm(query, dims);
		if (query_norm == 0.0f) query_norm = 1.0f;
	}

	idydb_sizing_max offset = 0;
	idydb_size_selection_type skip_offset = 0;
	unsigned char read_length = IDYDB_PARTITION_AND_SEGMENT;
	unsigned short row_count = 0;

	while ((offset + read_length) <= (*handler)->size)
	{
#ifdef IDYDB_MMAP_OK
		if ((*handler)->read_only != IDYDB_READONLY_MMAPPED)
		{
#endif
			fseek((*handler)->file_descriptor, offset, SEEK_SET);
			offset += read_length;
#ifdef IDYDB_MMAP_OK
		}
#endif
		if (read_length == IDYDB_PARTITION_AND_SEGMENT)
		{
			unsigned short skip_amount = 0;
#ifdef IDYDB_MMAP_OK
			if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
				skip_amount = (unsigned short)idydb_read_mmap(offset, sizeof(short), (*handler)->buffer).integer;
			else
#endif
				if (fread(&skip_amount, 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short))
			{
				idydb_error_state(handler, 14);
				return -1;
			}
			skip_offset += skip_amount;
			skip_offset += 1;
#ifdef IDYDB_MMAP_OK
			if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
				row_count = (unsigned short)idydb_read_mmap((offset + sizeof(short)), sizeof(short), (*handler)->buffer).integer;
			else
#endif
				if (fread(&row_count, 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short))
			{
				idydb_error_state(handler, 14);
				return -1;
			}
			row_count += 1;
		}

		unsigned char set_read_length = IDYDB_PARTITION_AND_SEGMENT;

		unsigned short row_pos = 0;
		if (skip_offset == vector_column) {
#ifdef IDYDB_MMAP_OK
			if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
				row_pos = (unsigned short)idydb_read_mmap((offset + ((sizeof(short) * 2) * (read_length == IDYDB_PARTITION_AND_SEGMENT))), sizeof(short), (*handler)->buffer).integer;
			else
#endif
				if (fread(&row_pos, 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short))
			{
				idydb_error_state(handler, 14);
				return -1;
			}
		} else {
#ifdef IDYDB_MMAP_OK
			if ((*handler)->read_only != IDYDB_READONLY_MMAPPED)
#endif
				fseek((*handler)->file_descriptor, sizeof(short), SEEK_CUR);
		}

		if (row_count > 1) { row_count -= 1; set_read_length = IDYDB_SEGMENT_SIZE; }

		unsigned char data_type;
#ifdef IDYDB_MMAP_OK
		idydb_sizing_max offset_mmap_standard_diff = offset + (sizeof(short) * 3);
		if (read_length == IDYDB_SEGMENT_SIZE)
			offset_mmap_standard_diff = (offset + sizeof(short));
		if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
		{
			if (read_length == IDYDB_PARTITION_AND_SEGMENT)
				data_type = (unsigned short)idydb_read_mmap((offset + (sizeof(short) * 3)), 1, (*handler)->buffer).integer;
			else
				data_type = (unsigned short)idydb_read_mmap((offset + sizeof(short)), 1, (*handler)->buffer).integer;
			offset_mmap_standard_diff += 1;
		}
		else
#endif
			if (fread(&data_type, 1, 1, (*handler)->file_descriptor) != 1)
		{
			idydb_error_state(handler, 14);
			return -1;
		}

		unsigned short adv = 0;
		switch (data_type)
		{
			case IDYDB_READ_CHAR: {
				unsigned short n = 0;
#ifdef IDYDB_MMAP_OK
				if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
					n = (unsigned short)idydb_read_mmap(offset_mmap_standard_diff, sizeof(short), (*handler)->buffer).integer;
				else
#endif
					if (fread(&n, 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short)) { idydb_error_state(handler, 14); return -1; }
				adv = (unsigned short)(sizeof(short) + n + 1);
				break;
			}
			case IDYDB_READ_INT:   adv = sizeof(int);   break;
			case IDYDB_READ_FLOAT: adv = sizeof(float); break;
			case IDYDB_READ_BOOL_TRUE:
			case IDYDB_READ_BOOL_FALSE:
				adv = 0; break;
			case IDYDB_READ_VECTOR: {
				unsigned short vdims = 0;
#ifdef IDYDB_MMAP_OK
				if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
					vdims = (unsigned short)idydb_read_mmap(offset_mmap_standard_diff, sizeof(short), (*handler)->buffer).integer;
				else
#endif
					if (fread(&vdims, 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short)) { idydb_error_state(handler, 14); return -1; }
				adv = (unsigned short)(sizeof(short) + vdims * sizeof(float));

				if (skip_offset == vector_column && vdims == dims) {
					const idydb_column_row_sizing row_api = (idydb_column_row_sizing)(row_pos + 1);
					if (allowed && ((size_t)row_api >= allowed_len || allowed[row_api] == 0))
					{
						/* Row filtered out: do not score it. In non-mmap, we also avoid reading floats. */
#ifdef IDYDB_MMAP_OK
						if ((*handler)->read_only != IDYDB_READONLY_MMAPPED)
#endif
						{
							/* No-op: next loop fseek()s to offset anyway. */
						}
						break;
					}
					float score = -INFINITY;
#ifdef IDYDB_MMAP_OK
					if ((*handler)->read_only == IDYDB_READONLY_MMAPPED) {
						idydb_sizing_max base = offset_mmap_standard_diff + sizeof(short);
						float dot = 0.0f, l2acc = 0.0f, normB = 0.0f;
						for (unsigned short i = 0; i < dims; ++i) {
							union idydb_read_mmap_response f =
								idydb_read_mmap((unsigned int)(base + (i * sizeof(float))), (unsigned char)sizeof(float), (*handler)->buffer);
							float b = f.floating_point;
							if (metric == IDYDB_SIM_COSINE) { dot += query[i] * b; normB += b*b; }
							else { float d = query[i]-b; l2acc += d*d; }
						}
						if (metric == IDYDB_SIM_COSINE) {
							normB = sqrtf(normB); if (normB == 0.0f) normB = 1.0f;
							score = dot / (query_norm * normB);
						} else {
							score = -sqrtf(l2acc);
						}
					} else
#endif
					{
						float dot = 0.0f, l2acc = 0.0f, normB = 0.0f;
						for (unsigned short i = 0; i < dims; ++i) {
							float b;
							if (fread(&b, sizeof(float), 1, (*handler)->file_descriptor) != 1) { idydb_error_state(handler, 18); return -1; }
							if (metric == IDYDB_SIM_COSINE) { dot += query[i]*b; normB += b*b; }
							else { float d = query[i]-b; l2acc += d*d; }
						}
						if (metric == IDYDB_SIM_COSINE) {
							float normBv = sqrtf(normB); if (normBv == 0.0f) normBv = 1.0f;
							score = dot / (query_norm * normBv);
						} else {
							score = -sqrtf(l2acc);
						}
					}

					unsigned short worst = 0;
					float worstScore = out_results[0].score;
					for (unsigned short i = 1; i < k; ++i) {
						if (out_results[i].score < worstScore) { worstScore = out_results[i].score; worst = i; }
					}
					if (score > worstScore) {
						out_results[worst].row = (idydb_column_row_sizing)(row_pos + 1);
						out_results[worst].score = score;
					}
				}
				break;
			}
			default:
				idydb_error_state(handler, 20);
				return -1;
		}

#ifdef IDYDB_MMAP_OK
		if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
			offset += read_length;
#endif
		read_length = set_read_length;
		offset += adv;
	}

	for (unsigned short i = 0; i < k; ++i) {
		for (unsigned short j = i+1; j < k; ++j) {
			if (out_results[j].row != 0 && (out_results[i].row == 0 || out_results[j].score > out_results[i].score)) {
				idydb_knn_result tmp = out_results[i];
				out_results[i] = out_results[j];
				out_results[j] = tmp;
			}
		}
	}

	unsigned short count = 0;
	for (unsigned short i = 0; i < k; ++i) if (out_results[i].row != 0) ++count;
	return (int)count;
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
	if (!out_results) { idydb_error_state(handler, 8); return -1; }

	const size_t allowed_len = (size_t)IDYDB_ROW_POSITION_MAX + 2;
	unsigned char* allowed = NULL;

	if (filter && filter->terms && filter->nterms > 0)
	{
		allowed = (unsigned char*)malloc(allowed_len);
		if (!allowed) { idydb_error_state(handler, 24); return -1; }
		if (!idydb_filter_build_allowed_mask(handler, filter, allowed, allowed_len))
		{
			free(allowed);
			idydb_error_state(handler, 26);
			return -1;
		}
	}

	int n = idydb_knn_search_vector_column_internal(handler, vector_column, query, dims, k, metric, allowed, allowed_len, out_results);
	if (allowed) free(allowed);
	return n;
}

/* ---------------- Utility: next row index ---------------- */
/* Your original function (unchanged) */

idydb_column_row_sizing idydb_column_next_row(idydb **handler, idydb_column_row_sizing column)
{
	if (!(*handler) || !(*handler)->configured) return 1;
	idydb_sizing_max offset = 0;
	idydb_size_selection_type skip_offset = 0;
	unsigned char read_length = IDYDB_PARTITION_AND_SEGMENT;
	unsigned short row_count = 0;
	unsigned short max_row = 0;
	while ((offset + read_length) <= (*handler)->size)
	{
#ifdef IDYDB_MMAP_OK
		if ((*handler)->read_only != IDYDB_READONLY_MMAPPED)
		{
#endif
			fseek((*handler)->file_descriptor, offset, SEEK_SET);
			offset += read_length;
#ifdef IDYDB_MMAP_OK
		}
#endif
		if (read_length == IDYDB_PARTITION_AND_SEGMENT)
		{
			unsigned short skip_amount = 0;
#ifdef IDYDB_MMAP_OK
			if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
				skip_amount = (unsigned short)idydb_read_mmap(offset, sizeof(short), (*handler)->buffer).integer;
			else
#endif
				if (fread(&skip_amount, 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short)) break;
			skip_offset += skip_amount;
			skip_offset += 1;
#ifdef IDYDB_MMAP_OK
			if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
				row_count = (unsigned short)idydb_read_mmap((offset + sizeof(short)), sizeof(short), (*handler)->buffer).integer;
			else
#endif
				if (fread(&row_count, 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short)) break;
			row_count += 1;
		}
		unsigned char set_read_length = IDYDB_PARTITION_AND_SEGMENT;
		if (skip_offset == column)
		{
			unsigned short row_pos = 0;
#ifdef IDYDB_MMAP_OK
			if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
				row_pos = (unsigned short)idydb_read_mmap((offset + ((sizeof(short) * 2) * (read_length == IDYDB_PARTITION_AND_SEGMENT))), sizeof(short), (*handler)->buffer).integer;
			else
#endif
				if (fread(&row_pos, 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short)) break;
			if ((row_pos + 1) > max_row) max_row = (unsigned short)(row_pos + 1);
		}
		else
#ifdef IDYDB_MMAP_OK
			if ((*handler)->read_only != IDYDB_READONLY_MMAPPED)
#endif
			fseek((*handler)->file_descriptor, sizeof(short), SEEK_CUR);

		if (row_count > 1) { row_count -= 1; set_read_length = IDYDB_SEGMENT_SIZE; }

		unsigned char t;
#ifdef IDYDB_MMAP_OK
		idydb_sizing_max offset_mmap_standard_diff = offset + (sizeof(short) * 3);
		if (read_length == IDYDB_SEGMENT_SIZE)
			offset_mmap_standard_diff = (offset + sizeof(short));
		if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
			t = (unsigned short)idydb_read_mmap((read_length == IDYDB_PARTITION_AND_SEGMENT ? (offset + (sizeof(short) * 3)) : (offset + sizeof(short))), 1, (*handler)->buffer).integer;
		else
#endif
			if (fread(&t, 1, 1, (*handler)->file_descriptor) != 1) break;

		unsigned short adv = 0;
		switch (t) {
			case IDYDB_READ_CHAR: {
				unsigned short n = 0;
#ifdef IDYDB_MMAP_OK
				if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
					n = (unsigned short)idydb_read_mmap(offset_mmap_standard_diff, sizeof(short), (*handler)->buffer).integer;
				else
#endif
					if (fread(&n, 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short)) n = 0;
				adv = (unsigned short)(sizeof(short) + n + 1);
				break;
			}
			case IDYDB_READ_INT:   adv = sizeof(int); break;
			case IDYDB_READ_FLOAT: adv = sizeof(float); break;
			case IDYDB_READ_BOOL_TRUE:
			case IDYDB_READ_BOOL_FALSE:
				adv = 0; break;
			case IDYDB_READ_VECTOR: {
				unsigned short d = 0;
#ifdef IDYDB_MMAP_OK
				if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
					d = (unsigned short)idydb_read_mmap(offset_mmap_standard_diff, sizeof(short), (*handler)->buffer).integer;
				else
#endif
					if (fread(&d, 1, sizeof(short), (*handler)->file_descriptor) != sizeof(short)) d = 0;
				adv = (unsigned short)(sizeof(short) + d * sizeof(float));
				break;
			}
			default: break;
		}
#ifdef IDYDB_MMAP_OK
		if ((*handler)->read_only == IDYDB_READONLY_MMAPPED)
			offset += read_length;
#endif
		read_length = set_read_length;
		offset += adv;
	}
	return (idydb_column_row_sizing)(max_row + 1);
}

/* ---------------- RAG helpers (unchanged from your version) ---------------- */

void idydb_set_embedder(idydb **handler, idydb_embed_fn fn, void* user) {
	if (!(*handler)) return;
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
	if (!text || !embedding || dims == 0) { idydb_error_state(handler, 8); return IDYDB_ERROR; }
	int rc = idydb_insert_const_char(handler, text_column, row, text);
	if (rc != IDYDB_DONE) return rc;
	rc = idydb_insert_vector(handler, vector_column, row, embedding, dims);
	return rc;
}

int idydb_rag_upsert_text_auto_embed(idydb **handler,
                                     idydb_column_row_sizing text_column,
                                     idydb_column_row_sizing vector_column,
                                     idydb_column_row_sizing row,
                                     const char* text)
{
	if (!(*handler) || !(*handler)->embedder || !text) { idydb_error_state(handler, 8); return IDYDB_ERROR; }
	float* vec = NULL; unsigned short dims = 0;
	int erc = (*handler)->embedder(text, &vec, &dims, (*handler)->embedder_user);
	if (erc != 0 || !vec || dims == 0) { if (vec) free(vec); idydb_error_state(handler, 24); return IDYDB_ERROR; }
	int rc = idydb_rag_upsert_text(handler, text_column, vector_column, row, text, vec, dims);
	free(vec);
	return rc;
}

/* ---------------- Return conventions ----------------
 *
 * Most APIs return an IDYDB_* status code:
 *   - IDYDB_DONE / IDYDB_SUCCESS on success
 *   - other IDYDB_* codes on failure (see idydb_errmsg)
 *
 * Some query-style APIs return a COUNT:
 *   - returns n in [0..k] on success (0 means “no hits”, not an error)
 *   - returns -1 on error (details in idydb_errmsg)
 *
 * IMPORTANT: wrappers must treat these as “count-returning” and check (rc < 0),
 * not (rc != IDYDB_DONE).
 */
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
  if (!out_results || !out_texts) { idydb_error_state(handler, 8); return -1; }

  for (unsigned short i = 0; i < k; ++i) out_texts[i] = NULL;

  int n = idydb_knn_search_vector_column(handler, vector_column, query_embedding, dims, k, metric, out_results);
  if (n <= 0) return n;

  for (int i = 0; i < n; ++i) {
    if (out_results[i].row == 0) { out_texts[i] = NULL; continue; }

    int rc = idydb_extract(handler, text_column, out_results[i].row);

    if (rc == IDYDB_NULL) { out_texts[i] = NULL; continue; }
    if (rc != IDYDB_DONE) {
      idydb_error_statef(handler, 18,
        "rag_query_topk: extract(text) failed col=%llu row=%llu rc=%d",
        (unsigned long long)text_column,
        (unsigned long long)out_results[i].row,
        rc
      );
      for (int j = 0; j < i; ++j) { if (out_texts[j]) { free(out_texts[j]); out_texts[j] = NULL; } }
      return -1;
    }

    if (idydb_retrieved_type(handler) != IDYDB_CHAR) { out_texts[i] = NULL; continue; }

    char* s = idydb_retrieve_char(handler);
    if (!s) { out_texts[i] = NULL; continue; }

    size_t len = strlen(s);
    out_texts[i] = (char*)malloc(len + 1);
    if (!out_texts[i]) {
      idydb_error_statef(handler, 24,
        "rag_query_topk: OOM copying text len=%zu (col=%llu row=%llu)",
        len,
        (unsigned long long)text_column,
        (unsigned long long)out_results[i].row
      );
      for (int j = 0; j < i; ++j) { if (out_texts[j]) { free(out_texts[j]); out_texts[j] = NULL; } }
      return -1;
    }
    memcpy(out_texts[i], s, len + 1);
  }

  return n;
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
  if (!out_results || !out_texts) { idydb_error_state(handler, 8); return -1; }
  for (unsigned short i = 0; i < k; ++i) out_texts[i] = NULL;

  int n = idydb_knn_search_vector_column_filtered(handler, vector_column, query_embedding, dims, k, metric, filter, out_results);
  if (n <= 0) return n;

  for (int i = 0; i < n; ++i) {
    if (out_results[i].row == 0) { out_texts[i] = NULL; continue; }

    int rc = idydb_extract(handler, text_column, out_results[i].row);

    if (rc == IDYDB_NULL) { out_texts[i] = NULL; continue; }
    if (rc != IDYDB_DONE) {
      idydb_error_statef(handler, 18,
        "rag_query_topk_filtered: extract(text) failed col=%llu row=%llu rc=%d",
        (unsigned long long)text_column,
        (unsigned long long)out_results[i].row,
        rc
      );
      for (int j = 0; j < i; ++j) { if (out_texts[j]) { free(out_texts[j]); out_texts[j] = NULL; } }
      return -1;
    }

    if (idydb_retrieved_type(handler) != IDYDB_CHAR) { out_texts[i] = NULL; continue; }

    char* s = idydb_retrieve_char(handler);
    if (!s) { out_texts[i] = NULL; continue; }

    size_t len = strlen(s);
    out_texts[i] = (char*)malloc(len + 1);
    if (!out_texts[i]) {
      idydb_error_statef(handler, 24,
        "rag_query_topk_filtered: OOM copying text len=%zu (col=%llu row=%llu)",
        len,
        (unsigned long long)text_column,
        (unsigned long long)out_results[i].row
      );
      for (int j = 0; j < i; ++j) { if (out_texts[j]) { free(out_texts[j]); out_texts[j] = NULL; } }
      return -1;
    }
    memcpy(out_texts[i], s, len + 1);
  }

  return n;
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
	if (!out_results || !out_texts) { idydb_error_state(handler, 8); return -1; }
	if (meta_columns_count > 0 && (!meta_columns || !out_meta)) { idydb_error_state(handler, 8); return -1; }

	for (unsigned short i = 0; i < k; ++i) out_texts[i] = NULL;
	if (out_meta && meta_columns_count > 0) {
		for (size_t i = 0; i < (size_t)k * meta_columns_count; ++i) out_meta[i].type = IDYDB_NULL;
	}

	int n = idydb_rag_query_topk_filtered(handler, text_column, vector_column, query_embedding, dims, k, metric,
	                                     filter, out_results, out_texts);
	if (n <= 0) return n;

	if (!out_meta || meta_columns_count == 0) return n;

	for (int i = 0; i < n; ++i)
	{
		if (out_results[i].row == 0) continue;
		for (size_t j = 0; j < meta_columns_count; ++j)
		{
			const size_t idx = (size_t)i * meta_columns_count + j;
			idydb_value* v = &out_meta[idx];
			v->type = IDYDB_NULL;

			int rc = idydb_extract(handler, meta_columns[j], out_results[i].row);
			if (rc == IDYDB_NULL) { v->type = IDYDB_NULL; continue; }
			if (rc != IDYDB_DONE) {
				idydb_error_statef(handler, 18,
					"rag_query_topk_with_metadata: extract(meta) failed meta_col=%llu row=%llu rc=%d",
					(unsigned long long)meta_columns[j],
					(unsigned long long)out_results[i].row,
					rc
				);
				return -1;
			}

			unsigned char t = (unsigned char)idydb_retrieved_type(handler);
			v->type = t;

			switch (t)
			{
				case IDYDB_INTEGER:
					v->as.i = idydb_retrieve_int(handler);
					break;
				case IDYDB_FLOAT:
					v->as.f = idydb_retrieve_float(handler);
					break;
				case IDYDB_BOOL:
					v->as.b = idydb_retrieve_bool(handler);
					break;
				case IDYDB_CHAR:
				{
					char* s = idydb_retrieve_char(handler);
					if (!s) { v->type = IDYDB_NULL; break; }

					size_t len = strlen(s);
					v->as.s = (char*)malloc(len + 1);
					if (!v->as.s) {
						idydb_error_statef(handler, 24,
							"rag_query_topk_with_metadata: OOM copying CHAR meta col=%llu row=%llu len=%zu",
							(unsigned long long)meta_columns[j],
							(unsigned long long)out_results[i].row,
							len
						);
						v->type = IDYDB_NULL;
						idydb_clear_values(handler); /* important: cleanup handler temp state */
						return -1;
					}

					memcpy(v->as.s, s, len + 1);
					break;
				}
				case IDYDB_VECTOR:
				{
					unsigned short vd = 0;
					const float* pv = idydb_retrieve_vector(handler, &vd);
					if (!pv || vd == 0) { v->type = IDYDB_NULL; break; }

					v->as.vec.dims = vd;
					v->as.vec.v = (float*)malloc(sizeof(float) * (size_t)vd);
					if (!v->as.vec.v) {
						idydb_error_statef(handler, 24,
							"rag_query_topk_with_metadata: OOM copying VECTOR meta col=%llu row=%llu dims=%u",
							(unsigned long long)meta_columns[j],
							(unsigned long long)out_results[i].row,
							(unsigned)vd
						);
						v->type = IDYDB_NULL;
						v->as.vec.dims = 0;
						idydb_clear_values(handler); /* important: frees handler->vector_value if allocated */
						return -1;
					}

					memcpy(v->as.vec.v, pv, sizeof(float) * (size_t)vd);
					break;
				}
				default:
					v->type = IDYDB_NULL;
					break;
			}

			/* important: avoid keeping extracted VECTOR allocated on handler */
			idydb_clear_values(handler);
		}
	}

	return n;
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
	if (!out_context) { idydb_error_state(handler, 8); return IDYDB_ERROR; }
	idydb_knn_result* res = (idydb_knn_result*)calloc(k, sizeof(idydb_knn_result));
	char** texts = (char**)calloc(k, sizeof(char*));
	if (!res || !texts) { if (res) free(res); if (texts) free(texts); idydb_error_state(handler, 24); return IDYDB_ERROR; }
	int n = idydb_rag_query_topk(handler, text_column, vector_column, query_embedding, dims, k, metric, res, texts);
	if (n <= 0) { free(res); free(texts); return (n == 0 ? IDYDB_DONE : IDYDB_ERROR); }
	size_t total = 0;
	const char* sep = "\n---\n";
	size_t sep_len = strlen(sep);
	for (int i = 0; i < n; ++i) { if (texts[i]) total += strlen(texts[i]); if (i+1 < n) total += sep_len; }
	if (max_chars > 0 && total > max_chars) total = max_chars;
	char* buf = (char*)malloc(total + 1);
	if (!buf) { for (int i=0;i<n;++i) if (texts[i]) free(texts[i]); free(res); free(texts); idydb_error_state(handler, 24); return IDYDB_ERROR; }
	size_t written = 0;
	for (int i = 0; i < n; ++i) {
		if (!texts[i]) continue;
		size_t len = strlen(texts[i]);
		if (max_chars > 0 && written + len > max_chars) len = max_chars - written;
		memcpy(buf + written, texts[i], len);
		written += len;
		if (i+1 < n) {
			if (max_chars > 0 && written + sep_len > max_chars) break;
			memcpy(buf + written, sep, sep_len);
			written += sep_len;
		}
	}
	buf[written] = '\0';
	for (int i=0;i<n;++i) if (texts[i]) free(texts[i]);
	free(res); free(texts);
	*out_context = buf;
	return IDYDB_DONE;
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
	idydb_knn_result* res = (idydb_knn_result*)calloc(k, sizeof(idydb_knn_result));
	char** texts = (char**)calloc(k, sizeof(char*));
	if (!res || !texts) { if (res) free(res); if (texts) free(texts); idydb_error_state(handler, 24); return IDYDB_ERROR; }

	int n = idydb_rag_query_topk_filtered(handler, text_column, vector_column, query_embedding, dims, k, metric, filter, res, texts);
	if (n <= 0) { free(res); free(texts); return (n == 0 ? IDYDB_DONE : IDYDB_ERROR); }

	size_t total = 0;
	const char* sep = "\n---\n";
	size_t sep_len = strlen(sep);
	for (int i = 0; i < n; ++i) { if (texts[i]) total += strlen(texts[i]); if (i+1 < n) total += sep_len; }
	if (max_chars > 0 && total > max_chars) total = max_chars;
	char* buf = (char*)malloc(total + 1);
	if (!buf) { for (int i=0;i<n;++i) if (texts[i]) free(texts[i]); free(res); free(texts); idydb_error_state(handler, 24); return IDYDB_ERROR; }

	size_t written = 0;
	for (int i = 0; i < n; ++i) {
		if (!texts[i]) continue;
		size_t len = strlen(texts[i]);
		if (max_chars > 0 && written + len > max_chars) len = max_chars - written;
		memcpy(buf + written, texts[i], len);
		written += len;
		if (i+1 < n) {
			if (max_chars > 0 && written + sep_len > max_chars) break;
			memcpy(buf + written, sep, sep_len);
			written += sep_len;
		}
	}
	buf[written] = '\0';
	for (int i=0;i<n;++i) if (texts[i]) free(texts[i]);
	free(res); free(texts);
	*out_context = buf;
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
