/* Приложение 2: Notepad — многострочный редактор в статическом буфере 4 КиБ, мягкий перенос, прокрутка. */
#include <kernel/gui.h>
#include <miga/string.h>

#define CAP   4096
#define COLS  53                       /* (440 - 16) / 8 */
#define ROWS  16
#define TX    8
#define TY    30

static char buf[CAP];
static unsigned len, cur, top;         /* top — первая видимая строка */
static uint16_t rowstart[CAP + 8];
static unsigned nrows, crow, ccol;
static int blink;

static void layout(void)
{
    unsigned col = 0;
    nrows = 1; rowstart[0] = 0; crow = ccol = 0;
    for (unsigned i = 0; i < len; i++) {
        if (i == cur) { crow = nrows - 1; ccol = col; }
        if (buf[i] == '\n') { rowstart[nrows++] = (uint16_t)(i + 1); col = 0; }
        else if (++col == COLS) { rowstart[nrows++] = (uint16_t)(i + 1); col = 0; }
    }
    if (cur == len) { crow = nrows - 1; ccol = col; }
}

static unsigned row_end(unsigned r) { return r + 1 < nrows ? rowstart[r + 1] : len; }

static unsigned row_maxcol(unsigned r)
{
    unsigned s = rowstart[r], e = row_end(r), n = e - s;
    if (r + 1 < nrows) {
        if (n && buf[e - 1] == '\n') return n - 1;      /* курсор может стоять на самом '\n' */
        return n ? n - 1 : 0;                           /* строка заполнена переносом */
    }
    return n;                                           /* последняя строка: можно встать в конец */
}

static void insert(char c)
{
    if (len >= CAP - 1) return;
    memmove(buf + cur + 1, buf + cur, len - cur);
    buf[cur++] = c;
    len++;
}

static void draw(struct gui_win *w)
{
    layout();
    if (crow < top) top = crow;
    if (crow >= top + ROWS) top = crow - ROWS + 1;

    gui_clear(w, GUI_C_WINBG);
    gui_printf(w, 8, 6, GUI_C_DIM, "Ln %u  Col %u   %u/%u bytes", crow + 1, ccol + 1, len, CAP - 1);
    gui_fill(w, 4, TY - 4, w->w - 8, ROWS * GUI_CHAR_H + 8, GUI_C_PANEL);
    gui_rect(w, 4, TY - 4, w->w - 8, ROWS * GUI_CHAR_H + 8, GUI_C_LINE);

    for (unsigned r = top; r < nrows && r < top + ROWS; r++) {
        int y = TY + (int)(r - top) * GUI_CHAR_H;
        unsigned e = row_end(r);
        for (unsigned i = rowstart[r], x = 0; i < e; i++, x++) {
            char ch[2] = {buf[i], 0};
            if (buf[i] == '\n') break;
            int cursor_here = blink && i == cur;
            if (cursor_here) gui_fill(w, TX + (int)x * GUI_CHAR_W, y, GUI_CHAR_W, GUI_CHAR_H, GUI_C_ACCENT);
            gui_text(w, TX + (int)x * GUI_CHAR_W, y, ch, GUI_C_TEXT);
        }
    }
    if (blink && crow >= top && crow < top + ROWS) {          /* курсор в конце строки / на '\n' */
        unsigned ci = cur;
        if (ci == len || buf[ci] == '\n')
            gui_fill(w, TX + (int)ccol * GUI_CHAR_W, TY + (int)(crow - top) * GUI_CHAR_H, GUI_CHAR_W, GUI_CHAR_H, GUI_C_ACCENT);
    }
    gui_text(w, 8, w->h - 20, "Arrows move, Enter newline, Backspace delete", GUI_C_DIM);
    gui_present(w);
}

static void ev(struct gui_win *w, const struct gui_event *e)
{
    if (e->type == GUI_EV_TICK) { blink = !blink; draw(w); return; }
    if (e->type != GUI_EV_KEY) return;
    unsigned k = e->key;
    blink = 1;
    layout();
    if (k >= 32 && k < 127) insert((char)k);
    else if (k == '\n') insert('\n');
    else if (k == '\t') { for (int i = 0; i < 4; i++) insert(' '); }
    else if (k == '\b') { if (cur) { memmove(buf + cur - 1, buf + cur, len - cur); cur--; len--; } }
    else if (k == GUI_KEY_LEFT)  { if (cur) cur--; }
    else if (k == GUI_KEY_RIGHT) { if (cur < len) cur++; }
    else if (k == GUI_KEY_UP && crow > 0) {
        unsigned c = ccol < row_maxcol(crow - 1) ? ccol : row_maxcol(crow - 1);
        cur = rowstart[crow - 1] + c;
    } else if (k == GUI_KEY_DOWN && crow + 1 < nrows) {
        unsigned c = ccol < row_maxcol(crow + 1) ? ccol : row_maxcol(crow + 1);
        cur = rowstart[crow + 1] + c;
    }
    draw(w);
}

static void start(struct gui_win *w)
{
    static const char hello[] = "Welcome to MigaOS!\nThis is a tiny notepad running as a client of the MigaGUI server.\n";
    if (!len) { memcpy(buf, hello, sizeof hello - 1); len = cur = sizeof hello - 1; }
    blink = 1;
    draw(w);
}

const struct gui_app app_notepad = {
    .name = "Notepad", .glyph = "Np", .color = 0xD99A1E, .w = 440, .h = 320, .tick_ms = 500,
    .start = start, .event = ev,
};
