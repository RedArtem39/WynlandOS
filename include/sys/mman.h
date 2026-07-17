#pragma once
#include <stdint.h>
#include <stddef.h>
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_FAILED ((void *)-1)

#ifdef __cplusplus
extern "C" {
#endif

inline void* mmap(void* addr, size_t length, int prot, int flags, int fd, off_t offset) { return MAP_FAILED; }
inline int munmap(void* addr, size_t length) { return 0; }
inline int shm_open(const char *name, int oflag, mode_t mode) { return -1; }
inline int shm_unlink(const char *name) { return -1; }

#ifdef __cplusplus
}
#endif
