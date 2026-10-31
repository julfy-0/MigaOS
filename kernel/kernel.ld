OUTPUT_FORMAT(elf64-x86-64)
ENTRY(_start)

PHDRS
{
  text   PT_LOAD FLAGS(5); /* R+X */
  rodata PT_LOAD FLAGS(4); /* R   */
  data   PT_LOAD FLAGS(6); /* R+W */
}

SECTIONS
{
  . = 0x200000;
  __kernel_start = .;

  .text : { *(.text.entry) *(.text .text.*) } :text

  . = ALIGN(4096);
  .rodata : {
    *(.rodata .rodata.*)
    . = ALIGN(8);
    __drivers_start = .;
    KEEP(*(.miga_drivers))
    __drivers_end = .;
  } :rodata

  . = ALIGN(4096);
  .data : { *(.data .data.*) } :data
  .bss  : { *(.bss .bss.*) *(COMMON) } :data

  . = ALIGN(4096);
  __kernel_end = .;

  /DISCARD/ : { *(.eh_frame*) *(.comment) *(.note*) }
}
