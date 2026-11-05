// See mods.h.
#include "mods.h"

#include "content.h"
#include "../platform/platform.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace
{
fs::path s_modsDirectory;
std::once_flag s_indexOnce;
// lower-case generic relative path -> the mod file
std::unordered_map<std::string, fs::path> s_index;

std::string Key(const fs::path& relative)
{
    const auto generic = relative.lexically_normal().generic_u8string();
    std::string key(generic.begin(), generic.end());
    std::transform(key.begin(), key.end(), key.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return key;
}

bool FOwnFolder(const std::string& key)
{
    // Textures and Text are read by their own loaders, never as installed files.
    return key.rfind("textures/", 0) == 0 || key.rfind("text/", 0) == 0;
}

void BuildIndex()
{
    if (s_modsDirectory.empty())
        return;
    std::error_code error;
    if (!fs::is_directory(s_modsDirectory, error))
    {
        std::printf("mods: no mods folder at %s\n", cane::utf8(s_modsDirectory).c_str());
        return;
    }
    std::size_t textures = 0;
    std::size_t texts = 0;
    for (fs::recursive_directory_iterator it(s_modsDirectory,
             fs::directory_options::skip_permission_denied, error), end;
         it != end; it.increment(error))
    {
        if (error)
        {
            error.clear();
            continue;
        }
        if (!it->is_regular_file(error))
            continue;
        const std::string key = Key(it->path().lexically_relative(s_modsDirectory));
        if (key.rfind("textures/", 0) == 0) ++textures;
        else if (key.rfind("text/", 0) == 0) ++texts;
        if (FOwnFolder(key))
            continue;
        s_index.emplace(key, it->path());
    }
    std::printf("mods: %zu files, %zu textures, %zu text files from %s\n", s_index.size(), textures, texts,
        cane::utf8(s_modsDirectory).c_str());
}

const std::unordered_map<std::string, fs::path>& Index()
{
    std::call_once(s_indexOnce, BuildIndex);
    return s_index;
}
}  // namespace

void SetModsDirectory(const fs::path& directory)
{
    s_modsDirectory = directory.empty() ? fs::path() : fs::absolute(directory);
    if (s_modsDirectory.empty())
        std::printf("mods: off\n");
}

const fs::path& ModsDirectory()
{
    return s_modsDirectory;
}

fs::path InstallationRelativePath(const fs::path& installed)
{
    const CONTENT_PACKAGE& package = ContentPackage();
    if (!package.manifestLoaded || package.root.empty() || installed.empty())
        return {};
    const fs::path relative = fs::absolute(installed).lexically_normal().lexically_relative(
        fs::absolute(package.root).lexically_normal());
    if (relative.empty() || *relative.begin() == "..")
        return {};
    return relative;
}

bool FModFileRelative(const fs::path& relative, fs::path& found)
{
    if (s_modsDirectory.empty() || relative.empty())
        return false;
    const auto& index = Index();
    if (index.empty())
        return false;
    const auto it = index.find(Key(relative));
    if (it == index.end())
        return false;
    found = it->second;
    return true;
}

fs::path ModPath(const fs::path& installed)
{
    if (s_modsDirectory.empty())
        return installed;
    fs::path found;
    if (FModFileRelative(InstallationRelativePath(installed), found))
    {
        // (logged once per file: dialog lines and sounds are opened many times)
        static std::mutex s_logMutex;
        static std::unordered_set<std::string> s_logged;
        std::lock_guard<std::mutex> lock(s_logMutex);
        if (s_logged.insert(cane::utf8(found)).second)
            std::printf("mods: using %s\n", cane::utf8(found).c_str());
        return found;
    }
    return installed;
}
