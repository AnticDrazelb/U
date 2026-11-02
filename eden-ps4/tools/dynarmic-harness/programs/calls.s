.arm
.text
start:
loop:
    bl func
    subs r0, r0, #1
    bne loop
    svc #0
func:
    push {r4-r11, lr}
    vpush {d8-d15}
    add r4, r0, #1
    add r5, r4, r1
    eor r6, r5, r2
    vpop {d8-d15}
    pop {r4-r11, pc}
