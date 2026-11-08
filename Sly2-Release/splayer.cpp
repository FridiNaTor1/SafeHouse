// See splayer.h. Reference ports: Sly2-Reference game/font_brx.cpp (0x2183E8, 0x219FE0, 0x219400, 0x219348,
// 0x218790, 0x215730, 0x2158C0, 0x2159E0, 0x2174C8, 0x216BC8), m_210E90_c.cpp (0x213300, 0x213830).

#include "splayer.h"
#include "shop.h"
#include "region.h"
#include "porttext.h"

#include "promptglyph.h"
#include "hudwidget.h"
#include "menu.h"
#include "controlspage.h"
#include "transition.h"

#include "rtgt.h"

#include "hud.h"
#include "sw.h"
#include "sound.h"
extern int g_iframeMain;  // (game.cpp: debug prints)

#include <algorithm>
#include <cmath>
#include <map>
#include <cstdio>
#include <cstdlib>

namespace
{
constexpr float UI_KX = 0.8f, UI_KY = 5.0f / 11.0f, GS_ORIGIN = 786432.0f;
// The screen objects' canvas origin: the object's +0xC8 (itself) -> +0x9C / +0xA0, set by 0x2123C8 from its canvas size
// +0x94 / +0x98 (640 x 480: 2048 - 256, 2048 - 240 x 5 / 11 = 1938.909, not the frame's 1936)
// orig: 0x002123C8 (the origin part)
glm::vec2 OriginOfScreen(const SCREENOBJ* pso)
{
    const uint32_t dx = pso ? pso->ul94 : 640u, dy = pso ? pso->ul98 : 480u;
    const int h = int((dx << 9) / 640u);
    const float w = float(int(dy)) * UI_KY;
    return glm::vec2(2048.0f - float(h) * 0.5f, 2048.0f - w * 0.5f);
}

uint8_t Ch(uint32_t c, int i) { return uint8_t(c >> (8 * i)); }

// orig: 0x0021A8E8-ish lerp of 0x219FE0 (LerpChannel: towards b by u, rounded up a step)
uint32_t LerpRgba(uint32_t a, uint32_t b, float u)
{
    uint32_t r = 0;
    for (int i = 0; i < 4; ++i)
    {
        const uint8_t x = Ch(a, i), y = Ch(b, i);
        uint8_t z = y;
        if (x < y) z = uint8_t(x + int(u * (float(y) + 0.999f - float(x))));
        else if (x > y) z = uint8_t(y + int((1.0f - u) * (float(x) + 0.999f - float(y))));
        r |= uint32_t(z) << (8 * i);
    }
    return r;
}

// orig: 0x00218790 (the parent's colours (pc0 offset, pc1 scale) with the item's (c0, c1); 127-biased units)
void CombineColors(uint32_t pc0, uint32_t pc1, uint32_t ic0, uint32_t ic1, uint32_t& c0, uint32_t& c1)
{
    c0 = c1 = 0;
    for (int i = 0; i < 4; ++i)
    {
        const float x = std::clamp((float(Ch(pc1, i)) - 127.0f) / 127.0f, -1.0f, 1.0f);
        const float y = std::clamp((float(Ch(ic1, i)) - 127.0f) / 127.0f, -1.0f, 1.0f);
        const float d = float(Ch(ic0, i)) - 127.0f, e = float(Ch(pc0, i)) - 127.0f;
        const float g0 = std::clamp(x * y, -1.0f, 1.0f) * 127.0f + 128.0f;
        const float g1 = std::clamp(x * d + e, -127.0f, 128.0f) + 127.0f;
        c1 |= uint32_t(uint8_t(int(g0))) << (8 * i);
        c0 |= uint32_t(uint8_t(int(g1))) << (8 * i);
    }
}

// the item's 2x3 times the parent's (row vectors: [x y 1] M V)
std::array<float, 6> Mul(const std::array<float, 6>& m, const std::array<float, 6>& v)
{
    return {m[0] * v[0] + m[1] * v[2], m[0] * v[1] + m[1] * v[3], m[2] * v[0] + m[3] * v[2], m[2] * v[1] + m[3] * v[3],
            m[4] * v[0] + m[5] * v[2] + v[4], m[4] * v[1] + m[5] * v[3] + v[5]};
}

std::array<float, 6> MatOfItem(const SITEM& it)
{
    return {it.mat[0][0], it.mat[0][1], it.mat[1][0], it.mat[1][1], it.mat[2][0], it.mat[2][1]};
}

const SELEM* PelemOf(const SCREENOBJ* pso, int i)
{
    return i >= 0 && size_t(i) < pso->aelem.size() ? &pso->aelem[size_t(i)] : nullptr;
}
}  // namespace

std::string (*g_pfnScriptString)(int id) = nullptr;
bool (*g_pfnTextDrawHook)(const TEXTDRAW& td) = nullptr;

std::string StrLocalized(const SW* psw, int id)
{
    if (!psw) return "";
    for (const auto& [n, str] : psw->atext)
        if (n == id)
        {
            // (native: the PS2's memory card messages said for a PC, porttext.h; not a text mod's string, not with
            // the "Original PlayStation Icons" option)
            if (FPcWording())
                if (const char* sz = SzPcWording(id);
                    sz && std::find(psw->aidTextModded.begin(), psw->aidTextModded.end(), id) == psw->aidTextModded.end())
                    return StrToGameText(sz);
            return str;
        }
    return "";
}

// (debug, SLY2N_SCREENLOOPSCAN: screenrt.cpp once per world) the parts whose keys start a looper (the world's sound
// table: kind 2), with their stop keys; a looper not stopped by a key is stopped only when its entry stops (0x218660)
void PrintScreenLoopers(const SCREENOBJ* pso)
{
    for (size_t g = 0; g < pso->agroup.size(); ++g)
        for (const SPART& part : pso->agroup[g].apart)
            for (size_t k = 0; k < part.akey.size(); ++k)
                for (const auto& [kind, n] : part.akey[k].atag)
                {
                    if (kind != 0) continue;
                    const int is = IsoundFromSfxid(n);
                    if (is < 0 || size_t(is) >= g_sound.aflagsSound.size() || g_sound.aflagsSound[size_t(is)] != 2) continue;
                    int kStop = -1;
                    for (size_t k2 = 0; k2 < part.akey.size() && kStop < 0; ++k2)
                        for (const auto& [kind2, n2] : part.akey[k2].atag)
                            if (kind2 == 1) kStop = int(k2);
                    std::printf("screenloop: screen %d group %zu part %d key %zu / %zu (t %.2f of %.2f) sfx %d (bank sound %d), stop key %d\n",
                                pso->oid, g, part.id, k, part.akey.size(), part.akey[k].t, part.dt, n, is, kStop);
                }
}

void CScreenPlayer::Init(const SCREENOBJ* p)
{
    pso = p;
    agroup.assign(pso->agroup.size(), SGROUPRT{});
    for (size_t g = 0; g < pso->agroup.size(); ++g)
    {
        agroup[g].aentry.resize(pso->agroup[g].cframeSet);
        for (SENTRYRT& e : agroup[g].aentry) e.aslot.resize(pso->agroup[g].cframe);
    }
}

namespace
{
// The looping sounds the key events keep per entry (orig: the entry's +0x14)
std::map<const SENTRYRT*, CSndRef>& MpSndEntry()
{
    static std::map<const SENTRYRT*, CSndRef> s_map;
    return s_map;
}

// orig: 0x00218660's Release(&e->x14): a playing entry stopped also stops the looper its keys kept (R45: the
// Contessa's hypnosis overlay, screen 65521 part 784, starts looper 524 at key 0 and stops it at key 76 two seconds
// later; the part loops, so the overlay's end leaves the newest start playing, one more each hypnosis)
void StopEntrySound(const SENTRYRT& e)
{
    auto it = MpSndEntry().find(&e);
    if (it == MpSndEntry().end()) return;
    if (it->second) it->second->Stop(0.0f);
    MpSndEntry().erase(it);
}
}

void CScreenPlayer::ReleaseSlot(SSLOT& slot)
{
    // orig: 0x002158C0 (kind 4: the nested entry stops) then 0x002156B8 (the slot's draw state reset)
    if (slot.pelem && slot.pelem->kind == SELEMK_Ref && slot.ientrySub >= 0)
    {
        const int g = slot.pelem->ref;
        if (g >= 0 && size_t(g) < agroup.size() && size_t(slot.ientrySub) < agroup[size_t(g)].aentry.size())
        {
            SENTRYRT& e = agroup[size_t(g)].aentry[size_t(slot.ientrySub)];
            // orig 0x218660: the group remembers where it stopped
            if (e.ipart >= 0)
            {
                if ([] { static const bool f = std::getenv("SLY2_NESTDBG") != nullptr; return f; }())
                    std::printf("nest stop: f %lld oid 0x%X group %d entry %d part %d t %g\n", (long long)g_cframeScreen,
                                pso->oid, g, slot.ientrySub, e.ipart, e.t);
                agroup[size_t(g)].ipartStopped = e.ipart;
                agroup[size_t(g)].tStopped = e.t;
                agroup[size_t(g)].frameStopped = g_cframeScreen;
                agroup[size_t(g)].ipartSel = -1;
                StopEntrySound(e);
            }
            for (SSLOT& s : e.aslot) ReleaseSlot(s);
            e.ipart = -1;
            e.ikey = 0;
            e.t = 0.0f;
            e.fUsed = false;
        }
    }
    // orig: 0x002158C0 kind 6: the object's render target given back
    if (slot.prtgtOwner) ReleaseRtgt(slot.prtgtOwner);
    slot = SSLOT{};
}

void CScreenPlayer::StartEntry(int igroup, int ientry, int ipart)
{
    SENTRYRT& e = agroup[size_t(igroup)].aentry[size_t(ientry)];
    for (SSLOT& s : e.aslot) ReleaseSlot(s);
    e.ipart = ipart;
    e.ikey = 0;
    e.t = 0.0f;
    e.fTrig = false;
    e.fUsed = true;
}

bool CScreenPlayer::FStartPart(int idPart)
{
    if (agroup.empty() || agroup[0].aentry.empty()) return false;
    const SGROUP& g = pso->agroup[0];
    for (size_t i = 0; i < g.apart.size(); ++i)
        if (g.apart[i].id == idPart) return FStartPartIndex(int(i));
    return false;
}

bool CScreenPlayer::FStartPartIndex(int ipart)
{
    if (agroup.empty() || agroup[0].aentry.empty() || ipart < 0 || size_t(ipart) >= pso->agroup[0].apart.size()) return false;
    if (agroup[0].aentry[0].ipart >= 0) StopEntrySound(agroup[0].aentry[0]);   // (0x217C20: 0x218638 when playing)
    StartEntry(0, 0, ipart);
    // orig: 0x00217C20 -> 0x00217D98 -> 0x00219FE0: starting an entry places its initial keys immediately.
    // SelectPart/SetPartFraction may follow before the frame update and must see the new nested entries.
    Update(0.0f);
    return true;
}

bool CScreenPlayer::FPartDone() const
{
    if (agroup.empty() || agroup[0].aentry.empty()) return true;
    const SENTRYRT& e = agroup[0].aentry[0];
    if (e.ipart < 0) return true;
    return e.fTrig || e.t >= pso->agroup[0].apart[size_t(e.ipart)].dt;
}


// orig: 0x00219190 (the key's events: 0 a sound (kept by the entry when it loops), 1 the entry's sound stopped,
// 2 a dialog line, 3 the dialog stopped)
void RunScreenKeyEvents(const SENTRYRT* pentry, const SKEY& key)
{
    for (const auto& [kind, n] : key.atag)
    {
        if (kind == 0)
        {
            CSnd* psnd = PsndNewPlain(n);
            psnd->SetSource(nullptr);
            if (psnd->Kind() == 2) MpSndEntry()[pentry] = CSndRef(psnd);
        }
        else if (kind == 1)
        {
            auto it = MpSndEntry().find(pentry);
            if (it == MpSndEntry().end() || !it->second) continue;
            it->second->Stop(0.0f);
            MpSndEntry().erase(it);
        }
        else if (kind == 2) PsndStartDialog(n, 0);
        else if (kind == 3) StopDialog();
    }
}

// orig: 0x00219400 (crossing key ikey: its items placed into their slots)
void CScreenPlayer::CrossKey(int igroup, int ientry, int ikey, const std::array<float, 6>& v, uint32_t pc0, uint32_t pc1)
{
    SENTRYRT& e = agroup[size_t(igroup)].aentry[size_t(ientry)];
    const SPART& part = pso->agroup[size_t(igroup)].apart[size_t(e.ipart)];
    const SKEY& key = part.akey[size_t(ikey)];
    // orig: 0x002190F8 (the key's triggers, code 7: the entry held, 0x218748)
    if (static const bool s_fLog = std::getenv("SLY2N_SCREENKEYLOG") != nullptr; s_fLog && !key.atag.empty())   // (debug)
        for (const auto& [kind, n] : key.atag)
            std::printf("screenkey: screen %d group %d entry %d part %d (id %d) key %d / %zu t %.3f: event %d %d frame %d\n", pso ? pso->oid : -1,
                        igroup, ientry, e.ipart, part.id, ikey, part.akey.size(), key.t, kind, n, g_iframeMain);
    RunScreenKeyEvents(&e, key);  // (0x219190 before 0x2190F8, as 0x219FE0 calls them)
    for (uint8_t b : key.ab)
        if (b == 7) e.fTrig = true;
    // orig: 0x00219348 (the key's released slots)
    for (uint16_t is : key.aw)
        if (is < e.aslot.size()) ReleaseSlot(e.aslot[is]);
    for (const SITEM& rec : key.aitem)
    {
        const uint32_t iitem = rec.aw[1], dLink = rec.aw[2], itext = rec.aw[0];
        if (iitem >= e.aslot.size()) continue;
        SSLOT& y = e.aslot[iitem];
        y.kc0 = 0x7F7F7F7F;
        y.kc1 = 0xFFFFFFFF;
        if (rec.s6 >= 0 && size_t(rec.s6) < key.acolor.size())
        {
            y.kc0 = key.acolor[size_t(rec.s6)][1];
            y.kc1 = key.acolor[size_t(rec.s6)][0];
        }
        y.pit0 = &rec;
        y.pit1 = nullptr;
        y.t0 = key.t;
        y.t1 = 0;
        if (rec.b30)
            for (size_t k = size_t(ikey) + 1; k < part.akey.size() && !y.pit1; ++k)
                for (const SITEM& rec2 : part.akey[k].aitem)
                {
                    if (rec2.aw[1] != iitem) continue;
                    y.pit1 = &rec2;
                    y.t1 = part.akey[k].t;
                    if (rec2.s6 >= 0 && size_t(rec2.s6) < part.akey[k].acolor.size())
                    {
                        y.nc0 = part.akey[k].acolor[size_t(rec2.s6)][1];
                        y.nc1 = part.akey[k].acolor[size_t(rec2.s6)][0];
                    }
                    else { y.nc0 = y.kc0; y.nc1 = y.kc1; }
                    break;
                }
        // the piece: 0 keeps it, the same keeps it, another replaces it (0x215730: kind 4 starts a nested entry)
        if (itext != 0)
        {
            const SELEM* pnew = PelemOf(pso, int(itext));
            if (pnew != y.pelem)
            {
                const SITEM* p0 = y.pit0; const SITEM* p1 = y.pit1;
                const float t0 = y.t0, t1 = y.t1;
                const uint32_t a = y.kc0, b = y.kc1, c = y.nc0, d = y.nc1;
                ReleaseSlot(y);
                y.pit0 = p0; y.pit1 = p1; y.t0 = t0; y.t1 = t1; y.kc0 = a; y.kc1 = b; y.nc0 = c; y.nc1 = d;
                y.pelem = pnew;
                // orig: 0x00215730 kind 6: the object its id names (0x1BEED8) takes a render target (0x1BEB28)
                if (pnew && pnew->kind == SELEMK_Image && pnew->idTexture != -1 && g_pfnRtgtOwnerOfId)
                {
                    const void* powner = g_pfnRtgtOwnerOfId(pnew->idTexture);
                    if (powner && IrtgtAcquire(powner) >= 0) y.prtgtOwner = powner;
                    if (static const bool s_f = std::getenv("SLY2N_RTGTBIND") != nullptr; s_f)   // (debug)
                        std::printf("rtgt bind id 0x%X owner %p taken %d player %d\n", pnew->idTexture, powner, y.prtgtOwner != nullptr, g_transition.ploPlayer ? g_transition.ploPlayer->oid : -1);
                }
                if (pnew && pnew->kind == SELEMK_Ref && pnew->ref < agroup.size())
                {
                    // orig: 0x00217EF8 (the first unused entry), 0x00217CA8 (its part: the selected one, else the
                    // first with keys)
                    SGROUPRT& g = agroup[pnew->ref];
                    size_t kFree = 0;   // (0x217EF8: the first unused entry, else entry 0 again)
                    while (kFree < g.aentry.size() && g.aentry[kFree].fUsed) ++kFree;
                    if (kFree == g.aentry.size()) kFree = 0;
                    if (kFree < g.aentry.size())
                    {
                        const size_t k = kFree;
                        float tStart = 0.0f;
                        const int ipart = IpartFirstForNested(pnew->ref, &tStart);
                        if ([] { static const bool f = std::getenv("SLY2_NESTDBG") != nullptr; return f; }())
                            std::printf("nest: f %lld oid 0x%X group %d entry %d part %d key %d slot %u -> group %u entry %zu part %d t %g\n", (long long)g_cframeScreen, pso->oid, igroup, ientry,
                                        e.ipart, ikey, iitem, pnew->ref, k, ipart, tStart);
                        StartEntry(pnew->ref, int(k), ipart);
                        agroup[pnew->ref].aentry[k].t = tStart;
                        y.ientrySub = int(k);
                    }
                }
            }
        }
        y.fInterp = rec.b30;
        y.tCached = key.t;   // (0x219400: item +0x5C = the key's time)
        y.idItem = rec.n34;
        y.b61 = rec.b31;
        if (dLink != 0 && iitem + dLink < e.aslot.size())
        {
            y.mask = 1;
            e.aslot[iitem + dLink].mask = 4;
        }
        else if (y.mask != 4) y.mask = 0;
    }
    (void)v; (void)pc0; (void)pc1;
}

// the slots' transforms and colours at the entry's time (0x219FE0's item loop), and the nested entries
void CScreenPlayer::PlaceSlots(int igroup, int ientry, const std::array<float, 6>& v, uint32_t pc0, uint32_t pc1, float dt)
{
    SENTRYRT& e = agroup[size_t(igroup)].aentry[size_t(ientry)];
    for (SSLOT& y : e.aslot)
    {
        if (!y.pit0) continue;
        std::array<float, 6> m = MatOfItem(*y.pit0);
        uint32_t ic0 = y.kc0, ic1 = y.kc1;
        if (y.fInterp && y.pit1 && y.t1 > y.t0)
        {
            const float u = std::clamp((e.t - y.t0) / (y.t1 - y.t0), 0.0f, 1.0f);
            const std::array<float, 6> m1 = MatOfItem(*y.pit1);
            for (int j = 0; j < 6; ++j) m[size_t(j)] = (1.0f - u) * m[size_t(j)] + u * m1[size_t(j)];
            ic0 = LerpRgba(y.kc0, y.nc0, u);
            ic1 = LerpRgba(y.kc1, y.nc1, u);
        }
        y.m = Mul(m, v);
        CombineColors(pc0, pc1, ic0, ic1, y.c0, y.c1);
        if (y.pelem && y.pelem->kind == SELEMK_Ref && y.ientrySub >= 0)
            Advance(y.pelem->ref, y.ientrySub, y.m, y.c0, y.c1, dt);
    }
}

// orig: 0x002183E8 (the entry's time: dt more, back to 0 at the part's length) + 0x00219FE0 (the keys crossed)
void CScreenPlayer::Advance(int igroup, int ientry, const std::array<float, 6>& v, uint32_t pc0, uint32_t pc1, float dt)
{
    if (igroup < 0 || size_t(igroup) >= agroup.size()) return;
    SENTRYRT& e = agroup[size_t(igroup)].aentry[size_t(ientry)];
    if (e.ipart < 0) return;
    const SPART& part = pso->agroup[size_t(igroup)].apart[size_t(e.ipart)];
    // the group's transform (0x2183E8: the entry's 2x3 times it)
    std::array<float, 6> vg = v;
    if (agroup[size_t(igroup)].fMat) vg = Mul(v, agroup[size_t(igroup)].mat);
    const std::array<float, 6>& vv = vg;
    // a held entry is drawn at its time (0x2188B0), its nested entries still playing
    if (e.fTrig)
    {
        PlaceSlots(igroup, ientry, vv, pc0, pc1, dt);
        return;
    }
    // orig: 0x00217D98 -> 0x00219FE0 places the supplied startup time before ordinary update looping.
    // A zero-time placement must cross its pending initial keys, including a hold at a remembered end time.
    const bool fInitialPlacement = dt == 0.0f && e.ikey == 0 && !part.akey.empty() && part.akey.front().t <= e.t;
    if (!fInitialPlacement)
    {
        // orig: 0x002183E8 (an already materialized part starts over at its length)
        if (part.dt <= e.t) { e.ikey = 0; e.t = 0.0f; }
        else e.t += dt;
    }
    // orig: 0x00219FE0: at phase 0 (a start, or the part starting over) the first pass after key 0 releases every
    // placed item key 0 did not place (its +0x5C time differs): the previous loop's pieces go.
    float tProcessed = e.t;
    if (e.ikey == 0)
    {
        if (!part.akey.empty() && part.akey.front().t <= e.t)
        {
            tProcessed = part.akey.front().t;
            CrossKey(igroup, ientry, 0, vv, pc0, pc1);
            ++e.ikey;
        }
        for (SSLOT& y : e.aslot)
            if (y.pelem && y.tCached != tProcessed) ReleaseSlot(y);
    }
    while (e.ikey < int(part.akey.size()) && part.akey[size_t(e.ikey)].t <= e.t)
    {
        tProcessed = part.akey[size_t(e.ikey)].t;
        CrossKey(igroup, ientry, e.ikey, vv, pc0, pc1);
        ++e.ikey;
    }
    PlaceSlots(igroup, ientry, vv, pc0, pc1, dt);
    for (SSLOT& y : e.aslot)
        if (y.pelem) y.tCached = tProcessed;   // (0x219FE0: the drawn items' +0x5C)
}

// orig: 0x00217CA8 with 0x314 (a nested group starting): the part selected for it, else where it stopped this
// frame (at its time), else its part 0 from the start
int CScreenPlayer::IpartFirstForNested(int igroup, float* pt)
{
    SGROUPRT& g = agroup[size_t(igroup)];
    *pt = 0.0f;
    if (g.ipartSel >= 0)
    {
        const int i = g.ipartSel;
        g.ipartSel = -1;
        return i;
    }
    if (g.frameStopped == g_cframeScreen)
    {
        *pt = g.tStopped;
        return g.ipartStopped;
    }
    return 0;
}

int64_t g_cframeScreen = 0;
float g_zScreenBase = 0.0f;

bool CScreenPlayer::FFindGroupPart(int idPart, int& igroup, int& ipart) const
{
    if (idPart == -1 || !pso) return false;
    for (size_t g = 0; g < pso->agroup.size(); ++g)
        for (size_t i = 0; i < pso->agroup[g].apart.size(); ++i)
            if (pso->agroup[g].apart[i].id == idPart)
            {
                igroup = int(g);
                ipart = int(i);
                return true;
            }
    return false;
}

void CScreenPlayer::StartRootPart(int idPart)
{
    if (agroup.empty() || agroup[0].aentry.empty()) return;
    int i = 0;
    if (idPart != 0x314)
    {
        const SGROUP& g = pso->agroup[0];
        for (i = 0; i < int(g.apart.size()); ++i)
            if (g.apart[size_t(i)].id == idPart) break;
        if (i >= int(g.apart.size())) i = 0;
    }
    FStartPartIndex(i);
}

void CScreenPlayer::StopRoot()
{
    if (agroup.empty() || agroup[0].aentry.empty()) return;
    SENTRYRT& e = agroup[0].aentry[0];
    if (e.ipart >= 0) StopEntrySound(e);   // (0x218660)
    for (SSLOT& s : e.aslot) ReleaseSlot(s);
    e.ipart = -1;
    e.fTrig = false;
}

bool CScreenPlayer::FGroupBusy(int idPart) const
{
    if (idPart == -1 || agroup.empty() || agroup[0].aentry.empty()) return false;
    if (agroup[0].aentry[0].ipart < 0) return false;   // (+0x7C -1: nothing shown)
    if (idPart == 0x314) return !agroup[0].aentry[0].fTrig;
    int g, i;
    if (!FFindGroupPart(idPart, g, i)) return false;
    // orig: 0x00218710 (the group's entry 0)
    const SENTRYRT& e = agroup[size_t(g)].aentry[0];
    return e.ipart != 0 && !e.fTrig;
}

// the slot whose kind-4 piece plays (igroup, ientrySub), searched from entry (igroupIn, ientryIn) (0x213E20)
SSLOT* CScreenPlayer::PslotLinking(int igroup, int ientrySub, int igroupIn, int ientryIn)
{
    SENTRYRT& e = agroup[size_t(igroupIn)].aentry[size_t(ientryIn)];
    for (SSLOT& y : e.aslot)
    {
        if (!y.pelem || y.pelem->kind != SELEMK_Ref || y.ientrySub < 0) continue;
        if (y.pelem->ref == igroup && (ientrySub < 0 || y.ientrySub == ientrySub)) return &y;
        if (y.pelem->ref < agroup.size())
            if (SSLOT* p = PslotLinking(igroup, ientrySub, y.pelem->ref, y.ientrySub)) return p;
    }
    return nullptr;
}

// orig: 0x00218060 (entry i of a group held at time t of its part: its slots released, the keys to 0 then to t
// crossed with the linking slot's transform and colours)
void CScreenPlayer::SeekEntry(int igroup, int ientry, float t)
{
    SENTRYRT& e = agroup[size_t(igroup)].aentry[size_t(ientry)];
    if (e.ipart < 0) return;
    const SPART& part = pso->agroup[size_t(igroup)].apart[size_t(e.ipart)];
    const float tc = std::min(part.dt, std::max(0.0f, t));
    std::array<float, 6> v = mRoot;
    uint32_t c0 = 0x7F7F7F7F, c1 = 0xFFFFFFFF;
    if (igroup != 0)
    {
        const SSLOT* py = PslotLinking(igroup, ientry, 0, 0);
        if (!py) return;
        v = py->m;
        c0 = py->c0;
        c1 = py->c1;
    }
    for (SSLOT& s : e.aslot) ReleaseSlot(s);
    e.t = 0.0f;
    e.ikey = 0;
    e.fTrig = false;
    Advance(igroup, ientry, v, c0, c1, 0.0f);
    e.fTrig = false;
    Advance(igroup, ientry, v, c0, c1, tc);
    e.fTrig = true;
}

void CScreenPlayer::SetPartFraction(int idPart, float g)
{
    if (!pso) return;
    const float u = std::min(1.0f, std::max(0.0f, g));
    if (idPart == 0x314)
    {
        // orig: 0x00217FB8 (the first group's entry 0)
        if (!agroup.empty() && !agroup[0].aentry.empty() && agroup[0].aentry[0].ipart >= 0)
            SeekEntry(0, 0, pso->agroup[0].apart[size_t(agroup[0].aentry[0].ipart)].dt * u);
        return;
    }
    for (size_t ig = 0; ig < pso->agroup.size(); ++ig)
    {
        bool fHas = false;
        for (const SPART& part : pso->agroup[ig].apart) fHas |= part.id == idPart;
        if (!fHas) continue;
        int l = -1;
        for (size_t m = 0; m < agroup[ig].aentry.size(); ++m)
            if (agroup[ig].aentry[m].ipart != -1) l = int(m);
        if (l < 0) continue;
        SENTRYRT& e = agroup[ig].aentry[size_t(l)];
        SeekEntry(int(ig), l, pso->agroup[ig].apart[size_t(e.ipart)].dt * u);
    }
}

// orig: 0x00212F80 -> 0x00218038 / 0x00218060: as SetPartFraction (0x212E40) but the part's time itself (seconds,
// clamped to the part's length by 0x218060), e.g. the Ep7 power chop meter's 983 (s34, its 1.5 s cycle)
void CScreenPlayer::SetPartTime(int idPart, float t)
{
    if (!pso) return;
    if (idPart == 0x314)
    {
        if (!agroup.empty() && !agroup[0].aentry.empty() && agroup[0].aentry[0].ipart >= 0) SeekEntry(0, 0, t);
        return;
    }
    for (size_t ig = 0; ig < pso->agroup.size(); ++ig)
    {
        bool fHas = false;
        for (const SPART& part : pso->agroup[ig].apart) fHas |= part.id == idPart;
        if (!fHas) continue;
        int l = -1;
        for (size_t m = 0; m < agroup[ig].aentry.size(); ++m)
            if (agroup[ig].aentry[m].ipart != -1) l = int(m);
        if (l < 0) continue;
        SeekEntry(int(ig), l, t);
    }
}

void CScreenPlayer::SelectPart(int idPart, bool fReset)
{
    int g, l;
    if (!FFindGroupPart(idPart, g, l)) return;
    if (fReset) l = 0;
    bool fPlaying = false;
    for (const SENTRYRT& e : agroup[size_t(g)].aentry) fPlaying |= e.ipart != -1;
    if (!fPlaying || g == 0)
    {
        agroup[size_t(g)].ipartSel = fReset ? -1 : l;
        return;
    }
    // orig: 0x00213F70 (the linking slot's entry stopped, a new one started on part l at the slot's transform)
    if (agroup.empty() || agroup[0].aentry.empty()) return;
    SSLOT* py = PslotLinking(g, -1, 0, 0);
    if (!py) return;
    SENTRYRT& eOld = agroup[size_t(g)].aentry[size_t(py->ientrySub)];
    if (eOld.ipart >= 0) StopEntrySound(eOld);   // (0x218660)
    for (SSLOT& s : eOld.aslot) ReleaseSlot(s);
    eOld.ipart = -1;
    eOld.fUsed = false;
    for (size_t k = 0; k < agroup[size_t(g)].aentry.size(); ++k)
        if (!agroup[size_t(g)].aentry[k].fUsed)
        {
            StartEntry(g, int(k), l);
            py->ientrySub = int(k);
            Advance(g, int(k), py->m, py->c0, py->c1, 0.0f);
            break;
        }
}

// orig: 0x00214100 (the slot linking group igroup with identity match, searched from (igroupIn, ientryIn)'s slots:
// its entry restarted on part ipart at the slot's transform)
bool CScreenPlayer::FRestartAt(int igroup, int ipart, int match, int igroupIn, int ientryIn)
{
    SENTRYRT& e = agroup[size_t(igroupIn)].aentry[size_t(ientryIn)];
    for (SSLOT& y : e.aslot)
    {
        if (!y.pelem || y.pelem->kind != SELEMK_Ref || y.ientrySub < 0 || y.pelem->ref >= agroup.size()) continue;
        if (y.pelem->ref == igroup)
        {
            if (y.idItem != match) continue;
            SGROUPRT& g = agroup[size_t(igroup)];
            SENTRYRT& eOld = g.aentry[size_t(y.ientrySub)];
            if (eOld.ipart >= 0) { g.ipartStopped = eOld.ipart; g.tStopped = eOld.t; g.frameStopped = g_cframeScreen; g.ipartSel = -1; StopEntrySound(eOld); }
            for (SSLOT& sl : eOld.aslot) ReleaseSlot(sl);
            eOld.ipart = -1;
            eOld.fUsed = false;
            for (size_t k = 0; k < g.aentry.size(); ++k)
                if (!g.aentry[k].fUsed)
                {
                    StartEntry(igroup, int(k), ipart);
                    y.ientrySub = int(k);
                    Advance(igroup, int(k), y.m, y.c0, y.c1, 0.0f);
                    break;
                }
            return true;
        }
        if (FRestartAt(igroup, ipart, match, y.pelem->ref, y.ientrySub)) return true;
    }
    return false;
}

void CScreenPlayer::SelectPartAt(int idPart, int match)
{
    int g, l;
    if (agroup.empty() || agroup[0].aentry.empty() || !FFindGroupPart(idPart, g, l)) return;
    FRestartAt(g, l, match, 0, 0);
}

void CScreenPlayer::ResetPartAt(int idPart, int match)
{
    if (agroup.empty() || agroup[0].aentry.empty() || !pso) return;
    for (size_t ig = 0; ig < pso->agroup.size(); ++ig)
        for (const SPART& part : pso->agroup[ig].apart)
            if (part.id == idPart)
            {
                FRestartAt(int(ig), 0, match, 0, 0);
                break;
            }
}

void CScreenPlayer::SetPartMatrix(int idPart, const std::array<float, 6>& m)
{
    if (!pso) return;
    for (size_t ig = 0; ig < pso->agroup.size(); ++ig)
        for (const SPART& part : pso->agroup[ig].apart)
            if (part.id == idPart)
            {
                agroup[ig].fMat = true;
                agroup[ig].mat = m;
                break;
            }
}

void CScreenPlayer::Update(float dt)
{
    if (agroup.empty() || agroup[0].aentry.empty()) return;
    Advance(0, 0, mRoot, 0x7F7F7F7F, 0xFFFFFFFF, dt);
}

void CScreenPlayer::Collect(int igroup, int ientry, std::vector<LEAF>& aleaf) const
{
    const SENTRYRT& e = agroup[size_t(igroup)].aentry[size_t(ientry)];
    for (size_t is = 0; is < e.aslot.size(); ++is)
    {
        const SSLOT& y = e.aslot[is];
        if (!y.pelem) { if (y.mask == 4) aleaf.push_back({&y, igroup, ientry, int(is)}); continue; }
        if (y.pelem->kind == SELEMK_Ref)
        {
            if (y.ientrySub >= 0 && y.pelem->ref < agroup.size()) Collect(y.pelem->ref, y.ientrySub, aleaf);
            continue;
        }
        aleaf.push_back({&y, igroup, ientry, int(is)});
    }
}

namespace
{
// orig: 0x002174C8 (the slot's 2x3 to the screen matrix) and 0x00213300's colours ((c - 127), (c - 127) / 127)
HUDSTATE StateOfSlot(const std::array<float, 6>& v, uint32_t c0, uint32_t c1, float z, bool fMask, glm::vec2 posOrigin)
{
    HUDSTATE st;
    st.m[0] = v[0] * UI_KX;
    st.m[1] = v[1] * UI_KY;
    st.m[4] = v[2] * UI_KX;
    st.m[5] = v[3] * UI_KY;
    st.m[8] = v[4] * UI_KX + posOrigin.x + GS_ORIGIN;
    st.m[9] = v[5] * UI_KY + posOrigin.y + GS_ORIGIN;
    st.depth = z;
    for (int i = 0; i < 4; ++i)
    {
        st.offset[i] = fMask ? 0.0f : float(Ch(c0, i)) - 127.0f;
        st.scale[i] = fMask ? 0.0f : std::clamp((float(Ch(c1, i)) - 127.0f) / 127.0f, -1.0f, 1.0f);
    }
    return st;
}
}  // namespace

// orig: 0x00215ED8's measure (the reader's text width at its scale, '&x' escapes switching to the icon font)
float DxTextUi(const FONTBRX& fontBase, const FONTBRX* pfontOther, const std::string& text, float k88)
{
    float w = 0.0f;
    const FONTBRX* pfont = &fontBase;
    for (size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '&' && i + 1 < text.size() && text[i + 1] != '&')
        {
            pfont = text[i + 1] == '.' || !pfontOther ? &fontBase : pfontOther;
            ++i;
            continue;
        }
        if (text[i] == '&') ++i;
        if (i < text.size() && text[i] != '\n')
        {
            std::size_t cb = 1;
            w += pfont->DxFromCh(ChDecode(text, i, &cb)) * k88;
            i += cb - 1;
        }
    }
    return w;
}
bool FHudPromptTicker(float* pt, float* pgEnd);   // hudwidget.cpp (0x3DA0C0 in state 2: its time, its +0x1C)
void EndHudPromptTicker();                         // (0x1DA5C8(0x3DA0C0, 3))

void CScreenPlayer::Draw(const FONTBRX* pfont, const FONTBRX* pfontIcons, const SW* psw)
{
    if (agroup.empty() || agroup[0].aentry.empty()) return;
    std::vector<LEAF> aleaf;
    Collect(0, 0, aleaf);
    // (QoL: the pause Controls page for the device in use, controlspage.h: the pieces with their states first)
    bool fCtlPage = false;
    if (pso && pso->oid == 0x370)
    {
        std::vector<CTLPIECE> apiece;
        float zc = 16.0f + g_zScreenBase;
        for (const LEAF& leaf : aleaf)
        {
            SSLOT y = *leaf.pslot;
            y.m[4] += dxOwner;
            y.m[5] += dyOwner;
            if (y.pelem && y.mask != 1)
            {
                if (y.mask == 4) zc += 1024.0f;
                CTLPIECE piece;
                piece.igroup = leaf.igroup;
                piece.pelem = y.pelem;
                piece.st = StateOfSlot(y.m, y.c0, y.c1, zc, false, OriginOfScreen(pso));
                piece.mx = y.m[4];
                piece.my = y.m[5];
                apiece.push_back(piece);
            }
            zc += 1024.0f;
        }
        fCtlPage = FControlsPageBegin(pso, apiece, pfont, pfontIcons, psw);
    }
    auto drawPiece = [&](const SSLOT& y, const HUDSTATE& st) {
        const SELEM& el = *y.pelem;
        switch (el.kind)
        {
        case SELEMK_Sprite: g_hud.DrawSprite(el, st); break;
        case SELEMK_Quad: g_hud.DrawQuad(el, st); break;
        case SELEMK_Image:
            if (y.prtgtOwner) g_hud.DrawRtgtQuad(el, st, GlTextureOfRtgt(IrtgtOfOwner(y.prtgtOwner)));
            break;
        case SELEMK_Sound:   // a text box (0x216BC8): a localized string (+0x14 0) or a script value (1)
        {
            if (!pfont) break;
            std::string text;
            bool fWrap = true;
            if (el.ab3[1] == 1 && el.n3 == 0x2EF)
            {
                // the subtitle (0x216BC8's 0x2EF): the line's whole text wrapped (0x215F40), typed by the line's time
                // (0x227820), then only its last lines that fit the box (height +0x28 / size +0x20) kept
                if (!FDialogSubtitleText(&text)) break;
                text = StrDialogSubtitleTyped(StrWrapped(*pfont, pfontIcons, text, el.ag3[0] / 51.2f, el.ag3[1]));
                int cline = 1;
                for (char ch : text) cline += ch == '\n';
                const int clineMax = el.ag3[0] != 0.0f ? static_cast<int>(el.ag3[2] / el.ag3[0]) : cline;
                size_t i = 0;
                while (clineMax < cline && i < text.size())
                    if (text[i++] == '\n') --cline;
                text = text.substr(i);
                fWrap = false;
            }
            else
            {
                text = el.ab3[1] == 1 && g_pfnScriptString ? g_pfnScriptString(el.n3) : StrLocalized(psw, el.n3);
                // (QoL N39: the words START / SELECT as the device's icons, promptglyph.h)
                if (pfontIcons) {
                    const int subtitle = el.ab3[1] == 1 && el.n3 == 0x2EF ? IdTextUsa(g_dialogsub.id) : -1;
                    text = StrPromptButtonWords(text, subtitle);
                }
            }
            const bool scriptText = el.ab3[1] == 1;
            int promptId = el.n3;
            if (scriptText) {
                if (el.n3 == 0x2F1) promptId = IdDisplayedHudPrompt();
                else if (el.n3 == 0x2EF) promptId = g_dialogsub.id;
                else promptId = -1;
            }
            const PROMPTTEXTGLYPHS promptContext(IdTextUsa(promptId));
            std::optional<PROMPTKEYGLYPH> menuBinding;
            int binding;
            bool controller;
            if (pfontIcons && scriptText && FMenuActionPrompt(el.n3, binding, controller)) {
                using cane::input::Family;
                Family family = controller ? PromptFamily() : Family::Keyboard;
                if (controller && family == Family::Keyboard) family = Family::PlayStation;
                if (PromptGlyphForBinding(binding, family).kind != PROMPTGLYPH::Original) {
                    text += "  &2T&.";
                    menuBinding.emplace('T', binding, binding, family);
                }
            }
            if (std::getenv("SLY2N_TEXTPRINT"))
                std::fprintf(stderr, "textelem kind %d id 0x%X: '%s'\n", int(el.ab3[1]), unsigned(el.n3), text.c_str());
            if (text.empty()) break;
            // (the subtitle 0x2EF in its speaker's colour: font_brx's 0x4435F4)
            const uint32_t base = el.ab3[1] == 1 && el.n3 == 0x2EF && g_dialogsub.id != -1 ? g_dialogsub.rgba : el.aul3[1];
            glm::vec4 off, sc;
            for (int i = 0; i < 4; ++i)
            {
                off[i] = std::min(st.offset[i] + (i < 3 ? float(Ch(base, i)) : 0.0f), 255.0f);
                sc[i] = st.scale[i];
            }
            sc.a = std::clamp(st.scale.a * float(Ch(base, 3)) / 127.0f, 0.0f, 1.0f);
            off.a = st.offset.a;
            // orig: 0x00215CA8 / 0x00215D40: '&x' switches to the icon font with the reader's colour c2, which
            // 0x215CA8 sets to black with the text's alpha (0x80000000 | a): the icons get the offset without the
            // text's colour (their own vertex colours, not tinted), the same alpha scale (U29)
            glm::vec4 offIcon = off;
            for (int i = 0; i < 3; ++i) offIcon[i] = std::min(st.offset[i], 255.0f);
            // the slot's 2x3 as the box: origin (v4, v5), scales v0 / v3
            const float sx = y.m[0], sy = y.m[3];
            float x = y.m[4];
            // the prompt 0x2F1 while shown (0x3DA0C0 state 2) scrolls as a ticker: "     text     text     " fading in
            // over 0.25 s, then moving left by one text width per (its width x 0.01) s; a timed prompt ends once the
            // text has crossed the box (0x1DA5C8(3))
            float tTicker = 0.0f, gEnd = -1.0f;
            float wBox = el.ag3[1];
            if (el.ab3[1] == 1 && el.n3 == 0x2F1 && FHudPromptTicker(&tTicker, &gEnd))
            {
                wBox = 0.0f;   // (the ticker's text is not wrapped: 0x216BC8 skips 0x215F40 there)
                const float k88 = el.ag3[0] / 51.2f;
                const std::string pad = "     ";
                const float dxPad = DxTextUi(*pfont, pfontIcons, pad, k88) * sx;
                const float dxText = DxTextUi(*pfont, pfontIcons, pad + text, k88) * sx;
                text = pad + text + pad + text + pad;
                const float tScroll = 0.25f;
                if (tTicker < tScroll) sc.a *= tTicker / tScroll;
                else if (dxText > 0.0f)
                {
                    const float t = tTicker - tScroll;
                    const float period = dxText * 0.01f;
                    const float u = std::fmod(t, period);
                    if (0.0f <= gEnd)
                    {
                        float g = std::max(0.0f, dxText - el.ag3[1] * sx);
                        g = std::max(gEnd, (dxText + g + dxPad) * 0.01f);
                        if (g <= t) EndHudPromptTicker();
                    }
                    x -= u / period * dxText;
                }
            }
            // orig: 0x00215BA8 / 0x002162D8: text uses the owning screen's canvas origin,
            // just like its sprites. The default frame origin is different from a 640x480 screen.
            const glm::vec2 posTextOrigin = (OriginOfScreen(pso) - glm::vec2(2048.0f)) / glm::vec2(UI_KX, UI_KY);
            const float xText = x + posTextOrigin.x, yText = y.m[5] + posTextOrigin.y;
            NoteJobHelpText(el.ab3[1], el.n3, xText, yText, el.ag3[0]);   // (PC menus: Job Help's rows for the mouse, shop.h)
            if (fCtlPage)   // (QoL: the pause Controls page for the device in use, controlspage.h)
            {
                TEXTDRAW td;
                td.pfont = pfont;
                td.pfontIcons = pfontIcons;
                td.kind = el.ab3[1];
                td.id = el.n3;
                td.x = xText;
                td.y = yText;
                td.size = el.ag3[0];
                td.sx = sx;
                td.sy = sy;
                td.wBox = wBox;
                td.hBox = el.ag3[2];
                td.halign = el.ab3[0];
                td.off = off;
                td.sc = sc;
                td.offIcon = offIcon;
                td.depth = st.depth;
                if (FControlsPageText(td, text)) break;
            }
            if (g_pfnTextDrawHook)   // (QoL overlays: replay.cpp)
            {
                TEXTDRAW td;
                td.pfont = pfont;
                td.pfontIcons = pfontIcons;
                td.kind = el.ab3[1];
                td.id = el.n3;
                td.x = xText;
                td.y = yText;
                td.size = el.ag3[0];
                td.sx = sx;
                td.sy = sy;
                td.wBox = wBox;
                td.hBox = el.ag3[2];
                td.halign = el.ab3[0];
                td.off = off;
                td.sc = sc;
                td.offIcon = offIcon;
                td.depth = st.depth;
                td.ptext = &text;
                if (g_pfnTextDrawHook(td)) break;
            }
            g_hud.DrawTextBox(*pfont, pfontIcons, text, xText, yText, el.ag3[0], sx, sy, wBox, el.ag3[2], el.ab3[0],
                              0, off, sc, offIcon, st.depth, fWrap);
            break;
        }
        default: break;
        }
    };
    if ([] { static const bool f = std::getenv("SLY2_SLOTDBG") != nullptr; return f; }())
        for (const LEAF& leaf : aleaf)
            std::printf("g %d e %d s %d (part %d t %.2f) slot kind %u idx %u m %.2f %.2f %.2f %.2f %.1f %.1f c0 %08X c1 %08X mask %d\n", leaf.igroup, leaf.ientry, leaf.islot, agroup[size_t(leaf.igroup)].aentry[size_t(leaf.ientry)].ipart,
                        agroup[size_t(leaf.igroup)].aentry[size_t(leaf.ientry)].t, leaf.pslot->pelem ? leaf.pslot->pelem->kind : 0,
                        leaf.pslot->pelem ? leaf.pslot->pelem->idx : 0, leaf.pslot->m[0], leaf.pslot->m[1], leaf.pslot->m[2], leaf.pslot->m[3],
                        leaf.pslot->m[4], leaf.pslot->m[5], leaf.pslot->c0, leaf.pslot->c1, leaf.pslot->mask);
    // (the screen object's priority +0xA4 << 18 above its slots' depths: 0x212C50 queues each object at its depth,
    // so a higher-priority object draws over a lower one whatever their slots' counts)
    float z = 16.0f + g_zScreenBase;
    const SSLOT* pmask = nullptr;
    SSLOT ymask;
    for (const LEAF& leaf : aleaf)
    {
        // (the owner's translation over the slot's)
        SSLOT ymoved;
        const SSLOT* pslot = leaf.pslot;
        if (dxOwner != 0.0f || dyOwner != 0.0f)
        {
            ymoved = *pslot;
            ymoved.m[4] += dxOwner;
            ymoved.m[5] += dyOwner;
            pslot = &ymoved;
        }
        const SSLOT& y = *pslot;
        if (y.mask == 1 && y.pelem)
        {
            ymask = y;
            pmask = &ymask;
            g_hud.SetMode(1);
            drawPiece(y, StateOfSlot(y.m, y.c0, y.c1, z, true, OriginOfScreen(pso)));
            g_hud.SetMode(3);
        }
        else if (y.mask == 4)
        {
            if (pmask)
            {
                g_hud.SetMode(2);
                drawPiece(*pmask, StateOfSlot(pmask->m, pmask->c0, pmask->c1, z, true, OriginOfScreen(pso)));
                g_hud.SetMode(4);
                pmask = nullptr;
            }
            if (y.pelem) { z += 1024.0f; drawPiece(y, StateOfSlot(y.m, y.c0, y.c1, z, false, OriginOfScreen(pso))); }
        }
        else if (y.pelem)
        {
            const HUDSTATE st = StateOfSlot(y.m, y.c0, y.c1, z, false, OriginOfScreen(pso));
            CTLPIECE piece;
            if (fCtlPage)
            {
                piece.igroup = leaf.igroup;
                piece.pelem = y.pelem;
                piece.st = st;
                piece.mx = y.m[4];
                piece.my = y.m[5];
            }
            if (!fCtlPage || !FControlsPageSkip(piece)) drawPiece(y, st);
        }
        z += 1024.0f;
    }
    if (fCtlPage) ControlsPageEnd(pfont, pfontIcons);
    g_hud.SetMode(0);
}
