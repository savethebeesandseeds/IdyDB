# Record compatibility fixtures

These small, fixed files were produced by `idydb_compatibility_tests` on
little-endian x64 Linux. Tests read both files, write equivalent new files,
compare the exact plaintext encoding, and verify the encrypted payload through
the public API. The native Windows test runner also exchanges independently
written images with the Linux build using `--write DIRECTORY` and `--read DIRECTORY`.

The fixture passphrase is deliberately public:
`public-idydb-v1-compatibility-fixture`. These are synthetic test records.

| File | SHA-256 |
| --- | --- |
| `portable-v1.db` | `6b573cdf9d88b3e559630dcf8058c3b8abf5bb7f903cfb9e84d223080ca50278` |
| `portable-v1.encrypted.db` | `61cae19f4679d6b1b0dc251a6a80bdbb18c0adbf9c941d3befcd49efe4b608f8` |

Records cover signed integer limits, negative integers, positive and negative
floats, signed zero, UTF-8 Arabic/accented/emoji text, CRLF and `0x1a` bytes,
an empty string, both booleans, float vectors, deleted cells, and RAG text/vector
pairs. The encrypted fixture uses the version 1 AES-256-GCM envelope and 10,000
PBKDF2 iterations to keep the regression suite fast. Its salt and nonce are
fixed only by retaining the originally generated file; normal writes continue
to generate fresh random values.

The current plaintext format assumes 8-bit bytes, 16-bit shorts, 32-bit integers,
IEEE-754 binary32 floats and matching byte order. The compatibility test checks
these assumptions. No platform ABI structs or native pointers are serialized.

The default fixture directory comes from CMake. Set `IDYDB_FIXTURE_DIR` to an
absolute UTF-8 path when running a cross-compiled executable on another OS.
Do not regenerate these files to make an encoding regression pass. An
intentional format change needs an explicit version/migration decision.
