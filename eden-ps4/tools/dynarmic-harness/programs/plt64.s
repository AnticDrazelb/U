// (04.81) Bluey's hottest code (04.80's profile: 29% of its samples): calls through a PLT stub
// (ADRP X16; LDR X17, [X16, #o]; ADD X16; BR X17) to a three-instruction "current thread" function
// (MRS X8, TPIDRRO_EL0; LDR X0, [X8, #504]; RET). The GOT slot in the data region (written first),
// TPIDRRO_EL0 pointing there too (DYN_TPIDRRO=0x10300000). x19: iterations, two calls each.
.text
.globl _start
.set gotpage, 0x10300000          // (lld adds the image base: the page is 0x10310000; the slot at +8 in it)
_start:
    adr x1, current_thread
    movz x9, #0x1031, lsl #16
    str x1, [x9, #8]                // the GOT slot: current_thread's address
    movz x8, #0x1030, lsl #16
    mov x1, #0x1234
    str x1, [x8, #504]              // what it returns
    mov x21, #0
loop:
    bl stub
    add x21, x21, x0
    bl stub
    eor x21, x21, x0
    subs x19, x19, #1
    b.ne loop
    svc #0
stub:
    adrp x16, gotpage
    ldr x17, [x16, #8]
    add x16, x16, #8
    br x17
current_thread:
    mrs x8, tpidrro_el0
    ldr x0, [x8, #504]
    ret
