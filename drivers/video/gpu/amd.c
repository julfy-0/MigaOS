/* AMD/ATI (PCI 1002): поколение определяется эвристически по старшему байту Device ID.
 * Инициализация (ATOMBIOS, PSP, SMU) не реализована. */
#define MIGA_TAG "amd"
#include <miga/driver.h>
#include <miga/log.h>
#include "gpu_priv.h"

static void ident(struct gpu_info *g)
{
    unsigned hi = g->device >> 8;
    g->integrated = (hi == 0x13 || hi == 0x15 || hi == 0x16 || hi == 0x98); /* APU: Kaveri/Raven/Renoir/Kabini */
    switch (hi) {
    case 0x13: case 0x98:                      g->family = "GCN APU (Kaveri/Kabini)"; break;
    case 0x15:                                 g->family = "Vega APU (Raven/Picasso/Cezanne)"; break;
    case 0x16:                                 g->family = "RDNA/Vega APU (Renoir..Raphael)"; break;
    case 0x66: case 0x67: case 0x68: case 0x69: g->family = "GCN (Polaris/Vega/Hawaii)"; break;
    case 0x73:                                 g->family = "RDNA 1/2 (Navi 1x/2x)"; break;
    case 0x74:                                 g->family = "RDNA 3 (Navi 3x)"; break;
    case 0x75:                                 g->family = "RDNA 4 (Navi 4x)"; break;
    default:                                   g->family = "unknown AMD"; break;
    }
}

static int amd_probe(const struct boot_info *bi)
{
    return gpu_scan(0x1002, "AMD", ident, bi) ? 0 : -1;
}

MIGA_DRIVER(amd, "amd", DRV_VER(0, 1), DRV_NORMAL, amd_probe);
