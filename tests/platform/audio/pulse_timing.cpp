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
        return played_frames * 1'000'000 / rate;
    }
    void tick(u32 milliseconds) noexcept {
        const u64 playable = pulse::frames_in_milliseconds(rate, milliseconds);
        const u64 queued = written_frames - played_frames;
        played_frames += playable < queued ? playable : queued;
    }
};

// A server whose sink holds `latency_ms` of audio after it reads the stream's queue (WSLg's RDP sink reports 0.4 to
// 1 s). It reads 10 ms in each tick, the audible position is the read position less the latency, and the read index
// comes with a timing update once in 50 ticks. The client keeps a ring of 8192 frames, as the video session does, and
// frees it with `release`. Its producer always has data, and its worker keeps 30 ms queued on the server. The result
// is the count of ticks in which the server found less than 10 ms queued.
template <typename Release> u32 starved_ticks(u32 latency_ms, Release release) noexcept {
    constexpr u32 kRate = 48000, kRingFrames = 8192, kQueueFrames = 1440, kTickFrames = 480;
    const u64 latency = pulse::frames_in_milliseconds(kRate, latency_ms);
    u64 submitted = 0, read = 0, reported = 0, consumed = 0;
    u32 starved = 0;
    for (u32 tick = 0; tick < 300; ++tick) {
        const u64 queued = submitted - read;
        if (queued < kQueueFrames)
            submitted = minimum(submitted + kQueueFrames - queued, consumed + kRingFrames);
        if (submitted - read < kTickFrames)
            ++starved;
        read += minimum<u64>(kTickFrames, submitted - read);
        if (tick % 50 == 49)
            reported = read;
        const u64 heard = read > latency ? read - latency : 0;
        consumed = release(heard * 1'000'000 / kRate, u64{latency_ms} * 1000, reported, submitted, consumed);
    }
    return starved;
}

// A pipewire-pulse playback stream and the worker that feeds it, in frames at 48 kHz
// (module-protocol-pulse: stream_process, stream_pop_missing, stream_update_minreq). The server grants the 20 ms plan
// and the 40 ms media plan a target length of 1440 frames in 480-frame requests. In each cycle it reads one quantum.
// Without a prebuffer it moves its read position by a quantum even when less is queued. On the first cycle it raises
// the target to the quantum plus two requests. After each cycle it asks for what the target is missing, and the
// worker wakes once for that request.
// `limit` caps each write (0: request_frames alone). With `poll`, the worker also wakes every `poll` frames between
// two cycles, as the notification and media workers read their clocks. The first cycle comes 240 frames after the
// stream starts. With `prebuffer` (the media stream), a cycle that finds less than a quantum queued plays silence and
// leaves the read position where it is.
struct Fed {
    u32 late_underruns; // cycles after the first that found less than a quantum queued
    i64 late_heard;     // frames of the worker's audio played in the last 100 cycles
    i64 lost;           // frames the worker wrote that the server never played
};
Fed feed(u32 quantum, u32 limit, u32 poll, bool prebuffer) noexcept {
    constexpr i64 kGranted = 1440, kRequest = 480;
    constexpr u32 kCycles = 500;
    i64 target = kGranted, written = 0, read = 0, requested = kGranted, heard = 0;
    Fed fed{};
    const auto wake = [&]() noexcept {
        u32 frames = pulse::request_frames(static_cast<usize>(requested) * 4, 4);
        if (limit && frames > limit)
            frames = limit;
        written += frames;
        requested -= frames;
    };
    wake(); // the fill before the stream starts
    for (u32 cycle = 0; cycle < kCycles; ++cycle) {
        const u32 interval = cycle ? quantum : 240;
        for (u32 at = poll; poll && at < interval; at += poll)
            wake();
        if (!cycle && target < quantum + 2 * kRequest)
            target = quantum + 2 * kRequest;
        const i64 queued = written - read;
        const i64 played = queued >= quantum ? quantum : prebuffer || queued <= 0 ? 0 : queued;
        if (cycle && queued < quantum)
            ++fed.late_underruns;
        if (!prebuffer || queued >= quantum)
            read += quantum;
        heard += played;
        if (cycle >= kCycles - 100)
            fed.late_heard += played;
        const i64 missing = target - requested - (written - read);
        if (missing >= kRequest) {
            requested += missing;
            wake();
        }
    }
    fed.lost = written - heard - (written > read ? written - read : 0);
    return fed;
}
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
    constexpr BufferAttr media = media_attributes(44100, 8);
    constexpr BufferAttr notification = playback_attributes(44100, 4, kNotificationBufferMs, kVoicePeriodMs);
    check(media.tlength == 1764 * 8 && media.minreq == 441 * 8 && notification.tlength == 882 * 4 &&
              notification.minreq == 441 * 4 && media.tlength % 8 == 0 && notification.tlength % 4 == 0,
          "pulse buffering: media and notification attributes are whole frames of their own rate and format");
    check(media.prebuf == kInvalid && media.maxlength == kInvalid && notification.prebuf == 0,
          "pulse buffering: the media stream asks for the server's prebuffer, so a stream that runs dry drops nothing");

    // The timeline.
    check(frames_from_microseconds(1'000'000, 48000) == 48000 && frames_from_microseconds(10'000, 48000) == 480 &&
              frames_from_microseconds(20'833, 48000) == 999 && frames_from_microseconds(0, 48000) == 0 &&
              frames_from_microseconds(1, 44100) == 0 && frames_from_microseconds(3, 500'000) == 1,
          "pulse timeline: microseconds convert to whole frames with one floor");
    check(frames_from_microseconds(~u64{0}, 4'000'000'000u) == ~u64{0} && frames_from_microseconds(~u64{0}, 1) < ~u64{0},
          "pulse timeline: the conversion saturates instead of wrapping");

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

    // The frames the server has read, which the ring frees. A ring freed only at the audible position holds the
    // sink's latency too, and an 8192-frame ring is shorter than WSLg's latency.
    const auto release = [](u64 stream_us, u64 sink_us, u64 read, u64 submitted_frames, u64 previous) noexcept {
        return read_frames(0, stream_us, sink_us, read, 48000, submitted_frames, previous);
    };
    check(starved_ticks(600, release) == 0 && starved_ticks(20, release) == 0,
          "pulse read position: a ring of 8192 frames keeps a server fed behind a 600 ms sink latency and a 20 ms one");
    check(read_frames(1000, 100'000, 600'000, 48000, 48000, 1000 + 48100, 0) == 1000 + 48000,
          "pulse read position: the read index stands in for an audible position that stands still");
    check(read_frames(1000, 100'000, 600'000, 0, 48000, 1000 + 48100, 0) == 1000 + 33600 &&
              read_frames(1000, 100'000, 600'000, 0, 48000, 1000 + 20000, 0) == 1000 + 20000 &&
              read_frames(1000, 100'000, 600'000, 90000, 48000, 1000 + 48100, 0) == 1000 + 48100,
          "pulse read position: between updates it is the audible position plus the sink latency, never past submitted");
    check(read_frames(1000, 0, 0, 0, 48000, 5000, 3000) == 3000 &&
              read_frames(~u64{0} - 10, ~u64{0}, ~u64{0}, ~u64{0}, 48000, ~u64{0}, 0) == ~u64{0},
          "pulse read position: it never goes back, and the sums saturate");

    // Requests: a worker writes all that the server asks for when it wakes.
    check(request_frames(3840, 4) == 960 && request_frames(3, 4) == 0 && request_frames(0, 4) == 0 &&
              request_frames(100, 0) == 0 && request_frames(7, 4) == 1 && request_frames(~usize{0}, 1) == ~u32{0},
          "pulse buffering: a server request becomes whole frames, all of them, and saturates");
    // The voice stream at server quanta of 10, 21 and 43 ms (clock.force-quantum, pulse.min.quantum), with one wake for
    // each request. A whole request at each wake keeps the queue ahead after the first cycle. That cycle drops only
    // what a quantum above the granted 1440 frames reads past. The old 20 ms cap (960 frames) keeps up at 480 frames.
    // In a larger cycle it falls behind by 64 or 1088 frames each time and soon plays nothing.
    const u32 quanta[]{480, 1024, 2048};
    for (u32 quantum : quanta) {
        const Fed whole = feed(quantum, 0, 0, false);
        check(whole.late_underruns == 0 && whole.late_heard == 100 * i64{quantum} &&
                  whole.lost == (quantum > 1440 ? quantum - 1440 : 0),
              "pulse buffering: voice output keeps ahead of a server that reads up to 2048 frames per cycle");
        const Fed capped = feed(quantum, 960, 0, false);
        check(quantum <= 960 ? capped.late_underruns == 0 && capped.late_heard == 100 * i64{quantum}
                             : capped.late_underruns == 499 && capped.late_heard == 0,
              "pulse buffering: the old 20 ms cap keeps up at 480 frames per cycle and goes silent above 960");
    }
    // The notification stream: the same plan, and the worker also reads its clock every 10 ms (480 frames) while it
    // plays. With the old 20 ms cap the poll keeps the queue ahead after the first cycle too. But the fill before the
    // start held 960 frames, not the granted 1440. A first cycle of 1024 frames then lost 64 frames of the sound, and
    // one of 2048 frames lost 1088 frames, not the 608 that a quantum above the granted target reads past.
    const Fed sound_1024 = feed(1024, 0, 480, false), capped_1024 = feed(1024, 960, 480, false);
    const Fed sound_2048 = feed(2048, 0, 480, false), capped_2048 = feed(2048, 960, 480, false);
    check(sound_1024.late_underruns == 0 && sound_1024.lost == 0 && sound_2048.late_underruns == 0 &&
              sound_2048.lost == 608,
          "pulse buffering: a notification sound loses no more at its start than the server's first cycle reads past");
    check(capped_1024.late_underruns == 0 && capped_1024.lost == 64 && capped_2048.late_underruns == 0 &&
              capped_2048.lost == 1088,
          "pulse buffering: the old 20 ms cap lost 64 or 1088 frames at the start of a sound");
    // The media stream: a prebuffer, at most 40 ms (1920 frames) at each wake, and a clock read every 10 ms. The
    // clock reads write the rest of a 2048-frame request long before the next cycle, so the cap costs one more write
    // and no audio. With one wake for each request it would fall 128 frames behind in each cycle and soon stop.
    const Fed playing = feed(2048, 1920, 480, true), request_wakes = feed(2048, 1920, 0, true);
    check(playing.late_underruns == 0 && playing.late_heard == 100 * 2048 && playing.lost == 0 &&
              request_wakes.late_heard == 0,
          "pulse buffering: the media stream's 40 ms cap keeps up with 2048-frame cycles through its 10 ms clock reads");

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
    check(media_open_retry_us(1) == 1'000'000 && media_open_retry_us(2) == 2'000'000 && media_open_retry_us(3) == 0 &&
              media_open_retry_us(0) == 0 && kMediaOpenRetries == 2,
          "pulse retry: the media stream retries a server twice, 1 s and then 2 s after a failure");
    check(server_unreachable(kErrorServerTimeout) && server_unreachable(kErrorPulse | pulse::kErrorConnectionRefused) &&
              !server_unreachable(kErrorPulse) && !server_unreachable(kErrorStopped) &&
              !server_unreachable(kErrorLibraryMissing) && !server_unreachable(kErrorPulse | 1) &&
              !server_unreachable(0),
          "pulse retry: only a server that did not answer or refused the connection is tried again");

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
