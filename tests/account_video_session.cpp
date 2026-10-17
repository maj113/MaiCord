#include "maicord/app/account_video_session.hpp"
#include "maicord/platform/media_audio.hpp"
#include "maicord/media/encoded_audio.hpp"
#include "fixtures/mp3_original_tone.hpp"

namespace maicord {
namespace {
bool cancel(void* context) noexcept {
    ++*static_cast<u32*>(context);
    return true;
}
u8 opus_scratch[kOggOpusMaximumPacketBytes];
u32 ogg_page(u8* output, u32 sequence, u8 flags, u64 granule, const u8* packet, u8 count) noexcept {
    const u32 bytes = 28u + count;
    for (u32 i = 0; i < bytes; ++i)
        output[i] = 0;
    const char capture[] = "OggS";
    for (u32 i = 0; i < 4; ++i)
        output[i] = static_cast<u8>(capture[i]);
    output[5] = flags;
    for (u32 i = 0; i < 8; ++i)
        output[6 + i] = static_cast<u8>(granule >> (i * 8));
    output[14] = 123;
    for (u32 i = 0; i < 4; ++i)
        output[18 + i] = static_cast<u8>(sequence >> (i * 8));
    output[26] = 1;
    output[27] = count;
    for (u32 i = 0; i < count; ++i)
        output[28 + i] = packet[i];
    u32 crc = 0;
    for (u32 i = 0; i < bytes; ++i) {
        crc ^= static_cast<u32>(output[i]) << 24;
        for (u32 bit = 0; bit < 8; ++bit)
            crc = (crc << 1) ^ ((crc & 0x80000000u) ? 0x04c11db7u : 0u);
    }
    for (u32 i = 0; i < 4; ++i)
        output[22 + i] = static_cast<u8>(crc >> (i * 8));
    return bytes;
}
bool cancel_before_decode(void* context) noexcept {
    return ++*static_cast<u32*>(context) == 3;
}
void write_le(u8* output, u32 value, u32 bytes) noexcept {
    for (u32 i = 0; i < bytes; ++i)
        output[i] = static_cast<u8>(value >> (8 * i));
}
void mp3_session(void (*check)(bool, const char*) noexcept, const AccountVideoIdentity& id) noexcept {
    Mp3Stream source{};
    const bool parsed =
        mp3_stream_initialize(&source, {kMp3OriginalTone, sizeof(kMp3OriginalTone)}) == Mp3Error::none;
    EncodedAudioMetadata metadata{source};
    check(parsed && metadata.mp3.total_frames == 5760 && metadata.mp3.sample_rate == 44100 &&
              metadata.mp3.channels == 1,
          "MP3 session: original encoded tone captures exact raw frame duration without guessed gapless "
          "trim");
    if (!parsed)
        return;
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    AccountVideoSession session{};
    const auto initialized = account_video_session_initialize_audio(&session, &metadata, id, event);
    check(initialized.completed && session.status.kind == AccountPlaybackKind::mp3 && !session.video &&
              session.encoded_metadata == &metadata && !session.workspace && session.status.has_audio,
          "MP3 session: generic retained metadata owns no H264 PCM decoder endpoint or thread");
    if (!initialized.ok()) {
        if (event)
            CloseHandle(event);
        return;
    }
    const auto sought = account_video_session_control(&session, id, 1, AccountVideoCommand::seek, 226757);
    check(sought.admitted && session.status.position_100ns == 226757 && session.status.seeking &&
              account_video_session_owned_bytes(&session) > 0 &&
              account_video_session_owned_bytes(&session) <= 128 * 1024 && !session.video,
          "MP3 session: paused seek lazily owns bounded decoder reservoir index and native PCM ring");
    u32 checks{};
    const auto interrupted = account_video_session_service(&session, cancel_before_decode, &checks);
    check(interrupted.interrupted && checks == 3 &&
              !account_video_session_audio_progress(&session).written_frames,
          "MP3 session: cancellation between frame advance and decode restores exact compressed source "
          "cursor");
    account_video_session_service(&session);
    const auto progress = account_video_session_audio_progress(&session);
    check(session.status.error == AccountVideoError::none && progress.written_frames == 3608 &&
              !progress.consumed_frames && !session.status.seeking && session.status.paused &&
              !session.status.playing && !session.status.clock_valid && !session.status.ended &&
              !session.status.frame_pending &&
              !account_video_session_acknowledge(&session, id, session.playback_epoch, 1),
          "MP3 session: real decoder discards exact seek prefix and queues four bounded writes without "
          "GPU or playback completion");
    for (u32 i = 0; i < 3; ++i)
        account_video_session_service(&session);
    check(account_video_session_audio_progress(&session).written_frames == progress.written_frames &&
              !session.status.ended,
          "MP3 session: completed paused seek performs no idle decode or invented completion");
    auto stale = id;
    ++stale.message_revision;
    check(account_video_session_control(&session, stale, 2, AccountVideoCommand::stop).error ==
                  AccountVideoError::stale &&
              account_video_session_audio_progress(&session).written_frames == progress.written_frames,
          "MP3 session: stale source revision cannot reset current reservoir or ring epoch");
    const auto tail = account_video_session_control(&session, id, 2, AccountVideoCommand::seek, 1044897);
    account_video_session_service(&session);
    check(tail.admitted && session.status.error == AccountVideoError::none && session.status.paused &&
              !session.status.ended && !session.status.seeking &&
              account_video_session_audio_progress(&session).written_frames == 1152,
          "MP3 session: exact final packet seek discards reservoir preroll and seals only the retained "
          "source tail");
    const auto endpoint = account_video_session_control(&session, id, 3, AccountVideoCommand::seek,
                                                        session.status.duration_100ns);
    account_video_session_service(&session);
    check(endpoint.admitted && session.status.ended &&
              !account_video_session_audio_progress(&session).written_frames,
          "MP3 session: exact audible endpoint seals empty epoch without default native device");
    const auto stopped = account_video_session_control(&session, id, 4, AccountVideoCommand::stop);
    account_video_session_service(&session);
    check(stopped.admitted && session.status.error == AccountVideoError::none && session.status.paused &&
              !session.status.ended &&
              account_video_session_audio_progress(&session).written_frames == 4608,
          "MP3 session: stopped replay resets codec reservoir and primes four exact source blocks while "
          "paused");
    u32 stop_checks{};
    session.source_cancelled = cancel;
    session.source_cancel_context = &stop_checks;
    const u32 retained_bytes = account_video_session_owned_bytes(&session);
    const auto cancelled_seek =
        account_video_session_control(&session, id, 5, AccountVideoCommand::seek, 226757);
    check(!cancelled_seek.ok() && stop_checks && session.status.error == AccountVideoError::demux &&
              session.status.native_error == static_cast<u32>(Mp3Error::cancelled) &&
              account_video_session_owned_bytes(&session) == retained_bytes &&
              session.encoded_metadata == &metadata,
          "MP3 session: lane stop cancels seek scan and retains decoder plus immutable source through "
          "joined release");
    check(account_video_session_release(&session, 0).completed && !session.workspace &&
              !session.initialized,
          "MP3 session: joined release retires bounded codec ring index and borrowed source");
    CloseHandle(event);
}
void wave_session(void (*check)(bool, const char*) noexcept, const AccountVideoIdentity& id) noexcept {
    u8 encoded[44 + 600 * 4]{};
    const char riff[] = "RIFF", wave[] = "WAVE", fmt[] = "fmt ", data[] = "data";
    for (u32 i = 0; i < 4; ++i) {
        encoded[i] = static_cast<u8>(riff[i]);
        encoded[8 + i] = static_cast<u8>(wave[i]);
        encoded[12 + i] = static_cast<u8>(fmt[i]);
        encoded[36 + i] = static_cast<u8>(data[i]);
    }
    write_le(encoded + 4, sizeof(encoded) - 8, 4);
    write_le(encoded + 16, 16, 4);
    write_le(encoded + 20, 1, 2);
    write_le(encoded + 22, 2, 2);
    write_le(encoded + 24, 44100, 4);
    write_le(encoded + 28, 44100 * 4, 4);
    write_le(encoded + 32, 4, 2);
    write_le(encoded + 34, 16, 2);
    write_le(encoded + 40, 600 * 4, 4);
    for (u32 i = 0; i < 600; ++i) {
        write_le(encoded + 44 + i * 4, 0x4000, 2);
        write_le(encoded + 46 + i * 4, 0xc000, 2);
    }
    WavePcmSource source{};
    const bool parsed = wave_pcm_parse(&source, {encoded, sizeof(encoded)}) == WavePcmError::none;
    EncodedAudioMetadata metadata{source};
    check(parsed && metadata.wave.total_frames == 600 && metadata.wave.channels == 2,
          "WAV session: original stereo PCM fixture preserves exact source frame metadata");
    if (!parsed)
        return;
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    AccountVideoSession session{};
    const auto initialized = account_video_session_initialize_audio(&session, &metadata, id, event);
    check(initialized.completed && session.status.kind == AccountPlaybackKind::wave_pcm &&
              session.encoded_metadata == &metadata && session.status.has_audio && !session.video &&
              !session.workspace && session.status.duration_100ns == 136054,
          "WAV session: borrowed generic metadata initializes without decoder GPU endpoint or PCM");
    if (!initialized.ok()) {
        if (event)
            CloseHandle(event);
        return;
    }
    check(account_video_session_control(&session, id, 1, AccountVideoCommand::volume, 0, .25f).admitted &&
              !session.workspace && !session.status.volume_valid,
          "WAV session: desired volume before playback performs no native initialization");
    check(account_video_session_control(&session, id, 2, AccountVideoCommand::seek, 22675).admitted &&
              session.status.position_100ns == 22675 && session.status.seeking &&
              account_video_session_owned_bytes(&session) <= 84 * 1024 && !session.video,
          "WAV session: fractional-rate seek selects exact source frame in bounded lazy PCM workspace");
    u32 checks{};
    const auto interrupted = account_video_session_service(&session, cancel, &checks);
    check(interrupted.interrupted && checks &&
              !account_video_session_audio_progress(&session).written_frames,
          "WAV session: cancellation preserves pending source frame and exposes no invented PCM");
    const auto serviced = account_video_session_service(&session);
    const auto progress = account_video_session_audio_progress(&session);
    check(!serviced.frame_ready && progress.written_frames == 500 && !progress.consumed_frames &&
              session.status.error == AccountVideoError::none && session.status.paused &&
              !session.status.playing && !session.status.clock_valid && !session.status.ended &&
              !session.status.seeking &&
              !account_video_session_acknowledge(&session, id, session.playback_epoch, 1),
          "WAV session: paused native ring queues exact remaining interleaved PCM without playback or GPU "
          "acknowledgement");
    auto stale = id;
    ++stale.cache_generation;
    check(account_video_session_control(&session, stale, 3, AccountVideoCommand::seek, 0).error ==
                  AccountVideoError::stale &&
              account_video_session_audio_progress(&session).written_frames == 500,
          "WAV session: stale retained content cannot reset its exact audio epoch");
    const auto endpoint = account_video_session_control(&session, id, 3, AccountVideoCommand::seek,
                                                        session.status.duration_100ns);
    account_video_session_service(&session);
    check(endpoint.admitted && session.status.ended &&
              !account_video_session_audio_progress(&session).written_frames,
          "WAV session: exact endpoint seals empty epoch without source tail or default native device");
    const auto stopped = account_video_session_control(&session, id, 4, AccountVideoCommand::stop);
    account_video_session_service(&session);
    check(stopped.admitted && session.status.paused && !session.status.ended &&
              account_video_session_audio_progress(&session).written_frames == 600 &&
              session.status.position_100ns == 0 && session.status.error == AccountVideoError::none,
          "WAV session: stop resets source and transport epoch for exact replay while remaining paused");
    check(account_video_session_release(&session, 0).completed && !session.workspace &&
              !account_video_session_owned_bytes(&session),
          "WAV session: joined release retires caller PCM workspace and borrowed immutable source");
    CloseHandle(event);
}
void opus_session(void (*check)(bool, const char*) noexcept, const AccountVideoIdentity& id) noexcept {
    u8 encoded[128]{}, head[19]{}, tags[16]{};
    const char head_magic[] = "OpusHead", tags_magic[] = "OpusTags";
    for (u32 i = 0; i < 8; ++i) {
        head[i] = static_cast<u8>(head_magic[i]);
        tags[i] = static_cast<u8>(tags_magic[i]);
    }
    head[8] = head[9] = 1;
    head[10] = 120;
    head[12] = 0x80;
    head[13] = 0xbb;
    head[17] = 0xff; // -1 dB Q8 header gain, separate from user volume.
    const u8 silence[]{0xf8, 0xff, 0xfe};
    u32 bytes = ogg_page(encoded, 0, 2, 0, head, 19);
    bytes += ogg_page(encoded + bytes, 1, 0, 0, tags, 16);
    bytes += ogg_page(encoded + bytes, 2, 4, 880, silence, 3);
    OggOpusDemuxer demux{};
    const bool parsed =
        ogg_opus_demux_initialize(&demux, {encoded, bytes}, {opus_scratch, sizeof(opus_scratch)}) ==
        OggOpusError::none;
    check(parsed && demux.pre_skip == 120 && demux.output_gain_q8 == -256 && demux.total_frames == 760,
          "Opus session: real CRC-validated compact stream captures pre-skip gain and final granule "
          "trimming");
    if (!parsed)
        return;
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    AccountVideoSession session{};
    EncodedAudioMetadata generic{demux};
    const auto generic_initialized = account_video_session_initialize_audio(&session, &generic, id, event);
    check(generic_initialized.completed && session.encoded_metadata == &generic &&
              session.status.kind == AccountPlaybackKind::ogg_opus &&
              !session.workspace && account_video_session_release(&session, 0).completed,
          "Opus session: generic retained source preserves legacy audio kind and metadata-only release");
    const auto initialized = account_video_session_initialize_audio(&session, &generic, id, event);
    check(initialized.completed && session.status.kind == AccountPlaybackKind::ogg_opus &&
              session.status.has_audio && session.status.duration_100ns == 158333 && !session.video &&
              !session.workspace && !session.status.frame_pending,
          "Opus session: metadata initialization owns no H264 GPU PCM endpoint or worker");
    if (!initialized.ok()) {
        if (event)
            CloseHandle(event);
        return;
    }
    check(account_video_session_control(&session, id, 1, AccountVideoCommand::volume, 0, .4f).admitted &&
              !session.workspace && !session.status.volume_valid && session.status.desired_volume == .4f,
          "Opus session: user gain before playback is independent desired metadata without native "
          "acknowledgement");
    check(account_video_session_control(&session, id, 2, AccountVideoCommand::seek, 50000).admitted &&
              session.status.position_100ns == 50000 && session.status.seeking &&
              account_video_session_owned_bytes(&session) > 0 &&
              account_video_session_owned_bytes(&session) <= 336 * 1024 && !session.video,
          "Opus session: paused sample-accurate seek allocates bounded decoder storage without starting "
          "audio");
    u32 checks{};
    const auto interrupted = account_video_session_service(&session, cancel_before_decode, &checks);
    check(interrupted.interrupted && checks == 3 && !interrupted.frame_ready &&
              !account_video_session_audio_progress(&session).written_frames,
          "Opus session: cancellation after demux before native decode preserves the packet for resumed "
          "service");
    const auto serviced = account_video_session_service(&session);
    const auto progress = account_video_session_audio_progress(&session);
    check(!serviced.frame_ready && session.status.error == AccountVideoError::none &&
              !session.status.seeking && session.status.paused && !session.status.playing &&
              !session.status.ended && !session.status.clock_valid &&
              progress.epoch == session.playback_epoch && progress.written_frames == 520 &&
              !progress.consumed_frames &&
              !account_video_session_acknowledge(&session, id, session.playback_epoch, 1),
          "Opus session: real decoder queues only seek/pre-skip/end-trim PCM while paused and fabricates "
          "no GPU or playback completion");
    auto stale = id;
    ++stale.session_epoch;
    const auto before = account_video_session_audio_progress(&session);
    check(account_video_session_control(&session, stale, 3, AccountVideoCommand::seek, 0).error ==
                  AccountVideoError::stale &&
              account_video_session_audio_progress(&session).written_frames == before.written_frames,
          "Opus session: stale source/session cannot reset the retained PCM epoch");
    const bool eof_seek = account_video_session_control(&session, id, 3, AccountVideoCommand::seek,
                                                        session.status.duration_100ns)
                              .admitted;
    const auto eof = account_video_session_service(&session);
    check(eof_seek && !eof.frame_ready && session.status.ended &&
              session.status.error == AccountVideoError::none &&
              !account_video_session_audio_progress(&session).written_frames,
          "Opus session: seeking exact audible endpoint seals an empty fresh epoch without an audio device "
          "or frame lease");
    u32 stop_checks{};
    session.source_cancelled = cancel;
    session.source_cancel_context = &stop_checks;
    const u32 retained_bytes = account_video_session_owned_bytes(&session);
    const auto cancelled_seek =
        account_video_session_control(&session, id, 4, AccountVideoCommand::seek, 0);
    check(!cancelled_seek.ok() && stop_checks && session.status.error == AccountVideoError::demux &&
              session.status.native_error == static_cast<u32>(OggOpusError::cancelled) &&
              account_video_session_owned_bytes(&session) == retained_bytes &&
              session.encoded_metadata == &generic,
          "Opus session: lane-stop cancels demux scan and retains source/decoder storage until release");
    check(account_video_session_release(&session, 0).completed && !session.initialized &&
              !session.workspace && !account_video_session_owned_bytes(&session),
          "Opus session: metadata-only native ring and decoder source retire through joined release");
    if (event)
        CloseHandle(event);
}
} // namespace
void test_account_video_session(void (*check)(bool, const char*) noexcept) noexcept {
    const Mp4AudioFrameTimeBounds final_lc{97291609, 97291610};
    check(account_video_audio_timestamp_within_bounds(97291609, final_lc) &&
              account_video_audio_timestamp_within_bounds(97291610, final_lc) &&
              !account_video_audio_timestamp_within_bounds(97291608, final_lc) &&
              !account_video_audio_timestamp_within_bounds(97291611, final_lc),
          "selected video AAC drain: native final block may floor or ceil its exact fractional source "
          "time, not drift");
    check(account_video_audio_timestamp_within_bounds(0, {0, 0}) &&
              !account_video_audio_timestamp_within_bounds(-1, {0, 0}) &&
              !account_video_audio_timestamp_within_bounds(1, {0, 0}) &&
              account_video_audio_timestamp_within_bounds(-213333, {-213334, -213333}) &&
              !account_video_audio_timestamp_within_bounds(2, {0, 2}) &&
              !account_video_audio_timestamp_within_bounds(0, {1, 0}) &&
              !account_video_audio_timestamp_within_bounds(
                  0, {0x7fffffffffffffffll, (-0x7fffffffffffffffll - 1)}),
          "selected video AAC bounds: signed fractional rounding is bounded; integer-exact and invalid "
          "bounds refuse offsets");
    check(account_video_audio_timestamp_matches(19839999, 19840000, 19626666, 1024, 48000),
          "selected video AAC timestamp: final native stamp equals preceding exact mapped stamp plus "
          "floored 1024-frame duration");
    check(account_video_audio_timestamp_matches(-1, 0, -213334, 1024, 48000),
          "selected video AAC timestamp: signed preroll uses the same proved floor-of-parts bound");
    check(account_video_audio_timestamp_matches(5108389, 5108390, 4643990, 2048, 44100) &&
              account_video_audio_timestamp_matches(-1, 0, -464400, 2048, 44100) &&
              !account_video_audio_timestamp_matches(5108388, 5108390, 4643990, 2048, 44100) &&
              !account_video_audio_timestamp_matches(5108391, 5108390, 4643990, 2048, 44100),
          "selected video HE-AAC timestamp: doubled PCM frame clock retains exact one-unit decomposition "
          "without relaxing drift");
    check(!account_video_audio_timestamp_matches(19839998, 19840000, 19626666, 1024, 48000) &&
              !account_video_audio_timestamp_matches(19840001, 19840000, 19626666, 1024, 48000) &&
              !account_video_audio_timestamp_matches(1199999, 1200000, 1000000, 960, 48000) &&
              !account_video_audio_timestamp_matches(-1, 0, 0, 0, 48000),
          "selected video AAC timestamp: drift, future stamps, exact-duration offsets and first-block "
          "guessed priming remain refused");
    check(!account_video_audio_timestamp_matches(1, 2, 0x7fffffffffffffffll, 1024, 48000) &&
              !account_video_audio_timestamp_matches(1, 2, 0, 1024, 0) &&
              !account_video_audio_timestamp_matches(1, 2, 0, 4097, 48000) &&
              account_video_audio_timestamp_matches(0, 0, 0, 0, 48000),
          "selected video AAC timestamp: overflow and invalid decomposition refuse; exact native timestamp "
          "needs no invented predecessor");
    check(!account_video_audio_should_play(true, true, false) &&
              account_video_audio_should_play(true, true, true),
          "selected video wake: future-frame or EOF readiness starts paused full-ring audio without a "
          "nonexistent copy acknowledgement");
    check(!account_video_audio_should_play(false, true, true) &&
              account_video_audio_should_play(true, false, false),
          "selected video wake: paused seeks stay paused and ordinary playback does not depend on a seek "
          "readiness latch");
    MediaAudioStatus audio{};
    audio.desired_epoch = 2;
    audio.observed_epoch = 1;
    audio.playing = audio.clock_valid = audio.volume_valid = true;
    const auto old = account_video_audio_observation(audio, 2);
    check(!old.epoch_ready && !old.playing && !old.clock_valid && !old.volume_valid,
          "selected video epoch: prior stream still playing during reset cannot publish a new-epoch clock "
          "or frame readiness");
    audio.observed_epoch = 2;
    const auto current = account_video_audio_observation(audio, 2);
    check(
        current.epoch_ready && current.playing && current.clock_valid && current.volume_valid,
        "selected video epoch: readiness follows observed native reset rather than merely requested epoch");
    audio.playing = false;
    audio.ended = true;
    audio.written_frames = 1024;
    audio.consumed_frames = 0;
    check(!account_video_audio_observation(audio, 2).ended,
          "selected video EOF: sealed but queued unplayed PCM is not completed playback");
    audio.consumed_frames = 1024;
    check(account_video_audio_observation(audio, 2).ended,
          "selected video EOF: native stopped and fully consumed PCM completes the movie audio");
    AccountVideoIdentity id{11, 22, 33, 44, 0, 5, 0};
    check(account_video_identity_equal(id, id),
          "selected video identity: fully resolved revision-zero source is a valid exact identity");
    auto other = id;
    other.session_epoch = 45;
    check(!account_video_identity_equal(id, other),
          "selected video identity: settings/message reuse cannot reuse another session epoch");
    other = id;
    other.content_identity = 34;
    check(!account_video_identity_equal(id, other),
          "selected video identity: changed encoded content cannot reuse an old control");
    other = id;
    other.cache_generation = 6;
    check(!account_video_identity_equal(id, other),
          "selected video identity: cache replacement invalidates old frame acknowledgement");
    other = id;
    other.message_revision = 1;
    check(!account_video_identity_equal(id, other),
          "selected video identity: source message revision is part of admission");
    other = id;
    other.account_serial = 12;
    check(!account_video_identity_equal(id, other),
          "selected video identity: account change invalidates controls even for equal message bytes");
    other = id;
    other.reserved = 1;
    check(!account_video_identity_equal(id, other) && !account_video_identity_equal({}, {}),
          "selected video identity: unresolved and reserved-field identities cannot admit commands");
    check(account_video_frame_due(-400000, 800000, 0) == AccountVideoFrameDue::ready &&
              account_video_frame_due(-400000, 400000, 0) == AccountVideoFrameDue::expired,
          "selected video timing: negative preroll picture overlapping movie start is visible, one ending "
          "at zero is discarded");
    check(account_video_frame_due(400000, 400000, 399999) == AccountVideoFrameDue::future &&
              account_video_frame_due(400000, 400000, 400000) == AccountVideoFrameDue::ready &&
              account_video_frame_due(400000, 400000, 800000) == AccountVideoFrameDue::expired,
          "selected video timing: frame delivery uses its exact interval rather than a frame-counter "
          "animation clock");
    check(account_video_frame_due(0, 0, 0) == AccountVideoFrameDue::invalid &&
              account_video_frame_due(0, 1, -1) == AccountVideoFrameDue::invalid &&
              account_video_frame_due(0x7fffffffffffffffll, 1, 0) == AccountVideoFrameDue::invalid,
          "selected video timing: missing/overflowed intervals do not become fake due pictures");
    i64 position = 77;
    check(account_video_qpc_position(5000000, 100, 160, 60, &position) && position == 15000000 &&
              account_video_qpc_position(5000000, 160, 160, 60, &position) && position == 5000000,
          "selected video clock: audio-less clock advances from its preserved movie anchor and freezes "
          "exactly on pause");
    position = 77;
    check(!account_video_qpc_position(0, 160, 159, 60, &position) && position == 77 &&
              !account_video_qpc_position(0, 0, 1, 0, &position) && position == 77 &&
              !account_video_qpc_position(0x7fffffffffffffffll, 0, 1, 1, &position) && position == 77,
          "selected video clock: regressions, missing frequency and overflow refuse without replacing the "
          "last position");
    AccountVideoSession empty{};
    Mp4Decoder video{};
    const auto no_audio = account_video_session_audio_progress(&empty);
    check(!no_audio.epoch && !no_audio.written_frames && !no_audio.submitted_frames &&
              !no_audio.consumed_frames,
          "selected video progress: inactive owner reports no invented native PCM consumption");
    check(account_video_session_initialize(&empty, &video, id, nullptr).error ==
                  AccountVideoError::invalid_input &&
              !empty.initialized && !empty.workspace && !account_video_session_owned_bytes(&empty),
          "selected video init: absent ready decoder/event cannot allocate PCM, initialize MF or open an "
          "endpoint");
    // Offline controller scaffolding contains only caller-owned metadata, with
    // no decoder/source/native allocation. Exercise guards and configuration;
    // never call Play, Seek, a video read, native AAC submit or audio rendering.
    AccountVideoSession s{};
    s.initialized = true;
    s.thread_id = GetCurrentThreadId();
    s.status.identity = id;
    s.status.duration_100ns = 5000000;
    s.qpc_frequency = 1000;
    const auto volume = account_video_session_control(&s, id, 1, AccountVideoCommand::volume, 0, .4f);
    check(volume.admitted && volume.completed && s.status.desired_volume == .4f &&
              s.status.observed_command == 1 && !s.workspace && !s.status.volume_valid,
          "selected video controls: audio-less desired volume is metadata, never a claimed hardware volume "
          "acknowledgement");
    const auto before = s;
    check(account_video_session_control(&s, other, 2, AccountVideoCommand::pause).error ==
                  AccountVideoError::stale &&
              bytes_equal(&before, &s, sizeof(s)),
          "selected video controls: wrong identity cannot mutate another active owner or its receipt");
    check(account_video_session_control(&s, id, 1, AccountVideoCommand::pause).error ==
                  AccountVideoError::stale &&
              s.status.rejected_command == 1 && s.status.rejected_error == AccountVideoError::stale &&
              s.status.desired_command == 1 && s.status.observed_command == 1 &&
              s.playback_epoch == before.playback_epoch && !s.workspace,
          "selected video controls: replayed serial gets a separate refusal receipt without admitting or "
          "acknowledging it");
    check(account_video_session_control(&s, id, 2, AccountVideoCommand::seek, 5000001).error ==
                  AccountVideoError::invalid_input &&
              s.status.rejected_command == 2 &&
              s.status.rejected_error == AccountVideoError::invalid_input &&
              s.status.error == AccountVideoError::none && s.status.desired_command == 1 &&
              s.status.observed_command == 1 && s.playback_epoch == before.playback_epoch && !s.workspace,
          "selected video controls: rejected seek persists its receipt without mutating source cursor or "
          "playback epoch");
    s.published = s.held = s.status.frame_pending = true;
    s.status.frame_serial = 7;
    check(account_video_session_control(&s, id, 2, AccountVideoCommand::stop).error ==
                  AccountVideoError::busy &&
              s.status.rejected_error == AccountVideoError::busy && s.status.frame_pending &&
              s.status.desired_command == 1 && s.status.frame_serial == 7 &&
              !account_video_session_acknowledge(&s, id, s.playback_epoch + 1, 7) &&
              s.status.frame_pending && !account_video_session_acknowledge(&s, id, s.playback_epoch, 6) &&
              s.status.frame_pending,
          "selected video lease: stop and mismatched acknowledgements cannot revoke an outstanding "
          "copied-frame fence");
    s.held = s.published = s.status.frame_pending = false;
    u32 called{};
    const auto service = account_video_session_service(&s, cancel, &called);
    check(called == 1 && service.interrupted && !service.frame_ready && !s.workspace,
          "selected video cancellation: command cancellation is checked before native codec/device/read "
          "work");
    check(account_video_session_status(&s).rejected_error == AccountVideoError::busy &&
              account_video_session_status(&s).rejected_command == 2,
          "selected video refusal: ordinary service/status snapshots preserve rejected-command receipt");
    check(account_video_session_control(&s, id, 3, AccountVideoCommand::volume, 0, .5f).admitted &&
              s.status.rejected_command == 0 && s.status.rejected_error == AccountVideoError::none &&
              s.status.desired_command == 3,
          "selected video refusal: the next admitted exact-session command clears its prior receipt");
    const auto released = account_video_session_release(&s, 0);
    check(
        released.completed && !s.initialized && !s.workspace &&
            account_video_session_release(&empty, 0).completed,
        "selected video release: pure inactive metadata cleanup owns no worker, endpoint or borrowed PCM");
    opus_session(check, id);
    wave_session(check, id);
    mp3_session(check, id);
}
} // namespace maicord
