#pragma once

#include "maicord/core/slice.hpp"

namespace maicord {

// Reads exactly output.count bytes at offset; false on any short read.
using AttachmentProbeRead = bool (*)(void* context, u64 offset, Slice<u8> output) noexcept;

// Movie duration from an ISO-BMFF (MP4/MOV/M4V) movie header. Walks only
// box headers (top level, then moov) and reads the mvhd fields, so a
// non-faststart file costs a handful of small reads, never the media data.
bool attachment_mp4_duration(u64 file_bytes, AttachmentProbeRead read, void* context, u32* milliseconds) noexcept;

} // namespace maicord
