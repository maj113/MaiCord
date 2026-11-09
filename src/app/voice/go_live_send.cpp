#include "maicord/app/voice/go_live_send.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/platform/crypto/random.hpp"

namespace maicord {
namespace {
constexpr u64 kGroupUs = 5'000;             // Packets sent within 5 ms form one group.
constexpr double kOveruseTimeMs = 10;       // Overuse must last this long.
constexpr u64 kHistoryBytes = sizeof(GoLiveHistoryEntry) * kGoLiveHistoryPackets;
constexpr u64 kRetransmitSpacingUs = 50'000; // One RTX of a packet per 50 ms.
constexpr u64 kBurstUs = 5'000;              // The pacer's largest burst.
constexpr u16 kPlayoutMaximum = 10;          // Playout-delay extension: 0 to 100 ms (10 ms units).

u32 clamp_rate(const GoLiveBandwidth& b, u64 rate) noexcept {
    u64 high = b.bounds.maximum;
    if (b.remb && b.remb < high)
        high = b.remb;
    if (high < b.bounds.minimum)
        high = b.bounds.minimum;
    return static_cast<u32>(rate < b.bounds.minimum ? b.bounds.minimum : rate > high ? high : rate);
}
double absolute(double value) noexcept {
    return value < 0 ? -value : value;
}
// Least-squares slope of the delay trend.
double slope(const GoLiveBandwidth& b) noexcept {
    double x = 0, y = 0;
    for (u32 i = 0; i != kGoLiveTrendWindow; ++i) {
        x += b.trend_x[i];
        y += b.trend_y[i];
    }
    x /= kGoLiveTrendWindow;
    y /= kGoLiveTrendWindow;
    double numerator = 0, denominator = 0;
    for (u32 i = 0; i != kGoLiveTrendWindow; ++i) {
        numerator += (b.trend_x[i] - x) * (b.trend_y[i] - y);
        denominator += (b.trend_x[i] - x) * (b.trend_x[i] - x);
    }
    return denominator > 0 ? numerator / denominator : 0;
}
// One completed packet group: delay variation into the trendline and detector.
void group_done(GoLiveBandwidth& b) noexcept {
    if (b.has_previous) {
        const double variation = static_cast<double>((b.group_arrival_us - b.previous_arrival_us) -
                                                     (b.group_send_us - b.previous_send_us)) / 1000.0;
        b.accumulated += variation;
        b.smoothed = 0.9 * b.smoothed + 0.1 * b.accumulated;
        const u32 at = b.trend_count % kGoLiveTrendWindow;
        b.trend_x[at] = static_cast<double>(b.group_arrival_us - b.first_arrival_us) / 1000.0;
        b.trend_y[at] = b.smoothed;
        ++b.trend_count;
        ++b.groups;
    }
    b.previous_send_us = b.group_send_us;
    b.previous_arrival_us = b.group_arrival_us;
    b.has_previous = true;
}

void store_header(u8* out, bool extension, bool marker, u8 type, u16 sequence, u32 timestamp, u32 ssrc) noexcept {
    out[0] = static_cast<u8>(0x80 | (extension ? 0x10 : 0));
    out[1] = static_cast<u8>((marker ? 0x80 : 0) | type);
    out[2] = static_cast<u8>(sequence >> 8);
    out[3] = static_cast<u8>(sequence);
    byte_store_be32(out + 4, timestamp);
    byte_store_be32(out + 8, ssrc);
}
// Video header extensions (0xBEDE, two words): transport sequence, playout delay.
u32 video_extensions(u8* out, u16 transport) noexcept {
    out[0] = 0xbe;
    out[1] = 0xde;
    out[2] = 0;
    out[3] = 2;
    out[4] = static_cast<u8>(kGoLiveTransportSequenceExtensionId << 4 | 1);
    out[5] = static_cast<u8>(transport >> 8);
    out[6] = static_cast<u8>(transport);
    out[7] = static_cast<u8>(kGoLivePlayoutDelayExtensionId << 4 | 2);
    out[8] = 0; // Minimum 0, maximum kPlayoutMaximum (12 bits each).
    out[9] = static_cast<u8>(kPlayoutMaximum >> 8);
    out[10] = static_cast<u8>(kPlayoutMaximum);
    out[11] = 0;
    return 12;
}
u64 now_unix_us() noexcept {
    return clock_unix_milliseconds() * 1000;
}

void on_keyframe(void* context, u32 media) noexcept {
    auto& s = *static_cast<GoLiveSender*>(context);
    if (!media || media == s.streams.video_ssrc) {
        s.keyframe_wanted = true;
        ++s.stats.keyframe_requests;
    }
}
void on_nack(void* context, u32 media, u16 sequence) noexcept {
    auto& s = *static_cast<GoLiveSender*>(context);
    if (media != s.streams.video_ssrc || !s.history)
        return;
    ++s.stats.nacks;
    const auto& entry = s.history[sequence & (kGoLiveHistoryPackets - 1)];
    if (!entry.valid || entry.sequence != sequence)
        return;
    for (u32 i = 0; i != s.rtx_count; ++i)
        if (s.rtx[i] == sequence)
            return;
    if (s.rtx_count != kGoLiveRtxQueue)
        s.rtx[s.rtx_count++] = sequence;
}
void on_remb(void* context, u64 bitrate) noexcept {
    go_live_bandwidth_remb(&static_cast<GoLiveSender*>(context)->bandwidth, bitrate);
}
struct FeedbackContext {
    GoLiveSender* sender;
    u64 now_us;
    bool transport;
};
void on_report(void* context, const RtcpReportBlock& block) noexcept {
    auto& f = *static_cast<FeedbackContext*>(context);
    if (block.ssrc == f.sender->streams.video_ssrc && !f.transport)
        go_live_bandwidth_loss(&f.sender->bandwidth, block.fraction_lost, f.now_us);
}
void on_transport(void* context, ConstSlice<RtcpTransportArrival> arrivals) noexcept {
    auto& f = *static_cast<FeedbackContext*>(context);
    auto& s = *f.sender;
    for (u32 i = 0; i != arrivals.count; ++i) {
        const auto& arrival = arrivals[i];
        const auto& sent = s.transport_log[arrival.sequence & (kGoLiveTransportLog - 1)];
        if (sent.valid && sent.sequence == arrival.sequence)
            go_live_bandwidth_packet(&s.bandwidth, sent.send_us, arrival.arrival_us, sent.bytes, arrival.received);
    }
    go_live_bandwidth_update(&s.bandwidth, f.now_us);
    ++s.stats.feedback;
    f.transport = true;
}
// Proxies so the feedback visitor reaches the sender through FeedbackContext.
void on_keyframe_proxy(void* context, u32 media) noexcept {
    on_keyframe(static_cast<FeedbackContext*>(context)->sender, media);
}
void on_nack_proxy(void* context, u32 media, u16 sequence) noexcept {
    on_nack(static_cast<FeedbackContext*>(context)->sender, media, sequence);
}
void on_remb_proxy(void* context, u64 bitrate) noexcept {
    on_remb(static_cast<FeedbackContext*>(context)->sender, bitrate);
}

// Sends the held datagram; true when the transport took it (or it failed for good).
bool flush(GoLiveSender& s, const GoLiveSendLink& link, u64 now_us) noexcept {
    if (!s.datagram_bytes)
        return true;
    const auto sent = link.send(link.context, {s.datagram, s.datagram_bytes});
    if (sent == VoiceUdpMediaSend::busy)
        return false;
    if (sent == VoiceUdpMediaSend::accepted && s.datagram_transport) {
        const u16 transport = static_cast<u16>(s.datagram[17] << 8 | s.datagram[18]);
        s.transport_log[transport & (kGoLiveTransportLog - 1)] = {now_us, transport,
                                                                  static_cast<u16>(s.datagram_bytes), true};
    }
    if (s.datagram_video)
        s.budget_bytes -= s.datagram_bytes;
    s.datagram_bytes = 0;
    return true;
}
void hold(GoLiveSender& s, u32 bytes, bool video, bool transport) noexcept {
    s.datagram_bytes = bytes;
    s.datagram_video = video;
    s.datagram_transport = transport;
}
void hold_audio(GoLiveSender& s) noexcept {
    auto& packet = s.audio[s.audio_head];
    u8* out = s.datagram;
    store_header(out, true, false, kGoLiveOpusPayloadType, s.audio_sequence++, packet.timestamp, s.streams.audio_ssrc);
    // Discord speaking extension: SOUNDSHARE (flag 2) shifted left once.
    out[12] = 0xbe;
    out[13] = 0xde;
    out[14] = 0;
    out[15] = 1;
    out[16] = static_cast<u8>(kGoLiveSpeakingExtensionId << 4);
    out[17] = 2 << 1;
    out[18] = out[19] = 0;
    copy_bytes(out + 20, packet.data, packet.bytes);
    s.audio_octets += packet.bytes;
    ++s.audio_total;
    ++s.stats.audio_packets;
    s.audio_head = (s.audio_head + 1) % kGoLiveAudioQueue;
    --s.audio_count;
    hold(s, 20u + packet.bytes, false, false);
}
void hold_rtx(GoLiveSender& s, u64 now_us) noexcept {
    const u16 sequence = s.rtx[0];
    --s.rtx_count;
    copy_bytes(s.rtx, s.rtx + 1, s.rtx_count * 2u);
    auto& entry = s.history[sequence & (kGoLiveHistoryPackets - 1)];
    if (!entry.valid || entry.sequence != sequence || now_us - entry.retransmitted_us < kRetransmitSpacingUs)
        return;
    entry.retransmitted_us = now_us;
    u8* out = s.datagram;
    const u16 transport = s.transport_sequence++;
    store_header(out, true, entry.marker, s.streams.rtx_payload_type, s.rtx_sequence++, entry.timestamp,
                 s.streams.rtx_ssrc);
    u32 at = 12 + video_extensions(out + 12, transport);
    out[at++] = static_cast<u8>(sequence >> 8);
    out[at++] = static_cast<u8>(sequence);
    copy_bytes(out + at, entry.payload, entry.bytes);
    ++s.stats.rtx_packets;
    hold(s, at + entry.bytes, true, true);
}
void hold_video(GoLiveSender& s) noexcept {
    u8* out = s.datagram;
    const u16 transport = s.transport_sequence++;
    const u32 header = 12 + video_extensions(out + 12, transport);
    bool marker = false;
    const u32 bytes =
        rtp_packetizer_next(&s.packetizer, {out + header, kGoLiveDatagramBytes - header}, &marker);
    if (!bytes) {
        s.frame_active = false;
        --s.transport_sequence;
        return;
    }
    const u16 sequence = s.video_sequence++;
    store_header(out, true, marker, s.streams.video_payload_type, sequence, s.frame_timestamp, s.streams.video_ssrc);
    auto& entry = s.history[sequence & (kGoLiveHistoryPackets - 1)];
    entry.retransmitted_us = 0;
    entry.timestamp = s.frame_timestamp;
    entry.sequence = sequence;
    entry.bytes = static_cast<u16>(bytes);
    entry.marker = marker;
    entry.valid = true;
    copy_bytes(entry.payload, out + header, bytes);
    s.video_octets += bytes;
    ++s.video_count;
    ++s.stats.video_packets;
    s.stats.video_bytes += header + bytes;
    if (marker)
        s.frame_active = false;
    hold(s, header + bytes, true, true);
}
void report(GoLiveSender& s, u64 now_us) noexcept {
    const u64 unix_us = now_unix_us();
    u32 seconds = 0, fraction = 0;
    rtcp_ntp_time(unix_us, &seconds, &fraction);
    u32 at = rtcp_write_sender_report({s.datagram, kGoLiveDatagramBytes}, 0,
                                      {s.streams.video_ssrc, seconds, fraction, static_cast<u32>(now_us * 9 / 100),
                                       s.video_count, s.video_octets});
    if (s.audio_total)
        at = rtcp_write_sender_report({s.datagram, kGoLiveDatagramBytes}, at,
                                      {s.streams.audio_ssrc, seconds, fraction,
                                       static_cast<u32>(now_us * 48 / 1000), s.audio_total, s.audio_octets});
    if (at)
        hold(s, at, false, false);
}
} // namespace

void go_live_bandwidth_reset(GoLiveBandwidth* bandwidth, StreamBitrate bounds, u64 now_us) noexcept {
    reset_value(bandwidth);
    bandwidth->bounds = bounds;
    bandwidth->target = bounds.start < bounds.minimum ? bounds.minimum : bounds.start;
    bandwidth->threshold = 12.5;
    bandwidth->last_update_us = bandwidth->last_threshold_us = now_us;
    bandwidth->overuse_since_us = -1;
}

void go_live_bandwidth_packet(GoLiveBandwidth* bandwidth, u64 send_us, i64 arrival_us, u32 bytes,
                              bool received) noexcept {
    auto& b = *bandwidth;
    if (!received) {
        ++b.lost;
        return;
    }
    ++b.received;
    if (!b.acked_since_us)
        b.acked_since_us = static_cast<u64>(arrival_us > 0 ? arrival_us : 1);
    b.acked_bytes += bytes;
    const u64 span = static_cast<u64>(arrival_us) > b.acked_since_us ? static_cast<u64>(arrival_us) - b.acked_since_us
                                                                      : 0;
    if (span >= 500'000) {
        b.acked_bps = static_cast<u32>(b.acked_bytes * 8'000'000 / span);
        b.acked_bytes = 0;
        b.acked_since_us = static_cast<u64>(arrival_us);
    }
    if (!b.has_first) {
        b.first_arrival_us = arrival_us;
        b.has_first = true;
    }
    const i64 send = static_cast<i64>(send_us);
    if (b.has_group && send - b.group_send_us < static_cast<i64>(kGroupUs) && send >= b.group_send_us) {
        b.group_arrival_us = arrival_us > b.group_arrival_us ? arrival_us : b.group_arrival_us;
        return;
    }
    if (b.has_group)
        group_done(b);
    b.group_send_us = send;
    b.group_arrival_us = arrival_us;
    b.has_group = true;
}

void go_live_bandwidth_update(GoLiveBandwidth* bandwidth, u64 now_us) noexcept {
    auto& b = *bandwidth;
    const double elapsed_s = static_cast<double>(now_us - b.last_update_us) / 1e6;
    b.last_update_us = now_us;
    // Delay: the trend slope scaled by the group count, against the adaptive threshold.
    bool overuse = false, underuse = false;
    if (b.trend_count >= kGoLiveTrendWindow) {
        const double trend = slope(b) * static_cast<double>(b.groups < 60 ? b.groups : 60) * 4.0;
        const double magnitude = absolute(trend);
        const double dt_ms = static_cast<double>(now_us - b.last_threshold_us) / 1000.0;
        b.last_threshold_us = now_us;
        if (magnitude < b.threshold + 15) {
            const double k = magnitude < b.threshold ? 0.039 : 0.0087;
            b.threshold += k * (magnitude - b.threshold) * (dt_ms < 100 ? dt_ms : 100);
            b.threshold = b.threshold < 6 ? 6 : b.threshold > 600 ? 600 : b.threshold;
        }
        if (trend > b.threshold) {
            if (b.overuse_since_us < 0)
                b.overuse_since_us = static_cast<i64>(now_us);
            ++b.overuse_hits;
            overuse = b.overuse_hits >= 2 &&
                      static_cast<double>(static_cast<i64>(now_us) - b.overuse_since_us) / 1000.0 >= kOveruseTimeMs;
        } else {
            b.overuse_since_us = -1;
            b.overuse_hits = 0;
            underuse = trend < -b.threshold;
        }
    }
    // Loss of this batch.
    const u32 packets = b.lost + b.received;
    const double loss = packets ? static_cast<double>(b.lost) / packets : 0;
    b.lost = b.received = 0;
    u64 rate = b.target;
    if (overuse) {
        rate = static_cast<u64>((b.acked_bps ? b.acked_bps : b.target) * 0.85);
        b.overuse_hits = 0;
        b.overuse_since_us = -1;
    } else if (loss > 0.10)
        rate = static_cast<u64>(b.target * (1.0 - 0.5 * loss));
    else if (!underuse && loss < 0.02) {
        rate = static_cast<u64>(b.target * (1.0 + 0.08 * (elapsed_s < 1 ? elapsed_s : 1)));
        // Never far beyond what actually got through (an estimate already above that is kept).
        const u64 cap = u64{b.acked_bps} * 3 / 2 + 10'000;
        if (b.acked_bps && rate > cap)
            rate = b.target > cap ? b.target : cap;
    }
    b.target = clamp_rate(b, rate);
}

void go_live_bandwidth_loss(GoLiveBandwidth* bandwidth, u8 fraction_lost, u64 now_us) noexcept {
    auto& b = *bandwidth;
    const double loss = fraction_lost / 256.0, elapsed_s = static_cast<double>(now_us - b.last_update_us) / 1e6;
    b.last_update_us = now_us;
    u64 rate = b.target;
    if (loss > 0.10)
        rate = static_cast<u64>(b.target * (1.0 - 0.5 * loss));
    else if (loss < 0.02)
        rate = static_cast<u64>(b.target * (1.0 + 0.08 * (elapsed_s < 1 ? elapsed_s : 1)));
    b.target = clamp_rate(b, rate);
}

void go_live_bandwidth_remb(GoLiveBandwidth* bandwidth, u64 bitrate) noexcept {
    bandwidth->remb = static_cast<u32>(bitrate > ~u32{0} ? ~u32{0} : bitrate);
    bandwidth->target = clamp_rate(*bandwidth, bandwidth->target);
}

bool go_live_sender_start(GoLiveSender* sender, const GoLiveSendStreams& streams, StreamBitrate bounds,
                          u64 now_us) noexcept {
    go_live_sender_stop(sender);
    auto* block = static_cast<u8*>(pages_allocate(kHistoryBytes + kGoLiveFrameBytes));
    u32 random[2]{};
    if (!block || !secure_random_fill({reinterpret_cast<u8*>(random), sizeof(random)}).ok()) {
        if (block)
            pages_release(block);
        return false;
    }
    sender->history = reinterpret_cast<GoLiveHistoryEntry*>(block);
    sender->frame = block + kHistoryBytes;
    sender->streams = streams;
    sender->video_sequence = static_cast<u16>(random[0]);
    sender->rtx_sequence = static_cast<u16>(random[0] >> 16);
    sender->audio_sequence = static_cast<u16>(random[1]);
    sender->transport_sequence = static_cast<u16>(random[1] >> 16);
    sender->next_report_us = now_us + kGoLiveReportIntervalUs;
    sender->pace_us = now_us - kBurstUs; // The first frame may start with one burst.
    go_live_bandwidth_reset(&sender->bandwidth, bounds, now_us);
    return true;
}

void go_live_sender_stop(GoLiveSender* sender) noexcept {
    if (sender->history)
        pages_release(sender->history);
    reset_value(sender);
}

Slice<u8> go_live_sender_frame_buffer(GoLiveSender* sender) noexcept {
    return sender->frame && !sender->frame_active ? Slice<u8>{sender->frame, kGoLiveFrameBytes} : Slice<u8>{};
}

bool go_live_sender_frame(GoLiveSender* sender, u32 bytes, u32 rtp_timestamp, bool keyframe) noexcept {
    if (!sender->frame || sender->frame_active || !bytes || bytes > kGoLiveFrameBytes ||
        !rtp_packetizer_begin(&sender->packetizer, sender->streams.codec, {sender->frame, bytes}, keyframe,
                              kRtpVideoMaximumPayload))
        return false;
    sender->frame_active = true;
    sender->keyframe = keyframe;
    sender->frame_timestamp = rtp_timestamp;
    ++sender->stats.frames;
    return true;
}

bool go_live_sender_audio(GoLiveSender* sender, ConstSlice<u8> packet, u32 rtp_timestamp) noexcept {
    if (sender->audio_count == kGoLiveAudioQueue || !packet.count || packet.count > sizeof(GoLiveAudioPacket::data))
        return false;
    auto& slot = sender->audio[(sender->audio_head + sender->audio_count) % kGoLiveAudioQueue];
    slot.timestamp = rtp_timestamp;
    slot.bytes = static_cast<u16>(packet.count);
    copy_bytes(slot.data, packet.data, packet.count);
    ++sender->audio_count;
    return true;
}

void go_live_sender_rtcp(GoLiveSender* sender, ConstSlice<u8> compound, u64 now_us) noexcept {
    FeedbackContext context{sender, now_us, false};
    rtcp_read_feedback(compound,
                       {&context, on_keyframe_proxy, on_nack_proxy, on_remb_proxy, on_report, on_transport});
}

u64 go_live_sender_pump(GoLiveSender* sender, const GoLiveSendLink& link, u64 now_us) noexcept {
    auto& s = *sender;
    if (!s.history)
        return 0;
    // Pacing budget: 2.5x the target, at most a 5 ms burst (and room for one packet).
    const u64 rate = u64{s.bandwidth.target} * 5 / 2;
    if (now_us > s.pace_us) {
        s.budget_bytes += static_cast<i64>(rate * (now_us - s.pace_us) / 8'000'000);
        const i64 burst = static_cast<i64>(rate * kBurstUs / 8'000'000) + kGoLiveDatagramBytes;
        if (s.budget_bytes > burst)
            s.budget_bytes = burst;
    }
    s.pace_us = now_us;
    for (;;) {
        if (!flush(s, link, now_us))
            return 0; // The transport's completion wakes the worker.
        if (s.audio_count) {
            hold_audio(s);
            continue;
        }
        if (now_us >= s.next_report_us && (s.video_count || s.audio_total)) {
            s.next_report_us = now_us + kGoLiveReportIntervalUs;
            report(s, now_us);
            continue;
        }
        if (!s.rtx_count && !s.frame_active)
            return s.video_count || s.audio_total ? s.next_report_us : 0;
        if (s.budget_bytes <= 0) {
            const u64 wait = rate ? (static_cast<u64>(-s.budget_bytes) + kGoLiveDatagramBytes) * 8'000'000 / rate : 1000;
            return now_us + (wait < 1000 ? 1000 : wait);
        }
        if (s.rtx_count)
            hold_rtx(s, now_us);
        else
            hold_video(s);
    }
}

bool go_live_sender_take_keyframe(GoLiveSender* sender) noexcept {
    const bool wanted = sender->keyframe_wanted;
    sender->keyframe_wanted = false;
    return wanted;
}

bool go_live_sender_idle(const GoLiveSender& sender) noexcept {
    return !sender.frame_active && !sender.datagram_bytes && !sender.rtx_count && !sender.audio_count;
}
} // namespace maicord
