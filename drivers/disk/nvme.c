/* NVMe: сброс контроллера, админ-очередь из 4 записей, Identify Controller и Identify Namespace 1 по опросу.
 * Ввод-вывод (очереди IO, чтение/запись) не реализован. */
#define MIGA_TAG "nvme"
#include <miga/driver.h>
#include <miga/pci.h>
#include <miga/mem.h>
#include <miga/log.h>
#include <miga/string.h>
#include "disk_priv.h"

#define R_CC   (0x14 / 4)
#define R_CSTS (0x1C / 4)
#define R_AQA  (0x24 / 4)
#define R_ASQ  (0x28 / 4)
#define R_ACQ  (0x30 / 4)
#define QD 4

struct nvme {
    volatile uint32_t *r;
    uint32_t *sq, *cq;
    volatile uint32_t *db_sq, *db_cq;
    unsigned tail, head, phase;
};

static int wait_rdy(volatile uint32_t *r, uint32_t want, unsigned to_500ms)
{
    if (to_500ms > 40) to_500ms = 40;
    for (unsigned i = 0; i < to_500ms * 500 + 500; i++) {
        uint32_t st = r[R_CSTS];
        if (st & 2) return -1;                                       /* CFS: фатальная ошибка */
        if ((st & 1) == want) return 0;
        disk_delay_us(1000);
    }
    return -2;
}

/* Возвращает 0 при успехе, иначе код статуса NVMe (>0) или -1 при тайм-ауте. */
static int admin(struct nvme *n, uint32_t opc, uint32_t nsid, uint32_t cdw10, uint64_t prp1)
{
    uint32_t *e = n->sq + n->tail * 16;
    memset(e, 0, 64);
    e[0] = opc | ((n->tail + 1) << 16);
    e[1] = nsid;
    e[6] = (uint32_t)prp1; e[7] = (uint32_t)(prp1 >> 32);
    e[10] = cdw10;
    n->tail = (n->tail + 1) % QD;
    __asm__ volatile("" ::: "memory");
    *n->db_sq = n->tail;

    volatile uint32_t *c = n->cq + n->head * 4;
    for (unsigned i = 0; i < 5000; i++) {                            /* до ~5 с */
        uint32_t dw3 = c[3];
        if (((dw3 >> 16) & 1) == n->phase) {
            unsigned status = (dw3 >> 17) & 0x7FFF;
            if (++n->head == QD) { n->head = 0; n->phase ^= 1; }
            *n->db_cq = n->head;
            return (int)status;
        }
        disk_delay_us(1000);
    }
    return -1;
}

static unsigned found;

static void nvme_one(uint8_t bus, uint8_t dev, uint8_t fn, void *arg)
{
    (void)arg;
    uint32_t cmd = pci_read32(bus, dev, fn, PCI_CMD) & 0xFFFF;
    pci_write32(bus, dev, fn, PCI_CMD, cmd | 6);
    uint32_t b0 = pci_read32(bus, dev, fn, PCI_BAR0);
    uint64_t bar = b0 & ~0xFull;
    if (((b0 >> 1) & 3) == 2) bar |= (uint64_t)pci_read32(bus, dev, fn, PCI_BAR0 + 4) << 32;
    if (!bar) return;

    struct nvme n;
    memset(&n, 0, sizeof n);
    n.r = mmio_map(bar, 0x2000);
    if (!n.r) { pr_info("%02x:%02x.%u: BAR0 %#lx not mappable\n", bus, dev, fn, bar); return; }
    found++;

    uint32_t caplo = n.r[0], caphi = n.r[1], vs = n.r[2];
    unsigned dstrd = caphi & 0xF, to = (caplo >> 24) & 0xFF, mpsmin = (caphi >> 16) & 0xF;
    pr_info("%02x:%02x.%u NVMe %u.%u, queue depth max %u\n", bus, dev, fn, vs >> 16, (vs >> 8) & 0xFF, (caplo & 0xFFFF) + 1);
    if (mpsmin) { pr_info("  min page size > 4 KiB, skipped\n"); return; }

    if (n.r[R_CC] & 1) { n.r[R_CC] &= ~1u; if (wait_rdy(n.r, 0, to)) { pr_info("  reset timeout\n"); return; } }

    n.sq = dma_alloc_page(); n.cq = dma_alloc_page();
    uint8_t *buf = dma_alloc_page();
    if (!n.sq || !n.cq || !buf) return;
    unsigned stride = 4u << dstrd;
    n.db_sq = (volatile uint32_t *)((uint8_t *)n.r + 0x1000);
    n.db_cq = (volatile uint32_t *)((uint8_t *)n.r + 0x1000 + stride);
    n.phase = 1;

    n.r[R_AQA] = ((QD - 1) << 16) | (QD - 1);
    n.r[R_ASQ] = (uint32_t)(uint64_t)n.sq; n.r[R_ASQ + 1] = (uint32_t)((uint64_t)n.sq >> 32);
    n.r[R_ACQ] = (uint32_t)(uint64_t)n.cq; n.r[R_ACQ + 1] = (uint32_t)((uint64_t)n.cq >> 32);
    n.r[R_CC] = 1 | (6u << 16) | (4u << 20);                         /* EN, SQ entry 64 B, CQ entry 16 B, страница 4 КиБ */
    if (wait_rdy(n.r, 1, to)) { pr_info("  controller did not become ready\n"); return; }

    int st = admin(&n, 0x06, 0, 1, (uint64_t)buf);                   /* Identify Controller */
    if (st) { pr_info("  Identify Controller failed (%d)\n", st); return; }

    struct disk_info di;
    memset(&di, 0, sizeof di);
    memcpy(di.bus, "NVMe", 5);
    di.pci_bus = bus; di.pci_dev = dev; di.pci_fn = fn; di.kind = DISK_NVME; di.port = 1;
    disk_text(di.serial, sizeof di.serial, (const char *)buf + 4, 20);
    disk_text(di.model, sizeof di.model, (const char *)buf + 24, 40);
    disk_text(di.firmware, sizeof di.firmware, (const char *)buf + 64, 8);
    uint32_t nn = *(uint32_t *)(buf + 516);
    di.trim = (*(uint16_t *)(buf + 520) >> 2) & 1;                   /* ONCS бит 2: Dataset Management */

    memset(buf, 0, 4096);
    st = admin(&n, 0x06, 1, 0, (uint64_t)buf);                       /* Identify Namespace 1 */
    if (!st) {
        uint64_t nsze = *(uint64_t *)buf;
        unsigned idx = buf[26] & 0xF, lbads = buf[128 + idx * 4 + 2];
        di.sectors = nsze;
        di.sector_size = lbads >= 9 && lbads < 32 ? 1u << lbads : 512;
    }
    pr_info("  namespaces: %u\n", nn);
    disk_register(&di);
}

static int nvme_probe(const struct boot_info *bi)
{
    (void)bi;
    disk_pci_scan(0x0108, 0x02, nvme_one, 0);                        /* NVM Express */
    return found ? 0 : -1;
}

MIGA_DRIVER(nvme, "nvme", DRV_VER(0, 1), DRV_NORMAL, nvme_probe);
