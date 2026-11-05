#include "jtthrow.h"
// Murray's carried-object state machine and authored attachment poses.
#include "murraycarry.h"
#include "gang.h"
#include "activate.h"
#include "scpt.h"
#include "brx.h"
#include "actanim.h"
#include "actmesh.h"
#include "actor.h"
#include "alo.h"
#include "asega.h"
#include "clock.h"
#include "gadget.h"
#include "jump.h"
#include "jt.h"
#include "joy.h"
#include "mat.h"
#include "po.h"
#include "so.h"
#include "smrun.h"
#include "stepphys.h"
#include "sw.h"
#include "task.h"
#include "step.h"
#include "vtnative.h"
#include "update.h"
#include "worldaccess.h"
#include "xfphys.h"
#include "aihit.h"
#include "aipursuit.h"
#include "animfind.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>

extern int g_iframeHarness;
LO* PloInteractOidField(LO*, uint32_t);


namespace {
ALO* Owner(MURRAY* p) { return dynamic_cast<ALO*>(PloInteractOidField(p->plo13F0, 0x180)); }
XF* XfObject(LO* p) {
    if (auto* a = dynamic_cast<ALO*>(p)) return PxfObj(a);
    if (auto* x = dynamic_cast<LOX*>(p)) return x->pxf;
    return nullptr;
}
const ASEGEV* Event(ASEG* a, int tag) {
    if (a) for (const auto& e : PasegdOf(a)->aaev[1]) if (e.kind == 11 && e.an[0] == tag) return &e;
    return nullptr;
}
void Anim(MURRAY* p, ASEG* clip, float time, int flags, SM* sm = nullptr) {
    SetActorAnimPp(&p->pasega,&p->psma,p->palo,clip,time,1.0f,flags,sm,-1,4,false);
}
void Modes(MURRAY* p, int k) {
    if (XF* x=PxfPos(p->palo)) SetXfMode(p->palo,x,k);
    if (XF* x=PxfRot(p->palo)) SetXfMode(p->palo,x,k);
}
// orig: 0x0022FF40: the complete matrix, expressed in the destination frame.
glm::mat4 Convert(XF* from, XF* to, const glm::mat4& in) {
    glm::mat4 m = from ? MulMatrix4(from->matWorld,in) : in;
    return to ? MulMatrix4(InverseAffineMatrix4(to->matWorld),m) : m;
}
// orig: 0x001B72B0: solve the carried object's two root records so its authored grip
// pose coincides with the holder. The relative-chain products preserve the original order.
bool Pose(MURRAY* p, XF* stop, const glm::mat4& input, glm::vec4* rot, glm::vec4* pos) {
    ALO* owner=Owner(p);
    if (!owner) return false;
    XF* left=PxfPos(owner), *right=PxfRot(owner);
    if (!left || !right) return false;
    LO* grip=PloInteractOidField(p->plo13F0,0x188);
    if (!grip) {
        if (rot) *rot=glm::vec4(EulerFromMat(MatConvertXfs(stop,right->pxfRef,Mat3Of(input))),0.0f);
        if (pos) *pos=glm::vec4(PosConvertXfs(stop,left->pxfRef,PosOf(input)),1.0f);
        return true;
    }
    const glm::mat4 gripLocal=dynamic_cast<LOX*>(grip) ? static_cast<LOX*>(grip)->mat : glm::mat4(1.0f);
    std::array<XF*,16> chain{};
    int count=0,l=-1,r=-1;
    for (XF* x=XfObject(grip);x!=stop;x=x->pxfRef) {
        if (!x || count==16) return false;
        chain[count]=x;
        if (x==left) l=count;
        if (x==right) r=count;
        ++count;
    }
    if (l<0 || r<0) return false;
    const glm::mat4 inverse=InverseAffineMatrix4(input);
    glm::mat4 from=Convert(XfObject(grip),right,glm::mat4(1.0f));
    const glm::mat4 to=Convert(right->pxfRef,stop,glm::mat4(1.0f));
    from=MulMatrix4(from,gripLocal);
    const glm::mat4 upper0=MulMatrix4(from,inverse);
    const glm::mat4 result0=MulMatrix4(upper0,to);
    glm::mat4 replace=InverseAffineMatrix4(result0);
    const glm::vec4 rotation(EulerFromMat(Mat3Of(replace)),0.0f);
    SetMat3(&replace,MatEuler(glm::vec3(rotation)));
    replace[3]=chain[r]->mat[3];
    glm::mat4 upper(1.0f),result(1.0f);
    for (int j=count-1;j>l;--j) result=MulMatrix4(result,j==r?replace:chain[j]->mat);
    for (int j=l-1;j>=0;--j) upper=MulMatrix4(upper,j==r?replace:chain[j]->mat);
    upper=MulMatrix4(upper,gripLocal);
    const glm::mat4 combined=MulMatrix4(upper,inverse);
    const glm::mat4 intermediate=MulMatrix4(combined,result);
    const glm::mat4 solved=InverseAffineMatrix4(intermediate);
    if (rot) *rot=rotation;
    if (pos) *pos=solved[3];
    return true;
}
// orig: 0x001B7700: start/end pickup velocities, including kind1's clip rewind.
void PickupVelocities(MURRAY* p, const glm::vec4& goal, float event, float* duration, glm::vec4* start, glm::vec4* end) {
    ALO* owner=Owner(p); XF* x=PxfPos(owner);
    if (p->n144C==0) {
        *start=glm::vec4(VecConvertXfs(nullptr,x->pxfRef,glm::vec3(0,0,2500)),0);
        *end=glm::vec4(VecConvertXfs(nullptr,x->pxfRef,glm::vec3(0,0,-2000)),0);
    } else if (p->n144C==1) {
        const glm::vec4 pos=x->mat[3], vel=x->vel;
        SOST* s=Psost(owner);
        const glm::vec4 accel(VecConvertXfs(nullptr,x->pxfRef,s?s->dvGravity:glm::vec3(0)),0);
        float roots[2]; const int n=CSolveQuadratic(accel.z*0.5f,vel.z,pos.z-goal.z,roots);
        const float t=n?std::clamp(roots[n-1],0.15f,*duration):0.15f;
        SetAsegaTime(p->pasega,0,event-t*p->pasega->svt,p->pasega->svt);
        *end=vel+accel*t;
        *duration=t;
        const glm::vec4 predicted=pos+(vel+*end)*(t*0.5f);
        const glm::vec4 correction=(goal-predicted)/t;
        *start=vel+correction; *end+=correction;
    }
}
// orig: 0x001B7D20: all actuators removed before the carried object's parent/modes are restored.
void Detach(MURRAY* p) {
    for (CTRL*& c:p->actrlCarry) DeleteCtrl(&c);
    if (!p->plo13F0) return;
    HandleLoSpliceEvent(p->plo13F0,6,false,0,nullptr);
    BroadcastTaskEvent(g_psw,0x1B,p->plo13F0);
    if (ALO* a=Owner(p)) {
        if (XF* x=PxfPos(a)) SetXfMode(a,x,p->mode1404);
        if (XF* x=PxfRot(a)) SetXfMode(a,x,p->mode1408);
        SetAloContactsOff(a,false);
        SetAloParent(a,nullptr,nullptr,15);
        p->plo1430=a; p->t1434=g_clock.t;
        RecalcSwOxfFilterForObject(p->palo);
    }
}
}

// orig: 0x001842B0: resolve the three authored reference oids after the world load.
// Native object fields retain oids; PloInteractOidField resolves the corresponding instance.
void PostLoadCarryUse(LO* use) {
    if (!FIsLoDerivedFrom(use,0x24)) return;
    for (uint32_t ib:{0x184u,0x18Cu,0x194u}) {
        LO* goal=PloFindSwNearest(use,static_cast<OID>(use->U32Field(0,ib,uint32_t(-1))));
        use->SetField(0,ib+4,goal?static_cast<uint32_t>(goal->oid):0);
    }
}

// orig: 0x001B93E8
void SetMurrayCarry(MURRAY* p,int phase) {
    if (p->n1450==phase) return;
    if (p->n1450==1) { DeleteCtrl(&p->actrlCarry[2]); DeleteCtrl(&p->actrlCarry[3]); }
    p->n1450=phase; p->t1454=g_clock.t;
    if (phase==0) p->n1448=0;
    else if (phase==1 || phase==2) {
        ALO* owner=Owner(p);
        if (std::getenv("SLY2N_CARRYPRINT") && p->plo13F0) {
            std::fprintf(stderr,"carry-owner: use %d cid 0x%X field180 %u owner %d parent %d options",p->plo13F0->oid,p->plo13F0->Cid(),
                         p->plo13F0->U32Field(0,0x180,0),owner?owner->oid*10+owner->fInWorld:-1,p->plo13F0->paloParent?p->plo13F0->paloParent->oid:-1);
            for (const OPT& o:p->plo13F0->aopt) std::fprintf(stderr," %d",o.eopid);
            std::fprintf(stderr," murray %d holder %d cid 0x%X alo %d xf %p palo-xf %p\n",p->palo->oid,p->plo13EC?p->plo13EC->oid:-1,p->plo13EC?p->plo13EC->Cid():-1,
                         dynamic_cast<ALO*>(p->plo13EC)!=nullptr,(void*)XfObject(p->plo13EC),(void*)PxfObj(p->palo));
        }
        if (!owner || !owner->fInWorld) {
            Detach(p); p->plo13F0=nullptr; SetMurrayState(p,0); return;
        }
        if (phase==1) {
            SetAloCnstrLin(owner,0);
            p->mode1404=PxfPos(owner)->mode; p->mode1408=PxfRot(owner)->mode;
            SetAloParent(owner,nullptr,nullptr,15);
            SetAloContactsOff(owner,true);
            const ASEGEV* ev=Event(p->pasega?p->pasega->paseg:nullptr,0x1C7);
            if (!ev) return;
            float duration=(ev->t-p->pasega->t)/p->pasega->svt;
            glm::mat4 predicted(1.0f); // 0x2309E8 initializes the chain accumulator
            PredictXfChain(XfObject(p->plo13EC),PxfObj(p->palo),duration,&predicted,nullptr);
            predicted=MulMatrix4(MatJtLo(p->palo),predicted); // 0x1A71B0(p,world,predicted)
            glm::vec4 rot(0),pos(0),start(0),end(0);
            const bool fPose=Pose(p,nullptr,predicted,&rot,&pos);
            if (FIsLoDerivedFrom(owner,0x38)) ReactGuardHeld(owner,p->palo);  // 0x209E28: reaction 6, job 0xB0
            if (!fPose) return;
            PickupVelocities(p,pos,ev->t,&duration,&start,&end);
            p->actrlCarry[2]=PctrlNewTarget(owner,PxfPos(owner),5,g_clock.t+duration,pos,end);
            SetAloVelocityTo(owner,VecConvertXfs(PxfPos(owner)->pxfRef,nullptr,glm::vec3(start)));
            p->actrlCarry[3]=PctrlNewPos(owner,PxfRot(owner),5,7,2,rot);
        } else {
            ALO* holder=dynamic_cast<ALO*>(p->plo13EC);
            SetAloParent(owner,holder,nullptr,15);
            SetAloContactsOff(owner,true);
            glm::vec4 rot(0),pos(0);
            if (!Pose(p,XfObject(p->plo13EC),glm::mat4(1.0f),&rot,&pos)) return;
            p->actrlCarry[1]=PctrlNewPos(owner,PxfRot(owner),5,7,2,rot);
            p->actrlCarry[0]=PctrlNewPos(owner,PxfPos(owner),5,7,2,pos);
            // Optional hand goals (+190/+198) also require kind6 frame controllers and
            // the actor's pair bindings. They remain outside this scripted-object slice.
            if (FIsLoDerivedFrom(owner,0x38)) CarryGuardHeld(owner);  // 0x209E88: job 0xB0 state 1
            p->vecVel=glm::vec3(0.0f);
        }
    }
    if (TARGETREC* target=PtargetrecOf(p->palo)) target->f1C=0<=p->n1450 && p->n1450<2;
    if (std::getenv("SLY2N_CARRYPRINT")) std::fprintf(stderr,"carry-phase: frame %d t %.9g phase %d state %d\n",g_iframeHarness,g_clock.t,phase,p->state);
}

// orig: 0x001B92F8
void UpdateMurrayCarryTransitions(MURRAY* p) {
    if (p->n1450==1 && p->n1448) {
        ALO* a=Owner(p);
        if (!a || !a->fInWorld) { SetMurrayState(p,0); if(a) p->plo13F0=nullptr; }
    } else if (p->n1450==2 && FAsegaRetracted(p->pasega)) SetMurrayState(p,5);
}

// orig: 0x001B5658
void SetMurrayCarryMove(MURRAY* p,int k) {
    if(p->n1458==k)return;
    if(p->n1458==0) Modes(p,2);
    else if((p->n1458==1||p->n1458==2)&&p->pasega) p->g1A0=p->pasega->t/PasegdOf(p->pasega->paseg)->tMax;
    p->n1458=k; p->t145C=g_clock.t;
    ASEG* clip=nullptr; SM* sm=nullptr; float time=0; int flags=1;
    if(k==0) { Modes(p,1); sm=p->psm1510; p->vecVel=glm::vec3(0); }
    else if(k==1) { clip=p->gait1460.pbl; if(clip)time=p->g1A0*PasegdOf(clip)->tMax; ResetGaitFeet(&p->gaitWalk); flags=9; }
    else if(k==2) { for(const auto& [ib,lo]:p->palo->apart)if(ib==0x151C)clip=dynamic_cast<ASEG*>(lo);if(clip)time=p->g1A0*PasegdOf(clip)->tMax; flags=9; }
    Anim(p,clip,time,flags,sm);
}

// orig: 0x001B5380: held-object validity and movement, including Atlas Strength's
// carried jump (kind15) and R1 run. Alternate put-down and R3 remain unported.
void UpdateMurrayCarryHeld(MURRAY* p) {
    ALO* a=Owner(p);
    if (!a || !a->fInWorld) { Detach(p);p->plo13F0=nullptr;SetMurrayState(p,0);return; }
    PODATA* po=PpodataFromLo(p->palo); PADCTL* pc=po?po->ppadctl:nullptr;
    if (!pc) return;
    // orig: 0x001B53E8..0x001B5430: Cross has priority over throwing and is
    // forgotten only when the passive is owned and the carried jump starts.
    // PADCTL uses the game's button layout (Cross0x40 / R1 0x8).
    if (GrfPadctlPressedWithin(pc,0x40,0.1f) && FPowerupOwned(18)) {
        ForgetPadctlPresses(pc,0x40);
        SetJumpKind(&p->jump,15,0,0);
        return;
    }
    if (GrfPadctlTakePressed(pc,0xA0,0.0f)) { SetMurrayState(p,7);return; }
    const float u=pc->uJoy;
    const bool moving=p->n1458==0?0.1f<u:0<u;
    // orig: 0x001B54FC..0x001B5594: the acquired passive selects the authored
    // running clip; the stick threshold keeps the standing/moving hysteresis.
    SetMurrayCarryMove(p,moving?((pc->grfbtn&0x8)&&FPowerupOwned(18)?2:1):0);
    if(p->n1458==1) { SetStepPhysHeading(p->palo,pc->radJoy);p->vecVel=glm::vec3(SetGaitMix(&p->gait1460,u));UpdateGaitFeet(&p->gait1460); }
    else if(p->n1458==2) {
        SetStepPhysHeading(p->palo,pc->radJoy);
        // orig: 0x001B5608..0x001B5644 / 0x00204E38: reverse the current
        // animation-root velocity at its authored time and speed.
        glm::vec4 velocity(0.0f);
        if(ASEGA* a=p->pasega; a && !FAsegaRetracted(a))
            if(const ASEGCHN* channel=PchnFindAseg(a->paseg,p->palo,1,true);channel&&channel->ptrkp)
                channel->ptrkp->Eval(nullptr,&velocity,a->t,a->svt);
        p->vecVel=-glm::vec3(velocity);
    }
}

// orig: 0x001B5E98: authored target selection; no scene-specific object ids.
void ChooseMurrayThrowTarget(MURRAY* p) {
    float max=p->g1410*1.3f; LO* best=nullptr;
    const bool printCandidates=std::getenv("SLY2N_THROWCANDIDATES")!=nullptr;
    if(printCandidates) {
        const glm::vec3 pos=PosAloCenter(p->palo);
        std::fprintf(stderr,"throw-origin: frame %d pos %.3f %.3f %.3f heading %.6f type %d\n",g_iframeHarness,
            pos.x,pos.y,pos.z,RadStepHeading(p),p->plo13F0?int(p->plo13F0->U32Field(0,0x19C,uint32_t(-1))):-1);
    }
    if(g_psw) for(LO* point:AploJtList(g_psw,JTL_Point)) {
        if(!(point->U32Field(0,0xE8)&0x10000))continue;
        if(printCandidates) {
            const glm::vec3 pos=PosJtLo(point),d=pos-PosAloCenter(p->palo);
            std::fprintf(stderr,"throw-candidate: frame %d oid %d parent %d active %d flags %X type %d pos %.3f %.3f %.3f distance %.3f dz %.3f angle %.6f\n",
                g_iframeHarness,point->oid,point->paloParent?point->paloParent->oid:-1,point->iupd>=0,point->U32Field(0,0xE8),
                int(point->U32Field(0,0xF4,uint32_t(-1))),pos.x,pos.y,pos.z,glm::length(glm::vec2(d)),d.z,
                RadNormalize(std::atan2(d.y,d.x)-RadStepHeading(p)));
        }
        // orig0x184380: matching carried-use kind and outside the carried owner's hierarchy (nothing carried, e.g.
        // a script's throw (method 938) after a skipped pick-up: the original reads the PS2's low RAM at 0x19C; natively
        // no point matches)
        if(!p->plo13F0||point->U32Field(0,0xF4,uint32_t(-1))!=p->plo13F0->U32Field(0,0x19C,uint32_t(-1)))continue;
        bool under=false;
        for(LO* a=point;a;a=a->paloParent)if(a==Owner(p)){under=true;break;}
        if(under)continue;
        glm::vec3 d=PosJtLo(point)-PosAloCenter(p->palo);float dist=glm::length(glm::vec2(d));
        if(dist>max||dist<150||d.z>500)continue;
        if(std::abs(RadNormalize(std::atan2(d.y,d.x)-RadStepHeading(p)))>1.0f)continue;
        best=point;max=dist;p->pos1420=PosJtLo(point);
    }
    if(!best)p->pos1420=PosAloCenter(p->palo)+cmm::SetVectorCylind(p->radHeading,p->g1410,0);
    if(std::getenv("SLY2N_CARRYPRINT"))std::fprintf(stderr,"carry-target: frame %d point %d (cid 0x%X parent %d) at %.1f %.1f %.1f speed %.1f\n",g_iframeHarness,
        best?best->oid:-1,best?best->Cid():-1,best&&best->paloParent?best->paloParent->oid:-1,p->pos1420.x,p->pos1420.y,p->pos1420.z,p->g1414);
}

// orig: 0x001B7A78: state7's initial target selection precedes the explicit scripted destination.
void StartMurrayThrow(MURRAY* p,LO* target) {
    SetMurrayState(p,7);
    if(target) { p->pos1420=PosJtLo(target);SetStepPhysHeadingToward(p->palo,p->pos1420-PosAloCenter(p->palo)); }
}
// orig: 0x001B7970 / 0x001D69F0
void ReleaseMurrayThrow(MURRAY* p) {
    if(!p->plo13F0)return;
    Detach(p); ALO* a=Owner(p);p->plo13F0=nullptr;
    if(!a||!a->fInWorld)return;
    if(FIsLoDerivedFrom(a,0x38))ThrowGuardHeld(a);  // 0x209FB8: job 0xB0 state 3
    const glm::vec3 pos=PosAloCenter(a),d=p->pos1420-pos;
    const float t=glm::length(glm::vec2(d))/p->g1414;
    if(!(0<t))return;
    SOST* s=Psost(a);const glm::vec3 acc=s?s->dvGravity:glm::vec3(0);
    const float z=d.z-(t*(t*0.5f))*acc.z;
    const glm::vec3 velocity(d.x/t,d.y/t,z/t);
    SetAloVelocityTo(a,velocity);SetAloEntryMode(a,2);
    if(std::getenv("SLY2N_CARRYPRINT"))std::fprintf(stderr,"carry-release: frame %d t %.9g pos %.9g %.9g %.9g target %.9g %.9g %.9g vel %.9g %.9g %.9g flight %.9g\n",g_iframeHarness,g_clock.t,pos.x,pos.y,pos.z,p->pos1420.x,p->pos1420.y,p->pos1420.z,velocity.x,velocity.y,velocity.z,t);
}
// orig: 0x001B7AE8 (event 0x1EB in state 8: the put-down): the object let go in front of Murray
void PutDownMurrayCarry(MURRAY* p) {
    if(!p->plo13F0)return;
    Detach(p); ALO* a=Owner(p);p->plo13F0=nullptr;
    if(!a||!a->fInWorld)return;
    if(FIsLoDerivedFrom(a,0x38))ThrowGuardHeld(a);  // 0x209FB8
    SetAloVelocityTo(a,VecJtConvert(p->palo,nullptr,glm::vec3(300.0f,0.0f,-1000.0f)));  // 0x1A6FC0(D_002C09D0), vt slot 35
    SetAloAngVelocityTo(a,glm::vec3(0.0f));  // vt slot 36
    SetAloEntryMode(a,3);  // 0x1F7A68
}
// orig: 0x001B7BA8
void CancelMurrayCarry(MURRAY* p) {
    if(p->state==6) { if(p->psma1530)SetSmaGoal(p->psma1530,0x1CA);return; }
    if(!p->plo13F0)return;
    if(p->state==4&&p->n1450==0) {HandleLoSpliceEvent(p->plo13F0,6,false,0,nullptr);
    BroadcastTaskEvent(g_psw,0x1B,p->plo13F0);p->plo13F0=nullptr;return;}
    Detach(p);
    if(ALO* a=Owner(p)) {
        if(FIsLoDerivedFrom(a,0x38))ReleaseGuardHeld(a);
        else SetAloEntryMode(a,1);
        if(p->state==5)SetAloVelocityTo(a,VecJtConvert(p->palo,nullptr,glm::vec3(-400,0,600)));
        else if(p->state==2||p->state==4||p->state==7||p->state==8)SetAloVelocityTo(a,glm::vec3(0));
    }
    p->plo13F0=nullptr;
}

// orig: 0x001B8450 (slot61): the just released object does not collide with Murray.
// The base actor's carried-gadget modifiers remain in their own systems.
int MurrayCarryOverlapKind(ALO* palo,ALO* other) {
    using PFN=int(*)(ALO*,ALO*);
    PFN base=reinterpret_cast<PFN>(PvNativeFn(0x144388));
    if(!base)base=reinterpret_cast<PFN>(PvNativeFn(0x1F4FC8));
    int kind=base?base(palo,other):0;
    MURRAY* p=PmurrayFromLo(palo);
    if(p&&p->plo1430)
        for(LO* q=other;q;q=q->paloParent)
            if(q==p->plo1430)return std::max(kind,3); // original0x134310
    if(p&&!p->plo1430&&p->state==15)kind=OverlapKindJtThrow(p->jtthrow1910,other,kind); // 0x220498 (+0x1910)
    return kind;
}
NATIVEFN(0x1B8450,MurrayCarryOverlapKind);

// orig: 0x001B52A0..0x001B530C: retire the release exclusion after0.1s once overlap ends.
void UpdateMurrayReleased(MURRAY* p) {
    if(!p->plo1430||!(p->t1434+0.1f<g_clock.t))return;
    ALO* a=dynamic_cast<ALO*>(p->plo1430);
    OXA* own=Poxa(p->palo),*other=a?Poxa(a):nullptr;
    if(!own||!other||!FOverlapOxa(own,other)) {
        p->plo1430=nullptr;
        RecalcSwOxfFilterForObject(p->palo);
    }
}
// Diagnostic observer only: every carry update and the recently released object's first second.
void PrintMurrayCarry(MURRAY* p) {
    if(!std::getenv("SLY2N_CARRYPRINT"))return;
    static std::map<MURRAY*,std::pair<ALO*,float>> observed;
    static uint64_t serial=0;
    if(!g_psw)return;
    if(serial!=g_psw->serial){observed.clear();serial=g_psw->serial;}
    auto& last=observed[p];
    ALO* a=Owner(p);
    if(a)last={a,g_clock.t};
    if(!a&&g_clock.t-last.second<3.0f)a=last.first;
    if(!a)return;
    glm::vec3 pos=PosAloCenter(a),vel=VelAloPos(a);
    std::fprintf(stderr,"carry: frame %d t %.9g state %d phase %d clip %d at %.9g object %d parent %d inworld %d removed %d pos %.9g %.9g %.9g vel %.9g %.9g %.9g\n",g_iframeHarness,g_clock.t,p->state,p->n1450,p->pasega&&p->pasega->paseg?p->pasega->paseg->oid:-1,p->pasega?p->pasega->t:0,a->oid,a->paloParent?a->paloParent->oid:-1,a->fInWorld,a->fRemoved,pos.x,pos.y,pos.z,vel.x,vel.y,vel.z);
    for(size_t i=0;i<a->axf.size();++i){XF* x=a->axf[i].get();std::fprintf(stderr,"carry-xf: frame %d index %zu kind %d mode %d state %d",g_iframeHarness,i,x->kind,x->mode,x->state);for(int c=0;c<4;++c)for(int r=0;r<3;++r)std::fprintf(stderr," %.9g",x->mat[c][r]);std::fputc('\n',stderr);}
}
