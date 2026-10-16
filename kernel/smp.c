/*
 * WynlandOS - Symmetric multiprocessing: the other CPU cores
 * ==========================================================
 * See include/wynland/smp.h for the model (one kernel lock, user code on
 * every core). Here: the lock itself, the local APIC (each core's
 * interrupt controller: its timer, inter-processor interrupts), finding
 * the cores (ACPI MADT), starting them (INIT-SIPI-SIPI through
 * kernel/ap_trampoline.asm) and TLB shootdowns.
 */

#include <wynland/smp.h>
#include <wynland/sched.h>
#include <wynland/vmm.h>
#include <wynland/heap.h>
#include <wynland/pmm.h>
#include <wynland/boot_info.h>

extern void serial_write_string(const char *str);
extern void uint_to_str(uint64_t val, char *buf);
extern uint64_t timer_get_ms(void);

Cpu g_cpus[MAX_CPUS];
volatile uint32_t g_cpu_count = 1;
volatile int32_t g_bkl_owner = 0;        /* the boot core holds it from the start */
volatile uint32_t g_deferred_due;
static volatile uint32_t g_bkl_waiters;

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t v)
{
    __asm__ volatile("wrmsr" :: "c"(msr), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

static inline uint64_t irq_save(void)
{
    uint64_t f;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(f) :: "memory");
    return f;
}

static inline void irq_restore(uint64_t f)
{
    if (f & 0x200) __asm__ volatile("sti" ::: "memory");
}

#define MSR_APIC_BASE       0x1B
#define MSR_PAT             0x277
#define MSR_EFER            0xC0000080
#define MSR_GS_BASE         0xC0000101
#define MSR_KERNEL_GS_BASE  0xC0000102

void smp_set_gs(Cpu *c)
{
    c->self = c;
    wrmsr(MSR_GS_BASE, (uint64_t)(uintptr_t)c);
    wrmsr(MSR_KERNEL_GS_BASE, 0);      /* the user's GS base: 0 */
}

/* ---- TLB shootdown, the receiving side ---------------------------------- */

static inline void tlb_poll(Cpu *c)
{
    if (c->tlb_flush) {
        uint64_t cr3;
        __asm__ volatile("mov %%cr3, %0; mov %0, %%cr3" : "=r"(cr3) :: "memory");
        __atomic_store_n(&c->tlb_flush, 0, __ATOMIC_RELEASE);
    }
}

void smp_tlb_poll(void)
{
    tlb_poll(this_cpu());
}

/* ---- the big kernel lock -------------------------------------------------- */

static inline bool bkl_try(int32_t me)
{
    int32_t expect = -1;
    return __atomic_compare_exchange_n(&g_bkl_owner, &expect, me, false,
                                       __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}

/* ---- who holds the lock how long (the thread dump prints it) ----------
   By the syscall the holding thread is in (its last one), 512: an idle
   core or an interrupt. Plus how long cores waited for it. */
#define BKL_PROF_KEYS 513
static uint64_t g_bkl_t0;
static uint64_t g_hold_cyc[BKL_PROF_KEYS], g_hold_n[BKL_PROF_KEYS], g_hold_max[BKL_PROF_KEYS];
static volatile uint64_t g_wait_cyc;

static inline void bkl_took(void)
{
    g_bkl_t0 = __builtin_ia32_rdtsc();
}

static inline void bkl_account(void)
{
    uint64_t dt = __builtin_ia32_rdtsc() - g_bkl_t0;
    Cpu *c = this_cpu();
    Thread *t = c->current;
    uint32_t k = (!t || t == c->idle || t->last_syscall >= 512) ? 512 : (uint32_t)t->last_syscall;
    g_hold_cyc[k] += dt;
    g_hold_n[k]++;
    if (dt > g_hold_max[k]) g_hold_max[k] = dt;
}

void bkl_profile_dump(void)
{
    extern uint64_t net_tsc_ms(void);
    extern void uint_to_str(uint64_t val, char *buf);
    uint64_t per_ms = net_tsc_ms();
    if (!per_ms) return;
    uint64_t total = 0;
    for (int k = 0; k < BKL_PROF_KEYS; k++) total += g_hold_cyc[k];
    char b[24];
    serial_write_string("kernel lock since the last dump: held ");
    uint_to_str(total / per_ms, b); serial_write_string(b);
    serial_write_string(" ms, waited for ");
    uint_to_str(g_wait_cyc / per_ms, b); serial_write_string(b);
    serial_write_string(" ms (all cores); longest holders:\r\n");
    for (int shown = 0; shown < 10; shown++) {
        int best = -1;
        for (int k = 0; k < BKL_PROF_KEYS; k++)
            if (g_hold_cyc[k] && (best < 0 || g_hold_cyc[k] > g_hold_cyc[best])) best = k;
        if (best < 0) break;
        serial_write_string(best == 512 ? "  idle/interrupt" : "  syscall ");
        if (best != 512) { uint_to_str((uint64_t)best, b); serial_write_string(b); }
        serial_write_string(": ");
        uint_to_str(g_hold_cyc[best] / per_ms, b); serial_write_string(b);
        serial_write_string(" ms in ");
        uint_to_str(g_hold_n[best], b); serial_write_string(b);
        serial_write_string(", longest ");
        uint_to_str(g_hold_max[best] * 1000 / per_ms, b); serial_write_string(b);
        serial_write_string(" us\r\n");
        g_hold_cyc[best] = 0;      /* shown: out of the next round, and reset */
        g_hold_n[best] = 0;
        g_hold_max[best] = 0;
    }
    for (int k = 0; k < BKL_PROF_KEYS; k++) g_hold_cyc[k] = g_hold_n[k] = g_hold_max[k] = 0;
    g_wait_cyc = 0;
}

/* Cores asleep (hlt) until the lock is free, a bit each: whoever releases
   it wakes them with an IPI. A waiter spins only briefly -- a virtual CPU
   spinning for long steals the host CPU from the emulator thread that
   serves the devices (virtio-gpu flushes then time out). */
static volatile uint32_t g_bkl_sleepers;
static void lapic_send_ipi(uint32_t apic_id, uint32_t icr);

static void bkl_sleep_while_held(Cpu *c)
{
    uint32_t bit = 1u << c->id;
    __atomic_or_fetch(&g_bkl_sleepers, bit, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&g_bkl_owner, __ATOMIC_SEQ_CST) != -1 && !c->tlb_flush)
        __asm__ volatile("sti; hlt; cli" ::: "memory");   /* the wake IPI, a TLB IPI or the tick */
    __atomic_and_fetch(&g_bkl_sleepers, ~bit, __ATOMIC_SEQ_CST);
    tlb_poll(c);
}

/* The lock free again (a full barrier: a waiter that went to sleep after
   it looked is in g_bkl_sleepers by then), and the sleepers woken. */
static void bkl_release_raw(void)
{
    bkl_account();
    __atomic_exchange_n(&g_bkl_owner, -1, __ATOMIC_SEQ_CST);
    uint32_t s = __atomic_load_n(&g_bkl_sleepers, __ATOMIC_SEQ_CST);
    for (uint32_t i = 0; s && i < g_cpu_count; i++)
        if (s & (1u << i)) lapic_send_ipi(g_cpus[i].apic_id, VEC_IPI_WAKE);
}

void bkl_lock(void)
{
    Cpu *c = this_cpu();
    int32_t me = (int32_t)c->id;
    if (g_bkl_owner == me) return;
    __atomic_add_fetch(&g_bkl_waiters, 1, __ATOMIC_RELAXED);
    uint32_t spins = 0;
    uint64_t w0 = __builtin_ia32_rdtsc();
    while (!bkl_try(me)) {
        /* Interrupts on while waiting: the holder may wait for the clock
           (the boot core's PIT tick: a millisecond sleep, a timeout) or
           for this core to flush its TLB. A handler that comes in finds
           the lock busy and leaves its work (include/wynland/smp.h). */
        while (__atomic_load_n(&g_bkl_owner, __ATOMIC_RELAXED) != -1) {
            __asm__ volatile("sti; pause; cli" ::: "memory");
            tlb_poll(c);
            if (++spins >= 2000) {
                spins = 0;
                bkl_sleep_while_held(c);
            }
        }
    }
    bkl_took();
    g_wait_cyc += __builtin_ia32_rdtsc() - w0;
    __atomic_sub_fetch(&g_bkl_waiters, 1, __ATOMIC_RELAXED);
}

void bkl_release_mine(void)
{
    if (g_bkl_owner != (int32_t)this_cpu()->id) return;
    {   /* the running thread ran off the bottom of its kernel stack */
        Thread *t = this_cpu()->current;
        if (t && t->stack_orig && t->stack_orig[0] != KSTACK_GUARD) {
            extern void uint_to_str(uint64_t val, char *buf);
            char b[24];
            serial_write_string("SMP: KERNEL STACK OVERFLOW, thread ");
            uint_to_str(t->id, b);
            serial_write_string(b);
            serial_write_string("\r\n");
            t->stack_orig[0] = KSTACK_GUARD;
        }
    }
    if (g_deferred_due) smp_run_deferred();
    bkl_release_raw();
}

int bkl_irq_enter(void)
{
    int32_t me = (int32_t)this_cpu()->id;
    if (g_bkl_owner == me) return 0;
    if (!bkl_try(me)) return -1;
    bkl_took();
    return 1;
}

void bkl_relax(void)
{
    if (g_cpu_count <= 1) return;
    Cpu *c = this_cpu();
    if (g_bkl_owner != (int32_t)c->id) return;
    if (g_deferred_due) smp_run_deferred();
    if (!g_bkl_waiters) return;
    uint64_t f = irq_save();
    bkl_release_raw();
    /* long enough for a spinning core to see it free and take it */
    for (int i = 0; i < 200 && __atomic_load_n(&g_bkl_owner, __ATOMIC_RELAXED) == -1; i++)
        __builtin_ia32_pause();
    bkl_lock();
    irq_restore(f);
}

void smp_run_deferred(void)
{
    if (!__atomic_exchange_n(&g_deferred_due, 0, __ATOMIC_ACQ_REL)) return;
    extern void irq_deferred_work(void);
    irq_deferred_work();
}

/* ---- local APIC ----------------------------------------------------------- */

#define LAPIC_ID        0x020
#define LAPIC_TPR       0x080
#define LAPIC_EOI       0x0B0
#define LAPIC_SVR       0x0F0
#define LAPIC_ICR_LO    0x300
#define LAPIC_ICR_HI    0x310
#define LAPIC_LVT_TIMER 0x320
#define LAPIC_LVT_LINT0 0x350
#define LAPIC_LVT_LINT1 0x360
#define LAPIC_LVT_ERR   0x370
#define LAPIC_TIMER_INIT 0x380
#define LAPIC_TIMER_CUR 0x390
#define LAPIC_TIMER_DIV 0x3E0

#define LVT_MASKED      (1u << 16)
#define LVT_PERIODIC    (1u << 17)

static volatile uint32_t *g_lapic;     /* xAPIC: registers in memory */
static bool g_x2apic;                  /* x2APIC: registers as MSRs 0x800+ */
static uint32_t g_lapic_per_ms;        /* timer counts per ms (divide by 16) */

static uint32_t lapic_read(uint32_t reg)
{
    if (g_x2apic) return (uint32_t)rdmsr(0x800 + (reg >> 4));
    return g_lapic[reg / 4];
}

static void lapic_write(uint32_t reg, uint32_t v)
{
    if (g_x2apic) { wrmsr(0x800 + (reg >> 4), v); return; }
    g_lapic[reg / 4] = v;
}

void lapic_eoi(void)
{
    if (g_lapic || g_x2apic) lapic_write(LAPIC_EOI, 0);
}

static uint32_t lapic_id(void)
{
    uint32_t v = lapic_read(LAPIC_ID);
    return g_x2apic ? v : v >> 24;
}

static void lapic_send_ipi(uint32_t apic_id, uint32_t icr)
{
    uint64_t f = irq_save();
    if (g_x2apic) {
        wrmsr(0x830, ((uint64_t)apic_id << 32) | icr);
    } else {
        while (lapic_read(LAPIC_ICR_LO) & (1u << 12)) __builtin_ia32_pause();
        lapic_write(LAPIC_ICR_HI, apic_id << 24);
        lapic_write(LAPIC_ICR_LO, icr);
        while (lapic_read(LAPIC_ICR_LO) & (1u << 12)) __builtin_ia32_pause();
    }
    irq_restore(f);
}

/* This core's APIC on: the spurious vector, everything accepted. The boot
   core keeps its LINT0 (ExtINT from the PIC, how the legacy IRQs come in)
   and gets no APIC timer -- the PIT is its clock; the others mask LINT0/1
   and tick on their own APIC timer (preemption). */
static void lapic_setup(bool boot_core)
{
    if (g_x2apic) {
        uint64_t b = rdmsr(MSR_APIC_BASE);
        if ((b & (3u << 10)) != (3u << 10)) wrmsr(MSR_APIC_BASE, b | (3u << 10));
    }
    lapic_write(LAPIC_SVR, 0x100 | VEC_SPURIOUS);
    lapic_write(LAPIC_TPR, 0);
    if (boot_core) return;
    lapic_write(LAPIC_LVT_LINT0, LVT_MASKED);
    lapic_write(LAPIC_LVT_LINT1, LVT_MASKED);
    lapic_write(LAPIC_LVT_ERR, LVT_MASKED);
    lapic_write(LAPIC_TIMER_DIV, 0x3);              /* divide by 16 */
    lapic_write(LAPIC_LVT_TIMER, LVT_PERIODIC | VEC_LAPIC_TIMER);
    lapic_write(LAPIC_TIMER_INIT, g_lapic_per_ms ? g_lapic_per_ms : 100000);
}

static void delay_ms(uint32_t ms)
{
    uint64_t t0 = timer_get_ms();
    while (timer_get_ms() - t0 < ms) __builtin_ia32_pause();
}

/* The APIC timer's rate against the PIT's millisecond clock (the boot
   core's own timer, one-shot and masked, counting down meanwhile). */
static void lapic_calibrate(void)
{
    lapic_write(LAPIC_TIMER_DIV, 0x3);
    lapic_write(LAPIC_LVT_TIMER, LVT_MASKED | VEC_LAPIC_TIMER);
    uint64_t t0 = timer_get_ms();
    while (timer_get_ms() == t0) __builtin_ia32_pause();     /* a tick edge */
    lapic_write(LAPIC_TIMER_INIT, 0xFFFFFFFFu);
    delay_ms(50);
    uint32_t left = lapic_read(LAPIC_TIMER_CUR);
    lapic_write(LAPIC_TIMER_INIT, 0);
    g_lapic_per_ms = (0xFFFFFFFFu - left) / 50;
}

/* ---- TLB shootdown, the sending side ------------------------------------- */

void tlb_shootdown(void *pml4)
{
    if (g_cpu_count <= 1) return;
    Cpu *me = this_cpu();
    uint64_t f = irq_save();
    uint32_t pending = 0;
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        Cpu *c = &g_cpus[i];
        if (c == me || !c->online) continue;
        if (pml4 && c->cr3 != V2P(pml4)) continue;
        c->tlb_flush = 1;
        pending |= 1u << i;
        lapic_send_ipi(c->apic_id, VEC_IPI_TLB);
    }
    uint64_t t0 = __builtin_ia32_rdtsc();
    while (pending) {
        for (uint32_t i = 0; i < g_cpu_count; i++)
            if ((pending & (1u << i)) && !__atomic_load_n(&g_cpus[i].tlb_flush, __ATOMIC_ACQUIRE))
                pending &= ~(1u << i);
        if (!pending) break;
        __builtin_ia32_pause();
        if (__builtin_ia32_rdtsc() - t0 > 4000000000ULL) {   /* > ~1 s: a core with interrupts off for good */
            serial_write_string("SMP: TLB shootdown: a core does not answer\r\n");
            break;
        }
    }
    irq_restore(f);
}

void smp_kick_idle(void)
{
    if (g_cpu_count <= 1) return;
    Cpu *me = this_cpu();
    for (uint32_t i = 0; i < g_cpu_count; i++) {
        Cpu *c = &g_cpus[i];
        if (c != me && c->online && c->current == c->idle) {
            lapic_send_ipi(c->apic_id, VEC_IPI_WAKE);
            return;
        }
    }
}

/* ---- ACPI: which cores exist --------------------------------------------- */

typedef struct PACKED {
    char     sig[4];
    uint32_t len;
    uint8_t  rev, csum;
    char     oem[6], oem_table[8];
    uint32_t oem_rev, creator, creator_rev;
} AcpiHeader;

/* ACPI tables live in firmware memory the direct map may have left out
   (reserved ranges): map what is read, at P2V() of it. */
static void acpi_map(uint64_t addr, uint64_t len)
{
    PageTable *k = vmm_get_kernel_pml4();
    for (uint64_t a = addr & ~0xFFFULL; a < addr + len; a += PAGE_SIZE)
        if (!vmm_get_phys(k, HHDM_BASE + a)) vmm_map_page(k, HHDM_BASE + a, a, PAGE_NX);
}

static const AcpiHeader *acpi_find(uint64_t rsdp, const char *sig)
{
    if (!rsdp) return NULL;
    acpi_map(rsdp, 36);
    const uint8_t *r = (const uint8_t *)P2V(rsdp);
    bool xsdt = r[15] >= 2 && *(const uint64_t *)(r + 24);
    uint64_t sdt = xsdt ? *(const uint64_t *)(r + 24) : *(const uint32_t *)(r + 16);
    acpi_map(sdt, sizeof(AcpiHeader));
    const AcpiHeader *h = (const AcpiHeader *)P2V(sdt);
    acpi_map(sdt, h->len);
    uint32_t n = (h->len - sizeof(AcpiHeader)) / (xsdt ? 8 : 4);
    const uint8_t *ents = (const uint8_t *)h + sizeof(AcpiHeader);
    for (uint32_t i = 0; i < n; i++) {
        uint64_t p = xsdt ? *(const uint64_t *)(ents + 8 * i) : *(const uint32_t *)(ents + 4 * i);
        acpi_map(p, sizeof(AcpiHeader));
        const AcpiHeader *t = (const AcpiHeader *)P2V(p);
        if (t->sig[0] == sig[0] && t->sig[1] == sig[1] && t->sig[2] == sig[2] && t->sig[3] == sig[3]) {
            acpi_map(p, t->len);
            return t;
        }
    }
    return NULL;
}

/* ---- starting the other cores --------------------------------------------- */

extern char ap_trampoline_start[], ap_trampoline_end[];
extern char ap_tr_cr3[], ap_tr_stack[], ap_tr_cpu[], ap_tr_entry[];
#define TR_VAR(base, sym) (*(volatile uint64_t *)P2V((base) + ((uintptr_t)(sym) - (uintptr_t)ap_trampoline_start)))

/* What every core must have like the boot core (control registers, the
   FPU components, the PAT's write-combining slot). */
static uint64_t g_bsp_cr0, g_bsp_cr4, g_bsp_xcr0, g_bsp_pat;

extern void gdt_load_ap(uint32_t id);
extern void idt_load(void);
extern void syscall_init_cpu(void);
extern Thread *sched_new_ap_idle(void);
extern void sched_idle_loop_ap(void) __attribute__((noreturn));
extern void gdt_update_tss_rsp0(uint64_t rsp0);

void ap_main(Cpu *c) __attribute__((noreturn));
void ap_main(Cpu *c)
{
    smp_set_gs(c);
    __asm__ volatile("mov %0, %%cr0" :: "r"(g_bsp_cr0));
    __asm__ volatile("mov %0, %%cr4" :: "r"(g_bsp_cr4));
    if (g_fpu_xsave)
        __asm__ volatile("xsetbv" :: "c"(0), "a"((uint32_t)g_bsp_xcr0), "d"((uint32_t)(g_bsp_xcr0 >> 32)));
    wrmsr(MSR_PAT, g_bsp_pat);
    wrmsr(MSR_EFER, rdmsr(MSR_EFER) | 1 | (g_page_nx_bit ? (1ULL << 11) : 0));

    gdt_load_ap(c->id);
    smp_set_gs(c);                     /* the segment reload cleared GS's base */
    idt_load();
    syscall_init_cpu();

    PageTable *k = vmm_get_kernel_pml4();
    c->cr3 = V2P(k);
    __asm__ volatile("mov %0, %%cr3" :: "r"(c->cr3) : "memory");

    __asm__ volatile("fninit");
    uint32_t mxcsr = 0x1F80;
    __asm__ volatile("ldmxcsr %0" :: "m"(mxcsr));

    Thread *idle = c->idle;
    c->current = idle;
    c->fx_user = thread_fx_user(idle);
    gdt_update_tss_rsp0(((uint64_t)(uintptr_t)idle->stack_orig + THREAD_STACK_SIZE) & ~0xFULL);

    lapic_setup(false);
    __atomic_store_n(&c->online, true, __ATOMIC_RELEASE);
    sched_idle_loop_ap();
}

/* Two free pages below 1 MB (the SIPI vector addresses 4 KB pages there;
   the kernel's physical allocator never hands out the first megabyte):
   the start-up code, and a copy of the kernel's PML4 a 32-bit CR3 load
   can reach. */
static uint64_t low_free_page(BootInfo *bi, uint64_t after)
{
    MemoryRegion *r = (MemoryRegion *)(uintptr_t)bi->mmap_addr;
    for (uint64_t a = after; a < 0x9F000; a += PAGE_SIZE) {
        for (uint32_t i = 0; i < bi->mmap_entries; i++)
            if (r[i].type == MEMORY_USABLE && a >= r[i].base && a + PAGE_SIZE <= r[i].base + r[i].size)
                return a;
    }
    return 0;
}

static void log_num(const char *a, uint64_t v, const char *b)
{
    char buf[24];
    serial_write_string(a);
    uint_to_str(v, buf);
    serial_write_string(buf);
    serial_write_string(b);
}

void smp_init(BootInfo *bi)
{
    uint64_t apic_base = rdmsr(MSR_APIC_BASE);
    g_x2apic = (apic_base & (1u << 10)) != 0;
    if (!g_x2apic) {
        uint64_t phys = apic_base & 0xFFFFF000ULL;
        g_lapic = (volatile uint32_t *)vmm_map_mmio(phys, PAGE_SIZE);
    }
    Cpu *bsp = &g_cpus[0];
    bsp->apic_id = lapic_id();
    bsp->online = true;
    lapic_setup(true);

    const uint8_t *madt = (const uint8_t *)acpi_find(bi->rsdp_addr, "APIC");
    if (!madt) {
        serial_write_string("SMP: no ACPI MADT, one core\r\n");
        return;
    }

    uint64_t tramp = low_free_page(bi, 0x1000);
    uint64_t pml4c = tramp ? low_free_page(bi, tramp + PAGE_SIZE) : 0;
    uint64_t tr_len = (uint64_t)(ap_trampoline_end - ap_trampoline_start);
    if (!tramp || !pml4c || tr_len > PAGE_SIZE) {
        serial_write_string("SMP: no low memory for the start-up code, one core\r\n");
        return;
    }
    /* the start-up code turns paging on while running at its physical
       address: the kernel's PML4 (and so the copy) maps that page at
       itself -- the kernel's own, not any process's (their lower half is
       theirs alone) */
    PageTable *k = vmm_get_kernel_pml4();
    vmm_map_page(k, tramp, tramp, PAGE_WRITE);       /* executable, identity */
    memcpy(P2V(tramp), ap_trampoline_start, tr_len);
    memcpy(P2V(pml4c), k, PAGE_SIZE);
    TR_VAR(tramp, ap_tr_cr3) = pml4c;
    TR_VAR(tramp, ap_tr_entry) = (uint64_t)(uintptr_t)ap_main;

    __asm__ volatile("mov %%cr0, %0" : "=r"(g_bsp_cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(g_bsp_cr4));
    if (g_fpu_xsave) {
        uint32_t lo, hi;
        __asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
        g_bsp_xcr0 = ((uint64_t)hi << 32) | lo;
    }
    g_bsp_pat = rdmsr(MSR_PAT);

    lapic_calibrate();
    log_num("SMP: APIC timer ", g_lapic_per_ms, " counts/ms\r\n");

    uint32_t len = ((const AcpiHeader *)madt)->len;
    for (uint32_t off = sizeof(AcpiHeader) + 8; off + 2 <= len; off += madt[off + 1]) {
        if (madt[off + 1] < 2) break;
        uint32_t apic, flags;
        if (madt[off] == 0) {                       /* processor local APIC */
            apic = madt[off + 3];
            flags = *(const uint32_t *)(madt + off + 4);
        } else if (madt[off] == 9) {                /* processor local x2APIC */
            apic = *(const uint32_t *)(madt + off + 4);
            flags = *(const uint32_t *)(madt + off + 8);
        } else continue;
        if (!(flags & 3) || apic == bsp->apic_id) continue;   /* neither enabled nor startable */
        if (g_cpu_count >= MAX_CPUS) break;

        uint32_t n = g_cpu_count;
        Cpu *c = &g_cpus[n];
        c->self = c;
        c->id = n;
        c->apic_id = apic;
        c->idle = sched_new_ap_idle();
        if (!c->idle) break;
        TR_VAR(tramp, ap_tr_stack) = ((uint64_t)(uintptr_t)c->idle->stack_orig + THREAD_STACK_SIZE) & ~0xFULL;
        TR_VAR(tramp, ap_tr_cpu) = (uint64_t)(uintptr_t)c;
        __sync_synchronize();

        lapic_send_ipi(apic, 0x4500);                       /* INIT */
        delay_ms(10);
        lapic_send_ipi(apic, 0x4600 | (uint32_t)(tramp >> 12));   /* SIPI */
        delay_ms(2);
        if (!c->online) lapic_send_ipi(apic, 0x4600 | (uint32_t)(tramp >> 12));
        for (int i = 0; i < 500 && !c->online; i++) delay_ms(1);
        if (!c->online) {
            log_num("SMP: core with APIC id ", apic, " did not start\r\n");
            continue;
        }
        __atomic_store_n(&g_cpu_count, n + 1, __ATOMIC_RELEASE);
    }
    log_num("SMP: ", g_cpu_count, g_cpu_count == 1 ? " core\r\n" : " cores online\r\n");
}
