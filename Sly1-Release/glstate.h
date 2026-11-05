#pragma once
// CPU-side mirror of the OpenGL state the game changes. Game headers include
// glad only through this file, so the gl* redirects below cover every call
// site. Setters drop changes that would not alter GL state, and queries of
// mirrored state are answered here: an untracked glGet/glIsEnabled drains
// Mesa's glthread queue and serializes the CPU with the driver thread.
//
// The Dear ImGui backend uses its own loader. It reads state with real glGet
// calls and restores everything it changes, so it never desynchronizes this
// mirror. glProgramUniform*, double and non-square matrix uniform setters are
// not redirected; the game does not use them.
//
// Set CANE_GLSTATE_VERIFY=1 to compare the mirror against the driver twice per
// frame and print any mismatch.
#include <glad/glad.h>
#include <cstdint>
#include <cstring>

namespace glstate
{
constexpr int kTextureUnits = 8;
constexpr GLuint kPrograms = 64;
constexpr GLint kUniformLocations = 256;
constexpr GLuint kUnknownName = 0xFFFFFFFFu;

// Bits of State::known. The capability bits double as State::caps bits.
enum : uint32_t
{
	Known_Blend = 1u << 0,
	Known_DepthTest = 1u << 1,
	Known_CullFace = 1u << 2,
	Known_ScissorTest = 1u << 3,
	Known_StencilTest = 1u << 4,
	Known_DepthMask = 1u << 5,
	Known_DepthFunc = 1u << 6,
	Known_DepthRange = 1u << 7,
	Known_BlendFunc = 1u << 8,
	Known_BlendEquation = 1u << 9,
	Known_ColorMask = 1u << 10,
	Known_Scissor = 1u << 11,
	Known_Viewport = 1u << 12,
	Known_CullFaceMode = 1u << 13,
	Known_FrontFace = 1u << 14,
	Known_StencilFunc = 1u << 15,
	Known_StencilOp = 1u << 16,
	Known_StencilMask = 1u << 17,
	Known_PointSize = 1u << 18,
	Known_Program = 1u << 19,
	Known_VertexArray = 1u << 20,
	Known_ActiveTexture = 1u << 21,
	Known_ReadFramebuffer = 1u << 22,
	Known_DrawFramebuffer = 1u << 23,
};

struct State
{
	uint32_t known = 0;
	uint32_t caps = 0;
	GLboolean depthMask = GL_TRUE;
	GLenum depthFunc = GL_LESS;
	GLdouble depthRange[2] = { 0.0, 1.0 };
	GLenum blendSrcRgb = GL_ONE;
	GLenum blendDstRgb = GL_ZERO;
	GLenum blendSrcAlpha = GL_ONE;
	GLenum blendDstAlpha = GL_ZERO;
	GLenum blendEquationRgb = GL_FUNC_ADD;
	GLenum blendEquationAlpha = GL_FUNC_ADD;
	GLboolean colorMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
	GLint scissor[4] = {};
	GLint viewport[4] = {};
	GLenum cullFaceMode = GL_BACK;
	GLenum frontFace = GL_CCW;
	GLenum stencilFunc = GL_ALWAYS;
	GLint stencilRef = 0;
	GLuint stencilValueMask = 0xFFFFFFFFu;
	GLenum stencilFail = GL_KEEP;
	GLenum stencilDepthFail = GL_KEEP;
	GLenum stencilDepthPass = GL_KEEP;
	GLuint stencilWriteMask = 0xFFFFFFFFu;
	GLfloat pointSize = 1.0f;
	GLuint program = 0;
	GLuint vertexArray = 0;
	GLuint readFramebuffer = 0;
	GLuint drawFramebuffer = 0;
	GLenum activeTexture = GL_TEXTURE0;
	// kUnknownName marks a binding that has not been observed yet.
	GLuint texture2D[kTextureUnits] = { kUnknownName, kUnknownName, kUnknownName, kUnknownName, kUnknownName, kUnknownName, kUnknownName, kUnknownName };
	GLuint texture2DArray[kTextureUnits] = { kUnknownName, kUnknownName, kUnknownName, kUnknownName, kUnknownName, kUnknownName, kUnknownName, kUnknownName };
};

// Uniform values are per-program state, so the cache is keyed by the program
// that is current when glUniform* runs. Values are compared bitwise.
struct UniformValue
{
	uint32_t tag;
	uint32_t data[16];
};

struct ProgramUniforms
{
	UniformValue values[kUniformLocations];
};

inline State g_state;
inline ProgramUniforms* g_uniforms[kPrograms] = {};

// Reads the complete mirrored state from the driver. Call once after the
// context is current and the loader is initialized.
void Sync();
// Compares the mirror with the driver when CANE_GLSTATE_VERIFY=1.
void Verify(const char* where);

GLboolean IsEnabled(GLenum cap);
void GetBooleanv(GLenum pname, GLboolean* data);
void GetIntegerv(GLenum pname, GLint* data);
void GetFloatv(GLenum pname, GLfloat* data);
void ResetProgramUniforms(GLuint program);
void ForgetUniforms(GLint location, GLsizei count);
void DeleteTextures(GLsizei n, const GLuint* textures);
void DeleteVertexArrays(GLsizei n, const GLuint* arrays);
void DeleteFramebuffers(GLsizei n, const GLuint* framebuffers);
void DeleteProgram(GLuint program);
void LinkProgram(GLuint program);

namespace detail
{
inline uint32_t CapBit(GLenum cap)
{
	switch (cap)
	{
	case GL_BLEND: return Known_Blend;
	case GL_DEPTH_TEST: return Known_DepthTest;
	case GL_CULL_FACE: return Known_CullFace;
	case GL_SCISSOR_TEST: return Known_ScissorTest;
	case GL_STENCIL_TEST: return Known_StencilTest;
	default: return 0;
	}
}

inline bool Known(uint32_t bits)
{
	return (g_state.known & bits) == bits;
}

inline GLboolean AsBoolean(GLboolean value)
{
	return value != GL_FALSE ? GL_TRUE : GL_FALSE;
}

inline int ActiveUnit()
{
	const GLuint unit = g_state.activeTexture - GL_TEXTURE0;
	return Known(Known_ActiveTexture) && unit < static_cast<GLuint>(kTextureUnits) ? static_cast<int>(unit) : -1;
}

inline GLuint* TextureSlots(GLenum target)
{
	if (target == GL_TEXTURE_2D)
		return g_state.texture2D;
	if (target == GL_TEXTURE_2D_ARRAY)
		return g_state.texture2DArray;
	return nullptr;
}

// Returns true when the current program already holds these exact bits at
// this location; otherwise records them and returns false.
inline bool UniformCached(GLint location, uint32_t tag, const void* value, size_t words)
{
	const GLuint program = g_state.program;
	if (!Known(Known_Program) || program == 0 || program >= kPrograms ||
		location < 0 || location >= kUniformLocations)
		return false;

	ProgramUniforms*& uniforms = g_uniforms[program];
	if (uniforms == nullptr)
		uniforms = new ProgramUniforms{};

	// Word loops rather than memcmp/memcpy: the sizes are compile-time
	// constants after inlining, and a libc call per uniform showed up in
	// profiles.
	UniformValue& cached = uniforms->values[location];
	uint32_t bits[16];
	std::memcpy(bits, value, words * sizeof(uint32_t));
	bool same = cached.tag == tag;
	for (size_t i = 0; i < words; ++i)
		same &= cached.data[i] == bits[i];
	if (same)
		return true;

	cached.tag = tag;
	for (size_t i = 0; i < words; ++i)
		cached.data[i] = bits[i];
	return false;
}

// Tag layout: component kind in the high byte, word count in the low byte.
enum : uint32_t
{
	UniformFloat = 1u << 8,
	UniformInt = 2u << 8,
	UniformUint = 3u << 8,
};
}

inline void Enable(GLenum cap)
{
	const uint32_t bit = detail::CapBit(cap);
	if (bit != 0)
	{
		if (detail::Known(bit) && (g_state.caps & bit) != 0)
			return;
		g_state.known |= bit;
		g_state.caps |= bit;
	}
	glad_glEnable(cap);
}

inline void Disable(GLenum cap)
{
	const uint32_t bit = detail::CapBit(cap);
	if (bit != 0)
	{
		if (detail::Known(bit) && (g_state.caps & bit) == 0)
			return;
		g_state.known |= bit;
		g_state.caps &= ~bit;
	}
	glad_glDisable(cap);
}

inline void Enablei(GLenum cap, GLuint index)
{
	g_state.known &= ~detail::CapBit(cap);
	glad_glEnablei(cap, index);
}

inline void Disablei(GLenum cap, GLuint index)
{
	g_state.known &= ~detail::CapBit(cap);
	glad_glDisablei(cap, index);
}

inline void DepthMask(GLboolean flag)
{
	const GLboolean value = detail::AsBoolean(flag);
	if (detail::Known(Known_DepthMask) && g_state.depthMask == value)
		return;
	g_state.known |= Known_DepthMask;
	g_state.depthMask = value;
	glad_glDepthMask(flag);
}

inline void DepthFunc(GLenum func)
{
	if (detail::Known(Known_DepthFunc) && g_state.depthFunc == func)
		return;
	g_state.known |= Known_DepthFunc;
	g_state.depthFunc = func;
	glad_glDepthFunc(func);
}

namespace detail
{
// GL clamps the depth range to [0, 1] when it is set.
inline bool SetDepthRange(GLdouble n, GLdouble f)
{
	n = n < 0.0 ? 0.0 : (n > 1.0 ? 1.0 : n);
	f = f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f);
	if (Known(Known_DepthRange) && g_state.depthRange[0] == n && g_state.depthRange[1] == f)
		return false;
	g_state.known |= Known_DepthRange;
	g_state.depthRange[0] = n;
	g_state.depthRange[1] = f;
	return true;
}
}

inline void DepthRange(GLdouble n, GLdouble f)
{
	if (detail::SetDepthRange(n, f))
		glad_glDepthRange(n, f);
}

inline void DepthRangef(GLfloat n, GLfloat f)
{
	if (detail::SetDepthRange(n, f))
		glad_glDepthRangef(n, f);
}

inline void DepthRangeIndexed(GLuint index, GLdouble n, GLdouble f)
{
	g_state.known &= ~Known_DepthRange;
	glad_glDepthRangeIndexed(index, n, f);
}

inline void DepthRangeArrayv(GLuint first, GLsizei count, const GLdouble* v)
{
	g_state.known &= ~Known_DepthRange;
	glad_glDepthRangeArrayv(first, count, v);
}

inline void BlendFuncSeparate(GLenum srcRgb, GLenum dstRgb, GLenum srcAlpha, GLenum dstAlpha)
{
	if (detail::Known(Known_BlendFunc) &&
		g_state.blendSrcRgb == srcRgb && g_state.blendDstRgb == dstRgb &&
		g_state.blendSrcAlpha == srcAlpha && g_state.blendDstAlpha == dstAlpha)
		return;
	g_state.known |= Known_BlendFunc;
	g_state.blendSrcRgb = srcRgb;
	g_state.blendDstRgb = dstRgb;
	g_state.blendSrcAlpha = srcAlpha;
	g_state.blendDstAlpha = dstAlpha;
	glad_glBlendFuncSeparate(srcRgb, dstRgb, srcAlpha, dstAlpha);
}

inline void BlendFunc(GLenum src, GLenum dst)
{
	if (detail::Known(Known_BlendFunc) &&
		g_state.blendSrcRgb == src && g_state.blendDstRgb == dst &&
		g_state.blendSrcAlpha == src && g_state.blendDstAlpha == dst)
		return;
	g_state.known |= Known_BlendFunc;
	g_state.blendSrcRgb = src;
	g_state.blendDstRgb = dst;
	g_state.blendSrcAlpha = src;
	g_state.blendDstAlpha = dst;
	glad_glBlendFunc(src, dst);
}

inline void BlendFunci(GLuint buf, GLenum src, GLenum dst)
{
	g_state.known &= ~Known_BlendFunc;
	glad_glBlendFunci(buf, src, dst);
}

inline void BlendFuncSeparatei(GLuint buf, GLenum srcRgb, GLenum dstRgb, GLenum srcAlpha, GLenum dstAlpha)
{
	g_state.known &= ~Known_BlendFunc;
	glad_glBlendFuncSeparatei(buf, srcRgb, dstRgb, srcAlpha, dstAlpha);
}

inline void BlendEquationSeparate(GLenum modeRgb, GLenum modeAlpha)
{
	if (detail::Known(Known_BlendEquation) &&
		g_state.blendEquationRgb == modeRgb && g_state.blendEquationAlpha == modeAlpha)
		return;
	g_state.known |= Known_BlendEquation;
	g_state.blendEquationRgb = modeRgb;
	g_state.blendEquationAlpha = modeAlpha;
	glad_glBlendEquationSeparate(modeRgb, modeAlpha);
}

inline void BlendEquation(GLenum mode)
{
	if (detail::Known(Known_BlendEquation) &&
		g_state.blendEquationRgb == mode && g_state.blendEquationAlpha == mode)
		return;
	g_state.known |= Known_BlendEquation;
	g_state.blendEquationRgb = mode;
	g_state.blendEquationAlpha = mode;
	glad_glBlendEquation(mode);
}

inline void BlendEquationi(GLuint buf, GLenum mode)
{
	g_state.known &= ~Known_BlendEquation;
	glad_glBlendEquationi(buf, mode);
}

inline void BlendEquationSeparatei(GLuint buf, GLenum modeRgb, GLenum modeAlpha)
{
	g_state.known &= ~Known_BlendEquation;
	glad_glBlendEquationSeparatei(buf, modeRgb, modeAlpha);
}

inline void ColorMask(GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
	const GLboolean value[4] = { detail::AsBoolean(r), detail::AsBoolean(g), detail::AsBoolean(b), detail::AsBoolean(a) };
	if (detail::Known(Known_ColorMask) && std::memcmp(g_state.colorMask, value, sizeof(value)) == 0)
		return;
	g_state.known |= Known_ColorMask;
	std::memcpy(g_state.colorMask, value, sizeof(value));
	glad_glColorMask(r, g, b, a);
}

inline void ColorMaski(GLuint index, GLboolean r, GLboolean g, GLboolean b, GLboolean a)
{
	g_state.known &= ~Known_ColorMask;
	glad_glColorMaski(index, r, g, b, a);
}

inline void Scissor(GLint x, GLint y, GLsizei width, GLsizei height)
{
	if (detail::Known(Known_Scissor) &&
		g_state.scissor[0] == x && g_state.scissor[1] == y &&
		g_state.scissor[2] == width && g_state.scissor[3] == height)
		return;
	g_state.known |= Known_Scissor;
	g_state.scissor[0] = x;
	g_state.scissor[1] = y;
	g_state.scissor[2] = width;
	g_state.scissor[3] = height;
	glad_glScissor(x, y, width, height);
}

inline void ScissorIndexed(GLuint index, GLint left, GLint bottom, GLsizei width, GLsizei height)
{
	g_state.known &= ~Known_Scissor;
	glad_glScissorIndexed(index, left, bottom, width, height);
}

inline void ScissorIndexedv(GLuint index, const GLint* v)
{
	g_state.known &= ~Known_Scissor;
	glad_glScissorIndexedv(index, v);
}

inline void ScissorArrayv(GLuint first, GLsizei count, const GLint* v)
{
	g_state.known &= ~Known_Scissor;
	glad_glScissorArrayv(first, count, v);
}

inline void Viewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
	if (detail::Known(Known_Viewport) &&
		g_state.viewport[0] == x && g_state.viewport[1] == y &&
		g_state.viewport[2] == width && g_state.viewport[3] == height)
		return;
	g_state.known |= Known_Viewport;
	g_state.viewport[0] = x;
	g_state.viewport[1] = y;
	g_state.viewport[2] = width;
	g_state.viewport[3] = height;
	glad_glViewport(x, y, width, height);
}

inline void ViewportIndexedf(GLuint index, GLfloat x, GLfloat y, GLfloat w, GLfloat h)
{
	g_state.known &= ~Known_Viewport;
	glad_glViewportIndexedf(index, x, y, w, h);
}

inline void ViewportIndexedfv(GLuint index, const GLfloat* v)
{
	g_state.known &= ~Known_Viewport;
	glad_glViewportIndexedfv(index, v);
}

inline void ViewportArrayv(GLuint first, GLsizei count, const GLfloat* v)
{
	g_state.known &= ~Known_Viewport;
	glad_glViewportArrayv(first, count, v);
}

inline void CullFace(GLenum mode)
{
	if (detail::Known(Known_CullFaceMode) && g_state.cullFaceMode == mode)
		return;
	g_state.known |= Known_CullFaceMode;
	g_state.cullFaceMode = mode;
	glad_glCullFace(mode);
}

inline void FrontFace(GLenum mode)
{
	if (detail::Known(Known_FrontFace) && g_state.frontFace == mode)
		return;
	g_state.known |= Known_FrontFace;
	g_state.frontFace = mode;
	glad_glFrontFace(mode);
}

inline void StencilFunc(GLenum func, GLint ref, GLuint mask)
{
	if (detail::Known(Known_StencilFunc) &&
		g_state.stencilFunc == func && g_state.stencilRef == ref && g_state.stencilValueMask == mask)
		return;
	g_state.known |= Known_StencilFunc;
	g_state.stencilFunc = func;
	g_state.stencilRef = ref;
	g_state.stencilValueMask = mask;
	glad_glStencilFunc(func, ref, mask);
}

inline void StencilFuncSeparate(GLenum face, GLenum func, GLint ref, GLuint mask)
{
	if (face == GL_FRONT_AND_BACK)
	{
		StencilFunc(func, ref, mask);
		return;
	}
	g_state.known &= ~Known_StencilFunc;
	glad_glStencilFuncSeparate(face, func, ref, mask);
}

inline void StencilOp(GLenum fail, GLenum depthFail, GLenum depthPass)
{
	if (detail::Known(Known_StencilOp) &&
		g_state.stencilFail == fail && g_state.stencilDepthFail == depthFail && g_state.stencilDepthPass == depthPass)
		return;
	g_state.known |= Known_StencilOp;
	g_state.stencilFail = fail;
	g_state.stencilDepthFail = depthFail;
	g_state.stencilDepthPass = depthPass;
	glad_glStencilOp(fail, depthFail, depthPass);
}

inline void StencilOpSeparate(GLenum face, GLenum fail, GLenum depthFail, GLenum depthPass)
{
	if (face == GL_FRONT_AND_BACK)
	{
		StencilOp(fail, depthFail, depthPass);
		return;
	}
	g_state.known &= ~Known_StencilOp;
	glad_glStencilOpSeparate(face, fail, depthFail, depthPass);
}

inline void StencilMask(GLuint mask)
{
	if (detail::Known(Known_StencilMask) && g_state.stencilWriteMask == mask)
		return;
	g_state.known |= Known_StencilMask;
	g_state.stencilWriteMask = mask;
	glad_glStencilMask(mask);
}

inline void StencilMaskSeparate(GLenum face, GLuint mask)
{
	if (face == GL_FRONT_AND_BACK)
	{
		StencilMask(mask);
		return;
	}
	g_state.known &= ~Known_StencilMask;
	glad_glStencilMaskSeparate(face, mask);
}

inline void PointSize(GLfloat size)
{
	if (detail::Known(Known_PointSize) && std::memcmp(&g_state.pointSize, &size, sizeof(size)) == 0)
		return;
	g_state.known |= Known_PointSize;
	g_state.pointSize = size;
	glad_glPointSize(size);
}

inline void UseProgram(GLuint program)
{
	if (detail::Known(Known_Program) && g_state.program == program)
		return;
	g_state.known |= Known_Program;
	g_state.program = program;
	glad_glUseProgram(program);
}

inline void BindVertexArray(GLuint array)
{
	if (detail::Known(Known_VertexArray) && g_state.vertexArray == array)
		return;
	g_state.known |= Known_VertexArray;
	g_state.vertexArray = array;
	glad_glBindVertexArray(array);
}

inline void BindFramebuffer(GLenum target, GLuint framebuffer)
{
	uint32_t bits = 0;
	if (target == GL_FRAMEBUFFER)
		bits = Known_ReadFramebuffer | Known_DrawFramebuffer;
	else if (target == GL_READ_FRAMEBUFFER)
		bits = Known_ReadFramebuffer;
	else if (target == GL_DRAW_FRAMEBUFFER)
		bits = Known_DrawFramebuffer;

	const bool readSame = (bits & Known_ReadFramebuffer) == 0 ||
		(detail::Known(Known_ReadFramebuffer) && g_state.readFramebuffer == framebuffer);
	const bool drawSame = (bits & Known_DrawFramebuffer) == 0 ||
		(detail::Known(Known_DrawFramebuffer) && g_state.drawFramebuffer == framebuffer);
	if (bits != 0 && readSame && drawSame)
		return;

	if (bits & Known_ReadFramebuffer)
		g_state.readFramebuffer = framebuffer;
	if (bits & Known_DrawFramebuffer)
		g_state.drawFramebuffer = framebuffer;
	g_state.known |= bits;
	glad_glBindFramebuffer(target, framebuffer);
}

inline void ActiveTexture(GLenum texture)
{
	if (detail::Known(Known_ActiveTexture) && g_state.activeTexture == texture)
		return;
	g_state.known |= Known_ActiveTexture;
	g_state.activeTexture = texture;
	glad_glActiveTexture(texture);
}

inline void BindTexture(GLenum target, GLuint texture)
{
	GLuint* const slots = detail::TextureSlots(target);
	if (slots != nullptr)
	{
		const int unit = detail::ActiveUnit();
		if (unit >= 0)
		{
			if (slots[unit] == texture)
				return;
			slots[unit] = texture;
		}
		else if (!detail::Known(Known_ActiveTexture))
		{
			for (int i = 0; i < kTextureUnits; ++i)
				slots[i] = kUnknownName;
		}
	}
	glad_glBindTexture(target, texture);
}

// Binds texture to target on unit and leaves that unit active, switching units
// only when the binding actually changes.
inline void BindTextureUnit(GLuint unit, GLenum target, GLuint texture)
{
	GLuint* const slots = detail::TextureSlots(target);
	if (slots != nullptr && unit < static_cast<GLuint>(kTextureUnits) && slots[unit] == texture)
		return;
	ActiveTexture(GL_TEXTURE0 + unit);
	BindTexture(target, texture);
}

#define GLSTATE_UNIFORM_VALUES(Name, Type, Kind, Words)                                            \
	inline void Name(GLint location, GLsizei count, const Type* value)                             \
	{                                                                                              \
		if (count == 1)                                                                            \
		{                                                                                          \
			if (detail::UniformCached(location, detail::Kind | (Words), value, (Words)))           \
				return;                                                                            \
		}                                                                                          \
		else                                                                                       \
			ForgetUniforms(location, count);                                                       \
		glad_gl##Name(location, count, value);                                                     \
	}

GLSTATE_UNIFORM_VALUES(Uniform1fv, GLfloat, UniformFloat, 1)
GLSTATE_UNIFORM_VALUES(Uniform2fv, GLfloat, UniformFloat, 2)
GLSTATE_UNIFORM_VALUES(Uniform3fv, GLfloat, UniformFloat, 3)
GLSTATE_UNIFORM_VALUES(Uniform4fv, GLfloat, UniformFloat, 4)
GLSTATE_UNIFORM_VALUES(Uniform1iv, GLint, UniformInt, 1)
GLSTATE_UNIFORM_VALUES(Uniform2iv, GLint, UniformInt, 2)
GLSTATE_UNIFORM_VALUES(Uniform3iv, GLint, UniformInt, 3)
GLSTATE_UNIFORM_VALUES(Uniform4iv, GLint, UniformInt, 4)
GLSTATE_UNIFORM_VALUES(Uniform1uiv, GLuint, UniformUint, 1)
GLSTATE_UNIFORM_VALUES(Uniform2uiv, GLuint, UniformUint, 2)
GLSTATE_UNIFORM_VALUES(Uniform3uiv, GLuint, UniformUint, 3)
GLSTATE_UNIFORM_VALUES(Uniform4uiv, GLuint, UniformUint, 4)
#undef GLSTATE_UNIFORM_VALUES

inline void Uniform1f(GLint location, GLfloat v0) { const GLfloat v[] = { v0 }; Uniform1fv(location, 1, v); }
inline void Uniform2f(GLint location, GLfloat v0, GLfloat v1) { const GLfloat v[] = { v0, v1 }; Uniform2fv(location, 1, v); }
inline void Uniform3f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2) { const GLfloat v[] = { v0, v1, v2 }; Uniform3fv(location, 1, v); }
inline void Uniform4f(GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3) { const GLfloat v[] = { v0, v1, v2, v3 }; Uniform4fv(location, 1, v); }
inline void Uniform1i(GLint location, GLint v0) { const GLint v[] = { v0 }; Uniform1iv(location, 1, v); }
inline void Uniform2i(GLint location, GLint v0, GLint v1) { const GLint v[] = { v0, v1 }; Uniform2iv(location, 1, v); }
inline void Uniform3i(GLint location, GLint v0, GLint v1, GLint v2) { const GLint v[] = { v0, v1, v2 }; Uniform3iv(location, 1, v); }
inline void Uniform4i(GLint location, GLint v0, GLint v1, GLint v2, GLint v3) { const GLint v[] = { v0, v1, v2, v3 }; Uniform4iv(location, 1, v); }
inline void Uniform1ui(GLint location, GLuint v0) { const GLuint v[] = { v0 }; Uniform1uiv(location, 1, v); }
inline void Uniform2ui(GLint location, GLuint v0, GLuint v1) { const GLuint v[] = { v0, v1 }; Uniform2uiv(location, 1, v); }
inline void Uniform3ui(GLint location, GLuint v0, GLuint v1, GLuint v2) { const GLuint v[] = { v0, v1, v2 }; Uniform3uiv(location, 1, v); }
inline void Uniform4ui(GLint location, GLuint v0, GLuint v1, GLuint v2, GLuint v3) { const GLuint v[] = { v0, v1, v2, v3 }; Uniform4uiv(location, 1, v); }

inline void UniformMatrix3fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
	if (count == 1 && transpose == GL_FALSE)
	{
		if (detail::UniformCached(location, detail::UniformFloat | 9, value, 9))
			return;
	}
	else
		ForgetUniforms(location, count);
	glad_glUniformMatrix3fv(location, count, transpose, value);
}

inline void UniformMatrix4fv(GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)
{
	if (count == 1 && transpose == GL_FALSE)
	{
		if (detail::UniformCached(location, detail::UniformFloat | 16, value, 16))
			return;
	}
	else
		ForgetUniforms(location, count);
	glad_glUniformMatrix4fv(location, count, transpose, value);
}
}

#undef glEnable
#define glEnable glstate::Enable
#undef glDisable
#define glDisable glstate::Disable
#undef glEnablei
#define glEnablei glstate::Enablei
#undef glDisablei
#define glDisablei glstate::Disablei
#undef glIsEnabled
#define glIsEnabled glstate::IsEnabled
#undef glGetBooleanv
#define glGetBooleanv glstate::GetBooleanv
#undef glGetIntegerv
#define glGetIntegerv glstate::GetIntegerv
#undef glGetFloatv
#define glGetFloatv glstate::GetFloatv
#undef glDepthMask
#define glDepthMask glstate::DepthMask
#undef glDepthFunc
#define glDepthFunc glstate::DepthFunc
#undef glDepthRange
#define glDepthRange glstate::DepthRange
#undef glDepthRangef
#define glDepthRangef glstate::DepthRangef
#undef glDepthRangeIndexed
#define glDepthRangeIndexed glstate::DepthRangeIndexed
#undef glDepthRangeArrayv
#define glDepthRangeArrayv glstate::DepthRangeArrayv
#undef glBlendFunc
#define glBlendFunc glstate::BlendFunc
#undef glBlendFuncSeparate
#define glBlendFuncSeparate glstate::BlendFuncSeparate
#undef glBlendFunci
#define glBlendFunci glstate::BlendFunci
#undef glBlendFuncSeparatei
#define glBlendFuncSeparatei glstate::BlendFuncSeparatei
#undef glBlendEquation
#define glBlendEquation glstate::BlendEquation
#undef glBlendEquationSeparate
#define glBlendEquationSeparate glstate::BlendEquationSeparate
#undef glBlendEquationi
#define glBlendEquationi glstate::BlendEquationi
#undef glBlendEquationSeparatei
#define glBlendEquationSeparatei glstate::BlendEquationSeparatei
#undef glColorMask
#define glColorMask glstate::ColorMask
#undef glColorMaski
#define glColorMaski glstate::ColorMaski
#undef glScissor
#define glScissor glstate::Scissor
#undef glScissorIndexed
#define glScissorIndexed glstate::ScissorIndexed
#undef glScissorIndexedv
#define glScissorIndexedv glstate::ScissorIndexedv
#undef glScissorArrayv
#define glScissorArrayv glstate::ScissorArrayv
#undef glViewport
#define glViewport glstate::Viewport
#undef glViewportIndexedf
#define glViewportIndexedf glstate::ViewportIndexedf
#undef glViewportIndexedfv
#define glViewportIndexedfv glstate::ViewportIndexedfv
#undef glViewportArrayv
#define glViewportArrayv glstate::ViewportArrayv
#undef glCullFace
#define glCullFace glstate::CullFace
#undef glFrontFace
#define glFrontFace glstate::FrontFace
#undef glStencilFunc
#define glStencilFunc glstate::StencilFunc
#undef glStencilFuncSeparate
#define glStencilFuncSeparate glstate::StencilFuncSeparate
#undef glStencilOp
#define glStencilOp glstate::StencilOp
#undef glStencilOpSeparate
#define glStencilOpSeparate glstate::StencilOpSeparate
#undef glStencilMask
#define glStencilMask glstate::StencilMask
#undef glStencilMaskSeparate
#define glStencilMaskSeparate glstate::StencilMaskSeparate
#undef glPointSize
#define glPointSize glstate::PointSize
#undef glUseProgram
#define glUseProgram glstate::UseProgram
#undef glLinkProgram
#define glLinkProgram glstate::LinkProgram
#undef glDeleteProgram
#define glDeleteProgram glstate::DeleteProgram
#undef glBindVertexArray
#define glBindVertexArray glstate::BindVertexArray
#undef glDeleteVertexArrays
#define glDeleteVertexArrays glstate::DeleteVertexArrays
#undef glBindFramebuffer
#define glBindFramebuffer glstate::BindFramebuffer
#undef glDeleteFramebuffers
#define glDeleteFramebuffers glstate::DeleteFramebuffers
#undef glActiveTexture
#define glActiveTexture glstate::ActiveTexture
#undef glBindTexture
#define glBindTexture glstate::BindTexture
#undef glDeleteTextures
#define glDeleteTextures glstate::DeleteTextures
#undef glUniform1f
#define glUniform1f glstate::Uniform1f
#undef glUniform2f
#define glUniform2f glstate::Uniform2f
#undef glUniform3f
#define glUniform3f glstate::Uniform3f
#undef glUniform4f
#define glUniform4f glstate::Uniform4f
#undef glUniform1i
#define glUniform1i glstate::Uniform1i
#undef glUniform2i
#define glUniform2i glstate::Uniform2i
#undef glUniform3i
#define glUniform3i glstate::Uniform3i
#undef glUniform4i
#define glUniform4i glstate::Uniform4i
#undef glUniform1ui
#define glUniform1ui glstate::Uniform1ui
#undef glUniform2ui
#define glUniform2ui glstate::Uniform2ui
#undef glUniform3ui
#define glUniform3ui glstate::Uniform3ui
#undef glUniform4ui
#define glUniform4ui glstate::Uniform4ui
#undef glUniform1fv
#define glUniform1fv glstate::Uniform1fv
#undef glUniform2fv
#define glUniform2fv glstate::Uniform2fv
#undef glUniform3fv
#define glUniform3fv glstate::Uniform3fv
#undef glUniform4fv
#define glUniform4fv glstate::Uniform4fv
#undef glUniform1iv
#define glUniform1iv glstate::Uniform1iv
#undef glUniform2iv
#define glUniform2iv glstate::Uniform2iv
#undef glUniform3iv
#define glUniform3iv glstate::Uniform3iv
#undef glUniform4iv
#define glUniform4iv glstate::Uniform4iv
#undef glUniform1uiv
#define glUniform1uiv glstate::Uniform1uiv
#undef glUniform2uiv
#define glUniform2uiv glstate::Uniform2uiv
#undef glUniform3uiv
#define glUniform3uiv glstate::Uniform3uiv
#undef glUniform4uiv
#define glUniform4uiv glstate::Uniform4uiv
#undef glUniformMatrix3fv
#define glUniformMatrix3fv glstate::UniformMatrix3fv
#undef glUniformMatrix4fv
#define glUniformMatrix4fv glstate::UniformMatrix4fv
