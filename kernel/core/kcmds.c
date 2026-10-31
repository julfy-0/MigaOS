/* Дополнительные команды kshell (их же видит терминал в GUI): точки монтирования, настройки, сведения о системе.
 * Диспетчер по таблице: kshell.c зовёт kcmds_exec() для всего, что не знает сам. */
#include <kernel/kernel.h>
#include <kernel/vfs.h>
#include <kernel/settings.h>
#include <kernel/gui.h>
#include <miga/console.h>
#include <miga/disk.h>
#include <miga/igfx.h>
#include <miga/string.h>
#include <miga/time.h>

#define MAXARGS 8

extern const struct miga_driver __drivers_start[], __drivers_end[];

/* ------------------------------------------------------------------ вывод */
static void spaces(int n)
{
    static const char sp[] = "                                ";      /* 32 пробела */
    if (n > 0) kprintf("%s", sp + (sizeof sp - 1) - (n > 32 ? 32 : (unsigned)n));
}

static void col(const char *s, int w)                                  /* текст, дополненный пробелами до w */
{
    kprintf("%s", s);
    spaces(w - (int)strlen(s));
}

static void print_size(uint64_t b)
{
    if (b >= (1ULL << 30)) kprintf("%lu GiB", b >> 30);
    else if (b >= (1ULL << 20)) kprintf("%lu MiB", b >> 20);
    else if (b) kprintf("%lu KiB", b >> 10);
    else kprintf("-");
}

static int parse_i64(const char *s, int64_t *out)
{
    int neg = 0, base = 10;
    uint64_t v = 0;
    if (*s == '-') { neg = 1; s++; }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) { base = 16; s += 2; }
    if (!*s) return -1;
    for (; *s; s++) {
        int d;
        if (*s >= '0' && *s <= '9') d = *s - '0';
        else if (base == 16 && *s >= 'a' && *s <= 'f') d = *s - 'a' + 10;
        else if (base == 16 && *s >= 'A' && *s <= 'F') d = *s - 'A' + 10;
        else return -1;
        if (v > (1ULL << 58)) return -1;                 /* чтобы v * 16 не переполнилось */
        v = v * (unsigned)base + (unsigned)d;
    }
    *out = neg ? -(int64_t)v : (int64_t)v;
    return 0;
}

/* ------------------------------------------------------------------ точки монтирования */
static void flags_str(char *o, const struct vfs_mount *m)
{
    o[0] = (m->flags & VFS_F_RO) ? 'r' : 'w';
    o[1] = (m->flags & VFS_F_RO) ? 'o' : 'w';
    o[2] = 0;
}

static void c_pwd(int argc, char **argv) { kprintf("%s\n", vfs_cwd()); }

static void c_cd(int argc, char **argv)
{
    const char *p = argc > 1 ? argv[1] : "/";
    int r = vfs_chdir(p);
    if (r) kprintf("cd: %s: %s\n", p, vfs_strerror(r));
}

static void c_ls(int argc, char **argv)
{
    char p[VFS_PATH_MAX];
    struct vfs_dirent e[VFS_MAX_MOUNTS];
    int r = vfs_normalize(vfs_cwd(), argc > 1 ? argv[1] : ".", p, sizeof p);
    if (!r) r = vfs_dir_status(p);
    if (r) { kprintf("ls: %s: %s\n", argc > 1 ? argv[1] : ".", vfs_strerror(r)); return; }

    int n = vfs_list(p, e, VFS_MAX_MOUNTS);
    if (!n) {
        const struct vfs_mount *m = vfs_find(p);
        if (m && m->mounted) kprintf("(empty: %s is bound to %s, but there is no filesystem driver)\n", p, m->src);
        else if (m) kprintf("(empty: mount point is idle, 'mount %s' binds it)\n", p);
        else kprintf("(empty)\n");
        return;
    }
    for (int i = 0; i < n; i++) {
        char nm[VFS_PATH_MAX + 2];
        gui_snprintf(nm, sizeof nm, "%s/", e[i].name);
        col(nm, 14);
        col(e[i].point ? "mount" : "dir", 7);
        if (e[i].point) {
            col(e[i].src, 8);
            kprintf("%s%s", e[i].mounted ? "mounted" : "idle", (e[i].flags & VFS_F_RO) ? " ro" : "");
        }
        kprintf("\n");
    }
}

static unsigned seg_len(const char *s)
{
    unsigned n = 0;
    while (s[n] && s[n] != '/') n++;
    return n;
}

static void c_tree(int argc, char **argv)
{
    char root[VFS_PATH_MAX];
    int r = vfs_normalize(vfs_cwd(), argc > 1 ? argv[1] : ".", root, sizeof root);
    if (!r) r = vfs_dir_status(root);
    if (r) { kprintf("tree: %s: %s\n", argc > 1 ? argv[1] : ".", vfs_strerror(r)); return; }

    kprintf("%s\n", root);
    size_t rl = strlen(root) == 1 ? 0 : strlen(root);
    const char *prev = "";
    for (unsigned i = 0; i < vfs_count(); i++) {
        const struct vfs_mount *m = vfs_get(i);
        if (strcmp(root, "/") ? !(strlen(m->path) > rl && !memcmp(m->path, root, rl) && m->path[rl] == '/')
                             : m->path[1] == 0) continue;
        const char *a = prev, *b = m->path + rl + 1;
        const char *rest = b;
        unsigned depth = 0;
        while (*a && *b) {
            unsigned la = seg_len(a), lb = seg_len(b);
            if (la != lb || memcmp(a, b, la)) break;
            a += la; b += lb;
            if (*a == '/') a++;
            if (*b == '/') b++;
            depth++;
        }
        while (*b) {
            unsigned lb = seg_len(b);
            char nm[VFS_PATH_MAX];
            memcpy(nm, b, lb);
            nm[lb] = 0;
            spaces((int)(2 * depth));
            kprintf("+- %s", nm);
            if (!b[lb]) kprintf("   [%s %s%s]", m->src, m->mounted ? "mounted" : "idle", (m->flags & VFS_F_RO) ? " ro" : "");
            else kprintf("/");
            kprintf("\n");
            b += lb;
            if (*b == '/') b++;
            depth++;
        }
        prev = rest;
    }
}

static void list_mounts(void)
{
    kprintf("path            source  type    mode  state\n");
    for (unsigned i = 0; i < vfs_count(); i++) {
        const struct vfs_mount *m = vfs_get(i);
        char f[3];
        flags_str(f, m);
        col(m->path, 16); col(m->src, 8); col(m->type, 8); col(f, 6);
        kprintf("%s%s\n", m->mounted ? "mounted" : "idle", (m->flags & VFS_F_SYS) ? "  (system)" : "");
    }
}

static void mount_usage(void)
{
    kprintf("usage: mount                       list mount points\n"
            "       mount <path>                bind an idle point (e.g. /mnt/disk0) to its device\n"
            "       mount <src> <path> [-r] [-t type]   src: none, esp, disk0..\n");
}

static void c_mount(int argc, char **argv)
{
    if (argc == 1) { list_mounts(); return; }
    const char *src = 0, *path = 0, *type = 0;
    unsigned fl = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-r")) fl |= VFS_F_RO;
        else if (!strcmp(argv[i], "-t") && i + 1 < argc) type = argv[++i];
        else if (!src) src = argv[i];
        else if (!path) path = argv[i];
        else { mount_usage(); return; }
    }
    if (!src) { mount_usage(); return; }

    char sbuf[VFS_SRC_MAX];
    if (!path) {                                                 /* mount <path>: источник берётся из объявленной точки */
        char p[VFS_PATH_MAX];
        int r = vfs_normalize(vfs_cwd(), src, p, sizeof p);
        const struct vfs_mount *m = r ? 0 : vfs_find(p);
        if (!m) { kprintf("mount: %s: %s\n", src, vfs_strerror(r ? r : VFS_E_NOENT)); return; }
        memcpy(sbuf, m->src, sizeof sbuf);
        path = src;
        src = sbuf;
    }
    int r = vfs_mount(src, path, type, fl);
    if (r) kprintf("mount: %s\n", vfs_strerror(r));
    else kprintf("mounted %s on %s (no filesystem driver: mount point only)\n", src, path);
}

static void c_umount(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: umount <path>\n"); return; }
    int r = vfs_umount(argv[1]);
    if (r) kprintf("umount: %s: %s\n", argv[1], vfs_strerror(r));
}

static void c_df(int argc, char **argv)
{
    kprintf("mounted on      source  type    size\n");
    for (unsigned i = 0; i < vfs_count(); i++) {
        const struct vfs_mount *m = vfs_get(i);
        if (!m->mounted) continue;
        col(m->path, 16); col(m->src, 8); col(m->type, 8);
        print_size(vfs_src_bytes(m->src));
        kprintf("\n");
    }
    kprintf("(usage is not shown: there is no filesystem driver yet)\n");
}

static void c_stat(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: stat <path>\n"); return; }
    char p[VFS_PATH_MAX];
    int r = vfs_normalize(vfs_cwd(), argv[1], p, sizeof p);
    if (r) { kprintf("stat: %s: %s\n", argv[1], vfs_strerror(r)); return; }
    kprintf("path: %s\n", p);
    const struct vfs_mount *m = vfs_find(p);
    if (m) {
        char f[3];
        flags_str(f, m);
        kprintf("kind: mount point (%s), source %s, type %s, mode %s%s\n", m->mounted ? "mounted" : "idle",
                m->src, m->type, f, (m->flags & VFS_F_SYS) ? ", system" : "");
        return;
    }
    r = vfs_dir_status(p);
    if (r == 0) { kprintf("kind: directory that only leads to mount points\n"); return; }
    const char *rest = "";
    const struct vfs_mount *o = vfs_resolve(p, &rest);
    if (o) kprintf("inside: %s (source %s), rest \"%s\"\n", o->path, o->src, rest);
    kprintf("%s\n", vfs_strerror(r));
}

static void c_lsblk(int argc, char **argv)
{
    if (!disk_count()) { kprintf("lsblk: no drives found (AHCI/NVMe only)\n"); return; }
    kprintf("name    size      type      mounted on      model\n");
    for (unsigned i = 0; i < disk_count(); i++) {
        struct disk_info d;
        if (disk_get(i, &d)) continue;
        char nm[VFS_SRC_MAX], sz[24];
        gui_snprintf(nm, sizeof nm, "disk%u", i);
        uint64_t b = d.sectors * d.sector_size;
        if (b >= (1ULL << 30)) gui_snprintf(sz, sizeof sz, "%lu GiB", b >> 30);
        else if (b >= (1ULL << 20)) gui_snprintf(sz, sizeof sz, "%lu MiB", b >> 20);
        else gui_snprintf(sz, sizeof sz, "-");
        const char *where = "-";
        for (unsigned k = 0; k < vfs_count(); k++) {
            const struct vfs_mount *m = vfs_get(k);
            if (m->mounted && !strcmp(m->src, nm)) { where = m->path; break; }
        }
        col(nm, 8); col(sz, 10); col(disk_kind_name(d.kind), 10); col(where, 16);
        kprintf("%s\n", d.model[0] ? d.model : "?");
    }
}

/* ------------------------------------------------------------------ настройки */
static void show_setting(const struct setting_info *s)
{
    col(s->key, 17);
    col(s->value, 9);
    kprintf("%s", s->help);
    if (strcmp(s->value, s->def)) kprintf("  [default %s]", s->def);
    kprintf("\n");
}

static void c_settings(int argc, char **argv)
{
    struct setting_info s;
    for (unsigned i = 0; i < settings_count(); i++)
        if (!settings_info(i, &s)) show_setting(&s);
    kprintf("(runtime only: values are lost on reboot)\n");
}

static void bad_value(const char *key)
{
    int i = settings_find(key);
    struct setting_info s;
    if (i < 0 || settings_info((unsigned)i, &s)) return;
    if (s.type == SET_INT) kprintf("set: %s expects an integer %d..%d\n", key, (int)s.min, (int)s.max);
    else if (s.type == SET_BOOL) kprintf("set: %s expects on/off (1/0, yes/no, true/false)\n", key);
    else kprintf("set: %s expects 1-%d chars from a-z A-Z 0-9 . _ -\n", key, SET_VAL_MAX - 1);
}

static void c_set(int argc, char **argv)
{
    if (argc == 1) { c_settings(argc, argv); return; }
    int i = settings_find(argv[1]);
    if (i < 0) { kprintf("set: unknown setting '%s' (try 'settings')\n", argv[1]); return; }
    struct setting_info s;
    if (argc == 2) { if (!settings_info((unsigned)i, &s)) show_setting(&s); return; }
    if (settings_set(argv[1], argv[2])) { bad_value(argv[1]); return; }
    kprintf("%s = %s\n", argv[1], settings_get(argv[1]));
}

static void c_get(int argc, char **argv)
{
    if (argc < 2) { kprintf("usage: get <key>\n"); return; }
    if (settings_find(argv[1]) < 0) { kprintf("get: unknown setting '%s'\n", argv[1]); return; }
    kprintf("%s\n", settings_get(argv[1]));
}

static void c_reset(int argc, char **argv)
{
    int r = settings_reset(argc > 1 ? argv[1] : 0);
    if (r) kprintf("reset: unknown setting '%s'\n", argv[1]);
    else kprintf("%s reset to default\n", argc > 1 ? argv[1] : "all settings");
}

/* ------------------------------------------------------------------ система */
#define MIGA_ARCH "x86_64"

static void c_hostname(int argc, char **argv)
{
    if (argc > 1) {
        if (settings_set("sys.hostname", argv[1])) { bad_value("sys.hostname"); return; }
    }
    kprintf("%s\n", settings_get("sys.hostname"));
}

static void c_whoami(int argc, char **argv) { kprintf("%s\n", settings_get("sys.user")); }

static void c_ver(int argc, char **argv)
{
    kprintf("MigaOS %s (%s, UEFI boot), built %s %s\n", MIGA_VERSION, MIGA_ARCH, __DATE__, __TIME__);
}

static void c_uname(int argc, char **argv)
{
    const char *o = argc > 1 ? argv[1] : "";
    if (!strcmp(o, "-a"))
        kprintf("MigaOS %s %s %s %s\n", settings_get("sys.hostname"), MIGA_VERSION, __DATE__, MIGA_ARCH);
    else if (!strcmp(o, "-r")) kprintf("%s\n", MIGA_VERSION);
    else if (!strcmp(o, "-n")) kprintf("%s\n", settings_get("sys.hostname"));
    else if (!strcmp(o, "-m")) kprintf("%s\n", MIGA_ARCH);
    else kprintf("MigaOS\n");
}

static void c_free(int argc, char **argv)
{
    uint64_t tot = pmm_total_pages() * PAGE_SIZE >> 20, fr = pmm_free_pages() * PAGE_SIZE >> 20;
    kprintf("        total    used    free   (MiB)\n");
    kprintf("Mem:  %7lu %7lu %7lu\n", tot, tot - fr, fr);
}

static void c_info(int argc, char **argv)
{
    uint64_t s = time_uptime_ms() / 1000;
    uint64_t tot = pmm_total_pages() * PAGE_SIZE >> 20, fr = pmm_free_pages() * PAGE_SIZE >> 20;
    unsigned mounted = 0;
    for (unsigned i = 0; i < vfs_count(); i++) mounted += vfs_get(i)->mounted;
    kprintf("%s@%s  MigaOS %s (%s)\n", settings_get("sys.user"), settings_get("sys.hostname"), MIGA_VERSION, MIGA_ARCH);
    kprintf("uptime  %u:%02u:%02u\n", (unsigned)(s / 3600), (unsigned)(s / 60 % 60), (unsigned)(s % 60));
    kprintf("memory  %lu / %lu MiB used\n", tot - fr, tot);
    kprintf("disks   %u, mount points %u (%u mounted), cwd %s\n", disk_count(), vfs_count(), mounted, vfs_cwd());
    kprintf("apps    %d in the GUI (run <app>)\n", gui_app_count());
}

static void c_lsmod(int argc, char **argv)
{
    static const char *const stage[] = {"early", "normal", "late"};
    kprintf("driver                  version  stage\n");
    for (const struct miga_driver *d = __drivers_start; d < __drivers_end; d++) {
        col(d->name, 24);
        kprintf("v%02u.%02u    %s\n", (unsigned)(d->version >> 8), (unsigned)(d->version & 0xFF),
                d->stage >= 0 && d->stage < 3 ? stage[d->stage] : "?");
    }
}

static void c_consoles(int argc, char **argv)
{
    for (unsigned i = 0; i < console_count(); i++)
        kprintf("%s%s\n", console_name(i), (console_fb_muted() && !strcmp(console_name(i), "fbcon")) ? "  (muted: GUI owns the framebuffer)" : "");
}

static void c_echo(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) kprintf("%s%s", i > 1 ? " " : "", argv[i]);
    kprintf("\n");
}

static void c_expr(int argc, char **argv)
{
    int64_t a, b, r;
    if (argc != 4 || parse_i64(argv[1], &a) || parse_i64(argv[3], &b) || argv[2][1]) {
        kprintf("usage: expr <a> <+|-|*|/|%%> <b>   (decimal or 0x hex)\n");
        return;
    }
    switch (argv[2][0]) {
    case '+': r = a + b; break;
    case '-': r = a - b; break;
    case '*': r = a * b; break;
    case '/': if (!b) { kprintf("expr: division by zero\n"); return; } r = a / b; break;
    case '%': if (!b) { kprintf("expr: division by zero\n"); return; } r = a % b; break;
    default: kprintf("expr: unknown operator '%s'\n", argv[2]); return;
    }
    kprintf("%ld (%#lx)\n", r, (uint64_t)r);
}

static void c_cls(int argc, char **argv) { kprintf("\f"); }
static void c_lsusb(int argc, char **argv) { kshell_exec("usb"); }
static void c_lscpu(int argc, char **argv) { kshell_exec("cpu"); }
static void c_lsmem(int argc, char **argv) { kshell_exec("ram"); }

static void c_igfx(int argc, char **argv)
{
    igfx_dump(argc > 1 && !strcmp(argv[1], "edid"));
}

/* ------------------------------------------------------------------ таблица */
struct kcmd {
    const char *name, *args, *help;
    void (*fn)(int argc, char **argv);
};

static const struct kcmd table[] = {
    {"pwd",      "",                    "print the current directory",                   c_pwd},
    {"cd",       "[path]",              "change directory (mount points only)",           c_cd},
    {"ls",       "[path]",              "list mount points under a path",                 c_ls},
    {"tree",     "[path]",              "mount point tree",                               c_tree},
    {"mount",    "[src path [-r] [-t type]]", "list mount points or bind a source",       c_mount},
    {"umount",   "<path>",              "release a mount point",                          c_umount},
    {"df",       "",                    "mounted sources and their sizes",                c_df},
    {"stat",     "<path>",              "what a path is",                                 c_stat},
    {"lsblk",    "",                    "drives and where they are mounted",              c_lsblk},
    {"settings", "",                    "list all settings",                              c_settings},
    {"set",      "[key [value]]",       "show or change a setting",                       c_set},
    {"get",      "<key>",               "print a setting value",                          c_get},
    {"reset",    "[key]",               "restore default (all settings without a key)",   c_reset},
    {"hostname", "[name]",              "show or set the host name",                      c_hostname},
    {"whoami",   "",                    "show the user name",                             c_whoami},
    {"uname",    "[-a|-r|-n|-m]",       "system name and version",                        c_uname},
    {"ver",      "",                    "MigaOS version and build time",                  c_ver},
    {"free",     "",                    "memory in MiB",                                  c_free},
    {"info",     "",                    "short system summary",                           c_info},
    {"lsmod",    "",                    "registered drivers",                             c_lsmod},
    {"consoles", "",                    "registered output consoles",                     c_consoles},
    {"lsusb",    "",                    "same as usb",                                    c_lsusb},
    {"lscpu",    "",                    "same as cpu",                                    c_lscpu},
    {"lsmem",    "",                    "same as ram",                                    c_lsmem},
    {"echo",     "[text]",              "print text",                                     c_echo},
    {"expr",     "<a> <op> <b>",        "integer arithmetic",                             c_expr},
    {"cls",      "",                    "clear the screen",                               c_cls},
    {"igfx",     "[edid]",              "Intel GPU: pipes, measured refresh, EDID",       c_igfx},
};
#define NCMD ((unsigned)(sizeof table / sizeof table[0]))

unsigned kcmds_count(void) { return NCMD; }
const char *kcmds_name(unsigned i) { return i < NCMD ? table[i].name : ""; }

void kcmds_help(void)
{
    kprintf("more   :\n");
    for (unsigned i = 0; i < NCMD; i++) {
        kprintf("  ");
        col(table[i].name, 9);
        col(table[i].args, 25);
        kprintf("%s\n", table[i].help);
    }
}

/* 1 = команда найдена и выполнена, 0 = такой команды нет */
int kcmds_exec(const char *line)
{
    char buf[160];
    char *argv[MAXARGS];
    int argc = 0;
    size_t n = strlen(line);
    if (n >= sizeof buf) n = sizeof buf - 1;
    memcpy(buf, line, n);
    buf[n] = 0;

    for (char *p = buf; *p && argc < MAXARGS;) {
        while (*p == ' ') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
        if (*p) *p++ = 0;
    }
    if (!argc) return 1;
    for (unsigned i = 0; i < NCMD; i++)
        if (!strcmp(argv[0], table[i].name)) { table[i].fn(argc, argv); return 1; }
    return 0;
}
