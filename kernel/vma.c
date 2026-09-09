/*
 * WynlandOS - Virtual Memory Area (VMA) list implementation
 */
#include <wynland/vma.h>
#include <wynland/process.h>
#include <wynland/heap.h>

VMA *vma_insert(Process *proc, uint64_t start, uint64_t end, uint32_t prot, uint32_t flags)
{
    VMA *v = (VMA *)kmalloc(sizeof(VMA));
    if (!v) return NULL;
    v->start = start;
    v->end = end;
    v->prot = prot;
    v->flags = flags;
    v->next = proc->vma_list;
    proc->vma_list = v;
    return v;
}

VMA *vma_find(Process *proc, uint64_t addr)
{
    for (VMA *v = proc->vma_list; v; v = v->next) {
        if (addr >= v->start && addr < v->end) return v;
    }
    return NULL;
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
            kfree(v);
        } else {
            prev = v;
        }
        v = next;
    }
}
