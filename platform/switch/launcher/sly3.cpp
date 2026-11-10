// Sly 3 on the Switch: its installation only, ahead of its port. The disc is installed as the PC's extractor does it
// (File Extractor/main.cpp ExtractDiscImage, platform/install.h): the image kept whole as disc.iso (the game reads
// SLY3.WAD by sector), its system files beside it and installation.json, so the port finds the PC's layout.
#include "../installer.h"
#include "../runtime.h"
#include "../../disc_image.h"
#include "../../install.h"

#include <switch.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace
{
namespace fs = std::filesystem;

std::string Hex(const u8* hash)
{
    char text[SHA256_HASH_SIZE * 2 + 1];
    for (size_t i = 0; i < SHA256_HASH_SIZE; ++i) std::snprintf(text + i * 2, 3, "%02x", hash[i]);
    return text;
}

// (an .iso or a CHD: platform/disc_image.h)
std::string Convert(const std::string& isoPath, const std::string& directory, const CaneSwitch::InstallProgress& progress)
{
    const cane::DiscImageInstall& d = *cane::discImageInstall(cane::GameId::Sly3);
    std::string error;
    const std::unique_ptr<cane::DiscImage> image = cane::DiscImage::open(isoPath, &error);
    if (!image) return error;
    if (!progress(0.0f, "Checking your ISO", fs::path(isoPath).filename().string())) return "cancelled";
    const cane::IsoVolume volume = cane::readIsoVolume(image->reader());
    const uint64_t imageSize = uint64_t(volume.blocks) * cane::kIsoSector;
    // the release: its executable with its SHA-1, the volume's size, the system files and SLY3.WAD where the game reads them
    const cane::DiscVariant* variant = nullptr;
    for (size_t k = 0; k < d.variantCount && !variant && volume.valid; ++k)
    {
        const cane::DiscVariant& v = d.variants[k];
        const cane::IsoFile* exe = volume.find(v.bootFile);
        if (!exe || volume.blocks != v.volumeBlocks || image->size() < imageSize) continue;
        std::vector<uint8_t> executable(exe->size);
        if (image->read(uint64_t(exe->sector) * cane::kIsoSector, executable.data(), executable.size()) &&
            cane::sha1Hex(executable.data(), executable.size()) == v.executableSha1)
            variant = &v;
    }
    bool valid = variant != nullptr;
    for (size_t i = 0; valid && i < variant->systemFileCount; ++i)
    {
        const cane::IsoFile* file = volume.find(variant->systemFiles[i]);
        valid = file && uint64_t(file->sector) * cane::kIsoSector + file->size <= imageSize;
    }
    if (valid && d.dataFile)
    {
        const cane::IsoFile* file = volume.find(d.dataFile);
        valid = file && file->sector == d.dataFileSector && uint64_t(file->sector) * cane::kIsoSector + file->size <= imageSize;
    }
    if (!valid) return "That disc image is damaged or not the original Sly 3 disc (USA, SCUS-97464).";

    // one pass: the image copied and hashed
    cane::json files = cane::json::array();
    {
        std::ofstream copy(fs::path(directory) / d.imageName, std::ios::binary | std::ios::trunc);
        if (!copy) return "The disc image copy cannot be created.";
        Sha256Context sha;
        sha256ContextCreate(&sha);
        std::vector<char> buffer(4u << 20);
        for (uint64_t copied = 0; copied < imageSize;)
        {
            const size_t chunk = size_t(std::min<uint64_t>(buffer.size(), imageSize - copied));
            if (!image->read(copied, buffer.data(), chunk)) return "The disc image could not be read.";
            copy.write(buffer.data(), std::streamsize(chunk));
            if (!copy) return "The disc image could not be copied (is the SD card full?).";
            sha256ContextUpdate(&sha, buffer.data(), chunk);
            copied += chunk;
            if (!progress(float(double(copied) / double(imageSize)), "Copying the disc image", d.imageName)) return "cancelled";
        }
        copy.close();
        if (!copy) return "The disc image could not be copied (is the SD card full?).";
        u8 hash[SHA256_HASH_SIZE];
        sha256ContextGetHash(&sha, hash);
        files.push_back({{"path", d.imageName}, {"size", imageSize}, {"sha256", Hex(hash)}});
    }
    for (size_t i = 0; i < variant->systemFileCount; ++i)
    {
        const char* name = variant->systemFiles[i];
        const cane::IsoFile* file = volume.find(name);
        std::vector<uint8_t> data(file->size);
        if (!image->read(uint64_t(file->sector) * cane::kIsoSector, data.data(), data.size()))
            return std::string("The disc image's ") + name + " could not be read.";
        std::ofstream out(fs::path(directory) / name, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
        if (!out) return std::string(name) + " could not be written.";
        u8 hash[SHA256_HASH_SIZE];
        sha256CalculateHash(hash, data.data(), data.size());
        files.push_back({{"path", name}, {"size", data.size()}, {"sha256", Hex(hash)}});
    }
    const cane::json manifest = {{"schema", 1}, {"game", cane::game(d.game).key}, {"asset_version", d.assetVersion},
                                 {"extractor_version", "0.2.0"}, {"disc", variant->bootFile}, {"serial", variant->serial},
                                 {"region", variant->region}, {"disc_sha1", variant->executableSha1}, {"iso_path", isoPath},
                                 {"image", {{"path", d.imageName}, {"size", imageSize}, {"sectors", volume.blocks}}},
                                 {"files", files}};
    std::ofstream out(fs::path(directory) / "installation.json", std::ios::trunc);
    out << manifest.dump(2) << '\n';
    if (!out) return "installation.json could not be written.";
    return {};
}

// installation.json current for Sly 3 (platform/install.h manifestCurrent) and the image of its size beside it
bool Installed(const std::string& directory)
{
    std::ifstream in(fs::path(directory) / "installation.json");
    if (!in) return false;
    const cane::json manifest = cane::json::parse(in, nullptr, false);
    if (!cane::manifestCurrent(manifest, cane::GameId::Sly3)) return false;
    uint64_t expected = 0;
    if (const auto image = manifest.find("image"); image != manifest.end() && image->is_object())
        if (const auto size = image->find("size"); size != image->end() && size->is_number_unsigned()) expected = size->get<uint64_t>();
    std::error_code ec;
    const uint64_t size = fs::file_size(fs::path(directory) / cane::discImageInstall(cane::GameId::Sly3)->imageName, ec);
    return !ec && expected && size == expected;
}

const CaneSwitch::InstallJob s_install = {
    int(cane::GameId::Sly3),
    "Select a Sly 3: Honor Among Thieves ISO or CHD (USA).",
    Convert,
    nullptr, nullptr,
    false,   // (nothing runs after it yet: back to the launcher)
    true,    // (a CHD read as it is: the image is copied once)
};
}  // namespace

// Sly 3's descriptor for the launcher (launcher.cpp): installed and installable, without a main yet.
extern "C" const CaneSwitch::Game cane_game_sly3;
const CaneSwitch::Game cane_game_sly3 = {"sly3", "Sly 3: Honor Among Thieves", Installed, &s_install, nullptr};
