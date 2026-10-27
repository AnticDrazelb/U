/* hello.nro's layout: the NRO header and MOD0 header (written here, as the offsets are only
 * known at link time), then code, read-only data and data, each page aligned. The image is
 * linked at 0 and runs anywhere: the code only addresses memory relative to itself. */
OUTPUT_FORMAT(elf64-littleaarch64)
ENTRY(_start)

PHDRS {
    text PT_LOAD FLAGS(5);
    rodata PT_LOAD FLAGS(4);
    data PT_LOAD FLAGS(6);
}

SECTIONS {
    . = 0;
    .nro_header : {
        LONG(0x14000000 | ((_start / 4) & 0x3FFFFFF))  /* b _start: the entry is offset 0 */
        LONG(__mod0)                                   /* MOD0 header offset */
        LONG(0x454D4F48) LONG(0x57455242)              /* "HOMEBREW" */
        LONG(0x304F524E)                               /* "NRO0" */
        LONG(0)                                        /* version */
        LONG(__end)                                    /* file size */
        LONG(0)                                        /* flags */
        LONG(0) LONG(__rodata_start)                   /* text: offset, size */
        LONG(__rodata_start) LONG(__data_start - __rodata_start)
        LONG(__data_start) LONG(__end - __data_start)
        LONG(__bss_end - __bss_start)                  /* bss size */
        LONG(0)
        QUAD(0) QUAD(0) QUAD(0) QUAD(0)                /* build id */
        LONG(0) LONG(0)
        QUAD(0) QUAD(0) QUAD(0)                        /* api info, dynstr, dynsym */
    } :text

    .mod0 : {
        __mod0 = ABSOLUTE(.);
        LONG(0x30444F4D)                               /* "MOD0" */
        LONG(0)                                        /* no dynamic section */
        LONG(__bss_start - __mod0)
        LONG(__bss_end - __mod0)
        LONG(0) LONG(0)                                /* no unwind tables */
        LONG(__bss_start - __mod0)                     /* module object */
    } :text

    .text : { *(.text.start) *(.text .text.*) } :text

    . = ALIGN(0x1000);
    __rodata_start = .;
    .rodata : { *(.rodata .rodata.*) } :rodata

    . = ALIGN(0x1000);
    __data_start = .;
    .data : { *(.data .data.*) } :data
    . = ALIGN(0x1000);
    __end = .;

    __bss_start = .;
    .bss (NOLOAD) : { *(.bss .bss.* COMMON) } :data
    . = ALIGN(0x1000);
    __bss_end = .;

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) *(.dynamic) *(.interp) }
}
