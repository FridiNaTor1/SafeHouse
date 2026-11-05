# Launcher artwork

`safehouse-logo.png` is the SafeHouse logo: the mask and cane emblem by
**HoshiYumeHime** (DeviantArt), the artist of the app icon too. The launcher
shows it at the top of the game rail and on the About page, with "SafeHouse"
set beside it in the launcher typeface (the emblem has no lettering).
`scripts/make-safehouse-logo.py SOURCE.png` makes it from the artist's original
(1800x2100 RGBA, not in the repository): trimmed to its visible pixels, scaled
to 512 px high (drawn at 60-120 px times the UI scale, with mipmaps) and given a
thin blue-white rim, because the art's black outline and the mask's dark navy
otherwise disappear on the launcher's dark background. It replaced a
placeholder wordmark (2026-11-05), which had replaced the ProjectCane logo by
k1nomoto at the rename (2026-11-03); both are in the git history.

The application icon (the cane) is by HoshiYumeHime (DeviantArt) as well.
`safehouse-icon.png` is an optimized 256px PNG export with transparent margins
trimmed and proportions preserved. `safehouse.ico` contains 16, 24, 32, 48, 64,
128 and 256px exports for the Windows launcher, setup wizard and shortcuts.

Artwork is embedded in the launcher and included in desktop/AppImage resources.
Source images are not needed at runtime.

`games/*-logo.png` are the game list logos (Sly 1-3, Thieves in Time and
Bentley's Hackpack), converted from the supplied WebP files with Pillow:
cropped to their visible pixels and scaled to at most 720px wide, with alpha.
They are trademarks of Sony Interactive Entertainment, used to identify the
games (see THIRD_PARTY_NOTICES.md).

`AtkinsonHyperlegible-Regular.ttf` and `-Bold.ttf` are the launcher typeface
(SIL Open Font License, `OFL.txt`).

`steam/<game>/{grid,wide,hero,logo,icon}` are the Steam library images that the
launcher's "Add to Steam" button copies into Steam's grid folder. They come from
SteamGridDB; sources and authors are in `steam/CREDITS.md`.

`shortcut/<game>-logo.png` / `.ico` are the icons of the desktop shortcuts the
launcher makes ("Add desktop shortcut", Sly 1 and Sly 2): the game's logo from
`games/`, trimmed and centred on a transparent 256x256 square (padded, not
squashed). The PNG is embedded in the launcher, which writes it to
`$XDG_DATA_HOME/icons/hicolor/256x256/apps/safehouse-<game>.png` for a Linux
.desktop file; the .ico (16-256 px) is embedded in safehouse.exe for a Windows
.lnk. `shortcut/<game>.ico` (without `-logo`) is the Windows icon of the game
programs themselves (Sly1-Release.exe / Sly2-Release.exe, window and taskbar),
made from the SteamGridDB icon in `steam/<game>/icon.png`. All of them come from
`scripts/make-shortcut-icons.py`.
