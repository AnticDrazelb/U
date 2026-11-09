// The NV2A's drawing through Vulkan: see gpu.h. One device, one queue; draws are recorded as they come
// and submitted when something must see their results (Sync). Every image stays in the GENERAL layout
// and a full memory barrier separates transfers from drawing: simple, and enough for a first version.
#ifdef XB_NO_VULKAN
// A build without RADV (the PS4 app at 0x400000, where its size would crowd the Xbox's RAM): software
// drawing only.
#include "gpu.h"
namespace xb::gpu {
bool Enabled() { return false; }
void SetWanted(bool) {}
void SetGuardFile(const std::string&) {}
const char* Status() { return "off: not in this build (the 1 GiB one has it)"; }
bool DrawTriangles(const Vertex*, u32) { return false; }
bool Clear(u32) { return false; }
void Sync() {}
void SyncRange(u32, u32) {}
} // namespace xb::gpu
#else
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include "draw.h"
#include "gpu.h"
#include "nv2a.h"

#include "nv2a_frag.h" // SPIR-V, made at build time from shaders/nv2a.frag (glslangValidator --vn)
#include "nv2a_vert.h"

#ifdef __PS4__
// RADV, linked in: its ICD entry point (Crema's port of it, port/mesa)
extern "C" VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char* name);
#else
#include <dlfcn.h>
#endif

namespace xb::gpu {

namespace {

#define XB_VK_INSTANCE_FUNCS(X)                                                                                        \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties)         \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice)                    \
    X(vkGetDeviceProcAddr) X(vkEnumerateDeviceExtensionProperties)
#define XB_VK_DEVICE_FUNCS(X)                                                                                          \
    X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer)                     \
    X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkQueueSubmit) X(vkCreateFence) X(vkWaitForFences)                \
    X(vkResetFences) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) X(vkFreeMemory)            \
    X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements)            \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkCreateShaderModule)        \
    X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkCmdBeginRendering)       \
    X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindVertexBuffers) X(vkCmdSetViewport) X(vkCmdSetScissor)        \
    X(vkCmdPushConstants) X(vkCmdPushDescriptorSetKHR) X(vkCmdDraw) X(vkCmdPipelineBarrier) X(vkCmdCopyBufferToImage) \
    X(vkCmdCopyImageToBuffer) X(vkCmdClearAttachments) X(vkCmdSetBlendConstants)

PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr_ = nullptr;
PFN_vkCreateInstance vkCreateInstance_ = nullptr;
#define XB_DECLARE(name) PFN_##name name##_ = nullptr;
XB_VK_INSTANCE_FUNCS(XB_DECLARE)
XB_VK_DEVICE_FUNCS(XB_DECLARE)
#undef XB_DECLARE

constexpr VkFormat kColorFormat = VK_FORMAT_B8G8R8A8_UNORM; // A8R8G8B8 in memory
constexpr VkFormat kDepthFormat = VK_FORMAT_D32_SFLOAT_S8_UINT;
constexpr VkDeviceSize kVertexRing = 32u << 20, kUniformRing = 8u << 20, kStagingRing = 64u << 20;

std::recursive_mutex g_mutex;
bool g_wanted = false, g_tried = false, g_ok = false;
std::string g_status = "off";
// Where the console notes the step it's in while the GPU starts (as the CPU self-test does): a run
// that dies there leaves it, and the next run of the same build keeps the GPU off.
std::string g_guard_file;
bool g_guard_pending = false;
const char* const kBuild = __DATE__ " " __TIME__;

void Guard(const char* step)
{
    if (g_guard_file.empty())
        return;
    if (!step)
    {
        std::remove(g_guard_file.c_str());
        g_guard_pending = false;
        return;
    }
    if (FILE* f = std::fopen(g_guard_file.c_str(), "w"))
    {
        std::fprintf(f, "%s\n%s\n", step, kBuild);
        std::fclose(f);
    }
    g_guard_pending = true;
}

// the step the last run of this build died in, or ""
std::string GuardedDeath()
{
    std::string step;
    if (g_guard_file.empty())
        return step;
    if (FILE* f = std::fopen(g_guard_file.c_str(), "r"))
    {
        char a[128] = {}, b[128] = {};
        if (std::fgets(a, sizeof(a), f) && std::fgets(b, sizeof(b), f) && std::string(b).rfind(kBuild, 0) == 0)
        {
            step = a;
            while (!step.empty() && (step.back() == '\n' || step.back() == '\r'))
                step.pop_back();
        }
        std::fclose(f);
    }
    return step;
}

VkInstance g_instance = VK_NULL_HANDLE;
VkPhysicalDevice g_physical = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
VkQueue g_queue = VK_NULL_HANDLE;
u32 g_queue_family = 0;
VkPhysicalDeviceMemoryProperties g_memory_props{};
VkDeviceSize g_ubo_align = 256;
VkCommandPool g_pool = VK_NULL_HANDLE;
VkCommandBuffer g_cmd = VK_NULL_HANDLE;
VkFence g_fence = VK_NULL_HANDLE;
VkDescriptorSetLayout g_set_layout = VK_NULL_HANDLE;
VkPipelineLayout g_layout = VK_NULL_HANDLE;
VkShaderModule g_vert = VK_NULL_HANDLE, g_frag = VK_NULL_HANDLE;
bool g_recording = false;
u64 g_epoch = 1; // advances at each Sync: textures are rehashed once an epoch at most

struct Buffer
{
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    u8* map = nullptr;
    VkDeviceSize size = 0, used = 0;
};
Buffer g_vertices, g_uniforms, g_staging;

u32 MemoryType(u32 bits, VkMemoryPropertyFlags want)
{
    for (u32 i = 0; i < g_memory_props.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g_memory_props.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return ~0u;
}

bool MakeBuffer(Buffer& b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size, info.usage = usage;
    if (vkCreateBuffer_(g_device, &info, nullptr, &b.buffer) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements_(g_device, b.buffer, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (alloc.memoryTypeIndex == ~0u || vkAllocateMemory_(g_device, &alloc, nullptr, &b.memory) != VK_SUCCESS)
        return false;
    vkBindBufferMemory_(g_device, b.buffer, b.memory, 0);
    void* p = nullptr;
    vkMapMemory_(g_device, b.memory, 0, size, 0, &p);
    b.map = static_cast<u8*>(p), b.size = size, b.used = 0;
    return true;
}

// room for `size` bytes in a ring (aligned); ~0 if it's full until the next Sync
VkDeviceSize Take(Buffer& b, VkDeviceSize size, VkDeviceSize align)
{
    const VkDeviceSize at = (b.used + align - 1) & ~(align - 1);
    if (at + size > b.size)
        return ~VkDeviceSize(0);
    b.used = at + size;
    return at;
}

void EndRendering();

struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
};

void Barrier()
{
    VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    m.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    m.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    vkCmdPipelineBarrier_(g_cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &m, 0,
                          nullptr, 0, nullptr);
}

void BeginCommands()
{
    if (g_recording)
        return;
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer_(g_cmd, &begin);
    g_recording = true;
}

bool MakeImage(Image& img, VkFormat format, u32 width, u32 height, u32 levels, bool cube, VkImageUsageFlags usage,
               VkImageAspectFlags aspect)
{
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {width, height, 1};
    info.mipLevels = levels;
    info.arrayLayers = cube ? 6 : 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage_(g_device, &info, nullptr, &img.image) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements_(g_device, img.image, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (alloc.memoryTypeIndex == ~0u)
        alloc.memoryTypeIndex = MemoryType(req.memoryTypeBits, 0);
    if (vkAllocateMemory_(g_device, &alloc, nullptr, &img.memory) != VK_SUCCESS)
        return false;
    vkBindImageMemory_(g_device, img.image, img.memory, 0);
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = img.image;
    view.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {aspect, 0, levels, 0, cube ? 6u : 1u};
    if (vkCreateImageView_(g_device, &view, nullptr, &img.view) != VK_SUCCESS)
        return false;
    // to GENERAL, where it stays
    EndRendering();
    BeginCommands();
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img.image;
    b.subresourceRange = {aspect, 0, levels, 0, cube ? 6u : 1u};
    vkCmdPipelineBarrier_(g_cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                          nullptr, 1, &b);
    return true;
}

std::vector<Image> g_trash; // images replaced while commands may still use them: freed after the next Sync

void Free(Image& img)
{
    if (img.view)
        vkDestroyImageView_(g_device, img.view, nullptr);
    if (img.image)
        vkDestroyImage_(g_device, img.image, nullptr);
    if (img.memory)
        vkFreeMemory_(g_device, img.memory, nullptr);
    img = Image{};
}

u64 Hash(u32 address, u32 bytes)
{
    u64 h = 0xcbf29ce484222325ull ^ bytes;
    const u8* p = Ptr<const u8>(address);
    u32 i = 0;
    for (; i + 8 <= bytes; i += 8)
    {
        u64 v;
        std::memcpy(&v, p + i, 8);
        h = (h ^ v) * 0x100000001b3ull;
    }
    for (; i < bytes; i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// ---- surfaces: the guest's colour and depth buffers, as images

struct GpuSurface
{
    Surface s;
    bool depth = false;
    Image img;
    Buffer readback;
    bool gpu_newer = false; // drawn since last written back
    bool loaded = false;    // the image has had the guest's content
    bool check = true;      // compare the guest's memory before the next use
    u64 hash = 0;
    bool z_float = false;
    float z_max = 1;
    u32 Bytes() const { return s.swizzled ? s.width * s.height * s.bytes : s.pitch * s.height; }
};
std::vector<std::unique_ptr<GpuSurface>> g_surfaces;

GpuSurface* FindSurface(const Surface& s, bool depth)
{
    for (auto& g : g_surfaces)
        if (g->depth == depth && g->s.address == s.address && g->s.width == s.width && g->s.height == s.height &&
            g->s.pitch == s.pitch && g->s.bytes == s.bytes && g->s.swizzled == s.swizzled && g->s.format == s.format)
            return g.get();
    return nullptr;
}

// the guest's content into the image (recorded; the staging ring holds it until the submit)
bool Upload(GpuSurface& g)
{
    const u32 w = g.s.width, h = g.s.height;
    if (!g.depth)
    {
        const VkDeviceSize at = Take(g_staging, VkDeviceSize(w) * h * 4, 16);
        if (at == ~VkDeviceSize(0))
            return false;
        u32* out = reinterpret_cast<u32*>(g_staging.map + at);
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++)
                out[y * w + x] = ReadPixel(g.s, x, y);
        EndRendering();
        BeginCommands();
        Barrier();
        VkBufferImageCopy copy{};
        copy.bufferOffset = at;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {w, h, 1};
        vkCmdCopyBufferToImage_(g_cmd, g_staging.buffer, g.img.image, VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
        Barrier();
        return true;
    }
    const VkDeviceSize zat = Take(g_staging, VkDeviceSize(w) * h * 4, 16), sat = Take(g_staging, VkDeviceSize(w) * h, 16);
    if (zat == ~VkDeviceSize(0) || sat == ~VkDeviceSize(0))
        return false;
    float* z = reinterpret_cast<float*>(g_staging.map + zat);
    u8* st = g_staging.map + sat;
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++)
        {
            const u32 p = PixelAddress(g.s, x, y);
            const u32 word = g.s.bytes == 2 ? Read<u16>(p) : Read<u32>(p);
            const u32 stored = g.s.bytes == 2 ? word : word >> 8;
            z[y * w + x] = float(stored) / g.z_max;
            st[y * w + x] = g.s.bytes == 2 ? 0 : u8(word);
        }
    EndRendering();
    BeginCommands();
    Barrier();
    VkBufferImageCopy copies[2]{};
    copies[0].bufferOffset = zat;
    copies[0].imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
    copies[0].imageExtent = {w, h, 1};
    copies[1].bufferOffset = sat;
    copies[1].imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1};
    copies[1].imageExtent = {w, h, 1};
    vkCmdCopyBufferToImage_(g_cmd, g_staging.buffer, g.img.image, VK_IMAGE_LAYOUT_GENERAL, 2, copies);
    Barrier();
    return true;
}

// the image's content into the guest's memory, from its readback buffer (after the submit)
void WriteBack(GpuSurface& g)
{
    const u32 w = g.s.width, h = g.s.height;
    if (!g.depth)
    {
        const u32* in = reinterpret_cast<const u32*>(g.readback.map);
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++)
                WritePixel(g.s, x, y, in[y * w + x], 0xffffffff);
        return;
    }
    const float* z = reinterpret_cast<const float*>(g.readback.map);
    const u8* st = g.readback.map + VkDeviceSize(w) * h * 4;
    for (u32 y = 0; y < h; y++)
        for (u32 x = 0; x < w; x++)
        {
            const float v = std::clamp(z[y * w + x] * g.z_max, 0.f, g.z_max);
            const u32 zi = u32(v + 0.5f), p = PixelAddress(g.s, x, y);
            if (g.s.bytes == 2)
                Write<u16>(p, u16(zi));
            else
                Write<u32>(p, (zi << 8) | st[y * w + x]);
        }
}

GpuSurface* UseSurface(const Surface& s, bool depth, bool z_float, float z_max)
{
    GpuSurface* g = FindSurface(s, depth);
    if (!g)
    {
        auto made = std::make_unique<GpuSurface>();
        made->s = s, made->depth = depth, made->z_float = z_float, made->z_max = z_max;
        const VkImageUsageFlags usage = (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                                        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (!MakeImage(made->img, depth ? kDepthFormat : kColorFormat, s.width, s.height, 1, false, usage,
                       depth ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT))
            return nullptr;
        if (!MakeBuffer(made->readback, VkDeviceSize(s.width) * s.height * (depth ? 5 : 4), VK_BUFFER_USAGE_TRANSFER_DST_BIT))
            return nullptr;
        g = made.get();
        g_surfaces.push_back(std::move(made));
        Log("gpu: surface %#x %ux%u (%s, %u bytes a pixel%s) as an image", s.address, s.width, s.height,
            depth ? "depth" : "colour", s.bytes, s.swizzled ? ", swizzled" : "");
    }
    if (g->check)
    {
        const u64 h = Hash(s.address, g->Bytes());
        if (!g->loaded || h != g->hash)
        {
            if (!Upload(*g))
                return nullptr;
            g->loaded = true;
        }
        g->hash = h, g->check = false;
    }
    return g;
}

// ---- textures

struct GpuTexture
{
    TextureInfo info;
    Image img;
    u64 hash = 0, checked_epoch = 0;
};
std::vector<std::unique_ptr<GpuTexture>> g_textures;
std::map<std::tuple<u32, u32, u32, u32, u32, u32>, VkSampler> g_samplers;
Image g_dummy_2d, g_dummy_cube;
VkSampler g_dummy_sampler = VK_NULL_HANDLE;

u64 TextureHash(const TextureInfo& t)
{
    u64 h = Hash(t.address, t.bytes);
    if (t.palette)
        h ^= Hash(t.palette, 1024) * 31;
    return h;
}

bool Overlaps(u32 a, u32 asize, u32 b, u32 bsize)
{
    return a < b + bsize && b < a + asize;
}

void SyncLocked();

GpuTexture* UseTexture(int stage, const TextureInfo& t)
{
    // a texture drawn into by the GPU (render to texture): its texels back in memory first
    for (auto& s : g_surfaces)
        if (s->gpu_newer && Overlaps(s->s.address, s->Bytes(), t.address, t.bytes))
        {
            SyncLocked();
            break;
        }
    GpuTexture* g = nullptr;
    for (auto& x : g_textures)
        if (x->info.address == t.address && x->info.format == t.format && x->info.width == t.width &&
            x->info.height == t.height && x->info.levels == t.levels && x->info.cube == t.cube && x->info.pitch == t.pitch &&
            x->info.palette == t.palette)
        {
            g = x.get();
            break;
        }
    const bool small = t.bytes <= 64 * 1024;
    if (g && g->checked_epoch == g_epoch && !small)
        return g;
    const u64 h = TextureHash(t);
    if (g && g->hash == h)
    {
        g->checked_epoch = g_epoch;
        return g;
    }
    if (!g)
    {
        g_textures.push_back(std::make_unique<GpuTexture>());
        g = g_textures.back().get();
    }
    else
        g_trash.push_back(g->img), g->img = Image{};
    g->info = t, g->hash = h, g->checked_epoch = g_epoch;
    const u32 levels = std::max<u32>(1, t.levels), faces = t.cube ? 6 : 1;
    if (!MakeImage(g->img, kColorFormat, t.width, t.height, levels, t.cube,
                   VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT))
        return nullptr;
    std::vector<VkBufferImageCopy> copies;
    for (u32 face = 0; face < faces; face++)
        for (u32 level = 0; level < levels; level++)
        {
            const u32 w = std::max(1u, t.width >> level), h2 = std::max(1u, t.height >> level);
            const VkDeviceSize at = Take(g_staging, VkDeviceSize(w) * h2 * 4, 16);
            if (at == ~VkDeviceSize(0))
                return nullptr;
            DecodeStageTexture(stage, face, level, reinterpret_cast<u32*>(g_staging.map + at));
            VkBufferImageCopy c{};
            c.bufferOffset = at;
            c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, face, 1};
            c.imageExtent = {w, h2, 1};
            copies.push_back(c);
        }
    EndRendering();
    BeginCommands();
    Barrier();
    vkCmdCopyBufferToImage_(g_cmd, g_staging.buffer, g->img.image, VK_IMAGE_LAYOUT_GENERAL, u32(copies.size()), copies.data());
    Barrier();
    return g;
}

VkSamplerAddressMode AddressMode(u32 mode)
{
    switch (mode)
    {
    case 1: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    }
}

VkSampler Sampler(const TextureInfo& t)
{
    const u32 lods = u32(t.min_lod * 256) | u32(t.max_lod * 256) << 12;
    const auto key = std::make_tuple(t.wrap, t.filter & 0xffffe000u, t.border, lods, t.levels, u32(int(t.lod_bias * 256)));
    if (auto it = g_samplers.find(key); it != g_samplers.end())
        return it->second;
    VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    const u32 mag = (t.filter >> 24) & 0xf, min = (t.filter >> 16) & 0xff;
    info.magFilter = mag == 1 ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.minFilter = (min == 1 || min == 3 || min == 5) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    info.mipmapMode = (min == 5 || min == 6) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info.addressModeU = AddressMode(t.wrap & 0xf);
    info.addressModeV = AddressMode((t.wrap >> 8) & 0xf);
    info.addressModeW = AddressMode((t.wrap >> 16) & 0xf);
    info.mipLodBias = t.lod_bias;
    const bool mips = t.levels > 1 && min >= 3;
    const float top = std::min(t.max_lod, float(t.levels - 1));
    info.minLod = mips ? std::min(t.min_lod, top) : 0.f;
    info.maxLod = mips ? top : 0.f;
    info.borderColor = (t.border >> 24) == 0 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                       : (t.border & 0xffffff) == 0xffffff ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                                                            : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
    VkSampler s = VK_NULL_HANDLE;
    vkCreateSampler_(g_device, &info, nullptr, &s);
    g_samplers[key] = s;
    return s;
}

// ---- pipelines: one for each combination of blending, depth and write mask

struct PipelineKey
{
    bool blend, has_depth, depth_test, depth_write, stencil;
    u32 sfactor, dfactor, equation, depth_func, write_mask;
    u32 stencil_func, stencil_ref, stencil_mask, stencil_write, op_fail, op_zfail, op_zpass;
    bool operator<(const PipelineKey& o) const
    {
        return std::tie(blend, has_depth, depth_test, depth_write, stencil, sfactor, dfactor, equation, depth_func, write_mask,
                        stencil_func, stencil_ref, stencil_mask, stencil_write, op_fail, op_zfail, op_zpass) <
               std::tie(o.blend, o.has_depth, o.depth_test, o.depth_write, o.stencil, o.sfactor, o.dfactor, o.equation,
                        o.depth_func, o.write_mask, o.stencil_func, o.stencil_ref, o.stencil_mask, o.stencil_write,
                        o.op_fail, o.op_zfail, o.op_zpass);
    }
};

VkStencilOp StencilOp(u32 op)
{
    switch (op)
    {
    case 0x0000: return VK_STENCIL_OP_ZERO;
    case 0x1e01: return VK_STENCIL_OP_REPLACE;
    case 0x1e02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case 0x1e03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case 0x150a: return VK_STENCIL_OP_INVERT;
    case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}
std::map<PipelineKey, VkPipeline> g_pipelines;

VkBlendFactor Factor(u32 f)
{
    switch (f)
    {
    case 0x0000: return VK_BLEND_FACTOR_ZERO;
    case 0x0001: return VK_BLEND_FACTOR_ONE;
    case 0x0300: return VK_BLEND_FACTOR_SRC_COLOR;
    case 0x0301: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 0x0302: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 0x0303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 0x0304: return VK_BLEND_FACTOR_DST_ALPHA;
    case 0x0305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 0x0306: return VK_BLEND_FACTOR_DST_COLOR;
    case 0x0307: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 0x0308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

VkBlendOp Equation(u32 e)
{
    switch (e)
    {
    case 0x800a: return VK_BLEND_OP_SUBTRACT;
    case 0x800b: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case 0x8007: return VK_BLEND_OP_MIN;
    case 0x8008: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

VkPipeline Pipeline(const PipelineKey& k)
{
    if (auto it = g_pipelines.find(k); it != g_pipelines.end())
        return it->second;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT, stages[0].module = g_vert, stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, stages[1].module = g_frag, stages[1].pName = "main";
    VkVertexInputBindingDescription binding{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription attributes[8];
    for (u32 i = 0; i < 8; i++)
        attributes[i] = {i, 0, VK_FORMAT_R32G32B32A32_SFLOAT, i * 16};
    VkPipelineVertexInputStateCreateInfo input{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    input.vertexBindingDescriptionCount = 1, input.pVertexBindingDescriptions = &binding;
    input.vertexAttributeDescriptionCount = 8, input.pVertexAttributeDescriptions = attributes;
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    raster.depthClampEnable = VK_TRUE; // as the software path clamps z to the buffer's range
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    raster.cullMode = VK_CULL_MODE_NONE; // culled on the CPU side
    raster.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    // depth written with the test off: the test on, always passing (Vulkan writes only when testing)
    ds.depthTestEnable = k.has_depth && (k.depth_test || k.depth_write);
    ds.depthWriteEnable = k.has_depth && k.depth_write;
    ds.depthCompareOp = k.depth_test ? VkCompareOp(k.depth_func & 7) : VK_COMPARE_OP_ALWAYS;
    // the stencil, the same both ways round: (reference & mask) func (stored & mask), as the CPU tests it
    ds.stencilTestEnable = k.has_depth && k.stencil;
    ds.front.failOp = StencilOp(k.op_fail);
    ds.front.passOp = StencilOp(k.op_zpass);
    ds.front.depthFailOp = StencilOp(k.op_zfail);
    ds.front.compareOp = VkCompareOp(k.stencil_func & 7);
    ds.front.compareMask = k.stencil_mask, ds.front.writeMask = k.stencil_write, ds.front.reference = k.stencil_ref;
    ds.back = ds.front;
    VkPipelineColorBlendAttachmentState attachment{};
    attachment.blendEnable = k.blend;
    attachment.srcColorBlendFactor = attachment.srcAlphaBlendFactor = Factor(k.sfactor);
    attachment.dstColorBlendFactor = attachment.dstAlphaBlendFactor = Factor(k.dfactor);
    attachment.colorBlendOp = attachment.alphaBlendOp = Equation(k.equation);
    attachment.colorWriteMask = k.write_mask;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1, blend.pAttachments = &attachment;
    const VkDynamicState dynamic[3] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS};
    VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dyn.dynamicStateCount = 3, dyn.pDynamicStates = dynamic;
    const VkFormat color = kColorFormat;
    VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rendering.colorAttachmentCount = 1, rendering.pColorAttachmentFormats = &color;
    rendering.depthAttachmentFormat = k.has_depth ? kDepthFormat : VK_FORMAT_UNDEFINED;
    rendering.stencilAttachmentFormat = k.has_depth ? kDepthFormat : VK_FORMAT_UNDEFINED;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.pNext = &rendering;
    info.stageCount = 2, info.pStages = stages;
    info.pVertexInputState = &input, info.pInputAssemblyState = &assembly, info.pViewportState = &viewport;
    info.pRasterizationState = &raster, info.pMultisampleState = &ms, info.pDepthStencilState = &ds;
    info.pColorBlendState = &blend, info.pDynamicState = &dyn, info.layout = g_layout;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines_(g_device, VK_NULL_HANDLE, 1, &info, nullptr, &p) != VK_SUCCESS)
        p = VK_NULL_HANDLE;
    g_pipelines[k] = p;
    return p;
}

// ---- rendering: the current pair of surfaces

GpuSurface* g_rendering_color = nullptr;
GpuSurface* g_rendering_depth = nullptr;
bool g_in_rendering = false;

void EndRendering()
{
    if (g_in_rendering)
    {
        vkCmdEndRendering_(g_cmd);
        g_in_rendering = false;
    }
}

void BeginRendering(GpuSurface* color, GpuSurface* depth)
{
    if (g_in_rendering && color == g_rendering_color && depth == g_rendering_depth)
        return;
    EndRendering();
    BeginCommands();
    Barrier();
    VkRenderingAttachmentInfo c{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    c.imageView = color->img.view, c.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    c.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, c.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingAttachmentInfo d{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (depth)
    {
        d.imageView = depth->img.view, d.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        d.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, d.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingInfo info{VK_STRUCTURE_TYPE_RENDERING_INFO};
    info.renderArea = {{0, 0}, {color->s.width, color->s.height}};
    info.layerCount = 1;
    info.colorAttachmentCount = 1, info.pColorAttachments = &c;
    info.pDepthAttachment = depth ? &d : nullptr;
    info.pStencilAttachment = depth ? &d : nullptr;
    vkCmdBeginRendering_(g_cmd, &info);
    g_in_rendering = true;
    g_rendering_color = color, g_rendering_depth = depth;
}

// ---- set-up

bool Init(std::string& why)
{
#ifdef __PS4__
    vkGetInstanceProcAddr_ = vk_icdGetInstanceProcAddr;
#else
    void* lib = dlopen("libvulkan.so.1", RTLD_NOW);
    if (!lib)
        return why = "no libvulkan.so.1", false;
    vkGetInstanceProcAddr_ = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
#endif
    if (!vkGetInstanceProcAddr_)
        return why = "no vkGetInstanceProcAddr", false;
    vkCreateInstance_ = reinterpret_cast<PFN_vkCreateInstance>(vkGetInstanceProcAddr_(VK_NULL_HANDLE, "vkCreateInstance"));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "xbox-core", app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    if (!vkCreateInstance_ || vkCreateInstance_(&ici, nullptr, &g_instance) != VK_SUCCESS)
        return why = "vkCreateInstance failed", false;
#define XB_LOAD(name) name##_ = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr_(g_instance, #name));
    XB_VK_INSTANCE_FUNCS(XB_LOAD)
#undef XB_LOAD
    u32 count = 0;
    vkEnumeratePhysicalDevices_(g_instance, &count, nullptr);
    if (!count)
        return why = "no Vulkan device", false;
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices_(g_instance, &count, devices.data());
    g_physical = devices[0];
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties_(g_physical, &props);
    g_ubo_align = std::max<VkDeviceSize>(16, props.limits.minUniformBufferOffsetAlignment);
    vkGetPhysicalDeviceMemoryProperties_(g_physical, &g_memory_props);
    u32 families = 0;
    vkGetPhysicalDeviceQueueFamilyProperties_(g_physical, &families, nullptr);
    std::vector<VkQueueFamilyProperties> fam(families);
    vkGetPhysicalDeviceQueueFamilyProperties_(g_physical, &families, fam.data());
    g_queue_family = ~0u;
    for (u32 i = 0; i < families; i++)
        if (fam[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
        {
            g_queue_family = i;
            break;
        }
    if (g_queue_family == ~0u)
        return why = "no graphics queue", false;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties_(g_physical, kDepthFormat, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT))
        return why = "no D32_SFLOAT_S8_UINT depth buffers", false;
    const float priority = 1;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = g_queue_family, qi.queueCount = 1, qi.pQueuePriorities = &priority;
    VkPhysicalDeviceVulkan13Features v13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    v13.dynamicRendering = VK_TRUE;
    VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    features.pNext = &v13;
    features.features.depthClamp = VK_TRUE;
    const char* extensions[] = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &features;
    dci.queueCreateInfoCount = 1, dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = 1, dci.ppEnabledExtensionNames = extensions;
    if (vkCreateDevice_(g_physical, &dci, nullptr, &g_device) != VK_SUCCESS)
        return why = std::string("vkCreateDevice failed on ") + props.deviceName, false;
#define XB_LOAD(name) name##_ = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr_(g_device, #name));
    XB_VK_DEVICE_FUNCS(XB_LOAD)
#undef XB_LOAD
    if (!vkCmdPushDescriptorSetKHR_ || !vkCmdBeginRendering_)
        return why = "no push descriptors or dynamic rendering", false;
    vkGetDeviceQueue_(g_device, g_queue_family, 0, &g_queue);
    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, pool.queueFamilyIndex = g_queue_family;
    vkCreateCommandPool_(g_device, &pool, nullptr, &g_pool);
    VkCommandBufferAllocateInfo cba{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cba.commandPool = g_pool, cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, cba.commandBufferCount = 1;
    vkAllocateCommandBuffers_(g_device, &cba, &g_cmd);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence_(g_device, &fci, nullptr, &g_fence);
    if (!MakeBuffer(g_vertices, kVertexRing, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) ||
        !MakeBuffer(g_uniforms, kUniformRing, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) ||
        !MakeBuffer(g_staging, kStagingRing, VK_BUFFER_USAGE_TRANSFER_SRC_BIT))
        return why = "out of buffer memory", false;
    VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    smi.codeSize = sizeof(nv2a_vert), smi.pCode = nv2a_vert;
    vkCreateShaderModule_(g_device, &smi, nullptr, &g_vert);
    smi.codeSize = sizeof(nv2a_frag), smi.pCode = nv2a_frag;
    vkCreateShaderModule_(g_device, &smi, nullptr, &g_frag);
    VkDescriptorSetLayoutBinding bindings[3]{};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dsl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dsl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    dsl.bindingCount = 3, dsl.pBindings = bindings;
    vkCreateDescriptorSetLayout_(g_device, &dsl, nullptr, &g_set_layout);
    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, 16};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1, pli.pSetLayouts = &g_set_layout;
    pli.pushConstantRangeCount = 1, pli.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout_(g_device, &pli, nullptr, &g_layout);
    // unused texture stages sample these
    MakeImage(g_dummy_2d, kColorFormat, 1, 1, 1, false, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
              VK_IMAGE_ASPECT_COLOR_BIT);
    MakeImage(g_dummy_cube, kColorFormat, 1, 1, 1, true, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
              VK_IMAGE_ASPECT_COLOR_BIT);
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    vkCreateSampler_(g_device, &sci, nullptr, &g_dummy_sampler);
    g_status = std::string("on: ") + props.deviceName;
    return true;
}

bool Ready()
{
    if (!g_tried)
    {
        g_tried = true;
        if (const std::string died = GuardedDeath(); !died.empty())
        {
            g_status = "off: the last run of this build died at \"" + died + "\": software drawing this time";
            Log("gpu: %s", g_status.c_str());
            return false;
        }
        Guard("starting Vulkan (RADV)");
        LogSync("gpu: starting Vulkan (if the app dies here, the next start draws in software)");
        std::string why;
        g_ok = Init(why);
        if (!g_ok)
        {
            g_status = "off: " + why;
            Guard(nullptr);
        }
        else
            Guard("the first frames on the GPU");
        Log("gpu: %s", g_status.c_str());
    }
    return g_ok;
}

void SyncLocked()
{
    if (!g_ok || !g_recording)
    {
        for (auto& s : g_surfaces)
            s->check = true;
        return;
    }
    EndRendering();
    std::vector<GpuSurface*> back;
    for (auto& s : g_surfaces)
        if (s->gpu_newer)
            back.push_back(s.get());
    if (!back.empty())
    {
        Barrier();
        for (GpuSurface* g : back)
        {
            const u32 w = g->s.width, h = g->s.height;
            if (!g->depth)
            {
                VkBufferImageCopy c{};
                c.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                c.imageExtent = {w, h, 1};
                vkCmdCopyImageToBuffer_(g_cmd, g->img.image, VK_IMAGE_LAYOUT_GENERAL, g->readback.buffer, 1, &c);
            }
            else
            {
                VkBufferImageCopy c[2]{};
                c[0].imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
                c[0].imageExtent = {w, h, 1};
                c[1].bufferOffset = VkDeviceSize(w) * h * 4;
                c[1].imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1};
                c[1].imageExtent = {w, h, 1};
                vkCmdCopyImageToBuffer_(g_cmd, g->img.image, VK_IMAGE_LAYOUT_GENERAL, g->readback.buffer, 2, c);
            }
        }
        VkMemoryBarrier m{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        m.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, m.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier_(g_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &m, 0, nullptr, 0, nullptr);
    }
    vkEndCommandBuffer_(g_cmd);
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1, submit.pCommandBuffers = &g_cmd;
    vkQueueSubmit_(g_queue, 1, &submit, g_fence);
    vkWaitForFences_(g_device, 1, &g_fence, VK_TRUE, ~0ull);
    vkResetFences_(g_device, 1, &g_fence);
    vkResetCommandBuffer_(g_cmd, 0);
    g_recording = false;
    for (GpuSurface* g : back)
    {
        static int logged = 0;
        if (std::getenv("XB_GPU_DEBUG") && logged++ < 20)
            Log("gpu: %#x written back (first pixel %#x, middle %#x)", g->s.address, *reinterpret_cast<const u32*>(g->readback.map),
                reinterpret_cast<const u32*>(g->readback.map)[g->s.width * (g->s.height / 2) + g->s.width / 2]);
        WriteBack(*g);
        g->gpu_newer = false;
        g->hash = Hash(g->s.address, g->Bytes());
    }
    for (auto& s : g_surfaces)
        s->check = true;
    for (Image& img : g_trash)
        Free(img);
    g_trash.clear();
    g_vertices.used = g_uniforms.used = g_staging.used = 0;
    g_epoch++;
    static u32 syncs = 0;
    if (g_guard_pending && ++syncs >= 120) // two seconds of frames drawn and written back
    {
        Guard(nullptr);
        Log("gpu: the first frames were drawn on the GPU");
    }
}

struct DepthState
{
    bool test, write, stencil, z_float;
    u32 func;
    float z_max;
    u32 stencil_func, stencil_ref, stencil_mask, stencil_write, op_fail, op_zfail, op_zpass;
};

DepthState ReadDepthState(const Surface& zs)
{
    const KelvinState& k = g_kelvin;
    DepthState d{};
    d.test = k.Get(0x30c), d.write = k.Get(0x35c), d.func = k.Get(0x354);
    d.stencil = k.Get(0x32c) && zs.bytes == 4;
    d.z_float = k.Get(0x290) & (1u << 12);
    d.z_max = zs.bytes == 2 ? 65535.f : 16777215.f;
    d.stencil_func = k.Get(0x364), d.stencil_ref = k.Get(0x368) & 0xff, d.stencil_mask = k.Get(0x36c) & 0xff;
    d.stencil_write = k.Get(0x360) & 0xff;
    d.op_fail = k.Get(0x370), d.op_zfail = k.Get(0x374), d.op_zpass = k.Get(0x378);
    return d;
}

} // namespace

void SetGuardFile(const std::string& path)
{
    std::lock_guard lock(g_mutex);
    g_guard_file = path;
}

void SetWanted(bool on)
{
    std::lock_guard lock(g_mutex);
    g_wanted = on;
}

bool Enabled()
{
    std::lock_guard lock(g_mutex);
    static const bool env = [] {
        const char* e = std::getenv("XB_GPU");
        return e && *e && *e != '0';
    }();
    if (!env && !g_wanted)
        return false;
    return Ready();
}

const char* Status()
{
    return g_status.c_str();
}

bool Declined(int why)
{
    static int logged = 0;
    if (std::getenv("XB_GPU_DEBUG") && logged++ < 20)
        Log("gpu: draw declined (%d)", why);
    return false;
}

bool DrawTriangles(const Vertex* vertices, u32 count)
{
    std::lock_guard lock(g_mutex);
    if (!Ready() || !count)
        return count == 0;
    const Surface cs = ColorSurface(), zs = ZetaSurface();
    if (!cs.address || !cs.width || !cs.height || cs.width > 4096 || cs.height > 4096)
        return Declined(1);
    const bool has_depth = zs.address && zs.width == cs.width && zs.height == cs.height;
    const DepthState depth = ReadDepthState(zs);
    if (has_depth && depth.z_float)
        return Declined(2); // float depth: not yet
    Uniforms u;
    TextureInfo tex[4];
    BlendState blend;
    if (!GpuPixelSetup(u, tex, blend))
        return Declined(3);
    // the textures first (uploads end any rendering), then the surfaces
    VkDescriptorImageInfo images2d[4], cubes[4];
    for (int s = 0; s < 4; s++)
    {
        images2d[s] = {g_dummy_sampler, g_dummy_2d.view, VK_IMAGE_LAYOUT_GENERAL};
        cubes[s] = {g_dummy_sampler, g_dummy_cube.view, VK_IMAGE_LAYOUT_GENERAL};
        if (!tex[s].used)
            continue;
        GpuTexture* t = UseTexture(s, tex[s]);
        if (!t)
            return Declined(4);
        (tex[s].cube ? cubes[s] : images2d[s]) = {Sampler(tex[s]), t->img.view, VK_IMAGE_LAYOUT_GENERAL};
    }
    GpuSurface* color = UseSurface(cs, false, false, 1);
    GpuSurface* zbuf = has_depth ? UseSurface(zs, true, depth.z_float, depth.z_max) : nullptr;
    if (!color || (has_depth && !zbuf))
        return Declined(5);
    const VkDeviceSize vbytes = VkDeviceSize(count) * 3 * sizeof(Vertex);
    VkDeviceSize vat = Take(g_vertices, vbytes, 16), uat = Take(g_uniforms, sizeof(Uniforms), g_ubo_align);
    if (vat == ~VkDeviceSize(0) || uat == ~VkDeviceSize(0))
    {
        SyncLocked();
        color = UseSurface(cs, false, false, 1);
        zbuf = has_depth ? UseSurface(zs, true, depth.z_float, depth.z_max) : nullptr;
        vat = Take(g_vertices, vbytes, 16), uat = Take(g_uniforms, sizeof(Uniforms), g_ubo_align);
        if (!color || vat == ~VkDeviceSize(0) || uat == ~VkDeviceSize(0))
            return Declined(6);
    }
    std::memcpy(g_vertices.map + vat, vertices, vbytes);
    std::memcpy(g_uniforms.map + uat, &u, sizeof(u));
    const KelvinState& k = g_kelvin;
    const u32 m = k.Get(0x358);
    PipelineKey key{};
    key.blend = blend.enabled, key.sfactor = blend.sfactor, key.dfactor = blend.dfactor, key.equation = blend.equation;
    key.has_depth = has_depth, key.depth_test = depth.test, key.depth_write = depth.write, key.depth_func = depth.func;
    key.stencil = has_depth && depth.stencil;
    if (key.stencil)
    {
        key.stencil_func = depth.stencil_func, key.stencil_ref = depth.stencil_ref, key.stencil_mask = depth.stencil_mask;
        key.stencil_write = depth.stencil_write, key.op_fail = depth.op_fail, key.op_zfail = depth.op_zfail;
        key.op_zpass = depth.op_zpass;
    }
    key.write_mask = m ? ((m & 0x00010000 ? VK_COLOR_COMPONENT_R_BIT : 0) | (m & 0x00000100 ? VK_COLOR_COMPONENT_G_BIT : 0) |
                          (m & 0x00000001 ? VK_COLOR_COMPONENT_B_BIT : 0) | (m & 0x01000000 ? VK_COLOR_COMPONENT_A_BIT : 0))
                       : 0xf;
    const VkPipeline pipeline = Pipeline(key);
    if (!pipeline)
        return Declined(7);
    BeginRendering(color, zbuf);
    vkCmdBindPipeline_(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    const VkViewport vp{0, 0, float(cs.width), float(cs.height), 0, 1};
    vkCmdSetViewport_(g_cmd, 0, 1, &vp);
    u32 ax, ay;
    AntiAliasing(ax, ay);
    // the surface's clip origin, within the window clip's rectangle
    int wx0, wy0, wx1, wy1;
    WindowClipRect(wx0, wy0, wx1, wy1);
    const u32 x0 = std::min(std::max((k.Get(0x200) & 0xffff) * ax, u32(std::max(wx0, 0))), cs.width);
    const u32 y0 = std::min(std::max((k.Get(0x204) & 0xffff) * ay, u32(std::max(wy0, 0))), cs.height);
    const u32 x1 = std::max(x0, std::min(cs.width, u32(std::max(wx1, 0)))), y1 = std::max(y0, std::min(cs.height, u32(std::max(wy1, 0))));
    const VkRect2D scissor{{int32_t(x0), int32_t(y0)}, {x1 - x0, y1 - y0}};
    vkCmdSetScissor_(g_cmd, 0, 1, &scissor);
    const float bc[4] = {((blend.color >> 16) & 255) / 255.f, ((blend.color >> 8) & 255) / 255.f, (blend.color & 255) / 255.f,
                         (blend.color >> 24) / 255.f};
    vkCmdSetBlendConstants_(g_cmd, bc);
    const float scale[4] = {2.f / float(cs.width), 2.f / float(cs.height), has_depth ? 1.f / depth.z_max : 0.f, 0};
    vkCmdPushConstants_(g_cmd, g_layout, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, scale);
    VkDescriptorBufferInfo ubo{g_uniforms.buffer, uat, sizeof(Uniforms)};
    VkWriteDescriptorSet writes[3]{};
    for (auto& w : writes)
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstBinding = 0, writes[0].descriptorCount = 1, writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].pBufferInfo = &ubo;
    writes[1].dstBinding = 1, writes[1].descriptorCount = 4, writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = images2d;
    writes[2].dstBinding = 2, writes[2].descriptorCount = 4, writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo = cubes;
    vkCmdPushDescriptorSetKHR_(g_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_layout, 0, 3, writes);
    vkCmdBindVertexBuffers_(g_cmd, 0, 1, &g_vertices.buffer, &vat);
    vkCmdDraw_(g_cmd, count * 3, 1, 0, 0);
    static int logged = 0;
    if (std::getenv("XB_GPU_DEBUG") && logged++ < 20)
        Log("gpu: draw %u triangles into %#x (depth %#x), first vertex %g %g %g %g colour %g %g %g %g", count, cs.address,
            has_depth ? zs.address : 0, vertices[0].pos[0], vertices[0].pos[1], vertices[0].pos[2], vertices[0].pos[3],
            vertices[0].diffuse[0], vertices[0].diffuse[1], vertices[0].diffuse[2], vertices[0].diffuse[3]);
    color->gpu_newer = true;
    if (zbuf && (depth.write || key.stencil))
        zbuf->gpu_newer = true;
    return true;
}

bool Clear(u32 flags)
{
    std::lock_guard lock(g_mutex);
    if (!Ready())
        return false;
    const KelvinState& k = g_kelvin;
    const Surface cs = ColorSurface(), zs = ZetaSurface();
    const bool color_part = flags & 0xf0, depth_part = flags & 0x3;
    if (color_part && ((flags & 0xf0) != 0xf0 || cs.bytes != 4))
        return false; // some channels only, 16-bit colour: the software path
    if (!cs.address || !cs.width || !cs.height)
        return false;
    const bool has_depth = zs.address && zs.width == cs.width && zs.height == cs.height;
    if (depth_part && (!has_depth || (k.Get(0x290) & (1u << 12)) || (zs.bytes == 4 && (flags & 3) != 3)))
        return false;
    GpuSurface* color = UseSurface(cs, false, false, 1);
    GpuSurface* zbuf = has_depth ? UseSurface(zs, true, false, zs.bytes == 2 ? 65535.f : 16777215.f) : nullptr;
    if (!color || (has_depth && !zbuf))
        return false;
    u32 ax, ay;
    AntiAliasing(ax, ay);
    const u32 h = k.Get(0x1d98), v = k.Get(0x1d9c);
    const u32 x0 = (h & 0xffff) * ax, x1 = std::min(((h >> 16) + 1) * ax, cs.width);
    const u32 y0 = (v & 0xffff) * ay, y1 = std::min(((v >> 16) + 1) * ay, cs.height);
    if (x1 <= x0 || y1 <= y0)
        return true;
    BeginRendering(color, zbuf);
    VkClearAttachment att[2]{};
    u32 n = 0;
    if (color_part)
    {
        const u32 c = FixedBits(cs.format, k.Get(0x1d90));
        att[n].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        att[n].clearValue.color = {{((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, (c >> 24) / 255.f}};
        n++;
        color->gpu_newer = true;
    }
    if (depth_part)
    {
        const u32 value = k.Get(0x1d8c);
        att[n].aspectMask = (flags & 1 ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (flags & 2 ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        if (zs.bytes == 2)
            att[n].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        att[n].clearValue.depthStencil = {zs.bytes == 2 ? float(value & 0xffff) / 65535.f : float(value >> 8) / 16777215.f,
                                          zs.bytes == 2 ? 0u : value & 0xff};
        n++;
        zbuf->gpu_newer = true;
    }
    VkClearRect rect{{{int32_t(x0), int32_t(y0)}, {x1 - x0, y1 - y0}}, 0, 1};
    vkCmdClearAttachments_(g_cmd, n, att, 1, &rect);
    return true;
}

void Sync()
{
    std::lock_guard lock(g_mutex);
    SyncLocked();
}

void SyncRange(u32 address, u32 size)
{
    std::lock_guard lock(g_mutex);
    for (auto& s : g_surfaces)
        if (s->gpu_newer && Overlaps(s->s.address, s->Bytes(), address, size))
        {
            SyncLocked();
            return;
        }
}

} // namespace xb::gpu

#endif // XB_NO_VULKAN
