/*
 * WynlandOS - reference-counted kernel file objects.
 *
 * Every fd is its own small VfsFile wrapper (deep-copied on dup/fork/spawn,
 * see kernel/process.c's fd-inheritance comment); the wrapper indexes a
 * kernel object via node.first_cluster (type sentinel) + current_cluster
 * (slot). For the object types below the object counts how many wrappers
 * reference it, so it can react to the LAST close -- a Unix socket's peer
 * sees EOF/EPIPE, a memfd's pages are released.
 *
 * Rules: whoever creates a wrapper that copies another (dup, fork, spawn,
 * SCM_RIGHTS) calls kfile_get() on the copy (kfile_dup() does both);
 * whoever drops a wrapper calls kfile_close(), which releases the object
 * reference and frees the wrapper. Types without a refcount are no-ops in
 * kfile_get() and keep their historical cleanup in kfile_close().
 */
#pragma once

#include <wynland/types.h>
#include <wynland/vfs.h>

/* node.first_cluster sentinels (see vfs.h / syscall.c for the others) */
#define USOCK_FD    0xFFFFFFE0  /* AF_UNIX socket; current_cluster = socket slot */
#define MEMFD_FD    0xFFFFFFE1  /* memfd_create(); current_cluster = memfd slot */
#define TIMERFD_FD  0xFFFFFFE2  /* timerfd_create(); current_cluster = timer slot */

void     kfile_get(VfsFile *f);
VfsFile *kfile_dup(const VfsFile *f);   /* kmalloc'd copy + kfile_get(); NULL on OOM */
void     kfile_close(VfsFile *f);       /* kernel/syscall.c: per-type release + free */
