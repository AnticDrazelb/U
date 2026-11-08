// Structured exception handling as XDK games use it (MSVC's __try/__except/__finally through
// _except_handler3): raised exceptions caught, unwinds running __finally blocks, resumed execution,
// nested handlers, and a fault inside __try. Each check is a DbgPrint "PASS"/"FAIL" line.
#include <hal/debug.h>
#include <hal/video.h>
#include <excpt.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

static int failures = 0;
static void check(const char *what, int ok)
{
    DbgPrint("%s %s\n", ok ? "PASS" : "FAIL", what);
    debugPrint("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        failures++;
}

static void raise(DWORD code)
{
    EXCEPTION_RECORD record = {0};
    record.ExceptionCode = code;
    RtlRaiseException(&record);
}

static volatile int order[8], order_at = 0;
static void mark(int n) { order[order_at++] = n; }

static int filter_continue_search(void) { mark(1); return EXCEPTION_CONTINUE_SEARCH; }

// The fault in a function of its own: clang keeps the __try state only around calls (it doesn't do
// MSVC's asynchronous /EHa tracking), so a fault in the __try body itself wouldn't be in its scope.
__attribute__((noinline)) static void poke(volatile int *p) { *p = 1; }
__attribute__((noinline)) static int divide(volatile int a, volatile int b) { return a / b; }

int main(void)
{
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

    // 1: caught, with its code
    volatile int caught = 0;
    volatile DWORD code = 0;
    __try {
        raise(0xE0001234);
        caught = -1;
    } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        caught = 1;
    }
    check("a raised exception is caught", caught == 1);
    check("GetExceptionCode", code == 0xE0001234);

    // 2: the inner __finally runs during the unwind, before the outer __except's body
    order_at = 0;
    __try {
        __try {
            raise(0xE0000002);
        } __finally {
            mark(2);
        }
    } __except (mark(1), EXCEPTION_EXECUTE_HANDLER) {
        mark(3);
    }
    check("filter, then __finally, then the handler", order_at == 3 && order[0] == 1 && order[1] == 2 && order[2] == 3);

    // 3: a filter that continues execution: back after the raise, locals intact
    volatile int after = 0, local = 1234;
    __try {
        raise(0xE0000003);
        after = local;
    } __except (EXCEPTION_CONTINUE_EXECUTION) {
        after = -1;
    }
    check("EXCEPTION_CONTINUE_EXECUTION resumes after the raise", after == 1234);

    // 4: an inner filter passes it on, the outer takes it
    order_at = 0;
    __try {
        __try {
            raise(0xE0000004);
        } __except (filter_continue_search()) {
            mark(9);
        }
    } __except (mark(5), EXCEPTION_EXECUTE_HANDLER) {
        mark(6);
    }
    check("the outer handler takes what the inner passes on", order_at == 3 && order[0] == 1 && order[1] == 5 && order[2] == 6);

    // 5: a fault inside __try: an access violation, caught
    volatile int fault_caught = 0;
    volatile DWORD fault_code = 0;
    __try {
        poke((volatile int *)0x4);
    } __except (fault_code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        fault_caught = 1;
    }
    check("an access violation in __try is caught", fault_caught == 1 && fault_code == 0xC0000005);

    // 6: an integer division by zero
    volatile DWORD divide_code = 0;
    __try {
        divide(1, 0);
    } __except (divide_code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
    }
    check("a division by zero is caught", divide_code == 0xC0000094);

    // 7: and the program goes on normally after all of it
    check("after the exceptions", 1);
    DbgPrint("SEH test: %d failures\n", failures);
    debugPrint("SEH test: %d failures\n", failures);
    for (;;)
        Sleep(1000);
}
