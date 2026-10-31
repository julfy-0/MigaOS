/* Приложение 3: Calculator — целые числа int64, + - * / %, с проверкой переполнения и деления на ноль. */
#include <kernel/gui.h>
#include <miga/string.h>

static int64_t acc, cur;
static char op;
static int entering, err;
static char flash;                     /* последняя нажатая кнопка (подсветка) */
static int flash_left;

static const char layout[5][4] = {
    {'C', '<', '%', '/'},
    {'7', '8', '9', '*'},
    {'4', '5', '6', '-'},
    {'1', '2', '3', '+'},
    {'0', '0', '=', '='},
};

static void reset(void) { acc = cur = 0; op = 0; entering = 0; err = 0; }

static int apply(char o, int64_t a, int64_t b, int64_t *r)
{
    switch (o) {
    case '+': return __builtin_add_overflow(a, b, r);
    case '-': return __builtin_sub_overflow(a, b, r);
    case '*': return __builtin_mul_overflow(a, b, r);
    case '/': if (!b || (a == INT64_MIN && b == -1)) return 1; *r = a / b; return 0;
    case '%': if (!b || (a == INT64_MIN && b == -1)) return 1; *r = a % b; return 0;
    }
    *r = b;
    return 0;
}

static void press(char k)
{
    if (k == 'c' || k == 'C') { reset(); return; }
    if (err) return;
    if (k >= '0' && k <= '9') {
        if (!entering) { cur = 0; entering = 1; }
        if (cur < 900000000000000000LL) cur = cur * 10 + (k - '0');
    } else if (k == '\b' || k == '<') {
        if (entering) cur /= 10;
    } else if (k == '+' || k == '-' || k == '*' || k == '/' || k == '%') {
        if (op && entering) { if (apply(op, acc, cur, &acc)) { err = 1; return; } }
        else if (!op) acc = cur;
        op = k; entering = 0;
    } else if (k == '=' || k == '\n') {
        if (op) {
            int64_t b = entering ? cur : acc;
            if (apply(op, acc, b, &acc)) { err = 1; return; }
            op = 0; entering = 0; cur = acc;
        }
    }
}

static void draw(struct gui_win *w)
{
    char b[48];
    gui_clear(w, GUI_C_WINBG);
    gui_fill(w, 10, 10, w->w - 20, 76, GUI_C_PANEL);
    gui_rect(w, 10, 10, w->w - 20, 76, GUI_C_LINE);

    if (op) { gui_snprintf(b, sizeof b, "%ld %c", acc, op); gui_text(w, 18, 16, b, GUI_C_DIM); }
    if (err) gui_text_big(w, 18, 44, "Error", GUI_C_BAD, 2);
    else {
        gui_snprintf(b, sizeof b, "%ld", entering ? cur : acc);
        int sc = strlen(b) > 13 ? 1 : 2;
        gui_text_big(w, w->w - 20 - (int)strlen(b) * GUI_CHAR_W * sc, 44 + (sc == 1 ? 8 : 0), b, GUI_C_TEXT, sc);
    }

    int bw = 56, bh = 44, gap = 6, x0 = 10, y0 = 98;
    for (int r = 0; r < 5; r++) {
        for (int c = 0; c < 4; c++) {
            char k = layout[r][c];
            if (r == 4 && (c == 1 || c == 3)) continue;               /* вторая половина широких кнопок */
            int x = x0 + c * (bw + gap), y = y0 + r * (bh + gap), cw = bw;
            if (r == 4) cw = bw * 2 + gap;
            int isop = k == '+' || k == '-' || k == '*' || k == '/' || k == '%' || k == '=';
            uint32_t bg = k == 'C' ? GUI_C_BAD : isop ? GUI_C_ACCENT : GUI_C_PANEL2;
            if (flash == k && flash_left) bg = GUI_C_WARN;
            gui_fill(w, x, y, cw, bh, bg);
            gui_rect(w, x, y, cw, bh, GUI_C_LINE);
            char t[2] = {k, 0};
            gui_text_big(w, x + cw / 2 - 8, y + bh / 2 - 16, t, flash == k && flash_left ? 0x101010 : 0xFFFFFF, 2);
        }
    }
    gui_text(w, 10, w->h - 20, "keys: 0-9 + - * / % = Enter, Bksp, C", GUI_C_DIM);
    gui_present(w);
}

static void ev(struct gui_win *w, const struct gui_event *e)
{
    if (e->type == GUI_EV_TICK) { if (flash_left && --flash_left == 0) draw(w); return; }
    if (e->type != GUI_EV_KEY) return;
    char k = (char)e->key;
    int known = (k >= '0' && k <= '9') || k == '+' || k == '-' || k == '*' || k == '/' || k == '%' ||
                k == '=' || k == '\n' || k == '\b' || k == 'c' || k == 'C';
    if (!known) return;
    press(k);
    flash = k == '\n' ? '=' : k == '\b' ? '<' : k == 'c' ? 'C' : k;
    flash_left = 2;
    draw(w);
}

static void start(struct gui_win *w) { reset(); flash_left = 0; draw(w); }

const struct gui_app app_calc = {
    .name = "Calculator", .glyph = "Ca", .color = 0x8A4FD8, .w = 260, .h = 340, .tick_ms = 80,
    .start = start, .event = ev,
};
