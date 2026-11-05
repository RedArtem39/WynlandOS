/*
 * WynlandOS - the vDSO in every process (vdso/vdso.c: clock_gettime(),
 * gettimeofday(), time() without a system call)
 *
 * Two kinds of page, shared by every process (PAGE_SHARED_MAP: never
 * freed with a process, fork() shares them as they are):
 *   VDSO_VVAR_ADDR         the data page: the clock's numbers (read-only)
 *   VDSO_VVAR_ADDR + 4 KB  the vDSO's code (build/vdso.so, linked in as
 *                          g_vdso_image: tools in the Makefile)
 * elf.c maps them into each new image and tells glibc where they are
 * (AT_SYSINFO_EHDR).
 *
 * The numbers do not change after boot (the TSC's rate, each core's
 * offset, the RTC's reading are taken once): filled the first time a
 * process gets the pages, which is after all of that.
 */
#include <wynland/types.h>
#include <wynland/pmm.h>
#include <wynland/vmm.h>
#include <wynland/vma.h>
#include <wynland/process.h>
#include <wynland/memlayout.h>
#include <wynland/smp.h>
#include <wynland/vdso.h>

extern const unsigned char g_vdso_image[];
extern const unsigned long g_vdso_image_len;
extern void serial_write_string(const char *s);
extern void sched_clock_params(uint64_t *tsc0, uint64_t *ms0, uint64_t *per);
extern void rtc_vdso_params(uint64_t *boot_unix, uint64_t *base_ns);
extern int64_t g_tsc_off[MAX_CPUS];
extern bool g_tsc_aux;

_Static_assert(VDSO_MAX_CPUS == MAX_CPUS, "the vDSO's table of TSC offsets is MAX_CPUS long");
_Static_assert(sizeof(VdsoData) <= 4096, "the vDSO's data fits its page");

#define VDSO_TEXT_PAGES 2u                     /* room for it (build/vdso.so is ~2 KB) */

static uint64_t g_vvar_phys;
static uint64_t g_text_phys[VDSO_TEXT_PAGES];
static uint32_t g_text_pages;
static volatile int g_vdso_state;              /* 0 not yet, 1 being made, 2 ready, 3 none */

static void vdso_make(void)
{
    int want = 0;
    if (!__atomic_compare_exchange_n(&g_vdso_state, &want, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        while (g_vdso_state == 1) __builtin_ia32_pause();
        return;
    }
    uint32_t pages = (uint32_t)((g_vdso_image_len + 4095) / 4096);
    void *v = pmm_alloc_page();
    if (!v || !pages || pages > VDSO_TEXT_PAGES) {
        serial_write_string("vDSO: none (no memory, or the image is too big): the clock by system calls\r\n");
        __atomic_store_n(&g_vdso_state, 3, __ATOMIC_RELEASE);
        return;
    }
    g_vvar_phys = (uint64_t)(uintptr_t)v;
    VdsoData *d = (VdsoData *)P2V(g_vvar_phys);
    uint8_t *raw = (uint8_t *)d;
    for (uint32_t i = 0; i < 4096; i++) raw[i] = 0;
    sched_clock_params(&d->tsc0, &d->ms0, &d->per);
    rtc_vdso_params(&d->boot_unix, &d->base_ns);
    d->use_aux = g_tsc_aux ? 1u : 0u;
    for (uint32_t c = 0; c < MAX_CPUS; c++) d->off[c] = g_tsc_off[c];
    d->ok = d->per ? 1u : 0u;                  /* no TSC rate yet: the kernel's ms count -- its own */

    for (uint32_t p = 0; p < pages; p++) {
        void *t = pmm_alloc_page();
        if (!t) {
            serial_write_string("vDSO: none (no memory): the clock by system calls\r\n");
            __atomic_store_n(&g_vdso_state, 3, __ATOMIC_RELEASE);
            return;
        }
        g_text_phys[p] = (uint64_t)(uintptr_t)t;
        uint8_t *dst = (uint8_t *)P2V(g_text_phys[p]);
        for (uint32_t i = 0; i < 4096; i++) {
            uint64_t o = (uint64_t)p * 4096 + i;
            dst[i] = o < g_vdso_image_len ? g_vdso_image[o] : 0;
        }
    }
    g_text_pages = pages;
    serial_write_string(d->ok ? "vDSO: clock_gettime/gettimeofday/time in user space\r\n"
                              : "vDSO: mapped, but no TSC clock: its calls go to the kernel\r\n");
    __atomic_store_n(&g_vdso_state, 2, __ATOMIC_RELEASE);
}

/* Into a new image's address space: the address for AT_SYSINFO_EHDR (the
   code), 0 when there is no vDSO. proc may be NULL (no VMA bookkeeping). */
uint64_t vdso_map(PageTable *pml4, struct Process *proc)
{
    if (g_vdso_state != 2) vdso_make();
    if (g_vdso_state != 2) return 0;
    const uint64_t text = VDSO_VVAR_ADDR + 4096;
    vmm_map_page(pml4, VDSO_VVAR_ADDR, g_vvar_phys, PAGE_USER | PAGE_NX | PAGE_SHARED_MAP);
    for (uint32_t p = 0; p < g_text_pages; p++)
        vmm_map_page(pml4, text + (uint64_t)p * 4096, g_text_phys[p], PAGE_USER | PAGE_SHARED_MAP);
    if (proc) {
        vma_insert(proc, VDSO_VVAR_ADDR, text, VMA_PROT_READ, 0);
        vma_insert(proc, text, text + (uint64_t)g_text_pages * 4096, VMA_PROT_READ | VMA_PROT_EXEC, 0);
    }
    return text;
}
