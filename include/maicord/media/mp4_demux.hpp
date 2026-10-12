#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/media/encoded_video_limits.hpp"

namespace maicord {

enum class Mp4DemuxError : u8 {
    none,
    invalid_input,
    malformed,
    unsupported,
    no_video,
    end_of_stream,
    no_audio,
    // Progressive source: the sample payload has not arrived. Cursors are unchanged.
    data_pending,
};

// Progressive sources report whether [offset, offset + bytes) of the encoded
// span has arrived. Null means a complete source. Box headers and moov must be
// resident before initialization; only sample payloads are ever queried.
struct Mp4SourceAvailability {
    bool (*available)(void* context, u64 offset, u64 bytes) noexcept;
    void* context;
};

struct Mp4AccessUnit {
    ConstSlice<u8> bytes;
    u64 decode_time_us;
    u64 presentation_time_us;
    u64 duration_us;
    u32 sample_index;
    bool sync;
    Mp4DemuxError error;
    // Signed movie timeline after supported edit-list mapping. Negative values
    // are decoder preroll, never permission to play before the movie starts.
    i64 presentation_time_us_signed;
    u64 decode_time_ticks;
    i64 presentation_time_ticks;
    u32 duration_ticks;

    constexpr bool ok() const noexcept {
        return error == Mp4DemuxError::none;
    }
};

// Sequential ISO-BMFF H.264 reader. Table entries stay in
// the encoded buffer and are consumed by compact cursors; no sample
// index or per-frame allocation is built.
struct Mp4Demuxer {
    ConstSlice<u8> encoded;
    u32 timescale;
    u32 width;
    u32 height;
    // Optional VisualSampleEntry pasp. Zero/zero means absent; a present
    // ratio is positive and takes precedence over the codec's VUI ratio.
    u32 sar_width;
    u32 sar_height;
    u32 sample_count;
    u32 sample_index;
    u32 avcc_offset;
    u32 avcc_bytes;
    u32 stts_offset;
    u32 stts_count;
    u32 ctts_offset;
    u32 ctts_count;
    u32 stsc_offset;
    u32 stsc_count;
    u32 stsz_offset;
    u32 constant_sample_bytes;
    u32 chunk_offset_table;
    u32 chunk_count;
    u32 stss_offset;
    u32 stss_count;
    u64 decode_time;
    u64 chunk_byte_offset;
    u32 chunk_index;
    u32 samples_left_in_chunk;
    u32 stsc_index;
    u32 stts_index;
    u32 stts_left;
    u32 stts_delta;
    u32 ctts_index;
    u32 ctts_left;
    i64 ctts_delta;
    u32 stss_index;
    u8 nal_length_bytes;
    u8 ctts_version;
    bool chunk_offsets_64;
    bool ready;
    bool sync_table_present;
    bool has_edit_list;
    // Track header display matrix as clockwise quarter turns (portrait camera
    // recordings: 1). Mirrored, scaled or sheared matrices stay unrotated.
    u8 rotation;
    u64 duration_us;
    u64 media_duration_ticks;
    u64 empty_edit_us;
    i64 media_start_ticks;
    u32 maximum_sample_bytes;
    u64 mdat_cursor;
    u64 mdat_begin;
    u64 mdat_end;
    // Edit-list timebase and bounds in exact ticks; the *_us fields are
    // rounded. Without edits they use the media timescale and the presentation
    // endpoint.
    u32 movie_timescale;
    u64 empty_edit_ticks;
    u64 movie_duration_ticks;
    Mp4SourceAvailability availability;
};

inline constexpr u32 kMp4DemuxMaximumEncodedBytes = kMediaMaximumEncodedVideoBytes;
// Clockwise quarter turns of a tkhd matrix {a b u, c d v, x y w}, where a
// source point maps to (a*x + c*y + tx, b*x + d*y + ty). Scale is ignored;
// mirrors and shears count as unrotated.
constexpr u8 mp4_matrix_rotation(i32 a, i32 b, i32 c, i32 d) noexcept {
    if (!b && !c)
        return a < 0 && d < 0 ? 2 : 0;
    if (!a && !d)
        return b > 0 && c < 0 ? 1 : b < 0 && c > 0 ? 3 : 0;
    return 0;
}
struct Mp4SeekPoint {
    u32 sample_index;
    u64 decode_time_us;
    i64 presentation_time_us;
};

// A non-null availability admits a sparse source up to the progressive limit.
Mp4DemuxError mp4_demux_initialize(Mp4Demuxer* demuxer, ConstSlice<u8> encoded,
                                   Mp4SourceAvailability availability = {}) noexcept;
void mp4_demux_reset(Mp4Demuxer* demuxer) noexcept;
// Table walk only: the returned bytes are not checked against availability.
Mp4AccessUnit mp4_demux_next(Mp4Demuxer* demuxer) noexcept;
// Payload readers check here before touching unit bytes.
bool mp4_demux_available(const Mp4Demuxer& demuxer, ConstSlice<u8> bytes) noexcept;
// mp4_demux_next plus availability. data_pending leaves the cursor unchanged
// and still reports the pending unit's bytes.
Mp4AccessUnit mp4_demux_next_ready(Mp4Demuxer* demuxer) noexcept;
ConstSlice<u8> mp4_demux_avcc(const Mp4Demuxer& demuxer) noexcept;
// Transactional cursor operations: failure preserves the caller's cursor and
// output. Sync lookup also validates an IDR NAL; stss alone is not a decoder
// reference-reset guarantee. The caller still decodes/discards seek preroll.
// A progressive source returns data_pending until the chosen IDR has arrived;
// the availability context observes that sample's range.
Mp4DemuxError mp4_demux_seek_sample(Mp4Demuxer*, u32 sample_index) noexcept;
Mp4DemuxError mp4_demux_seek_sync(Mp4Demuxer*, i64 movie_time_us, Mp4SeekPoint*) noexcept;

} // namespace maicord
