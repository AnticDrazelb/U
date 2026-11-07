// The switch between the emulator (64-bit code) and Xbox code (32-bit code, run by the CPU itself in
// compatibility mode, flat segments). One copy of xcs_begin .. xcs_end is put below 4 GiB at start-up
// (32-bit code can only far-return to an address below 4 GiB) and its slots filled in.
//
// While Xbox code runs, fs is the guest's: its base is the thread's KPCR (Xbox code reads its thread
// through fs:[0x28], its TLS through fs:[0x04]). Every KPCR has a host block at 0x300:
//   0x300 the emulator's fs base (its own thread pointer)    0x318 the emulator's ds, es
//   0x308 the GuestThread (C++)                              0x320 the KPCR itself
//   0x310 the emulator's rsp in the innermost xcs_enter64    0x328 the guest's esp in a kernel call
// so 64-bit code entered from the guest finds its context through fs, before any C code runs (C code
// needs the emulator's fs back first: the slot functions switch it).
//
//   xcs_enter64(fn, esp, kpcr)  calls fn (cdecl/stdcall; arguments already on the guest stack at esp)
//                               and returns edx:eax. Nests: a kernel call may call back into the guest.
//   xcs_gate32                  32-bit: target of the per-export stubs ("mov eax, ordinal; jmp").
//                               Calls the handler with the frame (below) on the emulator's stack and
//                               returns to the guest with the handler's eax, edx, popping its bytes.
//   xcs_bail64                  the fault handler resumes a guest that can't go on here: the call
//                               returns 0xbad0bad0ffffffff.
// Gate frame (on the guest stack, linear): rdi, rsi (8 each), then eax = ordinal, ecx, edx, the return
// address, the stack arguments. The handler writes the results over it: [0] eax, [1] edx, [2] bytes
// to pop above the return address.
    .section .rodata.xcs, "a"
    .p2align 7
    .globl xcs_begin, xcs_end, xcs_enter64, xcs_gate32, xcs_bail64, xcs_back64
xcs_begin:
slot_sel32:     .quad 0     // 0   32-bit user code selector
slot_sel64:     .quad 0     // 8   64-bit user code selector
slot_guest_ds:  .quad 0     // 16  data selector for ds, es
slot_handler:   .quad 0     // 24  void (*)(uint32_t* frame, uint64_t kpcr)
slot_fs_guest:  .quad 0     // 32  void (*)(uint64_t kpcr): fs base = the KPCR
slot_fs_host:   .quad 0     // 40  void (*)(uint64_t base): fs base = the emulator's
    .p2align 7

    .code64
xcs_enter64:                // uint64_t (uint32_t fn /* edi */, uint32_t esp /* esi */, uint64_t kpcr /* rdx */)
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rdx, %r12
    mov %edi, %r13d
    mov %esi, %r14d
    pushq 0x310(%r12)       // an outer xcs_enter64's rsp (nesting), restored on the way out
    mov %rsp, 0x310(%r12)   // 16-byte aligned: 8 pushes after the call's return address
    mov %ds, 0x318(%r12)
    mov %es, 0x31a(%r12)
    mov %r12, %rdi
    call *slot_fs_guest(%rip)
    mov slot_guest_ds(%rip), %eax
    mov %eax, %ds
    mov %eax, %es
    mov %r14d, %esp         // the guest's stack
    pushq slot_sel32(%rip)
    lea stub32(%rip), %rax
    push %rax
    mov %r13d, %eax
    lretq

    .code32
stub32:
    call *%eax
    push %edx
    push %eax
    call 1f
1:  pop %ecx
    push (slot_sel64 - 1b)(%ecx)
    lea (xcs_back64 - 1b)(%ecx), %ecx
    push %ecx
    lret

    .code64
xcs_back64:
    mov %esp, %esp
    mov (%rsp), %r13d
    mov 4(%rsp), %r14d
    jmp leave64
xcs_bail64:
    mov %esp, %esp
    mov $0xffffffff, %r13d
    mov $0xbad0bad0, %r14d
leave64:
    mov %fs:0x320, %r12     // fs is the guest's: its base is the KPCR
    and $-16, %rsp
    mov 0x300(%r12), %rdi
    call *slot_fs_host(%rip) // on the guest's stack: a few bytes
    mov 0x310(%r12), %rsp
    mov 0x318(%r12), %ds
    mov 0x31a(%r12), %es
    mov %r14, %rax
    shl $32, %rax
    or %r13, %rax
    popq 0x310(%r12)
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    ret

    .code32
xcs_gate32:
    push %edx
    push %ecx
    push %eax
    call 2f
2:  pop %ecx
    push (slot_sel64 - 2b)(%ecx)
    lea (gate64 - 2b)(%ecx), %ecx
    push %ecx
    lret

    .code64
gate64:
    mov %esp, %esp
    push %rsi
    push %rdi
    mov %rsp, %r13          // the frame (callee-saved across the calls below)
    mov %fs:0x320, %r12
    mov %r13, 0x328(%r12)
    and $-16, %rsp
    mov 0x300(%r12), %rdi
    call *slot_fs_host(%rip)
    mov 0x310(%r12), %rsp
    and $-16, %rsp
    mov %r13, %rdi
    mov %r12, %rsi
    call *slot_handler(%rip)
    mov %r12, %rdi
    call *slot_fs_guest(%rip)
    mov %r13, %rsp
    pop %rdi
    pop %rsi
    mov (%rsp), %eax
    mov 4(%rsp), %r8d
    mov 8(%rsp), %r9d
    mov 12(%rsp), %r10d
    lea 16(%rsp,%r9), %rsp
    pushq slot_sel32(%rip)
    push %r10
    mov %r8d, %edx
    lretq

xcs_end:

// The emulator's fs functions on Linux with FSGSBASE: wrfsbase, no system call. In compatibility
// mode a null fs faults on use, so the guest's fs gets the flat data selector first.
    .text
    .globl xcs_linux_fs_guest, xcs_linux_fs_host
xcs_linux_fs_guest:         // (uint64_t kpcr)
    mov %ss, %eax
    mov %eax, %fs
    wrfsbase %rdi
    ret
xcs_linux_fs_host:          // (uint64_t base)
    xor %eax, %eax
    mov %eax, %fs
    wrfsbase %rdi
    ret

    .section .note.GNU-stack, "", @progbits
