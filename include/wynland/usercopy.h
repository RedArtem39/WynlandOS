/*
 * WynlandOS - Safe user-memory access helpers
 * ============================================================
 * The syscall dispatcher runs with the calling process's own page
 * tables still loaded (CR3 unchanged across a syscall), so every
 * `(char *)a2`-style cast in kernel/syscall.c used to dereference a
 * user-supplied address directly at CPL0 with no validation: an
 * unmapped address panics the kernel, and a mapped *kernel* address
 * (identity-mapped RAM, a page table page, etc.) lets a process read
 * or corrupt kernel memory outright.
 *
 * These helpers are the single choke point that closes that hole.
 * They validate a user range page-by-page (rejecting unmapped pages,
 * kernel/shared mappings, overflowing address+length, non-canonical
 * addresses, and writes to read-only pages) before ever touching it,
 * and return -EFAULT instead of letting a bad pointer fault the
 * kernel.
 */
#pragma once

#include <wynland/types.h>
#include <wynland/vmm.h>

#define UACCESS_READ  0x1
#define UACCESS_WRITE 0x2

/* True if every page in [addr, addr+len) is present, PAGE_USER, and
   (for UACCESS_WRITE) either PAGE_WRITE or PAGE_COW -- a COW page is
   real user-writable memory, just not yet duplicated off its sibling.
   Rejects addr+len integer overflow and non-canonical/kernel-half
   addresses. A zero-length range is trivially valid without touching
   any page. Never faults: only ever reads page-table entries that are
   already resolved. */
bool user_range_valid(PageTable *pml4, uint64_t addr, uint64_t len, uint32_t required_access);

/* Copies `len` bytes from the current process's user address `usrc`
   into the kernel buffer `kdst`. Returns 0 on success, -EFAULT if any
   page in the range fails validation (nothing is copied in that
   case). */
int copy_from_user(void *kdst, const void *usrc, uint64_t len);

/* Copies `len` bytes from the kernel buffer `ksrc` to the current
   process's user address `udst`. Any COW page the range touches is
   resolved (duplicated, or reclaimed outright if this process is the
   last owner -- same logic as the page-fault handler's COW path, see
   vmm_resolve_cow_page()) before the write, so the copy can never
   land on a frame still shared with another process. Returns 0 on
   success, -EFAULT if any page in the range fails validation (nothing
   is copied in that case). */
int copy_to_user(void *udst, const void *ksrc, uint64_t len);

/* Convenience wrapper: is [addr, addr+len) fully valid for the CURRENT
   process to read? (vmm_get_current_pml4() + UACCESS_READ.) */
bool user_check_read(uint64_t addr, uint64_t len);

/* Convenience wrapper for handing a raw user pointer to an internal
   subsystem (vfs_read(), pipe_read(), tcp_recv(), vfs_getdents(), ...)
   that writes into it directly instead of going through copy_to_user().
   Validates [addr, addr+len) for the current process (UACCESS_WRITE) and
   resolves every COW page in it up front -- same effect as copy_to_user()
   minus the actual memcpy, for call sites where the destination is handed
   to code the kernel doesn't control the write pattern of. Returns false
   (leaving the range untouched by definition) if any page fails
   validation or a COW duplication is OOM. */
bool user_prepare_write(uint64_t addr, uint64_t len);

/* Copies at most `maxlen`-1 bytes from a NUL-terminated user string at
   `usrc` into `dst`, always leaving `dst` NUL-terminated somewhere
   within the first `maxlen` bytes. Validates one page at a time as it
   advances (a string shorter than `maxlen` never touches pages past
   its own NUL). Returns the string length excluding the NUL on
   success, -EFAULT if it reaches an invalid page before finding one,
   or -ENAMETOOLONG if no NUL appears within maxlen-1 bytes. */
int64_t strncpy_from_user(char *dst, const void *usrc, uint64_t maxlen);
