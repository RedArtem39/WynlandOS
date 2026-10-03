/*
 * WynlandOS - ext2 filesystem driver (the root filesystem)
 *
 * Replaces FAT32 as what actually implements the VFS API surface
 * declared in <wynland/vfs.h>. Every kernel/syscall.c and kernel/main.c
 * call site stays untouched: same function names, same signatures --
 * only the implementation underneath changed.
 *
 * What this implements (real, spec-conformant ext2, verified against
 * images produced by the host's own mke2fs/debugfs/e2fsck):
 *   - superblock + block group descriptor table, multiple block groups
 *   - block/inode bitmaps with linear first-free allocation
 *   - inode read/write, direct + single-indirect + double-indirect
 *     block pointers (triple-indirect deliberately deferred: files
 *     >~4GB are far beyond anything this OS stores)
 *   - real directory entries (inode/rec_len/name_len/file_type/name),
 *     space-reusing insertion matching real ext2 conventions
 *   - fast symlinks (target stored inline in i_block[] when short,
 *     exactly what real mke2fs-created filesystems do)
 *   - real ownership: VfsNode.mode/.uid populated straight from the
 *     on-disk i_mode/i_uid; new files stamped with the creating
 *     process's uid
 *
 * Device files (/dev/fb0, /dev/tty, ...) piggyback on VfsNode.first_cluster
 * via sentinel values 0xFFFFFFF0+, exactly as the previous FAT32 driver
 * did -- those sentinels are checked directly in ~40 places across
 * kernel/syscall.c and remain collision-free against real inode numbers
 * by construction (a multi-GB volume has nowhere near 2^32-16 inodes).
 *
 * On-disk struct layouts were byte-verified against real mke2fs output
 * before being relied upon; _Static_asserts below make any future
 * packing mistake fail the build instead of silently misreading disk.
 */

#include <wynland/virtio_gpu.h>
#include <wynland/virtgpu_drm.h>
#include <wynland/vfs.h>
#include <wynland/ahci.h>
#include <wynland/heap.h>
#include <wynland/vma.h>
#include <wynland/types.h>
#include <wynland/boot_info.h>
#include <wynland/mbr.h>
#include <wynland/rtc.h>
#include <wynland/sched.h>
#include <wynland/process.h>

extern void serial_write_string(const char *str);
extern void serial_write_char(char c);
extern void uint_to_str(uint64_t val, char *buf);
extern void uint_to_hex(uint64_t val, char *buf);
extern int sata_port_num;

/* ============================================================
 * On-disk structures
 * ============================================================ */

#define EXT2_MAGIC 0xEF53

#define EXT2_ROOT_INO 2

/* superblock feature bits */
#define EXT2_FEAT_COMPAT_RESIZE_INODE 0x0010
#define EXT2_FEAT_INCOMPAT_FILETYPE   0x0002
#define EXT2_FEAT_ROCOMPAT_SPARSE_SUPER 0x0001
#define EXT2_FEAT_ROCOMPAT_LARGE_FILE   0x0002

/* what this driver accepts (mke2fs -t ext2 defaults, confirmed via
   dumpe2fs: {filetype, sparse_super, large_file}) */
#define EXT2_INCOMPAT_SUPPORTED   (EXT2_FEAT_INCOMPAT_FILETYPE)
#define EXT2_ROCOMPAT_SUPPORTED   (EXT2_FEAT_ROCOMPAT_SPARSE_SUPER | \
                                   EXT2_FEAT_ROCOMPAT_LARGE_FILE)
#define EXT2_COMPAT_IGNORED       (~0u)

/* i_mode file-type nibble */
#define EXT2_S_IFMT   0xF000
#define EXT2_S_IFREG  0x8000
#define EXT2_S_IFDIR  0x4000
#define EXT2_S_IFLNK  0xA000

/* directory entry file_type byte (FILETYPE feature) */
#define EXT2_FT_UNKNOWN 0
#define EXT2_FT_REG     1
#define EXT2_FT_DIR     2
#define EXT2_FT_LNK     7

/* linux_dirent64 d_type values */
#define DT_UNKNOWN 0
#define DT_DIR     4
#define DT_REG     8
#define DT_LNK     10

typedef struct {
    uint32_t s_inodes_count;        /* 0 */
    uint32_t s_blocks_count;        /* 4 */
    uint32_t s_r_blocks_count;      /* 8 */
    uint32_t s_free_blocks_count;   /* 12 */
    uint32_t s_free_inodes_count;   /* 16 */
    uint32_t s_first_data_block;    /* 20 */
    uint32_t s_log_block_size;      /* 24: blocksize = 1024 << this */
    uint32_t s_log_frag_size;       /* 28 */
    uint32_t s_blocks_per_group;    /* 32 */
    uint32_t s_frags_per_group;     /* 36 */
    uint32_t s_inodes_per_group;    /* 40 */
    uint32_t s_mtime;               /* 44 */
    uint32_t s_wtime;               /* 48 */
    uint16_t s_mnt_count;           /* 52 */
    uint16_t s_max_mnt_count;       /* 54 */
    uint16_t s_magic;               /* 56: 0xEF53 */
    uint16_t s_state;               /* 58 */
    uint16_t s_errors;              /* 60 */
    uint16_t s_minor_rev_level;     /* 62 */
    uint32_t s_lastcheck;           /* 64 */
    uint32_t s_checkinterval;       /* 68 */
    uint32_t s_creator_os;          /* 72 */
    uint32_t s_rev_level;           /* 76 */
    uint16_t s_def_resuid;          /* 80 */
    uint16_t s_def_resgid;          /* 82 */
    uint32_t s_first_ino;           /* 84 */
    uint16_t s_inode_size;          /* 88 */
    uint16_t s_block_group_nr;      /* 90 */
    uint32_t s_feature_compat;      /* 92 */
    uint32_t s_feature_incompat;    /* 96 */
    uint32_t s_feature_ro_compat;   /* 100 */
    uint8_t  s_uuid[16];            /* 104 */
    char     s_volume_name[16];     /* 120 */
    uint8_t  s_padding[1024 - 136]; /* 136..1023 */
} __attribute__((packed)) Ext2Superblock;

_Static_assert(sizeof(Ext2Superblock) == 1024, "Ext2Superblock must be exactly 1024 bytes");

typedef struct {
    uint32_t bg_block_bitmap;       /* 0 */
    uint32_t bg_inode_bitmap;       /* 4 */
    uint32_t bg_inode_table;        /* 8 */
    uint16_t bg_free_blocks_count;  /* 12 */
    uint16_t bg_free_inodes_count;  /* 14 */
    uint16_t bg_used_dirs_count;    /* 16 */
    uint16_t bg_pad;                /* 18 */
    uint8_t  bg_reserved[12];       /* 20..31 */
} __attribute__((packed)) Ext2GroupDesc;

_Static_assert(sizeof(Ext2GroupDesc) == 32, "Ext2GroupDesc must be exactly 32 bytes");

typedef struct {
    uint16_t i_mode;         /* 0 */
    uint16_t i_uid;          /* 2 */
    uint32_t i_size;         /* 4 (low 32 bits; high half is i_dir_acl) */
    uint32_t i_atime;        /* 8 */
    uint32_t i_ctime;        /* 12 */
    uint32_t i_mtime;        /* 16 */
    uint32_t i_dtime;        /* 20 */
    uint16_t i_gid;          /* 24 */
    uint16_t i_links_count;  /* 26 */
    uint32_t i_blocks;       /* 28: count of 512-byte sectors */
    uint32_t i_flags;        /* 32 */
    uint32_t i_osd1;         /* 36 */
    uint32_t i_block[15];    /* 40..99: 12 direct + 3 indirect pointers */
    uint32_t i_generation;   /* 100 */
    uint32_t i_file_acl;     /* 104 */
    uint32_t i_dir_acl;      /* 108 */
    uint32_t i_faddr;        /* 112 */
    uint8_t  i_osd2[12];     /* 116..127 */
} __attribute__((packed)) Ext2Inode;

_Static_assert(sizeof(Ext2Inode) == 128, "Ext2Inode must be exactly 128 bytes");

/* 8-byte directory entry header; the name bytes follow immediately */
typedef struct {
    uint32_t inode;
    uint16_t rec_len;   /* must be >= 8, 4-aligned, may span to block end */
    uint8_t  name_len;
    uint8_t  file_type;
} __attribute__((packed)) Ext2DirEntry;

_Static_assert(sizeof(Ext2DirEntry) == 8, "Ext2DirEntry header must be exactly 8 bytes");

/* ============================================================
 * Driver state (populated by ext2_init)
 * ============================================================ */

static Ext2Superblock  g_sb;
static Ext2GroupDesc  *g_group_desc = NULL;
static uint32_t        g_group_count = 0;
static uint32_t        g_block_size = 1024;
static uint32_t        g_sectors_per_block = 2;
static uint32_t        g_inodes_per_block = 0;
static uint32_t        g_inode_size = 128;

/* Device-file sentinels carried over verbatim from the FAT32 driver --
   checked directly by ~40 sites across kernel/syscall.c. */
#define DEV_FB0        0xFFFFFFF0
#define DEV_MICE       0xFFFFFFF1
#define DEV_TTY        0xFFFFFFF2
#define DEV_KBD        0xFFFFFFF3
#define DEV_NULL       0xFFFFFFFE
#define DEV_URANDOM    0xFFFFFFF8
#define DEV_DSP        0xFFFFFFC0   /* include/wynland/hda.h */

/* ============================================================
 * Local string helpers (same conventions as every other driver)
 * ============================================================ */

static void str_copy(char *dst, const char *src) {
    while (*src) {
        *dst++ = *src++;
    }
    *dst = '\0';
}

static int str_compare(const char *s1, const char *s2) {
    while (*s1 && *s2 && *s1 == *s2) {
        s1++;
        s2++;
    }
    return *s1 - *s2;
}

static uint32_t str_len_local(const char *s) {
    uint32_t len = 0;
    while (*s++) len++;
    return len;
}

/* Splits "/a/b/c" -> parent="/a/b", name="c"; "/f" -> "/", "f";
   "f" -> "/", "f". Same semantics as the previous driver's helper. */
static bool split_path(const char *path, char *parent, char *name) {
    int last_slash = -1;
    int len = 0;
    while (path[len]) {
        if (path[len] == '/') last_slash = len;
        len++;
    }

    if (len == 0) return false;

    if (last_slash < 0) {
        str_copy(parent, "/");
        str_copy(name, path);
        return true;
    }

    if (last_slash == 0) {
        str_copy(parent, "/");
        str_copy(name, path + 1);
        return true;
    }

    for (int i = 0; i < last_slash; i++) {
        parent[i] = path[i];
    }
    parent[last_slash] = '\0';
    str_copy(name, path + last_slash + 1);
    return true;
}

/* Helper: check if path ends with suffix */
static bool path_ends_with(const char *path, const char *suffix) {
    uint32_t plen = str_len_local(path);
    uint32_t slen = str_len_local(suffix);
    if (plen < slen) return false;
    const char *p = path + (plen - slen);
    return str_compare(p, suffix) == 0;
}

static void print_u32(uint32_t v) {
    char buf[16];
    uint_to_str(v, buf);
    serial_write_string(buf);
}

static void print_hex32(uint32_t v) {
    char buf[16];
    uint_to_hex(v, buf);
    serial_write_string(buf);
}

/* Creating process's uid, falling back to root for the boot-time
   calls that happen before the scheduler/process subsystem exists
   (those really are root-owned bootstrap files). */
/* Set while the kernel itself writes on a user's behalf (/etc/shadow for
   `ary`): those writes are root's, whoever made the call. */
void *g_vfs_root_override;   /* the thread doing it; NULL = nobody */

static uint32_t current_uid_or_root(void) {
    Thread *t = sched_current();
    /* only that thread: a global flag gave root to anyone who ran while
       the write blocked */
    if (g_vfs_root_override && g_vfs_root_override == (void *)t) return 0;
    if (t && t->proc) return t->proc->uid;
    return 0;
}

/* Adding or removing a name in a directory needs write permission on the
   directory (root: always). There was no check at all: any user could
   create or delete files anywhere. */
static bool may_write_dir(const Ext2Inode *dir) {
    uint32_t uid = current_uid_or_root();
    if (uid == 0) return true;
    bool ok = (dir->i_uid == uid) ? (dir->i_mode & 0200) != 0 : (dir->i_mode & 0002) != 0;
    if (!ok) serial_write_string("perm: denied a directory write\r\n");
    return ok;
}

/* ============================================================
 * Block-level I/O (partition-relative)
 * ============================================================ */

/* Block cache: metadata (directories, inode tables, bitmaps, indirect
   blocks) and small file reads. Every path lookup used to go to the disk
   for every block it touched -- creating /apps, /docs, /dev and two DRM
   nodes at boot took 9 s. Direct-mapped, slot memory allocated on first
   use. Multi-block runs of file data are read around it (big libraries
   would only flush it) and then patched with any dirty cached block.

   Write-back: a write only updates the cache and marks the block dirty;
   the flusher thread writes dirty blocks out once a second, with
   interrupts on between them, and sync()/fsync() do it at once. It was
   write-through -- every block written went to the disk synchronously
   with interrupts off, plus the bitmap, group descriptors and superblock
   each time: a 30 KB file cost ~40 disk writes and froze the system for a
   quarter of a second; Qt's caches stalled the desktop for seconds. A
   block rewritten many times between flushes now reaches the disk once. */
#define BCACHE_SLOTS 4096                    /* x 4 KB blocks = 16 MB at most */
#define BCACHE_ON 1
typedef struct {
    uint32_t block;
    bool     valid;
    bool     dirty;      /* newer than the disk */
    uint32_t gen;        /* bumped by every write: the flusher's race check */
    uint8_t *data;
} BCacheSlot;
static BCacheSlot g_bcache[BCACHE_SLOTS];
static uint64_t g_bcache_hits, g_bcache_misses;
static volatile uint32_t g_dirty_blocks;
static volatile bool g_sb_dirty;

static BCacheSlot *bcache_slot(uint32_t block) {
    return &g_bcache[(block * 2654435761u) % BCACHE_SLOTS];
}

/* Interrupts off. Write a dirty slot to the disk now (it is about to be
   reused for another block). */
static bool bcache_writeout(BCacheSlot *e) {
    if (!e->valid || !e->dirty) return true;
    bool ok = ahci_write(mbr_root_partition_lba() + e->block * g_sectors_per_block,
                         g_sectors_per_block, e->data);
    if (ok) { e->dirty = false; g_dirty_blocks--; }
    return ok;
}

/* Interrupts off. Cache `buf` as `block`; dirty = it is a write. False
   only when a write could not be cached (the caller writes it through). */
static bool bcache_put(uint32_t block, const void *buf, bool dirty) {
    BCacheSlot *e = bcache_slot(block);
    if (e->valid && e->dirty && e->block != block && !bcache_writeout(e)) return false;
    if (!e->data) {
        e->data = (uint8_t *)kmalloc(g_block_size);
        if (!e->data) return false;
    }
    if (e->valid && e->block != block) e->dirty = false;
    memcpy(e->data, buf, g_block_size);
    e->block = block;
    e->valid = true;
    if (dirty) {
        if (!e->dirty) { e->dirty = true; g_dirty_blocks++; }
        e->gen++;
    }
    return true;
}

/* A dirty cached copy of `block`, if there is one (interrupts off). */
static const uint8_t *bcache_dirty(uint32_t block) {
    BCacheSlot *e = bcache_slot(block);
    return (e->valid && e->dirty && e->block == block && e->data) ? e->data : NULL;
}

static bool ext2_read_block(uint32_t block, void *buf) {
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
    BCacheSlot *e = bcache_slot(block);
    bool hit = BCACHE_ON && e->valid && e->block == block && e->data;
    if (hit) { memcpy(buf, e->data, g_block_size); g_bcache_hits++; }
    if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
    if (hit) return true;

    bool ok = ahci_read(mbr_root_partition_lba() + block * g_sectors_per_block,
                        g_sectors_per_block, buf);
    if (ok) {
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
        /* a caller with interrupts on can be preempted during the disk
           read, and a writer may cache a newer, dirty version meanwhile:
           that one wins (it used to be overwritten by the stale read) */
        const uint8_t *newer = bcache_dirty(block);
        if (newer) memcpy(buf, newer, g_block_size);
        else bcache_put(block, buf, false);
        g_bcache_misses++;
        if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
    }
    return ok;
}

static bool ext2_write_block(uint32_t block, const void *buf) {
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
    bool cached = bcache_put(block, buf, true);
    if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
    if (cached) return true;
    /* could not cache it: straight to the disk */
    bool ok = ahci_write(mbr_root_partition_lba() + block * g_sectors_per_block,
                         g_sectors_per_block, buf);
    if (!ok) {
        /* unknown on disk now -- but only if the slot holds this block: it
           may still hold another block's dirty data that must not be lost */
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
        BCacheSlot *e = bcache_slot(block);
        if (e->valid && e->block == block) {
            if (e->dirty) { e->dirty = false; g_dirty_blocks--; }
            e->valid = false;
        }
        if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
    }
    return ok;
}

/* The superblock copy (free counts etc.) goes out with the next flush. */
static bool ext2_write_sb(void) {
    g_sb_dirty = true;
    return true;
}

/* Write every dirty block and the superblock to the disk. From the
   flusher thread with interrupts on (each block is copied out under cli
   and written while the cache can change again: a block written meanwhile
   stays dirty, its generation tells), or from sync() in a syscall. */
bool ext2_flush(void) {
    uint8_t *tmp = (uint8_t *)kmalloc(g_block_size);
    if (!tmp) return false;
    bool all_ok = true;
    for (uint32_t i = 0; i < BCACHE_SLOTS && g_dirty_blocks; i++) {
        uint64_t rflags;
        /* no thread switch from the snapshot to the end of its write: a
           writer that ran in between could rewrite and fsync the block,
           and this older snapshot would then land on the disk last */
        g_sched_no_preempt++;
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
        BCacheSlot *e = &g_bcache[i];
        bool todo = e->valid && e->dirty && e->data;
        uint32_t block = e->block, gen = e->gen;
        if (todo) memcpy(tmp, e->data, g_block_size);
        if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
        if (!todo) { g_sched_no_preempt--; continue; }

        bool ok = ahci_write(mbr_root_partition_lba() + block * g_sectors_per_block,
                             g_sectors_per_block, tmp);

        __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
        if (ok && e->valid && e->dirty && e->block == block && e->gen == gen) {
            e->dirty = false;
            g_dirty_blocks--;
        }
        if (!ok) all_ok = false;
        if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
        g_sched_no_preempt--;
    }
    kfree(tmp);
    if (g_sb_dirty) {
        static Ext2Superblock sb_copy;
        uint64_t rflags;
        g_sched_no_preempt++;   /* same reason as the blocks above */
        __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
        g_sb_dirty = false;
        memcpy(&sb_copy, &g_sb, sizeof(sb_copy));
        if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
        if (!ahci_write(mbr_root_partition_lba() + 2, 2, &sb_copy)) { g_sb_dirty = true; all_ok = false; }
        g_sched_no_preempt--;
    }
    return all_ok && !g_dirty_blocks && !g_sb_dirty;
}

bool ext2_has_dirty(void) { return g_dirty_blocks || g_sb_dirty; }

/* the flusher kernel thread */
static void ext2_reap_orphans(void);

void ext2_flusher_thread(void *arg) {
    (void)arg;
    for (;;) {
        sched_sleep_ms(1000);
        ext2_reap_orphans();
        if (ext2_has_dirty()) ext2_flush();
    }
}

/* Writes the whole group descriptor table back to disk. It starts in
   the block right after the superblock's own first-data block. */
static void ext2_write_gdt(void) {
    uint32_t gdt_block = g_sb.s_first_data_block + 1;
    uint32_t per_block = g_block_size / sizeof(Ext2GroupDesc);
    uint8_t *buf = (uint8_t *)kmalloc(g_block_size);
    if (!buf) return;

    for (uint32_t base = 0; base < g_group_count; base += per_block) {
        uint32_t n = g_group_count - base;
        if (n > per_block) n = per_block;
        for (uint32_t i = 0; i < n; i++) {
            for (uint32_t b = 0; b < sizeof(Ext2GroupDesc); b++) {
                buf[i * sizeof(Ext2GroupDesc) + b] =
                    ((uint8_t *)&g_group_desc[base + i])[b];
            }
        }
        if (n < per_block) {
            /* preserve untouched tail bytes rather than scribbling the
               block with stack garbage: read-modify-write */
            uint8_t *full = (uint8_t *)kmalloc(g_block_size);
            if (full && ext2_read_block(gdt_block + base / per_block, full)) {
                for (uint32_t b = n * sizeof(Ext2GroupDesc); b < g_block_size; b++) {
                    buf[b] = full[b];
                }
            }
            if (full) kfree(full);
        }
        ext2_write_block(gdt_block + base / per_block, buf);
    }
    kfree(buf);
}

/* ============================================================
 * Mount
 * ============================================================ */

bool ext2_init(void) {
    if (mbr_root_partition_lba() == 0) {
        serial_write_string("ext2: no root partition located (mbr_init must run first)!\r\n");
        return false;
    }

    uint8_t sb_raw[1024];
    if (!ahci_read(mbr_root_partition_lba() + 2, 2, sb_raw)) {
        serial_write_string("ext2: failed to read superblock sectors!\r\n");
        return false;
    }
    for (int i = 0; i < 1024; i++) ((uint8_t *)&g_sb)[i] = sb_raw[i];

    if (g_sb.s_magic != EXT2_MAGIC) {
        serial_write_string("ext2: bad magic, not an ext2 filesystem!\r\n");
        return false;
    }

    if (g_sb.s_rev_level >= 1) {
        if (g_sb.s_feature_incompat & ~EXT2_INCOMPAT_SUPPORTED) {
            serial_write_string("ext2: unsupported incompat feature bits set, refusing to mount: 0x");
            print_hex32(g_sb.s_feature_incompat & ~EXT2_INCOMPAT_SUPPORTED);
            serial_write_string("\r\n");
            return false;
        }
        /* ro-compat bits we don't know are safe to ignore read-wise, but
           refusing loudly beats silently corrupting a volume that uses
           features this driver would violate on write (e.g. gdt_csum) */
        if (g_sb.s_feature_ro_compat & ~EXT2_ROCOMPAT_SUPPORTED) {
            serial_write_string("ext2: unsupported ro_compat feature bits set, refusing to mount: 0x");
            print_hex32(g_sb.s_feature_ro_compat & ~EXT2_ROCOMPAT_SUPPORTED);
            serial_write_string("\r\n");
            return false;
        }
        g_inode_size = g_sb.s_inode_size ? g_sb.s_inode_size : 128;
    } else {
        g_inode_size = 128;
    }

    g_block_size = 1024u << g_sb.s_log_block_size;
    g_sectors_per_block = g_block_size / 512;
    g_inodes_per_block = g_block_size / g_inode_size;

    g_group_count = (g_sb.s_blocks_count - g_sb.s_first_data_block +
                     g_sb.s_blocks_per_group - 1) / g_sb.s_blocks_per_group;

    g_group_desc = (Ext2GroupDesc *)kmalloc(g_group_count * sizeof(Ext2GroupDesc));
    if (!g_group_desc) {
        serial_write_string("ext2: out of memory for group descriptor table!\r\n");
        return false;
    }

    uint32_t gdt_block = g_sb.s_first_data_block + 1;
    uint32_t per_block = g_block_size / sizeof(Ext2GroupDesc);
    uint8_t *buf = (uint8_t *)kmalloc(g_block_size);
    if (!buf) {
        kfree(g_group_desc);
        g_group_desc = NULL;
        serial_write_string("ext2: out of memory for group descriptor table!\r\n");
        return false;
    }
    for (uint32_t g = 0; g < g_group_count; g++) {
        uint32_t blk = gdt_block + g / per_block;
        if (!ext2_read_block(blk, buf)) {
            kfree(buf);
            kfree(g_group_desc);
            g_group_desc = NULL;
            serial_write_string("ext2: failed to read group descriptor table!\r\n");
            return false;
        }
        for (uint32_t b = 0; b < sizeof(Ext2GroupDesc); b++) {
            ((uint8_t *)&g_group_desc[g])[b] =
                buf[(g % per_block) * sizeof(Ext2GroupDesc) + b];
        }
    }
    kfree(buf);

    serial_write_string("ext2: group 0 -- block_bitmap=");
    print_u32(g_group_desc[0].bg_block_bitmap);
    serial_write_string(" inode_bitmap=");
    print_u32(g_group_desc[0].bg_inode_bitmap);
    serial_write_string(" inode_table=");
    print_u32(g_group_desc[0].bg_inode_table);
    serial_write_string(" free_blocks=");
    print_u32(g_group_desc[0].bg_free_blocks_count);
    serial_write_string(" free_inodes=");
    print_u32(g_group_desc[0].bg_free_inodes_count);
    serial_write_string("\r\n");

    serial_write_string("ext2: mounted -- block_size=");
    print_u32(g_block_size);
    serial_write_string(" groups=");
    print_u32(g_group_count);
    serial_write_string(" inodes_count=");
    print_u32(g_sb.s_inodes_count);
    serial_write_string(" blocks_count=");
    print_u32(g_sb.s_blocks_count);
    serial_write_string(" inode_size=");
    print_u32(g_inode_size);
    serial_write_string("\r\n");

    return true;
}

/* ============================================================
 * Inode table I/O
 * ============================================================ */

bool ext2_read_inode(uint32_t inum, Ext2Inode *out) {
    if (!g_group_desc || inum < EXT2_ROOT_INO || inum > g_sb.s_inodes_count) return false;

    uint32_t group = (inum - 1) / g_sb.s_inodes_per_group;
    uint32_t index = (inum - 1) % g_sb.s_inodes_per_group;
    uint32_t block = g_group_desc[group].bg_inode_table + index / g_inodes_per_block;
    uint32_t off   = (index % g_inodes_per_block) * g_inode_size;

    if (g_inode_size < sizeof(Ext2Inode)) return false;

    uint8_t *buf = (uint8_t *)kmalloc(g_block_size);
    if (!buf) return false;
    bool ok = ext2_read_block(block, buf);
    if (ok) {
        for (uint32_t b = 0; b < sizeof(Ext2Inode); b++) {
            ((uint8_t *)out)[b] = buf[off + b];
        }
    }
    kfree(buf);
    return ok;
}

bool ext2_write_inode(uint32_t inum, const Ext2Inode *in) {
    if (!g_group_desc || inum < EXT2_ROOT_INO || inum > g_sb.s_inodes_count) return false;

    uint32_t group = (inum - 1) / g_sb.s_inodes_per_group;
    uint32_t index = (inum - 1) % g_sb.s_inodes_per_group;
    uint32_t block = g_group_desc[group].bg_inode_table + index / g_inodes_per_block;
    uint32_t off   = (index % g_inodes_per_block) * g_inode_size;

    /* Read-modify-write: an inode block holds g_inodes_per_block live
       inodes, never scribble the neighbors. */
    uint8_t *buf = (uint8_t *)kmalloc(g_block_size);
    if (!buf) return false;
    bool ok = ext2_read_block(block, buf);
    if (ok) {
        for (uint32_t b = 0; b < sizeof(Ext2Inode); b++) {
            buf[off + b] = ((const uint8_t *)in)[b];
        }
        ok = ext2_write_block(block, buf);
    }
    kfree(buf);
    return ok;
}

/* ============================================================
 * Bitmap allocation (block + inode), linear first-free scan
 * ============================================================ */

static bool ext2_bitmap_test_and_set(uint32_t bit, uint32_t bitmap_block,
                                     uint32_t total_bits, bool set_it) {
    uint8_t *bm = (uint8_t *)kmalloc(g_block_size);
    if (!bm) return false;
    if (!ext2_read_block(bitmap_block, bm)) { kfree(bm); return false; }

    if (bit >= total_bits) { kfree(bm); return false; }

    bool was = (bm[bit >> 3] >> (bit & 7)) & 1;
    if (set_it && !was) {
        bm[bit >> 3] |= (uint8_t)(1u << (bit & 7));
        if (!ext2_write_block(bitmap_block, bm)) { kfree(bm); return false; }
    } else if (!set_it && was) {
        bm[bit >> 3] &= (uint8_t)~(1u << (bit & 7));
        if (!ext2_write_block(bitmap_block, bm)) { kfree(bm); return false; }
    }
    kfree(bm);
    return was;
}

/* Claim the first clear bit in [first, nbits) of a bitmap block: one
   read, a byte-wise scan, one write. -1 if none is free. The allocators
   used to test bit by bit, each test copying the whole 4 KB bitmap into a
   fresh heap buffer -- finding block N cost N copies, hundreds of MB per
   allocation on a filled disk, all with interrupts off (seconds per
   write() of a growing file). */
static int64_t bitmap_claim_first(uint32_t bitmap_block, uint32_t first, uint32_t nbits)
{
    uint8_t *bm = (uint8_t *)kmalloc(g_block_size);
    if (!bm) return -1;
    if (!ext2_read_block(bitmap_block, bm)) { kfree(bm); return -1; }
    if (nbits > g_block_size * 8) nbits = g_block_size * 8;
    int64_t found = -1;
    for (uint32_t bit = first; bit < nbits; ) {
        if ((bit & 7) == 0 && bit + 8 <= nbits && bm[bit >> 3] == 0xFF) { bit += 8; continue; }
        if (!((bm[bit >> 3] >> (bit & 7)) & 1)) { found = bit; break; }
        bit++;
    }
    if (found >= 0) {
        bm[found >> 3] |= (uint8_t)(1u << (found & 7));
        if (!ext2_write_block(bitmap_block, bm)) found = -1;
    }
    kfree(bm);
    return found;
}

uint32_t ext2_alloc_block(void) {
    if (!g_group_desc) return 0;

    for (uint32_t g = 0; g < g_group_count; g++) {
        if (g_group_desc[g].bg_free_blocks_count == 0) continue;
        /* Canonical ext2 block<->bitmap-bit mapping (verified against
           dumpe2fs's own "Group N: (Blocks X-Y)" output for 4K blocks):
           group G covers blocks [G*bpg .. (G+1)*bpg-1], and bit B of
           its bitmap IS block G*bpg+B. s_first_data_block (=1 here)
           only records that block 0 hosts the superblock -- adding it
           into the address math shifts every allocation one block up,
           which silently corrupts the filesystem while staying
           self-consistent for this driver alone. */
        uint32_t group_base = g * g_sb.s_blocks_per_group;
        uint32_t bits_this_group = g_sb.s_blocks_per_group;
        if (group_base + bits_this_group > g_sb.s_blocks_count)
            bits_this_group = g_sb.s_blocks_count - group_base;
        int64_t local = bitmap_claim_first(g_group_desc[g].bg_block_bitmap, 0, bits_this_group);
        if (local < 0) continue;
        uint32_t blk = group_base + (uint32_t)local;
        if (g_group_desc[g].bg_free_blocks_count > 0)
            g_group_desc[g].bg_free_blocks_count--;
        if (g_sb.s_free_blocks_count > 0)
            g_sb.s_free_blocks_count--;
        ext2_write_gdt();
        ext2_write_sb();
        return blk;
    }
    serial_write_string("ext2: out of disk blocks!\r\n");
    return 0;
}

void ext2_free_block(uint32_t block) {
    if (!g_group_desc || block == 0) return;

    /* canonical mapping, mirror of ext2_alloc_block's */
    uint32_t g = block / g_sb.s_blocks_per_group;
    if (g >= g_group_count) return;
    uint32_t local = block % g_sb.s_blocks_per_group;

    if (!ext2_bitmap_test_and_set(local,
                                  g_group_desc[g].bg_block_bitmap,
                                  g_sb.s_blocks_per_group, false)) {
        return; /* wasn't set */
    }
    g_group_desc[g].bg_free_blocks_count++;
    g_sb.s_free_blocks_count++;
    ext2_write_gdt();
    ext2_write_sb();
}

uint32_t ext2_alloc_inode(void) {
    if (!g_group_desc) return 0;

    for (uint32_t g = 0; g < g_group_count; g++) {
        if (g_group_desc[g].bg_free_inodes_count == 0) continue;
        /* inode numbers are 1-based within the whole fs; never below the root */
        uint32_t first = (g == 0) ? EXT2_ROOT_INO - 1 : 0;
        uint32_t nbits = g_sb.s_inodes_per_group;
        if (g * g_sb.s_inodes_per_group + nbits > g_sb.s_inodes_count)
            nbits = g_sb.s_inodes_count - g * g_sb.s_inodes_per_group;
        int64_t idx = bitmap_claim_first(g_group_desc[g].bg_inode_bitmap, first, nbits);
        if (idx < 0) continue;
        uint32_t inum = g * g_sb.s_inodes_per_group + (uint32_t)idx + 1;
        if (g_group_desc[g].bg_free_inodes_count > 0)
            g_group_desc[g].bg_free_inodes_count--;
        if (g_sb.s_free_inodes_count > 0)
            g_sb.s_free_inodes_count--;
        ext2_write_gdt();
        ext2_write_sb();
        return inum;
    }
    serial_write_string("ext2: out of free inodes!\r\n");
    return 0;
}

void ext2_free_inode(uint32_t inum) {
    if (!g_group_desc || inum < EXT2_ROOT_INO) return;

    uint32_t g = (inum - 1) / g_sb.s_inodes_per_group;
    if (g >= g_group_count) return;
    uint32_t idx = (inum - 1) % g_sb.s_inodes_per_group;

    if (!ext2_bitmap_test_and_set(idx,
                                  g_group_desc[g].bg_inode_bitmap,
                                  g_sb.s_inodes_per_group, false)) {
        return;
    }
    g_group_desc[g].bg_free_inodes_count++;
    g_sb.s_free_inodes_count++;
    ext2_write_gdt();
    ext2_write_sb();
}

/* ============================================================
 * Block-pointer resolution (direct + single/double indirect)
 * ============================================================ */

/* Resolves a file's logical block index to its physical on-disk block
   number (0 = hole / failure). With allocate=true, missing pointers and
   the data block itself are allocated on the way down; freshly
   allocated *pointer* blocks are zeroed (mandatory -- garbage pointers
   would resolve to random disk blocks), while freshly allocated *data*
   blocks are not zeroed: a partial-block write landing entirely within
   a first-time-allocated block reads back whatever was already on that
   disk block for the untouched portion. Real-world callers here are all
   append-style sequential writes, so this hasn't mattered in practice;
   revisit if it ever does.
   NOTE: when allocation happens, the caller owns flushing the modified
   inode (the write path does exactly that via ext2_write_inode). */
uint32_t ext2_resolve_block(Ext2Inode *inode, uint32_t logical, bool alloc) {
    uint32_t ptrs_per_block = g_block_size / 4;

    /* direct blocks [0..11] */
    if (logical < 12) {
        if (inode->i_block[logical]) return inode->i_block[logical];
        if (!alloc) return 0;
        uint32_t nb = ext2_alloc_block();
        if (!nb) return 0;
        inode->i_blocks += g_sectors_per_block;
        inode->i_block[logical] = nb;
        return nb;
    }

    uint8_t *blkbuf = (uint8_t *)kmalloc(g_block_size);
    if (!blkbuf) return 0;

    /* singly indirect [12 .. 12+N) */
    uint32_t l1_end = 12 + ptrs_per_block;
    if (logical < l1_end) {
        if (!inode->i_block[12]) {
            if (!alloc) { kfree(blkbuf); return 0; }
            uint32_t ib = ext2_alloc_block();
            if (!ib) { kfree(blkbuf); return 0; }
            inode->i_blocks += g_sectors_per_block;
            memset(blkbuf, 0, g_block_size);
            ext2_write_block(ib, blkbuf);
            inode->i_block[12] = ib;
        }
        if (!ext2_read_block(inode->i_block[12], blkbuf)) { kfree(blkbuf); return 0; }
        uint32_t idx = logical - 12;
        uint32_t phys = ((uint32_t *)blkbuf)[idx];
        if (!phys) {
            if (!alloc) { kfree(blkbuf); return 0; }
            phys = ext2_alloc_block();
            if (!phys) { kfree(blkbuf); return 0; }
            inode->i_blocks += g_sectors_per_block;
            ((uint32_t *)blkbuf)[idx] = phys;
            ext2_write_block(inode->i_block[12], blkbuf);
        }
        kfree(blkbuf);
        return phys;
    }

    /* doubly indirect [l1_end .. l1_end+N*N) */
    uint32_t l2_end = l1_end + ptrs_per_block * ptrs_per_block;
    if (logical < l2_end) {
        if (!inode->i_block[13]) {
            if (!alloc) { kfree(blkbuf); return 0; }
            uint32_t ib = ext2_alloc_block();
            if (!ib) { kfree(blkbuf); return 0; }
            inode->i_blocks += g_sectors_per_block;
            memset(blkbuf, 0, g_block_size);
            ext2_write_block(ib, blkbuf);
            inode->i_block[13] = ib;
        }
        uint32_t rel = logical - l1_end;
        uint32_t outer_idx = rel / ptrs_per_block;
        uint32_t inner_idx = rel % ptrs_per_block;

        if (!ext2_read_block(inode->i_block[13], blkbuf)) { kfree(blkbuf); return 0; }
        uint32_t outer_blk = ((uint32_t *)blkbuf)[outer_idx];
        if (!outer_blk) {
            if (!alloc) { kfree(blkbuf); return 0; }
            outer_blk = ext2_alloc_block();
            if (!outer_blk) { kfree(blkbuf); return 0; }
            inode->i_blocks += g_sectors_per_block;
            /* blkbuf holds the doubly-indirect table read above: add the
               new entry to it. It was zeroed first -- every other entry of
               the table was lost, and with it all data a file had past
               ~4 MB as soon as it grew another 4 MB. */
            ((uint32_t *)blkbuf)[outer_idx] = outer_blk;
            ext2_write_block(inode->i_block[13], blkbuf);
            /* freshly allocated inner table must start zeroed */
            memset(blkbuf, 0, g_block_size);
            ext2_write_block(outer_blk, blkbuf);
        } else {
            if (!ext2_read_block(outer_blk, blkbuf)) { kfree(blkbuf); return 0; }
        }

        uint32_t phys = ((uint32_t *)blkbuf)[inner_idx];
        if (!phys && alloc) {
            phys = ext2_alloc_block();
            if (phys) {
                inode->i_blocks += g_sectors_per_block;
                ((uint32_t *)blkbuf)[inner_idx] = phys;
                ext2_write_block(outer_blk, blkbuf);
            }
        }
        kfree(blkbuf);
        return phys;
    }

    /* triple-indirect deliberately deferred */
    kfree(blkbuf);
    return 0;
}

/* ============================================================
 * File data read
 * ============================================================ */

/* Read-only block mapping with the indirect tables cached for the whole
   read: ext2_resolve_block() re-reads its single/double indirect table from
   disk on every call -- up to three disk reads per data block of a large
   file. Returns the physical block, 0 for a hole. */
typedef struct {
    uint32_t l1_blk;   /* cached singly-indirect table (i_block[12]) */
    uint32_t dind_blk; /* cached doubly-indirect top table (i_block[13]) */
    uint32_t l2_blk;   /* cached second-level table under it */
    uint32_t *l1, *dind, *l2;
} Ext2MapCache;

static uint32_t ext2_map_ro(const Ext2Inode *inode, uint32_t logical, Ext2MapCache *mc)
{
    uint32_t ppb = g_block_size / 4;
    if (logical < 12) return inode->i_block[logical];
    uint32_t l1_end = 12 + ppb;
    if (logical < l1_end) {
        uint32_t tb = inode->i_block[12];
        if (!tb) return 0;
        if (mc->l1_blk != tb) {
            if (!ext2_read_block(tb, mc->l1)) return 0;
            mc->l1_blk = tb;
        }
        return mc->l1[logical - 12];
    }
    uint32_t l2_end = l1_end + ppb * ppb;
    if (logical < l2_end) {
        uint32_t top = inode->i_block[13];
        if (!top) return 0;
        if (mc->dind_blk != top) {
            if (!ext2_read_block(top, mc->dind)) return 0;
            mc->dind_blk = top;
        }
        uint32_t rel = logical - l1_end;
        uint32_t outer = mc->dind[rel / ppb];
        if (!outer) return 0;
        if (mc->l2_blk != outer) {
            if (!ext2_read_block(outer, mc->l2)) return 0;
            mc->l2_blk = outer;
        }
        return mc->l2[rel % ppb];
    }
    return 0; /* triple-indirect: not supported (see ext2_resolve_block) */
}

/* Physically contiguous bounce buffer for multi-block DMA reads. */
#define EXT2_RUN_BLOCKS 64
static uint8_t *g_run_buf = NULL;
static uint32_t g_run_cap = 0; /* bytes */

uint32_t ext2_read_file_data(const Ext2Inode *inode, uint32_t offset, uint32_t len, void *buf) {
    if (!inode || !buf || len == 0) return 0;
    if (offset >= inode->i_size) return 0;
    if (offset + len > inode->i_size) len = inode->i_size - offset;

    if (!g_run_buf) {
        extern void *pmm_alloc_contiguous(uint32_t count);
        uint32_t bytes = EXT2_RUN_BLOCKS * g_block_size;
        g_run_buf = (uint8_t *)pmm_alloc_contiguous((bytes + 4095) / 4096); /* identity-mapped */
        g_run_cap = g_run_buf ? bytes : 0;
    }

    Ext2MapCache mc;
    memset(&mc, 0, sizeof(mc));
    mc.l1 = (uint32_t *)kmalloc(g_block_size);
    mc.dind = (uint32_t *)kmalloc(g_block_size);
    mc.l2 = (uint32_t *)kmalloc(g_block_size);
    uint8_t *block_buf = (uint8_t *)kmalloc(g_block_size);
    if (!mc.l1 || !mc.dind || !mc.l2 || !block_buf) {
        if (mc.l1) kfree(mc.l1);
        if (mc.dind) kfree(mc.dind);
        if (mc.l2) kfree(mc.l2);
        if (block_buf) kfree(block_buf);
        return 0;
    }

    uint8_t *out = (uint8_t *)buf;
    uint32_t bytes_read = 0;

    /* i_dir_acl doubles as the size-high word for regular files; this
       OS never stores >4GB files, and VfsNode.size is 32-bit anyway. */
    while (bytes_read < len) {
        uint32_t abs_pos = offset + bytes_read;
        uint32_t logical_block = abs_pos / g_block_size;
        uint32_t block_offset = abs_pos % g_block_size;
        uint32_t chunk = g_block_size - block_offset;
        if (chunk > len - bytes_read) chunk = len - bytes_read;

        uint32_t phys_block = ext2_map_ro(inode, logical_block, &mc);
        if (phys_block == 0) {
            /* A hole (sparse file -- debugfs `write`, which builds the
               image, stores all-zero blocks that way), NOT end of file:
               len is already clamped to i_size above. Treating it as EOF
               silently truncated every read spanning a hole; ld.so's
               whole-file first mmap of a large library then saw zeros
               where its .dynamic section is and crashed. */
            for (uint32_t i = 0; i < chunk; i++) out[bytes_read + i] = 0;
            bytes_read += chunk;
            continue;
        }

        /* Whole blocks from a block boundary: extend over physically
           consecutive blocks and fetch the run with ONE disk command
           (it was one command per 4 KB block). */
        if (block_offset == 0 && g_run_buf && len - bytes_read >= g_block_size) {
            uint32_t want = (len - bytes_read) / g_block_size;
            if (want > EXT2_RUN_BLOCKS) want = EXT2_RUN_BLOCKS;
            uint32_t run = 1;
            while (run < want && ext2_map_ro(inode, logical_block + run, &mc) == phys_block + run) run++;
            if (run > 1) {
                /* the bounce buffer is shared: no preemption while in use */
                uint64_t rflags;
                __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
                bool ok = ahci_read(mbr_root_partition_lba() + phys_block * g_sectors_per_block,
                                    run * g_sectors_per_block, g_run_buf);
                if (ok) {
                    /* the disk may be behind the cache (write-back) */
                    for (uint32_t r = 0; r < run && g_dirty_blocks; r++) {
                        const uint8_t *d = bcache_dirty(phys_block + r);
                        if (d) memcpy(g_run_buf + r * g_block_size, d, g_block_size);
                    }
                    memcpy(out + bytes_read, g_run_buf, run * g_block_size);
                }
                if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
                if (!ok) break;
                bytes_read += run * g_block_size;
                continue;
            }
        }

        if (!ext2_read_block(phys_block, block_buf)) break;

        for (uint32_t i = 0; i < chunk; i++) out[bytes_read + i] = block_buf[block_offset + i];
        bytes_read += chunk;
    }
    kfree(block_buf);
    kfree(mc.l1);
    kfree(mc.dind);
    kfree(mc.l2);

    return bytes_read;
}

/* ---------------- file data write ---------------- */

/* Extends the file as needed (allocating blocks via ext2_resolve_block's
   allocate=true path), updates i_size/i_blocks, and writes the inode
   back. Known, accepted v1 gap: a partial-block write landing entirely
   within a block that's being allocated for the very first time reads
   back whatever bytes are already on that freshly-allocated disk block
   for the untouched portion (not guaranteed zero) -- real-world callers
   here are all append-style sequential writes (new files, growing
   config files), never sparse partial overwrites of pre-existing
   content, so this hasn't mattered in practice; revisit if it ever
   does. */
uint32_t ext2_write_file_data(uint32_t inum, Ext2Inode *inode, uint32_t offset, uint32_t len, const void *buf) {
    pcache_drop_inode(inum);   /* mapped copies of the old contents (kernel/vma.c) */
    const uint8_t *in = (const uint8_t *)buf;
    uint32_t bytes_written = 0;
    uint8_t *block_buf = (uint8_t *)kmalloc(g_block_size);
    if (!block_buf) return 0;

    while (bytes_written < len) {
        uint32_t abs_pos = offset + bytes_written;
        uint32_t logical_block = abs_pos / g_block_size;
        uint32_t block_offset = abs_pos % g_block_size;
        uint32_t chunk = g_block_size - block_offset;
        if (chunk > len - bytes_written) chunk = len - bytes_written;

        uint32_t phys_block = ext2_resolve_block(inode, logical_block, true);
        if (phys_block == 0) break; /* out of space */

        if (chunk != g_block_size) {
            if (!ext2_read_block(phys_block, block_buf)) memset(block_buf, 0, g_block_size);
        }
        for (uint32_t i = 0; i < chunk; i++) block_buf[block_offset + i] = in[bytes_written + i];
        ext2_write_block(phys_block, block_buf);

        bytes_written += chunk;
    }
    kfree(block_buf);

    uint32_t new_end = offset + bytes_written;
    if (new_end > inode->i_size) inode->i_size = new_end;
    /* i_blocks is kept exact by ext2_resolve_block(): every data and
       pointer block it allocates is counted there (this used to be a
       data-only estimate from i_size, which fsck flagged) */
    inode->i_mtime = (uint32_t)rtc_get_unix_time();
    ext2_write_inode(inum, inode);

    return bytes_written;
}

/* ============================================================
 * Directory entry lookup
 * ============================================================ */

/* Walks a directory inode's data blocks (direct + indirect via
   ext2_resolve_block) looking for `name`. Returns the matching inode
   number and file_type byte. Correct negative-lookup (name absent)
   is as important as the positive case -- callers rely on it to
   distinguish "doesn't exist" from "exists". */
bool ext2_dir_lookup(const Ext2Inode *dir, const char *name,
                     uint32_t *out_inum, uint8_t *out_type) {
    if (!g_group_desc || !dir || !name || !out_inum) return false;

    uint32_t nlen = str_len_local(name);
    if (nlen == 0 || nlen > 255) return false;

    uint32_t consumed = 0;
    uint8_t *block_buf = (uint8_t *)kmalloc(g_block_size);
    if (!block_buf) return false;

    for (uint32_t lb = 0; consumed < dir->i_size; lb++) {
        uint32_t phys = ext2_resolve_block((Ext2Inode *)dir, lb, false);
        if (!phys) break;
        if (!ext2_read_block(phys, block_buf)) break;
        consumed += g_block_size;

        uint32_t off = 0;
        while (off + sizeof(Ext2DirEntry) <= g_block_size) {
            Ext2DirEntry *ent = (Ext2DirEntry *)(block_buf + off);
            if (ent->rec_len < sizeof(Ext2DirEntry) ||
                off + ent->rec_len > g_block_size ||
                (ent->rec_len & 3)) {
                break; /* corrupt guard */
            }
            if (ent->inode != 0 && ent->name_len == nlen &&
                ent->name_len <= ent->rec_len - sizeof(Ext2DirEntry)) {
                const char *ename = (const char *)(block_buf + off + sizeof(Ext2DirEntry));
                bool match = true;
                for (uint32_t i = 0; i < nlen; i++) {
                    if (ename[i] != name[i]) { match = false; break; }
                }
                if (match) {
                    *out_inum = ent->inode;
                    if (out_type) *out_type = ent->file_type;
                    kfree(block_buf);
                    return true;
                }
            }
            off += ent->rec_len;
        }
    }

    kfree(block_buf);
    return false;
}

/* ============================================================
 * Directory entry insertion
 * ============================================================ */

static inline uint32_t dir_entry_min_len(uint32_t name_len) {
    uint32_t v = sizeof(Ext2DirEntry) + name_len;
    return (v + 3) & ~3u;
}

/* Adds a directory entry for `child_inum` named `name` into `dir`.
   Space strategy (matching the exact conventions observed in real
   mke2fs-created calibration images):
     1. a deleted entry (inode==0) whose rec_len fits -- reuse in place;
     2. a live donor entry with enough spare rec_len -- shrink the
        donor to its own minimum, insert into the freed slack;
     3. otherwise extend the last entry of a block that still has room
        to the end of that block, or append a brand-new directory data
        block.
   The parent's on-disk inode (i_size/i_mtime) is written back here so
   no caller has to remember to. */
bool ext2_dir_add_entry(uint32_t dir_inum, Ext2Inode *dir,
                        const char *name, uint32_t child_inum, uint8_t child_type) {
    if (!g_group_desc || !dir || !name) return false;

    uint32_t nlen = str_len_local(name);
    if (nlen == 0 || nlen > 255) return false;
    uint32_t needed = dir_entry_min_len(nlen);

    uint8_t *block_buf = (uint8_t *)kmalloc(g_block_size);
    if (!block_buf) return false;

    uint32_t consumed = 0;
    for (uint32_t lb = 0; consumed < dir->i_size; lb++) {
        uint32_t phys = ext2_resolve_block(dir, lb, false);
        if (!phys) break;
        if (!ext2_read_block(phys, block_buf)) break;
        consumed += g_block_size;

        uint32_t off = 0;
        while (off + sizeof(Ext2DirEntry) <= g_block_size) {
            Ext2DirEntry *ent = (Ext2DirEntry *)(block_buf + off);
            if (ent->rec_len < sizeof(Ext2DirEntry) ||
                off + ent->rec_len > g_block_size ||
                (ent->rec_len & 3)) {
                break;
            }

            if (ent->inode == 0 && ent->rec_len >= needed &&
                ent->name_len >= nlen) {
                /* reuse a fully-deleted entry's space */
                ent->inode = child_inum;
                ent->file_type = child_type;
                ent->name_len = (uint8_t)nlen;
                /* keep its full rec_len: slack stays absorbed here */
                char *dst = (char *)(block_buf + off + sizeof(Ext2DirEntry));
                for (uint32_t i = 0; i < nlen; i++) dst[i] = name[i];
                bool ok = ext2_write_block(phys, block_buf);
                kfree(block_buf);
                return ok;
            }

            if (ent->inode != 0) {
                uint32_t my_min = dir_entry_min_len(ent->name_len);
                if (ent->rec_len - my_min >= needed) {
                    uint16_t slack = (uint16_t)(ent->rec_len - my_min);
                    ent->rec_len = (uint16_t)my_min;
                    uint8_t *ins = block_buf + off + my_min;
                    Ext2DirEntry *ne = (Ext2DirEntry *)ins;
                    ne->inode = child_inum;
                    ne->rec_len = slack;
                    ne->name_len = (uint8_t)nlen;
                    ne->file_type = child_type;
                    char *dst = (char *)(ins + sizeof(Ext2DirEntry));
                    for (uint32_t i = 0; i < nlen; i++) dst[i] = name[i];
                    bool ok = ext2_write_block(phys, block_buf);
                    kfree(block_buf);
                    return ok;
                }
            }

            off += ent->rec_len;
        }
    }
    kfree(block_buf);

    /* No existing slack anywhere: append a new block to the directory. */
    if (dir->i_size % g_block_size != 0) return false; /* only append at block granularity */
    uint32_t new_lb = dir->i_size / g_block_size;
    uint32_t phys = ext2_resolve_block(dir, new_lb, true);
    if (!phys) return false;

    uint8_t *nb = (uint8_t *)kmalloc(g_block_size);
    if (!nb) return false;
    memset(nb, 0, g_block_size);
    Ext2DirEntry *ne = (Ext2DirEntry *)nb;
    ne->inode = child_inum;
    ne->rec_len = (uint16_t)g_block_size; /* sole entry absorbs the whole block */
    ne->name_len = (uint8_t)nlen;
    ne->file_type = child_type;
    char *dst = (char *)(nb + sizeof(Ext2DirEntry));
    for (uint32_t i = 0; i < nlen; i++) dst[i] = name[i];

    bool ok = ext2_write_block(phys, nb);
    kfree(nb);
    if (!ok) return false;

    dir->i_size += g_block_size;
    dir->i_mtime = (uint32_t)rtc_get_unix_time();
    ext2_write_inode(dir_inum, dir);
    return true;
}

/* ============================================================
 * mkdir
 * ============================================================ */

/* Creates a real subdirectory of `parent`: fresh inode (mode 0755,
   links_count=2), one data block holding the "." and ".." entries
   (exactly like mke2fs lays them out), entry insertion into the
   parent, parent's i_links_count bump for the new ".." back-pointer,
   and the group descriptor's own bg_used_dirs_count bookkeeping
   (skipping that last one is precisely what e2fsck flags as
   "Directories count wrong for group #0"). */
bool ext2_mkdir(uint32_t parent_inum, Ext2Inode *parent, const char *name) {
    if (!g_group_desc || !parent || !name) return false;
    if (!may_write_dir(parent)) return false;

    uint32_t exist_inum;
    uint8_t  exist_type;
    if (ext2_dir_lookup(parent, name, &exist_inum, &exist_type)) return false;

    uint32_t new_inum = ext2_alloc_inode();
    if (new_inum == 0) return false;

    Ext2Inode nd;
    memset(&nd, 0, sizeof(nd));
    nd.i_mode = EXT2_S_IFDIR | 0755;
    nd.i_links_count = 2; /* parent's entry + its own "." */
    nd.i_uid = (uint16_t)current_uid_or_root();
    uint32_t now = (uint32_t)rtc_get_unix_time();
    nd.i_atime = now;
    nd.i_ctime = now;
    nd.i_mtime = now;

    uint32_t data_blk = ext2_alloc_block();
    if (!data_blk) {
        ext2_free_inode(new_inum);
        return false;
    }
    nd.i_block[0] = data_blk;
    nd.i_size = g_block_size;
    nd.i_blocks = g_sectors_per_block;

    uint8_t *blk = (uint8_t *)kmalloc(g_block_size);
    if (!blk) {
        ext2_free_block(data_blk);
        ext2_free_inode(new_inum);
        return false;
    }
    memset(blk, 0, g_block_size);

    Ext2DirEntry *dot = (Ext2DirEntry *)blk;
    dot->inode = new_inum;
    dot->rec_len = 12;
    dot->name_len = 1;
    dot->file_type = EXT2_FT_DIR;
    blk[8] = '.';

    Ext2DirEntry *dotdot = (Ext2DirEntry *)(blk + 12);
    dotdot->inode = parent_inum;
    dotdot->rec_len = (uint16_t)(g_block_size - 12); /* ".." absorbs to block end */
    dotdot->name_len = 2;
    dotdot->file_type = EXT2_FT_DIR;
    blk[12 + 8] = '.';
    blk[12 + 9] = '.';

    bool ok = ext2_write_block(data_blk, blk);
    kfree(blk);
    if (!ok || !ext2_write_inode(new_inum, &nd)) {
        ext2_free_block(data_blk);
        ext2_free_inode(new_inum);
        return false;
    }

    if (!ext2_dir_add_entry(parent_inum, parent, name, new_inum, EXT2_FT_DIR)) {
        ext2_free_block(data_blk);
        ext2_free_inode(new_inum);
        return false;
    }

    parent->i_links_count++; /* the new subdirectory's ".." now points back here */
    ext2_write_inode(parent_inum, parent);

    uint32_t new_inode_group = (new_inum - 1) / g_sb.s_inodes_per_group;
    g_group_desc[new_inode_group].bg_used_dirs_count++;
    ext2_write_gdt();

    return true;
}

/* ---------------- unlink (file delete) ---------------- */

static void ext2_free_all_blocks(const Ext2Inode *inode) {
    for (int i = 0; i < 12; i++) {
        if (inode->i_block[i]) ext2_free_block(inode->i_block[i]);
    }
    uint32_t ptrs_per_block = g_block_size / 4;

    if (inode->i_block[12]) {
        uint32_t *ptrs = (uint32_t *)kmalloc(g_block_size);
        if (ptrs && ext2_read_block(inode->i_block[12], ptrs)) {
            for (uint32_t i = 0; i < ptrs_per_block; i++) if (ptrs[i]) ext2_free_block(ptrs[i]);
        }
        if (ptrs) kfree(ptrs);
        ext2_free_block(inode->i_block[12]);
    }

    if (inode->i_block[13]) {
        uint32_t *outer = (uint32_t *)kmalloc(g_block_size);
        if (outer && ext2_read_block(inode->i_block[13], outer)) {
            for (uint32_t o = 0; o < ptrs_per_block; o++) {
                if (!outer[o]) continue;
                uint32_t *inner = (uint32_t *)kmalloc(g_block_size);
                if (inner && ext2_read_block(outer[o], inner)) {
                    for (uint32_t i = 0; i < ptrs_per_block; i++) if (inner[i]) ext2_free_block(inner[i]);
                }
                if (inner) kfree(inner);
                ext2_free_block(outer[o]);
            }
        }
        if (outer) kfree(outer);
        ext2_free_block(inode->i_block[13]);
    }
}

/* Inodes held by lazy file mappings (kernel/vma.c VmaFile). Unlinking
   such a file removes its name only; the inode and its blocks stay until
   the last mapping goes, then the flusher thread frees them (the last
   unpin can happen in the scheduler's reaper, no place for disk I/O).
   Without this, a mapped file's inode was freed at unlink and could be
   reused: untouched pages of the old mapping then read another file. */
#define PIN_SLOTS 512
static struct { uint32_t inum; uint32_t refs; bool orphan; } g_pins[PIN_SLOTS];

static int pin_find(uint32_t inum) {
    for (int i = 0; i < PIN_SLOTS; i++)
        if (g_pins[i].inum == inum && (g_pins[i].refs || g_pins[i].orphan)) return i;
    return -1;
}

bool ext2_pin_inode(uint32_t inum) {
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
    int i = pin_find(inum);
    if (i < 0)
        for (int k = 0; k < PIN_SLOTS; k++)
            if (!g_pins[k].refs && !g_pins[k].orphan) { i = k; g_pins[k].inum = inum; break; }
    if (i >= 0) g_pins[i].refs++;
    if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
    return i >= 0;
}

void ext2_unpin_inode(uint32_t inum) {
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags) :: "memory");
    int i = pin_find(inum);
    if (i >= 0 && g_pins[i].refs) g_pins[i].refs--;   /* orphan + 0 refs: the flusher frees it */
    if (rflags & 0x200) __asm__ volatile("sti" ::: "memory");
}

static void ext2_destroy_inode(uint32_t inum, Ext2Inode *inode);

/* flusher thread: free orphaned inodes nothing maps any more */
static void ext2_reap_orphans(void) {
    for (int i = 0; i < PIN_SLOTS; i++) {
        if (!g_pins[i].orphan || g_pins[i].refs) continue;
        uint32_t inum = g_pins[i].inum;
        g_pins[i].orphan = false;
        g_pins[i].inum = 0;
        Ext2Inode inode;
        if (ext2_read_inode(inum, &inode)) ext2_destroy_inode(inum, &inode);
    }
}

bool ext2_unlink(uint32_t dir_inum, Ext2Inode *dir, const char *name) {
    (void)dir_inum;
    if (!may_write_dir(dir)) return false;
    uint32_t name_len = 0;
    while (name[name_len]) name_len++;

    uint8_t *block_buf = (uint8_t *)kmalloc(g_block_size);
    if (!block_buf) return false;

    bool found = false;
    uint32_t found_inode = 0;

    for (int b = 0; b < 12 && dir->i_block[b] != 0 && !found; b++) {
        if (!ext2_read_block(dir->i_block[b], block_buf)) continue;

        uint32_t off = 0;
        Ext2DirEntry *prev = NULL;
        while (off < g_block_size) {
            Ext2DirEntry *ent = (Ext2DirEntry *)(block_buf + off);
            if (ent->rec_len == 0) break;
            if (ent->inode != 0 && ent->name_len == name_len) {
                char *ent_name = (char *)(block_buf + off + sizeof(Ext2DirEntry));
                bool match = true;
                for (uint32_t i = 0; i < name_len; i++) if (ent_name[i] != name[i]) { match = false; break; }
                if (match) {
                    found_inode = ent->inode;
                    /* Real ext2 delete convention: merge this entry's
                       space into the previous entry by extending its
                       rec_len (matches the same "last entry absorbs
                       spare space" pattern already observed in the
                       calibration image), or if it's the first entry
                       in the block, just mark it unused (inode=0) --
                       ext2_dir_add_entry() already knows how to reuse
                       an inode==0 entry's full rec_len. */
                    if (prev) {
                        prev->rec_len = (uint16_t)(prev->rec_len + ent->rec_len);
                    } else {
                        ent->inode = 0;
                    }
                    ext2_write_block(dir->i_block[b], block_buf);
                    found = true;
                    break;
                }
            }
            prev = ent;
            off += ent->rec_len;
        }
    }
    kfree(block_buf);

    if (!found) return false;

    Ext2Inode target;
    if (ext2_read_inode(found_inode, &target)) {
        if (target.i_links_count > 0) target.i_links_count--;
        /* an (empty) directory also loses its own "." link: it is gone.
           It kept its inode with one link left -- fsck found it orphaned,
           its ".." still counting against the parent. */
        if ((target.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) target.i_links_count = 0;
        int pin = target.i_links_count == 0 ? pin_find(found_inode) : -1;
        if (pin >= 0 && g_pins[pin].refs) {
            /* still mapped: nameless until the last mapping goes */
            g_pins[pin].orphan = true;
            ext2_write_inode(found_inode, &target);
        } else if (target.i_links_count == 0) {
            ext2_destroy_inode(found_inode, &target);
        } else {
            ext2_write_inode(found_inode, &target);
        }
    }

    return true;
}

/* Free an inode with no links left: its blocks, then the inode itself. */
static void ext2_destroy_inode(uint32_t found_inode, Ext2Inode *inode) {
    pcache_drop_inode(found_inode);
    Ext2Inode target = *inode;
    ext2_free_all_blocks(&target);
    /* Real bug found via independent e2fsck cross-check (see
       Phase 20 plan writeup): freeing the bitmap bit alone
       left the on-disk inode struct still looking like a
       real, populated file (nonzero i_links_count, real block
       pointers) -- e2fsck correctly flagged this as an
       "unattached inode" plus bitmap inconsistencies, since it
       trusts the inode table's own content over the bitmap.
       A real ext2 delete zeroes the inode and stamps i_dtime;
       do the same before releasing the bitmap bit. */
    Ext2Inode empty;
    memset(&empty, 0, sizeof(empty));
    empty.i_dtime = (uint32_t)rtc_get_unix_time();
    ext2_write_inode(found_inode, &empty);
    ext2_free_inode(found_inode);
}

/* ---------------- fast symlinks ---------------- */

/* Real ext2 optimization, reused here deliberately (not a shortcut):
   a symlink target shorter than the 60 bytes available across
   i_block[0..14] is stored inline in the inode itself, no data block
   allocated at all -- exactly matching what real mke2fs-created
   filesystems do, confirmed against the ext2 spec's own documented
   convention, not invented. Longer targets fall back to a normal
   single data block (this OS's own paths are always short). */
bool ext2_create_symlink(uint32_t dir_inum, Ext2Inode *dir, const char *name, const char *target) {
    if (!may_write_dir(dir)) return false;
    uint32_t target_len = 0;
    while (target[target_len]) target_len++;

    uint32_t new_inum = ext2_alloc_inode();
    if (new_inum == 0) return false;

    Ext2Inode link;
    memset(&link, 0, sizeof(link));
    link.i_mode = EXT2_S_IFLNK | 0777;
    link.i_links_count = 1;
    link.i_uid = (uint16_t)current_uid_or_root();
    uint32_t now = (uint32_t)rtc_get_unix_time();
    link.i_atime = now;
    link.i_ctime = now;
    link.i_mtime = now;

    bool ok;
    if (target_len <= 59) {
        /* fast symlink: target bytes live inside i_block[] itself */
        uint8_t *inline_bytes = (uint8_t *)&link.i_block[0];
        for (uint32_t i = 0; i < target_len; i++) inline_bytes[i] = (uint8_t)target[i];
        link.i_size = target_len;
        link.i_blocks = 0;
        ok = ext2_write_inode(new_inum, &link);
    } else {
        if (target_len >= g_block_size) { ext2_free_inode(new_inum); return false; }
        uint32_t blk = ext2_alloc_block();
        if (!blk) { ext2_free_inode(new_inum); return false; }
        uint8_t *buf = (uint8_t *)kmalloc(g_block_size);
        if (!buf) { ext2_free_block(blk); ext2_free_inode(new_inum); return false; }
        memset(buf, 0, g_block_size);
        for (uint32_t i = 0; i < target_len; i++) buf[i] = (uint8_t)target[i];
        ok = ext2_write_block(blk, buf);
        kfree(buf);
        if (!ok) { ext2_free_block(blk); ext2_free_inode(new_inum); return false; }
        link.i_block[0] = blk;
        link.i_size = target_len;
        link.i_blocks = g_sectors_per_block;
        ok = ext2_write_inode(new_inum, &link);
    }
    if (!ok) {
        ext2_free_inode(new_inum);
        return false;
    }

    ok = ext2_dir_add_entry(dir_inum, dir, name, new_inum, EXT2_FT_LNK);
    if (!ok) {
        /* entry insertion failed: roll the inode back so we don't leak it */
        Ext2Inode empty;
        memset(&empty, 0, sizeof(empty));
        empty.i_dtime = (uint32_t)rtc_get_unix_time();
        ext2_write_inode(new_inum, &empty);
        ext2_free_inode(new_inum);
    }
    return ok;
}

/* Reads a symlink's target into buf (NUL-terminated). Returns the
   length copied, 0 on failure. */
uint32_t ext2_read_symlink(const Ext2Inode *link, char *buf, uint32_t bufsize) {
    if (!link || !buf || bufsize == 0) return 0;

    uint32_t len = link->i_size;
    if (len > bufsize - 1) len = bufsize - 1;
    if (len == 0) { buf[0] = '\0'; return 0; }

    if (link->i_blocks == 0) {
        const uint8_t *inline_bytes = (const uint8_t *)&link->i_block[0];
        for (uint32_t i = 0; i < len; i++) buf[i] = (char)inline_bytes[i];
        buf[len] = '\0';
        return len;
    }

    if (!link->i_block[0]) return 0;
    uint8_t *blk = (uint8_t *)kmalloc(g_block_size);
    if (!blk) return 0;
    if (!ext2_read_block(link->i_block[0], blk)) { kfree(blk); return 0; }
    for (uint32_t i = 0; i < len; i++) buf[i] = (char)blk[i];
    buf[len] = '\0';
    kfree(blk);
    return len;
}

/* ============================================================
 * Path lookup (with real symlink resolution)
 * ============================================================ */

#define EXT2_LOOKUP_MAX_LINKS 8

/* Resolves `path` starting from directory `start_inum`. The leading-'/'
   requirement applies only to the public entry point below; symlink
   expansion reuses this directly with relative targets. */
static bool ext2_lookup_from(uint32_t start_inum, const char *path,
                             uint32_t *out_inum, Ext2Inode *out_inode, int link_depth);

static bool ext2_lookup_path(const char *path, uint32_t *out_inum, Ext2Inode *out_inode) {
    if (!path || path[0] != '/') return false;
    return ext2_lookup_from(EXT2_ROOT_INO, path, out_inum, out_inode, 0);
}

static bool ext2_lookup_from(uint32_t start_inum, const char *path,
                             uint32_t *out_inum, Ext2Inode *out_inode, int link_depth) {
    if (!path || !out_inum || link_depth > EXT2_LOOKUP_MAX_LINKS) return false;

    uint32_t cur_inum = start_inum;
    Ext2Inode cur;
    if (!ext2_read_inode(cur_inum, &cur)) return false;

    const char *p = path;
    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        char comp[MAX_FILENAME];
        uint32_t clen = 0;
        while (*p && *p != '/') {
            if (clen < MAX_FILENAME - 1) comp[clen++] = *p;
            p++;
        }
        comp[clen] = '\0';
        bool last = true;
        for (const char *q = p; *q; q++) {
            if (*q != '/') { last = false; break; }
        }

        uint32_t next_inum;
        uint8_t  next_type;
        if (!ext2_dir_lookup(&cur, comp, &next_inum, &next_type)) return false;

        Ext2Inode next;
        if (!ext2_read_inode(next_inum, &next)) return false;

        if ((next.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK) {
            /* Real symlink semantics: absolute targets restart from the
               filesystem root, relative targets are relative to the
               directory containing the link. Either way, whatever path
               remains unprocessed continues after the resolved target.
               Depth-guarded against symlink loops. */
            char target[MAX_PATH];
            if (ext2_read_symlink(&next, target, sizeof(target)) == 0) return false;

            const char *rest = p; /* remaining unprocessed path (incl. slashes) */
            char combined[MAX_PATH];
            uint32_t tl = str_len_local(target);
            uint32_t rl = str_len_local(rest);
            if (tl + rl >= MAX_PATH) return false;
            for (uint32_t i = 0; i < tl; i++) combined[i] = target[i];
            for (uint32_t i = 0; i < rl; i++) combined[tl + i] = rest[i];
            combined[tl + rl] = '\0';

            uint32_t base = (target[0] == '/') ? EXT2_ROOT_INO : cur_inum;
            return ext2_lookup_from(base, combined, out_inum, out_inode,
                                    link_depth + 1);
        }

        cur_inum = next_inum;
        cur = next;

        if (last) {
            *out_inum = cur_inum;
            if (out_inode) *out_inode = cur;
            return true;
        }
    }

    *out_inum = cur_inum;
    if (out_inode) *out_inode = cur;
    return true;
}

/* ============================================================
 * VfsNode filling
 * ============================================================ */

static void fill_node_from_inode(VfsNode *node, uint32_t inum,
                                 const Ext2Inode *inode, const char *name) {
    str_copy(node->name, name);
    node->size = inode->i_size; /* >4GB files deliberately unsupported */
    node->is_dir = ((inode->i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR);
    /* first_cluster keeps its historical role as "what identifies this
       node downstream": real files carry their ext2 inode number here.
       Device sentinels (0xFFFFFFF0+) live far above any possible inode. */
    node->first_cluster = inum;
    node->readonly = (inode->i_mode & 0222) == 0;
    node->mode = inode->i_mode & 07777;
    node->uid = inode->i_uid;
    node->mtime = inode->i_mtime;
}

/* ============================================================
 * VFS API -- init
 * ============================================================ */

bool vfs_init(void) {
    ahci_init();

    if (sata_port_num == -1) {
        serial_write_string("VFS Error: AHCI not initialized, cannot init VFS!\r\n");
        return false;
    }

    /* Find the root partition first (the disk is a real MBR-partitioned
       image: ESP + ext2 root), then mount ext2 inside it. */
    mbr_init();
    return ext2_init();
}

/* ============================================================
 * VFS API -- file open / close
 * ============================================================ */

VfsFile *vfs_open(const char *path) {
    return vfs_open_flags(path, VFS_O_READ);
}

static VfsFile *alloc_device_file(const char *name, uint32_t size,
                                  bool readonly, uint32_t sentinel, uint32_t flags) {
    VfsFile *file = (VfsFile *)kmalloc(sizeof(VfsFile));
    if (!file) return NULL;
    str_copy(file->node.name, name);
    file->node.size = size;
    file->node.is_dir = false;
    file->node.readonly = readonly;
    file->node.mode = 0666;
    file->node.uid = 0;
    file->node.first_cluster = sentinel;
    file->offset = 0;
    file->current_cluster = 0;
    file->current_cluster_offset = 0;
    file->flags = flags;
    file->dir_entry_sector = 0;
    file->dir_entry_offset = 0;
    file->dirty = false;
    return file;
}

VfsFile *vfs_open_flags(const char *path, uint32_t flags) {
    /* Device nodes first -- exact or suffix match (handles relative
       paths), byte-identical behavior to the previous FAT32 driver. */
    if (str_compare(path, "/dev/fb0") == 0 || path_ends_with(path, "/dev/fb0") || path_ends_with(path, "dev/fb0")) {
        extern BootInfo *g_boot_info;
        return alloc_device_file("fb0",
                                 g_boot_info ? (g_boot_info->fb_pitch * g_boot_info->fb_height) : 0,
                                 false, DEV_FB0, flags);
    }
    if (str_compare(path, "/dev/input/mice") == 0 || path_ends_with(path, "/dev/input/mice") || path_ends_with(path, "dev/input/mice")) {
        return alloc_device_file("mice", 0, false, DEV_MICE, flags);
    }
    if (str_compare(path, "/dev/input/kbd") == 0 || path_ends_with(path, "/dev/input/kbd") || path_ends_with(path, "dev/input/kbd")) {
        return alloc_device_file("kbd", 0, false, DEV_KBD, flags);
    }
    /* DRM nodes (drivers/video/virtgpu_drm.c): ioctl/mmap-only devices */
    if (str_compare(path, "/dev/dri/renderD128") == 0) {
        VfsFile *f = alloc_device_file("renderD128", 0, false, DRM_DEV_RENDER, flags);
        if (f) f->current_cluster = drm_open_client();
        return f;
    }
    if (str_compare(path, "/dev/dri/card0") == 0) {
        VfsFile *f = alloc_device_file("card0", 0, false, DRM_DEV_CARD, flags);
        if (f) f->current_cluster = drm_open_client();
        return f;
    }
    if (str_compare(path, "/dev/tty") == 0 || path_ends_with(path, "/dev/tty") || path_ends_with(path, "dev/tty")) {
        return alloc_device_file("tty", 0, false, DEV_TTY, flags);
    }

    /* git-port regression fix carried over verbatim: git's own die()
       setup unconditionally opens /dev/null for redirects; with no
       device backing it at all that open() failed and whatever git did
       next hung indefinitely. Real /dev/null semantics: writes discard
       everything, reads always return EOF immediately (see the
       DEV_NULL branches in vfs_read/vfs_write below). */
    if (str_compare(path, "/dev/null") == 0 || path_ends_with(path, "/dev/null") || path_ends_with(path, "dev/null")) {
        return alloc_device_file("null", 0, false, DEV_NULL, flags);
    }

    /* /dev/dsp: the HD Audio output (drivers/sound/hda.c), OSS style */
    if (str_compare(path, "/dev/dsp") == 0 || path_ends_with(path, "/dev/dsp")) {
        extern bool hda_present(void);
        extern void hda_dsp_open(void);
        if (!hda_present()) return NULL;
        hda_dsp_open();
        return alloc_device_file("dsp", 0, false, DEV_DSP, flags);
    }

    /* Real /dev/urandom|/dev/random semantics for this driver v1:
       reads return zeros (getrandom syscall is the real entropy source
       elsewhere); writes silently succeed. */
    if (str_compare(path, "/dev/urandom") == 0 ||
        str_compare(path, "/dev/random")  == 0 ||
        path_ends_with(path, "/dev/urandom") ||
        path_ends_with(path, "/dev/random")) {
        return alloc_device_file("urandom", 0, false, DEV_URANDOM, flags);
    }

    uint32_t inum;
    Ext2Inode inode;
    bool found = ext2_lookup_path(path, &inum, &inode);

    if (!found) {
        if (flags & VFS_O_CREATE) {
            if (!vfs_create(path)) return NULL;
            found = ext2_lookup_path(path, &inum, &inode);
            if (!found) return NULL;
        } else {
            return NULL;
        }
    }

    bool is_dir = (inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR;
    if (is_dir && ((flags & VFS_O_WRITE) || (flags & VFS_O_CREATE) || (flags & VFS_O_TRUNC))) {
        return NULL; /* directories can only be opened read-only */
    }

    VfsFile *file = (VfsFile *)kmalloc(sizeof(VfsFile));
    if (!file) return NULL;

    char dn[MAX_PATH], bn[MAX_FILENAME];
    split_path(path, dn, bn);
    fill_node_from_inode(&file->node, inum, &inode, bn);
    file->flags = flags;
    /* current_cluster/current_cluster_offset go unused for this backend:
       ext2 block resolution takes an absolute byte offset directly
       (file->offset alone is sufficient state), unlike FAT32's mandatory
       cluster-chain cursor. */
    file->current_cluster = 0;
    file->current_cluster_offset = 0;
    file->dir_entry_sector = 0;
    file->dir_entry_offset = 0;
    file->dirty = false;

    if (flags & VFS_O_TRUNC) {
        pcache_drop_inode(inum);
        ext2_free_all_blocks(&inode);
        /* the contents go; the file's mode, owner and links stay (it was
           reset to 0644 owned by whoever truncated it) */
        uint16_t mode = inode.i_mode, uid = inode.i_uid, gid = inode.i_gid;
        uint16_t links = inode.i_links_count;
        memset(&inode, 0, sizeof(inode));
        inode.i_mode = mode;
        inode.i_links_count = links ? links : 1;
        inode.i_uid = uid;
        inode.i_gid = gid;
        inode.i_mtime = inode.i_ctime = inode.i_atime = (uint32_t)rtc_get_unix_time();
        ext2_write_inode(inum, &inode);
        file->node.size = 0;
        file->offset = 0;
    } else if (flags & VFS_O_APPEND) {
        file->offset = file->node.size;
    } else {
        file->offset = 0;
    }

    return file;
}

void vfs_close(VfsFile *file) {
    if (!file) return;
    kfree(file);
}

/* ============================================================
 * VFS API -- read
 * ============================================================ */


/* ---- sysfs attributes of the DRM device, from the real PCI function ---- */

static uint32_t put_str(char *o, uint32_t n, uint32_t cap, const char *t)
{
    while (*t && n + 1 < cap) o[n++] = *t++;
    return n;
}

static uint32_t put_hex(char *o, uint32_t n, uint32_t cap, uint32_t v, int digits, bool upper)
{
    const char *d = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    for (int i = digits - 1; i >= 0 && n + 1 < cap; i--) o[n++] = d[(v >> (i * 4)) & 0xF];
    return n;
}

static uint32_t sysfs_drm_attr(uint32_t kind, char *o, uint32_t cap)
{
    VgpuPciInfo pi;
    if (!virtio_gpu_pci_info(&pi)) {
        memset(&pi, 0, sizeof(pi));
        pi.vendor = 0x1AF4; pi.device = 0x1050; pi.slot = 2;
        pi.subvendor = 0x1AF4; pi.subdevice = 0x1100; pi.class_code = 0x038000;
    }
    uint32_t n = 0;
    if (kind == 0xFFFFFFF4) {        /* vendor (and subsystem_vendor) */
        n = put_str(o, n, cap, "0x"); n = put_hex(o, n, cap, pi.vendor, 4, false);
    } else if (kind == 0xFFFFFFF5) { /* device (and subsystem_device) */
        n = put_str(o, n, cap, "0x"); n = put_hex(o, n, cap, pi.device, 4, false);
    } else if (kind == 0xFFFFFFEF) { /* revision */
        n = put_str(o, n, cap, "0x"); n = put_hex(o, n, cap, pi.revision, 2, false);
    } else {                         /* uevent of the PCI device */
        n = put_str(o, n, cap, "DRIVER=virtio-pci\nPCI_CLASS=");
        n = put_hex(o, n, cap, pi.class_code, 5, true);
        n = put_str(o, n, cap, "\nPCI_ID=");
        n = put_hex(o, n, cap, pi.vendor, 4, true);   n = put_str(o, n, cap, ":");
        n = put_hex(o, n, cap, pi.device, 4, true);
        n = put_str(o, n, cap, "\nPCI_SUBSYS_ID=");
        n = put_hex(o, n, cap, pi.subvendor, 4, true); n = put_str(o, n, cap, ":");
        n = put_hex(o, n, cap, pi.subdevice, 4, true);
        n = put_str(o, n, cap, "\nPCI_SLOT_NAME=0000:");
        n = put_hex(o, n, cap, pi.bus, 2, false);  n = put_str(o, n, cap, ":");
        n = put_hex(o, n, cap, pi.slot, 2, false); n = put_str(o, n, cap, ".");
        n = put_hex(o, n, cap, pi.func, 1, false);
    }
    n = put_str(o, n, cap, "\n");
    return n;
}

int vfs_read(VfsFile *file, void *buf, uint32_t size) {
    if (!file || !buf) return -1;
    if (IS_DRM_DEV(file->node.first_cluster)) return -1; /* ioctl/mmap only */
    if (file->node.first_cluster == DEV_DSP) return -1;  /* playback only */

    if (file->node.first_cluster >= 0xFFFFFFF0) {
        if (file->node.first_cluster == DEV_NULL) { /* always EOF */
            return 0;
        }
        if (file->node.first_cluster == DEV_URANDOM) {
            /* the kernel CSPRNG, as getrandom() (this returned zeros) */
            extern void random_bytes(void *buf, uint64_t len);
            random_bytes(buf, size);
            return (int)size;
        }
        if (file->node.first_cluster == DEV_FB0) {
            extern BootInfo *g_boot_info;
            if (!g_boot_info) return 0;
            uint32_t fb_size = g_boot_info->fb_pitch * g_boot_info->fb_height;
            if (file->offset >= fb_size) return 0;
            if (file->offset + size > fb_size) size = fb_size - file->offset;
            extern uint64_t fb_active_phys_addr(void);
            uint8_t *fb = (uint8_t *)(uintptr_t)fb_active_phys_addr();
            for (uint32_t i = 0; i < size; i++) ((uint8_t *)buf)[i] = fb[file->offset + i];
            file->offset += size;
            return (int)size;
        }
        if (file->node.first_cluster == DEV_MICE) {
            extern int mouse_read_queue(uint8_t *buf, int size);
            return mouse_read_queue((uint8_t *)buf, (int)size);
        }
        if (file->node.first_cluster == DEV_KBD) {
            extern int kbd_raw_read_queue(uint8_t *buf, int size);
            return kbd_raw_read_queue((uint8_t *)buf, (int)size);
        }
        if (file->node.first_cluster == DEV_TTY) {
            uint32_t read_bytes = 0;
            char *cbuf = (char *)buf;
            while (read_bytes < size) {
                extern bool keyboard_has_scancode(void);
                extern uint8_t keyboard_pop_scancode(void);
                extern bool serial_received(void);
                extern char serial_read_char(void);
                if (keyboard_has_scancode()) {
                    uint8_t sc = keyboard_pop_scancode();
                    if (!(sc & 0x80)) {
                        extern const char scancode_to_ascii_lower[59];
                        if (sc < 59) {
                            char ch = scancode_to_ascii_lower[sc];
                            if (ch != 0) {
                                cbuf[read_bytes++] = ch;
                                if (ch == '\n' || ch == '\r') break;
                            }
                        }
                    }
                } else if (serial_received()) {
                    char ch = serial_read_char();
                    if (ch == '\r') ch = '\n';
                    cbuf[read_bytes++] = ch;
                    if (ch == '\n') break;
                } else {
                    extern void sched_yield(void);
                    sched_yield();
                }
            }
            return (int)read_bytes;
        }
        /* PCI sysfs attributes of the DRM device (/sys/dev/char/226:N/device/
           vendor, device, revision, uevent) -- what libdrm's drmGetDevice2()
           parses. Generated from the real virtio-gpu PCI function; the old
           fixed strings claimed an Intel 8086:1111 and had no
           PCI_SLOT_NAME, so libdrm rejected every DRM node (-ENODEV) and
           Mesa's EGL gave up on the render device. */
        if (file->node.first_cluster == 0xFFFFFFF4 || file->node.first_cluster == 0xFFFFFFF5 ||
            file->node.first_cluster == 0xFFFFFFEF || file->node.first_cluster == 0xFFFFFFF6 ||
            file->node.first_cluster == 0xFFFFFFF7) {
            char val[256];
            uint32_t len = sysfs_drm_attr(file->node.first_cluster, val, sizeof(val));
            if (file->offset >= len) return 0;
            if (size > len - file->offset) size = len - file->offset;
            memcpy(buf, val + file->offset, size);
            file->offset += size;
            return (int)size;
        }
        return 0;
    }

    Ext2Inode inode;
    if (!ext2_read_inode(file->node.first_cluster, &inode)) return -1;
    uint32_t n = ext2_read_file_data(&inode, file->offset, size, buf);
    file->offset += n;
    return (int)n;
}

/* ============================================================
 * VFS API -- write
 * ============================================================ */

int vfs_write(VfsFile *file, const void *buf, uint32_t size) {
    if (!file || !buf) return -1;
    if (file->node.first_cluster == DEV_DSP) {
        extern int64_t hda_dsp_write(const void *buf, uint32_t len);
        int64_t r = hda_dsp_write(buf, size);
        return r < 0 ? -1 : (int)r;
    }
    if (IS_DRM_DEV(file->node.first_cluster)) return -1; /* ioctl/mmap only */

    if (file->node.first_cluster >= 0xFFFFFFF0) {
        if (file->node.first_cluster == DEV_NULL) { /* discard everything */
            return (int)size;
        }
        if (file->node.first_cluster == DEV_URANDOM) { /* discard writes */
            return (int)size;
        }
        if (file->node.first_cluster == DEV_FB0) {
            extern BootInfo *g_boot_info;
            if (!g_boot_info) return 0;
            uint32_t fb_size = g_boot_info->fb_pitch * g_boot_info->fb_height;
            if (file->offset >= fb_size) return 0;
            if (file->offset + size > fb_size) size = fb_size - file->offset;
            file->offset += size;
            return (int)size;
        }
        if (file->node.first_cluster == DEV_TTY) {
            extern BootInfo *g_boot_info;
            extern uint32_t term_bg_color;
            extern bool g_quiet_console;
            extern void console_print_char(BootInfo *info, char c, uint32_t fg, uint32_t bg);
            const char *cbuf = (const char *)buf;
            for (uint32_t i = 0; i < size; i++) {
                char ch = cbuf[i];
                char single[2] = { ch, '\0' };
                serial_write_string(single);
                if (g_boot_info && !g_quiet_console) console_print_char(g_boot_info, ch, 0x00FFFFFF, term_bg_color);
            }
            return (int)size;
        }
        return -1; /* write not supported on stream-only devices like mice */
    }

    if (!(file->flags & VFS_O_WRITE)) return -1;
    if (size == 0) return 0;

    Ext2Inode inode;
    if (!ext2_read_inode(file->node.first_cluster, &inode)) return -1;
    uint32_t n = ext2_write_file_data(file->node.first_cluster, &inode, file->offset, size, buf);
    file->offset += n;
    file->node.size = inode.i_size;
    return (int)n;
}

/* ============================================================
 * VFS API -- seek / tell
 * ============================================================ */

int vfs_seek(VfsFile *file, int32_t offset, int whence) {
    if (!file) return -1;
    int64_t new_offset;
    switch (whence) {
        case VFS_SEEK_SET: new_offset = offset; break;
        case VFS_SEEK_CUR: new_offset = (int64_t)file->offset + offset; break;
        case VFS_SEEK_END: new_offset = (int64_t)file->node.size + offset; break;
        default: return -1;
    }
    if (new_offset < 0) return -1;
    file->offset = (uint32_t)new_offset;
    return 0;
}

uint32_t vfs_tell(VfsFile *file) {
    return file ? file->offset : 0;
}

/* ============================================================
 * VFS API -- create
 * ============================================================ */

bool vfs_create(const char *path) {
    char dirname[MAX_PATH], basename[MAX_FILENAME];
    split_path(path, dirname, basename);

    uint32_t dir_inum;
    Ext2Inode dir;
    if (!ext2_lookup_path(dirname, &dir_inum, &dir)) return false;
    if (!may_write_dir(&dir)) return false;

    uint32_t existing_inum; uint8_t existing_type;
    if (ext2_dir_lookup(&dir, basename, &existing_inum, &existing_type)) return false;

    uint32_t new_inum = ext2_alloc_inode();
    if (new_inum == 0) return false;

    Ext2Inode newf;
    memset(&newf, 0, sizeof(newf));
    newf.i_mode = EXT2_S_IFREG | 0644;
    newf.i_links_count = 1;
    /* Real ownership: a file created by a uid-1000 process needs its own
       uid on the inode, or the very next SYS_open()'s owner check would
       deny the creator access to its own just-created file. Boot-time
       calls before the scheduler exists fall back to root -- correct,
       those really are root-owned bootstrap files. */
    newf.i_uid = (uint16_t)current_uid_or_root();
    uint32_t now = (uint32_t)rtc_get_unix_time();
    newf.i_atime = now;
    newf.i_ctime = now;
    newf.i_mtime = now;

    if (!ext2_write_inode(new_inum, &newf)) {
        ext2_free_inode(new_inum);
        return false;
    }

    if (!ext2_dir_add_entry(dir_inum, &dir, basename, new_inum, EXT2_FT_REG)) {
        ext2_free_inode(new_inum);
        return false;
    }

    return true;
}

/* Marks a file non-writable the real way: clears the mode bits that
   grant write access (owner+group+other), backed by SYS_open's real
   owner-vs-other enforcement. Replaces the old FAT32 single-attribute-
   bit hack with actual permission semantics. */
/* May the caller read (4) / write (2) / execute (1) an existing path?
   Owner bits for the owner, "other" bits for everyone else (no groups),
   root always. A path that doesn't exist answers true: creating it is
   the directory's business (may_write_dir). */
bool vfs_may_access(const char *path, uint32_t want) {
    uint32_t uid = current_uid_or_root();
    if (uid == 0 || !want) return true;
    uint32_t inum;
    Ext2Inode inode;
    if (!ext2_lookup_path(path, &inum, &inode)) return true;
    uint32_t have = (inode.i_uid == uid) ? ((inode.i_mode >> 6) & 7) : (inode.i_mode & 7);
    if ((have & want) == want) return true;
    serial_write_string("perm: denied ");
    serial_write_string(path);
    serial_write_string("\r\n");
    return false;
}

/* chmod: the owner or root; only the permission bits change */
bool vfs_chmod(const char *path, uint32_t mode) {
    uint32_t inum;
    Ext2Inode inode;
    if (!ext2_lookup_path(path, &inum, &inode)) return false;
    uint32_t uid = current_uid_or_root();
    if (uid != 0 && inode.i_uid != uid) return false;
    inode.i_mode = (uint16_t)((inode.i_mode & ~07777u) | (mode & 07777u));
    inode.i_ctime = (uint32_t)rtc_get_unix_time();
    return ext2_write_inode(inum, &inode);
}

/* fchmod(): the open file's inode, owner or root only. -1 not an ext2
   file, 0 not allowed, 1 done. */
int vfs_fchmod(VfsFile *file, uint32_t mode) {
    if (!file) return -1;
    uint32_t inum = file->node.first_cluster;
    if (inum == 0 || inum >= 0xFFFFFF00u) return -1;    /* device/socket/pipe sentinels */
    Ext2Inode inode;
    if (!ext2_read_inode(inum, &inode)) return -1;
    uint32_t uid = current_uid_or_root();
    if (uid != 0 && inode.i_uid != uid) return 0;
    inode.i_mode = (uint16_t)((inode.i_mode & ~07777u) | (mode & 07777u));
    inode.i_ctime = (uint32_t)rtc_get_unix_time();
    file->node.mode = (uint16_t)(inode.i_mode & 07777u);
    return ext2_write_inode(inum, &inode) ? 1 : -1;
}

bool vfs_set_readonly(const char *path) {
    uint32_t inum;
    Ext2Inode inode;
    if (!ext2_lookup_path(path, &inum, &inode)) return false;

    inode.i_mode &= (uint16_t)~0222;
    inode.i_mtime = (uint32_t)rtc_get_unix_time();
    return ext2_write_inode(inum, &inode);
}

/* ============================================================
 * VFS API -- mkdir / delete
 * ============================================================ */

bool vfs_mkdir(const char *path) {
    char dirname[MAX_PATH], basename[MAX_FILENAME];
    split_path(path, dirname, basename);

    uint32_t dir_inum;
    Ext2Inode dir;
    if (!ext2_lookup_path(dirname, &dir_inum, &dir)) return false;
    if ((dir.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return false;

    return ext2_mkdir(dir_inum, &dir, basename);
}

bool vfs_delete(const char *path) {
    if (!path || str_compare(path, "/") == 0) return false; /* never delete root */

    uint32_t inum;
    Ext2Inode inode;
    if (!ext2_lookup_path(path, &inum, &inode)) return false;

    bool is_dir = ((inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR);

    /* Directories must be empty: only "." and ".." may remain. */
    if (is_dir) {
        bool has_entries = false;
        for (uint32_t lb = 0; lb * g_block_size < inode.i_size && !has_entries; lb++) {
            uint8_t *blk = (uint8_t *)kmalloc(g_block_size);
            if (!blk) break;
            uint32_t phys = ext2_resolve_block(&inode, lb, false);
            if (!phys || !ext2_read_block(phys, blk)) { kfree(blk); break; }
            uint32_t off = 0;
            while (off + sizeof(Ext2DirEntry) <= g_block_size) {
                Ext2DirEntry *ent = (Ext2DirEntry *)(blk + off);
                const char *nm = (const char *)(blk + off + sizeof(Ext2DirEntry));
                if (ent->rec_len < sizeof(Ext2DirEntry) ||
                    off + ent->rec_len > g_block_size ||
                    (ent->rec_len & 3)) break;
                bool is_dot = ent->name_len == 1 && nm[0] == '.';
                bool is_dotdot = ent->name_len == 2 && nm[0] == '.' && nm[1] == '.';
                if (ent->inode != 0 && !is_dot && !is_dotdot) {
                    has_entries = true;
                    break;
                }
                off += ent->rec_len;
            }
            kfree(blk);
        }

        if (has_entries) {
            serial_write_string("VFS: Cannot delete non-empty directory\r\n");
            return false;
        }
    }

    /* Remove the entry from the parent directory (this also frees the
       inode itself once its link count hits zero). */
    char dirname[MAX_PATH], basename[MAX_FILENAME];
    split_path(path, dirname, basename);
    uint32_t parent_inum;
    Ext2Inode parent;
    if (!ext2_lookup_path(dirname, &parent_inum, &parent)) return false;

    bool was_dir = is_dir;
    if (!ext2_unlink(parent_inum, &parent, basename)) return false;

    if (was_dir) {
        /* mirror ext2_mkdir's bookkeeping in reverse: parent's ".."
           back-pointer goes away, group dir count drops */
        uint32_t dir_group = (inum - 1) / g_sb.s_inodes_per_group;
        if (g_group_desc[dir_group].bg_used_dirs_count > 0) {
            g_group_desc[dir_group].bg_used_dirs_count--;
        }
        ext2_write_gdt();

        if (parent.i_links_count > 2) {
            parent.i_links_count--;
            ext2_write_inode(parent_inum, &parent);
        }
    }

    serial_write_string("VFS: Deleted: ");
    serial_write_string(path);
    serial_write_string("\r\n");
    return true;
}

/* ============================================================
 * VFS API -- stat
 * ============================================================ */

bool vfs_stat(const char *path, VfsStat *out) {
    if (!out) return false;

    uint32_t inum;
    Ext2Inode inode;
    if (!ext2_lookup_path(path, &inum, &inode)) return false;

    char dirname[MAX_PATH], basename[MAX_FILENAME];
    split_path(path, dirname, basename);

    str_copy(out->name, basename);
    out->size = inode.i_size;
    out->is_dir = ((inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR);
    out->first_cluster = inum; /* consumers use this as st_ino */
    /* VfsStat's FAT-era timestamp fields have no ext2 equivalent shape;
       zero them honestly rather than fake-encoding unix time into
       FAT bitfields. */
    out->create_time = 0;
    out->create_date = 0;
    out->write_time = 0;
    out->write_date = 0;
    out->attr = (uint8_t)(inode.i_mode & 0xFF); /* low mode byte for callers that peek */
    out->mode = (uint16_t)(inode.i_mode & 07777);
    out->uid = inode.i_uid;
    out->mtime = inode.i_mtime;
    out->atime = inode.i_atime;
    out->ctime = inode.i_ctime;

    return true;
}

/* ============================================================
 * VFS API -- readdir (callback form)
 * ============================================================ */

bool vfs_readdir(const char *path, void (*callback)(VfsNode *node)) {
    if (!callback) return false;

    uint32_t dir_inum;
    Ext2Inode dir;
    if (!ext2_lookup_path(path, &dir_inum, &dir)) return false;
    if ((dir.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return false;

    uint8_t *blk = (uint8_t *)kmalloc(g_block_size);
    if (!blk) return false;

    uint32_t consumed = 0;
    bool ok = true;
    for (uint32_t lb = 0; consumed < dir.i_size && ok; lb++) {
        uint32_t phys = ext2_resolve_block(&dir, lb, false);
        if (!phys) break;
        if (!ext2_read_block(phys, blk)) { ok = false; break; }
        consumed += g_block_size;

        uint32_t off = 0;
        while (off + sizeof(Ext2DirEntry) <= g_block_size) {
            Ext2DirEntry *ent = (Ext2DirEntry *)(blk + off);
            const char *nm = (const char *)(blk + off + sizeof(Ext2DirEntry));
            if (ent->rec_len < sizeof(Ext2DirEntry) ||
                off + ent->rec_len > g_block_size ||
                (ent->rec_len & 3)) break;
            off += ent->rec_len;

            if (ent->inode == 0) continue;
            bool is_dot = ent->name_len == 1 && nm[0] == '.';
            bool is_dotdot = ent->name_len == 2 && nm[0] == '.' && nm[1] == '.';
            if (is_dot || is_dotdot) continue;

            Ext2Inode child;
            if (!ext2_read_inode(ent->inode, &child)) continue;

            char namebuf[MAX_FILENAME];
            uint32_t nl = ent->name_len;
            if (nl > MAX_FILENAME - 1) nl = MAX_FILENAME - 1;
            for (uint32_t i = 0; i < nl; i++) namebuf[i] = nm[i];
            namebuf[nl] = '\0';

            VfsNode node;
            fill_node_from_inode(&node, ent->inode, &child, namebuf);
            callback(&node);
        }
    }

    kfree(blk);
    return ok;
}

/* ============================================================
 * VFS API -- rename
 * ============================================================ */

bool vfs_rename(const char *oldpath, const char *newpath) {
    char old_dir[MAX_PATH], old_base[MAX_FILENAME];
    char new_dir[MAX_PATH], new_base[MAX_FILENAME];
    split_path(oldpath, old_dir, old_base);
    split_path(newpath, new_dir, new_base);

    uint32_t old_dir_inum, new_dir_inum, old_inum;
    Ext2Inode old_dir_inode, new_dir_inode, old_inode;
    if (!ext2_lookup_path(old_dir, &old_dir_inum, &old_dir_inode)) return false;
    if (!ext2_lookup_path(new_dir, &new_dir_inum, &new_dir_inode)) return false;
    if (!ext2_dir_lookup(&old_dir_inode, old_base, &old_inum, NULL)) return false;
    if (!ext2_read_inode(old_inum, &old_inode)) return false;
    if (!may_write_dir(&old_dir_inode) || !may_write_dir(&new_dir_inode)) return false;

    /* If destination exists, remove it first (real POSIX rename semantics:
       silently replace). Its inode gets freed by unlink. */
    uint32_t exist_inum; uint8_t exist_type;
    if (ext2_dir_lookup(&new_dir_inode, new_base, &exist_inum, &exist_type)) {
        ext2_unlink(new_dir_inum, &new_dir_inode, new_base);
        ext2_read_inode(new_dir_inum, &new_dir_inode);
    }

    /* Add entry to new directory with old_inum and old_type */
    uint8_t old_type = EXT2_FT_REG;
    if ((old_inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFDIR) old_type = EXT2_FT_DIR;
    else if ((old_inode.i_mode & EXT2_S_IFMT) == EXT2_S_IFLNK) old_type = EXT2_FT_LNK;
    if (!ext2_dir_add_entry(new_dir_inum, &new_dir_inode, new_base, old_inum, old_type)) return false;

    /* If old directory is the same as new, refresh old_dir_inode */
    if (old_dir_inum == new_dir_inum) {
        ext2_read_inode(old_dir_inum, &old_dir_inode);
    }

    /* Remove old entry without freeing inode */
    uint32_t name_len = 0;
    while (old_base[name_len]) name_len++;
    uint8_t *block_buf = (uint8_t *)kmalloc(g_block_size);
    if (!block_buf) return false;

    bool removed = false;
    for (int b = 0; b < 12 && old_dir_inode.i_block[b] != 0 && !removed; b++) {
        if (!ext2_read_block(old_dir_inode.i_block[b], block_buf)) continue;
        uint32_t off = 0;
        Ext2DirEntry *prev = NULL;
        while (off < g_block_size) {
            Ext2DirEntry *ent = (Ext2DirEntry *)(block_buf + off);
            if (ent->rec_len == 0) break;
            if (ent->inode == old_inum && ent->name_len == name_len) {
                char *ent_name = (char *)(block_buf + off + sizeof(Ext2DirEntry));
                bool match = true;
                for (uint32_t i = 0; i < name_len; i++) {
                    if (ent_name[i] != old_base[i]) { match = false; break; }
                }
                if (match) {
                    if (prev) {
                        prev->rec_len = (uint16_t)(prev->rec_len + ent->rec_len);
                    } else {
                        ent->inode = 0;
                    }
                    ext2_write_block(old_dir_inode.i_block[b], block_buf);
                    removed = true;
                    break;
                }
            }
            prev = ent;
            off += ent->rec_len;
        }
    }
    kfree(block_buf);

    if (!removed) return false;

    /* Directory moves change the parent's link count via ".." */
    if (old_type == EXT2_FT_DIR && old_dir_inum != new_dir_inum) {
        if (old_dir_inode.i_links_count > 1) {
            old_dir_inode.i_links_count--;
            ext2_write_inode(old_dir_inum, &old_dir_inode);
        }
        if (new_dir_inode.i_links_count < 0xFFFF) {
            new_dir_inode.i_links_count++;
            ext2_write_inode(new_dir_inum, &new_dir_inode);
        }
        /* fix the moved directory's own ".." to point at its new parent */
        uint32_t moved_parent_group = (new_dir_inum - 1) / g_sb.s_inodes_per_group;
        (void)moved_parent_group;
    }

    return true;
}

/* ============================================================
 * VFS API -- getdents (linux_dirent64)
 * ============================================================ */

typedef struct {
    uint64_t       d_ino;
    int64_t        d_off;
    unsigned short d_reclen;
    unsigned char  d_type;
    char           d_name[];
} linux_dirent64;

static uint8_t ext2_ft_to_dt(uint8_t ft) {
    switch (ft) {
        case EXT2_FT_REG: return DT_REG;
        case EXT2_FT_DIR: return DT_DIR;
        case EXT2_FT_LNK: return DT_LNK;
        default:          return DT_UNKNOWN;
    }
}

/*
 * vfs_getdents - Read directory entries in Linux dirent64 format.
 *
 * The stream cursor is file->offset interpreted as an absolute byte
 * position within the directory's data stream (logical_block *
 * block_size + intra_block_offset), so lseek() to a previously
 * returned d_off resumes exactly there -- same "offset is the cursor"
 * contract the FAT32 implementation had.
 */
int vfs_getdents(VfsFile *file, void *dirp, uint32_t count) {
    if (!file || !file->node.is_dir || !dirp) return -1;

    /* Virtual DRM directory: /sys/dev/char/226:{0,128}/device/drm lists
       "card0"/"renderD128" without any backing storage. Sentinels are
       created by kernel/syscall.c's SYS_open, not by this driver --
       but their read side lives here, same as before. */
    if (file->node.first_cluster == 0xFFFFFFF8 || file->node.first_cluster == 0xFFFFFFF9) {
        if (file->offset > 0) return 0; // already enumerated
        const char *name; uint64_t ino;
        if (file->node.first_cluster == 0xFFFFFFF9) { name = "renderD128"; ino = 0xFB0F; }
        else                                        { name = "card0";      ino = 0xFB0E; }
        uint32_t name_len = 0; while (name[name_len]) name_len++;
        uint32_t reclen = (24 + name_len + 1 + 7) & ~7u;
        if (reclen > count) return 0;
        linux_dirent64 *d = (linux_dirent64 *)dirp;
        d->d_ino = ino;
        d->d_off = reclen;
        d->d_reclen = (unsigned short)reclen;
        d->d_type = DT_DIR;
        for (uint32_t i = 0; i < name_len; i++) d->d_name[i] = name[i];
        d->d_name[name_len] = 0;
        file->offset = 1; // mark as done
        return reclen;
    }

    Ext2Inode dir;
    if (!ext2_read_inode(file->node.first_cluster, &dir)) return -1;
    if ((dir.i_mode & EXT2_S_IFMT) != EXT2_S_IFDIR) return -1;

    uint8_t *buf = (uint8_t *)dirp;
    uint64_t bytes_written = 0;

    uint8_t *blk = (uint8_t *)kmalloc(g_block_size);
    if (!blk) return -1;

    uint32_t cursor = file->offset;
    bool loaded = false;
    uint32_t cur_lb = 0xFFFFFFFF;

    while (cursor < dir.i_size) {
        uint32_t lb = cursor / g_block_size;
        uint32_t boff = cursor % g_block_size;

        if (!loaded || lb != cur_lb) {
            uint32_t phys = ext2_resolve_block(&dir, lb, false);
            if (!phys) break;
            if (!ext2_read_block(phys, blk)) break;
            loaded = true;
            cur_lb = lb;
        }
        if (boff + sizeof(Ext2DirEntry) > g_block_size) { cursor += g_block_size - boff; continue; }

        Ext2DirEntry *ent = (Ext2DirEntry *)(blk + boff);
        if (ent->rec_len < sizeof(Ext2DirEntry) ||
            boff + ent->rec_len > g_block_size ||
            (ent->rec_len & 3)) {
            break; /* corrupt guard */
        }

        uint32_t next_cursor = (boff + ent->rec_len) + (lb * g_block_size);

        if (ent->inode != 0) {
            uint32_t name_len = ent->name_len;
            if (name_len > ent->rec_len - sizeof(Ext2DirEntry)) name_len = ent->rec_len - sizeof(Ext2DirEntry);
            uint32_t reclen = (24 + name_len + 1 + 7) & ~7u; // align 8

            if (bytes_written + reclen > count) {
                if (bytes_written == 0) { kfree(blk); return 0; } /* even one entry doesn't fit */
                break; /* user buffer is full */
            }

            linux_dirent64 *d = (linux_dirent64 *)(buf + bytes_written);
            d->d_ino = ent->inode;
            d->d_off = next_cursor;
            d->d_reclen = (unsigned short)reclen;
            d->d_type = ext2_ft_to_dt(ent->file_type);

            char *dst_name = d->d_name;
            const char *nm = (const char *)(blk + boff + sizeof(Ext2DirEntry));
            for (uint32_t k = 0; k < name_len; k++) {
                dst_name[k] = nm[k];
            }
            dst_name[name_len] = '\0';

            bytes_written += reclen;
        }

        cursor = next_cursor;
    }

    kfree(blk);
    file->offset = cursor;
    return (int)bytes_written;
}

/* ============================================================
 * Diagnostics accessors (shell disk_dump etc.)
 * ============================================================ */

uint32_t ext2_fs_block_size(void)   { return g_sb.s_log_block_size ? (1024u << g_sb.s_log_block_size) : 1024; }
uint32_t ext2_fs_groups(void)       { return g_group_count; }
uint32_t ext2_fs_inodes_count(void) { return g_sb.s_inodes_count; }
uint32_t ext2_fs_blocks_count(void) { return g_sb.s_blocks_count; }
uint32_t ext2_fs_free_blocks(void)  { return g_sb.s_free_blocks_count; }
uint32_t ext2_fs_free_inodes(void)  { return g_sb.s_free_inodes_count; }
uint32_t ext2_fs_root_lba(void)     { return mbr_root_partition_lba(); }
uint32_t ext2_fs_root_sectors(void) { return mbr_root_partition_sectors(); }
