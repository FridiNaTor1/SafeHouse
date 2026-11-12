// Sly 2's OpenGL on Vulkan: see gl_vulkan.h.
//
// Design (after the Switch's gl_deko.cpp, which this follows; performance first, the CPU records every frame):
// - GL objects live in dense tables indexed by their names. GPU memory is suballocated from large blocks: every GL
//   buffer is a range of a block's one VkBuffer in host-visible memory (device-local where the driver offers it), so a
//   buffer never needs its own Vulkan object. Deleted objects go when the frame that last could use them has retired.
// - Stream buffers (GL_STREAM_*, the game's per-frame uploads) live in the frame's ring: glBufferData orphans by taking
//   new ring memory; glBufferSubData into memory no command has read yet writes in place, else copies on write.
// - Every texture and sampler has a slot for its life in one descriptor set (descriptor indexing, update-after-bind),
//   bound once per command buffer: a program's sampler is a pair of slot indices in its push constants, so changing a
//   texture costs a few pushed bytes, never a descriptor update. The loose uniforms are push constants too; the storage
//   buffers are push descriptors (set 1), pushed when a binding changes.
// - Pipelines are created on first use for the GL state that needs them (program, vertex layout, primitive, blend,
//   colour mask, target formats) and kept; everything else (viewport, scissor, depth / stencil, cull, front face, line
//   width, blend constant) is dynamic state set only when it changed. The driver's pipeline cache is kept on disk.
// - Rendering uses dynamic rendering: a render-pass instance begins lazily at the first draw after a target change and
//   ends at the next change, transfer or read-back. A clear before the first draw becomes the instance's load
//   operation. Images keep one tracked layout; the attachments and transfer images go back to SHADER_READ_ONLY at the
//   next instance's start (one barrier batch), so a texture draws are allowed to sample is always readable.
// - Uploads into objects the frame's commands have not used yet (new textures, last frame's buffers) are recorded into
//   a command buffer submitted ahead of the frame's own (one barrier batch each way), so they never split a render
//   pass; uploads into objects already used this frame are copied in order (the render pass ends, as GL drivers do).
// - glMultiDrawElementsIndirect is the device's multi-draw-indirect (the game's runs of draws: one command each).
// - Two frames in flight (a timeline semaphore); the swapchain image is acquired at the present: the window's picture
//   (framebuffer 0) is an offscreen image, copied flipped into it.
#include "gl_vulkan.h"
#include "vk_loader.h"
#include "vk_shaders.h"

#include <GLFW/glfw3.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace CaneVk
{
namespace
{
using namespace cane::vk;

// ------------------------------------------------------------------------------------------------ constants
constexpr unsigned kFrameSlots = 2;          // frames in flight: the CPU runs at most this many frames ahead
constexpr uint32_t kMaxImages = 16384, kMaxArrays = 1024, kMaxSamplers = 256;
constexpr VkDeviceSize kBufferBlockSize = 64ull << 20, kImageBlockSize = 128ull << 20, kRingChunkSize = 8ull << 20;
constexpr unsigned kMaxAttribs = 16, kGenericBinding = 15, kMaxUnits = 32, kMaxBindings = 16;
constexpr unsigned kFetchBinding = kMaxBindings;   // (set 1: the in-place read's input attachment, after the storage buffers)
constexpr GLenum kAnisotropyEnum = 0x84FE, kMaxAnisotropyEnum = 0x84FF;   // (GL 4.6 / EXT_texture_filter_anisotropic)
constexpr uint32_t kPushBytes = 256;

VkDeviceSize Align(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

void (*g_onFatal)(const char* why) = nullptr;   // (Options::onFatal)
[[noreturn]] void Fatal(const char* fmt, ...)
{
    char sz[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(sz, sizeof(sz), fmt, args);
    va_end(args);
    std::fprintf(stderr, "Vulkan: %s\n", sz);
    std::fflush(stderr);
    if (auto onFatal = g_onFatal)
    {
        g_onFatal = nullptr;   // (once, also when the hook itself fails)
        onFatal(sz);
    }
    std::abort();
}
// (tests: SLY2N_VKFAIL=<stage>[,<stage>...] makes a stage fail as a driver would: feature (no device has a required
// feature), oldloader / olddriver (a loader / driver older than Vulkan 1.3), device (vkCreateDevice), swapchain, program (no program compiled for Vulkan), pipeline (the first pipeline
// creation), devicelost (the first submission))
bool FForcedFailure(const char* stage)
{
    static const std::string s_list = [] {
        const char* sz = std::getenv("SLY2N_VKFAIL");
        return sz && *sz ? "," + std::string(sz) + "," : std::string();
    }();
    return !s_list.empty() && s_list.find("," + std::string(stage) + ",") != std::string::npos;
}
#define VKCHECK(call)                                                        \
    do {                                                                     \
        const VkResult r_ = (call);                                          \
        if (r_ != VK_SUCCESS) Fatal("%s failed (%d)", #call, int(r_));       \
    } while (0)

// ------------------------------------------------------------------------------------------------ device
VkInstance g_instance = VK_NULL_HANDLE;
VkDebugUtilsMessengerEXT g_messenger = VK_NULL_HANDLE;
VkPhysicalDevice g_phys = VK_NULL_HANDLE;
VkDevice g_device = VK_NULL_HANDLE;
VkQueue g_queue = VK_NULL_HANDLE;
uint32_t g_queueFamily = 0;
VkPhysicalDeviceProperties g_props{};
VkPhysicalDeviceMemoryProperties g_memProps{};
std::string g_deviceName;
struct Features
{
    bool dualSrcBlend = false;
    bool depthBounds = false;      // (GL_EXT_depth_bounds_test: glEnable(GL_DEPTH_BOUNDS_TEST_EXT), glDepthBoundsEXT)
    bool msrtss = false;   // (VK_EXT_multisampled_render_to_single_sampled, Android: MSAA resolved on the tile)
    bool fdm = false;      // (VK_EXT_fragment_density_map with non-subsampled images: VR's fixed foveation)
    VkExtent2D fdmTexel{32, 32};   // (GL_SRC1_* factors: Sly 2's glob CANE_BLEND1 on Android)
    bool geometry = false, wideLines = false, anisotropy = false, mdi = false, firstInstance = false, nonUniform = false;
    bool fillModeNonSolid = false;
    bool provokingLast = false;
    bool eds3 = false;
    bool float16 = false;           // (shaderFloat16: the SLY2N_VKFP16 experiment)
    bool demote = false;            // (VK_EXT_extended_dynamic_state3: blend and colour mask dynamic, fewer pipelines)   // (VK_EXT_provoking_vertex: GL's flat outputs come from a primitive's last vertex)
    bool pushDescriptors = true;    // (VK_KHR_push_descriptor: the storage buffers pushed; else per-frame descriptor sets)
    uint32_t pushBytes = 256;       // the push-constant bytes used (a program with a larger block reads it from set 2)
    bool classicFetch = false;      // (no local read: the in-place read in a render pass object's subpass, the colour also its
                                    // input attachment with a by-region self-dependency: core Vulkan; SLY2N_VKCLASSICFETCH=0: off)
    bool localRead = false;         // (VK_KHR_dynamic_rendering_local_read: the colour attachment read in place, FramebufferFetchUsable)
    bool multiview = false;         // (VK_KHR_multiview, core 1.1: VR's eyes as the two views of one pass; MultiviewSupported)
    bool multiviewGeometry = false; // (multiviewGeometryShader: the near-clip geometry stage in such a pass)
    float lineWidthMax = 1.0f, anisotropyMax = 1.0f;
    VkFormat depthStencil = VK_FORMAT_D32_SFLOAT_S8_UINT, depth24 = VK_FORMAT_D24_UNORM_S8_UINT;
    VkSampleCountFlags samples = VK_SAMPLE_COUNT_1_BIT;
} g_feat;
GLFWwindow* g_window = nullptr;
bool g_visible = false;
std::string g_cachePath;
VkPipelineCache g_pipelineCache = VK_NULL_HANDLE;
VkSemaphore g_timeline = VK_NULL_HANDLE;
uint64_t g_submitValue = 0;            // the timeline value of the last submission
Stats g_stats;

// ------------------------------------------------------------------------------------------------ GPU memory
class RangeAllocator
{
public:
    explicit RangeAllocator(VkDeviceSize size = 0) { if (size) m_free.emplace(0, size); }
    bool Allocate(VkDeviceSize size, VkDeviceSize alignment, VkDeviceSize* offset)
    {
        for (auto it = m_free.begin(); it != m_free.end(); ++it)
        {
            const VkDeviceSize start = Align(it->first, alignment), end = start + size;
            if (end > it->first + it->second) continue;
            const VkDeviceSize oldStart = it->first, oldSize = it->second;
            m_free.erase(it);
            if (start > oldStart) m_free.emplace(oldStart, start - oldStart);
            if (end < oldStart + oldSize) m_free.emplace(end, oldStart + oldSize - end);
            *offset = start;
            return true;
        }
        return false;
    }
    void Release(VkDeviceSize start, VkDeviceSize size)
    {
        auto next = m_free.lower_bound(start);
        if (next != m_free.begin())
        {
            auto prev = std::prev(next);
            if (prev->first + prev->second == start) { start = prev->first; size += prev->second; m_free.erase(prev); }
        }
        if (next != m_free.end() && start + size == next->first) { size += next->second; m_free.erase(next); }
        m_free.emplace(start, size);
    }
private:
    std::map<VkDeviceSize, VkDeviceSize> m_free;
};

class Heap;
struct Block
{
    Heap* heap = nullptr;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;   // (buffer heaps: one buffer over the whole block)
    uint8_t* cpu = nullptr;
    VkDeviceSize size = 0, used = 0;
    RangeAllocator ranges;
};
struct Alloc
{
    Block* block = nullptr;
    VkDeviceSize offset = 0, size = 0;
    explicit operator bool() const { return block != nullptr; }
    uint8_t* Cpu() const { return block->cpu ? block->cpu + offset : nullptr; }
    VkBuffer Buffer() const { return block->buffer; }
};
constexpr VkBufferUsageFlags kBufferUsage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
class Heap
{
public:
    Heap(uint32_t typeIndex, VkDeviceSize blockSize, bool buffers)
        : m_type(typeIndex), m_blockSize(blockSize), m_buffers(buffers),
          m_mapped((g_memProps.memoryTypes[typeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0) {}
    bool Allocate(VkDeviceSize size, VkDeviceSize alignment, Alloc* out)
    {
        size = Align(std::max<VkDeviceSize>(size, 1), 64);
        alignment = std::max<VkDeviceSize>(alignment, 64);
        // (buffer heaps: the size rounded up to the alignment, so consecutive allocations leave no gap below the next
        // aligned start. Every buffer allocation is 256-aligned, and such a gap could never hold one: the offsets are the
        // same as before, but the free list no longer collects one dead gap per allocation, which the first-fit scan
        // below walked on every allocation (a streamed world part's ~700 buffers: ~3 ms))
        if (m_buffers) size = Align(size, alignment);
        for (auto& b : m_blocks)
        {
            VkDeviceSize offset;
            if (b->size - b->used >= size && b->ranges.Allocate(size, alignment, &offset))
            {
                b->used += size;
                *out = {b.get(), offset, size};
                return true;
            }
        }
        auto b = std::make_unique<Block>();
        b->heap = this;
        b->size = std::max(m_blockSize, Align(size, 4096));
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = b->size;
        ai.memoryTypeIndex = m_type;
        if (vkAllocateMemory(g_device, &ai, nullptr, &b->memory) != VK_SUCCESS) return false;
        if (m_buffers)
        {
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = b->size;
            bi.usage = kBufferUsage;
            VKCHECK(vkCreateBuffer(g_device, &bi, nullptr, &b->buffer));
            VKCHECK(vkBindBufferMemory(g_device, b->buffer, b->memory, 0));
        }
        if (m_mapped)
        {
            void* p = nullptr;
            VKCHECK(vkMapMemory(g_device, b->memory, 0, VK_WHOLE_SIZE, 0, &p));
            b->cpu = static_cast<uint8_t*>(p);
        }
        b->ranges = RangeAllocator(b->size);
        VkDeviceSize offset = 0;
        b->ranges.Allocate(size, alignment, &offset);
        b->used = size;
        m_blocks.push_back(std::move(b));
        *out = {m_blocks.back().get(), offset, size};
        return true;
    }
    void Free(const Alloc& a)
    {
        if (!a) return;
        a.block->ranges.Release(a.offset, a.size);
        a.block->used -= a.size;
    }
    uint32_t Type() const { return m_type; }
    void Destroy()
    {
        for (auto& b : m_blocks)
        {
            if (b->buffer) vkDestroyBuffer(g_device, b->buffer, nullptr);
            vkFreeMemory(g_device, b->memory, nullptr);
        }
        m_blocks.clear();
    }
private:
    uint32_t m_type;
    VkDeviceSize m_blockSize;
    bool m_buffers, m_mapped;
    std::vector<std::unique_ptr<Block>> m_blocks;
};

std::unique_ptr<Heap> g_bufferHeap, g_bufferHeapFallback, g_readbackHeap;
std::map<uint32_t, std::unique_ptr<Heap>> g_imageHeaps;

int FindMemoryType(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < g_memProps.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (g_memProps.memoryTypes[i].propertyFlags & want) == want) return int(i);
    return -1;
}
Alloc AllocBuffer(VkDeviceSize size, VkDeviceSize alignment = 256)
{
    Alloc a;
    if (g_bufferHeap->Allocate(size, alignment, &a)) return a;
    if (g_bufferHeapFallback && g_bufferHeapFallback->Allocate(size, alignment, &a)) return a;
    Fatal("out of buffer memory (%llu bytes)", static_cast<unsigned long long>(size));
}
void FreeAlloc(const Alloc& a) { if (a) a.block->heap->Free(a); }

// ------------------------------------------------------------------------------------------------ objects
template <class T> class Table
{
public:
    Table() { m_objects.emplace_back(); }   // (name 0)
    GLuint Gen()
    {
        GLuint name;
        if (!m_free.empty()) { name = m_free.back(); m_free.pop_back(); }
        else { name = GLuint(m_objects.size()); m_objects.emplace_back(); }
        m_objects[name] = std::make_unique<T>();
        return name;
    }
    T* Get(GLuint name) const { return name < m_objects.size() ? m_objects[name].get() : nullptr; }
    GLuint Limit() const { return GLuint(m_objects.size()); }   // (names below this: Get them to walk the table)
    GLuint Adopt(std::unique_ptr<T> object)
    {
        const GLuint name = Gen();
        m_objects[name] = std::move(object);
        return name;
    }
    T* Default() { if (!m_objects[0]) m_objects[0] = std::make_unique<T>(); return m_objects[0].get(); }
    T* Ensure(GLuint name)
    {
        if (!name) return nullptr;
        if (name >= m_objects.size()) m_objects.resize(name + 1);
        if (!m_objects[name])
        {
            m_objects[name] = std::make_unique<T>();
            m_free.erase(std::remove(m_free.begin(), m_free.end(), name), m_free.end());
        }
        return m_objects[name].get();
    }
    std::unique_ptr<T> Take(GLuint name)
    {
        if (!name || name >= m_objects.size() || !m_objects[name]) return nullptr;
        auto p = std::move(m_objects[name]);
        m_free.push_back(name);
        return p;
    }
    template <class F> void ForEach(F f) { for (size_t i = 1; i < m_objects.size(); ++i) if (m_objects[i]) f(GLuint(i), *m_objects[i]); }
    void Clear() { m_objects.clear(); m_objects.emplace_back(); m_free.clear(); }
private:
    std::vector<std::unique_ptr<T>> m_objects;
    std::vector<GLuint> m_free;
};

uint64_t g_imageIds = 0;
struct Image
{
    // (the colour resolve once a frame, BlitFramebuffer: which image, and a count of the commands recorded that may
    // change its contents: render-pass instances it is attached to, transfers into it, external acquires)
    const uint64_t id = ++g_imageIds;
    uint64_t writes = 0;
    VkImage image = VK_NULL_HANDLE;
    Alloc memory;
    VkImageView view = VK_NULL_HANDLE;                       // the sampled view (all levels and layers)
    std::vector<std::pair<uint32_t, VkImageView>> attachViews;   // (level | layer << 8 | layers << 24: more than one: an array view)
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t width = 0, height = 0, layers = 1, levels = 1, samples = 1;
    VkImageUsageFlags usage = 0;   // (as created: an imageless framebuffer names it)
    bool array = false, depth = false, sampled = false, immutable = false;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    int slot = -1;                 // the descriptor slot (binding 0: 2D, binding 2: arrays)
    uint64_t useSegment = 0;       // the last submission whose main commands use the image
    uint64_t xferBatch = 0;        // the transfer batch that last took it (Xfers)
    bool dirty = false;            // in g_dirty (left in an attachment or transfer layout)
    bool resident = false;         // a bindless handle names it (its uses are not seen per draw)
    bool inputAttachment = false;  // (g_feat.localRead) single-sampled colour with input-attachment use: readable in place
    bool external = false;         // (VR) an OpenXR swapchain image: not created, allocated or destroyed here
    bool transient = false;
    uint8_t renderSamples = 1;     // (msrtss) a single-sampled image drawn with this many samples, resolved on the tile
    bool fdm = false;              // a fragment density map (VR's fixed foveation: SetDensityMap)
    Image* densityMap = nullptr;   // drawn into with this density map (its passes shade the edges more coarsely)        // (VR, SetRenderbufferTransient) depth / stencil never read after a pass: not stored
};
struct Buffer
{
    uint32_t size = 0;
    bool stream = false;           // transient storage (GL_STREAM_*)
    Alloc storage;                 // persistent storage
    VkBuffer vk = VK_NULL_HANDLE;  // the storage's buffer and offset (persistent or transient)
    VkDeviceSize offset = 0;
    uint8_t* cpu = nullptr;
    uint64_t serial = 0;           // the frame of a transient allocation
    uint64_t useSegment = 0;       // the last submission whose main commands read the storage
    bool bound = false;            // a recorded command may read the current storage
    std::vector<uint8_t> shadow;   // a CPU copy (indirect command buffers without multi-draw-indirect)
    bool keepShadow = false;
    uint32_t version = 1;
};
struct Attrib
{
    GLuint buffer = 0;
    uint32_t offset = 0, stride = 0, divisor = 0;
    uint8_t components = 4;
    GLenum type = GL_FLOAT;
    bool enabled = false, normalized = false, integer = false;
};
struct VertexArray
{
    std::array<Attrib, kMaxAttribs> attribs{};
    GLuint elements = 0;
    uint32_t version = 1;
};
struct SamplerParams
{
    GLenum minFilter = GL_NEAREST_MIPMAP_LINEAR, magFilter = GL_LINEAR, wrapS = GL_REPEAT, wrapT = GL_REPEAT;
    float anisotropy = 1.0f;
    int maxLevel = 1000;
    bool operator==(const SamplerParams&) const = default;
};
struct Texture
{
    std::unique_ptr<Image> image;
    SamplerParams params;
    int samplerSlot = -1;
};
struct Sampler
{
    SamplerParams params;
    int samplerSlot = -1;
};
struct Renderbuffer
{
    std::unique_ptr<Image> image;
};
struct Attachment
{
    GLuint object = 0;
    bool renderbuffer = false;
    uint16_t layer = 0;
    uint8_t level = 0;
    uint8_t views = 0;             // (VR multiview, FramebufferMultiview) 2: layers layer, layer + 1 as a pass's two views
};
struct Framebuffer
{
    Attachment color, depth;
    bool drawColor = true;
};
struct Program
{
    const Shaders::Program* info = nullptr;
    bool fetch = false;            // (CANE_FETCH) reads the colour attachment in place (subpassInput, set 1 binding kFetchBinding)
    uint32_t serial = 0;           // (pipeline keys: unique per program object)
    std::array<VkShaderModule, 3> modules{};
    std::array<VkShaderModule, 3> modulesMv{};   // (two-view passes: the stages made for them, else modules)
    std::vector<uint8_t> constants;
    uint32_t dirtyLo = UINT32_MAX, dirtyHi = 0;
    std::array<uint8_t, kMaxBindings> samplerUnit{};
    std::array<uint8_t, kMaxBindings> samplerArray{};
    unsigned samplerCount = 0;
    uint16_t storageMask = 0, storageMaskMv = 0;
    bool uniformBuffer = false;    // its block from a uniform buffer (set 2) instead of push constants (g_feat.pushBytes)
    std::array<uint16_t, 8> specOffset{};   // (per-state specialization) the SPEC uniforms' block offsets (info->specMask)
    uint32_t hash = 0;             // (prewarm manifest: the build-time key, the block's source and the SPIR-V)
};
struct Query
{
    uint32_t index = 0;            // its two timestamps' first
    uint64_t value = 0;            // the submission that writes the second
};
struct Sync
{
    uint64_t value = 0;
};

Table<Buffer> g_buffers;
Table<VertexArray> g_arrays;
Table<Texture> g_textures;
Table<Sampler> g_samplers;
Table<Renderbuffer> g_renderbuffers;
Table<Framebuffer> g_framebuffers;
Table<Program> g_programs;
Table<Query> g_queries;
std::vector<std::unique_ptr<Sync>> g_syncs;
uint32_t g_programSerial = 0;
std::vector<VkShaderModule> g_retiredModules;
std::vector<std::unique_ptr<Program>> g_prePrograms;   // (made by the prewarm before the game asked for them)
std::unique_ptr<Program> MakeProgram(const Shaders::Program* info, bool uniformBuffer);

// ------------------------------------------------------------------------------------------------ GL state
struct Range { GLuint buffer = 0; uint32_t offset = 0, size = 0; bool whole = true; };
struct State
{
    GLuint arrayBuffer = 0, uniformBuffer = 0, storageBuffer = 0, indirectBuffer = 0, copyRead = 0, copyWrite = 0, pixelUnpack = 0, pixelPack = 0;
    GLuint vertexArray = 0, program = 0, renderbuffer = 0, drawFramebuffer = 0, readFramebuffer = 0;
    unsigned activeUnit = 0;
    std::array<std::array<GLuint, 2>, kMaxUnits> units{};     // [unit][0: 2D, 1: 2D array]
    std::array<GLuint, kMaxUnits> samplerUnits{};
    std::array<Range, kMaxBindings> storage{};
    bool depthTest = false, blend = false, cull = false, stencilTest = false, scissorTest = false;
    bool depthBoundsTest = false;
    float depthBoundsMin = 0.0f, depthBoundsMax = 1.0f;
    bool depthWrite = true;
    GLenum depthFunc = GL_LESS, frontFace = GL_CCW, cullFace = GL_BACK;
    GLenum blendSrc = GL_ONE, blendDst = GL_ZERO, blendSrcAlpha = GL_ONE, blendDstAlpha = GL_ZERO;
    GLenum blendEq = GL_FUNC_ADD, blendEqAlpha = GL_FUNC_ADD;
    std::array<float, 4> blendColor{};
    struct Face { GLenum func = GL_ALWAYS, fail = GL_KEEP, zfail = GL_KEEP, zpass = GL_KEEP; GLint ref = 0; GLuint mask = 0xFF, writeMask = 0xFF; };
    Face front, back;
    unsigned colorMask = 0xF;
    std::array<float, 4> clearColor{};
    float clearDepth = 1.0f;
    GLint clearStencil = 0;
    GLint viewport[4]{}, scissor[4]{};
    float depthNear = 0.0f, depthFar = 1.0f;
    float lineWidth = 1.0f;
    GLenum polygonMode = GL_FILL;
    GLint unpackAlignment = 4, unpackRowLength = 0, packAlignment = 4;
    GLenum error = GL_NO_ERROR;
};
State s;
// (draw packets) the fixed-function group of the packet state whose values s holds (0: none known): every GL call that
// changes one of those fields forgets it
uint32_t g_fixedPacket = 0;

// What the command buffer has been told (reset with each command buffer).
struct Bound
{
    bool targetDirty = true, viewportDirty = true, rasterDirty = true, depthDirty = true, stencilDirty = true;
    bool blendConstDirty = true, lineDirty = true, pipelineDirty = true, blendDirty = true;
    // the target
    bool hasColor = false, hasDepth = false;
    uint32_t width = 0, height = 0, samples = 1;
    bool msrtss = false;   // (the target's images are drawn multisampled into single-sampled storage)
    bool classic = false;  // (the instance is a render pass object: the pipelines made for it, g_feat.classicFetch)
    bool fdm = false;      // (the target is drawn with a fragment density map: the pipelines made for it)
    bool multiview = false;  // (the target's attachments are two views: the pipelines made for the view mask, the programs' two-view stages)
    uint8_t colorFormat = 0, depthFormat = 0;   // (pipeline keys: 0 none, 1 RGBA8 / the depth format)
    // the program and its resources
    GLuint program = ~0u;
    uint32_t programSerial = 0;
    uint16_t spec = 0;             // (the bound pipeline's spec set)
    VkPipeline pipeline = VK_NULL_HANDLE;
    GLenum mode = ~0u;
    // vertex input
    const VertexArray* vao = nullptr;
    uint32_t vaoVersion = 0, vaoProgram = 0;
    uint16_t layout = 0;
    std::array<VkBuffer, kMaxAttribs> vertexBuffers{};
    std::array<VkDeviceSize, kMaxAttribs> vertexOffsets{};
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceSize indexOffset = 0;
    VkIndexType indexType = VK_INDEX_TYPE_UINT32;
    bool genericBound = false;
    GLuint storageProgram = ~0u;
    bool storageMv = false;
    uint64_t storageEpoch = 0, storageAddrEpoch = 0;
    const VertexArray* vertexVao = nullptr;
    uint32_t vertexVaoVersion = 0;
    uint64_t vertexAddrEpoch = 0;
    uint16_t vertexLayout = 0xFFFF;
};
Bound b;
uint64_t g_addrEpoch = 1, g_storageEpoch = 1;
std::array<uint8_t, 256> g_pushShadow{};   // the push constants the command buffer holds (their first g_pushValid bytes)
uint32_t g_pushValid = 0;
void InvalidateBound() { b = Bound{}; g_pushValid = 0; }

void SetError(GLenum error) { if (s.error == GL_NO_ERROR) s.error = error; }
// The blend state or colour mask changed: dynamic state with extended_dynamic_state3, else a pipeline's.
void BlendChanged() { if (g_feat.eds3) b.blendDirty = true; else b.pipelineDirty = true; }

// ------------------------------------------------------------------------------------------------ resource log
// SLY2N_VKRESLOG=1: the game thread's time in resource creation / upload calls (GL-style and native alike): before the
// first frame was presented ("load") and after it (each frame over 1 ms printed: a mid-game hitch), summed at the exit.
struct ResLog
{
    double ms = 0, bufferMs = 0;
    uint64_t bytes = 0, bufferBytes = 0;
    uint32_t textures = 0, uploads = 0, buffers = 0;
    void Add(const ResLog& o)
    {
        ms += o.ms; bytes += o.bytes; textures += o.textures; uploads += o.uploads; buffers += o.buffers;
        bufferMs += o.bufferMs; bufferBytes += o.bufferBytes;
    }
};
const bool g_resLogOn = std::getenv("SLY2N_VKRESLOG") && std::atoi(std::getenv("SLY2N_VKRESLOG")) != 0;
ResLog g_resFrame, g_resLoad, g_resAfter;
double g_resMaxFrame = 0;
uint64_t g_resMaxFrameSerial = 0, g_resFrames = 0;
bool g_resLoaded = false;
const std::chrono::steady_clock::time_point g_resStart = std::chrono::steady_clock::now();
struct ResTime
{
    std::chrono::steady_clock::time_point t0;
    int depth = 0;
    bool on;
    static int& Depth() { static int d = 0; return d; }
    explicit ResTime(bool count = true) : on(g_resLogOn && count) { if (on) { depth = Depth()++; t0 = std::chrono::steady_clock::now(); } }
    ~ResTime()
    {
        if (!on) return;
        --Depth();
        if (depth == 0) g_resFrame.ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
};
// (at each Present: the frame's share)
void ResLogFrame()
{
    if (!g_resLogOn) return;
    if (!g_resLoaded)
    {
        g_resLoad.Add(g_resFrame);
        g_resLoaded = true;
        std::fprintf(stderr, "vkres: load: %u textures, %u uploads (%.1f MB), %u buffers (%.1f MB, %.1f ms): %.1f ms in resource calls; first frame "
                     "presented %.1f ms into the run\n", g_resLoad.textures, g_resLoad.uploads, g_resLoad.bytes / 1048576.0, g_resLoad.buffers,
                     g_resLoad.bufferBytes / 1048576.0, g_resLoad.bufferMs, g_resLoad.ms,
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - g_resStart).count());
    }
    else
    {
        if (g_resFrame.ms > 1.0)
            std::fprintf(stderr, "vkres: frame %llu: %u textures, %u uploads (%.2f MB), %u buffers (%.2f MB, %.2f ms): %.2f ms\n",
                         (unsigned long long)g_resFrames, g_resFrame.textures, g_resFrame.uploads, g_resFrame.bytes / 1048576.0, g_resFrame.buffers,
                         g_resFrame.bufferBytes / 1048576.0, g_resFrame.bufferMs, g_resFrame.ms);
        if (g_resFrame.ms > g_resMaxFrame) g_resMaxFrame = g_resFrame.ms, g_resMaxFrameSerial = g_resFrames;
        g_resAfter.Add(g_resFrame);
    }
    ++g_resFrames;
    g_resFrame = ResLog{};
}

// ------------------------------------------------------------------------------------------------ frames
struct RingChunk { Alloc alloc; VkDeviceSize cursor = 0; };
struct Frame
{
    VkCommandPool pool = VK_NULL_HANDLE;
    std::vector<VkCommandBuffer> cmds;
    size_t cmdNext = 0;
    std::vector<RingChunk> ring;
    size_t ringIndex = 0;
    uint64_t lastValue = 0;        // the timeline value of the frame's last submission
    uint64_t serial = 0;
    std::vector<Alloc> garbage;
    std::vector<std::unique_ptr<Image>> garbageImages;
    std::vector<uint32_t> garbageSlots2D, garbageSlotsArray;
    std::vector<GLuint> streamBuffers;
    VkSemaphore acquire = VK_NULL_HANDLE;
    // (no push descriptors / few push-constant bytes) the frame's descriptor sets: storage buffers (set 1) and the
    // uniform-buffer sets of set 2 (one per ring buffer), from pools reset when the frame retires
    std::vector<VkDescriptorPool> setPools;
    size_t setPoolIndex = 0;
    std::vector<std::pair<VkBuffer, VkDescriptorSet>> uniformSets;
    // (SLY2N_VKGPUTIME) the render-pass instances timed in this frame: their sizes and attachments
    uint32_t passTimed = 0;
    std::vector<std::array<uint32_t, 3>> passInfo;
    // (SLY2N_VKGPUMARKS) the marks written in this frame
    std::vector<const char*> markLabels;
};
std::array<Frame, kFrameSlots> g_frames;
unsigned g_frameIndex = 0;
uint64_t g_nextSerial = 1;
bool g_recording = false;
uint64_t g_segment = 1;            // the submission being recorded (Image / Buffer useSegment)
VkCommandBuffer g_main = VK_NULL_HANDLE, g_pre = VK_NULL_HANDLE;
Frame& Cur() { return g_frames[g_frameIndex]; }

// ------------------------------------------------------------------------------------------------ deferred recording
// (SLY2N_VKTHREAD, on by default) The commands are recorded into the driver's command buffers on a worker thread, as
// OpenGL drivers do on theirs (Mesa's glthread): the game thread keeps the GL state and only appends each command, its
// arguments copied, to a stream; the worker records the streams, submits them and presents. The functions below shadow
// cane::vk's vkCmd* in this file: with the worker they append, without it they call the driver directly. g_main and
// g_pre are then stream tokens, not command buffers.
bool g_threaded = false;
// (threaded: the open rendering instance's attachments in the stream, glInvalidateFramebuffer; and what it dropped)
VkRenderingAttachmentInfo *g_recColor = nullptr, *g_recDepth = nullptr, *g_recStencil = nullptr;
bool g_invalidatedColor = false, g_invalidatedDepth = false;
// (threaded, the classic render pass object of the in-place read: the instance's begin as recorded; glInvalidateFramebuffer
// swaps its render pass for the compatible one with DONT_CARE stores, g_instanceClassicKey's ops changed)
VkRenderPassBeginInfo* g_recClassic = nullptr;
// (threaded) the depth-stencil image of the last instance that ended, while nothing has used it since (a layout change,
// another instance on it, the submission): a glInvalidateFramebuffer of it then still drops that instance's stores
// (the scene's depth: the frame copy ends its instance before GL::Present invalidates it). Its recorded attachments /
// classic begin and ops below (EndedInstance).
const void* g_endedDepth = nullptr;
const CaneVk::ExternalDevice* g_external = nullptr;   // (VR: the instance / device through OpenXR)
const VkCommandBuffer kStreamMain = reinterpret_cast<VkCommandBuffer>(uintptr_t(0x10)), kStreamPre = reinterpret_cast<VkCommandBuffer>(uintptr_t(0x20));
class Stream
{
public:
    void* Alloc(size_t n)
    {
        n = (n + 15) & ~size_t(15);
        if (m_chunks.empty() || m_chunks[m_cur].used + n > m_chunks[m_cur].cap)
        {
            if (m_chunks.empty() || ++m_cur == m_chunks.size())
            {
                const size_t cap = std::max<size_t>(n, 256u << 10);
                m_chunks.push_back({std::unique_ptr<uint8_t[]>(new uint8_t[cap]), 0, cap});
                m_cur = m_chunks.size() - 1;
            }
            m_chunks[m_cur].used = 0;
        }
        Chunk& c = m_chunks[m_cur];
        void* p = c.data.get() + c.used;
        c.used += n;
        return p;
    }
    template <class T> const T* Dup(const T* p, uint32_t n)
    {
        if (!p || !n) return nullptr;
        T* q = static_cast<T*>(Alloc(sizeof(T) * n));
        std::memcpy(static_cast<void*>(q), p, sizeof(T) * n);
        return q;
    }
    template <class F> void Rec(F f)
    {
        static_assert(std::is_trivially_destructible_v<F>);
        struct Node { void (*run)(const void*, VkCommandBuffer); uint32_t size; };
        auto* node = static_cast<Node*>(Alloc(sizeof(Node) + sizeof(F)));
        node->run = [](const void* p, VkCommandBuffer cmd) { (*static_cast<const F*>(p))(cmd); };
        node->size = uint32_t(((sizeof(Node) + sizeof(F)) + 15) & ~size_t(15));
        new (node + 1) F(f);
        if (!m_first) m_first = node;
        ++m_count;
        m_nodes.push_back(node);
    }
    void Replay(VkCommandBuffer cmd) const
    {
        struct Node { void (*run)(const void*, VkCommandBuffer); uint32_t size; };
        for (void* p : m_nodes) { const Node* n = static_cast<const Node*>(p); n->run(n + 1, cmd); }
    }
    bool Empty() const { return m_nodes.empty(); }
    void Clear()
    {
        m_nodes.clear();
        m_first = nullptr;
        m_count = 0;
        m_cur = 0;
        if (!m_chunks.empty()) m_chunks[0].used = 0;
    }
private:
    struct Chunk { std::unique_ptr<uint8_t[]> data; size_t used, cap; };
    std::vector<Chunk> m_chunks;
    size_t m_cur = 0;
    std::vector<void*> m_nodes;
    void* m_first = nullptr;
    size_t m_count = 0;
};
Stream* g_streamMain = nullptr;
Stream* g_streamPre = nullptr;
Stream& S(VkCommandBuffer c) { return c == kStreamPre ? *g_streamPre : *g_streamMain; }

#define CANE_DEFER(call_args, ...)                                                       \
    do {                                                                                 \
        if (!g_threaded) return cane::vk::call_args;                                     \
        S(cmd).Rec(__VA_ARGS__);                                                         \
    } while (0)

void vkCmdBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint bp, VkPipeline p)
{ CANE_DEFER(vkCmdBindPipeline(cmd, bp, p), [=](VkCommandBuffer c) { cane::vk::vkCmdBindPipeline(c, bp, p); }); }
void vkCmdBindDescriptorSets(VkCommandBuffer cmd, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t first, uint32_t n,
                             const VkDescriptorSet* sets, uint32_t nd, const uint32_t* dyn)
{
    if (!g_threaded) return cane::vk::vkCmdBindDescriptorSets(cmd, bp, l, first, n, sets, nd, dyn);
    const VkDescriptorSet* s2 = S(cmd).Dup(sets, n);
    const uint32_t* d2 = S(cmd).Dup(dyn, nd);
    S(cmd).Rec([=](VkCommandBuffer c) { cane::vk::vkCmdBindDescriptorSets(c, bp, l, first, n, s2, nd, d2); });
}
void vkCmdPushConstants(VkCommandBuffer cmd, VkPipelineLayout l, VkShaderStageFlags st, uint32_t off, uint32_t size, const void* data)
{
    if (!g_threaded) return cane::vk::vkCmdPushConstants(cmd, l, st, off, size, data);
    const uint8_t* d2 = S(cmd).Dup(static_cast<const uint8_t*>(data), size);
    S(cmd).Rec([=](VkCommandBuffer c) { cane::vk::vkCmdPushConstants(c, l, st, off, size, d2); });
}
void vkCmdPushDescriptorSetKHR(VkCommandBuffer cmd, VkPipelineBindPoint bp, VkPipelineLayout l, uint32_t set, uint32_t n,
                               const VkWriteDescriptorSet* w)
{
    if (!g_threaded) return cane::vk::vkCmdPushDescriptorSetKHR(cmd, bp, l, set, n, w);
    Stream& s = S(cmd);
    auto* w2 = static_cast<VkWriteDescriptorSet*>(s.Alloc(sizeof(VkWriteDescriptorSet) * n));
    for (uint32_t i = 0; i < n; ++i)
    {
        w2[i] = w[i];
        w2[i].pBufferInfo = s.Dup(w[i].pBufferInfo, w[i].descriptorCount);
        w2[i].pImageInfo = s.Dup(w[i].pImageInfo, w[i].descriptorCount);
    }
    s.Rec([=](VkCommandBuffer c) { cane::vk::vkCmdPushDescriptorSetKHR(c, bp, l, set, n, w2); });
}
void vkCmdBindVertexBuffers(VkCommandBuffer cmd, uint32_t first, uint32_t n, const VkBuffer* b, const VkDeviceSize* o)
{
    if (!g_threaded) return cane::vk::vkCmdBindVertexBuffers(cmd, first, n, b, o);
    const VkBuffer* b2 = S(cmd).Dup(b, n);
    const VkDeviceSize* o2 = S(cmd).Dup(o, n);
    S(cmd).Rec([=](VkCommandBuffer c) { cane::vk::vkCmdBindVertexBuffers(c, first, n, b2, o2); });
}
void vkCmdBindIndexBuffer(VkCommandBuffer cmd, VkBuffer b, VkDeviceSize o, VkIndexType t)
{ CANE_DEFER(vkCmdBindIndexBuffer(cmd, b, o, t), [=](VkCommandBuffer c) { cane::vk::vkCmdBindIndexBuffer(c, b, o, t); }); }
void vkCmdDraw(VkCommandBuffer cmd, uint32_t a, uint32_t b, uint32_t c0, uint32_t d)
{ CANE_DEFER(vkCmdDraw(cmd, a, b, c0, d), [=](VkCommandBuffer c) { cane::vk::vkCmdDraw(c, a, b, c0, d); }); }
void vkCmdDrawIndexed(VkCommandBuffer cmd, uint32_t a, uint32_t b, uint32_t c0, int32_t d, uint32_t e)
{ CANE_DEFER(vkCmdDrawIndexed(cmd, a, b, c0, d, e), [=](VkCommandBuffer c) { cane::vk::vkCmdDrawIndexed(c, a, b, c0, d, e); }); }
void vkCmdDrawIndexedIndirect(VkCommandBuffer cmd, VkBuffer b, VkDeviceSize o, uint32_t n, uint32_t st)
{ CANE_DEFER(vkCmdDrawIndexedIndirect(cmd, b, o, n, st), [=](VkCommandBuffer c) { cane::vk::vkCmdDrawIndexedIndirect(c, b, o, n, st); }); }
void vkCmdSetViewport(VkCommandBuffer cmd, uint32_t first, uint32_t n, const VkViewport* v)
{
    if (!g_threaded) return cane::vk::vkCmdSetViewport(cmd, first, n, v);
    const VkViewport v2 = v[0];
    S(cmd).Rec([=](VkCommandBuffer c) { cane::vk::vkCmdSetViewport(c, first, 1, &v2); });
}
void vkCmdSetScissor(VkCommandBuffer cmd, uint32_t first, uint32_t n, const VkRect2D* r)
{
    if (!g_threaded) return cane::vk::vkCmdSetScissor(cmd, first, n, r);
    const VkRect2D r2 = r[0];
    S(cmd).Rec([=](VkCommandBuffer c) { cane::vk::vkCmdSetScissor(c, first, 1, &r2); });
}
void vkCmdSetLineWidth(VkCommandBuffer cmd, float w)
{ CANE_DEFER(vkCmdSetLineWidth(cmd, w), [=](VkCommandBuffer c) { cane::vk::vkCmdSetLineWidth(c, w); }); }
void vkCmdSetBlendConstants(VkCommandBuffer cmd, const float k[4])
{
    if (!g_threaded) return cane::vk::vkCmdSetBlendConstants(cmd, k);
    const std::array<float, 4> k2{k[0], k[1], k[2], k[3]};
    S(cmd).Rec([=](VkCommandBuffer c) { cane::vk::vkCmdSetBlendConstants(c, k2.data()); });
}
#define CANE_DEFER_FACE(name, T)                                                                              \
    void name(VkCommandBuffer cmd, VkStencilFaceFlags f, T v)                                                 \
    { CANE_DEFER(name(cmd, f, v), [=](VkCommandBuffer c) { cane::vk::name(c, f, v); }); }
CANE_DEFER_FACE(vkCmdSetStencilCompareMask, uint32_t)
CANE_DEFER_FACE(vkCmdSetStencilWriteMask, uint32_t)
CANE_DEFER_FACE(vkCmdSetStencilReference, uint32_t)
#undef CANE_DEFER_FACE
#define CANE_DEFER_1(name, T) \
    void name(VkCommandBuffer cmd, T v) { CANE_DEFER(name(cmd, v), [=](VkCommandBuffer c) { cane::vk::name(c, v); }); }
CANE_DEFER_1(vkCmdSetCullMode, VkCullModeFlags)
CANE_DEFER_1(vkCmdSetFrontFace, VkFrontFace)
CANE_DEFER_1(vkCmdSetDepthTestEnable, VkBool32)
CANE_DEFER_1(vkCmdSetDepthWriteEnable, VkBool32)
CANE_DEFER_1(vkCmdSetDepthCompareOp, VkCompareOp)
CANE_DEFER_1(vkCmdSetStencilTestEnable, VkBool32)
CANE_DEFER_1(vkCmdSetDepthBoundsTestEnable, VkBool32)
#undef CANE_DEFER_1
void vkCmdSetDepthBounds(VkCommandBuffer cmd, float lo, float hi)
{ CANE_DEFER(vkCmdSetDepthBounds(cmd, lo, hi), [=](VkCommandBuffer c) { cane::vk::vkCmdSetDepthBounds(c, lo, hi); }); }
void vkCmdSetStencilOp(VkCommandBuffer cmd, VkStencilFaceFlags f, VkStencilOp a, VkStencilOp b, VkStencilOp d, VkCompareOp o)
{ CANE_DEFER(vkCmdSetStencilOp(cmd, f, a, b, d, o), [=](VkCommandBuffer c) { cane::vk::vkCmdSetStencilOp(c, f, a, b, d, o); }); }
void vkCmdSetColorBlendEnableEXT(VkCommandBuffer cmd, uint32_t first, uint32_t n, const VkBool32* e)
{
    if (!g_threaded) return cane::vk::vkCmdSetColorBlendEnableEXT(cmd, first, n, e);
    const VkBool32 e2 = e[0];
    S(cmd).Rec([=](VkCommandBuffer c) { cane::vk::vkCmdSetColorBlendEnableEXT(c, first, 1, &e2); });
}
void vkCmdSetColorBlendEquationEXT(VkCommandBuffer cmd, uint32_t first, uint32_t n, const VkColorBlendEquationEXT* e)
{
    if (!g_threaded) return cane::vk::vkCmdSetColorBlendEquationEXT(cmd, first, n, e);
    const VkColorBlendEquationEXT e2 = e[0];
    S(cmd).Rec([=](VkCommandBuffer c) { cane::vk::vkCmdSetColorBlendEquationEXT(c, first, 1, &e2); });
}
void vkCmdSetColorWriteMaskEXT(VkCommandBuffer cmd, uint32_t first, uint32_t n, const VkColorComponentFlags* m)
{
    if (!g_threaded) return cane::vk::vkCmdSetColorWriteMaskEXT(cmd, first, n, m);
    const VkColorComponentFlags m2 = m[0];
    S(cmd).Rec([=](VkCommandBuffer c) { cane::vk::vkCmdSetColorWriteMaskEXT(c, first, 1, &m2); });
}
void vkCmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo* ri)
{
    if (!g_threaded) return cane::vk::vkCmdBeginRendering(cmd, ri);
    Stream& s = S(cmd);
    auto* r2 = static_cast<VkRenderingInfo*>(s.Alloc(sizeof(VkRenderingInfo)));
    *r2 = *ri;
    // (the one extension structure BeginRenderingNow chains: msrtss's)
    // (the extension structures BeginRenderingNow chains: msrtss's, the density map's)
    {
        const void** link = &r2->pNext;
        *link = nullptr;
        for (auto* p = static_cast<const VkBaseInStructure*>(ri->pNext); p; p = p->pNext)
        {
            void* q = nullptr;
            if (p->sType == VK_STRUCTURE_TYPE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_INFO_EXT)
                q = const_cast<VkMultisampledRenderToSingleSampledInfoEXT*>(s.Dup(reinterpret_cast<const VkMultisampledRenderToSingleSampledInfoEXT*>(p), 1));
            else if (p->sType == VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_INFO_EXT)
                q = const_cast<VkRenderingFragmentDensityMapAttachmentInfoEXT*>(s.Dup(reinterpret_cast<const VkRenderingFragmentDensityMapAttachmentInfoEXT*>(p), 1));
            if (!q) continue;
            static_cast<VkBaseOutStructure*>(q)->pNext = nullptr;
            *link = q;
            link = const_cast<const void**>(reinterpret_cast<void**>(&static_cast<VkBaseOutStructure*>(q)->pNext));
        }
    }
    r2->pColorAttachments = s.Dup(ri->pColorAttachments, ri->colorAttachmentCount);
    r2->pDepthAttachment = s.Dup(ri->pDepthAttachment, ri->pDepthAttachment ? 1 : 0);
    r2->pStencilAttachment = s.Dup(ri->pStencilAttachment, ri->pStencilAttachment ? 1 : 0);
    // (the instance's attachments as recorded: glInvalidateFramebuffer may still drop their stores before the worker
    // replays them)
    g_recColor = const_cast<VkRenderingAttachmentInfo*>(r2->pColorAttachments);
    g_recDepth = const_cast<VkRenderingAttachmentInfo*>(r2->pDepthAttachment);
    g_recStencil = const_cast<VkRenderingAttachmentInfo*>(r2->pStencilAttachment);
    s.Rec([=](VkCommandBuffer c) { cane::vk::vkCmdBeginRendering(c, r2); });
}
void vkCmdEndRendering(VkCommandBuffer cmd)
{ CANE_DEFER(vkCmdEndRendering(cmd), [=](VkCommandBuffer c) { cane::vk::vkCmdEndRendering(c); }); }
void vkCmdBeginRenderPass2(VkCommandBuffer cmd, const VkRenderPassBeginInfo* rb, const VkSubpassBeginInfo* sb)
{
    if (!g_threaded) return cane::vk::vkCmdBeginRenderPass2(cmd, rb, sb);
    Stream& s = S(cmd);
    auto* r2 = static_cast<VkRenderPassBeginInfo*>(s.Alloc(sizeof(VkRenderPassBeginInfo)));
    *r2 = *rb;
    r2->pClearValues = s.Dup(rb->pClearValues, rb->clearValueCount);
    // (the one extension structure: the imageless framebuffer's views)
    if (rb->pNext)
    {
        const auto* a = static_cast<const VkRenderPassAttachmentBeginInfo*>(rb->pNext);
        auto* a2 = static_cast<VkRenderPassAttachmentBeginInfo*>(s.Alloc(sizeof(VkRenderPassAttachmentBeginInfo)));
        *a2 = *a;
        a2->pAttachments = s.Dup(a->pAttachments, a->attachmentCount);
        r2->pNext = a2;
    }
    const VkSubpassBeginInfo sb2 = *sb;
    g_recClassic = r2;
    s.Rec([=](VkCommandBuffer c) { cane::vk::vkCmdBeginRenderPass2(c, r2, &sb2); });
}
void vkCmdEndRenderPass2(VkCommandBuffer cmd, const VkSubpassEndInfo* ei)
{
    const VkSubpassEndInfo e2 = *ei;
    CANE_DEFER(vkCmdEndRenderPass2(cmd, ei), [=](VkCommandBuffer c) { cane::vk::vkCmdEndRenderPass2(c, &e2); });
}
void vkCmdPipelineBarrier2(VkCommandBuffer cmd, const VkDependencyInfo* di)
{
    if (!g_threaded) return cane::vk::vkCmdPipelineBarrier2(cmd, di);
    Stream& s = S(cmd);
    auto* d2 = static_cast<VkDependencyInfo*>(s.Alloc(sizeof(VkDependencyInfo)));
    *d2 = *di;
    d2->pMemoryBarriers = s.Dup(di->pMemoryBarriers, di->memoryBarrierCount);
    d2->pBufferMemoryBarriers = s.Dup(di->pBufferMemoryBarriers, di->bufferMemoryBarrierCount);
    d2->pImageMemoryBarriers = s.Dup(di->pImageMemoryBarriers, di->imageMemoryBarrierCount);
    s.Rec([=](VkCommandBuffer c) { cane::vk::vkCmdPipelineBarrier2(c, d2); });
}
void vkCmdClearAttachments(VkCommandBuffer cmd, uint32_t n, const VkClearAttachment* a, uint32_t nr, const VkClearRect* r)
{
    if (!g_threaded) return cane::vk::vkCmdClearAttachments(cmd, n, a, nr, r);
    const VkClearAttachment* a2 = S(cmd).Dup(a, n);
    const VkClearRect* r2 = S(cmd).Dup(r, nr);
    S(cmd).Rec([=](VkCommandBuffer c) { cane::vk::vkCmdClearAttachments(c, n, a2, nr, r2); });
}
void vkCmdClearColorImage(VkCommandBuffer cmd, VkImage im, VkImageLayout l, const VkClearColorValue* v, uint32_t n, const VkImageSubresourceRange* r)
{
    if (!g_threaded) return cane::vk::vkCmdClearColorImage(cmd, im, l, v, n, r);
    const VkClearColorValue* v2 = S(cmd).Dup(v, 1);
    const VkImageSubresourceRange* r2 = S(cmd).Dup(r, n);
    S(cmd).Rec([=](VkCommandBuffer c) { cane::vk::vkCmdClearColorImage(c, im, l, v2, n, r2); });
}
void vkCmdClearDepthStencilImage(VkCommandBuffer cmd, VkImage im, VkImageLayout l, const VkClearDepthStencilValue* v, uint32_t n,
                                 const VkImageSubresourceRange* r)
{
    if (!g_threaded) return cane::vk::vkCmdClearDepthStencilImage(cmd, im, l, v, n, r);
    const VkClearDepthStencilValue* v2 = S(cmd).Dup(v, 1);
    const VkImageSubresourceRange* r2 = S(cmd).Dup(r, n);
    S(cmd).Rec([=](VkCommandBuffer c) { cane::vk::vkCmdClearDepthStencilImage(c, im, l, v2, n, r2); });
}
void vkCmdCopyBuffer(VkCommandBuffer cmd, VkBuffer a, VkBuffer b, uint32_t n, const VkBufferCopy* r)
{
    if (!g_threaded) return cane::vk::vkCmdCopyBuffer(cmd, a, b, n, r);
    const VkBufferCopy* r2 = S(cmd).Dup(r, n);
    S(cmd).Rec([=](VkCommandBuffer c) { cane::vk::vkCmdCopyBuffer(c, a, b, n, r2); });
}
void vkCmdCopyBufferToImage(VkCommandBuffer cmd, VkBuffer a, VkImage b, VkImageLayout l, uint32_t n, const VkBufferImageCopy* r)
{
    if (!g_threaded) return cane::vk::vkCmdCopyBufferToImage(cmd, a, b, l, n, r);
    const VkBufferImageCopy* r2 = S(cmd).Dup(r, n);
    S(cmd).Rec([=](VkCommandBuffer c) { cane::vk::vkCmdCopyBufferToImage(c, a, b, l, n, r2); });
}
void vkCmdCopyImageToBuffer(VkCommandBuffer cmd, VkImage a, VkImageLayout l, VkBuffer b, uint32_t n, const VkBufferImageCopy* r)
{
    if (!g_threaded) return cane::vk::vkCmdCopyImageToBuffer(cmd, a, l, b, n, r);
    const VkBufferImageCopy* r2 = S(cmd).Dup(r, n);
    S(cmd).Rec([=](VkCommandBuffer c) { cane::vk::vkCmdCopyImageToBuffer(c, a, l, b, n, r2); });
}
void vkCmdCopyImage(VkCommandBuffer cmd, VkImage a, VkImageLayout la, VkImage b, VkImageLayout lb, uint32_t n, const VkImageCopy* r)
{
    if (!g_threaded) return cane::vk::vkCmdCopyImage(cmd, a, la, b, lb, n, r);
    const VkImageCopy* r2 = S(cmd).Dup(r, n);
    S(cmd).Rec([=](VkCommandBuffer c) { cane::vk::vkCmdCopyImage(c, a, la, b, lb, n, r2); });
}
void vkCmdBlitImage(VkCommandBuffer cmd, VkImage a, VkImageLayout la, VkImage b, VkImageLayout lb, uint32_t n, const VkImageBlit* r, VkFilter f)
{
    if (!g_threaded) return cane::vk::vkCmdBlitImage(cmd, a, la, b, lb, n, r, f);
    const VkImageBlit* r2 = S(cmd).Dup(r, n);
    S(cmd).Rec([=](VkCommandBuffer c) { cane::vk::vkCmdBlitImage(c, a, la, b, lb, n, r2, f); });
}
void vkCmdResolveImage(VkCommandBuffer cmd, VkImage a, VkImageLayout la, VkImage b, VkImageLayout lb, uint32_t n, const VkImageResolve* r)
{
    if (!g_threaded) return cane::vk::vkCmdResolveImage(cmd, a, la, b, lb, n, r);
    const VkImageResolve* r2 = S(cmd).Dup(r, n);
    S(cmd).Rec([=](VkCommandBuffer c) { cane::vk::vkCmdResolveImage(c, a, la, b, lb, n, r2); });
}
void vkCmdWriteTimestamp(VkCommandBuffer cmd, VkPipelineStageFlagBits st, VkQueryPool pool, uint32_t q)
{ CANE_DEFER(vkCmdWriteTimestamp(cmd, st, pool, q), [=](VkCommandBuffer c) { cane::vk::vkCmdWriteTimestamp(c, st, pool, q); }); }
#undef CANE_DEFER

// The worker: a batch is one submission (its pre and main streams), optionally followed by the present.
struct Batch
{
    Stream* pre = nullptr;
    Stream* main = nullptr;
    unsigned slot = 0;
    bool resetPool = false;
    uint64_t value = 0;
    VkSemaphore wait = VK_NULL_HANDLE, signal = VK_NULL_HANDLE;
    bool present = false;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    uint32_t imageIndex = 0;
    bool quit = false;
    std::vector<std::function<void()>> after;   // (VR: AfterNextSubmit's calls, run after the submission)
};
std::vector<std::function<void()>> g_afterSubmit;   // (AfterNextSubmit: for the next submission)
std::thread g_worker;
std::mutex g_workerMutex;
std::condition_variable g_workerCv, g_workerDoneCv;
std::deque<Batch> g_batches;
std::vector<Stream*> g_freeStreams;
uint64_t g_workerDone = 0;              // the last batch value the worker has submitted (under g_workerMutex)
std::atomic<bool> g_presentStale{false};
// (Android reports SUBOPTIMAL for every present whose transform is not the display's current rotation: the swapchain
// keeps the identity transform there and the compositor rotates; a new size comes as OUT_OF_DATE or a window resize)
#if defined(__ANDROID__)
constexpr bool kSuboptimalStale = false;
#else
constexpr bool kSuboptimalStale = true;
#endif
Stream* TakeStream()
{
    std::lock_guard<std::mutex> lock(g_workerMutex);
    if (g_freeStreams.empty()) return new Stream;
    Stream* s = g_freeStreams.back();
    g_freeStreams.pop_back();
    return s;
}

VkDescriptorSetLayout g_setLayoutTextures = VK_NULL_HANDLE, g_setLayoutBuffers = VK_NULL_HANDLE;
VkPipelineLayout g_pipelineLayout = VK_NULL_HANDLE;
VkDescriptorPool g_descriptorPool = VK_NULL_HANDLE;
VkDescriptorSet g_textureSet = VK_NULL_HANDLE;
VkDescriptorSetLayout g_setLayoutUniforms = VK_NULL_HANDLE;   // (set 2: g_feat.pushBytes below a program's block)
constexpr uint32_t kSetPoolSize = 1024;                         // descriptor sets per frame pool (more pools when needed)
uint32_t g_uniformAlign = 256;
VkShaderStageFlags g_stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
Alloc g_nullStorage;               // (a storage binding with nothing bound reads zeros)
Alloc g_generic;                   // the disabled attributes' values: 16 x float (0, 0, 0, 1), then 16 x int (0, 0, 0, 1)

void BeginFrame();
void BeginSegment();
VkCommandBuffer Main()
{
    if (!g_recording) BeginFrame();
    return g_main;
}
VkCommandBuffer AllocCommandBuffer(Frame& f)
{
    if (f.cmdNext == f.cmds.size())
    {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        VkCommandBuffer cmd;
        VKCHECK(vkAllocateCommandBuffers(g_device, &ai, &cmd));
        f.cmds.push_back(cmd);
    }
    VkCommandBuffer cmd = f.cmds[f.cmdNext++];
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VKCHECK(vkBeginCommandBuffer(cmd, &bi));
    return cmd;
}

struct Upload { VkBuffer buffer; VkDeviceSize offset; uint8_t* cpu; };
void DrainRing();
// Transient memory of this frame (reused once the frame has retired).
Upload Ring(VkDeviceSize size, VkDeviceSize alignment = 256)
{
    if (!g_recording) BeginFrame();
    Frame& f = Cur();
    size = Align(std::max<VkDeviceSize>(size, 4), 4);
    // (a world load uploads every texture before its first frame: past 128 MB the frame waits for the GPU and starts
    // over in the memory it has, rather than growing without bound)
    if (f.ring.size() * kRingChunkSize >= (128ull << 20) && f.ringIndex + 1 >= f.ring.size())
    {
        const RingChunk& last = f.ring.back();
        if (Align(last.cursor, alignment) + size > last.alloc.size) DrainRing();
    }
    for (; f.ringIndex < f.ring.size(); ++f.ringIndex)
    {
        RingChunk& c = f.ring[f.ringIndex];
        const VkDeviceSize offset = Align(c.cursor, alignment);
        if (offset + size <= c.alloc.size)
        {
            c.cursor = offset + size;
            return {c.alloc.Buffer(), c.alloc.offset + offset, c.alloc.Cpu() + offset};
        }
    }
    RingChunk c;
    c.alloc = AllocBuffer(std::max(kRingChunkSize, Align(size, 4096)), 256);
    c.cursor = size;
    f.ring.push_back(c);
    f.ringIndex = f.ring.size() - 1;
    return {c.alloc.Buffer(), c.alloc.offset, c.alloc.Cpu()};
}

// ------------------------------------------------------------------------------------------------ barriers
struct LayoutScope { VkPipelineStageFlags2 stage; VkAccessFlags2 access; };
constexpr VkPipelineStageFlags2 kShaderStages = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_GEOMETRY_SHADER_BIT |
                                                VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
LayoutScope ScopeOf(VkImageLayout layout)
{
    switch (layout)
    {
    case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL: return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT};
    case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL: return {VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT};
    case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL: return {kShaderStages, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT};
    case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT};
    case VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL:
        return {VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT};
    case VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR:
        return {VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT};
    case VK_IMAGE_LAYOUT_PRESENT_SRC_KHR: return {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
    default: return {VK_PIPELINE_STAGE_2_NONE, VK_ACCESS_2_NONE};
    }
}
struct Barriers
{
    std::vector<VkImageMemoryBarrier2> images;
    VkMemoryBarrier2 memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    bool fMemory = false;
    void Image(const struct Image& im, VkImageLayout from, VkImageLayout to, uint32_t level = 0, uint32_t levels = VK_REMAINING_MIP_LEVELS)
    {
        if (&im == g_endedDepth) g_endedDepth = nullptr;
        VkImageMemoryBarrier2 bar{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        const LayoutScope a = ScopeOf(from), z = ScopeOf(to);
        bar.srcStageMask = a.stage ? a.stage : VK_PIPELINE_STAGE_2_NONE;
        bar.srcAccessMask = a.access & ~(VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_TRANSFER_READ_BIT);   // (reads: an execution dependency)
        bar.dstStageMask = z.stage ? z.stage : VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        bar.dstAccessMask = z.access;
        bar.oldLayout = from;
        bar.newLayout = to;
        bar.srcQueueFamilyIndex = bar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bar.image = im.image;
        bar.subresourceRange = {im.aspect, level, levels, 0, VK_REMAINING_ARRAY_LAYERS};
        images.push_back(bar);
    }
    void Memory(VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
    {
        memory.srcStageMask |= srcStage;
        memory.srcAccessMask |= srcAccess;
        memory.dstStageMask |= dstStage;
        memory.dstAccessMask |= dstAccess;
        fMemory = true;
    }
    void Flush(VkCommandBuffer cmd)
    {
        if (images.empty() && !fMemory) return;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.memoryBarrierCount = fMemory ? 1 : 0;
        di.pMemoryBarriers = &memory;
        di.imageMemoryBarrierCount = uint32_t(images.size());
        di.pImageMemoryBarriers = images.data();
        vkCmdPipelineBarrier2(cmd, &di);
        images.clear();
        memory = VkMemoryBarrier2{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        fMemory = false;
    }
};
VkImageLayout FinalLayout(const Image& im)
{
    if (im.fdm) return VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
    if (im.sampled) return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return im.depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}
std::vector<Image*> g_dirty;       // images left in an attachment or transfer layout (back to readable at a pass start)
void MarkDirty(Image& im)
{
    if (im.dirty) return;
    im.dirty = true;
    g_dirty.push_back(&im);
}

// ------------------------------------------------------------------------------------------------ descriptors
std::vector<uint32_t> g_freeSlots2D, g_freeSlotsArray;
uint32_t g_nextSlot2D = 1, g_nextSlotArray = 1;   // (slot 0 of each: the black texture, never freed)
std::vector<VkSampler> g_vkSamplers;
struct SamplerKeyHash { size_t operator()(uint64_t k) const { return size_t(k ^ (k >> 29)); } };
std::unordered_map<uint64_t, int, SamplerKeyHash> g_samplerSlots;

uint64_t SamplerKey(const SamplerParams& p)
{
    auto filter = [](GLenum f) -> uint64_t {
        switch (f)
        {
        case GL_NEAREST: return 0; case GL_LINEAR: return 1; case GL_NEAREST_MIPMAP_NEAREST: return 2;
        case GL_LINEAR_MIPMAP_NEAREST: return 3; case GL_NEAREST_MIPMAP_LINEAR: return 4; default: return 5;
        }
    };
    auto wrap = [](GLenum w) -> uint64_t { return w == GL_CLAMP_TO_EDGE ? 1 : w == GL_MIRRORED_REPEAT ? 2 : w == GL_CLAMP_TO_BORDER ? 3 : 0; };
    const uint64_t aniso = uint64_t(std::clamp(p.anisotropy, 1.0f, 16.0f) * 4.0f);
    return filter(p.minFilter) | (uint64_t(p.magFilter == GL_NEAREST ? 0 : 1) << 3) | (wrap(p.wrapS) << 4) | (wrap(p.wrapT) << 6) |
           (aniso << 8) | (uint64_t(std::clamp(p.maxLevel, 0, 1000)) << 16);
}
bool MipFilter(GLenum min) { return min != GL_NEAREST && min != GL_LINEAR; }
int SamplerSlot(const SamplerParams& p)
{
    const uint64_t key = SamplerKey(p);
    auto it = g_samplerSlots.find(key);
    if (it != g_samplerSlots.end()) return it->second;
    const int slot = int(g_vkSamplers.size());
    if (slot >= int(kMaxSamplers)) return 0;
    VkSamplerCreateInfo ci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    const GLenum min = p.minFilter;
    ci.minFilter = (min == GL_NEAREST || min == GL_NEAREST_MIPMAP_NEAREST || min == GL_NEAREST_MIPMAP_LINEAR) ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.magFilter = p.magFilter == GL_NEAREST ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    ci.mipmapMode = (min == GL_NEAREST_MIPMAP_LINEAR || min == GL_LINEAR_MIPMAP_LINEAR) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    auto wrap = [](GLenum w) {
        return w == GL_CLAMP_TO_EDGE ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE : w == GL_MIRRORED_REPEAT ? VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT
             : w == GL_CLAMP_TO_BORDER ? VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER : VK_SAMPLER_ADDRESS_MODE_REPEAT;
    };
    ci.addressModeU = wrap(p.wrapS);
    ci.addressModeV = wrap(p.wrapT);
    ci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    ci.minLod = 0.0f;
    // (GL's non-mipmapped minification reads the base level alone: Vulkan's way to say it is a LOD clamp of 0.25)
    ci.maxLod = MipFilter(min) ? float(std::clamp(p.maxLevel, 0, 1000)) : 0.25f;
    ci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (g_feat.anisotropy && p.anisotropy > 1.0f)
    {
        ci.anisotropyEnable = VK_TRUE;
        ci.maxAnisotropy = std::min(p.anisotropy, g_feat.anisotropyMax);
    }
    VkSampler sampler;
    VKCHECK(vkCreateSampler(g_device, &ci, nullptr, &sampler));
    g_vkSamplers.push_back(sampler);
    VkDescriptorImageInfo ii{sampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = g_textureSet;
    w.dstBinding = 1;
    w.dstArrayElement = uint32_t(slot);
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(g_device, 1, &w, 0, nullptr);
    g_samplerSlots.emplace(key, slot);
    return slot;
}

void WriteImageDescriptor(Image& im)
{
    if (im.slot < 0)
    {
        auto& freeSlots = im.array ? g_freeSlotsArray : g_freeSlots2D;
        uint32_t& next = im.array ? g_nextSlotArray : g_nextSlot2D;
        if (!freeSlots.empty()) { im.slot = int(freeSlots.back()); freeSlots.pop_back(); }
        else
        {
            if (next >= (im.array ? kMaxArrays : kMaxImages)) Fatal("texture descriptor capacity exceeded");
            im.slot = int(next++);
        }
    }
    VkDescriptorImageInfo ii{VK_NULL_HANDLE, im.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = g_textureSet;
    w.dstBinding = im.array ? 2 : 0;
    w.dstArrayElement = uint32_t(im.slot);
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    w.pImageInfo = &ii;
    vkUpdateDescriptorSets(g_device, 1, &w, 0, nullptr);
}

// ------------------------------------------------------------------------------------------------ images
VkSampleCountFlagBits SampleBits(unsigned n)
{
    switch (n) { case 2: return VK_SAMPLE_COUNT_2_BIT; case 4: return VK_SAMPLE_COUNT_4_BIT; case 8: return VK_SAMPLE_COUNT_8_BIT;
                 case 16: return VK_SAMPLE_COUNT_16_BIT; default: return VK_SAMPLE_COUNT_1_BIT; }
}
bool IsDepthFormat(VkFormat f) { return f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT; }

struct XferOp;
void QueueClear(Image& im);
uint32_t g_nextRenderSamples = 1;   // (msrtss: the next image made is drawn with this many samples; SetNextImageRenderSamples)
uint32_t g_nextRenderbufferLayers = 1;   // (VR multiview: the next renderbuffer made has this many layers; SetNextRenderbufferLayers)
std::unique_ptr<Image> MakeImage(uint32_t width, uint32_t height, uint32_t layers, uint32_t levels, uint32_t samples,
                                 VkFormat format, bool array, bool sampled, VkImageUsageFlags extraUsage = 0, bool fClear = true)
{
    auto im = std::make_unique<Image>();
    im->width = width; im->height = height; im->layers = layers; im->levels = levels; im->samples = samples;
    im->format = format; im->array = array; im->sampled = sampled;
    im->depth = IsDepthFormat(format);
    im->aspect = im->depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT) : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {width, height, 1};
    ci.mipLevels = levels;
    ci.arrayLayers = layers;
    ci.samples = SampleBits(samples);
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    if (g_nextRenderSamples > 1 && samples == 1 && g_feat.msrtss)
    {
        ci.flags |= VK_IMAGE_CREATE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_BIT_EXT;
        im->renderSamples = uint8_t(g_nextRenderSamples);
        static int s_cLogged = 0;
        if (s_cLogged++ < 4) std::printf("Vulkan: %ux%u %s drawn with %u samples into one (resolved on the tile)\n", width, height,
                                         IsDepthFormat(format) ? "depth" : "colour", g_nextRenderSamples);
    }
    g_nextRenderSamples = 1;
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (im->depth) ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    else ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (sampled) ci.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    // (not with msrtss: an in-place read of a single-sampled image drawn multisampled is not defined)
    im->inputAttachment = (g_feat.localRead || g_feat.classicFetch) && !im->depth && samples <= 1 && im->renderSamples <= 1;
    if (im->inputAttachment) ci.usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    ci.usage |= extraUsage;
    im->usage = ci.usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKCHECK(vkCreateImage(g_device, &ci, nullptr, &im->image));
    static const bool s_fTrace = std::getenv("SLY2N_VKIMAGES") != nullptr;
    if (s_fTrace) std::fprintf(stderr, "vkimage %p %ux%u x%u levels %u samples %u %s%s\n", static_cast<void*>(im->image), width, height, layers,
                               levels, samples, sampled ? "sampled" : "", im->depth ? " depth" : "");
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_device, im->image, &req);
    int type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) type = FindMemoryType(req.memoryTypeBits, 0);
    auto& heap = g_imageHeaps[uint32_t(type)];
    if (!heap) heap = std::make_unique<Heap>(uint32_t(type), kImageBlockSize, false);
    if (!heap->Allocate(req.size, req.alignment, &im->memory)) Fatal("out of image memory (%ux%u)", width, height);
    VKCHECK(vkBindImageMemory(g_device, im->image, im->memory.block->memory, im->memory.offset));
    if (sampled)
    {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = im->image;
        vi.viewType = array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers};
        VKCHECK(vkCreateImageView(g_device, &vi, nullptr, &im->view));
        WriteImageDescriptor(*im);
    }
    // (GL's new storage reads as zeros on the drivers the game is tested with: cleared, which also gives it a layout;
    // not when the caller's upload covers every texel next: createTexture with pixels)
    if (fClear) QueueClear(*im);
    return im;
}
VkImageView AttachView(Image& im, unsigned level, unsigned layer, unsigned layers = 1)
{
    const uint32_t key = level | (layer << 8) | (layers > 1 ? layers << 24 : 0u);
    for (auto& [k, v] : im.attachViews) if (k == key) return v;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.image;
    vi.viewType = layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = im.format;
    vi.subresourceRange = {im.aspect, level, 1, layer, layers};
    VkImageView v;
    VKCHECK(vkCreateImageView(g_device, &vi, nullptr, &v));
    im.attachViews.emplace_back(key, v);
    return v;
}
void DestroyImage(Image& im)
{
    if (&im == g_endedDepth) g_endedDepth = nullptr;
    for (auto& [k, v] : im.attachViews) vkDestroyImageView(g_device, v, nullptr);
    if (im.view) vkDestroyImageView(g_device, im.view, nullptr);
    if (im.external) return;
    if (im.image) vkDestroyImage(g_device, im.image, nullptr);
    FreeAlloc(im.memory);
}
// An image no longer used: its memory and descriptor slot go when the frame retires.
void ReleaseImage(std::unique_ptr<Image>& im)
{
    if (!im) return;
    if (im->dirty) g_dirty.erase(std::remove(g_dirty.begin(), g_dirty.end(), im.get()), g_dirty.end());
    im->dirty = false;
    if (im->slot >= 0) (im->array ? Cur().garbageSlotsArray : Cur().garbageSlots2D).push_back(uint32_t(im->slot));
    Cur().garbageImages.push_back(std::move(im));
}

// ------------------------------------------------------------------------------------------------ transfers
// A copy, clear or mip chain into a buffer or image, queued on one of two lists: the main list (objects the frame's
// commands already use: executed in order, before the next draw, outside a render pass) or the pre list (objects not
// used yet in this submission: executed before all of its commands, in a command buffer of their own).
struct XferOp
{
    enum Kind : uint8_t { CopyBuffer, BufferToImage, ClearImage, MipChain, CopyImage } kind = CopyBuffer;
    VkBuffer srcBuffer = VK_NULL_HANDLE, dstBuffer = VK_NULL_HANDLE;
    VkDeviceSize srcOffset = 0, dstOffset = 0, size = 0;
    Image* image = nullptr;        // the destination
    Image* source = nullptr;       // (CopyImage)
    uint32_t level = 0, layer = 0, layerCount = 1, x = 0, y = 0, width = 0, height = 0, rowLength = 0;
};
struct XferList
{
    std::vector<XferOp> ops;
    struct Taken { Image* image; VkImageLayout before, after; };
    std::vector<Taken> images;   // the images taken, their layouts before and after the list (the pre list runs before
                                 // commands recorded after the queueing: the image's tracked layout may have moved on)
    uint64_t batch = 0;
};
XferList g_mainXfers, g_preXfers;
uint64_t g_batchCounter = 1;
unsigned g_splits = 0;

void TakeImage(XferList& list, Image& im)
{
    if (im.xferBatch == list.batch) return;
    im.xferBatch = list.batch;
    list.images.push_back({&im, im.layout, FinalLayout(im)});
    // (after the list: readable, or its attachment layout)
    im.layout = FinalLayout(im);
    if (im.dirty) { g_dirty.erase(std::remove(g_dirty.begin(), g_dirty.end(), &im), g_dirty.end()); im.dirty = false; }
}
// Which list an operation on an object goes to (the object's last main use in this submission: in order).
XferList& ListFor(uint64_t useSegment) { return useSegment == g_segment ? g_mainXfers : g_preXfers; }
void QueueImageOp(XferOp op)
{
    Image& im = *op.image;
    XferList& list = (im.resident || (op.source && op.source->useSegment == g_segment)) ? g_mainXfers : ListFor(im.useSegment);
    if (!g_recording) BeginFrame();
    TakeImage(list, im);
    if (op.source) TakeImage(list, *op.source);
    if (&list == &g_mainXfers) { im.useSegment = g_segment; if (op.source) op.source->useSegment = g_segment; }
    list.ops.push_back(op);
}
void QueueClear(Image& im)
{
    XferOp op;
    op.kind = XferOp::ClearImage;
    op.image = &im;
    QueueImageOp(op);
}

void ExecuteXfers(VkCommandBuffer cmd, XferList& list)
{
    if (list.ops.empty()) { list.images.clear(); list.batch = g_batchCounter++; return; }
    Barriers bar;
    bar.Memory(VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
               VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    for (const auto& t : list.images) bar.Image(*t.image, t.before, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    bar.Flush(cmd);
    // (two operations on one object in a batch: a barrier between them, else they may overlap)
    std::vector<const void*> touchedImages;
    struct BufRange { VkBuffer buffer; VkDeviceSize lo, hi; };
    std::vector<BufRange> touchedBuffers;
    auto hazard = [&](const XferOp& op) {
        bool f = false;
        if (op.image) f = std::find(touchedImages.begin(), touchedImages.end(), op.image) != touchedImages.end();
        if (op.source) f = f || std::find(touchedImages.begin(), touchedImages.end(), op.source) != touchedImages.end();
        if (op.kind == XferOp::CopyBuffer)
            for (const BufRange& r : touchedBuffers)
                if (r.buffer == op.dstBuffer && op.dstOffset < r.hi && op.dstOffset + op.size > r.lo) f = true;
        if (f)
        {
            bar.Memory(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
            bar.Flush(cmd);
            touchedImages.clear();
            touchedBuffers.clear();
        }
        if (op.image) touchedImages.push_back(op.image);
        if (op.source) touchedImages.push_back(op.source);
        if (op.kind == XferOp::CopyBuffer) touchedBuffers.push_back({op.dstBuffer, op.dstOffset, op.dstOffset + op.size});
    };
    for (const XferOp& op : list.ops)
    {
        hazard(op);
        if (op.image) ++op.image->writes;
        switch (op.kind)
        {
        case XferOp::CopyBuffer:
        {
            const VkBufferCopy region{op.srcOffset, op.dstOffset, op.size};
            vkCmdCopyBuffer(cmd, op.srcBuffer, op.dstBuffer, 1, &region);
            break;
        }
        case XferOp::BufferToImage:
        {
            VkBufferImageCopy region{};
            region.bufferOffset = op.srcOffset;
            region.bufferRowLength = op.rowLength;
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, op.level, op.layer, op.layerCount};
            region.imageOffset = {int32_t(op.x), int32_t(op.y), 0};
            region.imageExtent = {op.width, op.height, 1};
            vkCmdCopyBufferToImage(cmd, op.srcBuffer, op.image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            break;
        }
        case XferOp::ClearImage:
        {
            if (op.image->depth)
            {
                const VkClearDepthStencilValue v{0.0f, 0};
                const VkImageSubresourceRange r{op.image->aspect, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
                vkCmdClearDepthStencilImage(cmd, op.image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
            }
            else
            {
                const VkClearColorValue v{};
                const VkImageSubresourceRange r{VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS};
                vkCmdClearColorImage(cmd, op.image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &v, 1, &r);
            }
            break;
        }
        case XferOp::CopyImage:
        {
            // (the source's level 0 into the destination's: GenerateMipmap's new chain)
            Barriers sb;
            sb.Image(*op.source, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            sb.Flush(cmd);
            VkImageCopy region{};
            region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, op.source->layers};
            region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, op.source->layers};
            region.extent = {op.source->width, op.source->height, 1};
            vkCmdCopyImage(cmd, op.source->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, op.image->image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            sb.Image(*op.source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            sb.Flush(cmd);
            break;
        }
        case XferOp::MipChain:
        {
            Image& im = *op.image;
            for (uint32_t level = 1; level < im.levels; ++level)
            {
                Barriers lb;
                lb.Image(im, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, level - 1, 1);
                lb.images.back().srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
                lb.Flush(cmd);
                VkImageBlit blit{};
                blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level - 1, 0, im.layers};
                blit.srcOffsets[1] = {int32_t(std::max(1u, im.width >> (level - 1))), int32_t(std::max(1u, im.height >> (level - 1))), 1};
                blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, im.layers};
                blit.dstOffsets[1] = {int32_t(std::max(1u, im.width >> level)), int32_t(std::max(1u, im.height >> level)), 1};
                vkCmdBlitImage(cmd, im.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                               1, &blit, VK_FILTER_LINEAR);
                lb.Image(im, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, level - 1, 1);
                lb.Flush(cmd);
            }
            break;
        }
        }
    }
    bar.Memory(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
               VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT);
    for (const auto& t : list.images)
    {
        bar.Image(*t.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, t.after);
        bar.images.back().srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    }
    bar.Flush(cmd);
    list.ops.clear();
    list.images.clear();
    list.batch = g_batchCounter++;
}

// ------------------------------------------------------------------------------------------------ rendering instances
struct Target
{
    Image* color = nullptr;
    Image* depth = nullptr;
    uint16_t colorLayer = 0, depthLayer = 0;
    uint8_t colorLevel = 0, depthLevel = 0;
    uint8_t views = 0;             // (multiview: the attachments' two views; 0 one)
    bool operator==(const Target&) const = default;
};
Target g_target;                   // the target of the instance being recorded (or about to begin)
bool g_inRendering = false;        // between vkCmdBeginRendering and vkCmdEndRendering
// (the open instance: whether anything wrote its colour / depth-stencil (a clear, a draw that can write them); not
// written: its store becomes STORE_OP_NONE at its end (threaded: still patchable), so a tiler does not write back what
// it loaded unchanged; SLY2N_VKSTORENONE=0: off)
bool g_instWroteColor = false, g_instWroteDepth = false;
unsigned g_storesNone = 0;         // (SLY2N_VKSTATS: stores made NONE)
unsigned g_storesDropped = 0;     // (SLY2N_VKSTATS: instances whose depth store glInvalidateFramebuffer dropped)
bool g_pendingBegin = false;       // g_target set, its instance not begun yet (the next draw begins it)
bool g_fetchUsed = false;          // a CANE_FETCH program exists: colour attachments that allow it begin in the local-read layout
bool g_instanceLocalRead = false;
// (g_feat.classicFetch) the instance is a render pass object's single subpass: its colour attachment is also its input
// attachment (GENERAL), with a by-region self-dependency for the in-place reads (ApplyFetch's barrier)
bool g_instanceClassic = false;
struct ClassicKey
{
    VkFormat color = VK_FORMAT_UNDEFINED, depth = VK_FORMAT_UNDEFINED;
    uint8_t ops[6] = {};   // colour load / store, depth load / store, stencil load / store
    bool fdm = false;      // a fragment density map attachment after them
    uint8_t viewMask = 0;  // (multiview) the subpass's views
    bool operator==(const ClassicKey&) const = default;
};
std::vector<std::pair<ClassicKey, VkRenderPass>> g_classicPasses;
ClassicKey g_instanceClassicKey;   // (the open classic instance's attachments and load / store ops)
struct EndedInstance
{
    VkRenderingAttachmentInfo *depth = nullptr, *stencil = nullptr;
    VkRenderPassBeginInfo* classic = nullptr;
    ClassicKey key;
} g_ended;
VkRenderPass ClassicRenderPass(const ClassicKey& k)
{
    for (const auto& [key, rp] : g_classicPasses)
        if (key == k) return rp;
    VkAttachmentDescription2 at[3]{{VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2}, {VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2},
                                   {VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2}};
    at[0].format = k.color;
    at[0].samples = VK_SAMPLE_COUNT_1_BIT;
    at[0].loadOp = VkAttachmentLoadOp(k.ops[0]);
    at[0].storeOp = VkAttachmentStoreOp(k.ops[1]);
    at[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at[0].initialLayout = at[0].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    at[1].format = k.depth;
    at[1].samples = VK_SAMPLE_COUNT_1_BIT;
    at[1].loadOp = VkAttachmentLoadOp(k.ops[2]);
    at[1].storeOp = VkAttachmentStoreOp(k.ops[3]);
    at[1].stencilLoadOp = VkAttachmentLoadOp(k.ops[4]);
    at[1].stencilStoreOp = VkAttachmentStoreOp(k.ops[5]);
    at[1].initialLayout = at[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference2 cref{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 0, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_ASPECT_COLOR_BIT};
    VkAttachmentReference2 iref = cref;
    VkAttachmentReference2 dref{VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, nullptr, 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT};
    VkSubpassDescription2 sd{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
    sd.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sd.colorAttachmentCount = 1;
    sd.pColorAttachments = &cref;
    sd.inputAttachmentCount = 1;
    sd.pInputAttachments = &iref;
    sd.viewMask = k.viewMask;   // (the input attachment then reads each view's own layer)
    if (k.depth != VK_FORMAT_UNDEFINED) sd.pDepthStencilAttachment = &dref;
    VkSubpassDependency2 dep[3]{{VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2}, {VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2}, {VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2}};
    // (the in-place reads: the earlier draws' colour writes before the input attachment's reads, per region)
    dep[0].srcSubpass = dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[0].dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
    dep[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
    // (before and after: as dynamic rendering's instances, the explicit barriers outside order the rest)
    const VkPipelineStageFlags attach = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    const VkAccessFlags attachAccess = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                       VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                                       VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
    dep[1].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].dstSubpass = 0;
    dep[1].srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[1].dstStageMask = attach;
    dep[1].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    dep[1].dstAccessMask = attachAccess;
    dep[2].srcSubpass = 0;
    dep[2].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[2].srcStageMask = attach;
    dep[2].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[2].srcAccessMask = attachAccess;
    dep[2].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    VkRenderPassCreateInfo2 ci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
    ci.attachmentCount = k.depth != VK_FORMAT_UNDEFINED ? 2 : 1;
    ci.pAttachments = at;
    VkRenderPassFragmentDensityMapCreateInfoEXT fdmci{VK_STRUCTURE_TYPE_RENDER_PASS_FRAGMENT_DENSITY_MAP_CREATE_INFO_EXT};
    if (k.fdm)
    {
        VkAttachmentDescription2& a = at[ci.attachmentCount];
        a.format = VK_FORMAT_R8G8_UNORM;
        a.samples = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        a.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout = a.finalLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
        fdmci.fragmentDensityMapAttachment = {ci.attachmentCount, VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT};
        ++ci.attachmentCount;
        ci.pNext = &fdmci;
    }
    ci.subpassCount = 1;
    ci.pSubpasses = &sd;
    ci.dependencyCount = 3;
    ci.pDependencies = dep;
    // (multiview: the self-dependency stays within a view; the views are correlated, a hint to render them together)
    const uint32_t correlated = k.viewMask;
    if (k.viewMask)
    {
        dep[0].dependencyFlags |= VK_DEPENDENCY_VIEW_LOCAL_BIT;
        dep[0].viewOffset = 0;
        ci.correlatedViewMaskCount = 1;
        ci.pCorrelatedViewMasks = &correlated;
    }
    VkRenderPass rp = VK_NULL_HANDLE;
    VKCHECK(vkCreateRenderPass2(g_device, &ci, nullptr, &rp));
    g_classicPasses.push_back({k, rp});
    return rp;
}
// (imageless: one framebuffer per size and attachments' kind, the views given at each begin)
struct ClassicFbKey
{
    VkRenderPass pass;
    uint32_t width, height;
    VkFormat color, depth;
    VkImageUsageFlags colorUsage, depthUsage;
    uint32_t fdmWidth = 0, fdmHeight = 0;   // (a density map: its size)
    uint32_t layers = 1, fdmLayers = 1;      // (multiview: the attachments' views' layers)
    bool operator==(const ClassicFbKey&) const = default;
};
std::vector<std::pair<ClassicFbKey, VkFramebuffer>> g_classicFbs;
std::vector<std::pair<std::array<VkImageView, 3>, VkFramebuffer>> g_viewFbs;
VkFramebuffer ViewsFramebuffer(VkRenderPass pass, const VkImageView* views, uint32_t n, uint32_t w, uint32_t h)
{
    const std::array<VkImageView, 3> key{views[0], n > 1 ? views[1] : VK_NULL_HANDLE, n > 2 ? views[2] : VK_NULL_HANDLE};
    for (const auto& [k, fb] : g_viewFbs)
        if (k == key) return fb;
    VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    ci.renderPass = pass;
    ci.attachmentCount = n;
    ci.pAttachments = views;
    ci.width = w, ci.height = h, ci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VKCHECK(vkCreateFramebuffer(g_device, &ci, nullptr, &fb));
    g_viewFbs.push_back({key, fb});
    return fb;
}
VkFramebuffer ClassicFramebuffer(const ClassicFbKey& k)
{
    for (const auto& [key, fb] : g_classicFbs)
        if (key == k) return fb;
    VkFramebufferAttachmentImageInfo ai[3]{{VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO}, {VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO},
                                           {VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENT_IMAGE_INFO}};
    ai[0].usage = k.colorUsage;
    ai[0].width = k.width, ai[0].height = k.height, ai[0].layerCount = k.layers;
    ai[0].viewFormatCount = 1, ai[0].pViewFormats = &k.color;
    ai[1].usage = k.depthUsage;
    ai[1].width = k.width, ai[1].height = k.height, ai[1].layerCount = k.layers;
    ai[1].viewFormatCount = 1, ai[1].pViewFormats = &k.depth;
    VkFramebufferAttachmentsCreateInfo aci{VK_STRUCTURE_TYPE_FRAMEBUFFER_ATTACHMENTS_CREATE_INFO};
    aci.attachmentImageInfoCount = k.depth != VK_FORMAT_UNDEFINED ? 2 : 1;
    static const VkFormat kFdmFormat = VK_FORMAT_R8G8_UNORM;
    if (k.fdmWidth)
    {
        VkFramebufferAttachmentImageInfo& f = ai[aci.attachmentImageInfoCount++];
        f.usage = VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        f.width = k.fdmWidth, f.height = k.fdmHeight, f.layerCount = k.fdmLayers;
        f.viewFormatCount = 1, f.pViewFormats = &kFdmFormat;
    }
    aci.pAttachmentImageInfos = ai;
    VkFramebufferCreateInfo ci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, &aci};
    ci.flags = VK_FRAMEBUFFER_CREATE_IMAGELESS_BIT;
    ci.renderPass = k.pass;
    ci.attachmentCount = aci.attachmentImageInfoCount;
    ci.width = k.width, ci.height = k.height, ci.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    VKCHECK(vkCreateFramebuffer(g_device, &ci, nullptr, &fb));
    g_classicFbs.push_back({k, fb});
    return fb;
}  // the instance's colour attachment is in VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR
struct PendingClear { bool color = false, depth = false, stencil = false; VkClearValue colorValue{}, depthValue{}; } g_pendingClear;

void TimePassBegin();
void TimePassEnd(uint32_t kind, uint32_t width, uint32_t height);
void BeginRenderingNow()
{
    VkCommandBuffer cmd = Main();
    Barriers bar;
    for (Image* im : g_dirty)
    {
        im->dirty = false;
        if (im == g_target.color || im == g_target.depth) continue;
        if (im->sampled && im->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            bar.Image(*im, im->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            im->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
    }
    g_dirty.clear();
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingAttachmentInfo stencil{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.layerCount = 1;
    ri.renderArea = {{0, 0}, {b.width, b.height}};
    // (multiview: both eyes in one instance, the attachments' array views; a common render area: the eyes are drawn at
    // one size)
    const unsigned nviews = g_target.views > 1 ? g_target.views : 1;
    ri.viewMask = nviews > 1 ? (1u << nviews) - 1u : 0u;
    VkMultisampledRenderToSingleSampledInfoEXT msi{VK_STRUCTURE_TYPE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_INFO_EXT};
    if (b.msrtss)
    {
        msi.multisampledRenderToSingleSampledEnable = VK_TRUE;
        msi.rasterizationSamples = SampleBits(b.samples);
        ri.pNext = &msi;
    }
    VkRenderingFragmentDensityMapAttachmentInfoEXT fdi{VK_STRUCTURE_TYPE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_INFO_EXT};
    Image* fdmImage = b.fdm && g_target.color ? g_target.color->densityMap : nullptr;
    if (fdmImage)
    {
        // (multiview: a layer a view where the map has them, else the one shared)
        fdi.imageView = AttachView(*fdmImage, 0, 0, nviews > 1 && fdmImage->layers >= nviews ? nviews : 1);
        fdi.imageLayout = VK_IMAGE_LAYOUT_FRAGMENT_DENSITY_MAP_OPTIMAL_EXT;
        fdi.pNext = ri.pNext;
        ri.pNext = &fdi;
    }
    g_instanceLocalRead = false;
    const bool classic = g_feat.classicFetch && g_fetchUsed && g_target.color && g_target.color->inputAttachment && !b.msrtss;
    if (classic != b.classic) b.classic = classic, b.pipelineDirty = true;
    if (Image* im = g_target.color)
    {
        // (a frame read in place by a CANE_FETCH program: the whole instance in the local-read layout; classic: GENERAL)
        const VkImageLayout layout = classic ? VK_IMAGE_LAYOUT_GENERAL
                                   : g_fetchUsed && im->inputAttachment ? VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        g_instanceLocalRead = layout == VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR;
        bar.Image(*im, im->layout, layout);
        im->layout = layout;
        im->useSegment = g_segment;
        color.imageView = AttachView(*im, g_target.colorLevel, g_target.colorLayer, nviews);
        color.imageLayout = layout;
        color.loadOp = g_pendingClear.color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue = g_pendingClear.colorValue;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
    }
    if (Image* im = g_target.depth)
    {
        if (im == g_endedDepth) g_endedDepth = nullptr;
        bar.Image(*im, im->layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        im->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        im->useSegment = g_segment;
        depth.imageView = AttachView(*im, g_target.depthLevel, g_target.depthLayer, nviews);
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = g_pendingClear.depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue = g_pendingClear.depthValue;
        stencil = depth;
        stencil.loadOp = g_pendingClear.stencil ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        if (im->transient)
        {
            // (SetRenderbufferTransient: not stored; an instance that would load it back means the hint was wrong:
            // stored from then on)
            if (depth.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD || stencil.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD)
            {
                im->transient = false;
                std::printf("Vulkan: a transient depth target loaded back: stored from now on\n");
            }
            else depth.storeOp = stencil.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        }
        ri.pDepthAttachment = &depth;
        ri.pStencilAttachment = &stencil;
    }
    bar.Flush(cmd);
    if (g_target.color) ++g_target.color->writes;
    if (g_target.depth) ++g_target.depth->writes;
    TimePassBegin();
    g_instanceClassic = classic;
    if (classic)
    {
        ClassicKey ck;
        ck.color = g_target.color->format;
        ck.ops[0] = uint8_t(color.loadOp), ck.ops[1] = uint8_t(color.storeOp);
        if (g_target.depth)
        {
            ck.depth = g_target.depth->format;
            ck.ops[2] = uint8_t(depth.loadOp), ck.ops[3] = uint8_t(depth.storeOp);
            ck.ops[4] = uint8_t(stencil.loadOp), ck.ops[5] = uint8_t(stencil.storeOp);
        }
        ck.fdm = fdmImage != nullptr;
        ck.viewMask = uint8_t(ri.viewMask);
        g_instanceClassicKey = ck;
        g_recClassic = nullptr;   // (set by the stream's vkCmdBeginRenderPass2 below)
        const VkRenderPass rp = ClassicRenderPass(ck);
        ClassicKey compat;
        compat.color = ck.color, compat.depth = ck.depth, compat.fdm = ck.fdm, compat.viewMask = ck.viewMask;
        VkImageView views[3] = {color.imageView, VK_NULL_HANDLE, VK_NULL_HANDLE};
        uint32_t cview = 1;
        if (g_target.depth) views[cview++] = depth.imageView;
        if (fdmImage) views[cview++] = fdi.imageView;
        // (an OpenXR image: a framebuffer of its views, its usage and view formats being the runtime's; else imageless)
        const VkFramebuffer fb = g_target.color->external
                                     ? ViewsFramebuffer(ClassicRenderPass(compat), views, cview, b.width, b.height)
                                     : ClassicFramebuffer({ClassicRenderPass(compat), b.width, b.height, ck.color, ck.depth,
                                                           g_target.color->usage, g_target.depth ? g_target.depth->usage : 0,
                                                           fdmImage ? fdmImage->width : 0, fdmImage ? fdmImage->height : 0, nviews,
                                                           fdmImage && nviews > 1 && fdmImage->layers >= nviews ? nviews : 1u});
        VkClearValue clears[3] = {color.clearValue, depth.clearValue, {}};
        clears[1].depthStencil.stencil = stencil.clearValue.depthStencil.stencil;
        VkRenderPassAttachmentBeginInfo abi{VK_STRUCTURE_TYPE_RENDER_PASS_ATTACHMENT_BEGIN_INFO};
        abi.attachmentCount = cview;
        abi.pAttachments = views;
        VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, g_target.color->external ? nullptr : &abi};
        rb.renderPass = rp;
        rb.framebuffer = fb;
        rb.renderArea = ri.renderArea;
        rb.clearValueCount = abi.attachmentCount;
        rb.pClearValues = clears;
        VkSubpassBeginInfo sb{VK_STRUCTURE_TYPE_SUBPASS_BEGIN_INFO};
        sb.contents = VK_SUBPASS_CONTENTS_INLINE;
        vkCmdBeginRenderPass2(cmd, &rb, &sb);
    }
    else vkCmdBeginRendering(cmd, &ri);
    g_inRendering = true;
    g_pendingBegin = false;
    g_instWroteColor = g_pendingClear.color;
    g_instWroteDepth = g_pendingClear.depth || g_pendingClear.stencil;
    g_pendingClear = PendingClear{};
    ++g_stats.renderPasses;
}
void EnsureRendering()
{
    if (g_pendingBegin) BeginRenderingNow();
}
void StoreNoneUnwritten()
{
    static const bool s_fOff = std::getenv("SLY2N_VKSTORENONE") && std::atoi(std::getenv("SLY2N_VKSTORENONE")) == 0;
    if (s_fOff) return;
    const bool color = !g_instWroteColor && g_target.color && !g_target.color->external;
    const bool depth = !g_instWroteDepth && g_target.depth;
    if (!color && !depth) return;
    if (g_recColor || g_recDepth)
    {
        if (color && g_recColor && g_recColor->storeOp == VK_ATTACHMENT_STORE_OP_STORE) g_recColor->storeOp = VK_ATTACHMENT_STORE_OP_NONE, ++g_storesNone;
        if (depth && g_recDepth && g_recStencil && g_recDepth->storeOp == VK_ATTACHMENT_STORE_OP_STORE && g_recStencil->storeOp == VK_ATTACHMENT_STORE_OP_STORE)
        {
            g_recDepth->storeOp = g_recStencil->storeOp = VK_ATTACHMENT_STORE_OP_NONE;
            ++g_storesNone;
        }
    }
    else if (g_recClassic && g_instanceClassic)
    {
        ClassicKey& ck = g_instanceClassicKey;
        bool changed = false;
        if (color && ck.ops[1] == uint8_t(VK_ATTACHMENT_STORE_OP_STORE)) ck.ops[1] = uint8_t(VK_ATTACHMENT_STORE_OP_NONE), changed = true, ++g_storesNone;
        if (depth && ck.depth != VK_FORMAT_UNDEFINED && ck.ops[3] == uint8_t(VK_ATTACHMENT_STORE_OP_STORE) && ck.ops[5] == uint8_t(VK_ATTACHMENT_STORE_OP_STORE))
            ck.ops[3] = ck.ops[5] = uint8_t(VK_ATTACHMENT_STORE_OP_NONE), changed = true, ++g_storesNone;
        if (changed) g_recClassic->renderPass = ClassicRenderPass(ck);
    }
}
// The instance ends (a transfer, a read-back, another target, the submission); the next draw on the target resumes it.
void EndRendering()
{
    if (g_pendingBegin && (g_pendingClear.color || g_pendingClear.depth || g_pendingClear.stencil)) BeginRenderingNow();
    if (!g_inRendering) return;
    StoreNoneUnwritten();
    g_endedDepth = nullptr;
    if (g_target.depth && (g_recDepth || (g_instanceClassic && g_recClassic)))
    {
        g_ended.depth = g_recDepth, g_ended.stencil = g_recStencil;
        g_ended.classic = g_instanceClassic ? g_recClassic : nullptr;
        g_ended.key = g_instanceClassicKey;
        g_endedDepth = g_target.depth;
    }
    if (g_instanceClassic)
    {
        VkSubpassEndInfo ei{VK_STRUCTURE_TYPE_SUBPASS_END_INFO};
        vkCmdEndRenderPass2(g_main, &ei);
        g_instanceClassic = false;
    }
    else vkCmdEndRendering(g_main);
    TimePassEnd(0, 0, 0);
    g_inRendering = false;
    g_recColor = g_recDepth = g_recStencil = nullptr;
    g_recClassic = nullptr;
    g_invalidatedColor = g_invalidatedDepth = false;
    g_pendingBegin = true;
    if (g_target.color) MarkDirty(*g_target.color);
    if (g_target.depth) MarkDirty(*g_target.depth);
}
void FlushCopies()
{
    if (g_mainXfers.ops.empty()) return;
    if (g_inRendering) ++g_splits;
    EndRendering();
    ExecuteXfers(Main(), g_mainXfers);
}

// ------------------------------------------------------------------------------------------------ frame lifetime
void Persist(Buffer& buf);
void Submit(VkSemaphore wait = VK_NULL_HANDLE, VkSemaphore signal = VK_NULL_HANDLE);
// The frame's transient memory reused within the frame: everything recorded so far runs to completion first, and the
// stream buffers still in the ring move to persistent storage (they may be drawn later in the frame).
void DrainRing()
{
    Frame& f = Cur();
    for (GLuint name : f.streamBuffers)
    {
        Buffer* buf = g_buffers.Get(name);
        if (buf && buf->stream && buf->serial == f.serial && buf->size) Persist(*buf);
    }
    f.streamBuffers.clear();
    Submit();
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &g_timeline;
    wi.pValues = &g_submitValue;
    VKCHECK(vkWaitSemaphores(g_device, &wi, UINT64_MAX));
    for (RingChunk& c : f.ring) c.cursor = 0;
    f.ringIndex = 0;
}
void WaitValue(uint64_t value)
{
    if (!value) return;
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &g_timeline;
    wi.pValues = &value;
    VKCHECK(vkWaitSemaphores(g_device, &wi, UINT64_MAX));
}
// (SLY2N_VKGPUTIME=N: every N frames, the GPU time of each render-pass instance by its place in the frame and their sum,
// averaged, stderr. A bottom-of-pipe timestamp before each instance's begin and after its end: what the instance adds
// once the work before it is done.)
constexpr uint32_t kPassTimes = 64;
PFN_vkResetQueryPool g_vkResetQueryPool = nullptr;
int g_gpuTimeEvery = 0;
VkQueryPool g_passPool = VK_NULL_HANDLE;
struct PassTimeSum { double ms = 0; uint32_t n = 0; std::array<uint32_t, 3> info{}; };
std::array<PassTimeSum, kPassTimes> g_passSums;
double g_passFrameMs = 0, g_passSpanMs = 0;
constexpr uint32_t kMarks = 256;
VkQueryPool g_markPool = VK_NULL_HANDLE;
struct MarkSum { double ms = 0; uint32_t n = 0; uint32_t order = 0; };
std::map<std::string, MarkSum> g_markSums;
uint32_t g_markOrder = 0;
uint64_t g_markLastEnd = 0;
void ReadMarks(Frame& f, uint32_t slot)
{
    if (!g_markPool) return;
    const uint32_t base = slot * kMarks;
    const uint32_t n = uint32_t(f.markLabels.size());
    if (n > 1)
    {
        std::vector<uint64_t> t(n);
        if (vkGetQueryPoolResults(g_device, g_markPool, base, n, sizeof(uint64_t) * n, t.data(), 8,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
        {
            // (between the frames: the last mark of the frame before to this one's first: the swap blit, idle time)
            if (g_markLastEnd && t[0] > g_markLastEnd)
            {
                MarkSum& m = g_markSums["(between frames)"];
                if (!m.n && m.ms == 0) m.order = g_markOrder++;
                m.ms += double(t[0] - g_markLastEnd) * double(g_props.limits.timestampPeriod) * 1e-6;
                ++m.n;
            }
            g_markLastEnd = t[n - 1];
            for (uint32_t i = 0; i + 1 < n; ++i)
            {
                MarkSum& m = g_markSums[f.markLabels[i]];
                if (!m.n && m.ms == 0) m.order = g_markOrder++;
                m.ms += t[i + 1] > t[i] ? double(t[i + 1] - t[i]) * double(g_props.limits.timestampPeriod) * 1e-6 : 0.0;
                ++m.n;
            }
        }
    }
    g_vkResetQueryPool(g_device, g_markPool, base, kMarks);
    f.markLabels.clear();
}
void PrintMarks(int frames)
{
    std::vector<std::pair<uint32_t, const std::pair<const std::string, MarkSum>*>> v;
    for (const auto& kv : g_markSums) v.push_back({kv.second.order, &kv});
    std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::fprintf(stderr, "vkmarks:");
    for (const auto& e : v) std::fprintf(stderr, " [%s %.3f x%.1f]", e.second->first.c_str(), e.second->second.ms / frames, double(e.second->second.n) / frames);
    std::fprintf(stderr, "\n");
    g_markSums.clear();
    g_markOrder = 0;
}
int g_passFrames = 0;
void ReadPassTimes(Frame& f, uint32_t slot)
{
    if (!g_passPool) return;
    const uint32_t base = slot * kPassTimes * 2;
    if (f.passTimed)
    {
        std::array<uint64_t, kPassTimes * 2> t{};
        if (vkGetQueryPoolResults(g_device, g_passPool, base, f.passTimed * 2, sizeof(uint64_t) * f.passTimed * 2, t.data(), 8,
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
        {
            double frame = 0;
            for (uint32_t i = 0; i < f.passTimed; ++i)
            {
                const double ms = t[2 * i + 1] > t[2 * i] ? double(t[2 * i + 1] - t[2 * i]) * double(g_props.limits.timestampPeriod) * 1e-6 : 0.0;
                g_passSums[i].ms += ms, ++g_passSums[i].n, g_passSums[i].info = f.passInfo[i];
                frame += ms;
            }
            g_passFrameMs += frame;
            if (t[2 * f.passTimed - 1] > t[0]) g_passSpanMs += double(t[2 * f.passTimed - 1] - t[0]) * double(g_props.limits.timestampPeriod) * 1e-6;
            if (++g_passFrames == g_gpuTimeEvery)
            {
                std::fprintf(stderr, "vkgpu: %.3f ms a frame in render passes and transfers (first to last %.3f):", g_passFrameMs / g_passFrames,
                             g_passSpanMs / g_passFrames);
                for (uint32_t i = 0; i < kPassTimes; ++i)
                    if (g_passSums[i].n)
                    {
                        const uint32_t k = g_passSums[i].info[2];
                        std::fprintf(stderr, " [%u %ux%u%s %.3f]", i, g_passSums[i].info[0], g_passSums[i].info[1],
                                     k == 1 ? "c" : k == 2 ? "d" : k == 3 ? "cd" : k == 4 ? "r" : k == 8 ? "x" : k == 32 ? "s" : "b", g_passSums[i].ms / g_passFrames);
                    }
                std::fprintf(stderr, "\n");
                g_passSums = {};
                if (g_markPool) PrintMarks(g_passFrames);
                g_passFrameMs = 0, g_passSpanMs = 0, g_passFrames = 0;
            }
        }
    }
    g_vkResetQueryPool(g_device, g_passPool, base, kPassTimes * 2);
    f.passTimed = 0;
    f.passInfo.clear();
}
void TimePassBegin()
{
    Frame& f = Cur();
    if (!g_passPool || f.passTimed >= kPassTimes) return;
    vkCmdWriteTimestamp(Main(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_passPool, g_frameIndex * kPassTimes * 2 + f.passTimed * 2);
}
// (kind: 0 a render pass (its attachments from the target), else a transfer outside the passes: 4 resolve, 8 copy, 16 blit)
void TimePassEnd(uint32_t kind, uint32_t width, uint32_t height)
{
    Frame& f = Cur();
    if (!g_passPool || f.passTimed >= kPassTimes) return;
    vkCmdWriteTimestamp(Main(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_passPool, g_frameIndex * kPassTimes * 2 + f.passTimed * 2 + 1);
    if (kind) f.passInfo.push_back({width, height, kind});
    else f.passInfo.push_back({b.width, b.height, uint32_t((g_target.color ? 1 : 0) | (g_target.depth ? 2 : 0))});
    ++f.passTimed;
}
void RetireFrame(Frame& f)
{
    WaitValue(f.lastValue);   // (the CPU two frames ahead of the GPU waits here)
    ReadMarks(f, uint32_t(&f - g_frames.data()));
    ReadPassTimes(f, uint32_t(&f - g_frames.data()));
    for (VkDescriptorPool pool : f.setPools) vkResetDescriptorPool(g_device, pool, 0);
    f.setPoolIndex = 0;
    f.uniformSets.clear();
    for (const Alloc& a : f.garbage) FreeAlloc(a);
    f.garbage.clear();
    for (auto& im : f.garbageImages) DestroyImage(*im);
    f.garbageImages.clear();
    for (uint32_t slot : f.garbageSlots2D) g_freeSlots2D.push_back(slot);
    for (uint32_t slot : f.garbageSlotsArray) g_freeSlotsArray.push_back(slot);
    f.garbageSlots2D.clear();
    f.garbageSlotsArray.clear();
    for (GLuint name : f.streamBuffers)
    {
        Buffer* buf = g_buffers.Get(name);
        if (buf && buf->stream && buf->serial == f.serial && buf->size) Persist(*buf);
    }
    f.streamBuffers.clear();
}
void BeginSegment()
{
    if (g_threaded)
    {
        g_main = kStreamMain;
        g_streamMain->Clear();
        g_streamPre->Clear();
    }
    else g_main = AllocCommandBuffer(Cur());
    g_pre = VK_NULL_HANDLE;
    InvalidateBound();
    vkCmdBindDescriptorSets(g_main, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipelineLayout, 0, 1, &g_textureSet, 0, nullptr);
    g_inRendering = false;
    g_pendingBegin = false;
    g_target = Target{};
    g_mainXfers.batch = g_batchCounter++;
    if (g_preXfers.batch == 0) g_preXfers.batch = g_batchCounter++;
}
void ResizeWindowImage();
bool g_frameNeedsReset = false;
struct PendingPresent { bool present = false; } g_pendingPresent;
void SubmitBatch(VkCommandBuffer* cmds, uint32_t n, uint64_t value, VkSemaphore wait, VkSemaphore signal)
{
    VkSemaphore signals[2] = {g_timeline, signal};
    const uint64_t values[2] = {value, 0};
    VkTimelineSemaphoreSubmitInfo ti{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
    const uint64_t waitValue = 0;
    ti.waitSemaphoreValueCount = wait ? 1 : 0;
    ti.pWaitSemaphoreValues = &waitValue;
    ti.signalSemaphoreValueCount = signal ? 2 : 1;
    ti.pSignalSemaphoreValues = values;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.pNext = &ti;
    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    si.waitSemaphoreCount = wait ? 1 : 0;
    si.pWaitSemaphores = &wait;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = n;
    si.pCommandBuffers = cmds;
    si.signalSemaphoreCount = signal ? 2 : 1;
    si.pSignalSemaphores = signals;
    const VkResult r = vkQueueSubmit(g_queue, 1, &si, VK_NULL_HANDLE);
    if (r == VK_ERROR_DEVICE_LOST || FForcedFailure("devicelost")) Fatal("the GPU device was lost (a command faulted)");
    VKCHECK(r);
}
void BeginFrame()
{
    Frame& f = Cur();
    g_recording = true;   // (first: retiring may persist buffers, which must not begin another frame)
    RetireFrame(f);
    if (g_threaded) g_frameNeedsReset = true;   // (the worker resets the pool at the frame's first batch)
    else
    {
        VKCHECK(vkResetCommandPool(g_device, f.pool, 0));
        f.cmdNext = 0;
    }
    for (RingChunk& c : f.ring) c.cursor = 0;
    f.ringIndex = 0;
    f.serial = g_nextSerial++;
    BeginSegment();
    ResizeWindowImage();
}
// The commands recorded so far to the GPU: the pre list first (its own command buffer), then the main one.
void Submit(VkSemaphore wait, VkSemaphore signal)
{
    if (!g_recording) return;
    EndRendering();
    g_endedDepth = nullptr;
    FlushCopies();
    // (no image waits in a transfer or attachment layout between submissions: the sampled ones back to readable)
    {
        Barriers bar;
        for (Image* im : g_dirty)
        {
            im->dirty = false;
            if (im->sampled && im->layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            {
                bar.Image(*im, im->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
                im->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
        }
        g_dirty.clear();
        bar.Flush(g_main);
    }
    if (g_threaded)
    {
        if (!g_preXfers.ops.empty())
        {
            g_pre = kStreamPre;
            ExecuteXfers(g_pre, g_preXfers);
        }
        else g_preXfers.images.clear(), g_preXfers.batch = g_batchCounter++;
        Batch bt;
        bt.pre = g_streamPre->Empty() ? nullptr : g_streamPre;
        bt.main = g_streamMain;
        bt.slot = g_frameIndex;
        bt.resetPool = g_frameNeedsReset;
        g_frameNeedsReset = false;
        bt.value = ++g_submitValue;
        bt.present = g_pendingPresent.present;
        g_pendingPresent = PendingPresent{};
        bt.after.swap(g_afterSubmit);
        if (!bt.pre) g_streamPre->Clear();
        Stream* spareMain = TakeStream();
        Stream* sparePre = bt.pre ? TakeStream() : g_streamPre;
        {
            std::lock_guard<std::mutex> lock(g_workerMutex);
            g_batches.push_back(bt);
        }
        g_workerCv.notify_one();
        g_streamMain = spareMain;
        g_streamPre = sparePre;
        Cur().lastValue = bt.value;
        ++g_segment;
        ++g_stats.submits;
        BeginSegment();
        return;
    }
    VKCHECK(vkEndCommandBuffer(g_main));
    VkCommandBuffer cmds[2];
    uint32_t n = 0;
    if (!g_preXfers.ops.empty())
    {
        g_pre = AllocCommandBuffer(Cur());
        ExecuteXfers(g_pre, g_preXfers);
        VKCHECK(vkEndCommandBuffer(g_pre));
        cmds[n++] = g_pre;
    }
    else g_preXfers.images.clear(), g_preXfers.batch = g_batchCounter++;
    cmds[n++] = g_main;
    const uint64_t value = ++g_submitValue;
    SubmitBatch(cmds, n, value, wait, signal);
    if (!g_afterSubmit.empty())
    {
        std::vector<std::function<void()>> after;
        after.swap(g_afterSubmit);
        for (auto& fn : after) fn();
    }
    Cur().lastValue = value;
    ++g_segment;
    ++g_stats.submits;
    BeginSegment();
}
void Finish()
{
    if (!g_recording) { WaitValue(g_submitValue); return; }
    Submit();
    WaitValue(g_submitValue);
}

// ------------------------------------------------------------------------------------------------ buffers
GLuint& BufferBinding(GLenum target)
{
    switch (target)
    {
    case GL_ARRAY_BUFFER: return s.arrayBuffer;
    case GL_ELEMENT_ARRAY_BUFFER: return g_arrays.Get(s.vertexArray)->elements;
    case GL_UNIFORM_BUFFER: return s.uniformBuffer;
    case GL_SHADER_STORAGE_BUFFER: return s.storageBuffer;
    case GL_DRAW_INDIRECT_BUFFER: return s.indirectBuffer;
    case GL_COPY_READ_BUFFER: return s.copyRead;
    case GL_COPY_WRITE_BUFFER: return s.copyWrite;
    case GL_PIXEL_UNPACK_BUFFER: return s.pixelUnpack;
    case GL_PIXEL_PACK_BUFFER: return s.pixelPack;
    default:
        static GLuint s_none;
        s_none = 0;
        return s_none;
    }
}
void APIENTRY GenBuffers(GLsizei n, GLuint* names) { for (GLsizei i = 0; i < n; ++i) names[i] = g_buffers.Gen(); }
void APIENTRY BindBuffer(GLenum target, GLuint name)
{
    if (name) g_buffers.Ensure(name);
    if (target == GL_ELEMENT_ARRAY_BUFFER)
    {
        g_arrays.Get(s.vertexArray)->elements = name;
        return;
    }
    BufferBinding(target) = name;
}
void ReleaseStorage(Buffer& buf)
{
    if (buf.storage) Cur().garbage.push_back(buf.storage);
    buf.storage = {};
    buf.vk = VK_NULL_HANDLE;
    buf.offset = 0;
    buf.cpu = nullptr;
}
void APIENTRY DeleteBuffers(GLsizei n, const GLuint* names)
{
    for (GLsizei i = 0; i < n; ++i)
    {
        const GLuint name = names[i];
        auto buf = g_buffers.Take(name);
        if (!buf) continue;
        if (!g_recording) BeginFrame();
        ReleaseStorage(*buf);
        for (GLuint* binding : {&s.arrayBuffer, &s.uniformBuffer, &s.storageBuffer, &s.indirectBuffer, &s.copyRead, &s.copyWrite, &s.pixelUnpack, &s.pixelPack})
            if (*binding == name) *binding = 0;
        g_arrays.ForEach([&](GLuint, VertexArray& vao) {
            bool changed = false;
            if (vao.elements == name) { vao.elements = 0; changed = true; }
            for (Attrib& a : vao.attribs) if (a.buffer == name) { a.buffer = 0; changed = true; }
            if (changed) ++vao.version;
        });
        if (VertexArray* vao0 = g_arrays.Get(0))
        {
            if (vao0->elements == name) vao0->elements = 0;
            for (Attrib& a : vao0->attribs) if (a.buffer == name) a.buffer = 0;
        }
        for (Range& r : s.storage) if (r.buffer == name) r = {};
        ++g_addrEpoch;
        ++g_storageEpoch;
    }
}
// New storage for a buffer (glBufferData): transient for stream buffers, persistent otherwise.
void Respecify(GLuint name, Buffer& buf, uint32_t size, GLenum usage)
{
    const bool stream = usage == GL_STREAM_DRAW || usage == GL_STREAM_READ || usage == GL_STREAM_COPY;
    // (the ring memory first: taking it may drain the frame's ring, which moves this buffer's old contents if they
    // live there; the old storage is let go after)
    const Upload u = stream && size ? Ring(size, 256) : Upload{VK_NULL_HANDLE, 0, nullptr};
    if (!g_recording) BeginFrame();
    ReleaseStorage(buf);
    buf.size = size;
    buf.stream = stream;
    buf.bound = false;
    buf.useSegment = 0;
    ++g_addrEpoch;
    if (!size) return;
    if (buf.stream)
    {
        buf.vk = u.buffer;
        buf.offset = u.offset;
        buf.cpu = u.cpu;
        buf.serial = Cur().serial;
        Cur().streamBuffers.push_back(name);
    }
    else
    {
        buf.storage = AllocBuffer(size, 256);
        buf.vk = buf.storage.Buffer();
        buf.offset = buf.storage.offset;
        buf.cpu = buf.storage.Cpu();
    }
}
// A stream buffer's contents into persistent storage (its ring memory is about to be, or may soon be, reused).
void Persist(Buffer& buf)
{
    if (!buf.cpu || !buf.size) return;
    Alloc a = AllocBuffer(buf.size, 256);
    std::memcpy(a.Cpu(), buf.cpu, buf.size);
    buf.storage = a;
    buf.stream = false;
    buf.vk = a.Buffer();
    buf.offset = a.offset;
    buf.cpu = a.Cpu();
    buf.bound = false;
    buf.useSegment = 0;
    ++g_addrEpoch;
}
// (glBufferData on a buffer by name: the GL call's body and the bufferData / createBuffer packets'; indirect: the buffer's
// target is GL_DRAW_INDIRECT_BUFFER)
void BufferDataOn(GLuint name, GLsizeiptr size, const void* data, GLenum usage, bool indirect)
{
    const bool fRes = usage != GL_STREAM_DRAW;
    ResTime rt(fRes);
    struct BufTime   // (SLY2N_VKRESLOG: the buffers' own share)
    {
        bool on; std::chrono::steady_clock::time_point t0;
        ~BufTime() { if (on) g_resFrame.bufferMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); }
    } bt{g_resLogOn && fRes, g_resLogOn && fRes ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}};
    if (g_resLogOn && fRes) ++g_resFrame.buffers, g_resFrame.bufferBytes += uint64_t(std::max<GLsizeiptr>(size, 0));
    Buffer* buf = g_buffers.Get(name);
    if (!buf || size < 0 || uint64_t(size) > 0x7FFFFFFF) { SetError(GL_INVALID_OPERATION); return; }
    if (indirect && !g_feat.mdi) buf->keepShadow = true;
    Respecify(name, *buf, uint32_t(size), usage);
    ++buf->version;
    if (data && size) std::memcpy(buf->cpu, data, size_t(size));
    if (buf->keepShadow)
    {
        buf->shadow.resize(size_t(size));
        if (data && size) std::memcpy(buf->shadow.data(), data, size_t(size));
    }
}
void APIENTRY BufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage)
{ BufferDataOn(BufferBinding(target), size, data, usage, target == GL_DRAW_INDIRECT_BUFFER); }
// Where BufferSubData's size bytes at offset go (the caller writes them there at once): in place while nothing
// recorded reads this storage, else a copy on write (a stream buffer of this frame) or a transfer from ring memory.
uint8_t* SubDataTarget(GLuint name, Buffer& buf, GLintptr offset, GLsizeiptr size)
{
    if (!buf.bound)
    {
        // nothing recorded reads this storage yet: written in place (a stream buffer from an earlier frame moves to
        // persistent storage first: that frame's ring is not ours)
        if (buf.stream && buf.serial != Cur().serial) Persist(buf);
        return buf.cpu + offset;
    }
    if (buf.stream && buf.serial == Cur().serial)
    {
        // read by an earlier draw of this frame: the later draws get a copy with the update (copy on write)
        const Upload u = Ring(buf.size, 256);
        if (buf.stream)
        {
            std::memcpy(u.cpu, buf.cpu, buf.size);
            buf.vk = u.buffer;
            buf.offset = u.offset;
            buf.cpu = u.cpu;
            buf.bound = false;
            buf.useSegment = 0;
            ++g_addrEpoch;
            Cur().streamBuffers.push_back(name);
            return u.cpu + offset;
        }
        if (!buf.bound) return buf.cpu + offset;
    }
    if (buf.stream)
    {
        Persist(buf);
        return buf.cpu + offset;
    }
    const Upload u = Ring(uint32_t(size), 16);
    XferOp op;
    op.kind = XferOp::CopyBuffer;
    op.srcBuffer = u.buffer;
    op.srcOffset = u.offset;
    op.dstBuffer = buf.vk;
    op.dstOffset = buf.offset + VkDeviceSize(offset);
    op.size = VkDeviceSize(size);
    XferList& list = ListFor(buf.useSegment);
    list.ops.push_back(op);
    return u.cpu;
}
void BufferSubDataOn(GLuint name, GLintptr offset, GLsizeiptr size, const void* data, bool indirect)
{
    Buffer* buf = g_buffers.Get(name);
    if (!buf || offset < 0 || size < 0 || uint64_t(offset) + uint64_t(size) > buf->size) { SetError(GL_INVALID_VALUE); return; }
    if (!size || !data) return;
    if (!g_recording) BeginFrame();
    if (indirect && !g_feat.mdi && !buf->keepShadow)
    {
        buf->keepShadow = true;
        buf->shadow.assign(buf->size, 0);
    }
    if (buf->keepShadow) std::memcpy(buf->shadow.data() + offset, data, size_t(size));
    ++buf->version;
    std::memcpy(SubDataTarget(name, *buf, offset, size), data, size_t(size));
}
void APIENTRY BufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data)
{ BufferSubDataOn(BufferBinding(target), offset, size, data, target == GL_DRAW_INDIRECT_BUFFER); }
// (not supported: the caller falls back)
void* APIENTRY MapBufferRange(GLenum, GLintptr, GLsizeiptr, GLbitfield) { return nullptr; }
GLboolean APIENTRY UnmapBuffer(GLenum) { return GL_TRUE; }
GLboolean APIENTRY IsBuffer(GLuint name) { return g_buffers.Get(name) ? GL_TRUE : GL_FALSE; }
void APIENTRY BindBufferRange(GLenum target, GLuint index, GLuint name, GLintptr offset, GLsizeiptr size)
{
    if (index >= kMaxBindings || offset < 0 || size < 0) { SetError(GL_INVALID_VALUE); return; }
    if (name) g_buffers.Ensure(name);
    if (target != GL_SHADER_STORAGE_BUFFER) { BufferBinding(target) = name; return; }   // (no uniform blocks in Sly 2)
    s.storage[index] = {name, uint32_t(offset), uint32_t(size), false};
    ++g_storageEpoch;
    BufferBinding(target) = name;
}
void APIENTRY BindBufferBase(GLenum target, GLuint index, GLuint name)
{
    BindBufferRange(target, index, name, 0, 0);
    if (index < kMaxBindings && target == GL_SHADER_STORAGE_BUFFER) s.storage[index].whole = true;
}
// A buffer's storage for a command recorded now. A stream buffer filled in an earlier frame (not respecified since)
// moves to persistent storage first: its ring memory is only kept until that frame retires.
void Use(Buffer& buf)
{
    if (buf.stream && buf.serial != Cur().serial && buf.size) Persist(buf);
    buf.bound = true;
    buf.useSegment = g_segment;
}

// ------------------------------------------------------------------------------------------------ vertex arrays
void APIENTRY GenVertexArrays(GLsizei n, GLuint* names) { for (GLsizei i = 0; i < n; ++i) names[i] = g_arrays.Gen(); }
void APIENTRY BindVertexArray(GLuint name)
{
    if (name && !g_arrays.Get(name)) g_arrays.Ensure(name);
    s.vertexArray = name;
}
void APIENTRY DeleteVertexArrays(GLsizei n, const GLuint* names)
{
    for (GLsizei i = 0; i < n; ++i)
    {
        if (!names[i]) continue;
        if (auto vao = g_arrays.Take(names[i]))
        {
            if (b.vao == vao.get()) b.vao = nullptr;
            if (b.vertexVao == vao.get()) b.vertexVao = nullptr;
        }
        if (s.vertexArray == names[i]) s.vertexArray = 0;
    }
}
unsigned ComponentBytes(GLenum type)
{
    switch (type)
    {
    case GL_FLOAT: case GL_INT: case GL_UNSIGNED_INT: return 4;
    case GL_HALF_FLOAT: case GL_SHORT: case GL_UNSIGNED_SHORT: return 2;
    default: return 1;
    }
}
void SetAttrib(GLuint index, GLint components, GLenum type, bool normalized, GLsizei stride, const void* pointer, bool integer)
{
    if (index >= kGenericBinding || components < 1 || components > 4 || stride < 0) { SetError(GL_INVALID_VALUE); return; }
    VertexArray* vao = g_arrays.Get(s.vertexArray);
    Attrib& a = vao->attribs[index];
    a.buffer = s.arrayBuffer;
    a.components = uint8_t(components);
    a.type = type;
    a.normalized = normalized;
    a.integer = integer;
    a.offset = uint32_t(reinterpret_cast<uintptr_t>(pointer));
    a.stride = stride ? uint32_t(stride) : uint32_t(components) * ComponentBytes(type);
    ++vao->version;
}
void APIENTRY VertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer)
{ SetAttrib(index, size, type, normalized == GL_TRUE, stride, pointer, false); }
void APIENTRY VertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride, const void* pointer)
{ SetAttrib(index, size, type, false, stride, pointer, true); }
void APIENTRY EnableVertexAttribArray(GLuint index)
{
    if (index >= kGenericBinding) return;
    VertexArray* vao = g_arrays.Get(s.vertexArray);
    if (!vao->attribs[index].enabled) { vao->attribs[index].enabled = true; ++vao->version; }
}
void APIENTRY DisableVertexAttribArray(GLuint index)
{
    if (index >= kGenericBinding) return;
    VertexArray* vao = g_arrays.Get(s.vertexArray);
    if (vao->attribs[index].enabled) { vao->attribs[index].enabled = false; ++vao->version; }
}
void APIENTRY VertexAttribDivisor(GLuint index, GLuint divisor)
{
    if (index >= kGenericBinding) return;
    VertexArray* vao = g_arrays.Get(s.vertexArray);
    vao->attribs[index].divisor = divisor;
    ++vao->version;
}

// ------------------------------------------------------------------------------------------------ textures
Texture* BoundTexture(GLenum target)
{
    const GLuint name = s.units[s.activeUnit][target == GL_TEXTURE_2D_ARRAY ? 1 : 0];
    return g_textures.Get(name);
}
void APIENTRY GenTextures(GLsizei n, GLuint* names) { for (GLsizei i = 0; i < n; ++i) names[i] = g_textures.Gen(); }
void APIENTRY ActiveTexture(GLenum unit)
{
    if (unit < GL_TEXTURE0 || unit >= GL_TEXTURE0 + kMaxUnits) { SetError(GL_INVALID_ENUM); return; }
    s.activeUnit = unit - GL_TEXTURE0;
}
void APIENTRY BindTexture(GLenum target, GLuint name)
{
    if (target != GL_TEXTURE_2D && target != GL_TEXTURE_2D_ARRAY) return;
    if (name) g_textures.Ensure(name);
    s.units[s.activeUnit][target == GL_TEXTURE_2D_ARRAY ? 1 : 0] = name;
}
void DetachEverywhere(GLuint name, bool renderbuffer)
{
    g_framebuffers.ForEach([&](GLuint, Framebuffer& f) {
        if (f.color.object == name && f.color.renderbuffer == renderbuffer) f.color = {};
        if (f.depth.object == name && f.depth.renderbuffer == renderbuffer) f.depth = {};
    });
    b.targetDirty = true;
}
void ForgetTarget(const Image* im)
{
    // (an image going away while it is the instance's attachment: the instance ends first)
    if (im && (g_target.color == im || g_target.depth == im))
    {
        EndRendering();
        g_target = Target{};
        g_pendingBegin = false;
        b.targetDirty = true;
    }
}
void APIENTRY DeleteTextures(GLsizei n, const GLuint* names)
{
    ResTime rt;
    for (GLsizei i = 0; i < n; ++i)
    {
        const GLuint name = names[i];
        auto t = g_textures.Take(name);
        if (!t) continue;
        if (!g_recording) BeginFrame();
        ForgetTarget(t->image.get());
        ReleaseImage(t->image);
        for (auto& unit : s.units) for (GLuint& bound : unit) if (bound == name) bound = 0;
        DetachEverywhere(name, false);
    }
}
GLboolean APIENTRY IsTexture(GLuint name) { return g_textures.Get(name) ? GL_TRUE : GL_FALSE; }
void SetParam(SamplerParams& p, int& slot, GLenum pname, GLint value)
{
    switch (pname)
    {
    case GL_TEXTURE_MIN_FILTER: p.minFilter = GLenum(value); break;
    case GL_TEXTURE_MAG_FILTER: p.magFilter = GLenum(value); break;
    case GL_TEXTURE_WRAP_S: p.wrapS = GLenum(value); break;
    case GL_TEXTURE_WRAP_T: p.wrapT = GLenum(value); break;
    case GL_TEXTURE_MAX_LEVEL: p.maxLevel = value; break;
    case kAnisotropyEnum: p.anisotropy = float(value); break;
    default: return;
    }
    slot = -1;
}
void APIENTRY TexParameteri(GLenum target, GLenum pname, GLint value)
{
    if (Texture* t = BoundTexture(target)) SetParam(t->params, t->samplerSlot, pname, value);
}
void APIENTRY TexParameterf(GLenum target, GLenum pname, GLfloat value)
{
    Texture* t = BoundTexture(target);
    if (!t) return;
    if (pname == kAnisotropyEnum) { t->params.anisotropy = value; t->samplerSlot = -1; }
    else SetParam(t->params, t->samplerSlot, pname, GLint(value));
}
void APIENTRY PixelStorei(GLenum pname, GLint value)
{
    switch (pname)
    {
    case GL_UNPACK_ALIGNMENT: s.unpackAlignment = value; break;
    case GL_UNPACK_ROW_LENGTH: s.unpackRowLength = value; break;
    case GL_PACK_ALIGNMENT: s.packAlignment = value; break;
    default: break;
    }
}
bool FormatComponents(GLenum format, unsigned* components)
{
    switch (format)
    {
    case GL_RGBA: case GL_BGRA: *components = 4; return true;
    case GL_RGB: case GL_BGR: *components = 3; return true;
    case GL_RG: *components = 2; return true;
    case GL_RED: *components = 1; return true;
    default: return false;
    }
}
// Pixels into the ring as tightly laid out RGBA8 rows and a queued copy.
void UploadPixelsPitch(Image& im, unsigned level, unsigned layer, int x, int y, unsigned width, unsigned height, GLenum format, GLenum type,
                       const void* pixels, size_t srcPitch);
void UploadPixels(Image& im, unsigned level, unsigned layer, int x, int y, unsigned width, unsigned height, GLenum format, GLenum type, const void* pixels)
{
    unsigned components = 4;
    FormatComponents(format, &components);
    const size_t srcPitch = Align(VkDeviceSize(s.unpackRowLength ? s.unpackRowLength : width) * components, VkDeviceSize(std::max(1, s.unpackAlignment)));
    UploadPixelsPitch(im, level, layer, x, y, width, height, format, type, pixels, srcPitch);
}
void UploadPixelsPitch(Image& im, unsigned level, unsigned layer, int x, int y, unsigned width, unsigned height, GLenum format, GLenum type,
                       const void* pixels, size_t srcPitch)
{
    ResTime rt;
    if (g_resLogOn && pixels) ++g_resFrame.uploads, g_resFrame.bytes += uint64_t(width) * height * 4;
    if (!pixels || !width || !height) return;
    if (im.format != VK_FORMAT_R8G8B8A8_UNORM || type != GL_UNSIGNED_BYTE) return;
    unsigned components = 0;
    if (!FormatComponents(format, &components)) return;
    const uint32_t mipW = std::max(1u, im.width >> level), mipH = std::max(1u, im.height >> level);
    if (x < 0 || y < 0 || uint64_t(x) + width > mipW || uint64_t(y) + height > mipH || level >= im.levels || layer >= im.layers)
    { SetError(GL_INVALID_VALUE); return; }
    const size_t pitch = size_t(width) * 4;
    const Upload u = Ring(VkDeviceSize(pitch) * height, 16);
    const auto* src = static_cast<const uint8_t*>(pixels);
    if (format == GL_RGBA && srcPitch == pitch) std::memcpy(u.cpu, src, pitch * height);
    else
    {
        std::vector<uint8_t> row(pitch);
        for (unsigned y0 = 0; y0 < height; ++y0)
        {
            const uint8_t* p = src + size_t(y0) * srcPitch;
            if (format == GL_RGBA) { std::memcpy(u.cpu + size_t(y0) * pitch, p, pitch); continue; }
            for (unsigned col = 0; col < width; ++col)
            {
                const uint8_t* q = p + size_t(col) * components;
                uint8_t* o = row.data() + size_t(col) * 4;
                switch (format)
                {
                case GL_BGRA: o[0] = q[2]; o[1] = q[1]; o[2] = q[0]; o[3] = q[3]; break;
                case GL_RGB: o[0] = q[0]; o[1] = q[1]; o[2] = q[2]; o[3] = 255; break;
                case GL_BGR: o[0] = q[2]; o[1] = q[1]; o[2] = q[0]; o[3] = 255; break;
                case GL_RG: o[0] = q[0]; o[1] = q[1]; o[2] = 0; o[3] = 255; break;
                default: o[0] = q[0]; o[1] = 0; o[2] = 0; o[3] = 255; break;
                }
            }
            std::memcpy(u.cpu + size_t(y0) * pitch, row.data(), pitch);
        }
    }
    XferOp op;
    op.kind = XferOp::BufferToImage;
    op.srcBuffer = u.buffer;
    op.srcOffset = u.offset;
    op.image = &im;
    op.level = level;
    op.layer = layer;
    op.x = uint32_t(x);
    op.y = uint32_t(y);
    op.width = width;
    op.height = height;
    op.rowLength = width;
    QueueImageOp(op);
}
void Replace(Texture& t, std::unique_ptr<Image> im)
{
    ForgetTarget(t.image.get());
    ReleaseImage(t.image);
    t.image = std::move(im);
    b.targetDirty = true;
}
unsigned FullLevels(uint32_t w, uint32_t h)
{
    unsigned levels = 1;
    for (uint32_t n = std::max(w, h); n > 1; n >>= 1) ++levels;
    return levels;
}
// (the uncompressed textures are RGBA8, whatever their internal format)
void APIENTRY TexImage2D(GLenum target, GLint level, GLint, GLsizei width, GLsizei height, GLint, GLenum format, GLenum type, const void* pixels)
{
    ResTime rt;
    if (g_resLogOn) ++g_resFrame.textures;
    Texture* t = BoundTexture(target);
    if (!t || target != GL_TEXTURE_2D || level < 0 || width <= 0 || height <= 0) { SetError(GL_INVALID_OPERATION); return; }
    if (!g_recording) BeginFrame();
    if (level == 0)
    {
        // (a texture redefined at its size keeps its image: the game re-uploads render textures that way)
        if (!t->image || t->image->width != uint32_t(width) || t->image->height != uint32_t(height) || t->image->array || t->image->levels != 1)
            Replace(*t, MakeImage(uint32_t(width), uint32_t(height), 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false, true));
    }
    else if (!t->image || unsigned(level) >= t->image->levels) return;
    UploadPixels(*t->image, unsigned(level), 0, 0, 0, unsigned(width), unsigned(height), format, type, pixels);
}
void APIENTRY TexStorage2D(GLenum target, GLsizei levels, GLenum, GLsizei width, GLsizei height)
{
    ResTime rt;
    if (g_resLogOn) ++g_resFrame.textures;
    Texture* t = BoundTexture(target);
    if (!t || levels <= 0 || width <= 0 || height <= 0) { SetError(GL_INVALID_OPERATION); return; }
    if (!g_recording) BeginFrame();
    const unsigned l = std::min<unsigned>(unsigned(levels), FullLevels(uint32_t(width), uint32_t(height)));
    Replace(*t, MakeImage(uint32_t(width), uint32_t(height), 1, l, 1, VK_FORMAT_R8G8B8A8_UNORM, false, true));
    t->image->immutable = true;
}
void APIENTRY TexStorage3D(GLenum target, GLsizei levels, GLenum, GLsizei width, GLsizei height, GLsizei depth)
{
    ResTime rt;
    if (g_resLogOn) ++g_resFrame.textures;
    Texture* t = BoundTexture(target);
    if (!t || target != GL_TEXTURE_2D_ARRAY || levels <= 0 || width <= 0 || height <= 0 || depth <= 0) { SetError(GL_INVALID_OPERATION); return; }
    if (!g_recording) BeginFrame();
    const unsigned l = std::min<unsigned>(unsigned(levels), FullLevels(uint32_t(width), uint32_t(height)));
    Replace(*t, MakeImage(uint32_t(width), uint32_t(height), uint32_t(depth), l, 1, VK_FORMAT_R8G8B8A8_UNORM, true, true));
    t->image->immutable = true;
}
void APIENTRY TexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels)
{
    ResTime rt;
    Texture* t = BoundTexture(target);
    if (!t || !t->image || level < 0 || width < 0 || height < 0) { SetError(GL_INVALID_OPERATION); return; }
    UploadPixels(*t->image, unsigned(level), 0, x, y, unsigned(width), unsigned(height), format, type, pixels);
}
void APIENTRY TexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels)
{
    ResTime rt;
    Texture* t = BoundTexture(target);
    if (!t || !t->image || level < 0 || width < 0 || height < 0 || depth < 0 || z < 0) { SetError(GL_INVALID_OPERATION); return; }
    unsigned components = 4;
    FormatComponents(format, &components);
    const size_t layerBytes = size_t(Align(VkDeviceSize(s.unpackRowLength ? s.unpackRowLength : width) * components, VkDeviceSize(std::max(1, s.unpackAlignment)))) * size_t(height);
    for (GLsizei i = 0; i < depth; ++i)
        UploadPixels(*t->image, unsigned(level), unsigned(z + i), x, y, unsigned(width), unsigned(height), format, type,
                     pixels ? static_cast<const uint8_t*>(pixels) + layerBytes * size_t(i) : nullptr);
}
void GenerateMips(Texture* t);
void APIENTRY GenerateMipmap(GLenum target)
{
    ResTime rt;
    GenerateMips(BoundTexture(target));
}
void GenerateMips(Texture* t)
{
    if (!t || !t->image || t->image->samples > 1) return;
    if (!g_recording) BeginFrame();
    if (t->image->levels == 1)
    {
        // (a single level: GL makes the whole chain; the image is reallocated with it)
        Image& old = *t->image;
        const unsigned levels = FullLevels(old.width, old.height);
        if (levels == 1) return;
        auto chain = MakeImage(old.width, old.height, old.layers, levels, 1, old.format, old.array, true);
        XferOp op;
        op.kind = XferOp::CopyImage;
        op.image = chain.get();
        op.source = &old;
        QueueImageOp(op);
        Replace(*t, std::move(chain));
    }
    XferOp op;
    op.kind = XferOp::MipChain;
    op.image = t->image.get();
    QueueImageOp(op);
}
void APIENTRY GetTexLevelParameteriv(GLenum target, GLint level, GLenum pname, GLint* value)
{
    Texture* t = BoundTexture(target);
    if (!value) return;
    *value = 0;
    if (!t || !t->image || level < 0 || unsigned(level) >= t->image->levels) return;
    if (pname == GL_TEXTURE_WIDTH) *value = GLint(std::max(1u, t->image->width >> level));
    else if (pname == GL_TEXTURE_HEIGHT) *value = GLint(std::max(1u, t->image->height >> level));
    else if (pname == GL_TEXTURE_DEPTH) *value = GLint(t->image->layers);
    else if (pname == GL_TEXTURE_INTERNAL_FORMAT) *value = GL_RGBA8;
}
void APIENTRY GenSamplers(GLsizei n, GLuint* names) { for (GLsizei i = 0; i < n; ++i) names[i] = g_samplers.Gen(); }
void APIENTRY BindSampler(GLuint unit, GLuint name)
{
    if (unit >= kMaxUnits) { SetError(GL_INVALID_VALUE); return; }
    if (name) g_samplers.Ensure(name);
    s.samplerUnits[unit] = name;
}
void APIENTRY DeleteSamplers(GLsizei n, const GLuint* names)
{
    for (GLsizei i = 0; i < n; ++i)
    {
        if (!g_samplers.Take(names[i])) continue;
        for (GLuint& u : s.samplerUnits) if (u == names[i]) u = 0;
    }
}
void APIENTRY SamplerParameteri(GLuint name, GLenum pname, GLint value)
{
    if (Sampler* sm = g_samplers.Ensure(name)) SetParam(sm->params, sm->samplerSlot, pname, value);
}
void APIENTRY SamplerParameterf(GLuint name, GLenum pname, GLfloat value)
{
    Sampler* sm = g_samplers.Ensure(name);
    if (!sm) return;
    if (pname == kAnisotropyEnum) { sm->params.anisotropy = value; sm->samplerSlot = -1; }
    else SetParam(sm->params, sm->samplerSlot, pname, GLint(value));
}
// ARB_bindless_texture: a handle is the image's slot (low half, never 0) and the sampler's (high half), which the
// BINDLESS programs read from the global arrays (cmake/vulkan_shaders.py)
GLuint64 APIENTRY GetTextureSamplerHandleARB(GLuint texture, GLuint sampler)
{
    Texture* t = g_textures.Get(texture);
    Sampler* sm = g_samplers.Get(sampler);
    if (!t || !t->image || !t->image->sampled || t->image->array || t->image->slot <= 0) return 0;
    if (sm && sm->samplerSlot < 0) sm->samplerSlot = SamplerSlot(sm->params);
    if (!sm && t->samplerSlot < 0) t->samplerSlot = SamplerSlot(t->params);
    t->image->resident = true;
    return GLuint64(uint32_t(t->image->slot)) | (GLuint64(uint32_t(sm ? sm->samplerSlot : t->samplerSlot)) << 32);
}
void APIENTRY MakeTextureHandleResidentARB(GLuint64) {}
void APIENTRY MakeTextureHandleNonResidentARB(GLuint64) {}

// ------------------------------------------------------------------------------------------------ framebuffers
void APIENTRY GenFramebuffers(GLsizei n, GLuint* names) { for (GLsizei i = 0; i < n; ++i) names[i] = g_framebuffers.Gen(); }
void APIENTRY BindFramebuffer(GLenum target, GLuint name)
{
    if (name) g_framebuffers.Ensure(name);
    if ((target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER) && s.drawFramebuffer != name)
    {
        s.drawFramebuffer = name;
        b.targetDirty = true;
    }
    if (target == GL_FRAMEBUFFER || target == GL_READ_FRAMEBUFFER) s.readFramebuffer = name;
}
void APIENTRY DeleteFramebuffers(GLsizei n, const GLuint* names)
{
    for (GLsizei i = 0; i < n; ++i)
    {
        if (!g_framebuffers.Take(names[i])) continue;
        if (s.drawFramebuffer == names[i]) { s.drawFramebuffer = 0; b.targetDirty = true; }
        if (s.readFramebuffer == names[i]) s.readFramebuffer = 0;
        b.targetDirty = true;
    }
}
Framebuffer* TargetFramebuffer(GLenum target)
{
    const GLuint name = target == GL_READ_FRAMEBUFFER ? s.readFramebuffer : s.drawFramebuffer;
    return g_framebuffers.Get(name);
}
void Attach(GLenum target, GLenum attachment, GLuint object, bool renderbuffer, unsigned level, unsigned layer)
{
    Framebuffer* f = TargetFramebuffer(target);
    if (!f) { SetError(GL_INVALID_OPERATION); return; }
    const Attachment a{object, renderbuffer, uint16_t(layer), uint8_t(level), 0};
    if (attachment == GL_COLOR_ATTACHMENT0) f->color = a;
    else if (attachment == GL_DEPTH_STENCIL_ATTACHMENT || attachment == GL_DEPTH_ATTACHMENT) f->depth = a;
    else if (attachment == GL_STENCIL_ATTACHMENT) { if (!f->depth.object) f->depth = a; }
    b.targetDirty = true;
}
void APIENTRY FramebufferTexture2D(GLenum target, GLenum attachment, GLenum, GLuint texture, GLint level)
{ Attach(target, attachment, texture, false, unsigned(std::max(level, 0)), 0); }
void APIENTRY FramebufferTextureLayer(GLenum target, GLenum attachment, GLuint texture, GLint level, GLint layer)
{ Attach(target, attachment, texture, false, unsigned(std::max(level, 0)), unsigned(std::max(layer, 0))); }
void APIENTRY FramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum, GLuint renderbuffer)
{ Attach(target, attachment, renderbuffer, true, 0, 0); }
void APIENTRY GenRenderbuffers(GLsizei n, GLuint* names) { for (GLsizei i = 0; i < n; ++i) names[i] = g_renderbuffers.Gen(); }
void APIENTRY BindRenderbuffer(GLenum, GLuint name) { if (name) g_renderbuffers.Ensure(name); s.renderbuffer = name; }
void APIENTRY DeleteRenderbuffers(GLsizei n, const GLuint* names)
{
    for (GLsizei i = 0; i < n; ++i)
    {
        auto rb = g_renderbuffers.Take(names[i]);
        if (!rb) continue;
        if (!g_recording) BeginFrame();
        ForgetTarget(rb->image.get());
        ReleaseImage(rb->image);
        if (s.renderbuffer == names[i]) s.renderbuffer = 0;
        DetachEverywhere(names[i], true);
    }
}
void RenderbufferStorageOn(Renderbuffer* rb, GLsizei samples, GLenum internal, GLsizei width, GLsizei height);
void APIENTRY RenderbufferStorageMultisample(GLenum, GLsizei samples, GLenum internal, GLsizei width, GLsizei height)
{ RenderbufferStorageOn(g_renderbuffers.Get(s.renderbuffer), samples, internal, width, height); }
// (the GL call's body on a renderbuffer by name: also the createRenderbuffer packet's)
void RenderbufferStorageOn(Renderbuffer* rb, GLsizei samples, GLenum internal, GLsizei width, GLsizei height)
{
    if (!rb || width <= 0 || height <= 0) { SetError(GL_INVALID_OPERATION); return; }
    if (!g_recording) BeginFrame();
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    switch (internal)
    {
    case GL_DEPTH32F_STENCIL8: case GL_DEPTH_COMPONENT32F: case GL_STENCIL_INDEX8: format = g_feat.depthStencil; break;
    case GL_DEPTH24_STENCIL8: case GL_DEPTH_COMPONENT24: case GL_DEPTH_COMPONENT: format = g_feat.depth24; break;
    default: break;
    }
    unsigned n = samples <= 1 ? 1 : samples <= 2 ? 2 : samples <= 4 ? 4 : samples <= 8 ? 8 : 16;
    while (n > 1 && !(g_feat.samples & n)) n >>= 1;
    ForgetTarget(rb->image.get());
    ReleaseImage(rb->image);
    // (msrtss: a multisampled renderbuffer is a single-sampled image drawn with n samples, resolved on the tile as it is
    // stored: its resolve blit becomes a copy)
    if (n > 1 && g_feat.msrtss) g_nextRenderSamples = n, n = 1;
    const uint32_t layers = g_nextRenderbufferLayers;
    g_nextRenderbufferLayers = 1;
    rb->image = MakeImage(uint32_t(width), uint32_t(height), layers, 1, n, format, false, false);
    b.targetDirty = true;
}
void APIENTRY RenderbufferStorage(GLenum target, GLenum internal, GLsizei width, GLsizei height)
{ RenderbufferStorageMultisample(target, 1, internal, width, height); }
Image* AttachmentImage(const Attachment& a)
{
    if (!a.object) return nullptr;
    if (a.renderbuffer) { Renderbuffer* rb = g_renderbuffers.Get(a.object); return rb ? rb->image.get() : nullptr; }
    Texture* t = g_textures.Get(a.object);
    return t ? t->image.get() : nullptr;
}
GLenum APIENTRY CheckFramebufferStatus(GLenum target)
{
    const GLuint name = target == GL_READ_FRAMEBUFFER ? s.readFramebuffer : s.drawFramebuffer;
    if (!name) return GL_FRAMEBUFFER_COMPLETE;
    Framebuffer* f = g_framebuffers.Get(name);
    Image* color = AttachmentImage(f->color);
    Image* depth = AttachmentImage(f->depth);
    if (!color && !depth) return GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT;
    if ((f->color.object && !color) || (f->depth.object && !depth)) return GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT;
    if (color && depth && color->samples != depth->samples) return GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE;
    return GL_FRAMEBUFFER_COMPLETE;
}
void APIENTRY DrawBuffer(GLenum mode)
{
    if (Framebuffer* f = g_framebuffers.Get(s.drawFramebuffer)) { f->drawColor = mode != GL_NONE; b.targetDirty = true; }
}
void APIENTRY DrawBuffers(GLsizei n, const GLenum* modes) { DrawBuffer(n > 0 ? modes[0] : GL_NONE); }
void APIENTRY ReadBuffer(GLenum) {}
// The draw framebuffer's attachments not kept: while their rendering instance is still open (recorded, not yet replayed:
// the threaded stream), its store operations become DONT_CARE, so a tiler never writes them out; a draw or clear into
// the instance after this stores them again. (Without the worker the instance's stores are already recorded: the
// transient hint, SetRenderbufferTransient, covers VR's eyes.)
void ApplyTarget();
void RestoreInvalidatedStores()
{
    if (g_invalidatedColor && g_recColor) g_recColor->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (g_invalidatedDepth && g_recDepth) g_recDepth->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (g_invalidatedDepth && g_recStencil) g_recStencil->storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    if (g_recClassic && (g_invalidatedColor || g_invalidatedDepth))
    {
        if (g_invalidatedColor) g_instanceClassicKey.ops[1] = uint8_t(VK_ATTACHMENT_STORE_OP_STORE);
        if (g_invalidatedDepth) g_instanceClassicKey.ops[3] = g_instanceClassicKey.ops[5] = uint8_t(VK_ATTACHMENT_STORE_OP_STORE);
        g_recClassic->renderPass = ClassicRenderPass(g_instanceClassicKey);
    }
    g_invalidatedColor = g_invalidatedDepth = false;
}
void APIENTRY InvalidateFramebuffer(GLenum target, GLsizei n, const GLenum* attachments)
{
    if (!g_recording || target == GL_READ_FRAMEBUFFER || !attachments) return;
    ApplyTarget();   // (the bound framebuffer's: the open instance if it is the same target)
    static const bool s_fOff = std::getenv("SLY2N_VKINVALIDATE") && std::atoi(std::getenv("SLY2N_VKINVALIDATE")) == 0;
    if (s_fOff) return;
    if (!g_inRendering)
    {
        // (the target's last instance already ended, unused since: its depth-stencil stores dropped; SLY2N_VKENDEDINV=0: not)
        static const bool s_fEndedOff = std::getenv("SLY2N_VKENDEDINV") && std::atoi(std::getenv("SLY2N_VKENDEDINV")) == 0;
        bool ds[2] = {false, false};
        for (GLsizei i = 0; i < n; ++i)
        {
            const GLenum a = attachments[i];
            ds[0] |= a == GL_DEPTH_ATTACHMENT || a == GL_DEPTH_STENCIL_ATTACHMENT || a == GL_DEPTH;
            ds[1] |= a == GL_STENCIL_ATTACHMENT || a == GL_DEPTH_STENCIL_ATTACHMENT || a == GL_STENCIL;
        }
        if (s_fEndedOff || !ds[0] || !ds[1] || !g_target.depth || g_target.depth != g_endedDepth) return;
        if (g_ended.classic)
        {
            ClassicKey ck = g_ended.key;
            if (ck.ops[3] != uint8_t(VK_ATTACHMENT_STORE_OP_DONT_CARE) || ck.ops[5] != uint8_t(VK_ATTACHMENT_STORE_OP_DONT_CARE))
            {
                ck.ops[3] = ck.ops[5] = uint8_t(VK_ATTACHMENT_STORE_OP_DONT_CARE);
                g_ended.classic->renderPass = ClassicRenderPass(ck);
                ++g_storesDropped;
            }
        }
        else if (g_ended.depth && g_ended.stencil && g_ended.depth->storeOp != VK_ATTACHMENT_STORE_OP_DONT_CARE)
        {
            g_ended.depth->storeOp = g_ended.stencil->storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            ++g_storesDropped;
        }
        g_endedDepth = nullptr;
        return;
    }
    bool color = false, depth = false, stencil = false;
    for (GLsizei i = 0; i < n; ++i)
    {
        const GLenum a = attachments[i];
        color |= a == GL_COLOR_ATTACHMENT0 || a == GL_COLOR;
        depth |= a == GL_DEPTH_ATTACHMENT || a == GL_DEPTH_STENCIL_ATTACHMENT || a == GL_DEPTH;
        stencil |= a == GL_STENCIL_ATTACHMENT || a == GL_DEPTH_STENCIL_ATTACHMENT || a == GL_STENCIL;
    }
    // (depth and stencil share one image: both or neither)
    if (depth && stencil && g_recDepth && g_recStencil)
    {
        g_recDepth->storeOp = g_recStencil->storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        g_invalidatedDepth = true;
        ++g_storesDropped;
    }
    if (color && g_recColor && g_target.color && !g_target.color->external)
    {
        g_recColor->storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        g_invalidatedColor = true;
    }
    // (the classic in-place-read instance: the compatible render pass object with those stores DONT_CARE; SLY2N_VKCLASSICINV=0: kept)
    static const bool s_fClassicOff = std::getenv("SLY2N_VKCLASSICINV") && std::atoi(std::getenv("SLY2N_VKCLASSICINV")) == 0;
    if (g_recClassic && g_instanceClassic && !s_fClassicOff)
    {
        ClassicKey& ck = g_instanceClassicKey;
        bool changed = false;
        if (depth && stencil && ck.depth != VK_FORMAT_UNDEFINED && ck.ops[3] == uint8_t(VK_ATTACHMENT_STORE_OP_STORE))
        {
            ck.ops[3] = ck.ops[5] = uint8_t(VK_ATTACHMENT_STORE_OP_DONT_CARE);
            g_invalidatedDepth = changed = true;
        }
        if (color && g_target.color && !g_target.color->external && ck.ops[1] == uint8_t(VK_ATTACHMENT_STORE_OP_STORE))
        {
            ck.ops[1] = uint8_t(VK_ATTACHMENT_STORE_OP_DONT_CARE);
            g_invalidatedColor = changed = true;
        }
        if (changed) g_recClassic->renderPass = ClassicRenderPass(ck), ++g_storesDropped;
    }
}

// ------------------------------------------------------------------------------------------------ state
bool* Capability(GLenum cap)
{
    switch (cap)
    {
    case GL_DEPTH_TEST: return &s.depthTest;
    case GL_BLEND: return &s.blend;
    case GL_CULL_FACE: return &s.cull;
    case GL_STENCIL_TEST: return &s.stencilTest;
    case GL_SCISSOR_TEST: return &s.scissorTest;
    case 0x8890: return g_feat.depthBounds ? &s.depthBoundsTest : nullptr;   // GL_DEPTH_BOUNDS_TEST_EXT
    default: return nullptr;
    }
}
void SetCapability(GLenum cap, bool enable)
{
    bool* p = Capability(cap);
    if (!p || *p == enable) return;
    *p = enable;
    g_fixedPacket = 0;
    switch (cap)
    {
    case GL_DEPTH_TEST: case GL_STENCIL_TEST: case 0x8890: b.depthDirty = true; break;
    case GL_BLEND: BlendChanged(); break;
    case GL_CULL_FACE: b.rasterDirty = true; break;
    case GL_SCISSOR_TEST: b.viewportDirty = true; break;
    default: break;
    }
}
void APIENTRY Enable(GLenum cap) { SetCapability(cap, true); }
void APIENTRY Disable(GLenum cap) { SetCapability(cap, false); }
GLboolean APIENTRY IsEnabled(GLenum cap) { const bool* p = Capability(cap); return p && *p ? GL_TRUE : GL_FALSE; }
void APIENTRY DepthBoundsEXT(GLclampd zmin, GLclampd zmax)
{
    const float lo = float(std::clamp(zmin, 0.0, 1.0)), hi = float(std::clamp(zmax, 0.0, 1.0));
    if (s.depthBoundsMin == lo && s.depthBoundsMax == hi) return;
    s.depthBoundsMin = lo, s.depthBoundsMax = hi;
    if (s.depthBoundsTest) b.depthDirty = true;
    g_fixedPacket = 0;
}
void APIENTRY DepthMask(GLboolean flag) { g_fixedPacket = 0; if (s.depthWrite != (flag != GL_FALSE)) { s.depthWrite = flag != GL_FALSE; b.depthDirty = true; } }
void APIENTRY DepthFunc(GLenum func) { g_fixedPacket = 0; if (s.depthFunc != func) { s.depthFunc = func; b.depthDirty = true; } }
void APIENTRY DepthRange(GLdouble n, GLdouble f) { s.depthNear = float(std::clamp(n, 0.0, 1.0)); s.depthFar = float(std::clamp(f, 0.0, 1.0)); b.viewportDirty = true; }
void APIENTRY DepthRangef(GLfloat n, GLfloat f) { DepthRange(n, f); }
void APIENTRY FrontFace(GLenum mode) { g_fixedPacket = 0; if (s.frontFace != mode) { s.frontFace = mode; b.rasterDirty = true; } }
void APIENTRY CullFace(GLenum mode) { g_fixedPacket = 0; if (s.cullFace != mode) { s.cullFace = mode; b.rasterDirty = true; } }
void APIENTRY PolygonMode(GLenum, GLenum mode) { if (s.polygonMode != mode) { s.polygonMode = mode; b.pipelineDirty = true; } }
void APIENTRY BlendFuncSeparate(GLenum src, GLenum dst, GLenum srcA, GLenum dstA)
{
    g_fixedPacket = 0;
    if (s.blendSrc == src && s.blendDst == dst && s.blendSrcAlpha == srcA && s.blendDstAlpha == dstA) return;
    s.blendSrc = src; s.blendDst = dst; s.blendSrcAlpha = srcA; s.blendDstAlpha = dstA;
    if (s.blend) BlendChanged();
}
void APIENTRY BlendFunc(GLenum src, GLenum dst) { BlendFuncSeparate(src, dst, src, dst); }
void APIENTRY BlendEquationSeparate(GLenum rgb, GLenum alpha)
{
    g_fixedPacket = 0;
    if (s.blendEq == rgb && s.blendEqAlpha == alpha) return;
    s.blendEq = rgb; s.blendEqAlpha = alpha;
    if (s.blend) BlendChanged();
}
void APIENTRY BlendEquation(GLenum mode) { BlendEquationSeparate(mode, mode); }
void APIENTRY BlendColor(GLfloat r, GLfloat g, GLfloat bl, GLfloat a)
{
    g_fixedPacket = 0;
    const std::array<float, 4> c{r, g, bl, a};
    if (s.blendColor != c) { s.blendColor = c; b.blendConstDirty = true; }
}
void APIENTRY ColorMask(GLboolean r, GLboolean g, GLboolean bl, GLboolean a)
{
    g_fixedPacket = 0;
    const unsigned mask = (r ? 1u : 0u) | (g ? 2u : 0u) | (bl ? 4u : 0u) | (a ? 8u : 0u);
    if (s.colorMask != mask) { s.colorMask = mask; BlendChanged(); }
}
void APIENTRY StencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask)
{
    g_fixedPacket = 0;
    for (State::Face* f : {&s.front, &s.back})
    {
        if ((f == &s.front && face == GL_BACK) || (f == &s.back && face == GL_FRONT)) continue;
        f->func = func; f->ref = ref; f->mask = mask;
    }
    b.depthDirty = b.stencilDirty = true;
}
void APIENTRY StencilFunc(GLenum func, GLint ref, GLuint mask) { StencilFuncSeparate(GL_FRONT_AND_BACK, func, ref, mask); }
void APIENTRY StencilOpSeparate(GLenum face, GLenum fail, GLenum zfail, GLenum zpass)
{
    g_fixedPacket = 0;
    for (State::Face* f : {&s.front, &s.back})
    {
        if ((f == &s.front && face == GL_BACK) || (f == &s.back && face == GL_FRONT)) continue;
        f->fail = fail; f->zfail = zfail; f->zpass = zpass;
    }
    b.depthDirty = true;
}
void APIENTRY StencilOp(GLenum fail, GLenum zfail, GLenum zpass) { StencilOpSeparate(GL_FRONT_AND_BACK, fail, zfail, zpass); }
void APIENTRY StencilMaskSeparate(GLenum face, GLuint mask)
{
    g_fixedPacket = 0;
    if (face != GL_BACK) s.front.writeMask = mask;
    if (face != GL_FRONT) s.back.writeMask = mask;
    b.stencilDirty = true;
}
void APIENTRY StencilMask(GLuint mask) { StencilMaskSeparate(GL_FRONT_AND_BACK, mask); }
void APIENTRY Viewport(GLint x, GLint y, GLsizei w, GLsizei h)
{
    if (s.viewport[0] == x && s.viewport[1] == y && s.viewport[2] == w && s.viewport[3] == h) return;
    s.viewport[0] = x; s.viewport[1] = y; s.viewport[2] = w; s.viewport[3] = h;
    b.viewportDirty = true;
}
void APIENTRY Scissor(GLint x, GLint y, GLsizei w, GLsizei h)
{
    if (s.scissor[0] == x && s.scissor[1] == y && s.scissor[2] == w && s.scissor[3] == h) return;
    s.scissor[0] = x; s.scissor[1] = y; s.scissor[2] = w; s.scissor[3] = h;
    if (s.scissorTest) b.viewportDirty = true;
}
void APIENTRY LineWidth(GLfloat width) { if (s.lineWidth != width) { s.lineWidth = width; b.lineDirty = true; } }
void APIENTRY PointSize(GLfloat) {}
void APIENTRY ClearColor(GLfloat r, GLfloat g, GLfloat bl, GLfloat a) { s.clearColor = {r, g, bl, a}; }
void APIENTRY ClearDepth(GLdouble d) { s.clearDepth = float(std::clamp(d, 0.0, 1.0)); }
void APIENTRY ClearDepthf(GLfloat d) { ClearDepth(d); }
void APIENTRY ClearStencil(GLint v) { s.clearStencil = v; }
// (Vulkan's depth range is [0, 1] and the framebuffers' rows are GL's: the game's glClipControl(GL_LOWER_LEFT,
// GL_ZERO_TO_ONE) is how this layer works anyway)
void APIENTRY ClipControl(GLenum, GLenum) {}
#ifdef MemoryBarrier
#undef MemoryBarrier   // (winnt.h's macro on ARM64 Windows)
#endif
void APIENTRY MemoryBarrier(GLbitfield) {}
void APIENTRY Flush() { if (g_recording) Submit(); }

// ------------------------------------------------------------------------------------------------ state application
VkCompareOp Compare(GLenum f)
{
    switch (f)
    {
    case GL_NEVER: return VK_COMPARE_OP_NEVER; case GL_LESS: return VK_COMPARE_OP_LESS; case GL_EQUAL: return VK_COMPARE_OP_EQUAL;
    case GL_LEQUAL: return VK_COMPARE_OP_LESS_OR_EQUAL; case GL_GREATER: return VK_COMPARE_OP_GREATER;
    case GL_NOTEQUAL: return VK_COMPARE_OP_NOT_EQUAL; case GL_GEQUAL: return VK_COMPARE_OP_GREATER_OR_EQUAL;
    default: return VK_COMPARE_OP_ALWAYS;
    }
}
VkStencilOp StencilOpOf(GLenum op)
{
    switch (op)
    {
    case GL_ZERO: return VK_STENCIL_OP_ZERO; case GL_REPLACE: return VK_STENCIL_OP_REPLACE;
    case GL_INCR: return VK_STENCIL_OP_INCREMENT_AND_CLAMP; case GL_DECR: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case GL_INVERT: return VK_STENCIL_OP_INVERT; case GL_INCR_WRAP: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case GL_DECR_WRAP: return VK_STENCIL_OP_DECREMENT_AND_WRAP; default: return VK_STENCIL_OP_KEEP;
    }
}
VkBlendFactor Factor(GLenum f)
{
    switch (f)
    {
    case GL_ZERO: return VK_BLEND_FACTOR_ZERO; case GL_ONE: return VK_BLEND_FACTOR_ONE;
    case GL_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR; case GL_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case GL_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA; case GL_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case GL_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA; case GL_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case GL_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR; case GL_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case GL_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case GL_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR; case GL_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case GL_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case GL_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case GL_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA; case GL_ONE_MINUS_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case GL_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR; case GL_ONE_MINUS_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    default: return VK_BLEND_FACTOR_ONE;
    }
}
VkBlendOp BlendOpOf(GLenum op)
{
    switch (op)
    {
    case GL_FUNC_SUBTRACT: return VK_BLEND_OP_SUBTRACT; case GL_FUNC_REVERSE_SUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case GL_MIN: return VK_BLEND_OP_MIN; case GL_MAX: return VK_BLEND_OP_MAX; default: return VK_BLEND_OP_ADD;
    }
}
uint32_t FactorCode(GLenum f) { return uint32_t(Factor(f)); }
uint32_t BlendOpCode(GLenum op) { return uint32_t(BlendOpOf(op)); }
VkPrimitiveTopology Topology(GLenum mode)
{
    switch (mode)
    {
    case GL_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST; case GL_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case GL_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; case GL_TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case GL_TRIANGLE_FAN: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

// The window (framebuffer 0): an offscreen picture in GL's convention, copied flipped into the swapchain at the present.
std::unique_ptr<Image> g_windowImage;
uint32_t g_outputWidth = 0, g_outputHeight = 0;

void ApplyTarget()
{
    if (!b.targetDirty) return;
    Target t;
    uint32_t w = 1, h = 1, samples = 1;
    const bool msrtssWas = b.msrtss, fdmWas = b.fdm, multiviewWas = b.multiview;
    b.msrtss = false;
    b.fdm = false;
    if (!s.drawFramebuffer)
    {
        t.color = g_windowImage.get();
        w = g_outputWidth;
        h = g_outputHeight;
    }
    else
    {
        Framebuffer* f = g_framebuffers.Get(s.drawFramebuffer);
        Image* ci = f ? AttachmentImage(f->color) : nullptr;
        Image* di = f ? AttachmentImage(f->depth) : nullptr;
        const Image* dims = ci ? ci : di;
        if (dims)
        {
            const unsigned level = ci ? f->color.level : f->depth.level;
            w = std::max(1u, dims->width >> level);
            h = std::max(1u, dims->height >> level);
            samples = dims->samples;
            if (dims->renderSamples > 1) samples = dims->renderSamples;
        }
        b.msrtss = dims && dims->renderSamples > 1;
        b.fdm = ci && ci->densityMap && f->drawColor;
        if (ci && f->drawColor) { t.color = ci; t.colorLevel = f->color.level; t.colorLayer = f->color.layer; }
        if (di) { t.depth = di; t.depthLevel = f->depth.level; t.depthLayer = f->depth.layer; }
        // (multiview: every attachment drawn has the two views)
        const uint8_t views = t.color ? f->color.views : t.depth ? f->depth.views : 0;
        if (views > 1 && g_feat.multiview && (!t.color || f->color.views == views) && (!t.depth || f->depth.views == views)) t.views = views;
    }
    if (!(t == g_target) || (!g_pendingBegin && !g_inRendering))
    {
        EndRendering();
        g_target = t;
        g_pendingBegin = t.color || t.depth;
        g_pendingClear = PendingClear{};
    }
    const uint8_t colorFormat = t.color ? 1 : 0, depthFormat = t.depth ? 1 : 0;
    b.multiview = t.views > 1;
    if (colorFormat != b.colorFormat || depthFormat != b.depthFormat || samples != b.samples || msrtssWas != b.msrtss || fdmWas != b.fdm ||
        multiviewWas != b.multiview)
        b.pipelineDirty = true;
    b.hasColor = t.color != nullptr;
    b.hasDepth = t.depth != nullptr;
    b.colorFormat = colorFormat;
    b.depthFormat = depthFormat;
    b.width = w;
    b.height = h;
    b.samples = samples;
    b.targetDirty = false;
    b.viewportDirty = b.depthDirty = b.stencilDirty = b.blendDirty = true;
}

VkRect2D ScissorRect()
{
    const int W = int(b.width), H = int(b.height);
    int x0 = 0, y0 = 0, x1 = W, y1 = H;
    if (s.scissorTest)
    {
        x0 = std::clamp(s.scissor[0], 0, W);
        x1 = int(std::clamp<int64_t>(int64_t(s.scissor[0]) + s.scissor[2], x0, W));
        y0 = std::clamp(s.scissor[1], 0, H);
        y1 = int(std::clamp<int64_t>(int64_t(s.scissor[1]) + s.scissor[3], y0, H));
    }
    return {{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
}
void ApplyDynamic()
{
    VkCommandBuffer cmd = g_main;
    if (b.viewportDirty)
    {
        // (GL's rectangles are the memory's rows: see gl_vulkan.h "Coordinates")
        const VkViewport vp{float(s.viewport[0]), float(s.viewport[1]), float(std::max(1, s.viewport[2])), float(std::max(1, s.viewport[3])),
                            s.depthNear, s.depthFar};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        const VkRect2D sc = ScissorRect();
        vkCmdSetScissor(cmd, 0, 1, &sc);
        b.viewportDirty = false;
    }
    if (b.rasterDirty)
    {
        vkCmdSetCullMode(cmd, !s.cull ? VK_CULL_MODE_NONE : s.cullFace == GL_FRONT ? VK_CULL_MODE_FRONT_BIT
                            : s.cullFace == GL_FRONT_AND_BACK ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_BACK_BIT);
        // (Vulkan's winding sign is the opposite of GL's for the same window coordinates)
        vkCmdSetFrontFace(cmd, s.frontFace == GL_CW ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
        b.rasterDirty = false;
    }
    if (b.depthDirty)
    {
        vkCmdSetDepthTestEnable(cmd, s.depthTest && b.hasDepth);
        vkCmdSetDepthWriteEnable(cmd, s.depthTest && s.depthWrite && b.hasDepth);
        vkCmdSetDepthCompareOp(cmd, Compare(s.depthFunc));
        const bool stencil = s.stencilTest && b.hasDepth;
        vkCmdSetStencilTestEnable(cmd, stencil);
        vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_BIT, StencilOpOf(s.front.fail), StencilOpOf(s.front.zpass), StencilOpOf(s.front.zfail), Compare(s.front.func));
        vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_BACK_BIT, StencilOpOf(s.back.fail), StencilOpOf(s.back.zpass), StencilOpOf(s.back.zfail), Compare(s.back.func));
        if (g_feat.depthBounds)
        {
            const bool bounds = s.depthBoundsTest && b.hasDepth;
            vkCmdSetDepthBoundsTestEnable(cmd, bounds);
            if (bounds) vkCmdSetDepthBounds(cmd, s.depthBoundsMin, s.depthBoundsMax);
        }
        b.depthDirty = false;
    }
    if (b.stencilDirty)
    {
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_BIT, s.front.writeMask & 0xFF);
        vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_BACK_BIT, s.back.writeMask & 0xFF);
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_BIT, s.front.mask & 0xFF);
        vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_BACK_BIT, s.back.mask & 0xFF);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_BIT, uint32_t(s.front.ref) & 0xFF);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_BACK_BIT, uint32_t(s.back.ref) & 0xFF);
        b.stencilDirty = false;
    }
    if (g_feat.eds3 && b.blendDirty && b.hasColor)
    {
        const VkBool32 enable = s.blend ? VK_TRUE : VK_FALSE;
        vkCmdSetColorBlendEnableEXT(cmd, 0, 1, &enable);
        if (s.blend)
        {
            const VkColorBlendEquationEXT eq{Factor(s.blendSrc), Factor(s.blendDst), BlendOpOf(s.blendEq), Factor(s.blendSrcAlpha),
                                             Factor(s.blendDstAlpha), BlendOpOf(s.blendEqAlpha)};
            vkCmdSetColorBlendEquationEXT(cmd, 0, 1, &eq);
        }
        const VkColorComponentFlags mask = s.colorMask;
        vkCmdSetColorWriteMaskEXT(cmd, 0, 1, &mask);
        b.blendDirty = false;
    }
    if (b.blendConstDirty)
    {
        vkCmdSetBlendConstants(cmd, s.blendColor.data());
        b.blendConstDirty = false;
    }
    if (b.lineDirty)
    {
        vkCmdSetLineWidth(cmd, g_feat.wideLines ? std::clamp(s.lineWidth, 1.0f, g_feat.lineWidthMax) : 1.0f);
        b.lineDirty = false;
    }
}

// ------------------------------------------------------------------------------------------------ pipelines
struct VertexLayout
{
    uint32_t vaoMask = 0, genericMask = 0;
    std::array<VkFormat, kMaxAttribs> format{};
    std::array<uint32_t, kMaxAttribs> stride{};
    std::array<uint8_t, kMaxAttribs> instanced{};
    std::array<uint8_t, kMaxAttribs> genericKind{};
    bool operator==(const VertexLayout& o) const
    {
        return vaoMask == o.vaoMask && genericMask == o.genericMask && format == o.format && stride == o.stride &&
               instanced == o.instanced && genericKind == o.genericKind;
    }
};
std::vector<VertexLayout> g_layouts;
std::unordered_map<VkFormat, bool> g_vertexFormatOk;
bool VertexFormatSupported(VkFormat f)
{
    auto it = g_vertexFormatOk.find(f);
    if (it != g_vertexFormatOk.end()) return it->second;
    VkFormatProperties fp;
    vkGetPhysicalDeviceFormatProperties(g_phys, f, &fp);
    const bool ok = (fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
    g_vertexFormatOk.emplace(f, ok);
    return ok;
}
VkFormat AttribFormat(const Attrib& a)
{
    const unsigned n = a.components;
    auto pick = [&](const std::array<VkFormat, 4>& t) { return t[n - 1]; };
    VkFormat f = VK_FORMAT_UNDEFINED;
    switch (a.type)
    {
    case GL_FLOAT: f = pick({VK_FORMAT_R32_SFLOAT, VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT}); break;
    case GL_HALF_FLOAT: f = pick({VK_FORMAT_R16_SFLOAT, VK_FORMAT_R16G16_SFLOAT, VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16A16_SFLOAT}); break;
    case GL_UNSIGNED_BYTE:
        f = a.integer ? pick({VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8B8_UINT, VK_FORMAT_R8G8B8A8_UINT})
          : a.normalized ? pick({VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8B8_UNORM, VK_FORMAT_R8G8B8A8_UNORM})
          : pick({VK_FORMAT_R8_USCALED, VK_FORMAT_R8G8_USCALED, VK_FORMAT_R8G8B8_USCALED, VK_FORMAT_R8G8B8A8_USCALED});
        break;
    case GL_BYTE:
        f = a.integer ? pick({VK_FORMAT_R8_SINT, VK_FORMAT_R8G8_SINT, VK_FORMAT_R8G8B8_SINT, VK_FORMAT_R8G8B8A8_SINT})
          : a.normalized ? pick({VK_FORMAT_R8_SNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8B8_SNORM, VK_FORMAT_R8G8B8A8_SNORM})
          : pick({VK_FORMAT_R8_SSCALED, VK_FORMAT_R8G8_SSCALED, VK_FORMAT_R8G8B8_SSCALED, VK_FORMAT_R8G8B8A8_SSCALED});
        break;
    case GL_UNSIGNED_SHORT:
        f = a.integer ? pick({VK_FORMAT_R16_UINT, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16B16_UINT, VK_FORMAT_R16G16B16A16_UINT})
          : a.normalized ? pick({VK_FORMAT_R16_UNORM, VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16A16_UNORM})
          : pick({VK_FORMAT_R16_USCALED, VK_FORMAT_R16G16_USCALED, VK_FORMAT_R16G16B16_USCALED, VK_FORMAT_R16G16B16A16_USCALED});
        break;
    case GL_SHORT:
        f = a.integer ? pick({VK_FORMAT_R16_SINT, VK_FORMAT_R16G16_SINT, VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R16G16B16A16_SINT})
          : a.normalized ? pick({VK_FORMAT_R16_SNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16A16_SNORM})
          : pick({VK_FORMAT_R16_SSCALED, VK_FORMAT_R16G16_SSCALED, VK_FORMAT_R16G16B16_SSCALED, VK_FORMAT_R16G16B16A16_SSCALED});
        break;
    case GL_UNSIGNED_INT: f = pick({VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32_UINT, VK_FORMAT_R32G32B32A32_UINT}); break;
    case GL_INT: f = pick({VK_FORMAT_R32_SINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32B32_SINT, VK_FORMAT_R32G32B32A32_SINT}); break;
    default: f = VK_FORMAT_R32G32B32A32_SFLOAT; break;
    }
    // (three 8- or 16-bit components are often not a vertex format: the four-component one, its fourth unread)
    if (!VertexFormatSupported(f) && n == 3)
    {
        Attrib a4 = a;
        a4.components = 4;
        const VkFormat f4 = AttribFormat(a4);
        if (VertexFormatSupported(f4)) return f4;
    }
    return f;
}
uint16_t InternLayout(const VertexLayout& l)
{
    for (size_t k = 0; k < g_layouts.size(); ++k)
        if (g_layouts[k] == l) return uint16_t(k);
    g_layouts.push_back(l);
    return uint16_t(g_layouts.size() - 1);
}
uint16_t LayoutFor(const VertexArray& vao, const Program& p)
{
    VertexLayout l;
    for (unsigned i = 0; i < kGenericBinding; ++i)
    {
        if (!(p.info->inputMask & (1u << i))) continue;
        const Attrib& a = vao.attribs[i];
        if (a.enabled && a.buffer)
        {
            l.vaoMask |= 1u << i;
            l.format[i] = AttribFormat(a);
            l.stride[i] = a.stride;
            l.instanced[i] = a.divisor ? 1 : 0;
        }
        else
        {
            l.genericMask |= 1u << i;
            l.genericKind[i] = uint8_t((p.info->inputKinds >> (2 * i)) & 3);
        }
    }
    return InternLayout(l);
}

struct PipelineKey
{
    uint32_t program = 0;
    uint16_t layout = 0;
    uint8_t topology = 0, colorFormat = 0, depthFormat = 0, samples = 1, colorMask = 0xF, blend = 0, polygon = 0, pad[3]{};
    uint32_t blendFactors = 0;
    uint16_t spec = 0, pad2 = 0;   // (per-state specialization: g_specSets index, 0 none)
    bool operator==(const PipelineKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};
static_assert(sizeof(PipelineKey) == 24);

// ------------------------------------------------------------------------------------------------ per-state specialization
// (cmake/vulkan_shaders.py SPEC) The glob programs read the uniforms a packet state fixes for its run (the texture
// function, colour source, blend-pair mode, alpha test, fog black) through specialization constants where the
// pipeline has them: a packet state's values become a spec set (interned; PipelineKey::spec), and its draws take the
// pipeline made with them, so the driver folds those branches away. Draws without a packet state (the GL-style calls)
// use spec 0: the push block, as before. Off by default; SLY2N_VKSPEC=ff: on, =hex: only these SPEC uniforms (bit i = kSpecNames[i]).
constexpr const char* kSpecNames[8] = {"fTextured", "icolor", "caneBlend1", "ate", "atst", "aref", "afailMode", "fFogBlack"};
struct SpecData
{
    uint32_t mask = 0;
    uint32_t value[8] = {};
    bool operator==(const SpecData&) const = default;
};
std::vector<SpecData> g_specSets(1);
uint32_t g_specAllowed = 0;     // (SLY2N_VKSPEC)
uint16_t g_drawSpec = 0;        // (the packet draw being made: its state's spec set)
uint16_t InternSpec(const SpecData& d)
{
    if (!d.mask) return 0;
    for (size_t i = 1; i < g_specSets.size(); ++i)
        if (g_specSets[i] == d) return uint16_t(i);
    if (g_specSets.size() >= 0xFFFF) return 0;
    g_specSets.push_back(d);
    return uint16_t(g_specSets.size() - 1);
}
struct PipelineKeyHash
{
    size_t operator()(const PipelineKey& k) const
    {
        uint64_t h = 1469598103934665603ull;
        const auto* p = reinterpret_cast<const uint8_t*>(&k);
        for (size_t i = 0; i < sizeof(k); ++i) h = (h ^ p[i]) * 1099511628211ull;
        return size_t(h);
    }
};
std::unordered_map<PipelineKey, VkPipeline, PipelineKeyHash> g_pipelines;

// (SLY2N_VKPIPELOG=1: each frame that made pipelines on the game thread, with the time spent, and a summary at the end)
uint64_t g_frameSerial = 0;                 // (frames presented since the start)
double g_pipeMsFrame = 0, g_pipeMsTotal = 0, g_pipeMsMaxFrame = 0;
unsigned g_pipeCountFrame = 0, g_pipeCountTotal = 0;
double MsSince(std::chrono::steady_clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
}
// What a pipeline is made from besides its key: the program's modules, the vertex layout, the classic render pass (all
// copied, so the prewarm worker never reads the game thread's tables)
struct PipelineSource
{
    std::array<VkShaderModule, 3> modules{}, modulesMv{};
    VertexLayout layout;
    VkRenderPass classic = VK_NULL_HANDLE;
    SpecData spec;
};
VkPipeline CreatePipeline(const PipelineKey& k, const PipelineSource& p, bool fGameThread = true)
{
    std::array<VkPipelineShaderStageCreateInfo, 3> stages{};
    uint32_t nStages = 0;
    const VkShaderStageFlagBits kinds[3] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    // (per-state specialization: constant 0 the mask, i + 1 SPEC uniform i's value; the same for every stage)
    VkSpecializationMapEntry specEntries[9];
    for (uint32_t i = 0; i < 9; ++i) specEntries[i] = {i, i * 4, 4};
    const VkSpecializationInfo specInfo{9, specEntries, sizeof(SpecData), &p.spec};
    for (unsigned i = 0; i < 3; ++i)
    {
        if (!p.modules[i]) continue;
        VkPipelineShaderStageCreateInfo& st = stages[nStages++];
        st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st.stage = kinds[i];
        st.module = (k.pad[2] & 2) && p.modulesMv[i] ? p.modulesMv[i] : p.modules[i];
        st.pName = "main";
        if (p.spec.mask) st.pSpecializationInfo = &specInfo;
    }
    const VertexLayout& l = p.layout;
    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attributes;
    for (unsigned i = 0; i < kGenericBinding; ++i)
    {
        if (l.vaoMask & (1u << i))
        {
            bindings.push_back({i, l.stride[i], l.instanced[i] ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX});
            attributes.push_back({i, i, l.format[i], 0});
        }
        else if (l.genericMask & (1u << i))
        {
            const uint8_t kind = l.genericKind[i];
            const VkFormat f = kind == 0 ? VK_FORMAT_R32G32B32A32_SFLOAT : kind == 1 ? VK_FORMAT_R32G32B32A32_SINT : VK_FORMAT_R32G32B32A32_UINT;
            attributes.push_back({i, kGenericBinding, f, kind == 0 ? i * 16 : 256 + i * 16});
        }
    }
    if (l.genericMask) bindings.push_back({kGenericBinding, 0, VK_VERTEX_INPUT_RATE_VERTEX});
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = uint32_t(bindings.size());
    vi.pVertexBindingDescriptions = bindings.data();
    vi.vertexAttributeDescriptionCount = uint32_t(attributes.size());
    vi.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VkPrimitiveTopology(k.topology);
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = k.polygon == 1 ? VK_POLYGON_MODE_LINE : k.polygon == 2 ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL;
    rs.lineWidth = 1.0f;
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pv{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_PROVOKING_VERTEX_STATE_CREATE_INFO_EXT};
    pv.provokingVertexMode = VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT;
    if (g_feat.provokingLast) rs.pNext = &pv;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = SampleBits(k.samples);
    VkMultisampledRenderToSingleSampledInfoEXT msi{VK_STRUCTURE_TYPE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_INFO_EXT};
    msi.multisampledRenderToSingleSampledEnable = VK_TRUE;
    msi.rasterizationSamples = SampleBits(k.samples);
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.maxDepthBounds = 1.0f;
    VkPipelineColorBlendAttachmentState att{};
    att.blendEnable = k.blend ? VK_TRUE : VK_FALSE;
    att.srcColorBlendFactor = VkBlendFactor(k.blendFactors & 0x1F);
    att.dstColorBlendFactor = VkBlendFactor((k.blendFactors >> 5) & 0x1F);
    att.srcAlphaBlendFactor = VkBlendFactor((k.blendFactors >> 10) & 0x1F);
    att.dstAlphaBlendFactor = VkBlendFactor((k.blendFactors >> 15) & 0x1F);
    att.colorBlendOp = VkBlendOp((k.blendFactors >> 20) & 0x7);
    att.alphaBlendOp = VkBlendOp((k.blendFactors >> 23) & 0x7);
    att.colorWriteMask = k.colorMask;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = k.colorFormat ? 1 : 0;
    cb.pAttachments = &att;
    static const VkDynamicState kDynamic[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_LINE_WIDTH, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_CULL_MODE, VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
        VK_DYNAMIC_STATE_STENCIL_OP};
    std::vector<VkDynamicState> dynamic(std::begin(kDynamic), std::end(kDynamic));
    if (g_feat.depthBounds) dynamic.push_back(VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE), dynamic.push_back(VK_DYNAMIC_STATE_DEPTH_BOUNDS);
    if (g_feat.eds3 && k.colorFormat)
        for (VkDynamicState d : {VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT, VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT, VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT})
            dynamic.push_back(d);
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = uint32_t(dynamic.size());
    dy.pDynamicStates = dynamic.data();
    const VkFormat colorFormat = VK_FORMAT_R8G8B8A8_UNORM;
    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rci.colorAttachmentCount = k.colorFormat ? 1 : 0;
    rci.pColorAttachmentFormats = &colorFormat;
    rci.depthAttachmentFormat = k.depthFormat ? g_feat.depthStencil : VK_FORMAT_UNDEFINED;
    rci.stencilAttachmentFormat = rci.depthAttachmentFormat;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &rci;
    if (k.pad[0]) msi.pNext = rci.pNext, rci.pNext = &msi;
    if ((k.pad[2] & 1) && !k.pad[1]) ci.flags |= VK_PIPELINE_CREATE_RENDERING_FRAGMENT_DENSITY_MAP_ATTACHMENT_BIT_EXT;
    const bool multiview = (k.pad[2] & 2) != 0;
    rci.viewMask = multiview ? 3u : 0u;
    if (k.pad[1])
    {
        // (classic fetch: for the render pass object's subpass; any of its load / store variants is compatible)
        ci.pNext = nullptr;
        ci.renderPass = p.classic;
        ci.subpass = 0;
    }
    ci.stageCount = nStages;
    ci.pStages = stages.data();
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dy;
    ci.layout = g_pipelineLayout;
    VkPipeline pipeline;
    const auto t0 = std::chrono::steady_clock::now();
    const VkResult r = FForcedFailure("pipeline") ? VK_ERROR_INITIALIZATION_FAILED
                                                  : vkCreateGraphicsPipelines(g_device, g_pipelineCache, 1, &ci, nullptr, &pipeline);
    if (r != VK_SUCCESS) Fatal("vkCreateGraphicsPipelines failed (%d)", int(r));
    if (fGameThread)
    {
        ++g_stats.pipelinesCreated;
        g_pipeMsFrame += MsSince(t0);
        ++g_pipeCountFrame;
    }
    return pipeline;
}
// (classic fetch) the render pass object a pipeline of this key is made for: any load / store variant is compatible
VkRenderPass ClassicPassFor(const PipelineKey& k)
{
    if (!k.pad[1]) return VK_NULL_HANDLE;
    ClassicKey compat;
    compat.color = VK_FORMAT_R8G8B8A8_UNORM;
    compat.depth = k.depthFormat ? g_feat.depthStencil : VK_FORMAT_UNDEFINED;
    compat.fdm = (k.pad[2] & 1) != 0;
    compat.viewMask = (k.pad[2] & 2) ? 3 : 0;
    return ClassicRenderPass(compat);
}

// ------------------------------------------------------------------------------------------------ pipeline prewarm
// Every pipeline made is noted in a manifest (<pipeline cache>.keys: the program by its build-time key, the vertex
// layout, the rest of the key, and the scope it was first used in: the game's world, PrewarmPipelines). When the game
// enters a scope again (a world's load, a later run, a new build whose driver cache is cold), a worker thread makes that
// scope's pipelines from the manifest (through the driver's disk cache) while the load finishes; the load waits a short
// while for it. A pipeline the game needs before the worker got to it is made on the game thread as before (the
// worker's own copy is then dropped). SLY2N_VKPREWARM=0: no manifest, no worker.
struct ManifestEntry
{
    uint32_t program = 0;          // (ProgramHashOf: the build-time key, the block's source and the SPIR-V)
    int32_t scope = 0;
    PipelineKey key;               // (program serial, layout index and spec 0)
    VertexLayout layout;
    SpecData spec;
};
static_assert(std::is_trivially_copyable_v<ManifestEntry>);
struct PrewarmJob
{
    PipelineKey key;
    PipelineSource source;
    std::shared_ptr<std::vector<ManifestEntry>> save;   // (instead: write these entries and the driver cache)
};
std::mutex g_pipeMutex;                  // (g_pipelines, while the prewarm worker runs)
bool g_prewarmOn = false;
int32_t g_scope = INT32_MIN;            // (the game's scope: its world; INT32_MIN none set)
uint32_t g_scopeSerial = 1;             // (changes with the scope: a pipeline noted for the current one)
std::unordered_map<VkPipeline, uint32_t> g_pipeScopeMark;   // (pipeline -> the scope serial it was last noted for)
std::vector<ManifestEntry> g_manifest;
std::unordered_map<uint64_t, uint32_t> g_manifestIndex;      // (entry hash -> count, dedup)
size_t g_manifestSaved = 0;             // (entries in the file)
std::vector<int32_t> g_scopesWarmed;
std::deque<PrewarmJob> g_prewarmJobs;
std::mutex g_prewarmMutex;
std::condition_variable g_prewarmCv, g_prewarmIdleCv;
bool g_prewarmBusy = false, g_prewarmQuit = false;
std::thread g_prewarmThread;
unsigned g_prewarmMade = 0, g_prewarmDropped = 0;
uint64_t g_deviceSignature = 0;
uint64_t g_manifestNoteFrame = 0;       // (the frame of the last new entry)
void SavePipelineCache();

uint64_t Fnv64(const void* data, size_t n, uint64_t h = 1469598103934665603ull)
{
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}
// (experiment, notes/VKGPU section 3b) the float16 level of glob's fragment maths: SLY2N_VKFP16, else the platform's
// default (kFp16Default*: 0 everywhere for now; Android / the Quest: kFp16DefaultAndroid)
int Fp16Level()
{
#if defined(__ANDROID__)
    constexpr int kFp16Default = 0;   // (kFp16DefaultAndroid)
#else
    constexpr int kFp16Default = 0;
#endif
    static const int s_level = std::getenv("SLY2N_VKFP16") ? std::atoi(std::getenv("SLY2N_VKFP16")) : kFp16Default;
    return s_level;
}
// (a table program the run can use: the float16 variants only at their level, so with the switch off the prewarm's
// look-ups never hash them)
bool FProgramInUse(const Shaders::Program& p)
{
    const char* f = std::strstr(p.defines, "CANE_FP16=");
    if (!f) return true;
    const int level = Fp16Level();
    return (level == 1 || level == 2) && g_feat.float16 && f[10] == char('0' + level);
}
// (a program changed by a new build gets another hash: its old entries are not made for it)
uint32_t ProgramHashOf(const Shaders::Program* info, bool uniformBuffer)
{
    uint64_t h = Fnv64(info->key, std::strlen(info->key), uniformBuffer ? 0x5542ull : 0x5043ull);
    for (unsigned k = 0; k < 3; ++k)
    {
        const uint32_t* code = uniformBuffer ? info->spirvUbo[k] : info->spirv[k];
        if (code) h = Fnv64(code, uniformBuffer ? info->spirvUboSize[k] : info->spirvSize[k], h);
    }
    return uint32_t(h ^ (h >> 32));
}
uint32_t ProgramHash(const Program& p) { return p.hash; }
uint64_t EntryHash(const ManifestEntry& e) { return Fnv64(&e, sizeof e); }
void NoteManifest(const PipelineKey& k, const Program& p, const VertexLayout& layout)
{
    if (!g_prewarmOn || g_scope == INT32_MIN || !p.info) return;
    ManifestEntry e;
    std::memset(&e, 0, sizeof e);
    e.program = ProgramHash(p);
    e.scope = g_scope;
    e.key = k;
    e.key.program = 0;
    e.key.layout = 0;
    e.key.spec = 0;
    e.layout = layout;
    e.spec = g_specSets[k.spec];
    if (g_manifestIndex.emplace(EntryHash(e), 0).second)
    {
        g_manifest.push_back(e);
        g_manifestNoteFrame = g_frameSerial;
    }
}
std::string ManifestPath() { return g_cachePath.empty() ? std::string() : g_cachePath + ".keys"; }
constexpr uint32_t kManifestMagic = 0x4D4B5643u, kManifestVersion = 2;   // "CVKM"
void LoadManifest()
{
    const std::string path = ManifestPath();
    if (path.empty()) return;
    std::ifstream f(path, std::ios::binary);
    uint32_t head[4] = {};
    uint64_t sig = 0;
    if (!f.read(reinterpret_cast<char*>(head), sizeof head) || !f.read(reinterpret_cast<char*>(&sig), sizeof sig)) return;
    // (another device, driver or feature set: other keys; the file is written anew)
    if (head[0] != kManifestMagic || head[1] != kManifestVersion || head[2] != sizeof(ManifestEntry) || sig != g_deviceSignature) return;
    const uint32_t n = std::min<uint32_t>(head[3], 1u << 16);
    for (uint32_t i = 0; i < n; ++i)
    {
        ManifestEntry e;
        if (!f.read(reinterpret_cast<char*>(&e), sizeof e)) break;
        if (g_manifestIndex.emplace(EntryHash(e), 0).second) g_manifest.push_back(e);
    }
    g_manifestSaved = g_manifest.size();
}
void WriteManifest(const std::vector<ManifestEntry>& entries)
{
    const std::string path = ManifestPath();
    const std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        const uint32_t head[4] = {kManifestMagic, kManifestVersion, uint32_t(sizeof(ManifestEntry)), uint32_t(entries.size())};
        f.write(reinterpret_cast<const char*>(head), sizeof head);
        f.write(reinterpret_cast<const char*>(&g_deviceSignature), sizeof g_deviceSignature);
        f.write(reinterpret_cast<const char*>(entries.data()), std::streamsize(entries.size() * sizeof(ManifestEntry)));
        if (!f) return;
    }
    std::remove(path.c_str());
    std::rename(tmp.c_str(), path.c_str());
}
void SaveManifest()
{
    if (ManifestPath().empty() || !g_prewarmOn || g_manifest.size() == g_manifestSaved) return;
    WriteManifest(g_manifest);
    g_manifestSaved = g_manifest.size();
}
void PrewarmLoop()
{
    for (;;)
    {
        PrewarmJob job;
        {
            std::unique_lock<std::mutex> lock(g_prewarmMutex);
            g_prewarmBusy = false;
            if (g_prewarmJobs.empty()) g_prewarmIdleCv.notify_all();
            g_prewarmCv.wait(lock, [] { return g_prewarmQuit || !g_prewarmJobs.empty(); });
            if (g_prewarmQuit) { g_prewarmIdleCv.notify_all(); return; }
            job = g_prewarmJobs.front();
            g_prewarmJobs.pop_front();
            g_prewarmBusy = true;
        }
        if (job.save)
        {
            WriteManifest(*job.save);
            SavePipelineCache();
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(g_pipeMutex);
            if (g_pipelines.count(job.key)) continue;
        }
        const VkPipeline pipeline = CreatePipeline(job.key, job.source, false);
        std::lock_guard<std::mutex> lock(g_pipeMutex);
        if (g_pipelines.emplace(job.key, pipeline).second) ++g_prewarmMade;
        else
        {
            vkDestroyPipeline(g_device, pipeline, nullptr);   // (the game thread made it meanwhile)
            ++g_prewarmDropped;
        }
    }
}
void StopPrewarm()
{
    if (!g_prewarmThread.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(g_prewarmMutex);
        g_prewarmQuit = true;
        g_prewarmJobs.clear();
    }
    g_prewarmCv.notify_all();
    g_prewarmThread.join();
    g_prewarmQuit = false;
}
// true: the queue is done (false: still working after waitMs)
bool WaitPrewarm(unsigned waitMs)
{
    std::unique_lock<std::mutex> lock(g_prewarmMutex);
    return g_prewarmIdleCv.wait_for(lock, std::chrono::milliseconds(waitMs), [] { return g_prewarmJobs.empty() && !g_prewarmBusy; });
}

VkPipeline PipelineFor(const PipelineKey& k, const Program& p)
{
    {
        std::unique_lock<std::mutex> lock(g_pipeMutex, std::defer_lock);
        if (g_prewarmOn) lock.lock();
        auto it = g_pipelines.find(k);
        if (it != g_pipelines.end())
        {
            const VkPipeline pipeline = it->second;
            if (lock.owns_lock()) lock.unlock();
            // (made in another scope, or by the worker: noted for this one too)
            if (g_prewarmOn)
            {
                uint32_t& mark = g_pipeScopeMark[pipeline];
                if (mark != g_scopeSerial) { mark = g_scopeSerial; NoteManifest(k, p, g_layouts[k.layout]); }
            }
            return pipeline;
        }
    }
    PipelineSource src;
    src.modules = p.modules, src.modulesMv = p.modulesMv;
    src.layout = g_layouts[k.layout];
    src.classic = ClassicPassFor(k);
    src.spec = g_specSets[k.spec];
    const VkPipeline pipeline = CreatePipeline(k, src);
    {
        std::unique_lock<std::mutex> lock(g_pipeMutex, std::defer_lock);
        if (g_prewarmOn) lock.lock();
        g_pipelines.emplace(k, pipeline);   // (the worker checks before it makes one)
    }
    if (g_prewarmOn)
    {
        g_pipeScopeMark[pipeline] = g_scopeSerial;
        NoteManifest(k, p, src.layout);
    }
    return pipeline;
}
// (the depth format's key: one depth format for every target here)
uint8_t PolygonCode() { return s.polygonMode == GL_LINE ? 1 : s.polygonMode == GL_POINT ? 2 : 0; }

void ApplyPipeline(GLenum mode, Program& p)
{
    VertexArray* vao = g_arrays.Get(s.vertexArray);
    if (b.vao != vao || b.vaoVersion != vao->version || b.vaoProgram != p.serial)
    {
        const uint16_t layout = LayoutFor(*vao, p);
        if (layout != b.layout) b.pipelineDirty = true;
        b.layout = layout;
        b.vao = vao;
        b.vaoVersion = vao->version;
        b.vaoProgram = p.serial;
    }
    if (mode != b.mode) { b.mode = mode; b.pipelineDirty = true; }
    if (b.programSerial != p.serial) b.pipelineDirty = true;
    if (b.spec != g_drawSpec) { b.spec = g_drawSpec; b.pipelineDirty = true; }
    if (!b.pipelineDirty) return;
    PipelineKey k;
    k.program = p.serial;
    k.layout = b.layout;
    k.topology = uint8_t(Topology(mode));
    k.colorFormat = b.colorFormat;
    k.depthFormat = b.depthFormat;
    k.samples = uint8_t(b.samples);
    k.pad[0] = b.msrtss ? 1 : 0;   // (msrtss: the pipeline carries VkMultisampledRenderToSingleSampledInfoEXT)
    k.pad[1] = b.classic ? 1 : 0;  // (classic fetch: made for the render pass object)
    k.pad[2] = uint8_t((b.fdm ? 1 : 0) | (b.multiview ? 2 : 0));   // (drawn with a fragment density map; two views)
    k.colorMask = g_feat.eds3 ? 0xF : uint8_t(s.colorMask);
    k.blend = s.blend && !g_feat.eds3 ? 1 : 0;
    k.polygon = PolygonCode();
    k.spec = g_drawSpec;
    if (s.blend && !g_feat.eds3)
        k.blendFactors = FactorCode(s.blendSrc) | (FactorCode(s.blendDst) << 5) | (FactorCode(s.blendSrcAlpha) << 10) |
                         (FactorCode(s.blendDstAlpha) << 15) | (BlendOpCode(s.blendEq) << 20) | (BlendOpCode(s.blendEqAlpha) << 23);
    const VkPipeline pipeline = PipelineFor(k, p);
    if (pipeline != b.pipeline)
    {
        vkCmdBindPipeline(g_main, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
        b.pipeline = pipeline;
        ++g_stats.pipelines;
    }
    b.programSerial = p.serial;
    b.pipelineDirty = false;
}

// ------------------------------------------------------------------------------------------------ resources
GLuint g_blackTexture = 0, g_blackArray = 0;
// The slots a program's sampler reads from a unit: the black texture (GL's incomplete texture reads 0, 0, 0, 1) for an
// empty unit, a texture of the other kind, or one a mipmapped filter would find incomplete.
std::pair<uint32_t, uint32_t> TextureSlots(unsigned unit, bool array)
{
    Texture* t = g_textures.Get(s.units[unit][array ? 1 : 0]);
    Sampler* sm = s.samplerUnits[unit] ? g_samplers.Get(s.samplerUnits[unit]) : nullptr;
    if (t && t->image && t->image->sampled && t->image->array == array)
    {
        const SamplerParams& params = sm ? sm->params : t->params;
        if (MipFilter(params.minFilter) && !t->image->immutable &&
            t->image->levels < std::min<unsigned>(FullLevels(t->image->width, t->image->height), unsigned(std::max(params.maxLevel, 0)) + 1))
            t = nullptr;
    }
    else t = nullptr;
    if (!t) t = g_textures.Get(array ? g_blackArray : g_blackTexture);
    int slot;
    if (sm)
    {
        if (sm->samplerSlot < 0) sm->samplerSlot = SamplerSlot(sm->params);
        slot = sm->samplerSlot;
    }
    else
    {
        if (t->samplerSlot < 0) t->samplerSlot = SamplerSlot(t->params);
        slot = t->samplerSlot;
    }
    t->image->useSegment = g_segment;
    return {uint32_t(t->image->slot), uint32_t(slot)};
}
// A descriptor set of the current frame (freed when it retires).
VkDescriptorSet FrameSet(VkDescriptorSetLayout layout)
{
    Frame& f = Cur();
    for (;; ++f.setPoolIndex)
    {
        if (f.setPoolIndex == f.setPools.size())
        {
            const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSetPoolSize * kMaxBindings},
                                                   {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, kSetPoolSize}};
            VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            pi.maxSets = kSetPoolSize;
            pi.poolSizeCount = 2;
            pi.pPoolSizes = sizes;
            VkDescriptorPool pool;
            VKCHECK(vkCreateDescriptorPool(g_device, &pi, nullptr, &pool));
            f.setPools.push_back(pool);
        }
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = f.setPools[f.setPoolIndex];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &layout;
        VkDescriptorSet set;
        if (vkAllocateDescriptorSets(g_device, &ai, &set) == VK_SUCCESS) return set;
    }
}
// Set 2 for a ring buffer (its range a block's worth; the draw's place in it is the dynamic offset).
VkDescriptorSet UniformSet(VkBuffer buffer)
{
    Frame& f = Cur();
    for (const auto& [b2, set] : f.uniformSets) if (b2 == buffer) return set;
    const VkDescriptorSet set = FrameSet(g_setLayoutUniforms);
    VkDescriptorBufferInfo info{buffer, 0, kPushBytes};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = set;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    w.pBufferInfo = &info;
    vkUpdateDescriptorSets(g_device, 1, &w, 0, nullptr);
    f.uniformSets.emplace_back(buffer, set);
    return set;
}

void ApplyResources(Program& p)
{
    VkCommandBuffer cmd = g_main;
    const bool fProgram = b.program != s.program;
    for (unsigned k = 0; k < p.samplerCount; ++k)
    {
        const auto [image, sampler] = TextureSlots(p.samplerUnit[k], p.samplerArray[k]);
        uint32_t pair[2] = {image, sampler};
        uint8_t* at = p.constants.data() + 8 * k;
        if (std::memcmp(at, pair, 8) != 0)
        {
            std::memcpy(at, pair, 8);
            p.dirtyLo = std::min(p.dirtyLo, 8 * k);
            p.dirtyHi = std::max(p.dirtyHi, 8 * k + 8);
        }
    }
    // (the push constants outlive pipeline binds: only the bytes that differ from what the command buffer holds are
    // pushed, whichever program pushed them; the glob variants share one layout)
    const uint32_t size = p.info->pushSize;
    if (p.uniformBuffer)
    {
        // (a block larger than the device's push constants: a copy in this frame's ring, bound with a dynamic offset)
        if (fProgram || p.dirtyLo < p.dirtyHi)
        {
            const Upload u = Ring(kPushBytes, g_uniformAlign);
            std::memcpy(u.cpu, p.constants.data(), size);
            const VkDescriptorSet set = UniformSet(u.buffer);
            const uint32_t offset = uint32_t(u.offset);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipelineLayout, 2, 1, &set, 1, &offset);
            ++g_stats.pushes, g_stats.pushBytes += size;
        }
    }
    else if (size && (fProgram || p.dirtyLo < p.dirtyHi))
    {
        uint32_t lo = 0, hi = size;
        if (g_pushValid >= size)
        {
            // (this program pushed last: the command buffer holds its bytes outside the range written since)
            if (!fProgram) lo = std::min(p.dirtyLo, size), hi = std::min(p.dirtyHi, size);
            while (lo < hi && g_pushShadow[lo] == p.constants[lo]) ++lo;
            while (hi > lo && g_pushShadow[hi - 1] == p.constants[hi - 1]) --hi;
        }
        lo &= ~3u;
        hi = std::min<uint32_t>(uint32_t(Align(hi, 4)), size);
        if (lo < hi)
        {
            vkCmdPushConstants(cmd, g_pipelineLayout, g_stages, lo, hi - lo, p.constants.data() + lo);
            std::memcpy(g_pushShadow.data() + lo, p.constants.data() + lo, hi - lo);
            ++g_stats.pushes, g_stats.pushBytes += hi - lo;
        }
        if (g_pushValid < size)
        {
            if (lo > 0) { vkCmdPushConstants(cmd, g_pipelineLayout, g_stages, 0, lo, p.constants.data()); std::memcpy(g_pushShadow.data(), p.constants.data(), lo); }
            if (hi < size) { vkCmdPushConstants(cmd, g_pipelineLayout, g_stages, hi, size - hi, p.constants.data() + hi); std::memcpy(g_pushShadow.data() + hi, p.constants.data() + hi, size - hi); }
            g_pushValid = size;
        }
    }
    p.dirtyLo = UINT32_MAX;
    p.dirtyHi = 0;
    b.program = s.program;
    // (the storage buffers pushed again only when the program, a storage binding or a buffer's storage changed)
    const uint16_t storageMask = b.multiview ? p.storageMaskMv : p.storageMask;
    if (storageMask && (b.storageProgram != s.program || b.storageMv != b.multiview || b.storageEpoch != g_storageEpoch ||
                        b.storageAddrEpoch != g_addrEpoch))
    {
        std::array<VkDescriptorBufferInfo, kMaxBindings> infos{};
        std::array<VkWriteDescriptorSet, kMaxBindings> writes{};
        uint32_t n = 0;
        unsigned mask = storageMask;
        while (mask)
        {
            const unsigned slot = unsigned(std::countr_zero(mask));
            mask &= mask - 1;
            const Range& r = s.storage[slot];
            Buffer* buf = g_buffers.Get(r.buffer);
            VkDescriptorBufferInfo& info = infos[n];
            info = {g_nullStorage.Buffer(), g_nullStorage.offset, 256};
            if (buf && buf->size && buf->vk && r.offset < buf->size)
            {
                Use(*buf);
                uint32_t sz = r.whole || !r.size ? buf->size - r.offset : std::min(r.size, buf->size - r.offset);
                info = {buf->vk, buf->offset + r.offset, std::max<VkDeviceSize>(sz, 4)};
            }
            VkWriteDescriptorSet& w = writes[n++];
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstBinding = slot;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.pBufferInfo = &info;
        }
        if (g_feat.pushDescriptors) vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipelineLayout, 1, n, writes.data());
        else
        {
            // (no push descriptors: a set of this frame's, written now, bound in order)
            const VkDescriptorSet set = FrameSet(g_setLayoutBuffers);
            for (uint32_t i = 0; i < n; ++i) writes[i].dstSet = set;
            vkUpdateDescriptorSets(g_device, n, writes.data(), 0, nullptr);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipelineLayout, 1, 1, &set, 0, nullptr);
        }
        ++g_stats.descriptorPushes;
        b.storageProgram = s.program;
        b.storageMv = b.multiview;
        b.storageEpoch = g_storageEpoch;
        b.storageAddrEpoch = g_addrEpoch;
    }
}
void ApplyVertexInput()
{
    VkCommandBuffer cmd = g_main;
    VertexArray* vao = g_arrays.Get(s.vertexArray);
    const VertexLayout& l = g_layouts[b.layout];
    if (l.genericMask && !b.genericBound)
    {
        const VkBuffer buf = g_generic.Buffer();
        const VkDeviceSize off = g_generic.offset;
        vkCmdBindVertexBuffers(cmd, kGenericBinding, 1, &buf, &off);
        b.genericBound = true;
    }
    if (b.vertexVao == vao && b.vertexVaoVersion == vao->version && b.vertexAddrEpoch == g_addrEpoch && b.vertexLayout == b.layout) return;
    for (unsigned i = 0; i < kGenericBinding; ++i)
    {
        if (!(l.vaoMask & (1u << i))) continue;
        const Attrib& a = vao->attribs[i];
        Buffer* buf = g_buffers.Get(a.buffer);
        VkBuffer vk = g_nullStorage.Buffer();
        VkDeviceSize off = g_nullStorage.offset;
        if (buf && buf->vk && a.offset < buf->size) { Use(*buf); vk = buf->vk; off = buf->offset + a.offset; }
        if (b.vertexBuffers[i] != vk || b.vertexOffsets[i] != off)
        {
            vkCmdBindVertexBuffers(cmd, i, 1, &vk, &off);
            b.vertexBuffers[i] = vk;
            b.vertexOffsets[i] = off;
        }
    }
    b.vertexVao = vao;
    b.vertexVaoVersion = vao->version;
    b.vertexAddrEpoch = g_addrEpoch;
    b.vertexLayout = b.layout;
}

// A CANE_FETCH draw: the attachment as its input attachment (pushed), after the earlier draws' writes to it (a
// by-region barrier inside the instance: VK_KHR_dynamic_rendering_local_read)
void ApplyFetch()
{
    VkCommandBuffer cmd = g_main;
    // (multiview: the attachment's own array view: each view reads its layer)
    const unsigned nviews = g_target.views > 1 ? g_target.views : 1;
    VkDescriptorImageInfo info{VK_NULL_HANDLE, AttachView(*g_target.color, g_target.colorLevel, g_target.colorLayer, nviews),
                               g_instanceClassic ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_RENDERING_LOCAL_READ_KHR};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstBinding = kFetchBinding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
    w.pImageInfo = &info;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipelineLayout, 1, 1, &w);
    ++g_stats.descriptorPushes;
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT;
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT | (nviews > 1 ? VK_DEPENDENCY_VIEW_LOCAL_BIT : 0);   // (multiview: within each view)
    di.memoryBarrierCount = 1;
    di.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &di);
}

bool PrepareDraw(GLenum mode)
{
    if (!g_recording) BeginFrame();
    FlushCopies();
    ApplyTarget();
    Program* p = g_programs.Get(s.program);
    if (!p || !(g_target.color || g_target.depth)) return false;
    if (p->fetch && (!g_target.color || !g_target.color->inputAttachment)) return false;   // (FramebufferFetchUsable said no)
    // (an instance begun before the first CANE_FETCH program: begun again in the local-read layout)
    if (p->fetch && g_inRendering && !g_instanceLocalRead && !g_instanceClassic) EndRendering();
    EnsureRendering();
    if (g_invalidatedColor || g_invalidatedDepth) RestoreInvalidatedStores();
    if (s.colorMask & 0xF) g_instWroteColor = true;
    if ((s.depthTest && s.depthWrite) || (s.stencilTest && ((s.front.writeMask | s.back.writeMask) & 0xFF))) g_instWroteDepth = true;
    ApplyPipeline(mode, *p);
    ApplyDynamic();
    ApplyResources(*p);
    if (p->fetch) ApplyFetch();
    ApplyVertexInput();
    ++g_stats.draws;
    return true;
}

void APIENTRY DrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instances, GLuint baseInstance)
{
    if (count <= 0 || instances <= 0 || first < 0) return;
    if (!PrepareDraw(mode)) return;
    vkCmdDraw(g_main, uint32_t(count), uint32_t(instances), uint32_t(first), baseInstance);
}
void APIENTRY DrawArrays(GLenum mode, GLint first, GLsizei count) { DrawArraysInstancedBaseInstance(mode, first, count, 1, 0); }
void APIENTRY DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instances) { DrawArraysInstancedBaseInstance(mode, first, count, instances, 0); }
bool BindIndices(GLenum type)
{
    VertexArray* vao = g_arrays.Get(s.vertexArray);
    Buffer* buf = g_buffers.Get(vao->elements);
    if (!buf || !buf->vk || type == GL_UNSIGNED_BYTE) return false;
    Use(*buf);
    const VkIndexType t = type == GL_UNSIGNED_SHORT ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    if (b.indexBuffer != buf->vk || b.indexOffset != buf->offset || b.indexType != t)
    {
        vkCmdBindIndexBuffer(g_main, buf->vk, buf->offset, t);
        b.indexBuffer = buf->vk;
        b.indexOffset = buf->offset;
        b.indexType = t;
    }
    return true;
}
void APIENTRY DrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances, GLint baseVertex, GLuint baseInstance)
{
    if (count <= 0 || instances <= 0) return;
    if (!PrepareDraw(mode) || !BindIndices(type)) return;
    const uint32_t first = uint32_t(reinterpret_cast<uintptr_t>(indices) / ComponentBytes(type));
    vkCmdDrawIndexed(g_main, uint32_t(count), uint32_t(instances), first, baseVertex, baseInstance);
}
void APIENTRY DrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices)
{ DrawElementsInstancedBaseVertexBaseInstance(mode, count, type, indices, 1, 0, 0); }
void APIENTRY DrawElementsBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint baseVertex)
{ DrawElementsInstancedBaseVertexBaseInstance(mode, count, type, indices, 1, baseVertex, 0); }
void APIENTRY DrawElementsInstanced(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances)
{ DrawElementsInstancedBaseVertexBaseInstance(mode, count, type, indices, instances, 0, 0); }
void APIENTRY DrawElementsInstancedBaseVertex(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances, GLint baseVertex)
{ DrawElementsInstancedBaseVertexBaseInstance(mode, count, type, indices, instances, baseVertex, 0); }
void APIENTRY DrawElementsInstancedBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances, GLuint baseInstance)
{ DrawElementsInstancedBaseVertexBaseInstance(mode, count, type, indices, instances, 0, baseInstance); }
void APIENTRY MultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect, GLsizei drawCount, GLsizei stride)
{
    if (drawCount <= 0) return;
    Buffer* buf = g_buffers.Get(s.indirectBuffer);
    if (!buf || !buf->vk) return;
    if (!PrepareDraw(mode) || !BindIndices(type)) return;
    struct Command { uint32_t count, instances, first; int32_t baseVertex; uint32_t baseInstance; };
    const uint32_t step = stride ? uint32_t(stride) : uint32_t(sizeof(Command));
    const size_t offset = size_t(reinterpret_cast<uintptr_t>(indirect));
    if (offset + size_t(step) * size_t(drawCount - 1) + sizeof(Command) > buf->size) { SetError(GL_INVALID_OPERATION); return; }
    if (g_feat.mdi && g_feat.firstInstance)
    {
        Use(*buf);
        vkCmdDrawIndexedIndirect(g_main, buf->vk, buf->offset + offset, uint32_t(drawCount), step);
        return;
    }
    if (!buf->keepShadow) return;
    for (GLsizei i = 0; i < drawCount; ++i)
    {
        Command c;
        std::memcpy(&c, buf->shadow.data() + offset + size_t(i) * step, sizeof(c));
        if (c.count && c.instances) vkCmdDrawIndexed(g_main, c.count, c.instances, c.first, c.baseVertex, c.baseInstance);
    }
}

// ------------------------------------------------------------------------------------------------ draw packets
// (platform/draw_packet.h) A packet state decided once: its fixed-function values as the GL state they stand for (the
// fields of s the equivalent GL calls set) and its constants as places in the program's block. A draw with it writes
// those directly: the group of fixed values skipped while s still holds them (g_fixedPacket), the constants compared
// word by word; the pipeline, dynamic state, push constants and draw then go the same way as a GL draw's.
struct PacketFixed
{
    uint32_t blend, src, dst, srcA, dstA, eq, eqA, fConst;
    std::array<float, 4> konst;
    uint32_t colorMask, cull, cullFace, frontFace, depthTest, depthWrite, depthFunc;
    uint32_t stencilTest, sfunc, sref, smask, swrite, sfail, szfail, szpass, sbfail, sbzfail, sbzpass;
    uint32_t keep;   // (cane::draw::kKeep*: fields left as they were)
};
struct PacketState
{
    uint32_t fixed = 0;
    GLuint program = 0;
    uint32_t cconst = 0, csampler = 0;
    struct { uint32_t offset, bits; } aconst[10]{};
    struct { uint8_t binding, unit; } asampler[4]{};   // (sampler uniforms: their units)
    uint16_t spec = 0;
};
std::vector<PacketFixed> g_packetFixed(1);
const Shaders::Uniform* UniformAt(const Program& p, GLint location);
std::vector<PacketState> g_packetStates(1);

GLenum GlFactor(uint8_t f)
{
    using namespace cane::draw;
    switch (f)
    {
    case kFactorZero: return GL_ZERO;
    case kFactorOne: return GL_ONE;
    case kFactorSrcAlpha: return GL_SRC_ALPHA;
    case kFactorOneMinusSrcAlpha: return GL_ONE_MINUS_SRC_ALPHA;
    case kFactorDstAlpha: return GL_DST_ALPHA;
    case kFactorOneMinusDstAlpha: return GL_ONE_MINUS_DST_ALPHA;
    case kFactorConstantAlpha: return GL_CONSTANT_ALPHA;
    case kFactorOneMinusConstantAlpha: return GL_ONE_MINUS_CONSTANT_ALPHA;
    case kFactorSrc1Alpha: return GL_SRC1_ALPHA;
    default: return GL_ONE_MINUS_SRC1_ALPHA;
    }
}
GLenum GlCompare(uint8_t c)
{
    static const GLenum k[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL, GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
    return k[c & 7];
}
GLenum GlStencilOp(uint8_t o)
{
    using namespace cane::draw;
    return o == kStencilZero ? GL_ZERO : o == kStencilReplace ? GL_REPLACE : o == kStencilIncrWrap ? GL_INCR_WRAP : o == kStencilDecrWrap ? GL_DECR_WRAP : GL_KEEP;
}

uint32_t PacketCreateState(const cane::draw::State& st)
{
    Program* p = g_programs.Get(st.program);
    if (!p || p->uniformBuffer || st.cconst > 10) return 0;
    PacketFixed f;
    std::memset(&f, 0, sizeof f);
    f.blend = st.blend;
    if (st.blend)
    {
        f.src = GlFactor(st.srcColor), f.dst = GlFactor(st.dstColor), f.srcA = GlFactor(st.srcAlpha), f.dstA = GlFactor(st.dstAlpha);
        f.eq = st.opColor == cane::draw::kBlendReverseSubtract ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD;
        f.eqA = st.opAlpha == cane::draw::kBlendReverseSubtract ? GL_FUNC_REVERSE_SUBTRACT : GL_FUNC_ADD;
        f.fConst = st.fBlendConstant;
        if (st.fBlendConstant) for (int i = 0; i < 4; ++i) f.konst[size_t(i)] = st.blendConstant[i];
    }
    f.colorMask = st.colorMask & 0xF;
    f.cull = st.cull ? 1 : 0;
    if (st.cull) f.cullFace = st.cull == 2 ? GL_FRONT : GL_BACK;
    if (st.cull || st.winding) f.frontFace = st.frontCCW ? GL_CCW : GL_CW;   // (0: left as it was)
    f.depthTest = st.depthTest, f.depthWrite = st.depthWrite, f.depthFunc = GlCompare(st.depthCompare);
    f.stencilTest = st.stencilTest;
    f.sfunc = GlCompare(st.stencilCompare), f.sref = st.stencilRef, f.smask = st.stencilCompareMask, f.swrite = st.stencilWriteMask;
    f.sfail = GlStencilOp(st.stencilFail), f.szfail = GlStencilOp(st.stencilDepthFail), f.szpass = GlStencilOp(st.stencilPass);
    f.sbfail = f.sfail, f.sbzfail = f.szfail, f.sbzpass = f.szpass;
    if (st.stencilBack)
        f.sbfail = GlStencilOp(st.stencilBackFail), f.sbzfail = GlStencilOp(st.stencilBackDepthFail), f.sbzpass = GlStencilOp(st.stencilBackPass);
    f.keep = st.keep;
    PacketState ps;
    for (size_t i = 1; i < g_packetFixed.size() && !ps.fixed; ++i)
        if (!std::memcmp(&g_packetFixed[i], &f, sizeof f)) ps.fixed = uint32_t(i);
    if (!ps.fixed) { ps.fixed = uint32_t(g_packetFixed.size()); g_packetFixed.push_back(f); }
    ps.program = st.program;
    for (unsigned i = 0; i < st.cconst; ++i)
    {
        const Shaders::Uniform* u = UniformAt(*p, st.aconst[i].location);
        if (u && u->type >= 32 && u->binding < kMaxBindings && st.aconst[i].bits < kMaxUnits && ps.csampler < 4)
        {
            ps.asampler[ps.csampler++] = {uint8_t(u->binding), uint8_t(st.aconst[i].bits)};
            continue;
        }
        if (!u || u->type >= 32 || uint32_t(u->offset) + 4 > p->constants.size()) return 0;
        ps.aconst[ps.cconst++] = {u->offset, st.aconst[i].bits};
    }
    // (per-state specialization: the constants the program reads as SPEC uniforms)
    if (const uint32_t mask = p->info->specMask & g_specAllowed)
    {
        SpecData d;
        for (uint32_t i = 0; i < ps.cconst; ++i)
            for (uint32_t j = 0; j < 8; ++j)
                if ((mask & (1u << j)) && p->specOffset[j] == ps.aconst[i].offset) d.mask |= 1u << j, d.value[j] = ps.aconst[i].bits;
        ps.spec = InternSpec(d);
    }
    g_packetStates.push_back(ps);
    return uint32_t(g_packetStates.size() - 1);
}

// s set to the packet's fixed values as the GL calls would set them (each change marking what it dirties)
void ApplyPacketFixed(uint32_t id)
{
    if (g_fixedPacket == id) return;
    const PacketFixed& f = g_packetFixed[id];
    if (s.blend != bool(f.blend)) { s.blend = f.blend; BlendChanged(); }
    if (f.blend && !(f.keep & cane::draw::kKeepBlendFactors))
    {
        if (s.blendEq != f.eq || s.blendEqAlpha != f.eqA) { s.blendEq = f.eq; s.blendEqAlpha = f.eqA; BlendChanged(); }
        if (s.blendSrc != f.src || s.blendDst != f.dst || s.blendSrcAlpha != f.srcA || s.blendDstAlpha != f.dstA)
        {
            s.blendSrc = f.src; s.blendDst = f.dst; s.blendSrcAlpha = f.srcA; s.blendDstAlpha = f.dstA;
            BlendChanged();
        }
        if (f.fConst && s.blendColor != f.konst) { s.blendColor = f.konst; b.blendConstDirty = true; }
    }
    if (s.cull != bool(f.cull)) { s.cull = f.cull; b.rasterDirty = true; }
    if (f.cull && s.cullFace != f.cullFace) { s.cullFace = f.cullFace; b.rasterDirty = true; }
    if (f.frontFace && s.frontFace != f.frontFace) { s.frontFace = f.frontFace; b.rasterDirty = true; }
    if (s.depthTest != bool(f.depthTest)) { s.depthTest = f.depthTest; b.depthDirty = true; }
    if (!(f.keep & cane::draw::kKeepDepthCompare) && s.depthFunc != f.depthFunc) { s.depthFunc = f.depthFunc; b.depthDirty = true; }
    if (!(f.keep & cane::draw::kKeepDepthWrite) && s.depthWrite != bool(f.depthWrite)) { s.depthWrite = f.depthWrite; b.depthDirty = true; }
    if (!(f.keep & cane::draw::kKeepColorMask) && s.colorMask != f.colorMask) { s.colorMask = f.colorMask; BlendChanged(); }
    if (s.stencilTest != bool(f.stencilTest)) { s.stencilTest = f.stencilTest; b.depthDirty = true; }
    if (!(f.keep & cane::draw::kKeepStencil))
    for (State::Face* fc : {&s.front, &s.back})
    {
        if (fc->func != f.sfunc || fc->ref != GLint(f.sref) || fc->mask != f.smask)
        {
            fc->func = f.sfunc; fc->ref = GLint(f.sref); fc->mask = f.smask;
            b.depthDirty = b.stencilDirty = true;
        }
        const bool back = fc == &s.back;
        const uint32_t fail = back ? f.sbfail : f.sfail, zfail = back ? f.sbzfail : f.szfail, zpass = back ? f.sbzpass : f.szpass;
        if (fc->fail != fail || fc->zfail != zfail || fc->zpass != zpass)
        {
            fc->fail = fail; fc->zfail = zfail; fc->zpass = zpass;
            b.depthDirty = true;
        }
        if (fc->writeMask != f.swrite) { fc->writeMask = f.swrite; b.stencilDirty = true; }
    }
    g_fixedPacket = id;
}

inline void PutConstant(Program& p, uint32_t at, uint32_t bits)
{
    uint8_t* const base = p.constants.data();
    uint32_t cur;
    std::memcpy(&cur, base + at, 4);
    if (cur == bits) return;
    std::memcpy(base + at, &bits, 4);
    p.dirtyLo = std::min(p.dirtyLo, at);
    p.dirtyHi = std::max(p.dirtyHi, at + 4);
}
// the packet's state into s and its program's constants; false: no such packet
bool ApplyPacket(uint32_t id, uint32_t vao, const cane::draw::Constant* aextra = nullptr, uint32_t cextra = 0)
{
    if (!id || id >= g_packetStates.size()) return false;
    const PacketState& ps = g_packetStates[id];
    s.program = ps.program;
    ApplyPacketFixed(ps.fixed);
    if (Program* p = g_programs.Get(ps.program))
    {
        for (uint32_t i = 0; i < ps.cconst; ++i) PutConstant(*p, ps.aconst[i].offset, ps.aconst[i].bits);
        for (uint32_t i = 0; i < ps.csampler; ++i) p->samplerUnit[ps.asampler[i].binding] = ps.asampler[i].unit;
        for (uint32_t i = 0; i < cextra; ++i)
            if (const Shaders::Uniform* u = UniformAt(*p, aextra[i].location); u && u->type < 32 && uint32_t(u->offset) + 4 <= p->constants.size())
                PutConstant(*p, u->offset, aextra[i].bits);
    }
    if (vao && !g_arrays.Get(vao)) g_arrays.Ensure(vao);
    s.vertexArray = vao;
    return true;
}
void PacketDrawIndexed(uint32_t id, uint32_t vao, uint32_t count, uint32_t firstIndex, int32_t baseVertex, uint32_t baseInstance,
                       const cane::draw::Constant* aconst, uint32_t cconst)
{
    if (!ApplyPacket(id, vao, aconst, cconst)) return;
    g_drawSpec = g_packetStates[id].spec;   // (the packet's own draws only: PacketApplyState's GL-style draws use spec 0)
    DrawElementsInstancedBaseVertexBaseInstance(GL_TRIANGLES, GLsizei(count), GL_UNSIGNED_INT, reinterpret_cast<const void*>(size_t(firstIndex) * 4), 1,
                                                baseVertex, baseInstance);
    g_drawSpec = 0;
}
void PacketDrawIndirect(uint32_t id, uint32_t vao, uint32_t indirectBuffer, size_t offset, uint32_t count)
{
    if (!ApplyPacket(id, vao)) return;
    g_drawSpec = g_packetStates[id].spec;
    s.indirectBuffer = indirectBuffer;
    MultiDrawElementsIndirect(GL_TRIANGLES, GL_UNSIGNED_INT, reinterpret_cast<const void*>(offset), GLsizei(count), 0);
    g_drawSpec = 0;
}
void PacketDrawArrays(uint32_t id, uint32_t vao, uint32_t first, uint32_t count, const cane::draw::Constant* aconst, uint32_t cconst)
{
    if (!ApplyPacket(id, vao, aconst, cconst)) return;
    g_drawSpec = g_packetStates[id].spec;
    DrawArraysInstancedBaseInstance(GL_TRIANGLES, GLint(first), GLsizei(count), 1, 0);
    g_drawSpec = 0;
}
void PacketApplyState(uint32_t id) { ApplyPacket(id, s.vertexArray); }
void PacketSetConstant(uint32_t program, int32_t location, const void* data, uint32_t bytes)
{
    Program* p = g_programs.Get(program);
    const Shaders::Uniform* u = p ? UniformAt(*p, location) : nullptr;
    if (!u || !data) return;
    if (u->type >= 32)
    {
        // (a sampler uniform: its unit, as glProgramUniform1i)
        int32_t unit;
        std::memcpy(&unit, data, 4);
        if (u->binding < kMaxBindings && unit >= 0 && unit < int32_t(kMaxUnits)) p->samplerUnit[u->binding] = uint8_t(unit);
        return;
    }
    bytes = std::min({bytes, uint32_t(u->count) * u->stride + 16u, uint32_t(p->constants.size()) - std::min<uint32_t>(u->offset, uint32_t(p->constants.size()))});
    const auto* src = static_cast<const uint8_t*>(data);
    for (uint32_t at = 0; at + 4 <= bytes; at += 4)
    {
        uint32_t bits;
        std::memcpy(&bits, src + at, 4);
        PutConstant(*p, u->offset + at, bits);
    }
}
void APIENTRY Clear(GLbitfield mask);
void APIENTRY BlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter);
void PacketBeginPass(const cane::draw::Pass& pass)
{
    BindFramebuffer(GL_FRAMEBUFFER, pass.framebuffer);
    Viewport(pass.viewport[0], pass.viewport[1], pass.viewport[2], pass.viewport[3]);
    if (!pass.clear) return;
    g_fixedPacket = 0;
    if (s.colorMask != 0xF) { s.colorMask = 0xF; BlendChanged(); }
    if (!s.depthWrite) { s.depthWrite = true; b.depthDirty = true; }
    s.front.writeMask = s.back.writeMask = 0xFF;
    b.stencilDirty = true;
    s.clearColor = {pass.color[0], pass.color[1], pass.color[2], pass.color[3]};
    s.clearDepth = std::clamp(pass.depth, 0.0f, 1.0f);
    s.clearStencil = pass.stencil;
    Clear(((pass.clear & cane::draw::kClearColor) ? GL_COLOR_BUFFER_BIT : 0) | ((pass.clear & cane::draw::kClearDepth) ? GL_DEPTH_BUFFER_BIT : 0) |
          ((pass.clear & cane::draw::kClearStencil) ? GL_STENCIL_BUFFER_BIT : 0));
    if (pass.cullOff) SetCapability(GL_CULL_FACE, false);
}
// (the masks and the scissor as glColorMask / glDepthMask / glStencilMask / glEnable(GL_SCISSOR_TEST) + glScissor set them;
// cane::draw::kMaskKept: left)
void PacketMasks(uint8_t colorMask, uint8_t depthWrite, uint8_t stencilWriteMask, uint8_t scissor, const int32_t* rect, bool stencilKept = false)
{
    constexpr uint8_t kept = cane::draw::kMaskKept;
    if (colorMask != kept && s.colorMask != (colorMask & 0xFu)) { s.colorMask = colorMask & 0xFu; BlendChanged(); g_fixedPacket = 0; }
    if (depthWrite != kept && s.depthWrite != bool(depthWrite)) { s.depthWrite = depthWrite; b.depthDirty = true; g_fixedPacket = 0; }
    if (!stencilKept && (s.front.writeMask != stencilWriteMask || s.back.writeMask != stencilWriteMask))
    {
        s.front.writeMask = s.back.writeMask = stencilWriteMask;
        b.stencilDirty = true;
        g_fixedPacket = 0;
    }
    if (scissor == kept) return;
    if (scissor) Scissor(rect[0], rect[1], rect[2], rect[3]);
    SetCapability(GL_SCISSOR_TEST, scissor != 0);
}
void PacketClear(const cane::draw::Clear& c)
{
    PacketMasks(c.colorMask, c.depthWrite, c.stencilWriteMask, c.scissor, c.rect, c.stencilMaskKept);
    GLbitfield mask = 0;
    if (c.clear & cane::draw::kClearColor) { s.clearColor = {c.color[0], c.color[1], c.color[2], c.color[3]}; mask |= GL_COLOR_BUFFER_BIT; }
    if (c.clear & cane::draw::kClearDepth) { s.clearDepth = std::clamp(c.depth, 0.0f, 1.0f); mask |= GL_DEPTH_BUFFER_BIT; }
    if (c.clear & cane::draw::kClearStencil) { s.clearStencil = c.stencil; mask |= GL_STENCIL_BUFFER_BIT; }
    if (mask) Clear(mask);
    PacketMasks(c.colorMaskAfter, c.depthWriteAfter, c.stencilWriteMaskAfter, c.scissorAfter, c.rect, c.stencilMaskKept);
}
void PacketCopy(uint32_t src, uint32_t dst, const int32_t rect[4], int32_t dstX, int32_t dstY, uint32_t framebufferAfter)
{
    BindFramebuffer(GL_READ_FRAMEBUFFER, src);
    BindFramebuffer(GL_DRAW_FRAMEBUFFER, dst);
    BlitFramebuffer(rect[0], rect[1], rect[0] + rect[2], rect[1] + rect[3], dstX, dstY, dstX + rect[2], dstY + rect[3], GL_COLOR_BUFFER_BIT, GL_NEAREST);
    BindFramebuffer(GL_FRAMEBUFFER, framebufferAfter);
}
void PacketScissor(const int32_t* rect)
{
    if (rect) Scissor(rect[0], rect[1], rect[2], rect[3]);
    SetCapability(GL_SCISSOR_TEST, rect != nullptr);
}
void PacketBindTexture(uint32_t unit, uint32_t texture, uint8_t array, uint32_t sampler)
{
    if (unit >= kMaxUnits) return;
    if (texture) g_textures.Ensure(texture);
    s.units[unit][array ? 1 : 0] = texture;
    if (sampler != cane::draw::kSamplerKept)
    {
        if (sampler) g_samplers.Ensure(sampler);
        s.samplerUnits[unit] = sampler;
    }
    s.activeUnit = 0;
}
void PacketBindStorage(uint32_t binding, uint32_t buffer, size_t offset, size_t size)
{
    if (size) BindBufferRange(GL_SHADER_STORAGE_BUFFER, binding, buffer, GLintptr(offset), GLsizeiptr(size));
    else BindBufferBase(GL_SHADER_STORAGE_BUFFER, binding, buffer);
}
void* PacketMapStream(uint32_t name, size_t size)
{
    Buffer* buf = name ? g_buffers.Get(name) : nullptr;
    if (!buf || !size || size > 0x7FFFFFFF || buf->keepShadow) return nullptr;   // (a CPU shadow kept: the GL calls)
    Respecify(name, *buf, uint32_t(size), GL_STREAM_DRAW);
    ++buf->version;
    return buf->cpu;
}
// (BufferSubData with the caller writing the bytes: a buffer keeping a CPU shadow takes the GL call)
void* PacketMapRange(uint32_t name, size_t offset, size_t size)
{
    Buffer* buf = name ? g_buffers.Get(name) : nullptr;
    if (!buf || !size || buf->keepShadow || uint64_t(offset) + uint64_t(size) > buf->size) return nullptr;
    if (!g_recording) BeginFrame();
    ++buf->version;
    return SubDataTarget(name, *buf, GLintptr(offset), GLsizeiptr(size));
}
// (copyLayer: the source layer attached to a framebuffer of the layer's own, one per layer, as the GL calls attach it)
void PacketCopyLayer(uint32_t texture, uint32_t layer, uint32_t dst, const int32_t rect[4], int32_t dstX, int32_t dstY, uint32_t framebufferAfter)
{
    static std::vector<GLuint> s_afbo;
    if (layer >= s_afbo.size()) s_afbo.resize(layer + 1, 0);
    if (!s_afbo[layer]) s_afbo[layer] = g_framebuffers.Gen();
    BindFramebuffer(GL_READ_FRAMEBUFFER, s_afbo[layer]);
    FramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 0, GLint(layer));
    PacketCopy(s_afbo[layer], dst, rect, dstX, dstY, framebufferAfter);
}
void PacketBindTarget(uint32_t framebuffer) { BindFramebuffer(GL_FRAMEBUFFER, framebuffer); }
void PacketDrawArraysMode(uint32_t id, uint32_t vao, uint8_t primitive, uint32_t first, uint32_t count, float lineWidth,
                          const cane::draw::Constant* aconst, uint32_t cconst)
{
    if (!ApplyPacket(id, vao, aconst, cconst)) return;
    if (lineWidth > 0.0f) LineWidth(lineWidth);
    if (!count) return;   // (GL draws nothing: only the state, constants and vertex array)
    using namespace cane::draw;
    const GLenum mode = primitive == kLines ? GL_LINES : primitive == kTriangleStrip ? GL_TRIANGLE_STRIP : GL_TRIANGLES;
    g_drawSpec = g_packetStates[id].spec;
    DrawArraysInstancedBaseInstance(mode, GLint(first), GLsizei(count), 1, 0);
    g_drawSpec = 0;
}
// (as glTexParameteri sets them on the texture; the sampler slot looked up again only when a value changes)
void PacketTextureParams(uint32_t texture, uint8_t wrapS, uint8_t wrapT, uint8_t minFilter, uint8_t magFilter)
{
    Texture* t = texture ? g_textures.Ensure(texture) : nullptr;
    if (!t) return;
    static const GLenum kWrap[3] = {GL_REPEAT, GL_CLAMP_TO_EDGE, GL_MIRRORED_REPEAT};
    static const GLenum kFilter[6] = {GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR,
                                      GL_LINEAR_MIPMAP_LINEAR};
    const GLenum ws = kWrap[wrapS % 3], wt = kWrap[wrapT % 3], mn = kFilter[minFilter % 6], mg = kFilter[magFilter % 6];
    SamplerParams& p = t->params;
    if (p.wrapS != ws || p.wrapT != wt || p.minFilter != mn || p.magFilter != mg)
    {
        p.wrapS = ws, p.wrapT = wt, p.minFilter = mn, p.magFilter = mg;
        t->samplerSlot = -1;
    }
}
void PacketBlit(uint32_t src, uint32_t dst, const int32_t sr[4], const int32_t dr[4], uint8_t linear, uint32_t framebufferAfter)
{
    BindFramebuffer(GL_READ_FRAMEBUFFER, src);
    BindFramebuffer(GL_DRAW_FRAMEBUFFER, dst);
    BlitFramebuffer(sr[0], sr[1], sr[0] + sr[2], sr[1] + sr[3], dr[0], dr[1], dr[0] + dr[2], dr[1] + dr[3], GL_COLOR_BUFFER_BIT,
                    linear ? GL_LINEAR : GL_NEAREST);
    BindFramebuffer(GL_FRAMEBUFFER, framebufferAfter);
}
void PacketInvalidate(uint32_t framebuffer, uint8_t mask)
{
    BindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    GLenum a[2];
    GLsizei n = 0;
    if (mask & cane::draw::kClearColor) a[n++] = GL_COLOR_ATTACHMENT0;
    if (mask & (cane::draw::kClearDepth | cane::draw::kClearStencil)) a[n++] = GL_DEPTH_STENCIL_ATTACHMENT;
    if (n) InvalidateFramebuffer(GL_FRAMEBUFFER, n, a);
}
// Resources (the texture entries: no binding point changes; the same objects as the GL-style calls make)
uint32_t PacketCreateTexture(const cane::draw::TextureDesc& d)
{
    ResTime rt;
    if (!d.width || !d.height || (d.storage == 0 && d.layers > 0)) return 0;
    if (g_resLogOn) ++g_resFrame.textures;
    if (!g_recording) BeginFrame();
    const GLuint name = g_textures.Gen();
    Texture* t = g_textures.Ensure(name);
    if (!t) return 0;
    const bool array = d.layers > 0;
    const uint32_t layers = array ? d.layers : 1u;
    const unsigned levels = d.storage ? std::min<unsigned>(std::max(1u, d.levels), FullLevels(d.width, d.height)) : 1u;
    // (every texel written by the upload below: no clear first; with more levels the others are still GL's zeros)
    Replace(*t, MakeImage(d.width, d.height, layers, levels, 1, VK_FORMAT_R8G8B8A8_UNORM, array, true, 0, !(d.pixels && levels == 1)));
    t->image->immutable = d.storage != 0;
    static const GLenum kWrap[3] = {GL_REPEAT, GL_CLAMP_TO_EDGE, GL_MIRRORED_REPEAT};
    static const GLenum kFilter[6] = {GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR,
                                      GL_LINEAR_MIPMAP_LINEAR};
    t->params.wrapS = kWrap[d.wrapS % 3], t->params.wrapT = kWrap[d.wrapT % 3];
    t->params.minFilter = kFilter[d.minFilter % 6], t->params.magFilter = kFilter[d.magFilter % 6];
    t->params.maxLevel = d.maxLevel;
    t->samplerSlot = -1;
    if (d.pixels)
        for (uint32_t i = 0; i < layers; ++i)
            UploadPixelsPitch(*t->image, 0, i, 0, 0, d.width, d.height, GL_RGBA, GL_UNSIGNED_BYTE,
                              static_cast<const uint8_t*>(d.pixels) + size_t(d.width) * d.height * 4 * i, size_t(d.width) * 4);
    return name;
}
void PacketUploadTexture(uint32_t texture, uint32_t level, uint32_t layer, int32_t x, int32_t y, uint32_t width, uint32_t height, const void* rgba)
{
    Texture* t = g_textures.Get(texture);
    if (!t || !t->image) { SetError(GL_INVALID_OPERATION); return; }
    UploadPixelsPitch(*t->image, level, layer, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba, size_t(width) * 4);
}
void PacketGenerateMipmaps(uint32_t texture)
{
    ResTime rt;
    GenerateMips(g_textures.Get(texture));
}
void PacketDeleteTextures(uint32_t count, const uint32_t* textures) { DeleteTextures(GLsizei(count), textures); }

// (the last leftovers: the program unbound, the screenshot's read-back, the FSR timer)
void APIENTRY UseProgram(GLuint program);
void APIENTRY ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels);
void APIENTRY GenQueries(GLsizei n, GLuint* names);
void APIENTRY BeginQuery(GLenum target, GLuint name);
void APIENTRY EndQuery(GLenum);
void APIENTRY GetQueryObjectui64v(GLuint name, GLenum pname, GLuint64* value);
void PacketUseProgram(uint32_t program) { UseProgram(program); }
void PacketReadPixels(uint32_t framebuffer, const int32_t r[4], void* rgba, uint32_t framebufferAfter)
{
    BindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    const GLint pack = s.packAlignment;
    s.packAlignment = 1;
    ReadPixels(r[0], r[1], r[2], r[3], GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    s.packAlignment = pack;
    BindFramebuffer(GL_FRAMEBUFFER, framebufferAfter);
}
uint32_t PacketCreateTimer() { GLuint q = 0; GenQueries(1, &q); return q; }
void PacketBeginTimer(uint32_t timer) { BeginQuery(GL_TIME_ELAPSED, timer); }
void PacketEndTimer() { EndQuery(GL_TIME_ELAPSED); }
uint64_t PacketTimerResult(uint32_t timer) { GLuint64 ns = 0; GetQueryObjectui64v(timer, GL_QUERY_RESULT, &ns); return ns; }

// Buffers (no binding point changes: the same Buffer objects, storage and transfer rules as the GL-style calls)
GLenum UsageOf(uint8_t usage)
{
    switch (usage & 0x0F)
    {
    case cane::draw::kBufferDynamic: return GL_DYNAMIC_DRAW;
    case cane::draw::kBufferStream: return GL_STREAM_DRAW;
    default: return GL_STATIC_DRAW;
    }
}
uint32_t PacketCreateBuffer(size_t size, const void* data, uint8_t usage)
{
    if (size > 0x7FFFFFFF) return 0;
    const GLuint name = g_buffers.Gen();
    if (size) BufferDataOn(name, GLsizeiptr(size), data, UsageOf(usage), (usage & cane::draw::kBufferIndirect) != 0);
    return name;
}
void PacketBufferData(uint32_t buffer, size_t size, const void* data, uint8_t usage)
{
    if (buffer) g_buffers.Ensure(buffer);
    BufferDataOn(buffer, GLsizeiptr(size), data, UsageOf(usage), (usage & cane::draw::kBufferIndirect) != 0);
}
void PacketBufferSubData(uint32_t buffer, size_t offset, size_t size, const void* data)
{
    Buffer* buf = buffer ? g_buffers.Get(buffer) : nullptr;
    BufferSubDataOn(buffer, GLintptr(offset), GLsizeiptr(size), data, buf && buf->keepShadow);
}
void PacketDeleteBuffers(uint32_t count, const uint32_t* buffers) { DeleteBuffers(GLsizei(count), buffers); }
// Vertex arrays (no binding point changes: the same VertexArray / Attrib fields SetAttrib writes, so the pipeline key's
// vertex layout, LayoutFor, and the prewarm manifest's keys are the same)
uint32_t PacketCreateVertexArray(const cane::draw::VertexAttrib* aattrib, uint32_t cattrib, uint32_t elements)
{
    const GLuint name = g_arrays.Gen();
    VertexArray* vao = g_arrays.Ensure(name);
    if (!vao) return 0;
    if (elements) g_buffers.Ensure(elements);
    vao->elements = elements;
    static const GLenum kType[3] = {GL_FLOAT, GL_UNSIGNED_BYTE, GL_UNSIGNED_INT};
    for (uint32_t i = 0; i < cattrib; ++i)
    {
        const cane::draw::VertexAttrib& d = aattrib[i];
        if (d.index >= kGenericBinding || d.components < 1 || d.components > 4) { SetError(GL_INVALID_VALUE); continue; }
        if (d.buffer) g_buffers.Ensure(d.buffer);
        Attrib& a = vao->attribs[d.index];
        a.buffer = d.buffer;
        a.components = d.components;
        a.type = kType[d.type % 3];
        a.normalized = d.normalized != 0;
        a.integer = d.integer != 0;
        a.offset = d.offset;
        a.stride = d.stride ? d.stride : uint32_t(d.components) * ComponentBytes(a.type);
        a.divisor = d.divisor;
        a.enabled = true;
    }
    ++vao->version;
    return name;
}
void PacketDeleteVertexArrays(uint32_t count, const uint32_t* arrays) { DeleteVertexArrays(GLsizei(count), arrays); }
// Framebuffers and renderbuffers (no binding point changes: the same Framebuffer / Renderbuffer objects and images as the
// GL-style calls, so ForgetTarget / DetachEverywhere / msrtss / multiview / the transient hint work as before)
uint32_t PacketCreateRenderbuffer(const cane::draw::RenderbufferDesc& d)
{
    if (!d.width || !d.height) return 0;
    const GLuint name = g_renderbuffers.Gen();
    Renderbuffer* rb = g_renderbuffers.Ensure(name);
    static const GLenum kFormat[3] = {GL_RGBA8, GL_DEPTH32F_STENCIL8, GL_DEPTH24_STENCIL8};
    g_nextRenderbufferLayers = std::max<uint32_t>(1u, d.layers);
    RenderbufferStorageOn(rb, GLsizei(std::max<uint8_t>(d.samples, 1)), kFormat[d.format % 3], GLsizei(d.width), GLsizei(d.height));
    g_nextRenderbufferLayers = 1;
    if (d.transient) SetRenderbufferTransient(name, true);
    return name;
}
void PacketDeleteRenderbuffers(uint32_t count, const uint32_t* names) { DeleteRenderbuffers(GLsizei(count), names); }
Attachment AttachmentOf(const cane::draw::Attachment& a)
{
    if (a.object) { if (a.renderbuffer) g_renderbuffers.Ensure(a.object); else g_textures.Ensure(a.object); }
    return Attachment{a.object, a.renderbuffer != 0, a.layer, a.level, uint8_t(std::min<uint8_t>(a.views, 2))};
}
void PacketAttach(uint32_t framebuffer, uint8_t mask, const cane::draw::Attachment& a)
{
    Framebuffer* f = framebuffer ? g_framebuffers.Get(framebuffer) : nullptr;
    if (!f) { SetError(GL_INVALID_OPERATION); return; }
    if (mask & cane::draw::kClearColor) f->color = AttachmentOf(a);
    if (mask & (cane::draw::kClearDepth | cane::draw::kClearStencil)) f->depth = AttachmentOf(a);
    b.targetDirty = true;   // (as Attach: the target looked at again at the next draw)
}
uint32_t PacketCreateFramebuffer(const cane::draw::Attachment& color, const cane::draw::Attachment& depth)
{
    const GLuint name = g_framebuffers.Gen();
    Framebuffer* f = g_framebuffers.Ensure(name);
    if (!f) return 0;
    f->color = AttachmentOf(color);
    f->depth = AttachmentOf(depth);
    return name;
}
void PacketDeleteFramebuffers(uint32_t count, const uint32_t* names) { DeleteFramebuffers(GLsizei(count), names); }
// Samplers (the same Sampler objects as GenSamplers / SamplerParameter*: the sampler cache and the handles' slots unchanged)
uint32_t PacketCreateSampler(const cane::draw::SamplerDesc& d)
{
    const GLuint name = g_samplers.Gen();
    Sampler* sm = g_samplers.Ensure(name);
    if (!sm) return 0;
    static const GLenum kWrap[3] = {GL_REPEAT, GL_CLAMP_TO_EDGE, GL_MIRRORED_REPEAT};
    static const GLenum kFilter[6] = {GL_NEAREST, GL_LINEAR, GL_NEAREST_MIPMAP_NEAREST, GL_LINEAR_MIPMAP_NEAREST, GL_NEAREST_MIPMAP_LINEAR,
                                      GL_LINEAR_MIPMAP_LINEAR};
    sm->params.wrapS = kWrap[d.wrapS % 3], sm->params.wrapT = kWrap[d.wrapT % 3];
    sm->params.minFilter = kFilter[d.minFilter % 6], sm->params.magFilter = kFilter[d.magFilter % 6];
    // (as the GL calls: glSamplerParameterf with the wanted value clamped to GL_MAX_TEXTURE_MAX_ANISOTROPY, when above 1)
    const float aniso = std::min(d.anisotropy, g_feat.anisotropy ? g_feat.anisotropyMax : 1.0f);
    if (aniso > 1.0f) sm->params.anisotropy = aniso;
    sm->samplerSlot = -1;
    return name;
}
void PacketDeleteSamplers(uint32_t count, const uint32_t* names) { DeleteSamplers(GLsizei(count), names); }
uint8_t PacketFramebufferComplete(uint32_t framebuffer)
{
    Framebuffer* f = framebuffer ? g_framebuffers.Get(framebuffer) : nullptr;
    if (!f) return 0;
    Image* color = AttachmentImage(f->color);
    Image* depth = AttachmentImage(f->depth);
    if (!color && !depth) return 0;
    if ((f->color.object && !color) || (f->depth.object && !depth)) return 0;
    return !(color && depth && color->samples != depth->samples);
}

const cane::draw::Packets g_packets{PacketCreateState, PacketDrawIndirect, PacketDrawIndexed, PacketBeginPass, PacketApplyState, PacketDrawArrays,
                                    PacketSetConstant, PacketClear, PacketCopy, PacketScissor, PacketBindTexture, PacketBindStorage,
                                    PacketMapStream, PacketMapRange, PacketCopyLayer, PacketBindTarget, PacketDrawArraysMode,
                                    PacketTextureParams, PacketBlit, PacketInvalidate,
                                    PacketCreateTexture, PacketUploadTexture, PacketGenerateMipmaps, PacketDeleteTextures,
                                    PacketUseProgram, PacketReadPixels, PacketCreateTimer, PacketBeginTimer, PacketEndTimer,
                                    PacketTimerResult, PacketCreateBuffer, PacketBufferData, PacketBufferSubData, PacketDeleteBuffers,
                                    PacketCreateVertexArray, PacketDeleteVertexArrays, PacketCreateRenderbuffer, PacketDeleteRenderbuffers,
                                    PacketCreateFramebuffer, PacketAttach, PacketDeleteFramebuffers, PacketFramebufferComplete,
                                    PacketCreateSampler, PacketDeleteSamplers};

// ------------------------------------------------------------------------------------------------ clears and blits
GLuint g_clearProgram = 0;
// A clear the attachment clears cannot do (a partial colour or stencil write mask, which GL's clears obey): one
// triangle over the scissor rectangle with the masks as the pipeline's and dynamic state.
void ClearMasked(bool color, bool depth, bool stencil)
{
    Program* p = g_programs.Get(g_clearProgram);
    if (!p) return;
    EnsureRendering();
    if (color) g_instWroteColor = true;
    if (depth || stencil) g_instWroteDepth = true;
    VkCommandBuffer cmd = g_main;
    PipelineKey k;
    k.program = p->serial;
    k.layout = LayoutFor(*g_arrays.Get(0), *p);
    k.topology = uint8_t(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    k.colorFormat = b.colorFormat;
    k.depthFormat = b.depthFormat;
    k.samples = uint8_t(b.samples);
    k.pad[0] = b.msrtss ? 1 : 0;   // (msrtss: the pipeline carries VkMultisampledRenderToSingleSampledInfoEXT)
    k.pad[1] = b.classic ? 1 : 0;  // (classic fetch: made for the render pass object)
    k.pad[2] = uint8_t((b.fdm ? 1 : 0) | (b.multiview ? 2 : 0));   // (drawn with a fragment density map; two views)
    k.colorMask = g_feat.eds3 ? 0xF : uint8_t(color ? s.colorMask : 0);
    const VkPipeline pipeline = PipelineFor(k, *p);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    if (g_feat.eds3 && b.hasColor)
    {
        const VkBool32 off = VK_FALSE;
        vkCmdSetColorBlendEnableEXT(cmd, 0, 1, &off);
        const VkColorComponentFlags mask = color ? s.colorMask : 0;
        vkCmdSetColorWriteMaskEXT(cmd, 0, 1, &mask);
    }
    const VkViewport vp{0.0f, 0.0f, float(b.width), float(b.height), 0.0f, 1.0f};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    const VkRect2D sc = ScissorRect();
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
    vkCmdSetDepthTestEnable(cmd, depth);
    vkCmdSetDepthWriteEnable(cmd, depth);
    vkCmdSetDepthCompareOp(cmd, VK_COMPARE_OP_ALWAYS);
    if (g_feat.depthBounds) vkCmdSetDepthBoundsTestEnable(cmd, VK_FALSE);
    vkCmdSetStencilTestEnable(cmd, stencil);
    vkCmdSetStencilOp(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_STENCIL_OP_REPLACE, VK_COMPARE_OP_ALWAYS);
    vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, s.front.writeMask & 0xFF);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, uint32_t(s.clearStencil) & 0xFF);
    vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0xFF);
    // (the program's push constants: its uniforms "depth" and "color")
    float data[8] = {s.clearDepth, 0, 0, 0, s.clearColor[0], s.clearColor[1], s.clearColor[2], s.clearColor[3]};
    uint16_t offDepth = 0, offColor = 16;
    for (uint32_t i = 0; i < p->info->uniformCount; ++i)
    {
        if (!std::strcmp(p->info->uniforms[i].name, "depth")) offDepth = p->info->uniforms[i].offset;
        if (!std::strcmp(p->info->uniforms[i].name, "color")) offColor = p->info->uniforms[i].offset;
    }
    std::vector<uint8_t> push(p->info->pushSize, 0);
    std::memcpy(push.data() + offDepth, &data[0], 4);
    std::memcpy(push.data() + offColor, &data[4], 16);
    vkCmdPushConstants(cmd, g_pipelineLayout, g_stages, 0, uint32_t(push.size()), push.data());
    g_pushValid = 0;
    vkCmdDraw(cmd, 3, 1, 0, 0);
    // (the GL state's own again at the next draw)
    b.pipeline = VK_NULL_HANDLE;
    b.pipelineDirty = b.viewportDirty = b.rasterDirty = b.depthDirty = b.stencilDirty = b.blendDirty = true;
    b.program = ~0u;
}
void APIENTRY Clear(GLbitfield mask)
{
    if (!g_recording) BeginFrame();
    FlushCopies();
    ApplyTarget();
    const bool color = (mask & GL_COLOR_BUFFER_BIT) && b.hasColor && s.colorMask;
    const bool depth = (mask & GL_DEPTH_BUFFER_BIT) && s.depthWrite && b.hasDepth;
    const bool stencil = (mask & GL_STENCIL_BUFFER_BIT) && (s.front.writeMask & 0xFF) && b.hasDepth;
    if (!color && !depth && !stencil) return;
    const VkRect2D rect = ScissorRect();
    if (!rect.extent.width || !rect.extent.height) return;
    const bool full = rect.offset.x == 0 && rect.offset.y == 0 && rect.extent.width == b.width && rect.extent.height == b.height;
    const bool masked = (color && s.colorMask != 0xF) || (stencil && (s.front.writeMask & 0xFF) != 0xFF);
    VkClearValue cv{};
    std::memcpy(cv.color.float32, s.clearColor.data(), 16);
    for (float& f : cv.color.float32) f = std::clamp(f, 0.0f, 1.0f);
    VkClearValue dv{};
    dv.depthStencil = {s.clearDepth, uint32_t(s.clearStencil) & 0xFF};
    if (masked) { ClearMasked(color, depth, stencil); return; }
    if (g_pendingBegin && full)
    {
        // (before the instance's first draw: its load operations)
        if (color) { g_pendingClear.color = true; g_pendingClear.colorValue = cv; }
        if (depth) { g_pendingClear.depth = true; g_pendingClear.depthValue.depthStencil.depth = s.clearDepth; }
        if (stencil) { g_pendingClear.stencil = true; g_pendingClear.depthValue.depthStencil.stencil = uint32_t(s.clearStencil) & 0xFF; }
        return;
    }
    EnsureRendering();
    if (g_invalidatedColor || g_invalidatedDepth) RestoreInvalidatedStores();
    VkClearAttachment atts[2];
    uint32_t n = 0;
    if (color) atts[n++] = {VK_IMAGE_ASPECT_COLOR_BIT, 0, cv};
    if (depth || stencil)
        atts[n++] = {VkImageAspectFlags((depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)), 0, dv};
    const VkClearRect cr{rect, 0, 1};
    if (color) g_instWroteColor = true;
    if (depth || stencil) g_instWroteDepth = true;
    vkCmdClearAttachments(g_main, n, atts, 1, &cr);
}
void APIENTRY ClearBufferfv(GLenum buffer, GLint, const GLfloat* value)
{
    if (buffer != GL_COLOR || !value) return;
    const auto saved = s.clearColor;
    s.clearColor = {value[0], value[1], value[2], value[3]};
    Clear(GL_COLOR_BUFFER_BIT);
    s.clearColor = saved;
}

// The colour image of a framebuffer for blits and reads (framebuffer 0: the window's picture).
Image* ColorImageOf(GLuint fbo, unsigned* level, unsigned* layer)
{
    *level = *layer = 0;
    if (!fbo) return g_windowImage.get();
    Framebuffer* f = g_framebuffers.Get(fbo);
    Image* im = f ? AttachmentImage(f->color) : nullptr;
    if (im) { *level = f->color.level; *layer = f->color.layer; }
    return im;
}
// (BlitFramebuffer) the last colour resolve recorded and its images' write counts after it
struct ResolveMemo
{
    uint64_t srcId = 0, dstId = 0;
    VkImage src = VK_NULL_HANDLE, dst = VK_NULL_HANDLE;
    uint64_t srcWrites = 0, dstWrites = 0;
    std::array<unsigned, 4> sub{};
    std::array<GLint, 6> rect{};
    bool operator==(const ResolveMemo& o) const
    {
        return srcId == o.srcId && dstId == o.dstId && src == o.src && dst == o.dst && srcWrites == o.srcWrites && dstWrites == o.dstWrites &&
               sub == o.sub && rect == o.rect;
    }
};
ResolveMemo g_lastResolve;
uint64_t g_resolvesSkipped = 0;
void TransferLayouts(Image& src, Image& dst)
{
    Barriers bar;
    if (&src != &dst)
    {
        bar.Image(src, src.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        bar.Image(dst, dst.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        src.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        dst.layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }
    bar.Flush(g_main);
    src.useSegment = dst.useSegment = g_segment;
    MarkDirty(src);
    MarkDirty(dst);
}
void APIENTRY BlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter)
{
    if (!(mask & GL_COLOR_BUFFER_BIT)) return;   // (depth / stencil blits are not supported)
    if (!g_recording) BeginFrame();
    unsigned sl, sla, dl, dla;
    Image* src = ColorImageOf(s.readFramebuffer, &sl, &sla);
    Image* dst = ColorImageOf(s.drawFramebuffer, &dl, &dla);
    if (!src || !dst || src == dst) return;
    const GLint sw = GLint(std::max(1u, src->width >> sl)), sh = GLint(std::max(1u, src->height >> sl));
    const GLint dw = GLint(std::max(1u, dst->width >> dl)), dh = GLint(std::max(1u, dst->height >> dl));
    bool flipX = false, flipY = false;
    if (sx1 < sx0) { std::swap(sx0, sx1); flipX = !flipX; }
    if (dx1 < dx0) { std::swap(dx0, dx1); flipX = !flipX; }
    if (sy1 < sy0) { std::swap(sy0, sy1); flipY = !flipY; }
    if (dy1 < dy0) { std::swap(dy0, dy1); flipY = !flipY; }
    sx0 = std::clamp(sx0, 0, sw); sx1 = std::clamp(sx1, 0, sw); sy0 = std::clamp(sy0, 0, sh); sy1 = std::clamp(sy1, 0, sh);
    dx0 = std::clamp(dx0, 0, dw); dx1 = std::clamp(dx1, 0, dw); dy0 = std::clamp(dy0, 0, dh); dy1 = std::clamp(dy1, 0, dh);
    if (sx1 <= sx0 || sy1 <= sy0 || dx1 <= dx0 || dy1 <= dy0) return;
    FlushCopies();
    EndRendering();
    TransferLayouts(*src, *dst);
    VkCommandBuffer cmd = g_main;
    const bool same = sx1 - sx0 == dx1 - dx0 && sy1 - sy0 == dy1 - dy0 && !flipX && !flipY;
    const uint64_t dstWritesBefore = dst->writes++;
    TimePassBegin();
    struct TimeXfer { uint32_t kind, w, h; ~TimeXfer() { TimePassEnd(kind, w, h); } } timeXfer{
        src->samples > 1 ? 4u : same && src->format == dst->format ? 8u : 16u, uint32_t(sx1 - sx0), uint32_t(sy1 - sy0)};
    if (src->samples > 1)
    {
        // a multisample resolve (the same rectangle size: GL has no scaled resolves). The same resolve again with neither
        // image written since (the game resolves the frame for the overlay capture and again at the present): its
        // result is already there, so only the layouts change (SLY2N_VKRESOLVEONCE=0: resolved every time).
        static const bool s_fOnceOff = std::getenv("SLY2N_VKRESOLVEONCE") && std::atoi(std::getenv("SLY2N_VKRESOLVEONCE")) == 0;
        const ResolveMemo memo{src->id, dst->id, src->image, dst->image, src->writes, dstWritesBefore, {sl, sla, dl, dla}, {sx0, sy0, sx1, sy1, dx0, dy0}};
        if (dst->samples == 1 && same && !s_fOnceOff && memo == g_lastResolve)
        {
            dst->writes = dstWritesBefore;   // (nothing written)
            ++g_resolvesSkipped;
            timeXfer.kind = 32;
            b.targetDirty = true;
            return;
        }
        if (dst->samples == 1 && same)
        {
            VkImageResolve r{};
            r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, sl, sla, 1};
            r.srcOffset = {sx0, sy0, 0};
            r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dl, dla, 1};
            r.dstOffset = {dx0, dy0, 0};
            r.extent = {uint32_t(sx1 - sx0), uint32_t(sy1 - sy0), 1};
            vkCmdResolveImage(cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
            g_lastResolve = memo;
            g_lastResolve.dstWrites = dst->writes;   // (this resolve's own write counted)
        }
    }
    else if (same && src->format == dst->format)
    {
        VkImageCopy r{};
        r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, sl, sla, 1};
        r.srcOffset = {sx0, sy0, 0};
        r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dl, dla, 1};
        r.dstOffset = {dx0, dy0, 0};
        r.extent = {uint32_t(sx1 - sx0), uint32_t(sy1 - sy0), 1};
        vkCmdCopyImage(cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    }
    else
    {
        VkImageBlit r{};
        r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, sl, sla, 1};
        r.srcOffsets[0] = {flipX ? sx1 : sx0, flipY ? sy1 : sy0, 0};
        r.srcOffsets[1] = {flipX ? sx0 : sx1, flipY ? sy0 : sy1, 1};
        r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dl, dla, 1};
        r.dstOffsets[0] = {dx0, dy0, 0};
        r.dstOffsets[1] = {dx1, dy1, 1};
        vkCmdBlitImage(cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r,
                       filter == GL_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    }
    b.targetDirty = true;
}
void APIENTRY FinishGL() { Finish(); }

Alloc g_readback;
void APIENTRY ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels)
{
    if (!pixels || width <= 0 || height <= 0) return;
    if (format != GL_RGBA || type != GL_UNSIGNED_BYTE) { std::memset(pixels, 0, size_t(width) * size_t(height) * (format == GL_RGBA ? 4 : 1)); return; }
    if (!g_recording) BeginFrame();
    unsigned level, layer;
    Image* im = ColorImageOf(s.readFramebuffer, &level, &layer);
    if (!im || im->samples > 1) return;
    const GLint w = GLint(std::max(1u, im->width >> level)), h = GLint(std::max(1u, im->height >> level));
    x = std::clamp(x, 0, w); y = std::clamp(y, 0, h);
    width = std::min<GLsizei>(width, w - x); height = std::min<GLsizei>(height, h - y);
    if (width <= 0 || height <= 0) return;
    const VkDeviceSize bytes = VkDeviceSize(width) * VkDeviceSize(height) * 4;
    if (!g_readback || g_readback.size < bytes)
    {
        if (g_readback) { Finish(); FreeAlloc(g_readback); }
        if (!g_readbackHeap->Allocate(Align(bytes, 1 << 20), 256, &g_readback)) Fatal("out of read-back memory");
    }
    FlushCopies();
    EndRendering();
    Barriers bar;
    bar.Image(*im, im->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    bar.Flush(g_main);
    im->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    im->useSegment = g_segment;
    MarkDirty(*im);
    VkBufferImageCopy r{};
    r.bufferOffset = g_readback.offset;
    r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, layer, 1};
    r.imageOffset = {x, y, 0};
    r.imageExtent = {uint32_t(width), uint32_t(height), 1};
    vkCmdCopyImageToBuffer(g_main, im->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_readback.Buffer(), 1, &r);
    bar.Memory(VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
    bar.Flush(g_main);
    b.targetDirty = true;
    Finish();
    const VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, nullptr, g_readback.block->memory, 0, VK_WHOLE_SIZE};
    vkInvalidateMappedMemoryRanges(g_device, 1, &range);
    const uint8_t* src = g_readback.Cpu();
    const size_t pitch = size_t(width) * 4;
    const size_t outPitch = Align(pitch, VkDeviceSize(std::max(1, s.packAlignment)));
    for (GLsizei row = 0; row < height; ++row)
        std::memcpy(static_cast<uint8_t*>(pixels) + size_t(row) * outPitch, src + size_t(row) * pitch, pitch);
}

// ------------------------------------------------------------------------------------------------ programs
const Shaders::Uniform* UniformAt(const Program& p, GLint location)
{
    if (location < 0 || uint32_t(location) >= p.info->uniformCount) return nullptr;
    return &p.info->uniforms[location];
}
GLint APIENTRY GetUniformLocation(GLuint program, const GLchar* name)
{
    Program* p = g_programs.Get(program);
    if (!p || !name) return -1;
    std::string key(name);
    if (key.size() > 3 && key.compare(key.size() - 3, 3, "[0]") == 0) key.resize(key.size() - 3);
    for (uint32_t i = 0; i < p->info->uniformCount; ++i)
        if (key == p->info->uniforms[i].name) return GLint(i);
    return -1;
}
void APIENTRY UseProgram(GLuint program)
{
    if (program && !g_programs.Get(program)) { SetError(GL_INVALID_OPERATION); return; }
    s.program = program;
}
void APIENTRY DeleteProgram(GLuint program)
{
    auto p = g_programs.Take(program);
    if (!p) return;
    // (its modules stay until the end: pipelines made from them may still be bound; a few kilobytes, once per program)
    for (VkShaderModule m : p->modules) if (m) g_retiredModules.push_back(m);
    for (VkShaderModule m : p->modulesMv) if (m) g_retiredModules.push_back(m);
    if (s.program == program) s.program = 0;
    if (b.program == program) b.program = ~0u;
}
// glUniform*: the value into the program's push constants (or a sampler's unit).
void WriteUniform(GLint location, const void* data, unsigned components, GLsizei count, unsigned matrix = 0, bool transpose = false)
{
    if (location < 0 || !data || count <= 0) return;
    Program* p = g_programs.Get(s.program);
    if (!p) { SetError(GL_INVALID_OPERATION); return; }
    const Shaders::Uniform* u = UniformAt(*p, location);
    if (!u) { SetError(GL_INVALID_OPERATION); return; }
    if (u->type >= 32)
    {
        const GLint unit = *static_cast<const GLint*>(data);
        if (unit >= 0 && unit < GLint(kMaxUnits) && u->binding < kMaxBindings) p->samplerUnit[u->binding] = uint8_t(unit);
        return;
    }
    const unsigned n = std::min<unsigned>(unsigned(count), u->count);
    uint8_t* base = p->constants.data() + u->offset;
    const auto* src = static_cast<const uint8_t*>(data);
    uint32_t lo = UINT32_MAX, hi = 0;
    auto put = [&](uint32_t at, const void* value, uint32_t bytes) {
        if (std::memcmp(base + at, value, bytes) == 0) return;
        std::memcpy(base + at, value, bytes);
        lo = std::min(lo, at);
        hi = std::max(hi, at + bytes);
    };
    for (unsigned i = 0; i < n; ++i)
    {
        const uint32_t at = i * u->stride;
        if (matrix)
        {
            const auto* m = reinterpret_cast<const float*>(src) + size_t(i) * matrix * matrix;
            for (unsigned col = 0; col < matrix; ++col)
            {
                float column[4];
                for (unsigned row = 0; row < matrix; ++row) column[row] = transpose ? m[row * matrix + col] : m[col * matrix + row];
                put(at + col * 16, column, matrix * 4);
            }
        }
        else put(at, src + size_t(i) * components * 4, components * 4);
    }
    if (lo < hi)
    {
        p->dirtyLo = std::min(p->dirtyLo, u->offset + lo);
        p->dirtyHi = std::max(p->dirtyHi, u->offset + hi);
    }
}
void APIENTRY Uniform1i(GLint l, GLint x) { WriteUniform(l, &x, 1, 1); }
void APIENTRY Uniform2i(GLint l, GLint x, GLint y) { const GLint v[]{x, y}; WriteUniform(l, v, 2, 1); }
void APIENTRY Uniform1iv(GLint l, GLsizei n, const GLint* v) { WriteUniform(l, v, 1, n); }
void APIENTRY Uniform1ui(GLint l, GLuint x) { WriteUniform(l, &x, 1, 1); }
void APIENTRY Uniform1f(GLint l, GLfloat x) { WriteUniform(l, &x, 1, 1); }
void APIENTRY Uniform2f(GLint l, GLfloat x, GLfloat y) { const GLfloat v[]{x, y}; WriteUniform(l, v, 2, 1); }
void APIENTRY Uniform3f(GLint l, GLfloat x, GLfloat y, GLfloat z) { const GLfloat v[]{x, y, z}; WriteUniform(l, v, 3, 1); }
void APIENTRY Uniform4f(GLint l, GLfloat x, GLfloat y, GLfloat z, GLfloat w) { const GLfloat v[]{x, y, z, w}; WriteUniform(l, v, 4, 1); }
void APIENTRY Uniform1fv(GLint l, GLsizei n, const GLfloat* v) { WriteUniform(l, v, 1, n); }
void APIENTRY Uniform2fv(GLint l, GLsizei n, const GLfloat* v) { WriteUniform(l, v, 2, n); }
void APIENTRY Uniform3fv(GLint l, GLsizei n, const GLfloat* v) { WriteUniform(l, v, 3, n); }
void APIENTRY Uniform4fv(GLint l, GLsizei n, const GLfloat* v) { WriteUniform(l, v, 4, n); }
void APIENTRY UniformMatrix3fv(GLint l, GLsizei n, GLboolean t, const GLfloat* v) { WriteUniform(l, v, 3, n, 3, t == GL_TRUE); }
void APIENTRY UniformMatrix4fv(GLint l, GLsizei n, GLboolean t, const GLfloat* v) { WriteUniform(l, v, 4, n, 4, t == GL_TRUE); }
GLuint APIENTRY GetUniformBlockIndex(GLuint, const GLchar*) { return GL_INVALID_INDEX; }
void APIENTRY UniformBlockBinding(GLuint, GLuint, GLuint) {}
// (the GL shader objects: no run-time compiler; gl.cpp asks CreateProgram for its programs instead)
GLuint APIENTRY CreateShader(GLenum) { return 0; }
void APIENTRY DeleteShader(GLuint) {}
void APIENTRY ShaderSource(GLuint, GLsizei, const GLchar* const*, const GLint*) {}
void APIENTRY CompileShader(GLuint) {}
void APIENTRY GetShaderiv(GLuint, GLenum pname, GLint* v) { if (v) *v = pname == GL_COMPILE_STATUS ? GL_FALSE : 0; }
void APIENTRY GetShaderInfoLog(GLuint, GLsizei size, GLsizei* length, GLchar* log)
{
    static const char kMessage[] = "no run-time GLSL compiler on Vulkan (programs are compiled at build time)";
    if (length) *length = 0;
    if (log && size > 0) { std::strncpy(log, kMessage, size_t(size) - 1); log[size - 1] = 0; if (length) *length = GLsizei(std::strlen(log)); }
}
GLuint APIENTRY CreateProgramGL() { return 0; }
void APIENTRY AttachShader(GLuint, GLuint) {}
void APIENTRY LinkProgram(GLuint) {}
void APIENTRY GetProgramiv(GLuint program, GLenum pname, GLint* v)
{
    if (!v) return;
    *v = 0;
    if (pname == GL_LINK_STATUS || pname == GL_VALIDATE_STATUS) *v = g_programs.Get(program) ? GL_TRUE : GL_FALSE;
    else if (pname == GL_ACTIVE_UNIFORMS) { const Program* p = g_programs.Get(program); *v = p ? GLint(p->info->uniformCount) : 0; }
}
void APIENTRY GetProgramInfoLog(GLuint p, GLsizei size, GLsizei* length, GLchar* log) { GetShaderInfoLog(p, size, length, log); }

// ------------------------------------------------------------------------------------------------ queries and syncs
VkQueryPool g_queryPool = VK_NULL_HANDLE;
constexpr uint32_t kQueries = 64;
std::vector<uint32_t> g_freeQueries;
void APIENTRY GenQueries(GLsizei n, GLuint* names)
{
    for (GLsizei i = 0; i < n; ++i)
    {
        names[i] = g_queries.Gen();
        Query* q = g_queries.Get(names[i]);
        if (!g_freeQueries.empty()) { q->index = g_freeQueries.back(); g_freeQueries.pop_back(); }
        else q->index = UINT32_MAX;
    }
}
void APIENTRY DeleteQueries(GLsizei n, const GLuint* names)
{
    for (GLsizei i = 0; i < n; ++i)
        if (auto q = g_queries.Take(names[i]))
            if (q->index != UINT32_MAX) g_freeQueries.push_back(q->index);
}
GLuint g_activeQuery = 0;
void APIENTRY BeginQuery(GLenum target, GLuint name)
{
    Query* q = g_queries.Get(name);
    if (!q || target != GL_TIME_ELAPSED || q->index == UINT32_MAX || !g_queryPool) return;
    // (the game reads each query before it begins it again: its earlier timestamps are no longer in use)
    g_vkResetQueryPool(g_device, g_queryPool, q->index, 2);
    vkCmdWriteTimestamp(Main(), VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, g_queryPool, q->index);
    g_activeQuery = name;
}
void APIENTRY EndQuery(GLenum)
{
    Query* q = g_queries.Get(g_activeQuery);
    if (!q || q->index == UINT32_MAX) return;
    vkCmdWriteTimestamp(Main(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_queryPool, q->index + 1);
    q->value = g_submitValue + 1;
    g_activeQuery = 0;
}
void APIENTRY GetQueryObjectui64v(GLuint name, GLenum pname, GLuint64* value)
{
    Query* q = g_queries.Get(name);
    if (!q || !value) return;
    if (pname == GL_QUERY_RESULT_AVAILABLE) { *value = 1; return; }
    *value = 0;
    if (q->index == UINT32_MAX || !q->value) return;
    if (q->value > g_submitValue) Submit();
    WaitValue(q->value);
    uint64_t t[2] = {};
    if (vkGetQueryPoolResults(g_device, g_queryPool, q->index, 2, sizeof(t), t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS && t[1] > t[0])
        *value = GLuint64(double(t[1] - t[0]) * double(g_props.limits.timestampPeriod));
}
void APIENTRY GetQueryObjectuiv(GLuint name, GLenum pname, GLuint* value)
{
    GLuint64 v = 0;
    GetQueryObjectui64v(name, pname, &v);
    if (value) *value = GLuint(v);
}
GLsync APIENTRY FenceSync(GLenum, GLbitfield)
{
    auto sync = std::make_unique<Sync>();
    sync->value = g_recording ? g_submitValue + 1 : g_submitValue;
    GLsync handle = reinterpret_cast<GLsync>(sync.get());
    g_syncs.push_back(std::move(sync));
    return handle;
}
Sync* FindSync(GLsync handle)
{
    for (auto& sync : g_syncs) if (reinterpret_cast<GLsync>(sync.get()) == handle) return sync.get();
    return nullptr;
}
GLenum APIENTRY ClientWaitSync(GLsync handle, GLbitfield flags, GLuint64 timeout)
{
    Sync* sync = FindSync(handle);
    if (!sync) return GL_WAIT_FAILED;
    if (sync->value > g_submitValue)
    {
        if (!(flags & GL_SYNC_FLUSH_COMMANDS_BIT) && timeout == 0) return GL_TIMEOUT_EXPIRED;
        Submit();
    }
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &g_timeline;
    wi.pValues = &sync->value;
    const VkResult r = vkWaitSemaphores(g_device, &wi, timeout == GL_TIMEOUT_IGNORED ? UINT64_MAX : timeout);
    return r == VK_SUCCESS ? GL_CONDITION_SATISFIED : r == VK_TIMEOUT ? GL_TIMEOUT_EXPIRED : GL_WAIT_FAILED;
}
void APIENTRY WaitSync(GLsync, GLbitfield, GLuint64) {}
void APIENTRY DeleteSync(GLsync handle)
{
    g_syncs.erase(std::remove_if(g_syncs.begin(), g_syncs.end(), [&](const auto& p) { return reinterpret_cast<GLsync>(p.get()) == handle; }), g_syncs.end());
}

// ------------------------------------------------------------------------------------------------ queries
GLenum APIENTRY GetError() { const GLenum e = s.error; s.error = GL_NO_ERROR; return e; }
std::vector<std::string> g_extensions;
std::string g_rendererString, g_versionString = "4.5.0 SafeHouse Vulkan";
const GLubyte* APIENTRY GetString(GLenum name)
{
    const char* v = "";
    switch (name)
    {
    case GL_VENDOR: v = "SafeHouse"; break;
    case GL_RENDERER: v = g_rendererString.c_str(); break;
    case GL_VERSION: v = g_versionString.c_str(); break;
    case GL_SHADING_LANGUAGE_VERSION: v = "4.50 (SPIR-V at build time)"; break;
    default: break;
    }
    return reinterpret_cast<const GLubyte*>(v);
}
const GLubyte* APIENTRY GetStringi(GLenum name, GLuint index)
{
    if (name == GL_EXTENSIONS && index < g_extensions.size()) return reinterpret_cast<const GLubyte*>(g_extensions[index].c_str());
    return nullptr;
}
void APIENTRY GetIntegerv(GLenum pname, GLint* v)
{
    if (!v) return;
    switch (pname)
    {
    case GL_NUM_EXTENSIONS: *v = GLint(g_extensions.size()); break;
    case GL_MAJOR_VERSION: *v = 4; break;
    case GL_MINOR_VERSION: *v = 5; break;
    case GL_MAX_SAMPLES: case GL_MAX_COLOR_TEXTURE_SAMPLES: case GL_MAX_DEPTH_TEXTURE_SAMPLES:
        *v = (g_feat.samples & VK_SAMPLE_COUNT_8_BIT) ? 8 : (g_feat.samples & VK_SAMPLE_COUNT_4_BIT) ? 4 : (g_feat.samples & VK_SAMPLE_COUNT_2_BIT) ? 2 : 1;
        break;
    case GL_MAX_TEXTURE_SIZE: case GL_MAX_RENDERBUFFER_SIZE: *v = GLint(g_props.limits.maxImageDimension2D); break;
    case GL_MAX_ARRAY_TEXTURE_LAYERS: *v = GLint(g_props.limits.maxImageArrayLayers); break;
    case GL_MAX_TEXTURE_IMAGE_UNITS: case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: *v = GLint(kMaxUnits); break;
    case GL_MAX_VERTEX_ATTRIBS: *v = GLint(kGenericBinding); break;
    case GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS: case GL_MAX_UNIFORM_BUFFER_BINDINGS: *v = GLint(kMaxBindings); break;
    case GL_MAX_DUAL_SOURCE_DRAW_BUFFERS: *v = g_feat.dualSrcBlend ? 1 : 0; break;
    case GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT: *v = GLint(std::max<VkDeviceSize>(g_props.limits.minStorageBufferOffsetAlignment, 4)); break;
    case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT: *v = GLint(std::max<VkDeviceSize>(g_props.limits.minUniformBufferOffsetAlignment, 4)); break;
    case GL_MAX_SHADER_STORAGE_BLOCK_SIZE: *v = GLint(std::min<uint32_t>(g_props.limits.maxStorageBufferRange, 0x7FFFFFFF)); break;
    case GL_VERTEX_ARRAY_BINDING: *v = GLint(s.vertexArray); break;
    case GL_CURRENT_PROGRAM: *v = GLint(s.program); break;
    case GL_ARRAY_BUFFER_BINDING: *v = GLint(s.arrayBuffer); break;
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: *v = GLint(g_arrays.Get(s.vertexArray)->elements); break;
    case GL_DRAW_FRAMEBUFFER_BINDING: *v = GLint(s.drawFramebuffer); break;
    case GL_READ_FRAMEBUFFER_BINDING: *v = GLint(s.readFramebuffer); break;
    case GL_RENDERBUFFER_BINDING: *v = GLint(s.renderbuffer); break;
    case GL_TEXTURE_BINDING_2D: *v = GLint(s.units[s.activeUnit][0]); break;
    case GL_TEXTURE_BINDING_2D_ARRAY: *v = GLint(s.units[s.activeUnit][1]); break;
    case GL_SAMPLER_BINDING: *v = GLint(s.samplerUnits[s.activeUnit]); break;
    case GL_ACTIVE_TEXTURE: *v = GLint(GL_TEXTURE0 + s.activeUnit); break;
    case GL_BLEND_SRC_RGB: *v = GLint(s.blendSrc); break;
    case GL_BLEND_DST_RGB: *v = GLint(s.blendDst); break;
    case GL_BLEND_SRC_ALPHA: *v = GLint(s.blendSrcAlpha); break;
    case GL_BLEND_DST_ALPHA: *v = GLint(s.blendDstAlpha); break;
    case GL_BLEND_EQUATION_RGB: *v = GLint(s.blendEq); break;
    case GL_BLEND_EQUATION_ALPHA: *v = GLint(s.blendEqAlpha); break;
    case GL_DEPTH_FUNC: *v = GLint(s.depthFunc); break;
    case GL_STENCIL_FUNC: *v = GLint(s.front.func); break;
    case GL_STENCIL_REF: *v = s.front.ref; break;
    case GL_STENCIL_VALUE_MASK: *v = GLint(s.front.mask); break;
    case GL_STENCIL_WRITEMASK: *v = GLint(s.front.writeMask); break;
    case GL_STENCIL_FAIL: *v = GLint(s.front.fail); break;
    case GL_STENCIL_PASS_DEPTH_FAIL: *v = GLint(s.front.zfail); break;
    case GL_STENCIL_PASS_DEPTH_PASS: *v = GLint(s.front.zpass); break;
    case GL_CULL_FACE_MODE: *v = GLint(s.cullFace); break;
    case GL_FRONT_FACE: *v = GLint(s.frontFace); break;
    case GL_VIEWPORT: std::memcpy(v, s.viewport, sizeof(s.viewport)); break;
    case GL_SCISSOR_BOX: std::memcpy(v, s.scissor, sizeof(s.scissor)); break;
    case GL_UNPACK_ALIGNMENT: *v = s.unpackAlignment; break;
    case GL_UNPACK_ROW_LENGTH: *v = s.unpackRowLength; break;
    case GL_PACK_ALIGNMENT: *v = s.packAlignment; break;
    case GL_POLYGON_MODE: v[0] = v[1] = GLint(s.polygonMode); break;
    default: *v = 0; break;
    }
}
void APIENTRY GetBooleanv(GLenum pname, GLboolean* v)
{
    if (!v) return;
    if (pname == GL_DEPTH_WRITEMASK) *v = s.depthWrite ? GL_TRUE : GL_FALSE;
    else if (pname == GL_COLOR_WRITEMASK) for (unsigned i = 0; i < 4; ++i) v[i] = (s.colorMask >> i) & 1 ? GL_TRUE : GL_FALSE;
    else *v = IsEnabled(pname);
}
void APIENTRY GetFloatv(GLenum pname, GLfloat* v)
{
    if (!v) return;
    switch (pname)
    {
    case kMaxAnisotropyEnum: *v = g_feat.anisotropy ? g_feat.anisotropyMax : 1.0f; break;
    case GL_LINE_WIDTH: *v = s.lineWidth; break;
    case GL_POINT_SIZE: *v = 1.0f; break;
    case GL_DEPTH_RANGE: v[0] = s.depthNear; v[1] = s.depthFar; break;
    case GL_COLOR_CLEAR_VALUE: std::memcpy(v, s.clearColor.data(), 16); break;
    case GL_BLEND_COLOR: std::memcpy(v, s.blendColor.data(), 16); break;
    case GL_DEPTH_CLEAR_VALUE: *v = s.clearDepth; break;
    default: { GLint i[4]{}; GetIntegerv(pname, i); *v = float(i[0]); break; }
    }
}
void APIENTRY GetDoublev(GLenum pname, GLdouble* v)
{
    GLfloat f[4]{};
    GetFloatv(pname, f);
    if (v) { v[0] = f[0]; if (pname == GL_DEPTH_RANGE) v[1] = f[1]; }
}

// ------------------------------------------------------------------------------------------------ the entry points
#define CANE_CHECK(fn, pfn) static_assert(std::is_same_v<decltype(&fn), pfn>, #fn " signature")
CANE_CHECK(GenBuffers, PFNGLGENBUFFERSPROC); CANE_CHECK(BindBuffer, PFNGLBINDBUFFERPROC); CANE_CHECK(BufferData, PFNGLBUFFERDATAPROC);
CANE_CHECK(BufferSubData, PFNGLBUFFERSUBDATAPROC); CANE_CHECK(DeleteBuffers, PFNGLDELETEBUFFERSPROC); CANE_CHECK(MapBufferRange, PFNGLMAPBUFFERRANGEPROC);
CANE_CHECK(BindBufferRange, PFNGLBINDBUFFERRANGEPROC); CANE_CHECK(BindBufferBase, PFNGLBINDBUFFERBASEPROC);
CANE_CHECK(VertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC); CANE_CHECK(VertexAttribIPointer, PFNGLVERTEXATTRIBIPOINTERPROC);
CANE_CHECK(TexImage2D, PFNGLTEXIMAGE2DPROC); CANE_CHECK(TexSubImage2D, PFNGLTEXSUBIMAGE2DPROC); CANE_CHECK(TexSubImage3D, PFNGLTEXSUBIMAGE3DPROC);
CANE_CHECK(TexStorage2D, PFNGLTEXSTORAGE2DPROC); CANE_CHECK(TexStorage3D, PFNGLTEXSTORAGE3DPROC);
CANE_CHECK(BlitFramebuffer, PFNGLBLITFRAMEBUFFERPROC); CANE_CHECK(ReadPixels, PFNGLREADPIXELSPROC);
CANE_CHECK(DrawElementsInstancedBaseVertexBaseInstance, PFNGLDRAWELEMENTSINSTANCEDBASEVERTEXBASEINSTANCEPROC);
CANE_CHECK(MultiDrawElementsIndirect, PFNGLMULTIDRAWELEMENTSINDIRECTPROC); CANE_CHECK(UniformMatrix4fv, PFNGLUNIFORMMATRIX4FVPROC);
CANE_CHECK(FenceSync, PFNGLFENCESYNCPROC); CANE_CHECK(ClientWaitSync, PFNGLCLIENTWAITSYNCPROC); CANE_CHECK(GetQueryObjectui64v, PFNGLGETQUERYOBJECTUI64VPROC);
CANE_CHECK(StencilOpSeparate, PFNGLSTENCILOPSEPARATEPROC); CANE_CHECK(BlendColor, PFNGLBLENDCOLORPROC);
CANE_CHECK(InvalidateFramebuffer, PFNGLINVALIDATEFRAMEBUFFERPROC);
#undef CANE_CHECK

struct ProcEntry { const char* name; void* fn; };
#define P(name, fn) {"gl" name, reinterpret_cast<void*>(&fn)}
const ProcEntry kProcs[] = {
    P("GetError", GetError), P("GetString", GetString), P("GetStringi", GetStringi), P("GetIntegerv", GetIntegerv),
    P("GetBooleanv", GetBooleanv), P("GetFloatv", GetFloatv), P("GetDoublev", GetDoublev),
    P("Enable", Enable), P("Disable", Disable), P("IsEnabled", IsEnabled), P("Viewport", Viewport), P("Scissor", Scissor),
    P("DepthRange", DepthRange), P("DepthRangef", DepthRangef), P("DepthBoundsEXT", DepthBoundsEXT), P("ClearColor", ClearColor), P("ClearDepth", ClearDepth),
    P("ClearDepthf", ClearDepthf), P("ClearStencil", ClearStencil), P("Clear", Clear), P("ClearBufferfv", ClearBufferfv),
    P("ColorMask", ColorMask), P("DepthMask", DepthMask), P("DepthFunc", DepthFunc), P("StencilMask", StencilMask),
    P("StencilMaskSeparate", StencilMaskSeparate), P("StencilFunc", StencilFunc), P("StencilFuncSeparate", StencilFuncSeparate),
    P("StencilOp", StencilOp), P("StencilOpSeparate", StencilOpSeparate), P("BlendFunc", BlendFunc), P("BlendFuncSeparate", BlendFuncSeparate),
    P("BlendEquation", BlendEquation), P("BlendEquationSeparate", BlendEquationSeparate), P("BlendColor", BlendColor),
    P("CullFace", CullFace), P("FrontFace", FrontFace), P("LineWidth", LineWidth), P("PointSize", PointSize), P("PolygonMode", PolygonMode),
    P("PixelStorei", PixelStorei), P("Finish", FinishGL), P("Flush", Flush), P("MemoryBarrier", MemoryBarrier),
    P("DrawBuffer", DrawBuffer), P("DrawBuffers", DrawBuffers), P("ReadBuffer", ReadBuffer), P("ClipControl", ClipControl),
    P("InvalidateFramebuffer", InvalidateFramebuffer),
    P("GenBuffers", GenBuffers), P("DeleteBuffers", DeleteBuffers), P("BindBuffer", BindBuffer), P("BindBufferBase", BindBufferBase),
    P("BindBufferRange", BindBufferRange), P("BufferData", BufferData), P("BufferSubData", BufferSubData), P("MapBufferRange", MapBufferRange),
    P("UnmapBuffer", UnmapBuffer), P("IsBuffer", IsBuffer),
    P("GenTextures", GenTextures), P("DeleteTextures", DeleteTextures), P("ActiveTexture", ActiveTexture), P("BindTexture", BindTexture),
    P("TexImage2D", TexImage2D), P("TexStorage2D", TexStorage2D), P("TexStorage3D", TexStorage3D), P("TexSubImage2D", TexSubImage2D),
    P("TexSubImage3D", TexSubImage3D), P("TexParameteri", TexParameteri), P("TexParameterf", TexParameterf), P("GenerateMipmap", GenerateMipmap),
    P("GetTexLevelParameteriv", GetTexLevelParameteriv), P("IsTexture", IsTexture),
    P("GenSamplers", GenSamplers), P("DeleteSamplers", DeleteSamplers), P("BindSampler", BindSampler),
    P("SamplerParameteri", SamplerParameteri), P("SamplerParameterf", SamplerParameterf),
    P("GetTextureSamplerHandleARB", GetTextureSamplerHandleARB), P("MakeTextureHandleResidentARB", MakeTextureHandleResidentARB),
    P("MakeTextureHandleNonResidentARB", MakeTextureHandleNonResidentARB),
    P("GenRenderbuffers", GenRenderbuffers), P("DeleteRenderbuffers", DeleteRenderbuffers), P("BindRenderbuffer", BindRenderbuffer),
    P("RenderbufferStorage", RenderbufferStorage), P("RenderbufferStorageMultisample", RenderbufferStorageMultisample),
    P("GenVertexArrays", GenVertexArrays), P("DeleteVertexArrays", DeleteVertexArrays), P("BindVertexArray", BindVertexArray),
    P("EnableVertexAttribArray", EnableVertexAttribArray), P("DisableVertexAttribArray", DisableVertexAttribArray),
    P("VertexAttribPointer", VertexAttribPointer), P("VertexAttribIPointer", VertexAttribIPointer), P("VertexAttribDivisor", VertexAttribDivisor),
    P("GenFramebuffers", GenFramebuffers), P("DeleteFramebuffers", DeleteFramebuffers), P("BindFramebuffer", BindFramebuffer),
    P("FramebufferTexture2D", FramebufferTexture2D), P("FramebufferTextureLayer", FramebufferTextureLayer),
    P("FramebufferRenderbuffer", FramebufferRenderbuffer), P("CheckFramebufferStatus", CheckFramebufferStatus),
    P("BlitFramebuffer", BlitFramebuffer), P("ReadPixels", ReadPixels),
    P("CreateShader", CreateShader), P("DeleteShader", DeleteShader), P("ShaderSource", ShaderSource), P("CompileShader", CompileShader),
    P("GetShaderiv", GetShaderiv), P("GetShaderInfoLog", GetShaderInfoLog), P("CreateProgram", CreateProgramGL), P("DeleteProgram", DeleteProgram),
    P("AttachShader", AttachShader), P("LinkProgram", LinkProgram), P("GetProgramiv", GetProgramiv), P("GetProgramInfoLog", GetProgramInfoLog),
    P("UseProgram", UseProgram), P("GetUniformLocation", GetUniformLocation), P("GetUniformBlockIndex", GetUniformBlockIndex),
    P("UniformBlockBinding", UniformBlockBinding),
    P("Uniform1i", Uniform1i), P("Uniform2i", Uniform2i), P("Uniform1iv", Uniform1iv), P("Uniform1ui", Uniform1ui), P("Uniform1f", Uniform1f),
    P("Uniform2f", Uniform2f), P("Uniform3f", Uniform3f), P("Uniform4f", Uniform4f), P("Uniform1fv", Uniform1fv), P("Uniform2fv", Uniform2fv),
    P("Uniform3fv", Uniform3fv), P("Uniform4fv", Uniform4fv), P("UniformMatrix3fv", UniformMatrix3fv), P("UniformMatrix4fv", UniformMatrix4fv),
    P("DrawArrays", DrawArrays), P("DrawArraysInstanced", DrawArraysInstanced), P("DrawArraysInstancedBaseInstance", DrawArraysInstancedBaseInstance),
    P("DrawElements", DrawElements), P("DrawElementsBaseVertex", DrawElementsBaseVertex), P("DrawElementsInstanced", DrawElementsInstanced),
    P("DrawElementsInstancedBaseVertex", DrawElementsInstancedBaseVertex), P("DrawElementsInstancedBaseInstance", DrawElementsInstancedBaseInstance),
    P("DrawElementsInstancedBaseVertexBaseInstance", DrawElementsInstancedBaseVertexBaseInstance),
    P("MultiDrawElementsIndirect", MultiDrawElementsIndirect),
    P("GenQueries", GenQueries), P("DeleteQueries", DeleteQueries), P("BeginQuery", BeginQuery), P("EndQuery", EndQuery),
    P("GetQueryObjectui64v", GetQueryObjectui64v), P("GetQueryObjectuiv", GetQueryObjectuiv),
    P("FenceSync", FenceSync), P("DeleteSync", DeleteSync), P("ClientWaitSync", ClientWaitSync), P("WaitSync", WaitSync),
};
#undef P

// ------------------------------------------------------------------------------------------------ device lifetime
VkSurfaceKHR g_surface = VK_NULL_HANDLE;
VkSwapchainKHR g_swapchain = VK_NULL_HANDLE;
VkFormat g_swapFormat = VK_FORMAT_B8G8R8A8_UNORM;
std::vector<VkImage> g_swapImages;
std::vector<VkSemaphore> g_presentSemaphores;
VkExtent2D g_swapExtent{};
int g_swapInterval = 1;
bool g_swapchainStale = false;
// Pre-rotation (Android, the submission worker): a phone held in landscape has a display turned against its panel;
// the swapchain then takes the display's own orientation and transform, and the window's picture is drawn into it
// turned (shaders/present.*) instead of being copied, so the compositor need not turn every frame.
uint32_t g_swapRotation = 0;                 // the swapchain's clockwise quarter turns (0: identity, the copy)
bool g_swapTransformMatches = true;          // its transform was the display's when made (SUBOPTIMAL: the display turned)
std::vector<VkImageView> g_swapViews;
GLuint g_presentProgram = 0;
VkPipeline g_presentPipeline = VK_NULL_HANDLE;
VkFormat g_presentPipelineFormat = VK_FORMAT_UNDEFINED;
uint64_t (*g_surfaceSerialFn)() = nullptr;   // (SetSurfaceSerial)
uint64_t g_surfaceSerial = 0;                // the serial g_surface was made for (0: none)
unsigned g_surfaceRetry = 0;                 // frames until a failed surface is tried again
bool g_headlessWsi = false;                  // (SLY2N_VKHEADLESSWSI: the surface is a VK_EXT_headless_surface one)

GLuint MakeBlack(bool array)
{
    GLuint name = g_textures.Gen();
    Texture* t = g_textures.Get(name);
    t->image = MakeImage(1, 1, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, array, true);
    t->image->immutable = true;
    t->params.minFilter = GL_LINEAR;
    const uint32_t black = 0xFF000000u;
    const int alignment = s.unpackAlignment;
    s.unpackAlignment = 1;
    UploadPixels(*t->image, 0, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &black);
    s.unpackAlignment = alignment;
    return name;
}

// ---- the worker (deferred recording, above)
uint32_t g_workerIndex = UINT32_MAX;   // the swapchain image the worker acquired for the batch being recorded
VkCommandBuffer WorkerRecord(Frame& f, const Stream& stream)
{
    VkCommandBuffer cmd = AllocCommandBuffer(f);
    stream.Replay(cmd);
    VKCHECK(vkEndCommandBuffer(cmd));
    return cmd;
}
// (recorded as one deferred command: the image index is the worker's)
struct WindowPicture { VkImage image; uint32_t width, height; uint32_t rotation = 0, imageSlot = 0, samplerSlot = 0; };
void RecordPresentDraw(VkCommandBuffer cmd, uint32_t index, const WindowPicture& win);
void RecordSwapBlit(VkCommandBuffer cmd, uint32_t index, WindowPicture win)
{
    if (index == UINT32_MAX) return;
    if (win.rotation) return RecordPresentDraw(cmd, index, win);
    VkImageMemoryBarrier2 sw{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    sw.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    sw.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    sw.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    sw.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    sw.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    sw.srcQueueFamilyIndex = sw.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sw.image = g_swapImages[index];
    sw.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &sw;
    cane::vk::vkCmdPipelineBarrier2(cmd, &di);
    // (GL's rows bottom first: flipped into the swapchain's, top first)
    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {int32_t(win.width), int32_t(win.height), 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[0] = {0, int32_t(g_swapExtent.height), 0};
    blit.dstOffsets[1] = {int32_t(g_swapExtent.width), 0, 1};
    const bool same = win.width == g_swapExtent.width && win.height == g_swapExtent.height;
    cane::vk::vkCmdBlitImage(cmd, win.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_swapImages[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             1, &blit, same ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    sw.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
    sw.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    sw.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    sw.dstAccessMask = 0;
    sw.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    sw.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    cane::vk::vkCmdPipelineBarrier2(cmd, &di);
}
// (SLY2N_FPSLOG: the worker's thread CPU time: all of it, and its recording and submission)
std::atomic<int64_t> g_workerCpuNs{0}, g_workerRecordCpuNs{0};
int64_t ThreadCpuNs()
{
#if defined(CLOCK_THREAD_CPUTIME_ID) && !defined(_WIN32)
    timespec ts{};
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0) return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
#endif
    return 0;
}
void WorkerLoop()
{
    for (;;)
    {
        Batch bt;
        {
            std::unique_lock<std::mutex> lock(g_workerMutex);
            g_workerCv.wait(lock, [] { return !g_batches.empty(); });
            bt = g_batches.front();
            g_batches.pop_front();
        }
        if (bt.quit) break;
        Frame& f = g_frames[bt.slot];
        if (bt.resetPool)
        {
            VKCHECK(vkResetCommandPool(g_device, f.pool, 0));
            f.cmdNext = 0;
        }
        VkSemaphore wait = VK_NULL_HANDLE, signal = VK_NULL_HANDLE;
        g_workerIndex = UINT32_MAX;
        if (bt.present && g_swapchain)
        {
            uint32_t index = 0;
            const VkResult r = vkAcquireNextImageKHR(g_device, g_swapchain, UINT64_MAX, f.acquire, VK_NULL_HANDLE, &index);
            if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)
            {
                g_workerIndex = index;
                wait = f.acquire;
                signal = g_presentSemaphores[index];
                if (r == VK_SUBOPTIMAL_KHR && (kSuboptimalStale || g_swapTransformMatches)) g_presentStale = true;
            }
            else g_presentStale = true;
        }
        VkCommandBuffer cmds[2];
        uint32_t n = 0;
        const int64_t nsRecord0 = ThreadCpuNs();
        if (bt.pre) cmds[n++] = WorkerRecord(f, *bt.pre);
        cmds[n++] = WorkerRecord(f, *bt.main);
        SubmitBatch(cmds, n, bt.value, wait, signal);
        g_workerRecordCpuNs += ThreadCpuNs() - nsRecord0;
        if (g_workerIndex != UINT32_MAX)
        {
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &signal;
            pi.swapchainCount = 1;
            pi.pSwapchains = &g_swapchain;
            pi.pImageIndices = &g_workerIndex;
            const VkResult r = vkQueuePresentKHR(g_queue, &pi);
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR || (r == VK_SUBOPTIMAL_KHR && (kSuboptimalStale || g_swapTransformMatches)))
                g_presentStale = true;
            else if (r == VK_ERROR_DEVICE_LOST) Fatal("the GPU device was lost");
        }
        for (auto& fn : bt.after) fn();
        g_workerCpuNs = ThreadCpuNs();
        {
            std::lock_guard<std::mutex> lock(g_workerMutex);
            if (bt.pre) bt.pre->Clear(), g_freeStreams.push_back(bt.pre);
            bt.main->Clear();
            g_freeStreams.push_back(bt.main);
            g_workerDone = bt.value;
        }
        g_workerDoneCv.notify_all();
    }
}
// Everything handed to the worker submitted (before the swapchain or the device is touched from this thread).
void WorkerIdle()
{
    if (!g_threaded || !g_worker.joinable()) return;
    std::unique_lock<std::mutex> lock(g_workerMutex);
    g_workerDoneCv.wait(lock, [] { return g_batches.empty() && g_workerDone >= g_submitValue; });
}
void StopWorker()
{
    if (!g_worker.joinable()) return;
    WorkerIdle();
    {
        std::lock_guard<std::mutex> lock(g_workerMutex);
        Batch q;
        q.quit = true;
        g_batches.push_back(q);
    }
    g_workerCv.notify_one();
    g_worker.join();
    std::lock_guard<std::mutex> lock(g_workerMutex);
    for (Stream* st : g_freeStreams) delete st;
    g_freeStreams.clear();
}

// (pre-rotation) the present program's pipeline for the swapchain's format: no vertex input, no depth, no blending
void CreatePresentPipeline(VkFormat format)
{
    Program* p = g_programs.Get(g_presentProgram);
    if (!p) return;
    if (g_presentPipeline) vkDestroyPipeline(g_device, g_presentPipeline, nullptr);
    g_presentPipeline = VK_NULL_HANDLE;
    VkPipelineShaderStageCreateInfo stages[2]{};
    for (unsigned k = 0; k < 2; ++k)
    {
        stages[k].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[k].stage = k ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
        stages[k].module = p->modules[k ? 2 : 0];
        stages[k].pName = "main";
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &att;
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &format;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.pNext = &rci;
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &ds;
    ci.layout = g_pipelineLayout;
    if (vkCreateGraphicsPipelines(g_device, g_pipelineCache, 1, &ci, nullptr, &g_presentPipeline) != VK_SUCCESS) g_presentPipeline = VK_NULL_HANDLE;
    g_presentPipelineFormat = g_presentPipeline ? format : VK_FORMAT_UNDEFINED;
}
// (the worker) the window's picture drawn turned into swapchain image `index` (pre-rotation; else RecordSwapBlit's copy)
void RecordPresentDraw(VkCommandBuffer cmd, uint32_t index, const WindowPicture& win)
{
    Program* p = g_programs.Get(g_presentProgram);
    VkImageMemoryBarrier2 sw{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    sw.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    sw.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    sw.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    sw.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    sw.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    sw.srcQueueFamilyIndex = sw.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sw.image = g_swapImages[index];
    sw.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &sw;
    cane::vk::vkCmdPipelineBarrier2(cmd, &di);
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView = g_swapViews[index];
    ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, g_swapExtent};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &ca;
    cane::vk::vkCmdBeginRendering(cmd, &ri);
    if (p && g_presentPipeline)
    {
        const VkViewport viewport{0.0f, 0.0f, float(g_swapExtent.width), float(g_swapExtent.height), 0.0f, 1.0f};
        const VkRect2D scissor{{0, 0}, g_swapExtent};
        cane::vk::vkCmdSetViewport(cmd, 0, 1, &viewport);
        cane::vk::vkCmdSetScissor(cmd, 0, 1, &scissor);
        cane::vk::vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_presentPipeline);
        cane::vk::vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipelineLayout, 0, 1, &g_textureSet, 0, nullptr);
        // (the program's push constants: the picture's slot pair, the turn)
        uint8_t push[64] = {};
        for (uint32_t i = 0; i < p->info->uniformCount; ++i)
        {
            const Shaders::Uniform& u = p->info->uniforms[i];
            if (u.type >= 32) { const uint32_t pair[2] = {win.imageSlot, win.samplerSlot}; std::memcpy(push + u.offset, pair, 8); }
            else if (!std::strcmp(u.name, "rotation")) { const int32_t r = int32_t(win.rotation); std::memcpy(push + u.offset, &r, 4); }
        }
        cane::vk::vkCmdPushConstants(cmd, g_pipelineLayout, g_stages, 0, std::min<uint32_t>(p->info->pushSize, sizeof push), push);
        cane::vk::vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    cane::vk::vkCmdEndRendering(cmd);
    sw.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    sw.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    sw.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    sw.dstAccessMask = 0;
    sw.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    sw.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    cane::vk::vkCmdPipelineBarrier2(cmd, &di);
}

void DestroySwapchain()
{
    if (!g_device) return;
    WorkerIdle();
    vkDeviceWaitIdle(g_device);
    for (VkSemaphore sem : g_presentSemaphores) vkDestroySemaphore(g_device, sem, nullptr);
    g_presentSemaphores.clear();
    for (VkImageView v : g_swapViews) vkDestroyImageView(g_device, v, nullptr);
    g_swapViews.clear();
    if (g_swapchain) vkDestroySwapchainKHR(g_device, g_swapchain, nullptr);
    g_swapchain = VK_NULL_HANDLE;
    g_swapImages.clear();
}
VkPresentModeKHR g_swapMode = VK_PRESENT_MODE_MAX_ENUM_KHR;
bool CreateSwapchain(std::string* error)
{
    if (!g_surface) return true;   // (Android in the background: no surface until the app returns)
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_phys, g_surface, &caps) != VK_SUCCESS) { if (error) *error = "surface capabilities"; return false; }
    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(g_window, &fbw, &fbh);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) extent = {uint32_t(std::max(fbw, 1)), uint32_t(std::max(fbh, 1))};
    uint32_t rotation = 0;
#if defined(__ANDROID__)
    static const bool s_fNoPreRotate = std::getenv("SLY2N_VKNOPREROTATE") && std::atoi(std::getenv("SLY2N_VKNOPREROTATE")) != 0;
    if (g_threaded && g_presentProgram && !s_fNoPreRotate && (caps.supportedTransforms & caps.currentTransform))
        rotation = caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ? 1 : caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR ? 2
                 : caps.currentTransform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR ? 3 : 0;
    // (Android reports the extent as the app sees it: the image in the display's orientation has it the other way)
    if (rotation == 1 || rotation == 3) std::swap(extent.width, extent.height);
#endif
    extent.width = std::clamp(extent.width, caps.minImageExtent.width, caps.maxImageExtent.width);
    extent.height = std::clamp(extent.height, caps.minImageExtent.height, caps.maxImageExtent.height);
    if (!extent.width || !extent.height) return true;   // (minimized: no swapchain until it has a size)
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { if (error) *error = "the swapchain cannot be copied into"; return false; }
    uint32_t n = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &n, nullptr);
    std::vector<VkSurfaceFormatKHR> formats(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(g_phys, g_surface, &n, formats.data());
    VkSurfaceFormatKHR chosen = formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : formats[0];
    for (const auto& f : formats)
        if ((f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) { chosen = f; break; }
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_phys, g_surface, &n, nullptr);
    std::vector<VkPresentModeKHR> modes(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(g_phys, g_surface, &n, modes.data());
    auto has = [&](VkPresentModeKHR m) { return std::find(modes.begin(), modes.end(), m) != modes.end(); };
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    if (g_swapInterval == 0) mode = has(VK_PRESENT_MODE_IMMEDIATE_KHR) ? VK_PRESENT_MODE_IMMEDIATE_KHR : has(VK_PRESENT_MODE_MAILBOX_KHR) ? VK_PRESENT_MODE_MAILBOX_KHR : mode;
    VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    ci.surface = g_surface;
    ci.minImageCount = std::max(caps.minImageCount, 3u);
    if (caps.maxImageCount) ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount);
    ci.imageFormat = chosen.format;
    ci.imageColorSpace = chosen.colorSpace;
    ci.imageExtent = extent;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | (rotation ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = rotation ? caps.currentTransform
                    : (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
    ci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                      : VkCompositeAlphaFlagBitsKHR(caps.supportedCompositeAlpha & (~caps.supportedCompositeAlpha + 1));
    ci.presentMode = mode;
    ci.clipped = VK_TRUE;
    if (vkCreateSwapchainKHR(g_device, &ci, nullptr, &g_swapchain) != VK_SUCCESS) { if (error) *error = "swapchain creation failed"; return false; }
    vkGetSwapchainImagesKHR(g_device, g_swapchain, &n, nullptr);
    g_swapImages.resize(n);
    vkGetSwapchainImagesKHR(g_device, g_swapchain, &n, g_swapImages.data());
    g_presentSemaphores.resize(n);
    for (VkSemaphore& sem : g_presentSemaphores)
    {
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKCHECK(vkCreateSemaphore(g_device, &si, nullptr, &sem));
    }
    g_swapFormat = chosen.format;
    // (the log: what the window system gave; a tester's log then tells FIFO (VSync) from IMMEDIATE / MAILBOX)
    if (extent.width != g_swapExtent.width || extent.height != g_swapExtent.height || mode != g_swapMode)
        std::printf("Vulkan: swapchain %ux%u, %u images, %s\n", extent.width, extent.height, n,
                    mode == VK_PRESENT_MODE_FIFO_KHR ? "FIFO (VSync)" : mode == VK_PRESENT_MODE_IMMEDIATE_KHR ? "IMMEDIATE" : "MAILBOX");
    g_swapMode = mode;
    g_swapExtent = extent;
    g_swapchainStale = false;
    g_swapRotation = rotation;
    g_swapTransformMatches = ci.preTransform == caps.currentTransform;
    if (rotation)
    {
        for (VkImage image : g_swapImages)
        {
            VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vi.image = image;
            vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vi.format = chosen.format;
            vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            VkImageView v;
            VKCHECK(vkCreateImageView(g_device, &vi, nullptr, &v));
            g_swapViews.push_back(v);
        }
        if (g_presentPipelineFormat != chosen.format) CreatePresentPipeline(chosen.format);
    }
    std::printf("Vulkan: swapchain %ux%u (surface %ux%u, transform 0x%x, %s)\n", extent.width, extent.height, caps.currentExtent.width,
                caps.currentExtent.height, unsigned(caps.currentTransform), rotation ? "pre-rotated" : "identity");
    return true;
}

// The surface without its swapchain (Android: the activity's surface went away or failed; PollSurface makes it again).
void DropSurface()
{
    DestroySwapchain();
    if (g_surface && vkDestroySurfaceKHR) vkDestroySurfaceKHR(g_instance, g_surface, nullptr);
    g_surface = VK_NULL_HANDLE;
}
// (SetSurfaceSerial) the platform's surface replaced or gone: the swapchain and surface follow; while there is none the
// frames are rendered and submitted without a present.
void PollSurface()
{
    if (!g_surfaceSerialFn) return;
    const uint64_t serial = g_surfaceSerialFn();
    if (serial != g_surfaceSerial)
    {
        if (g_surface) DropSurface();
        g_surfaceSerial = serial;
        g_surfaceRetry = 0;
        std::printf("Vulkan: %s\n", serial ? "the window has a new surface" : "no window surface (in the background)");
    }
    if (g_surface || !serial) return;
    if (g_surfaceRetry) { --g_surfaceRetry; return; }
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (g_headlessWsi)
    {
        auto create = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(vkGetInstanceProcAddr(g_instance, "vkCreateHeadlessSurfaceEXT"));
        VkHeadlessSurfaceCreateInfoEXT hi{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
        if (create) r = create(g_instance, &hi, nullptr, &surface);
    }
    else r = glfwCreateWindowSurface(g_instance, g_window, nullptr, &surface);
    if (r != VK_SUCCESS || !surface)
    {
        std::printf("Vulkan: no surface for the window yet (%d): trying again\n", int(r));
        g_surfaceRetry = 30;
        return;
    }
    VkBool32 present = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(g_phys, g_queueFamily, surface, &present);
    if (!present)
    {
        std::printf("Vulkan: the queue cannot present to the new surface\n");
        vkDestroySurfaceKHR(g_instance, surface, nullptr);
        g_surfaceRetry = 600;
        return;
    }
    g_surface = surface;
    g_swapchainStale = true;
}

// The window's picture at the window's size (framebuffer 0), made again when the window's size changes.
void ResizeWindowImage()
{
    uint32_t w = g_outputWidth, h = g_outputHeight;
    if (g_visible && g_window)
    {
        int fbw = 0, fbh = 0;
        glfwGetFramebufferSize(g_window, &fbw, &fbh);
        if (fbw > 0 && fbh > 0) { w = uint32_t(fbw); h = uint32_t(fbh); }
    }
    if (g_windowImage && w == g_windowImage->width && h == g_windowImage->height) return;
    if (g_windowImage)
    {
        ForgetTarget(g_windowImage.get());
        ReleaseImage(g_windowImage);
        g_swapchainStale = true;
    }
#if defined(__ANDROID__)
    // (Android: also sampled, by the pre-rotated present's draw: RecordPresentDraw)
    g_windowImage = MakeImage(w, h, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false, false, VK_IMAGE_USAGE_SAMPLED_BIT);
#else
    g_windowImage = MakeImage(w, h, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false, false);
#endif
    g_outputWidth = w;
    g_outputHeight = h;
    b.targetDirty = true;
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*)
{
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) std::fprintf(stderr, "Vulkan validation: %s\n", data->pMessage);
    return VK_FALSE;
}

bool HasExtension(const std::vector<VkExtensionProperties>& list, const char* name)
{
    for (const auto& e : list) if (!std::strcmp(e.extensionName, name)) return true;
    return false;
}

std::vector<uint8_t> ReadFile(const std::string& path)
{
    std::vector<uint8_t> data;
    std::ifstream f(path, std::ios::binary);
    if (!f) return data;
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n <= 0 || n > (64 << 20)) return data;
    f.seekg(0);
    data.resize(size_t(n));
    f.read(reinterpret_cast<char*>(data.data()), n);
    if (!f) data.clear();
    return data;
}
void SavePipelineCache()
{
    if (!g_pipelineCache || g_cachePath.empty()) return;
    size_t n = 0;
    if (vkGetPipelineCacheData(g_device, g_pipelineCache, &n, nullptr) != VK_SUCCESS || !n) return;
    std::vector<uint8_t> data(n);
    if (vkGetPipelineCacheData(g_device, g_pipelineCache, &n, data.data()) != VK_SUCCESS) return;
    const std::string tmp = g_cachePath + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(reinterpret_cast<const char*>(data.data()), std::streamsize(n));
        if (!f) return;
    }
    std::remove(g_cachePath.c_str());
    std::rename(tmp.c_str(), g_cachePath.c_str());
}

void DestroyAll();
}  // namespace

// ------------------------------------------------------------------------------------------------ public
bool Initialize(GLFWwindow* window, const Options& options, std::string* error)
{
    if (g_device) return true;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        DestroyAll();
        return false;
    };
    if (!LoadLoader(error)) return false;
    uint32_t apiVersion = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&apiVersion);
    if (apiVersion < VK_API_VERSION_1_3 || FForcedFailure("oldloader")) return fail("the Vulkan loader is older than 1.3");
    g_window = window;
    g_visible = options.visible;
    // (tests: SLY2N_VKHEADLESSWSI=1: a hidden window still gets a swapchain, on VK_EXT_headless_surface, so the acquire /
    // copy / present path runs without anything on the screen)
    const bool fHeadlessWsi = !g_visible && std::getenv("SLY2N_VKHEADLESSWSI") && std::atoi(std::getenv("SLY2N_VKHEADLESSWSI")) != 0;
    if (fHeadlessWsi) g_visible = true;
    g_headlessWsi = fHeadlessWsi;
    g_swapInterval = std::clamp(options.swapInterval, 0, 1);
    g_cachePath = options.pipelineCachePath;
    g_onFatal = options.onFatal;
    std::vector<const char*> extensions;
    if (fHeadlessWsi) extensions = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
    else if (g_visible)
    {
        uint32_t n = 0;
        const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&n);
        if (!glfwExtensions) return fail("the window system offers no Vulkan surface");
        extensions.assign(glfwExtensions, glfwExtensions + n);
    }
    std::vector<const char*> layers;
    const bool fValidate = std::getenv("SLY2N_VKVALIDATE") && std::atoi(std::getenv("SLY2N_VKVALIDATE")) != 0;
    if (fValidate)
    {
        uint32_t n = 0;
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> props(n);
        vkEnumerateInstanceLayerProperties(&n, props.data());
        for (const auto& l : props)
            if (!std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation")) layers.push_back("VK_LAYER_KHRONOS_validation");
        if (layers.empty()) std::printf("Vulkan: SLY2N_VKVALIDATE: the validation layer is not installed\n");
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Sly 2 (SafeHouse)";
    app.pEngineName = "SafeHouse";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(extensions.size());
    ici.ppEnabledExtensionNames = extensions.data();
    ici.enabledLayerCount = uint32_t(layers.size());
    ici.ppEnabledLayerNames = layers.data();
    g_external = options.external;
    if (g_external)
    {
        // (VR) through the OpenXR runtime, which adds the extensions it needs
        if (g_external->createInstance(g_external->user, vkGetInstanceProcAddr, &ici, &g_instance) != VK_SUCCESS)
            return fail("the OpenXR runtime could not create the Vulkan instance");
    }
    else if (vkCreateInstance(&ici, nullptr, &g_instance) != VK_SUCCESS) return fail("vkCreateInstance failed");
    if (!LoadInstance(g_instance, error)) return fail(error ? *error : "instance entry points");
    if (fValidate && vkCreateDebugUtilsMessengerEXT)
    {
        VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
        mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        mi.pfnUserCallback = DebugCallback;
        vkCreateDebugUtilsMessengerEXT(g_instance, &mi, nullptr, &g_messenger);
    }
    if (fHeadlessWsi)
    {
        auto create = reinterpret_cast<PFN_vkCreateHeadlessSurfaceEXT>(vkGetInstanceProcAddr(g_instance, "vkCreateHeadlessSurfaceEXT"));
        VkHeadlessSurfaceCreateInfoEXT hi{VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
        if (!create || create(g_instance, &hi, nullptr, &g_surface) != VK_SUCCESS) return fail("no headless surface");
    }
    else if (g_visible && glfwCreateWindowSurface(g_instance, window, nullptr, &g_surface) != VK_SUCCESS) return fail("no Vulkan surface for the window");

    // The device: SLY2N_VKDEVICE (an index or part of the name), else the first discrete GPU, integrated, then the rest.
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g_instance, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(g_instance, &n, devices.data());
    if (devices.empty()) return fail("no Vulkan device");
    if (g_external)
    {
        // (VR) the runtime's device only
        const VkPhysicalDevice pdXr = g_external->physicalDevice(g_external->user, g_instance);
        if (!pdXr) return fail("the OpenXR runtime named no Vulkan device");
        devices.assign(1, pdXr);
    }
    const char* szWanted = std::getenv("SLY2N_VKDEVICE");
    std::string reasons;
    int bestScore = -1;
    for (uint32_t i = 0; i < devices.size(); ++i)
    {
        VkPhysicalDevice pd = devices[i];
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        auto reject = [&](const char* why) { reasons += std::string(reasons.empty() ? "" : "; ") + props.deviceName + ": " + why; };
        if (props.apiVersion < VK_API_VERSION_1_3 || FForcedFailure("olddriver")) { reject("Vulkan 1.3 is required"); continue; }
        if (FForcedFailure("feature")) { reject("no descriptor indexing (SLY2N_VKFAIL)"); continue; }
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> exts(ne);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, exts.data());
        // (push descriptors and 256 push-constant bytes where the device has them; else per-frame descriptor sets and
        // the programs' uniform-buffer variants, vk_shaders.h spirvUbo)
        const bool hasPushDescriptors = HasExtension(exts, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) && !std::getenv("SLY2N_VKNOPUSHDESC");
        if (g_visible && !HasExtension(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) { reject("no swapchain"); continue; }
        VkPhysicalDeviceExtendedDynamicState3FeaturesEXT fe3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
        const bool hasEds3 = HasExtension(exts, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME) && !std::getenv("SLY2N_VKNOEDS3");
        VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR flr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR,
                                                                 hasEds3 ? &fe3 : nullptr};
        // (the in-place read: tile GPUs (Android), or SLY2N_SHFETCH=1 (a test elsewhere); the desktop keeps its images as
        // they were, without the input-attachment use)
#if defined(__ANDROID__)
        const bool wantLocalRead = !(std::getenv("SLY2N_SHFETCH") && std::atoi(std::getenv("SLY2N_SHFETCH")) == 0);
#else
        const bool wantLocalRead = std::getenv("SLY2N_SHFETCH") && std::atoi(std::getenv("SLY2N_SHFETCH")) != 0;
#endif
        // (SLY2N_VKNOLOCALREAD=1: as a driver without it: the classic fetch, a test where the driver has local read)
        const bool hasLocalRead = wantLocalRead && HasExtension(exts, VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME) &&
                                  !(std::getenv("SLY2N_VKNOLOCALREAD") && std::atoi(std::getenv("SLY2N_VKNOLOCALREAD")) != 0);
        VkPhysicalDeviceProvokingVertexFeaturesEXT fpv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT, &flr};
        const bool hasProvoking = HasExtension(exts, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
#if defined(__ANDROID__)
        // (a tiler's GL-style MSAA: GL_EXT_multisampled_render_to_texture's Vulkan form; SLY2N_VKMSRTSS=0: real MSAA images)
        const bool hasMsrtss = HasExtension(exts, VK_EXT_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_EXTENSION_NAME) &&
                               !(std::getenv("SLY2N_VKMSRTSS") && std::atoi(std::getenv("SLY2N_VKMSRTSS")) == 0);
#else
        const bool hasMsrtss = false;
#endif
        const bool hasFdm = HasExtension(exts, VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME) &&
                            !(std::getenv("SLY2N_VKFDM") && std::atoi(std::getenv("SLY2N_VKFDM")) == 0);
        VkPhysicalDeviceFragmentDensityMapFeaturesEXT ffdm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT, &fpv};
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, hasFdm ? static_cast<void*>(&ffdm) : static_cast<void*>(&fpv)};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
        VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11};
        vkGetPhysicalDeviceFeatures2(pd, &f2);
        if (!f13.dynamicRendering || !f13.synchronization2) { reject("no dynamic rendering / synchronization2"); continue; }
        if (!f12.timelineSemaphore || !f12.hostQueryReset) { reject("no timeline semaphores"); continue; }
        if (!f12.runtimeDescriptorArray || !f12.descriptorBindingPartiallyBound || !f12.descriptorBindingSampledImageUpdateAfterBind ||
            !f12.descriptorBindingUpdateUnusedWhilePending) { reject("no descriptor indexing"); continue; }
        VkPhysicalDevicePushDescriptorPropertiesKHR pp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
        VkPhysicalDeviceFragmentDensityMapPropertiesEXT pfdm{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_PROPERTIES_EXT, &pp};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, hasFdm ? static_cast<void*>(&pfdm) : static_cast<void*>(&pp)};
        vkGetPhysicalDeviceProperties2(pd, &p2);
        const bool pushDescriptors = hasPushDescriptors && pp.maxPushDescriptors >= kMaxBindings;
        uint32_t pushBytes = std::min(props.limits.maxPushConstantsSize, kPushBytes);
        if (const char* sz = std::getenv("SLY2N_VKPUSHBYTES"); sz && std::atoi(sz) >= 128) pushBytes = std::min(pushBytes, uint32_t(std::atoi(sz)));
        if (pushBytes < 128) { reject("fewer than 128 push-constant bytes"); continue; }
        if (pushBytes < kPushBytes && props.limits.maxDescriptorSetUniformBuffersDynamic < 1) { reject("no dynamic uniform buffers"); continue; }
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, nullptr);
        std::vector<VkQueueFamilyProperties> queues(nq);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, queues.data());
        int family = -1;
        for (uint32_t q = 0; q < nq && family < 0; ++q)
        {
            if (!(queues[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) || queues[q].timestampValidBits == 0) continue;
            VkBool32 present = VK_TRUE;
            if (g_visible) vkGetPhysicalDeviceSurfaceSupportKHR(pd, q, g_surface, &present);
            if (present) family = int(q);
        }
        if (family < 0) { reject("no graphics queue that can present"); continue; }
        int score = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? 4 : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? 3
                  : props.deviceType == VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU ? 2 : 1;
        if (szWanted && *szWanted)
        {
            char* end = nullptr;
            const long index = std::strtol(szWanted, &end, 10);
            const bool fMatch = (end && !*end) ? long(i) == index : std::strstr(props.deviceName, szWanted) != nullptr;
            score += fMatch ? 100 : 0;
        }
        if (score > bestScore)
        {
            bestScore = score;
            g_phys = pd;
            g_queueFamily = uint32_t(family);
            g_props = props;
            g_feat = Features{};
            g_feat.geometry = f2.features.geometryShader;
            g_feat.dualSrcBlend = f2.features.dualSrcBlend;
            g_feat.depthBounds = f2.features.depthBounds && !(std::getenv("SLY2N_VKDEPTHBOUNDS") && std::atoi(std::getenv("SLY2N_VKDEPTHBOUNDS")) == 0);
            g_feat.wideLines = f2.features.wideLines;
            g_feat.lineWidthMax = props.limits.lineWidthRange[1];
            g_feat.anisotropy = f2.features.samplerAnisotropy;
            g_feat.anisotropyMax = props.limits.maxSamplerAnisotropy;
            g_feat.mdi = f2.features.multiDrawIndirect;
            g_feat.firstInstance = f2.features.drawIndirectFirstInstance;
            g_feat.fillModeNonSolid = f2.features.fillModeNonSolid;
            g_feat.provokingLast = hasProvoking && fpv.provokingVertexLast;
            g_feat.msrtss = hasMsrtss;
            g_feat.fdm = hasFdm && ffdm.fragmentDensityMap && ffdm.fragmentDensityMapNonSubsampledImages;
            if (g_feat.fdm) g_feat.fdmTexel = pfdm.maxFragmentDensityTexelSize;
            g_feat.demote = f13.shaderDemoteToHelperInvocation;
            g_feat.float16 = f12.shaderFloat16;
            g_feat.eds3 = hasEds3 && fe3.extendedDynamicState3ColorBlendEnable && fe3.extendedDynamicState3ColorBlendEquation &&
                          fe3.extendedDynamicState3ColorWriteMask;
            g_feat.nonUniform = f12.shaderSampledImageArrayNonUniformIndexing;
            g_feat.pushDescriptors = pushDescriptors;
            g_feat.pushBytes = pushBytes;
            g_uniformAlign = uint32_t(std::max<VkDeviceSize>(props.limits.minUniformBufferOffsetAlignment, 16));
            // (the in-place read's input attachment is pushed after the storage buffers: push descriptors only)
            g_feat.localRead = hasLocalRead && flr.dynamicRenderingLocalRead && pushDescriptors && pp.maxPushDescriptors > kMaxBindings;
            g_feat.classicFetch = !g_feat.localRead && wantLocalRead && f12.imagelessFramebuffer && pushDescriptors &&
                                  pp.maxPushDescriptors > kMaxBindings &&
                                  !(std::getenv("SLY2N_VKCLASSICFETCH") && std::atoi(std::getenv("SLY2N_VKCLASSICFETCH")) == 0);
            g_feat.samples = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts &
                             props.limits.framebufferStencilSampleCounts;
            // (VR's two views in one pass; SLY2N_VKMULTIVIEW=0: not offered)
            g_feat.multiview = f11.multiview && !(std::getenv("SLY2N_VKMULTIVIEW") && std::atoi(std::getenv("SLY2N_VKMULTIVIEW")) == 0);
            g_feat.multiviewGeometry = g_feat.multiview && f11.multiviewGeometryShader;
        }
    }
    if (!g_phys) return fail("no suitable Vulkan device (" + reasons + ")");
    for (VkFormat f : {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT})
    {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(g_phys, f, &fp);
        const bool ok = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
        if (f == VK_FORMAT_D32_SFLOAT_S8_UINT && !ok) g_feat.depthStencil = VK_FORMAT_D24_UNORM_S8_UINT;
        if (f == VK_FORMAT_D24_UNORM_S8_UINT && !ok) g_feat.depth24 = g_feat.depthStencil;
    }
#if defined(__ANDROID__)
    // (Android: D24S8 where it attaches, as OpenGL ES there: a tiler keeps fewer bytes a pixel on chip, so bigger tiles
    // (the Quest 3S's eyes in 8 tiles, not 12: Paris 36 -> 42 fps); SLY2N_VKD24=0: D32S8)
    if (g_feat.depth24 == VK_FORMAT_D24_UNORM_S8_UINT && !(std::getenv("SLY2N_VKD24") && std::atoi(std::getenv("SLY2N_VKD24")) == 0))
        g_feat.depthStencil = VK_FORMAT_D24_UNORM_S8_UINT;
#endif
    // (one depth format for every target: the pipelines' depth format is the device's)
    g_feat.depth24 = g_feat.depthStencil;
    vkGetPhysicalDeviceMemoryProperties(g_phys, &g_memProps);

    const float priority = 1.0f;
    VkDeviceQueueCreateInfo qi{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qi.queueFamilyIndex = g_queueFamily;
    qi.queueCount = 1;
    qi.pQueuePriorities = &priority;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT fe3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
    fe3.extendedDynamicState3ColorBlendEnable = fe3.extendedDynamicState3ColorBlendEquation = fe3.extendedDynamicState3ColorWriteMask = VK_TRUE;
    VkPhysicalDeviceProvokingVertexFeaturesEXT fpv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
    fpv.provokingVertexLast = VK_TRUE;
    void* chain = nullptr;
    VkPhysicalDeviceMultisampledRenderToSingleSampledFeaturesEXT fms{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_FEATURES_EXT};
    fms.multisampledRenderToSingleSampled = VK_TRUE;
    if (g_feat.msrtss) fms.pNext = chain, chain = &fms;
    VkPhysicalDeviceFragmentDensityMapFeaturesEXT ffdmOn{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT};
    ffdmOn.fragmentDensityMap = ffdmOn.fragmentDensityMapNonSubsampledImages = VK_TRUE;
    if (g_feat.fdm) ffdmOn.pNext = chain, chain = &ffdmOn;
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR flr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR};
    flr.dynamicRenderingLocalRead = VK_TRUE;
    if (g_feat.localRead) flr.pNext = chain, chain = &flr;
    if (g_feat.eds3) fe3.pNext = chain, chain = &fe3;
    if (g_feat.provokingLast) fpv.pNext = chain, chain = &fpv;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, chain};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.shaderDemoteToHelperInvocation = g_feat.demote;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceVulkan11Features f11{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12};
    f11.multiview = g_feat.multiview;
    f11.multiviewGeometryShader = g_feat.multiviewGeometry;
    f12.timelineSemaphore = VK_TRUE;
    f12.hostQueryReset = VK_TRUE;
    f12.shaderFloat16 = g_feat.float16;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = g_feat.nonUniform;
    f12.imagelessFramebuffer = g_feat.classicFetch;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, g_feat.multiview ? static_cast<void*>(&f11) : static_cast<void*>(&f12)};
    f2.features.geometryShader = g_feat.geometry;
    f2.features.dualSrcBlend = g_feat.dualSrcBlend;
    f2.features.depthBounds = g_feat.depthBounds;
    f2.features.wideLines = g_feat.wideLines;
    f2.features.samplerAnisotropy = g_feat.anisotropy;
    f2.features.multiDrawIndirect = g_feat.mdi;
    f2.features.drawIndirectFirstInstance = g_feat.firstInstance;
    f2.features.fillModeNonSolid = g_feat.fillModeNonSolid;
    f2.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    std::vector<const char*> deviceExtensions;
    if (g_feat.pushDescriptors) deviceExtensions.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    if (g_visible) deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (g_feat.provokingLast) deviceExtensions.push_back(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
    if (g_feat.eds3) deviceExtensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    if (g_feat.msrtss) deviceExtensions.push_back(VK_EXT_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_EXTENSION_NAME);
    if (g_feat.fdm) deviceExtensions.push_back(VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME);
    if (g_feat.localRead) deviceExtensions.push_back(VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME);
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = uint32_t(deviceExtensions.size());
    dci.ppEnabledExtensionNames = deviceExtensions.data();
    if (FForcedFailure("device")) return fail("vkCreateDevice failed");
    if (g_external ? g_external->createDevice(g_external->user, vkGetInstanceProcAddr, g_phys, &dci, &g_device) != VK_SUCCESS
                   : vkCreateDevice(g_phys, &dci, nullptr, &g_device) != VK_SUCCESS)
        return fail(g_external ? "the OpenXR runtime could not create the Vulkan device" : "vkCreateDevice failed");
    if (!LoadDevice(g_device, error)) return fail(error ? *error : "device entry points");
    if (g_feat.pushDescriptors && !vkCmdPushDescriptorSetKHR) return fail("no vkCmdPushDescriptorSetKHR");
    if (!vkCmdSetColorBlendEnableEXT || !vkCmdSetColorBlendEquationEXT || !vkCmdSetColorWriteMaskEXT) g_feat.eds3 = false;
    g_vkResetQueryPool = reinterpret_cast<PFN_vkResetQueryPool>(vkGetDeviceProcAddr(g_device, "vkResetQueryPool"));
    vkGetDeviceQueue(g_device, g_queueFamily, 0, &g_queue);
    g_stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | (g_feat.geometry ? VK_SHADER_STAGE_GEOMETRY_BIT : 0);

    // memory: the buffers in host-visible memory, device-local where the driver offers it (else system memory)
    {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = 65536;
        bi.usage = kBufferUsage;
        VkBuffer probe;
        VKCHECK(vkCreateBuffer(g_device, &bi, nullptr, &probe));
        VkMemoryRequirements req;
        vkGetBufferMemoryRequirements(g_device, probe, &req);
        vkDestroyBuffer(g_device, probe, nullptr);
        const VkMemoryPropertyFlags visible = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
        const int fast = FindMemoryType(req.memoryTypeBits, visible | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        const int slow = FindMemoryType(req.memoryTypeBits, visible);
        if (slow < 0) return fail("no host-visible memory for buffers");
        if (std::getenv("SLY2N_VKSYSMEM")) g_bufferHeap = std::make_unique<Heap>(uint32_t(slow), kBufferBlockSize, true);
        else
        {
            g_bufferHeap = std::make_unique<Heap>(uint32_t(fast >= 0 ? fast : slow), kBufferBlockSize, true);
            if (fast >= 0 && fast != slow) g_bufferHeapFallback = std::make_unique<Heap>(uint32_t(slow), kBufferBlockSize, true);
        }
        int cached = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        g_readbackHeap = std::make_unique<Heap>(uint32_t(cached >= 0 ? cached : slow), 16ull << 20, true);
    }

    // descriptors: set 0, every texture and sampler (bound once per command buffer); set 1, the storage buffers (pushed)
    {
        const uint32_t images = std::min(kMaxImages, g_props.limits.maxPerStageDescriptorSampledImages > kMaxArrays + 16 ?
                                                     g_props.limits.maxPerStageDescriptorSampledImages - kMaxArrays - 16 : kMaxImages);
        (void)images;
        VkDescriptorSetLayoutBinding bindings[3] = {
            {0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxImages, g_stages, nullptr},
            {1, VK_DESCRIPTOR_TYPE_SAMPLER, kMaxSamplers, g_stages, nullptr},
            {2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxArrays, g_stages, nullptr}};
        const VkDescriptorBindingFlags flag = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                              VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
        const VkDescriptorBindingFlags flags[3] = {flag, flag, flag};
        VkDescriptorSetLayoutBindingFlagsCreateInfo fi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
        fi.bindingCount = 3;
        fi.pBindingFlags = flags;
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.pNext = &fi;
        li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
        li.bindingCount = 3;
        li.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(g_device, &li, nullptr, &g_setLayoutTextures) != VK_SUCCESS) return fail("descriptor set layout");
        std::array<VkDescriptorSetLayoutBinding, kMaxBindings + 1> sb{};
        for (uint32_t i = 0; i < kMaxBindings; ++i) sb[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, g_stages, nullptr};
        sb[kFetchBinding] = {kFetchBinding, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo bli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        bli.flags = g_feat.pushDescriptors ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR : 0;
        bli.bindingCount = kMaxBindings + (g_feat.localRead || g_feat.classicFetch ? 1 : 0);
        bli.pBindings = sb.data();
        if (vkCreateDescriptorSetLayout(g_device, &bli, nullptr, &g_setLayoutBuffers) != VK_SUCCESS) return fail("push descriptor set layout");
        if (g_feat.pushBytes < kPushBytes)
        {
            const VkDescriptorSetLayoutBinding ub{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, g_stages, nullptr};
            VkDescriptorSetLayoutCreateInfo uli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
            uli.bindingCount = 1;
            uli.pBindings = &ub;
            if (vkCreateDescriptorSetLayout(g_device, &uli, nullptr, &g_setLayoutUniforms) != VK_SUCCESS) return fail("uniform set layout");
        }
        const VkDescriptorSetLayout sets[3] = {g_setLayoutTextures, g_setLayoutBuffers, g_setLayoutUniforms};
        const VkPushConstantRange range{g_stages, 0, g_feat.pushBytes};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = g_setLayoutUniforms ? 3 : 2;
        pli.pSetLayouts = sets;
        pli.pushConstantRangeCount = 1;
        pli.pPushConstantRanges = &range;
        if (vkCreatePipelineLayout(g_device, &pli, nullptr, &g_pipelineLayout) != VK_SUCCESS) return fail("pipeline layout");
        const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, kMaxImages + kMaxArrays}, {VK_DESCRIPTOR_TYPE_SAMPLER, kMaxSamplers}};
        VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpi.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
        dpi.maxSets = 1;
        dpi.poolSizeCount = 2;
        dpi.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(g_device, &dpi, nullptr, &g_descriptorPool) != VK_SUCCESS) return fail("descriptor pool");
        VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dai.descriptorPool = g_descriptorPool;
        dai.descriptorSetCount = 1;
        dai.pSetLayouts = &g_setLayoutTextures;
        if (vkAllocateDescriptorSets(g_device, &dai, &g_textureSet) != VK_SUCCESS) return fail("descriptor set");
    }
    // (pipeline prewarm: the manifest of the pipelines made before, for this device / driver / feature set)
    {
        const char* sz = std::getenv("SLY2N_VKPREWARM");
        g_prewarmOn = !g_cachePath.empty() && !(sz && std::atoi(sz) == 0);
        const char* szSpec = std::getenv("SLY2N_VKSPEC");
        // (off by default: on RDNA the extra pipeline binds cost more than the folded branches save, notes/VKGPU)
        g_specAllowed = !szSpec ? 0u : uint32_t(std::strtoul(szSpec, nullptr, 16)) & 0xFFu;
        g_specSets.assign(1, SpecData{});
        VkPhysicalDeviceProperties pp;
        vkGetPhysicalDeviceProperties(g_phys, &pp);
        const uint32_t ids[4] = {pp.vendorID, pp.deviceID, pp.driverVersion, pp.apiVersion};
        g_deviceSignature = Fnv64(ids, sizeof ids);
        const uint32_t feats[] = {g_feat.eds3, g_feat.dualSrcBlend, g_feat.msrtss, g_feat.fdm, g_feat.provokingLast, g_feat.classicFetch,
                                  g_feat.localRead, g_feat.multiview, g_feat.pushBytes, uint32_t(g_feat.depthStencil), g_feat.geometry};
        g_deviceSignature = Fnv64(feats, sizeof feats, g_deviceSignature);
        g_manifest.clear(); g_manifestIndex.clear(); g_manifestSaved = 0; g_scopesWarmed.clear(); g_pipeScopeMark.clear();
        g_scope = INT32_MIN;
        if (g_prewarmOn) LoadManifest();
    }
    // the pipeline cache from the last run
    {
        std::vector<uint8_t> data = g_cachePath.empty() ? std::vector<uint8_t>{} : ReadFile(g_cachePath);
        VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        pci.initialDataSize = data.size();
        pci.pInitialData = data.empty() ? nullptr : data.data();
        if (vkCreatePipelineCache(g_device, &pci, nullptr, &g_pipelineCache) != VK_SUCCESS)
        {
            pci.initialDataSize = 0;
            pci.pInitialData = nullptr;
            vkCreatePipelineCache(g_device, &pci, nullptr, &g_pipelineCache);
        }
    }
    {
        VkSemaphoreTypeCreateInfo ti{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        ti.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        si.pNext = &ti;
        if (vkCreateSemaphore(g_device, &si, nullptr, &g_timeline) != VK_SUCCESS) return fail("timeline semaphore");
        g_submitValue = 0;
    }
    {
        VkQueryPoolCreateInfo qpi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qpi.queryType = VK_QUERY_TYPE_TIMESTAMP;
        qpi.queryCount = kQueries * 2;
        if (vkCreateQueryPool(g_device, &qpi, nullptr, &g_queryPool) == VK_SUCCESS)
            for (uint32_t i = kQueries; i-- > 0;) g_freeQueries.push_back(i * 2);
        g_gpuTimeEvery = std::getenv("SLY2N_VKGPUTIME") ? std::max(0, std::atoi(std::getenv("SLY2N_VKGPUTIME"))) : 0;
        qpi.queryCount = kFrameSlots * kPassTimes * 2;
        if (g_gpuTimeEvery > 0 && g_props.limits.timestampComputeAndGraphics &&
            vkCreateQueryPool(g_device, &qpi, nullptr, &g_passPool) == VK_SUCCESS)
            g_vkResetQueryPool(g_device, g_passPool, 0, qpi.queryCount);
        qpi.queryCount = kFrameSlots * kMarks;
        if (g_passPool && std::getenv("SLY2N_VKGPUMARKS") && std::atoi(std::getenv("SLY2N_VKGPUMARKS")) != 0 &&
            vkCreateQueryPool(g_device, &qpi, nullptr, &g_markPool) == VK_SUCCESS)
            g_vkResetQueryPool(g_device, g_markPool, 0, qpi.queryCount);
    }
    for (Frame& f : g_frames)
    {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
        pi.queueFamilyIndex = g_queueFamily;
        if (vkCreateCommandPool(g_device, &pi, nullptr, &f.pool) != VK_SUCCESS) return fail("command pool");
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(g_device, &si, nullptr, &f.acquire) != VK_SUCCESS) return fail("semaphore");
    }
    g_nullStorage = AllocBuffer(256);
    std::memset(g_nullStorage.Cpu(), 0, 256);
    g_generic = AllocBuffer(512);
    for (unsigned i = 0; i < 16; ++i)
    {
        const float fv[4] = {0, 0, 0, 1};
        const int32_t iv[4] = {0, 0, 0, 1};
        std::memcpy(g_generic.Cpu() + i * 16, fv, 16);
        std::memcpy(g_generic.Cpu() + 256 + i * 16, iv, 16);
    }

    VkPhysicalDeviceDriverProperties driver{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &driver};
    vkGetPhysicalDeviceProperties2(g_phys, &p2);
    g_deviceName = g_props.deviceName;
    char sz[512];
    std::snprintf(sz, sizeof sz, "%s (Vulkan %u.%u, %s %s)", g_props.deviceName, VK_API_VERSION_MAJOR(g_props.apiVersion),
                  VK_API_VERSION_MINOR(g_props.apiVersion), driver.driverName, driver.driverInfo);
    g_rendererString = sz;
    g_extensions = {"GL_ARB_clip_control", "GL_ARB_multi_draw_indirect", "GL_ARB_base_instance", "GL_ARB_shader_storage_buffer_object"};
    if (g_feat.anisotropy) g_extensions.push_back("GL_EXT_texture_filter_anisotropic"), g_extensions.push_back("GL_ARB_texture_filter_anisotropic");
    if (g_feat.nonUniform) g_extensions.push_back("GL_ARB_bindless_texture");

    s = State{};
    g_fixedPacket = 0;
    g_packetFixed.resize(1);
    g_packetStates.resize(1);
    {
        int fbw = 0, fbh = 0;
        if (window) glfwGetWindowSize(window, &fbw, &fbh);
        if (g_visible && window) glfwGetFramebufferSize(window, &fbw, &fbh);
        g_outputWidth = uint32_t(std::max(fbw, 1));
        g_outputHeight = uint32_t(std::max(fbh, 1));
    }
    s.viewport[2] = s.scissor[2] = GLint(g_outputWidth);
    s.viewport[3] = s.scissor[3] = GLint(g_outputHeight);
    g_arrays.Clear();
    g_arrays.Default();
    InvalidateBound();
    g_frameIndex = 0;
    g_recording = false;
    g_threaded = !options.singleThread && !(std::getenv("SLY2N_VKTHREAD") && std::atoi(std::getenv("SLY2N_VKTHREAD")) == 0);
    if (g_threaded)
    {
        g_streamMain = new Stream;
        g_streamPre = new Stream;
        g_workerDone = 0;
        g_worker = std::thread([fn = options.onWorkerThread] {
            if (fn) fn();
            WorkerLoop();
        });
    }
    // (a game that ends with exit() without Shutdown: the worker is stopped before the statics it waits on are
    // destroyed (a joinable std::thread or a condition variable with a waiter at exit hangs or aborts), and the
    // pipeline cache is kept)
    static bool s_fAtExit = false;
    if (!s_fAtExit)
    {
        s_fAtExit = true;
        std::atexit([] {
            if (!g_device) return;
            StopWorker();
            StopPrewarm();
            if (g_external) return;   // (VR: the OpenXR runtime owns the device)
            vkDeviceWaitIdle(g_device);
            SaveManifest();
            SavePipelineCache();
        });
    }
    BeginFrame();
    g_blackTexture = MakeBlack(false);
    g_blackArray = MakeBlack(true);
    g_clearProgram = CreateProgram("clear.vert", nullptr, "clear.frag", "");
#if defined(__ANDROID__)
    g_presentProgram = CreateProgram("present.vert", nullptr, "present.frag", "");
#endif
    if (g_visible)
    {
        std::string why;
        if (FForcedFailure("swapchain")) why = "vkCreateSwapchainKHR failed (SLY2N_VKFAIL)";
        if (!why.empty() || !CreateSwapchain(&why)) return fail(why);
    }
    const bool fBuffersLocal = (g_memProps.memoryTypes[g_bufferHeap->Type()].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
    std::printf("Vulkan: %s%s%s%s\n", g_rendererString.c_str(), fBuffersLocal ? "" : " (buffers in system memory)",
                g_feat.pushDescriptors ? "" : " (descriptor sets: no push descriptors)",
                g_feat.pushBytes < kPushBytes ? " (uniform buffers for the large blocks: few push-constant bytes)" : "");
    return true;
}

bool Initialized() { return g_device != VK_NULL_HANDLE; }
const cane::draw::Packets* DrawPackets() { return &g_packets; }

void PrewarmPipelines(int32_t scope, unsigned waitMs)
{
    if (!g_device || !g_prewarmOn) return;
    if (scope != g_scope) { g_scope = scope; ++g_scopeSerial; }
    if (std::find(g_scopesWarmed.begin(), g_scopesWarmed.end(), scope) != g_scopesWarmed.end()) return;
    g_scopesWarmed.push_back(scope);
    const auto t0 = std::chrono::steady_clock::now();
    // (the live programs by their manifest hash)
    std::unordered_map<uint32_t, std::vector<const Program*>> programs;
    for (GLuint name = 1; name < g_programs.Limit(); ++name)
        if (const Program* p = g_programs.Get(name); p && p->info) programs[ProgramHash(*p)].push_back(p);
    for (const auto& pp : g_prePrograms) programs[ProgramHash(*pp)].push_back(pp.get());
    // (a program the game makes on first use, later in the frame: made now, CreateProgram hands it over then)
    for (const ManifestEntry& e : g_manifest)
    {
        if (e.scope != scope || programs.count(e.program)) continue;
        for (size_t i = 0; i < Shaders::programCount; ++i)
        {
            const Shaders::Program* info = &Shaders::programs[i];
            if (!FProgramInUse(*info)) continue;
            const bool ub = info->pushSize > g_feat.pushBytes;
            if (info->pushSize > kPushBytes || (ub && !info->spirvUbo[0]) || (info->geom && !g_feat.geometry)) continue;
            if (ProgramHashOf(info, ub) != e.program) continue;
            g_prePrograms.push_back(MakeProgram(info, ub));
            programs[e.program].push_back(g_prePrograms.back().get());
            break;
        }
    }
    std::vector<PrewarmJob> jobs;
    for (const ManifestEntry& e : g_manifest)
    {
        if (e.scope != scope) continue;
        auto it = programs.find(e.program);
        if (it == programs.end()) continue;
        for (const Program* p : it->second)
        {
            PrewarmJob job;
            job.key = e.key;
            job.key.program = p->serial;
            job.key.layout = InternLayout(e.layout);
            job.key.spec = (e.spec.mask & ~(p->info->specMask & g_specAllowed)) ? 0xFFFF : InternSpec(e.spec);
            if (job.key.spec == 0xFFFF) continue;   // (specialization off now, or another subset)
            {
                std::lock_guard<std::mutex> lock(g_pipeMutex);
                if (g_pipelines.count(job.key)) continue;
            }
            if ((job.key.pad[2] & 2) && !g_feat.multiview) continue;
            job.source.modules = p->modules, job.source.modulesMv = p->modulesMv;
            job.source.layout = e.layout;
            job.source.classic = ClassicPassFor(job.key);
            job.source.spec = e.spec;
            jobs.push_back(job);
        }
    }
    if (!jobs.empty())
    {
        {
            std::lock_guard<std::mutex> lock(g_prewarmMutex);
            for (PrewarmJob& j : jobs) g_prewarmJobs.push_back(j);
        }
        if (!g_prewarmThread.joinable()) g_prewarmThread = std::thread(PrewarmLoop);
        g_prewarmCv.notify_all();
    }
    const bool done = jobs.empty() || WaitPrewarm(waitMs);
    static const bool s_fLog = std::getenv("SLY2N_VKPIPELOG") && std::atoi(std::getenv("SLY2N_VKPIPELOG")) != 0;
    if (s_fLog || !jobs.empty())
        std::fprintf(s_fLog ? stderr : stdout, "Vulkan: scope %d: %zu pipelines from the manifest%s (%.1f ms at the load)\n", int(scope),
                     jobs.size(), done ? "" : ", the rest while the game runs", MsSince(t0));
    // (the load's moment: the driver cache and the manifest written, so a run that is killed later keeps them)
    if (done && (g_manifest.size() != g_manifestSaved))
    {
        SaveManifest();
        SavePipelineCache();
    }
}
const char* DeviceName() { return g_deviceName.c_str(); }
bool FramebufferFetchUsable()
{
    if (!g_feat.localRead && !g_feat.classicFetch) return false;
    unsigned level, layer;
    const Image* im = ColorImageOf(s.drawFramebuffer, &level, &layer);
    return im && im->inputAttachment;
}
bool DepthBoundsSupported() { return g_feat.depthBounds; }
bool GpuMarksOn() { return g_markPool != VK_NULL_HANDLE; }
void GpuMark(const char* label)
{
    if (!g_markPool || !g_recording) return;
    Frame& f = Cur();
    if (f.markLabels.size() >= kMarks) return;
    vkCmdWriteTimestamp(Main(), VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_markPool, g_frameIndex * kMarks + uint32_t(f.markLabels.size()));
    f.markLabels.push_back(label);
}
Stats TakeStats()
{
    Stats st = g_stats;
    st.splits = g_splits;
    g_stats = Stats{};
    g_splits = 0;
    return st;
}

void SetSurfaceSerial(uint64_t (*serial)())
{
    g_surfaceSerialFn = serial;
    g_surfaceSerial = serial && g_surface ? serial() : 0;
}

void SetSwapInterval(int interval)
{
    const int v = std::clamp(interval, 0, 1);
    if (v == g_swapInterval) return;
    g_swapInterval = v;
    g_swapchainStale = true;
}

void Present()
{
    if (!g_device) return;
    if (!g_recording) BeginFrame();
    EndRendering();
    FlushCopies();
    GpuMark("frame end");
    Frame& f = Cur();
    if (g_visible)
    {
        PollSurface();
        if (g_surface && (g_swapchainStale || !g_swapchain))
        {
            // (a new size or present mode: the frame's commands first, then the swapchain again)
            Submit();
            DestroySwapchain();
            std::string why;
            if (!CreateSwapchain(&why))
            {
                // (Android: the surface may be gone before the platform said so: it is made again in a moment)
                if (!g_surfaceSerialFn) Fatal("%s", why.c_str());
                std::printf("Vulkan: %s: the surface is made again\n", why.c_str());
                DropSurface();
                g_surfaceRetry = 30;
            }
        }
        if (g_threaded)
        {
            // (the worker acquires, copies and presents: the swapchain is its alone while it runs)
            if (g_presentStale.exchange(false)) g_swapchainStale = true;
            Image& win = *g_windowImage;
            WindowPicture pic{win.image, win.width, win.height};   // (the main thread may replace the window's image)
            const VkImageLayout layout = g_swapRotation ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            if (g_swapRotation)
            {
                // (pre-rotation: the picture sampled through a view and a descriptor slot of its own)
                if (!win.view)
                {
                    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
                    vi.image = win.image;
                    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
                    vi.format = win.format;
                    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    VKCHECK(vkCreateImageView(g_device, &vi, nullptr, &win.view));
                    WriteImageDescriptor(win);
                }
                SamplerParams sp;
                const bool same = win.width == (g_swapRotation & 1 ? g_swapExtent.height : g_swapExtent.width) &&
                                  win.height == (g_swapRotation & 1 ? g_swapExtent.width : g_swapExtent.height);
                sp.minFilter = sp.magFilter = same ? GL_NEAREST : GL_LINEAR;
                sp.wrapS = sp.wrapT = GL_CLAMP_TO_EDGE;
                sp.maxLevel = 0;
                pic.rotation = g_swapRotation;
                pic.imageSlot = uint32_t(win.slot);
                pic.samplerSlot = uint32_t(SamplerSlot(sp));
            }
            Barriers bar;
            bar.Image(win, win.layout, layout);
            win.layout = layout;
            bar.Flush(g_main);
            win.useSegment = g_segment;
            g_streamMain->Rec([pic](VkCommandBuffer c) { RecordSwapBlit(c, g_workerIndex, pic); });
            g_pendingPresent.present = g_swapchain != VK_NULL_HANDLE;
            Submit();
        }
        uint32_t index = 0;
        VkResult r = g_threaded ? VK_NOT_READY
                   : g_swapchain ? vkAcquireNextImageKHR(g_device, g_swapchain, UINT64_MAX, f.acquire, VK_NULL_HANDLE, &index)
                                 : VK_ERROR_OUT_OF_DATE_KHR;
        if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)
        {
            if (r == VK_SUBOPTIMAL_KHR && (kSuboptimalStale || g_swapTransformMatches)) g_swapchainStale = true;
            VkCommandBuffer cmd = g_main;
            Image& win = *g_windowImage;
            Barriers bar;
            bar.Image(win, win.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            win.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            VkImageMemoryBarrier2 sw{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            sw.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            sw.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            sw.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            sw.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            sw.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            sw.srcQueueFamilyIndex = sw.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            sw.image = g_swapImages[index];
            sw.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            bar.images.push_back(sw);
            bar.Flush(cmd);
            // (GL's rows bottom first: flipped into the swapchain's, top first)
            VkImageBlit blit{};
            blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.srcOffsets[1] = {int32_t(win.width), int32_t(win.height), 1};
            blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            blit.dstOffsets[0] = {0, int32_t(g_swapExtent.height), 0};
            blit.dstOffsets[1] = {int32_t(g_swapExtent.width), 0, 1};
            const bool same = win.width == g_swapExtent.width && win.height == g_swapExtent.height;
            vkCmdBlitImage(cmd, win.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_swapImages[index], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           1, &blit, same ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
            sw.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
            sw.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            sw.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            sw.dstAccessMask = 0;
            sw.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            sw.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            bar.images.push_back(sw);
            bar.Flush(cmd);
            win.useSegment = g_segment;
            const VkSemaphore present = g_presentSemaphores[index];
            Submit(f.acquire, present);
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &present;
            pi.swapchainCount = 1;
            pi.pSwapchains = &g_swapchain;
            pi.pImageIndices = &index;
            r = vkQueuePresentKHR(g_queue, &pi);
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_ERROR_SURFACE_LOST_KHR || (r == VK_SUBOPTIMAL_KHR && (kSuboptimalStale || g_swapTransformMatches)))
                g_swapchainStale = true;
            else if (r == VK_ERROR_DEVICE_LOST) Fatal("the GPU device was lost");
        }
        else if (!g_threaded)
        {
            g_swapchainStale = true;
            Submit();
        }
    }
    else Submit();
    // (SLY2N_VKSTATS=N: every N frames the per-frame averages of the counters, stderr)
    static const int s_cStats = std::getenv("SLY2N_VKSTATS") ? std::atoi(std::getenv("SLY2N_VKSTATS")) : 0;
    static int s_iStats = 0;
    static Stats s_sum;
    if (s_cStats > 0)
    {
        s_sum.renderPasses += g_stats.renderPasses; s_sum.pipelines += g_stats.pipelines; s_sum.draws += g_stats.draws;
        s_sum.pipelinesCreated += g_stats.pipelinesCreated; s_sum.submits += g_stats.submits; s_sum.splits += g_splits;
        s_sum.pushes += g_stats.pushes; s_sum.pushBytes += g_stats.pushBytes; s_sum.descriptorPushes += g_stats.descriptorPushes;
        g_stats = Stats{};
        g_splits = 0;
        if (++s_iStats == s_cStats)
        {
            const double n = s_cStats;
            std::fprintf(stderr, "vkstats: per frame %.1f draws, %.1f pipeline binds (%.2f created), %.1f render passes (%.2f split by "
                         "copies), %.2f submits, %.1f pushes (%.0f bytes), %.1f descriptor pushes, %.2f stores NONE, %.2f depth stores dropped\n", s_sum.draws / n, s_sum.pipelines / n,
                         s_sum.pipelinesCreated / n, s_sum.renderPasses / n, s_sum.splits / n, s_sum.submits / n, s_sum.pushes / n,
                         s_sum.pushBytes / n, s_sum.descriptorPushes / n, g_storesNone / n, g_storesDropped / n);
            g_storesNone = 0, g_storesDropped = 0;
            s_sum = Stats{};
            s_iStats = 0;
        }
    }
    ResLogFrame();
    {
        static const bool s_fPipeLog = std::getenv("SLY2N_VKPIPELOG") && std::atoi(std::getenv("SLY2N_VKPIPELOG")) != 0;
        if (g_pipeCountFrame)
        {
            if (s_fPipeLog) std::fprintf(stderr, "vkpipe: frame %llu: %u pipelines made in %.2f ms\n", (unsigned long long)g_frameSerial,
                                         g_pipeCountFrame, g_pipeMsFrame);
            g_pipeMsTotal += g_pipeMsFrame, g_pipeCountTotal += g_pipeCountFrame;
            g_pipeMsMaxFrame = std::max(g_pipeMsMaxFrame, g_pipeMsFrame);
        }
        g_pipeMsFrame = 0, g_pipeCountFrame = 0;
        ++g_frameSerial;
        // (new pipelines noted and none for 10 s of frames: the manifest and the driver cache written on the worker, so
        // a run that never exits cleanly (Android, a crash) keeps them; the exit writes them again)
        if (g_prewarmOn && g_manifest.size() != g_manifestSaved && g_frameSerial - g_manifestNoteFrame >= 600)
        {
            PrewarmJob job;
            job.save = std::make_shared<std::vector<ManifestEntry>>(g_manifest);
            {
                std::lock_guard<std::mutex> lock(g_prewarmMutex);
                g_prewarmJobs.push_back(job);
            }
            if (!g_prewarmThread.joinable()) g_prewarmThread = std::thread(PrewarmLoop);
            g_prewarmCv.notify_all();
            g_manifestSaved = g_manifest.size();
        }
    }
    // (the frame's last submission is the one Submit just made; the segment it began is dropped with the frame)
    g_recording = false;
    g_frameIndex = (g_frameIndex + 1) % kFrameSlots;
}

Handles DeviceHandles() { return Handles{g_instance, g_phys, g_device, g_queueFamily, 0}; }

GLuint WrapImage(VkImage image, VkFormat viewFormat, uint32_t width, uint32_t height, uint32_t layers, uint32_t samples)
{
    auto im = std::make_unique<Image>();
    im->image = image;
    im->external = true;
    im->format = viewFormat;
    im->width = width, im->height = height, im->layers = layers, im->levels = 1, im->samples = samples;
    im->array = layers > 1;
    im->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    const GLuint name = g_textures.Gen();
    g_textures.Ensure(name);
    g_textures.Get(name)->image = std::move(im);
    return name;
}

void SetImageAttributes(GLuint texture, uint32_t renderSamples, bool inputAttachment, VkImageUsageFlags usage)
{
    Texture* t = g_textures.Get(texture);
    if (!t || !t->image) return;
    t->image->renderSamples = uint8_t(g_feat.msrtss ? std::max(1u, renderSamples) : 1u);
    t->image->inputAttachment = inputAttachment && (g_feat.localRead || g_feat.classicFetch) && t->image->renderSamples <= 1;
    t->image->usage = usage;
}
bool CanReadInPlace() { return g_feat.localRead || g_feat.classicFetch; }

// (VR's fixed foveation) density maps: R8G8 images of one texel per maxFragmentDensityTexelSize of the target
std::vector<std::unique_ptr<Image>> g_densityMaps;
bool DensityMapsSupported() { return g_feat.fdm; }
GLuint CreateDensityMap(uint32_t width, uint32_t height, uint32_t layers)
{
    if (!g_feat.fdm || !g_device) return 0;
    if (!g_recording) BeginFrame();
    auto im = std::make_unique<Image>();
    im->width = std::max(1u, (width + g_feat.fdmTexel.width - 1) / g_feat.fdmTexel.width);
    im->height = std::max(1u, (height + g_feat.fdmTexel.height - 1) / g_feat.fdmTexel.height);
    im->format = VK_FORMAT_R8G8_UNORM;
    im->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    im->fdm = true;
    im->layers = std::max(1u, layers);   // (multiview: one a view)
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = im->format;
    ci.extent = {im->width, im->height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = im->layers;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    im->usage = ci.usage;
    ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VKCHECK(vkCreateImage(g_device, &ci, nullptr, &im->image));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(g_device, im->image, &req);
    int type = FindMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0) type = FindMemoryType(req.memoryTypeBits, 0);
    auto& heap = g_imageHeaps[uint32_t(type)];
    if (!heap) heap = std::make_unique<Heap>(uint32_t(type), kImageBlockSize, false);
    if (!heap->Allocate(req.size, req.alignment, &im->memory)) return 0;
    VKCHECK(vkBindImageMemory(g_device, im->image, im->memory.block->memory, im->memory.offset));
    g_densityMaps.push_back(std::move(im));
    return GLuint(g_densityMaps.size());
}
// Its densities: full within the inner ring around (cx, cy) (the lens's centre in the drawn rectangle's -1..1), then a half
// and a quarter outwards by the level (1 low, 2 medium, 3 high; 0: full everywhere); usedW / usedH: the part of the target
// drawn (the eye resolution), the rest at full density
void FillDensityMap(GLuint map, uint32_t usedW, uint32_t usedH, float cx, float cy, int level, uint32_t layer)
{
    if (!map || map > g_densityMaps.size()) return;
    Image& im = *g_densityMaps[map - 1];
    if (layer >= im.layers) return;
    if (!g_recording) BeginFrame();
    static const float s_ar[4][2] = {{9.0f, 9.0f}, {0.75f, 9.0f}, {0.6f, 0.9f}, {0.45f, 0.75f}};
    const float* ar = s_ar[std::clamp(level, 0, 3)];
    const Upload u = Ring(VkDeviceSize(im.width) * im.height * 2, 16);
    for (uint32_t y = 0; y < im.height; ++y)
        for (uint32_t x = 0; x < im.width; ++x)
        {
            const float px = (float(x) + 0.5f) * float(g_feat.fdmTexel.width), py = (float(y) + 0.5f) * float(g_feat.fdmTexel.height);
            const float nx = px / float(std::max(1u, usedW)) * 2.0f - 1.0f - cx, ny = py / float(std::max(1u, usedH)) * 2.0f - 1.0f - cy;
            const float r = std::sqrt(nx * nx + ny * ny);
            const uint8_t d = r > ar[1] ? 64 : r > ar[0] ? 128 : 255;
            u.cpu[(size_t(y) * im.width + x) * 2] = u.cpu[(size_t(y) * im.width + x) * 2 + 1] = d;
        }
    XferOp op;
    op.kind = XferOp::BufferToImage;
    op.srcBuffer = u.buffer;
    op.srcOffset = u.offset;
    op.image = &im;
    op.layer = layer;
    op.width = im.width;
    op.height = im.height;
    op.rowLength = im.width;
    QueueImageOp(op);
}
void SetDensityMap(GLuint texture, GLuint map)
{
    Texture* t = g_textures.Get(texture);
    if (!t || !t->image) return;
    Image* fdm = map && map <= g_densityMaps.size() ? g_densityMaps[map - 1].get() : nullptr;
    if (t->image->densityMap != fdm) { t->image->densityMap = fdm; b.targetDirty = true; }
}
void SetNextImageRenderSamples(uint32_t samples) { g_nextRenderSamples = g_feat.msrtss ? std::max(1u, samples) : 1u; }
bool RendersToSingleSampled() { return g_feat.msrtss; }
void SetRenderbufferTransient(GLuint renderbuffer, bool transient)
{
    if (Renderbuffer* rb = g_renderbuffers.Get(renderbuffer); rb && rb->image && rb->image->depth) rb->image->transient = transient;
}
bool MultiviewSupported(bool geometryStage)
{
    // (the programs' two-view stages read their block as push constants; the near-clip geometry stage where the caller
    // draws with it and the device has one)
    return g_feat.multiview && g_feat.pushBytes >= kPushBytes && (!geometryStage || !g_feat.geometry || g_feat.multiviewGeometry);
}
void SetNextRenderbufferLayers(uint32_t layers) { g_nextRenderbufferLayers = std::max(1u, layers); }
void FramebufferMultiview(GLenum target, GLenum attachment, GLuint object, bool renderbuffer, GLint baseLayer, GLsizei views)
{
    Attach(target, attachment, object, renderbuffer, 0, unsigned(std::max(baseLayer, 0)));
    if (Framebuffer* f = TargetFramebuffer(target))
    {
        const uint8_t n = uint8_t(std::clamp<GLsizei>(views, 0, 2));
        if (attachment == GL_COLOR_ATTACHMENT0) f->color.views = n;
        else f->depth.views = n;
    }
}

void AcquireImage(GLuint texture, VkImageLayout layout)
{
    if (Texture* t = g_textures.Get(texture); t && t->image) t->image->layout = layout, ++t->image->writes;
}

void ReleaseImage(GLuint texture, VkImageLayout layout, bool submit)
{
    Texture* t = g_textures.Get(texture);
    if (!g_device || !t || !t->image) return;
    if (!g_recording) BeginFrame();
    EndRendering();
    FlushCopies();
    Image& im = *t->image;
    if (im.dirty) g_dirty.erase(std::remove(g_dirty.begin(), g_dirty.end(), &im), g_dirty.end()), im.dirty = false;
    if (im.layout != layout)
    {
        Barriers bar;
        bar.Image(im, im.layout, layout);
        im.layout = layout;
        bar.Flush(Main());
    }
    if (submit) Submit();
}

void EndFrame() { Present(); }

void AfterNextSubmit(std::function<void()> fn)
{
    if (!g_device) { fn(); return; }
    g_afterSubmit.push_back(std::move(fn));
}

void WaitSubmissions() { WorkerIdle(); }

bool FSubmitsOnWorker() { return g_threaded && g_worker.joinable(); }
void WorkerCpuMs(double* pmsAll, double* pmsRecord)
{
    *pmsAll = double(g_workerCpuNs.load()) / 1e6;
    *pmsRecord = double(g_workerRecordCpuNs.load()) / 1e6;
}

GLuint CreateProgram(const char* vertexFile, const char* geometryFile, const char* fragmentFile, const char* prefix)
{
    if (!g_device || (FForcedFailure("program") && std::strcmp(vertexFile, "clear.vert"))) return 0;
    std::vector<std::string> defines;
    if (prefix)
    {
        const std::string text(prefix);
        size_t i = 0;
        while (i < text.size())
        {
            size_t j = text.find('\n', i);
            if (j == std::string::npos) j = text.size();
            std::string line = text.substr(i, j - i);
            i = j + 1;
            const size_t start = line.find_first_not_of(" \t");
            if (start == std::string::npos) continue;
            line = line.substr(start);
            if (line.rfind("#extension", 0) == 0)
            {
                // (ARB_bindless_texture: the BINDLESS variants read the global arrays; any other extension: none)
                if (line.find("GL_ARB_bindless_texture") != std::string::npos && g_feat.nonUniform) continue;
                return 0;
            }
            if (line.rfind("#define", 0) == 0)
            {
                char name[64] = {}, value[64] = {};
                if (std::sscanf(line.c_str() + 7, " %63s %63s", name, value) >= 1) defines.push_back(std::string(name) + "=" + value);
            }
        }
    }
    // (experiment, notes/VKGPU section 3b: glob's fragment maths in float16 where the device has shaderFloat16; 1: the fog
    // mix and alpha, 2: also the texture-function product. The per-platform default (kFp16Default*) is 0 everywhere for
    // now; to ship it on Android / the Quest only, set kFp16DefaultAndroid = 1. SLY2N_VKFP16=0 / 1 / 2 overrides.)
    const int s_fp16 = Fp16Level();
    static bool s_fLogged = false;
    if (!s_fLogged && (s_fp16 == 1 || s_fp16 == 2))
    {
        s_fLogged = true;
        std::printf("Vulkan: glob fragment maths in float16 (level %d)%s\n", s_fp16, g_feat.float16 ? "" : ": no shaderFloat16 on this device, fp32");
    }
    if ((s_fp16 == 1 || s_fp16 == 2) && g_feat.float16 && !std::strcmp(fragmentFile, "glob.frag"))
        defines.push_back(std::string("CANE_FP16=") + char('0' + s_fp16));
    std::sort(defines.begin(), defines.end());
    std::string key;
    for (const auto& d : defines) key += (key.empty() ? "" : ";") + d;
    if (geometryFile && !g_feat.geometry) return 0;
    const bool fetch = key.find("CANE_FETCH=") != std::string::npos;
    if (fetch && !g_feat.localRead && !g_feat.classicFetch) return 0;
    const Shaders::Program* info = nullptr;
    for (size_t i = 0; i < Shaders::programCount; ++i)
    {
        const Shaders::Program& p = Shaders::programs[i];
        if (std::strcmp(p.vert, vertexFile) || std::strcmp(p.frag, fragmentFile) || key != p.defines) continue;
        if ((geometryFile == nullptr) != (p.geom == nullptr) || (geometryFile && std::strcmp(geometryFile, p.geom))) continue;
        info = &p;
        break;
    }
    if (!info || info->pushSize > kPushBytes) return 0;
    // (fewer push-constant bytes than the block: the variant reading it from set 2)
    const bool uniformBuffer = info->pushSize > g_feat.pushBytes;
    if (uniformBuffer && !info->spirvUbo[0]) return 0;
    if (!g_recording) BeginFrame();
    // (made ahead by the prewarm, PrewarmPipelines: the same object, so the pipelines made for it match)
    std::unique_ptr<Program> made;
    for (auto it = g_prePrograms.begin(); it != g_prePrograms.end(); ++it)
        if ((*it)->info == info && (*it)->uniformBuffer == uniformBuffer)
        {
            made = std::move(*it);
            g_prePrograms.erase(it);
            break;
        }
    if (!made) made = MakeProgram(info, uniformBuffer);
    made->fetch = fetch;
    g_fetchUsed = g_fetchUsed || fetch;
    return g_programs.Adopt(std::move(made));
}
namespace
{
std::unique_ptr<Program> MakeProgram(const Shaders::Program* info, bool uniformBuffer)
{
    auto p = std::make_unique<Program>();
    p->info = info;
    p->serial = ++g_programSerial;
    p->uniformBuffer = uniformBuffer;
    for (unsigned k = 0; k < 3; ++k)
    {
        if (!info->spirv[k]) continue;
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = uniformBuffer ? info->spirvUboSize[k] : info->spirvSize[k];
        mi.pCode = uniformBuffer ? info->spirvUbo[k] : info->spirv[k];
        if (vkCreateShaderModule(g_device, &mi, nullptr, &p->modules[k]) != VK_SUCCESS) Fatal("shader module %s", info->key);
        p->storageMask |= info->storageMask[k];
        p->storageMaskMv |= info->storageMaskMv[k];
        // (two views: the stage made for them; not with the uniform-buffer block, MultiviewSupported)
        if (info->spirvMv[k] && !uniformBuffer && g_feat.multiview)
        {
            mi.codeSize = info->spirvMvSize[k];
            mi.pCode = info->spirvMv[k];
            if (vkCreateShaderModule(g_device, &mi, nullptr, &p->modulesMv[k]) != VK_SUCCESS) Fatal("shader module %s (two views)", info->key);
        }
    }
    p->constants.assign(info->pushSize, 0);
    for (uint32_t j = 0; j < 8; ++j)
        if (info->specMask & (1u << j))
            for (uint32_t i = 0; i < info->uniformCount; ++i)
                if (!std::strcmp(info->uniforms[i].name, kSpecNames[j])) p->specOffset[j] = info->uniforms[i].offset;
    p->hash = ProgramHashOf(info, uniformBuffer);
    for (uint32_t i = 0; i < info->uniformCount; ++i)
    {
        const Shaders::Uniform& u = info->uniforms[i];
        if (u.type < 32 || u.binding >= kMaxBindings) continue;
        p->samplerArray[u.binding] = u.type == 33;
        p->samplerUnit[u.binding] = 0;
        p->samplerCount = std::max<unsigned>(p->samplerCount, u.binding + 1u);
    }
    return p;
}
}  // namespace

void* ProcAddress(const char* name)
{
    if (!name) return nullptr;
    for (const ProcEntry& e : kProcs)
        if (!std::strcmp(e.name, name)) return e.fn;
    return nullptr;
}

namespace
{
void DestroyAll()
{
    if (g_device)
    {
        StopWorker();
        vkDeviceWaitIdle(g_device);
        StopPrewarm();
        SaveManifest();
        if (std::getenv("SLY2N_VKPIPELOG") && std::atoi(std::getenv("SLY2N_VKPIPELOG")) != 0)
            std::fprintf(stderr, "vkpipe: %u pipelines made on the game thread in %.1f ms (worst frame %.2f ms) over %llu frames\n",
                         g_pipeCountTotal + g_pipeCountFrame, g_pipeMsTotal + g_pipeMsFrame, std::max(g_pipeMsMaxFrame, g_pipeMsFrame),
                         (unsigned long long)g_frameSerial);
        if (g_resLogOn)
            std::fprintf(stderr, "vkres: after the load: %u textures, %u uploads (%.1f MB), %u buffers: %.1f ms over %llu frames (worst frame "
                         "%llu: %.2f ms)\n", g_resAfter.textures, g_resAfter.uploads, g_resAfter.bytes / 1048576.0, g_resAfter.buffers, g_resAfter.ms,
                         (unsigned long long)g_resFrames, (unsigned long long)g_resMaxFrameSerial, g_resMaxFrame);
        SavePipelineCache();
        g_mainXfers = XferList{};
        g_preXfers = XferList{};
        delete g_streamMain; delete g_streamPre;
        g_streamMain = g_streamPre = nullptr;
        g_threaded = false;
        g_dirty.clear();
        for (Frame& f : g_frames)
        {
            f.lastValue = 0;
            RetireFrame(f);
            if (f.pool) vkDestroyCommandPool(g_device, f.pool, nullptr);
            if (f.acquire) vkDestroySemaphore(g_device, f.acquire, nullptr);
            for (VkDescriptorPool pool : f.setPools) vkDestroyDescriptorPool(g_device, pool, nullptr);
            f = Frame{};
        }
        g_textures.ForEach([](GLuint, Texture& t) { if (t.image) DestroyImage(*t.image); t.image.reset(); });
        g_renderbuffers.ForEach([](GLuint, Renderbuffer& r) { if (r.image) DestroyImage(*r.image); r.image.reset(); });
        if (g_windowImage) DestroyImage(*g_windowImage);
        g_windowImage.reset();
        g_programs.ForEach([](GLuint, Program& p) {
            for (VkShaderModule m : p.modules) if (m) vkDestroyShaderModule(g_device, m, nullptr);
            for (VkShaderModule m : p.modulesMv) if (m) vkDestroyShaderModule(g_device, m, nullptr);
        });
        for (auto& pp : g_prePrograms)
        {
            for (VkShaderModule m : pp->modules) if (m) vkDestroyShaderModule(g_device, m, nullptr);
            for (VkShaderModule m : pp->modulesMv) if (m) vkDestroyShaderModule(g_device, m, nullptr);
        }
        g_prePrograms.clear();
        for (VkShaderModule m : g_retiredModules) vkDestroyShaderModule(g_device, m, nullptr);
        g_retiredModules.clear();
        g_buffers.Clear(); g_arrays.Clear(); g_textures.Clear(); g_samplers.Clear(); g_renderbuffers.Clear();
        g_framebuffers.Clear(); g_programs.Clear(); g_queries.Clear(); g_syncs.clear();
        for (auto& [k, pl] : g_pipelines) vkDestroyPipeline(g_device, pl, nullptr);
        if (g_presentPipeline) vkDestroyPipeline(g_device, g_presentPipeline, nullptr);
        g_presentPipeline = VK_NULL_HANDLE;
        g_presentPipelineFormat = VK_FORMAT_UNDEFINED;
        g_presentProgram = 0;
        g_pipelines.clear();
        g_layouts.clear();
        for (VkSampler sm : g_vkSamplers) vkDestroySampler(g_device, sm, nullptr);
        g_vkSamplers.clear();
        g_samplerSlots.clear();
        g_freeSlots2D.clear(); g_freeSlotsArray.clear(); g_nextSlot2D = g_nextSlotArray = 1;
        DestroySwapchain();
        if (g_queryPool) vkDestroyQueryPool(g_device, g_queryPool, nullptr);
        if (g_passPool) vkDestroyQueryPool(g_device, g_passPool, nullptr);
        g_passPool = VK_NULL_HANDLE;
        if (g_markPool) vkDestroyQueryPool(g_device, g_markPool, nullptr);
        g_markPool = VK_NULL_HANDLE;
        for (Frame& f : g_frames) f.passTimed = 0, f.passInfo.clear(), f.markLabels.clear();
        g_queryPool = VK_NULL_HANDLE;
        g_freeQueries.clear();
        if (g_timeline) vkDestroySemaphore(g_device, g_timeline, nullptr);
        g_timeline = VK_NULL_HANDLE;
        if (g_pipelineCache) vkDestroyPipelineCache(g_device, g_pipelineCache, nullptr);
        g_pipelineCache = VK_NULL_HANDLE;
        if (g_descriptorPool) vkDestroyDescriptorPool(g_device, g_descriptorPool, nullptr);
        if (g_pipelineLayout) vkDestroyPipelineLayout(g_device, g_pipelineLayout, nullptr);
        if (g_setLayoutTextures) vkDestroyDescriptorSetLayout(g_device, g_setLayoutTextures, nullptr);
        if (g_setLayoutBuffers) vkDestroyDescriptorSetLayout(g_device, g_setLayoutBuffers, nullptr);
        if (g_setLayoutUniforms) vkDestroyDescriptorSetLayout(g_device, g_setLayoutUniforms, nullptr);
        g_setLayoutUniforms = VK_NULL_HANDLE;
        g_descriptorPool = VK_NULL_HANDLE; g_pipelineLayout = VK_NULL_HANDLE;
        g_setLayoutTextures = g_setLayoutBuffers = VK_NULL_HANDLE;
        g_textureSet = VK_NULL_HANDLE;
        g_readback = {};
        g_nullStorage = {};
        g_generic = {};
        for (auto* heap : {&g_bufferHeap, &g_bufferHeapFallback, &g_readbackHeap})
            if (*heap) { (*heap)->Destroy(); heap->reset(); }
        for (auto& [t, heap] : g_imageHeaps) heap->Destroy();
        g_imageHeaps.clear();
        vkDestroyDevice(g_device, nullptr);
        g_device = VK_NULL_HANDLE;
    }
    if (g_instance)
    {
        if (g_surface && vkDestroySurfaceKHR) vkDestroySurfaceKHR(g_instance, g_surface, nullptr);
        g_surface = VK_NULL_HANDLE;
        if (g_messenger && vkDestroyDebugUtilsMessengerEXT) vkDestroyDebugUtilsMessengerEXT(g_instance, g_messenger, nullptr);
        g_messenger = VK_NULL_HANDLE;
        vkDestroyInstance(g_instance, nullptr);
        g_instance = VK_NULL_HANDLE;
    }
    g_phys = VK_NULL_HANDLE;
    g_recording = false;
}
}  // namespace

void Shutdown()
{
    DestroyAll();
    g_surfaceSerialFn = nullptr;
    g_surfaceSerial = 0;
}
}  // namespace CaneVk
