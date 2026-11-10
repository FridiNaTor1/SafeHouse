Vulkan headers (Khronos Group), Vulkan-Headers 1.4.321 (VK_HEADER_VERSION 321)
Source: https://github.com/KhronosGroup/Vulkan-Headers (tag v1.4.321)
Headers (Libraries/include/vulkan: vulkan.h, vulkan_core.h, vk_platform.h; Libraries/include/vk_video: the video codec
  headers vulkan_core.h includes): Apache-2.0 (SPDX-License-Identifier in each file; the licence text is here).
Used by Sly 2's optional Vulkan renderer (platform/vulkan). The Vulkan loader is not shipped: the game loads the
system's (libvulkan.so.1 on Linux, vulkan-1.dll on Windows, which the GPU driver installs) at run time and falls back
to OpenGL when there is none.
