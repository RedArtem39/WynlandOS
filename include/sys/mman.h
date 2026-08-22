#pragma once
#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>
#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#define PROT_EXEC 4
#define MAP_SHARED 0x01
#define MAP_PRIVATE 0x02
#define MAP_FILE 0
#define MAP_FIXED 0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON MAP_ANONYMOUS
#define MAP_NORESERVE 0x4000
#define MAP_POPULATE 0x8000
#define MAP_FAILED ((void *)-1)

#ifdef __cplusplus
extern "C" {
#endif

inline void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) { return MAP_FAILED; }
inline int munmap(void* addr, size_t length) { return 0; }
inline int mprotect(void* addr, size_t len, int prot) { return -1; }
inline int shm_open(const char *name, int oflag, mode_t mode) { return -1; }
inline int shm_unlink(const char *name) { return -1; }

#ifdef __cplusplus
}
#endif
