/*
 * WynlandOS - ELF64 Executable Loader Implementation
 */
#include <wynland/elf.h>
#include <wynland/vfs.h>
#include <wynland/pmm.h>
#include <wynland/vmm.h>
#include <wynland/heap.h>

#define EI_MAG0        0
#define EI_MAG1        1
#define EI_MAG2        2
#define EI_MAG3        3
#define EI_CLASS       4
#define ELFCLASS64     2
#define EM_X86_64      62

#define PT_LOAD        1

typedef struct {
    unsigned char e_ident[16];
    uint16_t      e_type;
    uint16_t      e_machine;
    uint32_t      e_version;
    uint64_t      e_entry;
    uint64_t      e_phoff;
    uint64_t      e_shoff;
    uint32_t      e_flags;
    uint16_t      e_ehsize;
    uint16_t      e_phentsize;
    uint16_t      e_phnum;
    uint16_t      e_shentsize;
    uint16_t      e_shnum;
    uint16_t      e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t   p_type;
    uint32_t   p_flags;
    uint64_t   p_offset;
    uint64_t   p_vaddr;
    uint64_t   p_paddr;
    uint64_t   p_filesz;
    uint64_t   p_memsz;
    uint64_t   p_align;
} Elf64_Phdr;

extern void serial_write_string(const char *str);
extern void *memset(void *s, int c, size_t n);

static void *find_mapped_page(LoadedPages *pages, uint64_t virt) {
    for (int i = 0; i < pages->count; i++) {
        if (pages->virt_addrs[i] == virt) {
            return pages->phys_pages[i];
        }
    }
    return NULL;
}

bool elf_load(const char *path, uint64_t *out_entry, uint64_t *out_stack_top, PageTable *pml4, LoadedPages *out_pages)
{
    out_pages->count = 0;

    VfsFile *f = vfs_open(path);
    if (!f) {
        serial_write_string("ELF Loader Error: File not found: ");
        serial_write_string(path);
        serial_write_string("\r\n");
        return false;
    }

    /* 1. Read Elf64 Header */
    Elf64_Ehdr hdr;
    if (vfs_read(f, &hdr, sizeof(Elf64_Ehdr)) != sizeof(Elf64_Ehdr)) {
        serial_write_string("ELF Loader Error: Failed to read ELF header.\r\n");
        vfs_close(f);
        return false;
    }

    /* 2. Validate ELF Header */
    if (hdr.e_ident[EI_MAG0] != 0x7F ||
        hdr.e_ident[EI_MAG1] != 'E'  ||
        hdr.e_ident[EI_MAG2] != 'L'  ||
        hdr.e_ident[EI_MAG3] != 'F') {
        serial_write_string("ELF Loader Error: Invalid ELF magic.\r\n");
        vfs_close(f);
        return false;
    }

    if (hdr.e_ident[EI_CLASS] != ELFCLASS64) {
        serial_write_string("ELF Loader Error: Not a 64-bit ELF.\r\n");
        vfs_close(f);
        return false;
    }

    if (hdr.e_machine != EM_X86_64) {
        serial_write_string("ELF Loader Error: Not an x86_64 ELF.\r\n");
        vfs_close(f);
        return false;
    }

    /* 3. Read and load segments */
    for (uint16_t i = 0; i < hdr.e_phnum; i++) {
        Elf64_Phdr phdr;
        uint64_t phdr_offset = hdr.e_phoff + i * hdr.e_phentsize;
        vfs_seek(f, phdr_offset, VFS_SEEK_SET);
        if (vfs_read(f, &phdr, sizeof(Elf64_Phdr)) != sizeof(Elf64_Phdr)) {
            serial_write_string("ELF Loader Error: Failed to read program header.\r\n");
            goto error_cleanup;
        }

        if (phdr.p_type != PT_LOAD) {
            continue; /* Skip non-loadable segments */
        }

        /* Align to page boundaries */
        uint64_t start_addr = phdr.p_vaddr & ~(PAGE_SIZE - 1);
        uint64_t end_addr = (phdr.p_vaddr + phdr.p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        for (uint64_t addr = start_addr; addr < end_addr; addr += PAGE_SIZE) {
            void *phys = find_mapped_page(out_pages, addr);
            if (!phys) {
                phys = pmm_alloc_page();
                if (!phys) {
                    serial_write_string("ELF Loader Error: Out of physical memory for segment load.\r\n");
                    goto error_cleanup;
                }

                if (out_pages->count >= MAX_LOADED_PAGES) {
                    serial_write_string("ELF Loader Error: Maximum loaded pages exceeded.\r\n");
                    pmm_free_page(phys);
                    goto error_cleanup;
                }

                out_pages->phys_pages[out_pages->count] = phys;
                out_pages->virt_addrs[out_pages->count] = addr;
                out_pages->count++;

                vmm_map_page(pml4, addr, (uint64_t)(uintptr_t)phys, PAGE_WRITE | PAGE_USER);
                memset((void *)addr, 0, PAGE_SIZE);
            }

            /* Calculate portion of file to read into this page */
            uint64_t page_start = addr;
            uint64_t page_end = addr + PAGE_SIZE;

            uint64_t file_start = phdr.p_vaddr;
            uint64_t file_end = phdr.p_vaddr + phdr.p_filesz;

            uint64_t overlap_start = (page_start > file_start) ? page_start : file_start;
            uint64_t overlap_end = (page_end < file_end) ? page_end : file_end;

            if (overlap_start < overlap_end) {
                uint64_t read_len = overlap_end - overlap_start;
                uint64_t file_off = phdr.p_offset + (overlap_start - phdr.p_vaddr);

                vfs_seek(f, file_off, VFS_SEEK_SET);
                int bytes = vfs_read(f, (void *)overlap_start, read_len);
                if (bytes != (int)read_len) {
                    serial_write_string("ELF Loader Error: Segment read mismatch.\r\n");
                    goto error_cleanup;
                }
            }
        }
    }

    vfs_close(f);

    /* 4. Map user stack (64 KB) at 0x50000000 */
    uint64_t stack_base = 0x50000000;
    uint64_t stack_size = 65536;
    for (uint64_t offset = 0; offset < stack_size; offset += PAGE_SIZE) {
        uint64_t addr = stack_base + offset;
        void *phys = pmm_alloc_page();
        if (!phys) {
            serial_write_string("ELF Loader Error: Out of physical memory for stack allocation.\r\n");
            goto error_cleanup_no_file;
        }

        if (out_pages->count >= MAX_LOADED_PAGES) {
            serial_write_string("ELF Loader Error: Maximum loaded pages exceeded (stack).\r\n");
            pmm_free_page(phys);
            goto error_cleanup_no_file;
        }

        out_pages->phys_pages[out_pages->count] = phys;
        out_pages->virt_addrs[out_pages->count] = addr;
        out_pages->count++;

        vmm_map_page(pml4, addr, (uint64_t)(uintptr_t)phys, PAGE_WRITE | PAGE_USER);
        memset((void *)addr, 0, PAGE_SIZE);
    }

    *out_entry = hdr.e_entry;
    *out_stack_top = stack_base + stack_size;
    return true;

error_cleanup:
    vfs_close(f);

error_cleanup_no_file:
    /* Clean up all pages mapped so far */
    for (int i = 0; i < out_pages->count; i++) {
        vmm_unmap_page(pml4, out_pages->virt_addrs[i]);
        pmm_free_page(out_pages->phys_pages[i]);
    }
    out_pages->count = 0;
    return false;
}
