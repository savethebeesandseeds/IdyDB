#ifndef IDYDB_TEST_FAILURE_HOOKS_H
#define IDYDB_TEST_FAILURE_HOOKS_H

/* Private to the separately compiled instrumented test library. */
enum {
    IDYDB_TEST_COPY_READ = 1,
    IDYDB_TEST_COPY_WRITE = 2,
    IDYDB_TEST_CELL_WRITE = 3,
    IDYDB_TEST_SEEK = 4,
    IDYDB_TEST_FLUSH = 5,
    IDYDB_TEST_FILE_SYNC = 6,
    IDYDB_TEST_REPLACE = 7,
    IDYDB_TEST_DIRECTORY_SYNC = 8,
    IDYDB_TEST_BEFORE_REPLACE = 9,
    IDYDB_TEST_AFTER_REPLACE = 10,
    IDYDB_TEST_WRITE = 11,
    IDYDB_TEST_LOCK = 12
};

typedef int (*idydb_test_hook)(int stage, void* user);

#ifdef __cplusplus
extern "C" {
#endif
void idydb_test_set_hook(idydb_test_hook hook, void* user);
#ifdef __cplusplus
}
#endif

#endif
