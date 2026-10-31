/* Общая часть дисковых драйверов: реестр, вывод, перебор PCI. */
#define MIGA_TAG "disk"
#include <miga/driver.h>
#include <miga/pci.h>
#include <miga/log.h>
#include <miga/string.h>
#include "disk_priv.h"

static struct disk_info disks[DISK_MAX];
static unsigned ndisks;

unsigned disk_count(void) { return ndisks; }

int disk_get(unsigned idx, struct disk_info *out)
{
    if (idx >= ndisks) return -1;
    *out = disks[idx];
    return 0;
}

const char *disk_kind_name(unsigned k)
{
    switch (k) {
    case DISK_HDD: return "HDD"; case DISK_SSD: return "SSD"; case DISK_NVME: return "NVMe SSD";
    case DISK_OPTICAL: return "optical";
    }
    return "disk";
}

void disk_delay_us(unsigned us) { while (us--) io_wait(); }

void disk_text(char *dst, unsigned cap, const char *src, unsigned n)
{
    unsigned a = 0, b = n;
    while (a < n && src[a] == ' ') a++;
    while (b > a && (src[b - 1] == ' ' || src[b - 1] == 0)) b--;
    unsigned o = 0;
    for (; a < b && o < cap - 1; a++) dst[o++] = (src[a] >= 0x20 && src[a] < 0x7F) ? src[a] : '?';
    dst[o] = 0;
}

static void print_one(const struct disk_info *d, int tagged)
{
    uint64_t bytes = d->sectors * d->sector_size;
    uint64_t gib10 = bytes * 10 >> 30, gb = bytes / 1000000000ULL;
    kprintf("%s%s %02x:%02x.%u #%u: \"%s\"  S/N %s  fw %s\n", tagged ? "[disk] " : "", d->bus, d->pci_bus, d->pci_dev,
            d->pci_fn, d->port, d->model[0] ? d->model : "?", d->serial[0] ? d->serial : "-", d->firmware[0] ? d->firmware : "-");
    kprintf("%s        %s", tagged ? "[disk] " : "", disk_kind_name(d->kind));
    if (d->sectors) kprintf(", %lu.%lu GiB (%lu GB), %u B sectors", gib10 / 10, gib10 % 10, gb, d->sector_size);
    else kprintf(", no media / size unknown");
    if (d->rpm) kprintf(", %u RPM", d->rpm);
    if (d->link_gen) kprintf(", SATA %s Gb/s", d->link_gen == 3 ? "6" : d->link_gen == 2 ? "3" : "1.5");
    if (d->ncq) kprintf(", NCQ");
    if (d->trim) kprintf(", TRIM");
    kprintf("\n");
}

void disk_register(const struct disk_info *d)
{
    if (ndisks >= DISK_MAX) return;
    disks[ndisks++] = *d;
    print_one(d, 1);
}

void disk_print_all(void)
{
    if (!ndisks) { kprintf("disk: no drives found (AHCI/NVMe only)\n"); return; }
    for (unsigned i = 0; i < ndisks; i++) print_one(&disks[i], 0);
}

unsigned disk_pci_scan(unsigned cls, int pif, disk_pci_fn fn, void *arg)
{
    unsigned n = 0;
    for (unsigned b = 0; b < 256; b++)
        for (unsigned d = 0; d < 32; d++)
            for (unsigned f = 0; f < 8; f++) {
                if ((pci_read32(b, d, f, 0) & 0xFFFF) == 0xFFFF) { if (f == 0) break; continue; }
                uint32_t c = pci_read32(b, d, f, 8) >> 8;
                if ((c >> 8) == cls && (pif < 0 || (c & 0xFF) == (unsigned)pif)) { fn(b, d, f, arg); n++; }
                if (f == 0 && !(pci_read32(b, d, f, 0x0C) & 0x00800000)) break;
            }
    return n;
}
