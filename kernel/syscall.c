#include <wynland/types.h>
#include <wynland/sched.h>
#include <wynland/vfs.h>
#include <wynland/vmm.h>
#include <wynland/pmm.h>
#include <wynland/boot_info.h>
#include <wynland/heap.h>

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

static int get_free_fd(void) {
    for (int i = 3; i < MAX_OPEN_FILES; i++) {
        if (fd_table[i] == NULL) {
            return i;
        }
    }
    return -1;
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
            
        case 2: // SYS_open (Linux standard)
            if (!a1) return (uint64_t)-1;
            {
                int fd = get_free_fd();
                if (fd == -1) return (uint64_t)-1;
                VfsFile *file = vfs_open_flags((const char *)a1, (uint32_t)a2);
                if (!file) return (uint64_t)-1;
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
                uint64_t pages = (a2 + PAGE_SIZE - 1) / PAGE_SIZE;
                uint64_t size_aligned = pages * PAGE_SIZE;
                
                // Check if mapping the framebuffer character device /dev/fb0
                if (a5 < MAX_OPEN_FILES && fd_table[a5] != NULL && fd_table[a5]->node.first_cluster == 0xFFFFFFF0) {
                    if (!g_boot_info) return 0;
                    uint64_t fb_phys = g_boot_info->fb_addr;
                    // Allocate virtual space from heap
                    void *ptr = kmalloc(size_aligned + PAGE_SIZE);
                    if (!ptr) return 0;
                    uint64_t aligned_addr = ((uint64_t)ptr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                    PageTable *pml4 = vmm_get_current_pml4();
                    for (uint64_t i = 0; i < pages; i++) {
                        uint64_t page_addr = aligned_addr + i * PAGE_SIZE;
                        uint64_t phys_addr = fb_phys + i * PAGE_SIZE;
                        vmm_map_page(pml4, page_addr, phys_addr, PAGE_WRITE | PAGE_USER);
                    }
                    return aligned_addr;
                }
                
                // Normal heap memory mmap
                void *ptr = kmalloc(size_aligned + PAGE_SIZE);
                if (!ptr) return 0;
                uint64_t aligned_addr = ((uint64_t)ptr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
                PageTable *pml4 = vmm_get_current_pml4();
                for (uint64_t i = 0; i < pages; i++) {
                    uint64_t page_addr = aligned_addr + i * PAGE_SIZE;
                    uint64_t phys_addr = vmm_get_phys(pml4, page_addr);
                    if (phys_addr != 0) {
                        vmm_map_page(pml4, page_addr, phys_addr, PAGE_WRITE | PAGE_USER);
                    }
                }
                return aligned_addr;
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

        case 60: // SYS_exit (Linux standard)
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
            
        default:
            {
                char num_str[32];
                uint_to_hex(num, num_str);
                serial_write_string("Syscall: Unknown syscall number called: ");
                serial_write_string(num_str);
                serial_write_string("\r\n");
            }
            return (uint64_t)-1;
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
