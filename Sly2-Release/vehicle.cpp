// Wheeled vehicle runtime, class0x4D. Matched source 0x1D1370..0x1D4FF8.
#include "vehicle.h"
#include "worldglobals.h"
#include "alo.h"
#include "animfind.h"
#include "brx.h"
#include "clock.h"
#include "objmethod.h"
#include "sm.h"
#include "smrun.h"
#include "spliceglue.h"
#include "so.h"
#include "sw.h"
#include "update.h"
#include "vtnative.h"
#include "worldaccess.h"
#include "xf.h"
#include "xfphys.h"
#include <algorithm>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>
#include <string>

extern int g_cXfStamp;
uint32_t GrfUpdOfLo(const LO*);
glm::mat4 InverseAffineMatrix4(const glm::mat4&);
glm::mat3 LevelMatrix3(const glm::mat3&, glm::mat3*);
glm::vec3 DvAloImpulse(ALO*, const glm::vec3&);
glm::vec3 DwAloImpulseAt(ALO*, const glm::vec3&, const glm::vec3&);

namespace {
const glm::vec3 g_normalZ{0,0,1},g_normalY{0,1,0},g_vecZero{0};
glm::mat3 Mat3Of(const glm::mat4& m) { return glm::mat3(m); }
glm::vec3 PosOf(const glm::mat4& m) { return glm::vec3(m[3]); }
glm::vec3 XformPos(const glm::mat4& m,const glm::vec3& p) { return glm::vec3(m*glm::vec4(p,1)); }
void SetPos(glm::mat4* m,const glm::vec3& p) { (*m)[3]=glm::vec4(p,1); }
void SetMat3(glm::mat4* m,const glm::mat3& r) { for (int i=0;i<3;++i) (*m)[i]=glm::vec4(r[i],0); }
using cmm::BuildRotateMatrixAxis;
std::unordered_map<const LO*, VEHICLE> s_vehicles;
WORLD_TABLE(s_vehicles);   // (PC: moved aside while a kept world loads, worldglobals.h)
float Dot(const glm::vec3& a, const glm::vec3& b) { return (a.x*b.x+a.y*b.y)+a.z*b.z; }
float Poly(const glm::vec3& p, float x) { return p.x+x*(p.y+x*p.z); }
float Clamp(float x, float lo, float hi) { return x<lo?lo:hi<x?hi:x; }
XF* FindNode(ALO* p, int id, int kind) {
    for (const auto& node : p->axf) if (node->tag==id && node->kind==kind) return node.get();
    return nullptr;
}

// orig0x1D2DD0: largest positive support depth in this wheel's separate list.
XP* BestContact(const VEHICLEWHEEL& wheel) {
    XP* best=nullptr; float depth=0;
    for (XP* hit=wheel.contacts; hit; hit=hit->pxpNext)
        if (depth<hit->gDist) { depth=hit->gDist; best=hit; }
    return best;
}

void TraceVehicle(const VEHICLE& v,const char* phase) {
    if (!std::getenv("SLY2N_VEHICLETRACE")) return;
    std::string line;
    auto append=[&](const char* format,auto... values) { char part[2048]; std::snprintf(part,sizeof(part),format,values...); line+=part; };
    const glm::vec3 position=PosAloCenter(v.owner),velocity=VelAloPos(v.owner);
    append("vehicle state {\"phase\":\"%s\",\"time\":%.9g,\"instance\":%d,\"mode\":%d,\"ai\":%d,"
        "\"position\":[%.9g,%.9g,%.9g],\"velocity\":[%.9g,%.9g,%.9g],\"heading\":%.9g,\"goal\":%.9g,"
        "\"drive\":%.9g,\"turn\":%.9g,\"health\":%d,\"resources\":[%d,%d,%d],\"wheels\":[",
        phase,g_clock.t,v.owner->oidInstance,v.mode,v.aiState,position.x,position.y,position.z,velocity.x,velocity.y,velocity.z,
        v.heading,v.goalHeading,v.drive,v.turn,v.health,v.resources[0].count,v.resources[1].count,v.resources[2].count);
    for (size_t i=0;i<v.wheels.size();++i) {
        const VEHICLEWHEEL& w=v.wheels[i]; XP* best=BestContact(w); int count=0;
        for (XP* hit=w.contacts;hit;hit=hit->pxpNext) ++count;
        append("%s{\"id\":%d,\"contacts\":%d,\"depth\":%.9g,\"tilt\":%.9g,\"speed\":%.9g,\"angle\":%.9g}",
            i?",":"",w.id,count,best?best->gDist:0,w.tilt,w.speed,w.angle);
    }
    append("],\"physics\":{\"flags\":%u,\"position_mode\":%d,\"rotation_mode\":%d,\"mass\":%.9g},\"attachments\":[",GrfUpdOfLo(v.owner),PxfPos(v.owner)?PxfPos(v.owner)->mode:-1,PxfRot(v.owner)?PxfRot(v.owner)->mode:-1,Psost(v.owner)?Psost(v.owner)->m:0);
    for (size_t i=0;i<v.attachments.size();++i) {
        const VEHICLEATTACH& a=*v.attachments[i];
        append("%s{\"index\":%d,\"ready\":%d,\"active\":%d,\"angle\":%.9g,\"speed\":%.9g}",
            i?",":"",a.index,FVehicleAttachmentReady(a),a.active,a.angle,a.speed);
    }
    append("]}\n");
    std::fwrite(line.data(),1,line.size(),stdout); std::fflush(stdout);
}

// orig0x1D3450: prune the obstacle BSP, intersect its edges and six wheel
// profile segments, then pair supports by surface in the two original passes.
void MarkWheel(VEHICLE& vehicle, VEHICLEWHEEL& wheel, SOE* entry, const BSPREF& tree, int root) {
    ALO* owner=vehicle.owner;
    if (!PxfObj(owner) || owner->asoe.empty()) return;
    glm::mat4 frame=PxfObj(owner)->matWorld;
    SetPos(&frame,PosConvertXfs(PxfObj(owner),nullptr,wheel.lower));
    const glm::mat4 inverse=InverseAffineMatrix4(frame);
    const float height=wheel.top-wheel.bottom, radius=wheel.radius;
    const glm::vec4 plane=PlaneFromNormalPos(glm::vec3(frame[1]),PosOf(frame));
    std::vector<BSPN> nodes; int left=0x1555;
    if (ClipBspPlane(tree,root,INT_MAX,PosOf(frame),std::sqrt(radius*radius+height*height),&left,&nodes)!=2) return;
    const int prunedRoot=int(nodes.size())-1;
    const GEOM& geom=entry->geomWorld;
    struct SUPPORT { glm::vec3 pos{0}; const SURF* face[2]{}; bool used=false; };
    std::vector<SUPPORT> support; support.reserve(64);
    for (const BSPN& node:nodes)
        if (node.isurf>=0) geom.asurf[node.isurf].grfMark|=0x400;
    for (const BSPN& node:nodes) {
        if (node.isurf<0) continue;
        const SURF* face=&geom.asurf[node.isurf];
        if (face->grfMark&0x800) continue;
        face->grfMark|=0x800;
        for (int index=face->iedgeFirst;index>=0;index=geom.aedge[index].iedgeNextInSurf) {
            const EDGE& edge=geom.aedge[index];
            const SURF* other=&geom.asurf[edge.isurf==node.isurf?edge.isurfOther:edge.isurf];
            if (!(other->grfMark&0x400)) continue;
            const glm::vec3 a=geom.apos[edge.ipos0], b=geom.apos[edge.ipos1];
            const float da=PlaneDist(plane,a), db=PlaneDist(plane,b), u=da/(da-db);
            if (u<0 || 1<u) continue;
            const glm::vec3 local=XformPos(inverse,a*(1-u)+b*u);
            if (local.z>0) { if (radius<std::fabs(local.x)) continue; }
            else if (radius*radius<Dot(local,local)) continue;
            SUPPORT s; s.pos=local;
            if (Dot(glm::vec3(plane),a-b)<0) { s.face[0]=face; s.face[1]=other; }
            else { s.face[0]=other; s.face[1]=face; }
            support.push_back(s);
        }
    }
    for (const BSPN& node:nodes)
        if (node.isurf>=0) geom.asurf[node.isurf].grfMark&=~0xc00;
    // D_002C2150 (five points of the lower wheel outline).
    static const glm::vec3 profile[]={{-1,0,0},{-0.707f,0,-0.707f},{0,0,-1},{0.707f,0,-0.707f},{1,0,0}};
    glm::vec3 previous=XformPos(frame,profile[0]*radius+g_normalZ*height);
    const BSPREF pruned(nodes,geom);
    for (int j=0;j<6;++j) {
        const glm::vec3 direction=j<5?profile[j]*radius:profile[4]*radius+g_normalZ*height;
        const glm::vec3 position=XformPos(frame,direction);
        LSG segments[16];
        const int count=ClipSegmentBsp(pruned,prunedRoot,INT_MAX,previous,position,16,segments);
        for (int k=0;k<count;++k) {
            const LSG& segment=segments[k];
            if (segment.psurf0) { SUPPORT s; s.face[1]=segment.psurf0; s.pos=XformPos(inverse,segment.pos0); support.push_back(s); }
            if (segment.psurf1) { SUPPORT s; s.face[0]=segment.psurf1; s.pos=XformPos(inverse,segment.pos1); support.push_back(s); }
        }
        previous=position;
    }
    float best=0; glm::vec3 bestPoint{0};
    for (int pass=0;pass<2;++pass) for (SUPPORT& s:support) {
        float lower,upper;
        if (pass==0) {
            if (!s.face[1]) continue;
            lower=s.face[0]?s.pos.x:-FLT_MAX; upper=FLT_MAX;
            SUPPORT* partner=nullptr;
            for (SUPPORT& other:support)
                if (other.face[0]==s.face[1] && lower<other.pos.x && other.pos.x<upper) {
                    upper=other.pos.x; partner=&other;
                }
            if (partner) partner->used=true;
        } else {
            if (!s.face[0] || s.used) continue;
            lower=-FLT_MAX; upper=s.face[1]?s.pos.x:FLT_MAX;
        }
        // orig0x1193D8: transform a (normal, distance) plane into wheel space.
        const SURF& face=*s.face[1-pass];
        const glm::vec3 normal=Mat3Of(inverse)*face.normal;
        const glm::vec3 onPlane=PosOf(inverse)+normal*face.gDot;
        const glm::vec4 local(normal,Dot(onPlane,normal));
        if (local.z<0) continue;
        const float slope=-local.x/local.z;
        float x=std::sqrt(radius*radius*slope*slope/(slope*slope+1));
        if (slope<0) x=-x;
        x=Clamp(x,lower,upper);
        const float z=-std::sqrt(radius*radius-x*x);
        const float amount=std::min(-PlaneDist(local,{x,0,z})/local.z,height);
        if (best<amount) { best=amount; bestPoint={x,0,z+best}; }
    }
    if (best>0) PxpAddSw(&owner->asoe.front(),entry,XformPos(frame,bestPoint),glm::vec3(plane),10,nullptr,nullptr,&wheel.contacts,best);
}

// orig0x1D3E48: only support against a stationary other physics root (slot62).
void VehicleOwnContacts(ALO* owner, SOE* entry, int, const BSPREF& tree, int root, XP**) {
    VEHICLE* vehicle=Pvehicle(owner);
    if (!vehicle || !entry || !entry->palo || !Poxa(entry->palo)) return;
    ALO* other=Poxa(entry->palo)->pso;
    using STILL=bool(*)(ALO*);
    auto still=reinterpret_cast<STILL>(PvNativeVt(other,62));
    if (!(still?still(other):FAloStill(other))) return;
    for (VEHICLEWHEEL& wheel:vehicle->wheels) MarkWheel(*vehicle,wheel,entry,tree,root);
}
NATIVEFN(0x1D3E48,VehicleOwnContacts);

// orig0x1D3F30/0x1F4DA8: discard only this pair's per-wheel contacts.
void VehicleRemoveContacts(ALO* owner, ALO* other) {
    VEHICLE* vehicle=Pvehicle(owner); if (!vehicle) return;
    for (VEHICLEWHEEL& wheel:vehicle->wheels) {
        XP** cursor=&wheel.contacts; XP* removed=nullptr;
        while (XP* hit=*cursor) {
            const int side=hit->axpd[0].paloRoot==owner?1:0;
            if (hit->axpd[side].paloRoot==other) {
                *cursor=hit->pxpNext; hit->pxpNext=removed; removed=hit;
            } else cursor=&hit->pxpNext;
        }
        if (removed) { WakeAlo(owner); FreeSwXpList(removed); }
    }
}
NATIVEFN(0x1D3F30,VehicleRemoveContacts);

// orig0x1D4178 ->0x1F50E0, then0x1F5480 for each wheel's lower position.
void VehicleBounds(ALO* owner) {
    UpdateAloBounds(owner); VEHICLE* vehicle=Pvehicle(owner); OXA* bounds=Poxa(owner);
    if (!vehicle || !bounds) return;
    for (const VEHICLEWHEEL& wheel:vehicle->wheels) {
        const glm::vec3 point=PosConvertXfs(PxfObj(owner),nullptr,wheel.lower);
        bounds->posMin=glm::min(bounds->posMin,point-glm::vec3(wheel.radius));
        bounds->posMax=glm::max(bounds->posMax,point+glm::vec3(wheel.radius));
        // 0x1F5480 compares centre distance only, without adding the radius.
        bounds->sRadius=std::max(bounds->sRadius,std::sqrt(Dot(point-bounds->posCenter,point-bounds->posCenter)));
    }
}
NATIVEFN(0x1D4178,VehicleBounds);

// orig0x1FAB78 adds the linear/angular force response, without the velocity-mode
// guard of the independent acceleration helpers.
void ForceAt(ALO* owner,const glm::vec3& point,const glm::vec3& force) {
    const glm::vec3 linear=DvAloImpulse(owner,force), angular=DwAloImpulseAt(owner,point,force);
    XF* position=PxfPos(owner); XF* rotation=PxfRot(owner);
    position->aseg[3]+=glm::vec4(VecConvertXfs(nullptr,position->pxfRef,linear),0);
    rotation->aseg[3]+=glm::vec4(VecConvertXfs(nullptr,rotation->pxfRef,angular),0);
}
// orig0x1FAC98.
void AccelRotation(ALO* owner,const glm::vec3& acceleration) {
    XF* rotation=PxfRot(owner);
    if (FXfModeVelocity(rotation->mode)) rotation->aseg[3]+=glm::vec4(VecConvertXfs(nullptr,rotation->pxfRef,acceleration),0);
}

// orig0x1D2E18: gravity, suspension, turning budget, then longitudinal traction.
void VehicleForces(ALO* owner) {
    ApplyAloGravity(owner);
    VEHICLE* vehicle=Pvehicle(owner); SOST* physics=Psost(owner);
    XF* position=PxfPos(owner); XF* rotation=PxfRot(owner); XF* space=PxfObj(owner);
    if (!vehicle || !physics || !position || !rotation || !space) return;
    VEHICLE& v=*vehicle;
    if (g_clock.t-v.lastCorrection>3) {
        glm::mat3 level; LevelMatrix3(Mat3Of(space->matWorld),&level);
        static const CLQ correction{0,15,1,0}; // D_002C2100
        AccelAloTowardRotation(owner,&level,&correction,g_vecZero,nullptr);
    }
    float total=0;
    for (const VEHICLEWHEEL& wheel:v.wheels) if (XP* hit=BestContact(wheel)) {
        const glm::vec3 point=hit->axpd[0].pos, normal=glm::vec3(space->matWorld[2]);
        const glm::vec3 previous=VelXfPoint(space,nullptr,point);
        const glm::vec3 other=VelXfPoint(hit->axpd[1].pxf,nullptr,point);
        const float fraction=Clamp(hit->gDist/(wheel.top-wheel.bottom),0,1);
        const float velocityCurve=Poly({0,0.25f,0.75f},fraction),depthCurve=fraction*fraction;
        const float response=Dot(other-previous,normal)*velocityCurve*15+hit->gDist*depthCurve*90;
        if (response<0) continue;
        const glm::vec3 force=normal*(response*physics->m);
        ForceAt(owner,point,force); total+=Dot(force,normal);
    }
    float budget=std::min(11250.0f,v.contactScale*total/physics->m); // D_002C2140
    const float dt=g_clock.dt;
    const glm::vec3 velocity=VecConvertXfs(position->pxfRef,rotation,glm::vec3(position->vel)+glm::vec3(position->aseg[3])*dt);
    const glm::vec3 localVelocity=glm::transpose(Mat3Of(rotation->mat))*glm::vec3(rotation->vel);
    const float acceleration=0.5f*physics->m*v.width/physics->momint[2][2];
    const float turnImpulse=cmm::GLimitAbs((v.turn-localVelocity.z)/dt,acceleration*budget);
    budget-=std::fabs(0.25f*turnImpulse/acceleration);
    AccelRotation(owner,Mat3Of(rotation->matWorld)*glm::vec3(0,0,turnImpulse));
    const glm::mat3 phase=BuildRotateMatrixAxis((localVelocity.z+turnImpulse*(dt*0.5f))*dt,g_normalZ);
    const glm::vec3 desired=phase*glm::vec3(v.acceleration*v.drive,0,0);
    const float fraction=velocity.x/v.maxSpeed;
    float minimum,maximum;
    if (v.drive>=0) { minimum=-v.reverseLimit; maximum=cmm::GLimitLm(&v.tractionLimits,Poly(v.traction,Clamp(fraction,0,1))); }
    else { minimum=-cmm::GLimitLm(&v.tractionLimits,Poly(v.traction,Clamp(-fraction,0,1))); maximum=v.reverseLimit; }
    glm::vec3 correction=(desired-velocity)/dt;
    correction.z=0; correction.x=Clamp(correction.x,minimum,maximum);
    AccelAloPos(owner,VecConvertXfs(rotation,nullptr,cmm::LimitVectorLength(correction,budget)));
    TraceVehicle(v,"forces");
}
NATIVEFN(0x1D2E18,VehicleForces);

// orig0x1D14C0: shared post-load/placement vehicle state.
void ResetVehicleState(LO* owner) {
    VEHICLE* v=Pvehicle(owner); if (!v) return;
    v->health=v->maxHealth;
    UpdateVehicleHealth(*v);
    if (v->binding && v->machine && !v->machine->asms.empty()) SnapSmaGoal(v->binding,v->machine->asms.front().oid);
    SetVehicleMode(*v,0);
}
// orig0x1D1910 ->0x1CB518: AttachProxy already copied the activation mask.
// The original +0xCC model-packet tick has no native counterpart; scene.cpp
// submits each instance directly. Preserve the physics and child reset work.
void ResetVehiclePlacement(LO* owner,LO* proxy) {
    auto* alo=static_cast<ALO*>(owner);
    SetAloEntryMode(alo,0);
    if (Pregent(alo)) UpdateAloRegion(alo);
    using RESET=void(*)(LO*,LO*);
    for (LO* child:alo->aploChild)
        if (auto reset=reinterpret_cast<RESET>(PvNativeVt(child,26))) reset(child,proxy);
    ResetVehicleState(owner);
}
NATIVEFN(0x1D1910,ResetVehiclePlacement);

void UpdateVehicle(LO* owner) {
    if (PFNUPDATE base=PfnUpdateOrig(0x1F36E0)) base(owner);
    if (VEHICLE* v=Pvehicle(owner)) { UpdateVehicleControl(*v); TraceVehicle(*v,"update"); }
}
} // namespace

VEHICLE* Pvehicle(LO* owner) {
    if (!owner || !owner->pcls || owner->pcls->PfnOrig(16)!=0x1D19A8) return nullptr;
    VEHICLE& v=s_vehicles[owner]; v.owner=static_cast<ALO*>(owner); return &v;
}
// Native replacement for the original world arena lifetime: unlink controllers
// while their owners' transform records still exist, then release wheel contacts.
void ResetVehicles() {
    for (auto& [owner,v]:s_vehicles) if (FBasicLive(owner)) {
        for (auto& side:v.sides) { CTRL* control=side.controller; DeleteCtrl(&control); side.controller=nullptr; }
        for (auto& attachment:v.attachments) { CTRL* control=attachment->meshBinding; DeleteCtrl(&control); attachment->meshBinding=nullptr; }
        for (auto& wheel:v.wheels) { FreeSwXpList(wheel.contacts); wheel.contacts=nullptr; }
    }
    s_vehicles.clear(); ResetVehicleBindings();
}

// orig0x1D1410/0x1D4DE0: each clone gets independent BASIC attachments.
void CloneVehicle(const LO* source,LO* destination) {
    VEHICLE* v=Pvehicle(destination); if (!v) return;
    auto it=s_vehicles.find(source); if (it==s_vehicles.end()) return;
    *v=it->second; v->owner=static_cast<ALO*>(destination); v->loaded=false;
    // Native handle replacement must not retract the source instance's SMA.
    v->binding=nullptr; v->path=nullptr;
    for (auto& a:v->attachments) { a=std::make_shared<VEHICLEATTACH>(*a); a->owner=v->owner; a->meshBinding=nullptr; a->binding=nullptr; a->mesh=nullptr; a->anchor=nullptr; a->machine=nullptr; }
    for (auto& side:v->sides) { side.node=nullptr; side.controller=nullptr; }
    for (auto& wheel:v->wheels) { wheel.node=nullptr; wheel.contacts=nullptr; }
}

// orig0x1D1518, after the common ALO and animation postloads.
void PostLoadVehicle(LO* owner) {
    VEHICLE* vehicle=Pvehicle(owner); if (!vehicle || vehicle->loaded) return;
    VEHICLE& v=*vehicle; ALO* p=v.owner;
    auto object=[&](const OPTVAL& value)->LO* { return value.pbasic?dynamic_cast<LO*>(value.pbasic):value.n==-1?nullptr:PloFindRelative(owner,value.n); };
    for (const OPT& option:owner->aopt) {
        if (option.aval.empty()) continue;
        const OPTVAL& value=option.aval.front();
        switch(option.eopid) {
        case 1423: v.target=object(value); break;
        case 1424: v.navigator=dynamic_cast<LOXMESH*>(object(value)); break;
        case 1426: v.maxHealth=value.n; break;
        case 1428: v.healthWidget=object(value); break;
        case 1429: v.invertHealth=value.n!=0; break;
        case 1430: v.machine=dynamic_cast<SM*>(object(value)); break;
        case 1434: v.soundId=value.n; break;
        }
    }
    glm::vec3 minimum{0},maximum{0};
    for (auto it=v.wheels.begin();it!=v.wheels.end();) {
        VEHICLEWHEEL& wheel=*it; wheel.node=FindNode(p,wheel.id,13);
        if (!wheel.node) { it=v.wheels.erase(it); continue; }
        wheel.position=PosConvertXfs(nullptr,PxfObj(p),PosOf(wheel.node->matWorld));
        minimum=glm::min(minimum,wheel.position); maximum=glm::max(maximum,wheel.position);
        wheel.upper=wheel.position+g_normalZ*wheel.top;
        wheel.lower=wheel.position+g_normalZ*wheel.bottom;
        ++it;
    }
    v.width=maximum.x-minimum.x;
    const float xScale=std::max(-minimum.x,maximum.x), yScale=std::max(-minimum.y,maximum.y);
    for (VEHICLEWHEEL& wheel:v.wheels) { wheel.steerFraction=wheel.position.x/xScale; wheel.sideFraction=wheel.position.y/yScale; }
    for (auto it=v.sides.begin();it!=v.sides.end();) {
        VEHICLESIDE& side=*it; side.node=FindNode(p,side.id,6);
        if (!side.node) { it=v.sides.erase(it); continue; }
        side.controller=PctrlNewPos(p,side.node,1,7,3,glm::vec4(0)); ++it;
    }
    for (auto it=v.attachments.begin();it!=v.attachments.end();) {
        VEHICLEATTACH& a=**it; a.owner=p;
        a.machine=dynamic_cast<SM*>(PloFindRelative(owner,a.objectId,1));
        a.mesh=FindNode(p,a.meshId,6);
        a.anchor=PloFindRelative(owner,a.anchorId,1);
        if (!a.machine || (!a.mesh && a.meshId!=-1) || (!a.anchor && a.anchorId!=-1)) { it=v.attachments.erase(it); continue; }
        a.binding=PsmaApplySm(a.machine,p,-1,0);
        if (a.mesh) a.meshBinding=PctrlNewPos(p,a.mesh,1,7,3,glm::vec4(0));
        ++it;
    }
    v.path=dynamic_cast<LOXPATH*>(PloNew(0x0a,g_psw,nullptr,0x2c9,-1));
    ReplaceSmaHandle(v.machine,p,-1,0,&v.binding);
    v.loaded=true; ResetVehicleState(owner);
    if (std::getenv("SLY2N_VEHICLETRACE")) std::printf("vehicle load owner %d instance %d wheels %zu sides %zu attachments %zu width %.9g target %d\n",owner->oid,owner->oidInstance,v.wheels.size(),v.sides.size(),v.attachments.size(),v.width,v.target?v.target->oid:-1);
}

// orig0x1D41F8: draw-time suspension heights and rolling/steering transforms.
void DrawVehiclePose(ALO* owner) {
    VEHICLE* v=Pvehicle(owner); if (!v || !v->loaded) return;
    const glm::vec3 velocity=VecConvertXfs(nullptr,PxfObj(owner),VelAloPos(owner));
    static const cmm::SMP heightCurve{1500,500,0.1f}, speedUp{20,5,0.2f}, speedDown{5,0,0.2f};
    for (size_t i=0;i<v->wheels.size();++i) {
        VEHICLEWHEEL& wheel=v->wheels[i];
        glm::vec3 point=PosConvertXfs(nullptr,PxfObj(owner),PosOf(wheel.node->matWorld));
        point.x=wheel.position.x; point.y=wheel.position.y;
        XP* hit=BestContact(wheel);
        const float height=wheel.lower.z+(hit?hit->gDist:0);
        if (!hit || point.z>=height) (void)cmm::GSmooth(point.z,height,g_clock.dt,&heightCurve,nullptr);
        point.z=height; // original discards the smooth return in this branch
        point=PosConvertXfs(PxfObj(owner),wheel.node->pxfRef,point);
        const float desired=i<2?0:v->drive/wheel.radius;
        if (hit) {
            const float current=velocity.x/wheel.radius;
            if ((desired>0 && current<0.25f*desired) || (desired<0 && 0.25f*desired<current))
                wheel.speed=cmm::GSmooth(wheel.speed,0.25f*desired,g_clock.dt,&speedUp,nullptr);
            else wheel.speed=current;
        } else wheel.speed=cmm::GSmooth(wheel.speed,desired,g_clock.dt,std::fabs(wheel.speed)<std::fabs(desired)?&speedUp:&speedDown,nullptr);
        wheel.angle=cmm::RadNormalize(wheel.angle+wheel.speed*g_clock.dt);
        const glm::mat3 steering=BuildRotateMatrixAxis(wheel.tilt,g_normalZ), rolling=BuildRotateMatrixAxis(wheel.angle,g_normalY);
        SetMat3(&wheel.node->mat,steering*rolling); SetPos(&wheel.node->mat,point);
        wheel.node->nStamp=++g_cXfStamp;
    }
    UpdateAloXfWorlds(owner);
    TraceVehicle(*v,"pose");
}

void SetVehicleFocus(LO* owner,bool on) {
    if (VEHICLE* v=Pvehicle(owner)) { if (on) SetVehicleMode(*v,2); else if (v->mode==2) SetVehicleMode(*v,0); }
}

namespace {
struct INITVEHICLE {
    INITVEHICLE() {
        RegisterLoUpdate(0x1D19A8,UpdateVehicle);
        using ARGS=const std::vector<CRef>&;
        RegisterObjectMethod(0x1104F0,[](LO* owner,ARGS args) {
            if (VEHICLE* v=Pvehicle(owner); v && v->wheels.size()<8) {
                VEHICLEWHEEL w; w.id=NArg(args,0); LM range=args[2].GetLm(); w.bottom=range.gMin; w.top=range.gMax; w.radius=GArg(args,1); v->wheels.push_back(w);
            }
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x1105B8,[](LO* owner,ARGS args) {
            if (VEHICLE* v=Pvehicle(owner); v && v->attachments.size()<8) {
                auto a=std::make_shared<VEHICLEATTACH>(); a->owner=v->owner; a->index=NArg(args,0); a->objectId=NArg(args,1); a->meshId=NArg(args,2); a->anchorId=NArg(args,3); v->attachments.push_back(a);
            }
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x110680,[](LO* owner,ARGS args) {
            if (VEHICLE* v=Pvehicle(owner); v && v->sides.size()<4) {
                VEHICLESIDE side; side.id=NArg(args,0); LM range=args[1].GetLm(); side.limits={range.gMin,range.gMax}; v->sides.push_back(side);
            }
            return CRef(TAGK_Void);
        });
        InitVehicleBindings();
    }
} s_initVehicle;
}
