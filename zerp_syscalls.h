/*
 * WynlandOS / Zerp - shared raw syscall wrappers for freestanding,
 * no-libc binaries (the compositor, zerp_client.h, and every demo app).
 * Same style as spawner.c/shm_test_parent.c's sys3/sys6 helpers, pulled
 * into one place so every Zerp-side binary doesn't hand-roll its own copy.
 * All `static` -- safe to #include from multiple binaries' single
 * translation units without linkage conflicts.
 */
#ifndef ZERP_SYSCALLS_H
#define ZERP_SYSCALLS_H

#include <stdint.h> /* compiler-provided freestanding header, safe with -nostdlib */

#define SYS_read       0
#define SYS_write      1
#define SYS_close      3
#define SYS_open       2
#define SYS_mmap       9
#define SYS_ioctl      16
#define SYS_getdents64 217
#define SYS_pipe       22
#define SYS_yield      24
#define SYS_exit       60
#define SYS_fcntl      72
#define SYS_getuid     102
#define SYS_spawn      405
#define SYS_fb_flush   406
#define SYS_shm_create 407
#define SYS_spawn_argv 408
#define SYS_wynland_elevate 409
#define SYS_pty_create 410
#define SYS_process_alive 411
#define SYS_mouse_events 412
#define SYS_dup2       33
#define SYS_execve     59
#define SYS_fork       57

#define F_SETFD    2
#define F_SETFL    4
#define FD_CLOEXEC 1

#define O_RDONLY   00
#define O_RDWR     02
#define O_NONBLOCK 04000

#define PROT_READ  0x1
#define PROT_WRITE 0x2
#define MAP_SHARED 0x1

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

struct zerp_fb_var_screeninfo {
    uint32_t xres, yres;
    uint32_t xres_virtual, yres_virtual;
    uint32_t xoffset, yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
    uint8_t  _pad[120];
};
struct zerp_fb_fix_screeninfo {
    char     id[16];
    uint64_t smem_start;
    uint32_t smem_len;
    uint32_t type;
    uint32_t type_aux;
    uint32_t visual;
    uint16_t xpanstep, ypanstep, ywrapstep;
    uint32_t line_length;
    uint8_t  _pad[32];
};

static long zsys3(long num, long a1, long a2, long a3) {
    long ret;
    __asm__ volatile(
        "movq %1, %%rax\n"
        "movq %2, %%rdi\n"
        "movq %3, %%rsi\n"
        "movq %4, %%rdx\n"
        "syscall\n"
        "movq %%rax, %0\n"
        : "=r"(ret)
        : "r"(num), "r"(a1), "r"(a2), "r"(a3)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
    return ret;
}

static long zsys6(long num, long a1, long a2, long a3, long a4, long a5, long a6) {
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8  __asm__("r8")  = a5;
    register long r9  __asm__("r9")  = a6;
    __asm__ volatile(
        "movq %1, %%rax\n"
        "movq %2, %%rdi\n"
        "movq %3, %%rsi\n"
        "movq %4, %%rdx\n"
        "syscall\n"
        "movq %%rax, %0\n"
        : "=r"(ret)
        : "r"(num), "r"(a1), "r"(a2), "r"(a3), "r"(r10), "r"(r8), "r"(r9)
        : "rax", "rdi", "rsi", "rdx", "rcx", "r11", "memory"
    );
    return ret;
}

static long zwrite(int fd, const void *buf, long len) { return zsys3(SYS_write, fd, (long)buf, len); }
static long zread(int fd, void *buf, long len) { return zsys3(SYS_read, fd, (long)buf, len); }
static long zpipe(int *pfd) { return zsys3(SYS_pipe, (long)pfd, 0, 0); }
static long zopen(const char *path, long flags) { return zsys3(SYS_open, (long)path, flags, 0); }
static void zclose(int fd) { zsys3(SYS_close, fd, 0, 0); }
static long zioctl(int fd, long req, void *arg) { return zsys3(SYS_ioctl, fd, req, (long)arg); }
static void zyield(void) { zsys3(SYS_yield, 0, 0, 0); }
static void zexit(int code) { zsys3(SYS_exit, code, 0, 0); }
static long zfcntl(int fd, int cmd, long arg) { return zsys3(SYS_fcntl, fd, cmd, arg); }
static long zgetuid(void) { return zsys3(SYS_getuid, 0, 0, 0); }
static long zelevate(const char *password) { return zsys3(SYS_wynland_elevate, (long)password, 0, 0); }
static long zgetdents64(int fd, void *dirp, long count) { return zsys3(SYS_getdents64, fd, (long)dirp, count); }
static long zspawn_argv(const char *path, const char **argv) { return zsys3(SYS_spawn_argv, (long)path, (long)argv, 0); }
static long zshm_create(long size) { return zsys3(SYS_shm_create, size, 0, 0); }
static void *zmmap_shm(long fd, long size) {
    return (void *)zsys6(SYS_mmap, 0, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
}
static void zfb_flush(int x, int y, int w, int h) { zsys6(SYS_fb_flush, x, y, w, h, 0, 0); }

/* ---- Timing / waiting (the kernel clock is 1 kHz: real ms resolution) ---- */
#define SYS_poll_nr          7
#define SYS_nanosleep_nr     35
#define SYS_clock_gettime_nr 228
#define ZCLOCK_MONOTONIC     1

/* Monotonic milliseconds since boot. */
static uint64_t zclock_ms(void) {
    struct { long sec, nsec; } ts = { 0, 0 };
    zsys3(SYS_clock_gettime_nr, ZCLOCK_MONOTONIC, (long)&ts, 0);
    return (uint64_t)ts.sec * 1000u + (uint64_t)ts.nsec / 1000000u;
}

/* A genuine sleep (the thread is parked, not spinning). */
static void zsleep_ms(long ms) {
    struct { long sec, nsec; } ts = { ms / 1000, (ms % 1000) * 1000000L };
    zsys3(SYS_nanosleep_nr, (long)&ts, 0, 0);
}

/* Block until `fd` is readable or `timeout_ms` passes (-1 = forever).
   Single-fd poll()s sleep on the pipe's own wait queue in the kernel, so
   an idle client costs zero CPU instead of a zyield() spin. */
struct zpollfd { int fd; short events; short revents; };
static long zpoll_in(int fd, long timeout_ms) {
    struct zpollfd p = { fd, 0x0001 /* POLLIN */, 0 };
    return zsys3(SYS_poll_nr, (long)&p, 1, timeout_ms);
}

/* Absolute pointer events from the kernel (x, y, button mask after each
   PS/2 packet). Exclusive to the first caller; returns the number read. */
typedef struct { int32_t x, y; uint32_t buttons; } ZMouseEvent;
static long zmouse_events(ZMouseEvent *buf, long max) { return zsys3(SYS_mouse_events, (long)buf, max, 0); }

/* Fast 32-bit pixel copy (rep movsl) for blits. */
static inline void zcopy32(uint32_t *dst, const uint32_t *src, uint32_t count) {
    __asm__ volatile("rep movsl" : "+D"(dst), "+S"(src), "+c"(count) : : "memory");
}
static inline void zfill32(uint32_t *dst, uint32_t value, uint32_t count) {
    __asm__ volatile("rep stosl" : "+D"(dst), "+c"(count) : "a"(value) : "memory");
}

/* Phase 18: real PTY + fork()+dup2()+execve() so zerp_term.c can run a
   genuine interactive child (nano) with its stdio wired to a real PTY,
   the same idiom every real terminal emulator uses. */
static long zpty_create(int fds[2]) { return zsys3(SYS_pty_create, (long)fds, 0, 0); }
static long zdup2(int oldfd, int newfd) { return zsys3(SYS_dup2, oldfd, newfd, 0); }
static long zfork(void) { return zsys3(SYS_fork, 0, 0, 0); }
static long zexecve(const char *path, const char **argv, const char **envp) {
    return zsys3(SYS_execve, (long)path, (long)argv, (long)envp);
}
static long zprocess_alive(long pid) { return zsys3(SYS_process_alive, pid, 0, 0); }

/* GCC's optimizer can turn a plain element-copy loop into an implicit
   call to the standard `memcpy`/`memset` symbols (e.g. zerp_term.c's
   pixel-blit loop in redraw()) -- there's no libc linked in these
   -nostdlib freestanding binaries to provide them. Each Zerp binary is
   its own single-file compilation (one .c file, no separate .o linking),
   so defining these with external linkage here is safe: never more than
   one definition ends up in any given link. */
/* -fno-tree-loop-distribute-patterns on these three definitions specifically
   (not the whole translation unit) -- without it, GCC's loop-idiom pass
   recognizes each function's OWN body as "the memcpy/memset/strlen
   pattern" and rewrites it into a call to the symbol of that very same
   name, i.e. infinite self-recursion. This was latent since these were
   first written (every prior caller's loop got rewritten INTO a call to
   these, which is fine and intended) until zerp_vt100.h's vt_reset()
   became the first fill-loop big/regular enough that GCC ALSO rewrote
   memset()'s own body this way -- confirmed via objdump: `memset` compiled
   to `movabs $memset, %rax; call *%rax`, self-recursive, observed live as
   a real Page Fault (CR2 = stack floor - 8) from exhausting an 8MB stack
   in ~524288 recursive calls. */
__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memcpy(void *dst, const void *src, unsigned long n) {
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    for (unsigned long i = 0; i < n; i++) d[i] = s[i];
    return dst;
}
__attribute__((optimize("no-tree-loop-distribute-patterns")))
void *memset(void *dst, int val, unsigned long n) {
    unsigned char *d = (unsigned char *)dst;
    for (unsigned long i = 0; i < n; i++) d[i] = (unsigned char)val;
    return dst;
}
__attribute__((optimize("no-tree-loop-distribute-patterns")))
unsigned long strlen(const char *s) {
    unsigned long n = 0;
    while (s[n]) n++;
    return n;
}

static int zstrlen(const char *s) { return (int)strlen(s); }
static long zstrtol(const char *s) {
    long n = 0;
    while (*s >= '0' && *s <= '9') { n = n * 10 + (*s - '0'); s++; }
    return n;
}
static void znum_to_str(long n, char *out) {
    char rev[24]; int r = 0, i = 0;
    if (n == 0) { out[0] = '0'; out[1] = '\0'; return; }
    while (n > 0) { rev[r++] = '0' + (n % 10); n /= 10; }
    while (r > 0) out[i++] = rev[--r];
    out[i] = '\0';
}

#endif /* ZERP_SYSCALLS_H */
