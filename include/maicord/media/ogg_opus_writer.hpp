#pragma once
#include "maicord/core/utf.hpp"

namespace maicord {
// Ogg Opus (RFC 7845) mono stream writer into one caller-owned buffer: the
// OpusHead and OpusTags header pages, then audio pages of at most
// kOggOpusWriterPagePackets packets. Packets never span pages. Nothing is
// allocated; a page is staged in place and committed when the next packet no
// longer fits it, or by finish, which marks it end-of-stream.
inline constexpr u32 kOggOpusWriterPagePackets = 50; // One second of 20 ms packets.
inline constexpr u32 kOggOpusWriterPageHeaderBytes = 27;
inline constexpr u32 kOggOpusWriterMaximumPacketBytes = 1275;
enum class OggOpusWriterError : u8 { none, invalid_input, invalid_state, capacity };
struct OggOpusWriter {
    Slice<u8> output;
    u32 used;        // Committed pages.
    u32 staged;      // Body bytes of the open page, kept after its widest header.
    u32 serial, sequence;
    u64 granule;     // 48 kHz samples through the last staged packet.
    u64 committed_granule;
    u32 packets;
    u16 pre_skip;
    u8 lacing[255];
    u8 lacing_count, page_packets;
    bool started, finished;
};
// Header pages need this plus the vendor string.
inline constexpr u32 kOggOpusWriterHeaderPagesBytes = 2 * (kOggOpusWriterPageHeaderBytes + 1) + 19 + 16;
// Output bytes for `packets` packets of at most `packet_bytes` each, plus the
// in-place staging slack of one page header.
constexpr u64 ogg_opus_writer_capacity(u32 packets, u32 packet_bytes, u32 vendor_bytes) noexcept {
    const u64 lacing = packet_bytes / 255 + 1;
    const u64 pages = (u64{packets} + kOggOpusWriterPagePackets - 1) / kOggOpusWriterPagePackets + 1;
    return kOggOpusWriterHeaderPagesBytes + vendor_bytes + pages * (kOggOpusWriterPageHeaderBytes + 255) +
           u64{packets} * (packet_bytes + lacing);
}
// Writes both header pages. pre_skip is the encoder lookahead at 48 kHz.
OggOpusWriterError ogg_opus_writer_begin(OggOpusWriter*, Slice<u8> output, u32 serial, u16 pre_skip,
                                         u32 input_sample_rate, Utf8View vendor) noexcept;
// One complete packet decoding to `samples` 48 kHz samples. Capacity failure
// leaves the writer unchanged, so finish still closes a valid stream.
OggOpusWriterError ogg_opus_writer_packet(OggOpusWriter*, ConstSlice<u8> packet, u32 samples) noexcept;
// Commits the open page as end-of-stream. final_granule is pre_skip plus the
// audible samples; it trims at most the final page's samples.
OggOpusWriterError ogg_opus_writer_finish(OggOpusWriter*, u64 final_granule) noexcept;
// Bytes still available for packet bodies, keeping room to commit the open
// page and stage one more.
inline u32 ogg_opus_writer_free(const OggOpusWriter& w) noexcept {
    const u64 used = u64{w.used} + w.staged + w.lacing_count + 2 * (kOggOpusWriterPageHeaderBytes + 255);
    return used < w.output.count ? static_cast<u32>(w.output.count - used) : 0;
}
inline ConstSlice<u8> ogg_opus_writer_stream(const OggOpusWriter& writer) noexcept {
    return writer.finished ? ConstSlice<u8>{writer.output.data, writer.used} : ConstSlice<u8>{};
}
} // namespace maicord
