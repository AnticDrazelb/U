.arm
.text
loop:
    bl func
    subs r0, r0, #1
    bne loop
    svc #0
func:
    add r4, r0, #1
    add r5, r4, r1
    eor r6, r5, r2
    bx lr
