# IdyDB

IdyDB is a C++17 library with a C API for a sparse, flat-file database. It stores integers, floats, strings, booleans, and float vectors. Vector queries support cosine similarity, L2 distance, metadata filters, and retrieval of associated text.

The public interface is in `idydb/include/db.h`; the implementation must also be compiled and linked with OpenSSL Crypto. This is a header-and-library project.

## Development

Build and test in the managed Linux development container. On Windows, Docker Desktop must be running:

```powershell
.\dev.ps1 start
.\dev.ps1 setup
.\dev.ps1 build
.\dev.ps1 test
.\dev.ps1 test-addon
.\dev.ps1 check
.\dev.ps1 status
```

`setup.sh` only installs dependencies, using `apt-get --no-install-recommends` and versions recorded in `dev/packages.lock`. `dev.ps1` manages lifecycle and project commands. It reuses a matching `idydb-dev` container, including a stopped one, and refuses a conflicting container or unmanaged build volume.

The base image is Debian 13 slim, pinned by digest. The repository mounts at `/workspace`; all builds, npm packages, and dependency caches stay in the container or the `idydb-build` volume mounted at `/build`. No ports, GPUs, or devices are exposed. `.\dev.ps1 stop` stops the container while retaining its contents and build volume.

`check` runs all five library configurations (big, tiny, small, shared with mmap disabled, and address/undefined-behavior sanitizers), plus ordinary and instrumented Node tests. The GitHub workflow runs the same scripts in the pinned Debian container for pushes and pull requests. Fault hooks are compiled into separate test builds and are absent from the production library and addon.

The verified runtime is Linux. The source includes Windows compatibility code; native Windows behavior still needs separate validation before a Windows release.

Inside the configured container, the CMake workflow is:

```sh
cmake -S /workspace/idydb -B /build/cmake -DBUILD_TESTING=ON
cmake --build /build/cmake --parallel 2
ctest --test-dir /build/cmake --output-on-failure
```

CMake options:

| Option | Default | Purpose |
| --- | --- | --- |
| `IDYDB_BUILD_SHARED` | OFF | Build a shared library |
| `IDYDB_ENABLE_MMAP` | ON | Map eligible read-only files |
| `IDYDB_SIZING_MODE` | 3 | 1 = tiny, 2 = small, 3 = big |
| `BUILD_TESTING` | ON | Build C/C++ regression suites |

Consumers must use the same sizing mode as the library. Prefer linking the CMake `idydb` target so its public definitions and OpenSSL dependency propagate.

## Basic C++ usage

Coordinates start at 1. Open returns `IDYDB_SUCCESS`; inserts, successful extracts, deletes, and closes return `IDYDB_DONE`. A missing cell returns `IDYDB_NULL`.

```cpp
#include "db.h"
#include <cstdio>

int main() {
    idydb* handle = nullptr;
    int rc = idydb_open("data.db", &handle, IDYDB_CREATE);
    if (rc != IDYDB_SUCCESS) {
        std::fprintf(stderr, "%s\n", idydb_errmsg(&handle));
        idydb_close(&handle);
        return 1;
    }

    rc = db::idydb_insert(&handle, 1, 1, "Hello IdyDB");
    if (rc == IDYDB_DONE) rc = idydb_extract(&handle, 1, 1);
    if (rc == IDYDB_DONE) std::puts(idydb_retrieve_char(&handle));

    if (idydb_close(&handle) != IDYDB_DONE) {
        std::fprintf(stderr, "%s\n", idydb_errmsg(&handle));
        return 1;
    }
    return rc == IDYDB_DONE ? 0 : 1;
}
```

Initialize a handle to `nullptr` (or `NULL` in C). Close it before reopening. After a failed open, a handler may remain solely for error reporting; discard it to release that allocation. Extracted strings and vectors are borrowed and become invalid on the next extract, insert, delete, close, or discard.

## Vector retrieval

```cpp
const float embedding[] = {0.1f, 0.5f, 0.9f};
int rc = idydb_rag_upsert_text(&handle, 1, 2, 1,
                             "A useful document", embedding, 3);
if (rc == IDYDB_DONE) {
    idydb_knn_result hits[5] = {};
    char* texts[5] = {};
    int count = idydb_rag_query_topk(&handle, 1, 2, embedding, 3, 5,
                                   IDYDB_SIM_COSINE, hits, texts);
    if (count < 0) {
        std::fprintf(stderr, "%s\n", idydb_errmsg(&handle));
    } else {
        for (int i = 0; i < count; ++i) {
            std::printf("row=%llu score=%g text=%s\n",
                        static_cast<unsigned long long>(hits[i].row),
                        hits[i].score, texts[i] ? texts[i] : "");
        }
    }
    for (char* text : texts) idydb_free(text);
}
```

kNN and top-k RAG queries return a **count from 0 to k**, or **-1 on error**. Counts are separate from operation status codes. Results are sorted with higher scores first, with lower row numbers breaking ties. Cosine scores range from -1 to 1; a zero vector scores 0. L2 scores are negative Euclidean distances. Dimension-mismatched records are skipped; query components must be finite.

Filtered queries combine terms with AND. `IS_NULL` selects missing metadata cells; comparison terms require the requested type. `idydb_column_next_row` returns the next index after the largest stored row, or 0 for an error/full column.

Returned RAG strings and contexts are owned allocations: free them with `idydb_free`. Metadata strings and vectors are deep copies: release them with `idydb_values_free`. An automatic embedder callback must return a vector allocated compatibly with `free()`; IdyDB takes ownership of that allocation.

## Encryption

OpenSSL Crypto is required for every build. AES-256-GCM protects the encrypted backing file, with PBKDF2-HMAC-SHA256 deriving the key.

```cpp
idydb_open_options options = {};
options.flags = IDYDB_CREATE;
options.encrypted_at_rest = true;
options.passphrase = password; // supplied by the caller
int rc = idydb_open_with_options("secure.db", &handle, &options);
```

Encrypted changes persist during **explicit flush or close**. `idydb_flush(&handle)` saves without closing and retains the lock; `idydb_close(&handle)` saves and releases the handler. Saving writes and syncs complete ciphertext to a temporary sibling file before replacing the backing file. If saving fails, the handle stays open for inspection and retry. A directory-sync error can be reported after the replacement is already visible. Always check persistence status.

Plaintext flush and close also sync the containing directory, so retrying after a failed replacement durability check repeats that check even without another mutation.

`idydb_discard(&handle)` releases the handler and its locks without saving pending encrypted changes. It cannot undo a completed plaintext commit or an encrypted replacement already visible after a directory-sync error. Discard is also safe after a failed open or on a NULL handler. The Node wrapper exposes `flush()` and `discard()`; its finalizer attempts close and falls back to discard if saving fails. Use explicit close when the caller needs to observe persistence errors.

A writable encrypted open can migrate an existing valid plaintext file; a read-only encrypted open refuses that migration. Wrong passwords, authentication failures, and malformed data do not rewrite the backing file. Linux uses anonymous RAM-backed working storage; the Windows implementation uses a temporary plaintext file deleted on release.

Debug logging is disabled by default. Define `CUWACUNU_CAMAHJUCUNU_DB_VERBOSE_DEBUG` to opt in; encrypted values are redacted.

## Limits and behavior

| Sizing mode | Maximum row/column index |
| --- | --- |
| Tiny | 16 |
| Small | 256 |
| Big | 65,536 |

Strings may contain up to 65,532 bytes excluding the terminating NUL; embedded NULs are unsupported. Vectors support 1–16,383 float components. The Node addon rejects out-of-range, fractional, and nonfinite arguments before narrowing numeric values.

The file format is unchanged. It uses native integer/float encoding and assumes matching byte order and primitive sizes. Opens validate the complete record structure before enabling read-only mapping.

Each plaintext insert, replacement, or delete builds a complete private sibling file, syncs it, then atomically replaces the backing file. `idydb_rag_upsert_text` stages both text and vector in one snapshot, so an I/O or allocation failure cannot publish only one cell. Encrypted mutations similarly replace a private working image, which is saved by flush or close. Before replacement, failure leaves the previous image unchanged. After replacement, a directory-sync error can report failure while the complete new image is visible; inspect the retained handle before retrying.

This is a scan-based database: vector search is brute force. Updates copy the whole database and need temporary space proportional to its size. Atomic replacement requires write access to the containing directory and gives the file a new inode. POSIX owner, group, and permission bits are retained; hard-link aliases, ACLs, and extended attributes are not preserved as a group. Interrupted commits can leave unused `.tmp.*` siblings; the database path remains the authoritative image. Durability depends on the filesystem honoring file and directory sync operations. There is no general multi-call transaction API or write-ahead log.

Do not share a handler concurrently across threads. Advisory locks coordinate cooperating processes, with multiple read-only handles or one writer.

POSIX consumers that pin an existing directory can use `idydb_open_at(directory_fd, "data.db", &handle, flags)` for plaintext access. Open the directory with `O_RDONLY | O_DIRECTORY` so the descriptor can synchronize its entries; Linux `O_PATH` descriptors are rejected before any file is opened or created. It owns a close-on-exec duplicate of the directory descriptor and uses `openat`, `renameat`, and directory sync against that anchor; renaming the parent directory does not redirect writes. Pass a basename, and close the caller's descriptor after open if desired. Symlinks and non-regular entries are refused. Read-only pathname opens also support Linux `/proc/self/fd` snapshots whose original inode has been unlinked. The exchange storage adapter uses these interfaces; they do not provide general multi-call transactions.

Context length limits count bytes and can split a UTF-8 character. Embedding generation requires a caller-provided callback; no embedding model is bundled.

Regression suites cover mixed mutations, malformed records, maximum payloads, full columns, query ranking/filtering, metadata ownership, encryption authentication, C interoperability, and Node lifecycle/input validation. Failure tests inject short writes, disk-full errors, failed seeks/flushes/syncs/replacements, and interruptions before and after replacement; they verify atomic row pairs, borrowed-input safety, retry/discard behavior, and lock release after failed Node finalization.

## Credits

Based on the original work of [FlitDB](https://github.com/bradley499/flitdb), and used in project idyicyanere. See [LICENSE](LICENSE).
