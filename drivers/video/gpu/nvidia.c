/* NVIDIA (PCI 10de): определение поколения по регистру PMC_BOOT_0 (BAR0+0x0), как в nouveau.
 * Дальше идентификации не идёт: для инициализации нужна подписанная прошивка (GSP/FWSEC). */
#define MIGA_TAG "nvidia"
#include <miga/driver.h>
#include <miga/pci.h>
#include <miga/mem.h>
#include <miga/log.h>
#include "gpu_priv.h"

static const char *family_of(unsigned chipset)
{
    if (chipset >= 0x1b0) return "Blackwell";
    if (chipset >= 0x190) return "Ada Lovelace";
    if (chipset >= 0x170) return "Ampere";
    if (chipset >= 0x160) return "Turing";
    if (chipset >= 0x140) return "Volta";
    if (chipset >= 0x130) return "Pascal";
    if (chipset >= 0x110) return "Maxwell";
    if (chipset >= 0xe0)  return "Kepler";
    if (chipset >= 0xc0)  return "Fermi";
    if (chipset >= 0x50)  return "Tesla";
    if (chipset >= 0x40)  return "Curie";
    return "pre-Curie";
}

static void ident(struct gpu_info *g)
{
    g->integrated = 0;
    if (!g->bar[0].size || !g->bar[0].base) return;
    uint32_t cmd = pci_read32(g->bus, g->dev, g->fn, PCI_CMD) & 0xFFFF;
    if (!(cmd & 2)) pci_write32(g->bus, g->dev, g->fn, PCI_CMD, cmd | 2); /* включить декодирование MEM, если выключено */
    volatile uint32_t *regs = mmio_map(g->bar[0].base, 0x1000);
    if (!regs) return;
    uint32_t boot0 = regs[0];
    if (boot0 == 0xFFFFFFFFu) return;                                      /* карта спит / BAR не отвечает */
    if (!(boot0 & 0x1F000000u)) {                                          /* новые GPU: чип в PMC_BOOT_42 (0xA00) */
        regs = mmio_map(g->bar[0].base, 0x1000);
        uint32_t b42 = regs[0xA00 / 4];
        if (b42 == 0xFFFFFFFFu || !((b42 >> 20) & 0x1FF)) return;
        g->chip_id = b42;
        g->family = family_of((b42 >> 20) & 0x1FF);
        return;
    }
    g->chip_id = boot0;
    g->family = family_of((boot0 & 0x1FF00000u) >> 20);
}

static int nv_probe(const struct boot_info *bi)
{
    return gpu_scan(0x10DE, "NVIDIA", ident, bi) ? 0 : -1;
}

MIGA_DRIVER(nvidia, "nvidia", DRV_VER(0, 1), DRV_NORMAL, nv_probe);
