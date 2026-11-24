// The Vulkan loader opened at run time: see vk_loader.h.
#include "vk_loader.h"

#include <cstdlib>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#if defined(__ANDROID__)
#include <cstdio>
#include "android/vulkan_driver.h"
#endif
#if defined(CANE_ADRENOTOOLS)
#include <adrenotools/driver.h>
#endif

namespace cane::vk {

#define CANE_VK_DEFINE(name) PFN_##name name = nullptr;
CANE_VK_GLOBAL(CANE_VK_DEFINE)
CANE_VK_INSTANCE(CANE_VK_DEFINE)
CANE_VK_INSTANCE_OPTIONAL(CANE_VK_DEFINE)
CANE_VK_DEVICE(CANE_VK_DEFINE)
CANE_VK_DEVICE_OPTIONAL(CANE_VK_DEFINE)
PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#undef CANE_VK_DEFINE

namespace {
#if defined(_WIN32)
HMODULE g_library = nullptr;
void* Symbol(const char* name) { return reinterpret_cast<void*>(GetProcAddress(g_library, name)); }
#else
void* g_library = nullptr;
void* Symbol(const char* name) { return dlsym(g_library, name); }
#endif
bool g_customDriver = false;   // (Android: a driver package loaded through libadrenotools, android/vulkan_driver.h)
std::string g_customName;

#if defined(__ANDROID__)
// The player's custom driver (Settings > Display > Vulkan driver), or nullptr: the system's driver.
void* OpenCustomDriver()
{
    namespace drv = cane::android::vkdriver;
    const drv::Selection chosen = drv::selection();
    if (chosen.empty()) return nullptr;
    if (cane::android::openXrRuntimeInstalled())
    {
        std::printf("Vulkan: custom driver %s not used on a VR headset (the system's driver)\n", chosen.name.c_str());
        return nullptr;
    }
    std::string why;
    if (drv::previousStartCrashed(&why))
    {
        drv::disable("The custom Vulkan driver " + chosen.name + " crashed while a game was starting (" + why +
                     "): SafeHouse uses the system's driver again");
        return nullptr;
    }
#if defined(CANE_ADRENOTOOLS)
    std::error_code ec;
    const std::filesystem::path tmp = drv::root() / ".tmp";
    std::filesystem::create_directories(tmp, ec);
    const std::string libs = cane::android::nativeLibraryDirectory() + "/";
    const std::string dir = (drv::root() / chosen.id).string() + "/";
    const std::string tmpDir = tmp.string() + "/";
    void* handle = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, tmpDir.c_str(), libs.c_str(), dir.c_str(),
                                              chosen.library.c_str(), nullptr, nullptr);
    if (!handle)
    {
        std::printf("Vulkan: the custom driver %s could not be loaded (adrenotools): the system's driver\n", chosen.name.c_str());
        return nullptr;
    }
    std::printf("Vulkan: custom driver %s (%s, through libadrenotools)\n", chosen.name.c_str(), chosen.library.c_str());
    drv::beginStart();
    g_customDriver = true;
    g_customName = chosen.name;
    return handle;
#else
    std::printf("Vulkan: custom drivers need an arm64 build: the system's driver\n");
    return nullptr;
#endif
}
#endif
}  // namespace

bool CustomDriverActive() { return g_customDriver; }

void NoteStartFinished()
{
#if defined(__ANDROID__)
    if (g_customDriver) cane::android::vkdriver::endStart();
#endif
}

void NoteStartFailed(const std::string& why)
{
#if defined(__ANDROID__)
    if (!g_customDriver || why.empty()) return;
    cane::android::vkdriver::endStart();
    cane::android::vkdriver::disable("The custom Vulkan driver " + g_customName + " did not start (" + why +
                                     "): SafeHouse uses the system's driver again");
#else
    (void)why;
#endif
}

bool LoadLoader(std::string* error)
{
    if (vkGetInstanceProcAddr) return true;
    // (tests: SLY2N_VKLOADER=<library> loads that library instead of the system's loader; a name that does not exist
    // is a machine without Vulkan)
    const char* szOverride = std::getenv("SLY2N_VKLOADER");
    if (szOverride && *szOverride)
    {
#if defined(_WIN32)
        g_library = LoadLibraryA(szOverride);
#else
        g_library = dlopen(szOverride, RTLD_NOW | RTLD_LOCAL);
#endif
    }
    else
    {
#if defined(_WIN32)
    g_library = LoadLibraryA("vulkan-1.dll");
#elif defined(__APPLE__)
    g_library = dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!g_library) g_library = dlopen("libMoltenVK.dylib", RTLD_NOW | RTLD_LOCAL);
#elif defined(__ANDROID__)
    g_library = OpenCustomDriver();
    if (!g_library) g_library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);   // (the system's loader: no .so.1 on Android)
#else
    g_library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!g_library) g_library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
#endif
    }
    if (!g_library)
    {
        if (error) *error = "no Vulkan loader on this system";
        return false;
    }
    vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(Symbol("vkGetInstanceProcAddr"));
    if (!vkGetInstanceProcAddr)
    {
        if (error) *error = "the Vulkan loader has no vkGetInstanceProcAddr";
        UnloadLoader();
        return false;
    }
#define CANE_VK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(nullptr, #name));
    CANE_VK_GLOBAL(CANE_VK_LOAD)
#undef CANE_VK_LOAD
    if (!vkCreateInstance || !vkEnumerateInstanceExtensionProperties)
    {
        if (error) *error = "the Vulkan loader is incomplete";
        UnloadLoader();
        return false;
    }
    return true;
}

bool LoadInstance(VkInstance instance, std::string* error)
{
    bool ok = true;
#define CANE_VK_LOAD(name)                                                                  \
    name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name));          \
    if (!name && ok) { ok = false; if (error) *error = std::string("missing ") + #name; }
    CANE_VK_INSTANCE(CANE_VK_LOAD)
#undef CANE_VK_LOAD
#define CANE_VK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(instance, #name));
    CANE_VK_INSTANCE_OPTIONAL(CANE_VK_LOAD)
#undef CANE_VK_LOAD
    return ok;
}

bool LoadDevice(VkDevice device, std::string* error)
{
    bool ok = true;
#define CANE_VK_LOAD(name)                                                                  \
    name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));              \
    if (!name && ok) { ok = false; if (error) *error = std::string("missing ") + #name; }
    CANE_VK_DEVICE(CANE_VK_LOAD)
#undef CANE_VK_LOAD
#define CANE_VK_LOAD(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));
    CANE_VK_DEVICE_OPTIONAL(CANE_VK_LOAD)
#undef CANE_VK_LOAD
    return ok;
}

void UnloadLoader()
{
    // (the loader stays mapped: GLFW may hold its own reference, and unloading drivers at exit gains nothing)
    vkGetInstanceProcAddr = nullptr;
    g_library = nullptr;
}

}  // namespace cane::vk
