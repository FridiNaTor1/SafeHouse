#pragma once
// The Vulkan entry points Sly 2's Vulkan renderer uses (platform/vulkan/gl_vulkan.cpp), loaded at run time from the
// system's loader (libvulkan.so.1 / vulkan-1.dll): nothing links against Vulkan, and a machine without it starts the
// game on OpenGL. The functions keep their Vulkan names inside cane::vk.
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>

#include <string>

namespace cane::vk {

#define CANE_VK_GLOBAL(X) \
    X(vkCreateInstance) X(vkEnumerateInstanceVersion) X(vkEnumerateInstanceExtensionProperties) \
    X(vkEnumerateInstanceLayerProperties)

#define CANE_VK_INSTANCE(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceProperties2) X(vkGetPhysicalDeviceFeatures2) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) \
    X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr)

// (optional: present only with the extensions enabled)
#define CANE_VK_INSTANCE_OPTIONAL(X) \
    X(vkDestroySurfaceKHR) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkGetPhysicalDeviceSurfacePresentModesKHR) \
    X(vkCreateDebugUtilsMessengerEXT) X(vkDestroyDebugUtilsMessengerEXT)

#define CANE_VK_DEVICE(X) \
    X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkDeviceWaitIdle) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkInvalidateMappedMemoryRanges) \
    X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) \
    X(vkCreatePipelineCache) X(vkDestroyPipelineCache) X(vkGetPipelineCacheData) X(vkCreateGraphicsPipelines) \
    X(vkDestroyPipeline) \
    X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkResetCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkWaitSemaphores) X(vkGetSemaphoreCounterValue) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkGetQueryPoolResults) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp) \
    X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdPipelineBarrier2) X(vkCmdBindPipeline) \
    X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) \
    X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdDrawIndexedIndirect) X(vkCmdDrawIndirect) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetLineWidth) X(vkCmdSetBlendConstants) \
    X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetStencilReference) \
    X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) \
    X(vkCmdSetDepthCompareOp) X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) X(vkCmdSetDepthBoundsTestEnable) X(vkCmdSetDepthBounds) \
    X(vkCmdClearAttachments) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) \
    X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) X(vkCmdCopyImage) X(vkCmdBlitImage) \
    X(vkCmdResolveImage) \
    X(vkCreateRenderPass2) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) \
    X(vkCmdBeginRenderPass2) X(vkCmdEndRenderPass2)

#define CANE_VK_DEVICE_OPTIONAL(X) \
    X(vkCmdPushDescriptorSetKHR) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkCmdSetColorBlendEnableEXT) X(vkCmdSetColorBlendEquationEXT) \
    X(vkCmdSetColorWriteMaskEXT)

#define CANE_VK_DECLARE(name) extern PFN_##name name;
CANE_VK_GLOBAL(CANE_VK_DECLARE)
CANE_VK_INSTANCE(CANE_VK_DECLARE)
CANE_VK_INSTANCE_OPTIONAL(CANE_VK_DECLARE)
CANE_VK_DEVICE(CANE_VK_DECLARE)
CANE_VK_DEVICE_OPTIONAL(CANE_VK_DECLARE)
extern PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
#undef CANE_VK_DECLARE

// The system's loader opened and the global entry points found; false (and why) when there is none.
bool LoadLoader(std::string* error);
// The instance's entry points; false (the missing one's name in *error) when a required one is absent.
bool LoadInstance(VkInstance instance, std::string* error);
// The device's entry points (vkGetDeviceProcAddr: no loader trampoline per call).
bool LoadDevice(VkDevice device, std::string* error);
void UnloadLoader();

}  // namespace cane::vk
