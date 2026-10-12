#include "maicord/app/local_video_poster.hpp"

#include "maicord/media/hevc_mp4_demux.hpp"

namespace maicord {

LocalVideoPosterStep local_video_poster_read(ProgressiveSource* source, ProgressiveRange range,
                                             LocalVideoPosterReader* reader) noexcept {
    if (!source || !source->base || !reader || !reader->read || !reader->scratch || !range.bytes ||
        range.first >= source->total_bytes)
        return LocalVideoPosterStep::malformed;
    constexpr u64 block = kProgressiveBlockBytes;
    const u64 end = minimum<u64>(range.first + range.bytes, source->total_bytes);
    for (u64 first = range.first / block * block; first < end; first += block) {
        const u32 bytes = static_cast<u32>(minimum<u64>(block, source->total_bytes - first));
        if (progressive_source_available(*source, first, bytes))
            continue;
        if (bytes > reader->budget)
            return LocalVideoPosterStep::budget;
        if (!reader->read(reader->context, first, {reader->scratch, bytes}))
            return LocalVideoPosterStep::read_failed;
        reader->budget -= bytes;
        reader->bytes_read += bytes;
        ++reader->reads;
        if (!progressive_source_write(source, first, {reader->scratch, bytes}))
            return LocalVideoPosterStep::read_failed;
    }
    return LocalVideoPosterStep::ready;
}

LocalVideoPosterStep local_video_poster_metadata(ProgressiveSource* source, LocalVideoPosterReader* reader) noexcept {
    // Each probe step either finishes or names one missing range; the box
    // count and the 16 MiB moov bound keep this loop finite.
    constexpr u32 steps = 2 * kProgressiveMaximumTopLevelBoxes +
                          kProgressiveMaximumMetadataBytes / kLocalVideoPosterMetadataStep + 2;
    for (u32 step = 0; step < steps; ++step) {
        const auto probe = progressive_mp4_probe(source);
        if (probe.state == ProgressiveProbeState::ready)
            return LocalVideoPosterStep::ready;
        if (probe.state == ProgressiveProbeState::unsupported)
            return LocalVideoPosterStep::unsupported;
        if (probe.state != ProgressiveProbeState::need_data)
            return LocalVideoPosterStep::malformed;
        auto need = probe.need;
        need.bytes = minimum(need.bytes, kLocalVideoPosterMetadataStep);
        const auto read = local_video_poster_read(source, need, reader);
        if (read != LocalVideoPosterStep::ready)
            return read;
    }
    return LocalVideoPosterStep::malformed;
}

bool local_video_poster_service(ProgressiveSource* source, LocalVideoPosterReader* reader,
                                LocalVideoPosterStep* step) noexcept {
    if (!source || !reader || !step)
        return false;
    // One sparse source, one video reader: the refused range is the next need.
    const auto plan = progressive_schedule(*source, {}, false);
    if (!plan.submit || !plan.request.bytes) {
        *step = LocalVideoPosterStep::malformed;
        return false;
    }
    *step = local_video_poster_read(source, plan.request, reader);
    return *step == LocalVideoPosterStep::ready;
}

LocalVideoPosterStep local_video_poster_prepare(ProgressiveSource* source, LocalVideoPosterReader* reader) noexcept {
    const auto metadata = local_video_poster_metadata(source, reader);
    if (metadata != LocalVideoPosterStep::ready)
        return metadata;
    const auto bytes = progressive_source_bytes(*source);
    const auto availability = progressive_source_reader(source, ProgressiveTrack::video);
    Mp4Demuxer demuxer{};
    const auto error = hevc_container_present(bytes) ? hevc_demux_initialize(&demuxer, bytes, availability)
                                                     : mp4_demux_initialize(&demuxer, bytes, availability);
    if (error == Mp4DemuxError::no_video || error == Mp4DemuxError::unsupported)
        return LocalVideoPosterStep::unsupported;
    if (error != Mp4DemuxError::none)
        return LocalVideoPosterStep::malformed;
    // A data_pending result preserves the cursor. Service only fills absent blocks.
    for (u32 attempt = 0; attempt < 8; ++attempt) {
        const auto unit = mp4_demux_next_ready(&demuxer);
        if (unit.ok())
            return LocalVideoPosterStep::ready;
        if (unit.error != Mp4DemuxError::data_pending)
            return unit.error == Mp4DemuxError::unsupported ? LocalVideoPosterStep::unsupported
                                                            : LocalVideoPosterStep::malformed;
        LocalVideoPosterStep step{};
        if (!local_video_poster_service(source, reader, &step))
            return step;
    }
    return LocalVideoPosterStep::malformed;
}

} // namespace maicord
