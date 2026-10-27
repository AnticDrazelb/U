// Entry point and Horizon kernel calls (SVCs) for hello.c, as libnx writes them: arguments in
// x0-x7, results in w0 and x1-x7, which the AArch64 calling convention lets a callee overwrite.

    .section .text.start, "ax"
    .global _start
_start:
    bl      hello_main          // never returns: it ends with svcExitProcess

    .text
    .macro svc_func name, number
    .global \name
    .type \name, %function
\name:
    .endm

// Result svcSetHeapSize(u64 *out_address, u64 size)
svc_func svcSetHeapSize
    str     x0, [sp, #-16]!
    svc     0x1
    ldr     x2, [sp], #16
    str     x1, [x2]
    ret

// void svcExitProcess(void): never returns. The kernel call itself does (Eden asks its frontend
// to end the process and resumes the thread), so the thread then sleeps until it is ended.
svc_func svcExitProcess
    svc     0x7
1:  movz    x0, #0xCA00         // 1 s
    movk    x0, #0x3B9A, lsl #16
    svc     0xB
    b       1b

// Result svcCreateThread(u32 *out_handle, void (*entry)(void *), void *arg, void *stack_top,
//                        int priority, int processor_id)
svc_func svcCreateThread
    str     x0, [sp, #-16]!
    svc     0x8
    ldr     x2, [sp], #16
    str     w1, [x2]
    ret

// Result svcStartThread(u32 handle)
svc_func svcStartThread
    svc     0x9
    ret

// void svcExitThread(void)
svc_func svcExitThread
    svc     0xA
    ret

// void svcSleepThread(s64 nanoseconds)
svc_func svcSleepThread
    svc     0xB
    ret

// Result svcGetThreadPriority(int *out_priority, u32 handle)
svc_func svcGetThreadPriority
    str     x0, [sp, #-16]!
    svc     0xC
    ldr     x2, [sp], #16
    str     w1, [x2]
    ret

// u32 svcGetCurrentProcessorNumber(void)
svc_func svcGetCurrentProcessorNumber
    svc     0x10
    ret

// Result svcCloseHandle(u32 handle)
svc_func svcCloseHandle
    svc     0x16
    ret

// Result svcWaitSynchronization(int *out_index, const u32 *handles, int count, s64 timeout_ns)
svc_func svcWaitSynchronization
    str     x0, [sp, #-16]!
    svc     0x18
    ldr     x2, [sp], #16
    str     w1, [x2]
    ret

// u64 svcGetSystemTick(void): 19.2 MHz
svc_func svcGetSystemTick
    svc     0x1E
    ret

// Result svcOutputDebugString(const char *text, u64 length)
svc_func svcOutputDebugString
    svc     0x27
    ret

// Result svcGetInfo(u64 *out, u32 info_type, u32 handle, u64 sub_id)
svc_func svcGetInfo
    str     x0, [sp, #-16]!
    svc     0x29
    ldr     x2, [sp], #16
    str     x1, [x2]
    ret
