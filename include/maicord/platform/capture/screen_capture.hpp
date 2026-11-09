#pragma once

// Screen and window capture for Go Live: the sources a stream can show, their picker thumbnails, and the frames of one
// running capture, delivered on the GPU with a timestamp and whether the cursor is in the pixels.
//
// Windows: Windows.Graphics.Capture for windows (every supported version) and for screens where the capture border
// can be turned off (Windows 11); DXGI Desktop Duplication for screens before that, and WGC where duplication is
// refused (a screen on another adapter). Frames are BGRA textures on the caller's device, which must be the
// renderer's multithread-protected device: the consumer's GPU reads are ordered with the capture's copies on its
// immediate context.
// Linux: the xdg-desktop-portal ScreenCast interface over the in-house D-Bus client picks the source (the portal's own
// picker replaces MaiCord's, so capture_sources lists nothing) and opens a PipeWire stream (libpipewire-0.3, dlopen()ed:
// absent means capture is unavailable). Frames are dma-bufs the compositor rendered, never copied by the CPU.

#include "maicord/core/slice.hpp"
#include "maicord/gpu/gpu.hpp"
#if !defined(_WIN32)
#include "maicord/gpu/video_planes.hpp"
#endif

namespace maicord {

enum class CaptureSourceKind : u8 { screen, window };

inline constexpr u32 kCaptureSourceNameBytes = 192;

// One entry of MaiCord's picker (Windows). `handle` is the HMONITOR or HWND; `process_id` owns the window (0 for a
// screen) and is what sound sharing follows. `name` is the window title or "Screen N", UTF-8, cut at a character.
struct CaptureSource {
    u64 handle;
    u32 process_id;
    u32 width, height; // pixels: the monitor's mode, or the window's client area
    CaptureSourceKind kind;
    bool primary; // the primary screen
    u16 name_bytes;
    char name[kCaptureSourceNameBytes];
};

// What this system offers (Linux asks the session bus once: call it from the UI thread). Both false: nothing can
// capture (Linux without the portal or libpipewire).
struct CaptureSupport {
    bool screens, windows;
    bool system_picker; // the system shows its own picker (Linux portal): start with a null source
    bool cursor_control; // the cursor can be left out of the pixels
};
CaptureSupport capture_support() noexcept;

// Screens first (primary first), then windows in z order, MaiCord's own windows left out. The count written.
u32 capture_sources(Slice<CaptureSource> output) noexcept;

// A picker thumbnail of `source`: one frame, reduced on the GPU to fit max_width x max_height and read back as RGBA8
// (opaque, rows of *width * 4 bytes) into `rgba`, which holds max_width * max_height * 4 bytes. Never shows a capture
// border. False when the source is gone or cannot be captured. Any thread; `device` as for capture_start.
bool capture_thumbnail(GpuDevice* device, const CaptureSource& source, u32 max_width, u32 max_height, u8* rgba,
                       u32* width, u32* height) noexcept;

// One captured picture, valid during the sink's frame callback only. The consumer enqueues its GPU reads (the colour
// conversion) before it returns and keeps nothing: after the return the buffer goes back to the system, which may
// write the next picture into it; reads enqueued before that are ordered ahead of the write (Windows: the shared
// immediate context; Linux: the dma-buf's implicit fences).
struct CaptureFrame {
#if defined(_WIN32)
    GpuTexture* texture; // DXGI_FORMAT_B8G8R8A8_UNORM, shader-readable, subresource 0
#else
    // One dma-buf plane of packed 32-bit pixels (DRM fourcc XR24, AR24, XB24 or AB24 in plane.fourcc), with its
    // modifier. The fd stays the capture's.
    GpuDmabufPlane plane;
#endif
    // The picture is the top-left width x height of the buffer (a window that resized may leave the buffer larger
    // for a frame or two).
    u32 width, height;
    // When the picture was composed: microseconds of the client clock (clock_microseconds(clock_initialize(), 0,
    // clock_now())), the clock stream audio is stamped with.
    u64 timestamp_us;
    bool cursor; // the cursor is drawn into the pixels
};

enum class CaptureEnd : u8 {
    stopped,     // capture_stop
    closed,      // the window closed or the screen went away
    denied,      // the user cancelled the system picker, or the system refused capture
    unsupported, // no capture method works for this source and device
    failed,      // the device was lost or the system's capture failed
};

// Callbacks run on the capture's own thread. `ended` is the last call and comes once, except after capture_stop.
// `minimized` (may be null) reports each change of a shared window's minimized state (Windows; a minimized window sends
// no frames, and Discord pauses the stream meanwhile); it is never called for screens or on Linux.
struct CaptureSink {
    void* context;
    void (*frame)(void* context, const CaptureFrame& frame) noexcept;
    void (*ended)(void* context, CaptureEnd reason) noexcept;
    void (*minimized)(void* context, bool minimized) noexcept;
};

struct CaptureOptions {
    GpuDevice* device;     // Windows: the renderer's device. Linux: unused (dma-bufs need none).
    CaptureSource source;  // ignored with a system picker
    u8 kinds;              // system picker: 1 << CaptureSourceKind it may offer
    u32 max_fps;           // frames closer together than 1/max_fps are dropped (0: every frame)
    bool cursor;           // ask for the cursor in the pixels (always there where cursor_control is false)
};

struct ScreenCapture;
// Starts capturing on a thread of its own; frames follow as the source changes (a still screen sends none). Null when
// nothing could start (the reason is logged); otherwise every outcome reaches `ended`.
ScreenCapture* capture_start(const CaptureOptions& options, const CaptureSink& sink) noexcept;
// Joins the thread; no callback runs after it returns. Null is fine.
void capture_stop(ScreenCapture* capture) noexcept;

} // namespace maicord
