// A GPU test for Eden for PS4: a Switch homebrew program that draws with the Switch's GPU instead of
// the CPU. It talks to the GPU directly through libnx's nvidia services (no deko3d, no shaders):
// a GPU channel bound to the Maxwell 3D engine, the window's framebuffers as its render target,
// and every rectangle a clear limited to a scissor rectangle. A program that draws this way shows
// a picture only where the GPU is emulated; with Eden's null renderer its frames stay empty.
//
// It draws a backdrop, bouncing squares and the word GPU with the 3D engine's clears, and sprites
// with the 2D engine: a 32x32 image drawn by the CPU, copied once by the DMA engine, then blitted
// and scaled onto the frame. And a triangle drawn with shaders: a vertex and a fragment program in
// Maxwell machine code, assembled by hand below (there's no shader compiler here). And textured
// shapes: a square with the sprite on it, turning, and a backdrop of tiles blended over the bands,
// scrolling: a tile drawn by the CPU, copied by the DMA engine into a block-linear image (how
// games upload textures) and filtered bilinearly. It waits for each frame's GPU work through a
// fence the command stream signals, and quits on +. And a depth test: two bars across each other
// in depth, each in front on its own half. And a square coloured from a storage buffer, lines and a
// point, and a stencil test: a triangle drawn only into the stencil, then a square drawn where it
// is (the storage square is drawn with its indices in the command stream). From frame 30 the CPU
// paints the sprite's middle blue, which the turning square shows. The
// bouncing squares are cleared by a GPU macro (MME, as deko3d and Mesa draw through macros).
// And a floor in perspective (bottom middle) with a mipmapped texture: each level a colour of its
// own, sampled where the floor's derivatives choose, the shader branching per pixel for stripes.
// A turns a stress test on and off: 400 more sprites, each a draw of its own, as 2D games draw
// them (up and down change how many, by 100).

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
#define MDMA_DST_PARAMS 0x1C3 // block size, width, height, depth, layer, origin
#define SPRITE 32
#define M_VIEWPORT_TRANSFORM 0x280 // scale x, y, z, translate x, y, z, swizzle, snap
#define M_VIEWPORT 0x300           // x | width << 16, y | height << 16, near, far
#define M_VERTEX_FIRST 0x35D       // first, count
#define M_VERTEX_ATTRIB 0x458      // one word per attribute
#define M_VIEWPORT_TRANSFORM_ENABLE 0x64B
#define M_PROGRAM_REGION 0x582     // address (2)
#define M_DRAW_END 0x585
#define M_DRAW_BEGIN 0x586
#define M_VERTEX_STREAM 0x700      // config (stride | enable << 12), address (2), frequency
#define M_VERTEX_STREAM_LIMIT 0x7C0 // last address (2)
#define M_PIPELINE 0x800           // 0x10 apart: config (enable | type << 4), offset, -, registers
#define M_SAMPLER_BINDING 0x48D    // 1: a texture's sampler is the one with its index
#define M_TEX_SAMPLER_POOL 0x557   // address (2), last index
#define M_TEX_HEADER_POOL 0x55D    // address (2), last index
#define M_CONST_BUFFER 0x8E0       // size, address (2): the buffer the next bind binds
#define M_BIND_FRAGMENT_CBUF 0x924 // valid | slot << 4, for the fragment stage (bind group 4)
#define M_TEXTURE_CBUF_SLOT 0x982  // the constant buffer holding texture handles
#define M_BLEND 0x4CF              // separate alpha, colour op, source, dest, alpha op, source, -, dest
#define M_BLEND_ENABLE 0x4D8       // one word per render target
#define BLEND_ADD 0x8006
#define BLEND_SRC_ALPHA 0x4302
#define BLEND_ONE_MINUS_SRC_ALPHA 0x4303
#define M_ZETA 0x3F8               // address (2), format, tile mode, array pitch
#define M_ZETA_SIZE 0x48A          // width, height, depth | dimension control << 16
#define M_ZETA_ENABLE 0x54E
#define M_DEPTH_TEST_ENABLE 0x4B3
#define M_DEPTH_WRITE_ENABLE 0x4BA
#define M_DEPTH_FUNC 0x4C3
#define M_CLEAR_DEPTH 0x364
#define ZETA_Z24S8 0x14
#define M_STENCIL_ENABLE 0x4E0
#define M_STENCIL_FRONT_OP 0x4E1   // fail, depth fail, pass, function
#define M_STENCIL_FRONT_REF 0x4E5  // reference, function mask, write mask
#define M_CLEAR_STENCIL 0x368
#define M_STENCIL_TWO_SIDE 0x565   // on by default: back faces with their own stencil state
#define STENCIL_KEEP 0x1E00
#define STENCIL_REPLACE 0x1E01
#define FUNC_EQUAL 0x202
#define FUNC_ALWAYS 0x207
#define CLEAR_S 2u
#define DEPTH_LESS_EQUAL 0x203
#define CLEAR_Z 1u
#define STENCIL_OFFSET(i) (0x3800 + (i) * 0x100) // the stencil triangle (3 vertices), then its square (4)
#define M_LINE_WIDTH 0x4ED         // aliased lines
#define M_POINT_SIZE 0x546
#define TOPOLOGY_POINTS 0
#define M_DRAW_INLINE_INDEX 0x57A  // a draw's indices in the command stream, one a word
#define TOPOLOGY_LINE_STRIP 3
#define TOPOLOGY_TRIANGLES 4
#define TOPOLOGY_TRIANGLE_STRIP 5
#define ATTR_RGBA32F (0x01u << 21 | 7u << 27) // R32G32B32A32, float, buffer 0 (offset << 7)

// Maxwell instructions (64-bit), with the guard predicate PT (always) in bits 16..19
#define PT (7ull << 16)
#define RZ 0xFFull
static u64 ald128(u32 dest, u32 attribute) { // ALD.128 Rdest, a[attribute]
    return 0x1DFBull << 51 | 3ull << 47 | RZ << 39 | (u64)attribute << 20 | PT | RZ << 8 | dest;
}
static u64 ast128(u32 attribute, u32 src) { // AST.128 a[attribute], Rsrc
    return 0x1DFEull << 51 | 3ull << 47 | RZ << 39 | (u64)attribute << 20 | PT | RZ << 8 | src;
}
static u64 mov32i(u32 dest, float value) { // MOV32I Rdest, value
    u32 bits;
    memcpy(&bits, &value, 4);
    return 0x010ull << 52 | (u64)bits << 20 | PT | 0xFull << 12 | dest;
}
static u64 ipa(u32 dest, u32 attribute) { // IPA.PASS Rdest, a[attribute] (an interpolated input)
    return 0xE0ull << 56 | (u64)(attribute / 4) << 30 | RZ << 20 | PT | RZ << 8 | dest;
}
// TEX Rdest..Rdest+3, Rcoord (s, t), a 2D texture whose handle is at 'handle_offset' in the bound
// texture constant buffer
static u64 tex2d(u32 dest, u32 coord, u32 handle_offset) {
    return 0x18ull << 59 | 7ull << 51 /* no sparse predicate */ | (u64)(handle_offset / 4) << 36 |
           0xFull << 31 /* rgba */ | 2ull << 28 /* 2D */ | RZ << 20 | PT | (u64)coord << 8 | dest;
}
// MOV Rdest, c[bank][offset]
static u64 mov_cbuf(u32 dest, u32 bank, u32 offset) {
    return 0x4C98ull << 48 | 0xFull << 39 /* all bytes */ | (u64)bank << 34 | (u64)(offset / 4) << 20 | PT | dest;
}
// LDG.E.128 Rdest..Rdest+3, [Raddr..Raddr+1]: 16 bytes from a 64-bit GPU address
static u64 ldg128(u32 dest, u32 addr) {
    return 0xEED0ull << 48 | 6ull << 48 /* 128 bits */ | 1ull << 45 /* 64-bit address */ | PT | (u64)addr << 8 | dest;
}
// IPA.MUL Rdest, a[attribute], Rmultiplier (a perspective varying: interpolated over w, times w)
static u64 ipa_mul(u32 dest, u32 attribute, u32 multiplier) {
    return 0xE0ull << 56 | 1ull << 54 | (u64)(attribute / 4) << 30 | (u64)multiplier << 20 | PT | RZ << 8 | dest;
}
// MUFU Rdest, Rsrc: 1 sine, 4 reciprocal
static u64 mufu(u32 dest, u32 src, u32 operation) {
    return 0x5080ull << 48 | (u64)operation << 20 | PT | (u64)src << 8 | dest;
}
// FMUL32I Rdest, Rsrc, value
static u64 fmul32i(u32 dest, u32 src, float value) {
    u32 bits;
    memcpy(&bits, &value, 4);
    return 0x1Eull << 56 | (u64)bits << 20 | PT | (u64)src << 8 | dest;
}
// FSETP.GT.AND Ppred, PT, Rsrc, value, PT (the value's top 19 bits)
static u64 fsetp_gt(u32 pred, u32 src, float value) {
    u32 bits;
    memcpy(&bits, &value, 4);
    return 0x36Bull << 52 | 4ull << 48 /* GT */ | (u64)(bits >> 31) << 56 | 7ull << 39 /* and PT */ |
           (u64)((bits >> 12) & 0x7FFFF) << 20 | PT | (u64)src << 8 | (u64)pred << 3 | 7 /* PT */;
}
// @Ppred BRA: a jump 'offset' bytes from the next instruction, taken where the predicate is true
static u64 bra_if(u32 pred, s32 offset) {
    return 0xE24ull << 52 | ((u64)(u32)offset & 0xFFFFFF) << 20 | (u64)pred << 16 | 0xF /* always */;
}
static u64 exit_(void) { // EXIT (condition: true)
    return 0xE30ull << 52 | PT | 0xF;
}
static u64 nop(void) {
    return 0xA16ull << 51 | PT;
}
#define SCHED 0x001F8000FC0007E0ull // a scheduling word (before every three instructions)

// The shader programs, in a GPU buffer: each a 0x50-byte header (SPH) and its code, placed so
// that the code starts on 32 bytes (the scheduling words sit at multiples of 32).
#define VS_OFFSET 0x10
#define FS_OFFSET 0x110
#define FS_TEX_OFFSET 0x210
#define FS_TILES_OFFSET 0x310
#define FS_STORAGE_OFFSET 0x410
#define FS_FLOOR_OFFSET 0x510
#define STORAGE_OFFSET 0x3000 // a storage buffer: a colour
#define TIC_POOL 0x2000 // texture headers (32 bytes each)
#define TSC_POOL 0x2100 // samplers (32 bytes each)
#define FS_CBUF 0x2200  // the fragment stage's constant buffer 0: texture handles
static void write_programs(u8* heap) {
    u32* vs_sph = (u32*)(heap + VS_OFFSET);
    vs_sph[0] = 1 /* vertex, geometry etc. */ | 3 << 5 /* version */ | 1 << 10 /* vertex */;
    ((u8*)vs_sph)[24] = 0xFF; // reads attributes 0 and 1 (xyzw)
    ((u8*)vs_sph)[53] = 0xF0; // writes position (xyzw)
    ((u8*)vs_sph)[54] = 0x0F; // and varying 0 (xyzw)
    u64* vs = (u64*)(heap + VS_OFFSET + 0x50);
    vs[0] = SCHED;
    vs[1] = ald128(0, 0x80); // R0..R3 = vertex attribute 0 (position)
    vs[2] = ald128(4, 0x90); // R4..R7 = vertex attribute 1 (colour)
    vs[3] = ast128(0x70, 0); // position = R0..R3
    vs[4] = SCHED;
    vs[5] = ast128(0x80, 4); // varying 0 = R4..R7
    vs[6] = exit_();
    vs[7] = nop();

    u32* fs_sph = (u32*)(heap + FS_OFFSET);
    fs_sph[0] = 2 /* pixel */ | 3 << 5 | 5 << 10 /* pixel */;
    ((u8*)fs_sph)[24] = 0xFF; // varying 0, xyzw interpolated linearly in screen space
    fs_sph[18] = 0xF;         // render target 0 gets R0..R3
    u64* fs = (u64*)(heap + FS_OFFSET + 0x50);
    fs[0] = SCHED;
    fs[1] = ipa(0, 0x80); // R0..R3 = varying 0
    fs[2] = ipa(1, 0x84);
    fs[3] = ipa(2, 0x88);
    fs[4] = SCHED;
    fs[5] = ipa(3, 0x8C);
    fs[6] = exit_();
    fs[7] = nop();
    (void)mov32i;

    // the textured one: varying 0's x, y are texture coordinates
    u32* ft_sph = (u32*)(heap + FS_TEX_OFFSET);
    ft_sph[0] = 2 | 3 << 5 | 5 << 10;
    ((u8*)ft_sph)[24] = 0x0F; // varying 0, xy, linear in screen space
    ft_sph[18] = 0xF;
    u64* ft = (u64*)(heap + FS_TEX_OFFSET + 0x50);
    ft[0] = SCHED;
    ft[1] = ipa(4, 0x80);
    ft[2] = ipa(5, 0x84);
    ft[3] = tex2d(0, 4, 0);
    ft[4] = SCHED;
    ft[5] = exit_();
    ft[6] = nop();
    ft[7] = nop();

    // the tiles: the same, with the texture whose handle is the constant buffer's second word
    u32* tt_sph = (u32*)(heap + FS_TILES_OFFSET);
    memcpy(tt_sph, ft_sph, 0x50);
    u64* tt = (u64*)(heap + FS_TILES_OFFSET + 0x50);
    memcpy(tt, ft, 8 * sizeof(u64));
    tt[3] = tex2d(0, 4, 4);

    // the storage buffer's: its colour loaded from the buffer whose address (as NVN puts it) is
    // at 0x110 in the constant buffer
    u32* fb_sph = (u32*)(heap + FS_STORAGE_OFFSET);
    fb_sph[0] = 2 | 3 << 5 | 5 << 10;
    fb_sph[18] = 0xF;
    u64* fbuf = (u64*)(heap + FS_STORAGE_OFFSET + 0x50);
    fbuf[0] = SCHED;
    fbuf[1] = mov_cbuf(2, 0, 0x110);
    fbuf[2] = mov_cbuf(3, 0, 0x114);
    fbuf[3] = ldg128(0, 2);
    fbuf[4] = SCHED;
    fbuf[5] = exit_();
    fbuf[6] = nop();
    fbuf[7] = nop();

    // the floor's: a perspective-correct varying (u, v), the mipmapped texture sampled at the level
    // its derivatives choose, and stripes darkened where sin(pi v) isn't positive (a branch)
    u32* ff_sph = (u32*)(heap + FS_FLOOR_OFFSET);
    ff_sph[0] = 2 | 3 << 5 | 5 << 10;
    ((u8*)ff_sph)[24] = 0x0A; // varying 0, xy, perspective
    ff_sph[18] = 0xF;
    u64* ff = (u64*)(heap + FS_FLOOR_OFFSET + 0x50);
    ff[0] = SCHED;
    ff[1] = ipa(7, 0x7C);        // R7 = 1/w
    ff[2] = mufu(7, 7, 4);       // R7 = w
    ff[3] = ipa_mul(4, 0x80, 7); // R4 = u
    ff[4] = SCHED;
    ff[5] = ipa_mul(5, 0x84, 7); // R5 = v
    ff[6] = tex2d(0, 4, 8);      // R0..R3 = the floor's texture
    ff[7] = fmul32i(6, 5, 3.14159265f);
    ff[8] = SCHED;
    ff[9] = mufu(6, 6, 1);       // R6 = sin(pi v)
    ff[10] = fsetp_gt(0, 6, 0.0f);
    ff[11] = bra_if(0, 0x20);    // positive: on to the exit
    ff[12] = SCHED;
    ff[13] = fmul32i(0, 0, 0.6f);
    ff[14] = fmul32i(1, 1, 0.6f);
    ff[15] = fmul32i(2, 2, 0.6f);
    ff[16] = SCHED;
    ff[17] = exit_();
    ff[18] = nop();
    ff[19] = nop();
}

// A texture header (TIC) for a 2D, pitch-linear RGBA8 texture, and a point-sampling, clamping
// sampler (TSC).
static void write_texture(u8* heap, iova_t image, u32 width, u32 height, u32 pitch) {
    u32* tic = (u32*)(heap + TIC_POOL);
    tic[0] = 0x08 /* A8B8G8R8 */ | 2 << 7 | 2 << 10 | 2 << 13 | 2 << 16 /* unorm */ |
             2 << 19 | 3 << 22 | 4 << 25 | 5 << 28 /* r, g, b, a */;
    tic[1] = (u32)image;
    tic[2] = (u32)(image >> 32) | 2 << 21 /* pitch */;
    tic[3] = pitch >> 5;
    tic[4] = (width - 1) | 1 << 23 /* 2D */;
    tic[5] = (height - 1) | 1u << 31 /* normalized coordinates */;
    u32* tsc = (u32*)(heap + TSC_POOL);
    tsc[0] = 2 | 2 << 3 | 2 << 6; // clamp to edge
    tsc[1] = 1 | 1 << 4 | 1 << 6; // nearest, nearest, no mipmaps
    *(u32*)(heap + FS_CBUF) = 0;  // handle: texture header 0 (its sampler with it)
}

// The tile: 64x64, drawn by the CPU (pitch linear), then copied by the DMA engine into a
// block-linear image (one block: 8 GOBs high), sampled with header and sampler 1: bilinear,
// repeating.
#define TILE 64
static void draw_tile(u32* pixels) {
    for (int y = 0; y < TILE; y++)
        for (int x = 0; x < TILE; x++) {
            const int dx = abs(2 * x + 1 - TILE), dy = abs(2 * y + 1 - TILE);
            const int ring = (dx + dy) / 8; // diamonds
            const u32 r = (u32)(40 + (ring % 4) * 50), g = (u32)(120 + (x * 2) % 90), b = (u32)(220 - ring * 9);
            const u32 a = (ring % 2) ? 110 : 60;
            pixels[y * TILE + x] = r | g << 8 | b << 16 | a << 24;
        }
}
static void write_tile_texture(u8* heap, iova_t image) {
    u32* tic = (u32*)(heap + TIC_POOL + 32);
    tic[0] = 0x08 | 2 << 7 | 2 << 10 | 2 << 13 | 2 << 16 | 2 << 19 | 3 << 22 | 4 << 25 | 5 << 28;
    tic[1] = (u32)image;
    tic[2] = (u32)(image >> 32) | 3 << 21 /* block linear */;
    tic[3] = 3 << 3; // blocks 8 GOBs high
    tic[4] = (TILE - 1) | 1 << 23;
    tic[5] = (TILE - 1) | 1u << 31;
    u32* tsc = (u32*)(heap + TSC_POOL + 32);
    tsc[0] = 0;                   // repeat
    tsc[1] = 2 | 2 << 4 | 1 << 6; // linear, linear, no mipmaps
    *(u32*)(heap + FS_CBUF + 4) = 1;
}
// The floor's texture: 64x64 and its 6 smaller levels, block linear, written by the CPU. Level 0
// a grey checkerboard, the rest a colour each (red, orange, green, cyan, blue, purple), so where
// each is sampled shows; header and sampler 2: trilinear, repeating.
#define MIP_LEVELS 7
static const u32 mip_colours[MIP_LEVELS] = {0, 0xFF3C46E6, 0xFF28A0F0, 0xFF3CDC96, 0xFFDCC83C,
                                            0xFFF05A46, 0xFFDC46AA};
// a block-linear image's byte for byte column xb, row y (GOBs of 64 bytes by 8 rows, blocks of
// 8 << block_height rows)
static u32 block_linear(u32 xb, u32 y, u32 width_bytes, u32 block_height) {
    const u32 gobs_x = (width_bytes + 63) / 64, block_rows = 8u << block_height;
    const u32 block_bytes = 512u << block_height, x = xb % 64, r = y % 8;
    return (y / block_rows) * gobs_x * block_bytes + (xb / 64) * block_bytes + (y % block_rows / 8) * 512 +
           (x / 32) * 256 + (r / 2) * 64 + ((x % 32) / 16) * 32 + (r % 2) * 16 + x % 16;
}
static void write_mip_texture(u8* heap, u8* image_mem, iova_t image) {
    u32 offset = 0;
    for (u32 level = 0; level < MIP_LEVELS; level++) {
        const u32 size = 64 >> level, gobs_y = (size + 7) / 8;
        u32 block_height = 3; // as the GPU shrinks a level's blocks to fit it
        while (block_height > 0 && gobs_y <= (1u << block_height) / 2)
            block_height--;
        const u32 bytes = ((size * 4 + 63) / 64) * ((gobs_y + (1u << block_height) - 1) >> block_height) *
                          (512u << block_height);
        for (u32 y = 0; y < size; y++)
            for (u32 x = 0; x < size; x++) {
                const u32 grey = ((x / 8 + y / 8) % 2) ? 0xFF3C3C3C : 0xFFEBEBEB;
                const u32 texel = level == 0 ? grey : mip_colours[level];
                memcpy(image_mem + offset + block_linear(x * 4, y, size * 4, block_height), &texel, 4);
            }
        offset += bytes;
    }
    armDCacheFlush(image_mem, offset);
    u32* tic = (u32*)(heap + TIC_POOL + 64);
    tic[0] = 0x08 | 2 << 7 | 2 << 10 | 2 << 13 | 2 << 16 | 2 << 19 | 3 << 22 | 4 << 25 | 5 << 28;
    tic[1] = (u32)image;
    tic[2] = (u32)(image >> 32) | 3 << 21 /* block linear */;
    tic[3] = 3 << 3 /* blocks 8 GOBs high */ | (MIP_LEVELS - 1) << 28 /* levels */;
    tic[4] = 63 | 1 << 23;
    tic[5] = 63 | 1u << 31;
    tic[7] = (MIP_LEVELS - 1) << 4; // the view's levels: all
    u32* tsc = (u32*)(heap + TSC_POOL + 64);
    tsc[0] = 0;                               // repeat
    tsc[1] = 2 | 2 << 4 | 3 << 6;             // linear, linear, linear between levels
    tsc[2] = ((MIP_LEVELS - 1) * 256) << 12;  // levels of detail 0 to 6
    *(u32*)(heap + FS_CBUF + 8) = 2;
}
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
#define NONINCREASING 3 // every word to the same method
#define INCREASE_ONCE 5 // the first word to the method, the rest to the next (a macro's parameters)
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
// A GPU macro (Maxwell's MME), uploaded once: it takes a colour and clear flags, sets the clear
// colour from the first four and clears with the fifth. Each instruction an add of an immediate,
// its result sent to a method, or the method set, or the next parameter fetched.
#define M_MME_INSTRUCTION_PTR 0x45 // where uploaded instructions go, then the instructions
#define M_MME_START_PTR 0x47       // a macro's number, then its first instruction
#define M_MACRO_CALL 0xE00         // macro n: its first parameter at 0xE00 + 2n, the rest at + 1
enum { MME_MOVE = 1, MME_MOVE_SET_METHOD = 2, MME_FETCH_SEND = 3, MME_MOVE_SEND = 4 };
static u32 mme_addi(u32 result, u32 dst, u32 src, s32 imm, int exit) {
    return 1 /* add immediate */ | result << 4 | (exit ? 1u << 7 : 0) | dst << 8 | src << 11 | ((u32)imm & 0x3FFFF) << 14;
}
static void upload_macro(void) {
    u32 code[8];
    code[0] = mme_addi(MME_MOVE_SET_METHOD, 0, 0, M_CLEAR_COLOR | 1 << 12, 0); // the clear colour, ...
    for (int i = 1; i <= 4; i++)
        code[i] = mme_addi(MME_FETCH_SEND, 1, 1, 0, 0); // r1 (the first parameter) sent, the next fetched
    code[5] = mme_addi(MME_MOVE_SET_METHOD, 0, 0, M_CLEAR_BUFFERS, 0);
    code[6] = mme_addi(MME_MOVE_SEND, 0, 1, 0, 1); // the flags sent: the clear; and the end
    code[7] = mme_addi(MME_MOVE, 0, 0, 0, 0);      // the end's delay slot: nothing
    method1(M_MME_INSTRUCTION_PTR, 0);
    emit(header(NONINCREASING, 8, SUBCH_3D, M_MME_INSTRUCTION_PTR + 1)); // all to one method
    for (int i = 0; i < 8; i++)
        emit(code[i]);
    const u32 bind[2] = {0 /* macro 0 */, 0 /* its first instruction */};
    method(M_MME_START_PTR, 2, bind);
}
// A rectangle cleared by the macro
static void rect_by_macro(int x, int y, int w, int h, float r, float g, float b) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    if (w <= 0 || h <= 0)
        return;
    const u32 scissor[3] = {1, (u32)x | (u32)(x + w) << 16, (u32)y | (u32)(y + h) << 16};
    method(M_SCISSOR, 3, scissor);
    // the parameters in one go: the macro runs when the call's last one has come
    const u32 parameters[5] = {f32bits(r), f32bits(g), f32bits(b), f32bits(1.0f), CLEAR_RGBA};
    emit(header(INCREASE_ONCE, 5, SUBCH_3D, M_MACRO_CALL));
    for (int i = 0; i < 5; i++)
        emit(parameters[i]);
}

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
    // the tile, drawn at 0x2000, copied by the DMA engine to 0xC000 (block linear)
    draw_tile(sprite_mem + 0x2000 / 4);
    const iova_t tile_image = sprite_iova + 0xC000;

    // The shader programs and, for each command buffer, the triangle's vertices.
    u8* heap = aligned_alloc(0x1000, 0x10000);
    NvMap heap_map;
    iova_t heap_iova = 0;
    if (!heap || R_FAILED(rc = nvMapCreate(&heap_map, heap, 0x10000, 0x1000, NvKind_Pitch, true)) ||
        R_FAILED(rc = nvAddressSpaceMap(&as, nvMapGetHandle(&heap_map), true, NvKind_Pitch, &heap_iova))) {
        fprintf(stderr, "gputest: shader buffer: %#x\n", rc);
        return 1;
    }
    memset(heap, 0, 0x10000);
    // The depth buffer: 32-bit float, block linear like the framebuffers
    void* depth_mem = aligned_alloc(0x1000, 0x100000);
    NvMap depth_map;
    iova_t depth_iova = 0;
    if (!depth_mem || R_FAILED(rc = nvMapCreate(&depth_map, depth_mem, 0x100000, 0x1000, NvKind_Pitch, true)) ||
        R_FAILED(rc = nvAddressSpaceMap(&as, nvMapGetHandle(&depth_map), true, NvKind_Pitch, &depth_iova))) {
        fprintf(stderr, "gputest: depth buffer: %#x\n", rc);
        return 1;
    }
    write_programs(heap);
    write_texture(heap, sprite_iova, SPRITE, SPRITE, SPRITE * 4);
    write_tile_texture(heap, tile_image);
    u8* mip_mem = aligned_alloc(0x1000, 0x8000);
    NvMap mip_map;
    iova_t mip_iova = 0;
    if (!mip_mem || R_FAILED(rc = nvMapCreate(&mip_map, mip_mem, 0x8000, 0x1000, NvKind_Pitch, true)) ||
        R_FAILED(rc = nvAddressSpaceMap(&as, nvMapGetHandle(&mip_map), true, NvKind_Pitch, &mip_iova))) {
        fprintf(stderr, "gputest: floor texture: %#x\n", rc);
        return 1;
    }
    memset(mip_mem, 0, 0x8000);
    write_mip_texture(heap, mip_mem, mip_iova);
    // The stress test's sprites' vertices: 4 for each, for each command buffer
#define MAX_SPRITES 2000
#define SPRITE_VERTEX_BYTES ((CMD_BUFS * MAX_SPRITES * 128 + 0xFFF) & ~0xFFF) // whole pages
    float* sprite_vertices = aligned_alloc(0x1000, SPRITE_VERTEX_BYTES);
    NvMap sprite_vertex_map;
    iova_t sprite_vertices_iova = 0;
    if (!sprite_vertices ||
        R_FAILED(rc = nvMapCreate(&sprite_vertex_map, sprite_vertices, SPRITE_VERTEX_BYTES, 0x1000, NvKind_Pitch, true)) ||
        R_FAILED(rc = nvAddressSpaceMap(&as, nvMapGetHandle(&sprite_vertex_map), true, NvKind_Pitch, &sprite_vertices_iova))) {
        fprintf(stderr, "gputest: sprite vertices: %#x\n", rc);
        return 1;
    }
    bool stress = false;
    int stress_sprites = 400;
    {   // the storage buffer (a cyan), its address and size in the constant buffer
        const float colour[4] = {0.1f, 0.9f, 0.95f, 1.0f};
        memcpy(heap + STORAGE_OFFSET, colour, sizeof(colour));
        const iova_t storage = heap_iova + STORAGE_OFFSET;
        u32* const slot = (u32*)(heap + FS_CBUF + 0x110);
        slot[0] = (u32)storage, slot[1] = (u32)(storage >> 32), slot[2] = sizeof(colour), slot[3] = 0;
    }
#define VERTICES_OFFSET(i) (0x1000 + (i) * 0x100)
#define QUAD_OFFSET(i) (0x1800 + (i) * 0x100)
#define TILES_OFFSET(i) (0x1C00 + (i) * 0x80)
#define DEPTH_OFFSET(i) (0x2400 + (i) * 0x200) // the depth bars (8 vertices), then the storage square (4)
#define LINES_OFFSET(i) (0x3200 + (i) * 0x200)  // the lines (5 vertices), then the point (1)
#define FLOOR_OFFSET(i) (0x3C00 + (i) * 0x80)   // the floor (4 vertices)

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
        const u64 down = padGetButtonsDown(&pad);
        if (down & HidNpadButton_Plus)
            break;
        if (down & HidNpadButton_A) {
            stress = !stress;
            fprintf(stderr, "gputest: stress test %s (%d sprites)\n", stress ? "on" : "off", stress_sprites);
        }
        if (down & (HidNpadButton_Up | HidNpadButton_Down)) {
            stress_sprites += (down & HidNpadButton_Up) ? 100 : -100;
            stress_sprites = stress_sprites < 100 ? 100 : stress_sprites > MAX_SPRITES ? MAX_SPRITES : stress_sprites;
            fprintf(stderr, "gputest: stress test: %d sprites\n", stress_sprites);
        }

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
            // the tile's copy: 64 lines, pitch to block linear
            const iova_t tile_src = sprite_iova + 0x2000;
            const u32 tile_dma[8] = {(u32)(tile_src >> 32), (u32)tile_src, (u32)(tile_image >> 32),
                                     (u32)tile_image, TILE * 4, 0, TILE * 4, TILE};
            method_on(SUBCH_DMA, MDMA_OFFSET_IN, 8, tile_dma);
            const u32 tile_dst[6] = {3 << 4 /* 8 GOBs high */, TILE * 4, TILE, 1, 0, 0};
            method_on(SUBCH_DMA, MDMA_DST_PARAMS, 6, tile_dst);
            const u32 tile_launch = 2 | 1 << 2 | 1 << 7 /* from pitch */ | 0 << 8 /* to block linear */ |
                                    1 << 9 /* multi-line */;
            method_on(SUBCH_DMA, MDMA_LAUNCH, 1, &tile_launch);
            method1(M_RT_CONTROL, 1 | (076543210 << 4)); // one render target, mapped to itself
            method1(M_COLOR_MASK, 0x1111);
            method1(M_CLEAR_FLAGS, CLEAR_FLAGS_SCISSOR);
            // the shaders, and the triangle's fixed state: one float4 attribute, the viewport
            const u32 region[2] = {(u32)(heap_iova >> 32), (u32)heap_iova};
            method(M_PROGRAM_REGION, 2, region);
            const u32 vertex_b[4] = {1 | 1 << 4, VS_OFFSET, 0, 8};
            method(M_PIPELINE + 1 * 0x10, 4, vertex_b);
            const u32 pixel[4] = {1 | 5 << 4, FS_OFFSET, 0, 8};
            method(M_PIPELINE + 5 * 0x10, 4, pixel);
            const u32 attribs[2] = {ATTR_RGBA32F, ATTR_RGBA32F | 16 << 7};
            method(M_VERTEX_ATTRIB, 2, attribs);
            const u32 transform[8] = {f32bits(W / 2.0f), f32bits(H / 2.0f), f32bits(0.5f), f32bits(W / 2.0f),
                                      f32bits(H / 2.0f), f32bits(0.5f), 0x6420 /* identity */, 0};
            method(M_VIEWPORT_TRANSFORM, 8, transform);
            const u32 viewport[4] = {(u32)W << 16, (u32)H << 16, f32bits(0.0f), f32bits(1.0f)};
            method(M_VIEWPORT, 4, viewport);
            method1(M_VIEWPORT_TRANSFORM_ENABLE, 1);
            // textures: the pools, the handles' constant buffer
            method1(M_SAMPLER_BINDING, 1);
            const u32 headers[3] = {(u32)((heap_iova + TIC_POOL) >> 32), (u32)(heap_iova + TIC_POOL), 2};
            method(M_TEX_HEADER_POOL, 3, headers);
            const u32 samplers[3] = {(u32)((heap_iova + TSC_POOL) >> 32), (u32)(heap_iova + TSC_POOL), 2};
            // blending (for the tiles): source alpha over the frame
            const u32 blend[8] = {0, BLEND_ADD, BLEND_SRC_ALPHA, BLEND_ONE_MINUS_SRC_ALPHA,
                                  BLEND_ADD, BLEND_SRC_ALPHA, 0, BLEND_ONE_MINUS_SRC_ALPHA};
            method(M_BLEND, 8, blend);
            method(M_TEX_SAMPLER_POOL, 3, samplers);
            method1(M_TEXTURE_CBUF_SLOT, 0);
            const u32 cbuf[3] = {0x200, (u32)((heap_iova + FS_CBUF) >> 32), (u32)(heap_iova + FS_CBUF)};
            method(M_CONST_BUFFER, 3, cbuf);
            method1(M_BIND_FRAGMENT_CBUF, 1 | 0 << 4);
            const u32 screen[2] = {(u32)W << 16, (u32)H << 16};
            method(M_SCREEN_SCISSOR, 2, screen);
            // the depth buffer, its test off but for the depth bars
            const u32 zeta[5] = {(u32)(depth_iova >> 32), (u32)depth_iova, ZETA_Z24S8, 4 << 4, 0};
            method(M_ZETA, 5, zeta);
            const u32 zeta_size[3] = {W, H, 1 | 1 << 16};
            method(M_ZETA_SIZE, 3, zeta_size);
            method1(M_ZETA_ENABLE, 1);
            method1(M_DEPTH_FUNC, DEPTH_LESS_EQUAL);
            method1(M_CLEAR_DEPTH, f32bits(1.0f));
            method1(M_CLEAR_STENCIL, 0);
            upload_macro();
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
        // tiles over them, blended, scrolling: the whole frame in one quad, each tile 128 pixels
        {
            const int index = (int)(g_cmd - cmds);
            float* v = (float*)(heap + TILES_OFFSET(index));
            static const float corners[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
            const float scroll_s = (float)frame * 0.004f, scroll_t = (float)frame * 0.0025f;
            for (int k = 0; k < 4; k++) {
                v[k * 8 + 0] = corners[k][0];
                v[k * 8 + 1] = corners[k][1];
                v[k * 8 + 2] = 0.5f;
                v[k * 8 + 3] = 1.0f;
                v[k * 8 + 4] = (corners[k][0] + 1) / 2 * (W / 128.0f) + scroll_s;
                v[k * 8 + 5] = (corners[k][1] + 1) / 2 * (H / 128.0f) + scroll_t;
                v[k * 8 + 6] = 0.0f;
                v[k * 8 + 7] = 1.0f;
            }
            const iova_t tiles = heap_iova + TILES_OFFSET(index);
            const u32 scissor[3] = {1, (u32)W << 16, (u32)H << 16}; // the whole frame
            method(M_SCISSOR, 3, scissor);
            method1(M_PIPELINE + 5 * 0x10 + 1, FS_TILES_OFFSET);
            method1(M_BLEND_ENABLE, 1);
            const u32 stream[4] = {32 | 1 << 12, (u32)(tiles >> 32), (u32)tiles, 0};
            method(M_VERTEX_STREAM, 4, stream);
            const iova_t last = tiles + 4 * 32 - 1;
            const u32 limit[2] = {(u32)(last >> 32), (u32)last};
            method(M_VERTEX_STREAM_LIMIT, 2, limit);
            const u32 range[2] = {0, 4};
            method(M_VERTEX_FIRST, 2, range);
            method1(M_DRAW_BEGIN, TOPOLOGY_TRIANGLE_STRIP);
            method1(M_DRAW_END, 0);
            method1(M_BLEND_ENABLE, 0);
            method1(M_PIPELINE + 5 * 0x10 + 1, FS_OFFSET);
        }
        // from frame 30, the CPU paints the sprite's middle blue: the turning square, which samples
        // the sprite itself (the circling ones are blits of its copy), shows it
        if (frame == 30) {
            for (int y = 12; y < 20; y++)
                for (int x = 12; x < 20; x++)
                    sprite_mem[y * SPRITE + x] = 0xFFFF4010; // blue
            armDCacheFlush(sprite_mem, SPRITE * SPRITE * 4);
        }
        for (int i = 0; i < 12; i++) {
            squares[i].x += squares[i].vx;
            squares[i].y += squares[i].vy;
            if (squares[i].x < 0 || squares[i].x > W - 32) squares[i].vx = -squares[i].vx;
            if (squares[i].y < 0 || squares[i].y > H - 32) squares[i].vy = -squares[i].vy;
            rect_by_macro((int)squares[i].x, (int)squares[i].y, 32, 32, squares[i].r, squares[i].g, squares[i].b);
        }
        word_gpu(W / 2 - 3 * 6 * 12 / 2, H / 2 - 7 * 12 / 2, 12, 1.0f, 1.0f, 1.0f);
        // sprites from the DMA engine's copy, circling at three sizes
        for (int i = 0; i < 3; i++) {
            const float angle = (float)frame * 0.03f + i * 2.094f;
            const int scale = i + 1;
            const int cx = W / 2 + (int)(200 * cosf(angle)), cy = H / 2 + (int)(110 * sinf(angle));
            sprite(sprite_copy, target, cx - SPRITE * scale / 2, cy - SPRITE * scale / 2, scale);
        }
        // the triangle, drawn with the shaders, turning
        {
            const int index = (int)(g_cmd - cmds);
            float* v = (float*)(heap + VERTICES_OFFSET(index));
            static const float colours[3][4] = {{1, 0.2f, 0.2f, 1}, {0.2f, 1, 0.3f, 1}, {0.3f, 0.4f, 1, 1}};
            for (int k = 0; k < 3; k++) {
                const float a = (float)frame * 0.02f + k * 2.0944f;
                v[k * 8 + 0] = 0.55f * cosf(a) * H / W;
                v[k * 8 + 1] = 0.55f * sinf(a);
                v[k * 8 + 2] = 0.5f;
                v[k * 8 + 3] = 1.0f;
                memcpy(&v[k * 8 + 4], colours[k], sizeof(colours[k]));
            }
            const iova_t vertices = heap_iova + VERTICES_OFFSET(index);
            const u32 scissor[3] = {1, (u32)W << 16, (u32)H << 16};
            method(M_SCISSOR, 3, scissor);
            const u32 stream[4] = {32 | 1 << 12, (u32)(vertices >> 32), (u32)vertices, 0};
            method(M_VERTEX_STREAM, 4, stream);
            const iova_t last = vertices + 3 * 32 - 1;
            const u32 limit[2] = {(u32)(last >> 32), (u32)last};
            method(M_VERTEX_STREAM_LIMIT, 2, limit);
            const u32 range[2] = {0, 3};
            method(M_VERTEX_FIRST, 2, range);
            method1(M_DRAW_BEGIN, TOPOLOGY_TRIANGLES);
            method1(M_DRAW_END, 0);
        }
        // a textured square (the sprite, by the texture unit), turning the other way
        {
            const int index = (int)(g_cmd - cmds);
            float* v = (float*)(heap + QUAD_OFFSET(index));
            static const float corners[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
            const float a = -(float)frame * 0.015f, size = 0.22f;
            for (int k = 0; k < 4; k++) {
                const float cx = corners[k][0] * size, cy = corners[k][1] * size;
                v[k * 8 + 0] = (cx * cosf(a) - cy * sinf(a)) * H / W - 0.55f;
                v[k * 8 + 1] = cx * sinf(a) + cy * cosf(a) + 0.45f;
                v[k * 8 + 2] = 0.5f;
                v[k * 8 + 3] = 1.0f;
                v[k * 8 + 4] = (corners[k][0] + 1) / 2; // s
                v[k * 8 + 5] = (corners[k][1] + 1) / 2; // t
                v[k * 8 + 6] = 0.0f;
                v[k * 8 + 7] = 1.0f;
            }
            const iova_t quad = heap_iova + QUAD_OFFSET(index);
            method1(M_PIPELINE + 5 * 0x10 + 1, FS_TEX_OFFSET);
            const u32 stream[4] = {32 | 1 << 12, (u32)(quad >> 32), (u32)quad, 0};
            method(M_VERTEX_STREAM, 4, stream);
            const iova_t last = quad + 4 * 32 - 1;
            const u32 limit[2] = {(u32)(last >> 32), (u32)last};
            method(M_VERTEX_STREAM_LIMIT, 2, limit);
            const u32 range[2] = {0, 4};
            method(M_VERTEX_FIRST, 2, range);
            method1(M_DRAW_BEGIN, TOPOLOGY_TRIANGLE_STRIP);
            method1(M_DRAW_END, 0);
            method1(M_PIPELINE + 5 * 0x10 + 1, FS_OFFSET);
        }
        // a floor in perspective, bottom middle: its far end 8 times as far as its near one, its
        // texture repeated 4 times across and 6 along, mipmapped (the levels' colours show where each
        // is sampled), striped by its shader
        {
            const int index = (int)(g_cmd - cmds);
            float* v = (float*)(heap + FLOOR_OFFSET(index));
            for (int k = 0; k < 4; k++) {
                const int right = k & 1, far = k >> 1;
                const float w = far ? 8.0f : 1.0f;                // depth
                const float x = 0.2f + (right ? 0.36f : -0.36f) / w; // where it's seen
                const float y = 0.52f + 0.45f / w;
                v[k * 8 + 0] = x * w, v[k * 8 + 1] = y * w, v[k * 8 + 2] = 0.5f * w, v[k * 8 + 3] = w;
                v[k * 8 + 4] = right ? 4.0f : 0.0f, v[k * 8 + 5] = far ? 6.0f : 0.0f;
                v[k * 8 + 6] = 0.0f, v[k * 8 + 7] = 1.0f;
            }
            const iova_t floor_vertices = heap_iova + FLOOR_OFFSET(index);
            const u32 scissor[3] = {1, (u32)W << 16, (u32)H << 16};
            method(M_SCISSOR, 3, scissor);
            method1(M_PIPELINE + 5 * 0x10 + 1, FS_FLOOR_OFFSET);
            const u32 stream[4] = {32 | 1 << 12, (u32)(floor_vertices >> 32), (u32)floor_vertices, 0};
            method(M_VERTEX_STREAM, 4, stream);
            const iova_t last = floor_vertices + 4 * 32 - 1;
            const u32 limit[2] = {(u32)(last >> 32), (u32)last};
            method(M_VERTEX_STREAM_LIMIT, 2, limit);
            const u32 range[2] = {0, 4};
            method(M_VERTEX_FIRST, 2, range);
            method1(M_DRAW_BEGIN, TOPOLOGY_TRIANGLE_STRIP);
            method1(M_DRAW_END, 0);
            method1(M_PIPELINE + 5 * 0x10 + 1, FS_OFFSET);
        }
        // the depth test: two bars across each other in depth (a red one, then a green one), each
        // in front on its own half; the depth buffer cleared first
        {
            const u32 scissor[3] = {1, (u32)W << 16, (u32)H << 16};
            method(M_SCISSOR, 3, scissor);
            method1(M_CLEAR_BUFFERS, CLEAR_Z | CLEAR_S);
            const int index = (int)(g_cmd - cmds);
            float* v = (float*)(heap + DEPTH_OFFSET(index));
            static const float xs[2] = {0.45f, 0.95f}, ys[2] = {-0.85f, -0.55f};
            for (int bar = 0; bar < 2; bar++) {
                for (int k = 0; k < 4; k++) {
                    float* p = v + (bar * 4 + k) * 8;
                    const int right = k & 1;
                    p[0] = xs[right];
                    p[1] = ys[k >> 1];
                    p[2] = (right ^ bar) ? 0.6f : -0.6f; // red: far on the right; green: far on the left
                    p[3] = 1.0f;
                    p[4] = bar ? 0.2f : 1.0f;
                    p[5] = bar ? 0.9f : 0.15f;
                    p[6] = 0.15f;
                    p[7] = 1.0f;
                }
            }
            method1(M_DEPTH_TEST_ENABLE, 1);
            method1(M_DEPTH_WRITE_ENABLE, 1);
            const iova_t bars = heap_iova + DEPTH_OFFSET(index);
            const u32 stream[4] = {32 | 1 << 12, (u32)(bars >> 32), (u32)bars, 0};
            method(M_VERTEX_STREAM, 4, stream);
            const iova_t last = bars + 8 * 32 - 1;
            const u32 limit[2] = {(u32)(last >> 32), (u32)last};
            method(M_VERTEX_STREAM_LIMIT, 2, limit);
            for (int bar = 0; bar < 2; bar++) {
                const u32 range[2] = {(u32)bar * 4, 4};
                method(M_VERTEX_FIRST, 2, range);
                method1(M_DRAW_BEGIN, TOPOLOGY_TRIANGLE_STRIP);
                method1(M_DRAW_END, 0);
            }
            method1(M_DEPTH_TEST_ENABLE, 0);
            method1(M_DEPTH_WRITE_ENABLE, 0);
        }
        // a square coloured from a storage buffer, top left
        {
            const int index = (int)(g_cmd - cmds);
            float* v = (float*)(heap + DEPTH_OFFSET(index) + 8 * 32);
            for (int k = 0; k < 4; k++) {
                v[k * 8 + 0] = (k & 1) ? -0.75f : -0.95f;
                v[k * 8 + 1] = (k & 2) ? -0.80f : -0.95f;
                v[k * 8 + 2] = 0.5f, v[k * 8 + 3] = 1.0f;
                v[k * 8 + 4] = v[k * 8 + 5] = v[k * 8 + 6] = v[k * 8 + 7] = 1.0f;
            }
            const iova_t square = heap_iova + DEPTH_OFFSET(index) + 8 * 32;
            method1(M_PIPELINE + 5 * 0x10 + 1, FS_STORAGE_OFFSET);
            const u32 stream[4] = {32 | 1 << 12, (u32)(square >> 32), (u32)square, 0};
            method(M_VERTEX_STREAM, 4, stream);
            const iova_t last = square + 4 * 32 - 1;
            const u32 limit[2] = {(u32)(last >> 32), (u32)last};
            method(M_VERTEX_STREAM_LIMIT, 2, limit);
            // drawn with its indices in the command stream (backwards: the same square)
            method1(M_DRAW_BEGIN, TOPOLOGY_TRIANGLE_STRIP);
            emit(header(NONINCREASING, 4, SUBCH_3D, M_DRAW_INLINE_INDEX));
            for (u32 i = 0; i < 4; i++)
                emit(3 - i);
            method1(M_DRAW_END, 0);
            method1(M_PIPELINE + 5 * 0x10 + 1, FS_OFFSET);
        }
        // lines (a yellow rectangle, 3 pixels wide: a line strip back to its start; Eden's Vulkan
        // renderer draws line loops as triangles) and a point (white, 6 pixels), bottom right
        {
            const int index = (int)(g_cmd - cmds);
            float* v = (float*)(heap + LINES_OFFSET(index));
            static const float corners[6][2] = {{0.62f, 0.70f}, {0.92f, 0.70f}, {0.92f, 0.92f},
                                                {0.62f, 0.92f}, {0.62f, 0.70f}, {0.77f, 0.81f}};
            for (int k = 0; k < 6; k++) {
                v[k * 8 + 0] = corners[k][0], v[k * 8 + 1] = corners[k][1];
                v[k * 8 + 2] = 0.5f, v[k * 8 + 3] = 1.0f;
                v[k * 8 + 4] = 1.0f, v[k * 8 + 5] = k < 5 ? 0.9f : 1.0f, v[k * 8 + 6] = k < 5 ? 0.1f : 1.0f;
                v[k * 8 + 7] = 1.0f;
            }
            const iova_t lines = heap_iova + LINES_OFFSET(index);
            const u32 stream[4] = {32 | 1 << 12, (u32)(lines >> 32), (u32)lines, 0};
            method(M_VERTEX_STREAM, 4, stream);
            const iova_t last = lines + 6 * 32 - 1;
            const u32 limit[2] = {(u32)(last >> 32), (u32)last};
            method(M_VERTEX_STREAM_LIMIT, 2, limit);
            method1(M_LINE_WIDTH, f32bits(3.0f));
            method1(M_POINT_SIZE, f32bits(6.0f));
            const u32 strip[2] = {0, 5};
            method(M_VERTEX_FIRST, 2, strip);
            method1(M_DRAW_BEGIN, TOPOLOGY_LINE_STRIP);
            method1(M_DRAW_END, 0);
            const u32 point[2] = {5, 1};
            method(M_VERTEX_FIRST, 2, point);
            method1(M_DRAW_BEGIN, TOPOLOGY_POINTS);
            method1(M_DRAW_END, 0);
        }
        // the stencil test: a triangle drawn into the stencil only (1 where it is), then an orange
        // square drawn where the stencil is 1, left of the middle
        {
            const int index = (int)(g_cmd - cmds);
            float* v = (float*)(heap + STENCIL_OFFSET(index));
            static const float points[7][2] = {{-0.95f, 0.30f}, {-0.70f, 0.30f}, {-0.825f, 0.0f},
                                               {-0.95f, 0.0f}, {-0.70f, 0.0f}, {-0.95f, 0.30f}, {-0.70f, 0.30f}};
            for (int k = 0; k < 7; k++) {
                v[k * 8 + 0] = points[k][0], v[k * 8 + 1] = points[k][1];
                v[k * 8 + 2] = 0.5f, v[k * 8 + 3] = 1.0f;
                v[k * 8 + 4] = 1.0f, v[k * 8 + 5] = 0.55f, v[k * 8 + 6] = 0.1f, v[k * 8 + 7] = 1.0f;
            }
            const iova_t shapes = heap_iova + STENCIL_OFFSET(index);
            const u32 stream[4] = {32 | 1 << 12, (u32)(shapes >> 32), (u32)shapes, 0};
            method(M_VERTEX_STREAM, 4, stream);
            const iova_t last = shapes + 7 * 32 - 1;
            const u32 limit[2] = {(u32)(last >> 32), (u32)last};
            method(M_VERTEX_STREAM_LIMIT, 2, limit);
            method1(M_STENCIL_ENABLE, 1);
            method1(M_STENCIL_TWO_SIDE, 0); // the triangle faces away: the same state for it
            const u32 write_op[4] = {STENCIL_KEEP, STENCIL_KEEP, STENCIL_REPLACE, FUNC_ALWAYS};
            method(M_STENCIL_FRONT_OP, 4, write_op);
            const u32 ref[3] = {1, 0xFF, 0xFF};
            method(M_STENCIL_FRONT_REF, 3, ref);
            method1(M_COLOR_MASK, 0); // the stencil only
            const u32 triangle[2] = {0, 3};
            method(M_VERTEX_FIRST, 2, triangle);
            method1(M_DRAW_BEGIN, TOPOLOGY_TRIANGLES);
            method1(M_DRAW_END, 0);
            method1(M_COLOR_MASK, 0x1111);
            const u32 test_op[4] = {STENCIL_KEEP, STENCIL_KEEP, STENCIL_KEEP, FUNC_EQUAL};
            method(M_STENCIL_FRONT_OP, 4, test_op);
            const u32 square[2] = {3, 4};
            method(M_VERTEX_FIRST, 2, square);
            method1(M_DRAW_BEGIN, TOPOLOGY_TRIANGLE_STRIP);
            method1(M_DRAW_END, 0);
            method1(M_STENCIL_ENABLE, 0);
        }
        // the stress test: sprites (the sprite texture on 24x24 squares) wandering, a draw each
        if (stress) {
            const int index = (int)(g_cmd - cmds);
            float* base = sprite_vertices + (size_t)index * MAX_SPRITES * 32;
            const iova_t base_iova = sprite_vertices_iova + (iova_t)index * MAX_SPRITES * 128;
            const u32 scissor[3] = {1, (u32)W << 16, (u32)H << 16};
            method(M_SCISSOR, 3, scissor);
            method1(M_PIPELINE + 5 * 0x10 + 1, FS_TEX_OFFSET);
            for (int i = 0; i < stress_sprites; i++) {
                const float t = (float)frame * 0.02f + (float)i * 0.37f;
                const float cx = 0.9f * sinf(t * 0.7f + i), cy = 0.85f * cosf(t * 0.5f + i * 1.3f);
                const float hw = 12.0f * 2 / W, hh = 12.0f * 2 / H;
                float* v = base + i * 32;
                for (int k = 0; k < 4; k++) {
                    v[k * 8 + 0] = cx + ((k & 1) ? hw : -hw);
                    v[k * 8 + 1] = cy + ((k & 2) ? hh : -hh);
                    v[k * 8 + 2] = 0.5f, v[k * 8 + 3] = 1.0f;
                    v[k * 8 + 4] = (float)(k & 1), v[k * 8 + 5] = (float)((k >> 1) & 1);
                    v[k * 8 + 6] = 0.0f, v[k * 8 + 7] = 1.0f;
                }
                const iova_t quad = base_iova + (iova_t)i * 128;
                const u32 stream[4] = {32 | 1 << 12, (u32)(quad >> 32), (u32)quad, 0};
                method(M_VERTEX_STREAM, 4, stream);
                const iova_t last = quad + 4 * 32 - 1;
                const u32 limit[2] = {(u32)(last >> 32), (u32)last};
                method(M_VERTEX_STREAM_LIMIT, 2, limit);
                const u32 range[2] = {0, 4};
                method(M_VERTEX_FIRST, 2, range);
                method1(M_DRAW_BEGIN, TOPOLOGY_TRIANGLE_STRIP);
                method1(M_DRAW_END, 0);
            }
            method1(M_PIPELINE + 5 * 0x10 + 1, FS_OFFSET);
            armDCacheFlush(base, (size_t)stress_sprites * 128);
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
            if (frame < 200 || frame % 600 < 60 || stress)
                fprintf(stderr, "gputest: %d fps, %u command words a frame%s\n", fps, g_cmd->used,
                        stress ? " (stress test)" : "");
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
