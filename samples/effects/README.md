# Background effects

Pixel shaders that draw MaiCord's background. Each sample is one HLSL file written against
`<maicord/sdk/background.hlsli>` (`include/maicord/sdk/background.hlsli`) and a small `.effect` sidecar. An effect
is **data**, not code: MaiCord never runs anything of it on the CPU, so it installs from a link or a file without the
consent a plugin needs.

| Sample | Blend | Default | Inputs |
|---|---|---|---|
| `ascii-rain` | screen | 75 %, 30 fps | glyph grid |
| `aurora` | normal | 50 %, 24 fps | blue noise |
| `starfield` | add | 50 %, 24 fps | none |
| `plasma` | normal | 50 %, 24 fps | blue noise |
| `bokeh` | screen | 35 %, 24 fps | none |
| `wave-lines` | screen | 50 %, 24 fps | none |
| `film-grain` | normal | 100 %, 15 fps | noise |

## Quick start

You need Windows 10 or 11 x64 and Python 3.9 or newer; no Windows SDK (the tool uses `fxc.exe` when the SDK has it,
and the in-box `d3dcompiler_47.dll` when it doesn't).

```powershell
maicord-plugin effect new "Soft Rain"     # soft-rain\soft-rain.hlsl, soft-rain.effect, README.md
cd soft-rain
maicord-plugin effect build               # build\soft-rain.mcfx, with the lint report
maicord-plugin effect preview --watch     # copy it into MaiCord, again on every save
```

In MaiCord turn on Developer Mode (Settings > Advanced), pick the effect in Settings > Appearance > Background effect
(Settings > Effects > Developer lists what the dev folder holds), and edit the `.hlsl`. Each save compiles in the tool
(a compile error prints there, with file, line and column), copies the `.mcfx` into
`%LOCALAPPDATA%\MaiCord\effects\dev` (or the folder in `%MAICORD_EFFECT_DEV%`), and MaiCord scans the folder again
about a third of a second later: the effect keeps its place as the background and the values of its knobs.

## How an effect draws

MaiCord renders the selected effect once per frame at a share of the window (the resolution) and no more often than
the frame-rate cap. Every canvas (message view, channel list, members) samples that one picture, so panels join up
and nothing draws twice. Widgets and text stay above it. The effect pauses while the window is hidden, minimized,
covered, being resized or idle, and slows down when it is not focused. A GPU budget steps the resolution down, or
turns the effect off, when it costs more than the user allows. A graphics reset rebuilds it.

Colours are the theme's own sRGB values (`mc_role(0..7)`); the output is written as is, as in Shadertoy.

## The shader

An effect is one function; `background.hlsli` supplies `main`, which premultiplies the result:

```hlsl
float4 background(float2 pixel);   // pixel: effect-target pixels, top-left origin, centres at .5
```

What the header gives you (read it: it is the whole interface, 100 lines):

| Name | What |
|---|---|
| `mc.time` | Seconds, times the user's Speed; stops while paused |
| `mc.delta`, `mc.frame` | Seconds since the previous effect frame; effect frames since it was selected |
| `mc.resolution`, `mc.dip` | The target in pixels; target pixels per DIP (DPI scale x resolution scale): size things in DIPs with it |
| `mc.pointer` | The pointer in target pixels, `(-1,-1)` unless the effect uses `pointer` and the user allowed it |
| `mc_role(role)`, `mc.intensity`, `mc.dark` | Theme colours (`MC_ROLE_BACKGROUND/SURFACE/TEXT/MUTED/ACCENT/POSITIVE/WARNING/CRITICAL`); theme strength 0..1; 1 in a dark theme |
| `mc_uv(pixel)`, `mc_mod(x, y)` | `pixel / resolution`; GLSL's `mod` (`fmod` keeps the sign of `x`) |
| `mc_noise(uv)`, `mc_noise_smooth(uv)`, `mc_hash4(p)` | White noise texture (point, bilinear); a texture-free hash |
| `mc_glyph(uv)`, `mc_glyph_cell(code, inside)` | The 16x16 glyph grid of Latin-1 |
| `mc_dither(pixel)` | Blue noise in -0.5/255..0.5/255: add it to a gradient to hide banding |
| `mc_tex0`, `mc_tex1` | Your own PNG textures (at most 512x512, 512 KiB each) |

Only the inputs the effect declares with `@uses` are bound.

### Annotations

Knobs and metadata are comments at the top of the `.hlsl`; the tool reads them, so there is one source of truth:

```hlsl
// @effect   name="Soft Rain" authors="Ada; MaiCord port" license="MPL-2.0" version="1.0.0"   (one line; a port adds
//           derived_from="https://example.com/x|Original Author|CC BY-NC-SA 4.0" to it)
// @knob <id> float <min>..<max> = <default> "<Label>"          slider; add % after the default to show 0.75 as 75 %
// @knob <id> int   <min>..<max> = <default> "<Label>"
// @knob <id> toggle = on|off "<Label>"
// @knob <id> color  = #RRGGBB "<Label>"                        sRGB in the UI and in the shader
// @knob <id> choice "A|B|C" = <index> "<Label>"
// @uses noise | glyphs | blue_noise | pointer | texture0 | texture1     (space separated)
// @blend normal | screen | add                                 the default of the user's Blend choice
```

Limits: a name of at most 48 bytes, knob ids `[a-z][a-z0-9_]*` up to 24 characters, labels up to 48 bytes, at most 16
numbers and toggles, 4 colours and 8 choices of up to 16 entries of 24 bytes. Scalars, toggles and choices take one
float of `mc_knobs`, four to a row; a colour takes the `.rgb` of a row of its own. You write one `#define` per knob;
the tool checks each against the slot it assigns and prints the line to add when one is missing or wrong:

```hlsl
#define knob_speed (mc_knobs[0].x)
#define knob_steps ((int)mc_knobs[0].y)
#define knob_glow  (mc_knobs[0].z != 0)
#define knob_tint  (mc_knobs[1].rgb)
```

### The sidecar

`<slug>.effect` sits next to the sample, one `key=value` per line. `effect build` writes it from the annotations and keeps
the two keys you edit by hand; the catalog's CI checks that the sidecar and the annotations agree.

| Key | Value |
|---|---|
| `name` | The title shown in the picker. |
| `uses` | Space separated: `noise`, `glyphs`, `blue_noise`, `pointer`. Only declared inputs are bound. |
| `blend` | `normal`, `screen` or `add`. |
| `resolution` | 25, 35, 50, 75 or 100 (percent): the effect's request. A user setting wins. Yours to edit. |
| `fps` | 10, 15, 24, 30 or 60. A request too. Yours to edit. |
| `knobs` | The 32 default values in thousandths (`350` is 0.35), in `mc_knobs` order. |

The user's per-effect setting wins over the global one, which wins over the request, which wins over 50 % at 24 fps.

## Building

`maicord-plugin effect build` does what the catalog's CI does, except the cost measurement:

1. reads the annotations and the sidecar, and checks the defines;
2. compiles to `ps_5_0` (`/O3 /Ges /WX`, debug and reflection stripped; at most 64 KiB) with `fxc.exe` (found through
   `MAICORD_FXC_EXECUTABLE`, the Windows SDK, or `PATH`) or the in-box `d3dcompiler_47.dll` (`--compiler` picks one:
   the bytes of the two can differ, and the catalog rebuilds yours from source, with `fxc`, so that doesn't matter);
3. lints the disassembly: only constant buffer `b0`, textures `t0` to `t4` and samplers `s0` to `s2`, no unordered-access
   or 3D resources, at most 4,096 instructions and 64 texture reads;
4. packs the container and writes `build\<slug>.mcfx`.

It prints the instruction and texture-read counts and a guess ("likely light"). The real **cost class** (light,
medium, heavy) is measured in the catalog's CI by rendering 300 frames on WARP, the in-box software rasterizer; an effect
slower than the heavy class is refused, and the picker shows the cost measured on the user's own GPU.

Without `fxc`, errors print as `soft-rain.hlsl(23,19-28): error X3004: undeclared identifier 'x'`.

## The `.mcfx` package

MaiCord loads effects from `.mcfx` packages: a container with a manifest (name, authors, license, knobs, inputs,
blend, requested resolution and frame rate), the `ps_5_0` bytecode (at most 64 KiB), up to two PNG textures, the
source and a preview. `include/maicord/app/packages/mcfx.hpp` describes the layout, and the client never runs a
package on the CPU: it checks the container and the manifest, then asks D3D11 to build the shader.

You can also drop a `.mcfx` onto Settings > Effects > Install from link or file; it appears behind a "not reviewed"
card with a live preview and never updates by itself. Packages from the catalog live in
`%LOCALAPPDATA%\MaiCord\effects`.

## Porting from Shadertoy

| Shadertoy | MaiCord |
|---|---|
| `fragCoord` | `pixel`, with the y axis flipped (top-left origin): a falling effect uses `-p.y` |
| `iTime`, `iResolution` | `mc.time`, `mc.resolution` |
| `iChannel0` (a font), `iChannel1` (noise) | `mc_glyph`, `mc_noise`; `iChannelResolution[1]` is `MC_NOISE_SIZE` |
| `mod`, `fract`, `mix`, `vecN` | `mc_mod`, `frac`, `lerp`, `floatN` |
| a 16 px cell | a knob in DIPs times `mc.dip`, so it looks the same at every DPI and resolution |

A Shadertoy shader without a stated licence is CC BY-NC-SA 3.0 by default: ask the author, or rewrite the idea.
Put the author in `derived_from` (URL, author, licence); the picker shows the credit.

## Publishing to the catalog

Effects go to [maj113/MaiCord-plugins](https://github.com/maj113/MaiCord-plugins) as source:

```
effects/<slug>/
    <slug>.hlsl        written against maicord/sdk/background.hlsli
    <slug>.effect      the sidecar (effect build writes it)
    catalog.json       name, summary, authors, license, categories, version, preview_alt, and optional
                       homepage, derived_from, changelog
    details.md         the page (headings, paragraphs, - lists, code, **bold**, https links)
```

CI compiles with the pinned `fxc`, lints, measures the cost class, renders a still and a short loop on WARP for the
preview, and a maintainer reviews. The licence must allow redistribution and modification (CC BY-NC-SA is fine), and
credit (`authors`, `derived_from`) is shown to users. Bump `version` in `catalog.json` when you change an effect, so
clients notice; installed catalog effects update on their own and keep the values of the knobs whose ids still exist.

## Building and trying the repository's samples

```
fxc /nologo /T ps_5_0 /E main /O3 /Ges /WX /I include /Fo aurora.cso samples/effects/aurora/aurora.hlsl
```

or copy a sample's folder somewhere, run `maicord-plugin effect build` in it (it writes `build\` and rewrites the sidecar), and copy the `.mcfx` into the dev folder.

The tests compile every sample, draw it at 1440p to time it on the GPU, and draw it at 640x360 for a preview:
`tools/build.ps1 -Test -TestNames maicord_background_theme_probe [-ProbeCaptureDirectory <empty folder>]`.
Each sample's `preview.webp` (still) and `preview-loop.webp` (2.4 s loop, default theme) were encoded from those
captures: `effect-<name>-still.bmp` and 48 `effect-<name>-loop-NN.bmp` frames at 20 fps.

`ascii-rain` is an original rewrite of the matrix-rain idea and is MPL-2.0 like the rest of the samples.
