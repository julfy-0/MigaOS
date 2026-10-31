/* EDID 1.x: производитель, модель, размер, родное разрешение и частота. Без драйвера GPU и DDC читать EDID
 * самим нельзя, поэтому берём то, что нашла прошивка (EFI_EDID_ACTIVE/DISCOVERED_PROTOCOL). */
#include <kernel/kernel.h>
#include <kernel/display.h>
#include <miga/string.h>

static const struct boot_info *binfo;
static uint8_t edid[512];
static unsigned edid_len;

static struct {
    int valid, checksum_ok;
    char mfr[4], name[14], serial_str[14];
    unsigned product, week, year, ver, rev, ext;
    uint32_t serial;
    int digital; unsigned bpc;
    unsigned w_mm, h_mm;
    unsigned nat_w, nat_h, hz100;
    unsigned max_hz100, max_w, max_h;     /* самый частый тайминг среди всех блоков EDID */
    unsigned range_max_v;                 /* Range Limits: максимальная вертикальная частота, Гц */
    unsigned ntimings;
} d;
static unsigned hz_override;

static const struct { const char *id, *name; } vendors[] = {
    {"SAM", "Samsung"}, {"GSM", "LG"}, {"LGD", "LG Display"}, {"DEL", "Dell"}, {"AUO", "AU Optronics"},
    {"BOE", "BOE"}, {"ACR", "Acer"}, {"AOC", "AOC"}, {"HWP", "HP"}, {"PHL", "Philips"}, {"BNQ", "BenQ"},
    {"AUS", "ASUS"}, {"VSC", "ViewSonic"}, {"LEN", "Lenovo"}, {"APP", "Apple"}, {"SNY", "Sony"},
    {"CMN", "Chimei Innolux"}, {"IVM", "Iiyama"}, {"NEC", "NEC"}, {"MSI", "MSI"}, {"GBT", "Gigabyte"},
    {"XMI", "Xiaomi"}, {"RTK", "Realtek"}, {"TSB", "Toshiba"}, {"SHP", "Sharp"},
};

static unsigned isqrt(uint64_t v)
{
    uint64_t r = 0, bit = 1ULL << 62;
    while (bit > v) bit >>= 2;
    while (bit) {
        if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return (unsigned)r;
}

static void text_desc(char *dst, const uint8_t *p)
{
    unsigned n = 0;
    for (unsigned i = 5; i < 18 && p[i] != 0x0A && n < 13; i++) dst[n++] = (p[i] >= 0x20 && p[i] < 0x7F) ? (char)p[i] : '?';
    while (n && dst[n - 1] == ' ') n--;
    dst[n] = 0;
}

/* Учитывает тайминг в статистике: частота = pixel_clock / (h_total * v_total). */
static void note_timing(uint64_t clk_hz, unsigned ha, unsigned hb, unsigned va, unsigned vb)
{
    uint64_t total = (uint64_t)(ha + hb) * (va + vb);
    if (!total || !clk_hz) return;
    unsigned hz100 = (unsigned)(clk_hz * 100 / total);
    d.ntimings++;
    if (hz100 > d.max_hz100) { d.max_hz100 = hz100; d.max_w = ha; d.max_h = va; }
}

/* 18-байтовый Detailed Timing Descriptor (базовый блок и CTA-861). */
static void note_dtd(const uint8_t *p)
{
    if (!p[0] && !p[1]) return;
    unsigned ha = p[2] | (p[4] >> 4) << 8, hb = p[3] | (p[4] & 0xF) << 8;
    unsigned va = p[5] | (p[7] >> 4) << 8, vb = p[6] | (p[7] & 0xF) << 8;
    note_timing((uint64_t)(p[0] | p[1] << 8) * 10000, ha, hb, va, vb);
}

/* Блоки расширений: CTA-861 (0x02) — DTD идут после таблицы блоков данных; DisplayID (0x70) — Type I timing (0x03). */
static void parse_extensions(void)
{
    for (unsigned b = 1; b * 128 + 128 <= edid_len && b <= 3; b++) {
        const uint8_t *e = edid + b * 128;
        if (e[0] == 0x02) {
            unsigned off = e[2];
            if (off < 4 || off > 127) continue;
            for (; off + 18 <= 127; off += 18) {
                if (!e[off] && !e[off + 1]) break;
                note_dtd(e + off);
            }
        } else if (e[0] == 0x70) {
            unsigned len = e[2] < 120 ? e[2] : 120, pos = 0;
            const uint8_t *s = e + 5;
            while (pos + 3 <= len) {
                unsigned tag = s[pos], plen = s[pos + 2];
                if (!tag && !plen) break;
                if (pos + 3 + plen > len) break;
                if (tag == 0x03) {
                    for (unsigned o = 0; o + 20 <= plen; o += 20) {
                        const uint8_t *t = s + pos + 3 + o;
                        uint64_t clk = ((uint64_t)(t[0] | t[1] << 8 | t[2] << 16) + 1) * 10000;
                        unsigned ha = (t[4] | t[5] << 8) + 1, hb = (t[6] | t[7] << 8) + 1;
                        unsigned va = (t[12] | t[13] << 8) + 1, vb = (t[14] | t[15] << 8) + 1;
                        note_timing(clk, ha, hb, va, vb);
                    }
                }
                pos += 3 + plen;
            }
        }
    }
}

static void edid_parse(void)
{
    static const uint8_t magic[8] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    if (memcmp(edid, magic, 8)) return;                                  /* не EDID */
    d.valid = 1;
    unsigned sum = 0;
    for (unsigned i = 0; i < 128; i++) sum += edid[i];
    d.checksum_ok = (sum & 0xFF) == 0;

    unsigned c1 = (edid[8] >> 2) & 0x1F, c2 = ((edid[8] & 3) << 3) | (edid[9] >> 5), c3 = edid[9] & 0x1F;
    d.mfr[0] = (char)('A' + c1 - 1); d.mfr[1] = (char)('A' + c2 - 1); d.mfr[2] = (char)('A' + c3 - 1);
    d.product = edid[10] | edid[11] << 8;
    d.serial = edid[12] | edid[13] << 8 | edid[14] << 16 | (uint32_t)edid[15] << 24;
    d.week = edid[16]; d.year = 1990 + edid[17];
    d.ver = edid[18]; d.rev = edid[19];
    d.ext = edid[126];

    d.digital = edid[20] >> 7;
    if (d.digital) { unsigned b = (edid[20] >> 4) & 7; d.bpc = (b && b < 7) ? 4 + 2 * b : 0; }
    d.w_mm = edid[21] * 10u; d.h_mm = edid[22] * 10u;                    /* в см; если 0 — это соотношение сторон */
    if (!edid[21] || !edid[22]) d.w_mm = d.h_mm = 0;

    int have_timing = 0;
    for (unsigned o = 54; o <= 108; o += 18) {
        const uint8_t *p = edid + o;
        if (p[0] || p[1]) {
            if (have_timing) continue;                                   /* первый дескриптор = предпочтительный режим */
            have_timing = 1;
            unsigned ha = p[2] | (p[4] >> 4) << 8, hb = p[3] | (p[4] & 0xF) << 8;
            unsigned va = p[5] | (p[7] >> 4) << 8, vb = p[6] | (p[7] & 0xF) << 8;
            uint64_t clk = (uint64_t)(p[0] | p[1] << 8) * 10, total = (uint64_t)(ha + hb) * (va + vb);
            d.nat_w = ha; d.nat_h = va;
            d.hz100 = total ? (unsigned)(clk * 1000 * 100 / total) : 0;
            unsigned wmm = p[12] | (p[14] >> 4) << 8, hmm = p[13] | (p[14] & 0xF) << 8;
            if (wmm && hmm) { d.w_mm = wmm; d.h_mm = hmm; }
            note_dtd(p);
            continue;
        }
        if (p[3] == 0xFC) text_desc(d.name, p);
        else if (p[3] == 0xFF) text_desc(d.serial_str, p);
        else if (p[3] == 0xFD) {                                         /* Range Limits */
            unsigned vmax = p[6];
            if ((p[4] & 3) >= 2) vmax += 255;                            /* EDID 1.4: смещение максимума на +255 */
            if (vmax > d.range_max_v) d.range_max_v = vmax;
        }
        /* повторные DTD (кроме первого, он уже учтён выше) */
    }
    /* остальные DTD базового блока: первый уже разобран в цикле, поэтому проходим заново только для статистики */
    {
        int first = 1;
        for (unsigned o = 54; o <= 108; o += 18) {
            const uint8_t *p = edid + o;
            if (!(p[0] || p[1])) continue;
            if (first) { first = 0; continue; }
            note_dtd(p);
        }
    }
    parse_extensions();
}

void display_init(const struct boot_info *bi)
{
    binfo = bi;
    if (!bi->edid || bi->edid_size < 128 || bi->edid_size > sizeof edid) return;
    memcpy(edid, (const void *)bi->edid, bi->edid_size);
    edid_len = bi->edid_size;
    edid_parse();
}

/* EDID, который драйвер GPU прочитал сам по DDC (когда прошивка его не передала). */
void display_set_edid(const void *p, unsigned n)
{
    if (n < 128 || n > sizeof edid) return;
    memset(&d, 0, sizeof d);
    memcpy(edid, p, n);
    edid_len = n;
    edid_parse();
}

unsigned display_refresh_hz(void)
{
    if (hz_override) return hz_override;
    if (!d.valid) return 0;
    unsigned hz = (d.max_hz100 + 50) / 100;
    if (d.range_max_v > hz && d.range_max_v < 1000) hz = d.range_max_v;      /* Range Limits обычно честно называет максимум */
    return hz;
}

void display_set_refresh_hz(unsigned hz) { hz_override = hz; }

unsigned display_edid_native(unsigned *w, unsigned *h)
{
    if (!d.valid || !d.nat_w) return 0;
    if (w) *w = d.nat_w;
    if (h) *h = d.nat_h;
    return 1;
}

void display_print(void)
{
    kprintf("\n:: display\n");
    if (binfo && binfo->fb.base)
        kprintf("  current mode: %ux%u 32bpp %s, pitch %u, framebuffer %#lx (%lu KiB)\n", binfo->fb.width, binfo->fb.height,
                binfo->fb.format == FB_RGBX ? "RGBX" : "BGRX", binfo->fb.pitch, binfo->fb.base, binfo->fb.size >> 10);
    else kprintf("  no framebuffer\n");

    if (!d.valid) { kprintf("  EDID: not provided by firmware (no monitor details)\n"); return; }

    const char *vn = 0;
    for (unsigned i = 0; i < sizeof vendors / sizeof *vendors; i++)
        if (!strcmp(vendors[i].id, d.mfr)) { vn = vendors[i].name; break; }
    kprintf("  EDID %u.%u (%u bytes%s): %s%s%s  \"%s\"  product %04x\n", d.ver, d.rev, edid_len,
            d.checksum_ok ? "" : ", BAD CHECKSUM", d.mfr, vn ? " = " : "", vn ? vn : "",
            d.name[0] ? d.name : "no name", d.product);
    kprintf("  serial %08x", (unsigned)d.serial);
    if (d.serial_str[0]) kprintf(" / \"%s\"", d.serial_str);
    if (d.year > 1990) {
        if (d.week && d.week < 54) kprintf(", made week %u of %u", d.week, d.year);
        else kprintf(", made %u", d.year);
    }
    kprintf("\n  %s", d.digital ? "digital" : "analog");
    if (d.bpc) kprintf(" %u bpc", d.bpc);
    if (d.w_mm) {
        unsigned diag10 = isqrt((uint64_t)d.w_mm * d.w_mm + (uint64_t)d.h_mm * d.h_mm) * 100 / 254;
        kprintf(", %ux%u mm (%u.%u\")", d.w_mm, d.h_mm, diag10 / 10, diag10 % 10);
    }
    if (d.nat_w) kprintf(", preferred %ux%u @ %u.%02u Hz", d.nat_w, d.nat_h, d.hz100 / 100, d.hz100 % 100);
    if (d.ext) kprintf(", %u extension block(s)", d.ext);
    kprintf("\n");
    if (d.ntimings || d.range_max_v) {
        kprintf("  timings in EDID: %u", d.ntimings);
        if (d.max_hz100) kprintf(", fastest %ux%u @ %u.%02u Hz", d.max_w, d.max_h, d.max_hz100 / 100, d.max_hz100 % 100);
        if (d.range_max_v) kprintf(", range limit up to %u Hz", d.range_max_v);
        kprintf("\n");
    }
    unsigned hz = display_refresh_hz();
    if (hz) kprintf("  GUI frame rate target: %u Hz%s%s\n", hz, hz_override ? " (set by `hz`)" : "", hz >= 200 ? "  [high refresh]" : "");
}
