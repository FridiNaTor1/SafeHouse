// Class0x99 attachment properties and class0x6C ammunition pickups.
#include "vehicle.h"
#include "game/sly3.h"
#include "worldglobals.h"
#include "animfind.h"
#include "clock.h"
#include "lo.h"
#include "objmethod.h"
#include "sm.h"
#include "smrun.h"
#include "so.h"
#include "spliceglue.h"
#include "update.h"
#include "vtnative.h"
#include "worldaccess.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

namespace {
struct PICKUP {
    int state=-1; float time=0; int kind=0,amount=30; // +0x150..+0x15C
    SM* machine=nullptr; SMA* binding=nullptr;       // +0x160/+0x164
    bool loaded=false;
};
std::unordered_map<LO*,PICKUP> s_pickups;
WORLD_TABLE(s_pickups);   // (PC: moved aside while a kept world loads, worldglobals.h)
PICKUP* Pickup(LO* p) { return p && p->Cid()==S3CID(0x6c)?&s_pickups[p]:nullptr; }

// orig0x1D4FA0: state/time followed by the authored state-machine seek.
void SetPickupState(LO* owner,int state) {
    PICKUP* p=Pickup(owner); if (!p || state==p->state) return;
    p->state=state; p->time=g_clock.t;
    static const int states[]={119,120,121}; // D_002C2248
    if (p->binding && state>=0 && state<3) SeekSma(p->binding,states[state]);
}
// orig0x1D4F50: switch to ready when the position record rests.
void UpdatePickup(LO* owner) {
    if (PFNUPDATE base=PfnUpdateOrig(0x1F36E0)) base(owner);
    PICKUP* p=Pickup(owner); ALO* alo=static_cast<ALO*>(owner);
    if (p && p->state==0 && PxfPos(alo) && PxfPos(alo)->mode==3) SetPickupState(owner,1);
}
// orig0x1D4F10 (vt slot 26, a pool row taken again: after the base 0x1CB518 that AttachProxy runs): the state to -1,
// then 0, so a reused ammo token is live again (natively it kept state 2 "collected": no SM animation and the
// vehicles' contacts rejected it; N30 / N10's RC Combat Club tokens)
void ResetPickupSlot26(LO* owner,LO*) {
    if (std::getenv("SLY2N_VEHICLETRACE")) { PICKUP* p=Pickup(owner); std::printf("vehicle pickup %d reused (state %d) t %.3f\n",owner->oid,p?p->state:-9,g_clock.t); }
    SetPickupState(owner,-1); SetPickupState(owner,0);
}
NATIVEFN(0x1D4F10,ResetPickupSlot26);
void UseResource(VEHICLEATTACH* attachment) {
    if (!FBasicLive(attachment->owner)) return;
    VEHICLE* vehicle=Pvehicle(attachment->owner);
    if (!vehicle || attachment->resource<0 || attachment->resource>=int(vehicle->resources.size())) return;
    auto& resource=vehicle->resources[attachment->resource];
    resource.count=std::max(0,resource.count-attachment->cost); // orig0x1D4E30
}
CRef RefUseResource(BASIC* basic,int,CRef*) { UseResource(static_cast<VEHICLEATTACH*>(basic)); return CRef(TAGK_Void); }
CRef Int(int n) { CRef r; r.SetS32(n); return r; }
CRef Float(float n) { CRef r; r.SetF32(n); return r; }
CRef Object(BASIC* object) { CRef r; r.SetBasic(FBasicLive(object)?object:nullptr); return r; }
LO* Lo(const CRef& value) { return FBasicLive(value.m_pbasic)?dynamic_cast<LO*>(value.m_pbasic):nullptr; }
cmm::LM Limits(const CRef& value) { LM limits=value.GetLm(); return {limits.gMin,limits.gMax}; }
CRef LimitRef(const cmm::LM& limits) { CRef r; r.SetLm({limits.gMin,limits.gMax}); return r; }
}

bool FVehicleAttachmentReady(const VEHICLEATTACH& a) {
    VEHICLE* v=FBasicLive(a.owner)?Pvehicle(a.owner):nullptr;
    return v && a.resource>=0 && a.resource<int(v->resources.size()) && v->resources[a.resource].count>=a.cost;
}

// Class0x99 EOPIDs1197..1214 plus253, exact types/readonly flags in the option table.
CRef RefVehicleAttachmentOption(VEHICLEATTACH* a,int bif,int option,const CRef* value) {
    CRef result;
    if (bif==111) {
        switch(option) {
        case 253: return Float(a->speed);
        case 1197: return Int(int(a->heldButtons));
        case 1198: return Int(int(a->pressedButtons));
        case 1199: return LimitRef(a->angleLimits);
        case 1200: result.SetClq({a->flightTime.x,a->flightTime.y,a->flightTime.z,0}); return result;
        case 1201: return LimitRef(a->flightLimits);
        case 1202: return LimitRef(a->speedLimits);
        case 1203: return Float(a->gravity);
        case 1204: return Float(a->angle);
        case 1205: return Int(a->tactic);
        case 1206: return Float(a->radius);
        case 1207: return Object(a->constraint);
        case 1208: return LimitRef(a->enterRange);
        case 1209: return LimitRef(a->exitRange);
        case 1210: return LimitRef(a->fireInterval);
        case 1211: result.SetBool(a->active); return result;
        case 1212: return Int(a->resource);
        case 1213: return Int(a->cost);
        case 1214: {
            auto method=std::make_shared<CMethod>(); method->m_pbasic=a;
            method->m_crefReq=0; method->m_pfnthunk=RefUseResource; result.SetMethod(method); return result;
        }
        default: return result;
        }
    }
    if (bif!=110 || !value) return result;
    switch(option) {
    case 1197: a->heldButtons=uint32_t(value->RefCoerceS32().m_n); break;
    case 1198: a->pressedButtons=uint32_t(value->RefCoerceS32().m_n); break;
    case 1199: a->angleLimits=Limits(*value); break;
    case 1200: { CLQ polynomial=value->GetClq(); a->flightTime={polynomial.g0,polynomial.g1,polynomial.g2}; break; }
    case 1201: a->flightLimits=Limits(*value); break;
    case 1202: a->speedLimits=Limits(*value); break;
    case 1203: a->gravity=value->RefCoerceF32().m_g; break;
    case 1205: a->tactic=value->RefCoerceS32().m_n; break;
    case 1206: a->radius=value->RefCoerceF32().m_g; break;
    case 1207: a->constraint=Lo(*value); break;
    case 1208: a->enterRange=Limits(*value); break;
    case 1209: a->exitRange=Limits(*value); break;
    case 1210: a->fireInterval=Limits(*value); break;
    case 1212: a->resource=value->RefCoerceS32().m_n; break;
    case 1213: a->cost=value->RefCoerceS32().m_n; break;
    default: return result;
    }
    return CRef(TAGK_Void);
}

void PostLoadVehiclePickup(LO* owner) {
    PICKUP* pickup=Pickup(owner); if (!pickup || pickup->loaded) return;
    for (const OPT& option:owner->aopt) {
        if (option.aval.empty()) continue;
        const OPTVAL& value=option.aval.front();
        switch(option.eopid) {
        case 1435: pickup->kind=value.n; break;
        case 1436: pickup->amount=value.n; break;
        case 1437: pickup->machine=dynamic_cast<SM*>(value.pbasic?value.pbasic:PloFindRelative(owner,value.n)); break;
        }
    }
    ReplaceSmaHandle(pickup->machine,static_cast<ALO*>(owner),-1,0,&pickup->binding);
    pickup->loaded=true; SetPickupState(owner,0);
}
int VehiclePickupKind(LO* owner) { PICKUP* p=Pickup(owner); return p?p->kind:-1; }
int VehiclePickupState(LO* owner) { PICKUP* p=Pickup(owner); return p?p->state:-1; }

// orig0x1D4050 ->0x149AB8 ->0x1D3FF0. The native deferred FIFO's integer
// argument cannot store a 64-bit pointer; keep a per-vehicle ticket table.
namespace {
struct COLLECTION { LO* owner=nullptr; LO* pickup=nullptr; };
std::vector<COLLECTION> s_collection;
WORLD_TABLE(s_collection);   // (PC: moved aside while a kept world loads, worldglobals.h)
void Collect(void* object,int ticket) {
    if (ticket<0 || ticket>=int(s_collection.size())) return;
    const COLLECTION receipt=s_collection[ticket]; s_collection[ticket]={};
    if (object!=receipt.owner || !FBasicLive(receipt.owner) || !FBasicLive(receipt.pickup)) return;
    VEHICLE* vehicle=Pvehicle(receipt.owner); PICKUP* pickup=Pickup(receipt.pickup);
    if (std::getenv("SLY2N_VEHICLETRACE"))
        std::printf("vehicle pickup %d contact by %d (state %d) t %.3f\n",receipt.pickup->oid,receipt.owner->oid,pickup?pickup->state:-9,g_clock.t);
    if (!vehicle || !pickup || pickup->state==2 || pickup->kind<0 || pickup->kind>=int(vehicle->resources.size())) return;
    auto& resource=vehicle->resources[pickup->kind];
    resource.count=std::min(resource.maximum,resource.count+pickup->amount);
    SetPickupState(receipt.pickup,2);
}
}
void VehicleContactMaterial(ALO* owner,XP* contact,int side) {
    ALO* other=contact->axpd[1-side].paloRoot;
    const glm::vec3 normal=side?-contact->normal:contact->normal;
    contact->mu=(other && FCidDerivedFrom(other->Cid(),S3CID(0x4d))) || normal.z<0.7f?0:0.5f;
    contact->elas=0.1f;
    if (other && FCidDerivedFrom(other->Cid(),S3CID(0x6c))) {
        // Original suppression keys on (receiver,method,argument), so reuse
        // the same live ticket for repeated contacts with this pickup.
        int ticket=0;
        for (;ticket<int(s_collection.size());++ticket)
            if (s_collection[ticket].owner==owner && s_collection[ticket].pickup==other) break;
        if (ticket==int(s_collection.size())) {
            ticket=0;
            for (;ticket<int(s_collection.size()) && s_collection[ticket].owner;++ticket) {}
            if (ticket==int(s_collection.size())) s_collection.push_back({});
            s_collection[ticket]={owner,other};
        }
        PostDeferredCall(owner,0x1D3FF0,Collect,ticket);
    }
}
NATIVEFN(0x1D4050,VehicleContactMaterial);
void ResetVehicleBindings() { s_pickups.clear(); s_collection.clear(); }

void InitVehicleBindings() {
    RegisterLoUpdate(0x1D4F50,UpdatePickup);
    using ARGS=const std::vector<CRef>&;
    RegisterObjectMethod(0x110738,[](LO* owner,ARGS args) {
        CRef result; result.SetBasic(nullptr);
        if (VEHICLE* v=Pvehicle(owner)) for (auto& a:v->attachments)
            if (a->index==NArg(args,0)) { result.SetOwnedBasic(a); break; }
        return result;
    });
    RegisterObjectMethod(0x1107B8,[](LO* owner,ARGS args) {
        VEHICLE* v=Pvehicle(owner); int index=NArg(args,0);
        return Int(v && index>=0 && index<int(v->resources.size())?v->resources[index].count:0);
    });
    RegisterObjectMethod(0x110838,[](LO* owner,ARGS args) {
        VEHICLE* v=Pvehicle(owner); int index=NArg(args,0);
        if (v && index>=0 && index<int(v->resources.size())) v->resources[index].count=NArg(args,1);
        return CRef(TAGK_Void);
    });
    RegisterObjectMethod(0x1108E0,[](LO* owner,ARGS args) {
        VEHICLE* v=Pvehicle(owner); int index=NArg(args,0);
        if (v && index>=0 && index<int(v->resources.size())) { v->resources[index].maximum=NArg(args,1); v->resources[index].priority=GArg(args,2); }
        return CRef(TAGK_Void);
    });
    RegisterObjectGetter(1423,[](LO* p) { VEHICLE* v=Pvehicle(p); return Object(v?v->target:nullptr); });
    RegisterObjectGetter(1424,[](LO* p) { VEHICLE* v=Pvehicle(p); return Object(v?v->navigator:nullptr); });
    RegisterObjectGetter(1425,[](LO* p) { VEHICLE* v=Pvehicle(p); return Int(v?v->mode:0); });
    RegisterObjectGetter(1426,[](LO* p) { VEHICLE* v=Pvehicle(p); return Int(v?v->maxHealth:0); });
    RegisterObjectGetter(1427,[](LO* p) { VEHICLE* v=Pvehicle(p); return Int(v?v->health:0); });
    RegisterObjectGetter(1428,[](LO* p) { VEHICLE* v=Pvehicle(p); return Object(v?v->healthWidget:nullptr); });
    RegisterObjectGetter(1429,[](LO* p) { VEHICLE* v=Pvehicle(p); CRef r; r.SetBool(v&&v->invertHealth); return r; });
    RegisterObjectGetter(1430,[](LO* p) { VEHICLE* v=Pvehicle(p); return Object(v?v->machine:nullptr); });
    RegisterObjectGetter(1434,[](LO* p) { VEHICLE* v=Pvehicle(p); return Int(v?v->soundId:-1); });
    RegisterObjectSetter(1423,[](LO* p,const CRef& r) { if (VEHICLE* v=Pvehicle(p)) v->target=Lo(r); });
    RegisterObjectSetter(1424,[](LO* p,const CRef& r) { if (VEHICLE* v=Pvehicle(p)) v->navigator=dynamic_cast<LOXMESH*>(Lo(r)); });
    RegisterObjectSetter(1425,[](LO* p,const CRef& r) { if (VEHICLE* v=Pvehicle(p)) SetVehicleMode(*v,r.RefCoerceS32().m_n); });
    RegisterObjectSetter(1428,[](LO* p,const CRef& r) { if (VEHICLE* v=Pvehicle(p)) v->healthWidget=Lo(r); });
    RegisterObjectSetter(1429,[](LO* p,const CRef& r) { if (VEHICLE* v=Pvehicle(p)) v->invertHealth=r.m_tagk==TAGK_Bool?r.m_bool!=0:r.RefCoerceS32().m_n!=0; });
    RegisterObjectSetter(1430,[](LO* p,const CRef& r) { if (VEHICLE* v=Pvehicle(p)) v->machine=dynamic_cast<SM*>(Lo(r)); });
    RegisterObjectSetter(1434,[](LO* p,const CRef& r) { if (VEHICLE* v=Pvehicle(p)) v->soundId=r.RefCoerceS32().m_n; });
    RegisterObjectGetter(1435,[](LO* p) { PICKUP* v=Pickup(p); return Int(v?v->kind:0); });
    RegisterObjectGetter(1436,[](LO* p) { PICKUP* v=Pickup(p); return Int(v?v->amount:0); });
    RegisterObjectGetter(1437,[](LO* p) { PICKUP* v=Pickup(p); return Object(v?v->machine:nullptr); });
    RegisterObjectSetter(1435,[](LO* p,const CRef& r) { if (PICKUP* v=Pickup(p)) v->kind=r.RefCoerceS32().m_n; });
    RegisterObjectSetter(1436,[](LO* p,const CRef& r) { if (PICKUP* v=Pickup(p)) v->amount=r.RefCoerceS32().m_n; });
    RegisterObjectSetter(1437,[](LO* p,const CRef& r) { if (PICKUP* v=Pickup(p)) v->machine=dynamic_cast<SM*>(Lo(r)); });
}
