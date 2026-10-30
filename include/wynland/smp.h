/*
 * WynlandOS - Symmetric multiprocessing (several CPU cores)
 * =========================================================
 * Every core runs user code on its own; the kernel itself is still one
 * piece of code written for one core at a time (it protects its data by
 * turning interrupts off, which only stops the local core). So the kernel
 * runs under one lock, the big kernel lock (BKL): a core takes it when it
 * enters the kernel from user mode (a syscall, a page fault, a timer tick
 * that preempts) and gives it back on the way out to user mode or before
 * its idle hlt. While one core is in the kernel the others keep running
 * user code; one that needs the kernel too waits for the lock.
 *
 * The lock belongs to the thread in the kernel (Thread.bkl): one that
 * blocks or is preempted there gives it back while it is switched away and
 * takes it again before it goes on (the scheduler does both). The
 * scheduler itself runs without it, under its own spinlock
 * (include/wynland/spinlock.h), so a core switches threads while another
 * core is in the kernel.
 *
 * Interrupt handlers never wait for the lock: they take it if it is free
 * (or already theirs) and otherwise leave their work to whoever holds it
 * (smp_run_deferred(): the device polling the timer tick does, the PS/2
 * bytes). A handler spinning for the lock with interrupts off could wait
 * forever on a holder that waits for that very interrupt.
 */
#pragma once

#include <wynland/types.h>
#include <wynland/boot_info.h>

#define MAX_CPUS 16

/* Interrupt vectors of the local APIC (above the PIC's 0x20..0x2F) */
#define VEC_LAPIC_TIMER  0x40   /* each AP's own 1 kHz clock: preemption */
#define VEC_IPI_WAKE     0x41   /* "a thread is ready": out of the idle hlt */
#define VEC_IPI_TLB      0x42   /* page tables changed: flush the TLB */
#define VEC_GPU          0x50   /* the Intel GPU's MSI (drivers/gpu/i915.c) */
#define VEC_SPURIOUS     0xFF

struct Thread;

/* One per core, reached through GS (IA32_GS_BASE while in the kernel; the
   entry stubs swapgs on every user/kernel transition). The first fields
   are read by assembly at fixed offsets (CPU_* in kernel/percpu.inc). */
typedef struct Cpu {
    struct Cpu *self;            /* gs:0  -- this_cpu() */
    uint64_t user_rsp;           /* gs:8  -- syscall_entry's scratch */
    uint64_t kstack;             /* gs:16 -- kernel stack top of the running thread */
    uint8_t *fx_user;            /* gs:24 -- its user FPU image */
    uint32_t id;                 /* gs:32 -- index in g_cpus */
    uint32_t apic_id;
    struct Thread *current;      /* the thread running here */
    struct Thread *idle;         /* this core's idle thread (never runs elsewhere) */
    uint64_t cr3;                /* the page tables loaded here */
    volatile uint32_t tlb_flush; /* set by tlb_shootdown(), cleared once flushed */
    volatile bool online;
    bool prof_exc;               /* the big lock held for an exception from user code (its profile) */
    /* where its 1 kHz ticks went (kernel/irq.c tick_account; /proc/stat) */
    volatile uint64_t ticks_user, ticks_sys, ticks_idle;
} Cpu;

extern Cpu g_cpus[MAX_CPUS];
extern volatile uint32_t g_cpu_count;      /* cores online */
extern volatile int32_t g_bkl_owner;        /* core id holding the lock, -1 free */

static inline Cpu *this_cpu(void)
{
    Cpu *c;
    __asm__ volatile("mov %%gs:0, %0" : "=r"(c));
    return c;
}

/* GS base of this core (boot: before anything reads this_cpu()) */
void smp_set_gs(Cpu *c);

/* The big kernel lock. bkl_lock(): spin until held (interrupts off; a
   TLB flush request is answered meanwhile). bkl_release_mine(): release
   if this core holds it (the exit paths to user mode, the idle loop).
   bkl_irq_enter(): an interrupt handler's way in -- 0 already held by
   this core, 1 taken now, -1 busy elsewhere (leave the work). */
void bkl_lock(void);
void bkl_release_mine(void);
int  bkl_irq_enter(void);
/* A long wait inside a syscall that needs none of the big lock's data
   (its own lock guards what it touches): out of the lock for it, and back.
   bkl_drop() returns whether this core held it. */
bool bkl_drop(void);
void bkl_retake(bool had);
/* the handler's way out after bkl_irq_enter() returned 1 */
void bkl_irq_leave(void);
/* the scheduler, switching away from a thread: the lock goes back if this
   core holds it (the thread's Thread.bkl stays: it takes it again) */
void bkl_switch_out(void);
/* sched_yield() found nothing else to run: let a core waiting for the
   lock have it for a moment (a lock holder spinning on yield would
   otherwise keep everyone out, the one it waits for included). */
void bkl_relax(void);
/* answer a pending TLB flush request (loops waiting with interrupts off) */
void smp_tlb_poll(void);

/* Work an interrupt handler could not do (the lock was busy): the
   timer's device polling and the PS/2 bytes. Run by the lock holder at
   the next chance; flags set by the handlers. */
extern volatile uint32_t g_deferred_due;
void smp_run_deferred(void);

/* Bring up the other cores (ACPI MADT, INIT-SIPI-SIPI); each ends up in
   its own idle loop, picking threads like the boot core does. */
void smp_init(BootInfo *bi);

/* Page tables `pml4` changed (an entry that was present): every other
   core that has them loaded flushes its TLB before this returns. NULL:
   shared kernel mappings, every core. */
void tlb_shootdown(void *pml4);

/* A thread became ready: wake an idle core (hlt) to run it. */
void smp_kick_idle(void);

/* Local APIC end of interrupt / vectors 0x40+ (kernel/irq.c calls it) */
void lapic_eoi(void);
