/* The guest image, at an XBE's base address (0x00010000; 0x40010000 for the second layout, with
   --defsym=IMAGE_BASE=...): the kernel table first, then code and data. */
ENTRY(xb_table)
SECTIONS
{
    . = DEFINED(IMAGE_BASE) ? IMAGE_BASE : 0x00010000;
    .bench_table : { KEEP(*(.bench_table)) }
    .text : { *(.text .text.*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss : { *(.bss .bss.* COMMON) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
