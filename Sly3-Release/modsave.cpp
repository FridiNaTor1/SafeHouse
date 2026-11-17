// (Sly 2's modsave.cpp, the same: Sly 3's task code calls it, game/task3.cpp; Sly2-Release/notes/EDITOR_2026-11-12.md "Round 5")
#include "modsave.h"

#include "disc.h"
#include "textmod.h"

#include "../platform/platform.h"

#include <nlohmann/json.hpp>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace {
namespace fs = std::filesystem;

nlohmann::json s_jCur = nlohmann::json::object();       // {package id: {world: {"oid": state}}}
nlohmann::json s_jPending = nlohmann::json::object();

fs::path PathSide(const std::string& pathSlot) { return cane::path(pathSlot + ".mods.json"); }

nlohmann::json JRead(const std::string& pathSlot)
{
    if (pathSlot.empty()) return nlohmann::json::object();
    std::error_code ec;
    const fs::path p = PathSide(pathSlot);
    if (!fs::exists(p, ec)) return nlohmann::json::object();
    std::ifstream in(p, std::ios::binary);
    try
    {
        nlohmann::json j;
        in >> j;
        if (!j.is_object()) throw std::runtime_error("not an object");
        // (only the shape {string: {string: {string: int}}} is kept)
        nlohmann::json out = nlohmann::json::object();
        for (auto ip = j.begin(); ip != j.end(); ++ip)
        {
            if (!ip->is_object()) continue;
            for (auto iw = ip->begin(); iw != ip->end(); ++iw)
            {
                if (!iw->is_object()) continue;
                for (auto it = iw->begin(); it != iw->end(); ++it)
                    if (it->is_number_integer()) out[ip.key()][iw.key()][it.key()] = it->get<int>();
            }
        }
        std::printf("mods: task states from %s\n", cane::utf8(p).c_str());
        return out;
    }
    catch (const std::exception& e)
    {
        std::printf("mods: %s ignored (%s)\n", cane::utf8(p).c_str(), e.what());
        return nlohmann::json::object();
    }
}

// the current world's name as the packages' "custom_tasks" spell it, and the package listing oid there
bool FListed(int oid, std::string& world, std::string& id)
{
    if (!g_disc.FCustomTasks()) return false;
    world = g_disc.SzWorldName(g_isectorWorldLoading);
    if (world.empty()) return false;
    id = g_disc.IdPackageOfTask(world, oid);
    return !id.empty();
}
}  // namespace

bool FModTaskState(int oid, int* pn)
{
    std::string world, id;
    if (!FListed(oid, world, id)) return false;
    *pn = -1;
    const std::string key = std::to_string(oid);
    if (const auto ip = s_jCur.find(id); ip != s_jCur.end())
        if (const auto iw = ip->find(world); iw != ip->end())
            if (const auto it = iw->find(key); it != iw->end() && it->is_number_integer()) *pn = it->get<int>();
    return true;
}

bool FSetModTaskState(int oid, int n)
{
    std::string world, id;
    if (!FListed(oid, world, id)) return false;
    s_jCur[id][world][std::to_string(oid)] = n;
    return true;
}

void ReadModSave(const std::string& pathSlot) { s_jCur = JRead(pathSlot); }

bool WriteModSave(const std::string& pathSlot)
{
    std::error_code ec;
    const fs::path p = PathSide(pathSlot);
    if (s_jCur.empty())
    {
        if (fs::exists(p, ec)) fs::remove(p, ec);
        return true;
    }
    const fs::path tmp = cane::path(pathSlot + ".mods.json.tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) { std::printf("mods: cannot write %s\n", cane::utf8(p).c_str()); return false; }
        out << s_jCur.dump(1) << '\n';
        if (!out) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) { std::printf("mods: cannot write %s (%s)\n", cane::utf8(p).c_str(), ec.message().c_str()); return false; }
    return true;
}

void RemoveModSave(const std::string& pathSlot)
{
    std::error_code ec;
    if (const fs::path p = PathSide(pathSlot); fs::exists(p, ec)) fs::remove(p, ec);
}

void ClearModSave() { s_jCur = nlohmann::json::object(); }

void PendModSave(const std::string& pathSlot) { s_jPending = JRead(pathSlot); }

void ApplyPendingModSave() { s_jCur = s_jPending; }
