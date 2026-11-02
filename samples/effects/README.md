# Background effect samples

Pixel shaders that draw MaiCord's background. Each sample is one HLSL file written against
`<maicord/sdk/background.hlsli>` (`include/maicord/sdk/background.hlsli`) and a small `.effect` sidecar.

| Sample | Blend | Default | Inputs |
|---|---|---|---|
| `ascii-rain` | screen | 75 %, 30 fps | glyph grid |
| `aurora` | normal | 50 %, 24 fps | blue noise |
| `starfield` | add | 50 %, 24 fps | none |
| `plasma` | normal | 50 %, 24 fps | blue noise |
| `bokeh` | screen | 35 %, 24 fps | none |
| `wave-lines` | screen | 50 %, 24 fps | none |
| `film-grain` | normal | 100 %, 15 fps | noise |

## How an effect draws

MaiCord renders the selected effect once per frame at a share of the window (the resolution) and no more often than
the frame-rate cap. Every canvas (message view, channel list, members) samples that one picture, so panels join up
and nothing draws twice. Widgets and text stay above it. The effect pauses while the window is hidden, minimized,
covered, being resized or idle, and slows down when it is not focused. A GPU budget steps the resolution down, or
turns the effect off, when it costs more than the user allows. A graphics reset rebuilds it.

Colours are the theme's own sRGB values (`mc_role(0..7)`); the output is written as is, as in Shadertoy.

## The sidecar (tests and the packager)

`<name>.effect` sits next to the sample and drives the tests and the `.mcfx` packager, one `key=value` per line:

| Key | Value |
|---|---|
| `name` | The title shown in the picker. |
| `uses` | Space separated: `noise`, `glyphs`, `blue_noise`, `pointer`. Only declared inputs are bound. |
| `blend` | `normal`, `screen` or `add`. |
| `resolution` | 25, 35, 50, 75 or 100 (percent): the effect's request. A user setting wins. |
| `fps` | 10, 15, 24, 30 or 60. |
| `knobs` | Up to 32 default values in thousandths (`350` is 0.35), in `mc_knobs` order. |

The user's per-effect setting wins over the global one, which wins over the request, which wins over 50 % at 24 fps.

## The `.mcfx` package

MaiCord loads effects from `.mcfx` packages: a container with a manifest (name, authors, license, knobs, inputs,
blend, requested resolution and frame rate), the `ps_5_0` bytecode (at most 64 KiB), up to two PNG textures, the
source and a preview. `include/maicord/app/packages/mcfx.hpp` describes the layout, and the client never runs a
package on the CPU: it checks the container and the manifest, then asks D3D11 to build the shader.

## Building and trying one

```
fxc /nologo /T ps_5_0 /E main /O3 /Ges /WX /I include /Fo aurora.cso samples/effects/aurora/aurora.hlsl
```

The packager turns the `.cso` and its annotations into `aurora.mcfx`. Copy it into
`%LOCALAPPDATA%\MaiCord\effects\dev` (or the folder named by the `MAICORD_EFFECT_DEV` environment variable) and
choose the effect in Settings > Appearance > Background effect, or drop it onto Settings > Effects > Install from
link or file. Packages from the catalog live in `%LOCALAPPDATA%\MaiCord\effects`.

The tests compile every sample, draw it at 1440p to time it on the GPU, and draw it at 640x360 for a preview:
`tools/build.ps1 -Test -TestNames maicord_background_theme_probe [-ProbeCaptureDirectory <empty folder>]`.
Each sample's `preview.webp` (still) and `preview-loop.webp` (2.4 s loop, default theme) were encoded from those
captures: `effect-<name>-still.bmp` and 48 `effect-<name>-loop-NN.bmp` frames at 20 fps.

`ascii-rain` is an original rewrite of the matrix-rain idea and is MPL-2.0 like the rest of the samples.
