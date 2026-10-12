#include "maicord/media/ogg_opus.hpp"

namespace maicord {
namespace {
// RFC 3533 section 6: non-reflected CRC, no initial/final inversion.
struct CrcTable {
    u32 values[256]{};
};
constexpr CrcTable crc_table() noexcept {
    CrcTable table{};
    for (u32 i = 0; i < 256; ++i) {
        u32 value = i << 24;
        for (u32 bit = 0; bit < 8; ++bit)
            value = (value << 1) ^ ((value & 0x80000000u) ? 0x04c11db7u : 0);
        table.values[i] = value;
    }
    return table;
}
constexpr auto kCrc = crc_table();
u16 le16(const u8* p) noexcept {
    return static_cast<u16>(p[0] | (u32{p[1]} << 8));
}
u32 le32(const u8* p) noexcept {
    return p[0] | (u32{p[1]} << 8) | (u32{p[2]} << 16) | (u32{p[3]} << 24);
}
u64 le64(const u8* p) noexcept {
    return le32(p) | (u64{le32(p + 4)} << 32);
}
bool overlaps(const void* a, u64 a_bytes, const void* b, u64 b_bytes) noexcept {
    const auto x = reinterpret_cast<usize>(a), y = reinterpret_cast<usize>(b);
    return a_bytes && b_bytes && (x <= y ? y - x < a_bytes : x - y < b_bytes);
}
bool scratch_valid(ConstSlice<u8> encoded, Slice<u8> scratch, const void* out, u32 out_bytes) noexcept {
    return (!scratch.count || scratch.data) && !overlaps(encoded.data, encoded.count, out, out_bytes) &&
           !overlaps(scratch.data, scratch.count, encoded.data, encoded.count) &&
           !overlaps(scratch.data, scratch.count, out, out_bytes);
}
struct Page {
    u32 offset{}, body{}, end{}, segments{}, serial{}, sequence{};
    u64 granule{};
    u8 flags{};
};
OggOpusError page(ConstSlice<u8> source, u32 offset, Page* out, bool verify_crc,
                  u32 validated_end = 0) noexcept {
    if (offset > source.count || source.count - offset < 27)
        return OggOpusError::truncated;
    const auto* p = source.data + offset;
    if (!bytes_equal(p, "OggS", 4))
        return OggOpusError::malformed;
    if (p[4])
        return OggOpusError::unsupported;
    if (p[5] & ~u8{7})
        return OggOpusError::malformed;
    const u32 segments = p[26];
    if (source.count - offset - 27 < segments)
        return OggOpusError::truncated;
    const u32 body = offset + 27 + segments;
    u32 bytes = 0;
    if (validated_end) {
        if (validated_end < body || validated_end > source.count)
            return OggOpusError::malformed;
        bytes = validated_end - body;
    } else {
        for (u32 i = 0; i < segments; ++i)
            bytes += p[27 + i];
    }
    if (bytes > source.count - body)
        return OggOpusError::truncated;
    if (verify_crc) {
        u32 crc = 0;
        for (u32 i = 0; i < 27 + segments + bytes; ++i) {
            const u8 byte = i >= 22 && i < 26 ? 0 : p[i];
            crc = (crc << 8) ^ kCrc.values[(crc >> 24) ^ byte];
        }
        if (crc != le32(p + 22))
            return OggOpusError::checksum;
    }
    *out = {offset, body, body + bytes, segments, le32(p + 14), le32(p + 18), le64(p + 6), p[5]};
    return OggOpusError::none;
}
// RFC 6716 section 3: duration plus bounded CBR/VBR frame-length validation.
bool frame_size(ConstSlice<u8> bytes, u32* at, u32* size) noexcept {
    if (*at >= bytes.count)
        return false;
    const u32 first = bytes[(*at)++];
    if (first < 252) {
        *size = first;
        return true;
    }
    if (*at >= bytes.count)
        return false;
    *size = first + 4u * bytes[(*at)++];
    return true;
}
bool packet_frames(ConstSlice<u8> bytes, u32* frames) noexcept {
    if (!bytes.data || !bytes.count || bytes.count > kOggOpusMaximumPacketBytes)
        return false;
    const u32 toc = bytes[0], config = toc >> 3, code = toc & 3;
    const u32 per_frame = config >= 16        ? (120u << (config & 3))
                          : config >= 12      ? (480u << (config & 1))
                          : (config & 3) == 3 ? 2880u
                                              : (480u << (config & 3));
    u32 at = 1, count = code == 0 ? 1 : 2, end = bytes.count;
    if (code == 3) {
        if (at >= end)
            return false;
        const u32 control = bytes[at++];
        count = control & 63;
        if (!count || count > 48 || per_frame * count > 5760)
            return false;
        if (control & 64) {
            u32 padding = 0, byte = 255;
            while (byte == 255) {
                if (at >= end)
                    return false;
                byte = bytes[at++];
                const u32 added = byte == 255 ? 254 : byte;
                if (added > end - at || padding > end - at - added)
                    return false;
                padding += added;
            }
            if (padding > end - at)
                return false;
            end -= padding;
        }
        if (control & 128) {
            u32 used = 0;
            for (u32 i = 1; i < count; ++i) {
                u32 size = 0;
                if (!frame_size({bytes.data, end}, &at, &size) || size > 1275 || used > end - at ||
                    size > end - at - used)
                    return false;
                used += size;
            }
            if (used > end - at || end - at - used > 1275)
                return false;
        } else if ((end - at) % count || (end - at) / count > 1275)
            return false;
    } else if (code == 2) {
        u32 first = 0;
        if (!frame_size(bytes, &at, &first) || first > 1275 || first > end - at || end - at - first > 1275)
            return false;
    } else if ((end - at) % count || (end - at) / count > 1275)
        return false;
    if (per_frame * count > 5760)
        return false;
    *frames = per_frame * count;
    return true;
}
OggOpusError head(ConstSlice<u8> bytes, OggOpusDemuxer* d) noexcept {
    if (bytes.count < 19 || !bytes_equal(bytes.data, "OpusHead", 8))
        return OggOpusError::malformed;
    if (bytes[8] >= 16 || bytes[18])
        return OggOpusError::unsupported;
    if (!bytes[9] || bytes[9] > 2)
        return OggOpusError::unsupported;
    if (bytes[8] == 1 && bytes.count != 19)
        return OggOpusError::malformed;
    d->channels = bytes[9];
    d->pre_skip = le16(bytes.data + 10);
    d->input_sample_rate = le32(bytes.data + 12);
    const u16 gain = le16(bytes.data + 16);
    d->output_gain_q8 =
        static_cast<i16>(gain < 32768 ? static_cast<i32>(gain) : static_cast<i32>(gain) - 65536);
    return OggOpusError::none;
}
bool tags(ConstSlice<u8> bytes) noexcept {
    if (bytes.count < 16 || !bytes_equal(bytes.data, "OpusTags", 8))
        return false;
    const u32 vendor = le32(bytes.data + 8);
    if (vendor > bytes.count - 16)
        return false;
    u32 at = 12 + vendor;
    const u32 count = le32(bytes.data + at);
    at += 4;
    if (count > (bytes.count - at) / 4)
        return false;
    for (u32 i = 0; i < count; ++i) {
        if (bytes.count - at < 4)
            return false;
        const u32 size = le32(bytes.data + at);
        at += 4;
        if (size > bytes.count - at)
            return false;
        at += size;
    }
    return true; // RFC 7845 permits trailing binary metadata/padding.
}
OggOpusError read_packet(const OggOpusDemuxer& d, OggOpusCursor* cursor, Slice<u8> scratch,
                         ConstSlice<u8>* bytes) noexcept {
    u32 first = 0, length = 0;
    bool fragmented = false;
    while (cursor->page_offset < d.encoded.count) {
        Page p{};
        // The source was validated in full and remains immutable. Reuse only
        // this page's geometry, while checking its bounded header each time.
        const auto error = page(d.encoded, cursor->page_offset, &p, false, cursor->page_end);
        if (error != OggOpusError::none)
            return error;
        if (cursor->page_end && cursor->page_body != p.body)
            return OggOpusError::malformed;
        if (p.serial != d.serial || cursor->segment_index > p.segments)
            return OggOpusError::malformed;
        cursor->page_body = p.body;
        cursor->page_end = p.end;
        if (!cursor->segment_index)
            cursor->data_offset = p.body;
        if (cursor->data_offset < p.body || cursor->data_offset > p.end)
            return OggOpusError::malformed;
        if (!length)
            first = cursor->data_offset;
        for (; cursor->segment_index < p.segments; ++cursor->segment_index) {
            const u32 lace = d.encoded[p.offset + 27 + cursor->segment_index];
            if (lace > p.end - cursor->data_offset)
                return OggOpusError::malformed;
            if (lace > kOggOpusMaximumPacketBytes - length)
                return OggOpusError::capacity;
            if (fragmented && (!scratch.data || length + lace > scratch.count))
                return OggOpusError::capacity;
            if (fragmented)
                copy_bytes(scratch.data + length, d.encoded.data + cursor->data_offset, lace);
            length += lace;
            cursor->data_offset += lace;
            if (lace < 255) {
                ++cursor->segment_index;
                *bytes = {fragmented ? scratch.data : d.encoded.data + first, length};
                return OggOpusError::none;
            }
        }
        if (length && !fragmented) {
            if (!scratch.data || length > scratch.count)
                return OggOpusError::capacity;
            copy_bytes(scratch.data, d.encoded.data + first, length);
            fragmented = true;
        }
        cursor->page_offset = p.end;
        cursor->page_body = cursor->page_end = 0;
        cursor->segment_index = 0;
        cursor->data_offset = 0;
    }
    return length ? OggOpusError::truncated : OggOpusError::end_of_stream;
}
} // namespace

u32 ogg_page_checksum(ConstSlice<u8> page) noexcept {
    u32 crc = 0;
    for (u32 i = 0; page.data && i < page.count; ++i) {
        const u8 byte = i >= 22 && i < 26 ? 0 : page.data[i];
        crc = (crc << 8) ^ kCrc.values[(crc >> 24) ^ byte];
    }
    return crc;
}

OggOpusError ogg_opus_demux_initialize(OggOpusDemuxer* out, ConstSlice<u8> encoded, Slice<u8> scratch,
                                       OggOpusCancelled cancelled, void* context) noexcept {
    if (!out || !encoded.data || !encoded.count || encoded.count > kMediaMaximumEncodedVideoBytes ||
        !scratch_valid(encoded, scratch, out, sizeof(*out)))
        return OggOpusError::invalid_input;
    OggOpusDemuxer d{};
    d.encoded = encoded;
    u32 offset = 0, sequence = 0, stage = 0, packet_start = 0, packet_bytes = 0;
    u64 previous_granule = 0;
    bool pending = false, fragmented = false, audio_granule = false, ended = false;
    while (offset < encoded.count) {
        if (cancelled && cancelled(context))
            return OggOpusError::cancelled;
        Page p{};
        const auto error = page(encoded, offset, &p, true);
        if (error != OggOpusError::none)
            return error;
        if (!offset) {
            d.serial = p.serial;
            if (p.flags != 2 || p.sequence)
                return OggOpusError::malformed;
        } else if (p.serial != d.serial || (p.flags & 2))
            return OggOpusError::unsupported;
        if (p.sequence != sequence++)
            return OggOpusError::malformed;
        if (((p.flags & 1) != 0) != pending)
            return OggOpusError::malformed;
        if (pending && !fragmented) {
            if (!scratch.data || packet_bytes > scratch.count)
                return OggOpusError::capacity;
            copy_bytes(scratch.data, encoded.data + packet_start, packet_bytes);
            fragmented = true;
        }
        u32 data = p.body, completed = 0, audio_completed = 0;
        for (u32 i = 0; i < p.segments; ++i) {
            const u32 lace = encoded[p.offset + 27 + i];
            if (!pending)
                packet_start = data;
            if (lace > kOggOpusMaximumPacketBytes - packet_bytes)
                return OggOpusError::capacity;
            if (fragmented && (!scratch.data || packet_bytes + lace > scratch.count))
                return OggOpusError::capacity;
            if (fragmented)
                copy_bytes(scratch.data + packet_bytes, encoded.data + data, lace);
            packet_bytes += lace;
            data += lace;
            pending = lace == 255;
            if (pending)
                continue;
            const ConstSlice<u8> bytes{fragmented ? scratch.data : encoded.data + packet_start,
                                       packet_bytes};
            ++completed;
            if (!stage) {
                if (offset || i + 1 != p.segments || completed != 1 || p.granule)
                    return OggOpusError::malformed;
                const auto parsed = head(bytes, &d);
                if (parsed != OggOpusError::none)
                    return parsed;
                stage = 1;
            } else if (stage == 1) {
                if (!tags(bytes) || i + 1 != p.segments || p.granule || (p.flags & 4))
                    return OggOpusError::malformed;
                stage = 2;
                d.first_audio_offset = p.end;
            } else {
                u32 frames = 0;
                if (!packet_frames(bytes, &frames))
                    return OggOpusError::malformed;
                d.decoded_frames += frames;
                ++d.packet_count;
                ++audio_completed;
            }
            packet_bytes = 0;
            fragmented = false;
        }
        if (!completed && p.granule != ~u64{0}) {
            // An empty terminal page can close an already completed stream.
            if (!(p.flags & 4) || pending || !audio_granule || p.granule != previous_granule)
                return OggOpusError::malformed;
        }
        if (audio_completed) {
            if (p.granule > 0x7fffffffffffffffull)
                return OggOpusError::malformed;
            if (!audio_granule) {
                if (p.granule < d.decoded_frames && !(p.flags & 4))
                    return OggOpusError::malformed;
                d.granule_origin = p.granule >= d.decoded_frames ? p.granule - d.decoded_frames : 0;
                audio_granule = true;
            }
            if (d.decoded_frames > 0x7fffffffffffffffull - d.granule_origin)
                return OggOpusError::malformed;
            const u64 expected = d.granule_origin + d.decoded_frames;
            if (p.flags & 4) {
                if (p.granule < previous_granule || p.granule > expected)
                    return OggOpusError::malformed;
            } else if (p.granule != expected)
                return OggOpusError::malformed;
            previous_granule = p.granule;
        }
        if (p.flags & 4) {
            if (stage != 2 || pending || !audio_granule)
                return OggOpusError::truncated;
            if (p.end != encoded.count)
                return OggOpusError::unsupported;
            ended = true;
        }
        offset = p.end;
    }
    if (!ended || stage != 2 || pending || !audio_granule)
        return OggOpusError::truncated;
    if (previous_granule < d.granule_origin || previous_granule - d.granule_origin < d.pre_skip)
        return OggOpusError::malformed;
    d.total_frames = previous_granule - d.granule_origin - d.pre_skip;
    d.ready = true;
    d.cursor.page_offset = d.first_audio_offset;
    *out = d;
    return OggOpusError::none;
}
OggOpusPacket ogg_opus_demux_next(OggOpusDemuxer* d, Slice<u8> scratch) noexcept {
    OggOpusPacket packet{};
    if (!d || !d->ready || !d->encoded.data || !d->encoded.count ||
        d->encoded.count > kMediaMaximumEncodedVideoBytes || d->cursor.decoded_frame > d->decoded_frames ||
        d->cursor.packet_index > d->packet_count || !scratch_valid(d->encoded, scratch, d, sizeof(*d))) {
        packet.error = OggOpusError::invalid_input;
        return packet;
    }
    if (d->cursor.packet_index >= d->packet_count) {
        packet.error = OggOpusError::end_of_stream;
        return packet;
    }
    auto cursor = d->cursor;
    packet.error = read_packet(*d, &cursor, scratch, &packet.bytes);
    if (!packet.ok())
        return packet;
    if (!packet_frames(packet.bytes, &packet.duration_frames)) {
        packet = {};
        packet.error = OggOpusError::malformed;
        return packet;
    }
    packet.decoded_first_frame = cursor.decoded_frame;
    packet.packet_index = cursor.packet_index;
    const u64 end = cursor.decoded_frame + packet.duration_frames;
    const u64 keep_first = maximum(cursor.decoded_frame, u64{d->pre_skip});
    const u64 keep_end = minimum(end, u64{d->pre_skip} + d->total_frames);
    packet.trim_front_frames =
        static_cast<u32>(minimum<u64>(packet.duration_frames, keep_first - cursor.decoded_frame));
    packet.trim_back_frames =
        static_cast<u32>(end - maximum(keep_end, cursor.decoded_frame + packet.trim_front_frames));
    cursor.decoded_frame = end;
    ++cursor.packet_index;
    d->cursor = cursor;
    return packet;
}
void ogg_opus_demux_reset(OggOpusDemuxer* d) noexcept {
    if (d && d->ready) {
        d->cursor = {};
        d->cursor.page_offset = d->first_audio_offset;
    }
}
OggOpusError ogg_opus_demux_seek(OggOpusDemuxer* d, u64 target, OggOpusSeekPoint* out, Slice<u8> scratch,
                                 OggOpusCancelled cancelled, void* context) noexcept {
    if (!d || !d->ready || !out || target > d->total_frames ||
        !scratch_valid(d->encoded, scratch, d, sizeof(*d)) ||
        !scratch_valid(d->encoded, scratch, out, sizeof(*out)) ||
        overlaps(d, sizeof(*d), out, sizeof(*out)))
        return OggOpusError::invalid_input;
    auto candidate = *d;
    ogg_opus_demux_reset(&candidate);
    const u64 raw_target = u64{d->pre_skip} + target;
    const u64 wanted = target < kOggOpusSeekPrerollFrames ? 0 : raw_target - kOggOpusSeekPrerollFrames;
    auto selected = candidate.cursor;
    while (candidate.cursor.packet_index < candidate.packet_count) {
        if (cancelled && cancelled(context))
            return OggOpusError::cancelled;
        const auto before = candidate.cursor;
        const auto packet = ogg_opus_demux_next(&candidate, scratch);
        if (!packet.ok())
            return packet.error;
        if (packet.decoded_first_frame > wanted)
            break;
        selected = before;
        if (candidate.cursor.decoded_frame > wanted)
            break;
    }
    const OggOpusSeekPoint result{selected.decoded_frame, target, raw_target - selected.decoded_frame,
                                  selected.packet_index};
    d->cursor = selected;
    *out = result;
    return OggOpusError::none;
}
} // namespace maicord
