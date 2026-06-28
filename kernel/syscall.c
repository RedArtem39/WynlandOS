#include <wynland/types.h>
#include <wynland/sched.h>
#include <wynland/vfs.h>
#include <wynland/vmm.h>
#include <wynland/boot_info.h>

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

// C-level Syscall Handler
uint64_t syscall_dispatcher(uint64_t num, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5) {
    switch (num) {
        case 0: // Yield CPU
            sched_yield();
            return 0;
            
        case 1: // Print string to serial/logging
            if (a1) {
                serial_write_string((const char *)a1);
            }
            return 0;
            
        case 2: // Exit thread
            thread_exit();
            return 0;

        case 3: // Malloc (a1 = size)
            return (uint64_t)kmalloc(a1);

        case 4: // Free (a1 = ptr)
            if (a1) {
                kfree((void *)a1);
            }
            return 0;

        case 5: // Draw Rect (a1 = x, a2 = y, a3 = w, a4 = h, a5 = color)
            comp_fill_rect((uint32_t)a1, (uint32_t)a2, (uint32_t)a3, (uint32_t)a4, (uint32_t)a5);
            return 0;

        case 6: // Mark screen dirty
            comp_mark_dirty();
            return 0;

        case 10: // open(const char *path, uint64_t flags)
            if (!a1) return (uint64_t)-1;
            {
                int fd = get_free_fd();
                if (fd == -1) return (uint64_t)-1;
                VfsFile *file = vfs_open_flags((const char *)a1, (uint32_t)a2);
                if (!file) return (uint64_t)-1;
                fd_table[fd] = file;
                return fd;
            }
            
        case 11: // close(uint64_t fd)
            if (a1 >= MAX_OPEN_FILES || fd_table[a1] == NULL) {
                return (uint64_t)-1;
            }
            vfs_close(fd_table[a1]);
            fd_table[a1] = NULL;
            return 0;
            
        case 12: // read(uint64_t fd, void *buf, uint64_t size)
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
            
        case 13: // write(uint64_t fd, const void *buf, uint64_t size)
            if (a1 == 1 || a1 == 2) { // stdout/stderr
                if (a3 == 0 || !a2) return 0;
                const char *cbuf = (const char *)a2;
                for (uint64_t i = 0; i < a3; i++) {
                    char ch = cbuf[i];
                    char single[2] = {ch, '\0'};
                    serial_write_string(single);
                    if (g_boot_info) {
                        console_print_char(g_boot_info, ch, 0x00FFFFFF, term_bg_color);
                    }
                }
                return a3;
            }
            if (a1 >= MAX_OPEN_FILES || fd_table[a1] == NULL) {
                return (uint64_t)-1;
            }
            return vfs_write(fd_table[a1], (const void *)a2, (uint32_t)a3);
            
        case 14: // lseek(uint64_t fd, int32_t offset, int whence)
            if (a1 >= MAX_OPEN_FILES || fd_table[a1] == NULL) {
                return (uint64_t)-1;
            }
            return vfs_seek(fd_table[a1], (int32_t)a2, (int)a3);
            
        case 15: // mmap(uint64_t addr, uint64_t length, uint64_t prot, uint64_t flags, uint64_t fd)
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
