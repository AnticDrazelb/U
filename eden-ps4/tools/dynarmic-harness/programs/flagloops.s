@ The loops Mario Kart 8 Deluxe's main thread spent the most time in (04.01's hotblocks.log),
@ each run for a few iterations, r0 times: a fill (0x2533e2c), a copy (0x25481e4), a byte copy
@ (0x10c2e04), a test-and-branch chain (0x10c2ea4/0x10c2ebc), an indexed copy (0x239da84).
.arm
.text
start:
    sub r12, sp, #4096          @ a buffer below the stack
outer:
    @ fill: 96 bytes, 32 at a time
    mov r1, #0
    mov r2, #96
    mov r3, r12
fill:
    str r1, [r3]
    sub r2, r2, #32
    str r1, [r3, #4]
    cmp r2, #31
    str r1, [r3, #8]
    str r1, [r3, #12]
    str r1, [r3, #16]
    str r1, [r3, #20]
    str r1, [r3, #24]
    str r1, [r3, #28]
    add r3, r3, #32
    bhi fill
    @ copy: 64 bytes, 16 at a time
    mov r1, r12
    add r3, r12, #512
    mov r2, #64
copy:
    ldm r1, {r6, r7}
    sub r2, r2, #16
    cmp r2, #15
    ldr r5, [r1, #8]
    ldr r4, [r1, #12]
    add r1, r1, #16
    stm r3, {r6, r7}
    str r5, [r3, #8]
    str r4, [r3, #12]
    add r3, r3, #16
    bhi copy
    @ byte copy: 8 bytes
    mov r7, #8
    add r5, r12, #1024
    mov r4, #512
bytes:
    ldrb r6, [r5, -r4]
    subs r7, r7, #1
    strb r6, [r5]
    add r5, r5, #1
    bne bytes
    @ test-and-branch chain
    mov r9, #3
    mov r11, #0xff
    mov r1, r12
tst1:
    tst r9, #255
    bne tst2
    b tst3
tst2:
    ldrb r5, [r1]
    and r6, r11, r9
    tst r6, #255
    beq tst3
    sub r9, r9, #1
    b tst1
tst3:
    @ indexed copy: 6 words
    mov r1, #0
    mov r10, #6
    add r7, r12, #256
    add r0, r0, #0              @ (r0 is the outer counter: keep it)
    push {r0}
    add r0, r12, #768
icopy:
    ldr r2, [r7], #4
    str r2, [r0, r1, lsl #2]
    add r1, r1, #1
    cmp r1, r10
    blo icopy
    pop {r0}
    subs r0, r0, #1
    bne outer
    svc #0
