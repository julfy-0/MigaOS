/* include/miga/syscall.h — КОНТРАКТ user <-> kernel.
 *
 * Статус: PROPOSAL v1 (C4 -> C1). После согласования любое изменение номеров,
 * кодов ошибок или раскладки структур — только через повышение MIGA_ABI_VERSION.
 * v0 -> v1: добавлен SYS_SHM_GRANT, уточнена семантика IPC, лимиты вынесены сюда.
 *
 * ABI вызова (инструкция `syscall`; как SysV, но 4-й аргумент в R10, потому что
 * RCX затирается самой инструкцией):
 *
 *   RAX = номер вызова
 *   RDI, RSI, RDX, R10, R8, R9 = аргументы 1..6
 *   RAX = результат: >= 0 успех, < 0 код ошибки (MIGA_E*, уже со знаком минус)
 *   RCX, R11 затираются аппаратно; остальные регистры сохраняются ядром.
 *   Флаги: DF/IF/TF/NT/AC на входе сбрасываются; user RFLAGS восстанавливается.
 */
#ifndef MIGA_SYSCALL_H
#define MIGA_SYSCALL_H

#include <stdint.h>

#define MIGA_ABI_VERSION 1

/* ---- номера вызовов ---------------------------------------------------- */
enum miga_sys {
    SYS_EXIT        = 0,  /* (code)                              не возвращается      */
    SYS_YIELD       = 1,  /* ()                                                       */
    SYS_LOG         = 2,  /* (const char *s, size_t len)         len <= MIGA_LOG_MAX  */
    SYS_SPAWN       = 3,  /* (const struct miga_spawn_args *)    -> pid   [CAP_SPAWN] */

    SYS_PORT_CREATE = 4,  /* (flags=0)                           -> port id           */
    SYS_PORT_CLOSE  = 5,  /* (port)                              только владелец      */
    SYS_SEND        = 6,  /* (port, const struct miga_msg *, flags) -> 0                */
    SYS_RECV        = 7,  /* (port, struct miga_msg *, flags)    -> len; только владелец */

    SYS_SHM_CREATE  = 8,  /* (size)                              -> shm id            */
    SYS_SHM_MAP     = 9,  /* (shm, addr_hint=0, flags)           -> vaddr             */
    SYS_SHM_UNMAP   = 10, /* (vaddr)                                                  */

    /* привилегированные: без capability -> MIGA_EPERM */
    SYS_IOPORT      = 11, /* (op, port, value)  -> value        [CAP_IO_PORT]         */
    SYS_MMIO_MAP    = 12, /* (phys, size, flags=0) -> vaddr     [CAP_MMIO]            */
    SYS_IRQ_ATTACH  = 13, /* (irq, port) IRQ -> сообщение в порт [CAP_IRQ]            */

    SYS_SHM_GRANT   = 14, /* (shm, pid) разрешить pid отображать shm (только создатель) */

    SYS_MAX
};

/* ---- ошибки ------------------------------------------------------------ */
#define MIGA_OK      0
#define MIGA_EPERM  (-1)  /* нет capability / не владелец                */
#define MIGA_EINVAL (-2)  /* неверный аргумент                           */
#define MIGA_EFAULT (-3)  /* плохой указатель из user space              */
#define MIGA_ENOSYS (-4)  /* нет такого вызова                           */
#define MIGA_EAGAIN (-5)  /* неблокирующий вызов: нет данных / нет места */
#define MIGA_ENOMEM (-6)
#define MIGA_ENOENT (-7)  /* порт / shm / образ не существует            */
#define MIGA_EPIPE  (-8)  /* порт закрыт (в т.ч. пока вы спали в SEND)   */
#define MIGA_EBUSY  (-9)  /* IRQ уже занят                               */

/* ---- capabilities ------------------------------------------------------ */
#define CAP_IO_PORT (1u << 0)
#define CAP_MMIO    (1u << 1)
#define CAP_IRQ     (1u << 2)
#define CAP_SPAWN   (1u << 3)
#define CAP_ALL     (CAP_IO_PORT | CAP_MMIO | CAP_IRQ | CAP_SPAWN)
/* Правило: caps дочернего ⊆ caps родителя. Расширить права нельзя. Права не
 * отзываются и не передаются иначе, чем при SPAWN. */

/* ---- флаги ------------------------------------------------------------- */
#define MIGA_NONBLOCK  (1u << 0)  /* SEND/RECV: вернуть EAGAIN, не спать   */
#define MIGA_SHM_RO    (1u << 1)  /* SHM_MAP: только чтение (по умолчанию RW) */

#define IOPORT_IN8   0
#define IOPORT_IN16  1
#define IOPORT_IN32  2
#define IOPORT_OUT8  3
#define IOPORT_OUT16 4
#define IOPORT_OUT32 5

/* ---- лимиты (часть контракта: user может на них рассчитывать) ---------- */
#define MIGA_LOG_MAX     256
#define MIGA_PORT_DEPTH  16          /* сообщений в очереди порта             */
#define MIGA_SHM_MAX     (256u << 10) /* максимальный размер одного shm, байт */
#define MIGA_MMIO_MAX    (16u << 20)  /* максимальное окно MMIO за один вызов */

/* ---- сообщение: ровно 64 байта (одна кэш-линия) ------------------------ */
#define MIGA_MSG_DATA 48

struct miga_msg {
    uint32_t tag;                 /* протокол выбирают сами участники            */
    uint32_t len;                 /* байт в data, <= MIGA_MSG_DATA               */
    uint32_t sender;              /* pid; ЗАПОЛНЯЕТ ЯДРО. Сообщения от ядра (IRQ)
                                     приходят с sender == 0. Верить только этому */
    uint32_t reserved;            /* при SEND игнорируется, при RECV = 0         */
    uint8_t  data[MIGA_MSG_DATA];
};
_Static_assert(sizeof(struct miga_msg) == 64, "miga_msg must be 64 bytes");

/* Сообщение IRQ (sender == 0): tag = MIGA_TAG_IRQ, len = 4, data = u32 номер IRQ. */
#define MIGA_TAG_IRQ 0x00495251u

/* Порт: отправлять может любой, кто знает id (ядро не раздаёт id, их передают в arg/сообщениях);
 * получать и закрывать — только создатель. Очередь MIGA_PORT_DEPTH сообщений, порядок FIFO.
 * Владелец, шлющий в СВОЙ полный порт, получает EAGAIN даже без NONBLOCK (иначе вечный сон).
 * Закрытие порта (или выход владельца) будит всех спящих в SEND с EPIPE; старый id больше не действует. */

/* Большие данные: SHM_CREATE -> SHM_GRANT(shm, pid) -> в сообщении уходит shm id,
 * получатель делает SHM_MAP. Память общая и не синхронизируется ядром. */

/* ---- spawn ------------------------------------------------------------- */
struct miga_spawn_args {
    uint32_t image_id;   /* индекс встроенного образа (ФС/initrd пока нет)       */
    uint32_t caps;       /* запрошенные права, обязаны быть ⊆ caps родителя      */
    uint64_t arg;        /* кладётся в RDI нового процесса (например, id порта)  */
};

/* ---- user-side обёртка (необязательная, для тестов и libc позже) -------- */
static inline int64_t miga_syscall(uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3)
{
    int64_t r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(nr), "D"(a1), "S"(a2), "d"(a3)
                     : "rcx", "r11", "memory");
    return r;
}

#endif /* MIGA_SYSCALL_H */
