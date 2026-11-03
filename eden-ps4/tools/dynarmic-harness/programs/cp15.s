@ (04.17) CP15 as Eden's: TPIDRURO/TPIDRURW read and written through the coprocessor's fields, CNTPCT
@ through a callback with the coprocessor's parent as its argument, a DMB through one without: a
@ saved code cache's pointers to them must follow the objects (cachetest ... cp15)
    .arm
    .text
    .global _start
_start:
    mov r4, #0
    mov r5, #50
1:
    mrc p15, 0, r0, c13, c0, 3
    mrc p15, 0, r1, c13, c0, 2
    add r1, r1, r0
    add r1, r1, r4
    mcr p15, 0, r1, c13, c0, 2
    mrrc p15, 0, r2, r3, c14
    add r4, r4, r2
    eor r4, r4, r3, ror #3
    mcr p15, 0, r0, c7, c10, 5
    subs r5, r5, #1
    bne 1b
    mrc p15, 0, r6, c13, c0, 2
    svc #0
