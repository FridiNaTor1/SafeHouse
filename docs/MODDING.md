# Modding Sly 1 and Sly 2 in SafeHouse

SafeHouse installs each game from your own disc into a folder of ordinary files. This guide covers where those files
are, how to replace textures, music, dialog, text, movies and levels, which tools come with SafeHouse, and what is
safe to change.

Both games have a **Mods folder**: you drop replacement files into it, and the game uses them instead of the
originals. The installation itself is never touched, so removing a file from Mods brings the original back.

---

## Sly 2: Band of Thieves

### Where things are

| What | Linux | Windows | Portable SafeHouse |
|---|---|---|---|
| The installation (read only for mods) | `~/.local/share/SafeHouse/installations/sly2` | `%LOCALAPPDATA%\SafeHouse\installations\sly2` | `data/installations/sly2` |
| **Your Mods folder** | `~/.config/SafeHouse/sly2/Mods` | `%LOCALAPPDATA%\SafeHouse\user\sly2\Mods` | `data/user/sly2/Mods` |

- A second region, such as Europe installed beside USA, is in `installations/sly2-europe`. The Mods folder is shared
  by both.
- SafeHouse's **Open game folder** button opens the installation.
- Create the `Mods` folder if it doesn't exist.
- When the game starts with mods, its log prints `mods: N files from ...`.

The installation looks like this:

```text
sly2/
  extraction_manifest.json        the index: every file, what it is, and which levels use it
  Worlds/<N> <Episode>/<Level>/
      <Level>.world.json          the level's parts, in the order the game reads them (see "Levels")
      World/Text.json             its text and subtitles, every language (editable)
      World/<nnn> <group>/        one folder per group of the level (Sly, Murray, a guard, a door, the level itself):
          Scripts.splice              its scripts, as text (editable)
          Graphics.bin                its textures and shaders
          Objects.bin                 its objects: models, animations, actors (the index lists each one)
      World/Groups.bin, Sound.bin, Progress.bin, ...   the level's other blocks
      <Level>.bnk                 its sound effects' definitions
      Sounds/Sound <n>.wav        its sound effects, one WAV per sample
      Ambience.wav                its background ambience
      Screens/Screen <id>.brx     a menu or HUD screen only this level uses
  Worlds/Front End/Front End.brx  the title screen and menus
  Screens/Screen <id>.brx         HUD screens every level uses
  Music/<id> <name>.wav           the music, for example "28 Paris.wav"
  Dialog/<Language>/<Level>/<line>.wav   a voice line ("Shared" holds lines every level uses)
  Movies/<id> <name>.m2v          a movie's picture, for example "2009 Ep1 Intro.m2v"
  Movies/<id> <name>.wav          its sound
```

The audio is ordinary WAV (16-bit PCM), which any audio editor opens. On the European discs, with 11 languages, the
voice lines and the movies' sound are **FLAC** instead (lossless, about 60 % of the size; Audacity and most editors
open it): `Dialog/French/Paris/1259.flac`, `Movies/French/2062 Ep1 Intro.flac`. The game plays exactly the sound
of the disc from these files.

Examples: `Worlds/1 The Black Chateau/Paris/World/227 jt/Scripts.splice` (Sly's scripts in Paris), `Music/18 The Jungle.wav`,
`Movies/2013 Ep2 Operation Hippo Drop.m2v`.

**The rule: a file in Mods at the same path as a file of the installation replaces it.** For example,
`Mods/Music/28 Paris.wav` replaces `Music/28 Paris.wav`.

Music and movies also match by their **number** alone:

- `Mods/Music/28.wav` or `Mods/Music/28 My Theme.wav` replaces track 28.
- `Mods/Movies/2009.m2v` replaces movie 2009's picture, `Mods/Movies/2009.wav` its sound.

The PS2 formats of older SafeHouse versions still work in Mods: a `.vpk` replaces a music track, a `.vag` a voice
line, a `.pss` a whole movie (picture and sound).

Use the number when you share a mod: names can change between SafeHouse versions, numbers don't. Movie numbers
differ between the USA and European discs: the European Ep 1 intro is 2062. Music numbers are the same on both.

### The tools

The tools are options of the game program **Sly2-Release** (`Sly2-Release.exe` on Windows), which is in SafeHouse's
program folder. Run them from a terminal.

**The audio tools need no game data:**

```sh
Sly2-Release --export-wav "<installation>/Music/28 Paris.wav" paris-normal.wav --stem 1
Sly2-Release --make-music "Mods/Music/28 Paris.wav" mytrack.wav --like "<installation>/Music/28 Paris.wav"
```

Voice lines, movie sound and sound effects need no tool: put a WAV in Mods.

**The dumps need the installation** (`--disc` takes the installation folder):

```sh
Sly2-Release --disc "<installation>" --dump-text text
Sly2-Release --disc "<installation>" --dump-textures textures 2
```

`--dump-textures` takes a level by its record number, as listed in `extraction_manifest.json` under `worlds`:
2 is Paris, `boot` is the museum prologue, `frontend` is the menus.

To try a different Mods folder, use `--mods DIR`. An empty value (`--mods ""`) starts the game with no mods.

### Textures

1. Find the texture. **The installation already has every texture:** the `Textures` folder in the game folder (the
   launcher's **Open game folder**) holds all 7051 of them as `<W>x<H>_<hash>.png` (about 80 MB). The name
   identifies the texture by its contents.

   - Installed before 2026-11-04 and no `Textures` folder? Press **Export textures** on the launcher's Sly 2 page
     (about 10 seconds), or run `Sly2-Release --data-dir "<installation>" --export-textures`. No re-extract needed.
   - Only one level's textures: `Sly2-Release --disc "<installation>" --dump-textures dump 2` writes what Paris
     draws to `dump/`.

2. **Copy** the PNG into `Mods/Textures/` under the **same file name**, and edit the copy. (Leave `Textures`
   itself alone: it is the reference, and the game never reads it.)

The texture is then replaced wherever the game draws it: in every level, on characters, on HUD screens, in
animated (multi-frame) textures and in particles.

- **Higher resolution:** a replacement is used **at its own resolution, not stretched or scaled back down**. The
  game's texture coordinates run 0..1 across the texture, so a 4x PNG (a 64x64 texture as 256x256) maps exactly like
  the original and shows four times the detail. The width and height don't need to be the original's multiple or
  aspect, but keeping them a whole multiple (2x, 4x) keeps every detail where the original had it.
- **Filtering:** each replacement gets mipmaps, so a large texture doesn't shimmer in the distance. Textures the
  game draws smooth are drawn smooth (trilinear, with anisotropic filtering), and textures it draws with sharp
  pixels stay sharp.
- **Recommended limits:** 4x the original (most textures become 128x128 to 1024x1024; the largest, 512x128, becomes
  2048x512) and at most 4096 pixels a side. A whole level at 4x uses about 300 to 550 MB of video memory in place of
  about 25 MB, and loads about half a second slower. Only the current level's textures are kept. On the Steam Deck
  or a 2 GB card, use 2x for whole-level packs and save 4x for close-up details. 8x and above costs 4x as much again,
  for detail the camera rarely gets close enough to see. Replacements over 16384 pixels or over the GPU's limit are
  ignored.
- **Alpha:** 255 is fully opaque. The game's own range is half that, and SafeHouse converts it both ways, so a
  dumped PNG put back unchanged looks identical. Upscaled edges with in-between alpha are fine.
- **Texture cache:** the first time a replacement is drawn, the game keeps its decoded picture in a cache file, so the
  next load does not read and decode the PNG again. A PNG you add or change is picked up by itself (the cache goes by
  the file's name, size and time; for a pack's `Textures.pak` by that file's size and time), and a removed one is
  dropped. The picture is the same with and without the cache. It lives in your cache folder (Linux:
  `~/.cache/SafeHouse/sly2/Textures`, Windows: `%LOCALAPPDATA%\SafeHouse\Cache\sly2\Textures`, macOS:
  `~/Library/Caches/SafeHouse/sly2/Textures`, a portable copy: `data/cache/sly2/Textures`, Switch and Android:
  `Cache/Textures` in the Sly 2 profile folder) and can be deleted at any time: `Sly2-Release --clear-texture-cache`
  does it. `Sly2-Release --data-dir "<installation>" --build-texture-cache` (with `--texture-pack DIR` for a pack)
  fills it ahead of time. `Texture Cache = Off` under `[Graphics]` in `Settings.ini` turns it off; on the Nintendo
  Switch it is off unless you set it to `On` (it takes about two thirds of the pack's size on the SD card again).
  (Sly 3 has the same cache, with `sly3` in place of `sly2`.)
- **Texture streaming:** replacements load on the processor's other cores while the game goes on, so a texture can
  show its original for a moment when it first appears instead of the game pausing; a level starts with its
  replacements in place where the computer is fast enough. `Texture Streaming = Off` under `[Graphics]` in
  `Settings.ini` loads each replacement the moment it is first drawn instead.
- **Shared textures:** a texture used by several levels has the same name in each, so one PNG covers all of them.
- **Animated textures and particles:** each frame is its own PNG. Give all the frames of one animation the same
  size (particle frames of another size are scaled to the first replaced frame's size).
- Start the game with `SLY2N_MODSTATS=1` set to have its log list each replacement it uploads and the video memory
  they use.
- **Converting by hand:** `Sly2-Release --disc "Sly 2.iso" --convert sly2-files` writes the `Textures` folder too;
  `--no-export-textures` leaves it out.

### The PS3 HD textures (The Sly Collection)

If you own **The Sly Collection** for PS3, SafeHouse can take its redrawn textures (up to 4x sharper) and use them
over Sly 2's: **Settings > Games > Sly 2: Band of Thieves > Import PS3 HD textures**. Choose the PS3 disc image
(`.iso`), a `.zip` of it (read as it is, nothing is unpacked), or the extracted disc (the folder with `PS3_GAME`).
It takes about 6 minutes and writes one file of about 200 MB (5989 textures), `PS3 HD/Textures.pak`, in your Sly 2
profile folder (`~/.config/SafeHouse/sly2/PS3 HD`, `%LOCALAPPDATA%\SafeHouse\user\sly2\PS3 HD`).

- `Textures.pak` holds the PNGs under the same `<W>x<H>_<hash>.png` names as any texture pack, in one file that loads
  and copies much faster than thousands. Your own `Mods/Textures` win over it, so you can still change single
  textures.
- **Extract as PNG files** makes the next import write the PNG files in `PS3 HD/Textures` instead, to look through or
  edit them. The game reads either.
- **Use the PS3 HD textures** turns it on or off; **Remove** deletes it. The game itself never reads the PS3 disc:
  levels, scripts and gameplay stay the PS2's.
- Without the launcher: `Sly2-Release --data-dir "<installation>" --ps3-hd-textures "<PS3 disc, .iso or .zip>" OUT`
  builds the pack (add `--ps3-hd-png` for the PNG files), and `--texture-pack OUT` uses it.
- A pack imported before `Textures.pak` (a `PS3 HD/Textures` folder of PNGs) still works.
- On the Nintendo Switch, copy the `PS3 HD` folder to `sdmc:/switch/SafeHouse/user/sly2/`. The game's cover in
  SafeHouse shows an HD tag when it is found.
- Only the textures are taken: the PS3's cutscenes, music and voices are the same quality as the PS2's.

### Music

Each music track is one WAV. Most free-roam tracks have three **stems** the game cross-fades:

- **sneak** (mono), when Sly is sneaking;
- **normal** (stereo);
- **alert** (stereo), when guards spot you.

The WAV has a channel for each: Paris's five channels are sneak, normal left, normal right, alert left, alert right.
Battles, chases and menus have one plain stereo stem. The WAV also says where the loop starts (its `smpl` chunk,
which samplers and many editors show as a loop marker) and which channel is which stem (a `stem` chunk).

**The simplest replacement** is any WAV, for example a stereo song at `Mods/Music/28.wav`. It plays the same whether
you sneak or get spotted, and loops whole.

**With stems and a loop point**, build the track with `--make-music`:

```sh
Sly2-Release --make-music "Mods/Music/28 Paris.wav" normal.wav \
    --like "<installation>/Music/28 Paris.wav" --sneak sneak.wav --alert alert.wav --loop 14.9
```

- **`--like`** copies the original's stem layout, so the game's mixing keeps working. Always use it with the
  track you replace. Without `--like` you get one plain stereo stem.
- **Stems:** `--sneak` and `--alert` are optional. Without them, the normal WAV is used for every stem, which
  means your track plays the same whether you sneak or get spotted.
- **`--loop SECONDS`** is where the music jumps back to after the end. The default, 0, loops the whole file.
  `--loop none` plays the track once. The loop is sample-exact.
- **WAV formats:** 16-bit or 24-bit PCM or 32-bit float, mono or stereo. 44100 Hz is what the game uses. Other rates
  play too.
- **Getting the original:** `--export-wav` gives one stem of the original as a WAV, with its loop point: `--stem 1`
  the normal one (the default), `--stem 0` the sneak one, `--stem 2` the alert one.
- An edited copy of an installed track that lost its `stem` chunk (some editors drop unknown chunks) still mixes
  right when it keeps the original's channel count: the game takes the stems from the installed track.

Each level's background ambience (`Ambience.wav`) works the same way.

Track numbers and where they play are in the table below.

| # | Track | # | Track |
|---:|---|---:|---|
| 36 | Museum (prologue) | 34, 35 | Episode menus |
| 28 | Paris (Ep 1 hub) | 29 | Wine Cellar (Ep 1 interiors) |
| 23 | India (Ep 2 hub) | 24 | Guest Rooms (Ep 2 interiors) |
| 18 | The Jungle (Ep 3 hub) | 19 | Spice Factory |
| 14 | Prague (Ep 4 hub) | 15 | Prison |
| 10 | Contessa's Castle (Ep 5 hub) | 11 | The Crypt (Ep 5 interiors) |
| 7 | Train Station (Ep 6 hub) | 8 | Cabins (the train) |
| 3, 4 | Sawmill hub, Sawmill interiors (Ep 7) | 6 | Jean Bison Battle |
| 0 | Arpeggio's Airship (Ep 8 hub) | 1 | Clockwerk Assembly (blimp interiors) |
| 2 | Clock-La Battle | | others: jobs, chases and bosses, named by place |

### Dialog

Voice lines are mono WAV files (22050 Hz, 16-bit):

- `Dialog/<Language>/<Level>/<line>.wav`: a line one level uses;
- `Dialog/<Language>/Shared/<line>.wav`: a line every level uses.

To replace one:

1. Find the line: listen to the WAVs. The manifest lists every line with its level and length.
2. Record your line and save it as a WAV, ideally 22050 Hz mono, like the game's. Any rate, mono or stereo, plays.
3. Put it at the same path in Mods: `Mods/Dialog/English/Paris/1259.wav`.

The subtitle's timing and the lip sync come from the level, not the audio, so keep your line about as long as the
original. The manifest's `seconds` field gives the original length.

On European installations every line exists once per language, as FLAC: `Dialog/French/Paris/1259.flac`. A WAV or a
FLAC at that path (`Mods/Dialog/French/Paris/1259.wav` works too) replaces it.

### Text and subtitles

All on-screen text (menus, HUD, job names, hints, subtitles) can be replaced per language with
`Mods/Text/<Language>.json`:

1. Dump the current strings. This writes one file per language on your disc (`English.json`; on European discs also
   `French.json`, `German.json` ...):

   ```sh
   Sly2-Release --disc "<installation>" --dump-text text
   ```

2. Copy the strings you want to change into your own file:

   ```json
   {
     "strings": {
       "5": "Press START button for a New Heist",
       "305": "Press &2X&. to play as Sly."
     },
     "worlds": {
       "Paris": { "306": "This line only changes in Paris." }
     }
   }
   ```

3. Save it as `Mods/Text/English.json` (or `French.json`, ...).

How the file works:

- **`strings`** applies everywhere; **`worlds`**, keyed by the level's name as in the installation's folders, applies
  only in that level and wins over `strings`.
- **Keys** are the string numbers from the dump. You only need the strings you change. You can't add new numbers.
- **Encoding:** write the file as UTF-8. The USA and European fonts have the Western European (Latin-1)
  characters, such as é, ß, ñ and å, and the discs' own typographic marks (’ ‘ “ ” … – — Œ œ); other characters
  show as `?`. (Files made by versions before 2026-11-17 have those marks as control characters, U+0092 for ’ and
  U+0085 for …: the game reads both the same.) On the Japanese disc (`Japanese.json`), only
  characters the game's Japanese font has can show.
- **Button icons:** sequences like `&2X&.` (the X button), `&2T&.` (Triangle) and `&2S&.` (Square) draw the game's
  button icons. Keep them.
- **Comments:** `//` comment lines are allowed in the file.

A translation is a full `<Language>.json` with every string changed.

**SafeHouse's own PC strings** (the pause menu's Exit to Desktop, "Hold (O) to skip", the display, controls and
keyboard options, Replay Jobs, key and mouse names, the Remap page's actions) are not in the disc's text. A language
folder's `Text.json` (below: `Languages`, or `Mods/Languages/<Language>/Text.json`) can translate them too, in a
`"port"` object keyed by name:

```json
{
  "strings": { ... },
  "port": {
    "ExitToDesktop": "Far út á skriviborðið",
    "HoldToSkip": "Halt &2O&. inni fyri at leypa um",
    "KeyLeftMod": "Vinstri %s",
    "Action.jump": "Loyp",
    "ActionGroup.Movement": "Rørsla",
    "DecimalMark": ","
  }
}
```

- The names are the strings' names in the game's table (Sly2-Release/porttext.h, `PTX_<name>`), `Action.<id>` and
  `ActionGroup.<Words>` for the Remap page, and `DecimalMark` (`.` or `,`, the options' numbers).
- `Sly2-Release --dump-port-text DIR` writes every name with its English text and the game's 11 other built-in
  languages (`DIR/English.json`, `French.json`, ...; no disc needed). `--dump-text` puts the same `"port"` object in
  each language's file.
- Strings the object leaves out come from the built-in table: the language's own (one of the 12 the table has), else
  the one its `language.json` `"text"` or `"code"` names (`"code": "nb"`: Norwegian), else English.
- Keep each string's `%s` / `%d` in the English order: a string whose `%`-codes differ is not used (the game says so in
  its log, and `--check-language` fails). Keep the `&2O&.` icons. The fonts' rules below apply as for the game's text.
- `"worlds"` (a level's own text) works in the languages folder's `Text.json` too, as in `Mods/Text`.

### Languages (new languages, dubs and subtitles)

Every Sly 2 installation (USA, Europe, Japan) shares one **languages folder**, `sly2-languages`, beside the game
folders (the launcher: Settings > Games > Sly 2 > Languages > Languages folder). Each language is a folder in it:
`English`, `French`, `Japanese`, `English (UK)`... Installing a disc puts its languages there; the launcher can also
import another region's languages from a disc image, or French and Spanish subtitles from The Sly Collection (PS3).
Every language in the folder is offered on the title screen of every disc (Triangle opens the list; Settings.ini's
`[Language]` remembers the choice). The options' Language... page chooses the text and the voices apart (Japanese
voices with English text, ...; `Language` and `Voices` in Settings.ini); a language without its own `Dialog` lines
is offered for the text only. Choosing on the title screen sets both.

A language folder holds any of these:

| File | What |
|---|---|
| `Text.json` | the strings, in the `Mods/Text` format above, **numbered as on the USA disc** (the same numbers on every disc), with `"worlds"` and the PC strings' `"port"` |
| `Dialog/<anything>/<line>.wav` (or `.flac`) | the voice lines, named by the USA line number (the subfolders don't matter) |
| `Dialog/lengths.json` | optional `{"<line>": seconds}`: how long each line's subtitle stays (else the file's length) |
| `Movies/<name>.wav` (or `.flac`) | a cutscene's sound by its name: `Prologue`, `Ep1 Intro`, `Ep3 Briefing 2`, ... (stereo) |
| `language.json` | optional: `{"name": "Klingon", "text": "English", "voice": "English", "script": "latin"}` |
| `Font.bin` | Japanese only: the Japanese disc's text font (kana, kanji), which the import takes from your own Japanese disc |

**Make a new language:** create `Mods/Languages/<Name>/` (or a folder in the languages folder) with a `Text.json`. That
is a subtitle language: the voices fall back to English (`"voice"` in language.json picks another). For a dub, add the
`Dialog` (and `Movies`) files; a dub without its own text names the text it shows with `"text"`. Missing strings and
lines always fall back, so a partial translation works.

**Change an existing language:** `Mods/Languages/English/Text.json` (or one `Dialog` file) overrides just those
strings or lines of the shared folder, on every disc. `Mods/Text/<Language>.json` still works too (numbered as on your
disc: `--dump-text` shows them).

Starting points: the languages folder's own `Text.json` files, or `Sly2-Release --disc "<installation>" --dump-text
text`. Fonts: the USA and European discs show Western European (Latin-1) characters, and Latin text on the Japanese disc is
shown without accents. Japanese text (`"script": "japanese"` in language.json) is drawn with the Japanese disc's own
font on every disc: installing the Japanese disc, or importing its languages from a disc image, writes that font to
`Japanese/Font.bin` in the languages folder, and the USA and European discs then show the Japanese text with the
Japanese line breaks; the menus' button lines name the buttons of the disc you play (Cross confirms on the USA and
European discs, Circle on the Japanese). A Japanese text of your own (`Mods/Languages/Japanese (Mine)/` with
`"script": "japanese"`) uses the `Japanese` folder's font and can only show the characters that font has. Without a
`Font.bin` (a languages folder made by an older version: start the game from the launcher once with the Japan disc
installed, or import the Japan disc image again) Japanese plays its voices over the disc's own text, and the title's
language list says "voices only".

### Translating (the launcher's Translate page)

The launcher's **Sly 2** page has a **Translate** link (Windows and Linux, beside the Map editor). It edits a language
of the languages folder string by string, beside a reference language:

- **Language:** choose one, or **New language...** (a folder name such as `Føroyskt`, its code and letters: makes the
  folder with its `language.json` and an empty `Text.json`). **Language settings** edits `language.json`: its name on
  the title's list, its code, its letters (Latin or Japanese), the voices it plays and the language its untranslated
  strings come from.
- **Beside:** the reference shown next to each string (English by default; any language with text).
- **Game text / PC strings:** the disc's strings by number, and SafeHouse's own strings (the `"port"` object above).
- **Show:** all, untranslated, the same as the reference, problems, or changed since the last save; **Search** finds a
  number, a name or text in either column. Ctrl+Up / Ctrl+Down go through the list, Ctrl+S saves.
- Each string shows the levels it is in, and **Text per level** adds a level's own text (`"worlds"`).
- **Checks:** the button icons (`&2X&.`) and `%s` / `%d` codes must match the reference (a PC string: the English, in
  the same order), the line breaks, spaces at the ends and a much longer line are pointed out, and so are characters
  the game's font cannot draw (the USA / European fonts: the Western European letters, without Þ þ ½ « » and a few
  signs; Japanese: the characters of the Japanese font).
- **Save** writes the language's `Text.json` (UTF-8; its other keys stay). One of the game's own languages (English,
  French, ...) is saved as a change file instead, `Mods/Languages/<Language>/Text.json` with only the strings you
  changed: it applies on top of the disc's text and of the languages folder's.
- **Test in game** saves and starts Sly 2 in that language. The menu (the arrow) has **Export as a mod package**
  (below), **Export a CSV
  table** / **Import the CSV table** (`<Name>.csv` beside the `Text.json`, columns `id,reference,translation`; PC
  strings are `port:<name>`), and **Find where strings are used again**.

- **Voices** (dubbing): every voice line by its number (the number of its subtitle) and every cutscene's sound, with
  the reference's line (its own voices, else English) and the language's dub: **Play** each to compare (FLAC and other
  formats are decoded by the game program), their lengths, the subtitle in both languages (**Edit the subtitle** goes
  to its text), **Show:** missing / dubbed lines and the dub progress. **Add a dub** copies a WAV or FLAC file in under
  the line's name (`Dialog/<the reference's subfolder>/<line>.wav`, `Movies/<name>.wav`; one of the game's own
  languages: under `Mods/Languages/<Language>/`), and **Subtitle time** sets the line's entry in `Dialog/lengths.json`.
- **Groups:** both lists group by level (the episode whose worlds use a string or line: Prologue, Episode 1: Paris, ...;
  **Menus** for the front end's alone, **Shared** for those in several episodes, such as the HUD and the gadgets) and by
  character inside a level (the speaker the game's dialog tables give each line: Sly, Bentley, Murray, Carmelita,
  Neyla, the villains; **Other** where the table has none; strings without a voice line are **On-screen text**). A
  heading folds its group; **No groups** shows the plain list.
- **Button icons:** the lists, the reference and the voices' subtitles draw the button codes (`&2X&.`, `&2O&.`, ...)
  as the buttons' icons (PlayStation's, or the style chosen in Settings > Button icons). The text field keeps the
  codes; a row of icons under it types a code at the cursor, and **As the game shows it** previews the text.
- **Radio effect:** the lines the game plays as radio talk (the dialog tables mark them for the original's USB headset:
  Bentley and the gang on the binocucom) are tagged **radio**; a line the tables do not list is judged by its
  reference's sound (band-limited: no lows, few highs). A radio line's dub (recorded, or a file added) gets the effect
  estimated from its reference line: its band-pass and tone against the disc's average dry voice, its compression, a
  little saturation and its noise. Each line's checkbox switches it (saved in `Takes/takes.json`), **Preview the
  effect** plays it; the dry take is kept as `Takes/<line>.wav` beside `Dialog/` (the game does not read `Takes/`), so
  the effect can be switched off or redone.
- **Record a dub:** choose the microphone, press **Record**: a three-second countdown, then the reference line plays
  (wear headphones; **Play the reference along** turns it off) and the take stops by itself at the reference's length
  and a second, or with **Stop**. Then drag the two handles on the waveform to cut the start and the end (the dashed
  line is the reference's length), play the take (with the radio effect on a radio line) and **Save as the dub**: a
  16-bit WAV in the line's place, as an added file would be.

- **Export as a mod package** makes `Mods/Packages/language-<name>/` (its `mod.json` and
  `files/Languages/<Name>/`) with everything the language needs to play: `Text.json`, `language.json`, a `Font.bin`,
  every dub and cutscene sound under `Dialog/` and `Movies/`, and `Dialog/lengths.json`. A game language's package
  holds only its changes (`Mods/Languages/<Language>/`). The dry takes (`Takes/`) stay out unless **Include the
  original takes** is ticked. **Make the WAV dubs FLAC** (on by default) converts the WAV dubs to FLAC on the way
  (lossless, about half the size; the game reads FLAC as it reads the European installations' lines; a file that
  does not convert stays WAV), and **Also make a .zip** writes `Exports/language-<name>.zip` in Sly 2's save folder
  (`mod.json` at its root, names in UTF-8 so `Føroyskt` arrives as it is). The page shows the progress and the sizes.
  To install it elsewhere, copy the package folder (or the .zip unzipped as a folder) into `Mods/Packages/` in Sly 2's
  save folder; `Sly2-Release --language-line <Name> <line>` prints the file the game plays for a voice line in that
  language (and decodes it), to check an installed package.

The page runs `Sly2-Release --dump-port-text` and `--dump-text` once in the background (into the launcher's cache
folder) for the PC strings' English and each string's levels; `Sly2-Release --check-language <Name>` checks a
finished language on an installation (every level's strings, the font's letters, the PC strings' codes).

The Translate page is in the desktop launcher only (Windows, Linux, macOS), like the map editor. What it makes plays
everywhere: the game reads the languages folder and `Mods/Languages/`, and a package's `files/Languages/`, the same
way on every platform (phones, Quest, Switch): copy the language's folder into the languages folder, or the exported
package folder into `Mods/Packages/` in Sly 2's save folder.

### Movies

Each movie is two files, like Sly 1's cutscenes: `Movies/<number> <name>.m2v`, the picture (MPEG-2 video, 640x480,
29.97 fps), and `Movies/<number> <name>.wav`, its sound (16-bit stereo). On European discs the sound is one FLAC per
language: `Movies/French/2062 Ep1 Intro.flac`.

Put your versions in Mods under the movie's number, `Mods/Movies/2009.m2v` and `Mods/Movies/2009.wav` (European
sound: `Mods/Movies/French/2062.wav`). Each replaces its own part, so you can change only the sound. The picture is
timed by the sound: keep the two the same length. With ffmpeg, for example:

```sh
ffmpeg -i myvideo.mp4 -an -c:v mpeg2video -q:v 2 -s 640x480 -r 30000/1001 -f mpeg2video Mods/Movies/2009.m2v
ffmpeg -i myvideo.mp4 -vn -c:a pcm_s16le -ar 48000 -ac 2 Mods/Movies/2009.wav
```

A Sony PSS file (the PS2 format) at `Mods/Movies/2009.pss` still replaces the whole movie.

The movie names say what each one is:

| Name | What it is |
|---|---|
| `Prologue` | the game's opening |
| `Ep<N> Intro` | the cartoon opening of each episode |
| `Ep<N> Briefing 1`, `2` | Bentley's chalk-talk plans during the episode |
| `Ep<N> Operation ...` | the briefing for the episode's big operation |
| `Ep<N> Ending` | the episode's closing cartoon |
| `Credits` | the end credits |
| `Extra ...`, `Logo ...` | the extras and the boot logos |

### Sound effects

Every sound effect sample is a WAV in the level's `Sounds` folder: `Worlds/1 The Black Chateau/Paris/Sounds/Sound
021.wav`. A WAV at the same path in Mods replaces that sample. The samples are named after the first sound of the
level that plays them (`Sound 021.wav`; a sound with several samples has `Sound 021-2.wav`, `-3` ...).
`extraction_manifest.json` lists, for each bank, every sample with the sounds that use it: one sample often serves
several sounds (footsteps on different floors, say), and each level has its own copy.

- **Format:** the originals are mono, 48000 Hz. Any rate plays; the game converts it to 48000 Hz, and a sample plays
  at its own speed at the sound's normal pitch (the game raises or lowers some sounds' pitch, as it does the
  original's). Stereo is mixed to mono.
- **Loops:** sounds that loop (engines, alarms) have a loop marker (a `smpl` chunk). Your WAV loops only if it has
  one too; a WAV without one plays once.
- The volume, pitch and timing of each sound come from the level's bank (`<Level>.bnk`), not the WAV.

A whole bank in the 989snd format at `Mods/Worlds/.../<Level>.bnk` replaces the level's definitions and plays its own
samples. SafeHouse has no tool for banks, and every sound the level asks for must still be in the bank.

### Levels

Since SafeHouse 2026-11-10 (asset version 4) each level is stored as its **parts**, the blocks of Sly 2's level
format each in a file of its own, so you can change one part without touching the rest:

- `<Level>.world.json` lists the parts in the order the game reads them. The game puts them back together when the
  level loads; with no mods that is byte for byte the level on the disc.
- `World/Text.json`: the level's text, every language: `{"id": 305, "English": "Press &2X&. to play as Sly."}`.
  Edit a string and put the file at the same path in Mods. (`Mods/Text/<Language>.json`, above, changes a string
  in every level at once; it is applied on top.)
- `World/<nnn> <group>/Scripts.splice`: the scripts of one group (the level's objects come in groups: `227 jt` is
  Sly, `245 murray` Murray, `147 npc_dimitri` Dimitri, the groups named after the level are the level itself). They
  are the game's compiled scripts written as S-expressions: `(define sym1049 #obj:0)`, `(<seto!> sym1053 178 #t)`.
  `symN` is a variable (its number: the names are not on the disc), `<name>` a built-in function, numbers with a
  `.` are floats. Edit, save in Mods at the same path, and the game compiles the text when the level loads. A file
  that does not compile is reported in the log (`mods: ... line 8: unknown word ...`) and the level loads without
  your mods. Comments start with `;`.
- `Graphics.bin`, `Objects.bin`, `Collision.bin` and the `World/*.bin` blocks are binary, as on the disc. The index
  lists each group's objects (number, class, offset, size) for tools.
- A whole level file still works: `Mods/Worlds/<N> <Episode>/<Level>/<Level>.brx` (uncompressed) loads instead of
  all the parts.

Tools:

```sh
Sly2-Release --world-pack "<installation>/Worlds/1 The Black Chateau/Paris/Paris.world.json" Paris.brx
Sly2-Release --disc "<installation>" --world-unpack 2 paris-parts     # a level (record 2) or a .brx file into parts
Sly2-Release --disc "<installation>" --world-check                    # every level put back together and compared
```

This is the advanced end of modding:

- the format is only partly documented;
- the game expects every object it refers to by number to still exist;
- saves and jobs refer to the level's objects and tasks.

Start from small changes, and test with a fresh save.

### The map editor (desktop)

The launcher's **Sly 2** page has a **Map editor** button (Windows and Linux; not in the phone, Quest, Switch or
macOS builds yet). Its page lists the installed worlds and their groups, your projects and the installed mod
packages. **Open in 3D editor** opens a world in its own window, drawn by the game's own renderer:

- **Fly:** hold the right mouse button and use W A S D, Q / E (down / up), Shift (faster), the wheel (speed).
- **Pick:** click an object in the view or in the **Scene** list (groups, then each object's children; the filter
  takes a class name or an object number). Dots are objects without a model (volumes, paths, lights, markers).
- **Move / rotate / scale:** W / E / R choose the tool; drag a handle; or type in the **Inspector**. An instance of a
  shared prop (a chair, a trash can) moves as a whole: the editor changes the proxy that places it.
- **Duplicate** (Ctrl+D) and **Delete** (Del) work on a group's top-level objects and on placed instances.
- **Options:** the Inspector lists the options the object was loaded with; numbers and vectors can be changed (they
  show after **Project > Reload world** or in a play test).
- **Undo / Redo:** Ctrl+Z / Ctrl+Y. **Save:** Ctrl+S. **Play test:** starts Sly 2 in that world with the project.
- **Missions / Text / Scripts / History** (the World panel): the world's jobs, tasks and markers, its text, each
  group's scripts (read only for now), the edits made and the parts they changed.
- **Help:** **Getting started** (F1; shown at the first starts) and the **Keyboard shortcuts** list.

#### Models and textures

- **Import a model** (glTF 2.0 `.glb` / `.gltf`, or `.fbx`): the Inspector's **Model and materials > Replace model...**
  puts it in place of the selected object's model; **World > Models > As a new object...** (or **Project > Import model
  as a new object...**) places it in front of the camera, on the ground below, in the level. Blender: export glTF with
  +Y up (its default); 1 unit = 1 metre = 100 game units (**scale** in the Models tab). Each material's base colour
  picture (and colour) becomes a texture in the game's format (an indexed bitmap and palette, at most 256x256 by
  default) in the object's group; the full picture also goes into the package's `Textures/` (below), which the game
  draws instead at its own size with mipmaps. **Lit** (default) lights the model by the level's lights like the game's
  props; **cel outline** makes the black border along its silhouette; **collision** keeps the replaced object's, removes
  it, or makes a box around the new model.
- **Export GLB** (the Inspector, or the Models tab): the object (with its children), its whole group (a world part), or
  a character with its skeleton and animations (each animation sampled at 30 frames a second). Textures are the
  game's (or a texture pack's replacement, when there is one).
- **Characters:** a character exported here keeps its joints' names (`MURRAY#9.31`: object 9, record 31) and each
  part's name ends in `@<show id>,<draw order>,<hidden>`. Edit the mesh in Blender, keep the armature and the names,
  import it back in place of the character's model: it stays skinned (up to four bones a triangle) and animated.
- **Textures:** each material's **Texture...** puts a PNG / JPEG on it. **Texture pack** (default): the picture at its
  own size under the texture's name in the package's `Textures/` (every model drawing that texture changes; the
  game's own data is untouched). **Game data**: the picture re-encoded into the bitmap and its palettes at their
  size. **PNG...** saves the texture as the game decodes it; **lit** switches a material between lit and flat.
- Not carried by imported models: shader animations (blinks, scrolling textures), the game's baked vertex lighting
  (imported models are lit at run time), more than four bones a triangle. A model of a shared group (a prop placed
  many times) changes every instance; **Make unique** first to change one.

A project lives in Sly 2's save folder, `Editor/Sly 2/<project>/`, and is already a mod package: **Install** copies it
into `Mods/Packages/`, **Export .zip** makes a file to share.

Deleting objects that scripts or other objects name can break jobs; test with a fresh save.

### Mod packages

A mod package is a folder (shared as a .zip of it) that Sly 2 loads on every platform from its save folder's
`Mods/Packages/` (Windows, Linux, macOS, Android, Quest, iOS, Switch):

```text
Mods/Packages/paris-benches/
  mod.json
  files/
    Worlds/1 The Black Chateau/Paris/World/258 f_nightclub_exterior_rig/Objects.bin
```

`files/` holds files at the same paths as the loose Mods folder (world parts, music, movies, ...). A package made by
the map editor also has an `editor/` folder (its undo history, `history.bin`); the game ignores it and exported .zip
files leave it out. `mod.json`:

```json
{"format": "safehouse-mod", "format_version": 1, "id": "paris-benches", "name": "Paris benches", "version": "1.0.0",
 "author": "you", "description": "", "game": "sly2", "regions": ["USA"], "files_format": 3, "priority": 0,
 "dependencies": [{"id": "other-mod", "version": ">=1.2"}],
 "worlds": [{"path": "Worlds/1 The Black Chateau/Paris/Paris.world.json", "record": 2, "fnv64": "..."}]}
```

- `id`: letters, digits, `-`, `_`, `.`. `regions`: the installations it fits (`USA`, `Europe`, `Japan`; empty: any);
  world parts differ between regions, so the editor fills in the one you edited. `files_format`: the installation
  format (3: worlds as parts). `worlds`: the worlds it changes and their original hashes (information).
- Load order: dependencies first, then `priority` (lower first), then the id; a later package's file wins. Loose
  files in `Mods/` win over packages.
- A package that does not fit (game, region, format) or misses a dependency is skipped; the game's log says why
  (`mods: package ...`). Rename a package's folder to `<name>.disabled` (or untick it on the Editor page) to turn it off.
- Command line: `--mod-package DIR` loads one more package over everything (the play test), `--no-mod-packages`
  loads none.

A package's `files/` can also hold replacement textures (`files/Textures/<W>x<H>_<hash>.png`, as `Mods/Textures/`),
text (`files/Text/<Language>.json`, as `Mods/Text/`) and languages (`files/Languages/<Language>/`, as
`Mods/Languages/`). Every package is read, in load order: a later package's texture or string wins, and the loose
Mods folder's win over all.

**Sly 3** (in development) reads packages the same way, with `"game": "sly3"`: its worlds are installed as parts
beside the disc image (`Worlds/<world>/World/...`, made by the installer), so a package's
`files/Worlds/m_ext/World/Text.json` replaces that part, and `files/Textures/`, `files/Text/` and `files/Languages/`
work as for Sly 2. `Sly3-Release --mod-package DIR` and `--no-mod-packages` as above; `--world-check` checks the
installed parts against the disc. A part that changes size (a group's objects added or removed) is fine: the game
recomputes each group's size in the group table when it puts the parts together. The Editor page's **Sly 3** switch
lists Sly 3's worlds and their gameplay objects by area, and **Open in 3D editor** edits them as for Sly 2 (projects in
Sly 3's save folder, `Editor/Sly 3/`): options by Sly 3's own names, curves, duplicate / delete / create / make unique,
jobs (a new job from an existing one), scripts, dialogue and text, and **buttons and what they trigger**: a use (a
button, a switch, a gang member's ability spot) can set a task's state, start a script or run an object's state
machine (the Inspector's Links). Model import and textures are Sly 2's only for now.

**New jobs keep their progress.** A job made in the map editor has tasks the game's save has no place for (adding one
would change the save's layout and clear every save). The editor lists them in `mod.json`:

```json
"custom_tasks": [{"world": "Paris", "tasks": [1001, 1002]}]
```

(Sly 3: the same, with Sly 3's world names, e.g. `"world": "v_ext"`.)

and the game keeps their states in a file next to the save slot, `Saves/save1.mods.json` (beside `save1`, and so on
for each slot); the save slot itself is never changed. When the world loads, those tasks get their states back after
the save's own: a finished custom job stays finished. Removing the package leaves the save as it was (the side file's
states are kept for when the package comes back); erasing the slot or starting a new game in it drops them.

### What is safe to change

| Change | Safe? |
|---|---|
| Textures, any size | Yes |
| Music, any length (keep the stem layout with `--make-music --like`) | Yes |
| Dialog audio | Yes. The timing of subtitles and lip sync stays the original's. |
| Text and subtitles | Yes. Very long strings may not fit their box. |
| Movies, any length | Yes: an M2V and a WAV of the same length |
| Sound effect samples | Yes |
| Sound effect banks | With care: every sound the level uses must be present |
| Level text (`Text.json`) | Yes, as Text and subtitles |
| Level scripts and other parts | Advanced. Can break jobs and saves. |
| The installation folder itself | No. Re-extracting or reinstalling replaces it, and Verify reports changed files. Use Mods. |

**Saves:**

- Saves don't store anything from mods, so adding or removing a mod never harms a save. Level mods are the
  exception if they change jobs or tasks. A package's new jobs keep their progress in a separate file next to the
  slot (`save1.mods.json`, above), never in the save itself.
- Saves are shared between regions, so the same Mods folder and saves work with USA and Europe.

---

## Sly 1: Sly Cooper and the Thievius Raccoonus

### Where things are

| What | Linux | Windows | Portable SafeHouse |
|---|---|---|---|
| The installation (read only for mods) | `~/.local/share/SafeHouse/installations/sly1` | `%LOCALAPPDATA%\SafeHouse\installations\sly1` | `data/installations/sly1` |
| **Your Mods folder** | `~/.config/SafeHouse/Mods` | `%LOCALAPPDATA%\SafeHouse\user\Mods` | `data/user/Mods` |

- A European, Japanese or Korean installation is in `installations/sly1-europe` (`-japan`, `-korea`). The Mods
  folder is shared by all of them.
- Sly 1's Mods folder sits directly in your SafeHouse profile, not in a `sly1` subfolder: Sly 1 keeps its saves
  there too.
- Create the `Mods` folder if it doesn't exist.
- When the game starts, its log prints `mods: N files, N textures, N text files from ...`. Each replaced file is
  printed once, the first time the game uses it, as `mods: using ...`.

The installation looks like this:

```text
sly1/
  extraction_manifest.json                  region, languages, level format (PCBRX 19)
  Worlds/<Episode>/<Level>/
      <Level>.brx                           the level (PCBRX: SafeHouse's rebuilt level format)
      Dialog/VAG_<id>.wav                   its voice lines (USA)
      <Language>/Dialogs/VAG_<id>_<n>.wav   its voice lines (Europe, Japan, Korea: one folder per language)
      <Language>/Commentary/...             the bonus commentary (Europe, Japan, Korea)
  Worlds/Paris, Worlds/The Hideout, Worlds/Splash   the levels outside the episodes
  Worlds/Music/MUSID_<id>.mus               the music; MusicMap.txt says which level plays which
  Worlds/Sound Banks/<World>.bnk            the sound effects
  Worlds/Sounds/                            commentary, Jo ambience and prompt sounds (USA)
  Cutscenes/Cutscene_<NN>.m2v               the cutscenes' video
  Cutscenes/Cutscene_<NN>.wav               their sound (Europe: Cutscenes/<Language>/Cutscene_<NN>.wav)
```

The episode folders are `Tide of Terror`, `Sunset Snake Eyes`, `Vicious Voodoo`, `Fire in the sky` and
`The Cold Heart of Hate`. Example: `Worlds/Tide of Terror/A Stealthy Approach/A Stealthy Approach.brx`.

**The rule: a file in Mods at the same path as a file of the installation replaces it.** For example,
`Mods/Worlds/Music/MUSID_51.mus` replaces `Worlds/Music/MUSID_51.mus`, the music of A Stealthy Approach. Upper and
lower case don't matter. A file can only replace one the installation has; Mods can't add new ones.

### The tools

The tools are options of the game program **Sly1-Release** (`Sly1-Release.exe` on Windows), which is in
SafeHouse's program folder. Run them from a terminal.

**The music tools need no game data:**

```sh
Sly1-Release --make-mus "Mods/Worlds/Music/MUSID_51.mus" mytrack.wav --loop 14.9
Sly1-Release --export-wav "<installation>/Worlds/Music/MUSID_51.mus" original.wav --seconds 120
```

**The dumps need the installation** (`--data-dir` takes the installation folder):

```sh
Sly1-Release --data-dir "<installation>" --dump-text text
Sly1-Release --data-dir "<installation>" --dump-textures textures "A Stealthy Approach"
```

To try a different Mods folder, use `--mods DIR`, or set the environment variable `SLY1_MODS`. An empty value
(`--mods ""`) starts the game with no mods.

### Textures

1. Dump a level's textures:

   ```sh
   Sly1-Release --data-dir "<installation>" --dump-textures dump "A Stealthy Approach"
   ```

   The game starts, goes to that level, writes every texture it loaded as `dump/<W>x<H>_<hash>.png`, and quits.
   The name identifies the texture by its contents. The level name is the level's folder name.

   Without a level name, the game runs normally and writes every texture it loads while you play, so you can
   collect the textures of a whole run. Files already in the folder are kept.

2. Edit the PNG. It can be any size: a 64x64 texture can come back as 256x256.
3. Put it in `Mods/Textures/` under the **same file name**.

The texture is then replaced wherever the game draws it, in every level and on the HUD.

- **Higher resolution:** as in Sly 2, a replacement is used **at its own resolution, not stretched**: the texture
  coordinates are normalized, so a 4x PNG maps like the original and shows 4x the detail. World textures get
  mipmaps with trilinear filtering (and the anisotropic filtering setting); fonts and HUD pieces, which have no
  mipmaps in the game either, stay single-level. Same recommended limits as Sly 2: 4x, at most 4096 pixels a side,
  16384 at the most.
- **No dump at install:** unlike Sly 2, Sly 1's textures are inside its rebuilt levels and only decoded by the game
  as a level loads, so the installation has no `Textures` folder. Dump the levels you want to change as above.

- **Shared textures:** a texture used by several levels has the same name in each, so one PNG covers all of them.
  The dump of a level also holds the title screen's and Paris's textures, which load on the way there.
- **Three versions of a surface:** many walls and floors come as up to three textures with the same drawing in
  different colours: the surface in shadow, lit, and highlighted. Change all of them, or the surface changes
  colour as the light on it changes.
- **Alpha:** 255 is fully opaque, as in any PNG.
- **Unchanged textures:** a PNG that is the dump unchanged is ignored, so the texture looks exactly as before.
  This makes it safe to keep a whole dump in Mods and edit a few files.
- The game's log prints, for every level, how many textures it replaced.

### The PS3 HD textures (The Sly Collection)

As for Sly 2, **Settings > Games > Sly Cooper and the Thievius Raccoonus > Import PS3 HD textures** takes the redrawn
textures of **The Sly Collection** (PS3: the disc image, a `.zip` of it, or the extracted disc) and uses them over
Sly 1's. It takes about 2 minutes and writes `PS3 HD/Textures.pak`, about 530 MB (9222 textures, 45 of the 46 levels;
the start-up splash has none), in your profile folder (`~/.config/SafeHouse/PS3 HD`,
`%LOCALAPPDATA%\SafeHouse\user\PS3 HD`).

- `Textures.pak` holds the PNGs under the names of `--dump-textures`, so your own `Mods/Textures` win over it.
- **Use the PS3 HD textures** turns it on or off; **Remove** deletes it. The levels stay the installation's.
- Without the launcher: `Sly1-Release --data-dir "<installation>" --ps3-hd-textures "<PS3 disc, .iso or .zip>" OUT`,
  then `--texture-pack OUT`.
- As for Sly 2: **Extract as PNG files** (or `--ps3-hd-png`) writes the PNG files in `PS3 HD/Textures` instead, an older
  `PS3 HD/Textures` folder still works, and on the Nintendo Switch the `PS3 HD` folder goes to
  `sdmc:/switch/SafeHouse/user/sly1/`.

### Text

All on-screen text that the game takes from its string table (menus, the pause menu, prompts, level and world
names, power-up names, cutscene titles, the title screen) can be replaced per language with
`Mods/Text/<Language>.json`:

1. Dump the current strings. This writes one file per language of your disc: `English.json` (USA), `English.json`,
   `French.json`, `German.json`, `Spanish.json` and `Italian.json` (Europe), `Japanese.json` (Japan) or
   `Korean.json` (Korea):

   ```sh
   Sly1-Release --data-dir "<installation>" --dump-text text
   ```

2. Copy the strings you want to change into your own file:

   ```json
   {
     "strings": {
       "28": "A Very Sneaky Approach",
       "83": "Taking a Break",
       "120": "Back to Thieving"
     },
     "levels": {
       "A Stealthy Approach": { "79": "Look around" }
     }
   }
   ```

3. Save it as `Mods/Text/English.json` (or `French.json`, ...).

How the file works:

- **`strings`** applies everywhere; **`levels`**, keyed by the level's folder name, applies only in that level and
  wins over `strings`.
- **Keys** are the string numbers from the dump. You only need the strings you change. You can't add new numbers.
  The numbers are the same on every disc (the European discs' string table); the USA dump lists the USA game's own
  English text under them (152 strings: it shows fewer than the European game).
- Some examples: 0 and 1 are the title screen's "Press START" lines, 25 to 70 the level names, 83 the pause menu
  title, 117 to 149 the menu choices, 162 to 168 the power-up names, 173 to 188 the cutscene titles in the
  Thievius Raccoonus.
- **Encoding:** write the file as UTF-8. Characters the game's font lacks don't show. The European fonts have the
  Western European letters (é, ß, ñ ...).
- **Button icons:** sequences like `&2X&.` (the X button) and `&2T&.` (Triangle) draw the game's button icons.
  Keep them.
- **Comments:** `//` comment lines are allowed in the file.
- Not covered: the PC-only options (Video, Sound, Controls rows), which are not in the string table.

### Music

Sly 1's music files (`Worlds/Music/MUSID_<id>.mus`) are not recordings: each is a sound bank of instrument
samples and a sequence that plays them, and the game changes the arrangement as you sneak or get spotted.
`MusicMap.txt` beside them says which level plays which file.

To replace a track with your own recording:

```sh
Sly1-Release --make-mus "Mods/Worlds/Music/MUSID_51.mus" mytrack.wav --loop 14.9
```

- **`--loop SECONDS`** is where the music jumps back to after the end. The default, 0, loops the whole file.
  `--loop none` plays the track once. The loop is seamless: the tool moves the loop start by under a millisecond
  and changes the loop's length by under 14 samples (both inaudible) to fit the game's sample blocks.
- **`--volume 0..127`** sets the level. The default, 80, puts a normally mastered track about as loud as the
  original music.
- **WAV formats:** 16-bit or 24-bit PCM or 32-bit float, mono or stereo, any sample rate.
- Your track plays the same whether you sneak or get spotted.
- A track is stored at 48 kHz in the game's 4-bit format: a 3-minute stereo track makes a file of about 10 MB.

To get an original track as a WAV, for listening or as a reference:

```sh
Sly1-Release --export-wav "<installation>/Worlds/Music/MUSID_51.mus" original.wav --seconds 120
```

This plays the track through the game's own music engine for that many seconds and records it, so it takes that
long. It records the track as it starts in its level. On Windows it needs an audio output device (the recording
itself is silent).

### Dialog and sounds

Voice lines, commentary, the cutscenes' sound and the `Worlds/Sounds` files are ordinary WAV files. Replace one
by putting a WAV at the same path in Mods, for example `Mods/Worlds/Paris/Dialog/VAG_006B.wav`.

- **Format:** any PCM WAV plays. The originals are 16-bit mono at 22050 Hz (voice lines) or 16-bit stereo at
  48000 Hz (cutscenes).
- **Which line is which:** `Dialog/DialogMap.txt` beside the lines lists each line's id.
- **Languages:** on European, Japanese and Korean installations every line exists once per language, in
  `<Level>/<Language>/Dialogs/`. Replace it in each language you want to change.
- The subtitles and the lip sync stay the original's, so keep a line about as long as the original.

### Cutscenes

Each cutscene is two files: `Cutscenes/Cutscene_NN.m2v` (the video) and `Cutscenes/Cutscene_NN.wav` (its sound;
on European discs, one per language in `Cutscenes/<Language>/`). Put your versions at the same paths in Mods.
Replace both together. With ffmpeg, for example:

```sh
ffmpeg -i myvideo.mp4 -an -c:v mpeg2video -q:v 2 -s 640x480 -r 30000/1001 -f mpeg2video Mods/Cutscenes/Cutscene_00.m2v
ffmpeg -i myvideo.mp4 -vn -c:a pcm_s16le -ar 48000 -ac 2 Mods/Cutscenes/Cutscene_00.wav
```

The video and the sound can be longer or shorter than the original; keep them the same length as each other.
`Cutscene_17` is the start-up splash.

### Sound effects

Each world's sound effects are one 989snd bank (`Worlds/Sound Banks/<World>.bnk`). You can replace it with a bank
in the same format at the same path in Mods. SafeHouse has no tool for these, and every sound the game asks for
must still be in the bank.

### Levels

Levels are PCBRX, SafeHouse's rebuilt level format (see `PCBRX-v19.md`). A level in Mods at the same path replaces
the installed one:

- **The whole level:** `Mods/Worlds/<Episode>/<Level>/<Level>.brx`, a PCBRX file (v19 with all of the disc's
  languages, or a single-language v17).
- **One language only:** on European, Japanese and Korean installations a level holds one version per language.
  `Mods/Worlds/<Episode>/<Level>/<Language>/<Level>.brx`, a single-language PCBRX v17 file, replaces only that
  language's version. On the Japanese disc, the language folders are the voice languages, `Japanese` and
  `English`.

The `Sly1BrxRebuilder` tool, next to SafeHouse's extractor, rebuilds a decompressed retail-format level into
PCBRX v17:

```sh
Sly1BrxRebuilder MyLevel.brx ntsc
```

The tool prints the path of the rebuilt file. Editing levels needs community tools for the retail Sly 1 format.
This is the advanced end of modding: the game expects every object it refers to to still exist, and saves refer
to the level's progress. Start from small changes, and test with a fresh save.

### What is safe to change

| Change | Safe? |
|---|---|
| Textures, any size | Yes |
| Text | Yes. Very long strings may not fit their box. |
| Music, any length | Yes. It no longer changes when you are spotted. |
| Dialog and sounds | Yes. Subtitle and lip-sync timing stay the original's. |
| Cutscenes, any length | Yes, as MPEG-2 video and a WAV |
| Sound effect banks | With care: every sound the game uses must be present |
| Level files | Advanced. Can break the level and saves. |
| The installation folder itself | No. Re-extracting or reinstalling replaces it, and Verify reports changed files. Use Mods. |

Saves don't store anything from mods, so adding or removing a mod never harms a save; level mods are the
exception.


---

## Sharing a mod

- **Sly 2 levels:** share a mod package (the map editor's **Export .zip**); players unzip it into `Mods/Packages/`.
- **Sly 2:** zip your Mods folder's contents. A player unzips them into their own Mods folder. Name music and movies
  by number (`Music/28.vpk`), so the mod works with any SafeHouse version.
- **Sly 1:** zip your Mods folder's contents the same way. Players unzip them into their own Sly 1 Mods folder.
- **Never share game files you did not make.** That includes original textures and dumps: share only your own work.
