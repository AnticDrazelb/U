// What the device emulations share (devices.cpp, nv2a.cpp, usb.cpp, audio.cpp).
#pragma once

#include <functional>

#include "core.h"

namespace xb {

// A memory-mapped device: reads and writes at offsets from its base.
struct Device
{
    u32 base, size;
    const char* name;
    std::function<u32(u32 offset, u32 size)> read;
    std::function<void(u32 offset, u32 size, u32 value)> write;
};
void AddDevice(const Device& device);

// Interrupt lines (the Xbox's IRQs): level-triggered. The NV2A is 3, USB 1 and 9, the APU 5, AC97 6.
void SetIrq(int line, bool level);

// "Physical" addresses as the GPU and the other bus masters see them: contiguous memory is
// 0-64 MiB, the RAM above it (core.h: kRamPhysical).
u32 PhysToGuest(u32 phys);

// nv2a.cpp
void InitNv2a();
void Nv2aVblank();        // from the interrupt thread, at 60 Hz
u32 Nv2aDisplayStart();   // PCRTC_START: the physical address shown

// usb.cpp, audio.cpp
void InitUsb();
void InitAudio();
void InitApu();

} // namespace xb
