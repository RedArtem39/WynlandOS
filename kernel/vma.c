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

VMA *vma_find(Process *proc, uint64_t addr)
{
    for (VMA *v = proc->vma_list; v; v = v->next) {
        if (addr >= v->start && addr < v->end) return v;
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

static void *zeroed_frame(void)
{
    void *frame = pmm_alloc_page();
    if (!frame) return NULL;
    /* RAM is identity-mapped in the kernel's half: zero it there, before
       the process can see it */
    uint64_t *z = (uint64_t *)(uintptr_t)frame;
    for (uint32_t i = 0; i < PAGE_SIZE / 8; i++) z[i] = 0;
    return frame;
}

/* file-backed: the faulting page and the following not-yet-present pages
   of the same VMA, read in one go (up to the mapped length; past it, and
   past the end of the file, pages stay zero) */
static bool fault_in_file(Process *proc, VMA *v, uint64_t page)
{
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
        uint8_t *frame = (uint8_t *)pmm_alloc_page();
        if (!frame) {
            if (i == 0) return false;     /* out of memory: the access faults for real */
            break;                        /* read-ahead is only a bonus */
        }
        uint64_t off = (uint64_t)i * PAGE_SIZE;
        for (uint64_t k = 0; k < PAGE_SIZE; k++)
            frame[k] = off + k < (uint64_t)got ? buf[off + k] : 0;
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

bool vma_protect_range(Process *proc, uint64_t start, uint64_t end, uint32_t new_prot)
{
    vma_split_at(proc, start);
    vma_split_at(proc, end);

    uint64_t covered = 0;
    for (VMA *v = proc->vma_list; v; v = v->next) {
        if (v->start >= start && v->end <= end && v->start < v->end) {
            v->prot = new_prot;
            covered += (v->end - v->start);
        }
    }
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
            vma_free(v);
        } else {
            prev = v;
        }
        v = next;
    }
}
