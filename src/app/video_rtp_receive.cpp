#include "maicord/app/video_rtp_receive.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/media/rtp_av1.hpp"
#include "maicord/media/rtp_h264.hpp"
#include "maicord/media/rtp_h265.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
static_assert((kVideoRtpPacketSlots & (kVideoRtpPacketSlots - 1)) == 0);
static_assert(kVideoRtpPacketBytes <= 0xffff && kVideoRtpMaxNackGap < kVideoRtpPacketSlots);
// Extended sequences start at 2^32 + first so a reordered predecessor never
// underflows; the RR field is the low 32 bits relative to that base.
constexpr u64 kExtensionBase = u64{1} << 32;
constexpr u32 kInitialSlots = 256; // A stream's window starts here and doubles up to kVideoRtpPacketSlots.
constexpr u32 kLateRestart = 128; // Consecutive late packets: the sender restarted.
enum SlotState : u8 {
    empty,
    missing,
    present,
    padding
};
constexpr usize window_bytes(u32 slots) noexcept {
    return (sizeof(VideoRtpSlot) + kVideoRtpPacketBytes) * usize{slots};
}
VideoRtpSlot& slot_at(VideoRtpStream& stream, u64 sequence) noexcept {
    return stream.slots[sequence & (stream.capacity - 1)];
}
u8* payload_at(VideoRtpStream& stream, u64 sequence) noexcept {
    return stream.payloads + (sequence & (stream.capacity - 1)) * kVideoRtpPacketBytes;
}
const VideoRtpCodecBinding* media_codec(const VideoRtpReceiver& receiver, u8 type) noexcept {
    for (u32 i = 0; i != receiver.codec_count; ++i)
        if (receiver.codecs[i].payload_type == type)
            return &receiver.codecs[i];
    return nullptr;
}
const VideoRtpCodecBinding* rtx_codec(const VideoRtpReceiver& receiver, u8 type) noexcept {
    for (u32 i = 0; i != receiver.codec_count; ++i)
        if (receiver.codecs[i].rtx_payload_type && receiver.codecs[i].rtx_payload_type == type)
            return &receiver.codecs[i];
    return nullptr;
}
bool starts_access_unit(VideoRtpCodec codec, ConstSlice<u8> payload) noexcept {
    return codec == VideoRtpCodec::h264   ? rtp_h264_starts_access_unit(payload)
           : codec == VideoRtpCodec::h265 ? rtp_h265_starts_access_unit(payload)
           : codec == VideoRtpCodec::av1  ? rtp_av1_starts_temporal_unit(payload)
                                          : false;
}
void release_window(VideoRtpStream* stream) noexcept {
    if (stream->slots)
        VirtualFree(stream->slots, 0, MEM_RELEASE);
    reset_value(stream);
}
// Loss breaks the reference chain until the next keyframe.
void lose(VideoRtpStream& stream) noexcept {
    stream.keyframe_needed = true;
    stream.references_lost = true;
    ++stream.stats.losses;
}
void clear_slot(VideoRtpStream& stream, u64 sequence) noexcept {
    auto& slot = slot_at(stream, sequence);
    if (slot.state == missing && slot.sequence == sequence && stream.missing)
        --stream.missing;
    slot.state = empty;
}
// Drops every entry in [stream.base, end) and moves base there.
void discard_until(VideoRtpStream& stream, u64 end) noexcept {
    for (u64 sequence = stream.base; sequence < end && sequence <= stream.highest; ++sequence) {
        if (slot_at(stream, sequence).state == present)
            ++stream.stats.discarded_packets;
        clear_slot(stream, sequence);
    }
    if (end > stream.base)
        stream.base = end;
    stream.prev_known = false;
}
void start(VideoRtpStream& stream, u16 sequence, u64 now_us) noexcept {
    const u64 extended = kExtensionBase + sequence;
    stream.first = stream.base = extended;
    stream.highest = extended - 1;
    stream.received = stream.expected_prior = stream.received_prior = 0;
    stream.missing = 0;
    stream.prev_known = stream.has_transit = false;
    stream.references_lost = true; // No keyframe yet.
    stream.started = true;
    stream.consecutive_late = 0;
    stream.next_nack_us = stream.oldest_missing_us = 0;
    stream.last_packet_us = now_us;
}
void restart(VideoRtpStream& stream, u16 sequence, u64 now_us) noexcept {
    for (u64 i = 0; i != stream.capacity; ++i)
        stream.slots[i].state = empty;
    const bool seen = stream.keyframe_seen;
    start(stream, sequence, now_us);
    stream.keyframe_seen = seen;
    lose(stream);
}
u64 unwrap(const VideoRtpStream& stream, u16 sequence) noexcept {
    const i16 distance = static_cast<i16>(static_cast<u16>(sequence - static_cast<u16>(stream.highest)));
    return stream.highest + static_cast<i64>(distance);
}
void mark_missing(const VideoRtpConfig& config, VideoRtpStream& stream, u64 sequence, u64 now_us,
                  bool expire) noexcept {
    auto& slot = slot_at(stream, sequence);
    reset_value(&slot);
    slot.sequence = sequence;
    // An over-wide gap is lost at once: time zero is always expired.
    slot.time_us = expire ? 0 : now_us;
    slot.nacks = expire ? config.max_nacks : 0;
    slot.nack_us = now_us + config.nack_delay_us;
    slot.state = missing;
    if (!stream.missing)
        stream.oldest_missing_us = slot.time_us;
    if (!expire && (!stream.next_nack_us || slot.nack_us < stream.next_nack_us))
        stream.next_nack_us = slot.nack_us;
    ++stream.missing;
}
bool expired(const VideoRtpConfig& config, const VideoRtpSlot& slot, u64 now_us) noexcept {
    return now_us >= slot.time_us + config.give_up_us;
}
// Nothing was consumed yet and the head's predecessor is unknown: packets
// reordered around the first one received may still extend the window back.
bool starting(const VideoRtpStream& stream) noexcept {
    return stream.started && !stream.prev_known && stream.base == stream.first && stream.base <= stream.highest;
}
void update_jitter(VideoRtpStream& stream, u32 timestamp, u64 now_us) noexcept {
    // RFC 3550 A.8 in 90 kHz units, kept in Q4 like libwebrtc.
    const u32 transit = static_cast<u32>(now_us * 90 / 1000) - timestamp;
    if (stream.has_transit && timestamp != stream.jitter_timestamp) {
        i32 delta = static_cast<i32>(transit - stream.last_transit);
        if (delta < 0)
            delta = -delta;
        if (delta < 0x1000000)
            stream.jitter_q4 += ((delta << 4) - stream.jitter_q4 + 8) >> 4;
    }
    if (!stream.has_transit || timestamp != stream.jitter_timestamp) {
        stream.last_transit = transit;
        stream.jitter_timestamp = timestamp;
        stream.has_transit = true;
    }
}
VideoRtpCodec codec_of(const VideoRtpReceiver& receiver, u8 type) noexcept {
    const auto* binding = media_codec(receiver, type);
    return binding ? binding->codec : VideoRtpCodec::none;
}
void deliver(VideoRtpReceiver& receiver, VideoRtpStream& stream, u64 end) noexcept {
    const auto& head = slot_at(stream, stream.base);
    const u8 type = head.payload_type;
    const u32 timestamp = head.timestamp;
    const VideoRtpCodec codec = codec_of(receiver, type);
    RtpVideoAssembly assembly;
    rtp_video_assembly_begin(&assembly, {receiver.frame, kVideoRtpFrameBytes});
    bool intact = codec != VideoRtpCodec::none;
    for (u64 sequence = stream.base; intact && sequence <= end; ++sequence) {
        const auto& slot = slot_at(stream, sequence);
        if (slot.state != present)
            continue;
        if (slot.payload_type != type) {
            intact = false; // A payload type switch inside one access unit.
            break;
        }
        const ConstSlice<u8> payload{payload_at(stream, sequence), slot.bytes};
        const auto pushed = codec == VideoRtpCodec::h264   ? rtp_h264_push(&assembly, payload, false)
                            : codec == VideoRtpCodec::h265 ? rtp_h265_push(&assembly, payload, false)
                                                           : rtp_av1_push(&assembly, payload, false);
        intact = pushed == RtpDepacketize::appended && !assembly.damaged;
    }
    const u32 bytes = rtp_video_assembly_finish(&assembly);
    intact = intact && !assembly.damaged && bytes;
    stream.prev_known = true;
    stream.prev_timestamp = timestamp;
    stream.prev_marker = true; // Either the marker or a timestamp change ended the unit.
    stream.last_timestamp = timestamp;
    stream.has_last_timestamp = true;
    for (u64 sequence = stream.base; sequence <= end; ++sequence)
        clear_slot(stream, sequence);
    stream.base = end + 1;
    if (!intact) {
        ++stream.stats.damaged_frames;
        lose(stream);
        return;
    }
    const bool keyframe = codec == VideoRtpCodec::h264   ? rtp_h264_keyframe(assembly)
                          : codec == VideoRtpCodec::h265 ? rtp_h265_keyframe(assembly)
                                                         : rtp_av1_keyframe(assembly);
    if (keyframe) {
        stream.keyframe_needed = false;
        stream.references_lost = false;
    } else if (!stream.keyframe_seen) {
        stream.keyframe_needed = true; // Joined mid-stream: ask for a starting point.
    }
    stream.keyframe_seen = stream.keyframe_seen || keyframe;
    ++stream.stats.frames;
    VideoRtpFrame frame{};
    frame.user_id = stream.user_id;
    frame.annex_b = {receiver.frame, bytes};
    frame.ssrc = stream.ssrc;
    frame.rtp_timestamp = timestamp;
    frame.codec = codec;
    frame.keyframe = keyframe;
    frame.references_lost = stream.references_lost;
    if (receiver.sink)
        receiver.sink(receiver.sink_context, frame);
}
// Emits every access unit that is complete, in order, and resolves gaps whose
// recovery time ran out. Frames whose first packet is unknown are dropped.
void assemble(VideoRtpReceiver& receiver, VideoRtpStream& stream, u64 now_us) noexcept {
    const auto& config = receiver.config;
    while (stream.started && stream.base <= stream.highest) {
        auto& head = slot_at(stream, stream.base);
        if (head.state == missing) {
            if (!expired(config, head, now_us))
                return;
            while (stream.base <= stream.highest && slot_at(stream, stream.base).state == missing) {
                clear_slot(stream, stream.base);
                ++stream.base;
            }
            stream.prev_known = false;
            lose(stream);
            continue;
        }
        if (head.state != present) { // Padding: sequence space only.
            clear_slot(stream, stream.base);
            ++stream.base;
            continue;
        }
        if (starting(stream) && now_us < head.time_us + config.start_grace_us)
            return;
        const ConstSlice<u8> payload{payload_at(stream, stream.base), head.bytes};
        const bool begins =
            stream.prev_known
                ? stream.prev_marker || stream.prev_timestamp != head.timestamp
                : (!stream.has_last_timestamp || stream.last_timestamp != head.timestamp) &&
                      starts_access_unit(codec_of(receiver, head.payload_type), payload);
        if (!begins) {
            // Tail of an access unit whose beginning was lost.
            stream.prev_known = true;
            stream.prev_timestamp = stream.last_timestamp = head.timestamp;
            stream.prev_marker = head.marker;
            stream.has_last_timestamp = true;
            ++stream.stats.discarded_packets;
            clear_slot(stream, stream.base);
            ++stream.base;
            continue;
        }
        const u32 timestamp = head.timestamp;
        u64 sequence = stream.base;
        bool complete = false, blocked = false;
        u64 end = 0;
        while (sequence <= stream.highest) {
            const auto& slot = slot_at(stream, sequence);
            if (slot.state == missing) {
                blocked = true;
                break;
            }
            if (slot.state == present) {
                if (slot.timestamp != timestamp) {
                    complete = true;
                    end = sequence - 1;
                    break;
                }
                if (slot.marker) {
                    complete = true;
                    end = sequence;
                    break;
                }
            }
            ++sequence;
        }
        if (complete) {
            deliver(receiver, stream, end);
            continue;
        }
        if (!blocked || !expired(config, slot_at(stream, sequence), now_us))
            return; // Wait for the rest of the unit or for recovery.
        // The unit has an unrecoverable hole: drop what was received of it.
        stream.last_timestamp = timestamp;
        stream.has_last_timestamp = true;
        ++stream.stats.damaged_frames;
        discard_until(stream, sequence);
        lose(stream);
    }
}
// Recomputes the cached schedule from the window. Only the FIRST missing
// packet in sequence order can expire actionably: assembly resolves later
// gaps in the same pass once it is past the first.
void rescan(const VideoRtpConfig& config, VideoRtpStream& stream) noexcept {
    stream.next_nack_us = stream.oldest_missing_us = 0;
    u32 count = 0;
    if (stream.started)
        for (u64 sequence = stream.base; sequence <= stream.highest; ++sequence) {
            const auto& slot = slot_at(stream, sequence);
            if (slot.state != missing)
                continue;
            if (!count++)
                stream.oldest_missing_us = slot.time_us;
            if (slot.nacks < config.max_nacks && (!stream.next_nack_us || slot.nack_us < stream.next_nack_us))
                stream.next_nack_us = slot.nack_us;
        }
    stream.missing = count;
}
bool nack_due(const VideoRtpStream& stream, u64 now_us) noexcept {
    return stream.missing && stream.next_nack_us && now_us >= stream.next_nack_us;
}
bool keyframe_due(const VideoRtpConfig& config, const VideoRtpStream& stream, u64 now_us) noexcept {
    return stream.keyframe_needed &&
           (!stream.keyframe_request_us || now_us >= stream.keyframe_request_us + config.keyframe_retry_us);
}
VideoRtpStream* find(VideoRtpReceiver& receiver, u32 ssrc) noexcept {
#pragma clang loop unroll(disable)
    for (auto& stream : receiver.streams)
        if (stream.slots && stream.ssrc == ssrc)
            return &stream;
    return nullptr;
}
VideoRtpStream* create(VideoRtpReceiver& receiver, u32 ssrc, u64 user_id, u64 now_us) noexcept {
    VideoRtpStream* chosen = nullptr;
#pragma clang loop unroll(disable)
    for (auto& stream : receiver.streams)
        if (!stream.slots) {
            chosen = &stream;
            break;
        }
    if (!chosen) {
        // Reuse the longest-idle stream if it is past the idle limit.
#pragma clang loop unroll(disable)
        for (auto& stream : receiver.streams)
            if (now_us >= stream.last_packet_us + receiver.config.idle_stream_us &&
                (!chosen || stream.last_packet_us < chosen->last_packet_us))
                chosen = &stream;
        if (!chosen)
            return nullptr;
        release_window(chosen);
    }
    void* block = VirtualAlloc(nullptr, window_bytes(kInitialSlots), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!block)
        return nullptr;
    reset_value(chosen);
    chosen->slots = static_cast<VideoRtpSlot*>(block);
    chosen->payloads = static_cast<u8*>(block) + sizeof(VideoRtpSlot) * usize{kInitialSlots};
    chosen->capacity = kInitialSlots;
    chosen->ssrc = ssrc;
    chosen->user_id = user_id;
    chosen->last_packet_us = now_us;
    if (!receiver.next_report_us)
        receiver.next_report_us = now_us + receiver.config.report_interval_us / 2;
    return chosen;
}
// Moves the live window [base, highest] into a block of `slots` entries.
bool grow(VideoRtpStream& stream, u32 slots) noexcept {
    void* block = VirtualAlloc(nullptr, window_bytes(slots), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!block)
        return false;
    VideoRtpStream next = stream;
    next.slots = static_cast<VideoRtpSlot*>(block);
    next.payloads = static_cast<u8*>(block) + sizeof(VideoRtpSlot) * usize{slots};
    next.capacity = slots;
    for (u64 sequence = stream.base; sequence <= stream.highest; ++sequence) {
        const auto& slot = slot_at(stream, sequence);
        slot_at(next, sequence) = slot;
        copy_bytes(payload_at(next, sequence), payload_at(stream, sequence), slot.bytes);
    }
    VirtualFree(stream.slots, 0, MEM_RELEASE);
    stream = next;
    return true;
}
// Makes the window hold `span` consecutive sequences (at most kVideoRtpPacketSlots) in distinct slots.
bool fit(VideoRtpStream& stream, u64 span) noexcept {
    u32 slots = stream.capacity;
    while (slots < span)
        slots *= 2;
    return slots == stream.capacity || grow(stream, slots);
}
VideoRtpPush insert(VideoRtpReceiver& receiver, VideoRtpStream& stream, u16 sequence, u32 timestamp,
                    bool marker, u8 type, ConstSlice<u8> payload, bool rtx, u64 now_us) noexcept {
    const auto& config = receiver.config;
    if (!stream.started)
        start(stream, sequence, now_us);
    u64 extended = unwrap(stream, sequence);
    if (extended < stream.base && stream.base == stream.first && !stream.prev_known &&
        stream.highest - extended < kVideoRtpPacketSlots && fit(stream, stream.highest - extended + 1)) {
        // Reordered around the first packet: extend the window backwards.
        for (u64 missing_sequence = extended; missing_sequence < stream.base; ++missing_sequence)
            mark_missing(config, stream, missing_sequence, now_us, false);
        stream.base = stream.first = extended;
    }
    if (extended < stream.base) {
        ++stream.stats.late;
        if (rtx || ++stream.consecutive_late < kLateRestart)
            return VideoRtpPush::late;
        restart(stream, sequence, now_us);
        extended = stream.base;
    }
    stream.consecutive_late = 0;
    if (extended >= stream.base + kVideoRtpPacketSlots) {
        if (extended > stream.highest + kVideoRtpPacketSlots) {
            restart(stream, sequence, now_us); // Jump past the whole window.
            extended = stream.base;
        } else {
            discard_until(stream, extended - kVideoRtpPacketSlots + 1);
            lose(stream);
        }
    }
    if (extended > stream.highest && !fit(stream, extended - stream.base + 1)) {
        // No memory to grow the window: drop the oldest packets as if it were full.
        discard_until(stream, extended - stream.capacity + 1);
        lose(stream);
    }
    auto& slot = slot_at(stream, extended);
    if (extended <= stream.highest) {
        if (slot.sequence == extended && (slot.state == present || slot.state == padding))
            return VideoRtpPush::duplicate;
        if (slot.state == missing && slot.sequence == extended && stream.missing)
            --stream.missing;
    } else {
        const u64 gap = extended - stream.highest - 1;
        const bool wide = gap > kVideoRtpMaxNackGap;
        for (u64 missing_sequence = stream.highest + 1; missing_sequence < extended; ++missing_sequence)
            mark_missing(config, stream, missing_sequence, now_us, wide);
        if (wide)
            lose(stream);
        // RFC 3550 jitter from in-order original packets only.
        if (!rtx && payload.count)
            update_jitter(stream, timestamp, now_us);
        stream.highest = extended;
    }
    reset_value(&slot);
    slot.sequence = extended;
    slot.time_us = now_us;
    slot.timestamp = timestamp;
    slot.bytes = static_cast<u16>(payload.count);
    slot.payload_type = type;
    slot.marker = marker;
    slot.state = payload.count ? present : padding;
    copy_bytes(payload_at(stream, extended), payload.data, payload.count);
    ++stream.received;
    if (!payload.count)
        return VideoRtpPush::padding;
    return rtx ? VideoRtpPush::recovered : VideoRtpPush::stored;
}
// Feedback items only count once RTCP can be sent (a local SSRC is known).
u64 stream_deadline(const VideoRtpConfig& config, const VideoRtpStream& stream, bool feedback) noexcept {
    u64 deadline = 0;
    const auto earliest = [&](u64 value) noexcept {
        if (value && (!deadline || value < deadline))
            deadline = value;
    };
    if (stream.missing) {
        if (feedback)
            earliest(stream.next_nack_us);
        earliest(stream.oldest_missing_us + config.give_up_us);
    }
    if (feedback && stream.keyframe_needed)
        earliest(stream.keyframe_request_us ? stream.keyframe_request_us + config.keyframe_retry_us : 1);
    if (starting(stream))
        earliest(stream.slots[stream.base & (stream.capacity - 1)].time_us + config.start_grace_us);
    return deadline;
}
} // namespace

// The packet path above and video_rtp_receive run per packet and stay at full speed. The rest is
// setup, timers and RTCP feedback: compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
VideoRtpConfig video_rtp_default_config() noexcept {
    VideoRtpConfig config{};
    config.nack_delay_us = 5'000;
    config.nack_retry_us = 60'000;
    config.give_up_us = 300'000;
    config.start_grace_us = 20'000;
    config.keyframe_retry_us = 300'000;
    config.report_interval_us = 1'000'000;
    config.idle_stream_us = 5'000'000;
    config.max_nacks = 5;
    return config;
}

bool video_rtp_initialize(VideoRtpReceiver* receiver, u32 local_ssrc, VideoRtpFrameSink sink,
                          void* context) noexcept {
    if (!receiver || receiver->frame || !sink)
        return false;
    auto* frame =
        static_cast<u8*>(VirtualAlloc(nullptr, kVideoRtpFrameBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!frame)
        return false;
    reset_value(receiver);
    receiver->frame = frame;
    receiver->config = video_rtp_default_config();
    receiver->local_ssrc = local_ssrc;
    receiver->sink = sink;
    receiver->sink_context = context;
    return true;
}

void video_rtp_release(VideoRtpReceiver* receiver) noexcept {
    if (!receiver)
        return;
    for (auto& stream : receiver->streams)
        release_window(&stream);
    if (receiver->frame)
        VirtualFree(receiver->frame, 0, MEM_RELEASE);
    reset_value(receiver);
}

void video_rtp_set_local_ssrc(VideoRtpReceiver* receiver, u32 local_ssrc) noexcept {
    if (receiver)
        receiver->local_ssrc = local_ssrc;
}

bool video_rtp_set_codecs(VideoRtpReceiver* receiver, ConstSlice<VideoRtpCodecBinding> codecs) noexcept {
    if (!receiver || codecs.count > kVideoRtpMaxCodecs || (codecs.count && !codecs.data))
        return false;
    for (u32 i = 0; i != codecs.count; ++i) {
        const auto& codec = codecs[i];
        // RFC 5761: types 64-95 collide with RTCP under rtcp-mux.
        const auto valid = [](u8 type) noexcept { return type < 128 && (type < 64 || type > 95); };
        if (codec.codec == VideoRtpCodec::none || !valid(codec.payload_type) ||
            (codec.rtx_payload_type && (!valid(codec.rtx_payload_type) ||
                                        codec.rtx_payload_type == codec.payload_type)))
            return false;
        for (u32 j = 0; j != i; ++j) {
            const auto& other = codecs[j];
            const u8 types[4]{codec.payload_type, codec.rtx_payload_type, other.payload_type,
                              other.rtx_payload_type};
            if (types[0] == types[2] || types[0] == types[3] || (types[1] && (types[1] == types[2] ||
                                                                               types[1] == types[3])))
                return false;
        }
    }
    for (u32 i = 0; i != codecs.count; ++i)
        receiver->codecs[i] = codecs[i];
    receiver->codec_count = codecs.count;
    return true;
}

#pragma clang attribute pop
VideoRtpPush video_rtp_receive(VideoRtpReceiver* receiver, const VideoRtpSource& source,
                               const MediaPacketLayout& layout, ConstSlice<u8> plaintext, u64 now_us) noexcept {
    if (!receiver || !receiver->frame || layout.kind != MediaPacketKind::rtp || !source.media_ssrc ||
        !byte_range_valid(plaintext) || layout.payload_offset > plaintext.count ||
        layout.payload_bytes > plaintext.count - layout.payload_offset)
        return VideoRtpPush::invalid;
    ConstSlice<u8> payload{plaintext.data + layout.payload_offset, layout.payload_bytes};
    u16 sequence = layout.sequence;
    u8 type = layout.type;
    if (source.rtx) {
        const auto* binding = rtx_codec(*receiver, layout.type);
        // RFC 4588 4: the payload starts with the original sequence number.
        // Shorter RTX packets are bandwidth-probe padding of the RTX stream.
        if (!binding || payload.count < 2)
            return binding ? VideoRtpPush::padding : VideoRtpPush::ignored;
        sequence = byte_load_be16(payload.data);
        payload = {payload.data + 2, payload.count - 2};
        type = binding->payload_type;
    } else if (!media_codec(*receiver, layout.type))
        return VideoRtpPush::ignored;
    auto* stream = find(*receiver, source.media_ssrc);
    if (!stream)
        stream = create(*receiver, source.media_ssrc, source.user_id, now_us);
    if (!stream)
        return VideoRtpPush::ignored;
    stream->last_packet_us = now_us;
    stream->user_id = source.user_id;
    if (source.rtx)
        stream->rtx_ssrc = layout.ssrc;
    if (payload.count > kVideoRtpPacketBytes) {
        ++stream->stats.oversized;
        return VideoRtpPush::ignored; // Left missing: recovery or keyframe handles it.
    }
    if (source.rtx)
        ++stream->stats.rtx_packets;
    const auto result =
        insert(*receiver, *stream, sequence, layout.timestamp, layout.marker, type, payload, source.rtx, now_us);
    if (result == VideoRtpPush::stored || result == VideoRtpPush::recovered || result == VideoRtpPush::padding)
        assemble(*receiver, *stream, now_us);
    return result;
}

#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
void video_rtp_receive_rtcp(VideoRtpReceiver* receiver, ConstSlice<u8> plaintext, u64 now_us) noexcept {
    if (!receiver)
        return;
    RtcpSenderInfo reports[8];
    const u32 count = rtcp_read_sender_reports(plaintext, {reports, 8});
    for (u32 i = 0; i != count; ++i)
        if (auto* stream = find(*receiver, reports[i].ssrc)) {
            stream->last_sender_report = rtcp_compact_ntp(reports[i].ntp_seconds, reports[i].ntp_fraction);
            stream->last_sender_report_us = now_us;
        }
}

void video_rtp_tick(VideoRtpReceiver* receiver, u64 now_us) noexcept {
    if (!receiver)
        return;
    for (auto& stream : receiver->streams) {
        if (!stream.slots || !stream.started)
            continue;
        const bool startup = starting(stream);
        if (!startup && (!stream.missing || now_us < stream.oldest_missing_us + receiver->config.give_up_us))
            continue;
        assemble(*receiver, stream, now_us);
        rescan(receiver->config, stream);
    }
}

u64 video_rtp_deadline(const VideoRtpReceiver& receiver) noexcept {
    const bool feedback = receiver.local_ssrc != 0;
    u64 deadline = feedback ? receiver.next_report_us : 0;
    for (const auto& stream : receiver.streams) {
        if (!stream.slots || !stream.started)
            continue;
        const u64 value = stream_deadline(receiver.config, stream, feedback);
        if (value && (!deadline || value < deadline))
            deadline = value;
    }
    return deadline;
}

u32 video_rtp_feedback(VideoRtpReceiver* receiver, u64 now_us, Slice<u8> output) noexcept {
    if (!receiver || !receiver->local_ssrc)
        return 0;
    const auto& config = receiver->config;
    bool due = receiver->next_report_us && now_us >= receiver->next_report_us;
    for (const auto& stream : receiver->streams)
        if (stream.slots && stream.started)
            due = due || nack_due(stream, now_us) || keyframe_due(config, stream, now_us);
    if (!due)
        return 0;
    RtcpReportBlock blocks[kRtcpMaxReportBlocks];
    u32 block_count = 0;
    for (const auto& stream : receiver->streams) {
        if (!stream.slots || !stream.started || block_count == kRtcpMaxReportBlocks)
            continue;
        auto& block = blocks[block_count++];
        const u64 expected = stream.highest + 1 - stream.first;
        const i64 lost = static_cast<i64>(expected) - static_cast<i64>(stream.received);
        const u64 expected_interval = expected - stream.expected_prior;
        const u64 received_interval = stream.received - stream.received_prior;
        reset_value(&block);
        block.ssrc = stream.ssrc;
        block.cumulative_lost = lost > 0x7fffff ? 0x7fffff : lost < -0x800000 ? -0x800000 : static_cast<i32>(lost);
        if (expected_interval > received_interval)
            block.fraction_lost =
                static_cast<u8>(minimum<u64>(255, ((expected_interval - received_interval) << 8) / expected_interval));
        block.extended_highest_sequence = static_cast<u32>(stream.highest - kExtensionBase);
        block.jitter = static_cast<u32>(stream.jitter_q4 >> 4);
        if (stream.last_sender_report_us) {
            block.last_sender_report = stream.last_sender_report;
            block.delay_since_last_sender_report =
                static_cast<u32>(minimum<u64>(~u32{0}, (now_us - stream.last_sender_report_us) * 65536 / 1'000'000));
        }
    }
    u32 offset = rtcp_write_receiver_report(output, 0, receiver->local_ssrc, {blocks, block_count});
    if (!offset)
        return 0;
    for (auto& stream : receiver->streams) {
        if (!stream.slots || !stream.started)
            continue;
        stream.expected_prior = stream.highest + 1 - stream.first;
        stream.received_prior = stream.received;
    }
    receiver->next_report_us = now_us + config.report_interval_us;
    for (auto& stream : receiver->streams) {
        if (!stream.slots || !stream.started)
            continue;
        if (nack_due(stream, now_us) && output.count - offset >= 16) {
            // Slots NACK-able now, in sequence order; the first `consumed` of them are the ones packed.
            const auto wanted = [&](u64 sequence) noexcept {
                const auto& slot = slot_at(stream, sequence);
                return slot.state == missing && slot.time_us && slot.nacks < config.max_nacks && now_us >= slot.nack_us;
            };
            u16 sequences[kVideoRtpMaxNackItems * 17];
            u32 count = 0;
            for (u64 sequence = stream.base; sequence <= stream.highest && count != kVideoRtpMaxNackItems * 17;
                 ++sequence)
                if (wanted(sequence))
                    sequences[count++] = static_cast<u16>(sequence);
            RtcpNackItem items[kVideoRtpMaxNackItems];
            const u32 room = minimum<u32>(kVideoRtpMaxNackItems, (output.count - offset - 12) / 4);
            u32 consumed = 0;
            const u32 item_count = count ? rtcp_nack_pack({sequences, count}, {items, room}, &consumed) : 0;
            if (item_count) {
                const u32 next =
                    rtcp_write_nack(output, offset, receiver->local_ssrc, stream.ssrc, {items, item_count});
                if (next) {
                    offset = next;
                    for (u64 sequence = stream.base; consumed; ++sequence)
                        if (wanted(sequence)) {
                            auto& slot = slot_at(stream, sequence);
                            ++slot.nacks;
                            slot.nack_us = now_us + config.nack_retry_us;
                            --consumed;
                        }
                }
            }
            rescan(config, stream);
            // Entries that did not fit wait one retry interval.
            if (stream.next_nack_us && stream.next_nack_us <= now_us)
                stream.next_nack_us = now_us + config.nack_retry_us;
        }
        if (keyframe_due(config, stream, now_us)) {
            const u32 next = config.use_fir
                                 ? rtcp_write_fir(output, offset, receiver->local_ssrc, stream.ssrc,
                                                  stream.fir_sequence)
                                 : rtcp_write_pli(output, offset, receiver->local_ssrc, stream.ssrc);
            if (next) {
                offset = next;
                if (config.use_fir)
                    ++stream.fir_sequence;
                stream.keyframe_request_us = now_us;
            }
        }
    }
    return offset;
}

bool video_rtp_request_keyframe(VideoRtpReceiver* receiver, u32 media_ssrc) noexcept {
    auto* stream = receiver ? find(*receiver, media_ssrc) : nullptr;
    if (!stream)
        return false;
    stream->keyframe_needed = true;
    stream->references_lost = true;
    return true;
}

void video_rtp_remove_stream(VideoRtpReceiver* receiver, u32 media_ssrc) noexcept {
    if (auto* stream = receiver ? find(*receiver, media_ssrc) : nullptr)
        release_window(stream);
}

const VideoRtpStream* video_rtp_find_stream(const VideoRtpReceiver& receiver, u32 media_ssrc) noexcept {
    return find(const_cast<VideoRtpReceiver&>(receiver), media_ssrc);
}
} // namespace maicord
#pragma clang attribute pop
