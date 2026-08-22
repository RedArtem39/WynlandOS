/*
 * test_kbd.c — WynlandOS /dev/input/kbd diagnostic utility
 *
 * Opens the new raw-scancode keyboard device (Phase 3) and polls it for
 * a while, printing every scancode it receives. Used to verify the new
 * queue (kernel/irq.c: kbd_raw_queue_push/kbd_raw_read_queue) actually
 * delivers real keypresses to Ring-3 without interfering with /dev/tty.
 *
 * Build:
 *   x86_64-linux-musl-gcc -static -O2 -o test_kbd.elf test_kbd.c
 */

#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>

static void write_str(const char *s) {
    while (*s) {
        char c = *s++;
        __asm__ volatile(
            "syscall"
            : : "a"(1), "D"(1), "S"(&c), "d"(1)
            : "rcx", "r11", "memory"
        );
    }
}

static void write_hex8(uint8_t v) {
    char buf[5] = "0x00";
    static const char h[] = "0123456789ABCDEF";
    buf[2] = h[(v >> 4) & 0xF];
    buf[3] = h[v & 0xF];
    write_str(buf);
}

int main(void) {
    write_str("[test_kbd] opening /dev/input/kbd...\n");
    int fd = open("/dev/input/kbd", O_RDONLY);
    if (fd < 0) {
        write_str("[test_kbd] open failed\n");
        return 1;
    }
    write_str("[test_kbd] opened OK, polling for scancodes...\n");

    int got = 0;
    for (long i = 0; i < 300000000L && got < 5; i++) {
        uint8_t sc;
        long n = read(fd, &sc, 1);
        if (n > 0) {
            write_str("[test_kbd] got scancode ");
            write_hex8(sc);
            write_str("\n");
            got++;
        }
    }

    write_str("[test_kbd] done, total scancodes: ");
    write_str(got > 0 ? "some\n" : "none\n");
    close(fd);
    return 0;
}
