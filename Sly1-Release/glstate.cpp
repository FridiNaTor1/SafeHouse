#include "glstate.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace glstate
{
namespace
{
constexpr uint32_t kCapBits = Known_Blend | Known_DepthTest | Known_CullFace | Known_ScissorTest | Known_StencilTest;
constexpr GLenum kCaps[] = { GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST };
constexpr const char* kCapNames[] = { "GL_BLEND", "GL_DEPTH_TEST", "GL_CULL_FACE", "GL_SCISSOR_TEST", "GL_STENCIL_TEST" };

GLint RealInteger(GLenum pname)
{
	GLint value = 0;
	glad_glGetIntegerv(pname, &value);
	return value;
}

// Only queries attachment parameters that are valid for the attachment's type,
// so verification never raises a GL error the game could observe.
GLint DrawFramebufferStencilBits()
{
	const GLenum attachment = RealInteger(GL_DRAW_FRAMEBUFFER_BINDING) == 0 ? GL_STENCIL : GL_STENCIL_ATTACHMENT;
	GLint type = GL_NONE;
	glad_glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
	if (type == GL_NONE)
		return 0;
	GLint bits = 0;
	glad_glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, attachment, GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE, &bits);
	return bits;
}

// Returns the number of integer values written for a mirrored pname, or 0
// when the pname is not mirrored or its value is currently unknown.
int LookupIntegers(GLenum pname, GLint* out)
{
	const State& s = g_state;
	const uint32_t cap = detail::CapBit(pname);
	if (cap != 0)
	{
		if (!detail::Known(cap))
			return 0;
		out[0] = (s.caps & cap) != 0 ? 1 : 0;
		return 1;
	}

	auto one = [out](bool known, GLint value)
	{
		if (!known)
			return 0;
		out[0] = value;
		return 1;
	};

	switch (pname)
	{
	case GL_DEPTH_WRITEMASK: return one(detail::Known(Known_DepthMask), s.depthMask);
	case GL_DEPTH_FUNC: return one(detail::Known(Known_DepthFunc), static_cast<GLint>(s.depthFunc));
	case GL_BLEND_SRC_RGB: return one(detail::Known(Known_BlendFunc), static_cast<GLint>(s.blendSrcRgb));
	case GL_BLEND_DST_RGB: return one(detail::Known(Known_BlendFunc), static_cast<GLint>(s.blendDstRgb));
	case GL_BLEND_SRC_ALPHA: return one(detail::Known(Known_BlendFunc), static_cast<GLint>(s.blendSrcAlpha));
	case GL_BLEND_DST_ALPHA: return one(detail::Known(Known_BlendFunc), static_cast<GLint>(s.blendDstAlpha));
	case GL_BLEND_EQUATION_RGB: return one(detail::Known(Known_BlendEquation), static_cast<GLint>(s.blendEquationRgb));
	case GL_BLEND_EQUATION_ALPHA: return one(detail::Known(Known_BlendEquation), static_cast<GLint>(s.blendEquationAlpha));
	case GL_CULL_FACE_MODE: return one(detail::Known(Known_CullFaceMode), static_cast<GLint>(s.cullFaceMode));
	case GL_FRONT_FACE: return one(detail::Known(Known_FrontFace), static_cast<GLint>(s.frontFace));
	case GL_CURRENT_PROGRAM: return one(detail::Known(Known_Program), static_cast<GLint>(s.program));
	case GL_VERTEX_ARRAY_BINDING: return one(detail::Known(Known_VertexArray), static_cast<GLint>(s.vertexArray));
	case GL_READ_FRAMEBUFFER_BINDING: return one(detail::Known(Known_ReadFramebuffer), static_cast<GLint>(s.readFramebuffer));
	case GL_DRAW_FRAMEBUFFER_BINDING: return one(detail::Known(Known_DrawFramebuffer), static_cast<GLint>(s.drawFramebuffer));
	case GL_ACTIVE_TEXTURE: return one(detail::Known(Known_ActiveTexture), static_cast<GLint>(s.activeTexture));
	case GL_TEXTURE_BINDING_2D:
	case GL_TEXTURE_BINDING_2D_ARRAY:
	{
		const int unit = detail::ActiveUnit();
		if (unit < 0)
			return 0;
		const GLuint name = pname == GL_TEXTURE_BINDING_2D ? s.texture2D[unit] : s.texture2DArray[unit];
		return one(name != kUnknownName, static_cast<GLint>(name));
	}
	case GL_COLOR_WRITEMASK:
		if (!detail::Known(Known_ColorMask))
			return 0;
		for (int i = 0; i < 4; ++i)
			out[i] = s.colorMask[i];
		return 4;
	case GL_SCISSOR_BOX:
		if (!detail::Known(Known_Scissor))
			return 0;
		for (int i = 0; i < 4; ++i)
			out[i] = s.scissor[i];
		return 4;
	case GL_VIEWPORT:
		if (!detail::Known(Known_Viewport))
			return 0;
		for (int i = 0; i < 4; ++i)
			out[i] = s.viewport[i];
		return 4;
	default:
		return 0;
	}
}

void ForgetTexture(GLuint* slots, GLuint name)
{
	for (int unit = 0; unit < kTextureUnits; ++unit)
	{
		if (slots[unit] == name)
			slots[unit] = 0;
	}
}

bool VerifyEnabled()
{
	static const bool enabled = []
	{
		const char* value = std::getenv("CANE_GLSTATE_VERIFY");
		return value != nullptr && value[0] != '\0' && value[0] != '0';
	}();
	return enabled;
}
}

GLboolean IsEnabled(GLenum cap)
{
	const uint32_t bit = detail::CapBit(cap);
	if (bit != 0 && detail::Known(bit))
		return (g_state.caps & bit) != 0 ? GL_TRUE : GL_FALSE;
	return glad_glIsEnabled(cap);
}

void GetBooleanv(GLenum pname, GLboolean* data)
{
	GLint values[4];
	const int count = LookupIntegers(pname, values);
	if (count == 0)
	{
		glad_glGetBooleanv(pname, data);
		return;
	}
	for (int i = 0; i < count; ++i)
		data[i] = values[i] != 0 ? GL_TRUE : GL_FALSE;
}

void GetIntegerv(GLenum pname, GLint* data)
{
	GLint values[4];
	const int count = LookupIntegers(pname, values);
	if (count == 0)
	{
		glad_glGetIntegerv(pname, data);
		return;
	}
	for (int i = 0; i < count; ++i)
		data[i] = values[i];
}

void GetFloatv(GLenum pname, GLfloat* data)
{
	if (pname == GL_POINT_SIZE && detail::Known(Known_PointSize))
	{
		data[0] = g_state.pointSize;
		return;
	}
	if (pname == GL_DEPTH_RANGE && detail::Known(Known_DepthRange))
	{
		data[0] = static_cast<GLfloat>(g_state.depthRange[0]);
		data[1] = static_cast<GLfloat>(g_state.depthRange[1]);
		return;
	}
	glad_glGetFloatv(pname, data);
}

void ResetProgramUniforms(GLuint program)
{
	if (program < kPrograms && g_uniforms[program] != nullptr)
		*g_uniforms[program] = ProgramUniforms{};
}

void ForgetUniforms(GLint location, GLsizei count)
{
	const GLuint program = g_state.program;
	if (program >= kPrograms || g_uniforms[program] == nullptr || location < 0)
		return;
	// Array elements occupy consecutive locations.
	for (GLsizei i = 0; i < count && location + i < kUniformLocations; ++i)
		g_uniforms[program]->values[location + i].tag = 0;
}

void LinkProgram(GLuint program)
{
	// Linking resets every uniform of the program to its default.
	ResetProgramUniforms(program);
	glad_glLinkProgram(program);
}

void DeleteProgram(GLuint program)
{
	// A deleted program stays current until replaced, so the binding itself
	// is unchanged; only its name may be reused afterwards.
	ResetProgramUniforms(program);
	glad_glDeleteProgram(program);
}

void DeleteTextures(GLsizei n, const GLuint* textures)
{
	// GL unbinds a deleted texture from every unit and may hand the name out
	// again, so a stale entry could otherwise skip a needed bind.
	for (GLsizei i = 0; i < n; ++i)
	{
		if (textures[i] == 0)
			continue;
		ForgetTexture(g_state.texture2D, textures[i]);
		ForgetTexture(g_state.texture2DArray, textures[i]);
	}
	glad_glDeleteTextures(n, textures);
}

void DeleteVertexArrays(GLsizei n, const GLuint* arrays)
{
	for (GLsizei i = 0; i < n; ++i)
	{
		if (arrays[i] != 0 && g_state.vertexArray == arrays[i])
			g_state.vertexArray = 0;
	}
	glad_glDeleteVertexArrays(n, arrays);
}

void DeleteFramebuffers(GLsizei n, const GLuint* framebuffers)
{
	for (GLsizei i = 0; i < n; ++i)
	{
		if (framebuffers[i] == 0)
			continue;
		if (g_state.readFramebuffer == framebuffers[i])
			g_state.readFramebuffer = 0;
		if (g_state.drawFramebuffer == framebuffers[i])
			g_state.drawFramebuffer = 0;
	}
	glad_glDeleteFramebuffers(n, framebuffers);
}

void Sync()
{
	State& s = g_state;
	s.caps = 0;
	for (size_t i = 0; i < sizeof(kCaps) / sizeof(kCaps[0]); ++i)
	{
		if (glad_glIsEnabled(kCaps[i]))
			s.caps |= detail::CapBit(kCaps[i]);
	}

	glad_glGetBooleanv(GL_DEPTH_WRITEMASK, &s.depthMask);
	s.depthFunc = static_cast<GLenum>(RealInteger(GL_DEPTH_FUNC));
	glad_glGetDoublev(GL_DEPTH_RANGE, s.depthRange);
	s.blendSrcRgb = static_cast<GLenum>(RealInteger(GL_BLEND_SRC_RGB));
	s.blendDstRgb = static_cast<GLenum>(RealInteger(GL_BLEND_DST_RGB));
	s.blendSrcAlpha = static_cast<GLenum>(RealInteger(GL_BLEND_SRC_ALPHA));
	s.blendDstAlpha = static_cast<GLenum>(RealInteger(GL_BLEND_DST_ALPHA));
	s.blendEquationRgb = static_cast<GLenum>(RealInteger(GL_BLEND_EQUATION_RGB));
	s.blendEquationAlpha = static_cast<GLenum>(RealInteger(GL_BLEND_EQUATION_ALPHA));
	glad_glGetBooleanv(GL_COLOR_WRITEMASK, s.colorMask);
	glad_glGetIntegerv(GL_SCISSOR_BOX, s.scissor);
	glad_glGetIntegerv(GL_VIEWPORT, s.viewport);
	s.cullFaceMode = static_cast<GLenum>(RealInteger(GL_CULL_FACE_MODE));
	s.frontFace = static_cast<GLenum>(RealInteger(GL_FRONT_FACE));
	s.stencilFunc = static_cast<GLenum>(RealInteger(GL_STENCIL_FUNC));
	s.stencilRef = RealInteger(GL_STENCIL_REF);
	s.stencilValueMask = static_cast<GLuint>(RealInteger(GL_STENCIL_VALUE_MASK));
	s.stencilFail = static_cast<GLenum>(RealInteger(GL_STENCIL_FAIL));
	s.stencilDepthFail = static_cast<GLenum>(RealInteger(GL_STENCIL_PASS_DEPTH_FAIL));
	s.stencilDepthPass = static_cast<GLenum>(RealInteger(GL_STENCIL_PASS_DEPTH_PASS));
	s.stencilWriteMask = static_cast<GLuint>(RealInteger(GL_STENCIL_WRITEMASK));
	glad_glGetFloatv(GL_POINT_SIZE, &s.pointSize);
	s.program = static_cast<GLuint>(RealInteger(GL_CURRENT_PROGRAM));
	s.vertexArray = static_cast<GLuint>(RealInteger(GL_VERTEX_ARRAY_BINDING));
	s.readFramebuffer = static_cast<GLuint>(RealInteger(GL_READ_FRAMEBUFFER_BINDING));
	s.drawFramebuffer = static_cast<GLuint>(RealInteger(GL_DRAW_FRAMEBUFFER_BINDING));
	s.activeTexture = static_cast<GLenum>(RealInteger(GL_ACTIVE_TEXTURE));
	for (int unit = 0; unit < kTextureUnits; ++unit)
	{
		glad_glActiveTexture(GL_TEXTURE0 + unit);
		s.texture2D[unit] = static_cast<GLuint>(RealInteger(GL_TEXTURE_BINDING_2D));
		s.texture2DArray[unit] = static_cast<GLuint>(RealInteger(GL_TEXTURE_BINDING_2D_ARRAY));
	}
	glad_glActiveTexture(s.activeTexture);
	// GL_STENCIL_REF reads back clamped to the bound framebuffer's stencil
	// range (zero on the default framebuffer), so the stored reference may not
	// be the real one; the next glStencilFunc re-establishes it.
	s.known = 0xFFFFFFFFu & ~static_cast<uint32_t>(Known_StencilFunc);

	for (GLuint program = 0; program < kPrograms; ++program)
		ResetProgramUniforms(program);

	static bool s_announced = false;
	if (VerifyEnabled() && !s_announced)
	{
		s_announced = true;
		std::fprintf(stderr, "glstate: CANE_GLSTATE_VERIFY enabled\n");
	}
}

void Verify(const char* where)
{
	if (!VerifyEnabled())
		return;

	static unsigned long s_mismatches = 0;
	static unsigned long s_checks = 0;
	++s_checks;
	unsigned long mismatches = 0;
	auto report = [&](const char* what, double mirrored, double real)
	{
		++mismatches;
		if (++s_mismatches <= 64)
			std::fprintf(stderr, "GLSTATE MISMATCH at %s: %s mirror=%.9g driver=%.9g\n", where, what, mirrored, real);
	};
	auto checkInt = [&](uint32_t bit, const char* what, GLenum pname, GLint mirrored)
	{
		if (!detail::Known(bit))
			return;
		const GLint real = RealInteger(pname);
		if (real != mirrored)
			report(what, mirrored, real);
	};

	const State& s = g_state;
	for (size_t i = 0; i < sizeof(kCaps) / sizeof(kCaps[0]); ++i)
	{
		const uint32_t bit = detail::CapBit(kCaps[i]);
		if (!detail::Known(bit))
			continue;
		const bool mirrored = (s.caps & bit) != 0;
		const bool real = glad_glIsEnabled(kCaps[i]) != GL_FALSE;
		if (mirrored != real)
			report(kCapNames[i], mirrored, real);
	}

	if (detail::Known(Known_DepthMask))
	{
		GLboolean real = GL_FALSE;
		glad_glGetBooleanv(GL_DEPTH_WRITEMASK, &real);
		if (real != s.depthMask)
			report("GL_DEPTH_WRITEMASK", s.depthMask, real);
	}
	checkInt(Known_DepthFunc, "GL_DEPTH_FUNC", GL_DEPTH_FUNC, static_cast<GLint>(s.depthFunc));
	if (detail::Known(Known_DepthRange))
	{
		GLdouble real[2] = {};
		glad_glGetDoublev(GL_DEPTH_RANGE, real);
		for (int i = 0; i < 2; ++i)
		{
			// Drivers may keep the range in single precision.
			if (std::fabs(real[i] - s.depthRange[i]) > 1e-6)
				report("GL_DEPTH_RANGE", s.depthRange[i], real[i]);
		}
	}
	checkInt(Known_BlendFunc, "GL_BLEND_SRC_RGB", GL_BLEND_SRC_RGB, static_cast<GLint>(s.blendSrcRgb));
	checkInt(Known_BlendFunc, "GL_BLEND_DST_RGB", GL_BLEND_DST_RGB, static_cast<GLint>(s.blendDstRgb));
	checkInt(Known_BlendFunc, "GL_BLEND_SRC_ALPHA", GL_BLEND_SRC_ALPHA, static_cast<GLint>(s.blendSrcAlpha));
	checkInt(Known_BlendFunc, "GL_BLEND_DST_ALPHA", GL_BLEND_DST_ALPHA, static_cast<GLint>(s.blendDstAlpha));
	checkInt(Known_BlendEquation, "GL_BLEND_EQUATION_RGB", GL_BLEND_EQUATION_RGB, static_cast<GLint>(s.blendEquationRgb));
	checkInt(Known_BlendEquation, "GL_BLEND_EQUATION_ALPHA", GL_BLEND_EQUATION_ALPHA, static_cast<GLint>(s.blendEquationAlpha));
	if (detail::Known(Known_ColorMask))
	{
		GLboolean real[4] = {};
		glad_glGetBooleanv(GL_COLOR_WRITEMASK, real);
		for (int i = 0; i < 4; ++i)
		{
			if (real[i] != s.colorMask[i])
				report("GL_COLOR_WRITEMASK", s.colorMask[i], real[i]);
		}
	}
	if (detail::Known(Known_Scissor))
	{
		GLint real[4] = {};
		glad_glGetIntegerv(GL_SCISSOR_BOX, real);
		for (int i = 0; i < 4; ++i)
		{
			if (real[i] != s.scissor[i])
				report("GL_SCISSOR_BOX", s.scissor[i], real[i]);
		}
	}
	if (detail::Known(Known_Viewport))
	{
		GLint real[4] = {};
		glad_glGetIntegerv(GL_VIEWPORT, real);
		for (int i = 0; i < 4; ++i)
		{
			if (real[i] != s.viewport[i])
				report("GL_VIEWPORT", s.viewport[i], real[i]);
		}
	}
	checkInt(Known_CullFaceMode, "GL_CULL_FACE_MODE", GL_CULL_FACE_MODE, static_cast<GLint>(s.cullFaceMode));
	checkInt(Known_FrontFace, "GL_FRONT_FACE", GL_FRONT_FACE, static_cast<GLint>(s.frontFace));
	checkInt(Known_StencilFunc, "GL_STENCIL_FUNC", GL_STENCIL_FUNC, static_cast<GLint>(s.stencilFunc));
	if (detail::Known(Known_StencilFunc))
	{
		// Queries clamp the reference to the stencil range of the bound draw
		// framebuffer, which is empty on the default framebuffer.
		const GLint real = RealInteger(GL_STENCIL_REF);
		const GLint bits = DrawFramebufferStencilBits();
		const GLint maxRef = bits >= 31 ? 0x7FFFFFFF : (1 << bits) - 1;
		const GLint clamped = s.stencilRef < 0 ? 0 : (s.stencilRef > maxRef ? maxRef : s.stencilRef);
		if (real != clamped && real != s.stencilRef)
			report("GL_STENCIL_REF", s.stencilRef, real);
	}
	checkInt(Known_StencilFunc, "GL_STENCIL_VALUE_MASK", GL_STENCIL_VALUE_MASK, static_cast<GLint>(s.stencilValueMask));
	checkInt(Known_StencilOp, "GL_STENCIL_FAIL", GL_STENCIL_FAIL, static_cast<GLint>(s.stencilFail));
	checkInt(Known_StencilOp, "GL_STENCIL_PASS_DEPTH_FAIL", GL_STENCIL_PASS_DEPTH_FAIL, static_cast<GLint>(s.stencilDepthFail));
	checkInt(Known_StencilOp, "GL_STENCIL_PASS_DEPTH_PASS", GL_STENCIL_PASS_DEPTH_PASS, static_cast<GLint>(s.stencilDepthPass));
	checkInt(Known_StencilMask, "GL_STENCIL_WRITEMASK", GL_STENCIL_WRITEMASK, static_cast<GLint>(s.stencilWriteMask));
	if (detail::Known(Known_PointSize))
	{
		GLfloat real = 0.0f;
		glad_glGetFloatv(GL_POINT_SIZE, &real);
		if (real != s.pointSize)
			report("GL_POINT_SIZE", s.pointSize, real);
	}
	checkInt(Known_Program, "GL_CURRENT_PROGRAM", GL_CURRENT_PROGRAM, static_cast<GLint>(s.program));
	checkInt(Known_VertexArray, "GL_VERTEX_ARRAY_BINDING", GL_VERTEX_ARRAY_BINDING, static_cast<GLint>(s.vertexArray));
	checkInt(Known_ReadFramebuffer, "GL_READ_FRAMEBUFFER_BINDING", GL_READ_FRAMEBUFFER_BINDING, static_cast<GLint>(s.readFramebuffer));
	checkInt(Known_DrawFramebuffer, "GL_DRAW_FRAMEBUFFER_BINDING", GL_DRAW_FRAMEBUFFER_BINDING, static_cast<GLint>(s.drawFramebuffer));
	checkInt(Known_ActiveTexture, "GL_ACTIVE_TEXTURE", GL_ACTIVE_TEXTURE, static_cast<GLint>(s.activeTexture));

	const GLint realActive = RealInteger(GL_ACTIVE_TEXTURE);
	for (int unit = 0; unit < kTextureUnits; ++unit)
	{
		glad_glActiveTexture(GL_TEXTURE0 + unit);
		const GLuint real2D = static_cast<GLuint>(RealInteger(GL_TEXTURE_BINDING_2D));
		const GLuint real2DArray = static_cast<GLuint>(RealInteger(GL_TEXTURE_BINDING_2D_ARRAY));
		if (s.texture2D[unit] != kUnknownName && s.texture2D[unit] != real2D)
		{
			char what[48];
			std::snprintf(what, sizeof(what), "GL_TEXTURE_BINDING_2D[unit %d]", unit);
			report(what, s.texture2D[unit], real2D);
		}
		if (s.texture2DArray[unit] != kUnknownName && s.texture2DArray[unit] != real2DArray)
		{
			char what[48];
			std::snprintf(what, sizeof(what), "GL_TEXTURE_BINDING_2D_ARRAY[unit %d]", unit);
			report(what, s.texture2DArray[unit], real2DArray);
		}
	}
	glad_glActiveTexture(static_cast<GLenum>(realActive));

	for (GLuint program = 1; program < kPrograms; ++program)
	{
		const ProgramUniforms* uniforms = g_uniforms[program];
		if (uniforms == nullptr || !glad_glIsProgram(program))
			continue;
		for (GLint location = 0; location < kUniformLocations; ++location)
		{
			const UniformValue& cached = uniforms->values[location];
			if (cached.tag == 0)
				continue;
			uint32_t real[16] = {};
			switch (cached.tag & 0xFF00u)
			{
			case detail::UniformFloat: glad_glGetUniformfv(program, location, reinterpret_cast<GLfloat*>(real)); break;
			case detail::UniformInt: glad_glGetUniformiv(program, location, reinterpret_cast<GLint*>(real)); break;
			default: glad_glGetUniformuiv(program, location, reinterpret_cast<GLuint*>(real)); break;
			}
			const uint32_t words = cached.tag & 0xFFu;
			if (std::memcmp(real, cached.data, words * sizeof(uint32_t)) != 0)
			{
				char what[64];
				std::snprintf(what, sizeof(what), "uniform program %u location %d word0", program, location);
				report(what, cached.data[0], real[0]);
			}
		}
	}

	if (mismatches != 0)
	{
		// Continue from the driver's real state so one missed site does not
		// cascade into every later check.
		Sync();
	}
	if (s_checks % 1200 == 0)
		std::fprintf(stderr, "glstate: %lu checks, %lu mismatches\n", s_checks, s_mismatches);
}
}
