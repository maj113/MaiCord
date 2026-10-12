#pragma once
#include "maicord/media/mp4_demux.hpp"
namespace maicord {
// Codec selection reads bounded box metadata only, before a native source copy.
bool hevc_container_present(ConstSlice<u8>) noexcept;
Mp4DemuxError hevc_demux_initialize(Mp4Demuxer*, ConstSlice<u8>, Mp4SourceAvailability = {}) noexcept;
ConstSlice<u8> hevc_demux_hvcc(const Mp4Demuxer&) noexcept;
// Shared immutable sample cursors use mp4_demux_next/reset/seek_sample.
// Random access requires a real IDR picture, not merely an stss/CRA marker.
Mp4DemuxError hevc_demux_seek_sync(Mp4Demuxer*, i64 movie_time_us, Mp4SeekPoint*) noexcept;
} // namespace maicord
