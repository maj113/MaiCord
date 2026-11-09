#pragma once
#include "maicord/protocol/voice/dave_frame.hpp"

namespace maicord {
// Sender side of DAVE's codec handling: which bytes of a video frame stay
// unencrypted so the RTP packetizer and depacketizer can still read them,
// and the frame form those ranges are measured over (what the receiver's
// depacketizer rebuilds, rtp_video_assembly.hpp):
//   H.264, H.265: every NAL unit behind a 4-byte start code. The start codes
//     and non-VCL units stay clear; a VCL unit keeps its NAL header clear
//     (H.264 also the slice header through pic_parameter_set_id).
//   AV1: temporal delimiter, tile list and padding OBUs are dropped; each OBU
//     keeps its header (and extension) clear, every OBU but the last a
//     minimal LEB128 obu_size (clear), and the last OBU loses its size field
//     so the DAVE trailer can follow its payload.
enum class DaveVideoCodec : u8 {
    h264,
    h265,
    av1
};
struct DavePreparedFrame {
    u32 bytes, range_count; // bytes 0: malformed, too many ranges or no room.
};
// frame: Annex B (any start-code length) or AV1 OBUs with obu_size (the last
// may lack it). output must not overlap frame; frame.count + frame.count / 4
// + 4 bytes is always enough. Ranges are ordered, merged and non-empty.
DavePreparedFrame dave_frame_prepare(DaveVideoCodec codec, ConstSlice<u8> frame, Slice<u8> output,
                                     Slice<DaveClearRange> ranges) noexcept;
// H.264/H.265 senders retry a sealed frame with the next nonce until this
// holds: no start code prefix (00 00 01) in any encrypted span widened by two
// bytes on each side, nor in the trailer (from two bytes before it to the end).
bool dave_frame_start_code_free(ConstSlice<u8> sealed, ConstSlice<DaveClearRange> ranges) noexcept;
} // namespace maicord
