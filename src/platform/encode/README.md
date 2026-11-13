# Go Live encoding

`include/maicord/platform/encode/gpu_stream_encoder.hpp` turns captured pictures (`platform/capture/screen_capture.hpp`)
into the access units the stream sender packetizes. Everything stays on the GPU, and there is no software encoder: a GPU
without hardware H.264 cannot stream.

- **Tiers:** `gpu_stream_encoder_codecs` probes the hardware once per device. `gpu_stream_encoder_pick` takes the
  best of AV1, H.265 and H.264 that the stream also accepts (Discord's order).
- **Windows** (`gpu_stream_encoder.cpp`):
  - **Conversion:** `bgra_nv12.hlsl` scales and converts the BGRA capture into an NV12 surface (BT.709 limited range;
    a box of bilinear taps when it reduces). It draws twice (luma, chroma) on the shared immediate context, in a
    context state of its own, so the renderer's bindings never change. Its two shaders (one pixel shader serves
    both planes) are entries of the client's shader pack (`ShaderId::encode_*`, Windows only), inflated when the
    encoder opens.
  - **Encoding:** the GPU vendor's asynchronous hardware MFT, found with `MFTEnum2(MFT_ENUM_FLAG_HARDWARE)` on the
    capture device's adapter and fed through the DXGI device manager. The encoder thread serves its events.
  - **Settings:** CBR with low latency and no B-frames. Keyframes come on request through
    `CODECAPI_AVEncVideoForceKeyFrame`. A new extent reopens the transform. AMD's AV1 transform runs at its speed
    preset (`CODECAPI_AVEncCommonQualityVsSpeed` 0; its default is 70): at 1440p60 and 9 Mb/s it uses the video
    engine 14 % of the time instead of 36 %, at the same PSNR and size (`maicord_go_live_gpu_probe`, desktop and
    panning content). The H.265 and H.264 transforms keep their defaults: their faster presets lose picture quality.
- **Linux** (`linux/gpu_stream_encoder.cpp`):
  - **Display:** the hardware decoders' VA display (`src/platform/decode/vaapi`).
  - **Conversion:** the capture's dma-buf is imported as an RGB surface, and the driver's video processing scales and
    converts it to NV12.
  - **Encoding:** EncSlice, or the low-power EncSliceLP, at CBR with an IDR (key frame) on request: AV1 Profile 0,
    H.265 Main, and H.264 (Constrained Baseline, else Main, else High), as the display reports them.
  - **Headers:** for H.264 and H.265 the driver writes the parameter sets and slice headers. AV1 takes the sequence and
    frame header OBUs as packed headers from `av1_headers.cpp`, which also gives the bit positions of the quantizer,
    loop filter and CDEF fields the driver rewrites after rate control (one tile; every picture refreshes every slot
    and references the one before it).
  - **H.265 extent:** the driver's SPS has no conformance window, so H.265 extents are rounded down to multiples of 8.
- **Output** (`access_unit.cpp`, shared):
  - **H.264 and H.265:** Annex B with 4-byte start codes and no delimiters. Every keyframe leads with its parameter
    sets, repeated from the encoder's configuration when the hardware leaves them out.
  - **AV1:** every OBU has `obu_size`, with no temporal delimiters or padding. Keyframes lead with the sequence header.
- **Still screens:** a capture sends no pictures while its screen is still. A keyframe request (PLI/FIR) that comes
  after 100 ms (or a frame interval) without a picture encodes the newest picture again. This, the pending-picture
  queue, the settings check and the bitrate, keyframe and statistics calls are `gpu_stream_encoder_shared.inc`, which
  both systems' encoders include.
- **Unwatched streams:** `gpu_stream_encoder_hold` keeps taking and converting pictures (the self-preview stays
  current) but encodes none. The stream session holds the encoder while the stream has no viewers, as Discord encodes
  only while someone watches; the release encodes the newest picture as a keyframe at once.
- **Self-preview:** `gpu_stream_encoder_preview` returns the newest converted picture with a sequence that changes with
  every new picture. The call view presents a preview only when its sequence differs from the one it showed (as for a
  remote stream), so a preview without a changing sequence freezes on its first picture.
- **Tests:**
  - `maicord_stream_encoder_gpu_h264`, `_h265` and `_av1` (Windows, one process per hardware codec, side by side; a
    codec the GPU lacks passes with nothing to do) and `maicord_stream_encoder_vaapi` (Linux, status 77 without a
    VA-API encoder) decode the output with MaiCord's own decoders and require 40 dB PSNR on every picture. Each
    `maicord_stream_encoder_gpu_*` also runs a continuous stream (24 pictures, one per frame interval, submitted
    while earlier ones are still in the encoder, in four segments) and requires units, and new preview pictures,
    throughout. The first codec the GPU encodes also checks a held encoder: it takes a picture but encodes nothing,
    and the release encodes that picture as a keyframe.
  - `maicord_go_live_pipeline_probe` (Windows, opt-in `MAICORD_CAPTURE_PROBE=1`): the real primary screen into the real
    encoder for six seconds, printing counts per second only (capture pictures, encoder submissions and drops, units,
    preview sequence).
  - `maicord_go_live_gpu_probe` (Windows, opt-in `MAICORD_GO_LIVE_GPU_PROBE=<seconds>`): the encoder's GPU cost per
    picture from a synthetic 1440p60 desktop or panning texture (or, with `MAICORD_CAPTURE_PROBE=1`, the real screen):
    the conversion and self-preview conversion timed with D3D11 timestamp queries, the units' latency and sizes, and
    with `MAICORD_GO_LIVE_GPU_PSNR=1` the stream's PSNR through MaiCord's own decoder. The video engine's share is read
    beside it from the process's GPU Engine counters. The UI probe's `MAICORD_UI_GO_LIVE_GPU_PROBE=1` times the call
    view's side: a new 1440p self-preview picture converted into the preview atlas and the damage repainted, 60 times a
    second.
  - `maicord_stream_encoder_units` covers the presets, extents, tiers and unit rewriting, and parses the AV1 headers
    of many configurations with MaiCord's own AV1 parser (every field and patch position must round-trip).
