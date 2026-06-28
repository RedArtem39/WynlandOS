/*
 * WynlandOS - Virtual Memory Manager (VMM) Implementation
 */

#include <wynland/vmm.h>
#include <wynland/pmm.h>
#include <wynland/boot_info.h>

extern uint8_t __kernel_end[];
extern void *memset(void *s, int c, size_t n);
extern void serial_write_string(const char *str);

/* Helper to allocate and zero a new page table */
static PageTable *vmm_alloc_table(void)
{
    void *phys = pmm_alloc_page();
    if (!phys) {
        serial_write_string("VMM Error: Out of physical memory for page table!\r\n");
        return NULL;
    }
    memset(phys, 0, PAGE_SIZE);
    return (PageTable *)phys;
}

void vmm_map_page(PageTable *pml4, uint64_t virt, uint64_t phys, uint64_t flags)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));

    /* Round addresses to page boundaries */
    virt &= ~(PAGE_SIZE - 1);
    phys &= ~(PAGE_SIZE - 1);

    uint64_t pml4_idx = PML4_INDEX(virt);
    uint64_t pdpt_idx = PDPT_INDEX(virt);
    uint64_t pd_idx   = PD_INDEX(virt);
    uint64_t pt_idx   = PT_INDEX(virt);

    /* 1. Traverse PML4 -> PDPT */
    PageTableEntry *pml4_entry = &pml4->entries[pml4_idx];
    PageTable *pdpt = NULL;
    if (!(*pml4_entry & PAGE_PRESENT)) {
        pdpt = vmm_alloc_table();
        *pml4_entry = (uint64_t)(uintptr_t)pdpt | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    } else {
        pdpt = (PageTable *)(uintptr_t)(*pml4_entry & PAGE_ADDR_MASK);
    }

    /* 2. Traverse PDPT -> PD */
    PageTableEntry *pdpt_entry = &pdpt->entries[pdpt_idx];
    PageTable *pd = NULL;
    if (!(*pdpt_entry & PAGE_PRESENT)) {
        pd = vmm_alloc_table();
        *pdpt_entry = (uint64_t)(uintptr_t)pd | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    } else {
        pd = (PageTable *)(uintptr_t)(*pdpt_entry & PAGE_ADDR_MASK);
    }

    /* 3. Traverse PD -> PT */
    PageTableEntry *pd_entry = &pd->entries[pd_idx];
    PageTable *pt = NULL;
    if (!(*pd_entry & PAGE_PRESENT)) {
        pt = vmm_alloc_table();
        *pd_entry = (uint64_t)(uintptr_t)pt | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    } else {
        pt = (PageTable *)(uintptr_t)(*pd_entry & PAGE_ADDR_MASK);
    }

    /* 4. Map the physical address in the Page Table */
    pt->entries[pt_idx] = phys | flags | PAGE_PRESENT;

    /*
    if (virt >= 0x10000000 && virt < 0x11000000) {
        serial_write_string("VMM Map: ");
        char buf[32];
        extern void uint_to_hex(uint64_t val, char *buf);
        extern void uint_to_str(uint64_t val, char *buf);
        uint_to_hex(virt, buf); serial_write_string(buf); serial_write_string(" -> ");
        uint_to_hex(phys, buf); serial_write_string(buf); serial_write_string(" (pt=");
        uint_to_hex((uint64_t)(uintptr_t)pt, buf); serial_write_string(buf); serial_write_string(" idx=");
        uint_to_str(pt_idx, buf); serial_write_string(buf); serial_write_string(")\r\n");
    }
    */

    /* 5. Invalidate TLB for this virtual address */
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");

    if (rflags & 0x200) {
        __asm__ volatile("sti");
    }
}

void vmm_unmap_page(PageTable *pml4, uint64_t virt)
{
    uint64_t rflags;
    __asm__ volatile("pushfq; pop %0; cli" : "=r"(rflags));

    virt &= ~(PAGE_SIZE - 1);
    uint64_t pml4_idx = PML4_INDEX(virt);
    uint64_t pdpt_idx = PDPT_INDEX(virt);
    uint64_t pd_idx   = PD_INDEX(virt);
    uint64_t pt_idx   = PT_INDEX(virt);

    PageTableEntry *pml4_entry = &pml4->entries[pml4_idx];
    if (!(*pml4_entry & PAGE_PRESENT)) goto cleanup;
    
    PageTable *pdpt = (PageTable *)(uintptr_t)(*pml4_entry & PAGE_ADDR_MASK);
    PageTableEntry *pdpt_entry = &pdpt->entries[pdpt_idx];
    if (!(*pdpt_entry & PAGE_PRESENT)) goto cleanup;

    PageTable *pd = (PageTable *)(uintptr_t)(*pdpt_entry & PAGE_ADDR_MASK);
    PageTableEntry *pd_entry = &pd->entries[pd_idx];
    if (!(*pd_entry & PAGE_PRESENT)) goto cleanup;

    PageTable *pt = (PageTable *)(uintptr_t)(*pd_entry & PAGE_ADDR_MASK);
    
    /* Clear entry */
    pt->entries[pt_idx] = 0;

    /* Invalidate TLB */
    __asm__ volatile("invlpg (%0)" :: "r"(virt) : "memory");

cleanup:
    if (rflags & 0x200) {
        __asm__ volatile("sti");
    }
}

uint64_t vmm_get_phys(PageTable *pml4, uint64_t virt)
{
    uint64_t pml4_idx = PML4_INDEX(virt);
    uint64_t pdpt_idx = PDPT_INDEX(virt);
    uint64_t pd_idx   = PD_INDEX(virt);
    uint64_t pt_idx   = PT_INDEX(virt);

    PageTableEntry *pml4_entry = &pml4->entries[pml4_idx];
    if (!(*pml4_entry & PAGE_PRESENT)) return 0;
    
    PageTable *pdpt = (PageTable *)(uintptr_t)(*pml4_entry & PAGE_ADDR_MASK);
    PageTableEntry *pdpt_entry = &pdpt->entries[pdpt_idx];
    if (!(*pdpt_entry & PAGE_PRESENT)) return 0;

    PageTable *pd = (PageTable *)(uintptr_t)(*pdpt_entry & PAGE_ADDR_MASK);
    PageTableEntry *pd_entry = &pd->entries[pd_idx];
    if (!(*pd_entry & PAGE_PRESENT)) return 0;

    PageTable *pt = (PageTable *)(uintptr_t)(*pd_entry & PAGE_ADDR_MASK);
    PageTableEntry *pt_entry = &pt->entries[pt_idx];
    if (!(*pt_entry & PAGE_PRESENT)) return 0;

    return (*pt_entry & PAGE_ADDR_MASK) | (virt & (PAGE_SIZE - 1));
}

static inline uint64_t read_msr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

static inline void write_msr(uint32_t msr, uint64_t val) {
    uint32_t low = (uint32_t)val;
    uint32_t high = (uint32_t)(val >> 32);
    __asm__ volatile("wrmsr" :: "a"(low), "d"(high), "c"(msr));
}

void vmm_init(BootInfo *boot_info)
{
    serial_write_string("VMM: Initializing paging...\r\n");

    /* Allocate and zero the root PML4 table */
    PageTable *pml4 = vmm_alloc_table();
    if (!pml4) {
        serial_write_string("VMM Error: Failed to allocate PML4 table!\r\n");
        return;
    }

    /* 1. Identity map the kernel code & data [0x100000, __kernel_end] */
    uint64_t kernel_start = 0x100000;
    uint64_t kernel_end = ((uint64_t)__kernel_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    for (uint64_t addr = kernel_start; addr < kernel_end; addr += PAGE_SIZE) {
        vmm_map_page(pml4, addr, addr, PAGE_WRITE);
    }

    /* 2. Identity map all memory regions from UEFI memory map */
    MemoryRegion *regions = (MemoryRegion *)(uintptr_t)boot_info->mmap_addr;
    uint32_t region_count = boot_info->mmap_entries;

    for (uint32_t i = 0; i < region_count; i++) {
        /* Skip reserved and bad memory to avoid mapping unbacked/MMIO holes */
        if (regions[i].type == MEMORY_RESERVED || regions[i].type == MEMORY_BAD) {
            continue;
        }

        uint64_t base = regions[i].base;
        uint64_t size = regions[i].size;

        uint64_t flags = PAGE_WRITE;
        if (regions[i].type != MEMORY_USABLE) {
            flags |= PAGE_NX; /* Mark non-usable memory as No-Execute */
        }

        for (uint64_t offset = 0; offset < size; offset += PAGE_SIZE) {
            uint64_t addr = base + offset;
            /* Skip only the pages that fall within the kernel boundaries (already mapped) */
            if (addr >= kernel_start && addr < kernel_end) {
                continue;
            }
            vmm_map_page(pml4, addr, addr, flags);
        }
    }

    /* Configure PAT4 as Write-Combining (01h) in PAT MSR 0x277 */
    uint64_t pat = read_msr(0x277);
    pat &= ~(0xFFULL << 32); /* Clear PAT4 (bits 32..39) */
    pat |= (0x01ULL << 32);  /* Set PAT4 to WC (01h) */
    write_msr(0x277, pat);

    /* 3. Identity map the Graphics Output Protocol (GOP) Framebuffer */
    uint64_t fb_start = boot_info->fb_addr;
    uint64_t fb_size = boot_info->fb_pitch * boot_info->fb_height;
    fb_size = (fb_size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

    for (uint64_t offset = 0; offset < fb_size; offset += PAGE_SIZE) {
        vmm_map_page(pml4, fb_start + offset, fb_start + offset, PAGE_WRITE | PAGE_NX | PAGE_PAT);
    }

    /* 4. Switch to our new PML4 page table by loading it into CR3 */
    uint64_t pml4_phys = (uint64_t)(uintptr_t)pml4;
    __asm__ volatile("mov %0, %%cr3" :: "r"(pml4_phys) : "memory");

    serial_write_string("VMM: Switched to new page tables successfully!\r\n");
}

PageTable *vmm_get_current_pml4(void)
{
    uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    return (PageTable *)(uintptr_t)cr3;
}

void vmm_map_mmio(uint64_t phys_addr, uint64_t size)
{
    PageTable *pml4 = vmm_get_current_pml4();
    uint64_t start = phys_addr & ~(PAGE_SIZE - 1);
    uint64_t end = (phys_addr + size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    
    for (uint64_t addr = start; addr < end; addr += PAGE_SIZE) {
        vmm_map_page(pml4, addr, addr, PAGE_WRITE | PAGE_NX | PAGE_CACHE_DISABLE | PAGE_WRITE_THROUGH);
    }
}

