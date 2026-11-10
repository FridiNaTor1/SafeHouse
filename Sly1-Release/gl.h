#ifndef CANE_GL_H
#define CANE_GL_H
#pragma once
#include "glshaders.h"
#include "rocel.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/rotate_vector.hpp>
#include <glm/gtx/vector_angle.hpp>
#include <glm/gtx/string_cast.hpp>
#include "imgui/imgui.h"
#include "imgui/imgui_impl_glfw.h"
#include "imgui/imgui_impl_opengl3.h"
#include "imgui/ImGuiFileDialog.h"
#include "gl_context.h"
#include "mouse.h"
#include <vector>

enum AspectMode
{
	FitToScreen,
	Fixed_4_3,
	Fixed_16_9,
	Fixed_16_10,
	PS2_4_3,
	PS2_16_9,
};

enum WindowMode
{
	WindowMode_Windowed,
	WindowMode_Borderless,
	WindowMode_Fullscreen
};

enum GuiStyle
{
	GuiStyle_PS2,
	GuiStyle_Modern
};

struct CMGL
{
	glm::mat4 matWorldToClip;
	glm::vec4 cameraPos;
};

struct alignas(16) ROGL
{
	glm::mat4 model;         //   0 -  63
	float     uAlpha;        //  64 -  67
	float     uFog;          //  68 -  71
	float     darken;        //  72 -  75
	int32_t   grfglob;       //  76 -  79
	// Zero for ordinary world globs. TV portrait globs store their dedicated
	// TV-light SSBO index plus one (1 = left, 2 = right).
	int32_t   blotTvLight;   //  80 -  83
	int32_t   warpType;      //  84 -  87
	int32_t   warpCmat;      //  88 -  91
	int32_t   warpCvtx;      //  92 -  95
	glm::mat4 amatDpos[4];   //  96 - 351
	glm::mat4 amatDuv[4];    // 352 - 607
	int32_t   fDynamic;      // 608 - 611
	float     sRadius;       // 612 - 615
	int32_t   fDynamicLight; // 616 - 619
	int32_t   trlk;          // 620 - 623
	glm::vec4 posCenter;     // 624 - 639
};

// Glob programs read their per-object and per-draw data from arrays in two
// SSBO ranges that stay bound for the whole frame. Each draw passes its record
// index as the base instance; the instanced draw-index attribute (location 6,
// divisor 1) then yields that index, so consecutive draws change no uniform or
// buffer binding. Bindings 0 and 1 were free; using them keeps the highest SSBO
// binding at 13, so no more binding points are required than before.
constexpr GLuint ROGL_SSBO_BINDING = 0;
constexpr GLuint DRAW_SSBO_BINDING = 1;
constexpr GLuint DRAW_INDEX_ATTRIBUTE = 6;
constexpr int    DRAW_INDEX_COUNT = 65536;

// Values the glob program used to receive as per-draw uniforms (glob.vert
// DRAW, std430). ashadow lists up to 32 candidate projected shadows, one
// shadow index per byte, in active-shadow order.
struct alignas(16) DRAWGL
{
	int32_t   iro;                      //   0 -   3 index of the ROGL
	int32_t   fSkin;                    //   4 -   7
	int32_t   fPose;                    //   8 -  11
	int32_t   fAnimateUv;               //  12 -  15
	glm::vec2 uvOffsets;                //  16 -  23
	float     unSelfIllum;              //  24 -  27
	int32_t   rko;                      //  28 -  31
	glm::vec4 projectedVolumeColor;     //  32 -  47
	int32_t   fAlphaTest;               //  48 -  51
	int32_t   projectedVolumeFinalPass; //  52 -  55
	// Candidate count, or SHADOW_CANDIDATES_ALL when the list overflowed and
	// the vertex shader must test every active shadow itself.
	int32_t   cshadow;                  //  56 -  59
	// Candidates the vertex shader must still test per vertex (dynamic
	// shadows, and sphere tests too close to call on the CPU).
	uint32_t  grfshadowTest;            //  60 -  63
	glm::vec3 subGlobPosCenter;         //  64 -  75
	float     subGlobRadius;            //  76 -  79
	uint32_t  ashadow[8];               //  80 - 111
};

constexpr int SHADOW_CANDIDATES_MAX = 32;
constexpr int SHADOW_CANDIDATES_ALL = -1;

// Cel-border draw record (celborder.vert CELDRAW). It shares the draw-record
// stream, so its array stride must equal DRAWGL's.
struct alignas(16) CELDRAWGL
{
	glm::mat4 model;           //   0 -  63
	glm::vec4 celRgba;         //  64 -  79
	float     uAlphaCelBorder; //  80 -  83
	int32_t   fSkin;           //  84 -  87
	int32_t   fPose;           //  88 -  91
	int32_t   pad[5];          //  92 - 111
};

static_assert(sizeof(ROGL) == 640, "ROGL must match the GLSL RO struct");
static_assert(offsetof(DRAWGL, uvOffsets) == 16 && offsetof(DRAWGL, unSelfIllum) == 24 &&
	offsetof(DRAWGL, projectedVolumeColor) == 32 && offsetof(DRAWGL, fAlphaTest) == 48 &&
	offsetof(DRAWGL, cshadow) == 56 && offsetof(DRAWGL, subGlobPosCenter) == 64 &&
	offsetof(DRAWGL, subGlobRadius) == 76 && offsetof(DRAWGL, ashadow) == 80 &&
	sizeof(DRAWGL) == 112, "DRAWGL must match the GLSL DRAW struct");
static_assert(offsetof(CELDRAWGL, celRgba) == 64 && offsetof(CELDRAWGL, uAlphaCelBorder) == 80 &&
	offsetof(CELDRAWGL, fSkin) == 84 && offsetof(CELDRAWGL, fPose) == 88 &&
	sizeof(CELDRAWGL) == sizeof(DRAWGL), "CELDRAWGL must match the GLSL CELDRAW struct");

// Per-group BLIP parameters. One aligned range is streamed for every queued
// BLIP group, replacing several individual glUniform updates.
struct alignas(16) BLIPGROUPGPU
{
	glm::mat4 model;
	glm::vec4 params;   // groupAlpha, rSFlying, blipMode, unused
	glm::vec4 clqScale; // g0, g1, g2, unused
	glm::vec4 clqAlpha; // g0, g1, g2, unused
};

struct STREAM
{
	GLuint bufferObject;
	GLuint bindIndex;
	GLsizeiptr frameBase;
	GLsizeiptr cursor;
	GLsizeiptr stride; // use GLsizeiptr, not GLuint
	GLsizeiptr frameSize; // bytes in this frame slice
	uint8_t* mappedPtr; // persistent mapping; null on the glBufferSubData path
	// Array streams only: the bound SSBO range starts windowBase bytes into
	// the frame slice and may hold at most cwindow elements. Records are
	// indexed relative to it. iwindow changes whenever the range is rebound,
	// which invalidates every index handed out before.
	GLsizeiptr windowBase;
	GLsizeiptr cwindow;
	uint32_t iwindow;
};

struct BLOTLINEVERTEX
{
	glm::vec3 position;
	glm::vec2 uv;
	glm::vec4 color;
};

class GL
{
	public:
	// Window Object
	GLFWwindow* window;
	// Frame Buffer Object
	GLuint fbo;
	// Frame Buffer Color
	GLuint fbc;
	// Render Buffer Object
	GLuint rbo;

	GLuint dyshFbo;
	GLuint dyshFbc;
	GLuint dyshRbo;

	int dyshWidth;
	int dyshHeight;

	GLuint fboMSAA;
	GLuint rboColorMSAA;
	GLuint rboDepthStencilMSAA;

	// Screen Array Object
	GLuint sao;
	// Screen Buffer Object
	GLuint sbo;
	glm::mat4 screenProjection;

	// Text Array Object
	GLuint gao;
	// Text Buffer Object
	GLuint gbo;
	GLuint geo;
	// Dynamic glyph geometry. Text is expanded into screen-space triangles so
	// an entire font/texture run can be submitted with one draw call.
	GLuint fontVao;
	GLuint fontVbo;
	GLsizeiptr fontVboCapacity;

	// Instanced particle billboard geometry.
	GLuint blipVao;
	GLuint blipVbo;
	GLuint blipEbo;
	GLuint blipInstanceVbo;
	GLsizeiptr blipInstanceCapacity;
	GLuint sqtrVao;
	GLuint sqtrVbo;
	// 0..DRAW_INDEX_COUNT-1, read through DRAW_INDEX_ATTRIBUTE in every VAO
	// that draws with the glob or cel-border program.
	GLuint drawIndexVbo;
	// Cel borders read only SSBOs; this VAO holds just the draw index.
	GLuint celVao;
	glm::mat4 blotProjection;

	// Window width
	float width;
	// Window height
	float height;
	// Pixel dimensions of the off-screen 3D render target. These may differ
	// from width/height when internal resolution scaling is enabled.
	int renderWidth = 1;
	int renderHeight = 1;
	int outputWidth = 1;
	int outputHeight = 1;
	int presentX = 0;
	int presentY = 0;
	int presentWidth = 1;
	int presentHeight = 1;
	// Uniform transform from the original 640 x 492.8 UI canvas into the
	// current framebuffer. Updated by FrameBufferSizeCallBack.
	float uiScale = 1.0f;
	glm::vec2 uiOrigin = glm::vec2(0.0f);
	glm::vec2 screenOffset;
	glm::vec2 screenOffsetOriginal;

	float aspectRatio;
	AspectMode aspectMode;

	void InitGL();
	void CreateFramebuffers(int w, int h);
	bool ResizeFramebuffers(int w, int h);
	void UpdateGLProjections();
	void TerminateGL();
};

void InitCameraUbo();
void InitFrameStream(STREAM* pstream, int streamSize);
void AdvanceFrameStreams();
void BeginFrameStream(STREAM* pstream, int cappend);
void AppendStream(STREAM* pstream, void* ptr, int size, int copySize);
void InitArrayStream(STREAM* pstream, GLuint binding, GLsizeiptr stride, GLsizeiptr cwindow);
void BeginFrameArrayStream(STREAM* pstream, int cappend);
int AppendArrayStream(STREAM* pstream, const void* ptr, int copySize);
void DeleteFrameStream(STREAM* pstream);
void BindDrawIndexAttribute();
bool ApplyMsaaSettings();
bool ApplyInternalResolutionSettings();
bool ApplyWindowModeSettings(WindowMode mode);
bool ApplyMonitorSettings(int monitorIndex);
int VideoMonitorCount();
const char* VideoMonitorName(int monitorIndex);
int VideoMonitorIndexByName(const char* name);
void ApplyOutputGammaSettings(float gamma);
void ApplyAutoIconifySettings(bool enabled);
bool ApplyAspectRatioSettings(AspectMode mode);
void FrameBufferSizeCallBack(GLFWwindow* window, int width, int height);

extern GL g_gl;
extern GLuint cmUBO;
extern STREAM ropStream;
extern STREAM rcbStream;
extern STREAM drawStream;
extern STREAM blipStream;
extern GLuint geomUBO;
extern GLint glslBlipCurrentTime;
extern GLint glslBlipDtFrame;
extern GLint glslBlipCameraMat;
extern GLint glslBlipAlphaPass;
extern GLuint glslLsmShadow;
extern GLuint glslLsmDiffuse;
extern GLuint glslFogType;
extern GLuint glslFogNear;
extern GLuint glslFogFar;
extern GLuint glslFogMax;
extern GLuint glslFogColor;
extern GLuint glslAlphaCutOff;
extern GLint glslShadowHalfTexel;
extern GLint glslFarDepth;
extern GLuint glslDyshMatWorldClip;
extern GLuint glslDyshModel;
extern GLuint glslDyshfSkin;
extern GLuint glslAmbientMap;
extern GLuint glslDiffuseMap;
extern GLuint glslSaturateMap;
extern GLuint glslGeomModelToClip;
extern GLuint glslGeomDebugColor;
extern GLuint u_fontTexLoc;
extern GLuint glslfCull;
extern uint64_t screenTextureHandle;
extern GLuint g_sceneFbo;
extern int g_msaaSamples;
extern bool g_fMsaa;
extern bool g_fVsync;
extern int g_internalResolutionHeight;
extern int g_resolutionScalePercent;
extern int g_monitorIndex;
extern int g_textureFiltering;
extern float g_outputGamma;
extern float g_guiScale;
extern GuiStyle g_guiStyle;
extern WindowMode g_windowMode;
extern bool g_fAutoIconify;
extern float g_drawDistanceMultiplier;
extern int g_frames;
extern float s_dxDisplay;
extern float s_dyDisplay;
extern float s_dxDisplayOriginal;
extern float s_dyDisplayOriginal;
extern GLuint lineVao;
extern GLuint lineVbo;
extern GLuint glslPoseCount;
extern GLuint glslPoseWeights;
extern GLuint glslCelPose;
extern GLuint glslCelPoseCount;
extern GLuint glslCelPoseWeights;

#endif
