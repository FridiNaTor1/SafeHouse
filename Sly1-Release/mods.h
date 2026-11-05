#pragma once

// Sly 1's Mods folder (MODDING.md, "Sly 1"): a file in <mods>/<path> replaces the installed file at the same path
// below the installation folder (the folder holding extraction_manifest.json). Textures (mods/Textures) and text
// (mods/Text) have their own loaders (texmod.h, textmod.h).
//
// The mods folder is --mods DIR, else $SLY1_MODS, else <user dir>/Mods. An empty value turns mods off.

#include <cstdint>
#include <filesystem>
#include <string>

// Sets the mods folder. Called once at startup, before any file is opened.
void SetModsDirectory(const std::filesystem::path& directory);
// The mods folder, or an empty path when mods are off.
const std::filesystem::path& ModsDirectory();
// The mod file replacing `installed`, or `installed` itself. Only files below the installation are replaced.
std::filesystem::path ModPath(const std::filesystem::path& installed);
// True and the mod file when `relative` (a path relative to the installation) has a replacement.
bool FModFileRelative(const std::filesystem::path& relative, std::filesystem::path& found);
// `installed` relative to the installation folder, or an empty path when it lies outside it.
std::filesystem::path InstallationRelativePath(const std::filesystem::path& installed);
