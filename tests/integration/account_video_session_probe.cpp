// Account media session playback (MaiCordAccountVideoSessionProbe --audio): Opus, MP3 and WAV memory streams play
// through a real WASAPI endpoint, then pause, seek, change volume, mute, end, replay and stop. It is not in the
// default suite: it runs only with -AudioPlaybackProbe (maicord_account_audio_playback). No network, credential,
// registry, enrollment or output file.
#include "maicord/app/media/account_video_session.hpp"
#include "maicord/media/audio/encoded_audio.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "../support/ogg_page_fixture.hpp"
#include "../support/account_video_session_probe.hpp"

namespace maicord {
namespace {
struct Probe {
    AccountVideoSession session{};
    OggOpusDemuxer opus_metadata{};
    EncodedAudioMetadata audio_metadata{};
    AccountVideoIdentity identity{1, 2, 3, 4, 0, 1, 0};
    HANDLE lane{};
    u8* input{};
    u32 input_bytes{}, stage{}, native{};
    u64 deadline{}, serial{}, max_consumed{};
    bool retained{}, joined{}, played{}, paused{}, sought{}, volume{}, muted{}, unmuted{}, ended{}, replayed{},
        stopped{}, opus{};
    u64 opus_total_frames{}, opus_end_written{}, opus_end_consumed{};
    u64 audio_total_frames{}, audio_end_written{}, audio_end_consumed{};
};
Probe g_probe{}; // Failed join retains every borrowed owner until process exit.
void metric(const char* key, u64 value) noexcept {
    char out[128]{};
    u32 count{};
    while (*key && count < 90)
        out[count++] = *key++;
    out[count++] = '=';
    char digits[20]{};
    u32 n{};
    do {
        digits[n++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value && n < 20);
    while (n)
        out[count++] = digits[--n];
    out[count++] = '\n';
    DWORD written{};
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), out, count, &written, nullptr);
}
bool fail(Probe& p, u32 native) noexcept {
    p.native = native;
    return false;
}
void measure(Probe& p) noexcept {
    p.max_consumed = maximum(p.max_consumed, account_video_session_audio_progress(&p.session).consumed_frames);
}
enum class Until : u8 {
    playing,
    paused,
    seek,
    volume,
    mute,
    unmute,
    end,
    replay,
    stop
};
bool reached(Probe& p, Until until) noexcept {
    const auto s = account_video_session_status(&p.session);
    const auto a = account_video_session_audio_progress(&p.session);
    if (s.observed_command != p.serial)
        return false;
    switch (until) {
    case Until::playing:
    case Until::replay:
        return s.playing && s.clock_valid && a.epoch == s.playback_epoch && a.consumed_frames > 0;
    case Until::paused:
        return s.paused && !s.playing && s.clock_valid;
    case Until::seek:
        return !s.seeking && s.paused && !s.playing && s.position_100ns == 5000000;
    case Until::volume:
    case Until::unmute:
        return s.volume_valid && s.applied_volume == .25f && !s.muted;
    case Until::mute:
        return s.volume_valid && s.applied_volume == 0 && s.muted;
    case Until::end:
        return s.ended && !s.playing && a.written_frames && a.consumed_frames == a.written_frames;
    case Until::stop:
        return s.paused && !s.playing && !s.seeking && s.position_100ns == 0;
    }
    return false;
}
bool pump(Probe& p, Until until) noexcept {
    for (u32 turn = 0; turn < 65536; ++turn) {
        if (GetTickCount64() >= p.deadline)
            return fail(p, WAIT_TIMEOUT);
        const auto service = account_video_session_service(&p.session);
        // An audio session never publishes a picture.
        if (service.frame_ready)
            return fail(p, ERROR_INVALID_DATA);
        const auto s = account_video_session_status(&p.session);
        measure(p);
        if (s.error != AccountVideoError::none)
            return fail(p, s.native_error);
        if (reached(p, until))
            return true;
        if (service.needs_service)
            continue;
        const u64 now = GetTickCount64();
        if (now >= p.deadline)
            return fail(p, WAIT_TIMEOUT);
        const DWORD timeout =
            static_cast<DWORD>(minimum(p.deadline - now, u64{service.next_service_delay_ms}));
        const DWORD waited = WaitForSingleObject(p.lane, timeout);
        if (waited == WAIT_FAILED)
            return fail(p, GetLastError());
        if (waited != WAIT_OBJECT_0 && waited != WAIT_TIMEOUT)
            return fail(p, ERROR_INVALID_DATA);
    }
    return fail(p, ERROR_INVALID_DATA);
}
bool command(Probe& p, AccountVideoCommand kind, i64 target = 0, float gain = 1,
             bool muted = false) noexcept {
    const auto value =
        account_video_session_control(&p.session, p.identity, ++p.serial, kind, target, gain, muted);
    if (!value.admitted || !value.ok())
        return fail(p, value.native_error);
    return true;
}
bool option(const wchar_t* wanted) noexcept {
    const wchar_t* cursor = GetCommandLineW();
    if (!cursor)
        return false;
    bool quoted = false;
    while (*cursor) {
        if (*cursor == L'"')
            quoted = !quoted;
        else if (!quoted && (*cursor == L' ' || *cursor == L'\t'))
            break;
        ++cursor;
    }
    while (*cursor == L' ' || *cursor == L'\t')
        ++cursor;
    while (*wanted) {
        if (*cursor != *wanted)
            return false;
        ++cursor;
        ++wanted;
    }
    while (*cursor == L' ' || *cursor == L'\t')
        ++cursor;
    return !*cursor;
}
bool build_opus(Probe& p) noexcept {
    constexpr u32 packets = 40, encoded_bytes = 47 + 44 + 31 * packets;
    p.input =
        static_cast<u8*>(VirtualAlloc(nullptr, encoded_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!p.input)
        return fail(p, GetLastError());
    u8 head[19]{}, tags[16]{};
    constexpr char head_magic[] = "OpusHead", tags_magic[] = "OpusTags";
    for (u32 i = 0; i < 8; ++i) {
        head[i] = static_cast<u8>(head_magic[i]);
        tags[i] = static_cast<u8>(tags_magic[i]);
    }
    head[8] = head[9] = 1;
    head[10] = 120;
    head[12] = 0x80;
    head[13] = 0xbb;
    head[17] = 0xff;
    constexpr u8 silence[]{0xf8, 0xff, 0xfe};
    p.input_bytes = ogg_opus_page(p.input, 0, 2, 0, head, 19);
    p.input_bytes += ogg_opus_page(p.input + p.input_bytes, 1, 0, 0, tags, 16);
    for (u32 i = 0; i < packets; ++i) {
        const bool last = i + 1 == packets;
        p.input_bytes += ogg_opus_page(p.input + p.input_bytes, i + 2, last ? 4 : 0,
                                       u64{i + 1} * 960 - (last ? 80 : 0), silence, 3);
    }
    const auto parsed = ogg_opus_demux_initialize(&p.opus_metadata, {p.input, p.input_bytes}, {});
    if (parsed != OggOpusError::none)
        return fail(p, static_cast<u32>(parsed));
    p.opus_total_frames = packets * 960 - 120 - 80;
    if (p.input_bytes != encoded_bytes || p.opus_metadata.total_frames != p.opus_total_frames ||
        p.opus_metadata.pre_skip != 120 || p.opus_metadata.output_gain_q8 != -256)
        return fail(p, ERROR_INVALID_DATA);
    p.audio_metadata = EncodedAudioMetadata{p.opus_metadata};
    return true;
}
void little16(u8* bytes, u16 value) noexcept {
    bytes[0] = static_cast<u8>(value);
    bytes[1] = static_cast<u8>(value >> 8);
}
void little32(u8* bytes, u32 value) noexcept {
    for (u32 i = 0; i < 4; ++i)
        bytes[i] = static_cast<u8>(value >> (8 * i));
}
bool build_mp3(Probe& p) noexcept {
    constexpr u32 packets = 31, packet_bytes = 417;
    p.input_bytes = packets * packet_bytes;
    p.input =
        static_cast<u8*>(VirtualAlloc(nullptr, p.input_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!p.input)
        return fail(p, GetLastError());
    // Authored MPEG1 LayerIII mono44.1kHz128kbps: zero side information and
    // main data encode zero granules, with no reservoir or invented delay tag.
    for (u32 i = 0; i < packets; ++i) {
        auto* frame = p.input + i * packet_bytes;
        frame[0] = 0xff;
        frame[1] = 0xfb;
        frame[2] = 0x90;
        frame[3] = 0xc0;
    }
    Mp3Stream source{};
    const auto parsed = mp3_stream_initialize(&source, {p.input, p.input_bytes});
    if (parsed != Mp3Error::none)
        return fail(p, static_cast<u32>(parsed));
    if (source.sample_rate != 44100 || source.channels != 1 || source.frame_count != packets ||
        source.total_frames != u64{packets} * 1152 || source.delay_frames || source.padding_frames ||
        source.gapless)
        return fail(p, ERROR_INVALID_DATA);
    p.audio_metadata = EncodedAudioMetadata{source};
    return true;
}
bool build_wave(Probe& p) noexcept {
    constexpr u32 rate = 96000, frames = 76800, data_bytes = frames * 2;
    p.input_bytes = 44 + data_bytes;
    p.input =
        static_cast<u8*>(VirtualAlloc(nullptr, p.input_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!p.input)
        return fail(p, GetLastError());
    copy_bytes(p.input, "RIFF", 4);
    little32(p.input + 4, p.input_bytes - 8);
    copy_bytes(p.input + 8, "WAVEfmt ", 8);
    little32(p.input + 16, 16);
    little16(p.input + 20, 1);
    little16(p.input + 22, 1);
    little32(p.input + 24, rate);
    little32(p.input + 28, rate * 2);
    little16(p.input + 32, 2);
    little16(p.input + 34, 16);
    copy_bytes(p.input + 36, "data", 4);
    little32(p.input + 40, data_bytes);
    WavePcmSource source{};
    const auto parsed = wave_pcm_parse(&source, {p.input, p.input_bytes});
    if (parsed != WavePcmError::none)
        return fail(p, static_cast<u32>(parsed));
    if (source.sample_rate != rate || source.channels != 1 || source.total_frames != frames ||
        source.container_bits != 16 || source.block_align != 2)
        return fail(p, ERROR_INVALID_DATA);
    p.audio_metadata = EncodedAudioMetadata{source};
    return true;
}
bool run_audio(Probe& p, MediaAttachmentFormat format) noexcept {
    p.opus = format == MediaAttachmentFormat::ogg_opus;
    if (!p.deadline)
        p.deadline = GetTickCount64() + 2500;
    p.stage = 30;
    const bool built = p.opus                                 ? build_opus(p)
                       : format == MediaAttachmentFormat::mp3 ? build_mp3(p)
                       : format == MediaAttachmentFormat::wav ? build_wave(p)
                                                              : false;
    if (!built || !encoded_audio_ready(p.audio_metadata))
        return false;
    p.audio_total_frames = encoded_audio_total_frames(p.audio_metadata);
    const u32 rate = encoded_audio_rate(p.audio_metadata);
    const auto expected_kind = p.opus                                 ? AccountPlaybackKind::ogg_opus
                               : format == MediaAttachmentFormat::mp3 ? AccountPlaybackKind::mp3
                                                                      : AccountPlaybackKind::wave_pcm;
    p.lane = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!p.lane)
        return fail(p, GetLastError());
    const auto initialized =
        account_video_session_initialize_audio(&p.session, &p.audio_metadata, p.identity, p.lane);
    if (!initialized.completed || !initialized.ok())
        return fail(p, initialized.native_error);
    if (p.session.video || account_video_session_owned_bytes(&p.session) ||
        p.session.status.kind != expected_kind || !p.session.status.has_audio ||
        p.session.status.duration_100ns !=
            static_cast<i64>(encoded_audio_duration_units(p.audio_metadata, 10000000)))
        return fail(p, ERROR_INVALID_DATA);
    p.stage = 31;
    if (!command(p, AccountVideoCommand::volume, 0, .1f) || !command(p, AccountVideoCommand::play) ||
        !pump(p, Until::playing))
        return false;
    p.played = true;
    if (!account_video_session_audio_progress(&p.session).consumed_frames)
        return fail(p, ERROR_INVALID_DATA);
    p.stage = 32;
    if (!command(p, AccountVideoCommand::pause) || !pump(p, Until::paused))
        return false;
    p.paused = true;
    const auto paused = account_video_session_status(&p.session);
    const auto paused_audio = account_video_session_audio_progress(&p.session);
    Sleep(75);
    account_video_session_service(&p.session);
    const auto still = account_video_session_status(&p.session);
    if (!still.paused || still.playing || still.position_100ns != paused.position_100ns ||
        account_video_session_audio_progress(&p.session).consumed_frames != paused_audio.consumed_frames)
        return fail(p, ERROR_INVALID_DATA);
    p.stage = 33;
    const u64 before_epoch = p.session.playback_epoch;
    if (!command(p, AccountVideoCommand::seek, 5000000) || !pump(p, Until::seek))
        return false;
    p.sought = true;
    const auto sought = account_video_session_audio_progress(&p.session);
    if (sought.epoch <= before_epoch || sought.consumed_frames)
        return fail(p, ERROR_INVALID_DATA);
    p.stage = 34;
    if (!command(p, AccountVideoCommand::play) || !pump(p, Until::playing))
        return false;
    p.stage = 35;
    if (!command(p, AccountVideoCommand::volume, 0, .25f) || !pump(p, Until::volume))
        return false;
    p.volume = true;
    if (!command(p, AccountVideoCommand::mute, 0, 1, true) || !pump(p, Until::mute))
        return false;
    p.muted = true;
    if (!command(p, AccountVideoCommand::mute, 0, 1, false) || !pump(p, Until::unmute))
        return false;
    p.unmuted = true;
    p.stage = 36;
    if (!pump(p, Until::end))
        return false;
    p.ended = true;
    const auto ended = account_video_session_audio_progress(&p.session);
    p.audio_end_written = ended.written_frames;
    p.audio_end_consumed = ended.consumed_frames;
    if (p.opus) {
        p.opus_end_written = ended.written_frames;
        p.opus_end_consumed = ended.consumed_frames;
    }
    if (ended.written_frames != p.audio_total_frames - rate / 2 ||
        ended.consumed_frames != ended.written_frames || ended.submitted_frames != ended.written_frames)
        return fail(p, ERROR_INVALID_DATA);
    p.stage = 37;
    if (!command(p, AccountVideoCommand::play) || !pump(p, Until::replay))
        return false;
    p.replayed = true;
    p.stage = 38;
    if (!command(p, AccountVideoCommand::stop) || !pump(p, Until::stop))
        return false;
    p.stopped = true;
    return p.max_consumed > 0 && !p.session.status.frame_pending && !p.session.video &&
           p.session.status.error == AccountVideoError::none;
}
bool release(Probe& p) noexcept {
    // The only producer is this quiescent entry thread. No event or input is freed if the session's join retains
    // the owner.
    const u64 now = GetTickCount64();
    const u32 join_ms = static_cast<u32>(minimum(u64{1000}, p.deadline > now ? p.deadline - now : 0));
    const auto value = account_video_session_release(&p.session, join_ms);
    if (!value.completed || !value.ok()) {
        p.retained = true;
        return fail(p, value.native_error);
    }
    if (p.session.initialized || p.session.workspace || p.session.lane_event || p.session.encoded_metadata ||
        account_video_session_owned_bytes(&p.session)) {
        p.retained = true;
        return fail(p, ERROR_INVALID_DATA);
    }
    p.joined = true;
    if (p.lane) {
        if (!CloseHandle(p.lane)) {
            p.retained = true;
            return fail(p, GetLastError());
        }
        p.lane = nullptr;
    }
    if (p.input) {
        if (!VirtualFree(p.input, 0, MEM_RELEASE)) {
            p.retained = true;
            return fail(p, GetLastError());
        }
        p.input = nullptr;
    }
    return true;
}
void report(const Probe& p, bool okay) noexcept {
    metric("pass", okay);
    metric("stage", p.stage);
    metric("native_error", p.native);
    metric("joined", p.joined);
    metric("retained", p.retained);
    metric("opus_mode", p.opus);
    metric("opus_total_frames", p.opus_total_frames);
    metric("opus_end_written_frames", p.opus_end_written);
    metric("opus_end_consumed_frames", p.opus_end_consumed);
    metric("audio_total_frames", p.audio_total_frames);
    metric("audio_end_written_frames", p.audio_end_written);
    metric("audio_end_consumed_frames", p.audio_end_consumed);
    metric("played", p.played);
    metric("paused", p.paused);
    metric("sought", p.sought);
    metric("volume", p.volume);
    metric("muted", p.muted);
    metric("unmuted", p.unmuted);
    metric("ended", p.ended);
    metric("replayed", p.replayed);
    metric("stopped", p.stopped);
}
bool run_audio_suite(Probe& p) noexcept {
    const u64 deadline = GetTickCount64() + 4500;
    constexpr MediaAttachmentFormat formats[]{MediaAttachmentFormat::ogg_opus, MediaAttachmentFormat::mp3,
                                              MediaAttachmentFormat::wav};
    u32 completed = 0;
    for (const auto format : formats) {
        p = Probe{}; // Previous source/event/workspace has joined before reuse.
        p.deadline = deadline;
        p.audio_metadata.format = format;
        const bool ran = GetTickCount64() < deadline && run_audio(p, format);
        const u32 stage = p.stage, native = p.native;
        const bool released = release(p);
        if (released) {
            p.stage = stage;
            p.native = native;
        }
        const bool okay = ran && released && p.played && p.paused && p.sought && p.volume && p.muted &&
                          p.unmuted && p.ended && p.replayed && p.stopped && p.joined && !p.retained &&
                          p.audio_end_written && p.audio_end_consumed == p.audio_end_written;
        report(p, okay);
        if (!okay) {
            metric("audio_cases_completed", completed);
            return false;
        }
        ++completed;
    }
    metric("audio_cases_completed", completed);
    metric("audio_suite_pass", completed == 3);
    return completed == 3;
}
} // namespace
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_account_video_session_probe_entry() noexcept {
    ExitProcess(maicord::option(L"--audio") && maicord::run_audio_suite(maicord::g_probe) ? 0 : 1);
}
