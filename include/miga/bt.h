/* Публичный интерфейс Bluetooth-адаптеров для ядра. Реализация — drivers/bluetooth/.
 * Адаптеры ищутся среди USB-устройств (класс E0h/01h/01h); HCI-обмена пока нет, только идентификация. */
#pragma once
#include <stdint.h>

#define BT_MAX 4

struct bt_info {
    uint8_t  port, speed;              /* порт xHCI, скорость USB (1=FS 2=LS 3=HS 4=SS) */
    uint16_t vid, pid, bcd_usb;
    char     vendor_name[20], name[32];
};

unsigned bt_count(void);
int      bt_get(unsigned idx, struct bt_info *out);
void     bt_print_all(void);
