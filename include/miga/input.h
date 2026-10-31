#pragma once
#include <stdint.h>

typedef void (*input_poll_fn)(void);

void input_push(char c); /* драйвер ввода кладёт символ в очередь */
int input_getc(void);    /* -1 если пусто; перед чтением вызывает зарегистрированные poll-функции */
int input_register_poller(input_poll_fn fn); /* для драйверов без IRQ (USB): 0 или -1 */
void input_poll(void);   /* вызвать все poll-функции (GUI-цикл зовёт это на каждой итерации) */

/* ---- мышь ---- */
#define MOUSE_BTN_LEFT   1u
#define MOUSE_BTN_RIGHT  2u
#define MOUSE_BTN_MIDDLE 4u

struct mouse_pkt {
    int16_t  dx, dy;     /* относительное смещение (dy > 0 — вниз, как у экрана) */
    int8_t   wheel;      /* > 0 — колесо от себя (вверх) */
    uint8_t  buttons;    /* MOUSE_BTN_* — состояние ПОСЛЕ этого пакета */
};

/* Драйвер мыши (PS/2, USB HID) кладёт пакет. Безопасно из IRQ. */
void mouse_push(int dx, int dy, int wheel, unsigned buttons);
/* Ядро/GUI забирает пакеты по порядку. 0 = пакет получен, -1 = очередь пуста. */
int  mouse_pop(struct mouse_pkt *out);
/* Драйвер сообщает, что мышь есть (для kshell/GUI): kind "ps2" / "usb". */
void mouse_present(const char *kind);
unsigned mouse_count(void);
const char *mouse_kind(unsigned idx);
