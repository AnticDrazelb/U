.syntax unified
.arm
.text
.globl _start
_start:
    bl run
    svc #0
