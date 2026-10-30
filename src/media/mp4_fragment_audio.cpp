#include "maicord/media/mp4_fragment_audio.hpp"
#include "maicord/media/mp4_segment_store.hpp"
#include "maicord/core/byte_range.hpp"
// Container parsing, once per packet and no byte loop: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace {
using E = Mp4DemuxError;
bool separate(const Mp4FragmentSource& source, const void* owner, u32 owner_bytes,
              const void* output, u32 bytes) noexcept {
    const ConstSlice<u8> range{static_cast<const u8*>(output), bytes};
    return byte_range_valid(range) &&
        !byte_ranges_overlap(range, {static_cast<const u8*>(owner), owner_bytes}) &&
        !mp4_fragment_source_overlaps(source, output, bytes);
}
}
Mp4DemuxError mp4_fragment_audio_initialize(Mp4FragmentSource* source, Mp4FragmentAudio* output) noexcept {
    if (!source || !mp4_fragment_source_valid(*source) || !output || output->source || !source->track.audio ||
        source->ended || source->segment.index || (source->loaded && source->fragment.index) ||
        !separate(*source, source, sizeof(*source), output, sizeof(*output))) return E::invalid_input;
    const auto& track = source->track;
    const auto& index = source->index;
    if (!track.sample_rate || !track.timescale || !track.movie_timescale || track.media_start < 0 ||
        (track.frames_per_packet != 1024 && track.frames_per_packet != 2048)) return E::unsupported;
    const u64 ticks = u64{track.timescale} * track.frames_per_packet;
    if (ticks % track.sample_rate || ticks / track.sample_rate > ~u32{0}) return E::unsupported;
    const u32 packet_ticks = static_cast<u32>(ticks / track.sample_rate);
    if (!packet_ticks || !index.duration || index.duration > ~u64{0} - index.first_time) return E::unsupported;
    Mp4FragmentRange range{};
    if (source->segments) range = source->segment;
    else if (!mp4_fragment_index_range(index, 0, &range)) return E::malformed;
    u32 index_edit_ticks=0;
    Mp4FragmentPosition first{};
    if(!source->segments && track.edited && !index.first_time && track.media_start>0) {
        const auto error=mp4_fragment_source_prepare(source,0,0,&first);
        mp4_fragment_source_cancel_prepare(source);
        if(error!=E::none) return error;
        if(first.fragment.first_time) return E::unsupported;
        // Index timing may already include priming, or omit exactly the track
        // edit. Whole-packet priming is indistinguishable from alignment alone.
        if(first.fragment.duration!=range.duration) {
            if(first.fragment.duration<range.duration || static_cast<u64>(track.media_start)>~u32{0} ||
                first.fragment.duration-range.duration!=static_cast<u64>(track.media_start)) return E::unsupported;
            index_edit_ticks=static_cast<u32>(track.media_start);
        }
    }
    if(index.duration>~u64{0}-index.first_time-index_edit_ticks) return E::unsupported;
    const u64 decoded_duration=index.duration+index_edit_ticks;
    const u64 count=decoded_duration/packet_ticks+u64{decoded_duration%packet_ticks!=0};
    if(!count||count>~u32{0}) return E::unsupported;
    if (source->segments && index.count > 1 && range.duration % packet_ticks) return E::unsupported;
    for (u32 i = 1; !source->segments && i < index.count; ++i) {
        if ((range.duration+(i==1?index_edit_ticks:0)) % packet_ticks) return E::unsupported;
        if (!mp4_fragment_index_follow(index, range, &range)) return E::malformed;
    }
    const u64 end = index.first_time + decoded_duration, start = static_cast<u64>(track.media_start);
    if (start >= end) return E::unsupported;
    Mp4FragmentAudio value{};
    value.source = source; value.packet_ticks = packet_ticks; value.packet_count = static_cast<u32>(count);
    value.index_edit_ticks=index_edit_ticks;
    value.timeline = {count * track.frames_per_packet, index.first_time, start, track.empty_edit,
        track.edit_duration ? track.edit_duration : end - start, track.sample_rate, track.timescale,
        track.movie_timescale, track.edit_duration ? track.movie_timescale : track.timescale};
    Mp4AudioSampleWindow window{};
    if (!mp4_audio_sample_window(value.timeline, 0, &window) || window.duration_100ns <= 0 ||
        window.first_source_frame == window.end_source_frame) return E::unsupported;
    if(first.owner && !mp4_fragment_source_adopt(source,first)) return E::data_pending;
    *output = value;
    return E::none;
}
namespace {
E audio_packet(const Mp4FragmentAudio& audio, Mp4FragmentRange segment, const Mp4Fragment& fragment,
               const Mp4FragmentPacket& packet, Mp4FragmentAudioPacket* output) noexcept {
    const auto& stream = *audio.source;
    if (audio.packet_index >= audio.packet_count) return E::malformed;
    const u64 expected = stream.index.first_time + u64{audio.packet_index} * audio.packet_ticks;
    const u64 remaining = stream.index.first_time + stream.index.duration + audio.index_edit_ticks - expected;
    const u32 duration = static_cast<u32>(minimum<u64>(remaining, audio.packet_ticks));
    if(audio.index_edit_ticks) {
        if(segment.index) segment.time+=audio.index_edit_ticks;
        else segment.duration+=audio.index_edit_ticks;
        if(fragment.first_time!=segment.time || fragment.duration!=segment.duration) return E::unsupported;
    }
    if (packet.sample.decode_time != expected || packet.sample.presentation_time < 0 ||
        static_cast<u64>(packet.sample.presentation_time) != expected || packet.sample.duration != duration ||
        fragment.first_time < segment.time || fragment.first_time - segment.time > segment.duration ||
        fragment.duration > segment.duration - (fragment.first_time - segment.time) ||
        (fragment.first_time - segment.time) % audio.packet_ticks ||
        fragment.count != fragment.duration / audio.packet_ticks +
            u64{fragment.duration % audio.packet_ticks != 0}) return E::unsupported;
    Mp4FragmentAudioPacket value{};
    value.bytes = packet.bytes;
    value.first_source_frame = u64{audio.packet_index} * stream.track.frames_per_packet;
    i64 end{};
    if (!mp4_audio_frame_time_100ns(audio.timeline, value.first_source_frame, &value.presentation_time_100ns) ||
        !mp4_audio_frame_time_100ns(audio.timeline, value.first_source_frame + stream.track.frames_per_packet, &end) ||
        value.presentation_time_100ns == (-0x7fffffffffffffffll - 1) || end <= value.presentation_time_100ns)
        return E::unsupported;
    // Signed endpoints can straddle zero; subtract in the unsigned domain.
    value.duration_100ns = static_cast<u64>(end) - static_cast<u64>(value.presentation_time_100ns);
    *output = value;
    return E::none;
}
} // namespace
Mp4DemuxError mp4_fragment_audio_refresh(Mp4FragmentAudio* audio) noexcept {
    if (!audio || !audio->source || !mp4_fragment_source_valid(*audio->source) || !audio->packet_ticks) return E::invalid_input;
    auto& source = *audio->source;
    if (!source.segments || !source.segments->live) return E::none;
    if (!mp4_fragment_source_refresh(&source) || source.track.edit_duration || source.track.media_start < 0) return E::unsupported;
    const auto& index = source.index;
    const u64 count = index.duration / audio->packet_ticks + u64{index.duration % audio->packet_ticks != 0};
    if (count < audio->packet_count || count > ~u32{0} || index.first_time > ~u64{0} - index.duration ||
        (!source.segments->ended && index.duration % audio->packet_ticks)) return E::unsupported;
    const u64 end = index.first_time + index.duration;
    if (end <= static_cast<u64>(source.track.media_start)) return E::unsupported;
    audio->packet_count = static_cast<u32>(count);
    audio->timeline.source_frames = count * source.track.frames_per_packet;
    audio->timeline.content_ticks = end - static_cast<u64>(source.track.media_start);
    return E::none;
}
Mp4DemuxError mp4_fragment_audio_peek(Mp4FragmentAudio* audio, Mp4FragmentAudioPacket* output) noexcept {
    if (!audio || !audio->source || !mp4_fragment_source_valid(*audio->source) || !output ||
        !separate(*audio->source, audio, sizeof(*audio), output, sizeof(*output))) return E::invalid_input;
    const auto refreshed = mp4_fragment_audio_refresh(audio);
    if (refreshed != E::none) return refreshed;
    Mp4FragmentPacket packet{};
    const auto error = mp4_fragment_source_peek(audio->source, &packet);
    if (error != E::none)
        return error == E::end_of_stream && audio->packet_index != audio->packet_count ? E::malformed : error;
    Mp4FragmentAudioPacket value{};
    const auto converted = audio_packet(*audio, audio->source->segment, audio->source->fragment, packet, &value);
    if (converted != E::none) return converted;
    audio->pending = true; *output = value;
    return E::none;
}
bool mp4_fragment_audio_commit(Mp4FragmentAudio* audio) noexcept {
    if (!audio || !audio->source || !audio->pending || !mp4_fragment_source_commit(audio->source)) return false;
    ++audio->packet_index; audio->pending = false;
    return true;
}
Mp4DemuxError mp4_fragment_audio_seek(Mp4FragmentAudio* audio, u32 packet) noexcept {
    const auto refreshed = mp4_fragment_audio_refresh(audio);
    if (refreshed != E::none) return refreshed;
    if (packet > audio->packet_count) return E::invalid_input;
    auto& stream = *audio->source;
    if (packet == audio->packet_count) {
        if (!mp4_fragment_source_select(&stream, stream.index.count)) return E::invalid_input;
        audio->packet_index = packet; audio->pending = false;
        return E::none;
    }
    const u64 time = stream.index.first_time + u64{packet} * audio->packet_ticks;
    Mp4FragmentRange range{};
    const auto found = mp4_fragment_source_find(&stream, time>audio->index_edit_ticks?time-audio->index_edit_ticks:0, &range);
    if (found != E::none) return found;
    if(range.index) range.time+=audio->index_edit_ticks;
    if ((time - range.time) % audio->packet_ticks) return E::malformed;
    const u64 local = (time - range.time) / audio->packet_ticks;
    if (local > ~u32{0}) return E::unsupported;
    Mp4FragmentPosition position{};
    const auto error = mp4_fragment_source_prepare(&stream, range.index, static_cast<u32>(local), &position);
    if (error != E::none) return error;
    auto next = *audio; next.packet_index = packet;
    Mp4FragmentAudioPacket verified{};
    const auto converted = audio_packet(next, position.segment, position.fragment, position.packet, &verified);
    if (converted != E::none) { mp4_fragment_source_cancel_prepare(&stream); return converted; }
    if (!mp4_fragment_source_adopt(&stream, position)) return E::data_pending;
    next.pending = true; *audio = next;
    return E::none;
}
} // namespace maicord
#pragma clang attribute pop
