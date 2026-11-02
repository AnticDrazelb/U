.text
.globl _start
_start:
    movz x4, #0x0100
    movk x4, #0x1020, lsl #16
    add x6, x4, #16
loop:
1:  ldaxr w1, [x4]
    add w1, w1, #1
    stlxr w2, w1, [x4]
    cbnz w2, 1b
    ldr w5, [x4, #8]
    add w5, w5, #1
    str w5, [x4, #8]
2:  ldaxr x1, [x6]
    sub x1, x1, #1
    stlxr w2, x1, [x6]
    cbnz w2, 2b
    dmb ish
    subs x19, x19, #1
    b.ne loop
    svc #0
