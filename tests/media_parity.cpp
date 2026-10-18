// Manual extension: MAICORD_MEDIA_PARITY_SAMPLES=<directory> adds local sample files to the run.
#include "maicord/app/media_url_resolver.hpp"
#include "maicord/app/rest_client.hpp"
#include "maicord/app/account_media.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/message_rich_layout.hpp"
#include "media_fixture.hpp"

#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
u8 encoded[4 * 1024 * 1024];
u8 scratch[4 * 1024 * 1024];
MediaUrlResolver resolver;

void exercise_animation(Check check, MediaAssetKey key, u32 bytes, bool gif,
                        u32 minimum_frames = 2) noexcept {
    AccountMedia media{};
    if (!account_media_initialize(&media).ok()) {
        check(false, "message_animation_initialize");
        return;
    }
    const auto stored =
        encoded_media_cache_store(&media.cache, key, {encoded, bytes}, {{}, 50'000, true, false});
    const auto url =
        key.kind == MediaAssetKind::attachment
            ? utf8_literal("https://cdn.discordapp.com/attachments/7/9876/test.gif")
            : utf8_literal(
                  "https://images-ext-1.discordapp.net/external/test/https/example.com/animation.webp");
    const AccountMediaAttachmentRequest request{
        url, key, 777, 2, 0, 0, gif ? CdnImageType::gif : CdnImageType::webp, {}, nullptr};
    const auto requested = account_media_request_attachment(&media, request, 100, 100);
    AccountMediaFrame first{};
    const bool taken = account_media_take_frame(&media, &first);
    check(stored.stored() && requested.state == AccountMediaRequestState::ready && taken &&
              first.frame_count >= minimum_frames && first.duration_us > 0 &&
              first.source_message_id == 777 && first.source_message_revision == 2 && !media.client_started,
          gif ? "message_gif_cache_first_frame_and_owner" : "message_webp_cache_first_frame_and_owner");
    bool frames = taken;
    for (u32 i = 1; frames && i < first.frame_count; ++i) {
        const auto advance = account_media_advance_animation(&media, key, stored.generation, i, 100);
        AccountMediaFrame next{};
        frames = advance.state == AccountMediaRequestState::ready &&
                 account_media_take_frame(&media, &next) && next.frame_index == i && next.duration_us > 0 &&
                 media_asset_key_equal(key, next.key);
    }
    check(frames, gif ? "message_gif_all_frames" : "message_webp_all_frames");
    check(account_media_stop(&media, 5000), "message_animation_stop");
}

void local_sample(Check check, const wchar_t* name, bool gif) noexcept {
    wchar_t path[1024]{};
    const DWORD count = GetEnvironmentVariableW(L"MAICORD_MEDIA_PARITY_SAMPLES", path, 900);
    if (count == 0 || count >= 900)
        return;
    u32 used = count;
    path[used++] = L'/';
    for (u32 i = 0; name[i] && used < 1023; ++i)
        path[used++] = name[i];
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    DWORD bytes = 0;
    LARGE_INTEGER size{};
    const bool loaded = file != INVALID_HANDLE_VALUE && GetFileSizeEx(file, &size) && size.QuadPart > 0 &&
                        size.QuadPart <= sizeof(encoded) &&
                        ReadFile(file, encoded, static_cast<DWORD>(size.QuadPart), &bytes, nullptr) &&
                        bytes == size.QuadPart;
    if (file != INVALID_HANDLE_VALUE)
        CloseHandle(file);
    check(loaded, "message_animation_local_sample_loaded");
    if (loaded)
        exercise_animation(check,
                           gif ? media_asset_attachment(9876, MediaAttachmentFormat::gif)
                               : media_asset_embed(777, 2, 0, MediaEmbedResource::image,
                                                   MediaAttachmentFormat::webp, true),
                           bytes, gif);
}

void network_animation_sample(Check check, bool gif) noexcept {
    char source[2048]{};
    const DWORD bytes = GetEnvironmentVariableA(gif ? "MAICORD_MEDIA_PARITY_NETWORK_GIF"
                                                    : "MAICORD_MEDIA_PARITY_NETWORK_WEBP",
                                                source, sizeof(source));
    if (bytes == 0 || bytes >= sizeof(source))
        return;
    AccountMedia media{};
    if (!account_media_initialize(&media).ok()) {
        check(false, "network_gif_initialize");
        return;
    }
    const auto key =
        media_asset_embed(777, 2, 0, MediaEmbedResource::image,
                          gif ? MediaAttachmentFormat::gif : MediaAttachmentFormat::webp, true);
    const AccountMediaAttachmentRequest request{{source, bytes},
                                                key,
                                                777,
                                                2,
                                                static_cast<u16>(gif ? 0 : 512),
                                                static_cast<u16>(gif ? 0 : 288),
                                                gif ? CdnImageType::gif : CdnImageType::webp,
                                                {},
                                                nullptr};
    auto requested = account_media_request_attachment(&media, request, clock_monotonic_milliseconds(),
                                                      clock_unix_milliseconds() / 1000);
    const u64 deadline = clock_monotonic_milliseconds() + 30'000;
    while (!media.frame_ready && media.active && clock_monotonic_milliseconds() < deadline) {
        WaitForSingleObject(account_media_notification_handle(&media), 1000);
        if (!account_media_pump(&media, clock_monotonic_milliseconds(), clock_unix_milliseconds() / 1000))
            break;
    }
    AccountMediaFrame first{};
    bool ok = requested.ok() && account_media_take_frame(&media, &first) && first.frame_count > 1 &&
              (gif ? first.kind == AccountMediaFrameKind::gif_indices
                   : first.kind == AccountMediaFrameKind::animated_webp_vp8 ||
                         first.kind == AccountMediaFrameKind::animated_webp_rgba8) &&
              first.source_message_id == 777;
    for (u32 i = 1; ok && i < first.frame_count; ++i) {
        const auto advanced = account_media_advance_animation(&media, key, first.cache_generation, i,
                                                              clock_unix_milliseconds() / 1000);
        AccountMediaFrame frame{};
        ok = advanced.ok() && account_media_take_frame(&media, &frame) && frame.frame_index == i;
    }
    check(ok, gif ? "network_gif_native_download_and_all_frames"
                  : "network_webp_animated_proxy_and_all_frames");
    check(account_media_stop(&media, 5000), "network_animation_stop");
}
} // namespace

void test_media_parity(Check check) noexcept {
    const auto audio = media_asset_attachment(9876, MediaAttachmentFormat::ogg_opus,
                                               MediaResolutionTier::px2048);
    const auto audio_search = media_asset_search_attachment(9876, MediaAttachmentFormat::ogg_opus,
        MediaResolutionTier::px2048, kMediaSearchRevisionMaximum, true);
    auto invalid_audio = audio;
    invalid_audio.variant |= static_cast<u32>(MediaResolutionTier::px256) << kMediaAttachmentResolutionShift;
    check(media_asset_key_valid(audio) && media_asset_key_valid(audio_search) &&
              media_asset_attachment_format(audio) == MediaAttachmentFormat::ogg_opus &&
              media_asset_resolution_tier(audio) == MediaResolutionTier::original &&
              media_asset_search_revision(audio_search) == kMediaSearchRevisionMaximum &&
              media_asset_search_snapshot(audio_search) && !media_asset_key_valid(invalid_audio) &&
              !media_asset_key_valid(media_asset_attachment(9876, MediaAttachmentFormat::ogg_opus,
                   MediaResolutionTier::original, true)) &&
              !media_asset_key_valid(media_asset_attachment(9876, MediaAttachmentFormat::lottie)),
          "ogg_audio_uses_last_format_bits_without_truncating_search_revision_or_admitting_image_animation");
    const auto search = media_asset_search_attachment(9876, MediaAttachmentFormat::gif,
        MediaResolutionTier::px512, kMediaSearchRevisionMaximum);
    const auto child = media_asset_search_attachment(9876, MediaAttachmentFormat::gif,
        MediaResolutionTier::px512, kMediaSearchRevisionMaximum, true);
    check(media_asset_key_valid(search) && media_asset_search(search) &&
              media_asset_search_revision(search) == kMediaSearchRevisionMaximum &&
              media_asset_resolution_tier(search) == MediaResolutionTier::px512 &&
              media_asset_attachment_format(search) == MediaAttachmentFormat::gif &&
              media_asset_search_snapshot(child) && !media_asset_search_snapshot(search) &&
              !media_asset_key_equal(child, search) &&
              !media_asset_key_equal(search, media_asset_attachment(9876, MediaAttachmentFormat::gif,
                  MediaResolutionTier::px512)), "search_media_exact_publication_and_snapshot_keys");
    const auto search_embed_key = media_asset_search_embed(777, 3, 9, MediaEmbedResource::thumbnail,
        MediaAttachmentFormat::png, false, MediaResolutionTier::px256, true);
    check(media_asset_key_valid(search_embed_key) && media_asset_search_snapshot(search_embed_key) &&
              media_asset_search_revision(search_embed_key) == 3 && media_asset_embed_index(search_embed_key) == 9 &&
              media_asset_embed_resource(search_embed_key) == MediaEmbedResource::thumbnail &&
              !media_asset_key_equal(search_embed_key, media_asset_embed(777, 3, 9, MediaEmbedResource::thumbnail,
                  MediaAttachmentFormat::png, false, MediaResolutionTier::px256, true)),
          "search_embed_preserves_resource_resolution_and_outer_namespace");
    auto malformed = media_asset_attachment(9876, MediaAttachmentFormat::png);
    malformed.flags |= kMediaEmbedSnapshotFlag;
    auto profile = search_embed_key;
    profile.kind = MediaAssetKind::profile_image;
    check(!media_asset_key_valid(malformed) && !media_asset_key_valid(profile) &&
              !media_asset_key_valid(media_asset_search_attachment(9876, MediaAttachmentFormat::png,
                  MediaResolutionTier::original, 0)) &&
              !media_asset_key_valid(media_asset_search_attachment(9876, MediaAttachmentFormat::png,
                  MediaResolutionTier::original, kMediaSearchRevisionMaximum + 1)) &&
              !media_asset_key_valid(media_asset_search_embed(777, kMediaSearchRevisionMaximum + 1, 0,
                  MediaEmbedResource::image, MediaAttachmentFormat::png)),
          "search_media_never_truncates_publication_or_relaxes_ordinary_scope_flags");
    const auto original = utf8_literal("https://cdn.discordapp.com/attachments/7/9876/test.gif");
    const auto fresh = utf8_literal("https://media.discordapp.net/attachments/7/9876/"
                                    "test.gif?backend=b2&ex=ffff&is=1&hm="
                                    "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    check(attachment_refresh_needed(original, 100) &&
              !attachment_refresh_needed(fresh, 100),
          "attachment_refresh_unsigned_and_signed_expiry");
    check(!attachment_refresh_needed(
              utf8_literal("https://example.com/attachments/7/9876/test.gif"), 100) &&
              !attachment_refresh_needed(
                  utf8_literal("https://images-ext-1.discordapp.net/external/abc/test.gif"), 100),
          "attachment_refresh_external_urls_excluded");
    u8 body[4096]{};
    const u32 request_bytes = attachment_refresh_body(original, {body, sizeof(body)});
    check(utf8_equal({reinterpret_cast<const char*>(body), request_bytes},
                     utf8_literal("{\"attachment_urls\":[\"https://cdn.discordapp.com/attachments/7/9876/"
                                  "test.gif\"]}")),
          "attachment_refresh_installed_client_contract");
    ProtocolWriter writer{{body, sizeof(body)}, 0};
    protocol_writer_append_literal(&writer, "{\"refreshed_urls\":[{\"original\":");
    protocol_writer_append_json_string(&writer, original);
    protocol_writer_append_literal(&writer, ",\"refreshed\":");
    protocol_writer_append_json_string(&writer, fresh);
    protocol_writer_append_literal(&writer, "}]}");
    const Utf8View response{reinterpret_cast<const char*>(body), writer.used};
    char refreshed[2048]{};
    check(attachment_refresh_response(response, original, 100, {refreshed, sizeof(refreshed)}) ==
                  fresh.bytes &&
              utf8_equal({refreshed, fresh.bytes}, fresh),
          "attachment_refresh_response_validated");
    check(attachment_refresh_response(
              response, utf8_literal("https://cdn.discordapp.com/attachments/7/9999/test.gif"), 100,
              {refreshed, sizeof(refreshed)}) == 0 &&
              attachment_refresh_response(response, original, 65535,
                                          {refreshed, sizeof(refreshed)}) == 0,
          "attachment_refresh_reject_mismatched_original_and_expiry");
    resolver = {};
    check(media_url_resolve(&resolver, original, 1000, 100).bytes == 0 &&
              media_url_resolve(&resolver, original, 1001, 100).bytes == 0 &&
              resolver.entries[1].state == MediaUrlState::empty,
          "attachment_refresh_deduplicates_visible_waiters");
    auto* entry = media_url_next(&resolver, 1000);
    check(entry == &resolver.entries[0], "attachment_refresh_queued");
    resolver.pending_slot = 1;
    entry->state = MediaUrlState::pending;
    entry->attempts = 1;
    RestClientResponse ok{};
    ok.status_code = 200;
    media_url_complete(&resolver, ok, response, 1002, 100);
    check(utf8_equal(media_url_resolve(&resolver, original, 1003, 100), fresh) &&
              !media_url_next(&resolver, 1003),
          "attachment_refresh_ready_cache_no_repeat_requests");
    resolver = {};
    media_url_resolve(&resolver, original, 1000, 100);
    RestClientResponse limited{};
    limited.status_code = 429;
    limited.rate_limited = true;
    limited.retry_at_ms = 50'000;
    for (u32 attempt = 1; attempt <= 3; ++attempt) {
        resolver.pending_slot = 1;
        resolver.entries[0].state = MediaUrlState::pending;
        resolver.entries[0].attempts = static_cast<u8>(attempt);
        media_url_complete(&resolver, limited, {}, 2000, 100);
        if (attempt < 3)
            check(!media_url_next(&resolver, 49'999) && media_url_next_wake(resolver) == 50'000,
                  "attachment_refresh_honors_rate_limit_deadline");
    }
    check(resolver.entries[0].state == MediaUrlState::failed && !media_url_next(&resolver, 99'999),
          "attachment_refresh_retry_budget_terminal");
    const auto failed_receipt = resolver;
    check(media_url_resolution_state(resolver, original) == MediaUrlState::failed &&
              media_url_resolution_state(resolver, fresh) == MediaUrlState::empty &&
              bytes_equal(&failed_receipt, &resolver, sizeof(resolver)),
          "attachment_refresh_exact_terminal_query_preserves_retry_and_other_source_authority");
    resolver = {};
    media_url_resolve(&resolver, original, 1000, 100);
    resolver.pending_slot = 1;
    resolver.entries[0].state = MediaUrlState::pending;
    resolver.entries[0].attempts = 1;
    media_url_complete(&resolver, limited, {}, 2000, 100);
    check(media_url_resolution_state(resolver, original) == MediaUrlState::queued &&
              !media_url_next(&resolver, 49'999) && media_url_next(&resolver, 50'000),
          "attachment_refresh_finite_failure_remains_pending_until_its_real_deadline");
    limited.retry_at_ms = 2001;
    resolver.pending_slot = 1;
    resolver.entries[0].state = MediaUrlState::pending;
    media_url_complete(&resolver, limited, {}, 2000, 100);
    check(media_url_next_wake(resolver) == 2001 && !media_url_next(&resolver, 2000) &&
              media_url_next(&resolver, 2001),
          "attachment_refresh_429_uses_exact_server_deadline_without_local_backoff");
    bool terminal_rates = true;
    for (u32 scenario = 0; scenario < 3; ++scenario) {
        resolver = {};
        media_url_resolve(&resolver, original, 1000, 100);
        resolver.pending_slot = 1;
        resolver.entries[0].state = MediaUrlState::pending;
        resolver.entries[0].attempts = 1;
        RestClientResponse terminal{};
        terminal.status_code = 429;
        terminal.rate_limited = scenario != 0;
        terminal.retry_at_ms = scenario == 2 ? 0 : 50'000;
        terminal.rejection.captcha = scenario == 1;
        media_url_complete(&resolver, terminal, {}, 2000, 100);
        terminal_rates = terminal_rates && resolver.entries[0].state == MediaUrlState::failed &&
                         !resolver.pending_slot && !resolver.entries[0].retry_at_ms &&
                         !media_url_next_wake(resolver) && !media_url_next(&resolver, 99'999) &&
                         !media_url_resolve(&resolver, original, 99'999, 100).bytes &&
                         resolver.entries[0].state == MediaUrlState::failed;
    }
    check(terminal_rates,
          "attachment_refresh_verification_unclassified_and_missing_deadline429_never_queue_retry_or_wake");

    MessageRichEmbed embed{};
    embed.type = GatewayMessageEmbedType::image;
    embed.thumbnail.state = 3;
    embed.thumbnail.width = 1280;
    embed.thumbnail.height = 720;
    embed.thumbnail.url.bytes = 10;
    check(message_rich_embed_media_only(embed), "embed_image_thumbnail_normalized_inline");
    embed.has_title = true;
    check(!message_rich_embed_media_only(embed), "embed_empty_but_present_title_keeps_card");
    embed.type = GatewayMessageEmbedType::rich;
    embed.has_title = false;
    embed.image = embed.thumbnail;
    check(!message_rich_embed_media_only(embed), "embed_rich_image_keeps_card");
    embed.type = GatewayMessageEmbedType::gifv;
    embed.has_title = true;
    check(message_rich_embed_media_only(embed), "embed_gifv_title_still_inline");

    static MessageRichRecord records[1];
    u16 order[1]{};
    u8 text[4096]{};
    MessageRichStore store{};
    check(message_rich_store_initialize(&store, {records, 1}, {order, 1}, {text, sizeof(text)}),
          "embed_policy_store");
    GatewayMessageEmbed source{};
    source.type = GatewayMessageEmbedType::image;
    source.url = utf8_literal("https://example.com/animation.webp");
    source.thumbnail.url = source.url;
    source.thumbnail.proxy_url =
        utf8_literal("https://images-ext-1.discordapp.net/external/hash/animation.webp");
    source.thumbnail.width = 512;
    source.thumbnail.height = 288;
    source.thumbnail.has_width = source.thumbnail.has_height = true;
    source.thumbnail.flags = 32; // Embed media animated flag, not unfurled-media flags.
    GatewayMessageEvent event{};
    event.id = 777;
    event.fields = gateway_message_field_embeds;
    event.embeds = {&source, 1};
    const auto applied = message_rich_store_apply(&store, 777, 2, event);
    MessageRichEmbedMediaSelection selected{};
    check(applied.ok() && message_rich_embed_media_select(store, applied.record->embeds[0], &selected) &&
              selected.animated && selected.format == MediaAttachmentFormat::webp &&
              selected.resource == MediaEmbedResource::thumbnail &&
              utf8_equal(selected.source, source.thumbnail.proxy_url) &&
              message_rich_embed_media_only(applied.record->embeds[0]) &&
              message_rich_embed_suppresses_content(store, *applied.record, source.url),
          "embed_image_flag_proxy_animation_inline_policy");
    source.thumbnail.flags = 1; // A different flag must not enable animation.
    const auto still = message_rich_store_apply(&store, 777, 3, event);
    check(still.ok() && message_rich_embed_media_select(store, still.record->embeds[0], &selected) &&
              !selected.animated,
          "embed_static_webp_stays_static");
    u8 target[1024]{};
    const auto frozen_target = utf8_literal("/external/animation.webp?animated=false&format=png");
    const auto transformed = write_media_proxy_target(
        {reinterpret_cast<const u8*>(frozen_target.data), frozen_target.bytes},
        CdnHost::images_ext_1, 512, 288, CdnMediaFormat::webp, {target, sizeof(target)}, true);
    check(
        transformed.ok() &&
            utf8_equal({reinterpret_cast<const char*>(target), transformed.bytes},
                       utf8_literal("/external/animation.webp?width=512&height=288&format=webp&animated=true")),
        "animated_proxy_request_replaces_freeze_hint");
    const auto gif_target = utf8_literal("/attachments/7/8/test.gif?ex=ffff&is=1&hm=abc&format=webp");
    const auto resized_gif = write_media_proxy_target(
        {reinterpret_cast<const u8*>(gif_target.data), gif_target.bytes}, CdnHost::media, 512, 482,
        CdnMediaFormat::source, {target, sizeof(target)}, true);
    check(
        resized_gif.ok() &&
            utf8_equal({reinterpret_cast<const char*>(target), resized_gif.bytes},
                       utf8_literal(
                           "/attachments/7/8/test.gif?ex=ffff&is=1&hm=abc&width=512&height=482&animated=true")),
        "gif_proxy_resize_preserves_encoding_and_signature");

    CdnClient client{};
    const auto started = cdn_client_start(&client, kCdnMaximumEncodedBodyBytes);
    MEMORY_BASIC_INFORMATION region{};
    check(started.ok() && VirtualQuery(client.runtime, &region, sizeof(region)) &&
              region.State == MEM_COMMIT && region.RegionSize < 1024u * 1024u,
          "cdn_reserves_large_response_without_committing_it");
    check(cdn_client_stop(&client, 5000).phase == CdnClientPhase::stopped,
          "cdn_sparse_response_shutdown");

    AnimatedWebpFixtureResult webp{};
    check(build_mixed_animated_webp_fixture({encoded, sizeof(encoded)}, &webp), "message_webp_fixture");
    exercise_animation(
        check, media_asset_embed(777, 2, 0, MediaEmbedResource::image, MediaAttachmentFormat::webp, true),
        webp.encoded_bytes, false);
    GifFixtureResult gif{};
    check(build_indexed_gif_fixture({encoded, sizeof(encoded)}, {scratch, sizeof(scratch)}, &gif),
          "message_gif_fixture");
    exercise_animation(
        check, media_asset_embed(777, 2, 0, MediaEmbedResource::image, MediaAttachmentFormat::gif, true),
        gif.encoded_bytes, true);
    exercise_animation(check, media_asset_attachment(9876, MediaAttachmentFormat::gif), gif.encoded_bytes,
                       true);
    check(build_deferred_clear_gif_fixture({encoded, sizeof(encoded)}, {scratch, sizeof(scratch)}, &gif,
                                           1536, 1024) &&
              gif.encoded_bytes > 2u * 1024u * 1024u,
          "message_gif_large_fixture");
    const auto gif_head = utf8_literal("HTTP/1.1 200 OK\r\nContent-Type: image/gif\r\nTransfer-Encoding: "
                                       "chunked\r\nCache-Control: max-age=60\r\n\r\n");
    check(validate_static_image_response(CdnImageType::gif, {reinterpret_cast<const u8*>(gif_head.data), gif_head.bytes},
                                       gif.encoded_bytes, 100)
              .ok(),
          "message_gif_over_2mb_http_policy");
    exercise_animation(check, media_asset_attachment(9876, MediaAttachmentFormat::gif), gif.encoded_bytes,
                       true, 1);
    local_sample(check, L"fxtwitter-512-animated.webp", false);
    local_sample(check, L"oomfie.gif", true);
    local_sample(check, L"holdon-native.gif", true);
    local_sample(check, L"atlas-pressure-image0.gif", true);
    network_animation_sample(check, true);
    network_animation_sample(check, false);
}
} // namespace maicord
