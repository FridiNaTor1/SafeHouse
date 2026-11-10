#include "shadow.h"
#include "dysh.h"
#ifdef CANE_DEKO3D
#include "../platform/switch/sly1/deko/backend.h"
#endif
#include <algorithm>
#include <cmath>
#include <iostream>

namespace
{
	GLuint shadowTextureArray = 0;
	int shadowTextureLayerCount = 0;

	// Static sources are resampled into this RGBA8 layer exactly as they used
	// to be into the array, then their alpha is copied into the R8 array.
	// It lives only while AllocateShadows fills the array.
	GLuint shadowStagingTexture = 0;
	GLuint shadowAlphaProgram = 0;
	GLuint shadowAlphaVao = 0;

#ifdef CANE_DEKO3D
	// Precompiled from platform/switch/sly1/deko/shaders/shadowalpha.*
	bool EnsureShadowAlphaProgram()
	{
		if (shadowAlphaProgram != 0)
			return true;

		shadowAlphaProgram = CaneDeko::CreateProgram("shadowalpha.vert", nullptr, "shadowalpha.frag");
		if (shadowAlphaVao == 0)
			glGenVertexArrays(1, &shadowAlphaVao);
		return true;
	}
#else
	const char* const kShadowAlphaVertexSource = R"(#version 430 core
void main()
{
    // One triangle that covers the viewport.
    vec2 pos = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(pos * 2.0 - 1.0, 0.0, 1.0);
}
)";

	// texelFetch reads each staging texel unfiltered, so every layer texel is
	// the bit-exact 8-bit alpha the RGBA8 layer used to hold.
	const char* const kShadowAlphaFragmentSource = R"(#version 430 core
layout(binding = 0) uniform sampler2DArray sourceLayer;
out vec4 FragColor;
void main()
{
    FragColor = vec4(texelFetch(sourceLayer, ivec3(gl_FragCoord.xy, 0), 0).a);
}
)";

	GLuint CompileShadowAlphaShader(GLenum type, const char* source)
	{
		const GLuint shader = glCreateShader(type);
		glShaderSource(shader, 1, &source, nullptr);
		glCompileShader(shader);

		GLint compiled = GL_FALSE;
		glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
		if (compiled == GL_FALSE)
		{
			char infoLog[1024] = {};
			glGetShaderInfoLog(shader, sizeof(infoLog), nullptr, infoLog);
			std::cout << "Shadow alpha shader compilation failed:\n" << infoLog << "\n";
			glDeleteShader(shader);
			return 0;
		}

		return shader;
	}

	bool EnsureShadowAlphaProgram()
	{
		if (shadowAlphaProgram != 0)
			return true;

		const GLuint vertexShader = CompileShadowAlphaShader(GL_VERTEX_SHADER, kShadowAlphaVertexSource);
		const GLuint fragmentShader = CompileShadowAlphaShader(GL_FRAGMENT_SHADER, kShadowAlphaFragmentSource);
		if (vertexShader == 0 || fragmentShader == 0)
		{
			if (vertexShader != 0)
				glDeleteShader(vertexShader);
			if (fragmentShader != 0)
				glDeleteShader(fragmentShader);
			return false;
		}

		const GLuint program = glCreateProgram();
		glAttachShader(program, vertexShader);
		glAttachShader(program, fragmentShader);
		glLinkProgram(program);
		glDeleteShader(vertexShader);
		glDeleteShader(fragmentShader);

		GLint linked = GL_FALSE;
		glGetProgramiv(program, GL_LINK_STATUS, &linked);
		if (linked == GL_FALSE)
		{
			char infoLog[1024] = {};
			glGetProgramInfoLog(program, sizeof(infoLog), nullptr, infoLog);
			std::cout << "Shadow alpha program link failed:\n" << infoLog << "\n";
			glDeleteProgram(program);
			return false;
		}

		// Core profile draws need a vertex array object, even without attributes.
		if (shadowAlphaVao == 0)
			glGenVertexArrays(1, &shadowAlphaVao);

		shadowAlphaProgram = program;
		return true;
	}
#endif

	void CreateShadowTextureArray(int layerCount)
	{
		shadowTextureLayerCount = layerCount;
		if (layerCount <= 0)
			return;

		// glob.frag samples only the alpha of a shadow layer, so the layers are
		// single-channel and the swizzle returns the stored value as alpha.
		glGenTextures(1, &shadowTextureArray);
		glBindTexture(GL_TEXTURE_2D_ARRAY, shadowTextureArray);
		glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_R8,
			g_gl.dyshWidth, g_gl.dyshHeight, layerCount);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_SWIZZLE_A, GL_RED);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		// The bindless path defaulted each source texture to GL_REPEAT. Per-layer
		// clamp-to-edge exceptions are emulated in the shader from TEX::grftex.
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, GL_REPEAT);
		glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, GL_REPEAT);
		glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
	}

	void DeleteShadowStagingTexture()
	{
		if (shadowStagingTexture != 0)
		{
			glDeleteTextures(1, &shadowStagingTexture);
			shadowStagingTexture = 0;
		}
	}

	bool CopyTextureToShadowLayer(GLuint sourceTexture, int layer)
	{
		if (sourceTexture == 0 || shadowTextureArray == 0 ||
			layer < 0 || layer >= shadowTextureLayerCount)
			return false;

		if (!EnsureShadowAlphaProgram())
			return false;

		GLint sourceWidth = 0;
		GLint sourceHeight = 0;
		glBindTexture(GL_TEXTURE_2D, sourceTexture);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &sourceWidth);
		glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &sourceHeight);

		glBindTexture(GL_TEXTURE_2D, 0);

		if (sourceWidth <= 0 || sourceHeight <= 0)
			return false;

		if (shadowStagingTexture == 0)
		{
			glGenTextures(1, &shadowStagingTexture);
			glBindTexture(GL_TEXTURE_2D_ARRAY, shadowStagingTexture);
			glTexStorage3D(GL_TEXTURE_2D_ARRAY, 1, GL_RGBA8,
				g_gl.dyshWidth, g_gl.dyshHeight, 1);
			glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
		}

		GLint previousReadFbo = 0;
		GLint previousDrawFbo = 0;
		GLboolean previousColorMask[4] = {};
		const GLboolean scissorEnabled = glIsEnabled(GL_SCISSOR_TEST);
		glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &previousReadFbo);
		glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previousDrawFbo);
		glGetBooleanv(GL_COLOR_WRITEMASK, previousColorMask);

		GLuint readFbo = 0;
		GLuint drawFbo = 0;
		glGenFramebuffers(1, &readFbo);
		glGenFramebuffers(1, &drawFbo);
		glBindFramebuffer(GL_READ_FRAMEBUFFER, readFbo);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			GL_TEXTURE_2D, sourceTexture, 0);
		glReadBuffer(GL_COLOR_ATTACHMENT0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, drawFbo);
		glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
			shadowStagingTexture, 0, 0);
		glDrawBuffer(GL_COLOR_ATTACHMENT0);

		bool complete =
			glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE &&
			glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

		if (complete)
		{
			// Shadow layers are persistent. A scissor or color mask left by an
			// earlier pass would otherwise leave stale texels around a static
			// shadow and expose the projector's rectangular footprint.
			glDisable(GL_SCISSOR_TEST);
			glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
			const GLfloat transparent[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			glClearBufferfv(GL_COLOR, 0, transparent);

			glBlitFramebuffer(0, 0, sourceWidth, sourceHeight,
				0, 0, g_gl.dyshWidth, g_gl.dyshHeight,
				GL_COLOR_BUFFER_BIT, GL_LINEAR);

			glFramebufferTextureLayer(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				shadowTextureArray, 0, layer);
			complete = glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
		}

		if (complete)
		{
			GLint previousProgram = 0;
			GLint previousVao = 0;
			GLint previousActiveTexture = 0;
			GLint previousArrayTexture = 0;
			GLint previousViewport[4] = {};
			const GLboolean blendEnabled = glIsEnabled(GL_BLEND);
			const GLboolean depthTestEnabled = glIsEnabled(GL_DEPTH_TEST);
			const GLboolean stencilTestEnabled = glIsEnabled(GL_STENCIL_TEST);
			const GLboolean cullFaceEnabled = glIsEnabled(GL_CULL_FACE);
			glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
			glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &previousVao);
			glGetIntegerv(GL_ACTIVE_TEXTURE, &previousActiveTexture);
			glGetIntegerv(GL_VIEWPORT, previousViewport);
			glActiveTexture(GL_TEXTURE0);
			glGetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &previousArrayTexture);

			glDisable(GL_BLEND);
			glDisable(GL_DEPTH_TEST);
			glDisable(GL_STENCIL_TEST);
			glDisable(GL_CULL_FACE);
			glViewport(0, 0, g_gl.dyshWidth, g_gl.dyshHeight);
			glBindTexture(GL_TEXTURE_2D_ARRAY, shadowStagingTexture);
			glUseProgram(shadowAlphaProgram);
			glBindVertexArray(shadowAlphaVao);
			glDrawArrays(GL_TRIANGLES, 0, 3);

			glBindVertexArray(static_cast<GLuint>(previousVao));
			glUseProgram(static_cast<GLuint>(previousProgram));
			glBindTexture(GL_TEXTURE_2D_ARRAY, static_cast<GLuint>(previousArrayTexture));
			glActiveTexture(static_cast<GLenum>(previousActiveTexture));
			glViewport(previousViewport[0], previousViewport[1], previousViewport[2], previousViewport[3]);
			if (cullFaceEnabled)
				glEnable(GL_CULL_FACE);
			if (stencilTestEnabled)
				glEnable(GL_STENCIL_TEST);
			if (depthTestEnabled)
				glEnable(GL_DEPTH_TEST);
			if (blendEnabled)
				glEnable(GL_BLEND);
		}

		glColorMask(previousColorMask[0], previousColorMask[1],
			previousColorMask[2], previousColorMask[3]);
		if (scissorEnabled)
			glEnable(GL_SCISSOR_TEST);

		glBindFramebuffer(GL_READ_FRAMEBUFFER, previousReadFbo);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, previousDrawFbo);
		glDeleteFramebuffers(1, &readFbo);
		glDeleteFramebuffers(1, &drawFbo);
		return complete;
	}

	bool HasFiniteProjection(const SHADOWBLK& shadow)
	{
		const auto finiteValues = [](const float* values, int count)
		{
			for (int i = 0; i < count; ++i)
			{
				if (!std::isfinite(values[i]))
					return false;
			}
			return true;
		};

		return finiteValues(glm::value_ptr(shadow.matWorldToUv), 16) &&
			finiteValues(glm::value_ptr(shadow.rgba), 4) &&
			std::isfinite(shadow.wMin) &&
			std::isfinite(shadow.wMax) &&
			std::isfinite(shadow.wFadeMin) &&
			finiteValues(glm::value_ptr(shadow.posEffect), 4) &&
			std::isfinite(shadow.sRadiusEffect);
	}

}

void InitSwShadowDl(SW* psw)
{
	InitDl(&psw->dlShadow, offsetof(SHADOW, dle));
}

void InitShadow(SHADOW* pshadow)
{
	pshadow->sNearRadius = -1.0f;
	pshadow->sFarRadius = -1.0f;
	pshadow->sNearCast = 100.0f;
	pshadow->sFarCast = 400.0f;
	pshadow->oidDysh = OID_Nil;
	pshadow->ssboIndex = -1;
	pshadow->textureSlot = -1;
	pshadow->glTexture = 0;
	pshadow->rsh.textureSlot = -1;
	pshadow->rsh.clampS = 0;
	pshadow->rsh.clampT = 0;

	// Initialize up vector to g_normalY
	pshadow->vecUp = glm::vec3(0.0, 1.0, 0.0);

	// Compute normalCast = g_normalZ * -1.0f
	pshadow->normalCast = glm::vec3(-0.0, -0.0, -1.0);
	//pshadow->normalCast.gUnused = -g_normalZ.gUnused;
}

void SetShadowShader(SHADOW* pshadow, OID oidShdShadow)
{
	OID oid = (OID)0x2C;

	if (oidShdShadow != OID_Nil)
		oid = oidShdShadow;

	pshadow->pshd = PshdFindShader(oid);
}

void SetShadowNearRadius(SHADOW* pshadow, float sNearRadius)
{
	pshadow->sNearRadius = sNearRadius;
	if (pshadow->sFarRadius < 0.0) {
		pshadow->sFarRadius = sNearRadius;
	}
}

void SetShadowFarRadius(SHADOW* pshadow, float sFarRadius)
{
	pshadow->sFarRadius = sFarRadius;
	if (pshadow->sNearRadius < 0.0) {
		pshadow->sNearRadius = sFarRadius;
	}
}

void SetShadowNearCast(SHADOW* pshadow, float sNearCast)
{
	pshadow->sNearCast = sNearCast;
	RebuildShadowRegion(pshadow);
}

void SetShadowFarCast(SHADOW* pshadow, float sFarCast)
{
	pshadow->sFarCast = sFarCast;
	RebuildShadowRegion(pshadow);
}

void SetShadowConeAngle(SHADOW* pshadow, float degConeAngle)
{
	// Convert degrees to radians: 0.008726647 / 360
	float coneTangent = std::tan(degConeAngle * 0.008726647f);

	pshadow->sNearRadius = coneTangent * pshadow->sNearCast;
	pshadow->sFarRadius = coneTangent * pshadow->sFarCast;
}

void SetShadowFrustrumUp(SHADOW* pshadow, glm::vec3* pvecUp)
{
	pshadow->vecUp = *pvecUp;
}

void SetShadowCastPosition(SHADOW* pshadow, const glm::vec3& posCast)
{
	if (!glm::all(glm::epsilonEqual(pshadow->posCast, posCast, 0.0001f))) {
		pshadow->posCast = posCast;
		RebuildShadowRegion(pshadow);
	}
}

void SetShadowCastNormal(SHADOW* pshadow, const glm::vec3& normalCast)
{
	// Avoid unnecessary updates if the normal hasn't changed significantly
	if (!glm::all(glm::epsilonEqual(pshadow->normalCast, normalCast, 0.0001f))) {
		pshadow->normalCast = normalCast;
		RebuildShadowRegion(pshadow);
	}
}

int FShadowValid(SHADOW* pshadow, GRFGLOB grfglob)
{
	if ((grfglob & 1) != 0 && pshadow->pshd->shdk == 2)
		return true;

	if ((grfglob & 2) != 0 && pshadow->pshd->shdk == 3)
		return true;

	return false;
}

int FShadowRadiusSet(SHADOW* shadow)
{
	return (shadow->sNearRadius >= 0.0f && shadow->sFarRadius >= 0.0f);
}

int FShadowIntersectsSphere(SHADOW* pshadow, const glm::vec3& pos, float sRadius)
{
	uint32_t zon = pshadow->pshd->grfzon;
	bool zoneValid = (zon & 0x10000000) != 0 || (g_pcm->grfzon & zon) == g_pcm->grfzon;

	if (!zoneValid) return 0;

	// Convert posEffect (assumed to be glm::vec4 or VECTOR) to vec3 for distance
	glm::vec3 shadowPos = glm::vec3(pshadow->posEffect.x, pshadow->posEffect.y, pshadow->posEffect.z);

	float totalRadius = sRadius + pshadow->sRadiusEffect;
	float distSq = glm::distance2(pos, shadowPos);

	return distSq <= totalRadius * totalRadius;
}

void FindSwShadows(SW* psw, glm::vec3* ppos, float sRadius, int cpshadowMax, int* pcpshadow, SHADOW** apshadow)
{
	int cpshadow = 0;
	SHADOW* pshadow = psw->dlShadow.pshadowFirst;

	while (pshadow != nullptr)
	{
		if (FShadowIntersectsSphere(pshadow, *ppos, sRadius))
		{
			if (cpshadow >= cpshadowMax)
			{
				*pcpshadow = cpshadow;
				return;
			}

			*apshadow = pshadow;
			++apshadow;
			++cpshadow;
		}

		pshadow = pshadow->dle.pshadowNext;
	}

	*pcpshadow = cpshadow;
}

void PostShadowLoad(SHADOW* pshadow)
{
	if (pshadow->pshd == nullptr)
		SetShadowShader(pshadow, (OID)0x2C);

	if (!FShadowRadiusSet(pshadow))
	{
		SetShadowNearRadius(pshadow, 100.0f);
		SetShadowFarRadius(pshadow, 400.0f);
	}
}

void RebuildShadowRegion(SHADOW* pshadow)
{
	float nearRadius = pshadow->sNearRadius;
	float farRadius = pshadow->sFarRadius;
	float nearCast = pshadow->sNearCast;
	float farCast = pshadow->sFarCast;

	float effectDistance = 0.0f;
	float effectRadius = 0.0f;

	if (!FFloatsNear(nearRadius, farRadius, 0.0001f))
	{
		float dCast = farCast - nearCast;

		// Solve for the base offset of the shadow parabola
		float base = (nearCast * farRadius - farCast * nearRadius) / (nearRadius - farRadius);

		// Compute the midpoint position along the parabola
		float height = std::sqrt(dCast * dCast + farRadius * farRadius);
		float z1 = ((nearCast + base) * height) / dCast;
		float focus = (z1 * (z1 + 0.5f * height)) / (nearCast + base);

		// Compute final effect position and radius
		float z2 = (farCast + base) - focus;
		effectRadius = std::sqrt(z2 * z2 + farRadius * farRadius);
		effectDistance = focus - base;
	}
	else
	{
		// Fallback: cone case or degenerate radius
		float halfLength = std::abs(farCast - nearCast) * 0.5f;
		effectDistance = nearCast + halfLength;
		effectRadius = halfLength;
	}

	pshadow->sRadiusEffect = effectRadius;

	// Calculate posEffect = posCast + normalCast * effectDistance
	pshadow->posEffect.x = pshadow->posCast.x + pshadow->normalCast.x * effectDistance;
	pshadow->posEffect.y = pshadow->posCast.y + pshadow->normalCast.y * effectDistance;
	pshadow->posEffect.z = pshadow->posCast.z + pshadow->normalCast.z * effectDistance;
	//pshadow->posEffect.gUnused = pshadow->posCast.gUnused + pshadow->normalCast.gUnused * effectDistance;
}

void CombineShadowEyeLookAtProj(const glm::vec3& posEye, const glm::mat3& matLookAt, const glm::mat4& matProj, glm::mat4& out)
{
	glm::mat4 mat(1.0f);

	mat[0] = glm::vec4(-matLookAt[1], 0.0f);
	mat[1] = glm::vec4(-matLookAt[2], 0.0f);
	mat[2] = glm::vec4(matLookAt[0], 0.0f);
	mat[3] = glm::vec4(posEye, 1.0f);

	out = matProj * glm::inverse(mat);
}

int FFilterFastShadows(void*, void* pvso)
{
	SO* pso = static_cast<SO*>(pvso);

	if (pso->fNoXpsSelf != 0)
		return 0;

	return !pso->mpisurfihsgMic.empty();
}

void UpdateShadow(SHADOW* pshadow, float)
{
	/*if (g_droSnap.fDisableShadows != 0)
		return;*/

	std::vector <SO*> apso;
	IntersectSwBoundingSphere(g_psw, nullptr, &pshadow->posEffect, pshadow->sRadiusEffect, FFilterFastShadows, nullptr, apso);

	const glm::vec3 posNear = pshadow->posCast + pshadow->normalCast * pshadow->sNearCast;
	const glm::vec3 posFar = pshadow->posCast + pshadow->normalCast * pshadow->sFarCast;

	for (SO* pso : apso)
	{
		LSG lsg{};

		if (ClsgClipEdgeToBsp(pso->bspc.absp.data(), const_cast<glm::vec3*>(&posNear), const_cast<glm::vec3*>(&posFar), nullptr, 1, &lsg) == 0)
			continue;

		SURF* psurf = lsg.data.bsp.apsurf[0];

		if (psurf == nullptr)
			continue;

		const int isurf = static_cast<int>(psurf - pso->geomWorld.asurf.data());
		const int ihsgMin = pso->mpisurfihsgMic[isurf];
		const int ihsgMax = pso->mpisurfihsgMic[isurf + 1];

		for (int ihsg = ihsgMin; ihsg < ihsgMax; ++ihsg)
		{
			HSG& hsg = pso->ahsg[ihsg];
			GLOB& glob = pso->globset.aglob[hsg.ipglob];

			if (!FShadowValid(pshadow, glob.grfglob))
				continue;

			SUBGLOBI& subglobi = pso->globset.aglobi[hsg.ipglob].asubglobi[hsg.ipsubglob];

			if (subglobi.tShadowsValid != g_clock.t)
			{
				subglobi.tShadowsValid = g_clock.t;
				subglobi.cpshadow = 0;
			}

			if (subglobi.cpshadow >= 4)
				return;

			subglobi.apshadow[subglobi.cpshadow++] = pshadow;
		}
	}
}

void RebuildShadow(SHADOW* pshadow)
{
	constexpr float kFadeDistance = 50.0f;

	glm::vec3 posCast = pshadow->posCast;
	glm::vec3 normalCast = pshadow->normalCast;
	glm::vec3 vecUpRef = pshadow->vecUp;

	glm::mat3 matLookAt(1.0f);
	BuildOrthonormalMatrixZ(normalCast, vecUpRef, matLookAt);

	const float sNearCast = pshadow->sNearCast;
	const float sFarCast = pshadow->sFarCast;
	const float sNearRadius = pshadow->sNearRadius;
	const float sFarRadius = pshadow->sFarRadius;

	glm::vec3 posEye = posCast;
	glm::mat4 matProj(1.0f);

	const bool isPerspective = !FFloatsNear(sNearRadius, sFarRadius, 0.0001f);

	if (isPerspective)
	{
		float eyeOffset = (sFarRadius * sNearCast - sNearRadius * sFarCast) / (sFarRadius - sNearRadius);

		float sNear = sNearCast - eyeOffset;
		float sFar = sFarCast - eyeOffset;

		posEye = posCast + normalCast * eyeOffset;

		float scale = 0.4f / (sNearRadius / sNear);

		BuildSimpleProjectionMatrix(scale, scale, 0.5f, 0.5f, sNear, sFar, matProj);

		pshadow->rsh.wMin = std::min(sNear, sFar);
		pshadow->rsh.wMax = std::max(sNear, sFar);
		pshadow->rsh.wFadeMin = pshadow->rsh.wMin - kFadeDistance;
	}
	else
	{
		posEye = posCast;

		matProj = glm::mat4(1.0f);

		matProj[0][0] = 0.5f / sNearRadius;
		matProj[1][1] = 0.5f / sNearRadius;

		matProj[3][0] = 0.5f;
		matProj[3][1] = 0.5f;

		pshadow->rsh.wMin = -FLT_MAX;
		pshadow->rsh.wMax = FLT_MAX;
		pshadow->rsh.wFadeMin = sNearCast - kFadeDistance;
	}

	//CombineEyeLookAtProj(posEye, matLookAt, matProj, pshadow->matWorldToUv);
	CombineShadowEyeLookAtProj(posEye, matLookAt, matProj, pshadow->matWorldToUv);

	pshadow->rsh.matWorldToUv = pshadow->matWorldToUv;
	pshadow->rsh.matClipToUv = glm::mat4(1.0f);

	pshadow->rsh.rgba = pshadow->pshd->rgba;
	pshadow->rsh.shdk = pshadow->pshd->shdk;

	pshadow->rsh.posEffect = glm::vec4(pshadow->posEffect, 1.0f);
	pshadow->rsh.sRadiusEffect = pshadow->sRadiusEffect;

	pshadow->rsh.normalCast = glm::vec4(pshadow->normalCast, 0.0f);

	if (pshadow->rsh.fDynamic == 0)
	{
		const TEX& tex = pshadow->pshd->atex[0];
		const BMP* pbmp = tex.abmp[0];

		// Resolved non-three-way textures are owned by TEX because the same
		// indexed BMP can be referenced with different CLUTs. Prefer its OpenGL
		// texture name, with the legacy BMP-owned texture as a fallback.
		pshadow->glTexture = !tex.glDiffuseMap.empty() && tex.glDiffuseMap[0] != 0
			? tex.glDiffuseMap[0]
			: (pbmp != nullptr ? pbmp->glDiffuseMap : 0);

		// A bindless sampler inherited these flags from the source GL_TEXTURE_2D.
		// Texture-array layers share one sampler, so preserve the source wrapping
		// explicitly and reproduce it in SampleShadowTexture.
		pshadow->rsh.clampS = (tex.grftex & 1) != 0;
		pshadow->rsh.clampT = (tex.grftex & 2) != 0;
	}
	else if (pshadow->pdysh != nullptr)
	{
		pshadow->glTexture = pshadow->pdysh->shadowTex;
		pshadow->rsh.clampS = 0;
		pshadow->rsh.clampT = 0;
	}

	pshadow->rsh.textureSlot = pshadow->textureSlot;
}

void AllocateShadows(SW* psw)
{
	shadowBlk.clear();
	activeShadows = {};

	int requestedLayers = 0;
	for (SHADOW* pshadow = psw->dlShadow.pshadowFirst; pshadow; pshadow = pshadow->dle.pshadowNext)
		++requestedLayers;

	GLint hardwareLayers = 0;
	glGetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS, &hardwareLayers);
	const int layerCount = std::min(requestedLayers,
		std::min(hardwareLayers, MAX_PROJECTED_SHADOW_LAYERS));
	CreateShadowTextureArray(layerCount);

	SHADOW* pshadow = psw->dlShadow.pshadowFirst;
	int slot = 0;
	int ignoredCount = 0;

	while (pshadow != nullptr)
	{
		pshadow->ssboIndex = -1;
		pshadow->textureSlot = -1;
		pshadow->rsh.textureSlot = -1;

		if (slot < layerCount)
		{
			pshadow->ssboIndex = slot;
			pshadow->textureSlot = slot;
			RebuildShadow(pshadow);

			if (pshadow->rsh.fDynamic != 0 && pshadow->pdysh != nullptr)
			{
				pshadow->pdysh->shadowTex = shadowTextureArray;
				pshadow->pdysh->shadowLayer = slot;
				pshadow->glTexture = shadowTextureArray;
			}
			else if (!CopyTextureToShadowLayer(pshadow->glTexture, slot))
			{
				pshadow->glTexture = 0;
			}

			shadowBlk.push_back(pshadow->rsh);
			++slot;
		}
		else
		{
			++ignoredCount;
		}

		pshadow = pshadow->dle.pshadowNext;
	}

	DeleteShadowStagingTexture();

	if (ignoredCount > 0)
	{
		std::cout << "Projected shadow array layer limit reached; ignored "
			<< ignoredCount << " shadow(s).\n";
	}

	// ------------------------------------------------------------
	// Main shadows SSBO
	// ------------------------------------------------------------
	glGenBuffers(1, &shadowSsbo);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, shadowSsbo);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 4, shadowSsbo);

	const GLsizeiptr headerSize = sizeof(SHADOWSSBOHEADER);
	const GLsizeiptr shadowSize = sizeof(SHADOWBLK) * shadowBlk.size();
	const GLsizeiptr totalSize = headerSize + shadowSize;

	glBufferData(GL_SHADER_STORAGE_BUFFER, totalSize, nullptr, GL_DYNAMIC_DRAW);

	SHADOWSSBOHEADER header = {};
	header.numShadows = (int)shadowBlk.size();

	glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, headerSize, &header);
	glBufferSubData(GL_SHADER_STORAGE_BUFFER, headerSize, shadowSize, shadowBlk.data());

	// ------------------------------------------------------------
	// Active-shadows SSBO
	// ------------------------------------------------------------
	glGenBuffers(1, &activeShadowsSsbo);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, activeShadowsSsbo);
	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, activeShadowsSsbo);

	glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(ACTIVESHADOWS), &activeShadows, GL_DYNAMIC_DRAW);

	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void PrepareSwShadows(SW *psw, CM *pcm)
{
	// This must be cleared and uploaded even when the world has no shadows.
	// Otherwise the GPU keeps using the previous frame's active-shadow list.
	activeShadows.numShadows = 0;

	const GLsizeiptr headerSize = sizeof(SHADOWSSBOHEADER);

	glBindBuffer(GL_SHADER_STORAGE_BUFFER, shadowSsbo);

	for (SHADOW* pshadow = psw->dlShadow.pshadowFirst; pshadow; pshadow = pshadow->dle.pshadowNext)
	{
		if (pshadow->ssboIndex < 0 || pshadow->textureSlot < 0)
			continue;

		// DYSH owns the render target for a dynamic projected shadow. Once that
		// owner is deleted, the SHADOW can remain linked in the world for a short
		// time, but it no longer has a valid image to project.
		if (pshadow->rsh.fDynamic != 0 && pshadow->pdysh == nullptr)
			continue;

		// A frozen dynamic-shadow owner no longer contributes its projected
		// shadow. Static projected shadows have no DYSH owner and are unaffected.
		if (pshadow->pdysh != nullptr && pshadow->pdysh->fFrozen)
			continue;

		// Dynamic shadow maps are frame-local in the original renderer. If the
		// owning DYSH was not submitted this frame, do not reuse its previous
		// texture contents (for example, after Sly enters the van).
		if (pshadow->rsh.fDynamic != 0 && pshadow->pdysh != nullptr)
		{
			bool fDyshSubmitted = false;

			for (int i = 0; i < g_dynamicTextureCount; ++i)
			{
				if (g_dynamicTexturePrpl[i].pdysh == pshadow->pdysh)
				{
					fDyshSubmitted = true;
					break;
				}
			}

			if (!fDyshSubmitted)
				continue;
		}

		if (g_fBsp > 0)
		{
			uint32_t grfzon = pshadow->pshd->grfzon;

			bool visibleInZone = ((grfzon & 0x10000000u) != 0) || ((pcm->grfzon & grfzon) == pcm->grfzon);

			if (!visibleInZone)
				continue;
		}

		if (activeShadows.numShadows >= MAX_SHADOWS)
			break;

		// Rebuild this shadow's CPU-side GPU struct
		RebuildShadow(pshadow);

		// A disappearing or temporarily degenerate caster can produce a NaN
		// world-to-UV matrix. Sending it to the fragment shader poisons the
		// projected coordinates and can black out every intersecting object.
		// Treat that frame exactly like a shadow which was not submitted.
		if (!HasFiniteProjection(pshadow->rsh))
			continue;

		// Never expose an unbound sampler as an active projected shadow. OpenGL
		// commonly returns alpha 1 for an incomplete texture, which this shader
		// interprets as a fully opaque shadow over every intersecting object.
		if (shadowTextureArray == 0 || pshadow->glTexture == 0)
			continue;

		// Update only this shadow's slot in the main shadow SSBO
		GLsizeiptr offset = headerSize + GLsizeiptr(pshadow->ssboIndex) * sizeof(SHADOWBLK);
		glBufferSubData(GL_SHADER_STORAGE_BUFFER, offset,  sizeof(SHADOWBLK), &pshadow->rsh);

		ACTIVESHADOWTEST& test = activeShadowTests[activeShadows.numShadows];
		test.posEffect = glm::vec3(pshadow->rsh.posEffect);
		test.sRadiusEffect = pshadow->rsh.sRadiusEffect;
		test.fDynamic = pshadow->rsh.fDynamic;
		test.shdk = pshadow->rsh.shdk;

		// Store original global shadow index
		activeShadows.shadowsIndices[activeShadows.numShadows++] = pshadow->ssboIndex;
	}

	// Update active shadow list
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, activeShadowsSsbo);
	glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(ACTIVESHADOWS), &activeShadows);
	glBindBuffer(GL_SHADER_STORAGE_BUFFER, 0);
}

void BindSwShadowTextures(SW* psw)
{
	(void)psw;
	glActiveTexture(GL_TEXTURE0 + SHADOW_TEXTURE_UNIT);
	glBindTexture(GL_TEXTURE_2D_ARRAY, shadowTextureArray);

	// Material rendering expects texture unit zero to be active.
	glActiveTexture(GL_TEXTURE0);

}

void DeallocateSwShadows()
{
	// Break both sides of every dynamic owner link while SHADOW and DYSH
	// objects are still alive. Their world-object destruction order is not a
	// safe lifetime contract for raw cross-pointers.
	if (g_psw != nullptr)
	{
		for (SHADOW* pshadow = g_psw->dlShadow.pshadowFirst;
			pshadow; pshadow = pshadow->dle.pshadowNext)
		{
			if (pshadow->pdysh != nullptr)
			{
				pshadow->pdysh->shadowTex = 0;
				pshadow->pdysh->shadowLayer = -1;
				pshadow->pdysh->pshadowGen = nullptr;
				pshadow->pdysh = nullptr;
			}
			pshadow->glTexture = 0;
		}
	}

	// Drop the shared array binding before world-owned shadow state is reset.
	glActiveTexture(GL_TEXTURE0 + SHADOW_TEXTURE_UNIT);
	glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
	glActiveTexture(GL_TEXTURE0);

	if (shadowSsbo != 0)
	{
		glDeleteBuffers(1, &shadowSsbo);
		shadowSsbo = 0;
	}

	if (activeShadowsSsbo != 0)
	{
		glDeleteBuffers(1, &activeShadowsSsbo);
		activeShadowsSsbo = 0;
	}

	if (shadowTextureArray != 0)
	{
		glDeleteTextures(1, &shadowTextureArray);
		shadowTextureArray = 0;
	}
	shadowTextureLayerCount = 0;
	DeleteShadowStagingTexture();

	shadowBlk.clear();
	shadowBlk.shrink_to_fit();
	activeShadows = {};
}
GLuint shadowSsbo = 0;
GLuint activeShadowsSsbo = 0;
std::vector <SHADOWBLK> shadowBlk;
ACTIVESHADOWS activeShadows;
ACTIVESHADOWTEST activeShadowTests[MAX_SHADOWS];
DL g_dlShadowPending;
