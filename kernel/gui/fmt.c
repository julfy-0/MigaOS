/* Минимальный vsnprintf для GUI: %d %u %x %c %s %%, флаги '0' и '-', ширина, модификатор l/z. */
#include <kernel/gui.h>
#include <miga/string.h>

static int utoa_(char *t, uint64_t v, unsigned base)
{
    char tmp[24];
    int n = 0, len = 0;
    do { tmp[n++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
    while (n) t[len++] = tmp[--n];
    return len;
}

int gui_vsnprintf(char *b, size_t n, const char *f, va_list ap)
{
    size_t o = 0;
#define PUT(ch) do { if (o + 1 < n) b[o] = (ch); o++; } while (0)
    for (; *f; f++) {
        if (*f != '%') { PUT(*f); continue; }
        f++;
        int left = 0, width = 0, lng = 0, neg = 0, len = 0;
        char pad = ' ', t[24];
        const char *s = t;
        for (;; f++) { if (*f == '-') left = 1; else if (*f == '0') pad = '0'; else break; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        while (*f == 'l' || *f == 'z') { lng = 1; f++; }
        switch (*f) {
        case 'd': case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            neg = v < 0;
            len = utoa_(t, neg ? -(uint64_t)v : (uint64_t)v, 10);
            break; }
        case 'u': len = utoa_(t, lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 10); break;
        case 'x': len = utoa_(t, lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16); break;
        case 'c': t[0] = (char)va_arg(ap, int); len = 1; break;
        case 's': s = va_arg(ap, const char *); if (!s) s = "(null)"; len = (int)strlen(s); pad = ' '; break;
        case '%': PUT('%'); continue;
        case 0: f--; continue;
        default: PUT('%'); PUT(*f); continue;
        }
        int total = len + neg, fill = width > total ? width - total : 0;
        if (!left && pad == ' ') while (fill--) PUT(' ');
        if (neg) PUT('-');
        if (!left && pad == '0') while (fill--) PUT('0');
        for (int i = 0; i < len; i++) PUT(s[i]);
        if (left) while (fill--) PUT(' ');
    }
#undef PUT
    if (n) b[o < n ? o : n - 1] = 0;
    return (int)o;
}

int gui_snprintf(char *b, size_t n, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int r = gui_vsnprintf(b, n, fmt, ap);
    va_end(ap);
    return r;
}
