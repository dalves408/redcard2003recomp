# RecompLauncher (SDK Level 3)

RedCard 2003 can be installed, set up and played from [RecompLauncher](https://recomplauncher.com/sdk).
The launcher reads `recomplauncher.json` from the top of the release ZIP. The Inno Setup installer is
unchanged for everyone else (it installs the same file, so an installed copy can also be added in the
launcher's Developer tools).

| Level | What | Source |
|---|---|---|
| 1 | Plays `Redcard 2003.exe`; the player's USA disc image or folder (`default.xbe` check) | `scripts/recomplauncher_manifest.py` |
| 2 | Render scale, fullscreen, stretch, VSync, frame rate, MSAA, texture filtering, hi-res textures: the `[display]`/`[textures]` keys in `%LOCALAPPDATA%\RedCard2003\settings.ini` that F1 and `config.exe` use | same |
| 3 | `prepare`: `redcard_import.exe --prepare {game} {install}/game_files` (no window, `progress` lines, exit 0 only when complete); `game.dumps`: Redump USA, MW-015, 7,825,162,240 bytes, CRC-32 `630af9ec` (<http://redump.org/disc/54255/>) | same, `installer/redcard_import.c` |

Also declared: saves (`%LOCALAPPDATA%\RedCard 2003\UDATA`, `TDATA`), the **logs** folder the launcher attaches
to a player's report (`%LOCALAPPDATA%\RedCard 2003\logs`, or `logs\` beside the exe with `portable.txt`),
exit code 0 = normal quit, the importer's exit codes 2–6, and `looks` (D3D11, 480 lines).

Game side:
- The release build logs everything to `logs\redcard_<date>_<time>.log` (newest 10 kept), with a `[CRASH]`
  line for an unhandled exception. `RECOMP_LOG=0` turns it off.
- `RECOMPLAUNCHER_MODE=bigpicture` (or `bigscreen`) starts fullscreen whatever the setting says; the saved
  setting is not changed, so a desktop start still uses it.
- A new launcher install starts fullscreen: `config.new` writes `display/fullscreen=1` when the launcher
  creates `settings.ini` (as the Windows Setup does on a fresh install).
- The exe changes to its own folder when started from elsewhere without `game_files` in the working folder.

## Build

`scripts/build_installer.ps1` writes `build/<config>/recomplauncher.json` and
`installer/Output/RedCard2003-v<version><suffix>-Windows-x64.zip` (+ `.sha256`) next to the Setup exe.
Publish the ZIP on the GitHub release; the launcher prefers a ZIP. The manifest validates against
`https://recomplauncher.com/schema/recomplauncher.json` (0 errors).

## Not done

- The hi-res texture pack is not offered by the launcher: its `prepare` step would have to unpack the 7z, and
  Windows 10's `tar.exe` cannot read 7z (Windows 11's can). Players get it from the Windows Setup.
- Not yet opened in RecompLauncher itself (Settings > Developers > Add a build).
