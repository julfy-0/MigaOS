#include "syscall.h"

static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static void say(const char *s) { sys_write(1, s, slen(s)); }

static const struct { const char *n; u64 v; } capnames[] = {
    { "console", CAP_CONSOLE }, { "spawn", CAP_SPAWN }, { "initrd", CAP_INITRD },
};

/* "console,spawn" -> маска; неизвестное имя => *bad = 1 */
static u64 parse_caps(char *s, int *bad)
{
    u64 mask = 0;
    while (*s) {
        char *t = s;
        int ok = 0;
        while (*s && *s != ',') s++;
        if (*s) *s++ = 0;
        for (unsigned i = 0; i < sizeof capnames / sizeof capnames[0]; i++) {
            const char *a = capnames[i].n, *b = t;
            while (*a && *a == *b) { a++; b++; }
            if (!*a && !*b) { mask |= capnames[i].v; ok = 1; break; }
        }
        if (!ok) *bad = 1;
    }
    return mask;
}

static char conf[2048];

USER_ENTRY
{
    long n = sys_initrd_read("init.conf", conf, sizeof conf - 1);
    char *p = conf;
    if (n < 0) { say("init: no init.conf\n"); sys_exit(1); }
    conf[n] = 0;
    say("init: started\n");

    while (*p) {
        char *name, *capstr;
        int bad = 0, st = 0;
        long pid;
        u64 mask;

        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r') { while (*p && *p != '\n') p++; if (*p) p++; continue; }
        name = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n') p++;
        if (*p == ' ' || *p == '\t') *p++ = 0;
        while (*p == ' ' || *p == '\t') p++;
        capstr = p;
        while (*p && *p != '\n' && *p != '\r' && *p != ' ' && *p != '\t') p++;
        if (*p) { char c = *p; *p++ = 0; if (c != '\n') while (*p && *p != '\n') p++; if (*p == '\n') p++; }
        else *p = 0;
        if (!*name) continue;

        mask = parse_caps(capstr, &bad);
        if (bad) { say("init: bad caps for "); say(name); say("\n"); continue; }
        say("init: spawn "); say(name); say("\n");
        pid = sys_spawn(name, mask);          /* ядро: mask должна быть подмножеством прав init */
        if (pid < 0) { say("init: spawn failed: "); say(name); say("\n"); continue; }
        sys_wait(pid, &st);
    }
    say("init: all done\n");
    sys_exit(0);
}
