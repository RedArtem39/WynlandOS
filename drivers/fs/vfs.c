/*
 * WynlandOS - Virtual Filesystem & FAT32 Driver Implementation
 *
 * Phase 4: Full FAT32 read/write support
 * Supports: open, read, write, create, mkdir, delete, stat, seek
 */

#include <wynland/vfs.h>
#include <wynland/ahci.h>
#include <wynland/heap.h>
#include <wynland/types.h>
#include <wynland/boot_info.h>

extern void serial_write_string(const char *str);
extern int sata_port_num;

/* ============================================================
 * FAT32 BPB / Geometry (populated by vfs_init)
 * ============================================================ */

uint32_t bytes_per_sector = 512;
uint32_t sectors_per_cluster = 0;
uint32_t reserved_sectors = 0;
uint32_t fat_count = 0;
uint32_t sectors_per_fat = 0;
uint32_t first_fat_sector = 0;
uint32_t first_data_sector = 0;
uint32_t root_cluster = 0;
static uint32_t total_data_clusters = 0;

/* ============================================================
 * On-disk structures
 * ============================================================ */

typedef struct {
    uint8_t  jmp[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  fat_count;
    uint16_t root_dir_entries;
    uint16_t total_sectors_short;
    uint8_t  media_descriptor;
    uint16_t sectors_per_fat_short;
    uint16_t sectors_per_track;
    uint16_t head_count;
    uint32_t hidden_sectors;
    uint32_t total_sectors_long;

    /* FAT32 Extended fields */
    uint32_t sectors_per_fat_long;
    uint16_t flags;
    uint16_t fat_version;
    uint32_t root_cluster;
    uint16_t fs_info_sector;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
} __attribute__((packed)) Fat32Bpb;

typedef struct {
    char     name[11];      /* 8.3 filename */
    uint8_t  attr;          /* Attribute flags */
    uint8_t  nt_res;
    uint8_t  create_time_tenth;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t last_access_date;
    uint16_t first_cluster_high;
    uint16_t write_time;
    uint16_t write_date;
    uint16_t first_cluster_low;
    uint32_t file_size;
} __attribute__((packed)) Fat32DirEntry;

/* ============================================================
 * Local string helpers
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


/* ============================================================
 * FAT32 name formatting
 * ============================================================ */

/*
 * format_short_name - Convert 8.3 on-disk name to human-readable
 * e.g. "README  TXT" -> "readme.txt"
 */
static void format_short_name(char *dst, const char *src) {
    int name_end = 7;
    while (name_end >= 0 && src[name_end] == ' ') name_end--;

    int dst_pos = 0;
    for (int i = 0; i <= name_end; i++) {
        char c = src[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        dst[dst_pos++] = c;
    }

    int ext_end = 10;
    while (ext_end >= 8 && src[ext_end] == ' ') ext_end--;

    if (ext_end >= 8) {
        dst[dst_pos++] = '.';
        for (int i = 8; i <= ext_end; i++) {
            char c = src[i];
            if (c >= 'A' && c <= 'Z') c += 32;
            dst[dst_pos++] = c;
        }
    }
    dst[dst_pos] = '\0';
}

/*
 * format_to_83_name - Convert human-readable name to 8.3 on-disk format
 * e.g. "readme.txt" -> "README  TXT"
 */
static void format_to_83_name(char *dst, const char *src) {
    memset(dst, ' ', 11);

    int i = 0;
    int dst_pos = 0;

    /* Copy name part (up to 8 chars, before '.') */
    while (src[i] && src[i] != '.' && dst_pos < 8) {
        char c = src[i];
        if (c >= 'a' && c <= 'z') c -= 32; /* to uppercase */
        dst[dst_pos++] = c;
        i++;
    }

    /* Find and copy extension */
    if (src[i] == '.') {
        i++; /* skip dot */
        dst_pos = 8;
        while (src[i] && dst_pos < 11) {
            char c = src[i];
            if (c >= 'a' && c <= 'z') c -= 32;
            dst[dst_pos++] = c;
            i++;
        }
    }
}

/* ============================================================
 * FAT32 cluster/sector helpers
 * ============================================================ */

static uint32_t fat32_cluster_to_sector(uint32_t cluster) {
    return first_data_sector + ((cluster - 2) * sectors_per_cluster);
}

static uint32_t fat32_get_next_cluster(uint32_t cluster) {
    uint32_t fat_sector = first_fat_sector + (cluster * 4) / bytes_per_sector;
    uint32_t fat_offset = (cluster * 4) % bytes_per_sector;

    uint32_t temp[128]; /* 512 bytes */
    if (!ahci_read(fat_sector, 1, temp)) {
        return 0x0FFFFFFF;
    }

    return temp[fat_offset / 4] & 0x0FFFFFFF;
}

/* ============================================================
 * FAT32 write helpers (NEW - Phase 4)
 * ============================================================ */

/*
 * fat32_set_fat_entry - Write a value into the FAT for a given cluster
 */
static bool fat32_set_fat_entry(uint32_t cluster, uint32_t value) {
    uint32_t fat_sector = first_fat_sector + (cluster * 4) / bytes_per_sector;
    uint32_t fat_offset = (cluster * 4) % bytes_per_sector;

    uint8_t sector_buf[512];
    if (!ahci_read(fat_sector, 1, sector_buf)) return false;

    uint32_t *entry = (uint32_t *)(sector_buf + fat_offset);
    *entry = (*entry & 0xF0000000) | (value & 0x0FFFFFFF);

    return ahci_write(fat_sector, 1, sector_buf);
}

/*
 * fat32_alloc_cluster - Find a free cluster in the FAT and mark it as end-of-chain
 * Returns cluster number, or 0 on failure (disk full)
 */
static uint32_t fat32_alloc_cluster(void) {
    uint8_t sector_buf[512];
    uint32_t entries_per_sector = bytes_per_sector / 4;

    for (uint32_t s = 0; s < sectors_per_fat; s++) {
        if (!ahci_read(first_fat_sector + s, 1, sector_buf)) continue;
        uint32_t *entries = (uint32_t *)sector_buf;

        for (uint32_t i = 0; i < entries_per_sector; i++) {
            uint32_t cluster = s * entries_per_sector + i;
            if (cluster < 2) continue; /* Clusters 0,1 are reserved */

            if ((entries[i] & 0x0FFFFFFF) == 0) {
                /* Free cluster found — mark as end-of-chain */
                entries[i] = (entries[i] & 0xF0000000) | 0x0FFFFFFF;
                if (!ahci_write(first_fat_sector + s, 1, sector_buf)) return 0;

                /* Zero out the new cluster */
                uint8_t zero[512];
                memset(zero, 0, 512);
                uint32_t sect = fat32_cluster_to_sector(cluster);
                for (uint32_t z = 0; z < sectors_per_cluster; z++) {
                    ahci_write(sect + z, 1, zero);
                }

                return cluster;
            }
        }
    }

    serial_write_string("FAT32: No free clusters - disk full!\r\n");
    return 0;
}

/*
 * fat32_free_chain - Free an entire cluster chain starting from 'cluster'
 */
static void fat32_free_chain(uint32_t cluster) {
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t next = fat32_get_next_cluster(cluster);
        fat32_set_fat_entry(cluster, 0x00000000);
        cluster = next;
    }
}

/* ============================================================
 * Path manipulation helpers
 * ============================================================ */

/*
 * split_path - Split a path into parent directory and filename
 * "/foo/bar/test.txt" -> parent="/foo/bar", name="test.txt"
 * "/test.txt"         -> parent="/",        name="test.txt"
 * "test.txt"          -> parent="/",        name="test.txt"
 */
static bool split_path(const char *path, char *parent, char *name) {
    int last_slash = -1;
    int len = 0;
    while (path[len]) {
        if (path[len] == '/') last_slash = len;
        len++;
    }

    if (len == 0) return false;

    if (last_slash < 0) {
        /* No slash: parent is root */
        str_copy(parent, "/");
        str_copy(name, path);
        return true;
    }

    if (last_slash == 0) {
        /* File directly in root: "/test.txt" */
        str_copy(parent, "/");
        str_copy(name, path + 1);
        return true;
    }

    /* Copy parent path */
    for (int i = 0; i < last_slash; i++) {
        parent[i] = path[i];
    }
    parent[last_slash] = '\0';

    /* Copy filename */
    str_copy(name, path + last_slash + 1);
    return true;
}

/* ============================================================
 * FAT32 directory operations
 * ============================================================ */

/*
 * fat32_find_in_dir - Search a directory for a file/subdir by name
 *
 * Optionally returns the sector/offset of the directory entry found,
 * so callers can modify/delete it later.
 */
static bool fat32_find_in_dir(uint32_t dir_cluster, const char *name,
                               VfsNode *out_node,
                               uint32_t *out_entry_sector,
                               uint32_t *out_entry_offset) {
    uint32_t curr_cluster = dir_cluster;

    char short_name[11];
    format_to_83_name(short_name, name);

    while (curr_cluster < 0x0FFFFFF8) {
        uint32_t base_sector = fat32_cluster_to_sector(curr_cluster);

        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            uint8_t sector_buf[512];
            if (!ahci_read(base_sector + s, 1, sector_buf)) {
                return false;
            }

            Fat32DirEntry *entries = (Fat32DirEntry *)sector_buf;
            uint32_t entries_per_sector = 512 / sizeof(Fat32DirEntry);

            for (uint32_t i = 0; i < entries_per_sector; i++) {
                Fat32DirEntry *e = &entries[i];

                if (e->name[0] == 0x00) return false;             /* End of directory */
                if ((uint8_t)e->name[0] == 0xE5) continue;       /* Deleted entry */
                if (e->attr == 0x0F) continue;                    /* LFN entry */

                bool match = true;
                for (int k = 0; k < 11; k++) {
                    if (e->name[k] != short_name[k]) {
                        match = false;
                        break;
                    }
                }

                if (match) {
                    format_short_name(out_node->name, e->name);
                    out_node->size = e->file_size;
                    out_node->is_dir = (e->attr & 0x10) != 0;
                    out_node->first_cluster = e->first_cluster_low |
                                              ((uint32_t)e->first_cluster_high << 16);

                    if (out_entry_sector) *out_entry_sector = base_sector + s;
                    if (out_entry_offset) *out_entry_offset = i * sizeof(Fat32DirEntry);
                    return true;
                }
            }
        }
        curr_cluster = fat32_get_next_cluster(curr_cluster);
    }

    return false;
}

/*
 * vfs_lookup_path - Resolve a full path to a VfsNode
 *
 * Optionally returns the sector/offset of the final entry's dir record.
 */
static bool vfs_lookup_path(const char *path, VfsNode *out_node,
                             uint32_t *out_entry_sector,
                             uint32_t *out_entry_offset) {
    if (str_compare(path, "/") == 0) {
        out_node->is_dir = true;
        out_node->first_cluster = root_cluster;
        out_node->size = 0;
        str_copy(out_node->name, "/");
        if (out_entry_sector) *out_entry_sector = 0;
        if (out_entry_offset) *out_entry_offset = 0;
        return true;
    }

    char path_copy[MAX_PATH];
    str_copy(path_copy, path);

    uint32_t curr_cluster = root_cluster;
    char *p = path_copy;
    if (*p == '/') p++;

    while (*p) {
        char *slash = p;
        while (*slash && *slash != '/') slash++;

        bool last = (*slash == '\0');
        *slash = '\0';

        VfsNode next_node;
        uint32_t es = 0, eo = 0;
        if (!fat32_find_in_dir(curr_cluster, p, &next_node, &es, &eo)) {
            return false;
        }

        if (last) {
            *out_node = next_node;
            if (out_entry_sector) *out_entry_sector = es;
            if (out_entry_offset) *out_entry_offset = eo;
            return true;
        }

        if (!next_node.is_dir) return false;
        curr_cluster = next_node.first_cluster;
        p = slash + 1;
    }

    return false;
}

/*
 * fat32_add_dir_entry - Add a new directory entry to a directory
 *
 * Returns the sector/offset of the newly created entry.
 */
static bool fat32_add_dir_entry(uint32_t dir_cluster, const char *short_name,
                                 uint8_t attr, uint32_t first_cluster,
                                 uint32_t file_size,
                                 uint32_t *out_entry_sector,
                                 uint32_t *out_entry_offset) {
    uint32_t curr_cluster = dir_cluster;
    uint32_t prev_cluster = dir_cluster;

    while (curr_cluster < 0x0FFFFFF8) {
        uint32_t base_sector = fat32_cluster_to_sector(curr_cluster);

        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            uint8_t sector_buf[512];
            if (!ahci_read(base_sector + s, 1, sector_buf)) return false;

            Fat32DirEntry *entries = (Fat32DirEntry *)sector_buf;
            uint32_t entries_per_sector = 512 / sizeof(Fat32DirEntry);

            for (uint32_t i = 0; i < entries_per_sector; i++) {
                if (entries[i].name[0] == 0x00 || (uint8_t)entries[i].name[0] == 0xE5) {
                    /* Found a free slot */
                    memset(&entries[i], 0, sizeof(Fat32DirEntry));
                    memcpy(entries[i].name, short_name, 11);
                    entries[i].attr = attr;
                    entries[i].first_cluster_low = first_cluster & 0xFFFF;
                    entries[i].first_cluster_high = (first_cluster >> 16) & 0xFFFF;
                    entries[i].file_size = file_size;

                    if (!ahci_write(base_sector + s, 1, sector_buf)) return false;

                    if (out_entry_sector) *out_entry_sector = base_sector + s;
                    if (out_entry_offset) *out_entry_offset = i * sizeof(Fat32DirEntry);
                    return true;
                }
            }
        }
        prev_cluster = curr_cluster;
        curr_cluster = fat32_get_next_cluster(curr_cluster);
    }

    /* No free slot found — allocate a new cluster for the directory */
    uint32_t new_cluster = fat32_alloc_cluster();
    if (new_cluster == 0) return false;

    /* Link to directory chain */
    fat32_set_fat_entry(prev_cluster, new_cluster);
    fat32_set_fat_entry(new_cluster, 0x0FFFFFFF);

    /* Write the entry at the start of the new cluster */
    uint8_t sector_buf[512];
    memset(sector_buf, 0, 512);

    Fat32DirEntry *entry = (Fat32DirEntry *)sector_buf;
    memcpy(entry->name, short_name, 11);
    entry->attr = attr;
    entry->first_cluster_low = first_cluster & 0xFFFF;
    entry->first_cluster_high = (first_cluster >> 16) & 0xFFFF;
    entry->file_size = file_size;

    uint32_t new_sector = fat32_cluster_to_sector(new_cluster);
    if (!ahci_write(new_sector, 1, sector_buf)) return false;

    if (out_entry_sector) *out_entry_sector = new_sector;
    if (out_entry_offset) *out_entry_offset = 0;
    return true;
}

/* ============================================================
 * VFS Initialization
 * ============================================================ */

bool vfs_init(void) {
    ahci_init();

    if (sata_port_num == -1) {
        serial_write_string("VFS Error: AHCI not initialized, cannot init VFS!\r\n");
        return false;
    }

    uint8_t bpb_sector[512];
    if (!ahci_read(0, 1, bpb_sector)) {
        serial_write_string("VFS Error: Failed to read FAT32 BPB (sector 0)!\r\n");
        return false;
    }

    Fat32Bpb *bpb = (Fat32Bpb *)bpb_sector;
    if (bpb->boot_signature != 0x29) {
        serial_write_string("VFS Error: Invalid boot signature in BPB!\r\n");
        return false;
    }

    bytes_per_sector = bpb->bytes_per_sector;
    sectors_per_cluster = bpb->sectors_per_cluster;
    reserved_sectors = bpb->reserved_sectors;
    fat_count = bpb->fat_count;
    sectors_per_fat = bpb->sectors_per_fat_long;
    root_cluster = bpb->root_cluster;

    first_fat_sector = reserved_sectors;
    first_data_sector = reserved_sectors + (fat_count * sectors_per_fat);

    uint32_t total_sectors = bpb->total_sectors_long;
    if (total_sectors == 0) total_sectors = bpb->total_sectors_short;
    total_data_clusters = (total_sectors - first_data_sector) / sectors_per_cluster;

    serial_write_string("VFS: FAT32 filesystem detected.\r\n");
    serial_write_string("VFS: Sectors per cluster: ");
    char buf[16];
    extern void uint_to_str(uint64_t val, char *buf);
    uint_to_str(sectors_per_cluster, buf);
    serial_write_string(buf);
    serial_write_string("\r\n");

    serial_write_string("VFS: Total data clusters: ");
    uint_to_str(total_data_clusters, buf);
    serial_write_string(buf);
    serial_write_string("\r\n");

    return true;
}

/* ============================================================
 * File Open / Close
 * ============================================================ */

VfsFile *vfs_open(const char *path) {
    return vfs_open_flags(path, VFS_O_READ);
}

VfsFile *vfs_open_flags(const char *path, uint32_t flags) {
    if (str_compare(path, "/dev/fb0") == 0) {
        VfsFile *file = (VfsFile *)kmalloc(sizeof(VfsFile));
        if (!file) return NULL;
        extern BootInfo *g_boot_info;
        str_copy(file->node.name, "fb0");
        file->node.size = g_boot_info ? (g_boot_info->fb_pitch * g_boot_info->fb_height) : 0;
        file->node.is_dir = false;
        file->node.first_cluster = 0xFFFFFFF0; // DEV_FB0
        file->offset = 0;
        file->flags = flags;
        file->dirty = false;
        return file;
    }
    if (str_compare(path, "/dev/input/mice") == 0) {
        VfsFile *file = (VfsFile *)kmalloc(sizeof(VfsFile));
        if (!file) return NULL;
        str_copy(file->node.name, "mice");
        file->node.size = 0;
        file->node.is_dir = false;
        file->node.first_cluster = 0xFFFFFFF1; // DEV_MICE
        file->offset = 0;
        file->flags = flags;
        file->dirty = false;
        return file;
    }
    if (str_compare(path, "/dev/tty") == 0) {
        VfsFile *file = (VfsFile *)kmalloc(sizeof(VfsFile));
        if (!file) return NULL;
        str_copy(file->node.name, "tty");
        file->node.size = 0;
        file->node.is_dir = false;
        file->node.first_cluster = 0xFFFFFFF2; // DEV_TTY
        file->offset = 0;
        file->flags = flags;
        file->dirty = false;
        return file;
    }

    VfsNode node;
    uint32_t entry_sector = 0, entry_offset = 0;
    bool found = vfs_lookup_path(path, &node, &entry_sector, &entry_offset);

    if (!found) {
        if (flags & VFS_O_CREATE) {
            if (!vfs_create(path)) return NULL;
            found = vfs_lookup_path(path, &node, &entry_sector, &entry_offset);
            if (!found) return NULL;
        } else {
            return NULL;
        }
    }

    if (node.is_dir) return NULL;

    VfsFile *file = (VfsFile *)kmalloc(sizeof(VfsFile));
    if (!file) return NULL;

    file->node = node;
    file->flags = flags;
    file->dir_entry_sector = entry_sector;
    file->dir_entry_offset = entry_offset;
    file->dirty = false;

    if (flags & VFS_O_TRUNC) {
        /* Truncate: free existing cluster chain */
        if (node.first_cluster >= 2) {
            fat32_free_chain(node.first_cluster);
        }
        file->node.size = 0;
        file->node.first_cluster = 0;
        file->current_cluster = 0;
        file->current_cluster_offset = 0;
        file->offset = 0;
        file->dirty = true;
    } else if (flags & VFS_O_APPEND) {
        /* Position at end of file */
        file->offset = file->node.size;
        if (file->node.first_cluster >= 2 && file->node.size > 0) {
            uint32_t cluster_size = sectors_per_cluster * 512;
            uint32_t cluster = file->node.first_cluster;
            uint32_t remaining = file->node.size;
            while (remaining > cluster_size) {
                uint32_t next = fat32_get_next_cluster(cluster);
                if (next >= 0x0FFFFFF8) break;
                cluster = next;
                remaining -= cluster_size;
            }
            file->current_cluster = cluster;
            file->current_cluster_offset = remaining;
        } else {
            file->current_cluster = file->node.first_cluster;
            file->current_cluster_offset = 0;
        }
    } else {
        file->offset = 0;
        file->current_cluster = node.first_cluster;
        file->current_cluster_offset = 0;
    }

    return file;
}

void vfs_close(VfsFile *file) {
    if (!file) return;

    /* Flush dirty metadata (file size / first cluster) back to directory entry */
    if (file->dirty && file->dir_entry_sector != 0) {
        uint8_t sector_buf[512];
        if (ahci_read(file->dir_entry_sector, 1, sector_buf)) {
            Fat32DirEntry *entry = (Fat32DirEntry *)(sector_buf + file->dir_entry_offset);
            entry->file_size = file->node.size;
            entry->first_cluster_low = file->node.first_cluster & 0xFFFF;
            entry->first_cluster_high = (file->node.first_cluster >> 16) & 0xFFFF;
            ahci_write(file->dir_entry_sector, 1, sector_buf);
        }
    }

    kfree(file);
}

/* ============================================================
 * File Read
 * ============================================================ */

int vfs_read(VfsFile *file, void *buf, uint32_t size) {
    if (!file) return -1;

    if (file->node.first_cluster >= 0xFFFFFFF0) {
        if (file->node.first_cluster == 0xFFFFFFF0) { // DEV_FB0
            extern BootInfo *g_boot_info;
            if (!g_boot_info) return 0;
            uint32_t fb_size = g_boot_info->fb_pitch * g_boot_info->fb_height;
            if (file->offset >= fb_size) return 0;
            if (file->offset + size > fb_size) {
                size = fb_size - file->offset;
            }
            uint8_t *fb = (uint8_t *)(uintptr_t)g_boot_info->fb_addr;
            for (uint32_t i = 0; i < size; i++) {
                ((uint8_t *)buf)[i] = fb[file->offset + i];
            }
            file->offset += size;
            return size;
        }
        if (file->node.first_cluster == 0xFFFFFFF1) { // DEV_MICE
            extern int mouse_read_queue(uint8_t *buf, int size);
            return mouse_read_queue((uint8_t *)buf, size);
        }
        if (file->node.first_cluster == 0xFFFFFFF2) { // DEV_TTY
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
            return read_bytes;
        }
        return 0;
    }

    if (file->offset >= file->node.size) return 0;

    if (file->offset + size > file->node.size) {
        size = file->node.size - file->offset;
    }

    uint32_t bytes_read = 0;
    uint8_t *dst = (uint8_t *)buf;
    uint32_t cluster_size = sectors_per_cluster * 512;

    while (bytes_read < size) {
        uint32_t cluster_bytes_rem = cluster_size - file->current_cluster_offset;
        if (cluster_bytes_rem == 0) {
            file->current_cluster = fat32_get_next_cluster(file->current_cluster);
            if (file->current_cluster >= 0x0FFFFFF8) {
                break;
            }
            file->current_cluster_offset = 0;
            cluster_bytes_rem = cluster_size;
        }

        uint32_t to_copy = size - bytes_read;
        if (to_copy > cluster_bytes_rem) {
            to_copy = cluster_bytes_rem;
        }

        uint32_t sector_in_cluster = file->current_cluster_offset / 512;
        uint32_t offset_in_sector = file->current_cluster_offset % 512;
        uint32_t cluster_sector = fat32_cluster_to_sector(file->current_cluster);

        uint8_t sector_buf[512];
        if (!ahci_read(cluster_sector + sector_in_cluster, 1, sector_buf)) {
            break;
        }

        uint32_t copy_now = 512 - offset_in_sector;
        if (copy_now > to_copy) {
            copy_now = to_copy;
        }

        for (uint32_t i = 0; i < copy_now; i++) {
            dst[bytes_read + i] = sector_buf[offset_in_sector + i];
        }

        bytes_read += copy_now;
        file->offset += copy_now;
        file->current_cluster_offset += copy_now;
    }

    return bytes_read;
}

/* ============================================================
 * File Write (NEW - Phase 4)
 * ============================================================ */

int vfs_write(VfsFile *file, const void *buf, uint32_t size) {
    if (!file) return -1;

    if (file->node.first_cluster >= 0xFFFFFFF0) {
        if (file->node.first_cluster == 0xFFFFFFF0) { // DEV_FB0
            extern BootInfo *g_boot_info;
            if (!g_boot_info) return 0;
            uint32_t fb_size = g_boot_info->fb_pitch * g_boot_info->fb_height;
            if (file->offset >= fb_size) return 0;
            if (file->offset + size > fb_size) {
                size = fb_size - file->offset;
            }
            uint8_t *fb = (uint8_t *)(uintptr_t)g_boot_info->fb_addr;
            for (uint32_t i = 0; i < size; i++) {
                fb[file->offset + i] = ((const uint8_t *)buf)[i];
            }
            file->offset += size;
            return size;
        }
        if (file->node.first_cluster == 0xFFFFFFF2) { // DEV_TTY
            extern BootInfo *g_boot_info;
            extern uint32_t term_bg_color;
            extern void console_print_char(BootInfo *info, char c, uint32_t fg, uint32_t bg);
            extern void serial_write_string(const char *str);
            
            const char *cbuf = (const char *)buf;
            for (uint32_t i = 0; i < size; i++) {
                char ch = cbuf[i];
                char single[2] = {ch, '\0'};
                serial_write_string(single);
                if (g_boot_info) {
                    console_print_char(g_boot_info, ch, 0x00FFFFFF, term_bg_color);
                }
            }
            return size;
        }
        return -1; // Write not supported on stream devices like mice
    }

    if (!(file->flags & VFS_O_WRITE)) return -1;
    if (size == 0) return 0;

    uint32_t bytes_written = 0;
    const uint8_t *src = (const uint8_t *)buf;
    uint32_t cluster_size = sectors_per_cluster * 512;

    /* If the file has no clusters yet, allocate the first one */
    if (file->node.first_cluster < 2) {
        uint32_t new_cluster = fat32_alloc_cluster();
        if (new_cluster == 0) return -1; /* Disk full */
        file->node.first_cluster = new_cluster;
        file->current_cluster = new_cluster;
        file->current_cluster_offset = 0;
        file->dirty = true;
    }

    while (bytes_written < size) {
        uint32_t cluster_bytes_rem = cluster_size - file->current_cluster_offset;

        if (cluster_bytes_rem == 0) {
            /* Need next cluster */
            uint32_t next = fat32_get_next_cluster(file->current_cluster);
            if (next >= 0x0FFFFFF8) {
                /* Allocate a new cluster and chain it */
                next = fat32_alloc_cluster();
                if (next == 0) break; /* Disk full */
                fat32_set_fat_entry(file->current_cluster, next);
                /* alloc_cluster already marks the new cluster as 0x0FFFFFFF */
            }
            file->current_cluster = next;
            file->current_cluster_offset = 0;
            cluster_bytes_rem = cluster_size;
        }

        /* Write sector by sector */
        uint32_t sector_in_cluster = file->current_cluster_offset / 512;
        uint32_t offset_in_sector = file->current_cluster_offset % 512;
        uint32_t cluster_sector = fat32_cluster_to_sector(file->current_cluster);

        uint8_t sector_buf[512];

        /* Read-modify-write if we're not writing a full sector from offset 0 */
        if (offset_in_sector != 0 || (size - bytes_written) < 512) {
            if (!ahci_read(cluster_sector + sector_in_cluster, 1, sector_buf)) break;
        } else {
            memset(sector_buf, 0, 512);
        }

        uint32_t write_now = 512 - offset_in_sector;
        uint32_t remaining = size - bytes_written;
        if (write_now > remaining) write_now = remaining;

        memcpy(sector_buf + offset_in_sector, src + bytes_written, write_now);

        if (!ahci_write(cluster_sector + sector_in_cluster, 1, sector_buf)) break;

        bytes_written += write_now;
        file->offset += write_now;
        file->current_cluster_offset += write_now;
    }

    /* Update file size if we wrote past the end */
    if (file->offset > file->node.size) {
        file->node.size = file->offset;
        file->dirty = true;
    }

    return bytes_written;
}

/* ============================================================
 * File Seek / Tell
 * ============================================================ */

int vfs_seek(VfsFile *file, int32_t offset, int whence) {
    if (!file) return -1;

    if (file->node.first_cluster >= 0xFFFFFFF0) {
        if (file->node.first_cluster == 0xFFFFFFF0) { // DEV_FB0
            int32_t new_offset;
            switch (whence) {
                case VFS_SEEK_SET: new_offset = offset; break;
                case VFS_SEEK_CUR: new_offset = (int32_t)file->offset + offset; break;
                case VFS_SEEK_END: new_offset = (int32_t)file->node.size + offset; break;
                default: return -1;
            }
            if (new_offset < 0) new_offset = 0;
            if ((uint32_t)new_offset > file->node.size) {
                new_offset = (int32_t)file->node.size;
            }
            file->offset = (uint32_t)new_offset;
            return 0;
        }
        return -1; // SEEK not supported on stream devices like mice or tty
    }

    int32_t new_offset;
    switch (whence) {
        case VFS_SEEK_SET: new_offset = offset; break;
        case VFS_SEEK_CUR: new_offset = (int32_t)file->offset + offset; break;
        case VFS_SEEK_END: new_offset = (int32_t)file->node.size + offset; break;
        default: return -1;
    }

    if (new_offset < 0) new_offset = 0;
    if ((uint32_t)new_offset > file->node.size) {
        new_offset = (int32_t)file->node.size;
    }

    /* Navigate to the correct cluster */
    file->offset = (uint32_t)new_offset;
    uint32_t cluster_size = sectors_per_cluster * 512;
    uint32_t cluster = file->node.first_cluster;

    if (cluster < 2 || new_offset == 0) {
        file->current_cluster = file->node.first_cluster;
        file->current_cluster_offset = 0;
        return 0;
    }

    uint32_t remaining = (uint32_t)new_offset;
    while (remaining >= cluster_size) {
        uint32_t next = fat32_get_next_cluster(cluster);
        if (next >= 0x0FFFFFF8) break;
        cluster = next;
        remaining -= cluster_size;
    }

    file->current_cluster = cluster;
    file->current_cluster_offset = remaining;
    return 0;
}

uint32_t vfs_tell(VfsFile *file) {
    if (!file) return 0;
    return file->offset;
}

/* ============================================================
 * File/Directory Creation (NEW - Phase 4)
 * ============================================================ */

bool vfs_create(const char *path) {
    char parent[MAX_PATH];
    char name[MAX_FILENAME];
    if (!split_path(path, parent, name)) return false;

    if (str_len_local(name) == 0) return false;

    /* Check if file already exists */
    VfsNode existing;
    if (vfs_lookup_path(path, &existing, NULL, NULL)) {
        serial_write_string("VFS: File already exists: ");
        serial_write_string(name);
        serial_write_string("\r\n");
        return false;
    }

    /* Find parent directory */
    uint32_t parent_cluster;
    if (str_compare(parent, "/") == 0) {
        parent_cluster = root_cluster;
    } else {
        VfsNode parent_node;
        if (!vfs_lookup_path(parent, &parent_node, NULL, NULL)) return false;
        if (!parent_node.is_dir) return false;
        parent_cluster = parent_node.first_cluster;
    }

    /* Convert to 8.3 name */
    char short_name[11];
    format_to_83_name(short_name, name);

    /* Add directory entry: archive attribute, no cluster, size 0 */
    bool ok = fat32_add_dir_entry(parent_cluster, short_name,
                                   FAT32_ATTR_ARCHIVE, 0, 0, NULL, NULL);

    if (ok) {
        serial_write_string("VFS: Created file: ");
        serial_write_string(name);
        serial_write_string("\r\n");
    }
    return ok;
}

bool vfs_mkdir(const char *path) {
    char parent[MAX_PATH];
    char name[MAX_FILENAME];
    if (!split_path(path, parent, name)) return false;

    if (str_len_local(name) == 0) return false;

    /* Check if already exists */
    VfsNode existing;
    if (vfs_lookup_path(path, &existing, NULL, NULL)) {
        serial_write_string("VFS: Directory already exists: ");
        serial_write_string(name);
        serial_write_string("\r\n");
        return false;
    }

    /* Find parent directory */
    uint32_t parent_cluster;
    if (str_compare(parent, "/") == 0) {
        parent_cluster = root_cluster;
    } else {
        VfsNode parent_node;
        if (!vfs_lookup_path(parent, &parent_node, NULL, NULL)) return false;
        if (!parent_node.is_dir) return false;
        parent_cluster = parent_node.first_cluster;
    }

    /* Allocate a cluster for the new directory */
    uint32_t dir_cluster = fat32_alloc_cluster();
    if (dir_cluster == 0) return false;

    /* Initialize with . and .. entries */
    uint8_t sector_buf[512];
    memset(sector_buf, 0, 512);

    Fat32DirEntry *dot = (Fat32DirEntry *)sector_buf;
    memset(dot->name, ' ', 11);
    dot->name[0] = '.';
    dot->attr = FAT32_ATTR_DIRECTORY;
    dot->first_cluster_low = dir_cluster & 0xFFFF;
    dot->first_cluster_high = (dir_cluster >> 16) & 0xFFFF;

    Fat32DirEntry *dotdot = (Fat32DirEntry *)(sector_buf + sizeof(Fat32DirEntry));
    memset(dotdot->name, ' ', 11);
    dotdot->name[0] = '.';
    dotdot->name[1] = '.';
    dotdot->attr = FAT32_ATTR_DIRECTORY;
    dotdot->first_cluster_low = parent_cluster & 0xFFFF;
    dotdot->first_cluster_high = (parent_cluster >> 16) & 0xFFFF;

    uint32_t dir_sector = fat32_cluster_to_sector(dir_cluster);
    if (!ahci_write(dir_sector, 1, sector_buf)) {
        fat32_free_chain(dir_cluster);
        return false;
    }

    /* Add entry in parent directory */
    char short_name[11];
    format_to_83_name(short_name, name);

    bool ok = fat32_add_dir_entry(parent_cluster, short_name,
                                   FAT32_ATTR_DIRECTORY, dir_cluster, 0, NULL, NULL);

    if (ok) {
        serial_write_string("VFS: Created directory: ");
        serial_write_string(name);
        serial_write_string("\r\n");
    } else {
        fat32_free_chain(dir_cluster);
    }

    return ok;
}

/* ============================================================
 * File Deletion (NEW - Phase 4)
 * ============================================================ */

bool vfs_delete(const char *path) {
    VfsNode node;
    uint32_t entry_sector = 0, entry_offset = 0;

    if (!vfs_lookup_path(path, &node, &entry_sector, &entry_offset)) {
        serial_write_string("VFS: File not found for deletion: ");
        serial_write_string(path);
        serial_write_string("\r\n");
        return false;
    }

    /* Don't delete root */
    if (str_compare(path, "/") == 0) return false;

    /* If it's a directory, check it's empty (only . and .. entries) */
    if (node.is_dir) {
        bool has_entries = false;

        uint32_t curr_cluster = node.first_cluster;
        while (curr_cluster < 0x0FFFFFF8 && !has_entries) {
            uint32_t base_sector = fat32_cluster_to_sector(curr_cluster);
            for (uint32_t s = 0; s < sectors_per_cluster && !has_entries; s++) {
                uint8_t sbuf[512];
                if (!ahci_read(base_sector + s, 1, sbuf)) break;

                Fat32DirEntry *entries = (Fat32DirEntry *)sbuf;
                for (uint32_t i = 0; i < 512 / sizeof(Fat32DirEntry); i++) {
                    if (entries[i].name[0] == 0x00) break;
                    if ((uint8_t)entries[i].name[0] == 0xE5) continue;
                    if (entries[i].attr == 0x0F) continue;

                    /* Skip . and .. */
                    char formatted[32];
                    format_short_name(formatted, entries[i].name);
                    if (str_compare(formatted, ".") == 0 || str_compare(formatted, "..") == 0) {
                        continue;
                    }

                    has_entries = true;
                    break;
                }
            }
            curr_cluster = fat32_get_next_cluster(curr_cluster);
        }

        if (has_entries) {
            serial_write_string("VFS: Cannot delete non-empty directory\r\n");
            return false;
        }
    }

    /* Free the cluster chain */
    if (node.first_cluster >= 2) {
        fat32_free_chain(node.first_cluster);
    }

    /* Mark directory entry as deleted (0xE5) */
    if (entry_sector != 0) {
        uint8_t sector_buf[512];
        if (!ahci_read(entry_sector, 1, sector_buf)) return false;
        sector_buf[entry_offset] = 0xE5;
        if (!ahci_write(entry_sector, 1, sector_buf)) return false;
    }

    serial_write_string("VFS: Deleted: ");
    serial_write_string(path);
    serial_write_string("\r\n");
    return true;
}

/* ============================================================
 * File Stat (NEW - Phase 4)
 * ============================================================ */

bool vfs_stat(const char *path, VfsStat *out) {
    VfsNode node;
    uint32_t entry_sector = 0, entry_offset = 0;

    if (!vfs_lookup_path(path, &node, &entry_sector, &entry_offset)) {
        return false;
    }

    str_copy(out->name, node.name);
    out->size = node.size;
    out->is_dir = node.is_dir;
    out->first_cluster = node.first_cluster;
    out->attr = 0;
    out->create_time = 0;
    out->create_date = 0;
    out->write_time = 0;
    out->write_date = 0;

    /* Read the raw directory entry for timestamps */
    if (entry_sector != 0) {
        uint8_t sector_buf[512];
        if (ahci_read(entry_sector, 1, sector_buf)) {
            Fat32DirEntry *e = (Fat32DirEntry *)(sector_buf + entry_offset);
            out->create_time = e->create_time;
            out->create_date = e->create_date;
            out->write_time = e->write_time;
            out->write_date = e->write_date;
            out->attr = e->attr;
        }
    }

    return true;
}

/* ============================================================
 * Directory Listing
 * ============================================================ */

bool vfs_readdir(const char *path, void (*callback)(VfsNode *node)) {
    uint32_t dir_cluster = root_cluster;
    if (str_compare(path, "/") != 0) {
        VfsNode dir_node;
        if (!vfs_lookup_path(path, &dir_node, NULL, NULL)) return false;
        if (!dir_node.is_dir) return false;
        dir_cluster = dir_node.first_cluster;
    }

    uint32_t curr_cluster = dir_cluster;

    while (curr_cluster < 0x0FFFFFF8) {
        uint32_t base_sector = fat32_cluster_to_sector(curr_cluster);

        for (uint32_t s = 0; s < sectors_per_cluster; s++) {
            uint8_t sector_buf[512];
            if (!ahci_read(base_sector + s, 1, sector_buf)) {
                return false;
            }

            Fat32DirEntry *entries = (Fat32DirEntry *)sector_buf;
            uint32_t entries_per_sector = 512 / sizeof(Fat32DirEntry);

            for (uint32_t i = 0; i < entries_per_sector; i++) {
                Fat32DirEntry *e = &entries[i];
                if (e->name[0] == 0x00) {
                    return true; /* End of directory */
                }
                if ((uint8_t)e->name[0] == 0xE5) continue;
                if (e->attr == 0x0F) continue;

                VfsNode node;
                format_short_name(node.name, e->name);
                node.size = e->file_size;
                node.is_dir = (e->attr & 0x10) != 0;
                node.first_cluster = e->first_cluster_low |
                                     ((uint32_t)e->first_cluster_high << 16);

                if (str_compare(node.name, ".") == 0 || str_compare(node.name, "..") == 0) {
                    continue;
                }

                callback(&node);
            }
        }

        curr_cluster = fat32_get_next_cluster(curr_cluster);
    }

    return true;
}
