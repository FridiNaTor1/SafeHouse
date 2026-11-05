// The game's sound module (see sound.h). Ported from the reference port's readable C++ (Sly2-Reference
// game/sound.cpp, sound_c.cpp, sound_p3.cpp: verified ports of 0x1FAF70..0x1FF390) and the original's code for
// the rest (0x1FB0C0, 0x1FC270, 0x1FBEE0, 0x1FBF00, 0x1FBFB0, 0x1FE310, 0x1FE3F8, 0x1FE5B8, 0x1FE9C8).
// The 989snd / spiop commands are direct calls (snd989.h, spiop.h); their asynchronous callbacks run at once.

#include "sound.h"

#include <map>

#include "audio.h"
#include "bis.h"
#include "clock.h"
#include "loadtrace.h"
#include "snd989.h"
#include "spiop.h"
#include "splicetypes.h"
#include "disc.h"
#include "sly3/brx3.h"

#include <unordered_set>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <thread>

constinit SNDHOOKS g_sndhooks;
DIALOGSUB g_dialogsub;
SOUNDMGR g_sound;

namespace
{
// The previous world's streams, kept while sound objects may still point at them.
std::deque<SNDSTREAM> s_adialogOld, s_amusicOld;
std::vector<std::unique_ptr<SNDSTREAM>> s_apresidentOld;

float GLimit01(float g) { return std::clamp(g, 0.0f, 1.0f); }

// ps2_cvt_w: float to int, truncating (the volume compares at 1/1024)
int CvtW(float g) { return static_cast<int>(g); }

bool FNearOrigin(const glm::vec3& v)
{
    return std::fabs(v.x) < 0.0001f && std::fabs(v.y) < 0.0001f && std::fabs(v.z) < 0.0001f;
}

// Sly1-Release util.cpp GSmooth / CSolveQuadratic (0x11D018 in Sly 2). (Moves to util.cpp when it exists.)
int CSolveQuadraticSnd(float a, float b, float c, float* solutions)
{
    const float delta = b * b - a * c * 4.0f;
    if (delta < 0.0f) return 0;
    const float sqrtOver2a = std::sqrt(delta) / (a + a);
    const float bOver2a = b / (a + a);
    if (std::fabs(sqrtOver2a) < 0.0001f)
    {
        solutions[0] = -bOver2a;
        return 1;
    }
    solutions[0] = -bOver2a + sqrtOver2a;
    solutions[1] = -bOver2a - sqrtOver2a;
    return 2;
}

float GSmoothSnd(float gCur, float gTarget, float dt, const SMP* psmp)
{
    float delta = gCur - gTarget;
    const bool isNegative = delta < 0.0f;
    if (isNegative) delta = -delta;
    const float acc = psmp->svFast, vSlow = psmp->svSlow, dtFast = psmp->dtFast;
    const float accelDist = (vSlow + acc) * dtFast * 0.5f;
    bool finalStepHandled = false;
    if (accelDist <= delta)
    {
        const float accelTime = (delta - accelDist) / acc;
        if (dt <= accelTime)
        {
            delta -= acc * dt;
            finalStepHandled = true;
        }
        else
        {
            dt -= accelTime;
            delta = accelDist;
        }
    }
    if (!finalStepHandled)
    {
        float t2[2] = {0.0f, 0.0f};
        const float a = (acc - vSlow) / (2.0f * dtFast), b = vSlow, c = -delta;
        if (CSolveQuadraticSnd(a, b, c, t2) && t2[0] > dt)
        {
            const float t = t2[0] - dt;
            const float accelRate = (acc - vSlow) / dtFast;
            delta = (vSlow * t) + (0.5f * accelRate * t * t);
        }
        else
            delta = 0.0f;
    }
    return isNegative ? gTarget - delta : gTarget + delta;
}

bool FBusy(const CSnd* p) { return p->m_cbusy != 0 || (p->m_grfDirty & 1) != 0; }

CSndVoice* PvoiceTop()
{
    return g_sound.asndMusic.empty() ? nullptr : static_cast<CSndVoice*>(g_sound.asndMusic.back().get());
}

// orig: 0x1FD080 (where a stream is: 1 the music list, 0 the resident one, 2 a dialog line): spiop's track index
int ItrackFromStream(const SNDSTREAM* p)
{
    for (const SNDSTREAM& m : g_sound.amusic)
        if (&m == p) return 1;
    return p == g_sound.president.get() && p ? 0 : 2;
}

// The 3D attenuation of a point (0x1FB778): the falloff of its distance to the listener over [sStart, sEnd],
// the pan in degrees (0 when inaudible).
void Attenuate(const glm::vec3& pos, float sStart, float sEnd, float* pg, int* ppan)
{
    glm::vec3 posEar{};
    glm::mat3 mat(1.0f);
    if (!g_sndhooks.pfnListener || !g_sndhooks.pfnListener(&posEar, &mat))
    {
        if (pg) *pg = 1.0f;
        if (ppan) *ppan = 0;
        return;
    }
    const glm::vec3 d = pos - posEar;
    const float len = std::sqrt(glm::dot(d, d));
    const float g = GLimit01((sEnd - len) / (sEnd - sStart));
    if (pg) *pg = g;
    if (!ppan) return;
    if (g > 0.0f)
    {
        const glm::vec3 r = mat * d;
        float ang = std::atan2(-r.y, r.x);
        if (sEnd < sStart) ang += 3.14159265f;
        *ppan = CvtW(ang * 57.29578f);
    }
    else
        *ppan = 0;
}
}  // namespace

// ---------------------------------------------------------------------------------------------------------
// References

CSndRef::CSndRef(CSnd* p) : m_p(p)
{
    if (m_p) m_p->AddRef();
}

CSndRef& CSndRef::operator=(const CSndRef& o)
{
    if (o.m_p) o.m_p->AddRef();
    CSnd* pOld = m_p;
    m_p = o.m_p;
    if (pOld) pOld->Release();
    return *this;
}

CSndRef::~CSndRef()
{
    if (m_p) m_p->Release();
}

// ---------------------------------------------------------------------------------------------------------
// CSnd

// orig: 0x1FC0A0 / 0x1FCCA0 (the last reference destroys the object)
void CSnd::Release()
{
    if (--m_cref == 0) delete this;
}

bool CSnd::FPlaying() const { return m_h != 0 || m_cbusy != 0 || (m_grfDirty & 1) != 0; }

// orig: 0x1FB0C0 (start unless playing: start + volume / pan / pitch, paused when locked, 3D when attached or
// away from the origin)
void CSnd::Start()
{
    if (FPlaying()) return;
    uint32_t m = m_cpause > 0 ? 0x1F3u : 0x1F1u;
    if (m_psrc || !FNearOrigin(m_pos)) m |= 0x200;
    SetDirty(m, m);
}

// orig: 0x1FB018
void CSnd::Pause()
{
    if (++m_cpause == 1) SetDirty(6, 2);
}

// orig: 0x1FB060
void CSnd::Resume()
{
    if (--m_cpause == 0) SetDirty(6, 4);
}

// orig: 0x1FB098 (every dirty bit cleared but stop)
void CSnd::Stop(float tFade)
{
    m_tFade = tFade;
    SetDirty(0x1FFF, 8);
}

// orig: 0x1FB1F8
void CSnd::SetVolume(float x)
{
    const float g = GLimit01(x);
    if (CvtW(g * 1024.0f) != CvtW(m_gVol * 1024.0f))
    {
        m_gVol = g;
        SetDirty(0x10, 0x10);
    }
}

// orig: 0x1FB268
void CSnd::SetPan(int n)
{
    const int m = n % 360;
    const int d = m > -1 ? m : m + 360;
    if (d != m_pan)
    {
        m_pan = d;
        SetDirty(0x20, 0x20);
    }
}

// orig: 0x1FB2C0
void CSnd::SetPitch(float g)
{
    if (g == m_pitch) return;
    m_pitch = g;
    SetDirty(0x40, 0x40);
}

// orig: 0x1FB2F8
void CSnd::SetSource(void* psrc)
{
    m_psrc = psrc;
    if (psrc)
    {
        SetDirty(0x200, 0x200);
        return;
    }
    if (FNearOrigin(m_pos)) SetDirty(0x200, 0);
}

// orig: 0x1FB3F0
void CSnd::SetPosition(const glm::vec3& v)
{
    m_pos = v;
    if (!FNearOrigin(v)) SetDirty(0x200, 0x200);
    else if (!m_psrc) SetDirty(0x200, 0);
}

// orig: 0x1FB4F0
void CSnd::SetFalloff(float a, float b)
{
    if (b <= a) b = a + 1.0f;
    m_sEnd = b;
    m_sStart = a;
}

// orig: 0x1FB658 (+ 0x1FB6A8 / 0x1FB710: on the active list while any bit is set, holding a reference)
void CSnd::SetDirty(uint32_t m, uint32_t v)
{
    m &= 0xFFFF;
    v &= 0xFFFF;
    m_grfDirty = static_cast<uint16_t>((m_grfDirty & ~m) | (v & m));
    auto& a = g_sound.apsndActive;
    const auto it = std::find(a.begin(), a.end(), this);
    if (m_grfDirty)
    {
        if (it == a.end())
        {
            AddRef();
            a.push_back(this);
        }
    }
    else if (it != a.end())
    {
        *it = nullptr;  // (removed after the frame's walk: UpdateSoundMgr compacts the list)
        Release();
    }
}

// orig: 0x1FB528 (the 3D update of a sound attached to a source or at a position)
void CSnd::Update3D()
{
    if (m_psrc && g_sndhooks.pfnFSourceAlive && !g_sndhooks.pfnFSourceAlive(m_psrc))
    {
        Stop(0.0f);
        return;
    }
    const glm::vec3 v = m_psrc && g_sndhooks.pfnSourcePosWorld ? g_sndhooks.pfnSourcePosWorld(m_psrc, m_pos) : m_pos;
    float g = 1.0f;
    int pan = 0;
    Attenuate(v, m_sStart, m_sEnd, &g, &pan);
    const float r = m_gVol * g;
    if (CvtW(r * 1024.0f) != CvtW(m_gVolCur * 1024.0f))
    {
        if (m_gVolCur == 0.0f) OnAudible(0);
        else if (r == 0.0f) OnAudible(1);
        m_gVolCur = r;
        SetDirty(0x10, 0x10);
    }
    SetPan(pan);
}

// orig: 0x1FB8E0 (the frame update of an active sound)
void CSnd::UpdateFrame()
{
    CSndRef lock(this);
    if (m_grfDirty & 0x200) Update3D();
    else m_gVolCur = m_gVol;
    uint32_t fl = m_grfDirty;
    if (fl & 1)
    {
        StartIop();
        if (Kind() == 1) SetDirty(0x400, 0x400);
    }
    else if (m_h)
    {
        if (fl & 8)
        {
            StopIop();
            m_h = 0;
            SetDirty(0x1DFF, 0);
        }
        else
        {
            if (fl & 0x1C00) Requests();
            if (m_cbusy) return;
            if (m_grfDirty & 0x1F0)
            {
                SendChanges();
                SetDirty(0x1F0, 0);
            }
            fl = m_grfDirty;
            if (fl & 2)
            {
                PauseIop();
                SetDirty(2, 0);
            }
            else if (fl & 4)
            {
                ResumeIop();
                SetDirty(4, 0);
            }
        }
    }
    else if (!m_cbusy && !(fl & 0x200))
        SetDirty(0x1FFF, 0);
}

// ---------------------------------------------------------------------------------------------------------
// CSndPlain

// orig: 0x1FBBC8 (+ the base constructor 0x1FAFD0): starts at once
CSndPlain::CSndPlain(int sfxid) : m_sfxid(sfxid) { SetDirty(1, 1); }

// orig: 0x1FBC20 (the sound's bank entry: 2 a looper)
int CSndPlain::Kind()
{
    const int i = IsoundFromSfxid(m_sfxid);
    return i < 0 ? -1 : g_sound.aflagsSound[i];
}

// orig: 0x1FBC58 (the 0x30-byte SndPlayParams; a looper keeps its handle (asynchronous, callback 0x1FBAF8),
// a one-shot is fire and forget; then the mood check of sounds 0x6F / 0x73 / 0x15)
void CSndPlain::StartIop()
{
    const int isound = IsoundFromSfxid(m_sfxid);
    if (isound < 0 || !g_sound.hbank)
    {
        SetDirty(0x1FFF, 0);
        return;
    }
    if (m_grfDirty & 2)  // (started paused: dropped)
    {
        SetDirty(0x1FFF, 0);  // (the original takes it off the active list, keeping its bits)
        return;
    }
    SNDPLAYPARAMS m;
    m.mask = 3;
    m.hbank = g_sound.hbank;
    m.isound = isound;
    if (m_grfDirty & 0x10)
    {
        m.mask |= SNDPPM_Vol;
        m.vol = CvtW(m_gVolCur * 1024.0f);
    }
    if (m_grfDirty & 0x20)
    {
        m.mask |= SNDPPM_Pan;
        m.pan = m_pan;
    }
    if (m_grfDirty & 0x40)
    {
        m.mask |= SNDPPM_PitchMod;
        m.pm = static_cast<int16_t>(CvtW(m_pitch * 1524.0f));
    }
    if (g_sound.aflagsSound[isound] == 2)
    {
        m_cbusy = 1;
        CSndRef keep(this);
        const uint32_t h = snd_PlaySoundEx(m);
        SetDirty(0x7F, 0);
        // orig: 0x1FBAF8 (the callback: the handle, not busy)
        m_h = h;
        m_cbusy = 0;
        return;
    }
    snd_PlaySoundEx(m);
    SetDirty(0x1FFF, 0);
    // (0x6F / 0x73 / 0x15 of the player character: the mood sound, 0x1FEED0; needs the character objects)
}

// orig: 0x1FBE80 (a fade over +0x48 s with AutoVol -4 in 240ths, or at once)
void CSndPlain::StopIop()
{
    if (m_tFade > 0.0f) snd_AutoVol(m_h, -4, CvtW(m_tFade * 240.0f), 2);
    else snd_StopSound(m_h);
}

void CSndPlain::PauseIop() { snd_PauseSound(m_h); }      // orig: 0x1FBEE0
void CSndPlain::ResumeIop() { snd_ContinueSound(m_h); }  // orig: 0x1FBF00

// orig: 0x1FBF20 (volume in 1024ths, pan, pitch in 12ths)
void CSndPlain::SendChanges()
{
    const uint32_t fl = m_grfDirty;
    if (!(fl & 0x70)) return;
    uint32_t m = 0;
    int vol = 0, pm = 0;
    if (fl & 0x10)
    {
        m = 1;
        vol = CvtW(m_gVolCur * 1024.0f);
    }
    if (fl & 0x20) m |= 4;
    if (fl & 0x40)
    {
        m |= 8;
        pm = CvtW(m_pitch * 12.0f);
    }
    snd_SetSoundParams(m_h, m, vol, m_pan, pm, 0);
}

// orig: 0x1FBFB0 (poll "still playing": callback 0x1FBB30, at once here)
void CSndPlain::Requests()
{
    if (!(m_grfDirty & 0x400)) return;
    CSndRef keep(this);
    const uint32_t f = snd_SoundIsStillPlaying(m_h);
    SetDirty(0x400, 0);
    // orig: 0x1FBB30
    if (!f) Stop(0.0f);
    else SetDirty(0x400, 0x400);
}

// orig: 0x1FC018 (audible again: start; inaudible: a looper stops but keeps its 3D update, others stop)
void CSndPlain::OnAudible(int fInaudible)
{
    if (!fInaudible)
    {
        Start();
        return;
    }
    SetDirty(Kind() == 2 ? 0x1FFu : 0x3FFu, 8);
}

// ---------------------------------------------------------------------------------------------------------
// CSndVoice

// orig: 0x1FC190 (six voice volumes 1; paused when its channel's kind-2 volume is 0 or the manager is paused)
CSndVoice::CSndVoice(SNDSTREAM* pstream, uint32_t flags) : m_pstream(pstream), m_flags(flags)
{
    m_agVoice.fill(1.0f);
    SetDirty(0x801, 0x801);
    const int k = ItrackFromStream(pstream);
    if (g_sound.agVol[2][k] == 0.0f) Pause();
    if (g_sound.fMusicPaused) Pause();
}

// orig: 0x1FC270
void CSndVoice::Start()
{
    if (FPlaying()) return;
    CSnd::Start();
    m_tPlay = 0.0;
    m_grfVoiceChanged = 0xF;
}

int CSndVoice::Kind() { return (m_flags & 4) ? 2 : 1; }  // orig: 0x1FC4F8

// orig: 0x1FC510 ('SUKR' 1 {sector, vol, pan, track, flags | 1 (paused), headset}; callback 0x1FCBA8)
void CSndVoice::StartIop()
{
    const int n = ItrackFromStream(m_pstream);
    if (!m_pstream || m_pstream->fk.cb == 0)
    {
        SetDirty(0x1FFF, 0);
        return;
    }
    Pause();
    const int fHeadset = (g_sound.fEffect && n == 2) ? m_pstream->fNoHeadset == 0 : 0;
    m_cbusy = 1;
    CSndRef keep(this);
    const uint32_t id = SpiopStart(m_pstream->fk.isector, CvtW(m_gVolCur * 1024.0f), m_pan, n, m_flags | 1, fHeadset != 0);
    SetDirty(0x3F, 0);
    // orig: 0x1FCBA8 (the callback: its id, or failed: resumed and stopped)
    m_h = id;
    if (id) SetDirty(0x1000, 0x1000);
    else
    {
        Resume();
        m_cbusy = 0;
        if (!(m_grfDirty & 8)) Start();
    }
}

void CSndVoice::StopIop() { SpiopStop(m_h); }        // orig: 0x1FC698
void CSndVoice::PauseIop() { SpiopPause(m_h); }      // orig: 0x1FC6D0
void CSndVoice::ResumeIop() { SpiopContinue(m_h); }  // orig: 0x1FC708

// orig: 0x1FC740 ('SUKR' 5: volume / pan / pitch; 'SUKR' 6 per changed voice)
void CSndVoice::SendChanges()
{
    const uint32_t fl = m_grfDirty;
    if (fl & 0x70)
    {
        uint32_t f = 0;
        int vol = 0, pitch = 0;
        if (fl & 0x10)
        {
            f = 1;
            vol = CvtW(m_gVolCur * 1024.0f);
        }
        if (fl & 0x20) f |= 4;
        if (fl & 0x40)
        {
            f |= 8;
            pitch = CvtW(m_pitch * 12.0f);
        }
        SpiopSetParams(m_h, f, vol, m_pan, pitch);
    }
    if (m_grfDirty & 0x180)
    {
        for (int i = 0; i < 6; ++i)
            if (m_grfVoiceChanged & (1u << i)) SpiopSetChannel(m_h, i, CvtW(m_agVoice[i] * 1024.0f), m_apanVoice[i]);
        m_grfVoiceChanged = 0;
    }
}

// orig: 0x1FC878 (poll "started" ('SUKR' 7, callback 0x1FCC30) and "active" ('SUKR' 8, callback 0x1FBB30); the
// play clock runs by the manager's raw elapsed time while not paused)
void CSndVoice::Requests()
{
    if (m_grfDirty & 0x1000)
    {
        CSndRef keep(this);
        const int f = SpiopFStarted(m_h);
        SetDirty(0x1000, 0);
        // orig: 0x1FCC30
        if (f)
        {
            m_cbusy = 0;
            Resume();
        }
        else
            SetDirty(0x1000, 0x1000);
    }
    if (m_cbusy) return;
    if (m_grfDirty & 0x400)
    {
        CSndRef keep(this);
        const int f = SpiopFActive(m_h);
        SetDirty(0x400, 0);
        // orig: 0x1FBB30
        if (!f) Stop(0.0f);
        else SetDirty(0x400, 0x400);
    }
    if (!(m_grfDirty & 0x800)) return;
    if (m_cpause > 0) return;
    m_tPlay += g_sound.tClock - g_sound.tClockPrev;
}

// orig: 0x1FC2E0
void CSndVoice::SetVoiceVolume(int k, float g)
{
    if (k < 0 || k >= 6) return;
    const float f = GLimit01(g);
    if (CvtW(f * 1024.0f) != CvtW(m_agVoice[k] * 1024.0f))
    {
        m_grfVoiceChanged |= 1u << k;
        m_agVoice[k] = f;
        SetDirty(0x80, 0x80);
    }
}

// orig: 0x1FC380
void CSndVoice::SetVoicePan(int k, int deg)
{
    if (k < 0 || k >= 6) return;
    const int m = deg % 360;
    const int n = m > -1 ? m : m + 360;
    if (n == m_apanVoice[k]) return;
    m_grfVoiceChanged |= 1u << k;
    m_apanVoice[k] = n;
    SetDirty(0x100, 0x100);
}

// orig: 0x1FC408
void CSndVoice::AddVoiceFalloff(int k, void* psrc, float sStart, float sEnd, float g)
{
    if (k < 0 || k >= 6 || m_aafalloff[k].size() >= 8) return;
    SetDirty(0x200, 0x200);
    FALLOFF f{psrc, sStart, sEnd, g};
    if (f.sStart == f.sEnd) f.sEnd += 1.0f;
    m_aafalloff[k].push_back(f);
}

// orig: 0x1FC9B0 (its own source unless at the origin without one; each voice's loudest falloff entry, the pan
// of the only live one)
void CSndVoice::Update3D()
{
    if (m_psrc || !FNearOrigin(m_pos)) CSnd::Update3D();
    for (int i = 0; i < 6; ++i)
    {
        if (m_aafalloff[i].empty()) continue;
        float g = 0.0f;
        int pan = 0;
        bool fFirst = true;
        int cLive = 0;
        for (const FALLOFF& f : m_aafalloff[i])
        {
            if (!f.psrc) continue;
            if (g_sndhooks.pfnFSourceAlive && !g_sndhooks.pfnFSourceAlive(f.psrc)) continue;
            if (!g_sndhooks.pfnSourcePosWorld) continue;
            cLive++;
            float gf = 0.0f;
            Attenuate(g_sndhooks.pfnSourcePosWorld(f.psrc, glm::vec3(0.0f)), f.sStart, f.sEnd, &gf,
                      fFirst ? &pan : nullptr);
            fFirst = false;
            g = std::max(gf * f.g, g);
        }
        if (cLive != 1) pan = 0;
        SetVoicePan(i, pan);
        SetVoiceVolume(i, g);
    }
}

// ---------------------------------------------------------------------------------------------------------
// The manager

// orig: 0x1FBB90
CSndPlain* PsndNewPlain(int sfxid)
{
    static const bool s_fLog = std::getenv("SLY2N_SNDLOG") != nullptr;
    if (s_fLog) std::printf("sound: sfx %d (bank sound %d)\n", sfxid, IsoundFromSfxid(sfxid));
    return new CSndPlain(sfxid);
}

// orig: 0x1FC138
CSndVoice* PsndNewVoice(SNDSTREAM* pstream, uint32_t flags)
{
    if (!pstream) return nullptr;
    return new CSndVoice(pstream, flags);
}

CSnd* PsndStartSound(int sfxid, float gVol, int pan, void* psrc, const glm::vec3* ppos)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    CSndPlain* p = PsndNewPlain(sfxid);
    p->SetVolume(gVol);
    if (pan) p->SetPan(pan);
    if (ppos) p->SetPosition(*ppos);
    p->SetSource(psrc);
    return p;
}

SNDSTREAM* PstreamFindDialog(int id)  // orig: 0x1FCFA0 (newest first)
{
    for (auto it = g_sound.adialog.rbegin(); it != g_sound.adialog.rend(); ++it)
        if (it->id == id) return &*it;
    return nullptr;
}

SNDSTREAM* PstreamFindMusic(int id)  // orig: 0x1FCFF8 (newest first)
{
    for (auto it = g_sound.amusic.rbegin(); it != g_sound.amusic.rend(); ++it)
        if (it->id == id) return &*it;
    return nullptr;
}

// orig: 0x1FE990
int IvoiceFromTarget(int id)
{
    for (std::size_t i = 0; i < g_sound.aidTarget.size() && i < 6; ++i)
        if (g_sound.aidTarget[i] == id) return static_cast<int>(i);
    return -1;
}

// orig: 0x1FD028
int IsoundFromSfxid(int n)
{
    if (n < 0 || n >= static_cast<int>(g_sound.aisoundFromId.size())) return -1;
    const int i = g_sound.aisoundFromId[n];
    return i >= 0 && i < static_cast<int>(g_sound.aflagsSound.size()) ? i : -1;
}

// orig: 0x1FCD38 (the constructor's defaults) + 0x1FCEA0 StartupSound
void StartupSound()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    snd_StartSoundSystem();
    // ('SUKR' 0: spiop's entry)
    g_sound.gMaster = 1.0f;
    for (int ichan = 0; ichan < 4; ++ichan)
    {
        g_sound.agChannel[ichan] = 1.0f;
        for (int kind = 0; kind < 8; ++kind) g_sound.agVol[kind][ichan] = 1.0f;
    }
    // PC: the launcher's volume sliders (no change at their 100% defaults)
    for (int ichan = 0; ichan < 4; ++ichan) SendSoundChannelVolume(ichan);
    g_sound.grfState = 3;
    // orig: 0x1FCEA0 initializes both stamps from the same 0x14D240 raw tick sample.
    g_sound.tClock = g_sound.tClockPrev = static_cast<double>(TickNow()) / 294912000.0;
}

void ShutdownSound()
{
    ResetSoundMgr();
    snd_StopSoundSystem();
    ShutdownAudio();
}

// Every sound object alive (natively: the world change lets go of their sources, see ResetSoundMgr)
namespace
{
std::unordered_set<CSnd*>& SetSndLive()
{
    static auto* p = new std::unordered_set<CSnd*>();  // (leaked: sounds may outlive the statics at exit)
    return *p;
}
}
CSnd::CSnd() { std::lock_guard<std::recursive_mutex> lock(g_mtxSound); SetSndLive().insert(this); }
CSnd::~CSnd() { std::lock_guard<std::recursive_mutex> lock(g_mtxSound); SetSndLive().erase(this); }

// A movie that leads to a world change plays after the old world's sounds are gone (the original plays it inside
// the transition, after 0x1FD700); native plays queued movies before the transition, so they are stopped here.
void StopSoundsForMovie()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    snd_StopAllSounds();
    SpiopStopAll();
}

// orig: 0x1FD700 (before a world loads)
void ResetSoundMgr()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    snd_StopAllSounds();
    SpiopStopAll();
    SOUNDMGR& s = g_sound;
    s.aisoundFromId.clear();
    s.aidTarget.clear();
    s.fModeAlert = s.fModeSneak = 0;
    s.mood = 0;
    s_adialogOld = std::move(s.adialog);
    s_amusicOld = std::move(s.amusic);
    s.adialog.clear();
    s.amusic.clear();
    if (s.president) s_apresidentOld.push_back(std::move(s.president));
    if (s_apresidentOld.size() > 2) s_apresidentOld.erase(s_apresidentOld.begin());
    s.aflagsSound.clear();
    s.agStem = {};
    s.sndDialog = CSndRef();
    s.asndMusic.clear();
    s.sndResident = CSndRef();
    s.aoidTarget = {};
    s.grfState = 3;
    ResetDialogSubtitle();
    // the old world's objects are about to be freed: no sound may keep one as its source (the original's pools
    // are reset with the world; natively a sound still referenced elsewhere, e.g. a voice's falloff list
    // m_aafalloff, read a freed object in PosSourceWorld after a job restart's reload)
    g_sndhooks.apsrcMusic = {};  // (the music stems' characters: found again in the new world, soundglue.cpp)
    ++g_sndhooks.cReset;
    for (CSnd* p : SetSndLive())
    {
        p->m_psrc = nullptr;
        if (CSndVoice* pv = dynamic_cast<CSndVoice*>(p))
            for (auto& a : pv->m_aafalloff) a.clear();
    }
    // (the sound object pools reset: whatever is still active goes)
    std::vector<CSnd*> a;
    a.swap(s.apsndActive);
    for (CSnd* p : a)
        if (p)
        {
            p->m_grfDirty = 0;
            p->Release();
        }
    ResetSoundVolumes();
}

// orig: 0x1FD0E8 (the music controller's block of the world file)
void LoadMusicFromBrx(CBinaryInputStream* pbis)
{
    LOAD_SCOPE(0x1FD0E8, pbis);
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    SOUNDMGR& s = g_sound;
    uint8_t key[0x40];
    pbis->ReadRecord64(key);  // 0x118458
    uint32_t a8, a38, b8, b38;
    std::memcpy(&a8, key + 8, 4);
    std::memcpy(&a38, key + 0x38, 4);
    std::memcpy(&b8, s.keyBank + 8, 4);
    std::memcpy(&b38, s.keyBank + 0x38, 4);
    if ((a8 ^ a38) != (b8 ^ b38))  // another bank (by id): the old one goes
    {
        std::memcpy(s.keyBank, key, sizeof(key));
        if (s.hbank)
        {
            snd_UnloadBank(s.hbank);
            s.hbank = 0;
        }
    }
    const int n = pbis->S16Read();
    s.aflagsSound.assign(std::max(n, 0), 0);
    for (int i = 0; i < n; ++i) s.aflagsSound[i] = pbis->S8Read();
    const int m = pbis->S16Read();
    s.aisoundFromId.assign(std::max(m, 0), -1);
    for (int i = 0; i < n; ++i)
    {
        const int idx = pbis->S16Read();
        if (idx >= 0 && idx < m) s.aisoundFromId[idx] = i;
    }
    const int nt = pbis->S16Read();
    if (nt > 0)
    {
        if (s.president) s_apresidentOld.push_back(std::move(s.president));
        s.president = std::make_unique<SNDSTREAM>();
        pbis->ReadRecord64(s.president->key);  // 0x118458
        s.president->fk = FkFromRecord(s.president->key);
        s.sndResident = CSndRef(PsndNewVoice(s.president.get(), 4));
    }
    s.aidTarget.clear();
    for (int i = 0; i < nt; ++i) s.aidTarget.push_back(pbis->S16Read());
    const int nl = pbis->U8Read();
    int sel = 0;
    s.alanguage.clear();
    for (int i = 0; i < nl; ++i)
    {
        s.alanguage.push_back(pbis->U8Read());
        if (s.alanguage.back() == s.language) sel = i;
    }
    s.ilanguage = sel;
    const int ne = pbis->S32Read();
    for (int i = 0; i < ne; ++i)
    {
        SNDSTREAM e;
        e.id = pbis->S16Read();
        e.oidSpeaker = pbis->S16Read();
        for (int j = 0; j < nl; ++j)
        {
            uint8_t k[0x40];
            pbis->ReadRecord64(k);  // 0x118458
            const int g = pbis->S8Read();
            const float f = pbis->F32Read();
            if (j != sel) continue;
            std::memcpy(e.key, k, sizeof(k));
            e.fk = FkFromRecord(k);
            e.fNoHeadset = g;
            e.tLength = f;
        }
        const int c = pbis->S32Read();
        e.aevent.resize(std::max(c, 0));
        pbis->Read(static_cast<std::size_t>(std::max(c, 0)) * 8, e.aevent.data());  // the original reads even 0 bytes
        s.adialog.push_back(std::move(e));
    }
    const int no = pbis->S32Read();
    for (int i = 0; i < no; ++i)
    {
        SNDSTREAM o;
        o.id = pbis->S16Read();
        pbis->ReadRecord64(o.key);  // 0x118458
        o.fk = FkFromRecord(o.key);
        s.amusic.push_back(std::move(o));
    }
}

// orig: 0x1FD5E8 (the bank opened once from its key; the loopers among its sounds found)
void LoadSoundBank()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    SOUNDMGR& s = g_sound;
    if (!s.hbank)
    {
        const FK fk = FkFromRecord(s.keyBank);
        if (fk.cb != 0) s.hbank = snd_BankLoadByLoc(fk.isector, 0);
        snd_ResolveBankXREFS();
    }
    for (std::size_t i = 0; i < s.aflagsSound.size(); ++i)
    {
        if (s.aflagsSound[i] == 2) continue;
        if (snd_IsSoundALooper(s.hbank, static_cast<int>(i))) s.aflagsSound[i] = 2;
    }
}

// orig: 0x1FE478 (takes the player of music n out of the stack)
static CSndRef RefTakeMusic(int n)
{
    const SNDSTREAM* v = PstreamFindMusic(n);
    auto& a = g_sound.asndMusic;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (static_cast<CSndVoice*>(a[i].get())->m_pstream == v)
        {
            CSndRef r = a[i];
            a.erase(a.begin() + static_cast<std::ptrdiff_t>(i));
            return r;
        }
    return CSndRef();
}

// orig: 0x1FE150 (pushes music n: the top fades out; a player already for n resumes (Start), else a new one
// (looping) with the three characters' falloffs on voices 3 and 4)
CSndVoice* PsndPushMusic(int n)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    if (std::getenv("SLY2N_SNDLOG")) std::printf("sound: push music %d\n", n);
    CSndRef r = RefTakeMusic(n);
    if (CSndVoice* top = PvoiceTop()) top->Stop(0.0f);
    if (r) r->Start();
    else
    {
        SNDSTREAM* v = PstreamFindMusic(n);
        if (!v || g_sound.asndMusic.size() == 8) return nullptr;
        r = CSndRef(PsndNewVoice(v, 4));
        CSndVoice* p = static_cast<CSndVoice*>(r.get());
        if (g_sndhooks.pfnFindMusicSources) g_sndhooks.pfnFindMusicSources();
        p->AddVoiceFalloff(3, g_sndhooks.apsrcMusic[0], 2000.0f, 5000.0f, 1.0f);
        p->AddVoiceFalloff(3, g_sndhooks.apsrcMusic[1], 2000.0f, 5000.0f, 1.0f);
        p->AddVoiceFalloff(4, g_sndhooks.apsrcMusic[2], 2000.0f, 5000.0f, 1.0f);
    }
    g_sound.asndMusic.push_back(r);
    return static_cast<CSndVoice*>(r.get());
}

// orig: 0x1FE310 (takes music n out of the stack; fades it out when it plays; the new top starts again)
void PopMusic(int n)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    if (std::getenv("SLY2N_SNDLOG")) std::printf("sound: pop music %d\n", n);
    CSndRef r = RefTakeMusic(n);
    if (r)
    {
        if (r->FPlaying()) r->Stop(0.0f);
        r = g_sound.asndMusic.empty() ? CSndRef() : g_sound.asndMusic.back();
        if (r) r->Start();
    }
}

// orig: 0x1FE3F8 (the top music's play time)
float TMusicTop()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    const CSndVoice* p = PvoiceTop();
    return p ? static_cast<float>(p->m_tPlay) : 0.0f;
}

// orig: 0x1FE0E8 (one event of the dialog line)
static void FireDialogEvent(const SNDSTREAM::EVENT& e)
{
    if (e.itarget == -1 || e.itarget < 0 || e.itarget >= 6) return;
    if (e.itarget != 3 && e.value == -1) return;
    if (g_sndhooks.pfnDialogEvent) g_sndhooks.pfnDialogEvent(g_sound.oidDialogSpeaker, e.itarget, e.value);
}

// orig: 0x1FE738 (stops the dialog: its remaining events, the playing line, the targets)
void StopDialog()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    SOUNDMGR& s = g_sound;
    if (!s.sndDialog) return;
    const SNDSTREAM* pe = static_cast<CSndVoice*>(s.sndDialog.get())->m_pstream;
    while (s.ievent < static_cast<int>(pe->aevent.size())) FireDialogEvent(pe->aevent[s.ievent++]);
    if (g_sndhooks.pfnDialogUnbind) g_sndhooks.pfnDialogUnbind();  // (target 5 to 0x611, the targets released)
    s.sndDialog->Stop(0.0f);
    s.sndDialog = CSndRef();
    s.tDialog = 0.0f;
    s.ievent = 0;
    s.tEvent = 0.0f;
    s.aoidTarget = {};
    s.oidDialogSpeaker = -1;
}

// orig: 0x1FE5F0 (starts dialog line k (its constructor starts it): the old one ends, the line announced unless
// fQuiet (subtitles))
CSndVoice* PsndStartDialog(int k, int fQuiet)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    StopDialog();
    if (std::getenv("SLY2N_SNDLOG")) std::printf("sound: dialog %d%s\n", k, fQuiet ? " (quiet)" : "");
    static const bool s_fSubPrint = std::getenv("SLY2N_SUBPRINT") != nullptr;   // (debug: each line's text)
    if (s_fSubPrint && g_sndhooks.pfnText) std::printf("subtitle %d: %s\n", k, g_sndhooks.pfnText(k).c_str());
    SNDSTREAM* pd = PstreamFindDialog(k);
    if (!pd) return nullptr;
    SOUNDMGR& s = g_sound;
    s.sndDialog = CSndRef(PsndNewVoice(pd, 0));
    s.ievent = 0;
    s.tEvent = 0.0f;
    s.tDialog = 0.0f;
    s.oidDialogSpeaker = pd->oidSpeaker;
    if (!fQuiet)
    {
        SetDialogSubtitle(k, pd->oidSpeaker);
        if (g_sndhooks.pfnDialogStarted) g_sndhooks.pfnDialogStarted(k, pd->oidSpeaker);
    }
    if (pd->oidSpeaker != -1 && !pd->aevent.empty() && g_sndhooks.pfnDialogBind) g_sndhooks.pfnDialogBind(pd->oidSpeaker);
    return static_cast<CSndVoice*>(s.sndDialog.get());
}

// orig: 0x2275B8 (the speaker's colour)
uint32_t RgbaDialogSpeaker(int k)
{
    switch (k)
    {
    case 0x35: return 0x7F996633u;
    case 0x37: return 0x7F339933u;
    case 0x36: return 0x7F663399u;
    case 0x3F: return 0x7F333399u;
    case 0x3C: return 0x7F993399u;
    default: return 0x7F666666u;
    }
}

// orig: 0x227728
void SetDialogSubtitle(int id, int oidSpeaker)
{
    ResetDialogSubtitle();
    g_dialogsub.id = id;
    g_dialogsub.rgba = RgbaDialogSpeaker(oidSpeaker);
}

// orig: 0x227788
void ResetDialogSubtitle() { g_dialogsub = DIALOGSUB{}; }

// orig: 0x2277D0 (the line's text)
bool FDialogSubtitleText(std::string* pstr)
{
    if (g_dialogsub.id == -1 || !g_sndhooks.pfnText) return false;
    *pstr = g_sndhooks.pfnText(g_dialogsub.id);
    return true;
}

// orig: 0x227820 (on the reader's text, already wrapped: its first n characters by the line's time, then '|' while
// typing, else blinking at 4 Hz)
std::string StrDialogSubtitleTyped(const std::string& strWrapped)
{
    std::string str = strWrapped;
    if (g_dialogsub.id == -1) return str;
    const SNDSTREAM* p = PstreamFindDialog(g_dialogsub.id);
    const float f = (p ? p->tLength : 0.0f) - 0.5f;
    const int c = static_cast<int>(str.size());
    int n = c;
    if (!(f <= 0.0f) && FDialogPlaying(g_dialogsub.id)) n = static_cast<int>(TDialog() / f * static_cast<float>(c));
    bool fCursor;
    if (n < c)
    {
        str.resize(static_cast<std::size_t>(std::max(n, 0)));
        fCursor = true;
    }
    else
        fCursor = (static_cast<int>((g_sndhooks.pfnTGame ? g_sndhooks.pfnTGame() : float(g_sound.tClock)) * 4.0f) & 1) != 0;
    if (fCursor) str += "|";  // (0x227820: the literal at 0x2A97F0)
    return str;
}

// (0x2277D0 + 0x227820 on the unwrapped text)
bool FDialogSubtitle(std::string* pstr, uint32_t* prgba)
{
    std::string str;
    if (!FDialogSubtitleText(&str)) return false;
    *pstr = StrDialogSubtitleTyped(str);
    *prgba = g_dialogsub.rgba;
    return true;
}

int IdDialogCur()   // (debug: the current line's id, -1 none)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    const CSndVoice* p = static_cast<CSndVoice*>(g_sound.sndDialog.get());
    return p ? p->m_pstream->id : -1;
}

// orig: 0x1FE848 (-1: any line is current. The reference's own port, Sly2-Reference game/sound_p3.cpp, rejected -1,
// so its non --interp-only runs did not wait on SCPT 0x52 / splice 0x102150 with no line: the original's asm does)
bool FDialogPlaying(int n)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    const CSndVoice* p = static_cast<CSndVoice*>(g_sound.sndDialog.get());
    return p && (n == -1 || p->m_pstream->id == n);
}

// orig: 0x1FE888 (the dialog's play time, or the elapsed time while a stopped line's clock has not started)
float TDialog()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    const CSndVoice* p = static_cast<CSndVoice*>(g_sound.sndDialog.get());
    if (!p) return 0.0f;
    if (p->m_tPlay == 0.0 && p->m_h == 0 && !FBusy(p)) return g_sound.tDialog;
    return static_cast<float>(p->m_tPlay);
}

// orig: 0x1FE5B8
void SetSoundMgrFlags(uint32_t mask, int fSet)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    if (fSet) g_sound.grfState |= mask;
    else g_sound.grfState &= ~mask;
}

// orig: 0x1FE9C8 (989snd SetPlaybackMode, cmd 0xB: stereo / mono)
void SetSoundPlaybackMode(int mode)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    g_sound.playbackMode = mode;
}

// orig: 0x1FE9F8 (the master volume, sent when it changes at 1/1024)
void SetSoundMasterVolume(float g)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    const float gLim = GLimit01(g);
    const int n = CvtW(gLim * 1024.0f);
    if (CvtW(g_sound.gMaster * 1024.0f) == n) return;
    g_sound.gMaster = gLim;
    snd_SetMasterVolume(SND_GROUP_MASTER, n);
}

// orig: 0x1FEA60 (one volume factor; the channel's volume is the product of its eight; kind 2 at 0 / back pauses /
// resumes the channel's streams)
void SetSoundChannelVolume(int ichan, int kind, float g)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    if (kind < 0 || kind >= 8 || ichan < 0 || ichan >= 4) return;
    SOUNDMGR& s = g_sound;
    const float gLim = GLimit01(g);
    if (s.agVol[kind][ichan] == gLim) return;
    if (kind == 2)
    {
        int pmf = 0;  // 1 mute (pause), 2 unmute (resume)
        if (gLim == 0.0f) pmf = 1;
        else if (s.agVol[2][ichan] == 0.0f) pmf = 2;
        if (pmf)
        {
            auto call = [&](CSnd* p) {
                if (p) (pmf == 1 ? p->Pause() : p->Resume());
            };
            switch (ichan)
            {
            case 0: call(s.sndResident.get()); break;
            case 1:
                for (CSndRef& r : s.asndMusic) call(r.get());
                break;
            case 2: call(s.sndDialog.get()); break;
            default: break;
            }
        }
    }
    s.agVol[kind][ichan] = gLim;
    SendSoundChannelVolume(ichan);
}

namespace
{
// PC: the launcher's Music / Sound effects / Dialogue sliders (the shared settings, as Sly1-Release applies them: a
// linear factor on the output level), one factor per channel. 989snd squares the group's master with the volume
// (AdjustVolToGroup), so the channel's product carries the slider's square root; at 100% the factor is exactly 1.
std::array<float, 4> s_agUser = {1.0f, 1.0f, 1.0f, 1.0f};  // channel 0 ambience, 1 music, 2 dialog, 3 effects
}  // namespace

// The channel's volume (its eight factors, then the user's) sent to its 989snd group when it changes at 1/1024
void SendSoundChannelVolume(int ichan)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    if (ichan < 0 || ichan >= 4) return;
    SOUNDMGR& s = g_sound;
    float gChan = 1.0f;
    for (int k = 7; k >= 0; --k) gChan *= s.agVol[k][ichan];
    gChan *= s_agUser[size_t(ichan)];
    const int n = CvtW(gChan * 1024.0f);
    if (CvtW(s.agChannel[ichan] * 1024.0f) == n) return;
    s.agChannel[ichan] = gChan;
    snd_SetMasterVolume(ichan, n);
}

void SetSoundUserVolumes(float gMusic, float gEffects, float gDialog)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    auto factor = [](float g) {
        if (!std::isfinite(g) || g <= 0.0f) return 0.0f;
        return g >= 1.0f ? 1.0f : std::sqrt(g);
    };
    s_agUser = {factor(gEffects), factor(gMusic), factor(gDialog), factor(gEffects)};
    for (int ichan = 0; ichan < 4; ++ichan) SendSoundChannelVolume(ichan);
}

// orig: 0x1FEDF0 (all volumes to 1, kind 1 excepted)
void ResetSoundVolumes()
{
    SetSoundMasterVolume(1.0f);
    for (int kind = 0; kind < 8; ++kind)
    {
        if (kind == 1) continue;
        for (int ichan = 0; ichan < 4; ++ichan) SetSoundChannelVolume(ichan, kind, 1.0f);
    }
}

// orig: 0x1FEED0 (the mood: a random step from the tables at 0x2A82A8 / 0x2A8350; sound 0x28 with it as the
// pitch modifier... the original's field +0x20 of the play block)
void StepSoundMood()
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    SOUNDMGR& s = g_sound;
    if (s.istem) return;
    int32_t asteps[42], arows[25];
    std::memcpy(asteps, g_disc.PbElf(0x2A82A8, sizeof(asteps)), sizeof(asteps));
    std::memcpy(arows, g_disc.PbElf(0x2A8350, sizeof(arows)), sizeof(arows));
    static uint32_t s_seed = 1;
    s_seed = s_seed * 1103515245u + 12345u;
    const int r = static_cast<int>((s_seed >> 16) % 6);  // (0x11D3F8: 0..5)
    const int irow = std::clamp(s.mood + 12, 0, 24);
    s.mood += asteps[std::clamp(r + arows[irow] * 6, 0, 41)];
    const int isound = IsoundFromSfxid(0x28);
    if (isound < 0 || !s.hbank) return;
    SNDPLAYPARAMS m;
    m.mask = 0x13;
    m.hbank = s.hbank;
    m.isound = isound;
    m.pm = static_cast<int16_t>(s.mood * 127);
    snd_PlaySoundEx(m);
}

// orig: 0x1FDC08 (the three stem weights: the selected one goes to 1, the others share the rest)
static void UpdateStemWeights(float dtFrame)
{
    SOUNDMGR& s = g_sound;
    s.istem = 1;
    if (s.fModeAlert) s.istem = 2;
    else if (s.fModeSneak) s.istem = 0;
    else if (g_sndhooks.pfnMusicStem)
    {
        const int k = g_sndhooks.pfnMusicStem();
        if (k >= 0 && k <= 2) s.istem = k;
    }
    CSndVoice* ref = PvoiceTop();
    float* w = s.agStem.data();
    if (!ref || FBusy(ref)) w[s.istem] = 1.0f;
    else
    {
        const SMP smp{s.asmpStem[s.istem * 3], s.asmpStem[s.istem * 3 + 1], s.asmpStem[s.istem * 3 + 2]};
        w[s.istem] = GSmoothSnd(w[s.istem], 1.0f, dtFrame, &smp);
    }
    float sum = 0.0f;
    const float sq = w[s.istem] * w[s.istem];
    for (int i = 0; i < 3; ++i)
        if (i != s.istem) sum += w[i] * w[i];
    if (sum < 0.0001f)
        for (int i = 0; i < 3; ++i) w[i] = i == s.istem ? 1.0f : 0.0f;
    else
    {
        const float gain = std::sqrt((1.0f - sq) / sum);
        for (int i = 0; i < 3; ++i)
            if (i != s.istem) w[i] *= gain;
    }
    // (debug: SLY2N_STEMPRINT=N: every N-th update the stem chosen and the three weights, as the reference's
    // g_sndmgr 0x3DD040 +0x64 / +0x108)
    if (static const char* s_sz = std::getenv("SLY2N_STEMPRINT"); s_sz)
    {
        static int s_c = 0;
        if (s_c++ % std::max(1, std::atoi(s_sz)) == 0)
            std::printf("stem %d weights %.3f %.3f %.3f ref %d\n", s.istem, w[0], w[1], w[2], ref ? 1 : 0);
    }
    if (ref)
        for (int i = 0; i < 3; ++i) ref->SetVoiceVolume(i, w[i]);
}

// orig: 0x1FDEC8 (the dialog's events by its play clock; a stopped line ends the dialog)
static void UpdateDialogEvents(float dtFrame)
{
    SOUNDMGR& s = g_sound;
    CSndVoice* x = static_cast<CSndVoice*>(s.sndDialog.get());
    // (debug: SLY2N_FASTDIALOG ends a line after its length in frame time, for fixed-dt runs faster than real time;
    // the line's events fire by the same frame time)
    static const bool s_fFastEnv = std::getenv("SLY2N_FASTDIALOG") != nullptr;
    extern bool g_fSkipCutFast;   // (skipcut.h: a skipped cutscene runs its lines by the frame clock)
    const bool s_fFast = s_fFastEnv || g_fSkipCutFast;
    static const bool s_fState = std::getenv("SLY2N_DLGSTATE") != nullptr;  // (debug: the line's state every second)
    static int s_cState = 0;
    if (s_fState && x && s_cState++ % 60 == 0)
        std::printf("dialog %d: pause %d t %.2f / %.2f play %.2f h %d busy %d\n", x->m_pstream->id, x->m_cpause, s.tDialog, x->m_pstream->tLength,
                    static_cast<float>(x->m_tPlay), static_cast<int>(x->m_h != 0), static_cast<int>(FBusy(x)));
    if (s_fFast && x && x->m_cpause <= 0)
    {
        s.tDialog += dtFrame;
        while (s.ievent < static_cast<int>(x->m_pstream->aevent.size()))
        {
            const SNDSTREAM::EVENT& e = x->m_pstream->aevent[s.ievent];
            const float d = static_cast<float>(e.dt60) * (1.0f / 60.0f);
            if (s.tDialog < s.tEvent + d) break;
            FireDialogEvent(e);
            s.ievent++;
            s.tEvent += d;
        }
        if (x->m_pstream->tLength <= s.tDialog) StopDialog();
        return;
    }
    if (!x || FBusy(x) || x->m_cpause > 0) return;
    const SNDSTREAM* ps = x->m_pstream;
    s.tDialog += dtFrame;
    const bool fStopped = x->m_h == 0 && !FBusy(x);
    if (fStopped)
    {
        if (0.0 < x->m_tPlay || ps->tLength <= s.tDialog) StopDialog();
        return;
    }
    while (s.ievent < static_cast<int>(ps->aevent.size()))
    {
        const SNDSTREAM::EVENT& e = ps->aevent[s.ievent];
        const float d = static_cast<float>(e.dt60) * (1.0f / 60.0f);
        if (x->m_tPlay < s.tEvent + d) return;
        FireDialogEvent(e);
        s.ievent++;
        s.tEvent += d;
    }
}

bool g_fSndFrameClock = false;

// orig: 0x1FD830 (the manager's frame)
void UpdateSoundMgr(float dt, float dtFrame)
{
    std::lock_guard<std::recursive_mutex> lock(g_mtxSound);
    SOUNDMGR& s = g_sound;
    s.tClockPrev = s.tClock;
    // orig: 0x1FD830 samples 0x14D240 independently of every game-clock factor.
    // Photo overlays stop factor 8; their dialog still plays on the audio device.
    // (harness: g_fSndFrameClock (every --fixed-dt run; SLY2N_SNDFRAMECLOCK=1 too): the manager's clock runs by the frame
    // step instead of the host's, so runs faster than real time (or slowed by a loaded machine) keep the music's play
    // time (0x1FE3F8, the dances' beats) in step with the game; real-time play keeps the host clock, as the original's
    // EE counter: both advance 1001/60000 s per NTSC frame)
    static const bool s_fFrameClockEnv = std::getenv("SLY2N_SNDFRAMECLOCK") != nullptr;
    // (a skipped cutscene's fast-forward (skipcut.h) runs it by the frame too; the host's clock then continues
    // from there, offset, so it never goes back)
    extern bool g_fSkipCutFast;
    static double s_dtClockOffset = 0.0;
    if (s_fFrameClockEnv || g_fSndFrameClock || g_fSkipCutFast) s.tClock += dtFrame;
    else s.tClock = static_cast<double>(TickNow()) / 294912000.0 + s_dtClockOffset;
    if (g_fSkipCutFast && !(s_fFrameClockEnv || g_fSndFrameClock))
        s_dtClockOffset = s.tClock - static_cast<double>(TickNow()) / 294912000.0;
    LoadSoundBank();
    // (0x1FDA48 / 0x1FDBA8: the USB headset's effect and microphone; no headset here)
    CSnd* top = PvoiceTop();
    const bool fRunning = !g_sndhooks.pfnFGameRunning || g_sndhooks.pfnFGameRunning();
    if (fRunning)
    {
        if (s.fMusicPaused)
        {
            if (top) top->Resume();
            if (s.sndResident) s.sndResident->Resume();
            if (s.sndDialog) s.sndDialog->Resume();
            s.fMusicPaused = 0;
        }
    }
    else if (!s.fMusicPaused)
    {
        if (top) top->Pause();
        if (s.sndResident) s.sndResident->Pause();
        if (s.sndDialog) s.sndDialog->Pause();
        s.fMusicPaused = 1;
    }
    float g = 1.0f;
    UpdateStemWeights(dtFrame);
    UpdateDialogEvents(dtFrame);
    if (s.grfState == 3 && dt == 0.0f) g = 0.0f;
    static const SMP s_smpLevel{2.0f, 0.0f, 0.2f};  // 0x2A8288
    const float f = GSmoothSnd(s.agVol[2][0], g, dtFrame, &s_smpLevel);
    SetSoundChannelVolume(0, 2, f);
    SetSoundChannelVolume(1, 2, f);
    SetSoundChannelVolume(2, 2, g);
    // The active sounds (the list may grow while it is walked; removed entries are nulled, then compacted).
    for (std::size_t i = 0; i < s.apsndActive.size(); ++i)
        if (CSnd* p = s.apsndActive[i]) p->UpdateFrame();
    s.apsndActive.erase(std::remove(s.apsndActive.begin(), s.apsndActive.end(), nullptr), s.apsndActive.end());
}

// ---------------------------------------------------------------------------------------------------------
// Commands

namespace
{
struct WORLDBYTES
{
    std::string name;
    FK fk;
    std::vector<uint8_t> ab;
    std::size_t ibMusic = 0, ibMusicEnd = 0;  // the 0x246F58 block, as the world walk found it
};

bool FLoadMapRun(const std::string& path, uint32_t addr, std::size_t* pibStart, std::size_t* pibEnd)
{
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line))
    {
        unsigned long long a = 0, b = 0;
        char func[32] = {};
        if (std::sscanf(line.c_str(), "%llu %llu %31s", &a, &b, func) != 3) continue;
        if (std::strtoul(func, nullptr, 16) == addr)
        {
            *pibStart = static_cast<std::size_t>(a);
            if (pibEnd) *pibEnd = static_cast<std::size_t>(b);
            return true;
        }
    }
    return false;
}

// The world: a Sly 3 world index (0..39), its name (m_ext), or SECTOR:CB; the sound block's offset is where the world
// walk (sly3/brx3.cpp, orig3 0x14F440) finds it (after the prefix, the 0x58-byte groups and the progress block), and
// its end there is pmap's check (the walk's own reader of orig3 0x246F58).
bool FLoadWorldBytes(const std::string& arg, const std::string&, WORLDBYTES* pw, std::string* pmap)
{
    pmap->clear();
    if (const auto colon = arg.find(':'); colon != std::string::npos)
    {
        pw->name = arg;
        pw->fk.isector = static_cast<uint32_t>(std::strtoul(arg.c_str(), nullptr, 0));
        pw->fk.cb = static_cast<uint32_t>(std::strtoul(arg.c_str() + colon + 1, nullptr, 0));
    }
    else
    {
        char* pchEnd = nullptr;
        const unsigned long n = std::strtoul(arg.c_str(), &pchEnd, 0);
        const bool fIndex = pchEnd != arg.c_str() && !*pchEnd;
        bool fFound = false;
        for (const WORLD3& w : g_disc.Worlds())
            if (w.name == arg || (fIndex && static_cast<unsigned long>(w.iworld) == n))
            {
                pw->name = w.name;
                pw->fk = w.fk;
                fFound = true;
            }
        if (!fFound)
        {
            std::fprintf(stderr, "no world %s (--list-files)\n", arg.c_str());
            return false;
        }
    }
    if (pw->fk.cb == 0)
    {
        std::fprintf(stderr, "%s: the world has no file\n", pw->name.c_str());
        return false;
    }
    pw->ab = g_disc.ReadWorld(pw->fk);
    WORLDWALK3 walk;
    std::string error;
    if (!FWalkWorld3(pw->ab, walk, error) && walk.ibMusicEnd == 0)
    {
        std::fprintf(stderr, "%s: %s\n", pw->name.c_str(), error.c_str());
        return false;
    }
    pw->ibMusic = walk.ibMusic;
    pw->ibMusicEnd = walk.ibMusicEnd;
    return true;
}

// Loads world W's sound block (checked against the parse map when there is one).
bool FLoadWorldSound(const std::string& arg, const std::string& dirMaps, WORLDBYTES* pw)
{
    std::string pathMap;
    if (!FLoadWorldBytes(arg, dirMaps, pw, &pathMap)) return false;
    CBinaryInputStream bis;
    bis.OpenMemory(static_cast<int>(pw->ab.size()), pw->ab.data());
    bis.m_ib = pw->ibMusic;
    CLoadTrace trace(&bis);
    g_ploadtrace = &trace;
    ResetSoundMgr();
    LoadMusicFromBrx(&bis);
    g_ploadtrace = nullptr;
    const std::size_t ibEnd = bis.m_ib;
    std::printf("%s: sound block %zu..%zu, the world walk's %zu..%zu: %s\n", pw->name.c_str(), pw->ibMusic, ibEnd,
                pw->ibMusic, pw->ibMusicEnd, ibEnd == pw->ibMusicEnd ? "match" : "MISMATCH");
    return true;
}

void RunSound(double tSeconds, const std::function<bool()>& pfnDone = nullptr)
{
    using clock = std::chrono::steady_clock;
    const auto tStart = clock::now();
    auto next = tStart;
    double tReport = 0.0;
    for (;;)
    {
        const double t = std::chrono::duration<double>(clock::now() - tStart).count();
        if (t >= tSeconds) break;
        UpdateSoundMgr(1.0f / 60.0f, 1.0f / 60.0f);
        if (t >= tReport)
        {
            std::printf("  %5.1f s  %s\n", t, snd_Describe().c_str());
            std::fflush(stdout);
            tReport += 2.0;
        }
        if (pfnDone && pfnDone()) break;
        next += std::chrono::microseconds(16667);
        std::this_thread::sleep_until(next);
    }
}

std::string KeyText(const FK& fk)
{
    char ach[80];
    std::snprintf(ach, sizeof(ach), "id %5u sector 0x%06X cb 0x%08X", fk.id, fk.isector, fk.cb);
    return ach;
}
}  // namespace

int ListSoundsCommand(const std::string& world, const std::string& dirMaps)
{
    WORLDBYTES w;
    if (!FLoadWorldSound(world, dirMaps, &w)) return 1;
    SOUNDMGR& s = g_sound;
    const FK fkBank = FkFromRecord(s.keyBank);
    int cLooper = 0;
    const uint32_t hbank = fkBank.cb ? snd_BankLoadByLoc(fkBank.isector) : 0;
    for (std::size_t i = 0; i < s.aflagsSound.size(); ++i)
        if (snd_IsSoundALooper(hbank, static_cast<int>(i))) cLooper++;
    int cid = 0;
    for (int i : s.aisoundFromId) cid += i >= 0;
    std::printf("bank      %s: %d sounds (SBlk bank id %u), %d loopers; the world's table: %zu sounds, %d of %zu "
                "sound ids mapped\n",
                KeyText(fkBank).c_str(), snd_BankSoundCount(hbank), snd_BankId(hbank), cLooper, s.aflagsSound.size(), cid,
                s.aisoundFromId.size());
    snd_UnloadBank(hbank);
    if (s.president)
        std::printf("resident  %s: %s\n", KeyText(s.president->fk).c_str(), SpiopDescribeFile(s.president->fk.isector).c_str());
    std::printf("targets  ");
    for (int id : s.aidTarget) std::printf(" %d", id);
    std::printf("\nlanguages");
    for (int l : s.alanguage) std::printf(" %d", l);
    std::printf(" (using %d)\n", s.ilanguage);
    for (const SNDSTREAM& m : s.amusic)
        std::printf("music %4d %s: %s\n", m.id, KeyText(m.fk).c_str(),
                    m.fk.cb ? SpiopDescribeFile(m.fk.isector).c_str() : "(no file)");
    for (const SNDSTREAM& d : s.adialog)
        std::printf("dialog %5d speaker %4d %s %5.2f s %3zu events%s\n", d.id, d.oidSpeaker, KeyText(d.fk).c_str(),
                    d.tLength, d.aevent.size(), d.fNoHeadset ? " (no headset)" : "");
    std::printf("%zu music tracks, %zu dialog lines\n", s.amusic.size(), s.adialog.size());
    return 0;
}

int PlayMusicCommand(const std::string& world, const std::string& dirMaps, int idMusic, double tSeconds, int istem)
{
    StartupSound();
    WORLDBYTES w;
    if (!FLoadWorldSound(world, dirMaps, &w)) return 1;
    LoadSoundBank();
    if (idMusic < 0)
        for (const SNDSTREAM& m : g_sound.amusic)
            if (m.fk.cb)
            {
                idMusic = m.id;
                break;
            }
    static int s_istem = 1;
    s_istem = istem;
    g_sndhooks.pfnMusicStem = [] { return s_istem; };
    std::printf("resident %s, music %d, stem %d\n", g_sound.president ? KeyText(g_sound.president->fk).c_str() : "none",
                idMusic, istem);
    if (idMusic >= 0 && !PsndPushMusic(idMusic)) std::printf("no music %d\n", idMusic);
    RunSound(tSeconds);
    ShutdownSound();
    return 0;
}

int PlaySfxCommand(const std::string& world, const std::string& dirMaps, int sfxid, double tSeconds)
{
    StartupSound();
    WORLDBYTES w;
    if (!FLoadWorldSound(world, dirMaps, &w)) return 1;
    LoadSoundBank();
    if (g_sound.sndResident) g_sound.sndResident->Stop(0.0f);  // (only the effects / the line)
    g_sound.sndResident = CSndRef();
    std::vector<int> aid;
    if (sfxid >= 0) aid.push_back(sfxid);
    else
        for (std::size_t i = 0; i < g_sound.aisoundFromId.size(); ++i)
            if (g_sound.aisoundFromId[i] >= 0 && g_sound.aflagsSound[g_sound.aisoundFromId[i]] != 2)
                aid.push_back(static_cast<int>(i));
    for (int id : aid)
    {
        const int isound = IsoundFromSfxid(id);
        std::printf("sound id %d: bank sound %d%s\n", id, isound,
                    isound >= 0 && g_sound.aflagsSound[isound] == 2 ? " (looper)" : "");
        CSndRef r(PsndStartSound(id));
        RunSound(sfxid >= 0 ? tSeconds : std::min(tSeconds, 3.0), [] { return snd_CSoundsActive() == 0; });
        if (r) r->Stop(0.0f);
        RunSound(0.3);
    }
    ShutdownSound();
    return 0;
}

// The world's dialog lines' lengths (id -> seconds), from its sound block (no playback; Sly 3: scptrun3.cpp's test
// host times the dialog waits with them).
bool FDialogLengthsOfWorld(const std::string& world, std::map<int, float>* pmp)
{
    WORLDBYTES w;
    if (!FLoadWorldSound(world, "", &w)) return false;
    for (const SNDSTREAM& st : g_sound.adialog) (*pmp)[st.id] = st.tLength;
    ResetSoundMgr();
    return true;
}

int PlayDialogCommand(const std::string& world, const std::string& dirMaps, int idDialog, double tSeconds)
{
    StartupSound();
    WORLDBYTES w;
    if (!FLoadWorldSound(world, dirMaps, &w)) return 1;
    if (g_sound.sndResident) g_sound.sndResident->Stop(0.0f);  // (only the effects / the line)
    g_sound.sndResident = CSndRef();
    if (idDialog < 0 && !g_sound.adialog.empty()) idDialog = g_sound.adialog.front().id;
    g_sndhooks.pfnDialogEvent = [](int oid, int k, int32_t v) {
        std::printf("  event: speaker %d target %d value %d (t %.2f)\n", oid, k, v, TDialog());
    };
    if (!PsndStartDialog(idDialog)) std::printf("no dialog %d\n", idDialog);
    RunSound(tSeconds, [] { return !g_sound.sndDialog; });
    ShutdownSound();
    return 0;
}
