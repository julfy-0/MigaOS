/* Реестр настроек MigaOS (см. kernel/include/kernel/settings.h). */
#include <kernel/settings.h>
#include <kernel/display.h>
#include <kernel/gui.h>
#include <miga/string.h>

struct sdef {
    const char *key, *help, *def;
    uint8_t     type;
    int32_t     min, max;
    void      (*apply)(const char *val);   /* вызывается после успешного изменения, может быть NULL */
    char        val[SET_VAL_MAX];
};

static int parse_int(const char *s, int *out);

static void apply_hz(const char *v)
{
    int n = 0;
    if (parse_int(v, &n) || n < 0) n = 0;
    display_set_refresh_hz((unsigned)n);       /* то же, что команда `hz N` */
    gui_set_target_hz(0);
}

static void apply_theme(const char *v)
{
    (void)v;
    gui_theme_changed();                       /* перестроить обои/акцент и перерисовать */
}

static struct sdef defs[] = {
    {"sys.hostname",   "Host name shown by uname and hostname",      "miga", SET_STR,  0,    0, 0, {0}},
    {"sys.user",       "User name shown by whoami",                  "root", SET_STR,  0,    0, 0, {0}},
    {"term.blink",     "Terminal cursor blinks",                     "on",   SET_BOOL, 0,    1, 0, {0}},
    {"term.scrollstep","Terminal lines per mouse wheel notch",       "3",    SET_INT,  1,   10, 0, {0}},
    {"gui.hz",         "GUI frame rate target, 0 = from monitor EDID", "0",  SET_INT,  0, 1000, apply_hz, {0}},
    {"gui.wallpaper",  "Desktop wallpaper 0..4 (Aurora, Sunset, Emerald, Graphite, Ocean)", "0", SET_INT, 0, 4, apply_theme, {0}},
    {"gui.accent",     "Accent color 0..7 (violet, blue, teal, green, amber, orange, pink, red)", "1", SET_INT, 0, 7, apply_theme, {0}},
    {"gui.stats",      "Show Hz/fps in the taskbar tray",            "on",   SET_BOOL, 0,    1, apply_theme, {0}},
    {"sys.tz",         "Time zone offset from UTC in hours (clock)", "0",    SET_INT, -12,  14, apply_theme, {0}},
    {"files.details",  "Files: show type and source columns",        "on",   SET_BOOL, 0,    1, 0, {0}},
    {"files.show_sys", "Files: show system mount points",            "on",   SET_BOOL, 0,    1, 0, {0}},
};
#define NSET ((unsigned)(sizeof defs / sizeof defs[0]))

static int inited;

static void scopy(char *d, const char *s, size_t n)
{
    size_t i = 0;
    if (!n) return;
    for (; s[i] && i + 1 < n; i++) d[i] = s[i];
    d[i] = 0;
}

static void ensure(void)
{
    if (inited) return;
    inited = 1;
    for (unsigned i = 0; i < NSET; i++) scopy(defs[i].val, defs[i].def, sizeof defs[i].val);
}

/* ------------------------------------------------------------------ разбор значений */
static int parse_int(const char *s, int *out)
{
    int neg = 0;
    long v = 0;
    if (*s == '-') { neg = 1; s++; }
    if (!*s) return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        if (v > 1000000000L) return -1;
    }
    *out = (int)(neg ? -v : v);
    return 0;
}

static void fmt_int(char *out, int v)
{
    char t[16];
    int n = 0, k = 0;
    unsigned u = v < 0 ? (unsigned)(-(long)v) : (unsigned)v;
    do { t[n++] = (char)('0' + u % 10); u /= 10; } while (u);
    if (v < 0) out[k++] = '-';
    while (n) out[k++] = t[--n];
    out[k] = 0;
}

static int ieq(const char *a, const char *b)
{
    while (*a && *b) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a + 32) : *a;
        if (x != *b) return 0;
        a++; b++;
    }
    return !*a && !*b;
}

static int normalize(const struct sdef *d, const char *in, char *out)
{
    switch (d->type) {
    case SET_BOOL:
        if (ieq(in, "1") || ieq(in, "on") || ieq(in, "yes") || ieq(in, "true")) { scopy(out, "on", SET_VAL_MAX); return 0; }
        if (ieq(in, "0") || ieq(in, "off") || ieq(in, "no") || ieq(in, "false")) { scopy(out, "off", SET_VAL_MAX); return 0; }
        return -1;
    case SET_INT: {
        int v;
        if (parse_int(in, &v) || v < d->min || v > d->max) return -1;
        fmt_int(out, v);
        return 0; }
    default: {
        size_t n = strlen(in);
        if (!n || n >= SET_VAL_MAX) return -1;
        for (size_t i = 0; i < n; i++) {
            char c = in[i];
            int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
            if (!ok) return -1;
        }
        scopy(out, in, SET_VAL_MAX);
        return 0; }
    }
}

/* ------------------------------------------------------------------ API */
unsigned settings_count(void) { return NSET; }

int settings_find(const char *key)
{
    for (unsigned i = 0; i < NSET; i++)
        if (!strcmp(defs[i].key, key)) return (int)i;
    return -1;
}

int settings_info(unsigned i, struct setting_info *o)
{
    ensure();
    if (i >= NSET) return -1;
    o->key = defs[i].key; o->help = defs[i].help; o->def = defs[i].def;
    o->type = defs[i].type; o->min = defs[i].min; o->max = defs[i].max;
    scopy(o->value, defs[i].val, sizeof o->value);
    return 0;
}

const char *settings_get(const char *key)
{
    ensure();
    int i = settings_find(key);
    return i < 0 ? "" : defs[i].val;
}

int settings_get_int(const char *key, int dflt)
{
    ensure();
    int i = settings_find(key);
    if (i < 0) return dflt;
    if (defs[i].type == SET_BOOL) return !strcmp(defs[i].val, "on");
    if (defs[i].type == SET_INT) { int v; return parse_int(defs[i].val, &v) ? dflt : v; }
    return dflt;
}

int settings_set(const char *key, const char *val)
{
    ensure();
    int i = settings_find(key);
    if (i < 0) return SET_E_NOKEY;
    char tmp[SET_VAL_MAX];
    if (normalize(&defs[i], val, tmp)) return SET_E_BAD;
    scopy(defs[i].val, tmp, sizeof defs[i].val);
    if (defs[i].apply) defs[i].apply(defs[i].val);
    return SET_OK;
}

int settings_set_int(const char *key, int v)
{
    char t[16];
    fmt_int(t, v);
    return settings_set(key, t);
}

int settings_reset(const char *key)
{
    ensure();
    if (!key || !strcmp(key, "all")) {
        for (unsigned i = 0; i < NSET; i++) {
            scopy(defs[i].val, defs[i].def, sizeof defs[i].val);
            if (defs[i].apply) defs[i].apply(defs[i].val);
        }
        return SET_OK;
    }
    int i = settings_find(key);
    if (i < 0) return SET_E_NOKEY;
    scopy(defs[i].val, defs[i].def, sizeof defs[i].val);
    if (defs[i].apply) defs[i].apply(defs[i].val);
    return SET_OK;
}

const char *settings_type_name(unsigned t)
{
    return t == SET_BOOL ? "bool" : t == SET_INT ? "int" : "text";
}
