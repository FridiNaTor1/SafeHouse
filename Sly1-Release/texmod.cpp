// See texmod.h.
#include "texmod.h"

#include "mods.h"
#include "shd.h"
#include "../platform/platform.h"

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>
#include <stb/stb_image_write.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

namespace fs = std::filesystem;

namespace
{
constexpr uint32_t kTextureRgba8 = 0;
constexpr uint32_t kTextureBc1 = 1;
constexpr uint32_t kTextureBc3 = 2;
constexpr uint32_t kGenerateMipmaps = 1u << 2;   // (world textures; fonts lack it)

fs::path s_dumpDirectory, s_packDirectory;
int s_replaced = 0;
int s_unchanged = 0;
int s_dumped = 0;

uint64_t HashRgba(const std::vector<uint8_t>& rgba)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (uint8_t b : rgba) h = (h ^ b) * 0x100000001b3ull;
    return h;
}

void Rgb565(uint16_t c, uint8_t* rgb)
{
    const unsigned r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    rgb[0] = uint8_t((r << 3) | (r >> 2));
    rgb[1] = uint8_t((g << 2) | (g >> 4));
    rgb[2] = uint8_t((b << 3) | (b >> 2));
}

// BC1 (as GL's RGB DXT1: opaque) and BC3 colour blocks; the rebuilder writes them with stb_dxt (4-colour mode).
void DecodeColorBlock(const uint8_t* block, uint8_t* rgba, uint32_t stride, bool opaqueBc1)
{
    const uint16_t c0 = uint16_t(block[0] | (block[1] << 8)), c1 = uint16_t(block[2] | (block[3] << 8));
    uint8_t palette[4][4]{};
    Rgb565(c0, palette[0]);
    Rgb565(c1, palette[1]);
    const bool fourColor = !opaqueBc1 || c0 > c1;
    for (int k = 0; k < 3; ++k)
    {
        if (fourColor)
        {
            palette[2][k] = uint8_t((2 * palette[0][k] + palette[1][k]) / 3);
            palette[3][k] = uint8_t((palette[0][k] + 2 * palette[1][k]) / 3);
        }
        else
        {
            palette[2][k] = uint8_t((palette[0][k] + palette[1][k]) / 2);
            palette[3][k] = 0;
        }
    }
    for (auto& entry : palette) entry[3] = 255;
    const uint32_t indices = uint32_t(block[4]) | (uint32_t(block[5]) << 8) | (uint32_t(block[6]) << 16) |
        (uint32_t(block[7]) << 24);
    for (int p = 0; p < 16; ++p)
    {
        const uint8_t* c = palette[(indices >> (2 * p)) & 3];
        uint8_t* d = rgba + (p / 4) * stride + (p % 4) * 4;
        d[0] = c[0]; d[1] = c[1]; d[2] = c[2]; d[3] = c[3];
    }
}

void DecodeAlphaBlock(const uint8_t* block, uint8_t* rgba, uint32_t stride)
{
    const unsigned a0 = block[0], a1 = block[1];
    unsigned palette[8]{a0, a1};
    if (a0 > a1)
        for (int i = 1; i < 7; ++i) palette[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    else
    {
        for (int i = 1; i < 5; ++i) palette[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        palette[6] = 0;
        palette[7] = 255;
    }
    uint64_t indices = 0;
    for (int i = 0; i < 6; ++i) indices |= uint64_t(block[2 + i]) << (8 * i);
    for (int p = 0; p < 16; ++p)
        rgba[(p / 4) * stride + (p % 4) * 4 + 3] = uint8_t(palette[(indices >> (3 * p)) & 7]);
}

// Level 0 of the image as RGBA8, rows as stored.
bool DecodeLevel0(const BMP& image, std::vector<uint8_t>& rgba)
{
    const uint32_t w = image.bmpWidth, h = image.bmpHeight;
    const size_t bytes = size_t(w) * h * 4;
    if (image.textureFormat == kTextureRgba8)
    {
        if (image.diffuseTexture.size() < bytes) return false;
        rgba.assign(image.diffuseTexture.begin(), image.diffuseTexture.begin() + bytes);
        return true;
    }
    if (image.textureFormat != kTextureBc1 && image.textureFormat != kTextureBc3) return false;
    const bool bc1 = image.textureFormat == kTextureBc1;
    const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4, cb = bc1 ? 8 : 16;
    if (image.diffuseTexture.size() < size_t(bw) * bh * cb) return false;
    // decoded into a block-padded buffer, then cropped
    const uint32_t pw = bw * 4, ph = bh * 4;
    std::vector<uint8_t> padded(size_t(pw) * ph * 4);
    for (uint32_t by = 0; by < bh; ++by)
        for (uint32_t bx = 0; bx < bw; ++bx)
        {
            const uint8_t* block = image.diffuseTexture.data() + (size_t(by) * bw + bx) * cb;
            uint8_t* d = padded.data() + (size_t(by) * 4 * pw + bx * 4) * 4;
            DecodeColorBlock(bc1 ? block : block + 8, d, pw * 4, bc1);
            if (!bc1) DecodeAlphaBlock(block, d, pw * 4);
        }
    rgba.resize(bytes);
    for (uint32_t y = 0; y < h; ++y)
        std::copy_n(padded.data() + size_t(y) * pw * 4, size_t(w) * 4, rgba.data() + size_t(y) * w * 4);
    return true;
}

// World textures are stored bottom row first (GL's order); fonts, which the rebuilder flips, top row first.
bool FStoredFlipped(const BMP& image)
{
    return (image.textureFlags & kGenerateMipmaps) != 0;
}

void FlipRows(std::vector<uint8_t>& rgba, uint32_t w, uint32_t h)
{
    const size_t row = size_t(w) * 4;
    for (uint32_t y = 0; y < h / 2; ++y)
        std::swap_ranges(rgba.begin() + y * row, rgba.begin() + (y + 1) * row, rgba.begin() + (h - 1 - y) * row);
}

std::string TextureName(const std::vector<uint8_t>& rgba, uint32_t w, uint32_t h)
{
    char name[64];
    std::snprintf(name, sizeof(name), "%ux%u_%016llx.png", w, h, static_cast<unsigned long long>(HashRgba(rgba)));
    return name;
}

// The replacement textures by name (lower case): the mods folder's, then the texture pack's (--texture-pack) where
// the mods folder has none of that name. Scanned once.
const std::unordered_map<std::string, fs::path>& ModTextures()
{
    static std::once_flag s_once;
    static std::unordered_map<std::string, fs::path> s_names;
    std::call_once(s_once, [] {
        auto scan = [](const fs::path& directory, const char* what) {
            std::error_code error;
            if (directory.empty() || !fs::is_directory(directory, error)) return;
            size_t count = 0;
            for (const auto& entry : fs::directory_iterator(directory, error))
            {
                std::string name = cane::utf8(entry.path().filename());
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                if (entry.is_regular_file(error) && name.size() > 4 && name.compare(name.size() - 4, 4, ".png") == 0 &&
                    s_names.emplace(name, entry.path()).second)
                    ++count;
            }
            std::printf("mods: %zu textures in %s%s\n", count, cane::utf8(directory).c_str(), what);
        };
        if (!ModsDirectory().empty()) scan(ModsDirectory() / "Textures", "");
        if (!s_packDirectory.empty()) scan(s_packDirectory / "Textures", " (texture pack)");
    });
    return s_names;
}
}  // namespace

void SetTexturePackDirectory(const fs::path& directory)
{
    s_packDirectory = directory;
}

bool DecodeTextureLevel0(const BMP& image, std::vector<uint8_t>& rgba)
{
    return DecodeLevel0(image, rgba);
}

std::string TextureModName(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height)
{
    return TextureName(rgba, width, height);
}

void SetTextureDumpDirectory(const fs::path& directory)
{
    s_dumpDirectory = directory;
    std::error_code error;
    fs::create_directories(directory, error);
    std::printf("textures: dumping to %s\n", cane::utf8(directory).c_str());
}

bool FTextureModsActive()
{
    return !s_dumpDirectory.empty() || !ModTextures().empty();
}

bool FTextureReplacement(const BMP& image, std::vector<uint8_t>& rgba, uint32_t& width, uint32_t& height)
{
    if (!FTextureModsActive()) return false;
    std::vector<uint8_t> original;
    if (!DecodeLevel0(image, original)) return false;
    const uint32_t w = image.bmpWidth, h = image.bmpHeight;
    const std::string name = TextureName(original, w, h);

    if (!s_dumpDirectory.empty())
    {
        const fs::path path = s_dumpDirectory / name;
        std::error_code error;
        if (!fs::exists(path, error))
        {
            std::vector<uint8_t> upright = original;
            if (FStoredFlipped(image)) FlipRows(upright, w, h);
            if (!stbi_write_png(cane::utf8(path).c_str(), int(w), int(h), 4, upright.data(), int(w) * 4))
                std::printf("textures: cannot write %s\n", cane::utf8(path).c_str());
            else
                ++s_dumped;
        }
    }

    std::string key = name;
    std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    const auto found = ModTextures().find(key);
    if (found == ModTextures().end()) return false;
    const fs::path path = found->second;
    int pw = 0, ph = 0;
    stbi_uc* pixels = stbi_load(cane::utf8(path).c_str(), &pw, &ph, nullptr, 4);
    if (pixels == nullptr || pw <= 0 || ph <= 0 || pw > 16384 || ph > 16384)
    {
        std::printf("mods: cannot read %s\n", cane::utf8(path).c_str());
        if (pixels) stbi_image_free(pixels);
        return false;
    }
    rgba.assign(pixels, pixels + size_t(pw) * ph * 4);
    stbi_image_free(pixels);
    width = uint32_t(pw);
    height = uint32_t(ph);
    if (FStoredFlipped(image)) FlipRows(rgba, width, height);
    if (width == w && height == h && rgba == original)
    {
        ++s_unchanged;
        return false;   // (unchanged: keep the original)
    }
    ++s_replaced;
    return true;
}

void ReportTextureMods()
{
    if (s_replaced != 0 || s_unchanged != 0)
        std::printf("mods: %d textures replaced, %d unchanged (the original kept)\n", s_replaced, s_unchanged);
    if (s_dumped != 0)
        std::printf("textures: %d new textures written to %s\n", s_dumped, cane::utf8(s_dumpDirectory).c_str());
    s_replaced = s_unchanged = s_dumped = 0;
}
