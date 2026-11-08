// The high-level kernel: xboxkrnl.exe's exports, answered on the host. Kernel objects that games see
// (events, semaphores, mutants, threads, critical sections, DPCs, interrupts) live in guest memory in
// the Xbox's own layouts; waiting on them is a host condition variable over their DISPATCHER_HEADERs.
#include <cerrno>
#include <chrono>
#include <cinttypes>
#include <csetjmp>
#include <cstdarg>
#include <cstdio>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>

#include <sched.h>

#include "core.h"
#include "diagnostics.h"
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

// ---- data exports and object types

// OBJECT_TYPE: AllocateProcedure, FreeProcedure, CloseProcedure, DeleteProcedure, ParseProcedure,
// DefaultObject, PoolTag. Close/Delete/Parse point at small routines that do nothing (the types with
// them have them on the console; the kernel's own object manager here doesn't call them).
struct TypeInfo
{
    const char* name;
    u32 tag;
    bool close, del, parse;
};
const TypeInfo kTypes[] = {
    {"ExEventObjectType", 'vevE', false, false, false},     {"ExMutantObjectType", 'atuM', false, true, false},
    {"ExSemaphoreObjectType", 'ameS', false, false, false}, {"ExTimerObjectType", 'emiT', false, true, false},
    {"IoCompletionObjectType", 'pmoC', false, true, false}, {"IoDeviceObjectType", 'iveD', false, false, true},
    {"IoFileObjectType", 'eliF', true, true, true},         {"ObDirectoryObjectType", 'eriD', false, false, false},
    {"ObSymbolicLinkObjectType", 'bmyS', false, true, false}, {"PsThreadObjectType", 'erhT', false, false, false},
};

u32 ProcedureStub(int which) // 0 close (2 args), 1 delete (1), 2 parse (7: STATUS_OBJECT_NAME_NOT_FOUND)
{
    static u32 code = 0;
    if (!code)
    {
        code = AllocKernel(32, 16);
        const u8 bytes[] = {0xc2, 0x08, 0x00, 0x90, 0xc2, 0x04, 0x00, 0x90,       // ret 8; ret 4
                            0xb8, 0x34, 0x00, 0x00, 0xc0, 0xc2, 0x1c, 0x00}; // mov eax, 0xC0000034; ret 28
        std::memcpy(Ptr(code), bytes, sizeof(bytes));
    }
    return code + (which == 0 ? 0 : which == 1 ? 4 : 8);
}

u32 Ordinal(const char* name)
{
    auto it = g_ordinal_by_name.find(name);
    return it == g_ordinal_by_name.end() ? 0 : it->second;
}

// A data export's guest variable, made on first use, with what a program reads in it.
u32 DataExport(u32 ordinal)
{
    static std::mutex mutex; // threads can make an object of a type the program doesn't import
    std::lock_guard lock(mutex);
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
    {
        p = AllocKernel(0x100, 16); // object types, pointers (LaunchDataPage = 0), ...
        for (const TypeInfo& t : kTypes)
            if (name == t.name)
            {
                Write<u32>(p + 0x00, ExportStub(Ordinal("ExAllocatePoolWithTag")));
                Write<u32>(p + 0x04, ExportStub(Ordinal("ExFreePool")));
                Write<u32>(p + 0x08, t.close ? ProcedureStub(0) : 0);
                Write<u32>(p + 0x0c, t.del ? ProcedureStub(1) : 0);
                Write<u32>(p + 0x10, t.parse ? ProcedureStub(2) : 0);
                Write<u32>(p + 0x14, 0); // DefaultObject: the object itself
                Write<u32>(p + 0x18, t.tag);
            }
    }
    g_data[ordinal] = p;
    return p;
}

u32 ObjectTypeAddress(const char* type)
{
    const u32 ordinal = type ? Ordinal(type) : 0;
    return ordinal ? DataExport(ordinal) : 0;
}

// An object with its OBJECT_HEADER (PointerCount, HandleCount, Type, Flags) before the body; the body.
u32 NewObject(u32 size, const char* type)
{
    const u32 header = AllocKernel(0x10 + size, 16);
    std::memset(Ptr(header), 0, 0x10 + size);
    Write<u32>(header + 0, 1);
    Write<u32>(header + 4, 1);
    Write<u32>(header + 8, ObjectTypeAddress(type));
    return header + 0x10;
}

u32 HeaderType(u32 body) { return Read<u32>(body - 8); }

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
struct ThreadState;
bool HasApcs(ThreadState* s, bool alertable);
bool RunApcs(bool alertable); // runs the current thread's deliverable APCs; whether a user one ran
ThreadState* CurrentState();  // under g_sched

constexpr u32 STATUS_USER_APC = 0xC0;

u32 WaitObjects(const std::vector<u32>& objects, bool all, u32 timeout_ptr, bool alertable = false)
{
    const u32 me = CurrentThread() ? CurrentThread()->ethread : 0;
    Clock::time_point deadline;
    const bool timed = Deadline(timeout_ptr, deadline);
    std::unique_lock lock(g_sched);
    std::vector<u64> pulses(objects.size());
    for (size_t i = 0; i < objects.size(); i++)
        pulses[i] = g_pulses[objects[i]];
    ThreadState* self = CurrentState();
    for (;;)
    {
        if (self && HasApcs(self, alertable))
        {
            lock.unlock();
            const bool user = RunApcs(alertable);
            lock.lock();
            if (user)
                return STATUS_USER_APC;
            continue;
        }
        if (me && Read<u8>(me + kthread::SuspendCount) && !g_quit)
        {
            // a suspended thread takes nothing: what's signaled meanwhile waits for it to resume
            g_cv.wait_for(lock, std::chrono::milliseconds(50));
            continue;
        }
        if (all && !objects.empty())
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
        else if (!all)
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
                // one last look (a suspended thread's wait just times out)
                const bool suspended = me && Read<u8>(me + kthread::SuspendCount);
                for (size_t i = 0; i < objects.size() && !all && !suspended; i++)
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
    return it == g_handles.end() || it->second.kind != 0 ? 0 : it->second.object;
}

struct Completion
{
    u32 key, apc_context, status, information;
};
std::unordered_map<u32, std::deque<Completion>> g_completions; // under g_sched
std::unordered_map<std::string, u32> g_named;                   // devices by name, under g_handles_mutex

u32 PostCompletion(u32 queue, u32 key, u32 apc_context, u32 status, u32 information)
{
    std::lock_guard lock(g_sched);
    auto it = g_completions.find(queue);
    if (it == g_completions.end())
        return STATUS_INVALID_HANDLE;
    it->second.push_back({key, apc_context, status, information});
    SetSignal(queue, s32(it->second.size()));
    g_cv.notify_all();
    return STATUS_SUCCESS;
}

std::string LowerName(std::string s)
{
    for (char& ch : s)
        if (ch >= 'A' && ch <= 'Z')
            ch = char(ch - 'A' + 'a');
    return s;
}

// Any handle's object body (files, directories and links too).
u32 BodyOf(u32 handle)
{
    if (handle == 0xfffffffe)
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
        while (*f && std::strchr("-+ #0123456789.*", *f))
        {
            if (*f == '*') // a width or precision from the arguments
            {
                spec += std::to_string(s32(Read<u32>(args)));
                args += 4;
                f++;
            }
            else
                spec += *f++;
        }
        bool longlong = false, wide = false;
        while (*f == 'l' || *f == 'h' || *f == 'z' || *f == 'I' || *f == 'w')
        {
            if (*f == 'l' && f[1] == 'l')
                longlong = true, f++;
            else if (*f == 'I' && f[1] == '6' && f[2] == '4')
                longlong = true, f += 2;
            else if (*f == 'w' || *f == 'l')
                wide = true;
            f++;
        }
        char buf[512];
        const char conv = *f;
        if (!conv)
            break;
        switch (conv)
        {
        case 's':
        case 'S':
        case 'Z': // ANSI_STRING* (UNICODE_STRING* with w)
        {
            const u32 p = Read<u32>(args);
            args += 4;
            std::string text;
            if (!p)
                text = "(null)";
            else if (conv == 'Z')
            {
                const u16 length = Read<u16>(p);
                const u32 buffer = Read<u32>(p + 4);
                for (u32 i = 0; buffer && i < (wide ? length / 2u : length); i++)
                {
                    const u16 ch = wide ? Read<u16>(buffer + i * 2) : Read<u8>(buffer + i);
                    text += ch < 0x80 ? char(ch) : '?';
                }
            }
            else if (conv == 'S' || wide)
                for (u32 q = p; Read<u16>(q); q += 2)
                    text += Read<u16>(q) < 0x80 ? char(Read<u16>(q)) : '?';
            else
                text = Ptr<const char>(p);
            std::snprintf(buf, sizeof(buf), (spec + "s").c_str(), text.c_str());
            break;
        }
        case 'C':
        {
            const u32 v = Read<u32>(args) & 0xffff;
            args += 4;
            std::snprintf(buf, sizeof(buf), (spec + "c").c_str(), v < 0x80 ? int(v) : '?');
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
    // queued APCs: a KAPC (kapc != 0: its kernel routine first) or NtQueueApcThread's routine
    struct Apc
    {
        u32 kapc, kernel_routine, normal_routine, context, arg1, arg2;
        bool user;
    };
    std::deque<Apc> apcs;
    std::deque<u32> dpcs; // DPCs queued by this thread, waiting for its IRQL to drop
};
std::unordered_map<GuestThread*, ThreadState*> g_threads;

ThreadState* CurrentState()
{
    auto it = g_threads.find(CurrentThread());
    return it == g_threads.end() ? nullptr : it->second;
}

ThreadState* StateOf(u32 ethread) // under g_sched
{
    for (auto& [t, s] : g_threads)
        if (t->ethread == ethread)
            return s;
    return nullptr;
}

// Suspension: the KTHREAD's SuspendCount (at most 0x7f), under g_sched. A suspended thread stops when
// it next leaves a kernel call (a wait it was in included), or before it starts.
int SuspendThread(u32 ethread) // the previous count, or -1 at the limit
{
    const u8 n = Read<u8>(ethread + kthread::SuspendCount);
    if (n >= 0x7f)
        return -1;
    Write<u8>(ethread + kthread::SuspendCount, u8(n + 1));
    return n;
}

int ResumeThread(u32 ethread) // the previous count
{
    const u8 n = Read<u8>(ethread + kthread::SuspendCount);
    if (n)
        Write<u8>(ethread + kthread::SuspendCount, u8(n - 1));
    g_cv.notify_all();
    return n;
}

void StopWhileSuspended(GuestThread* t)
{
    std::unique_lock lock(g_sched);
    while (Read<u8>(t->ethread + kthread::SuspendCount) && !g_quit)
        g_cv.wait_for(lock, std::chrono::milliseconds(50));
}

// From a kernel call: raises the exception `record` describes in the calling thread, dispatched in the
// guest (switch.S, xcs_raise32) as the kernel's own exceptions are. The handler returns what this gives.
u64 RaiseRecord(Call& c, u32 record)
{
    if (!Read<u32>(record + 12))
        Write<u32>(record + 12, c.frame->ret); // ExceptionAddress: where it was raised
    if (diag::g_exceptions++ < 100 || g_verbose >= 2)
        Log("kernel: exception %#x raised at %#x", Read<u32>(record), Read<u32>(record + 12));
    // return into the dispatcher with eax = the record, edx = the eip to resume at
    const u32 eip = c.frame->ret;
    c.frame->ret = ExceptionDispatcher();
    return u64(record) | u64(eip) << 32;
}

// A status raised as a noncontinuable exception (ExRaiseStatus, RtlRaiseStatus).
u64 Raise(Call& c, u32 status)
{
    const u32 record = PoolAlloc(0x50); // EXCEPTION_RECORD; a raise is rare, so it isn't freed
    std::memset(Ptr(record), 0, 0x50);
    Write<u32>(record, status);
    Write<u32>(record + 4, 1); // EXCEPTION_NONCONTINUABLE
    return RaiseRecord(c, record);
}

bool HasApcs(ThreadState* s, bool alertable)
{
    for (const auto& a : s->apcs)
        if (!a.user || alertable)
            return true;
    return false;
}

bool RunApcs(bool alertable)
{
    bool user_ran = false;
    for (;;)
    {
        ThreadState::Apc a;
        {
            std::lock_guard lock(g_sched);
            ThreadState* s = CurrentState();
            if (!s)
                return user_ran;
            auto it = std::find_if(s->apcs.begin(), s->apcs.end(), [&](const auto& x) { return !x.user || alertable; });
            if (it == s->apcs.end())
                return user_ran;
            a = *it;
            s->apcs.erase(it);
        }
        u32 normal = a.normal_routine, context = a.context, arg1 = a.arg1, arg2 = a.arg2;
        if (a.kapc)
        {
            Write<u8>(a.kapc + 3, 0); // no longer inserted
            if (a.kernel_routine) // KernelRoutine(Apc, &NormalRoutine, &NormalContext, &Arg1, &Arg2)
            {
                CallGuestNested(a.kernel_routine, {a.kapc, a.kapc + 0x18, a.kapc + 0x1c, a.kapc + 0x20, a.kapc + 0x24});
                normal = Read<u32>(a.kapc + 0x18), context = Read<u32>(a.kapc + 0x1c);
                arg1 = Read<u32>(a.kapc + 0x20), arg2 = Read<u32>(a.kapc + 0x24);
            }
        }
        if (normal)
            CallGuestNested(normal, {context, arg1, arg2});
        user_ran |= a.user;
    }
}

// The current thread's queued DPCs, run as at dispatch level (DpcRoutineActive set while they run).
void RunPendingDpcs()
{
    GuestThread* t = CurrentThread();
    const u32 kpcr = t->kpcr;
    if (Read<u32>(kpcr + kpcr::DpcRoutineActive))
        return;
    for (;;)
    {
        u32 d;
        {
            std::lock_guard lock(g_sched);
            ThreadState* s = CurrentState();
            if (!s || s->dpcs.empty())
                break;
            d = s->dpcs.front();
            s->dpcs.pop_front();
        }
        Write<u8>(d + 2, 0);
        const u8 irql = Read<u8>(kpcr + kpcr::Irql);
        Write<u8>(kpcr + kpcr::Irql, 2);
        Write<u32>(kpcr + kpcr::DpcRoutineActive, 1);
        // DeferredRoutine(Dpc, DeferredContext, SystemArgument1, SystemArgument2)
        CallGuestNested(Read<u32>(d + 12), {d, Read<u32>(d + 16), Read<u32>(d + 20), Read<u32>(d + 24)});
        Write<u32>(kpcr + kpcr::DpcRoutineActive, 0);
        Write<u8>(kpcr + kpcr::Irql, irql);
    }
    Write<u32>(kpcr + kpcr::DpcInterruptRequested, 0);
}

// Return addresses along the guest's frame pointer chain from `ebp` (inside the thread's stack).
std::vector<u32> FrameChain(u32 ebp, u32 max)
{
    std::vector<u32> out;
    GuestThread* t = CurrentThread();
    while (out.size() < max && ebp >= t->stack_limit && ebp + 8 <= t->stack_top && !(ebp & 3))
    {
        const u32 ret = Read<u32>(ebp + 4);
        if (!ret)
            break;
        out.push_back(ret);
        const u32 next = Read<u32>(ebp);
        if (next <= ebp)
            break;
        ebp = next;
    }
    return out;
}

void QueueApc(ThreadState* s, const ThreadState::Apc& a) // under g_sched
{
    s->apcs.push_back(a);
    g_cv.notify_all();
}

// ---- timers (KTIMER: header 0x10, DueTime 0x10, TimerListEntry 0x18, Dpc 0x20, Period 0x24),
// fired by their own thread

struct TimerInfo
{
    Clock::time_point due;
    u32 period_ms = 0, dpc = 0, apc_routine = 0, apc_context = 0;
    ThreadState* apc_thread = nullptr;
};
std::mutex g_timer_mutex;
std::condition_variable g_timer_cv;
std::map<u32, TimerInfo> g_timers;

Clock::time_point DueTime(s64 t)
{
    if (t < 0)
        return Clock::now() + std::chrono::nanoseconds(-t * 100);
    return Clock::now() + std::chrono::nanoseconds(std::max<s64>(0, t - SystemTime()) * 100);
}

void TimerThread()
{
    std::unique_lock lock(g_timer_mutex);
    while (!g_quit)
    {
        auto next = g_timers.end();
        for (auto it = g_timers.begin(); it != g_timers.end(); ++it)
            if (next == g_timers.end() || it->second.due < next->second.due)
                next = it;
        if (next == g_timers.end())
        {
            g_timer_cv.wait_for(lock, std::chrono::milliseconds(100));
            continue;
        }
        if (Clock::now() < next->second.due)
        {
            g_timer_cv.wait_until(lock, std::min(next->second.due, Clock::now() + std::chrono::milliseconds(100)));
            continue;
        }
        const u32 timer = next->first;
        const TimerInfo info = next->second;
        if (info.period_ms)
            next->second.due += std::chrono::milliseconds(info.period_ms);
        else
            g_timers.erase(next);
        lock.unlock();
        {
            std::lock_guard sched(g_sched);
            SetSignal(timer, 1);
            if (!info.period_ms)
                Write<u8>(timer + 3, 0); // no longer inserted
            if (info.apc_routine && info.apc_thread && !info.apc_thread->done)
            {
                const u64 now = u64(SystemTime());
                QueueApc(info.apc_thread, {0, 0, info.apc_routine, info.apc_context, u32(now), u32(now >> 32), true});
            }
            g_cv.notify_all();
        }
        if (info.dpc)
            QueueDpc(info.dpc);
        lock.lock();
    }
}

// Sets a timer: returns whether it was already set.
bool SetTimer(u32 timer, s64 due, u32 period_ms, u32 dpc, u32 apc_routine = 0, u32 apc_context = 0)
{
    static std::once_flag started;
    std::call_once(started, [] { std::thread(TimerThread).detach(); });
    ThreadState* me;
    {
        std::lock_guard sched(g_sched);
        SetSignal(timer, 0);
        Write<u8>(timer + 3, 1);
        Write<s64>(timer + 0x10, due);
        Write<u32>(timer + 0x20, dpc);
        Write<u32>(timer + 0x24, period_ms);
        me = CurrentState();
    }
    std::lock_guard lock(g_timer_mutex);
    const bool was = g_timers.count(timer) != 0;
    g_timers[timer] = {DueTime(due), period_ms, dpc, apc_routine, apc_context, apc_routine ? me : nullptr};
    g_timer_cv.notify_all();
    return was;
}

bool CancelTimer(u32 timer)
{
    std::lock_guard lock(g_timer_mutex);
    Write<u8>(timer + 3, 0);
    return g_timers.erase(timer) != 0;
}

// ---- read/write locks (ERWLOCK; the core keeps its own fields: +0x04 the writer, +0x0c readers)

// ERWLOCK: LockCount 0 (-1 free; each request adds one), WritersWaitingCount 4, ReadersWaitingCount 8,
// ReadersEntryCount 0xc, WriterEvent 0x10 (a synchronization event), ReaderSemaphore 0x20.
void AcquireRwLock(u32 l, bool exclusive)
{
    std::unique_lock lock(g_sched);
    const s32 count = Read<s32>(l) + 1;
    Write<s32>(l, count);
    if (exclusive)
    {
        if (count == 0)
            return;
        Write<u32>(l + 4, Read<u32>(l + 4) + 1);
        while (Signal(l + 0x10) <= 0 && !g_quit) // the releaser hands the lock over through the event
            g_cv.wait_for(lock, std::chrono::milliseconds(50));
        SetSignal(l + 0x10, 0);
        return;
    }
    if (count == 0)
    {
        Write<u32>(l + 0xc, 1);
        return;
    }
    if (Read<u32>(l + 0xc) && !Read<u32>(l + 4))
    {
        Write<u32>(l + 0xc, Read<u32>(l + 0xc) + 1);
        return;
    }
    Write<u32>(l + 8, Read<u32>(l + 8) + 1);
    while (Signal(l + 0x20) <= 0 && !g_quit)
        g_cv.wait_for(lock, std::chrono::milliseconds(50));
    SetSignal(l + 0x20, Signal(l + 0x20) - 1);
}

void ReleaseRwLock(u32 l)
{
    std::lock_guard lock(g_sched);
    const s32 count = Read<s32>(l) - 1;
    Write<s32>(l, count);
    if (count < 0)
    {
        Write<u32>(l + 0xc, 0);
        return;
    }
    const u32 entered = Read<u32>(l + 0xc);
    if (entered > 1)
    {
        Write<u32>(l + 0xc, entered - 1);
        return;
    }
    if (entered == 0 && Read<u32>(l + 8)) // the writer leaves: every waiting reader goes in
    {
        const u32 readers = Read<u32>(l + 8);
        Write<u32>(l + 0xc, readers);
        Write<u32>(l + 8, 0);
        SetSignal(l + 0x20, Signal(l + 0x20) + s32(readers));
    }
    else // the writer or the last reader leaves: a waiting writer goes in
    {
        Write<u32>(l + 0xc, 0);
        Write<u32>(l + 4, Read<u32>(l + 4) - 1);
        SetSignal(l + 0x10, 1);
    }
    g_cv.notify_all();
}

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
    auto* s = new ThreadState{t, {}, 0, false, {}, {}};
    Write<u8>(t->ethread + kthread::SuspendCount, suspended ? 1 : 0);
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
            while (Read<u8>(s->thread->ethread + kthread::SuspendCount) && !g_quit)
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
    // the object a program sees through ObReferenceObjectByHandle: a FILE_OBJECT (Type IO_TYPE_FILE),
    // an object directory or a symbolic link
    u32 body = 0;
    if (kind == 1 || kind == 2)
    {
        body = NewObject(0x50, "IoFileObjectType");
        Write<u16>(body, 5);
        Write<u16>(body + 2, 0x50);
    }
    else if (kind == 3)
        body = NewObject(0x20, "ObDirectoryObjectType");
    else if (kind == 4)
        body = NewObject(0x10, "ObSymbolicLinkObjectType");
    return NewHandle(body, kind, host);
}

u32 NewObjectBody(u32 size, const char* type) { return NewObject(size, type); }

void* HostOf(u32 handle, int kind)
{
    std::lock_guard lock(g_handles_mutex);
    auto it = g_handles.find(handle);
    return it == g_handles.end() || it->second.kind != kind ? nullptr : it->second.host;
}

bool IsHandleKind(u32 handle, int kind)
{
    std::lock_guard lock(g_handles_mutex);
    auto it = g_handles.find(handle);
    return it != g_handles.end() && it->second.kind == kind;
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

std::string FormatGuest(u32 fmt, u32 args)
{
    return GuestFormat(fmt, args);
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
    RegisterRtlExports(Register);
    RegisterCryptoExports(Register);
    K(KeBugCheckEx) // the program's end, with its reason (an unhandled exception: 0x8e, code, address)
    {
        if (c.Arg(0) == 0x8e)
            Log("kernel: unhandled exception %#x at %#x: the program ends", c.Arg(1), c.Arg(2));
        else
            Log("kernel: KeBugCheckEx %#x (%#x, %#x, %#x, %#x): the program ends", c.Arg(0), c.Arg(1), c.Arg(2), c.Arg(3),
                c.Arg(4));
        g_quit = true;
        WakeAll();
        ExitThread();
    } END;

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
    K(RtlRaiseException) { return RaiseRecord(c, c.Arg(0)); } END; // record
    K(ExRaiseException) { return RaiseRecord(c, c.Arg(0)); } END;
    K(RtlRaiseStatus) { return Raise(c, c.Arg(0)); } END;
    K(ExRaiseStatus) { return Raise(c, c.Arg(0)); } END;
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
    K(KeSetBasePriorityThread) { return 0; } END;
    K(KeSetPriorityThread) { return 8; } END;
    K(NtYieldExecution) { sched_yield(); return STATUS_SUCCESS; } END;
    K(NtResumeThread)
    {
        const u32 object = ObjectOf(c.Arg(0));
        if (!object || Type(object) != kThread)
            return STATUS_INVALID_HANDLE;
        std::lock_guard lock(g_sched);
        const int old = ResumeThread(object);
        if (c.Arg(1))
            Write<u32>(c.Arg(1), u32(old));
        return STATUS_SUCCESS;
    } END;
    K(NtSuspendThread) // handle, previous count*
    {
        const u32 object = ObjectOf(c.Arg(0));
        if (!object || Type(object) != kThread)
            return STATUS_INVALID_HANDLE;
        std::lock_guard lock(g_sched);
        const int old = SuspendThread(object);
        if (old < 0)
            return 0xC000004A; // STATUS_SUSPEND_COUNT_EXCEEDED
        if (c.Arg(1))
            Write<u32>(c.Arg(1), u32(old));
        return STATUS_SUCCESS;
    } END;
    K(KeDelayExecutionThread) // mode, alertable, interval: a wait for nothing
    {
        const u32 status = WaitObjects({}, false, c.Arg(2), c.Arg(1) & 0xff);
        return status == STATUS_USER_APC ? STATUS_USER_APC : STATUS_SUCCESS;
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
        const u32 event = NewObject(0x10, "ExEventObjectType");
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
        const u32 sem = NewObject(0x14, "ExSemaphoreObjectType");
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
        const u32 m = NewObject(0x20, "ExMutantObjectType");
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
    K(NtWaitForSingleObjectEx) { const u32 o = ObjectOf(c.Arg(0)); return o ? WaitObjects({o}, false, c.Arg(3), c.Arg(2) & 0xff) : STATUS_INVALID_HANDLE; } END;
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
        return WaitObjects(objects, c.Arg(2) == 0, c.Arg(5), c.Arg(4) & 0xff);
    } END;
    K(KeWaitForSingleObject) { return WaitObjects({c.Arg(0)}, false, c.Arg(4), c.Arg(3) & 0xff); } END;
    K(KeWaitForMultipleObjects)
    {
        std::vector<u32> objects;
        for (u32 i = 0; i < c.Arg(0); i++)
            objects.push_back(Read<u32>(c.Arg(1) + i * 4));
        return WaitObjects(objects, c.Arg(2) == 0, c.Arg(6), c.Arg(5) & 0xff);
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
        // handle, object type (NULL: any), object*
        const u32 o = BodyOf(c.Arg(0));
        if (!o)
        {
            Write<u32>(c.Arg(2), 0);
            return STATUS_INVALID_HANDLE;
        }
        if (c.Arg(1) && HeaderType(o) != c.Arg(1))
        {
            Write<u32>(c.Arg(2), 0);
            return 0xC0000024; // STATUS_OBJECT_TYPE_MISMATCH
        }
        Write<u32>(c.Arg(2), o);
        return STATUS_SUCCESS;
    } END;
    K(ObfDereferenceObject) { return 0; } END;
    K(ObfReferenceObject) { return 0; } END;

    // ---- kernel mutants (KMUTANT: header, list entry 0x10, OwnerThread 0x18, Abandoned 0x1c)
    K(KeInitializeMutant)
    {
        const bool owned = c.Arg(1) & 0xff;
        InitHeader(c.Arg(0), kMutant, 8, owned ? 0 : 1);
        Write<u32>(c.Arg(0) + 0x18, owned ? CurrentThread()->ethread : 0);
        Write<u8>(c.Arg(0) + 0x1c, 0);
        return 0;
    } END;
    K(KeReleaseMutant) // mutant, increment, abandoned, wait: the previous state
    {
        const u32 m = c.Arg(0);
        std::lock_guard lock(g_sched);
        const s32 old = Signal(m);
        SetSignal(m, (c.Arg(2) & 0xff) ? 1 : old + 1);
        if (Signal(m) > 0)
            Write<u32>(m + 0x18, 0);
        g_cv.notify_all();
        return u32(old);
    } END;
    K(KeSetEventBoostPriority) // event, thread*
    {
        std::lock_guard lock(g_sched);
        SetSignal(c.Arg(0), 1);
        if (c.Arg(1))
            Write<u32>(c.Arg(1), 0);
        g_cv.notify_all();
        return 0;
    } END;

    // ---- object queries
    K(NtQueryEvent) // handle, EVENT_BASIC_INFORMATION { EventType, EventState }
    {
        const u32 o = ObjectOf(c.Arg(0));
        if (!o)
            return STATUS_INVALID_HANDLE;
        Write<u32>(c.Arg(1), Type(o) == kSynchronizationEvent ? 1 : 0);
        Write<s32>(c.Arg(1) + 4, Signal(o));
        return STATUS_SUCCESS;
    } END;
    K(NtQueryMutant) // handle, { CurrentCount, OwnedByCaller, AbandonedState }
    {
        const u32 o = ObjectOf(c.Arg(0));
        if (!o)
            return STATUS_INVALID_HANDLE;
        Write<s32>(c.Arg(1), Signal(o));
        Write<u8>(c.Arg(1) + 4, Read<u32>(o + 0x18) == CurrentThread()->ethread);
        Write<u8>(c.Arg(1) + 5, Read<u8>(o + 0x1c));
        return STATUS_SUCCESS;
    } END;
    K(NtQuerySemaphore) // handle, { CurrentCount, MaximumCount }
    {
        const u32 o = ObjectOf(c.Arg(0));
        if (!o)
            return STATUS_INVALID_HANDLE;
        Write<s32>(c.Arg(1), Signal(o));
        Write<s32>(c.Arg(1) + 4, Read<s32>(o + 0x10));
        return STATUS_SUCCESS;
    } END;
    K(NtDuplicateObject) // source, target*, options (1: close the source)
    {
        HandleEntry e;
        {
            std::lock_guard lock(g_handles_mutex);
            auto it = g_handles.find(c.Arg(0));
            if (it == g_handles.end())
            {
                if (c.Arg(0) != 0xfffffffe)
                    return STATUS_INVALID_HANDLE;
                e = {CurrentThread()->ethread, 0, nullptr};
            }
            else
                e = it->second;
            if (e.kind != 0 && !(c.Arg(2) & 1))
                return 0xC00000BB; // STATUS_NOT_SUPPORTED: a file handle has one owner here
            if (c.Arg(2) & 1)
                g_handles.erase(c.Arg(0));
        }
        Write<u32>(c.Arg(1), NewHandle(e.object, e.kind, e.host));
        return STATUS_SUCCESS;
    } END;
    K(NtSignalAndWaitForSingleObjectEx) // signal handle, wait handle, mode, alertable, timeout*
    {
        const u32 signal = ObjectOf(c.Arg(0)), wait = ObjectOf(c.Arg(1));
        if (!signal || !wait)
            return STATUS_INVALID_HANDLE;
        {
            std::lock_guard lock(g_sched);
            switch (Type(signal))
            {
            case kSemaphore: SetSignal(signal, Signal(signal) + 1); break;
            case kMutant:
                SetSignal(signal, Signal(signal) + 1);
                if (Signal(signal) > 0)
                    Write<u32>(signal + 0x18, 0);
                break;
            default: SetSignal(signal, 1);
            }
            g_cv.notify_all();
        }
        return WaitObjects({wait}, false, c.Arg(4), c.Arg(3) & 0xff);
    } END;
    K(ObOpenObjectByPointer) // object, type, handle*
    {
        Write<u32>(c.Arg(2), NewHandle(c.Arg(0)));
        return STATUS_SUCCESS;
    } END;
    K(ObReferenceObjectByPointer) { return STATUS_SUCCESS; } END;
    K(ObMakeTemporaryObject) { return 0; } END;

    // ---- threads by their KTHREAD
    K(PsCreateSystemThread) // handle*, thread id*, start, context, debugger thread
    {
        u32 handle = 0;
        const u32 id = CreateThread(0x10000, 0, c.Arg(2), c.Arg(3), false, 0, "system", &handle);
        if (c.Arg(0))
            Write<u32>(c.Arg(0), handle);
        if (c.Arg(1))
            Write<u32>(c.Arg(1), id);
        return STATUS_SUCCESS;
    } END;
    K(KeSuspendThread) // the previous count; past the limit, raises STATUS_SUSPEND_COUNT_EXCEEDED
    {
        int old;
        {
            std::lock_guard lock(g_sched);
            old = SuspendThread(c.Arg(0));
        }
        return old < 0 ? Raise(c, 0xC000004A) : u32(old);
    } END;
    K(KeResumeThread)
    {
        std::lock_guard lock(g_sched);
        return u32(ResumeThread(c.Arg(0)));
    } END;
    K(KeAlertThread) { return 0; } END;
    K(KeAlertResumeThread) { return 0; } END;
    K(KeTestAlertThread) { return 0; } END;

    // ---- APCs (KAPC: Type 0, ApcMode 2, Inserted 3, Thread 4, list 8, KernelRoutine 0x10,
    // RundownRoutine 0x14, NormalRoutine 0x18, NormalContext 0x1c, SystemArgument1 0x20, 2 0x24)
    K(KeInitializeApc) // apc, thread, kernel routine, rundown routine, normal routine, mode, context
    {
        const u32 apc = c.Arg(0);
        std::memset(Ptr(apc), 0, 0x28);
        Write<u16>(apc, 0x12);
        Write<u8>(apc + 2, u8(c.Arg(5)));
        Write<u32>(apc + 4, c.Arg(1));
        Write<u32>(apc + 0x10, c.Arg(2));
        Write<u32>(apc + 0x14, c.Arg(3));
        Write<u32>(apc + 0x18, c.Arg(4));
        Write<u32>(apc + 0x1c, c.Arg(6));
        return 0;
    } END;
    K(KeInsertQueueApc) // apc, argument 1, argument 2, increment
    {
        const u32 apc = c.Arg(0);
        std::lock_guard lock(g_sched);
        ThreadState* s = StateOf(Read<u32>(apc + 4));
        if (!s || s->done || Read<u8>(apc + 3))
            return 0;
        Write<u32>(apc + 0x20, c.Arg(1));
        Write<u32>(apc + 0x24, c.Arg(2));
        Write<u8>(apc + 3, 1);
        // user-mode APCs (mode 1) wait for an alertable wait; kernel ones run at the next wait
        QueueApc(s, {apc, Read<u32>(apc + 0x10), Read<u32>(apc + 0x18), Read<u32>(apc + 0x1c), c.Arg(1), c.Arg(2),
                     Read<u8>(apc + 2) == 1});
        return 1;
    } END;
    K(NtQueueApcThread) // thread, routine, context, status block, reserved
    {
        const u32 thread = ObjectOf(c.Arg(0));
        std::lock_guard lock(g_sched);
        ThreadState* s = StateOf(thread);
        if (!s || s->done)
            return STATUS_INVALID_HANDLE;
        QueueApc(s, {0, 0, c.Arg(1), c.Arg(2), c.Arg(3), c.Arg(4), true});
        return STATUS_SUCCESS;
    } END;

    // ---- timers
    K(KeInitializeTimerEx) // timer, type (0 notification, 1 synchronization)
    {
        InitHeader(c.Arg(0), c.Arg(1) ? kSynchronizationTimer : kNotificationTimer, 10, 0);
        Write<s64>(c.Arg(0) + 0x10, 0);
        Write<u32>(c.Arg(0) + 0x20, 0);
        Write<u32>(c.Arg(0) + 0x24, 0);
        return 0;
    } END;
    K(KeSetTimer) // timer, due time (2 slots), dpc
    {
        return SetTimer(c.Arg(0), s64(u64(c.Arg(1)) | u64(c.Arg(2)) << 32), 0, c.Arg(3));
    } END;
    K(KeSetTimerEx) // timer, due time (2 slots), period (ms), dpc
    {
        return SetTimer(c.Arg(0), s64(u64(c.Arg(1)) | u64(c.Arg(2)) << 32), c.Arg(3), c.Arg(4));
    } END;
    K(KeCancelTimer) { return CancelTimer(c.Arg(0)); } END;
    // ---- I/O completions (KQUEUE body, type 4: its signal state counts the packets waiting)
    K(NtCreateIoCompletion) // handle*, access, attributes, count
    {
        const u32 q = NewObject(0x28, "IoCompletionObjectType");
        InitHeader(q, 4, 10, 0);
        {
            std::lock_guard lock(g_sched);
            g_completions[q];
        }
        Write<u32>(c.Arg(0), NewHandle(q));
        return STATUS_SUCCESS;
    } END;
    K(NtSetIoCompletion) // handle, key, apc context, status, information
    {
        const u32 q = ObjectOf(c.Arg(0));
        return q ? PostCompletion(q, c.Arg(1), c.Arg(2), c.Arg(3), c.Arg(4)) : STATUS_INVALID_HANDLE;
    } END;
    K(IoSetIoCompletion) // the completion object, key, apc context, status, information
    {
        PostCompletion(c.Arg(0), c.Arg(1), c.Arg(2), c.Arg(3), c.Arg(4));
        return 0;
    } END;
    K(NtRemoveIoCompletion) // handle, key*, apc context*, I/O status block*, timeout*
    {
        const u32 q = ObjectOf(c.Arg(0));
        if (!q)
            return STATUS_INVALID_HANDLE;
        for (;;)
        {
            {
                std::lock_guard lock(g_sched);
                auto it = g_completions.find(q);
                if (it == g_completions.end())
                    return STATUS_INVALID_HANDLE;
                if (!it->second.empty())
                {
                    const Completion p = it->second.front();
                    it->second.pop_front();
                    SetSignal(q, s32(it->second.size()));
                    Write<u32>(c.Arg(1), p.key);
                    Write<u32>(c.Arg(2), p.apc_context);
                    Write<u32>(c.Arg(3), p.status);
                    Write<u32>(c.Arg(3) + 4, p.information);
                    return STATUS_SUCCESS;
                }
            }
            if (WaitObjects({q}, false, c.Arg(4)) == STATUS_TIMEOUT)
                return STATUS_TIMEOUT;
        }
    } END;
    K(NtQueryIoCompletion) // handle, information* (Depth)
    {
        const u32 q = ObjectOf(c.Arg(0));
        if (!q)
            return STATUS_INVALID_HANDLE;
        std::lock_guard lock(g_sched);
        Write<s32>(c.Arg(1), Signal(q));
        return STATUS_SUCCESS;
    } END;

    // ---- devices: named DEVICE_OBJECTs a program makes (ObOpenObjectByName finds them)
    K(IoCreateDevice) // driver object, extension size, name (OBJECT_STRING*), type, exclusive, device*
    {
        const u32 extension = c.Arg(1);
        const u32 d = NewObject(0x48 + extension, "IoDeviceObjectType");
        Write<u16>(d, 3); // IO_TYPE_DEVICE
        Write<u16>(d + 2, 0x48);
        Write<s32>(d + 4, 0);
        Write<u32>(d + 8, c.Arg(0));
        Write<u32>(d + 0x18, extension ? d + 0x48 : 0);
        Write<u8>(d + 0x1c, u8(c.Arg(3)));
        Write<u8>(d + 0x1e, 1); // StackSize
        if (c.Arg(2))
        {
            const std::string name = LowerName(GuestString(c.Arg(2)));
            std::lock_guard lock(g_handles_mutex);
            if (g_named.count(name))
                return 0xC0000035; // STATUS_OBJECT_NAME_COLLISION
            g_named[name] = d;
        }
        Write<u32>(c.Arg(5), d);
        return STATUS_SUCCESS;
    } END;
    K(IoDeleteDevice)
    {
        std::lock_guard lock(g_handles_mutex);
        for (auto it = g_named.begin(); it != g_named.end(); ++it)
            if (it->second == c.Arg(0))
            {
                g_named.erase(it);
                break;
            }
        return 0;
    } END;
    K(ObOpenObjectByName) // attributes, type (NULL: any), parse context, handle*
    {
        const u32 attributes = c.Arg(0), name_string = Read<u32>(attributes + 4);
        if (!name_string)
            return STATUS_OBJECT_NAME_NOT_FOUND;
        const std::string name = LowerName(GuestString(name_string));
        u32 o;
        {
            std::lock_guard lock(g_handles_mutex);
            auto it = g_named.find(name);
            if (it == g_named.end())
                return STATUS_OBJECT_NAME_NOT_FOUND;
            o = it->second;
        }
        if (c.Arg(1) && HeaderType(o) != c.Arg(1))
            return 0xC0000024; // STATUS_OBJECT_TYPE_MISMATCH
        Write<u32>(c.Arg(3), NewHandle(o, 5));
        return STATUS_SUCCESS;
    } END;
    K(NtCreateTimer) // handle*, attributes, type
    {
        const u32 t = NewObject(0x28, "ExTimerObjectType");
        InitHeader(t, c.Arg(2) ? kSynchronizationTimer : kNotificationTimer, 10, 0);
        Write<u32>(c.Arg(0), NewHandle(t));
        return STATUS_SUCCESS;
    } END;
    K(NtSetTimerEx) // handle, due time*, apc routine, apc mode, apc context, resume, period, previous state*
    {
        const u32 t = ObjectOf(c.Arg(0));
        if (!t)
            return STATUS_INVALID_HANDLE;
        if (c.Arg(7))
            Write<u8>(c.Arg(7), Signal(t) > 0);
        SetTimer(t, Read<s64>(c.Arg(1)), c.Arg(6), 0, c.Arg(2), c.Arg(4));
        return STATUS_SUCCESS;
    } END;
    K(NtCancelTimer) // handle, current state*
    {
        const u32 t = ObjectOf(c.Arg(0));
        if (!t)
            return STATUS_INVALID_HANDLE;
        CancelTimer(t);
        if (c.Arg(1))
            Write<u8>(c.Arg(1), Signal(t) > 0);
        return STATUS_SUCCESS;
    } END;
    K(NtQueryTimer) // handle, class, TIMER_BASIC_INFORMATION { RemainingTime, TimerState }
    {
        const u32 t = ObjectOf(c.Arg(0));
        if (!t)
            return STATUS_INVALID_HANDLE;
        s64 remaining = 0;
        {
            std::lock_guard lock(g_timer_mutex);
            auto it = g_timers.find(t);
            if (it != g_timers.end())
                remaining = std::max<s64>(0, std::chrono::duration_cast<std::chrono::nanoseconds>(it->second.due - Clock::now()).count() / 100);
        }
        Write<s64>(c.Arg(2), remaining);
        Write<u8>(c.Arg(2) + 8, Signal(t) > 0);
        return STATUS_SUCCESS;
    } END;

    // ---- read/write locks
    K(ExInitializeReadWriteLock)
    {
        const u32 l = c.Arg(0);
        std::memset(Ptr(l), 0, 0x34); // sizeof(ERWLOCK)
        Write<s32>(l, -1);
        InitHeader(l + 0x10, kSynchronizationEvent, 4, 0);
        InitHeader(l + 0x20, kSemaphore, 5, 0);
        Write<s32>(l + 0x30, 0x7fffffff);
        return 0;
    } END;
    K(ExAcquireReadWriteLockExclusive) { AcquireRwLock(c.Arg(0), true); return 0; } END;
    K(ExAcquireReadWriteLockShared) { AcquireRwLock(c.Arg(0), false); return 0; } END;
    K(ExReleaseReadWriteLock) { ReleaseRwLock(c.Arg(0)); return 0; } END;

    K(RtlUnwind) // target frame, target ip, exception record, return value
    {
        // every handler above the target is called with the unwinding flags (they run their
        // __finally blocks and local unwinds), and taken off the chain
        const u32 target = c.Arg(0);
        const u32 kpcr = CurrentThread()->kpcr;
        u32 record = c.Arg(2);
        const u32 own = record ? 0 : PoolAlloc(0x50 + 0x238 + 8);
        if (own)
        {
            std::memset(Ptr(own), 0, 0x50 + 0x238 + 8);
            record = own;
            Write<u32>(record, 0xC0000027); // STATUS_UNWIND
            Write<u32>(record + 12, c.frame->ret);
        }
        Write<u32>(record + 4, Read<u32>(record + 4) | (target ? 0x2u : 0x6u)); // UNWINDING (| EXIT_UNWIND)
        const u32 context = own ? own + 0x50 : PoolAlloc(0x238 + 8);
        const u32 dispatcher_context = context + 0x238;
        for (u32 frame = Read<u32>(kpcr); frame && frame != 0xffffffff && frame != target;)
        {
            const u32 handler = Read<u32>(frame + 4);
            CallGuestNested(handler, {record, frame, context, dispatcher_context});
            const u32 next = Read<u32>(frame);
            Write<u32>(kpcr, next); // NtTib.ExceptionList
            frame = next;
        }
        if (target)
            Write<u32>(kpcr, target);
        PoolFree(own ? own : context);
        return c.Arg(3);
    } END;

    // ---- XBE sections loaded and unloaded on demand (XBE_SECTION_HEADER: VirtualAddress 4,
    // VirtualSize 8, FileAddress 12, FileSize 16, SectionReferenceCount 24). Every section is in
    // memory from the start; a section loaded again after its last unload gets its data back.
    K(XeLoadSection)
    {
        const u32 h = c.Arg(0);
        const u32 count = Read<u32>(h + 24);
        if (count == 0)
        {
            const u32 va = Read<u32>(h + 4), vsize = Read<u32>(h + 8), raw = Read<u32>(h + 12), rsize = Read<u32>(h + 16);
            const u32 n = std::min(rsize, vsize);
            if (va && ReadXbeRange(g_xbe, raw, n, Ptr(va)) && vsize > n)
                std::memset(Ptr(va + n), 0, vsize - n);
            for (u32 ref : {Read<u32>(h + 28), Read<u32>(h + 32)}) // the shared head and tail pages
                if (ref)
                    Write<u16>(ref, u16(Read<u16>(ref) + 1));
        }
        Write<u32>(h + 24, count + 1);
        return STATUS_SUCCESS;
    } END;
    K(XeUnloadSection)
    {
        const u32 h = c.Arg(0);
        const u32 count = Read<u32>(h + 24);
        if (count == 0)
            return STATUS_INVALID_PARAMETER;
        Write<u32>(h + 24, count - 1);
        if (count == 1)
            for (u32 ref : {Read<u32>(h + 28), Read<u32>(h + 32)})
                if (ref && Read<u16>(ref))
                    Write<u16>(ref, u16(Read<u16>(ref) - 1));
        return STATUS_SUCCESS;
    } END;

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

    // the "...AndRegion" forms: the critical region (the thread's KernelApcDisable) around them
    {
        const Handler enter = g_handlers[g_ordinal_by_name["RtlEnterCriticalSection"]];
        const Handler leave = g_handlers[g_ordinal_by_name["RtlLeaveCriticalSection"]];
        Register("RtlEnterCriticalSectionAndRegion", [enter](Call& c) -> u64 {
            const u32 t = CurrentThread()->ethread;
            Write<s32>(t + kthread::KernelApcDisable, Read<s32>(t + kthread::KernelApcDisable) - 1);
            return enter(c);
        });
        Register("RtlLeaveCriticalSectionAndRegion", [leave](Call& c) -> u64 {
            const u64 r = leave(c);
            // the region is left once, when the section is free (as the Xbox's kernel does it)
            if (Read<u32>(c.Arg(0) + 0x18) == 0)
            {
                const u32 t = CurrentThread()->ethread;
                Write<s32>(t + kthread::KernelApcDisable, Read<s32>(t + kthread::KernelApcDisable) + 1);
            }
            return r;
        });
    }
    K(KeEnterCriticalRegion)
    {
        const u32 t = CurrentThread()->ethread;
        Write<s32>(t + kthread::KernelApcDisable, Read<s32>(t + kthread::KernelApcDisable) - 1);
        return 0;
    } END;
    K(KeLeaveCriticalRegion)
    {
        const u32 t = CurrentThread()->ethread;
        Write<s32>(t + kthread::KernelApcDisable, Read<s32>(t + kthread::KernelApcDisable) + 1);
        return 0;
    } END;

    // ---- the caller's context and its frames (the registers the gate saved in the KPCR)
    K(RtlCaptureContext) // CONTEXT*: the caller's registers; ContextFlags and the float area untouched
    {
        const u32 ctx = c.Arg(0), kpcr = CurrentThread()->kpcr;
        Write<u32>(ctx + 0x208, Read<u32>(kpcr + kpcr::GuestEdi));
        Write<u32>(ctx + 0x20c, Read<u32>(kpcr + kpcr::GuestEsi));
        Write<u32>(ctx + 0x210, Read<u32>(kpcr + kpcr::GuestEbx));
        Write<u32>(ctx + 0x214, c.frame->edx);
        Write<u32>(ctx + 0x218, c.frame->ecx);
        Write<u32>(ctx + 0x21c, Read<u32>(kpcr + kpcr::GuestEax));
        Write<u32>(ctx + 0x220, Read<u32>(kpcr + kpcr::GuestEbp));
        Write<u32>(ctx + 0x224, c.frame->ret);
        Write<u32>(ctx + 0x228, GuestCodeSelector());
        Write<u32>(ctx + 0x22c, Read<u32>(kpcr + kpcr::GuestEflags));
        Write<u32>(ctx + 0x230, Guest(&c.frame->args[1])); // after the return and the argument
        Write<u32>(ctx + 0x234, GuestDataSelector());
        return 0;
    } END;
    K(RtlGetCallersAddress) // caller*, caller's caller*: the return addresses one and two frames up
    {
        const u32 ebp = Read<u32>(CurrentThread()->kpcr + kpcr::GuestEbp);
        const std::vector<u32> frames = FrameChain(ebp, 2);
        if (c.Arg(0)) Write<u32>(c.Arg(0), frames.size() > 0 ? frames[0] : 0);
        if (c.Arg(1)) Write<u32>(c.Arg(1), frames.size() > 1 ? frames[1] : 0);
        return 0;
    } END;
    K(RtlWalkFrameChain) // callers*, count, flags: this call's return address, then the frame chain
    {
        std::vector<u32> frames = {c.frame->ret};
        const std::vector<u32> more = FrameChain(Read<u32>(CurrentThread()->kpcr + kpcr::GuestEbp), c.Arg(1));
        frames.insert(frames.end(), more.begin(), more.end());
        const u32 n = std::min<u32>(u32(frames.size()), c.Arg(1));
        for (u32 i = 0; i < n; i++)
            Write<u32>(c.Arg(0) + i * 4, frames[i]);
        return n;
    } END;
    K(RtlCaptureStackBackTrace) // skip, count, frames*, hash*: the same chain, from `skip` on
    {
        std::vector<u32> frames = {c.frame->ret};
        const std::vector<u32> more = FrameChain(Read<u32>(CurrentThread()->kpcr + kpcr::GuestEbp), 64);
        frames.insert(frames.end(), more.begin(), more.end());
        const u32 skip = c.Arg(0), want = c.Arg(1);
        u32 n = 0, hash = 0;
        for (u32 i = skip; i < frames.size() && n < want; i++, n++)
        {
            Write<u32>(c.Arg(2) + n * 4, frames[i]);
            hash += frames[i];
        }
        if (c.Arg(3))
            Write<u32>(c.Arg(3), hash);
        return n;
    } END;

    // ---- IRQL and DPCs: each thread's in its KPCR (Irql; the PRCB's DpcInterruptRequested and
    // DpcRoutineActive). A DPC queued at passive level runs at once; one queued at dispatch level or
    // above (or while another runs) waits until the IRQL drops below dispatch level.
    K(KeGetCurrentIrql) { return Read<u8>(CurrentThread()->kpcr + kpcr::Irql); } END;
    K(KfRaiseIrql) // fastcall: the new level; the old one back
    {
        const u32 kpcr = CurrentThread()->kpcr;
        const u8 old = Read<u8>(kpcr + kpcr::Irql);
        Write<u8>(kpcr + kpcr::Irql, u8(c.Arg(0)));
        return old;
    } END;
    K(KeRaiseIrqlToDpcLevel)
    {
        const u32 kpcr = CurrentThread()->kpcr;
        const u8 old = Read<u8>(kpcr + kpcr::Irql);
        Write<u8>(kpcr + kpcr::Irql, 2);
        return old;
    } END;
    K(KeRaiseIrqlToSynchLevel)
    {
        const u32 kpcr = CurrentThread()->kpcr;
        const u8 old = Read<u8>(kpcr + kpcr::Irql);
        Write<u8>(kpcr + kpcr::Irql, 0x1c);
        return old;
    } END;
    K(KfLowerIrql) // fastcall: the new level; pending DPCs run below dispatch level
    {
        const u32 kpcr = CurrentThread()->kpcr;
        Write<u8>(kpcr + kpcr::Irql, u8(c.Arg(0)));
        if (c.Arg(0) < 2)
            RunPendingDpcs();
        return 0;
    } END;
    K(KeIsExecutingDpc) { return Read<u32>(CurrentThread()->kpcr + kpcr::DpcRoutineActive) != 0; } END;
    K(HalRequestSoftwareInterrupt) // level: a dispatch-level request asks for the DPCs
    {
        if (c.Arg(0) == 2)
            Write<u32>(CurrentThread()->kpcr + kpcr::DpcInterruptRequested, 1);
        return 0;
    } END;
    K(KeSynchronizeExecution) // interrupt, routine, context: the routine's result
    {
        return u32(CallGuestNested(c.Arg(1), {c.Arg(2)}));
    } END;
    K(RtlMapGenericMask) // access mask*, GENERIC_MAPPING* { read, write, execute, all }
    {
        u32 mask = Read<u32>(c.Arg(0));
        const u32 m = c.Arg(1);
        if (mask & 0x80000000) mask |= Read<u32>(m);      // GENERIC_READ
        if (mask & 0x40000000) mask |= Read<u32>(m + 4);  // GENERIC_WRITE
        if (mask & 0x20000000) mask |= Read<u32>(m + 8);  // GENERIC_EXECUTE
        if (mask & 0x10000000) mask |= Read<u32>(m + 12); // GENERIC_ALL
        Write<u32>(c.Arg(0), mask & 0x0fffffff);
        return 0;
    } END;


    // ---- memory
    K(MmAllocateContiguousMemory) { return AllocContiguous(c.Arg(0), 0x1000, 0, 0xffffffff); } END;
    K(MmAllocateContiguousMemoryEx) { return AllocContiguous(c.Arg(0), c.Arg(3), c.Arg(1), c.Arg(2)); } END;
    K(MmFreeContiguousMemory) { FreeContiguous(c.Arg(0)); return 0; } END;
    K(MmPersistContiguousMemory) { return 0; } END;
    K(MmLockUnlockBufferPages) { return 0; } END;
    K(MmLockUnlockPhysicalPage) { return 0; } END;
    K(MmClaimGpuInstanceMemory)
    {
        // NumberOfBytes, NumberOfPaddingBytes*: the end of the GPU's instance memory (the RAM's
        // last 64 KiB stay the kernel's)
        if (c.Arg(1))
            Write<u32>(c.Arg(1), 0x10000);
        return kContiguousBase + kContiguousSize - 0x10000;
    } END;
    K(HalRegisterShutdownNotification) { return 0; } END;
    K(RtlTimeToTimeFields)
    {
        // Time (100 ns since 1601), TIME_FIELDS: Year, Month, Day, Hour, Minute, Second, Milliseconds,
        // Weekday. The kernel's arithmetic: milliseconds and days by signed division, then 32-bit
        // unsigned days and milliseconds, the years counted in 32 bits (negative times give what the
        // console gives, overflows included).
        const s64 t = Read<s64>(c.Arg(0));
        const s64 total_ms = t / 10000, signed_days = total_ms / 86400000;
        const u32 days = u32(signed_days), ms = u32(total_ms - signed_days * 86400000);
        const u32 n400 = days / 146097;
        u32 d = days - n400 * 146097;
        const u32 n100 = (d * 100 + 75) / 3652425;
        d -= n100 * 36524;
        const u32 n4 = d / 1461;
        d -= n4 * 1461;
        const u32 years = n400 * 400 + n100 * 100 + n4 * 4 + (d * 100 + 75) / 36525;
        u32 rest = days - (years * 365 + years / 4 - years / 100 + years / 400);
        const u32 y1 = years + 1;
        const bool leap = (y1 % 4 == 0 && y1 % 100 != 0) || y1 % 400 == 0;
        static const u32 kNormal[13] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365};
        static const u32 kLeap[13] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335, 366};
        const u32* table = leap ? kLeap : kNormal;
        u32 m = 0;
        while (m < 12 && table[m + 1] <= rest)
            m++;
        rest -= table[m];
        const u32 out = c.Arg(1);
        Write<u16>(out + 0, u16(1601 + years));
        Write<u16>(out + 2, u16(m + 1));
        Write<u16>(out + 4, u16(rest + 1));
        Write<u16>(out + 6, u16(ms / 3600000));
        Write<u16>(out + 8, u16(ms % 3600000 / 60000));
        Write<u16>(out + 10, u16(ms % 60000 / 1000));
        Write<u16>(out + 12, u16(ms % 1000));
        Write<u16>(out + 14, u16((days + 1) % 7)); // 1601-01-01 was a Monday
        return 0;
    } END;
    K(RtlTimeFieldsToTime)
    {
        const u32 in = c.Arg(0);
        const s64 y = Read<u16>(in), mo = Read<u16>(in + 2), d = Read<u16>(in + 4);
        auto leap = [](s64 yy) { return (yy % 4 == 0 && yy % 100 != 0) || yy % 400 == 0; };
        const u32 hour = Read<u16>(in + 6), minute = Read<u16>(in + 8), second = Read<u16>(in + 10), msec = Read<u16>(in + 12);
        if (y < 1601 || mo < 1 || mo > 12 || d < 1 || hour > 23 || minute > 59 || second > 59 || msec > 999)
            return 0; // FALSE, the time left alone
        const int mdays[12] = {31, leap(y) ? 29 : 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
        if (d > mdays[mo - 1])
            return 0;
        u64 days = 0;
        for (s64 yy = 1601; yy < y; yy++)
            days += leap(yy) ? 366 : 365;
        for (int m = 0; m < mo - 1; m++)
            days += u64(mdays[m]);
        days += u64(d - 1);
        // in 64-bit unsigned arithmetic: the latest dates wrap as the kernel's do
        const u64 ms = ((days * 24 + hour) * 60 + minute) * 60000 + second * 1000ull + msec;
        Write<u64>(c.Arg(1), ms * 10000);
        return 1;
    } END;
    K(MmGetPhysicalAddress) { return PhysicalAddress(c.Arg(0)); } END;
    K(MmQueryAllocationSize)
    {
        u32 start, size;
        return QueryAllocation(c.Arg(0), start, size) ? size : 0;
    } END;
    K(MmAllocateSystemMemory) // size, protect: PAGE_READONLY or PAGE_READWRITE, with NOCACHE or WRITECOMBINE
    {
        const u32 protect = c.Arg(1), base = protect & 0xff, modifiers = protect & ~0xffu;
        if ((base != 0x02 && base != 0x04) || (modifiers & ~0x600u) || modifiers == 0x600)
            return 0;
        return c.Arg(0) ? AllocSystem(c.Arg(0)) : 0;
    } END;
    K(MmFreeSystemMemory) { return FreeSystem(c.Arg(0)); } END; // base, size: the pages freed
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
    K(KeInsertQueueDpc) // dpc, argument 1, argument 2: whether it was queued
    {
        const u32 d = c.Arg(0);
        if (Read<u8>(d + 2))
            return 0;
        Write<u8>(d + 2, 1);
        Write<u32>(d + 20, c.Arg(1));
        Write<u32>(d + 24, c.Arg(2));
        ThreadState* s;
        {
            std::lock_guard lock(g_sched);
            s = CurrentState();
        }
        if (!s) // the interrupt thread's (an ISR's): it runs them after the ISRs
        {
            QueueDpc(d);
            return 1;
        }
        const u32 kpcr = s->thread->kpcr;
        {
            std::lock_guard lock(g_sched);
            s->dpcs.push_back(d);
        }
        Write<u32>(kpcr + kpcr::DpcInterruptRequested, 1);
        if (Read<u8>(kpcr + kpcr::Irql) < 2 && !Read<u32>(kpcr + kpcr::DpcRoutineActive))
            RunPendingDpcs();
        return 1;
    } END;
    K(KeRemoveQueueDpc) // dpc: whether it was queued (and now isn't)
    {
        const u32 d = c.Arg(0);
        if (!Read<u8>(d + 2))
            return 0;
        Write<u8>(d + 2, 0);
        std::lock_guard lock(g_sched);
        for (auto& [t, s] : g_threads)
            std::erase(s->dpcs, d);
        return 1;
    } END;

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
    K(HalWriteSMBusValue) // address, command, word?, value
    {
        // the SMC's power register (0x20, 2): reset, power cycle, shut down: the program ends
        if ((c.Arg(0) & 0xfe) == 0x20 && c.Arg(1) == 0x02 && (c.Arg(3) == 0x01 || c.Arg(3) == 0x40 || c.Arg(3) == 0x80))
        {
            Log("kernel: the SMC was told to %s", c.Arg(3) == 0x80 ? "shut down" : "restart");
            g_quit = true;
            WakeAll();
        }
        return STATUS_SUCCESS;
    } END;
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
        Write<u32>(at, e->kind == 'D' ? DataExport(ordinal) : ExportStub(ordinal));
        count++;
        if (e->kind != 'D' && !g_handlers[ordinal])
        {
            missing++;
            missing_names += std::string(" ") + e->name;
        }
    }
    Log("kernel: %u imports, %u of them not implemented yet:%s", count, missing, missing_names.c_str());
}

const char* KernelExportName(u32 ordinal)
{
    return ordinal < 400 && g_by_ordinal[ordinal] ? g_by_ordinal[ordinal]->name : nullptr;
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
        diag::CallStarted(thread, ordinal, c.Arg(0), c.Arg(1), c.Arg(2), c.Arg(3), frame->ret);
        result = g_handlers[ordinal](c);
        diag::CallEnded(thread, u32(result));
    }
    else
    {
        std::lock_guard lock(g_report_mutex);
        if (g_reported.insert(ordinal).second)
            Log("kernel: %s (ordinal %u) isn't implemented: returning STATUS_NOT_IMPLEMENTED", e ? e->name : "?", ordinal);
        diag::Note("kernel export", std::string(e ? e->name : "?") + " (ordinal " + std::to_string(ordinal) + ")");
    }
    // results: eax, edx; bytes the callee pops (stdcall; fastcall's stack part)
    const u32 bytes = !e ? 0 : e->kind == 'F' ? (e->arg_bytes > 8 ? e->arg_bytes - 8 : 0) : e->arg_bytes;
    frame->eax = u32(result);
    frame->ecx = u32(result >> 32);
    frame->edx = bytes;
    if (thread && Read<u8>(thread->ethread + kthread::SuspendCount))
        StopWhileSuspended(thread);
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
