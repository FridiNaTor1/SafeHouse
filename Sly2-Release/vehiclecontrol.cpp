#include "actioninput.h"
// Vehicle steering, weapon aiming and state transitions. Matched 0x1D19A8..0x1D4C88.
#include "vehicle.h"
#include "activate.h"
#include "clock.h"
#include "cls.h"
#include "crv.h"
#include "detect.h"
#include "lo.h"
#include "nav.h"
#include "po.h"
#include "screenrt.h"
#include "smrun.h"
#include "so.h"
#include "spliceobj.h"
#include "update.h"
#include "vtnative.h"
#include "xf.h"
#include "xfphys.h"
#include <algorithm>
#include <bit>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>

float GRandInRange(float low,float high);             // orig0x11D448
bool FFloatsNear(float a,float b,float tolerance);     // orig0x11D4D0

namespace {
float Clamp(float v,float low,float high) { return v<low?low:high<v?high:v; }
float Poly(const glm::vec3& p,float x) { return p.x+x*(p.y+x*p.z); }
float Distance2D(const glm::vec3& a,const glm::vec3& b) {
    const glm::vec3 d=a-b; return std::sqrt(d.x*d.x+d.y*d.y);
}
glm::vec3 UnitOr(const glm::vec3& v,const glm::vec3& fallback) {
    const float length=std::sqrt((v.x*v.x+v.y*v.y)+v.z*v.z);
    return length<0.0001f?fallback:v*(1.0f/length);
}
bool InRange(const cmm::LM& range,float x) { return range.gMin<x && x<range.gMax; }
glm::mat4 ObjectMatrix(LO* object) {
    if (ALO* alo=dynamic_cast<ALO*>(object)) {
        if (XF* xf=PxfObj(alo)) return xf->matWorld;
    } else if (object && FCidDerivedFrom(object->Cid(),3)) return MatWorldLox(object);
    return glm::mat4(1);
}
glm::vec3 ObjectVelocity(LO* object,int slot) {
    using GET=glm::vec3(*)(LO*);
    auto get=reinterpret_cast<GET>(PvNativeVt(object,slot));
    return get?get(object):glm::vec3(0);
}
void AddVehicleVelocity(ALO* owner,int getSlot,int setSlot,const glm::vec3& delta) {
    using SET=void(*)(ALO*,const glm::vec3&);
    if (auto set=reinterpret_cast<SET>(PvNativeVt(owner,setSlot))) set(owner,ObjectVelocity(owner,getSlot)+delta);
}

// orig: 0x001D4B28 (changing AI state records its entry time).
void SetVehicleAiState(VEHICLE& v,int state) {
    if (state!=v.aiState) { v.aiState=state; v.aiTime=g_clock.t; }
}

// orig: 0x001D2240 (the first usable attachment whose tactic is not 4).
VEHICLEATTACH* FirstReadyAttachment(VEHICLE& v) {
    for (const auto& a:v.attachments)
        if (FVehicleAttachmentReady(*a) && a->tactic!=4) return a.get();
    return nullptr;
}

// orig: 0x001D22D0 (global class query, sixteen candidates, planar distance plus resource priority).
LO* BestVehiclePickup(VEHICLE& v) {
    BASIC* objects[16]{};
    const int count=g_splicegame.pfnFindObjectsByClass?
        g_splicegame.pfnFindObjectsByClass(0x105,0x6c,nullptr,16,objects):0;
    LO* best=nullptr; float score=FLT_MAX;
    const glm::vec3 position=PosLoWorld(v.owner);
    for(int i=0;i<count;++i) {
        LO* object=dynamic_cast<LO*>(objects[i]); if (!object) continue;
        const int kind=VehiclePickupKind(object);
        if (kind<0 || kind>=int(v.resources.size())) continue;
        float next=Distance2D(PosLoWorld(object),position);
        if (object==v.selectedObject) next=Poly({-200,0.8f,0},next); // D_002C20C0
        next+=v.resources[kind].priority;
        if (next<score) { score=next; best=object; }
    }
    return best;
}

// The original PATH stores its editable points and cached lengths together.
void StoreVehiclePath(VEHICLE& v,const CRV& curve) {
    if (!v.path) return;
    v.path->fE0=curve.crvk; v.path->nE4=curve.fClosed; v.path->avecF4=curve.apos;
    v.path->an.resize(curve.ag.size());
    for(size_t i=0;i<curve.ag.size();++i) v.path->an[i]=std::bit_cast<int32_t>(curve.ag[i]);
    InvalidateCrvOfLo(v.path);
    (void)PcrvFromLo(v.path);
}

// orig: 0x001D2420. Flatten after path lengths were calculated, without recalculating them.
void ResetVehiclePathHeight(VEHICLE& v) {
    v.cursor=0;
    if (!v.path) return;
    const float height=PosLoWorld(v.owner).z;
    CRV* curve=const_cast<CRV*>(PcrvFromLo(v.path));
    for(glm::vec3& p:v.path->avecF4) p.z=height;
    if (curve) for(glm::vec3& p:curve->apos) p.z=height;
}

void NavigateVehicle(VEHICLE& v,const glm::vec3& start,const glm::vec3& end) {
    NAVREGION* region=PregionOfMesh(v.navigator);
    if (region && v.path) {
        CRV curve;
        if (const CRV* current=PcrvFromLo(v.path)) curve=*current;
        if (FNavPathCrv(region,start,end,&curve)) StoreVehiclePath(v,curve);
    }
    ResetVehiclePathHeight(v);
}

// orig: 0x001D2490 (nearest point, distance-dependent look-ahead and 75% driving speed).
void FollowVehiclePath(VEHICLE& v) {
    const CRV* curve=PcrvFromLo(v.path);
    if (!curve) { v.drive=0; return; }
    const glm::vec3 position=PosLoWorld(v.owner);
    glm::vec3 point,direction;
    NearestCrvS(curve,position,v.cursor,&point,&direction,nullptr,&v.cursor);
    const float lead=Clamp(Poly({150,-1,0},Distance2D(position,point)),0,150); // D_002C20D0/E0
    const glm::vec3 goal=UnitOr((point+direction*lead)-position,direction);
    v.goalHeading=std::atan2(goal.y,goal.x); v.drive=v.maxSpeed*0.75f;
}

bool NeedsVehiclePath(VEHICLE& v,const glm::vec3& destination) {
    return !v.path || v.path->avecF4.size()<2 || 50<Distance2D(v.path->avecF4.back(),destination);
}

void AttachmentAimError(VEHICLE& v,const VEHICLEATTACH& a,float* pan,float* pitch) {
    const glm::mat4 matrix=ObjectMatrix(a.anchor);
    const float heading=std::atan2(matrix[0].y,matrix[0].x);
    *pan=std::fabs(cmm::RadNormalize(heading-v.wheelAngle));
    *pitch=VecXfValue(a.mesh,a.mesh->mat).z-(a.meshBinding?a.meshBinding->pos.z:0);
}

// orig: 0x001D2610 (stable AI state, movement tactic, aimed attack, then opportunistic weapons).
void UpdateVehicleAi(VEHICLE& v) {
    if (!v.target) { v.drive=0; return; }
    const glm::vec3 position=PosLoWorld(v.owner), target=PosLoWorld(v.target), delta=target-position;
    const float distance=Distance2D(target,position);
    v.selectedAttach=FirstReadyAttachment(v);
    v.selectedObject=v.selectedAttach?nullptr:BestVehiclePickup(v);
    int old;
    do {
        old=v.aiState;
        switch(old) {
        case 0:
            if (v.selectedAttach) { if (InRange(v.selectedAttach->enterRange,distance)) SetVehicleAiState(v,1); }
            else SetVehicleAiState(v,v.selectedObject?2:3);
            break;
        case 1: if (!v.selectedAttach || !InRange(v.selectedAttach->exitRange,distance)) SetVehicleAiState(v,0); break;
        case 2: if (!v.selectedObject) SetVehicleAiState(v,0); break;
        case 3: if (v.selectedAttach || v.selectedObject) SetVehicleAiState(v,0); break;
        }
    } while(old!=v.aiState);
    switch(v.aiState) {
    case 0: {
        VEHICLEATTACH& a=*v.selectedAttach;
        const float angle=std::acos(a.radius/std::max(a.radius*1.25f,distance)); // D_002C20E8
        const glm::vec3 offset=UnitOr(position-target,{1,0,0})*a.radius;
        glm::vec3 goal=target;
        if (a.tactic==2 || a.tactic==3)
            goal=target+cmm::BuildRotateMatrixAxis(a.tactic==2?angle:-angle,{0,0,1})*offset;
        NAVSEG hits[4]; NAVREGION* region=PregionOfMesh(v.navigator);
        if (region && v.path && CsegNav(region,position,goal,4,hits,2)==1 && hits[0].t0==0 && hits[0].t1==1) {
            CRV curve; curve.apos={position,goal}; curve.ag={0,1};
            RecalcCrv(&curve,glm::mat4(1)); StoreVehiclePath(v,curve); ResetVehiclePathHeight(v);
        } else if (NeedsVehiclePath(v,target)) NavigateVehicle(v,position,target);
        FollowVehiclePath(v); v.wheelAngle=v.heading;
        break;
    }
    case 1:
        v.drive=0; v.goalHeading=v.heading; v.wheelAngle=std::atan2(delta.y,delta.x);
        if (v.nextAttack<g_clock.t) if (VEHICLEATTACH* a=FirstReadyAttachment(v)) {
            bool aim=true;
            if (a->mesh) { float pan,pitch; AttachmentAimError(v,*a,&pan,&pitch); aim=pan<0.3f && std::fabs(pitch)<0.3f; }
            if (aim) { SetSmaGoal(a->binding,0x68); v.nextAttack=g_clock.t+GRandInRange(1,1.5f); }
        }
        break;
    case 2: {
        const glm::vec3 point=PosLoWorld(v.selectedObject);
        if (NeedsVehiclePath(v,point)) NavigateVehicle(v,position,point);
        FollowVehiclePath(v); v.wheelAngle=v.heading; break;
    }
    case 3:
        v.goalHeading=std::atan2(-delta.y,-delta.x); v.wheelAngle=v.heading; v.drive=v.maxSpeed*0.85f; break;
    }
    if (g_clock.t>v.nextAttack) for (const auto& p:v.attachments) {
        VEHICLEATTACH& a=*p;
        if (!FVehicleAttachmentReady(a)) continue;
        if (a.constraint && !FPointInShape(a.constraint,target)) continue;
        if (a.enterRange.gMax>0 && v.aiState!=1) continue;
        if (a.mesh) {
            float pan,pitch; AttachmentAimError(v,a,&pan,&pitch);
            if (pan>0.3f || std::fabs(pitch)>0.3f) continue;
        }
        SetSmaGoal(a.binding,0x68);
        v.nextAttack=g_clock.t+GRandInRange(a.fireInterval.gMin,a.fireInterval.gMax);
    }
}

// orig: 0x001D2150 (camera-relative heading, held CROSS drive, held/pressed weapon inputs).
void UpdateVehiclePlayer(VEHICLE& v) {
    PODATA* po=PpodataFromLo(v.owner); PADCTL* pad=po?po->ppadctl:nullptr;
    if (!pad) return;
    v.goalHeading=pad->radJoy;
    if (pad->grfbtn&0x40) { v.drive=v.maxSpeed; v.wheelAngle=v.heading; }
    else { v.wheelAngle=pad->radJoy; v.drive=0; }
    for (const auto& a:v.attachments) if (FVehicleAttachmentReady(*a)) {
        const auto action = sly2actions::VehicleWeaponAction(a->heldButtons | a->pressedButtons);
        if (a->heldButtons && sly2actions::Held(pad, action, a->heldButtons)) SetSmaGoal(a->binding,0x67);
        else if (a->pressedButtons && sly2actions::Take(pad, action, a->pressedButtons,0)) SetSmaGoal(a->binding,0x68);
    }
}
}

// orig: 0x001D19A8. Its base ALO update runs in vehicle.cpp before this body.
void UpdateVehicleControl(VEHICLE& v) {
    const glm::mat4 object=ObjectMatrix(v.owner);
    v.heading=std::atan2(object[0].y,object[0].x);
    for(const auto& a:v.attachments) if (!FVehicleAttachmentReady(*a)) SetSmaGoal(a->binding,0x65);
    // (debug, tests only: SLY2N_VEHICLEHP=n caps an AI vehicle's health at n, so a few real hits end a battle)
    if (static const char* s_sz=std::getenv("SLY2N_VEHICLEHP"); s_sz && v.mode==1 && v.health>std::atoi(s_sz)) {
        v.health=std::atoi(s_sz); UpdateVehicleHealth(v);
    }
    switch(v.mode) {
    case 0: case 3: v.turn=0; v.drive=0; v.goalHeading=v.heading; break;
    case 2: UpdateVehiclePlayer(v); break;
    case 1: UpdateVehicleAi(v); break;
    }
    const float relative=cmm::RadNormalize(v.goalHeading-v.heading);
    v.steering=Clamp(Poly({0,0.8f,0},relative),-0.5f,0.5f);
    v.acceleration=Clamp(Poly({1.5f,-0.5f,0},std::fabs(relative)),0.3f,1);
    const float response=Clamp(Poly({-0.5f,1,0},std::fabs(relative)),1,2.5f);
    v.turn=((response*std::min(500.0f,v.drive))*v.steering)/v.width;
    if (object[2].z>0.5f) v.lastCorrection=g_clock.t;
    for(VEHICLESIDE& side:v.sides) if (side.node && side.controller) {
        const glm::vec3 direction=VecConvertXfs(nullptr,side.node->pxfRef,cmm::SetVectorCylind(v.wheelAngle,1,0));
        const float angle=std::atan2(direction.y,direction.x);
        SetCtrlPos(side.controller,{0,0,cmm::GLimitLm(&side.limits,angle),0});
    }
    static const cmm::SMP wheelSmoothing{10,0,0.25f}; // D_002C2098
    for(VEHICLEWHEEL& wheel:v.wheels)
        wheel.tilt=cmm::RadSmooth(wheel.tilt,v.drive!=0?wheel.steerFraction*v.steering:0,g_clock.dt,&wheelSmoothing,nullptr);
    if (v.target) {
        const glm::vec3 target=PosLoWorld(v.target), position=PosLoWorld(v.owner);
        const glm::vec3 velocity=ObjectVelocity(v.target,33);
        const float distance=Distance2D(target,position);
        for(const auto& p:v.attachments) {
            VEHICLEATTACH& a=*p; if (!a.mesh) continue;
            if (v.drive!=0 || !FVehicleAttachmentReady(a)) { a.active=false; a.speed=a.speedLimits.gMin; a.angle=0; }
            else {
                const float time=cmm::GLimitLm(&a.flightLimits,Poly(a.flightTime,distance));
                const glm::vec3 origin=a.anchor?PosLoWorld(a.anchor):glm::vec3(a.mesh->matWorld[3]);
                const glm::vec3 shot=(velocity-glm::vec3(0,0,velocity.z))+(target-origin)*(1.0f/time)+glm::vec3(0,0,a.gravity*(-0.5f*time));
                const float pitch=std::atan2(shot.z,std::sqrt(shot.x*shot.x+shot.y*shot.y));
                const float speed=std::sqrt((shot.x*shot.x+shot.y*shot.y)+shot.z*shot.z);
                a.angle=cmm::GLimitLm(&a.angleLimits,pitch); a.speed=cmm::GLimitLm(&a.speedLimits,speed);
                a.active=FFloatsNear(a.angle,pitch,0.01f) && FFloatsNear(a.speed,speed,0.01f);
                if (!a.active) { a.angle=0; a.speed=a.speedLimits.gMin; }
            }
            if (a.meshBinding) SetCtrlPos(a.meshBinding,{0,-a.angle,0,0});
        }
    }
    if (v.sound) {
        static const cmm::SMP soundSmoothing{1,0,0.25f}; // D_002C20A8
        v.soundRate=cmm::GSmooth(v.soundRate,v.drive/400,g_clock.dt,&soundSmoothing,nullptr);
        const float rate=Clamp(v.soundRate,0.2f,1);
        v.sound->SetVolume(rate); v.sound->SetPitch(rate*0.2f);
        if (v.sound->m_cpause>0) v.sound->Resume();
    }
    if (v.drive!=0 || v.turn!=0) WakeAlo(v.owner);
    if (std::getenv("SLY2N_VEHICLECONTROL")) {
        std::printf("vehicle control t %.9g owner %d instance %d mode %d ai %d heading %.9g goal %.9g steering %.9g wheel %.9g drive %.9g turn %.9g acceleration %.9g selected %d attachment %d cursor %.9g next %.9g\n",
            g_clock.t,v.owner->oid,v.owner->oidInstance,v.mode,v.aiState,v.heading,v.goalHeading,v.steering,v.wheelAngle,
            v.drive,v.turn,v.acceleration,v.selectedObject?v.selectedObject->oid:-1,v.selectedAttach?v.selectedAttach->index:-1,v.cursor,v.nextAttack);
        for(const auto& a:v.attachments) std::printf("vehicle aim t %.9g owner %d index %d active %d angle %.9g speed %.9g\n",
            g_clock.t,v.owner->oid,a->index,a->active,a->angle,a->speed);
    }
}

// orig: 0x001D4C88 (the authored HUD bar can display remaining or depleted health).
void UpdateVehicleHealth(VEHICLE& v) {
    if (!v.healthWidget) return;
    float fraction=float(v.health)/float(v.maxHealth);
    if (v.invertHealth) fraction=1-fraction;
    SetScreenObjFraction(v.healthWidget,0x34f,fraction);
}

// orig: 0x001D4940 (exit behavior, timestamp, entry behavior, then authored state-machine seek).
void SetVehicleMode(VEHICLE& v,int mode) {
    if (mode==v.mode) return;
    switch(v.mode) {
    case 0:
        if (v.healthWidget) { ShowScreenObj(v.healthWidget,-1); SelectScreenObjPart(v.healthWidget,0x34f); UpdateVehicleHealth(v); }
        if (v.soundId!=-1) {
            CSnd* sound=PsndNewPlain(v.soundId); sound->SetSource(v.owner); v.sound=CSndRef(sound);
            v.sound->SetFalloff(500,1500); v.sound->Pause();
        }
        break;
    case 1: SetVehicleAiState(v,-1); break;
    }
    v.mode=mode; v.modeTime=g_clock.t;
    switch(mode) {
    case 0:
        if (v.healthWidget) HideScreenObj(v.healthWidget);
        if (v.sound) { v.sound->Stop(0); v.sound=CSndRef(); }
        break;
    case 1: SetVehicleAiState(v,0); break;
    // vt slot 3 (5, immediate, no arguments): the battle's own event 5 (e.g. Ep7 RC Combat Club's block 38479 asks
    // for task 3081 state 2 on the AI car's defeat; g_pfnLoEvent is the contact event (two arguments) and dropped it: N30)
    case 3: HandleLoSpliceEvent(v.owner,5,false,0,nullptr); break;
    }
    static const int states[]={115,116,117,118}; // D_002C2208
    if (v.binding && mode>=0 && mode<4) SeekSma(v.binding,states[mode]);
}

// orig: 0x001D4568 (base contact acceptance, health and the original ordered random impulse draws).
bool AcceptVehicleContact(ALO* owner,CTMSG* event) {
    using ACCEPT=bool(*)(ALO*,CTMSG*);
    auto base=reinterpret_cast<ACCEPT>(PvNativeFn(0x1f7588));
    if (!base || !base(owner,event)) return false;
    VEHICLE* v=Pvehicle(owner); if (!v) return false;
    v->health=std::max(0,v->health+event->nState); UpdateVehicleHealth(*v);
    if (!v->health) SetVehicleMode(*v,3);
    glm::vec3 linear(0),angular(0);
    if (event->grfFilter==0x100) {
        const float angle=GRandInRange(-cmm::PI,cmm::PI);
        const float horizontal=GRandInRange(50,100), vertical=GRandInRange(300,400);
        linear=cmm::SetVectorCylind(angle,horizontal,vertical);
        angular=UnitOr(glm::cross(glm::vec3(0,0,1),linear),{1,0,0})*GRandInRange(2,5);
    } else if (event->grfFilter==0x1000 && event->ploSender) {
        glm::vec3 direction=PosLoWorld(owner)-PosLoWorld(event->ploSender); direction.z=0;
        direction=UnitOr(direction,{1,0,0});
        const float horizontal=GRandInRange(50,100), vertical=GRandInRange(50,75);
        linear=direction*horizontal+glm::vec3(0,0,vertical);
        const float pitch=GRandInRange(2.5f,3), spin=GRandInRange(-0.5f,0.5f);
        angular=glm::cross(glm::vec3(0,0,1),direction)*pitch+glm::vec3(0,0,spin);
    }
    AddVehicleVelocity(owner,33,35,linear);
    AddVehicleVelocity(owner,34,36,angular);
    return true;
}
NATIVEFN(0x1D4568,AcceptVehicleContact);
