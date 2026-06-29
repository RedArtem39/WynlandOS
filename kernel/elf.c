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
#define PT_INTERP      3
#define PT_PHDR        6

#define AT_NULL        0
#define AT_PHDR        3
#define AT_PHENT       4
#define AT_PHNUM       5
#define AT_PAGESZ      6
#define AT_BASE        7
#define AT_FLAGS       8
#define AT_ENTRY       9
#define AT_UID         11
#define AT_EUID        12
#define AT_GID         13
#define AT_EGID        14
#define AT_SECURE      23
#define AT_RANDOM      25

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

typedef struct {
    uint64_t a_type;
    uint64_t a_val;
} Elf64_Auxv;

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

static bool load_elf_segments(VfsFile *f, Elf64_Ehdr *hdr, uint64_t load_offset, PageTable *pml4, LoadedPages *out_pages)
{
    for (uint16_t i = 0; i < hdr->e_phnum; i++) {
        Elf64_Phdr phdr;
        uint64_t phdr_offset = hdr->e_phoff + i * hdr->e_phentsize;
        vfs_seek(f, phdr_offset, VFS_SEEK_SET);
        if (vfs_read(f, &phdr, sizeof(Elf64_Phdr)) != sizeof(Elf64_Phdr)) {
            serial_write_string("ELF Loader Error: Failed to read program header.\r\n");
            return false;
        }

        if (phdr.p_type != PT_LOAD) {
            continue; /* Skip non-loadable segments */
        }

        /* Align to page boundaries */
        uint64_t vaddr = phdr.p_vaddr + load_offset;
        uint64_t start_addr = vaddr & ~(PAGE_SIZE - 1);
        uint64_t end_addr = (vaddr + phdr.p_memsz + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

        for (uint64_t addr = start_addr; addr < end_addr; addr += PAGE_SIZE) {
            void *phys = find_mapped_page(out_pages, addr);
            if (!phys) {
                phys = pmm_alloc_page();
                if (!phys) {
                    serial_write_string("ELF Loader Error: Out of physical memory for segment load.\r\n");
                    return false;
                }

                if (out_pages->count >= MAX_LOADED_PAGES) {
                    serial_write_string("ELF Loader Error: Maximum loaded pages exceeded.\r\n");
                    pmm_free_page(phys);
                    return false;
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

            uint64_t file_start = vaddr;
            uint64_t file_end = vaddr + phdr.p_filesz;

            uint64_t overlap_start = (page_start > file_start) ? page_start : file_start;
            uint64_t overlap_end = (page_end < file_end) ? page_end : file_end;

            if (overlap_start < overlap_end) {
                uint64_t read_len = overlap_end - overlap_start;
                uint64_t file_off = phdr.p_offset + (overlap_start - vaddr);

                vfs_seek(f, file_off, VFS_SEEK_SET);
                int bytes = vfs_read(f, (void *)overlap_start, read_len);
                if (bytes != (int)read_len) {
                    serial_write_string("ELF Loader Error: Segment read mismatch.\r\n");
                    return false;
                }
            }
        }
    }
    return true;
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

    /* 3. Check for PT_INTERP (dynamic linker) and find PT_PHDR */
    char interp_path[256];
    bool has_interp = false;
    uint64_t phdr_vaddr = 0;

    for (uint16_t i = 0; i < hdr.e_phnum; i++) {
        Elf64_Phdr phdr;
        uint64_t phdr_offset = hdr.e_phoff + i * hdr.e_phentsize;
        vfs_seek(f, phdr_offset, VFS_SEEK_SET);
        if (vfs_read(f, &phdr, sizeof(Elf64_Phdr)) != sizeof(Elf64_Phdr)) {
            serial_write_string("ELF Loader Error: Failed to read program header.\r\n");
            vfs_close(f);
            return false;
        }

        if (phdr.p_type == PT_INTERP) {
            uint64_t len = phdr.p_filesz;
            if (len >= sizeof(interp_path)) len = sizeof(interp_path) - 1;
            vfs_seek(f, phdr.p_offset, VFS_SEEK_SET);
            if (vfs_read(f, interp_path, len) != (int)len) {
                serial_write_string("ELF Loader Error: Failed to read PT_INTERP path.\r\n");
                vfs_close(f);
                return false;
            }
            interp_path[len] = '\0';
            has_interp = true;
        } else if (phdr.p_type == PT_PHDR) {
            phdr_vaddr = phdr.p_vaddr;
        }
    }

    /* If no PT_PHDR was found, fallback: assume it is mapped at e_phoff in segment 0 */
    if (phdr_vaddr == 0) {
        for (uint16_t i = 0; i < hdr.e_phnum; i++) {
            Elf64_Phdr phdr;
            uint64_t phdr_offset = hdr.e_phoff + i * hdr.e_phentsize;
            vfs_seek(f, phdr_offset, VFS_SEEK_SET);
            if (vfs_read(f, &phdr, sizeof(Elf64_Phdr)) == sizeof(Elf64_Phdr)) {
                if (phdr.p_type == PT_LOAD && phdr.p_offset == 0) {
                    phdr_vaddr = phdr.p_vaddr + hdr.e_phoff;
                    break;
                }
            }
        }
    }

    /* Load main program segments (load_offset = 0) */
    if (!load_elf_segments(f, &hdr, 0, pml4, out_pages)) {
        vfs_close(f);
        goto error_cleanup_no_file;
    }
    vfs_close(f);

    /* 4. Load Interpreter if present */
    uint64_t interpreter_base = 0x700000000000;
    uint64_t entry_point = hdr.e_entry;

    if (has_interp) {
        VfsFile *interp_f = vfs_open(interp_path);
        if (!interp_f) {
            serial_write_string("ELF Loader Error: Dynamic linker not found: ");
            serial_write_string(interp_path);
            serial_write_string("\r\n");
            goto error_cleanup_no_file;
        }

        Elf64_Ehdr interp_hdr;
        if (vfs_read(interp_f, &interp_hdr, sizeof(Elf64_Ehdr)) != sizeof(Elf64_Ehdr)) {
            serial_write_string("ELF Loader Error: Failed to read interpreter ELF header.\r\n");
            vfs_close(interp_f);
            goto error_cleanup_no_file;
        }

        if (interp_hdr.e_ident[EI_MAG0] != 0x7F ||
            interp_hdr.e_ident[EI_MAG1] != 'E'  ||
            interp_hdr.e_ident[EI_MAG2] != 'L'  ||
            interp_hdr.e_ident[EI_MAG3] != 'F') {
            serial_write_string("ELF Loader Error: Invalid interpreter ELF magic.\r\n");
            vfs_close(interp_f);
            goto error_cleanup_no_file;
        }

        if (!load_elf_segments(interp_f, &interp_hdr, interpreter_base, pml4, out_pages)) {
            vfs_close(interp_f);
            goto error_cleanup_no_file;
        }

        entry_point = interpreter_base + interp_hdr.e_entry;
        vfs_close(interp_f);
    }

    /* 5. Map user stack (64 KB) at 0x50000000 */
    uint64_t stack_base = 0x50000000;
    uint64_t stack_size = 65536;
    for (uint64_t offset = 0; offset < stack_size; offset += PAGE_SIZE) {
        uint64_t addr = stack_base + offset;
        void *phys = find_mapped_page(out_pages, addr);
        if (!phys) {
            phys = pmm_alloc_page();
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
    }

    /* 6. Set up System V ABI stack layout */
    uint64_t stack_top = stack_base + stack_size;

    /* Write strings/data at the very top of stack */
    /* 16 random bytes */
    uint64_t random_addr = stack_top - 16;
    uint8_t *random_ptr = (uint8_t *)random_addr;
    for (int i = 0; i < 16; i++) random_ptr[i] = 0xAB; // pseudo-random bytes

    /* Program name string */
    uint32_t path_len = 0;
    while (path[path_len] != '\0') path_len++;
    uint64_t path_addr = random_addr - (path_len + 1);
    char *path_ptr = (char *)path_addr;
    for (uint32_t i = 0; i <= path_len; i++) path_ptr[i] = path[i];

    /* Align stack pointer to 16 bytes */
    uint64_t sp = (path_addr) & ~15ULL;

    /* Now build the auxv array */
    Elf64_Auxv auxv[16];
    int ac = 0;
    auxv[ac++] = (Elf64_Auxv){AT_PHDR, phdr_vaddr};
    auxv[ac++] = (Elf64_Auxv){AT_PHENT, hdr.e_phentsize};
    auxv[ac++] = (Elf64_Auxv){AT_PHNUM, hdr.e_phnum};
    auxv[ac++] = (Elf64_Auxv){AT_PAGESZ, 4096};
    auxv[ac++] = (Elf64_Auxv){AT_BASE, has_interp ? interpreter_base : 0};
    auxv[ac++] = (Elf64_Auxv){AT_FLAGS, 0};
    auxv[ac++] = (Elf64_Auxv){AT_ENTRY, hdr.e_entry};
    auxv[ac++] = (Elf64_Auxv){AT_UID, 0};
    auxv[ac++] = (Elf64_Auxv){AT_EUID, 0};
    auxv[ac++] = (Elf64_Auxv){AT_GID, 0};
    auxv[ac++] = (Elf64_Auxv){AT_EGID, 0};
    auxv[ac++] = (Elf64_Auxv){AT_SECURE, 0};
    auxv[ac++] = (Elf64_Auxv){AT_RANDOM, random_addr};
    auxv[ac++] = (Elf64_Auxv){AT_NULL, 0};

    /* Space for auxv, envp (NULL), argv (path_addr, NULL), argc */
    uint32_t auxv_size = ac * sizeof(Elf64_Auxv);
    uint32_t envp_size = 8; // envp[0] = NULL
    uint32_t argv_size = 16; // argv[0] = path_addr, argv[1] = NULL
    uint32_t argc_size = 8;  // argc = 1

    uint32_t total_stack_params = argc_size + argv_size + envp_size + auxv_size;
    sp = (sp - total_stack_params) & ~15ULL;

    uint64_t *sp_ptr = (uint64_t *)sp;
    int idx = 0;
    /* argc */
    sp_ptr[idx++] = 1;
    /* argv */
    sp_ptr[idx++] = path_addr;
    sp_ptr[idx++] = 0; // NULL
    /* envp */
    sp_ptr[idx++] = 0; // NULL
    /* auxv */
    Elf64_Auxv *sp_auxv = (Elf64_Auxv *)&sp_ptr[idx];
    for (int i = 0; i < ac; i++) {
        sp_auxv[i] = auxv[i];
    }

    *out_entry = entry_point;
    *out_stack_top = sp;
    return true;

error_cleanup_no_file:
    /* Clean up all pages mapped so far */
    for (int i = 0; i < out_pages->count; i++) {
        vmm_unmap_page(pml4, out_pages->virt_addrs[i]);
        pmm_free_page(out_pages->phys_pages[i]);
    }
    out_pages->count = 0;
    return false;
}
