// The arithmetic of the PulseAudio transport (src/platform/audio/pulse/timing.hpp): buffer attributes against the
// Windows engine timing, the playback timeline, the capture pacing and the reopen schedule, all driven by an injected
// clock (plain numbers: the functions read no clock). No server and no libpulse is involved.
#include "../../../src/platform/audio/pulse/pulse.hpp"
#include "../../../src/platform/audio/pulse/timing.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

// A fake server timeline: it plays what was written, 10 ms per tick, and stops advancing while it has nothing queued
// (a PulseAudio stream's audible position is read-index based), as the real stream timeline does.
struct FakeStream {
    u64 written_frames{}, played_frames{};
    u32 rate{48000};
    u64 microseconds() const noexcept {
        return pulse::microseconds_from_frames(played_frames, rate);
    }
    void tick(u32 milliseconds) noexcept {
        const u64 playable = pulse::frames_in_milliseconds(rate, milliseconds);
        const u64 queued = written_frames - played_frames;
        played_frames += playable < queued ? playable : queued;
    }
};
} // namespace

void test_pulse_timing(Check check) noexcept {
    using namespace pulse;
    check(frame_bytes(kSampleS16LE, 2) == 4 && frame_bytes(kSampleS16LE, 1) == 2 &&
              frame_bytes(kSampleFloat32LE, 2) == 8 && frame_bytes(0, 2) == 0 && frame_bytes(kSampleFloat32LE, 1) == 4,
          "pulse format: the frame sizes of the sample formats the transport uses, and none for any other");
    check(frames_in_milliseconds(48000, 10) == 480 && frames_in_milliseconds(48000, 20) == 960 &&
              frames_in_milliseconds(44100, 10) == 441 && frames_in_milliseconds(8000, 1) == 8 &&
              frames_in_milliseconds(192000, 40) == 7680,
          "pulse format: 10 ms is 480 samples at 48 kHz and whole frames at the other rates");

    // The voice render buffer: 20 ms of stereo PCM16, requested 10 ms at a time, started corked and never prebuffered.
    constexpr BufferAttr render = playback_attributes(kVoiceRate, 4, kRenderBufferMs, kVoicePeriodMs);
    check(render.tlength == 3840 && render.minreq == 1920 && render.prebuf == 0 && render.maxlength == kInvalid &&
              render.fragsize == kInvalid,
          "pulse buffering: the voice render buffer is 20 ms with 10 ms requests, as the WASAPI render buffer");
    // The voice capture buffer: 40 ms of mono PCM16, delivered in 10 ms (480 sample) fragments.
    constexpr BufferAttr capture = record_attributes(kVoiceRate, 2, kCaptureBufferMs, kVoicePeriodMs);
    check(capture.maxlength == 3840 && capture.fragsize == 960 && capture.tlength == kInvalid &&
              capture.prebuf == kInvalid && capture.minreq == kInvalid,
          "pulse buffering: the voice capture buffer is 40 ms with 480-sample fragments, as the WASAPI capture buffer");
    constexpr BufferAttr media = playback_attributes(44100, 8, kMediaBufferMs, kVoicePeriodMs);
    constexpr BufferAttr notification = playback_attributes(44100, 4, kNotificationBufferMs, kVoicePeriodMs);
    check(media.tlength == 1764 * 8 && media.minreq == 441 * 8 && notification.tlength == 882 * 4 &&
              notification.minreq == 441 * 4 && media.tlength % 8 == 0 && notification.tlength % 4 == 0,
          "pulse buffering: media and notification attributes are whole frames of their own rate and format");

    // The timeline.
    check(frames_from_microseconds(1'000'000, 48000) == 48000 && frames_from_microseconds(10'000, 48000) == 480 &&
              frames_from_microseconds(20'833, 48000) == 999 && frames_from_microseconds(0, 48000) == 0 &&
              frames_from_microseconds(1, 44100) == 0 && frames_from_microseconds(3, 500'000) == 1,
          "pulse timeline: microseconds convert to whole frames with one floor");
    check(frames_from_microseconds(~u64{0}, 4'000'000'000u) == ~u64{0} && frames_from_microseconds(~u64{0}, 1) < ~u64{0} &&
              microseconds_from_frames(48000, 48000) == 1'000'000 && microseconds_from_frames(1, 3) == 333'333 &&
              microseconds_from_frames(~u64{0}, 1) == ~u64{0} && microseconds_from_frames(5, 0) == 0,
          "pulse timeline: conversions saturate instead of wrapping and refuse a zero rate");

    // Playback against a fake server clock: written 10 ms at a time, played as the clock advances.
    FakeStream stream{};
    u64 consumed = 1000;
    const u64 anchor = consumed;
    u64 submitted = anchor;
    for (u32 step = 0; step < 5; ++step) {
        stream.written_frames += 480;
        submitted += 480;
        stream.tick(10);
        consumed = played_frames(anchor, stream.microseconds(), 48000, submitted, consumed);
    }
    check(consumed == anchor + 2400 && consumed == submitted,
          "pulse timeline: the played position follows the stream clock frame for frame from its anchor");
    stream.written_frames += 480;
    submitted += 480;
    stream.tick(4);
    check(played_frames(anchor, stream.microseconds(), 48000, submitted, consumed) == anchor + 2400 + 192,
          "pulse timeline: a partial tick advances by whole frames");
    // The stream underruns: nothing queued, so the clock stands still and the position does not creep on.
    stream.played_frames = stream.written_frames;
    const u64 final = played_frames(anchor, stream.microseconds(), 48000, submitted, 0);
    stream.tick(100);
    check(final == submitted && played_frames(anchor, stream.microseconds(), 48000, submitted, final) == submitted,
          "pulse timeline: an empty stream does not advance the position past what was submitted");
    check(played_frames(anchor, 10'000'000, 48000, submitted, 0) == submitted,
          "pulse timeline: the position never passes the frames submitted");
    check(played_frames(anchor, 0, 48000, submitted, anchor + 700) == anchor + 700,
          "pulse timeline: the position never goes back (a later, smaller stream time keeps the previous answer)");
    check(played_frames(~u64{0} - 10, 1'000'000, 48000, ~u64{0}, 0) == ~u64{0},
          "pulse timeline: anchor plus advance saturates");

    // Requests.
    check(request_frames(3840, 4, 4096) == 960 && request_frames(3840, 4, 480) == 480 && request_frames(3, 4, 480) == 0 &&
              request_frames(0, 4, 480) == 0 && request_frames(100, 0, 480) == 0 && request_frames(7, 4, 480) == 1,
          "pulse buffering: a server request becomes whole frames, capped by the fill limit");

    // The timer of the capture tick, with an injected clock: it never fires early and rounds up to the millisecond.
    check(due_milliseconds(1000, 1000) == 0 && due_milliseconds(900, 1000) == 0 && due_milliseconds(1001, 1000) == 1 &&
              due_milliseconds(11'000, 1000) == 10 && due_milliseconds(11'001, 1000) == 11 &&
              due_milliseconds(~u64{0}, 0) == 0x7FFFFFFFu,
          "pulse timing: the capture timer is armed in whole milliseconds, rounded up, 0 when due");
    // A 20 ms pacing loop on that clock: five ticks accumulate exactly 100 ms.
    u64 now = 5'000, wake = now, elapsed_ms = 0;
    for (u32 i = 0; i < 5; ++i) {
        wake += 20'000;
        const u32 wait = due_milliseconds(wake, now);
        elapsed_ms += wait;
        now += static_cast<u64>(wait) * 1000;
    }
    check(elapsed_ms == 100 && now == 105'000, "pulse timing: 20 ms pacing stays on the schedule it was given");

    // Reopen schedule.
    RetrySchedule retry{};
    check(retry_due(retry, 0), "pulse retry: a first open is due at once");
    retry_after(&retry, 2'000'000, 1'000'000);
    check(!retry_due(retry, 2'000'000) && !retry_due(retry, 2'999'999) && retry_due(retry, 3'000'000) &&
              retry_due(retry, 9'000'000),
          "pulse retry: a failed open is retried exactly after its delay");

    // The end of a stream: the server's confirmation, or the grace period after the request, whichever comes first.
    const u64 requested_at = 10'000, deadline = requested_at + kDrainGraceMs;
    check(kDrainGraceMs == 500 && !drain_finished(false, true, deadline, deadline + 1) &&
              !drain_finished(true, false, deadline, deadline - 1) && drain_finished(true, false, deadline, deadline) &&
              drain_finished(true, true, deadline, requested_at) && drain_finished(true, false, deadline, ~u64{0}),
          "pulse timing: a drain ends on the server's confirmation or after its grace period, never before a request");

    // Capture fragments: a 40 ms backlog is cut into the 480-sample pieces the sink is used to; a hole likewise.
    u32 pieces = 0, total = 0;
    for (u32 offset = 0; capture_piece(1920, offset, 480); offset += 480) {
        ++pieces;
        total += capture_piece(1920, offset, 480);
    }
    check(pieces == 4 && total == 1920 && capture_piece(500, 480, 480) == 20 && capture_piece(480, 480, 480) == 0 &&
              capture_piece(480, 9999, 480) == 0 && capture_piece(480, 0, 480) == 480,
          "pulse capture: a fragment is cut into 10 ms pieces with the remainder last");

    // Device IDs: PulseAudio names are ASCII; the shared UTF-16 settings hold them widened.
    wchar_t id[128];
    char name[128];
    check(device_id_from_name("alsa_output.pci-0000_00_1f.3.analog-stereo.monitor", id, 128) && id[0] == L'a' &&
              device_name_from_id(id, name, 128) && name[0] == 'a' && name[4] == '_' && name[5] == 'o',
          "pulse device: a sink name round-trips through the UTF-16 endpoint ID");
    wchar_t wide[]{L'a', 0x3bb, 0};
    check(!device_name_from_id(wide, name, 128) && !device_id_from_name("caf\xc3\xa9", id, 128) &&
              !device_id_from_name("0123456789", id, 5) && !device_name_from_id(L"0123456789", name, 5) &&
              device_id_from_name("", id, 1) && id[0] == 0 && !device_id_from_name("x", id, 0),
          "pulse device: non-ASCII IDs and names that do not fit are refused; the empty name selects the default");
}
} // namespace maicord
