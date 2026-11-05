// Exclusive animated overlays, original0x147220..0x147C20.
#include "overlay.h"
#include "worldglobals.h"
#include "activate.h"
#include "alo.h"
#include "display.h"
#include "gl.h"
#include "ai.h"
#include "anim.h"
#include "animfind.h"
#include "asega.h"
#include "clock.h"
#include "cls.h"
#include "lo.h"
#include "objmethod.h"
#include "sm.h"
#include "sound.h"
#include "sw.h"
#include "update.h"
#include <map>
#include <cmath>
#include <cstddef>
#include <algorithm>
#include <cstdio>
#include <cstdlib>

void BindLoPartsTable(ALO*, int, uint32_t);  // orig0x1A64E8, loaders.cpp
glm::mat4 InverseAffineMatrix4(const glm::mat4&);  // orig0x11C318, xfmath.cpp

namespace {
struct OVERLAY {
    LO* plo = nullptr;
    int state = -1;
    float tState = 0.0f;
    ASEGA* pasega = nullptr;
    int canClose = 1, canSkip = 1, accepts = 1;  // +164/+168/+16C: sheet0, sheet1, sheet0-first
    int dialog = -1;                           // +170
};
std::map<LO*, OVERLAY> s_overlays;
WORLD_TABLE(s_overlays);   // (PC: moved aside while a kept world loads, worldglobals.h)
uint32_t s_serial = 0;
WORLD_VALUE(s_serial);   // (PC: moved aside while a kept world loads, worldglobals.h)
LO* s_owner = nullptr;  // orig D_002DD644
WORLD_VALUE(s_owner);   // (PC: moved aside while a kept world loads, worldglobals.h)
GLuint s_vao = 0, s_vbo = 0;
struct OVERLAYVTX {
    glm::vec2 pos;
    glm::vec3 stq;
};
OVERLAY* OwnerOverlay() {
    if (!g_psw || s_serial != g_psw->serial || !s_owner) return nullptr;
    auto it = s_overlays.find(s_owner);
    return it == s_overlays.end() ? nullptr : &it->second;
}
// orig: 0x147410 (exclusive ownership, real-time factor and the animation's sound wait).
void State(OVERLAY& p, int state) {
    if (state == p.state) return;
    if (p.state == -1) {
        s_owner = p.plo;
        SetSoundMgrFlags(2, false); SetClockFactor(&g_clock, 8, 0.0f); PauseGame(0);
        AddLo(g_psw, p.plo);
        if (!FLoActive(p.plo)) {
            LO* root = PloRoot(p.plo);
            root->fNodeActive = true;
            RegisterLoTree(g_psw, root);
        }
    } else if (p.state == 3 && p.pasega) SetAsegaAsegSpeed(p.pasega, 1.0f);
    const int old = p.state;
    p.state = state; p.tState = g_clock.t3C;
    if (state == -1) {
        s_owner = nullptr;
        g_gl.ReleaseOverlayFrame();
        SetClockFactor(&g_clock, 8, 1.0f); SetSoundMgrFlags(2, true); ResumeGame(0); RemoveLo(g_psw, p.plo);
    } else if (state == 3 && p.pasega) SetAsegaAsegSpeed(p.pasega, 0.0f);
    if (std::getenv("SLY2N_PHOTOPRINT")) std::printf("overlay %d: t %.6f %d -> %d dialog %d\n", p.plo->oid, g_clock.t3C, old, state, p.dialog);
}
// orig: 0x147278.
void UpdateOverlay(LO* plo) {
    if (!g_psw || s_serial != g_psw->serial) return;
    auto it = s_overlays.find(plo);
    if (it == s_overlays.end()) return;
    OVERLAY& p = it->second;
    switch (p.state) {
    case 0: State(p, 1); break;
    case 3: if (!FDialogPlaying(p.dialog)) State(p, 2); break;
    case 2:
        if (!p.pasega || FAsegaRetracted(p.pasega)) {
            if (p.pasega) ReleaseAsega(p.pasega);  // orig1306A8 releases the nullable holder
            p.pasega = nullptr; State(p, -1);
        }
        break;
    default: break;
    }
}
// orig: 0x147338 (the overlay's own ASEGA message9).
bool Message(LO* plo, int kind, void* arg) {
    if (!g_psw || s_serial != g_psw->serial) return false;
    auto it = s_overlays.find(plo);
    if (it == s_overlays.end()) return false;
    if (kind != 9 || !arg) return true;
    OVERLAY& p = it->second;
    const ASEGMSG9& msg = *static_cast<const ASEGMSG9*>(arg);
    if (msg.pasega != p.pasega) return true;
    switch (msg.n) {
    case 0x5FB: p.canClose = 1; break;
    case 0x5FC: p.canClose = 0; break;
    case 0x5FD: p.canSkip = 1; break;
    case 0x5FE: p.canSkip = 0; break;
    case 0x5FF: p.accepts = 0; break;
    case 0x600: p.accepts = 1; break;
    case 0x601: if (p.dialog != -1) State(p, 3); break;
    }
    return true;
}
struct INIT {
    INIT() {
        RegisterLoUpdate(0x147278, UpdateOverlay);
        RegisterLoMessageHandler(Message);
        RegisterObjectMethod(0x10E4A8, [](LO* p, const std::vector<CRef>& a) {
            StartAnimatedOverlay(p, NArg(a, 0, -1), NArg(a, 1, -1)); return CRef(TAGK_Void);
        });
    }
} s_init;
}

// orig: 0x147220 (base start is performed by StartSw; bind its two display parts and remove until used).
void PostLoadAnimatedOverlay(LO* plo) {
    if (!plo || !g_psw) return;
    if (s_serial != g_psw->serial) { ResetAnimatedOverlays(); s_serial = g_psw->serial; }
    OVERLAY& p = s_overlays[plo]; p.plo = plo; p.state = -1;
    BindLoPartsTable(static_cast<ALO*>(plo), 2, 0x2BD380);
    RemoveLo(g_psw, plo);
}

// orig: 0x147C20.
void StartAnimatedOverlay(LO* plo, int oidAseg, int dialog) {
    if (!plo || !FCidDerivedFrom(plo->Cid(), 0x30)) return;
    if (s_serial != g_psw->serial) { ResetAnimatedOverlays(); s_serial = g_psw->serial; }
    OVERLAY& p = s_overlays[plo]; p.plo = plo; p.dialog = dialog;
    p.canClose = p.canSkip = p.accepts = 1;
    LO* loAseg = PloFindSwNearest(plo, oidAseg);  // original lookup1: named group
    ASEG* aseg = dynamic_cast<ASEG*>(loAseg);
    if (!aseg) return;
    aseg->grfAseg |= 0x100;
    SetGrfUpdOfLo(plo, GrfUpdOfLo(plo) | 0x102);
    ApplyAsegCur(aseg, static_cast<ALO*>(plo), 0.0f, 1.0f, 4, &p.pasega);
    State(p, 0);
}

// orig: 0x1477B0, queried before 0x1477D8 changes state 1 to 2.
bool FAnimatedOverlayDrawNormal() {
    const OVERLAY* p = OwnerOverlay();
    return !p || (p->state != 2 && p->state != 3);
}

// Native headless scheduling: keep the two source frames immediately before capture.
bool FAnimatedOverlayNeedsFrame() {
    const OVERLAY* p = OwnerOverlay();
    return p && (p->state == 0 || p->state == 1);
}

// orig: 0x1477D8. Called after the permitted ordinary world and HUD draw.
void DrawAnimatedOverlayFrame() {
    OVERLAY* p = OwnerOverlay();
    if (!p || p->state < 1 || p->state > 3) return;
    if (p->state == 1) {
        g_gl.CaptureOverlayFrame();  // orig0x1475C0, before the state change and black clear
        State(*p, 2);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, g_gl.FboScene());
    glViewport(0, 0, g_gl.renderWidth, g_gl.renderHeight);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClearDepth(0.0);
    // orig0x147890: packet2DF4B0, opaque black with TEST always.
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glDisable(GL_DEPTH_TEST);
    if (!g_gl.texOverlay) return;

    ALO* palo = static_cast<ALO*>(p->plo);
    UpdateXfWorlds(palo);
    glm::mat4 amat[2]{glm::mat4(1.0f), glm::mat4(1.0f)};
    // orig0x22FF40: owner+60 is the ALO's third reference, not its root record.
    const glm::mat4 invOwner = palo->apxfRef[2] ? InverseAffineMatrix4(palo->apxfRef[2]->matWorld) : glm::mat4(1.0f);
    bool bound[2]{};
    for (const auto& part : palo->apartxf)
        for (int i = 0; i < 2; ++i)
            if (part.first == uint32_t(0x158 + i * 4) && part.second) {
                amat[i] = invOwner * part.second->matWorld;
                bound[i] = true;
            }
    // Original immutable tables2BD3B0/2BD3F0/2BD398/2BD410.
    static const glm::vec4 corner[4] = {
        {-0.5f, 0.5f, 0.0f, 1.0f}, {-0.5f, -0.5f, 0.0f, 1.0f},
        {0.5f, 0.5f, 0.0f, 1.0f}, {0.5f, -0.5f, 0.0f, 1.0f}};
    static const glm::vec2 uv[4] = {{0.0f, 0.0f}, {0.0f, 0.21875f}, {0.25f, 0.0f}, {0.25f, 0.21875f}};
    static const glm::mat4 projection(
        glm::vec4(1.0f, 0.0f, 0.0f, 0.0f), glm::vec4(0.0f, -1.0f, 0.0f, 0.0f),
        glm::vec4(0.0f, 0.0f, 1.0f, -0.5f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
    if (!s_vao) {
        glGenVertexArrays(1, &s_vao);
        glGenBuffers(1, &s_vbo);
        glBindVertexArray(s_vao);
        glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(OVERLAYVTX), reinterpret_cast<void*>(offsetof(OVERLAYVTX, pos)));
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(OVERLAYVTX), reinterpret_cast<void*>(offsetof(OVERLAYVTX, stq)));
    }
    glOverlayShader.Use();
    glUniform1i(glOverlayShader.Loc("tex"), 0);
    glUniform4f(glOverlayShader.Loc("uCanvas"), g_display.uiScale.x, g_display.uiScale.y, g_display.uiOffset.x, g_display.uiOffset.y);
    glActiveTexture(GL_TEXTURE0);
    glBindSampler(0, 0);
    glBindTexture(GL_TEXTURE_2D, g_gl.texOverlay);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
    static int remainingPrints = [] {
        const char* e = std::getenv("SLY2N_OVERLAYPRINT");
        return e ? std::clamp(std::atoi(e), 0, 240) : 0;
    }();
    const bool print = remainingPrints > 0;
    if (print) {
        --remainingPrints;
        std::printf("overlaydraw %d t %.6f state %d flags %d %d %d bound %d %d\n", p->plo->oid, g_clock.t3C,
                    p->state, p->canClose, p->canSkip, p->accepts, bound[0], bound[1]);
        for (int i = 0; i < 2; ++i) {
            std::printf("overlaymatrix %d", i);
            for (int c = 0; c < 4; ++c) for (int r = 0; r < 4; ++r) std::printf(" %.9f", amat[i][c][r]);
            std::printf("\n");
        }
    }
    for (int k = 0; k < 3; ++k) {
        if ((k == 0 && (!p->canClose || !p->accepts)) || (k == 1 && !p->canSkip) ||
            (k == 2 && (!p->canClose || p->accepts))) continue;
        const int part = k & 1;
        if (!bound[part]) continue;
        OVERLAYVTX vtx[4];
        bool valid = true;
        for (int i = 0; i < 4; ++i) {
            // Keep the original two products, then reciprocal and pixel quantization.
            const glm::vec4 local = amat[part] * corner[i];
            const glm::vec4 h = projection * local;
            const float q = 1.0f / h.w;
            const float x = (h.x * q + 0.5f) * 512.0f + 0.5f;
            const float y = (h.y * q + 0.5f) * 224.0f + 0.5f;
            valid = valid && std::isfinite(x) && std::isfinite(y) && std::isfinite(q);
            vtx[i].pos = glm::vec2(std::trunc(x), std::trunc(y));
            vtx[i].stq = glm::vec3((uv[i].x + float(part) * 0.25f) * q, uv[i].y * q, q);
            if (print) std::printf("overlayvertex %d %d xy %.6f %.6f stq %.9f %.9f %.9f\n", k, i,
                                   vtx[i].pos.x, vtx[i].pos.y, vtx[i].stq.x, vtx[i].stq.y, vtx[i].stq.z);
        }
        if (!valid) continue;
        glBufferData(GL_ARRAY_BUFFER, sizeof(vtx), vtx, GL_STREAM_DRAW);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    }
    glBindVertexArray(0);
}

// orig: 0x1477D8's logical transition on frames with no GL draw. Run at the END
// of the frame so the normal-render query still observes state1 on its capture frame.
void PrepareAnimatedOverlayFrame() {
    OVERLAY* p = OwnerOverlay();
    if (p && p->state == 1) State(*p, 2);
}

void ResetAnimatedOverlays() {
    s_overlays.clear();
    s_owner = nullptr;
    s_serial = 0;
    if (!g_fKeptWorldLoad) g_gl.ReleaseOverlayFrame();   // (PC: not for a kept world's load, pcloader.h: the running world's overlay)
}

void ResetAnimatedOverlayGraphics() {
    if (s_vbo) glDeleteBuffers(1, &s_vbo);
    if (s_vao) glDeleteVertexArrays(1, &s_vao);
    s_vbo = s_vao = 0;
}
