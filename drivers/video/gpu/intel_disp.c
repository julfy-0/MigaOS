/* Display engine Intel: разбор pipe/transcoder/plane, частота по счётчику кадров, vblank, GGTT и flip.
 * Весь разбор — только чтение. Запись — только в igfx_flip() (PLANE_SURF + PTE свободных страниц GGTT). */
#define MIGA_TAG "igfx"
#include <miga/log.h>
#include <miga/time.h>
#include <miga/string.h>
#include <miga/pci.h>
#include "intel_priv.h"

#define TRANS_BASE(t)   ((t) == 4 ? 0x6F000u : 0x60000u + 0x1000u * (t))
#define TRANS_HTOTAL(t) (TRANS_BASE(t) + 0x00)
#define TRANS_VTOTAL(t) (TRANS_BASE(t) + 0x0C)
#define PIPECONF(t)     ((t) == 4 ? 0x7F008u : 0x70008u + 0x1000u * (t))
#define PIPESRC(p)      (0x6001Cu + 0x1000u * (p))
#define FRMCOUNT(p)     (0x70040u + 0x1000u * (p))
#define PLANE_CTL(p)    (0x70180u + 0x1000u * (p))
#define PLANE_STRIDE(p) (0x70188u + 0x1000u * (p))
#define PLANE_SURF(p)   (0x7019Cu + 0x1000u * (p))
#define TRANS_DDI_EDP   0x6F400u
#define GFX_FLSH_CNTL   0x101008u
#define GTT_OFF         0x800000u
#define PTE_ADDR_MASK   0x00003FFFFFFFF000ull

static const char *const pipe_name[] = {"A", "B", "C", "D"};

static int gtt_ok(void) { return IG.gtt && IG.ver >= 8 && IG.ver <= 12; }

static uint64_t gtt_pte(uint64_t gaddr)
{
    uint64_t i = gaddr >> 12;
    if (!gtt_ok() || (i + 1) * 8 > IG.gtt_bytes) return 0;
    return *(volatile uint64_t *)(IG.gtt + i * 8);
}

static void measure(struct igfx_head *h)
{
    uint32_t c0 = igr(FRMCOUNT(h->pipe)) & 0xFFFFFF;
    uint64_t t0 = time_now_us(), t1 = t0;
    uint32_t c1 = c0;
    while (t1 - t0 < 100000) {
        t1 = time_now_us();
        c1 = igr(FRMCOUNT(h->pipe)) & 0xFFFFFF;
    }
    uint32_t frames = (c1 - c0) & 0xFFFFFF;
    if (frames && frames < 100 && t1 > t0) h->hz_milli = (uint32_t)((uint64_t)frames * 1000000000ull / (t1 - t0));
}

void igfx_disp_init(void)
{
    unsigned ntr = IG.ver >= 12 ? 4 : 3;
    IG.nheads = 0;
    if (IG.ver < 7) return;
    for (unsigned t = 0; t <= 4 && IG.nheads < IGFX_MAX_HEADS; t++) {
        if (t == 3 && ntr < 4) continue;
        if (t == 4 && (IG.ver < 8 || IG.ver > 11)) continue;           /* transcoder EDP: HSW..ICL */
        uint32_t conf = igr(PIPECONF(t));
        if (conf == 0xFFFFFFFFu || !(conf & (1u << 31))) continue;     /* pipe выключен или регистры недоступны */
        struct igfx_head *h = &IG.head[IG.nheads];
        memset(h, 0, sizeof *h);
        h->trans = (uint8_t)t;
        h->pipe = (uint8_t)t;
        if (t == 4) {
            uint32_t sel = (igr(TRANS_DDI_EDP) >> 12) & 7;
            h->pipe = sel == 5 ? 1 : sel == 6 ? 2 : 0;
        }
        h->enabled = 1;
        uint32_t ht = igr(TRANS_HTOTAL(t)), vt = igr(TRANS_VTOTAL(t));
        h->htotal = (ht >> 16) + 1; h->vtotal = (vt >> 16) + 1;
        h->hactive = (ht & 0xFFFF) + 1; h->vactive = (vt & 0xFFFF) + 1;
        uint32_t src = igr(PIPESRC(h->pipe));
        if (src != 0xFFFFFFFFu) { h->hactive = (src >> 16) + 1; h->vactive = (src & 0xFFFF) + 1; }

        uint32_t pc = igr(PLANE_CTL(h->pipe));
        h->plane_ctl = pc;
        h->plane_on = pc != 0xFFFFFFFFu && (pc >> 31);
        h->tiled = IG.ver >= 9 ? ((pc >> 10) & 7) != 0 : ((pc >> 10) & 1) != 0;
        uint32_t st = igr(PLANE_STRIDE(h->pipe));
        h->stride = IG.ver >= 9 ? (st & 0x7FF) * 64 : (st & 0xFFFFFFC0u);
        h->surf_gtt = igr(PLANE_SURF(h->pipe)) & 0xFFFFF000u;
        uint64_t pte = gtt_pte(h->surf_gtt);
        if (pte & 1) h->surf_phys = pte & PTE_ADDR_MASK;
        if (h->plane_on) measure(h);
        IG.nheads++;
    }
}

int igfx_wait_vblank(unsigned head)
{
    if (!IG.ok || head >= IG.nheads) return -1;
    unsigned p = IG.head[head].pipe;
    uint32_t c0 = igr(FRMCOUNT(p));
    uint64_t t0 = time_now_us();
    while (time_now_us() - t0 < 50000)
        if (igr(FRMCOUNT(p)) != c0) return 0;
    return -1;
}

void igfx_flush(const void *p, size_t n)
{
    const uint8_t *a = (const uint8_t *)((uintptr_t)p & ~63ull), *e = (const uint8_t *)p + n;
    for (; a < e; a += 64) __asm__ volatile("clflush (%0)" ::"r"(a) : "memory");
    __asm__ volatile("mfence" ::: "memory");
}

/* Кэш отображений буферов в GGTT. */
static struct { uint64_t phys, gaddr; size_t pages; } map[4];

static int ggtt_map(uint64_t phys, size_t bytes, uint64_t tmpl, uint64_t *gaddr)
{
    size_t pages = (bytes + 4095) / 4096;
    for (unsigned i = 0; i < 4; i++)
        if (map[i].phys == phys && map[i].pages == pages) { *gaddr = map[i].gaddr; return 0; }
    unsigned slot = 4;
    for (unsigned i = 0; i < 4; i++) if (!map[i].pages) { slot = i; break; }
    if (slot == 4) return -1;
    uint64_t entries = IG.gtt_bytes / 8, run = 0, first = 0;
    for (uint64_t e = (entries / 2 + 15) & ~15ull; e < entries; e++) {   /* ищем свободный диапазон во второй половине, выравнивание 64 КиБ */
        if (*(volatile uint64_t *)(IG.gtt + e * 8) & 1) { run = 0; continue; }
        if (!run) { if (e & 15) continue; first = e; }
        if (++run == pages) break;
    }
    if (run != pages) return -1;
    uint64_t flags = tmpl & ~PTE_ADDR_MASK;
    for (size_t i = 0; i < pages; i++)
        *(volatile uint64_t *)(IG.gtt + (first + i) * 8) = ((phys + i * 4096) & PTE_ADDR_MASK) | flags;
    (void)igr(GTT_OFF);
    igw(GFX_FLSH_CNTL, 1);
    (void)igr(GFX_FLSH_CNTL);
    map[slot].phys = phys; map[slot].pages = pages; map[slot].gaddr = first * 4096;
    *gaddr = first * 4096;
    return 0;
}

int igfx_flip(unsigned head, uint64_t phys, size_t bytes)
{
    if (!IG.ok || !gtt_ok() || head >= IG.nheads) return -1;
    struct igfx_head *h = &IG.head[head];
    if (!h->plane_on || h->tiled || !h->stride || (phys & 0xFFF)) return -2;
    if (bytes < (size_t)h->stride * h->vactive) return -3;
    uint64_t tmpl = gtt_pte(h->surf_gtt);
    if (!(tmpl & 1)) return -4;
    uint64_t g;
    if (ggtt_map(phys, bytes, tmpl, &g)) return -5;
    igw(PLANE_SURF(h->pipe), (uint32_t)g);                 /* запись в PLANE_SURF взводит смену поверхности на ближайшем vblank */
    h->surf_gtt = g; h->surf_phys = phys;
    return 0;
}

unsigned igfx_edid(uint8_t *buf, unsigned max)
{
    unsigned n = IG.edid_len < max ? IG.edid_len : max;
    if (buf && n) memcpy(buf, IG.edid, n);
    return n;
}

void igfx_dump(int with_edid)
{
    if (!IG.ok) { kprintf("igfx: Intel display engine not found\n"); return; }
    kprintf("igfx: %s (%04x), display ver %u, MMIO %lu MiB, GGTT %s\n", IG.name, (unsigned)IG.did, (unsigned)IG.ver,
            IG.mmio_size >> 20, gtt_ok() ? "ok" : "n/a");
    if (!IG.nheads) kprintf("  no active pipes\n");
    for (unsigned i = 0; i < IG.nheads; i++) {
        const struct igfx_head *h = &IG.head[i];
        kprintf("  head %u: pipe %s%s  %ux%u (total %ux%u)  %u.%03u Hz\n", i, pipe_name[h->pipe & 3], h->trans == 4 ? " via EDP" : "",
                h->hactive, h->vactive, h->htotal, h->vtotal, h->hz_milli / 1000, h->hz_milli % 1000);
        kprintf("    plane %s%s, stride %u, surface gtt %#lx phys %#lx, ctl %#x\n", h->plane_on ? "on" : "off",
                h->tiled ? " (tiled)" : "", h->stride, h->surf_gtt, h->surf_phys, h->plane_ctl);
    }
    if (!IG.edid_len) kprintf("  EDID: DDC read failed\n");
    else if (with_edid)
        for (unsigned i = 0; i < IG.edid_len; i += 16) {
            kprintf("  %03x:", i);
            for (unsigned k = 0; k < 16; k++) kprintf(" %02x", IG.edid[i + k]);
            kprintf("\n");
        }
    else kprintf("  EDID: %u bytes read via DDC (`igfx edid` to dump)\n", IG.edid_len);
}

int igfx_present(void) { return IG.ok; }
const char *igfx_name(void) { return IG.ok ? IG.name : ""; }
unsigned igfx_heads(void) { return IG.ok ? IG.nheads : 0; }
int igfx_head_get(unsigned i, struct igfx_head *out)
{
    if (!IG.ok || i >= IG.nheads) return -1;
    *out = IG.head[i];
    return 0;
}
