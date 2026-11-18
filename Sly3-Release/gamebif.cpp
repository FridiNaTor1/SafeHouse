// The splice game built-ins the world side has (bif.h SetBifGameHook): the table 0x2BBB90's 97..168. The others
// stay counted stubs; 140..168 return Nil in the original too (0x1258F0..0x125FF0).

#include "rumble.h"
#include "game/sly3.h"
#include "activate.h"
#include "bif.h"
#include "clock.h"
#include "cm.h"
#include "pair.h"
#include "savedata.h"
#include "spliceobj.h"
#include "sw.h"
#include "task.h"
#include "transition.h"
#include "update.h"
#include "jt.h"
#include "so.h"
#include "game/suck3.h"
#include "pickup.h"


#include <algorithm>

#include <glm/glm.hpp>

namespace
{
CRef RefS32(int n) { CRef r; r.SetS32(n); return r; }
CRef RefF32(float g) { CRef r; r.SetF32(g); return r; }
CRef RefVoid() { return CRef(TAGK_Void); }
float GOf(const CRef& r) { return r.m_tagk == TAGK_F32 ? r.m_g : static_cast<float>(r.m_n); }
LO* PloOf(const CRef& r) { return dynamic_cast<LO*>(r.m_pbasic); }

CRef RefList(const std::vector<CRef>& a)
{
    CRef ref;
    std::shared_ptr<CPair> ppairHead, ppairTail;
    for (const CRef& r : a)
    {
        auto p = PpairNew();
        p->m_ref = r;
        if (ppairTail) ppairTail->m_ppairNext = p;
        else ppairHead = p;
        ppairTail = p;
    }
    if (ppairHead) ref.SetPair(ppairHead);
    else ref.SetTag(TAGK_None);
    return ref;
}
const WREC* PwrecCur()
{
    const auto& a = AwrecWorlds();
    const int i = g_transition.iwrecCur;
    return i >= 0 && i < static_cast<int>(a.size()) ? &a[i] : nullptr;
}

// an object's world matrix (vt slot 31): an ALO's object record (+0x60), a LOX's reference x its matrix, else the identity
glm::mat4 MatWorld(const LO* plo)
{
    if (!plo) return glm::mat4(1.0f);
    if (FCidDerivedFrom(plo->Cid(), S3CID(0x30)))
    {
        const ALO* palo = static_cast<const ALO*>(plo);
        if (palo->apxfRef[2]) return palo->apxfRef[2]->matWorld;
        return palo->axf.empty() ? glm::mat4(1.0f) : palo->axf[0]->matWorld;
    }
    if (FCidDerivedFrom(plo->Cid(), S3CID(3)))
    {
        const LOX* plox = static_cast<const LOX*>(plo);
        return plox->pxf ? plox->pxf->matWorld * plox->mat : plox->mat;
    }
    return glm::mat4(1.0f);
}

// orig 0x0015E678 (kinds 1 / 5: by name, relative to an object / global; 4: under it) as a list (0x124078)
CRef RefFindObjects(int kind, const CRef* a)
{
    if (!g_splicegame.pfnFindObjects) return CRef();
    BASIC* ap[0x40];
    const int c = g_splicegame.pfnFindObjects(kind, a[0].m_symid, a[1].m_pbasic, 0x40, ap);
    std::vector<CRef> aref;
    for (int i = 0; i < c; ++i)
    {
        CRef r;
        r.SetBasic(ap[i]);
        aref.push_back(r);
    }
    return RefList(aref);
}

// orig 0x001252D0: a list of symbols / ints as a progress key
std::vector<int16_t> AkeyFromList(const CRef& ref)
{
    std::vector<int16_t> akey;
    for (const CPair* p = ref.m_ppair.get(); p; p = p->m_ppairNext.get())
        akey.push_back(static_cast<int16_t>(p->m_ref.m_tagk == TAGK_Symid ? static_cast<int>(p->m_ref.m_symid) : p->m_ref.m_n));
    return akey;
}

CRef RefHitTestObjects3(CRef* argv, bool fFirst)
{
    if (argv[0].m_tagk == TAGK_None) return argv[0];
    std::vector<LO*> aplo;
    for (const CPair* p = argv[0].m_ppair.get(); p; p = p->m_ppairNext.get()) aplo.push_back(PloOf(p->m_ref));
    std::vector<HITOBJ3> ahit;
    if (argv[1].m_tagk == TAGK_Vector && argv[2].m_tagk == TAGK_Vector)
        HitTestObjects3(aplo, argv[1].GetVector(), argv[2].GetVector(), &ahit);
    auto refObj = [](LO* plo) { CRef r; r.SetBasic(plo); return r; };
    auto refVec = [](const glm::vec3& v) { CRef r; r.SetVector(v); return r; };
    if (static const bool s_f = std::getenv("SLY3_HITTESTPRINT") != nullptr; s_f)   // (debug)
        for (const HITOBJ3& h : ahit)
            std::printf("hit-test-objects: %d of %zu listed, %d %p (cid 0x%X, in world %d) at %.0f %.0f %.0f\n", int(ahit.size()), aplo.size(), h.plo->oid, (void*)h.plo, h.plo->Cid(), int(h.plo->fInWorld), h.pos.x, h.pos.y, h.pos.z);
    std::vector<CRef> aref;
    if (fFirst)
    {
        const HITOBJ3* pbest = nullptr;
        for (const HITOBJ3& h : ahit)
            if (!pbest || h.t < pbest->t) pbest = &h;
        if (!pbest) { CRef r; r.SetTag(TAGK_None); return r; }
        return RefList({refObj(pbest->plo), refVec(pbest->pos)});
    }
    for (const HITOBJ3& h : ahit) aref.push_back(RefList({refObj(h.plo), refVec(h.pos), refVec(glm::vec3(h.plane))}));
    return RefList(aref);
}

struct INITGAMEBIFS
{
    INITGAMEBIFS()
    {
        // Sly 3's 148 add-coins (orig3 0x1276B8): the save's coins (0x1852E0) plus the argument through 0x185328 (clamped
        // to 0..999999, the coin counter shown: 0x1853D8); returns void (tag 0x22) (audit 4 CORE-09)
        SetBifGameHook(static_cast<BIFK>(148), [](int argc, CRef* argv, CFrame*) {
            const int d = argc > 0 ? (argv[0].m_tagk == TAGK_F32 ? int(argv[0].m_g) : argv[0].m_n) : 0;
            const CRef rc = RefGetProgress({"loot", "ccoin"});
            const int c = rc.m_tagk == TAGK_S32 ? rc.m_n : rc.m_tagk == TAGK_F32 ? int(rc.m_g) : 0;
            CRef r;
            r.SetS32(std::clamp(int(unsigned(c) + unsigned(d)), 0, 0xF423F));
            FSetProgress({"loot", "ccoin"}, r);
            ShowCoinCounter();
            return CRef(TAGK_Void);
        });
        // 97 the world's index (0x210A88), 98 its kind (0x210A98), 99 its episode (0x210AE0), 104 0 (0x210B68)
        // (audit 4 CORE-20: Sly 2's 98 / 99 have no Sly 3 built-in (Bifk3FromSly2: BIFK_Nil, the hooks are dropped), and
        // Sly 2's 104 / 105 / 106 / 133 land on Sly 3's 108 / 111 / 112 / 141, which bif.cpp's table has as native BIFN
        // entries (RefOpZero3, RefOpScheduleCallback3, RefOpCancelCallback3, RefOpSymbolValue3): a BIFN wins over a hook,
        // so those four hooks below are never called; kept as the Sly 2 reference of the same behaviour)
        SetBifGameHook(Bifk3FromSly2(97), [](int, CRef*, CFrame*) { return RefS32(g_transition.iwrecCur); });
        SetBifGameHook(Bifk3FromSly2(98), [](int, CRef*, CFrame*) { const WREC* p = PwrecCur(); return RefS32(p ? p->kind : -1); });
        SetBifGameHook(Bifk3FromSly2(99), [](int, CRef*, CFrame*) { return RefS32(IepisodeCur()); });
        SetBifGameHook(Bifk3FromSly2(104), [](int, CRef*, CFrame*) { return RefS32(0); });
        // 105 schedule-callback (dt proc . args): the call (proc 'arg ...) at t + dt (0x133040); its id
        SetBifGameHook(Bifk3FromSly2(105), [](int argc, CRef* argv, CFrame*) {
            std::vector<CRef> aref;
            if (argc >= 3)
                for (const CPair* p = argv[2].m_ppair.get(); p; p = p->m_ppairNext.get()) aref.push_back(p->m_ref);
            return RefS32(IdScheduleCallback(GOf(argv[0]), argv[1], std::move(aref)));
        });
        // 106 its cancel (0x133130)
        SetBifGameHook(Bifk3FromSly2(106), [](int, CRef* argv, CFrame*) {
            CancelScheduledCallback(argv[0].m_n);
            return RefVoid();
        });
        // 107 defer-object-update (object dt): its update event waits until t + dt (+0x30)
        SetBifGameHook(Bifk3FromSly2(107), [](int, CRef* argv, CFrame*) {
            if (LO* plo = PloOf(argv[0])) plo->tNextUpdate = g_clock.t + GOf(argv[1]);
            return RefVoid();
        });
        // 113 the objects of a name (relative 1 / global 5: 0x1241E8), 116 those under an object (4: 0x124578)
        SetBifGameHook(Bifk3FromSly2(113), [](int, CRef* argv, CFrame*) { return RefFindObjects(argv[1].m_pbasic ? 0x401 : 0x405, argv); });
        SetBifGameHook(Bifk3FromSly2(116), [](int, CRef* argv, CFrame*) { return RefFindObjects(0x404, argv); });
        // 125 / 126 / 127: a position / vector / matrix from one object's space to another's (0x1A6F00 / 0x1A6FC0 /
        // 0x1A71B0; null: the world)
        // orig: 0x00125000 / 0x00125060 / 0x001250D8 (convert-object-position / -vector / -matrix: 0x1A6F00 /
        // 0x1A6FC0 / 0x1A71B0, through each object's vt slot 31 record (an ALO's object record, not its root): from
        // argv[0]'s frame to argv[1]'s, nil the world). The RC chopper's pull back into its area (world 8's trigger
        // 77417 sets its option 1526) converts the pull with 126 into the chopper's own (heading) frame.
        SetBifGameHook(Bifk3FromSly2(125), [](int, CRef* argv, CFrame*) {
            const glm::vec4 v = glm::inverse(MatJtLo(PloOf(argv[1]))) * (MatJtLo(PloOf(argv[0])) * glm::vec4(argv[2].GetVector(), 1.0f));
            CRef r;
            r.SetVector(glm::vec3(v));
            return r;
        });
        SetBifGameHook(Bifk3FromSly2(126), [](int, CRef* argv, CFrame*) {
            glm::vec3 v = argv[2].GetVector();
            if (LO* pa = PloOf(argv[0])) v = glm::mat3(MatJtLo(pa)) * v;
            if (LO* pb = PloOf(argv[1])) v = glm::inverse(glm::mat3(MatJtLo(pb))) * v;  // (0x11B340)
            CRef r;
            r.SetVector(v);
            return r;
        });
        SetBifGameHook(Bifk3FromSly2(127), [](int, CRef* argv, CFrame*) {
            CRef r;
            r.SetMatrix(glm::inverse(MatJtLo(PloOf(argv[1]))) * MatJtLo(PloOf(argv[0])) *
                        (argv[2].m_tagk == TAGK_Matrix ? argv[2].GetMatrix() : glm::mat4(1.0f)));
            return r;
        });
        // 128 near-clip-center: the camera's position + its view direction x its near distance (0x125168)
        SetBifGameHook(Bifk3FromSly2(128), [](int, CRef*, CFrame*) {
            CRef r;
            r.SetVector(g_cm.pos + g_cm.mat[0] * g_cm.sNearClip);
            return r;
        });
        // 131 / 132: a progress value by a path of symbols (0x125398 / 0x125450)
        // Sly 2 130 (0x125248): the pad's rumble (kind, seconds: 0x187E28), void. Sly 3's 138 (0x127158) takes an
        // optional third argument, the player (the rest list's first: 0x36E740 + n x 0x11C0, 0x1A9058); -1 none. The PC
        // drives player 0's pad (player 1's in the split screen has no rumble of its own here)
        SetBifGameHook(Bifk3FromSly2(130), [](int argc, CRef* argv, CFrame*) {
            int channel = 0;
            if (argc > 2 && argv[2].m_tagk == TAGK_Pair && argv[2].m_ppair) channel = argv[2].m_ppair->m_ref.m_n;
            if (channel == 0) TriggerJoyRumbleRumk(argv[0].m_n, argv[1].GAsFloat(), "bif 130");
            return RefVoid();
        });
        SetBifGameHook(Bifk3FromSly2(131), [](int, CRef* argv, CFrame*) { return RefGetProgressKey(AkeyFromList(argv[0])); });
        SetBifGameHook(Bifk3FromSly2(132), [](int, CRef* argv, CFrame*) {
            FSetProgressKey(AkeyFromList(argv[0]), argv[1]);
            return RefVoid();
        });
        // orig: 0x00125510 (bif 133): a symbol's binding in the parent of the calling code's module frame (the frame
        // chain walked up single parents to the first module frame, 0x1FFA70's +0x24): e.g. an instance's start
        // calling its group's start (world 8's turret 106717: (((bif133 's0) self) ...)); else the void bif (134)
        SetBifGameHook(Bifk3FromSly2(133), [](int, CRef* argv, CFrame* pframe) {
            CFrame* pf = pframe;
            if (pf && pf->m_cpframeParent == 1)
            {
                while (pf && !pf->m_fModule)
                {
                    pf = pf->m_apframeParent[0];
                    if (!pf || pf->m_cpframeParent != 1) pf = nullptr;
                }
                if (pf && pf->m_apframeParent[0])
                    if (CRef* pref = pf->m_apframeParent[0]->PrefFindBinding(argv[0].m_symid, 1)) return *pref;
            }
            CRef r;
            r.SetBifk(Bifk3FromSly2(134));
            return r;
        });
        // 134 void; 135 / 136: the AND / OR of a list of ints (0x1255F0 / 0x125640)
        SetBifGameHook(Bifk3FromSly2(134), [](int, CRef*, CFrame*) { return RefVoid(); });
        SetBifGameHook(Bifk3FromSly2(135), [](int argc, CRef* argv, CFrame*) {
            int n = -1;
            if (argc > 0)
                for (const CPair* p = argv[0].m_ppair.get(); p; p = p->m_ppairNext.get()) n &= p->m_ref.m_n;
            return RefS32(n);
        });
        SetBifGameHook(Bifk3FromSly2(136), [](int argc, CRef* argv, CFrame*) {
            int n = 0;
            if (argc > 0)
                for (const CPair* p = argv[0].m_ppair.get(); p; p = p->m_ppairNext.get()) n |= p->m_ref.m_n;
            return RefS32(n);
        });
        // 103 (Sly 3's 107, 0x171C98): the dynamic difficulty, game/suck3.cpp
        SetBifGameHook(Bifk3FromSly2(103), [](int, CRef*, CFrame*) {
            const float g = GSuck3();
            if (std::getenv("SLY3_SUCKPRINT")) std::printf("suck3: read %.2f\n", g);
            return RefF32(g);
        });
        // 137 (0x125690): the HUD prompt widget 0x3DA0C0 (spliceglue.cpp's PbasicGlobal)
        SetBifGameHook(Bifk3FromSly2(137), [](int, CRef*, CFrame*) {
            CRef r;
            if (g_splicegame.pfnPbasicGlobal) r.SetBasic(g_splicegame.pfnPbasicGlobal(137));
            return r;
        });
        // 138 (thunk 0x1256E0 -> 0x1DAEA8): ThiefNet item n acquired ("powerUps mppukfAcquired <n>"; -1: false); e.g.
        // Ep5's t5_paraglide_purchase polls (bif-138 27) every 2 s and closes itself once the Paraglide is bought
        // (stubbed to #f before: the Ep5 heist never became available)
        SetBifGameHook(Bifk3FromSly2(138), [](int argc, CRef* argv, CFrame*) {
            CRef r;
            const int n = argc > 0 ? argv[0].m_n : -1;
            bool f = false;
            if (n != -1)
            {
                const CRef v = RefGetProgress({"powerUps", "mppukfAcquired", std::to_string(n)});
                f = (v.m_tagk == TAGK_Bool && v.m_bool) || (v.m_tagk == TAGK_S32 && v.m_n != 0);
            }
            r.SetBool(f);
            return r;
        });
        // 139 (thunk 0x125738 -> 0x1DAF38): item a's "acquired" set to b; when set, the task manager's +0xC = a and
        // the tasks' event 0x3B (as the shop's purchase)
        SetBifGameHook(Bifk3FromSly2(139), [](int argc, CRef* argv, CFrame*) {
            const int n = argc > 0 ? argv[0].m_n : -1;
            const bool f = argc > 1 && ((argv[1].m_tagk == TAGK_Bool && argv[1].m_bool) || (argv[1].m_tagk == TAGK_S32 && argv[1].m_n != 0));
            CRef v;
            v.SetBool(f);
            FSetProgress({"powerUps", "mppukfAcquired", std::to_string(n)}, v);
            if (f)
            {
                g_taskmgr.nItemC = n;
                BroadcastTaskEvent(g_psw, 0x3B, nullptr);
            }
            return RefVoid();
        });
        // Sly 3's 129 find-objects-in-bounding-box (pos, pos) (orig3 0x126808 -> 0x25CF18 / 0x25D240): the world's
        // attachments whose box (+0x00 / +0x10, SW +0x44AC's list, refreshed first: 0x13C780 / 0x13C7D0) meets the box
        // of the two corners (min / max of each), at most 0x100, their entries' objects once each, as a list
        SetBifGameHook(static_cast<BIFK>(129), [](int, CRef* argv, CFrame*) {
            std::vector<CRef> aref;
            if (argv[0].m_tagk != TAGK_Vector || argv[1].m_tagk != TAGK_Vector) return RefList(aref);
            const glm::vec3 a = argv[0].GetVector(), b = argv[1].GetVector();
            const glm::vec3 mn = glm::min(a, b), mx = glm::max(a, b);
            std::vector<ALO*> apalo;
            int c = 0;
            FlushSwOxaDirty1();
            UpdateSwAaox();
            for (OXA* poxa : g_physw.apoxa)
            {
                if (!poxa || !poxa->pso || !poxa->pso->fInWorld) continue;
                if (poxa->posMin.x > mx.x || poxa->posMax.x < mn.x || poxa->posMin.y > mx.y || poxa->posMax.y < mn.y ||
                    poxa->posMin.z > mx.z || poxa->posMax.z < mn.z)
                    continue;
                for (SOE* psoe : poxa->apsoe)
                {
                    if (!psoe || !psoe->palo || c >= 0x100) continue;
                    c++;
                    if (std::find(apalo.begin(), apalo.end(), psoe->palo) == apalo.end()) apalo.push_back(psoe->palo);
                }
            }
            for (ALO* palo : apalo)
            {
                CRef ref;
                ref.SetBasic(palo);
                aref.push_back(ref);
            }
            return RefList(aref);
        });
        // Sly 3's 130 find-objects-in-bounding-sphere (pos, radius) (orig3 0x1268E0): the world's attachments whose
        // sphere meets it (0x25D0D8: SW +0x44AC, not each entry), their entries' objects once each (0x25D240), as a
        // list; e.g. h_dogfight's respawn (3146's 2430) skips a source with a plane within 750
        SetBifGameHook(static_cast<BIFK>(130), [](int, CRef* argv, CFrame*) {
            std::vector<CRef> aref;
            if (argv[0].m_tagk != TAGK_Vector) return RefList(aref);
            const glm::vec3 pos = argv[0].GetVector();
            const float r = GOf(argv[1]);
            std::vector<ALO*> apalo;
            int c = 0;
            FlushSwOxaDirty1();   // (0x13C780 / 0x13C7D0 first: the moved attachments' spheres, e.g. a plane just respawned)
            UpdateSwAaox();
            for (OXA* poxa : g_physw.apoxa)
            {
                if (!poxa || !poxa->pso || !poxa->pso->fInWorld) continue;
                const glm::vec3 d = pos - poxa->posCenter;
                const float s = r + poxa->sRadius;
                if (glm::dot(d, d) > s * s) continue;
                for (SOE* psoe : poxa->apsoe)
                {
                    if (!psoe || !psoe->palo || c >= 0x100) continue;
                    c++;
                    if (std::find(apalo.begin(), apalo.end(), psoe->palo) == apalo.end()) apalo.push_back(psoe->palo);
                }
            }
            for (ALO* palo : apalo)
            {
                CRef ref;
                ref.SetBasic(palo);
                aref.push_back(ref);
            }
            return RefList(aref);
        });
        // Sly 3's 131 hit-test-objects (objects, pos, pos) / 132 hit-test-objects-first (orig3 0x126E08 / 0x126E88 ->
        // 0x1269D8 with 0x83 / 0x84; so.h HitTestObjects3): 131 a list of (object point normal) for each collision entry of
        // the listed objects the segment crosses, 132 (object point) of the nearest; nil (the list itself when it is nil)
        // for none. E.g. the two-player hack's laser (the ship's s1: every enemy of its list on the beam gets 130 each frame)
        SetBifGameHook(static_cast<BIFK>(131), [](int, CRef* argv, CFrame*) { return RefHitTestObjects3(argv, false); });
        SetBifGameHook(static_cast<BIFK>(132), [](int, CRef* argv, CFrame*) { return RefHitTestObjects3(argv, true); });
    }
} s_initgamebifs;
}
