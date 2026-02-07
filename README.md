# IdyDB

**IdyDB** is a lightweight, high-performance C/C++ library designed for structured data storage and **Vector Search (RAG)**. Based on the architecture of FlitDB, it extends traditional flat-file database capabilities with modern AI requirements like **AES-256-GCM encryption**, **k-Nearest Neighbors (kNN) search**, and **automated text embedding**.

Whether you need a simple key-value store or a local vector database for LLM context injection, IdyDB provides a compact, single-header interface to manage your data.

---

## Key Features

* **Multi-Modal Storage**: Support for `INT`, `FLOAT`, `CHAR`, `BOOL`, and `VECTOR` (embeddings).
* **Vector Search & RAG**: Built-in support for Cosine and L2 similarity metrics.
* **Encrypted at Rest**: Secure your data using OpenSSL (AES-256-GCM + PBKDF2-HMAC-SHA256).
* **Scalable Architecture**: Three sizing modes to optimize memory:
* **TINY**: Up to 14.74 MB.
* **SMALL**: Up to 4.26 GB.
* **BIG**: Up to 281.47 TB.


* **Filtered Queries**: Perform kNN searches with metadata filters (EQ, NEQ, GT, LT, etc.).
* **Memory Efficient**: Optional memory mapping (`mmap`) support.

---

## 🛠 Installation

IdyDB is designed to be easily integrated into existing C or C++ projects.

1. **Dependencies**: If using encryption, ensure **OpenSSL** is installed on your system.
2. **Integration**: Include `db.h` in your project.
3. **Compilation**:
* For Windows DLLs, define `IDYDB_BUILD_DLL`.
* To enable large database support, ensure `IDYDB_SIZING_MODE` is set to `IDYDB_SIZING_MODE_BIG`.



---

## Quick Start

### 1. Basic Database Operations (C++)

```cpp
#include "db.h"

int main() {
    idydb *handle = nullptr;
    
    // Open or Create a database
    if (db::idydb_open("data.db", &handle, IDYDB_CREATE) == IDYDB_SUCCESS) {
        
        // Insert data at Column 1, Row 1
        db::idydb_insert(&handle, 1, 1, "Hello IdyDB");
        
        // Extract and Retrieve
        db::idydb_extract(&handle, 1, 1);
        char* value = db::idydb_retrieve_char(&handle);
        
        db::idydb_close(&handle);
    }
    return 0;
}

```

### 2. Vector Search (RAG)

IdyDB makes it easy to find similar vectors for Retrieval Augmented Generation:

```cpp
float query_vec[3] = {0.1f, 0.5f, 0.9f};
idydb_knn_result results[5];
char* retrieved_texts[5];

// Find top 5 most similar items in column 2 (vector column) 
// based on column 1 (text column)
idydb_rag_query_topk(&handle, 1, 2, query_vec, 3, 5, 
                     IDYDB_SIM_COSINE, results, retrieved_texts);

```

### 3. Encrypted Storage

```cpp
idydb_open_options opts = {0};
opts.encrypted_at_rest = true;
opts.passphrase = "your-secure-password";

idydb_open_with_options("secure.db", &handle, &opts);

```

---

## Sizing Modes

| Mode | Max Size | Offset Type |
| --- | --- | --- |
| **TINY** | 14.74 MB | `unsigned short` |
| **SMALL** | 4.26 GB | `unsigned short` |
| **BIG** | 281.47 TB | `unsigned long long` |

---

## 🛠 Technical Reference

### Supported Data Types

* `IDYDB_INTEGER`: Standard integer values.
* `IDYDB_FLOAT`: Floating point values.
* `IDYDB_CHAR`: String/Text data.
* `IDYDB_BOOL`: Boolean values.
* `IDYDB_VECTOR`: Array of `float32` (max 16,383 dimensions).

### Error Codes

* `IDYDB_SUCCESS (0)`: Operation successful.
* `IDYDB_BUSY (3)`: Database file is locked.
* `IDYDB_CORRUPT (5)`: File is malformed.
* `IDYDB_NOT_FOUND (4)`: File missing.

---

## 🤝 Credits

This project is based on the original work of [flitdb](https://github.com/bradley499/flitdb/tree/main) and is a core component of **project idyicyanere**.

Would you like me to generate a **CMakeLists.txt** file to help you compile this library with its OpenSSL dependencies?