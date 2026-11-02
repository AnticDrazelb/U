.arm
.text
    movw r4, #0x0100
    movt r4, #0x1020
loop:
    dmb ish
1:  ldrex r1, [r4]
    add r1, r1, #1
    strex r2, r1, [r4]
    cmp r2, #0
    bne 1b
    dmb ish
    ldr r5, [r4, #8]
    add r5, r5, #1
    str r5, [r4, #8]
    dmb ish
2:  ldrex r1, [r4]
    sub r1, r1, #1
    strex r2, r1, [r4]
    cmp r2, #0
    bne 2b
    dmb ish
    subs r0, r0, #1
    bne loop
    svc #0
