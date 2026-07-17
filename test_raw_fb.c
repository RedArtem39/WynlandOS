/*
 * test_raw_fb.c — WynlandOS Framebuffer Diagnostic Utility
 *
 * Draws a grid, color bars and corner markers directly into /dev/fb0
 * without Qt or any userspace library.
 *
 * If the grid appears correctly aligned -> problem is in Qt/QPA plugin.
 * If the grid appears skewed/diagonal  -> problem is in kernel mmap/driver.
 *
 * Build:
 *   x86_64-linux-musl-gcc -static -O2 -o test_raw_fb.elf test_raw_fb.c
 */

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <stdint.h>

/* Linux framebuffer ioctl structs (must match kernel layout) */
struct fb_var_screeninfo {
    uint32_t xres, yres;
    uint32_t xres_virtual, yres_virtual;
    uint32_t xoffset, yoffset;
    uint32_t bits_per_pixel;
    uint32_t grayscale;
    uint8_t  _pad[120]; /* rest of the struct we don't use */
};

struct fb_fix_screeninfo {
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

#define FBIOGET_VSCREENINFO 0x4600
#define FBIOGET_FSCREENINFO 0x4602

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

static void write_hex(uint64_t v) {
    char buf[19] = "0x0000000000000000";
    static const char h[] = "0123456789ABCDEF";
    for (int i = 15; i >= 2; i--) {
        buf[i] = h[v & 0xF];
        v >>= 4;
    }
    write_str(buf);
}

int main(void) {
    write_str("[test_raw_fb] Opening /dev/fb0...\n");

    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        write_str("[test_raw_fb] ERROR: Cannot open /dev/fb0\n");
        return 1;
    }
    write_str("[test_raw_fb] Opened OK. Reading display info...\n");

    struct fb_var_screeninfo vinfo;
    struct fb_fix_screeninfo finfo;

    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        write_str("[test_raw_fb] ERROR: FBIOGET_VSCREENINFO failed\n");
        return 1;
    }
    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
        write_str("[test_raw_fb] ERROR: FBIOGET_FSCREENINFO failed\n");
        return 1;
    }

    uint32_t W     = vinfo.xres;
    uint32_t H     = vinfo.yres;
    uint32_t pitch = finfo.line_length;
    uint32_t bpp   = vinfo.bits_per_pixel;
    uint64_t size  = (uint64_t)pitch * H;

    write_str("[test_raw_fb] Width:  "); write_hex(W);     write_str("\n");
    write_str("[test_raw_fb] Height: "); write_hex(H);     write_str("\n");
    write_str("[test_raw_fb] Pitch:  "); write_hex(pitch); write_str("\n");
    write_str("[test_raw_fb] BPP:    "); write_hex(bpp);   write_str("\n");
    write_str("[test_raw_fb] smem:   "); write_hex(finfo.smem_start); write_str("\n");

    if (W == 0 || H == 0 || pitch == 0) {
        write_str("[test_raw_fb] ERROR: Bad resolution from ioctl!\n");
        return 1;
    }

    uint8_t *fb = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if ((intptr_t)fb <= 0) {
        write_str("[test_raw_fb] ERROR: mmap failed\n");
        return 1;
    }
    write_str("[test_raw_fb] mmap OK at "); write_hex((uint64_t)(uintptr_t)fb); write_str("\n");

    /* ── Test 1: Solid color fill (dark navy) ── */
    write_str("[test_raw_fb] Drawing background...\n");
    for (uint32_t y = 0; y < H; y++) {
        uint32_t *row = (uint32_t *)(fb + y * pitch);
        for (uint32_t x = 0; x < W; x++)
            row[x] = 0xFF0A0E1A;
    }

    /* ── Test 2: Grid lines every 64px (bright cyan) ── */
    write_str("[test_raw_fb] Drawing alignment grid (64px cells)...\n");
    for (uint32_t y = 0; y < H; y++) {
        uint32_t *row = (uint32_t *)(fb + y * pitch);
        int h_line = (y % 64 == 0);
        for (uint32_t x = 0; x < W; x++) {
            int v_line = (x % 64 == 0);
            if (h_line || v_line)
                row[x] = 0xFF00FFCC;  /* cyan grid */
        }
    }

    /* ── Test 3: Horizontal color bars ── */
    write_str("[test_raw_fb] Drawing color bars...\n");
    uint32_t bar_colors[8] = {
        0xFFFF0000, 0xFFFF8000, 0xFFFFFF00, 0xFF00FF00,
        0xFF00FFFF, 0xFF0080FF, 0xFF8000FF, 0xFFFFFFFF
    };
    uint32_t bar_h = H / 16;
    for (int b = 0; b < 8; b++) {
        uint32_t y0 = (H * 3 / 8) + b * bar_h;
        uint32_t y1 = y0 + bar_h;
        for (uint32_t y = y0; y < y1 && y < H; y++) {
            uint32_t *row = (uint32_t *)(fb + y * pitch);
            for (uint32_t x = W / 4; x < W * 3 / 4; x++)
                row[x] = bar_colors[b];
        }
    }

    /* ── Test 4: Corner markers (red squares 32x32) ── */
    write_str("[test_raw_fb] Drawing corner markers...\n");
    uint32_t corners[4][2] = {{0,0},{W-32,0},{0,H-32},{W-32,H-32}};
    for (int c = 0; c < 4; c++) {
        for (uint32_t dy = 0; dy < 32; dy++) {
            uint32_t y = corners[c][1] + dy;
            if (y >= H) break;
            uint32_t *row = (uint32_t *)(fb + y * pitch);
            for (uint32_t dx = 0; dx < 32; dx++) {
                uint32_t x = corners[c][0] + dx;
                if (x < W) row[x] = 0xFFFF2222;
            }
        }
    }

    /* ── Test 5: Center crosshair ── */
    write_str("[test_raw_fb] Drawing crosshair at center...\n");
    uint32_t cx = W / 2, cy = H / 2;
    for (uint32_t x = cx - 40; x < cx + 40 && x < W; x++) {
        uint32_t *row = (uint32_t *)(fb + cy * pitch);
        row[x] = 0xFFFFFF00;
    }
    for (uint32_t y = cy - 40; y < cy + 40 && y < H; y++) {
        uint32_t *row = (uint32_t *)(fb + y * pitch);
        if (cx < W) row[cx] = 0xFFFFFF00;
    }

    write_str("[test_raw_fb] Done!\n");
    write_str("[test_raw_fb] PASS: If grid is straight -> kernel fb OK, check QPA plugin.\n");
    write_str("[test_raw_fb] FAIL: If grid is diagonal -> kernel mmap/pitch wrong.\n");

    /* Keep it visible — wait for any keypress via read on stdin */
    write_str("[test_raw_fb] Press Enter to exit...\n");
    char buf[1];
    read(0, buf, 1);

    munmap(fb, size);
    close(fd);
    return 0;
}
