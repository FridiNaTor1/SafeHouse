#include "render.h"
#include "main.h"
#include "blip.h"
#include "emitter.h"
#include "gl.h"
#include "tv.h"
#include "thndflash.h"
#include "sensor.h"
#include "wm.h"
#include "button.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

void SetGlobDraw(GLOB* pglob)
{
	switch (pglob->rp)
	{
		case RP_DynamicTexture:
		pglob->PFNDRAW = DrawSubGlob;
		break;

		case RP_Background:
		pglob->PFNDRAW = DrawSubGlob;
		break;

		case RP_BlotContext:
		pglob->PFNDRAW = DrawTranslucent;
		break;

		case RP_Opaque:
		pglob->PFNDRAW = DrawSubGlob;
		break;

		case RP_Cutout:
		pglob->PFNDRAW = DrawTranslucent;
		break;

		case RP_ProjVolume:
		pglob->PFNDRAW = DrawProjVolume;
		break;

		case RP_OpaqueAfterProjVolume:
		pglob->PFNDRAW = DrawSubGlob;
		break;

		case RP_CutoutAfterProjVolume:
		pglob->PFNDRAW = DrawTranslucent;
		break;

		case RP_MurkClear:
		pglob->PFNDRAW = DrawMurkClear;
		break;

		case RP_MurkOpaque:
		pglob->PFNDRAW = DrawSubGlob;
		break;

		case RP_MurkFill:
		pglob->PFNDRAW = DrawMurkFill;
		break;

		case RP_Translucent:
		pglob->PFNDRAW = DrawTranslucent;
		break;

		/*case RP_Blip:
		pglob->PFNDRAW = Draw;
		break;*/

		case RP_Foreground:
		pglob->PFNDRAW = DrawSubGlob;
		break;

		case RP_WorldMap:
		pglob->PFNDRAW = DrawSubGlob;
		break;

		case RP_Max:
		pglob->PFNDRAW = DrawSubGlob;
		break;
	}
}

void AllocateRpl()
{
	g_dynamicTexturePrpl.resize(2048);
	g_backGroundPrpl.resize(2048);
	g_backGroundBlendPrpl.resize(2048);
	g_blotContextPrpl.resize(2048);
	g_opaquePrpl.resize(2048);
	g_cutOutBlendAddPrpl.resize(2048);
	g_cutOutPrpl.resize(2048);
	g_celBorderPrpl.resize(2048);
	g_projVolumePrpl.resize(6144);
	g_opaqueAfterProjVolumePrpl.resize(2048);
	g_cutOutAfterProjVolumeAddPrpl.resize(2048);
	g_cutOutAfterProjVolumePrpl.resize(2048);
	g_celBorderAfterProjVolumePrpl.resize(2048);
	g_murkClearPrpl.resize(2048);
	g_murkOpaquePrpl.resize(2048);
	g_murkFillPrpl.resize(2048);
	g_translucentAddPrpl.resize(2048);
	g_translucentPrpl.resize(2048);
	g_translucentCelBorderPrpl.resize(2048);
	g_blipPrpl.resize(2048);
	g_foreGroundPrpl.resize(2048);
	g_worldMapPrpl.resize(2048);
	g_maxPrpl.resize(2048);
	
	g_dynamicTextureCount = 0;
	g_backGroundCount = 0;
	g_backGroundBlendCount = 0;
	g_blotContextCount = 0;
	g_opaqueCount = 0;
	g_cutOutBlendAddCount = 0;
	g_cutOutCount = 0;
	g_celBorderCount = 0;
	g_projVolumeCount = 0;
	g_opaqueAfterProjVolumeCount = 0;
	g_cutOutAfterProjVolumeAddCount = 0;
	g_cutOutAfterProjVolumeCount = 0;
	g_celBorderAfterProjVolumeCount = 0;
	g_murkClearCount = 0;
	g_murkOpaqueCount = 0;
	g_murkFillCount = 0;
	g_translucentAddCount = 0;
	g_translucentCount = 0;
	g_translucentCelBorderCount = 0;
	g_blipCount = 0;
	g_foreGroundCount = 0;
	g_worldMapCount = 0;
}

void RenderSw(SW* psw, CM* pcm)
{
	DLI dlBusyWalker;
	dlBusyWalker.m_pdl = &psw->dlBusy;
	dlBusyWalker.m_ibDle = psw->dlBusy.ibDle;
	dlBusyWalker.m_pdliNext = s_pdliFirst;

	LO* currentObject = psw->dlBusy.ploFirst;

	dlBusyWalker.m_ppv = (currentObject != nullptr) ? reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(currentObject) + dlBusyWalker.m_ibDle) : nullptr;

	s_pdliFirst = &dlBusyWalker;

	while (currentObject != nullptr)
	{
		currentObject->pvtalo->pfnRenderAloAll((ALO*)currentObject, pcm, nullptr);

		currentObject = reinterpret_cast<LO*>(*dlBusyWalker.m_ppv);
		dlBusyWalker.m_ppv = (currentObject != nullptr) ? reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(currentObject) + dlBusyWalker.m_ibDle) : nullptr;
	}

	s_pdliFirst = dlBusyWalker.m_pdliNext;

	DLI dlRealClockWalker;
	dlRealClockWalker.m_pdl = &psw->dlMRDRealClock;
	dlRealClockWalker.m_ibDle = psw->dlMRDRealClock.ibDle;
	dlRealClockWalker.m_pdliNext = s_pdliFirst;

	currentObject = psw->dlMRDRealClock.ploFirst;

	dlRealClockWalker.m_ppv = (currentObject != nullptr) ? reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(currentObject) + dlRealClockWalker.m_ibDle) : nullptr;

	s_pdliFirst = &dlRealClockWalker;

	while (currentObject != nullptr)
	{
		currentObject->pvtalo->pfnRenderAloAll((ALO*)currentObject, pcm, nullptr);

		currentObject = reinterpret_cast<LO*>(*dlRealClockWalker.m_ppv);
		dlRealClockWalker.m_ppv = (currentObject != nullptr) ? reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(currentObject) + dlRealClockWalker.m_ibDle) : nullptr;
	}

	s_pdliFirst = dlRealClockWalker.m_pdliNext;

	DLI dlThndFlashWalker;
	dlThndFlashWalker.m_pdl = &psw->dlThndFlash;
	dlThndFlashWalker.m_ibDle = psw->dlThndFlash.ibDle;
	dlThndFlashWalker.m_pdliNext = s_pdliFirst;

	THNDFLASH* pthndflash = static_cast<THNDFLASH*>(psw->dlThndFlash.pvFirst);
	dlThndFlashWalker.m_ppv = pthndflash != nullptr
		? reinterpret_cast<void**>(reinterpret_cast<std::byte*>(pthndflash) + dlThndFlashWalker.m_ibDle)
		: nullptr;
	s_pdliFirst = &dlThndFlashWalker;

	while (pthndflash != nullptr)
	{
		RenderThndFlash(pthndflash);

		pthndflash = static_cast<THNDFLASH*>(*dlThndFlashWalker.m_ppv);
		dlThndFlashWalker.m_ppv = pthndflash != nullptr
			? reinterpret_cast<void**>(reinterpret_cast<std::byte*>(pthndflash) + dlThndFlashWalker.m_ibDle)
			: nullptr;
	}

	s_pdliFirst = dlThndFlashWalker.m_pdliNext;

}

void RenderSwAloAll(SW* psw, CM* pcm)
{
	for (int i = 0; i < allSWAloObjs.size(); i++)
	{
		CID cid = allSWAloObjs[i]->pvtalo->cid;

		allSWAloObjs[i]->pvtalo->pfnRenderAloAll(allSWAloObjs[i], pcm, nullptr);
	}
}

void RenderSwGlobset(SW* psw, CM* pcm)
{
	for (int i = 0; i < 40; i++)
		allSWAloObjs[i]->pvtalo->pfnRenderAloGlobset(allSWAloObjs[i], pcm, nullptr);
}

void SubmitRpl(RPL* prpl)
{ 
	switch (prpl->rp)
	{
		case RP_DynamicTexture:
		g_dynamicTexturePrpl[g_dynamicTextureCount] = *prpl;
		g_dynamicTexturePrpl[g_dynamicTextureCount].PFNDRAWRPL = DrawDysh;
		g_dynamicTextureCount++;
		break;

		case RP_Background:
		if (prpl->fTransluscentSort == 1)
		{
			g_backGroundBlendPrpl[g_backGroundBlendCount] = *prpl;
			g_backGroundBlendPrpl[g_backGroundBlendCount].PFNDRAWRPL = DrawGlob;
			g_backGroundBlendCount++;
		}
		else
		{
			g_backGroundPrpl[g_backGroundCount] = *prpl;
			g_backGroundPrpl[g_backGroundCount].PFNDRAWRPL = DrawGlob;
			g_backGroundCount++;
		}
		break;

		case RP_BlotContext:
		g_blotContextPrpl[g_blotContextCount] = *prpl;
		// Blot-context entries are an ordered stream.  Ordinary ALO globs do
		// not supply a callback here and use DrawGlob, while TV context entries
		// supply PreTvContext/PostTvContext and must retain those callbacks.
		if (g_blotContextPrpl[g_blotContextCount].PFNDRAWRPL == nullptr)
			g_blotContextPrpl[g_blotContextCount].PFNDRAWRPL = DrawGlob;
		g_blotContextCount++;
		break;

		case RP_Opaque:
		g_opaquePrpl[g_opaqueCount] = *prpl;
		g_opaquePrpl[g_opaqueCount].PFNDRAWRPL = DrawGlob;
		g_opaqueCount++;
		break;

		case RP_Cutout:
		if (prpl->fTransluscentSort == 1)
		{
			g_cutOutPrpl[g_cutOutCount] = *prpl;
			g_cutOutPrpl[g_cutOutCount].PFNDRAWRPL = DrawGlob;
			g_cutOutCount++;
		}
		else
		{
			g_cutOutBlendAddPrpl[g_cutOutBlendAddCount] = *prpl;
			g_cutOutBlendAddPrpl[g_cutOutBlendAddCount].PFNDRAWRPL = DrawGlob;
			g_cutOutBlendAddCount++;
		}
		break;

		case RP_ProjVolume:
		g_projVolumePrpl[g_projVolumeCount] = *prpl;
		g_projVolumePrpl[g_projVolumeCount].PFNDRAWRPL = DrawGlob;
		g_projVolumeCount++;
		break;

		case RP_OpaqueAfterProjVolume:
		g_opaqueAfterProjVolumePrpl[g_opaqueAfterProjVolumeCount] = *prpl;
		g_opaqueAfterProjVolumePrpl[g_opaqueAfterProjVolumeCount].PFNDRAWRPL = DrawGlob;
		g_opaqueAfterProjVolumeCount++;
		break;

		case RP_CutoutAfterProjVolume:
		if (prpl->fTransluscentSort == 1)
		{
			g_cutOutAfterProjVolumePrpl[g_cutOutAfterProjVolumeCount] = *prpl;
			g_cutOutAfterProjVolumePrpl[g_cutOutAfterProjVolumeCount].PFNDRAWRPL = DrawGlob;
			g_cutOutAfterProjVolumeCount++;
		}
		else
		{
			g_cutOutAfterProjVolumeAddPrpl[g_cutOutAfterProjVolumeAddCount] = *prpl;
			g_cutOutAfterProjVolumeAddPrpl[g_cutOutAfterProjVolumeAddCount].PFNDRAWRPL = DrawGlob;
			g_cutOutAfterProjVolumeAddCount++;
		}
		break;

		case RP_MurkClear:
		g_murkClearPrpl[g_murkClearCount] = *prpl;
		g_murkClearPrpl[g_murkClearCount].PFNDRAWRPL = DrawGlob;
		g_murkClearCount++;
		break;

		case RP_MurkOpaque:
		g_murkOpaquePrpl[g_murkOpaqueCount] = *prpl;
		g_murkOpaquePrpl[g_murkOpaqueCount].PFNDRAWRPL = DrawGlob;
		g_murkOpaqueCount++;
		break;

		case RP_MurkFill:
		g_murkFillPrpl[g_murkFillCount] = *prpl;
		g_murkFillPrpl[g_murkFillCount].PFNDRAWRPL = DrawGlob;
		g_murkFillCount++;
		break;

		case RP_Translucent:
		if (prpl->fTransluscentSort == 1)
		{
			g_translucentPrpl[g_translucentCount] = *prpl;
			g_translucentPrpl[g_translucentCount].PFNDRAWRPL = DrawGlob;
			g_translucentCount++;
		}
		else
		{
			g_translucentAddPrpl[g_translucentAddCount] = *prpl;
			g_translucentAddPrpl[g_translucentAddCount].PFNDRAWRPL = DrawGlob;
			g_translucentAddCount++;
		}
		break;

		case RP_Blip:
		g_blipPrpl[g_blipCount] = *prpl;
		g_blipPrpl[g_blipCount].PFNDRAWRPL = DrawBlip;
		g_blipCount++;
		break;

		case RP_Foreground:
		g_foreGroundPrpl[g_foreGroundCount] = *prpl;
		if (g_foreGroundPrpl[g_foreGroundCount].PFNDRAWRPL == nullptr)
			g_foreGroundPrpl[g_foreGroundCount].PFNDRAWRPL = DrawGlob;
		g_foreGroundCount++;
		break;

		case RP_WorldMap:
		g_worldMapPrpl[g_worldMapCount] = *prpl;
		g_worldMapPrpl[g_worldMapCount].PFNDRAWRPL = DrawGlob;
		g_worldMapCount++;
		break;

		case RP_Max:
		g_maxPrpl[g_maxCount] = *prpl;
		g_maxPrpl[g_maxCount].PFNDRAWRPL = DrawGlob;
		g_maxCount++;
		break;
	}

	g_cFrameGlobs++;
	//numFrameObjs += prpl->pglob->asubglob.size();
}

void SubmitRplCel(RPLCEL* prplcel)
{
	switch (prplcel->rp)
	{
		case RP_CelBorder:
		g_celBorderPrpl[g_celBorderCount] = *prplcel;
		g_celBorderCount++;
		break;

		case RP_CelBorderAfterProjVolume:
		g_celBorderAfterProjVolumePrpl[g_celBorderAfterProjVolumeCount] = *prplcel;
		g_celBorderAfterProjVolumeCount++;
		break;

		case RP_TranslucentCelBorder:
		g_translucentCelBorderPrpl[g_translucentCelBorderCount] = *prplcel;
		g_translucentCelBorderCount++;
		break;
	}

	g_cFrameCelGlobs++;
	//numFrameObjs++;
}

namespace
{
struct RPLSORTKEY
{
	float z;
	int irpl;
};

// Sorts a list into the same order as std::stable_sort with compareZ, but
// sorts 8-byte keys and then moves each 704-byte RPL at most once.
void StableSortRplZ(std::vector<RPL>& arpl, int crpl)
{
	static std::vector<RPLSORTKEY> s_akey;
	static std::vector<int> s_airplSrc;
	s_akey.resize(crpl);
	s_airplSrc.resize(crpl);

	for (int i = 0; i < crpl; i++)
		s_akey[i] = { arpl[i].z, i };

	std::stable_sort(s_akey.begin(), s_akey.end(), [](const RPLSORTKEY& key0, const RPLSORTKEY& key1)
	{
		return key0.z > key1.z;
	});

	for (int i = 0; i < crpl; i++)
		s_airplSrc[i] = s_akey[i].irpl;

	// Apply arpl[i] = old arpl[s_airplSrc[i]] one permutation cycle at a time.
	RPL rplTemp;
	for (int i = 0; i < crpl; i++)
	{
		if (s_airplSrc[i] == i)
			continue;

		rplTemp = arpl[i];
		int j = i;
		for (;;)
		{
			const int k = s_airplSrc[j];
			s_airplSrc[j] = j;
			if (k == i)
			{
				arpl[j] = rplTemp;
				break;
			}
			arpl[j] = arpl[k];
			j = k;
		}
	}
}

// Upper bound on this frame's ropStream appends: each RPL in these lists is
// drawn once, and each draw callback appends at most one ROGL.
int CountRopStreamRpl()
{
	return g_backGroundCount + g_backGroundBlendCount + g_blotContextCount +
		g_opaqueCount + g_cutOutBlendAddCount + g_cutOutCount + g_projVolumeCount +
		g_opaqueAfterProjVolumeCount + g_cutOutAfterProjVolumeAddCount + g_cutOutAfterProjVolumeCount +
		g_murkClearCount + g_murkOpaqueCount + g_murkFillCount +
		g_translucentAddCount + g_translucentCount +
		g_foreGroundCount + g_worldMapCount + g_maxCount;
}

void PrepareBlipGroups();

// The glob program's per-draw parameters. Like the uniforms they replace, a
// field keeps its value until a draw path sets it again (some paths inherit
// rko, the subglob sphere or projectedVolumeColor from earlier draws).
DRAWGL s_drawgl{};
int s_grfglobRo = 0; // grfglob of the ROGL that s_drawgl.iro names
bool s_fDrawglAppended = false;
uint32_t s_iwindowDrawgl = 0;
int s_idrawgl = 0;
int s_cdrawRecord = 0;
int s_cdrawRecordLast = 0;

// Inputs of the candidate list currently in s_drawgl.
bool s_fShadowCandidatesValid = false;
int s_grfglobShadowCandidates = 0;
glm::vec3 s_posShadowCandidates{};
float s_sRadiusShadowCandidates = 0.0f;

constexpr int MAX_OBJECT_SHADOWS = 12;

// The vertex shader's FShadowValid and sphere tests over the active shadows,
// in the same order. The shader still applies the per-vertex normal test of
// dynamic shadows, and repeats any sphere test whose float result could round
// differently on the GPU (fused or reordered operations, flushed denormals,
// NaN), for the candidates flagged in grfshadowTest.
void BuildShadowCandidates(DRAWGL* pdrawgl, int grfglob)
{
	pdrawgl->cshadow = 0;
	pdrawgl->grfshadowTest = 0;
	std::memset(pdrawgl->ashadow, 0, sizeof(pdrawgl->ashadow));

	if (activeShadows.numShadows <= 0 || (grfglob & 3) == 0)
		return;

	const glm::vec3 pos = pdrawgl->subGlobPosCenter;
	const float sRadius = pdrawgl->subGlobRadius;
	int ccertain = 0;

	for (int i = 0; i < activeShadows.numShadows; ++i)
	{
		const ACTIVESHADOWTEST& test = activeShadowTests[i];

		if (!(((grfglob & 1) != 0 && test.shdk == 2) || ((grfglob & 2) != 0 && test.shdk == 3)))
			continue;

		const float dx = pos.x - test.posEffect.x;
		const float dy = pos.y - test.posEffect.y;
		const float dz = pos.z - test.posEffect.z;
		const float r = sRadius + test.sRadiusEffect;
		const float dd = dx * dx + dy * dy + dz * dz;
		const float rr = r * r;

		bool fTest = test.fDynamic == 1;
		if (!(dd >= 1e-30f && rr >= 1e-30f && std::fabs(dd - rr) > 1e-5f * (dd + rr)))
			fTest = true;
		else if (!(dd <= rr))
			continue;

		if (pdrawgl->cshadow == SHADOW_CANDIDATES_MAX)
		{
			// The first MAX_OBJECT_SHADOWS survivors may lie further on.
			pdrawgl->cshadow = SHADOW_CANDIDATES_ALL;
			pdrawgl->grfshadowTest = 0;
			return;
		}

		const int ishadow = pdrawgl->cshadow++;
		pdrawgl->ashadow[ishadow >> 2] |= static_cast<uint32_t>(activeShadows.shadowsIndices[i]) << ((ishadow & 3) * 8);

		if (fTest)
			pdrawgl->grfshadowTest |= 1u << ishadow;
		else if (++ccertain == MAX_OBJECT_SHADOWS)
			return; // the first MAX_OBJECT_SHADOWS survivors are all listed
	}
}
}

DRAWGL* PdrawglEdit()
{
	s_fDrawglAppended = false;
	return &s_drawgl;
}

void AppendGlobRo(const RO* pro)
{
	s_drawgl.iro = AppendArrayStream(&ropStream, pro, sizeof(ROGL));
	s_grfglobRo = pro->grfglob;
	s_fDrawglAppended = false;
}

int IdrawglCurrent()
{
	if (s_fDrawglAppended && s_iwindowDrawgl == drawStream.iwindow)
		return s_idrawgl;

	if (!s_fShadowCandidatesValid || s_grfglobShadowCandidates != (s_grfglobRo & 3) ||
		std::memcmp(&s_posShadowCandidates, &s_drawgl.subGlobPosCenter, sizeof(glm::vec3)) != 0 ||
		std::memcmp(&s_sRadiusShadowCandidates, &s_drawgl.subGlobRadius, sizeof(float)) != 0)
	{
		BuildShadowCandidates(&s_drawgl, s_grfglobRo);
		s_fShadowCandidatesValid = true;
		s_grfglobShadowCandidates = s_grfglobRo & 3;
		s_posShadowCandidates = s_drawgl.subGlobPosCenter;
		s_sRadiusShadowCandidates = s_drawgl.subGlobRadius;
	}

	s_idrawgl = AppendArrayStream(&drawStream, &s_drawgl, sizeof(DRAWGL));
	s_iwindowDrawgl = drawStream.iwindow;
	s_fDrawglAppended = true;
	s_cdrawRecord++;
	return s_idrawgl;
}

namespace
{
void DrawGlobElements(int baseVertex, int firstIndex, int indexCount)
{
	const GLuint idrawgl = static_cast<GLuint>(IdrawglCurrent());
	glDrawElementsInstancedBaseVertexBaseInstance(GL_TRIANGLES, (GLsizei)indexCount, GL_UNSIGNED_INT,
		(void*)(uintptr_t)(firstIndex * sizeof(uint32_t)), 1, (GLint)baseVertex, idrawgl);
}
}

void SortRenderRpl()
{
	if (g_backGroundBlendCount > 1)
		StableSortRplZ(g_backGroundBlendPrpl, g_backGroundBlendCount);

	if (g_cutOutCount > 1)
		StableSortRplZ(g_cutOutPrpl, g_cutOutCount);

	if (g_cutOutAfterProjVolumeCount > 1)
		StableSortRplZ(g_cutOutAfterProjVolumePrpl, g_cutOutAfterProjVolumeCount);

	if (g_translucentCount > 1)
		StableSortRplZ(g_translucentPrpl, g_translucentCount);
}

inline bool compareZ(const RPL& prpl0, const RPL& prpl1)
{
	return prpl0.z > prpl1.z;
}

void DrawSw(SW* psw, CM* pcm)
{
	/*std::cout << numFrameObjs << "\n";*/
	//numFrameObjs = 0;

	glstate::Verify("DrawSw begin");
	glGlobShader.Use();
	PdrawglEdit()->projectedVolumeFinalPass = 0;
	glUniform2f(glslShadowHalfTexel, 0.5f / static_cast<float>(g_gl.dyshWidth), 0.5f / static_cast<float>(g_gl.dyshHeight));

	//std::cout << g_cFrameGlobs << "\n";
	AdvanceFrameStreams();
	BeginFrameArrayStream(&ropStream, CountRopStreamRpl());
	// Levels draw roughly 400-1500 records a frame; starting at 2048 avoids a
	// chain of mid-frame buffer growths on the first frames.
	BeginFrameArrayStream(&drawStream, (std::max)(s_cdrawRecordLast, 2048));
	BeginFrameStream(&blipStream, g_blipCount);
	
	SortRenderRpl();

	glBindBuffer(GL_UNIFORM_BUFFER, cmUBO);
	glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(CMGL), &pcm->matWorldToClip);

	PrepareSwLights(psw, pcm);
	PrepareSwShadows(psw, pcm);
	s_fShadowCandidatesValid = false;

	// OpenGL line widths are measured in render-target pixels. Scale the
	// authored cel-border width with the internal resolution so resolving the
	// scene back to the window does not make outlines appear thinner.
	const float celBorderRasterScale = static_cast<float>(g_gl.renderHeight) / static_cast<float>((std::max)(g_gl.presentHeight, 1));
	glLineWidth(4.125f * celBorderRasterScale);

	glEnable(GL_CULL_FACE);


	if (g_dynamicTextureCount > 0)
	{
		// Do not leave the array simultaneously attached and bound for sampling.
		glActiveTexture(GL_TEXTURE0 + SHADOW_TEXTURE_UNIT);
		glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
		glActiveTexture(GL_TEXTURE0);

		glBindFramebuffer(GL_FRAMEBUFFER, g_gl.dyshFbo);
		glViewport(0, 0, g_gl.dyshWidth, g_gl.dyshHeight);

		/*glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);*/

		glDepthFunc(GL_ALWAYS);
		glDepthMask(false);

		glDyshadow.Use();

		for (int i = 0; i < g_dynamicTextureCount; i++)
			g_dynamicTexturePrpl[i].PFNDRAWRPL(&g_dynamicTexturePrpl[i]);

		glBindFramebuffer(GL_FRAMEBUFFER, g_sceneFbo);
		glViewport(0, 0, g_gl.renderWidth, g_gl.renderHeight);

		//glDisable(GL_BLEND);

		glDepthFunc(GL_LESS);
		glDepthMask(true);

		glGlobShader.Use();

		g_dynamicTextureCount = 0;
	}

	// Dynamic maps have now been rendered. Bind the shared projected-shadow
	// array once before regular geometry is submitted.
	BindSwShadowTextures(psw);

	if (g_backGroundCount > 0)
	{
		glDepthFunc(GL_ALWAYS);
		glDepthMask(false);

		for (int i = 0; i < g_backGroundCount; i++)
			g_backGroundPrpl[i].PFNDRAWRPL(&g_backGroundPrpl[i]);

		glDepthFunc(GL_LESS);
		glDepthMask(true);

		g_backGroundCount = 0;
	}

	if (g_backGroundBlendCount > 0)
	{
		glEnable(GL_BLEND);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
		glDepthFunc(GL_ALWAYS);
		glDepthMask(false);

		for (int i = 0; i < g_backGroundBlendCount; i++)
			g_backGroundBlendPrpl[i].PFNDRAWRPL(&g_backGroundBlendPrpl[i]);

		glDisable(GL_BLEND);
		glDepthFunc(GL_LESS);
		glDepthMask(true);

		g_backGroundBlendCount = 0;
	}

	if (g_blotContextCount > 0)
	{
		// TV lighting is scoped by PreTvContext/PostTvContext entries inside
		// this ordered list. Never carry an unfinished context across frames.
		g_ptvDrawContext = nullptr;

		for (int i = 0; i < g_blotContextCount; i++)
			g_blotContextPrpl[i].PFNDRAWRPL(&g_blotContextPrpl[i]);

		g_ptvDrawContext = nullptr;
		g_blotContextCount = 0;

	}

	if (g_opaqueCount > 0)
	{
		for (int i = 0; i < g_opaqueCount; i++)
			g_opaquePrpl[i].PFNDRAWRPL(&g_opaquePrpl[i]);

		g_opaqueCount = 0;
	}

	if (g_cutOutBlendAddCount > 0)
	{
		glEnable(GL_BLEND);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
		glUniform1f(glslAlphaCutOff, 0.9);

		for (int i = 0; i < g_cutOutBlendAddCount; i++)
			g_cutOutBlendAddPrpl[i].PFNDRAWRPL(&g_cutOutBlendAddPrpl[i]);

		glDepthMask(true);
		glDisable(GL_BLEND);

		g_cutOutBlendAddCount = 0;
	}

	if (g_cutOutCount > 0)
	{
		glEnable(GL_BLEND);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
		glUniform1f(glslAlphaCutOff, 0.9);

		for (int i = 0; i < g_cutOutCount; i++)
			g_cutOutPrpl[i].PFNDRAWRPL(&g_cutOutPrpl[i]);

		glDepthMask(true);
		glDisable(GL_BLEND);

		g_cutOutCount = 0;
	}

	if (g_celBorderCount > 0)
	{
		glCelBorderShader.Use();
		glBindVertexArray(g_gl.celVao);

		glEnable(GL_BLEND);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
		glEnable(GL_STENCIL_TEST);
		glStencilMask(0x40);
		glStencilFunc(GL_ALWAYS, 0x40, 0x40);
		glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);

		glDepthFunc(GL_LEQUAL);
		glDepthMask(GL_TRUE);

		for (int i = 0; i < g_celBorderCount; i++)
			DrawCelBorder(&g_celBorderPrpl[i]);

		glStencilMask(0xFF);
		glDisable(GL_STENCIL_TEST);
		glDisable(GL_BLEND);
		glDepthFunc(GL_LESS);

		glGlobShader.Use();

		g_celBorderCount = 0;
	}

	if (g_projVolumeCount > 0)
	{
		glEnable(GL_BLEND);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
		glEnable(GL_STENCIL_TEST);
		glDepthMask(GL_FALSE);
		glDepthFunc(GL_LESS);
		glStencilMask(128);

		for (int i = 0; i < g_projVolumeCount; i++)
		{
			g_grfshd = g_projVolumePrpl[i].pglob->grfshd;
			g_projVolumePrpl[i].PFNDRAWRPL(&g_projVolumePrpl[i]);
		}

		glStencilMask(0xFF);
		glDepthMask(GL_TRUE);
		glDisable(GL_STENCIL_TEST);
		glDisable(GL_BLEND);
		glDepthFunc(GL_LESS);
		glFrontFace(GL_CCW);

		g_projVolumeCount = 0;
	}

	if (g_opaqueAfterProjVolumeCount > 0)
	{
		for (int i = 0; i < g_opaqueAfterProjVolumeCount; i++)
			g_opaqueAfterProjVolumePrpl[i].PFNDRAWRPL(&g_opaqueAfterProjVolumePrpl[i]);

		g_opaqueAfterProjVolumeCount = 0;
	}

	if (g_cutOutAfterProjVolumeAddCount > 0)
	{
		glEnable(GL_BLEND);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
		glUniform1f(glslAlphaCutOff, 0.9);

		for (int i = 0; i < g_cutOutAfterProjVolumeAddCount; i++)
			g_cutOutAfterProjVolumeAddPrpl[i].PFNDRAWRPL(&g_cutOutAfterProjVolumeAddPrpl[i]);

		glDepthMask(true);
		glDisable(GL_BLEND);

		g_cutOutAfterProjVolumeAddCount = 0;
	}

	if (g_cutOutAfterProjVolumeCount > 0)
	{
		glEnable(GL_BLEND);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
		glUniform1f(glslAlphaCutOff, 0.9);

		for (int i = 0; i < g_cutOutAfterProjVolumeCount; i++)
			g_cutOutAfterProjVolumePrpl[i].PFNDRAWRPL(&g_cutOutAfterProjVolumePrpl[i]);

		glDepthMask(true);
		glDisable(GL_BLEND);

		g_cutOutAfterProjVolumeCount = 0;
	}

	if (g_celBorderAfterProjVolumeCount > 0)
	{
		glCelBorderShader.Use();
		glBindVertexArray(g_gl.celVao);

		glEnable(GL_BLEND);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);

		glDepthFunc(GL_LEQUAL);

		for (int i = 0; i < g_celBorderAfterProjVolumeCount; i++)
			DrawCelBorder(&g_celBorderAfterProjVolumePrpl[i]);

		glDepthFunc(GL_LESS);
		glDisable(GL_BLEND);

		glGlobShader.Use();

		g_celBorderAfterProjVolumeCount = 0;
	}

	if (g_murkClearCount > 0)
	{
		glEnable(GL_BLEND);
		glDepthMask(GL_FALSE);
		glDepthFunc(GL_LEQUAL);

		for (int i = 0; i < g_murkClearCount; i++)
			g_murkClearPrpl[i].PFNDRAWRPL(&g_murkClearPrpl[i]);

		glDepthMask(GL_TRUE);
		glDisable(GL_BLEND);
		glDepthFunc(GL_LESS);

		g_murkClearCount = 0;
	}

	if (g_murkOpaqueCount > 0)
	{
		PdrawglEdit()->fAlphaTest = 1;
		glUniform1f(glslAlphaCutOff, 0.1);

		for (int i = 0; i < g_murkOpaqueCount; i++)
			g_murkOpaquePrpl[i].PFNDRAWRPL(&g_murkOpaquePrpl[i]);

		PdrawglEdit()->fAlphaTest = 0;

		g_murkOpaqueCount = 0;
	}

	if (g_murkFillCount > 0)
	{
		glEnable(GL_BLEND);
		glDepthMask(false);
		glDepthFunc(GL_LEQUAL);

		for (int i = 0; i < g_murkFillCount; i++)
			g_murkFillPrpl[i].PFNDRAWRPL(&g_murkFillPrpl[i]);

		glDisable(GL_BLEND);
		glDepthMask(true);
		glDepthFunc(GL_LESS);

		g_murkFillCount = 0;
	}

	if (g_translucentAddCount > 0)
	{
		glEnable(GL_BLEND);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
		glUniform1f(glslAlphaCutOff, 0.9);

		for (int i = 0; i < g_translucentAddCount; i++)
			g_translucentAddPrpl[i].PFNDRAWRPL(&g_translucentAddPrpl[i]);

		glDepthMask(true);
		glDisable(GL_BLEND);

		g_translucentAddCount = 0;
	}

	if (g_translucentCount > 0)
	{
		glEnable(GL_BLEND);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
		glUniform1f(glslAlphaCutOff, 0.9);

		for (int i = 0; i < g_translucentCount; i++)
			g_translucentPrpl[i].PFNDRAWRPL(&g_translucentPrpl[i]);

		glDepthMask(true);
		glDisable(GL_BLEND);

		g_translucentCount = 0;
	}

	if (g_translucentCelBorderCount > 0)
	{
		glCelBorderShader.Use();
		glBindVertexArray(g_gl.celVao);

		glEnable(GL_BLEND);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);

		glDepthFunc(GL_LEQUAL);

		for (int i = 0; i < g_translucentCelBorderCount; i++)
			DrawCelBorder(&g_translucentCelBorderPrpl[i]);

		glDepthFunc(GL_LESS);
		glDisable(GL_BLEND);

		glGlobShader.Use();

		g_translucentCelBorderCount = 0;
	}

	if (g_blipCount > 0)
	{
		glEnable(GL_BLEND);
		glEnable(GL_DEPTH_TEST);
		glDepthFunc(GL_LESS);
		glDepthMask(false);
		glDisable(GL_CULL_FACE);

		glBlipShader.Use();
		glBindBufferBase(GL_UNIFORM_BUFFER, 0, cmUBO);
		glActiveTexture(GL_TEXTURE0);
		glBindVertexArray(g_gl.blipVao);
		glBindBuffer(GL_ARRAY_BUFFER, g_gl.blipInstanceVbo);

		// DrawBlipg loads g_clock.t for curve age and g_clock.dtReal for the
		// flying-strip displacement (retail instructions 0x0013B0E8 and
		// 0x0013B140). Thus pause freezes lifetime without collapsing streaks.
		glUniform1f(glslBlipCurrentTime, g_clock.t);
		glUniform1f(glslBlipDtFrame, g_clock.dtReal);
		glUniformMatrix3fv(glslBlipCameraMat, 1, GL_FALSE, glm::value_ptr(g_pcm->mat));

		PrepareBlipGroups();

		for (int i = 0; i < g_blipCount; i++)
			g_blipPrpl[i].PFNDRAWRPL(&g_blipPrpl[i]);

		glBindVertexArray(0);
		glBindBuffer(GL_ARRAY_BUFFER, 0);
		glBindTexture(GL_TEXTURE_2D, 0);
		glDepthMask(true);
		glDisable(GL_BLEND);
		glEnable(GL_CULL_FACE);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
		glGlobShader.Use();

		g_blipCount = 0;
	}

	if (g_foreGroundCount > 0)
	{
		// Foreground objects are camera/UI composites (including confrontation
		// TV speakers), not members of the level depth hierarchy.
		glDepthFunc(GL_ALWAYS);
		glDepthMask(GL_FALSE);

		for (int i = 0; i < g_foreGroundCount; i++)
			g_foreGroundPrpl[i].PFNDRAWRPL(&g_foreGroundPrpl[i]);

		glDepthMask(GL_TRUE);
		glDepthFunc(GL_LESS);

		g_foreGroundCount = 0;
	}

	if (g_worldMapCount > 0)
	{
		glEnable(GL_BLEND);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
		glDepthMask(false);
		glDepthFunc(GL_ALWAYS);

		// Every world-map presentation uses the original 4:3 frame, including
		// SELECT and the automatic map shown while entering or leaving a level.
		// Hide the paused 3D world in the unused widescreen area.
		constexpr float mapAspect = 4.0f / 3.0f;
		const float screenWidth = static_cast<float>(g_gl.width);
		const float screenHeight = static_cast<float>(g_gl.height);
		const float mapWidth = screenHeight * mapAspect;
		float pillarboxOpacity = 1.0f;
		WM* const pwm = g_wmc.pwmCurrent;

		if (pwm != nullptr)
		{
			if (pwm->wms == WMS_Appearing && pwm->pasegOpen != nullptr && pwm->pasegOpen->tMax > 0.0f)
			{
				pillarboxOpacity = std::clamp(
					(g_clock.tReal - pwm->tWms) / pwm->pasegOpen->tMax, 0.0f, 1.0f);
			}
			else if (pwm->wms == WMS_Disappearing && pwm->pasegClose != nullptr && pwm->pasegClose->tMax > 0.0f)
			{
				pillarboxOpacity = 1.0f - std::clamp(
					(g_clock.tReal - pwm->tWms) / pwm->pasegClose->tMax, 0.0f, 1.0f);
			}
		}

		if (screenWidth > mapWidth)
		{
			const float xMapLeft = (screenWidth - mapWidth) * 0.5f;
			const float xMapRight = xMapLeft + mapWidth;
			const int pillarboxAlpha = static_cast<int>(pillarboxOpacity * 255.0f);
			FillScreenRect(0, 0, 0, pillarboxAlpha, 0.0f, 0.0f, xMapLeft, screenHeight);
			FillScreenRect(0, 0, 0, pillarboxAlpha, xMapRight, 0.0f, screenWidth, screenHeight);
			glGlobShader.Use();
		}

		for (int i = 0; i < g_worldMapCount; i++)
		{
			g_worldMapPrpl[i].PFNDRAWRPL(&g_worldMapPrpl[i]);
		}

		glDepthMask(true);
		glDepthFunc(GL_LESS);
		glDisable(GL_BLEND);

		g_worldMapCount = 0;
	}

	if (g_maxCount > 0)
	{
		for (int i = 0; i < g_maxCount; i++)
			g_maxPrpl[i].PFNDRAWRPL(&g_maxPrpl[i]);

		g_maxCount = 0;
	}

	g_cFrameGlobs = 0;
	g_cFrameCelGlobs = 0;
	s_cdrawRecordLast = s_cdrawRecord;
	s_cdrawRecord = 0;

	glDisable(GL_CULL_FACE);
	glClearStencil(0x00);
	glstate::Verify("DrawSw end");
}

void FillScreenRect(int r, int g, int b, int alpha, float xLeft, float yTop, float xRight, float yBottom, bool fAdditive)
{
	const float width = xRight - xLeft;
	const float height = yBottom - yTop;

	if (width <= 0.0f || height <= 0.0f || alpha <= 0)
		return;

	const glm::mat4 model = glm::translate(glm::mat4(1.0f), glm::vec3(xLeft, yTop, 0.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(width, height, 1.0f));
	const glm::vec4 color(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f, static_cast<float>(alpha) / 255.0f);
	const GLboolean fCullEnabled = glIsEnabled(GL_CULL_FACE);
	const GLboolean fDepthEnabled = glIsEnabled(GL_DEPTH_TEST);
	const GLboolean fBlendEnabled = glIsEnabled(GL_BLEND);
	GLint blendEquationRgb = GL_FUNC_ADD;
	GLint blendEquationAlpha = GL_FUNC_ADD;
	glGetIntegerv(GL_BLEND_EQUATION_RGB, &blendEquationRgb);
	glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &blendEquationAlpha);

	glBlotShader.Use();

	glUniformMatrix4fv(u_modelLoc, 1, GL_FALSE, glm::value_ptr(model));
	glUniformMatrix4fv(u_projectionLoc, 1, GL_FALSE, glm::value_ptr(g_gl.blotProjection));
	glUniform4f(uvRectLoc, 0.0f, 0.0f, 1.0f, 1.0f);
	glUniform4fv(blotColorLoc, 1, glm::value_ptr(color));

	// Screen-space overlays may be submitted from a 3D render pass.  Do not let
	// that pass's culling, depth, or subtractive blend state discard the quad.
	glDisable(GL_CULL_FACE);
	glDisable(GL_DEPTH_TEST);
	glEnable(GL_BLEND);
	glBlendEquation(GL_FUNC_ADD);
	glBlendFunc(GL_SRC_ALPHA, fAdditive ? GL_ONE : GL_ONE_MINUS_SRC_ALPHA);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, whiteTex);
	glUniform1i(u_fontTexLoc, 0);

	glBindVertexArray(g_gl.gao);
	glDrawElements(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr);
	glBindVertexArray(0);

	glBlendEquationSeparate(blendEquationRgb, blendEquationAlpha);
	if (!fBlendEnabled)
		glDisable(GL_BLEND);
	if (fDepthEnabled)
		glEnable(GL_DEPTH_TEST);
	if (fCullEnabled)
		glEnable(GL_CULL_FACE);
}

void DrawDysh(RPL *prpl)
{
	DYSH *pdysh = prpl->pdysh;

	if (pdysh == nullptr || pdysh->shadowTex == 0 || pdysh->shadowLayer < 0)
		return;

	glFramebufferTextureLayer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
		pdysh->shadowTex, 0, pdysh->shadowLayer);
	// The idle shadow FBO is depth-only (glDrawBuffer(GL_NONE), gl.cpp).
	glDrawBuffer(GL_COLOR_ATTACHMENT0);

	glClearColor(0.f, 0.f, 0.f, 0.f);
	glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

	glm::mat4 matWorldToClip = g_uvToClip * pdysh->pshadowGen->matWorldToUv;

	glUniformMatrix4fv(glslDyshMatWorldClip, 1, GL_FALSE, glm::value_ptr(matWorldToClip));
	glUniformMatrix4fv(glslDyshModel, 1, GL_FALSE, glm::value_ptr(prpl->ro.model));

	GLOBSET* pglobset = &pdysh->globset;
	const bool fHasBones = pglobset->cbnd > 0 && pglobset->boneMatrixSSBO != 0;

	if (fHasBones)
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, pglobset->boneMatrixSSBO);

	for (int i = 0; i < pglobset->aglob.size(); i++)
	{
		GLOB& glob = pglobset->aglob[i];
		glBindVertexArray(glob.VAO);

		for (int a = 0; a < glob.asubglob.size(); a++)
		{
			SUBGLOB& sg = glob.asubglob[a];
			glUniform1i(glslDyshfSkin, fHasBones && sg.cibnd > 0 ? 1 : 0);
			glDrawElementsBaseVertex(GL_TRIANGLES, (GLsizei)sg.indexCount, GL_UNSIGNED_INT, (void*)(uintptr_t)(sg.firstIndex * sizeof(uint32_t)), (GLint)sg.baseVertex);
		}
	}

	glUniform1i(glslDyshfSkin, 0);
}

// Units are only switched when a binding changes. Unit 0 is left active
// because later code binds textures without selecting a unit.
void BindGlobOneWayTexture(GLuint diffuseTexture)
{
	glstate::BindTextureUnit(1, GL_TEXTURE_2D, diffuseTexture != 0 ? diffuseTexture : whiteTex);
	glActiveTexture(GL_TEXTURE0);
}

void BindGlobThreeWayTextures(GLuint ambientTexture, GLuint diffuseTexture, GLuint saturateTexture)
{
	const GLuint textures[3] = { ambientTexture, diffuseTexture, saturateTexture };
	for (int unit = 0; unit < 3; ++unit)
		glstate::BindTextureUnit(unit, GL_TEXTURE_2D, textures[unit] != 0 ? textures[unit] : whiteTex);
	glActiveTexture(GL_TEXTURE0);
}

namespace
{
// DrawThndFlash leaves the blot program bound for the rest of its list, so
// later globs there draw with it. Their parameters used to be glob-program
// uniforms that such draws never reached; keep their edits from leaking into
// later glob-program draws. iro stays, as the RO range binding it replaced was
// not program state.
class DrawglForeignProgramGuard
{
public:
	DrawglForeignProgramGuard()
		: m_fActive((glstate::g_state.known & glstate::Known_Program) != 0 &&
			glstate::g_state.program != glGlobShader.ID)
	{
		if (m_fActive)
			m_drawglSaved = s_drawgl;
	}

	~DrawglForeignProgramGuard()
	{
		if (!m_fActive)
			return;

		const int32_t iro = s_drawgl.iro;
		s_drawgl = m_drawglSaved;
		s_drawgl.iro = iro;
		s_fDrawglAppended = false;
		s_fShadowCandidatesValid = false;
	}

	DrawglForeignProgramGuard(const DrawglForeignProgramGuard&) = delete;
	DrawglForeignProgramGuard& operator=(const DrawglForeignProgramGuard&) = delete;

private:
	bool m_fActive;
	DRAWGL m_drawglSaved;
};
}

void DrawGlob(RPL* prpl)
{
	DrawglForeignProgramGuard drawglGuard;
	GLOB* pglob = prpl->pglob;
	const bool fQueuedCacheWrite = prpl->ro.trlk == TRLK_Relight;
	prpl->ro.blotTvLight = 0;

	// Retail evaluates three-way TV globs through the Quick path against the
	// final posed/skinned normals. The glob directly selects its persistent TV
	// light slot, completely bypassing the world's light selection.
	// pglob->pblot persists after a dialogue and is also present on full-screen
	// confrontation speakers. Only PreTvContext establishes an active isolated
	// TV-light domain; PostTvContext clears it again.
	if (g_ptvDrawContext != nullptr && pglob != nullptr &&
		pglob->fThreeWay == 1 &&
		pglob->pblot == static_cast<BLOT*>(g_ptvDrawContext))
	{
		if (g_ptvDrawContext == &g_tvLeft)
			prpl->ro.blotTvLight = 1;
		else if (g_ptvDrawContext == &g_tvRight)
			prpl->ro.blotTvLight = 2;

		if (prpl->ro.blotTvLight != 0)
		{
			prpl->ro.trlk = TRLK_Quick;
			prpl->ro.fDynamicLight = 0;
		}
	}

	AppendGlobRo(&prpl->ro);

	GLOBSET* pglobset = prpl->palo != nullptr ? &prpl->palo->globset : nullptr;

	const bool fHasBones = pglobset != nullptr && pglobset->cbnd > 0 && pglobset->boneMatrixSSBO != 0;
	const bool fHasPoses = pglobset != nullptr && pglob->poseCount > 0 && pglob->poseDposSSBO != 0 && pglob->poseDnormalSSBO != 0 && !pglobset->agPoses.empty();

	DRAWGL* const pdrawgl = PdrawglEdit();
	pdrawgl->fPose = fHasPoses ? 1 : 0;

	if (fHasBones)
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, pglobset->boneMatrixSSBO);

	if (fHasPoses)
	{
		const int poseCount = pglob->poseCount;

		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 10, pglob->poseDposSSBO);
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 11, pglob->poseDnormalSSBO);
		glUniform1i(glslPoseCount, pglob->poseCount);
		glUniform1fv(glslPoseWeights, poseCount, pglobset->agPoses.data());
	}

	if (pglob->fThreeWay == 1)
	{
		if (prpl->ro.trlk == TRLK_Relight ||
			(prpl->ro.trlk == TRLK_Baked && prpl->ro.fDynamic == 0))
			glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, pglob->ssboCachedMaterial);
	}

	// The element buffer is part of the VAO state (bound in glob.cpp).
	glBindVertexArray(pglob->VAO);

	if (prpl->ro.warpType != WARP_NONE)
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, pglob->pwarpGlob->ssboState);

	for (int i = 0; i < static_cast<int>(pglob->asubglob.size()); ++i)
	{
		SUBGLOB& sub = pglob->asubglob[i];
		SAI* sai = sub.uvSai;
		int iframe = 0;

		if (sai != nullptr && sai->pshd != nullptr && sai->pshd->cframe > 0)
		{
			iframe = sai->iframe;

			if (iframe < 0)
				iframe = 0;
			else if (iframe >= sai->pshd->cframe)
				iframe = sai->pshd->cframe - 1;
		}

		PdrawglEdit();
		pdrawgl->fSkin = fHasBones && sub.cibnd > 0 ? 1 : 0;

		const int animate = sai != nullptr && (sai->grfsai & 0x2) != 0 ? 1 : 0;

		pdrawgl->fAnimateUv = animate;

		if (animate)
			pdrawgl->uvOffsets = glm::vec2(sai->tcx.du, sai->tcx.dv);

		SHD* pshd = sub.pshd;

		if (prpl->rp == RP_Translucent && pshd != nullptr)
		{
			// FillShaders emits the GS ALPHA state with each material packet.
			// grfshd 2/6 use ordinary source-alpha blending; the remaining
			// translucent material variants use additive destination blending.
			if (pshd->grfshd == 2 || pshd->grfshd == 6)
				glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
			else
				glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
		}

		// The visible projected-color packet uses PRIM 0x6c (TME=0), so its
		// PackRGBA bytes are ordinary 0..255 framebuffer color. The earlier
		// textured packet only participates in constructing the volume mask.
		// GS alpha uses 0x80 as full strength and is quantized again after the
		// VU applies the object's fade.
		if (pshd != nullptr && pshd->shdk == SHDK_ProjectedVolume)
		{
			auto SourceByte = [](float channel)
			{
				return std::clamp(static_cast<int>(channel * 255.0f + 0.5f), 0, 255);
			};

			// PackRGBA stores (source + 1) >> 1. RGB remains ordinary 8-bit GS
			// color and therefore normalizes against 255. Only GS alpha uses 0x80
			// as full strength. Expanding RGB against 128 saturates additive CAMSEN
			// footprints into opaque-looking primary/secondary colors.
			constexpr float colorDivisor = 255.0f;
			const float packedRed = static_cast<float>((SourceByte(pshd->rgbaVolume.r) + 1) >> 1);
			const float packedGreen = static_cast<float>((SourceByte(pshd->rgbaVolume.g) + 1) >> 1);
			const float packedBlue = static_cast<float>((SourceByte(pshd->rgbaVolume.b) + 1) >> 1);

			// DrawVolume scales the packed alpha byte in-place and truncates it.
			// Compensate for DrawOneWay's later op.uAlpha multiply so the final
			// source alpha equals that exact GS byte divided by 0x80.
			const int packedAlpha = (SourceByte(pshd->rgbaVolume.a) + 1) >> 1;
			const float objectAlpha = std::clamp(prpl->ro.uAlpha, 0.0f, 1.0f);
			const int fadedAlpha = static_cast<int>(packedAlpha * objectAlpha);
			const float projectedAlpha = objectAlpha > 0.0f
				? (static_cast<float>(fadedAlpha) / 128.0f) / objectAlpha
				: 0.0f;

			pdrawgl->projectedVolumeColor = glm::vec4(
				packedRed / colorDivisor,
				packedGreen / colorDivisor,
				packedBlue / colorDivisor,
				projectedAlpha);
		}
		else
		{
			pdrawgl->projectedVolumeColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
		}

		if (pshd != nullptr && !pshd->atex.empty() && !pshd->atex[0].abmp.empty())
		{
			if (iframe >= static_cast<int>(pshd->atex[0].abmp.size()))
				iframe = static_cast<int>(pshd->atex[0].abmp.size()) - 1;

			BMP* pbmp = pshd->atex[0].abmp[iframe];

			if (pbmp != nullptr)
			{
				if (pshd->shdk != SHDK_ThreeWay)
				{
					pdrawgl->rko = 0;
					const TEX& tex = pshd->atex[0];
					GLuint diffuseTexture =
						iframe < static_cast<int>(tex.glDiffuseMap.size())
						? tex.glDiffuseMap[iframe]
						: pbmp->glDiffuseMap;

					// The retail projected-volume packet binds a texture only when
					// grfshd bit 1 is set. Untextured volumes use their packed
					// volume RGBA directly.
					if (pshd->shdk == SHDK_ProjectedVolume && (pshd->grfshd & 2) == 0)
						diffuseTexture = whiteTex;

					BindGlobOneWayTexture(diffuseTexture);
				}
				else
				{
					pdrawgl->rko = 1;
					const TEX& tex = pshd->atex[0];
					const GLuint ambientTexture =
						iframe < static_cast<int>(tex.glShadowMap.size()) ? tex.glShadowMap[iframe] : 0;
					const GLuint diffuseTexture =
						iframe < static_cast<int>(tex.glDiffuseMap.size()) ? tex.glDiffuseMap[iframe] : 0;
					const GLuint saturateTexture =
						iframe < static_cast<int>(tex.glSaturateMap.size()) ? tex.glSaturateMap[iframe] : 0;
					BindGlobThreeWayTextures(ambientTexture, diffuseTexture, saturateTexture);
				}
			}
		}

		if (activeShadows.numShadows > 0)
		{
			const glm::vec4 subGlobPosCenter = prpl->ro.model * glm::vec4(sub.posCenter, 1.0f);
			const float subGlobRadius = pglob->rSubglobRadius * sub.sRadius;

			pdrawgl->subGlobPosCenter = glm::vec3(subGlobPosCenter);
			pdrawgl->subGlobRadius = subGlobRadius;
		}

		pdrawgl->unSelfIllum = sub.unSelfIllum;
		pglob->PFNDRAW(sub.baseVertex, sub.firstIndex, sub.indexCount);
	}

	PdrawglEdit();
	pdrawgl->fSkin = 0;
	pdrawgl->fPose = 0;

	if (prpl->ro.trlk == TRLK_Relight)
	{
		// Make vertex-shader SSBO writes visible before any later packet reads
		// this glob's cached three-way material.
		glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

		if (pglob->trlk == TRLK_BakePending)
			pglob->trlk = TRLK_Baked;

	}
	else if (fQueuedCacheWrite && pglob->trlk == TRLK_BakePending)
	{
		// A TV draw can redirect a queued relight packet through TRLK_Quick.
		// No cache was written, so make the normal world draw try again.
		pglob->trlk = TRLK_Relight;
	}
}

void DrawCelBorder(RPLCEL* prplcel)
{
	GLOB* pglob = prplcel->pglob;
	GLOBSET* pglobset = prplcel->palo != nullptr ? &prplcel->palo->globset : nullptr;

	const bool fSkin = pglobset != nullptr && pglobset->cbnd > 0 && pglobset->boneMatrixSSBO != 0;
	const bool fPose = pglobset != nullptr && pglob->poseCount > 0 && pglob->edgePoseDposSSBO != 0 && !pglobset->agPoses.empty();

	CELDRAWGL celdraw{};
	celdraw.model = prplcel->rocel.model;
	celdraw.celRgba = prplcel->rocel.celRgba;
	celdraw.uAlphaCelBorder = prplcel->rocel.uAlphaCelBorder;
	celdraw.fSkin = fSkin ? 1 : 0;
	celdraw.fPose = fPose ? 1 : 0;
	const GLuint idraw = static_cast<GLuint>(AppendArrayStream(&drawStream, &celdraw, sizeof(CELDRAWGL)));
	s_cdrawRecord++;

	glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 8, pglob->edgeSSBO);

	if (fSkin)
	{
		// Bone indices / weights are part of each CELPOINT (binding 8).
		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 9, pglobset->boneMatrixSSBO);
	}

	if (fPose)
	{
		const int cposeUpload = pglobset->agPoses.size();

		glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 12, pglob->edgePoseDposSSBO);
		glUniform1i(glslCelPoseCount, pglob->poseCount);
		glUniform1fv(glslCelPoseWeights, cposeUpload, pglobset->agPoses.data());
	}

	glDrawArraysInstancedBaseInstance(GL_LINES, 0, pglob->edgeCount * 2, 1, idraw);
}

void DrawSubGlob(int baseVertex, int firstIndex, int indexCount)
{
	DrawGlobElements(baseVertex, firstIndex, indexCount);
}

void DrawProjVolume(int baseVertex, int firstIndex, int indexCount)
{
	const auto DrawVolumeMesh = [baseVertex, firstIndex, indexCount]()
	{
		DrawGlobElements(baseVertex, firstIndex, indexCount);
	};

	if (g_grfshd == 0)
	{
		glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_ONE, GL_ZERO);
		glDepthFunc(GL_LESS);
		glStencilFunc(GL_ALWAYS, 128, 128);
		glStencilOp(GL_ZERO, GL_REPLACE, GL_ZERO);
		glColorMask(0, 0, 0, 0);
		glFrontFace(GL_CW);
		DrawVolumeMesh();

		glColorMask(1, 1, 1, 1);
		glStencilOp(GL_KEEP, GL_ZERO, GL_KEEP);
		glFrontFace(GL_CCW);
		DrawVolumeMesh();

		glDepthFunc(GL_ALWAYS);
		// Bit 6 protects cel-border pixels; bit 7 contains the volume mask.
		// Testing both bits prevents projected volumes from drawing over borders.
		glStencilFunc(GL_EQUAL, 128, 0xC0);
		glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
		glFrontFace(GL_CW);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
		PdrawglEdit()->projectedVolumeFinalPass = 1;
		DrawVolumeMesh();
		PdrawglEdit()->projectedVolumeFinalPass = 0;
		return;
	}

	if (g_grfshd == 2)
	{
		glDepthFunc(GL_LESS);
		glStencilFunc(GL_ALWAYS, 128, 128);
		glStencilOp(GL_NONE, GL_REPLACE, GL_NONE);
		glColorMask(0, 0, 0, 0);
		glFrontFace(GL_CW);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
		DrawVolumeMesh();

		glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_ONE, GL_ZERO);
		glColorMask(1, 1, 1, 1);
		glStencilOp(GL_KEEP, GL_NONE, GL_KEEP);
		glFrontFace(GL_CCW);
		DrawVolumeMesh();

		glDepthFunc(GL_ALWAYS);
		glStencilFunc(GL_EQUAL, 128, 0xC0);
		glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
		glFrontFace(GL_CW);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
		PdrawglEdit()->projectedVolumeFinalPass = 1;
		DrawVolumeMesh();
		PdrawglEdit()->projectedVolumeFinalPass = 0;
		return;
	}

	// grfshd 1 and 3 use the additive projected-volume packet path.
	PdrawglEdit()->projectedVolumeFinalPass = 2;
	glBlendFuncSeparate(g_grfshd == 3 ? GL_SRC_ALPHA : GL_ZERO,
		GL_ONE, GL_ONE, GL_ZERO);
	glDepthFunc(GL_LESS);
	glStencilFunc(GL_ALWAYS, 128, 128);
	glStencilOp(GL_ZERO, GL_REPLACE, GL_ZERO);
	glColorMask(0, 0, 0, 0);
	glFrontFace(GL_CW);
	DrawVolumeMesh();

	if ((g_grfshd & 2) != 0)
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
	else
		glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_ONE, GL_ZERO);
	glColorMask(1, 1, 1, 1);
	glStencilOp(GL_KEEP, GL_ZERO, GL_KEEP);
	glFrontFace(GL_CCW);
	DrawVolumeMesh();

	glDepthFunc(GL_ALWAYS);
	glStencilFunc(GL_EQUAL, 128, 0xC0);
	glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);
	glFrontFace(GL_CW);
	if ((g_grfshd & 2) != 0)
		BindGlobOneWayTexture(whiteTex);
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
	PdrawglEdit()->projectedVolumeFinalPass = 1;
	DrawVolumeMesh();
	PdrawglEdit()->projectedVolumeFinalPass = 0;
}

void DrawMurkClear(int baseVertex, int firstIndex, int indexCount)
{
	glBlendFuncSeparate(GL_ZERO, GL_ONE, GL_ONE, GL_ZERO);
	glFrontFace(GL_CW);
	DrawGlobElements(baseVertex, firstIndex, indexCount);

	glFrontFace(GL_CCW);
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
	DrawGlobElements(baseVertex, firstIndex, indexCount);
}

void DrawMurkFill(int baseVertex, int firstIndex, int indexCount)
{
	glBlendFuncSeparate(GL_ONE_MINUS_DST_ALPHA, GL_DST_ALPHA, GL_ONE, GL_NONE);
	DrawGlobElements(baseVertex, firstIndex, indexCount);

	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);
	DrawGlobElements(baseVertex, firstIndex, indexCount);
}

void DrawTranslucent(int baseVertex, int firstIndex, int indexCount)
{
	GLboolean depthWriteWasEnabled = GL_TRUE;
	glGetBooleanv(GL_DEPTH_WRITEMASK, &depthWriteWasEnabled);

	// Retail's translucent packet performs an alpha-tested depth-writing
	// pass followed by a blended pass which does not write depth.  The packet
	// must not leak that second pass's Z-write mask into the next glob.  Sly's
	// TV model mixes translucent and ordinary globs, so leaking GL_FALSE here
	// caused every glob after his first translucent piece to stop populating
	// depth and visually overlap into his face.
	PdrawglEdit()->fAlphaTest = 1;
	glDepthMask(GL_TRUE);
	DrawGlobElements(baseVertex, firstIndex, indexCount);

	PdrawglEdit()->fAlphaTest = 0;
	glDepthMask(GL_FALSE);
	DrawGlobElements(baseVertex, firstIndex, indexCount);

	glDepthMask(depthWriteWasEnabled);
}

namespace
{
struct alignas(16) BLIPGPU
{
	glm::vec4 posCreated;
	glm::vec4 velocity;
	glm::vec4 color;
	glm::vec4 roll;
};

struct BLIPBATCH
{
	GLuint texture;
	GLsizei cinstance;
	GLuint baseInstance;
};

struct BLIPGROUPDRAW
{
	RPL* prpl;
	bool fBlend; // DrawBlip sets the group's blend function
	bool fAdditive;
	int ibatchFirst;
	int cbatch; // zero when no particle survived; nothing is drawn
};

// Every queued BLIP group is built up front so the frame's instances go to the
// GPU in one upload. Retained between frames so nothing is allocated per frame.
std::vector<BLIPGPU> s_ablipgpu;
std::vector<BLIPBATCH> s_ablipbatch;
std::vector<BLIPGROUPDRAW> s_ablipgroupdraw;
size_t s_iblipgroupdraw = 0;

void BuildBlipGroup(RPL* prplblip, BLIPGROUPDRAW* pgroup)
{
	pgroup->prpl = prplblip;
	pgroup->fBlend = false;
	pgroup->fAdditive = false;
	pgroup->ibatchFirst = static_cast<int>(s_ablipbatch.size());
	pgroup->cbatch = 0;

	if (prplblip == nullptr || prplblip->pblipg == nullptr || g_pcm == nullptr)
		return;

	BLIPG* pblipg = prplblip->pblipg;

	if (pblipg->pshd == nullptr || pblipg->pshd->atex.empty() || pblipg->pshd->atex[0].abmp.empty() || pblipg->cblipe <= 0)
		return;

	const bool additiveBlip = (pblipg->pshd->grfshd & 1u) != 0;
	pgroup->fBlend = true;
	pgroup->fAdditive = additiveBlip;

	// A BLIP texture table is capped at 32 entries by PropagateBlipgShader.
	// Additive particles are regrouped per texture; these keep their capacity.
	static std::vector<BLIPGPU> s_aadditiveInstances[32];

	const TEX& tex = pblipg->pshd->atex[0];
	const int textureTableCount = (std::min)(pblipg->cqwTexture, 32);
	if (textureTableCount <= 0)
		return;

	GLuint batchTextures[32]{};
	GLuint tableTextures[32]{};
	int tableBatches[32]{};
	int batchCount = 0;
	for (int tableIndex = 0; tableIndex < textureTableCount; ++tableIndex)
	{
		tableBatches[tableIndex] = -1;
		const int iframe = pblipg->aiframeTexture[tableIndex];
		if (iframe < 0 || iframe >= static_cast<int>(tex.abmp.size()) || tex.abmp[iframe] == nullptr)
			continue;

		BMP* const pframeBmp = tex.abmp[iframe];
		// The original BLIP texture packet is built with CTK_Ambient. For a
		// three-way shader that is the ambient/shadow texture, not diffuse.
		const bool threeWay = pblipg->pshd->shdk == SHDK_ThreeWay;
		// Non-three-way textures are palette-resolved per TEX/iframe during
		// LoadShadersFromBrx. The underlying BMP can be shared by TEX records
		// with different CLUTs, so its GL texture fields are intentionally zero.
		const GLuint textureId = threeWay
			? (iframe < static_cast<int>(tex.glShadowMap.size())
				? tex.glShadowMap[iframe] : 0)
			: (iframe < static_cast<int>(tex.glDiffuseMap.size())
				? tex.glDiffuseMap[iframe] : 0);
		if (textureId == 0)
			continue;

		tableTextures[tableIndex] = textureId;
		int ibatch = 0;
		while (ibatch < batchCount && batchTextures[ibatch] != textureId)
			++ibatch;
		if (ibatch == batchCount)
			batchTextures[batchCount++] = textureId;
		tableBatches[tableIndex] = ibatch;
	}

	if (additiveBlip)
	{
		for (int ibatch = 0; ibatch < batchCount; ++ibatch)
			s_aadditiveInstances[ibatch].clear();
	}

	auto evaluateClq = [](const CLQ& clq, float t)
	{
		return clq.g0 + t * (clq.g1 + t * clq.g2);
	};

	for (BLIP* pblip = pblipg->dlBlip.pblipFirst; pblip != nullptr; pblip = pblip->dle.pblipNext)
	{
		const int iblipf = pblip->iblipfLatest & 1;
		const BLIPF& blipf = pblip->ablipf[iblipf];
		const int particleCount = pblip->cblipe;

		for (int i = 0; i < particleCount; ++i)
		{
			const BLIPP& pos = blipf.ablipp[i];
			const BLIPV& velocity = blipf.ablipv[i];
			const BLIPX& extra = blipf.ablipx[i];
			// VU1 evaluates every BLIP curve from the raw creation-time delta.
			// Emitters may place a creation time slightly ahead of the current
			// tick, so clamping here changes the first texture/color frame.
			const float age = g_clock.t - pos.tCreated;

			const int frameOffset = static_cast<int>(evaluateClq(pblipg->clqTexture, age));
			int tableIndex = static_cast<int>(extra.itex0) + frameOffset;
			if ((textureTableCount & (textureTableCount - 1)) == 0)
				tableIndex &= textureTableCount - 1;
			else
				tableIndex = ((tableIndex % textureTableCount) + textureTableCount) % textureTableCount;

			const int ibatch = tableBatches[tableIndex];
			if (ibatch < 0 || tableTextures[tableIndex] == 0)
				continue;

			glm::vec4 color(1.0f);
			if (pblipg->crgba > 0)
			{
				const int colorIndex = (std::min)(static_cast<int>(extra.irgba), pblipg->crgba - 1);
				color = pblipg->argba[colorIndex];
				if (pblipg->fColorRanges != 0 && colorIndex + 1 < pblipg->crgba)
				{
					const float u = glm::clamp(evaluateClq(pblipg->clqColor, age), 0.0f, 1.0f);
					color = glm::mix(color, pblipg->argba[colorIndex + 1], u);
				}
			}

			BLIPGPU instance{};
			instance.posCreated = glm::vec4(pos.x, pos.y, pos.z, pos.tCreated);
			instance.velocity = glm::vec4(velocity.x, velocity.y, velocity.z, 0.0f);
			instance.color = color;
			instance.roll = glm::vec4(extra.radRoll, extra.swRoll, 0.0f, 0.0f);
			if (additiveBlip)
			{
				s_aadditiveInstances[ibatch].push_back(instance);
				continue;
			}

			// Alpha blending is order-dependent. Preserve the BLIP list order and
			// merge only adjacent particles which already reference the same frame.
			const GLuint textureId = tableTextures[tableIndex];
			if (pgroup->cbatch == 0 || s_ablipbatch.back().texture != textureId)
			{
				s_ablipbatch.push_back({ textureId, 0, static_cast<GLuint>(s_ablipgpu.size()) });
				pgroup->cbatch++;
			}
			s_ablipbatch.back().cinstance++;
			s_ablipgpu.push_back(instance);
		}
	}

	if (additiveBlip)
	{
		for (int ibatch = 0; ibatch < batchCount; ++ibatch)
		{
			const std::vector<BLIPGPU>& instances = s_aadditiveInstances[ibatch];
			if (instances.empty())
				continue;

			s_ablipbatch.push_back({ batchTextures[ibatch], static_cast<GLsizei>(instances.size()),
				static_cast<GLuint>(s_ablipgpu.size()) });
			pgroup->cbatch++;
			s_ablipgpu.insert(s_ablipgpu.end(), instances.begin(), instances.end());
		}
	}
}

// Expects blipInstanceVbo bound to GL_ARRAY_BUFFER.
void PrepareBlipGroups()
{
	s_ablipgpu.clear();
	s_ablipbatch.clear();
	s_ablipgroupdraw.resize(g_blipCount);
	s_iblipgroupdraw = 0;

	for (int i = 0; i < g_blipCount; i++)
		BuildBlipGroup(&g_blipPrpl[i], &s_ablipgroupdraw[i]);

	if (s_ablipgpu.empty())
		return;

	const GLsizeiptr uploadSize = static_cast<GLsizeiptr>(s_ablipgpu.size() * sizeof(BLIPGPU));
	g_gl.blipInstanceCapacity = (std::max)(g_gl.blipInstanceCapacity, uploadSize);

	// Orphan the previous instance store once per frame, then upload every
	// group. Draws still consuming the old store keep it, and no mapped-buffer
	// synchronization is involved.
	glBufferData(GL_ARRAY_BUFFER, g_gl.blipInstanceCapacity, nullptr, GL_STREAM_DRAW);
	glBufferSubData(GL_ARRAY_BUFFER, 0, uploadSize, s_ablipgpu.data());
}
}

void DrawBlip(RPL* prplblip)
{
	// Groups are drawn in the order PrepareBlipGroups built them.
	if (s_iblipgroupdraw >= s_ablipgroupdraw.size())
		return;

	const BLIPGROUPDRAW& group = s_ablipgroupdraw[s_iblipgroupdraw++];
	if (group.prpl != prplblip || !group.fBlend)
		return;

	BLIPG* pblipg = prplblip->pblipg;

	// DrawBlipg selected between the PS2's normal and clamped-add GIF
	// packets from shader flag bit 0.  Applying additive blending to every
	// BLIP makes effects such as jump/landing dust glow and disappear into
	// the scene, even though additive rain still happens to look plausible.
	const bool additiveBlip = group.fAdditive;
	if (additiveBlip)
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
	else
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO);

	if (group.cbatch == 0)
		return;

	BLIPGROUPGPU groupGpu{};
	groupGpu.model = prplblip->ro.model;
	groupGpu.params = glm::vec4(prplblip->ro.uAlpha, pblipg->rSFlying, static_cast<float>(pblipg->blipok), 0.0f);
	groupGpu.clqScale = glm::vec4(pblipg->clqScale.g0, pblipg->clqScale.g1, pblipg->clqScale.g2, 0.0f);
	groupGpu.clqAlpha = glm::vec4(pblipg->clqAlpha.g0, pblipg->clqAlpha.g1, pblipg->clqAlpha.g2, 0.0f);
	AppendStream(&blipStream, &groupGpu, sizeof(groupGpu), sizeof(groupGpu));

	auto drawBatches = [&]()
	{
		for (int ibatch = group.ibatchFirst; ibatch < group.ibatchFirst + group.cbatch; ++ibatch)
		{
			const BLIPBATCH& batch = s_ablipbatch[ibatch];
			glBindTexture(GL_TEXTURE_2D, batch.texture);
			glDrawElementsInstancedBaseInstance(GL_TRIANGLES, 6, GL_UNSIGNED_SHORT, nullptr, batch.cinstance, batch.baseInstance);
		}
	};

	if (!additiveBlip)
	{
		glUniform1i(glslBlipAlphaPass, 1);
		glDepthMask(GL_TRUE);
		drawBatches();

		glUniform1i(glslBlipAlphaPass, 2);
		glDepthMask(GL_FALSE);
		drawBatches();
	}
	else
	{
		glUniform1i(glslBlipAlphaPass, 0);
		glDepthMask(GL_FALSE);
		drawBatches();
	}

}

void DrawSwCollisionAll(CM* pcm)
{
	glGeomShader.Use();

	glBindBufferBase(GL_UNIFORM_BUFFER, 0, cmUBO);
	glBindBuffer(GL_UNIFORM_BUFFER, cmUBO);
	glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(CMGL), &pcm->matWorldToClip);

	glBindBufferBase(GL_UNIFORM_BUFFER, 1, geomUBO);
	glBindBuffer(GL_UNIFORM_BUFFER, geomUBO);

	glLineWidth(2.0);

	glm::mat4 model{};

	for (int i = 0; i < allSWSoObjs.size(); i++)
	{
		SO* pso = allSWSoObjs[i];

		if (!SphereInFrustum(pcm->frustum, pso->xf.posWorld, pso->geomLocal.sRadius))
			continue;

		LoadMatrixFromPosRot(&pso->xf.posWorld, &pso->xf.matWorld, &model);

		glm::mat4 modelToClip = pcm->matWorldToClip * model;
		glUniformMatrix4fv(glslGeomModelToClip, 1, GL_FALSE, glm::value_ptr(modelToClip));

		if (pso->geomLocal.VAO != 0) 
		{
			glBindVertexArray(pso->geomLocal.VAO);
			glDrawElements(GL_LINES, (GLsizei)pso->geomLocal.indices.size(), GL_UNSIGNED_SHORT, 0);
		}

		if (pso->geomCameraLocal.VAO != 0) 
		{
			glBindVertexArray(pso->geomCameraLocal.VAO);
			glDrawElements(GL_LINES, (GLsizei)pso->geomCameraLocal.indices.size(), GL_UNSIGNED_SHORT, 0);
		}
	}

	glBindVertexArray(0);
}

int numRo = 0;
int numRoCel = 0;

int g_cFrameGlobs = 0;
int g_cFrameCelGlobs = 0;

int numFrameObjs = 0;

int g_cframe = 0;
int g_cframeStaticLightsInvalid = 0;

int g_boundVAO = 0;

int g_shdIDBound = 0;
int g_lastAnimateUv = -1;

int g_grfshd = 0;

int g_dynamicTextureCount = 0;
std::vector <RPL> g_dynamicTexturePrpl;

int g_backGroundCount = 0;
std::vector <RPL> g_backGroundPrpl;

int g_backGroundBlendCount = 0;
std::vector <RPL> g_backGroundBlendPrpl;

int g_blotContextCount = 0;
std::vector <RPL> g_blotContextPrpl;

int g_opaqueCount = 0;
std::vector <RPL> g_opaquePrpl;

int g_cutOutCount = 0;
std::vector <RPL> g_cutOutPrpl;

int g_cutOutBlendAddCount = 0;
std::vector <RPL> g_cutOutBlendAddPrpl;

int g_celBorderCount = 0;
std::vector <RPLCEL> g_celBorderPrpl;


int g_projVolumeCount = 0;
std::vector <RPL> g_projVolumePrpl;

int g_opaqueAfterProjVolumeCount = 0;
std::vector <RPL> g_opaqueAfterProjVolumePrpl;

int g_cutOutAfterProjVolumeCount = 0;
std::vector <RPL> g_cutOutAfterProjVolumePrpl;

int g_cutOutAfterProjVolumeAddCount = 0;
std::vector <RPL> g_cutOutAfterProjVolumeAddPrpl;

int g_celBorderAfterProjVolumeCount = 0;
std::vector <RPLCEL> g_celBorderAfterProjVolumePrpl;

int g_murkClearCount = 0;
std::vector <RPL> g_murkClearPrpl;

int g_murkOpaqueCount = 0;
std::vector <RPL> g_murkOpaquePrpl;

int g_murkFillCount = 0;
std::vector <RPL> g_murkFillPrpl;

int g_translucentCount = 0;
std::vector <RPL> g_translucentPrpl;

int g_translucentAddCount = 0;
std::vector <RPL> g_translucentAddPrpl;

int g_translucentCelBorderCount = 0;
std::vector <RPLCEL> g_translucentCelBorderPrpl;

int g_blipCount = 0;
std::vector <RPL> g_blipPrpl;

int g_foreGroundCount = 0;
std::vector <RPL> g_foreGroundPrpl;

int g_worldMapCount = 0;
std::vector <RPL> g_worldMapPrpl;

int g_maxCount = 0;
std::vector <RPL> g_maxPrpl;

bool g_fVsync = false;

int g_fDisableSkin = 0;
int g_fDisablePoses = 0;

// Render-queue appends used by game code (upstream ProjectCane's bounds-
// checked queue API). A full list grows instead of overflowing.
namespace
{
RPL& AppendRplQueue(std::vector<RPL>& queue, int& count, const RPL& rpl)
{
	if (count < 0)
		count = 0;
	if (static_cast<size_t>(count) >= queue.size())
		queue.resize(queue.size() * 2 + 64);
	RPL& entry = queue[static_cast<size_t>(count++)];
	entry = rpl;
	return entry;
}
}

RPL& QueueTranslucentRpl(const RPL& rpl)
{
	return AppendRplQueue(g_translucentPrpl, g_translucentCount, rpl);
}

RPL& QueueTranslucentAddRpl(const RPL& rpl)
{
	return AppendRplQueue(g_translucentAddPrpl, g_translucentAddCount, rpl);
}

RPL& QueueBlotContextRpl(const RPL& rpl)
{
	return AppendRplQueue(g_blotContextPrpl, g_blotContextCount, rpl);
}

// Debug overlay (upstream ProjectCane): dialogue trigger volumes. Yellow is an
// available dialogue volume, green means Sly is currently inside it, and red
// means the volume is disabled or already pushed.
void DrawSwDialogTriggers(CM* pcm)
{
	glGeomShader.Use();

	glBindBufferBase(GL_UNIFORM_BUFFER, 0, cmUBO);
	glBindBuffer(GL_UNIFORM_BUFFER, cmUBO);
	glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(CMGL), &pcm->matWorldToClip);

	glBindBufferBase(GL_UNIFORM_BUFFER, 1, geomUBO);
	glBindBuffer(GL_UNIFORM_BUFFER, geomUBO);
	glLineWidth(4.0f);

	glm::mat4 model{};
	for (SO* pso : allSWSoObjs)
	{
		if (pso == nullptr || !FIsBasicDerivedFrom(pso, CID_VOLBTN))
			continue;

		VOLBTN* pvolbtn = static_cast<VOLBTN*>(pso);
		if (pvolbtn->btn.oidDialog == OID_Nil)
			continue;

		if (!SphereInFrustum(pcm->frustum, pso->xf.posWorld, pso->geomLocal.sRadius))
			continue;

		if (!pvolbtn->btn.fEnabled || pvolbtn->btn.buttons == BUTTONS_Disabled ||
			pvolbtn->btn.buttons == BUTTONS_Pushed)
			glUniform4f(glslGeomDebugColor, 1.0f, 0.20f, 0.16f, 1.0f);
		else if (pvolbtn->btn.buttons == BUTTONS_Contact || pvolbtn->cploPushWithin > 0)
			glUniform4f(glslGeomDebugColor, 0.20f, 1.0f, 0.30f, 1.0f);
		else
			glUniform4f(glslGeomDebugColor, 1.0f, 0.78f, 0.08f, 1.0f);

		LoadMatrixFromPosRot(&pso->xf.posWorld, &pso->xf.matWorld, &model);
		const glm::mat4 modelToClip = pcm->matWorldToClip * model;
		glUniformMatrix4fv(glslGeomModelToClip, 1, GL_FALSE, glm::value_ptr(modelToClip));

		if (pso->geomLocal.VAO != 0)
		{
			glBindVertexArray(pso->geomLocal.VAO);
			glDrawElements(GL_LINES, static_cast<GLsizei>(pso->geomLocal.indices.size()),
				GL_UNSIGNED_SHORT, nullptr);
		}

		if (pso->geomCameraLocal.VAO != 0)
		{
			glBindVertexArray(pso->geomCameraLocal.VAO);
			glDrawElements(GL_LINES, static_cast<GLsizei>(pso->geomCameraLocal.indices.size()),
				GL_UNSIGNED_SHORT, nullptr);
		}
	}

	glLineWidth(1.0f);
	glBindVertexArray(0);
}
