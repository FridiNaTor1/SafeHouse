// duel3.cpp: the Biplane Duel's free-for-all for three or four players (duel3.h; notes/PLANE3_2026-11-12.md "Round 3").
#include "duel3.h"

#include "actor.h"
#include "activate.h"
#include "animfind.h"
#include "challenge3.h"
#include "clock.h"
#include "cmworld.h"
#include "render.h"
#include "epmenu3.h"
#include "guard.h"
#include "hud.h"
#include "hudwidget.h"
#include "lo.h"
#include "porttext.h"
#include "screenrt.h"
#include "screenobj.h"
#include "sidebag.h"
#include "sm.h"
#include "splitscreen3.h"
#include "sw.h"
#include "update.h"
#include "gauge3.h"
#include "game/biplane3.h"
#include "game/sly3.h"
#include "game/task3.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace duel3
{
namespace
{
constexpr int kWorld = 21;            // h_dogfight
constexpr OID kJob = 3323;            // m3_biplane_multi (row 2026, task 1814)
constexpr OID kNode = 2026;           // the Two Player list's row object
constexpr OID kPlane = 95691;         // the player plane (three copies in the world: proxies 93166, 94392, 94393)
constexpr OID kProxy1 = 94392, kProxy2 = 94393;
// the duel splice's spawn points (h_dogfight block 93165 module 133: s2176 = 94376..94387; the checkpoint 94610 puts
// players 1 / 2 at 94376 / 94382; s4961 respawns a player at the one farthest from the other player)
constexpr OID kSpawnFirst = 94376, kSpawnLast = 94387;
constexpr int kTarget = 10;           // (the splice's s3387: 10, raised by one at a tie at the top)
constexpr float kRespawn = 2.25f;     // (the original's: the fade 3326 / 3327's phases, 2.27 s natively measured)
constexpr float kShooterWindow = 6.0f;   // (a death within this of a plane's hit is that plane's kill)
constexpr float kMsg = 3.0f, kWinHold = 5.0f;
// the original 2P HUD 3325 (h_dogfight, also v_ext / h_hack: the same object): each band's pieces (elements, by the
// draws' states): 6 the view box's frame (white, coloured by its slot: band 0 blue 0 / 102 / 152, band 1 orange
// 152 / 50 / 0), 8 Sly's portrait and 4 his hat behind it (faded) in band 0, 7 Carmelita's portrait and 3 her badge in
// band 1. Players 3 / 4 (Bentley's green / Murray's purple in the kills row) get their characters' own art: the gang's
// HUD gauges' icons (Bentley's 6575, Murray's 6094, element 11: the character's face of the 1P HUD, loaded in h_dogfight)
constexpr OID kHud2p = 3325;
constexpr int kElemFrame = 6, kElemSly = 8, kElemSlyBack = 4, kElemCarm = 7, kElemCarmBack = 3;
constexpr OID kGaugeBentley = 6575, kGaugeMurray = 6094;
constexpr int kElemGaugeFace = 11;

struct PLAYER
{
    LO* plo = nullptr;
    int kills = 0;
    bool fDead = false;
    float tDead = 0.0f;
    int pilot = 24;   // (its plane's 1823: 24 Sly's body, 28 Carmelita's)
};
struct ROUND
{
    bool fActive = false;
    bool fEnded = false;
    SW* psw = nullptr;
    int cplayer = 0;
    std::array<PLAYER, 4> ap;
    int target = kTarget;
    int winner = -1;
    float tWin = 0.0f;
    std::string msg;
    int iMsg = -1;   // (the player whose colour the message takes)
    float tMsg = -100.0f;
    int leader = -1;
};
ROUND r;
int s_cChosen = 0;   // (0: not read yet)

bool FPrint() { static const bool s_f = std::getenv("SLY3_PLANEPRINT") || std::getenv("SLY3_CHALPRINT"); return s_f; }

int CChosenRaw()
{
    if (s_cChosen == 0)
    {
        s_cChosen = 2;
        if (const char* sz = std::getenv("SLY3_DUELPLAYERS")) s_cChosen = std::clamp(std::atoi(sz), 2, 4);
    }
    return s_cChosen;
}

bool FRoundWorld(const SW* psw)
{
    return psw && psw->filter == kWorld && FChallengeRunning3() && OidChallengeJob3() == kJob && CChosenRaw() > 2;
}

std::string StrPlayer(int i)
{
    char ach[64];
    std::snprintf(ach, sizeof ach, StrPort(PTX_SplitPlayerN).c_str(), i + 1);
    return ach;
}
std::string Fmt1(PTX ptx, const std::string& a)
{
    char ach[160];
    std::snprintf(ach, sizeof ach, StrPort(ptx).c_str(), a.c_str());
    return ach;
}
std::string Fmt2(PTX ptx, const std::string& a, const std::string& b)
{
    char ach[200];
    std::snprintf(ach, sizeof ach, StrPort(ptx).c_str(), a.c_str(), b.c_str());
    return ach;
}
void Message(const std::string& s, int i)
{
    r.msg = s;
    r.iMsg = i;
    r.tMsg = g_clock.t;
    if (FPrint()) std::printf("duel3: message \"%s\" (t %.2f)\n", s.c_str(), g_clock.t);
}

int IplayerOf(const LO* plo)
{
    for (int i = 0; i < r.cplayer; ++i)
        if (plo && r.ap[size_t(i)].plo == plo) return i;
    return -1;
}

// the world's copies of the player plane: the task's two (players 1 / 2), the rest in their load order
std::vector<LO*> AplaneCopies(SW* psw)
{
    std::vector<LO*> a;
    for (LO* plo : AploOfOid(psw, kPlane))
        if (plo->oid == kPlane && FPlane3(plo)) a.push_back(plo);
    std::stable_sort(a.begin(), a.end(), [](const LO* p, const LO* q) { return p->iclone < q->iclone; });
    return a;
}

LO* PloExtra(SW* psw, int i)   // (i 2 / 3: the copies other than the task's two)
{
    LO* p1 = PloFindSwPlayerByOid3(psw, kProxy1);
    LO* p2 = PloFindSwPlayerByOid3(psw, kProxy2);
    int k = 2;
    for (LO* plo : AplaneCopies(psw))
    {
        if (plo == p1 || plo == p2) continue;
        if (k == i) return plo;
        ++k;
    }
    return nullptr;
}

std::vector<LO*> AploSpawns(SW* psw)
{
    std::vector<LO*> a;
    for (OID oid = kSpawnFirst; oid <= kSpawnLast; ++oid)
        if (LO* p = PloFindSwObjectByOid(psw, oid)) a.push_back(p);
    return a;
}

// the spawn point farthest from the other players' planes (its nearest one), as the splice's s4961 for one other
LO* PloSpawnFarthest(SW* psw, int iExcept)
{
    LO* pbest = nullptr;
    float best = -1.0f;
    for (LO* ps : AploSpawns(psw))
    {
        const glm::vec3 pos = PosLoWorld(ps);
        float dmin = 1e30f;
        for (int j = 0; j < r.cplayer; ++j)
        {
            const PLAYER& o = r.ap[size_t(j)];
            if (j == iExcept || !o.plo || o.fDead || !o.plo->fInWorld) continue;
            dmin = std::min(dmin, glm::length(PosLoWorld(o.plo) - pos));
        }
        if (dmin > best)
        {
            best = dmin;
            pbest = ps;
        }
    }
    return pbest;
}

void CopyBinding(LO* pfrom, LO* pto, int n)
{
    if (!pfrom || !pto || !pfrom->psidebag) return;
    CRef ref;
    if (!pfrom->psidebag->FFindBinding(n, &ref)) return;
    if (!pto->psidebag) pto->psidebag = PsidebagNew();
    if (pto->psidebag->FFindBinding(n, nullptr)) pto->psidebag->RefSetBinding(n, &ref);
    else pto->psidebag->RefAddBinding(n, &ref);
}

// (the plane's procedure s3453 keeps the body it showed in its module's variable s1361 and takes it away (42) before
// showing the next one: the copies configured one after the other took each other's bodies (player 3's plane flew as a
// bare propeller). Each round plane gets its own pilot's body (24: 95867, 28: 95823) and the pilot 35547 back.)
void EnsurePlaneBody(SW* psw, LO* plo, int pilot)
{
    const OID oidBody = pilot == 28 ? 95823 : 95867, oidOther = pilot == 28 ? 95867 : 95823;
    if (LO* p = PloFindSwChild(plo, oidBody); p && !p->fInWorld) AddLo(psw, p);
    if (LO* p = PloFindSwChild(plo, oidOther); p && p->fInWorld) RemoveLo(psw, p);
    if (LO* p = PloFindSwChild(plo, 35547); p && !p->fInWorld) AddLo(psw, p);
}

void Score(int ikiller, int ivictim)
{
    if (ikiller >= 0)
    {
        r.ap[size_t(ikiller)].kills++;
        Message(Fmt2(PTX_DuelShotDown, StrPlayer(ikiller), StrPlayer(ivictim)), ikiller);
    }
    else Message(Fmt1(PTX_DuelCrashed, StrPlayer(ivictim)), ivictim);
    if (FPrint())
    {
        std::printf("duel3: kills");
        for (int i = 0; i < r.cplayer; ++i) std::printf(" %d", r.ap[size_t(i)].kills);
        std::printf(" (player %d down, by %d)\n", ivictim + 1, ikiller + 1);
    }
    // the leader / the end (the original's s3409: someone at the target, a tie at the top raises it)
    int best = -1, cbest = 0;
    for (int i = 0; i < r.cplayer; ++i)
    {
        const int k = r.ap[size_t(i)].kills;
        if (best < 0 || k > r.ap[size_t(best)].kills) { best = i; cbest = 1; }
        else if (k == r.ap[size_t(best)].kills) ++cbest;
    }
    if (best >= 0 && r.ap[size_t(best)].kills >= r.target)
    {
        if (cbest > 1) r.target++;
        else
        {
            r.winner = best;
            r.tWin = g_clock.t;
            Message(Fmt1(PTX_DuelWins, StrPlayer(best)), best);
            std::printf("duel3: winner player %d (%d kills)\n", best + 1, r.ap[size_t(best)].kills);
            return;
        }
    }
    if (best >= 0 && cbest == 1 && best != r.leader && ikiller == best)
    {
        r.leader = best;
        Message(Fmt1(PTX_DuelLeads, StrPlayer(best)), best);   // (after the kill's line: the lead changed)
    }
}

// ---- drawing (copsmap3.cpp's style)
constexpr float DEPTH = 16760000.0f;
glm::vec4 Rgb(int rr, int g, int b, int a = 128) { return glm::vec4(float(rr), float(g), float(b), float(a)); }
glm::vec4 RgbOf(uint32_t rgba, int a = 128)
{
    return glm::vec4(float(rgba & 0xFF), float((rgba >> 8) & 0xFF), float((rgba >> 16) & 0xFF), float(a));
}
glm::vec4 OffOf(int i)   // (text: the player's colour lifted)
{
    const glm::vec4 c = RgbOf(RgbaPlayer(i));
    return glm::vec4(std::min(128.0f, c.r * 0.6f + 50.0f), std::min(128.0f, c.g * 0.6f + 50.0f), std::min(128.0f, c.b * 0.6f + 50.0f), 0.0f);
}
void Text(const std::string& text, float x, float y, float size, glm::vec4 off, int halign = 1)
{
    const FONTBRX* pfont = PfontHud();
    if (!pfont) return;
    g_hud.DrawTextBox(*pfont, PfontHudIcons(), text, x - 320.0f, (y - 224.0f) * 1.1f, size, 1.0f, 1.0f, 0.0f, 0.0f, halign, 0, off,
                      glm::vec4(1.0f), glm::vec4(0.0f), DEPTH + 100.0f, false);
}
void Rect(float x0, float y0, float x1, float y1, glm::vec4 rgba, float dz = 0.0f)
{
    g_hud.FillScreenRect(x0, y0, x1, y1, rgba, (DEPTH + dz) / 16777216.0f);
}
constexpr float kXAspect = 640.0f / (448.0f * 4.0f / 3.0f);
void Arrow(float cx, float cy, float ang, float s, glm::vec4 rgba, float dz)
{
    const glm::vec2 d(std::cos(ang), std::sin(ang)), n(-d.y, d.x);
    auto pt = [&](glm::vec2 v) { return glm::vec2(cx + v.x * kXAspect, cy + v.y); };
    const glm::vec2 a[3] = {pt(d * s), pt(-d * s * 0.6f + n * s * 0.7f), pt(-d * s * 0.6f - n * s * 0.7f)};
    g_hud.FillScreenTris(a, 3, rgba, (DEPTH + dz) / 16777216.0f);
}

// ---- the 2P HUD's bands per player (hud.h g_pfnHudPiece3)
bool FBox(const SELEM& el, glm::vec2* plo, glm::vec2* phi)
{
    glm::vec2 lo(1e9f), hi(-1e9f);
    for (const SPOLY& p : el.apoly)
        for (const auto& xy : p.axy)
        {
            const glm::vec2 v(float(xy[0]) / 16.0f, float(xy[1]) / 16.0f);
            lo = glm::min(lo, v), hi = glm::max(hi, v);
        }
    *plo = lo, *phi = hi;
    return el.kind == SELEMK_Sprite && lo.x < hi.x && lo.y < hi.y;
}

const SELEM* PelemGaugeFace(int iplayer)
{
    if (!r.psw || iplayer < 2) return nullptr;
    LO* plo = PloFindSwObjectByOid(r.psw, iplayer == 2 ? kGaugeBentley : kGaugeMurray);
    if (!plo || plo->Cid() != 0xA3) return nullptr;
    const SCREENOBJ* pso = static_cast<SPRITESET*>(plo)->pso.get();
    if (!pso || pso->aelem.size() <= size_t(kElemGaugeFace)) return nullptr;
    const SELEM& el = pso->aelem[size_t(kElemGaugeFace)];
    glm::vec2 lo, hi;
    return FBox(el, &lo, &hi) ? &el : nullptr;
}

// draws el (its own coordinates) fitted into the box of elTarget as st places it (centred, fill x the box)
void DrawFitted(const SELEM& el, const SELEM& elTarget, const HUDSTATE& st, float fill)
{
    glm::vec2 loT, hiT, loE, hiE;
    if (!FBox(elTarget, &loT, &hiT) || !FBox(el, &loE, &hiE)) return;
    const float k = fill * std::min((hiT.x - loT.x) / (hiE.x - loE.x), (hiT.y - loT.y) / (hiE.y - loE.y));
    const glm::vec2 cT = 0.5f * (loT + hiT), cE = 0.5f * (loE + hiE);
    const glm::vec2 d = cT - k * cE;   // (element -> target coordinates: v * k + d)
    HUDSTATE s = st;
    s.m[0] = st.m[0] * k, s.m[1] = st.m[1] * k, s.m[4] = st.m[4] * k, s.m[5] = st.m[5] * k;
    s.m[8] = st.m[0] * d.x + st.m[4] * d.y + st.m[8];
    s.m[9] = st.m[1] * d.x + st.m[5] * d.y + st.m[9];
    g_hud.DrawSprite(el, s);
}

// (screenrt.h g_pfnScreenObjSkip3) a view's pass: the gauges (class 0xA3) of the other players' planes left out (two
// players share each band's place in the 2 x 2 layout)
bool FScreenObjSkip(LO* plo)
{
    if (!r.fActive || g_iviewHud3 < 0 || g_iviewHud3 >= r.cplayer || !plo || plo->Cid() != 0xA3) return false;
    LO* player = PloGaugePlayer3(plo);
    return player && player != r.ap[size_t(g_iviewHud3)].plo;
}

bool FHudPiece(int oid, int ielem, const SELEM& el, const HUDSTATE& st)
{
    if (oid != kHud2p || !r.fActive || !g_hud.band2p.f || g_iviewHud3 < 0 || g_iviewHud3 >= r.cplayer) return false;
    const int i = g_iviewHud3;
    if (ielem == kElemFrame)
    {
        // the view box's frame in the player's colour (the original's brightness: its strongest channel 152)
        const glm::vec4 c = RgbOf(RgbaPlayer(i));
        const float k = 152.0f / std::max({c.r, c.g, c.b, 1.0f});
        HUDSTATE s = st;
        s.offset = glm::vec4(c.r * k, c.g * k, c.b * k, st.offset.a);
        g_hud.DrawSprite(el, s);
        return true;
    }
    if (i < 2 || (ielem != kElemSly && ielem != kElemSlyBack && ielem != kElemCarm && ielem != kElemCarmBack)) return false;
    // players 3 / 4: their characters' faces in the portrait panel (the face over the panel, a large faded one behind it
    // as the original's hat / badge); without the art the panel stays empty (no team's portrait)
    if (const SELEM* pface = PelemGaugeFace(i))
    {
        const bool fBack = ielem == kElemSlyBack || ielem == kElemCarmBack;
        DrawFitted(*pface, el, st, fBack ? 1.0f : 0.8f);
    }
    return true;
}
}  // namespace

uint32_t RgbaPlayer(int i)
{
    // (the split screen's characters' colours, hackmp3.cpp: Sly, Carmelita, Bentley, Murray)
    static const uint32_t s_argba[4] = {0x80803818u, 0x80107C80u, 0x80207828u, 0x80742C5Cu};
    return s_argba[size_t(std::clamp(i, 0, 3))];
}

int CPlayersChosen() { return CChosenRaw(); }
void StepPlayersChosen(int d) { s_cChosen = std::clamp(CChosenRaw() + d, 2, 4); }
bool FDuelNode(const LO* pnode) { return pnode && pnode->oid == kNode; }

bool FDescription(const LO* pnode, std::string* pstr)
{
    if (!FDuelNode(pnode) || CChosenRaw() <= 2) return false;
    *pstr = StrPort(PTX_DuelDescFFA);
    return true;
}

bool FActive() { return r.fActive; }
int CPlayers() { return r.fActive ? r.cplayer : 0; }

int CExtraCopies(const SW* psw, const std::vector<LO*>& aploGroup)
{
    // (player 4's plane: the world has three copies of 95691; called before the round's world is set up)
    if (!FRoundWorld(psw) || CChosenRaw() < 4) return 0;
    for (const LO* plo : aploGroup)
        if (plo && plo->oid == kPlane && !plo->paloParent && plo->Cid() == 0x52) return CChosenRaw() - 3;
    return 0;
}

void OnWorldLoaded(SW* psw)
{
    r = ROUND{};
    if (!FRoundWorld(psw)) return;
    g_pfnHudPiece3 = FHudPiece;   // (only a round answers: these test r.fActive)
    g_pfnScreenObjSkip3 = FScreenObjSkip;
    r.fActive = true;
    r.psw = psw;
    r.cplayer = CChosenRaw();
    if (FPrint()) std::printf("duel3: free-for-all, %d players\n", r.cplayer);
}

int CViewsWanted() { return r.fActive ? r.cplayer : 0; }

LO* PloPlayerWanted(SW* psw, int i)
{
    if (!r.fActive || i <= 0 || i >= r.cplayer) return nullptr;
    if (i == 1) return PloFindSwPlayerByOid3(psw, kProxy2);
    return PloExtra(psw, i);
}

void OnPlayersChosen(SW* psw)
{
    if (!r.fActive) return;
    for (int i = 0; i < r.cplayer; ++i) r.ap[size_t(i)] = PLAYER{split3::PloPlayer(psw, i)};
    for (int i = 0; i < r.cplayer; ++i)
    {
        LO* plo = r.ap[size_t(i)].plo;
        if (!plo) continue;
        if (i >= 2)
        {
            // configured as player 1 / 2's plane (the pilot 1823: Sly / Carmelita, 2483: the two-player game's
            // weapons and hit points), its start event 17 again (guardstart.cpp SendStartEvents3)
            LO* ptmpl = r.ap[size_t(i - 2)].plo;
            CopyBinding(ptmpl, plo, 1823);
            CopyBinding(ptmpl, plo, 2483);
            // (the plane's procedure s3453 also has a third body, 98236 (the original's cheat plane: handler s5 swaps
            // 1823 between 24 and 98236), but natively it shows only its propeller in h_dogfight: not used. Players 3 / 4
            // fly Sly's / Carmelita's planes and are told apart by their colours: the HUD's markers and kills)
            CRef refNone;
            HandleLoSpliceEvent(plo, SPLICEEV3_ATTACH, false, 1, &refNone);
            if (!plo->fInWorld) AddLo(psw, plo);
            RestorePlane3(plo);
            if (LO* ps = PloSpawnFarthest(psw, i)) TeleportLo(plo, ps);
        }
        SetPlaneTeam3(plo, i);   // (each player a team of its own: the planes' and the autopilot's targets)
        r.ap[size_t(i)].pilot = (i & 1) ? 28 : 24;
        if (FPrint()) std::printf("duel3: player %d plane %p (instance %d) team %d hp %d\n", i + 1, (void*)plo, plo->oidInstance, i, NPlaneHp3(plo));
    }
    for (int i = 0; i < r.cplayer; ++i)
        if (LO* plo = r.ap[size_t(i)].plo) EnsurePlaneBody(psw, plo, r.ap[size_t(i)].pilot);
    char ach[96];
    std::snprintf(ach, sizeof ach, StrPort(PTX_DuelFirstTo).c_str(), kTarget);
    Message(ach, -1);
}

bool FOnBroadcast(int code, LO* plo)
{
    if (!r.fActive || r.fEnded || code != 0xC) return false;
    const int i = IplayerOf(plo);
    if (i < 0) return false;
    PLAYER& p = r.ap[size_t(i)];
    if (p.fDead) return true;
    p.fDead = true;
    p.tDead = g_clock.t;
    if (r.winner < 0)
    {
        float t = -1.0f;
        LO* pshooter = PloPlaneLastShooter3(plo, &t);
        const int ik = pshooter && t >= 0.0f && g_clock.t - t <= kShooterWindow ? IplayerOf(pshooter) : -1;
        Score(ik != i ? ik : -1, i);
    }
    return true;   // (not the duel splice's: its two-player scoring and respawn)
}

// players 3 / 4's gauges: the look of the player whose plane they fly (players 1 / 2's 96163 / 96161: the blue / orange
// plane in its oval), a copy of it from the extra group (the world's own copies of 95691 keep the single-player 95688)
void EnsureGauges()
{
    for (int i = 2; i < r.cplayer; ++i)
    {
        LO* plo = r.ap[size_t(i)].plo;
        LO* pgTmpl = PloGaugeOf3(r.ap[size_t(i - 2)].plo);
        LO* pg = PloGaugeOf3(plo);
        if (!plo || !pgTmpl || (pg && pg->oid == pgTmpl->oid)) continue;
        for (LO* p : AploOfOid(r.psw, pgTmpl->oid))
        {
            if (p->Cid() != 0xA3 || p == pgTmpl) continue;
            LO* owner = PloGaugePlayer3(p);
            if (owner && IplayerOf(owner) >= 0) continue;
            SetGaugeOwner3(p, plo);
            if (FPrint()) std::printf("duel3: player %d gauge %d (%p)\n", i + 1, p->oid, (void*)p);
            break;
        }
    }
}

void Update(SW* psw)
{
    if (!r.fActive || r.fEnded || !psw) return;
    EnsureGauges();
    const float t = g_clock.t;
    if (r.winner >= 0)
    {
        if (t - r.tWin > kWinHold)
        {
            r.fEnded = true;
            if (FPrint()) std::printf("duel3: round over, back to the chooser\n");
            EndChallenge3(psw);
        }
        return;
    }
    for (int i = 0; i < r.cplayer; ++i)
    {
        PLAYER& p = r.ap[size_t(i)];
        if (!p.plo || !p.fDead || t - p.tDead < kRespawn) continue;
        // the original's respawn (s4961): back in the world, restored (option 948), at the farthest spawn point
        p.fDead = false;
        if (!p.plo->fInWorld) AddLo(psw, p.plo);
        RestorePlane3(p.plo);
        if (LO* ps = PloSpawnFarthest(psw, i)) TeleportLo(p.plo, ps);
        EnsurePlaneBody(psw, p.plo, p.pilot);
        if (FPrint()) std::printf("duel3: player %d back (t %.2f)\n", i + 1, t);
    }
}

bool FTagOffset(int i, glm::vec4* poff)
{
    if (!r.fActive || i < 0 || i >= r.cplayer) return false;
    const glm::vec4 c = RgbOf(RgbaPlayer(i));
    const float k = 110.0f / std::max({c.r, c.g, c.b, 1.0f});   // (split3's tags: the strongest channel 110)
    *poff = glm::vec4(c.r * k, c.g * k, c.b * k, 0.0f);
    return true;
}

bool FScoreString(std::string* pstr)
{
    if (!r.fActive || g_iviewHud3 < 0 || g_iviewHud3 >= r.cplayer) return false;
    *pstr = std::to_string(r.ap[size_t(g_iviewHud3)].kills);
    return true;
}

void DrawOverlay(int width, int height, float xLeft, float xRight, float yTop, float yBottom)
{
    if (!r.fActive || !split3::FActive()) return;
    auto xCanvas = [&](float x) { return xLeft + (xRight - xLeft) * x / float(std::max(width, 1)); };
    auto yCanvas = [&](float y) { return yTop + (yBottom - yTop) * (1.0f - y / float(std::max(height, 1))); };
    const float t = g_clock.t;
    for (int iv = 0; iv < split3::CView() && iv < r.cplayer; ++iv)
    {
        const split3::VIEWRECT rv = split3::RectView(iv, width, height);
        split3::VIEWRECT rs{0, 0, rv.dx, rv.dy};
        split3::FRectScene(iv, rv.dx, rv.dy, &rs);
        const float x0 = xCanvas(float(rv.x + rs.x)), x1 = xCanvas(float(rv.x + rs.x + rs.dx));
        const float y0 = yCanvas(float(rv.y + rs.y + rs.dy)), y1 = yCanvas(float(rv.y + rs.y));   // (y0 the top)
        const float xc = 0.5f * (x0 + x1);
        // the other players' planes: a marker in their colour with their number (an arrow on the edge when outside)
        const CAMERA cam = CameraFromCm(split3::CmView(iv));
        const glm::mat4 m = cam.MatWorldToClip(float(rs.dx) / float(std::max(rs.dy, 1)));
        for (int j = 0; j < r.cplayer; ++j)
        {
            const PLAYER& o = r.ap[size_t(j)];
            if (j == iv || !o.plo || o.fDead || !FPlaneFlying3(o.plo)) continue;
            const glm::vec4 c = m * glm::vec4(PosLoWorld(o.plo) + glm::vec3(0.0f, 0.0f, 250.0f), 1.0f);
            glm::vec2 ndc = c.w > 1.0f ? glm::vec2(c.x, c.y) / c.w : glm::vec2(c.x, c.y) * (c.w < 0.0f ? -1.0f : 1.0f);
            const glm::vec4 rgb = RgbOf(RgbaPlayer(j));
            if (c.w > 1.0f && std::fabs(ndc.x) < 0.95f && std::fabs(ndc.y) < 0.9f)
            {
                const float x = x0 + (ndc.x * 0.5f + 0.5f) * (x1 - x0), y = y1 - (ndc.y * 0.5f + 0.5f) * (y1 - y0);
                Arrow(x, y + 4.0f, 1.5708f, 7.0f, Rgb(0, 0, 0, 110), 41.0f);
                Arrow(x, y + 4.0f, 1.5708f, 5.5f, rgb, 42.0f);
                Text(std::to_string(j + 1), x, y - 16.0f, 12.0f, OffOf(j));
                continue;
            }
            if (std::fabs(ndc.x) < 1e-4f && std::fabs(ndc.y) < 1e-4f) ndc.y = -1.0f;
            const float k = std::min(0.9f / std::max(std::fabs(ndc.x), 1e-4f), 0.8f / std::max(std::fabs(ndc.y), 1e-4f));
            const glm::vec2 e = ndc * k;
            const float x = 0.5f * (x0 + x1) + e.x * 0.5f * (x1 - x0), y = 0.5f * (y0 + y1) - e.y * 0.5f * (y1 - y0);
            const float ang = std::atan2(-e.y * (y1 - y0), e.x * (x1 - x0));
            Arrow(x, y, ang, 7.0f, Rgb(0, 0, 0, 110), 41.0f);
            Arrow(x, y, ang, 5.5f, rgb, 42.0f);
        }
        // the message (a few seconds), the winner's banner
        if (t - r.tMsg < kMsg || r.winner >= 0)
        {
            const float size = r.winner >= 0 ? 26.0f : 13.0f;
            const float yc = r.winner >= 0 ? 0.5f * (y0 + y1) - size : y0 + 6.0f;   // (the view's top: little of the view covered)
            const float wm = std::min(x1 - x0 - 8.0f, r.winner >= 0 ? 300.0f : 260.0f);
            Rect(xc - 0.5f * wm, yc - 4.0f, xc + 0.5f * wm, yc + size * 1.3f, Rgb(0, 0, 0, 90), 30.0f);
            if (r.iMsg >= 0) Rect(xc - 0.5f * wm, yc + size * 1.3f - 2.0f, xc + 0.5f * wm, yc + size * 1.3f, RgbOf(RgbaPlayer(r.iMsg)), 31.0f);
            Text(r.msg, xc, yc, size, r.iMsg >= 0 ? OffOf(r.iMsg) : glm::vec4(110, 110, 110, 0));
        }
    }
    // the kills of every player, once, in the middle of the picture (where the 2 x 2 views meet: the portraits' column on
    // a wide picture)
    {
        // (very wide pictures: the bands' gauges meet there, a smaller row)
        const float k = float(width) > 2.5f * float(std::max(height, 1)) ? 0.7f : 1.0f;
        const float xc = 0.5f * (xLeft + xRight), yc = 0.5f * (yTop + yBottom);
        const float dx = 30.0f * k, w = dx * float(r.cplayer);
        Rect(xc - 0.5f * w - 4.0f * k, yc - 12.0f * k, xc + 0.5f * w + 4.0f * k, yc + 11.0f * k, Rgb(0, 0, 0, 100), 30.0f);
        for (int j = 0; j < r.cplayer; ++j)
        {
            const float xj = xc - 0.5f * w + dx * (float(j) + 0.5f);
            Rect(xj - 13.0f * k, yc + 9.0f * k, xj + 13.0f * k, yc + 11.0f * k, RgbOf(RgbaPlayer(j)), 31.0f);
            Text(std::to_string(r.ap[size_t(j)].kills), xj, yc - 11.0f * k, 17.0f * k, OffOf(j));
        }
    }
}

void DrawMenu(int width, int height)
{
    const LO* pnode = PnodeTwoPlayerShown3();
    if (!FDuelNode(pnode)) return;
    g_hud.Begin();
    // (the episode menu's 4:3 page: its right column, between the description and "Start Challenge", as the Cops and
    // Robbers map select)
    const int c = CChosenRaw();
    const float xL = 255.0f, xR = 608.0f, y = 262.0f;
    Rect(xL, y, xR, y + 46.0f, Rgb(0, 4, 24, 110), -10.0f);
    Rect(xL, y + 44.0f, xR, y + 46.0f, Rgb(20, 40, 90, 128), -9.0f);
    Text(StrPort(PTX_DuelPlayers), xL + 8.0f, y + 5.0f, 15.0f, glm::vec4(70, 90, 128, 0), 0);
    Text("&2+&.", xL + 8.0f, y + 24.0f, 15.0f, glm::vec4(70, 90, 128, 0), 0);
    const float xc = 0.5f * (xL + xR) + 30.0f;
    if (c > 2) Arrow(xc - 118.0f, y + 15.0f, 3.14159f, 7.0f, Rgb(126, 104, 10), -8.0f);
    if (c < 4) Arrow(xc + 118.0f, y + 15.0f, 0.0f, 7.0f, Rgb(126, 104, 10), -8.0f);
    char ach[64];
    std::snprintf(ach, sizeof ach, StrPort(PTX_DuelNPlayers).c_str(), c);
    Text(ach, xc, y + 3.0f, 22.0f, glm::vec4(128, 106, 10, 0));
    Text(StrPort(c == 2 ? PTX_DuelOriginal : PTX_DuelFreeForAll), xc, y + 29.0f, 11.0f, glm::vec4(80, 80, 80, 0));
    g_hud.Flush(width, height);
}
}  // namespace duel3
