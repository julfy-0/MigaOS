/* Линкер-скрипт user-программ: без libc, три PT_LOAD с раздельными правами */
ENTRY(_start)
PHDRS {
    text   PT_LOAD FLAGS(5);   /* R+X */
    rodata PT_LOAD FLAGS(4);   /* R   */
    data   PT_LOAD FLAGS(6);   /* R+W */
}
SECTIONS {
    . = 0x400000;
    .text   : { *(.text .text.*) } :text
    . = ALIGN(0x1000);
    .rodata : { *(.rodata .rodata.*) } :rodata
    . = ALIGN(0x1000);
    .data   : { *(.data .data.*) } :data
    .bss    : { *(.bss .bss.* COMMON) } :data
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
