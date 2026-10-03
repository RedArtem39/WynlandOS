/*
 * WynlandOS - pseudo-terminals (kernel/tty.c)
 * ============================================================
 * A PTY pair the Linux way: /dev/ptmx gives a master, /dev/pts/N the
 * slave, and between them a line discipline -- canonical line editing
 * with echo (erase, kill, EOF), ^C/^\ as SIGINT/SIGQUIT to the
 * foreground process group, CR/NL translation, and a window size whose
 * change is SIGWINCH. A session leader gets its controlling terminal on
 * TIOCSCTTY (or by opening a slave); shells move jobs to the foreground
 * with TIOCSPGRP.
 *
 * fds carry PTY_FD_MASTER / PTY_FD_SLAVE in node.first_cluster and the
 * PTY's index in current_cluster. Each open fd holds a reference on its
 * side: the last slave closing makes the master read -EIO (how a terminal
 * learns its shell is gone), the last master closing hangs the slave up
 * (SIGHUP to the session, reads see EOF).
 */
#pragma once

#include <wynland/types.h>

#define PTY_FD_MASTER 0xFFFFFFE6
#define PTY_FD_SLAVE  0xFFFFFFE5
#define MAX_PTYS      32

struct Process;
struct WaitQueue;

/* a new PTY with no references yet: its index, or -errno */
int  pty_alloc(void);
bool pty_exists(int idx);
/* one more fd on a side / one fd of it closed */
void pty_ref(int idx, bool master);
void pty_unref(int idx, bool master);

int64_t pty_read(int idx, bool master, void *ubuf, uint32_t n, bool nonblock);
int64_t pty_write(int idx, bool master, const void *ubuf, uint32_t n, bool nonblock);
/* poll(): the POLLIN/POLLOUT/POLLHUP bits that hold for `events`; *wq is
   where to sleep when none does */
uint32_t pty_poll(int idx, bool master, uint32_t events, struct WaitQueue **wq);
/* ioctl() on either side (TIOCGPTPEER excepted: it makes an fd, the
   caller does that); -ENOTTY for what a terminal does not do */
int64_t pty_ioctl(int idx, bool master, uint64_t req, uint64_t argp);
/* a slave was opened by p: a session leader without a terminal gets
   this one (unless O_NOCTTY) */
void pty_open_slave(int idx, struct Process *p, bool noctty);
/* the controlling terminal of p (its index), or -1 */
int  pty_ctty(struct Process *p);
/* setsid(): p leaves its terminal */
void pty_detach(struct Process *p);
