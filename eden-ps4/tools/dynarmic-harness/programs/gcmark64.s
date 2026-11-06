// (04.81) Little Kitty's hottest loop (04.80's profile: 25% of its samples), as Unity's Boehm GC
// scans a range for pointers: two words an iteration from the top down, each checked against the
// heap's bounds (CMP, CCMP, B.LO), the loop's exits backward branches (B.HI to code before it), its
// way back a B.LO from the third block to the first. x19: how many 4 KiB scans; x21: pointers found.
.text
.globl _start
_start:
    movz x9, #0x1020, lsl #16          // the range: 512 words at 0x10200000
    mov x10, #0
    movz x11, #0x7c15
    movk x11, #0x7f4a, lsl #16
    movk x11, #0x79b9, lsl #32
    movk x11, #0x9e37, lsl #48
fill:                                   // small numbers, every 32nd word a "pointer" into the heap
    mul x12, x10, x11
    lsr x12, x12, #40
    tst x10, #31
    b.ne 1f
    movz x12, #0x2000, lsl #16
    add x12, x12, x10, lsl #6
1:  str x12, [x9, x10, lsl #3]
    add x10, x10, #1
    cmp x10, #512
    b.lo fill
    movz x28, #0x2000, lsl #16          // the heap: [0x20000000, 0x30000000)
    movz x27, #0x3000, lsl #16
    mov x21, #0
    b start_scan
done_scan:                              // (before the loop: its exits are backward branches)
    subs x19, x19, #1
    b.eq finish
start_scan:
    mov x23, x9
    add x20, x9, #4080
scan:                                   // (block A)
    cmp x23, x20
    sub x20, x20, #16
    b.hi done_scan
    prfum pldl1strm, [x20, #-112]       // (block B)
    add x8, x20, #8
    ldr x24, [x20, #16]
    cmp x24, x28
    ccmp x24, x27, #2, hs
    b.lo found1
    cmp x23, x8
    b.hi done_scan
second:                                 // (block C)
    ldr x24, [x8]
    cmp x24, x28
    b.lo scan
    cmp x24, x27
    b.hs scan
    add x21, x21, #1
    b scan
found1:
    add x21, x21, #1
    cmp x23, x8
    b.hi done_scan
    b second
finish:
    svc #0
