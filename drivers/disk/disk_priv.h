/* Внутренний интерфейс между disk.c (реестр, вывод, PCI-перебор) и драйверами контроллеров. */
#pragma once
#include <miga/disk.h>
#include <miga/boot_info.h>

void disk_register(const struct disk_info *d);                 /* добавляет в реестр и пишет строку в лог */
void disk_text(char *dst, unsigned cap, const char *src, unsigned n);  /* ASCII-строка фикс. длины -> без пробелов по краям */
void disk_delay_us(unsigned us);

/* Перебор PCI-функций с кодом класса cls (class<<8 | subclass) и, если pif >= 0, programming interface. */
typedef void (*disk_pci_fn)(uint8_t bus, uint8_t dev, uint8_t fn, void *arg);
unsigned disk_pci_scan(unsigned cls, int pif, disk_pci_fn fn, void *arg);
