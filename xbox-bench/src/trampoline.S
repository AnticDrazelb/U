// The switch between the emulator (64-bit code) and Xbox code (32-bit code, run by the CPU itself
// in compatibility mode). Nothing here runs where it is linked: the bench copies xbt_begin ..
// xbt_end to memory below 4 GiB (one copy per emulated core) and fills in the slots at its start.
// Everything is position-independent: rip-relative in 64-bit code, call/pop in 32-bit code.
//
// The guest's segments (cs, ds, es, ss) may have a base B: guest address a is then linear address
// B + a (mod 4 GiB). Compatibility mode applies segment bases, 64-bit mode doesn't, so every
// address that crosses between the two is moved by B here. With B = 0 the guest sees the flat
// address space (the system's own 32-bit segments).
//
//   enter64(fn, esp)   64-bit: switches to the guest's segments and stack (esp, a guest address),
//                      far-returns into 32-bit code that calls fn (cdecl; its arguments are
//                      already on that stack), far-returns back and gives the guest's eax.
//   gate32             32-bit: the target of a guest call to a "kernel export". Switches to 64-bit
//                      code, calls the C function in the handler slot with a pointer (linear) to
//                      the guest's arguments on the emulator's own stack, and returns its eax to the
//                      guest. The guest's ebx, esi, edi and ebp survive.
//   back64             64-bit: where enter64 resumes; the fault handler also resumes a faulting
//                      guest here (with cs set back to the 64-bit selector).
    .section .rodata.xbt, "a"
    .p2align 7
    .globl xbt_begin, xbt_end, xbt_enter64, xbt_gate32, xbt_back64
xbt_begin:
slot_save_rsp:  .quad 0     // 0   the emulator's rsp while the guest runs
slot_sel32:     .quad 0     // 8   the guest's code selector (32-bit)
slot_sel64:     .quad 0     // 16  64-bit user code selector (the emulator's cs)
slot_handler:   .quad 0     // 24  gate32's C function: uint32_t (*)(const uint32_t* args)
slot_guest_ds:  .quad 0     // 32  the guest's data selector (ds, es, ss)
slot_saved_ds:  .word 0     // 40  the emulator's ds
slot_saved_es:  .word 0     // 42  the emulator's es
slot_saved_ss:  .word 0     // 44  the emulator's ss
                .word 0
slot_base:      .quad 0     // 48  the guest segments' base B
    .p2align 7

    .code64
xbt_enter64:                // uint32_t enter64(uint32_t fn /* edi */, uint32_t esp /* esi */)
    push %rbx
    push %rbp
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, slot_save_rsp(%rip)
    mov %ds, slot_saved_ds(%rip)
    mov %es, slot_saved_es(%rip)
    mov %ss, slot_saved_ss(%rip)
    mov slot_guest_ds(%rip), %eax
    mov %eax, %ds
    mov %eax, %es
    mov slot_base(%rip), %rdx
    mov %esi, %esi
    lea (%rsi,%rdx), %rsp   // the guest's stack, as a linear address while still in 64-bit mode
    mov %eax, %ss
    pushq slot_sel32(%rip)
    lea stub32(%rip), %rax
    sub %rdx, %rax          // as a guest address
    push %rax
    mov %edi, %eax
    lretq                   // into 32-bit code

    .code32
stub32:
    sub %edx, %esp          // esp as a guest address (ss has base B now)
    call *%eax              // the guest function
    call 3f
3:  pop %ecx                // a guest address
    mov (slot_base - 3b)(%ecx), %edx
    push (slot_sel64 - 3b)(%ecx)
    lea (xbt_back64 - 3b)(%ecx), %ecx
    add %edx, %ecx          // linear, for 64-bit code
    push %ecx
    lret                    // back to 64-bit code

    .code64
xbt_back64:
    mov slot_saved_ss(%rip), %ss
    mov slot_save_rsp(%rip), %rsp
    mov %eax, %eax
    mov slot_saved_ds(%rip), %ds
    mov slot_saved_es(%rip), %es
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
    mov (slot_base - 5b)(%ecx), %edx
    push (slot_sel64 - 5b)(%ecx)
    lea (gate64 - 5b)(%ecx), %ecx
    add %edx, %ecx
    push %ecx
    lret

    .code64
gate64:
    mov %esp, %esp          // the upper half of rsp is undefined after compatibility mode
    mov %edx, %edx
    add %rdx, %rsp          // linear
    push %rbx
    push %rsi
    push %rdi
    push %rbp               // 32 bytes on the guest's stack, then its return address
    mov %rsp, %rbx
    mov slot_save_rsp(%rip), %rsp
    sub $8, %rsp            // 16-byte aligned for the call (slot_save_rsp is 8 mod 16)
    lea 36(%rbx), %rdi
    call *slot_handler(%rip)
    mov %eax, %r8d
    mov %rbx, %rsp
    pop %rbp
    pop %rdi
    pop %rsi
    pop %rbx
    // a system call in the handler returns with the system's ss: the guest's again
    mov slot_guest_ds(%rip), %eax
    mov %eax, %ss
    pushq slot_sel32(%rip)
    mov slot_base(%rip), %rdx
    lea gate_ret32(%rip), %rcx
    sub %rdx, %rcx
    push %rcx
    mov %r8d, %eax
    lretq

    .code32
gate_ret32:
    sub %edx, %esp
    ret

    .code64
xbt_end:
    .section .note.GNU-stack, "", @progbits
