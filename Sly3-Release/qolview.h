#pragma once

// PC QoL for wide pictures and the front end's look (Phase 3, the view QoL): kept apart from the ported code.

struct LO;

// The episode menu (class 0x83) on a picture wider than 4:3: its pictures are cut at the 4:3 canvas's border
// (hud.h fClipCanvasObj). SLY2N_EPMENUCLIP=0 turns it off.
bool FClipEpisodeMenuToCanvas(const LO* plo);
// (PC menus) the gadget grid (screen objects 0x375 / 0x376 / 0x377: Sly's, Bentley's, Murray's) and Job
// Help (0x4E0) at 4:3 on a wider picture, as the episode menu: cut at the canvas (their bands and portraits reach
// past it) and pillarboxed black. SLY2N_MENU43=0 turns it off (the bands widened as before).
bool FFourThreeMenuObj(const LO* plo);

// The front end's Sly 2 logo (shader 0x657's 256 x 128 texture, on the title and the episode menu) replaced by the
// launcher's high-quality one (launcher/assets/games/sly2-logo.png, embedded): a PC-side texture override, the disc
// untouched. Given the shader's id and its decoded texture, returns the override's GL texture (mipmapped) or 0, and the
// rectangle of the quad (u0, v0, u1, v1 in the quad's 0..1 display space, top-left origin) the new logo takes: the
// old logo's body (without its "TM"), the new one's aspect kept. display.h fHqLogo / --hq-logo 0 / SLY2N_HQLOGO=0: off.
struct SHD;
unsigned GlTexLogoOverride(const SHD* pshd, float arect[4]);
