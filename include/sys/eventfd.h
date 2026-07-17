#pragma once
#include <stdint.h>
#include <stddef.h>
#define EFD_CLOEXEC 02000000
inline int eventfd(unsigned int initval, int flags) { return -1; }
