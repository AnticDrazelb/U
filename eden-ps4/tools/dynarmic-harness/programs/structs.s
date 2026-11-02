.arm
.text
    movw r4, #0x0100
    movt r4, #0x1020
    movw r12, #0x0800
    movt r12, #0x1040
    mov sp, r12
loop:
    ldr r1, [r4, #0]
    ldr r2, [r4, #4]
    add r1, r1, r2
    str r1, [r4, #8]
    ldr r3, [r4, #12]
    str r3, [r4, #16]
    ldr r5, [sp, #4]
    ldr r6, [sp, #8]
    str r5, [sp, #12]
    str r6, [sp, #16]
    subs r0, r0, #1
    bne loop
    svc #0
