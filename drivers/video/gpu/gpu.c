/* Общая часть GPU-драйверов: перебор PCI, размеры BAR, PCIe-ссылка, реестр и лог.
 * Модесет и ускорение не реализованы: вывод картинки идёт через UEFI-фреймбуфер (fbcon). */
#define MIGA_TAG "gpu"
#include <miga/driver.h>
#include <miga/pci.h>
#include <miga/log.h>
#include <miga/string.h>
#include "gpu_priv.h"

static struct gpu_info gpus[GPU_MAX];
static unsigned ngpus;

unsigned gpu_count(void) { return ngpus; }

int gpu_get(unsigned idx, struct gpu_info *out)
{
    if (idx >= ngpus) return -1;
    *out = gpus[idx];
    return 0;
}

const char *gpu_link_gts(unsigned s)
{
    switch (s) {
    case 1: return "2.5"; case 2: return "5"; case 3: return "8";
    case 4: return "16";  case 5: return "32"; case 6: return "64";
    }
    return "?";
}

/* Размеры BAR определяются записью 0xFFFFFFFF; на это время декодирование памяти/IO выключается. */
static void read_bars(struct gpu_info *g)
{
    uint8_t b = g->bus, d = g->dev, f = g->fn;
    uint32_t cmd = pci_read32(b, d, f, PCI_CMD) & 0xFFFF;   /* статус (старшие 16 бит) не трогаем: RW1C */
    pci_write32(b, d, f, PCI_CMD, cmd & ~7u);

    for (unsigned i = 0; i < GPU_BARS; i++) {
        uint8_t off = (uint8_t)(PCI_BAR0 + 4 * i);
        uint32_t lo = pci_read32(b, d, f, off);
        if (lo & 1) continue;                                /* порт ввода-вывода: не интересует */
        int is64 = ((lo >> 1) & 3) == 2 && i + 1 < GPU_BARS;

        pci_write32(b, d, f, off, 0xFFFFFFFFu);
        uint32_t slo = pci_read32(b, d, f, off);
        pci_write32(b, d, f, off, lo);

        uint64_t base = lo & ~0xFull, mask = slo & ~0xFull;
        if (is64) {
            uint32_t hi = pci_read32(b, d, f, off + 4);
            pci_write32(b, d, f, off + 4, 0xFFFFFFFFu);
            uint32_t shi = pci_read32(b, d, f, off + 4);
            pci_write32(b, d, f, off + 4, hi);
            base |= (uint64_t)hi << 32;
            mask |= (uint64_t)shi << 32;
        } else {
            mask |= 0xFFFFFFFF00000000ull;                   /* 32-битный BAR: старшая часть маски — единицы */
        }
        if (!(slo & ~0xFu) && !(is64 && mask >> 32)) { if (is64) i++; continue; } /* не реализован */

        struct gpu_bar *r = &g->bar[i];
        r->base = base; r->size = ~mask + 1;
        r->prefetch = (lo >> 3) & 1; r->is64 = (uint8_t)is64;
        if (is64) i++;
    }
    pci_write32(b, d, f, PCI_CMD, cmd);
}

static void read_link(struct gpu_info *g)
{
    uint8_t b = g->bus, d = g->dev, f = g->fn;
    if (!(pci_read32(b, d, f, 0x04) & (1u << 20))) return;  /* бит Capabilities List в Status */
    uint8_t p = pci_read32(b, d, f, 0x34) & 0xFC;
    for (unsigned n = 0; p && n < 48; n++) {
        uint32_t h = pci_read32(b, d, f, p);
        if ((h & 0xFF) == 0x10) {                            /* PCI Express capability */
            uint32_t cap = pci_read32(b, d, f, p + 0x0C), st = pci_read32(b, d, f, p + 0x10) >> 16;
            g->link_max_speed = cap & 0xF;  g->link_max_width = (cap >> 4) & 0x3F;
            g->link_speed = st & 0xF;       g->link_width = (st >> 4) & 0x3F;
            return;
        }
        p = (h >> 8) & 0xFC;
    }
}

static void log_gpu(const struct gpu_info *g)
{
    pr_info("%02x:%02x.%u %s %04x:%04x rev %02x: %s (%s)\n", g->bus, g->dev, g->fn, g->vendor_name,
            g->vendor, g->device, g->rev, g->family, g->integrated ? "iGPU" : "dGPU");
    if (g->link_width)
        pr_info("        PCIe %s GT/s x%u (max %s GT/s x%u)\n", gpu_link_gts(g->link_speed), g->link_width,
                gpu_link_gts(g->link_max_speed), g->link_max_width);
    if (g->vram_aperture) pr_info("        VRAM aperture %lu MiB\n", g->vram_aperture >> 20);
    for (unsigned i = 0; i < GPU_BARS; i++)
        if (g->bar[i].size)
            pr_info("        BAR%u %#lx, %lu KiB%s\n", i, g->bar[i].base, g->bar[i].size >> 10,
                    g->bar[i].prefetch ? ", prefetch" : "");
    if (g->chip_id) pr_info("        chip id %#x\n", g->chip_id);
    if (g->boot_fb) pr_info("        boot framebuffer is on this adapter\n");
}

unsigned gpu_scan(uint16_t vendor, const char *vendor_name, gpu_ident_fn ident, const struct boot_info *bi)
{
    unsigned found = 0;
    for (unsigned b = 0; b < 256; b++)
        for (unsigned d = 0; d < 32; d++)
            for (unsigned f = 0; f < 8; f++) {
                uint32_t id = pci_read32(b, d, f, 0);
                if ((id & 0xFFFF) == 0xFFFF) { if (f == 0) break; continue; }
                int multi = (pci_read32(b, d, f, 0x0C) & 0x00800000) != 0;
                uint32_t hdr = (pci_read32(b, d, f, 0x0C) >> 16) & 0x7F;
                uint32_t cl = pci_read32(b, d, f, 8);
                if ((id & 0xFFFF) == vendor && (cl >> 24) == 3 && hdr == 0 && ngpus < GPU_MAX) {
                    struct gpu_info *g = &gpus[ngpus];
                    memset(g, 0, sizeof *g);
                    g->bus = (uint8_t)b; g->dev = (uint8_t)d; g->fn = (uint8_t)f; g->rev = cl & 0xFF;
                    g->vendor = id & 0xFFFF; g->device = id >> 16;
                    g->vendor_name = vendor_name; g->family = "unknown";
                    read_bars(g);
                    read_link(g);
                    for (unsigned i = 0; i < GPU_BARS; i++) {
                        const struct gpu_bar *r = &g->bar[i];
                        if (r->prefetch && r->size > g->vram_aperture) g->vram_aperture = r->size;
                        if (r->size && bi && bi->fb.base && bi->fb.base >= r->base && bi->fb.base < r->base + r->size)
                            g->boot_fb = 1;
                    }
                    if (ident) ident(g);
                    log_gpu(g);
                    ngpus++; found++;
                }
                if (f == 0 && !multi) break;
            }
    return found;
}
