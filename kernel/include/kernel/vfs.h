/* VFS без файловой системы: только таблица ТОЧЕК МОНТИРОВАНИЯ.
 * Нет ни RAM-диска, ни ramfs, ни initrd, ни драйверов ФС: точка просто связывает путь с источником
 * ("none", "esp", "diskN"). Читать файлы пока нельзя. Когда появится драйвер ФС, он будет подключаться
 * к vfs_resolve(): тот возвращает точку и остаток пути внутри неё. */
#pragma once
#include <stdint.h>
#include <stddef.h>

#define VFS_MAX_MOUNTS 16
#define VFS_PATH_MAX   48
#define VFS_SRC_MAX    16
#define VFS_TYPE_MAX   8

enum { VFS_F_RO = 1, VFS_F_SYS = 2, VFS_F_AUTO = 4 };   /* AUTO: точка создана ядром под диск, umount её не удаляет */

enum {
    VFS_OK = 0,
    VFS_E_PATH = -1,    /* плохой или слишком длинный путь */
    VFS_E_NOENT = -2,   /* нет такой точки / каталога */
    VFS_E_EXISTS = -3,  /* уже смонтировано */
    VFS_E_NOSRC = -4,   /* нет такого источника */
    VFS_E_INUSE = -5,   /* источник уже смонтирован в другом месте */
    VFS_E_BUSY = -6,    /* системная точка, есть вложенные точки или текущий каталог внутри */
    VFS_E_FULL = -7,    /* таблица точек заполнена */
    VFS_E_NOTMNT = -8,  /* не смонтировано */
    VFS_E_AUTO = -9,    /* точка привязана к другому устройству */
    VFS_E_NODRV = -10,  /* путь внутри точки, а драйвера ФС нет */
};

struct vfs_mount {
    char    path[VFS_PATH_MAX];
    char    src[VFS_SRC_MAX];    /* "none", "esp", "disk0".. */
    char    type[VFS_TYPE_MAX];  /* только метка, ядро её не проверяет */
    uint8_t flags;               /* VFS_F_* */
    uint8_t mounted;             /* 0 = точка объявлена, но пуста (idle) */
};

struct vfs_dirent {              /* элемент списка «содержимое каталога» (каталог состоит только из точек и путей к ним) */
    char    name[VFS_PATH_MAX];
    char    src[VFS_SRC_MAX];
    char    type[VFS_TYPE_MAX];
    uint8_t point;               /* ровно здесь есть точка монтирования */
    uint8_t mounted;
    uint8_t has_sub;             /* ниже есть ещё точки */
    uint8_t flags;
};

void vfs_init(void);             /* после drivers_init: создаёт "/", "/boot" и "/mnt/diskN" для каждого диска */

int  vfs_normalize(const char *cwd, const char *in, char *out, size_t n);   /* 0 или VFS_E_PATH */
int  vfs_mount(const char *src, const char *path, const char *type, unsigned flags);
int  vfs_umount(const char *path);

unsigned                 vfs_count(void);
const struct vfs_mount  *vfs_get(unsigned i);                 /* порядок: по дереву путей */
const struct vfs_mount  *vfs_find(const char *abs_path);      /* точное совпадение, путь уже нормализован */
const struct vfs_mount  *vfs_resolve(const char *abs_path, const char **rest);   /* самая глубокая СМОНТИРОВАННАЯ точка */
int  vfs_dir_status(const char *abs_path);                    /* 0 = каталог существует, иначе VFS_E_NOENT / VFS_E_NODRV */
int  vfs_list(const char *abs_dir, struct vfs_dirent *out, int max);

const char *vfs_cwd(void);
int  vfs_chdir(const char *path);
uint64_t vfs_src_bytes(const char *src);                      /* размер источника в байтах, 0 = неизвестно */
const char *vfs_strerror(int err);
