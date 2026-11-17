// player3.cpp: Sly 3's player in the native game (player3.h): the development start in gameplay and the per-frame
// trace. The player's own code is the copied Sly 2 player (sly.cpp, actor.cpp, step*.cpp, jump.cpp ...) dispatched on
// Sly 3's classes through vtmap3.cpp, with the Sly 3 differences ported from the Sly 3 asm (orig3 comments).
#include "player3.h"
#include "game/guru3.h"
#include "codepanel3.h"
#include "outfit3.h"
#include "combo3.h"
#include "binoc3.h"
#include "brx.h"

#include "game/sly3.h"
#include "game/scptgame3.h"
#include "game/trigger3.h"
#include "interact.h"
#include "jtthrow.h"
#include "reach.h"
#include "activate.h"
#include "jump.h"
#include "actorai.h"
#include "aijobs.h"
#include "ai.h"
#include "aistack.h"
#include "binoc.h"
#include "jump.h"

#include "actor.h"
LO* g_ploStartPlaced3 = nullptr;   // (dev harness SLY3_START: the actor it placed and the frame)
int g_iframeStartPlaced3 = -100;
extern int g_iframeMain;
#include "activate.h"
#include "alo.h"
#include "clock.h"
#include "cm.h"
#include "crv.h"
#include "jt.h"
#include "lo.h"
#include "po.h"
#include "gang.h"
#include "sly.h"
#include "sm.h"
#include "smrun.h"
#include "asega.h"
#include "aseg.h"
#include "cls.h"
#include "so.h"
#include "step.h"
#include "stepphys.h"
#include "sw.h"
#include "update.h"
#include "worldaccess.h"
#include "xf.h"

#include <cmath>
#include <unordered_map>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>

void LaunchActorTo(ACTOR* pa, const glm::vec3& posT, float g, float h);  // pad.cpp: orig 0x141968

namespace
{
// orig3 0x1566D0: the actor's jump kind (vt slot 0x320: Sly 2's slot 89) is a script animation's (5 / 6)
bool FActorInScriptAnim3(ACTOR* pa) { return static_cast<unsigned>(ActorJumpKind(pa) - 5) < 2; }
// orig3 0x156690: the script animation over: the jump kind 0
void EndActorScriptAnim(ACTOR* pa) { if (FActorInScriptAnim3(pa)) NotifyActorJumpKind(pa, 0); }
// (the commands' wait, 0x1566D0: in kind 5 / 6. The actors' own states (Sly 0x13, Bentley 10, Murray 17) end the
// kind when the clip is over; those state codes are not ported natively, so the clip's end (retracted, or its SM
// retracted) ends it here, as scpt.cpp's FActorScriptAnim does for Sly 2's Sly. A looping clip holds the kind.)
// (the POs that are not native actors (e.g. the Panda King 0x38, whose own states are not ported): the script's clip
// kept here per object; its wait holds until the clip retracts, as the actors' script-animation state does)
std::unordered_map<const LO*, ASEGA*>& MpScriptAnimPo()
{
    static std::unordered_map<const LO*, ASEGA*> s_mp;
    return s_mp;
}
bool FPoScriptAnim(LO* plo)
{
    auto& mp = MpScriptAnimPo();
    auto it = mp.find(plo);
    if (it == mp.end()) return false;
    if (it->second && std::find(g_apasega.begin(), g_apasega.end(), it->second) != g_apasega.end() && !FAsegaRetracted(it->second)) return true;
    mp.erase(it);
    return false;
}
void EndPoScriptAnim(LO* plo)
{
    auto& mp = MpScriptAnimPo();
    auto it = mp.find(plo);
    if (it == mp.end()) return;
    if (it->second && std::find(g_apasega.begin(), g_apasega.end(), it->second) != g_apasega.end()) RetractAsegaPp(&it->second);
    mp.erase(it);
}

bool FActorScriptAnim(LO* ploActor)
{
    ACTOR* pa = PactorFromLo(ploActor);
    if (!pa) return FPoScriptAnim(ploActor);
    if (!pa || !FActorInScriptAnim3(pa)) return false;
    const bool fOver = pa->psma ? FSmaRetracted(pa->psma) : (!pa->pasega || FAsegaRetracted(pa->pasega));
    if (!fOver) return true;
    NotifyActorJumpKind(pa, 0);
    return false;
}
}  // namespace
LO* PloFindSwChild(LO* ploCtx, OID oid);

extern int g_iframeMain;   // (game.cpp)

bool FGrappleCam3(const LO* plo);   // game/grapplecam3.cpp (Bentley's Grapple-Cam, class 0x56)

namespace sly3
{
namespace
{
FILE* s_ptrace = nullptr;
}

LO* PloSly3(SW* psw)
{
    if (!psw) return nullptr;
    const auto& a = psw->mpcidaplo[S3CID(0x35)];
    return a.empty() ? nullptr : a[0];
}

// The reference at "Sly in control" (Sly3-research/ref3/boot v.5600.ram; PCSX2 st4 to 0.01): Sly's root record
// (Sly +0x48 -> +0x70) and his heading (STEP +0x1BC).
const glm::vec3 c_posSpawn(-6713.0708f, -14376.3467f, -2913.2537f);
constexpr float c_radSpawn = 1.20430f;

// SLY3_JTLIST=1: the world's player targets (ledges, pipes, spires, ... : the Sly 3 classes of the JT kinds) and what
// the copied Sly 2 lists (jt.cpp AploJtList) hold
void ListJt3(SW* psw)
{
    if (std::getenv("SLY3_JTLIST")[0] == '2')
        for (int cid : {0x0A, 0x0B, 0x0C, 0x19, 0x20, 0x21, 0x24, 0x25, 0x26})
            for (LO* p : psw->mpcidaplo[cid])
            {
                std::printf("jt3: all cid 0x%02X oid %6d inworld %d removed %d st %d:", cid, p->oid, p->fInWorld, p->fRemoved, p->st);
                for (LO* q = p->paloParent; q; q = q->paloParent)
                    std::printf(" <- %d (cid 0x%X inworld %d removed %d)", q->oid, q->Cid(), q->fInWorld, q->fRemoved);
                std::printf("\n");
            }
    static const char* c_aszJtl[JTL_Max] = {"point", "pipe", "rail", "spire", "hide", "ledge", "wall", "swing", "use", "response", "climbsurf", "crawlhook"};
    for (int j = 0; j < JTL_Max; ++j)
        for (LO* p : AploJtList(psw, static_cast<JTL>(j)))
        {
            const glm::vec3 pos(MatJtLo(p)[3]);
            const JTDESC d = JtDescOf(p, static_cast<JTL>(j));
            std::printf("jt3: %-9s cid 0x%02X oid %6d at %9.1f %9.1f %9.1f mask 0x%X id %d prio %d\n", c_aszJtl[j], p->Cid(), p->oid, pos.x, pos.y, pos.z,
                        d.mask, d.id, d.prio);
            if (std::getenv("SLY3_JTLIST")[0] == '3')  // (3: the object's stored option fields)
                for (const auto& [key, u] : p->afield)
                {
                    float g;
                    std::memcpy(&g, &u, 4);
                    std::printf("jt3:     field getter 0x%X +0x%X = 0x%X (%g)\n", unsigned(key >> 32), unsigned(key), u, g);
                }
            if (const CRV* pcrv = PcrvFromLo(p); pcrv && !pcrv->apos.empty())
                for (const glm::vec3& v : pcrv->apos)
                {
                    const glm::vec3 w(pcrv->mat * glm::vec4(v, 1.0f));
                    std::printf("jt3:     point %9.1f %9.1f %9.1f\n", w.x, w.y, w.z);
                }
        }
}

// orig3: SCPT 0x22's placement (0x226D00: the actor's vt slot 43 with the rotation about z and the position): a stepping
// object (STEP, Sly / Bentley / Murray / the guards) takes it through its own rotation (Sly 2's slot 30 for STEP:
// the step heading +0x1BC follows the rotation), so its heading is the script's and does not snap back to
// the old one (the reference: Sly's +0x1BC 1.2043 from the attract's placement on; natively the record was edited and
// the step heading stayed 0). Natively the record is placed by s3-script's PlaceLo3, then the step heading set.
void PlaceLoScript3(LO* plo, const float mat[9], const float pos[3])
{
    if (!plo) return;
    PlaceLo3(plo, mat, glm::vec3(pos[0], pos[1], pos[2]));
    if (STEP* p = PstepFromLo(plo))
    {
        // (as SetStepRotation after the record's placement: the step heading from the new rotation, the pad's too)
        SetStepPhysHeading(p->palo, std::atan2(mat[1], mat[0]));
        if (PODATA* ppo = PpodataFromLo(p->palo); ppo && ppo->ppadctl) ppo->ppadctl->radJoy = p->radHeading;
    }
    // (debug: SLY3_PLACEPRINT=1: the script's heading and the object's world heading after the placement)
    if (static const bool s_f = std::getenv("SLY3_PLACEPRINT") != nullptr; s_f)
    {
        const glm::mat4 w = sly3::MatLoWorld3(plo);
        glm::mat4 o = w;
        if (FIsLoDerivedFrom(plo, sly3::CID_ALO))
            if (const XF* px = PxfObj(static_cast<ALO*>(plo))) o = px->matWorld;
        std::printf("place3: %d heading %.3f world %.3f at %.0f %.0f %.0f; object %.3f at %.0f %.0f %.0f\n", plo->oid, std::atan2(mat[1], mat[0]),
                    std::atan2(w[0][1], w[0][0]), w[3].x, w[3].y, w[3].z, std::atan2(o[0][1], o[0][0]), o[3].x, o[3].y, o[3].z);
    }
}

// orig3: SCPT 0x22 after the placement (0x226D00 case 34: when the command's first flag is 0 and the actor is not held
// for a talk (+0x13EC, Sly 2's +0x12F4): 0x1B7268 SetJumpKind(jump +0x320, 11) and 0x23DF78 (Sly 2's 0x1F4E28: the
// contacts detached), as Sly 2's 0x1E26A8; the camera target at the new place)
void ResetActorAfterPlace3(LO* plo)
{
    ACTOR* pa = PactorFromLo(plo);
    if (!pa) return;
    pa->posCmTarget = glm::vec4(PosLoWorld(plo) + glm::vec3(0, 0, 1) * pa->sHeight, 1.0f);
    if (CActorTalkHold(plo) != 0) return;
    // (s3-script: an actor whose class is not initialised natively yet (the Guru 0x37 in o_ext: no InitJump) has no
    // jump owner: SetJumpKind crashed on it when a script placed him (Episode 2's SCPT 2664 / 2789))
    if (!pa->jump.pactor) return;
    SetJumpKind(&pa->jump, 0xB, 0, nullptr);   // 0x1B7268
    if (pa->palo) DetachAloContactsAll(pa->palo);   // 0x23DF78
}

namespace
{
bool FSlyNotPlayed(ACTOR* pa)
{
    if (std::getenv("SLY3_NOTPLAYEDWALK")) return false;   // (debug: the jobs pushed as before)
    return dynamic_cast<SLY*>(pa) && g_psw && pa->palo != PloPoCurrent(g_psw);
}
void PlaceNotPlayed(ACTOR* pa, glm::vec3 pos)
{
    FDropOnGround(&pos, 100.0f, -350.0f, true);
    const glm::mat4 m = sly3::MatLoWorld3(pa->palo);
    float mat[9];
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 3; ++c) mat[r * 3 + c] = m[r][c];
    sly3::PlaceLo3(pa->palo, mat, pos);
    if (std::getenv("SLY3_SCPTPRINT")) std::printf("player3: 0x33 / 0x34 of %d (not played): placed at %.0f %.0f %.0f\n", pa->palo->oid, pos.x, pos.y, pos.z);
}
}  // namespace

// orig3: SCPT 0x33 / 0x34 on a player actor (0x226D00: after ClearJobs on the actor's job stack +0x29C, the path-step
// job 0xF3 on a PATH, or the step-to job 0xF4 toward an object's aim: orig3 0x157880.. = Sly 2's JOBACTORWALK 0xC1 /
// JOBACTORFOLLOW 0xA8, the copied actorai.cpp; the jobs keep Sly 2's ids inside the AI code). As Sly 2's scpt.cpp
// 0x27 / 0x28. The "no avoid" flag is the run's +0x104 (0x31 sets it, 0x32 clears it): without it the path point was
// clipped to the nav region and an actor stopped where the region ends (the gauntlet's Bentley, SCPT 2290 [13]).
void ActorPathStep3(LO* ploActor, LO* ploPath, int b1, int b2, float g3, int f104)
{
    ACTOR* pa = PactorFromLo(ploActor);
    const CRV* pcrv = ploPath ? PcrvFromLo(ploPath) : nullptr;
    if (!pa || !pcrv) return;
    ClearJobs(pa);
    NotifyActorJumpKind(pa, 0);   // (orig3 0x226D00 case 51: the actor's vt slot 99 with kind 0: a script clip's state left)
    // orig3 0x226D00 case 51: Sly 3's path step 0xF3 (+0x30 the path, +0x58 the run's +0x104 (0x31 sets it, 0x32 clears
    // it), +0x34 / +0x5C / +0x60 the command's words 1..3), entered (actorai.h JOBPATHSTEP3: steps of 200 along the path,
    // each walked by a 0xF4). (s3-integrate: before, one A8 follower on the whole curve, Sly 2's way: avoidance and the
    // nav keep held Murray off h_ext's platform path, SCPT 3282 [28], where the reference walks him to its end.)
    // SLY3_PATHSTEPA8=1 (debug): the old follower.
    if (static const bool s_fOld = std::getenv("SLY3_PATHSTEPA8") != nullptr; s_fOld)
    {
        auto* pfollow = static_cast<JOBACTORFOLLOW*>(PjobPush(pa, 0xA8));
        pfollow->fNoAvoid = f104;
        pfollow->fWalk = b2;
        pfollow->uSpeedActor = g3;
        pfollow->Follow(pcrv, b1 == 0);
        pfollow->Enter();
        pfollow->sArrive = 40.0f;
        pfollow->sNear = std::min(pfollow->sNear, pfollow->sArrive * 0.25f);
        pfollow->sFar = std::min(pfollow->sFar, pfollow->sArrive);
        return;
    }
    // (Sly while another character is played: natively only the current player's states run, so his walk never
    // moved (ep1_heist_end's SCPT 2489 [81]: the boss intro waited at [85] until the task was forced): placed at the
    // path's end, as SCPT 0x36 / 0x30 place an actor the native code cannot move; no job left, the run's wait is over)
    if (FSlyNotPlayed(pa))
    {
        glm::vec3 pos(0.0f);
        EvalCrvS(pcrv, b1 != 0 ? SLengthCrv(pcrv) : (pcrv->as.empty() ? 0.0f : pcrv->as.front()), &pos, nullptr);
        PlaceNotPlayed(pa, pos);
        return;
    }
    auto* pstep = static_cast<JOBPATHSTEP3*>(PjobPush(pa, CID_JOBPATHSTEP3));
    pstep->pcrv = pcrv;
    pstep->fForward = b1 != 0;
    pstep->fNoAvoid = f104;
    pstep->fWalk = b2;
    pstep->uSpeed = g3;
    pstep->Enter();
}

void ActorStepTo3(LO* ploActor, const float pos[3], float g1, int b2, float g3, int f104)
{
    ACTOR* pa = PactorFromLo(ploActor);
    if (!pa) return;
    ClearJobs(pa);
    NotifyActorJumpKind(pa, 0);   // (orig3 0x226D00 case 52: as case 51)
    if (FSlyNotPlayed(pa)) { PlaceNotPlayed(pa, glm::vec3(pos[0], pos[1], pos[2])); return; }
    auto* pwalk = static_cast<JOBACTORWALK*>(PjobPush(pa, 0xC1));
    pwalk->fSly3 = 1;     // (Sly 3's 0xF4: 0x157970's region rule)
    pwalk->fRun = f104;   // (case 52: the job's +0x30)
    pwalk->sArrive = g1;
    pwalk->fWalk = b2;
    pwalk->uSpeed = g3;
    pwalk->SetGoal(glm::vec3(pos[0], pos[1], pos[2]));
    pwalk->Enter();
}

// (the wait of 0x33 / 0x34: no job left, or its done test 0x1577C0 / 0x157950; then the jobs cleared)
bool FActorJobDone3(LO* ploActor, int kind)
{
    ACTOR* pa = PactorFromLo(ploActor);
    if (!pa) return true;
    bool fDone;
    if (kind == 0xF3 && PjobFind(pa, CID_JOBPATHSTEP3))
    {
        const auto* pstep = static_cast<const JOBPATHSTEP3*>(PjobFind(pa, CID_JOBPATHSTEP3));
        fDone = pstep->FDone();
        if (static const bool s_f = std::getenv("SLY3_PATHPRINT") != nullptr; s_f && (fDone || std::getenv("SLY3_PATHPRINTALL")))
        {
            const glm::vec3 pos = PosLoWorld(ploActor);
            std::printf("pathstep: actor %d t %.3f done %d s %.1f end %.1f at %.1f %.1f %.1f goal %.1f %.1f %.1f\n", ploActor->oid, g_clock.t, int(fDone),
                        pstep->s, pstep->fForward ? SLengthCrv(pstep->pcrv) : 0.0f, pos.x, pos.y, pos.z, pstep->pos.x, pstep->pos.y, pstep->pos.z);
        }
    }
    else if (kind == 0xF3)
    {
        const auto* pfollow = static_cast<const JOBACTORFOLLOW*>(PjobFind(pa, 0xA8));
        fDone = !pfollow || pfollow->FArrived();
        if (static const bool s_f = std::getenv("SLY3_PATHPRINT") != nullptr; s_f && pfollow && (fDone || std::getenv("SLY3_PATHPRINTALL")))
        {
            const glm::vec3 pos = PosLoWorld(ploActor);
            std::printf("pathstep: actor %d t %.3f done %d st %d s %.1f end %.1f at %.1f %.1f %.1f end %.1f %.1f %.1f arrive %.1f reverse %d open %d\n", ploActor->oid, g_clock.t, int(fDone),
                        pfollow->st, pfollow->s, pfollow->sEnd, pos.x, pos.y, pos.z, pfollow->posEnd.x, pfollow->posEnd.y, pfollow->posEnd.z, pfollow->sArrive, pfollow->fReverse, pfollow->fOpen);
        }
    }
    else
    {
        const auto* pwalk = static_cast<const JOBACTORWALK*>(PjobFind(pa, 0xC1));
        fDone = !pwalk || pwalk->FAtGoal();
    }
    if (fDone) ClearJobs(pa);
    return fDone;
}

// orig3: 0x24E920 FaceObject (SCPT 0x77: the step object's heading 0x24E878 toward the object's position, through
// 0x24E8E8's angle of the difference 0x1205C8). No hook was set: v_interpol's caught scene (SCPT 2101 [7]) left
// Carmelita 28 facing the wall, so the key in her frame sat behind her.
void FaceObject3(LO* ploActor, LO* plo)
{
    if (!ploActor || !plo || !FIsLoDerivedFrom(ploActor, S3CID(0x30))) return;
    const glm::vec3 d = glm::vec3(sly3::MatLoWorld3(plo)[3]) - glm::vec3(sly3::MatLoWorld3(ploActor)[3]);
    if (d.x == 0.0f && d.y == 0.0f) return;
    SetStepPhysHeadingToward(static_cast<ALO*>(ploActor), d);
}

// orig3: the actor's own script commands (SCPT 0x2A..0x2E, 0x35, 0x37; the executor resolves the arguments and keeps
// the run pending on the actor). Their bodies are the actor functions Sly 2's executor called (scpt.cpp's 0x1E, 0x1F,
// 0x20, 0x21, 0x22, 0x2B), found again by the function map:
//   0x2A 0x154C80 = 0x142278 (the object handed over: the throw controller's put-down); done 0x154ED0 = 0x1424C0
//   0x2B 0x154D40 = 0x142340 (interact kind 9);                                         done 0x154EE0 = 0x1424D0
//   0x2C 0x154BA8 = 0x1421A0 (the reach ended, the object held / let go; not waited on)
//   0x2D 0x154C00 = 0x1421F8 (interact kind 5, the held object put there);              done 0x154DA0 = 0x1423A0
//   0x2E 0x154EF0 = 0x1424E0 (the prize interaction, kind 12);                          done 0x154F40 = 0x142530
//   0x35 (pending only): done when the actor's interact kind 8 has ended
//   0x37 0x153F68 = 0x141968 (the actor launched to a point; not waited on)
//   0x6A 0x172C30 + 0x172AE8 on the actor's +0x1108 object (the outfits, outfit3.cpp): the outfit n worn with the
//        change's animation; done when the change is over (+0x48 == -1)
// TODO(s3-gadgets): 0x74 (orig3 0x226D00 case 116, Bentley only): his item 0xD (0x1400C0) lets go of its object
// (0x1FC0E0, 0x1AF2A8), the landed object +0x19B8 returned to its proxy, vt slot 114 (0xD), then the controller +0x1290
// aimed at the object (0x273030: in its states 4..6 the flight from +0xF0 to the object's position at speed +0x28);
// the done test (case 116: 10 s at most; states 4 / 5 / 6 until +0x13C8 is 0, else +0x19B8's SM in 0x92 / 0xA8).
// These are the Grapple-Cam's states (claimed by s3-gadgets).
void ActorCmd3(int id, LO* ploActor, LO* ploA, LO* ploB, int n, int n2, const float pos[3])
{
    ACTOR* pa = PactorFromLo(ploActor);
    if (!pa) return;
    switch (id)
    {
    case 0x2A:
        if (!ploA) return;
        SwitchLoFaded(ploA, true, 0.25f);
        pa->jtthrow.selected = ploA;
        if (ploB) pa->jtthrow.targetWorld = FIsLoDerivedFrom(ploB, S3CID(0x30)) ? PosAloCenter(static_cast<ALO*>(ploB)) : glm::vec3(pos[0], pos[1], pos[2]);
        UpdateJtThrowArc(pa, false);
        NotifyActorJumpKind(pa, 7);
        SetJtThrowState(pa, 7);
        break;
    case 0x2B: SetActorInteractObject(pa, 9, ploA); break;
    case 0x2C:
        SetActorInteractObject(pa, 5, nullptr);
        if (ploA) AttachReachObject(&pa->inter.reach, ploA);
        else ReleaseReachObject(&pa->inter.reach);
        break;
    case 0x2D:
        SetActorInteractObject(pa, 5, ploA);
        if (ploA) PlaceReachObject(&pa->inter.reach, ploA);
        break;
    case 0x2E: StartPrizeInteract(pa, dynamic_cast<ASEG*>(ploA), ploB, n); break;
    case 0x6A: FWearOutfit3(ploActor, n); break;
    case 0x6D: sly3::GuruJumpOff3(ploActor); break;   // (the Guru: game/guru3.cpp)
    case 0x37:
    {
        float g, h;
        std::memcpy(&g, &n, 4);
        std::memcpy(&h, &n2, 4);
        ::LaunchActorTo(pa, glm::vec3(pos[0], pos[1], pos[2]), g, h);
        break;
    }
    default: break;
    }
}

bool FActorCmdDone3(int id, LO* ploActor)
{
    ACTOR* pa = PactorFromLo(ploActor);
    if (!pa) return true;
    switch (id)
    {
    case 0x2A: return pa->jtthrow.state == -1;
    case 0x2B: return pa->n4C8 != 9;
    case 0x2D: return FActorPoseDone(pa);
    case 0x2E: return pa->n4C8 != 12;
    case 0x35: return pa->n4C8 != 8;
    case 0x6A: return !FOutfitChanging3(ploActor);
    default: return true;
    }
}


// orig3 0x156578 (SCPT 0x16 / 0x30 on an actor: the script's animation): oid -1 ends it (0x156690); else the object
// found from the actor (0x17E1B8(SW, 1, oid, actor)): an ASEG (0x82): the jump kind n (vt slot 0x318: Sly 2's slot 88),
// the SMA +0x1140 cleared (0x23AC58), the ASEG applied as the current one +0x113C (0x137588(aseg, actor, 0, 1, grf));
// an SM (0x84): the kind, the ASEG retracted (0x137628), the SM run on +0x1140 (0x23AAE8(sm, actor, -1, grf)). The
// same code as Sly 2's 0x143EE0 with Sly 3's classes. Unhooked, every script animation of an actor ended at once (e.g.
// m4_rccar's SCPT 69353: Murray harnessed to the fireworks cart 69350 for the race was let go the next frame).
void PlayActorScriptAnim3(LO* ploActor, int oidAnim, int grf, int n)
{
    ACTOR* pa = PactorFromLo(ploActor);
    if (!pa)
    {
        // (a PO that is no native actor, e.g. c_intro's Panda King 30 (class 0x38) in SCPT 3382: its clip applied on it
        // and kept for the wait (FPoScriptAnim); before, nothing played and the wait ended at once: the scene went on
        // the next frame. TODO(sly3): the class's own states (its slots 99 / 100))
        if (oidAnim == -1) { EndPoScriptAnim(ploActor); return; }
        if (n == 6 || !FIsLoDerivedFrom(ploActor, sly3::CID_ALO)) return;
        LO* plo = ::PloFindSwChild(ploActor, oidAnim);
        if (std::getenv("SLY3_SCPTPRINT")) std::printf("player3: script anim %d on PO %d (cid 0x%X) grf %d kind %d\n", oidAnim, ploActor->oid, plo ? plo->Cid() : 0, grf, n);
        if (!plo || !FCidDerivedFrom(plo->Cid(), 0x82)) return;
        ASEGA*& r = MpScriptAnimPo()[ploActor];
        if (r && std::find(g_apasega.begin(), g_apasega.end(), r) == g_apasega.end()) r = nullptr;
        ApplyAsegCur(static_cast<ASEG*>(plo), static_cast<ALO*>(ploActor), 0.0f, 1.0f, grf, &r);
        return;
    }
    if (oidAnim == -1)
    {
        ::EndActorScriptAnim(pa);
        return;
    }
    // (kind 6: the scripted walks' clip (0x33 / 0x34): the native walk (ActorPathStep3 / ActorStepTo3) drives and
    // animates the actor itself; the kind would take the actor out of it (ep2_job2730's 2744 / ep2_job2663's 1806 then
    // stalled). Left to the walk, as before the hooks.)
    if (n == 6) return;
    // (the Guru as the current player too: his state 8 (guru3.cpp, orig3 0x22CF00 kind 5) holds him while the script's
    // clip plays, as the reference does (warp 8:1806: state 8 at vblanks 1748..1966 and 2161..2512, SCPT 2664's talks;
    // Cross pressed then does nothing). Skipped before, he stood in state 0 with his buttons read and jumped on the
    // harness's dialog presses during the talks (ep2_job2663 2673 / 2674 forced in round 19).)
    if (std::getenv("SLY3_GURUNOSCRIPTANIM") && g_psw && ploActor == PloPoCurrent(g_psw) && ploActor->Cid() == sly3::CID_GURU) return;
    LO* plo = ::PloFindSwChild(ploActor, oidAnim);
    if (std::getenv("SLY3_SCPTPRINT")) std::printf("player3: script anim %d on %d (cid 0x%X) grf %d kind %d\n", oidAnim, ploActor->oid, plo ? plo->Cid() : 0, grf, n);
    if (!plo) return;
    if (FCidDerivedFrom(plo->Cid(), 0x82))
    {
        NotifyActorJumpKind(pa, n);
        ClearSmaHandle(&pa->psma);
        ApplyAsegCur(static_cast<ASEG*>(plo), pa->palo, 0.0f, 1.0f, grf, &pa->pasega);
    }
    else if (FCidDerivedFrom(plo->Cid(), 0x84))
    {
        NotifyActorJumpKind(pa, n);
        RetractAsegaPp(&pa->pasega);
        ReplaceSmaHandle(static_cast<SM*>(plo), pa->palo, -1, grf, &pa->psma);
    }
}

// SCPT 0x30 (orig3 0x226D00 case 48: the actor driven by the run toward the point until within the radius, 0x22B028
// -> 0x156468; Sly 2's 0x24 drive 0x1E46A0: the stick toward the aim, its deflection 0.25 + d / 100 clamped to 1, R1
// for a walk): the done test refreshes the point every frame (ActorWalkToward3) and UpdateActor's drive steers the
// actor's stick (DriveActorWalk30) while it is fresh. Before, the interim put the actor at the point at once (c_intro's
// walk-ins ended in a frame; the reference's take 60..70 vblanks). The POs with no native actor keep the placement.
namespace
{
struct WALK30 { glm::vec3 pos{0.0f}; int frame = -10; float dBest = 0.0f; int frameBest = 0; int frameFloor = 0; int frameStart = 0; float gSpeed = 1.0f; bool fR1 = false; };
std::unordered_map<const ACTOR*, WALK30>& MpWalk30() { static std::unordered_map<const ACTOR*, WALK30> s_mp; return s_mp; }
}  // namespace

bool ActorWalkToward3(LO* ploActor, const float pos[3], float gSpeed, bool fR1)
{
    ACTOR* pa = PactorFromLo(ploActor);
    if (!pa || std::getenv("SLY3_NOWALK30")) return false;
    // (an actor not updated (not in the world, its root not active, or its update off: the table entry's flag 2) runs no states and cannot walk: the
    // interim placement at once. E.g. Ep6 m_ext's Panda King 30 (an actor since AUDIT4 PLAYER-01; before, no actor: placed)
    // floated still through SCPT 4478's walks, each waiting out the no-floor net)
    if (!pa->palo || !pa->palo->fInWorld || !g_psw || pa->palo->iupd < 0 || pa->palo->iupd >= static_cast<int>(g_psw->aupd.size()) ||
        !(g_psw->aupd[static_cast<size_t>(pa->palo->iupd)].grf & GRFUPD_Update))
        return false;
    WALK30& w = MpWalk30()[pa];
    const glm::vec3 goal(pos[0], pos[1], pos[2]);
    const glm::vec3 q = PosAloCenter(pa->palo);
    const float d = std::hypot(goal.x - q.x, goal.y - q.y);
    // (a point far off (over 1500: the actor was not brought near it, e.g. a survey's forced chain left Sly elsewhere;
    // the original's walk-ins are short) or an actor off the floor for a second: the interim placement)
    // (an actor on a rail / ledge / pipe (the interaction +0x4C0's state, the climb +0xF54: e.g. m_ext's attract 4262 [40],
    // Sly along the rail 15084 to 15085, 1586 away, over the pillar's trigger 15072) walks it however far and off the floor)
    const bool fOnPath = pa->nSub4CC != -1 || pa->nSubF54 != -1;
    // (and its slow / stalled nets count from where it leaves the path: m_ext's RC chopper scene, SCPT 4277 [16], shimmies
    // 1186 along the ledge 15188 at half speed, over 9 s in the reference (build/ref/chop1: still on it at vblank 6550),
    // then walks off its end to 15189)
    if (fOnPath) w.frameFloor = w.frameStart = w.frameBest = g_iframeMain;
    // (round 23: 3000, was 1500: 4262 [108] runs Sly 2670 from the elephant to 15070 (the reference's 204 vblanks at
    // 833 a second); the stall / 5 s nets below still place an actor that cannot get there)
    if (w.frame < g_iframeMain - 1 && d > 3000.0f && !fOnPath) return false;
    if (!pa->pxpFloor && g_iframeMain - w.frameFloor > 60 && w.frame >= g_iframeMain - 1)
    {
        w.frame = -10;
        return false;
    }
    if (pa->pxpFloor) w.frameFloor = g_iframeMain;
    if (w.frame < g_iframeMain - 1 || glm::length(w.pos - goal) > 10.0f)
    {
        w.frameFloor = g_iframeMain;
        if (std::getenv("SLY3_SCPTPRINT")) std::printf("player3: 0x30 walk of %d toward %.0f %.0f %.0f\n", ploActor->oid, pos[0], pos[1], pos[2]);
        w.dBest = d;
        w.frameBest = g_iframeMain;
        w.frameStart = g_iframeMain;
    }
    else if (g_iframeMain - w.frameStart > 300)
    {
        // (a walk still going after 5 s (the reference's walk-ins take 60..70 vblanks): an actor that creeps (c_intro's
        // Penelope, round 21: her states run now, but she moves at ~20 units a second): the interim placement, so the scene
        // does not wait on it)
        if (std::getenv("SLY3_SCPTPRINT")) std::printf("player3: 0x30 walk of %d too slow at %.0f: placed\n", ploActor->oid, d);
        w.frame = -10;
        return false;
    }
    else if (d < w.dBest - 10.0f)
    {
        w.dBest = d;
        w.frameBest = g_iframeMain;
    }
    else if (g_iframeMain - w.frameBest > 60)
    {
        // (no progress for a second: an actor whose states take no stick here (e.g. Ep4 3636's walks 20,000 up, no
        // floor): the interim placement, as before)
        if (std::getenv("SLY3_SCPTPRINT")) std::printf("player3: 0x30 walk of %d stalled at %.0f: placed\n", ploActor->oid, d);
        w.frame = -10;
        return false;
    }
    w.pos = goal;
    w.frame = g_iframeMain;
    w.gSpeed = gSpeed;
    w.fR1 = fR1;
    if (static const bool s_f = std::getenv("SLY3_WALKPRINT") != nullptr; s_f)   // (debug: the walk's progress per frame)
        std::printf("walk30 frame %d actor %d at %.1f %.1f %.1f goal %.1f %.1f %.1f d %.1f speed %.2f floor %d sub %d/%d vel %.1f\n",
                    g_iframeMain, ploActor->oid, q.x, q.y, q.z, goal.x, goal.y, goal.z, d, gSpeed, pa->pxpFloor != nullptr,
                    pa->nSub4CC, pa->nSubF54, glm::length(pa->vecVel));
    return true;
}

// orig3 0x22A7F8 case 47: 0x24E878(actor, the object's heading), the actor's +0x110C (the stick's heading) the same, then
// the stopped branch (no deflection; circle (+0x18 | 0x20) while the actor's interaction is on, +0x50C != -1)
bool FDriveActorFace2F(ACTOR* pa, PADCTL* pc)
{
    float rad = 0.0f;
    if (!pc || !pa->palo || !FActorRunFaceHeading3(pa, &rad)) return false;
    SetStepPhysHeading(pa->palo, rad);
    pc->radJoy = rad;
    pc->uJoy = 0.0f;
    if (pa->nSub4CC != -1) pc->grfbtn |= 0x20;
    return true;
}

bool FDriveActorWalk30(ACTOR* pa, PADCTL* pc)
{
    auto it = MpWalk30().find(pa);
    if (it == MpWalk30().end() || it->second.frame < g_iframeMain - 1 || !pc) return false;
    const glm::vec3 q = PosAloCenter(pa->palo);
    const float dx = it->second.pos.x - q.x, dy = it->second.pos.y - q.y;
    const float d = std::sqrt(dx * dx + dy * dy);
    pc->radJoy = std::atan2(dy, dx);
    // (0x22A7F8 case 48: the deflection the command's |speed|; on a rail (the interaction's kind 3) along it at that speed,
    // else times 0.25 + d / 100 clamped to [0, 1])
    const bool fRail = pa->nSub4CC != -1 && pa->n4C8 == 3;
    pc->uJoy = glm::clamp(it->second.gSpeed, 0.0f, 1.0f) * (fRail ? 1.0f : glm::clamp(0.25f + d * 0.01f, 0.0f, 1.0f));
    // (the command's +0x14: R1 held (the drive's +0x18 | 8): Sly 3's state 2, the gait +0x1470 at full and 1.3 times its
    // speed (0x1AD7F0, sly.cpp's state 1); m_ext's attract 4262 [89] runs to 15109 at the reference's 833 a second)
    // (Sly only: the other characters' native code reads R1 as their own button (Bentley's walk in ep1_heist_end's
    // SCPT 2467 [77] then never arrived))
    // (s3-prologue2: Bentley too (m_ext's beast intro, SCPT 4318 [29] / [33]: the reference's Bentley walks in his state
    // 1 and glides on after the stick's release, so his bomb lands at the lamp post's base; SLY3_R1SLYONLY=1: Sly only)
    static const bool s_fSlyOnly = std::getenv("SLY3_R1SLYONLY") != nullptr;
    if (it->second.fR1 && (dynamic_cast<SLY*>(pa) || (!s_fSlyOnly && dynamic_cast<BENTLEY*>(pa)))) pc->grfbtn |= 0x8;
    // (0x22A7F8 case 48's end: circle held (+0x18 | 0x20) while the actor's interaction is on (+0x50C != -1) and of kind 0
    // (a ledge) or 0xD: the hang is kept while the walk shimmies along it (m_ext's RC chopper scene, SCPT 4277 [15] / [16]))
    if (pa->nSub4CC != -1 && (pa->n4C8 == 0 || pa->n4C8 == 0xD)) pc->grfbtn |= 0x20;
    return true;
}

namespace
{
struct HOLD5D { bool fHold = false, fFresh = false; };
std::unordered_map<const ACTOR*, HOLD5D>& MpHold5D() { static std::unordered_map<const ACTOR*, HOLD5D> s_mp; return s_mp; }
}  // namespace

// (on by default since s3fix5-player, AUDIT5: in m_ext's beast intro Bentley holds, summons and drops his bomb at the
// reference's timing (state 8 at the script's frame 218 / vblank 2013, the bomb's states 1 / 2 30 / 44 frames later) and
// its blast knocks the lamp post 15452 over as in the reference (carry.cpp BlastCarry: Sly 3's 0x277830 hits every
// object, natively the Sly 2 filter dropped all). The "yellow aim view" once blamed on the hold is m_ext's lightning
// (the reference flashes the same way, e.g. vblanks 2340 / 2640). Left (not the hold's): natively Bentley stops ~75
// short of the reference's place and faces his walk's way, so the bomb lands ~100 left of the post's base (the
// post still falls) and the shot cuts ~100 frames earlier; the scripted walk's arrival / glide. SLY3_HOLD5D=0: the old
// native way, 0x5D / 0x5E done at once and no hold)
bool FHold5D3() { static const bool s_f = [] { const char* sz = std::getenv("SLY3_HOLD5D"); return !(sz && sz[0] == '0'); }(); return s_f; }

void SetActorHold5D3(ACTOR* pa, bool fHold)
{
    if (!pa || !FHold5D3()) return;
    HOLD5D& h = MpHold5D()[pa];
    h.fFresh = fHold && !h.fHold;
    h.fHold = fHold;
}

void ApplyActorHold5D3(ACTOR* pa, PADCTL* pc)
{
    auto it = MpHold5D().find(pa);
    if (it == MpHold5D().end() || !it->second.fHold || !pc) return;
    pc->grfbtn |= 0x10;
    // (the press at the hold's start: the input's Query(0.15, 0x10) sees it once, as a pad's press)
    if (it->second.fFresh) pc->atPressed[4] = g_clock.t;
    it->second.fFresh = false;
}

namespace
{
struct CLIMB26 { float sGoal = 0.0f; int frame = -10; };
std::unordered_map<const ACTOR*, CLIMB26>& MpClimb26() { static std::unordered_map<const ACTOR*, CLIMB26> s_mp; return s_mp; }
}  // namespace

void ActorClimbToward3(ACTOR* pa, float sGoal)
{
    CLIMB26& c = MpClimb26()[pa];
    c.sGoal = sGoal;
    c.frame = g_iframeMain;
}

// orig3: 0x22A7F8 case 38 (Sly 2's 0x1E46A0 case 0x1A, scpt.cpp): the stick along the pipe's tangent at the actor (the
// points 50 either way) in the camera's frame, turned round when the goal lies behind; full deflection
bool FDriveActorClimb26(ACTOR* pa, PADCTL* pc)
{
    auto it = MpClimb26().find(pa);
    if (it == MpClimb26().end() || it->second.frame < g_iframeMain - 1 || !pc || !pa->pcrvF50 || pa->nSubF54 == -1) return false;
    const CRV* pcrv = pa->pcrvF50;
    glm::vec3 a, b;
    EvalCrvS(pcrv, SWrapCrv(pcrv, pa->sCrvF50 + 50.0f), &a, nullptr);
    EvalCrvS(pcrv, SWrapCrv(pcrv, pa->sCrvF50 - 50.0f), &b, nullptr);
    const glm::vec3 cc = glm::transpose(pc->matCam) * (a - b);
    const float l = std::sqrt(cc.y * cc.y + cc.z * cc.z);
    float x = l > 0.0f ? -cc.y / l : 0.0f, y = l > 0.0f ? cc.z / l : 0.0f;
    const float gCur = pa->sCrvF50, gTarget = it->second.sGoal;
    bool fBehind;
    if (pcrv->fClosed)
    {
        const float len = SLengthCrv(pcrv);
        const float da = gTarget < gCur ? gCur - gTarget : (len + gCur) - gTarget;
        const float db = gTarget < gCur ? (len + gTarget) - gCur : gTarget - gCur;
        fBehind = da < db;
    }
    else fBehind = gTarget < gCur;
    if (fBehind) { x = -x; y = -y; }
    pc->x = x;
    pc->y = y;
    pc->radJoy = std::atan2(y, x);
    pc->uJoy = 1.0f;
    return true;
}

// (the scripts' actor hooks: before the first world start's task activation, transition.cpp: a script started there
// (ep1_turf's 2142 [77]: Bentley's path step 0x33) found no hook and its job ended at once)
void InstallPlayerHooks3()
{
    if (!g_scpthooks3.pfnPlayActorScriptAnim) g_scpthooks3.pfnPlayActorScriptAnim = PlayActorScriptAnim3;
    if (!g_scpthooks3.pfnEndActorScriptAnim)
        g_scpthooks3.pfnEndActorScriptAnim = [](LO* plo) { if (ACTOR* pa = PactorFromLo(plo)) ::EndActorScriptAnim(pa); else ::EndPoScriptAnim(plo); };   // 0x156690
    if (!g_scpthooks3.pfnFActorInScriptAnim) g_scpthooks3.pfnFActorInScriptAnim = ::FActorScriptAnim;   // 0x1566D0
    if (!g_scpthooks3.pfnActorPathStep) g_scpthooks3.pfnActorPathStep = ActorPathStep3;
    if (!g_scpthooks3.pfnActorStepTo) g_scpthooks3.pfnActorStepTo = ActorStepTo3;
    if (!g_scpthooks3.pfnActorWalkToward) g_scpthooks3.pfnActorWalkToward = ActorWalkToward3;
    if (!g_scpthooks3.pfnFActorJobDone) g_scpthooks3.pfnFActorJobDone = FActorJobDone3;
    if (!g_scpthooks3.pfnPlaceLo) g_scpthooks3.pfnPlaceLo = PlaceLoScript3;
    if (!g_scpthooks3.pfnFaceObject) g_scpthooks3.pfnFaceObject = FaceObject3;
    if (!g_scpthooks3.pfnActorCmd) g_scpthooks3.pfnActorCmd = ActorCmd3;
    if (!g_scpthooks3.pfnFActorCmdDone) g_scpthooks3.pfnFActorCmdDone = FActorCmdDone3;
    if (!g_scpthooks3.pfnResetActorAfterPlace) g_scpthooks3.pfnResetActorAfterPlace = ResetActorAfterPlace3;
}

bool FStartPlayer3(SW* psw)
{
    ::MpScriptAnimPo().clear();
    StartCodePanels3(psw);
    InstallPlayerHooks3();
    const char* sz = std::getenv("SLY3_START");
    if (!sz || !*sz || *sz == '0') return false;
    LO* plo = PloSly3(psw);
    // (SLY3_STARTOID=oid: another player object started instead, e.g. Bentley 25 / Murray 26)
    if (const char* szOid = std::getenv("SLY3_STARTOID"))
        for (const auto& up : psw->aploAll)
            if (up->oid == std::atoi(szOid) && FIsLoDerivedFrom(up.get(), S3CID(0x30))) plo = up.get();
    if (!plo)
    {
        std::fprintf(stderr, "player3: no Sly (cid 0x%X) in this world\n", S3CID(0x35));
        return false;
    }
    ALO* palo = static_cast<ALO*>(plo);
    glm::vec3 pos = c_posSpawn;
    float rad = c_radSpawn;
    // SLY3_START=x,y,z[,rad]: another place
    if (std::strchr(sz, ','))
    {
        float x, y, z, r = rad;
        if (std::sscanf(sz, "%f,%f,%f,%f", &x, &y, &z, &r) >= 3) pos = glm::vec3(x, y, z), rad = r;
    }
    // (the development start skips the prologue's scripts (the script code: the attract, the title, the binocucom), so
    // their cameras do not hold the view; SLY3_START_SCRIPTS=1 keeps them)
    if (!std::getenv("SLY3_START_SCRIPTS")) ResetScripts3();
    // SLY3_ADDLO=oid,..: objects the development start adds (the ones the skipped scripts / tasks add: e.g. i_trainer's
    // training stations 163607 163795 129582 83655 164129, in the world in the reference from vblank 2400)
    if (const char* szAdd = std::getenv("SLY3_ADDLO"))
        for (const char* q = szAdd; q && *q; q = std::strchr(q, ',') ? std::strchr(q, ',') + 1 : nullptr)
        {
            const int oid = std::atoi(q);
            for (const auto& up : psw->aploAll)
                if (up->oid == oid && up->fRemoved) AddLo(psw, up.get());
        }
    // (the prologue removes him at the start; its binocucom task (the script code) adds him back: orig 0x1C7530 AddLo)
    if (plo->fRemoved) AddLo(psw, plo);
    if (std::getenv("SLY3_STARTDBG"))
        if (XF* pxf = PxfPos(palo))
            std::printf("player3: before: Sly's record local %.1f %.1f %.1f world %.1f %.1f %.1f; ref world %.1f %.1f %.1f local %.1f %.1f %.1f\n",
                        pxf->mat[3].x, pxf->mat[3].y, pxf->mat[3].z, pxf->matWorld[3].x, pxf->matWorld[3].y, pxf->matWorld[3].z,
                        pxf->pxfRef ? pxf->pxfRef->matWorld[3].x : 0.f, pxf->pxfRef ? pxf->pxfRef->matWorld[3].y : 0.f,
                        pxf->pxfRef ? pxf->pxfRef->matWorld[3].z : 0.f, pxf->pxfRef ? pxf->pxfRef->mat[3].x : 0.f,
                        pxf->pxfRef ? pxf->pxfRef->mat[3].y : 0.f, pxf->pxfRef ? pxf->pxfRef->mat[3].z : 0.f);
    STEP* p = PstepFromLo(plo);
    const float c = std::cos(rad), s = std::sin(rad);
    const glm::mat3 mat(glm::vec3(c, s, 0.0f), glm::vec3(-s, c, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    if (p)
    {
        SetStepRotation(p, mat);
        SetStepPos(p, pos);
    }
    else
    {
        SetAloRotation(palo, mat);
        SetAloPos(palo, pos);
    }
    EnablePo(psw, palo, 1);
    // (dev harness: the start actor is moved from its load place; the water overlaps of that place (m_ext's water 30387
    // for Bentley) still hold at the first frame and gave it the water's contact (the reaction 3, its clip and the
    // recovery): damage.cpp's UpdateActorWater skips it for 2 frames)
    g_ploStartPlaced3 = palo;
    g_iframeStartPlaced3 = g_iframeMain;
    // the camera as the reference's at that moment (its follow policy +0xA0 pan -2.1146, tilt 0.1974 over the beach
    // zone's entry): a cut to that pan (the follow's g_cm.fCut 2 path)
    {
        float radPan = -2.1146f;
        if (const char* szPan = std::getenv("SLY3_START_PAN")) radPan = float(std::atof(szPan));
        g_cm.fCut = 2;
        g_cm.radCutPan = radPan;
        g_cm.radCutTilt = 0.1974f + std::atan(-0.25f * g_cm.yScreenRange);
    }
    if (std::getenv("SLY3_STARTDBG"))
        std::printf("player3: Sly iupd %d grfUpd 0x%x removed %d inworld %d st %d nodeActive %d parent %p step %p sly state %d\n", plo->iupd,
                    plo->grfUpd, plo->fRemoved, plo->fInWorld, plo->st, PloRoot(plo)->fNodeActive, (void*)plo->paloParent, (void*)p,
                    PslyFromLo(plo) ? PslyFromLo(plo)->jts : -9);
    if (std::getenv("SLY3_STARTDBG"))
        if (SLY* psly = PslyFromLo(plo); psly && psly->psmIdle)
        {
            std::printf("player3: idle SM %d states:", psly->psmIdle->oid);
            for (const auto& [oid, next] : psly->psmIdle->anode) std::printf(" 0x%X", oid);
            std::printf("\n");
        }
    DumpCombos3(psw);
    if (std::getenv("SLY3_PARTDBG"))
        for (const auto& [ib, plop] : palo->apart)
        {
            const ASEG* pa = dynamic_cast<const ASEG*>(plop);
            std::printf("player3: part +0x%X%s -> %d cid 0x%X%s", ib & ~FN3_TAG, (ib & FN3_TAG) ? " (Sly 3)" : "", plop ? plop->oid : -1,
                        plop ? plop->Cid() : -1, pa ? "" : "\n");
            if (pa) std::printf(" ASEG %.3f s, %zu channels\n", PasegdOf(const_cast<ASEG*>(pa)) ? PasegdOf(const_cast<ASEG*>(pa))->tMax : -1.0f,
                                PasegdOf(const_cast<ASEG*>(pa)) ? PasegdOf(const_cast<ASEG*>(pa))->achn.size() : 0);
        }
    if (std::getenv("SLY3_STARTDBG"))
        if (XF* pxf = PxfPos(palo))
            std::printf("player3: Sly's record: local %.1f %.1f %.1f world %.1f %.1f %.1f ref %p (owner %d cid 0x%X) kind %d\n", pxf->mat[3].x,
                        pxf->mat[3].y, pxf->mat[3].z, pxf->matWorld[3].x, pxf->matWorld[3].y, pxf->matWorld[3].z, (void*)pxf->pxfRef,
                        pxf->pxfRef && pxf->pxfRef->palo ? pxf->pxfRef->palo->oid : -1, pxf->pxfRef && pxf->pxfRef->palo ? pxf->pxfRef->palo->Cid() : -1,
                        pxf->kind);
    std::printf("player3: Sly (oid %d) placed at %.2f %.2f %.2f heading %.4f, in control\n", plo->oid, pos.x, pos.y, pos.z, rad);
    if (std::getenv("SLY3_JTLIST")) ListJt3(psw);
    return true;
}

// orig3 0x1A9260 (the player's camera kind, for Camera Options...): 0 the camera, 1 the binocucom (its window open on
// the current player), 2 the grapple-cam, 3 a turret, 4 strafing, 5 swimming, 6 the harpoon. TODO(player): 2..6 with
// their moves (none on the goal path so far)
int NPlayerCamKind3(SW* psw)
{
    LO* plo = psw ? PloPoCurrent(psw) : nullptr;
    if (plo && NBinocPanelState3(plo) >= 0) return 1;
    if (::FGrappleCam3(plo)) return 2;   // game/grapplecam3.cpp
    return 0;
}

void SetPosPlayer3(SW* psw, int frame)
{
    // (debug: SLY3_OIDPOS=oid[,oid..]@F: those objects' world positions at frame F, e.g. a splice script's target marker)
    // (@F+N: every N frames from F on)
    static const char* s_szPos = std::getenv("SLY3_OIDPOS");
    static const int s_fPos = s_szPos && std::strchr(s_szPos, '@') ? std::atoi(std::strchr(s_szPos, '@') + 1) : -1;
    static const int s_dfPos = s_szPos && std::strchr(s_szPos, '+') ? std::atoi(std::strchr(s_szPos, '+') + 1) : 0;
    if (s_fPos >= 0 && (frame == s_fPos || (s_dfPos > 0 && frame > s_fPos && (frame - s_fPos) % s_dfPos == 0)))
        for (const char* q = s_szPos; q && *q && *q != '@'; q = std::strchr(q, ',') ? std::strchr(q, ',') + 1 : nullptr)
        {
            const int oid = std::atoi(q);
            for (const auto& up : psw->aploAll)
                if (up->oid == oid || up->oidInstance == oid)   // (or its instance name)
                {
                    const glm::vec3 p = PosLoWorld(up.get());
                    const glm::mat4 m = sly3::MatLoWorld3(up.get());
                    const SOST* pst = FIsLoDerivedFrom(up.get(), S3CID(0x30)) ? Psost(static_cast<ALO*>(up.get())) : nullptr;
                    std::printf("oidpos %d f %d (oid %d cid 0x%X, removed %d, in world %d): %.2f %.2f %.2f x axis %.2f %.2f %.2f gravity %.0f\n", oid, frame, up->oid, up->Cid(),
                                int(up->fRemoved), int(up->fInWorld), p.x, p.y, p.z, m[0].x, m[0].y, m[0].z, pst ? pst->dvGravity.z : 0.0f);
                }
            for (const auto& up : psw->aploAll)   // (and the records tagged with it: an ALO's points)
                if (FIsLoDerivedFrom(up.get(), S3CID(0x30)))
                    for (const auto& pxf : static_cast<ALO*>(up.get())->axf)
                        if (pxf && pxf->tag == oid)
                            std::printf("oidpos %d: record (kind %d) of %d: %.1f %.1f %.1f x axis %.3f %.3f %.3f\n", oid, pxf->kind, up->oid, pxf->matWorld[3].x,
                                        pxf->matWorld[3].y, pxf->matWorld[3].z, pxf->matWorld[0].x, pxf->matWorld[0].y, pxf->matWorld[0].z);
            for (const auto& up : psw->aploAll)   // (and the collision entries tagged with it)
                if (FIsLoDerivedFrom(up.get(), S3CID(0x30)))
                    for (const SOE& e : static_cast<ALO*>(up.get())->asoe)
                        if (e.tag == oid)
                        {
                            const glm::vec3 p = e.pxf ? glm::vec3(e.pxf->matWorld[3]) : PosLoWorld(up.get());
                            std::printf("oidpos %d: collision entry of %d (cid 0x%X): %.1f %.1f %.1f\n", oid, up->oid, up->Cid(), p.x, p.y, p.z);
                        }
        }
    static const char* s_sz = std::getenv("SLY3_SETPOS");
    if (!s_sz) return;
    for (const char* p = s_sz; p && *p; p = std::strchr(p, ';') ? std::strchr(p, ';') + 1 : nullptr)
    {
        int f;
        float x, y, z;
        if (std::sscanf(p, "%d:%f %f %f", &f, &x, &y, &z) != 4 || f != frame) continue;
        // (the current player's character when it is an actor, else Sly: as the trace)
        LO* plo = PloPoCurrent(psw);
        if (!plo || !PactorFromLo(plo)) plo = PloSly3(psw);
        if (!plo) return;
        ALO* palo = static_cast<ALO*>(plo);
        // (the reference's SLY2_SETPOS places the position record (vt slot 29): the object moved by the same amount)
        glm::vec3 posTo(x, y, z);
        if (PxfPos(palo)) posTo += PosLoWorld(plo) - glm::vec3(PxfPos(palo)->matWorld[3]);
        if (STEP* pstep = PstepFromLo(plo)) SetStepPos(pstep, posTo);
        else SetAloPos(palo, posTo);
        SetAloVelocity(palo, glm::vec3(0.0f));
        SetAloAngVelocity(palo, glm::vec3(0.0f));
        std::printf("player3: frame %d: the player %d moved to %.1f %.1f %.1f\n", frame, plo->oid, x, y, z);
    }
}

void TracePlayer3(SW* psw, int frame)
{
    static const char* s_szTrace = std::getenv("SLY3_PLAYERTRACE");
    if (!s_szTrace) return;
    if (!s_ptrace)
    {
        s_ptrace = std::fopen(s_szTrace, "w");
        if (!s_ptrace) return;
        std::fprintf(s_ptrace, "f,t,state,substate,jstate,x,y,z,vx,vy,vz,heading,mode,cx,cy,cz,cmpol,u,oid\n");
    }
    // the current player's character when it is an actor (Sly 24, Bentley 25, Murray 26, ..), else Sly; the column
    // "oid" says which. The state is the character's own (Sly +0x19E0; Bentley / Murray +0x1520).
    LO* plo = PloPoCurrent(psw);
    if (!plo || !PactorFromLo(plo)) plo = PloSly3(psw);
    ACTOR* pa = plo ? PactorFromLo(plo) : nullptr;
    if (!plo || !pa) return;
    SLY* p = PslyFromLo(plo);
    int state = -1, substate = -1, mode = -1;
    if (p) state = p->jts, substate = p->jtbs, mode = p->mode;   // (Sly 3's numbers: the attack is 23)
    else if (const BENTLEY* pb = PbentleyFromLo(plo)) state = pb->state;
    else if (const MURRAY* pm = PmurrayFromLo(plo)) state = pm->state;
    // (the position record +0x58 (Sly 3 +0x48), as the reference's peek +0x48 -> +0x70: for Sly the object record's
    // place, for Bentley 40 below it (the wheelchair's object record stands at its seat))
    const glm::vec3 pos = PxfPos(static_cast<ALO*>(plo)) ? glm::vec3(PxfPos(static_cast<ALO*>(plo))->matWorld[3]) : PosLoWorld(plo);
    const glm::vec3 v = VelAloPos(static_cast<ALO*>(plo));
    if (std::getenv("SLY3_TRACEXF") && frame % 100 == 0)
        for (std::size_t i = 0; i < 4 && i < static_cast<ALO*>(plo)->axf.size(); ++i)
        {
            const XF* pxf = static_cast<ALO*>(plo)->axf[i].get();
            std::printf("tracexf f %d: record %zu tag %d kind %d at %.1f %.1f %.1f (object %.1f %.1f %.1f, pos record %.1f)\n", frame, i, pxf->tag, pxf->kind, pxf->matWorld[3].x,
                        pxf->matWorld[3].y, pxf->matWorld[3].z, pos.x, pos.y, pos.z, PxfPos(static_cast<ALO*>(plo)) ? PxfPos(static_cast<ALO*>(plo))->matWorld[3].z : -1.0f);
        }
    const PODATA* ppo = PpodataFromLo(plo);
    std::fprintf(s_ptrace, "%d,%.4f,%d,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.5f,%d,%.2f,%.2f,%.2f,%d,%.3f,%d\n", frame, g_clock.t,
                 state, substate, pa->jump.state, pos.x, pos.y, pos.z, v.x, v.y, v.z, pa->radHeading, mode, g_cm.pos.x, g_cm.pos.y,
                 g_cm.pos.z, g_cm.nCur, ppo && ppo->ppadctl ? ppo->ppadctl->uJoy : -1.0f, plo->oid);
    std::fflush(s_ptrace);
}
}  // namespace sly3
