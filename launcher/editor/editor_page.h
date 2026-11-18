#pragma once
// The launcher's Editor page (desktop only: not in the Android / iOS launcher; CANE_LAUNCHER_EDITOR). Sly 2's map
// editor: the installed worlds read without running the game (editor/core/world.h), the editor projects (each a mod
// package, platform/mod_package.h), the installed packages; it starts the 3D editor (Sly2-Release --editor W) and the
// play test (Sly2-Release --mod-package P --world W). Sly 3 (the Sly 2 / Sly 3 switch, when Sly 3's worlds are
// installed as parts): the same with Sly3-Release, its gameplay objects listed by area. The page speaks the launcher's
// languages: its strings are in launcher/editor/lang (merged by launcher/l10n.h; scripts/test-launcher-l10n.py checks
// them). Design: Sly2-Release/notes/EDITOR_2026-11-12.md.
#include "../backend.h"
#include "../ui/widgets.h"

#include "core/project.h"
#include "core/spawn.h"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace cane::ui {

class EditorPage {
public:
    explicit EditorPage(LauncherBackend& backend) : m_backend(backend) {
        // (development / screenshots: SAFEHOUSE_EDITOR_GAME=sly3 starts on Sly 3, SAFEHOUSE_EDITOR_VIEW=gameplay on its data)
        if (const char* sz = std::getenv("SAFEHOUSE_EDITOR_GAME"); sz && std::strcmp(sz, "sly3") == 0) m_game = GameId::Sly3;
        if (const char* sz = std::getenv("SAFEHOUSE_EDITOR_VIEW"); sz && std::strcmp(sz, "gameplay") == 0) m_view = 1;
    }

    void open() { m_loaded = false; }

    void draw() {
        namespace ce = cane::editor;
        if (!m_loaded) load();
        heading(tr("Map Editor"));
        if (sly3Available()) {
            static const char* const kGames[] = {"Sly 2", "Sly 3"};
            int g = sly3() ? 1 : 0;
            if (segmented("ed-game", &g, kGames, px(260))) {
                m_game = g ? GameId::Sly3 : GameId::Sly2;
                m_world = -1;
                m_loaded = false;
                return;
            }
            gap(6);
        }
        if (sly3())
            caption(tr("Sly 3. Edit a world's objects in a 3D view drawn by the game (their options, curves, jobs, scripts, buttons and "
                       "what they trigger), then play it with your changes. Projects are mod packages that Sly 3 loads on every platform. "
                       "Experimental."));
        else
            caption(tr("Sly 2. Edit a world's objects in a 3D view drawn by the game, then play it with your changes. Projects are mod "
                       "packages that every SafeHouse version of Sly 2 loads (desktop, phones, Switch). Experimental."));
        gap(8);
        if (!m_error.empty()) {
            beginCard("ed-error");
            textWrapped(m_error.c_str(), fonts().body, color::warning);
            endCard();
            return;
        }
        const float w = ImGui::GetContentRegionAvail().x;
        const float left = std::floor(w * 0.36f);
        // ---- worlds
        beginCard("ed-worlds", {left, px(470)}, 16, true);
        overline(tr("Worlds"));
        for (std::size_t i = 0; i < m_inst.worlds.size(); ++i) {
            const ce::WorldEntry& e = m_inst.worlds[i];
            char label[160];
            std::snprintf(label, sizeof(label), "%s%s##w%d", e.episode ? (trf("Ep %d", e.episode) + "  ").c_str() : "", e.name.c_str(), e.record);
            if (ImGui::Selectable(label, int(i) == m_world)) selectWorld(int(i));
        }
        endCard();
        ImGui::SameLine(0, px(12));
        // ---- the world
        beginCard("ed-world", {0, px(470)}, 16, true);
        if (m_world >= 0) {
            const ce::WorldEntry& e = m_inst.worlds[std::size_t(m_world)];
            text(e.name.c_str(), fonts().tab);
            std::size_t cobj = 0, clevel = 0, cscript = 0;
            for (const ce::GroupInfo& g : m_index.groups) {
                cobj += g.objects.size();
                clevel += g.level;
                cscript += !g.scriptsPart.empty();
            }
            const std::string facts = (sly3() ? trf("world %d", e.record) : trf("record %d", e.record)) + "  |  " + e.code + "  |  " +
                                      trf("%zu groups (%zu level), %zu top-level objects, %zu script modules", m_index.groups.size(), clevel, cobj, cscript);
            caption(facts.c_str());
            gap(4);
            if (button(tr("Open in 3D editor"), Tone::Cta, {0, 0}, Icon::ChevronRight)) start3d(e.record, projectFor(e));
            ImGui::SameLine(0, px(8));
            if (button(tr("Play unmodified"), Tone::Secondary)) playTest(e.record, {});
            if (sly3()) {
                ImGui::SameLine(0, px(8));
                const char* const kViews[] = {tr("Groups"), tr("Gameplay data")};
                segmented("ed-view", &m_view, kViews, std::min(px(300), ImGui::GetContentRegionAvail().x));   // (long labels: within the card)
            }
            gap(6);
            if (sly3() && m_view == 1) {
                drawGameplay();
                endCard();
                gap(12);
                drawProjects();
                return;
            }
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##edfilter", tr("Filter groups"), m_filter, sizeof(m_filter));
            if (ImGui::BeginTable("ed-groups", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp, {0, 0})) {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn(tr("Group"));
                ImGui::TableSetupColumn(tr("Objects"), ImGuiTableColumnFlags_WidthFixed, px(90));
                ImGui::TableSetupColumn(tr("Scripts"), ImGuiTableColumnFlags_WidthFixed, px(80));
                ImGui::TableSetupColumn(tr("Classes"));
                ImGui::TableHeadersRow();
                const std::string filter = m_filter;
                for (const ce::GroupInfo& g : m_index.groups) {
                    if (!filter.empty() && g.name.find(filter) == std::string::npos) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (g.level) ImGui::TextColored(vec(color::cta), "%s", g.name.c_str());
                    else ImGui::TextUnformatted(g.name.c_str());
                    ImGui::TableNextColumn();
                    ImGui::Text("%zu", g.objects.size());
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(g.scriptsPart.empty() ? "-" : tr("yes"));
                    ImGui::TableNextColumn();
                    std::string classes;
                    for (std::size_t i = 0; i < g.classNames.size() && i < 4; ++i) classes += (i ? ", " : "") + g.classNames[i];
                    if (g.classNames.size() > 4) classes += ", ...";
                    ImGui::TextColored(vec(color::text2), "%s", classes.c_str());
                }
                ImGui::EndTable();
            }
        } else caption(tr("Choose a world."));
        endCard();
        gap(12);
        drawProjects();
    }

private:
    // ---- projects and installed packages (the game's own: its save folder's Editor/ and Mods/Packages)
    void drawProjects() {
        namespace ce = cane::editor;
        beginCard("ed-projects", {0, 0}, 16);
        overline(tr("Projects"));
        ImGui::SetNextItemWidth(px(260));
        ImGui::InputTextWithHint("##edname", tr("New project name"), m_name, sizeof(m_name));
        ImGui::SameLine(0, px(8));
        if (button(tr("New project"), Tone::Secondary, {0, 0}, Icon::Plus) && m_name[0] && m_world >= 0) {
            ce::Project p;
            std::string error;
            const fs::path dir = projectsDir() / ce::pathOf(cane::mods::idFromName(m_name));
            if (fs::exists(dir)) m_status = tr("A project with that name exists.");
            else if (!p.create(dir, m_name, m_inst, error)) m_status = error;
            else {
                p.manifest.extra["editor"] = {{"world", m_inst.worlds[std::size_t(m_world)].record}};
                cane::mods::writeManifest(p.manifest, error);
                m_name[0] = 0;
                m_status = trf("Made %s.", p.manifest.name.c_str());
            }
            scanProjects();
        }
        if (m_projects.empty()) caption(tr("No projects yet. \"Open in 3D editor\" makes one for the world, or name one above."));
        for (const Proj& p : m_projects) {
            ImGui::PushID(p.dir.c_str());
            ImGui::Separator();
            text(p.name.c_str(), fonts().tab);
            ImGui::SameLine();
            const std::string info = "  " + p.id + "  |  " + trf("%zu changed parts", p.parts) + (p.world >= 0 ? "  |  " + trf("world %d", p.world) : std::string());
            ImGui::TextColored(vec(color::muted), "%s", info.c_str());
            const int world = p.world >= 0 ? p.world : m_world >= 0 ? m_inst.worlds[std::size_t(m_world)].record : sly3() ? 3 : 2;
            if (button(tr("Edit##project"), Tone::Primary, {0, px(32)})) start3d(world, p.dir);
            ImGui::SameLine(0, px(6));
            if (button(tr("Play test"), Tone::Cta, {0, px(32)})) playTest(world, p.dir);
            ImGui::SameLine(0, px(6));
            if (button(tr("Install##project"), Tone::Secondary, {0, px(32)})) packageAction(p, 0);
            ImGui::SameLine(0, px(6));
            if (button(tr("Export .zip"), Tone::Secondary, {0, px(32)})) packageAction(p, 1);
            ImGui::SameLine(0, px(6));
            if (button(tr("Open folder##project"), Tone::Quiet, {0, px(32)}, Icon::Folder)) cane::openFolder(cane::path(p.dir));
            ImGui::PopID();
        }
        endCard();
        gap(12);
        // ---- installed packages (Sly 2's Mods/Packages)
        beginCard("ed-packages", {0, 0}, 16);
        overline(tr("Installed mod packages"));
        if (m_packages.empty())
            caption(sly3() ? tr("None. Copy a package folder (mod.json with \"game\": \"sly3\") into Mods/Packages in Sly 3's save folder.")
                           : tr("None. Install a project, or copy a package folder into Mods/Packages in Sly 2's save folder."));
        for (std::size_t i = 0; i < m_packages.size(); ++i) {
            Pkg& k = m_packages[i];
            ImGui::PushID(int(i));
            bool on = k.enabled;
            if (ImGui::Checkbox("##on", &on)) setEnabled(k, on);
            ImGui::SameLine();
            ImGui::TextUnformatted(k.label.c_str());
            ImGui::PopID();
        }
        endCard();
        if (!m_status.empty()) {
            gap(6);
            caption(m_status.c_str(), color::text2);
        }
    }

    // ---- (Sly 3, read only) the world's gameplay objects by area, from its index: jobs, hacking, two-player, ...
    struct Area {
        const char* name;
        const char* note;
        std::vector<const char*> classes;
    };
    static const std::vector<Area>& sly3Areas() {
        static const std::vector<Area> a = {
            {N_("Jobs"), N_("missions and their tasks (the story's job graph), task counters"), {"MISSION", "TASK", "TASKCOUNTER"}},
            {N_("Buttons and switches"), N_("uses (what they trigger is edited in the 3D editor: task states, state machines, scripts), pads"),
             {"USE", "CARRYUSE", "PAD", "NEW_PAD_5E"}},
            {N_("Hacking"), N_("the hacking minigames' ships, enemies, pieces and docks"), {"HACKSO", "HACKSHIP", "HACKENEMY", "HACKPIECE", "HACKDOCK"}},
            {N_("Rides and vehicles"), N_("the RC car, planes, the dump truck, racers, the Guru's ride links"),
             {"VEHICLE", "NEW_SO_51", "NEW_SO_52", "NEW_SO_55", "NEW_LO_98", "NEW_ALO_7A"}},
            {N_("Two-player"), N_("booty stands (Cops and Robbers) and spawn points"), {"BOOTYSTAND", "SPAWNER"}},
            {N_("Pickups and progress"), N_("coins, charms, loot, clue bottles, power-ups, vaults, booty, checkpoints"),
             {"COIN", "CHARM", "LOOT", "CLUEBOTTLE", "PICKUP_6C", "NEW_PICKUP_BASE_6C", "POCKET", "VAULT", "BOOTY", "PRIZE", "CHECKPOINT", "SAFEHOUSE"}},
            {N_("Characters"), N_("the gang, guards and bosses placed in the world"),
             {"BENTLEY", "MURRAY", "GURU", "PENELOPE", "PANDA_KING", "DIMITRI", "CARMELITA", "RAJAN", "CONTESSA", "STEPGUARD",
              "GUNGUARD", "GUARD_3E", "GUARD_42", "GUARD_43"}},
            {N_("Traversal"), N_("paths, pipes, ledges, rails, spires, grips and climbing surfaces"),
             {"PATH", "PIPE", "LEDGE", "RAIL", "SPIRE", "GRIP", "CLIMBSURF", "CRAWLHOOK", "NAVPATH"}},
            {N_("Triggers and exits"), N_("volumes, triggers, exits, warps, lasers and barriers"),
             {"VOLTRIGGER", "VOLUME", "EXIT", "WARP", "LASER", "BARRIER"}},
        };
        return a;
    }
    void drawGameplay() {
        namespace ce = cane::editor;
        caption(tr("The objects of each gameplay area in this world (class, object number). Open the 3D editor to change them."), color::text2);
        if (!ImGui::BeginChild("ed-gameplay", {0, 0})) { ImGui::EndChild(); return; }
        for (const Area& area : sly3Areas()) {
            struct Hit { std::string cls, group; int oid; };
            std::vector<Hit> hits;
            for (const ce::GroupInfo& g : m_index.groups)
                for (std::size_t i = 0; i < g.objects.size() && i < g.classNames.size(); ++i)
                    for (const char* c : area.classes)
                        if (g.classNames[i] == c) hits.push_back({g.classNames[i], g.name, g.objects[i].oid});
            char head[160];
            std::snprintf(head, sizeof(head), "%s  (%zu)###%s", tr(area.name), hits.size(), area.name);
            if (!ImGui::CollapsingHeader(head, !hits.empty() && &area == &sly3Areas().front() ? ImGuiTreeNodeFlags_DefaultOpen : 0)) continue;
            ImGui::TextColored(vec(color::muted), "%s", tr(area.note));
            std::map<std::string, std::vector<const Hit*>> byClass;
            for (const Hit& h : hits) byClass[h.cls].push_back(&h);
            for (const auto& [cls, a] : byClass) {
                std::string oids;
                for (std::size_t i = 0; i < a.size() && i < 24; ++i) oids += (i ? " " : "") + std::to_string(a[i]->oid);
                if (a.size() > 24) oids += " ...";
                ImGui::TextColored(vec(color::cta), "%s", cls.c_str());
                ImGui::SameLine();
                ImGui::TextColored(vec(color::text2), "x%zu", a.size());
                ImGui::SameLine();
                ImGui::TextWrapped("%s", oids.c_str());
            }
        }
        ImGui::EndChild();
    }

    struct Proj {
        std::string dir, name, id;
        int world = -1;
        std::size_t parts = 0;
    };
    struct Pkg {
        fs::path dir;
        std::string label;
        bool enabled = true;
    };

    // (SAFEHOUSE_EDITOR_DATA=<a Sly 2 installation>, SAFEHOUSE_EDITOR_DATA3=<a Sly 3 installation: the disc image's folder
    // with Worlds/>: that one instead of the selected region's; development / screenshots)
    bool sly3() const { return m_game == GameId::Sly3; }
    const char* envData() const { return sly3() ? "SAFEHOUSE_EDITOR_DATA3" : "SAFEHOUSE_EDITOR_DATA"; }
    fs::path dataDir() const {
        if (const char* sz = std::getenv(envData()); sz && *sz) return cane::path(sz);
        return m_backend.dataDir(m_game);
    }
    // Sly 3 is offered when its worlds are installed as parts (Worlds/worlds.json beside the disc image)
    bool sly3Available() const {
        if (const char* sz = std::getenv("SAFEHOUSE_EDITOR_DATA3"); sz && *sz) return true;
        std::error_code ec;
        return m_backend.assetsInstalled(GameId::Sly3) && fs::is_regular_file(m_backend.dataDir(GameId::Sly3) / "Worlds" / "worlds.json", ec);
    }
    fs::path projectsDir() const { return m_backend.userDir(m_game) / "Editor" / (sly3() ? "Sly 3" : "Sly 2"); }
    fs::path modsDir() const { return m_backend.userDir(m_game) / "Mods"; }

    void load() {
        m_loaded = true;
        m_error.clear();
        if (!std::getenv(envData()) && !m_backend.assetsInstalled(m_game)) {
            m_error = sly3() ? tr("Install Sly 3 first (its game page).") : tr("Install Sly 2 first (its game page).");
            return;
        }
        if (!m_inst.open(dataDir(), m_error)) return;
        if (m_world < 0 || m_world >= int(m_inst.worlds.size()))
            for (std::size_t i = 0; i < m_inst.worlds.size(); ++i)
                if (m_inst.worlds[i].record == (sly3() ? 3 : 2)) m_world = int(i);
        if (m_world >= 0) selectWorld(m_world);
        scanProjects();
    }

    void selectWorld(int i) {
        m_world = i;
        std::string error;
        if (!m_index.load(m_inst, m_inst.worlds[std::size_t(i)], error)) m_status = error;
    }

    void scanProjects() {
        m_projects.clear();
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(projectsDir(), ec)) {
            cane::mods::Manifest m;
            std::string error;
            if (!e.is_directory() || !cane::mods::readManifest(e.path(), m, error)) continue;
            Proj p;
            p.dir = cane::editor::utf8(e.path());
            p.name = m.name;
            p.id = m.id;
            if (m.extra.contains("editor")) p.world = m.extra["editor"].value("world", -1);
            if (p.world < 0 && !m.worlds.empty()) p.world = m.worlds[0].value("record", -1);
            for (const auto& f : fs::recursive_directory_iterator(e.path() / cane::mods::kFilesFolder, ec))
                if (f.is_regular_file()) ++p.parts;
            m_projects.push_back(p);
        }
        std::sort(m_projects.begin(), m_projects.end(), [](const Proj& a, const Proj& b) { return a.name < b.name; });
        m_packages.clear();
        for (const auto& e : fs::directory_iterator(modsDir() / cane::mods::kPackagesFolder, ec)) {
            if (!e.is_directory()) continue;
            Pkg k;
            k.dir = e.path();
            const std::string folder = cane::editor::utf8(e.path().filename());
            k.enabled = !(folder.size() > 9 && folder.compare(folder.size() - 9, 9, ".disabled") == 0);
            cane::mods::Manifest m;
            std::string error;
            if (cane::mods::readManifest(e.path(), m, error)) {
                k.label = m.name + "  (" + m.id + " " + m.version + ")";
                if (const std::string why = cane::mods::mismatch(m, m_inst.game, m_inst.region, m_inst.filesFormat); !why.empty()) k.label += "  - " + why;
            } else k.label = folder + "  - " + error;
            m_packages.push_back(k);
        }
    }

    std::string projectFor(const cane::editor::WorldEntry& e) {
        for (const Proj& p : m_projects)
            if (p.world == e.record) return p.dir;
        // a new one for the world (the 3D editor makes it)
        return cane::editor::utf8(projectsDir() / cane::editor::pathOf(cane::mods::idFromName(e.name)));
    }

    std::vector<std::string> baseArgs() const {
        return {"--data-dir", utf8(dataDir()), "--user-dir", utf8(m_backend.userDir(m_game)), "--languages-dir",
                utf8(m_backend.languagesDir(m_game))};
    }

    void start3d(int record, const std::string& project) {
        std::vector<std::string> a = baseArgs();
        a.insert(a.end(), {"--editor", std::to_string(record), "--editor-project", project});
        a.insert(a.end(), {"--editor-language", l10n::current()});   // (the 3D editor's strings in the launcher's language)
        std::error_code ec;
        fs::create_directories(projectsDir(), ec);
        if (!cane::editor::spawnDetached(m_backend.binary(cane::game(m_game).runtime), a)) m_status = tr("The 3D editor did not start.");
        else m_status = tr("The 3D editor is starting in its own window.");
        m_loaded = false;   // (its projects again when it is closed and this page shown)
    }

    void playTest(int record, const std::string& project) {
        std::vector<std::string> a = baseArgs();
        if (!project.empty()) a.insert(a.end(), {"--mod-package", project});
        a.insert(a.end(), {"--world", std::to_string(record)});
        const std::string name = sly3() ? "Sly 3" : "Sly 2";
        if (!cane::editor::spawnDetached(m_backend.binary(cane::game(m_game).runtime), a)) m_status = trf("%s did not start.", name.c_str());
        else m_status = project.empty() ? trf("%s is starting.", name.c_str()) : trf("%s is starting with the project loaded.", name.c_str());
    }

    void packageAction(const Proj& p, int action) {
        cane::editor::Project proj;
        std::string error;
        if (!proj.open(cane::path(p.dir), m_inst, error)) { m_status = error; return; }
        if (action == 0) {
            if (proj.install(modsDir(), error)) m_status = trf("Installed: %s loads %s now (Mods/Packages).", sly3() ? "Sly 3" : "Sly 2", proj.manifest.id.c_str());
            else m_status = error;
        } else {
            const fs::path zip = projectsDir() / cane::editor::pathOf(proj.manifest.id + ".zip");
            if (proj.exportZip(zip, error)) m_status = trf("Exported %s", utf8(zip).c_str());
            else m_status = error;
        }
        scanProjects();
    }

    void setEnabled(Pkg& k, bool on) {
        std::string folder = cane::editor::utf8(k.dir.filename());
        if (!on) folder += ".disabled";
        else if (folder.size() > 9) folder.resize(folder.size() - 9);
        std::error_code ec;
        fs::rename(k.dir, k.dir.parent_path() / cane::editor::pathOf(folder), ec);
        if (ec) m_status = ec.message();
        scanProjects();
    }

    LauncherBackend& m_backend;
    GameId m_game = GameId::Sly2;
    int m_view = 0;   // (Sly 3) 0 the groups, 1 the gameplay data
    bool m_loaded = false;
    std::string m_error, m_status;
    cane::editor::Installation m_inst;
    cane::editor::WorldIndex m_index;
    int m_world = -1;
    char m_filter[64] = "";
    char m_name[64] = "";
    std::vector<Proj> m_projects;
    std::vector<Pkg> m_packages;
};

}  // namespace cane::ui
