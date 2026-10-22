/*
 * WynlandOS - the kernel log on the boot stick, as the system runs.
 *
 * A live stick (tools/livepack.c) has a 4 MB partition 3 of type 0x7F.
 * Every second what the kernel logged since (kernel/bootlog.c) is
 * appended there through the kernel's own USB storage driver, and the
 * area's first sector updated: "WYNKLOG1", u64 bytes, u32 boot number.
 * The next boot's loader copies it to \logs\klog-N.log on the stick's
 * FAT partition (boot/boot.c, KlogRescue), which any computer can read:
 * a machine whose screen, keyboard and network all fail still leaves its
 * log. The RAM ring alone was lost on a real laptop whose firmware clears
 * memory at a reset. A panic writes what is left before the reset.
 */
#include <wynland/types.h>
#include <wynland/usb.h>
#include <wynland/sched.h>
#include <wynland/spinlock.h>
#include <wynland/disklabel.h>

extern uint64_t bootlog_head(void);
extern uint32_t bootlog_copy(uint64_t *from, char *dst, uint32_t len);
extern uint32_t bootlog_boot_no(void);

#define SL_CHUNK 65536u

extern void serial_write_string(const char *s);
static void sl_say(const char *what, uint64_t v)
{
    static const char hx[] = "0123456789ABCDEF";
    char b[20];
    int n = 0;
    b[n++] = '0'; b[n++] = 'x';
    for (int i = 60; i >= 0; i -= 4) if ((v >> i) || i == 0 || n > 2) b[n++] = hx[(v >> i) & 15];
    b[n] = 0;
    serial_write_string("sticklog: ");
    serial_write_string(what);
    serial_write_string(b);
    serial_write_string("\n");
}

static uint64_t g_sl_area;          /* byte offset of the area on the stick; 0: none */
static uint64_t g_sl_cap;           /* bytes of text it takes */
static uint64_t g_sl_from;          /* the log's bytes written so far (in log terms) */
static uint64_t g_sl_len;           /* bytes of text in the area */
static volatile int g_sl_busy;

static void sl_flush(void)
{
    static char buf[SL_CHUNK];
    static uint8_t hdr[512];
    if (!g_sl_area || __sync_lock_test_and_set(&g_sl_busy, 1)) return;
    bool wrote = false;
    while (g_sl_len < g_sl_cap && g_sl_from < bootlog_head()) {
        uint32_t room = g_sl_cap - g_sl_len < SL_CHUNK ? (uint32_t)(g_sl_cap - g_sl_len) : SL_CHUNK;
        uint64_t from = g_sl_from;
        uint32_t n = bootlog_copy(&from, buf, room);
        if (!n) break;
        if (usb_storage_write_bytes(g_sl_area + 512 + g_sl_len, buf, n) < 0) {
            static int said;
            if (!said++) sl_say("writing the stick failed at byte ", g_sl_area + 512 + g_sl_len);
            break;
        }
        g_sl_from = from;
        g_sl_len += n;
        wrote = true;
    }
    if (wrote) {
        for (int i = 0; i < 512; i++) hdr[i] = 0;
        for (int i = 0; i < 8; i++) hdr[i] = (uint8_t)"WYNKLOG1"[i];
        for (int i = 0; i < 8; i++) hdr[8 + i] = (uint8_t)(g_sl_len >> (8 * i));
        uint32_t no = bootlog_boot_no();
        for (int i = 0; i < 4; i++) hdr[16 + i] = (uint8_t)(no >> (8 * i));
        usb_storage_write_bytes(g_sl_area, hdr, 512);
    }
    __sync_lock_release(&g_sl_busy);
}

static void sticklog_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < 120 && !usb_storage_ready(); i++) sched_sleep_ms(500);
    if (!usb_storage_ready()) { sl_say("no USB stick after 60 s: ", 0); return; }
    static uint8_t mbr[512];
    if (usb_storage_read_bytes(0, mbr, 512) < 0) { sl_say("reading the stick's LBA 0 failed: ", 0); return; }
    if (!wyn_disk_is_ours(mbr) || mbr[0x1DE + 4] != 0x7F) { sl_say("no log area on the stick, partition 3 type ", mbr[0x1DE + 4]); return; }
    uint32_t start = mbr[0x1DE + 8] | mbr[0x1DE + 9] << 8 | mbr[0x1DE + 10] << 16 | (uint32_t)mbr[0x1DE + 11] << 24;
    uint32_t count = mbr[0x1DE + 12] | mbr[0x1DE + 13] << 8 | mbr[0x1DE + 14] << 16 | (uint32_t)mbr[0x1DE + 15] << 24;
    if (count < 2) return;
    g_sl_cap = (uint64_t)(count - 1) * 512;
    g_sl_area = (uint64_t)start * 512;
    sl_say("the log goes to the stick, LBA ", start);
    for (;;) {
        sl_flush();
        sched_sleep_ms(1000);
    }
}

void sticklog_start(void)
{
    thread_create(sticklog_thread, NULL);
}

/* a panic: the rest now -- unless USB was busy (it may be what crashed) */
void sticklog_panic_flush(void)
{
    extern Spinlock g_usb_lock;
    if (!g_sl_area || spin_held(&g_usb_lock)) return;
    sl_flush();
}
