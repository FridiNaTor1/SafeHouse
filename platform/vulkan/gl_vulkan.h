#pragma once
// Sly 2's OpenGL on Vulkan (Linux, Windows). The game renders through the OpenGL 4.3 subset it uses (render.cpp,
// hud.cpp, emitter.cpp, overlay.cpp, moviegl.cpp, rtgt.cpp, texmod.cpp, gl.cpp); with the Vulkan renderer chosen, the GL
// entry points glad loads are this layer's (ProcAddress), which records Vulkan command buffers directly: the same
// split as the Switch's deko3d layer (platform/switch/deko/gl_deko.cpp), so both renderers share every line of the
// game's scene and draw-list work and differ only in submission, and the OpenGL renderer stays exactly as it was.
//
// Coordinates: the framebuffer objects' memory is GL's (clip y -1 on row 0, GL's viewport and scissor rectangles, GL's
// gl_FragCoord), so render-to-texture, blits and glReadPixels carry over unchanged. Vulkan decides the front face with
// the opposite sign from GL for the same window coordinates (GL CCW is Vulkan CLOCKWISE). The window (framebuffer 0)
// is an offscreen image in the same convention, copied flipped into the swapchain at the present.
//
// Shaders are compiled to SPIR-V at build time (cmake/vulkan_shaders.py -> vk_shaders.cpp); GLSHADER (Sly2-Release
// gl.cpp) asks for a program with CreateProgram. notes/VULKAN_2026-11-10.md.
#include <glad/glad.h>
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include "draw_packet.h"

#include <cstdint>
#include <functional>
#include <string>

struct GLFWwindow;

namespace CaneVk
{
// (VR, Sly2-Release vr.cpp) the instance and device made through the OpenXR runtime (XR_KHR_vulkan_enable2): the
// runtime adds what it needs to each create info and picks the physical device
struct ExternalDevice
{
    void* user = nullptr;
    VkResult (*createInstance)(void* user, PFN_vkGetInstanceProcAddr getProc, const VkInstanceCreateInfo* ci, VkInstance* out) = nullptr;
    VkPhysicalDevice (*physicalDevice)(void* user, VkInstance instance) = nullptr;
    VkResult (*createDevice)(void* user, PFN_vkGetInstanceProcAddr getProc, VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, VkDevice* out) = nullptr;
};
struct Options
{
    bool visible = true;              // false: no surface or swapchain (headless screenshots, tests)
    int swapInterval = 1;
    std::string pipelineCachePath;    // the driver's pipeline cache between runs ("" none)
    // Called once with the reason when Vulkan stops with an error it cannot recover from (device lost, a failed pipeline
    // or allocation), just before the process ends (the game remembers it and may restart itself on OpenGL).
    void (*onFatal)(const char* why) = nullptr;
    const ExternalDevice* external = nullptr;   // (VR) instance / device through OpenXR
    bool singleThread = false;        // no submission worker (SLY2N_VKTHREAD=0 / SLY2N_VRVKTHREAD=0)
    void (*onWorkerThread)() = nullptr;   // called on the submission worker as it starts (VR: the thread told to the runtime)
};
// The instance, device and (visible) swapchain for the window (created with GLFW_CLIENT_API = GLFW_NO_API). False
// (and why) when Vulkan cannot run the game here: the caller falls back to OpenGL.
bool Initialize(GLFWwindow* window, const Options& options, std::string* error);
bool Initialized();
// The draw packets (draw_packet.h): states decided once, drawn without the GL-style calls.
const cane::draw::Packets* DrawPackets();
// Pipeline prewarm (notes/VKGPU_2026-11-11.md): the game enters a scope (Sly 2: a world's load). The pipelines noted in
// that scope before (this run or earlier ones, <pipeline cache>.keys) are made on a worker thread; this waits up to
// waitMs for them (the rest are made while the game runs; one needed first is made on the spot). Pipelines made from
// now on are noted for this scope. SLY2N_VKPREWARM=0: off.
void PrewarmPipelines(int32_t scope, unsigned waitMs);
void Shutdown();
// glad's loader: the implemented GL functions by name (nullptr for the others).
void* ProcAddress(const char* name);
// The frame: submits the commands and presents the window's picture (glfwSwapBuffers).
void Present();
// (Android) the window's surface comes and goes (platform/android/android_app.h windowSurfaceSerial): `serial` returns
// a number that changes whenever the platform replaced the surface and 0 while there is none (the app in the
// background). Each Present asks it: the layer then renders and submits without presenting, and makes its surface and
// swapchain again for a new number. Not set (desktop): the surface lives as long as the window.
void SetSurfaceSerial(uint64_t (*serial)());
// 0: no vsync, 1: every vblank (2: as 1).
void SetSwapInterval(int interval);
// A program compiled at build time: the GLSL files and the prefix gl.cpp inserts after #version; 0 when that variant
// does not exist here (the caller falls back as after a link failure).
GLuint CreateProgram(const char* vertexFile, const char* geometryFile, const char* fragmentFile, const char* prefix = "");
// (VR) the objects the OpenXR session binds (XrGraphicsBindingVulkan2KHR)
struct Handles { VkInstance instance; VkPhysicalDevice physicalDevice; VkDevice device; uint32_t queueFamily; uint32_t queueIndex; };
Handles DeviceHandles();
// (VR) an image of the OpenXR swapchain as a GL texture of this layer (an attachment / blit target; not owned): its
// view format (an sRGB swapchain image made with the mutable-format usage is written through a UNORM view: the game's
// colours are display-encoded already), size and layers; the layer it is in when the runtime hands it over.
GLuint WrapImage(VkImage image, VkFormat viewFormat, uint32_t width, uint32_t height, uint32_t layers, uint32_t samples = 1);
// (VR) A depth-stencil renderbuffer whose contents are never read after its rendering pass (the eyes': cleared at the
// start, dropped at the end): its passes do not store it (a tiler's write-out saved).
void SetRenderbufferTransient(GLuint renderbuffer, bool transient);
// (Android, VK_EXT_multisampled_render_to_single_sampled) GL-style tile MSAA: the next image made (TexStorage2D,
// RenderbufferStorage) is single-sampled but drawn with this many samples, resolved on the tile as it is stored; a
// multisampled renderbuffer is made that way by itself. False: no such device.
bool RendersToSingleSampled();
void SetNextImageRenderSamples(uint32_t samples);
// (VR) A wrapped OpenXR image's Vulkan attributes as the runtime made them (XR_META_vulkan_swapchain_create_info): drawn
// with renderSamples samples into its one (it has the multisampled-render-to-single-sampled flag), readable in place
// (the input-attachment use: the shadow phase's read in a render pass's subpass), its usage flags.
void SetImageAttributes(GLuint texture, uint32_t renderSamples, bool inputAttachment, VkImageUsageFlags usage);
// Whether a frame can be read in place on this device at all (local read, or the classic render pass's subpass).
bool CanReadInPlace();
// (VR's fixed foveation, VK_EXT_fragment_density_map) a density map for a target of width x height (0: none on this
// device), its densities for a level (0 off, 1 low, 2 medium, 3 high) around the lens's centre (cx, cy in -1..1 of the
// drawn usedW x usedH rectangle), and a texture drawn with it (0: none).
bool DensityMapsSupported();
GLuint CreateDensityMap(uint32_t width, uint32_t height, uint32_t layers = 1);
void FillDensityMap(GLuint map, uint32_t usedW, uint32_t usedH, float cx, float cy, int level, uint32_t layer = 0);
void SetDensityMap(GLuint texture, GLuint map);
// (VR multiview, VK_KHR_multiview) both eyes in one render pass: a framebuffer attachment made of `views` (2) layers from
// baseLayer, drawn as the pass's views (GL_OVR_multiview's glFramebufferTextureMultiviewOVR; a renderbuffer too). Every
// program then runs its two-view stages (vk_shaders.h spirvMv: gl_ViewIndex picks the eye's values from the storage
// buffer at binding 11, CaneViews); a target whose attachments differ in views is drawn as one view. A density map
// with a layer a view (CreateDensityMap's layers) gives each view its own. MultiviewSupported: the device can (the
// multiview feature, the geometry stage's where it has one and the caller's passes use it, push constants for the
// programs' blocks).
bool MultiviewSupported(bool geometryStage = true);
void FramebufferMultiview(GLenum target, GLenum attachment, GLuint object, bool renderbuffer, GLint baseLayer, GLsizei views);
// the next renderbuffer storage made has this many layers (a two-view depth-stencil)
void SetNextRenderbufferLayers(uint32_t layers);
// (VR) after the runtime handed it over (xrWaitSwapchainImage): the image's layout then
void AcquireImage(GLuint texture, VkImageLayout layout);
// (VR) before xrReleaseSwapchainImage: the image back in that layout and the commands so far submitted (submit false:
// only the layout; the commands go with the next submission, which then has to come before the release)
void ReleaseImage(GLuint texture, VkImageLayout layout, bool submit = true);
// (VR) the frame's end without a window present (the frame slots advance as at Present)
void EndFrame();
// (VR) The OpenXR runtime uses the queue inside some of its calls (xrBeginFrame, xrEndFrame, xrAcquire- / xrRelease-
// SwapchainImage), so the application must keep them apart from its own submissions. With the submission worker:
// AfterNextSubmit(fn) runs fn on the worker right after the next submission (in order; without the worker right after
// it on this thread): a swapchain image's release and xrEndFrame then follow the commands that drew them while the
// game thread goes on with the next step. WaitSubmissions(): every submission handed to the worker and its fns done,
// before this thread makes such a call itself. FSubmitsOnWorker(): whether there is a worker.
void AfterNextSubmit(std::function<void()> fn);
void WaitSubmissions();
bool FSubmitsOnWorker();
// (SLY2N_FPSLOG) the submission worker's thread CPU time so far (ms): all of it (with VR's xrEndFrame), and its command
// recording and submission; 0 without a worker
void WorkerCpuMs(double* pmsAll, double* pmsRecord);
// A tile-based GPU (Adreno, Mali, PowerVR, Apple, VideoCore: by the device's vendor / driver, every Android device; SLY2N_VKTILER
// overrides it): the layer's tilers' paths are on (in-place reads, D24S8, MSAA resolved on the tile); the game picks its own
// (vertex pulling, VR's passes without the near-clip stage) by it.
bool TileGpu();
// (VR's log) SetFrameTiming(true): from then on a GPU timestamp at each frame's start and end. FrameTimes: the totals so far:
// the GPU's ms from frames' first command to their last and the frames counted, and the ms this thread waited for the GPU
// (a frame slot, a full ring).
void SetFrameTiming(bool on);
void FrameTimes(double* pmsGpu, uint32_t* pframesGpu, double* pmsWait);
// The device, for the log and the debug menu ("AMD Radeon RX 9070 XT (RADV GFX1201)").
const char* DeviceName();
// Whether a CANE_FETCH program can read the bound draw framebuffer's colour in place (VK_KHR_dynamic_rendering_local_read,
// a single-sampled colour image the layer made: notes/VKBATCH_2026-11-10.md); else the caller copies it.
bool FramebufferFetchUsable();
// (tests) render-pass instances begun and transfer batches recorded inside the frame since the last call
struct Stats { unsigned renderPasses = 0, splits = 0, pipelines = 0, pipelinesCreated = 0, draws = 0, submits = 0, pushes = 0, pushBytes = 0, descriptorPushes = 0; };
Stats TakeStats();
// (measurement: SLY2N_VKGPUTIME=N with SLY2N_VKGPUMARKS=1) a GPU timestamp in the frame's command stream; every N frames
// the average GPU ms between each mark and the next, by the earlier mark's label (a string that outlives the run), stderr.
bool GpuMarksOn();
// GL_EXT_depth_bounds_test (glEnable(GL_DEPTH_BOUNDS_TEST_EXT), glDepthBoundsEXT) on a device with depthBounds
bool DepthBoundsSupported();
void GpuMark(const char* label);
}  // namespace CaneVk
