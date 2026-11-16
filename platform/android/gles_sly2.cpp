// Sly 2's renderer on OpenGL ES 3.2 (gles_sly2.h). The Quest 3S (Adreno 740, the AYN Thor's GPU too) has
// EXT_clip_control, NV_shader_noperspective_interpolation, EXT_geometry_shader (core in 3.2) and EXT_buffer_storage,
// but no EXT_base_instance / EXT_multi_draw_indirect: Sly2-Release/render.cpp then issues the pass draws one by one
// with the draw index as a uniform (g_fDrawUniform).
#include "gles_sly2.h"

#include <glad/glad.h>
#include <EGL/egl.h>
#include <SDL.h>

#include <cstring>
#include <string>

namespace {
void* Lookup(const char* name) {
    void* p = reinterpret_cast<void*>(eglGetProcAddress(name));
    if (!p) p = SDL_GL_GetProcAddress(name);
    return p;
}
using PFNCLEARDEPTHF = void (*)(GLfloat);
PFNCLEARDEPTHF s_pfnClearDepthf = nullptr;
void APIENTRY ClearDepthShim(GLdouble depth) {
    if (s_pfnClearDepthf) s_pfnClearDepthf(GLfloat(depth));
}
bool Extension(const char* name) { return SDL_GL_ExtensionSupported(name) == SDL_TRUE; }
}  // namespace

namespace cane::gles {
void* ProcAddress(const char* name) {
    if (!std::strcmp(name, "glClearDepth")) {
        if (!s_pfnClearDepthf) s_pfnClearDepthf = reinterpret_cast<PFNCLEARDEPTHF>(Lookup("glClearDepthf"));
        return s_pfnClearDepthf ? reinterpret_cast<void*>(&ClearDepthShim) : nullptr;
    }
    // (desktop-only entry points the renderer probes: an ES driver may still return a stub for them)
    if (!std::strcmp(name, "glBufferStorage")) return Extension("GL_EXT_buffer_storage") ? Lookup("glBufferStorageEXT") : nullptr;
    if (!std::strcmp(name, "glClipControl")) return Extension("GL_EXT_clip_control") ? Lookup("glClipControlEXT") : nullptr;
    if (!std::strcmp(name, "glMultiDrawElementsIndirect") || !std::strcmp(name, "glDrawElementsInstancedBaseVertexBaseInstance"))
        return nullptr;
    if (!std::strcmp(name, "glGetQueryObjectui64v")) return Lookup("glGetQueryObjectui64vEXT");
    if (!std::strncmp(name, "glGetTextureSamplerHandle", 25) || !std::strncmp(name, "glMakeTextureHandle", 19)) return nullptr;
    return Lookup(name);
}

bool FillGlad(std::string& error) {
    std::string missing;
#define CANE_FILL(fn, required)                                                          \
    if (!glad_##fn) glad_##fn = reinterpret_cast<decltype(glad_##fn)>(ProcAddress(#fn)); \
    if (!glad_##fn && required) missing += " " #fn;
    // ES 3.0 / 3.1 / 3.2 core under their desktop names (glad loads desktop 1.0-3.2 only)
    CANE_FILL(glBindSampler, true) CANE_FILL(glGenSamplers, true) CANE_FILL(glDeleteSamplers, true)
    CANE_FILL(glSamplerParameteri, true) CANE_FILL(glSamplerParameterf, true) CANE_FILL(glVertexAttribDivisor, true)
    CANE_FILL(glTexStorage2D, true) CANE_FILL(glTexStorage3D, true) CANE_FILL(glVertexAttribIPointer, true)
    CANE_FILL(glBindBufferBase, true) CANE_FILL(glBindBufferRange, true) CANE_FILL(glMapBufferRange, true)
    CANE_FILL(glUnmapBuffer, true) CANE_FILL(glFenceSync, true) CANE_FILL(glClientWaitSync, true) CANE_FILL(glDeleteSync, true)
    CANE_FILL(glDrawElementsBaseVertex, true) CANE_FILL(glRenderbufferStorageMultisample, true) CANE_FILL(glBlitFramebuffer, true)
    CANE_FILL(glUniform1ui, true) CANE_FILL(glGenQueries, false) CANE_FILL(glBeginQuery, false) CANE_FILL(glEndQuery, false)
    CANE_FILL(glClearDepth, true) CANE_FILL(glGetQueryObjectui64v, false)
    // (perf-5: the glob textures as array layers, render.cpp LayerRefOfTex; the frame end's invalidates, gl.cpp)
    CANE_FILL(glCopyImageSubData, false) CANE_FILL(glInvalidateFramebuffer, false)
#undef CANE_FILL
    // (glad loaded the desktop-only names it found as stubs on some drivers: never call them)
    glad_glMultiDrawElementsIndirect = nullptr;
    glad_glDrawElementsInstancedBaseVertexBaseInstance = nullptr;
    if (!missing.empty()) {
        error = "OpenGL ES 3.2 entry points missing:" + missing;
        return false;
    }
    return true;
}

bool HasBaseInstance() { return false; }

std::string TranslateShader(const std::string& source) {
    const size_t v = source.find("#version");
    const size_t body = v == std::string::npos ? 0 : source.find('\n', v) + 1;
    std::string header = "#version 320 es\n";
    // noperspective (the GS's screen-affine colours): native on Adreno; elsewhere perspective-correct (close)
    header += "#extension GL_NV_shader_noperspective_interpolation : enable\n"
              "#ifndef GL_NV_shader_noperspective_interpolation\n#define noperspective\n#endif\n";
    header += "precision highp float;\nprecision highp int;\nprecision highp sampler2D;\nprecision highp sampler2DArray;\n"
              "precision highp usampler2D;\nprecision highp isampler2D;\n";
    header += "#define CANE_GLES 1\n";
    if (!HasBaseInstance()) header += "#define CANE_PULL 1\n";   // (glob.vert: vertex pulling, render.cpp's merged runs)
    // GLSL 4.20+ allows `const` locals with run-time initializers (glob.vert: `const uint irec = floatBitsToUint(d.w)`);
    // GLSL ES wants constant expressions there. Function-scope (indented) declarations lose the qualifier.
    std::string text = source.substr(body), out;
    out.reserve(text.size());
    for (size_t at = 0; at < text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size(); else ++end;
        std::string line = text.substr(at, end - at);
        const size_t indent = line.find_first_not_of(" \t");
        if (indent != std::string::npos && indent > 0 && line.compare(indent, 6, "const ") == 0) line.erase(indent, 6);
        out += line;
        at = end;
    }
    return source.substr(0, v == std::string::npos ? 0 : v) + header + out;
}
}  // namespace cane::gles
