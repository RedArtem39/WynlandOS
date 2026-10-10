/*
 * WynlandOS - Virtual Memory Area (VMA) list
 * ============================================================
 * Phase 22c: per-process bookkeeping of which address ranges are
 * mapped, with what permissions, and why -- replaces the old
 * "map now, forget the range existed" model that made mprotect()/
 * munmap() no-ops. One list per Process (Process.vma_list).
 */
#pragma once

#include <wynland/types.h>

#define VMA_PROT_READ  0x1
#define VMA_PROT_WRITE 0x2
#define VMA_PROT_EXEC  0x4

#define VMA_ANON  0x1  /* anonymous (no backing file) */
#define VMA_GUARD 0x2  /* deliberately unmapped hole (e.g. below a stack) --
                           touching it is always fatal, never lazily faulted in */
#define VMA_LAZY  0x4  /* demand-zero: a page gets a frame on first touch */
#define VMA_FILE  0x8  /* with VMA_LAZY: the page is read from `file` on first touch */
#define VMA_SHARED 0x10 /* with VMA_FILE: MAP_SHARED -- every shared mapping of the
                           file's page is one frame, written back to the file */
#define VMA_MAYWRITE 0x20 /* with VMA_SHARED: the file was open for writing, so
                             mprotect() may make the mapping writable */

/* A file behind lazy mappings: a private copy of the open file (the fd
   may be closed right after mmap()), shared by the VMAs split from one
   mapping and by fork() copies. */
typedef struct VmaFile {
    uint8_t  vf[1024];       /* a VfsFile, by value (vma.c checks the size) */
    uint32_t refs;
} VmaFile;

struct Process; /* include/wynland/process.h -- forward-declared to avoid a
                    header cycle (Process embeds a VMA* list head) */

typedef struct VMA {
    uint64_t start;  /* page-aligned, inclusive */
    uint64_t end;    /* page-aligned, exclusive */
    uint32_t prot;   /* VMA_PROT_* */
    uint32_t flags;  /* VMA_ANON / VMA_GUARD / VMA_LAZY / VMA_FILE */
    VmaFile *file;       /* VMA_FILE: where the pages come from */
    uint64_t file_off;   /* file offset of `start` */
    uint64_t file_len;   /* bytes from `start` backed by the file; the rest reads as zero */
    struct VMA *next;
} VMA;

/* Register a new region [start,end). Caller is responsible for the actual
   page mapping (or deliberately leaving it unmapped, for VMA_GUARD) --
   this only tracks the range for future mprotect/munmap/fault lookups.
   Does not check for overlap with existing VMAs; callers that need
   MAP_FIXED-replace semantics should vma_unmap_range() first. */
VMA *vma_insert(struct Process *proc, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags);

/* A file-backed lazy region: [start,end) shows `len` bytes of `file`
   from `off`. Takes its own reference on file. */
VMA *vma_insert_file(struct Process *proc, uint64_t start, uint64_t end, uint32_t prot,
                     uint32_t flags, VmaFile *file, uint64_t off, uint64_t len);

/* A VmaFile holding a private copy of an open file (refs = 0 until a VMA
   takes it). NULL on OOM. */
VmaFile *vma_file_new(const void *vfsfile);   /* a VfsFile * */

/* Copy every VMA of `from` into `to` (fork). */
void vma_clone_list(struct Process *to, struct Process *from);

/* Free one VMA (dropping its file reference) / a whole list. */
void vma_free(VMA *v);
void vma_free_list(struct Process *proc);

/* MAP_SHARED file pages (kernel/vma.c): written back to their files --
   the ones nothing maps any more (then dropped) by the flusher thread,
   every one in [start,end) of proc by msync(). */
void shmap_flush_unmapped(void);
void shmap_sync_range(struct Process *proc, uint64_t start, uint64_t end);

/* The page cache behind file mappings (kernel/vma.c): a file's contents
   changed -- forget its cached pages. */
void pcache_drop_inode(uint32_t inum);
uint32_t pcache_pages(void);   /* pages it holds now */

/* The VMA covering address `addr`, or NULL if none does. */
VMA *vma_find(struct Process *proc, uint64_t addr);

/* Crash reports: print "libfoo.so+0x1234" (a file offset, what objdump
   and addr2line take) for an address in a file mapping (false: it is
   not in one), and the probable return addresses found on a user stack
   (no frame pointers needed). The process must be the current one. */
bool vma_print_addr(struct Process *proc, uint64_t addr);
void vma_print_stack(struct Process *proc, uint64_t rsp);

/* Demand paging: give the not-yet-present page holding `addr` a zeroed
   frame if it lies in a VMA_LAZY region that allows `access` (VMA_PROT_*).
   True when the page is now mapped -- the faulting access can be retried.
   Called by the page fault handler and by the user-copy checks. */
bool vma_fault_in(struct Process *proc, uint64_t addr, uint32_t access);

/* Update the prot field of every VMA overlapping [start,end), splitting
   VMAs at the boundary where the requested range only partially covers
   one. Returns false if any part of [start,end) isn't covered by an
   existing VMA (real mprotect() semantics: that's ENOMEM), but still
   applies the change to whatever portions ARE covered. Does not touch
   page tables -- caller applies the actual PTE flag change. */
bool vma_protect_range(struct Process *proc, uint64_t start, uint64_t end, uint32_t new_prot);

/* Remove VMA coverage of [start,end), splitting/trimming/deleting nodes as
   needed. Used by real munmap() and by mmap(MAP_FIXED)'s "replace whatever
   was there" semantics. Does not touch page tables or physical memory --
   caller unmaps/frees the actual pages. */
void vma_unmap_range(struct Process *proc, uint64_t start, uint64_t end);
