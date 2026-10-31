/* include/miga/syscall.h — КОНТРАКТ user<->kernel (предложение C4 → C1)
 *
 * Статус: PROPOSAL v0. После согласования с C1 любое изменение номеров или
 * раскладки структур — только через bump MIGA_ABI_VERSION.
 *
 * ABI вызова (инструкция `syscall`, как в SysV, но 4-й аргумент в R10,
 * потому что RCX затирается самой инструкцией):
 *
 *   RAX = номер вызова
 *   RDI, RSI, RDX, R10, R8, R9 = аргументы 1..6
 *   RAX = результат: >= 0 успех, < 0 код ошибки (-MIGA_E*)
 *   RCX, R11 затираются (аппаратно); остальные регистры сохраняются.
 */
#ifndef MIGA_SYSCALL_H
#define MIGA_SYSCALL_H

#include <stdint.h>

#define MIGA_ABI_VERSION 0

/* ---- номера вызовов ---------------------------------------------------- */
enum miga_sys {
    SYS_EXIT        = 0,  /* (code)                          не возвращается */
    SYS_YIELD       = 1,  /* ()                                              */
    SYS_LOG         = 2,  /* (const char *s, size_t len)     len <= 256      */
    SYS_SPAWN       = 3,  /* (const struct miga_spawn_args *) → pid          */

    SYS_PORT_CREATE = 4,  /* (flags) → port id                               */
    SYS_PORT_CLOSE  = 5,  /* (port)                                          */
    SYS_SEND        = 6,  /* (port, const struct miga_msg *, flags)          */
    SYS_RECV        = 7,  /* (port, struct miga_msg *, flags)                */

    SYS_SHM_CREATE  = 8,  /* (size) → shm id                                 */
    SYS_SHM_MAP     = 9,  /* (shm, void *addr_hint, flags) → vaddr           */
    SYS_SHM_UNMAP   = 10, /* (vaddr)                                         */

    /* привилегированные: требуют capability, иначе -MIGA_EPERM */
    SYS_IOPORT      = 11, /* (op, port, value) → value  [CAP_IO_PORT]        */
    SYS_MMIO_MAP    = 12, /* (phys, size, flags) → vaddr [CAP_MMIO]          */
    SYS_IRQ_ATTACH  = 13, /* (irq, port) IRQ → сообщение в порт [CAP_IRQ]    */

    SYS_MAX
};

/* ---- ошибки ------------------------------------------------------------ */
#define MIGA_OK      0
#define MIGA_EPERM  (-1)  /* нет capability                         */
#define MIGA_EINVAL (-2)  /* неверный аргумент / номер порта        */
#define MIGA_EFAULT (-3)  /* плохой указатель из user space         */
#define MIGA_ENOSYS (-4)  /* нет такого вызова                      */
#define MIGA_EAGAIN (-5)  /* неблокирующий вызов: нет данных/места  */
#define MIGA_ENOMEM (-6)
#define MIGA_ENOENT (-7)  /* порт/shm не существует                 */
#define MIGA_EPIPE  (-8)  /* порт закрыт (разбудить блокированных)  */

/* ---- capabilities ------------------------------------------------------ */
#define CAP_IO_PORT (1u << 0)
#define CAP_MMIO    (1u << 1)
#define CAP_IRQ     (1u << 2)
#define CAP_SPAWN   (1u << 3)
#define CAP_ALL     (CAP_IO_PORT | CAP_MMIO | CAP_IRQ | CAP_SPAWN)
/* Правило: caps дочернего ⊆ caps родителя. Расширить права нельзя. */

/* ---- флаги ------------------------------------------------------------- */
#define MIGA_NONBLOCK  (1u << 0)  /* SEND/RECV: вернуть -EAGAIN, не спать */
#define MIGA_SHM_RW    (1u << 1)
#define MIGA_SHM_RO    (1u << 2)

#define IOPORT_IN8   0
#define IOPORT_IN16  1
#define IOPORT_IN32  2
#define IOPORT_OUT8  3
#define IOPORT_OUT16 4
#define IOPORT_OUT32 5

/* ---- сообщение: ровно 64 байта (одна кэш-линия) ------------------------ */
#define MIGA_MSG_DATA 48

struct miga_msg {
    uint32_t tag;                 /* протокол выбирают сами участники     */
    uint32_t len;                 /* байт в data, <= MIGA_MSG_DATA        */
    uint32_t sender;              /* pid; ЗАПОЛНЯЕТ ЯДРО, user'у не верить */
    uint32_t reserved;            /* 0                                    */
    uint8_t  data[MIGA_MSG_DATA];
};
_Static_assert(sizeof(struct miga_msg) == 64, "miga_msg must be 64 bytes");
/* Большие данные: SHM_CREATE + SHM_MAP, а в сообщении передаётся shm id. */

/* ---- spawn ------------------------------------------------------------- */
struct miga_spawn_args {
    uint32_t image_id;   /* индекс встроенного модуля (ФС/initrd пока нет) */
    uint32_t caps;       /* запрошенные права, должны быть ⊆ caps родителя */
    uint64_t arg;        /* кладётся в RDI нового процесса                 */
};

#endif /* MIGA_SYSCALL_H */
