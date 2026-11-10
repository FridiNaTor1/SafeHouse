#pragma once
// PlayStation 2 disc images as the installers read them: a plain .iso, or a CHD (MAME's compressed disc image, as
// chdman createdvd and createcd write it), read with libchdr (Libraries/libchdr, cmake/chd.cmake). A CHD is read sector
// by sector to identify it, and written out as an .iso for the extraction, which reads .iso files.
#include "games.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace cane {
// A CHD by its header.
bool chdFile(const std::filesystem::path& file);

// A disc image's 2048-byte sectors as one stream, laid out as the .iso's.
class DiscImage {
public:
    virtual ~DiscImage() = default;
    virtual uint64_t size() const = 0;
    virtual bool read(uint64_t offset, void* data, size_t size) = 0;
    DiscReader reader() {
        return [this](uint64_t offset, void* data, size_t size) { return read(offset, data, size); };
    }
    // The image (a CHD: its DVD data, or its CD's data track); nullptr with `error` set when it cannot be read.
    static std::unique_ptr<DiscImage> open(const std::filesystem::path& file, std::string* error = nullptr);
};

// identifyDisc (games.h) for an .iso or a CHD.
DiscIdentity identifyDiscImage(const std::filesystem::path& file);

// The CHD written out as an .iso (removed again on failure): "" when done, "cancelled", or what went wrong.
// `progress(done, total)` in bytes; false cancels.
std::string writeChdAsIso(const std::filesystem::path& chd, const std::filesystem::path& iso,
                          const std::function<bool(uint64_t, uint64_t)>& progress = {});
}  // namespace cane
