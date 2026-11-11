#include "gl_metal.h"

#include <glad/glad.h>

#include <cstring>
#include <memory>
#include <type_traits>
#include <unordered_map>

#include "context.h"

namespace cane::gl_metal {

namespace {

std::unique_ptr<Context> g_context;

Context* Ctx() { return g_context.get(); }

}  // namespace
}  // namespace cane::gl_metal

using cane::gl_metal::Ctx;
using cane::gl_metal::GLenum;

namespace {

// Every entry point below is a free function with the exact signature of
// glad's typedef; `kProcTable` maps names to them for glad's loader.

GLenum APIENTRY mtl_glGetError() { return Ctx() ? Ctx()->TakeError() : GL_NO_ERROR; }

const GLubyte* APIENTRY mtl_glGetString(GLenum name) {
  const char* value = Ctx() ? Ctx()->GetString(name) : nullptr;
  return reinterpret_cast<const GLubyte*>(value ? value : "");
}

const GLubyte* APIENTRY mtl_glGetStringi(GLenum name, GLuint index) {
  const char* value = Ctx() ? Ctx()->GetStringIndexed(name, index) : nullptr;
  return reinterpret_cast<const GLubyte*>(value ? value : "");
}

void APIENTRY mtl_glGetIntegerv(GLenum pname, GLint* data) {
  if (!Ctx()->GetInteger(pname, data)) Ctx()->SetError(GL_INVALID_ENUM, "GetInteger", pname);
}

void APIENTRY mtl_glGetFloatv(GLenum pname, GLfloat* data) {
  if (!Ctx()->GetFloat(pname, data)) Ctx()->SetError(GL_INVALID_ENUM, "GetFloat", pname);
}

void APIENTRY mtl_glEnable(GLenum cap) { Ctx()->SetCapability(cap, true); }
void APIENTRY mtl_glDisable(GLenum cap) { Ctx()->SetCapability(cap, false); }

void APIENTRY mtl_glViewport(GLint x, GLint y, GLsizei w, GLsizei h) {
  Ctx()->Viewport(x, y, w, h);
}

void APIENTRY mtl_glScissor(GLint x, GLint y, GLsizei w, GLsizei h) { Ctx()->Scissor(x, y, w, h); }

void APIENTRY mtl_glClearColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
  Ctx()->ClearColor(r, g, b, a);
}

void APIENTRY mtl_glClearDepth(GLdouble depth) { Ctx()->ClearDepth(depth); }
void APIENTRY mtl_glClearStencil(GLint s) { Ctx()->ClearStencil(s); }
void APIENTRY mtl_glClear(GLbitfield mask) { Ctx()->Clear(mask); }

void APIENTRY mtl_glColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a) {
  Ctx()->ColorMask(r, g, b, a);
}

void APIENTRY mtl_glDepthMask(GLboolean flag) { Ctx()->DepthMask(flag); }
void APIENTRY mtl_glDepthFunc(GLenum func) { Ctx()->DepthFunc(func); }
void APIENTRY mtl_glStencilMask(GLuint mask) { Ctx()->StencilMask(mask); }

void APIENTRY mtl_glStencilFunc(GLenum func, GLint ref, GLuint mask) {
  Ctx()->StencilFunc(func, ref, mask);
}

void APIENTRY mtl_glStencilOp(GLenum fail, GLenum zfail, GLenum zpass) {
  Ctx()->StencilOpSeparate(GL_FRONT_AND_BACK, fail, zfail, zpass);
}

void APIENTRY mtl_glStencilOpSeparate(GLenum face, GLenum sfail, GLenum dpfail, GLenum dppass) {
  Ctx()->StencilOpSeparate(face, sfail, dpfail, dppass);
}

void APIENTRY mtl_glBlendFunc(GLenum s, GLenum d) { Ctx()->BlendFuncSeparate(s, d, s, d); }

void APIENTRY mtl_glBlendFuncSeparate(GLenum srgb, GLenum drgb, GLenum sa, GLenum da) {
  Ctx()->BlendFuncSeparate(srgb, drgb, sa, da);
}

void APIENTRY mtl_glBlendEquation(GLenum mode) { Ctx()->BlendEquationSeparate(mode, mode); }

void APIENTRY mtl_glBlendEquationSeparate(GLenum rgb, GLenum alpha) {
  Ctx()->BlendEquationSeparate(rgb, alpha);
}

void APIENTRY mtl_glBlendColor(GLfloat r, GLfloat g, GLfloat b, GLfloat a) {
  Ctx()->BlendColor(r, g, b, a);
}

void APIENTRY mtl_glCullFace(GLenum mode) { Ctx()->CullFace(mode); }
void APIENTRY mtl_glFrontFace(GLenum mode) { Ctx()->FrontFace(mode); }
void APIENTRY mtl_glLineWidth(GLfloat width) { Ctx()->LineWidth(width); }
void APIENTRY mtl_glPixelStorei(GLenum pname, GLint param) { Ctx()->PixelStorei(pname, param); }
void APIENTRY mtl_glFinish() { Ctx()->Finish(); }
void APIENTRY mtl_glFlush() { Ctx()->Flush(); }
void APIENTRY mtl_glMemoryBarrier(GLbitfield) {}
void APIENTRY mtl_glDrawBuffers(GLsizei, const GLenum*) {}
void APIENTRY mtl_glReadBuffer(GLenum) {}

void APIENTRY mtl_glGenBuffers(GLsizei n, GLuint* buffers) { Ctx()->GenBuffers(n, buffers); }

void APIENTRY mtl_glDeleteBuffers(GLsizei n, const GLuint* buffers) {
  Ctx()->DeleteBuffers(n, buffers);
}

void APIENTRY mtl_glBindBuffer(GLenum target, GLuint buffer) { Ctx()->BindBuffer(target, buffer); }

void APIENTRY mtl_glBindBufferBase(GLenum target, GLuint index, GLuint buffer) {
  Ctx()->BindBufferRange(target, index, buffer, 0, 0);
}

void APIENTRY mtl_glBindBufferRange(GLenum target, GLuint index, GLuint buffer, GLintptr offset,
                                    GLsizeiptr size) {
  Ctx()->BindBufferRange(target, index, buffer, static_cast<size_t>(offset),
                         static_cast<size_t>(size));
}

void APIENTRY mtl_glBufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage) {
  Ctx()->BufferData(target, static_cast<size_t>(size), data, usage);
}

void APIENTRY mtl_glBufferSubData(GLenum target, GLintptr offset, GLsizeiptr size,
                                  const void* data) {
  Ctx()->BufferSubData(target, static_cast<size_t>(offset), static_cast<size_t>(size), data);
}

void* APIENTRY mtl_glMapBufferRange(GLenum target, GLintptr offset, GLsizeiptr length,
                                    GLbitfield access) {
  return Ctx()->MapBufferRange(target, static_cast<size_t>(offset), static_cast<size_t>(length),
                               access);
}

GLboolean APIENTRY mtl_glUnmapBuffer(GLenum target) {
  return Ctx()->UnmapBuffer(target) ? GL_TRUE : GL_FALSE;
}

void APIENTRY mtl_glGenTextures(GLsizei n, GLuint* textures) { Ctx()->GenTextures(n, textures); }

void APIENTRY mtl_glDeleteTextures(GLsizei n, const GLuint* textures) {
  Ctx()->DeleteTextures(n, textures);
}

void APIENTRY mtl_glActiveTexture(GLenum texture) { Ctx()->ActiveTexture(texture); }
void APIENTRY mtl_glBindTexture(GLenum target, GLuint texture) {
  Ctx()->BindTexture(target, texture);
}

void APIENTRY mtl_glTexImage2D(GLenum target, GLint level, GLint internalformat, GLsizei width,
                               GLsizei height, GLint, GLenum format, GLenum type,
                               const void* pixels) {
  Ctx()->TexImage2D(target, level, static_cast<GLenum>(internalformat), width, height, format, type,
                    pixels);
}

void APIENTRY mtl_glTexStorage2D(GLenum target, GLsizei levels, GLenum internalformat,
                                 GLsizei width, GLsizei height) {
  Ctx()->TexStorage2D(target, levels, internalformat, width, height);
}

void APIENTRY mtl_glTexStorage3D(GLenum target, GLsizei levels, GLenum internalformat,
                                 GLsizei width, GLsizei height, GLsizei depth) {
  Ctx()->TexStorage3D(target, levels, internalformat, width, height, depth);
}

void APIENTRY mtl_glTexSubImage2D(GLenum target, GLint level, GLint x, GLint y, GLsizei width,
                                  GLsizei height, GLenum format, GLenum type, const void* pixels) {
  Ctx()->TexSubImage2D(target, level, x, y, width, height, format, type, pixels);
}

void APIENTRY mtl_glTexSubImage3D(GLenum target, GLint level, GLint x, GLint y, GLint z,
                                  GLsizei width, GLsizei height, GLsizei depth, GLenum format,
                                  GLenum type, const void* pixels) {
  Ctx()->TexSubImage3D(target, level, x, y, z, width, height, depth, format, type, pixels);
}

void APIENTRY mtl_glTexParameteri(GLenum target, GLenum pname, GLint param) {
  Ctx()->TexParameteri(target, pname, param);
}

void APIENTRY mtl_glGenerateMipmap(GLenum target) { Ctx()->GenerateMipmap(target); }
void APIENTRY mtl_glGenSamplers(GLsizei n, GLuint* samplers) { Ctx()->GenSamplers(n, samplers); }

void APIENTRY mtl_glDeleteSamplers(GLsizei n, const GLuint* samplers) {
  Ctx()->DeleteSamplers(n, samplers);
}

void APIENTRY mtl_glBindSampler(GLuint unit, GLuint sampler) { Ctx()->BindSampler(unit, sampler); }

void APIENTRY mtl_glSamplerParameteri(GLuint sampler, GLenum pname, GLint param) {
  Ctx()->SamplerParameteri(sampler, pname, param);
}

void APIENTRY mtl_glSamplerParameterf(GLuint sampler, GLenum pname, GLfloat param) {
  Ctx()->SamplerParameterf(sampler, pname, param);
}

void APIENTRY mtl_glGenRenderbuffers(GLsizei n, GLuint* names) {
  Ctx()->GenRenderbuffers(n, names);
}

void APIENTRY mtl_glDeleteRenderbuffers(GLsizei n, const GLuint* names) {
  Ctx()->DeleteRenderbuffers(n, names);
}

void APIENTRY mtl_glBindRenderbuffer(GLenum target, GLuint name) {
  Ctx()->BindRenderbuffer(target, name);
}

void APIENTRY mtl_glRenderbufferStorage(GLenum target, GLenum internalformat, GLsizei width,
                                        GLsizei height) {
  Ctx()->RenderbufferStorage(target, 1, internalformat, width, height);
}

void APIENTRY mtl_glRenderbufferStorageMultisample(GLenum target, GLsizei samples,
                                                   GLenum internalformat, GLsizei width,
                                                   GLsizei height) {
  Ctx()->RenderbufferStorage(target, samples, internalformat, width, height);
}

void APIENTRY mtl_glGenVertexArrays(GLsizei n, GLuint* arrays) {
  Ctx()->GenVertexArrays(n, arrays);
}

void APIENTRY mtl_glDeleteVertexArrays(GLsizei n, const GLuint* arrays) {
  Ctx()->DeleteVertexArrays(n, arrays);
}

void APIENTRY mtl_glBindVertexArray(GLuint array) { Ctx()->BindVertexArray(array); }

void APIENTRY mtl_glEnableVertexAttribArray(GLuint index) {
  Ctx()->EnableVertexAttribArray(index, true);
}

void APIENTRY mtl_glDisableVertexAttribArray(GLuint index) {
  Ctx()->EnableVertexAttribArray(index, false);
}

void APIENTRY mtl_glVertexAttribPointer(GLuint index, GLint size, GLenum type, GLboolean normalized,
                                        GLsizei stride, const void* pointer) {
  Ctx()->VertexAttribPointer(index, size, type, normalized == GL_TRUE, false, stride,
                             reinterpret_cast<size_t>(pointer));
}

void APIENTRY mtl_glVertexAttribIPointer(GLuint index, GLint size, GLenum type, GLsizei stride,
                                         const void* pointer) {
  Ctx()->VertexAttribPointer(index, size, type, false, true, stride,
                             reinterpret_cast<size_t>(pointer));
}

void APIENTRY mtl_glVertexAttribDivisor(GLuint index, GLuint divisor) {
  Ctx()->VertexAttribDivisor(index, static_cast<int>(divisor));
}

void APIENTRY mtl_glGenFramebuffers(GLsizei n, GLuint* names) { Ctx()->GenFramebuffers(n, names); }

void APIENTRY mtl_glDeleteFramebuffers(GLsizei n, const GLuint* names) {
  Ctx()->DeleteFramebuffers(n, names);
}

void APIENTRY mtl_glBindFramebuffer(GLenum target, GLuint name) {
  Ctx()->BindFramebuffer(target, name);
}

void APIENTRY mtl_glFramebufferTexture2D(GLenum target, GLenum attachment, GLenum, GLuint texture,
                                         GLint level) {
  Ctx()->FramebufferTexture2D(target, attachment, texture, level);
}

void APIENTRY mtl_glFramebufferRenderbuffer(GLenum target, GLenum attachment, GLenum,
                                            GLuint renderbuffer) {
  Ctx()->FramebufferRenderbuffer(target, attachment, renderbuffer);
}

GLenum APIENTRY mtl_glCheckFramebufferStatus(GLenum target) {
  return Ctx()->CheckFramebufferStatus(target);
}

void APIENTRY mtl_glBlitFramebuffer(GLint sx0, GLint sy0, GLint sx1, GLint sy1, GLint dx0,
                                    GLint dy0, GLint dx1, GLint dy1, GLbitfield mask,
                                    GLenum filter) {
  Ctx()->BlitFramebuffer(sx0, sy0, sx1, sy1, dx0, dy0, dx1, dy1, mask, filter);
}

void APIENTRY mtl_glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height, GLenum format,
                               GLenum type, void* pixels) {
  Ctx()->ReadPixels(x, y, width, height, format, type, pixels);
}

GLuint APIENTRY mtl_glCreateShader(GLenum type) { return Ctx()->CreateShader(type); }
void APIENTRY mtl_glDeleteShader(GLuint shader) { Ctx()->DeleteShader(shader); }

void APIENTRY mtl_glShaderSource(GLuint shader, GLsizei count, const GLchar* const* string,
                                 const GLint* length) {
  Ctx()->ShaderSource(shader, count, string, length);
}

void APIENTRY mtl_glCompileShader(GLuint shader) { Ctx()->CompileShader(shader); }

void APIENTRY mtl_glGetShaderiv(GLuint shader, GLenum pname, GLint* params) {
  if (!Ctx()->GetShaderInt(shader, pname, params)) Ctx()->SetError(GL_INVALID_ENUM, "GetShaderInt", pname);
}

void APIENTRY mtl_glGetShaderInfoLog(GLuint shader, GLsizei bufSize, GLsizei* length,
                                     GLchar* infoLog) {
  Ctx()->GetShaderInfoLog(shader, bufSize, length, infoLog);
}

GLuint APIENTRY mtl_glCreateProgram() { return Ctx()->CreateProgram(); }
void APIENTRY mtl_glDeleteProgram(GLuint program) { Ctx()->DeleteProgram(program); }

void APIENTRY mtl_glAttachShader(GLuint program, GLuint shader) {
  Ctx()->AttachShader(program, shader);
}

void APIENTRY mtl_glLinkProgram(GLuint program) { Ctx()->LinkProgram(program); }

void APIENTRY mtl_glGetProgramiv(GLuint program, GLenum pname, GLint* params) {
  if (!Ctx()->GetProgramInt(program, pname, params)) Ctx()->SetError(GL_INVALID_ENUM, "GetProgramInt", pname);
}

void APIENTRY mtl_glGetProgramInfoLog(GLuint program, GLsizei bufSize, GLsizei* length,
                                      GLchar* infoLog) {
  Ctx()->GetProgramInfoLog(program, bufSize, length, infoLog);
}

void APIENTRY mtl_glUseProgram(GLuint program) { Ctx()->UseProgram(program); }

GLint APIENTRY mtl_glGetUniformLocation(GLuint program, const GLchar* name) {
  return Ctx()->GetUniformLocation(program, name);
}

void APIENTRY mtl_glUniform1i(GLint location, GLint v0) { Ctx()->UniformInts(location, 1, &v0); }

void APIENTRY mtl_glUniform1iv(GLint location, GLsizei count, const GLint* value) {
  Ctx()->UniformInts(location, count, value);
}

void APIENTRY mtl_glUniform1f(GLint location, GLfloat v0) {
  Ctx()->UniformFloats(location, 1, &v0);
}

void APIENTRY mtl_glUniform2f(GLint location, GLfloat v0, GLfloat v1) {
  const GLfloat v[2] = {v0, v1};
  Ctx()->UniformFloats(location, 2, v);
}

void APIENTRY mtl_glUniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2) {
  const GLfloat v[3] = {v0, v1, v2};
  Ctx()->UniformFloats(location, 3, v);
}

void APIENTRY mtl_glUniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) {
  const GLfloat v[4] = {v0, v1, v2, v3};
  Ctx()->UniformFloats(location, 4, v);
}

void APIENTRY mtl_glUniform2fv(GLint location, GLsizei count, const GLfloat* value) {
  Ctx()->UniformFloats(location, 2 * count, value);
}

void APIENTRY mtl_glUniform3fv(GLint location, GLsizei count, const GLfloat* value) {
  Ctx()->UniformFloats(location, 3 * count, value);
}

void APIENTRY mtl_glUniform4fv(GLint location, GLsizei count, const GLfloat* value) {
  Ctx()->UniformFloats(location, 4 * count, value);
}

void APIENTRY mtl_glUniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose,
                                     const GLfloat* value) {
  Ctx()->UniformMatrix4(location, count, transpose == GL_TRUE, value);
}

void APIENTRY mtl_glDrawArrays(GLenum mode, GLint first, GLsizei count) {
  Ctx()->DrawArrays(mode, first, count);
}

void APIENTRY mtl_glDrawElements(GLenum mode, GLsizei count, GLenum type, const void* indices) {
  Ctx()->DrawElementsInstancedBaseVertexBaseInstance(mode, count, type,
                                                     reinterpret_cast<size_t>(indices), 1, 0, 0);
}

void APIENTRY mtl_glDrawElementsInstancedBaseVertexBaseInstance(GLenum mode, GLsizei count,
                                                                GLenum type, const void* indices,
                                                                GLsizei instancecount,
                                                                GLint basevertex,
                                                                GLuint baseinstance) {
  Ctx()->DrawElementsInstancedBaseVertexBaseInstance(mode, count, type,
                                                     reinterpret_cast<size_t>(indices),
                                                     instancecount, basevertex, baseinstance);
}

void APIENTRY mtl_glMultiDrawElementsIndirect(GLenum mode, GLenum type, const void* indirect,
                                              GLsizei drawcount, GLsizei stride) {
  Ctx()->MultiDrawElementsIndirect(mode, type, reinterpret_cast<size_t>(indirect), drawcount,
                                   stride);
}

void APIENTRY mtl_glGenQueries(GLsizei n, GLuint* ids) { Ctx()->GenQueries(n, ids); }
void APIENTRY mtl_glDeleteQueries(GLsizei n, const GLuint* ids) { Ctx()->DeleteQueries(n, ids); }
void APIENTRY mtl_glBeginQuery(GLenum target, GLuint id) { Ctx()->BeginQuery(target, id); }
void APIENTRY mtl_glEndQuery(GLenum target) { Ctx()->EndQuery(target); }

void APIENTRY mtl_glGetQueryObjectui64v(GLuint id, GLenum pname, GLuint64* params) {
  uint64_t value = 0;
  if (Ctx()->GetQueryResult(id, pname, &value)) *params = value;
}

GLsync APIENTRY mtl_glFenceSync(GLenum, GLbitfield) {
  return static_cast<GLsync>(Ctx()->FenceSync());
}

void APIENTRY mtl_glDeleteSync(GLsync sync) { Ctx()->DeleteSync(sync); }

GLenum APIENTRY mtl_glClientWaitSync(GLsync sync, GLbitfield flags, GLuint64 timeout) {
  return Ctx()->ClientWaitSync(sync, flags, timeout);
}

void APIENTRY mtl_glClipControl(GLenum origin, GLenum depth) { Ctx()->ClipControl(origin, depth); }

#define CANE_CHECK(name, typedef_name) \
  static_assert(std::is_same_v<decltype(&mtl_##name), typedef_name>, #name " signature")

CANE_CHECK(glGetError, PFNGLGETERRORPROC);
CANE_CHECK(glGetString, PFNGLGETSTRINGPROC);
CANE_CHECK(glGetStringi, PFNGLGETSTRINGIPROC);
CANE_CHECK(glGetIntegerv, PFNGLGETINTEGERVPROC);
CANE_CHECK(glGetFloatv, PFNGLGETFLOATVPROC);
CANE_CHECK(glEnable, PFNGLENABLEPROC);
CANE_CHECK(glDisable, PFNGLDISABLEPROC);
CANE_CHECK(glViewport, PFNGLVIEWPORTPROC);
CANE_CHECK(glScissor, PFNGLSCISSORPROC);
CANE_CHECK(glClearColor, PFNGLCLEARCOLORPROC);
CANE_CHECK(glClearDepth, PFNGLCLEARDEPTHPROC);
CANE_CHECK(glClearStencil, PFNGLCLEARSTENCILPROC);
CANE_CHECK(glClear, PFNGLCLEARPROC);
CANE_CHECK(glColorMask, PFNGLCOLORMASKPROC);
CANE_CHECK(glDepthMask, PFNGLDEPTHMASKPROC);
CANE_CHECK(glDepthFunc, PFNGLDEPTHFUNCPROC);
CANE_CHECK(glStencilMask, PFNGLSTENCILMASKPROC);
CANE_CHECK(glStencilFunc, PFNGLSTENCILFUNCPROC);
CANE_CHECK(glStencilOp, PFNGLSTENCILOPPROC);
CANE_CHECK(glStencilOpSeparate, PFNGLSTENCILOPSEPARATEPROC);
CANE_CHECK(glBlendFunc, PFNGLBLENDFUNCPROC);
CANE_CHECK(glBlendFuncSeparate, PFNGLBLENDFUNCSEPARATEPROC);
CANE_CHECK(glBlendEquation, PFNGLBLENDEQUATIONPROC);
CANE_CHECK(glBlendEquationSeparate, PFNGLBLENDEQUATIONSEPARATEPROC);
CANE_CHECK(glBlendColor, PFNGLBLENDCOLORPROC);
CANE_CHECK(glCullFace, PFNGLCULLFACEPROC);
CANE_CHECK(glFrontFace, PFNGLFRONTFACEPROC);
CANE_CHECK(glLineWidth, PFNGLLINEWIDTHPROC);
CANE_CHECK(glPixelStorei, PFNGLPIXELSTOREIPROC);
CANE_CHECK(glFinish, PFNGLFINISHPROC);
CANE_CHECK(glFlush, PFNGLFLUSHPROC);
CANE_CHECK(glMemoryBarrier, PFNGLMEMORYBARRIERPROC);
CANE_CHECK(glDrawBuffers, PFNGLDRAWBUFFERSPROC);
CANE_CHECK(glReadBuffer, PFNGLREADBUFFERPROC);
CANE_CHECK(glGenBuffers, PFNGLGENBUFFERSPROC);
CANE_CHECK(glDeleteBuffers, PFNGLDELETEBUFFERSPROC);
CANE_CHECK(glBindBuffer, PFNGLBINDBUFFERPROC);
CANE_CHECK(glBindBufferBase, PFNGLBINDBUFFERBASEPROC);
CANE_CHECK(glBindBufferRange, PFNGLBINDBUFFERRANGEPROC);
CANE_CHECK(glBufferData, PFNGLBUFFERDATAPROC);
CANE_CHECK(glBufferSubData, PFNGLBUFFERSUBDATAPROC);
CANE_CHECK(glMapBufferRange, PFNGLMAPBUFFERRANGEPROC);
CANE_CHECK(glUnmapBuffer, PFNGLUNMAPBUFFERPROC);
CANE_CHECK(glGenTextures, PFNGLGENTEXTURESPROC);
CANE_CHECK(glDeleteTextures, PFNGLDELETETEXTURESPROC);
CANE_CHECK(glActiveTexture, PFNGLACTIVETEXTUREPROC);
CANE_CHECK(glBindTexture, PFNGLBINDTEXTUREPROC);
CANE_CHECK(glTexImage2D, PFNGLTEXIMAGE2DPROC);
CANE_CHECK(glTexStorage2D, PFNGLTEXSTORAGE2DPROC);
CANE_CHECK(glTexStorage3D, PFNGLTEXSTORAGE3DPROC);
CANE_CHECK(glTexSubImage2D, PFNGLTEXSUBIMAGE2DPROC);
CANE_CHECK(glTexSubImage3D, PFNGLTEXSUBIMAGE3DPROC);
CANE_CHECK(glTexParameteri, PFNGLTEXPARAMETERIPROC);
CANE_CHECK(glGenerateMipmap, PFNGLGENERATEMIPMAPPROC);
CANE_CHECK(glGenSamplers, PFNGLGENSAMPLERSPROC);
CANE_CHECK(glDeleteSamplers, PFNGLDELETESAMPLERSPROC);
CANE_CHECK(glBindSampler, PFNGLBINDSAMPLERPROC);
CANE_CHECK(glSamplerParameteri, PFNGLSAMPLERPARAMETERIPROC);
CANE_CHECK(glSamplerParameterf, PFNGLSAMPLERPARAMETERFPROC);
CANE_CHECK(glGenRenderbuffers, PFNGLGENRENDERBUFFERSPROC);
CANE_CHECK(glDeleteRenderbuffers, PFNGLDELETERENDERBUFFERSPROC);
CANE_CHECK(glBindRenderbuffer, PFNGLBINDRENDERBUFFERPROC);
CANE_CHECK(glRenderbufferStorage, PFNGLRENDERBUFFERSTORAGEPROC);
CANE_CHECK(glRenderbufferStorageMultisample, PFNGLRENDERBUFFERSTORAGEMULTISAMPLEPROC);
CANE_CHECK(glGenVertexArrays, PFNGLGENVERTEXARRAYSPROC);
CANE_CHECK(glDeleteVertexArrays, PFNGLDELETEVERTEXARRAYSPROC);
CANE_CHECK(glBindVertexArray, PFNGLBINDVERTEXARRAYPROC);
CANE_CHECK(glEnableVertexAttribArray, PFNGLENABLEVERTEXATTRIBARRAYPROC);
CANE_CHECK(glDisableVertexAttribArray, PFNGLDISABLEVERTEXATTRIBARRAYPROC);
CANE_CHECK(glVertexAttribPointer, PFNGLVERTEXATTRIBPOINTERPROC);
CANE_CHECK(glVertexAttribIPointer, PFNGLVERTEXATTRIBIPOINTERPROC);
CANE_CHECK(glVertexAttribDivisor, PFNGLVERTEXATTRIBDIVISORPROC);
CANE_CHECK(glGenFramebuffers, PFNGLGENFRAMEBUFFERSPROC);
CANE_CHECK(glDeleteFramebuffers, PFNGLDELETEFRAMEBUFFERSPROC);
CANE_CHECK(glBindFramebuffer, PFNGLBINDFRAMEBUFFERPROC);
CANE_CHECK(glFramebufferTexture2D, PFNGLFRAMEBUFFERTEXTURE2DPROC);
CANE_CHECK(glFramebufferRenderbuffer, PFNGLFRAMEBUFFERRENDERBUFFERPROC);
CANE_CHECK(glCheckFramebufferStatus, PFNGLCHECKFRAMEBUFFERSTATUSPROC);
CANE_CHECK(glBlitFramebuffer, PFNGLBLITFRAMEBUFFERPROC);
CANE_CHECK(glReadPixels, PFNGLREADPIXELSPROC);
CANE_CHECK(glCreateShader, PFNGLCREATESHADERPROC);
CANE_CHECK(glDeleteShader, PFNGLDELETESHADERPROC);
CANE_CHECK(glShaderSource, PFNGLSHADERSOURCEPROC);
CANE_CHECK(glCompileShader, PFNGLCOMPILESHADERPROC);
CANE_CHECK(glGetShaderiv, PFNGLGETSHADERIVPROC);
CANE_CHECK(glGetShaderInfoLog, PFNGLGETSHADERINFOLOGPROC);
CANE_CHECK(glCreateProgram, PFNGLCREATEPROGRAMPROC);
CANE_CHECK(glDeleteProgram, PFNGLDELETEPROGRAMPROC);
CANE_CHECK(glAttachShader, PFNGLATTACHSHADERPROC);
CANE_CHECK(glLinkProgram, PFNGLLINKPROGRAMPROC);
CANE_CHECK(glGetProgramiv, PFNGLGETPROGRAMIVPROC);
CANE_CHECK(glGetProgramInfoLog, PFNGLGETPROGRAMINFOLOGPROC);
CANE_CHECK(glUseProgram, PFNGLUSEPROGRAMPROC);
CANE_CHECK(glGetUniformLocation, PFNGLGETUNIFORMLOCATIONPROC);
CANE_CHECK(glUniform1i, PFNGLUNIFORM1IPROC);
CANE_CHECK(glUniform1iv, PFNGLUNIFORM1IVPROC);
CANE_CHECK(glUniform1f, PFNGLUNIFORM1FPROC);
CANE_CHECK(glUniform2f, PFNGLUNIFORM2FPROC);
CANE_CHECK(glUniform3f, PFNGLUNIFORM3FPROC);
CANE_CHECK(glUniform4f, PFNGLUNIFORM4FPROC);
CANE_CHECK(glUniform2fv, PFNGLUNIFORM2FVPROC);
CANE_CHECK(glUniform3fv, PFNGLUNIFORM3FVPROC);
CANE_CHECK(glUniform4fv, PFNGLUNIFORM4FVPROC);
CANE_CHECK(glUniformMatrix4fv, PFNGLUNIFORMMATRIX4FVPROC);
CANE_CHECK(glDrawArrays, PFNGLDRAWARRAYSPROC);
CANE_CHECK(glDrawElements, PFNGLDRAWELEMENTSPROC);
CANE_CHECK(glDrawElementsInstancedBaseVertexBaseInstance,
           PFNGLDRAWELEMENTSINSTANCEDBASEVERTEXBASEINSTANCEPROC);
CANE_CHECK(glMultiDrawElementsIndirect, PFNGLMULTIDRAWELEMENTSINDIRECTPROC);
CANE_CHECK(glGenQueries, PFNGLGENQUERIESPROC);
CANE_CHECK(glDeleteQueries, PFNGLDELETEQUERIESPROC);
CANE_CHECK(glBeginQuery, PFNGLBEGINQUERYPROC);
CANE_CHECK(glEndQuery, PFNGLENDQUERYPROC);
CANE_CHECK(glGetQueryObjectui64v, PFNGLGETQUERYOBJECTUI64VPROC);
CANE_CHECK(glFenceSync, PFNGLFENCESYNCPROC);
CANE_CHECK(glDeleteSync, PFNGLDELETESYNCPROC);
CANE_CHECK(glClientWaitSync, PFNGLCLIENTWAITSYNCPROC);

#undef CANE_CHECK

struct ProcEntry {
  const char* name;
  void* function;
};

#define CANE_PROC(name) {#name, reinterpret_cast<void*>(&mtl_##name)}

const ProcEntry kProcTable[] = {
    CANE_PROC(glGetError),
    CANE_PROC(glGetString),
    CANE_PROC(glGetStringi),
    CANE_PROC(glGetIntegerv),
    CANE_PROC(glGetFloatv),
    CANE_PROC(glEnable),
    CANE_PROC(glDisable),
    CANE_PROC(glViewport),
    CANE_PROC(glScissor),
    CANE_PROC(glClearColor),
    CANE_PROC(glClearDepth),
    CANE_PROC(glClearStencil),
    CANE_PROC(glClear),
    CANE_PROC(glColorMask),
    CANE_PROC(glDepthMask),
    CANE_PROC(glDepthFunc),
    CANE_PROC(glStencilMask),
    CANE_PROC(glStencilFunc),
    CANE_PROC(glStencilOp),
    CANE_PROC(glStencilOpSeparate),
    CANE_PROC(glBlendFunc),
    CANE_PROC(glBlendFuncSeparate),
    CANE_PROC(glBlendEquation),
    CANE_PROC(glBlendEquationSeparate),
    CANE_PROC(glBlendColor),
    CANE_PROC(glCullFace),
    CANE_PROC(glFrontFace),
    CANE_PROC(glLineWidth),
    CANE_PROC(glPixelStorei),
    CANE_PROC(glFinish),
    CANE_PROC(glFlush),
    CANE_PROC(glMemoryBarrier),
    CANE_PROC(glDrawBuffers),
    CANE_PROC(glReadBuffer),
    CANE_PROC(glGenBuffers),
    CANE_PROC(glDeleteBuffers),
    CANE_PROC(glBindBuffer),
    CANE_PROC(glBindBufferBase),
    CANE_PROC(glBindBufferRange),
    CANE_PROC(glBufferData),
    CANE_PROC(glBufferSubData),
    CANE_PROC(glMapBufferRange),
    CANE_PROC(glUnmapBuffer),
    CANE_PROC(glGenTextures),
    CANE_PROC(glDeleteTextures),
    CANE_PROC(glActiveTexture),
    CANE_PROC(glBindTexture),
    CANE_PROC(glTexImage2D),
    CANE_PROC(glTexStorage2D),
    CANE_PROC(glTexStorage3D),
    CANE_PROC(glTexSubImage2D),
    CANE_PROC(glTexSubImage3D),
    CANE_PROC(glTexParameteri),
    CANE_PROC(glGenerateMipmap),
    CANE_PROC(glGenSamplers),
    CANE_PROC(glDeleteSamplers),
    CANE_PROC(glBindSampler),
    CANE_PROC(glSamplerParameteri),
    CANE_PROC(glSamplerParameterf),
    CANE_PROC(glGenRenderbuffers),
    CANE_PROC(glDeleteRenderbuffers),
    CANE_PROC(glBindRenderbuffer),
    CANE_PROC(glRenderbufferStorage),
    CANE_PROC(glRenderbufferStorageMultisample),
    CANE_PROC(glGenVertexArrays),
    CANE_PROC(glDeleteVertexArrays),
    CANE_PROC(glBindVertexArray),
    CANE_PROC(glEnableVertexAttribArray),
    CANE_PROC(glDisableVertexAttribArray),
    CANE_PROC(glVertexAttribPointer),
    CANE_PROC(glVertexAttribIPointer),
    CANE_PROC(glVertexAttribDivisor),
    CANE_PROC(glGenFramebuffers),
    CANE_PROC(glDeleteFramebuffers),
    CANE_PROC(glBindFramebuffer),
    CANE_PROC(glFramebufferTexture2D),
    CANE_PROC(glFramebufferRenderbuffer),
    CANE_PROC(glCheckFramebufferStatus),
    CANE_PROC(glBlitFramebuffer),
    CANE_PROC(glReadPixels),
    CANE_PROC(glCreateShader),
    CANE_PROC(glDeleteShader),
    CANE_PROC(glShaderSource),
    CANE_PROC(glCompileShader),
    CANE_PROC(glGetShaderiv),
    CANE_PROC(glGetShaderInfoLog),
    CANE_PROC(glCreateProgram),
    CANE_PROC(glDeleteProgram),
    CANE_PROC(glAttachShader),
    CANE_PROC(glLinkProgram),
    CANE_PROC(glGetProgramiv),
    CANE_PROC(glGetProgramInfoLog),
    CANE_PROC(glUseProgram),
    CANE_PROC(glGetUniformLocation),
    CANE_PROC(glUniform1i),
    CANE_PROC(glUniform1iv),
    CANE_PROC(glUniform1f),
    CANE_PROC(glUniform2f),
    CANE_PROC(glUniform3f),
    CANE_PROC(glUniform4f),
    CANE_PROC(glUniform2fv),
    CANE_PROC(glUniform3fv),
    CANE_PROC(glUniform4fv),
    CANE_PROC(glUniformMatrix4fv),
    CANE_PROC(glDrawArrays),
    CANE_PROC(glDrawElements),
    CANE_PROC(glDrawElementsInstancedBaseVertexBaseInstance),
    CANE_PROC(glMultiDrawElementsIndirect),
    CANE_PROC(glGenQueries),
    CANE_PROC(glDeleteQueries),
    CANE_PROC(glBeginQuery),
    CANE_PROC(glEndQuery),
    CANE_PROC(glGetQueryObjectui64v),
    CANE_PROC(glFenceSync),
    CANE_PROC(glDeleteSync),
    CANE_PROC(glClientWaitSync),
    CANE_PROC(glClipControl),
};

#undef CANE_PROC

}  // namespace

namespace cane::gl_metal {

bool CreateContext(GLFWwindow* window, std::string* error) {
  auto context = std::make_unique<Context>();
  if (!context->Initialize(window, error)) return false;
  g_context = std::move(context);
  return true;
}

void DestroyContext() { g_context.reset(); }

void* ProcAddress(const char* name) {
  if (!name) return nullptr;
  for (const ProcEntry& entry : kProcTable) {
    if (std::strcmp(entry.name, name) == 0) return entry.function;
  }
  return nullptr;
}

void SetSwapInterval(int interval) {
  if (g_context) g_context->SetSwapInterval(interval);
}

void SwapBuffers() {
  if (g_context) g_context->SwapBuffers();
}

}  // namespace cane::gl_metal
