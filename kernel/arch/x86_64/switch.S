/* Переключение контекстов ядра и трамплины первого запуска задач. */
    .text

/* void ctx_switch(uint64_t *old_ksp, uint64_t new_ksp)
 * Сохраняет callee-saved регистры на текущем стеке ядра, кладёт RSP в *old_ksp, берёт RSP новой задачи. */
    .globl ctx_switch
ctx_switch:
    pushq %rbp
    pushq %rbx
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq  %rsp, (%rdi)
    movq  %rsi, %rsp
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %rbx
    popq  %rbp
    ret

/* Первый запуск потока ядра. R12 = функция, R13 = аргумент; RSP выровнен по 16. IF=0 до sti. */
    .globl kthread_tramp
kthread_tramp:
    call  sched_post_switch
    sti
    movq  %r13, %rdi
    call  *%r12
    call  kthread_exit

/* Первый запуск процесса кольца 3. Сразу над контекстом лежит кадр iretq (rip, cs, rflags, rsp, ss).
 * R13 = аргумент пользователя (RDI при старте). Остальные регистры обнуляем: ничего от ядра не утекает в кольцо 3. */
    .globl uthread_tramp
uthread_tramp:
    movq  %rsp, %rbx
    andq  $-16, %rsp
    call  sched_post_switch
    movq  %rbx, %rsp
    movq  %r13, %rdi
    xorl  %eax, %eax
    xorl  %ecx, %ecx
    xorl  %edx, %edx
    xorl  %esi, %esi
    xorl  %ebx, %ebx
    xorl  %ebp, %ebp
    xorl  %r8d, %r8d
    xorl  %r9d, %r9d
    xorl  %r10d, %r10d
    xorl  %r11d, %r11d
    xorl  %r12d, %r12d
    xorl  %r13d, %r13d
    xorl  %r14d, %r14d
    xorl  %r15d, %r15d
    iretq

    .section .note.GNU-stack,"",@progbits
