/* Таблица точек монтирования (см. kernel/include/kernel/vfs.h). Файловой системы здесь нет. */
#include <kernel/vfs.h>
#include <miga/disk.h>
#include <miga/string.h>

static struct vfs_mount mt[VFS_MAX_MOUNTS];   /* отсортирована деревом путей: "/", "/boot", "/mnt", "/mnt/disk0"... */
static unsigned nmt;
static char cwd[VFS_PATH_MAX] = "/";

/* ------------------------------------------------------------------ мелочи */
static void scopy(char *d, const char *s, size_t n)
{
    size_t i = 0;
    if (!n) return;
    for (; s[i] && i + 1 < n; i++) d[i] = s[i];
    d[i] = 0;
}

static int has_prefix(const char *s, const char *p)
{
    while (*p) if (*s++ != *p++) return 0;
    return 1;
}

/* порядок путей: '/' меньше любого символа, поэтому "/a" < "/a/x" < "/a-b" и поддерево идёт подряд */
static int pkey(char c) { return c == '/' ? 1 : (unsigned char)c; }
static int pcmp(const char *a, const char *b)
{
    for (; *a && *a == *b; a++, b++) { }
    return pkey(*a) - pkey(*b);
}

/* p строго ниже каталога dir */
static int below(const char *p, const char *dir)
{
    size_t dl = strlen(dir);
    if (dl == 1) return p[0] == '/' && p[1];
    return has_prefix(p, dir) && p[dl] == '/' && p[dl + 1];
}

static int find_idx(const char *path)
{
    for (unsigned i = 0; i < nmt; i++)
        if (!strcmp(mt[i].path, path)) return (int)i;
    return -1;
}

static void put_name(char *o, const char *pre, unsigned v)
{
    size_t k = 0;
    char t[12];
    int n = 0;
    while (*pre) o[k++] = *pre++;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (n) o[k++] = t[--n];
    o[k] = 0;
}

static int parse_disk(const char *s, unsigned *idx)      /* "disk12" -> 12 */
{
    if (!has_prefix(s, "disk") || !s[4]) return 0;
    unsigned v = 0;
    for (s += 4; *s; s++) {
        if (*s < '0' || *s > '9') return 0;
        v = v * 10 + (unsigned)(*s - '0');
        if (v > 255) return 0;
    }
    *idx = v;
    return 1;
}

static int add_raw(const char *path, const char *src, const char *type, unsigned fl, int mounted)
{
    if (nmt >= VFS_MAX_MOUNTS) return VFS_E_FULL;
    unsigned i = 0;
    while (i < nmt && pcmp(mt[i].path, path) < 0) i++;
    memmove(&mt[i + 1], &mt[i], (nmt - i) * sizeof mt[0]);
    memset(&mt[i], 0, sizeof mt[0]);
    scopy(mt[i].path, path, sizeof mt[i].path);
    scopy(mt[i].src, src, sizeof mt[i].src);
    scopy(mt[i].type, type, sizeof mt[i].type);
    mt[i].flags = (uint8_t)fl;
    mt[i].mounted = (uint8_t)mounted;
    nmt++;
    return (int)i;
}

static int check_src(const char *src)
{
    unsigned n;
    if (!strcmp(src, "none") || !strcmp(src, "esp")) return 0;
    if (parse_disk(src, &n)) return n < disk_count() ? 0 : VFS_E_NOSRC;
    return VFS_E_NOSRC;
}

/* ------------------------------------------------------------------ инициализация */
void vfs_init(void)
{
    nmt = 0;
    scopy(cwd, "/", sizeof cwd);
    add_raw("/", "none", "root", VFS_F_SYS, 1);
    add_raw("/boot", "esp", "vfat", VFS_F_SYS | VFS_F_RO, 1);     /* раздел, с которого MigaBoot взял kernel.elf */
    for (unsigned i = 0; i < disk_count() && i < 8; i++) {
        char s[VFS_SRC_MAX], p[VFS_PATH_MAX];
        put_name(s, "disk", i);
        put_name(p, "/mnt/disk", i);
        add_raw(p, s, "-", VFS_F_AUTO, 0);                         /* точка есть, смонтировать её должен пользователь */
    }
}

/* ------------------------------------------------------------------ пути */
int vfs_normalize(const char *cur, const char *in, char *out, size_t n)
{
    char buf[VFS_PATH_MAX * 2];
    size_t bl = 0, ol = 0;
    const char *p;

    if (n < 2) return VFS_E_PATH;
    if (in[0] != '/') {
        for (p = cur; *p && bl < sizeof buf - 2; p++) buf[bl++] = *p;
        buf[bl++] = '/';
    }
    for (p = in; *p && bl < sizeof buf - 1; p++) buf[bl++] = *p;
    if (*p) return VFS_E_PATH;
    buf[bl] = 0;

    out[0] = 0;
    for (const char *s = buf; *s;) {
        while (*s == '/') s++;
        if (!*s) break;
        const char *e = s;
        while (*e && *e != '/') e++;
        size_t sl = (size_t)(e - s);
        if (sl == 1 && s[0] == '.') {
            /* пропуск */
        } else if (sl == 2 && s[0] == '.' && s[1] == '.') {
            while (ol && out[ol - 1] != '/') ol--;
            if (ol) ol--;
        } else {
            if (ol + 1 + sl >= n || ol + 1 + sl >= VFS_PATH_MAX) return VFS_E_PATH;
            out[ol++] = '/';
            memcpy(out + ol, s, sl);
            ol += sl;
        }
        out[ol] = 0;
        s = e;
    }
    if (!ol) out[ol++] = '/';
    out[ol] = 0;
    return 0;
}

/* ------------------------------------------------------------------ mount / umount */
int vfs_mount(const char *src, const char *path_in, const char *type, unsigned flags)
{
    char path[VFS_PATH_MAX];
    int r = vfs_normalize(cwd, path_in, path, sizeof path);
    if (r) return r;
    if (!strcmp(path, "/")) return VFS_E_EXISTS;                   /* корень смонтирован всегда */
    r = check_src(src);
    if (r) return r;

    int idx = find_idx(path);
    if (idx >= 0 && mt[idx].mounted) return VFS_E_EXISTS;
    if (idx >= 0 && (mt[idx].flags & VFS_F_AUTO) && strcmp(mt[idx].src, src)) return VFS_E_AUTO;
    if (strcmp(src, "none"))
        for (unsigned i = 0; i < nmt; i++)
            if (mt[i].mounted && !strcmp(mt[i].src, src)) return VFS_E_INUSE;

    if (idx < 0) {
        idx = add_raw(path, src, "-", 0, 0);
        if (idx < 0) return idx;
    }
    struct vfs_mount *m = &mt[idx];
    scopy(m->src, src, sizeof m->src);
    if (type && *type) scopy(m->type, type, sizeof m->type);
    m->flags = (uint8_t)((m->flags & VFS_F_AUTO) | (flags & VFS_F_RO));
    m->mounted = 1;
    return 0;
}

int vfs_umount(const char *path_in)
{
    char path[VFS_PATH_MAX];
    int r = vfs_normalize(cwd, path_in, path, sizeof path);
    if (r) return r;
    int idx = find_idx(path);
    if (idx < 0) return VFS_E_NOENT;
    struct vfs_mount *m = &mt[idx];
    if (!m->mounted) return VFS_E_NOTMNT;
    if (m->flags & VFS_F_SYS) return VFS_E_BUSY;
    for (unsigned i = 0; i < nmt; i++)
        if ((int)i != idx && mt[i].mounted && below(mt[i].path, path)) return VFS_E_BUSY;
    if (!strcmp(cwd, path) || below(cwd, path)) return VFS_E_BUSY;

    if (m->flags & VFS_F_AUTO) m->mounted = 0;                     /* точка диска остаётся, как и /dev-узел */
    else {
        memmove(&mt[idx], &mt[idx + 1], (nmt - (unsigned)idx - 1) * sizeof mt[0]);
        nmt--;
    }
    return 0;
}

/* ------------------------------------------------------------------ запросы */
unsigned vfs_count(void) { return nmt; }
const struct vfs_mount *vfs_get(unsigned i) { return i < nmt ? &mt[i] : 0; }

const struct vfs_mount *vfs_find(const char *path)
{
    int i = find_idx(path);
    return i >= 0 ? &mt[i] : 0;
}

const struct vfs_mount *vfs_resolve(const char *path, const char **rest)
{
    const struct vfs_mount *best = 0;
    size_t bl = 0;
    for (unsigned i = 0; i < nmt; i++) {
        if (!mt[i].mounted) continue;
        size_t pl = strlen(mt[i].path);
        int ok = pl == 1 || (has_prefix(path, mt[i].path) && (path[pl] == 0 || path[pl] == '/'));
        if (ok && (!best || pl > bl)) { best = &mt[i]; bl = pl; }
    }
    if (best && rest) {
        const char *r = path + (bl == 1 ? 0 : bl);
        while (*r == '/') r++;
        *rest = r;
    }
    return best;
}

int vfs_dir_status(const char *path)
{
    if (!strcmp(path, "/")) return 0;
    for (unsigned i = 0; i < nmt; i++)
        if (!strcmp(mt[i].path, path) || below(mt[i].path, path)) return 0;
    const char *rest;
    const struct vfs_mount *m = vfs_resolve(path, &rest);
    if (m && strcmp(m->src, "none")) return VFS_E_NODRV;            /* внутри смонтированного источника, а читать его нечем */
    return VFS_E_NOENT;
}

int vfs_list(const char *dir, struct vfs_dirent *out, int max)
{
    size_t off = strlen(dir) == 1 ? 1 : strlen(dir) + 1;
    int n = 0;
    for (unsigned i = 0; i < nmt; i++) {
        const char *p = mt[i].path;
        if (!below(p, dir)) continue;
        const char *s = p + off;
        size_t sl = 0;
        while (s[sl] && s[sl] != '/') sl++;
        if (!sl || sl >= sizeof out[0].name) continue;

        int k;
        for (k = 0; k < n; k++)
            if (strlen(out[k].name) == sl && !memcmp(out[k].name, s, sl)) break;
        if (k == n) {
            if (n >= max) break;
            memset(&out[n], 0, sizeof out[n]);
            memcpy(out[n].name, s, sl);
            out[n].name[sl] = 0;
            n++;
        }
        struct vfs_dirent *d = &out[k];
        if (!s[sl]) {
            d->point = 1;
            d->mounted = mt[i].mounted;
            d->flags = mt[i].flags;
            scopy(d->src, mt[i].src, sizeof d->src);
            scopy(d->type, mt[i].type, sizeof d->type);
        } else {
            d->has_sub = 1;
        }
    }
    return n;
}

/* ------------------------------------------------------------------ текущий каталог */
const char *vfs_cwd(void) { return cwd; }

int vfs_chdir(const char *in)
{
    char p[VFS_PATH_MAX];
    int r = vfs_normalize(cwd, in, p, sizeof p);
    if (r) return r;
    r = vfs_dir_status(p);
    if (r) return r;
    scopy(cwd, p, sizeof cwd);
    return 0;
}

/* ------------------------------------------------------------------ сервис */
uint64_t vfs_src_bytes(const char *src)
{
    unsigned n;
    struct disk_info d;
    if (parse_disk(src, &n) && !disk_get(n, &d)) return d.sectors * d.sector_size;
    return 0;
}

const char *vfs_strerror(int e)
{
    switch (e) {
    case VFS_OK:        return "ok";
    case VFS_E_PATH:    return "bad or too long path";
    case VFS_E_NOENT:   return "no such mount point or directory";
    case VFS_E_EXISTS:  return "already mounted";
    case VFS_E_NOSRC:   return "no such source (use none, esp, disk0..)";
    case VFS_E_INUSE:   return "source is already mounted elsewhere";
    case VFS_E_BUSY:    return "busy (system point, nested mounts or current directory inside)";
    case VFS_E_FULL:    return "mount table is full";
    case VFS_E_NOTMNT:  return "not mounted";
    case VFS_E_AUTO:    return "this point belongs to another device";
    case VFS_E_NODRV:   return "no filesystem driver: only mount points exist";
    }
    return "error";
}
