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
constexpr GLenum kAnisotropyEnum = 0x84FE, kMaxAnisotropyEnum = 0x84FF;   // (GL 4.6 / EXT_texture_filter_anisotropic)
constexpr uint32_t kPushBytes = 256;

VkDeviceSize Align(VkDeviceSize v, VkDeviceSize a) { return (v + a - 1) / a * a; }

[[noreturn]] void Fatal(const char* fmt, ...)
{
    char sz[512];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(sz, sizeof(sz), fmt, args);
    va_end(args);
    std::fprintf(stderr, "Vulkan: %s\n", sz);
    std::fflush(stderr);
    std::abort();
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
    bool geometry = false, wideLines = false, anisotropy = false, mdi = false, firstInstance = false, nonUniform = false;
    bool fillModeNonSolid = false;
    bool provokingLast = false;
    bool eds3 = false;
    bool demote = false;            // (VK_EXT_extended_dynamic_state3: blend and colour mask dynamic, fewer pipelines)   // (VK_EXT_provoking_vertex: GL's flat outputs come from a primitive's last vertex)
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

struct Image
{
    VkImage image = VK_NULL_HANDLE;
    Alloc memory;
    VkImageView view = VK_NULL_HANDLE;                       // the sampled view (all levels and layers)
    std::vector<std::pair<uint32_t, VkImageView>> attachViews;   // (level | layer << 8)
    VkFormat format = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    uint32_t width = 0, height = 0, layers = 1, levels = 1, samples = 1;
    bool array = false, depth = false, sampled = false, immutable = false;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    int slot = -1;                 // the descriptor slot (binding 0: 2D, binding 2: arrays)
    uint64_t useSegment = 0;       // the last submission whose main commands use the image
    uint64_t xferBatch = 0;        // the transfer batch that last took it (Xfers)
    bool dirty = false;            // in g_dirty (left in an attachment or transfer layout)
    bool resident = false;         // a bindless handle names it (its uses are not seen per draw)
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
};
struct Framebuffer
{
    Attachment color, depth;
    bool drawColor = true;
};
struct Program
{
    const Shaders::Program* info = nullptr;
    uint32_t serial = 0;           // (pipeline keys: unique per program object)
    std::array<VkShaderModule, 3> modules{};
    std::vector<uint8_t> constants;
    uint32_t dirtyLo = UINT32_MAX, dirtyHi = 0;
    std::array<uint8_t, kMaxBindings> samplerUnit{};
    std::array<uint8_t, kMaxBindings> samplerArray{};
    unsigned samplerCount = 0;
    uint16_t storageMask = 0;
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

// What the command buffer has been told (reset with each command buffer).
struct Bound
{
    bool targetDirty = true, viewportDirty = true, rasterDirty = true, depthDirty = true, stencilDirty = true;
    bool blendConstDirty = true, lineDirty = true, pipelineDirty = true, blendDirty = true;
    // the target
    bool hasColor = false, hasDepth = false;
    uint32_t width = 0, height = 0, samples = 1;
    uint8_t colorFormat = 0, depthFormat = 0;   // (pipeline keys: 0 none, 1 RGBA8 / the depth format)
    // the program and its resources
    GLuint program = ~0u;
    uint32_t programSerial = 0;
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
#undef CANE_DEFER_1
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
    r2->pColorAttachments = s.Dup(ri->pColorAttachments, ri->colorAttachmentCount);
    r2->pDepthAttachment = s.Dup(ri->pDepthAttachment, ri->pDepthAttachment ? 1 : 0);
    r2->pStencilAttachment = s.Dup(ri->pStencilAttachment, ri->pStencilAttachment ? 1 : 0);
    s.Rec([=](VkCommandBuffer c) { cane::vk::vkCmdBeginRendering(c, r2); });
}
void vkCmdEndRendering(VkCommandBuffer cmd)
{ CANE_DEFER(vkCmdEndRendering(cmd), [=](VkCommandBuffer c) { cane::vk::vkCmdEndRendering(c); }); }
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
};
std::thread g_worker;
std::mutex g_workerMutex;
std::condition_variable g_workerCv, g_workerDoneCv;
std::deque<Batch> g_batches;
std::vector<Stream*> g_freeStreams;
uint64_t g_workerDone = 0;              // the last batch value the worker has submitted (under g_workerMutex)
std::atomic<bool> g_presentStale{false};
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
std::unique_ptr<Image> MakeImage(uint32_t width, uint32_t height, uint32_t layers, uint32_t levels, uint32_t samples,
                                 VkFormat format, bool array, bool sampled)
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
    ci.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (im->depth) ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    else ci.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (sampled) ci.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
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
    // (GL's new storage reads as zeros on the drivers the game is tested with: cleared, which also gives it a layout)
    QueueClear(*im);
    return im;
}
VkImageView AttachView(Image& im, unsigned level, unsigned layer)
{
    const uint32_t key = level | (layer << 8);
    for (auto& [k, v] : im.attachViews) if (k == key) return v;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = im.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = im.format;
    vi.subresourceRange = {im.aspect, level, 1, layer, 1};
    VkImageView v;
    VKCHECK(vkCreateImageView(g_device, &vi, nullptr, &v));
    im.attachViews.emplace_back(key, v);
    return v;
}
void DestroyImage(Image& im)
{
    for (auto& [k, v] : im.attachViews) vkDestroyImageView(g_device, v, nullptr);
    if (im.view) vkDestroyImageView(g_device, im.view, nullptr);
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
    bool operator==(const Target&) const = default;
};
Target g_target;                   // the target of the instance being recorded (or about to begin)
bool g_inRendering = false;        // between vkCmdBeginRendering and vkCmdEndRendering
bool g_pendingBegin = false;       // g_target set, its instance not begun yet (the next draw begins it)
struct PendingClear { bool color = false, depth = false, stencil = false; VkClearValue colorValue{}, depthValue{}; } g_pendingClear;

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
    if (Image* im = g_target.color)
    {
        bar.Image(*im, im->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        im->layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        im->useSegment = g_segment;
        color.imageView = AttachView(*im, g_target.colorLevel, g_target.colorLayer);
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = g_pendingClear.color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue = g_pendingClear.colorValue;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
    }
    if (Image* im = g_target.depth)
    {
        bar.Image(*im, im->layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        im->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        im->useSegment = g_segment;
        depth.imageView = AttachView(*im, g_target.depthLevel, g_target.depthLayer);
        depth.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth.loadOp = g_pendingClear.depth ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        depth.clearValue = g_pendingClear.depthValue;
        stencil = depth;
        stencil.loadOp = g_pendingClear.stencil ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
        ri.pDepthAttachment = &depth;
        ri.pStencilAttachment = &stencil;
    }
    bar.Flush(cmd);
    vkCmdBeginRendering(cmd, &ri);
    g_inRendering = true;
    g_pendingBegin = false;
    g_pendingClear = PendingClear{};
    ++g_stats.renderPasses;
}
void EnsureRendering()
{
    if (g_pendingBegin) BeginRenderingNow();
}
// The instance ends (a transfer, a read-back, another target, the submission); the next draw on the target resumes it.
void EndRendering()
{
    if (g_pendingBegin && (g_pendingClear.color || g_pendingClear.depth || g_pendingClear.stencil)) BeginRenderingNow();
    if (!g_inRendering) return;
    vkCmdEndRendering(g_main);
    g_inRendering = false;
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
void RetireFrame(Frame& f)
{
    WaitValue(f.lastValue);   // (the CPU two frames ahead of the GPU waits here)
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
    if (r == VK_ERROR_DEVICE_LOST) Fatal("the GPU device was lost (a command faulted)");
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
void APIENTRY BufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage)
{
    const GLuint name = BufferBinding(target);
    Buffer* buf = g_buffers.Get(name);
    if (!buf || size < 0 || uint64_t(size) > 0x7FFFFFFF) { SetError(GL_INVALID_OPERATION); return; }
    if (target == GL_DRAW_INDIRECT_BUFFER && !g_feat.mdi) buf->keepShadow = true;
    Respecify(name, *buf, uint32_t(size), usage);
    ++buf->version;
    if (data && size) std::memcpy(buf->cpu, data, size_t(size));
    if (buf->keepShadow)
    {
        buf->shadow.resize(size_t(size));
        if (data && size) std::memcpy(buf->shadow.data(), data, size_t(size));
    }
}
void APIENTRY BufferSubData(GLenum target, GLintptr offset, GLsizeiptr size, const void* data)
{
    const GLuint name = BufferBinding(target);
    Buffer* buf = g_buffers.Get(name);
    if (!buf || offset < 0 || size < 0 || uint64_t(offset) + uint64_t(size) > buf->size) { SetError(GL_INVALID_VALUE); return; }
    if (!size || !data) return;
    if (!g_recording) BeginFrame();
    if (target == GL_DRAW_INDIRECT_BUFFER && !g_feat.mdi && !buf->keepShadow)
    {
        buf->keepShadow = true;
        buf->shadow.assign(buf->size, 0);
    }
    if (buf->keepShadow) std::memcpy(buf->shadow.data() + offset, data, size_t(size));
    ++buf->version;
    if (!buf->bound)
    {
        // nothing recorded reads this storage yet: written in place (a stream buffer from an earlier frame moves to
        // persistent storage first: that frame's ring is not ours)
        if (buf->stream && buf->serial != Cur().serial) Persist(*buf);
        std::memcpy(buf->cpu + offset, data, size_t(size));
        return;
    }
    if (buf->stream && buf->serial == Cur().serial)
    {
        // read by an earlier draw of this frame: the later draws get a copy with the update (copy on write)
        const Upload u = Ring(buf->size, 256);
        if (buf->stream)
        {
            std::memcpy(u.cpu, buf->cpu, buf->size);
            std::memcpy(u.cpu + offset, data, size_t(size));
            buf->vk = u.buffer;
            buf->offset = u.offset;
            buf->cpu = u.cpu;
            buf->bound = false;
            buf->useSegment = 0;
            ++g_addrEpoch;
            Cur().streamBuffers.push_back(name);
            return;
        }
        if (!buf->bound)
        {
            std::memcpy(buf->cpu + offset, data, size_t(size));
            return;
        }
    }
    if (buf->stream) Persist(*buf), std::memcpy(buf->cpu + offset, data, size_t(size));
    else
    {
        const Upload u = Ring(uint32_t(size), 16);
        std::memcpy(u.cpu, data, size_t(size));
        XferOp op;
        op.kind = XferOp::CopyBuffer;
        op.srcBuffer = u.buffer;
        op.srcOffset = u.offset;
        op.dstBuffer = buf->vk;
        op.dstOffset = buf->offset + VkDeviceSize(offset);
        op.size = VkDeviceSize(size);
        XferList& list = ListFor(buf->useSegment);
        list.ops.push_back(op);
    }
}
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
void UploadPixels(Image& im, unsigned level, unsigned layer, int x, int y, unsigned width, unsigned height, GLenum format, GLenum type, const void* pixels)
{
    if (!pixels || !width || !height) return;
    if (im.format != VK_FORMAT_R8G8B8A8_UNORM || type != GL_UNSIGNED_BYTE) return;
    unsigned components = 0;
    if (!FormatComponents(format, &components)) return;
    const uint32_t mipW = std::max(1u, im.width >> level), mipH = std::max(1u, im.height >> level);
    if (x < 0 || y < 0 || uint64_t(x) + width > mipW || uint64_t(y) + height > mipH || level >= im.levels || layer >= im.layers)
    { SetError(GL_INVALID_VALUE); return; }
    const size_t srcPitch = Align(VkDeviceSize(s.unpackRowLength ? s.unpackRowLength : width) * components, VkDeviceSize(std::max(1, s.unpackAlignment)));
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
    Texture* t = BoundTexture(target);
    if (!t || levels <= 0 || width <= 0 || height <= 0) { SetError(GL_INVALID_OPERATION); return; }
    if (!g_recording) BeginFrame();
    const unsigned l = std::min<unsigned>(unsigned(levels), FullLevels(uint32_t(width), uint32_t(height)));
    Replace(*t, MakeImage(uint32_t(width), uint32_t(height), 1, l, 1, VK_FORMAT_R8G8B8A8_UNORM, false, true));
    t->image->immutable = true;
}
void APIENTRY TexStorage3D(GLenum target, GLsizei levels, GLenum, GLsizei width, GLsizei height, GLsizei depth)
{
    Texture* t = BoundTexture(target);
    if (!t || target != GL_TEXTURE_2D_ARRAY || levels <= 0 || width <= 0 || height <= 0 || depth <= 0) { SetError(GL_INVALID_OPERATION); return; }
    if (!g_recording) BeginFrame();
    const unsigned l = std::min<unsigned>(unsigned(levels), FullLevels(uint32_t(width), uint32_t(height)));
    Replace(*t, MakeImage(uint32_t(width), uint32_t(height), uint32_t(depth), l, 1, VK_FORMAT_R8G8B8A8_UNORM, true, true));
    t->image->immutable = true;
}
void APIENTRY TexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels)
{
    Texture* t = BoundTexture(target);
    if (!t || !t->image || level < 0 || width < 0 || height < 0) { SetError(GL_INVALID_OPERATION); return; }
    UploadPixels(*t->image, unsigned(level), 0, x, y, unsigned(width), unsigned(height), format, type, pixels);
}
void APIENTRY TexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z, GLsizei width, GLsizei height, GLsizei depth, GLenum format, GLenum type, const void* pixels)
{
    Texture* t = BoundTexture(target);
    if (!t || !t->image || level < 0 || width < 0 || height < 0 || depth < 0 || z < 0) { SetError(GL_INVALID_OPERATION); return; }
    unsigned components = 4;
    FormatComponents(format, &components);
    const size_t layerBytes = size_t(Align(VkDeviceSize(s.unpackRowLength ? s.unpackRowLength : width) * components, VkDeviceSize(std::max(1, s.unpackAlignment)))) * size_t(height);
    for (GLsizei i = 0; i < depth; ++i)
        UploadPixels(*t->image, unsigned(level), unsigned(z + i), x, y, unsigned(width), unsigned(height), format, type,
                     pixels ? static_cast<const uint8_t*>(pixels) + layerBytes * size_t(i) : nullptr);
}
void APIENTRY GenerateMipmap(GLenum target)
{
    Texture* t = BoundTexture(target);
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
    const Attachment a{object, renderbuffer, uint16_t(layer), uint8_t(level)};
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
void APIENTRY RenderbufferStorageMultisample(GLenum, GLsizei samples, GLenum internal, GLsizei width, GLsizei height)
{
    Renderbuffer* rb = g_renderbuffers.Get(s.renderbuffer);
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
    rb->image = MakeImage(uint32_t(width), uint32_t(height), 1, 1, n, format, false, false);
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
void APIENTRY InvalidateFramebuffer(GLenum, GLsizei, const GLenum*) {}

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
    default: return nullptr;
    }
}
void SetCapability(GLenum cap, bool enable)
{
    bool* p = Capability(cap);
    if (!p || *p == enable) return;
    *p = enable;
    switch (cap)
    {
    case GL_DEPTH_TEST: case GL_STENCIL_TEST: b.depthDirty = true; break;
    case GL_BLEND: BlendChanged(); break;
    case GL_CULL_FACE: b.rasterDirty = true; break;
    case GL_SCISSOR_TEST: b.viewportDirty = true; break;
    default: break;
    }
}
void APIENTRY Enable(GLenum cap) { SetCapability(cap, true); }
void APIENTRY Disable(GLenum cap) { SetCapability(cap, false); }
GLboolean APIENTRY IsEnabled(GLenum cap) { const bool* p = Capability(cap); return p && *p ? GL_TRUE : GL_FALSE; }
void APIENTRY DepthMask(GLboolean flag) { if (s.depthWrite != (flag != GL_FALSE)) { s.depthWrite = flag != GL_FALSE; b.depthDirty = true; } }
void APIENTRY DepthFunc(GLenum func) { if (s.depthFunc != func) { s.depthFunc = func; b.depthDirty = true; } }
void APIENTRY DepthRange(GLdouble n, GLdouble f) { s.depthNear = float(std::clamp(n, 0.0, 1.0)); s.depthFar = float(std::clamp(f, 0.0, 1.0)); b.viewportDirty = true; }
void APIENTRY DepthRangef(GLfloat n, GLfloat f) { DepthRange(n, f); }
void APIENTRY FrontFace(GLenum mode) { if (s.frontFace != mode) { s.frontFace = mode; b.rasterDirty = true; } }
void APIENTRY CullFace(GLenum mode) { if (s.cullFace != mode) { s.cullFace = mode; b.rasterDirty = true; } }
void APIENTRY PolygonMode(GLenum, GLenum mode) { if (s.polygonMode != mode) { s.polygonMode = mode; b.pipelineDirty = true; } }
void APIENTRY BlendFuncSeparate(GLenum src, GLenum dst, GLenum srcA, GLenum dstA)
{
    if (s.blendSrc == src && s.blendDst == dst && s.blendSrcAlpha == srcA && s.blendDstAlpha == dstA) return;
    s.blendSrc = src; s.blendDst = dst; s.blendSrcAlpha = srcA; s.blendDstAlpha = dstA;
    if (s.blend) BlendChanged();
}
void APIENTRY BlendFunc(GLenum src, GLenum dst) { BlendFuncSeparate(src, dst, src, dst); }
void APIENTRY BlendEquationSeparate(GLenum rgb, GLenum alpha)
{
    if (s.blendEq == rgb && s.blendEqAlpha == alpha) return;
    s.blendEq = rgb; s.blendEqAlpha = alpha;
    if (s.blend) BlendChanged();
}
void APIENTRY BlendEquation(GLenum mode) { BlendEquationSeparate(mode, mode); }
void APIENTRY BlendColor(GLfloat r, GLfloat g, GLfloat bl, GLfloat a)
{
    const std::array<float, 4> c{r, g, bl, a};
    if (s.blendColor != c) { s.blendColor = c; b.blendConstDirty = true; }
}
void APIENTRY ColorMask(GLboolean r, GLboolean g, GLboolean bl, GLboolean a)
{
    const unsigned mask = (r ? 1u : 0u) | (g ? 2u : 0u) | (bl ? 4u : 0u) | (a ? 8u : 0u);
    if (s.colorMask != mask) { s.colorMask = mask; BlendChanged(); }
}
void APIENTRY StencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask)
{
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
        }
        if (ci && f->drawColor) { t.color = ci; t.colorLevel = f->color.level; t.colorLayer = f->color.layer; }
        if (di) { t.depth = di; t.depthLevel = f->depth.level; t.depthLayer = f->depth.layer; }
    }
    if (!(t == g_target) || (!g_pendingBegin && !g_inRendering))
    {
        EndRendering();
        g_target = t;
        g_pendingBegin = t.color || t.depth;
        g_pendingClear = PendingClear{};
    }
    const uint8_t colorFormat = t.color ? 1 : 0, depthFormat = t.depth ? 1 : 0;
    if (colorFormat != b.colorFormat || depthFormat != b.depthFormat || samples != b.samples) b.pipelineDirty = true;
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
    for (size_t k = 0; k < g_layouts.size(); ++k)
        if (g_layouts[k] == l) return uint16_t(k);
    g_layouts.push_back(l);
    return uint16_t(g_layouts.size() - 1);
}

struct PipelineKey
{
    uint32_t program = 0;
    uint16_t layout = 0;
    uint8_t topology = 0, colorFormat = 0, depthFormat = 0, samples = 1, colorMask = 0xF, blend = 0, polygon = 0, pad[3]{};
    uint32_t blendFactors = 0;
    bool operator==(const PipelineKey& o) const { return std::memcmp(this, &o, sizeof(*this)) == 0; }
};
static_assert(sizeof(PipelineKey) == 20);
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

VkPipeline CreatePipeline(const PipelineKey& k, const Program& p)
{
    std::array<VkPipelineShaderStageCreateInfo, 3> stages{};
    uint32_t nStages = 0;
    const VkShaderStageFlagBits kinds[3] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_GEOMETRY_BIT, VK_SHADER_STAGE_FRAGMENT_BIT};
    for (unsigned i = 0; i < 3; ++i)
    {
        if (!p.modules[i]) continue;
        VkPipelineShaderStageCreateInfo& st = stages[nStages++];
        st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        st.stage = kinds[i];
        st.module = p.modules[i];
        st.pName = "main";
    }
    const VertexLayout& l = g_layouts[k.layout];
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
    VKCHECK(vkCreateGraphicsPipelines(g_device, g_pipelineCache, 1, &ci, nullptr, &pipeline));
    ++g_stats.pipelinesCreated;
    return pipeline;
}
VkPipeline PipelineFor(const PipelineKey& k, const Program& p)
{
    auto it = g_pipelines.find(k);
    if (it != g_pipelines.end()) return it->second;
    const VkPipeline pipeline = CreatePipeline(k, p);
    g_pipelines.emplace(k, pipeline);
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
    if (!b.pipelineDirty) return;
    PipelineKey k;
    k.program = p.serial;
    k.layout = b.layout;
    k.topology = uint8_t(Topology(mode));
    k.colorFormat = b.colorFormat;
    k.depthFormat = b.depthFormat;
    k.samples = uint8_t(b.samples);
    k.colorMask = g_feat.eds3 ? 0xF : uint8_t(s.colorMask);
    k.blend = s.blend && !g_feat.eds3 ? 1 : 0;
    k.polygon = PolygonCode();
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
    if (size && (fProgram || p.dirtyLo < p.dirtyHi))
    {
        uint32_t lo = 0, hi = size;
        if (g_pushValid >= size)
        {
            while (lo < size && g_pushShadow[lo] == p.constants[lo]) ++lo;
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
    if (p.storageMask && (b.storageProgram != s.program || b.storageEpoch != g_storageEpoch || b.storageAddrEpoch != g_addrEpoch))
    {
        std::array<VkDescriptorBufferInfo, kMaxBindings> infos{};
        std::array<VkWriteDescriptorSet, kMaxBindings> writes{};
        uint32_t n = 0;
        unsigned mask = p.storageMask;
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
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, g_pipelineLayout, 1, n, writes.data());
        ++g_stats.descriptorPushes;
        b.storageProgram = s.program;
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

bool PrepareDraw(GLenum mode)
{
    if (!g_recording) BeginFrame();
    FlushCopies();
    ApplyTarget();
    Program* p = g_programs.Get(s.program);
    if (!p || !(g_target.color || g_target.depth)) return false;
    EnsureRendering();
    ApplyPipeline(mode, *p);
    ApplyDynamic();
    ApplyResources(*p);
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

// ------------------------------------------------------------------------------------------------ clears and blits
GLuint g_clearProgram = 0;
// A clear the attachment clears cannot do (a partial colour or stencil write mask, which GL's clears obey): one
// triangle over the scissor rectangle with the masks as the pipeline's and dynamic state.
void ClearMasked(bool color, bool depth, bool stencil)
{
    Program* p = g_programs.Get(g_clearProgram);
    if (!p) return;
    EnsureRendering();
    VkCommandBuffer cmd = g_main;
    PipelineKey k;
    k.program = p->serial;
    k.layout = LayoutFor(*g_arrays.Get(0), *p);
    k.topology = uint8_t(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    k.colorFormat = b.colorFormat;
    k.depthFormat = b.depthFormat;
    k.samples = uint8_t(b.samples);
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
    VkClearAttachment atts[2];
    uint32_t n = 0;
    if (color) atts[n++] = {VK_IMAGE_ASPECT_COLOR_BIT, 0, cv};
    if (depth || stencil)
        atts[n++] = {VkImageAspectFlags((depth ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)), 0, dv};
    const VkClearRect cr{rect, 0, 1};
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
    if (src->samples > 1)
    {
        // a multisample resolve (the same rectangle size: GL has no scaled resolves)
        if (dst->samples == 1 && same)
        {
            VkImageResolve r{};
            r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, sl, sla, 1};
            r.srcOffset = {sx0, sy0, 0};
            r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dl, dla, 1};
            r.dstOffset = {dx0, dy0, 0};
            r.extent = {uint32_t(sx1 - sx0), uint32_t(sy1 - sy0), 1};
            vkCmdResolveImage(cmd, src->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
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
PFN_vkResetQueryPool g_vkResetQueryPool = nullptr;
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
    P("DepthRange", DepthRange), P("DepthRangef", DepthRangef), P("ClearColor", ClearColor), P("ClearDepth", ClearDepth),
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
struct WindowPicture { VkImage image; uint32_t width, height; };
void RecordSwapBlit(VkCommandBuffer cmd, uint32_t index, WindowPicture win)
{
    if (index == UINT32_MAX) return;
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
                if (r == VK_SUBOPTIMAL_KHR) g_presentStale = true;
            }
            else g_presentStale = true;
        }
        VkCommandBuffer cmds[2];
        uint32_t n = 0;
        if (bt.pre) cmds[n++] = WorkerRecord(f, *bt.pre);
        cmds[n++] = WorkerRecord(f, *bt.main);
        SubmitBatch(cmds, n, bt.value, wait, signal);
        if (g_workerIndex != UINT32_MAX)
        {
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = 1;
            pi.pWaitSemaphores = &signal;
            pi.swapchainCount = 1;
            pi.pSwapchains = &g_swapchain;
            pi.pImageIndices = &g_workerIndex;
            const VkResult r = vkQueuePresentKHR(g_queue, &pi);
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g_presentStale = true;
            else if (r == VK_ERROR_DEVICE_LOST) Fatal("the GPU device was lost");
        }
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

void DestroySwapchain()
{
    if (!g_device) return;
    WorkerIdle();
    vkDeviceWaitIdle(g_device);
    for (VkSemaphore sem : g_presentSemaphores) vkDestroySemaphore(g_device, sem, nullptr);
    g_presentSemaphores.clear();
    if (g_swapchain) vkDestroySwapchainKHR(g_device, g_swapchain, nullptr);
    g_swapchain = VK_NULL_HANDLE;
    g_swapImages.clear();
}
bool CreateSwapchain(std::string* error)
{
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(g_phys, g_surface, &caps) != VK_SUCCESS) { if (error) *error = "surface capabilities"; return false; }
    int fbw = 0, fbh = 0;
    glfwGetFramebufferSize(g_window, &fbw, &fbh);
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) extent = {uint32_t(std::max(fbw, 1)), uint32_t(std::max(fbh, 1))};
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
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR) ? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : caps.currentTransform;
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
    g_swapExtent = extent;
    g_swapchainStale = false;
    return true;
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
    g_windowImage = MakeImage(w, h, 1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM, false, false);
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
    if (apiVersion < VK_API_VERSION_1_3) return fail("the Vulkan loader is older than 1.3");
    g_window = window;
    g_visible = options.visible;
    // (tests: SLY2N_VKHEADLESSWSI=1: a hidden window still gets a swapchain, on VK_EXT_headless_surface, so the acquire /
    // copy / present path runs without anything on the screen)
    const bool fHeadlessWsi = !g_visible && std::getenv("SLY2N_VKHEADLESSWSI") && std::atoi(std::getenv("SLY2N_VKHEADLESSWSI")) != 0;
    if (fHeadlessWsi) g_visible = true;
    g_swapInterval = std::clamp(options.swapInterval, 0, 1);
    g_cachePath = options.pipelineCachePath;
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
    if (vkCreateInstance(&ici, nullptr, &g_instance) != VK_SUCCESS) return fail("vkCreateInstance failed");
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
    const char* szWanted = std::getenv("SLY2N_VKDEVICE");
    std::string reasons;
    int bestScore = -1;
    for (uint32_t i = 0; i < devices.size(); ++i)
    {
        VkPhysicalDevice pd = devices[i];
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(pd, &props);
        auto reject = [&](const char* why) { reasons += std::string(reasons.empty() ? "" : "; ") + props.deviceName + ": " + why; };
        if (props.apiVersion < VK_API_VERSION_1_3) { reject("Vulkan 1.3 is required"); continue; }
        uint32_t ne = 0;
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, nullptr);
        std::vector<VkExtensionProperties> exts(ne);
        vkEnumerateDeviceExtensionProperties(pd, nullptr, &ne, exts.data());
        if (!HasExtension(exts, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME)) { reject("no VK_KHR_push_descriptor"); continue; }
        if (g_visible && !HasExtension(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) { reject("no swapchain"); continue; }
        VkPhysicalDeviceExtendedDynamicState3FeaturesEXT fe3{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
        const bool hasEds3 = HasExtension(exts, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME) && !std::getenv("SLY2N_VKNOEDS3");
        VkPhysicalDeviceProvokingVertexFeaturesEXT fpv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT, hasEds3 ? &fe3 : nullptr};
        const bool hasProvoking = HasExtension(exts, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
        VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &fpv};
        VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
        VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
        vkGetPhysicalDeviceFeatures2(pd, &f2);
        if (!f13.dynamicRendering || !f13.synchronization2) { reject("no dynamic rendering / synchronization2"); continue; }
        if (!f12.timelineSemaphore || !f12.hostQueryReset) { reject("no timeline semaphores"); continue; }
        if (!f12.runtimeDescriptorArray || !f12.descriptorBindingPartiallyBound || !f12.descriptorBindingSampledImageUpdateAfterBind ||
            !f12.descriptorBindingUpdateUnusedWhilePending) { reject("no descriptor indexing"); continue; }
        VkPhysicalDevicePushDescriptorPropertiesKHR pp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &pp};
        vkGetPhysicalDeviceProperties2(pd, &p2);
        if (pp.maxPushDescriptors < kMaxBindings) { reject("too few push descriptors"); continue; }
        if (props.limits.maxPushConstantsSize < kPushBytes) { reject("too few push-constant bytes"); continue; }
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
            g_feat.wideLines = f2.features.wideLines;
            g_feat.lineWidthMax = props.limits.lineWidthRange[1];
            g_feat.anisotropy = f2.features.samplerAnisotropy;
            g_feat.anisotropyMax = props.limits.maxSamplerAnisotropy;
            g_feat.mdi = f2.features.multiDrawIndirect;
            g_feat.firstInstance = f2.features.drawIndirectFirstInstance;
            g_feat.fillModeNonSolid = f2.features.fillModeNonSolid;
            g_feat.provokingLast = hasProvoking && fpv.provokingVertexLast;
            g_feat.demote = f13.shaderDemoteToHelperInvocation;
            g_feat.eds3 = hasEds3 && fe3.extendedDynamicState3ColorBlendEnable && fe3.extendedDynamicState3ColorBlendEquation &&
                          fe3.extendedDynamicState3ColorWriteMask;
            g_feat.nonUniform = f12.shaderSampledImageArrayNonUniformIndexing;
            g_feat.samples = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts &
                             props.limits.framebufferStencilSampleCounts;
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
    if (g_feat.eds3) fe3.pNext = chain, chain = &fe3;
    if (g_feat.provokingLast) fpv.pNext = chain, chain = &fpv;
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, chain};
    f13.dynamicRendering = VK_TRUE;
    f13.synchronization2 = VK_TRUE;
    f13.shaderDemoteToHelperInvocation = g_feat.demote;
    VkPhysicalDeviceVulkan12Features f12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    f12.timelineSemaphore = VK_TRUE;
    f12.hostQueryReset = VK_TRUE;
    f12.runtimeDescriptorArray = VK_TRUE;
    f12.descriptorBindingPartiallyBound = VK_TRUE;
    f12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
    f12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
    f12.shaderSampledImageArrayNonUniformIndexing = g_feat.nonUniform;
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    f2.features.geometryShader = g_feat.geometry;
    f2.features.wideLines = g_feat.wideLines;
    f2.features.samplerAnisotropy = g_feat.anisotropy;
    f2.features.multiDrawIndirect = g_feat.mdi;
    f2.features.drawIndirectFirstInstance = g_feat.firstInstance;
    f2.features.fillModeNonSolid = g_feat.fillModeNonSolid;
    f2.features.shaderSampledImageArrayDynamicIndexing = VK_TRUE;
    std::vector<const char*> deviceExtensions = {VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME};
    if (g_visible) deviceExtensions.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    if (g_feat.provokingLast) deviceExtensions.push_back(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
    if (g_feat.eds3) deviceExtensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &f2;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qi;
    dci.enabledExtensionCount = uint32_t(deviceExtensions.size());
    dci.ppEnabledExtensionNames = deviceExtensions.data();
    if (vkCreateDevice(g_phys, &dci, nullptr, &g_device) != VK_SUCCESS) return fail("vkCreateDevice failed");
    if (!LoadDevice(g_device, error)) return fail(error ? *error : "device entry points");
    if (!vkCmdPushDescriptorSetKHR) return fail("no vkCmdPushDescriptorSetKHR");
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
        std::array<VkDescriptorSetLayoutBinding, kMaxBindings> sb{};
        for (uint32_t i = 0; i < kMaxBindings; ++i) sb[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, g_stages, nullptr};
        VkDescriptorSetLayoutCreateInfo bli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        bli.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        bli.bindingCount = kMaxBindings;
        bli.pBindings = sb.data();
        if (vkCreateDescriptorSetLayout(g_device, &bli, nullptr, &g_setLayoutBuffers) != VK_SUCCESS) return fail("push descriptor set layout");
        const VkDescriptorSetLayout sets[2] = {g_setLayoutTextures, g_setLayoutBuffers};
        const VkPushConstantRange range{g_stages, 0, kPushBytes};
        VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pli.setLayoutCount = 2;
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
    g_threaded = !(std::getenv("SLY2N_VKTHREAD") && std::atoi(std::getenv("SLY2N_VKTHREAD")) == 0);
    if (g_threaded)
    {
        g_streamMain = new Stream;
        g_streamPre = new Stream;
        g_workerDone = 0;
        g_worker = std::thread(WorkerLoop);
    }
    BeginFrame();
    g_blackTexture = MakeBlack(false);
    g_blackArray = MakeBlack(true);
    g_clearProgram = CreateProgram("clear.vert", nullptr, "clear.frag", "");
    if (g_visible)
    {
        std::string why;
        if (!CreateSwapchain(&why)) return fail(why);
    }
    const bool fBuffersLocal = (g_memProps.memoryTypes[g_bufferHeap->Type()].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) != 0;
    std::printf("Vulkan: %s%s\n", g_rendererString.c_str(), fBuffersLocal ? "" : " (buffers in system memory)");
    return true;
}

bool Initialized() { return g_device != VK_NULL_HANDLE; }
const char* DeviceName() { return g_deviceName.c_str(); }
Stats TakeStats()
{
    Stats st = g_stats;
    st.splits = g_splits;
    g_stats = Stats{};
    g_splits = 0;
    return st;
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
    Frame& f = Cur();
    if (g_visible)
    {
        if (g_swapchainStale || !g_swapchain)
        {
            // (a new size or present mode: the frame's commands first, then the swapchain again)
            Submit();
            DestroySwapchain();
            std::string why;
            if (!CreateSwapchain(&why)) Fatal("%s", why.c_str());
        }
        if (g_threaded)
        {
            // (the worker acquires, copies and presents: the swapchain is its alone while it runs)
            if (g_presentStale.exchange(false)) g_swapchainStale = true;
            Image& win = *g_windowImage;
            Barriers bar;
            bar.Image(win, win.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            win.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            bar.Flush(g_main);
            win.useSegment = g_segment;
            const WindowPicture pic{win.image, win.width, win.height};   // (the main thread may replace the window's image)
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
            if (r == VK_SUBOPTIMAL_KHR) g_swapchainStale = true;
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
            if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) g_swapchainStale = true;
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
                         "copies), %.2f submits, %.1f pushes (%.0f bytes), %.1f descriptor pushes\n", s_sum.draws / n, s_sum.pipelines / n,
                         s_sum.pipelinesCreated / n, s_sum.renderPasses / n, s_sum.splits / n, s_sum.submits / n, s_sum.pushes / n,
                         s_sum.pushBytes / n, s_sum.descriptorPushes / n);
            s_sum = Stats{};
            s_iStats = 0;
        }
    }
    // (the frame's last submission is the one Submit just made; the segment it began is dropped with the frame)
    g_recording = false;
    g_frameIndex = (g_frameIndex + 1) % kFrameSlots;
}

GLuint CreateProgram(const char* vertexFile, const char* geometryFile, const char* fragmentFile, const char* prefix)
{
    if (!g_device) return 0;
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
    std::sort(defines.begin(), defines.end());
    std::string key;
    for (const auto& d : defines) key += (key.empty() ? "" : ";") + d;
    if (geometryFile && !g_feat.geometry) return 0;
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
    if (!g_recording) BeginFrame();
    const GLuint name = g_programs.Gen();
    Program* p = g_programs.Get(name);
    p->info = info;
    p->serial = ++g_programSerial;
    for (unsigned k = 0; k < 3; ++k)
    {
        if (!info->spirv[k]) continue;
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = info->spirvSize[k];
        mi.pCode = info->spirv[k];
        if (vkCreateShaderModule(g_device, &mi, nullptr, &p->modules[k]) != VK_SUCCESS) Fatal("shader module %s", info->key);
        p->storageMask |= info->storageMask[k];
    }
    p->constants.assign(info->pushSize, 0);
    for (uint32_t i = 0; i < info->uniformCount; ++i)
    {
        const Shaders::Uniform& u = info->uniforms[i];
        if (u.type < 32 || u.binding >= kMaxBindings) continue;
        p->samplerArray[u.binding] = u.type == 33;
        p->samplerUnit[u.binding] = 0;
        p->samplerCount = std::max<unsigned>(p->samplerCount, u.binding + 1u);
    }
    return name;
}

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
            f = Frame{};
        }
        g_textures.ForEach([](GLuint, Texture& t) { if (t.image) DestroyImage(*t.image); t.image.reset(); });
        g_renderbuffers.ForEach([](GLuint, Renderbuffer& r) { if (r.image) DestroyImage(*r.image); r.image.reset(); });
        if (g_windowImage) DestroyImage(*g_windowImage);
        g_windowImage.reset();
        g_programs.ForEach([](GLuint, Program& p) { for (VkShaderModule m : p.modules) if (m) vkDestroyShaderModule(g_device, m, nullptr); });
        for (VkShaderModule m : g_retiredModules) vkDestroyShaderModule(g_device, m, nullptr);
        g_retiredModules.clear();
        g_buffers.Clear(); g_arrays.Clear(); g_textures.Clear(); g_samplers.Clear(); g_renderbuffers.Clear();
        g_framebuffers.Clear(); g_programs.Clear(); g_queries.Clear(); g_syncs.clear();
        for (auto& [k, pl] : g_pipelines) vkDestroyPipeline(g_device, pl, nullptr);
        g_pipelines.clear();
        g_layouts.clear();
        for (VkSampler sm : g_vkSamplers) vkDestroySampler(g_device, sm, nullptr);
        g_vkSamplers.clear();
        g_samplerSlots.clear();
        g_freeSlots2D.clear(); g_freeSlotsArray.clear(); g_nextSlot2D = g_nextSlotArray = 1;
        DestroySwapchain();
        if (g_queryPool) vkDestroyQueryPool(g_device, g_queryPool, nullptr);
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
}
}  // namespace CaneVk
