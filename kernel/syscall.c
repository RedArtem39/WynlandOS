#include <wynland/types.h>
#include <wynland/sched.h>
#include <wynland/vfs.h>
#include <wynland/auth.h>
#include <wynland/random.h>
#include <wynland/hda.h>
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
#include <wynland/virtgpu_drm.h>
#include <wynland/mouse.h>
#include <wynland/kfile.h>
#include <wynland/unix_socket.h>

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

/* What open()/openat() record for a new fd: the access mode, O_APPEND and
   O_NONBLOCK (F_GETFL reports them; glibc's fdopen() checks the access
   mode against its "w"/"r") and O_CLOEXEC. */
static void fd_set_open_flags(uint32_t *fd_flags, uint32_t *fd_oflags, int fd, int linux_flags)
{
    fd_oflags[fd] = (uint32_t)linux_flags & (3u | 02000u /* O_APPEND */ | 04000u /* O_NONBLOCK */);
    fd_flags[fd]  = (linux_flags & 02000000) ? FD_CLOEXEC : 0;
}
#define F_SETFL   4

/* Linux O_flags for fcntl */
#define LINUX_O_RDONLY    0
#define LINUX_O_WRONLY    1
#define LINUX_O_RDWR      2
#define LINUX_O_NONBLOCK  04000

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
/* argv/envp a program gets: these were 8 and 16, and everything past
   them was dropped without a word */
#define MAX_SPAWN_ARGV 64
#define ARG_STR_MAX    4096   /* one argv/envp string */
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
    /* Open fds on each end (pipe() only; eventfd leaves is_pipe 0). With no
       writer left, an empty pipe reads as EOF; with no reader, a write is
       EPIPE + SIGPIPE. kfile_get()/kfile_close() keep the counts. */
    bool     is_pipe;
    uint32_t readers;
    uint32_t writers;
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

/* A signal that will be delivered (unblocked, or SIGKILL) is pending on
   the current thread: a sleep must end with -EINTR. */
static bool sleep_interrupted(void) {
    Thread *t = sched_current();
    return t && (t->sig_pending & (~t->sig_mask | (1ULL << 9)));
}

/* -EINTR for an interrupted sleep; the remaining time goes to the user's
   rem timespec when one was given. */
static uint64_t sleep_eintr(uint64_t rem_uaddr, uint64_t end_ms) {
    extern uint64_t timer_get_ms(void);
    if (rem_uaddr) {
        uint64_t now = timer_get_ms();
        uint64_t left = end_ms > now ? end_ms - now : 0;
        struct { int64_t tv_sec, tv_nsec; } r = { (int64_t)(left / 1000), (int64_t)(left % 1000) * 1000000 };
        copy_to_user((void *)rem_uaddr, &r, sizeof(r));
    }
    return (uint64_t)-4; /* -EINTR */
}

static uint32_t pipe_write(KPipe *pipe, const void *buf, uint32_t size) {
    const uint8_t *src = (const uint8_t *)buf;
    /* POSIX PIPE_BUF atomicity: a write of <= PIPE_BUF_SIZE bytes waits
       until it fits WHOLE. Writing whatever fits would split fixed-size
       records (Zerp's 20-byte messages don't divide 4096) and desync
       every reader that parses them. Larger writes may still be split. */
    uint32_t need = size <= PIPE_BUF_SIZE ? size : 1;
    while (PIPE_BUF_SIZE - pipe->count < need) {
        if (pipe->is_pipe && pipe->readers == 0) return 0; /* caller: EPIPE */
        waitqueue_wait(&pipe->write_wq, timer_get_ticks() + PIPE_WAIT_RETRY_TICKS);
    }
    uint32_t written = 0;
    while (written < size && pipe->count < PIPE_BUF_SIZE) {
        pipe->buffer[pipe->tail] = src[written];
        pipe->tail = (pipe->tail + 1) % PIPE_BUF_SIZE;
        pipe->count++;
        written++;
    }
    if (written > 0) {
        waitqueue_wake_all(&pipe->read_wq);
        waitqueue_wake_all(&g_poll_any_wq);
    }
    return written;
}

static uint32_t pipe_read(KPipe *pipe, void *buf, uint32_t size) {
    uint8_t *dst = (uint8_t *)buf;
    while (pipe->count == 0) {
        if (pipe->is_pipe && pipe->writers == 0) return 0; /* EOF */
        waitqueue_wait(&pipe->read_wq, timer_get_ticks() + PIPE_WAIT_RETRY_TICKS);
    }
    uint32_t read_bytes = 0;
    while (read_bytes < size && pipe->count > 0) {
        dst[read_bytes] = pipe->buffer[pipe->head];
        pipe->head = (pipe->head + 1) % PIPE_BUF_SIZE;
        pipe->count--;
        read_bytes++;
    }
    if (read_bytes > 0) {
        waitqueue_wake_all(&pipe->write_wq);
        waitqueue_wake_all(&g_poll_any_wq);
    }
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

/* ---- process lifecycle helpers (wait4/waitid/kill/exit_group) ---- */

/* Set by the clone() process path right before it runs the fork code for a
   vfork (CLONE_VM|CLONE_VFORK); consumed there. Syscalls run with
   interrupts off on one CPU, so nothing can observe it in between. */
static bool g_fork_share_mm = false;

#define LNX_ECHILD 10
#define LNX_EINTR  4
#define LNX_ESRCH  3
#define LNX_EPERM  1
#define LNX_EINVAL 22

/* Does child `c` of `caller` match a wait4()-style pid selector? */
static bool wait_matches(Process *caller, Process *c, int64_t pid)
{
    if (c->ppid != caller->pid || c->reaped || c == caller) return false;
    if (pid > 0)   return c->pid == (uint64_t)pid;
    if (pid == -1) return true;
    if (pid == 0)  return c->pgid == caller->pgid;
    return c->pgid == (uint64_t)(-pid);
}

/* Find (and unless !consume, reap) an exited child matching `pid`.
   Returns its pid, 0 for WNOHANG-and-none-yet, or -errno. Sleeps on the
   caller's child_wq otherwise; a deliverable signal interrupts (-EINTR). */
static int64_t do_wait(int64_t pid, bool nohang, bool consume, Process **found)
{
    extern uint64_t timer_get_ms(void);
    Process *caller = sched_current()->proc;
    for (;;) {
        bool have_child = false;
        for (Process *c = process_list_head(); c; c = c->next) {
            if (!wait_matches(caller, c, pid)) continue;
            have_child = true;
            if (c->exited) {
                if (consume) c->reaped = true;
                *found = c;
                return (int64_t)c->pid;
            }
        }
        if (!have_child) return -LNX_ECHILD;
        if (nohang) return 0;
        Thread *t = sched_current();
        if (t->sig_pending & ~t->sig_mask) return -LNX_EINTR;
        /* short slices: a wake can race the scan above */
        waitqueue_wait_ms(&caller->child_wq, timer_get_ms() + 50);
    }
}

/* May the caller signal `p`? root, or same uid (the real-uid rule). */
static bool may_signal(Process *p)
{
    Process *me = sched_current()->proc;
    return me->uid == 0 || me->uid == p->uid;
}

/* Deliver `sig` to process `p` (its main thread); sig 0 = existence
   check only. A sleeping target is woken so the signal is seen promptly. */
static int64_t signal_process(Process *p, int sig)
{
    if (p->pid == 0) return -LNX_EPERM;
    if (!may_signal(p)) return -LNX_EPERM;
    if (sig == 0 || p->exited) return 0; /* zombies exist until reaped */
    Thread *t = p->main_thread;
    if (!t || t->state == THREAD_STATE_TERMINATED) return 0;
    signal_raise_thread(t->id, p->pid, sig);
    if (t->state == THREAD_STATE_BLOCKED) sched_unblock(t, -LNX_EINTR);
    return 0;
}

/* Unmap one user page and drop what the mapping owned: a private frame is
   freed; a shared one (PAGE_SHARED_MAP: fb0, SHM, DRM BO) is left to its
   owner, except that a mapping holding its own reference
   (PAGE_SHARED_REF: memfd) releases it. */
static void user_unmap_page(PageTable *pml4, uint64_t va)
{
    uint64_t pte = vmm_get_pte(pml4, va);
    if (!pte) return;
    /* only the process's own pages: the kernel has mappings in the user
       half too (the virtio-gpu queues), shared with every process */
    if (!(pte & PAGE_USER)) return;
    vmm_unmap_page(pml4, va);
    if (!(pte & PAGE_SHARED_MAP) || (pte & PAGE_SHARED_REF))
        pmm_free_page((void *)(uintptr_t)(pte & PAGE_ADDR_MASK));
}

/* The part of the address space a process may map, unmap and protect
   itself. PML4 slot 0 (below 512 GB) is the kernel/RAM identity map,
   shared by pointer with every process: MAP_FIXED or munmap there used to
   replace or free kernel pages. Above 2^47 is the canonical hole. */
#define USER_MAP_MIN     0x0000008000000000ULL
#define USER_MAP_MAX     0x0000800000000000ULL
#define USER_MAP_LEN_MAX (64ULL << 30)   /* one mapping, at most */

static bool user_map_range_ok(uint64_t addr, uint64_t len)
{
    if (addr & (PAGE_SIZE - 1)) return false;
    if (len == 0 || len > USER_MAP_MAX) return false;
    uint64_t end = addr + len;
    return end > addr && addr >= USER_MAP_MIN && end <= USER_MAP_MAX;
}

/* Unmap every page in [addr, end) the process has, skipping the empty
   parts of the page tables whole. Caller checked the range. */
static void user_unmap_range(PageTable *pml4, uint64_t addr, uint64_t end)
{
    for (uint64_t a = vmm_next_mapped(pml4, addr, end); a < end;
         a = vmm_next_mapped(pml4, a + PAGE_SIZE, end))
        user_unmap_page(pml4, a);
}

/* Does [addr, end) contain a page that isn't the process's own? The
   kernel keeps mappings in the user half too (the virtio-gpu queues). */
static bool range_has_kernel_pages(PageTable *pml4, uint64_t addr, uint64_t end)
{
    for (uint64_t a = vmm_next_mapped(pml4, addr, end); a < end;
         a = vmm_next_mapped(pml4, a + PAGE_SIZE, end))
        if (!(vmm_get_pte(pml4, a) & PAGE_USER)) return true;
    return false;
}

/* mmap() address slots, one PML4-sized region (1 TB here) per kind of
   mapping; addresses come from a per-process bump pointer. 0 when the
   slot is exhausted -- the caller fails with -ENOMEM instead of running
   into the next slot. */
enum { MMAP_SLOT_FB, MMAP_SLOT_SHM, MMAP_SLOT_ANON, MMAP_SLOT_MEMFD, MMAP_SLOT_DRM, MMAP_SLOT_DMABUF };
static const uint64_t mmap_slot_base[6] = {
    0x610000000000ULL, 0x620000000000ULL, 0x630000000000ULL,
    0x640000000000ULL, 0x650000000000ULL, 0x660000000000ULL,
};
#define MMAP_SLOT_SIZE 0x010000000000ULL

static uint64_t mmap_bump(int slot, uint64_t size)
{
    Process *p = sched_current()->proc;
    uint64_t base = mmap_slot_base[slot];
    uint64_t cur = p->mmap_next[slot] ? p->mmap_next[slot] : base;
    if (size > MMAP_SLOT_SIZE || cur - base > MMAP_SLOT_SIZE - size) return 0;
    p->mmap_next[slot] = cur + size;
    return cur;
}

/* Per-call tracing of open/openat/mkdir/pipe/eventfd/ioctl on the serial
   console. Off unless built with SYSCALL_TRACE=1: every line is written
   with interrupts off, a byte at a time, and Qt opens thousands of files
   at startup -- the trace alone stalled the system for seconds. */
#ifndef SYSCALL_TRACE
#define SYSCALL_TRACE 0
#endif
static inline void trace_str(const char *s)
{
    if (SYSCALL_TRACE) serial_write_string(s);
}

static int get_free_fd(VfsFile **fd_table) {
    for (int i = 3; i < MAX_OPEN_FILES; i++) {
        if (fd_table[i] == NULL) {
            return i;
        }
    }
    return -1;
}

/* dma-buf (PRIME) fds for drivers/video/virtgpu_drm.c */
int drm_install_prime_fd(uint32_t idx, bool cloexec)
{
    Process *p = sched_current()->proc;
    int fd = get_free_fd(p->fd_table);
    if (fd < 0) return -24; /* -EMFILE */
    VfsFile *f = (VfsFile *)kmalloc(sizeof(VfsFile));
    if (!f) return -12;
    memset(f, 0, sizeof(VfsFile));
    str_copy(f->node.name, "dmabuf");
    f->node.first_cluster = DRM_PRIME_FD;
    f->current_cluster = idx;
    p->fd_table[fd] = f;
    p->fd_flags[fd] = cloexec ? 1 : 0;
    p->fd_oflags[fd] = LINUX_O_RDWR;
    return fd;
}

uint32_t drm_prime_fd_bo(int fd)
{
    Process *p = sched_current()->proc;
    if (fd < 0 || fd >= MAX_OPEN_FILES || !p->fd_table[fd]) return 0;
    if (p->fd_table[fd]->node.first_cluster != DRM_PRIME_FD) return 0;
    return p->fd_table[fd]->current_cluster;
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
    for (int i = 0; count < max_entries && i < 4096; i++) {
        if (!user_check_read((uint64_t)(uintptr_t)(uarray + i), sizeof(char *))) break;
        const char *ustr = uarray[i];
        if (!ustr) break;
        char *buf = (char *)kmalloc(ARG_STR_MAX);
        if (!buf) break;
        /* a string too long (or unreadable) is left out and the rest still
           counts -- one long variable used to cut the whole list there */
        if (strncpy_from_user(buf, ustr, ARG_STR_MAX) < 0) { kfree(buf); continue; }
        kbufs[count] = buf;
        kptrs[count] = buf;
        count++;
    }
    kptrs[count] = NULL;
    return count;
}

/* owner and times of a path's inode */
static void stat_fill_owner(struct linux_stat *st, const VfsStat *vst) {
    st->st_uid = vst->uid;
    st->st_atime_sec = vst->atime;
    st->st_mtime_sec = vst->mtime;
    st->st_ctime_sec = vst->ctime;
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

    /* real files: the inode's own owner, permission bits and time
       (device sentinels below override) */
    const uint32_t perm = file->node.first_cluster < 0xFFFFFF00u && file->node.mode ? file->node.mode : 0;
    if (file->node.is_dir) {
        st->st_mode = S_IFDIR | (perm ? perm : 0755);
        st->st_size = 0;
    } else {
        st->st_mode = S_IFREG | (perm ? perm : 0644);
        st->st_size = (int64_t)file->node.size;
    }
    if (perm) {
        st->st_uid = file->node.uid;
        st->st_mtime_sec = st->st_ctime_sec = st->st_atime_sec = file->node.mtime;
    }
    st->st_blocks = (st->st_size + 511) / 512;

    if (file->node.first_cluster == USOCK_FD) {
        st->st_mode = 0140000 /* S_IFSOCK */ | 0777;
        st->st_size = 0;
        st->st_ino  = 0x10000000ULL + file->current_cluster;
    } else if (file->node.first_cluster == MEMFD_FD) {
        st->st_mode = S_IFREG | 0600;
        st->st_size = (int64_t)memfd_size((int)file->current_cluster);
        st->st_ino  = 0x20000000ULL + file->current_cluster;
    } else if (file->node.first_cluster == TIMERFD_FD) {
        st->st_mode = 0600; /* anon inode */
        st->st_size = 0;
        st->st_ino  = 0x30000000ULL + file->current_cluster;
    }
    st->st_blocks = (st->st_size + 511) / 512;

    if (file->node.first_cluster == 0xFFFFFFF0) {
        st->st_mode = S_IFCHR | 0666;
        st->st_rdev = ((uint64_t)29 << 8) | 0; /* major 29, minor 0 */
    } else if (file->node.first_cluster == DRM_DEV_RENDER) {
        st->st_mode = S_IFCHR | 0666;
        st->st_rdev = ((uint64_t)226 << 8) | 128;
    } else if (file->node.first_cluster == DRM_DEV_CARD) {
        st->st_mode = S_IFCHR | 0666;
        st->st_rdev = ((uint64_t)226 << 8) | 0;
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
extern uint64_t timer_get_ms(void);

/* Poll `nfds` KERNEL-owned wl_pollfd entries for readiness, genuinely
   blocking (not spinning) until at least one is ready or `timeout_ms`
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
/* A recv() on this TCP connection would not block: data buffered, or the
   peer closed/reset (recv then returns 0/-1 at once). */
static bool tcp_conn_readable(TcpConnection *conn) {
    return conn->rx_len > 0 || conn->fin_received || conn->reset_received ||
           conn->state == TCP_STATE_CLOSED;
}


/* /proc/self/maps, generated from the process's VMA list into a memfd
   (read with the ordinary memfd read path). glibc's pthread_getattr_np()
   finds the main thread's stack in it -- Qt's QML engine sizes its JS
   stack limit from that and refused to run any JS without it. */
static int proc_maps_memfd(Process *pr) {
    int mi = memfd_new("maps");
    if (mi < 0) return mi;
    char line[96];
    uint64_t off = 0;
    for (VMA *v = pr->vma_list; v; v = v->next) {
        if (v->flags & VMA_GUARD) continue;
        int k = 0;
        for (int sh = 44; sh >= 0; sh -= 4) line[k++] = "0123456789abcdef"[(v->start >> sh) & 0xF];
        line[k++] = '-';
        for (int sh = 44; sh >= 0; sh -= 4) line[k++] = "0123456789abcdef"[(v->end >> sh) & 0xF];
        line[k++] = ' ';
        line[k++] = (v->prot & VMA_PROT_READ) ? 'r' : '-';
        line[k++] = (v->prot & VMA_PROT_WRITE) ? 'w' : '-';
        line[k++] = (v->prot & VMA_PROT_EXEC) ? 'x' : '-';
        line[k++] = 'p';
        const char *rest = " 00000000 00:00 0";
        while (*rest) line[k++] = *rest++;
        if (v->start == 0x600000000000ULL) { /* elf.c's main thread stack */
            const char *st = "                          [stack]";
            while (*st) line[k++] = *st++;
        }
        line[k++] = '\n';
        memfd_pwrite(mi, off, line, (uint64_t)k);
        off += (uint64_t)k;
    }
    return mi;
}

/* readlink() for paths no mock above claimed: /proc/self/exe is the running
   image; any other existing path is not a symlink (ext2 here has none) --
   -EINVAL, as on Linux. It used to be -ENOENT for everything, so glibc's
   realpath() decided every path was missing (Qt then found no QML
   modules, no plugins). */
static uint64_t readlink_fallback(const char *path, uint64_t ubuf, uint64_t bufsiz) {
    if (str_compare(path, "/proc/self/exe") == 0) {
        Process *pr = sched_current()->proc;
        uint64_t len = 0;
        while (pr->exe_path[len]) len++;
        if (!len) return (uint64_t)-2;
        if (len > bufsiz) len = bufsiz;
        if (copy_to_user((void *)ubuf, pr->exe_path, len) != 0) return (uint64_t)-14;
        return len;
    }
    VfsStat vst;
    if (vfs_stat(path, &vst)) return (uint64_t)-22; /* -EINVAL: not a symlink */
    return (uint64_t)-2;                             /* -ENOENT */
}

static int do_poll(VfsFile **fd_table, struct wl_pollfd *fds, uint64_t nfds, int64_t timeout_ms) {
    /* Deadline and sleeps both on the 1 kHz clock: a timeout expires
       neither early nor up to a 100 Hz tick late. */
    uint64_t deadline_ms = (timeout_ms < 0) ? SCHED_NO_DEADLINE : timer_get_ms() + (uint64_t)timeout_ms;

    for (;;) {
        if (fds) {
            for (uint64_t i = 0; i < nfds; i++) fds[i].revents = 0;
        }

        int ready = 0;
        WaitQueue *single_wq = NULL; /* only meaningful when nfds == 1 and that one fd isn't ready yet */
        uint64_t timer_wake_ms = 0;  /* earliest watched timerfd expiry; nothing wakes us for it */

        for (uint64_t i = 0; i < nfds; i++) {
            int fd = fds[i].fd;
            if (fd < 0 || fd >= MAX_OPEN_FILES || fd_table[fd] == NULL) {
                if (fds) fds[i].revents = 0x0020; // POLLNVAL
                ready++;
                continue;
            }
            VfsFile *file = fd_table[fd];

            if (IS_DRM_DEV(file->node.first_cluster)) {
                /* readable = a page-flip event is waiting */
                if (drm_poll_ready(file->current_cluster)) {
                    if (fds && (fds[i].events & 0x0001)) { fds[i].revents |= 0x0001; ready++; }
                } else {
                    single_wq = drm_event_wq(file->current_cluster);
                }
                if (fds && (fds[i].events & 0x0004)) { fds[i].revents |= 0x0004; ready++; }
                continue;
            }

            if (file->node.first_cluster == 0xFFFFFFFA || // Pipe Read
                file->node.first_cluster == 0xFFFFFFFC)   // Eventfd
            {
                int p_idx = file->current_cluster;
                if (p_idx >= 0 && p_idx < MAX_PIPES && g_pipes[p_idx] != NULL) {
                    KPipe *p = g_pipes[p_idx];
                    if (p->count == 0 && p->is_pipe && p->writers == 0) {
                        if (fds) fds[i].revents |= 0x0010 | (fds[i].events & 0x0001); // POLLHUP (+POLLIN: read gives EOF)
                        ready++;
                    } else if (p->count > 0) {
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
                uint32_t pw = file->current_cluster;
                if (pw < MAX_PIPES && g_pipes[pw] && g_pipes[pw]->is_pipe && g_pipes[pw]->readers == 0) {
                    if (fds) fds[i].revents |= 0x0008; // POLLERR
                    ready++;
                } else if (fds && (fds[i].events & 0x0004)) { // POLLOUT
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
            else if (file->node.first_cluster == USOCK_FD || file->node.first_cluster == TIMERFD_FD) {
                WaitQueue *wq = NULL;
                uint32_t want = fds ? (uint32_t)(uint16_t)fds[i].events : 0;
                uint32_t rev;
                if (file->node.first_cluster == USOCK_FD) {
                    rev = usock_poll((int)file->current_cluster, want, &wq);
                } else {
                    uint64_t w = 0;
                    rev = timerfd_poll((int)file->current_cluster, &wq, &w) & (want | UPOLLERR | UPOLLHUP);
                    if (w && (timer_wake_ms == 0 || w < timer_wake_ms)) timer_wake_ms = w;
                }
                if (rev) {
                    if (fds) fds[i].revents |= (short)rev;
                    ready++;
                } else {
                    single_wq = wq;
                }
            }
            else if (file->node.first_cluster == SOCK_FD_TCP) {
                /* Real readiness: "always readable" made a non-blocking
                   client (curl: poll(socket, eventfd) then read) fall into
                   a blocking recv once the server went quiet -- forever. */
                TcpConnection *conn = (file->current_cluster == TCP_FD_NOT_CONNECTED) ? NULL
                                    : tcp_get_connection((int)file->current_cluster);
                short ev = fds ? fds[i].events : 0;
                short rev = 0;
                if (!conn) {
                    if (file->current_cluster != TCP_FD_NOT_CONNECTED) rev |= 0x0010; /* POLLHUP */
                    else rev |= ev & 0x0004;                                          /* unconnected: writable (connect) */
                } else {
                    if (tcp_conn_readable(conn)) rev |= ev & 0x0001;
                    if (conn->fin_received || conn->reset_received || conn->state == TCP_STATE_CLOSED) rev |= 0x0010;
                    if (conn->state == TCP_STATE_ESTABLISHED) rev |= ev & 0x0004;
                }
                if (rev) {
                    if (fds) fds[i].revents |= rev;
                    ready++;
                } else if (conn) {
                    single_wq = &conn->rx_wq;
                }
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
        if (timeout_ms == 0) return 0; // caller asked for an immediate check only

        uint64_t now_ms = timer_get_ms();
        if (deadline_ms != SCHED_NO_DEADLINE && now_ms >= deadline_ms) return 0; // real timeout

        /* A watched timerfd can't wake us itself: never sleep past it. */
        uint64_t sleep_until = deadline_ms;
        if (timer_wake_ms && (sleep_until == SCHED_NO_DEADLINE || timer_wake_ms < sleep_until))
            sleep_until = timer_wake_ms;

        /* Genuinely sleep instead of spinning. Single watched fd
           with a real wait queue -> wake instantly on data via
           waitqueue_wake_all(). Several fds: sleep on g_poll_any_wq,
           which every pipe/socket/timerfd state change also wakes, so
           those still wake us immediately; the ~50ms cap remains for
           sources that don't signal it yet (TCP, PTYs). */
        if (single_wq && nfds == 1) {
            waitqueue_wait_ms(single_wq, sleep_until);
        } else {
            uint64_t nap_deadline = now_ms + 50;
            if (sleep_until != SCHED_NO_DEADLINE && nap_deadline > sleep_until) nap_deadline = sleep_until;
            waitqueue_wait_ms(&g_poll_any_wq, nap_deadline);
        }
    }
}

/* See the declaration in include/wynland/process.h for the full contract.
   Lives here (not process.c) because it needs this file's pipe/PTY/socket/
   SHM tables to give each fd type the same real cleanup a live close()
   would -- mirrors SYS_close's own per-sentinel handling below, but can't
   just call it: SYS_close operates on sched_current()'s process via
   syscall_dispatcher()'s local fd_table/fd_flags/fd_oflags shadows (top of
   this function), and a process being torn down here is never the
   currently-running one (see kernel/sched.c's cleanup loop, and
   process_spawn()'s own failure path, kernel/process.c). */
/* A new wrapper copied from another now references the same object
   (include/wynland/kfile.h). */
void kfile_get(VfsFile *f) {
    uint32_t fc = f->node.first_cluster;
    if (fc == USOCK_FD) usock_ref((int)f->current_cluster);
    else if (fc == MEMFD_FD) memfd_ref((int)f->current_cluster);
    else if (fc == TIMERFD_FD) timerfd_ref((int)f->current_cluster);
    else if (fc == DRM_PRIME_FD) drm_prime_get(f->current_cluster);
    else if (IS_DRM_DEV(fc)) drm_client_ref(f->current_cluster);
    else if ((fc == 0xFFFFFFFA || fc == 0xFFFFFFFB) && f->current_cluster < MAX_PIPES &&
             g_pipes[f->current_cluster]) {
        KPipe *kp = g_pipes[f->current_cluster];
        if (fc == 0xFFFFFFFA) kp->readers++; else kp->writers++;
    }
    else if (fc == 0xFFFFFFFD) { /* SHM segment: keep close()'s decrement balanced */
        int seg_idx = (int)f->current_cluster;
        if (seg_idx >= 0 && seg_idx < MAX_SHM_SEGMENTS) g_shm_segments[seg_idx].refcount++;
    }
}

VfsFile *kfile_dup(const VfsFile *f) {
    VfsFile *copy = (VfsFile *)kmalloc(sizeof(VfsFile));
    if (!copy) return NULL;
    *copy = *f;
    kfile_get(copy);
    return copy;
}

/* Drop one fd's wrapper: the per-type cleanup SYS_close, process teardown
   and discarded SCM_RIGHTS fds all share. Frees f. */
void kfile_close(VfsFile *f) {
    uint32_t fc = f->node.first_cluster;
    if (fc == SOCK_FD_TCP) {
        if (f->current_cluster != TCP_FD_NOT_CONNECTED) {
            TcpConnection *conn = tcp_get_connection((int)f->current_cluster);
            if (conn) tcp_close(conn);
        }
        kfree(f);
    } else if (fc == SOCK_FD_UDP) {
        udp_socket_close((int)f->current_cluster);
        kfree(f);
    } else if (fc == 0xFFFFFFFD) { /* SHM segment */
        int seg_idx = (int)f->current_cluster;
        if (seg_idx >= 0 && seg_idx < MAX_SHM_SEGMENTS && g_shm_segments[seg_idx].refcount > 0) {
            g_shm_segments[seg_idx].refcount--;
        }
        kfree(f);
    } else if (fc == USOCK_FD) {
        int idx = (int)f->current_cluster;
        kfree(f);             /* first: teardown may recurse into in-flight fds */
        usock_unref(idx);
    } else if (fc == MEMFD_FD) {
        memfd_unref((int)f->current_cluster);
        kfree(f);
    } else if (fc == TIMERFD_FD) {
        timerfd_unref((int)f->current_cluster);
        kfree(f);
    } else if (fc == DRM_PRIME_FD) {
        drm_prime_put(f->current_cluster); /* never sleeps: BO freed later */
        kfree(f);
    } else if ((fc == 0xFFFFFFFA || fc == 0xFFFFFFFB) && f->current_cluster < MAX_PIPES &&
               g_pipes[f->current_cluster] && g_pipes[f->current_cluster]->is_pipe) {
        uint32_t idx = f->current_cluster;
        KPipe *kp = g_pipes[idx];
        if (fc == 0xFFFFFFFA) { if (kp->readers) kp->readers--; }
        else                  { if (kp->writers) kp->writers--; }
        kfree(f);
        if (kp->readers == 0 && kp->writers == 0) {
            g_pipes[idx] = NULL;
            kfree(kp);
        } else {
            /* last writer gone: blocked readers see EOF; last reader gone:
               blocked writers see EPIPE; poll() re-evaluates */
            waitqueue_wake_all(&kp->read_wq);
            waitqueue_wake_all(&kp->write_wq);
            waitqueue_wake_all(&g_poll_any_wq);
        }
    } else if (fc == 0xFFFFFFFA || fc == 0xFFFFFFFB || fc == 0xFFFFFFFC ||
               fc == PTY_FD_MASTER || fc == PTY_FD_SLAVE) {
        /* Pipes/eventfd/PTYs: only the small per-fd VfsFile wrapper is
           freed, never the underlying KPipe/Pty object -- a real refcount
           for those is still the accepted gap described at SYS_close. */
        kfree(f);
    } else {
        vfs_close(f); /* frees f itself */
    }
}

static bool is_kobj_fd(const VfsFile *f) {
    uint32_t fc = f->node.first_cluster;
    return fc == USOCK_FD || fc == MEMFD_FD || fc == TIMERFD_FD;
}

/* read()/write()/readv()/writev() on sockets, memfds and timerfds. The
   iovecs hold user pointers the caller already validated (read) or
   prepared (write). A socket gets the whole vector as ONE send/recv, so
   writev() on a seqpacket socket is one message. fds that arrive with a
   plain read() are closed, as on Linux. */
static int64_t kobj_readv(VfsFile *f, uint32_t oflags, const UIoVecW *iov, int iovcnt) {
    uint32_t fc = f->node.first_cluster;
    bool nonblock = (oflags & LINUX_O_NONBLOCK) != 0;
    if (fc == USOCK_FD) {
        VfsFile *fds[USOCK_MAX_MSG_FDS];
        uint32_t nfds = 0;
        int mflags = 0;
        int64_t r = usock_recv((int)f->current_cluster, iov, iovcnt, 0, nonblock,
                               fds, USOCK_MAX_MSG_FDS, &nfds, &mflags, NULL, NULL);
        for (uint32_t i = 0; i < nfds; i++) kfile_close(fds[i]);
        return r;
    }
    if (fc == MEMFD_FD) {
        int64_t total = 0;
        for (int i = 0; i < iovcnt; i++) {
            int64_t r = memfd_pread((int)f->current_cluster, f->offset, iov[i].base, iov[i].len);
            if (r < 0) return total ? total : r;
            f->offset += (uint32_t)r;
            total += r;
            if ((uint64_t)r < iov[i].len) break;
        }
        return total;
    }
    if (fc == TIMERFD_FD) {
        if (iovcnt < 1 || iov[0].len < 8) return -22; /* -EINVAL */
        uint64_t count = 0;
        int64_t r = timerfd_read((int)f->current_cluster, &count, nonblock);
        if (r < 0) return r;
        memcpy(iov[0].base, &count, 8);
        return 8;
    }
    return -9;
}

static int64_t kobj_writev(VfsFile *f, uint32_t oflags, const UIoVecR *iov, int iovcnt) {
    uint32_t fc = f->node.first_cluster;
    bool nonblock = (oflags & LINUX_O_NONBLOCK) != 0;
    if (fc == USOCK_FD) {
        return usock_send((int)f->current_cluster, iov, iovcnt, NULL, 0, 0, nonblock, NULL, 0);
    }
    if (fc == MEMFD_FD) {
        int64_t total = 0;
        for (int i = 0; i < iovcnt; i++) {
            uint64_t off = (oflags & 02000 /* O_APPEND */) ? memfd_size((int)f->current_cluster) : f->offset;
            int64_t r = memfd_pwrite((int)f->current_cluster, off, iov[i].base, iov[i].len);
            if (r < 0) return total ? total : r;
            f->offset = (uint32_t)(off + (uint64_t)r);
            total += r;
        }
        return total;
    }
    if (fc == TIMERFD_FD) return -22; /* -EINVAL */
    return -9;
}

/* Store a u32 into ANOTHER process's user memory (a fork child that hasn't
   run yet). A COW-shared page is split first so the write lands only in
   that process. Used for clone()'s CLONE_CHILD_SETTID on the fork path. */
static bool poke_user_u32(PageTable *pml4, uint64_t va, uint32_t val) {
    if ((va & 3) || !va) return false;
    uint64_t page = va & ~(PAGE_SIZE - 1);
    uint64_t flags = vmm_get_page_flags(pml4, page);
    uint64_t phys = vmm_get_phys(pml4, page);
    if (!(flags & PAGE_PRESENT) || !(flags & PAGE_USER) || !phys) return false;
    phys &= ~(PAGE_SIZE - 1);
    if (flags & PAGE_COW) {
        void *np = pmm_alloc_page();
        if (!np) return false;
        memcpy(np, (void *)(uintptr_t)phys, PAGE_SIZE);
        vmm_map_page(pml4, page, (uint64_t)(uintptr_t)np, PAGE_USER | PAGE_WRITE | PAGE_NX);
        pmm_free_page((void *)(uintptr_t)phys); /* drop this process's share */
        phys = (uint64_t)(uintptr_t)np;
    } else if (!(flags & PAGE_WRITE)) {
        return false;
    }
    *(uint32_t *)(uintptr_t)(phys + (va & (PAGE_SIZE - 1))) = val;
    return true;
}

/* ---- AF_UNIX syscall glue helpers ---- */

#define SOCK_NONBLOCK_FLAG 04000
#define SOCK_CLOEXEC_FLAG  02000000

/* Wrap socket slot `s` in a new fd. On failure the slot's reference is
   dropped and a negative errno returned. */
static int64_t install_usock(VfsFile **fd_table, uint32_t *fd_flags, uint32_t *fd_oflags,
                             int s, uint32_t sflags) {
    int fd = get_free_fd(fd_table);
    VfsFile *f = (fd < 0) ? NULL : (VfsFile *)kmalloc(sizeof(VfsFile));
    if (!f) {
        usock_unref(s);
        return fd < 0 ? -24 /* EMFILE */ : -12 /* ENOMEM */;
    }
    memset(f, 0, sizeof(VfsFile));
    str_copy(f->node.name, "socket");
    f->node.first_cluster = USOCK_FD;
    f->current_cluster = (uint32_t)s;
    fd_table[fd] = f;
    fd_flags[fd]  = (sflags & SOCK_CLOEXEC_FLAG) ? FD_CLOEXEC : 0;
    fd_oflags[fd] = LINUX_O_RDWR | (sflags & SOCK_NONBLOCK_FLAG);
    return fd;
}

static bool fd_is_usock(VfsFile **fd_table, uint64_t fd) {
    return fd < MAX_OPEN_FILES && fd_table[fd] && fd_table[fd]->node.first_cluster == USOCK_FD;
}

/* struct sockaddr_un from user memory -> name bytes (sun_path, abstract
   names keep their leading NUL). */
static int64_t sun_from_user(uint64_t uaddr, uint64_t alen, char *name, uint32_t *nlen) {
    uint8_t buf[2 + 108];
    if (!uaddr) return -14;              /* EFAULT */
    if (alen < 2 || alen > sizeof(buf)) return -22; /* EINVAL */
    if (copy_from_user(buf, (const void *)uaddr, alen) != 0) return -14;
    if ((buf[0] | (buf[1] << 8)) != 1 /* AF_UNIX */) return -97; /* EAFNOSUPPORT */
    *nlen = (uint32_t)alen - 2;
    memcpy(name, buf + 2, *nlen);
    return 0;
}

/* name bytes -> struct sockaddr_un at uaddr, with *ulen (socklen_t) in/out
   semantics: copy at most the caller's size, report the real size. */
static int64_t sun_to_user(uint64_t uaddr, uint64_t ulenp, const char *name, uint32_t nlen) {
    if (!uaddr || !ulenp) return 0;
    uint32_t have;
    if (copy_from_user(&have, (const void *)ulenp, sizeof(have)) != 0) return -14;
    uint8_t buf[2 + 108 + 1];
    memset(buf, 0, sizeof(buf));
    buf[0] = 1; /* AF_UNIX */
    memcpy(buf + 2, name, nlen);
    uint32_t real = 2 + nlen + ((nlen > 0 && name[0] != '\0') ? 1 : 0); /* fs names include their NUL */
    uint32_t n = real < have ? real : have;
    if (n && copy_to_user((void *)uaddr, buf, n) != 0) return -14;
    if (copy_to_user((void *)ulenp, &real, sizeof(real)) != 0) return -14;
    return 0;
}

struct linux_msghdr {
    uint64_t msg_name;
    uint32_t msg_namelen;
    uint32_t pad0;
    uint64_t msg_iov;
    uint64_t msg_iovlen;
    uint64_t msg_control;
    uint64_t msg_controllen;
    int32_t  msg_flags;
    uint32_t pad1;
};
struct linux_cmsghdr { uint64_t cmsg_len; int32_t cmsg_level; int32_t cmsg_type; };
#define CMSG_ALIGN8(n) (((n) + 7) & ~7ULL)
#define SCM_RIGHTS_TYPE 1
#define SOL_SOCKET_LVL  1
#define MSG_CTRL_MAX    4096

void process_teardown(Process *proc) {
    if (!proc || proc->pid == 0) return; /* never tear down the kernel process */

    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        VfsFile *f = proc->fd_table[i];
        if (!f) continue;
        proc->fd_table[i] = NULL;
        if (IS_DRM_DEV(f->node.first_cluster)) {
            /* can't wait for the host from here: deferred to the next ioctl */
            drm_release(f->current_cluster, proc->pid, false);
        }
        kfile_close(f);
    }

    vma_free_list(proc);

    /* Real address-space teardown: walks the actual page tables (never a
       side list -- see vmm_destroy_process_pml4()'s own comment) freeing
       every leaf frame and private page-table page this process owns,
       then the PML4 root itself. Never touches the shared kernel/RAM/
       framebuffer identity map every process's PML4 aliases by pointer. */
    if (proc->pml4 && proc->vfork_shared) {
        proc->pml4 = NULL; /* the parent's, borrowed by a vfork child */
    } else if (proc->pml4) {
        vmm_destroy_process_pml4(proc->pml4);
        proc->pml4 = NULL;
    }
}

bool g_syscall_trace = false;

volatile uint64_t g_last_syscall;   /* for the latency report in kernel/irq.c */
uint64_t syscall_dispatcher(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, SyscallRegs *regs) {
    g_last_syscall = num;
    sched_current()->last_syscall = num;
    /* Per-process fd namespace -- shadows the identifiers every case below
       already uses, so this is the only change needed to make fd_table/
       fd_flags/fd_oflags per-process instead of one shared global table. */
    Process *proc = sched_current()->proc;
    VfsFile  **fd_table  = proc->fd_table;
    uint32_t  *fd_flags  = proc->fd_flags;
    uint32_t  *fd_oflags = proc->fd_oflags;

    /* Per-syscall serial trace, OFF by default: every byte written to the
       UART is a VM exit, and the compositor/clients make thousands of
       syscalls a second (nanosleep, clock_gettime, poll, fb_flush) -- the
       trace alone was eating a large share of every frame. Flip
       g_syscall_trace for debugging. */
    if (g_syscall_trace && num != 0 && num != 1 && num != 20) {
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
            if (a1 < MAX_OPEN_FILES && fd_table[a1] && IS_DRM_DEV(fd_table[a1]->node.first_cluster)) {
                return (uint64_t)drm_read(fd_table[a1]->current_cluster, a2, a3,
                                          (fd_oflags[a1] & LINUX_O_NONBLOCK) != 0);
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
            if (is_kobj_fd(fd_table[a1])) {
                UIoVecW v = { (uint8_t *)a2, a3 };
                return (uint64_t)kobj_readv(fd_table[a1], fd_oflags[a1], &v, 1);
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
                    /* O_NONBLOCK: an empty pipe is -EAGAIN, not a sleep.
                       Poll-driven loops (the Zerp compositor drains every
                       client's pipe each frame) depend on this -- without
                       it one quiet client parked the whole compositor. */
                    KPipe *kp = g_pipes[pipe_idx];
                    if (kp->count == 0 && kp->is_pipe && kp->writers == 0) return 0; /* EOF */
                    if ((fd_oflags[a1] & LINUX_O_NONBLOCK) && kp->count == 0) {
                        return (uint64_t)-11; /* -EAGAIN */
                    }
                    return pipe_read(kp, (void *)a2, (uint32_t)a3);
                }
                return (uint64_t)-1;
            }
            if (fd_table[a1]->node.first_cluster == SOCK_FD_TCP) {
                if (fd_table[a1]->current_cluster == TCP_FD_NOT_CONNECTED) return (uint64_t)-107; // ENOTCONN
                TcpConnection *conn = tcp_get_connection((int)fd_table[a1]->current_cluster);
                if (!conn) return (uint64_t)-104; // ECONNRESET
                if ((fd_oflags[a1] & LINUX_O_NONBLOCK) && !tcp_conn_readable(conn))
                    return (uint64_t)-11; /* -EAGAIN: it used to block regardless */
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
                if ((fd_oflags[a1] & LINUX_O_NONBLOCK) && pty->s2m_count == 0) {
                    return (uint64_t)-11; /* -EAGAIN */
                }
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
                if ((fd_oflags[a1] & LINUX_O_NONBLOCK) && pty->m2s_count == 0) {
                    return (uint64_t)-11; /* -EAGAIN */
                }
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
            if (is_kobj_fd(fd_table[a1])) {
                UIoVecR v = { (const uint8_t *)a2, a3 };
                return (uint64_t)kobj_writev(fd_table[a1], fd_oflags[a1], &v, 1);
            }
            if (fd_table[a1]->node.first_cluster == 0xFFFFFFFB || fd_table[a1]->node.first_cluster == 0xFFFFFFFC) {
                uint32_t pipe_idx = fd_table[a1]->current_cluster;
                if (pipe_idx < MAX_PIPES && g_pipes[pipe_idx] != NULL) {
                    /* Non-blocking + atomic (POSIX PIPE_BUF): a small message either
                       fits whole or fails, never a torn half-message; a larger
                       one writes what fits, or fails only if nothing does.
                       Check and write can't be separated: syscalls run with
                       IF=0 (SFMASK) and pipe_write() only sleeps when it
                       can't proceed, which these checks rule out. */
                    if (a3 == 0) return 0;
                    KPipe *kp = g_pipes[pipe_idx];
                    if (kp->is_pipe && kp->readers == 0) {
                        signal_raise_current(13 /* SIGPIPE */);
                        return (uint64_t)-32; /* -EPIPE */
                    }
                    uint32_t len = a3 > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)a3;
                    uint32_t want = len <= PIPE_BUF_SIZE ? len : 1;
                    if ((fd_oflags[a1] & LINUX_O_NONBLOCK) && PIPE_BUF_SIZE - kp->count < want) {
                        return (uint64_t)-11; /* -EAGAIN */
                    }
                    uint32_t w = pipe_write(kp, (const void *)a2, len);
                    if (w == 0 && kp->is_pipe && kp->readers == 0) {
                        signal_raise_current(13 /* SIGPIPE */);
                        return (uint64_t)-32; /* -EPIPE */
                    }
                    return w;
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

                if ((a1 == 1 || a1 == 2) && fd_table[a1] == NULL) { // stdout/stderr, not redirected
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
                if (is_kobj_fd(fd_table[a1])) {
                    UIoVecR v[IOV_MAX_LOCAL];
                    for (int i = 0; i < iovcnt; i++) {
                        v[i].base = (const uint8_t *)kiov[i].iov_base;
                        v[i].len = kiov[i].iov_base ? kiov[i].iov_len : 0;
                    }
                    return (uint64_t)kobj_writev(fd_table[a1], fd_oflags[a1], v, iovcnt);
                }
                /* Everything else: one write() per entry, so pipes/PTYs/TCP
                   get their own paths instead of vfs_write(). */
                for (int i = 0; i < iovcnt; i++) {
                    if (kiov[i].iov_base && kiov[i].iov_len > 0) {
                        int64_t written = (int64_t)syscall_dispatcher(1, a1, (uint64_t)(uintptr_t)kiov[i].iov_base,
                                                                      kiov[i].iov_len, 0, 0, regs);
                        if (written < 0) return total_written ? total_written : (uint64_t)written;
                        total_written += (size_t)written;
                        if ((size_t)written < kiov[i].iov_len) break;
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

                trace_str("Syscall: open path: ");
                trace_str(path_kbuf);
                trace_str("\r\n");

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
                /* before opening: O_TRUNC acts inside vfs_open_flags */
                if (!vfs_may_access(path_kbuf, ((vfs_flags & 0x01) ? 4u : 0u) |
                                               ((vfs_flags & 0x1A) ? 2u : 0u)))
                    return (uint64_t)-13; /* -EACCES */
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
                fd_set_open_flags(fd_flags, fd_oflags, fd, linux_flags);
                return fd;
            }

        case 3: // SYS_close (Linux standard)
            if (a1 >= MAX_OPEN_FILES || fd_table[a1] == NULL) {
                return (uint64_t)-1;
            }
            if (IS_DRM_DEV(fd_table[a1]->node.first_cluster))
                drm_release(fd_table[a1]->current_cluster, proc->pid, true);
            /* The next open()/socket() reusing this number must not inherit
               this fd's O_NONBLOCK or FD_CLOEXEC (only some creators set
               them). */
            fd_oflags[a1] = 0;
            fd_flags[a1] = 0;
            /* Per-type cleanup lives in kfile_close() (shared with process
               teardown and dropped SCM_RIGHTS fds). Pipes/eventfd/PTYs
               still only free the wrapper, never the KPipe/Pty itself --
               a real refcount for those is the accepted gap noted there.
               Unlink first: a socket teardown may close other fds. */
            {
                VfsFile *f = fd_table[a1];
                fd_table[a1] = NULL;
                kfile_close(f);
            }
            return 0;

        case 436: // SYS_close_range(first, last, flags): glib closes a child's fds with it
            {
                if (a1 > a2) return (uint64_t)-22;                      /* -EINVAL */
                if (a3 & ~(uint64_t)(2 | 4)) return (uint64_t)-22;      /* only UNSHARE (no-op), CLOEXEC */
                uint64_t last = a2 < MAX_OPEN_FILES - 1 ? a2 : MAX_OPEN_FILES - 1;
                for (uint64_t fd = a1; fd <= last && fd < MAX_OPEN_FILES; fd++) {
                    if (!fd_table[fd]) continue;
                    if (a3 & 4) { fd_flags[fd] |= FD_CLOEXEC; continue; }  /* CLOSE_RANGE_CLOEXEC */
                    if (IS_DRM_DEV(fd_table[fd]->node.first_cluster))
                        drm_release(fd_table[fd]->current_cluster, proc->pid, true);
                    fd_oflags[fd] = 0;
                    fd_flags[fd] = 0;
                    VfsFile *f = fd_table[fd];
                    fd_table[fd] = NULL;
                    kfile_close(f);
                }
                return 0;
            }

        case 32: // SYS_dup(oldfd)
            {
                int oldfd = (int)a1;
                if (oldfd < 0 || oldfd >= MAX_OPEN_FILES || fd_table[oldfd] == NULL) return (uint64_t)-9; // EBADF
                int newfd = get_free_fd(fd_table);
                if (newfd < 0) return (uint64_t)-24; // EMFILE
                VfsFile *copy = kfile_dup(fd_table[oldfd]); // deep copy of the wrapper -- same convention as
                                                            // process_spawn()'s fd-inheritance loop, never share the pointer
                if (!copy) return (uint64_t)-12; // ENOMEM
                fd_table[newfd] = copy;
                fd_flags[newfd] = 0; // dup'd fds are never CLOEXEC by default (POSIX)
                fd_oflags[newfd] = fd_oflags[oldfd];
                return newfd;
            }

        case 33: // SYS_dup2(oldfd, newfd) -- Phase 18. Standard idiom used
                  // by zerp_term.c's fork()+dup2()+execve() PTY spawn: the
                  // child remaps the PTY slave fd onto 0/1/2 before
                  // exec'ing nano.
        case 292: // SYS_dup3(oldfd, newfd, flags) -- same, plus O_CLOEXEC,
                   // and oldfd == newfd is -EINVAL instead of a no-op.
            {
                int oldfd = (int)a1;
                int newfd = (int)a2;
                uint32_t dflags = (num == 292) ? (uint32_t)a3 : 0;
                if (num == 292 && (dflags & ~02000000u)) return (uint64_t)-22; // EINVAL
                if (oldfd < 0 || oldfd >= MAX_OPEN_FILES || fd_table[oldfd] == NULL) return (uint64_t)-9; // EBADF
                if (newfd < 0 || newfd >= MAX_OPEN_FILES) return (uint64_t)-9; // EBADF
                if (oldfd == newfd) return (num == 292) ? (uint64_t)-22 : (uint64_t)newfd;
                VfsFile *copy = kfile_dup(fd_table[oldfd]);
                if (!copy) return (uint64_t)-12; // ENOMEM
                if (fd_table[newfd] != NULL) {
                    /* Reuse SYS_close's own per-sentinel cleanup (pipe/
                       socket/PTY-specific) instead of duplicating it --
                       same self-recursion pattern already used by
                       SYS_sendto/SYS_recvfrom's non-socket fallback. */
                    syscall_dispatcher(3, (uint64_t)newfd, 0, 0, 0, 0, regs);
                }
                fd_table[newfd] = copy;
                fd_flags[newfd] = (dflags & 02000000u) ? FD_CLOEXEC : 0;
                fd_oflags[newfd] = fd_oflags[oldfd];
                return newfd;
            }

        case 8: // SYS_lseek (Linux standard)
            if (a1 >= MAX_OPEN_FILES || fd_table[a1] == NULL) {
                return (uint64_t)-9; /* -EBADF */
            }
            if (fd_table[a1]->node.first_cluster == USOCK_FD ||
                fd_table[a1]->node.first_cluster == TIMERFD_FD) return (uint64_t)-29; /* -ESPIPE */
            if (fd_table[a1]->node.first_cluster == MEMFD_FD) {
                int64_t base = (a3 == 0) ? 0 : (a3 == 1) ? (int64_t)fd_table[a1]->offset
                             : (a3 == 2) ? (int64_t)memfd_size((int)fd_table[a1]->current_cluster) : -1;
                if (base < 0) return (uint64_t)-22; /* -EINVAL (SEEK_DATA/HOLE unsupported) */
                int64_t pos = base + (int64_t)a2;
                if (pos < 0 || pos > 0xFFFFFFFFLL) return (uint64_t)-22;
                fd_table[a1]->offset = (uint32_t)pos;
                return (uint64_t)pos;
            }
            {
                /* the new offset is the result (vfs_seek says only ok or
                   not): programs check lseek(fd, off, SEEK_SET) == off */
                VfsFile *f = fd_table[a1];
                int64_t size = (int64_t)f->node.size, cur = (int64_t)f->offset, off = (int64_t)a2, pos;
                switch (a3) {
                    case 0: pos = off; break;                       /* SEEK_SET */
                    case 1: pos = cur + off; break;                 /* SEEK_CUR */
                    case 2: pos = size + off; break;                /* SEEK_END */
                    case 3: if (off < 0 || off >= size) return (uint64_t)-6;  /* SEEK_DATA: -ENXIO past the end */
                            pos = off; break;                       /* no holes: data is everywhere */
                    case 4: if (off < 0 || off >= size) return (uint64_t)-6;  /* SEEK_HOLE */
                            pos = size; break;
                    default: return (uint64_t)-22;                  /* -EINVAL */
                }
                if (pos < 0 || pos > 0xFFFFFFFFLL) return (uint64_t)-22;
                f->offset = (uint32_t)pos;
                return (uint64_t)pos;
            }

        case 9: // SYS_mmap (Linux standard)
            if (a2 == 0) return 0;
            {
                uint64_t addr = a1;
                uint64_t len = a2;
                uint64_t prot = a3;
                uint64_t flags = a4;
                int fd = (int)a5;
                uint64_t offset = regs->r9;

                if (len > USER_MAP_LEN_MAX) return (uint64_t)-12; /* -ENOMEM */
                uint64_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
                uint64_t size_aligned = pages * PAGE_SIZE;

                /* MAP_FIXED: only where the process may map at all (not
                   over the shared kernel identity map). Without MAP_FIXED
                   the address is a hint the paths below ignore. */
                if ((flags & 0x10) && (!user_map_range_ok(addr, size_aligned) ||
                    range_has_kernel_pages(vmm_get_current_pml4(), addr, addr + size_aligned)))
                    return (uint64_t)-22; /* -EINVAL */

                /* Same W^X policy as mprotect()/the ELF loader (kernel/elf.c):
                   reject outright rather than silently granting a mapping
                   that's both writable and executable -- except anonymous
                   memory of a process that opted in for a JIT (allow_wx). */
                if ((prot & 0x2) && (prot & 0x4) &&
                    !(sched_current()->proc->allow_wx && (flags & 0x20))) {
                    return (uint64_t)-13; /* -EACCES (it was -1: EPERM) */
                }

                // Check if mapping the framebuffer character device /dev/fb0
                if (fd >= 0 && fd < MAX_OPEN_FILES && fd_table[fd] != NULL && fd_table[fd]->node.first_cluster == 0xFFFFFFF0) {
                    if (!g_boot_info) return 0;
                    extern uint64_t fb_active_phys_addr(void);
                    uint64_t fb_phys = fb_active_phys_addr();
                    /* The framebuffer and not one page more: the length
                       used to be taken as given, so mmap(fb0, 1 GB) mapped
                       whatever physical memory follows it -- the kernel's
                       included -- writable into the process. */
                    uint64_t fb_bytes = (uint64_t)g_boot_info->fb_pitch * g_boot_info->fb_height;
                    uint64_t fb_pages = (fb_bytes + PAGE_SIZE - 1) / PAGE_SIZE;
                    if (offset != 0 || pages > fb_pages) return (uint64_t)-22; /* -EINVAL */

                    uint64_t virt_addr = addr;
                    if (!(flags & 0x10) || virt_addr == 0) { // MAP_FIXED is 0x10
                        virt_addr = mmap_bump(MMAP_SLOT_FB, size_aligned);
                        if (!virt_addr) return (uint64_t)-12;
                    }
                    PageTable *pml4 = vmm_get_current_pml4();
                    /* Cache type depends on what fb_phys actually is:
                       - virtio-gpu: the compositor back-buffer in ordinary
                         RAM, read by the host via DMA (coherent) -> plain
                         write-back, the fastest thing there is for both
                         reads and writes.
                       - GOP: real video memory -> Write-Combining, which is
                         PAT index 4 = the PAT bit ALONE (vmm_init() programs
                         PAT4=WC). The old PAT|PWT selected index 5, which
                         is still the power-on default Write-Through: every
                         single pixel store went straight to memory. */
                    extern bool virtio_gpu_is_active(void);
                    uint64_t cache_flags = virtio_gpu_is_active() ? 0 : PAGE_PAT;
                    for (uint64_t i = 0; i < pages; i++) {
                        vmm_map_page(pml4, virt_addr + i * PAGE_SIZE, fb_phys + i * PAGE_SIZE,
                                     PAGE_WRITE | PAGE_USER | cache_flags | PAGE_NX | PAGE_SHARED_MAP);
                    }

                    return virt_addr;
                }

                // dma-buf (PRIME fd): the same BO pages, from offset 0
                if (fd >= 0 && fd < MAX_OPEN_FILES && fd_table[fd] != NULL &&
                    fd_table[fd]->node.first_cluster == DRM_PRIME_FD) {
                    uint64_t bo_phys;
                    if (drm_prime_mmap_lookup(fd_table[fd]->current_cluster, offset, size_aligned, &bo_phys) != 0)
                        return (uint64_t)-22; /* -EINVAL */
                    uint64_t virt_addr = addr & ~(uint64_t)(PAGE_SIZE - 1);
                    if (!(flags & 0x10) || virt_addr == 0) { // MAP_FIXED is 0x10
                        virt_addr = mmap_bump(MMAP_SLOT_DMABUF, size_aligned);
                        if (!virt_addr) return (uint64_t)-12;
                    }
                    /* PAGE_SHARED_REF: the mapping holds its own reference
                       on every frame (dropped by munmap/teardown), so
                       closing the BO while it is still mapped can no
                       longer hand the frames out again under the mapping */
                    uint64_t pflags = PAGE_USER | PAGE_NX | PAGE_SHARED_MAP | PAGE_SHARED_REF | ((prot & 0x2) ? PAGE_WRITE : 0);
                    PageTable *pml4 = vmm_get_current_pml4();
                    for (uint64_t i = 0; i < pages; i++) {
                        user_unmap_page(pml4, virt_addr + i * PAGE_SIZE);
                        pmm_page_incref((void *)(uintptr_t)(bo_phys + i * PAGE_SIZE));
                        vmm_map_page(pml4, virt_addr + i * PAGE_SIZE, bo_phys + i * PAGE_SIZE, pflags);
                    }
                    return virt_addr;
                }

                // DRM buffer object (VIRTGPU_MAP offset): the BO owns the pages
                if (fd >= 0 && fd < MAX_OPEN_FILES && fd_table[fd] != NULL &&
                    IS_DRM_DEV(fd_table[fd]->node.first_cluster)) {
                    uint64_t bo_phys;
                    if (drm_mmap_lookup(fd_table[fd]->current_cluster, offset, size_aligned, &bo_phys) != 0)
                        return (uint64_t)-22; /* -EINVAL */
                    uint64_t virt_addr = addr & ~(uint64_t)(PAGE_SIZE - 1);
                    if (!(flags & 0x10) || virt_addr == 0) { // MAP_FIXED is 0x10
                        virt_addr = mmap_bump(MMAP_SLOT_DRM, size_aligned);
                        if (!virt_addr) return (uint64_t)-12;
                    }
                    /* PAGE_SHARED_REF: the mapping holds its own reference
                       on every frame (dropped by munmap/teardown), so
                       closing the BO while it is still mapped can no
                       longer hand the frames out again under the mapping */
                    uint64_t pflags = PAGE_USER | PAGE_NX | PAGE_SHARED_MAP | PAGE_SHARED_REF | ((prot & 0x2) ? PAGE_WRITE : 0);
                    PageTable *pml4 = vmm_get_current_pml4();
                    for (uint64_t i = 0; i < pages; i++) {
                        user_unmap_page(pml4, virt_addr + i * PAGE_SIZE);
                        pmm_page_incref((void *)(uintptr_t)(bo_phys + i * PAGE_SIZE));
                        vmm_map_page(pml4, virt_addr + i * PAGE_SIZE, bo_phys + i * PAGE_SIZE, pflags);
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
                    /* the whole segment gets mapped, whatever len said */
                    if ((flags & 0x10) && (!user_map_range_ok(addr, (uint64_t)shm_pages * PAGE_SIZE) ||
                        range_has_kernel_pages(vmm_get_current_pml4(), addr, addr + (uint64_t)shm_pages * PAGE_SIZE)))
                        return (uint64_t)-22; /* -EINVAL */

                    uint64_t virt_addr = addr;
                    if (!(flags & 0x10) || virt_addr == 0) { // MAP_FIXED is 0x10
                        virt_addr = mmap_bump(MMAP_SLOT_SHM, (uint64_t)shm_pages * PAGE_SIZE);
                        if (!virt_addr) return (uint64_t)-12;
                    }
                    PageTable *pml4 = vmm_get_current_pml4();
                    for (uint32_t i = 0; i < shm_pages; i++) {
                        vmm_map_page(pml4, virt_addr + (uint64_t)i * PAGE_SIZE, seg->phys_addr + (uint64_t)i * PAGE_SIZE,
                                     PAGE_WRITE | PAGE_USER | PAGE_NX | PAGE_SHARED_MAP);
                    }
                    seg->refcount++;

                    return virt_addr;
                }

                /* memfd, MAP_SHARED: map the memfd's own frames, so every
                   process mapping it (fd passed over SCM_RIGHTS or
                   inherited) sees the same bytes. PAGE_SHARED_MAP marks
                   memory the process doesn't own (fork() keeps it shared;
                   munmap()/teardown don't free it). The mapping keeps the
                   frames alive with its own pmm reference per page -- a
                   process routinely mmap()s and then close()s the fd, and
                   the memfd's last close must not free mapped frames.
                   PAGE_SHARED_REF tells munmap()/teardown to drop it and
                   fork() to take one for the child. MAP_PRIVATE falls through to the
                   generic path, which copies. */
                if (fd >= 0 && fd < MAX_OPEN_FILES && fd_table[fd] != NULL &&
                    fd_table[fd]->node.first_cluster == MEMFD_FD && (flags & 0x01 /* MAP_SHARED */)) {
                    int mi = (int)fd_table[fd]->current_cluster;
                    if (offset & (PAGE_SIZE - 1)) return (uint64_t)-22; /* -EINVAL */
                    uint64_t first = offset / PAGE_SIZE;
                    for (uint64_t i = 0; i < pages; i++) {
                        if (!memfd_page_phys(mi, first + i)) return (uint64_t)-6; /* -ENXIO: past EOF */
                    }
                    PageTable *pml4 = vmm_get_current_pml4();
                    Process *mproc = sched_current()->proc;
                    uint64_t virt_addr = addr & ~(PAGE_SIZE - 1);
                    if (!(flags & 0x10) || virt_addr == 0) { // MAP_FIXED is 0x10
                        virt_addr = mmap_bump(MMAP_SLOT_MEMFD, size_aligned);
                        if (!virt_addr) return (uint64_t)-12;
                    } else {
                        for (uint64_t off = 0; off < size_aligned; off += PAGE_SIZE)
                            user_unmap_page(pml4, virt_addr + off);
                        vma_unmap_range(mproc, virt_addr, virt_addr + size_aligned);
                    }
                    uint64_t pflags = PAGE_USER | PAGE_SHARED_MAP | PAGE_SHARED_REF;
                    if (prot & 0x2) pflags |= PAGE_WRITE;
                    if (!(prot & 0x4)) pflags |= PAGE_NX;
                    for (uint64_t i = 0; i < pages; i++) {
                        uint64_t phys = memfd_page_phys(mi, first + i);
                        pmm_page_incref((void *)(uintptr_t)phys);
                        vmm_map_page(pml4, virt_addr + i * PAGE_SIZE, phys, pflags);
                    }
                    vma_insert(mproc, virt_addr, virt_addr + size_aligned,
                               (uint32_t)prot & (VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC), 0);
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
                    virt_addr = mmap_bump(MMAP_SLOT_ANON, size_aligned);
                    if (!virt_addr) return (uint64_t)-12; /* -ENOMEM */
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
                        user_unmap_page(cur_pml4, virt_addr + off); /* see munmap */
                    }
                    vma_unmap_range(mmap_proc, virt_addr, virt_addr + size_aligned);
                }

                PageTable *pml4 = vmm_get_current_pml4();

                /* Anonymous memory is demand-zero: only the range is recorded
                   here, each page gets a frame on first touch (vma_fault_in(),
                   from the page fault handler and the user-copy checks).
                   Allocating and zeroing everything up front made glibc's
                   64 MB per-thread malloc arenas and every 8 MB thread stack
                   cost real RAM, and WebKit's multi-GB reservations
                   impossible. MAP_POPULATE (0x8000) still populates now. */
                if ((flags & 0x20) && !(flags & 0x8000)) {
                    uint32_t lazy_prot = (uint32_t)prot & (VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC);
                    if (!vma_insert(sched_current()->proc, virt_addr, virt_addr + size_aligned,
                                    lazy_prot, VMA_ANON | VMA_LAZY))
                        return (uint64_t)-12; /* -ENOMEM */
                    return virt_addr;
                }

                /* A regular file the same way: each page is read from the
                   file on first touch. Reading the whole file here, with
                   interrupts off, froze the system for up to 2.7 s every
                   time ld.so mapped a big library (Mesa, Qt) -- audio
                   skipped, the clock stopped -- and read megabytes nobody
                   would touch. Device and memfd mappings keep their own
                   paths above/below. */
                if (!(flags & 0x20) && !(flags & 0x8000) &&
                    fd >= 0 && fd < MAX_OPEN_FILES && fd_table[fd] != NULL &&
                    !fd_table[fd]->node.is_dir &&
                    fd_table[fd]->node.first_cluster < 0xFFFFFF00u) {
                    VmaFile *vf = vma_file_new(fd_table[fd]);
                    if (vf) {   /* else: read it all below, as before */
                        uint32_t fprot = (uint32_t)prot & (VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC);
                        if (!vma_insert_file(sched_current()->proc, virt_addr, virt_addr + size_aligned,
                                             fprot, VMA_LAZY | VMA_FILE, vf, offset, len)) {
                            vf->refs = 1;   /* drop it the regular way (unpins) */
                            extern void vma_file_put(VmaFile *f);
                            vma_file_put(vf);
                            return (uint64_t)-12;
                        }
                        return virt_addr;
                    }
                }

                /* Physical pages one by one. This took one physically
                   contiguous run per mapping: every 8 MB thread stack of
                   every Qt app needed 2048 adjacent free frames, and once
                   memory fragmented, mmap failed while plenty was free
                   (three Qt clients were enough to get one killed). Nothing
                   needs user memory to be contiguous -- device paths walk
                   it page by page or bounce. */
                for (uint64_t i = 0; i < pages; i++) {
                    void *pg = pmm_alloc_page();
                    if (!pg) {
                        serial_write_string("SYS_mmap: out of memory\r\n");
                        for (uint64_t j = 0; j < i; j++) user_unmap_page(pml4, virt_addr + j * PAGE_SIZE);
                        return (uint64_t)-12; /* -ENOMEM */
                    }
                    /* Real prot: map writable now regardless of the
                       requested PROT_WRITE (file content/zeroing below
                       writes through this same mapping), then downgrade
                       once populated -- as the ELF loader does. */
                    vmm_map_page(pml4, virt_addr + i * PAGE_SIZE, (uint64_t)(uintptr_t)pg, PAGE_WRITE | PAGE_USER);
                }

                for (uint64_t i = 0; i < pages; i++) {
                    // Clear the page
                    memset((void *)(uintptr_t)(virt_addr + i * PAGE_SIZE), 0, PAGE_SIZE);
                }

                // If not MAP_ANONYMOUS (0x20), load file contents
                if (!(flags & 0x20)) {
                    if (fd >= 0 && fd < MAX_OPEN_FILES && fd_table[fd] != NULL &&
                        fd_table[fd]->node.first_cluster == MEMFD_FD) {
                        /* MAP_PRIVATE memfd: a private copy of its contents */
                        memfd_pread((int)fd_table[fd]->current_cluster, offset, (void *)(uintptr_t)virt_addr, len);
                    } else if (fd >= 0 && fd < MAX_OPEN_FILES && fd_table[fd] != NULL) {
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
                   have something to look up, then translate prot into real
                   page-table permissions (every page above was mapped
                   PAGE_WRITE|PAGE_USER unconditionally just to let the
                   zero-fill/file-content read above work at all). The W^X
                   reject above already ruled out PROT_WRITE|PROT_EXEC
                   together, so this never grants both. */
                Process *mmap_proc2 = sched_current()->proc;
                uint32_t vma_prot = (uint32_t)prot & (VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC);
                vma_insert(mmap_proc2, virt_addr, virt_addr + size_aligned, vma_prot,
                           (flags & 0x20) ? VMA_ANON : 0);

                {
                    uint64_t final_flags = PAGE_USER;
                    if (prot & 0x2) final_flags |= PAGE_WRITE;   // PROT_WRITE
                    if (!(prot & 0x4)) final_flags |= PAGE_NX;   // !PROT_EXEC
                    for (uint64_t i = 0; i < pages; i++) {
                        vmm_protect_page(pml4, virt_addr + i * PAGE_SIZE, final_flags);
                    }
                }

                return virt_addr;
            }

        case 22: // SYS_pipe (Linux standard)
        case 293: // SYS_pipe2 (Linux standard)
            {
                trace_str("Syscall: pipe/pipe2 called\r\n");
                int *pipefd = (int *)a1;
                if (!pipefd || !user_prepare_write(a1, 2 * sizeof(int))) {
                    serial_write_string("  Error: pipefd is NULL or invalid\r\n");
                    return (uint64_t)-14; /* -EFAULT */
                }

                // Allocate a pipe structure
                KPipe *p = (KPipe *)kmalloc(sizeof(KPipe));
                if (!p) return (uint64_t)-12; /* -ENOMEM */
                memset(p, 0, sizeof(KPipe));
                p->is_pipe = true;
                p->readers = 1;
                p->writers = 1;

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

                /* Fresh fds: don't inherit a previous occupant's status/
                   close-on-exec flags. pipe2() flags apply to both ends. */
                {
                    uint32_t p2 = (num == 293) ? (uint32_t)a2 : 0;
                    fd_oflags[fd_read]  = LINUX_O_RDONLY | (p2 & LINUX_O_NONBLOCK);
                    fd_oflags[fd_write] = LINUX_O_WRONLY | (p2 & LINUX_O_NONBLOCK);
                    fd_flags[fd_read]  = (p2 & 02000000) ? 1 : 0; /* O_CLOEXEC -> FD_CLOEXEC */
                    fd_flags[fd_write] = (p2 & 02000000) ? 1 : 0;
                }

                trace_str("  Success: created read_fd=");
                char fdbuf[16];
                uint_to_str(fd_read, fdbuf); trace_str(fdbuf);
                trace_str(", write_fd=");
                uint_to_str(fd_write, fdbuf); trace_str(fdbuf);
                trace_str("\r\n");

                return 0; // Success
            }

        case 87:  // SYS_unlink(path)
        case 84:  // SYS_rmdir(path)
        case 263: // SYS_unlinkat(dirfd, path, flags) -- AT_REMOVEDIR 0x200
            {
                /* There were none of these: rm, file managers and every
                   temp-file cleanup got -ENOSYS. Paths are absolute (cwd is /). */
                uint64_t up = (num == 263) ? a2 : a1;
                bool want_dir = (num == 84) || (num == 263 && (a3 & 0x200));
                char path[MAX_PATH];
                if (!up || strncpy_from_user(path, (const void *)up, sizeof(path)) < 0) return (uint64_t)-14;
                VfsStat ust;
                if (!vfs_stat(path, &ust)) return (uint64_t)-2;          /* -ENOENT */
                if (ust.is_dir && !want_dir) return (uint64_t)-21;      /* -EISDIR */
                if (!ust.is_dir && want_dir) return (uint64_t)-20;      /* -ENOTDIR */
                if (vfs_delete(path)) return 0;
                return ust.is_dir ? (uint64_t)-39 : (uint64_t)-13;       /* -ENOTEMPTY / -EACCES */
            }

        case 82:  // SYS_rename(old, new)
        case 264: // SYS_renameat(olddirfd, old, newdirfd, new)
        case 316: // SYS_renameat2(..., flags)
            {
                uint64_t uo = (num == 82) ? a1 : a2;
                uint64_t un = (num == 82) ? a2 : a4;
                char op[MAX_PATH], np[MAX_PATH];
                if (!uo || !un || strncpy_from_user(op, (const void *)uo, sizeof(op)) < 0 ||
                    strncpy_from_user(np, (const void *)un, sizeof(np)) < 0) return (uint64_t)-14;
                VfsStat rst;
                if (!vfs_stat(op, &rst)) return (uint64_t)-2;
                if (str_compare(op, np) == 0) return 0;
                if (vfs_stat(np, &rst)) {
                    /* POSIX: the target is replaced */
                    if (num == 316 && (a5 & 1)) return (uint64_t)-17;   /* RENAME_NOREPLACE */
                    if (!vfs_delete(np)) return (uint64_t)-13;
                }
                return vfs_rename(op, np) ? 0 : (uint64_t)-13;
            }

        case 258: // SYS_mkdirat(dirfd, path, mode)
            {
                char path[MAX_PATH];
                if (!a2 || strncpy_from_user(path, (const void *)a2, sizeof(path)) < 0) return (uint64_t)-14;
                VfsStat mst;
                if (vfs_stat(path, &mst)) return (uint64_t)-17;          /* -EEXIST */
                return vfs_mkdir(path) ? 0 : (uint64_t)-13;
            }

        case 83: // SYS_mkdir
            {
                char path[MAX_PATH];
                if (strncpy_from_user(path, (const void *)a1, sizeof(path)) < 0) {
                    return (uint64_t)-14; /* -EFAULT */
                }
                trace_str("Syscall: mkdir path: ");
                trace_str(path);
                trace_str("\r\n");

                extern bool vfs_mkdir(const char *path);
                if (vfs_mkdir(path)) {
                    return 0;
                }
                return (uint64_t)-17; /* -EEXIST or other error */
            }

        case 290: // SYS_eventfd2 (Linux standard)
            {
                trace_str("Syscall: eventfd2 called\r\n");
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

                trace_str("  Success: created eventfd fd=");
                char fdbuf[16];
                uint_to_str(fd, fdbuf); trace_str(fdbuf);
                trace_str("\r\n");
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
                stat_fill_owner(user_stat, &vst);
                if (vst.is_dir) {
                    user_stat->st_mode = S_IFDIR | (vst.mode ? vst.mode : 0755);
                } else {
                    user_stat->st_mode = S_IFREG | (vst.mode ? vst.mode : 0644);
                    user_stat->st_size = (int64_t)vst.size;
                }
                user_stat->st_blocks = (user_stat->st_size + 511) / 512;
                return 0;
            }

        case 10: // SYS_mprotect (Linux standard)
            {
                if (a2 > USER_MAP_MAX) return (uint64_t)-22; /* -EINVAL */
                uint64_t addr = a1;
                uint64_t len = (a2 + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
                if (len == 0) return 0;
                if (!user_map_range_ok(addr, len)) return (uint64_t)-22;
                uint64_t end = addr + len;
                uint64_t prot = a3;

                /* Same W^X policy as the ELF loader (kernel/elf.c): reject
                   outright rather than silently granting a page that's both
                   writable and executable -- except anonymous memory of a
                   process that opted in for a JIT (allow_wx). */
                if ((prot & 0x2) && (prot & 0x4)) {
                    VMA *jv = vma_find(sched_current()->proc, addr);
                    if (!sched_current()->proc->allow_wx || !jv || !(jv->flags & VMA_ANON) ||
                        (jv->flags & VMA_FILE) || jv->end < end)
                        return (uint64_t)-13; /* -EACCES */
                }

                Process *proc = sched_current()->proc;
                uint32_t new_prot = (uint32_t)prot & (VMA_PROT_READ | VMA_PROT_WRITE | VMA_PROT_EXEC);
                if (!vma_protect_range(proc, addr, end, new_prot)) {
                    return (uint64_t)-12; /* -ENOMEM: part of the range isn't mapped */
                }

                PageTable *pml4 = vmm_get_current_pml4();
                for (uint64_t a = addr; a < end; a += PAGE_SIZE) {
                    uint64_t old_flags = vmm_get_page_flags(pml4, a);
                    if (!(old_flags & PAGE_PRESENT)) continue; /* shouldn't happen under the eager-allocation model, but don't fault the kernel over it */

                    uint64_t pte_flags = PAGE_USER | (old_flags & (PAGE_SHARED_MAP | PAGE_SHARED_REF));
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
                    if (!(prot & 0x4)) { // !PROT_EXEC
                        pte_flags |= PAGE_NX;
                    }
                    vmm_protect_page(pml4, a, pte_flags);
                }
                return 0;
            }

        case 11: // SYS_munmap (Linux standard)
            {
                /* Only the process's own part of the address space: this
                   used to unmap (and free!) whatever was at any address,
                   kernel identity-map pages shared with every process
                   included, and munmap(x, 2^63) looped page by page with
                   interrupts off for ever. */
                if (a2 > USER_MAP_MAX) return (uint64_t)-22; /* -EINVAL */
                uint64_t addr = a1;
                uint64_t len = (a2 + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
                if (!user_map_range_ok(addr, len)) return (uint64_t)-22;
                uint64_t end = addr + len;

                Process *proc = sched_current()->proc;
                PageTable *pml4 = vmm_get_current_pml4();
                user_unmap_range(pml4, addr, end); /* fb0/SHM/DRM pages belong to their object */
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
            if (fd_table[a1]->node.first_cluster == MEMFD_FD) {
                return (uint64_t)memfd_pread((int)fd_table[a1]->current_cluster, a4, (void *)a2, a3);
            }
            if (fd_table[a1]->node.first_cluster == USOCK_FD) return (uint64_t)-29; /* -ESPIPE */
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
                /* Phase 2 (sec hardening): process-local break in a private
                   PML4 slot (HEAP_BASE/HEAP_MAX, include/wynland/process.h)
                   -- replaces the old single function-static current_brk
                   shared by every process on the system, starting at
                   0x60000000 (PML4 index 0, the SAME shared kernel/RAM/
                   framebuffer identity-map chain every process's PML4
                   aliases by pointer, see vmm_new_process_pml4()). Mapping
                   heap pages there modified that shared chain directly:
                   one process's malloc() could appear in (or corrupt)
                   every other process's address space, and the single
                   global counter made unrelated processes fight over each
                   other's heap. */
                Process *proc = sched_current()->proc;
                uint64_t requested = a1;

                if (requested == 0) {
                    return proc->brk_current;
                }
                /* Reject out-of-range requests by leaving the break
                   unchanged (real brk(2) semantics: an invalid request is
                   a no-op that returns the CURRENT break, not a hard
                   error) -- this also catches address+length-style
                   overflow for free, since any wrapped uint64_t value is
                   either < brk_start or > HEAP_MAX. */
                if (requested < proc->brk_start || requested > HEAP_MAX) {
                    return proc->brk_current;
                }

                PageTable *pml4 = vmm_get_current_pml4();

                if (requested > proc->brk_current) {
                    uint64_t start = (proc->brk_current + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                    uint64_t end   = (requested + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                    uint64_t total_pages = (end - start) / PAGE_SIZE;

                    /* Transactional growth: map every page the request
                       needs before reporting success. If allocation fails
                       partway through, unmap and free everything mapped
                       so far and report the OLD break unchanged -- never
                       leave (or report) a half-mapped range. */
                    uint64_t mapped;
                    bool ok = true;
                    for (mapped = 0; mapped < total_pages; mapped++) {
                        uint64_t addr = start + mapped * PAGE_SIZE;
                        void *phys = pmm_alloc_page();
                        if (!phys) { ok = false; break; }
                        vmm_map_page(pml4, addr, (uint64_t)(uintptr_t)phys, PAGE_WRITE | PAGE_USER | PAGE_NX);
                        memset((void *)(uintptr_t)addr, 0, PAGE_SIZE);
                    }
                    if (!ok) {
                        for (uint64_t j = 0; j < mapped; j++) {
                            uint64_t addr = start + j * PAGE_SIZE;
                            uint64_t phys = vmm_get_phys(pml4, addr);
                            vmm_unmap_page(pml4, addr);
                            if (phys) pmm_free_page((void *)(uintptr_t)phys);
                        }
                        return proc->brk_current;
                    }
                    if (total_pages > 0) vma_insert(proc, start, end, VMA_PROT_READ | VMA_PROT_WRITE, VMA_ANON);
                    proc->brk_current = requested;
                } else if (requested < proc->brk_current) {
                    /* Shrink: only free pages whose ENTIRE range is past
                       the new break -- a page the new break still lands
                       inside keeps whatever live sub-page data it holds. */
                    uint64_t new_ceiling = (requested + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                    uint64_t old_ceiling = (proc->brk_current + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                    for (uint64_t addr = new_ceiling; addr < old_ceiling; addr += PAGE_SIZE) {
                        uint64_t phys = vmm_get_phys(pml4, addr);
                        if (phys) {
                            vmm_unmap_page(pml4, addr);
                            pmm_free_page((void *)(uintptr_t)phys); /* refcount-aware -- correct even if this page is still COW-shared post-fork() */
                        }
                    }
                    if (new_ceiling < old_ceiling) vma_unmap_range(proc, new_ceiling, old_ceiling);
                    proc->brk_current = requested;
                }

                return proc->brk_current;
            }

        case 24: // SYS_sched_yield (Linux standard)
            sched_yield();
            return 0;

        case 1000: // WynlandOS debugging: dump every user thread to the serial log
            sched_dump_user_threads();
            return 0;

        case 39: // SYS_getpid (Linux standard)
            return sched_current()->proc->pid;

        case 56: // SYS_clone (Linux standard)
            if (!(a1 & 0x00010000)) { /* no CLONE_THREAD: a new PROCESS */
                /* glibc's fork() is clone(SIGCHLD|CLONE_CHILD_SETTID|
                   CLONE_CHILD_CLEARTID, stack=0, ctid=&self->tid) and its
                   posix_spawn() is clone(CLONE_VM|CLONE_VFORK|SIGCHLD,
                   stack) -- both used to land in the thread path below, so
                   the "child" was a thread of the caller and its execve()
                   replaced the CALLER's image. Run them through the real
                   fork (COW copy). CLONE_VM is treated as a copy too: the
                   posix_spawn child only runs on its own stack until
                   execve(), so the one visible difference is that a
                   failure code it writes back into the parent's memory is
                   lost (the parent then sees success and a child that
                   exits with 127). */
                if ((a1 & 0x00100000) && a3 && !user_prepare_write(a3, sizeof(int))) return (uint64_t)-14;
                /* CLONE_VM|CLONE_VFORK (vfork(), glibc's posix_spawn): a
                   real vfork -- the child runs in OUR address space, so what
                   it writes (posix_spawn's exec error code) is what we read,
                   and we sleep until it execve()s or exits. Plain CLONE_VM
                   without VFORK stays a copy. */
                bool vfork_mode = (a1 & 0x00000100) && (a1 & 0x00004000);
                uint64_t saved_rsp = regs->rsp;
                if (a2) regs->rsp = a2;           /* child runs on the given stack */
                g_fork_share_mm = vfork_mode;
                uint64_t cpid = syscall_dispatcher(57, 0, 0, 0, 0, 0, regs);
                g_fork_share_mm = false;
                regs->rsp = saved_rsp;
                if ((int64_t)cpid < 0) return cpid;
                Process *cp = process_find_by_pid(cpid);
                Thread *ct = cp ? cp->main_thread : NULL;
                if (ct) {
                    uint32_t ctidv = (uint32_t)ct->id;
                    if ((a1 & 0x01000000) && a4) poke_user_u32(cp->pml4, a4, ctidv); /* CLONE_CHILD_SETTID */
                    if (a1 & 0x00200000) ct->clear_tid = (uint32_t *)a4;             /* CLONE_CHILD_CLEARTID */
                    if ((a1 & 0x00100000) && a3) *(int *)(uintptr_t)a3 = (int)ctidv; /* CLONE_PARENT_SETTID */
                }
                if (vfork_mode && cp) {
                    extern uint64_t timer_get_ms(void);
                    while (!cp->vfork_released)
                        waitqueue_wait_ms(&cp->vfork_wq, timer_get_ms() + 20);
                }
                return cpid;
            }
            {
                bool want_parent_tid = (a1 & 0x00100000) != 0; // CLONE_PARENT_SETTID (0x100 is CLONE_VM)
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
                /* a user thread's tid: from the pid space (it runs only
                   after this syscall returns: IF=0 here) */
                t->id = process_alloc_pid();
                /* CLONE_SETTLS (0x80000): tls = a5, user half only (see
                   arch_prctl). Without the flag the child keeps ours. */
                if (a1 & 0x80000) t->tls_base = (a5 < 0x0000800000000000ULL) ? a5 : 0;
                else              t->tls_base = sched_current()->tls_base;
                /* This new thread shares the CALLER's process (thread_create()
                   -> thread_create_ex(..., NULL) defaults Thread.proc to
                   current_thread->proc, kernel/sched.c) -- keep a real live
                   count so the process's address space isn't torn down
                   (kernel/sched.c's cleanup loop) while a pthread sibling
                   is still running on it after the main thread exits. */
                sched_current()->proc->thread_count++;

                if (want_parent_tid && a3) {
                    *(int *)(uintptr_t)a3 = (int)t->id;
                }
                if (want_child_tid && a4) {
                    *(int *)(uintptr_t)a4 = (int)t->id;
                }
                if (a1 & 0x00200000) { // CLONE_CHILD_CLEARTID (0x80000 is CLONE_SETTLS)
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

                /* vfork: the child borrows the parent's address space as-is
                   (no copy, no COW) -- see Process.vfork_shared. */
                bool share_mm = g_fork_share_mm;
                g_fork_share_mm = false;
                PageTable *child_pml4 = share_mm ? parent->pml4 : vmm_new_process_pml4();
                if (!child_pml4) return (uint64_t)-12; /* -ENOMEM */

                if (!share_mm && !vmm_cow_clone_user_pages(parent->pml4, child_pml4)) {
                    /* Unwinds whatever pages THIS call managed to
                       COW-share before hitting OOM (each was refcounted
                       on both sides -- freeing here drops the child's
                       never-actually-created share back down). The
                       parent-side PTEs that sharing already flipped to
                       read-only+PAGE_COW are NOT reverted -- harmless
                       (a real COW candidate either way) but documented
                       rather than silently assumed away. */
                    vmm_destroy_process_pml4(child_pml4);
                    return (uint64_t)-12; /* -ENOMEM */
                }

                Process *child = process_create(child_pml4);
                if (!child) {
                    if (!share_mm) vmm_destroy_process_pml4(child_pml4);
                    return (uint64_t)-12; /* -ENOMEM */
                }
                child->uid = parent->uid;
                child->ppid = parent->pid;
                child->pgid = parent->pgid;
                child->sid = parent->sid;
                memcpy(child->exe_path, parent->exe_path, sizeof(child->exe_path));
                child->vfork_shared = share_mm;
                child->vfork_released = !share_mm;
                /* signal dispositions are inherited across fork() */
                memcpy(child->sig_acts, parent->sig_acts, sizeof(parent->sig_acts));
                /* Heap pages themselves were already deep-copied/COW-shared
                   by vmm_cow_clone_user_pages() above (they're ordinary
                   PAGE_USER leaves in that range, same as any other); this
                   just carries over the BREAK METADATA so the child's own
                   future brk() calls grow/shrink from the same point the
                   parent was at, instead of process_create()'s fresh-process
                   default of "no heap yet". */
                child->allow_wx = parent->allow_wx;
                child->brk_start = parent->brk_start;
                child->brk_current = parent->brk_current;
                memcpy(child->mmap_next, parent->mmap_next, sizeof(child->mmap_next));

                /* The VMA list is process-local bookkeeping, not part of the
                   page tables vmm_cow_clone_user_pages() just shared -- copy
                   every parent VMA into the child's own list so mprotect/
                   munmap/the page fault handler's vma_find() lookups work
                   identically in both processes after the fork. */
                vma_clone_list(child, parent);

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
                        if (!copy) break; /* OOM partway through -- same best-effort tolerance as process_spawn()'s own fd-inheritance loop */
                        *copy = *parent->fd_table[i];
                        kfile_get(copy); /* sockets/memfds count their fds */
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
                if (child_thread) child_thread->id = child->pid; /* main thread: tid == pid */
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
                #define MAX_SPAWN_ENVP 128
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

                /* execve() replaces the address space in place -- the old
                   image's VMAs (segments, stack, any mmap'd regions) no
                   longer describe anything real once elf_load() below
                   overwrites their virtual addresses with the new binary's
                   content, so drop them before loading rather than leaving
                   stale/overlapping records for mprotect()/munmap() to trip
                   over later. */
                Process *exec_proc = sched_current()->proc;
                /* A vfork child is still running in its PARENT's address
                   space: don't touch a single page of it. It gets a fresh
                   one of its own below, after the target check. */
                bool exec_vfork = exec_proc->vfork_shared;

                /* Check the target BEFORE the point of no return below: a
                   missing or non-ELF file must still fail with an error the
                   caller can see, in its intact old image. */
                {
                    VfsFile *probe = vfs_open(kernel_path);
                    unsigned char ident[20];
                    bool looks_ok = probe && vfs_read(probe, ident, sizeof(ident)) == (int)sizeof(ident) &&
                                    ident[0] == 0x7F && ident[1] == 'E' && ident[2] == 'L' && ident[3] == 'F' &&
                                    ident[4] == 2 /* ELFCLASS64 */ && ident[18] == 62 /* EM_X86_64 */;
                    if (probe) vfs_close(probe);
                    if (!looks_ok) {
                        for (int k = 0; k < argc_e; k++) kfree(argv_bufs_e[k]);
                        for (int k = 0; k < envc_e; k++) kfree(envp_bufs_e[k]);
                        return (uint64_t)-2; /* -ENOENT / -ENOEXEC */
                    }
                }

                /* The old image's bookkeeping goes only now, past the target
                   check: freed before it, a failing execve() returned into an
                   image whose untouched lazy pages could no longer fault in. */
                vma_free_list(exec_proc);

                /* Same reasoning as the VMA list above: the old image's heap
                   pages describe nothing the new image wants, and unlike the
                   VMA nodes (plain kernel bookkeeping) these are real
                   physical frames -- reset the metadata AND actually give
                   the frames back, or every execve() would leak the exiting
                   image's entire heap forever. */
                for (uint64_t a = exec_proc->brk_start; !exec_vfork && a < exec_proc->brk_current; a += PAGE_SIZE) {
                    uint64_t phys = vmm_get_phys(pml4, a);
                    if (phys) {
                        vmm_unmap_page(pml4, a);
                        pmm_free_page((void *)(uintptr_t)phys);
                    }
                }
                exec_proc->brk_current = exec_proc->brk_start;
                memset(exec_proc->mmap_next, 0, sizeof(exec_proc->mmap_next));   /* new address space */

                /* Point of no return: drop the WHOLE old user address space
                   (segments, libraries, stack, mmaps), not just its VMAs and
                   heap. elf_load() writes the new image through these same
                   virtual addresses; a page left over from the old image
                   (e.g. exec of the same binary: its read-only text sits
                   exactly where the new text goes) made that write fault in
                   the kernel. argv/envp/path are already in kernel buffers. */
                if (exec_vfork) {
                    PageTable *own = vmm_new_process_pml4();
                    if (!own) {
                        for (int k = 0; k < argc_e; k++) kfree(argv_bufs_e[k]);
                        for (int k = 0; k < envc_e; k++) kfree(envp_bufs_e[k]);
                        return (uint64_t)-12; /* -ENOMEM, parent's memory untouched */
                    }
                    exec_proc->pml4 = own;
                    exec_proc->vfork_shared = false;
                    pml4 = own;
                    __asm__ volatile("mov %0, %%cr3" :: "r"((uint64_t)(uintptr_t)own) : "memory");
                    /* the parent may run again: we no longer use its memory */
                    exec_proc->vfork_released = true;
                    waitqueue_wake_all(&exec_proc->vfork_wq);
                } else {
                    vmm_free_user_mappings(pml4);
                    __asm__ volatile("mov %%cr3, %%rax; mov %%rax, %%cr3" ::: "rax", "memory");
                }
                /* Like Linux's exec: the clear-tid word lived in the old image */
                sched_current()->clear_tid = NULL;
                /* ...and the new image starts from a clean FPU/SSE state (the
                   syscall exit path FXRSTORs this image) */
                thread_fx_default(thread_fx_user(sched_current()));

                bool execve_ok = elf_load(kernel_path, &entry_point, &stack_top, pml4,
                                          have_argv_e ? kargv_e : NULL, have_envp_e ? kenvp_e : NULL, exec_proc);
                for (int k = 0; k < argc_e; k++) kfree(argv_bufs_e[k]);
                for (int k = 0; k < envc_e; k++) kfree(envp_bufs_e[k]);
                if (!execve_ok) {
                    /* The old image is gone: nothing to return to. */
                    serial_write_string("SYS_execve: elf_load failed after teardown, exiting\r\n");
                    thread_exit();
                    return (uint64_t)-2;
                }

                /* Close-on-exec: an fd marked FD_CLOEXEC must not survive
                   into the new image. (Before this nothing closed them --
                   e.g. a socket end a spawned helper should never have kept
                   stayed open in it, so the peer never saw EOF.) */
                for (int i = 0; i < MAX_OPEN_FILES; i++) {
                    if (fd_table[i] && (fd_flags[i] & FD_CLOEXEC)) {
                        VfsFile *cf = fd_table[i];
                        fd_table[i] = NULL;
                        fd_flags[i] = 0;
                        fd_oflags[i] = 0;
                        kfile_close(cf);
                    }
                }

                // Update syscall regs to jump to the new entry point on return
                regs->rip = entry_point;
                regs->rsp = stack_top;
                
                str_copy(exec_proc->exe_path, kernel_path);
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
            /* This OS's model: the main thread's exit is the process's
               exit, so remember the code for its wait status. */
            if (proc->main_thread == sched_current()) proc->exit_code = (int)a1;
            thread_exit();
            return 0;

        case 158: // SYS_arch_prctl (Linux standard)
            {
                if (a1 == 0x1002) { // ARCH_SET_FS
                    /* User addresses only (canonical lower half): anything
                       else would #GP in the kernel's own wrmsr below and on
                       every later context switch -- a one-syscall kernel
                       crash on real hardware. Linux answers -EPERM. */
                    if (a2 >= 0x0000800000000000ULL) return (uint64_t)-1; /* -EPERM */
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

        case 231: // SYS_exit_group (Linux standard) -- the whole process
            {
                proc->exit_code = (int)a1;
                /* Every other thread of the process dies too: SIGKILL
                   pending + woken if asleep; the syscall-return path or the
                   timer (for one running user code) terminates it. It used
                   to end only the calling thread, so glibc's exit() from a
                   helper thread left the rest of the program running. */
                extern Thread *sched_get_thread_list(void);
                Thread *start = sched_get_thread_list(), *it = start;
                int guard = 0;
                if (it) do {
                    if (it->proc == proc && it != sched_current() &&
                        it->state != THREAD_STATE_TERMINATED) {
                        it->sig_pending |= 1ULL << 9;
                        if (it->state == THREAD_STATE_BLOCKED) sched_unblock(it, -LNX_EINTR);
                    }
                    it = it->next;
                } while (it != start && ++guard < 100000);
                process_mark_exited(proc, ((int)a1 & 0xFF) << 8);
                thread_exit();
                return 0;
            }

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
                /* Checked against the salted hash in /etc/shadow
                   (kernel/auth.c); it was a constant in this file.
                   -ENOENT: no root password yet (`ary login` sets one). */
                if (!a1) return (uint64_t)-14;
                char given[128];
                if (strncpy_from_user(given, (const void *)a1, sizeof(given)) < 0) {
                    return (uint64_t)-14; /* -EFAULT */
                }
                if (!auth_root_password_set()) return (uint64_t)-2;
                if (auth_check_root(given)) {
                    sched_current()->proc->uid = 0;
                    return 0;
                }
                /* like sudo: a wrong password costs a couple of seconds,
                   so guessing it by brute force is not practical */
                sched_sleep_ms(2000);
                return (uint64_t)-13; /* -EACCES */
            }

        case 413: // SYS_ary_passwd(new_password, old_password): set the root
                  // password. Allowed when none is set yet (first boot,
                  // `ary login`), for root, or with the current password.
            {
                char npw[128], opw[128] = {0};
                if (!a1 || strncpy_from_user(npw, (const void *)a1, sizeof(npw)) < 0) return (uint64_t)-14;
                if (a2 && strncpy_from_user(opw, (const void *)a2, sizeof(opw)) < 0) return (uint64_t)-14;
                if (!npw[0]) return (uint64_t)-22; /* -EINVAL: empty */
                bool allowed = !auth_root_password_set() || sched_current()->proc->uid == 0 ||
                               (a2 && auth_check_root(opw));
                if (!allowed) { sched_sleep_ms(2000); return (uint64_t)-13; } /* -EACCES */
                return auth_set_root(npw) ? 0 : (uint64_t)-5; /* -EIO */
            }

        case 414: // SYS_ary_status: 1 when a root password is set, else 0
            return auth_root_password_set() ? 1 : 0;

        case 105: // SYS_setuid(uid): root may become anyone; others only themselves
            {
                Process *pr = sched_current()->proc;
                if (pr->uid != 0 && (uint32_t)a1 != pr->uid) return (uint64_t)-1; /* -EPERM */
                pr->uid = (uint32_t)a1;
                return 0;
            }

        case 90:  // SYS_chmod(path, mode)
        case 268: // SYS_fchmodat(dirfd, path, mode, flags) -- absolute paths
            {
                uint64_t up = (num == 90) ? a1 : a2;
                uint32_t mode = (uint32_t)((num == 90) ? a2 : a3);
                char cpath[MAX_PATH];
                if (!up || strncpy_from_user(cpath, (const void *)up, sizeof(cpath)) < 0) return (uint64_t)-14;
                VfsStat cst;
                if (!vfs_stat(cpath, &cst)) return (uint64_t)-2;  /* -ENOENT */
                return vfs_chmod(cpath, mode) ? 0 : (uint64_t)-1; /* -EPERM */
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
                /* Fresh fds: blocking by default, whatever the slot held. */
                fd_oflags[fd_master] = LINUX_O_RDWR;
                fd_oflags[fd_slave]  = LINUX_O_RDWR;
                return 0;
            }

        case 412: // SYS_mouse_events(MouseEvent *buf, int max) -- the
                   // kernel's own absolute pointer events {x, y, buttons},
                   // one per PS/2 packet (see drivers/input/mouse.c). The
                   // IRQ is the single integrator (it also drives the
                   // virtio-gpu cursor plane), so the compositor's hit-
                   // testing can't drift from what's on screen. Exclusive:
                   // the first caller owns the stream until it exits (like
                   // an input grab) -- other processes get -EBUSY instead
                   // of reading someone else's pointer. The owner's first
                   // read starts with the current position.
            {
                static uint64_t mouse_owner_pid = 0;
                int64_t max = (int64_t)a2;
                if (max <= 0) return (uint64_t)-22; /* -EINVAL */
                if (max > 64) max = 64;
                /* validate before claiming: a bad call must not take the stream */
                if (!a1 || !user_prepare_write(a1, (uint64_t)max * sizeof(MouseEvent))) return (uint64_t)-14; /* -EFAULT */
                if (mouse_owner_pid != proc->pid) {
                    Process *owner = mouse_owner_pid ? process_find_by_pid(mouse_owner_pid) : NULL;
                    if (owner && !owner->exited) return (uint64_t)-16; /* -EBUSY */
                    mouse_owner_pid = proc->pid;
                    mouse_events_attach();
                }
                return (uint64_t)mouse_events_read((MouseEvent *)a1, (int)max);
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

                /* DRM render/card node: straight to the virtio-gpu DRM layer
                   (hot path -- EXECBUFFER per draw batch -- so before the
                   per-call serial log below). */
                if (fd >= 0 && fd < MAX_OPEN_FILES && fd_table[fd] != NULL &&
                    IS_DRM_DEV(fd_table[fd]->node.first_cluster)) {
                    return (uint64_t)drm_ioctl(&fd_table[fd]->current_cluster, request, a3);
                }
                /* /dev/dsp: OSS ioctls to the HD Audio driver */
                if (fd >= 0 && fd < MAX_OPEN_FILES && fd_table[fd] != NULL &&
                    fd_table[fd]->node.first_cluster == DEV_DSP) {
                    return (uint64_t)hda_dsp_ioctl(request, a3);
                }

                {
                    char fd_str[32], req_str[32];
                    uint_to_hex(fd, fd_str);
                    uint_to_hex(request, req_str);
                    trace_str("SYS_ioctl: fd=");
                    trace_str(fd_str);
                    trace_str(" request=");
                    trace_str(req_str);
                    trace_str("\r\n");
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
                if (is_kobj_fd(fd_table[a1])) {
                    UIoVecW v[IOV_MAX_LOCAL];
                    for (int i = 0; i < iovcnt; i++) {
                        v[i].base = (uint8_t *)kiov[i].iov_base;
                        v[i].len = kiov[i].iov_base ? kiov[i].iov_len : 0;
                    }
                    return (uint64_t)kobj_readv(fd_table[a1], fd_oflags[a1], v, iovcnt);
                }
                /* Everything else: one read() per entry (pipes/PTYs/TCP get
                   their own paths instead of vfs_read()); stop at the first
                   short read so a blocking source isn't waited on twice. */
                for (int i = 0; i < iovcnt; i++) {
                    if (kiov[i].iov_base && kiov[i].iov_len > 0) {
                        int64_t nread = (int64_t)syscall_dispatcher(0, a1, (uint64_t)(uintptr_t)kiov[i].iov_base,
                                                                    kiov[i].iov_len, 0, 0, regs);
                        if (nread < 0) return total_read ? total_read : (uint64_t)nread;
                        total_read += (size_t)nread;
                        if ((size_t)nread < kiov[i].iov_len) break; /* short read */
                    }
                }
                return total_read;
            }

        case 21:  // SYS_access (Linux standard) — check file accessibility
        case 269: // SYS_faccessat(dirfd, path, mode)
        case 439: // SYS_faccessat2(dirfd, path, mode, flags)
            {
                if (num != 21) { a1 = a2; a2 = a3; }   /* absolute paths: dirfd unused */
                if (!a1) return (uint64_t)-14; /* -EFAULT */
                char path[MAX_PATH];
                if (strncpy_from_user(path, (const void *)a1, sizeof(path)) < 0) {
                    return (uint64_t)-14; /* -EFAULT */
                }
                VfsStat vst;
                if (!vfs_stat(path, &vst)) return (uint64_t)-2; /* -ENOENT */
                return vfs_may_access(path, (uint32_t)a2 & 7) ? 0 : (uint64_t)-13; /* -EACCES */
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
                        /* Linux semantics: only status flags change; the
                           access mode (low 2 bits) is fixed at open time. */
                        if (fd < MAX_OPEN_FILES) fd_oflags[fd] = (fd_oflags[fd] & 3) | ((uint32_t)a3 & ~3u);
                        return 0;
                    case F_DUPFD:
                    case 1030: /* F_DUPFD_CLOEXEC */ {
                        /* Find first free fd >= a3. A real copy of the
                           wrapper (never an alias: close() frees the
                           wrapper, which left the other fd dangling). */
                        if (fd >= MAX_OPEN_FILES || fd_table[fd] == NULL) return (uint64_t)-9; /* -EBADF */
                        int min_fd = (int)a3;
                        if (min_fd < 3) min_fd = 3;
                        for (int i = min_fd; i < MAX_OPEN_FILES; i++) {
                            if (fd_table[i] == NULL && i != (int)fd) {
                                VfsFile *copy = kfile_dup(fd_table[fd]);
                                if (!copy) return (uint64_t)-12; /* -ENOMEM */
                                fd_table[i] = copy;
                                fd_flags[i] = (cmd == 1030) ? FD_CLOEXEC : 0;
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
                /* prlimit64(pid, resource, new, old): report the limits this
                   kernel really has. It used to answer 1 MB for EVERY
                   resource, so RLIMIT_NOFILE said 1048576 while the fd table
                   holds MAX_OPEN_FILES -- programs that close "all" fds or
                   size tables by it did a million times too much work.
                   Setting limits is accepted but not enforced. */
                struct rlimit64 {
                    uint64_t rlim_cur;
                    uint64_t rlim_max;
                };
                const uint64_t INF = ~0ULL;
                /* getrlimit(resource, old) vs prlimit64(pid, resource, new, old) */
                int      res  = (num == 97) ? (int)a1 : (int)a2;
                uint64_t newp = (num == 97) ? 0 : a3;
                uint64_t oldp = (num == 97) ? a2 : a4;
                if (res < 0 || res >= 16) return (uint64_t)-22; /* -EINVAL */
                if (newp) {
                    struct rlimit64 ignored;
                    if (copy_from_user(&ignored, (const void *)newp, sizeof(ignored)) != 0)
                        return (uint64_t)-14;
                }
                if (oldp) {
                    if (!user_prepare_write(oldp, sizeof(struct rlimit64))) return (uint64_t)-14; /* -EFAULT */
                    struct rlimit64 *out = (struct rlimit64 *)oldp;
                    switch (res) {
                    case 3:  out->rlim_cur = 8ULL << 20; out->rlim_max = INF; break;      /* RLIMIT_STACK */
                    case 7:  out->rlim_cur = out->rlim_max = MAX_OPEN_FILES; break;       /* RLIMIT_NOFILE */
                    case 4:  out->rlim_cur = 0; out->rlim_max = INF; break;               /* RLIMIT_CORE */
                    default: out->rlim_cur = out->rlim_max = INF; break;
                    }
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

        case 61: // SYS_wait4(pid, int *wstatus, options, struct rusage *)
            {

                int options = (int)a3;
                if (options & ~(1 | 2 | 8 | 0x20000000 | 0x40000000 | (int)0x80000000)) return (uint64_t)-LNX_EINVAL;
                if (a2 && !user_prepare_write(a2, sizeof(int))) return (uint64_t)-14;
                if (a4 && !user_prepare_write(a4, 144)) return (uint64_t)-14;
                Process *c = NULL;
                int64_t r = do_wait((int64_t)a1, (options & 1) /* WNOHANG */, true, &c);
                if (r > 0) {
                    if (a2) *(int *)(uintptr_t)a2 = c->wait_status;
                    if (a4) memset((void *)(uintptr_t)a4, 0, 144); /* no resource accounting */
                }
                return (uint64_t)r;
            }

        case 247: // SYS_waitid(idtype, id, siginfo_t *, options, struct rusage *)
            {
                int options = (int)a4;
                if (!(options & (4 | 2 | 8))) return (uint64_t)-LNX_EINVAL; /* WEXITED|WSTOPPED|WCONTINUED */
                int64_t pid;
                if (a1 == 0)      pid = -1;                                   /* P_ALL */
                else if (a1 == 1) { if ((int64_t)a2 <= 0) return (uint64_t)-LNX_EINVAL; pid = (int64_t)a2; } /* P_PID */
                else if (a1 == 2) pid = a2 ? -(int64_t)a2 : 0;              /* P_PGID */
                else return (uint64_t)-LNX_EINVAL;                            /* P_PIDFD: not supported */
                if (a3 && !user_prepare_write(a3, 128)) return (uint64_t)-14;
                if (a5 && !user_prepare_write(a5, 144)) return (uint64_t)-14;
                if (!(options & 4)) {
                    /* only stopped/continued children asked for: none ever are */
                    Process *dummy = NULL;
                    int64_t any = do_wait(pid, true, false, &dummy);
                    if (any < 0) return (uint64_t)any;
                    if (a3) memset((void *)(uintptr_t)a3, 0, 128);
                    return 0;
                }
                Process *c = NULL;
                int64_t r = do_wait(pid, (options & 1) /* WNOHANG */,
                                    !(options & 0x01000000) /* WNOWAIT */, &c);
                if (r < 0) return (uint64_t)r;
                if (a3) {
                    int32_t *si = (int32_t *)(uintptr_t)a3;
                    memset(si, 0, 128);
                    if (r > 0) {
                        bool killed = (c->wait_status & 0x7F) != 0;
                        si[0] = 17;                                   /* si_signo = SIGCHLD */
                        si[2] = killed ? 2 : 1;                       /* si_code: CLD_KILLED / CLD_EXITED */
                        si[4] = (int32_t)c->pid;                      /* si_pid */
                        si[5] = (int32_t)c->uid;                      /* si_uid */
                        si[6] = killed ? (c->wait_status & 0x7F)      /* si_status */
                                       : ((c->wait_status >> 8) & 0xFF);
                    }
                }
                if (a5) memset((void *)(uintptr_t)a5, 0, 144);
                return 0;
            }

        case 62: // SYS_kill(pid, sig)
            {
                int64_t pid = (int64_t)a1;
                int sig = (int)a2;
                if (sig < 0 || sig > 64) return (uint64_t)-LNX_EINVAL;
                if (pid > 0) {
                    Process *p = process_find_by_pid((uint64_t)pid);
                    if (!p || p->reaped) return (uint64_t)-LNX_ESRCH;
                    return (uint64_t)signal_process(p, sig);
                }
                /* groups (0: ours, < -1: -pid) and -1 (everyone we may
                   signal except init-ish pid 0/1 and ourselves) */
                uint64_t grp = pid == 0 ? proc->pgid : (uint64_t)(-pid);
                bool any = false, perm = false;
                for (Process *p = process_list_head(); p; p = p->next) {
                    if (p->pid == 0 || p->reaped) continue;
                    if (pid == -1) {
                        if (p->pid == 1 || p == proc) continue;
                    } else if (p->pgid != grp) {
                        continue;
                    }
                    any = true;
                    if (signal_process(p, sig) == 0) perm = true;
                }
                if (!any) return (uint64_t)-LNX_ESRCH;
                return perm ? 0 : (uint64_t)-LNX_EPERM;
            }

        case 109: // SYS_setpgid(pid, pgid)
            {
                Process *p = a1 ? process_find_by_pid(a1) : proc;
                if (!p || p->reaped) return (uint64_t)-LNX_ESRCH;
                if (p != proc && p->ppid != proc->pid) return (uint64_t)-LNX_ESRCH; /* self or a child */
                if ((int64_t)a2 < 0) return (uint64_t)-LNX_EINVAL;
                if (p->sid != proc->sid) return (uint64_t)-LNX_EPERM;
                if (p->pid == p->sid) return (uint64_t)-LNX_EPERM;                 /* session leader */
                p->pgid = a2 ? a2 : p->pid;
                return 0;
            }

        case 121: // SYS_getpgid(pid)
            {
                Process *p = a1 ? process_find_by_pid(a1) : proc;
                if (!p || p->reaped) return (uint64_t)-LNX_ESRCH;
                return p->pgid;
            }

        case 111: // SYS_getpgrp
            return proc->pgid;

        case 112: // SYS_setsid
            if (proc->pgid == proc->pid) return (uint64_t)-LNX_EPERM; /* already a group leader */
            proc->sid = proc->pgid = proc->pid;
            return proc->sid;

        case 124: // SYS_getsid(pid)
            {
                Process *p = a1 ? process_find_by_pid(a1) : proc;
                if (!p || p->reaped) return (uint64_t)-LNX_ESRCH;
                return p->sid;
            }

        case 58: // SYS_vfork: clone(CLONE_VM | CLONE_VFORK | SIGCHLD)
            return syscall_dispatcher(56, 0x00000100 | 0x00004000 | 17, 0, 0, 0, 0, regs);

        case 110: // SYS_getppid
            return sched_current()->proc->ppid;

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
                    /* 1 kHz clock: millisecond-resolution timestamps, so
                       frame pacing / animation code sees real deltas
                       instead of 10ms steps. */
                    extern uint64_t timer_get_ms(void);
                    uint64_t ms = timer_get_ms();
                    /* CLOCK_REALTIME (0) -- real wall-clock UTC, from the
                       CMOS RTC read at boot (see kernel/rtc.c). Everything
                       else (CLOCK_MONOTONIC=1, etc.) stays uptime-based,
                       matching real Linux semantics: monotonic clocks are
                       never supposed to jump with wall-clock adjustments. */
                    if ((int)a1 == 0) ms = rtc_get_unix_time_ms();
                    tp->tv_sec = (int64_t)(ms / 1000);
                    tp->tv_nsec = (int64_t)((ms % 1000) * 1000000);
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
                trace_str("Syscall: openat path: ");
                trace_str(path);
                trace_str("\r\n");

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

                if (str_compare(path, "/proc/self/maps") == 0) {
                    int mi = proc_maps_memfd(proc);
                    if (mi < 0) return (uint64_t)-12;
                    VfsFile *f = (VfsFile *)kmalloc(sizeof(VfsFile));
                    if (!f) { memfd_unref(mi); return (uint64_t)-12; }
                    memset(f, 0, sizeof(VfsFile));
                    str_copy(f->node.name, "maps");
                    f->node.first_cluster = MEMFD_FD;
                    f->current_cluster = (uint32_t)mi;
                    fd_table[fd] = f;
                    fd_flags[fd]  = (linux_flags & 02000000) ? FD_CLOEXEC : 0;
                    fd_oflags[fd] = LINUX_O_RDONLY;
                    return (uint64_t)fd;
                }
                
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
                    sfile->node.first_cluster = 0xFFFFFFEF; // REVISION (0xFFFFFFFA is a pipe read end)
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
                
                {
                    uint32_t want = 0;
                    if (vfs_flags & 0x01) want |= 4;
                    if (vfs_flags & (0x02 | 0x10 | 0x08)) want |= 2;
                    if (!vfs_may_access(path, want)) return (uint64_t)-13; /* -EACCES */
                }
                VfsFile *file = vfs_open_flags(path, vfs_flags);
                if (!file) return (uint64_t)-2; /* -ENOENT */
                fd_table[fd] = file;
                fd_set_open_flags(fd_flags, fd_oflags, fd, linux_flags);
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
                /* fstatat(fd, "", st, AT_EMPTY_PATH) is fstat(fd) -- glibc's
                   fstat() and Qt's file size lookup use exactly this; it
                   used to look up a file named "" (-ENOENT), so Qt could not
                   size, and so not map, its plugins. */
                if (path[0] == 0) {
                    if (!(a4 & 0x1000 /* AT_EMPTY_PATH */)) return (uint64_t)-2;
                    return syscall_dispatcher(5, a1, a3, 0, 0, 0, regs);
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
                stat_fill_owner(user_stat, &vst);
                if (vst.is_dir) {
                    user_stat->st_mode = S_IFDIR | (vst.mode ? vst.mode : 0755);
                } else {
                    user_stat->st_mode = S_IFREG | (vst.mode ? vst.mode : 0644);
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

        case 200: // SYS_tkill(tid, sig) -- 2-arg form.
        case 234: // SYS_tgkill(tgid, tid, sig) -- 3-arg form.
            {
                /* Phase 6: raise a real pending signal on the THREAD NAMED
                   BY tid, not unconditionally the caller -- the previous
                   version called signal_raise_current() regardless of what
                   tid/tgid actually named, silently misdelivering any
                   cross-thread tkill/tgkill to the wrong thread. Delivered
                   at the target's own next syscall-return tail
                   (signal_deliver_check()); for the overwhelmingly common
                   self-targeting case (raise()/abort(), tid == caller's own
                   tid) that's still synchronous from the caller's point of
                   view, exactly as before. */
                uint64_t tid, tgid;
                int sig;
                if (num == 200) {
                    tid = a1; tgid = 0; sig = (int)a2;
                } else {
                    tgid = a1; tid = a2; sig = (int)a3;
                }
                if (!signal_raise_thread(tid, tgid, sig)) {
                    return (uint64_t)-3; /* -ESRCH */
                }
                return 0;
            }

        case 204: // SYS_sched_getparam (Linux standard)
            return (uint64_t)-38; /* -ENOSYS */

        case 319: // SYS_memfd_create(name, flags) -- anonymous resizable
                   // file (kernel/memfd.c); MAP_SHARED mappings of it are
                   // real shared memory across processes. (Used to sit
                   // under 324, which is membarrier: glibc never called it.)
            {
                char name[64];
                if (!a1 || strncpy_from_user(name, (const void *)a1, sizeof(name)) < 0) {
                    /* Linux caps the name at 249 bytes; a longer one here is
                       just truncated for our own bookkeeping, not an error */
                    if (!a1) return (uint64_t)-14; /* -EFAULT */
                    name[0] = '\0';
                }
                uint32_t mflags = (uint32_t)a2;
                if (mflags & ~(0x1u /* MFD_CLOEXEC */ | 0x2u /* MFD_ALLOW_SEALING */)) return (uint64_t)-22; /* -EINVAL (MFD_HUGETLB etc.) */
                int fd = get_free_fd(fd_table);
                if (fd < 0) return (uint64_t)-24; /* -EMFILE */
                int mi = memfd_new(name);
                if (mi < 0) return (uint64_t)(int64_t)mi;
                VfsFile *f = (VfsFile *)kmalloc(sizeof(VfsFile));
                if (!f) { memfd_unref(mi); return (uint64_t)-12; }
                memset(f, 0, sizeof(VfsFile));
                str_copy(f->node.name, "memfd");
                f->node.first_cluster = MEMFD_FD;
                f->current_cluster = (uint32_t)mi;
                fd_table[fd] = f;
                fd_flags[fd]  = (mflags & 0x1u) ? FD_CLOEXEC : 0;
                fd_oflags[fd] = LINUX_O_RDWR;
                return (uint64_t)fd;
            }

        case 324: // SYS_membarrier -- not implemented (single CPU anyway)
            return (uint64_t)-38; /* -ENOSYS */

        case 77: // SYS_ftruncate(fd, length)
            if (a1 >= MAX_OPEN_FILES || fd_table[a1] == NULL) return (uint64_t)-9; /* -EBADF */
            if (fd_table[a1]->node.first_cluster == MEMFD_FD) {
                if ((int64_t)a2 < 0) return (uint64_t)-22;
                return (uint64_t)memfd_truncate((int)fd_table[a1]->current_cluster, a2);
            }
            return (uint64_t)-38; /* regular files: no truncate in the VFS yet */

        case 285: // SYS_fallocate(fd, mode, offset, len)
            if (a1 >= MAX_OPEN_FILES || fd_table[a1] == NULL) return (uint64_t)-9; /* -EBADF */
            if (fd_table[a1]->node.first_cluster == MEMFD_FD && a2 == 0) {
                int mi = (int)fd_table[a1]->current_cluster;
                uint64_t end = a3 + a4;
                if (end > memfd_size(mi)) return (uint64_t)memfd_truncate(mi, end);
                return 0;
            }
            return (uint64_t)-95; /* -EOPNOTSUPP */

        case 18: // SYS_pwrite64(fd, buf, count, offset)
            if (a1 >= MAX_OPEN_FILES || fd_table[a1] == NULL) return (uint64_t)-9; /* -EBADF */
            if (a3 != 0 && !user_check_read(a2, a3)) return (uint64_t)-14; /* -EFAULT */
            if (fd_table[a1]->node.first_cluster == MEMFD_FD) {
                return (uint64_t)memfd_pwrite((int)fd_table[a1]->current_cluster, a4, (const void *)a2, a3);
            }
            if (fd_table[a1]->node.first_cluster == USOCK_FD) return (uint64_t)-29; /* -ESPIPE */
            {
                VfsFile *file = fd_table[a1];
                uint32_t prev_pos = file->offset;
                vfs_seek(file, (int32_t)a4, 0); // SEEK_SET
                int written = vfs_write(file, (const void *)a2, (uint32_t)a3);
                vfs_seek(file, (int32_t)prev_pos, 0); // restore pos
                return (uint64_t)(int64_t)written;
            }

        case 63: // SYS_uname -- report as Linux so glibc-era software takes
                  // its Linux code paths (this kernel speaks the Linux ABI)
            {
                struct { char f[6][65]; } u;
                if (!a1 || !user_prepare_write(a1, sizeof(u))) return (uint64_t)-14; /* -EFAULT */
                memset(&u, 0, sizeof(u));
                str_copy(u.f[0], "Linux");
                str_copy(u.f[1], "wynland");
                str_copy(u.f[2], "6.8.0-wynland");
                str_copy(u.f[3], "#1 WynlandOS");
                str_copy(u.f[4], "x86_64");
                str_copy(u.f[5], "(none)");
                memcpy((void *)a1, &u, sizeof(u));
                return 0;
            }

        case 96: // SYS_gettimeofday(tv, tz) -- no vDSO here, glibc calls it
            {
                if (a1) {
                    struct { int64_t sec, usec; } tv;
                    uint64_t ms = rtc_get_unix_time_ms();
                    tv.sec = (int64_t)(ms / 1000);
                    tv.usec = (int64_t)((ms % 1000) * 1000);
                    if (copy_to_user((void *)a1, &tv, sizeof(tv)) != 0) return (uint64_t)-14;
                }
                if (a2) {
                    int32_t tz[2] = { 0, 0 };
                    if (copy_to_user((void *)a2, tz, sizeof(tz)) != 0) return (uint64_t)-14;
                }
                return 0;
            }

        case 131: // SYS_sigaltstack(ss, old_ss) -- accepted; delivery always
                   // uses the normal stack (kernel/signal.c has no alt-stack
                   // switch), so the old stack is always reported disabled.
            {
                if (a2) {
                    struct { uint64_t sp; int32_t flags; int32_t pad; uint64_t size; } old;
                    memset(&old, 0, sizeof(old));
                    old.flags = 2; /* SS_DISABLE */
                    if (copy_to_user((void *)a2, &old, sizeof(old)) != 0) return (uint64_t)-14;
                }
                if (a1 && !user_check_read(a1, 24)) return (uint64_t)-14;
                return 0;
            }

        case 25: // SYS_mremap(old, old_size, new_size, flags, new_addr)
            {
                uint64_t old = a1;
                if (old & (PAGE_SIZE - 1)) return (uint64_t)-22; /* -EINVAL */
                if (a2 > USER_MAP_LEN_MAX || a3 > USER_MAP_LEN_MAX) return (uint64_t)-12; /* -ENOMEM */
                uint64_t old_sz = (a2 + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                uint64_t new_sz = (a3 + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                if (new_sz == 0) return (uint64_t)-22;
                /* the old range is copied below: it must be the process's
                   own readable memory, not a kernel address */
                if (!user_map_range_ok(old, old_sz ? old_sz : PAGE_SIZE) ||
                    (old_sz && !user_check_read(old, old_sz))) return (uint64_t)-14; /* -EFAULT */
                if (a4 & ~1ULL) return (uint64_t)-22; /* only MREMAP_MAYMOVE */
                PageTable *pml4 = vmm_get_current_pml4();
                /* Shared mappings would lose their sharing if copied. */
                if (vmm_get_page_flags(pml4, old) & PAGE_SHARED_MAP) return (uint64_t)-12; /* -ENOMEM */
                if (new_sz <= old_sz) {
                    syscall_dispatcher(11, old + new_sz, old_sz - new_sz, 0, 0, 0, regs); /* munmap the tail */
                    return old;
                }
                if (!(a4 & 1)) return (uint64_t)-12; /* can't grow in place */
                /* Move = fresh anonymous mapping + copy + unmap the old one.
                   glibc only does this for its own private anonymous
                   chunks; on any failure it falls back to malloc+copy. */
                uint64_t nw = syscall_dispatcher(9, 0, new_sz, 0x3 /* RW */, 0x22 /* PRIVATE|ANON */,
                                                 (uint64_t)-1, regs);
                if ((int64_t)nw < 0 || nw == 0) return (uint64_t)-12;
                memcpy((void *)(uintptr_t)nw, (const void *)(uintptr_t)old, (size_t)old_sz);
                syscall_dispatcher(11, old, old_sz, 0, 0, 0, regs);
                return nw;
            }

        case 99: // SYS_sysinfo
            {
                struct {
                    int64_t uptime; uint64_t loads[3];
                    uint64_t totalram, freeram, sharedram, bufferram, totalswap, freeswap;
                    uint16_t procs, pad; uint32_t pad2;
                    uint64_t totalhigh, freehigh; uint32_t mem_unit; char f[4];
                } si;
                if (!a1 || !user_prepare_write(a1, sizeof(si))) return (uint64_t)-14;
                memset(&si, 0, sizeof(si));
                si.uptime = (int64_t)(timer_get_ms() / 1000);
                si.totalram = pmm_get_total_memory();
                si.freeram = pmm_get_free_memory();
                si.procs = 1;
                si.mem_unit = 1;
                memcpy((void *)a1, &si, sizeof(si));
                return 0;
            }

        case 283: // SYS_timerfd_create(clockid, flags)
            {
                int clockid = (int)a1;
                if (clockid != 0 && clockid != 1 && clockid != 7 /* BOOTTIME */) return (uint64_t)-22;
                int fd = get_free_fd(fd_table);
                if (fd < 0) return (uint64_t)-24;
                int ti = timerfd_new(clockid);
                if (ti < 0) return (uint64_t)(int64_t)ti;
                VfsFile *f = (VfsFile *)kmalloc(sizeof(VfsFile));
                if (!f) { timerfd_unref(ti); return (uint64_t)-12; }
                memset(f, 0, sizeof(VfsFile));
                str_copy(f->node.name, "timerfd");
                f->node.first_cluster = TIMERFD_FD;
                f->current_cluster = (uint32_t)ti;
                fd_table[fd] = f;
                fd_flags[fd]  = (a2 & 02000000) ? FD_CLOEXEC : 0;
                fd_oflags[fd] = LINUX_O_RDWR | ((uint32_t)a2 & LINUX_O_NONBLOCK);
                return (uint64_t)fd;
            }

        case 286: // SYS_timerfd_settime(fd, flags, new, old)
        case 287: // SYS_timerfd_gettime(fd, cur)
            {
                if (a1 >= MAX_OPEN_FILES || !fd_table[a1] || fd_table[a1]->node.first_cluster != TIMERFD_FD)
                    return (uint64_t)-22; /* -EINVAL */
                int ti = (int)fd_table[a1]->current_cluster;
                struct lts { int64_t sec, nsec; };
                struct { struct lts interval, value; } its;
                uint64_t ov = 0, oi = 0;
                uint64_t outp = (num == 287) ? a2 : a4;
                if (num == 286) {
                    if (!a3 || copy_from_user(&its, (const void *)a3, sizeof(its)) != 0) return (uint64_t)-14;
                    if (its.value.nsec < 0 || its.value.nsec >= 1000000000 ||
                        its.interval.nsec < 0 || its.interval.nsec >= 1000000000 ||
                        its.value.sec < 0 || its.interval.sec < 0) return (uint64_t)-22;
                    uint64_t v  = (uint64_t)its.value.sec * 1000 + ((uint64_t)its.value.nsec + 999999) / 1000000;
                    uint64_t iv = (uint64_t)its.interval.sec * 1000 + ((uint64_t)its.interval.nsec + 999999) / 1000000;
                    bool abs = (a2 & 1) != 0; /* TFD_TIMER_ABSTIME */
                    if (abs && v && timerfd_clock(ti) == 0 /* CLOCK_REALTIME */) {
                        /* the timer runs on the monotonic ms clock: move a
                           wall-clock deadline onto it */
                        uint64_t mono = timer_get_ms();
                        uint64_t rt = rtc_get_unix_time_ms();
                        v = (v > rt) ? mono + (v - rt) : mono;
                        if (v == 0) v = 1;
                    }
                    timerfd_settime(ti, v, iv, abs, &ov, &oi);
                } else {
                    timerfd_gettime(ti, &ov, &oi);
                }
                if (outp) {
                    struct { struct lts interval, value; } o;
                    o.interval.sec = (int64_t)(oi / 1000); o.interval.nsec = (int64_t)(oi % 1000) * 1000000;
                    o.value.sec = (int64_t)(ov / 1000);    o.value.nsec = (int64_t)(ov % 1000) * 1000000;
                    if (copy_to_user((void *)outp, &o, sizeof(o)) != 0) return (uint64_t)-14;
                }
                return 0;
            }

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
                   real millisecond budget. */
                int64_t timeout_ms;
                if (num == 7) {
                    int t = (int)(int32_t)a3;
                    timeout_ms = (t < 0) ? -1 : (int64_t)t;
                } else if (a3 == 0) {
                    timeout_ms = -1;
                } else {
                    struct linux_timespec { int64_t tv_sec; int64_t tv_nsec; } tmo;
                    if (copy_from_user(&tmo, (const void *)a3, sizeof(tmo)) != 0) return (uint64_t)-14;
                    if (tmo.tv_sec < 0 || tmo.tv_nsec < 0 || tmo.tv_nsec >= 1000000000) return (uint64_t)-22; /* -EINVAL */
                    /* ~292 million years in ms fits int64; beyond that, forever */
                    timeout_ms = (tmo.tv_sec > 9000000000000000LL / 1000) ? -1
                               : tmo.tv_sec * 1000 + (tmo.tv_nsec + 999999) / 1000000; /* round UP */
                }

                int ready = do_poll(fd_table, have_fds ? kfds : NULL, nfds, timeout_ms);

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
                   it silently fall through to the generic mock branch
                   below (as it did before this check existed) was a real,
                   previously-invisible bug: SYS_connect() unconditionally
                   "succeeds" for any mock socket, so a caller doing real
                   Happy-Eyeballs-style dual-stack connection racing (e.g.
                   curl, which tries AAAA records before falling back to A
                   records) would believe its IPv6 attempt had genuinely
                   connected, then hang forever trying to actually use that
                   fake, unbacked fd for a real protocol handshake -- never
                   reaching the IPv4 fallback its own logic would otherwise
                   correctly take (confirmed on the real host: IPv6 connect
                   there fails fast with ENETUNREACH, and curl falls back to
                   IPv4 immediately). */
                if (domain == 10 /* AF_INET6 */) {
                    return (uint64_t)-97; // -EAFNOSUPPORT
                }

                /* Real AF_UNIX sockets (kernel/unix_socket.c). */
                if (domain == 1 /* AF_UNIX */) {
                    int s = usock_create(type);
                    if (s < 0) return (uint64_t)-94; // -ESOCKTNOSUPPORT
                    return (uint64_t)install_usock(fd_table, fd_flags, fd_oflags, s, (uint32_t)a2);
                }

                /* Netlink and every other family: not supported, said so.
                   There used to be a mock fd here (sharing DEV_URANDOM's
                   sentinel: reads gave random bytes) -- GLib's network
                   monitor took those for routing messages and the WebKit
                   network process died on them. With the error it falls
                   back to its plain monitor, getifaddrs() to ioctls. */
                if (domain != 2 /* AF_INET */) return (uint64_t)-97;      // -EAFNOSUPPORT
                if (type != 1 && type != 2) return (uint64_t)-94;          // -ESOCKTNOSUPPORT (raw, seqpacket)

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
                    /* Other families (netlink, ...): unchanged mock. Shares
                       its sentinel with DEV_URANDOM -- a known wart. */
                    sfile->node.first_cluster = 0xFFFFFFF8;
                }
                fd_table[fd] = sfile;
                /* SOCK_NONBLOCK / SOCK_CLOEXEC in the type: they used to be
                   masked off and dropped, so curl's non-blocking socket
                   blocked in recvfrom() forever once the server went quiet. */
                fd_oflags[fd] = LINUX_O_RDWR | (((uint32_t)a2 & 04000) ? LINUX_O_NONBLOCK : 0);
                fd_flags[fd] = ((uint32_t)a2 & 02000000) ? FD_CLOEXEC : 0;
                return fd;
            }

        case 53: // SYS_socketpair(domain, type, protocol, int sv[2])
            {
                if ((int)a1 != 1 /* AF_UNIX */) return (uint64_t)-97; // -EAFNOSUPPORT
                if (!a4 || !user_prepare_write(a4, 2 * sizeof(int))) return (uint64_t)-14; // -EFAULT
                int slots[2];
                int r = usock_pair((int)a2 & 0xFF, slots);
                if (r < 0) return (uint64_t)-94; // -ESOCKTNOSUPPORT
                int64_t fd0 = install_usock(fd_table, fd_flags, fd_oflags, slots[0], (uint32_t)a2);
                if (fd0 < 0) { usock_unref(slots[1]); return (uint64_t)fd0; }
                int64_t fd1 = install_usock(fd_table, fd_flags, fd_oflags, slots[1], (uint32_t)a2);
                if (fd1 < 0) {
                    VfsFile *f0 = fd_table[fd0];
                    fd_table[fd0] = NULL;
                    kfile_close(f0);
                    return (uint64_t)fd1;
                }
                ((int *)a4)[0] = (int)fd0;
                ((int *)a4)[1] = (int)fd1;
                return 0;
            }

        case 42: // SYS_connect
            {
                int fd = (int)a1;
                if (fd < 0 || fd >= MAX_OPEN_FILES || fd_table[fd] == NULL) return -9; // EBADF

                VfsFile *file = fd_table[fd];
                if (file->node.first_cluster == USOCK_FD) {
                    char name[108]; uint32_t nlen;
                    int64_t r = sun_from_user(a2, a3, name, &nlen);
                    if (r < 0) return (uint64_t)r;
                    return (uint64_t)usock_connect((int)file->current_cluster, name, nlen,
                                                   (fd_oflags[fd] & LINUX_O_NONBLOCK) != 0);
                }

                struct linux_sockaddr_in addr;
                bool have_addr = (a2 != 0) && (copy_from_user(&addr, (const void *)a2, sizeof(addr)) == 0);

                if (file->node.first_cluster == SOCK_FD_UDP) {
                    if (!have_addr) return -14; // EFAULT
                    udp_socket_connect((int)file->current_cluster, addr.sin_addr, ntohs(addr.sin_port));
                    return 0;
                }

                if (file->node.first_cluster != SOCK_FD_TCP) {
                    return 0; // other mock sockets: pretend success (unchanged)
                }

                if (!have_addr) return -14; // EFAULT

                int idx = tcp_connect_slot(addr.sin_addr, ntohs(addr.sin_port));
                if (idx < 0) return -110; // ETIMEDOUT (covers both handshake timeout and RST)

                /* the handshake sleeps: another thread may have closed (and
                   freed) this fd meanwhile -- don't write through a stale
                   pointer */
                if (fd_table[fd] != file) {
                    tcp_close(tcp_get_connection(idx));
                    return -9; // EBADF
                }
                file->current_cluster = (uint32_t)idx;
                return 0;
            }

        case 44: // SYS_sendto(fd, buf, len, flags, dest_addr, addrlen) -- dest_addr is a5
            {
                int fd = (int)a1;
                if (fd < 0 || fd >= MAX_OPEN_FILES || fd_table[fd] == NULL) return -9; // EBADF
                if (a3 != 0 && !user_check_read(a2, a3)) return -14; // EFAULT
                VfsFile *file = fd_table[fd];

                if (file->node.first_cluster == USOCK_FD) {
                    char name[108]; uint32_t nlen = 0;
                    if (a5) {
                        int64_t r = sun_from_user(a5, regs->r9, name, &nlen);
                        if (r < 0) return (uint64_t)r;
                    }
                    UIoVecR v = { (const uint8_t *)a2, a3 };
                    return (uint64_t)usock_send((int)file->current_cluster, &v, 1, NULL, 0, (int)a4,
                                                (fd_oflags[fd] & LINUX_O_NONBLOCK) != 0,
                                                a5 ? name : NULL, nlen);
                }

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

                if (file->node.first_cluster == USOCK_FD) {
                    UIoVecW v = { (uint8_t *)a2, a3 };
                    VfsFile *fds[USOCK_MAX_MSG_FDS];
                    uint32_t nfds = 0; int mflags = 0;
                    char src[108]; uint32_t srclen = 0;
                    int64_t n = usock_recv((int)file->current_cluster, &v, 1, (int)a4,
                                           (fd_oflags[fd] & LINUX_O_NONBLOCK) != 0,
                                           fds, USOCK_MAX_MSG_FDS, &nfds, &mflags, src, &srclen);
                    for (uint32_t i = 0; i < nfds; i++) kfile_close(fds[i]); /* no cmsg buffer: discarded */
                    if (n >= 0 && a5) {
                        int64_t r = sun_to_user(a5, regs->r9, src, srclen);
                        if (r < 0) return (uint64_t)r;
                    }
                    return (uint64_t)n;
                }

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
                    if (((fd_oflags[a1] & LINUX_O_NONBLOCK) || (a4 & 0x40 /* MSG_DONTWAIT */)) &&
                        !tcp_conn_readable(conn))
                        return (uint64_t)-11; /* -EAGAIN */
                    int n = tcp_recv(conn, (void *)a2, (uint32_t)a3);
                    if (n < 0) return (uint64_t)-1;
                    return (uint64_t)n;
                }
                // Non-TCP/UDP fds: fall back to plain read() semantics
                return syscall_dispatcher(0, a1, a2, a3, 0, 0, regs);
            }

        case 46: // SYS_sendmsg(fd, const struct msghdr *, flags)
            {
                int fd = (int)a1;
                if (fd < 0 || fd >= MAX_OPEN_FILES || fd_table[fd] == NULL) return (uint64_t)-9; // EBADF
                struct linux_msghdr mh;
                if (!a2 || copy_from_user(&mh, (const void *)a2, sizeof(mh)) != 0) return (uint64_t)-14;
                if (mh.msg_iovlen > IOV_MAX_LOCAL) return (uint64_t)-22; // EINVAL

                if (!fd_is_usock(fd_table, (uint64_t)fd)) {
                    /* TCP/pipes etc.: no ancillary data here -- gather write */
                    return syscall_dispatcher(20, a1, mh.msg_iov, mh.msg_iovlen, 0, 0, regs);
                }

                struct { uint64_t base, len; } kiov[IOV_MAX_LOCAL];
                int iovcnt = (int)mh.msg_iovlen;
                if (iovcnt > 0 && copy_from_user(kiov, (const void *)mh.msg_iov, (uint64_t)iovcnt * sizeof(kiov[0])) != 0)
                    return (uint64_t)-14;
                UIoVecR v[IOV_MAX_LOCAL];
                for (int i = 0; i < iovcnt; i++) {
                    if (kiov[i].len && !user_check_read(kiov[i].base, kiov[i].len)) return (uint64_t)-14;
                    v[i].base = (const uint8_t *)kiov[i].base;
                    v[i].len = kiov[i].len;
                }

                /* SCM_RIGHTS: each passed fd becomes an in-flight copy of its
                   wrapper (holding its own object reference) until the
                   receiver installs it or the message is dropped. */
                VfsFile *fds[USOCK_MAX_MSG_FDS];
                uint32_t nfds = 0;
                if (mh.msg_control && mh.msg_controllen >= sizeof(struct linux_cmsghdr)) {
                    if (mh.msg_controllen > MSG_CTRL_MAX) return (uint64_t)-105; // ENOBUFS
                    uint8_t ctrl[MSG_CTRL_MAX];
                    if (copy_from_user(ctrl, (const void *)mh.msg_control, mh.msg_controllen) != 0) return (uint64_t)-14;
                    uint64_t off = 0;
                    while (off + sizeof(struct linux_cmsghdr) <= mh.msg_controllen) {
                        struct linux_cmsghdr ch;
                        memcpy(&ch, ctrl + off, sizeof(ch));
                        if (ch.cmsg_len < sizeof(ch) || off + ch.cmsg_len > mh.msg_controllen) break;
                        if (ch.cmsg_level == SOL_SOCKET_LVL && ch.cmsg_type == SCM_RIGHTS_TYPE) {
                            uint64_t n = (ch.cmsg_len - sizeof(ch)) / sizeof(int);
                            for (uint64_t k = 0; k < n; k++) {
                                int pfd;
                                memcpy(&pfd, ctrl + off + sizeof(ch) + k * sizeof(int), sizeof(int));
                                VfsFile *copy = NULL;
                                int64_t err = 0;
                                if (nfds >= USOCK_MAX_MSG_FDS) err = -22;           /* EINVAL (SCM_MAX_FD) */
                                else if (pfd < 0 || pfd >= MAX_OPEN_FILES || !fd_table[pfd]) err = -9; /* EBADF */
                                else if (!(copy = kfile_dup(fd_table[pfd]))) err = -12; /* ENOMEM */
                                if (err) {
                                    for (uint32_t j = 0; j < nfds; j++) kfile_close(fds[j]);
                                    return (uint64_t)err;
                                }
                                fds[nfds++] = copy;
                            }
                        }
                        /* SCM_CREDENTIALS and others: accepted and ignored */
                        off += CMSG_ALIGN8(ch.cmsg_len);
                    }
                }

                char name[108]; uint32_t nlen = 0;
                if (mh.msg_name && mh.msg_namelen) {
                    int64_t r = sun_from_user(mh.msg_name, mh.msg_namelen, name, &nlen);
                    if (r < 0) { for (uint32_t j = 0; j < nfds; j++) kfile_close(fds[j]); return (uint64_t)r; }
                }
                return (uint64_t)usock_send((int)fd_table[fd]->current_cluster, v, iovcnt, fds, nfds, (int)a3,
                                            (fd_oflags[fd] & LINUX_O_NONBLOCK) != 0,
                                            nlen ? name : NULL, nlen);
            }

        case 47: // SYS_recvmsg(fd, struct msghdr *, flags)
            {
                int fd = (int)a1;
                if (fd < 0 || fd >= MAX_OPEN_FILES || fd_table[fd] == NULL) return (uint64_t)-9; // EBADF
                struct linux_msghdr mh;
                if (!a2 || copy_from_user(&mh, (const void *)a2, sizeof(mh)) != 0) return (uint64_t)-14;
                if (mh.msg_iovlen > IOV_MAX_LOCAL) return (uint64_t)-22; // EINVAL

                if (!fd_is_usock(fd_table, (uint64_t)fd)) {
                    int64_t n = (int64_t)syscall_dispatcher(19, a1, mh.msg_iov, mh.msg_iovlen, 0, 0, regs);
                    if (n >= 0) {
                        mh.msg_controllen = 0; mh.msg_flags = 0; mh.msg_namelen = 0;
                        if (copy_to_user((void *)a2, &mh, sizeof(mh)) != 0) return (uint64_t)-14;
                    }
                    return (uint64_t)n;
                }

                struct { uint64_t base, len; } kiov[IOV_MAX_LOCAL];
                int iovcnt = (int)mh.msg_iovlen;
                if (iovcnt > 0 && copy_from_user(kiov, (const void *)mh.msg_iov, (uint64_t)iovcnt * sizeof(kiov[0])) != 0)
                    return (uint64_t)-14;
                UIoVecW v[IOV_MAX_LOCAL];
                for (int i = 0; i < iovcnt; i++) {
                    if (kiov[i].len && !user_prepare_write(kiov[i].base, kiov[i].len)) return (uint64_t)-14;
                    v[i].base = (uint8_t *)kiov[i].base;
                    v[i].len = kiov[i].len;
                }
                /* How many fds fit in the caller's control buffer: the rest
                   are closed by usock_recv() and MSG_CTRUNC is reported. */
                uint32_t fd_cap = 0;
                if (mh.msg_control && mh.msg_controllen >= sizeof(struct linux_cmsghdr) + sizeof(int)) {
                    if (!user_prepare_write(mh.msg_control, mh.msg_controllen)) return (uint64_t)-14;
                    fd_cap = (uint32_t)((mh.msg_controllen - sizeof(struct linux_cmsghdr)) / sizeof(int));
                    if (fd_cap > USOCK_MAX_MSG_FDS) fd_cap = USOCK_MAX_MSG_FDS;
                }
                VfsFile *fds[USOCK_MAX_MSG_FDS];
                uint32_t nfds = 0; int mflags = 0;
                char src[108]; uint32_t srclen = 0;
                int64_t n = usock_recv((int)fd_table[fd]->current_cluster, v, iovcnt, (int)a3,
                                       (fd_oflags[fd] & LINUX_O_NONBLOCK) != 0,
                                       fds, fd_cap, &nfds, &mflags, src, &srclen);
                if (n < 0) return (uint64_t)n;

                /* Install received fds into this process's table. */
                int newfds[USOCK_MAX_MSG_FDS];
                uint32_t ninst = 0;
                for (uint32_t i = 0; i < nfds; i++) {
                    int nfd = get_free_fd(fd_table);
                    if (nfd < 0) { kfile_close(fds[i]); mflags |= UMSG_CTRUNC; continue; }
                    fd_table[nfd] = fds[i];
                    fd_flags[nfd]  = ((uint32_t)a3 & UMSG_CMSG_CLOEXEC) ? FD_CLOEXEC : 0;
                    fd_oflags[nfd] = LINUX_O_RDWR;
                    newfds[ninst++] = nfd;
                }
                uint64_t ctl_used = 0;
                if (ninst > 0) {
                    struct linux_cmsghdr ch;
                    ch.cmsg_len = sizeof(ch) + ninst * sizeof(int);
                    ch.cmsg_level = SOL_SOCKET_LVL;
                    ch.cmsg_type = SCM_RIGHTS_TYPE;
                    memcpy((void *)mh.msg_control, &ch, sizeof(ch));
                    memcpy((uint8_t *)mh.msg_control + sizeof(ch), newfds, ninst * sizeof(int));
                    ctl_used = CMSG_ALIGN8(ch.cmsg_len);
                    if (ctl_used > mh.msg_controllen) ctl_used = mh.msg_controllen;
                }
                mh.msg_controllen = ctl_used;
                mh.msg_flags = mflags;
                if (mh.msg_name && mh.msg_namelen) {
                    uint32_t real = srclen ? 2 + srclen + (src[0] ? 1 : 0) : 0;
                    if (real) {
                        uint8_t sa[2 + 108 + 1];
                        memset(sa, 0, sizeof(sa));
                        sa[0] = 1;
                        memcpy(sa + 2, src, srclen);
                        uint32_t c = real < mh.msg_namelen ? real : mh.msg_namelen;
                        if (copy_to_user((void *)mh.msg_name, sa, c) != 0) return (uint64_t)-14;
                    }
                    mh.msg_namelen = real;
                }
                if (copy_to_user((void *)a2, &mh, sizeof(mh)) != 0) return (uint64_t)-14;
                return (uint64_t)n;
            }

        case 49: // SYS_bind
            if (fd_is_usock(fd_table, a1)) {
                char name[108]; uint32_t nlen;
                int64_t r = sun_from_user(a2, a3, name, &nlen);
                if (r < 0) return (uint64_t)r;
                return (uint64_t)usock_bind((int)fd_table[a1]->current_cluster, name, nlen);
            }
            return 0; // other sockets: pretend success (unchanged)

        case 50: // SYS_listen
            if (fd_is_usock(fd_table, a1)) return (uint64_t)usock_listen((int)fd_table[a1]->current_cluster, (int)a2);
            return 0;

        case 48: // SYS_shutdown
            if (fd_is_usock(fd_table, a1)) return (uint64_t)usock_shutdown((int)fd_table[a1]->current_cluster, (int)a2);
            return 0;

        case 54: // SYS_setsockopt
            return 0; // accepted and ignored (SO_PASSCRED, buffer sizes, ...)

        case 55: // SYS_getsockopt(fd, level, optname, optval, optlen*)
            if (fd_is_usock(fd_table, a1) && (int)a2 == SOL_SOCKET_LVL) {
                int s = (int)fd_table[a1]->current_cluster;
                uint8_t val[16];
                uint32_t vlen = sizeof(int);
                int iv = 0;
                switch ((int)a3) {
                    case 17: { /* SO_PEERCRED */
                        struct UCred c;
                        usock_peercred(s, &c);
                        memcpy(val, &c, sizeof(c));
                        vlen = sizeof(c);
                        break;
                    }
                    case 3:  iv = usock_type(s); break;          /* SO_TYPE */
                    case 4:  iv = (int)usock_sock_error(s); break; /* SO_ERROR */
                    case 7:                                        /* SO_SNDBUF */
                    case 8:  iv = 208 * 1024; break;               /* SO_RCVBUF */
                    case 39: iv = 1; break;                        /* SO_DOMAIN: AF_UNIX */
                    default: iv = 0; break;                        /* SO_PASSCRED etc. */
                }
                if (vlen == sizeof(int)) memcpy(val, &iv, sizeof(int));
                if (a4 && a5) {
                    uint32_t have;
                    if (copy_from_user(&have, (const void *)a5, sizeof(have)) != 0) return (uint64_t)-14;
                    uint32_t c = vlen < have ? vlen : have;
                    if (c && copy_to_user((void *)a4, val, c) != 0) return (uint64_t)-14;
                    if (copy_to_user((void *)a5, &vlen, sizeof(vlen)) != 0) return (uint64_t)-14;
                }
                return 0;
            }
            return 0; // other sockets: pretend success (unchanged)

        case 51: // SYS_getsockname
        case 52: // SYS_getpeername
            if (fd_is_usock(fd_table, a1)) {
                char name[108]; uint32_t nlen = 0;
                int64_t r = usock_getname((int)fd_table[a1]->current_cluster, num == 52, name, &nlen);
                if (r < 0) return (uint64_t)r;
                return (uint64_t)sun_to_user(a2, a3, name, nlen);
            }
            if (a1 < MAX_OPEN_FILES && fd_table[a1] &&
                (fd_table[a1]->node.first_cluster == SOCK_FD_UDP ||
                 fd_table[a1]->node.first_cluster == SOCK_FD_TCP)) {
                /* IPv4 names. glibc's getaddrinfo() connect()s a UDP
                   socket to each answer and asks getsockname() for the
                   source address to sort them -- it asserts the family
                   (the old "return 0, write nothing" killed WebKit's
                   network process on the first HTTPS name lookup). */
                VfsFile *f = fd_table[a1];
                uint32_t ip = 0;
                uint16_t port = 0;
                if (f->node.first_cluster == SOCK_FD_UDP) {
                    int idx = (int)f->current_cluster;
                    bool connected = udp_socket_remote(idx, &ip, &port);
                    if (num == 52) {
                        if (!connected) return (uint64_t)-107;          /* -ENOTCONN */
                    } else {
                        ip = connected ? net_get_ip() : 0;               /* source address once connected */
                        port = udp_socket_local_port(idx);
                    }
                } else {
                    TcpConnection *conn = f->current_cluster == TCP_FD_NOT_CONNECTED
                                        ? NULL : tcp_get_connection((int)f->current_cluster);
                    if (num == 52) {
                        if (!conn) return (uint64_t)-107;                /* -ENOTCONN */
                        ip = conn->remote_ip;
                        port = conn->remote_port;
                    } else if (conn) {
                        ip = conn->local_ip;
                        port = conn->local_port;
                    }
                }
                struct linux_sockaddr_in sa;
                memset(&sa, 0, sizeof(sa));
                sa.sin_family = 2; /* AF_INET */
                sa.sin_port = htons(port);
                sa.sin_addr = ip;
                uint32_t len;
                if (!a2 || !a3 || copy_from_user(&len, (const void *)a3, sizeof(len)) != 0) return (uint64_t)-14;
                uint32_t n = len < sizeof(sa) ? len : (uint32_t)sizeof(sa);
                uint32_t full = (uint32_t)sizeof(sa);
                if (copy_to_user((void *)a2, &sa, n) != 0 ||
                    copy_to_user((void *)a3, &full, sizeof(full)) != 0) return (uint64_t)-14;
                return 0;
            }
            if (a1 >= MAX_OPEN_FILES || !fd_table[a1]) return (uint64_t)-9;  /* -EBADF */
            return (uint64_t)-88;                                            /* -ENOTSOCK */

        case 43:  // SYS_accept(fd, addr, addrlen*)
        case 288: // SYS_accept4(fd, addr, addrlen*, flags)
            {
                if (fd_is_usock(fd_table, a1)) {
                    uint32_t aflags = (num == 288) ? (uint32_t)a4 : 0;
                    int64_t s = usock_accept((int)fd_table[a1]->current_cluster,
                                             (fd_oflags[a1] & LINUX_O_NONBLOCK) != 0);
                    if (s < 0) return (uint64_t)s;
                    int64_t nfd = install_usock(fd_table, fd_flags, fd_oflags, (int)s, aflags);
                    if (nfd < 0) return (uint64_t)nfd;
                    /* the connecting side is (almost always) unnamed */
                    char name[108]; uint32_t nlen = 0;
                    usock_getname((int)s, true, name, &nlen);
                    int64_t r = sun_to_user(a2, a3, name, nlen);
                    if (r < 0) return (uint64_t)r;
                    return (uint64_t)nfd;
                }
                // Non-AF_UNIX (MVP Phase 1, no clients): just yield
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
                int ready = do_poll(fd_table, pfds, inst->num_watches, timeout_ms < 0 ? -1 : (int64_t)timeout_ms);

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
                /* kernel CSPRNG (kernel/random.c) -- these used to be raw
                   TSC readings, i.e. predictable TLS keys */
                for (uint64_t done = 0; done < len; ) {
                    uint64_t n = len - done < 256 ? len - done : 256;
                    random_bytes(buf + done, n);
                    done += n;
                }
                return len;
            }

        case 162: // SYS_sync
        case 74:  // SYS_fsync(fd)
        case 75:  // SYS_fdatasync(fd)
        case 306: // SYS_syncfs(fd)
            {
                /* the write-back cache, all of it, on the disk now (one
                   filesystem: fsync of one file flushes everything) */
                extern bool ext2_flush(void);
                return ext2_flush() ? 0 : (uint64_t)-5;   /* -EIO: not all of it reached the disk */
            }

        case 28: // SYS_madvise(addr, len, advice)
            {
                /* MADV_DONTNEED (4) / MADV_FREE (8): Linux promises that a
                   private anonymous page reads as zero afterwards, a private
                   file page as the file again -- allocators (WebKit's libpas,
                   glibc) give memory back this way and may count on it. On
                   demand-paged regions: drop the page, the next touch faults
                   a fresh one in. Shared pages keep their data (and are
                   left alone); everything else is advice and ignored. */
                if (a3 != 4 && a3 != 8) return 0;
                if (a1 & (PAGE_SIZE - 1)) return (uint64_t)-22;
                uint64_t len = (a2 + PAGE_SIZE - 1) & ~(uint64_t)(PAGE_SIZE - 1);
                if (len == 0) return 0;
                if (!user_map_range_ok(a1, len)) return (uint64_t)-22;
                PageTable *pml4 = vmm_get_current_pml4();
                uint64_t end = a1 + len;
                for (uint64_t a = vmm_next_mapped(pml4, a1, end); a < end;
                     a = vmm_next_mapped(pml4, a + PAGE_SIZE, end)) {
                    VMA *v = vma_find(proc, a);
                    if (!v || !(v->flags & VMA_LAZY)) continue;
                    if (vmm_get_pte(pml4, a) & PAGE_SHARED_MAP) continue;
                    user_unmap_page(pml4, a);
                }
                return 0;
            }

        case 35: // SYS_nanosleep (Linux standard)
            {
                struct timespec {
                    int64_t tv_sec;
                    int64_t tv_nsec;
                };
                struct timespec *req = (struct timespec *)a1;
                if (req) {
                    if (!user_check_read(a1, sizeof(*req))) return (uint64_t)-14; /* -EFAULT */
                    if (req->tv_sec < 0 || req->tv_nsec < 0 || req->tv_nsec >= 1000000000) {
                        return (uint64_t)-22; /* -EINVAL */
                    }
                    /* Real sleep on the 1 kHz clock (rounded UP to whole
                       ms, like Linux never sleeping short), not a
                       sched_yield() spin: a frame-paced loop like the
                       compositor's used to burn a full core here. */
                    extern uint64_t timer_get_ms(void);
                    /* Clamp to ~31,000 years so tv_sec*1000 and now+ms
                       can't wrap into a short sleep. */
                    uint64_t sec = (uint64_t)req->tv_sec;
                    if (sec > 1000000000000ull) sec = 1000000000000ull;
                    uint64_t ms = sec * 1000 +
                                  ((uint64_t)req->tv_nsec + 999999) / 1000000;
                    uint64_t end_ms = timer_get_ms() + ms;
                    /* Woken early by a signal (SIGKILL from exit_group/kill
                       included): -EINTR + the time left in *rem, so it is
                       acted on now instead of after the whole sleep. */
                    while (ms > 0 && timer_get_ms() < end_ms) {
                        if (sleep_interrupted()) return sleep_eintr(a2, end_ms);
                        sched_sleep_ms(end_ms - timer_get_ms());
                    }
                }
                return 0;
            }

        case 230: // SYS_clock_nanosleep(clockid, flags, req, rem)
            {
                /* glibc's nanosleep()/usleep()/sleep() all come here; it was
                   missing, so every sleep returned at once with ENOSYS. */
                extern uint64_t timer_get_ms(void);
                int clk = (int)a1;
                if (clk != 0 /* REALTIME */ && clk != 1 /* MONOTONIC */ &&
                    clk != 7 /* BOOTTIME */ && clk != 4 /* MONOTONIC_RAW */) return (uint64_t)-22;
                struct { int64_t tv_sec, tv_nsec; } ts;
                if (copy_from_user(&ts, (const void *)a3, sizeof(ts)) != 0) return (uint64_t)-14;
                if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000) return (uint64_t)-22;
                uint64_t sec = (uint64_t)ts.tv_sec;
                if (sec > 1000000000000ull) sec = 1000000000000ull;
                uint64_t req_ms = sec * 1000 + ((uint64_t)ts.tv_nsec + 999999) / 1000000;
                uint64_t now = timer_get_ms();
                uint64_t end;
                if (a2 & 1) { /* TIMER_ABSTIME: target on that clock */
                    uint64_t clock_now = now;
                    if (clk == 0) clock_now = rtc_get_unix_time_ms();
                    end = req_ms > clock_now ? now + (req_ms - clock_now) : now;
                } else {
                    end = now + req_ms;
                }
                while (timer_get_ms() < end) {
                    if (sleep_interrupted())
                        return sleep_eintr((a2 & 1) ? 0 : a4, end); /* no rem with TIMER_ABSTIME */
                    sched_sleep_ms(end - timer_get_ms());
                }
                return 0;
            }

        case 332: // SYS_statx(dirfd, path, flags, mask, struct statx *)
            {
                /* Qt (and newer glibc) try statx first; -ENOSYS cost every
                   stat a second syscall. Filled from the same data stat()
                   gives. */
                if (!a5 || !user_prepare_write(a5, 256)) return (uint64_t)-14;
                uint64_t r = syscall_dispatcher(262, a1, a2, a5, a3, 0, regs);
                if ((int64_t)r < 0) return r;
                struct linux_stat st;
                if (copy_from_user(&st, (const void *)a5, sizeof(st)) != 0) return (uint64_t)-14;
                struct {
                    uint32_t mask, blksize; uint64_t attributes;
                    uint32_t nlink, uid, gid; uint16_t mode, pad1;
                    uint64_t ino, size, blocks, attributes_mask;
                    struct { int64_t sec; uint32_t nsec; int32_t pad; } atime, btime, ctime, mtime;
                    uint32_t rdev_major, rdev_minor, dev_major, dev_minor;
                    uint64_t spare[14];
                } sx;
                _Static_assert(sizeof(sx) == 256, "struct statx layout");
                memset(&sx, 0, sizeof(sx));
                sx.mask = 0x7FF;                       /* STATX_BASIC_STATS */
                sx.blksize = (uint32_t)st.st_blksize;
                sx.nlink = (uint32_t)st.st_nlink;
                sx.uid = st.st_uid; sx.gid = st.st_gid;
                sx.mode = (uint16_t)st.st_mode;
                sx.ino = st.st_ino; sx.size = (uint64_t)st.st_size; sx.blocks = (uint64_t)st.st_blocks;
                sx.atime.sec = (int64_t)st.st_atime_sec; sx.mtime.sec = (int64_t)st.st_mtime_sec;
                sx.ctime.sec = (int64_t)st.st_ctime_sec;
                sx.rdev_major = (uint32_t)(st.st_rdev >> 8) & 0xFFF; sx.rdev_minor = (uint32_t)st.st_rdev & 0xFF;
                sx.dev_major = (uint32_t)(st.st_dev >> 8) & 0xFFF; sx.dev_minor = (uint32_t)st.st_dev & 0xFF;
                if (copy_to_user((void *)a5, &sx, sizeof(sx)) != 0) return (uint64_t)-14;
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
                return readlink_fallback(path, a2, a3);
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
                return readlink_fallback(path, a3, a4);
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
