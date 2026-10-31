/* Внутренний интерфейс между xhci.c (контроллер + перечисление) и usbhid.c (класс HID).
 * Не часть include/miga/: используется только драйверами USB. */
#pragma once
#include <stdint.h>

#define USB_MAX_HID 8

enum { HID_KBD = 1, HID_MOUSE = 2 };                  /* протокол boot-интерфейса (bInterfaceProtocol) */

typedef void (*usb_hid_cb)(unsigned idx, const uint8_t *report, unsigned len);

unsigned xhci_hid_count(void);                        /* сколько HID boot-устройств (клавиатур и мышей) найдено */
unsigned xhci_hid_kind(unsigned idx);                 /* HID_KBD / HID_MOUSE */
int      xhci_hid_start(unsigned idx, usb_hid_cb cb); /* настроить endpoint и начать приём отчётов */
void     xhci_poll(void);                             /* разобрать event ring (вызывается из input_poll) */
