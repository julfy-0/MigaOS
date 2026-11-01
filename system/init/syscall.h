#pragma once
/* ABI user<->kernel. Номера и права согласовать с C4 (там, где уже есть syscall). */
typedef unsigned long u64;

enum { SYS_WRITE = 1, SYS_EXIT = 2, SYS_SPAWN = 3, SYS_WAIT = 4, SYS_INITRD_READ = 5 };

/* права процесса (битовая маска); ядро даёт init CAP_ALL */
#define CAP_CONSOLE (1ul << 0)   /* SYS_WRITE: вывод в serial/консоль */
#define CAP_SPAWN   (1ul << 1)   /* SYS_SPAWN, SYS_WAIT */
#define CAP_INITRD  (1ul << 2)   /* SYS_INITRD_READ */
#define CAP_ALL     (~0ul)

static inline long sc3(long n, long a, long b, long c)
{
    long r;
    __asm__ volatile("syscall" : "=a"(r) : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return r;
}

static inline long sys_write(int fd, const void *p, u64 n) { return sc3(SYS_WRITE, fd, (long)p, n); }
static inline __attribute__((noreturn)) void sys_exit(int code)
{ sc3(SYS_EXIT, code, 0, 0); for (;;) __asm__ volatile("hlt"); }
static inline long sys_spawn(const char *name, u64 caps) { return sc3(SYS_SPAWN, (long)name, caps, 0); }
static inline long sys_wait(long pid, int *status) { return sc3(SYS_WAIT, pid, (long)status, 0); }
static inline long sys_initrd_read(const char *name, void *buf, u64 n)
{ return sc3(SYS_INITRD_READ, (long)name, (long)buf, n); }

/* вход: при старте rsp выровнен на 16 без адреса возврата, поэтому realign */
#define USER_ENTRY __attribute__((noreturn, force_align_arg_pointer)) void _start(void)
