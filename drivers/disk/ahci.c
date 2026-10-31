/* AHCI (SATA): на каждом порту с подключённым устройством выполняется IDENTIFY (PACKET) DEVICE по опросу.
 * Прерывания не используются, чтение/запись секторов не реализованы. */
#define MIGA_TAG "ahci"
#include <miga/driver.h>
#include <miga/pci.h>
#include <miga/mem.h>
#include <miga/log.h>
#include <miga/string.h>
#include "disk_priv.h"

#define HBA_CAP  (0x00 / 4)
#define HBA_GHC  (0x04 / 4)
#define HBA_PI   (0x0C / 4)
#define HBA_CAP2 (0x24 / 4)
#define HBA_BOHC (0x28 / 4)
#define PORT(n)  (0x100 / 4 + (n) * 0x80 / 4)
#define P_CLB 0
#define P_CLBU 1
#define P_FB 2
#define P_FBU 3
#define P_IS 4
#define P_IE 5
#define P_CMD 6
#define P_TFD 8
#define P_SIG 9
#define P_SSTS 10
#define P_SERR 12
#define P_CI 14
#define CMD_ST  0x0001u
#define CMD_FRE 0x0010u
#define CMD_FR  0x4000u
#define CMD_CR  0x8000u

static int wait_clear(volatile uint32_t *r, uint32_t mask, unsigned ms)
{
    for (unsigned i = 0; i < ms * 10; i++) { if (!(*r & mask)) return 0; disk_delay_us(100); }
    return -1;
}

static int port_stop(volatile uint32_t *p)
{
    p[P_CMD] &= ~CMD_ST;
    if (wait_clear(&p[P_CMD], CMD_CR, 500)) return -1;
    p[P_CMD] &= ~CMD_FRE;
    return wait_clear(&p[P_CMD], CMD_FR, 500);
}

/* cmd: 0xEC IDENTIFY DEVICE или 0xA1 IDENTIFY PACKET DEVICE. Результат: 256 слов в data. */
static int port_identify(volatile uint32_t *p, uint8_t cmd, uint8_t *mem, uint16_t *data, int s64)
{
    uint64_t clb = (uint64_t)mem, fb = clb + 0x400, ct = clb + 0x500, buf = (uint64_t)data;
    if (!s64 && (clb >> 32 || buf >> 32)) return -2;
    if (port_stop(p)) return -3;

    memset(mem, 0, 0x600);
    memset(data, 0, 512);
    p[P_CLB] = (uint32_t)clb; p[P_CLBU] = (uint32_t)(clb >> 32);
    p[P_FB] = (uint32_t)fb;   p[P_FBU] = (uint32_t)(fb >> 32);
    p[P_SERR] = 0xFFFFFFFFu; p[P_IS] = 0xFFFFFFFFu; p[P_IE] = 0;

    uint32_t *h = (uint32_t *)clb;                                  /* слот 0: заголовок команды */
    h[0] = 5 | (1u << 16);                                          /* CFL = 5 dword, PRDTL = 1, чтение */
    h[2] = (uint32_t)ct; h[3] = (uint32_t)(ct >> 32);
    uint8_t *cfis = (uint8_t *)ct;
    cfis[0] = 0x27; cfis[1] = 0x80; cfis[2] = cmd;                  /* Register H2D, бит C, команда */
    uint32_t *prd = (uint32_t *)(ct + 0x80);
    prd[0] = (uint32_t)buf; prd[1] = (uint32_t)(buf >> 32); prd[3] = 511;

    if (wait_clear(&p[P_TFD], 0x88, 1000)) return -4;               /* BSY|DRQ */
    p[P_CMD] |= CMD_FRE;
    p[P_CMD] |= CMD_ST;
    __asm__ volatile("" ::: "memory");
    p[P_CI] = 1;

    int rc = -5;
    for (unsigned i = 0; i < 30000; i++) {                          /* до ~3 с */
        if (!(p[P_CI] & 1)) { rc = (p[P_TFD] & 1) ? -6 : 0; break; }
        if (p[P_IS] & (1u << 30)) { rc = -6; break; }               /* Task File Error */
        disk_delay_us(100);
    }
    port_stop(p);
    return rc;
}

static void identify_fill(struct disk_info *di, const uint16_t *id, int atapi)
{
    char t[41];
    for (unsigned i = 0; i < 20; i++) { t[2 * i] = (char)(id[27 + i] >> 8); t[2 * i + 1] = (char)id[27 + i]; }
    disk_text(di->model, sizeof di->model, t, 40);
    for (unsigned i = 0; i < 10; i++) { t[2 * i] = (char)(id[10 + i] >> 8); t[2 * i + 1] = (char)id[10 + i]; }
    disk_text(di->serial, sizeof di->serial, t, 20);
    for (unsigned i = 0; i < 4; i++) { t[2 * i] = (char)(id[23 + i] >> 8); t[2 * i + 1] = (char)id[23 + i]; }
    disk_text(di->firmware, sizeof di->firmware, t, 8);

    if (atapi) { di->kind = DISK_OPTICAL; di->sector_size = 2048; return; }

    di->sector_size = 512;
    if ((id[106] & 0xC000) == 0x4000 && (id[106] & 0x1000))        /* логический сектор больше 512 */
        di->sector_size = ((uint32_t)id[117] | (uint32_t)id[118] << 16) * 2;
    di->sectors = (id[83] & 0x400)
        ? (uint64_t)id[100] | (uint64_t)id[101] << 16 | (uint64_t)id[102] << 32 | (uint64_t)id[103] << 48
        : (uint64_t)id[60] | (uint64_t)id[61] << 16;
    unsigned rot = id[217];
    if (rot == 1) di->kind = DISK_SSD;
    else if (rot > 1 && rot != 0xFFFF) { di->kind = DISK_HDD; di->rpm = (uint16_t)rot; }
    di->ncq = (id[76] >> 8) & 1;
    di->trim = id[169] & 1;
}

static unsigned found_ctrl, found_disks;

static void ahci_one(uint8_t bus, uint8_t dev, uint8_t fn, void *arg)
{
    (void)arg;
    uint32_t cmd = pci_read32(bus, dev, fn, PCI_CMD) & 0xFFFF;
    pci_write32(bus, dev, fn, PCI_CMD, cmd | 6);                    /* MEM + bus master */
    uint64_t abar = pci_read32(bus, dev, fn, 0x24) & ~0xFull;      /* BAR5 */
    if (!abar) return;
    volatile uint32_t *h = mmio_map(abar, 0x2000);
    if (!h) return;
    found_ctrl++;

    h[HBA_GHC] |= 1u << 31;                                         /* AHCI Enable */
    uint32_t cap = h[HBA_CAP], pi = h[HBA_PI];
    if ((h[HBA_CAP2] & 1) && (h[HBA_BOHC] & 1)) {                   /* BIOS Owned: просим управление */
        h[HBA_BOHC] |= 2;
        for (unsigned i = 0; i < 2000 && (h[HBA_BOHC] & 1); i++) disk_delay_us(1000);
    }
    uint32_t vs = h[0x10 / 4];
    unsigned nports = 0;
    for (unsigned k = 0; k < 32; k++) nports += pi >> k & 1;
    pr_info("%02x:%02x.%u AHCI %x.%x: %u port(s) implemented%s\n", bus, dev, fn, vs >> 16, (vs >> 8) & 0xFF,
            nports, (cap >> 31) ? ", 64-bit DMA" : "");

    uint8_t *mem = dma_alloc_page();
    uint16_t *data = dma_alloc_page();
    if (!mem || !data) return;

    for (unsigned n = 0; n < 32; n++) {
        if (!(pi >> n & 1)) continue;
        volatile uint32_t *p = h + PORT(n);
        uint32_t ssts = p[P_SSTS];
        if ((ssts & 0xF) != 3) continue;                            /* нет устройства / нет связи */
        uint32_t sig = p[P_SIG];
        int atapi = sig == 0xEB140101u;
        if (sig != 0x00000101u && !atapi) { pr_info("port %u: device with signature %#x skipped\n", n, sig); continue; }

        int rc = port_identify(p, atapi ? 0xA1 : 0xEC, mem, data, (cap >> 31) & 1);
        if (rc) { pr_info("port %u: IDENTIFY failed (%d)\n", n, rc); continue; }

        struct disk_info di;
        memset(&di, 0, sizeof di);
        memcpy(di.bus, atapi ? "SATAPI" : "SATA", atapi ? 7 : 5);
        di.pci_bus = bus; di.pci_dev = dev; di.pci_fn = fn; di.port = (uint8_t)n;
        di.link_gen = (ssts >> 4) & 0xF;
        identify_fill(&di, data, atapi);
        disk_register(&di);
        found_disks++;
    }
}

static int ahci_probe(const struct boot_info *bi)
{
    (void)bi;
    disk_pci_scan(0x0106, 0x01, ahci_one, 0);                       /* SATA controller, AHCI 1.0 */
    return found_ctrl ? 0 : -1;
}

MIGA_DRIVER(ahci, "ahci", DRV_VER(0, 1), DRV_NORMAL, ahci_probe);
