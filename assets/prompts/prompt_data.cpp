// Embedded prompt atlases and SDL_GameControllerDB, compiled into every target
// that links cane_prompts (cmake/prompts.cmake). The data headers are generated
// from assets/prompts/*.png and Libraries/gamecontrollerdb/gamecontrollerdb.txt
// with cmake/embed-image.cmake.
#ifndef GLFW_INCLUDE_NONE
#define GLFW_INCLUDE_NONE
#endif
#include <cstddef>
#include <string_view>

#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
// A private PNG-only decoder: the game and the launcher each have their own
// stb_image implementation, which this must not clash with.
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "prompts_keyboard_png.h"
#include "prompts_playstation_png.h"
#include "prompts_xbox_png.h"
#include "prompts_nintendo_png.h"
#include "prompts_steamdeck_png.h"
#include "prompts_steamcontroller_png.h"
#include "prompts_ps4_png.h"
#include "prompts_ps5_png.h"
#include "prompts_steamcontroller2026_png.h"
#include "prompts_gamecube_png.h"
#include "prompts_switch2_png.h"
#include "prompts_wii_png.h"
#include "prompts_wiiu_png.h"
#include "prompts_metaquest_png.h"
#include "prompts_valveindex_png.h"
#include "prompts_steamframe_png.h"
#include "prompts_vivewand_png.h"
#include "prompts_pico_png.h"
#include "prompts_devices_png.h"
#include "gamecontrollerdb_txt.h"
// Declarations (checked against the definitions below).
#include "button_prompts.h"
#include "gamepad_mappings.h"

namespace cane::input::embedded {
namespace {
struct Blob {
    const unsigned char* data;
    std::size_t size;
};
// Order == cane::input::Family.
const Blob kAtlases[] = {
    { cane_prompts_keyboard, sizeof(cane_prompts_keyboard) },
    { cane_prompts_playstation, sizeof(cane_prompts_playstation) },
    { cane_prompts_xbox, sizeof(cane_prompts_xbox) },
    { cane_prompts_nintendo, sizeof(cane_prompts_nintendo) },
    { cane_prompts_steamdeck, sizeof(cane_prompts_steamdeck) },
    { cane_prompts_steamcontroller, sizeof(cane_prompts_steamcontroller) },
    { cane_prompts_ps4, sizeof(cane_prompts_ps4) },
    { cane_prompts_ps5, sizeof(cane_prompts_ps5) },
    { cane_prompts_steamcontroller2026, sizeof(cane_prompts_steamcontroller2026) },
    { cane_prompts_gamecube, sizeof(cane_prompts_gamecube) },
    { cane_prompts_switch2, sizeof(cane_prompts_switch2) },
    { cane_prompts_wii, sizeof(cane_prompts_wii) },
    { cane_prompts_wiiu, sizeof(cane_prompts_wiiu) },
    { cane_prompts_metaquest, sizeof(cane_prompts_metaquest) },
    { cane_prompts_valveindex, sizeof(cane_prompts_valveindex) },
    { cane_prompts_steamframe, sizeof(cane_prompts_steamframe) },
    { cane_prompts_vivewand, sizeof(cane_prompts_vivewand) },
    { cane_prompts_pico, sizeof(cane_prompts_pico) },
};
constexpr int kAtlasCount = static_cast<int>(sizeof(kAtlases) / sizeof(kAtlases[0]));
} // namespace

const unsigned char* promptAtlasPng(int index, std::size_t* size) {
    if (index < 0 || index >= kAtlasCount) {
        if (size) *size = 0;
        return nullptr;
    }
    if (size) *size = kAtlases[index].size;
    return kAtlases[index].data;
}
Image decodePromptAtlas(int index) {
    Image image;
    if (index < 0 || index >= kAtlasCount) return image;
    image.rgba = stbi_load_from_memory(kAtlases[index].data, static_cast<int>(kAtlases[index].size), &image.width,
                                       &image.height, nullptr, 4);
    if (!image.rgba) image.width = image.height = 0;
    return image;
}
Image decodePromptDevices() {
    Image image;
    image.rgba = stbi_load_from_memory(cane_prompts_devices, static_cast<int>(sizeof(cane_prompts_devices)), &image.width,
                                       &image.height, nullptr, 4);
    if (!image.rgba) image.width = image.height = 0;
    return image;
}
void freeImage(Image& image) {
    if (image.rgba) stbi_image_free(image.rgba);
    image = {};
}
std::string_view gameControllerDb() {
    return { reinterpret_cast<const char*>(cane_gamecontrollerdb), sizeof(cane_gamecontrollerdb) };
}
} // namespace cane::input::embedded
