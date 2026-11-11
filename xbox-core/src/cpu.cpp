// CPU: Xbox code runs on the host CPU in 32-bit compatibility mode (switch.S). Guest threads (KPCR,
// ETHREAD, stack, TLS), the kernel export stubs, and the fault handler: device registers are
// emulated instruction by instruction, privileged instructions skipped, anything else reported.
// The host's side of it (memory, fs, signals) is in platform_*.cpp.
#include <cctype>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <unistd.h>
#include <sys/stat.h>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <thread>

#include "core.h"
#include "diagnostics.h"
#include "platform.h"

extern "C" {
extern const unsigned char xcs_begin[], xcs_end[], xcs_enter64[], xcs_gate32[], xcs_bail64[], xcs_back64[],
    xcs_raise32[], xcs_fault32[], xcs_fault64[];
}

namespace xb {

namespace {

constexpr int kSlotSel32 = 0, kSlotSel64 = 8, kSlotGuestDs = 16, kSlotHandler = 24, kSlotFsGuest = 32,
              kSlotFsHost = 40, kSlotBugCheck = 48, kSlotFault = 56;
constexpr u32 kExportCount = 400, kStubSize = 16;

u32 g_switch = 0; // the switch's copy below 4 GiB
u32 g_stubs = 0;
u16 g_sel32 = 0, g_sel64 = 0, g_ds = 0;
thread_local GuestThread* tl_thread = nullptr;
}
std::atomic<int> g_sampling{0};
namespace {

// ---- the Xbox's single processor (StopGuestThreads): a generation, odd while the interrupt thread's
// routine runs and the other guest threads are held
std::atomic<u64> g_stop_gen{0};
std::atomic<GuestThread*> g_stopper{nullptr};
std::mutex g_park_mutex;
std::condition_variable g_park_cv;
std::atomic<u64> g_stop_late{0};
std::atomic<u64> g_stops{0}, g_stop_wait_ns{0}, g_stop_signals{0}; // for the stats line
const bool g_single_processor = [] {
    const char* e = std::getenv("XB_UP");
    return !(e && e[0] == '0');
}();

// Back into guest code; held first while the interrupt thread's routine runs.
void EnterGuest(GuestThread* t)
{
    if (!t)
        return;
    for (;;)
    {
        t->running_guest.store(1, std::memory_order_seq_cst);
        const u64 gen = g_stop_gen.load(std::memory_order_seq_cst);
        if (!(gen & 1) || g_stopper.load(std::memory_order_relaxed) == t)
            return;
        t->running_guest.store(0, std::memory_order_seq_cst);
        std::unique_lock lock(g_park_mutex);
        g_park_cv.wait(lock, [gen] { return g_stop_gen.load() != gen; });
    }
}

// Out of guest code into the emulator's (a kernel call, a fault's handling, a call's end).
void LeaveGuest(GuestThread* t)
{
    if (t)
        t->running_guest.store(0, std::memory_order_seq_cst);
}
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
    LeaveGuest(thread);
    KernelCall(reinterpret_cast<GateFrame*>(frame), thread);
    EnterGuest(thread);
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
// With `memory`, the access is to ordinary guest memory (a backpatched instruction that met RAM).
bool IsDeviceAddress(u32 a);
size_t EmulateDeviceAccess(const u8* code, u32 address, Regs& r, bool memory = false, u32 eip = 0)
{
    const u8* p = code;
    u32 size = 4;
    bool rep = false;
    for (;; p++)
    {
        if (*p == 0x66)
            size = 2;
        else if (*p == 0xf3 || *p == 0xf2)
            rep = true;
        else if (*p == 0x2e || *p == 0x3e || *p == 0x26 || *p == 0x36 || *p == 0x64 || *p == 0x65 || *p == 0xf0)
            ;
        else
            break;
    }
    const u8 op = *p++;
    auto read = [&](u32 bytes) {
        u32 v = 0;
        if (memory)
            std::memcpy(&v, Ptr(address), bytes);
        else if (!DeviceRead(address, bytes, v, eip))
            v = 0;
        return v;
    };
    auto write = [&](u32 bytes, u32 v) {
        if (memory)
            std::memcpy(Ptr(address), &v, bytes);
        else
            DeviceWrite(address, bytes, v, eip);
    };
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
    // movs, stos, lods (rep or not), either side device memory or RAM: DirectSound copies blocks of
    // the APU's DSP memory with rep movsd. All the repeats are done here; stopped at an address
    // that is neither (esi, edi and ecx then say how far it got, as a fault inside rep leaves them).
    if ((op >= 0xa4 && op <= 0xa5) || (op >= 0xaa && op <= 0xad))
    {
        const u32 bytes = (op & 1) ? size : 1;
        const u32 step = (*r.f.eflags & 0x400) ? u32(-s32(bytes)) : bytes;
        auto valid = [&](u32 a) { return IsDeviceAddress(a) || GuestMapped(a, bytes); };
        auto rd = [&](u32 a) {
            u32 v = 0;
            if (IsDeviceAddress(a))
                DeviceRead(a, bytes, v, eip);
            else
                std::memcpy(&v, Ptr(a), bytes);
            return v;
        };
        auto wr = [&](u32 a, u32 v) {
            if (IsDeviceAddress(a))
                DeviceWrite(a, bytes, v, eip);
            else
                std::memcpy(Ptr(a), &v, bytes);
        };
        const bool src = op != 0xaa && op != 0xab, dst = op != 0xac && op != 0xad;
        for (u32 n = rep ? r.Get(1) : 1; n; n--)
        {
            const u32 si = r.Get(6), di = r.Get(7);
            if ((src && !valid(si)) || (dst && !valid(di)))
                return 0;
            if (op <= 0xa5)
                wr(di, rd(si));
            else if (op <= 0xab)
                wr(di, get_reg(0, bytes));
            else
                set_reg(0, bytes, rd(si));
            if (src)
                r.Set(6, si + step);
            if (dst)
                r.Set(7, di + step);
            if (rep)
                r.Set(1, n - 1);
        }
        return size_t(p - code);
    }
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

// ---- backpatching: a hardware-register access costs a fault (on the PS4 a signal and four context
// switches, ~10 us). The first time an instruction faults on device memory it is rewritten as a
// jump to a thunk of its own (pushfd; pushad; push site; call the gate's device-access stub; popad;
// popfd; jmp back), which reaches BackpatchedAccess through the kernel-call gate (~0.2 us) with the
// registers and flags on the guest stack; the decoder above does the access on a saved copy of the
// original instruction. Only instructions of 5 bytes or more in the program's own image whose first
// 5 bytes lie in one aligned 8-byte word are patched (one atomic store), so another thread never
// sees half a jump. XB_NO_BACKPATCH=1 turns it off.
constexpr u32 kBackpatchOrdinal = 399; // a free slot of the export stubs (xboxkrnl's end at 379)
struct PatchSite
{
    u32 eip, length, span, thunk;
    u8 code[16];
};
constexpr u32 kThunkMoved = 14; // where a thunk's copy of the moved instructions starts
std::mutex g_patch_mutex;
std::deque<PatchSite> g_sites;                 // by site number (stable: a deque only grows)
std::unordered_map<u32, u32> g_site_by_eip;    // under g_patch_mutex
std::unordered_set<u32> g_refused;             // instructions that can't be patched, under g_patch_mutex
std::atomic<u32> g_site_count{0};
std::atomic<u64> g_backpatched_accesses{0};
constexpr u32 kMaxSites = 4096, kThunkSize = 48;
u32 g_thunks = 0;

bool IsDeviceAddress(u32 a)
{
    return a >= kDeviceBase || (a >= kAliasBase && a - kAliasBase < kAliasSize);
}

// The data address of the memory operand of the instruction at `code`, from the registers; false if
// it has none this decodes.
bool EffectiveAddress(const u8* code, const Regs& r, u32 esp, u32& ea)
{
    const u8* p = code;
    while (*p == 0x66 || *p == 0x2e || *p == 0x3e || *p == 0x26 || *p == 0x36 || *p == 0xf0)
        p++;
    if (*p == 0x64 || *p == 0x65)
        return false; // fs/gs-relative: not device memory
    u8 op = *p++;
    if (op >= 0xa0 && op <= 0xa3)
    {
        std::memcpy(&ea, p, 4);
        return true;
    }
    if (op == 0x0f)
        op = *p++;
    auto reg = [&](int i) { return i == 4 ? esp : r.Get(i); };
    const int mod = p[0] >> 6, rm = p[0] & 7;
    if (mod == 3)
        return false;
    const u8* d = p + 1;
    u32 a = 0;
    if (rm == 4)
    {
        const u8 sib = *d++;
        const int scale = sib >> 6, index = (sib >> 3) & 7, base = sib & 7;
        if (index != 4)
            a += reg(index) << scale;
        if (base == 5 && mod == 0)
        {
            u32 disp;
            std::memcpy(&disp, d, 4);
            d += 4;
            a += disp;
        }
        else
            a += reg(base);
    }
    else if (rm == 5 && mod == 0)
    {
        u32 disp;
        std::memcpy(&disp, d, 4);
        ea = disp;
        return true;
    }
    else
        a += reg(rm);
    if (mod == 1)
        a += u32(s32(int8_t(*d)));
    else if (mod == 2)
    {
        u32 disp;
        std::memcpy(&disp, d, 4);
        a += disp;
    }
    ea = a;
    return true;
}

// The length of an instruction that can run anywhere (moved into a thunk after a patched one: no
// relative branch, no call), or 0. The common ones after a register access: mov, ALU ops, lea,
// movzx/movsx, push/pop, mov reg imm, ret.
size_t MovableLength(const u8* p)
{
    const u8* q = p;
    while (*q == 0x66 || *q == 0x2e || *q == 0x3e || *q == 0x26 || *q == 0x36)
        q++;
    const bool word = q != p && (p[0] == 0x66 || q[-1] == 0x66);
    const u8 op = *q++;
    auto modrm = [&](size_t imm) { return size_t(q - p) + 1 + ModrmExtra(q) + imm; };
    if ((op & 0xc4) == 0x00 && (op & 7) < 4) // ALU r/m forms 00-03, 08-0b, ..., 38-3b
        return modrm(0);
    if (op >= 0x50 && op <= 0x5f) // push/pop reg
        return size_t(q - p);
    if (op >= 0x88 && op <= 0x8b) // mov
        return modrm(0);
    if (op == 0x8d) // lea
        return modrm(0);
    if (op >= 0xb8 && op <= 0xbf) // mov reg, imm
        return size_t(q - p) + (word ? 2 : 4);
    if (op == 0xc7)
        return modrm(word ? 2 : 4);
    if (op == 0xc6 || op == 0x83)
        return modrm(1);
    if (op == 0x81)
        return modrm(word ? 2 : 4);
    if (op == 0x85 || op == 0x84)
        return modrm(0);
    if (op == 0xc3) // ret
        return size_t(q - p);
    if (op == 0xc2) // ret imm16
        return size_t(q - p) + 2;
    if (op == 0x0f && (*q == 0xb6 || *q == 0xb7 || *q == 0xbe || *q == 0xbf))
    {
        q++;
        return modrm(0);
    }
    return 0;
}

// Whether anything in the program could jump into (begin, end): a relative jump or call aimed there,
// or the address itself stored as data (a jump table). Conservative: a false alarm only means no patch.
bool MaybeBranchTarget(u32 begin, u32 end)
{
    if (end <= begin + 1)
        return false;
    const u8* image = Ptr<u8>(g_xbe.base);
    const u32 size = g_xbe.image_size;
    for (u32 i = 0; i + 5 <= size; i++)
    {
        const u8 b = image[i];
        u32 target = 0;
        if (b == 0xe8 || b == 0xe9)
        {
            s32 rel;
            std::memcpy(&rel, image + i + 1, 4);
            target = g_xbe.base + i + 5 + u32(rel);
        }
        else if (b == 0xeb || (b >= 0x70 && b <= 0x7f) || b == 0xe3)
            target = g_xbe.base + i + 2 + u32(s32(int8_t(image[i + 1])));
        else if (b == 0x0f && i + 6 <= size && image[i + 1] >= 0x80 && image[i + 1] <= 0x8f)
        {
            s32 rel;
            std::memcpy(&rel, image + i + 2, 4);
            target = g_xbe.base + i + 6 + u32(rel);
        }
        if (target > begin && target < end)
            return true;
        u32 word;
        std::memcpy(&word, image + i, 4);
        if (word > begin && word < end)
            return true;
    }
    return false;
}

// Writes `n` bytes at `at` in one atomic store: 8 bytes if they fit in an aligned 8-byte word, else 16
// (cmpxchg16b, which the PS4's Jaguar has) if they fit in an aligned 16-byte block.
bool CanWriteAtomic(u32 at, u32 n)
{
    return (at & 7) + n <= 8 || (at & 15) + n <= 16;
}

bool WriteAtomic(u32 at, const u8* data, u32 n)
{
    if ((at & 7) + n <= 8)
    {
        const u32 word = at & ~7u;
        u8 bytes[8];
        std::memcpy(bytes, Ptr(word), 8);
        std::memcpy(bytes + (at - word), data, n);
        u64 value;
        std::memcpy(&value, bytes, 8);
        __atomic_store_n(Ptr<u64>(word), value, __ATOMIC_SEQ_CST);
        return true;
    }
    if ((at & 15) + n <= 16)
    {
        const u32 block = at & ~15u;
        u64* p = Ptr<u64>(block);
        for (;;)
        {
            u64 lo = p[0], hi = p[1];
            u8 bytes[16];
            std::memcpy(bytes, &lo, 8);
            std::memcpy(bytes + 8, &hi, 8);
            std::memcpy(bytes + (at - block), data, n);
            u64 new_lo, new_hi;
            std::memcpy(&new_lo, bytes, 8);
            std::memcpy(&new_hi, bytes + 8, 8);
            bool ok;
            __asm__ volatile("lock cmpxchg16b %1\n\tsete %0"
                             : "=q"(ok), "+m"(*reinterpret_cast<volatile u64(*)[2]>(p)), "+a"(lo), "+d"(hi)
                             : "b"(new_lo), "c"(new_hi)
                             : "cc", "memory");
            if (ok)
                return true;
        }
    }
    return false;
}

bool WriteJump(u32 eip, u32 target)
{
    u8 jump[5] = {0xe9};
    const u32 rel = target - (eip + 5);
    std::memcpy(jump + 1, &rel, 4);
    return WriteAtomic(eip, jump, 5);
}

// A code cave near `eip` for a 5-byte jump: padding that never runs, within a short jump's reach of
// eip + 2. Runs of int3 (0xcc: MSVC's filler between functions, so the XDK's) or nops right after a
// function's end (pop ebp or leave, then ret: clang's). 0 if there's none. Caves used are remembered.
std::unordered_set<u32> g_caves_used; // under g_patch_mutex
u32 FindCave(u32 eip)
{
    const u32 from = std::max(g_xbe.base + 2, eip + 2 >= 128 ? eip + 2 - 128 : 0), to = eip + 2 + 127 - 5;
    if (to + 8 >= g_xbe.base + g_xbe.image_size)
        return 0;
    auto is_nop_run = [](const u8* p) {
        // the multi-byte nops: 90, 66 90, 0f 1f 00, 0f 1f 40 00, 0f 1f 44 00 00, 66 0f 1f 44 00 00, ...
        return p[0] == 0x90 || (p[0] == 0x66 && (p[1] == 0x90 || p[1] == 0x0f || p[1] == 0x2e || p[1] == 0x66)) ||
               (p[0] == 0x0f && p[1] == 0x1f);
    };
    for (u32 a = from; a <= to; a++)
    {
        if (a + 5 > eip && a < eip + 16)
            continue; // not over the instruction itself
        bool reserved = false;
        for (u32 k = 0; k < 5 && !reserved; k++)
            reserved = g_caves_used.count(a + k) != 0;
        if (reserved)
            continue;
        const u8* p = Ptr<u8>(a);
        if (p[0] == 0xcc && p[1] == 0xcc && p[2] == 0xcc && p[3] == 0xcc && p[4] == 0xcc && p[-1] == 0xcc)
            return a; // inside a run of int3 (not its first byte: that one may follow a call's last byte)
        // nops starting right after "pop ebp; ret" or "leave; ret", 5 bytes of them at least
        if ((p[-1] == 0xc3 && (p[-2] == 0x5d || p[-2] == 0xc9)) && is_nop_run(p))
        {
            u32 len = 0;
            while (len < 16 && (p[len] == 0x90 || p[len] == 0x66 || p[len] == 0x0f || p[len] == 0x1f || p[len] == 0x00 ||
                                p[len] == 0x40 || p[len] == 0x44 || p[len] == 0x84 || p[len] == 0x80 || p[len] == 0x2e))
                len++;
            if (len >= 5)
                return a;
        }
    }
    return 0;
}

// After the fault at `eip` (an instruction of `length` bytes) was emulated: its patch, if it can have
// one. An instruction shorter than the jump takes the movable instructions after it into its thunk.
// Returns where the faulting thread goes on: after the instruction, or (if what followed it moved)
// to the thunk's copy of what follows.
u32 Backpatch(u32 eip, size_t length)
{
    const u32 next = eip + u32(length);
    if (InSignalHandler())
        return next; // nothing below (a static's guard, a mutex) runs in the handler
    static const bool off = std::getenv("XB_NO_BACKPATCH") != nullptr;
    if (off || length > 15 || eip < g_xbe.base || eip + 16 >= g_xbe.base + g_xbe.image_size)
        return next;
    std::lock_guard lock(g_patch_mutex);
    if (g_site_by_eip.count(eip) || g_refused.count(eip) || g_sites.size() >= kMaxSites)
        return next;
    // a short instruction: a short jump to a 5-byte jump in nearby padding, if there is some
    static const bool no_caves = std::getenv("XB_NO_CAVES") != nullptr;
    static const char* only_cave = std::getenv("XB_ONLY_CAVE"); // debugging: just the cave patch at this address
    static u32 caves_made = 0;
    static const u32 max_caves = std::getenv("XB_MAX_CAVES") ? u32(std::atoi(std::getenv("XB_MAX_CAVES"))) : ~0u;
    const bool direct = length >= 5 && CanWriteAtomic(eip, 5);
    if (!direct && length >= 2 && !no_caves && (!only_cave || std::strtoul(only_cave, nullptr, 0) == eip) && caves_made < max_caves)
        if (const u32 cave = CanWriteAtomic(eip, 2) ? FindCave(eip) : 0; cave && ++caves_made)
        {
            if (!g_thunks)
                g_thunks = AllocKernel(kMaxSites * kThunkSize, 16);
            const u32 site = u32(g_sites.size());
            const u32 thunk = g_thunks + site * kThunkSize;
            PatchSite ps{eip, u32(length), u32(length), thunk, {}};
            std::memcpy(ps.code, Ptr<u8>(eip), length);
            u8* t = Ptr<u8>(thunk);
            size_t n = 0;
            t[n++] = 0x9c, t[n++] = 0x60, t[n++] = 0x68;
            std::memcpy(t + n, &site, 4), n += 4;
            t[n++] = 0xe8;
            const u32 call_rel = ExportStub(kBackpatchOrdinal) - (thunk + u32(n) + 4);
            std::memcpy(t + n, &call_rel, 4), n += 4;
            t[n++] = 0x61, t[n++] = 0x9d, t[n++] = 0xe9;
            const u32 back_rel = next - (thunk + u32(n) + 4);
            std::memcpy(t + n, &back_rel, 4), n += 4;
            // the cave's jump first (it never runs until the short jump is there), then the short jump
            u8* c = Ptr<u8>(cave);
            c[0] = 0xe9;
            const u32 cave_rel = thunk - (cave + 5);
            std::memcpy(c + 1, &cave_rel, 4);
            const u8 short_jump[2] = {0xeb, u8(int8_t(s32(cave) - s32(eip + 2)))};
            g_sites.push_back(ps);
            g_site_by_eip[eip] = site;
            g_site_count++;
            for (u32 k = 0; k < 5; k++)
                g_caves_used.insert(cave + k);
            if (!WriteAtomic(eip, short_jump, 2))
                return next;
            if (site < 64 || std::getenv("XB_LOG_PATCHES"))
                Log("cpu: hardware access at %#x patched (site %u, %u bytes: a short jump to padding at %#x: %02x %02x %02x)",
                    eip, site, u32(length), cave, ps.code[0], ps.code[1], ps.code[2]);
            return next;
        }
    // the span replaced: the instruction and, if it's short, the movable ones after it
    u32 span = u32(length);
    while (span < 5)
    {
        const size_t more = MovableLength(Ptr<u8>(eip + span));
        const u8 last = Ptr<u8>(eip + span)[0];
        if (!more)
            break;
        span += u32(more);
        if (last == 0xc3 || last == 0xc2)
            break; // a ret: nothing after it is reached through here
    }
    if (span < 5 || !CanWriteAtomic(eip, 5) || (span > length && MaybeBranchTarget(eip, eip + span)))
    {
        g_refused.insert(eip);
        return next;
    }
    if (!g_thunks)
        g_thunks = AllocKernel(kMaxSites * kThunkSize, 16);
    const u32 site = u32(g_sites.size());
    const u32 thunk = g_thunks + site * kThunkSize;
    PatchSite ps{eip, u32(length), span, thunk, {}};
    std::memcpy(ps.code, Ptr<u8>(eip), length);
    u8* t = Ptr<u8>(thunk);
    size_t n = 0;
    t[n++] = 0x9c; // pushfd
    t[n++] = 0x60; // pushad
    t[n++] = 0x68; // push site
    std::memcpy(t + n, &site, 4), n += 4;
    t[n++] = 0xe8; // call the stub
    const u32 call_rel = ExportStub(kBackpatchOrdinal) - (thunk + u32(n) + 4);
    std::memcpy(t + n, &call_rel, 4), n += 4;
    t[n++] = 0x61; // popad
    t[n++] = 0x9d; // popfd
    // the moved instructions, then back to what follows them
    std::memcpy(t + n, Ptr<u8>(eip + u32(length)), span - length), n += span - length;
    t[n++] = 0xe9;
    const u32 back_rel = (eip + span) - (thunk + u32(n) + 4);
    std::memcpy(t + n, &back_rel, 4), n += 4;
    if (n > kThunkSize)
        return next;
    g_sites.push_back(ps);
    g_site_by_eip[eip] = site;
    g_site_count++;
    if (!WriteJump(eip, thunk))
        return next;
    const u32 resume = span > length ? thunk + kThunkMoved : next;
    if (site < 64 || std::getenv("XB_LOG_PATCHES"))
        Log("cpu: hardware access at %#x patched (site %u, %u bytes%s: %02x %02x %02x %02x %02x %02x)", eip, site, span,
            span > length ? " with the instructions after it" : "", ps.code[0], ps.code[1], ps.code[2], ps.code[3],
            ps.code[4], ps.code[5]);
    return resume;
}

// A thread at `eip` inside a patched span (it faulted on the instruction just as another thread
// patched it, or it was between two of the moved instructions): where it goes on instead.
bool PatchedSpanResume(u32 eip, u32& resume)
{
    if (g_site_count == 0 || InSignalHandler())
        return false;
    std::lock_guard lock(g_patch_mutex);
    auto it = g_site_by_eip.find(eip);
    if (it != g_site_by_eip.end())
    {
        const u8 b = Ptr<u8>(eip)[0];
        if (b != 0xe9 && b != 0xeb)
            return false; // not written (yet): emulated as before
        resume = eip; // the jump into the thunk is there now
        return true;
    }
    for (const PatchSite& ps : g_sites)
        if (eip > ps.eip && eip < ps.eip + ps.span)
        {
            if (eip < ps.eip + ps.length)
                return false;
            // a boundary between moved instructions: the same place in the thunk's copy
            resume = ps.thunk + kThunkMoved + (eip - ps.eip - ps.length);
            return true;
        }
    return false;
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
    // a first handler record that can't be dispatched (its handler 0 or not mapped: the stack
    // overwritten) would only fault again, each time deeper in the stack until it runs out: the
    // fault is reported as unhandled instead
    if (!GuestMapped(chain, 8) || !GuestMapped(Read<u32>(chain + 4), 1) || Read<u32>(chain + 4) < kRamBase)
    {
        Log("cpu: thread %u's exception handler record at %#x points at %#x: not dispatched (the stack was overwritten?)",
            t->id, chain, GuestMapped(chain, 8) ? Read<u32>(chain + 4) : 0);
        return false;
    }
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
    // every guest exception counted; the first hundred logged with where, all the registers, the
    // calls that led there (the frame pointer chain), the stack and the handler chain
    if (diag::g_exceptions++ < 100)
    {
        Log("exception %#x at %#x in thread %u (fault address %#x): to the program's handlers", code, eip, t->id, f.address);
        Log("  eax %#x ecx %#x edx %#x ebx %#x esp %#x ebp %#x esi %#x edi %#x eflags %#x", r.Get(0), r.Get(1), r.Get(2),
            r.Get(3), esp, r.Get(5), r.Get(6), r.Get(7), u32(*f.eflags));
        auto on_stack = [&](u32 a) { return a >= t->stack_limit && a + 4 <= t->stack_top && !(a & 3); };
        std::string calls, stack, handlers;
        std::vector<u32> returns;
        char b[24];
        for (u32 ebp = r.Get(5), n = 0; n < 16 && on_stack(ebp) && on_stack(ebp + 4); n++)
        {
            std::snprintf(b, sizeof(b), " %#x", Read<u32>(ebp + 4));
            calls += b;
            returns.push_back(Read<u32>(ebp + 4));
            const u32 next = Read<u32>(ebp);
            if (next <= ebp)
                break;
            ebp = next;
        }
        for (u32 i = 0; i < 128 && on_stack(esp + i * 4); i++)
        {
            std::snprintf(b, sizeof(b), "%s%08x", i % 8 ? " " : (i ? "\n    " : "    "), Read<u32>(esp + i * 4));
            stack += b;
        }
        for (u32 rec = chain, n = 0; n < 8 && on_stack(rec) && on_stack(rec + 4); n++)
        {
            std::snprintf(b, sizeof(b), " %#x->%#x", rec, Read<u32>(rec + 4));
            handlers += b;
            rec = Read<u32>(rec);
        }
        Log("  called from (frame chain):%s", calls.c_str());
        Log("  handler records (address->handler):%s", handlers.c_str());
        Log("  stack at esp:\n%s", stack.c_str());
        if (GuestMapped(eip > 16 ? eip - 16 : 0, 48))
        {
            std::string around;
            for (u32 i = 0; i < 48; i++)
            {
                std::snprintf(b, sizeof(b), i == 16 ? " [%02x" : (i == 17 ? "] %02x" : " %02x"), Read<u8>(eip - 16 + i));
                around += b;
            }
            Log("  code around eip (16 before, [eip]):%s", around.c_str());
        }
        // the code that made each call in the chain: 384 bytes before each return address (to read
        // what the program was doing there; the first few exceptions only)
        if (diag::g_exceptions <= 4)
            for (size_t k = 0; k < returns.size() && k < 4; k++)
            {
                const u32 ret = returns[k], from = ret > 384 ? ret - 384 : 0;
                if (!ret || !GuestMapped(from, 384 + 32))
                    continue;
                std::string bytes;
                for (u32 a = from; a < ret + 32; a++)
                {
                    if ((a - from) % 32 == 0)
                    {
                        std::snprintf(b, sizeof(b), "\n    %08x:", a);
                        bytes += b;
                    }
                    std::snprintf(b, sizeof(b), " %02x", Read<u8>(a));
                    bytes += b;
                }
                Log("  code before the return address %#x:%s", ret, bytes.c_str());
            }
    }
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

// A fault in guest code, handled: 0 when the guest resumes (*f.rip and the registers updated), 1
// when it can't (the call into it bails). Outside the signal handler (FaultResumed), except for the
// rare fault OnFault can't send there.
int HandleGuestFault(platform::Fault& f)
{
    const u64 rip = *f.rip;
    const bool guest_fs = f.fs_base && f.fs_base < 0x100000000ull;
    Regs r{f};
    // the instruction's bytes; zeros when eip isn't mapped (a jump to nowhere: an access violation
    // for the program, not a read the emulator itself faults on)
    static const u8 kNoCode[16] = {};
    const bool code_mapped = GuestMapped(u32(rip), 16);
    const u8* code = code_mapped ? Ptr<u8>(u32(rip)) : kNoCode;
    // the faulting guest thread: its KPCR's, or (no fs base known) the one whose stack holds esp
    GuestThread* const fault_thread =
        guest_fs ? reinterpret_cast<GuestThread*>(Read<u64>(u32(f.fs_base) + kpcr::HostThread)) : diag::ThreadByStack(r.Get(4));
    u32 regs[8];
    for (int i = 0; i < 8; i++)
        regs[i] = r.Get(i);
    // the first faults, each on the disk before it's handled: if handling one kills the app, the
    // log ends with it
    static std::atomic<u32> s_faults{0};
    if (const u32 n = g_fault_log ? s_faults++ : 300; n < 300)
    {
        GuestThread* ft = fault_thread;
        LogSync("cpu: fault %u in thread %u: signal %d at %#x%s, address %#x, code %02x %02x %02x %02x %02x %02x, eax %#x "
                "ecx %#x edx %#x ebx %#x esp %#x ebp %#x esi %#x edi %#x", n + 1, ft ? ft->id : 0, f.signal, u32(rip),
                code_mapped ? "" : " (not mapped)", f.address, code[0], code[1], code[2], code[3], code[4], code[5], r.Get(0),
                r.Get(1), r.Get(2), r.Get(3), r.Get(4), r.Get(5), r.Get(6), r.Get(7));
    }
    // every fault into the ring the reports show (the last ones, all their registers and code)
    auto record = [&](const char* outcome) {
        diag::RecordFault(fault_thread ? fault_thread->id : 0, f.signal, u32(rip), f.address, code, regs, outcome);
    };
    size_t length = 0;
    // inside a patched span (patched by another thread after this one faulted, or stopped between
    // moved instructions): on from the matching place
    if (u32 resume; PatchedSpanResume(u32(rip), resume))
    {
        *f.rip = resume;
        return 0;
    }
    const bool device = f.signal == 11 && IsDeviceAddress(f.address);
    if (device)
        length = EmulateDeviceAccess(code, f.address, r, false, u32(rip));
    if (device && length)
    {
        record("a hardware register, emulated");
        *f.rip = Backpatch(u32(rip), length);
        return 0;
    }
    if (!length)
        length = SkipPrivileged(code, r);
    if (length)
    {
        record(f.signal == 11 && f.address >= kDeviceBase ? "a hardware register, emulated" : "emulated or skipped");
        *f.rip = rip + length;
        return 0;
    }
    GuestThread* t = fault_thread;
    if (code[0] != 0xf4 && DeliverException(f, r, t))
    {
        record("to the program's exception handlers");
        return 0;
    }
    record(code[0] == 0xf4 ? "hlt: the program stopped" : "not handled: the thread stops");
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
    // the code around it, and around each call it's in (return addresses on the stack, into the
    // program's image): to read what it was doing (the frame pointer chain is often empty)
    auto code_window = [](const char* what, u32 at, u32 before, u32 after) {
        const u32 from = at - before;
        if (!GuestMapped(from, before + after))
            return;
        std::string hex;
        for (u32 a = from; a < at + after; a++)
        {
            char b[4];
            std::snprintf(b, sizeof(b), a == at ? "|%02x" : " %02x", Read<u8>(a));
            hex += b;
        }
        Log("  %s %#x (from %#x):%s", what, at, from, hex.c_str());
    };
    code_window("code at", u32(rip), 96, 32);
    {
        u32 shown = 0;
        const u32 esp = r.Get(4);
        for (u32 i = 0; i < 128 && shown < 10 && GuestMapped(esp + i * 4, 4); i++)
        {
            const u32 v = Read<u32>(esp + i * 4);
            if (v < g_xbe.base + 8 || v >= g_xbe.base + g_xbe.image_size || !GuestMapped(v - 8, 8))
                continue;
            // a call before it: e8 rel32 (5 bytes) or ff /2 (2 to 7 bytes)
            const bool call = Read<u8>(v - 5) == 0xe8 || Read<u8>(v - 2) == 0xff || Read<u8>(v - 3) == 0xff ||
                              Read<u8>(v - 6) == 0xff || Read<u8>(v - 7) == 0xff;
            if (!call)
                continue;
            char what[48];
            std::snprintf(what, sizeof(what), "returns to (esp+%#x)", i * 4);
            code_window(what, v, 64, 16);
            shown++;
        }
    }
    diag::LogState("guest fault");
    return 1;
}

// The signal handler: no C library in here (on the PS4 it can't be used where the kernel enters the
// handler directly). A fault in guest code is saved in the thread's KPCR and the thread sent on to
// xcs_fault64, which has it handled (FaultResumed) on the emulator's stack and resumes the guest.
// What can't go there (no KPCR, a fault in the switch's own code) is handled here as before.
uint64_t FindKpcr(uint64_t kernel_value, uint64_t context_value, uint64_t rsp, uint16_t cs)
{
    auto is_kpcr = [](uint64_t k) {
        return k >= kKernelBase && k + kpcr::Self + 8 <= u64(kKernelBase) + kKernelSize &&
               Read<u32>(u32(k) + kpcr::NtTibSelf) == k && Read<u64>(u32(k) + kpcr::Self) == k;
    };
    if (is_kpcr(kernel_value))
        return kernel_value;
    if (is_kpcr(context_value))
        return context_value;
    if (cs == g_sel32 && rsp < 0x100000000ull)
        if (const GuestThread* t = diag::ThreadByStack(u32(rsp)); t && is_kpcr(t->kpcr))
            return t->kpcr;
    return 0;
}

bool OnFault(platform::Fault& f)
{
    if (f.signal == platform::SampleSignal())
    {
        // a sample of where a thread is: in guest code, its eip, registers and code are noted on its
        // GuestThread (plain stores, nothing fs-relative); anywhere else nothing. It resumes as it was.
        if (f.cs == g_sel32 && g_sampling.load(std::memory_order_relaxed))
        {
            const bool guest_fs = f.fs_base && f.fs_base < 0x100000000ull;
            GuestThread* t = guest_fs ? reinterpret_cast<GuestThread*>(Read<u64>(u32(f.fs_base) + kpcr::HostThread))
                                      : diag::ThreadByStack(u32(*f.gpr[4]));
            if (t)
            {
                const u32 eip = u32(*f.rip), n = t->samples.load(std::memory_order_relaxed);
                t->sample_eip[n % GuestThread::kSamples] = eip;
                for (int i = 0; i < 8; i++)
                    t->sample_regs[i] = u32(*f.gpr[i]);
                if (GuestMapped(eip - 16, 48))
                    std::memcpy(t->sample_code, Ptr<u8>(eip - 16), 48);
                t->samples.store(n + 1, std::memory_order_release);
            }
        }
        // held while the interrupt thread's routine runs (StopGuestThreads), where it is: in guest
        // code, or between the emulator's code and the guest's (the switch: nothing held there)
        const u64 gen = g_stop_gen.load(std::memory_order_acquire);
        if (gen & 1)
        {
            GuestThread* t = nullptr;
            const u64 fs = f.fs_base;
            if (fs >= kKernelBase && fs + kpcr::Self + 8 <= u64(kKernelBase) + kKernelSize &&
                Read<u32>(u32(fs) + kpcr::NtTibSelf) == fs)
                t = reinterpret_cast<GuestThread*>(Read<u64>(u32(fs) + kpcr::HostThread));
            else
                for (u32 i = 0, n = diag::ThreadCount(); i < n && !t; i++)
                    if (GuestThread* c = diag::ThreadAt(i); c && c->kpcr && fs && Read<u64>(c->kpcr + kpcr::HostFsBase) == fs)
                        t = c;
            if (t && t != g_stopper.load(std::memory_order_relaxed) && t->running_guest.load(std::memory_order_seq_cst))
            {
                t->held.store(1, std::memory_order_seq_cst);
                while (g_stop_gen.load(std::memory_order_acquire) == gen)
                    __builtin_ia32_pause();
                t->held.store(0, std::memory_order_release);
            }
        }
        return true;
    }
    if (f.cs != g_sel32)
    {
        platform::Trail(4, 0, *f.rip, f.fs_base, f.address);
        return false; // the emulator's own: the crash log's
    }
    const u32 eip = u32(*f.rip);
    const bool guest_fs = f.fs_base && f.fs_base < 0x100000000ull;
    const bool in_switch = eip >= g_switch && eip < g_switch + u32(xcs_end - xcs_begin);
    platform::Trail(4, (!guest_fs || in_switch) ? 1 : 2, eip, f.fs_base, f.address);
    if (!guest_fs)
    {
        // 3 no KPCR from the fs base: the thread its esp says, and that thread's KPCR
        const GuestThread* by_stack = diag::ThreadByStack(u32(*f.gpr[4]));
        platform::Trail(3, u32(*f.gpr[4]), by_stack ? by_stack->id : 0, by_stack ? by_stack->kpcr : 0, f.signal);
    }
    if (!guest_fs || in_switch)
    {
        if (HandleGuestFault(f) != 0)
        {
            *f.rip = SwitchEntry(xcs_bail64);
            platform::SetCs(f, g_sel64);
        }
        return true;
    }
    const u32 k = u32(f.fs_base);
    for (int i = 0; i < 8; i++)
        Write<u32>(k + kpcr::FaultRegs + u32(i) * 4, u32(*f.gpr[i]));
    Write<u32>(k + kpcr::FaultEip, eip);
    Write<u32>(k + kpcr::FaultEflags, u32(*f.eflags));
    Write<u32>(k + kpcr::FaultSignal, u32(f.signal));
    Write<u32>(k + kpcr::FaultAddress, f.address);
    *f.rip = SwitchEntry(xcs_fault64);
    platform::SetCs(f, g_sel64);
    platform::SetSwitchArg(f, k);
    return true;
}

// xcs_fault64's: the saved fault handled with the whole C library at hand; 0 resume, 1 bail.
extern "C" u32 FaultResumed(u64 kpcr_address)
{
    const u32 k = u32(kpcr_address);
    auto* thread = reinterpret_cast<GuestThread*>(Read<u64>(k + kpcr::HostThread));
    LeaveGuest(thread);
    platform::Trail(6, k, Read<u32>(k + kpcr::FaultEip), Read<u32>(k + kpcr::FaultSignal), Read<u32>(k + kpcr::FaultAddress));
    u64 gpr[8], rip = Read<u32>(k + kpcr::FaultEip), eflags = Read<u32>(k + kpcr::FaultEflags);
    for (int i = 0; i < 8; i++)
        gpr[i] = Read<u32>(k + kpcr::FaultRegs + u32(i) * 4);
    platform::Fault f{};
    for (int i = 0; i < 8; i++)
        f.gpr[i] = &gpr[i];
    f.rip = &rip;
    f.eflags = &eflags;
    f.cs = g_sel32;
    f.signal = int(Read<u32>(k + kpcr::FaultSignal));
    f.address = Read<u32>(k + kpcr::FaultAddress);
    f.fs_base = k;
    f.raw = nullptr;
    const int result = HandleGuestFault(f);
    for (int i = 0; i < 8; i++)
        Write<u32>(k + kpcr::FaultRegs + u32(i) * 4, u32(gpr[i]));
    Write<u32>(k + kpcr::FaultEip, u32(rip));
    Write<u32>(k + kpcr::FaultEflags, u32(eflags));
    platform::Trail(7, u32(result), rip, eflags, Read<u32>(k + kpcr::FaultRegs + 4 * 4));
    if (result == 0)
        EnterGuest(thread);
    return u32(result);
}

} // namespace

// A backpatched instruction, through the gate: its thunk pushed eflags, then the registers (pushad),
// then its site number. Done on the original instruction with those registers, which popad and popfd
// then restore.
void BackpatchedAccess(GateFrame* frame)
{
    frame->edx = 4; // the site number: popped as stdcall pops its arguments
    const u32 site = frame->args[0];
    if (site >= g_site_count)
        return;
    const PatchSite& ps = g_sites[site];
    // args[1..8]: edi esi ebp esp ebx edx ecx eax (pushad's order in memory); args[9]: eflags
    static constexpr int kSlot[8] = {8, 7, 6, 5, 4, 3, 2, 1}; // eax..edi -> args index
    u64 g[8], eflags = frame->args[9];
    for (int i = 0; i < 8; i++)
        g[i] = frame->args[kSlot[i]];
    platform::Fault f{};
    for (int i = 0; i < 8; i++)
        f.gpr[i] = &g[i];
    f.eflags = &eflags;
    Regs r{f};
    const u32 esp = frame->args[4] + 4; // the instruction's esp: before pushfd
    u32 ea = 0;
    if (!EffectiveAddress(ps.code, r, esp, ea))
        return;
    g_backpatched_accesses++;
    // an access by the patched instruction to memory that isn't mapped (its register pointing nowhere
    // this time): the Xbox would fault; it isn't done (the registers stay as they were), and said,
    // rather than the emulator faulting on it itself
    if (!IsDeviceAddress(ea) && !GuestMapped(ea, 16))
    {
        static std::atomic<u32> told{0};
        if (told++ < 20)
            Log("cpu: the patched instruction at %#x in thread %u accessed %#x, which isn't mapped (the Xbox would "
                "fault): not done", ps.eip, CurrentThread() ? CurrentThread()->id : 0, ea);
        return;
    }
    // as long as the console's own access takes, at least: a register read crosses the PCI bus there
    // (about 1 us), and drivers wait by counting reads, which mustn't run out before the device changes
    static const double min_us = std::getenv("XB_ACCESS_US") ? std::atof(std::getenv("XB_ACCESS_US")) : 1.0;
    const auto start = std::chrono::steady_clock::now();
    EmulateDeviceAccess(ps.code, ea, r, !IsDeviceAddress(ea), ps.eip);
    if (IsDeviceAddress(ea))
        while (std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() < min_us)
            __builtin_ia32_pause();
    for (int i = 0; i < 8; i++)
        if (i != 4)
            frame->args[kSlot[i]] = u32(g[i]);
    frame->args[9] = u32(eflags);
}

u64 BackpatchedAccessCount() { return g_backpatched_accesses; }
u32 BackpatchedSiteCount() { return g_site_count; }

GuestThread* CurrentThread()
{
    return tl_thread;
}

namespace {
// The stages the fault path writes to the trail (platform::Trail), for the report of the last run's.
const char* StageName(u32 stage)
{
    switch (stage)
    {
    case 1: return "handler entered: signal, rip, cs, its stack";
    case 2: return "the context: fs base, fault address, trapno | err << 32, rflags | ss << 32";
    case 3: return "no KPCR from the fs base: esp, the guest thread its stack is, that thread's KPCR, signal";
    case 11: return "the fs base, in detail: the kernel's answer (result << 32 | ok), its value, the context's copy, the context's flags | fs selector << 32";
    case 12: return "in the handler's table: its signal stack (base, size: 0 the thread's own), slot";
    case 4: return "classified: 0 not ours, 1 handled in the handler, 2 sent to xcs_fault64; eip, fs base, address";
    case 5: return "handler returning to: rip, cs, guest fs, rsp";
    case 6: return "xcs_fault64 reached FaultResumed: KPCR, eip, signal, address";
    case 7: return "FaultResumed done: result, eip, eflags, esp";
    case 8: return "self-test: about to fault (step number)";
    case 9: return "self-test: the step passed (step number)";
    case 10: return "not ours: on to the previous handler (signal, its handler, rip)";
    case 100: return "trail opened: OnFault, FaultResumed, the switch, xcs_fault64's entry";
    default: return "?";
    }
}

} // namespace

// The last run's trail, in order, to <trail>-previous.txt and its end to the log: how far the last
// fault got if the run died in the fault path. Done once for each trail file written since.
void ReportFaultTrail(const std::string& path)
{
    std::string text_path = path;
    if (text_path.size() > 4 && text_path.compare(text_path.size() - 4, 4, ".bin") == 0)
        text_path.resize(text_path.size() - 4);
    text_path += "-previous.txt";
    struct stat bin, txt;
    if (stat(path.c_str(), &bin) != 0 || (stat(text_path.c_str(), &txt) == 0 && txt.st_mtime > bin.st_mtime))
        return; // no trail, or already reported
    std::vector<platform::TrailRecord> records;
    if (FILE* f = std::fopen(path.c_str(), "rb"))
    {
        platform::TrailRecord r;
        while (std::fread(&r, sizeof(r), 1, f) == 1)
            if (r.magic == 0x52544258 && r.seq)
                records.push_back(r);
        std::fclose(f);
    }
    if (records.empty())
        return;
    std::sort(records.begin(), records.end(), [](const auto& a, const auto& b) { return a.seq < b.seq; });
    FILE* out = std::fopen(text_path.c_str(), "w");
    auto line = [&](const platform::TrailRecord& r) {
        char buf[320];
        std::snprintf(buf, sizeof(buf), "#%u stage %u (%s) thread %#x: %#llx %#llx %#llx %#llx", r.seq, r.stage,
                      StageName(r.stage), r.thread, (unsigned long long)r.a, (unsigned long long)r.b,
                      (unsigned long long)r.c, (unsigned long long)r.d);
        return std::string(buf);
    };
    if (out)
    {
        std::fprintf(out, "the fault trail of the run before this one: %zu records (the last %u kept), oldest first\n",
                     records.size(), platform::kTrailRecords);
        for (const auto& r : records)
            std::fprintf(out, "%s\n", line(r).c_str());
        std::fclose(out);
    }
    LogSync("cpu: the last run's fault trail: %zu records, the last ones (all in %s):", records.size(), text_path.c_str());
    for (size_t i = records.size() > 12 ? records.size() - 12 : 0; i < records.size(); i++)
        LogSync("  %s", line(records[i]).c_str());
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
    Write<u64>(g_switch + kSlotFault, reinterpret_cast<u64>(&FaultResumed));
    if (!g_fault_trail_file.empty())
    {
        std::string how;
        if (platform::OpenFaultTrail(g_fault_trail_file, how))
        {
            LogSync("cpu: fault trail in %s (%s)", g_fault_trail_file.c_str(), how.c_str());
            platform::Trail(100, reinterpret_cast<u64>(&OnFault), reinterpret_cast<u64>(&FaultResumed), g_switch,
                            SwitchEntry(xcs_fault64));
        }
        else
            LogSync("cpu: no fault trail: %s", how.c_str());
    }
    platform::SetKpcrFinder(FindKpcr);
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
                std::string(build).rfind(kBuildId, 0) == 0)
                died = line;
            std::fclose(f);
            while (!died.empty() && (died.back() == '\n' || died.back() == '\r'))
                died.pop_back();
        }
    if (!died.empty())
        LogSync("cpu self-test: the last run died in the step \"%s\"", died.c_str());
    u32 step_number = 0;
    auto step = [&](const char* name, const char* what) {
        step_number++;
        if (died == name)
        {
            LogSync("cpu self-test: step \"%s\" left out (%s stays off): the last run died in it", name, what);
            return false;
        }
        if (!g_selftest_file.empty())
            if (FILE* f = std::fopen(g_selftest_file.c_str(), "w"))
            {
                std::fprintf(f, "%s\n%s\n", name, kBuildId);
                std::fflush(f);
                fsync(fileno(f));
                std::fclose(f);
            }
        LogSync("cpu self-test: step \"%s\": %s (if the app dies here, the next start leaves it out)", name, what);
        platform::Trail(8, step_number);
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
        platform::Trail(9, step_number);
        if (const u64 rsp = platform::LastSignalStack())
            LogSync("cpu self-test: the kernel entered the fault handler with its stack at %#llx (%llu past 16-byte "
                    "alignment; a function expects 8: the entry realigns it)",
                    (unsigned long long)rsp, (unsigned long long)(rsp & 15));
    }
    if (step("fault log", "each of the first faults logged to the disk from the handler"))
    {
        g_fault_log = true;
        fault_test(t);
        Log("cpu self-test: faults logged from the handler");
        platform::Trail(9, step_number);
    }
    bool big_stacks = false;
    if (step("signal stack", "a bigger signal stack for each thread that runs guest code"))
    {
        big_stacks = true;
        platform::UseBigSignalStacks(true);
        fault_test(t);
        Log("cpu self-test: faults handled on the bigger signal stack");
        platform::Trail(9, step_number);
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
        platform::Trail(9, step_number);
    }
    // a step that killed a run stays left out in the runs after (a new build tries every step again)
    if (!g_selftest_file.empty())
    {
        if (died.empty())
            std::remove(g_selftest_file.c_str());
        else if (FILE* f = std::fopen(g_selftest_file.c_str(), "w"))
        {
            std::fprintf(f, "%s\n%s\n", died.c_str(), kBuildId);
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
    EnterGuest(t);
    const u64 result = reinterpret_cast<Enter>(SwitchEntry(xcs_enter64))(fn, esp, t->kpcr);
    LeaveGuest(t);
    return result;
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
    EnterGuest(t);
    const u64 result = reinterpret_cast<Enter>(SwitchEntry(xcs_enter64))(fn, esp, t->kpcr);
    LeaveGuest(t);
    Write<u64>(t->kpcr + kpcr::GuestEsp, saved_esp);
    return result;
}

void StartHostThread(GuestThread* t, std::function<void()> body)
{
    std::thread([t, body = std::move(body)] {
        platform::PrepareThread();
        tl_thread = t;
        t->host = platform::CurrentHostThread();
        body();
    }).detach();
}

void StopGuestThreads(GuestThread* self)
{
    if (!g_single_processor)
        return;
    g_stopper.store(self, std::memory_order_relaxed);
    g_stop_gen.fetch_add(1, std::memory_order_seq_cst); // odd: stopped
    const auto start = std::chrono::steady_clock::now();
    g_stops++;
    struct Waited
    {
        std::chrono::steady_clock::time_point from;
        ~Waited()
        {
            g_stop_wait_ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                      std::chrono::steady_clock::now() - from).count());
        }
    } waited{start};
    auto last_signal = start - std::chrono::seconds(1);
    for (;;)
    {
        const auto now = std::chrono::steady_clock::now();
        const bool signal = now - last_signal >= std::chrono::milliseconds(2);
        bool all = true;
        for (u32 i = 0, n = diag::ThreadCount(); i < n; i++)
        {
            GuestThread* t = diag::ThreadAt(i);
            const u64 host = t ? t->host.load() : 0;
            if (!t || t == self || !host)
                continue;
            if (t->running_guest.load(std::memory_order_seq_cst) && !t->held.load(std::memory_order_seq_cst))
            {
                all = false;
                if (signal)
                    platform::SendSample(host), g_stop_signals++;
            }
        }
        if (all)
            return;
        if (signal)
            last_signal = now;
        if (now - start > std::chrono::milliseconds(20))
        {
            // a thread that didn't stop in time runs on (a routine is never held up for long)
            if (g_stop_late.fetch_add(1) < 10)
                Log("cpu: a guest thread didn't stop for an interrupt routine within 20 ms: it ran alongside");
            return;
        }
        std::this_thread::yield();
    }
}

std::string SingleProcessorStats(double seconds)
{
    static u64 last_stops = 0, last_wait = 0, last_signals = 0, last_late = 0;
    const u64 stops = g_stops, wait = g_stop_wait_ns, signals = g_stop_signals, late = g_stop_late;
    char out[200];
    std::snprintf(out, sizeof(out),
                  "%.0f interrupt routines and DPCs a second held the other threads (%.0f signals a second; "
                  "%.1f us on average to stop them; %llu not in time)",
                  double(stops - last_stops) / seconds, double(signals - last_signals) / seconds,
                  stops > last_stops ? double(wait - last_wait) / 1000.0 / double(stops - last_stops) : 0.0,
                  (unsigned long long)(late - last_late));
    last_stops = stops, last_wait = wait, last_signals = signals, last_late = late;
    return out;
}

void ResumeGuestThreads()
{
    if (!g_single_processor)
        return;
    g_stopper.store(nullptr, std::memory_order_relaxed);
    {
        std::lock_guard lock(g_park_mutex);
        g_stop_gen.fetch_add(1, std::memory_order_seq_cst); // even: running
    }
    g_park_cv.notify_all();
}

} // namespace xb
