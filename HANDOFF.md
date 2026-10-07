# Handoff: SMT IV: Apocalypse single-screen "remaster" (Azahar fork)

Branch `single-screen` of `wigwav/azahar`. Last commit before this file: `51da281`.

## 1. What the user wants

The user wants a native-looking single-screen version of *Shin Megami Tensei IV: Apocalypse* (title `000400000019A200`) running in this Azahar fork:

- The game's top screen is shown full size.
- Everything the bottom screen normally gives (menus, party status, lists, targeting, damage) is rebuilt as an HD overlay ("HUD"). The overlay is drawn from the game's real memory, so it always matches the game's state.
- The bottom screen is hidden and is never copied or moved to the top screen.

### How the user works and what they expect

- Native feel is what matters most. They rejected copying bottom-screen pixels or effects onto the top screen ("you're just bringing the bottom screen to the top… awful").
- Check every change against their own save states and a real view before claiming it works. Several rounds failed because something was reported fixed and wasn't.
- Be fast and report honestly. Nothing should be installed on their PC until they ask.

### Their PC (Windows, OpenGL)

| What | Where |
|---|---|
| Emulator they launch | `C:\Users\super\Downloads\azahar-windows-mxe-20261005-9a838a7\azahar.exe` |
| HUD config | `C:\Users\super\AppData\Roaming\Azahar\load\screen_regions\000400000019A200.ini` |
| HUD art and text tables | `C:\Users\super\AppData\Roaming\Azahar\load\screen_regions\000400000019A200\` (PNGs, `names.txt`, `smt4a_*.txt`, `bu_index.txt`, `itemuse.txt`, `hit_0..2.png`, `bu/`, `cache/`) |
| Save states | Azahar `states` folder; slots 1–4 and 8–10 are used for testing |
| Full RomFS dump | `C:\Users\super\AppData\Roaming\Azahar\dump\romfs\000400000019A200\` |
| Draw tracer switch | `load\screen_regions\trace.on` (contents `on`/`off`; keep `off`) |

Keyboard mapping: A=a, B=s, X=z, Y=x, D-pad t/g/f/h, L=q, R=w. F4 pauses.

## 2. Build and install

- **Build.** Push to `single-screen`. The CI job `citra-build` builds `windows-mxe` in about 9 minutes. Then `single-screen-release` publishes the release asset `single-screen-latest/azahar-single-screen-windows.zip`.
  - The `citra-format` failure is old and comes from upstream files.
- **Download.** Use the REST API: `gh api -H "Accept: application/octet-stream" repos/wigwav/azahar/releases/assets/<id>`. GraphQL is blocked.
- **Transfer.** `azahar.exe` is about 37 MB, over the 30 MB transfer limit. Compress it with UPX (`upx --best`, about 15 MB).
- **Install.** Copy the new `azahar.exe` over the old one, then the `.ini`, then any changed assets. The HUD reloads the ini automatically; restarting is the safe option.
- **Linux tools.** `ninja sr_tool video_core` in a Linux build directory compiles the headless tool and checks the OpenGL renderer code.

## 3. Architecture (fork changes)

### HUD engine

Files: `src/core/frontend/hud.cpp/.h` and `hud_expr.cpp/.h`.

**Elements:**

| Element | What it draws |
|---|---|
| `rect` | Filled rectangle |
| `image` | PNG; `@name` uses a capture |
| `text` | Text in the bundled font |
| `bar` | Value/max bar |
| `capture` | Area grabbed from the bottom screen |
| `fx` | Not used any more (see FxCapture below) |
| `fxlearn` | Not used any more (see FxCapture below) |

**Element options:**

| Option | Meaning |
|---|---|
| `if=`, `and=` | Visibility |
| `ox==`, `oy==` | Offsets from expressions |
| `fade==` | Opacity 0–100 |
| `linger=` | Keep visible for N ms after the condition drops |
| `hold=` | Keep the previous visibility while true |
| `v=` | Values substituted into `{0}`, `{1}`, … |
| `max=` | Bar maximum |
| `fit=` | Shrink text to this width |

**Expressions** support `let` variables, `?:`, `u8`/`u16`/`u32` memory reads, `recent(cond, ms)`, `track(key, value, ms)` (change summed over a window), `trackage(key)`, `time()`, `pix(x, y)` (bottom-screen pixel probe), `lookup('file.txt', idx)`, `has(str, sub)`, `sjis(addr, off)`, and the `smt4a_*` helpers listed below.

**`smt4a_*` helpers:** `obj`, `save`, `unit`, `demon`, `hp`, `mp`, `maxhp`, `maxmp`, `level`, `cmd`, `cmdcount`, `cmddesc`, `entries`, `entry`, `skillname`, `skillicon`, `skilldesc`, `cost`, `item`, `itemcount`, `itemname`, `rec`, `prec`.

The canvas is 1920x1080 and is rasterised on a worker thread whenever any value changes.

### Screen regions

`src/core/frontend/screen_regions.cpp` handles profiles, the automatic profile choice (8-frame debounce), and `hide_bottom`.

### HUD config

- `dist/screen_regions/000400000019A200.ini` is generated. Do not edit it by hand.
- `tools/smt4a/hud3gen.py` writes the `[hud battle]` section. Run: `python3 hud3gen.py <out.ini> [more outputs]`.
- The camp-menu section comes from `campgen.py`.

### FxCapture (built, but no longer used by the HUD)

Files: `src/video_core/pica/fx_capture.*`, plus `ScreenRegions::FxLayer` in `hud.h`.

How it works:

- While the player chooses commands, it learns which bottom-screen textures belong to the menus.
- During actions, it rasterises every other bottom-screen triangle on the CPU into a 320x240 layer. The layer is published at each bottom-screen display transfer (`gpu.cpp`), and `RendererOpenGL::DrawFx` draws it.

The user rejected the result, for three reasons:

- The screen mapping in `EndBottomFrame` is mirrored: a damage of 18 showed as "81".
- It also copied the game's own damage digits.
- It looked like a copy of the bottom screen.

It now costs nothing, because no `fx`/`fxlearn` elements are used. Remove it or keep it as a tool.

### Draw tracer

Triggered by the `trace.on` file. It logs per-draw framebuffer, viewport, textures and blending to `<log>/drawtrace.txt`.

- The bottom-screen render target is `fb=240x319`.
- A bottom-screen display transfer is logged as `X in=18000000 … 240x320 tex=0`.

### Save states

Save states made by other builds of the fork load; a version mismatch is only logged.

## 4. Memory map (US version, battle)

### Battle object `O`

`O = smt4a_obj()`, usually `0x8650000`.

| Offset | Meaning |
|---|---|
| `+0x554` | Acting slot (0–3 party, 4+ enemies) |
| `+0x18A0` | UI mode: 11 command, 16 ally target / demon list, 13 Effects, 14 Analyze, 0 other |
| `+0x14D4` | Input stack; `0xFFFFFFFF` = no input (a command was confirmed / an action is playing) |
| `+0x113FC` | List open (1) |
| `+0x5C8 + A*0x64` | Command cursor |
| `+0x5B8 + A*0x64` | Command count |
| `+0x8E8` | Ally target cursor |
| `+0x764` / `+0x730` | Skill cursor / scroll |
| `+0x82C` / `+0x7F8` | Item cursor / scroll |
| `+0x96C` | u16 item id list, 0-terminated |
| `+0x11370` | Command description |
| `+0xE64C` | Popup |
| `+0x898` | Sub-picker (0 = open) |

### Party units

Unit `k` is at `[0x57113C] + 0x200E6 + k*0x408`.

| Offset | Meaning |
|---|---|
| `+0xF2` | HP (u32) |
| `+0xFA` | MP (u32) |
| `+0xF6` | Max HP (u32) |
| `+0xFE` | Max MP (u32) |
| `+0x114` | Level |
| `+0x10E` | u16 flags; bit `0x10` = Smirk |

In slot 4, Inanna's HP is at `0x85f9360`. HP can go above 9999, so never clamp displayed values.

### Other addresses

- Camp menu widget `[0x57F108]`: `+0x78` count, `+0x88` cursor.
- Demon list: `0x57F268`. Save pointer: `0x57F69C`.
- Command-row-visible probe (bottom screen): cream pixels at (148,176), (168,176), (186,178).

### Item data

`item/ItemTable.tbb`, parsed by `tools/smt4a/itemuse.py`:

- Table 1 holds ids 1–60 (`id = index + 1`); table 6 holds ids 1951+.
- Each record is 0x6C bytes: tag at `+0x20` (`hpup`, `mpup`, `rest` = revive, `recv` = cure, `mris` = full restore), HP `+0x34`, MP `+0x36`, HP% `+0x38`.
- Bit `0x10` of `+0x42` means "usable on allies in battle".
- The game silently ignores A on a heal when everyone is at full HP/MP, and on a revive when nobody is down. The HUD greys those items.

### Skills

`smt4a_skills.txt` has one row per skill id: name, cost, …, icon. Examples: Hades Blast = 162, Heaven's Bow = 187, Akasha Arts = 363, Cheer = 226.

## 5. Current state (installed on the PC)

### Battle HUD (`hud3gen.py`)

- **Hints:** button hints top-left.
- **Portrait and lists:** the acting member's portrait, skill/item list and description sit bottom-left. Skills are greyed when there isn't enough MP; items are greyed when unusable (see section 4).
- **Command row:** centred.
- **Talk menu:** semi-transparent.
- **Popups:** Status/other popups.
- **Ally targeting:** gold "TARGET" frame on the card.
- **Party cards:** small cards (236x104) along the bottom edge at y 966. They stay in that one place and never move to the top. They show real HP/MP (no 9999 cap).
- **Hits:** a hit shakes the card, flashes it red and plays `hit_0..2.png` (generated by `hitart.py`). A rising red damage number (heals: green "+") appears over the card. Damage is the HP change summed over 1.8 s.

### Camp main menu

Native grid and command row.

## 6. Open items (what to do next)

### 6.1 Element- or skill-specific hit animation

The user asked to find the attacking skill in memory. After Nanashi used Hades Blast (id 162), these u16 locations newly held 162:

- `0x82cd498`
- `0x85d6e24`
- `0x85f9024`, `0x85f902c`, `0x85f903c` (inside unit 0)
- `0x8650560` = `O+0x560`
- `0x86518b8` = `O+0x18B8`
- `0x8656450` = `O+0x6450`
- `0x86935a8`

Next steps:

- Confirm which of these holds the skill of the *current action* for enemy actions too. The `O+...` candidates are the most promising.
- Then read the skill's element (`smt4a_skillicon` or `battle/SkillData.tbb`) and pick per-element strike art: Phys, Gun, Fire, Ice, Elec, Force, Light, Dark, Almighty.
- Ailments: inflict an ailment on the party and diff memory the same way.

### 6.2 Status ailments

The memory location is unknown. Setting other bits of unit `+0x10E` showed no icon. Once found:

- Show ailment icons on the cards.
- Make cure items (`recv`) follow the "greyed when unusable" rule.

### 6.3 Headless testing of enemy turns

The test copy of the game (`smt4a.cxi`) was rebuilt from a partial dump of about 755 files. Files it lacks read as zeros. Enemy AI scripts (e.g. `MISHAGUJISAMA.bf`) are missing, so the game's script interpreter loops on a null context and enemy turns hang.

Fix:

1. Run an enemy turn with `CITRA_LOG_ROMFS=1`.
2. Run `tools/smt4a/needed.py <err log>` to list the missing paths.
3. Fetch those paths from the PC dump.
4. Run `fillromfs.py` to write them into the image.

Notes:

- The save states were made from the user's own install, whose RomFS starts at another offset. Run `sr_tool` with `SR_ROMFS_SUB=317000:54b000` (see `file_derived.cpp`).
- Loading a second save state in the same `sr_tool` process crashes it. Restart it each time (`emu/fresh.sh`).

### 6.4 Other polish worth doing

- Check the hit animation timing on the PC.
- Check that the cards never cover the game's skill banner (the banner ends about y 937 on the 1080 canvas).

## 7. Headless tools (`src/sr_tool`, scripts in `tools/smt4a/emu`)

`sr_tool <user_dir> <game file>` reads commands on stdin. `emu/drv.sh start`, then `drv.sh cmd "..."`, wraps it and waits for each command's `ok`.

| Command | What it does |
|---|---|
| `load N` / `save N` | Load / save a state slot |
| `run F` | Run F frames |
| `hold BTN F` / `tap BTN F` | Press a button (`tap down 12` scrolls lists; confirm with `run 40` then `hold a 10`) |
| `film PREFIX COUNT STEP` | Screens + battle object + units; read with `emu/fr.py` |
| `dump PATH` | Write an `SRRAW1` dump |
| `hud INI ASSETS PROFILE OUT.rgba` | Render the HUD (`emu/see.sh` makes a side-by-side PNG) |
| `mem ADDR SIZE PATH` | Read guest memory to a file |
| `w8` / `w16` / `w32 ADDR VAL` | Write guest memory (invalidates the JIT for code) |
| `watch ADDR SIZE` | Log reads/writes to stderr with PC |
| `regs` / `threads` | CPU registers / thread list |
| `fx …`, `fxdump`, `fxsave`, `fxload` | Effect-layer experiments |

Input-timing notes:

- `ctl.py` passes party turns with Next (`next_index` rules) and films enemy turns.
- With the skill/item list open, A uses the highlighted entry.

## 8. Not in the repo (game-derived)

The following are not committed because they come from the game: HUD art, portraits (`bu/`), the text tables (`names.txt`, `smt4a_*.txt`, `bu_index.txt`, `font.*`, `itemuse.txt`) and the RomFS dump.

- They are installed on the user's PC (section 1) and can be regenerated from the dump with `strtables.py`, `portraits.py`, `assets3.py`, `itemuse.py` and `hitart.py`.
- Scripts have absolute paths from the old workspace (`/home/claude/...`, `/mnt/user-data/uploads/dump/...`, `/tmp/claude-0/emu`). Adjust them.
