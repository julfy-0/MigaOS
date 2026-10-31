/* Драйвер USB HID (boot protocol): клавиатуры (8-байтовые отчёты -> input_push) и мыши
 * (отчёт: кнопки, dx, dy, колесо -> mouse_push). Транспорт — xhci.c, опрос идёт из input_poll()
 * через input_register_poller(): GUI-цикл дёргает его на каждой итерации, т.е. не реже 1 кГц. */
#define MIGA_TAG "usbhid"
#include <miga/driver.h>
#include <miga/input.h>
#include <miga/log.h>
#include "xhci.h"

static uint8_t prev[USB_MAX_HID][8];
static int caps;

static char usage_to_char(uint8_t u, int shift)
{
    if (u >= 0x04 && u <= 0x1D) return (char)((shift ? 'A' : 'a') + (u - 0x04));
    if (u >= 0x1E && u <= 0x27) return shift ? "!@#$%^&*()"[u - 0x1E] : "1234567890"[u - 0x1E];
    if (u >= 0x59 && u <= 0x61) return (char)('1' + (u - 0x59));  /* keypad 1..9 */
    switch (u) {
    case 0x28: case 0x58: return '\n';
    case 0x29: return 27;
    case 0x2A: return '\b';
    case 0x2B: return '\t';
    case 0x2C: return ' ';
    case 0x2D: return shift ? '_' : '-';
    case 0x2E: return shift ? '+' : '=';
    case 0x2F: return shift ? '{' : '[';
    case 0x30: return shift ? '}' : ']';
    case 0x31: return shift ? '|' : '\\';
    case 0x33: return shift ? ':' : ';';
    case 0x34: return shift ? '"' : '\'';
    case 0x35: return shift ? '~' : '`';
    case 0x36: return shift ? '<' : ',';
    case 0x37: return shift ? '>' : '.';
    case 0x38: return shift ? '?' : '/';
    case 0x4A: return (char)0x86;   /* Home */
    case 0x4B: return (char)0x88;   /* Page Up */
    case 0x4C: return (char)0x85;   /* Delete */
    case 0x4D: return (char)0x87;   /* End */
    case 0x4E: return (char)0x89;   /* Page Down */
    case 0x4F: return (char)0x83;   /* стрелки: вправо, влево, вниз, вверх */
    case 0x50: return (char)0x82;
    case 0x51: return (char)0x81;
    case 0x52: return (char)0x80;
    case 0x54: return '/';
    case 0x55: return '*';
    case 0x56: return '-';
    case 0x57: return '+';
    case 0x62: return '0';
    case 0x63: return '.';
    }
    return 0;
}

/* Boot-отчёт мыши: [0] кнопки, [1] dx (int8), [2] dy (int8), [3] колесо (int8, если есть).
 * Мыши с высоким DPI в boot-режиме шлют те же 8 бит: за 1 мс больше ±127 отсчётов не бывает. */
static void mouse_report(unsigned idx, const uint8_t *r, unsigned len)
{
    (void)idx;
    if (len < 3) return;
    int wheel = len >= 4 ? (int8_t)r[3] : 0;
    mouse_push((int8_t)r[1], (int8_t)r[2], wheel, r[0] & 7u);
}

static void hid_report(unsigned idx, const uint8_t *r, unsigned len)
{
    if (xhci_hid_kind(idx) == HID_MOUSE) { mouse_report(idx, r, len); return; }
    if (len < 8) return;
    int shift = (r[0] & 0x22) != 0, ctrl = (r[0] & 0x11) != 0;
    if ((r[0] & 0x88) && !(prev[idx][0] & 0x88)) input_push((char)0x84);    /* Win/Super (GUI_KEY_SUPER): по фронту нажатия */

    for (int i = 2; i < 8; i++) {
        uint8_t k = r[i];
        if (k < 0x04) continue;                                   /* 0 = пусто, 1..3 = ошибки/rollover */
        int was = 0;
        for (int j = 2; j < 8; j++) if (prev[idx][j] == k) was = 1;
        if (was) continue;                                        /* клавиша уже была нажата */

        if (k == 0x39) { caps = !caps; continue; }                /* Caps Lock */
        char c = usage_to_char(k, shift);
        if (!c) continue;
        if (caps && c >= 'a' && c <= 'z') c -= 32;
        else if (caps && shift && c >= 'A' && c <= 'Z') c += 32;
        if (ctrl && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))) c &= 0x1F;
        input_push(c);
    }
    for (int i = 0; i < 8; i++) prev[idx][i] = r[i];
}

static int hid_probe(const struct boot_info *bi)
{
    (void)bi;
    unsigned n = xhci_hid_count(), kbds = 0, mice = 0;
    if (!n) return -1;                                            /* USB-клавиатур и мышей нет */
    for (unsigned i = 0; i < n; i++) {
        if (xhci_hid_start(i, hid_report)) continue;
        if (xhci_hid_kind(i) == HID_MOUSE) { mice++; mouse_present("usb"); }
        else kbds++;
    }
    if (!kbds && !mice) return -2;
    if (input_register_poller(xhci_poll)) return -3;
    pr_info("%u keyboard(s), %u mouse/mice active\n", kbds, mice);
    return 0;
}

MIGA_DRIVER(usbhid, "usbhid", DRV_VER(1, 1), DRV_LATE, hid_probe);
