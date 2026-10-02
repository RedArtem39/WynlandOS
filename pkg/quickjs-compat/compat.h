#ifndef QUICKJS_COMPAT_H
#define QUICKJS_COMPAT_H

#include <wynland/types.h>
#include <wynland/heap.h>

/* Basic types */
typedef int64_t intptr_t;
typedef uint64_t uintptr_t;
typedef int64_t ptrdiff_t;

#ifndef INT64_MAX
#define INT64_MAX 9223372036854775807LL
#endif
#ifndef INTPTR_MAX
#define INTPTR_MAX INT64_MAX
#endif

#ifdef __SIZEOF_INT128__
#undef __SIZEOF_INT128__
#endif

/* NULL definition */
#ifndef NULL
#define NULL ((void*)0)
#endif

/* Standard assert macro */
#define assert(expr) ((expr) ? (void)0 : abort())

/* Map asm to __asm__ for standard C compliance */
#define asm __asm__

/* Map alloca to compiler builtin */
#define alloca(size) __builtin_alloca(size)

#ifdef __cplusplus
extern "C" {
#endif

/* Serial logger from kernel */
void serial_write_string(const char *str);

/* Memory mapping using functions to avoid macro clashes */
void *malloc(size_t size);
void free(void *ptr);
void *realloc(void *ptr, size_t size);
void *krealloc(void *ptr, size_t size);
size_t malloc_usable_size(void *ptr);

/* String/Memory functions */
void *memcpy(void *dest, const void *src, size_t n);
void *memset(void *s, int c, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
void *memchr(const void *s, int c, size_t n);
size_t strlen(const char *s);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);
char *strstr(const char *haystack, const char *needle);
char *strdup(const char *s);

/* GCC built-in varargs support */
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap) __builtin_va_end(ap)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define va_copy(dest, src) __builtin_va_copy(dest, src)

/* I/O Stubs */
typedef void FILE;
#define stdout ((FILE*)1)
#define stderr ((FILE*)2)
#define stdin  ((FILE*)3)

int printf(const char *format, ...);
int sprintf(char *str, const char *format, ...);
int snprintf(char *str, size_t size, const char *format, ...);
int vsnprintf(char *str, size_t size, const char *format, va_list ap);
int fprintf(FILE *stream, const char *format, ...);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int fputc(int c, FILE *stream);
int putchar(int c);

void abort(void);
void exit(int status);

/* setjmp.h Stubs */
typedef int jmp_buf[16];
#define setjmp(env) (0)
void longjmp(jmp_buf env, int val);

/* Error code support */
extern int errno;
#define EINVAL 22
#define ENOMEM 12
#define ERANGE 34
#define ETIMEDOUT 110

/* Format macros */
#define PRId64 "lld"
#define PRIu64 "llu"
#define PRIx64 "llx"
#define PRIu32 "u"
#define PRId32 "d"
#define PRIx32 "x"

/* Int limits and constants */
#ifndef INT32_MAX
#define INT32_MAX 2147483647
#endif
#ifndef INT32_MIN
#define INT32_MIN (-2147483647 - 1)
#endif
#ifndef UINT32_MAX
#define UINT32_MAX 4294967295U
#endif
#ifndef INT64_MAX
#define INT64_MAX 9223372036854775807LL
#endif
#ifndef INT64_MIN
#define INT64_MIN (-9223372036854775807LL - 1LL)
#endif
#ifndef UINT64_C
#define UINT64_C(c) c ## ULL
#endif
#ifndef SIZE_MAX
#define SIZE_MAX (~(size_t)0)
#endif

/* Character checking macros (ctype.h) / Math abs */
#define isdigit(c)  ((c) >= '0' && (c) <= '9')
#define isspace(c)  ((c) == ' ' || (c) == '\t' || (c) == '\r' || (c) == '\n' || (c) == '\v' || (c) == '\f')
#define isxdigit(c) (isdigit(c) || ((c) >= 'a' && (c) <= 'f') || ((c) >= 'A' && (c) <= 'F'))
#define isalpha(c)  (((c) >= 'a' && (c) <= 'z') || ((c) >= 'A' && (c) <= 'Z'))
#define isalnum(c)  (isalpha(c) || isdigit(c))
#define tolower(c)  (((c) >= 'A' && (c) <= 'Z') ? (c) - 'A' + 'a' : (c))
#define toupper(c)  (((c) >= 'a' && (c) <= 'z') ? (c) - 'a' + 'A' : (c))
#define abs(x)      ((x) < 0 ? -(x) : (x))

/* Time Support */
typedef int64_t time_t;

struct timespec {
    long tv_sec;
    long tv_nsec;
};

struct timeval {
    time_t tv_sec;
    long tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
    long tm_gmtoff;
    const char *tm_zone;
};

#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1

int clock_gettime(int clock_id, struct timespec *tp);
struct tm *localtime_r(const time_t *timep, struct tm *result);
int gettimeofday(struct timeval *tv, struct timezone *tz);

/* Atomics/Pthread stubs for QuickJS thread support */
typedef struct { int dummy; } pthread_cond_t;
typedef struct { int dummy; } pthread_mutex_t;
#define PTHREAD_MUTEX_INITIALIZER {0}

int pthread_cond_init(pthread_cond_t *cond, const void *attr);
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex);
int pthread_cond_destroy(pthread_cond_t *cond);
int pthread_cond_signal(pthread_cond_t *cond);
int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime);
int pthread_mutex_lock(pthread_mutex_t *mutex);
int pthread_mutex_unlock(pthread_mutex_t *mutex);

/* GCC standard built-in atomics mapping to emulate <stdatomic.h> */
#define _Atomic(type) type

#define atomic_fetch_add(ptr, val) __atomic_fetch_add(ptr, val, __ATOMIC_SEQ_CST)
#define atomic_fetch_and(ptr, val) __atomic_fetch_and(ptr, val, __ATOMIC_SEQ_CST)
#define atomic_fetch_or(ptr, val)  __atomic_fetch_or(ptr, val, __ATOMIC_SEQ_CST)
#define atomic_fetch_sub(ptr, val) __atomic_fetch_sub(ptr, val, __ATOMIC_SEQ_CST)
#define atomic_fetch_xor(ptr, val) __atomic_fetch_xor(ptr, val, __ATOMIC_SEQ_CST)
#define atomic_exchange(ptr, val)  __atomic_exchange_n(ptr, val, __ATOMIC_SEQ_CST)
#define atomic_load(ptr)           __atomic_load_n(ptr, __ATOMIC_SEQ_CST)
#define atomic_store(ptr, val)     __atomic_store_n(ptr, val, __ATOMIC_SEQ_CST)

#define atomic_compare_exchange_strong(ptr, expected, desired) \
    __atomic_compare_exchange_n(ptr, expected, desired, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)

/* Math helpers */
#define NAN (__builtin_nan(""))
#define INFINITY (__builtin_inf())
#define isnan(x) __builtin_isnan(x)
#define isinf(x) __builtin_isinf(x)
#define signbit(x) __builtin_signbit(x)
#define isfinite(x) __builtin_isfinite(x)

double floor(double x);
double ceil(double x);
double fabs(double x);
double pow(double x, double y);
double atan2(double y, double x);
double fmod(double x, double y);
double sin(double x);
double cos(double x);
double tan(double x);
double log(double x);
double exp(double x);
double sqrt(double x);
double modf(double x, double *iptr);
double cbrt(double x);
double trunc(double x);
double cosh(double x);
double sinh(double x);
double tanh(double x);
double acosh(double x);
double asinh(double x);
double atanh(double x);
double expm1(double x);
double log1p(double x);
double log2(double x);
double log10(double x);
double fmin(double x, double y);
double fmax(double x, double y);
double hypot(double x, double y);
double acos(double x);
double asin(double x);
double atan(double x);
double round(double x);
long lrint(double x);

#ifdef __cplusplus
}
#endif

#endif /* QUICKJS_COMPAT_H */
