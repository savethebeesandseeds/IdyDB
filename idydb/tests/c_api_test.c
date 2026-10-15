#include "db.h"
#include <stdio.h>

int main(void)
{
    idydb* handle = NULL;
    if (idydb_close(&handle) != IDYDB_DONE) return 1;
    if (idydb_extract(&handle, 1, 1) != IDYDB_ERROR) return 2;
    if (idydb_retrieved_type(&handle) != IDYDB_NULL) return 3;
    if (!idydb_errmsg(NULL)) return 4;
    if (idydb_version_check() != IDYDB_VERSION) return 5;
    if (idydb_flush(&handle) != IDYDB_ERROR) return 6;
    if (idydb_discard(&handle) != IDYDB_DONE) return 7;
    {
        void* buffer = idydb_alloc(32);
        if (!buffer) return 8;
        idydb_free(buffer);
    }
    puts("C API passed");
    return 0;
}
