// The reference's controller waypoint programs (the reference's route format)
// replayed natively (debug, SLY2N_ROUTE=file@tT or file@F: the program's epoch at game time T or frame F): the same
// file drives both games' pads, so their positions can be compared. Directives: after-warp N, move X Y NEAR BUDGET
// ZMIN ZMAX run|walk [strength=G], wait N, input N CSV, aim N X Y run|walk CSV [strength=G]. The stick toward a world
// point through the camera's view direction (CM +0x60's x row, the reference's 0x2DDFA0), run = R1 beyond 170.
// Feedback on another object (a guard's patrol pauses at random): waitobj CID OID X Y R N (neutral pad until an in-world
// object of that class / oid is within R of X,Y; fails after N frames), chase CID OID NEAR BUDGET ZMIN ZMAX run|walk
// [strength=G] [jump=N] [within=R] [follow] (move toward the nearest such object until within NEAR; jump=N: a double jump
// every N frames; within=R: only objects within R of the player, none: the step is over; follow: stay within NEAR of it,
// neutral while there, for the whole budget N frames, which then ends the step instead of failing it).
// point N X Y Z CSV [obj=CID:OID:DZ]: N frames of the left stick turning the camera (e.g. the binocucom's view) toward the
// world point (or the nearest in-world object CID / OID to X,Y, DZ above it; lo=OID[:DZ]: that object's world position), the CSV's presses on top (RY zoom, R1).
// setpos X Y Z: the current player placed there by its position setter (vt slot 29), velocities zeroed (both engines:
// the reference's route setpos = SLY2_SETPOS on 3DD490+3EFC@); one frame.
// teleport X Y Z [H]: the current player put there (as SLY2N_TELEPORT: Z is the object's, H the heading, camera cut) at the
// step's start (one frame); waittask OID STATE N [CSV]: until the task's state is >= STATE (after N frames it goes on), the CSV's
// presses meanwhile (e.g. CROSS taps through a dialog); taskreq OID STATE: a poke (as SLY2N_TASKREQ); intovol OID [DZ]: the player at a volume's center (its floor + DZ); tail CID OID DIST N [DZ] [CSV]: N frames (the CSV's presses every 60 frames), the player put DIST/2 behind the nearest such object whenever farther than DIST; onlo OID [DZ] [N]: the player onto the N-th in-world copy of OID, DZ above, with its velocity (a moving train); giver OID [D]: as SLY2N_TELEGIVER (-1: the first guard with a prize); waitclock DT N: until the game clock ran DT (popups pause it); all independent of the game clock (pauses, scenes).
#include "activate.h"
#include "crv.h"
#include "actor.h"
#include "alo.h"
#include "damage.h"
#include "guard.h"
#include "clock.h"
#include "cls.h"
#include "cm.h"
#include "joy.h"
#include "lo.h"
#include "po.h"
#include "so.h"
#include "sw.h"
#include "task.h"
#include "routetele.h"
#include "vtnative.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <string>
#include <vector>

ROUTETELE g_routetele;

namespace
{
struct PRESSR { int at = 1, c = 1, axis = -1; uint16_t grf = 0; uint8_t b = 0x80; };
struct STEPR
{
    enum { Move, Wait, Input, Aim, WaitObj, Chase, Point, Teleport, WaitTask, TaskReq, WaitClock, Giver, IntoVol, SetPos, OnLo, Tail } kind = Wait;
    float h = 0; bool fH = false;
    int cid = 0, oid = 0;
    int frames = 0;
    float x = 0, y = 0, z = 0, dz = 0, near = 0, zmin = -1e30f, zmax = 1e30f, strength = 1.0f;
    float within = 0.0f;  // (chase within=R: targets beyond R of the player ignored)
    int jump = 0;  // (chase jump=N: Cross at the step's frames N*k+1..+6 and +13..+18, as the reference's)
    bool follow = false;  // (chase follow: within NEAR the pad is neutral and the step goes on until its budget)
    bool run = false;
    std::vector<PRESSR> apress;
};
struct ROUTE
{
    bool fLoaded = false, fStarted = false, fDone = false, fFailed = false, fAdvance = false;
    int after = 0, istep = 0, frameStart = -1, frameStep = 0, frameEpoch = -1;
    float tEpoch = -1.0f;
    std::vector<STEPR> astep;
    std::FILE* pfTrace = nullptr;
};
ROUTE& Route()
{
    static ROUTE s_r;
    return s_r;
}

bool FParsePresses(const std::string& csv, std::vector<PRESSR>& a)
{
    if (csv == "-") return true;
    std::vector<JOYPRESS> ap;
    if (!FParseJoyPresses(csv, ap)) return false;
    for (const JOYPRESS& p : ap) a.push_back({p.frame, p.c, p.axis, p.grfbtn, p.bAxis});
    return true;
}

void Load()
{
    ROUTE& r = Route();
    r.fLoaded = true;
    const char* sz = std::getenv("SLY2N_ROUTE");
    if (!sz) return;
    std::string spec(sz), path = spec;
    if (size_t at = spec.rfind('@'); at != std::string::npos)
    {
        path = spec.substr(0, at);
        const std::string e = spec.substr(at + 1);
        if (!e.empty() && e[0] == 't') r.tEpoch = std::strtof(e.c_str() + 1, nullptr);
        else r.frameEpoch = std::atoi(e.c_str());
    }
    std::ifstream in(path);
    std::string line;
    bool fHeader = false;
    while (std::getline(in, line))
    {
        if (size_t h = line.find('#'); h != std::string::npos) line.resize(h);
        std::istringstream ss(line);
        std::vector<std::string> f;
        for (std::string w; ss >> w;) f.push_back(w);
        if (f.empty()) continue;
        if (!fHeader)
        {
            fHeader = f[0] == "after-warp" && f.size() == 2;
            r.after = fHeader ? std::atoi(f[1].c_str()) : 0;
            continue;
        }
        STEPR s;
        auto strength = [&](size_t i) { if (f.size() > i && f[i].rfind("strength=", 0) == 0) s.strength = std::strtof(f[i].c_str() + 9, nullptr); };
        if (f[0] == "move" && f.size() >= 8)
        {
            s.kind = STEPR::Move;
            s.x = std::strtof(f[1].c_str(), nullptr); s.y = std::strtof(f[2].c_str(), nullptr);
            s.near = std::strtof(f[3].c_str(), nullptr); s.frames = std::atoi(f[4].c_str());
            s.zmin = std::strtof(f[5].c_str(), nullptr); s.zmax = std::strtof(f[6].c_str(), nullptr);
            s.run = f[7] == "run";
            strength(8);
        }
        else if (f[0] == "waitobj" && f.size() >= 7)
        {
            s.kind = STEPR::WaitObj;
            s.cid = static_cast<int>(std::strtol(f[1].c_str(), nullptr, 0)); s.oid = std::atoi(f[2].c_str());
            s.x = std::strtof(f[3].c_str(), nullptr); s.y = std::strtof(f[4].c_str(), nullptr);
            s.near = std::strtof(f[5].c_str(), nullptr); s.frames = std::atoi(f[6].c_str());
        }
        else if (f[0] == "chase" && f.size() >= 8)
        {
            s.kind = STEPR::Chase;
            s.cid = static_cast<int>(std::strtol(f[1].c_str(), nullptr, 0)); s.oid = std::atoi(f[2].c_str());
            s.near = std::strtof(f[3].c_str(), nullptr); s.frames = std::atoi(f[4].c_str());
            s.zmin = std::strtof(f[5].c_str(), nullptr); s.zmax = std::strtof(f[6].c_str(), nullptr);
            s.run = f[7] == "run";
            strength(8);
            for (size_t i = 8; i < f.size(); ++i)
            {
                if (f[i].rfind("jump=", 0) == 0) s.jump = std::atoi(f[i].c_str() + 5);
                else if (f[i].rfind("within=", 0) == 0) s.within = std::strtof(f[i].c_str() + 7, nullptr);
                else if (f[i] == "follow") s.follow = true;
                else strength(i);
            }
        }
        else if (f[0] == "wait" && f.size() >= 2) { s.kind = STEPR::Wait; s.frames = std::atoi(f[1].c_str()); }
        else if (f[0] == "input" && f.size() >= 3) { s.kind = STEPR::Input; s.frames = std::atoi(f[1].c_str()); FParsePresses(f[2], s.apress); }
        else if (f[0] == "aim" && f.size() >= 6)
        {
            s.kind = STEPR::Aim;
            s.frames = std::atoi(f[1].c_str());
            s.x = std::strtof(f[2].c_str(), nullptr); s.y = std::strtof(f[3].c_str(), nullptr);
            s.run = f[4] == "run";
            FParsePresses(f[5], s.apress);
            strength(6);
        }
        else if (f[0] == "point" && f.size() >= 6)
        {
            s.kind = STEPR::Point;
            s.frames = std::atoi(f[1].c_str());
            s.x = std::strtof(f[2].c_str(), nullptr); s.y = std::strtof(f[3].c_str(), nullptr); s.z = std::strtof(f[4].c_str(), nullptr);
            FParsePresses(f[5], s.apress);
            s.cid = -1;
            if (f.size() >= 7 && f[6].rfind("obj=", 0) == 0)
                std::sscanf(f[6].c_str() + 4, "%i:%d:%f", &s.cid, &s.oid, &s.dz);
            else if (f.size() >= 7 && f[6].rfind("lo=", 0) == 0)  // (any object's world position, e.g. a photo target)
            {
                s.cid = -2;
                std::sscanf(f[6].c_str() + 3, "%d:%f", &s.oid, &s.dz);
            }
        }
        else if (f[0] == "teleport" && f.size() >= 4)
        {
            s.kind = STEPR::Teleport; s.frames = 1;
            s.x = std::strtof(f[1].c_str(), nullptr); s.y = std::strtof(f[2].c_str(), nullptr); s.z = std::strtof(f[3].c_str(), nullptr);
            if (f.size() >= 5) { s.fH = true; s.h = std::strtof(f[4].c_str(), nullptr); }
        }
        else if (f[0] == "setpos" && f.size() == 4)
        {
            s.kind = STEPR::SetPos; s.frames = 1;
            s.x = std::strtof(f[1].c_str(), nullptr); s.y = std::strtof(f[2].c_str(), nullptr); s.z = std::strtof(f[3].c_str(), nullptr);
        }
        else if (f[0] == "intovol" && f.size() >= 2)
        {
            s.kind = STEPR::IntoVol; s.frames = 1;
            s.oid = std::atoi(f[1].c_str()); s.dz = f.size() >= 3 ? std::strtof(f[2].c_str(), nullptr) : 0.0f;
        }
        else if (f[0] == "tail" && f.size() >= 5)
        {
            // tail CID OID DIST N [DZ]: for N frames, whenever the player is farther than DIST (2D) from the nearest such
            // object, he is put DIST/2 behind it (along its motion), DZ above its position (a follow job by teleports)
            s.kind = STEPR::Tail;
            s.cid = static_cast<int>(std::strtol(f[1].c_str(), nullptr, 0)); s.oid = std::atoi(f[2].c_str());
            s.near = std::strtof(f[3].c_str(), nullptr); s.frames = std::atoi(f[4].c_str());
            s.dz = f.size() >= 6 ? std::strtof(f[5].c_str(), nullptr) : 50.0f;
            if (f.size() >= 7) FParsePresses(f[6], s.apress);  // (repeated every 60 frames, e.g. CIRCLE for a pickpocket)
        }
        else if (f[0] == "onlo" && f.size() >= 2)
        {
            // tail CID OID DIST N [DZ] [CSV]: N frames (the CSV's presses every 60 frames), the player put DIST/2 behind the nearest such object whenever farther than DIST; onlo OID [DZ] [N]: the player onto the N-th in-world copy of OID (DZ above its position), with its velocity
            s.kind = STEPR::OnLo; s.frames = 2;
            s.oid = std::atoi(f[1].c_str()); s.dz = f.size() >= 3 ? std::strtof(f[2].c_str(), nullptr) : 0.0f;
            s.cid = f.size() >= 4 ? std::atoi(f[3].c_str()) : 0;
        }
        else if (f[0] == "giver" && f.size() >= 2)
        {
            s.kind = STEPR::Giver; s.frames = 1;
            s.oid = std::atoi(f[1].c_str()); s.dz = f.size() >= 3 ? std::strtof(f[2].c_str(), nullptr) : 150.0f;
        }
        else if (f[0] == "waitclock" && f.size() >= 3)
        {
            s.kind = STEPR::WaitClock;
            s.x = std::strtof(f[1].c_str(), nullptr); s.frames = std::atoi(f[2].c_str());
        }
        else if (f[0] == "taskreq" && f.size() >= 3)
        {
            s.kind = STEPR::TaskReq; s.frames = 1;
            s.oid = std::atoi(f[1].c_str()); s.cid = std::atoi(f[2].c_str());
        }
        else if (f[0] == "waittask" && f.size() >= 4)
        {
            s.kind = STEPR::WaitTask;
            s.oid = std::atoi(f[1].c_str()); s.cid = std::atoi(f[2].c_str()); s.frames = std::atoi(f[3].c_str());
            if (f.size() >= 5) FParsePresses(f[4], s.apress);
        }
        else { std::fprintf(stderr, "route: bad line: %s\n", line.c_str()); continue; }
        r.astep.push_back(s);
    }
    std::printf("route: %s, %zu steps, epoch %s%g\n", path.c_str(), r.astep.size(), r.tEpoch >= 0 ? "t " : "frame ",
                r.tEpoch >= 0 ? r.tEpoch : float(r.frameEpoch));
    if (const char* szT = std::getenv("SLY2N_ROUTETRACE"))
        if ((r.pfTrace = std::fopen(szT, "w"))) std::fprintf(r.pfTrace, "frame,t,step,buttons,lx,ly,x,y,z,pan\n");
}

// the reference's stick_axis (Python round: nearest, ties to even)
uint8_t BStick(double dir, double strength)
{
    const double v = strength == 1 ? 128 - 127 * dir : 128 - 127 * (dir * strength);
    const double lo = std::floor(v), fr = v - lo;
    long n = static_cast<long>(lo);
    if (fr > 0.5 || (fr == 0.5 && (n & 1))) ++n;
    return static_cast<uint8_t>(std::clamp(n, 0L, 255L));
}

// the in-world object of class cid / oid nearest (x, y) (2D), as the reference walks SW +0x3038's class list
// (chase, pzr given (the step's ZMIN / ZMAX): only targets in that height window, and a guard (class 0x38..0x43) whose
// health (+0x33C) is gone is skipped)
bool FNearestObj(int cid, int oid, float x, float y, glm::vec3* ppos, const float* pzr = nullptr)
{
    float dBest = 1e30f;
    if (!g_psw) return false;
    for (const auto& up : g_psw->aploAll)
    {
        LO* plo = up.get();
        static const bool s_fDbg = std::getenv("SLY2N_CHASEDBG") != nullptr;  // (debug: why a chase target is skipped)
        if (s_fDbg && plo->oid == oid)
            std::printf("chase: oid %d cid 0x%X (want 0x%X) in world %d hp %d z %.0f window %.0f..%.0f\n", oid, plo->Cid(), cid,
                        plo->fInWorld, PguardFromLo(plo) ? PguardFromLo(plo)->n33C : -999,
                        FCidDerivedFrom(plo->Cid(), 0x30) ? PosAloCenter(static_cast<ALO*>(plo)).z : 0.0f, pzr ? pzr[0] : 0.0f, pzr ? pzr[1] : 0.0f);
        if (plo->oid != oid || !plo->fInWorld || plo->Cid() != cid || !FCidDerivedFrom(plo->Cid(), 0x30)) continue;
        if (pzr && cid >= 0x38 && cid < 0x44)
            if (const GUARD* pg = PguardFromLo(plo); pg && pg->n33C <= 0) continue;
        const glm::vec3 p = PosAloCenter(static_cast<ALO*>(plo));
        if (pzr && (p.z < pzr[0] || p.z > pzr[1])) continue;
        const float d = std::hypot(p.x - x, p.y - y);
        if (d < dBest) { dBest = d; *ppos = p; }
    }
    return dBest < 1e30f;
}
}  // namespace

// (joy.cpp's UpdateJoy: the program's pad sample of this frame, replacing the scripted presses once it runs)
bool FRouteSample(int frame, uint16_t* pgrf, uint8_t abAxis[4])
{
    ROUTE& r = Route();
    if (!r.fLoaded) Load();
    static const bool s_fHp = std::getenv("SLY2N_HPTRACE") != nullptr;  // (debug: the player's health changes)
    if (s_fHp)
    {
        static int s_hp = -1000;
        LO* ploH = g_psw ? PloPoCurrent(g_psw) : nullptr;
        ACTOR* pa = ploH ? PactorFromLo(ploH) : nullptr;
        const int* pn = pa ? DmgCounterOf(pa) : nullptr;
        if (pn && *pn != s_hp)
        {
            const glm::vec3 p = FCidDerivedFrom(ploH->Cid(), 0x30) ? PosAloCenter(static_cast<ALO*>(ploH)) : glm::vec3(0.0f);
            std::printf("hp: frame %d t %.3f %d -> %d at (%.0f %.0f %.0f)\n", frame, g_clock.t, s_hp, *pn, p.x, p.y, p.z);
            s_hp = *pn;
        }
    }
    // (debug: SLY2N_GUARDHP=oid[,oid..]: those guards' health +0x33C changes, e.g. the Ep3 boss Rajan 95460)
    if (static const char* s_szGhp = std::getenv("SLY2N_GUARDHP"); s_szGhp && g_psw)
    {
        static std::unordered_map<int, int> s_mphp;
        for (const char* q = s_szGhp; q && *q; q = std::strchr(q, ',') ? std::strchr(q, ',') + 1 : nullptr)
        {
            const int oid = std::atoi(q);
            LO* plo = PloFindSwObjectByOid(g_psw, oid);
            const GUARD* pg = plo ? PguardFromLo(plo) : nullptr;
            if (!pg) continue;
            auto it = s_mphp.find(oid);
            if (it == s_mphp.end() || it->second != pg->n33C)
            {
                std::printf("guardhp: frame %d t %.3f %d %d -> %d\n", frame, g_clock.t, oid, it == s_mphp.end() ? -1000 : it->second, pg->n33C);
                s_mphp[oid] = pg->n33C;
            }
        }
    }
    // (test only: SLY2N_GUARDHPMAX=N: every guard's health +0x33C held at most N, a route's fights shortened)
    if (static const char* s_szMax = std::getenv("SLY2N_GUARDHPMAX"); s_szMax && g_psw)
        for (const auto& up : g_psw->aploAll)
            if (GUARD* pg = FCidDerivedFrom(up->Cid(), 0x38) ? PguardFromLo(up.get()) : nullptr)
                pg->n33C = std::min(pg->n33C, std::atoi(s_szMax));
    if (r.astep.empty() || r.fDone || r.fFailed) return false;
    if (r.frameStart < 0)
    {
        const bool fEpoch = r.tEpoch >= 0 ? g_clock.t >= r.tEpoch : frame >= r.frameEpoch;
        if (!fEpoch) return false;
        r.frameStart = frame + r.after;
        std::printf("route: epoch at frame %d (t %.3f), start at %d\n", frame, g_clock.t, r.frameStart);
    }
    if (frame < r.frameStart) return false;
    uint16_t grf = 0;
    uint8_t a[4] = {0x80, 0x80, 0x80, 0x80};
    if (!r.fStarted) { r.fStarted = true; r.frameStep = frame; std::printf("route: step 1 at frame %d (t %.3f)\n", frame, g_clock.t); }
    const STEPR* s = &r.astep[r.istep];
    const auto fOpen = [](const STEPR* p) { return p->kind == STEPR::Move || p->kind == STEPR::Chase || p->kind == STEPR::WaitObj || p->kind == STEPR::WaitTask || p->kind == STEPR::WaitClock; };
    if (r.fAdvance || (!fOpen(s) && frame - r.frameStep >= s->frames))
    {
        ++r.istep;
        r.fAdvance = false;
        r.frameStep = frame;
        if (r.istep == static_cast<int>(r.astep.size()))
        {
            r.fDone = true;
            std::printf("route: completed at frame %d (t %.3f)\n", frame, g_clock.t);
            return false;
        }
        s = &r.astep[r.istep];
        std::printf("route: step %d at frame %d (t %.3f)\n", r.istep + 1, frame, g_clock.t);
    }
    const int rel = frame - r.frameStep + 1;
    LO* plo = g_psw ? PloPoCurrent(g_psw) : nullptr;
    glm::vec3 pos(0.0f);
    if (plo && FCidDerivedFrom(plo->Cid(), 0x30)) pos = PosAloCenter(static_cast<ALO*>(plo));
    const float camx = g_cm.mat[0].x, camy = g_cm.mat[0].y;
    if (s->kind == STEPR::WaitObj)
    {
        glm::vec3 po;
        if (FNearestObj(s->cid, s->oid, s->x, s->y, &po) && std::hypot(po.x - s->x, po.y - s->y) <= s->near)
        {
            std::printf("route: step %d's object at %.0f %.0f %.0f (frame %d)\n", r.istep + 1, po.x, po.y, po.z, frame);
            if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; std::printf("route: completed at frame %d\n", frame); }
            else r.fAdvance = true;
        }
        else if (frame - r.frameStep >= s->frames)
        {
            std::printf("route: failed at frame %d: step %d's object never came\n", frame, r.istep + 1);
            r.fFailed = true;
            return false;
        }
    }
    if (s->kind == STEPR::Teleport && rel == 1)
    {
        g_routetele = {true, s->x, s->y, s->z, s->h, s->fH};
        std::printf("route: step %d teleport %.0f %.0f %.0f (frame %d)\n", r.istep + 1, s->x, s->y, s->z, frame);
    }
    if (s->kind == STEPR::SetPos && rel == 1 && plo && FCidDerivedFrom(plo->Cid(), 0x30))
    {
        // as SLY2N_SETPOS / the reference's route setpos (SLY2_SETPOS): the class's slot 29, velocities zeroed
        ALO* palo = static_cast<ALO*>(plo);
        if (auto pfn = reinterpret_cast<void (*)(ALO*, const glm::vec3&)>(PvNativeVt(plo, 29))) pfn(palo, glm::vec3(s->x, s->y, s->z));
        else SetAloPos(palo, glm::vec3(s->x, s->y, s->z));
        SetAloVelocityTo(palo, glm::vec3(0.0f));
        SetAloAngVelocityTo(palo, glm::vec3(0.0f));
        std::printf("route: step %d setpos %.1f %.1f %.1f (frame %d)\n", r.istep + 1, s->x, s->y, s->z, frame);
    }
    if (s->kind == STEPR::WaitClock)
    {
        static float s_t0 = 0.0f;
        if (rel == 1) s_t0 = g_clock.t;
        if (g_clock.t - s_t0 >= s->x || frame - r.frameStep >= s->frames)
        {
            std::printf("route: step %d's clock ran %.3f (frame %d)\n", r.istep + 1, g_clock.t - s_t0, frame);
            if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; return false; }
            r.fAdvance = true;
        }
    }
    if (s->kind == STEPR::IntoVol && rel == 1 && g_psw)
    {
        // (the center of a volume's shape (a trigger or an exit) in the world, DZ added: the player teleported there)
        if (const LOXSHAPE* px = dynamic_cast<const LOXSHAPE*>(PloFindSwObjectByOid(g_psw, s->oid)); px && !px->geom.apos.empty())
        {
            const glm::mat4 m = MatWorldLox(px);
            glm::vec3 mn(1e30f), mx(-1e30f);
            for (const glm::vec3& v : px->geom.apos) { const glm::vec3 w = glm::vec3(m * glm::vec4(v, 1.0f)); mn = glm::min(mn, w); mx = glm::max(mx, w); }
            const glm::vec3 c = (mn + mx) * 0.5f;
            g_routetele = {true, c.x, c.y, mn.z + s->dz, 0.0f, false};
            std::printf("route: step %d into volume %d at %.0f %.0f %.0f (box %.0f..%.0f %.0f..%.0f %.0f..%.0f)\n", r.istep + 1, s->oid, c.x, c.y,
                        mn.z + s->dz, mn.x, mx.x, mn.y, mx.y, mn.z, mx.z);
        }
        else std::printf("route: step %d: no volume %d\n", r.istep + 1, s->oid);
    }
    if (s->kind == STEPR::Tail && g_psw)
    {
        static glm::vec3 s_posPrev(0.0f);
        glm::vec3 po;
        if (FNearestObj(s->cid, s->oid, pos.x, pos.y, &po))
        {
            if (rel == 1) s_posPrev = po;
            glm::vec3 d = po - s_posPrev;
            d.z = 0.0f;
            if (rel > 1 && std::hypot(po.x - pos.x, po.y - pos.y) > s->near)
            {
                const glm::vec3 back = glm::length(d) > 1e-3f ? -glm::normalize(d) : glm::vec3(0.0f);
                const glm::vec3 q = po + back * (s->near * 0.5f) + glm::vec3(0.0f, 0.0f, s->dz);
                g_routetele = {true, q.x, q.y, q.z, 0.0f, false};
                std::printf("route: step %d tail %d: player put at %.0f %.0f %.0f (frame %d)\n", r.istep + 1, s->oid, q.x, q.y, q.z, frame);
            }
            if (rel % 15 == 0) s_posPrev = po;
        }
    }
    if (s->kind == STEPR::OnLo && g_psw)
    {
        // (frame 1: the copy's position; frame 2: the player put there, DZ above, moving with it)
        static glm::vec3 s_pos0(0.0f);
        int i = 0;
        for (const auto& up : g_psw->aploAll)
        {
            LO* pl = up.get();
            if (pl->oid != s->oid || !pl->fInWorld || !FCidDerivedFrom(pl->Cid(), 0x30) || i++ != s->cid) continue;
            const glm::vec3 p = PosAloCenter(static_cast<ALO*>(pl));
            if (rel == 1) s_pos0 = p;
            else if (rel == 2)
            {
                const glm::vec3 v = g_clock.dt > 0.0f ? (p - s_pos0) / g_clock.dt : glm::vec3(0.0f);
                g_routetele = {true, p.x + v.x * g_clock.dt, p.y + v.y * g_clock.dt, p.z + v.z * g_clock.dt + s->dz, 0.0f, false};
                g_routetele.fVel = true; g_routetele.vel = v;
                std::printf("route: step %d onto %d#%d at %.0f %.0f %.0f vel %.0f %.0f %.0f (frame %d)\n", r.istep + 1, s->oid, s->cid, p.x, p.y,
                            p.z + s->dz, v.x, v.y, v.z, frame);
            }
            break;
        }
    }
    if (s->kind == STEPR::Giver && rel == 1)
    {
        g_routetele.fGiver = true; g_routetele.oidGiver = s->oid; g_routetele.dGiver = s->dz;
        std::printf("route: step %d giver %d (frame %d)\n", r.istep + 1, s->oid, frame);
    }
    if (s->kind == STEPR::TaskReq && rel == 1)
    {
        bool FRequestTaskStateOid(OID oid, int n);  // (task.cpp: as SLY2N_TASKREQ)
        std::printf("route: step %d taskreq %d = %d (frame %d)\n", r.istep + 1, s->oid, s->cid, frame);
        std::printf("route: taskreq %d: %d\n", s->oid, FRequestTaskStateOid(s->oid, s->cid) ? 1 : 0);
    }
    if (s->kind == STEPR::WaitTask)
    {
        const TASK* pt = g_psw ? dynamic_cast<const TASK*>(PloFindSwObjectByOid(g_psw, s->oid)) : nullptr;
        if (pt && pt->nState >= s->cid)
        {
            std::printf("route: step %d's task %d state %d (frame %d t %.3f)\n", r.istep + 1, s->oid, pt->nState, frame, g_clock.t);
            if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; std::printf("route: completed at frame %d\n", frame); }
            else r.fAdvance = true;
        }
        else if (frame - r.frameStep >= s->frames)
        {
            // (a timeout goes on: the next steps may retry, e.g. a moving photo target from another place)
            std::printf("route: timeout at frame %d: step %d's task %d at state %d\n", frame, r.istep + 1, s->oid, pt ? pt->nState : -9);
            if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; return false; }
            r.fAdvance = true;
        }
    }
    STEPR sChase;
    if (s->kind == STEPR::Chase)
    {
        glm::vec3 po;
        const float azr[2] = {s->zmin, s->zmax};
        if (!FNearestObj(s->cid, s->oid, pos.x, pos.y, &po, azr) || (s->within > 0.0f && std::hypot(po.x - pos.x, po.y - pos.y) > s->within))
        {
            // (none left in the height window: the chase is over, e.g. every guard of a fight knocked out)
            std::printf("route: step %d's object is gone (frame %d)\n", r.istep + 1, frame);
            if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; std::printf("route: completed at frame %d\n", frame); }
            else r.fAdvance = true;
            return false;
        }
        if (s->follow)
        {
            // (follow: the budget's end is the step's end; within NEAR the pad stays neutral)
            if (frame - r.frameStep >= s->frames)
            {
                if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; std::printf("route: completed at frame %d\n", frame); }
                else r.fAdvance = true;
                return false;
            }
            if (std::hypot(po.x - pos.x, po.y - pos.y) <= s->near) return false;
        }
        sChase = *s;
        sChase.kind = STEPR::Move;
        sChase.x = po.x; sChase.y = po.y;
        s = &sChase;
    }
    if (s->kind == STEPR::Move || s->kind == STEPR::Aim)
    {
        if (s->kind == STEPR::Move && (pos.z < s->zmin || pos.z > s->zmax))
        {
            std::printf("route: failed at frame %d: step %d's height %.0f outside %.0f..%.0f\n", frame, r.istep + 1, pos.z, s->zmin, s->zmax);
            r.fFailed = true;
            return false;
        }
        if (s->kind == STEPR::Move && frame - r.frameStep > s->frames)
        {
            std::printf("route: failed at frame %d: step %d's budget (at %.0f %.0f %.0f)\n", frame, r.istep + 1, pos.x, pos.y, pos.z);
            r.fFailed = true;
            return false;
        }
        const double dx = s->x - pos.x, dy = s->y - pos.y, d = std::hypot(dx, dy);
        if (s->kind == STEPR::Move && d <= s->near)
        {
            if (r.istep + 1 == static_cast<int>(r.astep.size())) { r.fDone = true; std::printf("route: completed at frame %d\n", frame); }
            else r.fAdvance = true;
        }
        else if (d > 0)
        {
            const double ang = std::atan2(dy, dx) - std::atan2(camy, camx);
            a[0] = BStick(std::sin(ang), s->strength);
            a[1] = BStick(std::cos(ang), s->strength);
            if (s->run && d > 170) grf |= BTN_R1;
            if (s->jump > 0) { const int k = (rel - 1) % s->jump; if (k < 6 || (k >= 12 && k < 18)) grf |= BTN_Cross; }
        }
    }
    if (s->kind == STEPR::Point)
    {
        glm::vec3 tgt(s->x, s->y, s->z);
        if (s->cid >= 0)
        {
            glm::vec3 po;
            if (FNearestObj(s->cid, s->oid, s->x, s->y, &po)) tgt = po + glm::vec3(0.0f, 0.0f, s->dz);
        }
        else if (s->cid == -2 && g_psw)
            if (LO* plo = PloFindSwObjectByOid(g_psw, s->oid)) tgt = PosLoWorld(plo) + glm::vec3(0.0f, 0.0f, s->dz);
        const glm::vec3 d = tgt - g_cm.pos, fw = g_cm.mat[0];
        double eYaw = std::atan2(d.y, d.x) - std::atan2(fw.y, fw.x);
        while (eYaw > M_PI) eYaw -= 2 * M_PI;
        while (eYaw < -M_PI) eYaw += 2 * M_PI;
        const double ePitch = std::atan2(d.z, std::hypot(d.x, d.y)) - std::atan2(fw.z, std::hypot(fw.x, fw.y));
        const auto dir = [](double e) { return std::fabs(e) < 0.003 ? 0.0 : std::copysign(std::clamp(std::fabs(e) * 60.0, 0.5, 1.0), e); };
        a[0] = BStick(dir(eYaw), 1.0);
        a[1] = BStick(-dir(ePitch), 1.0);
        static const bool s_fPrint = std::getenv("SLY2N_POINTPRINT") != nullptr;
        if (s_fPrint) std::printf("point frame %d yaw %.5f pitch %.5f stick %u %u target %.0f %.0f %.0f\n", frame, eYaw, ePitch, a[0], a[1], tgt.x, tgt.y, tgt.z);
    }
    const int relp = s->kind == STEPR::Tail ? (rel - 1) % 60 + 1 : rel;
    for (const PRESSR& p : s->apress)
        if (relp >= p.at && relp - p.at < p.c)
        {
            if (p.axis < 0) grf |= p.grf;
            else a[p.axis] = p.b;
        }
    *pgrf = grf;
    for (int i = 0; i < 4; ++i) abAxis[i] = a[i];
    if (r.pfTrace)
        std::fprintf(r.pfTrace, "%d,%.4f,%d,%u,%u,%u,%.2f,%.2f,%.2f,%.5f\n", frame, g_clock.t, r.istep + 1, grf, a[0], a[1], pos.x, pos.y, pos.z,
                     std::atan2(camy, camx));
    return true;
}
