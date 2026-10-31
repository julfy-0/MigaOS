/* Публичный интерфейс накопителей для ядра (kshell и т.п.). Реализация — drivers/disk/.
 * Пока только идентификация (модель, серийный номер, объём, тип); чтения секторов нет. */
#pragma once
#include <stdint.h>

#define DISK_MAX 16

enum disk_kind { DISK_UNKNOWN = 0, DISK_HDD, DISK_SSD, DISK_NVME, DISK_OPTICAL };

struct disk_info {
    char     bus[8];                   /* "SATA", "SATAPI", "NVMe" */
    char     model[41], serial[21], firmware[9];
    uint8_t  kind;                     /* enum disk_kind */
    uint8_t  pci_bus, pci_dev, pci_fn;
    uint8_t  port;                     /* номер порта AHCI; для NVMe — nsid */
    uint8_t  link_gen;                 /* SATA: 1/2/3 = 1.5/3/6 Гбит/с */
    uint8_t  ncq, trim;
    uint16_t rpm;                      /* HDD: обороты, 0 = неизвестно */
    uint32_t sector_size;
    uint64_t sectors;                  /* 0 = неизвестно (например, пустой привод) */
};

unsigned    disk_count(void);
int         disk_get(unsigned idx, struct disk_info *out);
void        disk_print_all(void);      /* вывод списка в консоль */
const char *disk_kind_name(unsigned kind);
