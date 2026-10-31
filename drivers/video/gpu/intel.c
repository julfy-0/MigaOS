/* Intel (PCI 8086): встроенная графика (iGPU) и дискретные Arc.
 * Определяет поколение по Device ID, отображает MMIO (BAR0), разбирает display engine, читает EDID по DDC.
 * Модесет (смена разрешения/частоты) НЕ реализован: режим выбирает прошивка (GOP). */
#define MIGA_TAG "intel"
#include <miga/driver.h>
#include <miga/log.h>
#include <miga/mem.h>
#include <miga/pci.h>
#include <miga/string.h>
#include "gpu_priv.h"
#include "intel_priv.h"

struct igfx_dev IG;

static void lookup(uint16_t d, const char **name, uint8_t *ver)
{
    uint8_t hi = d >> 8, lo = d & 0xFF;
    switch (hi) {
    case 0x01: if (lo >= 0x50) { *name = "Ivy Bridge (Gen7)"; *ver = 7; } else { *name = "Sandy Bridge (Gen6)"; *ver = 6; } return;
    case 0x04: case 0x0A: case 0x0C: case 0x0D:
        if (d == 0x0A84) { *name = "Broxton (Gen9)"; *ver = 9; } else { *name = "Haswell (Gen7.5)"; *ver = 7; }
        return;
    case 0x0F: *name = "Valleyview (Gen7)"; *ver = 7; return;
    case 0x16: *name = "Broadwell (Gen8)"; *ver = 8; return;
    case 0x22: *name = "Cherryview (Gen8)"; *ver = 8; return;
    case 0x19: *name = "Skylake (Gen9)"; *ver = 9; return;
    case 0x1A: *name = "Broxton (Gen9)"; *ver = 9; return;
    case 0x59: *name = "Kaby Lake (Gen9.5)"; *ver = 9; return;
    case 0x3E: *name = "Coffee Lake (Gen9.5)"; *ver = 9; return;
    case 0x9B: *name = "Comet Lake (Gen9.5)"; *ver = 9; return;
    case 0x5A: if (lo == 0x84 || lo == 0x85) { *name = "Apollo Lake (Gen9)"; *ver = 9; } else { *name = "Cannon Lake (Gen10)"; *ver = 10; } return;
    case 0x31: *name = "Gemini Lake (Gen9.5)"; *ver = 10; return;
    case 0x8A: *name = "Ice Lake (Gen11)"; *ver = 11; return;
    case 0x45: *name = "Elkhart Lake (Gen11)"; *ver = 11; return;
    case 0x9A: *name = "Tiger Lake (Xe-LP)"; *ver = 12; return;
    case 0x4C: *name = "Rocket Lake (Xe-LP)"; *ver = 12; return;
    case 0x49: *name = "DG1 (Xe-LP)"; *ver = 12; return;
    case 0x46: *name = "Alder Lake (Xe-LP)"; *ver = 13; return;
    case 0xA7: *name = "Raptor Lake (Xe-LP)"; *ver = 13; return;
    case 0x56: case 0x4F: *name = "Arc Alchemist (Xe-HPG)"; *ver = 13; return;
    case 0x7D: *name = "Meteor/Arrow Lake (Xe-LPG)"; *ver = 14; return;
    case 0x64: *name = "Lunar Lake (Xe2-LPG)"; *ver = 20; return;
    case 0xE2: *name = "Arc Battlemage (Xe2-HPG)"; *ver = 14; return;
    case 0xB0: case 0xB2: *name = "Panther Lake (Xe3)"; *ver = 30; return;
    }
    *name = "Intel GPU"; *ver = 0;
}

static void ident(struct gpu_info *g)
{
    const char *n; uint8_t v;
    g->integrated = (g->bus == 0 && g->dev == 2);
    lookup(g->device, &n, &v);
    g->family = v ? n : (g->integrated ? "Intel iGPU" : "Intel GPU");
}

static int intel_probe(const struct boot_info *bi)
{
    if (!gpu_scan(0x8086, "Intel", ident, bi)) return -1;

    struct gpu_info g;
    int found = -1;
    for (unsigned i = 0; i < gpu_count(); i++)
        if (!gpu_get(i, &g) && g.vendor == 0x8086 && g.bar[0].size >= 0x100000 && (found < 0 || g.boot_fb)) {
            found = (int)i;
            if (g.boot_fb) break;
        }
    if (found < 0 || gpu_get((unsigned)found, &g)) return 0;

    memset(&IG, 0, sizeof IG);
    IG.bus = g.bus; IG.dev = g.dev; IG.fn = g.fn; IG.did = g.device;
    lookup(g.device, &IG.name, &IG.ver);
    if (IG.ver < 7) { pr_info("%s: display driver supports Gen7 and newer\n", IG.name); return 0; }

    uint32_t cmd = pci_read32(g.bus, g.dev, g.fn, PCI_CMD) & 0xFFFF;
    if (!(cmd & 2)) pci_write32(g.bus, g.dev, g.fn, PCI_CMD, cmd | 2);   /* memory space */

    uint64_t size = g.bar[0].size > (16ull << 20) ? (16ull << 20) : g.bar[0].size;
    IG.mmio = mmio_map(g.bar[0].base, size);
    if (!IG.mmio) { pr_warn("cannot map MMIO %#lx\n", g.bar[0].base); return 0; }
    IG.mmio_size = size;
    if (igr(0x70008) == 0xFFFFFFFFu && igr(0x60000) == 0xFFFFFFFFu) { pr_warn("display registers read as all-ones (power-gated?)\n"); return 0; }

    if (IG.ver >= 8 && IG.ver <= 12 && g.integrated && size >= (16ull << 20)) {
        uint32_t ggms = (pci_read32(g.bus, g.dev, g.fn, 0x50) >> 6) & 3;    /* GMCH_CTRL[7:6] */
        if (ggms) { IG.gtt = IG.mmio + 0x800000; IG.gtt_bytes = (uint64_t)(1u << ggms) << 20; }
    }
    IG.ok = 1;
    igfx_disp_init();

    if (igfx_ddc_read_edid() == 0 && !bi->edid) display_set_edid(IG.edid, IG.edid_len);   /* EDID от прошивки приоритетнее */

    pr_info("%s: %u active pipe(s)\n", IG.name, IG.nheads);
    for (unsigned i = 0; i < IG.nheads; i++) {
        const struct igfx_head *h = &IG.head[i];
        pr_info("  pipe %c: %ux%u, %u.%03u Hz measured\n", 'A' + (h->pipe & 3), h->hactive, h->vactive, h->hz_milli / 1000, h->hz_milli % 1000);
    }
    return 0;
}

MIGA_DRIVER(intel, "intel", DRV_VER(0, 2), DRV_NORMAL, intel_probe);
