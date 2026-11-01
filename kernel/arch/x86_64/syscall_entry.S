# kernel/arch/x86_64/syscall.S
#
# SYSCALL не переключает стек и не трогает RSP. На входе:
#   RCX = RIP возврата, R11 = RFLAGS, RFLAGS &= ~SFMASK (IF=0), CPL=0.
# Пока одно ядро (PIC, без APIC), сохраняем user RSP в глобальную ячейку без
# swapgs: окно безопасно, потому что IF=0 до самого sysretq. При переходе на
# SMP заменить на swapgs + per-CPU область.

    .section .bss
    .balign 16
    .global syscall_kstack_top
syscall_kstack_top:  .quad 0     # C3 обновляет при каждом переключении задач
syscall_user_rsp:    .quad 0

    .section .text
    .global syscall_entry
    .type   syscall_entry, @function
syscall_entry:
    movq    %rsp, syscall_user_rsp(%rip)
    movq    syscall_kstack_top(%rip), %rsp

    # кадр: порядок должен совпадать со struct syscall_frame в syscall.c
    pushq   syscall_user_rsp(%rip)
    pushq   %r11                 # user RFLAGS
    pushq   %rcx                 # user RIP
    pushq   %rax                 # номер / результат
    pushq   %rdi
    pushq   %rsi
    pushq   %rdx
    pushq   %r10
    pushq   %r8
    pushq   %r9
    pushq   %rbx
    pushq   %rbp
    pushq   %r12
    pushq   %r13
    pushq   %r14
    pushq   %r15                 # 16 qword = 128 байт, выравнивание 16 сохранено

    cld
    movq    %rsp, %rdi           # arg1 = struct syscall_frame *
    call    syscall_dispatch     # кладёт результат в frame->rax

    popq    %r15
    popq    %r14
    popq    %r13
    popq    %r12
    popq    %rbp
    popq    %rbx
    popq    %r9
    popq    %r8
    popq    %r10
    popq    %rdx
    popq    %rsi
    popq    %rdi
    popq    %rax
    popq    %rcx                 # RIP
    popq    %r11                 # RFLAGS (вернёт IF)
    popq    %rsp                 # user RSP
    sysretq                      # CS=STAR[63:48]+16, SS=STAR[63:48]+8, RPL=3
    .size syscall_entry, . - syscall_entry
