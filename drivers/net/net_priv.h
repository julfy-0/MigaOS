/* Внутренний интерфейс между net.c (реестр, PCI-перебор, общие поля) и драйверами ethernet.c / wifi.c. */
#pragma once
#include <miga/net.h>

typedef void (*net_pci_fn)(uint8_t bus, uint8_t dev, uint8_t fn, void *arg);
unsigned net_pci_scan(unsigned cls, int pif, net_pci_fn fn, void *arg);

/* Заполняет IDs, ревизию, subsystem, название вендора и PCIe-ссылку; name = "" */
void net_fill_common(struct net_info *n, unsigned kind, uint8_t bus, uint8_t dev, uint8_t fn);
void net_register(const struct net_info *n);
void net_setname(struct net_info *n, const char *s);
