#include "account_media_internal.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/render/effects/lottie_program.hpp"
#include "maicord/media/containers/hevc_mp4_demux.hpp"
#include "maicord/media/image/regional_flag_svg.hpp"

namespace maicord {
namespace account_media_detail {

// Drives the head probe, moov probe and first frame. True while a range is in
// flight; false once the poster is published or a failure was recorded.
bool continue_progressive(AccountMedia* media, u64 now_ms, u64 unix_now_seconds) noexcept {
    auto* fetch = media->progressive;
    const auto failure = [&](AccountMediaRequestResult value) noexcept {
        remember_failure(media, media->active_key, now_ms, value);
        return false;
    };
    for (u32 step = 0; step < 4; ++step) {
        ProgressiveVideoFetch* selected{};
        const auto next = account_media_progressive_next(media, false, unix_now_seconds, &selected);
        if (next.step == ProgressiveVideoStep::submit)
            return submit_progressive(media, next.range, selected) ||
                   failure(result(AccountMediaRequestState::failed, AccountMediaError::client));
        if (next.step == ProgressiveVideoStep::refresh_required) {
            // Not a cooldown: the owner refreshes the signed URL and asks again.
            auto refresh = result(AccountMediaRequestState::refresh_required);
            refresh.retry_at_ms = deadline_after(now_ms, 1'000);
            media->last_failure = refresh;
            return false;
        }
        if (next.step == ProgressiveVideoStep::failed) {
            // Unsupported layouts (fragmented, oversized moov) and exhausted
            // refreshes alike retry after the normal cooldown.
            auto value = result(AccountMediaRequestState::failed, AccountMediaError::decode);
            value.video_failure = AccountMediaVideoFailure::demux_unsupported;
            return failure(value);
        }
        if (next.step == ProgressiveVideoStep::ready || fetch->state != ProgressiveVideoFetchState::ready)
            continue;
        if (fetch->kind == ProgressiveVideoKind::fragmented_audio) {
            Mp4AudioSampleWindow window{};
            if (!mp4_audio_sample_window(fetch->audio.timeline, 0, &window))
                return failure(result(AccountMediaRequestState::failed, AccountMediaError::decode));
            account_media_release_audio_source(media);
            auto& source = media->audio_source;
            source.key = media->active_key;
            source.content_identity = encoded_media_new_content_identity();
            if (!++media->audio_generation) ++media->audio_generation;
            source.cache_generation = media->audio_generation;
            source.source_message_id = media->active_source_message_id;
            source.source_message_revision = media->active_source_message_revision;
            source.duration_us = static_cast<u64>(window.duration_100ns / 10);
            publish_audio_frame(media, source.key, media->active_request_id);
            return false;
        }
        const auto bytes = progressive_source_bytes(fetch->source);
        const auto reader = progressive_source_reader(&fetch->source, ProgressiveTrack::video);
        if (!media->mp4_decoder || !media->mp4_decoder->ready) {
            if (fetch->kind != ProgressiveVideoKind::movie) {
                if (!mp4_decoder_initialize_fragmented(ensure_mp4_decoder(media), &fetch->fragment, media->video_device)) {
                    if (media->mp4_decoder->native.demux_error == Mp4DemuxError::data_pending) continue;
                    return failure(video_failure(media->mp4_decoder));
                }
                adopt_mp4_source(media);
            } else {
            // The decoder configuration reads the first sample; fetch it first.
            Mp4Demuxer first{};
            const auto error = hevc_container_present(bytes) ? hevc_demux_initialize(&first, bytes, reader)
                                                             : mp4_demux_initialize(&first, bytes, reader);
            if (error != Mp4DemuxError::none) {
                auto value = video_failure(nullptr);
                value.state = AccountMediaRequestState::unsupported;
                value.video_failure = error == Mp4DemuxError::no_video ? AccountMediaVideoFailure::demux_no_video
                                      : error == Mp4DemuxError::unsupported
                                          ? AccountMediaVideoFailure::demux_unsupported
                                          : AccountMediaVideoFailure::demux_malformed;
                value.retry_at_ms = ~0ull;
                return failure(value);
            }
            progressive_source_set_duration(&fetch->source, first.duration_us);
            if (mp4_demux_next_ready(&first).error == Mp4DemuxError::data_pending)
                continue;
            if (!mp4_decoder_initialize(ensure_mp4_decoder(media), bytes, media->active_proxy_width,
                                        media->active_proxy_height, media->video_device, reader))
                return failure(video_failure(media->mp4_decoder));
            adopt_mp4_source(media);
            }
        }
        const Mp4DecodedFrame decoded = mp4_decoder_read(media->mp4_decoder);
        if (decoded.error == Mp4DecodeError::needs_service) { media->progressive_continue = true; return true; }
        if (decoded.error == Mp4DecodeError::data_pending)
            continue;
        if (!decoded.ok())
            return failure(video_failure(media->mp4_decoder));
        account_media_cache_progressive(media, media->active_key);
        publish_mp4_frame(media, media->active_key, decoded, media->active_request_id,
                          media->active_source_message_id, media->active_source_message_revision, 0);
        return false;
    }
    return failure(result(AccountMediaRequestState::failed, AccountMediaError::decode));
}

} // namespace account_media_detail
namespace {

// The failure of a response whose body went through a codec's admission.
// A missing codec workspace outranks a decode error, which outranks the HTTP status.
AccountMediaRequestResult admit_failure(const CdnResponseResult& policy, bool workspace_failed = false) noexcept {
    auto failure = result(AccountMediaRequestState::failed,
                          workspace_failed ? AccountMediaError::memory
                          : policy.ok()    ? AccountMediaError::decode
                                           : AccountMediaError::http);
    failure.http_error = policy.error;
    return failure;
}

} // namespace
namespace account_media_detail {

// Lottie stickers and regional flags publish one worker-compiled paint
// program from a single lazily reserved workspace; the encoded bytes stay
// with the caller (response body or cache lease).
bool publish_vector(AccountMedia* media, const MediaAssetKey& key_view, const ConstSlice<u8>& encoded_view, u64 request_id,
                    u32 frame_index) noexcept {
    const MediaAssetKey key = key_view; // the media state is rewritten below
    const ConstSlice<u8> encoded = encoded_view;
    const bool lottie = lottie_asset(key);
    if (!supported_key(key) || (!lottie && key.kind != MediaAssetKind::regional_flag) || !encoded.data ||
        encoded.count > (lottie ? kLottieMaximumBytes : kRegionalFlagSvgMaximumBytes))
        return false;
    // Both vector kinds compile here into the shared paint workspace; the
    // client only rasterizes the borrowed program.
    const auto workspace = +[](void* context) noexcept -> ColrPaintProgram* {
        auto* media = static_cast<AccountMedia*>(context);
        if (!media->lottie_workspace)
            media->lottie_workspace =
                commit_pages(sizeof(ColrPaintProgram));
        return static_cast<ColrPaintProgram*>(media->lottie_workspace);
    };
    LottieTiming timing{};
    const u32 pixels = lottie ? lottie_raster_pixels(key) : 0;
    if (lottie) {
        if (!lottie_program_compile_frame(encoded, frame_index, static_cast<float>(pixels), &timing, workspace, media))
            return false;
    } else {
        RegionalFlagSvg image{};
        ColrPaintProgram* program = nullptr;
        if (!regional_flag_svg_parse(encoded, &image) || !(program = workspace(media)) ||
            !regional_flag_svg_compile(image, program, kRegionalFlagPixelsPerUnit))
            return false;
    }
    stage_frame(media, key, static_cast<u32>(sizeof(ColrPaintProgram)), request_id, 0, 0);
    media->frame_duration_us = timing.duration_us;
    media->frame_index = frame_index;
    media->frame_count = lottie ? timing.frames : 0;
    media->frame_play_count = 0;
    media->frame_cache_generation = 0;
    media->frame_canvas_width = static_cast<u16>(pixels);
    media->frame_canvas_height = static_cast<u16>(pixels);
    reset_value(&media->frame_webp_image);
    media->frame_webp_image.width = media->frame_webp_image.height = lottie ? pixels : 64;
    media->frame_kind =
        lottie ? AccountMediaFrameKind::lottie_paint : AccountMediaFrameKind::regional_flag_paint;
    mark_frame_ready(media, key);
    return true;
}

// Separate frame so the inflate scratch only occupies stack on this path.
__declspec(noinline) bool publish_bundled_flag(AccountMedia* media, MediaAssetKey key) noexcept {
    u8 scratch[kRegionalFlagSvgMaximumBytes];
    return publish_vector(media, key,
                          regional_flag_bundled_svg(static_cast<u16>(key.id), {scratch, sizeof(scratch)}), 0);
}

bool consume_response(void* opaque, const CdnClientResponse& response,
                      ConstSlice<u8> body) noexcept {
    auto* context = static_cast<ConsumeContext*>(opaque);
    AccountMedia* media = context->media;
    if (!media->active || response.request_id != media->active_request_id) {
        return true;
    }
    const auto fail = [&](const AccountMediaRequestResult& failure) noexcept {
        remember_failure(media, media->active_key, context->now_ms, failure);
        return true;
    };
    if (media->progressive_active) {
        const auto policy = cdn_client_response_policy(response);
        const auto accepted = account_media_accept_progressive(media, response, body);
        if (accepted == ProgressiveVideoAccept::progress && policy.range_first == 0 &&
            policy.disposition == CdnResponseDisposition::accept_partial) {
            const auto body_failure = account_media_video_body_failure(body);
            if (body_failure != AccountMediaVideoFailure::none) {
                auto failure = result(AccountMediaRequestState::unsupported, AccountMediaError::decode);
                failure.video_failure = body_failure;
                return fail(failure);
            }
        }
        if (accepted == ProgressiveVideoAccept::progress || accepted == ProgressiveVideoAccept::ignored ||
            accepted == ProgressiveVideoAccept::refresh_required) {
            media->progressive_continue = true;
            return true;
        }
        if (accepted == ProgressiveVideoAccept::failed) {
            auto failure = result(AccountMediaRequestState::failed,
                                  response.transport_ok() ? AccountMediaError::http : AccountMediaError::client);
            failure.client_error = response.error;
            failure.http_error = response.http_error;
            return fail(failure);
        }
        // The server ignored Range and sent the whole bounded entity.
    }
    if (!response.transport_ok()) {
        AccountMediaRequestResult failure =
            result(AccountMediaRequestState::failed, AccountMediaError::client);
        failure.client_error = response.error;
        failure.http_error = response.http_error;
        return fail(failure);
    }
    const CdnResponseResult policy = cdn_client_response_policy(response);
    const ConstSlice<u8> conditional_etag{response.conditional_etag, response.conditional_etag_bytes};
    if (lottie_asset(media->active_key)) {
        bool published = false;
        if (policy.status_code == 200 && policy.body_bytes == body.count &&
            body.count <= cdn_encoded_body_limit(media->active_image_type)) {
            published = publish_vector(media, media->active_key, body, response.request_id);
            if (published) {
                const auto stored = encoded_media_cache_store(
                    media->pipeline.cache, media->active_key, body,
                    {policy.etag, policy.expires_at_seconds, policy.store_allowed, policy.must_revalidate});
                if (stored.stored())
                    media->frame_cache_generation = stored.generation;
            }
        } else if (policy.status_code == 304 && !body.count && conditional_etag.count) {
            auto lease = encoded_media_cache_acquire(media->pipeline.cache, media->active_key, 0);
            const bool matches = lease.acquired && lease.lookup.etag.count == conditional_etag.count &&
                                 bytes_equal(lease.lookup.etag.data, conditional_etag.data, conditional_etag.count);
            if (matches)
                published =
                    publish_vector(media, media->active_key, lease.lookup.bytes, response.request_id);
            if (published && policy.store_allowed)
                media->frame_cache_generation = lease.lookup.generation;
            if (lease.acquired)
                encoded_media_cache_release(media->pipeline.cache, &lease);
            if (published) {
                if (policy.store_allowed)
                    encoded_media_cache_refresh(media->pipeline.cache, media->active_key,
                                                policy.expires_at_seconds, policy.must_revalidate,
                                                policy.etag);
                else
                    encoded_media_cache_erase(media->pipeline.cache, media->active_key);
            }
        }
        if (!published)
            fail(result(AccountMediaRequestState::failed, AccountMediaError::decode));
    } else if (audio_image_type(media->active_image_type)) {
        CdnEncodedAudioResult decoded;
        cdn_media_pipeline_admit_audio(&decoded, &media->pipeline, media->active_key, policy, body,
                                       conditional_etag, media->audio_cancelled,
                                       media->audio_cancel_context);
        if (!decoded.ready() ||
            !publish_audio_source(media, media->active_key, &decoded, response.request_id)) {
            auto failure =
                result(AccountMediaRequestState::failed, !policy.ok()      ? AccountMediaError::http
                                                         : decoded.ready() ? AccountMediaError::memory
                                                                           : AccountMediaError::decode);
            failure.http_error = policy.error;
            fail(failure);
        }
    } else if (video_asset(media->active_key) && media->active_image_type == CdnImageType::mp4) {
        const bool catalog = media->active_key.kind == MediaAssetKind::catalog_preview;
        const auto body_failure = account_media_video_body_failure(body);
        // A picker tile decodes at most an image-sized source.
        if (policy.ok() &&
            (body_failure != AccountMediaVideoFailure::none ||
             (catalog && body.count > kAccountMediaCatalogVideoMaximumBytes))) {
            auto failure = result(AccountMediaRequestState::unsupported, AccountMediaError::decode);
            failure.video_failure = body_failure;
            return fail(failure);
        }
        account_media_release_video(media);
        if (!policy.ok() ||
            !mp4_decoder_initialize(ensure_mp4_decoder(media), body, media->active_proxy_width,
                                    media->active_proxy_height, media->video_device)) {
            AccountMediaRequestResult failure =
                policy.ok() ? video_failure(media->mp4_decoder)
                            : result(AccountMediaRequestState::failed, AccountMediaError::http);
            failure.http_error = policy.error;
            return fail(failure);
        }
        adopt_mp4_source(media);
        const Mp4DecodedFrame decoded = mp4_decoder_read(media->mp4_decoder);
        if (!decoded.ok())
            return fail(video_failure(media->mp4_decoder));
        // Small picker tile sources stay in the shared encoded cache so a
        // reopened picker restarts them without another download.
        if (catalog && policy.store_allowed && body.count <= kAccountMediaProgressiveCacheMaximumBytes)
            encoded_media_cache_store(
                media->pipeline.cache, media->active_key, body,
                {policy.etag, policy.expires_at_seconds, policy.store_allowed, policy.must_revalidate});
        publish_mp4_frame(media, media->active_key, decoded, response.request_id,
                          media->active_source_message_id, media->active_source_message_revision, 0);
    } else if ((media->active_key.kind == MediaAssetKind::attachment ||
                (media->active_key.kind == MediaAssetKind::embed ||
                 media->active_key.kind == MediaAssetKind::profile_image)) &&
               media->active_image_type == CdnImageType::jpeg) {
        CdnStaticJpegResult decoded;
        cdn_media_pipeline_admit_static_jpeg(&decoded, &media->pipeline, media->active_key, policy, body,
                                             conditional_etag);
        if (decoded.state == CdnStaticJpegState::jpeg_workspace_required && ensure_jpeg_workspace(media)) {
            cdn_media_pipeline_admit_static_jpeg(&decoded, &media->pipeline, media->active_key, policy,
                                                 body, conditional_etag);
        }
        if (!decoded.decoded()) {
            auto failure =
                admit_failure(policy, decoded.state == CdnStaticJpegState::jpeg_workspace_required);
            failure.jpeg_error = decoded.decode.error;
            return fail(failure);
        }
        publish_jpeg_frame(media, media->active_key, decoded, response.request_id,
                           media->active_source_message_id, media->active_source_message_revision);
    } else if (webp_asset(media->active_key) && animated_asset(media->active_key)) {
        CdnAnimatedWebpResult decoded;
        cdn_media_pipeline_admit_animated_webp(&decoded, &media->pipeline, media->active_key, policy, body,
                                               conditional_etag);
        if (decoded.state == CdnAnimatedWebpState::lossy_workspace_required &&
            ensure_lossy_webp_workspace(media)) {
            cdn_media_pipeline_admit_animated_webp(&decoded, &media->pipeline, media->active_key, policy,
                                                   body, conditional_etag);
        }
        if (!decoded.decoded()) {
            auto failure =
                admit_failure(policy, decoded.state == CdnAnimatedWebpState::lossy_workspace_required);
            failure.webp_error = decoded.decode.frame_decode.error;
            failure.animated_webp_error = decoded.decode.error;
            failure.lossy_webp_error = decoded.decode.lossy_decode.error;
            return fail(failure);
        }
        publish_animated_webp_frame(media, media->active_key, decoded, response.request_id);
    } else if (webp_asset(media->active_key) ||
               ((media->active_key.kind == MediaAssetKind::attachment ||
                 (media->active_key.kind == MediaAssetKind::embed ||
                  media->active_key.kind == MediaAssetKind::profile_image)) &&
                media->active_image_type == CdnImageType::webp)) {
        CdnStaticWebpResult decoded;
        cdn_media_pipeline_admit_static_webp(&decoded, &media->pipeline, media->active_key, policy, body,
                                             conditional_etag);
        if (decoded.state == CdnStaticWebpState::lossy_workspace_required &&
            ensure_lossy_webp_workspace(media)) {
            cdn_media_pipeline_admit_static_webp(&decoded, &media->pipeline, media->active_key, policy,
                                                 body, conditional_etag);
        }
        if (!decoded.decoded()) {
            auto failure =
                admit_failure(policy, decoded.state == CdnStaticWebpState::lossy_workspace_required);
            failure.webp_error = decoded.decode.error;
            failure.lossy_webp_error = decoded.lossy_decode.error;
            return fail(failure);
        }
        publish_webp_frame(media, media->active_key, decoded, response.request_id,
                           media->active_source_message_id, media->active_source_message_revision);
    } else if (apng_asset(media->active_key) ||
               (catalog_png_key(media->active_key) &&
                (png_animation_body(body) ||
                 (response.status_code == 304 &&
                  catalog_png_cached_animation(media, media->active_key, 0))))) {
        if (!ensure_apng_workspace(media))
            return fail(result(AccountMediaRequestState::failed, AccountMediaError::memory));
        CdnApngResult decoded;
        cdn_media_pipeline_admit_apng(&decoded, &media->pipeline, media->active_key, policy, body,
                                      conditional_etag);
        if (!decoded.decoded()) {
            auto failure = admit_failure(policy);
            copy_value(&failure.apng_error, decoded.decode.error);
            return fail(failure);
        }
        publish_apng_frame(media, media->active_key, decoded, response.request_id);
    } else if (gif_asset(media->active_key)) {
        CdnGifResult decoded;
        cdn_media_pipeline_admit_gif(&decoded, &media->pipeline, media->active_key, policy, body,
                                     conditional_etag);
        if (!decoded.decoded()) {
            auto failure = admit_failure(policy);
            failure.gif_error = decoded.decode.error;
            return fail(failure);
        }
        publish_gif_frame(media, media->active_key, decoded, response.request_id);
    } else {
        CdnStaticPngResult decoded;
        cdn_media_pipeline_admit_static_png(&decoded, &media->pipeline, media->active_key, policy, body,
                                            conditional_etag);
        if (!decoded.decoded()) {
            auto failure = admit_failure(policy);
            failure.png_error = decoded.decode.error;
            return fail(failure);
        }
        publish_png_frame(media, media->active_key, decoded, response.request_id,
                          media->active_source_message_id, media->active_source_message_revision);
    }
    return true;
}

} // namespace account_media_detail
} // namespace maicord
