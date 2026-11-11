// VR through OpenXR (vr.h). notes/VR_2026-11-10.md.
//
// The loader is opened at run time; every OpenXR function comes from xrGetInstanceProcAddr. Graphics: desktop OpenGL
// (XR_KHR_opengl_enable: GLX on Linux, WGL on Windows; Monado's XR_MNDX_egl_enable for an EGL context on Linux) and
// OpenGL ES on Android (XR_KHR_opengl_es_enable). The eyes render straight into the runtime's swapchain images (on
// GLES with EXT_multisampled_render_to_texture: MSAA resolved on the tile, Meta's fixed foveation applies), the game
// picture (HUD, menus, the Flat Screen) goes to a quad layer.

#include "vr.h"

#if defined(__SWITCH__)
// (no OpenXR, no dlopen on the Switch: VR stays off, the hooks do nothing)
namespace vr
{
VROPTS g_vropts;
void Request(bool) {}
bool FRequested() { return false; }
bool FRuntimeAvailable() { return false; }
bool FStart() { return false; }
bool FActive() { return false; }
bool FMenuShown() { return false; }
void Shutdown() {}
bool FExitRequested() { return false; }
void BeginFrame() {}
bool FImmersiveFrame() { return false; }
bool FEyeViews(const CAMERA&, STEREOVIEW*) { return false; }
void FinishEyes() {}
bool FPresent() { return false; }
bool FSkipSwap() { return false; }
bool FPadState(GLFWgamepadstate*) { return false; }
const std::string& StrInteractionProfile() { static const std::string s_str; return s_str; }
void Rumble(float, float) {}
void ApplyRefreshRate() {}
void Recenter() {}
bool FAutoCameraRotation(bool fFlatOption) { return fFlatOption; }
void SetGameView(int) {}
bool FHeadLook(float* a, float* b) { *a = *b = 0.0f; return false; }
bool FRcTableActive() { return false; }
bool FCompositorUpscaling() { return false; }
bool FRcAimPoint(glm::vec3*) { return false; }
void SetRcArenaHeight(float) {}
bool FPanelSize(int*, int*) { return false; }
void NoteThread(VRTHREAD) {}
}  // namespace vr
#else

#include "controller_family.h"
#include "display.h"
#include "gl.h"
#include "render.h"

#include <GLFW/glfw3.h>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <stb/stb_image_write.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <mutex>
#include <vector>
#if defined(__ANDROID__)
#include <unistd.h>   // (gettid: XR_KHR_android_thread_settings)
#endif

#if defined(__ANDROID__)
#include <EGL/egl.h>
#include <jni.h>
#include <dlfcn.h>
#include <android/log.h>
#include <SDL_system.h>
#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_OPENGL
#else
#include <dlfcn.h>
// (the X11 / GLX / EGL types the bindings name, without the system headers: they would clash with glad)
typedef struct _XDisplay Display;
typedef unsigned long XID;
typedef XID GLXDrawable;
typedef struct __GLXFBConfigRec* GLXFBConfig;
typedef struct __GLXcontextRec* GLXContext;
typedef void* EGLDisplay;
typedef void* EGLConfig;
typedef void* EGLContext;
typedef void (*__eglMustCastToProperFunctionPointerType)(void);
#define XR_USE_PLATFORM_XLIB
#define XR_USE_PLATFORM_EGL
#define XR_USE_GRAPHICS_API_OPENGL
#endif
#if CANE_VULKAN
// (VR on the Vulkan renderer: XR_KHR_vulkan_enable2, the renderer's swapchain images, platform/vulkan/gl_vulkan.h)
#include "vulkan/gl_vulkan.h"
#include "renderer.h"
#define XR_USE_GRAPHICS_API_VULKAN
#endif
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

namespace vr
{
VROPTS g_vropts;
constexpr float kPi = 3.14159265358979f;

namespace
{
void Log(const char* fmt, ...)
{
    char ach[1024];
    va_list va;
    va_start(va, fmt);
    std::vsnprintf(ach, sizeof ach, fmt, va);
    va_end(va);
    std::printf("vr: %s\n", ach);
    std::fflush(stdout);
#if defined(__ANDROID__)
    __android_log_print(ANDROID_LOG_INFO, "SafeHouseVR", "%s", ach);
#endif
}

#if defined(__ANDROID__)
constexpr bool kGles = true;
#else
constexpr bool kGles = false;
#endif

// ------------------------------------------------------------ the loader and the functions
void* s_hLoader = nullptr;
PFN_xrGetInstanceProcAddr s_pfnGetProc = nullptr;
#define XRFN(name) PFN_##name p##name = nullptr;
#define XRFN_LIST                                                                                                   \
    XRFN(xrEnumerateInstanceExtensionProperties) XRFN(xrCreateInstance) XRFN(xrDestroyInstance) XRFN(xrGetSystem)   \
    XRFN(xrGetInstanceProperties) XRFN(xrGetSystemProperties) XRFN(xrEnumerateViewConfigurationViews)               \
    XRFN(xrCreateSession) XRFN(xrDestroySession) XRFN(xrCreateReferenceSpace) XRFN(xrDestroySpace)                  \
    XRFN(xrEnumerateSwapchainFormats) XRFN(xrCreateSwapchain) XRFN(xrDestroySwapchain)                              \
    XRFN(xrEnumerateSwapchainImages) XRFN(xrAcquireSwapchainImage) XRFN(xrWaitSwapchainImage)                       \
    XRFN(xrReleaseSwapchainImage) XRFN(xrPollEvent) XRFN(xrBeginSession) XRFN(xrEndSession)                         \
    XRFN(xrRequestExitSession) XRFN(xrWaitFrame) XRFN(xrBeginFrame) XRFN(xrEndFrame) XRFN(xrLocateViews)            \
    XRFN(xrLocateSpace) XRFN(xrStringToPath) XRFN(xrPathToString) XRFN(xrCreateActionSet) XRFN(xrCreateAction)      \
    XRFN(xrSuggestInteractionProfileBindings) XRFN(xrAttachSessionActionSets) XRFN(xrSyncActions)                   \
    XRFN(xrGetActionStateBoolean) XRFN(xrGetActionStateFloat) XRFN(xrGetActionStateVector2f)                        \
    XRFN(xrApplyHapticFeedback) XRFN(xrGetCurrentInteractionProfile) XRFN(xrResultToString)                         \
    XRFN(xrCreateActionSpace) XRFN(xrEnumerateViewConfigurations)
XRFN_LIST
#undef XRFN
// extensions' functions (null without them)
PFN_xrEnumerateDisplayRefreshRatesFB pxrEnumerateDisplayRefreshRatesFB = nullptr;
PFN_xrRequestDisplayRefreshRateFB pxrRequestDisplayRefreshRateFB = nullptr;
PFN_xrGetDisplayRefreshRateFB pxrGetDisplayRefreshRateFB = nullptr;
PFN_xrCreateFoveationProfileFB pxrCreateFoveationProfileFB = nullptr;
PFN_xrDestroyFoveationProfileFB pxrDestroyFoveationProfileFB = nullptr;
PFN_xrUpdateSwapchainFB pxrUpdateSwapchainFB = nullptr;
PFN_xrPerfSettingsSetPerformanceLevelEXT pxrPerfSettingsSetPerformanceLevelEXT = nullptr;
#if defined(__ANDROID__)
PFN_xrSetAndroidApplicationThreadKHR pxrSetAndroidApplicationThreadKHR = nullptr;
#endif
#if defined(__ANDROID__)
PFN_xrGetOpenGLESGraphicsRequirementsKHR pxrGetOpenGLESGraphicsRequirementsKHR = nullptr;
#else
PFN_xrGetOpenGLGraphicsRequirementsKHR pxrGetOpenGLGraphicsRequirementsKHR = nullptr;
#endif
#if CANE_VULKAN
PFN_xrGetVulkanGraphicsRequirements2KHR pxrGetVulkanGraphicsRequirements2KHR = nullptr;
PFN_xrCreateVulkanInstanceKHR pxrCreateVulkanInstanceKHR = nullptr;
PFN_xrGetVulkanGraphicsDevice2KHR pxrGetVulkanGraphicsDevice2KHR = nullptr;
PFN_xrCreateVulkanDeviceKHR pxrCreateVulkanDeviceKHR = nullptr;
#endif

bool FOpenLoader()
{
#if defined(_WIN32)
    HMODULE h = LoadLibraryA("openxr_loader.dll");
    if (!h) return false;
    s_hLoader = h;
    s_pfnGetProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(reinterpret_cast<void*>(GetProcAddress(h, "xrGetInstanceProcAddr")));
#else
    // (SLY2N_OPENXR_LOADER=path: a loader of one's own, e.g. a test build's)
    const char* aszName[] = {std::getenv("SLY2N_OPENXR_LOADER"), "libopenxr_loader.so.1", "libopenxr_loader.so"};
    for (const char* sz : aszName)
        if (sz && *sz && (s_hLoader = dlopen(sz, RTLD_NOW | RTLD_LOCAL))) break;
    if (!s_hLoader) return false;
    s_pfnGetProc = reinterpret_cast<PFN_xrGetInstanceProcAddr>(dlsym(s_hLoader, "xrGetInstanceProcAddr"));
#endif
    return s_pfnGetProc != nullptr;
}

template <typename T> void GetProc(XrInstance inst, const char* name, T& pfn)
{
    PFN_xrVoidFunction f = nullptr;
    if (XR_SUCCEEDED(s_pfnGetProc(inst, name, &f))) pfn = reinterpret_cast<T>(f);
    else pfn = nullptr;
}

void LoadInstanceFunctions(XrInstance inst)
{
#define XRFN(name) GetProc(inst, #name, p##name);
    XRFN_LIST
#undef XRFN
    GetProc(inst, "xrEnumerateDisplayRefreshRatesFB", pxrEnumerateDisplayRefreshRatesFB);
    GetProc(inst, "xrRequestDisplayRefreshRateFB", pxrRequestDisplayRefreshRateFB);
    GetProc(inst, "xrGetDisplayRefreshRateFB", pxrGetDisplayRefreshRateFB);
    GetProc(inst, "xrCreateFoveationProfileFB", pxrCreateFoveationProfileFB);
    GetProc(inst, "xrDestroyFoveationProfileFB", pxrDestroyFoveationProfileFB);
    GetProc(inst, "xrUpdateSwapchainFB", pxrUpdateSwapchainFB);
    GetProc(inst, "xrPerfSettingsSetPerformanceLevelEXT", pxrPerfSettingsSetPerformanceLevelEXT);
#if defined(__ANDROID__)
    GetProc(inst, "xrSetAndroidApplicationThreadKHR", pxrSetAndroidApplicationThreadKHR);
#endif
#if defined(__ANDROID__)
    GetProc(inst, "xrGetOpenGLESGraphicsRequirementsKHR", pxrGetOpenGLESGraphicsRequirementsKHR);
#else
    GetProc(inst, "xrGetOpenGLGraphicsRequirementsKHR", pxrGetOpenGLGraphicsRequirementsKHR);
#endif
#if CANE_VULKAN
    GetProc(inst, "xrGetVulkanGraphicsRequirements2KHR", pxrGetVulkanGraphicsRequirements2KHR);
    GetProc(inst, "xrCreateVulkanInstanceKHR", pxrCreateVulkanInstanceKHR);
    GetProc(inst, "xrGetVulkanGraphicsDevice2KHR", pxrGetVulkanGraphicsDevice2KHR);
    GetProc(inst, "xrCreateVulkanDeviceKHR", pxrCreateVulkanDeviceKHR);
#endif
}

// ------------------------------------------------------------ the state
bool s_fRequested = false;
bool s_fVk = false;               // the session is on the Vulkan renderer (XR_KHR_vulkan_enable2)
bool FGles() { return kGles && !s_fVk; }   // (the OpenGL ES paths: not with the Vulkan renderer on Android)
bool s_fStarted = false;          // FStart succeeded (instance, system, session)
bool s_fExit = false;
XrInstance s_inst = XR_NULL_HANDLE;
XrSystemId s_sys = XR_NULL_SYSTEM_ID;
XrSession s_session = XR_NULL_HANDLE;
XrSessionState s_state = XR_SESSION_STATE_UNKNOWN;
bool s_fRunning = false;          // between xrBeginSession and xrEndSession
XrSpace s_spaceLocal = XR_NULL_HANDLE, s_spaceView = XR_NULL_HANDLE;
std::vector<std::string> s_aext;  // the runtime's extensions
bool s_fExtRefresh = false, s_fExtFoveation = false, s_fExtPerf = false, s_fExtEglMndx = false, s_fExtThreads = false;
// (Vulkan: the eyes drawn straight into the OpenXR images, with their clip space's Y flipped (the images are read top
// row first); the images made with the tile MSAA flag or the input-attachment use (XR_META_vulkan_swapchain_create_info).
// Without it: a target of the eye's own, copied in flipped.)
bool s_fExtVkSwapchainMeta = false, s_fVkDirect = false;
// (Meta's compositor filters for the projection layer, XR_FB_composition_layer_settings: VR Settings Upscaling; the
// automatic choice XR_META_automatic_layer_filter where offered (SLY2N_VR_LAYERFLAGS tests))
bool s_fExtLayerSettings = false, s_fExtAutoLayerFilter = false;
GLuint s_amapFdm[2] = {};   // (Vulkan) the eyes' density maps (fixed foveation)
int s_levelFoveationApplied = -1, s_dxFdm = 0, s_dyFdm = 0;
void ApplyFoveation();
bool s_fGlesMsrtt = false;        // EXT_multisampled_render_to_texture
bool s_fSrgbWriteControl = false; // EXT_sRGB_write_control (GLES)
bool s_fMultiviewExt = false;     // GL_OVR_multiview2 + GL_OVR_multiview_multisampled_render_to_texture (GLES)
bool s_fMultiview = false;        // the eyes are one two-layer swapchain drawn in one pass (render.cpp RenderSceneStereo)
GLuint s_texDepthMv = 0;          // its depth-stencil (a two-layer array)
// (Vulkan, VK_KHR_multiview: both eyes in one render pass, the two views of a two-layer target (the eye's own, copied
// into the two-layer swapchain in one two-view draw; or the swapchain itself, straight in), every draw recorded once;
// the programs' two-view stages read each eye's values (cmake/vulkan_shaders.py); VR Settings Multiview, the device's
// support (CaneVk::MultiviewSupported) else the eyes one by one; SLY2N_VR_MULTIVIEW=0 / 1 overrides)
GLuint s_mapFdmMv = 0;            // (its density map: a layer an eye)
GLuint s_afboLayerVk[2][2] = {};  // (orientation check / shots: read framebuffers on a layer of the eyes' target / image)
GLSHADER s_shCopyArray;           // (vrcopy from a two-layer target: in a two-view draw each view its layer)
int s_msaa = 1;
std::string s_strProfile;
std::string s_strRuntime;

bool FHasExt(const char* sz) { return std::find(s_aext.begin(), s_aext.end(), sz) != s_aext.end(); }
bool FCheck(XrResult r, const char* what)
{
    if (XR_SUCCEEDED(r)) return true;
    char ach[XR_MAX_RESULT_STRING_SIZE] = {};
    if (pxrResultToString && s_inst) pxrResultToString(s_inst, r, ach);
    Log("%s failed: %s (%d)", what, ach[0] ? ach : "?", int(r));
    return false;
}

// a swapchain and its images' GL textures (and framebuffers on them)
struct SWAPCHAIN
{
    XrSwapchain h = XR_NULL_HANDLE;
    int dx = 0, dy = 0;
    int64_t format = 0;
    std::vector<GLuint> atex;
    std::vector<GLuint> afbo;     // one framebuffer per image (its colour; the eyes' also the depth)
    uint32_t iimage = 0;
    bool fAcquired = false;
};
SWAPCHAIN s_aeye[2], s_panel;
std::vector<XrViewConfigurationView> s_aviewcfg;
GLuint s_arboDepth[2] = {}, s_afboMsaa[2] = {}, s_arboColorMsaa[2] = {};   // the eyes' depth; desktop MSAA targets
GLuint s_atexEyeVk[2] = {};   // (Vulkan: the eyes' colour targets, drawn flipped into the images; multiview: [0] a two-layer array)
int64_t s_formatColor = 0;
bool s_fSrgb = false;

// the frame
struct FRAME
{
    bool fBegun = false;
    XrFrameState state{XR_TYPE_FRAME_STATE};
    XrView aview[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    bool fViews = false;
    bool fEyesDrawn = false;
    bool fImmersive = false;
} s_frame;
float s_hzDisplay = 0.0f;
// Eye resolution (VR Settings): the eyes drawn into the bottom-left part of their targets (the layer's imageRect tells
// the compositor), at s_gEyeRes of the full size: fixed, or dynamic between 100 % and 75 % (frames missed at the
// display's rate step it down, a calm spell steps it back up). Not with the two-view pass.
float s_gEyeRes = 1.0f;
int s_dxEye = 0, s_dyEye = 0;   // this frame's eye size (psv->dx / dy)
bool s_fEyesLastFrame = false;
double s_tFramePrev = -1.0, s_tWindow = 0.0, s_tCalm = 0.0, s_tUp = -100.0, s_sHoldUp = 3.0;
int s_cfWindow = 0, s_cMissWindow = 0;
constexpr float kEyeResFloor = 0.75f, kEyeResStep = 0.05f;
// The compositor's filters for the eyes (VR Settings Upscaling, XR_FB_composition_layer_settings): the projection
// layer's flags, 0 none. Super Resolution: Meta's quality super sampling + quality sharpening (the "Quest Super
// Resolution" filter of recent runtimes); Sharpen: quality sharpening alone. SLY2N_VR_LAYERFLAGS=hex: those flags as
// given (device tests, e.g. 0x20 the automatic filter of XR_META_automatic_layer_filter).
uint64_t LayerFilterFlags()
{
    if (!s_fExtLayerSettings) return 0;
    if (const char* sz = std::getenv("SLY2N_VR_LAYERFLAGS")) return std::strtoull(sz, nullptr, 16);
    switch (g_vropts.upscaling)
    {
    case VRUPSCALE_Sharpen: return XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB;
    case VRUPSCALE_SuperResolution: return XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB | XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SHARPENING_BIT_FB;
    default: return 0;
    }
}
// Dynamic eye resolution's ceiling: 100 %, or 90 % while the compositor upscales the eyes (the filters above applied:
// the runtime has the extension and Upscaling is on), so the headroom goes into the frame rate and the filter makes up
// the detail; the floor stays 75 % either way (never a large upscale)
float EyeResCeiling() { return LayerFilterFlags() ? kEyeResCeilingUpscaled : 1.0f; }
void UpdateEyeResolution(bool fEyesLastFrame)
{
    const bool fFullOnly = s_fMultiview && !s_fVk;   // (GLES's two-view pass: the whole image)
    if (g_vropts.eyeResolution > 0 || fFullOnly)
    {
        s_gEyeRes = fFullOnly ? 1.0f : float(std::clamp(g_vropts.eyeResolution, 50, 100)) / 100.0f;
        s_tFramePrev = -1.0;
        return;
    }
    // (dynamic: within [75 %, the ceiling]; the ceiling follows Upscaling when it is changed)
    const float gCeiling = EyeResCeiling();
    if (s_gEyeRes > gCeiling) s_gEyeRes = gCeiling;
    const double t = glfwGetTime();
    // (the display's refresh, not predictedDisplayPeriod: Meta's runtime reports the app's own pace there once it is late)
    const double period = s_hzDisplay > 1.0f ? 1.0 / double(s_hzDisplay)
                        : s_frame.state.predictedDisplayPeriod > 0 ? double(s_frame.state.predictedDisplayPeriod) * 1e-9 : 1.0 / 72.0;
    if (!fEyesLastFrame || s_tFramePrev < 0.0) { s_tFramePrev = t; s_tWindow = t; s_cfWindow = s_cMissWindow = 0; return; }
    const double dt = t - s_tFramePrev;
    s_tFramePrev = t;
    ++s_cfWindow;
    if (dt > period * 1.4) ++s_cMissWindow;   // (a display refresh passed without a new frame)
    if (t - s_tWindow < 0.5) return;
    const float gWas = s_gEyeRes;
    if (s_cMissWindow * 10 > s_cfWindow)
    {
        // (more than a tenth of the frames late: down a step; a step up that brought this on waits longer next time)
        s_sHoldUp = t - s_tUp < 2.0 ? std::min(s_sHoldUp * 2.0, 30.0) : 3.0;
        s_gEyeRes = std::max(kEyeResFloor, s_gEyeRes - kEyeResStep);
        s_tCalm = 0.0;
    }
    else if (s_cMissWindow == 0)
    {
        s_tCalm += t - s_tWindow;
        if (s_tCalm >= s_sHoldUp && s_gEyeRes < gCeiling)
        {
            s_gEyeRes = std::min(gCeiling, s_gEyeRes + kEyeResStep);
            s_tUp = t;
            s_tCalm = 0.0;
        }
    }
    else s_tCalm = 0.0;
    if (s_gEyeRes != gWas)
        Log("eye resolution %d %% (%d of %d frames late at %.0f Hz)", int(std::lround(s_gEyeRes * 100.0f)), s_cMissWindow, s_cfWindow, 1.0 / period);
    s_tWindow = t;
    s_cfWindow = s_cMissWindow = 0;
}

// the view's anchor (Recenter): the head's position and heading then, in the LOCAL space
glm::vec3 s_posAnchor{0.0f};
float s_yawAnchor = 0.0f;
bool s_fAnchored = false;
float s_yawPanel = 0.0f;   // the HUD panel's heading (Immersive: it follows the head lazily)
// the game camera's heading last frame (the vignette's turn rate) and position
float s_yawCamPrev = 0.0f;
glm::vec3 s_posCamPrev{0.0f};
double s_tCamPrev = -1.0;
float s_gVignette = 0.0f;
// the game's moment (SetGameView) and what the views keep across frames
int s_view = VRVIEW_Normal, s_viewPrev = VRVIEW_Normal;
float s_yawHead = 0.0f, s_pitchHead = 0.0f;           // the head now, relative to the anchor (BeginFrame)
float s_yawConsumed = 0.0f, s_pitchConsumed = 0.0f;   // Binoc: the head's turn already given to the game (FHeadLook)
glm::vec3 s_posHeadBinoc{0.0f};                        // Binoc: the head's place when it opened
glm::vec3 s_posRc{0.0f};                               // RcTable: the fixed view's place and heading
float s_yawRc = 0.0f;
float s_zRcArena = 0.0f;
glm::vec3 s_posAim{0.0f};                              // RcTable: where the head looks on the arena
bool s_fAim = false;
glm::mat4 s_amatEye[2]{glm::mat4(1.0f), glm::mat4(1.0f)};   // this frame's eyes (the aim mark)
bool s_fShotPanel = false;      // (SLY2N_VR_SHOT: the HUD panel's picture at Present)
std::string s_strShotPanel;

// ------------------------------------------------------------ math
glm::quat Quat(const XrQuaternionf& q) { return glm::quat(q.w, q.x, q.y, q.z); }
glm::vec3 Vec(const XrVector3f& v) { return glm::vec3(v.x, v.y, v.z); }
XrQuaternionf XrQuat(const glm::quat& q) { return XrQuaternionf{q.x, q.y, q.z, q.w}; }
XrVector3f XrVec(const glm::vec3& v) { return XrVector3f{v.x, v.y, v.z}; }
// the heading (radians, around +Y) of an orientation in the XR space (-Z forward)
float YawOf(const glm::quat& q)
{
    const glm::vec3 f = q * glm::vec3(0, 0, -1);
    return std::atan2(-f.x, -f.z);
}
// the reversed-Z infinite-far projection of an eye's field of view (the game's convention: render.cpp
// CAMERA::MatWorldToClip, clip control 0..1, depth = near / w)
glm::mat4 ProjOfFov(const XrFovf& fov, float sNear)
{
    const float tl = std::tan(fov.angleLeft), tr = std::tan(fov.angleRight), tu = std::tan(fov.angleUp), td = std::tan(fov.angleDown);
    glm::mat4 m(0.0f);
    m[0][0] = 2.0f / (tr - tl);
    m[2][0] = (tr + tl) / (tr - tl);
    m[1][1] = 2.0f / (tu - td);
    m[2][1] = (tu + td) / (tu - td);
    m[2][3] = -1.0f;
    m[3][2] = sNear;
    return m;
}

// ------------------------------------------------------------ GL helpers
#if defined(__ANDROID__)
using PFNFBTEX2DMS = void (*)(GLenum, GLenum, GLenum, GLuint, GLint, GLsizei);
using PFNRBSTORAGEMS = void (*)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
PFNFBTEX2DMS s_pfnFbTex2DMs = nullptr;
PFNRBSTORAGEMS s_pfnRbStorageMs = nullptr;
using PFNFBTEXMSMV = void (*)(GLenum, GLenum, GLuint, GLint, GLsizei, GLint, GLsizei);
PFNFBTEXMSMV s_pfnFbTexMsMv = nullptr;   // glFramebufferTextureMultisampleMultiviewOVR
#endif
constexpr GLenum kGlFramebufferSrgb = 0x8DB9;   // GL_FRAMEBUFFER_SRGB (_EXT)
// glInvalidateFramebuffer (GL 4.3 / ES 3.0), looked up by name (the GL 4.3 loader leaves it out on an ES context)
using PFNINVALIDATE = void (*)(GLenum, GLsizei, const GLenum*);
PFNINVALIDATE s_pfnInvalidate = nullptr;

bool FGlExtension(const char* sz)
{
    GLint c = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &c);
    for (GLint i = 0; i < c; ++i)
        if (const char* p = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, GLuint(i))); p && !std::strcmp(p, sz)) return true;
    return false;
}

// a small program from inline sources (the version line for the context: GLSL ES 3.00 or GLSL 3.30)
GLuint ProgramFrom(const char* szVert, const char* szFrag, const char* szExt = "")
{
    const std::string head = kGles ? std::string("#version 300 es\n") + szExt + "precision highp float;\n" : std::string("#version 330 core\n") + szExt;
    auto compile = [&](GLenum kind, const char* sz) {
        const std::string src = head + sz;
        const char* p = src.c_str();
        GLuint sh = glCreateShader(kind);
        glShaderSource(sh, 1, &p, nullptr);
        glCompileShader(sh);
        GLint ok = 0;
        glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char ach[1024] = {};
            glGetShaderInfoLog(sh, sizeof ach, nullptr, ach);
            Log("shader: %s", ach);
        }
        return sh;
    };
    const GLuint v = compile(GL_VERTEX_SHADER, szVert), f = compile(GL_FRAGMENT_SHADER, szFrag);
    const GLuint prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    glLinkProgram(prog);
    glDeleteShader(v);
    glDeleteShader(f);
    return prog;
}
GLSHADER s_shCopy, s_shVignette, s_shMark;
GLuint s_progCopy = 0, s_progVignette = 0, s_vaoEmpty = 0;
GLuint s_progMark = 0, s_vaoMark = 0, s_vboMark = 0;   // (the RC table's aim mark: clip-space triangles, one colour)
GLint s_locVigCenter = -1, s_locVigStrength = -1, s_locVigScale = -1;
// (multiview: the vignette over both views at once, each view's centre and scale)
GLuint s_progVignetteMv = 0;
GLint s_locVigMvCenter = -1, s_locVigMvStrength = -1, s_locVigMvScale = -1;
const char* const s_szFragVignette =
    "uniform float strength;\nout vec4 FragColor;\n"
    "void main() { vec2 d = (vUv * 2.0 - 1.0 - vCenter) * vScale; float r = length(d);\n"
    "  float inner = mix(1.1, 0.35, strength); float a = smoothstep(inner, inner + 0.45, r) * min(strength * 1.5, 1.0);\n"
    "  FragColor = vec4(0.0, 0.0, 0.0, a); }\n";

void InitGlPrograms()
{
    if (s_progCopy) return;
#if defined(__ANDROID__)
    if (s_fVk) s_pfnInvalidate = reinterpret_cast<PFNINVALIDATE>(cane::gl::ProcAddress("glInvalidateFramebuffer"));
    else s_pfnInvalidate = reinterpret_cast<PFNINVALIDATE>(eglGetProcAddress("glInvalidateFramebuffer"));
#else
    s_pfnInvalidate = reinterpret_cast<PFNINVALIDATE>(cane::gl::ProcAddress("glInvalidateFramebuffer"));
#endif
    // (the game's own program files: vrfull.vert, vrcopy.frag, vrvignette.frag, vrmark.*; on OpenGL and on the Vulkan
    // renderer alike, which compiles them at build time)
    s_shCopy.Init("vrfull.vert", "vrcopy.frag");
#if CANE_VULKAN
    if (s_fVk && !s_shCopyArray.FInit("vrfull.vert", "vrcopy.frag", "#define CANE_ARRAY 1\n")) s_shCopyArray.ID = 0;
#endif
    s_shVignette.Init("vrfull.vert", "vrvignette.frag");
    s_shMark.Init("vrmark.vert", "vrmark.frag");
    s_progCopy = s_shCopy.ID;
    s_progVignette = s_shVignette.ID;
    s_locVigCenter = glGetUniformLocation(s_progVignette, "center");
    s_locVigStrength = glGetUniformLocation(s_progVignette, "strength");
    s_locVigScale = glGetUniformLocation(s_progVignette, "scale");
    glGenVertexArrays(1, &s_vaoEmpty);
    s_progMark = s_shMark.ID;
    glGenVertexArrays(1, &s_vaoMark);
    glGenBuffers(1, &s_vboMark);
    glBindVertexArray(s_vaoMark);
    glBindBuffer(GL_ARRAY_BUFFER, s_vboMark);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 4, GL_FLOAT, GL_FALSE, 16, nullptr);
    glBindVertexArray(0);
    if (FGles() && s_fMultiviewExt)
    {
        s_progVignetteMv = ProgramFrom(
            "layout(num_views = 2) in;\nuniform vec2 center[2];\nuniform vec2 scale[2];\nout vec2 vUv;\nflat out vec2 vCenter;\nflat out vec2 vScale;\n"
            "void main() { vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)); vUv = p; vCenter = center[gl_ViewID_OVR];\n"
            "  vScale = scale[gl_ViewID_OVR]; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n",
            (std::string("in vec2 vUv;\nflat in vec2 vCenter;\nflat in vec2 vScale;\n") + s_szFragVignette).c_str(),
            "#extension GL_OVR_multiview2 : require\n");
        s_locVigMvCenter = glGetUniformLocation(s_progVignetteMv, "center");
        s_locVigMvStrength = glGetUniformLocation(s_progVignetteMv, "strength");
        s_locVigMvScale = glGetUniformLocation(s_progVignetteMv, "scale");
    }
}

// ------------------------------------------------------------ swapchains
int64_t s_formatView = 0;   // (Vulkan) the format the images are written through (UNORM on an sRGB image)
int64_t ChooseColorFormat()
{
#if CANE_VULKAN
    if (s_fVk)
    {
        uint32_t cv = 0;
        pxrEnumerateSwapchainFormats(s_session, 0, &cv, nullptr);
        std::vector<int64_t> av(cv);
        pxrEnumerateSwapchainFormats(s_session, cv, &cv, av.data());
        auto has = [&](VkFormat f) { return std::find(av.begin(), av.end(), int64_t(f)) != av.end(); };
        // (an sRGB image written through a UNORM view: the game's colours are display-encoded already, the runtime
        // reads them as sRGB)
        if (has(VK_FORMAT_R8G8B8A8_SRGB)) { s_formatView = VK_FORMAT_R8G8B8A8_UNORM; return VK_FORMAT_R8G8B8A8_SRGB; }
        if (has(VK_FORMAT_R8G8B8A8_UNORM)) { s_formatView = VK_FORMAT_R8G8B8A8_UNORM; return VK_FORMAT_R8G8B8A8_UNORM; }
        s_formatView = av.empty() ? int64_t(VK_FORMAT_R8G8B8A8_UNORM) : av.front();
        return s_formatView;
    }
#endif
    uint32_t c = 0;
    pxrEnumerateSwapchainFormats(s_session, 0, &c, nullptr);
    std::vector<int64_t> a(c);
    pxrEnumerateSwapchainFormats(s_session, c, &c, a.data());
    // sRGB (the game's colours are already display-encoded: written as they are, the runtime reads them as sRGB);
    // on GLES only where the encoding on write can be turned off (EXT_sRGB_write_control)
    const bool fSrgbOk = !kGles || s_fSrgbWriteControl;
    if (fSrgbOk && std::find(a.begin(), a.end(), int64_t(GL_SRGB8_ALPHA8)) != a.end()) return GL_SRGB8_ALPHA8;
    if (std::find(a.begin(), a.end(), int64_t(GL_RGBA8)) != a.end()) return GL_RGBA8;
    return a.empty() ? int64_t(GL_RGBA8) : a.front();
}

// (Vulkan with the submission worker, gl_vulkan.h AfterNextSubmit: the runtime uses the queue it shares with the
// layer inside xrBegin/EndFrame and the swapchain image calls. The images' releases and xrEndFrame run on the worker
// after its submission of the frame (the frame's commands recorded and submitted there while the game thread goes on
// with the next step); this thread's own calls of that kind first wait for the worker's submissions.
// SLY2N_VRVKTHREAD=0 / 1: no worker in VR (everything on the game thread as before) / the worker; the default is on
// except on Android until it is measured on the Quest (renderer.cpp).)
void WaitVkSubmissions()
{
#if CANE_VULKAN
    if (s_fVk) CaneVk::WaitSubmissions();
#endif
}
bool FVkDeferred()
{
#if CANE_VULKAN
    return s_fVk && CaneVk::FSubmitsOnWorker();
#else
    return false;
#endif
}

bool FCreateSwapchain(SWAPCHAIN& sc, int dx, int dy, int64_t format, uint32_t sampleCount, uint32_t arraySize = 1, bool fEye = false)
{
    WaitVkSubmissions();
    XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
#if CANE_VULKAN
    // (the eyes drawn straight into the images: their Vulkan flags for the tile MSAA / the in-place read)
    XrVulkanSwapchainCreateInfoMETA vci{XR_TYPE_VULKAN_SWAPCHAIN_CREATE_INFO_META};
    const bool fDirect = s_fVk && s_fVkDirect && fEye;
    const bool fTileMsaa = fDirect && s_fExtVkSwapchainMeta && s_msaa > 1 && CaneVk::RendersToSingleSampled();
    const bool fInput = fDirect && s_fExtVkSwapchainMeta && !fTileMsaa && CaneVk::CanReadInPlace();
    if (fDirect && s_fExtVkSwapchainMeta)
    {
        vci.additionalCreateFlags = fTileMsaa ? VK_IMAGE_CREATE_MULTISAMPLED_RENDER_TO_SINGLE_SAMPLED_BIT_EXT : 0;
        vci.additionalUsageFlags = fInput ? VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT : 0;
        ci.next = &vci;
    }
#endif
    ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT |
                    XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT;
    if (s_fVk && s_formatView != format) ci.usageFlags |= XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
    ci.format = format;
    ci.sampleCount = sampleCount;
    ci.width = uint32_t(dx);
    ci.height = uint32_t(dy);
    ci.faceCount = 1;
    ci.arraySize = arraySize;
    ci.mipCount = 1;
    if (!FCheck(pxrCreateSwapchain(s_session, &ci, &sc.h), "xrCreateSwapchain")) return false;
    uint32_t c = 0;
    pxrEnumerateSwapchainImages(sc.h, 0, &c, nullptr);
#if CANE_VULKAN
    if (s_fVk)
    {
        // (the images as the Vulkan renderer's GL textures: attachments and blit targets)
        std::vector<XrSwapchainImageVulkan2KHR> aimgVk(c, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
        if (!FCheck(pxrEnumerateSwapchainImages(sc.h, c, &c, reinterpret_cast<XrSwapchainImageBaseHeader*>(aimgVk.data())), "xrEnumerateSwapchainImages"))
            return false;
        sc.dx = dx, sc.dy = dy, sc.format = format;
        sc.atex.clear();
        for (const auto& img : aimgVk)
        {
            sc.atex.push_back(CaneVk::WrapImage(img.image, VkFormat(s_formatView), uint32_t(dx), uint32_t(dy), arraySize));
            if (fDirect)
                CaneVk::SetImageAttributes(sc.atex.back(), fTileMsaa ? uint32_t(s_msaa) : 1u, fInput,
                                           VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                               VK_IMAGE_USAGE_TRANSFER_DST_BIT | (fInput ? VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT : 0));
        }
        sc.afbo.assign(sc.atex.size(), 0);
        glGenFramebuffers(GLsizei(sc.afbo.size()), sc.afbo.data());
        return true;
    }
#endif
#if defined(__ANDROID__)
    std::vector<XrSwapchainImageOpenGLESKHR> aimg(c, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
#else
    std::vector<XrSwapchainImageOpenGLKHR> aimg(c, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_KHR});
#endif
    if (!FCheck(pxrEnumerateSwapchainImages(sc.h, c, &c, reinterpret_cast<XrSwapchainImageBaseHeader*>(aimg.data())), "xrEnumerateSwapchainImages"))
        return false;
    sc.dx = dx;
    sc.dy = dy;
    sc.format = format;
    sc.atex.clear();
    for (const auto& img : aimg) sc.atex.push_back(img.image);
    sc.afbo.assign(sc.atex.size(), 0);
    glGenFramebuffers(GLsizei(sc.afbo.size()), sc.afbo.data());
    return true;
}

std::map<GLuint, bool> s_mpfFboReady;
void DestroySwapchain(SWAPCHAIN& sc)
{
    WaitVkSubmissions();
    for (GLuint fbo : sc.afbo) s_mpfFboReady.erase(fbo);
    if (!sc.afbo.empty()) glDeleteFramebuffers(GLsizei(sc.afbo.size()), sc.afbo.data());
    if (sc.h) pxrDestroySwapchain(sc.h);
    sc = SWAPCHAIN{};
}

bool FAcquire(SWAPCHAIN& sc)
{
    WaitVkSubmissions();
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (!FCheck(pxrAcquireSwapchainImage(sc.h, &ai, &sc.iimage), "xrAcquireSwapchainImage")) return false;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    if (!FCheck(pxrWaitSwapchainImage(sc.h, &wi), "xrWaitSwapchainImage"))
    {
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        pxrReleaseSwapchainImage(sc.h, &ri);
        return false;
    }
    sc.fAcquired = true;
#if CANE_VULKAN
    if (s_fVk) CaneVk::AcquireImage(sc.atex[sc.iimage], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
#endif
    return true;
}
void Release(SWAPCHAIN& sc)
{
    if (!sc.fAcquired) return;
#if CANE_VULKAN
    if (FVkDeferred())
    {
        // (the layout now; the commands with the frame's submission, the release after it on the worker)
        CaneVk::ReleaseImage(sc.atex[sc.iimage], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, false);
        const XrSwapchain h = sc.h;
        CaneVk::AfterNextSubmit([h] {
            XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
            pxrReleaseSwapchainImage(h, &ri);
        });
        sc.fAcquired = false;
        return;
    }
    // (the image back in the runtime's layout, the commands that drew it submitted to the queue the session shares)
    if (s_fVk) CaneVk::ReleaseImage(sc.atex[sc.iimage], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
#endif
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    pxrReleaseSwapchainImage(sc.h, &ri);
    sc.fAcquired = false;
}

// (test, SLY2N_VR_ORIENTCHECK=1: each layer image's rows against its source picture's, logged once per kind: an OpenGL
// image is read by the runtime bottom row first like the source, a Vulkan one top row first, so its first row in memory
// must be the source's last)
// (read on first use: on Android the test environment is set after the library's static initialisers)
bool FOrientCheck()
{
    static const bool s_f = std::getenv("SLY2N_VR_ORIENTCHECK") && std::atoi(std::getenv("SLY2N_VR_ORIENTCHECK")) != 0;
    return s_f;
}
void CheckOrientation(const char* szKind, GLuint fboSource, GLuint fboImage, int dx, int dy)
{
    static std::map<std::string, int> s_mpcChecked;
    int& c = s_mpcChecked[szKind];
    if (++c != 120) return;   // (a frame well after the start: a picture with content)
    std::vector<uint8_t> abSrc(size_t(dx) * size_t(dy) * 4), abImg(abSrc.size());
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fboSource);
    glReadPixels(0, 0, dx, dy, GL_RGBA, GL_UNSIGNED_BYTE, abSrc.data());
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fboImage);
    glReadPixels(0, 0, dx, dy, GL_RGBA, GL_UNSIGNED_BYTE, abImg.data());
    // (the rows' mean colour difference, the image's rows taken in the order the runtime reads them)
    auto diff = [&](bool fFlipped) {
        double d = 0.0;
        for (int y = 0; y < dy; y += 4)
        {
            const uint8_t* ps = &abSrc[size_t(y) * size_t(dx) * 4];
            const uint8_t* pi = &abImg[size_t(fFlipped ? dy - 1 - y : y) * size_t(dx) * 4];
            for (int x = 0; x < dx * 4; x += 4 * 4)
                for (int k = 0; k < 3; ++k) d += std::abs(int(ps[x + k]) - int(pi[x + k]));
        }
        return d;
    };
    if (std::atoi(std::getenv("SLY2N_VR_ORIENTCHECK")) == 2)
    {
        stbi_write_png((std::string("orient_") + szKind + "_src.png").c_str(), dx, dy, 4, abSrc.data(), dx * 4);
        stbi_write_png((std::string("orient_") + szKind + "_img.png").c_str(), dx, dy, 4, abImg.data(), dx * 4);
    }
    const double dSame = diff(false), dFlipped = diff(true);
    const bool fWantFlipped = s_fVk;
    const bool fOk = fWantFlipped ? dFlipped < dSame : dSame < dFlipped;
    Log("orientation check: %s %s (rows as read %.0f, the other way %.0f)", szKind, fOk ? "upright" : "UPSIDE DOWN",
        fWantFlipped ? dFlipped : dSame, fWantFlipped ? dSame : dFlipped);
}

// the eye image's framebuffer: its colour (MSAA on the tile where GLES can) and the eye's depth-stencil
GLuint FboEyeImage(int ieye)
{
    SWAPCHAIN& sc = s_aeye[ieye];
    GLuint& fbo = sc.afbo[sc.iimage];
    if (!s_mpfFboReady[fbo])
    {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
#if CANE_VULKAN
        if (s_fVk && s_fMultiview)
        {
            // (Vulkan multiview: the image's two layers as the two views; its depth when drawn straight in, else it is the
            // copy's target)
            CaneVk::FramebufferMultiview(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, sc.atex[sc.iimage], false, 0, 2);
            if (!s_afboMsaa[0]) CaneVk::FramebufferMultiview(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, s_arboDepth[0], true, 0, 2);
        }
        else
#endif
#if defined(__ANDROID__)
        if (!s_fVk && s_fMultiview)
        {
            // both eyes as the two views of one framebuffer (MSAA resolved on the tile)
            s_pfnFbTexMsMv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, sc.atex[sc.iimage], 0, std::max(s_msaa, 1), 0, 2);
            s_pfnFbTexMsMv(GL_DRAW_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, s_texDepthMv, 0, std::max(s_msaa, 1), 0, 2);
        }
        else if (!s_fVk && s_fGlesMsrtt && s_msaa > 1)
        {
            s_pfnFbTex2DMs(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sc.atex[sc.iimage], 0, s_msaa);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_arboDepth[ieye]);
        }
        else
#endif
        {
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sc.atex[sc.iimage], 0);
            if (!s_afboMsaa[ieye]) glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_arboDepth[ieye]);
        }
        const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
        if (st != GL_FRAMEBUFFER_COMPLETE) Log("eye %d framebuffer incomplete: 0x%x", ieye, st);
        s_mpfFboReady[fbo] = true;
    }
    return fbo;
}

// Multiview wanted: SLY2N_VR_MULTIVIEW=0 / 1 when set; else VR Settings' Multiview on the Vulkan renderer, off on GLES
// (measured slower on the Quest there)
#if defined(__ANDROID__)
constexpr bool kVkEyesGeometryStage = false;   // (render.cpp useGlob: no near-clip geometry stage in Android's VR passes)
#else
constexpr bool kVkEyesGeometryStage = true;
#endif
bool FMultiviewWanted()
{
    if (const char* sz = std::getenv("SLY2N_VR_MULTIVIEW")) return std::atoi(sz) != 0;
    return s_fVk && g_vropts.fMultiview;
}
// The eyes' swapchains and targets gone (to be made again: multiview turned on or off)
void DestroyEyeTargets()
{
    WaitVkSubmissions();
    for (int i = 0; i < 2; ++i)
    {
        DestroySwapchain(s_aeye[i]);
        if (s_arboDepth[i]) glDeleteRenderbuffers(1, &s_arboDepth[i]), s_arboDepth[i] = 0;
        if (s_arboColorMsaa[i]) glDeleteRenderbuffers(1, &s_arboColorMsaa[i]), s_arboColorMsaa[i] = 0;
        if (s_afboMsaa[i]) glDeleteFramebuffers(1, &s_afboMsaa[i]), s_afboMsaa[i] = 0;
        if (s_atexEyeVk[i]) glDeleteTextures(1, &s_atexEyeVk[i]), s_atexEyeVk[i] = 0;
        for (GLuint& fbo : s_afboLayerVk[i]) if (fbo) glDeleteFramebuffers(1, &fbo), fbo = 0;
    }
    if (s_texDepthMv) glDeleteTextures(1, &s_texDepthMv), s_texDepthMv = 0;
    s_levelFoveationApplied = -1;
}

bool FCreateEyeTargets()
{
    s_formatColor = ChooseColorFormat();
    s_fSrgb = s_formatColor == GL_SRGB8_ALPHA8;
#if CANE_VULKAN
    if (s_fVk) s_fSrgb = s_formatColor == VK_FORMAT_R8G8B8A8_SRGB;
#endif
    // MSAA: 2x on GLES (on-tile resolve), else the game's own setting
    GLint maxSamples = 1;
    glGetIntegerv(GL_MAX_SAMPLES, &maxSamples);
    // (GLES: 2x, resolved on the tile; on the Quest 3S 4x costs a third of the frame rate, 2x little over none)
    s_msaa = kGles ? (s_fGlesMsrtt ? 2 : 1) : std::max(1, g_display.msaaSamples);
#if CANE_VULKAN
    // (Vulkan: 2x where the device draws multisampled into single-sampled images, resolved on the tile
    // (VK_EXT_multisampled_render_to_single_sampled, as GLES's EXT_multisampled_render_to_texture); else 1x: the eye's
    // target is drawn into the image by a program, which reads one sample)
    if (s_fVk) s_msaa = CaneVk::RendersToSingleSampled() ? 2 : 1;
#endif
    if (g_vropts.msaa <= 1) s_msaa = 1;   // (VR Settings: MSAA off)
    if (const char* sz = std::getenv("SLY2N_VR_MSAA")) s_msaa = std::max(1, std::atoi(sz));
#if CANE_VULKAN
    if (s_fVk && !CaneVk::RendersToSingleSampled()) s_msaa = 1;
    // (straight into the images only for one pass an eye: with MSAA the eye's own target and its copy measured faster)
    // (test, SLY2N_VR_VKDIRECT=2: straight into the images on a runtime without XR_META_vulkan_swapchain_create_info, e.g.
    // Monado: the path's drawing checked on the desktop, the shadow phases then copying the frame from the image)
    const bool fDirectTest = s_fVk && std::getenv("SLY2N_VR_VKDIRECT") && std::atoi(std::getenv("SLY2N_VR_VKDIRECT")) == 2;
    s_fVkDirect = s_fExtVkSwapchainMeta || fDirectTest;
    if (s_fVkDirect && !(s_msaa == 1 && (CaneVk::CanReadInPlace() || fDirectTest))) s_fVkDirect = false;
#endif
    s_msaa = std::min(s_msaa, std::max(1, int(maxSamples)));
    // (GLES with OVR_multiview: one two-layer swapchain, both eyes drawn in one pass; the glob program's two-view
    // variant is tried now: if it cannot be made, the eyes one by one)
    s_fMultiview = false;
    // (GLES: off by default: on the Quest 3S (Adreno 740, driver 0837) the two-view pass saves CPU (main thread 15.6 ->
    // 12.5 ms) but costs more GPU (17 -> 21 ms a frame in Paris), a net loss; SLY2N_VR_MULTIVIEW=1 turns it on)
    if (!s_fVk && s_fMultiviewExt && FMultiviewWanted() && !g_fMultiviewFailed)
    {
        g_fMultiviewPass = true;
        // (the programs the eyes use: without the near-clip stage where it was built, render.cpp useGlob on Android)
        (glGlobShaderNoGeom.ID ? glGlobShaderNoGeom : glGlobShader).Use();
        if (!g_fMultiviewFailed) (glGlobShaderNoKillNoGeom.ID ? glGlobShaderNoKillNoGeom : glGlobShaderNoKill).Use(), glCelBorderShader.Use();
        g_fMultiviewPass = false;
        glUseProgram(0);
        s_fMultiview = !g_fMultiviewFailed;
    }
#if CANE_VULKAN
    // (Vulkan: VK_KHR_multiview where the device has it (the programs' two-view stages are built ahead); the copy into the
    // images needs the array copy program)
    if (s_fVk && FMultiviewWanted())
    {
        if (!CaneVk::MultiviewSupported(kVkEyesGeometryStage)) Log("multiview: not on this device (the eyes one by one)");
        else if (!s_fVkDirect && !s_shCopyArray.ID) Log("multiview: no array copy program (the eyes one by one)");
        else s_fMultiview = true;
    }
    if (s_fVk && s_fMultiview)
    {
        // one two-layer swapchain (a layer an eye), a two-layer depth-stencil; the eyes' own two-layer target unless drawn
        // straight into the images
        const XrViewConfigurationView& v = s_aviewcfg[0];
        int dx = int(v.recommendedImageRectWidth), dy = int(v.recommendedImageRectHeight);
        if (const char* sz = std::getenv("SLY2N_VR_EYESCALE"))
        {
            const float g = float(std::atof(sz));
            if (g > 0.1f) dx = int(dx * g), dy = int(dy * g);
        }
        dx = std::min(dx, int(v.maxImageRectWidth));
        dy = std::min(dy, int(v.maxImageRectHeight));
        if (!FCreateSwapchain(s_aeye[0], dx, dy, s_formatColor, 1, 2, true))
        {
            Log("multiview: no two-layer swapchain (the eyes one by one)");
            s_fMultiview = false;
        }
        else
        {
            glGenRenderbuffers(1, &s_arboDepth[0]);
            glBindRenderbuffer(GL_RENDERBUFFER, s_arboDepth[0]);
            CaneVk::SetNextRenderbufferLayers(2);
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, s_msaa, GL_DEPTH32F_STENCIL8, dx, dy);
            CaneVk::SetRenderbufferTransient(s_arboDepth[0], true);   // (cleared at the start, dropped at the end)
            if (!s_fVkDirect)
            {
                glGenTextures(1, &s_atexEyeVk[0]);
                glBindTexture(GL_TEXTURE_2D_ARRAY, s_atexEyeVk[0]);
                CaneVk::SetNextImageRenderSamples(uint32_t(s_msaa));   // (MSAA resolved on the tile, as the depth's)
                glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8, dx, dy, 2);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
                glGenFramebuffers(1, &s_afboMsaa[0]);
                glBindFramebuffer(GL_FRAMEBUFFER, s_afboMsaa[0]);
                CaneVk::FramebufferMultiview(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, s_atexEyeVk[0], false, 0, 2);
                CaneVk::FramebufferMultiview(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, s_arboDepth[0], true, 0, 2);
            }
        }
    }
    if (s_fMultiview && !s_fVk)
#else
    if (s_fMultiview)
#endif
    {
        const XrViewConfigurationView& v = s_aviewcfg[0];
        int dx = int(v.recommendedImageRectWidth), dy = int(v.recommendedImageRectHeight);
        if (const char* sz = std::getenv("SLY2N_VR_EYESCALE"))
        {
            const float g = float(std::atof(sz));
            if (g > 0.1f) dx = int(dx * g), dy = int(dy * g);
        }
        dx = std::min(dx, int(v.maxImageRectWidth));
        dy = std::min(dy, int(v.maxImageRectHeight));
        if (!FCreateSwapchain(s_aeye[0], dx, dy, s_formatColor, 1, 2)) return false;
        glGenTextures(1, &s_texDepthMv);
        glBindTexture(GL_TEXTURE_2D_ARRAY, s_texDepthMv);
        glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_DEPTH24_STENCIL8, dx, dy, 2);
        glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
    }
    for (int i = 0; i < 2 && !s_fMultiview; ++i)
    {
        const XrViewConfigurationView& v = s_aviewcfg[size_t(i)];
        int dx = int(v.recommendedImageRectWidth), dy = int(v.recommendedImageRectHeight);
        if (const char* sz = std::getenv("SLY2N_VR_EYESCALE"))
        {
            const float g = float(std::atof(sz));
            if (g > 0.1f) dx = int(dx * g), dy = int(dy * g);
        }
        dx = std::min(dx, int(v.maxImageRectWidth));
        dy = std::min(dy, int(v.maxImageRectHeight));
        if (!FCreateSwapchain(s_aeye[i], dx, dy, s_formatColor, 1, 1, true)) return false;
        glGenRenderbuffers(1, &s_arboDepth[i]);
        glBindRenderbuffer(GL_RENDERBUFFER, s_arboDepth[i]);
        // (Vulkan: the eye is drawn into a target of its own and copied flipped into the image, FinishEyes: the
        // renderer's images keep GL's rows, bottom first, and OpenXR reads a Vulkan image top first)
#if defined(__ANDROID__)
        if (!s_fVk)
        {
            if (s_fGlesMsrtt && s_msaa > 1) s_pfnRbStorageMs(GL_RENDERBUFFER, s_msaa, GL_DEPTH24_STENCIL8, dx, dy);
            else glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, dx, dy);
        }
        else
#endif
        if (s_fVkDirect)
        {
            // (straight into the image: the depth alone, as the image's samples; not stored)
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, s_msaa, GL_DEPTH32F_STENCIL8, dx, dy);
#if CANE_VULKAN
            CaneVk::SetRenderbufferTransient(s_arboDepth[i], true);
#endif
        }
        else if (s_msaa > 1 || s_fVk)
        {
            // desktop: an MSAA target, resolved into the image after the eye (FinishEyes); Vulkan: the eye's target
            glRenderbufferStorageMultisample(GL_RENDERBUFFER, s_msaa, GL_DEPTH32F_STENCIL8, dx, dy);
            glGenFramebuffers(1, &s_afboMsaa[i]);
            if (s_fVk)
            {
                // (a texture: drawn into the image by vrcopy, through the image's view, flipped; a blit would convert
                // into the image's sRGB format)
                glGenTextures(1, &s_atexEyeVk[i]);
                glBindTexture(GL_TEXTURE_2D, s_atexEyeVk[i]);
#if CANE_VULKAN
                CaneVk::SetNextImageRenderSamples(uint32_t(s_msaa));   // (MSAA resolved on the tile, the depth's alike)
#endif
                glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, dx, dy);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                glBindTexture(GL_TEXTURE_2D, 0);
                glBindFramebuffer(GL_FRAMEBUFFER, s_afboMsaa[i]);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_atexEyeVk[i], 0);
            }
            else
            {
                glGenRenderbuffers(1, &s_arboColorMsaa[i]);
                glBindRenderbuffer(GL_RENDERBUFFER, s_arboColorMsaa[i]);
                glRenderbufferStorageMultisample(GL_RENDERBUFFER, s_msaa, GL_RGBA8, dx, dy);
                glBindFramebuffer(GL_FRAMEBUFFER, s_afboMsaa[i]);
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, s_arboColorMsaa[i]);
            }
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, s_arboDepth[i]);
#if CANE_VULKAN
            // (the eye's depth: cleared at its start, dropped at its end: not written out, a tiler's store saved)
            if (s_fVk) CaneVk::SetRenderbufferTransient(s_arboDepth[i], true);
#endif
        }
        else glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH32F_STENCIL8, dx, dy);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    Log("eyes %dx%d, %s, MSAA %dx%s%s, %s", s_aeye[0].dx, s_aeye[0].dy, s_fSrgb ? "sRGB" : "RGBA8", s_msaa,
        (FGles() && s_fGlesMsrtt || s_fVk) && s_msaa > 1 ? " (resolved on the tile)" : "", s_fMultiview ? ", multiview (both eyes in one pass)" : "",
        s_fVk ? (s_fVkDirect ? "Vulkan, straight into the images" : "Vulkan") : kGles ? "OpenGL ES" : "OpenGL");
#if CANE_VULKAN
    // (Vulkan: a density map per eye, linked to what the eye draws into; filled per level / resolution: ApplyFoveation;
    // multiview: one of two layers, a layer an eye; made once, kept when the targets are made again)
    if (s_fVk && CaneVk::DensityMapsSupported() && s_fMultiview)
    {
        if (!s_mapFdmMv) s_mapFdmMv = CaneVk::CreateDensityMap(uint32_t(s_aeye[0].dx), uint32_t(s_aeye[0].dy), 2);
        if (s_atexEyeVk[0]) CaneVk::SetDensityMap(s_atexEyeVk[0], s_mapFdmMv);
        else for (GLuint tex : s_aeye[0].atex) CaneVk::SetDensityMap(tex, s_mapFdmMv);
    }
    else if (s_fVk && CaneVk::DensityMapsSupported())
        for (int i = 0; i < 2; ++i)
        {
            if (!s_amapFdm[i]) s_amapFdm[i] = CaneVk::CreateDensityMap(uint32_t(s_aeye[i].dx), uint32_t(s_aeye[i].dy));
            if (s_atexEyeVk[i]) CaneVk::SetDensityMap(s_atexEyeVk[i], s_amapFdm[i]);
            else for (GLuint tex : s_aeye[i].atex) CaneVk::SetDensityMap(tex, s_amapFdm[i]);
        }
#endif
    s_levelFoveationApplied = -1;
    ApplyFoveation();
    return true;
}

// The fixed foveation's level (VR Settings) to the eyes: GLES through the runtime (XR_FB_foveation), Vulkan through the
// density maps (VK_EXT_fragment_density_map, centred on each lens, over the part drawn); again when it changes
void ApplyFoveation()
{
    int level = std::clamp(g_vropts.foveation, 0, 3);
    if (const char* sz = std::getenv("SLY2N_VR_FOVEATION")) level = std::clamp(std::atoi(sz), 0, 3);
#if CANE_VULKAN
    if (s_fVk)
    {
        const int dxUsed = s_dxEye ? s_dxEye : s_aeye[0].dx, dyUsed = s_dyEye ? s_dyEye : s_aeye[0].dy;
        const bool fMapMv = s_fMultiview && s_mapFdmMv;
        if (!(fMapMv || s_amapFdm[0]) || (level == s_levelFoveationApplied && dxUsed == s_dxFdm && dyUsed == s_dyFdm)) return;
        for (int i = 0; i < 2; ++i)
        {
            float cx = 0.0f, cy = 0.0f;
            if (s_frame.fViews)
            {
                const XrFovf& f = s_frame.aview[i].fov;
                const float l = std::tan(f.angleLeft), r = std::tan(f.angleRight), u = std::tan(f.angleUp), d = std::tan(f.angleDown);
                cx = -(r + l) / (r - l);
                cy = (s_fVkDirect ? 1.0f : -1.0f) * (u + d) / (u - d);   // (the rows' direction: as the vignette's)
            }
            if (fMapMv) CaneVk::FillDensityMap(s_mapFdmMv, uint32_t(dxUsed), uint32_t(dyUsed), cx, cy, level, uint32_t(i));
            else CaneVk::FillDensityMap(s_amapFdm[i], uint32_t(dxUsed), uint32_t(dyUsed), cx, cy, level);
        }
        if (level != s_levelFoveationApplied) Log("fixed foveation: level %d (Vulkan density map)", level);
        s_levelFoveationApplied = level, s_dxFdm = dxUsed, s_dyFdm = dyUsed;
        return;
    }
#endif
    // Meta's fixed foveated rendering (GLES, XR_FB_foveation): the eyes' edges at a lower density
    if (level == s_levelFoveationApplied) return;
    s_levelFoveationApplied = level;
    if (s_fExtFoveation && pxrCreateFoveationProfileFB && pxrUpdateSwapchainFB)
    {
        XrFoveationLevelProfileCreateInfoFB lci{XR_TYPE_FOVEATION_LEVEL_PROFILE_CREATE_INFO_FB};
        lci.level = XrFoveationLevelFB(level);
        lci.verticalOffset = 0.0f;
        lci.dynamic = XR_FOVEATION_DYNAMIC_DISABLED_FB;
        XrFoveationProfileCreateInfoFB pci{XR_TYPE_FOVEATION_PROFILE_CREATE_INFO_FB};
        pci.next = &lci;
        XrFoveationProfileFB profile = XR_NULL_HANDLE;
        if (FCheck(pxrCreateFoveationProfileFB(s_session, &pci, &profile), "xrCreateFoveationProfileFB"))
        {
            XrSwapchainStateFoveationFB fs{XR_TYPE_SWAPCHAIN_STATE_FOVEATION_FB};
            fs.profile = profile;
            for (SWAPCHAIN& sc : s_aeye)
                if (sc.h) FCheck(pxrUpdateSwapchainFB(sc.h, reinterpret_cast<XrSwapchainStateBaseHeaderFB*>(&fs)), "xrUpdateSwapchainFB");
            pxrDestroyFoveationProfileFB(profile);
            Log("fixed foveated rendering: level %d", level);
        }
    }
}

// ------------------------------------------------------------ actions (the controllers as a PS2 pad)
enum ACT : int
{
    ACT_A, ACT_B, ACT_X, ACT_Y,                // the face buttons (Touch: A / B right, X / Y left)
    ACT_GripL, ACT_GripR, ACT_TrigL, ACT_TrigR, // values
    ACT_StickL, ACT_StickR,                    // vec2
    ACT_ClickL, ACT_ClickR,                    // the sticks' clicks
    ACT_Menu,                                  // Touch's left menu button: Start as well
    ACT_Start, ACT_Select,                     // controllers with real Start / Select buttons
    ACT_HapticL, ACT_HapticR,
    ACT_Max
};
XrActionSet s_actionset = XR_NULL_HANDLE;
std::array<XrAction, ACT_Max> s_aaction{};
XrPath s_pathLeft = XR_NULL_PATH, s_pathRight = XR_NULL_PATH;
GLFWgamepadstate s_pad{};
bool s_fPad = false;           // the controllers are in use (an interaction profile bound)
bool s_fPadNoted = false;      // (the prompts told once that VR controllers are here)
bool s_fRealStartSelect = false;
bool s_afCombo[2] = {};
cane::input::Family s_familyPrompt = cane::input::Family::MetaQuest;        // grip + stick click held as Start (right) / Select (left)

XrPath Path(const char* sz)
{
    XrPath p = XR_NULL_PATH;
    pxrStringToPath(s_inst, sz, &p);
    return p;
}

bool FCreateActions()
{
    XrActionSetCreateInfo asci{XR_TYPE_ACTION_SET_CREATE_INFO};
    std::strcpy(asci.actionSetName, "gameplay");
    std::strcpy(asci.localizedActionSetName, "Gameplay");
    if (!FCheck(pxrCreateActionSet(s_inst, &asci, &s_actionset), "xrCreateActionSet")) return false;
    struct DEF { const char* name; const char* loc; XrActionType type; };
    static const DEF s_adef[ACT_Max] = {
        {"cross", "Cross", XR_ACTION_TYPE_BOOLEAN_INPUT}, {"circle", "Circle", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {"square", "Square", XR_ACTION_TYPE_BOOLEAN_INPUT}, {"triangle", "Triangle", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {"l1", "L1", XR_ACTION_TYPE_FLOAT_INPUT}, {"r1", "R1", XR_ACTION_TYPE_FLOAT_INPUT},
        {"l2", "L2", XR_ACTION_TYPE_FLOAT_INPUT}, {"r2", "R2", XR_ACTION_TYPE_FLOAT_INPUT},
        {"left_stick", "Left Stick", XR_ACTION_TYPE_VECTOR2F_INPUT}, {"right_stick", "Right Stick", XR_ACTION_TYPE_VECTOR2F_INPUT},
        {"l3", "L3", XR_ACTION_TYPE_BOOLEAN_INPUT}, {"r3", "R3", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {"menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {"start", "Start", XR_ACTION_TYPE_BOOLEAN_INPUT}, {"select", "Select", XR_ACTION_TYPE_BOOLEAN_INPUT},
        {"rumble_left", "Rumble (left)", XR_ACTION_TYPE_VIBRATION_OUTPUT}, {"rumble_right", "Rumble (right)", XR_ACTION_TYPE_VIBRATION_OUTPUT},
    };
    for (int i = 0; i < ACT_Max; ++i)
    {
        XrActionCreateInfo aci{XR_TYPE_ACTION_CREATE_INFO};
        std::strcpy(aci.actionName, s_adef[i].name);
        std::strcpy(aci.localizedActionName, s_adef[i].loc);
        aci.actionType = s_adef[i].type;
        if (!FCheck(pxrCreateAction(s_actionset, &aci, &s_aaction[size_t(i)]), "xrCreateAction")) return false;
    }
    s_pathLeft = Path("/user/hand/left");
    s_pathRight = Path("/user/hand/right");
    struct BIND { int act; const char* path; };
    auto suggest = [&](const char* profile, std::initializer_list<BIND> abind) {
        std::vector<XrActionSuggestedBinding> a;
        for (const BIND& b : abind) a.push_back({s_aaction[size_t(b.act)], Path(b.path)});
        XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
        sb.interactionProfile = Path(profile);
        sb.suggestedBindings = a.data();
        sb.countSuggestedBindings = uint32_t(a.size());
        const XrResult r = pxrSuggestInteractionProfileBindings(s_inst, &sb);
        if (XR_FAILED(r)) Log("bindings for %s not taken (%d)", profile, int(r));
    };
    // Meta Quest Touch (and Touch Pro / Plus as the runtime presents them): A / B / X / Y, grips, triggers, sticks
    suggest("/interaction_profiles/oculus/touch_controller", {
        {ACT_A, "/user/hand/right/input/a/click"}, {ACT_B, "/user/hand/right/input/b/click"},
        {ACT_X, "/user/hand/left/input/x/click"}, {ACT_Y, "/user/hand/left/input/y/click"},
        {ACT_GripL, "/user/hand/left/input/squeeze/value"}, {ACT_GripR, "/user/hand/right/input/squeeze/value"},
        {ACT_TrigL, "/user/hand/left/input/trigger/value"}, {ACT_TrigR, "/user/hand/right/input/trigger/value"},
        {ACT_StickL, "/user/hand/left/input/thumbstick"}, {ACT_StickR, "/user/hand/right/input/thumbstick"},
        {ACT_ClickL, "/user/hand/left/input/thumbstick/click"}, {ACT_ClickR, "/user/hand/right/input/thumbstick/click"},
        {ACT_Menu, "/user/hand/left/input/menu/click"},
        {ACT_HapticL, "/user/hand/left/output/haptic"}, {ACT_HapticR, "/user/hand/right/output/haptic"}});
    // Valve Index: A / B on each hand (left A / B: Square / Triangle)
    suggest("/interaction_profiles/valve/index_controller", {
        {ACT_A, "/user/hand/right/input/a/click"}, {ACT_B, "/user/hand/right/input/b/click"},
        {ACT_X, "/user/hand/left/input/a/click"}, {ACT_Y, "/user/hand/left/input/b/click"},
        {ACT_GripL, "/user/hand/left/input/squeeze/value"}, {ACT_GripR, "/user/hand/right/input/squeeze/value"},
        {ACT_TrigL, "/user/hand/left/input/trigger/value"}, {ACT_TrigR, "/user/hand/right/input/trigger/value"},
        {ACT_StickL, "/user/hand/left/input/thumbstick"}, {ACT_StickR, "/user/hand/right/input/thumbstick"},
        {ACT_ClickL, "/user/hand/left/input/thumbstick/click"}, {ACT_ClickR, "/user/hand/right/input/thumbstick/click"},
        {ACT_HapticL, "/user/hand/left/output/haptic"}, {ACT_HapticR, "/user/hand/right/output/haptic"}});
    // Windows Mixed Reality: the menu buttons are real Start / Select equivalents; the touchpad clicks as the face
    // buttons' Cross / Square
    suggest("/interaction_profiles/microsoft/motion_controller", {
        {ACT_A, "/user/hand/right/input/trackpad/click"}, {ACT_X, "/user/hand/left/input/trackpad/click"},
        {ACT_GripL, "/user/hand/left/input/squeeze/click"}, {ACT_GripR, "/user/hand/right/input/squeeze/click"},
        {ACT_TrigL, "/user/hand/left/input/trigger/value"}, {ACT_TrigR, "/user/hand/right/input/trigger/value"},
        {ACT_StickL, "/user/hand/left/input/thumbstick"}, {ACT_StickR, "/user/hand/right/input/thumbstick"},
        {ACT_ClickL, "/user/hand/left/input/thumbstick/click"}, {ACT_ClickR, "/user/hand/right/input/thumbstick/click"},
        {ACT_Select, "/user/hand/left/input/menu/click"}, {ACT_Start, "/user/hand/right/input/menu/click"},
        {ACT_HapticL, "/user/hand/left/output/haptic"}, {ACT_HapticR, "/user/hand/right/output/haptic"}});
    // HTC Vive wands: the trackpads as sticks, their clicks Cross / Square; the menu buttons Select / Start
    suggest("/interaction_profiles/htc/vive_controller", {
        {ACT_A, "/user/hand/right/input/trackpad/click"}, {ACT_X, "/user/hand/left/input/trackpad/click"},
        {ACT_GripL, "/user/hand/left/input/squeeze/click"}, {ACT_GripR, "/user/hand/right/input/squeeze/click"},
        {ACT_TrigL, "/user/hand/left/input/trigger/value"}, {ACT_TrigR, "/user/hand/right/input/trigger/value"},
        {ACT_StickL, "/user/hand/left/input/trackpad"}, {ACT_StickR, "/user/hand/right/input/trackpad"},
        {ACT_Select, "/user/hand/left/input/menu/click"}, {ACT_Start, "/user/hand/right/input/menu/click"},
        {ACT_HapticL, "/user/hand/left/output/haptic"}, {ACT_HapticR, "/user/hand/right/output/haptic"}});
    // the generic profile every runtime knows: confirm and Start
    suggest("/interaction_profiles/khr/simple_controller", {
        {ACT_A, "/user/hand/right/input/select/click"}, {ACT_X, "/user/hand/left/input/select/click"},
        {ACT_Start, "/user/hand/right/input/menu/click"}, {ACT_Select, "/user/hand/left/input/menu/click"},
        {ACT_HapticL, "/user/hand/left/output/haptic"}, {ACT_HapticR, "/user/hand/right/output/haptic"}});
    XrSessionActionSetsAttachInfo ai{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    ai.countActionSets = 1;
    ai.actionSets = &s_actionset;
    return FCheck(pxrAttachSessionActionSets(s_session, &ai), "xrAttachSessionActionSets");
}

void UpdateProfile()
{
    XrInteractionProfileState st{XR_TYPE_INTERACTION_PROFILE_STATE};
    std::string str;
    for (XrPath hand : {s_pathRight, s_pathLeft})
    {
        if (XR_FAILED(pxrGetCurrentInteractionProfile(s_session, hand, &st)) || st.interactionProfile == XR_NULL_PATH) continue;
        char ach[XR_MAX_PATH_LENGTH] = {};
        uint32_t c = 0;
        if (XR_SUCCEEDED(pxrPathToString(s_inst, st.interactionProfile, sizeof ach, &c, ach))) { str = ach; break; }
    }
    if (str != s_strProfile) Log("controllers: %s", str.empty() ? "none" : str.c_str());
    s_strProfile = str;
    s_fPad = !str.empty();
    // (the controllers with real Start / Select buttons use them; the others keep grip + stick click)
    auto has = [&](const char* sz) { return str.find(sz) != std::string::npos; };
    s_fRealStartSelect = has("microsoft/motion_controller") || has("htc/vive_controller") || has("frame");
    // the prompts' family (controllers' controller_family.h VR families)
    using cane::input::Family;
    s_familyPrompt = has("valve/index") ? Family::ValveIndex : has("htc/vive") ? Family::ViveWand
                   : has("frame") ? Family::SteamFrame : has("pico") || has("bytedance") ? Family::Pico : Family::MetaQuest;
}

bool BoolOf(int act)
{
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = s_aaction[size_t(act)];
    XrActionStateBoolean st{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(pxrGetActionStateBoolean(s_session, &gi, &st)) && st.isActive && st.currentState;
}
float FloatOf(int act)
{
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = s_aaction[size_t(act)];
    XrActionStateFloat st{XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(pxrGetActionStateFloat(s_session, &gi, &st)) && st.isActive ? st.currentState : 0.0f;
}
glm::vec2 Vec2Of(int act)
{
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO};
    gi.action = s_aaction[size_t(act)];
    XrActionStateVector2f st{XR_TYPE_ACTION_STATE_VECTOR2F};
    return XR_SUCCEEDED(pxrGetActionStateVector2f(s_session, &gi, &st)) && st.isActive ? glm::vec2(st.currentState.x, st.currentState.y) : glm::vec2(0.0f);
}

// the controllers into the pad (GLFW's layout, the game's PS2 names: A Cross, B Circle, X Square, Y Triangle, the
// grips L1 / R1, the triggers L2 / R2, the sticks' clicks L3 / R3; Start = right grip + right stick click (and
// Touch's menu button), Select = left grip + left stick click, except on controllers with real Start / Select)
void SyncActions()
{
    if (!s_actionset || !s_fRunning) return;
    XrActiveActionSet aas{s_actionset, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &aas;
    if (XR_FAILED(pxrSyncActions(s_session, &si))) return;
    GLFWgamepadstate p{};
    const float gGripL = FloatOf(ACT_GripL), gGripR = FloatOf(ACT_GripR);
    bool fClickL = BoolOf(ACT_ClickL), fClickR = BoolOf(ACT_ClickR);
    bool fGripL = gGripL > 0.5f, fGripR = gGripR > 0.5f;
    bool fStart = BoolOf(ACT_Start) || BoolOf(ACT_Menu), fSelect = BoolOf(ACT_Select);
    if (!s_fRealStartSelect)
    {
        // the combo holds from the click's press with the grip down until the click is released; meanwhile neither
        // the grip nor the click goes to the game
        if (!fClickR) s_afCombo[1] = false;
        else if (fGripR && !s_afCombo[1] && !(s_pad.buttons[GLFW_GAMEPAD_BUTTON_RIGHT_THUMB])) s_afCombo[1] = true;
        if (!fClickL) s_afCombo[0] = false;
        else if (fGripL && !s_afCombo[0] && !(s_pad.buttons[GLFW_GAMEPAD_BUTTON_LEFT_THUMB])) s_afCombo[0] = true;
        if (s_afCombo[1]) fStart = true, fClickR = false, fGripR = false;
        if (s_afCombo[0]) fSelect = true, fClickL = false, fGripL = false;
    }
    p.buttons[GLFW_GAMEPAD_BUTTON_A] = BoolOf(ACT_A);
    p.buttons[GLFW_GAMEPAD_BUTTON_B] = BoolOf(ACT_B);
    p.buttons[GLFW_GAMEPAD_BUTTON_X] = BoolOf(ACT_X);
    p.buttons[GLFW_GAMEPAD_BUTTON_Y] = BoolOf(ACT_Y);
    p.buttons[GLFW_GAMEPAD_BUTTON_LEFT_BUMPER] = fGripL;
    p.buttons[GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER] = fGripR;
    p.buttons[GLFW_GAMEPAD_BUTTON_BACK] = fSelect;
    p.buttons[GLFW_GAMEPAD_BUTTON_START] = fStart;
    p.buttons[GLFW_GAMEPAD_BUTTON_LEFT_THUMB] = fClickL;
    p.buttons[GLFW_GAMEPAD_BUTTON_RIGHT_THUMB] = fClickR;
    const glm::vec2 l = Vec2Of(ACT_StickL), r = Vec2Of(ACT_StickR);
    p.axes[GLFW_GAMEPAD_AXIS_LEFT_X] = l.x;
    p.axes[GLFW_GAMEPAD_AXIS_LEFT_Y] = -l.y;   // (GLFW: down is positive)
    p.axes[GLFW_GAMEPAD_AXIS_RIGHT_X] = r.x;
    p.axes[GLFW_GAMEPAD_AXIS_RIGHT_Y] = -r.y;
    p.axes[GLFW_GAMEPAD_AXIS_LEFT_TRIGGER] = FloatOf(ACT_TrigL) * 2.0f - 1.0f;
    p.axes[GLFW_GAMEPAD_AXIS_RIGHT_TRIGGER] = FloatOf(ACT_TrigR) * 2.0f - 1.0f;
    // a newly pressed button or a stick / trigger past half: the prompts show these controllers' icons
    bool fActive = false;
    for (int b = 0; b <= GLFW_GAMEPAD_BUTTON_LAST; ++b) fActive |= p.buttons[b] && !s_pad.buttons[b];
    for (int a = 0; a < 4; ++a) fActive |= std::fabs(p.axes[a]) > 0.5f && std::fabs(s_pad.axes[a]) <= 0.5f;
    for (int a = 4; a < 6; ++a) fActive |= p.axes[a] > 0.0f && s_pad.axes[a] <= 0.0f;
    if (fActive || (!s_fPadNoted && s_fPad)) cane::input::tracker().noteVr(s_familyPrompt), s_fPadNoted = true;
    s_pad = p;
}

// ------------------------------------------------------------ the session
void SetRefreshRate()
{
    if (!s_fExtRefresh || !pxrEnumerateDisplayRefreshRatesFB || !pxrRequestDisplayRefreshRateFB) return;
    uint32_t c = 0;
    pxrEnumerateDisplayRefreshRatesFB(s_session, 0, &c, nullptr);
    std::vector<float> a(c);
    pxrEnumerateDisplayRefreshRatesFB(s_session, c, &c, a.data());
    std::string str;
    for (float hz : a) str += std::to_string(int(hz)) + " ";
    float hzWant = 0.0f;
    const int want = std::getenv("SLY2N_VR_HZ") ? std::atoi(std::getenv("SLY2N_VR_HZ")) : g_vropts.refreshRate;
    for (float hz : a)
        if (want > 0 ? std::fabs(hz - float(want)) < 1.0f : (hz <= 120.5f && hz > hzWant)) hzWant = hz;
    if (hzWant > 0.0f && FCheck(pxrRequestDisplayRefreshRateFB(s_session, hzWant), "xrRequestDisplayRefreshRateFB")) s_hzDisplay = hzWant;
    Log("refresh rates: %s-> %.0f Hz", str.c_str(), double(s_hzDisplay));
}

// (XR_KHR_android_thread_settings) the game's threads told to the runtime: the game thread is the application's and the
// renderer's main thread (it draws and submits the frames); helpers note themselves (NoteThread)
std::mutex s_mutexThreads;
std::vector<std::pair<int, int>> s_athreadNoted;   // (kind: XrAndroidThreadTypeKHR, tid)
int s_tidMain = 0;
void SetThread(int type, int tid)
{
#if defined(__ANDROID__)
    if (!s_fExtThreads || !pxrSetAndroidApplicationThreadKHR || s_session == XR_NULL_HANDLE || tid <= 0) return;
    const XrResult r = pxrSetAndroidApplicationThreadKHR(s_session, XrAndroidThreadTypeKHR(type), uint32_t(tid));
    if (XR_FAILED(r)) Log("xrSetAndroidApplicationThreadKHR(%d, %d) failed: %d", type, tid, int(r));
#else
    (void)type, (void)tid;
#endif
}
void RegisterThreads()
{
#if defined(__ANDROID__)
    if (!s_fExtThreads) return;
    s_tidMain = int(gettid());
    SetThread(XR_ANDROID_THREAD_TYPE_APPLICATION_MAIN_KHR, s_tidMain);
    SetThread(XR_ANDROID_THREAD_TYPE_RENDERER_MAIN_KHR, s_tidMain);
    std::lock_guard<std::mutex> lock(s_mutexThreads);
    for (const auto& [type, tid] : s_athreadNoted) SetThread(type, tid);
    Log("threads: the game thread %d as the main and renderer thread, %d helpers", s_tidMain, int(s_athreadNoted.size()));
#endif
}

void BeginSessionNow()
{
    XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    if (!FCheck(pxrBeginSession(s_session, &bi), "xrBeginSession")) return;
    s_fRunning = true;
    SetRefreshRate();
    if (s_fExtPerf && pxrPerfSettingsSetPerformanceLevelEXT)
    {
        pxrPerfSettingsSetPerformanceLevelEXT(s_session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
        pxrPerfSettingsSetPerformanceLevelEXT(s_session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
    }
    RegisterThreads();
    s_fAnchored = false;
}

void PollEvents()
{
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (pxrPollEvent(s_inst, &ev) == XR_SUCCESS)
    {
        switch (ev.type)
        {
        case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
        {
            const auto& e = *reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            s_state = e.state;
            Log("session state %d", int(s_state));
            if (s_state == XR_SESSION_STATE_READY) BeginSessionNow();
            else if (s_state == XR_SESSION_STATE_STOPPING) { pxrEndSession(s_session); s_fRunning = false; }
            else if (s_state == XR_SESSION_STATE_EXITING || s_state == XR_SESSION_STATE_LOSS_PENDING) s_fExit = true;
            break;
        }
        case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING: s_fExit = true; break;
        case XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED: UpdateProfile(); break;
        case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING: s_fAnchored = false; break;
        case XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB:
            s_hzDisplay = reinterpret_cast<XrEventDataDisplayRefreshRateChangedFB*>(&ev)->toDisplayRefreshRate;
            Log("refresh rate now %.0f Hz", double(s_hzDisplay));
            break;
        default: break;
        }
        ev = XrEventDataBuffer{XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// ------------------------------------------------------------ the graphics binding
#if !defined(__ANDROID__) && !defined(_WIN32)
void* SymGl(const char* name)
{
    static void* s_hGlx = [] {
        void* h = dlopen("libGLX.so.0", RTLD_LAZY | RTLD_NOLOAD);
        if (!h) h = dlopen("libGL.so.1", RTLD_LAZY | RTLD_NOLOAD);
        if (!h) h = dlopen("libGL.so.1", RTLD_LAZY);
        return h;
    }();
    return s_hGlx ? dlsym(s_hGlx, name) : nullptr;
}
void* SymEgl(const char* name)
{
    static void* s_hEgl = [] {
        void* h = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_NOLOAD);
        return h;
    }();
    return s_hEgl ? dlsym(s_hEgl, name) : nullptr;
}
#endif

bool FCreateSession()
{
#if CANE_VULKAN
    if (s_fVk)
    {
        // (the Vulkan renderer's instance / device, made through the runtime: CreateVulkanInstance / CreateVulkanDevice)
        const CaneVk::Handles h = CaneVk::DeviceHandles();
        XrGraphicsBindingVulkan2KHR gbv{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
        gbv.instance = h.instance;
        gbv.physicalDevice = h.physicalDevice;
        gbv.device = h.device;
        gbv.queueFamilyIndex = h.queueFamily;
        gbv.queueIndex = h.queueIndex;
        XrSessionCreateInfo sciv{XR_TYPE_SESSION_CREATE_INFO};
        sciv.next = &gbv;
        sciv.systemId = s_sys;
        return FCheck(pxrCreateSession(s_inst, &sciv, &s_session), "xrCreateSession (Vulkan)");
    }
#endif
#if defined(__ANDROID__)
    XrGraphicsRequirementsOpenGLESKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    if (!pxrGetOpenGLESGraphicsRequirementsKHR || !FCheck(pxrGetOpenGLESGraphicsRequirementsKHR(s_inst, s_sys, &req), "graphics requirements")) return false;
    XrGraphicsBindingOpenGLESAndroidKHR gb{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    gb.display = eglGetCurrentDisplay();
    gb.context = eglGetCurrentContext();
    EGLint id = 0, c = 0;
    eglQueryContext(gb.display, gb.context, EGL_CONFIG_ID, &id);
    const EGLint attr[] = {EGL_CONFIG_ID, id, EGL_NONE};
    EGLConfig cfg = nullptr;
    eglChooseConfig(gb.display, attr, &cfg, 1, &c);
    gb.config = cfg;
    const void* pBinding = &gb;
#elif defined(_WIN32)
    XrGraphicsRequirementsOpenGLKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
    if (!pxrGetOpenGLGraphicsRequirementsKHR || !FCheck(pxrGetOpenGLGraphicsRequirementsKHR(s_inst, s_sys, &req), "graphics requirements")) return false;
    XrGraphicsBindingOpenGLWin32KHR gb{XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
    // (opengl32's own functions, looked up: the game does not link it)
    HMODULE hgl = GetModuleHandleA("opengl32.dll");
    if (!hgl) hgl = LoadLibraryA("opengl32.dll");
    using PFNDC = HDC(WINAPI*)();
    using PFNRC = HGLRC(WINAPI*)();
    auto getDc = hgl ? reinterpret_cast<PFNDC>(reinterpret_cast<void*>(GetProcAddress(hgl, "wglGetCurrentDC"))) : nullptr;
    auto getRc = hgl ? reinterpret_cast<PFNRC>(reinterpret_cast<void*>(GetProcAddress(hgl, "wglGetCurrentContext"))) : nullptr;
    if (!getDc || !getRc) return false;
    gb.hDC = getDc();
    gb.hGLRC = getRc();
    const void* pBinding = &gb;
#else
    XrGraphicsRequirementsOpenGLKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_KHR};
    if (!pxrGetOpenGLGraphicsRequirementsKHR || !FCheck(pxrGetOpenGLGraphicsRequirementsKHR(s_inst, s_sys, &req), "graphics requirements")) return false;
    XrGraphicsBindingOpenGLXlibKHR gbx{XR_TYPE_GRAPHICS_BINDING_OPENGL_XLIB_KHR};
    XrGraphicsBindingEGLMNDX gbe{XR_TYPE_GRAPHICS_BINDING_EGL_MNDX};
    const void* pBinding = nullptr;
    using PFNGETCTX = void* (*)();
    auto glxGetCurrentContext = reinterpret_cast<PFNGETCTX>(SymGl("glXGetCurrentContext"));
    auto eglGetCurrentContextP = reinterpret_cast<PFNGETCTX>(SymEgl("eglGetCurrentContext"));
    if (glxGetCurrentContext && glxGetCurrentContext())
    {
        using PFNGETDPY = Display* (*)();
        using PFNGETDRAW = GLXDrawable (*)();
        using PFNQUERYCTX = int (*)(Display*, GLXContext, int, int*);
        using PFNCHOOSEFB = GLXFBConfig* (*)(Display*, int, const int*, int*);
        using PFNFREE = void (*)(void*);
        gbx.xDisplay = reinterpret_cast<PFNGETDPY>(SymGl("glXGetCurrentDisplay"))();
        gbx.glxContext = static_cast<GLXContext>(glxGetCurrentContext());
        gbx.glxDrawable = reinterpret_cast<PFNGETDRAW>(SymGl("glXGetCurrentDrawable"))();
        int id = 0, c = 0, screen = 0;
        constexpr int GLX_FBCONFIG_ID_ = 0x8013, GLX_SCREEN_ = 0x800C;
        auto query = reinterpret_cast<PFNQUERYCTX>(SymGl("glXQueryContext"));
        if (query)
        {
            query(gbx.xDisplay, gbx.glxContext, GLX_FBCONFIG_ID_, &id);
            query(gbx.xDisplay, gbx.glxContext, GLX_SCREEN_, &screen);
        }
        const int attr[] = {GLX_FBCONFIG_ID_, id, 0};
        if (auto choose = reinterpret_cast<PFNCHOOSEFB>(SymGl("glXChooseFBConfig")))
            if (GLXFBConfig* a = choose(gbx.xDisplay, screen, attr, &c))
            {
                if (c > 0) gbx.glxFBConfig = a[0];
                if (auto xfree = reinterpret_cast<PFNFREE>(dlsym(RTLD_DEFAULT, "XFree"))) xfree(a);
            }
        pBinding = &gbx;
    }
    else if (eglGetCurrentContextP && eglGetCurrentContextP() && s_fExtEglMndx)
    {
        using PFNGETDPY = void* (*)();
        using PFNQUERY = unsigned (*)(void*, void*, int, int*);
        using PFNCHOOSE = unsigned (*)(void*, const int*, void**, int, int*);
        gbe.getProcAddress = reinterpret_cast<PFN_xrEglGetProcAddressMNDX>(SymEgl("eglGetProcAddress"));
        gbe.display = reinterpret_cast<PFNGETDPY>(SymEgl("eglGetCurrentDisplay"))();
        gbe.context = eglGetCurrentContextP();
        int id = 0, c = 0;
        constexpr int EGL_CONFIG_ID_ = 0x3028, EGL_NONE_ = 0x3038;
        reinterpret_cast<PFNQUERY>(SymEgl("eglQueryContext"))(gbe.display, gbe.context, EGL_CONFIG_ID_, &id);
        const int attr[] = {EGL_CONFIG_ID_, id, EGL_NONE_};
        void* cfg = nullptr;
        reinterpret_cast<PFNCHOOSE>(SymEgl("eglChooseConfig"))(gbe.display, attr, &cfg, 1, &c);
        gbe.config = cfg;
        pBinding = &gbe;
    }
    if (!pBinding) { Log("no current GLX context (nor EGL with XR_MNDX_egl_enable)"); return false; }
#endif
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = pBinding;
    sci.systemId = s_sys;
    return FCheck(pxrCreateSession(s_inst, &sci, &s_session), "xrCreateSession");
}
}  // namespace

// ------------------------------------------------------------ public
void Request(bool f) { s_fRequested = f; }
bool FRequested() { return s_fRequested; }
bool FRuntimeAvailable()
{
    static int s_iAvailable = -1;
    if (s_iAvailable >= 0) return s_iAvailable != 0;
    s_iAvailable = 0;
    if (!FOpenLoader()) return false;
    PFN_xrEnumerateInstanceExtensionProperties pfnEnum = nullptr;
    GetProc(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", pfnEnum);
#if defined(__ANDROID__)
    {
        // (as FStart: the loader needs the VM and the activity before it can find the runtime)
        PFN_xrInitializeLoaderKHR pfnInit = nullptr;
        GetProc(XR_NULL_HANDLE, "xrInitializeLoaderKHR", pfnInit);
        JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
        JavaVM* vm = nullptr;
        if (env) env->GetJavaVM(&vm);
        static jobject s_activity = nullptr;
        if (!s_activity && env)
        {
            jobject local = static_cast<jobject>(SDL_AndroidGetActivity());
            s_activity = env->NewGlobalRef(local);
            env->DeleteLocalRef(local);
        }
        XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        li.applicationVM = vm;
        li.applicationContext = s_activity;
        if (!pfnInit || !vm || !s_activity || XR_FAILED(pfnInit(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR*>(&li)))) return false;
    }
#endif
    uint32_t c = 0;
    if (!pfnEnum || XR_FAILED(pfnEnum(nullptr, 0, &c, nullptr)) || c == 0) return false;
    s_iAvailable = 1;
    return true;
}
bool FActive() { return s_fStarted; }
bool FMenuShown() { return s_fStarted; }
bool FExitRequested() { return s_fExit; }
const std::string& StrInteractionProfile() { return s_strProfile; }

// The OpenXR instance and the headset (the renderer's graphics extension: Vulkan's or OpenGL's); false: no VR
bool FCreateInstance(bool fVulkan)
{
    s_fVk = fVulkan;
    s_aext.clear();
    if (!s_hLoader && !FOpenLoader()) { Log("no OpenXR loader: VR off"); return false; }
    GetProc(XR_NULL_HANDLE, "xrEnumerateInstanceExtensionProperties", pxrEnumerateInstanceExtensionProperties);
    GetProc(XR_NULL_HANDLE, "xrCreateInstance", pxrCreateInstance);
#if defined(__ANDROID__)
    {
        PFN_xrInitializeLoaderKHR pfnInit = nullptr;
        GetProc(XR_NULL_HANDLE, "xrInitializeLoaderKHR", pfnInit);
        JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
        JavaVM* vm = nullptr;
        if (env) env->GetJavaVM(&vm);
        static jobject s_activity = nullptr;
        if (!s_activity && env)
        {
            jobject local = static_cast<jobject>(SDL_AndroidGetActivity());
            s_activity = env->NewGlobalRef(local);
            env->DeleteLocalRef(local);
        }
        XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        li.applicationVM = vm;
        li.applicationContext = s_activity;
        if (!pfnInit || !vm || !s_activity || XR_FAILED(pfnInit(reinterpret_cast<XrLoaderInitInfoBaseHeaderKHR*>(&li))))
        {
            Log("xrInitializeLoaderKHR failed: VR off");
            return false;
        }
    }
#endif
    if (!pxrEnumerateInstanceExtensionProperties || !pxrCreateInstance) { Log("the loader has no xrCreateInstance"); return false; }
    uint32_t c = 0;
    if (XR_FAILED(pxrEnumerateInstanceExtensionProperties(nullptr, 0, &c, nullptr))) { Log("no OpenXR runtime: VR off"); return false; }
    std::vector<XrExtensionProperties> aprop(c, {XR_TYPE_EXTENSION_PROPERTIES});
    pxrEnumerateInstanceExtensionProperties(nullptr, c, &c, aprop.data());
    for (const auto& p : aprop) s_aext.push_back(p.extensionName);
    if (std::getenv("SLY2N_VR_EXTS"))
        for (const std::string& str : s_aext) Log("runtime extension %s", str.c_str());
    std::vector<const char*> aextWant;
#if defined(__ANDROID__)
    const char* szGraphics = XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
#else
    const char* szGraphics = XR_KHR_OPENGL_ENABLE_EXTENSION_NAME;
#endif
#if CANE_VULKAN
    if (fVulkan) szGraphics = XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME;
#endif
    if (!FHasExt(szGraphics)) { Log("the runtime has no %s: VR off", szGraphics); return false; }
    aextWant.push_back(szGraphics);
#if defined(__ANDROID__)
    if (FHasExt(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME)) aextWant.push_back(XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME);
#endif
#if !defined(__ANDROID__) && !defined(_WIN32)
    if (!fVulkan && (s_fExtEglMndx = FHasExt(XR_MNDX_EGL_ENABLE_EXTENSION_NAME))) aextWant.push_back(XR_MNDX_EGL_ENABLE_EXTENSION_NAME);
#endif
    if ((s_fExtRefresh = FHasExt(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME))) aextWant.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
    if (FGles() && FHasExt(XR_FB_FOVEATION_EXTENSION_NAME) && FHasExt(XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME) &&
        FHasExt(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME))
    {
        s_fExtFoveation = true;
        aextWant.push_back(XR_FB_FOVEATION_EXTENSION_NAME);
        aextWant.push_back(XR_FB_FOVEATION_CONFIGURATION_EXTENSION_NAME);
        aextWant.push_back(XR_FB_SWAPCHAIN_UPDATE_STATE_EXTENSION_NAME);
    }
    if ((s_fExtPerf = FHasExt(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME))) aextWant.push_back(XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
    if ((s_fExtLayerSettings = FHasExt(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME))) aextWant.push_back(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME);
    if ((s_fExtAutoLayerFilter = s_fExtLayerSettings && FHasExt("XR_META_automatic_layer_filter"))) aextWant.push_back("XR_META_automatic_layer_filter");
    s_fExtVkSwapchainMeta = s_fVkDirect = false;
#if CANE_VULKAN
    if (fVulkan && FHasExt(XR_META_VULKAN_SWAPCHAIN_CREATE_INFO_EXTENSION_NAME) &&
        !(std::getenv("SLY2N_VR_VKDIRECT") && std::atoi(std::getenv("SLY2N_VR_VKDIRECT")) == 0))
    {
        s_fExtVkSwapchainMeta = s_fVkDirect = true;
        aextWant.push_back(XR_META_VULKAN_SWAPCHAIN_CREATE_INFO_EXTENSION_NAME);
    }
#endif
#if defined(__ANDROID__)
    if ((s_fExtThreads = FHasExt(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME))) aextWant.push_back(XR_KHR_ANDROID_THREAD_SETTINGS_EXTENSION_NAME);
#endif

    XrInstanceCreateInfo ici{XR_TYPE_INSTANCE_CREATE_INFO};
#if defined(__ANDROID__)
    XrInstanceCreateInfoAndroidKHR iciAndroid{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    {
        JNIEnv* env = static_cast<JNIEnv*>(SDL_AndroidGetJNIEnv());
        JavaVM* vm = nullptr;
        env->GetJavaVM(&vm);
        jobject local = static_cast<jobject>(SDL_AndroidGetActivity());
        static jobject s_activity2 = env->NewGlobalRef(local);
        env->DeleteLocalRef(local);
        iciAndroid.applicationVM = vm;
        iciAndroid.applicationActivity = s_activity2;
        ici.next = &iciAndroid;
    }
#endif
    std::strcpy(ici.applicationInfo.applicationName, "SafeHouse Sly 2");
    ici.applicationInfo.applicationVersion = 1;
    std::strcpy(ici.applicationInfo.engineName, "SafeHouse");
    ici.applicationInfo.engineVersion = 1;
    ici.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ici.enabledExtensionCount = uint32_t(aextWant.size());
    ici.enabledExtensionNames = aextWant.data();
    if (XR_FAILED(pxrCreateInstance(&ici, &s_inst))) { Log("xrCreateInstance failed (no runtime?): VR off"); s_inst = XR_NULL_HANDLE; return false; }
    LoadInstanceFunctions(s_inst);
    XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(pxrGetInstanceProperties(s_inst, &ip)))
    {
        char ach[160];
        std::snprintf(ach, sizeof ach, "%s %u.%u.%u", ip.runtimeName, unsigned(XR_VERSION_MAJOR(ip.runtimeVersion)),
                      unsigned(XR_VERSION_MINOR(ip.runtimeVersion)), unsigned(XR_VERSION_PATCH(ip.runtimeVersion)));
        s_strRuntime = ach;
    }
    XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (XR_FAILED(pxrGetSystem(s_inst, &sgi, &s_sys)))
    {
        Log("%s: no headset: VR off", s_strRuntime.c_str());
        pxrDestroyInstance(s_inst);
        s_inst = XR_NULL_HANDLE;
        return false;
    }
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    pxrGetSystemProperties(s_inst, s_sys, &sp);
    Log("runtime %s, headset %s", s_strRuntime.c_str(), sp.systemName);
    c = 0;
    pxrEnumerateViewConfigurationViews(s_inst, s_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &c, nullptr);
    s_aviewcfg.assign(c, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    pxrEnumerateViewConfigurationViews(s_inst, s_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, c, &c, s_aviewcfg.data());
    if (c != 2) { Log("not a stereo headset (%u views): VR off", c); Shutdown(); return false; }
#if CANE_VULKAN
    if (fVulkan)
    {
        // (required before the Vulkan instance: the runtime's Vulkan version range)
        XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
        if (!pxrGetVulkanGraphicsRequirements2KHR || !pxrCreateVulkanInstanceKHR || !pxrGetVulkanGraphicsDevice2KHR || !pxrCreateVulkanDeviceKHR ||
            !FCheck(pxrGetVulkanGraphicsRequirements2KHR(s_inst, s_sys, &req), "Vulkan graphics requirements"))
        {
            Shutdown();
            return false;
        }
        Log("the runtime's Vulkan: %u.%u to %u.%u", unsigned(XR_VERSION_MAJOR(req.minApiVersionSupported)), unsigned(XR_VERSION_MINOR(req.minApiVersionSupported)),
            unsigned(XR_VERSION_MAJOR(req.maxApiVersionSupported)), unsigned(XR_VERSION_MINOR(req.maxApiVersionSupported)));
    }
#endif
    return true;
}

bool FStart()
{
    if (!s_fRequested || s_fStarted) return s_fStarted;
#if CANE_VULKAN
    const bool fVulkan = FRendererVulkan();
#else
    const bool fVulkan = false;
#endif
    // (an instance made for the other renderer: made again)
    if (s_inst && s_fVk != fVulkan) Shutdown();
    if (!s_inst && !FCreateInstance(fVulkan)) return false;

#if defined(__ANDROID__)
    if (!s_fVk)
    {
    s_fGlesMsrtt = FGlExtension("GL_EXT_multisampled_render_to_texture");
    s_fSrgbWriteControl = FGlExtension("GL_EXT_sRGB_write_control");
    s_pfnFbTex2DMs = reinterpret_cast<PFNFBTEX2DMS>(eglGetProcAddress("glFramebufferTexture2DMultisampleEXT"));
    s_pfnRbStorageMs = reinterpret_cast<PFNRBSTORAGEMS>(eglGetProcAddress("glRenderbufferStorageMultisampleEXT"));
    if (!s_pfnFbTex2DMs || !s_pfnRbStorageMs) s_fGlesMsrtt = false;
    s_pfnFbTexMsMv = reinterpret_cast<PFNFBTEXMSMV>(eglGetProcAddress("glFramebufferTextureMultisampleMultiviewOVR"));
    s_fMultiviewExt = s_pfnFbTexMsMv && FGlExtension("GL_OVR_multiview2") && FGlExtension("GL_OVR_multiview_multisampled_render_to_texture");
    // (GLES encodes writes into sRGB images unless told not to: the game's colours are display-encoded already)
    if (s_fSrgbWriteControl) glDisable(kGlFramebufferSrgb);
    Log("GLES: multisampled render to texture %s, sRGB write control %s", s_fGlesMsrtt ? "yes" : "no", s_fSrgbWriteControl ? "yes" : "no");
    }
#endif
    if (!FCreateSession()) { Shutdown(); return false; }
    XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.poseInReferenceSpace.orientation.w = 1.0f;
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (!FCheck(pxrCreateReferenceSpace(s_session, &rsci, &s_spaceLocal), "local space")) { Shutdown(); return false; }
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    FCheck(pxrCreateReferenceSpace(s_session, &rsci, &s_spaceView), "view space");
    InitGlPrograms();
    if (!FCreateEyeTargets()) { Shutdown(); return false; }
    if (!FCreateActions()) Log("controllers: no actions");
    s_fStarted = true;
    // the game picture's size in VR: a 16:9 screen (display.cpp LayoutDisplay asks FPanelSize)
    LayoutDisplay(1920, 1080);
    cane::gl::SwapInterval(0);   // (the headset paces the frames; the desktop window only mirrors)
    // the session starts when the runtime says it is ready
    for (int i = 0; i < 50 && !s_fRunning && !s_fExit; ++i)
    {
        PollEvents();
        if (!s_fRunning) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Log("started (%s)", s_fRunning ? "running" : "waiting for the headset");
    return true;
}

void Shutdown()
{
    WaitVkSubmissions();
    s_fStarted = false;
    if (s_session)
    {
        for (SWAPCHAIN& sc : s_aeye) DestroySwapchain(sc);
        DestroySwapchain(s_panel);
        if (s_spaceLocal) pxrDestroySpace(s_spaceLocal);
        if (s_spaceView) pxrDestroySpace(s_spaceView);
        s_spaceLocal = s_spaceView = XR_NULL_HANDLE;
        pxrDestroySession(s_session);
        s_session = XR_NULL_HANDLE;
    }
    if (s_inst) pxrDestroyInstance(s_inst);
    s_inst = XR_NULL_HANDLE;
    s_fStarted = s_fRunning = false;
}

#if CANE_VULKAN
namespace
{
VkResult XrVkCreateInstance(void*, PFN_vkGetInstanceProcAddr getProc, const VkInstanceCreateInfo* ci, VkInstance* out)
{
    XrVulkanInstanceCreateInfoKHR xi{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    xi.systemId = s_sys;
    xi.pfnGetInstanceProcAddr = getProc;
    xi.vulkanCreateInfo = ci;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (!FCheck(pxrCreateVulkanInstanceKHR(s_inst, &xi, out, &r), "xrCreateVulkanInstanceKHR")) return VK_ERROR_INITIALIZATION_FAILED;
    return r;
}
VkPhysicalDevice XrVkPhysicalDevice(void*, VkInstance instance)
{
    XrVulkanGraphicsDeviceGetInfoKHR gi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    gi.systemId = s_sys;
    gi.vulkanInstance = instance;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    FCheck(pxrGetVulkanGraphicsDevice2KHR(s_inst, &gi, &pd), "xrGetVulkanGraphicsDevice2KHR");
    return pd;
}
VkResult XrVkCreateDevice(void*, PFN_vkGetInstanceProcAddr getProc, VkPhysicalDevice pd, const VkDeviceCreateInfo* ci, VkDevice* out)
{
    XrVulkanDeviceCreateInfoKHR di{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    di.systemId = s_sys;
    di.pfnGetInstanceProcAddr = getProc;
    di.vulkanPhysicalDevice = pd;
    di.vulkanCreateInfo = ci;
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (!FCheck(pxrCreateVulkanDeviceKHR(s_inst, &di, out, &r), "xrCreateVulkanDeviceKHR")) return VK_ERROR_INITIALIZATION_FAILED;
    return r;
}
}  // namespace

const void* PvulkanDevice()
{
    static CaneVk::ExternalDevice s_ext;
    if (!s_fRequested) return nullptr;
    if (!s_inst && !FCreateInstance(true)) { Log("the runtime offers no Vulkan path"); return nullptr; }
    if (!s_fVk) return nullptr;
    s_ext.createInstance = XrVkCreateInstance;
    s_ext.physicalDevice = XrVkPhysicalDevice;
    s_ext.createDevice = XrVkCreateDevice;
    return &s_ext;
}

void VulkanFailed()
{
    // (the Vulkan renderer did not start: the instance is made again for OpenGL at FStart)
    if (s_fVk && s_inst) Shutdown();
    s_fVk = false;
}
#endif

void NoteThread(VRTHREAD kind)
{
#if defined(__ANDROID__)
    const int type = kind == VRTHREAD_RendererWorker ? XR_ANDROID_THREAD_TYPE_RENDERER_WORKER_KHR : XR_ANDROID_THREAD_TYPE_APPLICATION_WORKER_KHR;
    const int tid = int(gettid());
    {
        std::lock_guard<std::mutex> lock(s_mutexThreads);
        s_athreadNoted.push_back({type, tid});
    }
    if (s_fRunning) SetThread(type, tid);
#else
    (void)kind;
#endif
}

bool FPanelSize(int* pdx, int* pdy)
{
    if (!s_fStarted) return false;
    *pdx = 1920;
    *pdy = 1080;
    return true;
}

void Recenter() { s_fAnchored = false; }
void ApplyRefreshRate()
{
    if (s_fStarted && s_fRunning) SetRefreshRate();
}

bool FAutoCameraRotation(bool fFlatOption)
{
    return s_fStarted && g_vropts.mode == VRMODE_Immersive ? g_vropts.fAutoCameraRotation : fFlatOption;
}

// (test, SLY2N_VR_FIXEDPOSE=1: the head still (below) and the comfort vignette's clock a 20th of a second a frame)
bool FCompositorUpscaling() { return s_fStarted && s_fExtLayerSettings; }
bool FFixedPose()
{
    static const bool s_f = std::getenv("SLY2N_VR_FIXEDPOSE") && std::atoi(std::getenv("SLY2N_VR_FIXEDPOSE")) != 0;
    return s_f;
}
void BeginFrame()
{
    if (!s_fStarted || s_frame.fBegun) return;
    WaitVkSubmissions();   // (the last frame's xrEndFrame done: the session's calls below in their order)
    PollEvents();
    s_frame.fEyesDrawn = false;
    s_frame.fImmersive = false;
    if (!s_fRunning) return;
    s_frame.state = XrFrameState{XR_TYPE_FRAME_STATE};
    XrFrameWaitInfo wi{XR_TYPE_FRAME_WAIT_INFO};
    if (!FCheck(pxrWaitFrame(s_session, &wi, &s_frame.state), "xrWaitFrame")) return;
    UpdateEyeResolution(s_fEyesLastFrame);
    XrFrameBeginInfo bi{XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(pxrBeginFrame(s_session, &bi))) return;
    s_frame.fBegun = true;
    SyncActions();
    XrViewLocateInfo li{XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = s_frame.state.predictedDisplayTime;
    li.space = s_spaceLocal;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    uint32_t c = 0;
    s_frame.aview[0] = s_frame.aview[1] = XrView{XR_TYPE_VIEW};
    s_frame.fViews = XR_SUCCEEDED(pxrLocateViews(s_session, &li, &vs, 2, &c, s_frame.aview)) && c == 2 &&
                     (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT) && (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT);
    // (test, SLY2N_VR_FIXEDPOSE=1: the head still, level, 1.6 m up, the eyes 64 mm apart, the runtime's fields of view:
    // frames that repeat run to run with --fixed-dt, for picture comparisons)
    if (s_frame.fViews && FFixedPose())
        for (int i = 0; i < 2; ++i)
        {
            s_frame.aview[i].pose.orientation = XrQuaternionf{0.0f, 0.0f, 0.0f, 1.0f};
            s_frame.aview[i].pose.position = XrVector3f{i ? 0.032f : -0.032f, 1.6f, 0.0f};
        }
    if (s_frame.fViews && !s_fAnchored)
    {
        // the anchor: the head (between the eyes) and its heading now
        const glm::vec3 pos = (Vec(s_frame.aview[0].pose.position) + Vec(s_frame.aview[1].pose.position)) * 0.5f;
        s_posAnchor = pos;
        s_yawAnchor = YawOf(Quat(s_frame.aview[0].pose.orientation));
        s_yawPanel = s_yawAnchor;
        s_fAnchored = true;
    }
    s_frame.fImmersive = s_frame.state.shouldRender && s_frame.fViews && g_vropts.mode == VRMODE_Immersive;
    if (s_frame.fViews)
    {
        const glm::quat q = glm::angleAxis(-s_yawAnchor, glm::vec3(0, 1, 0)) * Quat(s_frame.aview[0].pose.orientation);
        s_yawHead = YawOf(q);
        s_pitchHead = std::asin(std::clamp((q * glm::vec3(0, 0, -1)).y, -1.0f, 1.0f));
    }
}

void SetGameView(int view)
{
    s_viewPrev = s_view;
    s_view = view;
    if (view == s_viewPrev) return;
    static const char* const s_asz[] = {"normal", "flat (cutscene / dance / RC chopper)", "binocucom", "RC table"};
    Log("view: %s", s_asz[std::clamp(view, 0, 3)]);
    // (entering the binocucom: the head's turn so far is not the game's; entering the RC table: re-anchored there)
    if (view == VRVIEW_Binoc)
    {
        s_yawConsumed = s_yawHead, s_pitchConsumed = s_pitchHead;
        s_posHeadBinoc = s_frame.fViews ? (Vec(s_frame.aview[0].pose.position) + Vec(s_frame.aview[1].pose.position)) * 0.5f : s_posAnchor;
    }
    s_fAim = false;
}

bool FHeadLook(float* pdradPan, float* pdradTilt)
{
    *pdradPan = *pdradTilt = 0.0f;
    if (!s_fStarted || s_view != VRVIEW_Binoc || g_vropts.mode != VRMODE_Immersive || !s_frame.fViews) return false;
    float d = s_yawHead - s_yawConsumed;
    while (d > kPi) d -= 2.0f * kPi;
    while (d < -kPi) d += 2.0f * kPi;
    *pdradPan = d;
    *pdradTilt = s_pitchHead - s_pitchConsumed;
    s_yawConsumed = s_yawHead, s_pitchConsumed = s_pitchHead;
    return true;
}

bool FRcTableActive() { return s_fStarted && s_view == VRVIEW_RcTable && g_vropts.mode == VRMODE_Immersive; }
bool FRcAimPoint(glm::vec3* ppos)
{
    if (!FRcTableActive() || !s_fAim) return false;
    *ppos = s_posAim;
    return true;
}
void SetRcArenaHeight(float z) { s_zRcArena = z; }

bool FImmersiveFrame() { return s_frame.fBegun && s_frame.fImmersive && s_view != VRVIEW_Flat; }

bool FEyeViews(const CAMERA& cam, STEREOVIEW* psv)
{
    if (!FImmersiveFrame()) return false;
    if (s_fMultiview && g_fMultiviewFailed && !s_fVk)
    {
        // (a program's two-view variant could not be made: the eyes one by one from now on)
        Log("multiview off: the eyes one by one");
        DestroyEyeTargets();
        if (!FCreateEyeTargets()) return false;
    }
#if CANE_VULKAN
    // (Vulkan: VR Settings' Multiview changed: the targets made again for it)
    if (s_fVk && FMultiviewWanted() != s_fMultiview && (s_fMultiview || CaneVk::MultiviewSupported(kVkEyesGeometryStage)))
    {
        Log("multiview %s", s_fMultiview ? "off: the eyes one by one" : "on: both eyes in one pass");
        DestroyEyeTargets();
        if (!FCreateEyeTargets()) return false;
    }
#endif
    for (int i = 0; i < (s_fMultiview ? 1 : 2); ++i)
        if (!FAcquire(s_aeye[i]))
        {
            if (i) Release(s_aeye[0]);
            return false;
        }
    // the base: the game camera's position, its heading only (level: the head does the pitch and the roll)
    glm::vec3 f = cam.Dir();
    glm::vec2 f2(f.x, f.y);
    static glm::vec2 s_f2Last(1.0f, 0.0f);
    if (glm::length(f2) > 1e-3f) s_f2Last = glm::normalize(f2);
    f2 = s_f2Last;
    glm::vec3 posBase = cam.pos;
    // (the RC table: the view where the match started, its heading, kept)
    if (s_view == VRVIEW_RcTable)
    {
        if (s_viewPrev != VRVIEW_RcTable) { s_posRc = cam.pos; s_yawRc = std::atan2(f2.y, f2.x); s_fAnchored = false; }
        posBase = s_posRc;
        f2 = glm::vec2(std::cos(s_yawRc), std::sin(s_yawRc));
    }
    const glm::vec3 fwd(f2, 0.0f), up(0, 0, 1), right = glm::cross(fwd, up);
    // XR (x right, y up, -z forward) -> game (z up)
    glm::mat3 B(right, up, -fwd);
    const float s = 100.0f * std::clamp(g_vropts.worldScale, 0.1f, 10.0f);   // game units a metre
    // the head relative to the anchor (its heading undone)
    glm::quat qAnchorInv = glm::angleAxis(-s_yawAnchor, glm::vec3(0, 1, 0));
    glm::vec3 posHeadRef = s_posAnchor;
    // (the binocucom: the game camera's whole orientation, which the head's turn drives (FHeadLook); only the head's
    // turn not yet given to the game on top, and its movement since the binocucom opened)
    if (s_view == VRVIEW_Binoc)
    {
        const glm::vec3 fwdB = glm::normalize(f), leftB = cam.fMat ? glm::normalize(cam.mat[1]) : glm::normalize(glm::cross(glm::vec3(0, 0, 1), fwdB));
        const glm::vec3 upB = glm::normalize(glm::cross(fwdB, leftB));
        B = glm::mat3(-leftB, upB, -fwdB);
        qAnchorInv = glm::inverse(glm::angleAxis(s_yawAnchor + s_yawConsumed, glm::vec3(0, 1, 0)) * glm::angleAxis(s_pitchConsumed, glm::vec3(1, 0, 0)));
        posHeadRef = s_posHeadBinoc;
    }
    glm::vec3 aposEye[2];
    glm::mat3 arot[2];
    for (int i = 0; i < 2; ++i)
    {
        const XrView& v = s_frame.aview[i];
        const glm::vec3 p = glm::angleAxis(-s_yawAnchor, glm::vec3(0, 1, 0)) * (Vec(v.pose.position) - posHeadRef);
        const glm::quat q = qAnchorInv * Quat(v.pose.orientation);
        // (the binocucom: the head's movement along the camera's own heading)
        aposEye[i] = posBase + (s_view == VRVIEW_Binoc ? glm::mat3(right, up, -fwd) : B) * (p * s);
        arot[i] = B * glm::mat3_cast(q);
        glm::mat4 eye(1.0f);
        eye[0] = glm::vec4(arot[i][0], 0.0f);
        eye[1] = glm::vec4(arot[i][1], 0.0f);
        eye[2] = glm::vec4(arot[i][2], 0.0f);
        eye[3] = glm::vec4(aposEye[i], 1.0f);
        psv->amatWorldToClip[i] = ProjOfFov(v.fov, cam.sNear) * glm::inverse(eye);
        // (Vulkan, straight into the OpenXR images: the image is read top row first, the renderer's rows are GL's: the
        // eye drawn with its clip space's Y flipped (render.cpp turns the winding with it); Meta's compositor does not
        // flip projection layers (XR_FB_composition_layer_image_layout))
        if (s_fVkDirect) psv->amatWorldToClip[i] = glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, -1.0f, 1.0f)) * psv->amatWorldToClip[i];
        s_amatEye[i] = psv->amatWorldToClip[i];
        psv->apos[i] = aposEye[i];
        psv->afbo[i] = s_fMultiview ? (s_afboMsaa[0] ? s_afboMsaa[0] : FboEyeImage(0)) : s_afboMsaa[i] ? s_afboMsaa[i] : FboEyeImage(i);
    }
    // the culling frustum: both eyes' fields from between them, a little behind
    XrFovf fov;
    fov.angleLeft = std::min(s_frame.aview[0].fov.angleLeft, s_frame.aview[1].fov.angleLeft);
    fov.angleRight = std::max(s_frame.aview[0].fov.angleRight, s_frame.aview[1].fov.angleRight);
    fov.angleUp = std::max(s_frame.aview[0].fov.angleUp, s_frame.aview[1].fov.angleUp);
    fov.angleDown = std::min(s_frame.aview[0].fov.angleDown, s_frame.aview[1].fov.angleDown);
    const glm::vec3 posCenter = (aposEye[0] + aposEye[1]) * 0.5f;
    const float sBack = glm::distance(aposEye[0], aposEye[1]) * 1.5f + 1.0f;
    glm::mat4 eyeC(1.0f);
    eyeC[0] = glm::vec4(arot[0][0], 0.0f);
    eyeC[1] = glm::vec4(arot[0][1], 0.0f);
    eyeC[2] = glm::vec4(arot[0][2], 0.0f);
    eyeC[3] = glm::vec4(posCenter + arot[0][2] * sBack, 1.0f);
    psv->matCull = ProjOfFov(fov, cam.sNear) * glm::inverse(eyeC);
    psv->posCenter = posCenter;
    // (the RC table: the gaze from between the eyes onto the arena's plane)
    if (s_view == VRVIEW_RcTable)
    {
        const glm::vec3 dir = -arot[0][2];
        s_fAim = dir.z < -0.05f && posCenter.z > s_zRcArena;
        if (s_fAim) s_posAim = posCenter + dir * ((s_zRcArena - posCenter.z) / dir.z);
    }
    psv->dirCenter = -arot[0][2];
    s_dxEye = std::max(16, int(float(s_aeye[0].dx) * s_gEyeRes) & ~7);
    s_dyEye = std::max(16, int(float(s_aeye[0].dy) * s_gEyeRes) & ~7);
    if (s_gEyeRes >= 1.0f) s_dxEye = s_aeye[0].dx, s_dyEye = s_aeye[0].dy;
    psv->dx = s_dxEye;
    psv->dy = s_dyEye;
    ApplyFoveation();   // (a new level, or the density maps over a new eye resolution)
    psv->fMultiview = s_fMultiview;
    psv->fFlipY = s_fVkDirect;
    psv->texMultiviewColor = !s_fMultiview ? 0 : s_atexEyeVk[0] ? s_atexEyeVk[0] : s_aeye[0].atex[s_aeye[0].iimage];
    // the vignette's strength from the base camera's turn rate and speed (Immersive comfort)
    static int s_cframeFixed = 0;
    const double t = FFixedPose() ? ++s_cframeFixed / 20.0 : glfwGetTime();
    const float yaw = std::atan2(f2.y, f2.x);
    if (s_tCamPrev >= 0.0 && t > s_tCamPrev)
    {
        const float dt = float(t - s_tCamPrev);
        float dyaw = yaw - s_yawCamPrev;
        while (dyaw > kPi) dyaw -= 2.0f * kPi;
        while (dyaw < -kPi) dyaw += 2.0f * kPi;
        const float degPerS = std::fabs(glm::degrees(dyaw)) / dt;
        const float mPerS = glm::distance(cam.pos, s_posCamPrev) / s / dt;
        // a jump cut (a new shot) is not motion
        const float target = mPerS > 30.0f ? 0.0f : std::clamp(std::max(degPerS / 120.0f, mPerS / 6.0f), 0.0f, 1.0f);
        const float k = target > s_gVignette ? std::min(1.0f, dt * 8.0f) : std::min(1.0f, dt * 2.5f);
        s_gVignette += (target - s_gVignette) * k;
    }
    s_tCamPrev = t;
    s_yawCamPrev = yaw;
    s_posCamPrev = cam.pos;
    return true;
}

// (Vulkan multiview: the two-view programs' per-eye values, CaneViews at storage binding 11 (cmake/vulkan_shaders.py
// MV_BLOCK): the eyes' matrices (the aim mark) and per-program values (the vignette's centre and scale))
void BindVkViews(const glm::vec4 aaux[2])
{
    struct { glm::mat4 amat[2]; glm::vec4 apos[2], aaux[2]; glm::ivec4 fetch; } views{};
    for (int i = 0; i < 2; ++i) views.amat[i] = s_amatEye[i], views.aaux[i] = aaux[i];
    static GLuint s_buf = 0;
    if (!s_buf) glGenBuffers(1, &s_buf);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, s_buf);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(views), &views, GL_STREAM_DRAW);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 11, s_buf);
}
// (Vulkan multiview: a read framebuffer on one layer of the eyes' target (kind 0) or of the image (kind 1))
GLuint FboLayerVk(int kind, int layer, GLuint tex)
{
    GLuint& fbo = s_afboLayerVk[kind][layer];
    if (!fbo) glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo);
    glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0, layer);
    return fbo;
}

void FinishEyes()
{
    if (!s_aeye[0].fAcquired) return;
    const bool fViewsVk = s_fVk && s_fMultiview;   // (Vulkan multiview: both eyes at once, the two-view programs)
    static const float s_agLevel[4] = {0.0f, 0.55f, 0.8f, 1.0f};
    const float gVig = s_agLevel[std::clamp(g_vropts.vignette, 0, 3)] * s_gVignette;
    for (int i = 0; i < (s_fMultiview ? 1 : 2); ++i)
    {
        const GLuint fboDraw = s_afboMsaa[i] ? s_afboMsaa[i] : FboEyeImage(i);
        glBindFramebuffer(GL_FRAMEBUFFER, fboDraw);
        glViewport(0, 0, s_dxEye, s_dyEye);
        if (gVig > 0.01f)
        {
            const XrFovf& fov = s_frame.aview[i].fov;
            const float tl = std::tan(fov.angleLeft), tr = std::tan(fov.angleRight), tu = std::tan(fov.angleUp), td = std::tan(fov.angleDown);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_CULL_FACE);
            glDisable(GL_SCISSOR_TEST);
            glEnable(GL_BLEND);
            glBlendEquation(GL_FUNC_ADD);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
            if (fViewsVk)
            {
                glm::vec4 aaux[2];
                for (int k = 0; k < 2; ++k)
                {
                    const XrFovf& f = s_frame.aview[k].fov;
                    const float l = std::tan(f.angleLeft), r = std::tan(f.angleRight), u = std::tan(f.angleUp), d = std::tan(f.angleDown);
                    aaux[k] = glm::vec4(-(r + l) / (r - l), (s_fVkDirect ? 1.0f : -1.0f) * (u + d) / (u - d), (r - l) * 0.5f, (u - d) * 0.5f);
                }
                BindVkViews(aaux);
                s_shVignette.Use();
                glUniform1f(s_locVigStrength, gVig);
            }
            else if (s_fMultiview)
            {
                float acenter[4], ascale[4];
                for (int k = 0; k < 2; ++k)
                {
                    const XrFovf& f = s_frame.aview[k].fov;
                    const float l = std::tan(f.angleLeft), r = std::tan(f.angleRight), u = std::tan(f.angleUp), d = std::tan(f.angleDown);
                    acenter[2 * k] = -(r + l) / (r - l), acenter[2 * k + 1] = -(u + d) / (u - d);
                    ascale[2 * k] = (r - l) * 0.5f, ascale[2 * k + 1] = (u - d) * 0.5f;
                }
                glUseProgram(s_progVignetteMv);
                glUniform2fv(s_locVigMvCenter, 2, acenter);
                glUniform2fv(s_locVigMvScale, 2, ascale);
                glUniform1f(s_locVigMvStrength, gVig);
            }
            else
            {
                s_shVignette.Use();
                glUniform2f(s_locVigCenter, -(tr + tl) / (tr - tl), (s_fVkDirect ? 1.0f : -1.0f) * (tu + td) / (tu - td));
                glUniform2f(s_locVigScale, (tr - tl) * 0.5f, (tu - td) * 0.5f);
                glUniform1f(s_locVigStrength, gVig);
            }
            glBindVertexArray(s_vaoEmpty);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glBindVertexArray(0);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDisable(GL_BLEND);
        }
        // (the RC table: where the gun aims, a ring on the arena with a dot, over everything)
        if (s_fAim && s_view == VRVIEW_RcTable && (!s_fMultiview || fViewsVk))
        {
            // (Vulkan multiview: in world space, each view's matrix from CaneViews: vrmark.vert's two-view stage)
            const glm::mat4 matMark = fViewsVk ? glm::mat4(1.0f) : s_amatEye[i];
            if (fViewsVk)
            {
                const glm::vec4 aaux[2] = {glm::vec4(0.0f), glm::vec4(0.0f)};
                BindVkViews(aaux);
            }
            std::vector<glm::vec4> av;
            const int cseg = 40;
            for (int k = 0; k <= cseg; ++k)
            {
                const float a = 2.0f * kPi * float(k) / float(cseg);
                const glm::vec3 d(std::cos(a), std::sin(a), 0.0f);
                av.push_back(matMark * glm::vec4(s_posAim + d * 34.0f + glm::vec3(0, 0, 2), 1.0f));
                av.push_back(matMark * glm::vec4(s_posAim + d * 24.0f + glm::vec3(0, 0, 2), 1.0f));
            }
            const size_t cRing = av.size();
            for (int k = 0; k <= cseg; ++k)
            {
                const float a = 2.0f * kPi * float(k) / float(cseg);
                av.push_back(matMark * glm::vec4(s_posAim + glm::vec3(0, 0, 2), 1.0f));
                av.push_back(matMark * glm::vec4(s_posAim + glm::vec3(std::cos(a), std::sin(a), 0.0f) * 6.0f + glm::vec3(0, 0, 2), 1.0f));
            }
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_CULL_FACE);
            glDisable(GL_BLEND);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
            s_shMark.Use();
            glUniform4f(glGetUniformLocation(s_progMark, "rgba"), 1.0f, 0.78f, 0.1f, 1.0f);
            glBindVertexArray(s_vaoMark);
            glBindBuffer(GL_ARRAY_BUFFER, s_vboMark);
            glBufferData(GL_ARRAY_BUFFER, GLsizeiptr(av.size() * sizeof(glm::vec4)), av.data(), GL_STREAM_DRAW);
            glDrawArrays(GL_TRIANGLE_STRIP, 0, GLsizei(cRing));
            glDrawArrays(GL_TRIANGLE_STRIP, GLsizei(cRing), GLsizei(av.size() - cRing));
            glBindVertexArray(0);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        }
        if (s_afboMsaa[i])
        {
            // desktop MSAA: resolved into the image; Vulkan: copied in flipped (the image is read top row first)
            const GLuint fboImage = FboEyeImage(i);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, s_afboMsaa[i]);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fboImage);
            const int dx = s_dxEye, dy = s_dyEye;
            if (s_atexEyeVk[i])
            {
                // (the eye's part of its target, flipped into the same part of the image's rows)
                glBindFramebuffer(GL_FRAMEBUFFER, fboImage);
                glViewport(0, 0, dx, dy);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_STENCIL_TEST);
                glDisable(GL_BLEND);
                glDisable(GL_CULL_FACE);
                glDisable(GL_SCISSOR_TEST);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                // (multiview: both layers in one two-view draw into the image's two layers)
                const GLSHADER& sh = fViewsVk ? s_shCopyArray : s_shCopy;
                const GLenum targetTex = fViewsVk ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
                sh.Use();
                glUniform1i(sh.Loc("tex"), 0);
                glUniform1i(sh.Loc("fOpaque"), 1);
                glUniform1i(sh.Loc("fFlipY"), 1);
                glUniform2f(sh.Loc("uvScale"), float(dx) / float(s_aeye[i].dx), float(dy) / float(s_aeye[i].dy));
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(targetTex, s_atexEyeVk[i]);
                glBindVertexArray(s_vaoEmpty);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glBindVertexArray(0);
                glBindTexture(targetTex, 0);
            }
            else glBlitFramebuffer(0, 0, dx, dy, 0, 0, dx, dy, GL_COLOR_BUFFER_BIT, GL_NEAREST);
            if (FOrientCheck() && fViewsVk)
                for (int l = 0; l < 2; ++l)
                    CheckOrientation(l ? "eye 1" : "eye", FboLayerVk(0, l, s_atexEyeVk[0]), FboLayerVk(1, l, s_aeye[0].atex[s_aeye[0].iimage]), dx, dy);
            else if (FOrientCheck()) CheckOrientation("eye", s_afboMsaa[i], fboImage, dx, dy);
            glBindFramebuffer(GL_FRAMEBUFFER, s_afboMsaa[i]);
        }
        // (test: SLY2N_VR_SHOT=prefix@frame: that immersive frame's eyes as prefix_eye0.png / _eye1.png, its HUD panel
        // as prefix_panel.png; prefix@f1,f2,...: several frames, prefix_<frame>_eye0.png ...)
        static const char* const s_szShot = std::getenv("SLY2N_VR_SHOT");
        static int s_cframeShot = 0;
        if (s_szShot && i == 0) ++s_cframeShot;
        const char* const pchAt = s_szShot ? std::strchr(s_szShot, '@') : nullptr;
        bool fShotFrame = false;
        for (const char* p = pchAt; p && !fShotFrame; p = std::strchr(p + 1, ',')) fShotFrame = s_cframeShot == std::atoi(p + 1);
        if (fShotFrame)
        {
            const bool fSeveral = std::strchr(pchAt, ',') != nullptr;
            const std::string prefix = std::string(s_szShot, pchAt) + (fSeveral ? "_" + std::to_string(s_cframeShot) : std::string());
            const int dx = s_dxEye, dy = s_dyEye;
            for (int ieye = s_fMultiview ? 0 : i; ieye <= (s_fMultiview ? 1 : i); ++ieye)
            {
                std::vector<uint8_t> ab(size_t(dx) * size_t(dy) * 4), flipped(ab.size());
                static GLuint s_fboLayer = 0;
                if (fViewsVk)
                    // (Vulkan multiview: the eyes' two-layer target (GL's rows), or the image drawn straight into (rows from
                    // the top: written as they are below))
                    FboLayerVk(s_atexEyeVk[0] ? 0 : 1, ieye, s_atexEyeVk[0] ? s_atexEyeVk[0] : s_aeye[0].atex[s_aeye[0].iimage]);
                else if (s_fMultiview)
                {
                    // (a two-view image: each layer read through a framebuffer of its own)
                    if (!s_fboLayer) glGenFramebuffers(1, &s_fboLayer);
                    glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fboLayer);
                    glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, s_aeye[0].atex[s_aeye[0].iimage], 0, ieye);
                }
                else glBindFramebuffer(GL_READ_FRAMEBUFFER, s_fVk && s_afboMsaa[i] ? s_afboMsaa[i] : FboEyeImage(i));
                glPixelStorei(GL_PACK_ALIGNMENT, 1);
                glReadPixels(0, 0, dx, dy, GL_RGBA, GL_UNSIGNED_BYTE, ab.data());
                const bool fTopFirst = s_fVkDirect;   // (drawn straight into the image: its rows from the top)
                for (int y = 0; y < dy; ++y) std::memcpy(&flipped[size_t(y) * size_t(dx) * 4], &ab[size_t(fTopFirst ? y : dy - 1 - y) * size_t(dx) * 4], size_t(dx) * 4);
                for (size_t k = 3; k < flipped.size(); k += 4) flipped[k] = 255;
                stbi_write_png((prefix + "_eye" + std::to_string(ieye) + ".png").c_str(), dx, dy, 4, flipped.data(), dx * 4);
                if (ieye == 1) s_fShotPanel = true, s_strShotPanel = prefix + "_panel.png";
                Log("wrote %s_eye%d.png", prefix.c_str(), ieye);
            }
        }
        // the depth is not kept (a tiler then never writes it out)
        const GLenum aatt[] = {GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT};
        glBindFramebuffer(GL_FRAMEBUFFER, fboDraw);
        if (s_pfnInvalidate) s_pfnInvalidate(GL_FRAMEBUFFER, 2, aatt);
        Release(s_aeye[i]);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    s_frame.fEyesDrawn = true;
}

bool FPresent()
{
    if (!s_fStarted) return false;
    if (!s_frame.fBegun) BeginFrame();   // (a movie's or a load's picture: a frame of its own)
    // the game picture (resolved)
    if (g_gl.fboMsaa)
    {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_gl.fboMsaa);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g_gl.fbo);
        glBlitFramebuffer(0, 0, g_gl.renderWidth, g_gl.renderHeight, 0, 0, g_gl.renderWidth, g_gl.renderHeight, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    const bool fHud = s_frame.fEyesDrawn;
    if (s_fShotPanel)
    {
        s_fShotPanel = false;
        const int dx = g_gl.renderWidth, dy = g_gl.renderHeight;
        std::vector<uint8_t> ab(size_t(dx) * size_t(dy) * 4), flipped(ab.size());
        glBindFramebuffer(GL_READ_FRAMEBUFFER, g_gl.fbo);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, dx, dy, GL_RGBA, GL_UNSIGNED_BYTE, ab.data());
        for (int y = 0; y < dy; ++y) std::memcpy(&flipped[size_t(y) * size_t(dx) * 4], &ab[size_t(dy - 1 - y) * size_t(dx) * 4], size_t(dx) * 4);
        stbi_write_png(s_strShotPanel.c_str(), dx, dy, 4, flipped.data(), dx * 4);
    }
    if (s_frame.fBegun)
    {
        std::vector<const XrCompositionLayerBaseHeader*> alayer;
        XrCompositionLayerProjection proj{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        XrCompositionLayerSettingsFB layerSettings{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
        XrCompositionLayerProjectionView apv[2]{{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}, {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW}};
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
        if (s_frame.state.shouldRender)
        {
            if (s_frame.fEyesDrawn)
            {
                for (int i = 0; i < 2; ++i)
                {
                    apv[i].pose = s_frame.aview[i].pose;
                    apv[i].fov = s_frame.aview[i].fov;
                    const SWAPCHAIN& sc = s_aeye[s_fMultiview ? 0 : i];
                    apv[i].subImage.swapchain = sc.h;
                    // (the eye resolution: the part drawn; OpenGL's rows from the bottom, Vulkan's copy from the top)
                    const bool fWhole = s_fMultiview && !s_fVk;   // (GLES's two-view pass: the whole image)
                    apv[i].subImage.imageRect = {{0, 0}, {fWhole ? sc.dx : s_dxEye, fWhole ? sc.dy : s_dyEye}};
                    apv[i].subImage.imageArrayIndex = s_fMultiview ? uint32_t(i) : 0;
                }
                proj.space = s_spaceLocal;
                proj.viewCount = 2;
                proj.views = apv;
                // (Upscaling: the compositor's super sampling / sharpening of the eyes, where the runtime offers it)
                if (const uint64_t flags = LayerFilterFlags())
                {
                    layerSettings.layerFlags = flags;
                    proj.next = &layerSettings;
                }
                static uint64_t s_flagsLogged = ~0ull;
                if (LayerFilterFlags() != s_flagsLogged)
                {
                    s_flagsLogged = LayerFilterFlags();
                    if (s_fExtLayerSettings) Log("eyes' compositor filter: flags 0x%llx (%s)", static_cast<unsigned long long>(s_flagsLogged),
                                                 s_flagsLogged == 0 ? "off" : g_vropts.upscaling == VRUPSCALE_Sharpen ? "sharpen" : "super resolution");
                    else Log("eyes' compositor filter: not offered by the runtime (XR_FB_composition_layer_settings): none");
                }
                alayer.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj));
            }
            // the game picture to the panel's swapchain (its size follows the picture's)
            const int dx = g_gl.renderWidth, dy = g_gl.renderHeight;
            if (s_panel.h && (s_panel.dx != dx || s_panel.dy != dy)) DestroySwapchain(s_panel);
            if (!s_panel.h) FCreateSwapchain(s_panel, dx, dy, s_formatColor, 1);
            if (s_panel.h && FAcquire(s_panel))
            {
                const GLuint fbo = s_panel.afbo[s_panel.iimage];
                glBindFramebuffer(GL_FRAMEBUFFER, fbo);
                glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s_panel.atex[s_panel.iimage], 0);
                glViewport(0, 0, dx, dy);
                glDisable(GL_DEPTH_TEST);
                glDisable(GL_STENCIL_TEST);
                glDisable(GL_BLEND);
                glDisable(GL_CULL_FACE);
                glDisable(GL_SCISSOR_TEST);
                if (FGles() && s_fSrgb) glDisable(kGlFramebufferSrgb);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                s_shCopy.Use();
                glUniform1i(glGetUniformLocation(s_progCopy, "tex"), 0);
                glUniform1i(glGetUniformLocation(s_progCopy, "fOpaque"), fHud ? 0 : 1);
                glUniform1i(glGetUniformLocation(s_progCopy, "fFlipY"), s_fVk ? 1 : 0);
                glUniform2f(glGetUniformLocation(s_progCopy, "uvScale"), 1.0f, 1.0f);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, g_gl.fbc);
                glBindVertexArray(s_vaoEmpty);
                glDrawArrays(GL_TRIANGLES, 0, 3);
                glBindVertexArray(0);
                if (FOrientCheck()) CheckOrientation(fHud ? "HUD panel" : "flat screen", g_gl.fbo, fbo, dx, dy);
                Release(s_panel);
                // where it floats: the Flat Screen ahead of the anchor; the HUD panel nearer, turning after the head
                const float rAspect = float(dx) / float(dy);
                float width = 0.0f, dist = 0.0f;
                float yaw = s_yawAnchor;
                if (fHud)
                {
                    width = 1.5f, dist = 1.4f;
                    if (s_frame.fViews)
                    {
                        float d = YawOf(Quat(s_frame.aview[0].pose.orientation)) - s_yawPanel;
                        while (d > kPi) d -= 2.0f * kPi;
                        while (d < -kPi) d += 2.0f * kPi;
                        const float dMax = glm::radians(30.0f);
                        if (std::fabs(d) > dMax) s_yawPanel += d > 0 ? d - dMax : d + dMax;
                    }
                    yaw = s_yawPanel;
                }
                else
                {
                    static const float s_awidth[3] = {2.2f, 3.2f, 4.6f};
                    width = s_awidth[std::clamp(g_vropts.screenSize, 0, 2)];
                    dist = 3.0f;
                }
                const glm::quat q = glm::angleAxis(yaw, glm::vec3(0, 1, 0));
                const glm::vec3 pos = s_posAnchor + q * glm::vec3(0.0f, fHud ? -0.1f : 0.0f, -dist);
                quad.layerFlags = fHud ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
                quad.space = s_spaceLocal;
                // (the binocucom: its frame fixed to the head, wide enough that the window fills the view)
                if (fHud && s_view == VRVIEW_Binoc && s_spaceView)
                {
                    quad.space = s_spaceView;
                    quad.pose.orientation = XrQuaternionf{0, 0, 0, 1};
                    quad.pose.position = XrVector3f{0.0f, 0.0f, -1.0f};
                    quad.size = {2.2f, 2.2f / rAspect};
                }
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain = s_panel.h;
                quad.subImage.imageRect = {{0, 0}, {dx, dy}};
                if (quad.space != s_spaceView)
                {
                    quad.pose.orientation = XrQuat(q);
                    quad.pose.position = XrVec(pos);
                    quad.size = {width, width / rAspect};
                }
                alayer.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad));
            }
        }
        XrFrameEndInfo ei{XR_TYPE_FRAME_END_INFO};
        ei.displayTime = s_frame.state.predictedDisplayTime;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        ei.layerCount = uint32_t(alayer.size());
        ei.layers = alayer.data();
        if (FVkDeferred())
        {
            // (on the worker after the frame's submission: the layers copied, their pointers made again there)
            struct ENDFRAME
            {
                XrFrameEndInfo ei;
                XrCompositionLayerProjection proj;
                XrCompositionLayerProjectionView apv[2];
                XrCompositionLayerSettingsFB layerSettings;
                XrCompositionLayerQuad quad;
                int akind[2] = {0, 0};   // the layers in order: 1 the projection, 2 the quad
            };
            auto p = std::make_shared<ENDFRAME>();
            p->ei = ei;
            p->proj = proj;
            p->apv[0] = apv[0], p->apv[1] = apv[1];
            p->layerSettings = layerSettings;
            p->quad = quad;
            for (size_t k = 0; k < alayer.size() && k < 2; ++k)
                p->akind[k] = alayer[k] == reinterpret_cast<const XrCompositionLayerBaseHeader*>(&proj) ? 1 : 2;
            const XrSession session = s_session;
            CaneVk::AfterNextSubmit([p, session] {
                const XrCompositionLayerBaseHeader* alayerCopy[2] = {};
                p->proj.views = p->apv;
                if (p->proj.next) p->proj.next = &p->layerSettings;
                for (uint32_t k = 0; k < p->ei.layerCount && k < 2; ++k)
                    alayerCopy[k] = p->akind[k] == 1 ? reinterpret_cast<const XrCompositionLayerBaseHeader*>(&p->proj)
                                                     : reinterpret_cast<const XrCompositionLayerBaseHeader*>(&p->quad);
                p->ei.layers = alayerCopy;
                FCheck(pxrEndFrame(session, &p->ei), "xrEndFrame");
            });
        }
        else FCheck(pxrEndFrame(s_session, &ei), "xrEndFrame");
        s_frame.fBegun = false;
        // (SLY2N_VR_FPSLOG: the headset frames' rate every 5 s)
        static const bool s_fLog = std::getenv("SLY2N_VR_FPSLOG") != nullptr;
        if (s_fLog)
        {
            static double s_t0 = glfwGetTime();
            static int s_c = 0, s_cEyes = 0;
            ++s_c;
            s_cEyes += fHud;
            const double t = glfwGetTime();
            if (t - s_t0 >= 5.0)
            {
                Log("fps %.1f (%d of %d frames immersive), display %.0f Hz", s_c / (t - s_t0), s_cEyes, s_c, double(s_hzDisplay));
                s_t0 = t, s_c = 0, s_cEyes = 0;
            }
        }
    }
    else
        std::this_thread::sleep_for(std::chrono::milliseconds(5));   // (no frame slot: the session not running)
    s_fEyesLastFrame = s_frame.fEyesDrawn;   // (the eye resolution: this frame was immersive)
    s_frame.fEyesDrawn = false;
#if CANE_VULKAN
    if (s_fVk)
    {
        // (the Vulkan renderer's frame ends here: no window picture in VR)
        CaneVk::EndFrame();
        return true;
    }
#endif
#if !defined(__ANDROID__)
    // the desktop window mirrors the game picture
    int w = 0, h = 0;
    glfwGetFramebufferSize(g_gl.window, &w, &h);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g_gl.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (w > 0 && h > 0)
    {
        const float r = float(g_gl.renderWidth) / float(g_gl.renderHeight);
        int dx = w, dy = int(float(w) / r);
        if (dy > h) dy = h, dx = int(float(h) * r);
        glBlitFramebuffer(0, 0, g_gl.renderWidth, g_gl.renderHeight, (w - dx) / 2, (h - dy) / 2, (w - dx) / 2 + dx, (h - dy) / 2 + dy,
                          GL_COLOR_BUFFER_BIT, GL_LINEAR);
    }
#endif
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

bool FSkipSwap()
{
#if defined(__ANDROID__)
    return s_fStarted;
#else
    return s_fStarted && s_fVk;   // (Vulkan: FPresent ends the renderer's frame; no window picture)
#endif
}

bool FPadState(GLFWgamepadstate* pstate)
{
    if (!s_fStarted || !s_fPad || !s_fRunning) return false;
    *pstate = s_pad;
    return true;
}

void Rumble(float gLow, float gHigh)
{
    if (!s_fStarted || !s_fRunning || !s_actionset) return;
    static float s_aPrev[2] = {-1.0f, -1.0f};
    const float ag[2] = {gLow, gHigh};
    for (int i = 0; i < 2; ++i)
    {
        if (std::fabs(ag[i] - s_aPrev[i]) < 0.02f) continue;
        s_aPrev[i] = ag[i];
        XrHapticActionInfo hi{XR_TYPE_HAPTIC_ACTION_INFO};
        hi.action = s_aaction[i == 0 ? ACT_HapticL : ACT_HapticR];
        XrHapticVibration v{XR_TYPE_HAPTIC_VIBRATION};
        v.amplitude = std::clamp(ag[i], 0.0f, 1.0f);
        v.duration = ag[i] > 0.0f ? XrDuration(500000000) : XR_MIN_HAPTIC_DURATION;   // (renewed while it changes)
        v.frequency = XR_FREQUENCY_UNSPECIFIED;
        pxrApplyHapticFeedback(s_session, &hi, reinterpret_cast<const XrHapticBaseHeader*>(&v));
    }
}
}  // namespace vr
#endif  // !__SWITCH__
