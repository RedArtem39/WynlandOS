#include <wynland/types.h>

#define SYS_exit 2
#define SYS_write 13
#define SYS_mmap 15

/* Helper declarations */
long syscall_raw(long num, long a1, long a2, long a3, long a4, long a5);
int sys_open(const char *path, uint64_t flags);
void sys_close(int fd);
int sys_read(int fd, void *buf, uint64_t size);
int sys_write(int fd, const void *buf, uint64_t size);
void sys_exit(int code);
void print_string(const char *msg);

/* Program entry point MUST be at the very top for flat binary entry */
extern "C" void _start() {
    const char *start_msg = "========================================\n"
                            "  WynlandOS userspace /dev/ test start  \n"
                            "  Instructions:                         \n"
                            "  - Draw with Left Mouse Button         \n"
                            "  - Exit program with Right Click       \n"
                            "========================================\n";

    print_string(start_msg);

    // 2. Open /dev/fb0
    print_string("Opening /dev/fb0...\n");
    int fb_fd = sys_open("/dev/fb0", 3); // O_RDWR
    if (fb_fd < 0) {
        print_string("Failed to open /dev/fb0!\n");
        sys_exit(1);
    }

    // 3. Map /dev/fb0
    print_string("mmap'ing /dev/fb0...\n");
    uint32_t fb_size = 1920 * 1080 * 4; // 1920x1080 resolution
    uint32_t *fb = (uint32_t*)syscall_raw(15, 0, fb_size, 0, 0, fb_fd);
    if (!fb) {
        print_string("Failed to mmap /dev/fb0!\n");
        sys_close(fb_fd);
        sys_exit(1);
    }

    // 4. Open /dev/input/mice
    print_string("Opening /dev/input/mice...\n");
    int mouse_fd = sys_open("/dev/input/mice", 1); // O_RDONLY
    if (mouse_fd < 0) {
        print_string("Failed to open /dev/input/mice!\n");
        sys_close(fb_fd);
        sys_exit(1);
    }

    print_string("Entering drawing loop! Draw on screen by holding Left Mouse Button.\n");

    uint8_t packet[3];
    int32_t cursor_x = 960;
    int32_t cursor_y = 540;

    while (1) {
        if (sys_read(mouse_fd, packet, 3) == 3) {
            // Right mouse button click triggers exit
            if (packet[0] & 2) {
                print_string("Right click detected. Exiting drawing loop!\n");
                break;
            }

            int8_t rel_x = (int8_t)packet[1];
            int8_t rel_y = (int8_t)packet[2];
            cursor_x += rel_x;
            cursor_y -= rel_y; // PS/2 Y goes up, screen Y goes down

            if (cursor_x < 0) cursor_x = 0;
            if (cursor_x >= 1920) cursor_x = 1919;
            if (cursor_y < 0) cursor_y = 0;
            if (cursor_y >= 1080) cursor_y = 1079;

            // Draw red block if Left button is pressed
            if (packet[0] & 1) {
                for (int dy = -5; dy <= 5; dy++) {
                    int py = cursor_y + dy;
                    if (py < 0 || py >= 1080) continue;
                    for (int dx = -5; dx <= 5; dx++) {
                        int px = cursor_x + dx;
                        if (px < 0 || px >= 1920) continue;
                        fb[py * 1920 + px] = 0x00FF0000; // Red pixel
                    }
                }
                // Mark compositor screen as dirty (syscall 6)
                syscall_raw(6, 0, 0, 0, 0, 0);
            }
        }
    }

    sys_close(mouse_fd);
    sys_close(fb_fd);
    print_string("Test devices completed successfully!\n");
    sys_exit(0);
}

/* Helper implementations */
long syscall_raw(long num, long a1, long a2, long a3, long a4, long a5) {
    long ret;
    __asm__ volatile(
        "movq %1, %%rax\n"
        "movq %2, %%rdi\n"
        "movq %3, %%rsi\n"
        "movq %4, %%rdx\n"
        "movq %5, %%r10\n"
        "movq %6, %%r8\n"
        "syscall\n"
        : "=a"(ret)
        : "g"(num), "g"(a1), "g"(a2), "g"(a3), "g"(a4), "g"(a5)
        : "rdi", "rsi", "rdx", "rcx", "r11", "r10", "r8", "memory"
    );
    return ret;
}

int sys_open(const char *path, uint64_t flags) {
    return (int)syscall_raw(10, (long)path, flags, 0, 0, 0);
}

void sys_close(int fd) {
    syscall_raw(11, fd, 0, 0, 0, 0);
}

int sys_read(int fd, void *buf, uint64_t size) {
    return (int)syscall_raw(12, fd, (long)buf, size, 0, 0);
}

int sys_write(int fd, const void *buf, uint64_t size) {
    return (int)syscall_raw(13, fd, (long)buf, size, 0, 0);
}

void sys_exit(int code) {
    syscall_raw(SYS_exit, code, 0, 0, 0, 0);
    while (1) {
        __asm__ volatile("hlt");
    }
}

void print_string(const char *msg) {
    int len = 0;
    while (msg[len]) len++;
    sys_write(1, msg, len);
}
