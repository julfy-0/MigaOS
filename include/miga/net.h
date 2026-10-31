/* Публичный интерфейс сетевых адаптеров (Ethernet, Wi-Fi) для ядра. Реализация — drivers/net/.
 * Пока только идентификация: модель, MAC (где читается), состояние линка. Передачи пакетов нет. */
#pragma once
#include <stdint.h>

#define NET_MAX 8
enum net_kind { NET_ETHERNET = 1, NET_WIFI };
enum net_link { NET_LINK_UNKNOWN = 0, NET_LINK_DOWN, NET_LINK_UP };

struct net_info {
    uint8_t  kind;                     /* enum net_kind */
    uint8_t  pci_bus, pci_dev, pci_fn, rev;
    uint16_t vendor, device, subvendor, subdevice;
    uint8_t  has_mac, mac[6];
    uint8_t  link;                     /* enum net_link */
    uint16_t speed_mbps;               /* 0 = неизвестно */
    uint8_t  link_speed, link_width, link_max_speed, link_max_width;   /* PCIe: speed 1=2.5 2=5 3=8 4=16 5=32 GT/s */
    char     vendor_name[20], name[48];
};

unsigned net_count(void);
int      net_get(unsigned idx, struct net_info *out);
void     net_print_all(void);
