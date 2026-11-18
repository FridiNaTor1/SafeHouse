// See modguests.h.
#include "modguests.h"

#include "ai.h"
#include "aseg.h"
#include "asega.h"
#include "sm.h"
#include "brx.h"
#include "cls.h"
#include "disc.h"
#include "guard.h"
#include "lo.h"
#include "modworld.h"
#include "pcloader.h"
#include "so.h"
#include "step.h"
#include "sw.h"
#include "transition.h"

#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
struct LIVE
{
    const cane::mods::Guest* pg = nullptr;
    LO* plo = nullptr;
    bool fHold = false;   // (a player class: kept at its place)
};
std::vector<LIVE> s_alive;
uint32_t s_serial = 0;

std::string WorldNameOf(int iwrec)
{
    const auto& awrec = AwrecWorlds();
    return iwrec >= 0 && iwrec < int(awrec.size()) ? g_disc.SzWorldName(awrec[std::size_t(iwrec)].fk.isector) : std::string();
}

// the guests of a world ("*": the command line's, every world)
std::vector<const cane::mods::Guest*> GuestsOf(const std::string& world)
{
    std::vector<const cane::mods::Guest*> a;
    if (world.empty()) return a;
    for (const cane::mods::Guest& g : g_disc.Aguest())
        if (g.world == world || g.world == "*") a.push_back(&g);
    return a;
}

int IwrecFrom(const cane::mods::Guest& g)
{
    const int iwrec = IwrecOfWorldArg(g.from);
    return iwrec >= 0 && !FModWorldRecord(iwrec) ? iwrec : -1;   // (installed levels only: a kept world is read from the disc)
}

bool FPrint() { return std::getenv("SLY2N_GUESTPRINT") != nullptr; }

void Place(LO* plo, const cane::mods::Guest& g)
{
    if (!FCidDerivedFrom(plo->Cid(), 0x30)) return;
    ALO* palo = static_cast<ALO*>(plo);
    const float rad = glm::radians(g.heading);
    const glm::mat3 m(glm::vec3(std::cos(rad), std::sin(rad), 0.0f), glm::vec3(-std::sin(rad), std::cos(rad), 0.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    const glm::vec3 pos(g.at[0], g.at[1], g.at[2]);
    if (STEP* pstep = PstepFromLo(plo))
    {
        // (as a teleport places a guard: actor.cpp TeleportLo, orig 0x2052E0)
        SetStepPos(pstep, pos - m * glm::vec3(pstep->vecBase));
        SetStepRotation(pstep, m);
        SetAloVelocityTo(palo, glm::vec3(0.0f));
        SetAloAngVelocityTo(palo, glm::vec3(0.0f));
        pstep->pxpFloor = nullptr;
        return;
    }
    SetAloRotation(palo, m);
    SetAloPos(palo, pos);
}

// after the world's load, before its start: each guest's copy made (started with the world's own objects) and placed
void OnLoaded(SW* psw)
{
    s_alive.clear();
    if (!psw) return;
    s_serial = psw->serial;
    const std::string world = WorldNameOf(g_transition.iwrecCur);
    for (const cane::mods::Guest* pg : GuestsOf(world))
    {
        const int iwrec = IwrecFrom(*pg);
        if (iwrec < 0)
        {
            std::fprintf(stderr, "mods: guest %s of %s: no level \"%s\"\n", pg->id.c_str(), world.c_str(), pg->from.c_str());
            continue;
        }
        if (PckwsKeptWorld(iwrec) != PCKWS_Ready)
        {
            std::fprintf(stderr, "mods: guest %s of %s: %s did not load\n", pg->id.c_str(), world.c_str(), pg->from.c_str());
            continue;
        }
        LO* ptmpl = PloKeptFind(iwrec, pg->object, 0);
        if (!ptmpl)
        {
            std::fprintf(stderr, "mods: guest %s of %s: %s has no object %d\n", pg->id.c_str(), world.c_str(), pg->from.c_str(), pg->object);
            continue;
        }
        LO* plo = PloPcCloneIntoWorld(psw, ptmpl, true);
        if (!plo) continue;
        s_alive.push_back({pg, plo});
        std::printf("mods: guest %s in %s: %s's object %d (cid 0x%X) at %.0f %.0f %.0f\n", pg->id.c_str(), world.c_str(), pg->from.c_str(), pg->object,
                    plo->Cid(), pg->at[0], pg->at[1], pg->at[2]);
    }
}

// after the start: a guard's patrol (a path of this world, walked along and back, again and again)
void OnStarted(SW* psw)
{
    if (!psw || psw->serial != s_serial) return;
    for (LIVE& l : s_alive)
    {
        Place(l.plo, *l.pg);   // (after the start: placed before, the start's records put it back where its template stands)
        if (FCidDerivedFrom(l.plo->Cid(), 0x34))
        {
            // Sly, Bentley or Murray as a character of the level (not the player): no gravity, held at its place, its idle
            // state machine's first animation looped (the player's +0x1028 part, gang.cpp psmIdle; else its +0x6F0 one)
            ALO* palo = static_cast<ALO*>(l.plo);
            if (SOST* ps = Psost(palo)) ps->dvGravity = glm::vec3(0.0f);
            l.fHold = true;
            ASEG* paseg = nullptr;
            for (const auto& [ib, ppart] : palo->apart)
                if ((ib == 0x1028 || ib == 0x6F0) && ppart && ppart->Cid() == 0x6F)
                    for (const SM::SMT& t : static_cast<SM*>(ppart)->asmt)
                        if (t.paseg && !paseg) paseg = t.paseg;
            if (paseg) PasegaEnsureAseg(paseg, palo, 0, 0.0f, 1.0f, 8);
            if (FPrint()) std::printf("mods: guest %s stands as a character (idle %d)\n", l.pg->id.c_str(), paseg ? paseg->oid : -1);
            continue;
        }
        GUARD* pgd = FCidDerivedFrom(l.plo->Cid(), 0x38) ? PguardFromLo(l.plo) : nullptr;
        if (!pgd) continue;
        if (l.pg->patrol < 0)
        {
            // (its own level's patrol names that level's paths: none here, it keeps its post)
            SetGuardPatrol(pgd, nullptr);
            continue;
        }
        LO* ppath = PloFindSwObjectByOid(psw, l.pg->patrol);
        if (!ppath)
        {
            std::fprintf(stderr, "mods: guest %s: no path %d in this world\n", l.pg->id.c_str(), l.pg->patrol);
            SetGuardPatrol(pgd, nullptr);
            continue;
        }
        auto* pl = static_cast<CMDLIST*>(PloNew(0x71, psw, nullptr, 0x2CB, -1));
        if (!pl) continue;
        // (label 1, along the path, back along it, back to label 1: the levels' own patrols' commands, ai.h CMD)
        PcmdAppend(pl, 9, 1);
        for (int fForward : {1, 0})
            if (CMD* pc = PcmdAppend(pl, 6, l.pg->patrol))
            {
                pc->n8 = fForward;
                pc->nC = -1;
            }
        PcmdAppend(pl, 10, 1);
        PostLoadCmdlist(pl);
        SetGuardPatrol(pgd, pl);
        if (FPrint()) std::printf("mods: guest %s walks along path %d\n", l.pg->id.c_str(), l.pg->patrol);
    }
}

// (SLY2N_GUESTPRINT: each guest's place every second)
void OnStep(SW* psw, bool)
{
    if (!psw || psw->serial != s_serial) return;
    for (const LIVE& l : s_alive)
        if (l.fHold) Place(l.plo, *l.pg);
    if (!FPrint()) return;
    static int s_c = 0;
    if (++s_c % 60 != 0) return;
    for (const LIVE& l : s_alive)
        if (FCidDerivedFrom(l.plo->Cid(), 0x30))
        {
            const glm::vec3 p = PosAloCenter(static_cast<ALO*>(l.plo));
            std::printf("mods: guest %s at %.0f %.0f %.0f%s%s\n", l.pg->id.c_str(), p.x, p.y, p.z, l.plo->fInWorld ? "" : " (out of the world)",
                        l.plo->fRemoved ? " (removed)" : "");
        }
}

struct INIT
{
    INIT()
    {
        PCWORLDHOOKS h;
        h.pfnLoaded = OnLoaded;
        h.pfnStarted = OnStarted;
        h.pfnStep = OnStep;
        RegisterPcWorldHooks(h);
    }
} s_init;
}  // namespace

void RequestModGuestWorlds(const uint8_t* abChk)
{
    const auto& awrec = AwrecWorlds();
    int iwrec = -1;
    for (std::size_t i = 0; i < awrec.size(); ++i)
        if (std::memcmp(awrec[i].abRecord, abChk, 64) == 0) iwrec = int(i);
    for (const cane::mods::Guest* pg : GuestsOf(WorldNameOf(iwrec)))
        if (const int iwrecFrom = IwrecFrom(*pg); iwrecFrom >= 0) RequestPcKeptWorld(iwrecFrom);
}

bool FAddGuestArg(const std::string& arg)
{
    // FROM:OBJECT@X,Y,Z[,HEADING][:PATROL]
    const std::size_t at = arg.find('@');
    const std::size_t colon = arg.rfind(':', at);
    if (at == std::string::npos || colon == std::string::npos) return false;
    cane::mods::Guest g;
    g.world = "*";
    g.from = arg.substr(0, colon);
    g.object = std::atoi(arg.c_str() + colon + 1);
    std::string rest = arg.substr(at + 1);
    if (const std::size_t c2 = rest.find(':'); c2 != std::string::npos)
    {
        g.patrol = std::atoi(rest.c_str() + c2 + 1);
        rest.resize(c2);
    }
    float v[4] = {0, 0, 0, 0};
    const int n = std::sscanf(rest.c_str(), "%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3]);
    if (n < 3) return false;
    for (int i = 0; i < 3; ++i) g.at[i] = v[i];
    g.heading = v[3];
    g.id = "cli-" + std::to_string(g_disc.Aguest().size());
    g.name = g.from + " " + std::to_string(g.object);
    g_disc.AddGuest(g);
    return true;
}

int CModGuestsLive() { return int(s_alive.size()); }
LO* PloModGuestLive(int i) { return i >= 0 && i < int(s_alive.size()) ? s_alive[std::size_t(i)].plo : nullptr; }
