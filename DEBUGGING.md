# Diagnostics and performance display

Xbox Debug Kit is integrated from https://github.com/dalves408/Xbox-Debug-Kit
at revision `0ce3b891d49a4bfa256bc3131019d8a4d0c0637d`.
Its sources remain unchanged. Game integration is in `src/redcard_debug.c`.

The default development build enables `REDCARD_DEBUGKIT`. The kit records logs,
guest CPU registers on crashes, minidumps of guest RAM, hang diagnostics, and
marked game-image captures under `%LOCALAPPDATA%\RedCardRecomp\debug`.
`scripts/build.ps1` and `scripts/run_game.py` default to the Debug configuration.
Use `-Configuration RelWithDebInfo` and `--configuration RelWithDebInfo` explicitly
for playing and performance work; it keeps PDB symbols, so crash reports stay readable.
Disable the kit separately for performance runs.
Press Delete in the game window to mark a problem. Press Insert to record about
three seconds of the presented game image (throttled to ~30 fps) to an
uncompressed `.avi` under `%LOCALAPPDATA%\RedCardRecomp\debug\clip_YYYYMMDD_HHMMSS.avi`.
The session log gets `[RC CLIP]` start/stop lines, and a red `CLIP:` status appears
under the FPS HUD while recording or saving. The normal window-close and
guest-return paths notify the kit. The runtime report hook remains independent
so its missing-file registration cannot replace the debug exit notification.
The kit's own crash/abort handling covers abnormal exits. The separate diagnostic
watchdog deliberately terminates bounded tests; it is not a normal gameplay exit.

Use `scripts/collect_debug.ps1` to package a local report. No report is uploaded.
Keep the EXE and matching PDB together for crash diagnosis.

FPS, CPU% and GPU are displayed on the game image and window title:

- FPS counts guest presented frames, not repeated window repaints.
- CPU% is process CPU time normalized across the machine's logical processors.
- GPU% polls Windows GPU Engine performance counters once per second, selecting
  the busiest engine reported for this process. It shows N/A if no matching
  counter is available. The default DX11 rasterizer runs game triangles on the
  GPU; guest vertex programs still run on the CPU. See `RENDERER.md`.

The HUD is drawn on the host scanout copy, never the guest framebuffer. It works
with the existing GDI and D3D presentation paths. Kit screenshots capture the
game image before the performance HUD.

For a normal non-kit build configure `-DREDCARD_DEBUGKIT=OFF`; the performance
display remains available. The kit is intended for development builds.

Release builds (no kit) log everything the game prints to
`%LOCALAPPDATA%\RedCard 2003\logs\redcard_<date>_<time>.log` (or `logs\` beside the exe when
`portable.txt` is there), keep the newest 10, and add a `[CRASH]` line for an unhandled exception.
RecompLauncher attaches the newest one to a player's report. `RECOMP_LOG=0` turns it off; a stderr
already redirected to a file is left alone.

Development fast boot: `REDCARD_SKIP_LOGOS=1` omits only the two publisher movies
by their verified game IDs. It leaves the title screen, menus and match loading
logic intact. The gameplay intro can be skipped with Start using the real input
path. The option is off by default.

Graphics faults: while the game runs from the project root, create an empty
`analysis\draw_dump.trigger` file. The next frame's DX11 batches are written to the
session log as `[RC DRAW]` lines: target, vertex count, screen x/y and z bounds,
w range, texture coordinates, texture address/format/size, combiner use and cull
state. The trigger file is deleted when the dump starts. A batch with `w=1..1`
and world-sized x/z values was drawn without a transform. That is how the
world-space stadium banner across the top of match frames was found
(**RED-8** kickoff transition glitch). Inline-array FFP batches are transformed by
default; `RECOMP_FFP_INLINE=0` restores the old screen-space handling for comparison.
If that signature returns, the session log gets a one-shot
`[RC DX11] WARN RED-8 untransformed world-space batch` line.

The trigger is also checked during drawing, so it works through stretches with no
flips (loading screens); the dump then runs until the next flip. Each dumped frame
also writes the image it produced to `analysis\draw_dump_f<frame>.bmp`, so a draw
list can be matched to what was on screen.

`RC_TRACE_TEX=<hex guest address>` logs every draw that samples that texture
(`[RC TRACE] f<frame> ... into <target>`) and a `[RC TRACE] flip` line per frame.
That found the flashing sky in the pre-match close-ups: the sky dome (`80BCD000`)
was missing from some frames because one horizon vertex sat exactly in the eye plane.

## Rendering and presentation switches

- `RECOMP_AC97_READY`: audio is **on by default**; `RECOMP_AC97_READY=0` runs silent.
- `RECOMP_SCANOUT_STALL_MS` (default 250, 0 disables): when the game keeps drawing
  without swapping for this long, the window shows the last finished full-screen
  buffer. The boot copyright screen after the Point of View logo is drawn this way
  during the main data load. Without it the screen was black for about 4 s.
- Swizzled render-to-texture surfaces are read back in Morton order. Each one seen
  is logged once as `[RC DX11] swizzled render target ...`. Row-order readback made
  scene dissolves sample scrambled tiles.

## GPU selection and GPU vertex shaders

- The exe exports `NvOptimusEnablement` and `AmdPowerXpressRequestHighPerformance`, so
  dual-GPU laptops run it on the dedicated GPU. `RECOMP_D3D11_ADAPTER=<index | name part>`
  (e.g. `intel`, `nvidia`) forces an adapter; every adapter is logged as `[RC DX11] adapter N`.
- `RECOMP_D3D11_GPUVS=0`: vertex programs on the CPU. `RECOMP_GPUVS_VERIFY=1` (best with
  `RECOMP_D3D11_GPUVS_ASYNC=0`): compare GPU and CPU vertex outputs per draw; mismatches log
  `[GPUVS-VERIFY] ... component=N` (0–3 pos, 4–7 color, 8–11 d1, 12–15 tex0, 16+ tex1..3, 28 fog).
  This found the Intel HD 4000 texture-coordinate bug.
- `RECOMP_GPUVS_DUMP=<dir>`: write each generated vertex program body as `gvs_<hash>.hlsl`.

## Hi-res texture pack

- The DDS pack is looked up in `RECOMP_TEXPACK`, `%LOCALAPPDATA%\RedCard2003\texture_pack`,
  then `texture_pack` next to the exe. It now applies at **any** Render Scale (it used
  to switch off silently below 2x). F1 → Hi-res textures and `[textures] enabled` in
  `%LOCALAPPDATA%\RedCard2003\settings.ini` turn it on and off.
- `RECOMP_TEXPACK_DIAG=1` logs one `[RC PACK] HIT/miss` line per distinct texture
  hash. Render-to-texture captures are never looked up, and an address that misses 8
  times within 3 s (a surface the game redraws every frame, e.g. `81070000`) is not
  hashed again for 10 s.

## Audio

- `RECOMP_AUDIO_WAV=<file>`: record exactly what is handed to the output device.
- `RECOMP_APU_TRACE=1`: once a second per voice, `[APU-VSTAT]` (format, rate, volume,
  the memory class its samples come from: `named`/`ram`/`win`/`hiwin`/`img`) and
  `[APU-VBINS]` (every mixbin and volume), plus `[APU-BIN]` per-bin peaks and clips.
- `RECOMP_APU_VOICE_DUMP=all` (+ `RECOMP_APU_VOICE_DUMP_FILE=<prefix>`): each voice's
  decoded samples, **before resampling**, at the voice's own rate. Heavy: with the trace
  on at boot it can stall the game; use with `REDCARD_SKIP_LOGOS=1`.
- `[APU] voice N: ADPCM header invalid at X (class)` / `... page switched`: an ADPCM
  block read from the wrong copy (heap RAM vs contiguous window) of a page and corrected.
- `RECOMP_APU_RESAMPLE=linear`: old linear interpolator (default is windowed sinc).
- `RECOMP_APU_MIXDOWN=legacy`: old even-left/odd-right bin sum (default folds bins 0–5
  as 5.1 speakers).
- `RECOMP_AUDIO_BUF_SAMPLES` / `RECOMP_AUDIO_BUF_COUNT` (default 512 × 8),
  `RECOMP_AUDIO_PRIORITY=0` (frame thread at normal priority).

## Controller and keyboard input

- `RECOMP_INPUT_DIAG=1`: once a second, the key table (W, Up, Z), whether a window
  of this process has focus, and what pad 0 reports (buttons, left stick, analog A,
  overlay block). `RECOMP_KEY_TRACE=1`: the first 40 key-downs as they arrive.
- `RECOMP_USB_STATS=1`: OHCI transfer counts and endpoint state every 5 s.
- `[OHCI] DMA page X now resolves to Y; keeping X`: the controller's DMA pages keep
  the mapping they had on first use. Before this, RedCard's report buffer (`012E0000`)
  flipped into the contiguous window once the contiguous allocator passed ~19 MB, so
  reports went where XAPI never read and in-match input froze on the last press.
