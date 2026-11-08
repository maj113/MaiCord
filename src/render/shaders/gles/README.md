# Hand-written GLSL ES 3.10 of the client's shaders

The Linux client draws and computes with the GLSL here. Each file is the **twin** of one HLSL file of `src/render/shaders`
(`ui.glsl` of `ui.hlsl`), which stays the source of the Windows build and of every number a test compares against.
`tools/build_gles_shader_pack.py` prunes the files for each packed shader (`cmake/generated_sources.cmake`, in `ShaderId`
order), strips comments and whitespace, and packs the texts into one deflate stream (`src/gpu/gles/shader_pack.cpp`
inflates it). No shader compiler runs in a client build. The plugin effects keep HLSL, DXC and SPIRV-Cross
(`tools/glsl_translate.py`): nothing here is theirs.

## Keeping a twin in step

The first line of a twin is `// twin: src/render/shaders/<x>.hlsl <16 hex digits>`, the SHA-256 of the HLSL (line ends as LF).
`tools/check_shader_twins.py` fails (every build, through `tools/check_source_rules.py`) when the HLSL no longer has that hash, naming
the twin. To change a shader:

1. Change the HLSL and the GLSL together. A GLSL line is the HLSL line with the GLSL spellings (below); where the HLSL
   changes behavior, the GLSL changes the same way, bit for bit where a golden can see it.
2. `python tools/build_gles_shader_pack.py --validate --dump DIR /tmp/pack.inc` builds every packed shader and runs
   `glslangValidator` on the source and the minified text; `DIR/<name>.<stage>.glsl` is what the driver compiles.
3. `python tools/check_shader_twins.py --stamp <x>` records the new HLSL hash, once the pair has been read side by side.
4. `MAICORD_UPDATE_DEFLATE_CACHE=1 python tools/build_gles_shader_pack.py <inc>` searches the new pack's deflate stream;
   commit `tools/deflate_cache.txt` (CI is strict).
5. `tools/check_cbuffer_layout.py` (part of every build) compares every uniform block with the HLSL cbuffer of the same name.

## What a file is

One file serves every pack entry its HLSL file has. The builder defines `STAGE_VS`, `STAGE_PS` or `STAGE_CS`,
`ENTRY_<hlsl entry>`, the defines of the `maicord_shader(...)` call (`GLYPH_TILE_CULLING`, `COLR_LAYERS`, `FX_VERTEX`, ...)
and the backend variants (`GLES_VARIANTS` in the builder: `COLR_SLICES=2`), evaluates `#if`, `#ifdef`, `#elif`, `#else`,
`#define NAME [value]` and `#undef` itself, and expands the macros: the text the driver gets has no conditionals. Nothing else
may be a directive except `#extension` (hoisted below `#version 310 es`). Fragment shaders get
`precision highp float; precision highp int;`; samplers and images say `highp` where they are declared.

## Conventions

- **Names.** The HLSL's, minus the HLSL-isms: `rect`, `fill`, `params` for vertex inputs (locations in the HLSL struct
  order, which is the order of the input layout's elements), `v_<name>` for the varyings both stages share, declared once
  above the stage split with `V2F` (`out` in a vertex shader, `in` in a fragment shader): `layout(location = 0) V2F vec2 v_uv;`.
  A fragment output is `out_color`. Names GLSL reserves or hides are not used (`sample`, `active`, `packed`, `step`, `input`,
  `output`, `layout`, `filter`, `distance` as a variable).
- **Vertex shaders** end `gl_Position = vec4(clip.x, -clip.y, -1.0, 1.0)`: the Y flip and the depth fix-up that give Direct3D's
  picture and range, which every shader of the pack and every translated plugin shader agrees on (a draw never sets
  `glClipControl`). A pixel shader's `gl_FragCoord.xy` is `SV_Position.xy` with no further flip. A corner of the quad strip
  is `vec2(gl_VertexID & 1, gl_VertexID >> 1)`.
- **Uniform blocks** are `layout(std140, binding = N)` named after the HLSL cbuffer; `N` is the register in a vertex or
  compute shader and `8 + register` in a pixel shader (the backend binds vertex and compute blocks at 0..7 and pixel blocks
  at 8..15). A cbuffer that both stages of one program read gets the suffixes `Vs` and `Ps` and an instance name
  (`} frame;`): a program holds one global name per block member, and the two stages' members would collide. The layout is the
  HLSL's with the gaps spelled out: a `float3` that sits at a 4-byte offset is three floats, padding is a member whose name
  contains `_unused_`, and members after the last one a shader reads may be left out (the buffer is the host's whole struct).
- **Textures and buffers** carry their manifest row in a `//!` comment: `//! t0 s0` (texture slot 0 with sampler slot 0),
  `//! t0` (read with `texelFetch`, no sampler), `//! t1` on a `readonly buffer` (a shader storage buffer, HLSL `t1`),
  `//! u0` on a `buffer` or an `image2D` (HLSL `u0`). The GL binding is `layout(binding = N)`: storage buffers are
  numbered from 0, shader resource views first, then unordered access views; images from 0; textures at the slot.
  The builder reads these into the manifest the backend binds by; there is no other table.
- **Numerics.** `min`, `max`, `clamp` plain: no NaN guards. A division by a literal that DXC turned into a multiplication
  stays one (`* (1.0 / 255.0)`). `saturate(x)` is `clamp(x, 0.0, 1.0)` (all arguments of one type: `clamp(v, vec2(0.0), w)`).
  `lerp` is `mix`, `frac` is `fract`, a vector `?:` is `mix(a, b, bvec)`, `asfloat` is `uintBitsToFloat`.
- **Barriers** in compute shaders are `memoryBarrierShared(); barrier();` (`memoryBarrierBuffer()` where it is the storage
  buffer that the next pass reads).
- **Paged atlases** (`media_draw.glsl`, `glyph_draw.glsl`): the HLSL samples one tiled texture whose v spans the whole
  virtual atlas; on OpenGL ES each page is its own texture, the renderer splits draws where consecutive quads change
  page, binds that page's view to the slot and gives the quads v in the page's rows. The GLSL is the plain one-sampler
  form.
