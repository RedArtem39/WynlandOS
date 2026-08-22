#pragma once
#include <stdint.h>

typedef uint32_t __u32;
typedef int32_t __s32;
typedef uint16_t __u16;
typedef int16_t __s16;
typedef uint8_t __u8;
typedef int8_t __s8;
typedef uint64_t __u64;
typedef int64_t __s64;

#ifndef __CHECKER__
#define __bitwise__
#define __bitwise
#define __force
#define __always_inline inline __attribute__((always_inline))
#else
#define __bitwise__ __attribute__((bitwise))
#define __bitwise __bitwise__
#define __force __attribute__((force))
#endif

typedef __u16 __bitwise __le16;
typedef __u16 __bitwise __be16;
typedef __u32 __bitwise __le32;
typedef __u32 __bitwise __be32;
typedef __u64 __bitwise __le64;
typedef __u64 __bitwise __be64;
typedef __u16 __bitwise __sum16;
typedef __u32 __bitwise __wsum;
