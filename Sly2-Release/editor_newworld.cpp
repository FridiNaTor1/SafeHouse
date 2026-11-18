// See editor_newworld.h.
#include "editor_newworld.h"

#include <cstdio>

NEWWORLDOPTS g_newworldopts;

#if defined(SLY2_EDITOR) && SLY2_EDITOR

#include "disc.h"
#include "sw.h"
#include "transition.h"
#include "core/newworld.h"
#include "../platform/platform.h"

#include <algorithm>
#include <cstdlib>

namespace ce = cane::editor;

namespace
{
bool FFloats(const std::string& s, float* a, int cmin, int cmax, int& c)
{
    c = std::sscanf(s.c_str(), "%f,%f,%f,%f", &a[0], &a[1], &a[2], &a[3]);
    return c >= cmin && c <= cmax;
}
}  // namespace

int NewWorldCommand()
{
    const NEWWORLDOPTS& o = g_newworldopts;
    ce::Installation inst;
    std::string error;
    if (!inst.open(cane::path(g_disc.Path()), error)) { std::fprintf(stderr, "new-world: %s\n", error.c_str()); return 1; }
    ce::Project proj;
    const ce::fs::path dir = ce::pathOf(o.project);
    std::error_code ec;
    const bool fOpen = ce::fs::exists(dir / cane::mods::kManifestName, ec);
    if (fOpen ? !proj.open(dir, inst, error) : !proj.create(dir, ce::utf8(dir.filename()), inst, error))
    {
        std::fprintf(stderr, "new-world: %s\n", error.c_str());
        return 1;
    }
    ce::NewWorldSpec spec;
    spec.name = o.name;
    spec.templateRecord = o.templateRecord;
    spec.emptyLevel = !o.keepLevel;
    int c = 0;
    if (!o.spawn.empty())
    {
        if (!FFloats(o.spawn, spec.spawn, 3, 4, c)) { std::fprintf(stderr, "new-world: --nw-spawn X,Y,Z[,H]\n"); return 2; }
        spec.hasSpawn = true;
    }
    // (a world entered from another: that world's episode, so the HUD, the jobs' end and a death outside a job lead back there)
    if (const auto colon = o.entry.rfind(':'); colon != std::string::npos)
        for (const ce::WorldEntry& we : inst.worlds)
            if (we.name == o.entry.substr(0, colon)) spec.episode = we.episode;
    cane::mods::NewWorld w;
    if (!ce::addNewWorld(proj, inst, spec, &w, error)) { std::fprintf(stderr, "new-world: %s\n", error.c_str()); return 1; }
    std::string worldFrom;
    if (!o.entry.empty())
    {
        // WORLD:X,Y,Z[,R]
        const auto colon = o.entry.rfind(':');
        cane::mods::Entry e;
        float a[4] = {0, 0, 0, 150};
        if (colon == std::string::npos || !FFloats(o.entry.substr(colon + 1), a, 3, 4, c)) { std::fprintf(stderr, "new-world: --nw-entry WORLD:X,Y,Z[,R]\n"); return 2; }
        e.world = worldFrom = o.entry.substr(0, colon);
        e.to = w.name;
        std::copy(a, a + 3, e.at);
        e.radius = c == 4 ? a[3] : 150.0f;
        ce::addEntry(proj, e);
    }
    if (!o.ret.empty())
    {
        // X,Y,Z[,R][:WORLD[:POINT]]
        std::string s = o.ret, world = worldFrom;
        int point = -1;
        if (const auto c1 = s.find(':'); c1 != std::string::npos)
        {
            world = s.substr(c1 + 1);
            s = s.substr(0, c1);
            if (const auto c2 = world.find(':'); c2 != std::string::npos)
            {
                point = std::atoi(world.c_str() + c2 + 1);
                world = world.substr(0, c2);
            }
        }
        cane::mods::Entry e;
        float a[4] = {0, 0, 0, 150};
        if (world.empty() || !FFloats(s, a, 3, 4, c)) { std::fprintf(stderr, "new-world: --nw-return X,Y,Z[,R][:WORLD[:POINT]]\n"); return 2; }
        e.world = w.name;
        e.to = world;
        std::copy(a, a + 3, e.at);
        e.radius = c == 4 ? a[3] : 150.0f;
        e.point = point;
        ce::addEntry(proj, e);
    }
    if (!proj.save(error)) { std::fprintf(stderr, "new-world: %s\n", error.c_str()); return 1; }
    int record = int(inst.worlds.size());
    for (const ce::WorldEntry& we : inst.worlds) record = std::max(record, we.record + 1);
    record += int(cane::mods::newWorlds(proj.manifest).size()) - 1;
    std::printf("new-world: \"%s\" from %s (%s) in %s: %s; the game's world record %d when this package is the only one adding worlds\n",
                w.name.c_str(), w.templateWorld.c_str(), spec.emptyLevel ? "its level left out" : "all of it", ce::utf8(dir).c_str(), w.index.c_str(), record);
    return 0;
}

void RegisterProjectWorlds(ce::Installation& inst, const std::string& dirProject)
{
    cane::mods::Manifest m;
    std::string error;
    if (dirProject.empty() || !cane::mods::readManifest(ce::pathOf(dirProject), m, error)) return;
    const auto aw = cane::mods::newWorlds(m);
    if (aw.empty()) return;
    bool fNew = false;
    for (const cane::mods::NewWorld& w : aw) fNew = fNew || !g_disc.IsectorOfWorldName(w.name);
    if (fNew) g_disc.AddPackageWorlds(m);
    // (the records as the game numbers them, modworld.h AppendModWorldRecords: after the disc's, in the packages' order;
    // the classes are not ready here to read the boot world's list: the installation's worlds are the disc's)
    int cdisc = 0;
    for (const ce::WorldEntry& we : inst.worlds)
        if (we.indexBase.empty()) cdisc = std::max(cdisc, we.record + 1);
    const auto& amod = g_disc.AmodWorld();
    for (std::size_t i = 0; i < amod.size(); ++i)
    {
        const CDisc::MODWORLD& mw = amod[i];
        if (mw.package != m.id) continue;
        const int record = cdisc + int(i);
        if (inst.world(record)) continue;
        ce::WorldEntry e;
        e.record = record;
        e.code = mw.w.id;
        e.name = mw.w.name;
        e.episode = mw.w.episode;
        e.kind = mw.w.kind;
        e.index = mw.w.index;
        e.indexBase = m.filesDir();
        e.templateName = mw.w.templateWorld;
        inst.worlds.push_back(e);
        std::printf("editor: the project's world \"%s\" is record %d\n", e.name.c_str(), record);
    }
}

#else

int NewWorldCommand()
{
    std::fprintf(stderr, "new-world: this build has no map editor\n");
    return 1;
}

#endif
