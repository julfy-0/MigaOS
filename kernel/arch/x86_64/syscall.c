/* kernel/core/syscall.c — инициализация syscall/sysret, диспетчер, проверка
 * указателей. Зависимости от C3 помечены [C3]: имена предварительные, нужно
 * свериться с их заголовками после слияния. */
#include <stdint.h>
#include <stddef.h>
#include <miga/syscall.h>
#include <miga/log.h>

/* ---- MSR (Intel SDM Vol.4 / Vol.2 SYSCALL, SYSRET) --------------------- */
#define MSR_EFER    0xC0000080u   /* бит 0 = SCE (SysCall Enable)           */
#define MSR_STAR    0xC0000081u   /* [47:32] kernel CS, [63:48] sysret base */
#define MSR_LSTAR   0xC0000082u   /* RIP входа в 64-битном режиме           */
#define MSR_SFMASK  0xC0000084u   /* биты RFLAGS, сбрасываемые на входе     */
#define EFER_SCE    (1ull << 0)

/* Требование к GDT (gdt.c, [C3]/[C1]): именно в таком порядке.
 *   0x08 kernel code64, 0x10 kernel data, 0x18 user data, 0x20 user code64
 * SYSCALL: CS = STAR[47:32] = 0x08,        SS = CS + 8  = 0x10
 * SYSRET64: CS = STAR[63:48]+16 = 0x20|3,  SS = STAR[63:48]+8 = 0x18|3 */
#define SEL_KCODE        0x08
#define SEL_SYSRET_BASE  0x10

/* IF, DF, TF, NT, AC: на входе IF=0 (стек ещё не переключён), DF=0 для ABI */
#define SFMASK_BITS ((1ull<<9)|(1ull<<10)|(1ull<<8)|(1ull<<14)|(1ull<<18))

static inline uint64_t rdmsr(uint32_t m){
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t m, uint64_t v){
    __asm__ volatile("wrmsr" :: "c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32)));
}

extern void syscall_entry(void);
extern uint64_t syscall_kstack_top;       /* из syscall.S */

void syscall_set_kstack(uint64_t top){ syscall_kstack_top = top; } /* [C3]: вызывать при switch */

void syscall_init(void){
    wrmsr(MSR_EFER,   rdmsr(MSR_EFER) | EFER_SCE);
    wrmsr(MSR_STAR,   ((uint64_t)SEL_SYSRET_BASE << 48) | ((uint64_t)SEL_KCODE << 32));
    wrmsr(MSR_LSTAR,  (uint64_t)syscall_entry);
    wrmsr(MSR_SFMASK, SFMASK_BITS);
}

/* ---- кадр: порядок строго обратен push-ам в syscall.S ------------------ */
struct syscall_frame {
    uint64_t r15, r14, r13, r12, rbp, rbx, r9, r8, r10, rdx, rsi, rdi;
    uint64_t rax, rip, rflags, rsp;
};

/* ---- [C3] интерфейс процессов (предварительный) ------------------------ */
struct proc;
extern struct proc *proc_current(void);
extern uint32_t     proc_pid(struct proc *p);
extern uint32_t     proc_caps(struct proc *p);
extern void         proc_exit(struct proc *p, int code);      /* не возвращается */
extern void         sched_yield(void);
extern int          proc_spawn(struct proc *parent, uint32_t image_id,
                               uint32_t caps, uint64_t arg);   /* pid или -err */
/* [C3/mm]: страница user-доступна (U=1) и, при write, W=1 в адресном
 * пространстве p. Единственный источник истины о том, что user может трогать. */
extern int          vmm_user_page_ok(struct proc *p, uint64_t va, int write);

/* ---- caps.c ------------------------------------------------------------ */
extern int caps_check(struct proc *p, uint32_t need);          /* 0 или MIGA_EPERM */
/* ---- ipc.c ------------------------------------------------------------- */
extern int64_t ipc_syscall(struct proc *p, uint64_t nr, uint64_t a1, uint64_t a2, uint64_t a3);

/* ---- user pointers ----------------------------------------------------- */
#define USER_VA_MAX 0x00007FFFFFFFFFFFull   /* [C3] подтвердить раскладку */

/* 0 если весь диапазон [va, va+len) принадлежит user и доступен постранично */
static int user_range_ok(struct proc *p, uint64_t va, uint64_t len, int write){
    if (len == 0) return 0;
    uint64_t end = va + len;
    if (end < va || end - 1 > USER_VA_MAX) return MIGA_EFAULT;   /* переполнение / ядро */
    for (uint64_t pg = va & ~0xFFFull; pg < end; pg += 0x1000)
        if (!vmm_user_page_ok(p, pg, write)) return MIGA_EFAULT;
    return 0;
}

int copy_from_user(struct proc *p, void *dst, uint64_t usrc, uint64_t len){
    int r = user_range_ok(p, usrc, len, 0);
    if (r) return r;
    __builtin_memcpy(dst, (const void *)usrc, len);
    return 0;
}
int copy_to_user(struct proc *p, uint64_t udst, const void *src, uint64_t len){
    int r = user_range_ok(p, udst, len, 1);
    if (r) return r;
    __builtin_memcpy((void *)udst, src, len);
    return 0;
}

/* ---- вызовы ------------------------------------------------------------ */
static int64_t sys_log(struct proc *p, uint64_t uptr, uint64_t len){
    char buf[257];
    if (len > 256) return MIGA_EINVAL;
    int r = copy_from_user(p, buf, uptr, len);
    if (r) return r;
    buf[len] = 0;
    klog("[pid %u] %s\n", proc_pid(p), buf);   /* имя API из <miga/log.h> уточнить */
    return (int64_t)len;
}

static int64_t sys_spawn(struct proc *p, uint64_t uargs){
    struct miga_spawn_args a;
    int r = caps_check(p, CAP_SPAWN);
    if (r) return r;
    r = copy_from_user(p, &a, uargs, sizeof a);
    if (r) return r;
    if ((a.caps & ~proc_caps(p)) != 0) return MIGA_EPERM;   /* ⊆ родителя */
    return proc_spawn(p, a.image_id, a.caps, a.arg);
}

void syscall_dispatch(struct syscall_frame *f){
    struct proc *p = proc_current();
    uint64_t nr = f->rax;
    int64_t ret;

    switch (nr) {
    case SYS_EXIT:  proc_exit(p, (int)f->rdi);               /* не вернётся */ ret = 0; break;
    case SYS_YIELD: sched_yield(); ret = 0; break;
    case SYS_LOG:   ret = sys_log(p, f->rdi, f->rsi); break;
    case SYS_SPAWN: ret = sys_spawn(p, f->rdi); break;
    default:
        ret = (nr < SYS_MAX) ? ipc_syscall(p, nr, f->rdi, f->rsi, f->rdx) : MIGA_ENOSYS;
        break;
    }
    f->rax = (uint64_t)ret;
}
