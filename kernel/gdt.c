#include <wynland/types.h>
#include <wynland/heap.h>
#include <wynland/sched.h>

// GDT Entry structures
typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
} __attribute__((packed)) GdtEntry;

typedef struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_middle;
    uint8_t access;
    uint8_t granularity;
    uint8_t base_high;
    uint32_t base_highest;
    uint32_t reserved;
} __attribute__((packed)) GdtTssEntry;

typedef struct {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed)) GdtPtr;

// TSS structure (x86_64)
typedef struct {
    uint32_t reserved0;
    uint64_t rsp0;      // Ring 0 stack pointer
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved1;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t io_map_base;
} __attribute__((packed)) Tss;

// Custom GDT backing store
static struct {
    uint64_t entries[5];
    GdtTssEntry tss_entry;
} __attribute__((packed)) g_gdt;

static GdtPtr g_gdt_ptr;
static Tss g_tss;

uint64_t current_kernel_stack = 0;
uint64_t gdt_original_tss_rsp0 = 0;

void gdt_flush(uint64_t gdt_ptr_addr);
void serial_write_string(const char *str);

static void gdt_set_tss_entry(uint64_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    g_gdt.tss_entry.limit_low = (limit & 0xFFFF);
    g_gdt.tss_entry.base_low = (base & 0xFFFF);
    g_gdt.tss_entry.base_middle = (base >> 16) & 0xFF;
    g_gdt.tss_entry.access = access;
    g_gdt.tss_entry.granularity = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    g_gdt.tss_entry.base_high = (base >> 24) & 0xFF;
    g_gdt.tss_entry.base_highest = (base >> 32) & 0xFFFFFFFF;
    g_gdt.tss_entry.reserved = 0;
}

void gdt_init(void) {
    serial_write_string("GDT: Re-initializing GDT with TSS Support...\r\n");

    // Load exact descriptor values from the stable entry GDT
    g_gdt.entries[0] = 0x0000000000000000ULL; // Null descriptor
    g_gdt.entries[1] = 0x002F9A000000FFFFULL; // Kernel Code (Selector 0x08)
    g_gdt.entries[2] = 0x000F92000000FFFFULL; // Kernel Data (Selector 0x10)
    g_gdt.entries[3] = 0x000FF2000000FFFFULL; // User Data (Selector 0x18)
    g_gdt.entries[4] = 0x002FFA000000FFFFULL; // User Code (Selector 0x20)

    // 6. Initialize TSS
    // Clear structure
    for (size_t i = 0; i < sizeof(Tss); i++) {
        ((char*)&g_tss)[i] = 0;
    }
    
    // Allocate 16 KB kernel interrupt/syscall stack
    void *stack = kmalloc(16384);
    if (!stack) {
        serial_write_string("GDT Error: Failed to allocate kernel TSS stack!\r\n");
        return;
    }
    
    g_tss.rsp0 = (uint64_t)stack + 16384;
    gdt_original_tss_rsp0 = g_tss.rsp0;
    extern uint64_t current_kernel_stack;
    current_kernel_stack = g_tss.rsp0;
    g_tss.io_map_base = sizeof(Tss);

    // Setup TSS Segment descriptor in GDT
    gdt_set_tss_entry((uint64_t)&g_tss, sizeof(Tss) - 1, 0x89, 0x00);

    // Load GDT
    g_gdt_ptr.limit = sizeof(g_gdt) - 1;
    g_gdt_ptr.base = (uint64_t)&g_gdt;
    gdt_flush((uint64_t)&g_gdt_ptr);

    // Load Task Register (LTR) pointing to selector 0x28 (TSS)
    __asm__ volatile("ltr %0" :: "r"((uint16_t)0x28));

    serial_write_string("GDT: Successfully loaded GDT and TSS.\r\n");
}

void gdt_update_tss_rsp0(uint64_t rsp0) {
    g_tss.rsp0 = rsp0;
    current_kernel_stack = rsp0;
}
