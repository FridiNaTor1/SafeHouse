// The Vulkan loader opened at run time: see vk_loader.h.
#include "vk_loader.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
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
}  // namespace

bool LoadLoader(std::string* error)
{
    if (vkGetInstanceProcAddr) return true;
#if defined(_WIN32)
    g_library = LoadLibraryA("vulkan-1.dll");
#elif defined(__APPLE__)
    g_library = dlopen("libvulkan.1.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!g_library) g_library = dlopen("libMoltenVK.dylib", RTLD_NOW | RTLD_LOCAL);
#else
    g_library = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!g_library) g_library = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
#endif
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
