#pragma once

// The PipeWire half of Linux screen capture: libpipewire-0.3 loaded on first use, the format negotiation PODs, and a
// consumer stream that receives the compositor's dma-bufs on PipeWire's loop thread and hands each to a sink as a
// CaptureFrame. The portal half (src/platform/capture/linux/screen_capture.cpp) finds the node and the remote's fd.

#include "maicord/platform/capture/pipewire/abi.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/platform/capture/screen_capture.hpp"

namespace maicord::pipewire {

// libpipewire, loaded on first use from any thread (pw_init done). Null when it is missing or lacks a function (the
// reason is logged once).
const Api* api() noexcept;

// What the stream settled on (SPA_PARAM_Format).
struct VideoFormat {
    u32 video_format;  // SPA_VIDEO_FORMAT_*
    u64 modifier;      // kDrmFormatModifierInvalid when the format carries none
    u32 width, height;
    bool modifier_choice; // the producer left the modifier for the consumer to fixate
};

// The formats the consumer accepts (packed 32-bit RGB as dma-bufs: BGRx, BGRA, RGBx, RGBA) as SPA_PARAM_EnumFormat
// objects; `modifier` fixes the modifier where `fixed`, else offers the linear and implicit layouts for the producer to
// choose. Writes them into `storage` and their addresses into `pods`; returns the count (0 when the storage is short).
u32 build_enum_formats(u8* storage, u32 capacity, const Pod** pods, u32 max_fps, bool fixed, u32 video_format,
                       u64 modifier) noexcept;
// The stream audio capture's format (SPA_PARAM_EnumFormat): 48 kHz stereo (FL, FR) interleaved S16LE. False when the
// storage is too small.
bool build_audio_format(u8* storage, u32 capacity, const Pod** pod) noexcept;
// SPA_PARAM_Buffers (dma-bufs only) and SPA_PARAM_Meta (the header, for the timestamp): two pods.
u32 build_buffer_params(u8* storage, u32 capacity, const Pod** pods) noexcept;
// Reads a SPA_PARAM_Format object; false for anything that is not one of the accepted formats.
bool parse_video_format(const Pod* pod, VideoFormat* format) noexcept;
// The DRM fourcc of an accepted SPA video format (XR24, AR24, XB24, AB24), 0 otherwise.
u32 drm_fourcc(u32 video_format) noexcept;

enum class StreamEnd : u8 { none, closed, failed, unsupported };
struct Consumer;
// Connects to the PipeWire remote `fd` (owned by the consumer from here on; -1 connects to the user's daemon) and
// consumes node `node`. Frames reach `sink.frame` on PipeWire's thread; when the stream ends, `ended` is set once.
// `cursor` is whether the producer draws the cursor into the pixels (it is copied into each frame).
Consumer* consumer_open(int fd, u32 node, bool cursor, u32 max_fps, const CaptureSink& sink, Waitable ended) noexcept;
// Stops PipeWire's thread; no frame is delivered after it returns. Null is fine.
void consumer_close(Consumer* consumer) noexcept;
// Why the stream ended (none while it runs).
StreamEnd consumer_end(const Consumer* consumer) noexcept;
// Whether a format settled with the node (the stream is linked to it and negotiated).
bool consumer_linked(const Consumer* consumer) noexcept;

} // namespace maicord::pipewire
