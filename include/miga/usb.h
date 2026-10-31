/* Публичный интерфейс USB-стека для ядра (kshell и т.п.): что найдено при перечислении.
 * Реализация — drivers/usb/xhci.c. Если контроллера нет, функции возвращают нули. */
#pragma once
#include <stdint.h>

#define USB_INFO_IFACES 4

struct usb_ctrl_info {
    uint16_t version;                  /* BCD, например 0x0100 = xHCI 1.0 */
    uint8_t  ports, slots;
    uint64_t mmio;                     /* физический адрес регистров */
};

struct usb_dev_info {
    uint8_t  port, slot;
    uint8_t  speed;                    /* 1=FS 2=LS 3=HS 4=SS */
    uint8_t  dev_class, dev_subclass, dev_proto;
    uint16_t vid, pid, bcd_usb;
    uint8_t  nifaces;                  /* всего интерфейсов в конфигурации */
    uint8_t  iface[USB_INFO_IFACES][3];/* class, subclass, protocol первых интерфейсов */
    uint8_t  is_kbd;                   /* есть HID boot-клавиатура */
    uint8_t  is_mouse;                 /* есть HID boot-мышь */
    char     product[32];              /* строка Product (ASCII), может быть пустой */
};

int         usb_ctrl_get(struct usb_ctrl_info *out); /* 0 = контроллер есть, -1 = нет */
unsigned    usb_dev_count(void);
int         usb_dev_get(unsigned idx, struct usb_dev_info *out);
const char *usb_class_name(unsigned cls);
const char *usb_speed_name(unsigned speed);
