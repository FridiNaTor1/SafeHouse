#pragma once
// The games' texture mod options (Settings.ini [Graphics] Texture Cache / Texture Streaming: sly2_settings.h,
// Sly2-Release texmod.h): their defaults per platform, in one place so a device test can flip one.
namespace cane::settings {
// The texture options' defaults. The cache: off on the Switch until it is measured on the console (it costs about two
// thirds of a pack's size on the SD card again, 130 MB for Sly 2's PS3 HD pack, to save the PNG decode, which the
// streaming already takes off the game thread there); on elsewhere. The streaming: on, but on Android, whose GLES
// draw path has not been tested with textures that change after their first draw.
#if defined(__SWITCH__)
inline constexpr int kTextureCacheDefault = 0;
#else
inline constexpr int kTextureCacheDefault = 1;
#endif
#if defined(__ANDROID__)
inline constexpr int kTextureStreamingDefault = 0;
#else
inline constexpr int kTextureStreamingDefault = 1;
#endif
}  // namespace cane::settings
