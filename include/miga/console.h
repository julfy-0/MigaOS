/* Вывод текста. Любой драйвер, умеющий показывать символы, регистрирует console. */
#pragma once
#include <stddef.h>

struct console {
    const char *name;
    void (*write)(struct console *c, const char *s, size_t n);
    struct console *next; /* заполняет ядро */
};

void console_register(struct console *c);
void console_write(const char *s, size_t n);
void console_capture(void (*fn)(const char *s, size_t n)); /* перехват вывода (NULL = выключить) */
unsigned    console_count(void);                 /* сколько консолей зарегистрировано */
const char *console_name(unsigned i);            /* имя i-й консоли в порядке вывода ("" если нет) */
int         console_fb_muted(void);              /* 1 = fbcon сейчас молчит (фреймбуфер у GUI) */
