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
//   xcs_gate32                  32-bit: target of the per-export stubs ("mov fs:[0x330], eax;
//                               mov eax, ordinal; jmp").
//                               Calls the handler with the frame (below) on the emulator's stack and
//                               returns to the guest with the handler's eax, edx, popping its bytes.
//   xcs_bail64                  the fault handler resumes a guest that can't go on here: the call
//                               returns 0xbad0bad0ffffffff.
//   xcs_raise32                 32-bit: structured exception dispatch, in the guest as on the Xbox:
//                               RtlRaiseException (or a fault) returns here with eax = the
//                               EXCEPTION_RECORD and edx = the eip to report; the caller's other
//                               registers as they were. Saves them as a CONTEXT, calls the handlers
//                               of the fs:[0] chain until one takes it (an __except jumps away from
//                               here) or continues (the CONTEXT is resumed); none: KeBugCheckEx.
//   xcs_fault32                 the same for a fault: the fault handler has pushed the interrupted
//                               code's esp, edx, ecx, eax (eax on top) and set eax, edx as above.
// Gate frame (on the guest stack, linear): rdi, rsi (8 each), then eax = ordinal, ecx, edx, the return
// address, the stack arguments. The handler writes the results over it: [0] eax, [1] edx, [2] bytes
// to pop above the return address.
    .section .rodata.xcs, "a"
    .p2align 7
    .globl xcs_begin, xcs_end, xcs_enter64, xcs_gate32, xcs_bail64, xcs_back64, xcs_raise32, xcs_fault32
xcs_begin:
slot_sel32:     .quad 0     // 0   32-bit user code selector
slot_sel64:     .quad 0     // 8   64-bit user code selector
slot_guest_ds:  .quad 0     // 16  data selector for ds, es
slot_handler:   .quad 0     // 24  void (*)(uint32_t* frame, uint64_t kpcr)
slot_fs_guest:  .quad 0     // 32  void (*)(uint64_t kpcr): fs base = the KPCR
slot_fs_host:   .quad 0     // 40  void (*)(uint64_t base): fs base = the emulator's
slot_bugcheck:  .quad 0     // 48  the guest address of KeBugCheckEx's export stub
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
    // the guest's other registers at the call (its eax the stub saved at 0x330), for the kernel's
    // context functions; the flags before anything here changes them
    pushfq
    popq 0x344(%r12)
    mov %ebx, 0x334(%r12)
    mov %ebp, 0x338(%r12)
    mov %esi, 0x33c(%r12)
    mov %edi, 0x340(%r12)
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

    // The Xbox's CONTEXT: ContextFlags 0, FloatSave (516 bytes), Edi 0x208, Esi 0x20c, Ebx 0x210,
    // Edx 0x214, Ecx 0x218, Eax 0x21c, Ebp 0x220, Eip 0x224, SegCs 0x228, EFlags 0x22c, Esp 0x230,
    // SegSs 0x234; 0x238 bytes.
    .code32
xcs_raise32:
    push %esp                   // the caller's esp (push takes esp's value before it moves)
    push $0                     // edx, ecx, eax: a call's, not kept across it
    push $0
    push $0
xcs_fault32:
    sub $0x238, %esp
    mov %edi, 0x208(%esp)
    mov %esi, 0x20c(%esp)
    mov %ebx, 0x210(%esp)
    mov 0x238(%esp), %ecx
    mov %ecx, 0x21c(%esp)       // eax
    mov 0x23c(%esp), %ecx
    mov %ecx, 0x218(%esp)       // ecx
    mov 0x240(%esp), %ecx
    mov %ecx, 0x214(%esp)       // edx
    mov %ebp, 0x220(%esp)
    mov %edx, 0x224(%esp)       // eip
    mov %cs, %ecx
    mov %ecx, 0x228(%esp)
    pushf
    pop %ecx
    mov %ecx, 0x22c(%esp)
    mov 0x244(%esp), %ecx
    mov %ecx, 0x230(%esp)       // esp
    mov %ss, %ecx
    mov %ecx, 0x234(%esp)
    movl $0x10007, (%esp)       // CONTEXT_CONTROL | CONTEXT_INTEGER | CONTEXT_SEGMENTS
    mov %eax, %esi              // the record
    mov %esp, %edi              // the context
    sub $4, %esp                // the handlers' DispatcherContext
    movl $0, (%esp)
    mov %fs:0, %ebx
3:  cmp $0xffffffff, %ebx
    je 5f
    test %ebx, %ebx
    jz 5f
    push %esp                   // &DispatcherContext (push takes esp's value before it moves)
    push %edi
    push %ebx
    push %esi
    call *4(%ebx)               // handler(record, frame, context, &dispatcher context)
    add $16, %esp
    test %eax, %eax
    jz 4f                       // ExceptionContinueExecution
    cmp $1, %eax
    jne 5f                      // nested or collided: not handled here
    mov (%ebx), %ebx            // ExceptionContinueSearch: the next frame
    jmp 3b
4:  // resume the context: eip, eax, edi and eflags go below its esp, the rest straight in
    mov 0x230(%edi), %eax
    mov 0x224(%edi), %ecx
    mov 0x21c(%edi), %edx
    mov 0x22c(%edi), %ebx
    mov 0x208(%edi), %esi
    mov %ecx, -4(%eax)
    mov %edx, -8(%eax)
    mov %esi, -12(%eax)
    mov %ebx, -16(%eax)
    mov 0x210(%edi), %ebx
    mov 0x214(%edi), %edx
    mov 0x218(%edi), %ecx
    mov 0x220(%edi), %ebp
    mov 0x20c(%edi), %esi
    lea -16(%eax), %esp
    popf
    pop %edi
    pop %eax
    ret
5:  // nobody took it: KeBugCheckEx(KERNEL_MODE_EXCEPTION_NOT_HANDLED, code, address, 0, 0)
    push $0
    push $0
    push 12(%esi)
    push (%esi)
    push $0x8e
    call 6f
6:  pop %ecx
    call *(slot_bugcheck - 6b)(%ecx)
7:  jmp 7b

    .code64
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
