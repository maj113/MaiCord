// Run the shipping dispatch and public lease/command boundaries with real
// Win32 synchronization. Native decoder/session operations are deterministic
// seams; no codec, GPU, audio endpoint, account or network is created.
#include "../src/app/account_media_video_internal.hpp"
#include "maicord/core/memory.hpp"
#include <Windows.h>
#include <objbase.h>

namespace maicord {
AccountVideoResult dispatch_fixture_initialize(AccountVideoSession*, Mp4Decoder*,
                                               const AccountVideoIdentity&, HANDLE,
                                               Mp4SourceAvailability = {}) noexcept;
AccountVideoResult dispatch_fixture_initialize_audio(AccountVideoSession*, const EncodedAudioMetadata*,
                                                     const AccountVideoIdentity&, HANDLE) noexcept;
AccountVideoResult dispatch_fixture_control(AccountVideoSession*, const AccountVideoIdentity&, u64,
                                            AccountVideoCommand, i64, float, bool) noexcept;
AccountVideoService dispatch_fixture_service(AccountVideoSession*, bool (*)(void*) noexcept,
                                             void*) noexcept;
bool dispatch_fixture_acknowledge(AccountVideoSession*, const AccountVideoIdentity&, u64, u64) noexcept;
AccountVideoStatus dispatch_fixture_status(const AccountVideoSession*) noexcept;
AccountVideoResult dispatch_fixture_release(AccountVideoSession*, u32) noexcept;
bool dispatch_fixture_decoder_acknowledge(Mp4Decoder*) noexcept;
bool dispatch_fixture_matches(const AccountMedia*, MediaAssetKey, u16) noexcept;
bool dispatch_fixture_audio_matches(const AccountMedia*, MediaAssetKey, u16) noexcept;
} // namespace maicord
#define account_media_video_reserved dispatch_fixture_reserved
#define account_media_worker_video_select dispatch_fixture_select
#define account_media_worker_video_control dispatch_fixture_submit
#define account_media_worker_video_status dispatch_fixture_snapshot
#define account_media_worker_video_take dispatch_fixture_take
#define account_media_worker_video_acknowledge dispatch_fixture_return
#define account_media_video_dispatch dispatch_fixture_dispatch
#define account_media_video_destroy dispatch_fixture_destroy
#define account_media_video_offer_url dispatch_fixture_offer_url
#define account_video_session_initialize dispatch_fixture_initialize
#define account_video_session_initialize_audio dispatch_fixture_initialize_audio
#define account_video_session_control dispatch_fixture_control
#define account_video_session_service dispatch_fixture_service
#define account_video_session_acknowledge dispatch_fixture_acknowledge
#define account_video_session_status dispatch_fixture_status
#define account_video_session_release dispatch_fixture_release
#define mp4_decoder_acknowledge_frame dispatch_fixture_decoder_acknowledge
#define account_media_video_matches dispatch_fixture_matches
#define account_media_audio_matches dispatch_fixture_audio_matches
#include "../src/app/account_media_video.cpp"
#undef account_media_video_reserved
#undef account_media_worker_video_select
#undef account_media_worker_video_control
#undef account_media_worker_video_status
#undef account_media_worker_video_take
#undef account_media_worker_video_acknowledge
#undef account_media_video_dispatch
#undef account_media_video_destroy
#undef account_video_session_initialize
#undef account_video_session_initialize_audio
#undef account_video_session_control
#undef account_video_session_service
#undef account_video_session_acknowledge
#undef account_video_session_status
#undef account_video_session_release
#undef mp4_decoder_acknowledge_frame
#undef account_media_video_matches
#undef account_media_audio_matches

namespace maicord::media_video_dispatch_test {
struct Fixture {
    AccountMediaWorker* worker;
    HANDLE thread, service_entered, publish_allowed, acknowledge_entered, acknowledge_allowed,
        seek_observed;
    GpuVideoFrame gpu;
    volatile LONG services, acknowledgements, releases, seeks, errors, decoder_acks;
    bool dispatched, stopped, audio_only;
};
Fixture* current;
constexpr u64 kPlaybackEpoch = 7, kFrameSerial = 9;
constexpr AccountVideoIdentity kIdentity{10, 20, 40, 50, 3, 1, 0};
void error(Fixture& fixture, LONG bit) noexcept {
    InterlockedOr(&fixture.errors, bit);
}
DWORD WINAPI dispatch(void* opaque) noexcept {
    auto& fixture = *static_cast<Fixture*>(opaque);
    fixture.dispatched = dispatch_fixture_dispatch(&fixture.worker->lanes[0], &fixture.stopped);
    return 0;
}
bool begin(Fixture& fixture, MediaAttachmentFormat format = MediaAttachmentFormat::mp4) noexcept {
    clear_bytes(&fixture, sizeof(fixture));
    const bool audio_only = format != MediaAttachmentFormat::mp4;
    fixture.audio_only = audio_only;
    current = &fixture;
    fixture.worker = static_cast<AccountMediaWorker*>(
        VirtualAlloc(nullptr, sizeof(AccountMediaWorker), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!fixture.worker)
        return false;
    auto& worker = *fixture.worker;
    worker.stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    worker.output_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fixture.service_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    fixture.publish_allowed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    fixture.acknowledge_entered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    fixture.acknowledge_allowed = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    fixture.seek_observed = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    auto& lane = worker.lanes[0];
    lane.owner = &worker;
    lane.index = 0;
    lane.command_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!worker.stop_event || !worker.output_event || !lane.command_event || !fixture.service_entered ||
        !fixture.publish_allowed || !fixture.acknowledge_entered || !fixture.acknowledge_allowed ||
        !fixture.seek_observed)
        return false;
    lane.phase = static_cast<LONG>(AccountMediaWorkerPhase::idle);
    lane.media.mp4_generation = kIdentity.cache_generation;
    lane.media.mp4_content_identity = kIdentity.content_identity;
    lane.media.mp4_source_message_id = kIdentity.message_id;
    lane.media.mp4_source_message_revision = kIdentity.message_revision;
    if (audio_only) {
        lane.media.audio_source.cache_generation = kIdentity.cache_generation;
        lane.media.audio_source.content_identity = kIdentity.content_identity;
        lane.media.audio_source.source_message_id = kIdentity.message_id;
        lane.media.audio_source.source_message_revision = kIdentity.message_revision;
        auto& metadata = lane.media.audio_source.metadata;
        if (format == MediaAttachmentFormat::ogg_opus) {
            OggOpusDemuxer source{};
            source.ready = true;
            metadata = EncodedAudioMetadata(source);
        } else if (format == MediaAttachmentFormat::mp3) {
            Mp3Stream source{};
            source.ready = true;
            metadata = EncodedAudioMetadata(source);
        } else if (format == MediaAttachmentFormat::wav) {
            WavePcmSource source{};
            source.ready = true;
            metadata = EncodedAudioMetadata(source);
        }
    }
    worker.initialized = true;
    const AccountVideoSelection selection{
        media_asset_attachment(30, format,
                               audio_only ? MediaResolutionTier::original : MediaResolutionTier::px512),
        kIdentity};
    if (!dispatch_fixture_select(&worker, selection).ok())
        return false;
    ResetEvent(worker.output_event);
    fixture.thread = CreateThread(nullptr, 0, dispatch, &fixture, 0, nullptr);
    return fixture.thread && WaitForSingleObject(fixture.service_entered, 2000) == WAIT_OBJECT_0;
}
bool command(Fixture& fixture, AccountVideoCommand kind, u64 serial) noexcept {
    const AccountVideoControl request{kIdentity, serial, 10000, 1, kind, false};
    return dispatch_fixture_submit(fixture.worker, request).ok();
}
bool publish(Fixture& fixture) noexcept {
    return SetEvent(fixture.publish_allowed) &&
           WaitForSingleObject(fixture.worker->output_event, 2000) == WAIT_OBJECT_0;
}
bool joined(Fixture& fixture) noexcept {
    return fixture.thread && WaitForSingleObject(fixture.thread, 2000) == WAIT_OBJECT_0;
}
void finish(Fixture& fixture, void (*check)(bool, const char*) noexcept) noexcept {
    if (fixture.thread && WaitForSingleObject(fixture.thread, 0) != WAIT_OBJECT_0) {
        SetEvent(fixture.publish_allowed);
        SetEvent(fixture.acknowledge_allowed);
        auto* runtime = fixture.worker->video_runtime;
        if (runtime) {
            AcquireSRWLockShared(&runtime->lock);
            const auto frame = runtime->frame;
            const bool taken = runtime->frame_taken && frame.gpu;
            ReleaseSRWLockShared(&runtime->lock);
            if (taken)
                dispatch_fixture_return(fixture.worker, frame);
        }
        SetEvent(fixture.worker->stop_event);
        SetEvent(fixture.worker->lanes[0].command_event);
        if (!joined(fixture)) {
            check(false, "video dispatch: bounded cleanup joins the borrowed fixture owner");
            // The caller exits this test process; retain every borrowed owner
            // rather than freeing a live producer after a failed join.
            ExitProcess(1);
        }
    }
    if (fixture.worker) {
        if (!fixture.thread && fixture.worker->video_runtime)
            InterlockedExchange(&fixture.worker->video_runtime->selected_lane, -1);
        check(dispatch_fixture_destroy(fixture.worker),
              "video dispatch: joined native owner releases its bounded runtime");
        if (fixture.worker->lanes[0].command_event)
            CloseHandle(fixture.worker->lanes[0].command_event);
        if (fixture.worker->stop_event)
            CloseHandle(fixture.worker->stop_event);
        if (fixture.worker->output_event)
            CloseHandle(fixture.worker->output_event);
        VirtualFree(fixture.worker, 0, MEM_RELEASE);
    }
    const HANDLE handles[]{fixture.thread,
                           fixture.service_entered,
                           fixture.publish_allowed,
                           fixture.acknowledge_entered,
                           fixture.acknowledge_allowed,
                           fixture.seek_observed};
    for (HANDLE handle : handles)
        if (handle)
            CloseHandle(handle);
    current = nullptr;
}
} // namespace maicord::media_video_dispatch_test

namespace maicord {
AccountVideoResult dispatch_fixture_initialize(AccountVideoSession* session, Mp4Decoder* decoder,
                                               const AccountVideoIdentity& identity, HANDLE event,
                                               Mp4SourceAvailability) noexcept {
    session->video = decoder;
    session->lane_event = event;
    session->status.identity = identity;
    session->initialized = true;
    return {};
}
AccountVideoResult dispatch_fixture_control(AccountVideoSession* session,
                                            const AccountVideoIdentity& identity, u64 serial,
                                            AccountVideoCommand kind, i64, float, bool) noexcept {
    auto& fixture = *media_video_dispatch_test::current;
    if (!account_video_identity_equal(identity, session->status.identity) || session->published)
        media_video_dispatch_test::error(fixture, 1);
    session->status.desired_command = session->status.observed_command = serial;
    if (kind == AccountVideoCommand::seek) {
        InterlockedIncrement(&fixture.seeks);
        SetEvent(fixture.seek_observed);
    }
    return {};
}
AccountVideoResult dispatch_fixture_initialize_audio(AccountVideoSession* session,
                                                     const EncodedAudioMetadata* source,
                                                     const AccountVideoIdentity& identity,
                                                     HANDLE event) noexcept {
    const auto& expected = media_video_dispatch_test::current->worker->lanes[0].media.audio_source.metadata;
    if (source != &expected || !encoded_audio_ready(*source))
        return {AccountVideoError::invalid_input};
    session->encoded_metadata = source;
    session->lane_event = event;
    session->status.identity = identity;
    session->status.kind = source->format == MediaAttachmentFormat::mp3   ? AccountPlaybackKind::mp3
                           : source->format == MediaAttachmentFormat::wav ? AccountPlaybackKind::wave_pcm
                                                                          : AccountPlaybackKind::ogg_opus;
    session->status.has_audio = true;
    session->status.duration_100ns = 10000000;
    session->initialized = true;
    return {};
}
AccountVideoService dispatch_fixture_service(AccountVideoSession* session, bool (*)(void*) noexcept,
                                             void*) noexcept {
    auto& fixture = *media_video_dispatch_test::current;
    AccountVideoService output{};
    if (InterlockedIncrement(&fixture.services) != 1)
        return output;
    SetEvent(fixture.service_entered);
    if (WaitForSingleObject(fixture.publish_allowed, 2000) != WAIT_OBJECT_0) {
        media_video_dispatch_test::error(fixture, 2);
        return output;
    }
    if (fixture.audio_only)
        return output;
    session->published = true;
    session->status.frame_pending = true;
    output.frame.gpu = &fixture.gpu;
    output.playback_epoch = media_video_dispatch_test::kPlaybackEpoch;
    output.frame_serial = media_video_dispatch_test::kFrameSerial;
    output.frame_ready = true;
    return output;
}
bool dispatch_fixture_acknowledge(AccountVideoSession* session, const AccountVideoIdentity& identity,
                                  u64 epoch, u64 serial) noexcept {
    auto& fixture = *media_video_dispatch_test::current;
    if (!account_video_identity_equal(identity, session->status.identity) || !session->published ||
        epoch != media_video_dispatch_test::kPlaybackEpoch ||
        serial != media_video_dispatch_test::kFrameSerial) {
        media_video_dispatch_test::error(fixture, 4);
        return false;
    }
    SetEvent(fixture.acknowledge_entered);
    AccountVideoFrame raced{};
    if (dispatch_fixture_take(fixture.worker, &raced)) {
        media_video_dispatch_test::error(fixture, 32);
        return false;
    }
    if (WaitForSingleObject(fixture.acknowledge_allowed, 2000) != WAIT_OBJECT_0) {
        media_video_dispatch_test::error(fixture, 8);
        return false;
    }
    InterlockedIncrement(&fixture.acknowledgements);
    session->published = false;
    session->status.frame_pending = false;
    return true;
}
AccountVideoStatus dispatch_fixture_status(const AccountVideoSession* session) noexcept {
    return session->status;
}
AccountVideoResult dispatch_fixture_release(AccountVideoSession* session, u32) noexcept {
    auto& fixture = *media_video_dispatch_test::current;
    if (session->published) {
        media_video_dispatch_test::error(fixture, 16);
        return {AccountVideoError::release};
    }
    InterlockedIncrement(&fixture.releases);
    session->initialized = false;
    session->workspace = nullptr;
    return {};
}
bool dispatch_fixture_decoder_acknowledge(Mp4Decoder*) noexcept {
    InterlockedIncrement(&media_video_dispatch_test::current->decoder_acks);
    return true;
}
bool dispatch_fixture_matches(const AccountMedia*, MediaAssetKey key, u16) noexcept {
    return media_asset_attachment_format(key) == MediaAttachmentFormat::mp4;
}
bool dispatch_fixture_audio_matches(const AccountMedia* media, MediaAssetKey key, u16 generation) noexcept {
    return media && encoded_audio_ready(media->audio_source.metadata) &&
           generation == media->audio_source.cache_generation &&
           media_asset_attachment_format(key) == media->audio_source.metadata.format;
}

void test_account_media_video_dispatch(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace media_video_dispatch_test;
    Fixture fixture{};
    bool ready = begin(fixture);
    check(ready, "video dispatch: real lane thread and deterministic late publication initialize");
    if (!ready) {
        finish(fixture, check);
        return;
    }
    ResetEvent(fixture.acknowledge_allowed);
    check(command(fixture, AccountVideoCommand::stop, 1),
          "video dispatch: user Stop is accepted during native service");
    SetEvent(fixture.publish_allowed);
    const bool disposing = WaitForSingleObject(fixture.acknowledge_entered, 2000) == WAIT_OBJECT_0;
    AccountVideoFrame output{};
    check(disposing && !dispatch_fixture_take(fixture.worker, &output) && !ReadAcquire(&fixture.releases) &&
              WaitForSingleObject(fixture.thread, 0) == WAIT_TIMEOUT,
          "video dispatch: accepted Stop atomically claims late unborrowed frame before native disposal");
    SetEvent(fixture.acknowledge_allowed);
    check(joined(fixture) && fixture.dispatched && !fixture.stopped &&
              ReadAcquire(&fixture.acknowledgements) == 1 && ReadAcquire(&fixture.releases) == 1 &&
              !ReadAcquire(&fixture.errors) && !dispatch_fixture_snapshot(fixture.worker).selected,
          "video dispatch: late unclaimed frame releases once and Stop retires without a UI drain");
    finish(fixture, check);

    ready = begin(fixture);
    check(ready, "video dispatch: taken-frame Stop fixture initializes");
    if (!ready) {
        finish(fixture, check);
        return;
    }
    const bool borrowed = publish(fixture) && dispatch_fixture_take(fixture.worker, &output);
    check(borrowed && command(fixture, AccountVideoCommand::stop, 1) &&
              WaitForSingleObject(fixture.thread, 50) == WAIT_TIMEOUT &&
              !ReadAcquire(&fixture.acknowledgements) && !ReadAcquire(&fixture.releases),
          "video dispatch: Stop cannot discard or release an already borrowed GPU frame");
    auto wrong = output;
    ++wrong.frame_serial;
    check(!dispatch_fixture_return(fixture.worker, wrong) &&
              WaitForSingleObject(fixture.thread, 50) == WAIT_TIMEOUT &&
              !ReadAcquire(&fixture.acknowledgements),
          "video dispatch: inexact borrowed frame ACK cannot unblock Stop");
    check(dispatch_fixture_return(fixture.worker, output) && joined(fixture) &&
              ReadAcquire(&fixture.acknowledgements) == 1 && ReadAcquire(&fixture.releases) == 1 &&
              !ReadAcquire(&fixture.errors),
          "video dispatch: exact borrowed frame ACK alone releases the stopped decoder");
    finish(fixture, check);

    ready = begin(fixture);
    check(ready, "video dispatch: unclaimed Seek fixture initializes");
    if (!ready) {
        finish(fixture, check);
        return;
    }
    check(publish(fixture) && command(fixture, AccountVideoCommand::seek, 1) &&
              WaitForSingleObject(fixture.seek_observed, 50) == WAIT_TIMEOUT &&
              !ReadAcquire(&fixture.acknowledgements) && !ReadAcquire(&fixture.releases) &&
              dispatch_fixture_take(fixture.worker, &output),
          "video dispatch: Seek preserves its unclaimed GPU frame for explicit client copy or discard");
    wrong = output;
    ++wrong.playback_epoch;
    check(!dispatch_fixture_return(fixture.worker, wrong) &&
              dispatch_fixture_return(fixture.worker, output) &&
              WaitForSingleObject(fixture.seek_observed, 2000) == WAIT_OBJECT_0 &&
              ReadAcquire(&fixture.seeks) == 1 && ReadAcquire(&fixture.acknowledgements) == 1 &&
              !ReadAcquire(&fixture.releases),
          "video dispatch: exact frame ACK precedes native Seek without changing Stop disposal policy");
    check(command(fixture, AccountVideoCommand::stop, 2) && joined(fixture) &&
              ReadAcquire(&fixture.releases) == 1 && !ReadAcquire(&fixture.errors),
          "video dispatch: acknowledged Seek owner then retires through ordinary Stop");
    finish(fixture, check);
    constexpr MediaAttachmentFormat formats[]{MediaAttachmentFormat::ogg_opus, MediaAttachmentFormat::mp3,
                                              MediaAttachmentFormat::wav};
    for (const auto format : formats) {
        ready = begin(fixture, format);
        check(ready, "audio dispatch: each retained tagged source selects the same existing codec lane");
        if (!ready) {
            finish(fixture, check);
            return;
        }
        auto stale = AccountVideoControl{kIdentity, 1, 10000, 1, AccountVideoCommand::pause, false};
        ++stale.identity.account_serial;
        check(dispatch_fixture_submit(fixture.worker, stale).error == AccountVideoError::stale &&
                  dispatch_fixture_snapshot(fixture.worker).selected,
              "audio dispatch: stale actor cannot control retained selected audio");
        check(command(fixture, AccountVideoCommand::seek, 1) && publish(fixture) &&
                  WaitForSingleObject(fixture.seek_observed, 2000) == WAIT_OBJECT_0 &&
                  dispatch_fixture_snapshot(fixture.worker).playback.kind ==
                      (format == MediaAttachmentFormat::mp3   ? AccountPlaybackKind::mp3
                       : format == MediaAttachmentFormat::wav ? AccountPlaybackKind::wave_pcm
                                                              : AccountPlaybackKind::ogg_opus) &&
                  fixture.worker->video_runtime->session.encoded_metadata ==
                      &fixture.worker->lanes[0].media.audio_source.metadata &&
                  !dispatch_fixture_take(fixture.worker, &output) && !ReadAcquire(&fixture.decoder_acks) &&
                  !ReadAcquire(&fixture.acknowledgements) &&
                  fixture.worker->video_runtime->session.video == nullptr,
              "audio dispatch: seek needs no H264 initialization or nonexistent GPU frame acknowledgment");
        check(command(fixture, AccountVideoCommand::stop, 2) && joined(fixture) &&
                  ReadAcquire(&fixture.releases) == 1 && !ReadAcquire(&fixture.acknowledgements) &&
                  !ReadAcquire(&fixture.errors) && !dispatch_fixture_snapshot(fixture.worker).selected,
              "audio dispatch: shared Stop drains audio-only owner before retained source release");
        finish(fixture, check);
    }
}
} // namespace maicord
