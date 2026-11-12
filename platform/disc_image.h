#pragma once
// PlayStation 2 disc images as the installers read them: a plain .iso, a CHD (MAME's compressed disc image, as chdman
// createdvd and createcd write it), read with libchdr (Libraries/libchdr, cmake/chd.cmake), or an .iso split into parts
// for a FAT32 drive (imageParts). Sly 2's conversion and Sly 3's install read them as they are; the extractions that
// read .iso files get one written out (writeImageAsIso).
#include "games.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cane {
// A CHD by its header.
bool chdFile(const std::filesystem::path& file);

// The parts of an .iso split for a FAT32 drive (no file of 4 GB or more), in order; empty when `path` is not one:
// - a folder of parts 00, 01, ...: the Switch's concatenation file, as DBI or NXMTP write it (Horizon reads it as one
//   file while the folder keeps its archive bit; this reads it when the bit was lost, copied on a PC);
// - numbered files from the first one: name.iso.000 / .001 / ... (.00, .0; 7-Zip and HJSplit count from .001).
std::vector<std::filesystem::path> imageParts(const std::filesystem::path& path);

// A disc image's 2048-byte sectors as one stream, laid out as the .iso's.
class DiscImage {
public:
    virtual ~DiscImage() = default;
    virtual uint64_t size() const = 0;
    virtual bool read(uint64_t offset, void* data, size_t size) = 0;
    DiscReader reader() {
        return [this](uint64_t offset, void* data, size_t size) { return read(offset, data, size); };
    }
    // The image (a CHD: its DVD data, or its CD's data track; split parts: joined); nullptr with `error` set when it
    // cannot be read.
    static std::unique_ptr<DiscImage> open(const std::filesystem::path& file, std::string* error = nullptr);
};

// identifyDisc (games.h) for any of them.
DiscIdentity identifyDiscImage(const std::filesystem::path& file);

// A CHD or a split .iso written out as one .iso (removed again on failure): "" when done, "cancelled", or what went
// wrong. `progress(done, total)` in bytes; false cancels.
std::string writeImageAsIso(const std::filesystem::path& image, const std::filesystem::path& iso,
                            const std::function<bool(uint64_t, uint64_t)>& progress = {});
}  // namespace cane
