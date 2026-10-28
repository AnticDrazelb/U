// A GPU test for Eden for PS4: a Switch homebrew program that draws with the Switch's GPU instead of
// the CPU. It talks to the GPU directly through libnx's nvidia services (no deko3d, no shaders):
// a GPU channel bound to the Maxwell 3D engine, the window's framebuffers as its render target,
// and every rectangle a clear limited to a scissor rectangle. A program that draws this way shows
// a picture only where the GPU is emulated; with Eden's null renderer its frames stay empty.
//
// It draws a backdrop, bouncing squares and the word GPU with the 3D engine's clears, and sprites
// with the 2D engine: a 32x32 image drawn by the CPU, copied once by the DMA engine, then blitted
// and scaled onto the frame. It waits for each frame's GPU work through a fence the command stream
// signals, and quits on +.

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <switch.h>

// A fixed heap: without one, libnx asks for all free memory, which Eden then clears.
size_t __nx_heap_size = 64 * 1024 * 1024;

#define W 640
#define H 360
#define NUM_FBS 2
#define CMD_BUFS 3
#define CMD_WORDS 0x4000 // 64 KiB each

// Maxwell: classes, methods (32-bit register indices) and values
#define CLASS_3D 0xB197 // Maxwell B
#define CLASS_2D 0x902D // Fermi 2D
#define CLASS_DMA 0xB0B5 // Maxwell DMA copy
#define SUBCH_3D 0
#define SUBCH_2D 2
#define SUBCH_DMA 4
#define M2D_DST_FORMAT 0x080 // format, layout, block size, depth, layer
#define M2D_DST_WIDTH 0x086  // width, height, address (2)
#define M2D_SRC_FORMAT 0x08C // format, layout
#define M2D_SRC_PITCH 0x091  // pitch, width, height, address (2)
#define M2D_OPERATION 0x0AB
#define M2D_SAMPLE_MODE 0x223
#define M2D_BLIT 0x22C // dst x, y, width, height, du/dx (frac, int), dv/dy, src x, src y (32.32)
#define MDMA_LAUNCH 0x0C0
#define MDMA_OFFSET_IN 0x100 // in (2), out (2), pitch in, pitch out, line length, line count
#define SPRITE 32
#define M_FENCE_VALUE 0x1C // GPFIFO (puller) methods
#define M_FENCE_ACTION 0x1D
#define M_RENDER_TARGET 0x200 // 8 words each, 0x10 apart: address (2), width, height, format,
                              // tile mode, array mode, layer stride
#define M_CLEAR_COLOR 0x360   // 4 floats
#define M_SCISSOR 0x380       // enable, horizontal (min | max << 16), vertical
#define M_SCREEN_SCISSOR 0x3FD
#define M_CLEAR_FLAGS 0x43E
#define M_RT_CONTROL 0x487
#define M_CLEAR_BUFFERS 0x674
#define M_COLOR_MASK 0x680
#define RT_FORMAT_RGBA8 0xD5
#define CLEAR_FLAGS_SCISSOR (1u << 8)
#define CLEAR_RGBA (0xFu << 2)

static u32 header(u32 mode, u32 count, u32 subch, u32 method) {
    return method | (subch << 13) | (count << 16) | (mode << 29);
}
#define INCREASING 1
#define INLINE 4

typedef struct {
    u32* words;
    iova_t iova;
    u32 used;
    NvFence fence; // signalled when the GPU is done with it
    bool in_flight;
} CmdBuf;

static CmdBuf* g_cmd;
static void emit(u32 word) {
    if (g_cmd->used < CMD_WORDS)
        g_cmd->words[g_cmd->used++] = word;
}
static void method(u32 m, u32 count, const u32* args) {
    emit(header(INCREASING, count, SUBCH_3D, m));
    for (u32 i = 0; i < count; i++)
        emit(args[i]);
}
static void method1(u32 m, u32 arg) {
    method(m, 1, &arg);
}
static void method_on(u32 subch, u32 m, u32 count, const u32* args) {
    emit(header(INCREASING, count, subch, m));
    for (u32 i = 0; i < count; i++)
        emit(args[i]);
}

// A sprite blitted by the 2D engine from the pitch-linear image at 'src' onto the frame, scaled.
static void sprite(iova_t src, iova_t frame, int x, int y, int scale) {
    if (x < 0 || y < 0 || x + SPRITE * scale > W || y + SPRITE * scale > H)
        return;
    const u32 src_format[2] = {RT_FORMAT_RGBA8, 1 /* pitch */};
    method_on(SUBCH_2D, M2D_SRC_FORMAT, 2, src_format);
    const u32 src_surface[5] = {SPRITE * 4, SPRITE, SPRITE, (u32)(src >> 32), (u32)src};
    method_on(SUBCH_2D, M2D_SRC_PITCH, 5, src_surface);
    const u32 dst_format[5] = {RT_FORMAT_RGBA8, 0 /* block linear */, 4 << 4 /* 16 GOBs */, 1, 0};
    method_on(SUBCH_2D, M2D_DST_FORMAT, 5, dst_format);
    const u32 dst_surface[4] = {W, H, (u32)(frame >> 32), (u32)frame};
    method_on(SUBCH_2D, M2D_DST_WIDTH, 4, dst_surface);
    const u32 copy = 3; // SrcCopy
    method_on(SUBCH_2D, M2D_OPERATION, 1, &copy);
    const u32 corner = 1; // pixel corners, point sampling
    method_on(SUBCH_2D, M2D_SAMPLE_MODE, 1, &corner);
    // the step through the source for a screen pixel: 1/scale, in 32.32
    const u64 step = (1ull << 32) / (u64)scale;
    const u32 blit[12] = {(u32)x, (u32)y, SPRITE * (u32)scale, SPRITE * (u32)scale, (u32)step,
                          (u32)(step >> 32), (u32)step, (u32)(step >> 32), 0, 0, 0, 0};
    method_on(SUBCH_2D, M2D_BLIT, 12, blit); // the last word starts it
}

// The sprite: a chip with a glowing core, drawn by the CPU.
static void draw_sprite(u32* pixels) {
    for (int y = 0; y < SPRITE; y++)
        for (int x = 0; x < SPRITE; x++) {
            const float dx = x - 15.5f, dy = y - 15.5f, d = sqrtf(dx * dx + dy * dy);
            u32 r = 34, g = 36, b = 48;
            if (x < 2 || y < 2 || x >= SPRITE - 2 || y >= SPRITE - 2) {
                r = 190, g = 196, b = 220;
            } else if (d < 9) {
                const float t = d / 9;
                r = 255, g = (u32)(220 - 120 * t), b = (u32)(90 - 90 * t);
            } else if ((x % 6 == 3) && (y < 6 || y > 25)) {
                r = g = b = 150; // pins
            }
            pixels[y * SPRITE + x] = r | g << 8 | b << 16 | 0xFFu << 24;
        }
}
static u32 f32bits(float f) {
    u32 u;
    memcpy(&u, &f, 4);
    return u;
}

// A rectangle: a clear of render target 0, limited to the scissor.
static void rect(int x, int y, int w, int h, float r, float g, float b) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0)
        return;
    const u32 scissor[3] = {1, (u32)x | (u32)(x + w) << 16, (u32)y | (u32)(y + h) << 16};
    method(M_SCISSOR, 3, scissor);
    const u32 color[4] = {f32bits(r), f32bits(g), f32bits(b), f32bits(1.0f)};
    method(M_CLEAR_COLOR, 4, color);
    method1(M_CLEAR_BUFFERS, CLEAR_RGBA); // target 0, layer 0
}

// The word GPU in a 5x7 font, each dot a rectangle.
static void word_gpu(int x0, int y0, int dot, float r, float g, float b) {
    static const u8 glyphs[3][7] = {
        {14, 17, 16, 23, 17, 17, 15}, // G
        {30, 17, 17, 30, 16, 16, 16}, // P
        {17, 17, 17, 17, 17, 17, 14}, // U
    };
    for (int c = 0; c < 3; c++)
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 5; col++)
                if (glyphs[c][row] & (16 >> col))
                    rect(x0 + (c * 6 + col) * dot, y0 + row * dot, dot - 1, dot - 1, r, g, b);
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    consoleDebugInit(debugDevice_SVC); // stderr goes to svcOutputDebugString (the launcher's log)

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    // The window's buffers, block linear (libnx's framebuffer allocates and registers them), drawn
    // by the GPU instead of through framebufferBegin.
    NWindow* win = nwindowGetDefault();
    Framebuffer fb;
    Result rc = framebufferCreate(&fb, win, W, H, PIXEL_FORMAT_RGBA_8888, NUM_FBS);
    if (R_FAILED(rc)) {
        fprintf(stderr, "gputest: framebufferCreate: %#x\n", rc);
        return 1;
    }

    NvAddressSpace as;
    NvGpuChannel channel;
    if (R_FAILED(rc = nvGpuInit()) || R_FAILED(rc = nvAddressSpaceCreate(&as, 0x10000)) ||
        R_FAILED(rc = nvGpuChannelCreate(&channel, &as, NvChannelPriority_Medium))) {
        fprintf(stderr, "gputest: GPU setup failed: %#x\n", rc);
        return 1;
    }
    iova_t fb_iova = 0;
    if (R_FAILED(rc = nvAddressSpaceMap(&as, nvMapGetHandle(&fb.map), true, NvKind_Generic_16BX2, &fb_iova))) {
        fprintf(stderr, "gputest: mapping the framebuffers: %#x\n", rc);
        return 1;
    }

    // Command buffers in GPU-visible memory, used in turn.
    CmdBuf cmds[CMD_BUFS] = {0};
    NvMap cmd_maps[CMD_BUFS];
    for (int i = 0; i < CMD_BUFS; i++) {
        cmds[i].words = aligned_alloc(0x1000, CMD_WORDS * 4);
        if (!cmds[i].words || R_FAILED(rc = nvMapCreate(&cmd_maps[i], cmds[i].words, CMD_WORDS * 4, 0x1000,
                                                         NvKind_Pitch, true)) ||
            R_FAILED(rc = nvAddressSpaceMap(&as, nvMapGetHandle(&cmd_maps[i]), true, NvKind_Pitch, &cmds[i].iova))) {
            fprintf(stderr, "gputest: command buffer %d: %#x\n", i, rc);
            return 1;
        }
    }
    // The sprite (drawn by the CPU) and its copy (made by the DMA engine), in one GPU buffer.
    u32* sprite_mem = aligned_alloc(0x1000, 0x10000);
    NvMap sprite_map;
    iova_t sprite_iova = 0;
    if (!sprite_mem || R_FAILED(rc = nvMapCreate(&sprite_map, sprite_mem, 0x10000, 0x1000, NvKind_Pitch, true)) ||
        R_FAILED(rc = nvAddressSpaceMap(&as, nvMapGetHandle(&sprite_map), true, NvKind_Pitch, &sprite_iova))) {
        fprintf(stderr, "gputest: sprite buffer: %#x\n", rc);
        return 1;
    }
    memset(sprite_mem, 0, 0x10000);
    draw_sprite(sprite_mem);
    const iova_t sprite_copy = sprite_iova + 0x8000;

    fprintf(stderr, "gputest: GPU channel open, syncpoint %u, framebuffers at GPU address %#lx\n",
            nvGpuChannelGetSyncpointId(&channel), fb_iova);

    struct {
        float x, y, vx, vy, r, g, b;
    } squares[12];
    for (int i = 0; i < 12; i++) {
        squares[i].x = 40 + i * 45;
        squares[i].y = 60 + (i * 37) % 220;
        squares[i].vx = (i % 2 ? 2.0f : -2.5f) + i * 0.15f;
        squares[i].vy = (i % 3 ? 1.5f : -2.0f) + i * 0.1f;
        squares[i].r = 0.3f + 0.7f * (float)((i * 5) % 7) / 6.0f;
        squares[i].g = 0.3f + 0.7f * (float)((i * 3) % 5) / 4.0f;
        squares[i].b = 0.3f + 0.7f * (float)((i * 2) % 3) / 2.0f;
    }

    bool first = true;
    u64 frame = 0, fps_tick = armGetSystemTick(), fps_frames = 0;
    int fps = 0;
    int next_cmd = 0;
    while (appletMainLoop()) {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;

        // a buffer the display is done with
        s32 slot = -1;
        NvMultiFence display_fence;
        if (R_FAILED(rc = nwindowDequeueBuffer(win, &slot, &display_fence))) {
            fprintf(stderr, "gputest: nwindowDequeueBuffer: %#x\n", rc);
            break;
        }
        nvMultiFenceWait(&display_fence, -1);

        // a command buffer the GPU is done with
        g_cmd = &cmds[next_cmd];
        next_cmd = (next_cmd + 1) % CMD_BUFS;
        if (g_cmd->in_flight)
            nvFenceWait(&g_cmd->fence, -1);
        g_cmd->used = 0;

        if (first) {
            emit(header(INCREASING, 1, SUBCH_3D, 0)); // bind the engines to their subchannels
            emit(CLASS_3D);
            emit(header(INCREASING, 1, SUBCH_2D, 0));
            emit(CLASS_2D);
            emit(header(INCREASING, 1, SUBCH_DMA, 0));
            emit(CLASS_DMA);
            // the sprite's copy, by the DMA engine: one line of its bytes, pitch to pitch
            const u32 dma[8] = {(u32)(sprite_iova >> 32), (u32)sprite_iova, (u32)(sprite_copy >> 32),
                                (u32)sprite_copy, SPRITE * SPRITE * 4, SPRITE * SPRITE * 4,
                                SPRITE * SPRITE * 4, 1};
            method_on(SUBCH_DMA, MDMA_OFFSET_IN, 8, dma);
            const u32 launch = 2 /* non-pipelined */ | 1 << 2 /* flush */ | 1 << 7 | 1 << 8 /* pitch */;
            method_on(SUBCH_DMA, MDMA_LAUNCH, 1, &launch);
            method1(M_RT_CONTROL, 1 | (076543210 << 4)); // one render target, mapped to itself
            method1(M_COLOR_MASK, 0x1111);
            method1(M_CLEAR_FLAGS, CLEAR_FLAGS_SCISSOR);
            const u32 screen[2] = {(u32)W << 16, (u32)H << 16};
            method(M_SCREEN_SCISSOR, 2, screen);
            first = false;
        }
        const iova_t target = fb_iova + (iova_t)slot * fb.fb_size;
        const u32 rt[8] = {(u32)(target >> 32), (u32)target, W, H, RT_FORMAT_RGBA8,
                           4 << 4 /* block linear, 16 GOBs high */, 1 /* layer */, fb.fb_size >> 2};
        method(M_RENDER_TARGET, 8, rt);

        // backdrop: bands shading from blue to violet, moving
        for (int band = 0; band < 12; band++) {
            const float t = (float)band / 11.0f;
            const float pulse = 0.08f * sinf((float)frame * 0.05f + band * 0.5f);
            rect(0, band * (H / 12), W, H / 12 + 1, 0.10f + 0.25f * t + pulse, 0.08f + pulse, 0.30f + 0.2f * (1 - t));
        }
        for (int i = 0; i < 12; i++) {
            squares[i].x += squares[i].vx;
            squares[i].y += squares[i].vy;
            if (squares[i].x < 0 || squares[i].x > W - 32) squares[i].vx = -squares[i].vx;
            if (squares[i].y < 0 || squares[i].y > H - 32) squares[i].vy = -squares[i].vy;
            rect((int)squares[i].x, (int)squares[i].y, 32, 32, squares[i].r, squares[i].g, squares[i].b);
        }
        word_gpu(W / 2 - 3 * 6 * 12 / 2, H / 2 - 7 * 12 / 2, 12, 1.0f, 1.0f, 1.0f);
        // sprites from the DMA engine's copy, circling at three sizes
        for (int i = 0; i < 3; i++) {
            const float angle = (float)frame * 0.03f + i * 2.094f;
            const int scale = i + 1;
            const int cx = W / 2 + (int)(200 * cosf(angle)), cy = H / 2 + (int)(110 * sinf(angle));
            sprite(sprite_copy, target, cx - SPRITE * scale / 2, cy - SPRITE * scale / 2, scale);
        }
        // a bar showing the frame rate: 6 pixels per frame a second
        rect(10, H - 16, fps * 6, 6, 0.4f, 1.0f, 0.5f);

        // the GPU signals the channel's syncpoint when it gets here
        emit(header(INCREASING, 2, SUBCH_3D, M_FENCE_VALUE));
        emit(0);
        emit((nvGpuChannelGetSyncpointId(&channel) << 8) | 1); // increment
        nvGpuChannelIncrFence(&channel);
        if (R_FAILED(rc = nvGpuChannelAppendEntry(&channel, g_cmd->iova, g_cmd->used, 0, 0)) ||
            R_FAILED(rc = nvGpuChannelKickoff(&channel))) {
            fprintf(stderr, "gputest: submitting: %#x\n", rc);
            break;
        }
        nvGpuChannelGetFence(&channel, &g_cmd->fence);
        g_cmd->in_flight = true;

        // shown once the GPU has drawn it
        NvMultiFence done;
        nvMultiFenceCreate(&done, &g_cmd->fence);
        if (R_FAILED(rc = nwindowQueueBuffer(win, slot, &done))) {
            fprintf(stderr, "gputest: nwindowQueueBuffer: %#x\n", rc);
            break;
        }

        frame++;
        fps_frames++;
        const u64 now = armGetSystemTick();
        if (now - fps_tick >= armGetSystemTickFreq()) {
            fps = (int)fps_frames;
            fps_frames = 0;
            fps_tick = now;
            if (frame < 200 || frame % 600 < 60)
                fprintf(stderr, "gputest: %d fps, %u command words a frame\n", fps, g_cmd->used);
        }
    }

    fprintf(stderr, "gputest: quitting\n");
    for (int i = 0; i < CMD_BUFS; i++)
        if (cmds[i].in_flight)
            nvFenceWait(&cmds[i].fence, -1);
    fprintf(stderr, "gputest: GPU idle\n");
    nvGpuChannelClose(&channel);
    nvAddressSpaceClose(&as);
    fprintf(stderr, "gputest: channel closed\n");
    framebufferClose(&fb);
    fprintf(stderr, "gputest: quit after %llu frames\n", (unsigned long long)frame);
    return 0;
}
