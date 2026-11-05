#pragma once
// Controller prompt icon atlases (Kenney Input Prompts 1.5A, CC0), one texture
// per family plus a keyboard/mouse atlas for launcher hints. Shared by the
// game and the launcher.
//
// The atlases are packed by tools/pack_prompts.py into assets/prompts and
// embedded by cmake/prompts.cmake, which defines CANE_HAVE_PROMPT_ATLAS for
// targets linking cane_prompts. Without it every query reports "unavailable"
// and callers fall back to buttonLabel() text.
//
// GL entry points are fetched through glfwGetProcAddress, so this header works
// with glad, with <GL/gl.h> or with no GL header included at all.
#include "controller_family.h"
#ifdef CANE_HAVE_PROMPT_ATLAS
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#endif
namespace cane::input {
// keyUV() ids for the mouse (GLFW key codes are all >= -1).
inline constexpr int kMouseLeftKey = -100, kMouseRightKey = -101, kMouseMiddleKey = -102, kMouseWheelUpKey = -103,
                     kMouseWheelDownKey = -104, kMouseBackKey = -105, kMouseForwardKey = -106,
                     kMouseMoveKey = -110,      // the mouse moved (camera / aim)
                     kArrowKeysKey = -111;      // the four arrow keys as one cap
// keyUV() id of a settings key binding (a GLFW key, or settings::kMouseBindingBase + button / the wheel).
inline int promptKeyForBinding(int binding) {
    switch (binding - settings::kMouseBindingBase) {
    case 0: return kMouseLeftKey;
    case 1: return kMouseRightKey;
    case 2: return kMouseMiddleKey;
    case 3: return kMouseBackKey;
    case 4: return kMouseForwardKey;
    case settings::kMouseButtonCount: return kMouseWheelUpKey;
    case settings::kMouseButtonCount + 1: return kMouseWheelDownKey;
    default: return binding;
    }
}
struct IconUV {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    bool valid = false;
};

#ifdef CANE_HAVE_PROMPT_ATLAS
struct PromptRect {
    int x, y, w, h;
};
struct PromptKeyRect {
    int key;
    PromptRect rect;
};
// A controller picture (prompts_devices.png): the family, its body and underlay cells, and the cells' top left in
// the Kenney SVG's 64 x 64 units.
struct PromptDeviceRect {
    int family;
    PromptRect body, under;
    float ux0, uy0;
};
struct PromptBlobId {
    unsigned size;
    unsigned long long fnv1a64;
};
namespace prompt_atlas {
// kPromptAtlasCount, kPromptAtlasSize, kPromptAtlasBlob, kPromptRects and
// kPromptKeyRects. Atlas index == Family; index 0 (Keyboard) is the keyboard
// and mouse atlas.
#include "prompt_atlas_rects.inc"
static_assert(kPromptAtlasCount == kFamilyCount);
static_assert(sizeof(kPromptRects[0]) / sizeof(kPromptRects[0][0]) == kButtonCount);
} // namespace prompt_atlas
namespace embedded {
// Defined in assets/prompts/prompt_data.cpp, which cane_prompts compiles into
// every target that links it.
struct Image {
    int width = 0, height = 0;
    unsigned char* rgba = nullptr; // straight alpha, top row first
};
const unsigned char* promptAtlasPng(int index, std::size_t* size);
Image decodePromptAtlas(int index); // rgba == nullptr on failure
Image decodePromptDevices();        // the controller pictures (prompts_devices.png)
void freeImage(Image& image);
} // namespace embedded

namespace detail {
#if defined(_WIN32) && !defined(_WIN64)
#define CANE_PROMPT_GLAPI __stdcall
#else
#define CANE_PROMPT_GLAPI
#endif
// The handful of GL calls the upload needs, resolved from the current context.
struct PromptGl {
    using GenTextures = void(CANE_PROMPT_GLAPI*)(int, unsigned*);
    using DeleteTextures = void(CANE_PROMPT_GLAPI*)(int, const unsigned*);
    using BindTexture = void(CANE_PROMPT_GLAPI*)(unsigned, unsigned);
    using BindBuffer = void(CANE_PROMPT_GLAPI*)(unsigned, unsigned);
    using TexParameteri = void(CANE_PROMPT_GLAPI*)(unsigned, unsigned, int);
    using TexImage2D = void(CANE_PROMPT_GLAPI*)(unsigned, int, int, int, int, int, unsigned, unsigned,
                                                 const void*);
    using GenerateMipmap = void(CANE_PROMPT_GLAPI*)(unsigned);
    using GetIntegerv = void(CANE_PROMPT_GLAPI*)(unsigned, int*);
    using PixelStorei = void(CANE_PROMPT_GLAPI*)(unsigned, int);
    GenTextures genTextures = nullptr;
    DeleteTextures deleteTextures = nullptr;
    BindTexture bindTexture = nullptr;
    BindBuffer bindBuffer = nullptr; // GL 2.1+, optional
    TexParameteri texParameteri = nullptr;
    TexImage2D texImage2D = nullptr;
    GenerateMipmap generateMipmap = nullptr; // GL 3.0+, optional
    GetIntegerv getIntegerv = nullptr;
    PixelStorei pixelStorei = nullptr;
    bool load() {
        if (!glfwGetCurrentContext()) return false;
        auto get = [](auto& fn, const char* name) { fn = reinterpret_cast<std::decay_t<decltype(fn)>>(glfwGetProcAddress(name)); };
        get(genTextures, "glGenTextures");
        get(deleteTextures, "glDeleteTextures");
        get(bindTexture, "glBindTexture");
        get(bindBuffer, "glBindBuffer");
        get(texParameteri, "glTexParameteri");
        get(texImage2D, "glTexImage2D");
        get(generateMipmap, "glGenerateMipmap");
        get(getIntegerv, "glGetIntegerv");
        get(pixelStorei, "glPixelStorei");
        return genTextures && deleteTextures && bindTexture && texParameteri && texImage2D && getIntegerv &&
               pixelStorei;
    }
};
#undef CANE_PROMPT_GLAPI
// GL enums (core values; no GL header needed).
inline constexpr unsigned kGlTexture2D = 0x0DE1, kGlTextureBinding2D = 0x8069, kGlRgba = 0x1908,
                          kGlUnsignedByte = 0x1401, kGlTextureMinFilter = 0x2801, kGlTextureMagFilter = 0x2800,
                          kGlLinear = 0x2601, kGlLinearMipmapLinear = 0x2703, kGlTextureWrapS = 0x2802,
                          kGlTextureWrapT = 0x2803, kGlClampToEdge = 0x812F, kGlTextureMaxLevel = 0x813D,
                          kGlUnpackAlignment = 0x0CF5, kGlUnpackRowLength = 0x0CF2, kGlUnpackSkipRows = 0x0CF3,
                          kGlUnpackSkipPixels = 0x0CF4, kGlPixelUnpackBuffer = 0x88EC,
                          kGlPixelUnpackBufferBinding = 0x88EF;
// Cells keep >= 8 px of transparent margin plus 8 px of bleed padding, so
// mip level 4 (1/16) is the deepest one that does not mix neighbours.
inline constexpr int kPromptMaxMipLevel = 4;
inline IconUV promptUV(const PromptRect& rect, int atlas) {
    if (rect.w <= 0 || rect.h <= 0) return {};
    const float w = static_cast<float>(prompt_atlas::kPromptAtlasSize[atlas].w);
    const float h = static_cast<float>(prompt_atlas::kPromptAtlasSize[atlas].h);
    // Unflipped upload (first PNG row at t = 0), like the game's font quads.
    return { rect.x / w, (rect.y + rect.h) / h, (rect.x + rect.w) / w, rect.y / h, true };
}
} // namespace detail

class PromptAtlas { // GL only, no ImGui types
public:
    // Creates one GL texture per atlas (mipmapped, clamped, GL state restored).
    // Needs a current GL context. Idempotent; returns false when no atlas is
    // available or the upload failed (nothing is then kept).
    bool upload() {
        if (m_uploaded) return true;
        detail::PromptGl gl;
        if (!gl.load()) return false;
        using namespace detail;
        int previousTexture = 0, previousUnpack = 0, alignment = 4, rowLength = 0, skipRows = 0, skipPixels = 0;
        gl.getIntegerv(kGlTextureBinding2D, &previousTexture);
        if (gl.bindBuffer) gl.getIntegerv(kGlPixelUnpackBufferBinding, &previousUnpack);
        gl.getIntegerv(kGlUnpackAlignment, &alignment);
        gl.getIntegerv(kGlUnpackRowLength, &rowLength);
        gl.getIntegerv(kGlUnpackSkipRows, &skipRows);
        gl.getIntegerv(kGlUnpackSkipPixels, &skipPixels);
        if (gl.bindBuffer && previousUnpack) gl.bindBuffer(kGlPixelUnpackBuffer, 0);
        gl.pixelStorei(kGlUnpackAlignment, 4);
        gl.pixelStorei(kGlUnpackRowLength, 0);
        gl.pixelStorei(kGlUnpackSkipRows, 0);
        gl.pixelStorei(kGlUnpackSkipPixels, 0);
        bool ok = true;
        std::array<unsigned, kFamilyCount> textures{};
        for (int atlas = 0; atlas < kFamilyCount && ok; ++atlas) {
            embedded::Image image = embedded::decodePromptAtlas(atlas);
            if (!image.rgba || image.width != prompt_atlas::kPromptAtlasSize[atlas].w ||
                image.height != prompt_atlas::kPromptAtlasSize[atlas].h) {
                embedded::freeImage(image);
                ok = false;
                break;
            }
            gl.genTextures(1, &textures[atlas]);
            gl.bindTexture(kGlTexture2D, textures[atlas]);
            gl.texParameteri(kGlTexture2D, kGlTextureWrapS, static_cast<int>(kGlClampToEdge));
            gl.texParameteri(kGlTexture2D, kGlTextureWrapT, static_cast<int>(kGlClampToEdge));
            gl.texParameteri(kGlTexture2D, kGlTextureMagFilter, static_cast<int>(kGlLinear));
            gl.texImage2D(kGlTexture2D, 0, static_cast<int>(kGlRgba), image.width, image.height, 0, kGlRgba,
                          kGlUnsignedByte, image.rgba);
            embedded::freeImage(image);
            if (gl.generateMipmap) {
                gl.texParameteri(kGlTexture2D, kGlTextureMaxLevel, kPromptMaxMipLevel);
                gl.generateMipmap(kGlTexture2D);
                gl.texParameteri(kGlTexture2D, kGlTextureMinFilter, static_cast<int>(kGlLinearMipmapLinear));
            } else {
                gl.texParameteri(kGlTexture2D, kGlTextureMaxLevel, 0);
                gl.texParameteri(kGlTexture2D, kGlTextureMinFilter, static_cast<int>(kGlLinear));
            }
            ok = textures[atlas] != 0;
        }
        unsigned devices = 0;
        if (ok) {
            embedded::Image image = embedded::decodePromptDevices();
            if (image.rgba && image.width == prompt_atlas::kPromptDevicesSize.w &&
                image.height == prompt_atlas::kPromptDevicesSize.h) {
                gl.genTextures(1, &devices);
                gl.bindTexture(kGlTexture2D, devices);
                gl.texParameteri(kGlTexture2D, kGlTextureWrapS, static_cast<int>(kGlClampToEdge));
                gl.texParameteri(kGlTexture2D, kGlTextureWrapT, static_cast<int>(kGlClampToEdge));
                gl.texParameteri(kGlTexture2D, kGlTextureMagFilter, static_cast<int>(kGlLinear));
                gl.pixelStorei(kGlUnpackAlignment, 1);
                gl.texImage2D(kGlTexture2D, 0, static_cast<int>(kGlRgba), image.width, image.height, 0, kGlRgba,
                              kGlUnsignedByte, image.rgba);
                gl.pixelStorei(kGlUnpackAlignment, 4);
                if (gl.generateMipmap) {
                    gl.texParameteri(kGlTexture2D, kGlTextureMaxLevel, 2);
                    gl.generateMipmap(kGlTexture2D);
                    gl.texParameteri(kGlTexture2D, kGlTextureMinFilter, static_cast<int>(kGlLinearMipmapLinear));
                } else {
                    gl.texParameteri(kGlTexture2D, kGlTextureMaxLevel, 0);
                    gl.texParameteri(kGlTexture2D, kGlTextureMinFilter, static_cast<int>(kGlLinear));
                }
            }
            embedded::freeImage(image);
        }
        gl.bindTexture(kGlTexture2D, static_cast<unsigned>(previousTexture));
        if (gl.bindBuffer && previousUnpack) gl.bindBuffer(kGlPixelUnpackBuffer, static_cast<unsigned>(previousUnpack));
        gl.pixelStorei(kGlUnpackAlignment, alignment);
        gl.pixelStorei(kGlUnpackRowLength, rowLength);
        gl.pixelStorei(kGlUnpackSkipRows, skipRows);
        gl.pixelStorei(kGlUnpackSkipPixels, skipPixels);
        if (!ok) {
            for (unsigned& texture : textures)
                if (texture) gl.deleteTextures(1, &texture);
            return false;
        }
        m_textures = textures;
        m_devices = devices;
        m_uploaded = true;
        return true;
    }
    bool uploaded() const { return m_uploaded; }
    // Deletes the textures (needs the context they were created in). A later
    // upload() creates them again, e.g. after the GL context was recreated.
    void release() {
        detail::PromptGl gl;
        if (m_uploaded && gl.load())
        {
            for (unsigned& texture : m_textures)
                if (texture) gl.deleteTextures(1, &texture);
            if (m_devices) gl.deleteTextures(1, &m_devices);
        }
        m_textures = {};
        m_devices = 0;
        m_uploaded = false;
    }
    // 0 when unavailable: callers then draw buttonLabel() text. Keyboard has
    // no button icons and always returns 0 (see keyboardTexture()).
    unsigned texture(Family family) const {
        const int index = static_cast<int>(family);
        if (family == Family::Keyboard || index < 0 || index >= kFamilyCount) return 0;
        return m_textures[index];
    }
    // Unflipped upload, same convention as the game's font quads:
    // v0 = (y + h) / H (bottom edge), v1 = y / H (top edge). For ImGui use
    // uv0 = {u0, v1} (top left) and uv1 = {u1, v0} (bottom right). Icons are
    // square. valid == false before upload() and when the family has no icon
    // for the button (PlayStation has no Guide icon).
    IconUV uv(Family family, Button button) const {
        const int atlas = static_cast<int>(family), index = static_cast<int>(button);
        if (!m_uploaded || family == Family::Keyboard || atlas < 0 || atlas >= kFamilyCount || index < 0 ||
            index >= kButtonCount)
            return {};
        return detail::promptUV(prompt_atlas::kPromptRects[atlas][index], atlas);
    }
    // Keycaps: Enter, Escape, Tab, Space, Backspace, the arrows, Shift/Ctrl/Alt
    // (either side), A-Z, 0-9 (and the keypad's), F1-F12, punctuation,
    // Insert/Delete/Home/End/Page Up/Down, Caps Lock (GLFW key codes) and the
    // mouse (kMouse*Key, kArrowKeysKey).
    // Other keys, and every key before upload(), return valid == false.
    unsigned keyboardTexture() const { return m_textures[static_cast<int>(Family::Keyboard)]; }
    IconUV keyUV(int glfwKey) const {
        if (!m_uploaded) return {};
        for (const PromptKeyRect& key : prompt_atlas::kPromptKeyRects)
            if (key.key == glfwKey) return detail::promptUV(key.rect, static_cast<int>(Family::Keyboard));
        return {};
    }

    // The controller picture of a family (Sly 2's pause Controls page): the texture (0 when none) and its cells.
    // uv as uv(): v0 the bottom edge.
    unsigned deviceTexture() const { return m_devices; }
    const PromptDeviceRect* device(Family family) const {
        if (!m_devices) return nullptr;
        for (const PromptDeviceRect& d : prompt_atlas::kPromptDeviceRects)
            if (d.family == static_cast<int>(family)) return &d;
        return nullptr;
    }
    static IconUV deviceUV(const PromptRect& rect) {
        const float w = static_cast<float>(prompt_atlas::kPromptDevicesSize.w);
        const float h = static_cast<float>(prompt_atlas::kPromptDevicesSize.h);
        return { rect.x / w, (rect.y + rect.h) / h, (rect.x + rect.w) / w, rect.y / h, true };
    }
    static constexpr float devicePxPerUnit() { return static_cast<float>(prompt_atlas::kPromptDevicePxPerUnit); }

private:
    std::array<unsigned, kFamilyCount> m_textures{};
    unsigned m_devices = 0;
    bool m_uploaded = false;
};
#else
class PromptAtlas { // no atlas built in: every query reports "unavailable"
public:
    bool upload() { return false; }
    bool uploaded() const { return false; }
    void release() {}
    unsigned texture(Family) const { return 0; }
    IconUV uv(Family, Button) const { return {}; }
    unsigned keyboardTexture() const { return 0; }
    IconUV keyUV(int) const { return {}; }
    unsigned deviceTexture() const { return 0; }
    const void* device(Family) const { return nullptr; }
};
#endif
inline PromptAtlas& promptAtlas() {
    static PromptAtlas atlas;
    return atlas;
}
} // namespace cane::input
