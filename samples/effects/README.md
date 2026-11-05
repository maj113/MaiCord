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
| `day-night` | normal | 50 %, 15 fps | blue noise, clock, appearance |
| `spark-ribbon` | normal | 50 %, 24 fps | blue noise; a pass graph: grid, quads, a persistent particle target |

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

Colours are the theme's own sRGB values (`mc_role(0..7)`); the output is written as is, as in Shadertoy. On a light
theme MaiCord adapts an effect that does not handle the appearance itself (see [Light themes](#light-themes)).

## The shader

An effect is one function; `background.hlsli` supplies `main`, which premultiplies the result:

```hlsl
float4 background(float2 pixel);   // pixel: effect-target pixels, top-left origin, centres at .5
```

What the header gives you (read it: it is the whole interface, about 125 lines):

| Name | What |
|---|---|
| `mc.time` | Seconds, times the user's Speed; stops while paused |
| `mc.delta`, `mc.frame` | Seconds since the previous effect frame (0 on the first, and when a paused effect draws again); effect frames since it was selected |
| `mc.resolution`, `mc.dip` | The target in pixels; target pixels per DIP (DPI scale x resolution scale): size things in DIPs with it |
| `mc.pointer` | The pointer in target pixels, `(-1,-1)` unless the effect uses `pointer` and the user allowed it |
| `mc_role(role)`, `mc.intensity`, `mc.dark` | Theme colours (`MC_ROLE_BACKGROUND/SURFACE/TEXT/MUTED/ACCENT/POSITIVE/WARNING/CRITICAL`); theme strength 0..1; 1 in a dark theme (and while MaiCord adapts the effect to a light one) |
| `mc_uv(pixel)`, `mc_mod(x, y)` | `pixel / resolution`; GLSL's `mod` (`fmod` keeps the sign of `x`) |
| `mc_noise(uv)`, `mc_noise_smooth(uv)`, `mc_hash4(p)` | White noise texture (point, bilinear); a texture-free hash |
| `mc_glyph(uv)`, `mc_glyph_cell(code, inside)` | The 16x16 glyph grid of Latin-1 |
| `mc_dither(pixel)` | Blue noise in -0.5/255..0.5/255: add it to a gradient to hide banding |
| `mc_tex0`, `mc_tex1` | Your own PNG textures (at most 512x512, 512 KiB each) |
| `mc_tex2`, `mc_tex3` | Interface 3: two more, PNG or float data |
| `mc_ambient.day`, `.year`, `.month`, `mc_daylight(sunrise, sunset)` | `@uses clock`: the local time of day and of the year (0..1) and the month (1..12); daylight 0..1 between two hours |
| `mc_ambient.dark` | `@uses appearance`: 1 dark, 0 light; glides when it changes |
| `mc_pass`, `mc_target0..3`, `mc_grid_point`, `mc_quad_corner`, `mc_clip` | Interface 3, a pass graph: the pass being drawn, the targets, and host geometry by vertex ID (see [Pass graphs](#pass-graphs)) |

Only the inputs the effect declares with `@uses` are bound; an undeclared `mc_ambient` value reads 0.

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
// @uses noise | glyphs | blue_noise | pointer | texture0 | texture1 | clock | appearance | texture2 | texture3
// @blend normal | screen | add                                 the default of the user's Blend choice
// @target <id> rgba8|rgba16f|rgba32f <W>x<H>|<P>% [persistent]  a pass graph's target (see Pass graphs)
// @pass <ps> [vs=<entry> grid=<W>x<H> | quads=<N>] [to=<id>] [in=<id>,...] [blend=...] [clear]
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

The knob page shows a choice as a row of buttons, each as wide as its label, while they all fit the card, and as a
dropdown when they don't (it decides again as the window changes size). Keep each entry to a word or two ("Clock",
"Day", "Night") so the buttons show at common window sizes.

When the user moves a `float` or `color` knob, MaiCord glides it: each frame the value moves part of the way to the
new one, 95 % in about 0.2 s and settled within about 0.45 s, so a slider drag trails the pointer by about 60 ms of
its motion and a click on the track or a new colour slides instead of jumping. `int`, `toggle` and `choice` knobs
switch at once; an effect that wants such a change gradual eases it itself, as Silk Waves keeps its month and its
day and night in a persistent target and moves them toward the knobs a little each frame (by `mc.delta`, taking a
frame with `mc.delta` 0 whole). Where the window can't show a change moving (as for the ambient values below) every
knob takes it at once.

### The sidecar

`<slug>.effect` sits next to the sample, one `key=value` per line. `effect build` writes it from the annotations and keeps
the two keys you edit by hand; the catalog's CI checks that the sidecar and the annotations agree.

| Key | Value |
|---|---|
| `name` | The title shown in the picker. |
| `uses` | Space separated, in this order: `noise`, `glyphs`, `blue_noise`, `pointer`, `texture0`, `texture1`, `clock`, `appearance`, `texture2`, `texture3`. Only declared inputs are bound. |
| `blend` | `normal`, `screen` or `add`. |
| `resolution` | 25, 35, 50, 75 or 100 (percent): the effect's request. A user setting wins. Yours to edit. |
| `fps` | 10, 15, 24, 30 or 60. A request too. Yours to edit. |
| `knobs` | The 32 default values in thousandths (`350` is 0.35), in `mc_knobs` order. |
| `targets`, `passes` | A pass graph's, as the manifest holds them (compact JSON; see [Pass graphs](#pass-graphs)). |

The user's per-effect setting wins over the global one, which wins over the request, which wins over 50 % at 24 fps.

### Time of day and appearance

An effect can follow the local clock and the light or dark appearance, opting in with `@uses clock` and
`@uses appearance`. The values arrive in `mc_ambient`, a row of the constant buffer after `mc_knobs` (bytes 304 to
319), where an effect that does not use them never reads, so every older effect keeps its layout:

| Value | Range | Source |
|---|---|---|
| `mc_ambient.day` | 0..1 from local midnight (0.5 is noon) | the local clock, read at each whole minute |
| `mc_ambient.year` | 0..1 from 1 January 00:00 | the local date and time (leap years count 366 days) |
| `mc_ambient.month` | 1..12, a whole number | the local month |
| `mc_ambient.dark` | 1 dark, 0 light | MaiCord's theme base and Windows' app mode: **dark wins** |

**Dark wins.** The appearance is dark when MaiCord's theme is (Dark, AMOLED, or a custom theme on a dark base) or
when Windows' app mode is (`AppsUseLightTheme` is 0; MaiCord reads it again when Windows announces
`ImmersiveColorSet`), and light only when both are light. The effect sits under the theme's text: a light picture
under a dark theme would break its contrast, while a light theme's plates keep text readable over a night picture.
So switching Windows to dark turns a light-themed MaiCord's effect to night, and switching MaiCord to Light turns it
to day while Windows is light. `mc.dark` stays what it was: the theme alone, 0 or 1, no gliding.

A theme that changes with the time of day (its `variants`) decides alone: it turns light by day and dark at night,
and the appearance follows its base whatever Windows says, so an effect shows its day under the light phases. The
base changes with the theme's own 1.5 s glide, beside this one.

**A theme following your knobs.** Such a theme can name knobs of the effect it asks for (its `background.effect`)
that hold it on a month or a phase, so the theme and the sky agree when the user overrides them:

```json
"background": {"effect": "silk-waves",
               "follow": {"month": "month", "phase": "sky", "phases": ["auto", "auto", "day", "night"]}}
```

`month` is a choice knob of 13 options: the first follows the date, the others hold January to December (the
month's colours, without the turn toward the next). `phase` is a choice knob, and `phases` says for each of its
options, in order, what the theme does: `"auto"` keeps the theme's own schedule, `"night"`, `"dawn"`, `"day"` or
`"dusk"` holds that phase whole, its base included. The theme reads the knobs' current values, the user's overrides
included, only while that effect draws as the window's background (the saved choice or the theme's own); otherwise,
or with every knob on an `"auto"` option, it keeps its schedule. Changing such a knob glides the theme over 0.6 s, as
any theme change the user makes does. The effect needs nothing new: the theme names the knobs, so any effect with
such choices works, and a MaiCord before 0.6.5 skips `follow` and keeps the schedule.

An effect that declares `appearance` has a light day picture of its own, so it should never draw that picture under
a dark appearance's text unless the user chose so: by default, show the night whenever `mc_ambient.dark` is 1 (Silk
Waves does when it follows the clock, `day-night` with its default "Either"), and leave a clock-only or always-day
look to an explicit choice (Silk Waves' "Day", `day-night`'s "Clock").

**Gliding.** MaiCord eases each value toward its source over 1.5 s on a smoothstep, so a shader blends with the
value directly. A change of appearance glides, and so does a jump of the clock (waking from sleep, a new time zone,
a simulated time); the clock moving on its own minute by minute is followed as it is, and the time of day wraps at
midnight the short way round. While a value glides MaiCord draws frames at the effect's frame rate even if the
effect is otherwise paused or idle; once it settles it asks for none. A running effect draws a change on its next
frame, so every frame it draws has a `mc.delta`; a paused one draws it at once with `mc.delta` 0. Where the change
cannot be watched moving (the window hidden, minimized, covered by Settings or not focused, unless the effects keep
moving unfocused, or reduced motion on) it takes effect at once. The month does not glide.

```hlsl
// @uses clock appearance
float4 background(float2 pixel) {
    const float night = max(1.0 - mc_daylight(6.5, 19.0), mc_ambient.dark); // the clock's night or a dark appearance
    const float3 day_sky = float3(0.45, 0.66, 0.92), night_sky = float3(0.02, 0.03, 0.08);
    return float4(lerp(day_sky, night_sky, night) + mc_dither(pixel), 1.0);
}
```

The `day-night` sample does this with a horizon glow at dawn and dusk, a seasonal tint from `mc_ambient.year`, stars and
clouds, and a knob to follow the clock, the appearance, or either.

**Older MaiCord.** An effect that uses either input is packed as manifest `"interface": 2`; every other effect stays
`1` and runs everywhere. A MaiCord from before interface 2 refuses such a package when it reads it (its manifest names
an input that MaiCord doesn't have) instead of drawing it with the inputs missing, so the effect doesn't appear in its
library. The catalog's index lists only the interface-1 inputs of an effect, as an older index reader refuses any
other name; `clock` and `appearance` live in the package's manifest. The catalog lists such an effect, with its
`"interface"`, in `index-3.json` (MaiCord 0.6.4 and later) and `index-full.json` (0.6.3), whose readers show an
effect above their own interface as needing a newer MaiCord; `index.json`, which 0.6.0 to 0.6.2 read, leaves it out.

**Linux.** The Linux client runs GLSL, not DXBC. A package of manifest `"interface": 4` carries both (a GLSL section next
to the DXBC, one file for both systems), and the catalog serves that file to the Linux client in `index-4.json` while
`index-3.json` and the older indexes keep the DXBC package of the same version for Windows. The catalog's build makes
the interface 4 package from the one you submit (scripts/build_universal.py in MaiCord-plugins), so a submission is
still one HLSL file. `maicord-plugin effect build` adds the section itself when the shader toolchain is there (see
`--glsl`); without it the package runs on Windows only, and a Linux client reports that it has no payload for it.

**Previewing.** `maicord-plugin effect preview` can stand in for both inputs while you work, for every effect, in
Developer Mode:

```powershell
maicord-plugin effect preview --time 18:30 --appearance light   # dusk, light
maicord-plugin effect preview --appearance dark                 # glides to dark; the time stays 18:30
maicord-plugin effect preview --day-seconds 60 --date 12-21     # a whole winter day every minute, from 18:30
maicord-plugin effect preview --real-inputs                     # back to the real clock and appearance
```

The options merge into `simulate-inputs.txt` in the dev folder (`time=HH:MM`, `date=MM-DD`, `appearance=dark|light`,
`day_seconds=N`), which MaiCord reads when the folder changes; a change glides as a real one does.

### Light themes

Most effects are light on a dark background: stars, glows, rain. On a light theme that light vanishes (white stars
on a white page), so MaiCord adapts every effect that does not declare `@uses appearance`, when the appearance is
light (MaiCord's theme and Windows both light: [dark wins](#time-of-day-and-appearance)):

- The effect draws for the theme's dark twin: each `mc_role` colour has its luma flipped and `mc.dark` reads 1.
- MaiCord flips the picture's luma back as it composites it (no extra pass). The flip is BT.709 luma
  `y -> 1 - y` with each channel's distance from the luma kept, so it keeps hue and chroma and undoes itself: the
  theme's own colours come back as they were, and the effect's own light turns to shade. White stars become dark
  specks, glows become soft shadows, and a blue glow becomes a blue shade.
- `normal` composites the flipped colour. `screen` and `add` can't add light to a white page, so they multiply the
  background by the flipped light instead, which takes light away in the light's own hue.
- The adaptation glides with the appearance over the same 1.5 s, so switching themes cross-fades.

Nothing changes in the format: an existing effect needs no rebuild, and an older MaiCord draws it as before. Users
choose **Light themes: Auto or Off** in the effects' performance defaults, and per effect on its page (the effect's
own choice wins). `maicord-plugin effect preview --appearance light` shows the adaptation in the dev folder.

**Opting out.** An effect that declares `@uses appearance` is never transformed: it reads `mc_ambient.dark` and draws
its own light picture, as `day-night` does with its daytime sky. Declare it when the effect has a real day look;
leave it out to get the adaptation for free.

### Pass graphs

One full-screen function can't draw everything: a mesh that folds over itself, particles that remember where they
were, a glow from a blurred copy. An interface-3 effect can be a **pass graph**: up to six passes, each a pixel
shader drawn over its whole target, or a vertex shader over geometry MaiCord makes, into the effect's output or into
one of up to four small targets of its own. Effects stay data: the package ships no vertex data or code for the CPU,
only shaders, and every count and size below is checked when MaiCord reads the package.

```hlsl
// @target parts rgba32f 32x32 persistent        a target: its format and a fixed size, or a share of the output
// @target glow  rgba16f 50%
// @pass simulate to=parts in=parts              shader 0: reads the last frame's state and writes the next
// @pass ribbon vs=ribbon_vs grid=96x24 to=glow blend=add clear
// @pass compose in=glow                         to=out (the effect's output) unless `to` says otherwise
// @pass spark vs=spark_vs quads=1024 in=parts blend=add
#define MC_GRAPH                                  // before the include: no main(); each pass names its entry points
#include <maicord/sdk/background.hlsli>
```

- **Targets**: `rgba8`, `rgba16f` or `rgba32f`, either a fixed `<W>x<H>` up to 512x512 or `<P>%` (1 to 100) of the
  output on both axes. Fixed targets may take 8 MiB together and targets sized by a share 16 bytes per output pixel
  (a persistent target counts twice). A pass reads the targets it lists in `in=` as `mc_target0`..`mc_target3`
  (`t7`..`t10`, in `@target` order) with `Load` or a sampler, in vertex and pixel shaders alike; the output can't be
  read.
- **Persistent targets** keep their contents from frame to frame: a pass that writes one and lists it in `in=` reads
  the last frame's state and writes the next (MaiCord keeps two planes and swaps them). The state never leaves the
  GPU, and starts over (all zero) when the effect is selected, after a graphics reset and when the target's size
  changes, so a texel that reads zero is one to initialise. That is how `spark-ribbon` simulates its sparks: one
  texel each, position, age and "alive".
- **Geometry**: `grid=<W>x<H>` (2 to 256 points each way) draws `H - 1` instanced triangle strips of `2W` vertices;
  `mc_grid_point(SV_VertexID, SV_InstanceID)` is the vertex's point of the grid. `quads=<N>` (up to 16,384) draws `N`
  instanced four-vertex strips; `mc_quad_corner(SV_VertexID)` is the corner. `mc_clip(pixel)` turns a position in
  the pass's target pixels into `SV_Position`. The vertex shader passes whatever it likes on to the pixel shader.
- **Blends** per pass: `opaque` (the default), `alpha`, `add` or `premultiplied`; `clear` clears the target to
  transparent black first. Whatever reaches the output is premultiplied colour, as `main()` writes it in a
  single-pass effect, and the user's Blend composites it.
- **`mc_pass`**: `.size` is the pass's target in pixels, `.count` the grid's points or the quad count and 1.
  `mc.resolution` is still the output's.
- **Cost**: the whole graph is one effect frame. The GPU budget times every pass together, the resolution and
  frame rate apply to the output (a target sized by a share follows them), and the graph pauses as an effect does.

**Textures.** Interface 3 adds `@uses texture2 texture3` (`mc_tex2`, `mc_tex3`), and any of the four may be float
data instead of a PNG: `texture<N>.mcft` beside the HLSL holds `MCFT`, the width and height as little-endian 16-bit
numbers (1 to 512), then width x height RGBA32F texels, at most 512 KiB as for a PNG. The tool packs
`texture<N>.png` or `texture<N>.mcft` for each declared texture; `mcfx_tool.float_texture(width, height, texels)`
writes the second.

**Older MaiCord.** A graph, `texture2`, `texture3` or float data make the package `"interface": 3`. A MaiCord before
interface 3 refuses such a package when it reads it, and a single-pass effect without them keeps interface 1 or 2 and
runs exactly as it did. As with interface 2, the catalog lists such an effect only for MaiCord 0.6.3 and later.

## Building

`maicord-plugin effect build` does what the catalog's CI does, except the cost measurement:

1. reads the annotations and the sidecar, and checks the defines and the graph's limits;
2. compiles to `ps_5_0` (`/O3 /Ges /WX`, debug and reflection stripped; at most 64 KiB) with `fxc.exe` (found through
   `MAICORD_FXC_EXECUTABLE`, the Windows SDK, or `PATH`) or the in-box `d3dcompiler_47.dll` (`--compiler` picks one:
   the bytes of the two can differ, and the catalog rebuilds yours from source, with `fxc`, so that doesn't matter);
   a graph compiles each pass's entry points, to `ps_5_0` and `vs_5_0`;
3. lints each shader's disassembly: only constant buffer `b0`, textures `t0` to `t4` (`t10` under interface 3) and
   samplers `s0` to `s2`, no unordered-access or 3D resources, at most 4,096 instructions and 64 texture reads;
4. packs the container (manifest `"interface": 3` for a graph or its textures, 2 when the effect uses `clock` or
   `appearance`, else 1) and writes `build\<slug>.mcfx`.

It prints the instruction and texture-read counts and a guess ("likely light"). The real **cost class** (light,
medium, heavy) is measured in the catalog's CI by rendering 300 frames on WARP, the in-box software rasterizer; an effect
slower than the heavy class is refused, and the picker shows the cost measured on the user's own GPU.

Without `fxc`, errors print as `soft-rain.hlsl(23,19-28): error X3004: undeclared identifier 'x'`.

## The `.mcfx` package

MaiCord loads effects from `.mcfx` packages: a container with a manifest (name, authors, license, knobs, inputs,
blend, requested resolution and frame rate, and a graph's targets and passes), the `ps_5_0` bytecode (at most 64
KiB; a graph's other shaders follow in a SHADERS section, each a whole container of at most 64 KiB), up to four
textures, the source and a preview. `include/maicord/app/packages/mcfx.hpp` describes the layout, and the client never
runs a package on the CPU: it checks the container and the manifest, then asks D3D11 to build the shaders.

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
preview (an effect with `clock` or `appearance` is rendered at noon on 21 June in the dark appearance), and a
maintainer reviews. The licence must allow redistribution and modification (CC BY-NC-SA is fine), and
credit (`authors`, `derived_from`) is shown to users. Bump `version` in `catalog.json` when you change an effect, so
clients notice; installed catalog effects update on their own and keep the values of the knobs whose ids still exist.

## Building and trying the repository's samples

```
fxc /nologo /T ps_5_0 /E main /O3 /Ges /WX /I include /Fo aurora.cso samples/effects/aurora/aurora.hlsl
fxc /nologo /T vs_5_0 /E spark_vs /O3 /Ges /WX /I include /Fo spark_vs.cso samples/effects/spark-ribbon/spark-ribbon.hlsl
```

or copy a sample's folder somewhere, run `maicord-plugin effect build` in it (it writes `build\` and rewrites the sidecar), and copy the `.mcfx` into the dev folder.

The tests compile every sample, draw it at 1440p to time it on the GPU, and draw it at 640x360 for a preview:
`tools/build.ps1 -Test -TestNames maicord_background_theme_probe [-ProbeCaptureDirectory <empty folder>]`.
Each sample's `preview.webp` (still) and `preview-loop.webp` (2.4 s loop, default theme) were encoded from those
captures: `effect-<name>-still.bmp` and 48 `effect-<name>-loop-NN.bmp` frames at 20 fps (`day-night` at 19:15 on
21 June in the dark appearance). The probe also drives `day-night`'s inputs as MaiCord eases them, with the effect's
clock held: `effect-day-night-hour-NN.bmp` is each hour of 21 June in the light appearance, and
`effect-day-night-transition-NN.bmp` is Windows turning dark at noon, one frame every 50 ms (30 frames move, then the
picture holds). It checks the graph pass against `tests/shaders/background_effect_graph.hlsl`, a graph of every
feature with known pixels (`effect-graph-test-frame-03.bmp`), and `maicord_packages` parses `spark-ribbon` as
`maicord-plugin` packs it. For light themes it checks the composite against its model for each blend, then draws every
sample on Light and on a light custom theme with the light row Off and Auto (`effect-<name>-light-before.bmp`,
`-light-after.bmp`, `-custom-before.bmp`, `-custom-after.bmp`) and logs what the adapted composite costs.

`ascii-rain` is an original rewrite of the matrix-rain idea and is MPL-2.0 like the rest of the samples.
