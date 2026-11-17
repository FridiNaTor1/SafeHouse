#pragma once
// The launcher's Localisation page (desktop only, beside the Map Editor: CANE_LAUNCHER_EDITOR): make and keep up a Sly 2
// translation. A language of the shared languages folder (or the mods folder's Mods/Languages) is shown string by
// string beside a reference language (English by default): the game's text by id and the port's own PC strings in their
// own section, with filters, search, each string's levels and its per-level texts, checks (icons, %-codes, line breaks,
// length, the font's letters) and progress; saved to its Text.json, exported as a mod package or a CSV table. The model:
// loc_core.h. The page speaks the launcher's languages (launcher/editor/lang). Docs: docs/MODDING.md "Translating".
//
// It runs the game program twice, in the background, into the launcher's cache folder: --dump-port-text (the port's
// built-in strings of the 12 languages: the PC strings' reference) and --dump-text (each string's levels: usage.json).
#include "../backend.h"
#include "../ui/widgets.h"

#include "../collection/audio.h"
#include "core/spawn.h"
#include "loc_core.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace cane::ui {

class LocalisationPage {
public:
    explicit LocalisationPage(LauncherBackend& backend) : m_backend(backend) {}
    ~LocalisationPage() { stopJob(); }

    void open() {
        m_loaded = false;
    }

    void draw() {
        if (!m_loaded) load();
        pollJob();
        heading(tr("Translate Sly 2"));
        caption(tr("Make or update a translation of Sly 2, string by string beside another language. The game's text and SafeHouse's own PC "
                   "strings (menus, options, key names) are both here; saved translations work on every disc and platform."));
        gap(8);
        if (!m_error.empty()) {
            beginCard("loc-error");
            textWrapped(m_error.c_str(), fonts().body, color::warning);
            endCard();
            return;
        }
        drawToolbar();
        gap(8);
        if (m_newLanguage) {
            drawNewLanguage();
            return;
        }
        if (m_current < 0) {
            beginCard("loc-none");
            caption(m_jobRunning ? tr("Preparing the strings...") : tr("Choose a language above, or make a new one."));
            endCard();
            return;
        }
        if (m_settings) {
            drawSettings();
            return;
        }
        drawFilters();
        gap(6);
        const float w = ImGui::GetContentRegionAvail().x;
        const float h = std::max(px(260), ImGui::GetContentRegionAvail().y - px(4));
        beginCard("loc-table", {std::floor(w * 0.58f), h}, 10);
        if (m_section == 2) drawVoiceTable();
        else drawTable();
        endCard();
        ImGui::SameLine(0, px(10));
        beginCard("loc-detail", {0, h}, 16, true);
        if (m_section == 2) drawVoiceDetail();
        else drawDetail();
        endCard();
    }

private:
    // ---------------------------------------------------------------- places
    fs::path languagesDir() const {
        if (const char* sz = std::getenv("SAFEHOUSE_LOC_LANGUAGES"); sz && *sz) return cane::path(sz);
        return m_backend.languagesDir(GameId::Sly2);
    }
    fs::path modsLanguagesDir() const { return m_backend.userDir(GameId::Sly2) / "Mods" / "Languages"; }
    fs::path dataDir() const {
        if (const char* sz = std::getenv("SAFEHOUSE_EDITOR_DATA"); sz && *sz) return cane::path(sz);
        return m_backend.dataDir(GameId::Sly2);
    }
    fs::path cacheDir() const { return cane::cacheDirectory("sly2") / "Localisation"; }
    fs::path portDir() const { return cacheDir() / "port"; }
    fs::path usageDir() const {
        // (one per installation: the region's worlds)
        std::string key = utf8(dataDir());
        uint64_t h = 1469598103934665603ull;
        for (char c : key) h = (h ^ uint8_t(c)) * 1099511628211ull;
        char name[32];
        std::snprintf(name, sizeof(name), "text-%016llx", static_cast<unsigned long long>(h));
        return cacheDir() / name;
    }
    std::vector<std::string> baseArgs() const {
        return {"--data-dir", utf8(dataDir()), "--user-dir", utf8(m_backend.userDir(GameId::Sly2)), "--languages-dir", utf8(languagesDir())};
    }
    fs::path runtime() const { return m_backend.binary(cane::game(GameId::Sly2).runtime); }

    // ---------------------------------------------------------------- loading
    void load() {
        m_loaded = true;
        m_error.clear();
        m_folders = loc::scanFolders(languagesDir(), modsLanguagesDir());
        std::error_code ec;
        const bool portReady = fs::is_regular_file(portDir() / "English.json", ec) && !runtimeNewer(portDir() / "English.json");
        const bool usageReady = fs::is_regular_file(usageDir() / "usage.json", ec);
        if (usageReady) m_usage = loc::readUsage(usageDir() / "usage.json");
        if (!portReady || !usageReady) startJob(!portReady, !usageReady);
        if (m_ref.empty()) m_ref = "English";
        if (m_current < 0 || !portReady) {
            // (development / screenshots: SAFEHOUSE_LOC_LANGUAGE=<folder> opens that one)
            std::string want = m_currentKey;
            if (const char* sz = std::getenv("SAFEHOUSE_LOC_LANGUAGE"); sz && *sz && want.empty()) want = sz;
            m_current = -1;
            if (!want.empty() && portReady) openLanguage(want);
        } else openLanguage(m_currentKey);
    }
    bool runtimeNewer(const fs::path& file) const {
        std::error_code ec;
        try {
            return fs::last_write_time(runtime(), ec) > fs::last_write_time(file, ec);
        } catch (...) {
            return false;
        }
    }
    int folderIndex(const std::string& key) const {
        for (std::size_t i = 0; i < m_folders.size(); ++i)
            if (m_folders[i].key == key) return int(i);
        return -1;
    }
    void openLanguage(const std::string& key) {
        const int i = folderIndex(key);
        if (i < 0) return;
        const int r = folderIndex(m_ref);
        const int en = folderIndex("English");
        m_t = loc::load(m_folders[std::size_t(i)], r >= 0 ? &m_folders[std::size_t(r)] : nullptr, en >= 0 ? &m_folders[std::size_t(en)] : nullptr, portDir());
        if (m_t.font.japanese) {
            m_fontChars = loc::japaneseFont(m_folders[std::size_t(i)], m_folders);
            m_t.font.characters = m_fontChars.empty() ? nullptr : &m_fontChars;
        }
        m_t.recheckAll();
        m_rowById.clear();
        for (std::size_t k = 0; k < m_t.rows.size(); ++k)
            if (!m_t.rows[k].port) m_rowById[m_t.rows[k].id] = int(k);
        scanVoices();
        m_current = i;
        m_currentKey = key;
        m_selected = -1;
        m_dirtyFilter = true;
        m_meta = m_folders[std::size_t(i)].meta;
        if (const char* sz = std::getenv("SAFEHOUSE_LOC_SELECT"); sz && *sz)
            for (std::size_t k = 0; k < m_t.rows.size(); ++k)
                if (m_t.rows[k].id == sz) select(int(k));
        if (const char* sz = std::getenv("SAFEHOUSE_LOC_SECTION"); sz && *sz) m_section = std::clamp(std::atoi(sz), 0, 2);
        if (const char* sz = std::getenv("SAFEHOUSE_LOC_VOICE"); sz && *sz)
            for (std::size_t k = 0; k < m_voices.size(); ++k)
                if (m_voices[k].id == sz) m_voiceSel = int(k), m_voiceScroll = true;
        if (const char* sz = std::getenv("SAFEHOUSE_LOC_FILTER"); sz && *sz) m_filter = std::clamp(std::atoi(sz), 0, 4);
        if (m_selected >= 0 && m_section != 2) m_section = m_t.rows[std::size_t(m_selected)].port ? 1 : 0;
    }
    bool unsaved() const {
        if (m_current < 0) return false;
        for (const loc::Row& r : m_t.rows)
            if (m_t.changed(r)) return true;
        return false;
    }

    // ---------------------------------------------------------------- the game program in the background
    void startJob(bool port, bool usage) {
        if (m_jobRunning) return;
        stopJob();
        m_jobRunning = true;
        m_jobDone = false;
        m_jobCancel = false;
        m_jobFailed.clear();
        fs::path bin;
        try {
            bin = runtime();
        } catch (const std::exception& e) {
            m_jobRunning = false;
            m_jobDone = true;
            m_jobFailed = e.what();
            return;
        }
        const fs::path portOut = portDir(), usageOut = usageDir();
        const std::vector<std::string> base = baseArgs();
        m_jobUsage = usage;
        m_job = std::thread([this, bin, portOut, usageOut, base, port, usage] {
            auto run = [&](std::vector<std::string> a) {
                a.insert(a.begin(), utf8(bin));
                try {
                    return cane::runProcess(a, bin.parent_path(), m_jobCancel, [](const std::string&) {});
                } catch (...) {
                    return 1;
                }
            };
            std::error_code ec;
            if (port) {
                fs::create_directories(portOut, ec);
                if (run({"--dump-port-text", utf8(portOut)}) != 0) m_jobFailedThread = "port";
            }
            m_portDone = true;
            if (usage && !m_jobCancel) {
                fs::create_directories(usageOut, ec);
                std::vector<std::string> a = base;
                a.insert(a.end(), {"--dump-text", utf8(usageOut)});
                if (run(a) != 0) m_jobFailedThread = "usage";
            }
            m_jobDone = true;
        });
    }
    void stopJob() {
        m_jobCancel = true;
        if (m_job.joinable()) m_job.join();
        m_jobRunning = false;
    }
    void pollJob() {
        if (!m_jobRunning) return;
        if (m_portDone.exchange(false) && m_current < 0) {
            std::string want = m_currentKey;
            if (const char* sz = std::getenv("SAFEHOUSE_LOC_LANGUAGE"); sz && *sz && want.empty()) want = sz;
            if (!want.empty()) openLanguage(want);
        }
        if (!m_jobDone) return;
        m_job.join();
        m_jobRunning = false;
        m_jobFailed = m_jobFailedThread;
        if (m_jobUsage) m_usage = loc::readUsage(usageDir() / "usage.json");
        // (the PC strings' reference arrived after the language was opened: open it again, unless it has edits)
        if (m_current >= 0 && !unsaved() && std::none_of(m_t.rows.begin(), m_t.rows.end(), [](const loc::Row& r) { return r.port; }))
            openLanguage(m_currentKey);
    }

    // ---------------------------------------------------------------- the toolbar: language, reference, actions
    void drawToolbar() {
        beginCard("loc-bar", {0, 0}, 14);
        const float comboW = px(220);
        // the language
        pushComboStyle();
        ImGui::SetNextItemWidth(comboW);
        const std::string preview = m_current >= 0 ? m_folders[std::size_t(m_current)].displayName() : std::string(tr("Choose a language"));
        if (beginCombo("##loc-lang", preview.c_str())) {
            for (std::size_t i = 0; i < m_folders.size(); ++i) {
                const loc::Folder& f = m_folders[i];
                if (!f.text && loc::gameLanguage(f.key) < 0) continue;
                if (ImGui::Selectable(f.displayName().c_str(), int(i) == m_current)) {
                    if (unsaved()) m_status = tr("Save or revert the changes first.");
                    else {
                        m_settings = false;
                        openLanguage(f.key);
                    }
                }
            }
            ImGui::Separator();
            if (ImGui::Selectable(tr("New language..."))) {
                m_newLanguage = true;
                m_newName[0] = m_newCode[0] = 0;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, px(10));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(vec(color::text2), "%s", tr("beside"));
        ImGui::SameLine(0, px(10));
        ImGui::SetNextItemWidth(comboW);
        if (beginCombo("##loc-ref", m_ref.c_str())) {
            for (const loc::Folder& f : m_folders) {
                if (!f.text) continue;
                if (ImGui::Selectable(f.key.c_str(), f.key == m_ref) && f.key != m_ref) {
                    m_ref = f.key;
                    if (m_current >= 0) changeReference();
                }
            }
            ImGui::EndCombo();
        }
        m_refComboRight = ImGui::GetItemRectMax().x;
        popComboStyle();
        if (m_current >= 0) {
            // (the actions beside the languages, or under them in a narrow window)
            const float actionsW = measure(fonts().tab, tr("Save")).x + measure(fonts().body, tr("Language settings")).x +
                                   measure(fonts().body, tr("Test in game")).x + px(3 * 48 + 3 * 6 + 40 + 14);
            const float right = ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x;
            if (right - m_refComboRight > actionsW + px(14)) ImGui::SameLine(0, px(14));
            else gap(-4);
            const bool dirty = unsaved();
            ImGui::BeginDisabled(!dirty);
            if (button(tr("Save"), Tone::Cta, {0, 0}, Icon::Check) || (dirty && ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_S, false))) saveNow();
            ImGui::EndDisabled();
            ImGui::SameLine(0, px(6));
            if (button(tr("Language settings"), Tone::Secondary, {0, 0}, Icon::Cog)) m_settings = !m_settings;
            ImGui::SameLine(0, px(6));
            if (button(tr("Test in game"), Tone::Secondary, {0, 0}, Icon::Play)) testInGame();
            ImGui::SameLine(0, px(6));
            if (iconButton(tr("More##loc"), Icon::ChevronDown)) ImGui::OpenPopup("loc-more");
            if (ImGui::BeginPopup("loc-more")) {
                if (ImGui::Selectable(tr("Export as a mod package"))) exportPackage();
                if (ImGui::Selectable(tr("Export a CSV table"))) exportCsv();
                if (ImGui::Selectable(tr("Import the CSV table"))) importCsv();
                if (ImGui::Selectable(tr("Revert to the saved file")) ) openLanguage(m_currentKey);
                if (ImGui::Selectable(tr("Find where strings are used again"))) startJob(false, true);
                if (ImGui::Selectable(tr("Open the language's folder"))) cane::openFolder(m_t.target().parent_path());
                ImGui::EndPopup();
            }
            // progress
            int game = 0, gameDone = 0, port = 0, portDone = 0;
            for (const loc::Row& r : m_t.rows) {
                const bool done = loc::Translation::status(r) == loc::Status::Done;
                (r.port ? port : game)++;
                (r.port ? portDone : gameDone) += done;
            }
            gap(6);
            const std::string line = trf("Game text: %d of %d translated (%d%%)", gameDone, game, game ? gameDone * 100 / game : 0) + "   |   " +
                                     trf("PC strings: %d of %d (%d%%)", portDone, port, port ? portDone * 100 / port : 0) + "   |   " +
                                     (m_t.gameLanguage ? trf("Saved as changes in %s", utf8(m_t.target()).c_str())
                                                       : trf("Saved to %s", utf8(m_t.target()).c_str()));
            caption(line.c_str(), color::text2);
            const float barW = ImGui::GetContentRegionAvail().x;
            const ImVec2 at = ImGui::GetCursorScreenPos();
            auto* dl = ImGui::GetWindowDrawList();
            const float frac = game + port ? float(gameDone + portDone) / float(game + port) : 0.0f;
            dl->AddRectFilled(at, {at.x + barW, at.y + px(4)}, ink(color::input), px(2));
            dl->AddRectFilled(at, {at.x + barW * frac, at.y + px(4)}, ink(color::success), px(2));
            ImGui::Dummy({barW, px(4)});
        }
        if (m_jobRunning) caption(tr("Finding where each string is used (the game reads every level once; a minute at most)..."), color::muted);
        else if (!m_jobFailed.empty()) caption(tr("The game program could not list the strings: the levels and the PC strings' reference are missing."), color::warning);
        if (!m_status.empty()) caption(m_status.c_str(), color::text2);
        endCard();
    }

    void changeReference() {
        // (a new reference: the rows again, the edits kept)
        std::map<std::string, std::string> edits;
        for (const loc::Row& r : m_t.rows) edits[(r.port ? "port:" : "") + r.id] = r.text;
        auto worlds = m_t.worlds;
        const std::string sel = m_selected >= 0 ? (m_t.rows[std::size_t(m_selected)].port ? "port:" : "") + m_t.rows[std::size_t(m_selected)].id : "";
        openLanguage(m_currentKey);
        for (loc::Row& r : m_t.rows) {
            const auto it = edits.find((r.port ? "port:" : "") + r.id);
            if (it != edits.end()) r.text = it->second;
            m_t.recheck(r);
        }
        m_t.worlds = worlds;
        for (std::size_t k = 0; k < m_t.rows.size(); ++k)
            if ((m_t.rows[k].port ? "port:" : "") + m_t.rows[k].id == sel) select(int(k));
    }

    // ---------------------------------------------------------------- filters and the table
    void drawFilters() {
        const char* const sections[] = {tr("Game text"), tr("PC strings"), tr("Voices")};
        if (segmented("loc-section", &m_section, sections, px(420))) m_dirtyFilter = m_voiceDirty = true;
        ImGui::SameLine(0, px(10));
        if (m_section == 2) {
            const char* const vf[] = {tr("All"), tr("Missing dub"), tr("Has a dub")};
            pushComboStyle();
            ImGui::SetNextItemWidth(px(240));
            if (beginCombo("##loc-vfilter", (std::string(tr("Show:")) + " " + vf[m_voiceFilter]).c_str())) {
                for (int k = 0; k < 3; ++k)
                    if (ImGui::Selectable(vf[k], k == m_voiceFilter)) m_voiceFilter = k, m_voiceDirty = true;
                ImGui::EndCombo();
            }
            popComboStyle();
            ImGui::SameLine(0, px(10));
            if (inputField("##loc-search", m_search, sizeof(m_search), launcher::Charset::Free, tr("Search id or text"))) m_voiceDirty = m_dirtyFilter = true;
            return;
        }
        const char* const filters[] = {tr("All"), tr("Untranslated"), tr("Same as reference"), tr("Problems"), tr("Changed")};
        pushComboStyle();
        ImGui::SetNextItemWidth(px(240));
        if (beginCombo("##loc-filter", (std::string(tr("Show:")) + " " + filters[m_filter]).c_str())) {
            for (int k = 0; k < 5; ++k)
                if (ImGui::Selectable(filters[k], k == m_filter)) m_filter = k, m_dirtyFilter = true;
            ImGui::EndCombo();
        }
        popComboStyle();
        ImGui::SameLine(0, px(10));
        if (inputField("##loc-search", m_search, sizeof(m_search), launcher::Charset::Free, tr("Search id or text"))) m_dirtyFilter = m_voiceDirty = true;
    }
    static std::string lower(std::string s) {
        for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    }
    bool matches(const loc::Row& r) const {
        if (r.port != (m_section == 1)) return false;
        switch (m_filter) {
        case 1: if (loc::Translation::status(r) != loc::Status::Untranslated) return false; break;
        case 2: if (loc::Translation::status(r) != loc::Status::Same) return false; break;
        case 3: if (r.problems.empty()) return false; break;
        case 4: if (!m_t.changed(r)) return false; break;
        default: break;
        }
        if (m_search[0]) {
            const std::string q = lower(m_search);
            if (lower(r.id).find(q) == std::string::npos && lower(r.ref).find(q) == std::string::npos && lower(r.text).find(q) == std::string::npos)
                return false;
        }
        return true;
    }
    void refilter() {
        m_dirtyFilter = false;
        m_visible.clear();
        for (std::size_t i = 0; i < m_t.rows.size(); ++i)
            if (matches(m_t.rows[i]) || int(i) == m_selected) m_visible.push_back(int(i));
    }
    static std::string oneLine(const std::string& s) {
        std::string out;
        out.reserve(std::min<std::size_t>(s.size(), 200));
        for (std::size_t i = 0; i < s.size() && out.size() < 200; ++i) out += s[i] == '\n' ? std::string(" / ") : std::string(1, s[i]);
        return out;
    }
    void drawTable() {
        if (m_dirtyFilter) refilter();
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable |
                                      ImGuiTableFlags_BordersInnerV;
        if (!ImGui::BeginTable("loc-rows", 4, flags, {0, 0})) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Id"), ImGuiTableColumnFlags_WidthFixed, px(m_section == 1 ? 170 : 60));
        ImGui::TableSetupColumn(m_ref.c_str(), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(m_t.folder.key.c_str(), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("##status", ImGuiTableColumnFlags_WidthFixed, px(24));
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(int(m_visible.size()));
        if (m_scrollToSelected) {
            for (std::size_t v = 0; v < m_visible.size(); ++v)
                if (m_visible[v] == m_selected) clipper.IncludeItemByIndex(int(v));
        }
        while (clipper.Step()) {
            for (int v = clipper.DisplayStart; v < clipper.DisplayEnd; ++v) {
                const int i = m_visible[std::size_t(v)];
                const loc::Row& r = m_t.rows[std::size_t(i)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                if (ImGui::Selectable(r.id.c_str(), i == m_selected, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) select(i);
                if (i == m_selected && m_scrollToSelected) {
                    ImGui::SetScrollHereY(0.4f);
                    m_scrollToSelected = false;
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                ImGui::TextColored(vec(color::text2), "%s", oneLine(r.ref).c_str());
                ImGui::TableNextColumn();
                const loc::Status st = loc::Translation::status(r);
                if (st == loc::Status::Untranslated) ImGui::TextColored(vec(color::muted), "%s", tr("(not translated)"));
                else ImGui::TextUnformatted(oneLine(r.text).c_str());
                ImGui::TableNextColumn();
                const ImVec2 c = {ImGui::GetCursorScreenPos().x + px(8), ImGui::GetCursorScreenPos().y + ImGui::GetTextLineHeight() * 0.5f};
                auto* dl = ImGui::GetWindowDrawList();
                const uint32_t col = r.hasError ? color::danger : !r.problems.empty() ? color::warning
                                   : st == loc::Status::Done ? color::success : st == loc::Status::Same ? color::accent : color::border;
                dl->AddCircleFilled(c, px(4.5f), ink(col));
                if (m_t.changed(r)) dl->AddCircle(c, px(7), ink(color::text), 0, px(1.2f));
                ImGui::Dummy({px(16), ImGui::GetTextLineHeight()});
            }
        }
        ImGui::EndTable();
        // (keyboard: Ctrl+Up / Ctrl+Down: the previous / next string of the list)
        if (ImGui::GetIO().KeyCtrl && !m_visible.empty()) {
            const int dir = ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? -1 : 0;
            if (dir) step(dir);
        }
    }
    void select(int i) {
        m_selected = i;
        m_scrollToSelected = true;
        m_addWorld = 0;
    }
    void step(int dir) {
        if (m_dirtyFilter) refilter();
        auto it = std::find(m_visible.begin(), m_visible.end(), m_selected);
        int v = it == m_visible.end() ? (dir > 0 ? 0 : int(m_visible.size()) - 1) : int(it - m_visible.begin()) + dir;
        if (v >= 0 && v < int(m_visible.size())) select(m_visible[std::size_t(v)]);
    }

    // ---------------------------------------------------------------- the selected string
    static int resize(ImGuiInputTextCallbackData* data) {
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            auto* s = static_cast<std::string*>(data->UserData);
            s->resize(std::size_t(data->BufTextLen));
            data->Buf = s->data();
        }
        return 0;
    }
    static bool inputText(const char* id, std::string& s, ImVec2 size) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, vec(color::input));
        const bool changed = ImGui::InputTextMultiline(id, s.data(), s.capacity() + 1, size, ImGuiInputTextFlags_CallbackResize, resize, &s);
        ImGui::PopStyleColor();
        return changed;
    }
    std::string problemText(const loc::Problem& p, const loc::Row& r) const {
        switch (p.issue) {
        case loc::Issue::Codes: return trf("The button icons and codes differ from the reference's (%s).", p.detail.empty() ? "-" : p.detail.c_str());
        case loc::Issue::Formats:
            return r.port ? trf("Keep the %%-codes of the English, in the same order (%s): the game skips this string otherwise.", p.detail.empty() ? "-" : p.detail.c_str())
                          : trf("Keep the %%-codes of the reference, in the same order (%s).", p.detail.empty() ? "-" : p.detail.c_str());
        case loc::Issue::Lines: return trf("The reference has %s lines.", p.detail.c_str());
        case loc::Issue::Spaces: return tr("The spaces or line breaks at the start or the end differ from the reference's.");
        case loc::Issue::Length: return trf("Much longer than the reference (longest line %s characters): it may not fit.", p.detail.c_str());
        case loc::Issue::Glyphs: return trf("The game's font cannot draw: %s", p.detail.c_str());
        case loc::Issue::Empty: break;
        }
        return {};
    }
    void drawDetail() {
        if (m_selected < 0 || m_selected >= int(m_t.rows.size())) {
            caption(tr("Choose a string in the list. Ctrl+Up / Ctrl+Down move through the list, Ctrl+S saves."));
            gap(10);
            overline(tr("Codes"));
            caption(tr("&2X&. draws a button icon (X, O, S, T, L, R, ...) and &. ends it; %s and %d are filled in by the game. Keep them, "
                       "and keep a string's line breaks where the reference has them."),
                    color::text2);
            return;
        }
        loc::Row& r = m_t.rows[std::size_t(m_selected)];
        text(r.port ? (std::string(tr("PC string")) + "  " + r.id).c_str() : trf("String %s", r.id.c_str()).c_str(), fonts().tab);
        // where it is used
        if (!r.port) {
            const auto it = m_usage.strings.find(r.id);
            std::string where;
            if (it != m_usage.strings.end()) {
                for (std::size_t k = 0; k < it->second.size() && k < 12; ++k)
                    if (it->second[k] >= 0 && it->second[k] < int(m_usage.worlds.size())) where += (where.empty() ? "" : ", ") + m_usage.worlds[std::size_t(it->second[k])];
                if (it->second.size() > 12) where += trf(" and %d more", int(it->second.size() - 12));
                caption(trf("In: %s", where.c_str()).c_str(), color::text2);
            } else if (!m_usage.empty()) caption(tr("Not in any level of this installation."), color::muted);
        } else caption(tr("SafeHouse's own text: menus, options, hints and key names of the PC version."), color::text2);
        gap(6);
        text(m_ref.c_str(), fonts().label, color::text2);
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(vec(color::text), "%s", r.ref.empty() ? tr("(none)") : r.ref.c_str());
        ImGui::PopTextWrapPos();
        if (r.port && r.ref != r.english) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(vec(color::muted), "%s: %s", tr("English"), r.english.c_str());
            ImGui::PopTextWrapPos();
        }
        gap(8);
        text(m_t.folder.key.c_str(), fonts().label, color::text2);
        const float boxH = px(110);
        if (inputText("##loc-text", r.text, {-1, boxH})) {
            m_t.recheck(r);
        }
        if (button(tr("Copy the reference"), Tone::Quiet, {0, px(30)}, Icon::Copy)) {
            r.text = r.ref;
            m_t.recheck(r);
        }
        ImGui::SameLine(0, px(6));
        ImGui::BeginDisabled(r.text == r.saved);
        if (button(tr("Undo changes"), Tone::Quiet, {0, px(30)}, Icon::Refresh)) {
            r.text = r.saved;
            m_t.recheck(r);
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0, px(6));
        if (button(tr("Next"), Tone::Quiet, {0, px(30)}, Icon::ChevronRight)) step(1);
        if (m_t.gameLanguage && !r.base.empty() && r.text != r.base) caption(tr("Differs from the languages folder's: saved as a change."), color::text2);
        // checks
        if (!r.problems.empty()) {
            gap(6);
            for (const loc::Problem& p : r.problems) {
                ImGui::PushTextWrapPos(0);
                ImGui::TextColored(vec(p.error ? color::danger : color::warning), "%s %s", p.error ? "x" : "!", problemText(p, r).c_str());
                ImGui::PopTextWrapPos();
            }
        } else if (!r.text.empty()) {
            gap(6);
            ImGui::TextColored(vec(color::success), "%s", tr("No problems found."));
        }
        if (!r.port) drawWorlds(r);
    }

    // a string's own text in one level ("worlds")
    void drawWorlds(loc::Row& r) {
        gap(10);
        overline(tr("Text per level"));
        caption(tr("A level's own text for this string, used there instead of the one above."), color::muted);
        std::vector<std::string> remove;
        for (auto& [world, m] : m_t.worlds) {
            const auto it = m.find(r.id);
            if (it == m.end()) continue;
            ImGui::PushID(world.c_str());
            ImGui::TextColored(vec(color::cta), "%s", world.c_str());
            ImGui::SameLine();
            if (button(tr("Remove##world"), Tone::Quiet, {0, px(26)}, Icon::Trash)) remove.push_back(world);
            std::string s = it->second;
            if (inputText("##w", s, {-1, px(60)})) it->second = s;
            for (const loc::Problem& p : loc::check(r.ref, s, m_t.font))
                if (p.error) ImGui::TextColored(vec(color::danger), "x %s", problemText(p, r).c_str());
            ImGui::PopID();
        }
        for (const std::string& w : remove) {
            m_t.worlds[w].erase(r.id);
            if (m_t.worlds[w].empty()) m_t.worlds.erase(w);
        }
        // add one: the levels that use it (or any level when unknown)
        std::vector<std::string> choices;
        if (const auto it = m_usage.strings.find(r.id); it != m_usage.strings.end())
            for (int w : it->second)
                if (w >= 0 && w < int(m_usage.worlds.size())) choices.push_back(m_usage.worlds[std::size_t(w)]);
        if (choices.empty()) choices = m_usage.worlds;
        choices.erase(std::remove_if(choices.begin(), choices.end(), [&](const std::string& w) { return m_t.worlds.count(w) && m_t.worlds[w].count(r.id); }),
                      choices.end());
        if (choices.empty()) return;
        m_addWorld = std::clamp(m_addWorld, 0, int(choices.size()) - 1);
        pushComboStyle();
        ImGui::SetNextItemWidth(px(220));
        if (beginCombo("##loc-addworld", choices[std::size_t(m_addWorld)].c_str())) {
            for (std::size_t k = 0; k < choices.size(); ++k)
                if (ImGui::Selectable(choices[k].c_str(), int(k) == m_addWorld)) m_addWorld = int(k);
            ImGui::EndCombo();
        }
        popComboStyle();
        ImGui::SameLine(0, px(6));
        if (button(tr("Add text for this level"), Tone::Secondary, {0, 0}, Icon::Plus))
            m_t.worlds[choices[std::size_t(m_addWorld)]][r.id] = r.text.empty() ? r.ref : r.text;
    }

    // ---------------------------------------------------------------- the language's settings (language.json)
    void drawSettings() {
        beginCard("loc-settings", {0, 0}, 20);
        text(tr("Language settings"), fonts().heading);
        caption(trf("language.json of %s: how the game names and draws the language, and what it falls back to.", m_t.folder.key.c_str()).c_str());
        gap(10);
        auto strField = [&](const char* label, const char* key, const char* help) {
            field(label, help);
            char buf[128];
            std::snprintf(buf, sizeof(buf), "%s", m_meta.value(key, std::string()).c_str());
            if (inputField((std::string("##") + key).c_str(), buf, sizeof(buf), launcher::Charset::Free, "", px(320))) m_meta[key] = std::string(buf);
            gap(6);
        };
        strField(tr("Name"), "name", tr("Its name in its own language, on the title's language list."));
        strField(tr("Code"), "code", tr("Its language code (fo, is, pl, ...): picks the decimal mark and the like."));
        field(tr("Letters"), tr("Latin: the USA / European fonts (Western European letters). Japanese: the Japanese disc's font. Korean: the Korean disc's font."));
        const std::string metaScript = m_meta.value("script", std::string("latin"));
        int script = metaScript == "japanese" ? 1 : metaScript == "korean" ? 2 : 0;
        const char* const scripts[] = {tr("Latin"), tr("Japanese"), tr("Korean")};
        if (segmented("loc-script", &script, scripts, px(360))) m_meta["script"] = script == 1 ? "japanese" : script == 2 ? "korean" : "latin";
        gap(6);
        auto fallback = [&](const char* label, const char* key, const char* help, bool voices) {
            field(label, help);
            const std::string cur = m_meta.value(key, std::string());
            pushComboStyle();
            ImGui::SetNextItemWidth(px(320));
            if (beginCombo((std::string("##fb") + key).c_str(), cur.empty() ? tr("(default: English)") : cur.c_str())) {
                if (ImGui::Selectable(tr("(default: English)"), cur.empty())) m_meta.erase(key);
                for (const loc::Folder& f : m_folders) {
                    if (f.key == m_t.folder.key || (voices ? !f.voice : !f.text)) continue;
                    if (ImGui::Selectable(f.key.c_str(), f.key == cur)) m_meta[key] = f.key;
                }
                ImGui::EndCombo();
            }
            popComboStyle();
            gap(6);
        };
        fallback(tr("Voices"), "voice", tr("The voices it plays (a language with its own Dialog lines); its own when it has them."), true);
        fallback(tr("Text it is based on"), "text", tr("The strings it has not translated come from this language, and the PC strings from its built-in ones."), false);
        gap(8);
        if (button(tr("Save settings"), Tone::Cta, {0, 0}, Icon::Check)) {
            std::string error;
            const fs::path dir = m_t.gameLanguage ? m_t.folder.modsDir : m_t.target().parent_path();
            if (loc::saveMeta(dir, m_meta, error)) {
                m_status = trf("Saved %s", utf8(dir / "language.json").c_str());
                m_folders[std::size_t(m_current)].meta = m_meta;
                m_t.font.japanese = m_folders[std::size_t(m_current)].japanese();
                m_t.recheckAll();
                m_settings = false;
            } else m_status = error;
        }
        ImGui::SameLine(0, px(8));
        if (button(tr("Back to the strings"), Tone::Secondary)) m_settings = false;
        endCard();
    }

    // ---------------------------------------------------------------- a new language
    void drawNewLanguage() {
        beginCard("loc-new", {0, 0}, 20);
        text(tr("New language"), fonts().heading);
        caption(trf("A folder in the languages folder (%s) with its language.json and an empty Text.json. Every Sly 2 disc offers it on the "
                    "title's language list once it has text.",
                    utf8(languagesDir()).c_str())
                    .c_str());
        gap(10);
        field(tr("Folder name"), tr("Its name in its own language works best: Føroyskt, Íslenska, Polski."));
        inputField("##loc-newname", m_newName, sizeof(m_newName), launcher::Charset::Free, "", px(320));
        gap(6);
        field(tr("Code"), tr("Its language code (fo, is, pl, ...)."));
        inputField("##loc-newcode", m_newCode, sizeof(m_newCode), launcher::Charset::Free, "", px(160));
        gap(6);
        field(tr("Letters"));
        const char* const scripts[] = {tr("Latin"), tr("Japanese"), tr("Korean")};
        segmented("loc-newscript", &m_newScript, scripts, px(360));
        gap(10);
        if (button(tr("Make the language"), Tone::Cta, {0, 0}, Icon::Plus)) makeLanguage();
        ImGui::SameLine(0, px(8));
        if (button(tr("Cancel"), Tone::Secondary)) m_newLanguage = false;
        endCard();
    }
    void makeLanguage() {
        std::string name = m_newName;
        while (!name.empty() && name.back() == ' ') name.pop_back();
        while (!name.empty() && name.front() == ' ') name.erase(0, 1);
        if (name.empty() || name.find_first_of("/\\:*?\"<>|") != std::string::npos || name[0] == '.') {
            m_status = tr("Choose a folder name without / \\ : * ? \" < > |.");
            return;
        }
        if (folderIndex(name) >= 0) {
            m_status = tr("That language is there already: choose it in the list.");
            return;
        }
        const fs::path dir = languagesDir() / loc::pathOf(name);
        loc::ojson meta = loc::ojson::object();
        meta["name"] = name;
        if (m_newCode[0]) meta["code"] = std::string(m_newCode);
        meta["script"] = m_newScript == 1 ? "japanese" : m_newScript == 2 ? "korean" : "latin";
        std::string error;
        if (!loc::saveMeta(dir, meta, error) ||
            !loc::writeJson(dir / "Text.json", loc::ojson{{"language", name}, {"strings", loc::ojson::object()}}, error)) {
            m_status = error;
            return;
        }
        m_newLanguage = false;
        m_folders = loc::scanFolders(languagesDir(), modsLanguagesDir());
        openLanguage(name);
        m_status = trf("Made %s.", utf8(dir).c_str());
    }

    // ---------------------------------------------------------------- saving, exporting, testing
    bool saveNow() {
        std::string error;
        if (!loc::save(m_t, error)) {
            m_status = error;
            return false;
        }
        m_status = trf("Saved %s", utf8(m_t.target()).c_str());
        m_dirtyFilter = true;
        // (a game language's first change makes its Mods/Languages folder)
        m_folders = loc::scanFolders(languagesDir(), modsLanguagesDir());
        m_current = folderIndex(m_currentKey);
        return true;
    }
    void exportPackage() {
        if (unsaved() && !saveNow()) return;
        const std::string id = cane::mods::idFromName("language-" + m_t.folder.key);
        cane::mods::Manifest m;
        m.id = id;
        m.name = m_t.folder.displayName();
        m.description = trf("The %s translation of Sly 2.", m_t.folder.key.c_str());
        m.game = "sly2";
        m.dir = m_backend.userDir(GameId::Sly2) / "Mods" / cane::mods::kPackagesFolder / loc::pathOf(id);
        std::string error;
        cane::mods::Manifest old;
        if (cane::mods::readManifest(m.dir, old, error)) m.version = old.version, m.author = old.author;
        if (!cane::mods::writeManifest(m, error)) {
            m_status = error;
            return;
        }
        const fs::path dest = m.filesDir() / "Languages" / loc::pathOf(m_t.folder.key);
        std::error_code ec;
        fs::create_directories(dest, ec);
        for (const char* file : {"Text.json", "language.json"}) {
            const fs::path src = m_t.target().parent_path() / file;
            if (fs::is_regular_file(src, ec)) fs::copy_file(src, dest / file, fs::copy_options::overwrite_existing, ec);
            if (ec) break;
        }
        if (ec) m_status = ec.message();
        else m_status = trf("Exported to %s (Mods/Packages: share the folder, or a .zip of it).", utf8(m.dir).c_str());
    }
    fs::path csvPath() const { return m_t.target().parent_path() / loc::pathOf(m_t.folder.key + ".csv"); }
    void exportCsv() {
        std::error_code ec;
        fs::create_directories(csvPath().parent_path(), ec);
        std::ofstream out(csvPath(), std::ios::binary | std::ios::trunc);
        out << loc::toCsv(m_t);
        m_status = out ? trf("Wrote %s (id, reference, translation).", utf8(csvPath()).c_str()) : trf("Could not write %s", utf8(csvPath()).c_str());
    }
    void importCsv() {
        std::ifstream in(csvPath(), std::ios::binary);
        if (!in) {
            m_status = trf("Put the table at %s first (a CSV with the columns id and translation).", utf8(csvPath()).c_str());
            return;
        }
        const std::string csv((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        const int n = loc::importCsv(m_t, csv);
        m_dirtyFilter = true;
        m_status = n < 0 ? trf("%s is not a table with the columns id and translation.", utf8(csvPath()).c_str())
                         : trf("%d strings changed from the table (not saved yet).", n);
    }
    void testInGame() {
        if (unsaved() && !saveNow()) return;
        std::vector<std::string> a = baseArgs();
        // (SLY2_LANGUAGE: the language the game starts in, as Settings.ini's would; the child inherits it)
#ifdef _WIN32
        _putenv_s("SLY2_LANGUAGE", m_t.folder.key.c_str());
#else
        setenv("SLY2_LANGUAGE", m_t.folder.key.c_str(), 1);
#endif
        bool ok = false;
        try {
            ok = cane::editor::spawnDetached(runtime(), a);
        } catch (...) {
        }
#ifdef _WIN32
        _putenv_s("SLY2_LANGUAGE", "");
#else
        unsetenv("SLY2_LANGUAGE");
#endif
        m_status = ok ? trf("Sly 2 is starting in %s.", m_t.folder.key.c_str()) : std::string(trf("%s did not start.", "Sly 2"));
    }

    // ---------------------------------------------------------------- the voices (dubbing)
    fs::path dubDir() const { return m_t.gameLanguage ? m_t.folder.modsDir : m_t.target().parent_path(); }
    void scanVoices() {
        // (the voices beside: the reference's when it has its own, else English)
        const int r = folderIndex(m_ref), en = folderIndex("English");
        const loc::Folder* ref = r >= 0 && m_folders[std::size_t(r)].voice ? &m_folders[std::size_t(r)] : en >= 0 ? &m_folders[std::size_t(en)] : nullptr;
        m_voiceRefKey = ref ? ref->key : std::string();
        const loc::VoiceFiles refFiles = ref ? loc::scanVoices(ref->dirs) : loc::VoiceFiles();
        std::vector<fs::path> dirs = m_t.folder.dirs;
        const loc::VoiceFiles mine = loc::scanVoices(dirs);
        m_lengths = mine.lengths;
        const std::string keep = m_voiceSel >= 0 && m_voiceSel < int(m_voices.size()) ? m_voices[std::size_t(m_voiceSel)].id : std::string();
        m_voices = ref && ref->key == m_t.folder.key ? std::vector<loc::VoiceRow>() : loc::voiceRows(refFiles, mine);
        m_voiceSel = -1;
        for (std::size_t k = 0; k < m_voices.size(); ++k)
            if (m_voices[k].id == keep) m_voiceSel = int(k);
        m_voiceDirty = true;
    }
    const loc::Row* subtitleRow(const loc::VoiceRow& v) const {
        if (v.movie) return nullptr;
        const auto it = m_rowById.find(v.id);
        return it == m_rowById.end() ? nullptr : &m_t.rows[std::size_t(it->second)];
    }
    bool voiceMatches(const loc::VoiceRow& v) const {
        if (m_voiceFilter == 1 && !v.dub.empty()) return false;
        if (m_voiceFilter == 2 && v.dub.empty()) return false;
        if (m_search[0]) {
            const std::string q = lower(m_search);
            const loc::Row* r = subtitleRow(v);
            if (lower(v.id).find(q) == std::string::npos && lower(v.group).find(q) == std::string::npos &&
                (!r || (lower(r->ref).find(q) == std::string::npos && lower(r->text).find(q) == std::string::npos)))
                return false;
        }
        return true;
    }
    void drawVoiceTable() {
        if (m_voiceDirty) {
            m_voiceDirty = false;
            m_voiceVisible.clear();
            for (std::size_t i = 0; i < m_voices.size(); ++i)
                if (voiceMatches(m_voices[i]) || int(i) == m_voiceSel) m_voiceVisible.push_back(int(i));
        }
        int lines = 0, dubbed = 0;
        for (const loc::VoiceRow& v : m_voices) lines += !v.movie, dubbed += !v.movie && !v.dub.empty();
        if (m_voices.empty()) {
            caption(m_voiceRefKey.empty() ? tr("No voice lines to compare with: the languages folder has no English Dialog lines (install Sly 2 again).")
                                          : tr("Choose another language beside this one: it is the same language."),
                    color::text2);
            return;
        }
        caption((trf("Dubbed lines: %d of %d (%d%%)", dubbed, lines, lines ? dubbed * 100 / lines : 0) + "   |   " +
                 trf("Beside: %s's voices", m_voiceRefKey.c_str()))
                    .c_str(),
                color::text2);
        const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_Resizable |
                                      ImGuiTableFlags_BordersInnerV;
        if (!ImGui::BeginTable("loc-voices", 4, flags, {0, 0})) return;
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn(tr("Line"), ImGuiTableColumnFlags_WidthFixed, px(130));
        ImGui::TableSetupColumn(tr("Subtitle"), ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn(tr("Place"), ImGuiTableColumnFlags_WidthFixed, px(130));
        ImGui::TableSetupColumn(tr("Dub"), ImGuiTableColumnFlags_WidthFixed, px(70));
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(int(m_voiceVisible.size()));
        if (m_voiceScroll)
            for (std::size_t k = 0; k < m_voiceVisible.size(); ++k)
                if (m_voiceVisible[k] == m_voiceSel) clipper.IncludeItemByIndex(int(k));
        while (clipper.Step()) {
            for (int k = clipper.DisplayStart; k < clipper.DisplayEnd; ++k) {
                const int i = m_voiceVisible[std::size_t(k)];
                const loc::VoiceRow& v = m_voices[std::size_t(i)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                if (ImGui::Selectable(v.movie ? (std::string(tr("Movie")) + ": " + v.id).c_str() : v.id.c_str(), i == m_voiceSel,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                    m_voiceSel = i;
                    m_dubFile[0] = 0;
                    m_lengthEdit = 0;
                }
                if (i == m_voiceSel && m_voiceScroll) {
                    ImGui::SetScrollHereY(0.4f);
                    m_voiceScroll = false;
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                const loc::Row* r = subtitleRow(v);
                if (r) ImGui::TextUnformatted(oneLine(r->text.empty() ? r->ref : r->text).c_str());
                else ImGui::TextColored(vec(color::muted), "%s", v.movie ? tr("(a cutscene's sound)") : tr("(no subtitle)"));
                ImGui::TableNextColumn();
                ImGui::TextColored(vec(color::text2), "%s", v.group.c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(vec(v.dub.empty() ? color::muted : color::success), "%s", v.dub.empty() ? tr("missing") : tr("yes"));
            }
        }
        ImGui::EndTable();
        if (ImGui::GetIO().KeyCtrl && !m_voiceVisible.empty()) {
            const int dir = ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? -1 : 0;
            if (dir) {
                auto it = std::find(m_voiceVisible.begin(), m_voiceVisible.end(), m_voiceSel);
                const int k = it == m_voiceVisible.end() ? 0 : int(it - m_voiceVisible.begin()) + dir;
                if (k >= 0 && k < int(m_voiceVisible.size())) m_voiceSel = m_voiceVisible[std::size_t(k)], m_voiceScroll = true, m_dubFile[0] = 0, m_lengthEdit = 0;
            }
        }
    }
    static std::string seconds(double s) {
        if (s < 0) return "?";
        char b[32];
        std::snprintf(b, sizeof(b), "%.2f s", s);
        return b;
    }
    // Plays a voice file (WAV as it is; FLAC, VAG or other WAVs through the game program: --decode-audio).
    void play(const fs::path& file) {
        if (!m_audio) {
            m_audio = std::make_unique<collection::Audio>();
            if (!m_audio->open()) m_status = tr("No sound output (SDL2 or an audio device is missing).");
        }
        if (!m_audio->available()) return;
        collection::PcmPtr pcm;
        if (auto it = m_pcm.find(utf8(file)); it != m_pcm.end()) pcm = it->second;
        else {
            auto readAll = [](const fs::path& f) {
                std::ifstream in(f, std::ios::binary);
                return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
            };
            collection::Pcm p = collection::decodeWav(readAll(file));
            if (p.samples.empty()) {
                const fs::path tmp = cacheDir() / "play.wav";
                std::error_code ec;
                fs::create_directories(tmp.parent_path(), ec);
                std::atomic<bool> cancel{false};
                try {
                    if (cane::runProcess({utf8(runtime()), "--decode-audio", utf8(file), utf8(tmp)}, runtime().parent_path(), cancel,
                                         [](const std::string&) {}) == 0)
                        p = collection::decodeWav(readAll(tmp));
                } catch (...) {
                }
            }
            if (p.samples.empty()) {
                m_status = trf("Cannot play %s", utf8(file).c_str());
                return;
            }
            p.loops = false;
            p.loopStart = 0;
            p.loopEnd = p.frames();
            pcm = std::make_shared<const collection::Pcm>(std::move(p));
            if (m_pcm.size() > 32) m_pcm.clear();
            m_pcm[utf8(file)] = pcm;
        }
        m_audio->playMusic(nullptr, 0.05f);
        m_audio->playSound(pcm);
    }
    void stopSound() {
        if (m_audio) m_audio = nullptr;   // (closing the device stops every voice)
    }
    void drawVoiceDetail() {
        if (m_voiceSel < 0 || m_voiceSel >= int(m_voices.size())) {
            caption(tr("Choose a voice line. Each line is named by the number of its subtitle; a dub is a WAV or FLAC file of that name in the "
                       "language's Dialog folder (any subfolder), a cutscene's sound one in its Movies folder."));
            return;
        }
        loc::VoiceRow& v = m_voices[std::size_t(m_voiceSel)];
        if (v.refSeconds < -1 && !v.ref.empty()) v.refSeconds = loc::audioSeconds(v.ref);
        if (v.dubSeconds < -1 && !v.dub.empty()) v.dubSeconds = loc::audioSeconds(v.dub);
        text(v.movie ? (std::string(tr("Movie")) + "  " + v.id).c_str() : trf("Line %s", v.id.c_str()).c_str(), fonts().tab);
        if (!v.group.empty()) caption(v.group.c_str(), color::text2);
        gap(6);
        // the reference's line
        text(m_voiceRefKey.c_str(), fonts().label, color::text2);
        ImGui::BeginDisabled(v.ref.empty());
        if (button(tr("Play##ref"), Tone::Secondary, {0, px(32)}, Icon::Play)) play(v.ref);
        ImGui::EndDisabled();
        ImGui::SameLine(0, px(8));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(vec(color::text2), "%s", v.ref.empty() ? tr("(none)") : seconds(v.refSeconds).c_str());
        if (const loc::Row* r = subtitleRow(v)) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(vec(color::text), "%s", r->ref.c_str());
            ImGui::PopTextWrapPos();
        }
        gap(8);
        // the language's dub
        text(m_t.folder.key.c_str(), fonts().label, color::text2);
        ImGui::BeginDisabled(v.dub.empty());
        if (button(tr("Play##dub"), Tone::Secondary, {0, px(32)}, Icon::Play)) play(v.dub);
        ImGui::EndDisabled();
        ImGui::SameLine(0, px(8));
        if (button(tr("Stop"), Tone::Quiet, {0, px(32)}, Icon::Stop)) stopSound();
        ImGui::SameLine(0, px(8));
        ImGui::AlignTextToFramePadding();
        if (v.dub.empty()) ImGui::TextColored(vec(color::warning), "%s", tr("No dub yet: the game plays the voices it falls back to."));
        else {
            const bool longer = v.refSeconds > 0 && v.dubSeconds > v.refSeconds * 1.25 + 0.3;
            ImGui::TextColored(vec(longer ? color::warning : color::text2), "%s", seconds(v.dubSeconds).c_str());
            if (longer && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tr("Much longer than the reference's line: a cutscene may cut it off."));
        }
        if (const loc::Row* r = subtitleRow(v)) {
            ImGui::PushTextWrapPos(0);
            if (r->text.empty()) ImGui::TextColored(vec(color::muted), "%s", tr("(not translated)"));
            else ImGui::TextUnformatted(r->text.c_str());
            ImGui::PopTextWrapPos();
            if (button(tr("Edit the subtitle"), Tone::Quiet, {0, px(30)}, Icon::ChevronRight)) {
                m_section = 0;
                m_dirtyFilter = true;
                select(m_rowById.at(v.id));
            }
        }
        if (!v.dub.empty()) caption(utf8(v.dub).c_str(), color::muted);
        // a new dub: a file copied in under the line's name
        gap(10);
        overline(v.dub.empty() ? tr("Add a dub") : tr("Replace the dub"));
        inputField("##loc-dubfile", m_dubFile, sizeof(m_dubFile), launcher::Charset::Free, tr("Path of a WAV or FLAC file"));
        if (button(tr("Use this file"), Tone::Secondary, {0, 0}, Icon::Plus)) useDub(v);
        // the subtitle's time (Dialog/lengths.json)
        if (!v.movie) {
            gap(10);
            overline(tr("Subtitle time"));
            const auto it = m_lengths.find(v.id);
            caption(it != m_lengths.end() ? trf("%.2f s (Dialog/lengths.json)", it->second).c_str()
                                          : tr("The dub's length (no Dialog/lengths.json entry)."),
                    color::text2);
            if (m_lengthEdit <= 0) m_lengthEdit = float(it != m_lengths.end() ? it->second : v.dubSeconds > 0 ? v.dubSeconds : std::max(0.0, v.refSeconds));
            ImGui::SetNextItemWidth(px(140));
            ImGui::InputFloat("##loc-len", &m_lengthEdit, 0.1f, 1.0f, "%.2f");
            ImGui::SameLine(0, px(8));
            if (button(tr("Set"), Tone::Quiet, {0, px(32)})) setLength(v.id, m_lengthEdit);
            ImGui::SameLine(0, px(6));
            ImGui::BeginDisabled(it == m_lengths.end());
            if (button(tr("Clear##len"), Tone::Quiet, {0, px(32)})) setLength(v.id, -1);
            ImGui::EndDisabled();
        }
    }
    void useDub(loc::VoiceRow& v) {
        std::string src = m_dubFile;
        while (!src.empty() && (src.back() == ' ' || src.back() == '\n')) src.pop_back();
        if (src.size() > 7 && src.rfind("file://", 0) == 0) src = src.substr(7);
        if (src.size() > 1 && src.front() == '"' && src.back() == '"') src = src.substr(1, src.size() - 2);
        const fs::path file = cane::path(src);
        std::error_code ec;
        std::string ext = utf8(file.extension());
        for (char& c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (!fs::is_regular_file(file, ec) || (ext != ".wav" && ext != ".flac")) {
            m_status = tr("Choose an existing .wav or .flac file.");
            return;
        }
        if (loc::audioSeconds(file) < 0) {
            m_status = tr("That file is not a WAV or FLAC sound.");
            return;
        }
        const fs::path dest = loc::dubPath(dubDir(), v, ext);
        fs::create_directories(dest.parent_path(), ec);
        // (one file per line: the other format's copy here goes)
        for (const char* other : {".wav", ".flac", ".vag"})
            if (ext != other) fs::remove(loc::dubPath(dubDir(), v, other), ec);
        fs::copy_file(file, dest, fs::copy_options::overwrite_existing, ec);
        if (ec) {
            m_status = ec.message();
            return;
        }
        m_pcm.erase(utf8(dest));
        m_dubFile[0] = 0;
        m_folders = loc::scanFolders(languagesDir(), modsLanguagesDir());
        m_current = folderIndex(m_currentKey);
        m_t.folder = m_folders[std::size_t(m_current)];
        scanVoices();
        m_status = trf("Copied to %s", utf8(dest).c_str());
    }
    void setLength(const std::string& id, float seconds) {
        const fs::path file = dubDir() / "Dialog" / "lengths.json";
        loc::ojson j = loc::readJson(file);
        if (!j.is_object()) j = loc::ojson::object();
        if (seconds > 0) j[id] = std::round(double(seconds) * 1000.0) / 1000.0;
        else j.erase(id);
        std::string error;
        if (!loc::writeJson(file, j, error)) {
            m_status = error;
            return;
        }
        if (seconds > 0) m_lengths[id] = j[id].get<double>();
        else m_lengths.erase(id);
        m_lengthEdit = 0;
        m_status = trf("Saved %s", utf8(file).c_str());
    }

    LauncherBackend& m_backend;
    bool m_loaded = false;
    std::string m_error, m_status;
    std::vector<loc::Folder> m_folders;
    int m_current = -1;
    std::string m_currentKey, m_ref;
    loc::Translation m_t;
    std::set<char32_t> m_fontChars;
    loc::Usage m_usage;
    loc::ojson m_meta;
    // the list
    int m_section = 0, m_filter = 0, m_selected = -1, m_addWorld = 0;
    bool m_dirtyFilter = true, m_scrollToSelected = false;
    std::vector<int> m_visible;
    char m_search[128] = "";
    // pages over the list
    bool m_settings = false, m_newLanguage = false;
    char m_newName[96] = "", m_newCode[16] = "";
    int m_newScript = 0;
    float m_refComboRight = 0;
    // the voices
    std::vector<loc::VoiceRow> m_voices;
    std::vector<int> m_voiceVisible;
    std::map<std::string, int> m_rowById;
    std::map<std::string, double> m_lengths;
    std::string m_voiceRefKey;
    int m_voiceSel = -1, m_voiceFilter = 0;
    bool m_voiceDirty = true, m_voiceScroll = false;
    char m_dubFile[512] = "";
    float m_lengthEdit = 0;
    std::unique_ptr<collection::Audio> m_audio;
    std::map<std::string, collection::PcmPtr> m_pcm;
    // the background job
    std::thread m_job;
    std::atomic<bool> m_jobCancel{false}, m_jobDone{false}, m_portDone{false};
    bool m_jobRunning = false, m_jobUsage = false;
    std::string m_jobFailed, m_jobFailedThread;
};

}  // namespace cane::ui
