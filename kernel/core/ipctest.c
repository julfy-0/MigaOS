/* Самотест C4: make proctest N=6.
 *
 * Два РЕАЛЬНЫХ процесса кольца 3 (свои CR3), всё через инструкцию `syscall`:
 *   parent (CAP_ALL): создаёт порт, спавнит child с caps=0 и id порта в RDI, блокирующе принимает 2 сообщения.
 *   child  (caps=0):  шлёт "hello", затем пробует IOPORT, MMIO_MAP и SPAWN и присылает три кода ответа.
 * Ожидается: все три = MIGA_EPERM (-1). Затем parent сам проверяет, что с CAP_IO_PORT порт читается, а окно
 * MMIO поверх ядра (0x200000) всё равно отклоняется. Итог parent пишет в своё слово `res` (1 = PASS, 2 = FAIL),
 * ядро читает его через code_pa (как st_user в proc.c). Строки только ASCII: шрифт fbcon 8x16. */
#ifdef PROCTEST
#include <kernel/kernel.h>
#include <kernel/arch.h>
#include <kernel/proc.h>
#include <kernel/syscall.h>
#include <miga/time.h>

__asm__(
    ".pushsection .rodata\n"
    ".macro SC nr\n    movl $\\nr, %eax\n    syscall\n.endm\n"
    ".macro STR name, text\n\\name: .ascii \"\\text\"\n    .equ \\name\\()_len, . - \\name\n.endm\n"
    ".macro LOG name\n    leaq \\name(%rip), %rdi\n    movl $\\name\\()_len, %esi\n    SC 2\n.endm\n"

    /* ---------------------------------------------------------------- parent */
    ".balign 16\n"
    ".globl ublob_ipc_parent_start, ublob_ipc_parent_end, ublob_ipc_parent_res\n"
    "ublob_ipc_parent_start:\n"
    "    subq $256, %rsp\n"
    "    LOG p_start\n"
    "    xorl %edi, %edi\n"
    "    SC 4\n"                                   /* PORT_CREATE */
    "    testq %rax, %rax\n    jle pfail\n"
    "    movq %rax, %r12\n"
    "    movl $1, 64(%rsp)\n    movl $0, 68(%rsp)\n    movq %r12, 72(%rsp)\n"   /* spawn_args {image 1, caps 0, arg = port} */
    "    leaq 64(%rsp), %rdi\n"
    "    SC 3\n"                                   /* SPAWN */
    "    testq %rax, %rax\n    jle pfail\n"
    "    movq %r12, %rdi\n    movq %rsp, %rsi\n    xorl %edx, %edx\n"
    "    SC 7\n"                                   /* RECV, блокирующий: ребёнок ещё не запущен */
    "    cmpq $5, %rax\n    jne pfail\n"
    "    cmpl $0xC1D, 0(%rsp)\n    jne pfail\n"
    "    cmpl $0, 8(%rsp)\n    je pfail\n"          /* sender заполнило ядро и это не 0 */
    "    cmpl $0x6C6C6568, 16(%rsp)\n    jne pfail\n"
    "    cmpb $0x6F, 20(%rsp)\n    jne pfail\n"
    "    LOG p_got\n"
    "    movq %r12, %rdi\n    movq %rsp, %rsi\n    xorl %edx, %edx\n"
    "    SC 7\n"
    "    cmpq $12, %rax\n    jne pfail\n"
    "    cmpl $0xC1E, 0(%rsp)\n    jne pfail\n"
    "    cmpl $-1, 16(%rsp)\n    jne pfail\n"      /* IOPORT  без CAP_IO_PORT -> EPERM */
    "    cmpl $-1, 20(%rsp)\n    jne pfail\n"      /* MMIO    без CAP_MMIO    -> EPERM */
    "    cmpl $-1, 24(%rsp)\n    jne pfail\n"      /* SPAWN   без CAP_SPAWN   -> EPERM */
    "    LOG p_denied\n"
    "    xorl %edi, %edi\n    movl $0x80, %esi\n    xorl %edx, %edx\n"
    "    SC 11\n"                                  /* IOPORT IN8 0x80 с правом: не ошибка */
    "    testq %rax, %rax\n    js pfail\n"
    "    movl $0x200000, %edi\n    movl $4096, %esi\n    xorl %edx, %edx\n"
    "    SC 12\n"                                  /* MMIO поверх ядра отклоняется даже с CAP_MMIO */
    "    cmpq $-1, %rax\n    jne pfail\n"
    "    movq %rsp, %rdi\n    movl $0x1000, %esi\n    SC 2\n"   /* LOG длиной 4096 > MIGA_LOG_MAX -> EINVAL */
    "    cmpq $-2, %rax\n    jne pfail\n"
    "    movabsq $0xFFFF800000200000, %rdi\n    movl $8, %esi\n    SC 2\n"   /* указатель в ядро: EFAULT (-3), не #PF */
    "    cmpq $-3, %rax\n    jne pfail\n"
    "    movq $0x200000, %rdi\n    movl $8, %esi\n    SC 2\n"                /* user-адрес без отображения: EFAULT */
    "    cmpq $-3, %rax\n    jne pfail\n"
    "    LOG p_pass\n"
    "    movq $1, ublob_ipc_parent_res(%rip)\n"
    "    jmp pspin\n"
    "pfail:\n"
    "    LOG p_fail\n"
    "    movq $2, ublob_ipc_parent_res(%rip)\n"
    "pspin:\n    SC 1\n    jmp pspin\n"
    "    STR p_start, \"parent: start\"\n"
    "    STR p_got, \"parent: got hello from child (sender set by kernel)\"\n"
    "    STR p_denied, \"parent: child was denied io, mmio, spawn (EPERM)\"\n"
    "    STR p_pass, \"parent: IPC and capabilities PASS\"\n"
    "    STR p_fail, \"parent: FAIL\"\n"
    ".balign 8\n"
    "ublob_ipc_parent_res: .quad 0\n"
    "ublob_ipc_parent_end:\n"

    /* ----------------------------------------------------------------- child */
    ".balign 16\n"
    ".globl ublob_ipc_child_start, ublob_ipc_child_end\n"
    "ublob_ipc_child_start:\n"
    "    subq $256, %rsp\n"
    "    movq %rdi, %r12\n"
    "    movl $0xC1D, 0(%rsp)\n    movl $5, 4(%rsp)\n    movl $0, 8(%rsp)\n    movl $0, 12(%rsp)\n"
    "    movl $0x6C6C6568, 16(%rsp)\n    movb $0x6F, 20(%rsp)\n"
    "    movq %r12, %rdi\n    movq %rsp, %rsi\n    xorl %edx, %edx\n"
    "    SC 6\n"                                   /* SEND hello */
    "    testq %rax, %rax\n    jnz cexit\n"
    "    xorl %edi, %edi\n    movl $0x80, %esi\n    xorl %edx, %edx\n"
    "    SC 11\n    movl %eax, %r13d\n"
    "    movl $0xFEE00000, %edi\n    movl $4096, %esi\n    xorl %edx, %edx\n"
    "    SC 12\n    movl %eax, %r14d\n"
    "    movl $2, 64(%rsp)\n    movl $0, 68(%rsp)\n    movq $0, 72(%rsp)\n"
    "    leaq 64(%rsp), %rdi\n"
    "    SC 3\n    movl %eax, %r15d\n"
    "    movl $0xC1E, 0(%rsp)\n    movl $12, 4(%rsp)\n"
    "    movl %r13d, 16(%rsp)\n    movl %r14d, 20(%rsp)\n    movl %r15d, 24(%rsp)\n"
    "    movq %r12, %rdi\n    movq %rsp, %rsi\n    xorl %edx, %edx\n"
    "    SC 6\n"
    "    LOG c_done\n"
    "cexit:\n    xorl %edi, %edi\n    SC 0\n"
    "    STR c_done, \"child: sent results, exiting\"\n"
    "ublob_ipc_child_end:\n"
    ".popsection\n");

extern const char ublob_ipc_parent_start[], ublob_ipc_parent_end[], ublob_ipc_parent_res[];
extern const char ublob_ipc_child_start[], ublob_ipc_child_end[];

void ipc_selftest(void)
{
    cpu_sti();
    image_register(0, ublob_ipc_parent_start, (size_t)(ublob_ipc_parent_end - ublob_ipc_parent_start));
    image_register(1, ublob_ipc_child_start, (size_t)(ublob_ipc_child_end - ublob_ipc_child_start));

    int pid = syscall_spawn_image(0, CAP_ALL, 0);
    if (pid < 0) { kprintf("[test6] cannot spawn parent: %d\nFAIL\n", pid); return; }

    /* parent крутит SYS_YIELD до конца теста и не выходит, поэтому его code_pa остаётся валидным */
    struct proc *p = proc_find_pid(pid);
    if (!p) { kprintf("[test6] parent pid %d vanished\nFAIL\n", pid); return; }
    volatile uint64_t *res = (volatile uint64_t *)(p->code_pa + (uint64_t)(ublob_ipc_parent_res - ublob_ipc_parent_start));

    kprintf("[test6] parent pid %d caps=ALL, child will get caps=0\n", pid);
    uint64_t end = time_ticks() + 3000;
    while (!*res && time_ticks() < end) cpu_idle();
    ipc_dump();
    kprintf("[test6] %s (res=%lu)\n", *res == 1 ? "PASS" : "FAIL", (unsigned long)*res);
}
#endif
