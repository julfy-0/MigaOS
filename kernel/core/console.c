#include <miga/console.h>
#include <miga/string.h>

static struct console *head;
static int fb_muted;
static void (*capture)(const char *, size_t);

void console_capture(void (*fn)(const char *, size_t)) { capture = fn; }

void console_register(struct console *c)
{
    c->next = head;
    head = c;
}

/* GUI забирает фреймбуфер: fbcon молчит, остальные консоли (serial) продолжают получать логи */
void console_fb_mute(int on) { fb_muted = on; }
int console_fb_muted(void) { return fb_muted; }

unsigned console_count(void)
{
    unsigned n = 0;
    for (struct console *c = head; c; c = c->next) n++;
    return n;
}

const char *console_name(unsigned i)
{
    for (struct console *c = head; c; c = c->next, i--)
        if (!i) return c->name;
    return "";
}

void console_write(const char *s, size_t n)
{
    if (capture) capture(s, n);
    for (struct console *c = head; c; c = c->next) {
        if (fb_muted && !strcmp(c->name, "fbcon")) continue;
        c->write(c, s, n);
    }
}
