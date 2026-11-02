.arm
.text
loop:
    push {r4-r11, lr}
    vpush {d8-d15}
    add r4, r0, #1
    vpop {d8-d15}
    pop {r4-r11, lr}
    subs r0, r0, #1
    bne loop
    svc #0
