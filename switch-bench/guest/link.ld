/* The guest image: the kernel table first (at the load address), then code and data. */
ENTRY(bench_table)
SECTIONS
{
    . = 0x80000000;
    .bench_table : { KEEP(*(.bench_table)) }
    .text : { *(.text .text.*) }
    . = ALIGN(4096);
    .rodata : { *(.rodata .rodata.*) }
    .data : { *(.data .data.*) }
    .bss : { *(.bss .bss.* COMMON) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
