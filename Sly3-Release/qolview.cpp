// See qolview.h.

#include "qolview.h"
#include "game/sly3.h"

#include "display.h"
#include "lo.h"
#include "render.h"
#include "shd.h"

#include "sly2n_logo.h"

#include <glad/glad.h>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

#include <cstdlib>

bool FClipEpisodeMenuToCanvas(const LO* plo)
{
    static const bool s_fOff = [] { const char* sz = std::getenv("SLY2N_EPMENUCLIP"); return sz && sz[0] == '0'; }();
    return !s_fOff && plo && plo->Cid() == S3CID(0x83) && g_display.uiScale.x < 1.0f;
}

bool FFourThreeMenuObj(const LO* plo)
{
    static const bool s_fOff = [] { const char* sz = std::getenv("SLY2N_MENU43"); return sz && sz[0] == '0'; }();
    // (Sly 3: the menu pages, full-screen 4:3 art whose backdrop layers run raggedly past the canvas: the menu's wheel
    // 1056 and 1057, the pause pages 1062..1065 (Gadget Grid, Buy Gadgets, ...) and 1389 (Jobs / Goals))
    return !s_fOff && plo && ((plo->oid >= 1062 && plo->oid <= 1065) || plo->oid == 1056 || plo->oid == 1057 || plo->oid == 1389) &&
           g_display.uiScale.x < 1.0f;
}

namespace
{
// FNV-1a 64 of the original logo's decoded texture (256 x 128 RGBA, PS2 colour units)
constexpr uint64_t HASH_LOGO = 0x7dd4b4b8d2dde3b5ull;
constexpr int OID_SHD_LOGO = 0x657;
float s_aspect = 1.0f;   // (the new logo's)

uint64_t HashBytes(const std::vector<uint8_t>& ab)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint8_t b : ab) h = (h ^ b) * 0x100000001b3ull;
    return h;
}

GLuint TexLogo()
{
    static GLuint s_tex = 0;
    static bool s_fTried = false;
    if (s_fTried) return s_tex;
    s_fTried = true;
    int dx = 0, dy = 0;
    stbi_uc* pb = stbi_load_from_memory(sly2n_logo_png, int(sizeof(sly2n_logo_png)), &dx, &dy, nullptr, 4);
    if (!pb) return 0;
    // rows bottom-up (the quads put t = 1 at the top, as the original's textures); the colours in the original's
    // units: RGB halved as the old logo's texels (their 0..128 is the menu's own brightness), alpha 0..128 = opaque
    std::vector<uint8_t> ab(size_t(dx) * dy * 4);
    for (int y = 0; y < dy; ++y)
        for (int x = 0; x < dx; ++x)
        {
            const stbi_uc* ps = pb + (size_t(y) * dx + x) * 4;
            uint8_t* pd = ab.data() + (size_t(dy - 1 - y) * dx + x) * 4;
            for (int k = 0; k < 3; ++k) pd[k] = uint8_t((ps[k] + 1) / 2);
            pd[3] = uint8_t((ps[3] * 128 + 127) / 255);
        }
    stbi_image_free(pb);
    glGenTextures(1, &s_tex);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dx, dy, 0, GL_RGBA, GL_UNSIGNED_BYTE, ab.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    s_aspect = float(dx) / float(dy);
    return s_tex;
}
}  // namespace

unsigned GlTexLogoOverride(const SHD* pshd, float arect[4])
{
    static const bool s_fOff = [] { const char* sz = std::getenv("SLY2N_HQLOGO"); return sz && sz[0] == '0'; }();
    if (s_fOff || !g_display.fHqLogo || !pshd || pshd->id != OID_SHD_LOGO) return 0;
    // (the texture's identity: shader ids are per world; checked once per bitmap)
    const TEXSEL sel = TexselFromShd(pshd, 0, 0);
    if (!sel.pbmp || sel.pbmp->dx != 256 || sel.pbmp->dy != 128) return 0;
    static std::unordered_map<const void*, bool> s_mpfLogo;
    auto it = s_mpfLogo.find(sel.pbmp);
    if (it == s_mpfLogo.end())
    {
        std::vector<uint8_t> rgba;
        std::string err;
        const bool f = FDecodeTexture(sel.pbmp, sel.pclut, rgba, err) && HashBytes(rgba) == HASH_LOGO;
        it = s_mpfLogo.emplace(sel.pbmp, f).first;
    }
    if (!it->second) return 0;
    const GLuint tex = TexLogo();
    if (!tex) return 0;
    // the old logo's body in the quad (texels 26..230 x 2..125 of 256 x 128, its "TM" right of it), shown on the 4:3
    // canvas (a canvas unit is 1 wide by 1.0714 tall); the quad's own aspect comes from the caller (arect[0] = w / h)
    const float rQuad = arect[0];
    const float u0 = 26.0f / 256.0f, u1 = 230.0f / 256.0f, v0 = 2.0f / 128.0f, v1 = 125.0f / 128.0f;
    const float rBody = rQuad * (u1 - u0) / (v1 - v0);
    float du = u1 - u0, dv = v1 - v0;
    if (s_aspect < rBody) du *= s_aspect / rBody;   // (narrower: centred across)
    else dv *= rBody / s_aspect;
    const float uc = 0.5f * (u0 + u1), vc = 0.5f * (v0 + v1);
    arect[0] = uc - 0.5f * du;
    arect[1] = vc - 0.5f * dv;
    arect[2] = uc + 0.5f * du;
    arect[3] = vc + 0.5f * dv;
    return tex;
}
