#pragma once
struct LO;
void PostLoadAnimatedOverlay(LO* plo);  // orig0x147220
// orig0x147C20: the exclusive animated overlay (e.g. the accepted photograph's authored title).
void StartAnimatedOverlay(LO* plo, int oidAseg, int dialog);
// orig0x1477B0: query before drawing the ordinary world and HUD.
bool FAnimatedOverlayDrawNormal();
// Native scheduling: draw states0/1 even when a headless run only requested a final screenshot.
bool FAnimatedOverlayNeedsFrame();
// orig0x1477D8: capture and draw after the permitted ordinary scene/HUD.
void DrawAnimatedOverlayFrame();
// orig0x1477D8: logical fallback at the END of a frame without a GL draw.
void PrepareAnimatedOverlayFrame();
void ResetAnimatedOverlays();
void ResetAnimatedOverlayGraphics();
