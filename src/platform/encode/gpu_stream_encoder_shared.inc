// The parts of the Go Live encoder that are the same on both systems, included inside namespace maicord by
// src/platform/encode/gpu_stream_encoder.cpp (Windows) and src/platform/encode/linux/gpu_stream_encoder.cpp after their
// GpuStreamEncoder. Each defines now_us, and feed, which hands the pending picture on (into the transform when it asked
// for input on Windows; by waking the encoder thread on Linux).

namespace {

// How long the capture must have sent nothing before a keyframe request encodes its newest picture again.
constexpr u64 kStillUs = 100'000;

// Under the lock: a converted picture becomes the newest and the pending one (the newest pending wins).
void enqueue(GpuStreamEncoder* e, u8 index, u64 timestamp_us) noexcept {
    if (e->pending != kNone && e->pending != index)
        saturating_increment(e->stats.dropped);
    e->slots[index].timestamp_us = timestamp_us;
    e->newest = e->pending = index;
    e->last_accepted_us = timestamp_us;
    e->accepted = true;
    ++e->serial;
    feed(e);
}

// Under the lock: a keyframe was asked for and the capture has sent nothing for kStillUs or a frame interval, whichever
// is longer (a still screen sends no pictures): the newest picture again, now, once the encoder no longer holds it.
// While the capture runs, its next picture is the keyframe instead (a request that meets a capture going still just
// then waits for the receiver's next one).
void repeat_newest(GpuStreamEncoder* e) noexcept {
    if (!e->keyframe_requested || e->pending != kNone || e->newest == kNone || e->ended ||
        e->slots[e->newest].in_flight)
        return;
    const u32 fps = e->options.settings.max_fps ? e->options.settings.max_fps : 60;
    const u64 interval = 1'000'000u / fps;
    const u64 now = now_us();
    if (now >= e->last_accepted_us + (interval > kStillUs ? interval : kStillUs))
        enqueue(e, e->newest, now);
}

// Create and configuration, once per encoder or reconfiguration: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
bool settings_valid(const GpuStreamEncoderSettings& s) noexcept {
    return s.width >= 16 && s.height >= 16 && !((s.width | s.height) & 1) && s.width <= 4096 && s.height <= 4096 &&
           u64{s.width} * s.height <= kGpuVideoVisiblePixels && s.bitrate;
}
#pragma clang attribute pop

} // namespace

void gpu_stream_encoder_set_bitrate(GpuStreamEncoder* e, u32 bitrate) noexcept {
    if (!e || !bitrate)
        return;
    AcquireSRWLockExclusive(&e->lock);
    e->options.settings.bitrate = bitrate;
    ReleaseSRWLockExclusive(&e->lock);
}

void gpu_stream_encoder_request_keyframe(GpuStreamEncoder* e) noexcept {
    if (!e)
        return;
    AcquireSRWLockExclusive(&e->lock);
    e->keyframe_requested = true;
    repeat_newest(e);
    ReleaseSRWLockExclusive(&e->lock);
}

GpuStreamEncoderStats gpu_stream_encoder_stats(const GpuStreamEncoder* e) noexcept {
    if (!e)
        return {};
    auto* lock = const_cast<SRWLOCK*>(&e->lock);
    AcquireSRWLockShared(lock);
    const GpuStreamEncoderStats stats = e->stats;
    ReleaseSRWLockShared(lock);
    return stats;
}
