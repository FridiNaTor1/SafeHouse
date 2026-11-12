// iOS has no ApplicationServices: GLFW's glfw3native.h (Cocoa part, included by ImGui's GLFW backend on Apple) only
// needs CGDirectDisplayID from it. platform/ios/glfw_sdl_ios.cpp answers glfwGetCocoaWindow / glfwGetCocoaMonitor.
#pragma once
#include <CoreGraphics/CoreGraphics.h>
#include <stdint.h>
typedef uint32_t CGDirectDisplayID;
