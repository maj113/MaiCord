#include "maicord/app/voice_jitter_buffer.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
constexpr u32 kFrameUs = 20'000;
struct Arrival {
    u16 sequence;
    u32 at_ms;
};
struct Expected {
    VoiceJitterPop kind;
    u16 sequence; // Packet sequence, or the lost sequence.
    i32 next;     // Sequence carried for FEC on a loss, -1 when none.
};
struct Case {
    const char* name;
    Arrival arrivals[12];
    u32 arrival_count;
    Expected pops[12];
    u32 pop_count;
};
u8 payload(u16 sequence) noexcept {
    return static_cast<u8>(sequence * 7 + 3);
}
VoiceJitterPush push(VoiceJitterBuffer& buffer, u16 sequence, u64 at_us) noexcept {
    const u8 bytes[4]{0xfc, payload(sequence), 1, 2};
    return voice_jitter_push(&buffer, sequence, static_cast<u32>(sequence) * 960u, {bytes, 4}, at_us);
}
bool matches(const VoiceJitterFrame& frame, const Expected& expected) noexcept {
    if (frame.kind != expected.kind)
        return false;
    if (frame.kind == VoiceJitterPop::buffering)
        return true;
    if (frame.sequence != expected.sequence)
        return false;
    if (frame.kind == VoiceJitterPop::packet)
        return frame.packet.count == 4 && frame.packet[1] == payload(expected.sequence) && !frame.next.count;
    return expected.next < 0 ? !frame.next.count
                             : frame.next.count == 4 && frame.next[1] == payload(static_cast<u16>(expected.next));
}
constexpr auto P = VoiceJitterPop::packet, L = VoiceJitterPop::lost, B = VoiceJitterPop::buffering;
// Every arrival is queued before the pops run (the playout point starts after buffering).
const Case kCases[]{
    {"Jitter: in-order packets play in sequence after the 40 ms target fills",
     {{100, 0}, {101, 20}, {102, 40}}, 3, {{P, 100, -1}, {P, 101, -1}, {P, 102, -1}}, 3},
    {"Jitter: reordered arrivals play in sequence order",
     {{100, 0}, {102, 20}, {101, 25}, {104, 40}, {103, 45}}, 5,
     {{P, 100, -1}, {P, 101, -1}, {P, 102, -1}, {P, 103, -1}, {P, 104, -1}}, 5},
    {"Jitter: a single gap is reported lost with the following packet for FEC",
     {{100, 0}, {101, 20}, {103, 60}, {104, 80}}, 4,
     {{P, 100, -1}, {P, 101, -1}, {L, 102, 103}, {P, 103, -1}, {P, 104, -1}}, 5},
    {"Jitter: a burst gap conceals each frame; FEC only for the last one",
     {{100, 0}, {101, 20}, {104, 80}}, 3,
     {{P, 100, -1}, {P, 101, -1}, {L, 102, -1}, {L, 103, 104}, {P, 104, -1}}, 5},
    {"Jitter: sequence numbers wrap at 16 bits",
     {{65534, 0}, {65535, 20}, {0, 40}, {1, 60}}, 4,
     {{P, 65534, -1}, {P, 65535, -1}, {P, 0, -1}, {P, 1, -1}}, 4},
    {"Jitter: one packet stays buffering below the target depth", {{100, 0}}, 1, {{B, 0, -1}, {B, 0, -1}}, 2},
    {"Jitter: playout starts at the oldest buffered packet, not the first arrival",
     {{102, 0}, {101, 5}, {103, 20}}, 3, {{P, 101, -1}, {P, 102, -1}, {P, 103, -1}}, 3},
};
void run_cases(Check check) noexcept {
    static VoiceJitterBuffer buffer;
    for (const auto& value : kCases) {
        voice_jitter_reset(&buffer);
        for (u32 i = 0; i < value.arrival_count; ++i)
            push(buffer, value.arrivals[i].sequence, u64{value.arrivals[i].at_ms} * 1000);
        bool ok = true;
        for (u32 i = 0; i < value.pop_count; ++i)
            ok = ok && matches(voice_jitter_pop(&buffer), value.pops[i]);
        check(ok, value.name);
    }
}
void run_late_duplicate(Check check) noexcept {
    static VoiceJitterBuffer buffer;
    voice_jitter_reset(&buffer);
    const u8 large[kVoiceJitterPacketBytes + 1]{};
    check(push(buffer, 100, 0) == VoiceJitterPush::stored && push(buffer, 100, 1000) == VoiceJitterPush::duplicate &&
              push(buffer, 101, 20'000) == VoiceJitterPush::stored &&
              voice_jitter_push(&buffer, 102, 0, {large, 0}, 0) == VoiceJitterPush::rejected &&
              voice_jitter_push(&buffer, 102, 0, {large, sizeof(large)}, 0) == VoiceJitterPush::rejected &&
              buffer.stats.duplicate == 1 && buffer.stats.rejected == 2 && buffer.count == 2,
          "Jitter: duplicates, empty and oversized packets are refused without consuming slots");
    voice_jitter_pop(&buffer);
    voice_jitter_pop(&buffer);
    check(push(buffer, 100, 60'000) == VoiceJitterPush::late && push(buffer, 101, 60'000) == VoiceJitterPush::late &&
              push(buffer, 102, 60'000) == VoiceJitterPush::stored && buffer.stats.late == 2,
          "Jitter: packets behind the playout point are dropped as late");
    check(push(buffer, 102 + kVoiceJitterSlots + 4, 80'000) == VoiceJitterPush::resync && buffer.count == 1 &&
              !buffer.playing && push(buffer, 7, 90'000) == VoiceJitterPush::resync && buffer.next_sequence == 7,
          "Jitter: jumps beyond the window or far behind resynchronize instead of stalling");
}
void run_underrun(Check check) noexcept {
    static VoiceJitterBuffer buffer;
    voice_jitter_reset(&buffer);
    push(buffer, 10, 0);
    push(buffer, 11, 20'000);
    bool ok = voice_jitter_pop(&buffer).kind == P && voice_jitter_pop(&buffer).kind == P;
    // The next packet is merely late: conceal in place, then play it, not drop it.
    const auto first = voice_jitter_pop(&buffer);
    ok = ok && first.kind == L && first.sequence == 12 && push(buffer, 12, 70'000) == VoiceJitterPush::stored;
    const auto resumed = voice_jitter_pop(&buffer);
    check(ok && resumed.kind == P && resumed.sequence == 12,
          "Jitter: an empty buffer conceals without advancing, so a late packet still plays");
    u32 concealed = 0;
    VoiceJitterFrame frame{};
    while ((frame = voice_jitter_pop(&buffer)).kind == L)
        ++concealed;
    check(concealed == kVoiceJitterConcealFrames && frame.kind == B && !buffer.playing,
          "Jitter: talk-spurt end conceals a bounded run and then goes silent (DTX)");
    // The server continues the sequence after silence; the next spurt must not be late.
    push(buffer, 13, 900'000);
    push(buffer, 14, 920'000);
    const auto spurt = voice_jitter_pop(&buffer);
    check(spurt.kind == P && spurt.sequence == 13 && buffer.stats.late == 0,
          "Jitter: the next talk spurt re-buffers and resumes at its first packet");
}
void run_adaptive(Check check) noexcept {
    static VoiceJitterBuffer steady, jittery;
    voice_jitter_reset(&steady);
    voice_jitter_reset(&jittery);
    for (u16 i = 0; i < 64; ++i) {
        push(steady, i, u64{i} * kFrameUs);
        // Alternating +-12 ms arrival error (about 24 ms of transit jitter).
        push(jittery, i, u64{i} * kFrameUs + (i & 1 ? 24'000 : 0));
        voice_jitter_pop(&steady);
        voice_jitter_pop(&jittery);
    }
    check(steady.target == kVoiceJitterMinimumTarget && steady.jitter_us < 1000 &&
              jittery.target == kVoiceJitterMaximumTarget && jittery.jitter_us > 15'000,
          "Jitter: RFC 3550 estimate keeps 40 ms for steady arrivals and grows to 80 ms under jitter");
    static VoiceJitterBuffer backlog;
    voice_jitter_reset(&backlog);
    for (u16 i = 0; i < 12; ++i)
        push(backlog, i, 0);
    const auto played = voice_jitter_pop(&backlog);
    check(played.kind == P && played.sequence > 0 && backlog.count <= backlog.target + 4 && backlog.stats.trimmed,
          "Jitter: a backlog beyond the target is trimmed to bound latency");
    check(sizeof(VoiceJitterBuffer) < 24 * 1024, "Jitter: per-stream memory is fixed and bounded");
}
} // namespace
void test_voice_jitter_buffer(Check check) noexcept {
    run_cases(check);
    run_late_duplicate(check);
    run_underrun(check);
    run_adaptive(check);
}
} // namespace maicord
