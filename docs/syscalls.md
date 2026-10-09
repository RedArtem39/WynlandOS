# Canopy system calls

The Canopy kernel speaks the **Linux x86_64 syscall ABI**: `syscall`
instruction, the number in `rax`, arguments in `rdi rsi rdx r10 r8 r9`,
the result in `rax`, errors as `-errno`. Prebuilt glibc programs call it as
they would call Linux. On top of that there are a few calls of our own,
numbered 400 to 415 -- a range Linux leaves free on x86_64 (its own
numbers there jump from 334 to 424).

Everything is in `kernel/syscall.c` (`syscall_handler`); an unknown number
returns `-ENOSYS` and is logged to the serial port. Boot with
`strace=NAME` in `/etc/wynland/boot.cfg` to log every call of the program
`NAME`.

## Linux calls

Numbers are Linux x86_64's (`asm/unistd_64.h`). "Partial" says what is
missing; anything not listed here is `-ENOSYS`.

### Files and directories

| # | call | notes |
|---|------|-------|
| 0, 1 | `read`, `write` | files, pipes, sockets, PTYs, devices |
| 2, 257, 437 | `open`, `openat`, `openat2` | `O_CREAT` honours the umask; `openat2`'s `resolve` flags are ignored |
| 3, 436 | `close`, `close_range` | |
| 4, 5, 6, 262, 332 | `stat`, `fstat`, `lstat`, `newfstatat`, `statx` | real owner, mode and times from ext2 |
| 8 | `lseek` | `SEEK_DATA`/`SEEK_HOLE` treat a file as one data extent |
| 17, 18 | `pread64`, `pwrite64` | |
| 19, 20 | `readv`, `writev` | |
| 21, 269, 439 | `access`, `faccessat`, `faccessat2` | |
| 32, 33, 292 | `dup`, `dup2`, `dup3` | |
| 72 | `fcntl` | `F_DUPFD(_CLOEXEC)`, `F_GET/SETFD`, `F_GET/SETFL`; record locks are not tracked (`F_SETLK` succeeds, `F_GETLK` says unlocked) |
| 73 | `flock` | always succeeds (one machine) |
| 74, 75, 162, 306 | `fsync`, `fdatasync`, `sync`, `syncfs` | write back the block cache |
| 77, 285 | `ftruncate`, `fallocate` | |
| 79, 80, 81 | `getcwd`, `chdir`, `fchdir` | a working directory per process |
| 82, 264, 316 | `rename`, `renameat`, `renameat2` | |
| 83, 258 | `mkdir`, `mkdirat` | |
| 84, 87, 263 | `rmdir`, `unlink`, `unlinkat` | |
| 85 | `creat` | |
| 86, 265 | `link`, `linkat` | hard links |
| 88, 266 | `symlink`, `symlinkat` | |
| 89, 267 | `readlink`, `readlinkat` | also `/proc/self/exe`, `/proc/self/fd/N` |
| 90, 91, 268 | `chmod`, `fchmod`, `fchmodat` | owner or root |
| 92–94, 260 | `chown`, `fchown`, `lchown`, `fchownat` | root; an owner may only change the group, to one of its own |
| 95 | `umask` | |
| 132, 235, 280 | `utime`, `utimes`, `utimensat` | |
| 137, 138 | `statfs`, `fstatfs` | |
| 217 | `getdents64` | |
| 221 | `fadvise64` | accepted, no effect |
| 188–199 | `*xattr` | `-EOPNOTSUPP`: ext2 here keeps no extended attributes |

### Memory

| # | call | notes |
|---|------|-------|
| 9 | `mmap` | anonymous, file-backed (through the page cache), `MAP_FIXED`; demand paging. `MAP_SHARED` is shared for memfds, shared memory, DRM buffers and `/dev/fb0`; for an ext2 file it is not yet (each process gets its own copy of the pages) |
| 10 | `mprotect` | W^X: writable+executable needs the JIT opt-in |
| 11 | `munmap` | |
| 12 | `brk` | |
| 25 | `mremap` | |
| 28 | `madvise` | `MADV_DONTNEED` frees; the rest are advice |
| 319 | `memfd_create` | |
| 324 | `membarrier` | `-ENOSYS` (one CPU) |

### Processes and threads

| # | call | notes |
|---|------|-------|
| 39, 110, 186 | `getpid`, `getppid`, `gettid` | as the caller's pid namespace sees them (`getppid` 0 when the parent is outside it); `kill`, `wait4`, `waitid`, `tgkill` and the group calls take and give these numbers too |
| 56, 57, 58 | `clone`, `fork`, `vfork` | copy-on-write `fork`; threads via `CLONE_VM\|CLONE_THREAD` |
| 59 | `execve` | ELF (static, dynamic, PIE) and `#!` scripts; setuid/setgid bits |
| 60, 231 | `exit`, `exit_group` | |
| 61, 247 | `wait4`, `waitid` | |
| 98 | `getrusage` | CPU time |
| 109, 111, 112, 121, 124 | `setpgid`, `getpgrp`, `setsid`, `getpgid`, `getsid` | |
| 157 | `prctl` | `PR_SET/GET_NO_NEW_PRIVS`, `PR_SET/GET_SECCOMP`, `PR_SET/GET_NAME`, `PR_SET/GET_DUMPABLE`, `PR_SET/GET_PDEATHSIG` (kept, not sent), `PR_CAPBSET_READ/DROP`; others accepted |
| 158 | `arch_prctl` | `ARCH_SET_FS`/`GET_FS` |
| 202 | `futex` | wait/wake queues, bitsets, requeue |
| 218, 273, 334 | `set_tid_address`, `set_robust_list`, `rseq` | |
| 24, 204 | `sched_yield`, `sched_getparam` | |
| 435 | `clone3` | `-ENOSYS`: glibc falls back to `clone` |
| 434 | `pidfd_open` | `-ENOSYS`: callers fall back to `waitpid` |

### Users and permissions

| # | call | notes |
|---|------|-------|
| 102, 104, 107, 108 | `getuid`, `getgid`, `geteuid`, `getegid` | as the caller's user namespace maps them (65534 when not mapped) |
| 105, 106 | `setuid`, `setgid` | root sets all three ids, others the effective one |
| 113, 114 | `setreuid`, `setregid` | |
| 117–120 | `setresuid`, `setresgid`, `getresuid`, `getresgid` | |
| 115, 116 | `getgroups`, `setgroups` | `setgroups` is root's |
| 122, 123 | `setfsuid`, `setfsgid` | the effective id is the filesystem id |

### Sandboxing

`kernel/sandbox.c`; what differs from Linux is listed in
`include/wynland/sandbox.h`.

| # | call | notes |
|---|------|-------|
| 317 | `seccomp` | `SET_MODE_STRICT`, `SET_MODE_FILTER` (classic BPF, up to 4096 instructions, stacked; flags TSYNC/LOG/SPEC_ALLOW), `GET_ACTION_AVAIL`. Actions: KILL_PROCESS, KILL_THREAD (both kill the process with SIGSYS), TRAP (SIGSYS with `si_syscall`, `si_call_addr`, `si_arch`; the handler's `REG_RAX` is the result), ERRNO, LOG, ALLOW; TRACE and USER_NOTIF give `-ENOSYS`. Filters are per process (every thread), inherited by fork, spawn and execve; installing one needs no_new_privs or CAP_SYS_ADMIN |
| 125, 126 | `capget`, `capset` | root has every capability, a user none; the creator of a user namespace has all of them inside it; `capset` only drops |
| 161 | `chroot` | needs CAP_SYS_CHROOT (root, or inside a user namespace); moves the working directory to the new root; `..` stays at it, absolute symlinks start at it, directory fds from outside lead nowhere |
| 272 | `unshare` | `CLONE_NEWUSER` (anyone; one-range uid/gid maps through `/proc/self/uid_map`, `gid_map`, `setgroups`), `CLONE_NEWPID` (children only; the first is pid 1, its death kills the rest), `CLONE_NEWNET` (no IP; abstract Unix sockets of its own); UTS/IPC/CGROUP/TIME accepted, isolating nothing; `CLONE_NEWNS` `-EINVAL` |
| 56 | `clone` | the same `CLONE_NEW*` flags for the child |
| 308 | `setns` | `-EINVAL` |

`readlink("/proc/self/ns/user")` and the other namespaces give
`user:[NNN]`, different for each namespace. With no_new_privs, or inside
a user namespace, setuid and setgid bits do nothing at execve.

### Signals

| # | call | notes |
|---|------|-------|
| 13, 14, 15 | `rt_sigaction`, `rt_sigprocmask`, `rt_sigreturn` | `SA_SIGINFO`, `SA_RESTART`, nested frames |
| 62, 200, 234 | `kill`, `tkill`, `tgkill` | to a process, a group, a thread |
| 130 | `rt_sigsuspend` | |
| 131 | `sigaltstack` | accepted; handlers run on the normal stack |

### Time

| # | call | notes |
|---|------|-------|
| 35, 230 | `nanosleep`, `clock_nanosleep` | |
| 96, 201 | `gettimeofday`, `time` | (there is no vDSO) |
| 228, 229 | `clock_gettime`, `clock_getres` | `CLOCK_REALTIME` from the RTC, every other clock is the uptime; 1 ms resolution |
| 283, 286, 287 | `timerfd_create`, `timerfd_settime`, `timerfd_gettime` | |

### Waiting and events

| # | call | notes |
|---|------|-------|
| 7, 271 | `poll`, `ppoll` | |
| 23, 270 | `select`, `pselect6` | |
| 213, 291, 233, 232, 281 | `epoll_create(1)`, `epoll_ctl`, `epoll_wait`, `epoll_pwait` | up to 64 fds per instance; an epoll fd is itself pollable (a Wayland server's event loop is one); `EPOLLONESHOT`; `EPOLLET` only for `EPOLLOUT` (`EPOLLIN` stays level-triggered) |
| 22, 293 | `pipe`, `pipe2` | |
| 290 | `eventfd2` | |
| 294 | `inotify_init1` | `-ENOSYS`: GLib falls back to polling |

### Sockets

| # | call | notes |
|---|------|-------|
| 41, 53 | `socket`, `socketpair` | `AF_UNIX` (stream, datagram), `AF_INET` (TCP, UDP) |
| 42, 43, 288 | `connect`, `accept`, `accept4` | |
| 44, 45, 46, 47, 307 | `sendto`, `recvfrom`, `sendmsg`, `recvmsg`, `sendmmsg` | fd passing (`SCM_RIGHTS`) on Unix sockets |
| 48, 49, 50 | `shutdown`, `bind`, `listen` | a bound Unix path is a socket node in the filesystem |
| 51, 52 | `getsockname`, `getpeername` | |
| 54, 55 | `setsockopt`, `getsockopt` | `SO_PEERCRED`, `SO_TYPE`, `SO_ERROR`, buffer sizes |
| 40, 326 | `sendfile`, `copy_file_range` | `-ENOSYS`: callers copy by hand |

### Devices and the rest

| # | call | notes |
|---|------|-------|
| 16 | `ioctl` | terminals (termios, window size, controlling terminal, foreground group, PTY numbers), `FIONREAD`, `FIONBIO`, `FIOCLEX`, the framebuffer, DRM (virtio-gpu, virgl, KMS, PRIME), `/dev/dsp` |
| 63 | `uname` | reports `Linux` so software takes its Linux paths |
| 97, 302 | `getrlimit`, `prlimit64` | fixed limits; setting is accepted |
| 99 | `sysinfo` | |
| 318 | `getrandom` | |
| 444–446 | `landlock_*` | `-ENOSYS` (callers fall back to the sandboxing above) |

## WynlandOS calls (400 and up)

Our own. Programs written for WynlandOS may use them; nothing from Linux
does. Arguments in the same registers as the Linux calls.

| # | name | arguments → result |
|---|------|--------------------|
| 400 | `draw_rect` | `(x, y, w, h, color)` → 0. Fills a rectangle on the kernel's own framebuffer compositor (the boot console, Zerp 1). |
| 401 | `mark_dirty` | `()` → 0. Asks that compositor to redraw. |
| 404 | `kfree` | Removed: `-ENOSYS`. (It freed any kernel pointer a program passed in.) |
| 405 | `spawn` | `(path)` → pid or −1. Starts a new process running `path`, as the caller's user, with the caller's fds that are not close-on-exec. No argv. |
| 406 | `fb_flush` | `(x, y, w, h)` → 0. Pushes a rectangle of `/dev/fb0` to the screen (virtio-gpu needs an explicit flush). |
| 407 | `shm_create` | `(size)` → fd or −1. An anonymous shared-memory segment; `mmap` the fd with `MAP_SHARED` to share it between processes. |
| 408 | `spawn_argv` | `(path, argv, envp)` → pid or −1. `spawn` with an argv and an environment (`envp` NULL: the kernel's default). What init and the desktop use instead of `fork`+`exec`. |
| 409 | `wynland_elevate` | `(password)` → 0, `-ENOENT` (no root password), `-EACCES` (wrong; after a 2 s delay). On success the calling process becomes root in place. The old `ary`; the current one is the setuid `/usr/bin/ary`. |
| 410 | `pty_create` | `(int fds[2])` → 0. A pseudo-terminal pair: `fds[0]` the master, `fds[1]` the slave. (`/dev/ptmx` is the standard way.) |
| 411 | `process_alive` | `(pid)` → 1 running, 0 exited or never existed. |
| 412 | `mouse_events` | `(MouseEvent *buf, max)` → count. Raw mouse events; one reader at a time (`-EBUSY` otherwise). |
| 413 | `ary_passwd` | `(new, old)` → 0, `-EACCES`, `-EINVAL` (empty), `-EIO`. Sets the kernel's root password (`/etc/wynland/rootpw`, salted hash): root may, others need the current one. |
| 414 | `ary_status` | `()` → 1 when a root password is set, else 0. |
| 415 | `spawn_as` | `(path, argv, envp, uid, gid, groups)` → pid, `-EPERM`, −1. `spawn_argv` as another user; root only. `groups` (in `r9`) points to `{ uint32_t n; uint32_t gid[n]; }` with `n` ≤ 32, or is NULL. Used by init for `user=` services and by the login daemon for sessions. |
| 1000 | (debug) | `()` → 0. Dumps every user thread (state, the syscall it waits in) to the serial log. |

`436`, `437`, `439`, `444`–`446` in the tables above are Linux's own
numbers, not ours.

### From C

glibc's `syscall()` works:

```c
#include <unistd.h>

char *argv[] = { "/usr/bin/fish", NULL };
extern char **environ;
pid_t pid = syscall(408, argv[0], argv, environ);
```
