#include "compat.h"

/* Define global errno */
int errno = 0;

/* Standard memory functions redirecting to kernel heap */
void *malloc(size_t size)
{
    return kmalloc(size);
}

void free(void *ptr)
{
    kfree(ptr);
}

void *realloc(void *ptr, size_t size)
{
    return krealloc(ptr, size);
}

/* krealloc implementation */
void *krealloc(void *ptr, size_t size)
{
    if (ptr == NULL) {
        return kmalloc(size);
    }
    if (size == 0) {
        kfree(ptr);
        return NULL;
    }

    size_t old_size = heap_get_block_size(ptr);

    if (size <= old_size) {
        return ptr;
    }

    void *new_ptr = kmalloc(size);
    if (!new_ptr) {
        return NULL;
    }

    memcpy(new_ptr, ptr, old_size);
    kfree(ptr);
    return new_ptr;
}

size_t malloc_usable_size(void *ptr)
{
    if (!ptr) return 0;
    return heap_get_block_size(ptr);
}

/* Standard memory functions */
void *memmove(void *dest, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dest;
    const unsigned char *s = (const unsigned char *)src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else if (d > s) {
        d += n;
        s += n;
        while (n--) *--d = *--s;
    }
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n)
{
    const unsigned char *p1 = (const unsigned char *)s1;
    const unsigned char *p2 = (const unsigned char *)s2;
    while (n--) {
        if (*p1 != *p2) return *p1 - *p2;
        p1++; p2++;
    }
    return 0;
}

void *memchr(const void *s, int c, size_t n)
{
    const unsigned char *p = (const unsigned char *)s;
    while (n--) {
        if (*p == (unsigned char)c) return (void *)p;
        p++;
    }
    return NULL;
}

/* Standard string functions */
size_t strlen(const char *s)
{
    size_t len = 0;
    while (*s++) len++;
    return len;
}

int strcmp(const char *s1, const char *s2)
{
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

int strncmp(const char *s1, const char *s2, size_t n)
{
    if (n == 0) return 0;
    while (n-- > 1 && *s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

char *strchr(const char *s, int c)
{
    while (*s != (char)c) {
        if (!*s++) return NULL;
    }
    return (char *)s;
}

char *strrchr(const char *s, int c)
{
    const char *last = NULL;
    do {
        if (*s == (char)c) last = s;
    } while (*s++);
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle)
{
    size_t n = strlen(needle);
    if (n == 0) return (char *)haystack;
    while (*haystack) {
        if (!memcmp(haystack, needle, n)) return (char *)haystack;
        haystack++;
    }
    return NULL;
}

char *strdup(const char *s)
{
    size_t len = strlen(s);
    char *dup = (char *)kmalloc(len + 1);
    if (dup) {
        memcpy(dup, s, len + 1);
    }
    return dup;
}

/* System process control */
void abort(void)
{
    serial_write_string("QuickJS: abort() called. System halted.\r\n");
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

void exit(int status)
{
    (void)status;
    serial_write_string("QuickJS: exit() called. System halted.\r\n");
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}

void longjmp(jmp_buf env, int val)
{
    (void)env;
    (void)val;
    serial_write_string("QuickJS: longjmp() called. System halted.\r\n");
    abort();
}

/* Simple helper: convert unsigned integer to string */
static void utoa(uint64_t val, char *buf, int base, int uppercase)
{
    char tmp[64];
    int i = 0;
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return;
    }
    while (val > 0) {
        uint64_t rem = val % base;
        if (rem < 10) {
            tmp[i++] = '0' + rem;
        } else {
            tmp[i++] = (uppercase ? 'A' : 'a') + (rem - 10);
        }
        val /= base;
    }
    int j = 0;
    while (i > 0) {
        buf[j++] = tmp[--i];
    }
    buf[j] = '\0';
}

/* Simple helper: convert signed integer to string */
static void itoa(int64_t val, char *buf, int base)
{
    if (val < 0) {
        *buf++ = '-';
        utoa((uint64_t)(-val), buf, base, 0);
    } else {
        utoa((uint64_t)val, buf, base, 0);
    }
}

/* Simple vsnprintf implementation */
int vsnprintf(char *str, size_t size, const char *format, va_list ap)
{
    if (size == 0) return 0;
    size_t pos = 0;
    while (*format && pos < size - 1) {
        if (*format == '%') {
            format++;
            if (*format == '\0') break;

            if (*format == '%') {
                str[pos++] = '%';
            } else if (*format == 'c') {
                char c = (char)va_arg(ap, int);
                str[pos++] = c;
            } else if (*format == 's') {
                const char *s = va_arg(ap, const char *);
                if (!s) s = "(null)";
                while (*s && pos < size - 1) {
                    str[pos++] = *s++;
                }
            } else if (*format == 'd' || *format == 'i') {
                int64_t val = va_arg(ap, int);
                char num_buf[32];
                itoa(val, num_buf, 10);
                char *p = num_buf;
                while (*p && pos < size - 1) {
                    str[pos++] = *p++;
                }
            } else if (*format == 'u') {
                uint64_t val = va_arg(ap, unsigned int);
                char num_buf[32];
                utoa(val, num_buf, 10, 0);
                char *p = num_buf;
                while (*p && pos < size - 1) {
                    str[pos++] = *p++;
                }
            } else if (*format == 'x' || *format == 'X') {
                uint64_t val = va_arg(ap, unsigned int);
                char num_buf[32];
                utoa(val, num_buf, 16, (*format == 'X'));
                char *p = num_buf;
                while (*p && pos < size - 1) {
                    str[pos++] = *p++;
                }
            } else if (*format == 'p') {
                uintptr_t val = (uintptr_t)va_arg(ap, void *);
                char num_buf[32];
                utoa(val, num_buf, 16, 0);
                if (pos < size - 3) {
                    str[pos++] = '0';
                    str[pos++] = 'x';
                    char *p = num_buf;
                    while (*p && pos < size - 1) {
                        str[pos++] = *p++;
                    }
                }
            } else if (*format == 'f') {
                double val = va_arg(ap, double);
                if (isnan(val)) {
                    const char *nan_str = "nan";
                    while (*nan_str && pos < size - 1) str[pos++] = *nan_str++;
                } else if (isinf(val)) {
                    const char *inf_str = val < 0 ? "-inf" : "inf";
                    while (*inf_str && pos < size - 1) str[pos++] = *inf_str++;
                } else {
                    if (val < 0) {
                        str[pos++] = '-';
                        val = -val;
                    }
                    int64_t integer_part = (int64_t)val;
                    double fraction_part = val - (double)integer_part;
                    char num_buf[32];
                    itoa(integer_part, num_buf, 10);
                    char *p = num_buf;
                    while (*p && pos < size - 1) {
                        str[pos++] = *p++;
                    }
                    if (pos < size - 2) {
                        str[pos++] = '.';
                        uint64_t frac = (uint64_t)(fraction_part * 1000000.0 + 0.5);
                        utoa(frac, num_buf, 10, 0);
                        p = num_buf;
                        /* Pad with zeros if needed */
                        int frac_len = strlen(num_buf);
                        int pad = 6 - frac_len;
                        while (pad-- > 0 && pos < size - 1) {
                            str[pos++] = '0';
                        }
                        while (*p && pos < size - 1) {
                            str[pos++] = *p++;
                        }
                    }
                }
            }
        } else {
            str[pos++] = *format;
        }
        format++;
    }
    str[pos] = '\0';
    return (int)pos;
}

int snprintf(char *str, size_t size, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int ret = vsnprintf(str, size, format, ap);
    va_end(ap);
    return ret;
}

int sprintf(char *str, const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    int ret = vsnprintf(str, 100000, format, ap); /* Assumed large buffer */
    va_end(ap);
    return ret;
}

int printf(const char *format, ...)
{
    char buf[1024];
    va_list ap;
    va_start(ap, format);
    int ret = vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    serial_write_string(buf);
    return ret;
}

int fprintf(FILE *stream, const char *format, ...)
{
    (void)stream;
    char buf[1024];
    va_list ap;
    va_start(ap, format);
    int ret = vsnprintf(buf, sizeof(buf), format, ap);
    va_end(ap);
    serial_write_string(buf);
    return ret;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream)
{
    (void)stream;
    size_t total = size * nmemb;
    if (total > 0) {
        char *buf = (char *)kmalloc(total + 1);
        if (buf) {
            memcpy(buf, ptr, total);
            buf[total] = '\0';
            serial_write_string(buf);
            kfree(buf);
        }
    }
    return nmemb;
}

int fputc(int c, FILE *stream)
{
    (void)stream;
    char buf[2] = {(char)c, '\0'};
    serial_write_string(buf);
    return c;
}

int putchar(int c)
{
    char buf[2] = {(char)c, '\0'};
    serial_write_string(buf);
    return c;
}

/* Math functions implementations */
double fabs(double x)
{
    return x < 0 ? -x : x;
}

double floor(double x)
{
    if (isnan(x) || isinf(x)) return x;
    int64_t i = (int64_t)x;
    return (double)(x < i ? i - 1 : i);
}

double ceil(double x)
{
    if (isnan(x) || isinf(x)) return x;
    int64_t i = (int64_t)x;
    return (double)(x > i ? i + 1 : i);
}

double fmod(double x, double y)
{
    if (y == 0.0 || isnan(x) || isnan(y)) return NAN;
    double ratio = x / y;
    return x - (double)((int64_t)ratio) * y;
}

double pow(double x, double y)
{
    if (y == 0.0) return 1.0;
    if (y == 1.0) return x;
    if (isnan(x) || isnan(y)) return NAN;

    int64_t iy = (int64_t)y;
    if ((double)iy == y) {
        double res = 1.0;
        double base = x;
        uint64_t exp_val = iy < 0 ? -iy : iy;
        while (exp_val > 0) {
            if (exp_val & 1) res *= base;
            base *= base;
            exp_val >>= 1;
        }
        return iy < 0 ? 1.0 / res : res;
    }
    return 0.0;
}

double modf(double x, double *iptr)
{
    if (isnan(x) || isinf(x)) {
        *iptr = x;
        return 0.0;
    }
    int64_t i = (int64_t)x;
    *iptr = (double)i;
    return x - (double)i;
}

double cbrt(double x)
{
    (void)x;
    return 0.0;
}

double trunc(double x)
{
    return x < 0.0 ? ceil(x) : floor(x);
}

/* Trig, Log and Hyperbolic stubs */
double cosh(double x) { (void)x; return 0.0; }
double sinh(double x) { (void)x; return 0.0; }
double tanh(double x) { (void)x; return 0.0; }
double acosh(double x) { (void)x; return 0.0; }
double asinh(double x) { (void)x; return 0.0; }
double atanh(double x) { (void)x; return 0.0; }
double expm1(double x) { (void)x; return 0.0; }
double log1p(double x) { (void)x; return 0.0; }
double log2(double x) { (void)x; return 0.0; }
double log10(double x) { (void)x; return 0.0; }

double fmin(double x, double y) { return x < y ? x : y; }
double fmax(double x, double y) { return x > y ? x : y; }
double hypot(double x, double y) { (void)x; (void)y; return 0.0; }
double acos(double x) { (void)x; return 0.0; }
double asin(double x) { (void)x; return 0.0; }
double atan(double x) { (void)x; return 0.0; }

double round(double x)
{
    if (isnan(x) || isinf(x)) return x;
    return x < 0.0 ? ceil(x - 0.5) : floor(x + 0.5);
}

long lrint(double x)
{
    return (long)round(x);
}

/* Atomics / Pthread Stubs */
int pthread_cond_init(pthread_cond_t *cond, const void *attr) { (void)cond; (void)attr; return 0; }
int pthread_cond_wait(pthread_cond_t *cond, pthread_mutex_t *mutex) { (void)cond; (void)mutex; return 0; }
int pthread_cond_destroy(pthread_cond_t *cond) { (void)cond; return 0; }
int pthread_cond_signal(pthread_cond_t *cond) { (void)cond; return 0; }
int pthread_cond_timedwait(pthread_cond_t *cond, pthread_mutex_t *mutex, const struct timespec *abstime) { (void)cond; (void)mutex; (void)abstime; return 0; }
int clock_gettime(int clock_id, struct timespec *tp) { (void)clock_id; if (tp) { tp->tv_sec = 0; tp->tv_nsec = 0; } return 0; }

int pthread_mutex_lock(pthread_mutex_t *mutex) { (void)mutex; return 0; }
int pthread_mutex_unlock(pthread_mutex_t *mutex) { (void)mutex; return 0; }

int gettimeofday(struct timeval *tv, struct timezone *tz)
{
    if (tv) {
        tv->tv_sec = 0;
        tv->tv_usec = 0;
    }
    if (tz) {
        tz->tz_minuteswest = 0;
        tz->tz_dsttime = 0;
    }
    return 0;
}

struct tm *localtime_r(const time_t *timep, struct tm *result)
{
    (void)timep;
    if (result) {
        memset(result, 0, sizeof(struct tm));
        result->tm_mday = 1;
        result->tm_gmtoff = 0;
        result->tm_zone = "UTC";
    }
    return result;
}

double atan2(double y, double x) { (void)y; (void)x; return 0.0; }
double sin(double x) { (void)x; return 0.0; }
double cos(double x) { (void)x; return 0.0; }
double tan(double x) { (void)x; return 0.0; }
double log(double x) { (void)x; return 0.0; }
double exp(double x) { (void)x; return 0.0; }
double sqrt(double x) { return __builtin_sqrt(x); }
