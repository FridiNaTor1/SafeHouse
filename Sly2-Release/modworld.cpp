// See modworld.h.
#include "modworld.h"

#include "cls.h"
#include "lo.h"
#include "po.h"
#include "routetele.h"
#include "so.h"
#include "sw.h"
#include "transition.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <map>

namespace
{
void PutLe32(uint8_t* p, uint32_t u)
{
    p[0] = uint8_t(u);
    p[1] = uint8_t(u >> 8);
    p[2] = uint8_t(u >> 16);
    p[3] = uint8_t(u >> 24);
}

std::map<int, int> s_mpiwrecFilter;              // a new world's record -> its template's record
std::map<int, const CDisc::MODWORLD*> s_mpiwrecMod;

struct ARMED
{
    const cane::mods::Entry* pe = nullptr;
    bool fArmed = false;
};
std::vector<ARMED> s_aarmed;   // this world's sphere entries
}  // namespace

int IwrecOfWorldArg(const std::string& arg)
{
    const auto& awrec = AwrecWorlds();
    std::string name = arg;
    // "@id" or a package's world id: that new world's name
    const std::string id = !arg.empty() && arg[0] == '@' ? arg.substr(1) : arg;
    for (const CDisc::MODWORLD& m : g_disc.AmodWorld())
        if (m.w.id == id || (m.package + "/" + m.w.id) == id) name = m.w.name;
    int iwrec = IwrecOfWorldName(awrec, name);
    if (iwrec < 0)   // (any case: "paris")
        for (std::size_t i = 0; i < awrec.size() && iwrec < 0; ++i)
        {
            std::string n = g_disc.SzWorldName(awrec[i].fk.isector), a = name;
            for (char& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
            for (char& c : a) c = char(std::tolower(static_cast<unsigned char>(c)));
            if (!n.empty() && n == a && awrec[i].fk.cb) iwrec = int(i);
        }
    return iwrec;
}

int IwrecOfWorldName(const std::vector<WREC>& awrec, const std::string& name)
{
    const uint32_t isector = g_disc.IsectorOfWorldName(name);
    if (!isector) return -1;
    for (std::size_t i = 0; i < awrec.size(); ++i)
        if (awrec[i].fk.isector == isector && awrec[i].fk.cb) return int(i);
    return -1;
}

void AppendModWorldRecords(std::vector<WREC>& awrec)
{
    const auto& amod = g_disc.AmodWorld();
    if (amod.empty() || awrec.empty()) return;
    // (a list from a world file has the disc's records only; one made before has the new worlds of then: made again)
    awrec.erase(std::remove_if(awrec.begin(), awrec.end(), [](const WREC& w) {
        return w.fk.isector >= CDisc::ISECTOR_MODWORLD && w.fk.isector < CDisc::ISECTOR_MODWORLD + 0x100000u;
    }), awrec.end());
    const std::size_t cdisc = awrec.size();
    for (std::size_t i = 0; i < amod.size(); ++i)
    {
        const CDisc::MODWORLD& m = amod[i];
        WREC w;
        w.fk.isector = m.isector;
        w.fk.cb = 1;
        w.fk.id = 0x7F000000u + (m.isector - CDisc::ISECTOR_MODWORLD);   // (stable: the sector's slot)
        // (the key as FkFromRecord decodes it: sector = (w0 ^ w34) + 0xCA0, cb = w4 ^ w3C, id = w8 ^ w38)
        PutLe32(w.abRecord + 0x00, m.isector - 0xCA0u);
        PutLe32(w.abRecord + 0x04, w.fk.cb);
        PutLe32(w.abRecord + 0x08, w.fk.id);
        std::memcpy(w.abRecord + 0x10, "SafeHouse mod", 13);
        w.slot = m.w.episode;
        w.kind = m.w.kind;
        w.fMain = false;
        const int iwrec = int(awrec.size());
        int filter = iwrec;
        const uint32_t isectorTpl = m.w.templateWorld.empty() ? 0 : g_disc.IsectorOfWorldName(m.w.templateWorld);
        for (std::size_t k = 0; k < cdisc && isectorTpl; ++k)
            if (awrec[k].fk.isector == isectorTpl) { filter = int(k); break; }
        s_mpiwrecFilter[iwrec] = filter;
        s_mpiwrecMod[iwrec] = &m;
        awrec.push_back(w);
    }
}

int IwrecFilterOf(int iwrec)
{
    const auto it = s_mpiwrecFilter.find(iwrec);
    return it != s_mpiwrecFilter.end() ? it->second : iwrec;
}

const CDisc::MODWORLD* PmodworldOfRecord(int iwrec)
{
    const auto it = s_mpiwrecMod.find(iwrec);
    return it != s_mpiwrecMod.end() ? it->second : nullptr;
}

bool FModWorldRecord(int iwrec) { return PmodworldOfRecord(iwrec) != nullptr; }

void OnModWorldStarted(SW* psw, int iwrec, bool fCheckpoint)
{
    s_aarmed.clear();
    if (!psw) return;
    const auto& awrec = AwrecWorlds();
    std::string world;
    if (iwrec >= 0 && iwrec < int(awrec.size())) world = g_disc.SzWorldName(awrec[std::size_t(iwrec)].fk.isector);
    // a new world's spawn
    if (const CDisc::MODWORLD* pm = PmodworldOfRecord(iwrec); pm && pm->w.hasSpawn && !fCheckpoint)
    {
        g_routetele = ROUTETELE{};
        g_routetele.fPending = true;
        g_routetele.x = pm->w.spawn[0];
        g_routetele.y = pm->w.spawn[1];
        g_routetele.z = pm->w.spawn[2];
        g_routetele.h = pm->w.spawn[3];
        g_routetele.fH = true;
        std::printf("mods: new world \"%s\" (record %d): the player at its spawn %.0f %.0f %.0f\n", world.c_str(), iwrec,
                    pm->w.spawn[0], pm->w.spawn[1], pm->w.spawn[2]);
    }
    if (world.empty()) return;
    for (const cane::mods::Entry& e : g_disc.Aentry())
    {
        if (e.world != world) continue;
        const int iwrecTo = IwrecOfWorldName(awrec, e.to);
        if (iwrecTo < 0)
        {
            std::fprintf(stderr, "mods: an entry of %s leads to \"%s\", which is not there\n", world.c_str(), e.to.c_str());
            continue;
        }
        if (e.exit >= 0)
        {
            // an exit object (class 0x1A, LOXSHAPE): its world key (+0x144) and point (+0x150)
            LO* pexit = nullptr;
            for (LO* plo : psw->mpcidaplo[0x1A])
                if (plo->oid == e.exit) pexit = plo;
            auto* px = dynamic_cast<LOXSHAPE*>(pexit);
            if (!px)
            {
                std::fprintf(stderr, "mods: %s has no exit %d (an entry to %s)\n", world.c_str(), e.exit, e.to.c_str());
                continue;
            }
            std::array<uint8_t, 64> key{};
            std::memcpy(key.data(), awrec[std::size_t(iwrecTo)].abRecord, 64);
            if (px->aabKey.empty()) px->aabKey.push_back(key);
            else px->aabKey[0] = key;
            px->aabKey.resize(1);   // (no movies before a mod's world)
            if (e.point >= 0) px->SetField(0, 0x150, uint32_t(e.point));
            std::printf("mods: exit %d of %s leads to %s (record %d)\n", e.exit, world.c_str(), e.to.c_str(), iwrecTo);
        }
        else s_aarmed.push_back({&e, false});
    }
}

void UpdateModEntries(SW* psw)
{
    if (s_aarmed.empty() || !psw || g_transition.fPending || g_transition.wipes != WIPES_Idle) return;
    LO* ppo = PloPoCurrent(psw);
    if (!ppo || !FCidDerivedFrom(ppo->Cid(), 0x30)) return;
    const glm::vec3 pos = PosAloCenter(static_cast<ALO*>(ppo));
    for (ARMED& a : s_aarmed)
    {
        const cane::mods::Entry& e = *a.pe;
        const bool fIn = glm::length(pos - glm::vec3(e.at[0], e.at[1], e.at[2])) <= e.radius;
        if (!fIn) { a.fArmed = true; continue; }
        if (!a.fArmed) continue;   // (started inside: armed once the player has left the sphere)
        a.fArmed = false;
        const int iwrecTo = IwrecOfWorldName(AwrecWorlds(), e.to);
        if (iwrecTo < 0) continue;
        std::printf("mods: entry of %s at %.0f %.0f %.0f: to %s (record %d)\n", e.world.c_str(), e.at[0], e.at[1], e.at[2], e.to.c_str(), iwrecTo);
        RequestTransition(psw, AwrecWorlds()[std::size_t(iwrecTo)].abRecord, e.point >= 0 ? e.point : 0x189, -1, 0, 0x30A);
        return;
    }
}
