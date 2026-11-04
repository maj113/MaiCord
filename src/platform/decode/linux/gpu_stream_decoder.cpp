#include "stream.hpp"

#include "maicord/core/interlocked.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"
#include "maicord/gpu/api.hpp"
#include "maicord/media/video/av1.hpp"
#include "maicord/media/video/video_display.hpp"
#include "maicord/platform/decode/video_capability.hpp"
#include "../software/software_decoder.hpp"

// The Linux counterpart of src/platform/decode/gpu_stream_decoder.cpp: the public interface of
// maicord/platform/decode/gpu_stream_decoder.hpp over the backends of stream.hpp.

namespace maicord {

struct GpuStreamDecoder : GpuStream {};

namespace {

constexpr u32 kDecodeStackBytes = 1024u * 1024u;

void release_buffer(GpuStreamInput* input) noexcept {
    if (input->data)
        pages_release(input->data, input->capacity);
    reset_value(input);
}

bool reserve(GpuStreamInput* input, u32 bytes) noexcept {
    if (input->capacity >= bytes)
        return true;
    const u32 capacity = (bytes + 0xFFFFu) & ~0xFFFFu;
    auto* data = commit_pages<u8>(capacity);
    if (!data)
        return false;
    if (input->data)
        pages_release(input->data, input->capacity);
    input->data = data;
    input->capacity = capacity;
    return true;
}

bool has_resources(const GpuStream& stream) noexcept {
    return stream.surfaces.count || stream.session ||
           (stream.backend == &kSoftwareStreamBackend && stream.software.decoder);
}

void publish_stats(GpuStream* stream) noexcept {
    auto& stats = stream->stats;
    stream->backend->stats(*stream, &stats);
    stats.coded_width = stream->coded_width;
    stats.coded_height = stream->coded_height;
    stats.surfaces = stream->surfaces.count;
}

// Gives a stream whose GPU decoder cannot take its stream (a profile, size or format the driver refuses) to the
// software tier, which then waits for a keyframe.
void fall_back_to_software(GpuStream* stream) noexcept {
    stream->backend->release(stream);
    stream->backend = &kSoftwareStreamBackend;
    clear_bytes(&stream->h264, static_cast<u32>(sizeof(GpuStream) - __builtin_offsetof(GpuStream, h264)));
    stream->backend->reset(stream);
    stream->stats.unsupported = false;
    stream->last_request_ms = 0;
    log_u64(LogLevel::info, "video", "hardware_decode_fell_back_to_software", static_cast<u64>(stream->options.codec));
    stream_request_keyframe(stream, false);
}

// Frees the decoder and surfaces; the stream then waits for a keyframe (parameter sets are kept) and recreates them.
bool release_idle(GpuStream* stream) noexcept {
    if (!has_resources(*stream))
        return false;
    stream->backend->drain(stream); // Nothing decoded may be lost silently.
    stream->backend->release(stream);
    stream->backend->reset(stream);
    stream->last_request_ms = 0; // The first picture after release asks at once.
    saturating_increment(stream->stats.idle_releases);
    return true;
}

void run_decode(GpuStream* stream) noexcept {
    stream->backend->decode(stream, {stream->work.data, stream->work.bytes}, stream->work.rtp_timestamp,
                            stream->work.intact, stream->work.resynchronize);
    publish_stats(stream);
    if (stream->backend->hardware && stream->stats.unsupported && software_codec_available(stream->options.codec)) {
        fall_back_to_software(stream);
        publish_stats(stream);
    }
}

u32 decode_main(void* parameter) noexcept {
    auto* stream = static_cast<GpuStream*>(parameter);
    for (;;) {
        // Sleep indefinitely without decoder objects; otherwise wake once when the stream has been input-idle long
        // enough to release them.
        const u32 wait = has_resources(*stream) ? kGpuStreamIdleReleaseMs : INFINITE;
        if (wait_one(stream->wake, wait) == WAIT_TIMEOUT) {
            AcquireSRWLockExclusive(&stream->lock);
            const bool idle = !stream->count && !stream->stop;
            ReleaseSRWLockExclusive(&stream->lock);
            if (idle && release_idle(stream)) {
                AcquireSRWLockExclusive(&stream->lock);
                publish_stats(stream);
                ReleaseSRWLockExclusive(&stream->lock);
            }
            continue;
        }
        for (;;) {
            AcquireSRWLockExclusive(&stream->lock);
            if (stream->stop) {
                ReleaseSRWLockExclusive(&stream->lock);
                return 0;
            }
            if (!stream->count) {
                ReleaseSRWLockExclusive(&stream->lock);
                break;
            }
            // Swap buffers so the producer reuses the previous work buffer.
            GpuStreamInput& slot = stream->queue[stream->head];
            const GpuStreamInput taken = slot;
            slot.data = stream->work.data;
            slot.capacity = stream->work.capacity;
            slot.bytes = 0;
            stream->work = taken;
            --stream->count;
            // An empty queue reuses the spare in this slot on the next push.
            if (stream->count)
                stream->head = (stream->head + 1u) % kGpuStreamQueueFrames;
            ReleaseSRWLockExclusive(&stream->lock);
            run_decode(stream);
        }
    }
}

} // namespace

// ---- Backend services -------------------------------------------------------------------------------------------

void stream_clear_geometry(GpuStream* stream) noexcept {
    reset_value(&stream->frame);
    stream->presented = 255;
    stream->coded_width = stream->coded_height = 0;
}

void stream_surfaces_release(GpuStream* stream) noexcept {
    for (u32 i = 0; i < stream->surfaces.count; ++i) {
        StreamSurface& slot = stream->surfaces.slot[i];
        gpu_release(slot.chroma_view);
        gpu_release(slot.chroma_texture);
        gpu_release(slot.luma_view);
        gpu_release(slot.luma_texture);
    }
    reset_value(&stream->surfaces);
    stream_clear_geometry(stream);
}

bool stream_surfaces_create_cpu(GpuStream* stream, u32 count, u32 coded_width, u32 coded_height) noexcept {
    stream_surfaces_release(stream);
    if (!count || count > kStreamSurfaceLimit || !coded_width || !coded_height || ((coded_width | coded_height) & 1u))
        return false;
    for (u32 i = 0; i < count; ++i) {
        StreamSurface& slot = stream->surfaces.slot[i];
        const GpuStatus luma = gpu_video_plane_create(stream->device, GpuVideoPlaneFormat::r8, coded_width, coded_height,
                                                      &slot.luma_texture, &slot.luma_view, &slot.luma_pixels,
                                                      &slot.luma_pitch);
        const GpuStatus chroma = gpu_ok(luma)
                                     ? gpu_video_plane_create(stream->device, GpuVideoPlaneFormat::rg8, coded_width / 2,
                                                              coded_height / 2, &slot.chroma_texture, &slot.chroma_view,
                                                              &slot.chroma_pixels, &slot.chroma_pitch)
                                     : luma;
        stream->surfaces.count = i + (gpu_ok(luma) ? 1u : 0u);
        if (!gpu_ok(chroma)) {
            stream->surfaces.count = i + 1u; // the luma plane made above is released with the rest
            stream_surfaces_release(stream);
            return false;
        }
    }
    stream->surfaces.count = count;
    stream->surfaces.width = coded_width;
    stream->surfaces.height = coded_height;
    stream->coded_width = coded_width;
    stream->coded_height = coded_height;
    return true;
}

void stream_set_format(GpuStream* stream, u32 crop_x, u32 crop_y, u32 width, u32 height, u8 matrix, bool full_range,
                       u32 sar_width, u32 sar_height) noexcept {
    stream->frame = {stream->surfaces.count ? stream->surfaces.slot[0].luma_texture : nullptr,
                     0,
                     crop_x,
                     crop_y,
                     width,
                     height,
                     video_matrix_bt709(matrix, height),
                     full_range,
                     0,
                     {}};
    const auto extent = video_display_extent(width, height, sar_width, sar_height);
    stream->display_width = extent.width;
    stream->display_height = extent.height;
}

void stream_present(GpuStream* stream, u8 slot, u32 rtp_timestamp, bool keyframe) noexcept {
    if (!stream->surfaces.count || slot >= stream->surfaces.count)
        return;
    const StreamSurface& surface = stream->surfaces.slot[slot];
    stream->frame.texture = surface.luma_texture;
    stream->frame.subresource = slot;
    stream->frame.luma = surface.luma_view;
    stream->frame.chroma = surface.chroma_view;
    stream->presented = slot;
    saturating_increment(stream->stats.presented);
    if (stream->options.sink.frame) {
        const GpuStreamFrame frame{&stream->frame, rtp_timestamp, stream->display_width, stream->display_height,
                                   keyframe};
        stream->options.sink.frame(stream->options.sink.context, stream->options.ssrc, frame);
    }
}

u32 stream_held(const GpuStream& stream) noexcept {
    const u32 hold = static_cast<u32>(read_acquire(&stream.hold));
    u32 mask = stream.presented < 32 ? 1u << stream.presented : 0u;
    if (hold && hold <= 32)
        mask |= 1u << (hold - 1u);
    return mask;
}

void stream_request_keyframe(GpuStream* stream, bool unsupported) noexcept {
    if (unsupported || stream->stats.device_lost)
        return;
    const u64 now = clock_monotonic_milliseconds();
    if (stream->last_request_ms && now - stream->last_request_ms < kGpuStreamKeyframeIntervalMs)
        return;
    stream->last_request_ms = now ? now : 1;
    saturating_increment(stream->stats.keyframe_requests);
    if (stream->options.sink.request_keyframe)
        stream->options.sink.request_keyframe(stream->options.sink.context, stream->options.ssrc);
}

bool stream_unit_is_keyframe(GpuStreamCodec codec, ConstSlice<u8> bytes) noexcept {
    if (codec == GpuStreamCodec::av1) {
        u32 offset = 0;
        Av1Obu obu{};
        while (offset < bytes.count && av1_next_obu(bytes, &offset, &obu) == Av1Status::ok)
            if (obu.type == kAv1ObuSequenceHeader)
                return true;
        return false;
    }
    H264AnnexBCursor cursor{bytes, 0};
    for (;;) {
        const H264Nal nal = h264_annexb_next(&cursor);
        if (nal.error == H264Error::end || nal.error == H264Error::invalid_input)
            return false;
        if (nal.error == H264Error::none && (codec == GpuStreamCodec::h264 ? nal.type == 5 : false))
            return true;
    }
}

// ---- gpu_stream_decoder.hpp -------------------------------------------------------------------------------------

u32 gpu_stream_decoder_codecs(GpuDevice* device) noexcept {
    VideoDecodeProbe probe{};
    probe.hardware = device ? hardware_probe_codecs(device) : 0;
    for (u8 codec = 0; codec <= static_cast<u8>(GpuStreamCodec::av1); ++codec)
        if (software_codec_available(static_cast<GpuStreamCodec>(codec)))
            probe.software |= 1u << codec;
    return video_decode_codecs_word(probe);
}

GpuStreamDecoder* gpu_stream_decoder_create(const GpuStreamDecoderOptions& options) noexcept {
    if (!options.device || options.codec > GpuStreamCodec::av1)
        return nullptr;
    const StreamBackend* backend = nullptr;
    if (hardware_probe_codecs(options.device) & gpu_stream_codec_bit(options.codec))
        backend = hardware_stream_backend(options.codec);
    if (!backend && (kVideoDecodeSoftwareCapable & gpu_stream_codec_bit(options.codec)) &&
        software_codec_available(options.codec))
        backend = &kSoftwareStreamBackend;
    if (!backend)
        return nullptr;
    auto* stream = commit_pages<GpuStreamDecoder>();
    if (!stream)
        return nullptr;
    stream->options = options;
    stream->presented = 255;
    stream->backend = backend;
    backend->reset(stream);
    stream->device = options.device;
    gpu_retain(options.device);
    if (!options.synchronous) {
        stream->wake = event_create(false, false);
        if (stream->wake)
            stream->thread = thread_start(decode_main, static_cast<GpuStream*>(stream), kDecodeStackBytes, nullptr);
        if (!stream->thread) {
            gpu_stream_decoder_destroy(stream);
            return nullptr;
        }
    }
    return stream;
}

bool gpu_stream_decoder_push(GpuStreamDecoder* stream, ConstSlice<u8> annexb, u32 rtp_timestamp, bool intact) noexcept {
    // Bound before copying: an oversized frame is dropped like a lost one.
    if (!stream || !annexb.data || !annexb.count || annexb.count > kGpuStreamMaximumFrameBytes)
        return false;
    if (stream->options.synchronous) {
        saturating_increment(stream->stats.received);
        const bool resynchronize = stream->resynchronize_pending;
        stream->resynchronize_pending = false;
        stream->backend->decode(stream, annexb, rtp_timestamp, intact, resynchronize);
        publish_stats(stream);
        if (stream->backend->hardware && stream->stats.unsupported &&
            software_codec_available(stream->options.codec)) {
            fall_back_to_software(stream);
            publish_stats(stream);
        }
        return true;
    }
    AcquireSRWLockExclusive(&stream->lock);
    saturating_increment(stream->stats.received);
    bool resynchronize = stream->resynchronize_pending;
    if (stream->count == kGpuStreamQueueFrames) {
        // The decoder fell behind: queued latency is worse than a freeze.
        stream->count = 0;
        resynchronize = true;
    }
    GpuStreamInput& slot = stream->queue[(stream->head + stream->count) % kGpuStreamQueueFrames];
    if (!reserve(&slot, annexb.count)) {
        stream->resynchronize_pending = true;
        ReleaseSRWLockExclusive(&stream->lock);
        return false;
    }
    copy_bytes(slot.data, annexb.data, annexb.count);
    slot.bytes = annexb.count;
    slot.rtp_timestamp = rtp_timestamp;
    slot.intact = intact;
    slot.resynchronize = resynchronize;
    stream->resynchronize_pending = false;
    ++stream->count;
    ReleaseSRWLockExclusive(&stream->lock);
    event_set(stream->wake);
    return true;
}

void gpu_stream_decoder_hold(GpuStreamDecoder* stream, u32 subresource) noexcept {
    if (stream && subresource < 32)
        InterlockedExchange(&stream->hold, static_cast<LONG>(subresource + 1u));
}

bool gpu_stream_decoder_release_idle(GpuStreamDecoder* stream) noexcept {
    if (!stream || !stream->options.synchronous || !release_idle(stream))
        return false;
    publish_stats(stream);
    return true;
}

GpuStreamDecoderStats gpu_stream_decoder_stats(const GpuStreamDecoder* stream) noexcept {
    if (!stream)
        return {};
    auto* lock = const_cast<SRWLOCK*>(&stream->lock);
    AcquireSRWLockShared(lock);
    const GpuStreamDecoderStats stats = stream->stats;
    ReleaseSRWLockShared(lock);
    return stats;
}

void gpu_stream_decoder_destroy(GpuStreamDecoder* stream) noexcept {
    if (!stream)
        return;
    if (stream->thread) {
        AcquireSRWLockExclusive(&stream->lock);
        stream->stop = true;
        ReleaseSRWLockExclusive(&stream->lock);
        event_set(stream->wake);
        thread_wait(stream->thread, INFINITE);
        thread_close(stream->thread);
    }
    if (stream->wake)
        waitable_close(stream->wake);
    stream->backend->release(stream);
    if (stream->device)
        gpu_release(stream->device);
    for (auto& input : stream->queue)
        release_buffer(&input);
    release_buffer(&stream->work);
    pages_release(stream, sizeof(GpuStreamDecoder));
}

} // namespace maicord
