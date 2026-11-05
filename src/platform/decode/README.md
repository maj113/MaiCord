# Video decode on Linux

`linux/` is the live stream decoder behind the unchanged `gpu_stream_decoder.hpp` (queue, decode thread, hold rules,
`stream.hpp`), the movie decoders (`mp4_decoder.cpp`, `h264_file_decoder.cpp`, `hevc_file_decoder.cpp`) and the in-house
AAC-LC decoder. The DXVA files next to it are Windows-only. Backends never call GL: they publish planes
(`include/maicord/gpu/video_planes.hpp`, `src/gpu/gles/video_planes.cpp`) that the renderer realizes as GL textures.

## Tiers

A stream starts on the hardware tier (`vaapi/`) and falls to the software tier (`software/`: `libdav1d.so.7`/`.6` for
AV1, `libopenh264.so.7`/`.6`/`.8` for H.264, 8-bit 4:2:0, CPU planes uploaded as textures) when the probe lists no
hardware codec or the hardware gives up (the profile, AV1 film grain, three failed pictures in a row). No software HEVC;
the live software tier refuses B slices (OpenH264 returns pictures in decode order). `video_capability.hpp` turns the
probes into what Discord is offered. Movies play on both tiers in presentation order (libopenh264 is the reorderer); HEVC
movies need a VA-API driver.

## VA-API (`vaapi/`)

`libva.so.2` and `libva-drm.so.2` are `dlopen`ed (libva 2.14, Ubuntu 22.04, is the floor). `MAICORD_VAAPI=off` disables
the tier; `MAICORD_VAAPI_DEVICE` names the render node. The render node is the one EGL reports for the renderer's device
(`EGL_EXT_device_drm_render_node`), else the first `/dev/dri/renderD*` a display initializes on.

- **Probe** (once, `driver()`): profiles with a VLD entry point, 8-bit 4:2:0, normal slice mode; then one real 64x64
  surface per path (NV12, and P010 for AV1 when the profile lists `VA_RT_FORMAT_YUV420_10`) is exported and its planes
  are imported by the renderer's EGL, modifier included. A driver whose surfaces EGL cannot sample leaves no hardware
  codec. The same surface tells whether `vaSyncSurface2` works.
- **Surfaces**: NV12 (P010 for 10-bit AV1) with `VA_SURFACE_ATTRIB_USAGE_HINT_DECODER | EXPORT`; a driver that refuses the
  hints gets the plain request.
- **Sync**: `picture_end` only submits. `export_picture` waits (`session_sync`: `vaSyncSurface2` with a 1 s timeout when the
  probe found it working, else `vaSyncSurface`), so a picture that is never shown is never waited for, and a hung GPU fails
  the picture instead of the decode thread. `session_destroy` waits for a picture nobody waited for. Reads by GL rely on the
  driver's implicit dma-buf fences after that wait; there is no explicit sync-file use (`DMA_BUF_IOCTL_EXPORT_SYNC_FILE`
  would be wrong on nvidia-vaapi-driver, whose fences are not in the dma-buf, and the CPU wait is already before the export).
- **Export**: `vaExportSurfaceHandle(DRM_PRIME_2, READ_ONLY | SEPARATE_LAYERS)`: one layer per plane (R8 and GR88, R16 and
  GR1616), because the conversion is a compute shader that fetches texels and cannot sample the external-only image a
  composed NV12 layer gives. A layer's second plane is the compression metadata of a tiled modifier (Intel media
  compression): it becomes plane 1 of the EGL image. The modifier goes in with
  `EGL_EXT_image_dma_buf_import_modifiers`; a modifier EGL does not list for the format (or lists as external-only) is
  refused (`gpu_dmabuf_plane_supported`) instead of being read as linear.
- **Kept exports**: every picture is still exported (drivers do work at export, and the export is where a driver may hand out
  a different buffer), but on drivers that hand out one dma-buf per surface (`exports_are_stable`: vendor string
  Mesa Gallium, Intel iHD, Intel i965) the EGL images and GL textures of the previous export of the surface are reused when
  the export names the same dma-buf inodes and layout (`export_identity`, `statx` inode of every plane). Others
  (nvidia-vaapi-driver copies the picture into its buffer at export) are imported again for every picture.
- **10-bit**: AV1 Profile 0 10-bit into P010, refused for BT.2020 matrices (no tone mapping). The shader reads a normalised
  sample times 255 as 8 bits, which is 0.4% under the 10-bit value divided by four: luma and chroma land within one 8-bit
  level and a channel within four after the matrix (`maicord_video_dmabuf`). HEVC Main10 and H.264 High10 are not decoded
  (the shared parsers refuse them).
- **Film grain**: not requested. VA-API applies grain into a separate `current_display_picture` surface on drivers that
  support it, with no capability to query; a stream with grain goes to dav1d.
- **Failures**: a shown picture that decoded, exported and imported clears the failure streak; three in a row hand the
  stream to the software tier.

## Tests

`maicord_video_software` (exact against libavcodec), `maicord_video_stream_software` (decode, plane import, GLES conversion
against a CPU reference), `maicord_vaapi_parameters` (every VA parameter against the DXVA builders and the libavcodec
oracle, bit depth, film grain), `maicord_video_hardware_tier` (tier choice, fallback, a session against a recording libva:
call order, waits and timeouts, surface requests, movie samples; and, over real dma-bufs, the import and the kept exports),
`maicord_video_capability` (capability policy, dma-buf layouts, the EGL attribute list, export identity, driver stability,
the plane pool), `maicord_video_dmabuf` (NV12 and P010 dumb buffers imported, converted and compared with the CPU, a rewrite
of the buffer seen through the same planes, and the per-frame cost of importing again against keeping the images),
`maicord_mp4_decoder_linux`, `maicord_account_video_playback`, `maicord_local_video_preview`, opt-in
`maicord_video_playback_audio`. The dma-buf tests use the dumb buffers of `/dev/dri/card0` (`tests/support/dumb_dmabuf.hpp`:
vgem in a VM or WSL, writable by the `video` group) and skip without it or an EGL that imports. `python
tools/check_video_abi.py` compares `include/maicord/platform/decode/vaapi/abi.hpp` and `software/abi.hpp` with libva-dev,
libdav1d-dev and libopenh264-dev. `tools/generate_video_fixtures.py` and `tools/generate_mp4_fixtures.py` (WSL, libavcodec,
not run by the build) write the fixtures.

No test decodes through a real VA-API driver (vgem has none; Mesa's d3d12 VA driver does not initialize on WSL's vgem node
here). To validate on hardware: decode on Intel (iHD, Gen9 and Gen12+ with media compression), AMD (radeonsi) and NVIDIA
(nvidia-vaapi-driver, `nvidia-drm.modeset=1`), check the log lines at start (the vendor string, `vaapi_codecs`,
`vaapi_features`: bit 0 10-bit, 1 timed sync, 2 stable exports), watch for `dmabuf_modifier_unusable`, and compare a live
stream with the software tier.
