/* Wi-Fi (PCI class 02h/80h и 0Dh/2xh): только определение адаптера по ID. Рабочий драйвер требует
 * прошивки производителя и сложного стека 802.11, поэтому здесь его нет. */
#define MIGA_TAG "wifi"
#include <miga/driver.h>
#include <miga/pci.h>
#include <miga/log.h>
#include <miga/string.h>
#include "net_priv.h"

static const struct { uint16_t vid, did; const char *name; } known[] = {
    {0x8086, 0x2723, "Wi-Fi 6 AX200"},     {0x8086, 0x2725, "Wi-Fi 6E AX210"},   {0x8086, 0x272B, "Wi-Fi 7 BE200"},
    {0x8086, 0x24FD, "Wireless-AC 8265"},  {0x8086, 0x24F3, "Wireless-AC 8260"}, {0x8086, 0x2526, "Wireless-AC 9260"},
    {0x8086, 0x31DC, "Wireless-AC 9560"},  {0x8086, 0x9DF0, "Wireless-AC 9560"}, {0x8086, 0x02F0, "Wi-Fi 6 AX201"},
    {0x8086, 0xA0F0, "Wi-Fi 6 AX201"},     {0x8086, 0x43F0, "Wi-Fi 6 AX201"},    {0x8086, 0x7AF0, "Wi-Fi 6E AX211"},
    {0x8086, 0x51F0, "Wi-Fi 6E AX211"},    {0x8086, 0x54F0, "Wi-Fi 6E AX211"},
    {0x14C3, 0x7961, "MT7921"},            {0x14C3, 0x0616, "MT7922"},           {0x14C3, 0x7925, "MT7925"},
    {0x10EC, 0x8821, "RTL8821AE"},         {0x10EC, 0x8822, "RTL8822BE"},        {0x10EC, 0xC822, "RTL8822CE"},
    {0x168C, 0x003E, "QCA6174"},           {0x168C, 0x0042, "QCA9377"},
};

static unsigned found;

static void wifi_one(uint8_t bus, uint8_t dev, uint8_t fn, void *arg)
{
    (void)arg;
    struct net_info n;
    net_fill_common(&n, NET_WIFI, bus, dev, fn);
    const char *nm = "Wireless controller";
    for (unsigned i = 0; i < sizeof known / sizeof *known; i++)
        if (known[i].vid == n.vendor && known[i].did == n.device) { nm = known[i].name; break; }
    net_setname(&n, nm);
    net_register(&n);
    found++;
}

static int wifi_probe(const struct boot_info *bi)
{
    (void)bi;
    net_pci_scan(0x0280, -1, wifi_one, 0);                                   /* «network controller, other» — так выглядит большинство Wi-Fi */
    net_pci_scan(0x0D20, -1, wifi_one, 0);                                   /* wireless: 802.11a */
    net_pci_scan(0x0D21, -1, wifi_one, 0);                                   /* wireless: 802.11b */
    net_pci_scan(0x0D80, -1, wifi_one, 0);                                   /* wireless: other */
    return found ? 0 : -1;
}

MIGA_DRIVER(wifi, "wifi", DRV_VER(0, 1), DRV_NORMAL, wifi_probe);
