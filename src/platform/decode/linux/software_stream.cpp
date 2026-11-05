#include "stream.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/gpu/video_planes.hpp"
#include "../software/software_decoder.hpp"

// The software tier of the live stream decoder: libopenh264 and libdav1d through SoftwareDecoder, the pictures copied
// into the planes of a few CPU-backed surfaces (what the renderer then uploads). Loss freezes the last good picture
// and waits for a keyframe, as the hardware tier does.

namespace maicord {
namespace {

// The newest picture and one a reader holds are never overwritten; one more is being decoded.
constexpr u32 kSoftwareSurfaces = 2 + 1;

SoftwareStreamState& state_of(GpuStream* stream) noexcept {
    return stream->software;
}

void release_decoder(GpuStream* stream) noexcept {
    auto& state = state_of(stream);
    software_decoder_destroy(state.decoder);
    state.decoder = nullptr;
    stream_surfaces_release(stream);
}

void request_keyframe(GpuStream* stream) noexcept {
    stream_request_keyframe(stream, stream->software.unsupported);
}

void lose(GpuStream* stream) noexcept {
    auto& state = state_of(stream);
    state.waiting = true;
    software_decoder_reset(state.decoder);
}

} // namespace

// Interleaves a picture into the planes of slot and publishes them; the bytes written.
u64 stream_copy_picture(const StreamSurface& slot, const SoftwarePicture& picture) noexcept {
    for (u32 y = 0; y < picture.height; ++y)
        copy_bytes(slot.luma_pixels + usize{y} * slot.luma_pitch, picture.plane[0] + usize{y} * picture.stride[0],
                   picture.width);
    const u32 chroma_width = (picture.width + 1) / 2, chroma_height = (picture.height + 1) / 2;
    for (u32 y = 0; y < chroma_height; ++y) {
        u8* out = slot.chroma_pixels + usize{y} * slot.chroma_pitch;
        const u8* u = picture.plane[1] + usize{y} * picture.stride[1];
        const u8* v = picture.plane[2] + usize{y} * picture.stride[2];
        for (u32 x = 0; x < chroma_width; ++x) {
            out[2 * x] = u[x];
            out[2 * x + 1] = v[x];
        }
    }
    gpu_video_plane_publish(slot.luma_texture);
    gpu_video_plane_publish(slot.chroma_texture);
    return u64{picture.width} * picture.height + u64{chroma_width} * chroma_height * 2;
}

namespace {

void copy_picture(GpuStream* stream, u32 slot_index, const SoftwarePicture& picture) noexcept {
    stream->stats.uploaded_bytes += stream_copy_picture(stream->surfaces.slot[slot_index], picture);
}

// The next slot that is neither presented nor held.
bool free_slot(GpuStream* stream, u32* slot) noexcept {
    auto& state = state_of(stream);
    const u32 held = stream_held(*stream);
    for (u32 i = 0; i < stream->surfaces.count; ++i) {
        const u32 candidate = (state.next_surface + i) % stream->surfaces.count;
        if (!(held & (1u << candidate))) {
            *slot = candidate;
            state.next_surface = candidate + 1;
            return true;
        }
    }
    return false;
}

void present_pictures(GpuStream* stream, u32 rtp_timestamp, bool keyframe) noexcept {
    auto& state = state_of(stream);
    SoftwarePicture picture{};
    while (software_decoder_pop(state.decoder, &picture)) {
        const u32 coded_width = (picture.width + 1) & ~1u, coded_height = (picture.height + 1) & ~1u;
        if (stream->surfaces.width != coded_width || stream->surfaces.height != coded_height || !stream->surfaces.count) {
            if (!stream_surfaces_create_cpu(stream, kSoftwareSurfaces, coded_width, coded_height)) {
                saturating_increment(stream->stats.gpu_failures);
                state.unsupported = true;
                return;
            }
            saturating_increment(stream->stats.reinitializations);
        }
        u32 slot = 0;
        if (!free_slot(stream, &slot)) {
            saturating_increment(state.dropped);
            continue;
        }
        copy_picture(stream, slot, picture);
        stream_set_format(stream, 0, 0, picture.width, picture.height, picture.matrix, picture.full_range,
                          picture.sar_width, picture.sar_height);
        ++state.decoded;
        stream->last_request_ms = 0; // The stream is healthy.
        stream_present(stream, static_cast<u8>(slot), rtp_timestamp, keyframe);
    }
}

void software_reset(GpuStream* stream) noexcept {
    auto& state = state_of(stream);
    if (state.decoder)
        software_decoder_reset(state.decoder);
    state.waiting = true;
}

void software_drain(GpuStream*) noexcept {
    // Pictures are shown as they are decoded; nothing is held back.
}

void software_decode(GpuStream* stream, ConstSlice<u8> bytes, u32 rtp_timestamp, bool intact,
                     bool resynchronize) noexcept {
    auto& state = state_of(stream);
    if (state.decoder && resynchronize)
        lose(stream);
    else if (resynchronize)
        state.waiting = true;
    if (state.unsupported) {
        saturating_increment(state.dropped);
        return;
    }
    if (!intact) {
        ++state.damaged;
        if (state.decoder)
            lose(stream);
        state.waiting = true;
        request_keyframe(stream);
        return;
    }
    const bool keyframe = stream_unit_is_keyframe(stream->options.codec, bytes);
    if (state.waiting) {
        if (!keyframe) {
            ++state.dropped;
            request_keyframe(stream);
            return;
        }
        state.waiting = false;
    }
    if (!state.decoder) {
        state.decoder = software_decoder_create(stream->options.codec);
        if (!state.decoder) {
            state.unsupported = true;
            return;
        }
    }
    const SoftwareStatus status = software_decoder_push(state.decoder, bytes);
    switch (status) {
    case SoftwareStatus::ok:
        present_pictures(stream, rtp_timestamp, keyframe);
        break;
    case SoftwareStatus::lost:
        ++state.damaged;
        lose(stream);
        request_keyframe(stream);
        break;
    case SoftwareStatus::unsupported:
        state.unsupported = true;
        lose(stream);
        break;
    case SoftwareStatus::failed:
        saturating_increment(stream->stats.gpu_failures);
        lose(stream);
        request_keyframe(stream);
        break;
    }
}

void software_stats(const GpuStream& stream, GpuStreamDecoderStats* stats) noexcept {
    const auto& state = stream.software;
    stats->decoded = state.decoded;
    stats->dropped = state.dropped;
    stats->damaged = state.damaged;
    stats->gaps = state.gaps;
    stats->unsupported = state.unsupported;
}

} // namespace

const StreamBackend kSoftwareStreamBackend{false, software_reset, software_decode, software_drain, release_decoder,
                                           software_stats};

} // namespace maicord
