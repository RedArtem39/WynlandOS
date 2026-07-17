#pragma once
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif
inline int ioctl(int fd, unsigned long request, ...) { return -1; }
#ifdef __cplusplus
}
#endif
