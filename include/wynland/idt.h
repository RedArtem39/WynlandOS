/*
 * WynlandOS - Interrupt Descriptor Table (IDT) Header
 */

#pragma once

#include <wynland/types.h>
#include <wynland/boot_info.h>

#define IDT_ENTRIES 256

/* IDT entry (Gate Descriptor) structure for x86_64 (16 bytes) */
typedef struct PACKED {
    uint16_t offset_low;      /* Offset bits 0..15 */
    uint16_t selector;        /* Code segment selector (e.g. 0x08) */
    uint8_t  ist;             /* Interrupt Stack Table index (0..7) */
    uint8_t  type_attributes; /* Gate type, DPL, Present */
    uint16_t offset_middle;   /* Offset bits 16..31 */
    uint32_t offset_high;     /* Offset bits 32..63 */
    uint32_t reserved;        /* Reserved (must be 0) */
} IdtEntry;

/* IDTR register layout */
typedef struct PACKED {
    uint16_t limit;
    uint64_t base;
} IdtPtr;

/* Structure for exception/interrupt register state passed to C handler */
typedef struct PACKED {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no, err_code;
    uint64_t rip, cs, rflags, rsp, ss;
} InterruptRegisters;

/* Function to initialize IDT */
void idt_init(BootInfo *boot_info);

/* C handler for all exceptions and interrupts */
void exception_handler(InterruptRegisters *regs);
