/*
 * WynlandOS - Boot Information Structure
 * Shared between the UEFI bootloader and the kernel.
 * The bootloader fills this structure, then passes a pointer to the kernel.
 */

#pragma once

#include <wynland/types.h>

/* UEFI Memory types (simplified) */
#define MEMORY_USABLE           1
#define MEMORY_RESERVED         2
#define MEMORY_ACPI_RECLAIMABLE 3
#define MEMORY_ACPI_NVS         4
#define MEMORY_BAD              5
#define MEMORY_BOOTLOADER       6
#define MEMORY_KERNEL           7
#define MEMORY_FRAMEBUFFER      8

/* A single memory region descriptor */
typedef struct PACKED {
    uint64_t base;      /* Physical base address */
    uint64_t size;      /* Size in bytes */
    uint32_t type;      /* MEMORY_* type */
    uint32_t reserved;  /* Padding for alignment */
} MemoryRegion;

/* Framebuffer pixel format */
typedef enum {
    PIXEL_FORMAT_BGRA = 0,  /* Blue-Green-Red-Alpha (most common) */
    PIXEL_FORMAT_RGBA = 1,  /* Red-Green-Blue-Alpha */
} PixelFormat;

/* Information passed from bootloader to kernel */
typedef struct PACKED {
    /* === Framebuffer === */
    uint64_t    fb_addr;        /* Physical address of the framebuffer */
    uint32_t    fb_width;       /* Horizontal resolution in pixels */
    uint32_t    fb_height;      /* Vertical resolution in pixels */
    uint32_t    fb_pitch;       /* Bytes per scanline */
    uint32_t    fb_bpp;         /* Bits per pixel (usually 32) */
    uint32_t    fb_format;      /* Pixel format */

    /* === Memory Map === */
    uint64_t    mmap_addr;      /* Pointer to array of MemoryRegion */
    uint32_t    mmap_entries;   /* Number of entries in the memory map */
    uint32_t    mmap_reserved;  /* Padding */
    uint64_t    total_memory;   /* Total usable memory in bytes */

    /* === ACPI === */
    uint64_t    rsdp_addr;      /* Physical address of ACPI RSDP table */

    /* === Kernel === */
    uint64_t    kernel_phys;    /* Physical address where kernel was loaded */
    uint64_t    kernel_size;    /* Size of kernel image in bytes */

    /* === Magic === */
    uint64_t    magic;          /* WYNLAND_BOOT_MAGIC - for validation */
} BootInfo;

#define WYNLAND_BOOT_MAGIC 0x57594E4C414E4400ULL  /* "WYNLAND\0" as uint64 */
