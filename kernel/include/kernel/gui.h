/* MigaGUI: публичный интерфейс графической подсистемы.
 *
 * Архитектура «сервер + клиенты» (как X11/Wayland, только внутри ядра, пока нет процессов):
 *   - СЕРВЕР (kernel/gui/server.c) — единственный, кто знает про фреймбуфер, окна, z-порядок,
 *     фокус, рабочий стол и панель задач. Он принимает ввод, раскладывает его по окнам и
 *     собирает (композитит) кадр.
 *   - КЛИЕНТЫ (kernel/gui/apps/, файлы .c) — приложения. Они видят ТОЛЬКО этот заголовок: рисуют в
 *     «общую поверхность» своего окна, получают события из очереди окна и шлют серверу запросы
 *     (damage, заголовок, таймер, закрытие) через очередь запросов.
 * Когда появятся процессы, очереди заменятся на IPC, а API клиента останется прежним. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <miga/boot_info.h>

/* ---- коды клавиш сверх ASCII (их выдают ps2kbd/usbhid, см. патч) ---- */
#define GUI_KEY_UP    0x80
#define GUI_KEY_DOWN  0x81
#define GUI_KEY_LEFT  0x82
#define GUI_KEY_RIGHT 0x83
#define GUI_KEY_ESC   27
#define GUI_KEY_SUPER 0x84   /* клавиша Windows / Super: меню «Пуск» */
#define GUI_KEY_DEL   0x85
#define GUI_KEY_HOME  0x86
#define GUI_KEY_END   0x87
#define GUI_KEY_PGUP  0x88
#define GUI_KEY_PGDN  0x89

/* ---- тема ---- */
#define GUI_C_WINBG   0x141A2B
#define GUI_C_PANEL   0x1B2236
#define GUI_C_PANEL2  0x252E47
#define GUI_C_LINE    0x39445F
#define GUI_C_TEXT    0xE6EAF2
#define GUI_C_DIM     0x8B95AD
#define GUI_C_ACCENT  0x4C8DFF
#define GUI_C_GOOD    0x4CD08A
#define GUI_C_WARN    0xF2B84B
#define GUI_C_BAD     0xE5566D

/* ---- события, которые сервер кладёт в очередь окна ---- */
enum gui_ev_type {
    GUI_EV_NONE = 0,
    GUI_EV_KEY,     /* key = символ или GUI_KEY_* */
    GUI_EV_FOCUS,   /* окно получило фокус */
    GUI_EV_BLUR,    /* окно потеряло фокус */
    GUI_EV_TICK,    /* сработал таймер окна (gui_set_tick / поле tick_ms), arg = uptime мс */
    GUI_EV_CLOSE,   /* окно закрывают: освободить ресурсы (ответа не требуется) */
    GUI_EV_MOUSE,   /* мышь над клиентской областью: mx,my — координаты в окне, buttons — MOUSE_BTN_*, mtype — GUI_MOUSE_* */
};

enum gui_mouse_type { GUI_MOUSE_MOVE = 0, GUI_MOUSE_DOWN, GUI_MOUSE_UP, GUI_MOUSE_WHEEL };

struct gui_event {
    uint8_t  type;  /* enum gui_ev_type */
    uint8_t  key;   /* GUI_EV_KEY: символ; GUI_MOUSE_DOWN/UP: изменившаяся кнопка (1=левая 2=правая 4=средняя) */
    uint8_t  mtype; /* GUI_EV_MOUSE: enum gui_mouse_type */
    uint8_t  buttons;
    uint32_t arg;   /* GUI_EV_TICK: uptime мс; GUI_MOUSE_WHEEL: шаги колеса (int, > 0 вверх) */
    int16_t  mx, my;
};

/* ---- окно, как его видит клиент ---- */
struct gui_win {
    int       id;
    int       w, h;   /* размер клиентской области */
    uint32_t *px;     /* поверхность w*h, формат 0x00RRGGBB, stride = w. Разделяется с сервером */
    void     *priv;   /* свободное поле клиента */
};

/* ---- описание приложения (клиента) ---- */
struct gui_app {
    const char *name;
    char        glyph[3];                   /* две буквы для иконки */
    uint32_t    color;                      /* цвет иконки и заголовка */
    uint16_t    w, h;                       /* размер клиентской области (w*h <= GUI_SURF_PX) */
    uint16_t    tick_ms;                    /* 0 = без таймера */
    void (*start)(struct gui_win *w);       /* окно создано: нарисовать первый кадр */
    void (*event)(struct gui_win *w, const struct gui_event *ev);
};

#define GUI_MAX_WIN 8
#define GUI_SURF_PX (720 * 480)             /* максимальная площадь поверхности окна */

/* ---- жизненный цикл сервера ---- */
void gui_set_target_hz(unsigned hz);        /* частота кадров композитора (0 = взять из EDID монитора) */
void gui_init(const struct boot_info *bi);  /* из kmain: запоминает параметры фреймбуфера */
int  gui_run(void);
int  gui_launch(const char *name);          /* запуск приложения по имени (без регистра, можно префикс); 0 = ок */
int  gui_app_count(void);
const char *gui_app_name(int i);
unsigned gui_fps(void);                     /* кадры композитора за последнюю секунду */
unsigned gui_target_hz(void);                         /* запускает сервер; вернётся после Ctrl+Q. 0 = ок */
void console_fb_mute(int on);               /* kernel/core/console.c: не писать в fbcon (логи идут в serial) */

/* ---- API клиента (kernel/gui/client.c) ---- */
void gui_clear(struct gui_win *w, uint32_t color);
void gui_fill(struct gui_win *w, int x, int y, int cw, int ch, uint32_t color);
void gui_rect(struct gui_win *w, int x, int y, int cw, int ch, uint32_t color);   /* контур 1 px */
void gui_line(struct gui_win *w, int x0, int y0, int x1, int y1, uint32_t color);
void gui_bar(struct gui_win *w, int x, int y, int cw, int ch, uint64_t val, uint64_t max, uint32_t fg, uint32_t bg);
void gui_text(struct gui_win *w, int x, int y, const char *s, uint32_t fg);       /* без фона */
void gui_text_bg(struct gui_win *w, int x, int y, const char *s, uint32_t fg, uint32_t bg);
void gui_text_big(struct gui_win *w, int x, int y, const char *s, uint32_t fg, int scale);
void gui_printf(struct gui_win *w, int x, int y, uint32_t fg, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
int  gui_text_width(const char *s);         /* в пикселях */

void gui_present(struct gui_win *w);        /* запрос серверу: перерисовать всё окно */
void gui_damage(struct gui_win *w, int x, int y, int cw, int ch);
void gui_set_title(struct gui_win *w, const char *title);
void gui_set_tick(struct gui_win *w, unsigned ms);
void gui_close(struct gui_win *w);
void gui_notify(struct gui_win *w, const char *msg);   /* всплывающее уведомление (до 23 символов) */
void gui_rrect(struct gui_win *w, int x, int y, int cw, int ch, int r, uint32_t color);   /* скруглённый прямоугольник со сглаживанием */
void gui_disc(struct gui_win *w, int cx, int cy, int r, uint32_t color);                   /* сглаженный круг */

/* ---- сервисы сервера для приложений ---- */
struct gui_time { int year, mon, day, hour, min, sec, wday; };   /* wday: 0 = воскресенье */
void gui_get_time(struct gui_time *t);                           /* RTC + часовой пояс (настройка sys.tz) */
int  gui_days_in_month(int year, int mon);
int  gui_weekday(int year, int mon, int day);

struct gui_task { char name[24]; uint32_t color, surf_kib, up_s; uint8_t focused, minimized; };
int  gui_task_count(void);                                       /* окон сейчас открыто */
int  gui_task_info(int i, struct gui_task *out);                 /* 0 = ок */
void gui_task_end(int i);                                        /* закрыть i-е окно */
unsigned gui_cpu_busy(void);                                     /* % времени, когда процессор не спал в hlt (за секунду) */
void gui_theme_changed(void);                                    /* настройки gui.wallpaper / gui.accent / sys.tz изменились */

/* мини-snprintf: %d %u %x %c %s, модификатор l, флаги '0' и '-', ширина */
int  gui_snprintf(char *buf, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int  gui_vsnprintf(char *buf, size_t n, const char *fmt, va_list ap);

#define GUI_CHAR_W 8
#define GUI_CHAR_H 16
