#include "maicord/app/gateway/go_live_devices.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/os/wait.hpp"
#include "maicord/gpu/api.hpp"

namespace maicord {
namespace {
GoLiveDevices& of(void* context) noexcept {
    return *static_cast<GoLiveDevices*>(context);
}

// Capture thread.
void frame(void* context, const CaptureFrame& picture) noexcept {
    auto& d = of(context);
    ++d.frames;
    if (picture.width != d.width || picture.height != d.height) {
        d.width = picture.width;
        d.height = picture.height;
        go_live_media_capture_size(d.media, picture.width, picture.height);
    }
    // The shared lock keeps the encoder alive through the submit.
    AcquireSRWLockShared(d.lock.native());
    if (d.encoder)
        gpu_stream_encoder_submit(d.encoder, picture);
    ReleaseSRWLockShared(d.lock.native());
}
void capture_ended(void* context, CaptureEnd reason) noexcept {
    auto& d = of(context);
    switch (reason) {
    case CaptureEnd::stopped:
        return;
    case CaptureEnd::closed:
        go_live_media_ended(d.media, GoLiveEnd::source_closed);
        return;
    case CaptureEnd::denied:
        go_live_media_ended(d.media, GoLiveEnd::capture_denied);
        return;
    default:
        go_live_media_ended(d.media, GoLiveEnd::capture_unavailable);
        return;
    }
}
void minimized(void* context, bool value) noexcept {
    go_live_media_minimized(of(context).media, value);
}
// Encoder thread.
void unit(void* context, const GpuStreamAccessUnit& encoded) noexcept {
    auto& d = of(context);
    go_live_media_unit(d.media, encoded);
    if (d.picture_event)
        event_set(d.picture_event);
}
void encoder_ended(void* context, GpuStreamEncoderEnd) noexcept {
    go_live_media_ended(of(context).media, GoLiveEnd::encoder_failed);
}
// Stream audio thread.
void audio_block(void* context, ConstSlice<i16> stereo, u64) noexcept {
    go_live_media_audio(of(context).media, stereo);
}

u32 encodable(void* context, bool* known) noexcept {
    auto& d = of(context);
    AcquireSRWLockShared(d.lock.native());
    GpuDevice* device = d.device;
    if (device)
        gpu_retain(device);
    ReleaseSRWLockShared(d.lock.native());
    *known = device != nullptr;
    if (!device)
        return 0;
    const u32 codecs = gpu_stream_encoder_codecs(device);
    gpu_release(device);
    return codecs;
}
bool audio_available(void*) noexcept {
    return stream_audio_available(stream_audio_method());
}
bool capture(void* context, GoLiveMedia* media, const CaptureSource& source, bool system_picker,
             u32 max_fps) noexcept {
    auto& d = of(context);
    AcquireSRWLockExclusive(d.lock.native());
    GpuDevice* device = d.device;
    d.media = media;
    ReleaseSRWLockExclusive(d.lock.native());
    d.width = d.height = 0;
    d.frames = 0;
    CaptureOptions options{};
    options.device = device;
    options.source = source;
    options.kinds = system_picker ? static_cast<u8>(1u << static_cast<u8>(source.kind)) : 0;
    options.max_fps = max_fps;
    options.cursor = true;
    d.capture = capture_start(options, {&d, frame, capture_ended, minimized});
    return d.capture != nullptr;
}
void capture_stop_all(void* context) noexcept {
    auto& d = of(context);
    if (!d.capture)
        return;
    capture_stop(d.capture);
    d.capture = nullptr;
    // Counts only, to see where pictures stop between capture, encoder and the wire.
    log_u64(LogLevel::info, "go_live", "capture_frames", d.frames);
}
bool sound(void* context, GoLiveMedia* media, u32 process_id) noexcept {
    auto& d = of(context);
    d.media = media;
    if (!stream_audio_available(stream_audio_method()))
        return false; // Stream audio never includes MaiCord's own output; without a way to leave it out, none.
    const StreamAudioOptions options{process_id ? StreamAudioScope::application : StreamAudioScope::everything,
                                     process_id};
    d.audio_running = stream_audio_start(&d.audio, options, {&d, audio_block});
    return d.audio_running;
}
void sound_stop(void* context) noexcept {
    auto& d = of(context);
    if (d.audio_running)
        stream_audio_stop(&d.audio);
    d.audio_running = false;
}
bool encoder(void* context, GoLiveMedia* media, GpuStreamCodec codec, const GpuStreamEncoderSettings& settings) noexcept {
    auto& d = of(context);
    AcquireSRWLockShared(d.lock.native());
    GpuDevice* device = d.device;
    if (device)
        gpu_retain(device);
    ReleaseSRWLockShared(d.lock.native());
    if (!device)
        return false;
    d.media = media;
    GpuStreamEncoder* created = gpu_stream_encoder_create({device, codec, settings, {&d, unit, encoder_ended}});
    gpu_release(device); // The encoder keeps its own reference.
    if (!created)
        return false;
    AcquireSRWLockExclusive(d.lock.native());
    GpuStreamEncoder* previous = d.encoder;
    d.encoder = created;
    ReleaseSRWLockExclusive(d.lock.native());
    gpu_stream_encoder_destroy(previous);
    return true;
}
void configure(void* context, const GpuStreamEncoderSettings& settings) noexcept {
    auto& d = of(context);
    AcquireSRWLockShared(d.lock.native());
    if (d.encoder)
        gpu_stream_encoder_configure(d.encoder, settings);
    ReleaseSRWLockShared(d.lock.native());
}
void bitrate(void* context, u32 bits_per_second) noexcept {
    auto& d = of(context);
    AcquireSRWLockShared(d.lock.native());
    if (d.encoder)
        gpu_stream_encoder_set_bitrate(d.encoder, bits_per_second);
    ReleaseSRWLockShared(d.lock.native());
}
void keyframe(void* context) noexcept {
    auto& d = of(context);
    AcquireSRWLockShared(d.lock.native());
    if (d.encoder)
        gpu_stream_encoder_request_keyframe(d.encoder);
    ReleaseSRWLockShared(d.lock.native());
}
void encoder_stop(void* context) noexcept {
    auto& d = of(context);
    AcquireSRWLockExclusive(d.lock.native());
    GpuStreamEncoder* encoder = d.encoder;
    d.encoder = nullptr;
    ReleaseSRWLockExclusive(d.lock.native());
    if (encoder) {
        const auto stats = gpu_stream_encoder_stats(encoder);
        log_u64(LogLevel::info, "go_live", "encoder_submitted", stats.submitted);
        log_u64(LogLevel::info, "go_live", "encoder_dropped", stats.dropped);
        log_u64(LogLevel::info, "go_live", "encoder_units", stats.encoded);
        log_u64(LogLevel::info, "go_live", "encoder_keyframes", stats.keyframes);
    }
    gpu_stream_encoder_destroy(encoder);
}
bool preview(void* context, GpuVideoFrame* output, u32* sequence) noexcept {
    auto& d = of(context);
    AcquireSRWLockShared(d.lock.native());
    const bool ok = d.encoder && gpu_stream_encoder_preview(d.encoder, output, sequence);
    ReleaseSRWLockShared(d.lock.native());
    return ok;
}
} // namespace

void go_live_devices_set_device(GoLiveDevices* devices, GpuDevice* device) noexcept {
    if (device)
        gpu_retain(device);
    AcquireSRWLockExclusive(devices->lock.native());
    GpuDevice* previous = devices->device;
    devices->device = device;
    ReleaseSRWLockExclusive(devices->lock.native());
    if (previous)
        gpu_release(previous);
}

GoLiveBackend go_live_devices_backend(GoLiveDevices* devices) noexcept {
    return {devices, encodable,   audio_available, capture, capture_stop_all, sound, sound_stop,
            encoder, configure,   bitrate,         keyframe, encoder_stop,    preview};
}

void go_live_devices_release(GoLiveDevices* devices) noexcept {
    capture_stop_all(devices);
    sound_stop(devices);
    encoder_stop(devices);
    go_live_devices_set_device(devices, nullptr);
}
} // namespace maicord
