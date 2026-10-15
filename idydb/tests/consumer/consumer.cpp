#include <db.h>
#include <cstdio>

int main() {
    const char* path = "installed-consumer-cpp.db";
    db::idydb* handle = nullptr;
    std::remove(path);
    bool ok = db::idydb_open(path, &handle, IDYDB_CREATE) == IDYDB_SUCCESS;
    ok = ok && db::idydb_insert(&handle, 1, 1, 42) == IDYDB_DONE;
    ok = ok && db::idydb_flush(&handle) == IDYDB_DONE;
    ok = ok && db::idydb_extract(&handle, 1, 1) == IDYDB_DONE;
    ok = ok && db::idydb_retrieve_int(&handle) == 42;
    if (db::idydb_close(&handle) != IDYDB_DONE) ok = false;
    db::idydb_discard(&handle);
    std::remove(path);
    return ok ? 0 : 1;
}
