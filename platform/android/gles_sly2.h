#pragma once
// Sly 2's renderer on OpenGL ES 3.2 (Android): what Sly2-Release/gl.cpp needs beyond the desktop GL 4.3 loader it
// shares with the desktop builds (Libraries/include/glad). See android/README.md, "Renderer".
#include <string>

namespace cane::gles {
// The ES entry point for a desktop GL name: glClipControl -> glClipControlEXT, glClearDepth -> a glClearDepthf
// wrapper, the names ES spells the same way; nullptr when the driver has neither (platform/gl_context.h ProcAddress).
void* ProcAddress(const char* name);
// After gladLoadGLLoader: the entry points glad skipped because the ES context reports version 3.2 (samplers, divisors,
// texture storage, ...) fetched by name. False (with the missing names in error) when a required one is absent.
bool FillGlad(std::string& error);
// EXT_base_instance + EXT_multi_draw_indirect: not used yet; without them every draw sets its index as a uniform.
bool HasBaseInstance();
// A GLSL 4.30 core source as GLSL ES 3.20 (#version, extensions, precision, CANE_GLES / CANE_DRAW_UNIFORM).
std::string TranslateShader(const std::string& source);
}  // namespace cane::gles
