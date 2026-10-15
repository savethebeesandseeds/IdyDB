#ifndef IDYDB_PLATFORM_H
#define IDYDB_PLATFORM_H

#include <cstdint>
#include <cstdio>

// Private storage interface. Public records and queries never depend on OS handles.
namespace idydb_platform {
enum lock_operation { shared = 1, exclusive = 2, nonblocking = 4, unlock = 8 };
int seek(FILE* stream, std::int64_t offset, int whence);
std::int64_t tell(FILE* stream);
int descriptor(FILE* stream);
int resize(FILE* stream, std::uint64_t size);
int sync(int descriptor);
int lock(int descriptor, int operation);
FILE* open_locked(const char* filename, int flags, int* status, int directory_fd = -1);
char* canonical_path(FILE* opened, const char* filename);
bool exists(const char* filename);
FILE* secure_plain_stream(const char** kind);
FILE* stage(FILE* backing, const char* filename, int directory_fd, char** path);
void remove_stage(const char* path, int directory_fd);
bool replace(FILE* current, FILE* replacement, const char* stage, const char* filename, int directory_fd);
bool sync_directory(const char* filename, int directory_fd);
void* map_readonly(FILE* stream, std::uint64_t size, void** cookie);
void unmap(void* view, std::uint64_t size, void* cookie);
int duplicate_directory(int descriptor);
void close_directory(int descriptor);
#ifdef IDYDB_TESTING
int fault(int point);
#endif
}

#ifdef IDYDB_TESTING
#include "../tests/failure_hooks.h"
#define IDYDB_IO_FAULT(point) idydb_platform::fault(point)
#else
#define IDYDB_IO_FAULT(point) 0
#endif

#endif
