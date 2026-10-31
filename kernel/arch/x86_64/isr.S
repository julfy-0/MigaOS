/* Заглушки прерываний 0..47. Для векторов без кода ошибки кладём 0, затем номер вектора. */
    .macro ISR n
    .pushsection .text
    .balign 16
isr\n:
    .if (((0x40227D00 >> \n) & 1) - 1)
    pushq $0
    .endif
    pushq $\n
    jmp   isr_common
    .popsection
    .pushsection .rodata
    .quad isr\n
    .popsection
    .endm

    .section .rodata
    .balign 8
    .globl isr_stub_table
isr_stub_table:
    .text
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,41,42,43,44,45,46,47
    ISR \n
    .endr

    .text
isr_common:
    pushq %rax
    pushq %rcx
    pushq %rdx
    pushq %rbx
    pushq %rbp
    pushq %rsi
    pushq %rdi
    pushq %r8
    pushq %r9
    pushq %r10
    pushq %r11
    pushq %r12
    pushq %r13
    pushq %r14
    pushq %r15
    movq  %rsp, %rdi
    cld
    call  isr_dispatch
    popq  %r15
    popq  %r14
    popq  %r13
    popq  %r12
    popq  %r11
    popq  %r10
    popq  %r9
    popq  %r8
    popq  %rdi
    popq  %rsi
    popq  %rbp
    popq  %rbx
    popq  %rdx
    popq  %rcx
    popq  %rax
    addq  $16, %rsp
    iretq

    .section .note.GNU-stack,"",@progbits
