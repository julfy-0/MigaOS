/* Terminal: консоль в окне GUI. Свои команды + весь kshell (kprintf перехватывается на время команды). */
#include <kernel/gui.h>
#include <kernel/kernel.h>
#include <kernel/settings.h>
#include <miga/console.h>
#include <miga/string.h>

#define COLS 89
#define ROWS 27
#define SB 400
#define PADX 4
#define PADY 4
#define PLEN 6
#define MAXIN 80
#define HIST 16

enum { C_TXT, C_OK, C_ACC, C_DIM };
static const uint32_t pal[] = {GUI_C_TEXT, GUI_C_GOOD, GUI_C_ACCENT, GUI_C_DIM};

static char txt[SB][COLS];
static uint8_t col[SB][COLS];
static int top, cx, scroll, focused, blink;
static uint8_t cattr;
static char line[MAXIN + 1];
static int len, pos;
static char hist[HIST][MAXIN + 1];
static int hn, hi;

static const char *const cmds[] = {"help", "clear", "exit", "apps", "run", "open", "echo", "mem", "ram", "cpu",
    "uptime", "usb", "lspci", "board", "acpi", "time", "audio", "gpu", "disk", "net", "bt", "monitor",
    "mouse", "hz", "reboot", "halt", "crash"};

static int starts(const char *s, const char *p) { while (*p) if (*s++ != *p++) return 0; return 1; }

static int max_scroll(void)
{
    int lo = top - SB + 1; if (lo < 0) lo = 0;
    int m = top - (ROWS - 1) - lo;
    return m > 0 ? m : 0;
}

static void clear_all(void)
{
    memset(txt, ' ', sizeof txt); memset(col, 0, sizeof col);
    top = cx = scroll = 0;
}

static void nl(void)
{
    top++;
    memset(txt[top % SB], ' ', COLS); memset(col[top % SB], 0, COLS);
    cx = 0;
    if (scroll && scroll < max_scroll()) scroll++;       /* прокрученный вид не «уезжает» от нового вывода */
}

static void tput(char c)
{
    if (c == '\n') { nl(); return; }
    if (c == '\f') { clear_all(); return; }
    if (c == '\r') { cx = 0; return; }
    if (c == '\b') { if (cx) cx--; return; }
    if (c == '\t') { do tput(' '); while (cx % 8); return; }
    if ((unsigned char)c < 32 || c > 126) c = '?';
    if (cx >= COLS) nl();
    txt[top % SB][cx] = c; col[top % SB][cx] = cattr; cx++;
}

static void tputs(const char *s) { while (*s) tput(*s++); }
static void sink(const char *s, size_t n) { while (n--) tput(*s++); }

static void input_sync(void)                             /* перерисовать строку ввода в ячейках */
{
    char *t = txt[top % SB]; uint8_t *c = col[top % SB];
    for (int i = PLEN; i < COLS; i++) { int k = i - PLEN; t[i] = k < len ? line[k] : ' '; c[i] = C_TXT; }
    cx = PLEN + pos;
}

static void prompt(void)
{
    if (cx) nl();
    cattr = C_OK; tputs("miga> "); cattr = C_TXT;
    len = pos = 0; hi = hn;
    input_sync();
}

static void hist_add(const char *l)
{
    if (!*l) return;
    if (hn && !strcmp(hist[(hn - 1) % HIST], l)) { hi = hn; return; }
    memcpy(hist[hn % HIST], l, strlen(l) + 1); hn++; hi = hn;
}

static void load(const char *s) { len = (int)strlen(s); memcpy(line, s, (size_t)len + 1); pos = len; input_sync(); }

static void complete(void)
{
    if (pos != len) return;
    for (int i = 0; i < len; i++) if (line[i] == ' ') return;
    const char *m = 0; int n = 0;
    unsigned own = sizeof cmds / sizeof *cmds;
    for (unsigned i = 0; i < own + kcmds_count(); i++) {
        const char *c = i < own ? cmds[i] : kcmds_name(i - own); int k = 0;
        while (k < len && c[k] == line[k]) k++;
        if (k == len && (!m || strcmp(m, c))) { m = c; n++; }
    }
    if (n == 1) { for (len = 0; m[len]; len++) line[len] = m[len]; line[len++] = ' '; pos = len; input_sync(); }
}

static void run_line(struct gui_win *w, char *l)
{
    while (*l == ' ') l++;
    char *e = l + strlen(l);
    while (e > l && e[-1] == ' ') *--e = 0;
    if (!*l) return;

    if (!strcmp(l, "clear")) { clear_all(); return; }
    if (!strcmp(l, "exit"))  { gui_close(w); return; }
    if (!strcmp(l, "gui"))   { tputs("already in GUI\n"); return; }
    if (!strcmp(l, "help")) {
        tputs("builtin: help clear exit apps run <app> echo <text>\n"
              "system : mem ram cpu uptime usb lspci [-v] board acpi time audio gpu disk net bt\n"
              "         monitor mouse hz [N] reboot halt\n"
              "keys   : Up/Down history, Left/Right edit, Tab complete, wheel scroll, ^L clear, ^C cancel\n");
        console_capture(sink); kcmds_help(); console_capture(0);
        return;
    }
    if (!strcmp(l, "apps")) {
        for (int i = 0; i < gui_app_count(); i++) { tputs(gui_app_name(i)); tputs("  "); }
        tput('\n'); return;
    }
    if (starts(l, "run ") || starts(l, "open ")) {
        const char *a = l + (l[0] == 'r' ? 4 : 5);
        while (*a == ' ') a++;
        if (gui_launch(a)) { tputs("no such app: "); tputs(a); tput('\n'); }
        return;
    }
    if (starts(l, "echo") && (!l[4] || l[4] == ' ')) { tputs(l[4] ? l + 5 : ""); tput('\n'); return; }

    console_capture(sink);
    kshell_exec(l);
    console_capture(0);
}

static void draw(struct gui_win *w)
{
    gui_clear(w, 0x0B0F1A);
    int first = top - scroll - (ROWS - 1);
    for (int r = 0; r < ROWS; r++) {
        int ln = first + r;
        if (ln < 0 || top - ln >= SB) continue;
        const char *t = txt[ln % SB]; const uint8_t *c = col[ln % SB];
        int y = PADY + r * GUI_CHAR_H;
        for (int x = 0; x < COLS;) {
            int s = x, n = 0, sp = 1; char b[COLS + 1];
            while (x < COLS && c[x] == c[s]) { if (t[x] != ' ') sp = 0; b[n++] = t[x++]; }
            b[n] = 0;
            if (!sp) gui_text(w, PADX + s * GUI_CHAR_W, y, b, pal[c[s]]);
        }
    }
    if (focused && blink && !scroll)
        gui_fill(w, PADX + cx * GUI_CHAR_W, PADY + (ROWS - 1) * GUI_CHAR_H + 13, GUI_CHAR_W, 2, GUI_C_GOOD);
    if (scroll) {
        char b[24]; gui_snprintf(b, sizeof b, "[-%d lines]", scroll);
        gui_text_bg(w, w->w - 120, PADY, b, 0x101010, GUI_C_WARN);
    }
    gui_present(w);
}

static void key(struct gui_win *w, int c)
{
    scroll = 0;
    if (c == '\n') {
        char cmd[MAXIN + 1];
        line[len] = 0; memcpy(cmd, line, (size_t)len + 1);
        hist_add(cmd);
        cx = PLEN + len; nl();
        run_line(w, cmd);
        prompt();
    } else if (c == '\b') {
        if (pos) { for (int i = pos - 1; i < len - 1; i++) line[i] = line[i + 1]; len--; pos--; input_sync(); }
    } else if (c == GUI_KEY_LEFT)  { if (pos) pos--; input_sync(); }
    else if (c == GUI_KEY_RIGHT)   { if (pos < len) pos++; input_sync(); }
    else if (c == 0x01) { pos = 0; input_sync(); }                       /* Ctrl+A */
    else if (c == 0x05) { pos = len; input_sync(); }                     /* Ctrl+E */
    else if (c == 0x15) { len = pos = 0; input_sync(); }                 /* Ctrl+U */
    else if (c == 0x0C) { clear_all(); prompt(); }                       /* Ctrl+L */
    else if (c == 0x03) { cx = PLEN + len; tputs("^C"); nl(); prompt(); }/* Ctrl+C */
    else if (c == '\t') complete();
    else if (c == GUI_KEY_UP) {
        if (hi > 0 && hi > hn - HIST) { hi--; load(hist[hi % HIST]); }
    } else if (c == GUI_KEY_DOWN) {
        if (hi < hn) { hi++; if (hi == hn) { len = pos = 0; input_sync(); } else load(hist[hi % HIST]); }
    } else if (c >= 32 && c < 127 && len < MAXIN) {
        for (int i = len; i > pos; i--) line[i] = line[i - 1];
        line[pos++] = (char)c; len++; input_sync();
    }
}

static void start(struct gui_win *w)
{
    clear_all(); hn = hi = 0; focused = blink = 1; cattr = C_ACC;
    tputs("MigaOS terminal\n"); cattr = C_DIM;
    tputs("Type 'help'. Tab completes, Up/Down history, mouse wheel scrolls.\n"); cattr = C_TXT;
    prompt();
    draw(w);
}

static void ev(struct gui_win *w, const struct gui_event *e)
{
    switch (e->type) {
    case GUI_EV_FOCUS: focused = 1; blink = 1; break;
    case GUI_EV_BLUR:  focused = 0; break;
    case GUI_EV_TICK:  blink = settings_get_int("term.blink", 1) ? blink ^ 1 : 1; if (!focused) return; break;
    case GUI_EV_KEY:   key(w, e->key); break;
    case GUI_EV_MOUSE:
        if (e->mtype != GUI_MOUSE_WHEEL) return;
        scroll += (int)e->arg * settings_get_int("term.scrollstep", 3);
        if (scroll < 0) scroll = 0;
        if (scroll > max_scroll()) scroll = max_scroll();
        break;
    default: return;
    }
    draw(w);
}

const struct gui_app app_terminal = {
    .name = "Terminal", .glyph = "Tm", .color = 0x1F9D6B, .w = 720, .h = 440, .tick_ms = 500,
    .start = start, .event = ev,
};
