// The pixel stage: for now the interpolated diffuse colour (the combiners and textures come next).
#include <algorithm>

#include "draw.h"

namespace xb {

u32 ShadePixel(const PixelInputs& in, const Surface&, u32, u32)
{
    auto b = [](float f) { return u32(std::clamp(f, 0.f, 1.f) * 255.f + 0.5f); };
    const float* d = in.v[3];
    return b(d[3]) << 24 | b(d[0]) << 16 | b(d[1]) << 8 | b(d[2]);
}

} // namespace xb
