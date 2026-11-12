/* WynlandOS - inotify (kernel/inotify.c). GPL-2.0-or-later. */
#ifndef WYNLAND_INOTIFY_H
#define WYNLAND_INOTIFY_H
#include <wynland/types.h>

/* event bits (Linux's) */
#define IN_MODIFY        0x00000002u
#define IN_ATTRIB        0x00000004u
#define IN_CLOSE_WRITE   0x00000008u
#define IN_CLOSE_NOWRITE 0x00000010u
#define IN_OPEN          0x00000020u
#define IN_MOVED_FROM    0x00000040u
#define IN_MOVED_TO      0x00000080u
#define IN_CREATE        0x00000100u
#define IN_DELETE        0x00000200u
#define IN_DELETE_SELF   0x00000400u
#define IN_MOVE_SELF     0x00000800u
#define IN_ISDIR         0x40000000u

bool     inotify_active(void);           /* any watch anywhere (the filesystem's fast check) */
int      inotify_create(void);           /* slot, refs=1; <0 errno */
void     inotify_ref(int i);
void     inotify_unref(int i);
bool     inotify_readable(int i);
uint32_t inotify_pending(int i);         /* bytes of events queued (FIONREAD) */
int32_t  inotify_add(int i, uint32_t inum, uint32_t mask);   /* wd or <0 errno */
int      inotify_rm(int i, int32_t wd);
int64_t  inotify_read(int i, uint8_t *out, uint32_t cap);
uint32_t inotify_cookie(void);           /* a fresh MOVED_FROM/MOVED_TO pair id */
/* `mask` happened to inode `inum`, named `name` in directory `dir` (0: unknown) */
void     inotify_note(uint32_t dir, uint32_t inum, uint32_t mask, uint32_t cookie, const char *name);
#endif
