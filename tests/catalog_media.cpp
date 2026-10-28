#include "maicord/app/account_media_worker.hpp"
#include "maicord/core/time.hpp"
#include "maicord/protocol/cdn_http.hpp"
#include "maicord/protocol/gif_favorites.hpp"
#include "maicord/protocol/base64.hpp"
#include "maicord/ui/message_rich_layout.hpp"
#include "media_fixture.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
constexpr auto kPreviewUrl =
    utf8_literal("https://static.klipy.com/ii/0123456789abcdef0123456789abcdef/01/ab/catalog-preview.webp");
u8 catalog_encoded[16 * 1024];
u8 catalog_scratch[16 * 1024];

void catalog_decode_case(Check check, MediaAttachmentFormat format, bool animated, bool discover_apng,
                         u32 encoded_bytes) noexcept {
    AccountMedia media{};
    if (!account_media_initialize(&media).ok()) {
        check(false, "catalog_media_initialize");
        return;
    }
    const auto key = media_asset_catalog_preview(700, 9, format, animated);
    const auto stored = encoded_media_cache_store(&media.cache, key, {catalog_encoded, encoded_bytes},
                                                  {{}, 50'000, true, false});
    const auto url = format == MediaAttachmentFormat::webp ? kPreviewUrl
                     : format == MediaAttachmentFormat::gif
                         ? utf8_literal("https://media.discordapp.net/stickers/42.gif")
                         : utf8_literal("https://cdn.discordapp.com/stickers/42.png");
    const AccountMediaCatalogPreviewRequest request{url, key, 300, 9, 64, 64, format, {}};
    const auto requested = account_media_request_catalog_preview(&media, request, 100, 100);
    AccountMediaFrame first{};
    const bool taken = account_media_take_frame(&media, &first);
    const bool ready = stored.stored() && requested.state == AccountMediaRequestState::ready && taken &&
                       first.source_kind == AccountMediaSourceKind::catalog_preview &&
                       first.source_catalog_scope == request.catalog_scope &&
                       first.source_catalog_generation == 9 && !first.source_message_id &&
                       !first.source_message_revision && media_asset_key_equal(first.key, key) &&
                       !media.client_started;
    check(ready, format == MediaAttachmentFormat::webp
                     ? "catalog_media_cached_webp_initial_frame_has_typed_owner_without_network"
                 : format == MediaAttachmentFormat::gif
                     ? "catalog_media_cached_gif_initial_frame_has_typed_owner_without_network"
                 : discover_apng
                     ? "catalog_media_cached_discovered_apng_initial_frame_has_typed_owner_without_network"
                 : animated ? "catalog_media_cached_apng_initial_frame_has_typed_owner_without_network"
                            : "catalog_media_cached_png_initial_frame_has_typed_owner_without_network");
    if (discover_apng)
        check(!(key.flags & media_asset_flag_animated) &&
                  first.kind == AccountMediaFrameKind::apng_scanlines && first.frame_count > 1,
              "catalog_media_png_decoder_discovers_apng_without_identity_change");
    if (taken && first.frame_count > 1) {
        bool advanced = true;
        for (u32 index = 1; index < first.frame_count && advanced; ++index) {
            const auto next = account_media_advance_animation(&media, key, stored.generation, index, 100);
            AccountMediaFrame frame{};
            advanced =
                next.state == AccountMediaRequestState::ready && account_media_take_frame(&media, &frame) &&
                frame.source_kind == AccountMediaSourceKind::catalog_preview &&
                !frame.source_catalog_scope && !frame.source_catalog_generation &&
                !frame.source_message_id && !frame.source_message_revision &&
                media_asset_key_equal(frame.key, key) && frame.frame_index == index && frame.duration_us;
        }
        check(advanced && !media.client_started,
              "catalog_media_cold_advances_require_retained_token_and_reuse_cache");
    }
    check(account_media_stop(&media, 5'000), "catalog_media_cached_decoder_joins");
}

void catalog_worker_case(Check check, u32 encoded_bytes) noexcept {
    AccountMediaWorker worker{};
    if (!account_media_worker_initialize(&worker).ok()) {
        check(false, "catalog_media_worker_initialize");
        return;
    }
    MediaAssetKey keys[kAccountMediaDecodeLaneCount]{};
    bool queued = true;
    bool shared = true;
    for (u32 index = 0; index < kAccountMediaDecodeLaneCount; ++index) {
        keys[index] = media_asset_catalog_preview(800 + index, 11);
        const auto stored =
            encoded_media_cache_store(&worker.lanes[0].media.cache, keys[index],
                                      {catalog_encoded, encoded_bytes}, {{}, 50'000, true, false});
        char borrowed[kCdnMaximumTargetBytes]{};
        CopyMemory(borrowed, kPreviewUrl.data, kPreviewUrl.bytes);
        const AccountMediaCatalogPreviewRequest request{
            {borrowed, kPreviewUrl.bytes}, keys[index], 301, 11, 64, 64, MediaAttachmentFormat::webp, {}};
        const auto admitted =
            account_media_worker_request_catalog_preview(&worker, request, 100 + index, 100);
        // The catalog may replace its source bank immediately after admission.
        secure_clear_bytes(borrowed, sizeof(borrowed));
        queued = queued && stored.stored() && admitted.state == AccountMediaRequestState::queued;
        shared = shared && worker.lanes[index].media.pipeline.cache == &worker.lanes[0].media.cache &&
                 worker.lanes[index].media.owns_cache == (index == 0);
    }
    u32 frames = 0;
    bool seen[kAccountMediaDecodeLaneCount]{};
    bool owned = true;
    const u64 deadline = clock_monotonic_milliseconds() + 2'000;
    while (frames < kAccountMediaDecodeLaneCount && clock_monotonic_milliseconds() < deadline) {
        AccountMediaFrame frame{};
        if (account_media_worker_take_frame(&worker, &frame)) {
            bool matched = false;
            for (u32 index = 0; index < kAccountMediaDecodeLaneCount; ++index)
                if (!seen[index] && media_asset_key_equal(frame.key, keys[index])) {
                    seen[index] = true;
                    ++frames;
                    matched = true;
                    break;
                }
            owned = owned && matched && frame.source_kind == AccountMediaSourceKind::catalog_preview &&
                    frame.source_catalog_scope == 301 && frame.source_catalog_generation == 11 &&
                    !frame.source_message_id && !frame.source_message_revision && frame.frame_count > 1;
            continue;
        }
        const u64 now = clock_monotonic_milliseconds();
        WaitForSingleObject(account_media_worker_notification_handle(&worker),
                            deadline > now ? static_cast<DWORD>(deadline - now) : 1);
    }
    bool no_network = true;
    for (const auto& lane : worker.lanes)
        no_network = no_network && !lane.media.client_started;
    check(queued && shared && owned && frames == kAccountMediaDecodeLaneCount && no_network,
          "catalog_media_eight_lanes_copy_source_bank_and_share_encoded_cache");
    check(account_media_worker_stop(&worker, 5'000), "catalog_media_existing_worker_lanes_join");
}

// Visible VIDEO tiles beyond the decode lanes wait for a decoder: while every
// idle lane's resident decoder still plays, a new video source is not admitted
// (no eviction, no download), so tiles cannot restart each other forever.
void catalog_video_lane_case(Check check) noexcept {
    AccountMediaWorker worker{};
    if (!account_media_worker_initialize(&worker).ok()) {
        check(false, "catalog_media_video_worker_initialize");
        return;
    }
    Mp4Decoder playing{};
    playing.ready = true;
    Mp4Decoder* resident[kAccountMediaDecodeLaneCount]{};
    const u64 now = clock_monotonic_milliseconds();
    for (u32 index = 0; index < kAccountMediaDecodeLaneCount; ++index) {
        auto& media = worker.lanes[index].media;
        resident[index] = media.mp4_decoder;
        media.mp4_decoder = &playing;
        media.mp4_key = media_asset_catalog_preview(900 + index, 12, MediaAttachmentFormat::mp4);
        media.mp4_used_ms = now;
    }
    constexpr auto url = utf8_literal(
        "https://static.klipy.com/ii/4493325008d34b7bf8cd6813cd5c1619/80/4e/BgpWPn9XyO5HDdwMMc.mp4");
    const auto key = media_asset_catalog_preview(990, 12, MediaAttachmentFormat::mp4);
    const AccountMediaCatalogPreviewRequest request{url, key, 302, 12, 160, 90, MediaAttachmentFormat::mp4, {}, nullptr};
    const auto waited = account_media_worker_request_catalog_preview(&worker, request, now, 100);
    bool idle = true, offline = true;
    for (u32 index = 0; index < kAccountMediaDecodeLaneCount; ++index) {
        auto& lane = worker.lanes[index];
        idle = idle && lane.phase == static_cast<LONG>(AccountMediaWorkerPhase::idle);
        offline = offline && !lane.media.client_started;
        lane.media.mp4_decoder = resident[index];
        lane.media.mp4_key = {};
        lane.media.mp4_used_ms = 0;
    }
    AccountMediaCatalogPreviewRequest mismatched = request;
    mismatched.format = MediaAttachmentFormat::webp;
    check(waited.state == AccountMediaRequestState::busy && idle && offline &&
              account_media_worker_request_catalog_preview(&worker, mismatched, now, 100).state ==
                  AccountMediaRequestState::invalid,
          "catalog_media_video_tiles_wait_for_a_decoder_instead_of_evicting_playing_ones");
    check(account_media_worker_stop(&worker, 5'000), "catalog_media_video_worker_joins");

    // The account boundary admits the mp4 format for its own key and refuses
    // any source the video path does not admit before touching the network.
    AccountMedia media{};
    if (!account_media_initialize(&media).ok()) {
        check(false, "catalog_media_video_media_initialize");
        return;
    }
    AccountMediaCatalogPreviewRequest refused = request;
    refused.url = utf8_literal("https://static.klipy.com/ii/4e7bea9f7a3371424e6d16ebab93db50/8b/4c/WqDpNsyW.webm");
    const auto webm = account_media_request_catalog_preview(&media, refused, 100, 100);
    refused.url = utf8_literal("https://media.tenor.com/AbCd-fGh_jKAAAPo/funny-cat.mp4");
    refused.key = media_asset_catalog_preview(991, 12, MediaAttachmentFormat::mp4);
    const auto tenor = account_media_request_catalog_preview(&media, refused, 200, 100);
    check(webm.state == AccountMediaRequestState::failed && webm.error == AccountMediaError::target &&
              tenor.state == AccountMediaRequestState::failed && tenor.error == AccountMediaError::target &&
              !media.client_started,
          "catalog_media_video_tile_refuses_undecodable_sources_without_network");
    check(account_media_stop(&media, 5'000), "catalog_media_video_media_joins");
}

// The same favourites sent in chat unfurl as gifv embeds whose media loads
// through the signed external proxy: Klipy's MP4 plays as video and the
// external animated WebP animates, from either embed shape the server produces.
void chat_gif_embed_case(Check check) noexcept {
    static MessageRichRecord records[1];
    static u8 text[8192];
    u16 order[1]{};
    MessageRichStore store{};
    if (!message_rich_store_initialize(&store, {records, 1}, {order, 1}, {text, sizeof(text)})) {
        check(false, "chat_gif_embed_store");
        return;
    }
    constexpr auto klipy_page = utf8_literal("https://klipy.com/gifs/cat-drool-1");
    constexpr auto klipy_video = utf8_literal(
        "https://static.klipy.com/ii/4493325008d34b7bf8cd6813cd5c1619/80/4e/BgpWPn9XyO5HDdwMMc.mp4");
    constexpr auto klipy_proxy = utf8_literal(
        "https://images-ext-1.discordapp.net/external/a1B2_c3-D4/https/static.klipy.com/ii/"
        "4493325008d34b7bf8cd6813cd5c1619/80/4e/BgpWPn9XyO5HDdwMMc.mp4");
    constexpr auto fx = utf8_literal("https://gif.fxtwitter.com/tweet_video/HQASuXQa4AEbQle.webp");
    constexpr auto fx_proxy = utf8_literal(
        "https://images-ext-2.discordapp.net/external/e5F6_g7-H8/https/gif.fxtwitter.com/tweet_video/"
        "HQASuXQa4AEbQle.webp");
    struct Case {
        GatewayMessageEmbedType type;
        Utf8View page, media, proxy;
        bool video;
        u32 flags;
        MediaAttachmentFormat format;
        MediaEmbedResource resource;
        CdnHost host;
        const char* label;
    };
    const Case cases[]{
        {GatewayMessageEmbedType::gifv, klipy_page, klipy_video, klipy_proxy, true, 0, MediaAttachmentFormat::mp4,
         MediaEmbedResource::video, CdnHost::images_ext_1,
         "chat_klipy_gifv_plays_its_mp4_through_the_signed_proxy"},
        {GatewayMessageEmbedType::gifv, fx, fx, fx_proxy, true, 0, MediaAttachmentFormat::webp,
         MediaEmbedResource::video, CdnHost::images_ext_2,
         "chat_external_gifv_webp_animates_through_the_signed_proxy"},
        {GatewayMessageEmbedType::image, fx, fx, fx_proxy, false, 32, MediaAttachmentFormat::webp,
         MediaEmbedResource::thumbnail, CdnHost::images_ext_2,
         "chat_external_animated_image_embed_animates_through_the_signed_proxy"},
    };
    u32 revision = 2;
    for (const auto& c : cases) {
        GatewayMessageEmbed source{};
        source.type = c.type;
        source.url = c.page;
        source.thumbnail.url = c.media;
        source.thumbnail.proxy_url = c.proxy;
        source.thumbnail.flags = c.flags;
        if (c.video) {
            // A gifv's poster thumbnail never replaces its playing media.
            source.thumbnail.url = utf8_literal("https://example.com/poster.png");
            source.thumbnail.proxy_url =
                utf8_literal("https://images-ext-1.discordapp.net/external/p/https/example.com/poster.png");
            source.video.url = c.media;
            source.video.proxy_url = c.proxy;
            source.video.width = 498;
            source.video.height = 280;
            source.video.has_width = source.video.has_height = true;
        }
        source.thumbnail.width = 498;
        source.thumbnail.height = 280;
        source.thumbnail.has_width = source.thumbnail.has_height = true;
        GatewayMessageEvent event{};
        event.id = 778;
        event.fields = gateway_message_field_embeds;
        event.embeds = {&source, 1};
        const u32 current = revision++;
        const auto applied = message_rich_store_apply(&store, 778, current, event);
        MessageRichEmbedMediaSelection selected{};
        bool ok = applied.ok() && message_rich_embed_media_select(store, applied.record->embeds[0], &selected) &&
                  selected.animated && selected.format == c.format && selected.resource == c.resource &&
                  selected.source.bytes == c.proxy.bytes &&
                  bytes_equal(selected.source.data, c.proxy.data, c.proxy.bytes) &&
                  media_asset_key_valid(media_asset_embed(778, current, 0, selected.resource, selected.format, true,
                                                          MediaResolutionTier::px512));
        u8 target[kCdnMaximumTargetBytes]{}, proxied[kCdnMaximumTargetBytes]{};
        u8 head[kCdnMaximumRequestHeadBytes]{};
        const auto parsed = ok ? parse_media_url(selected.source, {target, sizeof(target)}) : MediaUrlResult{};
        ok = ok && parsed.ok() && parsed.host == c.host;
        if (ok && c.format == MediaAttachmentFormat::mp4) {
            ok = write_static_cdn_get_request(CdnImageType::mp4, parsed.host,
                                              {target, parsed.target_bytes}, {}, {head, sizeof(head)})
                     .ok();
        } else if (ok) {
            const auto sized = write_media_proxy_target({target, parsed.target_bytes}, parsed.host, 400, 225,
                                                        CdnMediaFormat::webp, {proxied, sizeof(proxied)}, true);
            ok = sized.ok() && sized.bytes > 14 && bytes_equal(proxied + sized.bytes - 14, "&animated=true", 14) &&
                 write_static_cdn_get_request(CdnImageType::webp, sized.host, {proxied, sized.bytes},
                                              {}, {head, sizeof(head)})
                     .ok();
        }
        check(ok, c.label);
    }
}

// A tile that keeps its placeholder logs where its source lives (host, the
// origin behind the media proxy, the extension), never its path or query.
void tile_fallback_log_case(Check check) noexcept {
    struct Case {
        Utf8View url;
        const char* fields;
    };
    const Case cases[]{
        {utf8_literal("https://static.klipy.com/ii/4493325008d34b7bf8cd6813cd5c1619/80/4e/BgpWPn9XyO5HDdwMMc.mp4"),
         " host=static.klipy.com ext=mp4"},
        {utf8_literal("https://images-ext-1.discordapp.net/external/a1B2_c3-D4/https/static.klipy.com/ii/"
                      "4493325008d34b7bf8cd6813cd5c1619/80/4e/x.gif?format=webp&animated=true"),
         " host=images-ext-1.discordapp.net ext=gif via=static.klipy.com"},
        {utf8_literal("https://cdn.discordapp.com/attachments/1/2/clip.mp4?ex=1&is=2&hm=ab"),
         " host=cdn.discordapp.com ext=mp4"},
        {utf8_literal("https://klipy.com/gifs/cat-drool-1"), " host=klipy.com ext=none"},
        {utf8_literal("//static.klipy.com/ii/x.webm"), " host=none"},
    };
    bool exact = true;
    for (const auto& c : cases) {
        char text[128]{};
        const u32 bytes = media_url_log_fields(c.url, {text, sizeof(text)});
        const u32 expected = c_string_length(c.fields);
        exact = exact && bytes == expected && bytes_equal(text, c.fields, bytes);
    }
    char small[8]{};
    exact = exact && media_url_log_fields(cases[0].url, {small, sizeof(small)}) == sizeof(small) &&
            bytes_equal(small, " host=st", sizeof(small));
    check(exact, "catalog_media_tile_fallback_log_names_host_and_extension_without_path_or_query");
}


// The reported Klipy VIDEO favourite exactly as the frecency settings
// store it: favourite GIFs keyed by the GIF's page URL, the value {format VIDEO,
// src = the MP4 asset, width, height, order}. Decode, preview mapping, tile
// key, the provider request and Klipy's real response head all admit it from
// src (never the page key), on static.klipy.com and its static2 host.
struct ProtoWriter {
    u8* bytes;
    u32 capacity, used;
    void number(u64 n) noexcept {
        do {
            const u8 b = static_cast<u8>(n & 127);
            n >>= 7;
            if (used < capacity)
                bytes[used++] = static_cast<u8>(b | (n ? 128 : 0));
        } while (n);
    }
    void scalar(u32 field, u64 n) noexcept {
        number(field * 8);
        number(n);
    }
    void message(u32 field, const u8* data, u32 count) noexcept {
        number(field * 8 + 2);
        number(count);
        for (u32 i = 0; i < count && used < capacity; ++i)
            bytes[used++] = data[i];
    }
    void string(u32 field, Utf8View text) noexcept {
        message(field, reinterpret_cast<const u8*>(text.data), text.bytes);
    }
};
void klipy_video_favourite_case(Check check) noexcept {
    constexpr auto page = utf8_literal("https://klipy.com/gifs/cat-drool-1");
    constexpr auto mp4 = utf8_literal(
        "https://static.klipy.com/ii/4493325008d34b7bf8cd6813cd5c1619/80/4e/BgpWPn9XyO5HDdwMMc.mp4");
    constexpr char path[] = "/ii/4493325008d34b7bf8cd6813cd5c1619/80/4e/BgpWPn9XyO5HDdwMMc.mp4";
    u8 value_bytes[256]{}, entry_bytes[512]{}, group_bytes[640]{}, versions_bytes[32]{}, root_bytes[768]{};
    ProtoWriter value{value_bytes, sizeof(value_bytes), 0}, entry{entry_bytes, sizeof(entry_bytes), 0};
    ProtoWriter group{group_bytes, sizeof(group_bytes), 0}, versions{versions_bytes, sizeof(versions_bytes), 0};
    ProtoWriter root{root_bytes, sizeof(root_bytes), 0};
    value.scalar(1, 2); // GIFType VIDEO
    value.string(2, mp4);
    value.scalar(3, 498);
    value.scalar(4, 280);
    value.scalar(5, 1);
    entry.string(1, page);
    entry.message(2, value_bytes, value.used);
    group.message(1, entry_bytes, entry.used);
    versions.scalar(1, 7);
    versions.scalar(3, 9);
    root.message(1, versions_bytes, versions.used);
    root.message(2, group_bytes, group.used);
    static char settings_text[base64_encoded_size(sizeof(root_bytes))];
    const auto encoded = base64_encode({root_bytes, root.used}, {settings_text, sizeof(settings_text)});
    const Utf8View settings{settings_text, encoded.ok() ? encoded.bytes : 0};
    const auto measured = gif_favorites_measure_base64(settings);
    static u8 owned[1024];
    static GifFavoriteRecord records[4];
    static u32 index[64];
    const auto decoded =
        measured.ok() && measured.group_bytes <= sizeof(owned) && measured.entry_count <= 4 &&
                measured.index_words <= 64
            ? gif_favorites_decode_base64(settings, {owned, measured.group_bytes}, {records, measured.entry_count},
                                          {index, measured.index_words})
            : GifFavoritesResult{};
    const GifFavoriteRecord& record = records[0];
    check(decoded.ok() && decoded.record_count == 1 && utf8_equal(record.url, page) &&
              utf8_equal(record.src, mp4) &&
              record.format == GifFavoriteFormat::video && record.width == 498 && record.height == 280,
          "catalog_media_klipy_video_favourite_decodes_page_key_and_mp4_src");
    // The picker maps the record's src (the page key is no media URL) to a
    // VIDEO tile: mp4 format, an animated catalog key and Klipy's own asset.
    u8 target[kCdnMaximumTargetBytes]{};
    const auto format = catalog_favorite_format(record.src);
    const auto mapped =
        parse_catalog_preview_url(record.src, {target, sizeof(target)}, format, record.width, record.height);
    const auto key = media_asset_catalog_preview(41, 3, format);
    check(format == MediaAttachmentFormat::mp4 && mapped.ok() && mapped.host == CdnHost::catalog_klipy &&
              mapped.target_bytes == sizeof(path) - 1 && bytes_equal(target, path, sizeof(path) - 1) &&
              media_asset_catalog_format(key) == MediaAttachmentFormat::mp4 &&
              (key.flags & media_asset_flag_animated) && media_asset_key_valid(key) &&
              !parse_catalog_preview_url(record.url, {target, sizeof(target)}, format).ok() &&
              catalog_favorite_format(record.url) == MediaAttachmentFormat::webp,
          "catalog_media_klipy_video_favourite_maps_its_src_to_an_mp4_tile_key");
    // The tile's request and Klipy's actual answer (2026-10-12) for that asset.
    u8 head[kCdnMaximumRequestHeadBytes]{};
    const auto request = write_static_cdn_get_request(CdnImageType::mp4, mapped.host,
                                                      {target, mapped.target_bytes}, {}, {head, sizeof(head)});
    const Utf8View written{reinterpret_cast<const char*>(head), request.bytes};
    const auto has = [&](const char* text) noexcept {
        const u32 bytes = c_string_length(text);
        for (u32 i = 0; i + bytes <= written.bytes; ++i)
            if (bytes_equal(written.data + i, text, bytes))
                return true;
        return false;
    };
    constexpr auto answer = utf8_literal(
        "HTTP/1.1 200 OK\r\nDate: Sun, 11 Oct 2026 23:44:35 GMT\r\nContent-Type: video/mp4\r\n"
        "Content-Length: 476674\r\nConnection: keep-alive\r\nCF-Ray: a491bbd05db92a09-LJU\r\n"
        "CF-Cache-Status: HIT\r\nAccept-Ranges: bytes\r\naccess-control-allow-origin: *\r\nAge: 6060\r\n"
        "cache-control: public, max-age=86400\r\nETag: \"3bbd4d56650c52d8f3886749dad26af7\"\r\n"
        "Expires: Mon, 12 Oct 2026 23:44:35 GMT\r\nLast-Modified: Sat, 04 Jul 2026 10:35:49 GMT\r\n"
        "Server: cloudflare\r\nStrict-Transport-Security: max-age=31536000; includeSubDomains; preload\r\n"
        "X-Robots-Tag: noindex\r\n\r\n");
    const auto response = validate_static_image_response(
        CdnImageType::mp4, {reinterpret_cast<const u8*>(answer.data), answer.bytes}, 476674, 1'791'000'000);
    check(request.ok() && has("GET /ii/4493325008d34b7bf8cd6813cd5c1619/80/4e/BgpWPn9XyO5HDdwMMc.mp4 HTTP/1.1") &&
              has("Host: static.klipy.com\r\n") && has("Accept: video/mp4\r\n") && response.ok() &&
              response.disposition == CdnResponseDisposition::accept_body && response.body_bytes == 476674 &&
              response.store_allowed,
          "catalog_media_klipy_video_favourite_requests_the_mp4_and_admits_klipys_answer");
    // static2.klipy.com serves the same asset shapes.
    struct Static2 {
        Utf8View src;
        MediaAttachmentFormat format;
        CdnImageType type;
    };
    const Static2 shapes[]{
        {utf8_literal("https://static2.klipy.com/ii/4493325008d34b7bf8cd6813cd5c1619/80/4e/BgpWPn9XyO5HDdwMMc.mp4"),
         MediaAttachmentFormat::mp4, CdnImageType::mp4},
        {utf8_literal("https://static2.klipy.com/ii/4e7bea9f7a3371424e6d16ebab93db50/8b/4c/WqDpNsyW.gif"),
         MediaAttachmentFormat::gif, CdnImageType::gif},
        {utf8_literal("https://static2.klipy.com/ii/4e7bea9f7a3371424e6d16ebab93db50/8b/4c/WqDpNsyW.webp"),
         MediaAttachmentFormat::webp, CdnImageType::webp},
    };
    bool second = true;
    for (const auto& shape : shapes) {
        const auto parsed = parse_catalog_preview_url(shape.src, {target, sizeof(target)},
                                                      catalog_favorite_format(shape.src), 498, 280);
        u8 second_head[kCdnMaximumRequestHeadBytes]{};
        const auto get = write_static_cdn_get_request(shape.type, parsed.host, {target, parsed.target_bytes},
                                                      {}, {second_head, sizeof(second_head)});
        second = second && catalog_favorite_format(shape.src) == shape.format && parsed.ok() &&
                 parsed.host == CdnHost::catalog_klipy2 && target[0] == '/' && get.ok() &&
                 bytes_equal(second_head, "GET /ii/", 8);
        const Utf8View sent{reinterpret_cast<const char*>(second_head), get.bytes};
        bool host = false;
        for (u32 i = 0; i + 25 <= sent.bytes && !host; ++i)
            host = bytes_equal(sent.data + i, "Host: static2.klipy.com\r\n", 25);
        second = second && host;
    }
    constexpr auto category = utf8_literal(
        "https://images-ext-1.discordapp.net/external/abc/https/static2.klipy.com/ii/"
        "4e7bea9f7a3371424e6d16ebab93db50/8b/4c/category.gif?format=webp&animated=true&width=220&height=220");
    const auto proxied = parse_catalog_preview_url(category, {target, sizeof(target)});
    second = second && proxied.ok() && proxied.host == CdnHost::images_ext_1 &&
             !parse_catalog_preview_url(utf8_literal("https://static2.klipy.com.evil/ii/x.gif"),
                                        {target, sizeof(target)}, MediaAttachmentFormat::gif)
                  .ok() &&
             !parse_catalog_preview_url(utf8_literal("https://static3.klipy.com/ii/x.webp"),
                                        {target, sizeof(target)})
                  .ok();
    check(second, "catalog_media_klipy_static2_host_admits_the_same_gif_webp_mp4_and_proxy_shapes");
}

} // namespace

void run_catalog_media_tests(void (*check)(bool, const char*) noexcept) noexcept {
    const auto key = media_asset_catalog_preview(700, 9);
    check(media_asset_key_valid(key) && key.kind == MediaAssetKind::catalog_preview &&
              !media_asset_key_valid(media_asset_catalog_preview(0, 9)) &&
              !media_asset_key_valid(media_asset_catalog_preview(700, 0)) &&
              !media_asset_key_valid(media_asset_catalog_preview(700, 9, MediaAttachmentFormat::jpeg)) &&
              !media_asset_key_equal(key, media_asset_attachment(700, MediaAttachmentFormat::webp,
                                                                 MediaResolutionTier::px256, true)),
          "catalog_media_identity_is_opaque_typed_and_generation_scoped");
    const auto video_key = media_asset_catalog_preview(700, 9, MediaAttachmentFormat::mp4);
    check(media_asset_key_valid(video_key) && (video_key.flags & media_asset_flag_animated) != 0 &&
              media_asset_catalog_format(video_key) == MediaAttachmentFormat::mp4 &&
              !media_asset_key_valid(MediaAssetKey{700, 9, MediaAssetKind::catalog_preview,
                                                   static_cast<u16>(video_key.flags & ~media_asset_flag_animated)}),
          "catalog_media_video_favourite_identity_is_an_animated_mp4_tile");
    u8 target[kCdnMaximumTargetBytes]{};
    const auto parsed = parse_catalog_preview_url(kPreviewUrl, {target, sizeof(target)});
    check(parsed.ok() && parsed.host == CdnHost::catalog_klipy &&
              !parse_media_url(kPreviewUrl, {target, sizeof(target)}).ok(),
          "catalog_media_verified_origin_is_excluded_from_message_url_policy");
    constexpr auto category_url = utf8_literal(
        "https://images-ext-1.discordapp.net/external/fixtureToken/https/static.klipy.com/ii/"
        "0123456789abcdef0123456789abcdef/01/ab/category.gif?keep_aspect_ratio=true&height=220&"
        "width=220&format=webp&animated=true");
    const auto category = parse_catalog_preview_url(category_url, {target, sizeof(target)});
    check(category.ok() && category.host == CdnHost::images_ext_1 &&
              bytes_equal(target, "/external/", 10) &&
              bytes_equal(target + category.target_bytes - 13, "animated=true", 13),
          "catalog_media_category_wrapped_gif_preserves_explicit_animated_webp_proxy_target");
    constexpr Utf8View rejected_categories[]{
        utf8_literal(
            "https://images-ext-1.discordapp.net.evil/external/t/https/static.klipy.com/ii/"
            "0123456789abcdef0123456789abcdef/01/ab/a.gif?width=220&height=220&format=webp&animated=true"),
        utf8_literal(
            "https://images-ext-1.discordapp.net/external/t/https/static.klipy.com.evil/ii/"
            "0123456789abcdef0123456789abcdef/01/ab/a.gif?width=220&height=220&format=webp&animated=true"),
        utf8_literal(
            "https://images-ext-1.discordapp.net/external/t/https/static.klipy.com/ii/"
            "0123456789abcdef0123456789abcdef/01/ab/a.gif?width=220&height=220&format=gif&animated=true"),
        utf8_literal("https://images-ext-1.discordapp.net/external/t/https/static.klipy.com/ii/"
                     "0123456789abcdef0123456789abcdef/01/ab/"
                     "a.gif?width=220&height=220&format=webp&format=webp&animated=true"),
        utf8_literal(
            "https://images-ext-1.discordapp.net/external/t/https/static.klipy.com/ii/"
            "0123456789abcdef0123456789abcdef/01/ab/a.gif?width=513&height=220&format=webp&animated=true"),
        utf8_literal(
            "https://images-ext-1.discordapp.net/external/t/https/static.klipy.com/ii/"
            "0123456789abcdef0123456789abcdef/01/ab/a.gif?width=220&height=220&format=webp&animated=false"),
        utf8_literal("https://images-ext-1.discordapp.net/external/t/https/static.klipy.com/ii/"
                     "0123456789abcdef0123456789abcdef/01/ab/../"
                     "a.gif?width=220&height=220&format=webp&animated=true"),
        utf8_literal(
            "https://images-ext-1.discordapp.net/external/t/https/static.klipy.com/ii/"
            "0123456789abcdef0123456789abcdef/01/ab/a.gif?width=220&height=220&format=webp&animated=true&"),
        utf8_literal("https://images-ext-1.discordapp.net/external/t/https/static.klipy.com/ii/"
                     "0123456789abcdef0123456789abcdef/01/ab/"
                     "a.gif?width=220&height=220&format=webp&animated=true#fragment")};
    // Not the exact category rendition: refused, or rebuilt behind the
    // same signed path with only the client's own bounded query.
    constexpr char own_query[] = "?format=webp&animated=true&width=160&height=160";
    constexpr u32 own_query_bytes = sizeof(own_query) - 1;
    bool category_fenced = true;
    u32 category_refused = 0;
    for (auto url : rejected_categories) {
        const auto mapped = parse_catalog_preview_url(url, {target, sizeof(target)});
        u32 queries = 0;
        for (u32 i = 0; mapped.ok() && i < mapped.target_bytes; ++i)
            queries += target[i] == '?';
        category_refused += !mapped.ok();
        category_fenced &= !mapped.ok() ||
                           (mapped.target_bytes > own_query_bytes && queries == 1 &&
                            bytes_equal(target + mapped.target_bytes - own_query_bytes, own_query, own_query_bytes));
    }
    check(category_fenced && category_refused == 3 &&
              parse_catalog_preview_url(category_url, {target, 1}).error ==
                  MediaUrlError::output_capacity &&
              !parse_catalog_preview_url(category_url, {target, sizeof(target)},
                                         MediaAttachmentFormat::gif)
                   .ok(),
          "catalog_media_proxy_origin_path_format_bounds_and_duplicate_queries_are_fenced");
    constexpr Utf8View rejected[] = {
        utf8_literal(
            "https://static.klipy.com.evil/ii/0123456789abcdef0123456789abcdef/01/ab/preview.webp"),
        utf8_literal("https://static.klipy.com:443/ii/0123456789abcdef0123456789abcdef/01/ab/preview.webp"),
        utf8_literal(
            "https://user@static.klipy.com/ii/0123456789abcdef0123456789abcdef/01/ab/preview.webp"),
        utf8_literal(
            "https://static.klipy.com/ii/0123456789abcdef0123456789abcdef/01/ab/preview.webp?width=64"),
        // Only the favourite action's own parameters, each once, may follow.
        utf8_literal("https://static.klipy.com/ii/0123456789abcdef0123456789abcdef/01/ab/preview.webp?"),
        utf8_literal(
            "https://static.klipy.com/ii/0123456789abcdef0123456789abcdef/01/ab/preview.webp?animated=false"),
        utf8_literal("https://static.klipy.com/ii/0123456789abcdef0123456789abcdef/01/ab/preview.webp"
                     "?animated=true&animated=true"),
        utf8_literal(
            "https://static.klipy.com/ii/0123456789abcdef0123456789abcdef/01/ab/preview.webp?animated=true&"),
        utf8_literal("https://static.klipy.com/ii/0123456789abcdef0123456789abcdef/01/ab/preview.webp"
                     "?animated=true&width=64"),
        utf8_literal(
            "https://static.klipy.com/ii/0123456789abcdef0123456789abcdef/01/ab/preview.webp?animated=true#x"),
        utf8_literal(
            "https://static.klipy.com/ii/0123456789abcdef0123456789abcdef/01/ab/pre.view.webp?animated=true"),
        utf8_literal("https://static.klipy.com/ii/0123456789abcdef0123456789abcdef/01/ab/preview.gif.webp"),
        utf8_literal("https://static.klipy.com/ii/0123456789abcdef0123456789abcdef/01/ab/../preview.webp"),
        utf8_literal("https://static.klipy.com/ii/0123456789abcdef0123456789abcdef/01/ab/%2e%2e.webp"),
        utf8_literal("https://static.klipy.com/ii/0123456789abcdef0123456789abcdef/01/ab/preview.png"),
        utf8_literal("https://klipy.com/gifs/item"),
        utf8_literal("https://media.tenor.com/item/preview.webp"),
    };
    bool fenced = true;
    for (const auto url : rejected)
        fenced = fenced && !parse_catalog_preview_url(url, {target, sizeof(target)}).ok();
    check(fenced, "catalog_media_unverified_origins_and_noncanonical_targets_reject");
    const auto sticker =
        parse_catalog_preview_url(utf8_literal("https://cdn.discordapp.com/stickers/42.png"),
                                  {target, sizeof(target)}, MediaAttachmentFormat::png);
    check(
        sticker.ok() &&
            !parse_catalog_preview_url(
                 utf8_literal("https://cdn.discordapp.com/attachments/1/42/item.png"),
                 {target, sizeof(target)}, MediaAttachmentFormat::png)
                 .ok() &&
            !parse_catalog_preview_url(utf8_literal("https://cdn.discordapp.com/stickers/42.json"),
                                       {target, sizeof(target)}, MediaAttachmentFormat::png)
                 .ok(),
        "catalog_media_sticker_preview_route_does_not_impersonate_attachment_admission");
    const auto lottie=parse_catalog_preview_url(
        utf8_literal("https://cdn.discordapp.com/stickers/42.json"),{target,sizeof(target)},
        MediaAttachmentFormat::lottie);
    u8 request[kCdnMaximumRequestHeadBytes]{};
    const auto get=lottie.ok() ? write_static_cdn_get_request(CdnImageType::lottie_json,
        lottie.host,{target,lottie.target_bytes},{},{request,sizeof(request)}) : CdnRequestResult{};
    check(lottie.ok() && get.ok() &&
        !parse_catalog_preview_url(utf8_literal("https://media.discordapp.net/stickers/42.json"),
            {target,sizeof(target)},MediaAttachmentFormat::lottie).ok() &&
        !parse_catalog_preview_url(utf8_literal("https://cdn.discordapp.com/stickers/42.json?a=1"),
            {target,sizeof(target)},MediaAttachmentFormat::lottie).ok() &&
        !sticker_json_target_valid({reinterpret_cast<const u8*>("/stickers/0.json"),16}) &&
        cdn_encoded_body_limit(CdnImageType::lottie_json)==1024*1024,
        "catalog_media_lottie_requires_exact_cdn_numeric_json_target_and_bounded_json_http_type");

    // GIF favourites synced from the official client carry provider media in
    // any rendition (FavoriteGIF.format VIDEO for mp4/webm); each previews as
    // an animated WebP.
    struct Favourite { Utf8View src; CdnHost host; const char* target; };
    const Favourite favourites[]{
        {utf8_literal("https://media.tenor.com/AbCd-fGh_jKAAAPo/funny-cat.mp4"), CdnHost::catalog_tenor, "/AbCd-fGh_jKAAAA1/funny-cat.webp"},
        {utf8_literal("https://media.tenor.com/AbCd-fGh_jKAAAPs/funny-cat.webm"), CdnHost::catalog_tenor, "/AbCd-fGh_jKAAAA1/funny-cat.webp"},
        {utf8_literal("https://media.tenor.com/AbCd-fGh_jKAAAAC/funny-cat.gif"), CdnHost::catalog_tenor, "/AbCd-fGh_jKAAAA1/funny-cat.webp"},
        {utf8_literal("https://c.tenor.com/AbCd-fGh_jKAAAAC/tenor.gif"), CdnHost::catalog_tenor, "/AbCd-fGh_jKAAAA1/tenor.webp"},
        {utf8_literal("https://media1.tenor.com/m/AbCd-fGh_jKAAAAd/cat.gif"), CdnHost::catalog_tenor, "/AbCd-fGh_jKAAAA1/cat.webp"},
        {utf8_literal("https://images-ext-1.discordapp.net/external/tok_en-1/https/media.tenor.com/AbCd-fGh_jKAAAPo/funny-cat.mp4"),
         CdnHost::catalog_tenor, "/AbCd-fGh_jKAAAA1/funny-cat.webp"},
        {utf8_literal("https://media.giphy.com/media/3o7TKSjRrfIPjeiVyM/giphy.mp4"), CdnHost::catalog_giphy, "/media/3o7TKSjRrfIPjeiVyM/200w.webp"},
        {utf8_literal("https://media2.giphy.com/media/v1.Y2lkPTc5MGI3NjEx/3o7TKSjRrfIPjeiVyM/giphy.gif?cid=1&rid=giphy.gif"),
         CdnHost::catalog_giphy, "/media/3o7TKSjRrfIPjeiVyM/200w.webp"},
        {utf8_literal("https://i.giphy.com/3o7TKSjRrfIPjeiVyM.webp"), CdnHost::catalog_giphy, "/media/3o7TKSjRrfIPjeiVyM/200w.webp"},
    };
    bool providers = true;
    for (const auto& favourite : favourites) {
        const auto mapped = parse_catalog_preview_url(favourite.src, {target, sizeof(target)});
        const u32 length = c_string_length(favourite.target);
        u8 head[kCdnMaximumRequestHeadBytes]{};
        const auto fetch = mapped.ok() ? write_static_cdn_get_request(CdnImageType::webp, mapped.host,
                                                                             {target, mapped.target_bytes}, {}, {head, sizeof(head)})
                                     : CdnRequestResult{};
        providers = providers && mapped.ok() && mapped.host == favourite.host && mapped.target_bytes == length &&
                    bytes_equal(target, favourite.target, length) && fetch.ok();
    }
    check(providers, "catalog_media_tenor_and_giphy_favourites_in_every_rendition_map_to_tiny_animated_webp");
    const auto attachment = parse_catalog_preview_url(
        utf8_literal("https://cdn.discordapp.com/attachments/1/2/dance.gif?ex=1&is=2&hm=ab&format=webp&animated=true"),
        {target, sizeof(target)}, MediaAttachmentFormat::webp, 498, 280);
    const ConstSlice<u8> written{target, attachment.target_bytes};
    const auto contains = [&](const char* text) noexcept {
        const u32 length = c_string_length(text);
        for (u32 i = 0; i + length <= written.count; ++i)
            if (bytes_equal(written.data + i, text, length)) return true;
        return false;
    };
    check(attachment.ok() && attachment.host == CdnHost::media && bytes_equal(target, "/attachments/1/2/dance.gif?", 27) &&
              contains("hm=ab") && contains("width=160&height=89") && contains("format=webp") && contains("animated=true"),
          "catalog_media_attachment_gif_favourite_uses_media_proxy_animated_webp");
    constexpr Utf8View rejected_favourites[]{
        utf8_literal("https://media.tenor.com/short/cat.mp4"),
        utf8_literal("https://media.tenor.com/AbCd-fGh_jKBBBPo/cat.mp4"),
        utf8_literal("https://media.tenor.com.evil/AbCd-fGh_jKAAAPo/cat.mp4"),
        utf8_literal("https://media.tenor.com:8443/AbCd-fGh_jKAAAPo/cat.mp4"),
        utf8_literal("https://media.tenor.com/AbCd-fGh_jKAAAPo/extra/cat.mp4"),
        utf8_literal("https://media.tenor.com/AbCd-fGh_jKAAAPo/ca%2et.mp4"),
        utf8_literal("https://media.giphy.com/media/bad-id!/giphy.gif"),
        utf8_literal("https://media9.giphy.com/media/3o7TKSjRrfIPjeiVyM/giphy.gif"),
        utf8_literal("https://giphy.com/gifs/3o7TKSjRrfIPjeiVyM"),
        utf8_literal("https://cdn.discordapp.com/emojis/42.gif"),
        utf8_literal("http://media.tenor.com/AbCd-fGh_jKAAAPo/cat.mp4"),
    };
    bool favourites_fenced = true;
    for (const auto url : rejected_favourites)
        favourites_fenced = favourites_fenced && !parse_catalog_preview_url(url, {target, sizeof(target)}).ok();
    // Every stored FavoriteGIF src shape: the Klipy picker's gif_src (.gif,
    // IMAGE), Linux tinywebp (.webp), a VIDEO mp4 without gif_src (played as a
    // video tile, like the official picker's <video>), gifv embed media behind
    // the signed proxy (Klipy thumbnails, any external image or MP4),
    // and CDN attachments. Tenor/Giphy are covered above.
    struct Klipy { Utf8View src; MediaAttachmentFormat format; CdnHost host; const char* target; };
    const Klipy klipy[]{
        {utf8_literal("https://static.klipy.com/ii/4e7bea9f7a3371424e6d16ebab93db50/8b/4c/WqDpNsyW.gif"),
         MediaAttachmentFormat::gif, CdnHost::catalog_klipy,
         "/ii/4e7bea9f7a3371424e6d16ebab93db50/8b/4c/WqDpNsyW.gif"},
        {utf8_literal("https://static.klipy.com/ii/4e7bea9f7a3371424e6d16ebab93db50/8b/4c/WqDpNsyW.webp"),
         MediaAttachmentFormat::webp, CdnHost::catalog_klipy,
         "/ii/4e7bea9f7a3371424e6d16ebab93db50/8b/4c/WqDpNsyW.webp"},
        // The reported IMAGE favourite of a Klipy gifv sent in chat: the
        // official client stores the embed thumbnail (its gifSrc, a Klipy
        // animated WebP; real asset paths from Klipy's API documentation)
        // with ?animated=true, and ?format=webp&animated=true for a GIF.
        // Klipy serves the same bytes without the query.
        {utf8_literal("https://static.klipy.com/ii/935d7ab9d8c6202580a668421940ec81/14/af/eUbp2uNc.webp"
                      "?animated=true"),
         MediaAttachmentFormat::webp, CdnHost::catalog_klipy,
         "/ii/935d7ab9d8c6202580a668421940ec81/14/af/eUbp2uNc.webp"},
        {utf8_literal("https://static2.klipy.com/ii/935d7ab9d8c6202580a668421940ec81/14/af/Sp4pln3Z.webp"
                      "?animated=true"),
         MediaAttachmentFormat::webp, CdnHost::catalog_klipy2,
         "/ii/935d7ab9d8c6202580a668421940ec81/14/af/Sp4pln3Z.webp"},
        {utf8_literal("https://static.klipy.com/ii/935d7ab9d8c6202580a668421940ec81/14/af/8GCrVAB7.gif"
                      "?format=webp&animated=true"),
         MediaAttachmentFormat::gif, CdnHost::catalog_klipy,
         "/ii/935d7ab9d8c6202580a668421940ec81/14/af/8GCrVAB7.gif"},
        // The reported Klipy VIDEO favourite (https://klipy.com/gifs/cat-drool-1).
        {utf8_literal("https://static.klipy.com/ii/4493325008d34b7bf8cd6813cd5c1619/80/4e/BgpWPn9XyO5HDdwMMc.mp4"),
         MediaAttachmentFormat::mp4, CdnHost::catalog_klipy,
         "/ii/4493325008d34b7bf8cd6813cd5c1619/80/4e/BgpWPn9XyO5HDdwMMc.mp4"},
        {utf8_literal("https://images-ext-1.discordapp.net/external/Sig_n-1/https/static.klipy.com/ii/"
                      "4e7bea9f7a3371424e6d16ebab93db50/8b/4c/thumb.webp?animated=true"),
         MediaAttachmentFormat::webp, CdnHost::images_ext_1,
         "/external/Sig_n-1/https/static.klipy.com/ii/4e7bea9f7a3371424e6d16ebab93db50/8b/4c/thumb.webp"
         "?format=webp&animated=true&width=160&height=89"},
        {utf8_literal("https://images-ext-2.discordapp.net/external/Sig_n-2/https/static.klipy.com/ii/"
                      "4e7bea9f7a3371424e6d16ebab93db50/8b/4c/thumb.gif?format=webp&animated=true"),
         MediaAttachmentFormat::webp, CdnHost::images_ext_2,
         "/external/Sig_n-2/https/static.klipy.com/ii/4e7bea9f7a3371424e6d16ebab93db50/8b/4c/thumb.gif"
         "?format=webp&animated=true&width=160&height=89"},
        {utf8_literal("https://images-ext-1.discordapp.net/external/Sig_n-3/https/static.klipy.com/ii/"
                      "4e7bea9f7a3371424e6d16ebab93db50/8b/4c/clip.mp4?width=498"),
         MediaAttachmentFormat::mp4, CdnHost::images_ext_1,
         "/external/Sig_n-3/https/static.klipy.com/ii/4e7bea9f7a3371424e6d16ebab93db50/8b/4c/clip.mp4"},
        // The reported external animated WebP, favourited from its embed.
        {utf8_literal("https://images-ext-1.discordapp.net/external/Zm9vYmFyU2lnbmF0dXJl_-1/https/"
                      "gif.fxtwitter.com/tweet_video/HQASuXQa4AEbQle.webp?animated=true"),
         MediaAttachmentFormat::webp, CdnHost::images_ext_1,
         "/external/Zm9vYmFyU2lnbmF0dXJl_-1/https/gif.fxtwitter.com/tweet_video/HQASuXQa4AEbQle.webp"
         "?format=webp&animated=true&width=160&height=89"},
        {utf8_literal("https://media.discordapp.net/external/abc/https/example.com/dance.gif"),
         MediaAttachmentFormat::webp, CdnHost::media,
         "/external/abc/https/example.com/dance.gif?format=webp&animated=true&width=160&height=89"},
        {utf8_literal("https://images-ext-2.discordapp.net/external/abc/%3Fsize%3D1024/http/example.com:8080/a.png"),
         MediaAttachmentFormat::webp, CdnHost::images_ext_2,
         "/external/abc/%3Fsize%3D1024/http/example.com:8080/a.png?format=webp&animated=true&width=160&height=89"},
        {utf8_literal("https://images-ext-2.discordapp.net/external/c2ln/https/i.imgur.com/AbCd.MP4"),
         MediaAttachmentFormat::mp4, CdnHost::images_ext_2, "/external/c2ln/https/i.imgur.com/AbCd.MP4"},
        {utf8_literal("https://cdn.discordapp.com/attachments/1/2/clip.mp4?ex=1&is=2&hm=ab"),
         MediaAttachmentFormat::mp4, CdnHost::cdn, "/attachments/1/2/clip.mp4?ex=1&is=2&hm=ab"},
        {utf8_literal("https://media.discordapp.net/attachments/1/2/dance.gif?ex=1&is=2&hm=ab"),
         MediaAttachmentFormat::webp, CdnHost::media,
         "/attachments/1/2/dance.gif?ex=1&is=2&hm=ab&width=160&height=89&format=webp&animated=true"},
    };
    bool klipy_mapped = true;
    for (const auto& favourite : klipy) {
        const auto format = catalog_favorite_format(favourite.src);
        const auto mapped = parse_catalog_preview_url(favourite.src, {target, sizeof(target)}, format, 498, 280);
        const u32 length = c_string_length(favourite.target);
        const auto type = format == MediaAttachmentFormat::gif   ? CdnImageType::gif
                          : format == MediaAttachmentFormat::mp4 ? CdnImageType::mp4
                                                                 : CdnImageType::webp;
        u8 head[kCdnMaximumRequestHeadBytes]{};
        const auto fetch = mapped.ok() ? write_static_cdn_get_request(type, mapped.host,
                                                                             {target, mapped.target_bytes}, {}, {head, sizeof(head)})
                                     : CdnRequestResult{};
        const bool matched = format == favourite.format && mapped.ok() && mapped.host == favourite.host &&
                             mapped.target_bytes == length && bytes_equal(target, favourite.target, length) && fetch.ok();
        klipy_mapped = klipy_mapped && matched;
    }
    check(klipy_mapped, "catalog_media_every_stored_favourite_shape_maps_to_a_renderable_rendition");
    // Shapes without a decodable rendition keep the fallback tile: Klipy names
    // no image after a WebM (the former .webp sibling guess was wrong), and an
    // unsigned raw URL on an arbitrary host has no route (the media proxy
    // refuses unsigned paths, and the official client's CSP blocks the host).
    constexpr Utf8View unrenderable[]{
        utf8_literal("https://static.klipy.com/ii/4e7bea9f7a3371424e6d16ebab93db50/8b/4c/WqDpNsyW.webm"),
        utf8_literal("https://gif.fxtwitter.com/tweet_video/HQASuXQa4AEbQle.webp"),
        utf8_literal("https://example.com/clip.mp4"),
        utf8_literal("https://images-ext-1.discordapp.net/external/t/https/example.com/clip.webm"),
        utf8_literal("https://images-ext-1.discordapp.net/external/t/https/example.com/a/../b.gif"),
        utf8_literal("https://images-ext-1.discordapp.net/external/t/https/example.com/a/./b.gif"),
        utf8_literal("https://images-ext-1.discordapp.net/external/t/https/example.com/%2e%2e/b.gif"),
        utf8_literal("https://images-ext-1.discordapp.net/external/t/https/example.com/a%2Fb.gif"),
        utf8_literal("https://images-ext-1.discordapp.net/external/t/https/example.com/a%5cb.mp4"),
        utf8_literal("https://images-ext-1.discordapp.net/external/t/https/example.com//b.gif"),
        utf8_literal("https://images-ext-1.discordapp.net/external/t/https/example.com/"),
        utf8_literal("https://images-ext-1.discordapp.net/external/t/https/example.com"),
        utf8_literal("https://images-ext-1.discordapp.net/external/t/https//b.gif"),
        utf8_literal("https://images-ext-1.discordapp.net/external/t!/https/example.com/b.gif"),
        utf8_literal("https://images-ext-1.discordapp.net/external//https/example.com/b.gif"),
        utf8_literal("https://images-ext-1.discordapp.net/external/t/ftp/example.com/b.gif"),
        utf8_literal("https://images-ext-1.discordapp.net/external/t/https/exa_mple.com/b.gif"),
        utf8_literal("https://images-ext-1.discordapp.net/external/t/https/example.com/b.gif#x"),
        utf8_literal("https://images-ext-1.discordapp.net/attachments/1/2/b.gif"),
        utf8_literal("https://images-ext-1.discordapp.net.evil/external/t/https/example.com/b.mp4"),
        utf8_literal("http://images-ext-1.discordapp.net/external/t/https/example.com/b.gif"),
        utf8_literal("https://cdn.discordapp.com/external/t/https/example.com/b.mp4"),
        utf8_literal("https://cdn.discordapp.com/attachments/1/2/clip.webm?ex=1&is=2&hm=ab"),
        utf8_literal("https://static.klipy.com/ii/4e7bea9f7a3371424e6d16ebab93db50/8b/4c/a.mp4?width=1"),
    };
    bool unrenderable_fenced = true;
    for (const auto url : unrenderable) {
        const auto format = catalog_favorite_format(url);
        const bool refused = format != MediaAttachmentFormat::mp4 &&
                             !parse_catalog_preview_url(url, {target, sizeof(target)}, format).ok() &&
                             !parse_catalog_preview_url(url, {target, sizeof(target)},
                                                        MediaAttachmentFormat::mp4).ok();
        unrenderable_fenced = unrenderable_fenced && refused;
    }
    check(unrenderable_fenced, "catalog_media_unsigned_hosts_undecodable_video_and_noncanonical_proxy_paths_refuse");
    // Tenor/Giphy videos keep their provider WebP; the MP4 path refuses them
    // even behind the proxy, and the WebP path refuses video behind it.
    check(catalog_favorite_format(utf8_literal("https://media.tenor.com/AbCd-fGh_jKAAAPo/funny-cat.mp4")) ==
                  MediaAttachmentFormat::webp &&
              !parse_catalog_preview_url(
                   utf8_literal("https://images-ext-1.discordapp.net/external/t/https/media.tenor.com/AbCd-fGh_jKAAAPo/c.mp4"),
                   {target, sizeof(target)}, MediaAttachmentFormat::mp4)
                   .ok() &&
              !parse_catalog_preview_url(
                   utf8_literal("https://images-ext-1.discordapp.net/external/t/https/example.com/clip.mp4"),
                   {target, sizeof(target)}, MediaAttachmentFormat::webp)
                   .ok() &&
              !parse_catalog_preview_url(klipy[1].src, {target, sizeof(target)}, MediaAttachmentFormat::mp4).ok(),
          "catalog_media_video_and_image_renditions_never_cross_formats");
    const auto big = parse_catalog_preview_url(klipy[6].src, {target, sizeof(target)},
                                               MediaAttachmentFormat::webp, 0xFFFFFFFFu, 3);
    check(big.ok() && big.target_bytes > 19 && bytes_equal(target + big.target_bytes - 19, "&width=160&height=1", 19),
          "catalog_media_proxy_preview_box_is_overflow_safe_and_aspect_preserving");
    const u8 klipy_mp4[] = "/ii/4493325008d34b7bf8cd6813cd5c1619/80/4e/BgpWPn9XyO5HDdwMMc.mp4";
    const ConstSlice<u8> klipy_video{klipy_mp4, sizeof(klipy_mp4) - 1};
    const ConstSlice<u8> tenor_webp{reinterpret_cast<const u8*>("/AbCd-fGh_jKAAAA1/funny-cat.webp"), 32};
    check(catalog_provider_target_valid(CdnHost::catalog_klipy, klipy_video) &&
              catalog_provider_request_valid(CdnHost::catalog_klipy, CdnImageType::mp4,
                                             klipy_video) &&
              !catalog_provider_request_valid(CdnHost::catalog_klipy, CdnImageType::webp,
                                              klipy_video) &&
              !catalog_provider_request_valid(CdnHost::catalog_klipy, CdnImageType::gif,
                                              klipy_video) &&
              !catalog_provider_request_valid(CdnHost::catalog_tenor, CdnImageType::mp4,
                                              tenor_webp) &&
              catalog_provider_request_valid(CdnHost::catalog_tenor, CdnImageType::webp,
                                             tenor_webp),
          "catalog_media_provider_requests_agree_with_asset_type_and_only_klipy_serves_mp4");
    u8 gif_target[kCdnMaximumTargetBytes]{};
    const auto klipy_gif = parse_catalog_preview_url(klipy[0].src, {gif_target, sizeof(gif_target)},
                                                     MediaAttachmentFormat::gif);
    u8 refused_head[kCdnMaximumRequestHeadBytes]{};
    constexpr Utf8View klipy_rejected[]{
        utf8_literal("https://static.klipy.com/ii/4e7bea9f7a3371424e6d16ebab93db50/8b/4c/a.b.gif"),
        utf8_literal("https://static.klipy.com/ii//8b/4c/x.gif"),
        utf8_literal("https://static.klipy.com/ii/x.gif?width=64"),
        utf8_literal("https://static.klipy.com.evil/ii/x.gif"),
        utf8_literal("https://media.klipy.com/ii/x.gif"),
        utf8_literal("https://klipy.com/gifs/hello-world"),
    };
    bool klipy_fenced = klipy_gif.ok() &&
        !write_static_cdn_get_request(CdnImageType::webp, klipy_gif.host,
                                      {gif_target, klipy_gif.target_bytes}, {},
                                      {refused_head, sizeof(refused_head)}).ok() &&
        !parse_catalog_preview_url(klipy[0].src, {target, sizeof(target)}).ok() &&
        catalog_favorite_format(klipy[3].src) == MediaAttachmentFormat::webp &&
        catalog_favorite_format(utf8_literal("https://media.tenor.com/AbCd-fGh_jKAAAAC/cat.gif")) ==
            MediaAttachmentFormat::webp;
    for (const auto url : klipy_rejected)
        klipy_fenced = klipy_fenced && !parse_catalog_preview_url(url, {target, sizeof(target)}).ok() &&
                       !parse_catalog_preview_url(url, {target, sizeof(target)}, MediaAttachmentFormat::gif).ok();
    check(klipy_fenced, "catalog_media_klipy_favourites_keep_type_extension_agreement_and_reject_other_shapes");
    check(favourites_fenced &&
              !catalog_provider_target_valid(CdnHost::catalog_tenor,
                                             {reinterpret_cast<const u8*>("/AbCd-fGh_jKAAAPo/cat.mp4"), 25}) &&
              !catalog_provider_target_valid(CdnHost::catalog_giphy,
                                             {reinterpret_cast<const u8*>("/media/x/giphy.gif"), 18}) &&
              !catalog_provider_target_valid(CdnHost::media, {target, 1}),
          "catalog_media_provider_favourites_reject_other_hosts_shapes_and_renditions");

    catalog_video_lane_case(check);
    chat_gif_embed_case(check);
    tile_fallback_log_case(check);
    klipy_video_favourite_case(check);

    AnimatedWebpFixtureResult webp{};
    const bool webp_built =
        build_lossless_animated_webp_fixture({catalog_encoded, sizeof(catalog_encoded)}, &webp);
    check(webp_built, "catalog_media_deterministic_webp_fixture");
    if (webp_built) {
        catalog_decode_case(check, MediaAttachmentFormat::webp, true, false, webp.encoded_bytes);
        catalog_worker_case(check, webp.encoded_bytes);
    }
    ApngFixtureResult apng{};
    const bool apng_built = build_rgba8_apng_fixture({catalog_encoded, sizeof(catalog_encoded)},
                                                     {catalog_scratch, sizeof(catalog_scratch)}, &apng);
    check(apng_built, "catalog_media_deterministic_apng_fixture");
    if (apng_built) {
        catalog_decode_case(check, MediaAttachmentFormat::png, true, false, apng.encoded_bytes);
        catalog_decode_case(check, MediaAttachmentFormat::png, false, true, apng.encoded_bytes);
    }
    GifFixtureResult gif{};
    const bool gif_built = build_indexed_gif_fixture({catalog_encoded, sizeof(catalog_encoded)},
                                                     {catalog_scratch, sizeof(catalog_scratch)}, &gif);
    check(gif_built, "catalog_media_deterministic_gif_fixture");
    if (gif_built)
        catalog_decode_case(check, MediaAttachmentFormat::gif, true, false, gif.encoded_bytes);
    PngFixtureResult png{};
    const bool png_built = build_rgba8_png_fixture(4, 4, {catalog_encoded, sizeof(catalog_encoded)},
                                                   {catalog_scratch, sizeof(catalog_scratch)}, &png);
    check(png_built, "catalog_media_deterministic_png_fixture");
    if (png_built)
        catalog_decode_case(check, MediaAttachmentFormat::png, false, false, png.encoded_bytes);
}

} // namespace maicord
