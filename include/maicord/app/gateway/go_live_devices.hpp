#pragma once
#include "maicord/app/gateway/go_live_stream.hpp"
#include "maicord/core/win32_records.hpp"
#include "maicord/platform/audio/stream_audio.hpp"

namespace maicord {
// The Go Live producers of a client: screen/window capture and stream audio
// (platform/capture, platform/audio/stream_audio.hpp) and the GPU encoder
// (platform/encode), on the renderer's device. Owned by the account module
// (it outlives Gateway sessions); the session reaches it through the go_live_producer_* functions (go_live_stream.hpp),
// with a pointer to it as the context.
// Zero-initialize.
struct GoLiveDevices {
    SrwLock lock; // device, encoder and media, read by the capture and encoder threads
    GpuDevice* device; // Referenced.
    ScreenCapture* capture;
    StreamAudioCapture audio;
    GpuStreamEncoder* encoder;
    GoLiveMedia* media;
    u32 width, height; // The last picture size reported.
    u32 frames;        // Pictures the capture delivered since it started (capture thread).
    // Borrowed, set by the owner: signaled for every picture the encoder takes (encoded or, while it holds, only
    // converted), so the self-preview shows the new picture at once (the call view's video notification).
    HANDLE picture_event;
    bool audio_running;
};
// Any thread: the renderer's device (null detaches it; a running stream keeps its own).
void go_live_devices_set_device(GoLiveDevices* devices, GpuDevice* device) noexcept;
// After the session stopped every producer.
void go_live_devices_release(GoLiveDevices* devices) noexcept;
} // namespace maicord
