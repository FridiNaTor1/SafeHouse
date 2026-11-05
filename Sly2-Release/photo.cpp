// The authored photo targets (cid 0x2E) and the binocucom's photo helper.
#include "photo.h"
#include "overlay.h"
#include "disc.h"
#include "actor.h"
#include "activate.h"
#include "ai.h"
#include "alo.h"
#include "anim.h"
#include "animfind.h"
#include "binoc.h"
#include "clock.h"
#include "cls.h"
#include "cm.h"
#include "cplcy.h"
#include "detect.h"
#include "objmethod.h"
#include "po.h"
#include "screenrt.h"
#include "savedata.h"
#include "splayer.h"
#include "so.h"
#include "sound.h"
#include "sw.h"
#include "task.h"
#include "worldaccess.h"
#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <vector>

void RequestLoTaskState(LO*, int, LO*);  // orig: 0x164640
extern int g_iframeHarness;  // existing frame observer (only used by PHOTOPRINT)

namespace {
struct PHOTOTARGET {
    LO* plo = nullptr;
    float radius = 75.0f, size = 75.0f, tolerance = 0.25f;  // +E0/+E4/+E8
    LO* task = nullptr;                                  // +EC
    int done = 0, title = -1, listTitle = -1, dialog = -1; // +F0/+F4/+F8/+FC
    LO* linked = nullptr;                                // +100
    float linkedRadius = 5000.0f;                        // +104
    int visibility = 0, custom = 0, message = -1;         // +108/+10C/+110
};
struct PHOTOPROMPT {
    int state = -1;                                      // +0
    ACTOR* owner = nullptr;                              // +A8's owner
    glm::vec3 screen{320.0f, 246.40000915527344f, 0.0f};  // +10
    glm::vec3 eye{0.0f};                                 // +20
    std::vector<PHOTOTARGET> targets;                     // +30/+34, at most16
    std::vector<PHOTOTARGET*> listed;                     // +74/+78, at most5
    PHOTOTARGET* best = nullptr;                         // +8C
    float centering = 0.0f, size = 0.0f, tHint = 0.0f;    // +90/+94/+9C
    bool rising = false, closePending = false, volume = false;  // +98/+B4/+B8
    uint32_t savedFlags = 0;                             // +A0
    LO* overlay = nullptr;                              // +A4
    LO* primary = nullptr; LO* secondary = nullptr;       // +AC/+B0
    int selected = -1, message = -1;                     // +BC/+C0
};
std::map<ACTOR*, std::unique_ptr<PHOTOPROMPT>> s_prompts;
uint32_t s_serial = 0;
float FloatField(const LO* p, uint32_t off, float fallback) {
    uint32_t u; std::memcpy(&u, &fallback, 4); u = p->U32Field(0, off, u);
    float f; std::memcpy(&f, &u, 4); return f;
}
LO* ObjectField(LO* p, uint32_t off) {
    const int id = static_cast<int>(p->U32Field(0, off, uint32_t(-1)));
    return id == -1 ? nullptr : PloFindSwNearest(p, id);
}
// orig: 0x203568 / 0x2035A8 (target init/load defaults and authored option fields).
PHOTOTARGET Target(LO* plo) {
    PHOTOTARGET t; t.plo = plo;
    t.radius = FloatField(plo, 0xE0, 75.0f);
    t.size = FloatField(plo, 0xE4, 0.0f); if (t.size == 0.0f) t.size = t.radius;
    t.tolerance = FloatField(plo, 0xE8, 0.25f);
    t.task = ObjectField(plo, 0xEC);
    t.title = int(plo->U32Field(0, 0xF4, uint32_t(-1)));
    t.listTitle = int(plo->U32Field(0, 0xF8, uint32_t(-2)));
    if (t.listTitle == -2) t.listTitle = t.title;
    t.dialog = int(plo->U32Field(0, 0xFC, uint32_t(-1)));
    t.linked = ObjectField(plo, 0x100);
    t.linkedRadius = FloatField(plo, 0x104, 5000.0f);
    t.custom = int(plo->U32Field(0, 0x10C, 0));
    t.message = int(plo->U32Field(0, 0x110, uint32_t(-1)));
    return t;
}
// orig: 0x203758 / 0x203790.
bool Done(const PHOTOTARGET& t) {
    if (const TASK* task = dynamic_cast<const TASK*>(t.task)) {
        if (task->nState == -1 && task->prD8.F()) {
            const CRef complete = RefGetProgressKey(task->prD8.akey);
            return (complete.m_tagk == TAGK_Bool && complete.m_bool) || (complete.m_tagk == TAGK_S32 && complete.m_n);
        }
        return task->nState >= 2;
    }
    return t.done != 0;
}
bool Eligible(const PHOTOTARGET& t) {
    if (!t.plo->fInWorld || (t.linked && t.linked->fInWorld)) return false;
    const TASK* task = dynamic_cast<const TASK*>(t.task);
    return !task || task->nState == 1;
}
// orig: 0x201E88 / 0x201F00 (the linked models hidden while viewing, restored afterward).
void Linked(PHOTOPROMPT& p, bool on) {
    for (auto& t : p.targets)
        if (t.linked && (!on || t.plo->fInWorld)) {
            if (on) AddLo(g_psw, t.linked); else RemoveLo(g_psw, t.linked);
        }
}
// orig: 0x202030.
void List(PHOTOPROMPT& p) {
    p.listed.clear();
    for (auto& t : p.targets) {
        const TASK* task = dynamic_cast<const TASK*>(t.task);
        if (t.listTitle != -1 && (!task || task->nState == 1 || task->nState == 2) && p.listed.size() < 5)
            p.listed.push_back(&t);
    }
}
// orig: 0x2021F8 (five authored root matches, D_002C3588).
void ListMarks(PHOTOPROMPT& p) {
    for (size_t i = 0; i < p.listed.size(); ++i) {
        const int m = int(g_disc.U32Elf(0x2C3588 + uint32_t(i * 4)));
        SelectScreenObjPartAt(p.primary, Done(*p.listed[i]) ? 0x33A : 0x33B, m);
    }
}
// orig: 0x201F90 / 0x2022D8.
void Volume(PHOTOPROMPT& p) {
    p.volume = false;
    int count = 0;
    for (LO* o : AploOfClass(g_psw, 0x19))
        if (FLoActive(o)) {
            if (++count > 16) break;
            if (FPointInShape(o, p.eye)) { p.volume = true; break; }
        }
}
bool Active(const PHOTOPROMPT& p) {
    if (p.volume) return true;
    for (const auto& t : p.targets) if (t.plo->fInWorld && (!t.linked || !t.linked->fInWorld)) return true;
    return false;
}
// orig: 0x2020C8.
void DistanceModels(PHOTOPROMPT& p) {
    for (const auto& t : p.targets) if (t.linked) {
        const glm::vec3 d = PosLoWorld(t.plo) - g_cm.pos;
        const float distance = std::sqrt(std::fabs(d.x * d.x + d.y * d.y));
        if (t.linkedRadius < distance) {
            if (!t.linked->fInWorld) SwitchLoFaded(t.linked, true, 0.25f);
        } else if (distance <= t.linkedRadius && t.linked->fInWorld) SwitchLoFaded(t.linked, false, 0.25f);
    }
}
// orig: 0x202350 / 0x202378 (four original rays; nonzero hit means blocked).
void Visibility(PHOTOPROMPT& p, PHOTOTARGET& t) {
    const glm::vec3 at = PosLoWorld(t.plo);
    const glm::vec3 lo = glm::min(at, p.eye), hi = glm::max(at, p.eye);
    std::vector<SOE*> entries;
    FlushSwOxaDirty1(); UpdateSwAaox();  // orig20FAA0: publish dirty attachment boxes before the query
    for (OXA* box : g_physw.apoxaActive) {
        if (hi.x < box->posMin.x || box->posMax.x < lo.x || hi.y < box->posMin.y || box->posMax.y < lo.y ||
            hi.z < box->posMin.z || box->posMax.z < lo.z) continue;
        for (SOE* entry : box->apsoe)
            // orig 0x202350: not flag bit 48, and the entry's +0x70 (its camera hull's node count, the tree header's
            // third word BSP::x8, as 0x14F030) positive: entries without a camera hull (detailed props, actors) never
            // block a photo; the rays then test the whole tree (0x14CC58 -> 0x14C6A0's +0x74)
            if (!entry->f48 && !entry->bsp.abspn.empty() && 0 < static_cast<int>(entry->bsp.x8) && entries.size() < 0x100)
                entries.push_back(entry);
    }
    const glm::vec3 toward = glm::normalize(p.eye - at);
    const glm::vec3 cross = glm::cross(g_cm.mat[1], toward);
    const glm::vec3 lateral = glm::normalize(cross) * t.size;
    const glm::vec3 center = at + toward * t.size;
    const glm::vec3 up = g_cm.mat[1] * t.size;
    const glm::vec3 points[4] = {center, center + up, center - up, center + lateral};
    int blocked = 0;
    static const bool s_fWhy = std::getenv("SLY2N_PHOTOWHY") != nullptr;
    for (const auto& pt : points)
    {
        LSG hit;
        if (SOE* ph = PsoeHitTestLineObjects(0, p.eye, pt, entries, &hit))
        {
            ++blocked;
            if (s_fWhy)
                std::printf("photowhy %d: ray blocked by %d (cid 0x%X) at (%.0f %.0f %.0f), eye (%.0f %.0f %.0f)\n", t.plo->oid, ph->palo ? ph->palo->oid : -1,
                            ph->palo ? ph->palo->Cid() : 0, hit.pos0.x, hit.pos0.y, hit.pos0.z, p.eye.x, p.eye.y, p.eye.z);
        }
    }
    t.visibility = blocked == 0 ? 1 : blocked == 4 ? 2 : 3;
}
// orig: 0x202540.
void Pick(PHOTOPROMPT& p) {
    const ACTORPOSE pose = PoseFromLo(p.owner->palo);
    const ACTORCMCTX ctx{g_cm.fCut != 0, g_cm.psoFocusPrev == p.owner->palo, g_clock.t, g_clock.dt, 0.0f};
    if (const SLY* sly = dynamic_cast<SLY*>(p.owner)) p.eye = PosSlyFocus(*sly, pose, ctx);
    else p.eye = PosActorFocus(*p.owner, pose, ctx);
    p.eye.z += 20.0f;
    PHOTOTARGET* best = nullptr;
    float minimum;
    const uint32_t minimumBits = g_disc.U32Elf(0x2A8550); std::memcpy(&minimum, &minimumBits, 4);
    static const bool s_fWhy = std::getenv("SLY2N_PHOTOWHY") != nullptr;  // (debug: why each target is not picked)
    for (auto& t : p.targets) {
        if (!Eligible(t)) { if (s_fWhy) std::printf("photowhy %d: not eligible (in %d, linked %d in %d radius %.0f, cam distance %.0f)\n", t.plo->oid, t.plo->fInWorld, t.linked ? t.linked->oid : -1, t.linked ? t.linked->fInWorld : -1, t.linkedRadius, glm::length(glm::vec2(PosLoWorld(t.plo) - g_cm.pos))); continue; }
        const glm::vec3 pos = PosLoWorld(t.plo);
        if (!FInsideCmPlanes(&g_cm, pos - g_cm.pos, t.radius)) { if (s_fWhy) std::printf("photowhy %d: outside the view (radius %.1f)\n", t.plo->oid, t.radius); continue; }
        float alpha;
        if (!FInsideCmMrd(&g_cm, pos, t.plo->sRadius, &alpha)) { if (s_fWhy) std::printf("photowhy %d: beyond the draw distance (radius %.1f)\n", t.plo->oid, t.plo->sRadius); continue; }
        const glm::vec3 d = pos - g_cm.pos;
        const float distance = std::sqrt(std::fabs(d.x * d.x + d.y * d.y));
        if (s_fWhy) std::printf("photowhy %d: distance %.1f (minimum %.1f)\n", t.plo->oid, distance, minimum);
        if (distance < minimum) {
            Visibility(p, t);
            if (s_fWhy) std::printf("photowhy %d: visibility %d\n", t.plo->oid, t.visibility);
            if (t.visibility != 2) { best = &t; minimum = distance; }
        }
    }
    if (best != p.best) { p.best = best; SetPhotoPromptState(&p, 2); }
}
void Refresh(PHOTOPROMPT& p) {
    SelectScreenObjPart(p.primary, 0x35D);
    if (!p.listed.empty()) SelectScreenObjPart(p.primary, 0x35E);
    ListMarks(p);
}
void Trace(const PHOTOPROMPT& p, int old) {
    if (std::getenv("SLY2N_PHOTOPRINT")) std::printf("photo %d: frame %d t %.6f %d -> %d target %d center %.6f size %.6f visibility %d\n",
        p.owner->palo->oid, g_iframeHarness, g_clock.t, old, p.state, p.best ? p.best->plo->oid : -1, p.centering, p.size, p.best ? p.best->visibility : -1);
}
struct INIT {
    INIT() {
        // orig: 0x203758 (EOPID1204): the scripted photograph-completed query.
        RegisterObjectGetter(1204, [](LO* plo) {
            CRef out;
            if (g_psw && s_serial == g_psw->serial)
                for (const auto& [owner, p] : s_prompts)
                    for (const auto& t : p->targets)
                        if (t.plo == plo) { out.SetBool(Done(t)); return out; }
            out.SetBool(plo && Done(Target(plo)));
            return out;
        });
    }
} s_init;
}  // namespace

// orig: 0x201D78 / 0x201DC8.
void* PostLoadPhotoPrompt(ACTOR* pa) {
    if (!pa || !FCidDerivedFrom(pa->palo->Cid(), 0x35)) return nullptr;
    if (s_serial != g_psw->serial) { s_prompts.clear(); s_serial = g_psw->serial; }
    auto p = std::make_unique<PHOTOPROMPT>(); p->owner = pa;
    p->primary = PloFindSwAny(0x362); p->secondary = PloFindSwAny(0x365); p->overlay = PloFindSwAny(0x602);
    for (const auto& o : g_psw->aploAll)
        if (FCidDerivedFrom(o->Cid(), 0x2E) && p->targets.size() < 16) p->targets.push_back(Target(o.get()));
    Linked(*p, false);
    PHOTOPROMPT* out = p.get(); s_prompts[pa] = std::move(p); return out;
}
int NPhotoPromptState(const void* pv) { return pv ? static_cast<const PHOTOPROMPT*>(pv)->state : -1; }

// Native render scheduling only: 0x202768 leaves the title picture in the previous frame on leaving state9.
bool FPhotoCapturePending() {
    if (!g_psw || s_serial != g_psw->serial) return false;
    for (const auto& entry : s_prompts)
        if (entry.second->state == 9 && entry.second->overlay) return true;
    return false;
}

// orig: 0x211148 / 0x2111F0, with 0x202730 / 0x2026F8: the subject, feedback and listed target names.
std::string StrPhotoScript(int id) {
    if (id < 0x2DD || id > 0x2E4 || !g_psw || s_serial != g_psw->serial) return "";
    ACTOR* owner = PactorFromLo(PloPoCurrent(g_psw));
    auto it = s_prompts.find(owner);
    if (it == s_prompts.end()) return "";
    const PHOTOPROMPT& p = *it->second;
    int text = -1;
    if (id == 0x2DD) text = p.selected;
    else if (id == 0x2DE) text = p.message;
    else if (size_t(id - 0x2DF) < p.listed.size()) text = p.listed[id - 0x2DF]->listTitle;
    return text == -1 ? "" : StrLocalized(g_psw, text);
}

// orig: 0x202768 (the helper's authored state machine; no task or target IDs are special-cased).
void SetPhotoPromptState(void* pv, int next) {
    if (!pv) return;
    PHOTOPROMPT& p = *static_cast<PHOTOPROMPT*>(pv);
    const int old = p.state;
    if (next == old || !p.primary || !p.secondary) return;
    const bool wasPaused = unsigned(old - 8) < 2, paused = unsigned(next - 8) < 2;
    if (wasPaused != paused) { if (paused) PauseGame(0); else ResumeGame(0); }
    switch (old) {
    case 1: DistanceModels(p); List(p); Refresh(p); break;
    case 4: ResetScreenObjPart(p.secondary, 0x359); break;
    case 5: case 6: ResetScreenObjPart(p.secondary, 0x35A); break;
    case 7:
        ResetScreenObjPart(p.primary, 0x356); ResetScreenObjPart(p.secondary, 0x356); ResetScreenObjPart(p.secondary, 0x357); break;
    case 8:
        SetGrfUpdOfLo(p.owner->palo, p.savedFlags);
        SetClockFactor(&g_clock, 0, 1.0f); SetSoundMgrFlags(1, true);
        ResetScreenObjPart(p.primary, 0x358); break;
    case 9:
        g_cmpolicies.cprecon.ResetZoom();
        SetScreenObjListed(p.primary, false); ShowScreenObj(p.primary, 0x310);
        if (p.overlay && p.best) {
            if (p.best->dialog != -1) PsndStartDialog(p.best->dialog);
            StartAnimatedOverlay(p.overlay, 0x608, p.best->dialog);  // orig0x147C20
        }
        if (p.best) {
            if (p.best->task) RequestLoTaskState(p.best->task, 2, p.best->plo);  // orig0x2036E8
            else p.best->done = 1;
        }
        BroadcastTaskEvent(g_psw, 10, nullptr);
        p.best = nullptr;
        if (Active(p)) {
            SelectScreenObjPart(p.primary, 0x35D);
            if (!p.listed.empty()) SelectScreenObjPart(p.primary, 0x35E);
        }
        ListMarks(p);
        break;
    default: break;
    }
    p.state = next;
    switch (next) {
    case 0:
        p.best = nullptr; p.selected = p.message = -1;
        for (auto& t : p.targets) t.visibility = 0;
        ShowScreenObj(p.primary, -1); Linked(p, false); Volume(p);
        BroadcastTaskEvent(g_psw, 9, nullptr); break;
    case 1: ResetScreenObjPart(p.primary, 0x35D); ShowScreenObj(p.secondary, 0x339); break;
    case 2: ShowScreenObj(p.secondary, 0x339); break;
    case 3: case 4:
        SetScreenObjFollow(p.secondary, p.best ? p.best->plo : nullptr);  // orig202768: secondary +0xF4
        ShowScreenObj(p.secondary, 0x337); SelectScreenObjPart(p.secondary, 0x359); SetScreenObjFraction(p.secondary, 0x359, 0.0f); break;
    case 5: case 6:
        SelectScreenObjPart(p.secondary, 0x35A); SetScreenObjFraction(p.secondary, 0x35A, 0.0f); break;
    case 7:
        PsndStartSound(0x52);
        p.selected = p.best ? p.best->title : -1;
        SelectScreenObjPart(p.primary, 0x356); SelectScreenObjPart(p.secondary, 0x356); SelectScreenObjPart(p.secondary, 0x357); break;
    case 8:
        ResetScreenObjPart(p.primary, 0x35B); SelectScreenObjPart(p.primary, 0x358);
        p.savedFlags = GrfUpdOfLo(p.owner->palo); SetGrfUpdOfLo(p.owner->palo, p.savedFlags | 0x102);
        SetSoundMgrFlags(1, false); SetClockFactor(&g_clock, 0, 0.0f); break;
    case 9:
        ShowScreenObj(p.secondary, 0x339); ShowScreenObj(p.primary, 0x35C); SetScreenObjListed(p.primary, true); break;
    case 10:
        p.best = nullptr; p.selected = p.message = -1;
        HideScreenObj(p.secondary); HideScreenObj(p.primary); Linked(p, true);
        BroadcastTaskEvent(g_psw, 11, nullptr); break;
    case -1:
        if (old != 10) {
            BroadcastTaskEvent(g_psw, 11, nullptr);
            for (LO* o : {p.primary, p.secondary}) if (NScreenObjState(o)) { SetScreenObjState(o, 1); SetScreenObjTimers(o, 1); }
        }
        break;
    default: break;
    }
    Trace(p, old);
}

// orig: 0x202D38 (screen-space qualification, shutter, feedback and automatic final-photo close).
void UpdatePhotoPrompt(void* pv) {
    if (!pv) return;
    PHOTOPROMPT& p = *static_cast<PHOTOPROMPT*>(pv);
    if (!p.primary || !p.secondary) return;
    // the original keeps pointers to the targets: +0x10C (option 1040, "not yet") is read live, e.g. Ep4's spycam
    // station 88574's methods 1746 / 1747 clear / set it on its code target 77142 around the guard's code entry
    for (PHOTOTARGET& t : p.targets) t.custom = int(t.plo->U32Field(0, 0x10C, 0));
    if (p.closePending) { p.closePending = false; CloseBinoc(p.owner); }
    if (p.tHint + 5.0f < g_clock.t) ResetScreenObjPart(p.primary, 0x35B);
    if (p.state >= 2 && p.state < 8) {
        Pick(p); DistanceModels(p);
        if (!Active(p)) SetPhotoPromptState(&p, 1);
    }
    if (p.best) {
        const glm::vec3 pos = PosLoWorld(p.best->plo);
        const glm::vec3 first0 = ConvertCmWorldToScreen(&g_cm, pos);
        const glm::vec3 second0 = ConvertCmWorldToScreen(&g_cm, pos + g_cm.mat[1] * p.best->size);
        const glm::vec2 first(first0.x * 320.0f + 320.0f, first0.y * -246.40000915527344f + 246.40000915527344f);
        const glm::vec2 second(second0.x * 320.0f + 320.0f, second0.y * -246.40000915527344f + 246.40000915527344f);
        float center = glm::length(first - glm::vec2(p.screen));
        center = center - 128.0f; center = center * 0.0078125f; center = 1.0f - center;
        p.centering = std::clamp(center, 0.0f, 1.0f);
        p.size = glm::length(first - second) * (1.0f / 120.0f);
        p.rising = true;
        if (1.0f < p.size) { p.rising = false; p.size = 2.0f - p.size; }
        p.size = std::max(p.size, 0.0f);
    }
    // (debug: SLY2N_PHOTOAIM=K: every K frames each target's screen point (640 x 492.8), the reticle, size, state)
    if (static const int s_kAim = std::getenv("SLY2N_PHOTOAIM") ? std::atoi(std::getenv("SLY2N_PHOTOAIM")) : 0;
        s_kAim > 0 && g_iframeHarness % s_kAim == 0)
        for (const auto& t : p.targets) {
            const glm::vec3 pos = PosLoWorld(t.plo);
            const glm::vec3 s0 = ConvertCmWorldToScreen(&g_cm, pos);
            const glm::vec3 s1 = ConvertCmWorldToScreen(&g_cm, pos + g_cm.mat[1] * t.size);
            const glm::vec2 a(s0.x * 320.0f + 320.0f, s0.y * -246.4f + 246.4f), b(s1.x * 320.0f + 320.0f, s1.y * -246.4f + 246.4f);
            std::printf("photoaim frame %d state %d target %d at %.0f %.0f %.0f screen %.1f %.1f (z %.2f) reticle %.1f %.1f size %.3f best %d done %d\n",
                        g_iframeHarness, p.state, t.plo->oid, pos.x, pos.y, pos.z, a.x, a.y, s0.z, float(p.screen.x), float(p.screen.y),
                        glm::length(a - b) / 120.0f, p.best ? p.best->plo->oid : -1, int(Done(t)));
        }
    auto nearFull = [&] { return p.best && 1.0f - p.best->tolerance <= p.size; };
    switch (p.state) {
    case 0: if (FScreenObjBusy(p.primary, 0x314)) SetPhotoPromptState(&p, 1); break;
    case 1: DistanceModels(p); if (Active(p)) SetPhotoPromptState(&p, 2); break;
    case 2: if (p.best && p.best->visibility != 2) SetPhotoPromptState(&p, 4); break;
    case 3:
        if (!p.best) SetPhotoPromptState(&p, 2);
        else if (p.best->visibility != 3) SetPhotoPromptState(&p, 4);
        break;
    case 4:
        if (!p.best) SetPhotoPromptState(&p, 2);
        else if (p.best->visibility == 3) SetPhotoPromptState(&p, 3);
        else if (1.0f <= p.centering) SetPhotoPromptState(&p, p.rising ? 5 : 6);
        break;
    case 5: case 6:
        if (!p.best) SetPhotoPromptState(&p, 2);
        else if (p.centering < 1.0f) SetPhotoPromptState(&p, 4);
        else if (nearFull()) SetPhotoPromptState(&p, 7);
        else SetScreenObjFraction(p.secondary, 0x35A, p.size);
        break;
    case 7:
        if (!p.best) SetPhotoPromptState(&p, 2);
        else if (p.centering < 1.0f || p.size < 1.0f - p.best->tolerance) SetPhotoPromptState(&p, p.rising ? 5 : 6);
        break;
    case 8:
        if (!FScreenObjBusy(p.primary, 0x358)) {
            bool hint = false;
            if (p.best && p.best->visibility == 1 && 1.0f <= p.centering && nearFull()) {
                if (!p.best->custom) SetPhotoPromptState(&p, 9);
                else { p.message = p.best->message; hint = true; SetPhotoPromptState(&p, 7); }
            } else {
                int off, state;
                if (p.best && p.best->visibility != 1) { off = 0x110; state = 3; }
                else if (1.0f <= p.centering && p.rising) { off = 0x114; state = 5; }
                else if (1.0f <= p.centering && !p.rising) { off = 0x118; state = 6; }
                else if (p.best) { off = 0x11C; state = 4; }
                else { off = 0x120; state = 2; }
                p.message = int(p.primary->U32Field(0, uint32_t(off), uint32_t(-1)));
                hint = true; SetPhotoPromptState(&p, state);
            }
            if (hint) { SelectScreenObjPart(p.primary, 0x35B); p.tHint = g_clock.t; }
        }
        break;
    case 9:
        if (!FScreenObjBusy(p.primary, 0x35C)) {
            SetPhotoPromptState(&p, 2);
            bool ready = true;
            for (const auto& t : p.targets) {
                const TASK* task = dynamic_cast<const TASK*>(t.task);
                if (!Done(t) && (!task || task->nState == 1)) { ready = false; break; }
            }
            if (ready) p.closePending = true;
        }
        break;
    case 10: if (!FScreenObjBusy(p.primary, 0x314)) SetPhotoPromptState(&p, -1); break;
    default: break;
    }
    if (p.owner->nF20 == 1 || p.state < 2 || p.state > 7) return;
    const uint32_t buttons = p.state == 7 ? 0xFA : 0xA;
    if (FActiveCplcy(&g_cmpolicies.cpdefault)) return;  // orig0x14FD50
    if (PODATA* pd = PpodataFromLo(p.owner->palo); pd && pd->ppadctl && GrfPadctlTakePressed(pd->ppadctl, buttons, 0.0f))
        SetPhotoPromptState(&p, 8);
}
