/*
 * WynlandOS - Interrupt Descriptor Table (IDT) Implementation
 */

#include <wynland/idt.h>
#include <wynland/boot_info.h>
#include <wynland/vmm.h>

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

    /* Load IDT register */
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

void exception_handler(InterruptRegisters *regs)
{
    /* Check if the exception happened in user mode (Ring 3) */
    if ((regs->cs & 0x03) == 3) {
        serial_write_string("\r\n======================================\r\n");
        serial_write_string("!!! USER MODE PROCESS CRASHED !!!\r\n");
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
            serial_write_string("\r\n");
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

        extern void thread_exit(void);
        thread_exit();
        return;
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
    serial_write_string("======================================\r\n");

    /* Render visual panic screen (red background box) if framebuffer is available */
    if (g_boot_info) {
        uint32_t bg_color = 0x007f0000; /* Red alert background */

        /* Draw a red panic box in the terminal console */
        for (uint32_t y = g_boot_info->fb_height / 4; y < g_boot_info->fb_height * 3 / 4; y++) {
            uint32_t *row_ptr = (uint32_t *)((uint8_t *)(uintptr_t)g_boot_info->fb_addr + y * g_boot_info->fb_pitch);
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
        if (regs->int_no < 32) {
            console_print_string(g_boot_info, exception_messages[regs->int_no], 0x00FF3333, bg_color);
        } else {
            console_print_string(g_boot_info, "Unknown", 0x00FF3333, bg_color);
        }
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
                PageTable *pdpt = (PageTable *)(uintptr_t)(pml4_e & PAGE_ADDR_MASK);
                PageTableEntry pdpt_e = pdpt->entries[pdpt_idx];
                print_reg(g_boot_info, "PDPTE", pdpt_e);
                if (pdpt_e & PAGE_PRESENT) {
                    PageTable *pd = (PageTable *)(uintptr_t)(pdpt_e & PAGE_ADDR_MASK);
                    PageTableEntry pd_e = pd->entries[pd_idx];
                    print_reg(g_boot_info, "PDE  ", pd_e);
                    if (pd_e & PAGE_PRESENT) {
                        PageTable *pt = (PageTable *)(uintptr_t)(pd_e & PAGE_ADDR_MASK);
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
        
        console_print_string(g_boot_info, "\nSystem Halted.", 0x00FFAAAA, bg_color);
    }

    /* Loop forever and halt */
    while (1) {
        __asm__ volatile("cli; hlt");
    }
}
