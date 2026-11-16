# SafeHouse

SafeHouse is an offline launcher with native PC ports of the Sly Cooper games, for Linux (including the Steam Deck),
Windows, macOS and iPhone / iPad:

- **Sly Cooper and the Thievius Raccoonus** (Sly 1): USA, Europe, Japan and Korea discs.
- **Sly 2: Band of Thieves**: USA, Europe (v1.00 and v2.01) and Japan discs.
- **Sly 3: Honor Among Thieves**: in development.

SafeHouse contains no game data. You install each game from your own PlayStation 2 disc image (`.iso`).

SafeHouse was called ProjectCane until November 2026. It is not affiliated with or endorsed by Sony Interactive
Entertainment or Sucker Punch Productions.

## Start

| Package | How to start it |
| --- | --- |
| `SafeHouse-Launcher-Linux-x86_64.AppImage` (Linux) | Make it executable and open it. Without FUSE: `--appimage-extract-and-run`. |
| `SafeHouse-Launcher-Linux-x86_64.tar.gz` (Linux) | Extract it and open **SafeHouse**. |
| `SafeHouse-Launcher-Windows-x86_64-Setup.exe` | Installs for your Windows account into `%LOCALAPPDATA%\Programs\SafeHouse`, with a Start Menu entry. |
| `SafeHouse-Launcher-Windows-x86_64-Portable.zip` | Extract it into a writable folder and open **SafeHouse.exe**. Keep `portable.txt` beside `bin`. |
| `SafeHouse-Launcher-macOS-arm64.dmg` / `-x86_64.dmg` (macOS, Apple silicon / Intel; Sly 2 only for now) | Drag **SafeHouse** to Applications, then open it with right-click > **Open** the first time. |
| `SafeHouse-iOS.ipa` (iPhone and iPad, iOS / iPadOS 15 or newer; Sly 2 only, in testing) | Sideload it (see **iPhone and iPad** below). |

Then pick a game, choose **Install** and select (or drag in) your disc image. The image is checked first: the game,
the region, the free space needed, and whether the image is complete. When the install is done, choose **Play**.

On the Steam Deck, install in Desktop Mode, then use **Add to Steam** on the game's page (exit Steam first, or
restart it afterwards). Run the Linux package natively; do not force Proton.

## iPhone and iPad

SafeHouse is not in the App Store: install `SafeHouse-iOS.ipa` with a sideloading tool and a free Apple ID (no paid
developer account is needed, and the app asks for nothing a free account cannot grant).

- **AltStore** (with AltServer on a Mac or Windows PC) or **SideStore** (on the device, after a one-time setup with a
  computer): add the `.ipa` in the app's **My Apps** tab. Both refresh the app's signature in the background.
- **Sideloadly** (Mac or Windows): connect the device, drop the `.ipa` in, sign in with your Apple ID.

With a free Apple ID the signature lasts **7 days**: refresh it before then (AltStore / SideStore do it on their own
when they can reach their server; with Sideloadly install again), or the app stops opening until you do. A free
account can have **3 sideloaded apps** at a time. The first start may need **Settings > General > VPN & Device
Management** > your Apple ID > **Trust**, and on iOS 16 or newer **Settings > Privacy & Security > Developer Mode**.

To install a game, choose **Browse…** on its Install page and pick your disc image in the file picker (it is copied
into the app, about 3 GB for Sly 2, and removed after the install unless **Settings > Games > Keep imported disc
images** is on), or copy the image into SafeHouse's folder with the **Files** app first. Play with the on-screen
controls or a controller (Xbox, PlayStation, MFi); connecting a controller hides the touch controls. After quitting a
game, close SafeHouse in the app switcher and open it again to start another session.

## Where your files are

| | Installed games | Saves, settings and mods |
| --- | --- | --- |
| Linux | `~/.local/share/SafeHouse/installations` | `~/.config/SafeHouse` (Sly 2: `sly2/`) |
| Windows setup | `%LOCALAPPDATA%\SafeHouse\installations` | `%LOCALAPPDATA%\SafeHouse\user` (Sly 2: `sly2\`) |
| Windows portable | `data\installations` beside `bin` | `data\user` beside `bin` |
| macOS | `~/Library/Application Support/SafeHouse/installations` | `~/Library/Application Support/SafeHouse/user` |
| iPhone / iPad | **Files** > **On My iPhone / iPad** > **SafeHouse** > `installations` | the same folder's `user` (Sly 2: `sly2/`) |

Until 2026-11-10 these folders had the project's old name (`projectcane` on Linux, `ProjectCane` on Windows). The first start of a newer launcher moves them to `SafeHouse` once: games, saves, settings, mods and logs carry over, and paths stored in the launcher's settings are updated. If both folders exist, files the new folder already has win and differing old versions are kept in `ProjectCane leftovers` inside it; nothing is deleted before it is in the new place. **Settings > Games > Install games to** puts new installations in another folder or on another drive (they go to a `SafeHouse` folder there); **Move…** moves an installed game. Re-extracting or uninstalling a game never touches your saves.

## On the game page

- **Region:** with more than one region installed, pick which one **Play** starts. **Settings > Games > Install
  another region** adds one. Saves are shared between a game's regions.
- **Add to Steam** and **Add desktop shortcut:** start the game directly, without the launcher window.
- **Update needed / Re-extract:** after an update that changed the installed files, the game is extracted again from
  your disc image. Your saves are kept.

**Settings** has each game's options: display, audio volumes, controls (button prompts, **Controller vibration**)
and more. **Settings > Advanced > Debug mode** turns on the debug keys and Sly 2's debug menu.

## Updates

When the launcher starts it asks GitHub whether a newer SafeHouse exists (**Settings > Launcher > Check for
updates**, on by default; **Check for updates now** is beside it). Only the launcher's version is sent. When there is
a new version you see what is new and how many days older your build is, with **Update now**, **Later** and **Skip
this version**.

**Update now** downloads the package for your kind of installation, checks it against the SHA-256 published with the
release and restarts SafeHouse as the new version. If a game or an install is running, the restart waits for it.
Nothing else changes: installed games, saves, settings, mods and a portable copy's `data` folder are never touched.

| Installation | How it is updated |
| --- | --- |
| AppImage | The AppImage file is replaced where it is, keeping its name and permissions. |
| Linux archive (`.tar.gz`) | The files in its folder are replaced. |
| Windows setup | The installed folder is updated in place, without running the setup; Apps & features shows the new version and the uninstaller keeps working. |
| Windows portable | The folder is updated in place; `portable.txt` and `data` stay as they are. |
| macOS | `SafeHouse.app` is replaced. Keep it in Applications (or another folder you can write to). |

If a file cannot be replaced (another program holds it open, or the folder is read-only), the old files are put back
and SafeHouse keeps working as before; **Open download page** then leads to the release. Each release lists its
SHA-256 checksums in `SHA256SUMS`.

## Mods

Sly 1 and Sly 2 read a **Mods** folder in your settings folder (`Mods` for Sly 1, `sly2/Mods` for Sly 2) that
replaces textures, music, dialog, text and movies without changing the installed game. The guide is `MODDING.md`
beside this file.

## Problems

Turn on **Save logs** in **Settings > Advanced**, reproduce the problem, then open **Log folder** there and attach
`launcher.log` to your report.

Licences of the included libraries are in `THIRD_PARTY_NOTICES.md` and the `licenses` folder.
