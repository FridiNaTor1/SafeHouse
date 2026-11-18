#pragma once
// The GLFW API the programs use, implemented on SDL by platform/ios/glfw_sdl_ios.cpp (as platform/android/GLFW for the
// Android app): the repository's GLFW 3.3 header plus the 3.4 calls and constants they also use.
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include "../../../../Libraries/include/GLFW/glfw3.h"
#ifndef GLFW_PLATFORM_WAYLAND
#define GLFW_PLATFORM_WAYLAND 0x00060003
#define GLFW_PLATFORM_COCOA 0x00060002
#define GLFW_PLATFORM_NULL 0x00060005
#endif
#ifndef GLFW_WAYLAND_APP_ID
#define GLFW_WAYLAND_APP_ID 0x00026001
#endif
#ifdef __cplusplus
extern "C" {
#endif
int glfwGetPlatform(void);
void glfwWindowHintString(int hint, const char* value);
#ifdef __cplusplus
}
#endif
