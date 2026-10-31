/* Ethernet (PCI class 02h/00h): определение модели по ID и чтение MAC/линка там, где регистры известны:
 *  - Intel e1000/e1000e/igb/igc (BAR0 MMIO: RAL/RAH 0x5400, STATUS 0x08)
 *  - Realtek RTL8139/8168/8169/8125 (BAR0 порты ввода-вывода: IDR0 по смещению 0)
 *  - virtio-net legacy (BAR0 порты: MAC по смещению 20)
 * Приём и передача пакетов не реализованы. */
#define MIGA_TAG "eth"
#include <miga/driver.h>
#include <miga/pci.h>
#include <miga/mem.h>
#include <miga/io.h>
#include <miga/log.h>
#include <miga/string.h>
#include "net_priv.h"

static const struct { uint16_t vid, did; const char *name; } known[] = {
    {0x8086, 0x100E, "82540EM Gigabit (e1000)"},    {0x8086, 0x100F, "82545EM Gigabit (e1000)"},
    {0x8086, 0x10D3, "82574L Gigabit (e1000e)"},    {0x8086, 0x1533, "I210 Gigabit (igb)"},
    {0x8086, 0x1539, "I211 Gigabit (igb)"},         {0x8086, 0x15F2, "I225-LM 2.5G (igc)"},
    {0x8086, 0x15F3, "I225-V 2.5G (igc)"},          {0x8086, 0x125B, "I226-LM 2.5G (igc)"},
    {0x8086, 0x125C, "I226-V 2.5G (igc)"},
    {0x10EC, 0x8139, "RTL8139 Fast Ethernet"},      {0x10EC, 0x8168, "RTL8111/8168 Gigabit"},
    {0x10EC, 0x8169, "RTL8169 Gigabit"},            {0x10EC, 0x8125, "RTL8125 2.5G"},
    {0x1AF4, 0x1000, "virtio-net (legacy)"},        {0x1AF4, 0x1041, "virtio-net (modern)"},
};

static const char *lookup(uint16_t v, uint16_t d)
{
    for (unsigned i = 0; i < sizeof known / sizeof *known; i++)
        if (known[i].vid == v && known[i].did == d) return known[i].name;
    return 0;
}

static void intel_regs(struct net_info *n, uint8_t bus, uint8_t dev, uint8_t fn)
{
    uint32_t b0 = pci_read32(bus, dev, fn, PCI_BAR0);
    if (b0 & 1) return;                                                      /* BAR0 не память */
    uint64_t bar = b0 & ~0xFull;
    if (((b0 >> 1) & 3) == 2) bar |= (uint64_t)pci_read32(bus, dev, fn, PCI_BAR0 + 4) << 32;
    if (!bar) return;
    volatile uint32_t *r = mmio_map(bar, 0x6000);
    if (!r) return;
    uint32_t ral = r[0x5400 / 4], rah = r[0x5404 / 4], st = r[0x08 / 4];
    if (ral == 0xFFFFFFFFu) return;                                          /* устройство «спит» */
    if (rah & 0x80000000u) {
        n->has_mac = 1;
        for (unsigned i = 0; i < 4; i++) n->mac[i] = (uint8_t)(ral >> (8 * i));
        n->mac[4] = (uint8_t)rah; n->mac[5] = (uint8_t)(rah >> 8);
    }
    n->link = (st & 2) ? NET_LINK_UP : NET_LINK_DOWN;
    if ((st & 2) && n->device != 0x15F2 && n->device != 0x15F3 && n->device != 0x125B && n->device != 0x125C) {
        unsigned sp = (st >> 6) & 3;
        n->speed_mbps = sp == 0 ? 10 : sp == 1 ? 100 : 1000;
    }
}

static void io_regs(struct net_info *n, uint8_t bus, uint8_t dev, uint8_t fn, unsigned mac_off)
{
    uint32_t b0 = pci_read32(bus, dev, fn, PCI_BAR0);
    if (!(b0 & 1)) return;                                                   /* BAR0 не порты */
    uint16_t base = (uint16_t)(b0 & ~3u);
    uint8_t any = 0;
    for (unsigned i = 0; i < 6; i++) { n->mac[i] = inb((uint16_t)(base + mac_off + i)); any |= n->mac[i]; }
    n->has_mac = any && any != 0xFF;
    if (!n->has_mac) return;
    if (n->device == 0x8139) {                                               /* MSR: бит2 = нет линка, бит3 = 10 Мбит/с */
        uint8_t msr = inb((uint16_t)(base + 0x58));
        n->link = (msr & 4) ? NET_LINK_DOWN : NET_LINK_UP;
        if (!(msr & 4)) n->speed_mbps = (msr & 8) ? 10 : 100;
    } else if (n->device == 0x8168 || n->device == 0x8169) {                 /* PHYstatus */
        uint8_t ph = inb((uint16_t)(base + 0x6C));
        n->link = (ph & 2) ? NET_LINK_UP : NET_LINK_DOWN;
        if (ph & 2) n->speed_mbps = (ph & 0x10) ? 1000 : (ph & 0x08) ? 100 : (ph & 0x04) ? 10 : 0;
    }
}

static unsigned found;

static void eth_one(uint8_t bus, uint8_t dev, uint8_t fn, void *arg)
{
    (void)arg;
    struct net_info n;
    net_fill_common(&n, NET_ETHERNET, bus, dev, fn);
    const char *nm = lookup(n.vendor, n.device);
    net_setname(&n, nm ? nm : "Ethernet controller");

    uint32_t cmd = pci_read32(bus, dev, fn, PCI_CMD) & 0xFFFF;
    if (nm) {
        pci_write32(bus, dev, fn, PCI_CMD, cmd | 3);                         /* I/O + MEM decode: только чтение регистров */
        if (n.vendor == 0x8086) intel_regs(&n, bus, dev, fn);
        else if (n.vendor == 0x10EC) io_regs(&n, bus, dev, fn, 0);
        else if (n.vendor == 0x1AF4 && n.device == 0x1000) io_regs(&n, bus, dev, fn, 20);   /* без MSI-X: конфиг с 20 */
    }
    net_register(&n);
    found++;
}

static int eth_probe(const struct boot_info *bi)
{
    (void)bi;
    net_pci_scan(0x0200, -1, eth_one, 0);
    return found ? 0 : -1;
}

MIGA_DRIVER(ethernet, "ethernet", DRV_VER(0, 1), DRV_NORMAL, eth_probe);
