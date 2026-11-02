.syntax unified
.arm
.fpu neon
.text
.globl _start
_start:
    mov r3, r0               @ iterations (each: 4 KiB copied, 256 vector updates)
    movw r12, #0
    movt r12, #0x1004
    vmov.f32 q15, #1.5
0:  @ memcpy-like: 4 KiB, 32 bytes a step (vld1.8/vst1.8 of four D registers)
    mov r0, r12
    add r1, r12, #0x8000
    mov r2, #128
1:  vld1.8 {d0-d3}, [r1]!
    vst1.8 {d0-d3}, [r0]!
    subs r2, r2, #1
    bne 1b
    @ vector math: q = q + q * k, 16 bytes in and out (vld1.32/vst1.32 of a Q register)
    mov r0, r12
    mov r2, #256
2:  vld1.32 {d16-d17}, [r0]
    vld1.32 {d18-d19}, [r1]!
    vmla.f32 q8, q9, q15
    vst1.32 {d16-d17}, [r0]!
    subs r2, r2, #1
    bne 2b
    subs r3, r3, #1
    bne 0b
    svc #0
