/*
 * WynlandOS - loadable kernel modules (.ko): a driver built on its own and
 * loaded into the running kernel (insmod), no reboot -- and over the
 * network with test-wyn, as the dev loop wants.
 *
 * A module is a relocatable ELF object (gcc -c / ld -r, the kernel's flags:
 * tools/mod.mk). Loading: its allocated sections laid out in the module
 * area (just past the kernel image, so within 2 GB of it -- any code model
 * reaches the kernel), its relocations applied (x86-64: 64, PC32, PLT32,
 * 32, 32S, PC64), symbols it lacks taken from the kernel's own table
 * (ksyms_table, made at link time: tools/ksyms.py) or from modules loaded
 * before; then its init_module() runs (non-zero: it is undone). rmmod runs
 * its cleanup_module() and gives the pages back.
 *
 * Syscalls (root's only, CAP_SYS_MODULE outside a sandbox): init_module
 * (175: image, length, name), delete_module (176: name), and 420, ours: the
 * list as text (lsmod).
 */
#include <wynland/types.h>
#include <wynland/vmm.h>
#include <wynland/pmm.h>
#include <wynland/memlayout.h>
#include <wynland/heap.h>
#include <wynland/usercopy.h>
#include <wynland/sched.h>
#include <wynland/sandbox.h>

extern void serial_write_string(const char *s);
extern void uint_to_hex(uint64_t v, char *buf);
extern void *memset(void *d, int c, size_t n);

#define MODULE_BASE  0xFFFFFFFFA0000000ULL            /* the kernel image ends far below */
#define MODULE_LIMIT 0xFFFFFFFFC0000000ULL
#define MAX_MODULES  32
#define MAX_IMAGE    (16u << 20)

/* the kernel's symbols (tools/ksyms.py -> build/ksyms.c, linked last) */
typedef struct { uint64_t addr; const char *name; } KSym;
extern const KSym ksyms_table[];
extern const uint32_t ksyms_count;

typedef struct {
    bool     used;
    char     name[32];
    uint64_t base, size;                              /* its pages, in the module area */
    void   (*cleanup)(void);
    /* its own global symbols, for modules loaded after it */
    const char *strtab;
    const void *symtab;                               /* Elf64_Sym[], copied */
    uint32_t nsyms;
    uint64_t *sec_addr;                               /* where each section went */
} Module;

static Module g_mods[MAX_MODULES];
static uint64_t g_next = MODULE_BASE;                 /* the module area: never reused (a bump) */

/* ---- ELF64 ------------------------------------------------------------------ */

typedef struct {
    unsigned char e_ident[16];
    uint16_t e_type, e_machine;
    uint32_t e_version;
    uint64_t e_entry, e_phoff, e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize, e_phentsize, e_phnum, e_shentsize, e_shnum, e_shstrndx;
} Ehdr;
typedef struct {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
} Shdr;
typedef struct {
    uint32_t st_name;
    unsigned char st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
} Sym;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } Rela;

#define SHT_SYMTAB 2
#define SHT_RELA   4
#define SHT_NOBITS 8
#define SHF_ALLOC  2
#define SHN_UNDEF  0
#define SHN_ABS    0xFFF1
#define SHN_COMMON 0xFFF2
#define STB_GLOBAL 1
#define STB_WEAK   2

static void say(const char *s, const char *name, uint64_t v)
{
    char b[20];
    serial_write_string("module: ");
    serial_write_string(s);
    if (name) serial_write_string(name);
    if (v != ~0ull) { serial_write_string(" "); uint_to_hex(v, b); serial_write_string(b); }
    serial_write_string("\r\n");
}

static bool streq(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

/* a symbol by name: the kernel's, then the loaded modules' globals */
static bool lookup(const char *name, uint64_t *addr)
{
    for (uint32_t i = 0; i < ksyms_count; i++)
        if (streq(ksyms_table[i].name, name)) { *addr = ksyms_table[i].addr; return true; }
    for (uint32_t m = 0; m < MAX_MODULES; m++) {
        if (!g_mods[m].used) continue;
        const Sym *s = g_mods[m].symtab;
        for (uint32_t i = 0; i < g_mods[m].nsyms; i++) {
            uint8_t bind = s[i].st_info >> 4;
            if ((bind != STB_GLOBAL && bind != STB_WEAK) || s[i].st_shndx == SHN_UNDEF || !s[i].st_name) continue;
            if (!streq(g_mods[m].strtab + s[i].st_name, name)) continue;
            *addr = s[i].st_shndx == SHN_ABS ? s[i].st_value : g_mods[m].sec_addr[s[i].st_shndx] + s[i].st_value;
            return true;
        }
    }
    return false;
}

/* the kernel's symbol for an address (stack dumps of module code): the
   nearest at or below */
const char *ksym_name(uint64_t addr, uint64_t *off)
{
    const KSym *best = NULL;
    for (uint32_t i = 0; i < ksyms_count; i++)
        if (ksyms_table[i].addr <= addr && (!best || ksyms_table[i].addr > best->addr)) best = &ksyms_table[i];
    if (!best) return NULL;
    if (off) *off = addr - best->addr;
    return best->name;
}

static void area_free(uint64_t base, uint64_t size)
{
    PageTable *k = vmm_get_kernel_pml4();
    for (uint64_t va = base; va < base + size; va += PAGE_SIZE) {
        uint64_t phys = vmm_get_phys(k, va);
        vmm_unmap_page(k, va);
        if (phys) pmm_free_page((void *)(uintptr_t)(phys & ~(PAGE_SIZE - 1)));
    }
}

/* `size` bytes of the module area, writable and executable, zeroed */
static uint64_t area_alloc(uint64_t size)
{
    size = (size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    if (g_next + size > MODULE_LIMIT) return 0;
    uint64_t base = g_next;
    PageTable *k = vmm_get_kernel_pml4();
    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        void *p = pmm_alloc_page();
        if (!p) { area_free(base, off); return 0; }
        memset(P2V((uint64_t)(uintptr_t)p), 0, PAGE_SIZE);
        vmm_map_page(k, base + off, (uint64_t)(uintptr_t)p, PAGE_WRITE);   /* (no NX: code) */
    }
    g_next += size + PAGE_SIZE;                       /* a guard page between modules */
    return base;
}

static int64_t load(const uint8_t *img, uint64_t len, const char *name)
{
    const Ehdr *eh = (const Ehdr *)img;
    if (len < sizeof *eh || eh->e_ident[0] != 0x7F || eh->e_ident[1] != 'E' || eh->e_ident[2] != 'L' ||
        eh->e_ident[3] != 'F' || eh->e_ident[4] != 2 || eh->e_type != 1 /* ET_REL */ || eh->e_machine != 62) {
        say("not an x86-64 relocatable ELF: ", name, ~0ull);
        return -8;                                    /* -ENOEXEC */
    }
    if (eh->e_shoff > len || (uint64_t)eh->e_shnum * sizeof(Shdr) > len - eh->e_shoff) return -8;
    const Shdr *sh = (const Shdr *)(img + eh->e_shoff);
    uint32_t nsec = eh->e_shnum;
    for (uint32_t i = 0; i < nsec; i++)
        if (sh[i].sh_type != SHT_NOBITS && (sh[i].sh_offset > len || sh[i].sh_size > len - sh[i].sh_offset)) return -8;

    /* the layout: every allocated section, aligned */
    uint64_t *at = kmalloc(nsec * sizeof *at);
    if (!at) return -12;
    uint64_t total = 0;
    for (uint32_t i = 0; i < nsec; i++) {
        at[i] = 0;
        if (!(sh[i].sh_flags & SHF_ALLOC)) continue;
        uint64_t a = sh[i].sh_addralign ? sh[i].sh_addralign : 1;
        total = (total + a - 1) & ~(a - 1);
        at[i] = total;
        total += sh[i].sh_size;
    }
    uint64_t base = area_alloc(total ? total : 1);
    if (!base) { kfree(at); return -12; }
    for (uint32_t i = 0; i < nsec; i++) {
        if (!(sh[i].sh_flags & SHF_ALLOC)) continue;
        at[i] += base;
        if (sh[i].sh_type != SHT_NOBITS) memcpy((void *)at[i], img + sh[i].sh_offset, sh[i].sh_size);
    }

    /* the symbol table: values made addresses, the kernel's for those it lacks */
    const Shdr *symsh = NULL;
    for (uint32_t i = 0; i < nsec; i++) if (sh[i].sh_type == SHT_SYMTAB) symsh = &sh[i];
    if (!symsh || symsh->sh_link >= nsec) { say("no symbol table: ", name, ~0ull); goto bad; }
    uint32_t nsym = (uint32_t)(symsh->sh_size / sizeof(Sym));
    const Sym *sym = (const Sym *)(img + symsh->sh_offset);
    const char *str = (const char *)(img + sh[symsh->sh_link].sh_offset);
    uint64_t strsz = sh[symsh->sh_link].sh_size;
    uint64_t *val = kmalloc((nsym ? nsym : 1) * sizeof *val);
    if (!val) goto bad;
    uint64_t init = 0, cleanup = 0;
    for (uint32_t i = 0; i < nsym; i++) {
        const char *sn = sym[i].st_name < strsz ? str + sym[i].st_name : "";
        uint16_t sx = sym[i].st_shndx;
        if (sx == SHN_UNDEF) {
            val[i] = 0;
            if (!*sn) continue;
            if (!lookup(sn, &val[i])) {
                if ((sym[i].st_info >> 4) == STB_WEAK) continue;
                say("unknown symbol: ", sn, ~0ull);
                kfree(val);
                goto bad;
            }
        } else if (sx == SHN_ABS) {
            val[i] = sym[i].st_value;
        } else if (sx == SHN_COMMON) {
            say("common symbol (build with -fno-common): ", sn, ~0ull);
            kfree(val);
            goto bad;
        } else if (sx < nsec) {
            val[i] = at[sx] + sym[i].st_value;
        } else {
            val[i] = 0;
        }
        if (streq(sn, "init_module")) init = val[i];
        if (streq(sn, "cleanup_module")) cleanup = val[i];
    }

    /* relocations, into the allocated sections */
    for (uint32_t i = 0; i < nsec; i++) {
        if (sh[i].sh_type != SHT_RELA || sh[i].sh_info >= nsec || !(sh[sh[i].sh_info].sh_flags & SHF_ALLOC)) continue;
        const Rela *rl = (const Rela *)(img + sh[i].sh_offset);
        uint64_t nrl = sh[i].sh_size / sizeof(Rela);
        uint64_t tgt = at[sh[i].sh_info];
        for (uint64_t k = 0; k < nrl; k++) {
            uint32_t type = (uint32_t)rl[k].r_info, si = (uint32_t)(rl[k].r_info >> 32);
            if (si >= nsym || rl[k].r_offset + 8 > sh[sh[i].sh_info].sh_size + 4) { kfree(val); goto bad; }
            uint64_t S = val[si], P = tgt + rl[k].r_offset;
            int64_t A = rl[k].r_addend;
            switch (type) {
            case 0: break;                                            /* NONE */
            case 1: *(uint64_t *)P = S + (uint64_t)A; break;          /* 64 */
            case 24: *(uint64_t *)P = S + (uint64_t)A - P; break;     /* PC64 */
            case 2: case 4: {                                         /* PC32, PLT32 */
                int64_t v = (int64_t)(S + (uint64_t)A - P);
                if (v != (int32_t)v) { say("PC32 out of reach, symbol index", NULL, si); kfree(val); goto bad; }
                *(int32_t *)P = (int32_t)v;
                break;
            }
            case 10: {                                                /* 32 */
                uint64_t v = S + (uint64_t)A;
                if (v >> 32) { say("32 out of reach, symbol index", NULL, si); kfree(val); goto bad; }
                *(uint32_t *)P = (uint32_t)v;
                break;
            }
            case 11: {                                                /* 32S */
                int64_t v = (int64_t)(S + (uint64_t)A);
                if (v != (int32_t)v) { say("32S out of reach, symbol index", NULL, si); kfree(val); goto bad; }
                *(int32_t *)P = (int32_t)v;
                break;
            }
            default:
                say("relocation type not done:", NULL, type);
                kfree(val);
                goto bad;
            }
        }
    }
    kfree(val);

    Module *m = NULL;
    for (uint32_t i = 0; i < MAX_MODULES; i++) if (!g_mods[i].used) { m = &g_mods[i]; break; }
    if (!m) goto bad;
    memset(m, 0, sizeof *m);
    for (int i = 0; i < 31 && name[i]; i++) m->name[i] = name[i];
    m->base = base; m->size = total;
    m->cleanup = cleanup ? (void (*)(void))cleanup : NULL;
    /* its symbols kept for later modules */
    m->strtab = kmalloc(strsz ? strsz : 1);
    m->symtab = kmalloc(nsym * sizeof(Sym) + 1);
    m->sec_addr = at;
    if (!m->strtab || !m->symtab) goto bad;
    memcpy((void *)m->strtab, str, strsz);
    memcpy((void *)m->symtab, sym, nsym * sizeof(Sym));
    m->nsyms = nsym;
    m->used = true;
    say("loaded ", m->name, base);
    if (init) {
        int r = ((int (*)(void))init)();
        if (r) {
            say("init_module failed: ", m->name, (uint64_t)(int64_t)r);
            m->used = false;
            kfree((void *)m->strtab); kfree((void *)m->symtab);
            area_free(base, (total + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
            kfree(at);
            return r < 0 ? r : -22;
        }
    }
    return 0;

bad:
    area_free(base, (total + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    kfree(at);
    return -8;
}

static Module *find(const char *name)
{
    for (uint32_t i = 0; i < MAX_MODULES; i++) if (g_mods[i].used && streq(g_mods[i].name, name)) return &g_mods[i];
    return NULL;
}

static bool may_load(void)
{
    struct Process *p = sched_current() ? sched_current()->proc : NULL;
    return p && sandbox_capable_host(p, 16 /* CAP_SYS_MODULE */);
}

/* init_module(image, length, name): the name's ours (Linux: parameters) */
int64_t sys_init_module(uint64_t uimg, uint64_t len, uint64_t uname)
{
    if (!may_load()) return -1;                       /* -EPERM */
    if (!len || len > MAX_IMAGE) return -27;          /* -EFBIG */
    char name[32] = "module";
    if (uname && strncpy_from_user(name, (const void *)uname, sizeof name - 1) < 0) return -14;
    name[sizeof name - 1] = 0;
    if (find(name)) return -17;                       /* -EEXIST */
    uint8_t *img = kmalloc(len);
    if (!img) return -12;
    if (copy_from_user(img, (const void *)uimg, len)) { kfree(img); return -14; }
    int64_t r = load(img, len, name);
    kfree(img);
    return r;
}

int64_t sys_delete_module(uint64_t uname)
{
    if (!may_load()) return -1;
    char name[32];
    if (strncpy_from_user(name, (const void *)uname, sizeof name - 1) < 0) return -14;
    name[sizeof name - 1] = 0;
    Module *m = find(name);
    if (!m) return -2;                                /* -ENOENT */
    if (m->cleanup) m->cleanup();
    area_free(m->base, (m->size + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1));
    kfree((void *)m->strtab); kfree((void *)m->symtab); kfree(m->sec_addr);
    m->used = false;
    say("unloaded ", name, ~0ull);
    return 0;
}

/* lsmod: "name base size" a line, into the caller's buffer; its length */
int64_t sys_list_modules(uint64_t ubuf, uint64_t cap)
{
    char out[MAX_MODULES * 64];
    uint32_t n = 0;
    for (uint32_t i = 0; i < MAX_MODULES; i++) {
        if (!g_mods[i].used) continue;
        char b[20];
        for (const char *s = g_mods[i].name; *s && n < sizeof out - 48; s++) out[n++] = *s;
        out[n++] = ' ';
        uint_to_hex(g_mods[i].base, b);
        for (const char *s = b; *s; s++) out[n++] = *s;
        out[n++] = ' ';
        uint_to_hex(g_mods[i].size, b);
        for (const char *s = b; *s; s++) out[n++] = *s;
        out[n++] = '\n';
    }
    if (n > cap) n = (uint32_t)cap;
    if (n && copy_to_user((void *)ubuf, out, n)) return -14;
    return n;
}
