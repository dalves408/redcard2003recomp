# Direct3D 11 renderer

DX11 is the default renderer. Build and run the recommended RelWithDebInfo configuration with
`scripts/build.ps1 -Configuration RelWithDebInfo` and
`python scripts/run_game.py --configuration RelWithDebInfo` (Debug is for debugging and tests).

`src/redcard_d3d11.c` registers with the NV2A pushbuffer executor. Xbox vertex
programs and geometry decoding still run on the CPU; triangles, texture sampling,
depth/stencil, alpha testing, blending and color writes run on the hardware GPU.
The framebuffer window also presents through a DX11 swap chain. This reuses the
existing keyboard/controller path and Xbox Debug Kit HUD and captures.

GPU color targets are copied back before the executor snapshots each completed
frame. Sampling a pending render target also synchronizes it. This deliberately
retains the guest framebuffer semantics during bring-up, at a readback cost.
The staging resource uses CPU read access, following the DX11
[Map](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-map)
and [CopyResource](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11devicecontext-copyresource)
contracts.

Textures use the toolkit decoder, including RedCard's indexed menu artwork and
its guest palette. Exact source-byte and palette comparisons check changes on every bind, including writes between draws in the same frame.
DX11 errors are logged explicitly; the renderer does not silently claim GPU
success if device creation or a resource operation fails.

Environment overrides:

- `RECOMP_D3D11=0`: select the previous software rasterizer for comparison.
- `RECOMP_D3D_PRESENT=0`: use the GDI window presenter independently.
- `RECOMP_D3D11_LOG=1`: log a bounded sample of decoded draw state.

The 3D path carries all four texture coordinate sets, secondary vertex color,
fog, and Xbox general/final register combiner state to DX11. Its HLSL interpreter
is adapted from the user's NHL Hitz renderer at revision `9e8968bf`.
Shader variants specialize combiner control words and keep changing colors in
uniforms; an in-memory cache reuses them. First encounters can briefly compile.

Current limits: targets currently require 32-bit color. Texture sampling honors
the guest `SET_TEXTURE_FILTER` word (NEAREST stays point; otherwise linear, with
default 4× anisotropic via `RECOMP_D3D11_ANISO` and a GPU mip chain for
non-screen-sized textures). This is still not a claim of complete NV2A
compatibility. Cube textures and specialized dependent texture addressing
remain unsupported.

Validation:

- `redcard_d3d11_smoke` (Debug): hardware clear/readback, vertex color, depth,
  alpha discard, texturing, blending, write masks, render-target sampling,
  final/general combiners, sampling a second texture stage and swizzled UVs
  despite stale linear pitch registers (12 checks).
- Toolkit `tests/nv2a_ffp_backend`: palette decode regression, decoded geometry,
  render state, winding and synchronized presentation.

The executable logs the actual adapter and feature level as `[RC DX11]`.
The normal game logs are under `%LOCALAPPDATA%\RedCardRecomp\debug`.

## GPU vertex programs (default on)

Cached HLSL vertex programs with bounded raw attribute/constant uploads and CPU
fallback are **on by default** for the DX11 play build (async compile also default).
Set `RECOMP_D3D11_GPUVS=0` to force the CPU interpreter. `RECOMP_GPUVS_VERIFY=1`
samples GPU stream-output against the CPU interpreter (verify compiles sync).
`RECOMP_D3D11_GPUVS_ASYNC=0` forces synchronous first-time compiles.

`RECOMP_RENDER_CSV=<file>` records actual frame timestamps and individual map,
decode, upload, draw API and readback timings. `RECOMP_PRESENT_CSV=<file>` records
window upload/submission/Present timings. Callback and nested-operation durations
overlap; they are not additive GPU execution times. Direct GPU presentation and
render-target SRV binding remain future work.
