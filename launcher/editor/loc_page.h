#pragma once
// The launcher's Localisation page (desktop only, beside the Map Editor: CANE_LAUNCHER_EDITOR): make and keep up a Sly 2
// translation. A language of the shared languages folder (or the mods folder's Mods/Languages) is shown string by
// string beside a reference language (English by default): the game's text by id and the port's own PC strings in their
// own section, with filters, search, each string's levels and its per-level texts, checks (icons, %-codes, line breaks,
// length, the font's letters) and progress; saved to its Text.json, exported as a mod package or a CSV table. The model:
// loc_core.h. The page speaks the launcher's languages (launcher/editor/lang). Docs: docs/MODDING.md "Translating".
//
// Round 3: the lists grouped by level (the episode whose worlds use a string) and by speaker (the dialog tables' speaker
// of each line), the game's button codes drawn as icons (with a palette that types them), the radio effect for dubs of
// the lines the game plays as radio talk (loc_audio.h), and recording a dub here: a microphone, a countdown, the
// reference playing along, then trimming the take.
//
// It runs the game program twice, in the background, into the launcher's cache folder: --dump-port-text (the port's
// built-in strings of the 12 languages: the PC strings' reference) and --dump-text (each string's levels: usage.json).
#include "../backend.h"
#include "../ui/widgets.h"

#include "../collection/audio.h"
#include "../ui/prompt_bar.h"
#include "audio_devices_sdl.h"
#include "core/spawn.h"
#include "loc_core.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
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

    // The recording path on the fake microphone (part of safehouse --loc-selftest): a made-up take as
    // SAFEHOUSE_FAKE_MIC, recorded to the reference's length and the margin, trimmed to its speech, given the radio
    // effect and written as a dub would be. 0 when all pass.
    static int recordSelfTest() {
        int fails = 0;
        auto expect = [&](bool ok, const char* what) {
            if (!ok) ++fails, std::printf("FAIL %s\n", what);
        };
        std::error_code ec;
        const fs::path dir = fs::temp_directory_path(ec) / ("safehouse-loc-" + std::to_string(uint64_t(std::chrono::steady_clock::now().time_since_epoch().count())));
        fs::create_directories(dir, ec);
        loc::audio::Mono take;
        take.rate = 22050;
        take.s.assign(std::size_t(take.rate * 2), 0.0f);
        for (std::size_t i = std::size_t(take.rate / 2); i < std::size_t(take.rate * 3 / 2); ++i)
            take.s[i] = float(0.3 * std::sin(double(i) * 0.07) + 0.1 * std::sin(double(i) * 0.9));
        std::string error;
        expect(loc::audio::writeWav(dir / "take.wav", take, error), "the fake microphone's file written");
        const char* before = std::getenv("SAFEHOUSE_FAKE_MIC");
        const std::string keep = before ? before : "";
#ifdef _WIN32
        _putenv_s("SAFEHOUSE_FAKE_MIC", utf8(dir / "take.wav").c_str());
#else
        setenv("SAFEHOUSE_FAKE_MIC", utf8(dir / "take.wav").c_str(), 1);
#endif
        collection::Capture mic;
        expect(mic.open("any device") && mic.fake() && mic.rate() == 22050, "the fake microphone opens at its file's rate");
        loc::audio::Recorder rec;
        rec.reset(mic.rate(), 1.2 + 1.0);   // (a reference of 1.2 s and the margin)
        const auto first = mic.takeFake(0.25);
        rec.push(first.data(), first.size());
        expect(first.size() == std::size_t(22050 / 4), "the fake microphone gives its samples as the time passes");
        const auto rest = mic.takeFake(3.0);
        rec.push(rest.data(), rest.size());
        expect(rec.full() && std::abs(rec.seconds() - 2.2) < 0.001, "the recording stops at the reference's length and the margin");
        mic.close();
        loc::audio::Mono got;
        got.rate = rec.rate;
        got.s = rec.s;
        const auto span = loc::audio::speechSpan(got);
        expect(span.first > 0.4 && span.first < 0.5 && span.second > 1.5 && span.second < 1.6, "the trim handles start around the speech");
        const loc::audio::Mono cut = loc::audio::trimmed(got, span.first, span.second);
        loc::audio::RadioFilter bp;
        for (double c : loc::audio::bandCentres()) bp.gainDb.push_back(c < 400 ? -30 : c > 3150 ? -24 : 0);
        const loc::audio::Mono radio = loc::audio::applyRadio(cut, bp);
        const fs::path dub = dir / "Dialog" / "Cairo Museum" / "1179.wav";
        expect(loc::audio::writeWav(dub, radio, error) && loc::audio::writeWav(loc::takePath(dir, "1179"), cut, error), "the dub and its dry take written");
        const loc::audio::Mono back = loc::audio::readWav(dub);
        expect(back.rate == 22050 && back.s.size() == cut.s.size() && std::abs(loc::audioSeconds(dub) - cut.seconds()) < 0.01, "the dub reads back: 16-bit mono, the trimmed length");
        expect(loc::saveTakeChoice(dir, "1179", false, error) && loc::readTakeChoices(dir).at("1179") == false, "a line's radio choice kept in Takes/takes.json");
#ifdef _WIN32
        _putenv_s("SAFEHOUSE_FAKE_MIC", keep.c_str());
#else
        if (before) setenv("SAFEHOUSE_FAKE_MIC", keep.c_str(), 1);
        else unsetenv("SAFEHOUSE_FAKE_MIC");
#endif
        fs::remove_all(dir, ec);
        std::printf("loc record selftest: %d failed\n", fails);
        return fails ? 1 : 0;
    }
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
        // (screenshots: SAFEHOUSE_LOC_DETAIL_SCROLL=<pixels> holds the detail card scrolled there)
        if (const char* sz = std::getenv("SAFEHOUSE_LOC_DETAIL_SCROLL"); sz && *sz) ImGui::SetScrollY(px(float(std::atof(sz))));
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

    // ---------------------------------------------------------------- button codes drawn as icons
    // The icons' family: the launcher's button icon setting when a style is chosen there (Settings > Button icons), else
    // the PlayStation's, the buttons the game's text names.
    static input::Family iconFamily() {
        const auto& p = launcher::detail::promptPreferences();
        if (p.alwaysOriginalPsPrompts || p.promptStyle == 0) return input::Family::PlayStation;
        const input::Family f = input::promptFamily(p.promptStyle, 0);
        return f == input::Family::Keyboard ? input::Family::PlayStation : f;
    }
    // An icon font character as a pad button (Sly2-Release/promptglyph.h), Button::Count for the others
    static input::Button buttonOf(const std::string& c) {
        using B = input::Button;
        if (c.size() != 1) return B::Count;
        switch (c[0]) {
        case 'X': return B::South;
        case 'O': return B::East;
        case 'S': return B::West;
        case 'T': return B::North;
        case 'L': return B::LB;
        case 'R': return B::RB;
        case 'l': return B::LT;
        case 'r': return B::RT;
        case '(': return B::LS;
        case ')': return B::RS;
        case '+': return B::Dpad;
        case '!': return B::Start;
        case '_': return B::Back;
        default: return B::Count;
        }
    }
    // (the icon font's two pictures that are not buttons)
    static const char* pictureName(const std::string& c) {
        if (c == "a") return tr("save icon");
        if (c == "w") return tr("exit door icon");
        return nullptr;
    }
    static float iconWidth(const std::string& c, float h) {
        const input::Button b = buttonOf(c);
        if (b != input::Button::Count) return padButtonWidth(h, iconFamily(), b);
        const char* name = pictureName(c);
        return ImGui::CalcTextSize(name ? name : c.c_str()).x + px(10);
    }
    static void drawIcon(ImDrawList* dl, ImVec2 min, float h, const std::string& c) {
        const input::Button b = buttonOf(c);
        if (b != input::Button::Count) {
            launcher::detail::launcherAtlas();
            const float w = padButtonWidth(h, iconFamily(), b);
            drawPadButton(dl, {min.x + w * 0.5f, min.y + h * 0.5f}, h, iconFamily(), b);
            return;
        }
        const char* name = pictureName(c);
        const std::string label = name ? name : c;
        const ImVec2 t = ImGui::CalcTextSize(label.c_str());
        const float w = t.x + px(10);
        dl->AddRect(min, {min.x + w, min.y + h}, ink(color::borderStrong), px(4), 0, px(1));
        dl->AddText({min.x + px(5), min.y + (h - t.y) * 0.5f}, ink(color::text2), label.c_str());
    }
    // A text with its button codes as icons, wrapped to the available width (an item, like TextWrapped)
    static void richText(const std::string& s, uint32_t rgb, bool oneLine = false) {
        const float lineH = ImGui::GetTextLineHeight(), iconH = std::round(lineH * (oneLine ? 1.0f : 1.25f));
        const float rowH = std::max(lineH, iconH) + (oneLine ? 0 : px(2));
        const float wrap = oneLine ? 1e9f : std::max(px(60), ImGui::GetContentRegionAvail().x);
        struct Put { ImVec2 at; bool icon; std::string s; };
        std::vector<Put> puts;
        float x = 0, y = 0, widest = 0;
        auto newline = [&] { widest = std::max(widest, x); x = 0; y += rowH; };
        auto place = [&](float w, bool icon, const std::string& str) {
            if (x > 0 && x + w > wrap && !(str.size() == 1 && str[0] == ' ')) newline();
            if (x == 0 && !icon && str == " ") return;   // (no space at a wrapped line's start)
            puts.push_back({{x, y + (icon ? (rowH - iconH) * 0.5f : (rowH - lineH) * 0.5f)}, icon, str});
            x += w;
        };
        for (const loc::Piece& p : loc::pieces(s)) {
            if (p.icon) {
                place(iconWidth(p.text, iconH) + px(2), true, p.text);
                continue;
            }
            // words (and the spaces between) so a line can wrap between them
            std::string word;
            auto flush = [&] {
                if (!word.empty()) place(ImGui::CalcTextSize(word.c_str()).x, false, word), word.clear();
            };
            for (char c : p.text) {
                if (c == '\n') {
                    flush();
                    if (oneLine) place(ImGui::CalcTextSize(" / ").x, false, " / ");
                    else newline();
                } else if (c == ' ') {
                    flush();
                    place(ImGui::CalcTextSize(" ").x, false, " ");
                } else word += c;
            }
            flush();
        }
        widest = std::max(widest, x);
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        ImGui::Dummy({std::min(std::max(widest, 1.0f), oneLine ? ImGui::GetContentRegionAvail().x : wrap), y + rowH});
        auto* dl = ImGui::GetWindowDrawList();
        for (const Put& put : puts) {
            const ImVec2 at = {std::round(origin.x + put.at.x), std::round(origin.y + put.at.y)};
            if (put.icon) drawIcon(dl, {at.x + px(1), at.y}, iconH, put.s);
            else dl->AddText(at, ink(rgb), put.s.c_str());
        }
    }

    // ---------------------------------------------------------------- loading
    void load() {
        m_loaded = true;
        m_error.clear();
        m_folders = loc::scanFolders(languagesDir(), modsLanguagesDir());
        std::error_code ec;
        const bool portReady = fs::is_regular_file(portDir() / "English.json", ec) && !runtimeNewer(portDir() / "English.json");
        const bool usageReady = fs::is_regular_file(usageDir() / "usage.json", ec);
        if (usageReady) m_usage = loc::readUsage(usageDir() / "usage.json");
        // (a usage.json from before round 3 has no episodes or speakers: made again once)
        const bool usageOld = usageReady && m_usage.version < 2 && !m_usageRetried;
        if (usageOld) m_usageRetried = true;
        if (!portReady || !usageReady || usageOld) startJob(!portReady, !usageReady || usageOld);
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
            const char* const vf[] = {tr("All"), tr("Missing dub"), tr("Has a dub"), tr("Radio lines")};
            pushComboStyle();
            ImGui::SetNextItemWidth(px(240));
            if (beginCombo("##loc-vfilter", (std::string(tr("Show:")) + " " + vf[m_voiceFilter]).c_str())) {
                for (int k = 0; k < 4; ++k)
                    if (ImGui::Selectable(vf[k], k == m_voiceFilter)) m_voiceFilter = k, m_voiceDirty = true;
                ImGui::EndCombo();
            }
            popComboStyle();
            ImGui::SameLine(0, px(10));
            drawGroupBy();
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
        if (m_section == 0) {
            ImGui::SameLine(0, px(10));
            drawGroupBy();
        }
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
        std::vector<int> rows;
        for (std::size_t i = 0; i < m_t.rows.size(); ++i)
            if (matches(m_t.rows[i]) || int(i) == m_selected) rows.push_back(int(i));
        // (the PC strings are not in any level: never grouped)
        m_lines = grouped(rows, m_section == 1 ? 0 : m_groupBy, false);
        m_visible.clear();
        for (const ListLine& l : m_lines)
            if (l.row >= 0) m_visible.push_back(l.row);
    }

    // ---------------------------------------------------------------- groups: levels, then speakers
    static constexpr int kLevelCutscenes = 103;
    struct ListLine {
        int row = -1;            // a row (of m_t.rows or m_voices), or -1: a group's heading
        int depth = 0;           // (a heading) 0 the level, 1 the speaker
        std::string key, label;
        int count = 0, done = 0;
        bool collapsed = false;
    };
    std::string levelLabel(int level) const {
        if (level == loc::kLevelMenus) return tr("Menus");
        if (level == loc::kLevelShared) return tr("Shared: in several levels");
        if (level == loc::kLevelNone) return m_usage.empty() ? tr("Levels not known yet") : tr("Not in any level");
        if (level == kLevelCutscenes) return tr("Cutscenes");
        const std::string ep = level == 0 ? std::string(tr("Prologue")) : trf("Episode %d", level);
        const std::string hub = loc::episodeHub(m_usage, level);
        return hub.empty() ? ep : ep + ": " + hub;
    }
    // A row's speaker group (rank, name): the dialog tables' speaker; a string without a voice line is on-screen text
    std::pair<int, std::string> speakerOf(const std::string& id, bool voices) const {
        const auto it = m_usage.dialogs.find(id);
        if (it == m_usage.dialogs.end()) return voices ? std::pair<int, std::string>{100, tr("Other")} : std::pair<int, std::string>{200, tr("On-screen text")};
        const char* name = loc::speakerName(it->second.speaker);
        return name ? std::pair<int, std::string>{loc::speakerRank(it->second.speaker), name} : std::pair<int, std::string>{100, tr("Other")};
    }
    // The rows (in their order) under their headings: by level (by = 1), by level and speaker (2), or none (0)
    std::vector<ListLine> grouped(const std::vector<int>& rows, int by, bool voices) const {
        std::vector<ListLine> out;
        if (by == 0) {
            for (int r : rows) out.push_back({r});
            return out;
        }
        using Key = std::pair<int, std::pair<int, std::string>>;
        std::map<Key, std::vector<int>> groups;
        for (int r : rows) {
            std::string id;
            bool movie = false, done = false;
            if (voices) id = m_voices[std::size_t(r)].id, movie = m_voices[std::size_t(r)].movie;
            else id = m_t.rows[std::size_t(r)].id;
            (void)done;
            const int level = movie ? kLevelCutscenes : loc::levelOf(m_usage, id);
            groups[{level, by == 2 && !movie ? speakerOf(id, voices) : std::pair<int, std::string>{0, ""}}].push_back(r);
        }
        auto isDone = [&](int r) {
            return voices ? !m_voices[std::size_t(r)].dub.empty() : loc::Translation::status(m_t.rows[std::size_t(r)]) == loc::Status::Done;
        };
        int level = -999;
        bool levelCollapsed = false;
        for (const auto& [key, list] : groups) {
            if (key.first != level) {
                level = key.first;
                ListLine h;
                h.key = (voices ? "v" : "t") + std::to_string(level);
                h.label = levelLabel(level);
                for (const auto& [k2, l2] : groups)
                    if (k2.first == level)
                        for (int r : l2) ++h.count, h.done += isDone(r);
                h.collapsed = levelCollapsed = m_collapsed.count(h.key) > 0;
                out.push_back(h);
            }
            if (levelCollapsed) continue;
            bool collapsed = false;
            if (!key.second.second.empty()) {
                ListLine h;
                h.depth = 1;
                h.key = (voices ? "v" : "t") + std::to_string(level) + "/" + key.second.second;
                h.label = key.second.second;
                for (int r : list) ++h.count, h.done += isDone(r);
                h.collapsed = collapsed = m_collapsed.count(h.key) > 0;
                out.push_back(h);
            }
            if (!collapsed)
                for (int r : list) out.push_back({r});
        }
        return out;
    }
    // A heading's row of a table: its arrow, name and counts (done / all: translated or dubbed); a click folds it
    bool drawHeading(const ListLine& l) {
        ImGui::TableNextRow(0, ImGui::GetTextLineHeight() + px(8));
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, ink(l.depth == 0 ? color::input : color::border, l.depth == 0 ? 1.0f : 0.45f));
        ImGui::TableNextColumn();
        ImGui::PushID(l.key.c_str());
        const bool clicked = ImGui::Selectable("##heading", false, ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap,
                                               {0, ImGui::GetTextLineHeight() + px(4)});
        ImGui::PopID();
        auto* dl = ImGui::GetWindowDrawList();
        const ImVec2 at = ImGui::GetItemRectMin();
        const float indent = px(4 + 16 * l.depth), h = ImGui::GetItemRectSize().y;
        ImGui::RenderArrow(dl, {at.x + indent, at.y + (h - ImGui::GetFontSize()) * 0.5f}, ink(color::text2), l.collapsed ? ImGuiDir_Right : ImGuiDir_Down, 0.8f);
        const std::string text = l.label + "   " + std::to_string(l.done) + " / " + std::to_string(l.count);
        // (across the whole row, not only the first column)
        const ImGuiTable* table = ImGui::GetCurrentTable();
        dl->PushClipRect({table->InnerClipRect.Min.x, at.y}, {table->InnerClipRect.Max.x, at.y + h}, false);
        dl->AddText({at.x + indent + px(18), at.y + (h - ImGui::GetTextLineHeight()) * 0.5f}, ink(l.depth == 0 ? color::text : color::text2), text.c_str());
        dl->PopClipRect();
        return clicked;
    }
    void toggleGroup(const std::string& key) {
        if (!m_collapsed.erase(key)) m_collapsed.insert(key);
        m_dirtyFilter = m_voiceDirty = true;
    }
    void drawGroupBy() {
        const char* const by[] = {tr("No groups"), tr("By level"), tr("By level and character")};
        pushComboStyle();
        ImGui::SetNextItemWidth(px(250));
        if (beginCombo("##loc-groupby", by[m_groupBy])) {
            for (int k = 0; k < 3; ++k)
                if (ImGui::Selectable(by[k], k == m_groupBy)) m_groupBy = k, m_dirtyFilter = m_voiceDirty = true;
            ImGui::EndCombo();
        }
        popComboStyle();
    }
    static std::string oneLine(const std::string& s) {
        std::string out;
        out.reserve(std::min<std::size_t>(s.size(), 200));
        for (std::size_t i = 0; i < s.size() && out.size() < 200; ++i) out += s[i];
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
        clipper.Begin(int(m_lines.size()));
        if (m_scrollToSelected) {
            for (std::size_t v = 0; v < m_lines.size(); ++v)
                if (m_lines[v].row == m_selected && m_selected >= 0) clipper.IncludeItemByIndex(int(v));
        }
        std::string toggle;
        while (clipper.Step()) {
            for (int v = clipper.DisplayStart; v < clipper.DisplayEnd; ++v) {
                const ListLine& line = m_lines[std::size_t(v)];
                if (line.row < 0) {
                    if (drawHeading(line)) toggle = line.key;
                    continue;
                }
                const int i = line.row;
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
                richText(oneLine(r.ref), color::text2, true);
                ImGui::TableNextColumn();
                const loc::Status st = loc::Translation::status(r);
                if (st == loc::Status::Untranslated) ImGui::TextColored(vec(color::muted), "%s", tr("(not translated)"));
                else richText(oneLine(r.text), color::text, true);
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
        if (!toggle.empty()) toggleGroup(toggle);
        // (keyboard: Ctrl+Up / Ctrl+Down: the previous / next string of the list)
        if (ImGui::GetIO().KeyCtrl && !m_visible.empty()) {
            const int dir = ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? -1 : 0;
            if (dir) step(dir);
        }
    }
    void select(int i) {
        if (i != m_selected) m_cursor = -1;
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
    struct EditState {
        std::string* s = nullptr;
        int cursor = -1;   // the text cursor's byte offset, kept while the field is not focused (the palette types there)
    };
    static int resize(ImGuiInputTextCallbackData* data) {
        auto* e = static_cast<EditState*>(data->UserData);
        if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
            e->s->resize(std::size_t(data->BufTextLen));
            data->Buf = e->s->data();
        } else if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways) e->cursor = data->CursorPos;
        return 0;
    }
    static bool inputText(const char* id, std::string& s, ImVec2 size, int* cursor = nullptr) {
        ImGui::PushStyleColor(ImGuiCol_FrameBg, vec(color::input));
        EditState e{&s, cursor ? *cursor : -1};
        const bool changed = ImGui::InputTextMultiline(id, s.data(), s.capacity() + 1, size,
                                                       ImGuiInputTextFlags_CallbackResize | ImGuiInputTextFlags_CallbackAlways, resize, &e);
        if (cursor) *cursor = e.cursor;
        ImGui::PopStyleColor();
        return changed;
    }
    static void smallTag(const char* label, uint32_t rgb) {
        const ImVec2 t = ImGui::CalcTextSize(label);
        const ImVec2 at = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetTextLineHeight();
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(at, {at.x + t.x + px(10), at.y + h}, ink(rgb, 0.18f), px(4));
        dl->AddText({at.x + px(5), at.y}, ink(rgb), label);
        ImGui::Dummy({t.x + px(10), h});
    }
    // The button icon palette: a click types the code ("&2X&.") at the field's cursor (else at its end)
    bool iconPalette(std::string& text, int& cursor) {
        bool changed = false;
        const float h = px(26);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(vec(color::text2), "%s", tr("Insert:"));
        for (const std::string& c : loc::iconCodes()) {
            const float w = iconWidth(c, h - px(6)) + px(8);
            ImGui::SameLine(0, px(4));
            if (ImGui::GetContentRegionAvail().x < w) ImGui::NewLine();
            ImGui::PushID(c.c_str());
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (ImGui::Button("##icon", {w, h})) {
                const std::string code = "&2" + c + "&.";
                const std::size_t at2 = cursor >= 0 && std::size_t(cursor) <= text.size() ? std::size_t(cursor) : text.size();
                text.insert(at2, code);
                cursor = int(at2 + code.size());
                changed = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("&2%s&.", c.c_str());
            drawIcon(ImGui::GetWindowDrawList(), {at.x + px(4), at.y + px(3)}, h - px(6), c);
            ImGui::PopID();
        }
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
            gap(6);
            caption(tr("The lists show the button codes as the buttons' icons; under the text field, a row of icons types their codes."), color::text2);
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
        if (r.ref.empty()) ImGui::TextColored(vec(color::text), "%s", tr("(none)"));
        else richText(r.ref, color::text);
        if (r.port && r.ref != r.english) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextColored(vec(color::muted), "%s: %s", tr("English"), r.english.c_str());
            ImGui::PopTextWrapPos();
        }
        gap(8);
        text(m_t.folder.key.c_str(), fonts().label, color::text2);
        const float boxH = px(110);
        if (inputText("##loc-text", r.text, {-1, boxH}, &m_cursor)) {
            m_t.recheck(r);
        }
        // the button icons: a palette that types their codes, and the text as the game shows it
        if (iconPalette(r.text, m_cursor)) m_t.recheck(r);
        if (!r.text.empty() && r.text.find('&') != std::string::npos) {
            caption(tr("As the game shows it:"), color::muted);
            richText(r.text, color::text);
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
        m_takeChoices = loc::readTakeChoices(dubDir());
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
        if (m_voiceFilter == 3 && (v.movie || !radioChoice(v.id))) return false;
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
            std::vector<int> rows;
            for (std::size_t i = 0; i < m_voices.size(); ++i)
                if (voiceMatches(m_voices[i]) || int(i) == m_voiceSel) rows.push_back(int(i));
            m_voiceLines = grouped(rows, m_groupBy, true);
            m_voiceVisible.clear();
            for (const ListLine& l : m_voiceLines)
                if (l.row >= 0) m_voiceVisible.push_back(l.row);
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
        ImGui::TableSetupColumn(m_groupBy == 2 ? tr("Place") : tr("Speaker"), ImGuiTableColumnFlags_WidthFixed, px(130));
        ImGui::TableSetupColumn(tr("Dub"), ImGuiTableColumnFlags_WidthFixed, px(70));
        ImGui::TableHeadersRow();
        ImGuiListClipper clipper;
        clipper.Begin(int(m_voiceLines.size()));
        if (m_voiceScroll)
            for (std::size_t k = 0; k < m_voiceLines.size(); ++k)
                if (m_voiceLines[k].row == m_voiceSel && m_voiceSel >= 0) clipper.IncludeItemByIndex(int(k));
        std::string toggle;
        while (clipper.Step()) {
            for (int k = clipper.DisplayStart; k < clipper.DisplayEnd; ++k) {
                const ListLine& line = m_voiceLines[std::size_t(k)];
                if (line.row < 0) {
                    if (drawHeading(line)) toggle = line.key;
                    continue;
                }
                const int i = line.row;
                const loc::VoiceRow& v = m_voices[std::size_t(i)];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::PushID(i);
                if (ImGui::Selectable(v.movie ? (std::string(tr("Movie")) + ": " + v.id).c_str() : v.id.c_str(), i == m_voiceSel,
                                      ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap)) {
                    selectVoice(i);
                }
                if (!v.movie && radioChoice(v.id)) {
                    ImGui::SameLine(0, px(6));
                    smallTag(tr("radio"), color::accent);
                }
                if (i == m_voiceSel && m_voiceScroll) {
                    ImGui::SetScrollHereY(0.4f);
                    m_voiceScroll = false;
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                const loc::Row* r = subtitleRow(v);
                if (r) richText(oneLine(r->text.empty() ? r->ref : r->text), color::text, true);
                else ImGui::TextColored(vec(color::muted), "%s", v.movie ? tr("(a cutscene's sound)") : tr("(no subtitle)"));
                ImGui::TableNextColumn();
                if (m_groupBy == 2 || v.movie) ImGui::TextColored(vec(color::text2), "%s", v.group.c_str());
                else ImGui::TextColored(vec(color::text2), "%s", speakerOf(v.id, true).second.c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(vec(v.dub.empty() ? color::muted : color::success), "%s", v.dub.empty() ? tr("missing") : tr("yes"));
            }
        }
        ImGui::EndTable();
        if (!toggle.empty()) toggleGroup(toggle);
        if (ImGui::GetIO().KeyCtrl && !m_voiceVisible.empty()) {
            const int dir = ImGui::IsKeyPressed(ImGuiKey_DownArrow) ? 1 : ImGui::IsKeyPressed(ImGuiKey_UpArrow) ? -1 : 0;
            if (dir) {
                auto it = std::find(m_voiceVisible.begin(), m_voiceVisible.end(), m_voiceSel);
                const int k = it == m_voiceVisible.end() ? 0 : int(it - m_voiceVisible.begin()) + dir;
                if (k >= 0 && k < int(m_voiceVisible.size())) selectVoice(m_voiceVisible[std::size_t(k)]), m_voiceScroll = true;
            }
        }
    }
    static std::string seconds(double s) {
        if (s < 0) return "?";
        char b[32];
        std::snprintf(b, sizeof(b), "%.2f s", s);
        return b;
    }
    // ---------------------------------------------------------------- sound: decoding, playing
    // A voice file as mono samples (WAV as it is; FLAC, VAG or other WAVs through the game program: --decode-audio).
    std::shared_ptr<const loc::audio::Mono> loadMono(const fs::path& file) {
        if (auto it = m_mono.find(utf8(file)); it != m_mono.end()) return it->second;
        loc::audio::Mono m = loc::audio::readWav(file);
        if (m.empty()) {
            const fs::path tmp = cacheDir() / "decode.wav";
            std::error_code ec;
            fs::create_directories(tmp.parent_path(), ec);
            std::atomic<bool> cancel{false};
            try {
                if (cane::runProcess({utf8(runtime()), "--decode-audio", utf8(file), utf8(tmp)}, runtime().parent_path(), cancel,
                                     [](const std::string&) {}) == 0)
                    m = loc::audio::readWav(tmp);
            } catch (...) {
            }
        }
        if (m.empty()) return nullptr;
        auto p = std::make_shared<const loc::audio::Mono>(std::move(m));
        if (m_mono.size() > 24) m_mono.clear();
        m_mono[utf8(file)] = p;
        return p;
    }
    bool ensureAudio() {
        if (!m_audio) {
            m_audio = std::make_unique<collection::Audio>();
            if (!m_audio->open()) m_status = tr("No sound output (SDL2 or an audio device is missing).");
        }
        return m_audio->available();
    }
    void playMono(const loc::audio::Mono& m) {
        if (!ensureAudio() || m.empty()) return;
        collection::Pcm p;
        p.rate = m.rate;
        p.loops = false;
        p.samples.resize(m.s.size() * 2);
        for (std::size_t i = 0; i < m.s.size(); ++i) p.samples[i * 2] = p.samples[i * 2 + 1] = int16_t(std::clamp(m.s[i], -1.0f, 1.0f) * 32767.0f);
        p.loopStart = 0;
        p.loopEnd = p.frames();
        m_audio->playMusic(nullptr, 0.05f);
        m_audio->playSound(std::make_shared<const collection::Pcm>(std::move(p)));
    }
    void play(const fs::path& file) {
        const auto m = loadMono(file);
        if (!m) {
            m_status = trf("Cannot play %s", utf8(file).c_str());
            return;
        }
        playMono(*m);
    }
    void stopSound() {
        if (m_audio) m_audio = nullptr;   // (closing the device stops every voice)
    }

    // ---------------------------------------------------------------- the radio effect (loc_audio.h)
    // Whether a line is radio talk: the dialog tables' headset flag; a line not in them: its reference's sound
    // (measured when the line is chosen; until then not radio). The language's own choice (Takes/takes.json) wins.
    struct RadioInfo {
        bool known = false;        // the reference's sound measured
        bool soundsRadio = false;  // band-limited like the radio lines
        loc::audio::RadioFilter filter;
    };
    bool radioDetected(const std::string& id) const {
        if (const auto it = m_usage.dialogs.find(id); it != m_usage.dialogs.end()) return it->second.headset;
        const auto jt = m_radio.find(id);
        return jt != m_radio.end() && jt->second.soundsRadio;
    }
    bool radioChoice(const std::string& id) const {
        if (const auto it = m_takeChoices.find(id); it != m_takeChoices.end()) return it->second;
        return radioDetected(id);
    }
    // the reference's filter (and its sound's verdict), measured once per line
    const RadioInfo& radioInfo(const loc::VoiceRow& v) {
        RadioInfo& info = m_radio[v.id];
        if (!info.known && !v.ref.empty()) {
            info.known = true;
            if (const auto m = loadMono(v.ref)) {
                const loc::audio::Spectrum sp = loc::audio::spectrum(*m);
                info.soundsRadio = loc::audio::radioLike(sp);
                info.filter = loc::audio::estimateFilter(sp);
            }
        }
        return info;
    }
    loc::audio::Mono withRadio(const loc::VoiceRow& v, const loc::audio::Mono& dry) {
        const RadioInfo& info = radioInfo(v);
        if (!info.filter.valid()) return dry;
        return loc::audio::applyRadio(dry, info.filter);
    }
    void setRadioChoice(loc::VoiceRow& v, bool on) {
        std::string error;
        if (!loc::saveTakeChoice(dubDir(), v.id, on, error)) {
            m_status = error;
            return;
        }
        m_takeChoices[v.id] = on;
        m_voiceDirty = true;
        // (the dub made again from its dry take; a dub without one becomes its dry take first)
        const fs::path dry = loc::takePath(dubDir(), v.id);
        std::error_code ec;
        if (!fs::is_regular_file(dry, ec)) {
            if (v.dub.empty()) return;
            const auto m = loadMono(v.dub);
            if (!m || !loc::audio::writeWav(dry, *m, error)) {
                m_status = error.empty() ? trf("Cannot read %s", utf8(v.dub).c_str()) : error;
                return;
            }
        }
        m_mono.erase(utf8(dry));
        const auto m = loadMono(dry);
        if (m) writeDub(v, *m);
    }
    // The dub from a dry take: the radio effect on it when the line's choice is radio; a 16-bit WAV in the line's place
    // (any other format's file of the line removed), the dry take kept in Takes/.
    bool writeDub(loc::VoiceRow& v, const loc::audio::Mono& dry) {
        std::string error;
        if (!loc::audio::writeWav(loc::takePath(dubDir(), v.id), dry, error)) {
            m_status = error;
            return false;
        }
        const bool radio = !v.movie && radioChoice(v.id);
        const loc::audio::Mono out = radio ? withRadio(v, dry) : dry;
        const fs::path dest = loc::dubPath(dubDir(), v, ".wav");
        std::error_code ec;
        for (const char* other : {".flac", ".vag"}) fs::remove(loc::dubPath(dubDir(), v, other), ec);
        if (!loc::audio::writeWav(dest, out, error)) {
            m_status = error;
            return false;
        }
        m_pcm.erase(utf8(dest));
        m_mono.erase(utf8(dest));
        m_mono.erase(utf8(loc::takePath(dubDir(), v.id)));
        rescanAfterDub();
        m_status = radio ? trf("Saved %s (with the radio effect; the dry take in Takes)", utf8(dest).c_str()) : trf("Saved %s", utf8(dest).c_str());
        return true;
    }
    void rescanAfterDub() {
        m_folders = loc::scanFolders(languagesDir(), modsLanguagesDir());
        m_current = folderIndex(m_currentKey);
        m_t.folder = m_folders[std::size_t(m_current)];
        scanVoices();
    }

    // ---------------------------------------------------------------- recording a take
    enum class Rec { Idle, Countdown, Recording, Review };
    void selectVoice(int i) {
        if (i != m_voiceSel && m_rec != Rec::Idle) stopRecording(false);
        m_voiceSel = i;
        m_dubFile[0] = 0;
        m_lengthEdit = 0;
    }
    void startCountdown(const loc::VoiceRow& v) {
        stopSound();
        if (!m_capture.open(m_micName)) {
            m_status = tr("No microphone could be opened (SDL2 or a recording device is missing).");
            return;
        }
        m_recId = v.id;
        m_rec = Rec::Countdown;
        m_recT0 = ImGui::GetTime();
    }
    void startRecording(const loc::VoiceRow& v) {
        m_capture.take();   // (what came during the countdown)
        const double ref = v.refSeconds > 0 ? v.refSeconds : 20.0;
        m_recorder.reset(m_capture.rate(), ref + kRecordMargin);
        m_rec = Rec::Recording;
        m_recT0 = ImGui::GetTime();
        if (m_playAlong && !v.ref.empty()) play(v.ref);
    }
    void stopRecording(bool keep) {
        m_capture.close();
        if (keep && !m_recorder.s.empty()) {
            m_take.rate = m_recorder.rate;
            m_take.s = m_recorder.s;
            const auto span = loc::audio::speechSpan(m_take);
            m_trimA = span.first, m_trimB = span.second;
            m_rec = Rec::Review;
            if (m_playAlong) stopSound();
        } else m_rec = Rec::Idle;
    }
    void pollRecording(const loc::VoiceRow& v) {
        if (m_rec == Rec::Countdown && ImGui::GetTime() - m_recT0 >= kCountdown) startRecording(v);
        if (m_rec == Rec::Recording) {
            const std::vector<float> got = m_capture.take();
            m_recorder.push(got.data(), got.size());
            if (m_recorder.full()) stopRecording(true);
        }
    }
    // The take's waveform with its two trim handles (dragged), the reference's length marked from the start handle
    void drawWaveform(double refSeconds) {
        const float w = ImGui::GetContentRegionAvail().x, h = px(90);
        const ImVec2 a = ImGui::GetCursorScreenPos(), b = {a.x + w, a.y + h};
        ImGui::InvisibleButton("##wave", {w, h});
        auto* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(a, b, ink(color::input), px(6));
        const double len = std::max(m_take.seconds(), 0.01);
        auto xOf = [&](double t) { return a.x + float(t / len) * w; };
        const auto cols = loc::audio::outline(m_take, int(w / px(2)));
        const float mid = (a.y + b.y) * 0.5f;
        float pk = 0.05f;
        for (const auto& [lo, hi] : cols) pk = std::max({pk, -lo, hi});
        for (std::size_t c = 0; c < cols.size(); ++c) {
            const float x = a.x + (float(c) + 0.5f) * w / float(cols.size());
            const double t = double(c) / double(cols.size()) * len;
            const bool in = t >= m_trimA && t <= m_trimB;
            dl->AddLine({x, mid - cols[c].second / pk * h * 0.45f}, {x, mid - cols[c].first / pk * h * 0.45f + 1}, ink(in ? color::accent : color::muted), px(1.5f));
        }
        // the parts cut away, dimmed; the reference's length from the start
        dl->AddRectFilled(a, {xOf(m_trimA), b.y}, ink(0x000000, 0.35f), px(6));
        dl->AddRectFilled({xOf(m_trimB), a.y}, b, ink(0x000000, 0.35f), px(6));
        if (refSeconds > 0 && m_trimA + refSeconds < len) {
            const float x = xOf(m_trimA + refSeconds);
            for (float y = a.y; y < b.y; y += px(8)) dl->AddLine({x, y}, {x, std::min(b.y, y + px(4))}, ink(color::warning), px(1.5f));
        }
        for (int k = 0; k < 2; ++k) {
            const float x = xOf(k ? m_trimB : m_trimA);
            dl->AddLine({x, a.y}, {x, b.y}, ink(color::cta), px(2.5f));
            dl->AddRectFilled({x - px(5), a.y}, {x + px(5), a.y + px(14)}, ink(color::cta), px(3));
        }
        // dragging: the nearer handle
        if (ImGui::IsItemActivated()) {
            const float mx = ImGui::GetIO().MousePos.x;
            m_drag = std::abs(mx - xOf(m_trimA)) <= std::abs(mx - xOf(m_trimB)) ? 0 : 1;
        }
        if (ImGui::IsItemActive() && m_drag >= 0) {
            const double t = std::clamp(double(ImGui::GetIO().MousePos.x - a.x) / w * len, 0.0, len);
            if (m_drag == 0) m_trimA = std::min(t, m_trimB - 0.05);
            else m_trimB = std::max(t, m_trimA + 0.05);
        } else m_drag = -1;
        if (ImGui::IsItemHovered()) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    // The next button on this row when it fits, else on the next row
    static void flow(const char* label) {
        const float w = ImGui::CalcTextSize(label).x + px(70);
        ImGui::SameLine(0, px(6));
        if (ImGui::GetContentRegionAvail().x < w) ImGui::NewLine();
    }
    void drawRecorder(loc::VoiceRow& v) {
        pollRecording(v);
        gap(10);
        overline(tr("Record a dub"));
        if (m_rec == Rec::Idle) {
            // the microphone (the devices as Settings lists them for Sly 2's headset)
            if (!m_micsListed) m_mics = cane::audio::listDevices().inputs, m_micsListed = true;
            pushComboStyle();
            ImGui::SetNextItemWidth(px(300));
            if (beginCombo("##loc-mic", m_micName.empty() ? tr("System default microphone") : m_micName.c_str())) {
                if (ImGui::Selectable(tr("System default microphone"), m_micName.empty())) m_micName.clear();
                for (const std::string& n : m_mics)
                    if (ImGui::Selectable(n.c_str(), n == m_micName)) m_micName = n;
                ImGui::EndCombo();
            }
            popComboStyle();
            ImGui::SameLine(0, px(8));
            ImGui::Checkbox(tr("Play the reference along"), &m_playAlong);
            caption(m_playAlong ? tr("Wear headphones: the reference line plays while you record, so you can follow its timing.")
                                : tr("The reference does not play: listen to it first, then record."),
                    color::muted);
            const std::string limit = v.refSeconds > 0 ? trf("Stops by itself after %.1f s (the reference and %.1f s).", v.refSeconds + kRecordMargin, kRecordMargin)
                                                       : std::string(tr("Stop it with the button."));
            if (button(tr("Record"), Tone::Cta, {0, px(34)}, Icon::Headset)) startCountdown(v);
            ImGui::SameLine(0, px(8));
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(vec(color::text2), "%s", limit.c_str());
            return;
        }
        if (m_rec == Rec::Countdown || m_rec == Rec::Recording) {
            const double t = ImGui::GetTime() - m_recT0;
            auto* dl = ImGui::GetWindowDrawList();
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float w = ImGui::GetContentRegionAvail().x, h = px(90);
            dl->AddRectFilled(at, {at.x + w, at.y + h}, ink(color::input), px(6));
            if (m_rec == Rec::Countdown) {
                const int n = std::max(1, int(std::ceil(kCountdown - t)));
                const std::string big = std::to_string(n);
                ImFont* f = fonts().heading;
                const float fs = h * 0.8f;
                const ImVec2 ts = f->CalcTextSizeA(fs, FLT_MAX, 0, big.c_str());
                dl->AddText(f, fs, {at.x + (w - ts.x) * 0.5f, at.y + (h - ts.y) * 0.5f}, ink(color::cta), big.c_str());
                dl->AddText({at.x + px(10), at.y + px(8)}, ink(color::text2), tr("Get ready..."));
            } else {
                const double limit = m_recorder.limitSeconds;
                const float frac = limit > 0 ? float(std::min(1.0, m_recorder.seconds() / limit)) : 0.0f;
                dl->AddRectFilled(at, {at.x + w * frac, at.y + h}, ink(color::danger, 0.15f), px(6));
                // the take so far, on the whole length it may take
                if (!m_recorder.s.empty() && frac > 0) {
                    loc::audio::Mono so;
                    so.rate = m_recorder.rate;
                    so.s = m_recorder.s;
                    const int columns = std::max(1, int(w * frac / px(2)));
                    const auto cols = loc::audio::outline(so, columns);
                    float pk = 0.05f;
                    for (const auto& [lo, hi] : cols) pk = std::max({pk, -lo, hi});
                    const float mid = at.y + h * 0.55f;
                    for (std::size_t c = 0; c < cols.size(); ++c) {
                        const float x = at.x + (float(c) + 0.5f) * px(2);
                        dl->AddLine({x, mid - cols[c].second / pk * h * 0.3f}, {x, mid - cols[c].first / pk * h * 0.3f + 1}, ink(color::danger), px(1.5f));
                    }
                }
                // the live level (the last 50 ms)
                const std::size_t n = std::min<std::size_t>(m_recorder.s.size(), std::size_t(m_recorder.rate / 20));
                const double level = n ? loc::audio::rms(m_recorder.s.data() + m_recorder.s.size() - n, n) : 0.0;
                const float lw = float(std::min(1.0, level * 6)) * (w - px(20));
                dl->AddRectFilled({at.x + px(10), at.y + h - px(18)}, {at.x + px(10) + lw, at.y + h - px(10)}, ink(color::success), px(3));
                dl->AddCircleFilled({at.x + px(18), at.y + px(18)}, px(6), ink(color::danger));
                const std::string label = limit > 0 ? trf("Recording %.1f s of %.1f s", m_recorder.seconds(), limit) : trf("Recording %.1f s", m_recorder.seconds());
                dl->AddText({at.x + px(32), at.y + px(10)}, ink(color::text), label.c_str());
            }
            ImGui::Dummy({w, h});
            if (button(m_rec == Rec::Countdown ? tr("Cancel") : tr("Stop"), Tone::Secondary, {0, px(32)}, Icon::Stop)) stopRecording(m_rec == Rec::Recording);
            return;
        }
        // the take: trim it, listen, save
        drawWaveform(v.refSeconds);
        const double len = m_trimB - m_trimA;
        const bool longer = v.refSeconds > 0 && len > v.refSeconds * 1.25 + 0.3;
        ImGui::TextColored(vec(longer ? color::warning : color::text2), "%s",
                           (trf("Trimmed take: %.2f s", len) + (v.refSeconds > 0 ? "   |   " + trf("reference: %.2f s", v.refSeconds) : std::string())).c_str());
        caption(tr("Drag the handles to cut the start and the end; the dashed line is the reference's length from the start."), color::muted);
        if (button(tr("Play the take"), Tone::Secondary, {0, px(32)}, Icon::Play)) playMono(loc::audio::trimmed(m_take, m_trimA, m_trimB));
        const bool radio = !v.movie && radioChoice(v.id);
        if (radio) {
            flow(tr("Play with the radio effect"));
            if (button(tr("Play with the radio effect"), Tone::Secondary, {0, px(32)}, Icon::Play)) playMono(withRadio(v, loc::audio::trimmed(m_take, m_trimA, m_trimB)));
        }
        flow(tr("Play the reference"));
        ImGui::BeginDisabled(v.ref.empty());
        if (button(tr("Play the reference"), Tone::Quiet, {0, px(32)}, Icon::Play)) play(v.ref);
        ImGui::EndDisabled();
        gap(4);
        if (button(tr("Save as the dub"), Tone::Cta, {0, px(34)}, Icon::Check)) {
            if (writeDub(v, loc::audio::trimmed(m_take, m_trimA, m_trimB))) m_rec = Rec::Idle;
        }
        flow(tr("Record again"));
        if (button(tr("Record again"), Tone::Secondary, {0, px(34)}, Icon::Refresh)) startCountdown(v);
        flow(tr("Discard"));
        if (button(tr("Discard"), Tone::Quiet, {0, px(34)}, Icon::Trash)) m_rec = Rec::Idle;
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
        // (screenshots: SAFEHOUSE_LOC_RECORD=countdown|recording|review with SAFEHOUSE_FAKE_MIC, the fake microphone's file)
        if (const char* sz = std::getenv("SAFEHOUSE_LOC_RECORD"); sz && *sz && !m_recordEnvDone) {
            m_recordEnvDone = true;
            const std::string want = sz;
            if (want == "review") {
                if (const char* f = std::getenv("SAFEHOUSE_FAKE_MIC")) m_take = loc::audio::readWav(cane::path(f));
                const auto span = loc::audio::speechSpan(m_take);
                m_trimA = span.first, m_trimB = span.second;
                m_rec = m_take.empty() ? Rec::Idle : Rec::Review;
            } else if (m_capture.open(m_micName)) {
                m_recId = v.id;
                m_playAlong = false;
                m_rec = Rec::Countdown;
                m_recT0 = ImGui::GetTime() - 0.6;
                if (want == "recording") {
                    // (two and a half seconds in)
                    startRecording(v);
                    const auto got = m_capture.takeFake(2.5);
                    m_recorder.push(got.data(), got.size());
                    m_recT0 -= 2.5;
                }
            }
        }
        text(v.movie ? (std::string(tr("Movie")) + "  " + v.id).c_str() : trf("Line %s", v.id.c_str()).c_str(), fonts().tab);
        if (!v.movie) {
            const std::string who = speakerOf(v.id, true).second + "   |   " + levelLabel(loc::levelOf(m_usage, v.id)) + (v.group.empty() ? "" : "   |   " + v.group);
            caption(who.c_str(), color::text2);
        } else if (!v.group.empty()) caption(v.group.c_str(), color::text2);
        gap(6);
        // the reference's line
        text(m_voiceRefKey.c_str(), fonts().label, color::text2);
        ImGui::BeginDisabled(v.ref.empty());
        if (button(tr("Play##ref"), Tone::Secondary, {0, px(32)}, Icon::Play)) play(v.ref);
        ImGui::EndDisabled();
        ImGui::SameLine(0, px(8));
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(vec(color::text2), "%s", v.ref.empty() ? tr("(none)") : seconds(v.refSeconds).c_str());
        if (const loc::Row* r = subtitleRow(v)) richText(r->ref, color::text);
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
            if (r->text.empty()) ImGui::TextColored(vec(color::muted), "%s", tr("(not translated)"));
            else richText(r->text, color::text);
            if (button(tr("Edit the subtitle"), Tone::Quiet, {0, px(30)}, Icon::ChevronRight)) {
                m_section = 0;
                m_dirtyFilter = true;
                select(m_rowById.at(v.id));
            }
        }
        if (!v.dub.empty()) caption(utf8(v.dub).c_str(), color::muted);
        // the radio effect
        if (!v.movie) {
            gap(10);
            overline(tr("Radio effect"));
            const auto it = m_usage.dialogs.find(v.id);
            const RadioInfo& info = radioInfo(v);
            const bool detected = radioDetected(v.id);
            caption(it != m_usage.dialogs.end() ? (detected ? tr("The game plays this line as radio talk (its dialog table marks it for the headset).")
                                                            : tr("The game plays this line as normal speech."))
                                                : (detected ? tr("The reference sounds like radio talk (no lows, few highs).") : tr("The reference sounds like normal speech.")),
                    color::text2);
            if (it != m_usage.dialogs.end() && !detected && info.soundsRadio)
                caption(tr("Its reference still sounds band-limited (an announcement or talk heard through the binocucom): switch the effect on if the dub should too."),
                        color::muted);
            bool on = radioChoice(v.id);
            if (ImGui::Checkbox(tr("Put the radio effect on this line's dub"), &on)) setRadioChoice(v, on);
            if (on) {
                caption(tr("Estimated from the reference line: its band-pass and tone, compression, crackle and noise. The dry take is kept in the "
                           "language's Takes folder, so the effect can be switched off again."),
                        color::muted);
                const fs::path dry = loc::takePath(dubDir(), v.id);
                std::error_code ec;
                const bool hasDry = fs::is_regular_file(dry, ec);
                ImGui::BeginDisabled(!hasDry && v.dub.empty());
                if (button(tr("Preview the effect"), Tone::Secondary, {0, px(30)}, Icon::Play)) {
                    if (const auto m = loadMono(hasDry ? dry : v.dub)) playMono(withRadio(v, *m));
                }
                ImGui::EndDisabled();
                if (hasDry) {
                    ImGui::SameLine(0, px(6));
                    if (button(tr("Play the dry take"), Tone::Quiet, {0, px(30)}, Icon::Play)) play(dry);
                }
            }
        }
        // recording
        drawRecorder(v);
        // a new dub: a file copied in under the line's name
        gap(10);
        overline(v.dub.empty() ? tr("Add a dub from a file") : tr("Replace the dub with a file"));
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
        // a radio line: the file is the dry take, the dub gets the effect
        if (!v.movie && radioChoice(v.id)) {
            const auto m = loadMono(file);
            if (!m) {
                m_status = trf("Cannot read %s", utf8(file).c_str());
                return;
            }
            if (writeDub(v, *m)) m_dubFile[0] = 0;
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
        m_mono.erase(utf8(dest));
        m_dubFile[0] = 0;
        rescanAfterDub();
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
    bool m_usageRetried = false;
    loc::ojson m_meta;
    // the list
    int m_section = 0, m_filter = 0, m_selected = -1, m_addWorld = 0;
    bool m_dirtyFilter = true, m_scrollToSelected = false;
    std::vector<int> m_visible;
    std::vector<ListLine> m_lines, m_voiceLines;
    int m_groupBy = 2, m_cursor = -1;
    std::set<std::string> m_collapsed;
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
    std::map<std::string, std::shared_ptr<const loc::audio::Mono>> m_mono;
    std::map<std::string, RadioInfo> m_radio;
    std::map<std::string, bool> m_takeChoices;
    // recording
    static constexpr double kCountdown = 3.0, kRecordMargin = 1.0;
    collection::Capture m_capture;
    loc::audio::Recorder m_recorder;
    loc::audio::Mono m_take;
    Rec m_rec = Rec::Idle;
    double m_recT0 = 0, m_trimA = 0, m_trimB = 0;
    int m_drag = -1;
    bool m_playAlong = true, m_micsListed = false, m_recordEnvDone = false;
    std::string m_recId, m_micName;
    std::vector<std::string> m_mics;
    // the background job
    std::thread m_job;
    std::atomic<bool> m_jobCancel{false}, m_jobDone{false}, m_portDone{false};
    bool m_jobRunning = false, m_jobUsage = false;
    std::string m_jobFailed, m_jobFailedThread;
};

}  // namespace cane::ui
