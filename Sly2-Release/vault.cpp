#include "actioninput.h"
// The vault (cid 0x4E, one per episode hub; module 0x1C7D00..0x1C8B68): the safe the clue bottles open. Sly uses it
// (the splice method 235, thunk 0x110A28 -> 0x1C8AF0) once the world's 30 bottles are his: the vault takes the pad
// (vt slot 79), its state scripts run (options 1616..1620), and while the dial's script runs the left stick picks
// one of the nine keys (the 3 x 3 directions at 0x2C0FF0, 1..9), X (0xC0) enters it and triangle (0x30) gives up;
// three keys equal to the combination (option 1225, default 123) open it: the reward (option 1228, a power-up),
// Sly's line (option 1229) and the world's fVault progress flag. Without the bottles: Sly's line (option 1227).
//
// Native port of 0x1C8280 (init), 0x1C82F0 (post-load), 0x1C8350 (update), 0x1C84D8 (the dial), 0x1C8730 (the
// state), 0x1C8918 (the dial's sub-state), 0x1C8AF0 (used), 0x1C8110 / 0x1C8188 (fVault), 0x1C7D08 / 0x1C7D00 (the
// bottles counted, SW +0x4764 / +0x4768).
//
// Fields by the original's offsets: +0x180 state (-1, 0 closed, 1 opening, 2 the dial, 3 right, 4 wrong, 5 open),
// +0x188 the dial's sub-state (-1, 0 idle, 1 moving to a key, 2 entering it, 3 done), +0x190 the actor using it,
// +0x194 the key (0..8), +0x198 the number entered, +0x19C the state's script, +0x1A0 the key's ASEGA, +0x1A4 the
// dialog line, the parts (0x2C0EC8, 24): +0x1A8[9] the actor's poses per key (1596..1604), +0x1CC[9] the vault's
// ASEGs per key (1607..1615), +0x208 the actor's pose at the dial (1606), +0x210..+0x220 the scripts of states 1..5
// (1616..1620; +0x20C state 0: none); +0x204 the dial's record (tag 0x645, kind 1).

#include "jt.h"
#include "interact.h"
#include "actor.h"
#include "asega.h"
#include "clock.h"
#include "cls.h"
#include "lo.h"
#include "objmethod.h"
#include "po.h"
#include "savedata.h"
#include "scpt.h"
#include "so.h"
#include "sound.h"
#include "sw.h"
#include "task.h"
#include "transition.h"
#include "update.h"
#include "worldaccess.h"
#include "xf.h"
#include "xfphys.h"
#include "ai.h"
#include "actor.h"
#include "hudwidget.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

void BindLoPartsTable(ALO* palo, int c, uint32_t vaTable);  // loaders.cpp: orig 0x1A64E8
void EnsureScptAlarm(LO* pscpt, int n);                       // scpt.cpp: orig 0x1E0020

namespace
{
struct VAULT
{
    int nState = -1;          // +0x180
    float tState = 0;         // +0x184
    int nSub = -1;            // +0x188
    float tSub = 0;           // +0x18C
    LO* ploActor = nullptr;   // +0x190
    int ikey = -1;            // +0x194
    int nEntered = 0;         // +0x198
    LO* pscpt = nullptr;      // +0x19C
    ASEGA* pasega = nullptr;  // +0x1A0
    bool fDialog = false;     // +0x1A4
    LO* aploPose[9]{};        // +0x1A8
    LO* aploAseg[9]{};        // +0x1CC
    int nCombination = 123;   // +0x1F0 (option 1225)
    int idDialogStart = -1;   // +0x1F4 (1226)
    int idDialogBottles = -1; // +0x1F8 (1227)
    int idDialogOpen = -1;    // +0x1FC (1229)
    int idReward = -1;        // +0x200 (1228)
    XF* pxfDial = nullptr;    // +0x204
    LO* ploPoseDial = nullptr;  // +0x208
    LO* apscpt[6]{};          // +0x20C..+0x220 by state
};
std::unordered_map<LO*, VAULT> s_mpvault;

bool FDebug() { static const bool s_f = std::getenv("SLY2N_VAULTDBG") != nullptr; return s_f; }

void WarnOnce(const char* what)
{
    static std::unordered_map<std::string, bool> s_mp;
    if (s_mp[what]) return;
    s_mp[what] = true;
    std::fprintf(stderr, "vault: %s not ported\n", what);
}

int NOption(LO* plo, int eopid, int nDefault)
{
    for (const OPT& o : plo->aopt)
        if (o.eopid == eopid && !o.aval.empty()) return o.aval[0].n;
    return nDefault;
}

// the current world's progress key + name (+ n)
std::vector<int16_t> AkeyWorld(const char* pszName, int n = -1)
{
    std::vector<int16_t> akey;
    const auto& awrec = AwrecWorlds();
    const int i = g_transition.iwrecCur;
    if (i < 0 || i >= static_cast<int>(awrec.size())) return akey;
    akey.assign(awrec[i].an.begin(), awrec[i].an.end());
    akey.push_back(static_cast<int16_t>(NSymProgress(pszName)));
    if (n >= 0) akey.push_back(static_cast<int16_t>(n));
    return akey;
}
bool FRef(const CRef& r) { return (r.m_tagk == TAGK_Bool && r.m_bool) || (r.m_tagk == TAGK_S32 && r.m_n); }

// orig: 0x001C7D00 (the bottles a vault needs) / 0x001C7D08 (the world's bottles collected: its afClue bits)
int CClueNeeded() { return 30; }
// 0x1C7D08 walks the children of the current world's parent node in the progress trie (its key cut to three
// elements: the episode's levels), and for each one counts the bits of its afClue leaf's 32-bit word. The levels
// are the world records sharing the current record's key but for its last element. (Ep1's vault is in an interior,
// world 4, while its bottles are saved under the hub's level: counting only the current level gave 0.)
int CClueCollected()
{
    const auto& awrec = AwrecWorlds();
    const int i = g_transition.iwrecCur;
    if (i < 0 || i >= static_cast<int>(awrec.size()) || awrec[i].an.empty()) return 0;
    const std::vector<uint16_t>& anCur = awrec[i].an;
    const int16_t symClue = static_cast<int16_t>(NSymProgress("afClue"));
    std::vector<std::vector<uint16_t>> aanDone;
    int c = 0;
    for (const WREC& w : awrec)
    {
        if (w.an.size() != anCur.size() || !std::equal(anCur.begin(), anCur.end() - 1, w.an.begin())) continue;
        if (std::find(aanDone.begin(), aanDone.end(), w.an) != aanDone.end()) continue;
        aanDone.push_back(w.an);
#ifdef __SWITCH__
        // (the Switch: one key built per level, its last element set to each bit: the HUD's counter and the markers ask
        // every frame)
        std::vector<int16_t> akey(w.an.begin(), w.an.end());
        akey.push_back(symClue);
        akey.push_back(0);
        for (int n = 0; n < 32; ++n)
        {
            akey.back() = static_cast<int16_t>(n);
            if (FRef(RefGetProgressKey(akey))) ++c;
        }
#else
        for (int n = 0; n < 32; ++n)
        {
            std::vector<int16_t> akey(w.an.begin(), w.an.end());
            akey.push_back(symClue);
            akey.push_back(static_cast<int16_t>(n));
            if (FRef(RefGetProgressKey(akey))) ++c;
        }
#endif
    }
    return c;
}

// orig: 0x001C8110 / 0x001C8188 (the world's fVault)
bool FVaultOpened()
{
    const std::vector<int16_t> akey = AkeyWorld("fVault");
    return !akey.empty() && FRef(RefGetProgressKey(akey));
}
void SetVaultOpened(bool f)
{
    const std::vector<int16_t> akey = AkeyWorld("fVault");
    if (akey.empty()) return;
    const bool fFirst = f && !FVaultOpened();
    CRef r;
    r.SetBool(f);
    FSetProgressKey(akey, r);
    if (fFirst) HudAutoSave(false);  // 0x1ABD98(g_save, 0): the autosave
}

// orig: 0x00142140 (the actor's pose sub-object +0x4C0: kind 5 with the object, null: back out of it)
void SetActorPose(LO* ploActor, LO* plo)
{
    if (ACTOR* pa = PactorFromLo(ploActor)) SetActorInteractObject(pa, 5, plo);  // (interact.cpp)
}

void SetVaultState(LO* plo, VAULT& v, int n);

// orig: 0x001C8918
void SetVaultSub(LO* plo, VAULT& v, int n)
{
    if (n == v.nSub) return;
    RetractAsegaPp(&v.pasega);  // 0x1306A8
    v.nSub = n;
    v.tSub = g_clock.t;
    if (FDebug()) std::printf("vault %d: dial %d (key %d, entered %d)\n", plo->oid, n, v.ikey + 1, v.nEntered);
    switch (n)
    {
    case -1:
        v.ikey = -1;
        SetActorPose(v.ploActor, nullptr);
        break;
    case 0:
        SetActorPose(v.ploActor, v.ikey >= 0 ? v.aploPose[v.ikey] : nullptr);
        break;
    case 1:
    {
        // the dial's record moves (a spline, 0.25 s) to the key's pose, leaving along the actor's x axis
        SetActorPose(v.ploActor, v.ploPoseDial);
        // (R34: the key poses 1596..1604 are LOXs (class 3), so the goal is the pose's position by its vt slot
        // (0x1A62E8), any LO; natively it was taken only from an ALO, so the goal stayed where the dial was and Sly's
        // hand did not travel to the key, then jumped to it at the sub-state 0)
        XF* pxf = v.pxfDial;
        ALO* paloActor = dynamic_cast<ALO*>(v.ploActor);
        LO* ploPose = v.ikey >= 0 ? v.aploPose[v.ikey] : nullptr;
        if (!pxf) break;
        pxf->tTarget = g_clock.t + 0.25f;
        const glm::mat3 matActor = paloActor ? glm::mat3(MatJtLo(paloActor)) : glm::mat3(1.0f);  // 0x1A6FC0(actor, 0, v)
        pxf->vel = glm::vec4(VecConvertXfs(nullptr, pxf->pxfRef, matActor * glm::vec3(0.0f)), 0.0f);  // 0x2C1080
        if (ploPose) pxf->aseg[0] = glm::vec4(PosConvertXfs(nullptr, pxf->pxfRef, PosJtLo(ploPose)), 1.0f);  // 0x1A62E8
        pxf->aseg[1] = glm::vec4(VecConvertXfs(nullptr, pxf->pxfRef, matActor * glm::vec3(100.0f, 0.0f, 0.0f)), 0.0f);  // 0x2C1090
        if (ALO* palo = dynamic_cast<ALO*>(plo)) SetXfMode(palo, pxf, XFM_Spline);
        break;
    }
    case 2:
        SetActorPose(v.ploActor, v.ikey >= 0 ? v.aploPose[v.ikey] : nullptr);
        if (v.ikey >= 0 && v.aploAseg[v.ikey] && FCidDerivedFrom(v.aploAseg[v.ikey]->Cid(), 0x6D))
            ApplyAsegCur(static_cast<ASEG*>(v.aploAseg[v.ikey]), dynamic_cast<ALO*>(plo), 0.0f, 1.0f, 0, &v.pasega);
        break;
    default:
        break;
    }
}

// orig: 0x001C84D8 (the dial while its script runs)
void UpdateVaultDial(LO* plo, VAULT& v)
{
    PODATA* ppo = PpodataFromLo(plo);
    PADCTL* ppc = ppo ? ppo->ppadctl : nullptr;
    const int nSubStart = v.nSub;
    for (int i = 0; i < 8; ++i)
    {
        if (sly2actions::Take(ppc, sly2actions::Action::VaultCancel, 0x30, 0.0f))
        {
            if (v.pscpt) EnsureScptAlarm(v.pscpt, 0x655);
            SetVaultSub(plo, v, 3);
        }
        switch (v.nSub)
        {
        case 0:
            if (sly2actions::Take(ppc, sly2actions::Action::VaultConfirm, 0xC0, 0.0f)) SetVaultSub(plo, v, 2);
            else if (ppc)
            {
                // the nearest of the nine stick positions (the current one's distance x 0.7)
                static const float s_ag[9][2] = {{-0.7f, 0.7f}, {0.0f, 0.8f}, {0.7f, 0.7f}, {-0.8f, 0.0f}, {0.0f, 0.0f},
                                                 {0.8f, 0.0f}, {-0.7f, -0.7f}, {0.0f, -0.8f}, {0.7f, -0.7f}};
                float gMin = 3.4028235e38f;
                int iMin = -1;
                for (int k = 0; k < 9; ++k)
                {
                    const float dx = ppc->x - s_ag[k][0], dy = ppc->y - s_ag[k][1];
                    float g = dx * dx + dy * dy;
                    if (k == v.ikey) g *= 0.7f;
                    if (g < gMin) { gMin = g; iMin = k; }
                }
                if (iMin != v.ikey)
                {
                    v.ikey = iMin;
                    SetVaultSub(plo, v, 1);
                }
            }
            break;
        case 1:
            if (!v.pxfDial || v.pxfDial->tTarget <= g_clock.t) SetVaultSub(plo, v, 0);
            break;
        case 2:
            if (v.pasega && !FAsegaRetracted(v.pasega)) break;
            v.nEntered = (v.nEntered * 10) % 1000 + v.ikey + 1;
            if (v.nEntered == v.nCombination)
            {
                if (v.pscpt) EnsureScptAlarm(v.pscpt, 0x655);
                SetVaultSub(plo, v, 3);
            }
            else SetVaultSub(plo, v, 0);
            break;
        default:
            break;
        }
        if (v.nSub == nSubStart) break;
    }
}

// orig: 0x001C8730
void SetVaultState(LO* plo, VAULT& v, int n)
{
    if (n == v.nState) return;
    if (v.fDialog)
    {
        StopDialog();  // 0x1FE738
        v.fDialog = false;
    }
    if (v.pscpt) StopScpt(v.pscpt);
    switch (v.nState)
    {
    case 2:
        SetVaultSub(plo, v, -1);
        if (--g_gamepause.c80 == 0) HideSubtitleBox();  // 0x20F160: at 0 0x1A30D0
        break;
    case 3:
        if (v.idReward != -1)
        {
            // 0x1DAF38: the power-up is his (TODO: its broadcast 0x3B as the shop's)
            CRef r;
            r.SetBool(true);
            FSetProgress({"powerUps", "mppukfAcquired", std::to_string(v.idReward)}, r);
        }
        if (v.idDialogOpen != 0 && v.idDialogOpen != -1) v.fDialog = PsndStartDialog(v.idDialogOpen, 1) != nullptr;
        break;
    case 0:
        if (v.ploActor) ++g_gamepause.c84;  // 0x20F198
        break;
    default:
        break;
    }
    if (FDebug()) std::printf("vault %d: state %d -> %d (t %.2f)\n", plo->oid, v.nState, n, g_clock.t);
    v.nState = n;
    v.tState = g_clock.t;
    switch (n)
    {
    case 0:
    case 5:
        // the actor gets the pad back (vt slot 79)
        if (v.ploActor)
        {
            EnablePo(g_psw, v.ploActor, 1);
            v.ploActor = nullptr;
            --g_gamepause.c84;  // 0x20F1A8
        }
        break;
    case 1:
        EnablePo(g_psw, plo, 1);  // the vault takes it
        break;
    case 2:
        v.ikey = 5;
        SetVaultSub(plo, v, 1);
        if (++g_gamepause.c80 == 1) ShowSubtitleBox();  // 0x20F128: at 1 0x1A3078 (the subtitle reset, the box)
        if (v.idDialogStart != 0 && v.idDialogStart != -1) v.fDialog = PsndStartDialog(v.idDialogStart, 0) != nullptr;
        break;
    default:
        break;
    }
    v.pscpt = (n >= 0 && n < 6) ? v.apscpt[n] : nullptr;
    if (v.pscpt) StartScpt(v.pscpt, v.ploActor);
}

// orig: 0x001C8280 (init) and 0x001C82F0 (post-load)
void InitVault(SW* psw, LO* plo)
{
    VAULT& v = s_mpvault[plo];
    v = VAULT{};
    v.nCombination = NOption(plo, 1439, 0x7B);
    v.idDialogStart = NOption(plo, 1440, -1);
    v.idDialogBottles = NOption(plo, 1441, -1);
    v.idReward = NOption(plo, 1442, -1);
    v.idDialogOpen = NOption(plo, 1443, -1);
    ALO* palo = dynamic_cast<ALO*>(plo);
    if (palo)
    {
        BindLoPartsTable(palo, 0x18, 0x2C0EC8);  // 0x1A64E8
        for (const auto& [ib, p] : palo->apart)
        {
            if (ib >= 0x1A8 && ib < 0x1CC) v.aploPose[(ib - 0x1A8) / 4] = p;
            else if (ib >= 0x1CC && ib < 0x1F0) v.aploAseg[(ib - 0x1CC) / 4] = p;
            else if (ib == 0x208) v.ploPoseDial = p;
            else if (ib >= 0x20C && ib <= 0x220) v.apscpt[(ib - 0x20C) / 4] = p;
        }
        for (const auto& up : palo->axf)  // 0x1F4C58(0x645, 1)
            if (up && up->tag == 0x645 && up->kind == 1) { v.pxfDial = up.get(); break; }
    }
    if (plo->iupd >= 0) psw->aupd[static_cast<size_t>(plo->iupd)].grf |= GRFUPD_Update;
    else plo->grfUpd |= GRFUPD_Update;
    if (FDebug())
        std::printf("vault %d: combination %d, dialogs %d / %d / %d, reward %d, dial record %s, opened %d, bottles %d / %d\n",
                    plo->oid, v.nCombination, v.idDialogStart, v.idDialogBottles, v.idDialogOpen, v.idReward,
                    v.pxfDial ? "yes" : "no", FVaultOpened(), CClueCollected(), CClueNeeded());
    if (FDebug())
    {
        std::printf("vault %d: scripts", plo->oid);
        for (LO* p : v.apscpt) std::printf(" %d", p ? p->oid : -1);
        std::printf(", poses");
        for (LO* p : v.aploPose) std::printf(" %d", p ? p->oid : -1);
        std::printf(", asegs");
        for (LO* p : v.aploAseg) std::printf(" %d/0x%X", p ? p->oid : -1, p ? p->Cid() : 0);
        std::printf(", dial pose %d\n", v.ploPoseDial ? v.ploPoseDial->oid : -1);
    }
    SetVaultState(plo, v, FVaultOpened() ? 5 : 0);
}

// orig: 0x001C8350 (vt slot 16)
void UpdateVault(LO* plo)
{
    if (PFNUPDATE pfn = PfnUpdateOrig(0x1F36E0)) pfn(plo);
    auto it = s_mpvault.find(plo);
    if (it == s_mpvault.end()) return;
    VAULT& v = it->second;
    for (int i = 0; i < 8; ++i)
    {
        const int nState = v.nState;
        const bool fRunning = v.pscpt && FScptRunning(v.pscpt);
        switch (nState)
        {
        case 1:
            if (!fRunning) SetVaultState(plo, v, 2);
            break;
        case 2:
            if (fRunning) UpdateVaultDial(plo, v);
            else SetVaultState(plo, v, v.nEntered == v.nCombination ? 3 : 4);
            break;
        case 3:
            if (!fRunning)
            {
                SetVaultOpened(true);
                SetVaultState(plo, v, 5);
            }
            break;
        case 4:
            if (!fRunning) SetVaultState(plo, v, 0);
            break;
        default:
            break;
        }
        if (v.nState == nState) break;
    }
}

// orig: 0x001C8AF0 (the actor uses the vault)
void UseVault(LO* plo, LO* ploActor)
{
    auto it = s_mpvault.find(plo);
    if (it == s_mpvault.end()) return;
    VAULT& v = it->second;
    if (FDebug()) std::printf("vault %d: used by %d (bottles %d / %d)\n", plo->oid, ploActor ? ploActor->oid : -1, CClueCollected(), CClueNeeded());
    if (CClueCollected() >= CClueNeeded())
    {
        v.ploActor = ploActor;
        SetVaultState(plo, v, 1);
        return;
    }
    WarnOnce("0x1618B0 (SW +0x3BB8's object released)");
    if (v.idDialogBottles != -1) PsndStartDialog(v.idDialogBottles, 1);
}

struct INITVAULT
{
    INITVAULT()
    {
        RegisterLoUpdate(0x1C8350, UpdateVault);
        // the init / post-load at the world's first frame
        RegisterSwPass(0x1F77C0, [](SW* psw) {
            static uint32_t s_serial = 0;
            static SW* s_pswDone = nullptr;
            if (!psw) return;
            if (psw != s_pswDone || psw->serial != s_serial)
            {
                s_pswDone = psw;
                s_serial = psw->serial;
                s_mpvault.clear();
                for (const auto& up : psw->aploAll)
                {
                    LO* plo = up.get();
                    if (plo && plo->pcls && plo->pcls->PfnOrig(16) == 0x1C8350) InitVault(psw, plo);
                }
            }
            // debug: SLY2N_VAULT=frame[:bottles] uses the world's vault with the player at that frame (bottles: the
            // collected count assumed by setting the afClue bits)
            static int s_cframe = 0;
            ++s_cframe;
            if (const char* sz = std::getenv("SLY2N_VAULT"); sz && s_cframe == std::atoi(sz) && !s_mpvault.empty())
            {
                if (const char* pc = std::strchr(sz, ':'))
                    for (int n = 0; n < std::atoi(pc + 1); ++n)
                    {
                        CRef r;
                        r.SetBool(true);
                        FSetProgressKey(AkeyWorld("afClue", n), r);
                    }
                // (SLY2N_VAULTAT=oid: the player first placed at that object, e.g. 18440 in world 4)
                if (const char* szAt = std::getenv("SLY2N_VAULTAT"))
                    if (LO* pat = PloFindSwObjectByOid(psw, std::atoi(szAt))) TeleportActor(PloPoCurrent(psw), pat);
                UseVault(s_mpvault.begin()->first, PloPoCurrent(psw));
            }
        });
        // method 235 (EOPID 1444): the thunk 0x110A28 -> 0x1C8AF0 (the actor)
        RegisterObjectMethod(0x110A28, [](LO* plo, const std::vector<CRef>& a) {
            UseVault(plo, PloArg(a, 0));
            return CRef(TAGK_Void);
        });
    }
} s_initvault;
}  // namespace

// orig: 0x001C7D08 / 0x001C7D00 (for the bottle counter's string 0x2D0, screenrt.cpp: SW +0x4764 / +0x4768)
int CClueCollectedCur() { return CClueCollected(); }
int CClueNeededCur() { return CClueNeeded(); }
