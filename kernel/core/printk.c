#include <stdarg.h>
#include <stdint.h>
#include <miga/log.h>
#include <miga/console.h>
#include <miga/string.h>

static char buf[256];
static size_t pos;

static void flush(void) { if (pos) { console_write(buf, pos); pos = 0; } }
static void put(char c) { buf[pos++] = c; if (pos == sizeof buf) flush(); }

static void put_num(uint64_t v, unsigned base, int upper, int neg, int width, char pad, int alt)
{
    char t[24]; int n = 0;
    const char *dg = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do { t[n++] = dg[v % base]; v /= base; } while (v);
    int len = n + (neg ? 1 : 0) + (alt ? 2 : 0);
    if (pad == ' ') while (len++ < width) put(' ');
    if (neg) put('-');
    if (alt) { put('0'); put('x'); }
    if (pad == '0') while (len++ < width) put('0');
    while (n) put(t[--n]);
}

void vkprintf(const char *f, va_list ap)
{
    for (; *f; f++) {
        if (*f == '\n') { put('\n'); continue; }
        if (*f != '%') { put(*f); continue; }
        f++;
        int alt = 0, width = 0; char pad = ' ';
        for (;; f++) { if (*f == '#') alt = 1; else if (*f == '0') pad = '0'; else break; }
        while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0');
        int lng = 0;
        while (*f == 'l' || *f == 'z') { lng = 1; f++; }
        switch (*f) {
        case 'd': case 'i': {
            int64_t v = lng ? va_arg(ap, int64_t) : va_arg(ap, int);
            put_num(v < 0 ? -(uint64_t)v : (uint64_t)v, 10, 0, v < 0, width, pad, 0);
            break; }
        case 'u': put_num(lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 10, 0, 0, width, pad, 0); break;
        case 'x': put_num(lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16, 0, 0, width, pad, alt); break;
        case 'X': put_num(lng ? va_arg(ap, uint64_t) : va_arg(ap, unsigned), 16, 1, 0, width, pad, alt); break;
        case 'p': put_num((uint64_t)va_arg(ap, void *), 16, 0, 0, 0, ' ', 1); break;
        case 'c': put((char)va_arg(ap, int)); break;
        case 's': { const char *s = va_arg(ap, const char *); if (!s) s = "(null)";
                    size_t n = strlen(s); while ((int)n < width--) put(' '); while (*s) put(*s++); break; }
        case '%': put('%'); break;
        case 0: f--; break;
        default: put('%'); put(*f); break;
        }
    }
    flush();
}

void kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vkprintf(fmt, ap);
    va_end(ap);
}
