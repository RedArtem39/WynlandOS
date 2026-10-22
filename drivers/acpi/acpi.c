/*
 * WynlandOS - ACPI core
 *
 * RSDP -> XSDT/RSDT -> FADT/MADT/MCFG/HPET. Every table we use is copied
 * into the kernel heap, so the firmware's reclaimable memory can never be
 * handed out later while we still read it. On top of that: FADT sleep
 * states (S5) and reset, MADT access for SMP, MCFG/ECAM PCI config space.
 */

#include <wynland/acpi.h>
#include <wynland/vmm.h>
#include <wynland/memlayout.h>
#include <wynland/pmm.h>
#include <wynland/heap.h>

extern void serial_write_string(const char *s);
extern void uint_to_hex(uint64_t v, char *buf);
extern void uint_to_str(uint64_t v, char *buf);

#define ACPI_MAX_TABLES 24
#define ACPI_MAX_TABLE  (1u << 20)
#define ACPI_MAX_COPY   (4u << 20)
#define ACPI_MAX_MCFG   8

typedef struct PACKED {
    char     sig[4];
    uint32_t len;
    uint8_t  rev, csum;
    char     oem[6], oem_table[8];
    uint32_t oem_rev, creator, creator_rev;
} AcpiSdt;

typedef struct {
    char     sig[4];
    uint8_t *data;
    uint32_t len;
} AcpiTable;

typedef struct {
    bool     valid;
    uint8_t  space, width, access;
    uint64_t addr;
} AcpiReg;

typedef struct {
    uint64_t base;
    uint16_t seg;
    uint8_t  start, end;
} McfgRegion;

static AcpiTable g_tables[ACPI_MAX_TABLES];
static int       g_ntables;
static uint32_t  g_copied;
static bool      g_ready;

static uint16_t  g_pm1a_port, g_pm1b_port;
static uint8_t   g_pm1_len = 2;
static AcpiReg   g_pm1a_reg, g_pm1b_reg;
static uint16_t  g_smi_cmd;
static uint8_t   g_acpi_enable;
static uint8_t   g_slp_typa = 5, g_slp_typb = 5;
static bool      g_s5_found;
static uint32_t  g_fadt_flags;

static bool      g_reset_valid;
static uint8_t   g_reset_space, g_reset_width;
static uint64_t  g_reset_addr;
static uint8_t   g_reset_value;

static McfgRegion g_mcfg[ACPI_MAX_MCFG];
static int        g_nmcfg;

static inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile("outb %0, %1" :: "a"(v), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t v)
{
    __asm__ volatile("outw %0, %1" :: "a"(v), "Nd"(port));
}

static inline void outl(uint16_t port, uint32_t v)
{
    __asm__ volatile("outl %0, %1" :: "a"(v), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline uint16_t inw(uint16_t port)
{
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t rd64(const uint8_t *p)
{
    return (uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32);
}

static bool sig_eq(const char *a, const char *b)
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static void acpi_delay_ms(uint32_t ms)
{
    uint64_t start = __builtin_ia32_rdtsc();
    uint64_t limit = (uint64_t)ms * 5000000ULL;
    while (__builtin_ia32_rdtsc() - start < limit)
        __asm__ volatile("pause");
}

static void acpi_put(const char *s)
{
    serial_write_string(s);
}

static void acpi_put_hex(uint64_t v)
{
    char b[20];
    uint_to_hex(v, b);
    serial_write_string(b);
}

static void acpi_put_dec(uint64_t v)
{
    char b[24];
    uint_to_str(v, b);
    serial_write_string(b);
}

static void acpi_line(void)
{
    serial_write_string("\r\n");
}

/* Firmware tables may sit in reserved ranges the direct map left out.
   false: not a physical address at all (a broken pointer in a table):
   the caller must not touch it. */
static bool acpi_map(uint64_t addr, uint64_t len)
{
    if (!addr || len > (16u << 20)) return false;
    PageTable *k = vmm_get_kernel_pml4();
    uint64_t end = addr + len;
    if (end < addr || end > HHDM_SIZE) return false;
    for (uint64_t a = addr & ~0xFFFULL; a < end; a += PAGE_SIZE)
        if (!vmm_get_phys(k, HHDM_BASE + a))
            vmm_map_page(k, HHDM_BASE + a, a, PAGE_NX);
    return true;
}

static bool table_sum_ok(const uint8_t *p, uint32_t len)
{
    uint8_t sum = 0;
    for (uint32_t i = 0; i < len; i++) sum = (uint8_t)(sum + p[i]);
    return sum == 0;
}

static const uint8_t *find_table(const char *sig)
{
    for (int i = 0; i < g_ntables; i++)
        if (sig_eq(g_tables[i].sig, sig)) return g_tables[i].data;
    return NULL;
}

static uint32_t find_table_len(const char *sig)
{
    for (int i = 0; i < g_ntables; i++)
        if (sig_eq(g_tables[i].sig, sig)) return g_tables[i].len;
    return 0;
}

static const uint8_t *add_table(const uint8_t *src, uint32_t len)
{
    if (g_ntables >= ACPI_MAX_TABLES || len < 36 || len > ACPI_MAX_TABLE)
        return NULL;
    if (g_copied + len > ACPI_MAX_COPY) return NULL;

    uint8_t *dst = kmalloc(len);
    if (!dst) {
        acpi_put("ACPI: out of memory for a table\r\n");
        return NULL;
    }
    for (uint32_t i = 0; i < len; i++) dst[i] = src[i];

    AcpiTable *t = &g_tables[g_ntables++];
    t->sig[0] = (char)dst[0];
    t->sig[1] = (char)dst[1];
    t->sig[2] = (char)dst[2];
    t->sig[3] = (char)dst[3];
    t->data = dst;
    t->len = len;
    g_copied += len;

    if (!table_sum_ok(dst, len)) {
        acpi_put("ACPI: table ");
        acpi_put(t->sig);
        acpi_put(" has a bad checksum (kept)\r\n");
    }
    return dst;
}

static const uint8_t *copy_table(uint64_t phys, uint32_t len)
{
    if (!phys || len < 36 || len > ACPI_MAX_TABLE) return NULL;
    if (!acpi_map(phys, len)) return NULL;
    return add_table((const uint8_t *)P2V(phys), len);
}

static bool parse_gas(const uint8_t *p, uint32_t avail, AcpiReg *r)
{
    if (avail < 12) return false;
    r->space  = p[0];
    r->width  = p[1];
    r->access = p[3];
    r->addr   = rd64(p + 4);
    r->valid  = r->addr != 0 && (r->space == 0 || r->space == 1);
    return r->valid;
}

static bool acpi_reg_ok(const AcpiReg *r, uint16_t port)
{
    return (r->valid) || port != 0;
}

static uint8_t pm_width(const AcpiReg *r)
{
    uint8_t w = g_pm1_len;
    if (r->valid && r->width >= 8) w = (uint8_t)(r->width / 8);
    if (w < 1) w = 1;
    if (w > 4) w = 4;
    return w;
}

static uint32_t pm_read(const AcpiReg *r, uint16_t port)
{
    uint8_t w = pm_width(r);
    if (r->valid) {
        if (r->space == 0) {
            volatile uint8_t *p = (volatile uint8_t *)vmm_map_mmio(r->addr, w);
            if (!p) return 0;
            uint32_t v = 0;
            for (uint8_t i = 0; i < w; i++) v |= (uint32_t)p[i] << (8 * i);
            return v;
        }
        if (r->space != 1) return 0;
        port = (uint16_t)r->addr;
    }
    if (!port) return 0;
    if (w == 1) return inb(port);
    if (w == 2) return inw(port);
    return inl(port);
}

static void pm_write(const AcpiReg *r, uint16_t port, uint32_t v)
{
    uint8_t w = pm_width(r);
    if (r->valid) {
        if (r->space == 0) {
            volatile uint8_t *p = (volatile uint8_t *)vmm_map_mmio(r->addr, w);
            if (!p) return;
            for (uint8_t i = 0; i < w; i++) p[i] = (uint8_t)(v >> (8 * i));
            return;
        }
        if (r->space != 1) return;
        port = (uint16_t)r->addr;
    }
    if (!port) return;
    if (w == 1) outb(port, (uint8_t)v);
    else if (w == 2) outw(port, (uint16_t)v);
    else outl(port, v);
}

static bool aml_int(const uint8_t **pp, const uint8_t *end, uint32_t *out)
{
    const uint8_t *p = *pp;
    if (p >= end) return false;
    uint8_t op = *p++;

    switch (op) {
    case 0x00: *out = 0; break;
    case 0x01: *out = 1; break;
    case 0xFF: *out = 0xFFFFFFFFu; break;
    case 0x0A:
        if (p >= end) return false;
        *out = *p++;
        break;
    case 0x0B:
        if (p + 2 > end) return false;
        *out = rd16(p);
        p += 2;
        break;
    case 0x0C:
        if (p + 4 > end) return false;
        *out = rd32(p);
        p += 4;
        break;
    case 0x0E:
        if (p + 8 > end) return false;
        *out = rd32(p);
        p += 8;
        break;
    default:
        return false;
    }
    *pp = p;
    return true;
}

/* Name (_S5, Package (2) { SLP_TYPa, SLP_TYPb }) in the DSDT -- also
   written Name (\_S5, ...): a root prefix (0x5C) before the name, as real
   firmware often has it. */
static bool parse_s5(const uint8_t *dsdt, uint32_t len)
{
    if (!dsdt || len < 12) return false;

    for (uint32_t i = 0; i + 11 <= len; i++) {
        if (dsdt[i] != 0x08) continue;
        uint32_t at = i + 1 + (dsdt[i + 1] == 0x5C);
        if (!sig_eq((const char *)(dsdt + at), "_S5_")) continue;

        const uint8_t *pkg = dsdt + at + 4;
        const uint8_t *end = dsdt + len;
        if (*pkg != 0x12) continue;

        const uint8_t *q = pkg + 1;
        uint8_t lead = *q++;
        uint32_t plen = lead & 0x3F;
        uint32_t extra = lead >> 6;
        if (q + extra > end) continue;
        for (uint32_t j = 0; j < extra; j++)
            plen |= (uint32_t)(*q++) << (4 + 8 * j);

        const uint8_t *pkg_end = pkg + plen;
        if (pkg_end > end) pkg_end = end;
        if (q >= pkg_end) continue;
        q++;                                    /* NumElements */

        uint32_t a = 5, b = 5;
        if (!aml_int(&q, pkg_end, &a)) continue;
        if (!aml_int(&q, pkg_end, &b)) b = a;
        g_slp_typa = (uint8_t)a;
        g_slp_typb = (uint8_t)b;
        return true;
    }
    return false;
}

static void parse_mcfg(const uint8_t *t, uint32_t len)
{
    if (!t || len < 44) return;
    uint32_t n = (len - 44) / 16;
    for (uint32_t i = 0; i < n && g_nmcfg < ACPI_MAX_MCFG; i++) {
        const uint8_t *e = t + 44 + i * 16;
        uint64_t base = rd64(e);
        uint16_t seg = rd16(e + 8);
        uint8_t start = e[10], end = e[11];
        if (!base || start > end) continue;
        McfgRegion *r = &g_mcfg[g_nmcfg++];
        r->base = base;
        r->seg = seg;
        r->start = start;
        r->end = end;

        acpi_put("ACPI: MCFG ECAM ");
        acpi_put_hex(base);
        acpi_put(" bus ");
        acpi_put_dec(start);
        acpi_put("-");
        acpi_put_dec(end);
        acpi_line();
    }
}

static void log_madt(const uint8_t *m, uint32_t len)
{
    if (!m || len < 44) return;
    uint32_t cpus = 0, ioapics = 0;
    for (uint32_t off = 44; off + 2 <= len; off += m[off + 1]) {
        uint8_t elen = m[off + 1];
        if (elen < 2 || off + elen > len) break;
        if (m[off] == 0 && elen >= 8) {
            if (rd32(m + off + 4) & 1) cpus++;
        } else if (m[off] == 9 && elen >= 12) {
            if (rd32(m + off + 8) & 1) cpus++;
        } else if (m[off] == 1 && elen >= 12) {
            ioapics++;
        }
    }
    acpi_put("ACPI: MADT cpus=");
    acpi_put_dec(cpus);
    acpi_put(" ioapics=");
    acpi_put_dec(ioapics);
    acpi_line();
}

static bool ecam_phys(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off,
                      uint64_t *out)
{
    for (int i = 0; i < g_nmcfg; i++) {
        const McfgRegion *r = &g_mcfg[i];
        if (r->seg != 0 || bus < r->start || bus > r->end) continue;
        *out = r->base + ((uint64_t)(bus - r->start) << 20) +
               ((uint64_t)dev << 15) + ((uint64_t)fn << 12) + off;
        return true;
    }
    return false;
}

/* (the pointer comes from the address, not from a shared "page mapped
   last": with two cores at it, one got the other device's page -- and
   wrote its registers) */
static volatile uint8_t *ecam_map(uint64_t phys)
{
    uint64_t page = phys & ~0xFFFULL;
    if (!vmm_get_phys(vmm_get_kernel_pml4(), HHDM_BASE + page)) vmm_map_mmio(page, PAGE_SIZE);
    return (volatile uint8_t *)P2V(phys);
}

static bool acpi_try_root(uint64_t phys, bool xsdt)
{
    if (!acpi_map(phys, 36)) return false;
    const uint8_t *h = (const uint8_t *)P2V(phys);
    if (!xsdt) {
        if (!sig_eq((const char *)h, "RSDT")) return false;
    } else {
        if (!sig_eq((const char *)h, "XSDT")) return false;
    }
    uint32_t len = rd32(h + 4);
    if (len < 36 || len > (1u << 20)) return false;
    if (!acpi_map(phys, len)) return false;
    h = (const uint8_t *)P2V(phys);
    if (!table_sum_ok(h, len))
        acpi_put("ACPI: root table checksum bad (continuing)\r\n");

    uint32_t step = xsdt ? 8 : 4;
    uint32_t n = (len - 36) / step;
    for (uint32_t i = 0; i < n && g_ntables < ACPI_MAX_TABLES; i++) {
        const uint8_t *e = h + 36 + i * step;
        uint64_t p = xsdt ? rd64(e) : rd32(e);
        if (!p || !acpi_map(p, 36)) continue;
        const uint8_t *th = (const uint8_t *)P2V(p);
        uint32_t tl = rd32(th + 4);
        if (tl < 36 || tl > ACPI_MAX_TABLE) continue;
        copy_table(p, tl);
    }
    return true;
}

bool acpi_init(uint64_t rsdp_phys)
{
    g_ready = false;
    g_ntables = 0;
    g_copied = 0;
    g_nmcfg = 0;
    g_s5_found = false;

    if (!rsdp_phys) {
        acpi_put("ACPI: no RSDP from the bootloader\r\n");
        return false;
    }

    if (!acpi_map(rsdp_phys, 36)) {
        acpi_put("ACPI: the RSDP address is not memory\r\n");
        return false;
    }
    const uint8_t *r = (const uint8_t *)P2V(rsdp_phys);
    if (!sig_eq((const char *)r, "RSD PTR ")) {
        acpi_put("ACPI: bad RSDP signature\r\n");
        return false;
    }
    uint8_t sum = 0;
    for (int i = 0; i < 20; i++) sum = (uint8_t)(sum + r[i]);
    if (sum != 0) {
        acpi_put("ACPI: bad RSDP checksum\r\n");
        return false;
    }
    if (r[15] >= 2) {
        uint8_t ext = 0;
        for (int i = 0; i < 36; i++) ext = (uint8_t)(ext + r[i]);
        if (ext != 0) acpi_put("ACPI: bad extended RSDP checksum\r\n");
    }

    bool xsdt_ok = false;
    uint64_t root = 0;
    if (r[15] >= 2) {
        root = rd64(r + 24);
        if (root) xsdt_ok = acpi_try_root(root, true);
    }
    if (!xsdt_ok) {
        root = rd32(r + 16);
        if (!root || !acpi_try_root(root, false)) {
            acpi_put("ACPI: no usable RSDT/XSDT\r\n");
            return false;
        }
    }

    acpi_put("ACPI: tables=");
    acpi_put_dec((uint64_t)g_ntables);
    acpi_put(" rsdp rev=");
    acpi_put_dec(r[15]);
    acpi_line();

    const uint8_t *madt = find_table("APIC");
    if (madt) log_madt(madt, find_table_len("APIC"));

    const uint8_t *mcfg = find_table("MCFG");
    if (mcfg) {
        parse_mcfg(mcfg, find_table_len("MCFG"));
        uint32_t probe = 0;
        if (g_nmcfg && (!acpi_pci_cfg_read(0, 0, 0, 0, &probe) ||
                        probe == 0xFFFFFFFFu)) {
            g_nmcfg = 0;
            acpi_put("ACPI: ECAM unreadable, PCI falls back to CF8/CFC\r\n");
        }
    }

    const uint8_t *hpet = find_table("HPET");
    if (hpet) {
        AcpiReg base;
        if (parse_gas(hpet + 40, 12, &base)) {
            acpi_put("ACPI: HPET base ");
            acpi_put_hex(base.addr);
            acpi_line();
        }
    }

    const uint8_t *f = find_table("FACP");
    if (!f) {
        acpi_put("ACPI: no FADT\r\n");
        g_ready = true;
        return true;
    }
    uint32_t len = rd32(f + 4);
    if (len < 64) {
        acpi_put("ACPI: FADT too short\r\n");
        g_ready = true;
        return true;
    }

    g_pm1a_port = (uint16_t)rd32(f + 64);
    g_pm1b_port = (uint16_t)(len >= 72 ? rd32(f + 68) : 0);
    g_pm1_len = (len >= 90 && f[89]) ? f[89] : 2;
    g_smi_cmd = (uint16_t)rd32(f + 48);
    g_acpi_enable = (len >= 53) ? f[52] : 0;
    g_fadt_flags = (len >= 116) ? rd32(f + 112) : 0;

    if (len >= 184) parse_gas(f + 172, 12, &g_pm1a_reg);
    if (len >= 196) parse_gas(f + 184, 12, &g_pm1b_reg);

    if (len >= 129) {
        uint32_t fl = rd32(f + 112);
        g_reset_space = f[116];
        g_reset_width = f[117];
        g_reset_addr = rd64(f + 120);
        g_reset_value = f[128];
        g_reset_valid = (fl & (1u << 10)) && g_reset_addr &&
                        (g_reset_space == 0 || g_reset_space == 1);
    }

    /* X_DSDT (offset 140) is a plain 64-bit address -- not a GAS like
       the X_ registers after it; DSDT (offset 40) the 32-bit one */
    uint64_t dsdt_phys = len >= 148 ? rd64(f + 140) : 0;
    if (dsdt_phys && (!acpi_map(dsdt_phys, 36) ||
                      !sig_eq((const char *)P2V(dsdt_phys), "DSDT")))
        dsdt_phys = 0;
    if (!dsdt_phys && len >= 44) {
        dsdt_phys = rd32(f + 40);
        if (dsdt_phys && (!acpi_map(dsdt_phys, 36) ||
                          !sig_eq((const char *)P2V(dsdt_phys), "DSDT")))
            dsdt_phys = 0;
    }
    if (dsdt_phys) {
        const uint8_t *dh = (const uint8_t *)P2V(dsdt_phys);
        uint32_t dl = rd32(dh + 4);
        const uint8_t *copy = copy_table(dsdt_phys, dl);
        if (copy) {
            g_s5_found = parse_s5(copy, dl);
            acpi_put("ACPI: DSDT ");
            acpi_put_hex(dsdt_phys);
            acpi_put(" len ");
            acpi_put_dec(dl);
            if (!g_s5_found) acpi_put(" (no _S5)");
            acpi_line();
        }
    }

    if (!g_s5_found) {
        for (int i = 0; i < g_ntables && !g_s5_found; i++)
            if (sig_eq(g_tables[i].sig, "SSDT"))
                g_s5_found = parse_s5(g_tables[i].data, g_tables[i].len);
    }

    acpi_put("ACPI: FADT PM1a=");
    acpi_put_hex(acpi_reg_ok(&g_pm1a_reg, g_pm1a_port) ?
                 (g_pm1a_reg.valid ? g_pm1a_reg.addr : g_pm1a_port) : 0);
    acpi_put(" SLP_TYPa=");
    acpi_put_hex(g_slp_typa);
    if (!g_s5_found) acpi_put(" (default)");
    if (g_pm1a_reg.valid) {
        acpi_put(" gas space=");
        acpi_put_dec(g_pm1a_reg.space);
        acpi_put(" width=");
        acpi_put_dec(g_pm1a_reg.width);
    }
    acpi_line();

    if (g_fadt_flags & (1u << 20))
        acpi_put("ACPI: hardware-reduced FADT, legacy PM unavailable\r\n");

    if (g_reset_valid) {
        acpi_put("ACPI: reset reg ");
        acpi_put_hex(g_reset_addr);
        acpi_put(" value ");
        acpi_put_hex(g_reset_value);
        acpi_line();
    }

    g_ready = true;
    return true;
}

bool acpi_available(void)
{
    return g_ready;
}

const void *acpi_find_table(const char *sig)
{
    if (!sig) return NULL;
    const uint8_t *t = find_table(sig);
    return t;
}

uint32_t acpi_table_count(void)
{
    return (uint32_t)g_ntables;
}

static bool acpi_enable_sci(void)
{
    if (!acpi_reg_ok(&g_pm1a_reg, g_pm1a_port)) return false;
    if (pm_read(&g_pm1a_reg, g_pm1a_port) & 1u) return true;

    if (g_smi_cmd && g_acpi_enable) outb(g_smi_cmd, g_acpi_enable);

    for (int i = 0; i < 500; i++) {
        if (pm_read(&g_pm1a_reg, g_pm1a_port) & 1u) return true;
        acpi_delay_ms(1);
    }
    acpi_put("ACPI: SCI_EN did not come up\r\n");
    return false;
}

bool acpi_poweroff(void)
{
    if (!g_ready || (g_fadt_flags & (1u << 20))) return false;
    if (!acpi_reg_ok(&g_pm1a_reg, g_pm1a_port)) return false;
    /* Without the firmware's own _S5 numbers, no guessing: on Intel's
       chipsets 5 is S3 (suspend to RAM), not off -- a laptop "switched
       off" that way would not come back on (it resumes into nothing) */
    if (!g_s5_found) {
        acpi_put("ACPI: no _S5 in the tables -- not powering off\r\n");
        return false;
    }

    acpi_enable_sci();

    /* as Linux: the other PM1 control bits (SCI_EN...) kept, the sleep type
       written first, then with SLP_EN */
    const uint32_t mask = (7u << 10) | (1u << 13);
    uint32_t va = pm_read(&g_pm1a_reg, g_pm1a_port) & ~mask;
    uint32_t vb = acpi_reg_ok(&g_pm1b_reg, g_pm1b_port) ? pm_read(&g_pm1b_reg, g_pm1b_port) & ~mask : 0;
    va |= (uint32_t)(g_slp_typa & 7) << 10;
    vb |= (uint32_t)(g_slp_typb & 7) << 10;
    pm_write(&g_pm1a_reg, g_pm1a_port, va);
    if (acpi_reg_ok(&g_pm1b_reg, g_pm1b_port)) pm_write(&g_pm1b_reg, g_pm1b_port, vb);
    pm_write(&g_pm1a_reg, g_pm1a_port, va | (1u << 13));
    if (acpi_reg_ok(&g_pm1b_reg, g_pm1b_port))
        pm_write(&g_pm1b_reg, g_pm1b_port, vb | (1u << 13));

    for (int i = 0; i < 1000; i++) acpi_delay_ms(1);
    return false;                               /* still alive */
}

void acpi_reset(void)
{
    if (g_reset_valid) {
        if (g_reset_space == 1) {
            uint8_t w = g_reset_width ? (uint8_t)(g_reset_width / 8) : 1;
            if (w == 1) outb((uint16_t)g_reset_addr, g_reset_value);
            else if (w == 2) outw((uint16_t)g_reset_addr, g_reset_value);
            else outl((uint16_t)g_reset_addr, g_reset_value);
        } else if (g_reset_space == 0) {
            volatile uint8_t *p = (volatile uint8_t *)vmm_map_mmio(
                g_reset_addr & ~0xFFFULL, 0x1000);
            if (p) p[g_reset_addr & 0xFFF] = g_reset_value;
        }
        acpi_delay_ms(100);
    }
    outb(0xCF9, 0x02);
    acpi_delay_ms(10);
    outb(0xCF9, 0x06);
    acpi_delay_ms(100);
}

bool acpi_pci_cfg_read(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off,
                       uint32_t *out)
{
    if (!g_nmcfg || off > 0xFFF) return false;
    off &= (uint16_t)~3u;
    uint64_t phys;
    if (!ecam_phys(bus, dev, fn, off, &phys)) return false;
    volatile uint8_t *p = ecam_map(phys);
    if (!p) return false;
    *out = *(volatile uint32_t *)p;             /* one 32-bit access, as config space wants */
    return true;
}

bool acpi_pci_cfg_write(uint8_t bus, uint8_t dev, uint8_t fn, uint16_t off,
                        uint32_t val)
{
    if (!g_nmcfg || off > 0xFFF) return false;
    off &= (uint16_t)~3u;
    uint64_t phys;
    if (!ecam_phys(bus, dev, fn, off, &phys)) return false;
    volatile uint8_t *p = ecam_map(phys);
    if (!p) return false;
    *(volatile uint32_t *)p = val;              /* (byte writes would land as four) */
    return true;
}
