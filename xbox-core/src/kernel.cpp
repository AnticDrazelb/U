// The high-level kernel: xboxkrnl.exe's exports, answered on the host. Kernel objects that games see
// (events, semaphores, mutants, threads, critical sections, DPCs, interrupts) live in guest memory in
// the Xbox's own layouts; waiting on them is a host condition variable over their DISPATCHER_HEADERs.
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <csetjmp>
#include <cstdarg>
#include <cstdio>
#include <set>
#include <thread>
#include <unordered_map>

#include <sched.h>

#include "core.h"
#include "kernel.h"

namespace xb {

std::atomic<bool> g_quit{false};
double g_time_limit = 0;

namespace {

// ---- the export table

struct Export
{
    u32 ordinal;
    const char* name;
    char kind;
    u32 arg_bytes;
};
const Export kExports[] = {
#define X(ordinal, name, kind, bytes) {ordinal, #name, #kind[0], bytes},
#include "exports.inc"
#undef X
};
const Export* g_by_ordinal[400] = {};
std::unordered_map<std::string, u32> g_ordinal_by_name;
using Handler = std::function<u64(Call&)>;
Handler g_handlers[400];
u32 g_data[400] = {}; // data exports: their guest addresses
std::set<u32> g_reported;
std::mutex g_report_mutex;

void Register(const char* name, Handler h)
{
    auto it = g_ordinal_by_name.find(name);
    if (it == g_ordinal_by_name.end())
        Fatal("kernel: no export named %s", name);
    g_handlers[it->second] = std::move(h);
}
#define K(name) Register(#name, [](Call & c) -> u64
#define END )

// ---- objects and handles

std::mutex g_sched; // every object's state; waits use g_cv
std::condition_variable g_cv;
std::unordered_map<u32, u64> g_pulses; // object -> pulse count (NtPulseEvent wakes current waiters)

enum ObjectType : u8 { kNotificationEvent = 0, kSynchronizationEvent = 1, kMutant = 2, kSemaphore = 5,
                       kThread = 6, kNotificationTimer = 8, kSynchronizationTimer = 9 };

struct HandleEntry
{
    u32 object;
    int kind; // 0 kernel object, 1 file, 2 directory/other
    void* host = nullptr;
};
std::mutex g_handles_mutex;
std::unordered_map<u32, HandleEntry> g_handles;
u32 g_next_handle = 0x1000;

u32 NewHandle(u32 object, int kind = 0, void* host = nullptr)
{
    std::lock_guard lock(g_handles_mutex);
    const u32 h = g_next_handle;
    g_next_handle += 4;
    g_handles[h] = {object, kind, host};
    return h;
}

void InitHeader(u32 object, u8 type, u8 size_dwords, s32 signal)
{
    Write<u8>(object, type);
    Write<u8>(object + 2, size_dwords);
    Write<s32>(object + 4, signal);
    Write<u32>(object + 8, object + 8);
    Write<u32>(object + 12, object + 8);
}

u8 Type(u32 object) { return Read<u8>(object); }
s32 Signal(u32 object) { return Read<s32>(object + 4); }
void SetSignal(u32 object, s32 v) { Write<s32>(object + 4, v); }

// Is the object signaled for `thread`? (under g_sched)
bool IsSignaled(u32 object, u32 ethread)
{
    switch (Type(object))
    {
    case kMutant:
        return Signal(object) > 0 || Read<u32>(object + 0x18) == ethread;
    default:
        return Signal(object) > 0;
    }
}

// Takes what a satisfied wait takes (under g_sched).
void Acquire(u32 object, u32 ethread)
{
    switch (Type(object))
    {
    case kSynchronizationEvent:
    case kSynchronizationTimer:
        SetSignal(object, 0);
        break;
    case kSemaphore:
        SetSignal(object, Signal(object) - 1);
        break;
    case kMutant:
        SetSignal(object, Signal(object) - 1);
        Write<u32>(object + 0x18, ethread);
        break;
    }
}

constexpr u32 STATUS_SUCCESS = 0, STATUS_TIMEOUT = 0x102, STATUS_PENDING = 0x103,
              STATUS_INVALID_HANDLE = 0xC0000008, STATUS_NO_MEMORY = 0xC0000017,
              STATUS_INVALID_PARAMETER = 0xC000000D, STATUS_NOT_IMPLEMENTED = 0xC0000002,
              STATUS_OBJECT_NAME_NOT_FOUND = 0xC0000034, STATUS_MUTANT_NOT_OWNED = 0xC0000046,
              STATUS_SEMAPHORE_LIMIT_EXCEEDED = 0xC0000047, STATUS_NO_MORE_ENTRIES = 0x8000001A;

using Clock = std::chrono::steady_clock;
const Clock::time_point g_start = Clock::now();

// An NT timeout (LARGE_INTEGER*, 100 ns units: negative relative, positive absolute) as a deadline.
bool Deadline(u32 timeout_ptr, Clock::time_point& deadline)
{
    if (!timeout_ptr)
        return false;
    const s64 t = Read<s64>(timeout_ptr);
    if (t < 0)
        deadline = Clock::now() + std::chrono::nanoseconds(-t * 100);
    else
    {
        // absolute system time: relative to now
        const s64 now = SystemTime();
        deadline = Clock::now() + std::chrono::nanoseconds(std::max<s64>(0, t - now) * 100);
    }
    return true;
}

// Waits for objects; returns the NTSTATUS (STATUS_WAIT_0 + index, or STATUS_TIMEOUT).
u32 WaitObjects(const std::vector<u32>& objects, bool all, u32 timeout_ptr)
{
    const u32 me = CurrentThread() ? CurrentThread()->ethread : 0;
    Clock::time_point deadline;
    const bool timed = Deadline(timeout_ptr, deadline);
    std::unique_lock lock(g_sched);
    std::vector<u64> pulses(objects.size());
    for (size_t i = 0; i < objects.size(); i++)
        pulses[i] = g_pulses[objects[i]];
    for (;;)
    {
        if (all)
        {
            bool ok = true;
            for (size_t i = 0; i < objects.size(); i++)
                ok = ok && (IsSignaled(objects[i], me) || g_pulses[objects[i]] != pulses[i]);
            if (ok)
            {
                for (u32 o : objects)
                    Acquire(o, me);
                return STATUS_SUCCESS;
            }
        }
        else
            for (size_t i = 0; i < objects.size(); i++)
                if (IsSignaled(objects[i], me) || g_pulses[objects[i]] != pulses[i])
                {
                    Acquire(objects[i], me);
                    return u32(i);
                }
        if (g_quit)
            return STATUS_TIMEOUT;
        if (timed)
        {
            if (g_cv.wait_until(lock, deadline) == std::cv_status::timeout && Clock::now() >= deadline)
            {
                // one last look
                for (size_t i = 0; i < objects.size() && !all; i++)
                    if (IsSignaled(objects[i], me))
                    {
                        Acquire(objects[i], me);
                        return u32(i);
                    }
                return STATUS_TIMEOUT;
            }
        }
        else
            g_cv.wait_for(lock, std::chrono::milliseconds(100));
    }
}

u32 ObjectOf(u32 handle)
{
    if (handle == 0xfffffffe) // NtCurrentThread()
        return CurrentThread() ? CurrentThread()->ethread : 0;
    std::lock_guard lock(g_handles_mutex);
    auto it = g_handles.find(handle);
    return it == g_handles.end() ? 0 : it->second.object;
}

// ---- DbgPrint: printf over the guest's cdecl arguments

std::string GuestFormat(u32 fmt, u32 args)
{
    std::string out;
    const char* f = Ptr<const char>(fmt);
    for (; *f; f++)
    {
        if (*f != '%')
        {
            out += *f;
            continue;
        }
        std::string spec = "%";
        f++;
        while (*f && std::strchr("-+ #0123456789.", *f))
            spec += *f++;
        bool longlong = false;
        while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 'I')
        {
            if (*f == 'l' && f[1] == 'l')
                longlong = true, f++;
            f++;
        }
        char buf[512];
        const char conv = *f;
        if (!conv)
            break;
        switch (conv)
        {
        case 's':
        {
            const u32 p = Read<u32>(args);
            args += 4;
            std::snprintf(buf, sizeof(buf), (spec + "s").c_str(), p ? Ptr<const char>(p) : "(null)");
            break;
        }
        case 'c': case 'd': case 'i': case 'u': case 'x': case 'X': case 'o':
            if (longlong)
            {
                const u64 v = Read<u64>(args);
                args += 8;
                std::snprintf(buf, sizeof(buf), (spec + "ll" + conv).c_str(), v);
            }
            else
            {
                const u32 v = Read<u32>(args);
                args += 4;
                std::snprintf(buf, sizeof(buf), (spec + conv).c_str(), v);
            }
            break;
        case 'p':
            std::snprintf(buf, sizeof(buf), "%08x", Read<u32>(args));
            args += 4;
            break;
        case 'f': case 'g': case 'e':
            std::snprintf(buf, sizeof(buf), (spec + conv).c_str(), Read<double>(args));
            args += 8;
            break;
        case '%':
            std::snprintf(buf, sizeof(buf), "%%");
            break;
        default:
            std::snprintf(buf, sizeof(buf), "%%%c", conv);
        }
        out += buf;
    }
    return out;
}

// ---- threads

struct ThreadState
{
    GuestThread* thread;
    std::jmp_buf exit;
    int suspend = 0;
    bool done = false;
};
std::unordered_map<GuestThread*, ThreadState*> g_threads;
std::atomic<int> g_running{0};

void ThreadDone(ThreadState* s)
{
    {
        std::lock_guard lock(g_sched);
        s->done = true;
        SetSignal(s->thread->ethread, 1);
        Write<u8>(s->thread->ethread + kthread::HasTerminated, 1);
    }
    g_cv.notify_all();
    g_running--;
}

u32 CreateThread(u32 stack, u32 tls, u32 start, u32 context, bool suspended, u32 system_routine, const char* name,
                 u32* handle_out)
{
    GuestThread* t = NewGuestThread(stack, tls, name);
    Write<u32>(t->ethread + kthread::EthreadStartAddress, start);
    auto* s = new ThreadState{t, {}, suspended ? 1 : 0, false};
    {
        std::lock_guard lock(g_sched);
        g_threads[t] = s;
    }
    if (handle_out)
        *handle_out = NewHandle(t->ethread);
    g_running++;
    StartHostThread(t, [s, start, context, system_routine] {
        {
            std::unique_lock lock(g_sched);
            while (s->suspend > 0 && !g_quit)
                g_cv.wait_for(lock, std::chrono::milliseconds(100));
        }
        if (!setjmp(s->exit) && !g_quit)
        {
            if (system_routine)
                CallGuest(s->thread, system_routine, {start, context});
            else
                CallGuest(s->thread, start, {context});
        }
        ThreadDone(s);
    });
    return t->id;
}

[[noreturn]] void ExitThread()
{
    GuestThread* t = CurrentThread();
    ThreadState* s;
    {
        std::lock_guard lock(g_sched);
        s = g_threads[t];
    }
    std::longjmp(s->exit, 1);
}

// ---- critical sections (RTL_CRITICAL_SECTION: header 0x10, LockCount 0x10, RecursionCount 0x14,
// OwningThread 0x18)

void EnterCritical(u32 cs)
{
    const u32 me = CurrentThread()->ethread;
    std::unique_lock lock(g_sched);
    for (;;)
    {
        const u32 owner = Read<u32>(cs + 0x18);
        if (!owner || owner == me)
        {
            Write<u32>(cs + 0x18, me);
            Write<s32>(cs + 0x14, Read<s32>(cs + 0x14) + 1);
            Write<s32>(cs + 0x10, Read<s32>(cs + 0x10) + 1);
            return;
        }
        g_cv.wait_for(lock, std::chrono::milliseconds(50));
    }
}

} // namespace

u32 NewHostHandle(int kind, void* host)
{
    return NewHandle(0, kind, host);
}

void* HostOf(u32 handle, int kind)
{
    std::lock_guard lock(g_handles_mutex);
    auto it = g_handles.find(handle);
    return it == g_handles.end() || it->second.kind != kind ? nullptr : it->second.host;
}

s64 SystemTime()
{
    // 100 ns units since 1601-01-01
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() / 100 + 116444736000000000LL;
}

void SignalObject(u32)
{
    g_cv.notify_all();
}

void WakeAll()
{
    g_cv.notify_all();
}

std::string GuestString(u32 ansi_string)
{
    if (!ansi_string)
        return "";
    const u16 length = Read<u16>(ansi_string);
    const u32 buffer = Read<u32>(ansi_string + 4);
    return buffer ? std::string(Ptr<const char>(buffer), length) : "";
}

u32 Call::Arg(int i) const
{
    if (fastcall)
    {
        if (i == 0)
            return frame->ecx;
        if (i == 1)
            return frame->edx;
        return frame->args[i - 2];
    }
    return frame->args[i];
}

void InitKernel()
{
    for (const Export& e : kExports)
        if (e.ordinal < 400)
        {
            g_by_ordinal[e.ordinal] = &e;
            g_ordinal_by_name[e.name] = e.ordinal;
        }
    RegisterFileExports(Register);

    // ---- debugging, firmware
    K(DbgPrint)
    {
        std::string s = GuestFormat(c.Arg(0), Guest(&c.frame->args[1]));
        while (!s.empty() && s.back() == '\n')
            s.pop_back();
        Log("DbgPrint: %s", s.c_str());
        return 0;
    } END;
    K(HalReturnToFirmware)
    {
        Log("kernel: HalReturnToFirmware(%u): the program ended", c.Arg(0));
        g_quit = true;
        WakeAll();
        ExitThread();
    } END;
    K(RtlRaiseException)
    {
        const u32 record = c.Arg(0);
        Log("kernel: RtlRaiseException: code %#x at %#x", Read<u32>(record), Read<u32>(record + 12));
        g_quit = true;
        ExitThread();
    } END;

    // ---- threads
    K(PsCreateSystemThreadEx)
    {
        // ThreadHandle, ThreadExtensionSize, KernelStackSize, TlsDataSize, ThreadId, StartRoutine,
        // StartContext, CreateSuspended, DebuggerThread, SystemRoutine
        u32 handle = 0;
        const u32 id = CreateThread(c.Arg(2), c.Arg(3), c.Arg(5), c.Arg(6), c.Arg(7) & 0xff, c.Arg(9), "guest", &handle);
        if (c.Arg(0))
            Write<u32>(c.Arg(0), handle);
        if (c.Arg(4))
            Write<u32>(c.Arg(4), id);
        if (g_verbose >= 1)
            Log("kernel: thread %u created (start %#x, stack %#x, TLS %#x)", id, c.Arg(5), c.Arg(2), c.Arg(3));
        return STATUS_SUCCESS;
    } END;
    K(PsTerminateSystemThread) { ExitThread(); } END;
    K(KeGetCurrentThread) { return CurrentThread()->ethread; } END;
    K(KeGetCurrentIrql) { return 0; } END;
    K(KeSetBasePriorityThread) { return 0; } END;
    K(KeSetPriorityThread) { return 8; } END;
    K(KeRaiseIrqlToDpcLevel) { return 0; } END;
    K(KfRaiseIrql) { return 0; } END;
    K(KfLowerIrql) { return 0; } END;
    K(NtYieldExecution) { sched_yield(); return STATUS_SUCCESS; } END;
    K(NtResumeThread)
    {
        const u32 object = ObjectOf(c.Arg(0));
        std::lock_guard lock(g_sched);
        for (auto& [t, s] : g_threads)
            if (t->ethread == object)
            {
                if (c.Arg(1))
                    Write<u32>(c.Arg(1), u32(s->suspend));
                if (s->suspend > 0)
                    s->suspend--;
            }
        g_cv.notify_all();
        return STATUS_SUCCESS;
    } END;
    K(NtSuspendThread)
    {
        if (c.Arg(1))
            Write<u32>(c.Arg(1), 0);
        return STATUS_SUCCESS; // threads that already run aren't stopped
    } END;
    K(KeDelayExecutionThread)
    {
        Clock::time_point deadline;
        if (Deadline(c.Arg(2), deadline))
            std::this_thread::sleep_until(std::min(deadline, Clock::now() + std::chrono::seconds(60)));
        return STATUS_SUCCESS;
    } END;

    // ---- time
    K(KeQuerySystemTime) { Write<s64>(c.Arg(0), SystemTime()); return 0; } END;
    K(KeQueryPerformanceCounter)
    {
        const u64 ns = u64(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - g_start).count());
        return u64(double(ns) * 0.73333333); // 733.33 MHz
    } END;
    K(KeQueryPerformanceFrequency) { return 733333333; } END;
    K(KeQueryInterruptTime) { return u64(SystemTime()); } END;

    // ---- events, semaphores, mutants
    K(NtCreateEvent)
    {
        const u32 event = AllocKernel(0x10, 16);
        InitHeader(event, c.Arg(2) == 0 ? kNotificationEvent : kSynchronizationEvent, 4, c.Arg(3) & 0xff);
        Write<u32>(c.Arg(0), NewHandle(event));
        return STATUS_SUCCESS;
    } END;
    auto set_event = [](u32 event, u32 previous_ptr, int how) {
        if (!event)
            return STATUS_INVALID_HANDLE;
        {
            std::lock_guard lock(g_sched);
            if (previous_ptr)
                Write<s32>(previous_ptr, Signal(event));
            if (how == 0)
                SetSignal(event, 1);
            else if (how == 1)
                SetSignal(event, 0);
            else
            {
                g_pulses[event]++;
                SetSignal(event, 0);
            }
        }
        g_cv.notify_all();
        return STATUS_SUCCESS;
    };
    Register("NtSetEvent", [set_event](Call& c) -> u64 { return set_event(ObjectOf(c.Arg(0)), c.Arg(1), 0); });
    Register("NtClearEvent", [set_event](Call& c) -> u64 { return set_event(ObjectOf(c.Arg(0)), 0, 1); });
    Register("NtPulseEvent", [set_event](Call& c) -> u64 { return set_event(ObjectOf(c.Arg(0)), c.Arg(1), 2); });
    K(KeInitializeEvent)
    {
        InitHeader(c.Arg(0), c.Arg(1) == 0 ? kNotificationEvent : kSynchronizationEvent, 4, c.Arg(2) & 0xff);
        return 0;
    } END;
    K(KeSetEvent)
    {
        std::lock_guard lock(g_sched);
        const s32 old = Signal(c.Arg(0));
        SetSignal(c.Arg(0), 1);
        g_cv.notify_all();
        return u32(old);
    } END;
    K(KeResetEvent)
    {
        std::lock_guard lock(g_sched);
        const s32 old = Signal(c.Arg(0));
        SetSignal(c.Arg(0), 0);
        return u32(old);
    } END;
    K(KePulseEvent)
    {
        std::lock_guard lock(g_sched);
        const s32 old = Signal(c.Arg(0));
        g_pulses[c.Arg(0)]++;
        SetSignal(c.Arg(0), 0);
        g_cv.notify_all();
        return u32(old);
    } END;
    K(NtCreateSemaphore)
    {
        const u32 sem = AllocKernel(0x14, 16);
        InitHeader(sem, kSemaphore, 5, s32(c.Arg(2)));
        Write<s32>(sem + 0x10, s32(c.Arg(3)));
        Write<u32>(c.Arg(0), NewHandle(sem));
        return STATUS_SUCCESS;
    } END;
    K(NtReleaseSemaphore)
    {
        const u32 sem = ObjectOf(c.Arg(0));
        if (!sem)
            return STATUS_INVALID_HANDLE;
        std::lock_guard lock(g_sched);
        const s32 old = Signal(sem);
        if (old + s32(c.Arg(1)) > Read<s32>(sem + 0x10))
            return STATUS_SEMAPHORE_LIMIT_EXCEEDED;
        SetSignal(sem, old + s32(c.Arg(1)));
        if (c.Arg(2))
            Write<s32>(c.Arg(2), old);
        g_cv.notify_all();
        return STATUS_SUCCESS;
    } END;
    K(KeInitializeSemaphore)
    {
        InitHeader(c.Arg(0), kSemaphore, 5, s32(c.Arg(1)));
        Write<s32>(c.Arg(0) + 0x10, s32(c.Arg(2)));
        return 0;
    } END;
    K(KeReleaseSemaphore)
    {
        std::lock_guard lock(g_sched);
        const s32 old = Signal(c.Arg(0));
        SetSignal(c.Arg(0), old + s32(c.Arg(2)));
        g_cv.notify_all();
        return u32(old);
    } END;
    K(NtCreateMutant)
    {
        const u32 m = AllocKernel(0x20, 16);
        const bool owned = c.Arg(2) & 0xff;
        InitHeader(m, kMutant, 8, owned ? 0 : 1);
        if (owned)
            Write<u32>(m + 0x18, CurrentThread()->ethread);
        Write<u32>(c.Arg(0), NewHandle(m));
        return STATUS_SUCCESS;
    } END;
    K(NtReleaseMutant)
    {
        const u32 m = ObjectOf(c.Arg(0));
        if (!m)
            return STATUS_INVALID_HANDLE;
        std::lock_guard lock(g_sched);
        if (Read<u32>(m + 0x18) != CurrentThread()->ethread)
            return STATUS_MUTANT_NOT_OWNED;
        const s32 old = Signal(m);
        if (c.Arg(1))
            Write<s32>(c.Arg(1), old);
        SetSignal(m, old + 1);
        if (old + 1 > 0)
            Write<u32>(m + 0x18, 0);
        g_cv.notify_all();
        return STATUS_SUCCESS;
    } END;

    // ---- waits
    K(NtWaitForSingleObject) { const u32 o = ObjectOf(c.Arg(0)); return o ? WaitObjects({o}, false, c.Arg(2)) : STATUS_INVALID_HANDLE; } END;
    K(NtWaitForSingleObjectEx) { const u32 o = ObjectOf(c.Arg(0)); return o ? WaitObjects({o}, false, c.Arg(3)) : STATUS_INVALID_HANDLE; } END;
    K(NtWaitForMultipleObjectsEx)
    {
        std::vector<u32> objects;
        for (u32 i = 0; i < c.Arg(0); i++)
        {
            const u32 o = ObjectOf(Read<u32>(c.Arg(1) + i * 4));
            if (!o)
                return STATUS_INVALID_HANDLE;
            objects.push_back(o);
        }
        return WaitObjects(objects, c.Arg(2) == 0, c.Arg(5));
    } END;
    K(KeWaitForSingleObject) { return WaitObjects({c.Arg(0)}, false, c.Arg(4)); } END;
    K(KeWaitForMultipleObjects)
    {
        std::vector<u32> objects;
        for (u32 i = 0; i < c.Arg(0); i++)
            objects.push_back(Read<u32>(c.Arg(1) + i * 4));
        return WaitObjects(objects, c.Arg(2) == 0, c.Arg(6));
    } END;

    // ---- handles and objects
    K(NtClose)
    {
        HandleEntry e;
        {
            std::lock_guard lock(g_handles_mutex);
            auto it = g_handles.find(c.Arg(0));
            if (it == g_handles.end())
                return STATUS_INVALID_HANDLE;
            e = it->second;
            g_handles.erase(it);
        }
        CloseHostHandle(e.kind, e.host);
        return STATUS_SUCCESS;
    } END;
    K(ObReferenceObjectByHandle)
    {
        const u32 o = ObjectOf(c.Arg(0));
        if (!o)
            return STATUS_INVALID_HANDLE;
        Write<u32>(c.Arg(2), o);
        return STATUS_SUCCESS;
    } END;
    K(ObfDereferenceObject) { return 0; } END;
    K(ObfReferenceObject) { return 0; } END;

    // ---- interlocked (fastcall)
    K(InterlockedExchange) { return __atomic_exchange_n(Ptr<u32>(c.Arg(0)), c.Arg(1), __ATOMIC_SEQ_CST); } END;
    K(InterlockedIncrement) { return __atomic_add_fetch(Ptr<s32>(c.Arg(0)), 1, __ATOMIC_SEQ_CST); } END;
    K(InterlockedDecrement) { return __atomic_sub_fetch(Ptr<s32>(c.Arg(0)), 1, __ATOMIC_SEQ_CST); } END;
    K(InterlockedExchangeAdd) { return __atomic_fetch_add(Ptr<s32>(c.Arg(0)), s32(c.Arg(1)), __ATOMIC_SEQ_CST); } END;
    K(InterlockedCompareExchange)
    {
        u32 expected = c.Arg(2);
        __atomic_compare_exchange_n(Ptr<u32>(c.Arg(0)), &expected, c.Arg(1), false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        return expected;
    } END;

    // ---- critical sections
    K(RtlInitializeCriticalSection)
    {
        InitHeader(c.Arg(0), kSynchronizationEvent, 4, 0);
        Write<s32>(c.Arg(0) + 0x10, -1);
        Write<s32>(c.Arg(0) + 0x14, 0);
        Write<u32>(c.Arg(0) + 0x18, 0);
        return 0;
    } END;
    K(RtlEnterCriticalSection) { EnterCritical(c.Arg(0)); return 0; } END;
    K(RtlTryEnterCriticalSection)
    {
        const u32 cs = c.Arg(0), me = CurrentThread()->ethread;
        std::lock_guard lock(g_sched);
        const u32 owner = Read<u32>(cs + 0x18);
        if (owner && owner != me)
            return 0;
        Write<u32>(cs + 0x18, me);
        Write<s32>(cs + 0x14, Read<s32>(cs + 0x14) + 1);
        Write<s32>(cs + 0x10, Read<s32>(cs + 0x10) + 1);
        return 1;
    } END;
    K(RtlLeaveCriticalSection)
    {
        const u32 cs = c.Arg(0);
        {
            std::lock_guard lock(g_sched);
            Write<s32>(cs + 0x10, Read<s32>(cs + 0x10) - 1);
            if (Read<s32>(cs + 0x14) - 1 <= 0)
            {
                Write<s32>(cs + 0x14, 0);
                Write<u32>(cs + 0x18, 0);
            }
            else
                Write<s32>(cs + 0x14, Read<s32>(cs + 0x14) - 1);
        }
        g_cv.notify_all();
        return 0;
    } END;

    // ---- memory
    K(MmAllocateContiguousMemory) { return AllocContiguous(c.Arg(0), 0x1000, 0, 0xffffffff); } END;
    K(MmAllocateContiguousMemoryEx) { return AllocContiguous(c.Arg(0), c.Arg(3), c.Arg(1), c.Arg(2)); } END;
    K(MmFreeContiguousMemory) { FreeContiguous(c.Arg(0)); return 0; } END;
    K(MmPersistContiguousMemory) { return 0; } END;
    K(MmGetPhysicalAddress) { return PhysicalAddress(c.Arg(0)); } END;
    K(MmQueryAllocationSize)
    {
        u32 start, size;
        return QueryAllocation(c.Arg(0), start, size) ? size : 0;
    } END;
    K(MmAllocateSystemMemory) { return AllocContiguous(c.Arg(0), 0x1000, 0, 0xffffffff); } END;
    K(MmFreeSystemMemory) { FreeContiguous(c.Arg(0)); return 0; } END;
    K(NtAllocateVirtualMemory)
    {
        // BaseAddress*, ZeroBits, RegionSize*, AllocationType, Protect
        const u32 base_ptr = c.Arg(0), size_ptr = c.Arg(2);
        const u32 want = Read<u32>(base_ptr), size = Read<u32>(size_ptr);
        u32 start, existing;
        if (want && QueryAllocation(want, start, existing))
            return STATUS_SUCCESS; // committing what is reserved: already there
        const u32 p = AllocVirtual(size, want);
        if (!p)
            return STATUS_NO_MEMORY;
        Write<u32>(base_ptr, p);
        Write<u32>(size_ptr, (size + 0xfff) & ~0xfffu);
        return STATUS_SUCCESS;
    } END;
    K(NtFreeVirtualMemory)
    {
        const u32 base = Read<u32>(c.Arg(0));
        if (c.Arg(2) & 0x8000) // MEM_RELEASE
            FreeVirtual(base);
        return STATUS_SUCCESS;
    } END;
    K(NtQueryVirtualMemory)
    {
        const u32 at = c.Arg(0), info = c.Arg(1);
        u32 start, size;
        const bool used = QueryAllocation(at, start, size);
        Write<u32>(info + 0, used ? start : at & ~0xfffu); // BaseAddress
        Write<u32>(info + 4, used ? start : 0);             // AllocationBase
        Write<u32>(info + 8, used ? 0x04 : 0);              // AllocationProtect: PAGE_READWRITE
        Write<u32>(info + 12, used ? size : 0x1000);        // RegionSize
        Write<u32>(info + 16, used ? 0x1000 : 0x10000);     // State: MEM_COMMIT / MEM_FREE
        Write<u32>(info + 20, used ? 0x04 : 0x01);          // Protect
        Write<u32>(info + 24, used ? 0x20000 : 0);          // Type: MEM_PRIVATE
        return STATUS_SUCCESS;
    } END;
    K(MmQueryStatistics)
    {
        const u32 s = c.Arg(0);
        Write<u32>(s + 4, 64 * 1024 * 1024 / 0x1000); // TotalPhysicalPages
        Write<u32>(s + 8, FreePages());               // AvailablePages
        return STATUS_SUCCESS;
    } END;

    // ---- interrupts and DPCs
    K(HalGetInterruptVector)
    {
        if (c.Arg(1))
            Write<u8>(c.Arg(1), u8(c.Arg(0) + 6)); // an IRQL above DPC level
        return c.Arg(0) + 0x30;
    } END;
    K(KeInitializeInterrupt)
    {
        // Interrupt, ServiceRoutine, ServiceContext, Vector, Irql, InterruptMode, ShareVector
        const u32 i = c.Arg(0);
        Write<u32>(i + 0, c.Arg(1));
        Write<u32>(i + 4, c.Arg(2));
        Write<u32>(i + 8, c.Arg(3) - 0x30); // BusInterruptLevel
        Write<u32>(i + 12, c.Arg(4));
        Write<u8>(i + 16, 0);
        Write<u8>(i + 18, u8(c.Arg(5)));
        return 0;
    } END;
    K(KeConnectInterrupt)
    {
        Write<u8>(c.Arg(0) + 16, 1);
        ConnectInterrupt(Read<u32>(c.Arg(0) + 8), c.Arg(0));
        return 1;
    } END;
    K(KeDisconnectInterrupt)
    {
        Write<u8>(c.Arg(0) + 16, 0);
        DisconnectInterrupt(c.Arg(0));
        return 1;
    } END;
    K(KeInitializeDpc)
    {
        const u32 d = c.Arg(0);
        Write<u16>(d, 19); // DpcObject
        Write<u8>(d + 2, 0);
        Write<u32>(d + 12, c.Arg(1));
        Write<u32>(d + 16, c.Arg(2));
        return 0;
    } END;
    K(KeInsertQueueDpc)
    {
        const u32 d = c.Arg(0);
        if (Read<u8>(d + 2))
            return 0;
        Write<u8>(d + 2, 1);
        Write<u32>(d + 20, c.Arg(1));
        Write<u32>(d + 24, c.Arg(2));
        QueueDpc(d);
        return 1;
    } END;
    K(KeRemoveQueueDpc) { Write<u8>(c.Arg(0) + 2, 0); return 1; } END;

    // ---- strings, status codes
    K(RtlInitAnsiString)
    {
        const u32 dest = c.Arg(0), src = c.Arg(1);
        const u16 len = src ? u16(std::strlen(Ptr<const char>(src))) : 0;
        Write<u16>(dest, len);
        Write<u16>(dest + 2, src ? len + 1 : 0);
        Write<u32>(dest + 4, src);
        return 0;
    } END;
    K(RtlNtStatusToDosError)
    {
        switch (c.Arg(0))
        {
        case 0: return 0;
        case STATUS_TIMEOUT: return 1460;
        case STATUS_PENDING: return 997;
        case STATUS_INVALID_HANDLE: return 6;
        case STATUS_NO_MEMORY: return 8;
        case STATUS_INVALID_PARAMETER: return 87;
        case STATUS_OBJECT_NAME_NOT_FOUND: return 2;
        case 0xC000003A: return 3;  // object path not found
        case 0xC0000022: return 5;  // access denied
        case 0xC0000011: return 38; // end of file
        case 0xC0000035: return 183; // name collision
        case STATUS_NO_MORE_ENTRIES: return 259;
        default: return 317;
        }
    } END;

    // ---- audio/video, hardware
    K(AvGetSavedDataAddress) { return 0; } END;
    K(AvSetSavedDataAddress) { return 0; } END;
    K(AvSendTVEncoderOption)
    {
        // RegisterBase, Option, Param, Result*: AV_QUERY_AV_CAPABILITIES answers an NTSC-M
        // composite/HDTV pack, 480p capable
        if (c.Arg(3))
            Write<u32>(c.Arg(3), c.Arg(1) == 6 ? 0x00400106 : 0);
        return 0;
    } END;
    K(AvSetDisplayMode)
    {
        // RegisterBase, Step, Mode, Format, Pitch, FrameBuffer
        SetDisplayMode(c.Arg(2), c.Arg(3), c.Arg(4), c.Arg(5));
        return 0;
    } END;
    K(HalReadWritePCISpace)
    {
        // BusNumber, SlotNumber, RegisterNumber, Buffer, Length, WritePCISpace
        if (!(c.Arg(5) & 0xff))
            std::memset(Ptr(c.Arg(3)), 0xff, c.Arg(4));
        return 0;
    } END;
    K(HalReadSMBusValue) { if (c.Arg(3)) Write<u32>(c.Arg(3), 0); return 0; } END;
    K(HalWriteSMBusValue) { return 0; } END;
    K(ExQueryNonVolatileSetting)
    {
        // ValueIndex, Type*, Value, ValueLength, ResultLength*
        if (c.Arg(0) == 0xffff) // the whole EEPROM: 256 bytes (zeros: no keys, no user settings)
        {
            if (c.Arg(1))
                Write<u32>(c.Arg(1), 3); // REG_BINARY
            if (c.Arg(3) < 256)
                return 0xC0000023; // STATUS_BUFFER_TOO_SMALL
            std::memset(Ptr(c.Arg(2)), 0, 256);
            if (c.Arg(4))
                Write<u32>(c.Arg(4), 256);
            return STATUS_SUCCESS;
        }
        if (c.Arg(1))
            Write<u32>(c.Arg(1), 4); // REG_DWORD
        if (c.Arg(2) && c.Arg(3))
            std::memset(Ptr(c.Arg(2)), 0, c.Arg(3));
        u32 value = 0;
        switch (c.Arg(0))
        {
        case 0x103: value = 0x00400100; break; // video: NTSC-M, 480p allowed
        case 0x104: value = 0; break;          // audio
        case 0x107: value = 1; break;          // language: English
        case 0x108: value = 1; break;          // parental control: none
        }
        if (c.Arg(2) && c.Arg(3) >= 4)
            Write<u32>(c.Arg(2), value);
        if (c.Arg(4))
            Write<u32>(c.Arg(4), 4);
        return STATUS_SUCCESS;
    } END;
    K(ExSaveNonVolatileSetting) { return STATUS_SUCCESS; } END;
}

void ResolveThunks()
{
    // data exports: guest variables with what a program reads in them
    auto data = [](u32 ordinal) -> u32 {
        if (g_data[ordinal])
            return g_data[ordinal];
        const std::string name = g_by_ordinal[ordinal]->name;
        u32 p;
        if (name == "XeImageFileName")
        {
            p = AllocKernel(8 + 64, 4);
            const char* path = "\\Device\\CdRom0\\default.xbe";
            std::strcpy(Ptr<char>(p + 8), path);
            Write<u16>(p, u16(std::strlen(path)));
            Write<u16>(p + 2, u16(std::strlen(path) + 1));
            Write<u32>(p + 4, p + 8);
        }
        else if (name == "XboxKrnlVersion")
        {
            p = AllocKernel(8, 4);
            const u16 v[4] = {1, 0, 5838, 1};
            std::memcpy(Ptr(p), v, 8);
        }
        else if (name == "XboxHardwareInfo")
        {
            p = AllocKernel(8, 4);
            Write<u32>(p, 0x20);       // Flags
            Write<u8>(p + 4, 0xA2);    // GpuRevision (NV2A A2)
            Write<u8>(p + 5, 0xD3);    // McpRevision
        }
        else if (name == "KeTickCount" || name == "KeTimeIncrement")
        {
            p = AllocKernel(4, 4);
            Write<u32>(p, name == "KeTimeIncrement" ? 10000 : 0);
            if (name == "KeTickCount")
                SetTickCountVariable(p);
        }
        else
            p = AllocKernel(0x100, 16); // object types, pointers (LaunchDataPage = 0), ...
        g_data[ordinal] = p;
        return p;
    };
    u32 count = 0, missing = 0;
    std::string missing_names;
    for (u32 at = g_xbe.thunk_table;; at += 4)
    {
        const u32 entry = Read<u32>(at);
        if (!entry)
            break;
        const u32 ordinal = entry & 0x7fffffff;
        const Export* e = ordinal < 400 ? g_by_ordinal[ordinal] : nullptr;
        if (!e)
            Fatal("kernel: the XBE imports ordinal %u, which xboxkrnl.exe doesn't have", ordinal);
        Write<u32>(at, e->kind == 'D' ? data(ordinal) : ExportStub(ordinal));
        count++;
        if (e->kind != 'D' && !g_handlers[ordinal])
        {
            missing++;
            missing_names += std::string(" ") + e->name;
        }
    }
    Log("kernel: %u imports, %u of them not implemented yet:%s", count, missing, missing_names.c_str());
}

void KernelCall(GateFrame* frame, GuestThread* thread)
{
    const u32 ordinal = frame->eax;
    const Export* e = ordinal < 400 ? g_by_ordinal[ordinal] : nullptr;
    Call c{frame, thread, e && e->kind == 'F'};
    u64 result = STATUS_NOT_IMPLEMENTED;
    if (e && g_handlers[ordinal])
    {
        if (g_verbose >= 2)
            Log("[%u] %s(%#x, %#x, %#x, %#x)", thread ? thread->id : 0, e->name, c.Arg(0), c.Arg(1), c.Arg(2), c.Arg(3));
        result = g_handlers[ordinal](c);
    }
    else
    {
        std::lock_guard lock(g_report_mutex);
        if (g_reported.insert(ordinal).second)
            Log("kernel: %s (ordinal %u) isn't implemented: returning STATUS_NOT_IMPLEMENTED", e ? e->name : "?", ordinal);
    }
    // results: eax, edx; bytes the callee pops (stdcall; fastcall's stack part)
    const u32 bytes = !e ? 0 : e->kind == 'F' ? (e->arg_bytes > 8 ? e->arg_bytes - 8 : 0) : e->arg_bytes;
    frame->eax = u32(result);
    frame->ecx = u32(result >> 32);
    frame->edx = bytes;
}

int RunXbe()
{
    // the start-up thread: the XBE's entry point, with the XBE's TLS
    const u32 tls = g_xbe.tls_directory
                        ? ((g_xbe.tls_raw_end - g_xbe.tls_raw_start + g_xbe.tls_zero_fill + 15) & ~15u) + 4
                        : 0;
    u32 handle;
    CreateThread(0x10000, tls, g_xbe.entry, 0, false, 0, "startup", &handle);
    const auto start = Clock::now();
    while (!g_quit)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (g_running == 0)
        {
            Log("kernel: every guest thread has ended");
            break;
        }
        if (g_time_limit > 0 &&
            std::chrono::duration<double>(Clock::now() - start).count() >= g_time_limit)
        {
            Log("kernel: time limit (%.1f s) reached", g_time_limit);
            break;
        }
    }
    g_quit = true;
    WakeAll();
    return 0;
}

} // namespace xb
