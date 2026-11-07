// The switch between the emulator (64-bit code) and Xbox code (32-bit code, run by the CPU itself
// in compatibility mode). Nothing here runs where it is linked: the bench copies xbt_begin ..
// xbt_end to a page below 4 GiB (one copy per emulated core) and fills in the slots at its start.
// Everything is position-independent: rip-relative in 64-bit code, call/pop in 32-bit code.
//
//   enter64(fn, esp)   64-bit: switches to the guest's stack (esp), far-returns into 32-bit code
//                      that calls fn (cdecl; its arguments are already on that stack), far-returns
//                      back and gives the guest's edx:eax as a 64-bit value.
//   gate32             32-bit: the target of a guest call to a "kernel export". Switches to 64-bit
//                      code, calls the C function in the handler slot with a pointer to the
//                      guest's arguments on the emulator's own stack, and returns its eax to the
//                      guest. The guest's ebx, esi, edi and ebp survive (stdcall and cdecl both
//                      need them to).
//   back64             64-bit: where enter64 resumes; the fault handler also resumes a faulting
//                      guest here (with cs set back to the 64-bit selector).
    .section .rodata.xbt, "a"
    .p2align 7
    .globl xbt_begin, xbt_end, xbt_enter64, xbt_gate32, xbt_back64
xbt_begin:
slot_save_rsp:  .quad 0     // 0   the emulator's rsp while the guest runs
slot_sel32:     .quad 0     // 8   32-bit user code selector
slot_sel64:     .quad 0     // 16  64-bit user code selector (the emulator's cs)
slot_handler:   .quad 0     // 24  gate32's C function: uint32_t (*)(const uint32_t* args)
slot_guest_ds:  .quad 0     // 32  data selector for ds/es in the guest (the emulator's ss)
slot_saved_ds:  .word 0     // 40  the emulator's ds
slot_saved_es:  .word 0     // 42  the emulator's es
                .long 0
slot_guest_fs:  .quad 0     // 48  fs selector for the guest (0: fs untouched)
slot_fs_nr:     .quad 0     // 56  system call that puts the emulator's fs base back
slot_fs_a0:     .quad 0     // 64    its first argument
slot_fs_a1:     .quad 0     // 72    its second argument
slot_fs_base:   .quad 0     // 80    the emulator's fs base (for a1 to point at)
    .p2align 7

    .code64
xbt_enter64:                // uint64_t enter64(uint32_t fn /* edi */, uint32_t esp /* esi */)
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, slot_save_rsp(%rip)
    mov %ds, slot_saved_ds(%rip)
    mov %es, slot_saved_es(%rip)
    mov slot_guest_ds(%rip), %eax
    mov %eax, %ds
    mov %eax, %es
    mov %esi, %esp          // zero-extends: the guest's stack, below 4 GiB
    pushq slot_sel32(%rip)
    lea stub32(%rip), %rax
    push %rax
    mov %edi, %eax
    lretq                   // into 32-bit code

    .code32
stub32:
    call 1f
1:  pop %ecx
    mov (slot_guest_fs - 1b)(%ecx), %edx
    test %edx, %edx
    jz 2f
    mov %edx, %fs
2:  call *%eax              // the guest function
    call 3f
3:  pop %ecx
    push (slot_sel64 - 3b)(%ecx)
    lea (xbt_back64 - 3b)(%ecx), %ecx
    push %ecx
    lret                    // back to 64-bit code

    .code64
xbt_back64:
    mov slot_save_rsp(%rip), %rsp
    shl $32, %rdx
    mov %eax, %eax
    or %rdx, %rax
    mov %rax, %rbx
    mov slot_saved_ds(%rip), %ds
    mov slot_saved_es(%rip), %es
    cmpq $0, slot_guest_fs(%rip)
    je 4f
    // fs was the guest's: give the emulator its thread pointer back before any C code runs
    mov slot_fs_nr(%rip), %rax
    mov slot_fs_a0(%rip), %rdi
    mov slot_fs_a1(%rip), %rsi
    syscall
4:  mov %rbx, %rax
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %rbp
    pop %rbx
    ret

    .code32
xbt_gate32:                 // [esp] = the guest's return address, [esp + 4] = its arguments
    call 5f
5:  pop %ecx
    push (slot_sel64 - 5b)(%ecx)
    lea (gate64 - 5b)(%ecx), %ecx
    push %ecx
    lret

    .code64
gate64:
    mov %esp, %esp          // the upper half of rsp is undefined after compatibility mode
    push %rbx
    push %rsi
    push %rdi
    push %rbp               // 32 bytes on the guest's stack, then its return address
    mov %rsp, %rbx
    mov slot_save_rsp(%rip), %rsp
    sub $8, %rsp            // 16-byte aligned for the call (slot_save_rsp is 8 mod 16)
    lea 36(%rbx), %rdi
    call *slot_handler(%rip)
    mov %rbx, %rsp
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rbx
    pushq slot_sel32(%rip)
    lea gate_ret32(%rip), %rcx
    push %rcx
    lretq

    .code32
gate_ret32:
    ret

    .code64
xbt_end:
    .section .note.GNU-stack, "", @progbits
