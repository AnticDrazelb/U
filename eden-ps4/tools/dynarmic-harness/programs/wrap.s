@ Loads and stores whose base plus offset passes 4 GiB (wraps to the bottom of the address space),
@ beside ordinary ones and ones on the marked and unmapped pages: for fold_address_offsets (04.05),
@ where such an address faults in the arena and the fallback rebuilds it with the 32-bit wrap.
.arm
.text
start:
    movw r12, #0x0000
    movt r12, #0x1004        @ data
    mvn r1, #0x0f            @ 0xfffffff0
    mov r9, #0
wloop:
    ldr r0, [r1, #0x20]      @ wraps to 0x10: unmapped, the read callback
    str r0, [r12, #4]
    ldrb r2, [r1, #0x30]
    str r2, [r12, #8]
    str r9, [r1, #0x40]      @ a write that wraps
    ldr r3, [r12, #12]
    add r3, r3, #1
    str r3, [r12, #12]
    ldr r4, [r12, #0x100]
    add r4, r4, r9
    str r4, [r12, #0x104]
    movw r5, #0x0000
    movt r5, #0x1080         @ the marked page
    ldr r6, [r5, #0x10]
    str r6, [r12, #0x10]
    str r9, [r5, #0x20]
    movw r7, #0x0000
    movt r7, #0x1090         @ the unmapped page
    ldr r8, [r7, #0x14]
    str r8, [r12, #0x14]
    str r9, [r7, #0x24]
    ldr r10, [r12, #0x200]
    str r10, [r12, #0x18]
    add r9, r9, #1
    cmp r9, #20
    bne wloop
    svc #0
