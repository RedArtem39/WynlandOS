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

static void *find_mapped_page(PageTable *pml4, uint64_t virt) {
    uint64_t phys = vmm_get_phys(pml4, virt);
    if (phys == 0) return NULL;
    return (void *)(uintptr_t)phys;
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
            void *phys = find_mapped_page(pml4, addr);

            /* An existing mapping at this address that ISN'T PAGE_USER means
               this virtual address collides with kernel-identity-mapped
               memory this process's PML4 aliases (see vmm_new_process_pml4()
               in vmm.c) -- e.g. a low ET_EXEC link address that happens to
               land in the shared low region. Writing segment content
               straight into it would silently corrupt whatever the kernel
               actually uses that shared physical page for (in the worst
               case, a live page table). Refuse instead of corrupting
               memory. Link the binary at a high, non-aliased address
               (compare the stack/ET_DYN/interpreter fixes above) instead. */
            if (phys && !vmm_is_user_page(pml4, addr)) {
                serial_write_string("ELF Loader Error: segment at ");
                extern void uint_to_hex(uint64_t val, char *buf);
                char hb[20];
                uint_to_hex(addr, hb);
                serial_write_string(hb);
                serial_write_string(" collides with shared kernel memory -- refusing to load. "
                                     "Link this binary at a high, non-identity-mapped address.\r\n");
                return false;
            }

            if (!phys) {
                phys = pmm_alloc_page();
                if (!phys) {
                    serial_write_string("ELF Loader Error: Out of physical memory for segment load.\r\n");
                    return false;
                }

                if (out_pages->count < MAX_LOADED_PAGES) {
                    out_pages->phys_pages[out_pages->count] = phys;
                    out_pages->virt_addrs[out_pages->count] = addr;
                    out_pages->count++;
                }

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

bool elf_load(const char *path, uint64_t *out_entry, uint64_t *out_stack_top, PageTable *pml4, LoadedPages *out_pages, const char **argv, const char **envp)
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

    /* Load main program segments */
    uint64_t load_offset = 0;
    if (hdr.e_type == 3) { // ET_DYN (PIE executable or shared library)
        /* PML4 index 160 (0x500000000000) -- was 0x400000000 (16 GB, PML4
           index 0) which aliases the same top-level PML4 slot as the
           kernel/RAM/framebuffer identity map. Under per-process address
           spaces (vmm_new_process_pml4()) that slot is shared by pointer
           across every process, so any PIE (ET_DYN) binary loaded there
           would be visible/writable from every other process -- silently
           breaking isolation for anything built PIE (the common default
           for modern toolchains). Moved to a disjoint slot, clear of
           index 0, the interpreter base (index 224), and the user stack
           (index 192, see below). */
        load_offset = 0x500000000000ULL; // PML4 index 160
    }
    
    phdr_vaddr += load_offset;

    if (!load_elf_segments(f, &hdr, load_offset, pml4, out_pages)) {
        vfs_close(f);
        goto error_cleanup_no_file;
    }
    vfs_close(f);

    /* 4. Load Interpreter if present */
    uint64_t interpreter_base = 0x700000000000ULL;
    uint64_t entry_point = hdr.e_entry + load_offset;

    if (has_interp) {
        serial_write_string("ELF: opening interp=[");
        serial_write_string(interp_path);
        serial_write_string("]\r\n");
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
            /* Debug: print first 8 bytes */
            serial_write_string("ELF Loader Error: Invalid interpreter ELF magic. Bytes: ");
            char hx[4]; extern void uint_to_hex(uint64_t, char*);
            for (int di = 0; di < 8; di++) {
                uint_to_hex(interp_hdr.e_ident[di], hx);
                serial_write_string(hx); serial_write_string(" ");
            }
            serial_write_string("\r\n");
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

    /* 5. Map user stack (8 MB -- see below for why it grew from 64 KB).
       PML4 index 192 (0x600000000000) -- was 0x50000000, which aliases
       PML4 index 0, the same shared slot as the kernel/RAM/framebuffer
       identity map. Under per-process address spaces every process's
       stack would land in a page-table chain shared by pointer across
       ALL processes, silently breaking isolation (see vmm_new_process_pml4()
       in kernel/vmm.c). Moved to a slot disjoint from index 0, the ET_DYN
       load offset (index 160), and the interpreter base (index 224). Each
       PML4 index spans 512GB, so growing this by orders of magnitude below
       still leaves enormous headroom before the next region (/dev/fb0 at
       index 194, 0x610000000000).

       Size bumped from the original 64 KB to 8 MB (matching a typical real
       Linux default `ulimit -s`) after pkg-config's own real-world `main()`
       (a single ~66 KB local stack frame for its output-formatting buffers)
       stack-overflowed on its very first instruction -- 64 KB was simply
       too small for a real hosted program with realistically-sized locals,
       not something specific to pkg-config. Every future userspace port
       (CMake, git, curl, etc.) would hit the same wall sooner or later. */
    uint64_t stack_base = 0x600000000000ULL;
    uint64_t stack_size = 8 * 1024 * 1024;
    for (uint64_t offset = 0; offset < stack_size; offset += PAGE_SIZE) {
        uint64_t addr = stack_base + offset;
        void *phys = find_mapped_page(pml4, addr);
        if (!phys) {
            phys = pmm_alloc_page();
            if (!phys) {
                serial_write_string("ELF Loader Error: Out of physical memory for stack allocation.\r\n");
                goto error_cleanup_no_file;
            }

            if (out_pages->count < MAX_LOADED_PAGES) {
                out_pages->phys_pages[out_pages->count] = phys;
                out_pages->virt_addrs[out_pages->count] = addr;
                out_pages->count++;
            }

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
    auxv[ac++] = (Elf64_Auxv){AT_ENTRY, hdr.e_entry + load_offset};
    auxv[ac++] = (Elf64_Auxv){AT_UID, 0};
    auxv[ac++] = (Elf64_Auxv){AT_EUID, 0};
    auxv[ac++] = (Elf64_Auxv){AT_GID, 0};
    auxv[ac++] = (Elf64_Auxv){AT_EGID, 0};
    auxv[ac++] = (Elf64_Auxv){AT_SECURE, 0};
    auxv[ac++] = (Elf64_Auxv){AT_RANDOM, random_addr};
    auxv[ac++] = (Elf64_Auxv){AT_NULL, 0};

    /* envp: NULL preserves the historical hardcoded 5-var default every
       existing caller relies on. A real caller-supplied array (Phase 18's
       execve()) fully replaces it, mirroring argv's own NULL-means-default
       convention immediately below. */
    #define MAX_SPAWN_ENVP 16
    uint64_t envp_addrs[MAX_SPAWN_ENVP];
    int envp_count = 0;

    if (envp == NULL) {
        /* g_tz_envp_line (kernel/rtc.c) is a mutable global, not a string
           literal -- filled at boot by tz_auto_detect() (or left at its
           "TZ=UTC0" default if that failed/no network), so every process
           spawned this way picks up the real, currently-known timezone
           without needing a compile-time-constant value here. */
        extern char g_tz_envp_line[32];
        const char *default_envp[] = {
            "XDG_RUNTIME_DIR=/tmp",
            "LD_LIBRARY_PATH=/lib64",
            "WLR_BACKENDS=headless",
            "WLR_RENDERER=pixman",
            "LIBGL_DRIVERS_PATH=/lib64/dri",
            g_tz_envp_line,
        };
        for (int e = 0; e < 6; e++) {
            uint32_t len = 0;
            while (default_envp[e][len] != '\0') len++;
            uint64_t addr = sp - (len + 1);
            char *ptr = (char *)addr;
            for (uint32_t i = 0; i <= len; i++) ptr[i] = default_envp[e][i];
            sp = addr;
            envp_addrs[envp_count++] = addr;
        }
    } else {
        for (int e = 0; envp[e] != NULL && envp_count < MAX_SPAWN_ENVP; e++) {
            uint32_t len = 0;
            while (envp[e][len] != '\0') len++;
            uint64_t addr = sp - (len + 1);
            char *ptr = (char *)addr;
            for (uint32_t i = 0; i <= len; i++) ptr[i] = envp[e][i];
            sp = addr;
            envp_addrs[envp_count++] = addr;
        }
    }

    /* argv: NULL preserves the exact historical behavior every existing
       caller relies on (argc=2: path + a legacy dummy arg, both using
       stack addresses already built above/below). A real NULL-terminated
       array (e.g. Zerp spawning a client with fd numbers as argv) fully
       REPLACES that default -- argv[0] is whatever the caller supplied
       (real execve() semantics: argv[0] need not literally equal path),
       not appended after an implicit path_addr entry. Getting this
       conflated (path_addr always forced into argv[0], caller's argv
       appended after) was tried first and produced a duplicated/shifted
       argv in the child -- caught live via a spawn test where the child's
       argv[1] came back as the path string instead of the intended arg. */
    #define MAX_SPAWN_ARGV 8
    uint64_t argv_addrs[MAX_SPAWN_ARGV];
    int argv_count = 0;

    if (argv == NULL) {
        const char *dummy = "--i-am-really-stupid";
        uint32_t dummy_len = 0;
        while (dummy[dummy_len] != '\0') dummy_len++;
        uint64_t dummy_addr = sp - (dummy_len + 1);
        char *dummy_ptr = (char *)dummy_addr;
        for (uint32_t i = 0; i <= dummy_len; i++) dummy_ptr[i] = dummy[i];
        sp = dummy_addr;
        argv_addrs[argv_count++] = path_addr;
        argv_addrs[argv_count++] = dummy_addr;
    } else {
        for (int a = 0; argv[a] != NULL && argv_count < MAX_SPAWN_ARGV; a++) {
            uint32_t len = 0;
            while (argv[a][len] != '\0') len++;
            uint64_t addr = sp - (len + 1);
            char *ptr = (char *)addr;
            for (uint32_t i = 0; i <= len; i++) ptr[i] = argv[a][i];
            sp = addr;
            argv_addrs[argv_count++] = addr;
        }
    }

    /* Align stack to 16 bytes again */
    sp &= ~15ULL;

    uint32_t auxv_size = ac * sizeof(Elf64_Auxv);
    uint32_t envp_size = (envp_count + 1) * 8; // + NULL terminator
    uint32_t argc_val  = argv_count;
    uint32_t argv_size = (argc_val + 1) * 8;   // + NULL terminator
    uint32_t argc_size = 8;

    uint32_t total_stack_params = argc_size + argv_size + envp_size + auxv_size;
    sp = (sp - total_stack_params) & ~15ULL;

    uint64_t *sp_ptr = (uint64_t *)sp;
    int idx = 0;
    /* argc */
    sp_ptr[idx++] = argc_val;
    /* argv */
    for (int a = 0; a < argv_count; a++) sp_ptr[idx++] = argv_addrs[a];
    sp_ptr[idx++] = 0; // NULL
    /* envp */
    for (int e = 0; e < envp_count; e++) sp_ptr[idx++] = envp_addrs[e];
    sp_ptr[idx++] = 0; // NULL

    /* Copied byte-by-byte through a volatile pointer rather than a plain
       `sp_auxv[i] = auxv[i]` struct assignment. This is NOT the same bug as
       the syscall_entry.asm stack-misalignment issue fixed elsewhere (that
       fix is confirmed independently, see drivers/video/virtio_gpu.c, which
       now uses a plain memset() with no workaround) -- reverting *this*
       byte-loop (with the asm fix in place) instead produces a *different*
       crash: a Page Fault with RIP=2 inside thread_enter_user_mode_clone's
       iretq setup (kernel/gdt_flush.asm), when process_spawn() is reached
       via a nested SYS_spawn call from a Ring-3 process (spawner.elf
       -> inherited.elf). Root cause of that second issue is still open;
       this workaround stays in place until it's diagnosed separately. */
    volatile uint8_t *sp_auxv_bytes = (volatile uint8_t *)&sp_ptr[idx];
    const uint8_t *auxv_bytes = (const uint8_t *)auxv;
    for (uint32_t b = 0; b < ac * sizeof(Elf64_Auxv); b++) {
        sp_auxv_bytes[b] = auxv_bytes[b];
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
