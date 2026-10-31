/* Разбор ACPI без интерпретатора AML: RSDP -> RSDT/XSDT -> FADT, MADT, MCFG, HPET.
 * Наличие батареи, адаптера питания, крышки и EC определяется поиском HID-строк в DSDT/SSDT:
 * это говорит, что платформа их описывает, но не заряд и не то, вставлена ли батарея. */
#include <kernel/kernel.h>
#include <kernel/acpi.h>
#include <miga/string.h>

#define LIMIT (64ULL << 30)
#define MAX_TABLES 48

static struct {
    int valid;
    unsigned rev; char oem[7];
    unsigned ntables; char sig[MAX_TABLES][5];
    int have_fadt; unsigned fadt_rev, profile, sci, boot_arch; uint32_t fadt_flags;
    int have_madt; unsigned ioapics, overrides, pcat; uint32_t lapic_addr; struct { uint32_t addr, gsi; } ioapic[4];
    int have_mcfg; uint64_t ecam; unsigned bus0, bus1;
    uint64_t hpet;
    int battery, ac, lid, ec, pwrbtn, slpbtn;
} a;

static const uint8_t *tab(uint64_t pa, uint64_t len)
{
    return pa && pa + len <= LIMIT ? (const uint8_t *)pa : 0;
}

static int has(const uint8_t *p, uint32_t n, const char *s)
{
    unsigned l = (unsigned)strlen(s);
    for (uint32_t i = 0; i + l <= n; i++)
        if (p[i] == (uint8_t)s[0] && !memcmp(p + i, s, l)) return 1;
    return 0;
}

static void scan_aml(const uint8_t *t, uint32_t len)
{
    if (len < 36 || len > (8u << 20) || !tab((uint64_t)t, len)) return;
    a.battery |= has(t, len, "PNP0C0A");
    a.ac      |= has(t, len, "ACPI0003");
    a.lid     |= has(t, len, "PNP0C0D");
    a.ec      |= has(t, len, "PNP0C09");
    a.pwrbtn  |= has(t, len, "PNP0C0C");
    a.slpbtn  |= has(t, len, "PNP0C0E");
}

static void parse_fadt(const uint8_t *t, uint32_t len)
{
    a.have_fadt = 1; a.fadt_rev = t[8];
    if (len >= 48) { a.profile = t[45]; a.sci = *(const uint16_t *)(t + 46); }
    if (len >= 116) { a.boot_arch = *(const uint16_t *)(t + 109); a.fadt_flags = *(const uint32_t *)(t + 112); }
    uint64_t dsdt = len >= 44 ? *(const uint32_t *)(t + 40) : 0;
    if (len >= 148 && *(const uint64_t *)(t + 140)) dsdt = *(const uint64_t *)(t + 140);
    const uint8_t *d = tab(dsdt, 36);
    if (d) scan_aml(d, *(const uint32_t *)(d + 4));
}

static void parse_madt(const uint8_t *t, uint32_t len)
{
    if (len < 44) return;
    a.have_madt = 1; a.lapic_addr = *(const uint32_t *)(t + 36); a.pcat = *(const uint32_t *)(t + 40) & 1;
    for (uint32_t e = 44; e + 2 <= len && t[e + 1]; e += t[e + 1]) {
        if (t[e] == 1 && t[e + 1] >= 12) {
            if (a.ioapics < 4) { a.ioapic[a.ioapics].addr = *(const uint32_t *)(t + e + 4); a.ioapic[a.ioapics].gsi = *(const uint32_t *)(t + e + 8); }
            a.ioapics++;
        } else if (t[e] == 2) a.overrides++;
    }
}

void acpi_init(const struct boot_info *bi)
{
    const uint8_t *rsdp = tab(bi->rsdp, 36);
    if (!rsdp || memcmp(rsdp, "RSD PTR ", 8)) return;
    a.valid = 1; a.rev = rsdp[15];
    for (unsigned i = 0; i < 6; i++) a.oem[i] = (rsdp[9 + i] >= 0x20 && rsdp[9 + i] < 0x7F) ? (char)rsdp[9 + i] : '?';

    int xs = rsdp[15] >= 2;
    uint64_t root = xs ? *(const uint64_t *)(rsdp + 24) : *(const uint32_t *)(rsdp + 16);
    const uint8_t *r = tab(root, 36);
    if (!r) return;
    uint32_t rlen = *(const uint32_t *)(r + 4);
    if (rlen < 36 || !tab(root, rlen)) return;
    unsigned esz = xs ? 8 : 4;

    for (uint32_t o = 36; o + esz <= rlen; o += esz) {
        uint64_t pa = xs ? *(const uint64_t *)(r + o) : *(const uint32_t *)(r + o);
        const uint8_t *t = tab(pa, 36);
        if (!t) continue;
        uint32_t len = *(const uint32_t *)(t + 4);
        if (len < 36 || !tab(pa, len)) continue;
        if (a.ntables < MAX_TABLES) { memcpy(a.sig[a.ntables], t, 4); a.sig[a.ntables][4] = 0; a.ntables++; }

        if (!memcmp(t, "FACP", 4)) parse_fadt(t, len);
        else if (!memcmp(t, "APIC", 4)) parse_madt(t, len);
        else if (!memcmp(t, "SSDT", 4)) scan_aml(t, len);
        else if (!memcmp(t, "MCFG", 4) && len >= 60) {
            a.have_mcfg = 1; a.ecam = *(const uint64_t *)(t + 44); a.bus0 = t[54]; a.bus1 = t[55];
        } else if (!memcmp(t, "HPET", 4) && len >= 44 && t[40] == 0) a.hpet = *(const uint64_t *)(t + 44);  /* GAS: space 0 = память */
    }
}

uint64_t acpi_hpet_base(void) { return a.hpet; }

static const char *profile_name(unsigned p)
{
    static const char *n[] = {"unspecified", "desktop", "mobile (laptop)", "workstation", "enterprise server",
                              "SOHO server", "appliance PC", "performance server", "tablet"};
    return p < 9 ? n[p] : "?";
}

static const char *yn(int v) { return v ? "yes" : "no"; }

void acpi_print(void)
{
    kprintf("\n:: acpi\n");
    if (!a.valid) { kprintf("  not available (no RSDP)\n"); return; }
    kprintf("  revision %u, OEM %s, %u table(s):", a.rev, a.oem, a.ntables);
    unsigned col = 0;
    for (unsigned i = 0; i < a.ntables; i++) {
        if (col >= 64) { kprintf("\n   "); col = 0; }
        kprintf(" %s", a.sig[i]);
        col += 5;
    }
    kprintf("\n");
    if (a.have_fadt)
        kprintf("  FADT rev %u: profile %s, SCI IRQ %u, 8042 %s, hardware-reduced %s\n", a.fadt_rev, profile_name(a.profile), a.sci,
                yn(a.boot_arch & 2), yn(a.fadt_flags >> 20 & 1));
    if (a.have_madt) {
        kprintf("  MADT: LAPIC at %#x, %u IOAPIC(s)", a.lapic_addr, a.ioapics);
        for (unsigned i = 0; i < a.ioapics && i < 4; i++) kprintf(" [%#x, GSI base %u]", a.ioapic[i].addr, a.ioapic[i].gsi);
        kprintf(", %u IRQ override(s), 8259 compatible %s\n", a.overrides, yn(a.pcat));
    }
    if (a.have_mcfg) kprintf("  MCFG: PCIe ECAM at %#lx, buses %u-%u\n", a.ecam, a.bus0, a.bus1);
    if (a.hpet) kprintf("  HPET table: block at %#lx\n", a.hpet);
    kprintf("  power devices in AML: battery %s, AC adapter %s, lid %s, power button %s, sleep button %s, EC %s\n", yn(a.battery),
            yn(a.ac), yn(a.lid), yn(a.pwrbtn), yn(a.slpbtn), yn(a.ec));
    kprintf("  (charge level needs an AML interpreter: not implemented)\n");
}
