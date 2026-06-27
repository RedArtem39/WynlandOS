/*
 * WynlandOS - Basic type definitions
 * Freestanding environment - no libc available
 */

#pragma once

/* Unsigned integers */
typedef unsigned char      uint8_t;
typedef unsigned short     uint16_t;
typedef unsigned int       uint32_t;
typedef unsigned long long uint64_t;

/* Signed integers */
typedef signed char        int8_t;
typedef signed short       int16_t;
typedef signed int         int32_t;
typedef signed long long   int64_t;

/* Size types */
typedef uint64_t  size_t;
typedef int64_t   ssize_t;
typedef uint64_t  uintptr_t;
typedef int64_t   intptr_t;
typedef int64_t   ptrdiff_t;

#ifndef __cplusplus
typedef _Bool bool;
#define true  1
#define false 0
#endif

/* Null */
#define NULL ((void*)0)

/* Compiler attributes */
#define PACKED       __attribute__((packed))
#define ALIGNED(n)   __attribute__((aligned(n)))
#define NORETURN     __attribute__((noreturn))
#define UNUSED       __attribute__((unused))
#define INLINE       static inline __attribute__((always_inline))

/* Bit manipulation */
#define BIT(n)       (1ULL << (n))
#define KB(n)        ((n) * 1024ULL)
#define MB(n)        ((n) * 1024ULL * 1024ULL)
#define GB(n)        ((n) * 1024ULL * 1024ULL * 1024ULL)

/* Min/Max */
#define MIN(a, b)    ((a) < (b) ? (a) : (b))
#define MAX(a, b)    ((a) > (b) ? (a) : (b))

/* Standard Memory functions */
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
