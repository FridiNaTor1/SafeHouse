// The splice game built-ins the world side has (bif.h SetBifGameHook): the table 0x2BBB90's 97..168. The others
// stay counted stubs; 140..168 return Nil in the original too (0x1258F0..0x125FF0).

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

// an object's world matrix (vt slot 31): an ALO's first record, a LOX's reference x its matrix, else the identity
glm::mat4 MatWorld(const LO* plo)
{
    if (!plo) return glm::mat4(1.0f);
    if (FCidDerivedFrom(plo->Cid(), 0x30))
    {
        const ALO* palo = static_cast<const ALO*>(plo);
        return palo->axf.empty() ? glm::mat4(1.0f) : palo->axf[0]->matWorld;
    }
    if (FCidDerivedFrom(plo->Cid(), 3))
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

struct INITGAMEBIFS
{
    INITGAMEBIFS()
    {
        // 97 the world's index (0x210A88), 98 its kind (0x210A98), 99 its episode (0x210AE0), 104 0 (0x210B68)
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
        // 103 a float of 0x2DE578 (0x154C90; TODO(M4): that system)
        SetBifGameHook(Bifk3FromSly2(103), [](int, CRef*, CFrame*) { return RefF32(0.0f); });
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
    }
} s_initgamebifs;
}
