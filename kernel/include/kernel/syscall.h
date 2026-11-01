#pragma once
/* Приватный интерфейс ядра для syscall / IPC / capabilities (C4).
 * Контракт с user space — include/miga/syscall.h. */
#include <stdint.h>
#include <stddef.h>
#include <miga/boot_info.h>
#include <miga/syscall.h>
#include <kernel/proc.h>

/* Кадр после входа через syscall_entry. Порядок СТРОГО обратен push-ам в syscall.S. */
struct syscall_frame {
    uint64_t r15, r14, r13, r12, rbp, rbx, r9, r8, r10, rdx, rsi, rdi;
    uint64_t rax, rip, rflags, rsp;
};

/* ---- arch/x86_64/syscall_arch.c ---------------------------------------- */
void syscall_arch_init(void);                    /* только MSR: EFER.SCE, STAR, LSTAR, SFMASK */
void syscall_set_kstack(uint64_t top);           /* стек ядра для входа по syscall; зовёт gdt_set_rsp0 */

/* ---- core/syscall.c ---------------------------------------------------- */
void syscall_init(const struct boot_info *bi);   /* снимок карты памяти + MSR; после proc_init, до первого процесса */
void syscall_dispatch(struct syscall_frame *f);
int  copy_from_user(struct proc *p, void *dst, uint64_t usrc, uint64_t len);   /* 0 или MIGA_EFAULT */
int  copy_to_user(struct proc *p, uint64_t udst, const void *src, uint64_t len);
int  user_range_ok(struct proc *p, uint64_t va, uint64_t len, int write);

/* Встроенные образы для SYS_SPAWN, пока нет ФС/ELF (C5). Образ <= 1 страницы, позиционно-независимый. */
#define MAX_IMAGES 8
int  image_register(uint32_t id, const void *code, size_t len);
/* Запуск из ядра (kmain, тесты). caps не обязаны быть подмножеством чего-либо. Возвращает pid или MIGA_E*. */
int  syscall_spawn_image(uint32_t id, uint32_t caps, uint64_t arg);

/* ---- core/caps.c ------------------------------------------------------- */
int  caps_check(const struct proc *p, uint32_t need);        /* 0 или MIGA_EPERM; need = маска, нужны ВСЕ биты */
int  caps_subset(uint32_t child, uint32_t parent);           /* 1, если child ⊆ parent */
const char *caps_str(uint32_t caps, char *buf, size_t n);    /* "io,mmio,irq,spawn" для логов */

/* ---- core/ipc.c -------------------------------------------------------- */
#define MAX_PORTS 32
#define MAX_SHM   16
#define SHM_MAX_PAGES (MIGA_SHM_MAX / 4096)

/* Карта пользовательского адресного пространства. Каждая ветка = один слот PML4 (512 ГиБ).
 *  [128] USER_BASE: код/стек (создаёт C3; uspace_destroy освобождает и ВСЕ листовые страницы)
 *  [129] SHM:  общая память. Листья принадлежат shm-объекту, поэтому ветка НЕ в дереве [128]
 *  [130] MMIO: окна устройств. Листья — чужие физические адреса, ядро их не освобождает */
#define USER_SHM_BASE  (USER_BASE + (1ULL << 39))
#define USER_MMIO_BASE (USER_BASE + (2ULL << 39))
#define USER_BRANCH_SZ (1ULL << 39)

void    ipc_init(void);          /* после sched_init (нужен CR3 ядра); зовёт syscall_init */
int64_t ipc_syscall(struct proc *p, uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3);
void    ipc_proc_exit(struct proc *p);   /* из sched_exit: закрыть порты, отцепить shm/IRQ, освободить таблицы страниц */
int     ipc_mmio_map(struct proc *p, uint64_t phys, uint64_t size, uint64_t *va_out);
int     mmio_range_allowed(uint64_t phys, uint64_t size);   /* core/syscall.c: окно не задевает RAM/ядро */
void    ipc_dump(void);                  /* отладка */
#ifdef PROCTEST
void    ipc_selftest(void);              /* core/ipctest.c; make proctest N=6 */
#endif
