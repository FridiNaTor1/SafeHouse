// Sly 1's OpenGL renderer on OpenGL ES: see gles_sly1.h.
#include "gles_sly1.h"

#include <glad/glad.h>

#include <GLFW/glfw3.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "../gl_context.h"
#include "../switch/sly1/deko/texture_decode.h"   // (CaneDeko::DecodeBc: BC1 / BC3 blocks to RGBA8)

namespace cane::gles1 {
namespace {
bool s_active = false;
int s_minor = 0;   // (the context's ES minor version: 1 or 2)
std::string s_name = "OpenGL ES";

void* Lookup(const char* name) { return reinterpret_cast<void*>(cane::gl::ProcAddress(name)); }

bool HasExtension(const char* name)
{
    GLint count = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; ++i)
        if (const char* e = reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS, GLuint(i))); e && !std::strcmp(e, name))
            return true;
    return false;
}

bool EnvOn(const char* name)
{
    const char* sz = std::getenv(name);
    return sz && *sz && std::atoi(sz) != 0;
}

// ---- the real ES entry points behind the shims
using PFNDEPTHRANGEF = void(APIENTRYP)(GLfloat, GLfloat);
using PFNCLEARDEPTHF = void(APIENTRYP)(GLfloat);
using PFNDRAWBASEINSTANCEARRAYS = void(APIENTRYP)(GLenum, GLint, GLsizei, GLsizei, GLuint);
using PFNDRAWBASEINSTANCEELEMENTS = void(APIENTRYP)(GLenum, GLsizei, GLenum, const void*, GLsizei, GLuint);
using PFNDRAWBASEVERTEXBASEINSTANCE = void(APIENTRYP)(GLenum, GLsizei, GLenum, const void*, GLsizei, GLint, GLuint);
using PFNDRAWINSTANCEDBASEVERTEX = void(APIENTRYP)(GLenum, GLsizei, GLenum, const void*, GLsizei, GLint);
using PFNDRAWBASEVERTEX = void(APIENTRYP)(GLenum, GLsizei, GLenum, const void*, GLint);
PFNDEPTHRANGEF s_depthRangef = nullptr;
PFNCLEARDEPTHF s_clearDepthf = nullptr;
PFNGLGETFLOATVPROC s_getFloatv = nullptr;
PFNGLTEXSTORAGE2DPROC s_texStorage2D = nullptr;
PFNGLCOMPRESSEDTEXSUBIMAGE2DPROC s_compressedTexSubImage2D = nullptr;
PFNGLBINDVERTEXARRAYPROC s_bindVertexArray = nullptr;
PFNGLDELETEVERTEXARRAYSPROC s_deleteVertexArrays = nullptr;
PFNGLVERTEXATTRIBDIVISORPROC s_vertexAttribDivisor = nullptr;
PFNGLENABLEVERTEXATTRIBARRAYPROC s_enableVertexAttribArray = nullptr;
PFNGLDISABLEVERTEXATTRIBARRAYPROC s_disableVertexAttribArray = nullptr;
PFNDRAWBASEINSTANCEARRAYS s_extDrawArraysBaseInstance = nullptr;
PFNDRAWBASEINSTANCEELEMENTS s_extDrawElementsBaseInstance = nullptr;
PFNDRAWBASEVERTEXBASEINSTANCE s_extDrawElementsBaseVertexBaseInstance = nullptr;
PFNDRAWINSTANCEDBASEVERTEX s_drawElementsInstancedBaseVertex = nullptr;
PFNDRAWBASEVERTEX s_drawElementsBaseVertex = nullptr;
void* s_bufferStorage = nullptr;
bool s_s3tc = false;
GLfloat s_pointSize = 1.0f;

void APIENTRY ClearDepthShim(GLdouble depth) { s_clearDepthf(GLfloat(depth)); }
void APIENTRY DepthRangeShim(GLdouble n, GLdouble f) { s_depthRangef(GLfloat(n), GLfloat(f)); }
// (ES has no fixed point size: gl_PointSize decides; the value is kept for the state mirror's glGetFloatv)
void APIENTRY PointSizeShim(GLfloat size) { s_pointSize = size; }
void APIENTRY GetFloatvShim(GLenum pname, GLfloat* data)
{
    if (pname == GL_POINT_SIZE) { *data = s_pointSize; return; }
    s_getFloatv(pname, data);
}
void APIENTRY GetDoublevShim(GLenum pname, GLdouble* data)
{
    GLfloat values[16] = {};
    GetFloatvShim(pname, values);
    int count = 1;
    if (pname == GL_DEPTH_RANGE || pname == GL_ALIASED_LINE_WIDTH_RANGE) count = 2;
    else if (pname == GL_VIEWPORT || pname == GL_SCISSOR_BOX || pname == GL_COLOR_CLEAR_VALUE || pname == GL_BLEND_COLOR) count = 4;
    for (int i = 0; i < count; ++i) data[i] = values[i];
}
void APIENTRY DrawBufferShim(GLenum buffer) { glDrawBuffers(1, &buffer); }
// (the developer window's texture export: level 0 of the bound 2D texture through a read framebuffer)
void APIENTRY GetTexImageShim(GLenum target, GLint level, GLenum format, GLenum type, void* pixels)
{
    if (target != GL_TEXTURE_2D) return;
    GLint texture = 0, width = 0, height = 0, readFramebuffer = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, level, GL_TEXTURE_HEIGHT, &height);
    if (!texture || width <= 0 || height <= 0) return;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFramebuffer);
    GLuint framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, GLuint(texture), level);
    if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
        glReadPixels(0, 0, width, height, format, type, pixels);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, GLuint(readFramebuffer));
    glDeleteFramebuffers(1, &framebuffer);
}

// ---- BC1 / BC3 without EXT_texture_compression_s3tc: RGBA8 storage, the blocks decoded on upload
constexpr GLenum kBc1 = 0x83F0, kBc1a = 0x83F1, kBc3 = 0x83F3;
bool IsBc(GLenum format) { return format == kBc1 || format == kBc1a || format == kBc3; }
void APIENTRY TexStorage2DShim(GLenum target, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height)
{
    s_texStorage2D(target, levels, IsBc(internalformat) ? GL_RGBA8 : internalformat, width, height);
}
void APIENTRY CompressedTexSubImage2DShim(GLenum target, GLint level, GLint x, GLint y, GLsizei width, GLsizei height,
                                          GLenum format, GLsizei size, const void* data)
{
    if (!IsBc(format) || !data)
    {
        s_compressedTexSubImage2D(target, level, x, y, width, height, format, size, data);
        return;
    }
    // (whole blocks are decoded: the last row / column of blocks of a level whose size is not a multiple of 4 is cut)
    const unsigned bw = (unsigned(width) + 3) & ~3u, bh = (unsigned(height) + 3) & ~3u;
    const std::vector<uint8_t> rgba = CaneDeko::DecodeBc(static_cast<const uint8_t*>(data), bw, bh, format == kBc3);
    GLint unpackAlignment = 4, unpackRowLength = 0;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &unpackAlignment);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &unpackRowLength);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, GLint(bw));
    glTexSubImage2D(target, level, x, y, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, unpackRowLength);
}

// ---- base instance / base vertex without the extensions
// The vertex array objects' instanced (divisor) and enabled attributes, as the game sets them: the draw then moves the
// bindings of the attributes concerned (ES 3.1: glVertexAttribPointer is a format plus binding i = attribute i).
struct VaoAttribs
{
    uint32_t instanced = 0;
    uint32_t enabled = 0;
};
std::unordered_map<GLuint, VaoAttribs> s_vaos;
GLuint s_vao = 0;
constexpr GLuint kAttribs = 16;

void APIENTRY BindVertexArrayShim(GLuint vao)
{
    s_vao = vao;
    s_bindVertexArray(vao);
}
void APIENTRY DeleteVertexArraysShim(GLsizei n, const GLuint* arrays)
{
    for (GLsizei i = 0; i < n; ++i)
    {
        s_vaos.erase(arrays[i]);
        if (arrays[i] == s_vao) s_vao = 0;
    }
    s_deleteVertexArrays(n, arrays);
}
void APIENTRY VertexAttribDivisorShim(GLuint index, GLuint divisor)
{
    if (index < kAttribs)
    {
        uint32_t& mask = s_vaos[s_vao].instanced;
        mask = divisor ? mask | (1u << index) : mask & ~(1u << index);
    }
    s_vertexAttribDivisor(index, divisor);
}
void APIENTRY EnableVertexAttribArrayShim(GLuint index)
{
    if (index < kAttribs) s_vaos[s_vao].enabled |= 1u << index;
    s_enableVertexAttribArray(index);
}
void APIENTRY DisableVertexAttribArrayShim(GLuint index)
{
    if (index < kAttribs) s_vaos[s_vao].enabled &= ~(1u << index);
    s_disableVertexAttribArray(index);
}

GLint TypeSize(GLenum type)
{
    switch (type)
    {
    case GL_BYTE: case GL_UNSIGNED_BYTE: return 1;
    case GL_SHORT: case GL_UNSIGNED_SHORT: case GL_HALF_FLOAT: return 2;
    default: return 4;
    }
}

struct MovedBinding
{
    GLuint index;
    GLuint buffer;
    GLintptr offset;
    GLsizei stride;
};
// Moves the bindings of the attributes in mask by elements: returns how many it moved (to put back with Restore)
int Move(uint32_t mask, GLint elements, MovedBinding* moved)
{
    int count = 0;
    if (elements == 0) return 0;
    for (GLuint i = 0; mask; ++i, mask >>= 1)
    {
        if (!(mask & 1)) continue;
        GLint buffer = 0, offset = 0, stride = 0;
        glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &buffer);
        if (!buffer) continue;
        glGetIntegeri_v(GL_VERTEX_BINDING_OFFSET, i, &offset);
        glGetIntegeri_v(GL_VERTEX_BINDING_STRIDE, i, &stride);
        if (stride == 0)
        {
            GLint size = 4, type = GL_FLOAT;
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &size);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &type);
            stride = size * TypeSize(GLenum(type));
        }
        moved[count++] = {i, GLuint(buffer), GLintptr(offset), GLsizei(stride)};
        glBindVertexBuffer(i, GLuint(buffer), GLintptr(offset) + GLintptr(elements) * stride, stride);
    }
    return count;
}
void Restore(const MovedBinding* moved, int count)
{
    for (int i = 0; i < count; ++i) glBindVertexBuffer(moved[i].index, moved[i].buffer, moved[i].offset, moved[i].stride);
}
uint32_t InstancedMask() { return s_vaos[s_vao].instanced; }
uint32_t VertexMask() { const VaoAttribs& a = s_vaos[s_vao]; return a.enabled & ~a.instanced; }

void APIENTRY DrawArraysInstancedBaseInstanceShim(GLenum mode, GLint first, GLsizei count, GLsizei instances, GLuint base)
{
    MovedBinding moved[kAttribs];
    const int cmoved = Move(InstancedMask(), GLint(base), moved);
    glDrawArraysInstanced(mode, first, count, instances);
    Restore(moved, cmoved);
}
void APIENTRY DrawElementsInstancedBaseInstanceShim(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                    GLsizei instances, GLuint base)
{
    MovedBinding moved[kAttribs];
    const int cmoved = Move(InstancedMask(), GLint(base), moved);
    glDrawElementsInstanced(mode, count, type, indices, instances);
    Restore(moved, cmoved);
}
// gl_VertexID includes the base vertex of a base-vertex draw, and glob.vert indexes its per-vertex storage (poses,
// warps, cached lighting) with it: moved bindings alone leave it without, so the programs read gl_VertexID plus the
// uniform cane_BaseVertex (TranslateShader), set around each emulated draw and 0 otherwise.
bool s_emulateBaseVertex = false;
constexpr const char* kBaseVertexUniform = "cane_BaseVertex";
std::unordered_map<GLuint, GLint> s_baseVertexLocation;
struct BaseVertexUniform
{
    GLint location = -1;
    explicit BaseVertexUniform(GLint baseVertex)
    {
        if (baseVertex == 0) return;
        GLint program = 0;
        glGetIntegerv(GL_CURRENT_PROGRAM, &program);
        if (!program) return;
        auto it = s_baseVertexLocation.find(GLuint(program));
        if (it == s_baseVertexLocation.end())
            it = s_baseVertexLocation.emplace(GLuint(program), glGetUniformLocation(GLuint(program), kBaseVertexUniform)).first;
        location = it->second;
        if (location >= 0) glUniform1i(location, baseVertex);
    }
    ~BaseVertexUniform()
    {
        if (location >= 0) glUniform1i(location, 0);
    }
};

void APIENTRY DrawElementsInstancedBaseVertexBaseInstanceShim(GLenum mode, GLsizei count, GLenum type, const void* indices,
                                                              GLsizei instances, GLint baseVertex, GLuint base)
{
    MovedBinding moved[kAttribs * 2];
    int cmoved = Move(InstancedMask(), GLint(base), moved);
    if (s_drawElementsInstancedBaseVertex)
        s_drawElementsInstancedBaseVertex(mode, count, type, indices, instances, baseVertex);
    else
    {
        cmoved += Move(VertexMask(), baseVertex, moved + cmoved);
        BaseVertexUniform uniform(baseVertex);
        glDrawElementsInstanced(mode, count, type, indices, instances);
    }
    Restore(moved, cmoved);
}
void APIENTRY DrawElementsBaseVertexShim(GLenum mode, GLsizei count, GLenum type, const void* indices, GLint baseVertex)
{
    MovedBinding moved[kAttribs];
    const int cmoved = Move(VertexMask(), baseVertex, moved);
    BaseVertexUniform uniform(baseVertex);
    glDrawElements(mode, count, type, indices);
    Restore(moved, cmoved);
}

// The ES name of a desktop entry point: as it is, or with an EXT / OES suffix (ES 3.1's extensions to what 3.2 made core)
void* LookupEs(const char* name, bool suffixes)
{
    if (void* p = Lookup(name)) return p;
    if (!suffixes) return nullptr;
    for (const char* suffix : {"EXT", "OES"})
        if (void* p = Lookup((std::string(name) + suffix).c_str())) return p;
    return nullptr;
}
}  // namespace

bool Wanted()
{
#if defined(__ANDROID__)
    return true;
#else
    const char* sz = std::getenv("SLY1_RENDERER");
    if (!sz) return false;
    std::string s;
    for (const char* p = sz; *p; ++p)
        if (*p != ' ' && *p != '-' && *p != '_') s += char(*p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p);
    return s == "gles" || s == "opengles";
#endif
}

void ApplyWindowHints(int minor)
{
#if defined(__ANDROID__)
    (void)minor;   // (platform/android/glfw_sdl.cpp makes an ES 3.2 context, else 3.1 / 3.0)
#else
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, minor);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_ANY_PROFILE);
#endif
}

bool ContextIsEs()
{
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    return version && !std::strncmp(version, "OpenGL ES ", 10);
}

bool Init(std::string& error)
{
    s_active = false;
    const char* version = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    int major = 0, minor = 0;
    if (!version || std::sscanf(version, "OpenGL ES %d.%d", &major, &minor) != 2)
    {
        error = std::string("not an OpenGL ES context (") + (version ? version : "no version") + ")";
        return false;
    }
    if (major < 3 || (major == 3 && minor < 1))
    {
        error = "OpenGL ES 3.1 is required (vertex-stage storage buffers); this device offers " + std::string(version);
        return false;
    }
    s_minor = major > 3 ? 2 : (minor > 2 ? 2 : minor);
    s_name = "OpenGL ES " + std::to_string(major) + "." + std::to_string(minor);

    std::string missing;
#define CANE_ES(fn, suffixes)                                                                       \
    if (void* p = LookupEs(#fn, suffixes)) glad_##fn = reinterpret_cast<decltype(glad_##fn)>(p); \
    else missing += " " #fn;
    // (ES 3.0 / 3.1 / 3.2 core under their desktop names: glad loads the desktop versions up to the 3.x an ES context
    // reports, and nothing of desktop 3.3 - 4.3)
    CANE_ES(glBindSampler, false) CANE_ES(glGenSamplers, false) CANE_ES(glDeleteSamplers, false)
    CANE_ES(glSamplerParameteri, false) CANE_ES(glSamplerParameterf, false) CANE_ES(glVertexAttribDivisor, false)
    CANE_ES(glTexStorage2D, false) CANE_ES(glTexStorage3D, false) CANE_ES(glMemoryBarrier, false)
    CANE_ES(glDepthRangef, false) CANE_ES(glClearDepthf, false) CANE_ES(glBindVertexBuffer, false)
    CANE_ES(glGetIntegeri_v, false) CANE_ES(glGetTexLevelParameteriv, false) CANE_ES(glDrawBuffers, false)
    CANE_ES(glDrawArraysInstanced, false) CANE_ES(glDrawElementsInstanced, false)
    // (ES 3.2, or EXT / OES_draw_buffers_indexed on 3.1: glstate's indexed forms; the game does not call them today)
    CANE_ES(glEnablei, true) CANE_ES(glDisablei, true) CANE_ES(glBlendFunci, true) CANE_ES(glBlendFuncSeparatei, true)
    CANE_ES(glBlendEquationi, true) CANE_ES(glBlendEquationSeparatei, true) CANE_ES(glColorMaski, true)
#undef CANE_ES
    // (the indexed forms are optional: a missing one stays null, as before)
    for (const char* optional : {" glEnablei", " glDisablei", " glBlendFunci", " glBlendFuncSeparatei", " glBlendEquationi",
                                 " glBlendEquationSeparatei", " glColorMaski"})
        if (size_t at = missing.find(optional); at != std::string::npos) missing.erase(at, std::strlen(optional));
    if (!missing.empty())
    {
        error = "OpenGL ES entry points missing:" + missing;
        return false;
    }

    // ---- the desktop-only entry points the game calls
    s_depthRangef = reinterpret_cast<PFNDEPTHRANGEF>(glad_glDepthRangef);
    s_clearDepthf = reinterpret_cast<PFNCLEARDEPTHF>(LookupEs("glClearDepthf", false));
    s_getFloatv = glad_glGetFloatv;
    glad_glClearDepth = ClearDepthShim;
    glad_glDepthRange = DepthRangeShim;
    glad_glPointSize = PointSizeShim;
    glad_glGetFloatv = GetFloatvShim;
    glad_glGetDoublev = GetDoublevShim;
    glad_glDrawBuffer = DrawBufferShim;
    glad_glGetTexImage = GetTexImageShim;
    // (desktop-only names glad may have loaded as stubs: the game must not reach them)
    glad_glPolygonMode = nullptr;

    // ---- base instance, base vertex
    // (test: the paths for drivers without the extensions; SLY1_GLES_EMULATE=1 both, =instance / =vertex one of them)
    const char* szEmulate = std::getenv("SLY1_GLES_EMULATE");
    const std::string emulate = szEmulate ? szEmulate : "";
    const bool fNoNative = emulate == "1" || emulate == "instance";
    const bool fNoNativeVertex = emulate == "1" || emulate == "vertex";
    s_bindVertexArray = glad_glBindVertexArray;
    s_deleteVertexArrays = glad_glDeleteVertexArrays;
    s_vertexAttribDivisor = glad_glVertexAttribDivisor;
    s_enableVertexAttribArray = glad_glEnableVertexAttribArray;
    s_disableVertexAttribArray = glad_glDisableVertexAttribArray;
    if (!fNoNative && HasExtension("GL_EXT_base_instance"))
    {
        s_extDrawArraysBaseInstance = reinterpret_cast<PFNDRAWBASEINSTANCEARRAYS>(Lookup("glDrawArraysInstancedBaseInstanceEXT"));
        s_extDrawElementsBaseInstance = reinterpret_cast<PFNDRAWBASEINSTANCEELEMENTS>(Lookup("glDrawElementsInstancedBaseInstanceEXT"));
        s_extDrawElementsBaseVertexBaseInstance =
            reinterpret_cast<PFNDRAWBASEVERTEXBASEINSTANCE>(Lookup("glDrawElementsInstancedBaseVertexBaseInstanceEXT"));
    }
    const bool fBaseVertex = !fNoNativeVertex && (s_minor >= 2 || HasExtension("GL_EXT_draw_elements_base_vertex") ||
                                            HasExtension("GL_OES_draw_elements_base_vertex"));
    if (fBaseVertex)
    {
        s_drawElementsInstancedBaseVertex = reinterpret_cast<PFNDRAWINSTANCEDBASEVERTEX>(LookupEs("glDrawElementsInstancedBaseVertex", true));
        s_drawElementsBaseVertex = reinterpret_cast<PFNDRAWBASEVERTEX>(LookupEs("glDrawElementsBaseVertex", true));
    }
    const bool fBaseInstance = s_extDrawArraysBaseInstance && s_extDrawElementsBaseInstance && s_extDrawElementsBaseVertexBaseInstance;
    if (fBaseInstance)
    {
        glad_glDrawArraysInstancedBaseInstance = s_extDrawArraysBaseInstance;
        glad_glDrawElementsInstancedBaseInstance = s_extDrawElementsBaseInstance;
        glad_glDrawElementsInstancedBaseVertexBaseInstance = s_extDrawElementsBaseVertexBaseInstance;
    }
    else
    {
        glad_glDrawArraysInstancedBaseInstance = DrawArraysInstancedBaseInstanceShim;
        glad_glDrawElementsInstancedBaseInstance = DrawElementsInstancedBaseInstanceShim;
        glad_glDrawElementsInstancedBaseVertexBaseInstance = DrawElementsInstancedBaseVertexBaseInstanceShim;
    }
    s_emulateBaseVertex = !fBaseVertex;
    s_baseVertexLocation.clear();
    if (s_drawElementsBaseVertex) glad_glDrawElementsBaseVertex = s_drawElementsBaseVertex;
    else glad_glDrawElementsBaseVertex = DrawElementsBaseVertexShim;
    // (the vertex array tracking the moved bindings need: only without the native draws)
    if (!fBaseInstance || !fBaseVertex)
    {
        if (!s_bindVertexArray || !s_deleteVertexArrays || !s_enableVertexAttribArray || !s_disableVertexAttribArray)
        {
            error = "OpenGL ES vertex array entry points missing";
            return false;
        }
        GLint vao = 0;
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
        s_vao = GLuint(vao);
        s_vaos.clear();
        glad_glBindVertexArray = BindVertexArrayShim;
        glad_glDeleteVertexArrays = DeleteVertexArraysShim;
        glad_glVertexAttribDivisor = VertexAttribDivisorShim;
        glad_glEnableVertexAttribArray = EnableVertexAttribArrayShim;
        glad_glDisableVertexAttribArray = DisableVertexAttribArrayShim;
    }

    // ---- BC textures
    // (always decoded, as on Vulkan and Metal: the game makes the mip chain of a one-level BC texture with
    // glGenerateMipmap, which OpenGL ES refuses for compressed formats (GL_INVALID_OPERATION, the textures then have no
    // mips); SLY1_GLES_NATIVEBC=1: EXT_texture_compression_s3tc's native formats where the driver has them, a test)
    s_s3tc = EnvOn("SLY1_GLES_NATIVEBC") && HasExtension("GL_EXT_texture_compression_s3tc");
    if (!s_s3tc)
    {
        s_texStorage2D = glad_glTexStorage2D;
        s_compressedTexSubImage2D = glad_glCompressedTexSubImage2D;
        glad_glTexStorage2D = TexStorage2DShim;
        glad_glCompressedTexSubImage2D = CompressedTexSubImage2DShim;
    }

    // ---- persistently mapped streams
    s_bufferStorage = !EnvOn("SLY1_GLES_NOPERSIST") && HasExtension("GL_EXT_buffer_storage") ? Lookup("glBufferStorageEXT") : nullptr;

    std::printf("OpenGL ES: %s; base instance %s, base vertex %s, BC textures %s, buffer storage %s\n", version,
                fBaseInstance ? "native" : "emulated", fBaseVertex ? "native" : "emulated", s_s3tc ? "native" : "decoded",
                s_bufferStorage ? "yes" : "no");
    s_active = true;
    return true;
}

bool Active() { return s_active; }
const char* Name() { return s_name.c_str(); }
void* BufferStorageProc() { return s_active ? s_bufferStorage : nullptr; }

// Varyings of a struct type (glob.vert / glob.frag: `layout(location = 4) out MATERIAL material;`, one location per
// member) as one varying per member, with a global of the struct that the stage's main reads or fills. Adreno's ES
// compiler (the S26 Ultra's Adreno 840, driver 0842.19, 2026-11-24) refuses the struct varying itself at link time
// ("output material.light type mismatch with other stage") although both stages declare it identically; ANGLE and
// Mesa take it. Same values, same locations.
static std::string SplitStructVaryings(std::string text)
{
    struct Split { std::string name, qualifier; bool output; std::vector<std::pair<std::string, std::string>> members; };
    std::vector<Split> splits;
    for (size_t at = 0; (at = text.find("layout(location", at)) != std::string::npos;)
    {
        const size_t end = text.find(';', at);
        const size_t close = text.find(')', at);
        if (end == std::string::npos || close == std::string::npos || close > end) break;
        int location = -1;
        if (std::sscanf(text.c_str() + at, "layout(location = %d)", &location) != 1 &&
            std::sscanf(text.c_str() + at, "layout(location=%d)", &location) != 1) { at = end; continue; }
        // the words after the layout: [flat] in|out TYPE NAME
        std::vector<std::string> words;
        for (size_t i = close + 1; i < end;)
        {
            while (i < end && (text[i] == ' ' || text[i] == '\t')) ++i;
            size_t j = i;
            while (j < end && text[j] != ' ' && text[j] != '\t') ++j;
            if (j > i) words.push_back(text.substr(i, j - i));
            i = j;
        }
        std::string qualifier;
        if (!words.empty() && (words[0] == "flat" || words[0] == "smooth")) { qualifier = words[0] + " "; words.erase(words.begin()); }
        if (words.size() != 3 || (words[0] != "in" && words[0] != "out")) { at = end; continue; }
        const std::string& type = words[1];
        const size_t def = text.find("struct " + type);
        const size_t open = def == std::string::npos ? def : text.find('{', def);
        const size_t shut = open == std::string::npos ? open : text.find('}', open);
        if (shut == std::string::npos || shut > at) { at = end; continue; }   // (not a struct declared before it)
        Split split{words[2], qualifier, words[0] == "out", {}};
        for (size_t i = open + 1; i < shut;)
        {
            size_t semi = text.find(';', i);
            if (semi == std::string::npos || semi > shut) break;
            std::string decl = text.substr(i, semi - i);
            for (size_t c; (c = decl.find("//")) != std::string::npos;)
            {
                const size_t nl = decl.find('\n', c);
                decl.erase(c, nl == std::string::npos ? std::string::npos : nl - c);
            }
            std::vector<std::string> parts;
            for (size_t k = 0; k < decl.size();)
            {
                while (k < decl.size() && std::isspace(static_cast<unsigned char>(decl[k]))) ++k;
                size_t m = k;
                while (m < decl.size() && !std::isspace(static_cast<unsigned char>(decl[m]))) ++m;
                if (m > k) parts.push_back(decl.substr(k, m - k));
                k = m;
            }
            if (parts.size() != 2) { split.members.clear(); break; }   // (arrays, several names: left as they are)
            split.members.emplace_back(parts[0], parts[1]);
            i = semi + 1;
        }
        if (split.members.empty()) { at = end; continue; }
        std::string replacement = type + " " + split.name + ";";
        for (size_t m = 0; m < split.members.size(); ++m)
            replacement += "\nlayout(location = " + std::to_string(location + int(m)) + ") " + qualifier + words[0] + " " +
                           split.members[m].first + " cane_" + split.name + "_" + split.members[m].second + ";";
        text.replace(at, end + 1 - at, replacement);
        at += replacement.size();
        splits.push_back(split);
    }
    if (splits.empty()) return text;
    const size_t main = text.find("void main()");
    if (main == std::string::npos) return text;
    text.replace(main, 11, "void cane_main_()");
    std::string before, after;
    for (const Split& split : splits)
        for (const auto& member : split.members)
        {
            const std::string varying = "cane_" + split.name + "_" + member.second, field = split.name + "." + member.second;
            if (split.output) after += "    " + varying + " = " + field + ";\n";
            else before += "    " + field + " = " + varying + ";\n";
        }
    text += "\nvoid main()\n{\n" + before + "    cane_main_();\n" + after + "}\n";
    return text;
}

std::string TranslateShader(const std::string& source)
{
    size_t v = source.find("#version");
    const size_t body = v == std::string::npos ? 0 : source.find('\n', v) + 1;
    std::string header = s_minor >= 2 ? "#version 320 es\n" : "#version 310 es\n";
    header += "precision highp float;\nprecision highp int;\nprecision highp sampler2D;\nprecision highp sampler2DArray;\n"
              "precision highp usampler2D;\nprecision highp isampler2D;\n#define CANE_GLES 1\n";
    // GLSL 4.20+ allows `const` locals with run-time initializers; GLSL ES wants constant expressions there.
    // Function-scope (indented) declarations lose the qualifier (as Sly 2's platform/android/gles_sly2.cpp).
    std::string text = SplitStructVaryings(body == 0 && v == std::string::npos ? source : source.substr(body));
    // (without native base vertex: gl_VertexID with the draw's base vertex, the uniform the emulated draws set)
    if (s_emulateBaseVertex && text.find("gl_VertexID") != std::string::npos)
    {
        std::string replaced;
        for (size_t at = 0;;)
        {
            const size_t hit = text.find("gl_VertexID", at);
            replaced += text.substr(at, hit == std::string::npos ? std::string::npos : hit - at);
            if (hit == std::string::npos) break;
            replaced += std::string("(gl_VertexID + ") + kBaseVertexUniform + ")";
            at = hit + 11;
        }
        text = std::string("uniform int ") + kBaseVertexUniform + ";\n" + replaced;
    }
    std::string out;
    out.reserve(text.size() + header.size());
    for (size_t at = 0; at < text.size();)
    {
        size_t end = text.find('\n', at);
        end = end == std::string::npos ? text.size() : end + 1;
        std::string line = text.substr(at, end - at);
        const size_t indent = line.find_first_not_of(" \t");
        if (indent != std::string::npos && indent > 0 && line.compare(indent, 6, "const ") == 0) line.erase(indent, 6);
        out += line;
        at = end;
    }
    return (v == std::string::npos ? std::string() : source.substr(0, v)) + header + out;
}

unsigned ImguiProgram()
{
    static const char* const kVertex = "#version 300 es\nprecision highp float;\nuniform mat4 ProjMtx;\n"
        "layout(location = 0) in vec2 Position;\nlayout(location = 1) in vec2 UV;\nlayout(location = 2) in vec4 Color;\n"
        "out vec2 Frag_UV;\nout vec4 Frag_Color;\n"
        "void main() { Frag_UV = UV; Frag_Color = Color; gl_Position = ProjMtx * vec4(Position.xy, 0.0, 1.0); }\n";
    static const char* const kFragment = "#version 300 es\nprecision mediump float;\nuniform sampler2D Texture;\n"
        "in vec2 Frag_UV;\nin vec4 Frag_Color;\nlayout(location = 0) out vec4 Out_Color;\n"
        "void main() { Out_Color = Frag_Color * texture(Texture, Frag_UV.st); }\n";
    auto compile = [](GLenum type, const char* source) -> GLuint {
        const GLuint shader = glCreateShader(type);
        glShaderSource(shader, 1, &source, nullptr);
        glCompileShader(shader);
        GLint ok = GL_FALSE;
        glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
        if (!ok)
        {
            char log[512] = {};
            glGetShaderInfoLog(shader, sizeof(log), nullptr, log);
            std::printf("OpenGL ES: the ImGui program: %s\n", log);
            glDeleteShader(shader);
            return 0;
        }
        return shader;
    };
    const GLuint vs = compile(GL_VERTEX_SHADER, kVertex), fs = compile(GL_FRAGMENT_SHADER, kFragment);
    GLuint program = 0;
    if (vs && fs)
    {
        program = glCreateProgram();
        glAttachShader(program, vs);
        glAttachShader(program, fs);
        glLinkProgram(program);
        GLint ok = GL_FALSE;
        glGetProgramiv(program, GL_LINK_STATUS, &ok);
        if (!ok)
        {
            glDeleteProgram(program);
            program = 0;
        }
    }
    if (vs) glDeleteShader(vs);
    if (fs) glDeleteShader(fs);
    return program;
}
}  // namespace cane::gles1
