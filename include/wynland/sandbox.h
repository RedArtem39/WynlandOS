/*
 * WynlandOS - sandboxing: seccomp-bpf, no_new_privs, chroot, and user, pid
 * and network namespaces (kernel/sandbox.c).
 *
 * What browsers build their sandboxes from on Linux: Chromium wants
 * seccomp-bpf, user + pid + net namespaces and chroot; Firefox seccomp-bpf
 * and namespaces when it can have them; WebKit seccomp plus bubblewrap
 * (which also needs mount namespaces -- not here yet).
 *
 * Simplifications against Linux, all on the stricter side:
 *  - seccomp filters are per PROCESS, not per thread: installing one
 *    filters every thread at once (what SECCOMP_FILTER_FLAG_TSYNC asks
 *    for; programs that filter one thread only get all of them filtered).
 *  - chroot() moves the working directory to the new root too.
 *  - a user namespace maps one range of ids; ids not mapped read as 65534.
 *  - CLONE_NEWUTS / NEWIPC / NEWCGROUP / NEWTIME are accepted and isolate
 *    nothing (there is no hostname to set, no SysV IPC, no cgroups).
 *  - mounts are bind mounts of the one ext2 filesystem and tmpfs (a fresh
 *    directory of it, hidden, gone with its last mount); proc, sysfs,
 *    devpts, mqueue and cgroup mounts are accepted and change nothing (those
 *    files exist by name everywhere). MS_REC binds the directory without
 *    the mounts under it; propagation flags are accepted, mounts never
 *    propagate. Read-only mounts refuse changes by path and through files
 *    opened on them; a file opened for writing before a read-only remount
 *    keeps writing. At most 256 mounts per namespace, 16 stacked.
 *
 * Copyright (C) 2026 Red_Artem39. GPL-2.0-or-later.
 */
#pragma once
#include <wynland/types.h>

struct Process;
struct Thread;
typedef struct SeccompFilter SeccompFilter;
typedef struct UserNs UserNs;
typedef struct PidNs PidNs;
typedef struct MntNs MntNs;

/* capabilities (the bits Linux uses) */
#define CAP_SETGID     6
#define CAP_SETUID     7
#define CAP_SYS_CHROOT 18
#define CAP_SYS_ADMIN  21
#define CAP_ALL        0x000001FFFFFFFFFFULL   /* 0..40 */

/* ---- seccomp: in the syscall path ---- */
/* Run the process's filters on syscall `nr` (args a[0..5], ip = user RIP
   after the syscall instruction). true: the syscall must not run and
   *ret is its result (an errno, or the SIGSYS trap's). May not return
   (KILL actions). */
bool seccomp_filter_syscall(uint64_t nr, const uint64_t a[6], uint64_t ip, uint64_t *ret);
int64_t sandbox_seccomp(uint32_t op, uint32_t flags, uint64_t uargs);
int64_t sandbox_prctl(int option, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5);
int64_t sandbox_capget(uint64_t hdr, uint64_t data);
int64_t sandbox_capset(uint64_t hdr, uint64_t data);

/* ---- process life ---- */
/* before a fork/spawn/new thread creates anything: may it (its pid
   namespace alive, room for its id)? -errno or 0 */
int64_t sandbox_fork_check(struct Process *parent);
int64_t sandbox_thread_check(struct Process *p);
void sandbox_fork(struct Process *child, struct Process *parent);   /* inherit everything */
void sandbox_exec(struct Process *p);          /* capabilities after execve() */
bool sandbox_exec_ignores_setid(const struct Process *p);
void sandbox_teardown(struct Process *p);      /* drop references */
void sandbox_on_exit(struct Process *p);       /* a pid namespace's init died: the rest go */

/* ---- chroot ---- */
int64_t sandbox_chroot(const char *kpath);
/* the inode lookups of the calling thread start at (ext2 root = 2) */
uint32_t sandbox_lookup_root(void);
/* kernel code reading its own files (the root password) from the real
   root whatever the calling process's root is */
void sandbox_real_root(bool on);

/* ---- namespaces ---- */
int64_t sandbox_unshare(uint64_t flags);
/* clone()'s CLONE_NEW* flags: checked -- and the namespaces made -- before
   the fork, given to the child after it; abort() when the fork failed */
int64_t sandbox_clone_check(uint64_t flags);
void    sandbox_clone_apply(struct Process *child, uint64_t flags);
void    sandbox_clone_abort(void);
bool    sandbox_ns_admin(const struct Process *p);   /* CAP_SYS_ADMIN where it matters */
uint32_t sandbox_netns(const struct Process *p);     /* 0: the host's */

/* user ids as the process sees them, and back (-1: not mapped) */
uint32_t uid_to_ns(const struct Process *p, uint32_t global);
uint32_t gid_to_ns(const struct Process *p, uint32_t global);
uint32_t uid_from_ns(const struct Process *p, uint32_t inner);
uint32_t gid_from_ns(const struct Process *p, uint32_t inner);
bool     sandbox_setgroups_denied(const struct Process *p);

/* pids/tids as `viewer` sees them (0: not visible to it), and back */
uint64_t pid_to_ns(uint64_t global, const struct Process *viewer);
uint64_t pid_from_ns(uint64_t local, const struct Process *viewer);
/* a new process or thread id `global` exists in p's pid namespace */
void     pidns_register(uint64_t global, struct Process *p);

/* ---- mounts (the ext2 walker's side) ----
   `ctx` is the mount a lookup is in (NULL: none). */
uint32_t mnt_enter(uint32_t ino, const void **ctx);   /* through mountpoints */
bool     mnt_leave(uint32_t ino, const void **ctx, uint32_t *parent);   /* ".." out of a mount */
bool     mnt_ro(const void *ctx);
bool     mnt_any_ro(void);            /* the caller's namespace has read-only mounts */
bool     mnt_is_point(uint32_t ino);  /* something is mounted there */
uint32_t mnt_up(uint32_t ino);        /* a mount's root -> its mountpoint (getcwd) */
int64_t  sandbox_mount(const char *src, const char *tgt, const char *type, uint64_t flags);
int64_t  sandbox_umount(const char *tgt, int flags);
int64_t  sandbox_pivot_root(const char *new_root, const char *put_old);
void     sandbox_mnt_reap(void);      /* the flusher: tmpfs directories nothing mounts */
/* /proc/self/mountinfo or /proc/self/mounts of the caller: the length */
uint32_t sandbox_mountinfo(char *out, uint32_t cap, bool mountinfo);

/* /proc/{self,PID}/{uid_map,gid_map,setgroups}: kind 1, 2, 3, and the
   process's global pid */
int  sandbox_proc_file(const char *path, uint64_t *gpid);   /* kind, or 0 */
bool sandbox_proc_dir(const char *path);   /* /proc/self, /proc/PID, their ns/ and fd/ */
int64_t sandbox_proc_read(int kind, uint64_t gpid, char *out, uint32_t cap);
int64_t sandbox_proc_write(int kind, uint64_t gpid, const char *buf, uint32_t len);
/* readlink("/proc/self/ns/NAME"): "user:[4026531837]" */
bool sandbox_ns_link(const char *name, char *out, uint32_t cap);
