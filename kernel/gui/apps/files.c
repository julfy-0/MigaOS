/* Приложение 10: Files — обзор точек монтирования (kernel/core/vfs.c).
 * Файловой системы и RAM-диска нет: каталог здесь состоит только из точек монтирования и путей к ним.
 * Можно ходить по дереву, монтировать (M) и отмонтировать (U) выбранную точку. Файлы читать пока нечем. */
#include <kernel/gui.h>
#include <kernel/vfs.h>
#include <kernel/settings.h>
#include <miga/string.h>

#define WIN_W   600
#define WIN_H   380
#define ROW_H   20
#define LIST_Y  58
#define FOOT_H  62
#define VIS     ((WIN_H - LIST_Y - FOOT_H) / ROW_H)

static char path[VFS_PATH_MAX] = "/";
static struct vfs_dirent ents[VFS_MAX_MOUNTS];
static int nent, sel, topi;
static char msg[72];
static uint32_t msg_col;

static void note(uint32_t c, const char *fmt, const char *a, const char *b)
{
    gui_snprintf(msg, sizeof msg, fmt, a, b);
    msg_col = c;
}

static void child_path(char *out, size_t n, int i)
{
    gui_snprintf(out, n, "%s/%s", path[1] ? path : "", ents[i].name);
}

static void refresh(void)
{
    struct vfs_dirent all[VFS_MAX_MOUNTS];
    int n = vfs_list(path, all, VFS_MAX_MOUNTS), show_sys = settings_get_int("files.show_sys", 1);
    nent = 0;
    for (int i = 0; i < n; i++) {
        if (!show_sys && all[i].point && (all[i].flags & VFS_F_SYS)) continue;
        ents[nent++] = all[i];
    }
    if (sel >= nent) sel = nent - 1;
    if (sel < 0) sel = 0;
    if (sel < topi) topi = sel;
    if (sel >= topi + VIS) topi = sel - VIS + 1;
    if (topi < 0) topi = 0;
}

static void go_up(void)
{
    size_t l = strlen(path);
    char from[VFS_PATH_MAX];
    if (l <= 1) return;
    while (l > 1 && path[l - 1] != '/') l--;
    memcpy(from, path + l, strlen(path + l) + 1);          /* имя каталога, из которого вышли */
    path[l > 1 ? l - 1 : 1] = 0;
    sel = topi = 0;
    refresh();
    for (int i = 0; i < nent; i++)
        if (!strcmp(ents[i].name, from)) { sel = i; break; }
    refresh();
    msg[0] = 0;
}

static void open_sel(void)
{
    char np[VFS_PATH_MAX];
    if (sel < 0 || sel >= nent) return;
    child_path(np, sizeof np, sel);
    memcpy(path, np, strlen(np) + 1);
    sel = topi = 0;
    msg[0] = 0;
    refresh();
}

static void do_mount(int mount)
{
    char tp[VFS_PATH_MAX], src[VFS_SRC_MAX];
    if (nent > 0) child_path(tp, sizeof tp, sel);
    else memcpy(tp, path, strlen(path) + 1);                /* пустой каталог: действие над самой точкой */
    const struct vfs_mount *m = vfs_find(tp);
    if (!m) { note(GUI_C_BAD, "%s is not a mount point%s", tp, ""); return; }
    memcpy(src, m->src, sizeof src);
    int r = mount ? vfs_mount(src, tp, 0, 0) : vfs_umount(tp);
    if (r) note(GUI_C_BAD, "%s: %s", mount ? "mount" : "umount", vfs_strerror(r));
    else note(GUI_C_GOOD, mount ? "%s mounted on %s" : "%s released (%s)", mount ? src : tp, mount ? tp : src);
    refresh();
}

static void draw(struct gui_win *w)
{
    int det = settings_get_int("files.details", 1);
    gui_clear(w, GUI_C_WINBG);

    gui_fill(w, 0, 0, w->w, 30, GUI_C_PANEL);
    gui_fill(w, 8, 4, 40, 22, path[1] ? GUI_C_ACCENT : GUI_C_PANEL2);
    gui_text(w, 16, 8, "Up", 0xFFFFFF);
    gui_text(w, 60, 8, path, GUI_C_TEXT);

    gui_fill(w, 0, 36, w->w, 18, GUI_C_PANEL2);
    gui_text(w, 34, 38, "Name", GUI_C_DIM);
    if (det) { gui_text(w, 220, 38, "Type", GUI_C_DIM); gui_text(w, 340, 38, "Source", GUI_C_DIM); }
    gui_text(w, 450, 38, "State", GUI_C_DIM);

    for (int i = 0; i < VIS && topi + i < nent; i++) {
        const struct vfs_dirent *e = &ents[topi + i];
        int y = LIST_Y + i * ROW_H;
        if (topi + i == sel) gui_fill(w, 4, y, w->w - 8, ROW_H - 2, GUI_C_PANEL2);
        gui_fill(w, 12, y + 4, 12, 10, !e->point ? GUI_C_WARN : e->mounted ? GUI_C_GOOD : GUI_C_DIM);
        gui_text(w, 34, y + 2, e->name, GUI_C_TEXT);
        if (det) {
            gui_text(w, 220, y + 2, e->point ? "mount point" : "folder", GUI_C_DIM);
            if (e->point) gui_text(w, 340, y + 2, e->src, GUI_C_TEXT);
        }
        if (e->point) gui_text(w, 450, y + 2, e->mounted ? "mounted" : "idle", e->mounted ? GUI_C_GOOD : GUI_C_DIM);
        if (e->point && (e->flags & VFS_F_RO)) gui_text(w, 530, y + 2, "ro", GUI_C_WARN);
        if (e->has_sub) gui_text(w, w->w - 24, y + 2, ">", GUI_C_DIM);
    }

    if (!nent) {
        const struct vfs_mount *m = vfs_find(path);
        if (m && m->mounted) {
            gui_printf(w, 20, LIST_Y + 8, GUI_C_TEXT, "Bound to %s (%s).", m->src, m->type);
            gui_text(w, 20, LIST_Y + 28, "No filesystem driver yet: only mount points exist.", GUI_C_DIM);
        } else if (m) {
            gui_text(w, 20, LIST_Y + 8, "This mount point is idle.", GUI_C_TEXT);
            gui_text(w, 20, LIST_Y + 28, "Press M to mount it.", GUI_C_DIM);
        } else {
            gui_text(w, 20, LIST_Y + 8, "Empty.", GUI_C_DIM);
        }
    }

    int fy = WIN_H - FOOT_H;
    gui_line(w, 0, fy, w->w, fy, GUI_C_LINE);
    if (msg[0]) gui_text(w, 8, fy + 4, msg, msg_col);
    else gui_printf(w, 8, fy + 4, GUI_C_DIM, "%u mount points, mount points only (no RAM disk, no filesystem driver)", vfs_count());
    gui_text(w, 8, fy + 24, "Up/Down: select   Enter/Right: open   Left/Backspace: up", GUI_C_DIM);
    gui_text(w, 8, fy + 42, "M: mount   U: unmount   I: details   H: system points", GUI_C_DIM);
    gui_present(w);
}

static void toggle(const char *key)
{
    settings_set(key, settings_get_int(key, 1) ? "off" : "on");
    refresh();
}

static void ev(struct gui_win *w, const struct gui_event *e)
{
    if (e->type == GUI_EV_KEY) {
        int k = e->key;
        if (k == GUI_KEY_UP)         { if (sel > 0) sel--; refresh(); }
        else if (k == GUI_KEY_DOWN)  { if (sel + 1 < nent) sel++; refresh(); }
        else if (k == '\n' || k == GUI_KEY_RIGHT) open_sel();
        else if (k == GUI_KEY_LEFT || k == '\b') go_up();
        else if (k == 'm' || k == 'M') do_mount(1);
        else if (k == 'u' || k == 'U') do_mount(0);
        else if (k == 'i' || k == 'I') toggle("files.details");
        else if (k == 'h' || k == 'H') toggle("files.show_sys");
        else return;
    } else if (e->type == GUI_EV_MOUSE) {
        if (e->mtype == GUI_MOUSE_WHEEL) {
            topi -= (int)e->arg;
            if (topi > nent - VIS) topi = nent - VIS;
            if (topi < 0) topi = 0;
        } else if (e->mtype == GUI_MOUSE_DOWN && e->key == 1) {
            if (e->my < 30) { if (e->mx < 56) go_up(); }
            else if (e->my >= LIST_Y && e->my < LIST_Y + VIS * ROW_H) {
                int i = topi + (e->my - LIST_Y) / ROW_H;
                if (i < nent) { if (i == sel) open_sel(); else sel = i; }
            }
        } else return;
    } else return;
    draw(w);
}

static void start(struct gui_win *w)
{
    path[0] = '/'; path[1] = 0;
    sel = topi = 0;
    msg[0] = 0;
    refresh();
    draw(w);
}

const struct gui_app app_files = {
    .name = "Files", .glyph = "Fl", .color = 0xE0A030, .w = WIN_W, .h = WIN_H, .tick_ms = 0,
    .start = start, .event = ev,
};
