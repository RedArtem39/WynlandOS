/*
 * WynlandOS - UEFI Bootloader
 * Copyright (c) 2026 WynlandOS Project
 *
 * Phase 1 bootloader: initializes graphics, loads kernel ELF,
 * retrieves ACPI RSDP, exits boot services, and jumps to kernel.
 *
 * Compiled with: x86_64-w64-mingw32-gcc (MS ABI is default)
 */

#include "uefi.h"
#include "elf.h"
#include <wynland/boot_info.h>
#include <wynland/bootlog.h>
#include <wynland/disklabel.h>
#include <wynland/logo_draw.h>

/* ============================================================
 * Global State
 * ============================================================ */

static EFI_SYSTEM_TABLE  *gST;
static EFI_BOOT_SERVICES *gBS;
static EFI_HANDLE         gImageHandle;

/* ============================================================
 * Helper: Print - output a UCS-2 string via ConOut
 * ============================================================ */

/* while the boot screen (the logo) is up, the text stays off it -- unless
   something went wrong: an ERROR brings the text back */
static int g_quiet;
static int g_fb_ram;          /* a BLT-only GOP: the "framebuffer" is RAM nobody shows */

/* everything printed, also what the boot screen hides: saved to
   \logs\loader-N.log on a live stick (LiveLoad) */
static UINT8 g_ldr_log[16384];
static UINTN g_ldr_len;

static void Print(CHAR16 *str) {
    for (CHAR16 *c = str; *c && g_ldr_len < sizeof g_ldr_log; c++)
        if (*c != L'\r') g_ldr_log[g_ldr_len++] = *c < 128 ? (UINT8)*c : '?';
    CHAR16 *s = str;
    while (*s == L' ' || *s == L'\r' || *s == L'\n') s++;
    if (g_quiet && s[0] == L'E' && s[1] == L'R' && s[2] == L'R') {
        g_quiet = 0;
        gST->ConOut->ClearScreen(gST->ConOut);
    }
    if (g_quiet) return;
    gST->ConOut->OutputString(gST->ConOut, str);
}

/* ============================================================
 * Helper: PrintHex - print a 64-bit value in hexadecimal
 * ============================================================ */

static void PrintHex(UINT64 value) {
    CHAR16 buf[19]; /* "0x" + 16 hex digits + null */
    CHAR16 hexChars[] = L"0123456789ABCDEF";
    int i;

    buf[0] = L'0';
    buf[1] = L'x';
    for (i = 15; i >= 0; i--) {
        buf[2 + (15 - i)] = hexChars[(value >> (i * 4)) & 0xF];
    }
    buf[18] = L'\0';

    Print(buf);
}

/* ============================================================
 * Helper: memset - freestanding, no library dependency
 * ============================================================ */

void* memset(void *dst, int val, UINTN size) {
    UINT8 *p = (UINT8*)dst;
    UINTN i;
    for (i = 0; i < size; i++) {
        p[i] = (UINT8)val;
    }
    return dst;
}

/* ============================================================
 * Helper: memcpy - freestanding, no library dependency
 * ============================================================ */

void* memcpy(void *dst, const void *src, UINTN size) {
    UINT8 *d = (UINT8*)dst;
    const UINT8 *s = (const UINT8*)src;
    UINTN i;
    for (i = 0; i < size; i++) {
        d[i] = s[i];
    }
    return dst;
}

/* ============================================================
 * Helper: CompareGuid - compare two EFI_GUIDs
 * Returns 0 if equal, non-zero otherwise
 * ============================================================ */

static int CompareGuid(EFI_GUID *a, EFI_GUID *b) {
    UINT8 *pa = (UINT8*)a;
    UINT8 *pb = (UINT8*)b;
    UINTN i;
    for (i = 0; i < sizeof(EFI_GUID); i++) {
        if (pa[i] != pb[i]) return 1;
    }
    return 0;
}

/* ============================================================
 * InitGraphics - locate GOP protocol and set best video mode
 * ============================================================ */

/*
 * ReadResolution - the wanted screen size from \resolution.cfg on the boot
 * volume ("1920x1080"). Defaults to 1920x1080 when the file is missing or
 * unreadable. Uses the same boot-volume access LoadKernel() does.
 */
static void ReadResolution(UINT32 *want_w, UINT32 *want_h) {
    *want_w = 1920;
    *want_h = 1080;

    EFI_LOADED_IMAGE_PROTOCOL *loadedImage;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fileSystem;
    EFI_FILE_PROTOCOL *rootDir, *f;
    EFI_GUID loadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID fileSystemGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    if (EFI_IS_ERROR(gBS->HandleProtocol(gImageHandle, &loadedImageGuid, (VOID**)&loadedImage))) return;
    if (EFI_IS_ERROR(gBS->HandleProtocol(loadedImage->DeviceHandle, &fileSystemGuid, (VOID**)&fileSystem))) return;
    if (EFI_IS_ERROR(fileSystem->OpenVolume(fileSystem, &rootDir))) return;
    if (EFI_IS_ERROR(rootDir->Open(rootDir, &f, L"\\resolution.cfg", EFI_FILE_MODE_READ, 0))) {
        rootDir->Close(rootDir);
        return;
    }
    UINT8 buf[32];
    UINTN n = sizeof(buf) - 1;
    if (!EFI_IS_ERROR(f->Read(f, &n, buf))) {
        buf[n] = 0;
        /* "native": the mode the firmware is in (a laptop's panel) */
        if (n >= 6 && buf[0] == 'n' && buf[1] == 'a' && buf[2] == 't') {
            *want_w = *want_h = 0;
            f->Close(f);
            rootDir->Close(rootDir);
            return;
        }
        UINT32 w = 0, h = 0, *cur = &w;
        for (UINTN i = 0; i < n; i++) {
            if (buf[i] >= '0' && buf[i] <= '9') *cur = *cur * 10 + (UINT32)(buf[i] - '0');
            else if ((buf[i] == 'x' || buf[i] == 'X') && cur == &w) cur = &h;
            else if (cur == &h && h) break;
        }
        if (w >= 640 && h >= 480 && w <= 7680 && h <= 4320) { *want_w = w; *want_h = h; }
    }
    f->Close(f);
    rootDir->Close(rootDir);
}

/*
 * The previous boot's log (include/wynland/bootlog.h): when it ended in a
 * panic -- or never said it ended -- it goes to \logs on the boot volume,
 * written by the firmware's file system driver. Then this boot's log is
 * claimed: the first BOOTLOG_ADDRS range the firmware still has free.
 * First of all, before anything else allocates.
 */
static void PutDec(CHAR16 *out, UINT32 v)
{
    CHAR16 tmp[12];
    int n = 0;
    do { tmp[n++] = (CHAR16)(L'0' + v % 10); v /= 10; } while (v && n < 11);
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

/* \logs\<kind><no>.log on the boot volume: a, then b */
static void WriteLogFile(const CHAR16 *kind, UINT32 no, const UINT8 *a, UINTN alen, const UINT8 *b, UINTN blen)
{
    EFI_LOADED_IMAGE_PROTOCOL *loadedImage;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fileSystem;
    EFI_FILE_PROTOCOL *root, *dir, *f;
    EFI_GUID loadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID fileSystemGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    if (EFI_IS_ERROR(gBS->HandleProtocol(gImageHandle, &loadedImageGuid, (VOID**)&loadedImage))) return;
    if (EFI_IS_ERROR(gBS->HandleProtocol(loadedImage->DeviceHandle, &fileSystemGuid, (VOID**)&fileSystem))) return;
    if (EFI_IS_ERROR(fileSystem->OpenVolume(fileSystem, &root))) return;
    if (EFI_IS_ERROR(root->Open(root, &dir, L"\\logs", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                                EFI_FILE_DIRECTORY))) {
        root->Close(root);
        return;
    }
    CHAR16 name[48];
    int n = 0;
    while (kind[n]) { name[n] = kind[n]; n++; }
    PutDec(name + n, no);
    while (name[n]) n++;
    const CHAR16 *ext = L".log";
    for (int i = 0; ext[i]; i++) name[n++] = ext[i];
    name[n] = 0;
    if (!EFI_IS_ERROR(dir->Open(dir, &f, name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) {
        if (alen) f->Write(f, &alen, (VOID *)a);
        if (blen) f->Write(f, &blen, (VOID *)b);
        f->Flush(f);
        f->Close(f);
        Print(L"  saved a log: \\logs\\");
        Print(name);
        Print(L"\r\n");
    }
    dir->Close(dir);
    root->Close(root);
}

static void SaveLog(BootLog *l)
{
    EFI_LOADED_IMAGE_PROTOCOL *loadedImage;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fileSystem;
    EFI_FILE_PROTOCOL *root, *dir, *f;
    EFI_GUID loadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID fileSystemGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    if (EFI_IS_ERROR(gBS->HandleProtocol(gImageHandle, &loadedImageGuid, (VOID**)&loadedImage))) return;
    if (EFI_IS_ERROR(gBS->HandleProtocol(loadedImage->DeviceHandle, &fileSystemGuid, (VOID**)&fileSystem))) return;
    if (EFI_IS_ERROR(fileSystem->OpenVolume(fileSystem, &root))) return;
    if (EFI_IS_ERROR(root->Open(root, &dir, L"\\logs", EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE,
                                EFI_FILE_DIRECTORY))) {
        root->Close(root);
        return;
    }
    /* \logs\panic-<boot>.log, unclean-<boot>.log (reset without a word),
       or boot-<boot>.log (a live stick keeps every boot's) */
    CHAR16 name[48];
    const CHAR16 *kind = l->state == BOOTLOG_PANIC ? L"panic-"
                       : l->state == BOOTLOG_CLEAN ? L"boot-" : L"unclean-";
    int n = 0;
    while (kind[n]) { name[n] = kind[n]; n++; }
    PutDec(name + n, l->boot_no);
    while (name[n]) n++;
    const CHAR16 *ext = L".log";
    for (int i = 0; ext[i]; i++) name[n++] = ext[i];
    name[n] = 0;
    if (!EFI_IS_ERROR(dir->Open(dir, &f, name, EFI_FILE_MODE_READ | EFI_FILE_MODE_WRITE | EFI_FILE_MODE_CREATE, 0))) {
        /* the ring in order: the oldest byte first */
        UINT64 size = l->size, head = l->head;
        UINT64 start = head > size ? head % size : 0, len = head > size ? size : head;
        UINTN a = (UINTN)(start + len > size ? size - start : len);
        UINTN b = (UINTN)(len - a);
        if (a) f->Write(f, &a, l->data + start);
        if (b) f->Write(f, &b, l->data);
        f->Flush(f);
        f->Close(f);
        Print(L"  saved the last boot's log: \\logs\\");
        Print(name);
        Print(L"\r\n");
    }
    dir->Close(dir);
    root->Close(root);
}

static int BootFileExists(CHAR16 *name);

/* what BootLogSetup() found, shown by LiveLoad(): setting the screen mode
   in between clears whatever was printed */
static UINT64 g_log_prev, g_log_now;
static UINT32 g_boot_no;
static UINT32 g_log_prev_state;

static UINT64 BootLogSetup(void)
{
    UINT32 boot_no = 1;
    /* a live stick is for trying a machine out: every boot's log is kept */
    int keep_all = BootFileExists(L"\\live");
    for (int i = 0; i < BOOTLOG_NADDRS; i++) {
        BootLog *l = (BootLog *)(UINTN)BOOTLOG_ADDRS[i];
        if (!bootlog_valid(l, BOOTLOG_ADDRS[i])) continue;
        boot_no = l->boot_no + 1;
        g_log_prev = BOOTLOG_ADDRS[i];
        g_log_prev_state = l->state;
        if (l->state != BOOTLOG_CLEAN || keep_all) SaveLog(l);
        l->magic = 0;                      /* read: never saved twice */
    }
    for (int i = 0; i < BOOTLOG_NADDRS; i++) {
        EFI_PHYSICAL_ADDRESS a = BOOTLOG_ADDRS[i];
        if (EFI_IS_ERROR(gBS->AllocatePages(AllocateAddress, EfiReservedMemoryType, BOOTLOG_BYTES / 4096, &a))) continue;
        BootLog *l = (BootLog *)(UINTN)a;
        memset(l, 0, sizeof(BootLog));
        l->magic = BOOTLOG_MAGIC;
        l->self = a;
        l->size = BOOTLOG_BYTES - sizeof(BootLog);
        l->state = BOOTLOG_RUNNING;
        l->boot_no = boot_no;
        l->check = bootlog_check(l);
        g_log_now = a;
        g_boot_no = boot_no;
        return a;
    }
    return 0;
}

/*
 * The live boot (\live on the boot volume: make live): the system's disk
 * -- the one labelled WYNLANDOS-DISK (include/wynland/disklabel.h) --
 * from LBA 0 to the end of its root partition, read into RAM with the
 * firmware's own driver (a USB stick's too). The kernel then needs no disk
 * driver of its own to start, and the machine's own disks stay untouched.
 */
static int BootFileExists(CHAR16 *name)
{
    EFI_LOADED_IMAGE_PROTOCOL *loadedImage;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fileSystem;
    EFI_FILE_PROTOCOL *root, *f;
    EFI_GUID loadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID fileSystemGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    if (EFI_IS_ERROR(gBS->HandleProtocol(gImageHandle, &loadedImageGuid, (VOID**)&loadedImage))) return 0;
    if (EFI_IS_ERROR(gBS->HandleProtocol(loadedImage->DeviceHandle, &fileSystemGuid, (VOID**)&fileSystem))) return 0;
    if (EFI_IS_ERROR(fileSystem->OpenVolume(fileSystem, &root))) return 0;
    int found = !EFI_IS_ERROR(root->Open(root, &f, name, EFI_FILE_MODE_READ, 0));
    if (found) f->Close(f);
    root->Close(root);
    return found;
}

/*
 * The boot screen: the logo (include/wynland/logo_draw.h) winds in, then
 * a ring of light runs over it and a thin bar under it fills while the
 * system is read. Any key -- or anything going wrong -- brings the text
 * back. Frames are drawn between reads: the reads come in 1 MB pieces.
 */
static struct {
    int      on;
    UINT64   tsc0, per_ms, last_ms;
    UINT32  *buf;
    int      size, x, y, bar_y, bar_h;
    BootInfo *bi;
} g_ui;

static UINT64 Rdtsc(void)
{
    UINT32 lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((UINT64)hi << 32) | lo;
}

static void UiPut(int x, int y, UINT32 rgb)
{
    BootInfo *bi = g_ui.bi;
    if (!bi->fb_format) rgb = (rgb & 0xFF00u) | ((rgb >> 16) & 0xFFu) | ((rgb & 0xFFu) << 16);
    *(volatile UINT32 *)(UINTN)(bi->fb_addr + (UINT64)y * bi->fb_pitch + (UINT64)x * 4) = rgb;
}

static void UiStart(BootInfo *bi)
{
    if (g_fb_ram || bi->fb_bpp != 32 || !bi->fb_addr) return;
    int size = (int)(bi->fb_height * 3 / 10) & ~1;
    if (size < 128) size = 128;
    if (size > LOGO_MAX_SIZE) size = LOGO_MAX_SIZE;
    if (EFI_IS_ERROR(gBS->AllocatePool(EfiLoaderData, (UINTN)size * size * 4, (VOID **)&g_ui.buf))) return;
    UINT64 t = Rdtsc();
    gBS->Stall(20000);
    g_ui.per_ms = (Rdtsc() - t) / 20;
    if (!g_ui.per_ms) return;
    g_ui.bi = bi;
    g_ui.size = size;
    g_ui.x = (int)(bi->fb_width - size) / 2;
    g_ui.y = (int)(bi->fb_height - size) / 2 - (int)bi->fb_height / 24;
    g_ui.bar_h = size / 100 + 2;
    g_ui.bar_y = g_ui.y + size + size / 6;
    for (UINT32 y = 0; y < bi->fb_height; y++)
        for (UINT32 x = 0; x < bi->fb_width; x++) UiPut((int)x, (int)y, 0);
    g_ui.tsc0 = Rdtsc();
    g_ui.last_ms = 0;
    g_ui.on = 1;
    g_quiet = 1;
}

/* back to the text: a key, or an error */
static void UiOff(void)
{
    if (!g_ui.on) return;
    g_ui.on = 0;
    g_quiet = 0;
    gST->ConOut->ClearScreen(gST->ConOut);
}

/* a frame, if one is due (~30 a second); done/total: the bar */
static void UiFrame(UINT64 done, UINT64 total, int rest)
{
    if (!g_ui.on) return;
    EFI_INPUT_KEY key;
    if (!rest && gST->ConIn && !EFI_IS_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &key))) {
        UiOff();
        Print(L"[live] reading the system into memory (a key: the text)... ");
        return;
    }
    UINT64 ms = (Rdtsc() - g_ui.tsc0) / g_ui.per_ms;
    if (!rest && ms - g_ui.last_ms < 33 && g_ui.last_ms) return;
    g_ui.last_ms = ms ? ms : 1;
    INT32 glow = 0;
    if (!rest && ms > LOGO_ASSEMBLE_MS) {
        glow = (INT32)((ms - LOGO_ASSEMBLE_MS) / 3);
        if (glow > 256) glow = 256;
    }
    logo_draw(g_ui.buf, g_ui.size, rest ? 1u << 30 : (UINT32)ms, glow, (UINT32)(ms - LOGO_ASSEMBLE_MS));
    for (int y = 0; y < g_ui.size; y++)
        for (int x = 0; x < g_ui.size; x++) UiPut(g_ui.x + x, g_ui.y + y, g_ui.buf[y * g_ui.size + x]);
    /* the bar: a dim track, the part done bright; it fades in with the logo */
    int fill = total ? (int)(done * (UINT64)g_ui.size / total) : 0;
    UINT32 fade = ms >= LOGO_ASSEMBLE_MS ? 256 : (UINT32)(ms * 256 / LOGO_ASSEMBLE_MS);
    for (int y = 0; y < g_ui.bar_h; y++)
        for (int x = 0; x < g_ui.size; x++) {
            UINT32 r = x < fill ? 0x50 : 0x10, g = x < fill ? 0xB0 : 0x1C, b = x < fill ? 0xFF : 0x3A;
            UiPut(g_ui.x + x, g_ui.bar_y + y, ((r * fade >> 8) << 16) | ((g * fade >> 8) << 8) | (b * fade >> 8));
        }
}

/* an LZ4 block (tools/livepack.c packs them): its size unpacked, or -1 */
static INT64 Lz4Unpack(const UINT8 *s, UINTN n, UINT8 *d, UINTN cap)
{
    const UINT8 *se = s + n;
    UINT8 *dp = d, *de = d + cap;
    while (s < se) {
        UINT8 t = *s++;
        UINTN l = t >> 4;
        if (l == 15) {
            UINT8 b;
            do { if (s >= se) return -1; b = *s++; l += b; } while (b == 255);
        }
        if (l > (UINTN)(se - s) || l > (UINTN)(de - dp)) return -1;
        memcpy(dp, s, l);
        dp += l;
        s += l;
        if (s >= se) break;                     /* the last literals */
        if (se - s < 2) return -1;
        UINTN off = s[0] | (UINTN)s[1] << 8;
        s += 2;
        if (!off || off > (UINTN)(dp - d)) return -1;
        UINTN m = t & 15;
        if (m == 15) {
            UINT8 b;
            do { if (s >= se) return -1; b = *s++; m += b; } while (b == 255);
        }
        m += 4;
        if (m > (UINTN)(de - dp)) return -1;
        const UINT8 *r = dp - off;
        while (m--) *dp++ = *r++;               /* may overlap: byte by byte */
    }
    return dp - d;
}

/* The packed live stick (tools/livepack.c): partition 2 holds the system
   in RAMDISK_CHUNK pieces, LZ4 or all-zero, each unpacked straight into
   its piece of RAM. Only what is there is read: the minutes a whole raw
   2 GB took through a firmware's USB driver become seconds. */
/* a read of the stick; a failed one tried again (a USB stick, a firmware
   driver may fail one now and then), then reported with its status */
static int ReadRetry(EFI_BLOCK_IO_PROTOCOL *disk, UINT64 lba, UINTN bytes, VOID *buf)
{
    EFI_STATUS st = 0;
    for (int i = 0; i < 4; i++) {
        st = disk->ReadBlocks(disk, disk->Media->MediaId, lba, bytes, buf);
        if (!EFI_IS_ERROR(st)) return 1;
        gBS->Stall(100000);
    }
    UiOff();
    Print(L"[live] reading the stick failed, 4 times: LBA ");
    PrintHex(lba);
    Print(L", bytes ");
    PrintHex(bytes);
    Print(L", status ");
    PrintHex(st);
    Print(L"\r\n");
    return 0;
}

/* the stick is read into these, below 4 GB (LiveLoadInner takes them
   before the system's 2 GB use the low memory up) */
static EFI_PHYSICAL_ADDRESS g_low_table, g_low_piece;

/* a piece's sum (tools/livepack.c piece_sum: the same) */
static UINT32 PieceSum(const UINT8 *p)
{
    UINT32 h = 0x811C9DC5u;
    const UINT64 *w = (const UINT64 *)p;
    for (UINTN i = 0; i < RAMDISK_CHUNK / 8; i++)
        h = (h ^ (UINT32)w[i] ^ (UINT32)(w[i] >> 32)) * 16777619u;
    return h;
}

static int LivePacked(EFI_BLOCK_IO_PROTOCOL *disk, UINT64 at, UINT64 *chunks, UINT64 nchunks, UINT64 bytes, UINT8 *head)
{
    UINT32 tsec = (UINT32)((nchunks * 8 + 511) / 512);
    EFI_PHYSICAL_ADDRESS tbuf = 0, pbuf = 0;
    if (*(UINT64 *)(head + 8) != bytes || *(UINT32 *)(head + 20) != RAMDISK_CHUNK) {
        Print(L"[live] the pack's header does not match\r\n");
        return 0;
    }
    /* what the stick is read into lies below 4 GB: a firmware USB driver
       that DMAs above it may hand back wrong data without an error */
    tbuf = g_low_table;
    pbuf = g_low_piece;
    if (!tbuf || !pbuf || tsec * 512 > 65536) return 0;
    UINT32 *sizes = (UINT32 *)(UINTN)tbuf;
    if (!ReadRetry(disk, at + 1, (UINTN)tsec * 512, sizes)) return 0;
    UINT64 lba = at + 1 + tsec, packed = 0;
    for (UINT64 c = 0; c < nchunks; c++) packed += (sizes[2 * c] + 511) / 512 * 512;
    Print(L"[live] reading the system into memory (");
    PrintHex(packed >> 20);
    Print(L" MB packed, ");
    PrintHex(bytes >> 20);
    Print(L" MB unpacked, hex)... ");
    UINT32 shown = 0;
    UINT64 read = 0;
    for (UINT64 c = 0; c < nchunks; c++) {
        UINT8 *dst = (UINT8 *)(UINTN)chunks[c];
        UINT32 sz = sizes[2 * c], sum = sizes[2 * c + 1], secs = (sz + 511) / 512;
        if (!sz) {
            memset(dst, 0, RAMDISK_CHUNK);
        } else if (sz > RAMDISK_CHUNK) {
            UiOff();
            Print(L"[live] the pack's table is damaged at piece ");
            PrintHex(c);
            Print(L"\r\n");
            return 0;
        } else {
            UINT8 *src = (UINT8 *)(UINTN)pbuf;
            /* read, unpacked and summed; a piece that comes out wrong is
               read again: a laptop's firmware handed back wrong data once
               in a while, with no error, from a stick that held it right */
            int good = 0;
            for (int attempt = 0; attempt < 4 && !good; attempt++) {
                /* 1 MB at a time: a frame of the boot screen in between */
                for (UINT32 s = 0; s < secs; ) {
                    UINT32 n = secs - s < 2048 ? secs - s : 2048;
                    if (!ReadRetry(disk, lba + s, (UINTN)n * 512, src + (UINTN)s * 512)) {
                        UiOff();
                        Print(L"[live] ... at piece ");
                        PrintHex(c);
                        Print(L"\r\n");
                        return 0;
                    }
                    s += n;
                    if (!attempt) read += (UINT64)n * 512;
                    UiFrame(read, packed, 0);
                }
                if (sz == RAMDISK_CHUNK) memcpy(dst, src, RAMDISK_CHUNK);   /* stored as is */
                int unpacked = sz == RAMDISK_CHUNK || Lz4Unpack(src, sz, dst, RAMDISK_CHUNK) == RAMDISK_CHUNK;
                good = unpacked && PieceSum(dst) == sum;
                if (!good) {
                    Print(L"\r\n[live] piece ");
                    PrintHex(c);
                    Print(unpacked ? L" read wrong (its sum), try " : L" read wrong (does not unpack), try ");
                    PrintHex((UINT64)attempt + 1);
                    Print(L": LBA ");
                    PrintHex(lba);
                    Print(L", sectors ");
                    PrintHex(secs);
                    Print(L", buffer at ");
                    PrintHex((UINT64)(UINTN)src);
                    Print(L"\r\n");
                }
            }
            if (!good) {
                UiOff();
                Print(L"\r\n[live] piece ");
                PrintHex(c);
                Print(L" stays wrong after 4 reads\r\n");
                return 0;
            }
        }
        lba += secs;
        UINT32 pct = (UINT32)((c + 1) * 20 / nchunks);
        while (shown < pct) { shown++; Print(L"#"); }
    }
    /* the logo at rest, the bar full: what stays up until the kernel draws */
    UiFrame(packed, packed, 1);
    return 1;
}

/* The kernel's log of the last boot from the stick's log area (sector 0:
   "WYNKLOG1", u64 bytes, u32 boot number; then the text) into
   \logs\klog-N.log, the area then marked empty. A machine whose screen
   or network fails still leaves its log here, for any computer to read. */
static void KlogRescue(EFI_BLOCK_IO_PROTOCOL *disk, UINT32 start, UINT32 count)
{
    static UINT8 hdr[512];
    if (count < 2 || EFI_IS_ERROR(disk->ReadBlocks(disk, disk->Media->MediaId, start, 512, hdr))) return;
    for (int i = 0; i < 8; i++) if (hdr[i] != (UINT8)"WYNKLOG1"[i]) return;
    UINT64 len = *(UINT64 *)(hdr + 8);
    UINT32 no = *(UINT32 *)(hdr + 16);
    if (!len || len > (UINT64)(count - 1) * 512) return;
    UINTN bytes = (UINTN)((len + 511) / 512 * 512);
    UINT8 *data = NULL;
    if (EFI_IS_ERROR(gBS->AllocatePool(EfiLoaderData, bytes, (VOID **)&data))) return;
    if (!EFI_IS_ERROR(disk->ReadBlocks(disk, disk->Media->MediaId, start + 1, bytes, data)))
        WriteLogFile(L"klog-", no, data, (UINTN)len, NULL, 0);
    gBS->FreePool(data);
    for (int i = 0; i < 512; i++) hdr[i] = 0;
    if (disk->WriteBlocks) disk->WriteBlocks(disk, disk->Media->MediaId, start, 512, hdr);
}

static void LiveLoadInner(BootInfo *info);

/* The live boot, and what the loader said on the way kept as
   \logs\loader-N.log. If the system did not come into memory the kernel
   would start without it (a blank, garbled screen): the reason stays on
   the screen until a key instead. */
static void LiveLoad(BootInfo *info)
{
    if (!BootFileExists(L"\\live")) return;
    LiveLoadInner(info);
    if (!info->ramdisk_addr) {
        UiOff();
        /* what was said behind the boot screen: its last lines, again */
        {
            static CHAR16 line[1601];
            UINTN from = g_ldr_len > 1600 ? g_ldr_len - 1600 : 0, n = 0;
            for (UINTN i = from; i < g_ldr_len; i++) {
                line[n++] = g_ldr_log[i];
                if (g_ldr_log[i] == '\n') { line[n - 1] = L'\r'; line[n++] = L'\n'; }
                if (n >= 1598) break;
            }
            line[n] = 0;
            gST->ConOut->OutputString(gST->ConOut, line);
        }
        Print(L"\r\n[live] THE SYSTEM DID NOT COME INTO MEMORY: the reason is above. Any key to go on.\r\n");
        WriteLogFile(L"loader-", g_boot_no, g_ldr_log, g_ldr_len, NULL, 0);
        EFI_INPUT_KEY key;
        for (int i = 0; i < 6000; i++) {          /* 10 minutes at most */
            if (gST->ConIn && !EFI_IS_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &key))) break;
            gBS->Stall(100000);
        }
        return;
    }
    WriteLogFile(L"loader-", g_boot_no, g_ldr_log, g_ldr_len, NULL, 0);
}

static void LiveLoadInner(BootInfo *info)
{
    if (!BootFileExists(L"\\live")) return;
    if (g_log_prev) {
        Print(L"[log] the last boot's log was found at ");
        PrintHex(g_log_prev);
        Print(g_log_prev_state == BOOTLOG_PANIC ? L" (a panic)\r\n"
              : g_log_prev_state == BOOTLOG_CLEAN ? L" (a clean end)\r\n" : L" (it never ended)\r\n");
    } else {
        Print(L"[log] no log from the last boot (powered off, or the firmware cleared the RAM)\r\n");
    }
    if (g_log_now) { Print(L"[log] this boot's log at "); PrintHex(g_log_now); Print(L"\r\n"); }
    else Print(L"[log] no room for this boot's log: the firmware has every address\r\n");
    Print(L"[live] looking for the WynlandOS disk...\r\n");
    EFI_GUID bioGuid = EFI_BLOCK_IO_PROTOCOL_GUID;
    UINTN n = 0;
    EFI_HANDLE *hs = NULL;
    if (EFI_IS_ERROR(gBS->LocateHandleBuffer(2 /* ByProtocol */, &bioGuid, NULL, &n, &hs))) {
        Print(L"[live] no block devices: booting from the disk instead\r\n");
        return;
    }
    UINT8 *sec = NULL;
    if (EFI_IS_ERROR(gBS->AllocatePool(EfiLoaderData, 4096, (VOID**)&sec))) return;
    EFI_BLOCK_IO_PROTOCOL *disk = NULL;
    for (UINTN i = 0; i < n && !disk; i++) {
        EFI_BLOCK_IO_PROTOCOL *b;
        if (EFI_IS_ERROR(gBS->HandleProtocol(hs[i], &bioGuid, (VOID**)&b))) continue;
        if (!b->Media || !b->Media->MediaPresent || b->Media->LogicalPartition || b->Media->BlockSize != 512) continue;
        if (EFI_IS_ERROR(b->ReadBlocks(b, b->Media->MediaId, 0, 512, sec))) continue;
        if (wyn_disk_is_ours(sec)) disk = b;
    }
    if (!disk) {
        Print(L"[live] no disk labelled WYNLANDOS-DISK: booting from the disk instead\r\n");
        return;
    }
    /* the root partition (the second, type 0x83): its end is where to stop;
       or (type 0xDA) the pack of it, which says how big the system is */
    UINT32 start = *(UINT32 *)(sec + 0x1CE + 8), count = *(UINT32 *)(sec + 0x1CE + 12);
    UINT64 blocks = (UINT64)start + count;
    int packed = sec[0x1CE + 4] == 0xDA;
    if ((sec[0x1CE + 4] != 0x83 && !packed) || !count || blocks > disk->Media->LastBlock + 1) {
        Print(L"[live] the disk's partition table is not WynlandOS's\r\n");
        return;
    }
    /* partition 3 (type 0x7F, tools/livepack.c): the kernel's own log of
       the last boot, written there as it ran (kernel/sticklog.c) */
    if (sec[0x1DE + 4] == 0x7F) KlogRescue(disk, *(UINT32 *)(sec + 0x1DE + 8), *(UINT32 *)(sec + 0x1DE + 12));
    UINT8 head[512];
    if (packed) {
        int ok = !EFI_IS_ERROR(disk->ReadBlocks(disk, disk->Media->MediaId, start, 512, head));
        for (int i = 0; ok && i < 8; i++) ok = head[i] == (UINT8)"WYNPACK2"[i];
        if (!ok) {
            Print(L"[live] the system's pack is missing or damaged\r\n");
            return;
        }
        blocks = *(UINT64 *)(head + 8) / 512;
    }
    if (packed) {
        g_low_table = g_low_piece = 0xFFFFFFFFull;
        if (EFI_IS_ERROR(gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, 16, &g_low_table)) ||
            EFI_IS_ERROR(gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, RAMDISK_CHUNK / 4096, &g_low_piece))) {
            g_low_table = g_low_piece = 0;
            Print(L"[live] no memory below 4 GB to read the stick into\r\n");
            return;
        }
    }
    /* in RAMDISK_CHUNK pieces, anywhere: a few GB in one piece is often
       not there, even with 16 GB of RAM. A table of their addresses. */
    UINT64 bytes = blocks * 512;
    const UINT64 per = RAMDISK_CHUNK / 512;        /* blocks a piece */
    UINT64 nchunks = (blocks + per - 1) / per;
    UINT64 tpages = (nchunks * 8 + 4095) / 4096;
    EFI_PHYSICAL_ADDRESS table = 0;
    if (EFI_IS_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiReservedMemoryType, tpages, &table))) return;
    UINT64 *chunks = (UINT64 *)(UINTN)table;
    if (!packed) {
        Print(L"[live] reading the system into memory (");
        PrintHex(bytes >> 20);
        Print(L" MB, hex)... ");
    }
    UINT32 shown = 0;
    for (UINT64 c = 0; c < nchunks; c++) {
        UINT64 lba = c * per, nb = blocks - lba < per ? blocks - lba : per;
        EFI_PHYSICAL_ADDRESS a = 0;
        int ok = !EFI_IS_ERROR(gBS->AllocatePages(AllocateAnyPages, EfiReservedMemoryType, RAMDISK_CHUNK / 4096, &a));
        if (ok && !packed) ok = !EFI_IS_ERROR(disk->ReadBlocks(disk, disk->Media->MediaId, lba, (UINTN)(nb * 512), (VOID *)(UINTN)a));
        if (!ok) {
            Print(L"\r\n[live] not enough memory, or a read failed: booting from the disk instead\r\n");
            if (a) gBS->FreePages(a, RAMDISK_CHUNK / 4096);
            while (c--) gBS->FreePages(chunks[c], RAMDISK_CHUNK / 4096);
            gBS->FreePages(table, tpages);
            return;
        }
        chunks[c] = a;
        if (packed) continue;
        UINT32 pct = (UINT32)((c + 1) * 20 / nchunks);
        while (shown < pct) { shown++; Print(L"#"); }
    }
    if (packed) UiStart(info);
    if (packed && !LivePacked(disk, start, chunks, nchunks, bytes, head)) {
        Print(L"\r\n[live] reading the system failed\r\n");
        for (UINT64 c = 0; c < nchunks; c++) gBS->FreePages(chunks[c], RAMDISK_CHUNK / 4096);
        gBS->FreePages(table, tpages);
        return;
    }
    Print(L" done\r\n");
    info->ramdisk_addr = table;
    info->ramdisk_size = bytes;
}

static EFI_STATUS InitGraphics(BootInfo *info) {
    EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
    EFI_GUID gopGuid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    EFI_STATUS status;
    UINT32 bestMode = 0;
    UINT32 bestWidth = 0;
    UINT32 bestHeight = 0;
    UINT32 modeNum;

    /* Locate the Graphics Output Protocol */
    status = gBS->LocateProtocol(&gopGuid, (VOID*)0, (VOID**)&gop);
    if (EFI_IS_ERROR(status)) {
        Print(L"  ERROR: Could not locate GOP\r\n");
        return status;
    }

    /* Enumerate all available modes and find a standard crisp resolution (prefer exactly 2560x1440 first, then 1920x1080, then 1280x720, then 1024x768) */
    UINT32 found2560 = 0;
    UINT32 mode2560 = 0;
    UINT32 found1920 = 0;
    UINT32 mode1920 = 0;
    UINT32 found1280 = 0;
    UINT32 mode1280 = 0;
    UINT32 found1024 = 0;
    UINT32 mode1024 = 0;

    for (modeNum = 0; modeNum < gop->Mode->MaxMode; modeNum++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *modeInfo;
        UINTN infoSize;

        status = gop->QueryMode(gop, modeNum, &infoSize, &modeInfo);
        if (EFI_IS_ERROR(status)) {
            continue;
        }

        /* PixelBltOnly modes are fine: a BLT-only GOP (OVMF's virtio-gpu)
           gets a RAM framebuffer below. Skipping them left no mode at all
           and fell back to mode 0 (640x480). */

        UINT32 w = modeInfo->HorizontalResolution;
        UINT32 h = modeInfo->VerticalResolution;

        if (w == 2560 && h == 1440) {
            found2560 = 1;
            mode2560 = modeNum;
        }
        if (w == 1920 && h == 1080) {
            found1920 = 1;
            mode1920 = modeNum;
        }
        if (w == 1280 && h == 720) {
            found1280 = 1;
            mode1280 = modeNum;
        }
        if (w == 1024 && h == 768) {
            found1024 = 1;
            mode1024 = modeNum;
        }

        /* Fallback: pick the largest mode that is <= 2560x1600 */
        if (w <= 2560 && h <= 1600) {
            if (w > bestWidth || (w == bestWidth && h > bestHeight)) {
                bestMode = modeNum;
                bestWidth = w;
                bestHeight = h;
            }
        }
    }

    /* \resolution.cfg (default 1920x1080): that exact mode, else the
       largest one that fits inside it; the fixed preference list below is
       only the fallback when nothing fits. */
    UINT32 wantW, wantH, cfgMode = 0, cfgW = 0, cfgH = 0;
    ReadResolution(&wantW, &wantH);
    for (modeNum = 0; modeNum < gop->Mode->MaxMode; modeNum++) {
        EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi;
        UINTN miSize;
        if (EFI_IS_ERROR(gop->QueryMode(gop, modeNum, &miSize, &mi))) continue;
        UINT32 w = mi->HorizontalResolution, h = mi->VerticalResolution;
        if (w > wantW || h > wantH) continue;
        if ((UINT64)w * h > (UINT64)cfgW * cfgH) { cfgMode = modeNum; cfgW = w; cfgH = h; }
    }

    if (!wantW) {
        /* native: the screen's own size from its EDID (the first detailed
           timing is the preferred one) when the GOP offers that mode, else
           its largest mode -- a laptop's firmware may sit in a small one */
        UINT32 ew = 0, eh = 0;
        EFI_GUID edidGuids[2] = { EFI_EDID_ACTIVE_PROTOCOL_GUID, EFI_EDID_DISCOVERED_PROTOCOL_GUID };
        UINTN nh = 0;
        EFI_HANDLE *hs = NULL;
        if (!EFI_IS_ERROR(gBS->LocateHandleBuffer(2 /* ByProtocol */, &gopGuid, NULL, &nh, &hs))) {
            for (UINTN i = 0; i < nh && !ew; i++) {
                for (int g = 0; g < 2 && !ew; g++) {
                    EFI_EDID_PROTOCOL *edid;
                    if (EFI_IS_ERROR(gBS->HandleProtocol(hs[i], &edidGuids[g], (VOID**)&edid))) continue;
                    if (!edid->Edid || edid->SizeOfEdid < 128) continue;
                    const UINT8 *d = edid->Edid + 54;
                    if (!d[0] && !d[1]) continue;          /* not a timing */
                    ew = d[2] | ((UINT32)(d[4] & 0xF0) << 4);
                    eh = d[5] | ((UINT32)(d[7] & 0xF0) << 4);
                }
            }
        }
        if (ew) {
            Print(L"  The screen's own size (EDID): ");
            PrintHex(ew); Print(L" x "); PrintHex(eh); Print(L"\r\n");
        }
        for (modeNum = 0; modeNum < gop->Mode->MaxMode; modeNum++) {
            EFI_GRAPHICS_OUTPUT_MODE_INFORMATION *mi;
            UINTN miSize;
            if (EFI_IS_ERROR(gop->QueryMode(gop, modeNum, &miSize, &mi))) continue;
            UINT32 w = mi->HorizontalResolution, h = mi->VerticalResolution;
            if (ew && w == ew && h == eh) { cfgMode = modeNum; cfgW = w; cfgH = h; break; }
            if (!ew || cfgW != ew) {
                if ((UINT64)w * h > (UINT64)cfgW * cfgH && w <= 3840 && h <= 2400) { cfgMode = modeNum; cfgW = w; cfgH = h; }
            }
        }
    }
    if (cfgW) {
        bestMode = cfgMode;
        bestWidth = cfgW;
        bestHeight = cfgH;
    } else if (found1024) {
        bestMode = mode1024;
        bestWidth = 1024;
        bestHeight = 768;
    } else if (found1280) {
        bestMode = mode1280;
        bestWidth = 1280;
        bestHeight = 720;
    } else if (found1920) {
        bestMode = mode1920;
        bestWidth = 1920;
        bestHeight = 1080;
    } else if (found2560) {
        bestMode = mode2560;
        bestWidth = 2560;
        bestHeight = 1440;
    }

    /* Fallback if no moderate resolution mode is found */
    if (bestWidth == 0) {
        bestMode = 0;
    }

    /* Set the best mode */
    status = gop->SetMode(gop, bestMode);
    if (EFI_IS_ERROR(status)) {
        Print(L"  ERROR: Failed to set GOP mode\r\n");
        return status;
    }

    /* Fill in the BootInfo framebuffer fields */
    info->fb_addr   = gop->Mode->FrameBufferBase;
    info->fb_width  = gop->Mode->Info->HorizontalResolution;
    info->fb_height = gop->Mode->Info->VerticalResolution;
    info->fb_pitch  = gop->Mode->Info->PixelsPerScanLine * 4; /* 4 bytes per pixel (32bpp) */
    info->fb_bpp    = 32;

    if (gop->Mode->Info->PixelFormat == PixelBlueGreenRedReserved8BitPerColor) {
        info->fb_format = 1; /* BGR */
    } else {
        info->fb_format = 0; /* RGB */
    }

    /* A BLT-only GOP (OVMF's virtio-gpu driver, e.g. QEMU with -vga none)
       has no linear framebuffer: FrameBufferBase is 0. The kernel would
       then identity-map "the framebuffer" over physical 0.. -- right over
       its own code at 1 MB, with NX -- and die on the CR3 switch. Give it
       a RAM framebuffer instead (EfiReservedMemoryType: the kernel's page
       allocator never hands it out). Nothing scans it out until the
       virtio-gpu driver takes over the display. */
    if (gop->Mode->Info->PixelFormat == PixelBltOnly || gop->Mode->FrameBufferBase == 0) {
        UINTN fbPages = (UINTN)(((UINT64)info->fb_pitch * info->fb_height + 4095) / 4096);
        EFI_PHYSICAL_ADDRESS fbAddr = 0;
        status = gBS->AllocatePages(AllocateAnyPages, EfiReservedMemoryType, fbPages, &fbAddr);
        if (EFI_IS_ERROR(status)) {
            Print(L"  ERROR: BLT-only GOP and no memory for a framebuffer\r\n");
            return status;
        }
        UINT8 *p = (UINT8 *)(UINTN)fbAddr;
        for (UINTN k = 0; k < fbPages * 4096; k++) p[k] = 0;
        info->fb_addr = fbAddr;
        info->fb_format = 1; /* BGR, like every virtio-gpu 2D format we use */
        g_fb_ram = 1;
        Print(L"  GOP is BLT-only: using a RAM framebuffer\r\n");
    }

    /* Print selected resolution */
    Print(L"  Graphics: ");
    PrintHex(info->fb_width);
    Print(L" x ");
    PrintHex(info->fb_height);
    Print(L"\r\n");

    return EFI_SUCCESS;
}

/* ============================================================
 * The kernel slot: partition 4 of a live stick (type 0x7E, 16 MB,
 * tools/livepack.c). Sector 0: "WYNKERN1", u32 bytes, u32 sum (FNV-1a of
 * them), 64 bytes of text (what it is); then the kernel's ELF. Written
 * by test-wyn k (rootfs/usr/bin/test-wyn) from the developer's machine
 * (make devkernel, tools/kslot.py): a new kernel without flashing the
 * stick again. A key within 2 s boots the stick's own \kernel.elf.
 * ============================================================ */

static EFI_BLOCK_IO_PROTOCOL *FindOurDisk(UINT8 *sec)
{
    EFI_GUID bioGuid = EFI_BLOCK_IO_PROTOCOL_GUID;
    UINTN n = 0;
    EFI_HANDLE *hs = NULL;
    if (EFI_IS_ERROR(gBS->LocateHandleBuffer(2 /* ByProtocol */, &bioGuid, NULL, &n, &hs))) return NULL;
    for (UINTN i = 0; i < n; i++) {
        EFI_BLOCK_IO_PROTOCOL *b;
        if (EFI_IS_ERROR(gBS->HandleProtocol(hs[i], &bioGuid, (VOID**)&b))) continue;
        if (!b->Media || !b->Media->MediaPresent || b->Media->LogicalPartition || b->Media->BlockSize != 512) continue;
        if (EFI_IS_ERROR(b->ReadBlocks(b, b->Media->MediaId, 0, 512, sec))) continue;
        if (wyn_disk_is_ours(sec)) return b;
    }
    return NULL;
}

static UINT8 *SlotKernel(UINTN *size)
{
    static UINT8 sec[512], hdr[512];
    EFI_BLOCK_IO_PROTOCOL *disk = FindOurDisk(sec);
    if (!disk || sec[0x1EE + 4] != 0x7E) return NULL;
    UINT32 start = *(UINT32 *)(sec + 0x1EE + 8), count = *(UINT32 *)(sec + 0x1EE + 12);
    if (count < 2 || EFI_IS_ERROR(disk->ReadBlocks(disk, disk->Media->MediaId, start, 512, hdr))) return NULL;
    for (int i = 0; i < 8; i++) if (hdr[i] != (UINT8)"WYNKERN1"[i]) return NULL;
    UINT32 bytes = *(UINT32 *)(hdr + 8), sum = *(UINT32 *)(hdr + 12);
    if (!bytes || bytes > (count - 1) * 512u) return NULL;
    UINTN rounded = (bytes + 511) / 512 * 512;
    UINT8 *buf = NULL;
    if (EFI_IS_ERROR(gBS->AllocatePool(EfiLoaderData, rounded, (VOID **)&buf))) return NULL;
    if (EFI_IS_ERROR(disk->ReadBlocks(disk, disk->Media->MediaId, start + 1, rounded, buf))) { gBS->FreePool(buf); return NULL; }
    UINT32 h = 2166136261u;
    for (UINT32 i = 0; i < bytes; i++) h = (h ^ buf[i]) * 16777619u;
    if (h != sum) {
        Print(L"  [kernel] the slot's kernel is damaged (its sum): the stick's own\r\n");
        gBS->FreePool(buf);
        return NULL;
    }
    CHAR16 what[65];
    int n = 0;
    for (; n < 64 && hdr[16 + n] >= 32 && hdr[16 + n] < 127; n++) what[n] = hdr[16 + n];
    what[n] = 0;
    Print(L"  [kernel] the slot's kernel: ");
    Print(what);
    Print(L"\r\n  [kernel] booting it -- any key within 2 s: the stick's own instead\r\n");
    EFI_INPUT_KEY key;
    if (gST->ConIn) while (!EFI_IS_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &key))) ;   /* old keys */
    for (int i = 0; i < 20; i++) {
        if (gST->ConIn && !EFI_IS_ERROR(gST->ConIn->ReadKeyStroke(gST->ConIn, &key))) {
            Print(L"  [kernel] the stick's own kernel\r\n");
            gBS->FreePool(buf);
            return NULL;
        }
        gBS->Stall(100000);
    }
    *size = bytes;
    return buf;
}

/* ============================================================
 * LoadKernel - load kernel.elf from the boot volume
 * ============================================================ */

static EFI_STATUS LoadKernel(Elf64_Addr *entry_point) {
    EFI_STATUS status;
    EFI_LOADED_IMAGE_PROTOCOL *loadedImage;
    EFI_SIMPLE_FILE_SYSTEM_PROTOCOL *fileSystem;
    EFI_FILE_PROTOCOL *rootDir;
    EFI_FILE_PROTOCOL *kernelFile;
    EFI_GUID loadedImageGuid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID fileSystemGuid = EFI_SIMPLE_FILE_SYSTEM_PROTOCOL_GUID;
    EFI_GUID fileInfoGuid = EFI_FILE_INFO_GUID;
    UINT8 *fileBuffer;
    UINTN fileSize;
    Elf64_Ehdr *ehdr;
    Elf64_Phdr *phdr;
    UINT16 i;

    /* Get EFI_LOADED_IMAGE_PROTOCOL from our image handle */
    status = gBS->HandleProtocol(gImageHandle, &loadedImageGuid, (VOID**)&loadedImage);
    if (EFI_IS_ERROR(status)) {
        Print(L"  ERROR: Could not get LoadedImage protocol\r\n");
        return status;
    }

    /* Get EFI_SIMPLE_FILE_SYSTEM_PROTOCOL from the device we booted from */
    status = gBS->HandleProtocol(loadedImage->DeviceHandle, &fileSystemGuid, (VOID**)&fileSystem);
    if (EFI_IS_ERROR(status)) {
        Print(L"  ERROR: Could not get FileSystem protocol\r\n");
        return status;
    }

    /* Open the root directory of the boot volume */
    status = fileSystem->OpenVolume(fileSystem, &rootDir);
    if (EFI_IS_ERROR(status)) {
        Print(L"  ERROR: Could not open root volume\r\n");
        return status;
    }

    /* Open kernel.elf */
    status = rootDir->Open(rootDir, &kernelFile, L"\\kernel.elf", EFI_FILE_MODE_READ, 0);
    if (EFI_IS_ERROR(status)) {
        Print(L"  ERROR: Could not open \\kernel.elf\r\n");
        rootDir->Close(rootDir);
        return status;
    }

    /* Get file size using GetInfo */
    {
        UINT8 infoBuffer[256];
        UINTN infoSize = sizeof(infoBuffer);
        EFI_FILE_INFO *finfo;

        status = kernelFile->GetInfo(kernelFile, &fileInfoGuid, &infoSize, infoBuffer);
        if (EFI_IS_ERROR(status)) {
            Print(L"  ERROR: Could not get kernel file info\r\n");
            kernelFile->Close(kernelFile);
            rootDir->Close(rootDir);
            return status;
        }

        finfo = (EFI_FILE_INFO*)infoBuffer;
        fileSize = (UINTN)finfo->FileSize;
    }

    Print(L"  Kernel file size: ");
    PrintHex(fileSize);
    Print(L" bytes\r\n");

    /* Allocate buffer and read the entire file */
    status = gBS->AllocatePool(EfiLoaderData, fileSize, (VOID**)&fileBuffer);
    if (EFI_IS_ERROR(status)) {
        Print(L"  ERROR: Could not allocate memory for kernel\r\n");
        kernelFile->Close(kernelFile);
        rootDir->Close(rootDir);
        return status;
    }

    {
        UINTN readSize = fileSize;
        status = kernelFile->Read(kernelFile, &readSize, fileBuffer);
        if (EFI_IS_ERROR(status)) {
            Print(L"  ERROR: Could not read kernel file\r\n");
            gBS->FreePool(fileBuffer);
            kernelFile->Close(kernelFile);
            rootDir->Close(rootDir);
            return status;
        }
    }

    /* the kernel in the stick's slot instead (test-wyn k, leaf update -eK),
       unless a key says the stick's own */
    {
        UINTN slotSize = 0;
        UINT8 *slot = SlotKernel(&slotSize);
        if (slot) {
            gBS->FreePool(fileBuffer);
            fileBuffer = slot;
            fileSize = slotSize;
        }
    }

    /* Parse ELF header */
    ehdr = (Elf64_Ehdr*)fileBuffer;

    /* Verify ELF magic */
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        Print(L"  ERROR: Invalid ELF magic\r\n");
        gBS->FreePool(fileBuffer);
        kernelFile->Close(kernelFile);
        rootDir->Close(rootDir);
        return EFI_LOAD_ERROR;
    }

    /* Verify ELF class (64-bit) */
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
        Print(L"  ERROR: Not a 64-bit ELF\r\n");
        gBS->FreePool(fileBuffer);
        kernelFile->Close(kernelFile);
        rootDir->Close(rootDir);
        return EFI_LOAD_ERROR;
    }

    /* Verify machine type (x86_64) */
    if (ehdr->e_machine != EM_X86_64) {
        Print(L"  ERROR: Not an x86_64 ELF\r\n");
        gBS->FreePool(fileBuffer);
        kernelFile->Close(kernelFile);
        rootDir->Close(rootDir);
        return EFI_LOAD_ERROR;
    }

    /* Verify type (executable) */
    if (ehdr->e_type != ET_EXEC) {
        Print(L"  ERROR: ELF is not an executable\r\n");
        gBS->FreePool(fileBuffer);
        kernelFile->Close(kernelFile);
        rootDir->Close(rootDir);
        return EFI_LOAD_ERROR;
    }

    Print(L"  ELF header valid, loading segments...\r\n");

    /* Load each PT_LOAD segment */
    phdr = (Elf64_Phdr*)(fileBuffer + ehdr->e_phoff);
    for (i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            UINTN numPages = (UINTN)((phdr[i].p_memsz + 0xFFF) / 0x1000);
            EFI_PHYSICAL_ADDRESS segAddr = phdr[i].p_paddr;

            /* Allocate pages at the exact physical address */
            status = gBS->AllocatePages(AllocateAddress, EfiLoaderData, numPages, &segAddr);
            if (EFI_IS_ERROR(status)) {
                Print(L"  ERROR: Could not allocate pages for segment at ");
                PrintHex(phdr[i].p_paddr);
                Print(L"\r\n");
                gBS->FreePool(fileBuffer);
                kernelFile->Close(kernelFile);
                rootDir->Close(rootDir);
                return status;
            }

            /* Copy segment data from file buffer to physical memory */
            memcpy((VOID*)segAddr, fileBuffer + phdr[i].p_offset, (UINTN)phdr[i].p_filesz);

            /* Zero out BSS (p_memsz > p_filesz) */
            if (phdr[i].p_memsz > phdr[i].p_filesz) {
                memset((VOID*)(segAddr + phdr[i].p_filesz), 0,
                       (UINTN)(phdr[i].p_memsz - phdr[i].p_filesz));
            }

            Print(L"  Loaded segment: ");
            PrintHex(segAddr);
            Print(L" (");
            PrintHex(phdr[i].p_memsz);
            Print(L" bytes)\r\n");
        }
    }

    /* Set the kernel entry point */
    *entry_point = ehdr->e_entry;
    Print(L"  Kernel entry point: ");
    PrintHex(*entry_point);
    Print(L"\r\n");

    /* Cleanup */
    kernelFile->Close(kernelFile);
    rootDir->Close(rootDir);
    gBS->FreePool(fileBuffer);

    Print(L"  Kernel loaded successfully!\r\n");
    return EFI_SUCCESS;
}

/* ============================================================
 * FindRSDP - locate the ACPI RSDP in the configuration tables
 * ============================================================ */

static VOID* FindRSDP(void) {
    EFI_GUID acpi20Guid = EFI_ACPI_20_TABLE_GUID;
    EFI_GUID acpi10Guid = EFI_ACPI_TABLE_GUID;
    UINTN i;

    /* First pass: look for ACPI 2.0+ table (preferred) */
    for (i = 0; i < gST->NumberOfTableEntries; i++) {
        if (CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &acpi20Guid) == 0) {
            Print(L"  Found ACPI 2.0 RSDP at ");
            PrintHex((UINT64)gST->ConfigurationTable[i].VendorTable);
            Print(L"\r\n");
            return gST->ConfigurationTable[i].VendorTable;
        }
    }

    /* Second pass: fall back to ACPI 1.0 table */
    for (i = 0; i < gST->NumberOfTableEntries; i++) {
        if (CompareGuid(&gST->ConfigurationTable[i].VendorGuid, &acpi10Guid) == 0) {
            Print(L"  Found ACPI 1.0 RSDP at ");
            PrintHex((UINT64)gST->ConfigurationTable[i].VendorTable);
            Print(L"\r\n");
            return gST->ConfigurationTable[i].VendorTable;
        }
    }

    Print(L"  WARNING: ACPI RSDP not found!\r\n");
    return (VOID*)0;
}

/* ============================================================
 * ExitBootServicesAndGetMemoryMap
 * Retrieves the UEFI memory map, exits boot services, and
 * converts the map to WynlandOS MemoryRegion format.
 * ============================================================ */

static EFI_STATUS ExitBootServicesAndGetMemoryMap(BootInfo *info) {
    EFI_STATUS status;
    UINTN mmapSize = 8192; /* Start with 8KB buffer */
    UINTN mapKey;
    UINTN descSize;
    UINT32 descVersion;
    UINT8 *mmapBuffer;
    MemoryRegion *regions;
    UINTN regionCount;
    UINT64 totalUsable;
    UINTN numEntries;
    UINTN i;

    /* Allocate the memory map buffer */
    status = gBS->AllocatePool(EfiLoaderData, mmapSize, (VOID**)&mmapBuffer);
    if (EFI_IS_ERROR(status)) {
        Print(L"  ERROR: Could not allocate memory map buffer\r\n");
        return status;
    }

    /* Get the memory map - may need to retry with larger buffer */
    status = gBS->GetMemoryMap(&mmapSize, (EFI_MEMORY_DESCRIPTOR*)mmapBuffer,
                               &mapKey, &descSize, &descVersion);
    if (status == EFI_BUFFER_TOO_SMALL) {
        gBS->FreePool(mmapBuffer);
        /* Add extra space for map changes caused by our allocation */
        mmapSize += 2 * descSize;
        status = gBS->AllocatePool(EfiLoaderData, mmapSize, (VOID**)&mmapBuffer);
        if (EFI_IS_ERROR(status)) {
            Print(L"  ERROR: Could not allocate larger memory map buffer\r\n");
            return status;
        }
        status = gBS->GetMemoryMap(&mmapSize, (EFI_MEMORY_DESCRIPTOR*)mmapBuffer,
                                   &mapKey, &descSize, &descVersion);
    }

    if (EFI_IS_ERROR(status)) {
        Print(L"  ERROR: GetMemoryMap failed\r\n");
        gBS->FreePool(mmapBuffer);
        return status;
    }

    /* Try to exit boot services - retry if map key changed */
    status = gBS->ExitBootServices(gImageHandle, mapKey);
    if (EFI_IS_ERROR(status)) {
        /* Map key changed, retry once */
        mmapSize = 8192 + 4096; /* Use a larger buffer */
        /* Note: we cannot call AllocatePool after a failed ExitBootServices reliably,
           so we reuse the existing buffer if it's large enough, or retry with the
           existing allocation */
        status = gBS->GetMemoryMap(&mmapSize, (EFI_MEMORY_DESCRIPTOR*)mmapBuffer,
                                   &mapKey, &descSize, &descVersion);
        if (EFI_IS_ERROR(status)) {
            return status;
        }
        status = gBS->ExitBootServices(gImageHandle, mapKey);
        if (EFI_IS_ERROR(status)) {
            return status;
        }
    }

    /*
     * === BOOT SERVICES ARE NOW TERMINATED ===
     * We can no longer call any Boot Services functions.
     * Only the pre-allocated static buffers are available.
     */

    /* Convert EFI memory map to WynlandOS MemoryRegion format */
    regions = (MemoryRegion*)info->mmap_addr;
    regionCount = 0;
    totalUsable = 0;
    numEntries = mmapSize / descSize;

    for (i = 0; i < numEntries && regionCount < 512; i++) {
        EFI_MEMORY_DESCRIPTOR *desc = (EFI_MEMORY_DESCRIPTOR*)(mmapBuffer + i * descSize);
        UINT32 type;
        UINT64 regionSize = desc->NumberOfPages * 4096;

        /* Map EFI memory types to WynlandOS memory types */
        switch (desc->Type) {
            case EfiConventionalMemory:
                type = MEMORY_USABLE;
                totalUsable += regionSize;
                break;
            case EfiACPIReclaimMemory:
                type = MEMORY_ACPI_RECLAIMABLE;
                break;
            case EfiACPINVS:
                type = MEMORY_ACPI_NVS;
                break;
            case EfiLoaderCode:
            case EfiLoaderData:
                type = MEMORY_BOOTLOADER;
                break;
            case EfiBootServicesCode:
            case EfiBootServicesData:
                /* Boot services memory is reclaimable after ExitBootServices */
                type = MEMORY_USABLE;
                totalUsable += regionSize;
                break;
            default:
                type = MEMORY_RESERVED;
                break;
        }

        regions[regionCount].base        = desc->PhysicalStart;
        regions[regionCount].size        = regionSize;
        regions[regionCount].type        = type;
        regions[regionCount].reserved    = 0;
        regionCount++;
    }

    info->mmap_entries = (UINT32)regionCount;
    info->total_memory = totalUsable;

    return EFI_SUCCESS;
}

/* ============================================================
 * Page tables for the jump into the kernel
 *
 * The kernel is linked at KERNEL_VMA (0xFFFFFFFF80000000) + its physical
 * address and reaches physical memory through HHDM_BASE + address (see
 * include/wynland/memlayout.h); UEFI's own tables have neither. These map,
 * with 2 MB pages: the first 64 GB at itself (this loader's code, stack and
 * the BootInfo it hands over) and at HHDM_BASE, and the first 1 GB at
 * KERNEL_VMA. The kernel builds its real tables in vmm_init(); these stay
 * in loader memory, which the kernel never reuses.
 * ============================================================ */

#define BOOT_MAP_GB 64

static EFI_STATUS BuildKernelPageTables(UINT64 *pml4_out) {
    UINTN pages = 1 + 1 + 1 + BOOT_MAP_GB;     /* PML4, two PDPTs, the PDs */
    EFI_PHYSICAL_ADDRESS base = 0xFFFFFFFF;     /* below 4 GB */
    EFI_STATUS status = gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, pages, &base);
    if (EFI_IS_ERROR(status)) return status;
    memset((VOID*)(UINTN)base, 0, pages * 4096);

    UINT64 *pml4 = (UINT64*)(UINTN)base;
    UINT64 *pdpt_low = pml4 + 512;
    UINT64 *pdpt_kernel = pdpt_low + 512;
    UINT64 *pds = pdpt_kernel + 512;
    for (UINTN g = 0; g < BOOT_MAP_GB; g++) {
        UINT64 *pd = pds + g * 512;
        for (UINTN m = 0; m < 512; m++)
            pd[m] = (((UINT64)g << 30) + ((UINT64)m << 21)) | 0x83;  /* present, write, 2 MB */
        pdpt_low[g] = (UINT64)(UINTN)pd | 0x03;
    }
    pdpt_kernel[510] = (UINT64)(UINTN)pds | 0x03;    /* 0xFFFFFFFF80000000: the first GB */
    pml4[0]   = (UINT64)(UINTN)pdpt_low | 0x03;      /* identity */
    pml4[256] = (UINT64)(UINTN)pdpt_low | 0x03;      /* 0xFFFF800000000000 */
    pml4[511] = (UINT64)(UINTN)pdpt_kernel | 0x03;
    *pml4_out = base;
    return EFI_SUCCESS;
}

/* ============================================================
 * EfiMain - UEFI application entry point
 * ============================================================ */

EFI_STATUS EFIAPI EfiMain(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    EFI_STATUS status;
    Elf64_Addr kernelEntry;

    /* Store global references */
    gST = SystemTable;
    gBS = SystemTable->BootServices;
    gImageHandle = ImageHandle;

    /* Disable the 5-minute watchdog timer */
    gBS->SetWatchdogTimer(0, 0, 0, (CHAR16*)0);

    /* Clear screen */
    gST->ConOut->ClearScreen(gST->ConOut);

    Print(L"WynlandOS UEFI Bootloader v0.1\r\n");
    Print(L"================================\r\n\r\n");

    /*
     * Static BootInfo and MemoryRegion buffer.
     * Using static storage ensures they persist after ExitBootServices,
     * as they reside in the loader's data segment.
     */
    static BootInfo bootInfo;
    static MemoryRegion memRegions[512];

    memset(&bootInfo, 0, sizeof(BootInfo));
    bootInfo.mmap_addr = (UINT64)(UINTN)memRegions;

    /* the last boot's log to \logs (when it crashed), this boot's claimed */
    bootInfo.log_addr = BootLogSetup();

    /* [1/4] Initialize graphics */
    Print(L"[1/4] Initializing graphics...\r\n");
    status = InitGraphics(&bootInfo);
    if (EFI_IS_ERROR(status)) {
        Print(L"ERROR: Failed to initialize graphics!\r\n");
        while(1);
    }

    /* [2/4] Load kernel */
    Print(L"[2/4] Loading kernel...\r\n");
    status = LoadKernel(&kernelEntry);
    if (EFI_IS_ERROR(status)) {
        Print(L"ERROR: Failed to load kernel!\r\n");
        while(1);
    }

    /* the live boot: the whole system into RAM (\live on this volume) */
    LiveLoad(&bootInfo);

    /* [3/4] Find ACPI RSDP */
    Print(L"[3/4] Finding ACPI tables...\r\n");
    bootInfo.rsdp_addr = (UINT64)(UINTN)FindRSDP();

    UINT64 kernelPml4;
    status = BuildKernelPageTables(&kernelPml4);
    if (EFI_IS_ERROR(status)) {
        Print(L"ERROR: No memory for the kernel's page tables!\r\n");
        while(1);
    }

    /* [4/4] Get memory map and exit boot services */
    Print(L"[4/4] Exiting boot services...\r\n");
    status = ExitBootServicesAndGetMemoryMap(&bootInfo);
    if (EFI_IS_ERROR(status)) {
        /* Cannot print anymore - boot services may be partially exited */
        while(1);
    }

    /* Set the boot protocol magic number */
    bootInfo.magic = WYNLAND_BOOT_MAGIC;

    /*
     * Jump to the kernel!
     *
     * The bootloader is compiled with mingw (MS ABI), so the first
     * argument (BootInfo*) is passed in RCX. The kernel entry stub
     * (entry.asm) expects the BootInfo pointer in RCX.
     */
    typedef void (*KernelEntryFunc)(BootInfo*);
    KernelEntryFunc kernel = (KernelEntryFunc)(UINTN)kernelEntry;
    /* the kernel's addresses exist only in these tables (this code, its
       stack and bootInfo stay reachable: identity-mapped there too) */
    __asm__ volatile("cli; mov %0, %%cr3" :: "r"(kernelPml4) : "memory");
    kernel(&bootInfo);

    /* Should never reach here */
    while(1);
    return EFI_SUCCESS;
}
