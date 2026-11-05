# Steam library artwork (SteamGridDB)

The "Add to Steam" button copies these images into Steam's `userdata/<id>/config/grid/` folder for the shortcut it
creates. They were chosen on SteamGridDB (the same picks as a Steam ROM Manager setup), downloaded on
2026-11-03 from the public asset pages below and bundled so the button works offline. Every image belongs to its author;
the game logos and characters are trademarks of Sony Interactive Entertainment. SafeHouse is an unofficial fan project.

Changes: the two large logos (Sly 2, Sly 3) were scaled down to 1280 px wide; the Sly 3 icon was converted from .ico to
PNG. Everything else is the original file.

SteamGridDB games: Sly Cooper and the Thievius Raccoonus https://www.steamgriddb.com/game/5248314 ·
Sly 2: Band of Thieves https://www.steamgriddb.com/game/35932 · Sly 3: Honor Among Thieves https://www.steamgriddb.com/game/38836

| File | Steam use | Size | Author | Source |
|---|---|---|---|---|
| sly1/grid.png | capsule (`<id>p`) | 600x900 | ctwoafiveb | https://www.steamgriddb.com/grid/523131 |
| sly1/wide.png | wide capsule (`<id>`) | 460x215 | Benuno | https://www.steamgriddb.com/grid/71002 |
| sly1/hero.jpg | hero (`<id>_hero`) | 1920x620 | AnimeFan69 | https://www.steamgriddb.com/hero/147708 |
| sly1/logo.png | logo (`<id>_logo`) | 1029x592 | ctwoafiveb | https://www.steamgriddb.com/logo/115276 |
| sly1/icon.png | icon (`<id>_icon`) | 128x128 | Spaghetti Overlord | https://www.steamgriddb.com/icon/4569 |
| sly2/grid.png | capsule | 600x900 | Pedrogon18 | https://www.steamgriddb.com/grid/122173 |
| sly2/wide.png | wide capsule | 460x215 | Spaghetti Overlord | https://www.steamgriddb.com/grid/80136 |
| sly2/hero.jpg | hero | 1920x620 | AnimeFan69 | https://www.steamgriddb.com/hero/109527 |
| sly2/logo.png | logo | 5026x2982 → 1280x759 | RealSayakaMaizono | https://www.steamgriddb.com/logo/6258 |
| sly2/icon.png | icon | 128x128 | Spaghetti Overlord | https://www.steamgriddb.com/icon/4570 |
| sly3/grid.jpg | capsule | 600x900 | Spaghetti Overlord | https://www.steamgriddb.com/grid/37963 |
| sly3/wide.png | wide capsule | 460x215 | Nuke | https://www.steamgriddb.com/grid/66342 |
| sly3/hero.jpg | hero | 1920x620 (PNG → JPEG) | simenfje | https://www.steamgriddb.com/hero/86895 |
| sly3/logo.png | logo | 2120x1452 → 1280x877 | RealSayakaMaizono | https://www.steamgriddb.com/logo/6259 |
| sly3/icon.png | icon | 256x256 (.ico → PNG) | FrostWolf | https://www.steamgriddb.com/icon/3352 |

**Desktop shortcuts** ("Add desktop shortcut") used these `icon.png` files until 2026-11-05; they now use the game
logos (`launcher/assets/shortcut/<game>-logo.*`, see `launcher/assets/README.md`). The Sly 1 and Sly 2 icons are
still the Windows icons of the game programs (`launcher/assets/shortcut/<game>.ico`, `scripts/make-shortcut-icons.py`).

To swap an image, replace the file (keep the name; .png or .jpg both work) and update this table (for an icon, run `scripts/make-shortcut-icons.py` too).
