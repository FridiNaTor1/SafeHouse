#pragma once

// Upscaling (Settings.ini [Graphics] Upscaling; notes/FSR_2026-11-11.md): the scene drawn below the picture's size and
// scaled up to it by AMD FidelityFX Super Resolution 1.0 (spatial: EASU, then RCAS sharpening; fsreasu.frag,
// fsrrcas.frag) instead of the bilinear stretch. A self-contained pass of two full-screen fragment draws through the
// GL calls every renderer layer takes (OpenGL, OpenGL ES, the Vulkan and deko3d layers' offline programs); where the
// programs cannot be built (the Metal layer has no Metal versions of them) FAvailable is false and the callers keep
// the bilinear scale.
//
// Flat (gl.h GL::BeginNativeUi): the scene upscaled into a picture-sized target, the HUD, menus and overlays drawn into
// that at the window's resolution. VR (vr.cpp): each eye upscaled into its swapchain image.

#include <glad/glad.h>

namespace fsr
{
// The methods (Settings.ini Upscaling Method; more may follow) and AMD's FSR 1 quality presets (the render scale per
// axis: Ultra Quality 1.3x, Quality 1.5x, Balanced 1.7x, Performance 2x)
enum METHOD { METHOD_Fsr1 = 1 };
enum PRESET { PRESET_UltraQuality = 0, PRESET_Quality = 1, PRESET_Balanced = 2, PRESET_Performance = 3 };
float RScalePreset(int preset);
// The rendered size for an output of dxOut x dyOut at the preset (each side output / scale, rounded, at least 1)
void RenderSizeForPreset(int preset, int dxOut, int dyOut, int* pdx, int* pdy);
// The sharpness option (0..100 %) as RCAS's linear amount: 100 % the most (0 stops), 0 % two stops less
float GSharpness(int percent);

// The programs build (once, lazily, for the current renderer); false: FSR cannot run here
bool FAvailable();

struct SOURCE
{
    GLuint tex = 0;
    bool fArray = false;   // tex is a GL_TEXTURE_2D_ARRAY, layer its layer
    int layer = 0;
    int x = 0, y = 0, dx = 1, dy = 1;   // the rectangle of tex drawn
};
// Upscales the source rectangle into fboDst's rectangle (x, y, dx, dy): EASU into the pass's own picture-sized
// texture, then RCAS into fboDst (fFlipY: its rows the other way up, for an OpenXR image on Vulkan). Leaves blending,
// depth, stencil, scissor and culling off, all colour channels written, fboDst bound with the rectangle as viewport,
// and no program, vertex array or texture bound. False (nothing drawn) when FAvailable is false.
bool FUpscale(const SOURCE& src, GLuint fboDst, int x, int y, int dx, int dy, float gSharpness, bool fFlipY = false);
// The pass's texture and programs freed (the context's end)
void Release();
}
