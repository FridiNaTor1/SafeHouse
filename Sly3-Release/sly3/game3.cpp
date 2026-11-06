// Sly 3's game loop (game3.h).

#include "hud3.h"
#include "text3.h"
#include "game3.h"
#include "anim3.h"
#include "title3.h"
#include "card3.h"

#include "addr.h"
#include "boot3.h"
#include "scptrun3.h"
#include "main3.h"
#include "condset3.h"
#include "light3.h"
#include "../light.h"
#include "../emitter.h"
#include "ids3.h"
#include "load3.h"
#include "scene3.h"
#include "task3.h"
#include "trigger3.h"
#include "player3.h"
#include "spliceglue3.h"
#include "talk3.h"
#include "cmkey3.h"
#include "anim3.h"
#include "../asega.h"

#include "../bis.h"
#include "../cls.h"
#include "../disc.h"
#include "../display.h"
#include "../fmv.h"
#include "../frontend.h"
#include "../gl.h"
#include "../joy.h"
#include "../moviegl.h"
#include "../render.h"
#include "../serialize.h"
#include "../shd.h"
#include "../snd989.h"
#include "../sound.h"
#include "../splicemap.h"
#include "../sw.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>

#include <glm/gtc/matrix_transform.hpp>

GAME3 g_game3;
bool g_fScptRunner3 = false;
extern bool g_fSndFrameClock;  // sound.cpp
int IdDialogCur();  // sound.cpp (debug)
CLOCK g_clock3;  // orig3: the clock 0x165430 updates (Sly 2's CLOCK layout)

namespace
{
std::map<int, std::vector<PFNUPDATE3>> s_mpcidapfn;
std::vector<std::function<void(float)>> s_apfnFrame;

CM3* PcmNew(LO* plo)
{
    g_game3.acm.push_back(std::make_unique<CM3>());
    CM3* pcm = g_game3.acm.back().get();
    pcm->plo = plo;
    return pcm;
}

// orig3: 0x166E48 (cam, posEye, posTarget, dPan, dTilt): the rows from the eye to the target (ConvertVectorToSphere,
// BuildPanTiltMatrix), the translation the eye
void SetCmLookAt3(CM3* pcm, const glm::vec3& posEye, const glm::vec3& posTarget, float dPan, float dTilt)
{
    const glm::vec3 d = posTarget - posEye;
    const float pan = std::atan2(d.y, d.x) + dPan;
    const float tilt = std::atan2(d.z, std::sqrt(d.x * d.x + d.y * d.y)) + dTilt;
    const glm::vec3 x(std::cos(tilt) * std::cos(pan), std::cos(tilt) * std::sin(pan), std::sin(tilt));
    const glm::vec3 y(-std::sin(pan), std::cos(pan), 0.0f);
    pcm->mat = glm::mat3(x, y, glm::cross(x, y));  // (rows as the original's MATRIX3)
    pcm->pos = posEye;
}

}  // namespace

// The camera command 0x45 of the SCPT executor (args: pos x y z, euler roll / -tilt / pan, the vertical fov in radians,
// then the blend times ...): checked against PCSX2 st1, where the attract script 4262's first 0x45 left the world camera
// at its pos with rows {cos t cos p, cos t sin p, sin t} / {-sin p, cos p, 0} / their cross, +0x110 fov 1.36,
// +0x13C tan y = tan(fov / 2) = 0.8087.
void ApplyCmCutFromScpt3(CM3* pcm, const glm::vec3& pos, const glm::vec3& euler, float fovRad)
{
    SetCmLookAt3(pcm, pos, pos + glm::vec3(std::cos(euler.z), std::sin(euler.z), 0.0f), 0.0f, -euler.y);
    if (euler.x != 0.0f)  // the roll about the view direction
    {
        const glm::mat3 r = glm::mat3(glm::rotate(glm::mat4(1.0f), euler.x, pcm->mat[0]));
        pcm->mat[1] = r * pcm->mat[1];
        pcm->mat[2] = r * pcm->mat[2];
    }
    if (fovRad > 0.0f) pcm->tanY = std::tan(fovRad * 0.5f);
}

namespace
{

// orig3: 0x165748 (from StartSw 0x25B860): the world's camera is the one class 0x05 object of the world (SW +0x305C
// list of one; else PloNew(5, 0, 3)), stored at SW +0x463C; then cameras 4 and 5 (+0x4640 / +0x4644): looking from
// (0, -2000, 500) at the origin (0x320D40 / 0x320D50), fov 70 (0x1679E0), far 100000 (0x167A50), mode 0 / 1
// (0x168478), the viewports {6, 3, 0x1E5, 0xEC} / {0x9E, 0xF2, 0x27D, 0x1DA} (0x320D60 / 0x320D70: 0x167BF0).
void StartupCm3(SW* psw)
{
    const std::vector<LO*>& aplo = psw->mpcidaplo[0x05];
    LO* ploMain = aplo.size() == 1 ? aplo[0] : PloNew3(psw, 0x05, nullptr, 3, -1);
    g_game3.pcmMain = PcmNew(ploMain);
    if (const LOX* plox = static_cast<const LOX*>(ploMain))
    {
        for (int r = 0; r < 3; ++r) g_game3.pcmMain->mat[r] = glm::vec3(plox->mat[r]);
        g_game3.pcmMain->pos = glm::vec3(plox->mat[3]);
    }
    const int aarc[2][4] = {{6, 3, 0x1E5, 0xEC}, {0x9E, 0xF2, 0x27D, 0x1DA}};
    for (int i = 0; i < 2; ++i)
    {
        CM3* pcm = PcmNew(PloNew3(psw, 0x05, nullptr, 4 + i, -1));
        SetCmLookAt3(pcm, glm::vec3(0.0f, -2000.0f, 500.0f), glm::vec3(0.0f), 0.0f, 0.0f);
        pcm->fovDeg = 70.0f;
        pcm->sFar = 100000.0f;
        pcm->mode = i;
        std::copy(aarc[i], aarc[i] + 4, pcm->arcViewport);
        (i == 0 ? g_game3.pcm4 : g_game3.pcm5) = pcm;
    }
    // SW +0x3EB8 / +0x3EBC: at the title (PCSX2 st1 / st2) only the world's camera is updated
    g_game3.apcmUpdate = {g_game3.pcmMain};
}

// orig3: 0x173C10 NewFixedObjs (from StartSw): seven class 0x8B objects, oids 793..799 (in the original's order)
void NewFixedObjs3(SW* psw)
{
    for (OID oid : {793, 794, 795, 796, 798, 797, 799}) PloNew3(psw, 0x8B, nullptr, oid, -1);
}

// orig3: 0x25B130 (StartSw's first call): the class 0x8A object oid 11
void NewSw8A3(SW* psw) { PloNew3(psw, 0x8A, nullptr, 11, -1); }

// The SCPT executor's host for the game (the Sly 3 script port's scptrun3.h): the world's objects and scripts, the clock, the
// camera keys on the world's camera. The rest (binocucom, dialog, tasks) keeps SCPTHOST3's defaults until
// the Sly 3 player's / the Sly 3 script port's sides plug in.
struct GAMESCPTHOST3 : SCPTHOST3
{
    std::map<const LO*, SCPT3*> mpploscpt;
    float T(bool fRealTime) override { return fRealTime ? g_clock3.tReal : g_clock3.t; }
    void* PloFind(int, int oid, void*) override { return PloFindSwObjectByOid(g_game3.psw.get(), oid); }
    bool FDerivedFrom(void* plo, int cid) override { return plo && FCidDerivedFrom(static_cast<LO*>(plo)->Cid(), cid); }
    SCPT3* PscptFromLo(void* plo) override
    {
        const auto it = mpploscpt.find(static_cast<const LO*>(plo));
        return it == mpploscpt.end() ? nullptr : it->second;
    }
    // 0x50: the title (the Sly 3 script port's title3.*: START closes it; the input hook installed with the scripts)
    void ShowTitle() override { ShowTitle3(); talk3::ShowTitleScreen(true); }
    bool FTitleShowing() override { return FTitleShowing3(); }
    // 0x1E: the tasks' states (orig3 0x185908 ApplyCheckpointTasks, task3.cpp)
    void ApplyCheckpointTasks(int oidTask, int state, void* ploContext) override
    {
        const int apair[2] = {oidTask, state};
        ApplyCheckpointTasks3(apair, 1, ploContext);
    }
    // (SLY3_SCPTCMP=FILE: the executed commands and the done tests' changes per frame of the loop, in
    // tools/scpttrace3.py's format: scptcmp3.py REF FILE OID FRAME0 compares the game with the reference)
    FILE* pfCmp = nullptr;
    std::map<std::pair<RUN3*, int>, bool> mpdone;
    void Executed(RUN3& run, int icmd) override
    {
        if (!pfCmp) return;
        const SCPTCMD3& c = run.pscpt->acmd[std::size_t(icmd)];
        std::fprintf(pfCmp, "%5d exec oid %d [%d] %02X\n", g_game3.cframe, run.pscpt->oid, icmd, c.id);
    }
    void DoneTested(RUN3& run, int icmd, bool fDone) override
    {
        if (!pfCmp) return;
        auto [it, fNew] = mpdone.try_emplace({&run, icmd}, fDone);
        if (!fNew && it->second == fDone) return;
        it->second = fDone;
        const SCPTCMD3& c = run.pscpt->acmd[std::size_t(icmd)];
        std::fprintf(pfCmp, "%5d done oid %d [%d] %02X -> %d\n", g_game3.cframe, run.pscpt->oid, icmd, c.id, fDone ? 1 : 0);
    }
    // 0x16: an ASEG applied to an object (PasegaApplyAseg 0x137428(aseg, plo, 0.0, 1.0, grf): the Sly 3 animation's runtime,
    // asega.h), its retraction (FAsegaRetracted 0x138AC8 / RetractAsega 0x137B00) and the run's reference released
    static ALO* PaloOf(void* plo)
    {
        LO* p = static_cast<LO*>(plo);
        return p && FCidDerivedFrom(p->Cid(), Icid(CID3::ALO)) ? static_cast<ALO*>(p) : nullptr;
    }
    void* ApplyAseg(void* ploAseg, void* plo, int grf) override
    {
        LO* pa = static_cast<LO*>(ploAseg);
        ALO* palo = PaloOf(plo);
        if (!pa || !palo || !FCidDerivedFrom(pa->Cid(), Icid(CID3::ASEG)) || std::getenv("SLY3_NOANIM")) return nullptr;
        ASEGA* p = PasegaApplyAseg(static_cast<ASEG*>(pa), palo, 0.0f, 1.0f, grf);
        if (p) AddRefAsega(p);
        return p;
    }
    bool FAsegaRetracted(void* p) override { return !p || ::FAsegaRetracted(static_cast<ASEGA*>(p)); }
    void RetractAsega(void* p) override { if (p) ::RetractAsega(static_cast<ASEGA*>(p)); }
    void ReleaseAsega(void* p) override { if (p) ::ReleaseAsega(static_cast<ASEGA*>(p)); }
    // 0x30 / 0x16 with an actor: the actor's script animation (PlayActorScriptAnim 0x156578(actor, anim oid, grf, n),
    // EndActorScriptAnim 0x156690, FActorInScriptAnim 0x1566D0). Interim (TODO(sly3), the actor's own animation
    // state, the Sly 3 player): the ASEG of that oid near the actor applied to it, playing until it retracts.
    std::map<const void*, ASEGA*> mpactoranim;
    void PlayActorScriptAnim(void* ploActor, int oidAnim, int grf, int) override
    {
        EndActorScriptAnim(ploActor);
        ALO* palo = PaloOf(ploActor);
        if (!palo || std::getenv("SLY3_NOANIM")) return;
        if (ASEG* pa = sly3::PasegFind3(g_game3.psw.get(), oidAnim, palo))
            if (ASEGA* p = PasegaApplyAseg(pa, palo, 0.0f, 1.0f, grf))
            {
                AddRefAsega(p);
                mpactoranim[ploActor] = p;
            }
    }
    void EndActorScriptAnim(void* ploActor) override
    {
        auto it = mpactoranim.find(ploActor);
        if (it == mpactoranim.end()) return;
        if (!::FAsegaRetracted(it->second)) ::RetractAsega(it->second);
        ::ReleaseAsega(it->second);
        mpactoranim.erase(it);
    }
    bool FActorInScriptAnim(void* ploActor) override
    {
        auto it = mpactoranim.find(ploActor);
        return it != mpactoranim.end() && !::FAsegaRetracted(it->second);
    }
    // 0x1A / 0x1B: a screen object shown (vt slot +0x1A8 with n: a part, -1 its show part) / hidden (+0x1B0), hud3's
    // (m_ext 2081: the caption 2080 "Kaine Island, the South Pacific"); FScreenObjDone 0x260738: TODO(sly3) (done)
    void ShowScreenObj(void* plo, int n) override
    {
        if (SCREEN3* ps = PscreenFromLo3(static_cast<LO*>(plo))) ShowScreen3(ps, n > 0 ? n : -1);
    }
    void HideScreenObj(void* plo) override
    {
        if (SCREEN3* ps = PscreenFromLo3(static_cast<LO*>(plo))) HideScreen3(ps);
    }
    // the binocucom (talk3.h: interim, until the player side's binocucom), the dialog (sound.cpp's lines), the hint
    void OpenBinoc(void* ploActor, const BINOCOPEN3&, void* ploSpeaker, void*, void*, void*) override
    { talk3::OpenBinoc(static_cast<LO*>(ploActor), static_cast<LO*>(ploSpeaker)); }
    void CloseBinoc(void* ploActor) override { talk3::CloseBinoc(static_cast<LO*>(ploActor)); }
    int BinocState(void* ploActor) override { return talk3::BinocState(static_cast<LO*>(ploActor)); }
    void SetBinocSpeaker(void* ploActor, void* ploSpeaker) override { talk3::SetSpeaker(static_cast<LO*>(ploActor), static_cast<LO*>(ploSpeaker)); }
    // PsndStartDialog 0x2485D8 (+ SetSource / SetFalloff / SetVolume), FDialogPlaying 0x248858, StopDialog 0x248740
    bool StartDialog(const DIALOGSTART3& d, void* ploSource) override
    {
        if (d.aid.empty()) return false;
        CSndVoice* p = PsndStartDialog(d.aid[0], d.fQuiet);
        if (std::getenv("SLY3_TALKPRINT")) std::printf("talk3: frame %d dialog %d%s\n", g_game3.cframe, d.aid[0], p ? "" : " (no line)");
        if (!p) return false;
        p->SetSource(ploSource);
        if (d.sFalloffEnd > 0) p->SetFalloff(d.sFalloffStart, d.sFalloffEnd);
        p->SetVolume(d.gVolume);
        return true;
    }
    bool FDialogPlaying(int id) override { return ::FDialogPlaying(id); }
    void StopDialog() override { ::StopDialog(); }
    void ResetDialogSubtitle() override { ::ResetDialogSubtitle(); }
    void ShowHint(int id, float) override { talk3::ShowHint(id); }
    void ClearHint(int) override { talk3::ClearHint(); }
    // the player's control lock (0x22B238: 0x173448 take / 0x173498 release on the player's +0x1108 counter +0x64)
    void LockPlayerControl(bool fLock) override
    {
        clock += fLock ? 1 : -1;
        if (clock < 0) clock = 0;
        if (std::getenv("SLY3_TALKPRINT")) std::printf("talk3: frame %d player lock %d\n", g_game3.cframe, clock);
    }
    int clock = 0;
    // 0x4E: FButtonPressed 0x1A7A80(pad, mask, 0): the buttons pressed this frame, taken (one pad: the job's +0x134
    // and the default pad 0x36E740 are the same here); the prompt's buttons *(0x4BC228) (TODO(sly3): the HUD's icons)
    bool FButtonPressed(void*, int grfMask) override { return GrfbtnTakePressed(uint32_t(grfMask)) != 0; }
    void SetButtonPrompt(int grf) override { grfPrompt = grf; }
    int grfPrompt = 0;
    // 0x15 / the conditions outside 0x75E..0x767 / the start and stop: the script object's messages 4 / 5 / 6
    // (vt slot 3 0x1C86B8: its module's handlers; 0x22740C, 0x226A8C, 0x225EE8, 0x226050)
    void ScriptEvent(SCPT3* ps, int n) override
    {
        CRef a;
        a.SetS32(n);
        sly3::SendLoMessage3(static_cast<LO*>(ps->plo), 4, 1, &a);
    }
    bool FScriptCondition(SCPT3* ps, int kind, int) override
    {
        CRef a, r;
        a.SetS32(kind);
        if (!sly3::SendLoMessage3(static_cast<LO*>(ps->plo), 4, 1, &a, &r)) return false;
        return r.m_tagk == TAGK_Bool ? r.m_bool : r.m_tagk == TAGK_S32 ? r.m_n != 0 : false;
    }
    // an object's world placement (vt slot 36's matrix: rows, position) and 0x22's place of an object (the Sly 3 animation:
    // trigger3.h PlaceLo3 moves an ALO's records so its +0x48 record is there; the actor's reset is the player's)
    bool GetLoXf(void* plo, float mat[9], float pos[3]) override
    {
        if (!plo) return SCPTHOST3::GetLoXf(plo, mat, pos);
        const glm::mat4 m = sly3::MatLoWorld3(static_cast<LO*>(plo));
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 3; ++c) mat[r * 3 + c] = m[r][c];
        for (int i = 0; i < 3; ++i) pos[i] = m[3][i];
        return true;
    }
    void PlaceLo(void* plo, const float mat[9], const float pos[3]) override
    {
        sly3::PlaceLo3(static_cast<LO*>(plo), mat, glm::vec3(pos[0], pos[1], pos[2]));
    }
    void ScriptStarted(SCPT3* ps) override { sly3::SendLoMessage3(static_cast<LO*>(ps->plo), 5); }
    void ScriptStopped(SCPT3* ps) override { sly3::SendLoMessage3(static_cast<LO*>(ps->plo), 6); }
    // 0x760..0x762 (0x226950): the object's +0x54 == 1 (a task: activated once), +0xFC == 0 / 1 (the task's mode)
    bool FObjectCondition(int kind, void* plo) override
    {
        const TASK3* p = plo ? PtaskFind3(static_cast<LO*>(plo)->oid) : nullptr;
        if (!p) return false;
        if (kind == 0x760) return p->cActivated == 1;
        return kind == 0x761 ? p->nMode == 0 : kind == 0x762 ? p->nMode == 1 : false;
    }
    // 0x45: the run's camera keys (StartRunCamera 0x22BB78 / the key policy 0x16B670 / EndRunCamera 0x22BEE8: cmkey3.h)
    // on the run's camera (0x226B20: the world's camera here); the camera follows the newest run's policy
    std::map<const RUN3*, CPKEY3> mprunkey;
    const RUN3* prunCamera = nullptr;
    static LO* PloFindOid(int oid) { return PloFindSwObjectByOid(g_game3.psw.get(), oid); }
    void StartRunCamera(RUN3& run, const SCPTCMD3& key, const SCPTCMD3* pnext, const SCPTCMD3* pnext2) override
    {
        if (!g_game3.pcmMain) return;
        const CMKEY3 k = KeyFromCmd3(key, PloFindOid);
        const CMKEY3 kn = pnext ? KeyFromCmd3(*pnext, PloFindOid) : k;
        const CMKEY3 kn2 = pnext2 ? KeyFromCmd3(*pnext2, PloFindOid) : kn;
        const bool fReal = run.pscpt && (run.pscpt->grf & 0x200);
        StartCpkey3(mprunkey[&run], k, kn, kn2, *g_game3.pcmMain, T(fReal), fReal);
        prunCamera = &run;
        g_fScptRunner3 = true;
    }
    void EndRunCamera(RUN3& run) override
    {
        mprunkey.erase(&run);
        if (prunCamera == &run) prunCamera = nullptr;
    }
    // (the camera update 0x27DE38: the active key policy)
    void UpdateCamera()
    {
        if (!prunCamera || !g_game3.pcmMain) return;
        auto it = mprunkey.find(prunCamera);
        if (it != mprunkey.end()) UpdateCpkey3(it->second, g_game3.pcmMain, T(it->second.fRealTime));
    }
    bool FCameraKeyActive() const { return prunCamera != nullptr; }
};
GAMESCPTHOST3 s_scpthost;
std::vector<std::unique_ptr<SCPT3>> s_ascpt;

// The executor's records of the world's SCPT objects (class 0x87)
void StartupScpts3(SW* psw)
{
    s_ascpt.clear();
    s_scpthost.mpploscpt.clear();
    for (LO* plo : psw->mpcidaplo[0x87])
    {
        auto ps = std::make_unique<SCPT3>();
        ps->oid = plo->oid;
        ps->acmd = static_cast<const LOSCPT3*>(plo)->acmd;
        ps->plo = plo;
        s_scpthost.mpploscpt[plo] = ps.get();
        s_ascpt.push_back(std::move(ps));
    }
    SetScptHost3(&s_scpthost);
    if (const char* sz = std::getenv("SLY3_SCPTCMP"); sz && !s_scpthost.pfCmp) s_scpthost.pfCmp = std::fopen(sz, "w");
    InstallTitleGame3();
}

// The splice block of each group, read by the runtime (orig3 0x2499F0) into the world's splice state.
void SpliceBlockGame3(const uint8_t* pb, std::size_t ib, std::size_t ibEnd, int, const GROUP3& g)
{
    CBinaryInputStream bis;
    bis.OpenMemory(int(ibEnd), pb);
    bis.m_ib = ib;
    g_fLoadDebugInfo = g.fScriptDebug;
    // (SW +0x3F18 is the group's table while its objects load: their frames, 0x249B78; an empty block leaves none)
    EndSwSpliceGroup(g_game3.psw.get());
    LoadSwSpliceFromBrx(g_game3.psw.get(), g.x10, &bis);
}

// The SCPT class's splice methods (EOPIDs 747..754, orig3 thunks 0x108918..0x108C20: RefSetArgsFromSplice, then
// the executor's function on the script)
SCPT3* PscptOfLo(LO* plo) { return s_scpthost.PscptFromLo(plo); }
// orig3: 0x108918 (EOPID 747, 1 argument): StartScpt 0x225EE8(scpt, the argument)
CRef RefScptStart(LO* plo, const std::vector<CRef>& a)
{
    if (SCPT3* ps = PscptOfLo(plo))
    {
        LO* ploStarter = !a.empty() ? dynamic_cast<LO*>(a[0].m_pbasic) : nullptr;
        if (std::getenv("SLY3_TASKPRINT")) std::printf("splice: start SCPT %d\n", plo->oid);
        StartScpt3(ps, ploStarter);
        g_fScptRunner3 = true;
    }
    return CRef(TAGK_Void);
}
// orig3: 0x1089A0 (EOPID 748): 0x226010 = StartScpt(scpt, PjobFind(GAME, 0))
CRef RefScptStartJob(LO* plo, const std::vector<CRef>&)
{
    if (SCPT3* ps = PscptOfLo(plo))
    {
        StartScpt3(ps, nullptr);  // (TODO(sly3): PjobFind 0x1F4AD0's job 0 as the starter)
        g_fScptRunner3 = true;
    }
    return CRef(TAGK_Void);
}
// orig3: 0x108A18 (EOPID 749): StopScpt 0x226050
CRef RefScptStop(LO* plo, const std::vector<CRef>&)
{
    if (SCPT3* ps = PscptOfLo(plo)) StopScpt3(ps);
    return CRef(TAGK_Void);
}
// orig3: 0x108A90 / 0x108B18 (EOPIDs 751 / 752, 1 argument): the alarm set / cleared (0x2261A0 / 0x2261F0)
CRef RefScptSetAlarm(LO* plo, const std::vector<CRef>& a)
{
    if (SCPT3* ps = PscptOfLo(plo); ps && !a.empty()) SetScptAlarm3(ps, a[0].RefCoerceS32().m_n, true);
    return CRef(TAGK_Void);
}
CRef RefScptClearAlarm(LO* plo, const std::vector<CRef>& a)
{
    if (SCPT3* ps = PscptOfLo(plo); ps && !a.empty()) SetScptAlarm3(ps, a[0].RefCoerceS32().m_n, false);
    return CRef(TAGK_Void);
}
// orig3: 0x108BA0 (EOPID 753, 1 argument): 0x226270, the alarm is set (a bool)
CRef RefScptAlarm(LO* plo, const std::vector<CRef>& a)
{
    CRef r;
    SCPT3* ps = PscptOfLo(plo);
    r.SetBool(ps && !a.empty() && FScptAlarm3(ps, a[0].RefCoerceS32().m_n));
    return r;
}
// orig3: 0x108C20 (EOPID 754): 0x2262B0, every alarm cleared (SCPT +0x64 = 0)
CRef RefScptClearAlarms(LO* plo, const std::vector<CRef>&)
{
    if (SCPT3* ps = PscptOfLo(plo)) ps->aalarm.clear();
    return CRef(TAGK_Void);
}
// EOPID 1415 (TASK +0x44, grf 2): the task's state
bool FGetTaskState(LO* plo, CRef* pref)
{
    TASK3* p = PtaskFind3(plo->oid);
    if (!p) return false;
    pref->SetS32(p->nState);
    return true;
}
void RegisterSpliceGame3()
{
    static bool s_f = false;
    if (s_f) return;
    s_f = true;
    sly3::RegisterSpliceMethod3(0x108918, RefScptStart);
    sly3::RegisterSpliceMethod3(0x1089A0, RefScptStartJob);
    sly3::RegisterSpliceMethod3(0x108A18, RefScptStop);
    sly3::RegisterSpliceMethod3(0x108A90, RefScptSetAlarm);
    sly3::RegisterSpliceMethod3(0x108B18, RefScptClearAlarm);
    sly3::RegisterSpliceMethod3(0x108BA0, RefScptAlarm);
    sly3::RegisterSpliceMethod3(0x108C20, RefScptClearAlarms);
    sly3::RegisterSpliceGetter3(1415, FGetTaskState);
}

// The task system's host (task3.h): the task's SCPT, the per-state objects, the clock
void StartupTasks3(SW* psw)
{
    InitTasks3(psw);
    g_taskhost3.pfnTime = [] { return g_clock3.t; };
    g_taskhost3.pfnMessage = [](TASK3* p, int msg) { sly3::SendLoMessage3(p->plo, msg); };
    g_taskhost3.pfnStartScpt = [](LO* plo) {
        if (SCPT3* ps = s_scpthost.PscptFromLo(plo))
        {
            if (std::getenv("SLY3_TASKPRINT")) std::printf("task3: start SCPT %d\n", plo->oid);
            StartScpt3(ps, nullptr);
        }
    };
    // orig3: 0x185A70 ApplyTaskObjects (the objects added / removed: trigger3.h), 0x185DB0 ApplyTaskSms
    g_taskhost3.pfnApplyObjects = [psw](TASK3* p, int n) {
        ApplyTaskObjects3(psw, p, n);
        ApplyTaskSms3(psw, p, n);
    };
    // the triggers' SCPTs (+0x190) and their task pairs (ApplyCheckpointTasks3, trigger3.h)
    sly3::g_trighost3.pfnMessage = [](LO* plo, int msg) { sly3::SendLoMessage3(plo, msg); };
    sly3::g_trighost3.pfnStartScpt = [](LO* plo, LO*) {
        if (SCPT3* ps = s_scpthost.PscptFromLo(plo)) StartScpt3(ps, nullptr);
    };
}

// orig3: 0x25C2A8 (the transition's frame part) running 0x14F440 LoadSwFromBrx, then StartSw 0x25B860
bool FLoadWorld3(int iworld, std::string& error)
{
    const WORLD3* pwd = nullptr;
    const std::vector<WORLD3> aw = g_disc.Worlds();
    for (const WORLD3& w : aw)
        if (w.iworld == iworld) pwd = &w;
    if (!pwd || !pwd->fk.cb) { error = "no world file " + std::to_string(iworld); return false; }
    const auto t0 = std::chrono::steady_clock::now();
    if (g_game3.psw) DeleteSwSplice(g_game3.psw.get());
    g_game3 = GAME3{};
    InitClasses();
    g_game3.abWorld = g_disc.ReadWorld(pwd->fk);
    g_game3.psw = std::make_unique<SW>();
    g_game3.iworld = iworld;
    g_nFilterCur3 = iworld;
    LOAD3OPTS lo;
    lo.psw = g_game3.psw.get();
    lo.iworld = iworld;
    StartupSwSplice(lo.psw);
    RegisterSpliceGame3();
    sly3::InstallSpliceHooks3(lo.psw);  // (the modules' code finds the world's objects and their options)
    g_pfnSpliceBlock3 = std::getenv("SLY3_NO_SPLICE") ? nullptr : SpliceBlockGame3;
    const bool fOk = FWalkWorld3(g_game3.abWorld, g_game3.walk, error, &lo);
    g_pfnSpliceBlock3 = nullptr;
    EndSwSpliceGroup(lo.psw);
    if (!fOk) return false;
    // the renderer's scene of the same file (scene3: textures, globs, placements)
    // (its own walk: the load mode's records the objects, not the headers the scene reads)
    SCENE3STATS st;
    WORLDWALK3 walkScene;
    if (!FWalkWorld3(g_game3.abWorld, walkScene, error)) return false;
    if (!FBuildScene3(g_game3.abWorld, walkScene, g_game3.scene, st, error)) return false;
    // the lights (light3.h: the world's class-4 objects, the camera's world shader colour) and the bake / forward
    // lighting of light.cpp (SLY3_APPROX_LIGHT=1: the old stand-in)
    if (std::getenv("SLY3_APPROX_LIGHT")) ApproximateBakeLighting(g_game3.scene);
    else
    {
        UpdateWorldLights3(g_game3.psw.get());
        ApplyLighting(g_game3.scene, WorldLights());
    }
    // (checks: SLY3_LIGHTCHECK=eeMemory.bin compares the lights with a PCSX2 RAM dump's)
    if (const char* sz = std::getenv("SLY3_LIGHTCHECK"))
    {
        std::ifstream in(sz, std::ios::binary);
        const std::vector<uint8_t> ram((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        CheckLights3AgainstRam(g_game3.psw.get(), ram, true);
    }
    // the world's sound block (0x246F58: the bank, music, dialog lines; sound.cpp) for the scripts' dialog and music
    if (!std::getenv("SLY3_NO_SOUND"))
    {
        static bool s_fSound = false;
        if (!s_fSound)
        {
            g_sndhooks.pfnTGame = [] { return g_clock3.t; };
            StartupSound();
            std::atexit([] { snd_StopSoundSystem(); });
            s_fSound = true;
        }
        ResetSoundMgr();
        CBinaryInputStream bisSound;
        bisSound.OpenMemory(int(g_game3.abWorld.size()), g_game3.abWorld.data());
        bisSound.m_ib = walkScene.ibMusic;
        if (walkScene.ibMusic) LoadMusicFromBrx(&bisSound);
    }
    // StartSw 0x25B860 (the parts ported): 0x25B130, ..., NewFixedObjs 0x173C10, ..., StartupCm 0x165748
    SW* psw = lo.psw;
    NewSw8A3(psw);
    NewFixedObjs3(psw);
    StartupCm3(psw);
    StartupScpts3(psw);
    StartupTasks3(psw);
    talk3::Reset(psw);
    // (testing: SLY3_GAME_SCPT=oid starts that script at the world start, as its starter would)
    if (const char* sz = std::getenv("SLY3_GAME_SCPT"))
        for (auto& ps : s_ascpt)
            if (ps->oid == std::atoi(sz)) StartScpt3(ps.get(), nullptr), g_fScptRunner3 = true;
    // Until the Sly 3 script port's executor runs the attract script (m_ext: SCPT 4262, whose first camera command 0x45 places
    // the world camera before the first frame; PCSX2 st1 / the reference from vblank 1333), its first 0x45 is applied here.
    if (!g_fScptRunner3)
        for (LO* plo : psw->mpcidaplo[0x87])
        {
            if (plo->oid != SCPT_ATTRACT_M_EXT || iworld != orig3::WORLD_PROLOGUE) continue;
            for (const SCPTCMD3& c : static_cast<const LOSCPT3*>(plo)->acmd)
            {
                if (c.id != 0x45 || c.an.size() < 7) continue;
                float ag[7];
                std::memcpy(ag, c.an.data(), sizeof(ag));
                ApplyCmCutFromScpt3(g_game3.pcmMain, glm::vec3(ag[0], ag[1], ag[2]), glm::vec3(ag[3], ag[4], ag[5]), ag[6]);
                break;
            }
        }
    // (debug: SLY3_OPTPRINT=oid,oid... prints those objects' options)
    if (const char* sz = std::getenv("SLY3_OPTPRINT"))
        for (const char* q = sz; *q;)
        {
            const int oid = std::atoi(q);
            for (const auto& plo : psw->aploAll)
                if (plo->oid == oid) PrintObjectOptions3(plo.get(), stdout);
            while (*q && *q != ',') ++q;
            if (*q) ++q;
        }
    // (debug: SLY3_OPTPRINT_CID=cid prints the options of every object of that class)
    if (const char* sz = std::getenv("SLY3_OPTPRINT_CID"))
        for (LO* plo : psw->mpcidaplo[std::strtol(sz, nullptr, 0)]) PrintObjectOptions3(plo, stdout);
    // (debug: SLY3_SCPTPRINT=oid prints that script's commands as loaded)
    if (const char* sz = std::getenv("SLY3_SCPTPRINT"))
        for (LO* plo : psw->mpcidaplo[0x87])
            if (plo->oid == std::atoi(sz))
            {
                const LOSCPT3* pscpt = static_cast<const LOSCPT3*>(plo);
                for (std::size_t i = 0; i < pscpt->acmd.size(); ++i)
                {
                    const SCPTCMD3& c = pscpt->acmd[i];
                    std::printf("scpt %d cmd %zu id 0x%02X oid %d flag %d child %d sib %d:", plo->oid, i, c.id, c.oid, c.fFlag,
                                c.ichild, c.isib);
                    for (int32_t n : c.an)
                    {
                        float g;
                        std::memcpy(&g, &n, 4);
                        std::printf(std::fabs(g) > 1e-3f && std::fabs(g) < 1e7f ? " %d(%.2f)" : " %d", n, g);
                    }
                    std::printf("\n");
                }
            }
    // the screen objects (the Sly 3 renderer's hud3: the HUD, the prompts, the binocucom, the title) with the world's text
    {
        static TEXT3 s_text;
        s_text = TEXT3{};
        CBinaryInputStream bisText;
        bisText.OpenMemory(int(g_game3.abWorld.size()), g_game3.abWorld.data());
        bisText.m_ib = walkScene.ibText;
        if (walkScene.ibText) LoadTextFromBrx3(&s_text, 0, &bisText);
        std::string errHud;
        ClearHeadCameras3();
        if (!FInitHud3FromSw(g_game3.abWorld, psw, &s_text, errHud)) std::fprintf(stderr, "HUD: %s\n", errHud.c_str());
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("world %d %s loaded: %zu objects, %zu scene entries, %d glob sets; the camera oid %d at %.1f %.1f %.1f "
                "(%.0f ms)\n", iworld, pwd->name.c_str(), psw->aploAll.size(), g_game3.scene.aglobset.size(), st.cglobset,
                g_game3.pcmMain->plo ? g_game3.pcmMain->plo->oid : -1, g_game3.pcmMain->pos.x, g_game3.pcmMain->pos.y,
                g_game3.pcmMain->pos.z, ms);
    g_game3.sceneStats = std::move(st);
    sly3::StartWorldAnimation3(psw, &g_game3.scene, &g_game3.sceneStats);  // (the Sly 3 animation: anim3.h)
    sly3::StartTriggers3(psw);  // (the Sly 3 animation: trigger3.h)
    sly3::StartPlayer3(psw);  // (the Sly 3 player: SLY3_PLAY puts Sly in control, player3.h)
    return true;
}

// orig3: 0x27DE38 (the cameras SW +0x3EBC, each by 0x1662C0: the policy's vt calls, SetCmFov 0x167960, UpdateCmMat4
// 0x166A40). The policies are the Sly 3 player's; here the camera keeps the pose its owners set.
void UpdateCameras3(float dt)
{
    (void)dt;
    if (!sly3::FUpdatePlayerCamera3(s_scpthost.FCameraKeyActive())) s_scpthost.UpdateCamera();
    for (CM3* pcm : g_game3.apcmUpdate)
        if (pcm->tanY <= 0.0f) pcm->tanY = std::tan(glm::radians(pcm->fovDeg) * 0.5f);
}

// orig3: 0x27D7E8 (Sly 2 0x226550 UpdateSw): deferred splice events, the objects' updates, the cameras
void UpdateSw3(float dt)
{
    SW* psw = g_game3.psw.get();
    for (auto& [cid, apfn] : s_mpcidapfn)
        for (LO* plo : psw->mpcidaplo[std::size_t(cid)])
            for (auto& pfn : apfn) pfn(plo, dt);
    for (auto& pfn : s_apfnFrame) pfn(dt);
    for (auto& ps : s_ascpt)  // the SCPTs' update (vt slot 16, 0x225130)
        if (ps->fRunning) UpdateScpt3(ps.get(), dt);
    sly3::UpdateTriggers3(psw);  // the active triggers SW +0x473C (vt slot 55 0x15D510: trigger3.h)
    UpdateCameras3(dt);
}

CAMERA CameraFromCm3(const CM3& cm)
{
    CAMERA cam;
    cam.fMat = true;
    cam.mat = cm.mat;
    cam.pos = cm.pos;
    cam.fovY = glm::degrees(2.0f * std::atan(cm.tanY > 0 ? cm.tanY : std::tan(glm::radians(cm.fovDeg) * 0.5f)));
    cam.sNear = cm.sNear;
    cam.sFar = cm.sFar;
    const glm::vec3 d = cm.mat[0];
    cam.yaw = glm::degrees(std::atan2(d.y, d.x));
    cam.pitch = glm::degrees(std::asin(std::clamp(d.z, -1.0f, 1.0f)));
    return cam;
}
}  // namespace

CM3* PcmFromLo3(const LO* plo)
{
    for (auto& pcm : g_game3.acm)
        if (pcm->plo == plo) return pcm.get();
    return nullptr;
}

void RegisterUpdate3(int cid, PFNUPDATE3 pfn)
{
    for (int c = 0; c < CID_Max; ++c)
        if (FCidDerivedFrom(c, cid)) s_mpcidapfn[c].push_back(pfn);
}

void RegisterFrameHook3(std::function<void(float)> pfn) { s_apfnFrame.push_back(std::move(pfn)); }

namespace
{
std::vector<JOYPRESS> s_apressRoute;
// orig3: the main loop's pad read (0x1CB1D8 between the clock and the world update; FReadJoy's clock is Sly 3's)
// (the Sly 3 boot to gameplay: --press is the route's default; the menus' taken buttons (GrfbtnTakePressed: the card, the title,
// SCPT 0x4E) are cleared each frame by JoyBeginFrame)
void UpdateInput3()
{
    g_pclockJoy = &g_clock3;
    JoyBeginFrame();
    UpdateJoy(&g_joy, g_gl.window, g_game3.cframe, s_apressRoute);
}
}  // namespace
void SetInputRoute3(const std::vector<JOYPRESS>& apress) { s_apressRoute = apress; }

void QueueWorld3(int iworld, int point)
{
    g_game3.iworldPending = iworld;
    g_game3.pointPending = point;
}

namespace
{
int RunWorldLoop3(const GAME3OPTS& o);
}

int RunGame3(const GAME3OPTS& o)
{
    // orig3: 0x1C6EA8 / 0x1C6F30 (boot3.h, the Sly 3 script port): the two logo movies, the card check, then the transition to
    // world 35 (m_ext) at 0x1A8, which lands here: the main loop with that world queued
    if (o.fSkipLogos || o.iworld >= 0)
    {
        // (the boot's card check (kind 5) stays: the logos only are skipped; not with SLY3_PLAY, which starts at the
        // gameplay after the binocucom talk: player3.h)
        if (o.iworld < 0 && !sly3::FPlayEnabled3())
        {
            SetCardState3(g_card3, 1);
            InstallCardGame3();
        }
        QueueWorld3(o.iworld >= 0 ? o.iworld : orig3::WORLD_PROLOGUE, orig3::WORLD_BOOT_ARG);
        return RunWorldLoop3(o);
    }
    g_pfnBootTransition3 = [&o](const TRANSREQ3& req) {
        QueueWorld3(req.iworld, req.point);
        return RunWorldLoop3(o);
    };
    const int err = Boot3(o.fHeadless);
    g_pfnBootTransition3 = nullptr;
    return err;
}

namespace
{
// orig3: 0x1CB1D8 (the main loop), from the boot's world transition on
int RunWorldLoop3(const GAME3OPTS& o)
{
    std::string error;
    if (!g_gl.FInit(o.width, o.height, !o.fHeadless, "Sly3-Release", error)) { std::fprintf(stderr, "%s\n", error.c_str()); return 1; }
    if (o.fHeadless || o.fFixedDt) g_dtFixedStep = DT_FRAME_NTSC;
    if (o.fHeadless || o.fFixedDt)  // (the sound's clock and the dialog lines' ends by the frame: runs faster than real time)
    {
        g_fSndFrameClock = true;
        setenv("SLY2N_FASTDIALOG", "1", 0);
    }
    ResetClock(&g_clock3);
    std::string dirDump;
    int cframeEvery = 1;
    if (!o.frameDump.empty())
    {
        dirDump = o.frameDump;
        if (const std::size_t colon = dirDump.rfind(':'); colon != std::string::npos && colon > 1)
        {
            cframeEvery = std::max(1, std::atoi(dirDump.c_str() + colon + 1));
            dirDump.resize(colon);
        }
        std::filesystem::create_directories(dirDump);
    }
    RENDEROPTS opts;
    SetInputRoute3(o.apress);  // (--press; SLY3_ROUTE replaces it at the player's start)
    for (int iframe = 0; o.cframe < 0 || iframe < o.cframe; ++iframe)
    {
        if (!o.fHeadless)
        {
            glfwPollEvents();
            if (glfwWindowShouldClose(g_gl.window)) break;
        }
        // the transition (orig3 0x25C2A8): a queued world loads at the frame's start
        if (g_game3.iworldPending >= 0)
        {
            const int iworld = g_game3.iworldPending;
            if (!FLoadWorld3(iworld, error)) { std::fprintf(stderr, "world %d: %s\n", iworld, error.c_str()); g_gl.Terminate(); return 1; }
            BuildSceneGpu(g_game3.scene);
            g_game3.fSceneGpu = true;
            opts.rgbaFog = g_condset3.rgbaFog;
            opts.sFogNear = g_condset3.fogNear;
            opts.sFogFar = g_condset3.fogFar;
            opts.uFogMax = g_condset3.fogMax;
            opts.fFog = true;
            ResetClock(&g_clock3);
            RestoreTasks3();   // orig3: StartSw's RestoreTask 0x188CB0 per task (the reference: vblank 1323..1329)
            if (!o.census.empty())
                if (FILE* pf = std::fopen(o.census.c_str(), "w"))
                {
                    PrintObjectCensus3(g_game3.psw.get(), pf);
                    std::fclose(pf);
                }
        }
        // (SLY3_TASKAT=F,F..: the task keys and states at those frames, as tools/tasks3.py prints a RAM dump's)
        if (const char* sz = std::getenv("SLY3_TASKAT"))
            for (const char* q = sz; *q;)
            {
                if (std::atoi(q) == iframe) { std::printf("frame %d ", iframe); PrintTasks3(stdout); }
                while (*q && *q != ',') ++q;
                if (*q) ++q;
            }
        // (tests: SLY3_TASKREQ="FRAME:OID=STATE;..." asks a task for a state at a frame, as a script's 0x1E would)
        if (const char* sz = std::getenv("SLY3_TASKREQ"))
            for (const char* q = sz; *q;)
            {
                int f = -1, oid = -1, n = 0;
                if (std::sscanf(q, "%d:%d=%d", &f, &oid, &n) == 3 && f == iframe)
                {
                    const int apair[2] = {oid, n};
                    ApplyCheckpointTasks3(apair, 1, nullptr);
                    if (std::getenv("SLY3_TASKPRINT")) PrintTasks3(stdout);
                }
                while (*q && *q != ';') ++q;
                if (*q) ++q;
            }
        UpdateClock(&g_clock3, false);              // orig3: 0x165430
        UpdateInput3();                             // the pads (FReadJoy)
        UpdateSw3(g_clock3.dt);                     // orig3: 0x27D7E8
        talk3::Update(g_game3.cframe);              // (the interim binocucom's state, talk3.h)
        talk3::ShowTitleScreen(FTitleShowing3());
        // Sly in control (interim, TODO(sly3): the player's own control lock and camera policy decide it): a job is
        // current, the title is gone, the binocucom closed, no script camera key and no script lock on the player
        if (!sly3::g_fPlayGame3 && !sly3::FPlayEnabled3() && g_tasks3.pjobCur && !FTitleShowing3() && talk3::BinocState(nullptr) == -1 &&
            !s_scpthost.FCameraKeyActive() && s_scpthost.clock == 0 && g_game3.cframe > 0)
        {
            sly3::g_fPlayGame3 = true;
            std::printf("game3: frame %d Sly in control\n", g_game3.cframe);
            sly3::StartPlayer3(g_game3.psw.get());
            talk3::ShowGameplayHud();
        }
        if (!std::getenv("SLY3_NO_SOUND")) UpdateSoundMgr(g_clock3.dtReal > 0 ? g_clock3.dtReal : g_clock3.dt, DT_FRAME_NTSC);
        if (std::getenv("SLY3_SNDPRINT") && iframe % 60 == 0)
            std::printf("sound: frame %d dialog %d t %.2f: %s\n", iframe, IdDialogCur(), TDialog(), snd_Describe().c_str());
        UpdateHud3(g_clock3.dtReal > 0 ? g_clock3.dtReal : g_clock3.dt);  // the screen objects (hud3.h; on the real clock: the card stops the game's)
        ++g_game3.cframe;
        if (!o.fHeadless)
        {
            int fbw, fbh;
            glfwGetFramebufferSize(g_gl.window, &fbw, &fbh);
            if (fbw > 0 && fbh > 0 && (fbw != g_display.outputWidth || fbh != g_display.outputHeight)) LayoutDisplay(fbw, fbh);
        }
        // render (orig3 0x182C68 open, 0x209C40 RenderSw, 0x175268 DrawRpl, 0x182EE0 close) from SW +0x463C
        UpdateShaderAnimations(g_clock3.t, g_clock3.dt);
        // the lights move with their objects; the forward-lit groups take their light context again (as Sly 2's loop)
        if (!std::getenv("SLY3_APPROX_LIGHT"))
        {
            UpdateWorldLights3(g_game3.psw.get());
        }
        BindHeadTargets3(g_game3.scene);            // (the binocucom's heads: hud3.h)
        if (!std::getenv("SLY3_APPROX_LIGHT"))
            RelightForward(g_game3.scene, WorldLights(), true, AlightsHeadTargets3(WorldLights().worldShader));
        // the particles (emitter.cpp with Sly 3's emitters 0x1B, options mapped to Sly 2's): the placed objects' world
        // matrices, the camera, one step (SLY3_NO_PARTICLES=1: off)
        if (static const bool s_fOff = std::getenv("SLY3_NO_PARTICLES") != nullptr; !s_fOff)
        {
            const CAMERA camP = CameraFromCm3(*g_game3.pcmMain);
            UpdateLoWorlds3(g_game3.psw.get());
            SetParticleCamera(camP.pos, camP.Dir(), glm::normalize(glm::cross(glm::vec3(0, 0, 1), camP.Dir())));
            StepParticles(g_game3.psw.get(), 1, g_clock3.dt);
        }
        RenderScene(g_game3.scene, CameraFromCm3(*g_game3.pcmMain), opts);
        DrawHud3(g_gl.renderWidth, g_gl.renderHeight);
        const bool fLast = o.cframe >= 0 && iframe + 1 == o.cframe;
        if (!dirDump.empty() && iframe % cframeEvery == 0)
        {
            char sz[64];
            std::snprintf(sz, sizeof(sz), "/frame%05d.png", iframe);
            g_gl.FWriteScreenshot(dirDump + sz);
        }
        if (fLast && !o.screenshot.empty())
        {
            glFinish();
            const bool fOk = g_gl.FWriteScreenshot(o.screenshot);
            std::printf("%s %s (frame %d, t %.3f): %d draws\n", fOk ? "wrote" : "FAILED to write", o.screenshot.c_str(), iframe,
                        g_clock3.t, g_renderstats.cdraw);
        }
        g_gl.Present();
        if (!o.fHeadless) cane::gl::SwapBuffers(g_gl.window);
    }
    if (std::getenv("SLY3_TASKPRINT")) PrintTasks3(stdout);
    const CM3& cm = *g_game3.pcmMain;
    std::printf("ran %d frames (t %.3f): camera oid %d at %.1f %.1f %.1f\n", g_game3.cframe, g_clock3.t, cm.plo ? cm.plo->oid : -1,
                cm.pos.x, cm.pos.y, cm.pos.z);
    // the world's unload while the globals it touches are alive (not at static destruction)
    DeleteSwSplice(g_game3.psw.get());
    sly3::InstallSpliceHooks3(nullptr);
    SetScptHost3(nullptr);
    s_ascpt.clear();
    g_game3.acm.clear();
    g_game3.psw.reset();
    g_psw3 = nullptr;
    g_gl.Terminate();
    return 0;
}
}  // namespace
