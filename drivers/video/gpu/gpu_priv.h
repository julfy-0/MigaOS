/* Внутренний интерфейс между gpu.c (общий разбор PCI) и драйверами вендоров. */
#pragma once
#include <miga/gpu.h>
#include <miga/boot_info.h>

/* Вендорный хук: заполняет g->family, g->integrated, g->chip_id. BAR и PCIe-ссылка уже разобраны. */
typedef void (*gpu_ident_fn)(struct gpu_info *g);

/* Находит все видеоадаптеры (PCI class 03h) данного вендора, регистрирует их и пишет строку в лог.
 * Возвращает число найденных. */
unsigned gpu_scan(uint16_t vendor, const char *vendor_name, gpu_ident_fn ident, const struct boot_info *bi);
