// The NV2A's parts: nv2a.cpp (registers, PFIFO, PGRAPH's protocol) and render.cpp (the Kelvin
// state and drawing into guest memory).
#pragma once

#include "core.h"

namespace xb {

// render.cpp: a method of the Kelvin (3D, class 0x97) object, and of the 2D classes. Called on the
// GPU thread with the NV2A's lock held.
void KelvinMethod(u32 method, u32 parameter);
void RenderOtherClass(u32 cls, u32 method, u32 parameter);

// nv2a.cpp: the guest address a DMA object (its instance in RAMIN) starts at; its limit.
u32 Nv2aDmaAddress(u32 instance, u32* limit = nullptr);

} // namespace xb
