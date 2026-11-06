# The renderer on both systems

`src/render` (the `D3D11Renderer` logic, text, layout, media, caption, effects, presentation and damage) builds on Linux over the same
`gpu_*` calls (`cmake/linux_render.cmake`: `maicord_render_core`, `maicord_atlas_gpu`, `maicord_renderer`, `maicord_paint_program`, with
the scene library `maicord_ui` and the theme in `maicord_theme`). The GPU backends are `src/gpu/d3d11` and `src/gpu/gles`.

- `renderer_initialize` takes a `GpuSurfaceDesc`: the application passes `renderer_window_surface(window)`; a null window is an offscreen
  surface on the GLES backend (the frame texture alone, no swap; `gpu_surface_target_texture` reads it back), which is how Linux tests
  run.
- Shared renderer code names no HWND, `GetTickCount64` or `GetClientRect`: the clock is `core/time.hpp`, files `core/data_path.hpp` and
  `os/file.hpp`, local dates `os/local_time.hpp`.
- A new renderer test is written against `gpu_*` and the renderer API (`gpu_surface_target_texture`, `gpu_readback`), not `ID3D11*`, so
  it runs on both systems. The `tests/integration` probes stay Windows-only (they read the swap chain and own an HWND); a probe's
  scene moves to `tests/render` as a portable test, not by building the probe.
- `tests/render/scenes.cpp` is one source for both systems: the bootstrap title at three DPIs through the standalone draw path and the
  window caption (band, glyphs, hover and pressed fills, update badge, theme colours checked on any font), on llvmpipe, with
  `GALLIUM_DRIVER=d3d12` on WSL, and on Windows over a hidden window. Text differs between the systems by design (system fonts), so
  scenes check theme colours, counts and repeat-draw identity.
- GPU handles are released with `gpu_release`, never `com_release` (a handle is a COM object on Windows only).

## Atlases

Every growable GPU surface (the identity and preview media atlases, both animation frame caches, the glyph coverage and colour atlases)
is a paged surface (`gpu_paged_texture_*`, `include/maicord/gpu/api.hpp`) under one shelf allocator (`text/glyph_atlas.cpp`): a
virtual height of up to eight fixed pages that shelves never cross. Rows are backed as shelves open (Direct3D 11: 128 or 256-row bands
of 64 KiB tiles of one tiled texture; OpenGL ES: a whole texture per page) and returned at idle trim, so a surface never grows,
compacts or copies, and nothing visible is evicted or moved to make room. What the allocator keeps beyond the visible set is bounded
(`GlyphAtlasPaging::budget_rows`): past it, the least recently used slot that is not visible takes the request. Fixed-size profile media
(effect layers, banners, nameplates) have pools of their own pages (`kMediaAtlasProfilePools`), reused in ring order. Every entry keeps
`kMediaAtlasPadding` clear texels around it, so a page holds pictures up to its size less twice that. The CDN sizes what it can to the
drawn width (profile card images); what it cannot (effect layers, some at about twice their 450x880 slot) the media worker fits at
decode, every animation frame included, with an area-average filter (`media_fit_*`, `app/media/local_preview_thumbnail.hpp`). Uploads
validate sizes before any device call; a refused upload fails its asset alone, which backs off (`media_asset_failures.hpp`), never the
session. Out of virtual space (every page full of visible pictures) an upload is refused and logged (`media_atlas_out_of_virtual_space`).

Quads sample a page at a time where pages are separate textures (`kGpuPagedBindings`, OpenGL ES): media quads carry v in their page's
rows and their page in `RendererMediaDrawStorage::binding_data`; glyph quads keep v in the whole surface and the upload turns it into
their page's rows. Draws split where consecutive quads change page and bind that page's view; Direct3D 11 never splits.

Paged surfaces are never render targets: media atlases are written by compute and copies and cleared through their UAV
(`gpu_clear_uav_rect`), so a remapped tile of the Direct3D 11 tiled texture needs no clear through its new mapping (the rule for
renderable tiled resources, which partial entry clears cannot meet). Unbacked bands map nothing, and an empty surface releases its
pool, which lives until the GPU is done with it; otherwise a pool shrinks only once the GPU has passed the unmaps that freed its last
bands: ResizeTilePool frees memory at once, and shrinking it under queued writes hung an AMD GPU. Newly backed rows are undefined
until written: media allocations are cleared with their padding before a picture lands, and glyph rasters write their gutter.
Under the Direct3D 11 debug layer every compute write through a surface's UAV reports `NULL_TILE_MAPPING_ACCESS_WARNING`: a
`Texture2D` UAV spans the whole texture, unbacked bands included, though no shader or copy touches unbacked rows
(`gpu_paged_texture_backed`). The UI probe's GPU hang reproductions run only on WARP with the debug layer
(`tests/support/warp_device.hpp`), never on the hardware adapter, and allow that warning alone.

The glyph atlas runs on the clock its idle trim check is given; until that check first observes it, what the first screens
allocated counts as allocated then. The trim evicts glyphs nothing used within `kRendererGlyphProtectMs` once
`kRendererGlyphIdleTrimMs` passed without a new glyph, and content text built again from the stale flag replaces the quads of what
it evicted. The title stream (the sign-in screen, the bootstrap title) is retained quads nothing rebuilds, so the trim pins its
glyphs; the caption rebuilds its own when the atlas evicts.

## Background effects

Effects run through the shared pass executor (`d3d11/effect_renderer.cpp`, built into `maicord_effect_host`). A package of `.mcfx`
interface 4 carries a GLSL section (type 10, one entry per DXBC shader: `include/maicord/gpu/glsl_shader.hpp`) next to the DXBC, which
Windows ignores and Linux runs through `gpu_*_shader_create_blob`. `tools/maicord-plugin/mcfx_tool.py effect build --glsl auto|yes|no
[--dxbc-from PACKAGE]` makes it with `tools/glsl_translate.py` (DXC and SPIRV-Cross, pinned in `tools/linux_shader_toolchain.json`; the
client's own shaders are hand-written GLSL, `src/render/shaders/gles`, and use none of it;
`MAICORD_GLSL_TOOLCHAIN=<dir>` names a prebuilt toolchain). MaiCord-plugins' `scripts/build_universal.py` makes the catalog package from the
Windows build, and `plugin_index.py` (`check_source`, `check_universal`) checks it.

`tests/gpu/effects.cpp` (one source for both systems) renders every `samples/effects` effect and Silk Waves (`tests/gpu/effects/*.mcfx`)
for 24 frames, including persistent and multi-pass targets, and compares named frames with PNGs captured from Direct3D 11 by PSNR.
Changing an effect's HLSL, the SDK header or a sample rebuilds `tests/gpu/effects/*.mcfx` (DXBC on Windows, then the GLSL on Linux) and
renews the PNGs with `tools/update_gpu_goldens.py --scene effects`. `MAICORD_GPU_EFFECT_DUMP=<prefix>` writes the raw pictures.

## Lottie stickers

`lottie_program.cpp` builds its masks with the in-house vector geometry engine on both systems
(`include/maicord/render/effects/vector_geometry.hpp`): `vector_geometry.cpp` (figures of cubics or polylines in the arenas of one
`Factory`, rounded rectangles and ellipses, transforms, flattening), `vector_overlay.cpp` (flatten to a tolerance, snap to an integer
grid, split every crossing exactly, a sweep for the winding numbers, rings that keep the original curves between crossings: the four
combinations and the area of a nonzero fill; the last two results stay split, so a chain of merges or masks that takes the last result
as an operand does not build it again) and `vector_stroke.cpp` (caps, miter, bevel and round joins with the miter clip, polygon
outlines, one per figure or separate pieces where an inner join folds). Fills are nonzero everywhere; nothing is reference counted.
The factory a compile releases is kept for the next one with its arenas trimmed to their first 16 KiB, so a sticker's frames do not
reserve and fault in fresh arenas each.
It replaced Direct2D, whose answers the tests keep as their reference.

A change is checked by `maicord_lottie_geometry` (a fixed list of shapes, combinations and strokes: areas and coverage against
`tests/render/effects/golden/lottie_geometry.bin`, what Direct2D answered; and cases with exact answers: shared, collinear and touching
edges, self-crossing, tangent and zero-area outlines, huge and tiny coordinates, chains of combinations) and `maicord_lottie_program`
(the corpus `tests/render/effects/corpus` that `tools/generate_lottie_corpus.py` draws, compiled and compared mask by mask with
`golden/lottie_program.bin`, what the compiler made on Direct2D). Both write nothing and need no GPU. `MAICORD_LOTTIE_PROGRAM_CAPTURE`
set to a path writes the current build's digests instead. The sticker files the client shows are not ours to ship; to compare them
anyway, list their paths in a file (`MAICORD_LOTTIE_CORPUS_LIST`), capture their digests with `MAICORD_LOTTIE_CORPUS_CAPTURE` and compare
another build or system with `MAICORD_LOTTIE_CORPUS_COMPARE`. Not on Linux: the colour-glyph paint programs' tests that use Direct2D
geometry.
