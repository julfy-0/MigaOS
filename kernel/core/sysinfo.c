/* CPU: CPUID (+ MADT для числа логических процессоров). RAM: карта памяти UEFI + модули из SMBIOS (тип 16/17). */
#include <kernel/kernel.h>
#include <kernel/sysinfo.h>
#include <miga/string.h>
#include <miga/time.h>

#define MAX_DIMMS 16
#define LIMIT (64ULL << 30)                       /* identity map ядра: до 64 ГиБ */

/* ------------------------------------------------------------------ CPU */
static struct {
    char vendor[13], brand[49], hv[13];
    uint32_t max_leaf, max_ext;
    unsigned family, model, stepping;
    unsigned threads_per_core, cores, logical_pkg, acpi_cpus;
    unsigned base_mhz, max_mhz, bus_mhz;
    unsigned phys_bits, virt_bits;
    uint32_t f[6];                                /* 1.edx 1.ecx 7.ebx 7.ecx ext.edx ext.ecx */
    uint32_t cache[4][2];                         /* [L1d, L1i, L2, L3] -> размер КиБ, число экземпляров не знаем */
    int hypervisor, have_cache, intel;
    uint32_t leaf6a, leaf6c;                      /* CPUID.6: EAX bit0 = DTS, ECX bit0 = APERF/MPERF */
    unsigned tsc_cpuid_mhz;                       /* CPUID.15h, 0 = не сообщается */
} cpu;

static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4])
{
    __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub));
}

static void put4(char *d, uint32_t v) { for (int i = 0; i < 4; i++) d[i] = (char)(v >> (8 * i)); }

struct feat { uint8_t reg, bit; const char *name; };
static const struct feat feats[] = {
    {0, 4, "tsc"}, {0, 6, "pae"}, {0, 9, "apic"}, {0, 23, "mmx"}, {0, 25, "sse"}, {0, 26, "sse2"}, {0, 28, "htt"},
    {1, 0, "sse3"}, {1, 1, "pclmul"}, {1, 5, "vmx"}, {1, 9, "ssse3"}, {1, 12, "fma"}, {1, 19, "sse4.1"},
    {1, 20, "sse4.2"}, {1, 21, "x2apic"}, {1, 22, "movbe"}, {1, 23, "popcnt"}, {1, 25, "aes"}, {1, 26, "xsave"},
    {1, 28, "avx"}, {1, 29, "f16c"}, {1, 30, "rdrand"},
    {2, 3, "bmi1"}, {2, 5, "avx2"}, {2, 8, "bmi2"}, {2, 16, "avx512f"}, {2, 18, "rdseed"}, {2, 29, "sha"},
    {4, 11, "syscall"}, {4, 20, "nx"}, {4, 26, "1gb-pages"}, {4, 27, "rdtscp"}, {4, 29, "lm"},
    {5, 2, "svm"}, {5, 5, "abm"},
};

static void cache_scan(uint32_t leaf)
{
    for (uint32_t sub = 0; sub < 16; sub++) {
        uint32_t r[4];
        cpuid(leaf, sub, r);
        unsigned type = r[0] & 0x1F, level = (r[0] >> 5) & 7;
        if (!type) break;
        uint32_t ways = (r[1] >> 22) + 1, parts = ((r[1] >> 12) & 0x3FF) + 1, line = (r[1] & 0xFFF) + 1, sets = r[2] + 1;
        uint32_t kib = ways * parts * line * sets / 1024;
        int slot = level == 1 ? (type == 2 ? 1 : 0) : level == 2 ? 2 : level == 3 ? 3 : -1;
        if (slot >= 0) { cpu.cache[slot][0] = kib; cpu.have_cache = 1; }
    }
}

static const uint8_t *rd_tab(uint64_t pa, uint64_t len)
{
    return pa && pa + len <= LIMIT ? (const uint8_t *)pa : 0;
}

static unsigned madt_count(const struct boot_info *bi)
{
    const uint8_t *rsdp = rd_tab(bi->rsdp, 36);
    if (!rsdp || memcmp(rsdp, "RSD PTR ", 8)) return 0;
    int xs = rsdp[15] >= 2;
    uint64_t root = xs ? *(const uint64_t *)(rsdp + 24) : *(const uint32_t *)(rsdp + 16);
    const uint8_t *r = rd_tab(root, 36);
    if (!r) return 0;
    uint32_t rlen = *(const uint32_t *)(r + 4);
    unsigned esz = xs ? 8 : 4;
    if (rlen < 36 || !rd_tab(root, rlen)) return 0;
    for (uint32_t o = 36; o + esz <= rlen; o += esz) {
        uint64_t t = xs ? *(const uint64_t *)(r + o) : *(const uint32_t *)(r + o);
        const uint8_t *h = rd_tab(t, 44);
        if (!h || memcmp(h, "APIC", 4)) continue;
        uint32_t len = *(const uint32_t *)(h + 4);
        if (len < 44 || !rd_tab(t, len)) return 0;
        unsigned n = 0;
        for (uint32_t e = 44; e + 2 <= len && h[e + 1]; e += h[e + 1]) {
            if (h[e] == 0 && h[e + 1] >= 8 && (*(const uint32_t *)(h + e + 4) & 3)) n++;      /* LAPIC: enabled | online capable */
            if (h[e] == 9 && h[e + 1] >= 16 && (*(const uint32_t *)(h + e + 8) & 3)) n++;     /* x2APIC */
        }
        return n;
    }
    return 0;
}

static void cpu_init(const struct boot_info *bi)
{
    uint32_t r[4];
    cpuid(0, 0, r);
    cpu.max_leaf = r[0];
    put4(cpu.vendor, r[1]); put4(cpu.vendor + 4, r[3]); put4(cpu.vendor + 8, r[2]);
    int intel = !memcmp(cpu.vendor, "GenuineIntel", 12);
    cpu.intel = intel;

    cpuid(0x80000000, 0, r);
    cpu.max_ext = r[0] >= 0x80000000 ? r[0] : 0;
    if (cpu.max_ext >= 0x80000004)
        for (uint32_t i = 0; i < 3; i++) {
            cpuid(0x80000002 + i, 0, r);
            for (int k = 0; k < 4; k++) put4(cpu.brand + 16 * i + 4 * k, r[k]);
        }
    const char *b = cpu.brand;
    while (*b == ' ') b++;
    if (b != cpu.brand) memmove(cpu.brand, b, strlen(b) + 1);

    cpuid(1, 0, r);
    unsigned fam = (r[0] >> 8) & 0xF, mod = (r[0] >> 4) & 0xF;
    cpu.stepping = r[0] & 0xF;
    cpu.family = fam == 0xF ? fam + ((r[0] >> 20) & 0xFF) : fam;
    cpu.model = (fam == 6 || fam == 0xF) ? mod | ((r[0] >> 12) & 0xF0) : mod;
    cpu.logical_pkg = (r[3] >> 28 & 1) ? (r[1] >> 16) & 0xFF : 1;
    cpu.f[0] = r[3]; cpu.f[1] = r[2];
    cpu.hypervisor = (r[2] >> 31) & 1;

    if (cpu.max_leaf >= 7) { cpuid(7, 0, r); cpu.f[2] = r[1]; cpu.f[3] = r[2]; }
    if (cpu.max_ext >= 0x80000001) { cpuid(0x80000001, 0, r); cpu.f[4] = r[3]; cpu.f[5] = r[2]; }
    if (cpu.max_ext >= 0x80000008) { cpuid(0x80000008, 0, r); cpu.phys_bits = r[0] & 0xFF; cpu.virt_bits = (r[0] >> 8) & 0xFF; }

    /* топология: leaf 0xB (уровни SMT и core) */
    cpu.threads_per_core = 1; cpu.cores = cpu.logical_pkg;
    if (cpu.max_leaf >= 0xB) {
        uint32_t smt = 0, pkg = 0;
        for (uint32_t sub = 0; sub < 8; sub++) {
            cpuid(0xB, sub, r);
            unsigned type = (r[2] >> 8) & 0xFF;
            if (!type) break;
            if (type == 1) smt = r[1] & 0xFFFF;
            if (type == 2) pkg = r[1] & 0xFFFF;
        }
        if (smt && pkg) { cpu.threads_per_core = smt; cpu.logical_pkg = pkg; cpu.cores = pkg / smt; }
    }

    uint32_t topoext = cpu.max_ext >= 0x80000001 && (cpu.f[5] >> 22 & 1);
    if (intel && cpu.max_leaf >= 4) cache_scan(4);
    else if (topoext) cache_scan(0x8000001D);

    if (intel && cpu.max_leaf >= 0x16) {
        cpuid(0x16, 0, r);
        cpu.base_mhz = r[0] & 0xFFFF; cpu.max_mhz = r[1] & 0xFFFF; cpu.bus_mhz = r[2] & 0xFFFF;
    }
    if (cpu.hypervisor) {
        cpuid(0x40000000, 0, r);
        put4(cpu.hv, r[1]); put4(cpu.hv + 4, r[2]); put4(cpu.hv + 8, r[3]);
    }
    cpu.acpi_cpus = madt_count(bi);
    if (cpu.max_leaf >= 6) { cpuid(6, 0, r); cpu.leaf6a = r[0]; cpu.leaf6c = r[2]; }
    if (cpu.max_leaf >= 0x15) {
        cpuid(0x15, 0, r);                                          /* eax = знаменатель, ebx = числитель, ecx = Гц кристалла */
        if (r[0] && r[1] && r[2]) cpu.tsc_cpuid_mhz = (unsigned)((uint64_t)r[2] * r[1] / r[0] / 1000000);
    }
}

void sysinfo_cpu_print(void)
{
    kprintf("\n:: cpu\n  %s  %s\n", cpu.vendor, cpu.brand[0] ? cpu.brand : "(no brand string)");
    kprintf("  family %u model %u stepping %u\n", cpu.family, cpu.model, cpu.stepping);
    kprintf("  %u core(s) x %u thread(s) per package", cpu.cores, cpu.threads_per_core);
    if (cpu.acpi_cpus) kprintf(", %u logical CPU(s) in system (ACPI)", cpu.acpi_cpus);
    kprintf("\n");
    if (cpu.base_mhz) kprintf("  clock: base %u MHz, max %u MHz, bus %u MHz\n", cpu.base_mhz, cpu.max_mhz, cpu.bus_mhz);
    if (cpu.have_cache) {
        static const char *nm[4] = {"L1d", "L1i", "L2", "L3"};
        kprintf("  cache:");
        for (int i = 0; i < 4; i++) if (cpu.cache[i][0]) kprintf(" %s %u KiB", nm[i], cpu.cache[i][0]);
        kprintf("\n");
    }
    if (cpu.tsc_cpuid_mhz) kprintf("  TSC %u MHz (CPUID 15h)\n", cpu.tsc_cpuid_mhz);
    if (cpu.phys_bits) kprintf("  address bits: %u physical, %u virtual\n", cpu.phys_bits, cpu.virt_bits);
    if (cpu.hypervisor) kprintf("  hypervisor: %s\n", cpu.hv[0] ? cpu.hv : "(unknown)");
    kprintf("  features:");
    unsigned col = 11;
    for (unsigned i = 0; i < sizeof feats / sizeof *feats; i++) {
        if (!(cpu.f[feats[i].reg] >> feats[i].bit & 1)) continue;
        unsigned n = (unsigned)strlen(feats[i].name) + 1;
        if (col + n > 76) { kprintf("\n           "); col = 11; }
        kprintf(" %s", feats[i].name);
        col += n;
    }
    kprintf("\n");
}

static inline uint64_t rdtsc_(void) { uint32_t lo, hi; __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi)); return (uint64_t)hi << 32 | lo; }
static inline uint64_t rdmsr_(uint32_t m) { uint32_t lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m)); return (uint64_t)hi << 32 | lo; }

/* MSR читаем только на «железе»: в виртуалках APERF/MPERF и DTS часто не реализованы, а rdmsr даёт #GP. */
int sysinfo_cpu_live(struct cpu_live *o)
{
    memset(o, 0, sizeof *o);
    o->temp_c = -1000;
    int aperf = !cpu.hypervisor && (cpu.leaf6c & 1);
    int dts   = !cpu.hypervisor && cpu.intel && (cpu.leaf6a & 1);

    uint64_t t0 = time_uptime_ms(), t = t0;
    for (uint64_t spin = 0; t == t0; spin++) { if (spin > 400000000ULL) return -1; t = time_uptime_ms(); }
    uint64_t c0 = rdtsc_(), ms0 = t, a0 = aperf ? rdmsr_(0xE8) : 0, m0 = aperf ? rdmsr_(0xE7) : 0;
    for (uint64_t spin = 0; t < ms0 + 100; spin++) { if (spin > 2000000000ULL) return -1; t = time_uptime_ms(); }
    uint64_t c1 = rdtsc_(), a1 = aperf ? rdmsr_(0xE8) : 0, m1 = aperf ? rdmsr_(0xE7) : 0;

    o->tsc_mhz = (unsigned)((c1 - c0) / ((t - ms0) * 1000));
    if (aperf && m1 > m0) o->eff_mhz = (unsigned)((uint64_t)o->tsc_mhz * (a1 - a0) / (m1 - m0));
    if (dts) {
        uint64_t st = rdmsr_(0x19C);
        if (st >> 31 & 1) {
            unsigned tj = (unsigned)(rdmsr_(0x1A2) >> 16 & 0xFF);
            if (!tj) tj = 100;
            o->tjmax = tj;
            o->temp_c = (int)tj - (int)(st >> 16 & 0x7F);
            o->throttled = (unsigned)(st & 1);
        }
    }
    return 0;
}

unsigned sysinfo_tsc_mhz(void)
{
    struct cpu_live l;
    return sysinfo_cpu_live(&l) ? 0 : l.tsc_mhz;
}

/* ------------------------------------------------------------------ RAM */
static struct {
    uint64_t by_type[8];                          /* байт по enum mem_type */
    uint64_t top;                                 /* максимальный адрес ОЗУ-подобных областей */
    int have_smbios; unsigned smbios_major, smbios_minor;
    unsigned slots; uint64_t max_mb; unsigned ecc; int have_array;
    unsigned ndimm, nslots_seen;
    struct dimm { char loc[24], mfr[24], part[24]; uint32_t size_mb; uint16_t speed, cfg; uint8_t type, form; } dimm[MAX_DIMMS];
} ram;

static struct {
    int have_bios, have_sys, have_board, have_chassis;
    char bios_vendor[32], bios_ver[32], bios_date[16];
    unsigned bios_rom_kb, bios_major, bios_minor;
    char sys_mfr[32], sys_product[40], sys_ver[24], sys_serial[32];
    uint8_t uuid[16]; int have_uuid;
    char bd_mfr[32], bd_product[40], bd_ver[24], bd_serial[32];
    unsigned chassis;
} board;

static void sm_str(char *dst, unsigned cap, const uint8_t *s, unsigned formatted, unsigned idx)
{
    dst[0] = 0;
    if (!idx) return;
    const uint8_t *p = s + formatted;
    for (unsigned i = 1; i < idx; i++) { while (*p) p++; p++; if (!*p) return; }
    unsigned n = 0;
    for (; *p && n < cap - 1; p++) dst[n++] = (*p >= 0x20 && *p < 0x7F) ? (char)*p : '?';
    while (n && dst[n - 1] == ' ') n--;
    dst[n] = 0;
}

static void sm_walk(const uint8_t *t, uint64_t len, unsigned maxn)
{
    uint64_t o = 0;
    for (unsigned n = 0; o + 4 <= len && (!maxn || n < maxn); n++) {
        const uint8_t *h = t + o;
        unsigned type = h[0], hl = h[1];
        if (hl < 4) return;
        if (type == 127) return;
        uint64_t e = o + hl;
        while (e + 1 < len && (t[e] || t[e + 1])) e++;              /* конец набора строк: два нуля подряд */
        e += 2;

        if (type == 16 && hl >= 0x0F && !ram.have_array) {
            uint32_t cap = *(const uint32_t *)(h + 7);
            ram.max_mb = cap == 0x80000000u && hl >= 0x17 ? *(const uint64_t *)(h + 0x0F) >> 10 : (uint64_t)cap >> 10;
            ram.slots = *(const uint16_t *)(h + 0x0D); ram.ecc = h[6]; ram.have_array = 1;
        } else if (type == 17 && hl >= 0x15) {
            ram.nslots_seen++;
            uint32_t sz = *(const uint16_t *)(h + 0x0C);
            if (sz && sz != 0xFFFF && ram.ndimm < MAX_DIMMS) {
                struct dimm *d = &ram.dimm[ram.ndimm++];
                d->size_mb = sz == 0x7FFF && hl >= 0x20 ? *(const uint32_t *)(h + 0x1C) & 0x7FFFFFFF
                           : (sz & 0x8000) ? (sz & 0x7FFF) / 1024 : sz;
                d->form = h[0x0E]; d->type = h[0x12];
                d->speed = *(const uint16_t *)(h + 0x15);
                d->cfg = hl >= 0x22 ? *(const uint16_t *)(h + 0x20) : 0;
                sm_str(d->loc, sizeof d->loc, h, hl, h[0x10]);
                sm_str(d->mfr, sizeof d->mfr, h, hl, hl > 0x17 ? h[0x17] : 0);
                sm_str(d->part, sizeof d->part, h, hl, hl > 0x1A ? h[0x1A] : 0);
            }
        }
        if (type == 0 && hl >= 0x12) {
            sm_str(board.bios_vendor, sizeof board.bios_vendor, h, hl, h[4]);
            sm_str(board.bios_ver, sizeof board.bios_ver, h, hl, h[5]);
            sm_str(board.bios_date, sizeof board.bios_date, h, hl, h[8]);
            board.bios_rom_kb = (h[9] + 1u) * 64;
            if (hl >= 0x16 && h[0x14] != 0xFF) { board.bios_major = h[0x14]; board.bios_minor = h[0x15]; }
            board.have_bios = 1;
        } else if (type == 1 && hl >= 8 && !board.have_sys) {
            sm_str(board.sys_mfr, sizeof board.sys_mfr, h, hl, h[4]);
            sm_str(board.sys_product, sizeof board.sys_product, h, hl, h[5]);
            sm_str(board.sys_ver, sizeof board.sys_ver, h, hl, h[6]);
            sm_str(board.sys_serial, sizeof board.sys_serial, h, hl, h[7]);
            if (hl >= 0x18) { memcpy(board.uuid, h + 8, 16); board.have_uuid = 1; }
            board.have_sys = 1;
        } else if (type == 2 && hl >= 8 && !board.have_board) {
            sm_str(board.bd_mfr, sizeof board.bd_mfr, h, hl, h[4]);
            sm_str(board.bd_product, sizeof board.bd_product, h, hl, h[5]);
            sm_str(board.bd_ver, sizeof board.bd_ver, h, hl, h[6]);
            sm_str(board.bd_serial, sizeof board.bd_serial, h, hl, h[7]);
            board.have_board = 1;
        } else if (type == 3 && hl >= 9 && !board.have_chassis) {
            board.chassis = h[5] & 0x7F; board.have_chassis = 1;
        }
        o = e;
    }
}

static void smbios_init(const struct boot_info *bi)
{
    const uint8_t *ep = rd_tab(bi->smbios, 32);
    if (!ep) return;
    if (!memcmp(ep, "_SM3_", 5)) {
        uint64_t addr = *(const uint64_t *)(ep + 16); uint32_t len = *(const uint32_t *)(ep + 12);
        const uint8_t *t = rd_tab(addr, len);
        if (!t) return;
        ram.have_smbios = 1; ram.smbios_major = ep[7]; ram.smbios_minor = ep[8];
        sm_walk(t, len, 0);
    } else if (!memcmp(ep, "_SM_", 4) && !memcmp(ep + 0x10, "_DMI_", 5)) {
        uint32_t addr = *(const uint32_t *)(ep + 0x18); uint16_t len = *(const uint16_t *)(ep + 0x16), cnt = *(const uint16_t *)(ep + 0x1C);
        const uint8_t *t = rd_tab(addr, len);
        if (!t) return;
        ram.have_smbios = 1; ram.smbios_major = ep[6]; ram.smbios_minor = ep[7];
        sm_walk(t, len, cnt);
    }
}

static void ram_init(const struct boot_info *bi)
{
    for (uint64_t i = 0; i < bi->mem_count; i++) {
        const struct mem_region *r = &bi->mem[i];
        if (r->type < 8) ram.by_type[r->type] += r->length;
        if ((r->type == MEM_USABLE || r->type == MEM_LOADER) && r->base + r->length > ram.top) ram.top = r->base + r->length;
    }
    smbios_init(bi);
}

static const char *mem_type_name(unsigned t)
{
    switch (t) {
    case 0x12: return "DDR";    case 0x13: return "DDR2";   case 0x18: return "DDR3";
    case 0x1A: return "DDR4";   case 0x1B: return "LPDDR";  case 0x1C: return "LPDDR2";
    case 0x1D: return "LPDDR3"; case 0x1E: return "LPDDR4"; case 0x22: return "DDR5";
    case 0x23: return "LPDDR5"; case 0x20: return "HBM";    case 0x21: return "HBM2";
    case 0x24: return "HBM3";   case 0x03: return "DRAM";   case 0x07: return "SDRAM";
    }
    return "?";
}

static const char *form_name(unsigned f)
{
    switch (f) {
    case 0x09: return "DIMM"; case 0x0D: return "SODIMM"; case 0x0F: return "FB-DIMM";
    case 0x0B: return "chips"; case 0x10: return "die";
    }
    return "?";
}

static void print_mb(uint64_t mb)
{
    if (mb >= 1024 && !(mb & 1023)) kprintf("%lu GiB", mb >> 10);
    else kprintf("%lu MiB", mb);
}

void sysinfo_ram_print(void)
{
    uint64_t MiB = 1ULL << 20;
    uint64_t usable = ram.by_type[MEM_USABLE] + ram.by_type[MEM_LOADER];
    kprintf("\n:: ram\n  memory map: usable %lu MiB, firmware/reserved %lu MiB, ACPI %lu MiB, MMIO %lu MiB, bad %lu MiB\n",
            usable / MiB, ram.by_type[MEM_RESERVED] / MiB,
            (ram.by_type[MEM_ACPI_RECLAIM] + ram.by_type[MEM_ACPI_NVS]) / MiB, ram.by_type[MEM_MMIO] / MiB,
            ram.by_type[MEM_BAD] / MiB);
    kprintf("  highest RAM address %#lx, PMM: %lu MiB free of %lu MiB tracked\n", ram.top,
            pmm_free_pages() * PAGE_SIZE / MiB, pmm_total_pages() * PAGE_SIZE / MiB);

    if (!ram.have_smbios) { kprintf("  SMBIOS: not available (no module details)\n"); return; }
    kprintf("  SMBIOS %u.%u", ram.smbios_major, ram.smbios_minor);
    if (ram.have_array) {
        static const char *ecc[] = {"?", "other", "unknown", "none", "parity", "single-bit ECC", "multi-bit ECC", "CRC"};
        kprintf(": %u slot(s), max ", ram.slots);
        print_mb(ram.max_mb);
        kprintf(", error correction %s", ram.ecc < 8 ? ecc[ram.ecc] : "?");
    }
    kprintf("\n  modules: %u populated of %u slot(s) reported\n", ram.ndimm, ram.nslots_seen);
    for (unsigned i = 0; i < ram.ndimm; i++) {
        const struct dimm *d = &ram.dimm[i];
        kprintf("    %s: ", d->loc[0] ? d->loc : "slot");
        print_mb(d->size_mb);
        kprintf(" %s %s", mem_type_name(d->type), form_name(d->form));
        if (d->speed) kprintf(" %u MT/s", d->speed);
        if (d->cfg && d->cfg != d->speed) kprintf(" (running %u MT/s)", d->cfg);
        if (d->mfr[0]) kprintf("  %s", d->mfr);
        if (d->part[0]) kprintf(" %s", d->part);
        kprintf("\n");
    }
}

void sysinfo_init(const struct boot_info *bi)
{
    cpu_init(bi);
    ram_init(bi);
}

static const char *chassis_name(unsigned t)
{
    switch (t) {
    case 3: return "desktop";        case 4: return "low-profile desktop"; case 6: return "mini tower";
    case 7: return "tower";          case 8: return "portable";            case 9: return "laptop";
    case 10: return "notebook";      case 11: return "handheld";           case 13: return "all-in-one";
    case 14: return "sub-notebook";  case 17: return "server";             case 23: return "rack-mount";
    case 30: return "tablet";        case 31: return "convertible";        case 32: return "detachable";
    case 35: return "mini PC";       case 36: return "stick PC";
    }
    return "other";
}

void sysinfo_board_print(void)
{
    kprintf("\n:: board\n");
    if (!ram.have_smbios) { kprintf("  SMBIOS: not available\n"); return; }
    if (board.have_bios) {
        kprintf("  BIOS: %s %s (%s), ROM %u KiB", board.bios_vendor, board.bios_ver, board.bios_date, board.bios_rom_kb);
        if (board.bios_major) kprintf(", release %u.%u", board.bios_major, board.bios_minor);
        kprintf("\n");
    }
    if (board.have_sys) {
        kprintf("  system: %s %s, version %s, S/N %s\n", board.sys_mfr, board.sys_product,
                board.sys_ver[0] ? board.sys_ver : "-", board.sys_serial[0] ? board.sys_serial : "-");
        if (board.have_uuid) {
            const uint8_t *u = board.uuid;
            kprintf("  UUID %02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x\n", u[3], u[2], u[1], u[0],
                    u[5], u[4], u[7], u[6], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
        }
    }
    if (board.have_board)
        kprintf("  board: %s %s, version %s, S/N %s\n", board.bd_mfr, board.bd_product,
                board.bd_ver[0] ? board.bd_ver : "-", board.bd_serial[0] ? board.bd_serial : "-");
    if (board.have_chassis) kprintf("  chassis: %s (type %u)\n", chassis_name(board.chassis), board.chassis);
}
