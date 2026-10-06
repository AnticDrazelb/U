// CemuPS4 GX2 test: wut's samples/cmake/gx2_triangle (devkitPro, zlib licence) with the
// precompiled shader embedded instead of read from the SD card. Draws an animated, colour-cycling
// triangle on the TV (blue background) and the GamePad (magenta background), which exercises
// Cemu's Latte shader translation (GLSL -> SPIR-V via glslang), pipeline creation in RADV and the
// render/scan-out path on the PS4. The frame rate shows in log.txt's PS4WSI lines.
#include <gfd.h>
#include <gx2/draw.h>
#include <gx2/mem.h>
#include <gx2/registers.h>
#include <gx2/shaders.h>
#include <gx2r/buffer.h>
#include <gx2r/draw.h>
#include <string.h>
#include <whb/gfx.h>
#include <whb/log.h>
#include <whb/log_cafe.h>
#include <whb/proc.h>

#include "pos_col_shader_gsh.h"

static const float sPositionData[] = {
    1.0f, -1.0f, 0.0f, 1.0f,
    0.0f,  1.0f, 0.0f, 1.0f,
   -1.0f, -1.0f, 1.0f, 1.0f,
};

static const float sColourData[] = {
   1.0f, 0.0f, 0.0f, 1.0f,
   0.0f, 1.0f, 0.0f, 1.0f,
   0.0f, 0.0f, 1.0f, 1.0f,
};

static void step(float *c)
{
   *c = *c >= 1.0f ? 0.0f : (*c + 0.01f);
}

int main(int argc, char **argv)
{
   GX2RBuffer positionBuffer = {0};
   GX2RBuffer colourBuffer   = {0};
   WHBGfxShaderGroup group   = {0};
   void *buffer;
   int result = 0;

   WHBLogCafeInit();
   WHBProcInit();
   WHBGfxInit();

   if (!WHBGfxLoadGFDShaderGroup(&group, 0, pos_col_shader_gsh)) {
      WHBLogPrintf("WHBGfxLoadGFDShaderGroup returned FALSE");
      result = -1;
      goto exit;
   }
   WHBGfxInitShaderAttribute(&group, "aPosition", 0, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
   WHBGfxInitShaderAttribute(&group, "aColour", 1, 0, GX2_ATTRIB_FORMAT_FLOAT_32_32_32_32);
   WHBGfxInitFetchShader(&group);

   positionBuffer.flags = GX2R_RESOURCE_BIND_VERTEX_BUFFER | GX2R_RESOURCE_USAGE_CPU_READ |
                          GX2R_RESOURCE_USAGE_CPU_WRITE | GX2R_RESOURCE_USAGE_GPU_READ;
   positionBuffer.elemSize  = 4 * 4;
   positionBuffer.elemCount = 3;
   GX2RCreateBuffer(&positionBuffer);
   buffer = GX2RLockBufferEx(&positionBuffer, 0);
   memcpy(buffer, sPositionData, sizeof(sPositionData));
   GX2RUnlockBufferEx(&positionBuffer, 0);

   colourBuffer.flags = positionBuffer.flags;
   colourBuffer.elemSize  = 4 * 4;
   colourBuffer.elemCount = 3;
   GX2RCreateBuffer(&colourBuffer);
   buffer = GX2RLockBufferEx(&colourBuffer, 0);
   memcpy(buffer, sColourData, sizeof(sColourData));
   GX2RUnlockBufferEx(&colourBuffer, 0);

   WHBLogPrintf("Begin rendering...");
   while (WHBProcIsRunning()) {
      float *colours = (float *)GX2RLockBufferEx(&colourBuffer, 0);
      colours[0] = 1.0f; step(&colours[1]); step(&colours[2]); colours[3] = 1.0f;
      step(&colours[4]); colours[5] = 1.0f; step(&colours[6]); colours[7] = 1.0f;
      step(&colours[8]); step(&colours[9]); colours[10] = 1.0f; colours[11] = 1.0f;
      GX2RUnlockBufferEx(&colourBuffer, 0);

      WHBGfxBeginRender();

      WHBGfxBeginRenderTV();
      WHBGfxClearColor(0.0f, 0.0f, 1.0f, 1.0f);
      GX2SetFetchShader(&group.fetchShader);
      GX2SetVertexShader(group.vertexShader);
      GX2SetPixelShader(group.pixelShader);
      GX2RSetAttributeBuffer(&positionBuffer, 0, positionBuffer.elemSize, 0);
      GX2RSetAttributeBuffer(&colourBuffer, 1, colourBuffer.elemSize, 0);
      GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 3, 0, 1);
      WHBGfxFinishRenderTV();

      WHBGfxBeginRenderDRC();
      WHBGfxClearColor(1.0f, 0.0f, 1.0f, 1.0f);
      GX2SetFetchShader(&group.fetchShader);
      GX2SetVertexShader(group.vertexShader);
      GX2SetPixelShader(group.pixelShader);
      GX2RSetAttributeBuffer(&positionBuffer, 0, positionBuffer.elemSize, 0);
      GX2RSetAttributeBuffer(&colourBuffer, 1, colourBuffer.elemSize, 0);
      GX2DrawEx(GX2_PRIMITIVE_MODE_TRIANGLES, 3, 0, 1);
      WHBGfxFinishRenderDRC();

      WHBGfxFinishRender();
   }

exit:
   WHBLogPrintf("Exiting...");
   GX2RDestroyBufferEx(&positionBuffer, 0);
   GX2RDestroyBufferEx(&colourBuffer, 0);
   WHBGfxShutdown();
   WHBProcShutdown();
   WHBLogCafeDeinit();
   return result;
}
