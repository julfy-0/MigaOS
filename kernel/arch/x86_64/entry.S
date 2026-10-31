/* Вход в ядро из MigaBoot: CPU в long mode, RDI = struct boot_info*, прерывания выключены. */
    .section .text.entry, "ax"
    .globl _start
_start:
    cli
    leaq  stack_top(%rip), %rsp
    xorl  %ebp, %ebp
    call  kmain
1:  hlt
    jmp   1b

    .section .bss
    .balign 16
stack_bottom:
    .skip 32768
stack_top:

    .section .note.GNU-stack,"",@progbits
