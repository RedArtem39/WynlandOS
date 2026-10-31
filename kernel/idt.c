/*
 * WynlandOS - Interrupt Descriptor Table (IDT) Implementation
 */

#include <wynland/idt.h>
#include <wynland/boot_info.h>
#include <wynland/vmm.h>
#include <wynland/pmm.h>
#include <wynland/sched.h>
#include <wynland/process.h>
#include <wynland/vma.h>
#include <wynland/signal.h>
#include <wynland/smp.h>

extern void *memcpy(void *dest, const void *src, size_t n);

/* Declare all assembly ISR stubs */
extern void isr0();
extern void isr1();
extern void isr2();
extern void isr3();
extern void isr4();
extern void isr5();
extern void isr6();
extern void isr7();
extern void isr8();
extern void isr9();
extern void isr10();
extern void isr11();
extern void isr12();
extern void isr13();
extern void isr14();
extern void isr15();
extern void isr16();
extern void isr17();
extern void isr18();
extern void isr19();
extern void isr20();
extern void isr21();
extern void isr22();
extern void isr23();
extern void isr24();
extern void isr25();
extern void isr26();
extern void isr27();
extern void isr28();
extern void isr29();
extern void isr30();
extern void isr31();

/* Declare all assembly IRQ stubs */
extern void irq0();
extern void irq1();
extern void irq2();
extern void irq3();
extern void irq4();
extern void irq5();
extern void irq6();
extern void irq7();
extern void irq8();
extern void irq9();
extern void irq10();
extern void irq11();
extern void irq12();
extern void irq13();
extern void irq14();
extern void irq15();

/* IDT and IDTR pointer */
static IdtEntry idt[IDT_ENTRIES];
static IdtPtr   idt_ptr;
BootInfo *g_boot_info = NULL;

/* Non-static helper declarations from main.c */
extern void console_print_string(BootInfo *info, const char *str, uint32_t fg, uint32_t bg);
extern void console_print_char(BootInfo *info, char c, uint32_t fg, uint32_t bg);
extern void uint_to_hex(uint64_t val, char *buf);
extern void uint_to_str(uint64_t val, char *buf);
extern void serial_write_string(const char *str);

static const char *exception_messages[] = {
    "Division By Zero (#DE)",
    "Debug (#DB)",
    "Non Maskable Interrupt",
    "Breakpoint (#BP)",
    "Overflow (#OF)",
    "Out of Bounds (#BR)",
    "Invalid Opcode (#UD)",
    "No Coprocessor (#NM)",
    "Double Fault (#DF)",
    "Coprocessor Segment Overrun",
    "Bad TSS (#TS)",
    "Segment Not Present (#NP)",
    "Stack Fault (#SS)",
    "General Protection Fault (#GP)",
    "Page Fault (#PF)",
    "Unknown Interrupt",
    "Coprocessor Fault",
    "Alignment Check (#AC)",
    "Machine Check (#MC)",
    "SIMD Floating-Point Exception (#XM)",
    "Virtualization Exception (#VE)",
    "Control Protection Exception (#CP)",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Reserved",
    "Hypervisor Injection Exception",
    "VMM Communication Exception (#VC)",
    "Security Exception (#SE)",
    "Reserved"
};

static void idt_set_gate(uint8_t num, uint64_t base, uint16_t sel, uint8_t flags)
{
    idt[num].offset_low = (uint16_t)(base & 0xFFFF);
    idt[num].selector = sel;
    idt[num].ist = 0;
    idt[num].type_attributes = flags;
    idt[num].offset_middle = (uint16_t)((base >> 16) & 0xFFFF);
    idt[num].offset_high = (uint32_t)((base >> 32) & 0xFFFFFFFF);
    idt[num].reserved = 0;
}

void idt_init(BootInfo *boot_info)
{
    g_boot_info = boot_info;

    /* Initialize IDT pointer */
    idt_ptr.limit = (sizeof(IdtEntry) * IDT_ENTRIES) - 1;
    idt_ptr.base  = (uint64_t)&idt;

    /* Zero out all IDT gates initially */
    for (int i = 0; i < IDT_ENTRIES; i++) {
        idt_set_gate(i, 0, 0, 0);
    }

    /* Register CPU exceptions 0..31
     * Access byte 0x8E: Present, Ring 0, 64-bit Interrupt Gate */
    idt_set_gate(0,  (uint64_t)isr0,  0x08, 0x8E);
    idt_set_gate(1,  (uint64_t)isr1,  0x08, 0x8E);
    idt_set_gate(2,  (uint64_t)isr2,  0x08, 0x8E);
    idt_set_gate(3,  (uint64_t)isr3,  0x08, 0x8E);
    idt_set_gate(4,  (uint64_t)isr4,  0x08, 0x8E);
    idt_set_gate(5,  (uint64_t)isr5,  0x08, 0x8E);
    idt_set_gate(6,  (uint64_t)isr6,  0x08, 0x8E);
    idt_set_gate(7,  (uint64_t)isr7,  0x08, 0x8E);
    idt_set_gate(8,  (uint64_t)isr8,  0x08, 0x8E);
    idt_set_gate(9,  (uint64_t)isr9,  0x08, 0x8E);
    idt_set_gate(10, (uint64_t)isr10, 0x08, 0x8E);
    idt_set_gate(11, (uint64_t)isr11, 0x08, 0x8E);
    idt_set_gate(12, (uint64_t)isr12, 0x08, 0x8E);
    idt_set_gate(13, (uint64_t)isr13, 0x08, 0x8E);
    idt_set_gate(14, (uint64_t)isr14, 0x08, 0x8E);
    idt_set_gate(15, (uint64_t)isr15, 0x08, 0x8E);
    idt_set_gate(16, (uint64_t)isr16, 0x08, 0x8E);
    idt_set_gate(17, (uint64_t)isr17, 0x08, 0x8E);
    idt_set_gate(18, (uint64_t)isr18, 0x08, 0x8E);
    idt_set_gate(19, (uint64_t)isr19, 0x08, 0x8E);
    idt_set_gate(20, (uint64_t)isr20, 0x08, 0x8E);
    idt_set_gate(21, (uint64_t)isr21, 0x08, 0x8E);
    idt_set_gate(22, (uint64_t)isr22, 0x08, 0x8E);
    idt_set_gate(23, (uint64_t)isr23, 0x08, 0x8E);
    idt_set_gate(24, (uint64_t)isr24, 0x08, 0x8E);
    idt_set_gate(25, (uint64_t)isr25, 0x08, 0x8E);
    idt_set_gate(26, (uint64_t)isr26, 0x08, 0x8E);
    idt_set_gate(27, (uint64_t)isr27, 0x08, 0x8E);
    idt_set_gate(28, (uint64_t)isr28, 0x08, 0x8E);
    idt_set_gate(29, (uint64_t)isr29, 0x08, 0x8E);
    idt_set_gate(30, (uint64_t)isr30, 0x08, 0x8E);
    idt_set_gate(31, (uint64_t)isr31, 0x08, 0x8E);

    /* Register Hardware Interrupts (IRQs) 32..47
     * Access byte 0x8E: Present, Ring 0, 64-bit Interrupt Gate */
    idt_set_gate(32, (uint64_t)irq0,  0x08, 0x8E);
    idt_set_gate(33, (uint64_t)irq1,  0x08, 0x8E);
    idt_set_gate(34, (uint64_t)irq2,  0x08, 0x8E);
    idt_set_gate(35, (uint64_t)irq3,  0x08, 0x8E);
    idt_set_gate(36, (uint64_t)irq4,  0x08, 0x8E);
    idt_set_gate(37, (uint64_t)irq5,  0x08, 0x8E);
    idt_set_gate(38, (uint64_t)irq6,  0x08, 0x8E);
    idt_set_gate(39, (uint64_t)irq7,  0x08, 0x8E);
    idt_set_gate(40, (uint64_t)irq8,  0x08, 0x8E);
    idt_set_gate(41, (uint64_t)irq9,  0x08, 0x8E);
    idt_set_gate(42, (uint64_t)irq10, 0x08, 0x8E);
    idt_set_gate(43, (uint64_t)irq11, 0x08, 0x8E);
    idt_set_gate(44, (uint64_t)irq12, 0x08, 0x8E);
    idt_set_gate(45, (uint64_t)irq13, 0x08, 0x8E);
    idt_set_gate(46, (uint64_t)irq14, 0x08, 0x8E);
    idt_set_gate(47, (uint64_t)irq15, 0x08, 0x8E);

    /* The local APIC's vectors (include/wynland/smp.h) */
    extern void irqlapic_timer(), irqipi_wake(), irqipi_tlb(), irqgpu(), irq_spurious();
    idt_set_gate(0x40, (uint64_t)irqlapic_timer, 0x08, 0x8E);
    idt_set_gate(0x41, (uint64_t)irqipi_wake,    0x08, 0x8E);
    idt_set_gate(0x42, (uint64_t)irqipi_tlb,     0x08, 0x8E);
    idt_set_gate(0x50, (uint64_t)irqgpu,         0x08, 0x8E);
    idt_set_gate(0xFF, (uint64_t)irq_spurious,   0x08, 0x8E);

    /* Load IDT register */
    idt_load();
}

/* The same IDT on every core */
void idt_load(void)
{
    __asm__ volatile("lidt %0" :: "m"(idt_ptr));
}

static void print_reg(BootInfo *info, const char *name, uint64_t val)
{
    char hex[32];
    uint_to_hex(val, hex);
    console_print_string(info, "  ", 0x00FFFFFF, 0x007f0000);
    console_print_string(info, name, 0x00AAAAAA, 0x007f0000);
    console_print_string(info, ": ", 0x00AAAAAA, 0x007f0000);
    console_print_string(info, hex, 0x00FFFF00, 0x007f0000);
    console_print_string(info, "\n", 0x00FFFFFF, 0x007f0000);

    /* Mirror to serial */
    serial_write_string("  ");
    serial_write_string(name);
    serial_write_string(": ");
    serial_write_string(hex);
    serial_write_string("\r\n");
}

static void exception_handler_main(InterruptRegisters *regs);

/* A page fault in the kernel on a user address, from code outside the big
   kernel lock (a lock-free syscall's copy, kernel/usercopy.c): resolving
   it changes page tables, which are the lock's -- taken for the handling,
   given back after. A fault the handler cannot resolve on the copy
   instruction of kernel/uaccess.asm returns -EFAULT from the copy. */
void exception_handler(InterruptRegisters *regs)
{
    bool took = false;
    if (regs->int_no == 2) {                          /* NMI: the stall watchdog's (kernel/stallwatch.c) */
        extern void stall_nmi(InterruptRegisters *r);
        stall_nmi(regs);
        return;
    }
    extern char uaccess_nofault_insn[], uaccess_nofault_fixup[], uaccess_cmpxchg_insn[];
    if (regs->int_no == 14 && (regs->cs & 3) != 3 &&
        (regs->rip == (uint64_t)(uintptr_t)uaccess_nofault_insn || regs->rip == (uint64_t)(uintptr_t)uaccess_cmpxchg_insn)) {
        regs->rip = (uint64_t)(uintptr_t)uaccess_nofault_fixup;   /* no resolving at all */
        return;
    }
    /* A user page fault: a fresh anonymous page or a copy-on-write
       resolved under the process's own mm_lock, without the big lock (the
       entry stub left it for here); anything else takes the big lock, as
       every other exception from user mode. */
    if (regs->int_no == 14 && (regs->cs & 3) == 3) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        Thread *ut = sched_current();
        Process *up = ut ? ut->proc : NULL;
        if (up && cr2 < 0x0000800000000000ULL) {
            uint32_t access = (regs->err_code & 0x10) ? VMA_PROT_EXEC :
                              (regs->err_code & 0x2) ? VMA_PROT_WRITE : VMA_PROT_READ;
            uint64_t t0 = __builtin_ia32_rdtsc();
            kmutex_lock(&up->mm_lock);
            bool done = vma_fault_fast(up, cr2, access, regs->err_code);
            kmutex_unlock(&up->mm_lock);
            if (done) {
                up->st_faults++;
                up->st_fault_tsc += __builtin_ia32_rdtsc() - t0;
                return;
            }
        }
        bkl_lock();
        took = true;
        /* (a sleep on either lock let another fault on this core change
           CR2: the full path below reads it again) */
        __asm__ volatile("mov %0, %%cr2" :: "r"(cr2) : "memory");
    }
    if (regs->int_no == 14 && (regs->cs & 3) != 3) {
        uint64_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        Cpu *c = this_cpu();
        if (cr2 < 0x0000800000000000ULL && c->current && c->current != c->idle &&
            g_bkl_owner != (int32_t)c->id) {
            bkl_lock();
            took = true;
        }
    }
    /* the big lock's profile (kernel/smp.c): this hold is an exception's,
       not the thread's last syscall's */
    if ((regs->cs & 3) == 3) this_cpu()->prof_exc = true;
    exception_handler_main(regs);
    if (took) bkl_drop();
}

/* exception_handler_main() on the copy instruction: the fatal path below
   sends it to the fixup instead of crashing the kernel */
static bool uaccess_fault_fixup(InterruptRegisters *regs)
{
    extern char uaccess_copy_insn[], uaccess_fixup[], uaccess_touch_insn[];
    if (regs->int_no != 14 || (regs->cs & 3) == 3) return false;
    if (regs->rip != (uint64_t)(uintptr_t)uaccess_copy_insn && regs->rip != (uint64_t)(uintptr_t)uaccess_touch_insn) return false;
    regs->rip = (uint64_t)(uintptr_t)uaccess_fixup;
    return true;
}

static void exception_handler_main(InterruptRegisters *regs)
{
    /* Phase 22c: COW resolution. A write fault (err_code: PRESENT|WRITE
       both set) to a leaf tagged PAGE_COW (see vmm_cow_clone_user_pages())
       isn't a real crash -- it's fork()'s deferred copy actually happening.
       Resolve it and return to re-execute the faulting instruction instead
       of falling through to the fatal path below. Every other fault
       (not-present, a genuine read-only violation, a guard page, kernel
       mode) is unaffected and keeps today's behavior exactly. */
    /* Kernel-mode writes count too (CR0.WP is on): a syscall storing into a
       user buffer that is still COW-shared after fork() gets its private
       copy here, exactly like the user's own write would. Only for the
       user half of the address space. */
    uint64_t pf_cr2 = 0;
    if (regs->int_no == 14) __asm__ volatile("mov %%cr2, %0" : "=r"(pf_cr2));
    /* the address space's lock over the resolutions below (the fast path's
       is taken without the big lock: they exclude each other by it) */
    Thread *pf_t = sched_current();
    Process *pf_mm = (regs->int_no == 14 && pf_cr2 < 0x0000800000000000ULL && pf_t) ? pf_t->proc : NULL;
    if (pf_mm) kmutex_lock(&pf_mm->mm_lock);

    /* Demand paging: a not-present fault (err bit 0 clear) in a lazy
       anonymous region gets its zeroed frame now, and the instruction
       runs again. From user mode, or from the kernel touching the user
       half on a process's behalf. Instruction fetch (bit 4) needs EXEC,
       write (bit 1) needs WRITE. */
    if (regs->int_no == 14 && !(regs->err_code & 0x1) &&
        ((regs->cs & 0x03) == 3 || pf_cr2 < 0x0000800000000000ULL)) {
        Thread *ft = sched_current();
        uint32_t access = VMA_PROT_READ;
        if (regs->err_code & 0x2) access = VMA_PROT_WRITE;
        if (regs->err_code & 0x10) access = VMA_PROT_EXEC;
        if (ft && ft->proc) {
            uint64_t t0 = __builtin_ia32_rdtsc();
            bool ok = vma_fault_in(ft->proc, pf_cr2, access);
            ft->proc->st_faults++;
            ft->proc->st_fault_tsc += __builtin_ia32_rdtsc() - t0;
            if (ok) { if (pf_mm) kmutex_unlock(&pf_mm->mm_lock); return; }
        }
    }
    if (regs->int_no == 14 && (regs->err_code & 0x3) == 0x3 &&
        ((regs->cs & 0x03) == 3 || pf_cr2 < 0x0000800000000000ULL)) {
        uint64_t cr2 = pf_cr2;
        PageTable *pml4 = vmm_get_current_pml4();
        uint64_t flags = vmm_get_page_flags(pml4, cr2);

        if (flags & PAGE_COW) {
            /* vmm_resolve_cow_page() only returns false on OOM duplicating
               a still-shared page; fall through to the fatal path below
               rather than spin re-faulting forever in that case. */
            if (vmm_resolve_cow_page(pml4, cr2)) {
                if (pf_mm) kmutex_unlock(&pf_mm->mm_lock);
                return;
            }
        }
    }

    /* A fault the page tables allow by now: this core's TLB still had the
       page's old, narrower entry -- another core's thread of the process
       resolved the COW, brought the page in, or mprotect() widened it (an
       upgrade sends no shootdown). The fault dropped that entry; the
       access runs again. A few times in a row at one address is no stale
       entry: the fatal path. */
    if (regs->int_no == 14 && (regs->err_code & 0x1) &&
        ((regs->cs & 0x03) == 3 || pf_cr2 < 0x0000800000000000ULL)) {
        uint64_t f = vmm_get_page_flags(vmm_get_current_pml4(), pf_cr2);
        bool allowed = (f & PAGE_PRESENT) && !(f & PAGE_COW);
        if ((regs->err_code & 0x2) && !(f & PAGE_WRITE)) allowed = false;
        if ((regs->err_code & 0x4) && !(f & PAGE_USER)) allowed = false;
        if ((regs->err_code & 0x10) && PAGE_NX && (f & PAGE_NX)) allowed = false;
        if (regs->err_code & 0x8) allowed = false;          /* reserved bits: a real fault */
        Thread *ft = sched_current();
        if (allowed && ft) {
            uint64_t pg = pf_cr2 & ~0xFFFULL, now = __builtin_ia32_rdtsc();
            if (ft->spurious_addr == pg && now - ft->spurious_tsc < 100000000ULL) ft->spurious_n++;
            else { ft->spurious_addr = pg; ft->spurious_n = 1; }
            ft->spurious_tsc = now;
            if (ft->spurious_n <= 4) {
                __asm__ volatile("invlpg (%0)" :: "r"(pf_cr2) : "memory");
                if (pf_mm) kmutex_unlock(&pf_mm->mm_lock);
                return;
            }
        }
    }
    if (pf_mm) kmutex_unlock(&pf_mm->mm_lock);

    if (uaccess_fault_fixup(regs)) return;   /* a lock-free copy: -EFAULT */

    /* Check if the exception happened in user mode (Ring 3) */
    if ((regs->cs & 0x03) == 3) {
        serial_write_string("\r\n======================================\r\n");
        serial_write_string("!!! USER MODE PROCESS CRASHED !!!\r\n");
        {
            extern void uint_to_str(uint64_t val, char *buf);
            char b[24];
            uint32_t cpu_id;
            __asm__ volatile("mov %%gs:32, %0" : "=r"(cpu_id));
            Thread *ct = sched_current();
            serial_write_string("CPU "); uint_to_str(cpu_id, b); serial_write_string(b);
            if (ct) {
                serial_write_string(", thread "); uint_to_str(ct->id, b); serial_write_string(b);
                if (ct->proc) { serial_write_string(" of "); serial_write_string(ct->proc->exe_path); }
            }
            serial_write_string("\r\n");
        }
        serial_write_string("Exception: ");
        if (regs->int_no < 32) {
            serial_write_string(exception_messages[regs->int_no]);
        } else {
            serial_write_string("Unknown");
        }
        serial_write_string("\r\nRIP: 0x");
        char buf[64];
        uint_to_hex(regs->rip, buf);
        serial_write_string(buf);
        {
            /* in which file: "libfoo.so+0x1234" (a file offset, what
               objdump/addr2line take) and the probable callers --
               crashes in shared libraries are otherwise just numbers */
            Thread *ct = sched_current();
            if (ct && ct->proc) {
                serial_write_string("  in ");
                if (!vma_print_addr(ct->proc, regs->rip)) serial_write_string("?");
                serial_write_string("\r\n");
                vma_print_stack(ct->proc, regs->rsp);
            }
        }
        serial_write_string("\r\n");

        serial_write_string("RSP: 0x");
        uint_to_hex(regs->rsp, buf);
        serial_write_string(buf);
        serial_write_string("\r\n");

        serial_write_string("RBP: 0x");
        uint_to_hex(regs->rbp, buf);
        serial_write_string(buf);
        serial_write_string("\r\n");

        serial_write_string("RAX: 0x");
        uint_to_hex(regs->rax, buf);
        serial_write_string(buf);
        serial_write_string("  RBX: 0x");
        uint_to_hex(regs->rbx, buf);
        serial_write_string(buf);
        serial_write_string("\r\n");

        serial_write_string("RCX: 0x");
        uint_to_hex(regs->rcx, buf);
        serial_write_string(buf);
        serial_write_string("  RDX: 0x");
        uint_to_hex(regs->rdx, buf);
        serial_write_string(buf);
        serial_write_string("\r\n");

        serial_write_string("RSI: 0x");
        uint_to_hex(regs->rsi, buf);
        serial_write_string(buf);
        serial_write_string("  RDI: 0x");
        uint_to_hex(regs->rdi, buf);
        serial_write_string(buf);
        serial_write_string("\r\n");
        
        if (regs->int_no == 14) {
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            serial_write_string("CR2: 0x");
            uint_to_hex(cr2, buf);
            serial_write_string(buf);
            /* what the CPU said (bit 0 present, 1 write, 2 user, 4 fetch)
               and what the page table holds there */
            serial_write_string("  error ");
            uint_to_hex(regs->err_code, buf);
            serial_write_string(buf);
            serial_write_string("  entry ");
            uint_to_hex(vmm_get_pte_raw(vmm_get_current_pml4(), cr2), buf);
            serial_write_string(buf);
            serial_write_string("\r\n");

            serial_write_string("User Stack Dump:\r\n");
            /* through the page tables, present pages only: a stack page in
               swap (or gone) must not fault the kernel in its own report */
            PageTable *upml4 = vmm_get_current_pml4();
            for (int i = 0; i < 8; i++) {
                uint64_t va = regs->rsp + (uint64_t)i * 8;
                uint64_t pte = (va & 7) || va >= 0x0000800000000000ULL ? 0 : vmm_get_pte(upml4, va);
                if (!(pte & PAGE_PRESENT) || !(pte & PAGE_USER)) break;
                uint64_t val = *(uint64_t *)P2V((pte & PAGE_ADDR_MASK) | (va & 0xFFF));
                serial_write_string("  +");
                char offset_buf[16];
                extern void uint_to_str(uint64_t val, char *buf);
                uint_to_str(i * 8, offset_buf); serial_write_string(offset_buf);
                serial_write_string(": ");
                uint_to_hex(val, buf); serial_write_string(buf);
                serial_write_string("\r\n");
            }
        }
        serial_write_string("Terminating user thread...\r\n");
        serial_write_string("======================================\r\n");

        if (g_boot_info) {
            console_print_string(g_boot_info, "\n[Process Crash] Exception ", 0x00FF3333, 0x000F0F1A);
            if (regs->int_no < 32) {
                console_print_string(g_boot_info, exception_messages[regs->int_no], 0x00FF3333, 0x000F0F1A);
            }
            console_print_string(g_boot_info, " at RIP: 0x", 0x00FFFFFF, 0x000F0F1A);
            console_print_string(g_boot_info, buf, 0x00FFFFFF, 0x000F0F1A);
            console_print_string(g_boot_info, ". Thread terminated.\n", 0x00FFFFFF, 0x000F0F1A);
        }

        /* The whole process dies of the matching signal, as on Linux:
           only this thread used to end, with a normal exit status, while
           the process's other threads ran on. (A user SIGSEGV handler is
           not run here yet.) */
        {
            int sig = 11;                                        /* SIGSEGV */
            if (regs->int_no == 0 || regs->int_no == 16 || regs->int_no == 19) sig = 8;  /* SIGFPE */
            else if (regs->int_no == 6) sig = 4;                 /* SIGILL */
            else if (regs->int_no == 3 || regs->int_no == 1) sig = 5;  /* SIGTRAP */
            else if (regs->int_no == 17) sig = 7;                /* SIGBUS */
            Thread *ct = sched_current();
            if (ct && ct->proc) {
                signal_kill_process(ct->proc, sig);
                sched_schedule();
                while (1) __asm__ volatile("cli; hlt");
            }
        }
        extern void thread_exit(void);
        thread_exit();
        return;
    }

    /* the boot log says how this boot ended (kernel/bootlog.c): the loader
       of the next one saves it to the boot stick; and the panic screen is
       drawn even over a program's framebuffer (wynvt, classic Zerp) */
    {
        extern void bootlog_set_state(uint32_t state);
        extern bool g_quiet_console;
        bootlog_set_state(1);   /* BOOTLOG_PANIC */
        g_quiet_console = false;
    }

    /* Write emergency info to serial port COM1 first */
    serial_write_string("\r\n======================================\r\n");
    serial_write_string("!!! KERNEL PANIC: CPU EXCEPTION !!!\r\n");
    if (regs->int_no < 32) {
        serial_write_string("Type: ");
        serial_write_string(exception_messages[regs->int_no]);
        serial_write_string("\r\n");
    } else {
        serial_write_string("Type: Unknown Interrupt\r\n");
    }
    {   /* where: the core, its thread and process, the kernel lock, and
           the kernel stack's code addresses (probable return addresses) */
        extern void uint_to_str(uint64_t val, char *buf);
        extern volatile int32_t g_bkl_owner;
        char b[32];
        Thread *pt = sched_current();
        uint32_t cpu_id;
        __asm__ volatile("mov %%gs:32, %0" : "=r"(cpu_id));
        serial_write_string("CPU "); uint_to_str(cpu_id, b); serial_write_string(b);
        serial_write_string(", lock held by "); uint_to_str((uint64_t)(int64_t)g_bkl_owner, b); serial_write_string(b);
        if (pt) {
            serial_write_string(", thread "); uint_to_str(pt->id, b); serial_write_string(b);
            if (pt->proc) { serial_write_string(" of "); serial_write_string(pt->proc->exe_path); }
        }
        serial_write_string("\r\nkernel stack:");
        extern char __text_end[];
        uint64_t *sp = (uint64_t *)(uintptr_t)regs->rsp;
        for (int i = 0, shown = 0; i < 512 && shown < 24 && (regs->cs & 3) == 0 &&
                                     regs->rsp >= HHDM_BASE; i++) {
            uint64_t w = sp[i];
            if (w >= KERNEL_VMA + KERNEL_PHYS && w < (uint64_t)(uintptr_t)__text_end) {
                extern void uint_to_hex(uint64_t val, char *buf);
                uint_to_hex(w, b);
                serial_write_string(" ");
                serial_write_string(b);
                shown++;
            }
        }
        serial_write_string("\r\n");
    }
    serial_write_string("======================================\r\n");

    /* Render visual panic screen (red background box) if framebuffer is available */
    if (g_boot_info) {
        uint32_t bg_color = 0x007f0000; /* Red alert background */

        /* Draw a red panic box in the terminal console */
        for (uint32_t y = g_boot_info->fb_height / 4; y < g_boot_info->fb_height * 3 / 4; y++) {
            uint32_t *row_ptr = (uint32_t *)((uint8_t *)P2V(g_boot_info->fb_addr) + y * g_boot_info->fb_pitch);
            for (uint32_t x = g_boot_info->fb_width / 8; x < g_boot_info->fb_width * 7 / 8; x++) {
                row_ptr[x] = bg_color;
            }
        }

        /* Reposition console coordinates inside the panic box */
        /* Save and hack cursor positions to draw directly */
        extern uint32_t console_start_x;
        extern uint32_t console_start_y;
        extern uint32_t console_end_x;
        extern uint32_t console_end_y;
        extern uint32_t cursor_x;
        extern uint32_t cursor_y;

        console_start_x = g_boot_info->fb_width / 8 + 20;
        console_start_y = g_boot_info->fb_height / 4 + 20;
        console_end_x = g_boot_info->fb_width * 7 / 8 - 20;
        console_end_y = g_boot_info->fb_height * 3 / 4 - 20;
        cursor_x = 0;
        cursor_y = 0;

        console_print_string(g_boot_info, "====================================================\n", 0x00FFFFFF, bg_color);
        console_print_string(g_boot_info, "                  !!! KERNEL PANIC !!!              \n", 0x00FF0000, bg_color);
        console_print_string(g_boot_info, "====================================================\n", 0x00FFFFFF, bg_color);
        
        console_print_string(g_boot_info, "Exception: ", 0x00FFFFFF, bg_color);
        const char *msg = "Unknown Exception";
        switch (regs->int_no) {
            case 0: msg = "Division By Zero (#DE)"; break;
            case 1: msg = "Debug (#DB)"; break;
            case 2: msg = "Non Maskable Interrupt"; break;
            case 3: msg = "Breakpoint (#BP)"; break;
            case 4: msg = "Overflow (#OF)"; break;
            case 5: msg = "Out of Bounds (#BR)"; break;
            case 6: msg = "Invalid Opcode (#UD)"; break;
            case 7: msg = "No Coprocessor (#NM)"; break;
            case 8: msg = "Double Fault (#DF)"; break;
            case 9: msg = "Coprocessor Segment Overrun"; break;
            case 10: msg = "Bad TSS (#TS)"; break;
            case 11: msg = "Segment Not Present (#NP)"; break;
            case 12: msg = "Stack Fault (#SS)"; break;
            case 13: msg = "General Protection Fault (#GP)"; break;
            case 14: msg = "Page Fault (#PF)"; break;
            case 16: msg = "Coprocessor Fault"; break;
            case 17: msg = "Alignment Check (#AC)"; break;
            case 18: msg = "Machine Check (#MC)"; break;
            case 19: msg = "SIMD Floating-Point Exception (#XM)"; break;
            case 20: msg = "Virtualization Exception (#VE)"; break;
            case 21: msg = "Control Protection Exception (#CP)"; break;
        }
        console_print_string(g_boot_info, msg, 0x00FF3333, bg_color);
        console_print_string(g_boot_info, "\n\nRegisters state:\n", 0x00E0E0E0, bg_color);

        if (regs->int_no == 14) { /* Page Fault */
            uint64_t cr2;
            __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
            print_reg(g_boot_info, "CR2", cr2);

            /* Dump page tables for CR2 */
            extern PageTable *vmm_get_current_pml4(void);
            PageTable *pml4 = vmm_get_current_pml4();
            uint64_t pml4_idx = PML4_INDEX(cr2);
            uint64_t pdpt_idx = PDPT_INDEX(cr2);
            uint64_t pd_idx   = PD_INDEX(cr2);
            uint64_t pt_idx   = PT_INDEX(cr2);

            PageTableEntry pml4_e = pml4->entries[pml4_idx];
            print_reg(g_boot_info, "PML4E", pml4_e);
            if (pml4_e & PAGE_PRESENT) {
                PageTable *pdpt = (PageTable *)P2V(pml4_e & PAGE_ADDR_MASK);
                PageTableEntry pdpt_e = pdpt->entries[pdpt_idx];
                print_reg(g_boot_info, "PDPTE", pdpt_e);
                if (pdpt_e & PAGE_PRESENT) {
                    PageTable *pd = (PageTable *)P2V(pdpt_e & PAGE_ADDR_MASK);
                    PageTableEntry pd_e = pd->entries[pd_idx];
                    print_reg(g_boot_info, "PDE  ", pd_e);
                    if (pd_e & PAGE_PRESENT) {
                        PageTable *pt = (PageTable *)P2V(pd_e & PAGE_ADDR_MASK);
                        PageTableEntry pt_e = pt->entries[pt_idx];
                        print_reg(g_boot_info, "PTE  ", pt_e);
                    }
                }
            }
        }

        print_reg(g_boot_info, "RIP", regs->rip);
        print_reg(g_boot_info, "CS ", regs->cs);
        print_reg(g_boot_info, "RSP", regs->rsp);
        print_reg(g_boot_info, "SS ", regs->ss);
        print_reg(g_boot_info, "ERR", regs->err_code);
        print_reg(g_boot_info, "RAX", regs->rax);
        print_reg(g_boot_info, "RBX", regs->rbx);
        print_reg(g_boot_info, "RCX", regs->rcx);
        print_reg(g_boot_info, "RDX", regs->rdx);
        print_reg(g_boot_info, "RDI", regs->rdi);
        print_reg(g_boot_info, "RSI", regs->rsi);
        print_reg(g_boot_info, "RBP", regs->rbp);
        print_reg(g_boot_info, "RFL", regs->rflags);
        
        console_print_string(g_boot_info, "\nRestarting in 10 s: the boot stick gets this log (\\logs).", 0x00FFAAAA, bg_color);
    }

    /* A warm reset after a while to read the screen: the CPU only, RAM
       keeps the boot log for the next boot's loader. The keyboard
       controller's reset line, then port CF9's soft reset (not 0x06, a
       full reset that may clear the memory), then a triple fault. */
    serial_write_string("Restarting (warm) in 10 s: the next boot's loader saves this log.\r\n");
    {
        extern void serial_flush_sync(void);
        serial_flush_sync();                             /* the serial ring's rest */
        extern void netlog_panic_flush(void);
        netlog_panic_flush();                            /* boot.cfg netlog=: the rest of the log */
        extern void sticklog_panic_flush(void);
        sticklog_panic_flush();                          /* and onto a live stick */
    }
    __asm__ volatile("cli");
    {
        /* 10 s by the TSC clock (kernel/sched.c: it runs with interrupts
           off); before it is calibrated it stands still -- then a bounded
           count of port writes (~1 us each on hardware, longer in a VM) */
        extern uint64_t sched_clock_ns(void);
        uint64_t t0 = sched_clock_ns();
        for (uint32_t i = 0; i < 10u * 1000u * 1000u && sched_clock_ns() - t0 < 10000000000ULL; i++)
            __asm__ volatile("outb %%al, $0x80" :: "a"((uint8_t)0) : "memory");
    }
    for (int i = 0; i < 100000; i++) {                   /* the controller's input buffer empty */
        uint8_t st;
        __asm__ volatile("inb $0x64, %0" : "=a"(st));
        if (!(st & 0x02)) break;
    }
    __asm__ volatile("outb %%al, $0x64" :: "a"((uint8_t)0xFE));
    for (uint32_t i = 0; i < 100000u; i++) __asm__ volatile("outb %%al, $0x80" :: "a"((uint8_t)0));
    __asm__ volatile("outb %%al, %%dx" :: "a"((uint8_t)0x04), "d"((uint16_t)0xCF9));
    for (uint32_t i = 0; i < 100000u; i++) __asm__ volatile("outb %%al, $0x80" :: "a"((uint8_t)0));
    __asm__ volatile("lidt %0; int3" :: "m"((uint16_t[3]){0, 0, 0}));
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
