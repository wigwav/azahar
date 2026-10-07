# Handoff: SMT IV: Apocalypse single-screen remaster (Azahar fork)

This file is everything the previous assistant knew. Read all of it before changing anything.

- Repository: `https://github.com/wigwav/azahar` (public)
- Branch: `single-screen`
- Last work commit: `51da281`

---

## 0. Start here

### 0.1 What you are building

The game is *Shin Megami Tensei IV: Apocalypse* (US, title id `000400000019A200`). It is a 3DS game that splits its interface over two screens.

This fork of the Azahar 3DS emulator shows only the game's **top screen**, full size, at 1920x1080. Everything the bottom screen gives the player is redrawn on top of it as a native HD overlay (the "HUD"):

- menus, command row, skill/item lists
- party HP/MP, targeting, damage

The overlay reads the game's memory, so it always matches the real game state. The real bottom screen is hidden.

### 0.2 Hard rules from the user (they have been strict about each of these)

1. **Never copy or show bottom-screen pixels or effects on the top screen.** It must look like a native port. This was tried twice and rejected both times:
   - a docked bottom screen
   - copying the bottom screen's effect draws ("you're just bringing the bottom screen to the top… awful")
2. **Verify before claiming.** Check every change in a real rendered frame from the user's own save states, and preferably in motion. Several earlier rounds were reported "fixed" and weren't, and the user lost patience ("literally nothing changed").
3. **Don't install anything on the user's PC until they say so.** Prepare the files, then copy when asked.
4. **Be quick and honest.** Short progress updates; say plainly what you could not verify.
5. HP/MP can exceed 9999 (level 999 party). **Never clamp** displayed numbers.

### 0.3 Suggested first message from the user

> Read HANDOFF.md on the single-screen branch of wigwav/azahar. First task: section 7.1 (skill and element of the current action, per-element hit animation on the party card).

### 0.4 Checklist before your first change

- [ ] Clone the repo and check out `single-screen`.
- [ ] Get access to the user's PC (Claude desktop app link) or ask them for files. See section 2 for paths.
- [ ] Build the headless tool `sr_tool` on Linux (section 4) and build a test copy of the game from the user's full RomFS dump (section 4.3).
- [ ] Copy the user's save states (section 2) and render the HUD on them (section 5) before editing anything.

---

## 1. Where everything is: the repository

Everything I wrote is in the repository.

### 1.1 Emulator source changes (C++)

| File | What it is |
|---|---|
| `src/core/frontend/hud.cpp/.h` | HUD engine: element parsing, evaluation, async rasteriser (1920x1080 canvas, worker thread). Also `FxLayer`, `Captures`, `Probes` and `DrawTrace` (draw tracer). |
| `src/core/frontend/hud_expr.cpp/.h` | Expression language and all `smt4a_*` game helpers: party units, battle object scan, skill/item lists, names, SJIS text. |
| `src/core/frontend/screen_regions.cpp/.h` | Profiles from the `.ini`, automatic profile choice (8-frame debounce), `hide_bottom`, regions, recorder, HUD update per frame. |
| `src/video_core/renderer_opengl/renderer_opengl.cpp/.h` | Draws the HUD canvas (`DrawHud`), screen regions, bottom-pixel probes (`ReadProbes`), and the unused effect layer (`DrawFx`). The user's PC uses OpenGL. |
| `src/video_core/pica/fx_capture.cpp/.h` | Bottom-screen effect capture. **Unused by the HUD now** (see 6.2); remove or keep for research. |
| `src/video_core/pica/pica_core.cpp/.h` | Hooks for FxCapture plus the draw tracer (`TraceDraw`). |
| `src/video_core/gpu.cpp` | Display-transfer hooks (tracer, FxCapture frame end) and vblank tracer frames. |
| `src/core/savestate.cpp`, `core.cpp` | Save states from other builds of this fork load (the mismatch is only logged). |
| `src/common/file_util.cpp` | Headless only: `SR_GAME_FILE` substitutes the game file named inside a save state. |
| `src/common/file_derived.cpp` | Headless only: `SR_ROMFS_SUB=from:to` remaps the RomFS offset (see 4.3). |
| `src/core/file_sys/romfs_reader.cpp` | `CITRA_LOG_ROMFS=1` logs every RomFS read (offset, length) to stderr. |
| `src/core/memory.cpp/.h` | Watchpoint hook used by `sr_tool watch`. |
| `src/sr_tool/sr_tool.cpp` | Headless research tool (section 4). |
| `src/citra_qt/citra_qt.cpp` | Hotkeys and menu hooks for Screen Regions. |
| `.github/workflows/` | CI builds only Windows (MXE) on this branch. |

### 1.2 HUD configuration and tools

| Path | What it is |
|---|---|
| `dist/screen_regions/000400000019A200.ini` | Generated HUD config. Do not hand-edit. |
| `dist/screen_regions/000400000019A200/` | A few HUD images (cards, font, `hit_*.png`) and `names.txt`. The full art set is on the user's PC (section 2). |
| `tools/smt4a/hud3gen.py` | **Main generator** of the `[hud battle]` section. It reads the existing ini, keeps everything outside the battle section, and splices in `campgen.py`'s camp section. |
| `tools/smt4a/campgen.py` | Camp (field) main-menu HUD. |
| `tools/smt4a/itemuse.py` | Writes `itemuse.txt` (which items need what to be usable) from `item/ItemTable.tbb`. |
| `tools/smt4a/hitart.py` | Draws `hit_0..2.png` (the strike mark on a hit card). |
| `tools/smt4a/portraits.py`, `findbu.py`, `matchbu.py`, `stex.py`, `assets3.py`, `icons_game.py` | Build portrait/bust-up art (`bu/`, `bu_index.txt`), command icons and other art from the dump. |
| `tools/smt4a/strtables.py` | Extracts game text tables (`names.txt`, `smt4a_skills.txt`, `smt4a_items.txt`, `smt4a_cmds.txt`). |
| `tools/smt4a/mkcxi.py`, `fillromfs.py`, `needed.py`, `romfsmeta.py` | Build and fill the headless test copy of the game (section 4.3). |
| `tools/smt4a/rec.py`, `srrec.py`, `recpreview.py` | Readers for recorder files (`rec_*.zst`) and sr_tool dumps. |
| `tools/smt4a/mem3.py`, `memmatch.py`, `ptrpath.py`, `addrmap.py`, `texram.py` | Memory-research helpers. |
| `tools/smt4a/unzstd.c`, `cityhash_tool.cpp` | Small native helpers (compile with `cc`/`c++`). |
| `tools/smt4a/emu/drv.sh` | Starts `sr_tool` on a FIFO; `drv.sh cmd "..."` sends commands and waits for each `ok`. |
| `tools/smt4a/emu/fresh.sh` | Restarts sr_tool. Needed before every `load`: a second load in one process crashes. |
| `tools/smt4a/emu/see.sh` | Renders the HUD next to the real bottom screen into a PNG. |
| `tools/smt4a/emu/ctl.py` | Plays a battle headlessly: passes party turns with Next, films enemy turns. |
| `tools/smt4a/emu/fr.py` | Reads `film` frames (screens, battle object, units). |
| `tools/smt4a/emu/itemtest.sh`, `heal.sh` | Examples of item/target input sequences. |

**All scripts contain absolute paths from my workspace.** Fix them for your environment:

- `/home/claude/azahar` — the repo
- `/home/claude/smt4a` — game image, test user dir, HUD art
- `/home/claude/build-lin` — Linux build
- `/tmp/claude-0/emu` — scripts and film output
- `/mnt/user-data/uploads/dump/romfs/000400000019A200` — partial dump

### 1.3 Not in the repository (game-derived)

These are not committed because the repo is public: the HUD art set (about 22 MB of PNGs and `bu/` portraits), the text tables, `itemuse.txt` and the game image. They are on the user's PC (section 2) and can be regenerated from the dump with the tools above.

---

## 2. Where everything is: the user's PC

Windows, OpenGL, device name `winner`, user `super`. The previous assistant reached it through the Claude desktop app link (`device_list_dir`, `device_stage_files`, `device_commit_files`; a shell was available at times).

| What | Path |
|---|---|
| Emulator they launch (copy new builds over this `azahar.exe`; DLLs are already there) | `C:\Users\super\Downloads\azahar-windows-mxe-20261005-9a838a7\` |
| Azahar user dir | `C:\Users\super\AppData\Roaming\Azahar\` |
| HUD config (installed) | `...\Azahar\load\screen_regions\000400000019A200.ini` |
| HUD art and tables (installed, full set) | `...\Azahar\load\screen_regions\000400000019A200\` |
| Learned/persisted HUD cache | `...\000400000019A200\cache\` |
| Draw tracer switch (contents `on`/`off`, keep `off`) | `...\Azahar\load\screen_regions\trace.on` |
| Save states (slots 01–10) | `...\Azahar\states\000400000019A200.NN.cst` |
| **Full RomFS dump** (all game files) | `...\Azahar\dump\romfs\000400000019A200\` |
| Recorder output (`rec_*.zst`: RAM and bottom-screen snapshots) | `...\Azahar\dump\screen_regions\` |
| Logs (Azahar log, `drawtrace.txt`) | `...\Azahar\log\` |
| Game ROM files | `C:\Users\super\Downloads\Shin Megami Tensei IV - Apocalypse (USA).3ds` and `.zip` (installed copy is in `sdmc`) |
| In-game saves (copies) | `C:\Users\super\Downloads\sdds4game*.sav` |

Cheats folder: empty.

### Save states

| Slot | Contents |
|---|---|
| 1 | Battle; the user tested items here |
| 2 | Boss-like battle |
| 3 | Crashes sr_tool |
| 4 | Two Mishaguji, Nanashi's turn, skill list open |
| 8 | Partner Asahi's Cheer playing (sparkles on the bottom screen) |
| 9, 10 | Gladiator-type enemy, Nanashi's turn, skill list open |
| 5–7 | Recent, not examined |

Party in these states: Nanashi Lv999, Inanna (HP 10685), Shiva (HP 10761), Alilat. All have Smirk.

### Keyboard

| Key | Button |
|---|---|
| a | A |
| s | B |
| z | X |
| x | Y |
| t / g / f / h | D-pad up / down / left / right |
| q | L |
| w | R |

F4 pauses. Save/load states via the Emulation menu.

---

## 3. Build, release, install

1. **Push.** Push to `single-screen`.
2. **Build.** CI workflow `citra-build` builds `windows-mxe` in about 9 minutes. Then `single-screen-release` updates the release `single-screen-latest`, asset `azahar-single-screen-windows.zip`.
   - The `citra-format` failure is old and comes from upstream files; ignore it.
3. **Download.** The REST API works; GraphQL is blocked for Claude sessions.
   ```
   gh api repos/wigwav/azahar/releases/tags/single-screen-latest --jq '.assets[] | [.id,.name,.updated_at]|@tsv'
   gh api -H "Accept: application/octet-stream" repos/wigwav/azahar/releases/assets/<id> > a.zip
   ```
   Check that the folder name inside the zip contains your commit hash.
4. **Compress.** `azahar.exe` is about 37 MB, over the 30 MB file-transfer limit. Compress it with `upx --best` (about 15 MB; UPX source was built at `/home/claude/upx`).
5. **Install, only when the user asks:**
   - `azahar.exe` goes into the Downloads folder above.
   - The `.ini` and any changed art go into `load\screen_regions\...`.
   - The HUD reloads the ini automatically; to be safe, ask the user to restart Azahar.
   - If only the ini or art changed, no new exe is needed.

---

## 4. Headless test environment (Linux)

### 4.1 Build

Configure a normal Linux build of the repo, then:

```
ninja sr_tool        # headless tool
ninja video_core     # also compiles the OpenGL renderer: check it compiles
```

### 4.2 sr_tool

Usage: `sr_tool <user_dir> <game file> < commands`

- Software renderer, no frame limit.
- Environment variables:
  - `SR_GAME_FILE=<image>` substitutes the game file named inside a state.
  - `CITRA_ALLOW_STATE_BUILD_MISMATCH=1` loads states from other builds.
  - `CITRA_LOG_ROMFS=1` logs RomFS reads.
  - `SR_ROMFS_SUB=317000:54b000` remaps the RomFS offset (see 4.3).
  - `SR_INTERP=1` uses the interpreter instead of the JIT.

Commands (one per line):

| Command | What it does |
|---|---|
| `load N` / `save N` | Load / save a state slot. **Restart sr_tool before each load**: a second load crashes. |
| `run F` | Run F frames |
| `hold BTN F` / `tap BTN F` | Press a button. Buttons: a b x y l r up down left right start select. |
| `film PREFIX COUNT STEP` | Screens + battle object (`0x12000` bytes) + 4 units, every STEP frames. Use absolute paths; the tool's working dir is the game dir. |
| `dump PATH` | `SRRAW1` snapshot (`rec.py Raw`) |
| `hud INI ASSETS PROFILE OUT.rgba` | Render the HUD canvas (1920x1080 RGBA) |
| `mem ADDR SIZE PATH` | Read guest memory to a file |
| `w8` / `w16` / `w32 ADDR VAL` | Write guest memory (invalidates the JIT for code addresses) |
| `watch ADDR SIZE` | Log every CPU read/write in the range to stderr with PC/LR |
| `regs` | Live CPU registers |
| `threads` | Guest thread list (status, PC) |
| `pc` | Current PC |
| `fx learn\|capture\|off`, `fxdump`, `fxsave`, `fxload` | Effect-layer experiments |
| `hudbench`, `wfile`, `wrec`, `touch`, `quit` | Misc |

**Input timing that works:**

- Scroll lists with `tap down 12` then `run 4`.
- Confirm with `run 40`, then `hold a 10`, then `run 40`. Shorter waits drop the A press.
- From a state with the list open: `tap right 25` moves the command to Item.
- `hold b 6` twice from slot 4 closes the list to the command row.

### 4.3 The test copy of the game: important

`/home/claude/smt4a/emu/smt4a.cxi` was rebuilt from a **partial** dump (755 files, about 338 MB of about 1.7 GB). Missing files read as zeros. Two consequences, one fixed and one still open:

1. **RomFS offset (fixed).** The states were made with the user's install, whose RomFS starts at another offset than the rebuilt image. Use `SR_ROMFS_SUB=317000:54b000`. A file at RomFS offset R is at image offset `0x54f000 + R`; there is an extra 0x4000 somewhere in the read path, hence `54b000`.
2. **Enemy turns hang (open).** The enemy AI script (e.g. `battle/AI/MISHAGUJISAMA.bf`) and effect files are zeros, so the game's script VM (`0x224880`) loops on a null context and the enemy turn never starts. Fix:
   1. Run to an enemy turn with `CITRA_LOG_ROMFS=1` (e.g. `ctl.py 4 26`).
   2. Run `python3 tools/smt4a/needed.py <stderr log>`. It prints the missing paths as Windows paths in the PC dump.
   3. Fetch those files from `...\Azahar\dump\romfs\000400000019A200\` (`battle\AI\`, `battle\effect\` and others).
   4. Run `fillromfs.py` to write them into the image at their offsets. It uses `smt4a.cxi.json`, `romfs_layout.json` and `have.json`.
   5. Repeat until the turn plays.

   Even better: rebuild the whole image from the full PC dump (`mkcxi.py`) so everything is present.

### 4.4 Previewing the HUD

`see.sh NAME "cmd"...` runs the commands, renders the HUD over the top screen (1280x720 preview) next to the real bottom screen, and saves `NAME.png`.

To simulate damage: `w32 0x85f9360 <newHP>` (Inanna in slots 4, 8, 9), then render.

---

## 5. HUD engine reference

### Ini

`[profile battle]` with `hide_bottom = 1` and a `[hud battle]` block of lines: `let NAME = expr` and element lines.

### Elements

| Element | Meaning |
|---|---|
| `rect X Y W H #RRGGBBAA` | Filled rectangle |
| `image X Y W H file.png` | PNG (`{0}` in the name is substituted; `@name` uses a capture; `crop=` source rect) |
| `text X Y SIZE #COLOR left\|center\|right "fmt {0} {1}"` | Text (`v=` values, `fit=` max width) |
| `bar X Y W H #fg #bg` | Bar (`v=` value, `max=` maximum) |
| `capture NAME X Y W H` | Keep a bottom-screen area under a name |
| `fx SX SY SW SH X Y W H` / `fxlearn` | Effect layer (unused) |

### Options

| Option | Meaning |
|---|---|
| `if=` / `and=` | Visible when both are true |
| `ox==expr` / `oy==expr` | Offset |
| `fade==expr` | Opacity 0–100 |
| `opacity=` | Constant opacity |
| `linger=MS` | Stay visible for MS after the condition drops |
| `hold=` | Keep the previous visibility while true |
| `names=` / `lookup=` | Index into a text table |

Write expressions after `=` with no spaces. `hud3gen.py`'s `c()` helper strips them.

### Expression functions

- `u8` / `u16` / `u32(addr)` — memory reads
- `recent(cond, ms)` — true if `cond` was true within the last `ms`
- `track(key, value, ms)` — change of `value` summed over the window
- `trackage(key)` — ms since the last change
- `time()` — current time
- `pix(x, y)` — bottom-screen pixel as 0xRRGGBB, sampled by the renderer
- `lookup('file.txt', idx)` — line `idx` of a text table
- `has(s, sub)`
- `sjis(addr, off)`
- `min`, `max`, `clamp`, `abs`
- `?:`, `&&`, `||`, comparisons, arithmetic; string `==`

### `smt4a_*` helpers (in `hud_expr.cpp`)

| Helper | Returns |
|---|---|
| `obj()` | Battle UI object (found by header scan) |
| `save()` | Save pointer |
| `unit(k)` | Party unit pointer |
| `demon(k)` | Demon id |
| `hp` / `mp` / `maxhp` / `maxmp` / `level(k)` | Member stats |
| `cmd(A, i)` / `cmdcount(A)` | Command list |
| `cmddesc(line)` | Command description line |
| `entries(A)` / `entry(A, i)` | Skill list (−1 Attack, −2 Shoot) |
| `skillname` / `skillicon` / `skilldesc(line)` / `cost(A, skill)` | Skill info |
| `item(i)` / `itemcount(id)` / `itemname(id)` | Item list |
| `rec(k)` / `prec(k)` | Demon records |

---

## 6. Current state (installed on the user's PC as of 2026-10-07)

### 6.1 Battle HUD (`hud3gen.py`)

- **State detection:**
  - `$ACT` — a party member is acting
  - `$INP` — input stack not empty
  - `$CROW` — command row drawn, via 3 bottom-screen pixel probes
  - `$ROW`, `$ON` — command phase
  - `$TGT` — ally targeting
  - `$LON` — list open
  - `$TALK` / `$NEG` — talk / negotiation
  - `$POP` — popups
  - `$CLEAR` — screens where the cards hide
- **Layout (1920x1080):**
  - Button hints top-left.
  - Acting member's portrait, skill/item list (6 rows) and 2-line description bottom-left.
  - Command row centred at y 690.
  - Party cards along the bottom edge.
- **Lists:** skill rows grey when MP is too low. Item rows grey when unusable: heals when everyone alive is at full HP/MP, revives when nobody is down. Cure items are not handled yet.
- **Targeting:** "CHOOSE A TARGET" panel in the list's place; the targeted card gets a gold frame and a "TARGET" chip.
- **Party cards:**
  - Size and position: 236x104 at y 966, x from 470, gap 12. Always there, never moved.
  - Contents: name, Lv (or SMIRK badge), HP/MP bars and **full** values.
  - Acting member's card lifts 6 px; a targeted card lifts 14 px.
  - Hit: card shakes for about 450 ms, red flash, `hit_0/1/2.png` strike mark (0–70, 70–150, 150–320 ms), red damage number rising over the card.
  - Heal: green flash and "+N".
  - Damage is the HP change summed over 1.8 s (`track`).
- **Talk menu**, Status popup, demon list/swap grid (stock 6x4 grid with bust-up faces), Effects/Analyze hints.

### 6.2 Camp main menu (`campgen.py`)

Native grid and command row driven by the game's cursor `[0x57F108]`. Other sub-screens use a bottom-screen panel.

### 6.3 Rejected approaches (do not repeat)

- Docking or showing the bottom screen, or pieces of it (portrait columns), on the top.
- Copying the bottom screen's effect draws onto cards (FxCapture). It also came out mirrored (a damage of 18 read "81") and included the game's damage digits.
- Cards that move to the top of the screen during actions.
- Values clamped at 9999.

---

## 7. Open tasks, in order

### 7.1 Skill and element of the current action

**Goal:** a per-element hit animation (Phys, Gun, Fire, Ice, Elec, Force, Light, Dark, Almighty, ailment), and showing which skill hit.

Progress:

- After Nanashi used **Hades Blast (id 162)** in slot 9, these u16 locations newly held 162:
  - `0x82cd498`
  - `0x85d6e24`
  - `0x85f9024`, `0x85f902c`, `0x85f903c` (inside unit 0)
  - `0x8650560` = `O+0x560`
  - `0x86518b8` = `O+0x18B8`
  - `0x8656450` = `O+0x6450`
  - `0x86935a8`
- Skill ids are rows of `smt4a_skills.txt`: Hades Blast 162, Heaven's Bow 187, Akasha Arts 363, Cheer 226.
- The last column of that table (or `battle/SkillData.tbb`) gives the icon/element; `smt4a_skillicon(id)` already returns the element icon index, and the `el_N.png` art exists.

Next:

1. Make enemy turns run headless (4.3).
2. Check which candidate holds the enemy's skill during its action, and which holds the target(s). Prefer offsets inside `O`.
3. Add an `smt4a_` helper, or use `u16($O+0x...)`.
4. Draw per-element strike art. Extend `hitart.py` or make better art. Keep it native-looking.

### 7.2 Status ailments

The memory location is unknown. Known: unit `+0x10E` u16 bit `0x10` = Smirk. Setting other bits showed nothing on the bottom screen.

Approach: get a member poisoned or bound (enemy turns needed), then diff the unit records and `O`.

Then:

- Show ailment icons on the cards.
- Grey cure items (`recv`; `ItemTable` `+0x3A` holds the ailment id they cure, `0x80` = all allies) when nobody has that ailment.

### 7.3 Check timing in motion

- The hit animation and number timing have only been checked in still frames.
- Make sure nothing covers the game's skill-name banner during actions (bottom of the top screen, about y 847–937 on the canvas).

### 7.4 Cleanup

- Remove FxCapture and the DrawFx path, or keep them gated.
- Make the tool scripts use relative paths.

---

## 8. Memory map (US version)

### 8.1 Battle UI object `O`

`O = smt4a_obj()`, usually `0x8650000`, found by header scan.

| Offset | Meaning |
|---|---|
| `+0x554` | Acting slot: 0–3 party, 4+ enemies |
| `+0x18A0` | UI mode: 11 command (lists, talk picker, popups), 16 ally target / demon list, 13 Effects, 14 Analyze, 0 other. Unreliable on demon turns; gate on `+0x14D4` and the command-row pixel probes. |
| `+0x14D4` | Input stack top; `0xFFFFFFFF` = no input (command confirmed / action playing) |
| `+0x113FC` | Skill/item list open (1) |
| `+0x5C8 + A*0x64` / `+0x5B8 + A*0x64` | Command cursor / command count for actor A |
| `+0x8E8` | Ally target cursor |
| `+0x764` / `+0x730` | Skill cursor / scroll |
| `+0x82C` / `+0x7F8` | Item cursor / scroll |
| `+0x96C` | u16 item id list, 0-terminated |
| `+0x11370` | Command description text |
| `+0xE64C` | Popup present |
| `+0x898` | Sub-picker open (0) |
| `+0xFA8C` | Command-row transform |

Command list (Next index): Nanashi `[0..7]` minus Item when there are no items, then drop 7, 5, 2 until the length ≤ count. Demons: `[0,1,3,4,6]`. Next = index of 6.

### 8.2 Party units

Unit `k` is at `[0x57113C] + 0x200E6 + k*0x408`. In slots 4/8/9 the HP fields are Nanashi `0x85f8f58`, Inanna `0x85f9360`, Shiva `0x85f9768`, Alilat `0x85f9b70`.

| Offset | Meaning |
|---|---|
| `+0xF2` | HP (u32) |
| `+0xFA` | MP (u32) |
| `+0xF6` | Max HP (u32) |
| `+0xFE` | Max MP (u32) |
| `+0x114` | Level (u16) |
| `+0x10E` | u16 flags; `0x10` = Smirk |

### 8.3 Other addresses

| Address | Meaning |
|---|---|
| `[0x57F108]` | Camp menu widget: `+0x78` count, `+0x88` cursor |
| `0x57F268` | Demon list UI |
| `0x57F69C` | Save pointer |
| `0x224880` | FLW0 script interpreter |
| `0x3e688c` | Partner-AI script runner state machine (object state at `+0xC`, context at `+4`) |

Bottom-screen command-row probe: cream pixels at (148,176), (168,176), (186,178).

### 8.4 Game data

**Item records** in `item/ItemTable.tbb` (TBCR container of TBL1 tables):

- Table at `0x1300`: ids 1–60.
- Table at `0x272b0`: ids 1951+.
- Each record is 0x6C bytes: name `+0`; tag `+0x20` (`hpup`, `mpup`, `rest` = revive, `recv` = cure, `mris` = full restore); HP `+0x34`; MP `+0x36`; HP% `+0x38`; `+0x3A` target/ailment; `+0x42` bit `0x10` = usable on allies in battle.

**FLW0 battle AI scripts:** `battle/AI/*.bf`. Opcodes: 7 PROC, 9 END, 8 COMM, 29 PUSHIS.

**Rendering:**

- Both screens render into `0x18000000`.
- Bottom-screen draws target fb 240x319, mostly immediate mode.
- The game runs at 30 fps.
- Bottom display transfer: input 240x320, not a texture copy.

---

## 9. Ephemeral workspace of the previous assistant

These locations existed only in the previous assistant's cloud workspace and will be gone:

- `/home/claude/smt4a/emu`: game image, test user dir with states
- `/home/claude/smt4a/hud3`: full art set
- `/home/claude/build-lin`
- `/tmp/claude-0/emu`
- `/mnt/user-data/outputs`: staged exe/ini/art

Everything durable is in the repo or on the user's PC as listed above.
