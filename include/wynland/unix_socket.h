/*
 * WynlandOS - AF_UNIX sockets, memfd and timerfd objects.
 *
 * All entry points take kernel-side data (user memory is validated and
 * copied by the syscall glue in kernel/syscall.c) and return >= 0 on
 * success or a negative errno. Like everything else reached from a
 * syscall they run with interrupts off; blocking waits go through
 * waitqueue_wait_ms(), never a spin.
 */
#pragma once

#include <wynland/types.h>
#include <wynland/vfs.h>
#include <wynland/waitqueue.h>

#define USOCK_STREAM     1
#define USOCK_DGRAM      2
#define USOCK_SEQPACKET  5

#define USOCK_MAX_MSG_FDS 64   /* SCM_RIGHTS fds per message */

/* poll bits (same values as Linux) */
#define UPOLLIN   0x0001
#define UPOLLOUT  0x0004
#define UPOLLERR  0x0008
#define UPOLLHUP  0x0010
#define UPOLLRDHUP 0x2000

/* msg flags (Linux values) */
#define UMSG_PEEK      0x02
#define UMSG_CTRUNC    0x08
#define UMSG_TRUNC     0x20
#define UMSG_DONTWAIT  0x40
#define UMSG_WAITALL   0x100
#define UMSG_NOSIGNAL  0x4000
#define UMSG_CMSG_CLOEXEC 0x40000000

typedef struct {
    const uint8_t *base;   /* user pointer, already validated for reading */
    uint64_t len;
} UIoVecR;

typedef struct {
    uint8_t *base;         /* user pointer, already prepared for writing */
    uint64_t len;
} UIoVecW;

struct UCred { int32_t pid; uint32_t uid; uint32_t gid; };

/* Woken on every pipe/socket/timerfd state change: a poll() over several
   fds sleeps here (a thread can only park on one queue). Spurious wakes
   are fine -- do_poll() re-checks. */
extern WaitQueue g_poll_any_wq;

/* ---- sockets ---- */
int     usock_create(int type);                     /* new unconnected socket slot, refs=1 */
int     usock_pair(int type, int out[2]);           /* two connected slots, refs=1 each */
void    usock_ref(int idx);
void    usock_unref(int idx);
int     usock_type(int idx);
int64_t usock_bind(int idx, const char *name, uint32_t name_len);   /* name as in sun_path; abstract if name[0]==0 */
int64_t usock_listen(int idx, int backlog);
int64_t usock_connect(int idx, const char *name, uint32_t name_len, bool nonblock);
int64_t usock_accept(int idx, bool nonblock);       /* returns new slot (refs=1) */
int64_t usock_send(int idx, const UIoVecR *iov, int iovcnt, VfsFile **fds, uint32_t nfds,
                   int flags, bool nonblock, const char *dest, uint32_t dest_len);
/* fds_out receives up to fds_cap in-flight wrappers the caller must install
   or kfile_close(); *msg_flags gets MSG_TRUNC/MSG_CTRUNC. */
int64_t usock_recv(int idx, const UIoVecW *iov, int iovcnt, int flags, bool nonblock,
                   VfsFile **fds_out, uint32_t fds_cap, uint32_t *nfds_out, int *msg_flags,
                   char *src_name, uint32_t *src_len);
int64_t usock_shutdown(int idx, int how);
int64_t usock_getname(int idx, bool peer, char *name, uint32_t *name_len); /* name buffer >= 108 */
int64_t usock_peercred(int idx, struct UCred *out);
int64_t usock_sock_error(int idx);
uint32_t usock_poll(int idx, uint32_t events, WaitQueue **wq);

/* ---- memfd ---- */
int      memfd_new(const char *name);               /* refs=1 */
void     memfd_ref(int idx);
void     memfd_unref(int idx);
int64_t  memfd_truncate(int idx, uint64_t size);
uint64_t memfd_size(int idx);
int64_t  memfd_pread(int idx, uint64_t off, void *buf, uint64_t len);    /* buf: kernel or prepared user */
int64_t  memfd_pwrite(int idx, uint64_t off, const void *buf, uint64_t len);
uint64_t memfd_page_phys(int idx, uint64_t page_index);  /* 0 past EOF */

/* ---- timerfd ---- */
int      timerfd_new(int clockid);                  /* refs=1 */
void     timerfd_ref(int idx);
void     timerfd_unref(int idx);
int      timerfd_clock(int idx);                    /* clockid given at create */
/* times in ms on the 1 kHz clock; value_ms 0 disarms; abs = TFD_TIMER_ABSTIME
   (only meaningful for CLOCK_MONOTONIC here) */
int64_t  timerfd_settime(int idx, uint64_t value_ms, uint64_t interval_ms, bool abs,
                         uint64_t *old_value_ms, uint64_t *old_interval_ms);
void     timerfd_gettime(int idx, uint64_t *value_ms, uint64_t *interval_ms);
int64_t  timerfd_read(int idx, uint64_t *count, bool nonblock);
uint32_t timerfd_poll(int idx, WaitQueue **wq, uint64_t *wake_ms);
