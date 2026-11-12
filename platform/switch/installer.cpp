// See installer.h. The screens are ProjectCane's (platform/installer/view.h); this drives them on the Switch:
//   Browse    the SD card's folders and disc images: D-pad, A opens / installs, B goes up, + quits (touch works too)
//   Extract   the conversion's progress, on its own thread with the CPU boost mode on; B cancels
//   Complete  keep or delete the disc image: Left / Right, A confirms (B keeps it)
//   Error     what stopped it: A or B back to the list (or into the game, when it is installed after all)
#include "installer.h"
#include "runtime.h"
#include "switch_ui.h"
#include "switch_assets.h"
#include "../disc_image.h"
#include "../installer/view.h"

#include <pthread.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <filesystem>
#include <mutex>
#include <string>
#include <sys/statvfs.h>
#include <vector>

namespace CaneSwitch
{
namespace
{
namespace fs = std::filesystem;
using CaneSetup::Action;
using CaneSetup::Screen;

std::string Lower(std::string s)
{
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string SizeText(uint64_t bytes)
{
    char text[32];
    if (bytes >= (1ull << 30)) std::snprintf(text, sizeof(text), "%.2f GB", double(bytes) / double(1ull << 30));
    else std::snprintf(text, sizeof(text), "%.0f MB", double(bytes) / double(1ull << 20));
    return text;
}
uint64_t FreeBytes(const char* path)
{
    struct statvfs st{};
    return statvfs(path, &st) == 0 ? uint64_t(st.f_bavail) * uint64_t(st.f_bsize) : 0;
}

// The worker's state: it writes the progress, the screen reads it.
struct Work
{
    std::mutex mutex;
    std::string stage, file, error;
    float fraction = 0.0f;
    std::atomic<bool> cancel{false}, finished{false};
    pthread_t thread{};
    bool started = false;
    std::function<std::string(const InstallProgress&)> run;
};
void* WorkThread(void* p)
{
    Work& w = *static_cast<Work*>(p);
    std::string error;
    try
    {
        error = w.run([&w](float fraction, const std::string& stage, const std::string& file) {
            std::lock_guard<std::mutex> lock(w.mutex);
            w.fraction = std::clamp(fraction, 0.0f, 1.0f);
            w.stage = stage;
            w.file = file;
            return !w.cancel.load();
        });
    }
    catch (const std::exception& e) { error = e.what(); }
    catch (...) { error = "an unknown error"; }
    {
        std::lock_guard<std::mutex> lock(w.mutex);
        w.error = error;
    }
    w.finished = true;
    return nullptr;
}

class Installer
{
public:
    explicit Installer(const InstallJob& job) : m_job(job)
    {
        m_view.hint = job.hint;
        if (m_ui.Ok()) m_background = Ui::Texture(cane_installer_background, cane_installer_background_size);
        Refresh();
    }
    ~Installer()
    {
        Stop();
        if (m_background) glDeleteTextures(1, &m_background);
    }
    bool Ok() const { return m_ui.Ok(); }

    // A frame: the view's action from the controller or a touch (Exit: the program ends).
    Action Frame()
    {
        if (!m_ui.Begin()) return Action::Exit;
        const u64 pressed = m_ui.Pressed();
        CaneSetup::View& v = m_view;
        Action input = Action::None;
        if ((pressed & HidNpadButton_Plus) && v.screen != Screen::Extract) input = Action::Exit;
        else if (v.screen == Screen::Browse)
        {
            if (!v.entries.empty())
            {
                if (pressed & HidNpadButton_AnyUp) v.selected = (v.selected + v.entries.size() - 1) % v.entries.size();
                if (pressed & HidNpadButton_AnyDown) v.selected = (v.selected + 1) % v.entries.size();
                if (pressed & HidNpadButton_A) input = Action::Select;
            }
            if (pressed & HidNpadButton_B) input = Action::Back;
        }
        else if (v.screen == Screen::Extract)
        {
            if (pressed & HidNpadButton_B) input = Action::Back;
        }
        else if (v.screen == Screen::Complete)
        {
            if (pressed & (HidNpadButton_AnyLeft | HidNpadButton_AnyRight)) v.deleteIso = !v.deleteIso;
            if (pressed & HidNpadButton_A) input = v.deleteIso ? Action::DeleteIso : Action::KeepIso;
            if (pressed & HidNpadButton_B) input = Action::KeepIso;
        }
        else if (pressed & (HidNpadButton_A | HidNpadButton_B)) input = Action::Back;
        if (v.screen == Screen::Extract) PollWork();
        const Action clicked = CaneSetup::Draw(v, m_background ? Ui::Id(m_background) : ImTextureID{});
        m_ui.End();
        return clicked != Action::None ? clicked : input;
    }

    // The flow: true when the game is installed (and the program goes on), false to end.
    bool Run()
    {
        const std::string& data = GetPaths().data;
        if (m_job.needsUpdate && m_job.update && IsDirectory(data) && m_job.needsUpdate(data))
        {
            Start([this, data](const InstallProgress& p) { return m_job.update(data, p); }, "Updating game files", "");
            m_updating = true;
        }
        for (;;)
        {
            const Action action = Frame();
            if (action == Action::Exit) return false;
            CaneSetup::View& v = m_view;
            switch (v.screen)
            {
            case Screen::Browse:
                if (action == Action::Back) Up();
                else if (action == Action::Select && v.selected < m_paths.size())
                {
                    const fs::path path = m_paths[v.selected];
                    if (v.entries[v.selected].folder) { m_folder = path; Refresh(); }
                    else Begin(path);
                }
                break;
            case Screen::Extract:
                if (action == Action::Back && !m_work.cancel)
                {
                    m_work.cancel = true;
                    v.stage = "Cancelling";
                }
                break;
            case Screen::Complete:
                if (action == Action::KeepIso || action == Action::DeleteIso)
                {
                    if (action == Action::DeleteIso)
                    {
                        std::error_code ec;
                        for (const fs::path& part : cane::imageParts(m_iso)) fs::remove(part, ec);
                        fs::remove(m_iso, ec);
                    }
                    return true;
                }
                break;
            case Screen::Error:
                if (action == Action::Back)
                {
                    if (v.gameReady) return true;
                    v.screen = Screen::Browse;
                    Refresh();
                }
                break;
            }
            if (m_done) return true;
        }
    }

private:
    static bool IsDirectory(const std::string& p) { std::error_code ec; return fs::is_directory(p, ec); }
    // A disc image to list: an .iso, a .chd, or an .iso in parts (a folder named *.iso of 00, 01, ..., or the first of
    // name.iso.000, .001, ...: copies for a FAT32 card, platform/disc_image.h imageParts)
    static bool IsImage(const fs::path& path, bool folder)
    {
        const std::string extension = Lower(path.extension().string());
        if (!folder && (extension == ".iso" || extension == ".chd")) return true;
        return Lower((folder ? path : path.stem()).extension().string()) == ".iso" && !cane::imageParts(path).empty();
    }

    void Refresh()
    {
        struct Item
        {
            fs::path path;
            bool folder;
        };
        std::vector<Item> items;
        std::error_code ec;
        for (fs::directory_iterator it(m_folder, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec))
        {
            const fs::directory_entry& e = *it;
            const std::string name = e.path().filename().string();
            std::error_code ec2;
            if (name.empty() || name.front() == '.') continue;
            const bool directory = e.is_directory(ec2);
            if (IsImage(e.path(), directory)) items.push_back({e.path(), false});
            else if (directory) items.push_back({e.path(), true});
        }
        std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            if (a.folder != b.folder) return a.folder;
            return Lower(a.path.filename().string()) < Lower(b.path.filename().string());
        });
        m_paths.clear();
        m_view.entries.clear();
        m_view.selected = m_view.firstVisible = 0;
        m_view.directory = "SD card";
        const fs::path relative = m_folder.lexically_relative(fs::path("sdmc:/"));
        if (!relative.empty() && relative != ".") m_view.directory += " / " + relative.generic_string();
        for (const Item& item : items)
        {
            m_paths.push_back(item.path);
            m_view.entries.push_back({item.path.filename().string(), item.folder});
        }
    }
    void Up()
    {
        if (m_folder == fs::path("sdmc:/")) return;
        m_folder = m_folder.parent_path();
        if (m_folder.empty() || m_folder == fs::path("sdmc:")) m_folder = "sdmc:/";
        Refresh();
    }
    void ShowError(const std::string& message)
    {
        m_view.error = message;
        m_view.screen = Screen::Error;
    }

    // A disc image chosen: checked, then converted.
    void Begin(const fs::path& iso)
    {
        m_iso = iso;
        std::string error;
        const auto image = cane::DiscImage::open(iso, &error);
        if (!image) { ShowError(error); return; }
        const cane::DiscIdentity id = cane::identifyDisc(image->reader());
        const std::string mismatch = cane::discMismatch(id, cane::GameId(m_job.game));
        if (!mismatch.empty()) { ShowError(mismatch); return; }
        // the whole disc: a copy cut short (FAT32 holds no file of 4 GB or more) stops here, not where its end is read
        const uint64_t size = image->size(), disc = uint64_t(cane::readIsoVolume(image->reader()).blocks) * cane::kIsoSector;
        if (size < disc)
        {
            const bool fat32 = size >= (4ull << 30) - (64ull << 20) && size < (4ull << 30);
            ShowError("That disc image is incomplete: it holds " + SizeText(size) + " of the disc's " + SizeText(disc) + ". " +
                      (fat32 ? "A FAT32 SD card cannot hold a file of 4 GB or more, so the copy was cut short: copy the ISO "
                               "with DBI or another app that splits it, or use a CHD."
                             : "The copy did not finish: copy the disc image again."));
            return;
        }
        // a CHD or a split .iso: written out as one .iso for the conversion, unless the game reads it itself
        const bool chd = cane::chdFile(iso);
        const uint64_t temporary = (chd || !cane::imageParts(iso).empty()) && !m_job.readsImage ? size : 0;
        // (what the game writes for this disc, else about the image's size; and the CHD's .iso while it converts)
        const uint64_t output = m_job.needBytes ? m_job.needBytes(id.bootFile) : 0;
        const uint64_t need = (output ? output : size + (512ull << 20)) + temporary, free = FreeBytes("sdmc:/");
        if (free && free < need)
        {
            ShowError("There is not enough free space on the SD card: the game needs about " + SizeText(need) + " and " +
                      SizeText(free) + " is free.");
            return;
        }
        const std::string data = GetPaths().data, partial = data + ".partial";
        const std::string isoPath = iso.string(), decompressed = temporary ? data + ".from-image.iso" : std::string();
        const std::string writing = chd ? "Decompressing the CHD" : "Joining the parts";
        Start([this, isoPath, data, partial, decompressed, writing](const InstallProgress& progress) -> std::string {
                  std::error_code e;
                  fs::remove_all(partial, e);
                  fs::create_directories(partial, e);
                  if (e) return "The game folder could not be created: " + e.message();
                  std::string source = isoPath, error;
                  if (!decompressed.empty())
                  {
                      const std::string name = fs::path(isoPath).filename().string();
                      error = cane::writeImageAsIso(isoPath, decompressed, [&](uint64_t done, uint64_t total) {
                          return progress(total ? float(double(done) / double(total)) : 0.0f, writing, name);
                      });
                      source = decompressed;
                  }
                  if (error.empty()) error = m_job.convert(source, partial, progress);
                  if (!decompressed.empty()) fs::remove(decompressed, e);
                  if (error.empty())
                  {
                      progress(1.0f, "Finishing", "");
                      fs::remove_all(data, e);
                      e.clear();
                      fs::rename(partial, data, e);
                      if (e) error = "The game folder could not be renamed: " + e.message();
                  }
                  if (!error.empty())
                  {
                      progress(1.0f, error == "cancelled" ? "Cancelling" : "Cleaning up", "");
                      fs::remove_all(partial, e);
                  }
                  return error;
              },
              "Checking your ISO", iso.filename().string());
    }

    void Start(std::function<std::string(const InstallProgress&)> run, const std::string& stage, const std::string& file)
    {
        Stop();
        m_work.cancel = false;
        m_work.finished = false;
        m_work.run = std::move(run);
        m_work.stage = stage;
        m_work.file = file;
        m_work.fraction = 0.0f;
        m_work.error.clear();
        m_view.screen = Screen::Extract;
        m_view.stage = stage;
        m_view.file = file;
        m_view.progress = 0.0f;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 8u << 20);   // (Sly 2's conversion loads whole worlds with the game's loaders)
        m_work.started = pthread_create(&m_work.thread, &attr, WorkThread, &m_work) == 0;
        pthread_attr_destroy(&attr);
        if (!m_work.started) { ShowError("The conversion could not start."); return; }
        BeginLoadingBoost();
        m_boosting = true;
    }
    void Stop()
    {
        if (m_work.started)
        {
            m_work.cancel = true;
            pthread_join(m_work.thread, nullptr);
            m_work.started = false;
        }
        if (m_boosting) { EndLoadingBoost(); m_boosting = false; }
    }
    void PollWork()
    {
        {
            std::lock_guard<std::mutex> lock(m_work.mutex);
            m_view.progress = m_work.fraction;
            if (!m_work.cancel) m_view.stage = m_work.stage;
            m_view.file = m_work.file;
        }
        if (!m_work.finished) return;
        Stop();
        const std::string error = m_work.error;
        const bool updating = m_updating;
        m_updating = false;
        if (error.empty())
        {
            if (updating) { m_done = true; return; }
            m_view.deleteIso = false;
            m_view.gameReady = true;
            m_view.screen = Screen::Complete;
        }
        else if (error == "cancelled")
        {
            m_view.gameReady = false;
            ShowError("Extraction cancelled. Select your ISO again when you're ready.");
        }
        else
        {
            m_view.gameReady = false;
            ShowError(error);
        }
    }

    const InstallJob& m_job;
    Ui m_ui;
    GLuint m_background = 0;
    CaneSetup::View m_view;
    fs::path m_folder = "sdmc:/";
    std::vector<fs::path> m_paths;
    fs::path m_iso;
    Work m_work;
    bool m_boosting = false, m_updating = false, m_done = false;
};
}  // namespace

bool RunInstaller(const InstallJob& job)
{
    bool installed = false;
    {
        Installer installer(job);
        if (!installer.Ok())
        {
            ErrorScreen("The graphics could not start for the installer.");
            return false;
        }
        installed = installer.Run();
    }
    if (!installed) return false;
    // a clean process for the game, where the loader can start the program again
    if (job.restart && RestartSelf()) return false;
    return true;
}
}  // namespace CaneSwitch
