// The sound module in the running game: the per-frame update in the world update's pass (0x226550 calls
// 0x1FD830 there), the hooks it asks the object model through (sound.h SNDHOOKS), the gait's footsteps.

#include "actanim.h"
#include "game/sly3.h"
#include "worldglobals.h"
#include "bif.h"
#include "actor.h"
#include "gang.h"
#include "sly.h"
#include "brx.h"
#include "clock.h"
#include "cm.h"
#include "lo.h"
#include "objmethod.h"
#include "po.h"
#include "so.h"
#include "splayer.h"
#include "snd989.h"
#include "sound.h"
#include "ai.h"
#include "sw.h"
#include "screenrt.h"
#include "sm.h"
#include "animfind.h"
#include "shd.h"
#include "glob.h"
#include "smrun.h"
#include "worldaccess.h"
#include "update.h"
#include "xf.h"

#include <glm/glm.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include <vector>

int NRandInRange(int nLow, int nHigh);  // util.h (0x11D3F8)

namespace
{
// The three characters (Sly 0x35 at 0x2E1E40, the others at 0x2DD5BC / 0x2F7900), found once per world.
SW* s_pswChars = nullptr;
std::array<void*, 3> s_apchar{};

int s_cframeChars = 0;
int s_cResetChars = -1;   // (g_sndhooks.cReset when they were found)

void FindCharacters(SW* psw)
{
    // (again on a new world, and every second: a new SW may sit at the old one's address)
    if (psw == s_pswChars && s_cResetChars == g_sndhooks.cReset && ++s_cframeChars < 60) return;
    s_cframeChars = 0;
    s_cResetChars = g_sndhooks.cReset;
    s_pswChars = psw;
    s_apchar = {};
    if (!psw) return;
    ALO* pSly = nullptr;
    ALO* apOther[2] = {nullptr, nullptr};
    int cOther = 0;
    for (const auto& plo : psw->aploAll)
    {
        if (!plo || !FIsLoDerivedFrom(plo.get(), S3CID(0x34))) continue;
        ALO* palo = static_cast<ALO*>(plo.get());
        if (FIsLoDerivedFrom(palo, S3CID(0x35))) { if (!pSly) pSly = palo; }
        else if ((FIsLoDerivedFrom(palo, S3CID(0x36)) || FIsLoDerivedFrom(palo, S3CID(0x37))) && cOther < 2) apOther[cOther++] = palo;
    }
    // 0x1FE150: voice 3 follows 0x2DD5BC and 0x2F7900 (Murray, Bentley), voice 4 0x2E1E40 (Sly)
    s_apchar = {apOther[0], apOther[1], pSly};
}

// orig 0x1FB778: the forced/current player's object centre, otherwise CM +0x90;
// the camera rows still supply the pan direction (x forward, y left, z up).
bool FListener(glm::vec3* ppos, glm::mat3* pmat)
{
    LO* player = g_psw ? PloPoCurrent(g_psw) : nullptr;
    *ppos = player ? PosAloCenter(static_cast<ALO*>(player)) : g_cm.pos;
    *pmat = glm::transpose(g_cm.mat);  // (mat * d = (forward . d, left . d, up . d))
    return true;
}

bool FSourceAlive(void* psrc) { return static_cast<LO*>(psrc)->fInWorld; }

// orig 0x1A6F00(src, 0, pos): the local point in the world
glm::vec3 PosSourceWorld(void* psrc, const glm::vec3& posLocal)
{
    // a located object (LOX, cid 3 family: the ambient objects): its transform record's world matrix x its own
    if (const LOX* px = dynamic_cast<const LOX*>(static_cast<LO*>(psrc)))
        return glm::vec3((px->pxf ? px->pxf->matWorld : glm::mat4(1.0f)) * px->mat * glm::vec4(posLocal, 1.0f));
    ALO* palo = dynamic_cast<ALO*>(static_cast<LO*>(psrc));
    if (!palo) return posLocal;
    glm::vec3 pos = PosAloCenter(palo);
    if (posLocal != glm::vec3(0.0f) && g_cmworld.pfnMatLo) pos += g_cmworld.pfnMatLo(palo) * posLocal;
    return pos;
}

// orig 0x1FDC08's stem: the player character's state (0x1C4A48: the forced player, else the current one; a
// PLAYER, cid 0x34). (g_pfnPloPlayer, used here before, is never set: the stem stayed 1, U46.)
int MusicStem()
{
    LO* pl = g_psw ? PloPoCurrent(g_psw) : nullptr;
    if (!pl || !FCidDerivedFrom(pl->Cid(), S3CID(0x34))) return -1;
    ACTOR* pa = PactorFromLo(pl);
    if (!pa) return -1;
    if (pa->n10EC == 3) return 0;
    if (pa->n10EC == 4) return 2;
    if (pa->n10C8 != -1) return 0;
    if (pa->n4C8 >= 0 && (pa->n4C8 < 3 || pa->n4C8 == 13)) return 0;
    return 1;
}

// orig 0x1FBC58's test: the footstep's character (0x6F Sly, 0x73 Bentley, 0x15 Murray) is the current player
bool FMoodStep(int sfxid)
{
    LO* pl = g_psw ? PloPoCurrent(g_psw) : nullptr;
    if (!pl) return false;
    ACTOR* pa = sfxid == 0x6F ? static_cast<ACTOR*>(PslyGlobal()) : sfxid == 0x73 ? static_cast<ACTOR*>(PbentleyGlobal())
                                                                                   : static_cast<ACTOR*>(PmurrayGlobal());
    return pa && pa->palo == pl;
}

// ---- the ambient sound objects (cid 0x2B, module 0x12E600): their sound records (0x12EEE0: {sound id, falloff
// start / end, volume, pitch}, 8 at most at +0xE8) and the resident ambience's falloffs (0x12EF40: {target id,
// start / end, volume}, 8 at most at +0xF4), filled by the loader's EOPID 99 / 100 methods
struct AMBREC
{
    struct SND { int id; float s0, s1, g, pitch; };
    struct FALL { int id; float s0, s1, g; };
    std::vector<SND> asnd;
    std::vector<FALL> afall;
    std::vector<CSndRef> ah;  // +0xEC the sounds while the object is active
    bool fFalloffs = false;
};
std::unordered_map<LO*, AMBREC> s_mpamb;
WORLD_TABLE(s_mpamb);   // (PC: moved aside while a kept world loads, worldglobals.h)

AMBREC& Amb(LO* plo) { return s_mpamb[plo]; }
}  // namespace
void AddAmbientFalloffs(LO* plo, struct AMBREC& r);
namespace
{

// The subtitle box (screen object 0x373, found by the HUD's 0x1A3020 at each world start): the cut scenes' box
// (its letterbox bars and the line 0x2EF), shown / hidden by the pause count with grf 1 (SW +0x3D80) going 0 -> 1 / back
// to 0 (0x20F128 / 0x20F160: at once, before the scene's first line starts). Outside the pauses a line shows in the
// binocucom windows 0x368..0x36A.
uint32_t s_serialSub = 0;
LO* s_ploSub = nullptr;

LO* PloSubtitleBox(SW* psw)
{
    // orig: 0x001D9520 calls 0x001A3020 at each world start to replace the subtitle object's pointer.
    // A new native SW can reuse its predecessor's address, so its serial identifies this binding's world.
    if (!psw) return nullptr;
    if (s_serialSub != psw->serial)
    {
        s_serialSub = psw->serial;
        s_ploSub = nullptr;
        for (LO* p : AploOfClass(psw, S3CID(0x7E)))
            if (p->oid == 0x373) s_ploSub = p;
    }
    return s_ploSub;
}

void UpdateSubtitleBox(SW* psw) { PloSubtitleBox(psw); }
}  // namespace

// orig: 0x001A3078 (the subtitle reset, then vt slot 49 with -1)
void ShowSubtitleBox()
{
    if (std::getenv("SLY2N_SUBDBG")) std::printf("subtitle box: shown (id %d) t %.3f\n", g_dialogsub.id, g_clock.t);
    LO* plo = PloSubtitleBox(g_psw);
    if (!plo) return;
    ResetDialogSubtitle();   // (0x227788)
    ShowScreenObj(plo, -1);
}

// orig: 0x001A30D0 (vt slot 50)
void HideSubtitleBox()
{
    if (std::getenv("SLY2N_SUBDBG")) std::printf("subtitle box: hidden (id %d) t %.3f\n", g_dialogsub.id, g_clock.t);
    if (LO* plo = PloSubtitleBox(g_psw)) HideScreenObj(plo);
}

namespace
{
struct SOUNDPASS
{
    SOUNDPASS()
    {
        // orig: 0x001251E8 (bif 129): create the sound, attach its source, and
        // return a BASIC reference (0x1294C0 adds a reference). The active sound
        // list owns its separate reference while playback is pending or active.
        SetBifGameHook(static_cast<BIFK>(129), [](int, CRef* argv, CFrame*) {
            CSndPlain* psnd = PsndNewPlain(argv[0].m_n);
            LO* source = dynamic_cast<LO*>(argv[1].m_pbasic);
            psnd->SetSource(source);
            CRef result = RefRetainSound(psnd);
            if (const char* selected = std::getenv("SLY2N_SNDSCRIPT"); selected && psnd->m_sfxid == std::atoi(selected))
                std::printf("sndscript create id %d owner %d sound %p t %.3f\n", psnd->m_sfxid,
                            source ? source->oid : -1, static_cast<void*>(psnd), g_clock.t);
            return result;
        });
        g_sndhooks.pfnFindMusicSources = [] {
            FindCharacters(g_psw);
            g_sndhooks.apsrcMusic = s_apchar;
        };
        RegisterSwPass(0x1FD830, [](SW* psw) {
            FindCharacters(psw);
            g_sndhooks.apsrcMusic = s_apchar;
            for (auto& [plo, r] : s_mpamb)
                if (!r.fFalloffs) AddAmbientFalloffs(plo, r);  // (every one of the world's, active or not)
            // debug: SLY2N_DIALOG=id[@frame] starts a dialog line (default frame 60)
            static int s_cframe = 0;
            static const char* s_szDialog = std::getenv("SLY2N_DIALOG");
            if (s_szDialog && ++s_cframe == (std::strchr(s_szDialog, '@') ? std::atoi(std::strchr(s_szDialog, '@') + 1) : 60))
                PsndStartDialog(std::atoi(s_szDialog), 0);
            UpdateSoundMgr(g_clock.dt, g_clock.dt40);
            UpdateSubtitleBox(psw);
        });
        // orig 0x101768 -> 0x12EEE0 (EOPID 99), 0x101848 -> 0x12EF40 (EOPID 100)
        RegisterObjectMethod(0x101768, [](LO* plo, const std::vector<CRef>& a) {
            AMBREC& r = Amb(plo);
            if (r.asnd.size() < 8)
                r.asnd.push_back({NArg(a, 0), float(NArg(a, 1)), float(NArg(a, 2)), GArg(a, 3), GArg(a, 4)});
            return CRef(TAGK_Void);
        });
        RegisterObjectMethod(0x101848, [](LO* plo, const std::vector<CRef>& a) {
            AMBREC& r = Amb(plo);
            if (r.afall.size() < 8) r.afall.push_back({NArg(a, 0), float(NArg(a, 1)), float(NArg(a, 2)), GArg(a, 3)});
            return CRef(TAGK_Void);
        });
    }
} s_soundpass;
}  // namespace

// orig: 0x00129A10 / 0x001294C0, returned BASIC ownership for 0x102038,
// 0x102430 and 0x1251E8. Playback keeps a separate reference in the active list.
CRef RefRetainSound(CSnd* sound)
{
    CRef result;
    if (sound)
    {
        sound->AddRef();
        result.SetOwnedBasic(std::shared_ptr<BASIC>(sound, [](BASIC* pb) { static_cast<CSnd*>(pb)->Release(); }));
    }
    else result.SetBasic(nullptr);
    return result;
}

namespace
{
void TraceSoundOption(CSnd* sound, int bifk, int optid)
{
    const char* selected = std::getenv("SLY2N_SNDBRIDGE");
    if (!selected) return;
    auto* plain = dynamic_cast<CSndPlain*>(sound);
    auto* voice = dynamic_cast<CSndVoice*>(sound);
    const int id = plain ? plain->m_sfxid : voice && voice->m_pstream ? voice->m_pstream->id : -1;
    if (std::strcmp(selected, "all") != 0 && id != std::atoi(selected)) return;
    auto* source = static_cast<LO*>(sound->m_psrc);
    std::printf("sndbridge id %d bif %d option %d volume %.9g pan %d pitch %.9g source %d "
                "position %.9g %.9g %.9g range %.9g %.9g pause %d dirty %u fade %.9g\n",
                id, bifk, optid, sound->m_gVol, sound->m_pan, sound->m_pitch,
                FBasicLive(source) ? source->oid : -1, sound->m_pos.x, sound->m_pos.y, sound->m_pos.z,
                sound->m_sStart, sound->m_sEnd, sound->m_cpause, unsigned(sound->m_grfDirty), sound->m_tFade);
}

// orig: 0x00101E18 / 0x00101E90 / 0x00101F08 / 0x00101F90.
// RefOpObjectOption retains the sound's owning CRef on each bound method.
template<int Optid> CRef RefSoundMethod(BASIC* pb, int, CRef* args)
{
    CSnd* sound = static_cast<CSnd*>(pb);
    if constexpr (Optid == 256) sound->Pause();        // 0x1FB018, no arguments
    if constexpr (Optid == 257) sound->Resume();       // 0x1FB060, no arguments
    if constexpr (Optid == 258) sound->Stop(args[0].RefCoerceF32().m_g);  // 0x1FB098, OTYP1
    if constexpr (Optid == 262)
    {
        sound->SetFalloff(args[0].RefCoerceF32().m_g, args[1].RefCoerceF32().m_g);  // 0x1FB4F0, OTYP1 x2
        if (const char* selected = std::getenv("SLY2N_SNDSCRIPT"); selected)
            if (auto* plain = dynamic_cast<CSndPlain*>(sound); plain && plain->m_sfxid == std::atoi(selected))
                std::printf("sndscript falloff id %d sound %p range %.1f %.1f\n", plain->m_sfxid,
                            static_cast<void*>(sound), sound->m_sStart, sound->m_sEnd);
    }
    TraceSoundOption(sound, 0, Optid);
    return CRef(TAGK_Void);
}
}

// orig: 0x001FFD98 / 0x00200BA0 / 0x00200C28, class 0x97's EOPIDs 127..136.
// The descriptor table makes volume/pitch floats, pan an integer, source a LO,
// position a vector, and falloff a read-only LM (two floats).
CRef RefSoundObjectOption(CSnd* sound, int bifk, int optid, const CRef* value)
{
    CRef result;
    if (bifk == 111)
    {
        PFNTHUNK thunk = nullptr;
        int count = 0;
        switch (optid)
        {
        case 212: result.SetF32(sound->m_gVol); break;
        case 213: result.SetF32(sound->m_pitch); break;
        case 259: result.SetS32(sound->m_pan); break;
        case 260: result.SetBasic(FBasicLive(static_cast<BASIC*>(sound->m_psrc))
                                     ? static_cast<BASIC*>(sound->m_psrc) : nullptr); break;
        case 261: result.SetVector(sound->m_pos); break;
        case 263: result.SetLm({sound->m_sStart, sound->m_sEnd}); break;
        case 256: thunk = RefSoundMethod<256>; break;
        case 257: thunk = RefSoundMethod<257>; break;
        case 258: thunk = RefSoundMethod<258>; count = 1; break;
        case 262: thunk = RefSoundMethod<262>; count = 2; break;
        default: return result;
        }
        if (thunk)
        {
            auto method = std::make_shared<CMethod>();
            method->m_pbasic = sound;
            method->m_crefReq = count;
            method->m_pfnthunk = thunk;
            result.SetMethod(method);
        }
    }
    else if (bifk == 110 && value)
    {
        switch (optid)
        {
        case 212: sound->SetVolume(value->RefCoerceF32().m_g); break;  // orig: 0x1FB1F8
        case 213: sound->SetPitch(value->RefCoerceF32().m_g); break;   // orig: 0x1FB2C0
        case 259: sound->SetPan(value->RefCoerceS32().m_n); break;    // orig: 0x1FB268
        case 260: sound->SetSource(FBasicLive(value->m_pbasic) ? dynamic_cast<LO*>(value->m_pbasic) : nullptr); break; // orig: 0x1FB2F8
        case 261: sound->SetPosition(value->GetVector()); break;    // orig: 0x1FB3F0
        default: return result;  // falloff (263) has no setter
        }
        result.SetTag(TAGK_Void);
    }
    TraceSoundOption(sound, bifk, optid);
    return result;
}

// orig 0x12ED20 (slot 23: each record's
// sound started on the object: 0x1FBB90 + 0x1FB2F8 + 0x1FB4F0 + 0x1FB1F8 + 0x1FB2C0)
// orig 0x12EC50 (the class's post-load, slot 15): its records' falloffs on the resident ambience's voices
void AddAmbientFalloffs(LO* plo, AMBREC& r)
{
    if (!r.fFalloffs && g_sound.sndResident)
    {
        r.fFalloffs = true;
        CSndVoice* pv = static_cast<CSndVoice*>(g_sound.sndResident.get());
        for (const AMBREC::FALL& f : r.afall)
        {
            pv->AddVoiceFalloff(IvoiceFromTarget(f.id), plo, f.s0, f.s1, f.g);
            if (std::getenv("SLY2N_SNDLOG"))
            {
                const glm::vec3 pos = PosSourceWorld(plo, glm::vec3(0.0f));
                std::printf("sound:   falloff target %d -> voice %d, %g..%g x %g at (%g %g %g), camera distance %g\n", f.id,
                            IvoiceFromTarget(f.id), f.s0, f.s1, f.g, pos.x, pos.y, pos.z, glm::length(pos - g_cm.pos));
            }
        }
    }
}

void OnAmbientRegistered(LO* plo)
{
    auto it = s_mpamb.find(plo);
    if (it == s_mpamb.end()) return;
    AMBREC& r = it->second;
    if (std::getenv("SLY2N_SNDLOG"))
    {
        const glm::vec3 pos = PosSourceWorld(plo, glm::vec3(0.0f));
        std::printf("sound: ambient object %d%s at (%.0f %.0f %.0f): %zu sounds, %zu falloffs%s\n", plo->oid, plo->ploCloneOf ? " (a copy)" : "",
                    pos.x, pos.y, pos.z, r.asnd.size(), r.afall.size(), r.fFalloffs ? " (again)" : "");
    }
    r.ah.clear();
    for (const AMBREC::SND& e : r.asnd)
    {
        if (std::getenv("SLY2N_SNDLOG"))
            std::printf("sound:   ambient sound %d falloff %g..%g volume %g pitch %g\n", e.id, e.s0, e.s1, e.g, e.pitch);
        CSnd* p = PsndNewPlain(e.id);
        p->SetSource(plo);
        p->SetFalloff(e.s0, e.s1);
        p->SetVolume(e.g);
        p->SetPitch(e.pitch);
        r.ah.emplace_back(p);
    }
}

// orig 0x12EDE8 (slot 22: the sounds stopped and dropped)
void OnAmbientUnregistered(LO* plo)
{
    auto it = s_mpamb.find(plo);
    if (it == s_mpamb.end()) return;
    for (CSndRef& h : it->second.ah)
        if (h) h->Stop(0.0f);
    it->second.ah.clear();
}

// orig: 0x1A6730 (CloneLo, vt slot 40: the object's data after +0x50 copied): the sound records (+0xE8) and the
// falloffs (+0xF4) go with a copy; natively they live in s_mpamb, so a copy (e.g. the three Crypt Hack terminals
// 65416, each with its hum 6390) had none and stayed silent when switched on. The active sounds are not copied.
void CloneAmbientSoundRecords(const LO* source, LO* target)
{
    const auto it = s_mpamb.find(const_cast<LO*>(source));
    if (it == s_mpamb.end()) return;
    AMBREC& r = s_mpamb[target];
    r.asnd = it->second.asnd;
    r.afall = it->second.afall;
}

// A world's end: the records go with its objects.
void ResetAmbientSounds() { s_mpamb.clear(); }

namespace
{
// The dialog line's targets (SOUNDMGR +0x22C..+0x240): the speaker's effect SMAs (the mouth, the face ...) whose
// goals the line's events set; target 3 is the speaker itself (its eyes: 0x1F80D0).
SMA* s_apsmaDialog[6] = {};
ALO* s_paloDialog = nullptr;

// orig: 0x001F67E0 (the SMA of the SM oid (-1: any) running on palo, from the list 0x3E126C)
SMA* PsmaFindAloSm(const ALO* palo, OID oidSm)
{
    for (SMA* p : g_apsma)
        if (p->palo == palo && (oidSm == -1 || (p->psm && p->psm->oid == oidSm))) return p;
    return nullptr;
}

// orig: 0x001FE5F0's tail (the speaker and its targets found, each SMA referenced)
void BindDialogTargets(int oidSpeaker)
{
    static const int s_aidEffect[6] = {1548, 1549, 1550, -1, 1551, 1552};  // D_002C3C40 (0x1FD0C0)
    // 0x15ED78 PloFindSwObjectByClass (kind 5: the first object of the speaker's class or a derived one; the line's
    // speaker is a class: 0x35 Sly, 0x36 Murray, 0x37 Bentley, ...)
    LO* plo = nullptr;
    if (g_psw)
        for (int c = oidSpeaker; !plo && c >= 0 && c < CID_Max && FCidDerivedFrom(c, oidSpeaker); ++c)
            if (!g_psw->mpcidaplo[c].empty()) plo = g_psw->mpcidaplo[c].front();
    if (std::getenv("SLY2N_TALKPRINT"))
        std::printf("talk: speaker class 0x%X: %d\n", oidSpeaker, plo ? plo->oid : -1);
    if (!plo || !FCidDerivedFrom(plo->Cid(), S3CID(0x30))) return;
    ALO* palo = static_cast<ALO*>(plo);
    for (int i = 0; i < 6; ++i)
    {
        if (i == 3)
        {
            s_paloDialog = palo;  // +0x238
            continue;
        }
        SMA* p = PsmaFindAloSm(palo, s_aidEffect[i]);
        s_apsmaDialog[i] = p;
        if (p) ++p->cref;  // (vt slot 0: AddRef)
    }
    if (std::getenv("SLY2N_TALKPRINT"))
        std::printf("talk: speaker %d targets %p %p %p - %p %p (t %.2f)\n", oidSpeaker, (void*)s_apsmaDialog[0], (void*)s_apsmaDialog[1],
                    (void*)s_apsmaDialog[2], (void*)s_apsmaDialog[4], (void*)s_apsmaDialog[5], g_clock.t);
}

// orig: 0x001FE0E8 (one event: target 3 the speaker's eyes 0x1F80D0, the others their SMA's goal 0x1F0660)
void FireDialogTarget(int /*oidSpeaker*/, int itarget, int32_t value)
{
    if (std::getenv("SLY2N_TALKPRINT")) std::printf("talk: target %d value %d (t %.2f)\n", itarget, value, g_clock.t);
    if (itarget == 3)
    {
        float g;
        std::memcpy(&g, &value, 4);
        if (s_paloDialog) SetAloEyesClosed(s_paloDialog, g);  // 0x1F80D0
        return;
    }
    SMA* p = s_apsmaDialog[itarget];
    if (!p || FSmaRetracted(p)) return;
    SetSmaGoal(p, value);  // 0x1F0660
}

// orig: 0x001FE738's part (target 5's goal 0x611: the mouth closed; the targets released and cleared)
void UnbindDialogTargets()
{
    if (SMA* p = s_apsmaDialog[5]; p && !FSmaRetracted(p)) SetSmaGoal(p, 0x611);
    for (SMA*& p : s_apsmaDialog)
    {
        if (p) --p->cref;  // (vt slot 1: Release)
        p = nullptr;
    }
    s_paloDialog = nullptr;
}
}  // namespace

// orig: 0x001F80D0 (the eyes (class 0x8E SAAs of the glob set, ALO +0xA4) of the object and its ALO children closed by g)
void SetAloEyesClosed(ALO* palo, float g)
{
    if (palo->pglobset)
        for (SAA* p : palo->pglobset->apsaa)
            if (p && p->cid == SAAK2_Eyes) SetSaaEyesClosed(p, g, g_clock.t);  // 0x1EE3B0
    for (LO* plo : palo->aploChild)
        if (plo && FCidDerivedFrom(plo->Cid(), S3CID(0x30))) SetAloEyesClosed(static_cast<ALO*>(plo), g);
}

// orig: 0x00210678 (SCPT 0x35 / an ASEG event: the eyes of the object oid (0x15E938 kind 4 around ploCtx) and of
// the shader animation idSaa (0x1EC918, an EYES one) closed by g)
void SetEyesClosedOf(LO* ploCtx, int oid, int idSaa, float g)
{
    ALO* palo = nullptr;
    if (oid != -1)
        if (LO* plo = PloFindSwNearest(ploCtx, oid); plo && FCidDerivedFrom(plo->Cid(), S3CID(0x30))) palo = static_cast<ALO*>(plo);
    SAA* psaa = nullptr;
    if (idSaa != -1)
        for (auto& p : g_shds.asaa)
            if (p->oid == idSaa && !p->fOwned)
            {
                psaa = p.get();
                break;
            }
    if (psaa && psaa->cid != SAAK2_Eyes) psaa = nullptr;
    if (palo) SetAloEyesClosed(palo, g);  // 0x1F80D0
    if (psaa) SetSaaEyesClosed(psaa, g, g_clock.t);  // 0x1EE3B0
}

// orig 0x1FCEA0 at boot, with the hooks the sound module asks the object model through
void StartupGameSound()
{
    g_sndhooks.pfnDialogBind = BindDialogTargets;
    g_sndhooks.pfnDialogEvent = FireDialogTarget;
    g_sndhooks.pfnDialogUnbind = UnbindDialogTargets;
    g_sndhooks.pfnListener = FListener;
    g_sndhooks.pfnFSourceAlive = FSourceAlive;
    g_sndhooks.pfnSourcePosWorld = PosSourceWorld;
    g_sndhooks.pfnMusicStem = MusicStem;
    g_sndhooks.pfnFMoodStep = FMoodStep;
    // the mood's step from the game's generator (0x11D3F8), as the original (R32). (debug: SLY2N_MOODPRIVRAND=1 a
    // private one, which leaves the gameplay's random stream untouched by the tiptoe's steps)
    if (!std::getenv("SLY2N_MOODPRIVRAND")) g_sndhooks.pfnRandInRange = NRandInRange;
    else
        g_sndhooks.pfnRandInRange = [](int lo, int hi) {
            static uint32_t s_seed = 1;
            s_seed = s_seed * 1103515245u + 12345u;
            return lo + static_cast<int>((s_seed >> 16) % static_cast<uint32_t>(hi - lo + 1));
        };
    g_sndhooks.pfnText = [](int id) { return StrLocalized(g_psw, id); };
    g_sndhooks.pfnTGame = [] { return g_clock.t; };
    // the gait's footsteps (actanim.cpp, 0x1FBB90 + 0x1FB2F8 with the walker as the source)
    g_pfnGaitFootstep = [](int sfx, ALO* palo, float gVolume) {
        if (CSnd* p = PsndStartSound(sfx, gVolume, 0, palo)) p->SetFalloff(500.0f, 2000.0f);  // (0x20BC08)
    };
    StartupSound();
    static bool s_fAtExit = false;
    if (!s_fAtExit) std::atexit([] { snd_StopSoundSystem(); });  // (the sound thread joined before its destructor)
    s_fAtExit = true;
}
