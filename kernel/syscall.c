#include <wynland/types.h>
#include <wynland/sched.h>
#include <wynland/vfs.h>
#include <wynland/vmm.h>
#include <wynland/pmm.h>
#include <wynland/boot_info.h>
#include <wynland/heap.h>

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

    /* Special device: framebuffer /dev/fb0 */
    if (file->node.first_cluster == 0xFFFFFFF0) {
        st->st_mode = S_IFCHR | 0666;
        st->st_rdev = ((uint64_t)29 << 8) | 0; /* major 29, minor 0 */
    }
}

void clone_child_entry(void *arg) {
    typedef struct {
        uint64_t rip;
        uint64_t rsp;
        uint64_t tls;
    } CloneArg;
    CloneArg *ca = (CloneArg *)arg;
    uint64_t rip = ca->rip;
    uint64_t rsp = ca->rsp;
    kfree(ca);

    extern void thread_enter_user_mode_clone(void *rip, void *rsp);
    thread_enter_user_mode_clone((void *)rip, (void *)rsp);
}

// C-level Syscall Handler
uint64_t syscall_dispatcher(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, SyscallRegs *regs) {
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
            return vfs_read(fd_table[a1], (void *)a2, (uint32_t)a3);
            
        case 1: // SYS_write (Linux standard)
            if (a1 == 1 || a1 == 2) { // stdout/stderr
                if (a3 == 0 || !a2) return 0;
                const char *cbuf = (const char *)a2;
                for (uint64_t i = 0; i < a3; i++) {
                    char ch = cbuf[i];
                    if (g_boot_info) {
                        console_print_char(g_boot_info, ch, 0x00FFFFFF, term_bg_color);
                    } else {
                        char single[2] = {ch, '\0'};
                        extern void serial_write_string(const char *str);
                        serial_write_string(single);
                    }
                }
                return a3;
            }
            if (a1 >= MAX_OPEN_FILES || fd_table[a1] == NULL) {
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
                                } else {
                                    char single[2] = {ch, '\0'};
                                    extern void serial_write_string(const char *str);
                                    serial_write_string(single);
                                }
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

                int fd = get_free_fd();
                if (fd == -1) return (uint64_t)-2;
                VfsFile *file = vfs_open_flags((const char *)a1, (uint32_t)a2);
                if (!file) return (uint64_t)-2;
                fd_table[fd] = file;
                return fd;
            }
            
        case 3: // SYS_close (Linux standard)
            if (a1 >= MAX_OPEN_FILES || fd_table[a1] == NULL) {
                return (uint64_t)-1;
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
                        static uint64_t mmap_fb_ptr = 0x820000000000;
                        virt_addr = mmap_fb_ptr;
                        mmap_fb_ptr += size_aligned;
                    }
                    PageTable *pml4 = vmm_get_current_pml4();
                    for (uint64_t i = 0; i < pages; i++) {
                        vmm_map_page(pml4, virt_addr + i * PAGE_SIZE, fb_phys + i * PAGE_SIZE, PAGE_WRITE | PAGE_USER);
                    }
                    return virt_addr;
                }

                // Determine virtual address
                uint64_t virt_addr = addr;
                if (!(flags & 0x10) || virt_addr == 0) { // MAP_FIXED is 0x10
                    static uint64_t mmap_alloc_ptr = 0x800000000000;
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
                typedef struct {
                    uint64_t rip;
                    uint64_t rsp;
                    uint64_t tls;
                } CloneArg;

                CloneArg *ca = (CloneArg *)kmalloc(sizeof(CloneArg));
                ca->rip = regs->rip;
                ca->rsp = a2;
                ca->tls = a5;

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
                
                extern bool elf_load(const char *path, uint64_t *out_entry, uint64_t *out_stack_top, PageTable *pml4, void *out_pages);
                
                // We allocate a temporary LoadedPages structure on stack
                typedef struct {
                    void *phys_pages[512];
                    uint64_t virt_addrs[512];
                    uint32_t count;
                } LoadedPagesTemp;
                LoadedPagesTemp lp;
                
                if (!elf_load(kernel_path, &entry_point, &stack_top, pml4, &lp)) {
                    serial_write_string("SYS_execve: elf_load failed!\r\n");
                    return (uint64_t)-2; /* -ENOENT */
                }

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
                /* For ttys (fd 0/1/2), return -ENOTTY (25) for most ioctls */
                if (a1 <= 2) {
                    return (uint64_t)-25; /* -ENOTTY */
                }
                /* For regular files, also not supported */
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

        case 89: // SYS_readlink (Linux standard) — stub, no symlinks
            return (uint64_t)-22; /* -EINVAL (no symlinks supported) */

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
            return 0; /* root */

        case 110: // SYS_getppid
            return 1;

        case 186: // SYS_gettid
            {
                Thread *curr = sched_current();
                return curr ? curr->id : 1;
            }

        case 228: // SYS_clock_gettime
            {
                /* Stub: return zeroed timespec */
                struct timespec {
                    uint64_t tv_sec;
                    uint64_t tv_nsec;
                };
                struct timespec *tp = (struct timespec *)a2;
                if (tp) {
                    tp->tv_sec = 0;
                    tp->tv_nsec = 0;
                }
                return 0;
            }

        case 257: // SYS_openat (Linux standard) — open relative to dirfd
            {
                /* If dirfd == AT_FDCWD (-100), treat as regular open */
                const char *path = (const char *)a2;
                if (!path) return (uint64_t)-14;
                serial_write_string("Syscall: openat path: ");
                serial_write_string(path);
                serial_write_string("\r\n");

                int fd = get_free_fd();
                if (fd == -1) return (uint64_t)-24; /* -EMFILE */
                VfsFile *file = vfs_open_flags(path, (uint32_t)a3);
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

        case 273: // SYS_set_robust_list (Linux standard)
            return 0;

        case 318: // SYS_getrandom (Linux standard)
            {
                uint8_t *buf = (uint8_t *)a1;
                uint64_t len = a2;
                uint64_t val = 0;
                for (uint64_t i = 0; i < len; i++) {
                    if ((i % 8) == 0) {
                        __asm__ volatile("rdtsc" : "=a"(val));
                    }
                    buf[i] = (uint8_t)(val >> ((i % 8) * 8));
                }
                return len;
            }

        case 334: // SYS_rseq — restartable sequences (stub)
            return (uint64_t)-38; /* -ENOSYS */

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
