#include "maicord/media/containers/webm_opus.hpp"
#include "maicord/core/memory.hpp"
// Container parsing, a few elements for each packet and no byte loop: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)

namespace maicord {
namespace {
using E = Mp4DemuxError;
enum Id : u32 {
    kEbml = 0x1A45DFA3, kSegment = 0x18538067, kInfo = 0x1549A966, kScale = 0x2AD7B1, kTracks = 0x1654AE6B,
    kEntry = 0xAE, kNumber = 0xD7, kCodec = 0x86, kPrivate = 0x63A2, kDelay = 0x56AA, kCues = 0x1C53BB6B,
    kPoint = 0xBB, kCueTime = 0xB3, kCuePositions = 0xB7, kCueCluster = 0xF1, kCluster = 0x1F43B675,
    kTimestamp = 0xE7, kSimpleBlock = 0xA3, kBlockGroup = 0xA0, kBlock = 0xA1, kDiscard = 0x75A2,
};
constexpr u32 kMaximumPacketBytes = 61'440;
// An element: where it starts, and its content [data, end).
struct Element {
    u32 id;
    u64 start, data, end;
};

bool present(const WebmOpus& w, u64 offset, u64 bytes) noexcept {
    return !w.availability.available || w.availability.available(w.availability.context, offset, bytes);
}
// Reads the header of the element at `at`. The element must end at or before `limit`.
E element(const WebmOpus& w, u64 at, u64 limit, Element* out) noexcept {
    if (at >= limit || limit > w.encoded.count)
        return E::malformed;
    // An identifier has at most 4 bytes and a size has at most 8.
    const u32 room = static_cast<u32>(minimum<u64>(12, limit - at));
    if (!present(w, at, room))
        return E::data_pending;
    const u8* p = w.encoded.data + at;
    u32 id_bytes = 1, size_bytes = 1;
    while (id_bytes <= 4 && !(p[0] & (0x80u >> (id_bytes - 1))))
        ++id_bytes;
    if (id_bytes > 4 || id_bytes >= room)
        return E::malformed;
    const u32 first = p[id_bytes];
    while (size_bytes <= 8 && !(first & (0x80u >> (size_bytes - 1))))
        ++size_bytes;
    if (size_bytes > 8 || id_bytes + size_bytes > room)
        return E::malformed;
    u32 id = 0;
    for (u32 i = 0; i < id_bytes; ++i)
        id = id << 8 | p[i];
    u64 size = first & (0xffu >> size_bytes);
    bool unknown = size == (0xffu >> size_bytes);
    for (u32 i = 1; i < size_bytes; ++i) {
        size = size << 8 | p[id_bytes + i];
        unknown = unknown && p[id_bytes + i] == 0xff;
    }
    const u64 data = at + id_bytes + size_bytes;
    if (unknown) {
        // A file that a muxer could not seek has a Segment of unknown size. Any other such element is a live file.
        if (id != kSegment)
            return E::unsupported;
        size = limit - data;
    }
    if (size > limit - data)
        return E::malformed;
    *out = {id, at, data, data + size};
    return E::none;
}
// The first child of `parent` with the identifier. end_of_stream without one.
E child(const WebmOpus& w, const Element& parent, u32 id, Element* out) noexcept {
    for (u64 at = parent.data; at < parent.end; at = out->end) {
        const E error = element(w, at, parent.end, out);
        if (error != E::none)
            return error;
        if (out->id == id)
            return E::none;
    }
    return E::end_of_stream;
}
// An unsigned integer of 1 to 8 bytes. Its bytes must be present.
bool number(const WebmOpus& w, const Element& e, u64* out) noexcept {
    const u64 bytes = e.end - e.data;
    if (!bytes || bytes > 8)
        return false;
    u64 value = 0;
    for (u64 i = 0; i < bytes; ++i)
        value = value << 8 | w.encoded.data[e.data + i];
    *out = value;
    return true;
}
// The frames of one Opus packet, from its first byte (RFC 6716 section 3.1).
u32 packet_frames(ConstSlice<u8> packet) noexcept {
    const u32 toc = packet[0], config = toc >> 3, code = toc & 3;
    const u32 one = config >= 16        ? 120u << (config & 3)
                    : config >= 12      ? 480u << (config & 1)
                    : (config & 3) == 3 ? 2880u
                                        : 480u << (config & 3);
    return one * (code == 0 ? 1u : code < 3 ? 2u : packet.count > 1 ? packet[1] & 63u : 0u);
}
// The 48 kHz frames in a count of nanoseconds, to the nearest frame.
u64 frames_in(u64 nanoseconds) noexcept {
    return (minimum<u64>(nanoseconds, u64{1} << 58) * 48 + 500'000) / 1'000'000;
}
// The packet that starts at a timestamp. A muxer rounds a timestamp to its unit and can add the rounded codec
// delay, so the timestamp can be late by a millisecond or two. The packet grid is far coarser.
u32 packet_at(const WebmOpus& w, u64 ticks) noexcept {
    const u64 frames = frames_in(minimum<u64>(ticks, ~u32{0}) * w.scale);
    return w.frames_per_packet
               ? static_cast<u32>(minimum<u64>((frames + w.frames_per_packet / 2) / w.frames_per_packet, ~u32{0}))
               : 0;
}
// The next Cluster at or after `at`, and the index of its first packet. end_of_stream after the last Cluster.
E cluster(const WebmOpus& w, u64 at, Element* out, u32* first_packet) noexcept {
    for (;; at = out->end) {
        if (at >= w.clusters_end)
            return E::end_of_stream;
        const E error = element(w, at, w.clusters_end, out);
        if (error != E::none)
            return error;
        if (out->id == kCluster)
            break;
    }
    Element time{};
    u64 ticks = 0;
    const E error = child(w, *out, kTimestamp, &time);
    if (error != E::none)
        return error == E::end_of_stream ? E::malformed : error;
    if (!present(w, time.data, time.end - time.data))
        return E::data_pending;
    if (!number(w, time, &ticks))
        return E::malformed;
    *first_packet = packet_at(w, ticks);
    return E::none;
}
// The next packet of the track in [*position, end): a SimpleBlock, or the Block of a BlockGroup. `discard` receives
// the DiscardPadding of a group, in nanoseconds. end_of_stream at the end of the range.
E block(const WebmOpus& w, u64* position, u64 end, ConstSlice<u8>* packet, u64* discard = nullptr) noexcept {
    for (u64 at = *position; at < end;) {
        Element e{};
        E error = element(w, at, end, &e);
        if (error != E::none)
            return error;
        at = e.end;
        if (e.id != kSimpleBlock && e.id != kBlockGroup)
            continue;
        if (!present(w, e.data, e.end - e.data))
            return E::data_pending;
        if (e.id == kBlockGroup) {
            Element padding{};
            u64 value = 0;
            // A negative padding has its first bit set. It adds no frames here.
            if (discard && child(w, e, kDiscard, &padding) == E::none && number(w, padding, &value) &&
                !(w.encoded.data[padding.data] & 0x80))
                *discard = value;
            Element inner{};
            error = child(w, e, kBlock, &inner);
            if (error == E::end_of_stream)
                continue;
            if (error != E::none)
                return error;
            e = inner;
        }
        // The track number (one byte below track 127), a 16-bit time and the flags.
        const u8* p = w.encoded.data + e.data;
        const u64 bytes = e.end - e.data;
        if (bytes < 5 || bytes - 4 > kMaximumPacketBytes)
            return E::malformed;
        if (p[0] != (0x80u | w.track))
            continue;
        if (p[3] & 0x06)
            return E::unsupported; // laced frames
        *packet = {p + 4, static_cast<u32>(bytes - 4)};
        *position = at;
        return E::none;
    }
    *position = end;
    return E::end_of_stream;
}
// The next cue point at or after *at: the offset of its Cluster and the index of that Cluster's first packet.
E cue(const WebmOpus& w, u64* at, u64* offset, u32* packet) noexcept {
    while (*at < w.cues_end) {
        Element point{};
        const E error = element(w, *at, w.cues_end, &point);
        if (error != E::none)
            return error;
        *at = point.end;
        if (point.id != kPoint)
            continue;
        Element time{}, positions{}, position{};
        u64 ticks = 0, relative = 0;
        if (child(w, point, kCueTime, &time) != E::none || !number(w, time, &ticks) ||
            child(w, point, kCuePositions, &positions) != E::none ||
            child(w, positions, kCueCluster, &position) != E::none || !number(w, position, &relative) ||
            relative >= w.clusters_end - w.segment)
            return E::malformed;
        *offset = w.segment + relative;
        *packet = packet_at(w, ticks);
        return E::none;
    }
    return E::end_of_stream;
}
// The offset of the last Cluster that starts at or before `packet`: from the Cues, or from the Cluster headers.
E locate(const WebmOpus& w, u32 packet, u64* offset) noexcept {
    u64 found = w.clusters, at = w.cues, next = 0;
    u32 first = 0;
    E error = E::none;
    if (w.cues) {
        if (!present(w, w.cues, w.cues_end - w.cues))
            return E::data_pending;
        while ((error = cue(w, &at, &next, &first)) == E::none && first <= packet)
            found = next;
    } else {
        Element c{};
        for (at = w.clusters; (error = cluster(w, at, &c, &first)) == E::none && first <= packet; at = c.end)
            found = c.start;
    }
    if (error != E::none && error != E::end_of_stream)
        return error;
    *offset = found;
    return E::none;
}
// Counts the packets from the last known Cluster to the end of the file.
E tail(WebmOpus& w, u64* discard) noexcept {
    u64 at = 0;
    E error = locate(w, ~u32{0}, &at);
    u32 count = 0;
    for (Element c{}; error == E::none; at = c.end) {
        u32 first = 0;
        error = cluster(w, at, &c, &first);
        if (error != E::none)
            break;
        ConstSlice<u8> packet{};
        u32 packets = 0;
        for (u64 position = c.data; (error = block(w, &position, c.end, &packet, discard)) == E::none;)
            ++packets;
        if (error != E::end_of_stream)
            return error;
        if (first > ~u32{0} - packets)
            return E::malformed;
        count = first + packets;
        error = E::none;
    }
    if (error != E::end_of_stream)
        return error;
    w.packet_count = count;
    return count ? E::none : E::malformed;
}
} // namespace

bool webm_present(ConstSlice<u8> head) noexcept {
    return head.count >= 4 && head[0] == 0x1A && head[1] == 0x45 && head[2] == 0xDF && head[3] == 0xA3;
}

Mp4DemuxError webm_opus_initialize(WebmOpus* output, ConstSlice<u8> encoded, Mp4SourceAvailability availability) noexcept {
    if (!output || output->encoded.data || !encoded.data || encoded.count < 4)
        return E::invalid_input;
    WebmOpus w{};
    w.encoded = encoded;
    w.availability = availability;
    w.scale = 1'000'000;
    Element e{};
    E error = element(w, 0, encoded.count, &e);
    if (error == E::none && e.id == kEbml)
        error = element(w, e.end, encoded.count, &e);
    if (error != E::none)
        return error;
    if (e.id != kSegment)
        return E::unsupported;
    w.segment = e.data;
    w.clusters_end = e.end;
    const u8* head = nullptr;
    u64 delay = ~u64{0};
    for (u64 at = e.data; !w.clusters;) {
        Element top{};
        if ((error = element(w, at, w.clusters_end, &top)) != E::none)
            return error;
        at = top.end;
        if (top.id == kCluster) {
            w.clusters = top.start;
        } else if (top.id == kInfo || top.id == kTracks || top.id == kCues) {
            if (!present(w, top.data, top.end - top.data))
                return E::data_pending;
            Element found{};
            u64 value = 0;
            if (top.id == kCues) {
                w.cues = top.data;
                w.cues_end = top.end;
            } else if (top.id == kInfo) {
                if (child(w, top, kScale, &found) == E::none) {
                    if (!number(w, found, &value) || !value || value > 1'000'000'000)
                        return E::unsupported;
                    w.scale = static_cast<u32>(value);
                }
            } else {
                // The first Opus entry of Tracks. Any other entry is not played.
                for (u64 entry_at = top.data; entry_at < top.end && !head;) {
                    Element entry{};
                    if ((error = element(w, entry_at, top.end, &entry)) != E::none)
                        return error;
                    entry_at = entry.end;
                    if (entry.id != kEntry || child(w, entry, kCodec, &found) != E::none || found.end - found.data != 6 ||
                        !bytes_equal(encoded.data + found.data, "A_OPUS", 6))
                        continue;
                    if (child(w, entry, kNumber, &found) != E::none || !number(w, found, &value) || !value || value > 126 ||
                        child(w, entry, kPrivate, &found) != E::none || found.end - found.data < 19)
                        return E::unsupported;
                    w.track = static_cast<u8>(value);
                    head = encoded.data + found.data;
                    if (child(w, entry, kDelay, &found) == E::none)
                        number(w, found, &delay);
                }
            }
        }
    }
    // The identification header of RFC 7845: one or two channels in mapping family 0.
    if (!head || !bytes_equal(head, "OpusHead", 8) || head[8] > 15 || !head[9] || head[9] > 2 || head[18])
        return E::unsupported;
    w.channels = head[9];
    w.output_gain_q8 = static_cast<i16>(head[16] | head[17] << 8);
    const u64 pre_skip = delay != ~u64{0} ? frames_in(delay) : u64{head[10]} | u64{head[11]} << 8;
    // The length of the first packet is the length of every packet.
    u32 first = 0;
    ConstSlice<u8> packet{};
    if ((error = cluster(w, w.clusters, &e, &first)) == E::none) {
        u64 position = e.data;
        error = block(w, &position, e.end, &packet);
    }
    if (error != E::none)
        return error == E::end_of_stream ? E::malformed : error;
    // The audio pump holds one packet of at most 40 ms in a block.
    const u32 frames = packet_frames(packet);
    if (!frames || frames > kWebmOpusMaximumPacketFrames)
        return E::unsupported;
    w.frames_per_packet = static_cast<u16>(frames);
    u64 discard = 0;
    if ((error = tail(w, &discard)) != E::none)
        return error;
    const u64 decoded = u64{w.packet_count} * frames, padding = frames_in(discard);
    if (pre_skip > 0xffff || padding > decoded || pre_skip >= decoded - padding)
        return E::unsupported;
    w.pre_skip = static_cast<u16>(pre_skip);
    w.timeline = {decoded, 0, pre_skip, 0, decoded - padding - pre_skip, 48000, 48000, 48000, 48000};
    *output = w;
    return E::none;
}

Mp4DemuxError webm_opus_peek(WebmOpus* w, ConstSlice<u8>* packet) noexcept {
    if (!w || !w->encoded.data || !packet)
        return E::invalid_input;
    if (w->packet_index >= w->packet_count)
        return E::end_of_stream;
    u64 position = w->position, end = w->cluster_end;
    for (;;) {
        if (position >= end) {
            // The next Cluster must continue the grid at this packet.
            Element c{};
            u32 first = 0;
            const E error = cluster(*w, end ? end : w->clusters, &c, &first);
            if (error != E::none)
                return error == E::end_of_stream ? E::malformed : error;
            if (first != w->packet_index)
                return E::malformed;
            position = c.data;
            end = c.end;
        }
        u64 after = position;
        ConstSlice<u8> found{};
        const E error = block(*w, &after, end, &found);
        if (error == E::end_of_stream) {
            position = end;
            continue;
        }
        if (error != E::none)
            return error;
        if (packet_frames(found) != w->frames_per_packet)
            return E::unsupported;
        w->position = position;
        w->cluster_end = end;
        w->next = after;
        *packet = found;
        return E::none;
    }
}

void webm_opus_commit(WebmOpus* w) noexcept {
    if (w && w->next > w->position && w->packet_index < w->packet_count) {
        w->position = w->next;
        ++w->packet_index;
    }
}

Mp4DemuxError webm_opus_seek(WebmOpus* w, u32 packet) noexcept {
    if (!w || !w->encoded.data || packet > w->packet_count)
        return E::invalid_input;
    u64 at = 0, position = 0, end = 0;
    if (packet < w->packet_count) {
        E error = locate(*w, packet, &at);
        for (u32 index = ~u32{0}; error == E::none && index != packet; at = end) {
            Element c{};
            u32 first = 0;
            if ((error = cluster(*w, at, &c, &first)) != E::none)
                break;
            // A Cluster after the first one must continue where the Cluster before it stopped.
            if (first > packet || (index != ~u32{0} && first != index))
                return E::malformed;
            position = c.data;
            end = c.end;
            ConstSlice<u8> skipped{};
            for (index = first; index < packet && (error = block(*w, &position, end, &skipped)) == E::none;)
                ++index;
            if (error == E::end_of_stream)
                error = E::none;
        }
        if (error != E::none)
            return error == E::end_of_stream ? E::malformed : error;
    }
    w->position = w->next = position;
    w->cluster_end = end;
    w->packet_index = packet;
    return E::none;
}

bool webm_opus_next_cluster(const WebmOpus& w, u64* cursor, WebmOpusCluster* out) noexcept {
    if (!w.cues || !cursor || !out)
        return false;
    u64 at = *cursor ? *cursor : w.cues, offset = 0, next_offset = w.clusters_end;
    u32 first = 0, next_first = w.packet_count;
    // The owner keeps the head of the file resident, so these reads ask for nothing.
    WebmOpus head = w;
    head.availability = {};
    if (cue(head, &at, &offset, &first) != E::none)
        return false;
    *cursor = at;
    cue(head, &at, &next_offset, &next_first);
    if (next_offset <= offset || next_first < first)
        return false;
    *out = {offset, next_offset - offset, first, next_first};
    return true;
}

} // namespace maicord
#pragma clang attribute pop
