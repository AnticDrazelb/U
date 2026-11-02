.text
.globl _start
_start:
    movz x1, #0x0100
    movk x1, #0x1020, lsl #16
    movz x2, #0x0200
    movk x2, #0x1031, lsl #16
    movz x3, #0x0300
    movk x3, #0x1042, lsl #16
loop:
    ldr x4, [x1]
    add x1, x1, x4, lsr #60
    ldr x5, [x2]
    add x2, x2, x5, lsr #60
    str x4, [x3]
    add x3, x3, x5, lsr #61
    ldr w6, [x1, x6, lsl #2]
    subs x19, x19, #1
    b.ne loop
    svc #0
