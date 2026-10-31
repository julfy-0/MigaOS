/* Общая часть сетевых драйверов: реестр, вывод, PCI-перебор, названия вендоров, PCIe-ссылка. */
#define MIGA_TAG "net"
#include <miga/driver.h>
#include <miga/pci.h>
#include <miga/log.h>
#include <miga/string.h>
#include "net_priv.h"

static struct net_info nets[NET_MAX];
static unsigned nnets;

static const struct { uint16_t id; const char *name; } vendors[] = {
    {0x8086, "Intel"}, {0x10EC, "Realtek"}, {0x14E4, "Broadcom"}, {0x168C, "Qualcomm Atheros"},
    {0x1969, "Qualcomm Atheros"}, {0x17CB, "Qualcomm"}, {0x14C3, "MediaTek"}, {0x11AB, "Marvell"},
    {0x1B4B, "Marvell"}, {0x1AF4, "virtio"}, {0x15B3, "Mellanox"}, {0x1022, "AMD"}, {0x10DE, "NVIDIA"},
    {0x1106, "VIA"}, {0x1D6A, "Aquantia"}, {0x1814, "Ralink"}, {0x10B7, "3Com"}, {0x1186, "D-Link"},
};

unsigned net_count(void) { return nnets; }

int net_get(unsigned idx, struct net_info *out)
{
    if (idx >= nnets) return -1;
    *out = nets[idx];
    return 0;
}

void net_setname(struct net_info *n, const char *s)
{
    unsigned i = 0;
    for (; s[i] && i < sizeof n->name - 1; i++) n->name[i] = s[i];
    n->name[i] = 0;
}

static const char *gts(unsigned s)
{
    switch (s) { case 1: return "2.5"; case 2: return "5"; case 3: return "8"; case 4: return "16"; case 5: return "32"; }
    return "?";
}

static void print_one(const struct net_info *n, int tagged)
{
    const char *t = tagged ? "[net] " : "";
    kprintf("%s%s %02x:%02x.%u %04x:%04x rev %02x: %s %s\n", t, n->kind == NET_WIFI ? "wifi" : "eth ", n->pci_bus, n->pci_dev,
            n->pci_fn, n->vendor, n->device, n->rev, n->vendor_name, n->name[0] ? n->name : "(unknown model)");
    kprintf("%s        ", t);
    if (n->has_mac)
        kprintf("MAC %02x:%02x:%02x:%02x:%02x:%02x", n->mac[0], n->mac[1], n->mac[2], n->mac[3], n->mac[4], n->mac[5]);
    else kprintf("MAC unknown");
    if (n->link == NET_LINK_UP) {
        kprintf(", link up");
        if (n->speed_mbps) kprintf(" %u Mb/s", n->speed_mbps);
    } else if (n->link == NET_LINK_DOWN) kprintf(", link down");
    if (n->link_width) kprintf(", PCIe %s GT/s x%u", gts(n->link_speed), n->link_width);
    if (n->kind == NET_WIFI) kprintf(", no driver (identification only)");
    kprintf("\n");
}

void net_register(const struct net_info *n)
{
    if (nnets >= NET_MAX) return;
    nets[nnets++] = *n;
    print_one(n, 1);
}

void net_print_all(void)
{
    if (!nnets) { kprintf("net: no adapters found (PCI only)\n"); return; }
    for (unsigned i = 0; i < nnets; i++) print_one(&nets[i], 0);
}

void net_fill_common(struct net_info *n, unsigned kind, uint8_t bus, uint8_t dev, uint8_t fn)
{
    memset(n, 0, sizeof *n);
    n->kind = (uint8_t)kind;
    n->pci_bus = bus; n->pci_dev = dev; n->pci_fn = fn;
    uint32_t id = pci_read32(bus, dev, fn, 0), cl = pci_read32(bus, dev, fn, 8), sub = pci_read32(bus, dev, fn, 0x2C);
    n->vendor = id & 0xFFFF; n->device = id >> 16; n->rev = cl & 0xFF;
    n->subvendor = sub & 0xFFFF; n->subdevice = sub >> 16;
    const char *vn = "unknown vendor";
    for (unsigned i = 0; i < sizeof vendors / sizeof *vendors; i++)
        if (vendors[i].id == n->vendor) { vn = vendors[i].name; break; }
    unsigned k = 0;
    for (; vn[k] && k < sizeof n->vendor_name - 1; k++) n->vendor_name[k] = vn[k];
    n->vendor_name[k] = 0;

    if (pci_read32(bus, dev, fn, 0x04) & (1u << 20)) {                       /* Capabilities List */
        uint8_t p = pci_read32(bus, dev, fn, 0x34) & 0xFC;
        for (unsigned i = 0; p && i < 48; i++) {
            uint32_t h = pci_read32(bus, dev, fn, p);
            if ((h & 0xFF) == 0x10) {
                uint32_t cap = pci_read32(bus, dev, fn, p + 0x0C), st = pci_read32(bus, dev, fn, p + 0x10) >> 16;
                n->link_max_speed = cap & 0xF; n->link_max_width = (cap >> 4) & 0x3F;
                n->link_speed = st & 0xF;      n->link_width = (st >> 4) & 0x3F;
                break;
            }
            p = (h >> 8) & 0xFC;
        }
    }
}

unsigned net_pci_scan(unsigned cls, int pif, net_pci_fn fn, void *arg)
{
    unsigned n = 0;
    for (unsigned b = 0; b < 256; b++)
        for (unsigned d = 0; d < 32; d++)
            for (unsigned f = 0; f < 8; f++) {
                if ((pci_read32(b, d, f, 0) & 0xFFFF) == 0xFFFF) { if (f == 0) break; continue; }
                uint32_t c = pci_read32(b, d, f, 8) >> 8;
                uint32_t hdr = (pci_read32(b, d, f, 0x0C) >> 16) & 0x7F;
                if ((c >> 8) == cls && (pif < 0 || (c & 0xFF) == (unsigned)pif) && hdr == 0) { fn(b, d, f, arg); n++; }
                if (f == 0 && !(pci_read32(b, d, f, 0x0C) & 0x00800000)) break;
            }
    return n;
}
