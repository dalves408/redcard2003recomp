# RedCard 20-03 — PC recompilation

A Windows PC port of the original Xbox version of **RedCard 20-03**, made by static recompilation with
[xboxrecomp](https://github.com/sp00nznet/xboxrecomp) and drawn with Direct3D 11.
**Version 1.0.**

You need your own copy of the game: the **USA Xbox disc** (MW-015) as a disc image (`.iso` / `.xiso`)
or as a folder with its files. No game files are included.

## Install and play

1. Download `RedCard2003-Setup-1.0.0.exe` from [Releases](https://github.com/dalves408/redcard2003recomp/releases/latest).
2. Run it and pick your disc image or game folder. Setup checks it is the supported version and copies
   the game files it needs (about 2.5 GB). The default install folder is `C:\Games\RedCard 2003`.
3. Optional: tick **Download and install the 4x hi-res texture pack** (1.03 GB download, 1.9 GB on
   disk). Already installed? Run `RedCard2003-HiRes-Textures-1.0.0-Setup.exe` from the
   [textures release](https://github.com/dalves408/redcard2003recomp/releases/tag/textures-1.0.0) to add it.
   Both use a `texture.bin` placed next to them instead of downloading, and check the pack before using it.
4. Start **RedCard 2003** from the Start menu. A fresh install starts fullscreen; running Setup again to
   update or repair keeps your settings, game files and saves.

**RecompLauncher:** the game is also set up for [RecompLauncher](https://recomplauncher.com) (SDK Level 3):
it checks your disc (with a verified badge for a Redump-matching USA image), copies the game files,
shows the settings on the game's page and starts the game. See [RECOMPLAUNCHER.md](RECOMPLAUNCHER.md).

### Requirements

- Windows 10 or 11, 64-bit
- A Direct3D 11 graphics card (feature level 11.0). On laptops with two graphics chips the game asks
  for the NVIDIA or AMD one.

## Settings

Press **F1** in the game, or open **RedCard 2003 Settings** (`config.exe`) from the Start menu:
render scale (1x–4x), fullscreen, 4:3 or stretched picture, VSync, frame rate (30–360), anti-aliasing,
texture filtering, hi-res textures and key bindings. **Alt+Enter** switches fullscreen.
Settings are saved in `%LOCALAPPDATA%\RedCard2003\settings.ini`, saves in `%LOCALAPPDATA%\RedCard 2003`.

## Controls

An Xbox-style (XInput) controller works out of the box. The keyboard works too, alongside a controller:

| Action | Key |
| --- | --- |
| Start / Back | Enter / Backspace |
| D-pad | Arrow keys |
| A / B / X / Y | Z / X / C / V |
| Left / right stick | WASD / IJKL |
| White / Black | Q / E |
| Left / right trigger | 1 / 3 |

See [CONTROLS.md](CONTROLS.md) for the full mapping.

## Problems

Each run writes a log to `%LOCALAPPDATA%\RedCard 2003\logs` (the last 10 are kept). Please attach the
newest one when you report an issue; RecompLauncher sends it with its report automatically.

## Supported game version

| Property | Supported version |
| --- | --- |
| Disc | RedCard 20-03, Xbox, USA, serial MW-015 |
| Title ID | `4D57000F` |
| default.xbe SHA-256 | `652a3afcd18b4aa4b5e4d0ac0d776e2b72de4f36c7ebceb344e87f27bb830752` |
| Redump image | 7,825,162,240 bytes, CRC-32 `630af9ec` ([disc 54255](http://redump.org/disc/54255/)) |

Other regions and revisions are not supported.

## Build from source

Install Git, Python 3.12+, Visual Studio 2022 Build Tools (**Desktop development with C++** and a
Windows SDK), and CMake 3.20+ (the build script also finds the copy bundled with Visual Studio).
In PowerShell:

```powershell
git clone https://github.com/dalves408/redcard2003recomp.git
cd redcard2003recomp

# Fetch the pinned runtime, apply the project's runtime patch, create .venv.
.\scripts\setup.ps1

# Extract and verify your own disc.
.\.venv\Scripts\python.exe scripts/extract_disc.py 'C:\path\to\RedCard 20-03 (USA).iso' game_files

# Analyze the XBE and generate its C translation.
.\.venv\Scripts\python.exe scripts/pipeline.py --lift

# Build and run from the project root.
.\scripts\build.ps1 -Configuration RelWithDebInfo
.\.venv\Scripts\python.exe scripts/run_game.py --configuration RelWithDebInfo
```

- **RelWithDebInfo** is optimized and keeps symbols: use it to play and develop. It includes the
  [Xbox Debug Kit](https://github.com/dalves408/Xbox-Debug-Kit) (FPS/CPU/GPU display, Delete marks an
  issue, Insert records a clip). **Debug** is for stepping through code and the regression tests.
- **Release** (no debug kit) is what ships. `.\scripts\build_installer.ps1 -AppVersion 1.0.0` builds it
  and makes the game Setup, the texture add-on Setup and the RecompLauncher ZIP in `installer\Output`
  (needs [Inno Setup 6](https://jrsoftware.org/isinfo.php)). See [installer/Installer.md](installer/Installer.md).
- `.\scripts\test.ps1` runs the regression suites (renderer, combiners, CRT, input, audio pages).
- [dependencies.json](dependencies.json) pins the runtime revisions; `scripts/xboxrecomp-runtime.patch`
  holds this project's runtime changes.

Generated C translates the game's machine code; it is not recovered original source. The disc image,
extracted files, Xbox executable and generated translation must not be committed.

More: [RENDERER.md](RENDERER.md) (Direct3D 11 back end) and [DEBUGGING.md](DEBUGGING.md) (switches,
logs and diagnostics).

## Credits

- [sp00nznet/xboxrecomp](https://github.com/sp00nznet/xboxrecomp): recompilation tools and Xbox runtime.
- [dalves408/nhlhitz2003recomp](https://github.com/dalves408/nhlhitz2003recomp): related-engine reference and adapted register-combiner shader.
- [dalves408/Xbox-Debug-Kit](https://github.com/dalves408/Xbox-Debug-Kit): development diagnostics.

See [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for attribution and upstream notices. RedCard 20-03
and its original content belong to their respective owners.
