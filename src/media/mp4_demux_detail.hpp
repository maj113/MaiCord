#pragma once
#include "maicord/media/mp4_demux.hpp"

// Private bounded box/table walker shared by the video and audio cursors. It
// owns no storage and admits no codec.
namespace maicord::mp4_demux_detail {
struct Box { u32 type; u64 begin, payload, end; };
constexpr u32 fourcc(char a,char b,char c,char d) noexcept {
    return (u32{static_cast<u8>(a)}<<24)|(u32{static_cast<u8>(b)}<<16)|
           (u32{static_cast<u8>(c)}<<8)|u32{static_cast<u8>(d)};
}
u32 read_u32(const u8*) noexcept;
u64 read_u64(const u8*) noexcept;
bool next_box(ConstSlice<u8>,u64*,u64,Box*) noexcept;
bool find_box(ConstSlice<u8>,u64,u64,u32,Box*) noexcept;
// QuickTime may close an atom list (sample entry extensions, 'wave') with a
// 32-bit zero instead of another atom.
bool atom_list_end(ConstSlice<u8>,u64 cursor,u64 end) noexcept;
// First sample description of an stsd box. Several are legal; the stsc check
// requires every sample to reference this one.
bool first_description(ConstSlice<u8>,const Box& stsd,Box* entry) noexcept;
// The track read for a handler. Several may share one (a Cinematic-mode
// disparity track, a spatial-audio alternate): prefer a decodable first sample
// description, then an enabled track header, then file order.
Mp4DemuxError select_track(ConstSlice<u8>,const Box& moov,u32 handler,Box* track) noexcept;
u64 scale_us(u64,u32) noexcept;
Mp4DemuxError initialize_tables(Mp4Demuxer*,ConstSlice<u8>,u32 handler,Box* description,
                                Mp4SourceAvailability availability={}) noexcept;
// Codec-specific random-access classification of one resident sync sample.
enum class SyncKind : u8 { random_access, other, malformed, unsupported };
using SyncClassifier = SyncKind (*)(ConstSlice<u8> unit, u8 nal_length_bytes) noexcept;
// Shared seek scan; callers validate their own arguments first. A complete
// source classifies every candidate. A progressive source classifies lazily
// from the latest candidate and reports data_pending for a missing payload.
Mp4DemuxError seek_sync(Mp4Demuxer*,i64 target,Mp4SeekPoint*,SyncClassifier) noexcept;
}
