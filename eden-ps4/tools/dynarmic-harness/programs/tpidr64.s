// (04.18) The thread registers as Eden's A64 cores have them (TPIDR_EL0 read and written, TPIDRRO_EL0
// read, through pointers into the callbacks object), the counter and an exclusive pair: a saved
// code cache's pointers to them must follow the objects (dynbench64 cachetest)
    .text
    .global _start
_start:
    mov x5, #40
    mov x9, #0x10000000
    add x9, x9, #0x400
1:
    mrs x0, tpidr_el0
    mrs x1, tpidrro_el0
    add x0, x0, x1
    add x0, x0, x5
    msr tpidr_el0, x0
    mrs x2, cntpct_el0
    add x3, x3, x2
2:
    ldaxr x4, [x9]
    add x4, x4, x0
    stlxr w6, x4, [x9]
    cbnz w6, 2b
    sub x5, x5, #1
    cbnz x5, 1b
    mrs x7, tpidr_el0
    svc #0
