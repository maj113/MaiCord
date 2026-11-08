#pragma once

#include "maicord/media/video/h264_slice.hpp"
#include "maicord/media/containers/mp4_demux.hpp"
#include "maicord/media/containers/mp4_fragment_source.hpp"
#include "maicord/media/containers/mp4_segment_store.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/platform/decode/h264_gpu_decoder.hpp"

namespace maicord {

// Selected configuration and current access unit for the native GPU decoder.
struct H264GpuPicture {
    H264Sps sps;
    H264Pps pps;
    Mp4AccessUnit unit;
    H264Picture header;
    u8 length_bytes;
};
// Container-independent entry point: validates the actual first IDR against
// the borrowed AVC configuration, including its decoded display dimensions.
H264Error h264_gpu_configuration_from_unit(const Mp4AccessUnit&, ConstSlice<u8> avcc,
    u8 length_bytes, u32 width, u32 height, H264GpuPicture*) noexcept;
// Inspect an initialized demuxer through a copy. Preserve the playback cursor.
H264Error h264_gpu_configuration_from_demux(const Mp4Demuxer& demux, H264GpuPicture* output,
                                            Mp4DemuxError* demux_error = nullptr) noexcept;

// The Windows (h264_gpu_decoder.cpp) and Linux (linux/h264_file_decoder.cpp) movie decoders share the container side
// of fragmented sources below. Each runs once per stream or seek and has one caller per decoder, but the seek (two):
// they are folded into those callers.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

// Whether a segmented source holds [offset, offset + bytes): the availability of its decoders.
inline bool h264_fragment_bytes_available(void* context, u64 offset, u64 bytes) noexcept {
    const auto span = mp4_fragment_source_bytes(*static_cast<Mp4FragmentSource*>(context));
    return offset <= span.count && bytes <= span.count - offset;
}

// The availability a decoder reads a fragment source through, when the source can start one: a valid video source
// at its initial packet. False otherwise, with *demux_error set when the source refuses that packet.
__forceinline bool h264_fragment_availability(Mp4FragmentSource* source, Mp4DemuxError* demux_error,
                                              Mp4SourceAvailability* availability) noexcept {
    if (!mp4_fragment_source_valid(*source) || source->track.audio || source->ended || source->segment.index ||
        (source->loaded && source->fragment.index))
        return false;
    Mp4AccessUnit first{};
    const auto error = mp4_fragment_source_unit(source, &first);
    if (error != Mp4DemuxError::none) {
        *demux_error = error;
        return false;
    }
    *availability = source->segments ? Mp4SourceAvailability{h264_fragment_bytes_available, source}
                                     : progressive_source_reader(source->source, source->reader);
    return true;
}

// The demuxer of a fragment source at its initial packet (the track's metadata over `encoded`, ready when the
// packet is an IDR of the track's configuration and size), the configuration from that packet and its presentation
// time. *demux_error says why the source failed; the result why the packet did.
__forceinline H264Error h264_gpu_configuration_from_fragment(Mp4FragmentSource* source, ConstSlice<u8> encoded,
                                                             Mp4SourceAvailability availability, Mp4Demuxer* demux,
                                                             H264GpuPicture* picture, Mp4DemuxError* demux_error,
                                                             i64* first_presentation) noexcept {
    const auto& track = source->track;
    demux->encoded = encoded;
    demux->availability = availability;
    demux->width = track.width;
    demux->height = track.height;
    demux->sar_width = track.sar_width;
    demux->sar_height = track.sar_height;
    demux->rotation = track.rotation;
    demux->nal_length_bytes = track.nal_length_bytes;
    demux->timescale = track.timescale;
    demux->has_edit_list = track.edited;
    demux->avcc_offset = static_cast<u32>(track.configuration.data - encoded.data);
    demux->avcc_bytes = track.configuration.count;
    Mp4AccessUnit first{};
    *demux_error = mp4_fragment_source_unit(source, &first);
    if (*demux_error == Mp4DemuxError::none && !mp4_fragment_source_duration(*source, &demux->duration_us))
        *demux_error = Mp4DemuxError::unsupported;
    const H264Error parsed = *demux_error == Mp4DemuxError::none
                                 ? h264_gpu_configuration_from_unit(first, track.configuration, track.nal_length_bytes,
                                                                    track.width, track.height, picture)
                                 : H264Error::malformed;
    demux->ready = parsed == H264Error::none;
    *first_presentation = first.presentation_time_us_signed;
    return parsed;
}

// The seek in progress over a segmented source, resumed by the next call for the same target.
struct H264FragmentSeek {
    i64 target;
    u32 candidate, attempts;
    bool pending, start;
};
// Adopts the IDR at or before `target` (the first one for `start`) as the source's position; index SAP flags are
// hints, so the real access unit is validated against `picture`'s configuration first. data_pending while the
// candidate segment is still arriving.
inline Mp4DemuxError h264_fragment_seek(Mp4FragmentSource& stream, H264FragmentSeek* seek,
                                        const H264GpuPicture& picture, i64 target, Mp4SeekPoint* point,
                                        bool start) noexcept {
    Mp4FragmentRange range{};
    u32 selected = 0;
    if (stream.segments) {
        if (seek->pending && seek->target == target && seek->start == start)
            selected = seek->candidate;
        else {
            if (!start) {
                i64 empty{};
                if (target < 0 || !mp4_fragment_source_time(stream, stream.track.media_start, &empty))
                    return Mp4DemuxError::unsupported;
                const i64 relative = target - empty;
                const u64 absolute = relative < 0 ? static_cast<u64>(-relative) : static_cast<u64>(relative);
                if (absolute > 86400000000ull)
                    return Mp4DemuxError::unsupported;
                const u64 ticks = absolute / 1000000 * stream.track.timescale +
                                  (absolute % 1000000) * stream.track.timescale / 1000000;
                const u64 origin = static_cast<u64>(stream.track.media_start);
                if ((relative < 0 && ticks > origin) || ticks > 0x7fffffffffffffffull - origin)
                    return Mp4DemuxError::unsupported;
                u64 time = relative < 0 ? origin - ticks : origin + ticks;
                time = minimum(time, stream.index.first_time + stream.index.duration - 1);
                const auto found = mp4_fragment_source_find(&stream, time, &range);
                if (found != Mp4DemuxError::none)
                    return found;
                selected = range.index;
            }
            *seek = {target, selected, 0, true, start};
        }
    } else {
        if (!mp4_fragment_index_range(stream.index, 0, &range))
            return Mp4DemuxError::malformed;
        if (!start) {
            i64 time{};
            if (!mp4_fragment_source_time(stream, static_cast<i64>(range.time), &time))
                return Mp4DemuxError::unsupported;
            if (time > target)
                return Mp4DemuxError::unsupported;
            for (u32 i = 1; i < stream.index.count; ++i) {
                if (!mp4_fragment_index_follow(stream.index, range, &range))
                    return Mp4DemuxError::malformed;
                if (!mp4_fragment_source_time(stream, static_cast<i64>(range.time), &time))
                    return Mp4DemuxError::unsupported;
                if (time > target)
                    break;
                selected = i;
            }
        }
    }
    for (u32 attempts = 0; attempts < 64; ++attempts) {
        if (stream.segments && seek->attempts == 64)
            break;
        Mp4FragmentPosition candidate{};
        Mp4AccessUnit unit{};
        auto error = mp4_fragment_source_prepare(&stream, selected, 0, &candidate);
        if (error != Mp4DemuxError::none)
            return error;
        error = mp4_fragment_position_unit(candidate, &unit);
        if (error != Mp4DemuxError::none) {
            mp4_fragment_source_cancel_prepare(&stream);
            return error;
        }
        if (start || unit.presentation_time_us_signed <= target) {
            H264Picture parsed{};
            const auto result = h264_parse_picture(unit.bytes, picture.length_bytes, picture.sps, picture.pps, &parsed,
                                                   stream.track.configuration);
            if (result != H264Error::none) {
                mp4_fragment_source_cancel_prepare(&stream);
                return result == H264Error::unsupported ? Mp4DemuxError::unsupported : Mp4DemuxError::malformed;
            }
            if (parsed.first.idr) {
                if (!mp4_fragment_source_adopt(&stream, candidate))
                    return Mp4DemuxError::data_pending;
                seek->pending = false;
                *point = {unit.sample_index, unit.decode_time_us, unit.presentation_time_us_signed};
                return Mp4DemuxError::none;
            }
        }
        if (start || !selected)
            break;
        --selected;
        seek->candidate = selected;
        ++seek->attempts;
    }
    mp4_fragment_source_cancel_prepare(&stream);
    seek->pending = false;
    return Mp4DemuxError::unsupported;
}

// The movie of a ready demuxer; a fragment source reports its live horizon and the first presentation recorded at
// initialization.
__forceinline H264GpuMovie h264_gpu_movie(const Mp4Demuxer& demux, const Mp4FragmentSource* fragmented,
                                          i64 first_presentation) noexcept {
    if (!demux.ready)
        return {};
    H264GpuMovie movie{demux.duration_us, demux.sample_count, demux.has_edit_list};
    if (fragmented) {
        if (const auto* store = fragmented->segments; store && store->live) {
            if (!mp4_fragment_source_duration(*fragmented, &movie.duration_us))
                return {};
            movie.live = true;
            movie.duration_complete = store->ended && store->received_count == store->count;
        }
        movie.first_presentation_time_us = first_presentation;
        movie.first_presentation_valid = true;
        return movie;
    }
    Mp4Demuxer first;
    copy_value(&first, demux);
    mp4_demux_reset(&first);
    const auto unit = mp4_demux_next(&first);
    if (unit.ok()) {
        movie.first_presentation_time_us = unit.presentation_time_us_signed;
        movie.first_presentation_valid = true;
    }
    return movie;
}
#pragma clang attribute pop

} // namespace maicord
