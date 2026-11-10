// The games' OpenGL on deko3d: see gl_deko.h.
//
// Design (performance first: the Switch's Cortex-A57 at stock clocks records every frame):
// - Objects live in dense tables indexed by their GL names (no hashing per call). Deleted objects' GPU memory is
//   released when the frame that last could use it has retired (a per-frame garbage list), never per bind.
// - Static / dynamic buffers and images are suballocated from large memory blocks. Stream buffers (GL_STREAM_DRAW, the
//   game's per-draw and per-frame uploads) live in the frame's transient ring: glBufferData orphans by taking new ring
//   memory, glBufferSubData into memory no command has read yet writes in place.
// - Updates of memory the GPU may be reading (glBufferSubData on a static buffer, texture uploads) are staged in the
//   ring and copied in the command stream; the copies queued between two draws share one pair of barriers.
// - Loose uniforms (one std140 block per program, cmake/switch_shaders.py) are pushed with dkCmdBufPushConstants, the
//   hardware's pipelined constant update: no allocation and no rebinding per glUniform.
// - Textures keep their image descriptor slot for their life; samplers are interned (a few dozen states). A texture
//   handle per draw is two table reads.
// - Fixed-function state, vertex input, storage buffers and textures are re-emitted only when they changed.
// - glMultiDrawElementsIndirect (the game's runs of draws) is expanded on the CPU from the indirect buffer's shadow.
// - The swapchain image is acquired at the first use of the window in the frame (the scene renders off-screen first),
//   and long frames are submitted in parts so the GPU starts while the CPU still records.
//
// Coordinates: the device is created with deko3d's OpenGL conventions, the lower-left origin and [0, 1] depth
// (glClipControl(GL_LOWER_LEFT, GL_ZERO_TO_ONE), which Sly 2 sets once). In that mode deko3d 0.5.0 maps clip -1 to
// memory row 0 as GL does for its framebuffer objects, makes the hardware's front-face test GL's, and has the display
// flip the swapchain (nwindowSetTransform(HAL_TRANSFORM_FLIP_V), dk_swapchain.cpp): viewports, scissors, blits,
// glReadPixels rows, winding and render-to-texture all carry over from GL unchanged, the window included.
#include "gl_deko.h"
#include "deko_shaders.h"
#include "../saltynx.h"

#include <switch.h>
#include <deko3d.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace CaneDeko
{
namespace
{
// ------------------------------------------------------------------------------------------------ constants
constexpr unsigned kFrameSlots = 2;          // command lists in flight: the CPU runs at most this many frames ahead
constexpr unsigned kSwapImages = 3;          // the swapchain (triple buffering: the GPU never waits for the display)
constexpr unsigned kImageDescriptors = 16384, kSamplerDescriptors = 1024;
constexpr uint32_t kBufferBlockSize = 32u << 20, kImageBlockSize = 64u << 20, kRingChunkSize = 8u << 20;
constexpr uint32_t kCmdChunkSize = 1u << 20;
constexpr unsigned kMaxAttribs = 16, kGenericSlot = 15, kMaxUnits = 32, kMaxBindings = 16;
constexpr unsigned kSubmitDrawThreshold = 384;   // draws recorded before a part of the frame is submitted
constexpr int kStageIndex[3] = {DkStage_Vertex, DkStage_Geometry, DkStage_Fragment};
constexpr GLenum kAnisotropyEnum = 0x84FE, kMaxAnisotropyEnum = 0x84FF;   // (GL 4.6 / EXT_texture_filter_anisotropic)

uint32_t Align(uint32_t v, uint32_t a) { return (v + a - 1) / a * a; }
uint64_t Align64(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

std::string Format(const char* fmt, va_list args)
{
    char sz[512];
    std::vsnprintf(sz, sizeof(sz), fmt, args);
    return sz;
}
[[noreturn]] void Fatal(const char* fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    const std::string message = "Deko3D: " + Format(fmt, args);
    va_end(args);
    throw std::runtime_error(message);
}

// ------------------------------------------------------------------------------------------------ GPU memory
class RangeAllocator
{
public:
    explicit RangeAllocator(uint32_t size = 0) : m_free{{0u, size}} { if (!size) m_free.clear(); }
    bool Allocate(uint32_t size, uint32_t alignment, uint32_t* offset)
    {
        for (auto it = m_free.begin(); it != m_free.end(); ++it)
        {
            const uint64_t start = Align64(it->first, alignment);
            const uint64_t end = start + size;
            if (end > uint64_t(it->first) + it->second) continue;
            const uint32_t oldStart = it->first, oldSize = it->second;
            m_free.erase(it);
            if (start > oldStart) m_free.emplace(oldStart, uint32_t(start - oldStart));
            if (end < uint64_t(oldStart) + oldSize) m_free.emplace(uint32_t(end), uint32_t(oldStart + oldSize - end));
            *offset = uint32_t(start);
            return true;
        }
        return false;
    }
    void Release(uint32_t start, uint32_t size)
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
    std::map<uint32_t, uint32_t> m_free;
};

DkDevice g_device = nullptr;

DkMemBlock NewMemBlock(uint32_t size, uint32_t flags)
{
    DkMemBlockMaker maker;
    dkMemBlockMakerDefaults(&maker, g_device, Align(size, DK_MEMBLOCK_ALIGNMENT));
    maker.flags = flags;
    DkMemBlock block = dkMemBlockCreate(&maker);
    if (!block) Fatal("out of GPU memory (%u bytes, flags 0x%x)", unsigned(size), unsigned(flags));
    return block;
}

class Heap;
struct Block
{
    Heap* heap = nullptr;
    DkMemBlock mem = nullptr;
    uint8_t* cpu = nullptr;
    DkGpuAddr gpu = 0;
    uint32_t size = 0, used = 0;
    RangeAllocator ranges;
};
struct Alloc
{
    Block* block = nullptr;
    uint32_t offset = 0, size = 0;
    explicit operator bool() const { return block != nullptr; }
    DkGpuAddr Gpu() const { return block->gpu + offset; }
    uint8_t* Cpu() const { return block->cpu ? block->cpu + offset : nullptr; }
};
class Heap
{
public:
    Heap(uint32_t flags, uint32_t blockSize) : m_flags(flags), m_blockSize(blockSize) {}
    Alloc Allocate(uint32_t size, uint32_t alignment)
    {
        size = Align(std::max(size, 1u), 64);
        alignment = std::max(alignment, 64u);
        for (auto& b : m_blocks)
        {
            uint32_t offset;
            if (b->size - b->used >= size && b->ranges.Allocate(size, alignment, &offset))
            {
                b->used += size;
                return {b.get(), offset, size};
            }
        }
        auto b = std::make_unique<Block>();
        b->heap = this;
        b->size = Align(std::max(m_blockSize, size + alignment), DK_MEMBLOCK_ALIGNMENT);
        b->mem = NewMemBlock(b->size, m_flags);
        b->cpu = (m_flags & DkMemBlockFlags_CpuAccessMask) ? static_cast<uint8_t*>(dkMemBlockGetCpuAddr(b->mem)) : nullptr;
        b->gpu = dkMemBlockGetGpuAddr(b->mem);
        b->ranges = RangeAllocator(b->size);
        uint32_t offset = 0;
        b->ranges.Allocate(size, alignment, &offset);
        b->used = size;
        m_blocks.push_back(std::move(b));
        return {m_blocks.back().get(), offset, size};
    }
    void Free(const Alloc& a)
    {
        if (!a) return;
        a.block->ranges.Release(a.offset, a.size);
        a.block->used -= a.size;
    }
    void Destroy()
    {
        for (auto& b : m_blocks) dkMemBlockDestroy(b->mem);
        m_blocks.clear();
    }
private:
    uint32_t m_flags, m_blockSize;
    std::vector<std::unique_ptr<Block>> m_blocks;
};

std::unique_ptr<Heap> g_bufferHeap, g_imageHeap;

// ------------------------------------------------------------------------------------------------ frames
struct RingChunk
{
    DkMemBlock mem = nullptr;
    uint8_t* cpu = nullptr;
    DkGpuAddr gpu = 0;
    uint32_t size = 0, cursor = 0;
};
struct Frame
{
    DkCmdBuf cmd = nullptr;
    std::vector<DkMemBlock> cmdMem;
    size_t cmdMemNext = 0;
    std::vector<RingChunk> ring;
    size_t ringIndex = 0;
    DkFence fence{};
    bool submitted = false;
    uint64_t serial = 0;
    std::vector<Alloc> garbage;
    std::vector<uint32_t> garbageImageSlots;
    std::vector<GLuint> streamBuffers;
};
std::array<Frame, kFrameSlots> g_frames;
unsigned g_frameIndex = 0;
uint64_t g_nextSerial = 1;
bool g_recording = false;
bool g_commandsPending = false;    // commands recorded since the last submission
unsigned g_drawsSinceSubmit = 0;
Frame& Cur() { return g_frames[g_frameIndex]; }

DkQueue g_queue = nullptr;
DkSwapchain g_swapchain = nullptr;
DkMemBlock g_swapMemory = nullptr;
std::array<DkImage, kSwapImages> g_swapImages{};
int g_swapSlot = -1;                 // the image acquired this frame (-1: none yet)
unsigned g_outputWidth = 0, g_outputHeight = 0;
int g_swapInterval = 1;
DkMemBlock g_descriptorMemory = nullptr;
uint8_t* g_imageDescriptorsCpu = nullptr;
uint8_t* g_samplerDescriptorsCpu = nullptr;
DkGpuAddr g_imageDescriptorsGpu = 0, g_samplerDescriptorsGpu = 0;
DkMemBlock g_codeMemory = nullptr;
bool g_descriptorsDirty = false;
DkMemBlock g_readback = nullptr;
uint32_t g_readbackSize = 0;

void BeginFrame();
DkCmdBuf Cmd()
{
    if (!g_recording) BeginFrame();
    g_commandsPending = true;
    return Cur().cmd;
}

void GrowCommands(void* user, DkCmdBuf cmd, size_t minimum)
{
    Frame& f = *static_cast<Frame*>(user);
    const uint32_t size = Align(uint32_t(std::max<size_t>(minimum, kCmdChunkSize)), DK_MEMBLOCK_ALIGNMENT);
    if (f.cmdMemNext == f.cmdMem.size()) f.cmdMem.push_back(NewMemBlock(size, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached));
    DkMemBlock& mem = f.cmdMem[f.cmdMemNext++];
    if (dkMemBlockGetSize(mem) < size)
    {
        // (blocks from cmdMemNext on were last used by this slot's retired frame: free to replace)
        dkMemBlockDestroy(mem);
        mem = NewMemBlock(size, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    }
    dkCmdBufAddMemory(cmd, mem, 0, dkMemBlockGetSize(mem));
}

struct Upload { DkGpuAddr gpu; uint8_t* cpu; uint32_t size; };
void DrainRing();
// Transient memory of this frame (reused once the frame has retired).
Upload Ring(uint32_t size, uint32_t alignment = 256)
{
    if (!g_recording) BeginFrame();
    Frame& f = Cur();
    size = Align(std::max(size, 4u), 4);
    // (a world load uploads every texture before its first frame is shown: past 64 MB the frame waits for the GPU and
    // starts over in the memory it has, rather than growing without bound)
    if (f.ring.size() * kRingChunkSize >= (64u << 20) && f.ringIndex + 1 >= f.ring.size())
    {
        const RingChunk& last = f.ring.back();
        if (uint64_t(Align(last.cursor, alignment)) + size > last.size) DrainRing();
    }
    for (; f.ringIndex < f.ring.size(); ++f.ringIndex)
    {
        RingChunk& c = f.ring[f.ringIndex];
        const uint32_t offset = Align(c.cursor, alignment);
        if (uint64_t(offset) + size <= c.size)
        {
            c.cursor = offset + size;
            return {c.gpu + offset, c.cpu + offset, size};
        }
    }
    RingChunk c;
    c.size = Align(std::max(kRingChunkSize, size), DK_MEMBLOCK_ALIGNMENT);
    c.mem = NewMemBlock(c.size, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    c.cpu = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(c.mem));
    c.gpu = dkMemBlockGetGpuAddr(c.mem);
    c.cursor = size;
    f.ring.push_back(c);
    f.ringIndex = f.ring.size() - 1;
    return {c.gpu, c.cpu, size};
}

void Submit()
{
    if (!g_recording || !g_commandsPending) return;
    dkQueueSubmitCommands(g_queue, dkCmdBufFinishList(Cur().cmd));
    dkQueueFlush(g_queue);
    g_commandsPending = false;
    g_drawsSinceSubmit = 0;
}

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
    // The object of name 0 (the default vertex array).
    T* Default() { if (!m_objects[0]) m_objects[0] = std::make_unique<T>(); return m_objects[0].get(); }
    // GL lets a program bind a name it never generated (or one generated and not yet created): make it exist.
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

void Garbage(const Alloc& a) { if (a) Cur().garbage.push_back(a); }

struct Buffer
{
    uint32_t size = 0;
    bool stream = false;           // transient storage (GL_STREAM_*)
    Alloc storage;                 // persistent storage
    DkGpuAddr gpu = 0;             // the storage's GPU address (persistent or transient)
    uint8_t* cpu = nullptr;        // its CPU address (both kinds are CPU-writable)
    uint64_t serial = 0;           // the frame of a transient allocation
    bool bound = false;            // a recorded command may read the current storage
    std::vector<uint8_t> shadow;   // a CPU copy (indirect command buffers, uniform buffers)
    bool keepShadow = false;
    uint32_t version = 1;          // changes with the contents (the uniform buffer ranges uploaded per draw)
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
struct Image
{
    DkImageLayout layout{};
    DkImage image{};
    Alloc memory;
    uint32_t width = 0, height = 0, layers = 1, levels = 1, samples = 1;
    DkImageFormat format = DkImageFormat_RGBA8_Unorm;
    bool array = false;
    int descriptor = -1;           // the image descriptor slot (sampled images)
};
struct Texture
{
    std::unique_ptr<Image> image;
    SamplerParams params;
    int samplerSlot = -1;          // the interned sampler of params (-1: look up)
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
    std::array<DkShader, 3> shaders{};
    std::array<bool, 3> hasStage{};
    std::vector<uint8_t> constants;
    Alloc ubo;
    uint32_t uboSize = 0;
    uint32_t dirtyLo = UINT32_MAX, dirtyHi = 0;
    std::array<uint8_t, kMaxBindings> samplerUnit{};
    std::array<uint8_t, kMaxBindings> samplerArray{};
    std::array<uint8_t, kMaxBindings> samplerStages{};
    unsigned samplerCount = 0;
};
struct Query
{
    DkMemBlock memory = nullptr;   // two timestamp reports
    uint64_t serial = 0;           // the frame that wrote them
};
struct Sync
{
    DkFence fence{};
    uint64_t serial = 0;
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

// ------------------------------------------------------------------------------------------------ GL state
struct Range { GLuint buffer = 0; uint32_t offset = 0, size = 0; bool whole = true; };
struct State
{
    GLuint arrayBuffer = 0, uniformBuffer = 0, storageBuffer = 0, indirectBuffer = 0, copyRead = 0, copyWrite = 0, pixelUnpack = 0, pixelPack = 0;
    GLuint vertexArray = 0, program = 0, renderbuffer = 0, drawFramebuffer = 0, readFramebuffer = 0;
    unsigned activeUnit = 0;
    std::array<std::array<GLuint, 2>, kMaxUnits> units{};     // [unit][0: 2D, 1: 2D array]
    std::array<GLuint, kMaxUnits> samplerUnits{};
    std::array<Range, kMaxBindings> storage{}, uniforms{};
    std::array<std::array<float, 4>, kMaxAttribs> generic{};
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
    DkPolygonMode polygonMode = DkPolygonMode_Fill;
    GLint unpackAlignment = 4, unpackRowLength = 0, packAlignment = 4;
    GLenum error = GL_NO_ERROR;
};
State s;

// What the command buffer has been told (reset at each frame's start).
struct Bound
{
    bool targetDirty = true, viewportDirty = true, rasterDirty = true, depthDirty = true, stencilDirty = true;
    bool blendDirty = true, colorWriteDirty = true, blendConstDirty = true, lineDirty = true, msDirty = true;
    // the target
    bool window = false, hasColor = false, hasDepth = false;
    uint32_t width = 0, height = 0, samples = 1;
    GLuint fbo = ~0u;
    // the program and its resources
    GLuint program = ~0u;
    std::array<std::array<DkBufExtents, kMaxBindings>, 3> storage{};
    std::array<std::array<DkBufExtents, kMaxBindings>, 3> uniforms{};
    std::array<std::array<DkResHandle, kMaxBindings>, 3> textures{};
    // vertex input
    const VertexArray* vao = nullptr;
    uint32_t vaoVersion = 0;
    std::array<DkBufExtents, kMaxAttribs> vertexBuffers{};
    DkGpuAddr indexAddress = DK_GPU_ADDR_INVALID;
    DkIdxFormat indexFormat = DkIdxFormat_Uint32;
    bool genericDirty = true;
    // (what the storage and attribute buffers were last resolved at: ApplyProgram, ApplyVertexInput)
    GLuint storageProgram = ~0u;
    uint64_t storageEpoch = 0, storageAddrEpoch = 0;
    const VertexArray* vertexVao = nullptr;
    uint32_t vertexVaoVersion = 0;
    uint64_t vertexAddrEpoch = 0;
};
Bound b;
// The per-draw resolutions are skipped while nothing they read changed (Sly 2 binds its storage buffers once a pass and
// draws many runs with them): g_addrEpoch counts the buffers' storage moves and deletions, g_storageEpoch the storage
// bindings' changes. (Bound is reset at each frame: its first draw resolves everything.)
uint64_t g_addrEpoch = 1, g_storageEpoch = 1;

void InvalidateBound()
{
    b = Bound{};
    for (auto& stage : b.storage) stage.fill({DK_GPU_ADDR_INVALID, 0});
    for (auto& stage : b.uniforms) stage.fill({DK_GPU_ADDR_INVALID, 0});
    for (auto& stage : b.textures) stage.fill(~DkResHandle(0));
    b.vertexBuffers.fill({DK_GPU_ADDR_INVALID, 0});
}

// The uniform block ranges uploaded this frame, per binding (ResolveUniform).
struct UniformUpload
{
    GLuint buffer = 0;
    uint32_t version = 0, offset = 0, size = 0;
    uint64_t serial = 0;
    DkBufExtents extent{DK_GPU_ADDR_INVALID, 0};
};
std::array<UniformUpload, kMaxBindings> g_uniformUploads;
Alloc g_nullUniform;   // (a shader's block with nothing bound reads zeros)

void SetError(GLenum error) { if (s.error == GL_NO_ERROR) s.error = error; }

// ------------------------------------------------------------------------------------------------ descriptors
std::vector<uint32_t> g_freeImageSlots;
uint32_t g_nextImageSlot = 0;
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
int SamplerSlot(const SamplerParams& p)
{
    const uint64_t key = SamplerKey(p);
    auto it = g_samplerSlots.find(key);
    if (it != g_samplerSlots.end()) return it->second;
    const int slot = int(g_samplerSlots.size());
    if (slot >= int(kSamplerDescriptors)) return 0;   // (out of sampler descriptors: the first one)
    DkSampler sampler;
    dkSamplerDefaults(&sampler);
    const GLenum min = p.minFilter;
    sampler.minFilter = (min == GL_NEAREST || min == GL_NEAREST_MIPMAP_NEAREST || min == GL_NEAREST_MIPMAP_LINEAR) ? DkFilter_Nearest : DkFilter_Linear;
    sampler.magFilter = p.magFilter == GL_NEAREST ? DkFilter_Nearest : DkFilter_Linear;
    sampler.mipFilter = (min == GL_NEAREST || min == GL_LINEAR) ? DkMipFilter_None
                      : (min == GL_NEAREST_MIPMAP_NEAREST || min == GL_LINEAR_MIPMAP_NEAREST) ? DkMipFilter_Nearest : DkMipFilter_Linear;
    auto wrap = [](GLenum w) {
        return w == GL_CLAMP_TO_EDGE ? DkWrapMode_ClampToEdge : w == GL_MIRRORED_REPEAT ? DkWrapMode_MirroredRepeat
             : w == GL_CLAMP_TO_BORDER ? DkWrapMode_ClampToBorder : DkWrapMode_Repeat;
    };
    sampler.wrapMode[0] = wrap(p.wrapS);
    sampler.wrapMode[1] = wrap(p.wrapT);
    sampler.wrapMode[2] = DkWrapMode_ClampToEdge;
    sampler.lodClampMax = float(std::clamp(p.maxLevel, 0, 1000));
    sampler.maxAnisotropy = std::clamp(p.anisotropy, 1.0f, 16.0f);
    DkSamplerDescriptor descriptor;
    dkSamplerDescriptorInitialize(&descriptor, &sampler);
    std::memcpy(g_samplerDescriptorsCpu + size_t(slot) * sizeof(DkSamplerDescriptor), &descriptor, sizeof(descriptor));
    g_samplerSlots.emplace(key, slot);
    g_descriptorsDirty = true;
    return slot;
}

uint32_t AllocImageSlot()
{
    if (!g_freeImageSlots.empty()) { const uint32_t slot = g_freeImageSlots.back(); g_freeImageSlots.pop_back(); return slot; }
    if (g_nextImageSlot >= kImageDescriptors) Fatal("image descriptor capacity (%u) exceeded", kImageDescriptors);
    return g_nextImageSlot++;
}
void WriteImageDescriptor(Image& image)
{
    if (image.descriptor < 0) image.descriptor = int(AllocImageSlot());
    DkImageView view;
    dkImageViewDefaults(&view, &image.image);
    DkImageDescriptor descriptor;
    dkImageDescriptorInitialize(&descriptor, &view, false, false);
    std::memcpy(g_imageDescriptorsCpu + size_t(image.descriptor) * sizeof(DkImageDescriptor), &descriptor, sizeof(descriptor));
    g_descriptorsDirty = true;
}
struct PendingCopy;
void DropCopiesInto(const Image* image);
// An image no longer used: its memory and descriptor slot go when the frame retires.
void ReleaseImage(std::unique_ptr<Image>& image)
{
    if (!image) return;
    DropCopiesInto(image.get());
    Garbage(image->memory);
    if (image->descriptor >= 0) Cur().garbageImageSlots.push_back(uint32_t(image->descriptor));
    image.reset();
}

DkMsMode MsMode(unsigned samples)
{
    switch (samples) { case 2: return DkMsMode_2x; case 4: return DkMsMode_4x; case 8: return DkMsMode_8x; default: return DkMsMode_1x; }
}

// The S3TC (BC1-3) formats, sampled natively by the GPU: their deko3d format and bytes per 4x4 block.
bool CompressedFormat(GLenum internal, DkImageFormat* format, uint32_t* blockBytes)
{
    DkImageFormat f;
    uint32_t bytes = 16;
    switch (internal)
    {
    case 0x83F0: f = DkImageFormat_RGB_BC1; bytes = 8; break;          // GL_COMPRESSED_RGB_S3TC_DXT1_EXT
    case 0x83F1: f = DkImageFormat_RGBA_BC1; bytes = 8; break;         // GL_COMPRESSED_RGBA_S3TC_DXT1_EXT
    case 0x83F2: f = DkImageFormat_RGBA_BC2; break;                    // GL_COMPRESSED_RGBA_S3TC_DXT3_EXT
    case 0x83F3: f = DkImageFormat_RGBA_BC3; break;                    // GL_COMPRESSED_RGBA_S3TC_DXT5_EXT
    case 0x8C4C: f = DkImageFormat_RGB_BC1_sRGB; bytes = 8; break;     // GL_COMPRESSED_SRGB_S3TC_DXT1_EXT
    case 0x8C4D: f = DkImageFormat_RGBA_BC1_sRGB; bytes = 8; break;
    case 0x8C4E: f = DkImageFormat_RGBA_BC2_sRGB; break;
    case 0x8C4F: f = DkImageFormat_RGBA_BC3_sRGB; break;
    default: return false;
    }
    if (format) *format = f;
    if (blockBytes) *blockBytes = bytes;
    return true;
}
bool IsCompressed(DkImageFormat f)
{
    return f == DkImageFormat_RGB_BC1 || f == DkImageFormat_RGBA_BC1 || f == DkImageFormat_RGBA_BC2 || f == DkImageFormat_RGBA_BC3 ||
           f == DkImageFormat_RGB_BC1_sRGB || f == DkImageFormat_RGBA_BC1_sRGB || f == DkImageFormat_RGBA_BC2_sRGB ||
           f == DkImageFormat_RGBA_BC3_sRGB;
}

std::unique_ptr<Image> MakeImage(uint32_t width, uint32_t height, uint32_t layers, uint32_t levels, uint32_t samples,
                                 DkImageFormat format, bool array, bool render, bool sampled)
{
    auto image = std::make_unique<Image>();
    image->width = width; image->height = height; image->layers = layers; image->levels = levels; image->samples = samples;
    image->format = format; image->array = array;
    DkImageLayoutMaker maker;
    dkImageLayoutMakerDefaults(&maker, g_device);
    maker.type = array ? DkImageType_2DArray : samples > 1 ? DkImageType_2DMS : DkImageType_2D;
    maker.format = format;
    maker.dimensions[0] = width;
    maker.dimensions[1] = height;
    maker.dimensions[2] = array ? layers : 1;
    maker.mipLevels = levels;
    maker.msMode = MsMode(samples);
    const bool depth = format == DkImageFormat_Z24S8 || format == DkImageFormat_ZF32_X24S8;
    maker.flags = DkImageFlags_UsageRender;
    if (!depth) maker.flags |= DkImageFlags_Usage2DEngine;
    if (render && samples == 1) maker.flags |= DkImageFlags_HwCompression;
    if (depth || samples > 1) maker.flags |= DkImageFlags_HwCompression;
    if (IsCompressed(format)) maker.flags = 0;   // (block-compressed: sampled and copied into only)
    dkImageLayoutInitialize(&image->layout, &maker);
    const uint64_t size = dkImageLayoutGetSize(&image->layout);
    if (size > 0x7FFFFFFF) Fatal("image too large (%ux%u)", unsigned(width), unsigned(height));
    image->memory = g_imageHeap->Allocate(uint32_t(size), dkImageLayoutGetAlignment(&image->layout));
    dkImageInitialize(&image->image, &image->layout, image->memory.block->mem, image->memory.offset);
    if (sampled) WriteImageDescriptor(*image);
    return image;
}

DkImageView View(const Image& image, unsigned level = 0, unsigned layer = 0, bool single = true)
{
    DkImageView view;
    dkImageViewDefaults(&view, &image.image);
    if (single)
    {
        view.type = image.samples > 1 ? DkImageType_2DMS : DkImageType_2D;
        view.layerOffset = uint16_t(layer);
        view.layerCount = 1;
        view.mipLevelOffset = uint8_t(level);
        view.mipLevelCount = 1;
    }
    return view;
}

// ------------------------------------------------------------------------------------------------ queued copies
struct PendingCopy
{
    bool image = false;
    DkGpuAddr src = 0;
    DkGpuAddr dst = 0;
    uint32_t size = 0;
    // image copies
    const Image* target = nullptr;
    uint32_t rowLength = 0, level = 0, layer = 0;
    DkImageRect rect{};
};
std::vector<PendingCopy> g_pendingCopies;

// (uploads into an image being destroyed are moot)
void DropCopiesInto(const Image* image)
{
    g_pendingCopies.erase(std::remove_if(g_pendingCopies.begin(), g_pendingCopies.end(),
                                         [&](const PendingCopy& c) { return c.image && c.target == image; }),
                          g_pendingCopies.end());
}

void FlushCopies()
{
    if (g_pendingCopies.empty()) return;
    DkCmdBuf cmd = Cmd();
    dkCmdBufBarrier(cmd, DkBarrier_Primitives, 0);
    for (const PendingCopy& c : g_pendingCopies)
    {
        if (!c.image) dkCmdBufCopyBuffer(cmd, c.src, c.dst, c.size);
        else
        {
            const DkImageView view = View(*c.target, c.level, c.layer);
            const DkCopyBuf src{c.src, c.rowLength, 0};
            dkCmdBufCopyBufferToImage(cmd, &src, &view, &c.rect, 0);
        }
    }
    dkCmdBufBarrier(cmd, DkBarrier_None, DkInvalidateFlags_Image | DkInvalidateFlags_Shader | DkInvalidateFlags_L2Cache);
    g_pendingCopies.clear();
}

// ------------------------------------------------------------------------------------------------ frame lifetime
void Persist(Buffer& buf);
// The frame's transient memory reused within the frame: everything recorded so far runs to completion first, and the
// stream buffers still in the ring move to persistent storage (they may be drawn later in the frame).
void DrainRing()
{
    Frame& f = Cur();
    FlushCopies();
    for (GLuint name : f.streamBuffers)
    {
        Buffer* buf = g_buffers.Get(name);
        if (buf && buf->stream && buf->serial == f.serial && buf->size) Persist(*buf);
    }
    f.streamBuffers.clear();
    Submit();
    dkQueueWaitIdle(g_queue);
    for (RingChunk& c : f.ring) c.cursor = 0;
    f.ringIndex = 0;
    b.genericDirty = true;
    for (UniformUpload& u : g_uniformUploads) u.serial = 0;   // (their copies lived in the ring)
}
void RetireFrame(Frame& f)
{
    if (f.submitted)
    {
        // (the CPU two frames ahead of the GPU waits here)
        if (dkFenceWait(&f.fence, -1) != DkResult_Success) Fatal("frame fence wait failed");
        f.submitted = false;
    }
    for (const Alloc& a : f.garbage) a.block->heap->Free(a);
    f.garbage.clear();
    for (uint32_t slot : f.garbageImageSlots) g_freeImageSlots.push_back(slot);
    f.garbageImageSlots.clear();
    // Stream buffers still holding this frame's ring memory (never respecified since): their contents move into
    // persistent storage before the ring is reused (rare: a GL_STREAM_DRAW buffer drawn over several frames).
    for (GLuint name : f.streamBuffers)
    {
        Buffer* buf = g_buffers.Get(name);
        if (buf && buf->stream && buf->serial == f.serial && buf->size) Persist(*buf);
    }
    f.streamBuffers.clear();
}

void BeginFrame()
{
    Frame& f = Cur();
    RetireFrame(f);
    for (RingChunk& c : f.ring) c.cursor = 0;
    f.ringIndex = 0;
    f.serial = g_nextSerial++;
    dkCmdBufClear(f.cmd);
    f.cmdMemNext = 1;
    dkCmdBufAddMemory(f.cmd, f.cmdMem[0], 0, dkMemBlockGetSize(f.cmdMem[0]));
    g_recording = true;
    g_commandsPending = true;
    InvalidateBound();
    DkCmdBuf cmd = f.cmd;
    dkCmdBufBindImageDescriptorSet(cmd, g_imageDescriptorsGpu, kImageDescriptors);
    dkCmdBufBindSamplerDescriptorSet(cmd, g_samplerDescriptorsGpu, kSamplerDescriptors);
    dkCmdBufBarrier(cmd, DkBarrier_None, DkInvalidateFlags_Image | DkInvalidateFlags_Shader | DkInvalidateFlags_Descriptors);
    g_descriptorsDirty = false;
}

void AcquireSwapImage()
{
    if (g_swapSlot >= 0) return;
    if (!g_recording) BeginFrame();
    // (the display's pace: with three images, a wait here is the frame waiting for a vblank)
    g_swapSlot = dkQueueAcquireImage(g_queue, g_swapchain);
    if (g_swapSlot < 0) Fatal("swapchain acquire failed");
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
Buffer* BoundBuffer(GLenum target) { return g_buffers.Get(BufferBinding(target)); }

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
    if (buf.storage) Garbage(buf.storage);
    buf.storage = {};
    buf.gpu = 0;
    buf.cpu = nullptr;
}
void APIENTRY DeleteBuffers(GLsizei n, const GLuint* names)
{
    for (GLsizei i = 0; i < n; ++i)
    {
        const GLuint name = names[i];
        auto buf = g_buffers.Take(name);
        if (!buf) continue;
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
        for (Range& r : s.uniforms) if (r.buffer == name) r = {};
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
    const Upload u = stream && size ? Ring(size, 256) : Upload{0, nullptr, 0};
    ReleaseStorage(buf);
    buf.size = size;
    buf.stream = stream;
    buf.bound = false;
    ++g_addrEpoch;
    if (!size) return;
    if (buf.stream)
    {
        buf.gpu = u.gpu;
        buf.cpu = u.cpu;
        buf.serial = Cur().serial;
        Cur().streamBuffers.push_back(name);
    }
    else
    {
        buf.storage = g_bufferHeap->Allocate(size, 256);
        buf.gpu = buf.storage.Gpu();
        buf.cpu = buf.storage.Cpu();
    }
}
// A stream buffer's contents into persistent storage (its ring memory is about to be, or may soon be, reused).
void Persist(Buffer& buf)
{
    if (!buf.cpu || !buf.size) return;
    Alloc a = g_bufferHeap->Allocate(buf.size, 256);
    std::memcpy(a.Cpu(), buf.cpu, buf.size);
    buf.storage = a;
    buf.stream = false;
    buf.gpu = a.Gpu();
    buf.cpu = a.Cpu();
    buf.bound = false;
    ++g_addrEpoch;
}
void APIENTRY BufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage)
{
    const GLuint name = BufferBinding(target);
    Buffer* buf = g_buffers.Get(name);
    if (!buf || size < 0 || uint64_t(size) > 0x7FFFFFFF) { SetError(GL_INVALID_OPERATION); return; }
    if (target == GL_DRAW_INDIRECT_BUFFER) buf->keepShadow = true;
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
    if (target == GL_DRAW_INDIRECT_BUFFER && !buf->keepShadow)
    {
        buf->keepShadow = true;
        buf->shadow.assign(buf->size, 0);
    }
    if (buf->keepShadow) std::memcpy(buf->shadow.data() + offset, data, size_t(size));
    ++buf->version;
    if (!buf->bound)
    {
        // nothing recorded reads this storage yet: write it in place (a uniform buffer's draws read per-draw copies of
        // its CPU copy, ResolveUniform, so its storage is never "bound").
        // (A stream buffer from an earlier frame moves to persistent storage first: that frame's ring is not ours.)
        if (buf->stream && buf->serial != Cur().serial) Persist(*buf);
        std::memcpy(buf->cpu + offset, data, size_t(size));
        return;
    }
    if (buf->stream)
    {
        // read by an earlier draw of this frame: the later draws get a copy with the update (copy on write)
        // (taking ring memory may drain the ring, which moves this buffer to persistent storage: then it is updated
        // as a persistent one, below)
        const Upload u = Ring(buf->size, 256);
        if (buf->stream)
        {
            std::memcpy(u.cpu, buf->cpu, buf->size);
            std::memcpy(u.cpu + offset, data, size_t(size));
            buf->gpu = u.gpu;
            buf->cpu = u.cpu;
            buf->serial = Cur().serial;
            buf->bound = false;
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
    const Upload u = Ring(uint32_t(size), 16);
    std::memcpy(u.cpu, data, size_t(size));
    PendingCopy c;
    c.src = u.gpu;
    c.dst = buf->gpu + uint64_t(offset);
    c.size = uint32_t(size);
    g_pendingCopies.push_back(c);
}
// (not supported: the caller falls back)
void* APIENTRY MapBufferRange(GLenum, GLintptr, GLsizeiptr, GLbitfield) { return nullptr; }
GLboolean APIENTRY UnmapBuffer(GLenum) { return GL_TRUE; }
GLboolean APIENTRY IsBuffer(GLuint name) { return g_buffers.Get(name) ? GL_TRUE : GL_FALSE; }
void APIENTRY BindBufferRange(GLenum target, GLuint index, GLuint name, GLintptr offset, GLsizeiptr size)
{
    if (index >= kMaxBindings || offset < 0 || size < 0) { SetError(GL_INVALID_VALUE); return; }
    if (name) g_buffers.Ensure(name);
    auto& ranges = target == GL_SHADER_STORAGE_BUFFER ? s.storage : s.uniforms;
    if (target != GL_SHADER_STORAGE_BUFFER && target != GL_UNIFORM_BUFFER) return;
    ranges[index] = {name, uint32_t(offset), uint32_t(size), false};
    if (target == GL_SHADER_STORAGE_BUFFER) ++g_storageEpoch;
    BufferBinding(target) = name;
    // a uniform buffer keeps a CPU copy: each draw gets its bound range as of the draw (ResolveUniform)
    Buffer* buf = target == GL_UNIFORM_BUFFER && name ? g_buffers.Get(name) : nullptr;
    if (buf && !buf->keepShadow)
    {
        if (buf->stream && buf->serial != Cur().serial) Persist(*buf);
        buf->keepShadow = true;
        buf->shadow.assign(buf->size, 0);
        if (buf->cpu && buf->size) std::memcpy(buf->shadow.data(), buf->cpu, buf->size);
    }
}
void APIENTRY BindBufferBase(GLenum target, GLuint index, GLuint name)
{
    BindBufferRange(target, index, name, 0, 0);
    if (index < kMaxBindings) (target == GL_SHADER_STORAGE_BUFFER ? s.storage : s.uniforms)[index].whole = true;
}

// The GPU address of a buffer's storage for a command recorded now. A stream buffer filled in an earlier frame (not
// respecified since) moves to persistent storage first: its ring memory is only kept until that frame retires, while
// this frame's commands may run after.
DkGpuAddr Use(Buffer& buf)
{
    if (buf.stream && buf.serial != Cur().serial && buf.size) Persist(buf);
    buf.bound = true;
    return buf.gpu;
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
    if (index >= kGenericSlot || components < 1 || components > 4 || stride < 0) { SetError(GL_INVALID_VALUE); return; }
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
    if (index >= kGenericSlot) return;
    VertexArray* vao = g_arrays.Get(s.vertexArray);
    if (!vao->attribs[index].enabled) { vao->attribs[index].enabled = true; ++vao->version; }
}
void APIENTRY DisableVertexAttribArray(GLuint index)
{
    if (index >= kGenericSlot) return;
    VertexArray* vao = g_arrays.Get(s.vertexArray);
    if (vao->attribs[index].enabled) { vao->attribs[index].enabled = false; ++vao->version; }
}
void APIENTRY VertexAttribDivisor(GLuint index, GLuint divisor)
{
    if (index >= kGenericSlot) return;
    VertexArray* vao = g_arrays.Get(s.vertexArray);
    vao->attribs[index].divisor = divisor;
    ++vao->version;
}
void APIENTRY VertexAttrib4f(GLuint index, GLfloat x, GLfloat y, GLfloat z, GLfloat w)
{
    if (index >= kGenericSlot) return;
    s.generic[index] = {x, y, z, w};
    b.genericDirty = true;
}
void APIENTRY VertexAttrib3f(GLuint i, GLfloat x, GLfloat y, GLfloat z) { VertexAttrib4f(i, x, y, z, 1.0f); }
void APIENTRY VertexAttrib2f(GLuint i, GLfloat x, GLfloat y) { VertexAttrib4f(i, x, y, 0.0f, 1.0f); }
void APIENTRY VertexAttrib1f(GLuint i, GLfloat x) { VertexAttrib4f(i, x, 0.0f, 0.0f, 1.0f); }

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
void APIENTRY DeleteTextures(GLsizei n, const GLuint* names)
{
    for (GLsizei i = 0; i < n; ++i)
    {
        const GLuint name = names[i];
        auto t = g_textures.Take(name);
        if (!t) continue;
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
    default: return;   // (GL_TEXTURE_WRAP_R, GL_TEXTURE_BASE_LEVEL and the rest are ignored)
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
// Pixels into the ring as tightly laid out RGBA8 rows (pitch aligned for the copy engine) and a queued copy.
void UploadPixels(Image& image, unsigned level, unsigned layer, int x, int y, unsigned width, unsigned height, GLenum format, GLenum type, const void* pixels)
{
    if (!pixels || !width || !height) return;
    if (image.format != DkImageFormat_RGBA8_Unorm || type != GL_UNSIGNED_BYTE) return;
    unsigned components = 0;
    if (!FormatComponents(format, &components)) return;
    const uint32_t mipW = std::max(1u, image.width >> level), mipH = std::max(1u, image.height >> level);
    if (x < 0 || y < 0 || uint64_t(x) + width > mipW || uint64_t(y) + height > mipH || level >= image.levels || layer >= image.layers)
    { SetError(GL_INVALID_VALUE); return; }
    const uint32_t srcPitch = Align(uint32_t(s.unpackRowLength ? s.unpackRowLength : width) * components, uint32_t(std::max(1, s.unpackAlignment)));
    const uint32_t pitch = Align(width * 4, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
    const uint64_t bytes = uint64_t(pitch) * height;
    if (bytes > 0x7FFFFFFF) { SetError(GL_OUT_OF_MEMORY); return; }
    const Upload u = Ring(uint32_t(bytes), 256);
    const auto* src = static_cast<const uint8_t*>(pixels);
    // (GL's last row has no padding after it: never read past width x 4 there)
    if (format == GL_RGBA && srcPitch == pitch) std::memcpy(u.cpu, src, size_t(pitch) * (height - 1) + size_t(width) * 4);
    else
    {
        // (rows converted in cached memory, then written out whole: the ring is write-combined)
        std::vector<uint8_t> row(size_t(width) * 4);
        for (unsigned y0 = 0; y0 < height; ++y0)
        {
            const uint8_t* p = src + size_t(y0) * srcPitch;
            if (format == GL_RGBA) { std::memcpy(u.cpu + size_t(y0) * pitch, p, row.size()); continue; }
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
            std::memcpy(u.cpu + size_t(y0) * pitch, row.data(), row.size());
        }
    }
    PendingCopy c;
    c.image = true;
    c.src = u.gpu;
    c.target = &image;
    c.rowLength = pitch;
    c.level = level;
    c.layer = layer;
    c.rect = {uint32_t(x), uint32_t(y), 0, width, height, 1};
    g_pendingCopies.push_back(c);
}
// (an image replaced while copies into the old one are queued: those copies go first)
void Replace(Texture& t, std::unique_ptr<Image> image)
{
    for (const PendingCopy& c : g_pendingCopies)
        if (c.target == t.image.get()) { FlushCopies(); break; }
    ReleaseImage(t.image);
    t.image = std::move(image);
}
// (the uncompressed textures are RGBA8, whatever their internal format)
void APIENTRY TexImage2D(GLenum target, GLint level, GLint, GLsizei width, GLsizei height, GLint, GLenum format, GLenum type, const void* pixels)
{
    Texture* t = BoundTexture(target);
    if (!t || target != GL_TEXTURE_2D || level < 0 || width <= 0 || height <= 0) { SetError(GL_INVALID_OPERATION); return; }
    if (level == 0)
    {
        // (a texture redefined at its size keeps its image: the game re-uploads render textures that way)
        if (!t->image || t->image->width != uint32_t(width) || t->image->height != uint32_t(height) || t->image->array || t->image->levels != 1)
            Replace(*t, MakeImage(uint32_t(width), uint32_t(height), 1, 1, 1, DkImageFormat_RGBA8_Unorm, false, pixels == nullptr, true));
    }
    else if (!t->image || unsigned(level) >= t->image->levels) return;
    UploadPixels(*t->image, unsigned(level), 0, 0, 0, unsigned(width), unsigned(height), format, type, pixels);
}
unsigned FullLevels(uint32_t w, uint32_t h)
{
    unsigned levels = 1;
    for (uint32_t n = std::max(w, h); n > 1; n >>= 1) ++levels;
    return levels;
}
void APIENTRY TexStorage2D(GLenum target, GLsizei levels, GLenum internal, GLsizei width, GLsizei height)
{
    Texture* t = BoundTexture(target);
    if (!t || levels <= 0 || width <= 0 || height <= 0) { SetError(GL_INVALID_OPERATION); return; }
    const unsigned l = std::min<unsigned>(unsigned(levels), FullLevels(uint32_t(width), uint32_t(height)));
    DkImageFormat compressed;
    if (CompressedFormat(internal, &compressed, nullptr))
    {
        Replace(*t, MakeImage(uint32_t(width), uint32_t(height), 1, l, 1, compressed, false, false, true));
        return;
    }
    Replace(*t, MakeImage(uint32_t(width), uint32_t(height), 1, l, 1, DkImageFormat_RGBA8_Unorm, false, false, true));
}
// Compressed blocks into the ring as GL lays them out (rows of 4x4 blocks) and a queued copy: the copy engine takes
// them as they are (deko3d converts the texel rectangle into blocks).
void UploadBlocks(Image& image, unsigned level, int x, int y, unsigned width, unsigned height, GLsizei bytes, const void* data)
{
    DkImageFormat format;
    uint32_t blockBytes = 16;
    if (!IsCompressed(image.format)) { SetError(GL_INVALID_OPERATION); return; }
    for (GLenum e : {0x83F0u, 0x83F1u, 0x83F2u, 0x83F3u, 0x8C4Cu, 0x8C4Du, 0x8C4Eu, 0x8C4Fu})
        if (CompressedFormat(e, &format, &blockBytes) && format == image.format) break;
    const uint32_t mipW = std::max(1u, image.width >> level), mipH = std::max(1u, image.height >> level);
    if (!data || !width || !height || level >= image.levels || x < 0 || y < 0 || (x & 3) || (y & 3) ||
        uint64_t(x) + width > mipW || uint64_t(y) + height > mipH) { SetError(GL_INVALID_VALUE); return; }
    const uint32_t pitch = ((width + 3) / 4) * blockBytes, rows = (height + 3) / 4;
    const uint64_t size = uint64_t(pitch) * rows;
    if (bytes < 0 || uint64_t(bytes) < size) { SetError(GL_INVALID_VALUE); return; }
    const Upload u = Ring(uint32_t(size), 256);
    std::memcpy(u.cpu, data, size_t(size));
    PendingCopy c;
    c.image = true;
    c.src = u.gpu;
    c.target = &image;
    c.rowLength = pitch;
    c.level = level;
    c.layer = 0;
    c.rect = {uint32_t(x), uint32_t(y), 0, width, height, 1};
    g_pendingCopies.push_back(c);
}
void APIENTRY CompressedTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height, GLenum,
                                      GLsizei bytes, const void* data)
{
    Texture* t = BoundTexture(target);
    if (!t || !t->image || level < 0 || width < 0 || height < 0) { SetError(GL_INVALID_OPERATION); return; }
    UploadBlocks(*t->image, unsigned(level), x, y, unsigned(width), unsigned(height), bytes, data);
}
void APIENTRY CompressedTexImage2D(GLenum target, GLint level, GLenum internal, GLsizei width, GLsizei height, GLint,
                                   GLsizei bytes, const void* data)
{
    Texture* t = BoundTexture(target);
    DkImageFormat format;
    if (!t || target != GL_TEXTURE_2D || level < 0 || width <= 0 || height <= 0 || !CompressedFormat(internal, &format, nullptr))
    { SetError(GL_INVALID_ENUM); return; }
    if (level == 0 && (!t->image || t->image->format != format || t->image->width != uint32_t(width) || t->image->height != uint32_t(height)))
        Replace(*t, MakeImage(uint32_t(width), uint32_t(height), 1, 1, 1, format, false, false, true));
    if (!t->image || unsigned(level) >= t->image->levels) return;
    UploadBlocks(*t->image, unsigned(level), 0, 0, unsigned(width), unsigned(height), bytes, data);
}
// (no readback: the pixels come back black)
void APIENTRY GetTexImage(GLenum target, GLint level, GLenum format, GLenum type, void* pixels)
{
    Texture* t = BoundTexture(target);
    if (!t || !t->image || !pixels || level < 0) return;
    unsigned components = 4;
    FormatComponents(format, &components);
    const size_t bpc = type == GL_FLOAT ? 4 : 1;
    std::memset(pixels, 0, size_t(std::max(1u, t->image->width >> level)) * std::max(1u, t->image->height >> level) * components * bpc);
}
void APIENTRY TexStorage3D(GLenum target, GLsizei levels, GLenum, GLsizei width, GLsizei height, GLsizei depth)
{
    Texture* t = BoundTexture(target);
    if (!t || target != GL_TEXTURE_2D_ARRAY || levels <= 0 || width <= 0 || height <= 0 || depth <= 0) { SetError(GL_INVALID_OPERATION); return; }
    const unsigned l = std::min<unsigned>(unsigned(levels), FullLevels(uint32_t(width), uint32_t(height)));
    Replace(*t, MakeImage(uint32_t(width), uint32_t(height), uint32_t(depth), l, 1, DkImageFormat_RGBA8_Unorm, true, false, true));
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
    const size_t layerBytes = size_t(Align(uint32_t(s.unpackRowLength ? s.unpackRowLength : width) * components, uint32_t(std::max(1, s.unpackAlignment)))) * size_t(height);
    for (GLsizei i = 0; i < depth; ++i)
        UploadPixels(*t->image, unsigned(level), unsigned(z + i), x, y, unsigned(width), unsigned(height), format, type,
                     pixels ? static_cast<const uint8_t*>(pixels) + layerBytes * size_t(i) : nullptr);
}
void APIENTRY GenerateMipmap(GLenum target)
{
    Texture* t = BoundTexture(target);
    if (!t || !t->image) return;
    Image& image = *t->image;
    if (image.samples > 1) return;
    if (IsCompressed(image.format)) return;   // (a compressed texture keeps its uploaded levels)
    if (image.levels == 1)
    {
        // (a single level: GL makes the whole chain; the image is reallocated with it)
        const unsigned levels = FullLevels(image.width, image.height);
        if (levels == 1) return;
        auto chain = MakeImage(image.width, image.height, image.layers, levels, 1, image.format, image.array, false, true);
        FlushCopies();
        DkCmdBuf cmd = Cmd();
        dkCmdBufBarrier(cmd, DkBarrier_Primitives, 0);
        for (unsigned layer = 0; layer < image.layers; ++layer)
        {
            const DkImageView src = View(image, 0, layer), dst = View(*chain, 0, layer);
            const DkImageRect rect{0, 0, 0, image.width, image.height, 1};
            dkCmdBufCopyImage(cmd, &src, &rect, &dst, &rect, 0);
        }
        Replace(*t, std::move(chain));
    }
    FlushCopies();
    Image& im = *t->image;
    DkCmdBuf cmd = Cmd();
    for (unsigned level = 1; level < im.levels; ++level)
    {
        dkCmdBufBarrier(cmd, DkBarrier_Primitives, DkInvalidateFlags_Image);
        for (unsigned layer = 0; layer < im.layers; ++layer)
        {
            const DkImageView src = View(im, level - 1, layer), dst = View(im, level, layer);
            const DkImageRect srcRect{0, 0, 0, std::max(1u, im.width >> (level - 1)), std::max(1u, im.height >> (level - 1)), 1};
            const DkImageRect dstRect{0, 0, 0, std::max(1u, im.width >> level), std::max(1u, im.height >> level), 1};
            dkCmdBufBlitImage(cmd, &src, &srcRect, &dst, &dstRect, DkBlitFlag_FilterLinear, 0);
        }
    }
    dkCmdBufBarrier(cmd, DkBarrier_Primitives, DkInvalidateFlags_Image | DkInvalidateFlags_L2Cache);
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

// ------------------------------------------------------------------------------------------------ framebuffers
void APIENTRY GenFramebuffers(GLsizei n, GLuint* names) { for (GLsizei i = 0; i < n; ++i) names[i] = g_framebuffers.Gen(); }
void APIENTRY BindFramebuffer(GLenum target, GLuint name)
{
    if (name) g_framebuffers.Ensure(name);
    if (target == GL_FRAMEBUFFER || target == GL_DRAW_FRAMEBUFFER)
    {
        if (s.drawFramebuffer != name)
        {
            // (leaving a target after a long run of draws: submit, so the GPU works while the CPU records the next pass)
            if (g_drawsSinceSubmit >= 64) Submit();
            s.drawFramebuffer = name;
            b.targetDirty = true;
        }
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
        if (b.fbo == names[i]) b.targetDirty = true;
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
        ReleaseImage(rb->image);
        if (s.renderbuffer == names[i]) s.renderbuffer = 0;
        DetachEverywhere(names[i], true);
    }
}
void APIENTRY RenderbufferStorageMultisample(GLenum, GLsizei samples, GLenum internal, GLsizei width, GLsizei height)
{
    Renderbuffer* rb = g_renderbuffers.Get(s.renderbuffer);
    if (!rb || width <= 0 || height <= 0) { SetError(GL_INVALID_OPERATION); return; }
    DkImageFormat format = DkImageFormat_RGBA8_Unorm;
    switch (internal)
    {
    // (the PC's 32-bit float depth as Z24S8 too: half the depth traffic)
    case GL_DEPTH32F_STENCIL8: case GL_DEPTH_COMPONENT32F:
    case GL_DEPTH24_STENCIL8: case GL_DEPTH_COMPONENT24: case GL_DEPTH_COMPONENT: case GL_STENCIL_INDEX8:
        format = DkImageFormat_Z24S8;
        break;
    default: break;   // (colour: RGBA8)
    }
    const unsigned n = samples <= 1 ? 1 : samples <= 2 ? 2 : samples <= 4 ? 4 : 8;
    ReleaseImage(rb->image);
    rb->image = MakeImage(uint32_t(width), uint32_t(height), 1, 1, n, format, false, true, false);
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
    if (!p || *p == enable) return;   // (the other capabilities are ignored)
    *p = enable;
    switch (cap)
    {
    case GL_DEPTH_TEST: case GL_STENCIL_TEST: b.depthDirty = true; break;
    case GL_BLEND: b.blendDirty = true; break;
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
void APIENTRY PolygonMode(GLenum, GLenum mode)
{
    const DkPolygonMode m = mode == GL_LINE ? DkPolygonMode_Line : mode == GL_POINT ? DkPolygonMode_Point : DkPolygonMode_Fill;
    if (s.polygonMode != m) { s.polygonMode = m; b.rasterDirty = true; }
}
void APIENTRY BlendFuncSeparate(GLenum src, GLenum dst, GLenum srcA, GLenum dstA)
{
    if (s.blendSrc == src && s.blendDst == dst && s.blendSrcAlpha == srcA && s.blendDstAlpha == dstA) return;
    s.blendSrc = src; s.blendDst = dst; s.blendSrcAlpha = srcA; s.blendDstAlpha = dstA;
    b.blendDirty = true;
}
void APIENTRY BlendFunc(GLenum src, GLenum dst) { BlendFuncSeparate(src, dst, src, dst); }
void APIENTRY BlendEquationSeparate(GLenum rgb, GLenum alpha)
{
    if (s.blendEq == rgb && s.blendEqAlpha == alpha) return;
    s.blendEq = rgb; s.blendEqAlpha = alpha;
    b.blendDirty = true;
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
    if (s.colorMask != mask) { s.colorMask = mask; b.colorWriteDirty = true; }
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
// (the device has the GL_LOWER_LEFT / GL_ZERO_TO_ONE behaviour the game asks for: see "Coordinates")
void APIENTRY ClipControl(GLenum, GLenum) {}
void APIENTRY MemoryBarrier(GLbitfield) {}
void APIENTRY Flush() { Submit(); }

// ------------------------------------------------------------------------------------------------ state application
DkCompareOp Compare(GLenum f)
{
    switch (f)
    {
    case GL_NEVER: return DkCompareOp_Never; case GL_LESS: return DkCompareOp_Less; case GL_EQUAL: return DkCompareOp_Equal;
    case GL_LEQUAL: return DkCompareOp_Lequal; case GL_GREATER: return DkCompareOp_Greater; case GL_NOTEQUAL: return DkCompareOp_NotEqual;
    case GL_GEQUAL: return DkCompareOp_Gequal; default: return DkCompareOp_Always;
    }
}
DkStencilOp StencilOpOf(GLenum op)
{
    switch (op)
    {
    case GL_ZERO: return DkStencilOp_Zero; case GL_REPLACE: return DkStencilOp_Replace; case GL_INCR: return DkStencilOp_Incr;
    case GL_DECR: return DkStencilOp_Decr; case GL_INVERT: return DkStencilOp_Invert; case GL_INCR_WRAP: return DkStencilOp_IncrWrap;
    case GL_DECR_WRAP: return DkStencilOp_DecrWrap; default: return DkStencilOp_Keep;
    }
}
DkBlendFactor Factor(GLenum f)
{
    switch (f)
    {
    case GL_ZERO: return DkBlendFactor_Zero; case GL_ONE: return DkBlendFactor_One;
    case GL_SRC_COLOR: return DkBlendFactor_SrcColor; case GL_ONE_MINUS_SRC_COLOR: return DkBlendFactor_InvSrcColor;
    case GL_SRC_ALPHA: return DkBlendFactor_SrcAlpha; case GL_ONE_MINUS_SRC_ALPHA: return DkBlendFactor_InvSrcAlpha;
    case GL_DST_ALPHA: return DkBlendFactor_DstAlpha; case GL_ONE_MINUS_DST_ALPHA: return DkBlendFactor_InvDstAlpha;
    case GL_DST_COLOR: return DkBlendFactor_DstColor; case GL_ONE_MINUS_DST_COLOR: return DkBlendFactor_InvDstColor;
    case GL_SRC_ALPHA_SATURATE: return DkBlendFactor_SrcAlphaSaturate;
    case GL_CONSTANT_COLOR: return DkBlendFactor_ConstColor; case GL_ONE_MINUS_CONSTANT_COLOR: return DkBlendFactor_InvConstColor;
    case GL_CONSTANT_ALPHA: return DkBlendFactor_ConstAlpha; case GL_ONE_MINUS_CONSTANT_ALPHA: return DkBlendFactor_InvConstAlpha;
    default: return DkBlendFactor_One;
    }
}
DkBlendOp BlendOpOf(GLenum op)
{
    switch (op)
    {
    case GL_FUNC_SUBTRACT: return DkBlendOp_Sub; case GL_FUNC_REVERSE_SUBTRACT: return DkBlendOp_RevSub;
    case GL_MIN: return DkBlendOp_Min; case GL_MAX: return DkBlendOp_Max; default: return DkBlendOp_Add;
    }
}

void ApplyTarget()
{
    if (!b.targetDirty) return;
    DkCmdBuf cmd = Cmd();
    DkImageView colorView{}, depthView{};
    const DkImageView* color = nullptr;
    const DkImageView* depth = nullptr;
    const GLuint fbo = s.drawFramebuffer;
    if (!fbo)
    {
        AcquireSwapImage();
        dkImageViewDefaults(&colorView, &g_swapImages[size_t(g_swapSlot)]);
        color = &colorView;
        b.width = g_outputWidth;
        b.height = g_outputHeight;
        b.samples = 1;
    }
    else
    {
        Framebuffer* f = g_framebuffers.Get(fbo);
        Image* ci = f ? AttachmentImage(f->color) : nullptr;
        Image* di = f ? AttachmentImage(f->depth) : nullptr;
        const Image* dims = ci ? ci : di;
        if (!dims) b.width = b.height = 1;   // (an incomplete framebuffer)
        else
        {
            const unsigned level = ci ? f->color.level : f->depth.level;
            b.width = std::max(1u, dims->width >> level);
            b.height = std::max(1u, dims->height >> level);
            b.samples = dims->samples;
        }
        if (ci && f->drawColor) { colorView = View(*ci, f->color.level, f->color.layer); color = &colorView; }
        if (di) { depthView = View(*di, f->depth.level, f->depth.layer); depth = &depthView; }
    }
    // The same images bound again (after a blit, a framebuffer rebound): nothing to do. Otherwise the fragments drawn
    // so far finish before the new target (it may sample the old one); deko3d invalidates Zcull itself when the depth
    // target changes, so a rebind keeps it.
    // (by GPU address: an image struct freed and made again at the same address within the frame is another image)
    const struct Signature { DkGpuAddr color, depth; uint32_t colorSub, depthSub; } signature{
        color ? dkImageGetGpuAddr(color->pImage) : 0, depth ? dkImageGetGpuAddr(depth->pImage) : 0,
        color ? (uint32_t(color->layerOffset) << 8 | color->mipLevelOffset) : 0u,
        depth ? (uint32_t(depth->layerOffset) << 8 | depth->mipLevelOffset) : 0u};
    static Signature s_bound{};
    static uint64_t s_boundSerial = 0;
    if (b.fbo == ~0u || s_boundSerial != Cur().serial || std::memcmp(&s_bound, &signature, sizeof(signature)) != 0)
    {
        dkCmdBufBarrier(cmd, DkBarrier_Fragments, DkInvalidateFlags_Image);
        dkCmdBufBindRenderTargets(cmd, color ? &color : nullptr, color ? 1 : 0, depth);
        s_bound = signature;
        s_boundSerial = Cur().serial;
    }
    const bool window = fbo == 0;
    if (window != b.window || b.fbo == ~0u) b.rasterDirty = true;
    b.window = window;
    b.hasColor = color != nullptr;
    b.hasDepth = depth != nullptr;
    b.fbo = fbo;
    b.targetDirty = false;
    b.viewportDirty = b.depthDirty = b.msDirty = true;
}

void ApplyViewport()
{
    if (!b.viewportDirty) return;
    DkCmdBuf cmd = Cmd();
    const int H = int(b.height), W = int(b.width);
    // (GL's rectangles are the memory's rows in the lower-left mode: see "Coordinates")
    const DkViewport viewport{float(s.viewport[0]), float(s.viewport[1]), float(s.viewport[2]), float(s.viewport[3]), s.depthNear, s.depthFar};
    dkCmdBufSetViewports(cmd, 0, &viewport, 1);
    int x0 = 0, y0 = 0, x1 = W, y1 = H;
    if (s.scissorTest)
    {
        x0 = std::clamp(s.scissor[0], 0, W);
        x1 = int(std::clamp<int64_t>(int64_t(s.scissor[0]) + s.scissor[2], x0, W));
        y0 = std::clamp(s.scissor[1], 0, H);
        y1 = int(std::clamp<int64_t>(int64_t(s.scissor[1]) + s.scissor[3], y0, H));
    }
    const DkScissor scissor{uint32_t(x0), uint32_t(y0), uint32_t(x1 - x0), uint32_t(y1 - y0)};
    dkCmdBufSetScissors(cmd, 0, &scissor, 1);
    b.viewportDirty = false;
}

void ApplyFixedFunction()
{
    DkCmdBuf cmd = Cmd();
    if (b.msDirty)
    {
        DkMultisampleState ms;
        dkMultisampleStateDefaults(&ms);
        ms.mode = ms.rasterizerMode = MsMode(b.samples);
        dkMultisampleStateSetLocations(&ms, nullptr, 0);
        dkCmdBufBindMultisampleState(cmd, &ms);
        b.msDirty = false;
    }
    if (b.rasterDirty)
    {
        DkRasterizerState r;
        dkRasterizerStateDefaults(&r);
        r.cullMode = !s.cull ? DkFace_None : s.cullFace == GL_FRONT ? DkFace_Front : s.cullFace == GL_FRONT_AND_BACK ? DkFace_FrontAndBack : DkFace_Back;
        // (the lower-left mode's front-face test is GL's: see "Coordinates")
        r.frontFace = s.frontFace == GL_CW ? DkFrontFace_CW : DkFrontFace_CCW;
        r.polygonModeFront = r.polygonModeBack = s.polygonMode;
        dkCmdBufBindRasterizerState(cmd, &r);
        b.rasterDirty = false;
    }
    if (b.depthDirty)
    {
        DkDepthStencilState d;
        dkDepthStencilStateDefaults(&d);
        d.depthTestEnable = s.depthTest && b.hasDepth;
        d.depthWriteEnable = s.depthTest && s.depthWrite && b.hasDepth;
        d.depthCompareOp = Compare(s.depthFunc);
        d.stencilTestEnable = s.stencilTest && b.hasDepth;
        d.stencilFrontFailOp = StencilOpOf(s.front.fail);
        d.stencilFrontDepthFailOp = StencilOpOf(s.front.zfail);
        d.stencilFrontPassOp = StencilOpOf(s.front.zpass);
        d.stencilFrontCompareOp = Compare(s.front.func);
        d.stencilBackFailOp = StencilOpOf(s.back.fail);
        d.stencilBackDepthFailOp = StencilOpOf(s.back.zfail);
        d.stencilBackPassOp = StencilOpOf(s.back.zpass);
        d.stencilBackCompareOp = Compare(s.back.func);
        dkCmdBufBindDepthStencilState(cmd, &d);
        b.depthDirty = false;
    }
    if (b.stencilDirty)
    {
        dkCmdBufSetStencil(cmd, DkFace_Front, uint8_t(s.front.writeMask), uint8_t(s.front.ref), uint8_t(s.front.mask));
        dkCmdBufSetStencil(cmd, DkFace_Back, uint8_t(s.back.writeMask), uint8_t(s.back.ref), uint8_t(s.back.mask));
        b.stencilDirty = false;
    }
    if (b.blendDirty)
    {
        DkColorState c;
        dkColorStateDefaults(&c);
        c.blendEnableMask = s.blend ? 1 : 0;
        dkCmdBufBindColorState(cmd, &c);
        // Target 0's blend as raw Maxwell words, not dkCmdBufBindBlendState: deko3d 0.5.0 never enables separate alpha
        // blending and writes the colour's destination factor as the alpha's (also in sly1/deko/backend.cpp). Method
        // 0x780's seven registers: separate alpha, colour op / source / destination, alpha op / source / destination.
        auto factor = [](GLenum f) {
            const uint32_t v = uint32_t(Factor(f));
            return (v & 0x1F) | ((v & 0x20) ? 0xC000u : 0u);
        };
        const uint32_t words[8] = {
            (1u << 29) | (7u << 16) | (0u << 13) | 0x780u,   // (increasing, 7 words, 3D subchannel 0, method 0x780)
            1u,
            uint32_t(BlendOpOf(s.blendEq)), factor(s.blendSrc), factor(s.blendDst),
            uint32_t(BlendOpOf(s.blendEqAlpha)), factor(s.blendSrcAlpha), factor(s.blendDstAlpha)};
        dkCmdBufReplayCmds(cmd, words, 8);
        b.blendDirty = false;
    }
    if (b.colorWriteDirty)
    {
        DkColorWriteState w;
        dkColorWriteStateDefaults(&w);
        dkColorWriteStateSetMask(&w, 0, s.colorMask);
        dkCmdBufBindColorWriteState(cmd, &w);
        b.colorWriteDirty = false;
    }
    if (b.blendConstDirty)
    {
        dkCmdBufSetBlendConst(cmd, s.blendColor[0], s.blendColor[1], s.blendColor[2], s.blendColor[3]);
        b.blendConstDirty = false;
    }
    if (b.lineDirty)
    {
        dkCmdBufSetLineWidth(cmd, s.lineWidth);
        b.lineDirty = false;
    }
}

// The texture handle a program's sampler reads from a unit (the white texture for an empty or incomplete unit).
GLuint g_whiteTexture = 0, g_whiteArray = 0;
DkResHandle TextureHandle(unsigned unit, bool array)
{
    Texture* t = g_textures.Get(s.units[unit][array ? 1 : 0]);
    if (!t || !t->image || t->image->descriptor < 0 || t->image->array != array) t = g_textures.Get(array ? g_whiteArray : g_whiteTexture);
    int slot;
    if (const GLuint sn = s.samplerUnits[unit])
    {
        Sampler* sm = g_samplers.Get(sn);
        if (sm->samplerSlot < 0) sm->samplerSlot = SamplerSlot(sm->params);
        slot = sm->samplerSlot;
    }
    else
    {
        if (t->samplerSlot < 0) t->samplerSlot = SamplerSlot(t->params);
        slot = t->samplerSlot;
    }
    return dkMakeTextureHandle(uint32_t(t->image->descriptor), uint32_t(slot));
}

DkBufExtents ResolveRange(const Range& r)
{
    Buffer* buf = g_buffers.Get(r.buffer);
    if (!buf || !buf->size || !buf->gpu) return {0, 0};
    uint32_t size = r.whole ? buf->size : r.size;
    if (uint64_t(r.offset) + size > buf->size) size = r.offset < buf->size ? buf->size - r.offset : 0;
    if (!size) return {0, 0};
    return {Use(*buf) + r.offset, size};
}

// A uniform block binding's range for a draw recorded now: a copy of the buffer's CPU copy in this frame's ring (the
// draw sees the contents as of now, as GL's in-order semantics want, however the game rewrites the buffer after),
// reused while neither the range nor the contents change.
DkBufExtents ResolveUniform(unsigned slot)
{
    const Range& r = s.uniforms[slot];
    Buffer* buf = r.buffer ? g_buffers.Get(r.buffer) : nullptr;
    if (!buf || !buf->size || r.offset >= buf->size) return {g_nullUniform.Gpu(), 256};
    uint32_t size = r.whole || !r.size ? buf->size - r.offset : std::min(r.size, buf->size - r.offset);
    size = std::min<uint32_t>(size, 0x10000);
    UniformUpload& u = g_uniformUploads[slot];
    const uint64_t serial = Cur().serial;
    if (u.buffer == r.buffer && u.version == buf->version && u.offset == r.offset && u.size == size && u.serial == serial)
        return u.extent;
    const uint32_t padded = Align(size, DK_UNIFORM_BUF_ALIGNMENT);
    const Upload up = Ring(padded, DK_UNIFORM_BUF_ALIGNMENT);
    const uint8_t* src = buf->keepShadow ? buf->shadow.data() + r.offset : buf->cpu + r.offset;
    std::memcpy(up.cpu, src, size);
    if (padded > size) std::memset(up.cpu + size, 0, padded - size);
    u = {r.buffer, buf->version, r.offset, size, serial, {up.gpu, padded}};
    return u.extent;
}

bool ApplyProgram()
{
    Program* p = g_programs.Get(s.program);
    if (!p) return false;
    DkCmdBuf cmd = Cmd();
    const Shaders::Program& info = *p->info;
    if (b.program != s.program)
    {
        const DkShader* shaders[3];
        unsigned n = 0;
        for (unsigned i = 0; i < 3; ++i) if (p->hasStage[i]) shaders[n++] = &p->shaders[i];
        dkCmdBufBindShaders(cmd, DkStageFlag_GraphicsMask, shaders, n);
        if (p->ubo)
            for (unsigned i = 0; i < 3; ++i)
                if (info.constantStages & (1u << i))
                    dkCmdBufBindUniformBuffer(cmd, DkStage(kStageIndex[i]), Shaders::ConstantBinding, p->ubo.Gpu(), p->uboSize);
        b.program = s.program;
    }
    if (p->dirtyLo < p->dirtyHi)
    {
        const uint32_t lo = p->dirtyLo & ~3u, hi = Align(p->dirtyHi, 4);
        dkCmdBufPushConstants(cmd, p->ubo.Gpu(), p->uboSize, lo, hi - lo, p->constants.data() + lo);
        p->dirtyLo = UINT32_MAX;
        p->dirtyHi = 0;
    }
    // (the storage buffers resolved again only when the program, a storage binding or a buffer's storage changed)
    const bool fStorage = b.storageProgram != s.program || b.storageEpoch != g_storageEpoch || b.storageAddrEpoch != g_addrEpoch;
    for (unsigned i = 0; i < 3; ++i)
    {
        unsigned mask = fStorage ? info.storageMask[i] : 0u;
        while (mask)
        {
            const unsigned slot = unsigned(std::countr_zero(mask));
            mask &= mask - 1;
            const DkBufExtents e = ResolveRange(s.storage[slot]);
            DkBufExtents& bound = b.storage[i][slot];
            if (bound.addr != e.addr || bound.size != e.size)
            {
                dkCmdBufBindStorageBuffer(cmd, DkStage(kStageIndex[i]), slot, e.addr, e.size);
                bound = e;
            }
        }
        // the uniform blocks the stage reads (the GLSL's own bindings)
        mask = info.uniformMask[i];
        while (mask)
        {
            const unsigned slot = unsigned(std::countr_zero(mask));
            mask &= mask - 1;
            const DkBufExtents e = ResolveUniform(slot);
            DkBufExtents& bound = b.uniforms[i][slot];
            if (bound.addr != e.addr || bound.size != e.size)
            {
                dkCmdBufBindUniformBuffer(cmd, DkStage(kStageIndex[i]), slot, e.addr, e.size);
                bound = e;
            }
        }
    }
    if (fStorage)
    {
        // (after the loop: a stream buffer moved by its Use counts as resolved)
        b.storageProgram = s.program;
        b.storageEpoch = g_storageEpoch;
        b.storageAddrEpoch = g_addrEpoch;
    }
    if (p->samplerCount)
    {
        if (g_descriptorsDirty)
        {
            // (new descriptor slots written since the last draw: the GPU's descriptor cache forgets the old)
            dkCmdBufBarrier(cmd, DkBarrier_None, DkInvalidateFlags_Descriptors);
            g_descriptorsDirty = false;
        }
        std::array<DkResHandle, kMaxBindings> handles{};
        for (unsigned k = 0; k < p->samplerCount; ++k) handles[k] = TextureHandle(p->samplerUnit[k], p->samplerArray[k]);
        for (unsigned i = 0; i < 3; ++i)
        {
            unsigned first = kMaxBindings, last = 0;
            for (unsigned k = 0; k < p->samplerCount; ++k)
                if ((p->samplerStages[k] & (1u << i)) && b.textures[i][k] != handles[k]) { first = std::min(first, k); last = k; }
            if (first > last) continue;
            dkCmdBufBindTextures(cmd, DkStage(kStageIndex[i]), first, handles.data() + first, last - first + 1);
            for (unsigned k = first; k <= last; ++k) b.textures[i][k] = handles[k];
        }
    }
    else if (g_descriptorsDirty)
    {
        dkCmdBufBarrier(cmd, DkBarrier_None, DkInvalidateFlags_Descriptors);
        g_descriptorsDirty = false;
    }
    return true;
}

DkVtxAttribSize AttribSize(unsigned components, unsigned bytes)
{
    static constexpr DkVtxAttribSize sizes[3][4] = {
        {DkVtxAttribSize_1x8, DkVtxAttribSize_2x8, DkVtxAttribSize_3x8, DkVtxAttribSize_4x8},
        {DkVtxAttribSize_1x16, DkVtxAttribSize_2x16, DkVtxAttribSize_3x16, DkVtxAttribSize_4x16},
        {DkVtxAttribSize_1x32, DkVtxAttribSize_2x32, DkVtxAttribSize_3x32, DkVtxAttribSize_4x32}};
    return sizes[bytes == 1 ? 0 : bytes == 2 ? 1 : 2][components - 1];
}
DkVtxAttribType AttribType(const Attrib& a)
{
    if (a.type == GL_FLOAT || a.type == GL_HALF_FLOAT) return DkVtxAttribType_Float;
    const bool sign = a.type == GL_BYTE || a.type == GL_SHORT || a.type == GL_INT;
    if (a.integer) return sign ? DkVtxAttribType_Sint : DkVtxAttribType_Uint;
    if (a.normalized) return sign ? DkVtxAttribType_Snorm : DkVtxAttribType_Unorm;
    return sign ? DkVtxAttribType_Sscaled : DkVtxAttribType_Uscaled;
}

void ApplyVertexInput()
{
    DkCmdBuf cmd = Cmd();
    VertexArray* vao = g_arrays.Get(s.vertexArray);
    if (b.genericDirty)
    {
        const Upload u = Ring(sizeof(s.generic), 16);
        std::memcpy(u.cpu, s.generic.data(), sizeof(s.generic));
        const DkBufExtents e{u.gpu, uint32_t(sizeof(s.generic))};
        dkCmdBufBindVtxBuffers(cmd, kGenericSlot, &e, 1);
        b.vertexBuffers[kGenericSlot] = e;
        b.genericDirty = false;
    }
    if (b.vao != vao || b.vaoVersion != vao->version)
    {
        std::array<DkVtxAttribState, kMaxAttribs> attribs{};
        std::array<DkVtxBufferState, kMaxAttribs> buffers{};
        for (unsigned i = 0; i < kMaxAttribs; ++i)
        {
            const Attrib& a = vao->attribs[i];
            DkVtxAttribState& st = attribs[i];
            if (i < kGenericSlot && a.enabled && a.buffer)
            {
                st.bufferId = i;
                st.offset = 0;
                st.size = AttribSize(a.components, ComponentBytes(a.type));
                st.type = AttribType(a);
                buffers[i] = {a.stride, a.divisor};
            }
            else
            {
                st.bufferId = kGenericSlot;
                st.offset = i * 16;
                st.size = DkVtxAttribSize_4x32;
                st.type = DkVtxAttribType_Float;
            }
        }
        buffers[kGenericSlot] = {0, 0};
        dkCmdBufBindVtxAttribState(cmd, attribs.data(), kMaxAttribs);
        dkCmdBufBindVtxBufferState(cmd, buffers.data(), kMaxAttribs);
        b.vao = vao;
        b.vaoVersion = vao->version;
    }
    // (the attributes' buffers resolved again only when the vertex array, its attributes or a buffer's storage changed)
    if (b.vertexVao == vao && b.vertexVaoVersion == vao->version && b.vertexAddrEpoch == g_addrEpoch) return;
    for (unsigned i = 0; i < kGenericSlot; ++i)
    {
        const Attrib& a = vao->attribs[i];
        if (!a.enabled || !a.buffer) continue;
        Buffer* buf = g_buffers.Get(a.buffer);
        DkBufExtents e{0, 0};
        if (buf && buf->gpu && a.offset < buf->size) e = {Use(*buf) + a.offset, buf->size - a.offset};
        DkBufExtents& bound = b.vertexBuffers[i];
        if (bound.addr != e.addr || bound.size != e.size)
        {
            dkCmdBufBindVtxBuffers(cmd, i, &e, 1);
            bound = e;
        }
    }
    b.vertexVao = vao;
    b.vertexVaoVersion = vao->version;
    b.vertexAddrEpoch = g_addrEpoch;
}

bool PrepareDraw()
{
    FlushCopies();
    ApplyTarget();
    ApplyViewport();
    ApplyFixedFunction();
    if (!ApplyProgram()) return false;
    ApplyVertexInput();
    return true;
}
void CountDraw()
{
    if (++g_drawsSinceSubmit >= kSubmitDrawThreshold) Submit();
}

DkPrimitive Primitive(GLenum mode)
{
    switch (mode)
    {
    case GL_POINTS: return DkPrimitive_Points; case GL_LINES: return DkPrimitive_Lines; case GL_LINE_LOOP: return DkPrimitive_LineLoop;
    case GL_LINE_STRIP: return DkPrimitive_LineStrip; case GL_TRIANGLE_STRIP: return DkPrimitive_TriangleStrip;
    case GL_TRIANGLE_FAN: return DkPrimitive_TriangleFan; default: return DkPrimitive_Triangles;
    }
}

void APIENTRY DrawArraysInstancedBaseInstance(GLenum mode, GLint first, GLsizei count, GLsizei instances, GLuint baseInstance)
{
    if (count <= 0 || instances <= 0 || first < 0) return;
    if (!PrepareDraw()) return;
    dkCmdBufDraw(Cmd(), Primitive(mode), uint32_t(count), uint32_t(instances), uint32_t(first), baseInstance);
    CountDraw();
}
void APIENTRY DrawArrays(GLenum mode, GLint first, GLsizei count) { DrawArraysInstancedBaseInstance(mode, first, count, 1, 0); }
void APIENTRY DrawArraysInstanced(GLenum mode, GLint first, GLsizei count, GLsizei instances) { DrawArraysInstancedBaseInstance(mode, first, count, instances, 0); }

// The bound element buffer for an indexed draw; false when there is none.
bool BindIndices(GLenum type)
{
    VertexArray* vao = g_arrays.Get(s.vertexArray);
    Buffer* buf = g_buffers.Get(vao->elements);
    if (!buf || !buf->gpu) return false;
    const DkIdxFormat format = type == GL_UNSIGNED_SHORT ? DkIdxFormat_Uint16 : type == GL_UNSIGNED_BYTE ? DkIdxFormat_Uint8 : DkIdxFormat_Uint32;
    const DkGpuAddr addr = Use(*buf);
    if (b.indexAddress != addr || b.indexFormat != format)
    {
        dkCmdBufBindIdxBuffer(Cmd(), format, addr);
        b.indexAddress = addr;
        b.indexFormat = format;
    }
    return true;
}
void APIENTRY DrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instances, GLint baseVertex, GLuint baseInstance)
{
    if (count <= 0 || instances <= 0) return;
    if (!PrepareDraw() || !BindIndices(type)) return;
    const uint32_t first = uint32_t(reinterpret_cast<uintptr_t>(indices) / ComponentBytes(type));
    dkCmdBufDrawIndexed(Cmd(), Primitive(mode), uint32_t(count), uint32_t(instances), first, baseVertex, baseInstance);
    CountDraw();
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
    if (!buf || !buf->keepShadow) return;
    if (!PrepareDraw() || !BindIndices(type)) return;
    struct Command { uint32_t count, instances, first; int32_t baseVertex; uint32_t baseInstance; };
    const size_t step = stride ? size_t(stride) : sizeof(Command);
    size_t offset = size_t(reinterpret_cast<uintptr_t>(indirect));
    if (offset + step * size_t(drawCount - 1) + sizeof(Command) > buf->shadow.size()) { SetError(GL_INVALID_OPERATION); return; }
    DkCmdBuf cmd = Cmd();
    const DkPrimitive prim = Primitive(mode);
    for (GLsizei i = 0; i < drawCount; ++i, offset += step)
    {
        Command c;
        std::memcpy(&c, buf->shadow.data() + offset, sizeof(c));
        if (c.count && c.instances) dkCmdBufDrawIndexed(cmd, prim, c.count, c.instances, c.first, c.baseVertex, c.baseInstance);
    }
    CountDraw();
}
void APIENTRY MultiDrawArraysIndirect(GLenum mode, const void* indirect, GLsizei drawCount, GLsizei stride)
{
    if (drawCount <= 0) return;
    Buffer* buf = g_buffers.Get(s.indirectBuffer);
    if (!buf || !buf->keepShadow) return;
    if (!PrepareDraw()) return;
    struct Command { uint32_t count, instances, first, baseInstance; };
    const size_t step = stride ? size_t(stride) : sizeof(Command);
    size_t offset = size_t(reinterpret_cast<uintptr_t>(indirect));
    if (offset + step * size_t(drawCount - 1) + sizeof(Command) > buf->shadow.size()) return;
    for (GLsizei i = 0; i < drawCount; ++i, offset += step)
    {
        Command c;
        std::memcpy(&c, buf->shadow.data() + offset, sizeof(c));
        if (c.count && c.instances) dkCmdBufDraw(Cmd(), Primitive(mode), c.count, c.instances, c.first, c.baseInstance);
    }
    CountDraw();
}

// ------------------------------------------------------------------------------------------------ clears and blits
void APIENTRY Clear(GLbitfield mask)
{
    FlushCopies();
    ApplyTarget();
    DkCmdBuf cmd = Cmd();
    // (GL clears ignore the viewport: the scissor alone limits them)
    const GLint saved[4] = {s.viewport[0], s.viewport[1], s.viewport[2], s.viewport[3]};
    s.viewport[0] = 0; s.viewport[1] = 0; s.viewport[2] = GLint(b.width); s.viewport[3] = GLint(b.height);
    b.viewportDirty = true;
    ApplyViewport();
    if ((mask & GL_COLOR_BUFFER_BIT) && b.hasColor && s.colorMask)
        dkCmdBufClearColor(cmd, 0, s.colorMask, s.clearColor.data());
    const bool depth = (mask & GL_DEPTH_BUFFER_BIT) && s.depthWrite;
    const bool stencil = (mask & GL_STENCIL_BUFFER_BIT) && s.front.writeMask;
    if ((depth || stencil) && b.hasDepth)
        dkCmdBufClearDepthStencil(cmd, depth, s.clearDepth, stencil ? uint8_t(s.front.writeMask) : 0, uint8_t(s.clearStencil));
    std::memcpy(s.viewport, saved, sizeof(saved));
    b.viewportDirty = true;
}
void APIENTRY ClearBufferfv(GLenum buffer, GLint, const GLfloat* value)
{
    if (buffer != GL_COLOR || !value) return;
    const auto saved = s.clearColor;
    s.clearColor = {value[0], value[1], value[2], value[3]};
    Clear(GL_COLOR_BUFFER_BIT);
    s.clearColor = saved;
}

// The color image of a framebuffer for blits and reads (framebuffer 0: the swapchain image).
bool ColorImageOf(GLuint fbo, DkImageView* view, uint32_t* width, uint32_t* height, uint32_t* samples)
{
    if (!fbo)
    {
        AcquireSwapImage();
        dkImageViewDefaults(view, &g_swapImages[size_t(g_swapSlot)]);
        *width = g_outputWidth;
        *height = g_outputHeight;
        *samples = 1;
        return true;
    }
    Framebuffer* f = g_framebuffers.Get(fbo);
    Image* image = f ? AttachmentImage(f->color) : nullptr;
    if (!image) return false;
    *view = View(*image, f->color.level, f->color.layer);
    *width = std::max(1u, image->width >> f->color.level);
    *height = std::max(1u, image->height >> f->color.level);
    *samples = image->samples;
    return true;
}
void APIENTRY BlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0, GLint dy0, GLint dx1, GLint dy1, GLbitfield mask, GLenum filter)
{
    if (!(mask & GL_COLOR_BUFFER_BIT)) return;   // (depth / stencil blits are not supported)
    FlushCopies();
    DkImageView src, dst;
    uint32_t sw, sh, ss, dw, dh, ds;
    if (!ColorImageOf(s.readFramebuffer, &src, &sw, &sh, &ss) || !ColorImageOf(s.drawFramebuffer, &dst, &dw, &dh, &ds)) return;
    DkCmdBuf cmd = Cmd();
    dkCmdBufBarrier(cmd, DkBarrier_Primitives, 0);
    uint32_t flags = filter == GL_LINEAR ? DkBlitFlag_FilterLinear : DkBlitFlag_FilterNearest;
    // (GL's mirrored rectangles: the 2D engine's flips; the rows are GL's in every framebuffer, see "Coordinates")
    if (sx1 < sx0) { std::swap(sx0, sx1); flags ^= DkBlitFlag_FlipX; }
    if (dx1 < dx0) { std::swap(dx0, dx1); flags ^= DkBlitFlag_FlipX; }
    if (sy1 < sy0) { std::swap(sy0, sy1); flags ^= DkBlitFlag_FlipY; }
    if (dy1 < dy0) { std::swap(dy0, dy1); flags ^= DkBlitFlag_FlipY; }
    sx0 = std::clamp(sx0, 0, GLint(sw)); sx1 = std::clamp(sx1, 0, GLint(sw)); sy0 = std::clamp(sy0, 0, GLint(sh)); sy1 = std::clamp(sy1, 0, GLint(sh));
    dx0 = std::clamp(dx0, 0, GLint(dw)); dx1 = std::clamp(dx1, 0, GLint(dw)); dy0 = std::clamp(dy0, 0, GLint(dh)); dy1 = std::clamp(dy1, 0, GLint(dh));
    if (sx1 <= sx0 || sy1 <= sy0 || dx1 <= dx0 || dy1 <= dy0) return;
    if (ss > 1)
    {
        // a multisample resolve (same rectangle: the game resolves its scene target in place; no scaled resolves)
        if (ds == 1 && sx0 == 0 && sy0 == 0 && dx0 == 0 && dy0 == 0 && uint32_t(sx1) == sw && uint32_t(sy1) == sh && sw == dw && sh == dh)
            dkCmdBufResolveImage(cmd, &src, &dst);
    }
    else
    {
        const DkImageRect srcRect{uint32_t(sx0), uint32_t(sy0), 0, uint32_t(sx1 - sx0), uint32_t(sy1 - sy0), 1};
        const DkImageRect dstRect{uint32_t(dx0), uint32_t(dy0), 0, uint32_t(dx1 - dx0), uint32_t(dy1 - dy0), 1};
        dkCmdBufBlitImage(cmd, &src, &srcRect, &dst, &dstRect, flags, 0);
    }
    dkCmdBufBarrier(cmd, DkBarrier_Primitives, DkInvalidateFlags_Image);
    b.targetDirty = true;
}

void Finish()
{
    if (!g_recording) return;
    Submit();
    dkQueueWaitIdle(g_queue);
}
void APIENTRY FinishGL() { Finish(); }

void APIENTRY ReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format, GLenum type, void* pixels)
{
    if (!pixels || width <= 0 || height <= 0) return;
    if (format != GL_RGBA || type != GL_UNSIGNED_BYTE) { std::memset(pixels, 0, size_t(width) * size_t(height) * 4); return; }
    FlushCopies();
    DkImageView view;
    uint32_t w, h, n;
    if (!ColorImageOf(s.readFramebuffer, &view, &w, &h, &n) || n > 1) return;
    x = std::clamp(x, 0, GLint(w)); y = std::clamp(y, 0, GLint(h));
    width = std::min<GLsizei>(width, GLsizei(w) - x); height = std::min<GLsizei>(height, GLsizei(h) - y);
    if (width <= 0 || height <= 0) return;
    const int yImage = y;   // (GL's rows: see "Coordinates")
    const uint32_t pitch = Align(uint32_t(width) * 4, DK_IMAGE_LINEAR_STRIDE_ALIGNMENT);
    const uint32_t bytes = pitch * uint32_t(height);
    if (bytes > g_readbackSize)
    {
        if (g_readback) { Finish(); dkMemBlockDestroy(g_readback); }
        g_readbackSize = Align(bytes, DK_MEMBLOCK_ALIGNMENT);
        g_readback = NewMemBlock(g_readbackSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    }
    DkCmdBuf cmd = Cmd();
    dkCmdBufBarrier(cmd, DkBarrier_Primitives, 0);
    const DkImageRect rect{uint32_t(x), uint32_t(yImage), 0, uint32_t(width), uint32_t(height), 1};
    const DkCopyBuf dst{dkMemBlockGetGpuAddr(g_readback), pitch, 0};
    dkCmdBufCopyImageToBuffer(cmd, &view, &rect, &dst, 0);
    Finish();
    const auto* src = static_cast<const uint8_t*>(dkMemBlockGetCpuAddr(g_readback));
    const uint32_t outPitch = Align(uint32_t(width) * 4, uint32_t(std::max(1, s.packAlignment)));
    for (GLsizei row = 0; row < height; ++row)
        std::memcpy(static_cast<uint8_t*>(pixels) + size_t(row) * outPitch, src + size_t(row) * pitch, size_t(width) * 4);
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
    Garbage(p->ubo);
    if (s.program == program) s.program = 0;
    if (b.program == program) b.program = ~0u;
}
// glUniform*: the value into the program's constant block (or a sampler's unit).
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
// (the blocks keep the GLSL's own layout(binding): a block's index is its binding, and a program cannot rebind one)
GLuint APIENTRY GetUniformBlockIndex(GLuint program, const GLchar* name)
{
    const Program* p = g_programs.Get(program);
    if (!p || !p->info || !name) return GL_INVALID_INDEX;
    for (uint32_t i = 0; i < p->info->blockCount; ++i)
        if (!std::strcmp(p->info->blocks[i].name, name)) return p->info->blocks[i].binding;
    return GL_INVALID_INDEX;
}
void APIENTRY UniformBlockBinding(GLuint, GLuint, GLuint) {}
// (the GL shader objects: no run-time compiler; gl.cpp asks CreateProgram for its programs instead)
GLuint APIENTRY CreateShader(GLenum) { return 0; }
void APIENTRY DeleteShader(GLuint) {}
void APIENTRY ShaderSource(GLuint, GLsizei, const GLchar* const*, const GLint*) {}
void APIENTRY CompileShader(GLuint) {}
void APIENTRY GetShaderiv(GLuint, GLenum pname, GLint* v) { if (v) *v = pname == GL_COMPILE_STATUS ? GL_FALSE : 0; }
void APIENTRY GetShaderInfoLog(GLuint, GLsizei size, GLsizei* length, GLchar* log)
{
    static const char kMessage[] = "no run-time GLSL compiler on deko3d (programs are compiled offline)";
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
}
void APIENTRY GetProgramInfoLog(GLuint p, GLsizei size, GLsizei* length, GLchar* log) { GetShaderInfoLog(p, size, length, log); }

// ------------------------------------------------------------------------------------------------ queries and syncs
void APIENTRY GenQueries(GLsizei n, GLuint* names)
{
    for (GLsizei i = 0; i < n; ++i)
    {
        names[i] = g_queries.Gen();
        g_queries.Get(names[i])->memory = NewMemBlock(DK_MEMBLOCK_ALIGNMENT, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    }
}
void APIENTRY DeleteQueries(GLsizei n, const GLuint* names)
{
    for (GLsizei i = 0; i < n; ++i)
        if (auto q = g_queries.Take(names[i]))
        {
            Finish();
            dkMemBlockDestroy(q->memory);
        }
}
GLuint g_activeQuery = 0;
void APIENTRY BeginQuery(GLenum target, GLuint name)
{
    Query* q = g_queries.Get(name);
    if (!q || target != GL_TIME_ELAPSED) return;
    dkCmdBufReportCounter(Cmd(), DkCounter_Timestamp, dkMemBlockGetGpuAddr(q->memory));
    q->serial = Cur().serial;
    g_activeQuery = name;
}
void APIENTRY EndQuery(GLenum)
{
    Query* q = g_queries.Get(g_activeQuery);
    if (!q) return;
    dkCmdBufReportCounter(Cmd(), DkCounter_Timestamp, dkMemBlockGetGpuAddr(q->memory) + 16);
    g_activeQuery = 0;
}
void APIENTRY GetQueryObjectui64v(GLuint name, GLenum pname, GLuint64* value)
{
    Query* q = g_queries.Get(name);
    if (!q || !value) return;
    if (pname == GL_QUERY_RESULT_AVAILABLE) { *value = 1; return; }
    // (a result from a frame still on the GPU: wait for it)
    for (Frame& f : g_frames)
        if (f.submitted && f.serial == q->serial) dkFenceWait(&f.fence, -1);
    if (g_recording && Cur().serial == q->serial) Finish();
    const auto* report = static_cast<const uint64_t*>(dkMemBlockGetCpuAddr(q->memory));
    *value = report[3] > report[1] ? dkTimestampToNs(report[3] - report[1]) : 0;
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
    dkCmdBufSignalFence(Cmd(), &sync->fence, false);
    sync->serial = Cur().serial;
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
    // (a fence in the list being recorded is submitted first, or the wait could never end)
    if (g_recording && sync->serial == Cur().serial)
    {
        if (!(flags & GL_SYNC_FLUSH_COMMANDS_BIT) && timeout == 0) return GL_TIMEOUT_EXPIRED;
        Submit();
    }
    const DkResult r = dkFenceWait(&sync->fence, timeout == GL_TIMEOUT_IGNORED ? -1 : int64_t(std::min<uint64_t>(timeout, INT64_MAX)));
    return r == DkResult_Success ? GL_CONDITION_SATISFIED : r == DkResult_Timeout ? GL_TIMEOUT_EXPIRED : GL_WAIT_FAILED;
}
void APIENTRY WaitSync(GLsync, GLbitfield, GLuint64) {}
void APIENTRY DeleteSync(GLsync handle)
{
    g_syncs.erase(std::remove_if(g_syncs.begin(), g_syncs.end(), [&](const auto& p) { return reinterpret_cast<GLsync>(p.get()) == handle; }), g_syncs.end());
}

// ------------------------------------------------------------------------------------------------ queries
GLenum APIENTRY GetError() { const GLenum e = s.error; s.error = GL_NO_ERROR; return e; }
const char* const kExtensions[] = {"GL_EXT_texture_filter_anisotropic", "GL_ARB_texture_filter_anisotropic", "GL_ARB_clip_control",
                                   "GL_ARB_multi_draw_indirect", "GL_ARB_base_instance", "GL_ARB_shader_storage_buffer_object"};
const GLubyte* APIENTRY GetString(GLenum name)
{
    const char* v = "";
    switch (name)
    {
    case GL_VENDOR: v = "NVIDIA (deko3d)"; break;
    case GL_RENDERER: v = "Tegra X1 (Maxwell GM20B) via deko3d"; break;
    case GL_VERSION: v = "4.3.0 SafeHouse deko3d"; break;
    case GL_SHADING_LANGUAGE_VERSION: v = "4.30 (offline DKSH)"; break;
    default: break;
    }
    return reinterpret_cast<const GLubyte*>(v);
}
const GLubyte* APIENTRY GetStringi(GLenum name, GLuint index)
{
    if (name == GL_EXTENSIONS && index < std::size(kExtensions)) return reinterpret_cast<const GLubyte*>(kExtensions[index]);
    return nullptr;
}
void APIENTRY GetIntegerv(GLenum pname, GLint* v)
{
    if (!v) return;
    switch (pname)
    {
    case GL_NUM_EXTENSIONS: *v = GLint(std::size(kExtensions)); break;
    case GL_MAJOR_VERSION: *v = 4; break;
    case GL_MINOR_VERSION: *v = 3; break;
    case GL_MAX_SAMPLES: case GL_MAX_COLOR_TEXTURE_SAMPLES: case GL_MAX_DEPTH_TEXTURE_SAMPLES: *v = 8; break;
    case GL_MAX_TEXTURE_SIZE: case GL_MAX_RENDERBUFFER_SIZE: *v = 16384; break;
    case GL_MAX_ARRAY_TEXTURE_LAYERS: *v = 2048; break;
    case GL_MAX_TEXTURE_IMAGE_UNITS: case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: *v = GLint(kMaxUnits); break;
    case GL_MAX_VERTEX_ATTRIBS: *v = GLint(kGenericSlot); break;
    case GL_MAX_SHADER_STORAGE_BUFFER_BINDINGS: case GL_MAX_UNIFORM_BUFFER_BINDINGS: *v = GLint(kMaxBindings) - 1; break;
    case GL_SHADER_STORAGE_BUFFER_OFFSET_ALIGNMENT: *v = 32; break;
    case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT: *v = DK_UNIFORM_BUF_ALIGNMENT; break;
    case GL_MAX_SHADER_STORAGE_BLOCK_SIZE: *v = 0x7FFFFFFF; break;
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
    case GL_POLYGON_MODE: v[0] = v[1] = s.polygonMode == DkPolygonMode_Line ? GL_LINE : s.polygonMode == DkPolygonMode_Point ? GL_POINT : GL_FILL; break;
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
    case kMaxAnisotropyEnum: *v = 16.0f; break;
    case GL_LINE_WIDTH: *v = s.lineWidth; break;
    case GL_POINT_SIZE: *v = 1.0f; break;   // (the programs write gl_PointSize themselves)
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
CANE_CHECK(CompressedTexSubImage2D, PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC); CANE_CHECK(CompressedTexImage2D, PFNGLCOMPRESSEDTEXIMAGE2DPROC);
CANE_CHECK(GetTexImage, PFNGLGETTEXIMAGEPROC); CANE_CHECK(GetUniformBlockIndex, PFNGLGETUNIFORMBLOCKINDEXPROC);
CANE_CHECK(UniformBlockBinding, PFNGLUNIFORMBLOCKBINDINGPROC);
CANE_CHECK(BlitFramebuffer, PFNGLBLITFRAMEBUFFERPROC); CANE_CHECK(ReadPixels, PFNGLREADPIXELSPROC);
CANE_CHECK(DrawElementsInstancedBaseVertexBaseInstance, PFNGLDRAWELEMENTSINSTANCEDBASEVERTEXBASEINSTANCEPROC);
CANE_CHECK(MultiDrawElementsIndirect, PFNGLMULTIDRAWELEMENTSINDIRECTPROC); CANE_CHECK(UniformMatrix4fv, PFNGLUNIFORMMATRIX4FVPROC);
CANE_CHECK(FenceSync, PFNGLFENCESYNCPROC); CANE_CHECK(ClientWaitSync, PFNGLCLIENTWAITSYNCPROC); CANE_CHECK(GetQueryObjectui64v, PFNGLGETQUERYOBJECTUI64VPROC);
CANE_CHECK(StencilOpSeparate, PFNGLSTENCILOPSEPARATEPROC); CANE_CHECK(BlendColor, PFNGLBLENDCOLORPROC);
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
    P("GenBuffers", GenBuffers), P("DeleteBuffers", DeleteBuffers), P("BindBuffer", BindBuffer), P("BindBufferBase", BindBufferBase),
    P("BindBufferRange", BindBufferRange), P("BufferData", BufferData), P("BufferSubData", BufferSubData), P("MapBufferRange", MapBufferRange),
    P("UnmapBuffer", UnmapBuffer), P("IsBuffer", IsBuffer),
    P("GenTextures", GenTextures), P("DeleteTextures", DeleteTextures), P("ActiveTexture", ActiveTexture), P("BindTexture", BindTexture),
    P("TexImage2D", TexImage2D), P("TexStorage2D", TexStorage2D), P("TexStorage3D", TexStorage3D), P("TexSubImage2D", TexSubImage2D),
    P("CompressedTexSubImage2D", CompressedTexSubImage2D), P("CompressedTexImage2D", CompressedTexImage2D), P("GetTexImage", GetTexImage),
    P("TexSubImage3D", TexSubImage3D), P("TexParameteri", TexParameteri), P("TexParameterf", TexParameterf), P("GenerateMipmap", GenerateMipmap),
    P("GetTexLevelParameteriv", GetTexLevelParameteriv), P("IsTexture", IsTexture),
    P("GenSamplers", GenSamplers), P("DeleteSamplers", DeleteSamplers), P("BindSampler", BindSampler),
    P("SamplerParameteri", SamplerParameteri), P("SamplerParameterf", SamplerParameterf),
    P("GenRenderbuffers", GenRenderbuffers), P("DeleteRenderbuffers", DeleteRenderbuffers), P("BindRenderbuffer", BindRenderbuffer),
    P("RenderbufferStorage", RenderbufferStorage), P("RenderbufferStorageMultisample", RenderbufferStorageMultisample),
    P("GenVertexArrays", GenVertexArrays), P("DeleteVertexArrays", DeleteVertexArrays), P("BindVertexArray", BindVertexArray),
    P("EnableVertexAttribArray", EnableVertexAttribArray), P("DisableVertexAttribArray", DisableVertexAttribArray),
    P("VertexAttribPointer", VertexAttribPointer), P("VertexAttribIPointer", VertexAttribIPointer), P("VertexAttribDivisor", VertexAttribDivisor),
    P("VertexAttrib1f", VertexAttrib1f), P("VertexAttrib2f", VertexAttrib2f), P("VertexAttrib3f", VertexAttrib3f), P("VertexAttrib4f", VertexAttrib4f),
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
    P("MultiDrawElementsIndirect", MultiDrawElementsIndirect), P("MultiDrawArraysIndirect", MultiDrawArraysIndirect),
    P("GenQueries", GenQueries), P("DeleteQueries", DeleteQueries), P("BeginQuery", BeginQuery), P("EndQuery", EndQuery),
    P("GetQueryObjectui64v", GetQueryObjectui64v), P("GetQueryObjectuiv", GetQueryObjectuiv),
    P("FenceSync", FenceSync), P("DeleteSync", DeleteSync), P("ClientWaitSync", ClientWaitSync), P("WaitSync", WaitSync),
};
#undef P

// ------------------------------------------------------------------------------------------------ device lifetime
void CreateSwapchain(unsigned width, unsigned height)
{
    DkImageLayoutMaker maker;
    dkImageLayoutMakerDefaults(&maker, g_device);
    maker.flags = DkImageFlags_UsageRender | DkImageFlags_UsagePresent | DkImageFlags_Usage2DEngine | DkImageFlags_HwCompression;
    maker.format = DkImageFormat_RGBA8_Unorm;
    maker.dimensions[0] = width;
    maker.dimensions[1] = height;
    DkImageLayout layout;
    dkImageLayoutInitialize(&layout, &maker);
    const uint32_t stride = Align(uint32_t(dkImageLayoutGetSize(&layout)), dkImageLayoutGetAlignment(&layout));
    g_swapMemory = NewMemBlock(stride * kSwapImages, DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image);
    const DkImage* images[kSwapImages];
    for (unsigned i = 0; i < kSwapImages; ++i)
    {
        dkImageInitialize(&g_swapImages[i], &layout, g_swapMemory, stride * i);
        images[i] = &g_swapImages[i];
    }
    DkSwapchainMaker sm;
    dkSwapchainMakerDefaults(&sm, g_device, nwindowGetDefault(), images, kSwapImages);
    g_swapchain = dkSwapchainCreate(&sm);
    if (!g_swapchain) Fatal("swapchain creation failed");
    dkSwapchainSetSwapInterval(g_swapchain, uint32_t(g_swapInterval));
    g_outputWidth = width;
    g_outputHeight = height;
}

GLuint MakeWhite(bool array)
{
    GLuint name = g_textures.Gen();
    Texture* t = g_textures.Get(name);
    t->image = MakeImage(1, 1, 1, 1, 1, DkImageFormat_RGBA8_Unorm, array, false, true);
    t->params.minFilter = GL_LINEAR;
    const uint32_t white = 0xFFFFFFFFu;
    const int alignment = s.unpackAlignment;
    s.unpackAlignment = 1;
    UploadPixels(*t->image, 0, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, &white);
    s.unpackAlignment = alignment;
    return name;
}
}  // namespace

// ------------------------------------------------------------------------------------------------ public
bool Initialize(unsigned width, unsigned height)
{
    if (g_device) return true;
    DkDeviceMaker dm;
    dkDeviceMakerDefaults(&dm);
    dm.flags = DkDeviceFlags_DepthZeroToOne | DkDeviceFlags_OriginLowerLeft;
    g_device = dkDeviceCreate(&dm);
    if (!g_device) return false;

    uint32_t scratch = DK_PER_WARP_SCRATCH_MEM_ALIGNMENT;
    for (size_t i = 0; i < Shaders::programCount; ++i)
        for (unsigned k = 0; k < 3; ++k)
            if (Shaders::programs[i].dksh[k])
            {
                uint32_t program[6];
                std::memcpy(program, Shaders::programs[i].dksh[k] + reinterpret_cast<const uint32_t*>(Shaders::programs[i].dksh[k])[4], sizeof(program));
                scratch = std::max(scratch, program[5]);
            }
    DkQueueMaker qm;
    dkQueueMakerDefaults(&qm, g_device);
    qm.flags = DkQueueFlags_Graphics | DkQueueFlags_HighPrio | DkQueueFlags_EnableZcull;
    qm.commandMemorySize = 1u << 20;
    qm.flushThreshold = 256u << 10;
    qm.perWarpScratchMemorySize = Align(scratch, DK_PER_WARP_SCRATCH_MEM_ALIGNMENT);
    qm.maxConcurrentComputeJobs = 0;
    g_queue = dkQueueCreate(&qm);
    if (!g_queue) { dkDeviceDestroy(g_device); g_device = nullptr; return false; }

    g_bufferHeap = std::make_unique<Heap>(DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached, kBufferBlockSize);
    g_imageHeap = std::make_unique<Heap>(DkMemBlockFlags_GpuCached | DkMemBlockFlags_Image, kImageBlockSize);
    g_nullUniform = g_bufferHeap->Allocate(256, DK_UNIFORM_BUF_ALIGNMENT);
    std::memset(g_nullUniform.Cpu(), 0, 256);
    for (UniformUpload& u : g_uniformUploads) u = UniformUpload{};
    const uint32_t descriptorBytes = kImageDescriptors * sizeof(DkImageDescriptor) + kSamplerDescriptors * sizeof(DkSamplerDescriptor);
    g_descriptorMemory = NewMemBlock(descriptorBytes, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached);
    g_imageDescriptorsCpu = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(g_descriptorMemory));
    g_samplerDescriptorsCpu = g_imageDescriptorsCpu + kImageDescriptors * sizeof(DkImageDescriptor);
    g_imageDescriptorsGpu = dkMemBlockGetGpuAddr(g_descriptorMemory);
    g_samplerDescriptorsGpu = g_imageDescriptorsGpu + kImageDescriptors * sizeof(DkImageDescriptor);
    std::memset(g_imageDescriptorsCpu, 0, descriptorBytes);

    for (Frame& f : g_frames)
    {
        DkCmdBufMaker cm;
        dkCmdBufMakerDefaults(&cm, g_device);
        cm.userData = &f;
        cm.cbAddMem = GrowCommands;
        f.cmd = dkCmdBufCreate(&cm);
        if (!f.cmd) Fatal("command buffer creation failed");
        f.cmdMem.push_back(NewMemBlock(kCmdChunkSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached));
    }
    CreateSwapchain(width, height);

    s = State{};
    s.viewport[2] = s.scissor[2] = GLint(width);
    s.viewport[3] = s.scissor[3] = GLint(height);
    for (auto& g : s.generic) g = {0, 0, 0, 1};
    g_arrays.Clear();
    g_arrays.Default();   // (vertex array 0)
    InvalidateBound();
    g_whiteTexture = MakeWhite(false);
    g_whiteArray = MakeWhite(true);
    return true;
}

bool Initialized() { return g_device != nullptr; }

void ResizeOutput(unsigned width, unsigned height)
{
    if (!g_device || (width == g_outputWidth && height == g_outputHeight) || !width || !height) return;
    if (g_recording)
    {
        // (a frame with the old image acquired: shown first)
        if (g_swapSlot >= 0) Present();
        else Submit();
    }
    dkQueueWaitIdle(g_queue);
    dkSwapchainDestroy(g_swapchain);
    g_swapchain = nullptr;
    dkMemBlockDestroy(g_swapMemory);
    g_swapMemory = nullptr;
    CreateSwapchain(width, height);
    b.targetDirty = true;
}

void SetSwapInterval(int interval)
{
    g_swapInterval = std::clamp(interval, 0, 2);
    if (g_swapchain) dkSwapchainSetSwapInterval(g_swapchain, uint32_t(g_swapInterval));
}

// (CreateProgram's game: the runtime's, set at each Startup)
const char* g_shaderSet = "";
void SetShaderSet(const char* game) { g_shaderSet = game ? game : ""; }

// (SaltyNX's overlays: the render size of the frame presented next; 0: the output's)
unsigned g_frameResolutionWidth = 0, g_frameResolutionHeight = 0;
void SetFrameResolution(unsigned width, unsigned height)
{
    g_frameResolutionWidth = width && height ? width : 0;
    g_frameResolutionHeight = width && height ? height : 0;
}

void Present()
{
    if (!g_device) return;
    saltynx_set_resolution(g_frameResolutionWidth ? g_frameResolutionWidth : g_outputWidth,
                           g_frameResolutionHeight ? g_frameResolutionHeight : g_outputHeight);
    saltynx_frame(1);
    g_frameResolutionWidth = g_frameResolutionHeight = 0;
    if (!g_recording) BeginFrame();
    FlushCopies();
    AcquireSwapImage();
    Frame& f = Cur();
    DkCmdBuf cmd = Cmd();
    dkCmdBufSignalFence(cmd, &f.fence, true);
    dkQueueSubmitCommands(g_queue, dkCmdBufFinishList(cmd));
    dkQueuePresentImage(g_queue, g_swapchain, g_swapSlot);
    f.submitted = true;
    if (dkQueueIsInErrorState(g_queue)) Fatal("the GPU queue is in an error state (a command faulted)");
    g_swapSlot = -1;
    g_recording = false;
    g_commandsPending = false;
    g_drawsSinceSubmit = 0;
    g_frameIndex = (g_frameIndex + 1) % kFrameSlots;
}

GLuint CreateProgram(const char* vertexFile, const char* geometryFile, const char* fragmentFile, const char* prefix)
{
    if (!g_device) return 0;
    // the prefix's #define lines ("NAME VALUE"), canonical: "A=1;B=2"; an #extension line asks for a variant not built
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
            if (line.rfind("#extension", 0) == 0) return 0;
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
    const Shaders::Program* info = nullptr;
    for (size_t i = 0; i < Shaders::programCount; ++i)
    {
        const Shaders::Program& p = Shaders::programs[i];
        if (p.game[0] && std::strcmp(p.game, g_shaderSet)) continue;   // (another game's)
        if (std::strcmp(p.vert, vertexFile) || std::strcmp(p.frag, fragmentFile) || key != p.defines) continue;
        if ((geometryFile == nullptr) != (p.geom == nullptr) || (geometryFile && std::strcmp(geometryFile, p.geom))) continue;
        info = &p;
        break;
    }
    if (!info) return 0;   // (a variant not compiled offline)
    const GLuint name = g_programs.Gen();
    Program* p = g_programs.Get(name);
    p->info = info;
    // the stages' code in one block (each at a 256-byte boundary, the unusable tail after the last)
    uint32_t offsets[3]{}, total = 0;
    struct Binary { const uint8_t* control; const uint8_t* code; uint32_t size; } bins[3]{};
    for (unsigned k = 0; k < 3; ++k)
    {
        if (!info->dksh[k]) continue;
        uint32_t header[6];
        std::memcpy(header, info->dksh[k], sizeof(header));
        bins[k] = {info->dksh[k], info->dksh[k] + header[2], header[3]};
        offsets[k] = total;
        total = Align(total + header[3], DK_SHADER_CODE_ALIGNMENT);
        p->hasStage[k] = true;
    }
    total += DK_SHADER_CODE_UNUSABLE_SIZE;
    // (code memory for every program, allocated once: a few tens of KB; again after a Shutdown, which freed it)
    static DkMemBlock s_code = nullptr;
    static uint32_t s_codeCursor = 0, s_codeSize = 0;
    if (!s_code || s_code != g_codeMemory || s_codeCursor + total > s_codeSize)
    {
        s_codeSize = std::max<uint32_t>(512u << 10, Align(total, DK_MEMBLOCK_ALIGNMENT));
        s_code = NewMemBlock(s_codeSize, DkMemBlockFlags_CpuUncached | DkMemBlockFlags_GpuCached | DkMemBlockFlags_Code);
        s_codeCursor = 0;
        g_codeMemory = s_code;
    }
    auto* code = static_cast<uint8_t*>(dkMemBlockGetCpuAddr(s_code));
    for (unsigned k = 0; k < 3; ++k)
    {
        if (!p->hasStage[k]) continue;
        std::memcpy(code + s_codeCursor + offsets[k], bins[k].code, bins[k].size);
        DkShaderMaker maker;
        dkShaderMakerDefaults(&maker, s_code, s_codeCursor + offsets[k]);
        maker.control = bins[k].control;
        dkShaderInitialize(&p->shaders[k], &maker);
        if (!dkShaderIsValid(&p->shaders[k])) Fatal("shader %s stage %u is invalid", info->key, k);
    }
    s_codeCursor = Align(s_codeCursor + total, DK_SHADER_CODE_ALIGNMENT);
    p->constants.assign(info->constantSize, 0);
    if (info->constantSize)
    {
        p->uboSize = Align(info->constantSize, DK_UNIFORM_BUF_ALIGNMENT);
        p->ubo = g_bufferHeap->Allocate(p->uboSize, DK_UNIFORM_BUF_ALIGNMENT);
        std::memset(p->ubo.Cpu(), 0, p->uboSize);
    }
    for (uint32_t i = 0; i < info->uniformCount; ++i)
    {
        const Shaders::Uniform& u = info->uniforms[i];
        if (u.type < 32 || u.binding >= kMaxBindings) continue;
        p->samplerArray[u.binding] = u.type == 33;
        p->samplerStages[u.binding] = u.stages;
        p->samplerUnit[u.binding] = 0;
        p->samplerCount = std::max<unsigned>(p->samplerCount, u.binding + 1u);
    }
    return name;
}

void ResetState()
{
    State fresh;
    fresh.viewport[2] = fresh.scissor[2] = GLint(g_outputWidth);
    fresh.viewport[3] = fresh.scissor[3] = GLint(g_outputHeight);
    for (auto& g : fresh.generic) g = {0, 0, 0, 1};
    s = fresh;
    if (g_recording) InvalidateBound();
    b.targetDirty = true;
}

void* ProcAddress(const char* name)
{
    if (!name) return nullptr;
    for (const ProcEntry& e : kProcs)
        if (!std::strcmp(e.name, name)) return e.fn;
    return nullptr;
}

void Shutdown()
{
    if (!g_device) return;
    if (g_recording) { Submit(); g_recording = false; }
    dkQueueWaitIdle(g_queue);
    g_pendingCopies.clear();
    for (Frame& f : g_frames)
    {
        f.submitted = false;
        RetireFrame(f);
        for (RingChunk& c : f.ring) dkMemBlockDestroy(c.mem);
        f.ring.clear();
        if (f.cmd) dkCmdBufDestroy(f.cmd);
        f.cmd = nullptr;
        for (DkMemBlock m : f.cmdMem) dkMemBlockDestroy(m);
        f.cmdMem.clear();
    }
    g_queries.ForEach([](GLuint, Query& q) { if (q.memory) dkMemBlockDestroy(q.memory); });
    g_buffers.Clear(); g_arrays.Clear(); g_textures.Clear(); g_samplers.Clear(); g_renderbuffers.Clear();
    g_framebuffers.Clear(); g_programs.Clear(); g_queries.Clear(); g_syncs.clear();
    g_samplerSlots.clear(); g_freeImageSlots.clear(); g_nextImageSlot = 0;
    if (g_readback) dkMemBlockDestroy(g_readback);
    g_readback = nullptr; g_readbackSize = 0;
    if (g_swapchain) dkSwapchainDestroy(g_swapchain);
    g_swapchain = nullptr;
    if (g_swapMemory) dkMemBlockDestroy(g_swapMemory);
    g_swapMemory = nullptr;
    g_bufferHeap->Destroy(); g_imageHeap->Destroy();
    g_nullUniform = {};
    g_bufferHeap.reset(); g_imageHeap.reset();
    if (g_descriptorMemory) dkMemBlockDestroy(g_descriptorMemory);
    g_descriptorMemory = nullptr;
    if (g_codeMemory) dkMemBlockDestroy(g_codeMemory);
    g_codeMemory = nullptr;
    dkQueueDestroy(g_queue);
    g_queue = nullptr;
    dkDeviceDestroy(g_device);
    g_device = nullptr;
    g_swapSlot = -1;
}
}  // namespace CaneDeko
