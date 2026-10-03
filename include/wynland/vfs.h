/*
 * WynlandOS - Virtual Filesystem (VFS) Header
 *
 * The API surface the rest of the kernel programs against. Since the
 * ext2 migration this header is filesystem-neutral: the implementation
 * lives in drivers/fs/ext2.c (root filesystem) and drivers/fs/mbr.c
 * (partition discovery). The previous FAT32 implementation is kept
 * dormant as drivers/fs/vfs.c.dormant.
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

/* Device sentinels carried in VfsNode.first_cluster for synthetic
   nodes. Real files carry their ext2 inode number in this field; inode
   counts can never approach the sentinel range. kernel/syscall.c checks
   these directly in ~40 places -- do not renumber. */
#define DEV_FB0       0xFFFFFFF0
#define DEV_MICE      0xFFFFFFF1
#define DEV_TTY       0xFFFFFFF2
#define DEV_KBD       0xFFFFFFF3
#define DEV_NULL      0xFFFFFFFE
#define DEV_URANDOM   0xFFFFFFF8

typedef struct {
    char name[MAX_FILENAME];
    uint32_t size;
    bool is_dir;
    uint32_t first_cluster; /* real file: ext2 inode number; devices: DEV_* sentinel */
    bool readonly;          /* true iff no write bit at all in the ext2 mode */
    uint16_t mode;          /* ext2 i_mode & 07777 -- real permission bits */
    uint32_t uid;           /* ext2 i_uid -- real ownership */
    uint32_t mtime;         /* ext2 i_mtime (unix seconds) */
} VfsNode;

typedef struct {
    VfsNode  node;
    uint32_t offset;
    uint32_t current_cluster;        /* unused under ext2 (kept: ABI + syscall.c sentinels) */
    uint32_t current_cluster_offset; /* unused under ext2 */
    uint32_t flags;                  /* VFS_O_* flags */
    uint32_t dir_entry_sector;       /* unused under ext2 */
    uint32_t dir_entry_offset;       /* unused under ext2 */
    bool     dirty;
} VfsFile;

/* File statistics */
typedef struct {
    char     name[MAX_FILENAME];
    uint32_t size;
    bool     is_dir;
    uint32_t first_cluster; /* consumers pass this on as st_ino */
    uint16_t create_time;   /* FAT-era fields, zeroed under ext2 */
    uint16_t create_date;
    uint16_t write_time;
    uint16_t write_date;
    uint8_t  attr;          /* low byte of the ext2 mode */
    uint16_t mode;          /* ext2 i_mode & 07777 */
    uint32_t uid;           /* ext2 i_uid */
    uint32_t mtime, atime, ctime; /* unix seconds */
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
int  vfs_getdents(VfsFile *file, void *dirp, uint32_t count); /* linux_dirent64 */

/* File management */
bool vfs_create(const char *path);
bool vfs_delete(const char *path);
bool vfs_rename(const char *oldpath, const char *newpath);
bool vfs_stat(const char *path, VfsStat *out);
bool vfs_set_readonly(const char *path); /* clears all write bits in i_mode */
bool vfs_chmod(const char *path, uint32_t mode); /* owner or root */
int  vfs_fchmod(VfsFile *file, uint32_t mode);
int  vfs_ftruncate(VfsFile *file, uint32_t len);  /* 0 ok, -1 error */   /* same, an open file: -1 not ext2, 0 EPERM, 1 ok */
bool vfs_may_access(const char *path, uint32_t want); /* 4 r, 2 w, 1 x; root always */

/* ---- ext2 mount diagnostics (shell disk_dump etc.) ---- */
uint32_t ext2_fs_block_size(void);
uint32_t ext2_fs_groups(void);
uint32_t ext2_fs_inodes_count(void);
uint32_t ext2_fs_blocks_count(void);
uint32_t ext2_fs_free_blocks(void);
uint32_t ext2_fs_free_inodes(void);
uint32_t ext2_fs_root_lba(void);
uint32_t ext2_fs_root_sectors(void);
