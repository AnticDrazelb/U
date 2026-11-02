.text
.globl _start
_start:
    movz x1, #0x0100
    movk x1, #0x1020, lsl #16
    str x1, [x1]
loop:
    ldr x1, [x1]
    ldr x1, [x1]
    ldr x1, [x1]
    ldr x1, [x1]
    subs x19, x19, #1
    b.ne loop
    svc #0
