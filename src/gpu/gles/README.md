# OpenGL ES backend (`src/gpu/gles`)

The Linux implementation of `include/maicord/gpu/api.hpp`: OpenGL ES 3.1+ on EGL, `libEGL.so.1`, `libGLESv2.so.2` and
`libwayland-egl.so.1` opened with `dlopen`. The renderer above it is the Windows one (`src/render/d3d11`), so the API keeps
D3D11's vocabulary (immediate-mode state, views, constant/instance buffers) and this backend applies that state to GL when a
draw or dispatch happens.

| File | What |
|---|---|
| `loader.cpp`, `internal.hpp` | library loading, entry-point tables (`include/maicord/sys/linux/{egl,gles}.hpp`), handle slabs, state tracker |
| `device.cpp` | display and config choice, context, caps probe, prestart worker |
| `resources.cpp` | buffers, textures, views, samplers, map/update/copy/readback |
| `draw.cpp` | program cache (one link per vertex/pixel pair), resource binding from the shader manifests, draws, dispatches |
| `shader_pack.cpp` | the GLSL pack (`tools/build_gles_shader_pack.py` over the hand-written `src/render/shaders/gles`, one solid zlib stream), shader and compute-program creation |
| `surface.cpp` | the offscreen frame texture, the damage blit into the EGL window, buffer age, the frame clock |
| `sync.cpp` | timer queries, fences |
| `video_*.cpp` | dma-buf EGLImage import and the video frame conversion |

## Rules

- All GL runs on one thread. The prestart worker owns the context until `claim_context()` (called by `gpu_surface_create` and
  `gpu_device_prestart_wait/finish`); never add a GL call that can run on another thread.
- Every pipeline the renderer draws with is warmed on the worker by `d3d11_draw_pipeline_warm`, on a target of
  `gpu_surface_format(nullptr)` (the frame's own format). The warm-up must use the format and state real draws use: a driver
  that compiles on the first draw (d3d12 pipeline states, llvmpipe's LLVM) compiles again on the first frame otherwise.
- The pack order is `ShaderId` order (`MAICORD_SHADER_ORDER`); backend-only variants go after it (`GLES_VARIANTS`). Compute
  programs are shared per pack entry (`GlesComputeProgram`): creating the same shader twice costs one compile. The manifest
  rows a shader is bound by come from its source (`layout(binding = N)` and the `//!` register comments), never from a table.
- Requirements: ES 3.1 with `glCopyImageSubData` (ES 3.2, `GL_OES_copy_image` or `GL_EXT_copy_image`) and
  `GL_EXT_blend_func_extended` (the background-effect composite blends with a second fragment output); device creation refuses a
  driver without either. Every Mesa driver (radeonsi, iris, freedreno, panfrost, llvmpipe, d3d12, zink) and NVIDIA has both.
  `GL_EXT_disjoint_timer_query` is optional (no GPU timing: v3d has none); `EGL_EXT_buffer_age` and swap with damage are
  optional (a full blit and a full-damage swap: Mesa's software Wayland path has neither in 22.3, Debian 12), and swap with
  damage is looked up under both names (Mesa 22.3's Wayland platform has the EXT one, NVIDIA's egl-wayland the KHR one).
- EGL 1.5 on every display: the context is made current without a surface (`EGL_KHR_surfaceless_context`; a window
  display has always needed it, because its window surface comes later). A windowless display's config asks for
  `EGL_SURFACE_TYPE` 0: the default is `EGL_WINDOW_BIT`, which Mesa's surfaceless platform has no config for (that is why
  the 1x1 pbuffer used to be what ran there; it is gone, checked on llvmpipe, d3d12 and headless sway). dma-buf
  import uses `eglCreateImageKHR`, which `EGL_EXT_image_dma_buf_import` requires. Without `EGL_MESA_platform_surfaceless`
  the windowless device takes the default display (`eglGetDisplay`): NVIDIA offers only `EGL_EXT_platform_device` there.
- Compute group limits are capability data (`GpuCaps`): `colr_layers` has a 2-slice variant for devices under 512
  invocations. The GPU VP8 path needs one group of 512 invocations (`vp8_predict.glsl`), above the ES 3.1 floor of 128,
  and 29696 bytes of shared memory (`vp8_filter.glsl`), above the floor of 16384 that v3d has:
  `media_atlas_gpu_vp8_supported` (`render/media/atlas_gpu.hpp`, through `renderer_vp8_gpu`) tests
  `max_compute_invocations` and `max_shared_bytes`, the media pump tells the account module (`media_vp8_cpu`), and on a
  smaller device lossy WebP stills and animation frames decode on the CPU into RGBA8
  (`src/app/media/cdn_media_pipeline.cpp`) without a VP8 shader ever being compiled (`maicord_vp8_cpu_route`).
- The window config is the one without alpha (the compositor reads the buffers as XRGB); a driver with only alpha configs gets
  `EGL_EXT_present_opaque` on the surface instead.
- Test hooks (`test_hook`, test builds only): `MAICORD_GLES_MAX_COMPUTE_INVOCATIONS`, `_NO_TIMER_QUERY`,
  `_ALPHA_WINDOW`, `_CHECK` (a GL error is reported after each draw and clear).
- Tests: `maicord_gpu_*` (goldens captured from Direct3D 11 in `tests/gpu/golden`, renewed by `tools/update_gpu_goldens.py`) run on
  llvmpipe anywhere and with `GALLIUM_DRIVER=d3d12` on WSL; `tools/test_gpu_surface.sh` shows the EGL window path on headless sway.

## Measured decisions (Mesa 25.2.8; llvmpipe, and d3d12 on WSLg with an RX 9070 XT)

Adopted: the warm-up fix and shared compute programs (first-frame CPU, cold caches: llvmpipe 2.2 s to 1.0 s, d3d12 0.7 s to
0.4 s); `EGL_EXT_present_opaque` as the fallback for alpha-only window configs; swap with damage and buffer age (already in).

Not adopted, and why:

- Program-binary cache (`GL_OES_get_program_binary`, `EGL_ANDROID_blob_cache`): Mesa's own disk cache already takes llvmpipe's
  first present from 1.4-3 s to 0.15 s warm, and d3d12's compile is the vendor driver's (`$XDG_CACHE_HOME/AMD/DxcCache`), not
  something a GL program binary holds. A client-side cache would only duplicate both.
- `GL_KHR_parallel_shader_compile`: compile and link are 10% of the worker's time; the cost is at the first draw (pipeline
  state, JIT), which the extension does not move.
- Persistent-mapped rings (`GL_EXT_buffer_storage`): 100 times cheaper than a discard-map per megabyte on d3d12, but the renderer
  uploads 50-250 KB a frame: map and unmap together are 0.7% of the client's samples. The 60% `memmove` the d3d12 profile showed
  was `rep movsb` into write-combined memory, fixed in `src/core/linux/runtime.cpp` (the copy is a vector loop now).
- Rendering straight into the window (no frame texture, no blit): the blit is 6 us of CPU on d3d12 and 1.6 ms of a 25 ms frame on
  llvmpipe; direct rendering would need a Y flip in every vertex shader and a repaint of what a buffer of age 2-3 misses, which
  costs more than the blit it saves.
- `EGL_KHR_partial_update`, `glInvalidateFramebuffer`: the frame texture persists between frames by design.
- `EGL_ANDROID_native_fence_sync`: the fences poll `glClientWaitSync` once a present; no pollable-fd driver to measure on.
- Device choice (`EGL_EXT_platform_device`): the Wayland platform display already follows the compositor's dma-buf feedback
  (Mesa renders on the device the compositor scans out from unless `DRI_PRIME` says otherwise).
- `GL_EXT_base_instance`: one `glBindVertexBuffer` per batch, not measurable.
