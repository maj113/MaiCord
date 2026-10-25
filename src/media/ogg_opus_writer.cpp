#include "maicord/media/ogg_opus_writer.hpp"
#include "maicord/media/ogg_opus.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
constexpr u32 kStageOffset = kOggOpusWriterPageHeaderBytes + 255;
// Page header and lacing at w->used; the body must already follow the lacing.
void seal(OggOpusWriter* w, u8 flags, u64 granule, u32 body) noexcept {
    u8* p = w->output.data + w->used;
    copy_bytes(p, "OggS", 4);
    p[4] = 0;
    p[5] = flags;
    byte_store_le64(p + 6, granule);
    byte_store_le32(p + 14, w->serial);
    byte_store_le32(p + 18, w->sequence++);
    byte_store_le32(p + 22, 0);
    p[26] = w->lacing_count;
    copy_bytes(p + 27, w->lacing, w->lacing_count);
    const u32 bytes = kOggOpusWriterPageHeaderBytes + w->lacing_count + body;
    byte_store_le32(p + 22, ogg_page_checksum({p, bytes}));
    w->used += bytes;
}
// One packet on its own page (the header pages).
bool header_page(OggOpusWriter* w, u8 flags, ConstSlice<u8> body) noexcept {
    const u32 lacing = body.count / 255 + 1;
    if (lacing > 255 || w->output.count - w->used < kOggOpusWriterPageHeaderBytes + lacing + body.count)
        return false;
    w->lacing_count = static_cast<u8>(lacing);
    for (u32 i = 0; i + 1 < lacing; ++i)
        w->lacing[i] = 255;
    w->lacing[lacing - 1] = static_cast<u8>(body.count % 255);
    copy_bytes(w->output.data + w->used + kOggOpusWriterPageHeaderBytes + lacing, body.data, body.count);
    seal(w, flags, 0, body.count);
    w->lacing_count = 0;
    return true;
}
void commit(OggOpusWriter* w, bool end, u64 granule) noexcept {
    // The body was staged after the widest header; close the gap first.
    u8* page = w->output.data + w->used;
    copy_bytes(page + kOggOpusWriterPageHeaderBytes + w->lacing_count, page + kStageOffset, w->staged);
    seal(w, end ? 0x04 : 0, granule, w->staged);
    w->committed_granule = granule;
    w->staged = 0;
    w->lacing_count = w->page_packets = 0;
}
} // namespace

OggOpusWriterError ogg_opus_writer_begin(OggOpusWriter* w, Slice<u8> output, u32 serial, u16 pre_skip,
                                         u32 input_sample_rate, Utf8View vendor) noexcept {
    if (!w || !output.data || (!vendor.data && vendor.bytes) || vendor.bytes > 255)
        return OggOpusWriterError::invalid_input;
    reset_value(w);
    w->output = output;
    w->serial = serial;
    u8 head[19];
    copy_bytes(head, "OpusHead", 8);
    head[8] = 1;  // Version.
    head[9] = 1;  // Mono.
    byte_store_le16(head + 10, pre_skip);
    byte_store_le32(head + 12, input_sample_rate);
    byte_store_le16(head + 16, 0); // Output gain.
    head[18] = 0;        // Channel mapping family 0.
    u8 tags[16 + 255];
    copy_bytes(tags, "OpusTags", 8);
    byte_store_le32(tags + 8, vendor.bytes);
    copy_bytes(tags + 12, vendor.data, vendor.bytes);
    byte_store_le32(tags + 12 + vendor.bytes, 0); // No user comments.
    if (!header_page(w, 0x02, {head, sizeof(head)}) || !header_page(w, 0, {tags, 16 + vendor.bytes})) {
        reset_value(w);
        return OggOpusWriterError::capacity;
    }
    w->started = true;
    return OggOpusWriterError::none;
}

OggOpusWriterError ogg_opus_writer_packet(OggOpusWriter* w, ConstSlice<u8> packet, u32 samples) noexcept {
    if (!w || !w->started || w->finished)
        return OggOpusWriterError::invalid_state;
    if (!packet.data || !packet.count || packet.count > kOggOpusWriterMaximumPacketBytes || !samples ||
        samples > 5760)
        return OggOpusWriterError::invalid_input;
    const u32 lacing = packet.count / 255 + 1;
    const bool flush = w->page_packets == kOggOpusWriterPagePackets || w->lacing_count + lacing > 255;
    // Room for this packet after the open page commits: the committed page
    // shrinks by its unused lacing, never grows.
    const u32 after_flush = flush ? kOggOpusWriterPageHeaderBytes + w->lacing_count + w->staged : 0;
    const u64 needed = u64{w->used} + after_flush + kStageOffset + (flush ? 0 : w->staged) + packet.count;
    if (needed > w->output.count)
        return OggOpusWriterError::capacity;
    if (flush)
        commit(w, false, w->granule);
    copy_bytes(w->output.data + w->used + kStageOffset + w->staged, packet.data, packet.count);
    w->staged += packet.count;
    for (u32 i = 0; i + 1 < lacing; ++i)
        w->lacing[w->lacing_count++] = 255;
    w->lacing[w->lacing_count++] = static_cast<u8>(packet.count % 255);
    ++w->page_packets;
    w->granule += samples;
    return OggOpusWriterError::none;
}

OggOpusWriterError ogg_opus_writer_finish(OggOpusWriter* w, u64 final_granule) noexcept {
    if (!w || !w->started || w->finished || !w->page_packets)
        return OggOpusWriterError::invalid_state;
    if (final_granule <= w->committed_granule || final_granule > w->granule)
        return OggOpusWriterError::invalid_input;
    commit(w, true, final_granule);
    w->finished = true;
    return OggOpusWriterError::none;
}
} // namespace maicord
