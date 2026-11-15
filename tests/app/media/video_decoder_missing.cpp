// A video that this computer has no decoder for (kVideoDecoderMissing: no GPU profile, and on Linux no libopenh264 for
// H.264) against a stream that a decoder refuses or that is damaged:
//  - the request result: a missing decoder is a permanent "unsupported" decode failure that names the codec, a refused
//    stream stays "failed" with its 60 s retry, a damaged stream keeps its parse error;
//  - an AV1 movie: the client has no AV1 movie decoder on either system, so it keeps the demuxer's refusal and no
//    install text applies;
//  - the lane's failure cache keeps a missing decoder for the session and reports it again while it waits, and a
//    refused stream is tried again after 60 s;
//  - the worker's retained failure reports the missing decoder again.
// The decoders' own status is maicord_mp4_decoder_missing (Linux).
#include "../../support/test_output.hpp"
#include "maicord/app/media/account_media.hpp"
#include "maicord/app/media/account_media_worker.hpp"
#include "maicord/core/win32_error.hpp"
#include "../../../src/app/media/account_media_internal.hpp"
#include "../../../src/app/media/account_media_worker_internal.hpp"
#if !defined(_WIN32)
#include "maicord/ui/chat/messages/media_policy.hpp"
#endif

namespace maicord {
namespace video_decoder_missing_test {

u32 failures;
void check(bool valid, const char* label) noexcept {
    test_check(&failures, valid, label);
}

AccountMediaRequestResult failure_of(i32 status, bool hevc, H264Error parse = H264Error::none,
                                     Mp4DemuxError demux = Mp4DemuxError::none) noexcept {
    Mp4Decoder decoder{};
    decoder.hevc = hevc;
    decoder.native.status = status;
    decoder.native.parse_error = parse;
    decoder.native.demux_error = demux;
    return account_media_detail::video_failure(&decoder);
}

void results() noexcept {
    const auto h264 = failure_of(kVideoDecoderMissing, false);
    check(h264.state == AccountMediaRequestState::unsupported && h264.error == AccountMediaError::decode &&
              h264.video_failure == AccountMediaVideoFailure::missing_h264_decoder && h264.retry_at_ms == ~0ull,
          "result: a missing H.264 decoder is permanent and names H.264");
    const auto hevc = failure_of(kVideoDecoderMissing, true);
    check(hevc.state == AccountMediaRequestState::unsupported &&
              hevc.video_failure == AccountMediaVideoFailure::missing_hevc_decoder && hevc.retry_at_ms == ~0ull,
          "result: a missing HEVC decoder is permanent and names HEVC");
    const auto refused = failure_of(DXGI_ERROR_UNSUPPORTED, false);
    check(refused.state == AccountMediaRequestState::failed &&
              refused.video_failure == AccountMediaVideoFailure::unsupported_backend && refused.retry_at_ms == 0,
          "result: a decoder that refuses the stream keeps the retry");
    const auto damaged = failure_of(E_INVALIDARG, false, H264Error::truncated);
    check(damaged.state == AccountMediaRequestState::unsupported &&
              damaged.video_failure == AccountMediaVideoFailure::truncated_stream,
          "result: a truncated stream keeps its parse error");
    const auto av1 = failure_of(E_INVALIDARG, false, H264Error::malformed, Mp4DemuxError::unsupported);
    check(av1.state == AccountMediaRequestState::unsupported &&
              av1.video_failure == AccountMediaVideoFailure::demux_unsupported && av1.retry_at_ms == ~0ull,
          "result: an AV1 movie keeps the demuxer's refusal, not a missing decoder");
}

void cache() noexcept {
    auto* media = commit_pages<AccountMedia>();
    if (!media) {
        check(false, "cache: media state allocates");
        return;
    }
    const MediaAssetKey gifv = media_asset_embed(80, 12, 0, MediaEmbedResource::video, MediaAttachmentFormat::mp4,
                                                 true, MediaResolutionTier::px512);
    const MediaAssetKey refused = media_asset_attachment(70, MediaAttachmentFormat::mp4, MediaResolutionTier::px512);
    constexpr u64 now = 1'000, hour = 3'600'000;
    const auto remembered =
        account_media_detail::remember_failure(media, gifv, now, failure_of(kVideoDecoderMissing, false));
    AccountMediaRequestResult waiting{}, later{};
    const bool cooling = account_media_detail::failure_cooling(media, gifv, now + kAccountMediaFailureRetryMs + 1,
                                                               &waiting) &&
                         account_media_detail::failure_cooling(media, gifv, now + hour, &later);
    check(remembered.retry_at_ms == ~0ull && cooling && waiting.state == AccountMediaRequestState::retry_wait &&
              waiting.video_failure == AccountMediaVideoFailure::missing_h264_decoder &&
              later.video_failure == AccountMediaVideoFailure::missing_h264_decoder,
          "cache: a missing decoder is not tried again and is reported while it waits");
    account_media_detail::remember_failure(media, refused, now, failure_of(DXGI_ERROR_UNSUPPORTED, false));
    AccountMediaRequestResult before{}, after{};
    check(account_media_detail::failure_cooling(media, refused, now + kAccountMediaFailureRetryMs - 1, &before) &&
              before.video_failure == AccountMediaVideoFailure::unsupported_backend &&
              !account_media_detail::failure_cooling(media, refused, now + kAccountMediaFailureRetryMs, &after),
          "cache: a refused stream is tried again after 60 s");
    pages_release(media, sizeof(AccountMedia));
}

void worker() noexcept {
    const AccountMediaWorkerRetry retry{media_asset_attachment(71, MediaAttachmentFormat::mp4,
                                                               MediaResolutionTier::px512),
                                        ~0ull, AccountMediaVideoFailure::missing_hevc_decoder};
    const auto waiting = account_media_worker_detail::retry_wait_for(retry);
    check(waiting.state == AccountMediaRequestState::retry_wait && waiting.retry_at_ms == ~0ull &&
              waiting.video_failure == AccountMediaVideoFailure::missing_hevc_decoder,
          "worker: a retained missing decoder is reported again");
}

#if !defined(_WIN32)
// The Linux labels (the Windows ones: maicord_core's test_unplayable_video): H.264 names the library to install.
void labels() noexcept {
    check(utf8_equal(message_media_unplayable_label(MessageMediaUnplayable::h264),
                     utf8_literal("Install OpenH264 to play this video")) &&
              utf8_equal(message_media_unplayable_label(MessageMediaUnplayable::hevc),
                         utf8_literal("No decoder for this video format")),
          "label: H.264 names OpenH264, HEVC has no library to name");
}
#endif

} // namespace video_decoder_missing_test
} // namespace maicord

MAICORD_TEST_ENTRY(maicord_video_decoder_missing_entry) {
    using namespace maicord::video_decoder_missing_test;
    results();
    cache();
    worker();
#if !defined(_WIN32)
    labels();
#endif
    maicord::test_exit(failures, "maicord_video_decoder_missing");
}
