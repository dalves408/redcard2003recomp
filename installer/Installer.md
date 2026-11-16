# RedCard 2003 Windows installer

Port of the NHL Hitz Pro Inno Setup installer (`nhl_hitz_pro.iss`) with product strings/paths/exe renamed for RedCard.

**Release exe name:** a release installs the game as **`Redcard 2003.exe`** (the build outputs
`build\Release\redcard_recomp.exe`; `DestName` renames it). An update over an older install
deletes the old `redcard_recomp.exe`/`.pdb`. The diagnostic package (`-DebugBuild`) keeps
`redcard_recomp.exe`.

## Options / pages

| Page / option | Behavior |
| --- | --- |
| Welcome | Explains recomp; disc not included |
| Update / repair | If AppId install exists with complete `game_files`, shows installed vs this Setup; repair / update / quit |
| Install folder | Always shown; default `C:\Games\RedCard 2003`; previous install pre-filled (`UsePreviousAppDir`) |
| Your copy of the game | Disc image (`.iso`/`.xiso`) or folder with `default.xbe` — skipped when `game_files` already present |
| Tasks | **Download and install the 4x hi-res texture pack** (unchecked; hidden when the current pack is already installed) and **Create a desktop shortcut** (unchecked). |
| Texture pack | `texture_payload.iss` pins `texture.bin` 1.0.0 (1,030,982,957 bytes, SHA-256 `89badf42…`, URL `releases/download/textures-1.0.0/texture.bin`). `redcard_texture_download.exe` downloads it (resumable, cached in `%LOCALAPPDATA%\RedCard 2003\Downloads`) or uses a `texture.bin` beside Setup, and only hands it over once size and hash match. Setup extracts to a staging folder, checks every file against `manifest.txt`, then swaps it in as `texture_pack`. A failure leaves the game installed without textures. `/TASKS=hires`, `/REPAIRTEXTURES=1`, `/KEEPTEXTUREBIN=0|1` |
| Texture add-on | `redcard_textures_addon.iss` → `RedCard2003-HiRes-Textures-<version>-Setup.exe`: the same download/check/extract for an installed game (folder found from the game's uninstall entry). Uses `texture.bin` beside it, otherwise downloads it; finishes at once when the pack is already current. Exit code 1 when the pack was not installed |
| Fullscreen | A fresh install (no game files in the folder yet) writes `[display] fullscreen=1` to `%LOCALAPPDATA%\RedCard2003\settings.ini`. An update or repair leaves the player's settings alone |
| Pack already present | Counted as installed in `{app}\texture_pack` or `%LOCALAPPDATA%\RedCard2003\texture_pack` (both are where the game loads it from) |
| Release asset | `textures-1.0.0` pre-release on `dalves408/redcard2003recomp` (`texture.bin`, `.sha256`, `texture-payload.json`). The repo must be **public** for players to download it |
| RecompLauncher | `recomplauncher.json` is installed next to the game; see `RECOMPLAUNCHER.md` |
| Shortcuts | Start Menu (+ optional desktop) → `Redcard 2003.exe`; Start Menu **Settings** → `config.exe`. |
| Silent source | `/SOURCE=<iso or folder>` |

## Build

Requires Visual Studio C++ tools, a Release game build (debug kit off), Inno Setup 6, and a verified `game_files\` tree (for `manifest.h` / XBE hash).

```powershell
# From repo root — builds game, redcard_import.exe, then Setup exe
.\scripts\build_installer.ps1 -AppVersion 1.0.0

# Or skip rebuilding the game if the Release build is already current:
.\scripts\build_installer.ps1 -AppVersion 1.0.0 -SkipGameBuild
```

Output: `installer\Output\RedCard2003-Setup-<version>.exe`

Regenerate the import manifest after game-file set changes:

```powershell
.\.venv\Scripts\python.exe installer\gen_manifest.py game_files
```

## Branding

- Mark: `resources\user-icon-redcard.png` — red card + flames + soccer ball, “REDCARD 20-03”
- Multi-size `.ico` (16/32/48/64/128/256): `resources\redcard_2003.ico` — Setup icon, installed shortcuts (`IconFilename`), and embedded in the game exe / `config.exe` via `.rc`
- Do **not** use `icon-01-red-card` or other option-set art
- Wizard corner images: `installer\wizard_small_{55,64,80,96,128}.bmp` (same art)
- Rebuild icon assets from the master JPG with the snippet in `scripts/build_installer.ps1` comments / prior convert step if the mark changes

## Notes

- Test Setup on a separate PC or VM, or build it with test `/DGameGuid=` and `/DPackGuid=` values, so a real install is not changed.
- Supported disc: Xbox USA Title ID `4D57000F` (SHA-256 of `default.xbe` must match the recomp).
- Import exit codes match Hitz (`2`…`6`) for RecompLauncher `--prepare` later.
