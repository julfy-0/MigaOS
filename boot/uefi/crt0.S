/* Точка входа PE/EFI. Firmware вызывает _start по MS ABI: RCX = ImageHandle, RDX = SystemTable.
 * Сначала применяем динамические релокации (образ собран как PIC .so), потом зовём efi_main. */
    .text
    .globl _start
    .type _start, @function
_start:
    pushq %rcx
    pushq %rdx
    subq  $8, %rsp
    leaq  ImageBase(%rip), %rdi
    leaq  _DYNAMIC(%rip), %rsi
    call  _relocate            /* SysV: rdi = база, rsi = .dynamic */
    addq  $8, %rsp
    popq  %rdx
    popq  %rcx
    subq  $40, %rsp            /* shadow space + выравнивание */
    call  efi_main             /* MS ABI */
    addq  $40, %rsp
    ret

/* Пустая запись в .reloc: так firmware видит, что образ перемещаемый. */
    .section .reloc, "a"
    .long 0                    /* Page RVA (значения не важны: запись типа ABSOLUTE ничего не меняет) */
    .long 10                   /* размер блока: 8 байт заголовка + одна запись */
    .word 0                    /* IMAGE_REL_BASED_ABSOLUTE */

    .section .note.GNU-stack,"",@progbits
