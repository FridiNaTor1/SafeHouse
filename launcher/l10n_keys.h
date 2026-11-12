#pragma once
// Launcher strings whose text comes from outside launcher/ (or from data) and is translated where it is shown with
// tr(): scripts/test-launcher-l10n.py reads the N_() literals here like any other. Region names (regions.h
// regionName), the prompt-style choices (platform/controller_family.h), the Sly 1 / Sly 2 language names (the games'
// manifests and language folders), the languages' English names (launcher/lang/*.json "english") and the Sly 2
// Settings.ini option names the Collection Style Launcher shows (platform/sly2_settings.h).
#include "l10n.h"

namespace cane::l10n {
inline constexpr const char* kExternalKeys[] = {
    // regions
    N_("USA"), N_("Europe"), N_("Japan"), N_("Korea"), N_("Unknown region"),
    // prompt styles / controller families
    N_("Automatic"), N_("Keyboard and mouse"),
    // game and launcher languages
    N_("English"), N_("English (UK)"), N_("French"), N_("German"), N_("Italian"), N_("Spanish"), N_("Dutch"), N_("Portuguese"),
    N_("Danish"), N_("Finnish"), N_("Norwegian"), N_("Swedish"), N_("Japanese"), N_("Korean"), N_("Chinese"), N_("Polish"),
    N_("Russian"),
    // the games' one-line descriptions (platform/games.h GameInfo::blurb)
    N_("Sly's first heist: recover the Thievius Raccoonus from the Fiendish Five."),
    N_("The gang reunites to steal the scattered parts of Clockwerk."),
    N_("Sly assembles a crew to crack the Cooper family vault."),
    N_("Sly travels through history to save the Thievius Raccoonus."),
    N_("Bentley's collection of hacking mini-games."),
    // Sly 2 options shown by name (Settings.ini keys)
    N_("Music Volume"), N_("Effects Volume"), N_("Vibration"),
};
}  // namespace cane::l10n
