.text
.globl _start
_start:
    movz x20, #0x0100
    movk x20, #0x1020, lsl #16
    movz x16, #0x0800
    movk x16, #0x1040, lsl #16
    mov sp, x16
loop:
    mov x0, x20
    bl func
    subs x19, x19, #1
    b.ne loop
    svc #0
func:
    stp x29, x30, [sp, #-64]!
    mov x29, sp
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    str x25, [sp, #48]
    ldr x21, [x0, #0]
    ldr x22, [x0, #8]
    ldr w23, [x0, #16]
    add x21, x21, x22
    str x21, [x0, #24]
    str w23, [x0, #32]
    ldp x1, x2, [x0, #40]
    add x1, x1, x2
    str x1, [x0, #56]
    ldr x25, [sp, #48]
    ldp x23, x24, [sp, #32]
    ldp x21, x22, [sp, #16]
    ldp x29, x30, [sp], #64
    ret
