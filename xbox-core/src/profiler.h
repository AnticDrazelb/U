// A sampling profiler, as New Eden's PS4 builds have one (port/ps4_sampler.cpp), on xbox-core's own
// sample signal so it is as safe in guest code as a fault is: a thread sends the signal to every
// registered thread about 200 times a second; the fault handler notes where each was (plain stores,
// nothing fs-relative): the guest's eip when it ran Xbox code, else the host's rip. Every 10 s the
// log gets each thread's CPU use and its busiest functions (named from symbols.txt) or, for Xbox
// code, its busiest addresses with the XBE section they are in; profile.txt has the whole run's.
#pragma once

#include <cstdint>
#include <string>

namespace xb {
struct GuestThread;
}

namespace xb::prof {

// Starts it (once): symbols, "<hex address> <hex size> <name>" lines relative to platform::CodeBase()
// (build.sh writes them; "" none); out, the whole run's profile file ("" none). XB_PROFILE=0 leaves
// it off; XB_PROFILE_HZ sets the rate (default 200).
void Start(const std::string& symbols, const std::string& out);
bool Active();

// The calling thread, named, from now until UnregisterThread (call it before the thread ends: a
// signal can't be sent to a thread that has gone). Returns its slot, -1 when full or off.
int RegisterThread(const char* name);
void UnregisterThread(int slot);

// From the fault handler on the sample signal: lock-free, nothing fs-relative.
void NoteGuest(int slot, uint32_t eip);
void NoteHost(uint64_t rip, uint64_t rsp);

} // namespace xb::prof

// For xemu's C code (its threads, through the shim's qemu_thread_create)
extern "C" int xb_prof_register_thread(const char* name);
extern "C" void xb_prof_unregister_thread(int slot);
