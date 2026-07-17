#include <wynland/types.h>
#include <wynland/sched.h>
#include <wynland/vfs.h>
#include <wynland/vmm.h>
#include <wynland/pmm.h>
#include <wynland/boot_info.h>
#include <wynland/heap.h>

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
#define PIPE_BUF_SIZE 4096

typedef struct {
    uint8_t  buffer[PIPE_BUF_SIZE];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} KPipe;

static KPipe *g_pipes[MAX_PIPES];

static uint32_t pipe_write(KPipe *pipe, const void *buf, uint32_t size) {
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t written = 0;
    while (written < size) {
        if (pipe->count >= PIPE_BUF_SIZE) break;
        pipe->buffer[pipe->tail] = src[written];
        pipe->tail = (pipe->tail + 1) % PIPE_BUF_SIZE;
        pipe->count++;
        written++;
    }
    return written;
}

static uint32_t pipe_read(KPipe *pipe, void *buf, uint32_t size) {
    uint8_t *dst = (uint8_t *)buf;
    uint32_t read_bytes = 0;
    while (read_bytes < size) {
        if (pipe->count == 0) break;
        dst[read_bytes] = pipe->buffer[pipe->head];
        pipe->head = (pipe->head + 1) % PIPE_BUF_SIZE;
        pipe->count--;
        read_bytes++;
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

#define MAX_OPEN_FILES 128
static VfsFile *fd_table[MAX_OPEN_FILES] = {0};
static uint32_t fd_flags[MAX_OPEN_FILES] = {0};   /* Per-fd flags (FD_CLOEXEC etc.) */
static uint32_t fd_oflags[MAX_OPEN_FILES] = {0};   /* Per-fd open status flags */

static int get_free_fd(void) {
    for (int i = 3; i < MAX_OPEN_FILES; i++) {
        if (fd_table[i] == NULL) {
            return i;
        }
    }
    return -1;
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

uint64_t syscall_dispatcher(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, SyscallRegs *regs) {
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
            if (a1 == 0) { // stdin
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
            return vfs_read(fd_table[a1], (void *)a2, (uint32_t)a3);
            
        case 1: // SYS_write (Linux standard)
            if (a1 == 1 || a1 == 2) { // stdout/stderr
                if (a3 == 0 || !a2) return 0;
                const char *cbuf = (const char *)a2;
                for (uint64_t i = 0; i < a3; i++) {
                    char ch = cbuf[i];
                    if (g_boot_info) {
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
            return vfs_write(fd_table[a1], (const void *)a2, (uint32_t)a3);

        case 20: // SYS_writev (Linux standard)
            {
                struct iovec {
                    void  *iov_base;
                    size_t iov_len;
                } *iov = (struct iovec *)a2;
                int iovcnt = (int)a3;
                size_t total_written = 0;

                if (a1 == 1 || a1 == 2) { // stdout/stderr
                    for (int i = 0; i < iovcnt; i++) {
                        if (iov[i].iov_base && iov[i].iov_len > 0) {
                            const char *cbuf = (const char *)iov[i].iov_base;
                            for (size_t j = 0; j < iov[i].iov_len; j++) {
                                char ch = cbuf[j];
                                if (g_boot_info) {
                                    console_print_char(g_boot_info, ch, 0x00FFFFFF, term_bg_color);
                                }
                                char single[2] = {ch, '\0'};
                                extern void serial_write_string(const char *str);
                                serial_write_string(single);
                            }
                            total_written += iov[i].iov_len;
                        }
                    }
                    return total_written;
                }

                if (a1 >= MAX_OPEN_FILES || fd_table[a1] == NULL) {
                    return (uint64_t)-1;
                }
                for (int i = 0; i < iovcnt; i++) {
                    if (iov[i].iov_base && iov[i].iov_len > 0) {
                        int written = vfs_write(fd_table[a1], iov[i].iov_base, iov[i].iov_len);
                        if (written < 0) return (uint64_t)-1;
                        total_written += written;
                    }
                }
                return total_written;
            }
            
        case 2: // SYS_open (Linux standard)
            if (!a1) return (uint64_t)-2;
            {
                serial_write_string("Syscall: open path: ");
                serial_write_string((const char *)a1);
                serial_write_string("\r\n");

                int linux_flags = (int)a2;
                uint32_t vfs_flags = 0;
                if ((linux_flags & 3) == 0) vfs_flags |= 0x01; // VFS_O_READ
                if ((linux_flags & 3) == 1) vfs_flags |= 0x02; // VFS_O_WRITE
                if ((linux_flags & 3) == 2) vfs_flags |= (0x01 | 0x02);
                if (linux_flags & 0100) vfs_flags |= 0x04; // VFS_O_CREATE
                if (linux_flags & 01000) vfs_flags |= 0x10; // VFS_O_TRUNC
                if (linux_flags & 02000) vfs_flags |= 0x08; // VFS_O_APPEND

                int fd = get_free_fd();
                if (fd == -1) return (uint64_t)-2;
                VfsFile *file = vfs_open_flags((const char *)a1, vfs_flags);
                if (!file) return (uint64_t)-2;
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
            vfs_close(fd_table[a1]);
            fd_table[a1] = NULL;
            return 0;
            
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
                (void)prot;
                uint64_t flags = a4;
                int fd = (int)a5;
                uint64_t offset = regs->r9;

                uint64_t pages = (len + PAGE_SIZE - 1) / PAGE_SIZE;
                uint64_t size_aligned = pages * PAGE_SIZE;

                // Check if mapping the framebuffer character device /dev/fb0
                if (fd >= 0 && fd < MAX_OPEN_FILES && fd_table[fd] != NULL && fd_table[fd]->node.first_cluster == 0xFFFFFFF0) {
                    if (!g_boot_info) return 0;
                    uint64_t fb_phys = g_boot_info->fb_addr;
                    
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

                // Determine virtual address
                uint64_t virt_addr = addr;
                if (!(flags & 0x10) || virt_addr == 0) { // MAP_FIXED is 0x10
                    static uint64_t mmap_alloc_ptr = 0x600000000000;
                    virt_addr = mmap_alloc_ptr;
                    mmap_alloc_ptr += size_aligned;
                } else {
                    virt_addr &= ~(PAGE_SIZE - 1); // align down to page boundary
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

                // Map pages
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

                return virt_addr;
            }

        case 22: // SYS_pipe (Linux standard)
        case 293: // SYS_pipe2 (Linux standard)
            {
                serial_write_string("Syscall: pipe/pipe2 called\r\n");
                int *pipefd = (int *)a1;
                if (!pipefd) {
                    serial_write_string("  Error: pipefd is NULL\r\n");
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
                int fd_read = get_free_fd();
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

                int fd_write = get_free_fd();
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
                const char *path = (const char *)a1;
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

                int fd = get_free_fd();
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
            return (uint64_t)-38; /* -ENOSYS */

        case 10: // SYS_mprotect (Linux standard)
            return 0;

        case 11: // SYS_munmap (Linux standard)
            return 0;

        case 13: // SYS_rt_sigaction (Linux standard)
            return 0;

        case 14: // SYS_rt_sigprocmask (Linux standard)
            return 0;

        case 17: // SYS_pread64 (Linux standard)
            if (a1 >= MAX_OPEN_FILES || fd_table[a1] == NULL) {
                return (uint64_t)-9; /* -EBADF */
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
                CloneArg *ca = (CloneArg *)kmalloc(sizeof(CloneArg));
                ca->regs = *regs;
                ca->regs.rsp = a2; // Child stack pointer

                extern Thread *thread_create(void (*entry)(void*), void *arg);
                Thread *t = thread_create(clone_child_entry, ca);
                t->tls_base = a5;

                if (a1 & 0x00000100) { // CLONE_PARENT_SETTID
                    int *parent_tid = (int *)a3;
                    if (parent_tid) {
                        *parent_tid = (int)t->id;
                    }
                }
                if (a1 & 0x01000000) { // CLONE_CHILD_SETTID
                    int *child_tid = (int *)a4;
                    if (child_tid) {
                        *child_tid = (int)t->id;
                    }
                }

                return t->id;
            }
        case 59: // SYS_execve (Linux standard)
            {
                const char *user_path = (const char *)a1;
                if (!user_path) return (uint64_t)-14; /* -EFAULT */
                
                // Copy path from user space
                char kernel_path[256];
                uint32_t len = 0;
                while (user_path[len] != '\0' && len < 255) {
                    kernel_path[len] = user_path[len];
                    len++;
                }
                kernel_path[len] = '\0';

                serial_write_string("SYS_execve: Loading target: ");
                serial_write_string(kernel_path);
                serial_write_string("\r\n");

                uint64_t entry_point = 0;
                uint64_t stack_top = 0;
                PageTable *pml4 = vmm_get_current_pml4();
                

                
                #include <wynland/elf.h>
                LoadedPages *lp = kmalloc(sizeof(LoadedPages));
                if (!lp) {
                    serial_write_string("SYS_execve: out of memory for LoadedPages allocation!\r\n");
                    return (uint64_t)-12; /* -ENOMEM */
                }
                
                if (!elf_load(kernel_path, &entry_point, &stack_top, pml4, lp)) {
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

        case 202: // SYS_futex (Linux standard)
            return 0;

        case 218: // SYS_set_tid_address (Linux standard)
            return 1;

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
            
        case 5: // SYS_fstat (Linux standard) — get file status by fd
            {
                uint64_t fd = a1;
                struct linux_stat *user_stat = (struct linux_stat *)a2;
                if (!user_stat) return (uint64_t)-14; /* -EFAULT */

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
                            memset(finfo, 0, sizeof(*finfo));
                            // Set ID
                            finfo->id[0] = 'f'; finfo->id[1] = 'b'; finfo->id[2] = '0';
                            finfo->smem_start = g_boot_info->fb_addr;
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
                                v->version_major = 1;
                                v->version_minor = 0;
                                v->version_patchlevel = 0;
                                if (v->name && v->name_len > 0) {
                                    char *n = (char *)(uintptr_t)v->name;
                                    const char *drv = "virtio_gpu";
                                    for (int ki = 0; drv[ki] && (uint64_t)ki < v->name_len - 1; ki++) n[ki] = drv[ki];
                                }
                                if (v->date && v->date_len > 0) { char *d = (char *)(uintptr_t)v->date; d[0]='2'; d[1]='0'; d[2]='2'; d[3]='4'; d[4]=0; }
                                if (v->desc && v->desc_len > 0) { char *d = (char *)(uintptr_t)v->desc; d[0]='V'; d[1]='G'; d[2]='P'; d[3]='U'; d[4]=0; }
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
                            if (cap_pair) { cap_pair[1] = 0; } // cap value = 0
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
                struct iovec {
                    void  *iov_base;
                    size_t iov_len;
                } *iov = (struct iovec *)a2;
                int iovcnt = (int)a3;
                size_t total_read = 0;

                if (a1 >= MAX_OPEN_FILES || fd_table[a1] == NULL) {
                    return (uint64_t)-9; /* -EBADF */
                }
                for (int i = 0; i < iovcnt; i++) {
                    if (iov[i].iov_base && iov[i].iov_len > 0) {
                        int nread = vfs_read(fd_table[a1], iov[i].iov_base, iov[i].iov_len);
                        if (nread < 0) return (uint64_t)-1;
                        total_read += nread;
                        if ((size_t)nread < iov[i].iov_len) break; /* short read */
                    }
                }
                return total_read;
            }

        case 21: // SYS_access (Linux standard) — check file accessibility
            {
                if (!a1) return (uint64_t)-14; /* -EFAULT */
                VfsStat vst;
                if (vfs_stat((const char *)a1, &vst)) {
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
            return 1000; /* non-root user to please Hyprland */

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
                    extern uint64_t timer_get_ticks(void);
                    uint64_t ticks = timer_get_ticks();
                    tp->tv_sec = ticks / 100;
                    tp->tv_nsec = (ticks % 100) * 10000000;
                }
                return 0;
            }
        case 217: // SYS_getdents64 (Linux standard)
            {
                int fd = (int)a1;
                if (fd < 0 || fd >= MAX_OPEN_FILES || fd_table[fd] == NULL) {
                    return (uint64_t)-9; /* -EBADF */
                }
                extern int vfs_getdents(void *file, void *dirp, uint32_t count);
                return (uint64_t)vfs_getdents(fd_table[fd], (void *)a2, (uint32_t)a3);
            }

        case 257: // SYS_openat (Linux standard) — open relative to dirfd
            {
                /* If dirfd == AT_FDCWD (-100), treat as regular open */
                const char *path = (const char *)a2;
                if (!path) return (uint64_t)-14;
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

                int fd = get_free_fd();
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
                // /sys/dev/char/226:X/device/drm — virtual directory containing card0 subdir
                if (str_compare(path, "/sys/dev/char/226:0/device/drm") == 0 ||
                    str_compare(path, "/sys/dev/char/226:128/device/drm") == 0) {
                    VfsFile *sfile = kmalloc(sizeof(VfsFile));
                    memset(sfile, 0, sizeof(VfsFile));
                    sfile->node.first_cluster = 0xFFFFFFF8; // DRM_DIR virtual
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
                const char *path = (const char *)a2;
                struct linux_stat *user_stat = (struct linux_stat *)a3;
                if (!path || !user_stat) return (uint64_t)-14;

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

        case 200: // SYS_tkill (Linux standard)
        case 234: // SYS_tgkill (Linux standard)
            {
                int sig = (int)a3;
                // sig=6 is SIGABRT, sig=11 is SIGSEGV
                serial_write_string("Syscall: tkill/tgkill signal=");
                char sbuf[12];
                int si = 0;
                int sv = sig;
                if (sv < 0) { sbuf[si++] = '-'; sv = -sv; }
                if (sv == 0) { sbuf[si++] = '0'; }
                else {
                    char tmp[10]; int ti = 0;
                    while (sv > 0) { tmp[ti++] = '0' + (sv % 10); sv /= 10; }
                    for (int ri = ti-1; ri >= 0; ri--) sbuf[si++] = tmp[ri];
                }
                sbuf[si++] = '\r'; sbuf[si++] = '\n'; sbuf[si] = 0;
                serial_write_string(sbuf);
                if (sig == 6 || sig == 11 || sig == 9) {
                    thread_exit();
                }
                return 0; // ignore other signals
            }

        case 204: // SYS_sched_getparam (Linux standard)
            return (uint64_t)-38; /* -ENOSYS */

        case 324: // SYS_memfd_create (Linux standard)
            return (uint64_t)-38; /* -ENOSYS */

        case 229: // SYS_set_tid_address (Linux standard)
            return 1; // Return main thread ID 1

        case 273: // SYS_set_robust_list (Linux standard)
            return 0;

        case 157: // SYS_prctl (Linux standard)
            return 0;

        case 271: // SYS_ppoll (Linux standard)
            {
                struct pollfd {
                    int fd;
                    short events;
                    short revents;
                } *fds = (struct pollfd *)a1;
                uint64_t nfds = a2;
                struct linux_timespec {
                    int64_t tv_sec;
                    int64_t tv_nsec;
                } *tmo = (struct linux_timespec *)a3;
                (void)tmo;

                // Set all revents to 0 initially
                if (fds) {
                    for (uint64_t i = 0; i < nfds; i++) {
                        fds[i].revents = 0;
                    }
                }

                int ready = 0;
                for (uint64_t i = 0; i < nfds; i++) {
                    int fd = fds[i].fd;
                    if (fd < 0 || fd >= MAX_OPEN_FILES || fd_table[fd] == NULL) {
                        if (fds) fds[i].revents = 0x0020; // POLLNVAL
                        ready++;
                        continue;
                    }
                    VfsFile *file = fd_table[fd];
                    
                    // Check if it's eventfd or pipe
                    if (file->node.first_cluster == 0xFFFFFFFA || // Pipe Read
                        file->node.first_cluster == 0xFFFFFFFC)   // Eventfd
                    {
                        int p_idx = file->current_cluster;
                        if (p_idx >= 0 && p_idx < MAX_PIPES && g_pipes[p_idx] != NULL) {
                            KPipe *p = g_pipes[p_idx];
                            // Check if readable (there are bytes in the buffer)
                            if (p->head != p->tail) {
                                if (fds && (fds[i].events & 0x0001)) { // POLLIN
                                    fds[i].revents |= 0x0001;
                                    ready++;
                                }
                            }
                        }
                    }
                    else if (file->node.first_cluster == 0xFFFFFFFB) { // Pipe Write
                        // Pipes are always writable in our simple buffer
                        if (fds && (fds[i].events & 0x0004)) { // POLLOUT
                            fds[i].revents |= 0x0004;
                            ready++;
                        }
                    }
                    else {
                        // Regular files/devices are always readable/writable
                        if (fds) {
                            if (fds[i].events & 0x0001) fds[i].revents |= 0x0001;
                            if (fds[i].events & 0x0004) fds[i].revents |= 0x0004;
                        }
                        ready++;
                    }
                }

                if (ready > 0) {
                    return (uint64_t)ready;
                }

                // If no file descriptors are ready, yield to prevent tight busy loop
                sched_yield();
                return 0; // Timeout
            }
        case 41: // SYS_socket
            {
                // int domain = a1, int type = a2, int protocol = a3;
                int fd = get_free_fd();
                if (fd < 0) return -24; // EMFILE
                
                VfsFile *sfile = kmalloc(sizeof(VfsFile));
                memset(sfile, 0, sizeof(VfsFile));
                sfile->node.first_cluster = 0xFFFFFFF8; // Magic for AF_UNIX socket
                fd_table[fd] = sfile;
                return fd;
            }

        case 49: // SYS_bind
        case 50: // SYS_listen
        case 54: // SYS_setsockopt
        case 55: // SYS_getsockopt
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
                int fd = get_free_fd();
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
                struct epoll_event *event = (struct epoll_event *)a4;
                
                EpollInstance *inst = get_epoll_instance(epfd);
                if (!inst) return -9; // EBADF
                
                if (op == EPOLL_CTL_ADD) {
                    if (inst->num_watches >= MAX_EPOLL_EVENTS) return -12; // ENOMEM
                    inst->watches[inst->num_watches].target_fd = fd;
                    inst->watches[inst->num_watches].ev = *event;
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
                            inst->watches[i].ev = *event;
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
                
                EpollInstance *inst = get_epoll_instance(epfd);
                if (!inst) return -9; // EBADF
                
                struct pollfd {
                    int fd;
                    short events;
                    short revents;
                };
                struct pollfd pfds[MAX_EPOLL_EVENTS];
                for (int i = 0; i < inst->num_watches; i++) {
                    pfds[i].fd = inst->watches[i].target_fd;
                    pfds[i].events = 0;
                    if (inst->watches[i].ev.events & EPOLLIN) pfds[i].events |= 0x0001; // POLLIN
                    if (inst->watches[i].ev.events & EPOLLOUT) pfds[i].events |= 0x0004; // POLLOUT
                    pfds[i].revents = 0;
                }
                
                // Call our internal syscall_dispatcher for ppoll
                uint64_t ready = syscall_dispatcher(271, (uint64_t)pfds, inst->num_watches, 0, 0, 0, regs);
                
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
                const char *path = (const char *)a1;
                char *buf = (char *)a2;
                uint64_t bufsiz = a3;
                if (!path || !buf) return (uint64_t)-14;
                
                if (str_compare(path, "/sys/dev/char/226:0/device/subsystem") == 0 ||
                    str_compare(path, "/sys/dev/char/226:128/device/subsystem") == 0) {
                    const char *target = "../../../../../../bus/pci";
                    uint64_t len = 0;
                    while (target[len]) len++;
                    if (len > bufsiz) len = bufsiz;
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
                const char *path = (const char *)a2;
                char *buf = (char *)a3;
                uint64_t bufsiz = a4;
                if (!path || !buf) return (uint64_t)-14;
                
                if (str_compare(path, "/sys/dev/char/226:0/device/subsystem") == 0 ||
                    str_compare(path, "/sys/dev/char/226:128/device/subsystem") == 0) {
                    const char *target = "../../../../../../bus/pci";
                    uint64_t len = 0;
                    while (target[len]) len++;
                    if (len > bufsiz) len = bufsiz;
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
