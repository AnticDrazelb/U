// CPU: Xbox code runs on the host CPU in 32-bit compatibility mode (switch.S). Guest threads (KPCR,
// ETHREAD, stack, TLS), the kernel export stubs, and the fault handler: device registers are
// emulated instruction by instruction, privileged instructions skipped, anything else reported.
// The host's side of it (memory, fs, signals) is in platform_*.cpp.
#include <cctype>
#include <cstdio>
#include <unistd.h>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <thread>

#include "core.h"
#include "diagnostics.h"
#include "platform.h"

extern "C" {
extern const unsigned char xcs_begin[], xcs_end[], xcs_enter64[], xcs_gate32[], xcs_bail64[], xcs_back64[],
    xcs_raise32[], xcs_fault32[];
}

namespace xb {

namespace {

constexpr int kSlotSel32 = 0, kSlotSel64 = 8, kSlotGuestDs = 16, kSlotHandler = 24, kSlotFsGuest = 32,
              kSlotFsHost = 40, kSlotBugCheck = 48;
constexpr u32 kExportCount = 400, kStubSize = 16;

u32 g_switch = 0; // the switch's copy below 4 GiB
u32 g_stubs = 0;
u16 g_sel32 = 0, g_sel64 = 0, g_ds = 0;
thread_local GuestThread* tl_thread = nullptr;
std::atomic<u32> g_next_thread_id{1};

u64 SwitchEntry(const unsigned char* symbol)
{
    return g_switch + u64(symbol - xcs_begin);
}

bool Lar(u16 selector, u32& rights)
{
    u8 ok = 0;
    __asm__ volatile("lar %2, %0\n\tsetz %1" : "=r"(rights), "=q"(ok) : "r"(u32(selector)) : "cc");
    return ok;
}

u64 HostFsBase()
{
    return platform::HostFsBase();
}

extern "C" void GateEntry(u32* frame, u64 kpcr)
{
    auto* thread = reinterpret_cast<GuestThread*>(Read<u64>(u32(kpcr) + kpcr::HostThread));
    KernelCall(reinterpret_cast<GateFrame*>(frame), thread);
}

// ---- the fault handler

struct Regs
{
    platform::Fault& f;
    u32 Get(int r) const { return u32(*f.gpr[r]); }
    void Set(int r, u32 v) { *f.gpr[r] = (*f.gpr[r] & ~u64(0xffffffff)) | v; }
    u32 Get8(int r) const { return r < 4 ? Get(r) & 0xff : (Get(r - 4) >> 8) & 0xff; }
    void Set8(int r, u32 v)
    {
        if (r < 4)
            Set(r, (Get(r) & ~0xffu) | (v & 0xff));
        else
            Set(r - 4, (Get(r - 4) & ~0xff00u) | ((v & 0xff) << 8));
    }
    void Flags(u32 result, u32 size, bool carry = false)
    {
        const u32 mask = size == 4 ? 0xffffffff : size == 2 ? 0xffff : 0xff;
        result &= mask;
        u64 fl = *f.eflags & ~u64(0x8d5); // CF PF AF ZF SF OF
        if (!result)
            fl |= 0x40;
        if (result & ((mask >> 1) + 1))
            fl |= 0x80;
        if (carry)
            fl |= 1;
        *f.eflags = fl;
    }
};

// Length of a ModRM operand (after the ModRM byte).
size_t ModrmExtra(const u8* p)
{
    const int mod = p[0] >> 6, rm = p[0] & 7;
    if (mod == 3)
        return 0;
    size_t n = 0;
    if (rm == 4)
    {
        n++;
        if (mod == 0 && (p[1] & 7) == 5)
            n += 4;
    }
    else if (mod == 0 && rm == 5)
        n += 4;
    n += mod == 1 ? 1 : mod == 2 ? 4 : 0;
    return n;
}

// Emulates one instruction at `code` that accessed device memory at `address`. Returns its length.
size_t EmulateDeviceAccess(const u8* code, u32 address, Regs& r)
{
    const u8* p = code;
    u32 size = 4;
    for (;; p++)
    {
        if (*p == 0x66)
            size = 2;
        else if (*p == 0x2e || *p == 0x3e || *p == 0x26 || *p == 0x36 || *p == 0x64 || *p == 0x65 || *p == 0xf0)
            ;
        else
            break;
    }
    const u8 op = *p++;
    auto read = [&](u32 bytes) {
        u32 v = 0;
        if (!DeviceRead(address, bytes, v))
            v = 0;
        return v;
    };
    auto write = [&](u32 bytes, u32 v) { DeviceWrite(address, bytes, v); };
    auto reg_of = [&](const u8* m) { return (m[0] >> 3) & 7; };
    auto end = [&](const u8* m, size_t imm) { return size_t(m + 1 + ModrmExtra(m) + imm - code); };
    auto get_reg = [&](int reg, u32 bytes) { return bytes == 1 ? r.Get8(reg) : r.Get(reg) & (bytes == 2 ? 0xffff : ~0u); };
    auto set_reg = [&](int reg, u32 bytes, u32 v) {
        if (bytes == 1)
            r.Set8(reg, v);
        else if (bytes == 2)
            r.Set(reg, (r.Get(reg) & 0xffff0000) | (v & 0xffff));
        else
            r.Set(reg, v);
    };
    auto alu = [&](int kind, u32 a, u32 b, u32 bytes, bool& store) -> u32 {
        store = true;
        u32 v = 0;
        bool carry = false;
        switch (kind)
        {
        case 0: v = a + b; carry = (u64(a) + b) >> (bytes * 8); break;
        case 1: v = a | b; break;
        case 4: v = a & b; break;
        case 5: v = a - b; carry = a < b; break;
        case 6: v = a ^ b; break;
        case 7: v = a - b; carry = a < b; store = false; break;
        default: return a;
        }
        r.Flags(v, bytes, carry);
        return v;
    };
    switch (op)
    {
    case 0x88: write(1, r.Get8(reg_of(p))); return end(p, 0);
    case 0x89: write(size, get_reg(reg_of(p), size)); return end(p, 0);
    case 0x8a: r.Set8(reg_of(p), read(1)); return end(p, 0);
    case 0x8b: set_reg(reg_of(p), size, read(size)); return end(p, 0);
    case 0xc6: write(1, p[1 + ModrmExtra(p)]); return end(p, 1);
    case 0xc7:
    {
        u32 imm;
        std::memcpy(&imm, p + 1 + ModrmExtra(p), size);
        write(size, size == 2 ? imm & 0xffff : imm);
        return end(p, size);
    }
    case 0xa0: r.Set8(0, read(1)); return size_t(p + 4 - code);
    case 0xa1: set_reg(0, size, read(size)); return size_t(p + 4 - code);
    case 0xa2: write(1, r.Get8(0)); return size_t(p + 4 - code);
    case 0xa3: write(size, get_reg(0, size)); return size_t(p + 4 - code);
    case 0x80: case 0x81: case 0x83:
    {
        const u32 bytes = op == 0x80 ? 1 : size;
        const size_t imm_size = op == 0x81 ? bytes : 1;
        u32 imm = 0;
        std::memcpy(&imm, p + 1 + ModrmExtra(p), imm_size);
        if (op == 0x83)
            imm = u32(s32(int8_t(imm)));
        bool store;
        const u32 v = alu(reg_of(p), read(bytes), imm, bytes, store);
        if (store)
            write(bytes, v);
        return end(p, imm_size);
    }
    case 0x01: case 0x09: case 0x21: case 0x29: case 0x31: case 0x39: // op r/m, reg
    case 0x00: case 0x08: case 0x20: case 0x28: case 0x30: case 0x38:
    {
        const u32 bytes = (op & 1) ? size : 1;
        bool store;
        const u32 v = alu(op >> 3, read(bytes), get_reg(reg_of(p), bytes), bytes, store);
        if (store)
            write(bytes, v);
        return end(p, 0);
    }
    case 0x03: case 0x0b: case 0x23: case 0x2b: case 0x33: case 0x3b: // op reg, r/m
    {
        bool store;
        const u32 v = alu(op >> 3, get_reg(reg_of(p), size), read(size), size, store);
        if (store)
            set_reg(reg_of(p), size, v);
        return end(p, 0);
    }
    case 0x84: case 0x85:
    {
        const u32 bytes = op == 0x85 ? size : 1;
        r.Flags(read(bytes) & get_reg(reg_of(p), bytes), bytes);
        return end(p, 0);
    }
    case 0xf6: case 0xf7:
        if (reg_of(p) == 0)
        {
            const u32 bytes = op == 0xf7 ? size : 1;
            u32 imm = 0;
            std::memcpy(&imm, p + 1 + ModrmExtra(p), bytes);
            r.Flags(read(bytes) & imm, bytes);
            return end(p, bytes);
        }
        return 0;
    case 0x0f:
    {
        const u8 op2 = *p++;
        if (op2 == 0xb6 || op2 == 0xb7 || op2 == 0xbe || op2 == 0xbf)
        {
            const u32 bytes = (op2 & 1) ? 2 : 1;
            u32 v = read(bytes);
            if (op2 == 0xbe)
                v = u32(s32(int8_t(v)));
            else if (op2 == 0xbf)
                v = u32(s32(int16_t(v)));
            set_reg(reg_of(p), size == 2 ? 2 : 4, v);
            return end(p, 0);
        }
        return 0;
    }
    }
    return 0;
}

// Privileged instructions a game may run (the real kernel would): skipped. Returns the length.
size_t SkipPrivileged(const u8* p, Regs& r)
{
    switch (p[0])
    {
    case 0xfa: case 0xfb: return 1; // cli, sti
    case 0xe4: case 0xe5: r.Set(0, 0); return 2;           // in al/eax, imm8
    case 0xec: case 0xed: r.Set(0, 0); return 1;           // in al/eax, dx
    case 0xe6: case 0xe7: return 2;                        // out imm8, al/eax
    case 0xee: case 0xef: return 1;                        // out dx, al/eax
    case 0x0f:
        if (p[1] == 0x09 || p[1] == 0x08) // wbinvd, invd
            return 2;
        if (p[1] == 0x32) // rdmsr: EBL_CR_POWERON says 5.5 x 133 MHz (a 733 MHz Pentium III); others 0
        {
            r.Set(0, r.Get(1) == 0x2a ? 4u << 22 : 0);
            r.Set(2, 0);
            return 2;
        }
        if (p[1] == 0x30) // wrmsr
            return 2;
        if (p[1] == 0x20 || p[1] == 0x22) // mov from/to control register
            return 3;
        return 0;
    case 0xcd:
        // int 2dh: the kernel's debug service (eax 1: print the ecx string of edx bytes; 2, 3:
        // symbol loading). With no debugger the kernel carries on past the int 3 that follows.
        if (p[1] == 0x2d)
        {
            if (r.Get(0) == 1 && r.Get(1) >= kRamBase && r.Get(2) < 4096)
            {
                std::string text(Ptr<const char>(r.Get(1)), r.Get(2));
                while (!text.empty() && (text.back() == '\n' || text.back() == '\r'))
                    text.pop_back();
                Log("DbgPrint: %s", text.c_str());
            }
            return p[2] == 0xcc ? 3 : 2;
        }
        return 0;
    case 0x66:
        if (p[1] == 0xed) { r.Set(0, r.Get(0) & 0xffff0000); return 2; }
        if (p[1] == 0xef) return 2;
        return 0;
    }
    return 0;
}

// A fault in guest code becomes a structured exception, dispatched in the guest (switch.S,
// xcs_fault32) when the thread has handlers: the EXCEPTION_RECORD and the interrupted esp, edx, ecx,
// eax go on its stack. Not for faults in the dispatcher itself, nor without a usable stack.
bool DeliverException(platform::Fault& f, Regs& r, GuestThread* t)
{
    if (!t)
        return false;
    const u32 eip = u32(*f.rip), esp = r.Get(4);
    const u32 chain = Read<u32>(t->kpcr); // NtTib.ExceptionList
    if (!chain || chain == 0xffffffff)
        return false;
    if (eip >= g_switch && eip < g_switch + u32(xcs_end - xcs_begin))
        return false;
    if (esp < t->stack_limit + 0x1000 || esp > t->stack_top)
        return false;
    u32 code;
    switch (f.signal)
    {
    case 8: code = 0xC0000094; break;  // SIGFPE: integer divide by zero
    case 4: code = 0xC000001D; break;  // SIGILL: illegal instruction
    case 5: code = 0x80000003; break;  // SIGTRAP: breakpoint
    default: code = 0xC0000005; break; // access violation
    }
    const u32 record = (esp - 0x50) & ~3u;
    std::memset(Ptr(record), 0, 0x50);
    Write<u32>(record, code);
    Write<u32>(record + 12, eip);
    // every guest exception counted; the first hundred logged with where
    if (diag::g_exceptions++ < 100)
        Log("exception %#x at %#x in thread %u (fault address %#x): to the program's handlers", code, eip, t->id, f.address);
    if (code == 0xC0000005)
    {
        Write<u32>(record + 16, 2); // NumberParameters: read (0) or write, and the address
        Write<u32>(record + 20, 0);
        Write<u32>(record + 24, f.address);
    }
    const u32 frame = record - 16;
    Write<u32>(frame, r.Get(0));
    Write<u32>(frame + 4, r.Get(1));
    Write<u32>(frame + 8, r.Get(2));
    Write<u32>(frame + 12, esp);
    if (g_verbose >= 1)
        Log("cpu: exception %#x at %#x (address %#x) in thread %u: to its handlers", code, eip, f.address, t->id);
    r.Set(0, record);
    r.Set(2, eip);
    r.Set(4, frame);
    *f.rip = SwitchEntry(xcs_fault32);
    return true;
}

std::atomic<bool> g_fault_log{false}; // the first faults logged and synced from the handler (the self-test's step)

bool OnFault(platform::Fault& f)
{
    const u64 rip = *f.rip;
    const bool guest_fs = f.fs_base && f.fs_base < 0x100000000ull;
    Regs r{f};
    if (f.cs != g_sel32)
    {
        Log("host fault: signal %d at rip %#llx, address %#x%s", f.signal, (unsigned long long)rip, f.address,
            guest_fs ? " (with the guest's fs)" : "");
        return false;
    }
    const u8* code = Ptr<u8>(u32(rip));
    // the first faults, each on the disk before it's handled: if handling one kills the app, the
    // log ends with it
    static std::atomic<u32> s_faults{0};
    if (const u32 n = g_fault_log ? s_faults++ : 300; n < 300)
    {
        GuestThread* ft = guest_fs ? reinterpret_cast<GuestThread*>(Read<u64>(u32(f.fs_base) + kpcr::HostThread)) : nullptr;
        LogSync("cpu: fault %u in thread %u: signal %d at %#x, address %#x, code %02x %02x %02x %02x %02x %02x, eax %#x "
                "ecx %#x edx %#x esp %#x", n + 1, ft ? ft->id : 0, f.signal, u32(rip), f.address, code[0], code[1], code[2],
                code[3], code[4], code[5], r.Get(0), r.Get(1), r.Get(2), r.Get(4));
    }
    size_t length = 0;
    if (f.signal == 11 && (f.address >= kDeviceBase || (f.address >= kAliasBase && f.address - kAliasBase < kAliasSize)))
        length = EmulateDeviceAccess(code, f.address, r);
    if (!length)
        length = SkipPrivileged(code, r);
    if (length)
    {
        *f.rip = rip + length;
        return true;
    }
    GuestThread* t = guest_fs ? reinterpret_cast<GuestThread*>(Read<u64>(u32(f.fs_base) + kpcr::HostThread)) : nullptr;
    if (code[0] != 0xf4 && DeliverException(f, r, t))
        return true;
    if (code[0] == 0xf4) // hlt: the program stopped (an assertion, a fatal error): its message
    {
        const u32 ebp = r.Get(5);
        std::string args;
        for (int i = 0; i < 5; i++)
        {
            const u32 v = Read<u32>(ebp + 8 + i * 4);
            const bool text = v >= kRamBase && v < kRamEnd && std::isprint(Read<u8>(v));
            char buf[160];
            if (text)
                std::snprintf(buf, sizeof(buf), " \"%.120s\"", Ptr<const char>(v));
            else
                std::snprintf(buf, sizeof(buf), " %#x", v);
            args += buf;
        }
        Log("guest halted (hlt at %#x) in thread %u; its caller's arguments:%s", u32(rip), t ? t->id : 0, args.c_str());
    }
    Log("guest fault in thread %u: signal %d at %#x, address %#x, code %02x %02x %02x %02x %02x %02x, eax %#x "
        "ecx %#x edx %#x ebx %#x esp %#x ebp %#x esi %#x edi %#x",
        t ? t->id : 0, f.signal, u32(rip), f.address, code[0], code[1], code[2], code[3], code[4], code[5], r.Get(0),
        r.Get(1), r.Get(2), r.Get(3), r.Get(4), r.Get(5), r.Get(6), r.Get(7));
    // the calls that led there: the guest's frame pointer chain
    std::string chain;
    for (u32 ebp = r.Get(5), n = 0; n < 12 && ebp >= 0x10000 && (ebp & 3) == 0; n++)
    {
        u32 start, size;
        const bool mapped = (ebp >= kStackBase && ebp < kStackBase + kStackAreaSize) || QueryAllocation(ebp, start, size);
        if (!mapped)
            break;
        char buf[16];
        std::snprintf(buf, sizeof(buf), " %#x", Read<u32>(ebp + 4));
        chain += buf;
        const u32 next = Read<u32>(ebp);
        if (next <= ebp)
            break;
        ebp = next;
    }
    Log("  called from:%s", chain.c_str());
    diag::LogState("guest fault");
    *f.rip = SwitchEntry(xcs_bail64);
    platform::SetCs(f, g_sel64);
    return true;
}

} // namespace

GuestThread* CurrentThread()
{
    return tl_thread;
}

void InitCpu()
{
    Log("cpu: looking up the platform's functions");
    const std::string problem = platform::Init();
    if (!problem.empty())
        Fatal("%s", problem.c_str());
    __asm__("mov %%cs, %0" : "=r"(g_sel64));
    __asm__("mov %%ss, %0" : "=r"(g_ds));
    for (u16 index = 1; index < 32 && !g_sel32; index++)
    {
        u32 rights;
        const u16 sel = u16(index << 3 | 3);
        if (Lar(sel, rights) && (rights & 0x1800) == 0x1800 && (rights & 0x8000) && ((rights >> 13) & 3) == 3 &&
            !(rights & (1u << 21)) && (rights & (1u << 22)))
            g_sel32 = sel;
    }
    if (!g_sel32)
        Fatal("no 32-bit code segment");
    const size_t size = size_t(xcs_end - xcs_begin);
    g_switch = AllocKernel(u32(size), 128);
    std::memcpy(Ptr(g_switch), xcs_begin, size);
    Write<u64>(g_switch + kSlotSel32, g_sel32);
    Write<u64>(g_switch + kSlotSel64, g_sel64);
    Write<u64>(g_switch + kSlotGuestDs, g_ds);
    Write<u64>(g_switch + kSlotHandler, reinterpret_cast<u64>(&GateEntry));
    Write<u64>(g_switch + kSlotFsGuest, reinterpret_cast<u64>(platform::FsGuest()));
    Write<u64>(g_switch + kSlotFsHost, reinterpret_cast<u64>(platform::FsHost()));
    // export stubs: mov fs:[0x330], eax (the caller's eax, for RtlCaptureContext); mov eax, ordinal;
    // jmp xcs_gate32: 16 bytes
    g_stubs = AllocKernel(kExportCount * kStubSize, 16);
    const u32 gate = u32(SwitchEntry(xcs_gate32));
    for (u32 i = 0; i < kExportCount; i++)
    {
        u8* s = Ptr<u8>(g_stubs + i * kStubSize);
        const u8 save_eax[6] = {0x64, 0xa3, 0x30, 0x03, 0x00, 0x00};
        std::memcpy(s, save_eax, 6);
        s[6] = 0xb8;
        std::memcpy(s + 7, &i, 4);
        s[11] = 0xe9;
        const u32 rel = gate - (g_stubs + i * kStubSize + 16);
        std::memcpy(s + 12, &rel, 4);
    }
    Write<u64>(g_switch + kSlotBugCheck, ExportStub(96)); // KeBugCheckEx, for unhandled exceptions
    platform::InstallFaultHandler(OnFault);
    platform::PrepareThread();
    Log("cpu: 32-bit code segment %#x, 64-bit %#x, data %#x; switch at %#x", g_sel32, g_sel64, g_ds, g_switch);
    LogSync("cpu: fault handlers %s", platform::SignalRoute());
    // Self-test: a guest function that returns fs:[0x18] (the thread's NT_TIB.Self, its KPCR).
    // Right, it shows the switch into 32-bit code, the guest's fs and the way back all work.
    const u32 code = AllocKernel(16, 16);
    const u8 test[] = {0x64, 0xa1, 0x18, 0x00, 0x00, 0x00, 0xc3}; // mov eax, fs:[0x18]; ret
    std::memcpy(Ptr(code), test, sizeof(test));
    GuestThread* t = NewGuestThread(0x4000, 0, "self-test");
    Log("cpu self-test: switching to 32-bit code (if the app dies here, the console refuses it)");
    const u32 got = u32(CallGuest(t, code, {}));
    if (got != t->kpcr)
        Fatal("cpu self-test: guest code read fs:[0x18] = %#x, expected the KPCR %#x", got, t->kpcr);
    Log("cpu self-test: 32-bit code runs, its fs is its KPCR (%#x)", got);
    // Then faults in it, as every game makes (a privileged instruction here; a register access,
    // an exception): the console's signal, to the emulator's handler, and back into 32-bit code.
    // In steps, each named in g_selftest_file before it runs: a step the last run died in is left
    // out, and what it would switch on stays off (the app recovers by itself; the log says which).
    std::string died;
    if (!g_selftest_file.empty())
        if (FILE* f = std::fopen(g_selftest_file.c_str(), "r"))
        {
            // the step, then the build it was this one's (another build tries every step again)
            char line[64] = {}, build[64] = {};
            if (std::fgets(line, sizeof(line), f) && std::fgets(build, sizeof(build), f) &&
                std::string(build).rfind(__DATE__ " " __TIME__, 0) == 0)
                died = line;
            std::fclose(f);
            while (!died.empty() && (died.back() == '\n' || died.back() == '\r'))
                died.pop_back();
        }
    if (!died.empty())
        LogSync("cpu self-test: the last run died in the step \"%s\"", died.c_str());
    auto step = [&](const char* name, const char* what) {
        if (died == name)
        {
            LogSync("cpu self-test: step \"%s\" left out (%s stays off): the last run died in it", name, what);
            return false;
        }
        if (!g_selftest_file.empty())
            if (FILE* f = std::fopen(g_selftest_file.c_str(), "w"))
            {
                std::fprintf(f, "%s\n%s\n", name, __DATE__ " " __TIME__);
                std::fflush(f);
                fsync(fileno(f));
                std::fclose(f);
            }
        LogSync("cpu self-test: step \"%s\": %s (if the app dies here, the next start leaves it out)", name, what);
        return true;
    };
    const std::string handler = platform::CheckFaultHandler();
    if (!handler.empty())
        Log("cpu self-test: %s", handler.c_str());
    const u8 faulting[] = {0xfa, 0xfb, 0x64, 0xa1, 0x18, 0x00, 0x00, 0x00, 0xc3}; // cli; sti; mov eax, fs:[0x18]; ret
    std::memcpy(Ptr(code), faulting, sizeof(faulting));
    auto fault_test = [&](GuestThread* on) {
        const u32 after = u32(CallGuest(on, code, {}));
        if (after != on->kpcr)
            Fatal("cpu self-test: after the faults guest code read fs:[0x18] = %#x, expected the KPCR %#x", after, on->kpcr);
    };
    if (step("fault", "a fault in 32-bit code, to the emulator's handler"))
    {
        fault_test(t);
        Log("cpu self-test: faults in 32-bit code are handled, and the guest's fs is intact after them");
    }
    if (step("fault log", "each of the first faults logged to the disk from the handler"))
    {
        g_fault_log = true;
        fault_test(t);
        Log("cpu self-test: faults logged from the handler");
    }
    bool big_stacks = false;
    if (step("signal stack", "a bigger signal stack for each thread that runs guest code"))
    {
        big_stacks = true;
        platform::UseBigSignalStacks(true);
        fault_test(t);
        Log("cpu self-test: faults handled on the bigger signal stack");
    }
    if (step("new thread", "a fault on a new thread, as the program's threads run"))
    {
        GuestThread* t2 = NewGuestThread(0x4000, 0, "self-test 2");
        std::atomic<u32> other{0};
        StartHostThread(t2, [&] { other = u32(CallGuest(t2, code, {})) | 1u; });
        for (int i = 0; i < 5000 && !other; i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if ((other & ~1u) != (t2->kpcr & ~1u))
            Fatal("cpu self-test: on a new thread, guest code read fs:[0x18] = %#x, expected the KPCR %#x", other.load(),
                  t2->kpcr);
        Log("cpu self-test: faults on a new thread are handled too");
    }
    // a step that killed a run stays left out in the runs after (a new build tries every step again)
    if (!g_selftest_file.empty())
    {
        if (died.empty())
            std::remove(g_selftest_file.c_str());
        else if (FILE* f = std::fopen(g_selftest_file.c_str(), "w"))
        {
            std::fprintf(f, "%s\n%s\n", died.c_str(), __DATE__ " " __TIME__);
            std::fclose(f);
        }
    }
    LogSync("cpu self-test: done (fault logging %s, bigger signal stacks %s)", g_fault_log ? "on" : "off",
            big_stacks ? "on" : "off");
}

u16 GuestCodeSelector() { return g_sel32; }
u16 GuestDataSelector() { return g_ds; }

u32 ExceptionDispatcher()
{
    return u32(SwitchEntry(xcs_raise32));
}

u32 ExportStub(u32 ordinal)
{
    return ordinal < kExportCount ? g_stubs + ordinal * kStubSize : 0;
}

GuestThread* NewGuestThread(u32 stack_size, u32 tls_size, const char* name)
{
    auto* t = new GuestThread;
    diag::RegisterThread(t);
    t->id = g_next_thread_id++;
    t->name = name;
    stack_size = std::max<u32>((stack_size + 0xfff) & ~0xfffu, 64 * 1024);
    t->stack_top = AllocStack(stack_size + ((tls_size + 15) & ~15u));
    t->stack_limit = t->stack_top - stack_size - ((tls_size + 15) & ~15u);
    // TLS at the top of the stack, as the Xbox kernel puts it: TlsDataSize bytes below StackBase, a
    // pointer to the data, then the data (the XBE's template, then zeros), 16-byte aligned. Xbox code
    // finds the pointer at [StackBase + _tls_index * 4] (nxdk: _tls_index = -TlsDataSize / 4).
    const u32 stack_base = t->stack_top;
    u32 tls = 0;
    if (tls_size >= 4)
    {
        t->tls_area = stack_base - tls_size;
        tls = t->tls_area + 4;
        Write<u32>(t->tls_area, tls);
        if (g_xbe.tls_raw_end > g_xbe.tls_raw_start)
            std::memcpy(Ptr(tls), Ptr(g_xbe.tls_raw_start), std::min(g_xbe.tls_raw_end - g_xbe.tls_raw_start, tls_size - 4));
    }
    t->ethread = NewObjectBody(kthread::EthreadSize, "PsThreadObjectType");
    Write<u8>(t->ethread + 0, 6);                  // ThreadObject
    Write<u8>(t->ethread + 2, kthread::Size / 4); // Size in dwords
    Write<u32>(t->ethread + 8, t->ethread + 8);    // WaitListHead: empty
    Write<u32>(t->ethread + 12, t->ethread + 8);
    Write<u32>(t->ethread + kthread::StackBase, stack_base);
    Write<u32>(t->ethread + kthread::StackLimit, t->stack_limit);
    Write<u32>(t->ethread + kthread::TlsData, t->tls_area); // the pointer slot (nxdk adds 4)
    Write<u8>(t->ethread + kthread::Priority, 8);
    Write<u32>(t->ethread + kthread::EthreadUniqueThread, t->id);
    t->kpcr = AllocKernel(kpcr::BlockSize, 64);
    const u32 k = t->kpcr;
    Write<u32>(k + 0, 0xffffffff); // ExceptionList: the end of the SEH chain
    Write<u32>(k + kpcr::NtTibStackBase, stack_base);
    Write<u32>(k + kpcr::NtTibStackLimit, t->stack_limit);
    Write<u32>(k + kpcr::NtTibSelf, k);
    Write<u32>(k + kpcr::SelfPcr, k);
    Write<u32>(k + kpcr::Prcb, k + kpcr::CurrentThread);
    Write<u32>(k + kpcr::CurrentThread, t->ethread);
    Write<u64>(k + kpcr::HostThread, reinterpret_cast<u64>(t));
    Write<u64>(k + kpcr::Self, k);
    return t;
}

u64 CallGuest(GuestThread* t, u32 fn, std::initializer_list<u32> args)
{
    platform::PrepareThread();
    tl_thread = t;
    Write<u64>(t->kpcr + kpcr::HostFsBase, HostFsBase());
    platform::PrepareKpcr(t->kpcr, HostFsBase());
    u32 esp = ((t->tls_area ? t->tls_area : t->stack_top) - 64) & ~15u;
    esp -= u32(args.size() * 4 + 15) & ~15u;
    u32 a = esp;
    for (u32 v : args)
        Write<u32>(a, v), a += 4;
    using Enter = u64 (*)(u32, u32, u64);
    return reinterpret_cast<Enter>(SwitchEntry(xcs_enter64))(fn, esp, t->kpcr);
}

u64 CallGuestNested(u32 fn, std::initializer_list<u32> args)
{
    GuestThread* t = tl_thread;
    if (!t)
        Fatal("nested guest call outside a guest thread");
    const u64 saved_esp = Read<u64>(t->kpcr + kpcr::GuestEsp);
    u32 esp = (u32(saved_esp) - 128) & ~15u;
    esp -= u32(args.size() * 4 + 15) & ~15u;
    u32 a = esp;
    for (u32 v : args)
        Write<u32>(a, v), a += 4;
    Write<u64>(t->kpcr + kpcr::HostFsBase, HostFsBase());
    platform::PrepareKpcr(t->kpcr, HostFsBase());
    using Enter = u64 (*)(u32, u32, u64);
    const u64 result = reinterpret_cast<Enter>(SwitchEntry(xcs_enter64))(fn, esp, t->kpcr);
    Write<u64>(t->kpcr + kpcr::GuestEsp, saved_esp);
    return result;
}

void StartHostThread(GuestThread* t, std::function<void()> body)
{
    std::thread([t, body = std::move(body)] {
        platform::PrepareThread();
        tl_thread = t;
        body();
    }).detach();
}

} // namespace xb
