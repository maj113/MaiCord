#include "gpu_stream_decoder_backend.hpp"
#include "maicord/media/video/video_display.hpp"
#include "maicord/core/interlocked.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/core/time.hpp"
#if !defined(_WIN32)
#include "linux/stream.hpp"
#include "software/software_decoder.hpp"
#endif
// The queue and the decode thread, once per frame around the codec's work: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {

struct GpuStreamDecoder : GpuStream {};

namespace {

#if defined(_WIN32)
constexpr u32 kDecodeStackBytes = 256u * 1024u;
#else
constexpr u32 kDecodeStackBytes = 1024u * 1024u;
#endif

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
#if defined(_WIN32)
    return stream.decoder != nullptr; // The surfaces exist only with the decoder.
#else
    return stream.surfaces.count || stream.session ||
           (stream.backend == &kSoftwareStreamBackend && stream.software.decoder);
#endif
}

// Hands the decode thread's counters to readers (any thread) under the lock; the pusher's `received` stays.
void publish_stats(GpuStream* stream) noexcept {
    auto& stats = stream->stats;
    stream->backend->stats(*stream, &stats);
    stats.coded_width = stream->coded_width;
    stats.coded_height = stream->coded_height;
    stats.surfaces = stream->surfaces.count;
    AcquireSRWLockExclusive(&stream->lock);
    const u32 received = stream->published.received;
    stream->published = stats;
    stream->published.received = received;
    ReleaseSRWLockExclusive(&stream->lock);
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

void run_decode(GpuStream* stream, ConstSlice<u8> bytes, u32 rtp_timestamp, bool intact, bool resynchronize) noexcept {
    stream->backend->decode(stream, bytes, rtp_timestamp, intact, resynchronize);
    publish_stats(stream);
#if !defined(_WIN32)
    if (stream->backend->hardware && stream->stats.unsupported && software_codec_available(stream->options.codec)) {
        stream_fall_back_to_software(stream);
        publish_stats(stream);
    }
#endif
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
            if (idle && release_idle(stream))
                publish_stats(stream);
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
            run_decode(stream, {stream->work.data, stream->work.bytes}, stream->work.rtp_timestamp,
                       stream->work.intact, stream->work.resynchronize);
        }
    }
}

} // namespace

void stream_set_format(GpuStream* stream, u32 crop_x, u32 crop_y, u32 width, u32 height, u8 matrix, bool full_range,
                       u32 sar_width, u32 sar_height) noexcept {
#if defined(_WIN32)
    GpuTexture* texture = stream->surfaces.texture;
#else
    GpuTexture* texture = stream->surfaces.count ? stream->surfaces.slot[0].luma_texture : nullptr;
#endif
    stream->frame = {texture, 0, crop_x, crop_y, width, height, video_matrix_bt709(matrix, height), full_range, 0, {}};
    const auto extent = video_display_extent(width, height, sar_width, sar_height);
    stream->display_width = extent.width;
    stream->display_height = extent.height;
}

void stream_present(GpuStream* stream, u8 slot, u32 rtp_timestamp, bool keyframe) noexcept {
    if (slot >= stream->surfaces.count)
        return;
#if defined(_WIN32)
    stream->frame.texture = stream->surfaces.texture;
    stream->frame.luma = stream->surfaces.luma[slot];
    stream->frame.chroma = stream->surfaces.chroma[slot];
#else
    const StreamSurface& surface = stream->surfaces.slot[slot];
    stream->frame.texture = surface.luma_texture;
    stream->frame.luma = surface.luma_view;
    stream->frame.chroma = surface.chroma_view;
#endif
    stream->frame.subresource = slot;
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

GpuStreamDecoder* gpu_stream_decoder_create(const GpuStreamDecoderOptions& options) noexcept {
    if (!options.device || options.codec > GpuStreamCodec::av1)
        return nullptr;
    const StreamBackend* backend = stream_backend(options.device, options.codec);
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
#if defined(_WIN32)
    options.device->GetImmediateContext(&stream->immediate);
    if (!stream->immediate) {
        gpu_stream_decoder_destroy(stream);
        return nullptr;
    }
#endif
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
        saturating_increment(stream->published.received);
        const bool resynchronize = stream->resynchronize_pending;
        stream->resynchronize_pending = false;
        run_decode(stream, annexb, rtp_timestamp, intact, resynchronize);
        return true;
    }
    AcquireSRWLockExclusive(&stream->lock);
    saturating_increment(stream->published.received);
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
    const GpuStreamDecoderStats stats = stream->published;
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
#if defined(_WIN32)
    com_release(stream->guard);
    com_release(stream->context);
    com_release(stream->video);
    com_release(stream->immediate);
#endif
    gpu_release(stream->device);
    for (auto& input : stream->queue)
        release_buffer(&input);
    release_buffer(&stream->work);
    pages_release(stream, sizeof(GpuStreamDecoder));
}

} // namespace maicord
#pragma clang attribute pop
