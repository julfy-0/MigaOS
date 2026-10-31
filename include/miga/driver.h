/* Модель драйверов MigaOS.
 * Драйвер регистрирует себя макросом MIGA_DRIVER(); ядро находит все записи в
 * секции .miga_drivers и вызывает probe() в нужной стадии загрузки.
 * Драйвер видит ТОЛЬКО заголовки из include/miga/ (это его единственный API). */
#pragma once
#include <stdint.h>
#include <miga/boot_info.h>

enum drv_stage {
    DRV_EARLY = 0,  /* до GDT/IDT/PMM: только порты и фреймбуфер (serial, fbcon) */
    DRV_NORMAL = 1, /* после IDT и PMM: IRQ, шины, контроллеры (pit, ps2kbd, xhci) */
    DRV_LATE = 2,   /* после NORMAL: драйверы классов поверх шин (usbhid) */
};

/* Версия драйвера: DRV_VER(1, 5) -> "v01.05" в загрузочном логе. */
#define DRV_VER(major, minor) ((uint16_t)(((major) << 8) | (minor)))

struct miga_driver {
    const char *name;
    uint16_t version;
    int stage;
    int (*probe)(const struct boot_info *bi); /* 0 = успех */
};

/* aligned(_Alignof): иначе GCC выравнивает записи по 32 байта, и обход массива
 * с шагом sizeof(struct miga_driver) в ядре «съезжает». */
#define MIGA_DRIVER(ident, drv_name, drv_ver, drv_stage, probe_fn)         \
    static const struct miga_driver __miga_drv_##ident                      \
        __attribute__((used, section(".miga_drivers"),                      \
                       aligned(_Alignof(struct miga_driver)))) = {          \
            .name = drv_name, .version = drv_ver, .stage = drv_stage,       \
            .probe = probe_fn}
