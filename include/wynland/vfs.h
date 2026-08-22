/*
 * WynlandOS - Virtual Filesystem (VFS) Header
 *
 * Phase 4: Full FAT32 read/write support with VFS abstraction
 */
#pragma once

#include <wynland/types.h>

#define MAX_FILENAME 256
#define MAX_PATH     512

/* Open flags */
#define VFS_O_READ    0x01
#define VFS_O_WRITE   0x02
#define VFS_O_CREATE  0x04
#define VFS_O_APPEND  0x08
#define VFS_O_TRUNC   0x10

/* Seek whence */
#define VFS_SEEK_SET  0
#define VFS_SEEK_CUR  1
#define VFS_SEEK_END  2

/* FAT32 attributes */
#define FAT32_ATTR_READONLY  0x01
#define FAT32_ATTR_HIDDEN    0x02
#define FAT32_ATTR_SYSTEM    0x04
#define FAT32_ATTR_VOLUME_ID 0x08
#define FAT32_ATTR_DIRECTORY 0x10
#define FAT32_ATTR_ARCHIVE   0x20

typedef struct {
    char name[MAX_FILENAME];
    uint32_t size;
    bool is_dir;
    uint32_t first_cluster;
    bool readonly; /* Phase 5: mirrors the on-disk FAT32_ATTR_READONLY bit.
                       SYS_open() enforces write-open denial for non-root
                       processes against this -- the OS's one real
                       permission boundary, since FAT32 has no owner/mode
                       bits to build a fuller model on. */
} VfsNode;

typedef struct {
    VfsNode  node;
    uint32_t offset;
    uint32_t current_cluster;
    uint32_t current_cluster_offset; /* Byte offset within current cluster */
    uint32_t flags;                  /* VFS_O_* flags */
    uint32_t dir_entry_sector;       /* LBA sector of this file's directory entry */
    uint32_t dir_entry_offset;       /* Byte offset within that sector (0..480) */
    bool     dirty;                  /* File metadata changed, needs flush on close */
} VfsFile;

/* File statistics */
typedef struct {
    char     name[MAX_FILENAME];
    uint32_t size;
    bool     is_dir;
    uint32_t first_cluster;
    uint16_t create_time;
    uint16_t create_date;
    uint16_t write_time;
    uint16_t write_date;
    uint8_t  attr;
} VfsStat;

/* ---- Core VFS API ---- */
bool vfs_init(void);

/* File operations */
VfsFile *vfs_open(const char *path);
VfsFile *vfs_open_flags(const char *path, uint32_t flags);
int      vfs_read(VfsFile *file, void *buf, uint32_t size);
int      vfs_write(VfsFile *file, const void *buf, uint32_t size);
void     vfs_close(VfsFile *file);
int      vfs_seek(VfsFile *file, int32_t offset, int whence);
uint32_t vfs_tell(VfsFile *file);

/* Directory operations */
bool vfs_readdir(const char *path, void (*callback)(VfsNode *node));
bool vfs_mkdir(const char *path);

/* File management */
bool vfs_create(const char *path);
bool vfs_delete(const char *path);
bool vfs_stat(const char *path, VfsStat *out);
bool vfs_set_readonly(const char *path); /* Phase 5: mark FAT32_ATTR_READONLY */
