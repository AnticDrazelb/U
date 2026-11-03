// (04.22) image mode for the A64 harness: x19 = frames; the result in x0
.text
.globl _start
_start:
    mov x0, x19
    bl run
    svc #0
