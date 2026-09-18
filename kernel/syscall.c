#include <wynland/types.h>
#include <wynland/sched.h>
#include <wynland/vfs.h>
#include <wynland/vmm.h>
#include <wynland/pmm.h>
#include <wynland/boot_info.h>
#include <wynland/heap.h>
#include <wynland/process.h>
#include <wynland/tcp.h>
#include <wynland/udpsock.h>
#include <wynland/futex.h>
#include <wynland/signal.h>
#include <wynland/rtc.h>
#include <wynland/elf.h>
#include <wynland/waitqueue.h>
#include <wynland/usercopy.h>

extern uint64_t timer_get_ticks(void);

/* Magic first_cluster sentinels for real AF_INET sockets (distinct from
   the pre-existing 0xFFFFFFF8 AF_UNIX mock, which stays untouched for
   callers like libwayland). current_cluster holds the TCP
   connection-table / UDP-socket-table slot index. For TCP it's set to
   TCP_FD_NOT_CONNECTED right after socket() until connect() succeeds;
   for UDP the slot is allocated immediately in socket() since UDP has
   no handshake. */
#define SOCK_FD_TCP             0xFFFFFFE8
#define SOCK_FD_UDP             0xFFFFFFE7
#define TCP_FD_NOT_CONNECTED    0xFFFFFFFF

/* Standard Linux ABI layout, used by connect()/sendto()/recvfrom() for
   both TCP and UDP sockets. */
struct linux_sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;   // network byte order
    uint32_t sin_addr;   // network byte order
    uint8_t  sin_zero[8];
};

/* Phase 18: real PTY primitive for interactive programs (nano, and later
   git/vim) run inside zerp_term.c. Sentinels 0xFFFFFFE5/E6 -- the
   0xFFFFFFF0-FD block is fully occupied (with some pre-existing,
   documented, harmless collisions from earlier phases) and E7/E8 are
   already claimed by the UDP/TCP socket work, so this picks fresh,
   collision-free values rather than adding a third collision onto an
   already-shared sentinel. */
#define PTY_FD_MASTER 0xFFFFFFE6
#define PTY_FD_SLAVE  0xFFFFFFE5

#define PTY_BUF_SIZE  4096
#define MAX_PTYS      8
#define NCCS_LOCAL    32

/* Byte-for-byte match of musl's real struct termios (bits/termios.h):
   tcflag_t/speed_t = uint32_t, cc_t = uint8_t, NCCS = 32. Any caller
   (ncurses' tcgetattr/tcsetattr) reads/writes this exact layout via
   TCGETS/TCSETS, so the field order and sizes must match precisely --
   not guessed. */
struct linux_termios {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[NCCS_LOCAL];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
};

struct linux_winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

typedef struct {
    uint8_t  m2s_buf[PTY_BUF_SIZE]; /* terminal -> child (keystrokes) */
    uint32_t m2s_head, m2s_tail, m2s_count;
    uint8_t  s2m_buf[PTY_BUF_SIZE]; /* child -> terminal (screen output) */
    uint32_t s2m_head, s2m_tail, s2m_count;
    struct linux_termios term;
    uint16_t ws_row, ws_col;
    int      refcount; /* master and slave each hold a ref; freed at 0 --
                           needed because after fork() they typically end
                           up in different processes, closing independently */
    bool     in_use;
    /* Phase 22d: real blocking, same bounded-retry safety net as KPipe
       (see pipe_read/pipe_write's own comment -- neither end here tracks
       peer-closed either). */
    WaitQueue s2m_wq; /* master read() / slave write() block here */
    WaitQueue m2s_wq; /* slave read() / master write() block here */
} Pty;

static Pty *g_ptys[MAX_PTYS];

static uint32_t pty_ring_write(uint8_t *buf, uint32_t *head, uint32_t *tail, uint32_t *count, const void *data, uint32_t size) {
    (void)head;
    const uint8_t *src = (const uint8_t *)data;
    uint32_t written = 0;
    while (written < size && *count < PTY_BUF_SIZE) {
        buf[*tail] = src[written];
        *tail = (*tail + 1) % PTY_BUF_SIZE;
        (*count)++;
        written++;
    }
    return written;
}

static uint32_t pty_ring_read(uint8_t *buf, uint32_t *head, uint32_t *tail, uint32_t *count, void *data, uint32_t size) {
    (void)tail;
    uint8_t *dst = (uint8_t *)data;
    uint32_t got = 0;
    while (got < size && *count > 0) {
        dst[got] = buf[*head];
        *head = (*head + 1) % PTY_BUF_SIZE;
        (*count)--;
        got++;
    }
    return got;
}

/* Real musl termios flag values (bits/termios.h) -- defined locally
   since the kernel doesn't include musl's own headers. */
#define T_ICRNL   0000400
#define T_IXON    0002000
#define T_OPOST   0000001
#define T_ONLCR   0000004
#define T_CS8     0000060
#define T_CREAD   0000200
#define T_CLOCAL  0004000
#define T_ISIG    0000001
#define T_ICANON  0000002
#define T_ECHO    0000010
#define T_ECHOE   0000020
#define T_ECHOK   0000040
#define T_IEXTEN  0100000

#define EPOLLIN  0x00000001
#define EPOLLPRI 0x00000002
#define EPOLLOUT 0x00000004
#define EPOLLERR 0x00000008
#define EPOLLHUP 0x00000010

#define POLLIN  0x0001
#define POLLPRI 0x0002
#define POLLOUT 0x0004
#define POLLERR 0x0008
#define POLLHUP 0x0010
#define POLLNVAL 0x0020

/*
 * Linux x86_64 stat structure (from asm-generic/stat.h)
 * Must match the exact layout that musl / glibc expects.
 */
struct linux_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t  st_size;
    int64_t  st_blksize;
    int64_t  st_blocks;
    uint64_t st_atime_sec;
    uint64_t st_atime_nsec;
    uint64_t st_mtime_sec;
    uint64_t st_mtime_nsec;
    uint64_t st_ctime_sec;
    uint64_t st_ctime_nsec;
    int64_t  __unused[3];
};

/* Linux file mode constants */
#define S_IFREG  0100000
#define S_IFDIR  0040000
#define S_IFCHR  0020000

/* fcntl commands */
#define F_DUPFD   0
#define F_GETFD   1
#define F_SETFD   2
#define F_GETFL   3
#define F_SETFL   4

/* Linux O_flags for fcntl */
#define LINUX_O_RDONLY    0
#define LINUX_O_WRONLY    1
#define LINUX_O_RDWR      2

typedef struct {
    uint64_t r15;
    uint64_t r14;
    uint64_t r13;
    uint64_t r12;
    uint64_t r11;
    uint64_t r10;
    uint64_t r9;
    uint64_t r8;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbx;
    uint64_t rbp;
    uint64_t rip;
    uint64_t rflags;
    uint64_t rsp;
} SyscallRegs;

#define MSR_EFER       0xC0000080
#define MSR_STAR       0xC0000081
#define MSR_LSTAR      0xC0000082
#define MSR_SFMASK     0xC0000084

#ifndef PAGE_SIZE
#define PAGE_SIZE 4096
#endif

void syscall_entry(void);
void serial_write_string(const char *str);
void uint_to_hex(uint64_t val, char *buf);
void uint_to_str(uint64_t val, char *buf);

static void str_copy(char *dst, const char *src) {
    while (*src) {
        *dst++ = *src++;
    }
    *dst = '\0';
}

static int __attribute__((unused)) str_compare(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return *(const unsigned char *)s1 - *(const unsigned char *)s2;
}

#define MAX_PIPES 256
#define MAX_SPAWN_ARGV 8
#define PIPE_BUF_SIZE 4096
/* Bound on how many struct iovec entries SYS_readv/SYS_writev will copy
   into a kernel-side array in one call -- real Linux's own IOV_MAX is
   1024; 64 comfortably covers every real caller here (this OS has no
   scatter/gather-heavy workload) while keeping the on-stack copy small. */
#define IOV_MAX_LOCAL 64

typedef struct {
    uint8_t  buffer[PIPE_BUF_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    /* Phase 22d: real blocking. Zero-initialized by kmalloc+memset at
       creation, same as every other field here -- an empty WaitQueue is
       just a NULL head. */
    WaitQueue read_wq;  /* readers block here while count == 0 */
    WaitQueue write_wq; /* writers block here while count == PIPE_BUF_SIZE */
} KPipe;

static KPipe *g_pipes[MAX_PIPES];

/* Shared-memory segments -- Zerp's pixel transport. Mirrors the KPipe/
   g_pipes[] pattern exactly: a fixed global table, fd sentinel dispatch,
   index stored in VfsFile.current_cluster (same convention as pipes,
   syscall.c's pipe/pipe2 handler below). Unlike a pipe, the *same*
   physical pages get mapped into more than one process's PML4 (see the
   SYS_mmap branch below) -- true zero-copy, no kernel-side data movement
   after creation. */
#define MAX_SHM_SEGMENTS 64
typedef struct {
    uint64_t phys_addr;
    uint32_t size;
    int      refcount;
} ShmSegment;
static ShmSegment g_shm_segments[MAX_SHM_SEGMENTS];

/* Phase 22d: real blocking pipe I/O. Neither end tracks "peer closed" (see
   SYS_close's own comment on this pipe/PTY-wide accepted gap -- proper
   refcounting needs the fd-inheritance loop in process.c to know about
   pipe-specific bumping, deliberately deferred), so an unbounded block
   here could wedge a thread forever if its only peer closes without ever
   writing again. Retrying the wait in bounded ~2s slices (independent of
   the real, immediate wake pipe_write()/pipe_read() below fire on every
   actual data change) keeps this a genuine sleep -- not a spin -- while
   still bounding the worst case instead of trading one failure mode
   (busy-loop) for a strictly worse one (unkillable hang). */
#define PIPE_WAIT_RETRY_TICKS 200

static uint32_t pipe_write(KPipe *pipe, const void *buf, uint32_t size) {
    const uint8_t *src = (const uint8_t *)buf;
    while (pipe->count >= PIPE_BUF_SIZE) {
        waitqueue_wait(&pipe->write_wq, timer_get_ticks() + PIPE_WAIT_RETRY_TICKS);
    }
    uint32_t written = 0;
    while (written < size && pipe->count < PIPE_BUF_SIZE) {
        pipe->buffer[pipe->tail] = src[written];
        pipe->tail = (pipe->tail + 1) % PIPE_BUF_SIZE;
        pipe->count++;
        written++;
    }
    if (written > 0) waitqueue_wake_all(&pipe->read_wq);
    return written;
}

static uint32_t pipe_read(KPipe *pipe, void *buf, uint32_t size) {
    uint8_t *dst = (uint8_t *)buf;
    while (pipe->count == 0) {
        waitqueue_wait(&pipe->read_wq, timer_get_ticks() + PIPE_WAIT_RETRY_TICKS);
    }
    uint32_t read_bytes = 0;
    while (read_bytes < size && pipe->count > 0) {
        dst[read_bytes] = pipe->buffer[pipe->head];
        pipe->head = (pipe->head + 1) % PIPE_BUF_SIZE;
        pipe->count--;
        read_bytes++;
    }
    if (read_bytes > 0) waitqueue_wake_all(&pipe->write_wq);
    return read_bytes;
}

extern BootInfo *g_boot_info;
extern uint32_t term_bg_color;
void console_print_char(BootInfo *info, char c, uint32_t fg, uint32_t bg);

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = value & 0xFFFFFFFF;
    uint32_t high = value >> 32;
    __asm__ volatile("wrmsr" :: "a"(low), "d"(high), "c"(msr));
}

extern void comp_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
extern void comp_mark_dirty(void);
extern void *kmalloc(size_t size);
extern void kfree(void *ptr);

/* fd_table/fd_flags/fd_oflags used to be file-scope globals here, shared by
   every thread regardless of process. Now they live in Process (see
   include/wynland/process.h) -- each syscall_dispatcher() call shadows the
   names locally with the *current process's* arrays (see below), so the
   ~80 existing `fd_table[...]`-style call sites throughout this file are
   unchanged; only helpers declared outside syscall_dispatcher (like
   get_free_fd()) need the table passed in explicitly. */

static int get_free_fd(VfsFile **fd_table) {
    for (int i = 3; i < MAX_OPEN_FILES; i++) {
        if (fd_table[i] == NULL) {
            return i;
        }
    }
    return -1;
}

/* Copies up to max_entries strings out of a NULL-terminated user array of
   user string pointers at `uarray` (e.g. argv/envp) into freshly kmalloc'd
   kernel buffers (each up to MAX_PATH-1 bytes). Every level is validated:
   the array slot itself (a user pointer to a user pointer) via
   user_check_read() before it's dereferenced, and the string it points to
   via strncpy_from_user() -- neither the old execve()/SYS_spawn_argv()
   loops here validated anything, so a bad argv/envp pointer (or a bad
   pointer buried inside one) panicked the kernel outright.
   kptrs[] is left NULL-terminated at the returned count. Stops (without
   failing) at the first invalid array slot or string, an entry that's
   NULL, max_entries, or an allocation failure -- same truncate-not-fail
   style the original loops used for running out of slots. Returns -1
   only when `uarray` itself was NULL, so callers can keep their existing
   "NULL means no array / use defaults" convention. */
static int copy_argv_array_from_user(const char *const *uarray, char **kbufs, const char **kptrs, int max_entries) {
    if (!uarray) return -1;
    int count = 0;
    for (; count < max_entries; count++) {
        if (!user_check_read((uint64_t)(uintptr_t)(uarray + count), sizeof(char *))) break;
        const char *ustr = uarray[count];
        if (!ustr) break;
        char *buf = (char *)kmalloc(MAX_PATH);
        if (!buf) break;
        if (strncpy_from_user(buf, ustr, MAX_PATH) < 0) { kfree(buf); break; }
        kbufs[count] = buf;
        kptrs[count] = buf;
    }
    kptrs[count] = NULL;
    return count;
}

/* Helper: fill a linux_stat structure from a VFS file descriptor */
static void fill_stat_from_fd(struct linux_stat *st, VfsFile *file) {
    memset(st, 0, sizeof(*st));
    st->st_dev     = 1;          /* Synthetic device number */
    st->st_ino     = (uint64_t)file->node.first_cluster;
    st->st_nlink   = 1;
    st->st_uid     = 0;
    st->st_gid     = 0;
    st->st_blksize = 4096;

    if (file->node.is_dir) {
        st->st_mode = S_IFDIR | 0755;
        st->st_size = 0;
    } else {
        st->st_mode = S_IFREG | 0644;
        st->st_size = (int64_t)file->node.size;
    }
    st->st_blocks = (st->st_size + 511) / 512;

    if (file->node.first_cluster == 0xFFFFFFF0) {
        st->st_mode = S_IFCHR | 0666;
        st->st_rdev = ((uint64_t)29 << 8) | 0; /* major 29, minor 0 */
    } else if (str_compare(file->node.name, "renderd1") == 0) {
        st->st_mode = S_IFCHR | 0666;
        st->st_rdev = ((uint64_t)226 << 8) | 128;
    } else if (str_compare(file->node.name, "card0") == 0) {
        st->st_mode = S_IFCHR | 0666;
        st->st_rdev = ((uint64_t)226 << 8) | 0;
    }
}

typedef struct {
    SyscallRegs regs;
} CloneArg;

void clone_child_entry(void *arg) {
    CloneArg *ca = (CloneArg *)arg;
    extern void thread_enter_user_mode_clone(void *regs);
    thread_enter_user_mode_clone(&ca->regs);
}

#define MAX_EPOLL_INSTANCES 16
#define MAX_EPOLL_EVENTS 64

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

struct epoll_event {
    uint32_t events;
    uint64_t data;
} __attribute__((packed));

typedef struct {
    int active;
    int fd;
    struct {
        int target_fd;
        struct epoll_event ev;
    } watches[MAX_EPOLL_EVENTS];
    int num_watches;
} EpollInstance;

static EpollInstance epoll_instances[MAX_EPOLL_INSTANCES] = {0};

static EpollInstance* get_epoll_instance(int fd) {
    for (int i = 0; i < MAX_EPOLL_INSTANCES; i++) {
        if (epoll_instances[i].active && epoll_instances[i].fd == fd)
            return &epoll_instances[i];
    }
    return NULL;
}

/* Bound on how many struct wl_pollfd entries SYS_poll/SYS_ppoll will copy
   into a kernel-side array in one call. */
#define POLL_MAX_LOCAL 256

struct wl_pollfd { int fd; short events; short revents; };

/* Poll `nfds` KERNEL-owned wl_pollfd entries for readiness, genuinely
   blocking (not spinning) until at least one is ready or `timeout_ticks`
   elapses (-1 = block forever, 0 = one immediate check, >0 = real tick
   budget). Shared by SYS_poll/SYS_ppoll (case 7/271 below, which copy the
   user-supplied array in/out around this) and SYS_epoll_wait (case
   232/281, whose own pfds array is a local kernel stack buffer that was
   NEVER user memory -- it used to reach this same loop via a recursive
   syscall_dispatcher(7, (uint64_t)pfds, ...) call, which broke the moment
   case 7 started validating a1 as a genuine user pointer, since a kernel
   stack address is never PAGE_USER). Takes fd_table explicitly since it's
   a local in the calling syscall_dispatcher() invocation (per-process,
   see the top of that function), not a global this can reach on its own. */
static int do_poll(VfsFile **fd_table, struct wl_pollfd *fds, uint64_t nfds, int64_t timeout_ticks) {
    uint64_t deadline = (timeout_ticks < 0) ? SCHED_NO_DEADLINE : timer_get_ticks() + (uint64_t)timeout_ticks;

    for (;;) {
        if (fds) {
            for (uint64_t i = 0; i < nfds; i++) fds[i].revents = 0;
        }

        int ready = 0;
        WaitQueue *single_wq = NULL; /* only meaningful when nfds == 1 and that one fd isn't ready yet */

        for (uint64_t i = 0; i < nfds; i++) {
            int fd = fds[i].fd;
            if (fd < 0 || fd >= MAX_OPEN_FILES || fd_table[fd] == NULL) {
                if (fds) fds[i].revents = 0x0020; // POLLNVAL
                ready++;
                continue;
            }
            VfsFile *file = fd_table[fd];

            if (file->node.first_cluster == 0xFFFFFFFA || // Pipe Read
                file->node.first_cluster == 0xFFFFFFFC)   // Eventfd
            {
                int p_idx = file->current_cluster;
                if (p_idx >= 0 && p_idx < MAX_PIPES && g_pipes[p_idx] != NULL) {
                    KPipe *p = g_pipes[p_idx];
                    if (p->count > 0) {
                        if (fds && (fds[i].events & 0x0001)) { // POLLIN
                            fds[i].revents |= 0x0001;
                            ready++;
                        }
                    } else {
                        single_wq = &p->read_wq;
                    }
                }
            }
            else if (file->node.first_cluster == 0xFFFFFFFB) { // Pipe Write
                if (fds && (fds[i].events & 0x0004)) { // POLLOUT
                    fds[i].revents |= 0x0004;
                    ready++;
                }
            }
            else if (file->node.first_cluster == PTY_FD_MASTER) {
                Pty *pty = g_ptys[file->current_cluster];
                if (pty && pty->s2m_count > 0 && fds && (fds[i].events & 0x0001)) {
                    fds[i].revents |= 0x0001;
                    ready++;
                } else if (pty) {
                    single_wq = &pty->s2m_wq;
                }
                if (fds && (fds[i].events & 0x0004)) { fds[i].revents |= 0x0004; ready++; }
            }
            else if (file->node.first_cluster == PTY_FD_SLAVE) {
                Pty *pty = g_ptys[file->current_cluster];
                if (pty && pty->m2s_count > 0 && fds && (fds[i].events & 0x0001)) {
                    fds[i].revents |= 0x0001;
                    ready++;
                } else if (pty) {
                    single_wq = &pty->m2s_wq;
                }
                if (fds && (fds[i].events & 0x0004)) { fds[i].revents |= 0x0004; ready++; }
            }
            else {
                // Regular files/devices/sockets are always readable/writable
                // (real socket readiness lands in Phase 22d's net_poll wiring).
                if (fds) {
                    if (fds[i].events & 0x0001) fds[i].revents |= 0x0001;
                    if (fds[i].events & 0x0004) fds[i].revents |= 0x0004;
                }
                ready++;
            }
        }

        if (ready > 0) return ready;
        if (timeout_ticks == 0) return 0; // caller asked for an immediate check only

        uint64_t now = timer_get_ticks();
        if (deadline != SCHED_NO_DEADLINE && now >= deadline) return 0; // real timeout

        /* Genuinely sleep instead of spinning. Single watched fd
           with a real wait queue -> wake instantly on data via
           waitqueue_wake_all(); otherwise fall back to a bounded
           ~50ms nap so the CPU is actually free between checks
           (still not a spin -- just not per-fd event-driven yet
           for the multi-fd case, honestly short of "real epoll"
           but a genuine sleep, not the old single-yield). */
        if (single_wq && nfds == 1) {
            waitqueue_wait(single_wq, deadline);
        } else {
            uint64_t nap_deadline = now + 5;
            if (deadline != SCHED_NO_DEADLINE && nap_deadline > deadline) nap_deadline = deadline;
            sched_block(NULL, nap_deadline);
        }
    }
}

uint64_t syscall_dispatcher(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, SyscallRegs *regs) {
    /* Per-process fd namespace -- shadows the identifiers every case below
       already uses, so this is the only change needed to make fd_table/
       fd_flags/fd_oflags per-process instead of one shared global table. */
    Process *proc = sched_current()->proc;
    VfsFile  **fd_table  = proc->fd_table;
    uint32_t  *fd_flags  = proc->fd_flags;
    uint32_t  *fd_oflags = proc->fd_oflags;

    if (num != 0 && num != 1 && num != 20) { // Don't log read/write to avoid spam
        serial_write_string("Syscall ID: ");
        char buf[32];
        extern void uint_to_hex(uint64_t val, char *buf);
        uint_to_hex(num, buf);
        serial_write_string(buf);
        serial_write_string("\r\n");
    }
    switch (num) {
        case 0: // SYS_read (Linux standard)
            /* Only take the raw-keyboard/serial shortcut when fd 0 hasn't
               been redirected to something real (a pipe, a PTY slave,
               etc. via dup2()) -- otherwise a program with its stdin
               remapped (e.g. nano running inside a PTY, Phase 18) would
               have its reads silently hijacked back to the console
               instead of reaching whatever it was actually redirected
               to. */
            /* Every branch below this point writes at most a3 bytes into
               a2 (the raw keyboard/serial shortcut, the VENDOR/DEVICE
               mocks, pipe_read/tcp_recv/udp_socket_recvfrom/pty_ring_read,
               and the vfs_read() fallback) -- one guard up front covers
               all of them instead of validating only the first path taken.
               Real read(fd, NULL, n>0) is -EFAULT, not the old silent
               "return 0". */
            if (a3 != 0 && !user_prepare_write(a2, a3)) {
                return (uint64_t)-14; /* -EFAULT */
            }
            if (a1 == 0 && fd_table[0] == NULL) { // stdin
                if (a3 == 0 || !a2) return 0;
                char *cbuf = (char *)a2;
                uint64_t read_bytes = 0;
                while (read_bytes < a3) {
                    extern bool keyboard_has_scancode(void);
                    extern uint8_t keyboard_pop_scancode(void);
                    extern bool serial_received(void);
                    extern char serial_read_char(void);
                    
                    if (keyboard_has_scancode()) {
                        uint8_t sc = keyboard_pop_scancode();
                        if (!(sc & 0x80)) { // Key press
                            extern const char scancode_to_ascii_lower[59];
                            if (sc < 59) {
                                char ch = scancode_to_ascii_lower[sc];
                                if (ch != 0) {
                                    cbuf[read_bytes++] = ch;
                                    if (g_boot_info) {
                                        console_print_char(g_boot_info, ch, 0x00FFFFFF, term_bg_color);
                                    }
                                    if (ch == '\n' || ch == '\r') break;
                                }
                            }
                        }
                    } else if (serial_received()) {
                        char ch = serial_read_char();
                        if (ch == '\r') ch = '\n';
                        cbuf[read_bytes++] = ch;
                        if (g_boot_info) {
                            console_print_char(g_boot_info, ch, 0x00FFFFFF, term_bg_color);
                        }
                        if (ch == '\n') break;
                    } else {
                        sched_yield();
                    }
                }
                return read_bytes;
            }
            if (a1 >= MAX_OPEN_FILES || fd_table[a1] == NULL) {
                return (uint64_t)-1;
            }
            if (fd_table[a1]->node.first_cluster == 0xFFFFFFF4) { // VENDOR
                if (fd_table[a1]->offset > 0) return 0;
                const char *vendor_str = "0x1af4\n";
                uint32_t len = 7;
                if (a3 < len) len = a3;
                for (uint32_t i = 0; i < len; i++) ((char *)a2)[i] = vendor_str[i];
                fd_table[a1]->offset += len;
                return len;
            }
            if (fd_table[a1]->node.first_cluster == 0xFFFFFFF5) { // DEVICE
                if (fd_table[a1]->offset > 0) return 0;
                const char *device_str = "0x1050\n";
                uint32_t len = 7;
                if (a3 < len) len = a3;
                for (uint32_t i = 0; i < len; i++) ((char *)a2)[i] = device_str[i];
                fd_table[a1]->offset += len;
                return len;
            }
            if (fd_table[a1]->node.first_cluster == 0xFFFFFFFA || fd_table[a1]->node.first_cluster == 0xFFFFFFFC) {
                uint32_t pipe_idx = fd_table[a1]->current_cluster;
                if (pipe_idx < MAX_PIPES && g_pipes[pipe_idx] != NULL) {
                    return pipe_read(g_pipes[pipe_idx], (void *)a2, (uint32_t)a3);
                }
                return (uint64_t)-1;
            }
            if (fd_table[a1]->node.first_cluster == SOCK_FD_TCP) {
                if (fd_table[a1]->current_cluster == TCP_FD_NOT_CONNECTED) return (uint64_t)-107; // ENOTCONN
                TcpConnection *conn = tcp_get_connection((int)fd_table[a1]->current_cluster);
                if (!conn) return (uint64_t)-104; // ECONNRESET
                int n = tcp_recv(conn, (void *)a2, (uint32_t)a3);
                if (n < 0) return (uint64_t)-1;
                return (uint64_t)n;
            }
            if (fd_table[a1]->node.first_cluster == SOCK_FD_UDP) {
                int n = udp_socket_recvfrom((int)fd_table[a1]->current_cluster, (void *)a2, (uint32_t)a3, NULL, NULL);
                if (n < 0) return (uint64_t)-1;
                return (uint64_t)n;
            }
            if (fd_table[a1]->node.first_cluster == PTY_FD_MASTER) {
                Pty *pty = g_ptys[fd_table[a1]->current_cluster];
                if (!pty) return (uint64_t)-9; // EBADF
                while (pty->s2m_count == 0) {
                    waitqueue_wait(&pty->s2m_wq, timer_get_ticks() + PIPE_WAIT_RETRY_TICKS);
                }
                uint32_t n = pty_ring_read(pty->s2m_buf, &pty->s2m_head, &pty->s2m_tail, &pty->s2m_count, (void *)a2, (uint32_t)a3);
                /* One WaitQueue per direction serves both "empty" (reader)
                   and "full" (writer) waiters -- wake unconditionally after
                   any read/write touches the buffer, each waiter rechecks
                   its own condition on resume (see the while() loops here). */
                if (n > 0) waitqueue_wake_all(&pty->s2m_wq);
                return (uint64_t)n;
            }
            if (fd_table[a1]->node.first_cluster == PTY_FD_SLAVE) {
                Pty *pty = g_ptys[fd_table[a1]->current_cluster];
                if (!pty) return (uint64_t)-9; // EBADF
                while (pty->m2s_count == 0) {
                    waitqueue_wait(&pty->m2s_wq, timer_get_ticks() + PIPE_WAIT_RETRY_TICKS);
                }
                uint32_t n = pty_ring_read(pty->m2s_buf, &pty->m2s_head, &pty->m2s_tail, &pty->m2s_count, (void *)a2, (uint32_t)a3);
                if (n > 0) waitqueue_wake_all(&pty->m2s_wq);
                return (uint64_t)n;
            }
            return vfs_read(fd_table[a1], (void *)a2, (uint32_t)a3);
            
        case 1: // SYS_write (Linux standard)
            /* Same fix as SYS_read above: only take the raw
               console/serial shortcut when fd 1/2 hasn't been
               redirected to something real via dup2(). */
            /* Mirrors SYS_read's guard above: every branch below reads at
               most a3 bytes FROM a2 (console/serial, pipe_write/tcp_send/
               udp_socket_sendto/pty_ring_write, vfs_write() fallback). */
            if (a3 != 0 && !user_check_read(a2, a3)) {
                return (uint64_t)-14; /* -EFAULT */
            }
            if ((a1 == 1 || a1 == 2) && fd_table[a1] == NULL) { // stdout/stderr
                if (a3 == 0 || !a2) return 0;
                extern bool g_quiet_console;
                const char *cbuf = (const char *)a2;
                for (uint64_t i = 0; i < a3; i++) {
                    char ch = cbuf[i];
                    if (g_boot_info && !g_quiet_console) {
                        console_print_char(g_boot_info, ch, 0x00FFFFFF, term_bg_color);
                    }
                    char single[2] = {ch, '\0'};
                    extern void serial_write_string(const char *str);
                    serial_write_string(single);
                }
                return a3;
            }
            if (a1 >= MAX_OPEN_FILES || fd_table[a1] == NULL) {
                return (uint64_t)-1;
            }
            if (fd_table[a1]->node.first_cluster == 0xFFFFFFFB || fd_table[a1]->node.first_cluster == 0xFFFFFFFC) {
                uint32_t pipe_idx = fd_table[a1]->current_cluster;
                if (pipe_idx < MAX_PIPES && g_pipes[pipe_idx] != NULL) {
                    return pipe_write(g_pipes[pipe_idx], (const void *)a2, (uint32_t)a3);
                }
                return (uint64_t)-1;
            }
            if (fd_table[a1]->node.first_cluster == SOCK_FD_TCP) {
                if (fd_table[a1]->current_cluster == TCP_FD_NOT_CONNECTED) return (uint64_t)-107; // ENOTCONN
                TcpConnection *conn = tcp_get_connection((int)fd_table[a1]->current_cluster);
                if (!conn) return (uint64_t)-104; // ECONNRESET
                int n = tcp_send(conn, (const void *)a2, (uint32_t)a3);
                if (n < 0) return (uint64_t)-1;
                return (uint64_t)n;
            }
            if (fd_table[a1]->node.first_cluster == SOCK_FD_UDP) {
                int n = udp_socket_sendto((int)fd_table[a1]->current_cluster, (const void *)a2, (uint32_t)a3, 0, 0);
                if (n < 0) return (uint64_t)-1;
                return (uint64_t)n;
            }
            if (fd_table[a1]->node.first_cluster == PTY_FD_MASTER) {
                Pty *pty = g_ptys[fd_table[a1]->current_cluster];
                if (!pty) return (uint64_t)-9; // EBADF
                while (pty->m2s_count >= PTY_BUF_SIZE) {
                    waitqueue_wait(&pty->m2s_wq, timer_get_ticks() + PIPE_WAIT_RETRY_TICKS);
                }
                uint32_t n = pty_ring_write(pty->m2s_buf, &pty->m2s_head, &pty->m2s_tail, &pty->m2s_count, (const void *)a2, (uint32_t)a3);
                if (n > 0) waitqueue_wake_all(&pty->m2s_wq);
                return (uint64_t)n;
            }
            if (fd_table[a1]->node.first_cluster == PTY_FD_SLAVE) {
                Pty *pty = g_ptys[fd_table[a1]->current_cluster];
                if (!pty) return (uint64_t)-9; // EBADF
                while (pty->s2m_count >= PTY_BUF_SIZE) {
                    waitqueue_wait(&pty->s2m_wq, timer_get_ticks() + PIPE_WAIT_RETRY_TICKS);
                }
                uint32_t n = pty_ring_write(pty->s2m_buf, &pty->s2m_head, &pty->s2m_tail, &pty->s2m_count, (const void *)a2, (uint32_t)a3);
                if (n > 0) waitqueue_wake_all(&pty->s2m_wq);
                return (uint64_t)n;
            }
            return vfs_write(fd_table[a1], (const void *)a2, (uint32_t)a3);

        case 20: // SYS_writev (Linux standard)
            {
                struct iovec { void *iov_base; size_t iov_len; };
                int iovcnt = (int)a3;
                if (iovcnt < 0 || iovcnt > IOV_MAX_LOCAL) return (uint64_t)-22; /* -EINVAL */

                /* The iovec ARRAY itself is just as much user-controlled
                   memory as the buffers it points to -- copy it into a
                   bounded kernel buffer before trusting any entry in it. */
                struct iovec kiov[IOV_MAX_LOCAL];
                if (iovcnt > 0 && copy_from_user(kiov, (const void *)a2, (uint64_t)iovcnt * sizeof(struct iovec)) != 0) {
                    return (uint64_t)-14; /* -EFAULT */
                }
                /* Validate every buffer up front -- never validate only the
                   first one -- so a bad entry fails cleanly before an
                   earlier, valid entry has already been written out. */
                for (int i = 0; i < iovcnt; i++) {
                    if (kiov[i].iov_base && kiov[i].iov_len > 0 &&
                        !user_check_read((uint64_t)(uintptr_t)kiov[i].iov_base, kiov[i].iov_len)) {
                        return (uint64_t)-14; /* -EFAULT */
                    }
                }

                size_t total_written = 0;

                if (a1 == 1 || a1 == 2) { // stdout/stderr
                    extern bool g_quiet_console;
                    for (int i = 0; i < iovcnt; i++) {
                        if (kiov[i].iov_base && kiov[i].iov_len > 0) {
                            const char *cbuf = (const char *)kiov[i].iov_base;
                            for (size_t j = 0; j < kiov[i].iov_len; j++) {
                                char ch = cbuf[j];
                                if (g_boot_info && !g_quiet_console) {
                                    console_print_char(g_boot_info, ch, 0x00FFFFFF, term_bg_color);
                                }
                                char single[2] = {ch, '\0'};
                                extern void serial_write_string(const char *str);
                                serial_write_string(single);
                            }
                            total_written += kiov[i].iov_len;
                        }
                    }
                    return total_written;
                }

                if (a1 >= MAX_OPEN_FILES || fd_table[a1] == NULL) {
                    return (uint64_t)-1;
                }
                for (int i = 0; i < iovcnt; i++) {
                    if (kiov[i].iov_base && kiov[i].iov_len > 0) {
                        int written = vfs_write(fd_table[a1], kiov[i].iov_base, kiov[i].iov_len);
                        if (written < 0) return (uint64_t)-1;
                        total_written += written;
                    }
                }
                return total_written;
            }

        case 2: // SYS_open (Linux standard)
            if (!a1) return (uint64_t)-2;
            {
                char path_kbuf[MAX_PATH];
                if (strncpy_from_user(path_kbuf, (const void *)a1, sizeof(path_kbuf)) < 0) {
                    return (uint64_t)-14; /* -EFAULT (also covers a too-long path) */
                }

                serial_write_string("Syscall: open path: ");
                serial_write_string(path_kbuf);
                serial_write_string("\r\n");

                int linux_flags = (int)a2;
                uint32_t vfs_flags = 0;
                if ((linux_flags & 3) == 0) vfs_flags |= 0x01; // VFS_O_READ
                if ((linux_flags & 3) == 1) vfs_flags |= 0x02; // VFS_O_WRITE
                if ((linux_flags & 3) == 2) vfs_flags |= (0x01 | 0x02);
                if (linux_flags & 0100) vfs_flags |= 0x04; // VFS_O_CREATE
                if (linux_flags & 01000) vfs_flags |= 0x10; // VFS_O_TRUNC
                if (linux_flags & 02000) vfs_flags |= 0x08; // VFS_O_APPEND

                int fd = get_free_fd(fd_table);
                if (fd == -1) return (uint64_t)-2;
                VfsFile *file = vfs_open_flags(path_kbuf, vfs_flags);
                if (!file) return (uint64_t)-2;

                /* Real permission enforcement backed by the ext2 inode's
                   own owner + mode bits (replaces the old FAT32
                   readonly-attribute hack). Owner-vs-other model: the
                   Process struct has no gid, so there is honestly no
                   group dimension to check against -- documented as
                   such rather than overclaimed. Root bypasses. */
                if ((vfs_flags & (0x01 /* VFS_O_READ */ |
                                  0x02 /* VFS_O_WRITE */ |
                                  0x10 /* VFS_O_TRUNC */ |
                                  0x08 /* VFS_O_APPEND */))) {
                    uint32_t puid = sched_current()->proc->uid;
                    if (puid != 0) {
                        uint32_t want = 0;
                        if (vfs_flags & 0x01) want |= 04;
                        if (vfs_flags & (0x02 | 0x10 | 0x08)) want |= 02;
                        uint32_t have = (puid == file->node.uid)
                            ? ((file->node.mode >> 6) & 7)
                            : (file->node.mode & 7);
                        if ((have & want) != want) {
                            kfree(file);
                            return (uint64_t)-13; /* -EACCES */
                        }
                    }
                }

                fd_table[fd] = file;
                return fd;
            }

        case 3: // SYS_close (Linux standard)
            if (a1 >= MAX_OPEN_FILES || fd_table[a1] == NULL) {
                return (uint64_t)-1;
            }
            if (fd_table[a1]->node.first_cluster == 0xFFFFFFFA ||
                fd_table[a1]->node.first_cluster == 0xFFFFFFFB ||
                fd_table[a1]->node.first_cluster == 0xFFFFFFFC) {
                kfree(fd_table[a1]);
                fd_table[a1] = NULL;
                return 0;
            }
            if (fd_table[a1]->node.first_cluster == SOCK_FD_TCP) {
                if (fd_table[a1]->current_cluster != TCP_FD_NOT_CONNECTED) {
                    TcpConnection *conn = tcp_get_connection((int)fd_table[a1]->current_cluster);
                    if (conn) tcp_close(conn);
                }
                kfree(fd_table[a1]);
                fd_table[a1] = NULL;
                return 0;
            }
            if (fd_table[a1]->node.first_cluster == SOCK_FD_UDP) {
                udp_socket_close((int)fd_table[a1]->current_cluster);
                kfree(fd_table[a1]);
                fd_table[a1] = NULL;
                return 0;
            }
            if (fd_table[a1]->node.first_cluster == PTY_FD_MASTER ||
                fd_table[a1]->node.first_cluster == PTY_FD_SLAVE) {
                /* Matches this codebase's existing pipe/eventfd convention
                   exactly (see the 0xFFFFFFFA/FB/FC case above): only the
                   small per-fd VfsFile wrapper is freed here, never the
                   underlying Pty object itself. A real refcount would
                   need the generic fd-inheritance deep-copy loop in
                   process.c (used by both process_spawn() and fork()) to
                   know about PTY-specific bumping, which it deliberately
                   doesn't (it's sentinel-agnostic by design). Leaking the
                   Pty itself is the same accepted tradeoff already made
                   for KPipe -- fine at this OS's current scale (one PTY
                   per interactive nano session, not a long-running
                   server opening thousands of them). */
                kfree(fd_table[a1]);
                fd_table[a1] = NULL;
                return 0;
            }
            vfs_close(fd_table[a1]);
            fd_table[a1] = NULL;
            return 0;
            
        case 32: // SYS_dup(oldfd)
            {
                int oldfd = (int)a1;
                if (oldfd < 0 || oldfd >= MAX_OPEN_FILES || fd_table[oldfd] == NULL) return (uint64_t)-9; // EBADF
                int newfd = get_free_fd(fd_table);
                if (newfd < 0) return (uint64_t)-24; // EMFILE
                VfsFile *copy = (VfsFile *)kmalloc(sizeof(VfsFile));
                *copy = *fd_table[oldfd]; // deep copy of the wrapper -- same convention as
                                          // process_spawn()'s fd-inheritance loop, never share the pointer
                fd_table[newfd] = copy;
                fd_flags[newfd] = 0; // dup'd fds are never CLOEXEC by default (POSIX)
                fd_oflags[newfd] = fd_oflags[oldfd];
                return newfd;
            }

        case 33: // SYS_dup2(oldfd, newfd) -- Phase 18. Standard idiom used
                  // by zerp_term.c's fork()+dup2()+execve() PTY spawn: the
                  // child remaps the PTY slave fd onto 0/1/2 before
                  // exec'ing nano.
            {
                int oldfd = (int)a1;
                int newfd = (int)a2;
                if (oldfd < 0 || oldfd >= MAX_OPEN_FILES || fd_table[oldfd] == NULL) return (uint64_t)-9; // EBADF
                if (newfd < 0 || newfd >= MAX_OPEN_FILES) return (uint64_t)-9; // EBADF
                if (oldfd == newfd) return newfd;
                if (fd_table[newfd] != NULL) {
                    /* Reuse SYS_close's own per-sentinel cleanup (pipe/
                       socket/PTY-specific) instead of duplicating it --
                       same self-recursion pattern already used by
                       SYS_sendto/SYS_recvfrom's non-socket fallback. */
                    syscall_dispatcher(3, (uint64_t)newfd, 0, 0, 0, 0, regs);
                }
                VfsFile *copy = (VfsFile *)kmalloc(sizeof(VfsFile));
                *copy = *fd_table[oldfd];
                fd_table[newfd] = copy;
                fd_flags[newfd] = 0;
                fd_oflags[newfd] = fd_oflags[oldfd];
                return newfd;
            }

        case 8: // SYS_lseek (Linux standard)
            if (a1 >= MAX_OPEN_FILES || fd_table[a1] == NULL) {
                return (uint64_t)-1;
            }
            return vfs_seek(fd_table[a1], (int32_t)a2, (int)a3);
            
        case 9: // SYS_mmap (Linux standard)
            if (a2 == 0) return 0;
            {
                uint64_t addr = a1;
                uint64_t len = a2;
                uint64_t prot = a3;
                uint64_t flags = a4;
                int fd = (int)a5;
                uint64_t offset = regs->r9;

                uint64_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
                uint64_t size_aligned = pages * PAGE_SIZE;

                // Check if mapping the framebuffer character device /dev/fb0
                if (fd >= 0 && fd < MAX_OPEN_FILES && fd_table[fd] != NULL && fd_table[fd]->node.first_cluster == 0xFFFFFFF0) {
                    if (!g_boot_info) return 0;
                    extern uint64_t fb_active_phys_addr(void);
                    uint64_t fb_phys = fb_active_phys_addr();

                    uint64_t virt_addr = addr;
                    if (!(flags & 0x10) || virt_addr == 0) { // MAP_FIXED is 0x10
                        static uint64_t mmap_fb_ptr = 0x610000000000;
                        virt_addr = mmap_fb_ptr;
                        mmap_fb_ptr += size_aligned;
                    }
                    PageTable *pml4 = vmm_get_current_pml4();
                    for (uint64_t i = 0; i < pages; i++) {
                        /* Map framebuffer with Write-Combining (PAT4=WC via PAT MSR set in vmm_init)
                         * PAGE_PAT | PAGE_WRITE_THROUGH selects PAT4 entry = Write-Combining.
                         * This ensures pixel writes reach the display immediately without CPU cache delay. */
                        vmm_map_page(pml4, virt_addr + i * PAGE_SIZE, fb_phys + i * PAGE_SIZE,
                                     PAGE_WRITE | PAGE_USER | PAGE_PAT | PAGE_WRITE_THROUGH);
                    }

                    return virt_addr;
                }

                // Check if mapping a SHM segment (SYS_shm_create, case 407)
                if (fd >= 0 && fd < MAX_OPEN_FILES && fd_table[fd] != NULL && fd_table[fd]->node.first_cluster == 0xFFFFFFFD) {
                    int seg_idx = (int)fd_table[fd]->current_cluster;
                    if (seg_idx < 0 || seg_idx >= MAX_SHM_SEGMENTS || g_shm_segments[seg_idx].phys_addr == 0) {
                        return (uint64_t)-1;
                    }
                    ShmSegment *seg = &g_shm_segments[seg_idx];
                    uint32_t shm_pages = seg->size / PAGE_SIZE;

                    uint64_t virt_addr = addr;
                    if (!(flags & 0x10) || virt_addr == 0) { // MAP_FIXED is 0x10
                        static uint64_t mmap_shm_ptr = 0x620000000000; // dedicated slot, disjoint from the stack (0x600...) and /dev/fb0 (0x610...) ranges
                        virt_addr = mmap_shm_ptr;
                        mmap_shm_ptr += (uint64_t)shm_pages * PAGE_SIZE;
                    }
                    PageTable *pml4 = vmm_get_current_pml4();
                    for (uint32_t i = 0; i < shm_pages; i++) {
                        vmm_map_page(pml4, virt_addr + (uint64_t)i * PAGE_SIZE, seg->phys_addr + (uint64_t)i * PAGE_SIZE,
                                     PAGE_WRITE | PAGE_USER);
                    }
                    seg->refcount++;

                    return virt_addr;
                }

                // Determine virtual address
                uint64_t virt_addr = addr;
                if (!(flags & 0x10) || virt_addr == 0) { // MAP_FIXED is 0x10
                    /* Must be disjoint from the process's own stack region
                       (elf.c: stack_base = 0x600000000000, 64KB) -- this
                       bump allocator used to start at that exact same
                       address, so every non-MAP_FIXED mmap() (every shared
                       library a dynamic-linked process dlopen()s) grew
                       right on top of the live stack, silently corrupting
                       it. Matches the project's per-purpose "0x6X0..."
                       slot convention (stack=0x60.., /dev/fb0=0x61..,
                       SHM=0x62..). */
                    static uint64_t mmap_alloc_ptr = 0x630000000000;
                    virt_addr = mmap_alloc_ptr;
                    mmap_alloc_ptr += size_aligned;
                } else {
                    virt_addr &= ~(PAGE_SIZE - 1); // align down to page boundary
                    /* MAP_FIXED: real semantics replace whatever was mapped
                       in this range before -- drop the old VMA coverage and
                       actually unmap+free any pages currently backing it,
                       instead of silently leaking the old physical frames
                       (previously vmm_map_page() below would have just
                       overwritten the PTEs, orphaning whatever pmm_alloc'd
                       frame they used to point at). */
                    Process *mmap_proc = sched_current()->proc;
                    PageTable *cur_pml4 = vmm_get_current_pml4();
                    for (uint64_t off = 0; off < size_aligned; off += PAGE_SIZE) {
                        uint64_t old_phys = vmm_get_phys(cur_pml4, virt_addr + off);
                        if (old_phys) {
                            vmm_unmap_page(cur_pml4, virt_addr + off);
                            pmm_free_page((void *)(uintptr_t)old_phys);
                        }
                    }
                    vma_unmap_range(mmap_proc, virt_addr, virt_addr + size_aligned);
                }

                PageTable *pml4 = vmm_get_current_pml4();

                // Allocate physical pages
                extern void *pmm_alloc_contiguous(uint32_t count);
                void *phys_ptr = pmm_alloc_contiguous(pages);
                if (!phys_ptr) {
                    serial_write_string("SYS_mmap: pmm_alloc_contiguous failed\r\n");
                    return (uint64_t)-1; // MAP_FAILED
                }
                uint64_t phys_start = (uint64_t)(uintptr_t)phys_ptr;

                /* Real prot: map writable now regardless of the requested
                   PROT_WRITE (file content/zeroing below needs to write
                   through this same mapping), then downgrade to the real
                   requested permissions once populated -- same pattern as
                   the ELF loader's segment loading (kernel/elf.c). */
                for (uint64_t i = 0; i < pages; i++) {
                    vmm_map_page(pml4, virt_addr + i * PAGE_SIZE, phys_start + i * PAGE_SIZE, PAGE_WRITE | PAGE_USER);
                    // Clear the page
                    memset((void *)(uintptr_t)(virt_addr + i * PAGE_SIZE), 0, PAGE_SIZE);
                }

                // If not MAP_ANONYMOUS (0x20), load file contents
                if (!(flags & 0x20)) {
                    if (fd >= 0 && fd < MAX_OPEN_FILES && fd_table[fd] != NULL) {
                        VfsFile *file = fd_table[fd];
                        uint32_t prev_pos = file->offset;
                        vfs_seek(file, (int32_t)offset, 0); // SEEK_SET is 0
                        uint32_t read_bytes = vfs_read(file, (void *)(uintptr_t)virt_addr, (uint32_t)len);
                        vfs_seek(file, (int32_t)prev_pos, 0); // restore file position
                        (void)read_bytes;
                    }
                }

                /* Real VMA bookkeeping + real prot: register the range so
                   mprotect()/munmap()/the page fault handler's COW path
                   have something to look up, then drop PAGE_WRITE if the
                   caller didn't actually ask for it. */
                Process *mmap_proc2 = sched_current()->proc;
                uint32_t vma_prot = (uint32_t)prot & (VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC);
                vma_insert(mmap_proc2, virt_addr, virt_addr + size_aligned, vma_prot,
                           (flags & 0x20) ? VMA_ANON : 0);

                if (!(prot & 0x2)) { // !PROT_WRITE
                    for (uint64_t i = 0; i < pages; i++) {
                        vmm_protect_page(pml4, virt_addr + i * PAGE_SIZE, PAGE_USER);
                    }
                }

                return virt_addr;
            }

        case 22: // SYS_pipe (Linux standard)
        case 293: // SYS_pipe2 (Linux standard)
            {
                serial_write_string("Syscall: pipe/pipe2 called\r\n");
                int *pipefd = (int *)a1;
                if (!pipefd || !user_prepare_write(a1, 2 * sizeof(int))) {
                    serial_write_string("  Error: pipefd is NULL or invalid\r\n");
                    return (uint64_t)-14; /* -EFAULT */
                }

                // Allocate a pipe structure
                KPipe *p = (KPipe *)kmalloc(sizeof(KPipe));
                if (!p) return (uint64_t)-12; /* -ENOMEM */
                memset(p, 0, sizeof(KPipe));

                // Register pipe
                int p_idx = -1;
                for (int i = 0; i < MAX_PIPES; i++) {
                    if (g_pipes[i] == NULL) {
                        g_pipes[i] = p;
                        p_idx = i;
                        break;
                    }
                }
                if (p_idx == -1) {
                    kfree(p);
                    return (uint64_t)-23; /* -ENFILE */
                }

                // Get two free file descriptors
                int fd_read = get_free_fd(fd_table);
                if (fd_read == -1) {
                    g_pipes[p_idx] = NULL;
                    kfree(p);
                    return (uint64_t)-24; /* -EMFILE */
                }
                // Allocate read file structure
                VfsFile *f_read = (VfsFile *)kmalloc(sizeof(VfsFile));
                memset(f_read, 0, sizeof(VfsFile));
                str_copy(f_read->node.name, "pipe_read");
                f_read->node.is_dir = false;
                f_read->node.first_cluster = 0xFFFFFFFA; // Pipe Read signature
                f_read->current_cluster = p_idx; // Store pipe index
                fd_table[fd_read] = f_read;

                int fd_write = get_free_fd(fd_table);
                if (fd_write == -1) {
                    fd_table[fd_read] = NULL;
                    kfree(f_read);
                    g_pipes[p_idx] = NULL;
                    kfree(p);
                    return (uint64_t)-24; /* -EMFILE */
                }
                // Allocate write file structure
                VfsFile *f_write = (VfsFile *)kmalloc(sizeof(VfsFile));
                memset(f_write, 0, sizeof(VfsFile));
                str_copy(f_write->node.name, "pipe_write");
                f_write->node.is_dir = false;
                f_write->node.first_cluster = 0xFFFFFFFB; // Pipe Write signature
                f_write->current_cluster = p_idx; // Store pipe index
                fd_table[fd_write] = f_write;

                pipefd[0] = fd_read;
                pipefd[1] = fd_write;

                serial_write_string("  Success: created read_fd=");
                char fdbuf[16];
                uint_to_str(fd_read, fdbuf); serial_write_string(fdbuf);
                serial_write_string(", write_fd=");
                uint_to_str(fd_write, fdbuf); serial_write_string(fdbuf);
                serial_write_string("\r\n");

                return 0; // Success
            }

        case 83: // SYS_mkdir
            {
                char path[MAX_PATH];
                if (strncpy_from_user(path, (const void *)a1, sizeof(path)) < 0) {
                    return (uint64_t)-14; /* -EFAULT */
                }
                serial_write_string("Syscall: mkdir path: ");
                serial_write_string(path);
                serial_write_string("\r\n");

                extern bool vfs_mkdir(const char *path);
                if (vfs_mkdir(path)) {
                    return 0;
                }
                return (uint64_t)-17; /* -EEXIST or other error */
            }

        case 290: // SYS_eventfd2 (Linux standard)
            {
                serial_write_string("Syscall: eventfd2 called\r\n");
                int initval = (int)a1;
                // Allocate pipe for eventfd
                KPipe *p = (KPipe *)kmalloc(sizeof(KPipe));
                if (!p) return (uint64_t)-12; /* -ENOMEM */
                memset(p, 0, sizeof(KPipe));
                if (initval > 0) {
                    uint64_t val = initval;
                    pipe_write(p, &val, 8);
                }

                int p_idx = -1;
                for (int i = 0; i < MAX_PIPES; i++) {
                    if (g_pipes[i] == NULL) {
                        g_pipes[i] = p;
                        p_idx = i;
                        break;
                    }
                }
                if (p_idx == -1) {
                    kfree(p);
                    return (uint64_t)-23; /* -ENFILE */
                }

                int fd = get_free_fd(fd_table);
                if (fd == -1) {
                    g_pipes[p_idx] = NULL;
                    kfree(p);
                    return (uint64_t)-24; /* -EMFILE */
                }
                // Allocate file descriptor supporting both read and write
                VfsFile *file = (VfsFile *)kmalloc(sizeof(VfsFile));
                memset(file, 0, sizeof(VfsFile));
                str_copy(file->node.name, "eventfd");
                file->node.is_dir = false;
                file->node.first_cluster = 0xFFFFFFFC; // Eventfd signature
                file->current_cluster = p_idx;
                fd_table[fd] = file;

                serial_write_string("  Success: created eventfd fd=");
                char fdbuf[16];
                uint_to_str(fd, fdbuf); serial_write_string(fdbuf);
                serial_write_string("\r\n");
                return fd;
            }

        case 4: // SYS_stat (Linux standard)
            {
                char path[MAX_PATH];
                struct linux_stat *user_stat = (struct linux_stat *)a2;
                if (!a1 || !user_stat) return (uint64_t)-14; /* -EFAULT */
                if (strncpy_from_user(path, (const void *)a1, sizeof(path)) < 0 ||
                    !user_prepare_write(a2, sizeof(*user_stat))) {
                    return (uint64_t)-14; /* -EFAULT */
                }

                VfsStat vst;
                if (!vfs_stat(path, &vst)) {
                    return (uint64_t)-2; /* -ENOENT */
                }
                memset(user_stat, 0, sizeof(*user_stat));
                user_stat->st_dev     = 1;
                user_stat->st_ino     = (uint64_t)vst.first_cluster;
                user_stat->st_nlink   = 1;
                user_stat->st_blksize = 4096;
                if (vst.is_dir) {
                    user_stat->st_mode = S_IFDIR | 0755;
                } else {
                    user_stat->st_mode = S_IFREG | 0644;
                    user_stat->st_size = (int64_t)vst.size;
                }
                user_stat->st_blocks = (user_stat->st_size + 511) / 512;
                return 0;
            }

        case 10: // SYS_mprotect (Linux standard)
            {
                uint64_t addr = a1 & ~(PAGE_SIZE - 1);
                uint64_t end = addr + ((a2 + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;
                uint64_t prot = a3;

                Process *proc = sched_current()->proc;
                uint32_t new_prot = (uint32_t)prot & (VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC);
                if (!vma_protect_range(proc, addr, end, new_prot)) {
                    return (uint64_t)-12; /* -ENOMEM: part of the range isn't mapped */
                }

                PageTable *pml4 = vmm_get_current_pml4();
                for (uint64_t a = addr; a < end; a += PAGE_SIZE) {
                    uint64_t old_flags = vmm_get_page_flags(pml4, a);
                    if (!(old_flags & PAGE_PRESENT)) continue; /* shouldn't happen under the eager-allocation model, but don't fault the kernel over it */

                    uint64_t pte_flags = PAGE_USER;
                    if (prot & 0x2) { // requested PROT_WRITE
                        /* If this leaf is still COW-shared (fork()'d, never
                           written since), stay read-only + PAGE_COW rather
                           than granting real PAGE_WRITE directly -- the
                           frame is still shared physically, so the actual
                           grant has to go through the fault handler's COW
                           resolution on the next write, same as it would
                           without this mprotect() call. */
                        pte_flags |= (old_flags & PAGE_COW) ? PAGE_COW : PAGE_WRITE;
                    }
                    vmm_protect_page(pml4, a, pte_flags);
                }
                return 0;
            }

        case 11: // SYS_munmap (Linux standard)
            {
                uint64_t addr = a1 & ~(PAGE_SIZE - 1);
                uint64_t end = addr + ((a2 + PAGE_SIZE - 1) / PAGE_SIZE) * PAGE_SIZE;

                Process *proc = sched_current()->proc;
                PageTable *pml4 = vmm_get_current_pml4();
                for (uint64_t a = addr; a < end; a += PAGE_SIZE) {
                    uint64_t phys = vmm_get_phys(pml4, a);
                    if (phys) {
                        vmm_unmap_page(pml4, a);
                        pmm_free_page((void *)(uintptr_t)phys);
                    }
                }
                vma_unmap_range(proc, addr, end);
                return 0;
            }

        case 13: // SYS_rt_sigaction (Linux standard) -- Phase 22b: real
                   // handler registration (per-process dispositions).
            return signal_do_sigaction((int)a1, a2, a3);

        case 14: // SYS_rt_sigprocmask (Linux standard) -- Phase 22b: real
                   // blocked-set management (delivery happens at the
                   // syscall-return tail; masked signals stay pending).
            return signal_do_procmask((int)a1, a2, a3);

        case 15: // SYS_rt_sigreturn (Linux standard) -- Phase 22b: restore
                   // the interrupted context from the kernel-side frame.
            return signal_rt_return(regs);

        case 17: // SYS_pread64 (Linux standard)
            if (a1 >= MAX_OPEN_FILES || fd_table[a1] == NULL) {
                return (uint64_t)-9; /* -EBADF */
            }
            if (a3 != 0 && !user_prepare_write(a2, a3)) {
                return (uint64_t)-14; /* -EFAULT */
            }
            {
                VfsFile *file = fd_table[a1];
                uint32_t prev_pos = file->offset;
                vfs_seek(file, (int32_t)a4, 0); // SEEK_SET
                uint32_t read_bytes = vfs_read(file, (void *)a2, (uint32_t)a3);
                vfs_seek(file, (int32_t)prev_pos, 0); // restore pos
                return read_bytes;
            }

        case 12: // SYS_brk (Linux standard)
            {
                static uint64_t current_brk = 0x60000000;
                if (a1 == 0) {
                    return current_brk;
                }
                if (a1 > current_brk) {
                    uint64_t start = (current_brk + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                    uint64_t end = (a1 + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                    PageTable *pml4 = vmm_get_current_pml4();
                    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
                        void *phys = pmm_alloc_page();
                        if (phys) {
                            vmm_map_page(pml4, addr, (uint64_t)(uintptr_t)phys, PAGE_WRITE | PAGE_USER);
                            memset(phys, 0, PAGE_SIZE);
                        }
                    }
                    current_brk = a1;
                }
                return current_brk;
            }

        case 24: // SYS_sched_yield (Linux standard)
            sched_yield();
            return 0;

        case 39: // SYS_getpid (Linux standard)
            return 1;

        case 56: // SYS_clone (Linux standard)
            {
                bool want_parent_tid = (a1 & 0x00000100) != 0; // CLONE_PARENT_SETTID
                bool want_child_tid  = (a1 & 0x01000000) != 0; // CLONE_CHILD_SETTID
                /* Validate BEFORE creating the thread -- a bad ptid/ctid
                   pointer should fail the whole clone(), not leave an
                   orphaned thread behind because the write after the fact
                   discovered the pointer was bad. */
                if (want_parent_tid && a3 && !user_prepare_write(a3, sizeof(int))) {
                    return (uint64_t)-14; /* -EFAULT */
                }
                if (want_child_tid && a4 && !user_prepare_write(a4, sizeof(int))) {
                    return (uint64_t)-14; /* -EFAULT */
                }

                CloneArg *ca = (CloneArg *)kmalloc(sizeof(CloneArg));
                ca->regs = *regs;
                ca->regs.rsp = a2; // Child stack pointer

                extern Thread *thread_create(void (*entry)(void*), void *arg);
                Thread *t = thread_create(clone_child_entry, ca);
                t->tls_base = a5;

                if (want_parent_tid && a3) {
                    *(int *)(uintptr_t)a3 = (int)t->id;
                }
                if (want_child_tid && a4) {
                    *(int *)(uintptr_t)a4 = (int)t->id;
                }
                if (a1 & 0x00080000) { // CLONE_CHILD_CLEARTID
                    /* Phase 22a: on this thread's death the kernel must zero
                       this word and futex-wake it. musl's thread-list lock is
                       cloned with ctid = &__thread_list_lock precisely so a
                       dying holder gets released by the kernel. Deliberately
                       not validated here (unlike ptid/ctid above): it's only
                       ever read back later, at this same thread's own exit,
                       from its own address space (kernel/sched.c) -- not an
                       immediate cross-process disclosure/corruption risk the
                       way writing through a bad pointer right now would be. */
                    t->clear_tid = (uint32_t *)a4;
                }

                return t->id;
            }

        case 57: // SYS_fork (Linux standard) -- real process duplication
            {
                /* musl's fork() issues this syscall number directly (confirmed
                   via objdump on the vendored libc.so), separate from SYS_clone
                   above -- needed by any POSIX build tool (make, ninja, cmake,
                   sh) that spawns subprocesses via fork()+exec() rather than
                   posix_spawn(). Phase 22c: real copy-on-write via
                   vmm_cow_clone_user_pages() -- both address spaces share
                   physical frames read-only until either side writes,
                   resolved lazily in the page fault handler (kernel/idt.c)
                   instead of paying a full eager memcpy of the parent's
                   entire address space at every fork() call. */
                Process *parent = sched_current()->proc;

                PageTable *child_pml4 = vmm_new_process_pml4();
                if (!child_pml4) return (uint64_t)-12; /* -ENOMEM */

                if (!vmm_cow_clone_user_pages(parent->pml4, child_pml4)) {
                    return (uint64_t)-12; /* -ENOMEM */
                }

                Process *child = process_create(child_pml4);
                if (!child) return (uint64_t)-12; /* -ENOMEM */
                child->uid = parent->uid;

                /* The VMA list is process-local bookkeeping, not part of the
                   page tables vmm_cow_clone_user_pages() just shared -- copy
                   every parent VMA into the child's own list so mprotect/
                   munmap/the page fault handler's vma_find() lookups work
                   identically in both processes after the fork. */
                for (VMA *pv = parent->vma_list; pv; pv = pv->next) {
                    vma_insert(child, pv->start, pv->end, pv->prot, pv->flags);
                }

                /* fork() inherits every open fd as-is -- FD_CLOEXEC only
                   matters across an exec(), which fork() alone doesn't do.
                   Same deep-copy-the-VfsFile-wrapper pattern as
                   process_spawn()'s fd inheritance (see its own comment for
                   the use-after-free this avoids: the small per-fd wrapper
                   needs its own copy per process; the underlying
                   KPipe/ShmSegment/file position it indexes into does not). */
                for (int i = 0; i < MAX_OPEN_FILES; i++) {
                    if (parent->fd_table[i]) {
                        VfsFile *copy = (VfsFile *)kmalloc(sizeof(VfsFile));
                        *copy = *parent->fd_table[i];
                        child->fd_table[i]  = copy;
                        child->fd_flags[i]  = parent->fd_flags[i];
                        child->fd_oflags[i] = parent->fd_oflags[i];
                    }
                }

                /* Heap copy of the parent's exact syscall-entry register
                   state -- the child resumes execution from HERE (same RIP,
                   same RSP, same every register except RAX), not from a
                   fresh entry point. See fork_child_entry in
                   kernel/sched_switch.asm for the other half of this. */
                SyscallRegs *regs_copy = (SyscallRegs *)kmalloc(sizeof(SyscallRegs));
                *regs_copy = *regs;

                extern void fork_child_entry(void *arg);
                Thread *child_thread = thread_create_ex_tls(fork_child_entry, regs_copy, child, sched_current()->tls_base);
                child->main_thread = child_thread;
                child->thread_count = 1;

                return child->pid; /* parent's own return value: the child's real pid */
            }

        case 59: // SYS_execve (Linux standard)
            {
                if (!a1) return (uint64_t)-14; /* -EFAULT */

                char kernel_path[MAX_PATH];
                if (strncpy_from_user(kernel_path, (const void *)a1, sizeof(kernel_path)) < 0) {
                    return (uint64_t)-14; /* -EFAULT */
                }

                serial_write_string("SYS_execve: Loading target: ");
                serial_write_string(kernel_path);
                serial_write_string("\r\n");

                uint64_t entry_point = 0;
                uint64_t stack_top = 0;
                PageTable *pml4 = vmm_get_current_pml4();

                /* Phase 18: real argv/envp support. a2=argv, a3=envp, both
                   optional (NULL preserves the old hardcoded defaults,
                   matching elf_load()'s own NULL-means-default convention).
                   copy_argv_array_from_user() validates every level -- the
                   array of user pointers, and each string it points to --
                   before elf_load() touches any of it. */
                #define MAX_SPAWN_ENVP 16
                char *argv_bufs_e[MAX_SPAWN_ARGV] = {0};
                const char *kargv_e[MAX_SPAWN_ARGV + 1];
                int argc_e = copy_argv_array_from_user((const char *const *)a2, argv_bufs_e, kargv_e, MAX_SPAWN_ARGV);
                bool have_argv_e = (argc_e >= 0);
                if (!have_argv_e) { argc_e = 0; kargv_e[0] = NULL; }

                char *envp_bufs_e[MAX_SPAWN_ENVP] = {0};
                const char *kenvp_e[MAX_SPAWN_ENVP + 1];
                int envc_e = copy_argv_array_from_user((const char *const *)a3, envp_bufs_e, kenvp_e, MAX_SPAWN_ENVP);
                bool have_envp_e = (envc_e >= 0);
                if (!have_envp_e) { envc_e = 0; kenvp_e[0] = NULL; }

                LoadedPages *lp = kmalloc(sizeof(LoadedPages));
                if (!lp) {
                    serial_write_string("SYS_execve: out of memory for LoadedPages allocation!\r\n");
                    for (int k = 0; k < argc_e; k++) kfree(argv_bufs_e[k]);
                    for (int k = 0; k < envc_e; k++) kfree(envp_bufs_e[k]);
                    return (uint64_t)-12; /* -ENOMEM */
                }

                /* execve() replaces the address space in place -- the old
                   image's VMAs (segments, stack, any mmap'd regions) no
                   longer describe anything real once elf_load() below
                   overwrites their virtual addresses with the new binary's
                   content, so drop them before loading rather than leaving
                   stale/overlapping records for mprotect()/munmap() to trip
                   over later. */
                Process *exec_proc = sched_current()->proc;
                for (VMA *v = exec_proc->vma_list; v; ) {
                    VMA *next_v = v->next;
                    kfree(v);
                    v = next_v;
                }
                exec_proc->vma_list = NULL;

                bool execve_ok = elf_load(kernel_path, &entry_point, &stack_top, pml4, lp,
                                          have_argv_e ? kargv_e : NULL, have_envp_e ? kenvp_e : NULL, exec_proc);
                for (int k = 0; k < argc_e; k++) kfree(argv_bufs_e[k]);
                for (int k = 0; k < envc_e; k++) kfree(envp_bufs_e[k]);
                if (!execve_ok) {
                    serial_write_string("SYS_execve: elf_load failed!\r\n");
                    kfree(lp);
                    return (uint64_t)-2; /* -ENOENT */
                }
                kfree(lp);

                // Update syscall regs to jump to the new entry point on return
                regs->rip = entry_point;
                regs->rsp = stack_top;
                
                serial_write_string("SYS_execve: Successfully loaded ELF. Entry = ");
                char buf[32];
                uint_to_hex(entry_point, buf);
                serial_write_string(buf);
                serial_write_string(", Stack = ");
                uint_to_hex(stack_top, buf);
                serial_write_string(buf);
                serial_write_string("\r\n");

                return 0; // Will transition to Ring 3 entry point on syscall return
            }

        case 60: // SYS_exit (Linux standard)
            thread_exit();
            return 0;

        case 158: // SYS_arch_prctl (Linux standard)
            {
                if (a1 == 0x1002) { // ARCH_SET_FS
                    uint32_t msr = 0xC0000100; // IA32_FS_BASE
                    uint32_t low = a2 & 0xFFFFFFFF;
                    uint32_t high = a2 >> 32;
                    __asm__ volatile("wrmsr" :: "c"(msr), "a"(low), "d"(high));
                    
                    Thread *curr = sched_current();
                    if (curr) {
                        curr->tls_base = a2;
                    }
                    return 0; // Success
                }
                return (uint64_t)-1;
            }

        case 231: // SYS_exit_group (Linux standard)
            thread_exit();
            return 0;

        case 202: // SYS_futex -- Phase 22a: REAL sleep/wake queues. The old
                   // EAGAIN+yield approximation is gone: FUTEX_WAIT now
                   // genuinely parks the thread on a queue keyed by
                   // (address space, uaddr) until a matching FUTEX_WAKE (or
                   // CMP_REQUEUE handoff) unblocks it, or its timeout
                   // expires with -ETIMEDOUT. This is what pthread_join,
                   // contended mutexes/condvars and epoll's future sleepers
                   // reduce to; curl's threaded resolver runs again without
                   // the Phase 17 workaround build flag (verified live:
                   // TLS 1.3 + HTTP/1.1 200 OK from example.com).
            return (uint64_t)futex_syscall(a1, (uint32_t)a2, (uint32_t)a3,
                                           a4, a5, (uint32_t)regs->r9);

        case 218: // SYS_set_tid_address (Linux standard)
            /* Phase 22a: register this thread's clear-tid word. Real Linux
               returns the CALLER's tid; the historical constant-1 return is
               wrong for every thread after the first and broke nothing only
               because nothing compared it. musl calls this at pthread start
               as a belt-and-suspenders alternative to clone's ctid. */
            sched_current()->clear_tid = (uint32_t *)a1;
            return (uint64_t)sched_current()->id;

        case 229: // legacy duplicate seen in some musl paths
            return (uint64_t)sched_current()->id;

        /* Custom / Extended Syscalls */
        case 400: // SYS_draw_rect
            comp_fill_rect((uint32_t)a1, (uint32_t)a2, (uint32_t)a3, (uint32_t)a4, (uint32_t)a5);
            return 0;

        case 401: // SYS_mark_dirty
            comp_mark_dirty();
            return 0;

        case 404: // SYS_kfree
            if (a1) {
                kfree((void *)a1);
            }
            return 0;

        case 405: // SYS_spawn -- launch a1 (const char *path) as a new,
                   // isolated process (kernel/process.c: process_spawn()).
                   // Inherits every non-FD_CLOEXEC fd from the caller at
                   // the same fd number. Returns the new pid, or -1 on
                   // failure.
            {
                if (!a1) return (uint64_t)-1;

                /* process_spawn() switches CR3 to the NEW (still-empty)
                   process's PML4 before elf_load() ever dereferences the
                   path. If `path` were left as a raw pointer into the
                   CALLING process's own memory, that dereference would
                   fault as soon as CR3 changes (the new PML4 has nothing
                   mapped there yet) -- caught this live via a kernel panic
                   (CR2 pointed straight into the target binary's own
                   address range, PML4E=0 -- a fresh, unpopulated table).
                   Copy the path into kernel memory first, while the
                   caller's own CR3/PML4 is still active, exactly like a
                   real execve() syscall copies path/argv/envp in before
                   doing anything else. */
                char *path_buf = (char *)kmalloc(MAX_PATH);
                if (!path_buf) return (uint64_t)-1;
                if (strncpy_from_user(path_buf, (const void *)a1, MAX_PATH) < 0) {
                    kfree(path_buf);
                    return (uint64_t)-14; /* -EFAULT */
                }

                extern Process *process_spawn(const char *path, const char **argv, uint32_t uid);
                Process *child = process_spawn(path_buf, NULL, PROC_UID_INHERIT);
                kfree(path_buf);
                if (!child) return (uint64_t)-1;
                return child->pid;
            }

        case 406: // SYS_fb_flush(x, y, w, h) -- push a dirty rect of the
                   // active framebuffer (see fb_active_phys_addr(), which
                   // /dev/fb0's mmap/read/write now target) to the real
                   // display. Needed because virtio-gpu requires an
                   // explicit TRANSFER+FLUSH before it re-reads its
                   // resource's backing memory -- nothing else calls this
                   // on behalf of a Ring-3 writer. No-op when virtio-gpu
                   // isn't the active display (GOP framebuffer is scanned
                   // directly, no flush needed).
            {
                extern bool virtio_gpu_is_active(void);
                extern void virtio_gpu_flush(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
                if (virtio_gpu_is_active()) {
                    virtio_gpu_flush((uint32_t)a1, (uint32_t)a2, (uint32_t)a3, (uint32_t)a4);
                }
                return 0;
            }

        case 408: // SYS_spawn_argv(path_ptr, argv_ptr) -- like SYS_spawn
                   // (405) but also hands the child a real argv array: a2
                   // is a NULL-terminated array of user-space char*
                   // pointers. Kept as a SEPARATE syscall number rather
                   // than overloading 405's a2, because existing 2-arg
                   // callers of 405 (via musl's variadic syscall()) have
                   // no guarantee unused argument registers are zeroed --
                   // reusing 405 would risk misinterpreting register
                   // garbage as a real argv pointer and dereferencing it.
            {
                if (!a1) return (uint64_t)-1;

                char *path_buf = (char *)kmalloc(MAX_PATH);
                if (!path_buf) return (uint64_t)-1;
                if (strncpy_from_user(path_buf, (const void *)a1, MAX_PATH) < 0) {
                    kfree(path_buf);
                    return (uint64_t)-14; /* -EFAULT */
                }

                char *argv_bufs[MAX_SPAWN_ARGV] = {0};
                const char *kargv[MAX_SPAWN_ARGV + 1];
                int argc = copy_argv_array_from_user((const char *const *)a2, argv_bufs, kargv, MAX_SPAWN_ARGV);
                if (argc < 0) { argc = 0; kargv[0] = NULL; }

                extern Process *process_spawn(const char *path, const char **argv, uint32_t uid);
                Process *child = process_spawn(path_buf, argc > 0 ? kargv : NULL, PROC_UID_INHERIT);

                kfree(path_buf);
                for (int k = 0; k < argc; k++) kfree(argv_bufs[k]);

                if (!child) return (uint64_t)-1;
                return child->pid;
            }

        case 409: // SYS_wynland_elevate(password_ptr) -- Phase 5's minimal
                   // password gate. Not cryptographically secure -- a
                   // plain string compare against a constant compiled
                   // into the kernel (no crypto library exists in this
                   // tree). A real, working, auditable authentication
                   // boundary, not a hardened one; documented as such
                   // rather than overclaimed. On match, elevates the
                   // CALLING process's own uid to 0 IN PLACE (like `su`,
                   // not spawning a new process) -- any command it later
                   // runs via SYS_spawn_argv (408, PROC_UID_INHERIT)
                   // correctly inherits uid 0.
            {
                if (!a1) return (uint64_t)-1;
                static const char root_password[] = "wynland";
                char given[32];
                if (strncpy_from_user(given, (const void *)a1, sizeof(given)) < 0) {
                    return (uint64_t)-14; /* -EFAULT */
                }
                if (str_compare(given, root_password) == 0) {
                    sched_current()->proc->uid = 0;
                    return 0;
                }
                return (uint64_t)-1;
            }

        case 410: // SYS_pty_create(int fds[2]) -- Phase 18. Allocates a
                   // real Pty (dual ring buffer + termios + winsize),
                   // writes fds[0]=master, fds[1]=slave, mirroring the
                   // existing pipe()/pipe2() int[2] output convention
                   // (case 22/293 above). No canonical-mode line
                   // discipline is implemented -- the child (nano) always
                   // requests raw mode itself via tcsetattr(), so the PTY
                   // layer just passes bytes through unconditionally in
                   // both directions.
            {
                int *ptyfd = (int *)a1;
                if (!ptyfd || !user_prepare_write(a1, 2 * sizeof(int))) return (uint64_t)-14; /* -EFAULT */

                Pty *pty = NULL;
                int pty_idx = -1;
                for (int i = 0; i < MAX_PTYS; i++) {
                    if (!g_ptys[i]) {
                        pty = (Pty *)kmalloc(sizeof(Pty));
                        if (!pty) return (uint64_t)-12; /* -ENOMEM */
                        memset(pty, 0, sizeof(Pty));
                        pty->in_use = true;
                        pty->refcount = 2; /* master + slave */
                        /* Sane raw-mode-friendly defaults; nano overwrites
                           these itself via tcsetattr() right after open. */
                        pty->term.c_iflag = T_ICRNL | T_IXON;
                        pty->term.c_oflag = T_OPOST | T_ONLCR;
                        pty->term.c_cflag = T_CS8 | T_CREAD | T_CLOCAL;
                        pty->term.c_lflag = T_ISIG | T_ICANON | T_ECHO | T_ECHOE | T_ECHOK | T_IEXTEN;
                        pty->ws_row = 24;
                        pty->ws_col = 80;
                        g_ptys[i] = pty;
                        pty_idx = i;
                        break;
                    }
                }
                if (!pty) return (uint64_t)-23; /* -ENFILE */

                int fd_master = get_free_fd(fd_table);
                if (fd_master == -1) {
                    g_ptys[pty_idx] = NULL;
                    kfree(pty);
                    return (uint64_t)-24; /* -EMFILE */
                }
                VfsFile *f_master = (VfsFile *)kmalloc(sizeof(VfsFile));
                memset(f_master, 0, sizeof(VfsFile));
                str_copy(f_master->node.name, "pty_master");
                f_master->node.first_cluster = PTY_FD_MASTER;
                f_master->current_cluster = (uint32_t)pty_idx;
                fd_table[fd_master] = f_master;

                int fd_slave = get_free_fd(fd_table);
                if (fd_slave == -1) {
                    fd_table[fd_master] = NULL;
                    kfree(f_master);
                    g_ptys[pty_idx] = NULL;
                    kfree(pty);
                    return (uint64_t)-24; /* -EMFILE */
                }
                VfsFile *f_slave = (VfsFile *)kmalloc(sizeof(VfsFile));
                memset(f_slave, 0, sizeof(VfsFile));
                str_copy(f_slave->node.name, "pty_slave");
                f_slave->node.first_cluster = PTY_FD_SLAVE;
                f_slave->current_cluster = (uint32_t)pty_idx;
                fd_table[fd_slave] = f_slave;

                ptyfd[0] = fd_master;
                ptyfd[1] = fd_slave;
                return 0;
            }

        case 411: // SYS_process_alive(pid) -- Phase 18. Lets zerp_term.c
                   // poll whether a forked-and-exec'd child (nano) has
                   // exited yet, so it knows when to leave MODE_PTY. Reads
                   // Process.exited (set by thread_exit() itself, see
                   // sched.c) rather than the process's main_thread's
                   // state, since a terminated Thread struct can already be
                   // kfree()'d by the time anything else gets a chance to
                   // look at it -- Process itself is never freed.
            {
                Process *target = process_find_by_pid((uint64_t)a1);
                if (!target) return 0; /* never existed / bogus pid -- treat as dead */
                return target->exited ? 0 : 1;
            }

        case 407: // SYS_shm_create(size) -- allocate a shared-memory
                   // segment and return an fd for it. Mapping it (via
                   // SYS_mmap's 0xFFFFFFFD branch below) into more than one
                   // process's PML4 gives real zero-copy shared memory --
                   // hand the fd to a child via ordinary fd inheritance
                   // (process_spawn(), already proven end-to-end by
                   // spawner.elf/inherited.elf) exactly like a pipe fd.
            {
                uint64_t size = a1;
                if (size == 0) return (uint64_t)-1;

                uint32_t pages = (uint32_t)((size + PAGE_SIZE - 1) / PAGE_SIZE);
                extern void *pmm_alloc_contiguous(uint32_t count);
                void *phys_ptr = pmm_alloc_contiguous(pages);
                if (!phys_ptr) {
                    serial_write_string("SYS_shm_create: pmm_alloc_contiguous failed\r\n");
                    return (uint64_t)-1;
                }
                uint64_t phys_addr = (uint64_t)(uintptr_t)phys_ptr;

                /* Zero it through the caller's own mapping of the kernel's
                   identity-mapped physical range (kernel range is aliased
                   into every process's PML4, see vmm_new_process_pml4()),
                   so this is safe regardless of which process's CR3 is
                   currently loaded. Plain memset -- proven safe from
                   syscall context after the Phase 3 stack-alignment fix
                   (kernel/syscall_entry.asm), no workaround needed. */
                memset((void *)(uintptr_t)phys_addr, 0, pages * PAGE_SIZE);

                int seg_idx = -1;
                for (int i = 0; i < MAX_SHM_SEGMENTS; i++) {
                    if (g_shm_segments[i].phys_addr == 0) {
                        seg_idx = i;
                        break;
                    }
                }
                if (seg_idx == -1) {
                    serial_write_string("SYS_shm_create: out of SHM segment slots\r\n");
                    return (uint64_t)-1;
                }
                g_shm_segments[seg_idx].phys_addr = phys_addr;
                g_shm_segments[seg_idx].size      = pages * PAGE_SIZE;
                g_shm_segments[seg_idx].refcount   = 0;

                int fd = get_free_fd(fd_table);
                if (fd == -1) {
                    g_shm_segments[seg_idx].phys_addr = 0;
                    return (uint64_t)-1;
                }
                VfsFile *f = (VfsFile *)kmalloc(sizeof(VfsFile));
                memset(f, 0, sizeof(VfsFile));
                str_copy(f->node.name, "shm");
                f->node.is_dir = false;
                f->node.first_cluster = 0xFFFFFFFD; // SHM segment signature
                f->current_cluster = seg_idx;        // index into g_shm_segments[]
                fd_table[fd] = f;

                return fd;
            }

        case 5: // SYS_fstat (Linux standard) — get file status by fd
            {
                uint64_t fd = a1;
                struct linux_stat *user_stat = (struct linux_stat *)a2;
                if (!user_stat || !user_prepare_write(a2, sizeof(*user_stat))) return (uint64_t)-14; /* -EFAULT */

                /* Handle stdout/stderr/stdin */
                if (fd <= 2) {
                    memset(user_stat, 0, sizeof(*user_stat));
                    user_stat->st_mode    = S_IFCHR | 0666;
                    user_stat->st_blksize = 4096;
                    user_stat->st_rdev    = ((uint64_t)5 << 8) | 0;
                    return 0;
                }

                if (fd >= MAX_OPEN_FILES || fd_table[fd] == NULL) {
                    return (uint64_t)-9; /* -EBADF */
                }
                fill_stat_from_fd(user_stat, fd_table[fd]);
                return 0;
            }

        case 16: // SYS_ioctl (Linux standard) — stub for terminal/device control
            {
                int fd = (int)a1;
                uint64_t request = a2;
                void *argp = (void *)a3;

                {
                    char fd_str[32], req_str[32];
                    uint_to_hex(fd, fd_str);
                    uint_to_hex(request, req_str);
                    serial_write_string("SYS_ioctl: fd=");
                    serial_write_string(fd_str);
                    serial_write_string(" request=");
                    serial_write_string(req_str);
                    serial_write_string("\r\n");
                }

                // Phase 18: termios / window-size ioctls for PTY master+slave fds
                if (fd >= 0 && fd < MAX_OPEN_FILES && fd_table[fd] != NULL &&
                    (fd_table[fd]->node.first_cluster == PTY_FD_MASTER ||
                     fd_table[fd]->node.first_cluster == PTY_FD_SLAVE)) {
                    Pty *pty = g_ptys[fd_table[fd]->current_cluster];
                    if (!pty) return (uint64_t)-9; // EBADF
                    if (request == 0x5401) { // TCGETS
                        struct linux_termios *out = (struct linux_termios *)argp;
                        if (out) {
                            if (!user_prepare_write(a3, sizeof(*out))) return (uint64_t)-14;
                            *out = pty->term;
                        }
                        return 0;
                    }
                    if (request == 0x5402 || request == 0x5403 || request == 0x5404) { // TCSETS/W/F
                        struct linux_termios *in = (struct linux_termios *)argp;
                        if (in) {
                            if (!user_check_read(a3, sizeof(*in))) return (uint64_t)-14;
                            pty->term = *in;
                        }
                        return 0;
                    }
                    if (request == 0x5413) { // TIOCGWINSZ
                        struct linux_winsize *out = (struct linux_winsize *)argp;
                        if (out) {
                            if (!user_prepare_write(a3, sizeof(*out))) return (uint64_t)-14;
                            out->ws_row = pty->ws_row;
                            out->ws_col = pty->ws_col;
                            out->ws_xpixel = 0;
                            out->ws_ypixel = 0;
                        }
                        return 0;
                    }
                    if (request == 0x5414) { // TIOCSWINSZ
                        struct linux_winsize *in = (struct linux_winsize *)argp;
                        if (in) {
                            if (!user_check_read(a3, sizeof(*in))) return (uint64_t)-14;
                            pty->ws_row = in->ws_row;
                            pty->ws_col = in->ws_col;
                        }
                        return 0;
                    }
                    return (uint64_t)-25; // -ENOTTY for anything else on a PTY fd
                }

                // Handle Framebuffer ioctl queries
                if (fd >= 0 && fd < MAX_OPEN_FILES && fd_table[fd] != NULL && fd_table[fd]->node.first_cluster == 0xFFFFFFF0) {
                    if (request == 0x4600) { // FBIOGET_VSCREENINFO
                        struct fb_var_screeninfo {
                            uint32_t xres;
                            uint32_t yres;
                            uint32_t xres_virtual;
                            uint32_t yres_virtual;
                            uint32_t xoffset;
                            uint32_t yoffset;
                            uint32_t bits_per_pixel;
                            uint32_t grayscale;
                            struct {
                                uint32_t offset;
                                uint32_t length;
                                uint32_t msb_right;
                            } red, green, blue, transp;
                            uint32_t nonstd;
                            uint32_t activate;
                            uint32_t height;
                            uint32_t width;
                            uint32_t accel_flags;
                            uint32_t pixclock;
                            uint32_t left_margin;
                            uint32_t right_margin;
                            uint32_t upper_margin;
                            uint32_t lower_margin;
                            uint32_t hsync_len;
                            uint32_t vsync_len;
                            uint32_t sync;
                            uint32_t vmode;
                            uint32_t rotate;
                            uint32_t colorspace;
                            uint32_t reserved[4];
                        } *vinfo = (struct fb_var_screeninfo *)argp;
                        if (vinfo && g_boot_info) {
                            if (!user_prepare_write(a3, sizeof(*vinfo))) return (uint64_t)-14;
                            memset(vinfo, 0, sizeof(*vinfo));
                            vinfo->xres = g_boot_info->fb_width;
                            vinfo->yres = g_boot_info->fb_height;
                            vinfo->xres_virtual = g_boot_info->fb_width;
                            vinfo->yres_virtual = g_boot_info->fb_height;
                            vinfo->xoffset = 0;
                            vinfo->yoffset = 0;
                            vinfo->bits_per_pixel = 32;
                            vinfo->grayscale = 0;
                            return 0;
                        }
                    }
                    else if (request == 0x4602) { // FBIOGET_FSCREENINFO
                        struct fb_fix_screeninfo {
                            char id[16];
                            uint64_t smem_start;
                            uint32_t smem_len;
                            uint32_t type;
                            uint32_t type_aux;
                            uint32_t visual;
                            uint16_t xpanstep;
                            uint16_t ypanstep;
                            uint16_t ywrapstep;
                            uint32_t line_length;
                            uint64_t mmio_start;
                            uint32_t mmio_len;
                            uint32_t accel;
                            uint16_t capabilities;
                            uint16_t reserved[2];
                        } *finfo = (struct fb_fix_screeninfo *)argp;
                        if (finfo && g_boot_info) {
                            if (!user_prepare_write(a3, sizeof(*finfo))) return (uint64_t)-14;
                            memset(finfo, 0, sizeof(*finfo));
                            // Set ID
                            finfo->id[0] = 'f'; finfo->id[1] = 'b'; finfo->id[2] = '0';
                            extern uint64_t fb_active_phys_addr(void);
                            finfo->smem_start = fb_active_phys_addr();
                            finfo->smem_len = g_boot_info->fb_pitch * g_boot_info->fb_height;
                            finfo->type = 0; // FB_TYPE_PACKED_PIXELS
                            finfo->visual = 2; // FB_VISUAL_TRUECOLOR
                            finfo->line_length = g_boot_info->fb_pitch;
                            return 0;
                        }
                    }
                    return (uint64_t)-22; /* -EINVAL */
                }

                if (a1 <= 2) {
                    return (uint64_t)-25; /* -ENOTTY */
                }

                // DRM device ioctl handling
                if (fd >= 0 && fd < MAX_OPEN_FILES && fd_table[fd] != NULL) {
                    uint32_t cl = fd_table[fd]->node.first_cluster;
                    // card0 = 0xFB0E, renderD128 = 0xFB0F
                    if (cl == 0xFB0E || cl == 0xFB0F) {
                        // DRM_IOCTL_VERSION = 0xC0406400
                        if ((request & 0xFFFF) == 0x6400) {
                            struct drm_version {
                                int version_major;
                                int version_minor;
                                int version_patchlevel;
                                uint64_t name_len;
                                uint64_t name;
                                uint64_t date_len;
                                uint64_t date;
                                uint64_t desc_len;
                                uint64_t desc;
                            } *v = (struct drm_version *)argp;
                            if (v) {
                                if (!user_prepare_write(a3, sizeof(*v))) return (uint64_t)-14;
                                v->version_major = 1;
                                v->version_minor = 0;
                                v->version_patchlevel = 0;
                                if (v->name && v->name_len > 0 && user_prepare_write(v->name, v->name_len)) {
                                    char *n = (char *)(uintptr_t)v->name;
                                    const char *drv = "virtio_gpu";
                                    for (int ki = 0; drv[ki] && (uint64_t)ki < v->name_len - 1; ki++) n[ki] = drv[ki];
                                }
                                if (v->date && v->date_len > 0 && user_prepare_write(v->date, 5)) { char *d = (char *)(uintptr_t)v->date; d[0]='2'; d[1]='0'; d[2]='2'; d[3]='4'; d[4]=0; }
                                if (v->desc && v->desc_len > 0 && user_prepare_write(v->desc, 5)) { char *d = (char *)(uintptr_t)v->desc; d[0]='V'; d[1]='G'; d[2]='P'; d[3]='U'; d[4]=0; }
                            }
                            return 0;
                        }
                        // DRM_IOCTL_GET_UNIQUE = 0xC0106401
                        if ((request & 0xFFFF) == 0x6401) { return 0; }
                        // DRM_IOCTL_GET_MAGIC = 0x80046402
                        if ((request & 0xFFFF) == 0x6402) { return 0; }
                        // DRM_IOCTL_GET_CAP = 0xC010640C
                        if ((request & 0xFFFF) == 0x640C) {
                            uint64_t *cap_pair = (uint64_t *)argp;
                            if (cap_pair && user_prepare_write(a3, 2 * sizeof(uint64_t))) { cap_pair[1] = 0; } // cap value = 0
                            return 0;
                        }
                        // DRM_IOCTL_SET_CLIENT_CAP = 0x4010641D
                        if ((request & 0xFFFF) == 0x641D) { return 0; }
                        // DRM_IOCTL_PRIME_HANDLE_TO_FD = 0xC00C642D
                        if ((request & 0xFFFF) == 0x642D) { return 0; }
                        // DRM_IOCTL_PRIME_FD_TO_HANDLE = 0xC00C642E
                        if ((request & 0xFFFF) == 0x642E) { return 0; }
                        // Catch-all for DRM ioctls (0x64xx) — return success
                        if (((request >> 8) & 0xFF) == 0x64) { return 0; }
                        // virtio-gpu specific ioctls (0x76xx)
                        if (((request >> 8) & 0xFF) == 0x76) { return 0; }
                        return 0; // return success for unknown DRM ioctls
                    }
                }
                return (uint64_t)-25;
            }

        case 19: // SYS_readv (Linux standard) — scatter read
            {
                struct iovec { void *iov_base; size_t iov_len; };
                int iovcnt = (int)a3;
                if (iovcnt < 0 || iovcnt > IOV_MAX_LOCAL) return (uint64_t)-22; /* -EINVAL */

                struct iovec kiov[IOV_MAX_LOCAL];
                if (iovcnt > 0 && copy_from_user(kiov, (const void *)a2, (uint64_t)iovcnt * sizeof(struct iovec)) != 0) {
                    return (uint64_t)-14; /* -EFAULT */
                }
                /* Validate + resolve COW on every destination up front --
                   vfs_read() below writes straight into iov_base, so this
                   needs the same "prepare" treatment as SYS_read's buffer. */
                for (int i = 0; i < iovcnt; i++) {
                    if (kiov[i].iov_base && kiov[i].iov_len > 0 &&
                        !user_prepare_write((uint64_t)(uintptr_t)kiov[i].iov_base, kiov[i].iov_len)) {
                        return (uint64_t)-14; /* -EFAULT */
                    }
                }

                size_t total_read = 0;
                if (a1 >= MAX_OPEN_FILES || fd_table[a1] == NULL) {
                    return (uint64_t)-9; /* -EBADF */
                }
                for (int i = 0; i < iovcnt; i++) {
                    if (kiov[i].iov_base && kiov[i].iov_len > 0) {
                        int nread = vfs_read(fd_table[a1], kiov[i].iov_base, kiov[i].iov_len);
                        if (nread < 0) return (uint64_t)-1;
                        total_read += nread;
                        if ((size_t)nread < kiov[i].iov_len) break; /* short read */
                    }
                }
                return total_read;
            }

        case 21: // SYS_access (Linux standard) — check file accessibility
            {
                if (!a1) return (uint64_t)-14; /* -EFAULT */
                char path[MAX_PATH];
                if (strncpy_from_user(path, (const void *)a1, sizeof(path)) < 0) {
                    return (uint64_t)-14; /* -EFAULT */
                }
                VfsStat vst;
                if (vfs_stat(path, &vst)) {
                    return 0; /* File exists and is accessible */
                }
                return (uint64_t)-2; /* -ENOENT */
            }

        case 72: // SYS_fcntl (Linux standard) — file descriptor control
            {
                uint64_t fd = a1;
                int cmd = (int)a2;

                /* stdin/stdout/stderr are always valid */
                if (fd > 2 && (fd >= MAX_OPEN_FILES || fd_table[fd] == NULL)) {
                    return (uint64_t)-9; /* -EBADF */
                }

                switch (cmd) {
                    case F_GETFD:
                        return (fd < MAX_OPEN_FILES) ? fd_flags[fd] : 0;
                    case F_SETFD:
                        if (fd < MAX_OPEN_FILES) fd_flags[fd] = (uint32_t)a3;
                        return 0;
                    case F_GETFL:
                        return (fd < MAX_OPEN_FILES) ? fd_oflags[fd] : LINUX_O_RDONLY;
                    case F_SETFL:
                        if (fd < MAX_OPEN_FILES) fd_oflags[fd] = (uint32_t)a3;
                        return 0;
                    case F_DUPFD: {
                        /* Find first free fd >= a3 */
                        int min_fd = (int)a3;
                        if (min_fd < 3) min_fd = 3;
                        for (int i = min_fd; i < MAX_OPEN_FILES; i++) {
                            if (fd_table[i] == NULL && i != (int)fd) {
                                /* We don't actually dup the VfsFile handle, just alias it */
                                fd_table[i] = fd_table[fd];
                                fd_flags[i] = 0; /* F_DUPFD clears CLOEXEC */
                                fd_oflags[i] = fd_oflags[fd];
                                return i;
                            }
                        }
                        return (uint64_t)-24; /* -EMFILE */
                    }
                    default:
                        return 0; /* Stub: unknown fcntl commands succeed silently */
                }
            }

        case 79: // SYS_getcwd (Linux standard) — get current working directory
            {
                char *buf = (char *)a1;
                uint64_t size = a2;
                if (!buf || size < 2) return (uint64_t)-34; /* -ERANGE */
                if (!user_prepare_write(a1, 2)) return (uint64_t)-14; /* -EFAULT */
                buf[0] = '/';
                buf[1] = '\0';
                return (uint64_t)(uintptr_t)buf;
            }

        case 97: // SYS_getrlimit (Linux standard) — stub
        case 302: // SYS_prlimit64
            {
                /* Return generous defaults for RLIMIT queries */
                struct rlimit64 {
                    uint64_t rlim_cur;
                    uint64_t rlim_max;
                };
                if (num == 302 && a3) {
                    if (!user_prepare_write(a3, sizeof(struct rlimit64))) return (uint64_t)-14; /* -EFAULT */
                    struct rlimit64 *out = (struct rlimit64 *)a3;
                    out->rlim_cur = 0x100000; /* 1 MB stack */
                    out->rlim_max = 0x100000;
                }
                return 0;
            }

        case 102: // SYS_getuid
        case 104: // SYS_getgid
        case 107: // SYS_geteuid
        case 108: // SYS_getegid
            /* Phase 5: real per-process uid (Process.uid, kernel/process.c).
               No separate GID model -- gid mirrors uid, matching the
               convention that root (uid 0) is also "group 0". */
            return sched_current()->proc->uid;

        case 110: // SYS_getppid
            return 1;

        case 186: // SYS_gettid
            {
                Thread *curr = sched_current();
                return curr ? curr->id : 1;
            }

        case 228: // SYS_clock_gettime
            {
                struct timespec {
                    int64_t tv_sec;
                    int64_t tv_nsec;
                };
                struct timespec *tp = (struct timespec *)a2;
                if (tp) {
                    if (!user_prepare_write(a2, sizeof(*tp))) return (uint64_t)-14; /* -EFAULT */
                    extern uint64_t timer_get_ticks(void);
                    uint64_t ticks = timer_get_ticks();
                    /* CLOCK_REALTIME (0) -- real wall-clock UTC, from the
                       CMOS RTC read at boot (see kernel/rtc.c). Everything
                       else (CLOCK_MONOTONIC=1, etc.) stays uptime-based,
                       matching real Linux semantics: monotonic clocks are
                       never supposed to jump with wall-clock adjustments. */
                    if ((int)a1 == 0) {
                        tp->tv_sec = (int64_t)rtc_get_unix_time();
                    } else {
                        tp->tv_sec = (int64_t)(ticks / 100);
                    }
                    tp->tv_nsec = (int64_t)((ticks % 100) * 10000000);
                }
                return 0;
            }
        case 201: // SYS_time (legacy, some programs call this directly
                   // instead of clock_gettime) -- real wall-clock UTC.
            {
                int64_t now = (int64_t)rtc_get_unix_time();
                int64_t *tloc = (int64_t *)a1;
                if (tloc) {
                    if (!user_prepare_write(a1, sizeof(*tloc))) return (uint64_t)-14; /* -EFAULT */
                    *tloc = now;
                }
                return (uint64_t)now;
            }
        case 217: // SYS_getdents64 (Linux standard)
            {
                int fd = (int)a1;
                if (fd < 0 || fd >= MAX_OPEN_FILES || fd_table[fd] == NULL) {
                    return (uint64_t)-9; /* -EBADF */
                }
                if (a3 != 0 && !user_prepare_write(a2, a3)) {
                    return (uint64_t)-14; /* -EFAULT */
                }
                return (uint64_t)vfs_getdents(fd_table[fd], (void *)a2, (uint32_t)a3);
            }

        case 257: // SYS_openat (Linux standard) — open relative to dirfd
            {
                /* If dirfd == AT_FDCWD (-100), treat as regular open */
                if (!a2) return (uint64_t)-14;
                char path[MAX_PATH];
                if (strncpy_from_user(path, (const void *)a2, sizeof(path)) < 0) {
                    return (uint64_t)-14; /* -EFAULT */
                }
                serial_write_string("Syscall: openat path: ");
                serial_write_string(path);
                serial_write_string("\r\n");

                int linux_flags = (int)a3;
                uint32_t vfs_flags = 0;
                if ((linux_flags & 3) == 0) vfs_flags |= 0x01; // VFS_O_READ
                if ((linux_flags & 3) == 1) vfs_flags |= 0x02; // VFS_O_WRITE
                if ((linux_flags & 3) == 2) vfs_flags |= (0x01 | 0x02);
                if (linux_flags & 0100) vfs_flags |= 0x04; // VFS_O_CREATE
                if (linux_flags & 01000) vfs_flags |= 0x10; // VFS_O_TRUNC
                if (linux_flags & 02000) vfs_flags |= 0x08; // VFS_O_APPEND

                int fd = get_free_fd(fd_table);
                if (fd == -1) return (uint64_t)-24; /* -EMFILE */
                
                if (str_compare(path, "/sys/dev/char/226:0/device/vendor") == 0 ||
                    str_compare(path, "/sys/dev/char/226:128/device/vendor") == 0 ||
                    str_compare(path, "/sys/dev/char/226:0/device/subsystem_vendor") == 0 ||
                    str_compare(path, "/sys/dev/char/226:128/device/subsystem_vendor") == 0 ||
                    str_compare(path, "/sys/devices/pci0000:00/0000:00:02.0/vendor") == 0 ||
                    str_compare(path, "/sys/devices/pci0000:00/0000:00:02.0/subsystem_vendor") == 0) {
                    VfsFile *sfile = kmalloc(sizeof(VfsFile));
                    memset(sfile, 0, sizeof(VfsFile));
                    sfile->node.first_cluster = 0xFFFFFFF4; // VENDOR
                    sfile->node.size = 7;
                    fd_table[fd] = sfile;
                    return fd;
                }
                if (str_compare(path, "/sys/dev/char/226:0/device/device") == 0 ||
                    str_compare(path, "/sys/dev/char/226:128/device/device") == 0 ||
                    str_compare(path, "/sys/dev/char/226:0/device/subsystem_device") == 0 ||
                    str_compare(path, "/sys/dev/char/226:128/device/subsystem_device") == 0 ||
                    str_compare(path, "/sys/devices/pci0000:00/0000:00:02.0/device") == 0 ||
                    str_compare(path, "/sys/devices/pci0000:00/0000:00:02.0/subsystem_device") == 0) {
                    VfsFile *sfile = kmalloc(sizeof(VfsFile));
                    memset(sfile, 0, sizeof(VfsFile));
                    sfile->node.first_cluster = 0xFFFFFFF5; // DEVICE
                    sfile->node.size = 7;
                    fd_table[fd] = sfile;
                    return fd;
                }
                if (str_compare(path, "/sys/dev/char/226:0/device/revision") == 0 ||
                    str_compare(path, "/sys/dev/char/226:128/device/revision") == 0 ||
                    str_compare(path, "/sys/devices/pci0000:00/0000:00:02.0/revision") == 0) {
                    VfsFile *sfile = kmalloc(sizeof(VfsFile));
                    memset(sfile, 0, sizeof(VfsFile));
                    sfile->node.first_cluster = 0xFFFFFFFA; // REVISION
                    sfile->node.size = 5;
                    fd_table[fd] = sfile;
                    return fd;
                }
                if (str_compare(path, "/sys/dev/char/226:0/device/uevent") == 0) {
                    VfsFile *sfile = kmalloc(sizeof(VfsFile));
                    memset(sfile, 0, sizeof(VfsFile));
                    sfile->node.first_cluster = 0xFFFFFFF6; // UEVENT_CARD
                    sfile->node.size = 60;
                    fd_table[fd] = sfile;
                    return fd;
                }
                if (str_compare(path, "/sys/dev/char/226:128/device/uevent") == 0) {
                    VfsFile *sfile = kmalloc(sizeof(VfsFile));
                    memset(sfile, 0, sizeof(VfsFile));
                    sfile->node.first_cluster = 0xFFFFFFF7; // UEVENT_RENDER
                    sfile->node.size = 60;
                    fd_table[fd] = sfile;
                    return fd;
                }
                // /sys/dev/char/226:0/device/drm — virtual dir listing "card0"
                if (str_compare(path, "/sys/dev/char/226:0/device/drm") == 0) {
                    VfsFile *sfile = kmalloc(sizeof(VfsFile));
                    memset(sfile, 0, sizeof(VfsFile));
                    sfile->node.first_cluster = 0xFFFFFFF8; // DRM_DIR card
                    sfile->node.is_dir = 1;
                    fd_table[fd] = sfile;
                    return fd;
                }
                // /sys/dev/char/226:128/device/drm — virtual dir listing "renderD128"
                if (str_compare(path, "/sys/dev/char/226:128/device/drm") == 0) {
                    VfsFile *sfile = kmalloc(sizeof(VfsFile));
                    memset(sfile, 0, sizeof(VfsFile));
                    sfile->node.first_cluster = 0xFFFFFFF9; // DRM_DIR render
                    sfile->node.is_dir = 1;
                    fd_table[fd] = sfile;
                    return fd;
                }
                
                VfsFile *file = vfs_open_flags(path, vfs_flags);
                if (!file) return (uint64_t)-2; /* -ENOENT */
                fd_table[fd] = file;
                return fd;
            }

        case 262: // SYS_newfstatat (Linux standard) — fstat relative to dirfd
            {
                char path[MAX_PATH];
                struct linux_stat *user_stat = (struct linux_stat *)a3;
                if (!a2 || !user_stat) return (uint64_t)-14;
                if (strncpy_from_user(path, (const void *)a2, sizeof(path)) < 0 ||
                    !user_prepare_write(a3, sizeof(*user_stat))) {
                    return (uint64_t)-14; /* -EFAULT */
                }

                VfsStat vst;
                if (!vfs_stat(path, &vst)) {
                    // MOCK /sys/dev/char/... for libdrm
                    if (str_compare(path, "/sys/dev/char/226:0/device/drm") == 0 ||
                        str_compare(path, "/sys/dev/char/226:128/device/drm") == 0 ||
                        str_compare(path, "/sys/devices/pci0000:00/0000:00:02.0") == 0 ||
                        str_compare(path, "/sys/devices/pci0000:00") == 0) {
                        memset(user_stat, 0, sizeof(*user_stat));
                        user_stat->st_dev     = 1;
                        user_stat->st_ino     = 9999;
                        user_stat->st_nlink   = 1;
                        user_stat->st_blksize = 4096;
                        user_stat->st_mode = S_IFDIR | 0755;
                        user_stat->st_blocks = 1;
                        return 0;
                    }
                    return (uint64_t)-2; /* -ENOENT */
                }
                memset(user_stat, 0, sizeof(*user_stat));
                user_stat->st_dev     = 1;
                user_stat->st_ino     = (uint64_t)vst.first_cluster;
                user_stat->st_nlink   = 1;
                user_stat->st_blksize = 4096;
                if (vst.is_dir) {
                    user_stat->st_mode = S_IFDIR | 0755;
                } else {
                    user_stat->st_mode = S_IFREG | 0644;
                    user_stat->st_size = (int64_t)vst.size;
                    
                    if (str_compare(path, "/dev/dri/renderD128") == 0 || 
                        str_compare(path, "/dev/dri/card0") == 0 ||
                        str_compare(vst.name, "renderd1") == 0 ||
                        str_compare(vst.name, "card0") == 0) {
                        user_stat->st_mode = S_IFCHR | 0666;
                        if (str_compare(path, "/dev/dri/card0") == 0 || str_compare(vst.name, "card0") == 0) {
                            user_stat->st_rdev = ((uint64_t)226 << 8) | 0; /* major 226, minor 0 */
                        } else {
                            user_stat->st_rdev = ((uint64_t)226 << 8) | 128; /* major 226, minor 128 */
                        }
                    }
                }
                user_stat->st_blocks = (user_stat->st_size + 511) / 512;
                return 0;
            }

        case 200: // SYS_tkill(tid, sig) -- 2-arg form; the unused a3
                  // register holds UNINITIALIZED USER GARBAGE (same lesson
                  // as case 408 -- never read unused arg registers).
        case 234: // SYS_tgkill(tgid, tid, sig) -- 3-arg form.
            {
                /* Phase 22b: raise a real pending signal on the calling
                   thread. Delivered at this same syscall's return tail, so
                   raise()/abort() land in the installed handler (or die by
                   default action) right here, synchronously from the
                   caller's point of view. Pick the signal slot by syscall
                   NUMBER: tkill carries sig in a2, tgkill in a3. */
                int sig = (num == 200) ? (int)a2 : (int)a3;
                signal_raise_current(sig);
return 0;
            }

        case 204: // SYS_sched_getparam (Linux standard)
            return (uint64_t)-38; /* -ENOSYS */

        case 324: // SYS_memfd_create (Linux standard)
            return (uint64_t)-38; /* -ENOSYS */

        case 273: // SYS_set_robust_list (Linux standard)
            return 0;

        case 157: // SYS_prctl (Linux standard)
            return 0;

        case 7:   // SYS_poll (Linux standard) -- shares ppoll's implementation.
                   // a3 here is a plain int timeout_ms (poll()'s real third
                   // arg), NOT a timespec* like ppoll's -- same register,
                   // different meaning, disambiguated below via `num`.
        case 271: // SYS_ppoll (Linux standard)
            {
                uint64_t nfds = a2;
                if (nfds > POLL_MAX_LOCAL) return (uint64_t)-22; /* -EINVAL: also bounds the kernel copy below */

                /* Copy the caller's array into a bounded kernel buffer --
                   do_poll() writes revents into it repeatedly across
                   however many blocking iterations it takes, and none of
                   that is safe to do directly against a raw user pointer
                   (unmapped, kernel-space, or a COW page mid-syscall). */
                bool have_fds = (a1 != 0);
                struct wl_pollfd kfds[POLL_MAX_LOCAL];
                if (have_fds && nfds > 0 && copy_from_user(kfds, (const void *)a1, nfds * sizeof(struct wl_pollfd)) != 0) {
                    return (uint64_t)-14; /* -EFAULT */
                }

                /* Phase 22d: real timeout instead of the old single-check-
                   then-yield-then-return-0 (which silently ignored
                   whatever timeout the caller asked for). -1 = block
                   forever, 0 = poll once and return immediately, >0 =
                   real tick budget. Timer runs at 100Hz (kernel/irq.c). */
                int64_t timeout_ticks;
                if (num == 7) {
                    int timeout_ms = (int)(int32_t)a3;
                    timeout_ticks = (timeout_ms < 0) ? -1 : (int64_t)timeout_ms / 10;
                } else if (a3 == 0) {
                    timeout_ticks = -1;
                } else {
                    struct linux_timespec { int64_t tv_sec; int64_t tv_nsec; } tmo;
                    if (copy_from_user(&tmo, (const void *)a3, sizeof(tmo)) != 0) return (uint64_t)-14;
                    timeout_ticks = tmo.tv_sec * 100 + tmo.tv_nsec / 10000000;
                }

                int ready = do_poll(fd_table, have_fds ? kfds : NULL, nfds, timeout_ticks);

                if (have_fds && nfds > 0 && copy_to_user((void *)a1, kfds, nfds * sizeof(struct wl_pollfd)) != 0) {
                    return (uint64_t)-14; /* -EFAULT */
                }
                return (uint64_t)ready;
            }
        case 41: // SYS_socket
            {
                int domain = (int)a1;
                int type   = (int)a2 & 0xFF; // mask off SOCK_NONBLOCK/SOCK_CLOEXEC flag bits

                /* This OS's network stack is IPv4-only -- no IPv6 anywhere
                   in drivers/net/. AF_INET6 (10) must fail here, at
                   socket() creation time, with a real error, exactly like
                   a real kernel built without IPv6 support would. Letting
                   it silently fall through to the generic AF_UNIX-mock
                   branch below (as it did before this check existed) was
                   a real, previously-invisible bug: SYS_connect()
                   unconditionally "succeeds" for any non-TCP/UDP socket,
                   so a caller doing real Happy-Eyeballs-style dual-stack
                   connection racing (e.g. curl, which tries AAAA records
                   before falling back to A records) would believe its
                   IPv6 attempt had genuinely connected, then hang forever
                   trying to actually use that fake, unbacked fd for a
                   real protocol handshake -- never reaching the IPv4
                   fallback its own logic would otherwise correctly take
                   (confirmed on the real host: IPv6 connect there fails
                   fast with ENETUNREACH, and curl falls back to IPv4
                   immediately). */
                if (domain == 10 /* AF_INET6 */) {
                    return (uint64_t)-97; // -EAFNOSUPPORT
                }

                int fd = get_free_fd(fd_table);
                if (fd < 0) return -24; // EMFILE

                VfsFile *sfile = kmalloc(sizeof(VfsFile));
                memset(sfile, 0, sizeof(VfsFile));
                if (domain == 2 /* AF_INET */ && type == 1 /* SOCK_STREAM */) {
                    sfile->node.first_cluster = SOCK_FD_TCP;
                    sfile->current_cluster    = TCP_FD_NOT_CONNECTED;
                } else if (domain == 2 /* AF_INET */ && type == 2 /* SOCK_DGRAM */) {
                    int udp_idx = udp_socket_create();
                    if (udp_idx < 0) {
                        kfree(sfile);
                        return -24; // EMFILE (no free UDP socket slots)
                    }
                    sfile->node.first_cluster = SOCK_FD_UDP;
                    sfile->current_cluster    = (uint32_t)udp_idx;
                } else {
                    sfile->node.first_cluster = 0xFFFFFFF8; // Magic for AF_UNIX socket (unchanged mock)
                }
                fd_table[fd] = sfile;
                return fd;
            }

        case 42: // SYS_connect
            {
                int fd = (int)a1;
                if (fd < 0 || fd >= MAX_OPEN_FILES || fd_table[fd] == NULL) return -9; // EBADF

                VfsFile *file = fd_table[fd];
                struct linux_sockaddr_in addr;
                bool have_addr = (a2 != 0) && (copy_from_user(&addr, (const void *)a2, sizeof(addr)) == 0);

                if (file->node.first_cluster == SOCK_FD_UDP) {
                    if (!have_addr) return -14; // EFAULT
                    udp_socket_connect((int)file->current_cluster, addr.sin_addr, ntohs(addr.sin_port));
                    return 0;
                }

                if (file->node.first_cluster != SOCK_FD_TCP) {
                    return 0; // AF_UNIX / other mock sockets: pretend success (unchanged)
                }

                if (!have_addr) return -14; // EFAULT

                int idx = tcp_connect_slot(addr.sin_addr, ntohs(addr.sin_port));
                if (idx < 0) return -110; // ETIMEDOUT (covers both handshake timeout and RST)

                file->current_cluster = (uint32_t)idx;
                return 0;
            }

        case 44: // SYS_sendto(fd, buf, len, flags, dest_addr, addrlen) -- dest_addr is a5
            {
                int fd = (int)a1;
                if (fd < 0 || fd >= MAX_OPEN_FILES || fd_table[fd] == NULL) return -9; // EBADF
                if (a3 != 0 && !user_check_read(a2, a3)) return -14; // EFAULT
                VfsFile *file = fd_table[fd];

                if (file->node.first_cluster == SOCK_FD_UDP) {
                    struct linux_sockaddr_in addr;
                    bool have_addr = (a5 != 0) && (copy_from_user(&addr, (const void *)a5, sizeof(addr)) == 0);
                    uint32_t dst_ip   = have_addr ? addr.sin_addr : 0;
                    uint16_t dst_port = have_addr ? ntohs(addr.sin_port) : 0;
                    int n = udp_socket_sendto((int)file->current_cluster, (const void *)a2, (uint32_t)a3, dst_ip, dst_port);
                    if (n < 0) return (uint64_t)-1;
                    return (uint64_t)n;
                }

                if (file->node.first_cluster == SOCK_FD_TCP) {
                    if (file->current_cluster == TCP_FD_NOT_CONNECTED) return -107; // ENOTCONN
                    TcpConnection *conn = tcp_get_connection((int)file->current_cluster);
                    if (!conn) return -104; // ECONNRESET
                    int n = tcp_send(conn, (const void *)a2, (uint32_t)a3);
                    if (n < 0) return (uint64_t)-1;
                    return (uint64_t)n;
                }
                // Non-TCP/UDP fds: fall back to plain write() semantics
                return syscall_dispatcher(1, a1, a2, a3, 0, 0, regs);
            }

        case 45: // SYS_recvfrom(fd, buf, len, flags, src_addr, addrlen) -- src_addr is a5, addrlen* is r9
            {
                int fd = (int)a1;
                if (fd < 0 || fd >= MAX_OPEN_FILES || fd_table[fd] == NULL) return -9; // EBADF
                if (a3 != 0 && !user_prepare_write(a2, a3)) return -14; // EFAULT
                VfsFile *file = fd_table[fd];

                if (file->node.first_cluster == SOCK_FD_UDP) {
                    uint32_t from_ip = 0;
                    uint16_t from_port = 0;
                    int n = udp_socket_recvfrom((int)file->current_cluster, (void *)a2, (uint32_t)a3, &from_ip, &from_port);
                    if (n < 0) return (uint64_t)-1;
                    if (a5) {
                        struct linux_sockaddr_in addr;
                        addr.sin_family = 2; // AF_INET
                        addr.sin_port   = htons(from_port);
                        addr.sin_addr   = from_ip;
                        memset(addr.sin_zero, 0, sizeof(addr.sin_zero));
                        if (copy_to_user((void *)a5, &addr, sizeof(addr)) != 0) return (uint64_t)-14;
                        if (regs->r9) {
                            uint32_t addrlen = sizeof(struct linux_sockaddr_in);
                            if (copy_to_user((void *)regs->r9, &addrlen, sizeof(addrlen)) != 0) return (uint64_t)-14;
                        }
                    }
                    return (uint64_t)n;
                }

                if (file->node.first_cluster == SOCK_FD_TCP) {
                    if (file->current_cluster == TCP_FD_NOT_CONNECTED) return -107; // ENOTCONN
                    TcpConnection *conn = tcp_get_connection((int)file->current_cluster);
                    if (!conn) return -104; // ECONNRESET
                    int n = tcp_recv(conn, (void *)a2, (uint32_t)a3);
                    if (n < 0) return (uint64_t)-1;
                    return (uint64_t)n;
                }
                // Non-TCP/UDP fds: fall back to plain read() semantics
                return syscall_dispatcher(0, a1, a2, a3, 0, 0, regs);
            }

        case 49: // SYS_bind
        case 50: // SYS_listen
        case 54: // SYS_setsockopt
        case 55: // SYS_getsockopt
        case 48: // SYS_shutdown
            return 0; // Pretend success

        case 51: // SYS_getsockname
            {
                // struct sockaddr *addr = (struct sockaddr *)a2;
                // socklen_t *addrlen = (socklen_t *)a3;
                return 0; // Return 0 to pretend success, libwayland doesn't strictly check the name if it bound successfully
            }

        case 43: // SYS_accept
            {
                // For MVP Phase 1 (no clients), just yield or block
                sched_yield();
                return -11; // EAGAIN
            }

        case 213: // SYS_epoll_create
        case 291: // SYS_epoll_create1
            {
                int fd = get_free_fd(fd_table);
                if (fd < 0) return -24; // EMFILE
                
                VfsFile *efile = kmalloc(sizeof(VfsFile));
                memset(efile, 0, sizeof(VfsFile));
                efile->node.first_cluster = 0xFFFFFFF9; // Magic for epoll
                fd_table[fd] = efile;
                
                for (int i = 0; i < MAX_EPOLL_INSTANCES; i++) {
                    if (!epoll_instances[i].active) {
                        epoll_instances[i].active = 1;
                        epoll_instances[i].fd = fd;
                        epoll_instances[i].num_watches = 0;
                        break;
                    }
                }
                return fd;
            }

        case 233: // SYS_epoll_ctl
            {
                int epfd = (int)a1;
                int op = (int)a2;
                int fd = (int)a3;

                EpollInstance *inst = get_epoll_instance(epfd);
                if (!inst) return -9; // EBADF

                struct epoll_event event;
                if (op != EPOLL_CTL_DEL) { // real epoll_ctl allows event==NULL only for DEL
                    if (copy_from_user(&event, (const void *)a4, sizeof(event)) != 0) return -14; // EFAULT
                }

                if (op == EPOLL_CTL_ADD) {
                    if (inst->num_watches >= MAX_EPOLL_EVENTS) return -12; // ENOMEM
                    inst->watches[inst->num_watches].target_fd = fd;
                    inst->watches[inst->num_watches].ev = event;
                    inst->num_watches++;
                } else if (op == EPOLL_CTL_DEL) {
                    for (int i = 0; i < inst->num_watches; i++) {
                        if (inst->watches[i].target_fd == fd) {
                            inst->watches[i] = inst->watches[inst->num_watches - 1];
                            inst->num_watches--;
                            break;
                        }
                    }
                } else if (op == EPOLL_CTL_MOD) {
                    for (int i = 0; i < inst->num_watches; i++) {
                        if (inst->watches[i].target_fd == fd) {
                            inst->watches[i].ev = event;
                            break;
                        }
                    }
                }
                return 0;
            }

        case 232: // SYS_epoll_wait
        case 281: // SYS_epoll_pwait
            {
                int epfd = (int)a1;
                struct epoll_event *events = (struct epoll_event *)a2;
                int maxevents = (int)a3;
                if (maxevents > 0 && !user_prepare_write(a2, (uint64_t)maxevents * sizeof(struct epoll_event))) {
                    return (uint64_t)-14; /* -EFAULT */
                }
                /* epoll_wait(epfd, events, maxevents, timeout) -- timeout
                   is a4, milliseconds, same convention as poll()'s third
                   arg. Previously dropped entirely (the inner dispatch
                   always passed 0), so epoll_wait never actually honored
                   its own caller's timeout even before Phase 22d. */
                int timeout_ms = (int)(int32_t)a4;

                EpollInstance *inst = get_epoll_instance(epfd);
                if (!inst) return -9; // EBADF

                struct wl_pollfd pfds[MAX_EPOLL_EVENTS];
                for (int i = 0; i < inst->num_watches; i++) {
                    pfds[i].fd = inst->watches[i].target_fd;
                    pfds[i].events = 0;
                    if (inst->watches[i].ev.events & EPOLLIN) pfds[i].events |= 0x0001; // POLLIN
                    if (inst->watches[i].ev.events & EPOLLOUT) pfds[i].events |= 0x0004; // POLLOUT
                    pfds[i].revents = 0;
                }

                // Call do_poll() directly -- pfds is a kernel stack buffer,
                // never user memory, so this must NOT go back through
                // syscall_dispatcher(7, ...), which now validates its a1 as
                // a genuine user pointer.
                int64_t timeout_ticks = (timeout_ms < 0) ? -1 : (int64_t)timeout_ms / 10;
                int ready = do_poll(fd_table, pfds, inst->num_watches, timeout_ticks);

                if (ready > 0) {
                    int ev_count = 0;
                    for (int i = 0; i < inst->num_watches; i++) {
                        if (pfds[i].revents && ev_count < maxevents) {
                            events[ev_count].data = inst->watches[i].ev.data;
                            events[ev_count].events = 0;
                            if (pfds[i].revents & 0x0001) events[ev_count].events |= EPOLLIN;
                            if (pfds[i].revents & 0x0004) events[ev_count].events |= EPOLLOUT;
                            if (pfds[i].revents & 0x0020) events[ev_count].events |= EPOLLERR;
                            ev_count++;
                        }
                    }
                    return ev_count;
                }
                return 0;
            }

        case 318: // SYS_getrandom (Linux standard)
            {
                uint8_t *buf = (uint8_t *)a1;
                uint64_t len = a2;
                if (len != 0 && !user_prepare_write(a1, len)) return (uint64_t)-14; /* -EFAULT */
                uint64_t val = 0;
                for (uint64_t i = 0; i < len; i++) {
                    if ((i % 8) == 0) {
                        uint32_t lo, hi;
                        __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
                        val = ((uint64_t)hi << 32) | lo;
                    }
                    buf[i] = (uint8_t)(val >> ((i % 8) * 8));
                }
                return len;
            }

        case 28: // SYS_madvise (stub - returning 0 is always safe)
            return 0;

        case 35: // SYS_nanosleep (Linux standard)
            {
                struct timespec {
                    int64_t tv_sec;
                    int64_t tv_nsec;
                };
                struct timespec *req = (struct timespec *)a1;
                if (req) {
                    if (!user_check_read(a1, sizeof(*req))) return (uint64_t)-14; /* -EFAULT */
                    extern uint64_t timer_get_ticks(void);
                    extern void sched_yield(void);
                    uint64_t ticks_to_sleep = (req->tv_sec * 100) + (req->tv_nsec / 10000000);
                    if (ticks_to_sleep == 0 && req->tv_nsec > 0) {
                        ticks_to_sleep = 1;
                    }
                    uint64_t start_ticks = timer_get_ticks();
                    uint64_t end_ticks = start_ticks + ticks_to_sleep;
                    while (timer_get_ticks() < end_ticks) {
                        sched_yield();
                    }
                }
                return 0;
            }

        case 334: // SYS_rseq — restartable sequences (stub)
            return (uint64_t)-38; /* -ENOSYS */
            
        case 89: // SYS_readlink
            {
                char path[MAX_PATH];
                char *buf = (char *)a2;
                uint64_t bufsiz = a3;
                if (!a1 || !buf) return (uint64_t)-14;
                if (strncpy_from_user(path, (const void *)a1, sizeof(path)) < 0) return (uint64_t)-14;

                if (str_compare(path, "/sys/dev/char/226:0/device/subsystem") == 0 ||
                    str_compare(path, "/sys/dev/char/226:128/device/subsystem") == 0) {
                    const char *target = "../../../../../../bus/pci";
                    uint64_t len = 0;
                    while (target[len]) len++;
                    if (len > bufsiz) len = bufsiz;
                    if (!user_prepare_write(a2, len)) return (uint64_t)-14;
                    for (uint64_t i = 0; i < len; i++) {
                        buf[i] = target[i];
                    }
                    return len;
                }
                if (str_compare(path, "/sys/dev/char/226:0/device") == 0 ||
                    str_compare(path, "/sys/dev/char/226:128/device") == 0) {
                    const char *target = "../../../devices/pci0000:00/0000:00:02.0";
                    uint64_t len = 0;
                    while (target[len]) len++;
                    if (len > bufsiz) len = bufsiz;
                    if (!user_prepare_write(a2, len)) return (uint64_t)-14;
                    for (uint64_t i = 0; i < len; i++) {
                        buf[i] = target[i];
                    }
                    return len;
                }
                if (str_compare(path, "/sys/dev/char/226:0/device/driver") == 0 ||
                    str_compare(path, "/sys/dev/char/226:128/device/driver") == 0 ||
                    str_compare(path, "/sys/devices/pci0000:00/0000:00:02.0/driver") == 0) {
                    const char *target = "../../../bus/pci/drivers/virtio_gpu";
                    uint64_t len = 0;
                    while (target[len]) len++;
                    if (len > bufsiz) len = bufsiz;
                    if (!user_prepare_write(a2, len)) return (uint64_t)-14;
                    for (uint64_t i = 0; i < len; i++) {
                        buf[i] = target[i];
                    }
                    return len;
                }
                return (uint64_t)-2; // ENOENT
            }

        case 267: // SYS_readlinkat
            {
                // int dirfd = a1
                char path[MAX_PATH];
                char *buf = (char *)a3;
                uint64_t bufsiz = a4;
                if (!a2 || !buf) return (uint64_t)-14;
                if (strncpy_from_user(path, (const void *)a2, sizeof(path)) < 0) return (uint64_t)-14;

                if (str_compare(path, "/sys/dev/char/226:0/device/subsystem") == 0 ||
                    str_compare(path, "/sys/dev/char/226:128/device/subsystem") == 0) {
                    const char *target = "../../../../../../bus/pci";
                    uint64_t len = 0;
                    while (target[len]) len++;
                    if (len > bufsiz) len = bufsiz;
                    if (!user_prepare_write(a3, len)) return (uint64_t)-14;
                    for (uint64_t i = 0; i < len; i++) {
                        buf[i] = target[i];
                    }
                    return len;
                }
                if (str_compare(path, "/sys/dev/char/226:0/device") == 0 ||
                    str_compare(path, "/sys/dev/char/226:128/device") == 0) {
                    const char *target = "../../../devices/pci0000:00/0000:00:02.0";
                    uint64_t len = 0;
                    while (target[len]) len++;
                    if (len > bufsiz) len = bufsiz;
                    if (!user_prepare_write(a3, len)) return (uint64_t)-14;
                    for (uint64_t i = 0; i < len; i++) {
                        buf[i] = target[i];
                    }
                    return len;
                }
                if (str_compare(path, "/sys/dev/char/226:0/device/driver") == 0 ||
                    str_compare(path, "/sys/dev/char/226:128/device/driver") == 0 ||
                    str_compare(path, "/sys/devices/pci0000:00/0000:00:02.0/driver") == 0) {
                    const char *target = "../../../bus/pci/drivers/virtio_gpu";
                    uint64_t len = 0;
                    while (target[len]) len++;
                    if (len > bufsiz) len = bufsiz;
                    if (!user_prepare_write(a3, len)) return (uint64_t)-14;
                    for (uint64_t i = 0; i < len; i++) {
                        buf[i] = target[i];
                    }
                    return len;
                }
                return (uint64_t)-2; // ENOENT
            }

        default:
            {
                char num_str[32];
                uint_to_hex(num, num_str);
                serial_write_string("Syscall: Unknown syscall number called: ");
                serial_write_string(num_str);
                serial_write_string("\r\n");
            }
            return (uint64_t)-38; /* -ENOSYS instead of -1 for proper error reporting */
    }
}

void syscall_init(void) {
    serial_write_string("Syscalls: Enabling System Call Extensions...\r\n");

    // 1. Enable SCE (System Call Enable) bit in EFER MSR
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= 1; // Set SCE (bit 0)
    wrmsr(MSR_EFER, efer);

    // 2. Setup STAR MSR (Segment selectors)
    // Kernel CS = 0x08, User CS/SS base offset = 0x13
    uint64_t star = ((uint64_t)0x13 << 48) | ((uint64_t)0x08 << 32);
    wrmsr(MSR_STAR, star);

    // 3. Setup LSTAR MSR (Assembly entry point address)
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    // 4. Setup SFMASK MSR (Disable Interrupts and Clear Trap Flag on entry)
    // Mask IF (0x200), TF (0x100)
    wrmsr(MSR_SFMASK, 0x300);

    serial_write_string("Syscalls: Enabled successfully.\r\n");
}
