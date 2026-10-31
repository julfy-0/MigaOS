OUTPUT_FORMAT("elf64-x86-64")
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)
SECTIONS
{
  . = 0;
  ImageBase = .;
  . = 0x1000;          /* первая страница занята PE-заголовком */
  .text : { _text = .; *(.text) *(.text.*) . = ALIGN(16); }
  . = ALIGN(4096);
  .reloc : { *(.reloc) }
  . = ALIGN(4096);
  .data : {
    _data = .;
    *(.rodata*) *(.got.plt) *(.got) *(.data*) *(.sdata) *(.bss) *(.dynbss) *(COMMON)
    . = ALIGN(16);
  }
  . = ALIGN(4096);
  .dynamic : { *(.dynamic) }
  . = ALIGN(4096);
  .rela : { *(.rela.data*) *(.rela.got) *(.rela.dyn) *(.rela.plt) }
  . = ALIGN(4096);
  .dynsym : { *(.dynsym) }
  . = ALIGN(4096);
  .dynstr : { *(.dynstr) }
  . = ALIGN(4096);
  /* нужны линкеру для .so, но в PE-образ не попадают (objcopy -j их отфильтрует) */
  .hash     : { *(.hash) }
  .gnu.hash : { *(.gnu.hash) }
  /DISCARD/ : { *(.rela.reloc) *(.eh_frame*) *(.note*) *(.comment) *(.interp) }
}
