/*
 * WynlandOS - Virtual Memory Area (VMA) list implementation
 */
#include <wynland/vma.h>
#include <wynland/process.h>
#include <wynland/heap.h>
#include <wynland/pmm.h>
#include <wynland/vmm.h>
#include <wynland/vfs.h>

extern void *memcpy(void *dest, const void *src, size_t n);
_Static_assert(sizeof(VfsFile) <= sizeof(((VmaFile *)0)->vf), "VmaFile.vf too small for a VfsFile");

static void file_ref(VmaFile *f) { if (f) f->refs++; }
extern bool ext2_pin_inode(uint32_t inum);
extern void ext2_unpin_inode(uint32_t inum);

static void file_unref(VmaFile *f)
{
    if (f && f->refs && --f->refs == 0) {
        ext2_unpin_inode(((VfsFile *)f->vf)->node.first_cluster);
        kfree(f);
    }
}

/* NULL when the inode cannot be pinned (pin table full): the caller then
   reads the file eagerly instead */
VmaFile *vma_file_new(const void *f)
{
    if (!ext2_pin_inode(((const VfsFile *)f)->node.first_cluster)) return NULL;
    VmaFile *vf = (VmaFile *)kmalloc(sizeof(VmaFile));
    if (!vf) { ext2_unpin_inode(((const VfsFile *)f)->node.first_cluster); return NULL; }
    memcpy(vf->vf, f, sizeof(VfsFile));
    vf->refs = 0;
    return vf;
}

void vma_file_put(VmaFile *f) { file_unref(f); }

void vma_free(VMA *v)
{
    if (!v) return;
    file_unref(v->file);
    kfree(v);
}

void vma_free_list(Process *proc)
{
    proc->vma_cache = NULL;
    for (VMA *v = proc->vma_list; v; ) {
        VMA *n = v->next;
        vma_free(v);
        v = n;
    }
    proc->vma_list = NULL;
}

VMA *vma_insert(Process *proc, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags)
{
    VMA *v = (VMA *)kmalloc(sizeof(VMA));
    if (!v) return NULL;
    v->start = start;
    v->end = end;
    v->prot = prot;
    v->flags = flags;
    v->file = NULL;
    v->file_off = 0;
    v->file_len = 0;
    v->next = proc->vma_list;
    proc->vma_list = v;
    return v;
}

VMA *vma_insert_file(Process *proc, uint64_t start, uint64_t end, uint32_t prot,
                     uint32_t flags, VmaFile *file, uint64_t off, uint64_t len)
{
    VMA *v = vma_insert(proc, start, end, prot, flags);
    if (!v) return NULL;
    v->file = file;
    file_ref(file);
    v->file_off = off;
    v->file_len = len;
    return v;
}

void vma_clone_list(Process *to, Process *from)
{
    for (VMA *pv = from->vma_list; pv; pv = pv->next) {
        VMA *v = vma_insert(to, pv->start, pv->end, pv->prot, pv->flags);
        if (!v) return;
        v->file = pv->file;
        file_ref(pv->file);
        v->file_off = pv->file_off;
        v->file_len = pv->file_len;
    }
}

/* The list is unsorted and a WebKit process has thousands of entries:
   every page fault and user-pointer check walked it. Faults come in runs
   over the same mapping -- the last hit answers most of them. (Only freeing
   a node clears the cache; a split just changes the cached node's range,
   which is checked here.) */
VMA *vma_find(Process *proc, uint64_t addr)
{
    VMA *c = proc->vma_cache;
    if (c && addr >= c->start && addr < c->end) return c;
    for (VMA *v = proc->vma_list; v; v = v->next) {
        if (addr >= v->start && addr < v->end) { proc->vma_cache = v; return v; }
    }
    return NULL;
}

extern void serial_write_string(const char *str);
extern void uint_to_hex(uint64_t val, char *buf);
extern int copy_from_user(void *kdst, const void *usrc, uint64_t len);

bool vma_print_addr(Process *proc, uint64_t addr)
{
    VMA *v = proc ? vma_find(proc, addr) : NULL;
    if (!v || !(v->flags & VMA_FILE) || !v->file) return false;
    const VfsFile *vf = (const VfsFile *)(const void *)v->file->vf;
    char buf[24];
    serial_write_string(vf->node.name);
    serial_write_string("+");
    uint_to_hex(addr - v->start + v->file_off, buf);
    serial_write_string(buf);
    return true;
}

void vma_print_stack(Process *proc, uint64_t rsp)
{
    /* no frame pointers in most libraries: every stack word that points
       into a file's executable mapping is a probable return address */
    int shown = 0;
    for (int k = 0; k < 2048 && shown < 20; k++) {
        uint64_t w;
        if (copy_from_user(&w, (const void *)(rsp + 8 * (uint64_t)k), 8) != 0) break;
        VMA *v = vma_find(proc, w);
        if (!v || !(v->prot & VMA_PROT_EXEC) || !(v->flags & VMA_FILE)) continue;
        serial_write_string("  stack: ");
        vma_print_addr(proc, w);
        serial_write_string("\r\n");
        shown++;
    }
}

/* Read-ahead for file-backed faults: one disk request fills this many
   pages. Read page by page, a big library took tens of thousands of
   separate disk commands (each polled with interrupts off) where the old
   whole-file read took a few hundred; startup got slower than before. */
#define FILE_READAHEAD_PAGES 32
static uint8_t *g_ra_buf;   /* FILE_READAHEAD_PAGES pages; faults run with interrupts off */

static uint64_t pte_flags_for(const VMA *v)
{
    uint64_t f = PAGE_USER;
    if (v->prot & VMA_PROT_WRITE) f |= PAGE_WRITE;
    if (!(v->prot & VMA_PROT_EXEC)) f |= PAGE_NX;
    return f;
}

static void pcache_shrink(uint32_t want);

static void *zeroed_frame(void)
{
    void *frame = pmm_alloc_page();
    if (!frame) { pcache_shrink(1024); frame = pmm_alloc_page(); }   /* cached file pages first */
    if (!frame) return NULL;
    /* RAM is identity-mapped in the kernel's half: zero it there, before
       the process can see it */
    uint64_t *z = (uint64_t *)(uintptr_t)frame;
    for (uint32_t i = 0; i < PAGE_SIZE / 8; i++) z[i] = 0;
    return frame;
}

/* ---- the page cache ------------------------------------------------------
   One frame per (inode, page of the file), shared by every process mapping
   that file: libWPEWebKit (124 MB) was read from the disk again by every
   WebKit process, and held once per process in RAM. A cached frame is
   mapped read-only with PAGE_COW -- a write (a private .data page, an
   mprotect to writable) gets the process its own copy and never touches
   the cache. The cache holds one reference on each frame; writing to the
   file, truncating it or deleting it drops that inode's pages
   (pcache_drop_inode(), drivers/fs/ext2.c). When memory runs low, pages no
   process maps any more are given back (pcache_shrink()). */
#define PCACHE_BUCKETS 8192
#define PCACHE_INODES  1024
/* at most a quarter of RAM */
static uint32_t pcache_max_pages(void)
{
    static uint32_t max;
    if (!max) max = (uint32_t)(pmm_get_total_memory() / PAGE_SIZE / 4);
    return max;
}

/* Each entry is on two lists: its hash bucket (lookups) and its inode's
   pages (dropping a file's pages without walking the whole table -- that
   ran on every write() to any file, with interrupts off). pprev pointers
   make unlinking O(1). */
typedef struct PCEntry {
    uint32_t inum;
    uint32_t index;              /* page of the file */
    void    *frame;
    struct PCEntry *next, **pprev;            /* bucket */
    struct PCEntry *inext, **ipprev;          /* inode's pages */
} PCEntry;

typedef struct PCInode {
    uint32_t inum;
    PCEntry *pages;
    struct PCInode *next;
} PCInode;

static PCEntry *g_pcache[PCACHE_BUCKETS];
static PCInode *g_pcinodes[PCACHE_INODES];
static uint32_t g_pcache_pages;

static unsigned pcache_bucket(uint32_t inum, uint32_t index)
{
    uint64_t h = ((uint64_t)inum << 32 | index) * 0x9E3779B97F4A7C15ULL;
    return (unsigned)(h >> 51) & (PCACHE_BUCKETS - 1);
}

static PCInode **pcache_inode_slot(uint32_t inum)
{
    PCInode **pp = &g_pcinodes[(inum * 2654435761u) >> 22 & (PCACHE_INODES - 1)];
    while (*pp && (*pp)->inum != inum) pp = &(*pp)->next;
    return pp;
}

static void *pcache_get(uint32_t inum, uint32_t index)
{
    for (PCEntry *e = g_pcache[pcache_bucket(inum, index)]; e; e = e->next)
        if (e->inum == inum && e->index == index) return e->frame;
    return NULL;
}

/* off both lists, the frame's reference dropped, the entry freed */
static void pcache_remove(PCEntry *e)
{
    *e->pprev = e->next;
    if (e->next) e->next->pprev = e->pprev;
    *e->ipprev = e->inext;
    if (e->inext) e->inext->ipprev = e->ipprev;
    pmm_free_page(e->frame);     /* mappings keep theirs */
    kfree(e);
    g_pcache_pages--;
}

/* forget an inode record whose page list emptied */
static void pcache_inode_gc(uint32_t inum)
{
    PCInode **pp = pcache_inode_slot(inum);
    if (*pp && !(*pp)->pages) { PCInode *n = *pp; *pp = n->next; kfree(n); }
}

/* give back cached pages nobody maps (refcount 1: only the cache) */
static void pcache_shrink(uint32_t want)
{
    static unsigned cursor;      /* resume where the last pass stopped */
    uint32_t freed = 0;
    for (unsigned n = 0; n < PCACHE_BUCKETS && freed < want; n++) {
        unsigned b = (cursor + n) & (PCACHE_BUCKETS - 1);
        PCEntry *e = g_pcache[b];
        while (e && freed < want) {
            PCEntry *next = e->next;
            if (pmm_page_refcount(e->frame) <= 1) {
                uint32_t inum = e->inum;
                pcache_remove(e);
                pcache_inode_gc(inum);
                freed++;
            }
            e = next;
        }
        if (freed >= want) { cursor = b; break; }
    }
}

/* the cache takes a reference on `frame` */
static void pcache_put(uint32_t inum, uint32_t index, void *frame)
{
    /* full: make room -- but when nothing could be given back (every page
       in use), stop trying for a while instead of walking the whole table
       on every page fault with interrupts off (that slowed the machine down
       by half) */
    static uint32_t skip;
    if (g_pcache_pages >= pcache_max_pages()) {
        if (skip) { skip--; return; }
        uint32_t before = g_pcache_pages;
        pcache_shrink(1024);
        if (g_pcache_pages == before) { skip = 4096; return; }
    }
    if (pmm_page_refcount(frame) >= 250) return;               /* the byte-wide count */
    PCInode **slot = pcache_inode_slot(inum);
    if (!*slot) {
        PCInode *n = (PCInode *)kmalloc(sizeof(PCInode));
        if (!n) return;
        n->inum = inum;
        n->pages = NULL;
        n->next = NULL;
        *slot = n;
    }
    PCEntry *e = (PCEntry *)kmalloc(sizeof(PCEntry));
    if (!e) return;
    e->inum = inum;
    e->index = index;
    e->frame = frame;
    pmm_page_incref(frame);
    PCEntry **head = &g_pcache[pcache_bucket(inum, index)];
    e->next = *head;
    e->pprev = head;
    if (*head) (*head)->pprev = &e->next;
    *head = e;
    PCEntry **ihead = &(*slot)->pages;
    e->inext = *ihead;
    e->ipprev = ihead;
    if (*ihead) (*ihead)->ipprev = &e->inext;
    *ihead = e;
    g_pcache_pages++;
}

uint32_t pcache_pages(void) { return g_pcache_pages; }

void pcache_drop_inode(uint32_t inum)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));
    PCInode **slot = pcache_inode_slot(inum);
    if (*slot) {                 /* most writes: a file never mapped -- nothing here */
        while ((*slot)->pages) pcache_remove((*slot)->pages);
        PCInode *n = *slot;
        *slot = n->next;
        kfree(n);
    }
    if (rflags & 0x200) __asm__ volatile("sti");
}

/* a cached page into the process: shared, read-only, copy on write */
static void map_cached(Process *proc, VMA *v, uint64_t page, void *frame)
{
    uint64_t f = (pte_flags_for(v) & ~PAGE_WRITE) | PAGE_COW;
    pmm_page_incref(frame);
    vmm_map_page(proc->pml4, page, (uint64_t)(uintptr_t)frame, f);
}

/* file-backed: the faulting page and the following not-yet-present pages
   of the same VMA, read in one go (up to the mapped length; past it, and
   past the end of the file, pages stay zero) */
static bool fault_in_file(Process *proc, VMA *v, uint64_t page)
{
    /* whole pages of an ext2 file go through the page cache */
    VfsFile *cf = (VfsFile *)v->file->vf;
    uint32_t inum = cf->node.first_cluster;
    uint64_t in_vma0 = page - v->start;
    bool cacheable = inum != 0 && inum < 0xFFFFFF00u && !(v->file_off & (PAGE_SIZE - 1)) &&
                     in_vma0 + PAGE_SIZE <= v->file_len;
    if (cacheable) {
        void *hit = pcache_get(inum, (uint32_t)((v->file_off + in_vma0) / PAGE_SIZE));
        if (hit) {
            map_cached(proc, v, page, hit);
            /* fault-around: the cached pages after it too, as the disk
               path maps a whole read-ahead -- one page per fault made a
               warm cache slower than the disk */
            for (uint32_t i = 1; i < FILE_READAHEAD_PAGES; i++) {
                uint64_t a = page + (uint64_t)i * PAGE_SIZE;
                uint64_t in_a = a - v->start;
                if (a >= v->end || in_a + PAGE_SIZE > v->file_len ||
                    (vmm_get_page_flags(proc->pml4, a) & PAGE_PRESENT)) break;
                void *next = pcache_get(inum, (uint32_t)((v->file_off + in_a) / PAGE_SIZE));
                if (!next) break;
                map_cached(proc, v, a, next);
            }
            return true;
        }
    }

    if (!g_ra_buf) g_ra_buf = (uint8_t *)kmalloc(FILE_READAHEAD_PAGES * PAGE_SIZE);

    uint32_t npages = 1;
    if (g_ra_buf) {
        while (npages < FILE_READAHEAD_PAGES) {
            uint64_t a = page + (uint64_t)npages * PAGE_SIZE;
            if (a >= v->end || (vmm_get_page_flags(proc->pml4, a) & PAGE_PRESENT)) break;
            npages++;
        }
    }

    uint64_t in_vma = page - v->start;
    uint64_t want = 0;
    if (in_vma < v->file_len) {
        want = v->file_len - in_vma;
        if (want > (uint64_t)npages * PAGE_SIZE) want = (uint64_t)npages * PAGE_SIZE;
    }

    uint8_t *buf = g_ra_buf;
    uint8_t *single = NULL;
    if (!buf) {                       /* no read-ahead buffer: just this page */
        single = (uint8_t *)zeroed_frame();
        if (!single) return false;
        buf = single;
    }

    int32_t got = 0;
    if (want) {
        VfsFile *f = (VfsFile *)v->file->vf;
        if (vfs_seek(f, (int32_t)(v->file_off + in_vma), 0) >= 0)
            got = vfs_read(f, buf, (uint32_t)want);
        if (got < 0) got = 0;
    }

    if (single) {
        for (uint64_t i = (uint64_t)got; i < PAGE_SIZE; i++) single[i] = 0;
        vmm_map_page(proc->pml4, page, (uint64_t)(uintptr_t)single, pte_flags_for(v));
        return true;
    }

    for (uint32_t i = 0; i < npages; i++) {
        uint64_t off = (uint64_t)i * PAGE_SIZE;
        uint64_t in_vma_i = in_vma + off;
        bool cache_it = cacheable && in_vma_i + PAGE_SIZE <= v->file_len && off + PAGE_SIZE <= (uint64_t)got;
        uint32_t index = (uint32_t)((v->file_off + in_vma_i) / PAGE_SIZE);
        if (cache_it && i > 0) {
            void *hit = pcache_get(inum, index);   /* read-ahead over a cached page */
            if (hit) { map_cached(proc, v, page + off, hit); continue; }
        }
        uint8_t *frame = (uint8_t *)pmm_alloc_page();
        if (!frame && cache_it) { pcache_shrink(256); frame = (uint8_t *)pmm_alloc_page(); }
        if (!frame) {
            if (i == 0) return false;     /* out of memory: the access faults for real */
            break;                        /* read-ahead is only a bonus */
        }
        for (uint64_t k = 0; k < PAGE_SIZE; k++)
            frame[k] = off + k < (uint64_t)got ? buf[off + k] : 0;
        if (cache_it) {
            pcache_put(inum, index, frame);
            if (pmm_page_refcount(frame) > 1) {      /* cached: share it */
                vmm_map_page(proc->pml4, page + off, (uint64_t)(uintptr_t)frame,
                             (pte_flags_for(v) & ~PAGE_WRITE) | PAGE_COW);
                continue;
            }
        }
        vmm_map_page(proc->pml4, page + off, (uint64_t)(uintptr_t)frame, pte_flags_for(v));
    }
    return true;
}

bool vma_fault_in(Process *proc, uint64_t addr, uint32_t access)
{
    if (!proc || !proc->pml4) return false;
    VMA *v = vma_find(proc, addr);
    if (!v || !(v->flags & VMA_LAZY) || (v->flags & VMA_GUARD)) return false;
    if ((v->prot & access) != access) return false;   /* PROT_NONE, or no write/exec */

    uint64_t page = addr & ~(uint64_t)(PAGE_SIZE - 1);
    if (vmm_get_page_flags(proc->pml4, page) & PAGE_PRESENT) return false;   /* not ours to fix */

    if ((v->flags & VMA_FILE) && v->file) return fault_in_file(proc, v, page);

    void *frame = zeroed_frame();
    if (!frame) return false;   /* out of memory: the access faults for real */
    vmm_map_page(proc->pml4, page, (uint64_t)(uintptr_t)frame, pte_flags_for(v));
    return true;
}

/* Ensure a VMA boundary exists exactly at `addr`, splitting whichever VMA
   currently straddles it (if any) into two nodes with identical prot/flags.
   No-op if addr already falls on a boundary or inside no VMA. On OOM,
   leaves the list unsplit -- callers only use this to make a subsequent
   full-vs-partial-coverage check exact, they degrade to "partial coverage"
   rather than corrupting anything if it can't allocate. */
static void vma_split_at(Process *proc, uint64_t addr)
{
    for (VMA *v = proc->vma_list; v; v = v->next) {
        if (addr > v->start && addr < v->end) {
            VMA *tail = (VMA *)kmalloc(sizeof(VMA));
            if (!tail) return;
            tail->start = addr;
            tail->end = v->end;
            tail->prot = v->prot;
            tail->flags = v->flags;
            /* the file view continues where the head stops */
            uint64_t cut = addr - v->start;
            tail->file = v->file;
            file_ref(v->file);
            tail->file_off = v->file_off + cut;
            tail->file_len = v->file_len > cut ? v->file_len - cut : 0;
            if (v->file_len > cut) v->file_len = cut;
            tail->next = proc->vma_list;
            proc->vma_list = tail;
            v->end = addr;
            return;
        }
    }
}

/* Can b be appended to a (a->end == b->start)? Same prot and flags, and a
   file view that continues: b starts where a's file bytes end, a fully
   backed. */
static bool vma_mergeable(const VMA *a, const VMA *b)
{
    if (a->end != b->start || a->prot != b->prot || a->flags != b->flags || a->file != b->file) return false;
    if (!a->file) return true;
    return a->file_len == a->end - a->start && b->file_off == a->file_off + a->file_len;
}

/* Glue v to its neighbours where they match: mprotect() splits mappings
   and nothing joined them again -- JavaScriptCore and WebKit's allocator
   flip permissions all the time, and a process ended up with thousands of
   VMAs. */
static void vma_merge_around(Process *proc, VMA *v)
{
    for (int pass = 0; pass < 2; pass++) {
        VMA *prev = NULL;
        for (VMA *n = proc->vma_list; n; prev = n, n = n->next) {
            if (n == v) continue;
            VMA *keep, *gone;
            if (vma_mergeable(v, n)) { keep = v; gone = n; }
            else if (vma_mergeable(n, v)) { keep = n; gone = v; }
            else continue;
            keep->end = gone->end;
            if (keep->file) keep->file_len += gone->file_len;
            /* unlink `gone` */
            VMA **pp = &proc->vma_list;
            while (*pp && *pp != gone) pp = &(*pp)->next;
            if (*pp) *pp = gone->next;
            if (proc->vma_cache == gone) proc->vma_cache = NULL;
            vma_free(gone);
            v = keep;
            (void)prev;
            break;
        }
    }
}

bool vma_protect_range(Process *proc, uint64_t start, uint64_t end, uint32_t new_prot)
{
    vma_split_at(proc, start);
    vma_split_at(proc, end);

    uint64_t covered = 0;
    VMA *changed[4];
    int nchanged = 0;
    for (VMA *v = proc->vma_list; v; v = v->next) {
        if (v->start >= start && v->end <= end && v->start < v->end) {
            v->prot = new_prot;
            covered += (v->end - v->start);
            if (nchanged < 4) changed[nchanged++] = v;
        }
    }
    /* merge only while the changed nodes are few (the usual case); a node
       merged away is never used again from `changed` */
    if (nchanged == 1) vma_merge_around(proc, changed[0]);
    return covered == (end - start);
}

void vma_unmap_range(Process *proc, uint64_t start, uint64_t end)
{
    vma_split_at(proc, start);
    vma_split_at(proc, end);

    VMA *prev = NULL;
    VMA *v = proc->vma_list;
    while (v) {
        VMA *next = v->next;
        if (v->start >= start && v->end <= end && v->start < v->end) {
            if (prev) prev->next = next; else proc->vma_list = next;
            if (proc->vma_cache == v) proc->vma_cache = NULL;
            vma_free(v);
        } else {
            prev = v;
        }
        v = next;
    }
}
