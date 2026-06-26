/*
 * WynlandOS Kernel Linker Script
 * 
 * The kernel is loaded at physical address 0x100000 (1 MB).
 * The UEFI bootloader loads the ELF binary and jumps to _kernel_start.
 */

ENTRY(_kernel_start)

KERNEL_OFFSET = 0x100000;

SECTIONS
{
    . = KERNEL_OFFSET;

    /* Code section - entry point first */
    .text ALIGN(4K) : {
        KEEP(*(.text.entry))    /* Kernel entry point - must be first */
        *(.text .text.*)
    }

    /* Read-only data */
    .rodata ALIGN(4K) : {
        *(.rodata .rodata.*)
    }

    /* Initialized data */
    .data ALIGN(4K) : {
        *(.data .data.*)
    }

    /* Uninitialized data (BSS) */
    .bss ALIGN(4K) : {
        __bss_start = .;
        *(COMMON)
        *(.bss .bss.*)
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
