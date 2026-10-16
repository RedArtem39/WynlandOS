/*
 * WynlandOS Kernel Linker Script
 *
 * The kernel is loaded at physical address 0x100000 (1 MB) and runs at
 * KERNEL_VMA + that (include/wynland/memlayout.h): each section's load
 * address (AT) is its run address minus KERNEL_VMA. The UEFI bootloader
 * copies the PT_LOAD segments to their physical addresses, maps them high
 * and jumps to _kernel_start.
 */

ENTRY(_kernel_start)

KERNEL_VMA = 0xFFFFFFFF80000000;
KERNEL_PHYS = 0x100000;

SECTIONS
{
    . = KERNEL_VMA + KERNEL_PHYS;
    __kernel_start = .;

    /* Code section - entry point first */
    .text ALIGN(4K) : AT(ADDR(.text) - KERNEL_VMA) {
        KEEP(*(.text.entry))    /* Kernel entry point - must be first */
        *(.text .text.*)
    }
    __text_end = .;             /* kernel code ends: stack dumps pick return addresses by it */

    /* Read-only data */
    .rodata ALIGN(4K) : AT(ADDR(.rodata) - KERNEL_VMA) {
        *(.rodata .rodata.*)
        *(.lrodata .lrodata.*)
    }

    /* Initialized data */
    .data ALIGN(4K) : AT(ADDR(.data) - KERNEL_VMA) {
        *(.data .data.*)
        *(.ldata .ldata.*)
    }

    /* Uninitialized data (BSS) */
    .bss ALIGN(4K) : AT(ADDR(.bss) - KERNEL_VMA) {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
        *(.lbss .lbss.*)
        __bss_end = .;
    }

    /* End of kernel */
    __kernel_end = .;

    /* Discard unwanted sections */
    /DISCARD/ : {
        *(.comment)
        *(.note*)
        *(.eh_frame*)
    }
}
