#include "../../support/test_main_shared.hpp"

#include "../../support/message_media_controller_fixture.hpp"
#include "maicord/app/media/animation_visual_runtime.hpp"
#include "maicord/app/media/account_media_worker.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/media/core/presentation_policy.hpp"
#include "maicord/render/media/animation_frame_cache.hpp"
#include "maicord/render/d3d11/presentation_damage.hpp"
#include "maicord/ui/media/avatar_media.hpp"
#include "../../support/media_fixture.hpp"
#include "../../fixtures/jpeg_progressive_fixture.hpp"
#include "../../fixtures/webp_fixture.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/time.hpp"

namespace maicord {
namespace {

// Value-returning forms of the pipeline's in-place operations; production fills caller-owned results.
#define CDN_TEST_VALUE_FORMS(Result, kind)                                                                     \
    Result cdn_media_pipeline_lookup_##kind(CdnMediaPipeline* pipeline, const MediaAssetKey& key,             \
                                            u64 now_seconds) noexcept {                                       \
        Result result;                                                                                         \
        cdn_media_pipeline_lookup_##kind(&result, pipeline, key, now_seconds);                                 \
        return result;                                                                                         \
    }                                                                                                          \
    Result cdn_media_pipeline_admit_##kind(CdnMediaPipeline* pipeline, const MediaAssetKey& key,              \
                                           const CdnResponseResult& response, ConstSlice<u8> decoded_body,    \
                                           ConstSlice<u8> conditional_etag = {}) noexcept {                   \
        Result result;                                                                                         \
        cdn_media_pipeline_admit_##kind(&result, pipeline, key, response, decoded_body, conditional_etag);     \
        return result;                                                                                         \
    }

#define CDN_TEST_VALUE_FRAME(Result, name)                                                                     \
    Result cdn_media_pipeline_decode_##name(CdnMediaPipeline* pipeline, const MediaAssetKey& key,             \
                                            u16 cache_generation, u32 frame_index, u64 now_seconds) noexcept { \
        Result result;                                                                                         \
        cdn_media_pipeline_decode_##name(&result, pipeline, key, cache_generation, frame_index, now_seconds);  \
        return result;                                                                                         \
    }

CDN_TEST_VALUE_FORMS(CdnStaticWebpResult, static_webp)
CDN_TEST_VALUE_FORMS(CdnStaticJpegResult, static_jpeg)
CDN_TEST_VALUE_FORMS(CdnApngResult, apng)
CDN_TEST_VALUE_FORMS(CdnGifResult, gif)
CDN_TEST_VALUE_FORMS(CdnAnimatedWebpResult, animated_webp)
CDN_TEST_VALUE_FRAME(CdnApngResult, apng_frame)
CDN_TEST_VALUE_FRAME(CdnGifResult, gif_frame)
CDN_TEST_VALUE_FRAME(CdnAnimatedWebpResult, animated_webp_frame)
#undef CDN_TEST_VALUE_FRAME
#undef CDN_TEST_VALUE_FORMS

} // namespace

void test_cdn_targets() noexcept {
    u8 output[128]{};
    constexpr Utf8View guild_icon_hash = utf8_literal("0123456789abcdef0123456789abcdef");
    const CdnTargetResult guild_icon =
        write_guild_icon_cdn_target(123, guild_icon_hash, 64, Slice<u8>{output, sizeof(output)});
    expect(guild_icon.ok() && guild_icon.host == CdnHost::cdn &&
               cdn_guild_icon_variant(guild_icon_hash) != 0 &&
               utf8_equal(Utf8View{reinterpret_cast<const char*>(output), guild_icon.bytes},
                          utf8_literal("/icons/123/0123456789abcdef0123456789abcdef.webp?size=64")),
           "cdn_guild_icon_webp_target");
    expect(!guild_icon_hash_valid(utf8_literal("../../invalid")) &&
               write_guild_icon_cdn_target(123, utf8_literal("../../invalid"), 64,
                                           Slice<u8>{output, sizeof(output)})
                       .error == CdnTargetError::invalid_input,
           "cdn_guild_icon_hash_rejects_path_material");
    const CdnTargetResult avatar =
        write_user_avatar_cdn_target(456, guild_icon_hash, 32, Slice<u8>{output, sizeof(output)});
    expect(avatar.ok() && avatar.host == CdnHost::cdn &&
               utf8_equal(Utf8View{reinterpret_cast<const char*>(output), avatar.bytes},
                          utf8_literal("/avatars/456/0123456789abcdef0123456789abcdef.webp?size=32")),
           "cdn_user_avatar_webp_target");
    constexpr Utf8View animated_hash = utf8_literal("a_0123456789abcdef0123456789abcdef");
    const CdnTargetResult animated_avatar =
        write_user_avatar_cdn_target(456, animated_hash, 128, Slice<u8>{output, sizeof(output)});
    expect(animated_avatar.ok() &&
               utf8_equal(
                   Utf8View{reinterpret_cast<const char*>(output), animated_avatar.bytes},
                   utf8_literal(
                       "/avatars/456/a_0123456789abcdef0123456789abcdef.webp?size=128&animated=true")),
           "cdn_animated_avatar_requests_animated_webp");
    const CdnTargetResult animated_guild_icon =
        write_guild_icon_cdn_target(123, animated_hash, 128, Slice<u8>{output, sizeof(output)});
    expect(
        animated_guild_icon.ok() &&
            utf8_equal(
                Utf8View{reinterpret_cast<const char*>(output), animated_guild_icon.bytes},
                utf8_literal("/icons/123/a_0123456789abcdef0123456789abcdef.webp?size=128&animated=true")),
        "cdn_animated_guild_icon_requests_animated_webp");
    // A guild icon request that may not animate: the same path is the CDN's still.
    const CdnTargetResult still_guild_icon =
        write_guild_icon_cdn_target(123, animated_hash, 128, Slice<u8>{output, sizeof(output)}, false);
    expect(still_guild_icon.ok() &&
               utf8_equal(Utf8View{reinterpret_cast<const char*>(output), still_guild_icon.bytes},
                          utf8_literal("/icons/123/a_0123456789abcdef0123456789abcdef.webp?size=128")),
           "cdn_animated_guild_icon_still_omits_animated");
    const CdnTargetResult channel_icon =
        write_channel_icon_cdn_target(789, guild_icon_hash, 64, Slice<u8>{output, sizeof(output)});
    expect(channel_icon.ok() && channel_icon.host == CdnHost::cdn &&
               utf8_equal(Utf8View{reinterpret_cast<const char*>(output), channel_icon.bytes},
                          utf8_literal("/channel-icons/789/0123456789abcdef0123456789abcdef.webp?size=64")) &&
               write_channel_icon_cdn_target(789, utf8_literal("../x"), 64, Slice<u8>{output, sizeof(output)})
                       .error == CdnTargetError::invalid_input &&
               write_channel_icon_cdn_target(789, guild_icon_hash, 48, Slice<u8>{output, sizeof(output)})
                       .error == CdnTargetError::invalid_input &&
               write_channel_icon_cdn_target(0, guild_icon_hash, 64, Slice<u8>{output, sizeof(output)})
                       .error == CdnTargetError::invalid_input,
           "cdn_group_dm_channel_icon_target_and_validation");
    const MediaAssetKey channel_icon_key = media_asset_channel_icon(789, user_avatar_variant(guild_icon_hash), false);
    expect(media_asset_key_valid(channel_icon_key) &&
               !media_asset_key_valid(media_asset_channel_icon(789, 0, false)) &&
               !media_asset_key_equal(channel_icon_key,
                                      media_asset_user_avatar(789, user_avatar_variant(guild_icon_hash), false)),
           "media_asset_channel_icon_identity");
    // Shared avatar/icon identities: the CDN size covers the 40-DIP display
    // extent at the DPI, as a power of two, never above the 128px default.
    expect(cdn_image_size(kCdnIdentityDisplayDips, 96) == 64 &&
               cdn_image_size(kCdnIdentityDisplayDips, 144) == 64 &&
               cdn_image_size(kCdnIdentityDisplayDips, 168) == 128 &&
               cdn_image_size(kCdnIdentityDisplayDips, 288) == 128 &&
               cdn_image_size(16, 96) == 16 && cdn_image_size(24, 96) == 32 &&
               cdn_image_size(32, 96) == 32 && cdn_image_size(33, 96) == 64 &&
               cdn_image_size(40, 0) == 64 && cdn_image_size(300, 96) == 128 &&
               cdn_image_size(1, 96) == 16,
           "cdn_identity_size_matches_display_extent_times_dpi");
    const CdnTargetResult emoji =
        write_custom_emoji_cdn_target(44, true, 128, Slice<u8>{output, sizeof(output)});
    expect(emoji.ok() && emoji.host == CdnHost::cdn &&
               utf8_equal(Utf8View{reinterpret_cast<const char*>(output), emoji.bytes},
                          utf8_literal("/emojis/44.webp?size=128&animated=true")) &&
               utf8_equal(cdn_host_name(emoji.host), utf8_literal("cdn.discordapp.com")),
           "cdn_animated_emoji_webp_target");

    const CdnTargetResult static_emoji =
        write_custom_emoji_cdn_target(45, false, 16, Slice<u8>{output, sizeof(output)});
    expect(static_emoji.ok() &&
               utf8_equal(Utf8View{reinterpret_cast<const char*>(output), static_emoji.bytes},
                          utf8_literal("/emojis/45.webp?size=16")),
           "cdn_static_emoji_webp_target");

    const CdnTargetResult png =
        write_sticker_cdn_target(88, 2, Slice<u8>{output, sizeof(output)});
    const bool png_target_ok = png.ok() && png.host == CdnHost::cdn &&
                               utf8_equal(Utf8View{reinterpret_cast<const char*>(output), png.bytes},
                                          utf8_literal("/stickers/88.png"));
    const CdnTargetResult lottie =
        write_sticker_cdn_target(89, 3, Slice<u8>{output, sizeof(output)});
    expect(png_target_ok && lottie.ok() && lottie.host == CdnHost::cdn &&
               utf8_equal(Utf8View{reinterpret_cast<const char*>(output), lottie.bytes},
                          utf8_literal("/stickers/89.json")),
           "cdn_png_apng_lottie_targets");
    const CdnTargetResult gif =
        write_sticker_cdn_target(90, 4, Slice<u8>{output, sizeof(output)});
    expect(gif.ok() && gif.host == CdnHost::media &&
               utf8_equal(cdn_host_name(gif.host), utf8_literal("media.discordapp.net")) &&
               utf8_equal(Utf8View{reinterpret_cast<const char*>(output), gif.bytes},
                          utf8_literal("/stickers/90.gif")),
           "cdn_gif_sticker_media_host");

    u8 short_output[8]{};
    guard_fill(short_output, sizeof(short_output));
    const CdnTargetResult short_result =
        write_custom_emoji_cdn_target(1, false, 32, Slice<u8>{short_output, sizeof(short_output)});
    const bool short_unchanged = guard_intact(short_output, sizeof(short_output));
    expect(
        short_result.error == CdnTargetError::output_capacity && short_unchanged &&
            write_custom_emoji_cdn_target(1, false, 24, Slice<u8>{output, sizeof(output)}).error ==
                CdnTargetError::invalid_input &&
            write_custom_emoji_cdn_target(1, false, 8, Slice<u8>{output, sizeof(output)}).error ==
                CdnTargetError::invalid_input &&
            write_sticker_cdn_target(1, 5, Slice<u8>{output, sizeof(output)}).error ==
                CdnTargetError::unsupported_format &&
            write_sticker_cdn_target(0, 1, Slice<u8>{output, sizeof(output)}).error ==
                CdnTargetError::invalid_input,
        "cdn_target_bounds_and_formats");

    u8 proxy_target[256]{};
    constexpr Utf8View proxy_url =
        utf8_literal("https://media.discordapp.net/external/fixture/image.png?width=640&height=360");
    const MediaUrlResult proxy =
        parse_media_url(proxy_url, Slice<u8>{proxy_target, sizeof(proxy_target)});
    u8 images_ext_parse_target[256]{};
    const MediaUrlResult images_ext_proxy =
        parse_media_url(utf8_literal("https://images-ext-1.discordapp.net/external/token/https/"
                                     "example.com/image"),
                        Slice<u8>{images_ext_parse_target, sizeof(images_ext_parse_target)});
    u8 images_ext_target[256]{};
    const CdnTargetResult images_ext_bounded =
        images_ext_proxy.ok() ? write_media_proxy_target(
                                    ConstSlice<u8>{images_ext_parse_target, images_ext_proxy.target_bytes},
                                    images_ext_proxy.host, 320, 180, CdnMediaFormat::png,
                                    Slice<u8>{images_ext_target, sizeof(images_ext_target)})
                              : CdnTargetResult{};
    expect(proxy.ok() && proxy.host == CdnHost::media &&
               bytes_equal(ConstSlice<u8>{proxy_target, proxy.target_bytes},
                           byte_literal("/external/fixture/image.png?width=640&height=360")) &&
               parse_media_url(utf8_literal("https://cdn.discordapp.com/embed/avatars/0.png"),
                               Slice<u8>{proxy_target, sizeof(proxy_target)})
                       .host == CdnHost::cdn,
           "cdn_embed_proxy_exact_target");
    expect(images_ext_proxy.ok() && images_ext_proxy.host == CdnHost::images_ext_1 &&
               images_ext_bounded.ok() && images_ext_bounded.host == CdnHost::media &&
               bytes_equal(ConstSlice<u8>{images_ext_target, images_ext_bounded.bytes},
                           byte_literal("/external/token/https/example.com/image"
                                        "?width=320&height=180&format=png")) &&
               parse_media_url(utf8_literal("https://images-ext-2.discordapp.net/external/a"),
                               Slice<u8>{proxy_target, sizeof(proxy_target)})
                       .host == CdnHost::images_ext_2,
           "cdn_images_ext_proxy_is_allowlisted_and_bounded");
    expect(parse_media_url(utf8_literal("https://media.discordapp.net.evil.test/external/a.png"),
                           Slice<u8>{proxy_target, sizeof(proxy_target)})
                       .error == MediaUrlError::invalid_host &&
               parse_media_url(utf8_literal("http://media.discordapp.net/external/a.png"),
                               Slice<u8>{proxy_target, sizeof(proxy_target)})
                       .error == MediaUrlError::invalid_scheme &&
               parse_media_url(utf8_literal("https://media.discordapp.net/external/a%2.png"),
                               Slice<u8>{proxy_target, sizeof(proxy_target)})
                       .error == MediaUrlError::invalid_target,
           "cdn_embed_proxy_origin_and_encoding_reject");

    u8 attachment_target[kCdnMaximumTargetBytes]{};
    constexpr auto signed_attachment =
        utf8_literal("https://cdn.discordapp.com/attachments/123/456/image%20one.jpg"
                     "?ex=66d3ef00&is=66d3ee00&hm="
                     "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef&");
    const AttachmentUrlResult attachment = parse_signed_attachment_url(
        signed_attachment, 456, Slice<u8>{attachment_target, sizeof(attachment_target)});
    expect(
        attachment.ok() && attachment.host == CdnHost::cdn && !attachment.ephemeral &&
            attachment.expires_at_seconds == 0x66d3ef00ull &&
            bytes_equal(ConstSlice<u8>{attachment_target, attachment.target_bytes},
                        byte_literal("/attachments/123/456/image%20one.jpg"
                                     "?ex=66d3ef00&is=66d3ee00&hm="
                                     "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef&")),
        "cdn_signed_attachment_exact_target");
    constexpr auto supplied_media_attachment =
        utf8_literal("https://media.discordapp.net/attachments/1212343456567878909/"
                     "1414213562373095048/IntroPicture.png"
                     "?ex=6f000000&is=6e000000&hm="
                     "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5"
                     "&=&format=webp&quality=lossless");
    const AttachmentUrlResult supplied_media =
        parse_signed_attachment_url(supplied_media_attachment, 1'414'213'562'373'095'048ull,
                                    Slice<u8>{attachment_target, sizeof(attachment_target)});
    u8 bounded_target[kCdnMaximumTargetBytes]{};
    const CdnTargetResult bounded =
        supplied_media.ok()
            ? write_media_proxy_target(
                  ConstSlice<u8>{attachment_target, supplied_media.target_bytes}, supplied_media.host, 512,
                  375, CdnMediaFormat::png, Slice<u8>{bounded_target, sizeof(bounded_target)})
            : CdnTargetResult{};
    expect(supplied_media.ok() && supplied_media.host == CdnHost::media && bounded.ok() &&
               bounded.host == CdnHost::media &&
               bytes_equal(ConstSlice<u8>{bounded_target, bounded.bytes},
                           byte_literal("/attachments/1212343456567878909/"
                                        "1414213562373095048/IntroPicture.png"
                                        "?ex=6f000000&is=6e000000&hm="
                                        "a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5a5"
                                        "&width=512&height=375&format=png")),
           "cdn_supplied_media_attachment_is_canonicalized_and_bounded");
    constexpr auto avatar_media = utf8_literal("https://cdn.discordapp.com/avatars/400000000000000042/"
                                               "fedcba9876543210fedcba9876543210.png?size=1024");
    const MediaUrlResult avatar_parsed =
        parse_media_url(avatar_media, Slice<u8>{attachment_target, sizeof(attachment_target)});
    const CdnTargetResult avatar_bounded =
        avatar_parsed.ok()
            ? write_media_proxy_target(
                  ConstSlice<u8>{attachment_target, avatar_parsed.target_bytes}, avatar_parsed.host, 512,
                  512, CdnMediaFormat::png, Slice<u8>{bounded_target, sizeof(bounded_target)})
            : CdnTargetResult{};
    expect(avatar_bounded.ok() && avatar_bounded.host == CdnHost::cdn &&
               bytes_equal(ConstSlice<u8>{bounded_target, avatar_bounded.bytes},
                           byte_literal("/avatars/400000000000000042/"
                                        "fedcba9876543210fedcba9876543210.png?size=512")),
           "cdn_avatar_uses_native_size_without_128px_fallback");
    constexpr auto unsigned_attachment =
        utf8_literal("https://cdn.discordapp.com/attachments/1618033988749894848/"
                     "1732050807568877293/picture.png");
    const AttachmentUrlResult unsigned_parsed =
        parse_signed_attachment_url(unsigned_attachment, 1'732'050'807'568'877'293ull,
                                    Slice<u8>{attachment_target, sizeof(attachment_target)});
    expect(unsigned_parsed.ok() && unsigned_parsed.expires_at_seconds == 0 &&
               bytes_equal(ConstSlice<u8>{attachment_target, unsigned_parsed.target_bytes},
                           byte_literal("/attachments/1618033988749894848/"
                                        "1732050807568877293/picture.png")),
           "cdn_unsigned_attachment_is_validated_refresh_identity");
    u8 short_attachment_target[16]{};
    guard_fill(short_attachment_target, sizeof(short_attachment_target));
    const AttachmentUrlResult attachment_short = parse_signed_attachment_url(
        signed_attachment, 456, Slice<u8>{short_attachment_target, sizeof(short_attachment_target)});
    const bool attachment_short_unchanged =
        guard_intact(short_attachment_target, sizeof(short_attachment_target));
    expect(attachment_short.error == AttachmentUrlError::output_capacity &&
               attachment_short_unchanged,
           "cdn_signed_attachment_transactional_capacity");

    constexpr auto ephemeral_attachment =
        utf8_literal("https://cdn.discordapp.com/ephemeral-attachments/123/456/a.jpg"
                     "?hm=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
                     "&is=1&ex=2");
    const AttachmentUrlResult ephemeral = parse_signed_attachment_url(
        ephemeral_attachment, 456, Slice<u8>{attachment_target, sizeof(attachment_target)});
    expect(ephemeral.ok() && ephemeral.ephemeral && ephemeral.expires_at_seconds == 2,
           "cdn_signed_ephemeral_attachment");

    expect(
        parse_signed_attachment_url(
            utf8_literal("https://cdn.discordapp.com.evil.test/attachments/123/456/a.jpg"
                         "?ex=2&is=1&hm=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
            456, Slice<u8>{attachment_target, sizeof(attachment_target)})
                    .error == AttachmentUrlError::invalid_host &&
            parse_signed_attachment_url(
                utf8_literal(
                    "https://cdn.discordapp.com/attachments/123/457/a.jpg"
                    "?ex=2&is=1&hm=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
                456, Slice<u8>{attachment_target, sizeof(attachment_target)})
                    .error == AttachmentUrlError::attachment_mismatch &&
            parse_signed_attachment_url(
                utf8_literal(
                    "https://cdn.discordapp.com/attachments/123/456/a%2.jpg"
                    "?ex=2&is=1&hm=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
                456, Slice<u8>{attachment_target, sizeof(attachment_target)})
                    .error == AttachmentUrlError::invalid_path,
        "cdn_signed_attachment_origin_identity_and_encoding");

    expect(parse_signed_attachment_url(
               utf8_literal(
                   "https://cdn.discordapp.com/attachments/123/456/a.jpg"
                   "?ex=2&ex=3&is=1&hm=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
               456, Slice<u8>{attachment_target, sizeof(attachment_target)})
                       .error == AttachmentUrlError::invalid_query &&
               parse_signed_attachment_url(
                   utf8_literal(
                       "https://cdn.discordapp.com/attachments/123/456/a.jpg"
                       "?ex=1&is=2&hm=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
                   456, Slice<u8>{attachment_target, sizeof(attachment_target)})
                       .error == AttachmentUrlError::invalid_timestamp &&
               parse_signed_attachment_url(
                   utf8_literal("https://cdn.discordapp.com/attachments/123/456/a.jpg"
                                "?ex=2&is=1&hm=aaaaaaaa"),
                   456, Slice<u8>{attachment_target, sizeof(attachment_target)})
                       .error == AttachmentUrlError::invalid_signature &&
               parse_signed_attachment_url(
                   utf8_literal(
                       "https://cdn.discordapp.com/attachments/123/456/a.jpg"
                       "?ex=2&is=1&hm=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa&&"),
                   456, Slice<u8>{attachment_target, sizeof(attachment_target)})
                       .error == AttachmentUrlError::invalid_query &&
               parse_signed_attachment_url(
                   utf8_literal(
                       "https://cdn.discordapp.com/attachments/0123/456/a.jpg"
                       "?ex=2&is=1&hm=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
                   456, Slice<u8>{attachment_target, sizeof(attachment_target)})
                       .error == AttachmentUrlError::invalid_path &&
               parse_signed_attachment_url(
                   utf8_literal(
                       "https://cdn.discordapp.com/attachments/123/0456/a.jpg"
                       "?ex=2&is=1&hm=aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
                   456, Slice<u8>{attachment_target, sizeof(attachment_target)})
                       .error == AttachmentUrlError::invalid_path,
           "cdn_signed_attachment_query_contract");
}

void test_cdn_http_policy() noexcept {
    u8 request[1024]{};
    const CdnRequestResult unconditional = write_static_cdn_get_request(CdnImageType::png, CdnHost::cdn, byte_literal("/stickers/88.png"), {}, Slice<u8>{request, sizeof(request)});
    expect(unconditional.ok() && bytes_equal(ConstSlice<u8>{request, unconditional.bytes},
                                             byte_literal("GET /stickers/88.png HTTP/1.1\r\n"
                                                          "Host: cdn.discordapp.com\r\n"
                                                          "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) discord/1.0.9267 Chrome/152.0.7977.169 Electron/44.10.0 Safari/537.36\r\n"
                                                          "Accept: image/png\r\n"
                                                          "Accept-Encoding: identity\r\n"
                                                          "Connection: keep-alive\r\n\r\n")),
           "cdn_credential_free_request_head");

    const CdnRequestResult conditional = write_static_cdn_get_request(CdnImageType::png, CdnHost::media, byte_literal("/stickers/90.png"), byte_literal("W/\"asset-1\""),
        Slice<u8>{request, sizeof(request)});
    expect(conditional.ok() && bytes_equal(ConstSlice<u8>{request, conditional.bytes},
                                           byte_literal("GET /stickers/90.png HTTP/1.1\r\n"
                                                        "Host: media.discordapp.net\r\n"
                                                        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) discord/1.0.9267 Chrome/152.0.7977.169 Electron/44.10.0 Safari/537.36\r\n"
                                                        "Accept: image/png\r\n"
                                                        "Accept-Encoding: identity\r\n"
                                                        "Connection: keep-alive\r\n"
                                                        "If-None-Match: W/\"asset-1\"\r\n\r\n")),
           "cdn_bounded_conditional_request");

    const CdnRequestResult webp_request = write_static_cdn_get_request(
        CdnImageType::webp, CdnHost::cdn, byte_literal("/emojis/45.webp?size=64"), {},
        Slice<u8>{request, sizeof(request)});
    expect(webp_request.ok() && bytes_equal(ConstSlice<u8>{request, webp_request.bytes},
                                            byte_literal("GET /emojis/45.webp?size=64 HTTP/1.1\r\n"
                                                         "Host: cdn.discordapp.com\r\n"
                                                         "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) discord/1.0.9267 Chrome/152.0.7977.169 Electron/44.10.0 Safari/537.36\r\n"
                                                         "Accept: image/webp\r\n"
                                                         "Accept-Encoding: identity\r\n"
                                                         "Connection: keep-alive\r\n\r\n")),
           "cdn_webp_credential_free_request_head");

    const CdnRequestResult gif_request = write_static_cdn_get_request(
        CdnImageType::gif, CdnHost::media, byte_literal("/stickers/90.gif"), {},
        Slice<u8>{request, sizeof(request)});
    expect(gif_request.ok() && bytes_equal(ConstSlice<u8>{request, gif_request.bytes},
                                           byte_literal("GET /stickers/90.gif HTTP/1.1\r\n"
                                                        "Host: media.discordapp.net\r\n"
                                                        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) discord/1.0.9267 Chrome/152.0.7977.169 Electron/44.10.0 Safari/537.36\r\n"
                                                        "Accept: image/gif\r\n"
                                                        "Accept-Encoding: identity\r\n"
                                                        "Connection: keep-alive\r\n\r\n")),
           "cdn_gif_credential_free_request_head");

    const CdnRequestResult jpeg_request = write_static_cdn_get_request(
        CdnImageType::jpeg, CdnHost::cdn,
        byte_literal("/attachments/123/456/a.jpg?ex=2&is=1&hm="
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        {}, Slice<u8>{request, sizeof(request)});
    expect(
        jpeg_request.ok() &&
            bytes_equal(
                ConstSlice<u8>{request, jpeg_request.bytes},
                byte_literal("GET /attachments/123/456/a.jpg?ex=2&is=1&hm="
                             "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa HTTP/1.1\r\n"
                             "Host: cdn.discordapp.com\r\n"
                             "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) discord/1.0.9267 Chrome/152.0.7977.169 Electron/44.10.0 Safari/537.36\r\n"
                             "Accept: image/jpeg\r\n"
                             "Accept-Encoding: identity\r\n"
                             "Connection: keep-alive\r\n\r\n")),
        "cdn_jpeg_credential_free_request_head");

    const CdnRequestResult mp4_request =
        write_static_cdn_get_request(
            CdnImageType::mp4, CdnHost::images_ext_1,
            byte_literal("/external/hash/https/media.tenor.com/clip.mp4"), {},
            Slice<u8>{request, sizeof(request)});
    expect(mp4_request.ok() &&
               bytes_equal(
                   ConstSlice<u8>{request, mp4_request.bytes},
                   byte_literal(
                       "GET /external/hash/https/media.tenor.com/clip.mp4 HTTP/1.1\r\n"
                       "Host: images-ext-1.discordapp.net\r\n"
                       "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) discord/1.0.9267 Chrome/152.0.7977.169 Electron/44.10.0 Safari/537.36\r\n"
                       "Accept: video/mp4\r\n"
                       "Accept-Encoding: identity\r\n"
                       "Connection: keep-alive\r\n\r\n")),
           "cdn_mp4_credential_free_request_head");

    u8 long_target[1'500]{};
    long_target[0] = '/';
    for (u32 index = 1; index < sizeof(long_target); ++index) {
        long_target[index] = 'a';
    }
    u8 long_request[kCdnMaximumRequestHeadBytes]{};
    const CdnRequestResult long_written = write_static_cdn_get_request(
        CdnImageType::jpeg, CdnHost::cdn, ConstSlice<u8>{long_target, sizeof(long_target)},
        {}, Slice<u8>{long_request, sizeof(long_request)});
    expect(long_written.ok() && long_written.bytes > sizeof(long_target) &&
               bytes_equal(ConstSlice<u8>{long_request, 5}, byte_literal("GET /")),
           "cdn_long_signed_target_request_capacity");

    u8 short_output[8]{};
    guard_fill(short_output, sizeof(short_output));
    const CdnRequestResult short_result =
        write_static_cdn_get_request(CdnImageType::png, CdnHost::cdn, byte_literal("/stickers/1.png"), {},
                                         Slice<u8>{short_output, sizeof(short_output)});
    const bool short_unchanged = guard_intact(short_output, sizeof(short_output));
    expect(short_result.error == CdnHttpError::output_capacity && short_unchanged &&
               write_static_cdn_get_request(CdnImageType::png, CdnHost::cdn,
                                                byte_literal("/stickers/1.png\r\nCookie:x"), {},
                                                Slice<u8>{request, sizeof(request)})
                       .error == CdnHttpError::invalid_target &&
               write_static_cdn_get_request(CdnImageType::png, CdnHost::cdn, byte_literal("/stickers/1.png"),
                   byte_literal("\"x\"\r\nAuthorization:x"), Slice<u8>{request, sizeof(request)})
                       .error == CdnHttpError::invalid_etag,
           "cdn_request_injection_and_transaction_bounds");

    u8 redirect_target[512]{};
    const CdnRedirectResult cross_origin = parse_cdn_redirect(
        CdnHost::cdn, byte_literal("https://media.discordapp.net/stickers/90.gif?size=160"),
        Slice<u8>{redirect_target, sizeof(redirect_target)});
    expect(cross_origin.ok() && cross_origin.host == CdnHost::media &&
               bytes_equal(ConstSlice<u8>{redirect_target, cross_origin.target_bytes},
                           byte_literal("/stickers/90.gif?size=160")) &&
               parse_cdn_redirect(CdnHost::cdn, byte_literal("/stickers/2.png"),
                                  Slice<u8>{redirect_target, sizeof(redirect_target)})
                   .ok(),
           "cdn_redirect_allowlisted_origins");
    expect(parse_cdn_redirect(CdnHost::cdn,
                              byte_literal("https://cdn.discordapp.com.evil.test/a.png"),
                              Slice<u8>{redirect_target, sizeof(redirect_target)})
                       .error == CdnHttpError::invalid_location &&
               parse_cdn_redirect(CdnHost::cdn,
                                  byte_literal("https://cdn.discordapp.com:443/a.png"),
                                  Slice<u8>{redirect_target, sizeof(redirect_target)})
                       .error == CdnHttpError::invalid_location &&
               parse_cdn_redirect(CdnHost::cdn,
                                  byte_literal("https://cdn.discordapp.com/a.png#fragment"),
                                  Slice<u8>{redirect_target, sizeof(redirect_target)})
                       .error == CdnHttpError::invalid_location,
           "cdn_redirect_authority_and_fragment_reject");

    const CdnResponseResult accepted =
        validate_static_image_response(CdnImageType::png, byte_literal("HTTP/1.1 200 OK\r\n"
                                                  "Content-Type: image/png\r\n"
                                                  "Content-Length: 8\r\n"
                                                  "ETag: \"asset-1\"\r\n"
                                                  "Cache-Control: public, max-age=120\r\n"
                                                  "Age: 20\r\n\r\n"),
                                     8, 1'000);
    expect(accepted.ok() && accepted.disposition == CdnResponseDisposition::accept_body &&
               accepted.store_allowed && !accepted.must_revalidate &&
               accepted.expires_at_seconds == 1'100 &&
               bytes_equal(accepted.etag, byte_literal("\"asset-1\"")),
           "cdn_png_mime_and_freshness_admission");

    const CdnResponseResult webp_accepted =
        validate_static_image_response(CdnImageType::webp, byte_literal("HTTP/1.1 200 OK\r\n"
                                                   "Content-Type: image/webp\r\n"
                                                   "Content-Length: 8\r\n"
                                                   "Cache-Control: max-age=30\r\n\r\n"),
                                      8, 1'000);
    expect(webp_accepted.ok() && webp_accepted.disposition == CdnResponseDisposition::accept_body &&
               webp_accepted.expires_at_seconds == 1'030 &&
               validate_static_image_response(CdnImageType::webp, byte_literal("HTTP/1.1 200 OK\r\n"
                                                          "Content-Type: image/png\r\n"
                                                          "Content-Length: 8\r\n\r\n"),
                                             8, 0)
                       .error == CdnHttpError::invalid_content_type,
           "cdn_webp_exact_mime_admission");

    const CdnResponseResult gif_accepted =
        validate_static_image_response(CdnImageType::gif, byte_literal("HTTP/1.1 200 OK\r\n"
                                                  "Content-Type: image/gif\r\n"
                                                  "Content-Length: 8\r\n"
                                                  "Cache-Control: max-age=30\r\n\r\n"),
                                     8, 1'000);
    expect(gif_accepted.ok() && gif_accepted.disposition == CdnResponseDisposition::accept_body &&
               gif_accepted.expires_at_seconds == 1'030 &&
               validate_static_image_response(CdnImageType::gif, byte_literal("HTTP/1.1 200 OK\r\n"
                                                         "Content-Type: image/png\r\n"
                                                         "Content-Length: 8\r\n\r\n"),
                                            8, 0)
                       .error == CdnHttpError::invalid_content_type,
           "cdn_gif_exact_mime_admission");

    const CdnResponseResult jpeg_accepted =
        validate_static_image_response(CdnImageType::jpeg, byte_literal("HTTP/1.1 200 OK\r\n"
                                                   "Content-Type: image/jpeg\r\n"
                                                   "Content-Length: 8\r\n"
                                                   "Cache-Control: max-age=30\r\n\r\n"),
                                      8, 1'000);
    expect(jpeg_accepted.ok() && jpeg_accepted.disposition == CdnResponseDisposition::accept_body &&
               jpeg_accepted.expires_at_seconds == 1'030 &&
               validate_static_image_response(CdnImageType::jpeg, byte_literal("HTTP/1.1 200 OK\r\n"
                                                          "Content-Type: image/png\r\n"
                                                          "Content-Length: 8\r\n\r\n"),
                                             8, 0)
                       .error == CdnHttpError::invalid_content_type,
           "cdn_jpeg_exact_mime_admission");

    const CdnResponseResult mp4_accepted =
        validate_static_image_response(
            CdnImageType::mp4,
            byte_literal("HTTP/1.1 200 OK\r\n"
                         "Content-Type: video/mp4\r\n"
                         "Content-Length: 8\r\n"
                         "Cache-Control: max-age=30\r\n\r\n"),
            8, 1'000);
    expect(mp4_accepted.ok() &&
               mp4_accepted.disposition ==
                   CdnResponseDisposition::accept_body &&
               validate_static_image_response(
                   CdnImageType::mp4,
                   byte_literal("HTTP/1.1 200 OK\r\n"
                                "Content-Type: image/webp\r\n"
                                "Content-Length: 8\r\n\r\n"),
                   8, 0)
                       .error == CdnHttpError::invalid_content_type,
           "cdn_mp4_exact_mime_admission");

    const CdnResponseResult no_store =
        validate_static_image_response(CdnImageType::png, byte_literal("HTTP/1.1 200 OK\r\n"
                                                  "Content-Type: image/png\r\n"
                                                  "Content-Length: 4\r\n"
                                                  "Cache-Control: no-store, no-cache\r\n\r\n"),
                                     4, 200);
    const CdnResponseResult not_modified =
        validate_static_image_response(CdnImageType::png, byte_literal("HTTP/1.1 304 Not Modified\r\n"
                                                  "Cache-Control: max-age=60\r\n\r\n"),
                                     0, 500);
    expect(no_store.ok() && !no_store.store_allowed && no_store.must_revalidate && not_modified.ok() &&
               not_modified.disposition == CdnResponseDisposition::not_modified &&
               not_modified.expires_at_seconds == 560,
           "cdn_no_store_and_304_policy");

    const CdnResponseResult redirect = validate_static_image_response(CdnImageType::png, byte_literal("HTTP/1.1 302 Found\r\n"
                     "Location: https://cdn.discordapp.com/stickers/8.png\r\n"
                     "Content-Length: 0\r\n\r\n"),
        0, 0);
    expect(redirect.ok() && redirect.disposition == CdnResponseDisposition::redirect &&
               bytes_equal(redirect.location, byte_literal("https://cdn.discordapp.com/stickers/8.png")),
           "cdn_redirect_response_admission");

    expect(
        validate_static_image_response(CdnImageType::png, byte_literal("HTTP/1.1 200 OK\r\nContent-Type: image/webp\r\n"
                                                  "Content-Length: 4\r\n\r\n"),
                                     4, 0)
                    .error == CdnHttpError::invalid_content_type &&
            validate_static_image_response(CdnImageType::png, byte_literal("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\n"
                             "Content-Encoding: gzip\r\nContent-Length: 4\r\n\r\n"),
                4, 0)
                    .error == CdnHttpError::invalid_content_encoding &&
            validate_static_image_response(CdnImageType::png, byte_literal("HTTP/1.1 200 OK\r\nContent-Type: image/png\r\n"
                             "Content-Length: 5\r\n\r\n"),
                4, 0)
                    .error == CdnHttpError::body_size_mismatch,
        "cdn_mime_encoding_and_length_reject");
}

void test_cdn_client_contract() noexcept {
    expect(cdn_client_runtime_bytes(kCdnClientMinimumResponseBytes) >=
                   kCdnClientMinimumResponseBytes &&
               cdn_client_runtime_bytes(kCdnClientMinimumResponseBytes - 1) == 0 &&
               cdn_client_runtime_bytes(kMediaMaximumEncodedVideoBytes + 1) == 0,
           "cdn_client_runtime_bounds");

    CdnClientResponse metadata{};
    metadata.body_bytes = 123;
    metadata.status_code = 200;
    metadata.expires_at_seconds = 999;
    metadata.store_allowed = true;
    metadata.etag_bytes = 4;
    metadata.etag[0] = '"';
    metadata.etag[1] = 'v';
    metadata.etag[2] = '1';
    metadata.etag[3] = '"';
    const CdnResponseResult policy = cdn_client_response_policy(metadata);
    expect(policy.ok() && policy.disposition == CdnResponseDisposition::accept_body &&
               policy.body_bytes == 123 && bytes_equal(policy.etag, byte_literal("\"v1\"")),
           "cdn_client_stable_policy_view");

    CdnClient invalid{};
    expect(cdn_client_start(&invalid, kCdnClientMinimumResponseBytes - 1).error ==
               CdnClientError::invalid_input,
           "cdn_client_rejects_small_response_storage");

    CdnClient idle{};
    const CdnClientResult started =
        cdn_client_start(&idle, kCdnClientMinimumResponseBytes);
    expect(started.ok() && idle.runtime && idle.worker.thread, "cdn_client_idle_start");
    if (idle.worker.thread) {
        expect(cdn_client_submit(
                   &idle,
                   CdnClientRequest{
                       byte_literal("https://cdn.discordapp.com/a.png"), {}, 1, 1'000, CdnHost::cdn})
                       .error == CdnClientSubmitError::invalid_input,
               "cdn_client_origin_form_only");
        const CdnClientResult stopped = cdn_client_stop(&idle, 5'000);
        expect(stopped.phase == CdnClientPhase::stopped && stopped.connections == 0 &&
                   stopped.requests == 0,
               "cdn_client_idle_stop");
    }
}

void test_animation_timeline() noexcept {
    AnimationTimelineEntry bounded_entry[1]{};
    u32 bounded_buckets[2]{};
    AnimationTimeline bounded{};
    const MediaAssetKey bounded_key = media_asset_custom_emoji(699, true);
    const bool bounded_ready = animation_timeline_initialize(
        &bounded, Slice<AnimationTimelineEntry>{bounded_entry, 1}, Slice<u32>{bounded_buckets, 2});
    const AnimationTimelineResult oversized_frames = animation_timeline_publish(
        &bounded,
        AnimationTimelineDescriptor{bounded_key, 10'000, 1, kAnimationTimelineFrameMaximum + 1u, 0, true},
        100'000);
    const AnimationTimelineResult maximum_frames = animation_timeline_publish(
        &bounded,
        AnimationTimelineDescriptor{bounded_key, 10'000, 1, kAnimationTimelineFrameMaximum, 0, true},
        100'000);
    expect(bounded_ready && oversized_frames.error == AnimationTimelineError::invalid_descriptor &&
               maximum_frames.ok() && bounded_entry[0].frame_count == kAnimationTimelineFrameMaximum,
           "animation_timeline_frame_index_narrowing_is_validated");

    AnimationTimelineEntry entries[2]{};
    u32 buckets[4]{};
    AnimationTimeline timeline{};
    expect(!animation_timeline_initialize(nullptr, {}) &&
               animation_timeline_initialize(&timeline, Slice<AnimationTimelineEntry>{entries, 2},
                                             Slice<u32>{buckets, 4}) &&
               timeline.capacity == 2 && timeline.entry_count == 0,
           "animation_timeline_caller_owned_initialize");

    const MediaAssetKey emoji = media_asset_custom_emoji(700, true);
    const MediaAssetKey sticker = media_asset_sticker(701, 2);
    const MediaAssetKey overflow = media_asset_attachment(702);
    const AnimationTimelineResult first = animation_timeline_publish(
        &timeline, AnimationTimelineDescriptor{emoji, 30'000, 7, 3, 2, true}, 100'000);
    const AnimationTimelineResult second = animation_timeline_publish(
        &timeline, AnimationTimelineDescriptor{sticker, 10'000, 8, 2, 0, false}, 100'000);
    const AnimationTimelineResult full = animation_timeline_publish(
        &timeline, AnimationTimelineDescriptor{overflow, 10'000, 9, 2, 1, true}, 100'000);
    expect(first.ok() && second.ok() && full.error == AnimationTimelineError::no_capacity &&
               timeline.entry_count == 2 &&
               animation_timeline_next_wake_at_us(&timeline) == 130'000 &&
               animation_timeline_wait_timeout_ms(&timeline, 129'001) == 1 &&
               animation_timeline_wait_timeout_ms(&timeline, 130'000) == 0,
           "animation_timeline_publish_capacity_and_ceiling_wait");

    animation_timeline_begin_visibility(&timeline);
    expect(animation_timeline_observe_visible(&timeline, sticker, 110'000),
           "animation_timeline_resume_observed");
    animation_timeline_end_visibility(&timeline, 110'000);
    expect(animation_timeline_next_wake_at_us(&timeline) == 120'000,
           "animation_timeline_offscreen_pause_preserves_remaining");

    AnimationFrameAdvance advances[1]{};
    AnimationTimelineCollectResult collected =
        animation_timeline_collect_due(&timeline, 120'000, Slice<AnimationFrameAdvance>{advances, 1});
    expect(collected.ok() && collected.advances == 1 && collected.completed == 0 &&
               media_asset_key_equal(advances[0].key, sticker) && advances[0].scheduled_at_us == 120'000 &&
               advances[0].frame_index == 1 && advances[0].play_index == 0 &&
               animation_timeline_commit_advance(&timeline, advances[0], 10'000).ok() &&
               animation_timeline_next_wake_at_us(&timeline) == 130'000,
           "animation_timeline_due_and_commit");

    collected =
        animation_timeline_collect_due(&timeline, 130'000, Slice<AnimationFrameAdvance>{advances, 1});
    const AnimationFrameAdvance deferred = advances[0];
    expect(collected.advances == 1 && deferred.frame_index == 0 && deferred.play_index == 1 &&
               animation_timeline_defer_advance(&timeline, deferred, 130'000, 5'000).ok() &&
               animation_timeline_next_wake_at_us(&timeline) == 135'000,
           "animation_timeline_retry_defer_uses_current_time");

    animation_timeline_begin_visibility(&timeline);
    expect(animation_timeline_observe_visible(&timeline, emoji, 1'000'000),
           "animation_timeline_paused_entry_resume");
    animation_timeline_end_visibility(&timeline, 1'000'000);
    expect(animation_timeline_next_wake_at_us(&timeline) == 1'020'000,
           "animation_timeline_visibility_epoch_switch");

    collected =
        animation_timeline_collect_due(&timeline, 1'020'000, Slice<AnimationFrameAdvance>{advances, 1});
    expect(collected.advances == 1 && advances[0].frame_index == 1 && advances[0].play_index == 0 &&
               animation_timeline_commit_advance(&timeline, advances[0], 10'000).ok(),
           "animation_timeline_first_play_advance");
    collected =
        animation_timeline_collect_due(&timeline, 1'050'000, Slice<AnimationFrameAdvance>{advances, 1});
    const bool late_frame = collected.advances == 1 && advances[0].scheduled_at_us == 1'030'000 &&
                            advances[0].frame_index == 2 &&
                            animation_timeline_commit_advance(&timeline, advances[0], 10'000).ok() &&
                            animation_timeline_next_wake_at_us(&timeline) == 1'040'000;
    collected =
        animation_timeline_collect_due(&timeline, 1'050'000, Slice<AnimationFrameAdvance>{advances, 1});
    const bool looped = collected.advances == 1 && advances[0].scheduled_at_us == 1'040'000 &&
                        advances[0].frame_index == 0 && advances[0].play_index == 1 &&
                        animation_timeline_commit_advance(&timeline, advances[0], 10'000).ok();
    expect(late_frame && looped,
           "animation_timeline_absolute_deadline_catchup_no_drift");

    collected =
        animation_timeline_collect_due(&timeline, 1'050'000, Slice<AnimationFrameAdvance>{advances, 1});
    const bool second_frame = collected.advances == 1 && advances[0].frame_index == 1 &&
                              advances[0].play_index == 1 &&
                              animation_timeline_commit_advance(&timeline, advances[0], 10'000).ok();
    collected =
        animation_timeline_collect_due(&timeline, 1'060'000, Slice<AnimationFrameAdvance>{advances, 1});
    const bool final_frame = collected.advances == 1 && advances[0].frame_index == 2 &&
                             advances[0].play_index == 1 &&
                             animation_timeline_commit_advance(&timeline, advances[0], 10'000).ok();
    collected =
        animation_timeline_collect_due(&timeline, 1'070'000, Slice<AnimationFrameAdvance>{advances, 1});
    expect(second_frame && final_frame && collected.advances == 0 && collected.completed == 1 &&
               animation_timeline_next_wake_at_us(&timeline) == 0,
           "animation_timeline_finite_play_count_rests_on_final_frame");

    expect(animation_timeline_release(&timeline, emoji, 6).error ==
                   AnimationTimelineError::stale_generation &&
               animation_timeline_release(&timeline, emoji, 7).ok() &&
               animation_timeline_release(&timeline, emoji, 7).error == AnimationTimelineError::not_found,
           "animation_timeline_generation_safe_release");

    AnimationTimelineEntry peer_entries[2]{};
    u32 peer_buckets[4]{};
    AnimationTimeline peers{};
    AnimationFrameAdvance peer_advances[1]{};
    const MediaAssetKey peer_a = media_asset_custom_emoji(710, true);
    const MediaAssetKey peer_b = media_asset_custom_emoji(711, true);
    bool peers_ok = animation_timeline_initialize(&peers, Slice<AnimationTimelineEntry>{peer_entries, 2},
                                                  Slice<u32>{peer_buckets, 4}) &&
                    animation_timeline_publish(
                        &peers, AnimationTimelineDescriptor{peer_a, 10'000, 1, 2, 0, true}, 100'000)
                        .ok() &&
                    animation_timeline_publish(
                        &peers, AnimationTimelineDescriptor{peer_b, 10'000, 2, 2, 0, true}, 100'000)
                        .ok();
    const AnimationTimelineCollectResult first_peer =
        peers_ok ? animation_timeline_collect_due(&peers, 110'000,
                                                  Slice<AnimationFrameAdvance>{peer_advances, 1})
                 : AnimationTimelineCollectResult{};
    const AnimationFrameAdvance pending_peer = peer_advances[0];
    const u64 peer_wake = animation_timeline_next_wake_at_us(&peers);
    const AnimationTimelineCollectResult second_peer =
        first_peer.advances == 1 ? animation_timeline_collect_due(
                                       &peers, 110'000, Slice<AnimationFrameAdvance>{peer_advances, 1})
                                 : AnimationTimelineCollectResult{};
    expect(peers_ok && first_peer.advances == 1 && peer_wake == 110'000 && second_peer.advances == 1 &&
               !media_asset_key_equal(pending_peer.key, peer_advances[0].key),
           "animation_timeline_pending_decode_does_not_block_due_peer");

    AnimationTimelineEntry scale_entries[256]{};
    u32 scale_buckets[512]{};
    AnimationTimeline scale{};
    bool scale_ok = animation_timeline_initialize(
        &scale, Slice<AnimationTimelineEntry>{scale_entries, 256}, Slice<u32>{scale_buckets, 512});
    for (u32 index = 0; index < 256 && scale_ok; ++index) {
        const MediaAssetKey key = media_asset_custom_emoji(900'000 + index, true);
        const AnimationTimelineResult published = animation_timeline_publish(
            &scale, AnimationTimelineDescriptor{key, 16'667, index + 1, 8, 0, true}, 2'000'000);
        scale_ok = published.ok() && animation_timeline_find(&scale, key).slot == published.slot;
    }
    for (u32 index = 0; index < 256 && scale_ok; index += 2) {
        scale_ok =
            animation_timeline_release(&scale, media_asset_custom_emoji(900'000 + index, true), index + 1)
                .ok();
    }
    for (u32 index = 0; index < 128 && scale_ok; ++index) {
        const MediaAssetKey key = media_asset_custom_emoji(1'000'000 + index, true);
        scale_ok =
            animation_timeline_publish(
                &scale, AnimationTimelineDescriptor{key, 16'667, 10'000 + index, 8, 0, true}, 2'000'000)
                .ok() &&
            animation_timeline_find(&scale, key).ok();
    }
    AnimationFrameAdvance scale_advances[256]{};
    const AnimationTimelineCollectResult scale_collected = animation_timeline_collect_due(
        &scale, 2'016'667, Slice<AnimationFrameAdvance>{scale_advances, 256});
    scale_ok = scale_ok && scale_collected.ok() && scale_collected.advances == 256;
    for (u32 index = 0; index < scale_collected.advances && scale_ok; ++index) {
        scale_ok = animation_timeline_commit_advance(&scale, scale_advances[index], 16'667).ok();
    }
    expect(scale_ok && scale.entry_count == 256 &&
               scale.counters.due_advances == 256,
           "animation_timeline_identity_batch_churn");
}

void test_animation_visual_runtime() noexcept {
    AnimationVisualRuntime runtime{};
    expect(animation_visual_runtime_initialize(&runtime) &&
               runtime.timeline.capacity == kAnimationVisualInitialCapacity &&
               runtime.visual_storage.capacity == kAnimationVisualInitialCapacity,
           "animation_visual_runtime_commits_small_initial_working_set");
    AnimationTimelineEntry* const entry_base = runtime.timeline.entries;
    u32* const bucket_base = runtime.timeline.buckets;
    AnimationVisualState* const visual_base = runtime.visuals;
    bool published = true;
    u32 retained_slot = 0;
    for (u32 index = 0; index <= kAnimationVisualInitialCapacity && published; ++index) {
        const AnimationTimelineResult result = animation_visual_runtime_publish(
            &runtime,
            AnimationTimelineDescriptor{media_asset_custom_emoji(2'000'000 + index, true), 16'667,
                                        index + 1, 2, 0, true},
            1'000'000);
        published = result.ok();
        if (index == 1) {
            retained_slot = result.slot;
            runtime.visuals[result.slot].key = media_asset_custom_emoji(2'000'001, true);
            runtime.visuals[result.slot].generation = 2;
            runtime.visuals[result.slot].occupied = true;
        }
    }
    expect(published && runtime.timeline.capacity == 256 && runtime.timeline.entries == entry_base &&
               runtime.timeline.buckets == bucket_base && runtime.visuals == visual_base &&
               runtime.visuals[retained_slot].occupied &&
               animation_timeline_find(&runtime.timeline, media_asset_custom_emoji(2'000'001, true)).slot ==
                   retained_slot,
           "animation_visual_runtime_grows_without_moves_or_identity_loss");
    expect(animation_visual_runtime_reserve(&runtime, kAnimationVisualMaximumCapacity) &&
               runtime.timeline.capacity == kAnimationVisualMaximumCapacity &&
               !animation_visual_runtime_reserve(&runtime, kAnimationVisualMaximumCapacity + 1u),
           "animation_visual_runtime_enforces_hard_bound");
    animation_visual_runtime_reset(&runtime);
    expect(runtime.timeline.entry_count == 0 && runtime.visuals[retained_slot].occupied == false,
           "animation_visual_runtime_reset_clears_committed_state");
    animation_visual_runtime_release(&runtime);
    expect(runtime.visuals == nullptr && runtime.timeline.entries == nullptr &&
               runtime.entries.arena.base == nullptr,
           "animation_visual_runtime_release_returns_address_space");
}

void test_encoded_media_cache() noexcept {
    EncodedMediaCacheEntry entries[3]{};
    u16 page_owners[4]{};
    u32 index_buckets[8]{};
    u8 storage[4 * kEncodedMediaCachePageBytes]{};
    EncodedMediaCache cache{};
    expect(encoded_media_cache_initialize(&cache, Slice<EncodedMediaCacheEntry>{entries, 3},
                                          Slice<u16>{page_owners, 4}, Slice<u8>{storage, sizeof(storage)},
                                          Slice<u32>{index_buckets, 8}),
           "encoded_media_cache_caller_owned_initialize");

    u8 one_page[1'000]{};
    u8 two_pages[5'000]{};
    for (u32 index = 0; index < sizeof(one_page); ++index) {
        one_page[index] = static_cast<u8>(index * 17u);
    }
    for (u32 index = 0; index < sizeof(two_pages); ++index) {
        two_pages[index] = static_cast<u8>(index * 29u);
    }
    const EncodedMediaCachePolicy fresh{byte_literal("\"v1\""), 200, true, false};
    const MediaAssetKey key1 = media_asset_sticker(1, 1);
    const MediaAssetKey key2 = media_asset_sticker(2, 1);
    const MediaAssetKey key3 = media_asset_sticker(3, 1);
    const MediaAssetKey key4 = media_asset_sticker(4, 1);
    const EncodedMediaCacheStoreResult first =
        encoded_media_cache_store(&cache, key1, ConstSlice<u8>{one_page, sizeof(one_page)}, fresh);
    const EncodedMediaCacheLookup hit = encoded_media_cache_find(&cache, key1, 100);
    expect(first.stored() && hit.state == EncodedMediaCacheLookupState::fresh &&
               bytes_equal(hit.bytes, ConstSlice<u8>{one_page, sizeof(one_page)}) &&
               bytes_equal(hit.etag, byte_literal("\"v1\"")) && cache.entry_count == 1 &&
               cache.used_pages == 1,
           "encoded_media_cache_exact_publish_and_hit");

    encoded_media_cache_store(&cache, key2, ConstSlice<u8>{two_pages, sizeof(two_pages)}, fresh);
    encoded_media_cache_store(&cache, key3, ConstSlice<u8>{one_page, sizeof(one_page)}, fresh);
    encoded_media_cache_find(&cache, key1, 100);
    const EncodedMediaCacheStoreResult fragmented =
        encoded_media_cache_store(&cache, key4, ConstSlice<u8>{two_pages, sizeof(two_pages)}, fresh);
    expect(fragmented.stored() &&
               encoded_media_cache_find(&cache, key2, 100).state == EncodedMediaCacheLookupState::miss &&
               encoded_media_cache_find(&cache, key1, 100).found() &&
               encoded_media_cache_find(&cache, key3, 100).found() &&
               encoded_media_cache_find(&cache, key4, 100).found() && cache.used_pages == 4,
           "encoded_media_cache_contiguous_lru_eviction");

    const EncodedMediaCacheStoreResult replaced =
        encoded_media_cache_store(&cache, key1, ConstSlice<u8>{two_pages, sizeof(two_pages)},
                                  EncodedMediaCachePolicy{byte_literal("\"v2\""), 150, true, true});
    const EncodedMediaCacheLookup stale = encoded_media_cache_find(&cache, key1, 100);
    const bool refreshed = encoded_media_cache_refresh(&cache, key1, 300, false);
    const EncodedMediaCacheLookup refreshed_hit = encoded_media_cache_find(&cache, key1, 200);
    expect(replaced.state == EncodedMediaCacheStoreState::replaced &&
               stale.state == EncodedMediaCacheLookupState::stale && refreshed &&
               refreshed_hit.state == EncodedMediaCacheLookupState::fresh &&
               bytes_equal(refreshed_hit.etag, byte_literal("\"v2\"")) &&
               refreshed_hit.bytes.count == sizeof(two_pages),
           "encoded_media_cache_replace_stale_and_304_refresh");

    EncodedMediaCacheLease lease = encoded_media_cache_acquire(&cache, key1, 200);
    const EncodedMediaCacheStoreResult pinned_replace =
        encoded_media_cache_store(&cache, key1, ConstSlice<u8>{one_page, sizeof(one_page)}, fresh);
    const bool pinned_erase_rejected = !encoded_media_cache_erase(&cache, key1);
    const bool lease_immutable = lease.acquired && lease.lookup.bytes.count == sizeof(two_pages) &&
                                 lease.lookup.bytes[17] == two_pages[17];
    const bool released = encoded_media_cache_release(&cache, &lease);
    const EncodedMediaCacheStoreResult after_release =
        encoded_media_cache_store(&cache, key1, ConstSlice<u8>{one_page, sizeof(one_page)}, fresh);
    expect(pinned_replace.state == EncodedMediaCacheStoreState::no_capacity && pinned_erase_rejected &&
               lease_immutable && released &&
               after_release.state == EncodedMediaCacheStoreState::replaced &&
               cache.counters.lease_acquires == 1 && cache.counters.lease_releases == 1 &&
               cache.counters.pinned_rejections >= 2,
           "encoded_media_cache_lease_pins_immutable_source");

    const u32 before_entries = cache.entry_count;
    const EncodedMediaCacheStoreResult transient = encoded_media_cache_store(
        &cache, media_asset_sticker(9, 1), ConstSlice<u8>{one_page, sizeof(one_page)},
        EncodedMediaCachePolicy{{}, 0, false, true});
    expect(transient.state == EncodedMediaCacheStoreState::transient &&
               cache.entry_count == before_entries && encoded_media_cache_erase(&cache, key1) &&
               encoded_media_cache_find(&cache, key1, 0).state == EncodedMediaCacheLookupState::miss,
           "encoded_media_cache_no_store_and_explicit_erase");

    const bool reinitialized = encoded_media_cache_initialize(
        &cache, Slice<EncodedMediaCacheEntry>{entries, 3}, Slice<u16>{page_owners, 4},
        Slice<u8>{storage, sizeof(storage)}, Slice<u32>{index_buckets, 8});
    const auto animation1 = media_asset_custom_emoji(5001, true);
    const auto animation2 = media_asset_custom_emoji(5002, true);
    const auto animation3 = media_asset_custom_emoji(5003, true);
    const u64 video_source1 = encoded_media_new_content_identity();
    encoded_media_cache_store(&cache, animation1, {one_page, sizeof(one_page)}, fresh);
    const auto identity1 = encoded_media_cache_find(&cache, animation1, 100);
    const u64 video_source2 = encoded_media_new_content_identity();
    encoded_media_cache_store(&cache, animation2, {one_page, sizeof(one_page)}, fresh);
    const auto identity2 = encoded_media_cache_find(&cache, animation2, 100);
    expect(reinitialized && identity1.content_identity != 0 &&
               identity1.content_identity == identity2.content_identity && hit.content_identity == 0,
           "encoded_animation_identity_shares_exact_bytes_only");
    expect(video_source1 != 0 && video_source2 != 0 && video_source1 != video_source2 &&
               video_source1 != identity1.content_identity && video_source2 != identity1.content_identity,
           "encoded_media_source_lifetimes_and_cache_share_distinct_nonzero_identities");
    // Force a candidate hash collision: byte equality must still reject it.
    one_page[17] ^= 1;
    const u64 collision = hash64_counted_bytes(kHash64Seed, one_page, sizeof(one_page));
    for (auto& entry : entries) if (entry.occupied) entry.content_fingerprint = collision;
    encoded_media_cache_store(&cache, animation3, {one_page, sizeof(one_page)}, fresh);
    const auto identity3 = encoded_media_cache_find(&cache, animation3, 100);
    expect(identity3.content_identity != 0 && identity3.content_identity != identity1.content_identity,
           "encoded_animation_hash_collision_never_aliases_frames");
    encoded_media_cache_initialize(&cache, Slice<EncodedMediaCacheEntry>{entries, 3}, Slice<u16>{page_owners, 4},
                                   Slice<u8>{storage, sizeof(storage)}, Slice<u32>{index_buckets, 8});
    const u64 video_source3 = encoded_media_new_content_identity();
    expect(video_source3 != 0 && video_source3 != video_source1 && video_source3 != video_source2 &&
               video_source3 != identity1.content_identity && video_source3 != identity3.content_identity,
           "encoded_media_source_identity_survives_owner_cache_reset");
    encoded_media_cache_store(&cache, animation3, {one_page, sizeof(one_page)}, fresh);
    expect(encoded_media_cache_find(&cache, animation3, 100).content_identity != identity3.content_identity,
           "encoded_animation_identity_not_reused_after_reset");
}

void test_encoded_media_cache_dense_index() noexcept {
    AccountMedia media{};
    const AccountMediaRequestResult initialized = account_media_initialize(&media);
    if (!initialized.ok()) {
        expect(false, "encoded_media_cache_dense_initialize");
        return;
    }
    u8 payload = 0xA5;
    bool exact = true;
    for (u32 index = 0; index < kAccountMediaCacheEntryCapacity; ++index) {
        const MediaAssetKey key = media_asset_custom_emoji(50'000 + index, false);
        const EncodedMediaCacheStoreResult stored =
            encoded_media_cache_store(&media.cache, key, ConstSlice<u8>{&payload, 1},
                                      EncodedMediaCachePolicy{{}, ~0ull, true, false});
        exact = exact && stored.stored();
    }
    for (u32 offset = 0; offset < kAccountMediaCacheEntryCapacity; ++offset) {
        const u32 index = kAccountMediaCacheEntryCapacity - 1 - offset;
        EncodedMediaCacheLease lease = encoded_media_cache_acquire(
            &media.cache, media_asset_custom_emoji(50'000 + index, false), 1);
        exact = exact && lease.acquired && lease.lookup.state == EncodedMediaCacheLookupState::fresh &&
                lease.lookup.bytes.count == 1 && lease.lookup.bytes[0] == payload &&
                encoded_media_cache_release(&media.cache, &lease);
    }
    expect(exact && media.cache.entry_count == kAccountMediaCacheEntryCapacity &&
               media.cache.used_pages == kAccountMediaCacheEntryCapacity &&
               media.cache.counters.lease_acquires == kAccountMediaCacheEntryCapacity &&
               media.cache.counters.lease_acquires == media.cache.counters.lease_releases &&
               account_media_stop(&media, 5'000),
           "encoded_media_cache_full_dense_index_hash_and_lease");
}

namespace {

CdnResponseResult accepted_cdn_response(ConstSlice<u8> etag, u64 expires_at_seconds, u32 body_bytes) noexcept {
    return CdnResponseResult{etag, {}, expires_at_seconds, body_bytes, 200,
                             CdnResponseDisposition::accept_body, CdnHttpError::none, true, false};
}

} // namespace

// Profile artwork past the GPU decoder's 512 px (Spotify's 640 px album art)
// decodes once, reduced to the card's 160 px, from a response and from cache.
void test_cdn_reduced_jpeg_pipeline() noexcept {
    static u8 encoded[8'192], oriented[8'192], scanlines[640 * 480 * 4];
    static u8 storage[8 * kEncodedMediaCachePageBytes];
    EncodedMediaCacheEntry entries[4]{};
    u16 owners[8]{};
    u32 buckets[8]{};
    EncodedMediaCache cache{};
    CdnMediaPipeline pipeline{};
    JpegFixtureResult fixture{};
    u32 oriented_bytes = 0;
    const bool ready =
        build_marked_jpeg_fixture({encoded, sizeof(encoded)}, 640, 480, &fixture) &&
        build_jpeg_exif_orientation_fixture({encoded, fixture.encoded_bytes}, {oriented, sizeof(oriented)}, 6,
                                            false, &oriented_bytes) &&
        encoded_media_cache_initialize(&cache, {entries, 4}, {owners, 8}, {storage, sizeof(storage)}, {buckets, 8}) &&
        cdn_media_pipeline_initialize(&pipeline, &cache, {scanlines, sizeof(scanlines)});
    pipeline.jpeg_workspace = &g_jpeg_workspace;
    const auto policy = [](u32 bytes) noexcept {
        return accepted_cdn_response(byte_literal("\"art-1\""), 2'100, bytes);
    };
    const auto key = [](u64 id) noexcept {
        auto value = media_asset_embed(id, 1, 0, MediaEmbedResource::image, MediaAttachmentFormat::jpeg, false,
                                       MediaResolutionTier::px512);
        value.kind = MediaAssetKind::profile_image;
        return value;
    };
    const auto pixel = [&](const CdnStaticJpegResult& r, u32 x, u32 y) noexcept {
        return r.rgba8.data[(y * r.rgba_width + x) * 4u];
    };
    const auto admitted = cdn_media_pipeline_admit_static_jpeg(&pipeline, key(7001), policy(fixture.encoded_bytes),
                                                              {encoded, fixture.encoded_bytes});
    const bool admitted_ok = admitted.state == CdnStaticJpegState::decoded_response && admitted.store.stored() &&
                             !admitted.coefficients && admitted.rgba_width == 160 && admitted.rgba_height == 120 &&
                             admitted.rgba8.count == 160u * 120u * 4u && pixel(admitted, 0, 0) >= 156 &&
                             pixel(admitted, 2, 0) <= 132 && pixel(admitted, 159, 119) <= 132 &&
                             admitted.rgba8.data[3] == 255;
    const auto hit = cdn_media_pipeline_lookup_static_jpeg(&pipeline, key(7001), 2'010);
    expect(ready && admitted_ok && hit.state == CdnStaticJpegState::decoded_cache_hit && hit.rgba_width == 160 &&
               hit.rgba_height == 120 && pixel(hit, 1, 1) >= 156 && bytes_equal(hit.etag, byte_literal("\"art-1\"")),
           "cdn_pipeline_large_profile_jpeg_reduces_to_card_size_and_caches");
    // EXIF orientation 6 turns the image a quarter clockwise: the marked
    // top-left block lands top-right.
    const auto turned = cdn_media_pipeline_admit_static_jpeg(&pipeline, key(7002), policy(oriented_bytes),
                                                            {oriented, oriented_bytes});
    expect(turned.decoded() && turned.rgba_width == 120 && turned.rgba_height == 160 &&
               pixel(turned, 119, 0) >= 156 && pixel(turned, 0, 0) <= 132,
           "cdn_pipeline_large_profile_jpeg_applies_exif_orientation");
    // Attachment renditions past the GPU path decode at full size.
    const auto attachment = cdn_media_pipeline_admit_static_jpeg(
        &pipeline, media_asset_attachment(7003), policy(fixture.encoded_bytes), {encoded, fixture.encoded_bytes});
    expect(attachment.decoded() && !attachment.coefficients && attachment.rgba_width == 640 &&
               attachment.rgba_height == 480 && attachment.rgba8.count == 640u * 480u * 4u &&
               pixel(attachment, 0, 0) >= 156 && pixel(attachment, 639, 479) <= 132,
           "cdn_pipeline_large_attachment_jpeg_decodes_full_size_on_the_cpu");
}

namespace {

// Bits appended least significant first, as VP8L reads them.
struct WebpBitWriter {
    u8* data;
    u32 bits;
    void put(u32 value, u32 count) noexcept {
        for (u32 i = 0; i < count; ++i, ++bits) {
            if (!(bits & 7u))
                data[bits / 8u] = 0;
            data[bits / 8u] = static_cast<u8>(data[bits / 8u] | (((value >> i) & 1u) << (bits & 7u)));
        }
    }
    // A one-symbol prefix code: no bits per pixel.
    void single(u32 symbol) noexcept {
        put(1, 1);
        put(0, 1);
        put(symbol > 1 ? 1u : 0u, 1);
        put(symbol, symbol > 1 ? 8u : 1u);
    }
};
// A constant-colour VP8L image stream: no transforms, cache or meta codes.
u32 write_constant_vp8l_stream(u8* out, u8 red, u8 green, u8 blue, u8 alpha) noexcept {
    WebpBitWriter w{out, 0};
    w.put(0, 3);
    w.single(green);
    w.single(red);
    w.single(blue);
    w.single(alpha);
    w.single(0);
    return (w.bits + 7u) / 8u;
}
// A keyframe whose partitions are all zero decodes to flat grey (RGB 130
// in the first subblock row, 132 below); an optional VP8L-coded ALPH plane.
u32 build_large_lossy_webp(u8* out, u32 width, u32 height, bool alpha, u8 alpha_value) noexcept {
    constexpr u32 first_partition = 48u * 1024u, tokens = 64u * 1024u;
    const u32 vp8_bytes = 10 + first_partition + tokens;
    copy_bytes(out, "RIFF", 4);
    copy_bytes(out + 8, "WEBP", 4);
    u32 at = 12;
    if (alpha) {
        copy_bytes(out + at, "VP8X", 4);
        byte_store_le32(out + at + 4, 10);
        clear_bytes(out + at + 8, 10);
        out[at + 8] = 0x10;
        const u32 w = width - 1, h = height - 1;
        out[at + 12] = static_cast<u8>(w);
        out[at + 13] = static_cast<u8>(w >> 8);
        out[at + 14] = static_cast<u8>(w >> 16);
        out[at + 15] = static_cast<u8>(h);
        out[at + 16] = static_cast<u8>(h >> 8);
        out[at + 17] = static_cast<u8>(h >> 16);
        at += 18;
        u8* chunk = out + at + 8;
        chunk[0] = 1; // VP8L-coded, unfiltered
        const u32 bytes = 1 + write_constant_vp8l_stream(chunk + 1, 0, alpha_value, 0, 0);
        copy_bytes(out + at, "ALPH", 4);
        byte_store_le32(out + at + 4, bytes);
        if (bytes & 1u)
            chunk[bytes] = 0;
        at += 8 + bytes + (bytes & 1u);
    }
    copy_bytes(out + at, "VP8 ", 4);
    byte_store_le32(out + at + 4, vp8_bytes);
    u8* frame = out + at + 8;
    const u32 tag = (1u << 4) | (first_partition << 5);
    frame[0] = static_cast<u8>(tag);
    frame[1] = static_cast<u8>(tag >> 8);
    frame[2] = static_cast<u8>(tag >> 16);
    frame[3] = 0x9D;
    frame[4] = 0x01;
    frame[5] = 0x2A;
    frame[6] = static_cast<u8>(width);
    frame[7] = static_cast<u8>(width >> 8);
    frame[8] = static_cast<u8>(height);
    frame[9] = static_cast<u8>(height >> 8);
    clear_bytes(frame + 10, first_partition + tokens);
    at += 8 + vp8_bytes;
    byte_store_le32(out + 4, at - 8);
    return at;
}
u32 build_constant_lossless_webp(u8* out, u32 width, u32 height, u8 red, u8 green, u8 blue) noexcept {
    copy_bytes(out, "RIFF", 4);
    copy_bytes(out + 8, "WEBPVP8L", 8);
    u8* payload = out + 20;
    payload[0] = 0x2F;
    const u32 header = (width - 1) | ((height - 1) << 14);
    byte_store_le32(payload + 1, header);
    const u32 bytes = 5 + write_constant_vp8l_stream(payload + 5, red, green, blue, 255);
    byte_store_le32(out + 16, bytes);
    if (bytes & 1u)
        payload[bytes] = 0;
    const u32 total = 20 + bytes + (bytes & 1u);
    byte_store_le32(out + 4, total - 8);
    return total;
}

} // namespace

// Still WebPs past the GPU paths' 512 px: lossy ones stream on the CPU (and
// reduce past 2048), lossless ones decode whole, both as straight RGBA8.
void test_cdn_reduced_webp_pipeline() noexcept {
    static u8 encoded[160 * 1024], scanlines[12u << 20];
    static u8 storage[64 * kEncodedMediaCachePageBytes];
    EncodedMediaCacheEntry entries[8]{};
    u16 owners[64]{};
    u32 buckets[16]{};
    EncodedMediaCache cache{};
    CdnMediaPipeline pipeline{};
    const bool ready =
        encoded_media_cache_initialize(&cache, {entries, 8}, {owners, 64}, {storage, sizeof(storage)}, {buckets, 16}) &&
        cdn_media_pipeline_initialize(&pipeline, &cache, {scanlines, sizeof(scanlines)}) &&
        cdn_media_pipeline_bind_webp_workspace(&pipeline, &g_vp8l_workspace);
    const auto policy = [](u32 bytes) noexcept {
        return accepted_cdn_response(byte_literal("\"webp-1\""), 2'100, bytes);
    };
    const auto key = [](u64 id) noexcept {
        return media_asset_attachment(id, MediaAttachmentFormat::webp, MediaResolutionTier::px2048);
    };
    const auto pixel = [](const CdnStaticWebpResult& r, u32 x, u32 y) noexcept {
        u32 value = 0;
        copy_bytes(&value, r.rgba8.data + (y * r.image.width + x) * 4u, 4);
        return value;
    };
    u32 bytes = build_large_lossy_webp(encoded, 1104, 600, false, 0);
    const auto lossy = cdn_media_pipeline_admit_static_webp(&pipeline, key(9001), policy(bytes), {encoded, bytes}, {});
    expect(ready && lossy.state == CdnStaticWebpState::decoded_response && lossy.image.width == 1104 &&
               lossy.image.height == 600 && lossy.image.codec == WebpCodec::none &&
               lossy.rgba8.count == 1104u * 600u * 4u && pixel(lossy, 0, 0) == 0xFF828282u &&
               pixel(lossy, 1103, 599) == 0xFF848484u,
           "cdn_pipeline_large_lossy_webp_decodes_full_size_on_the_cpu");
    const auto hit = cdn_media_pipeline_lookup_static_webp(&pipeline, key(9001), 2'010);
    expect(hit.state == CdnStaticWebpState::decoded_cache_hit && hit.image.width == 1104 &&
               pixel(hit, 500, 300) == 0xFF848484u,
           "cdn_pipeline_large_lossy_webp_decodes_again_from_cache");
    bytes = build_large_lossy_webp(encoded, 2400, 160, false, 0);
    const auto reduced = cdn_media_pipeline_admit_static_webp(&pipeline, key(9002), policy(bytes), {encoded, bytes}, {});
    expect(reduced.decoded() && reduced.image.width == 1200 && reduced.image.height == 80 &&
               pixel(reduced, 600, 40) == 0xFF848484u,
           "cdn_pipeline_lossy_webp_past_2048_reduces_by_a_box_filter");
    bytes = build_large_lossy_webp(encoded, 1104, 64, true, 0x80);
    const auto alpha = cdn_media_pipeline_admit_static_webp(&pipeline, key(9003), policy(bytes), {encoded, bytes}, {});
    expect(alpha.decoded() && alpha.image.width == 1104 && alpha.image.has_alpha &&
               (pixel(alpha, 700, 40) >> 24) == 0x80 && (pixel(alpha, 700, 40) & 0xFFu) == 0x84,
           "cdn_pipeline_large_lossy_webp_applies_a_vp8l_coded_alpha_plane");
    bytes = build_constant_lossless_webp(encoded, 1024, 600, 10, 20, 30);
    const auto lossless =
        cdn_media_pipeline_admit_static_webp(&pipeline, key(9004), policy(bytes), {encoded, bytes}, {});
    expect(lossless.decoded() && lossless.image.width == 1024 && lossless.image.height == 600 &&
               lossless.rgba8.count == 1024u * 600u * 4u && pixel(lossless, 1023, 599) == 0xFF1E140Au,
           "cdn_pipeline_lossless_webp_past_512_decodes_whole_on_the_cpu");
    bytes = build_constant_lossless_webp(encoded, 2049, 8, 10, 20, 30);
    const auto oversized =
        cdn_media_pipeline_admit_static_webp(&pipeline, key(9005), policy(bytes), {encoded, bytes}, {});
    expect(!oversized.decoded() && oversized.decode.error == WebpError::dimensions,
           "cdn_pipeline_lossless_webp_past_2048_is_rejected");
}

void test_cdn_static_png_pipeline() noexcept {
    PngFixtureResult fixture{};
    expect(build_rgba8_png_fixture(64, 64, Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                   Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &fixture),
           "cdn_pipeline_png_fixture_build");

    EncodedMediaCacheEntry entries[3]{};
    u16 page_owners[5]{};
    u8 storage[5 * kEncodedMediaCachePageBytes]{};
    u32 buckets[8]{};
    EncodedMediaCache cache{};
    CdnMediaPipeline pipeline{};
    ApngWorkspace apng_workspace{};
    GifWorkspace gif_workspace{};
    AnimatedWebpWorkspace animated_webp_workspace{};
    expect(encoded_media_cache_initialize(&cache, Slice<EncodedMediaCacheEntry>{entries, 3},
                                          Slice<u16>{page_owners, 5},
                                          Slice<u8>{storage, sizeof(storage)}, Slice<u32>{buckets, 8}) &&
               cdn_media_pipeline_initialize(&pipeline, &cache,
                                             Slice<u8>{g_png_scanlines, sizeof(g_png_scanlines)}) &&
               cdn_media_pipeline_bind_webp_workspace(&pipeline, &g_vp8l_workspace) &&
               cdn_media_pipeline_bind_animated_webp_workspace(&pipeline, &animated_webp_workspace) &&
               cdn_media_pipeline_bind_apng_workspace(&pipeline, &apng_workspace) &&
               cdn_media_pipeline_bind_gif_workspace(&pipeline, &gif_workspace),
           "cdn_pipeline_caller_owned_initialize");
    pipeline.webp_workspace = &g_vp8l_workspace;
    pipeline.animated_webp_workspace = &animated_webp_workspace;
    pipeline.apng_workspace = &apng_workspace;
    pipeline.gif_workspace = &gif_workspace;
    pipeline.lossy_webp_workspace = &g_lossy_webp_workspace;

    const CdnResponseResult accepted =
        validate_static_image_response(CdnImageType::png, byte_literal("HTTP/1.1 200 OK\r\n"
                                                  "Content-Type: image/png\r\n"
                                                  "Transfer-Encoding: chunked\r\n"
                                                  "ETag: \"png-1\"\r\n"
                                                  "Cache-Control: max-age=60\r\n\r\n"),
                                     fixture.encoded_bytes, 1'000);
    const MediaAssetKey key = media_asset_sticker(500, 1);
    const CdnStaticPngResult admitted = cdn_media_pipeline_admit_static_png(
        &pipeline, key, accepted, ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes});
    const CdnStaticPngResult hit = cdn_media_pipeline_lookup_static_png(&pipeline, key, 1'020);
    expect(admitted.state == CdnStaticPngState::decoded_response && admitted.store.stored() &&
               admitted.image.width == 64 && admitted.image.height == 64 &&
               admitted.scanlines.count == fixture.filtered_bytes &&
               hit.state == CdnStaticPngState::decoded_cache_hit && hit.decode.ok() &&
               bytes_equal(hit.etag, byte_literal("\"png-1\"")),
           "cdn_pipeline_decode_before_cache_and_fresh_hit");

    const CdnStaticPngResult stale = cdn_media_pipeline_lookup_static_png(&pipeline, key, 1'060);
    const bool stale_etag_ok = bytes_equal(stale.etag, byte_literal("\"png-1\""));
    const CdnResponseResult not_modified =
        validate_static_image_response(CdnImageType::png, byte_literal("HTTP/1.1 304 Not Modified\r\n"
                                                  "ETag: \"png-2\"\r\n"
                                                  "Cache-Control: max-age=120\r\n\r\n"),
                                     0, 1'060);
    const CdnStaticPngResult refreshed =
        cdn_media_pipeline_admit_static_png(&pipeline, key, not_modified, {}, byte_literal("\"png-1\""));
    const CdnStaticPngResult after_refresh = cdn_media_pipeline_lookup_static_png(&pipeline, key, 1'100);
    expect(stale.state == CdnStaticPngState::fetch_required && stale_etag_ok &&
               refreshed.state == CdnStaticPngState::decoded_cache_hit &&
               after_refresh.state == CdnStaticPngState::decoded_cache_hit &&
               bytes_equal(after_refresh.etag, byte_literal("\"png-2\"")),
           "cdn_pipeline_stale_conditional_and_304_refresh");

    const CdnResponseResult no_store =
        validate_static_image_response(CdnImageType::png, byte_literal("HTTP/1.1 200 OK\r\n"
                                                  "Content-Type: image/png\r\n"
                                                  "Transfer-Encoding: chunked\r\n"
                                                  "Cache-Control: no-store\r\n\r\n"),
                                     fixture.encoded_bytes, 0);
    const MediaAssetKey transient_key = media_asset_sticker(501, 1);
    const CdnStaticPngResult transient = cdn_media_pipeline_admit_static_png(
        &pipeline, transient_key, no_store, ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes});
    expect(transient.state == CdnStaticPngState::decoded_transient &&
               encoded_media_cache_find(&cache, transient_key, 0).state ==
                   EncodedMediaCacheLookupState::miss,
           "cdn_pipeline_no_store_decodes_without_publication");

    g_png_encoded[0] ^= 1;
    const MediaAssetKey corrupt_key = media_asset_sticker(502, 1);
    const CdnStaticPngResult corrupt = cdn_media_pipeline_admit_static_png(
        &pipeline, corrupt_key, accepted, ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes});
    g_png_encoded[0] ^= 1;
    expect(corrupt.state == CdnStaticPngState::decode_rejected &&
               encoded_media_cache_find(&cache, corrupt_key, 0).state == EncodedMediaCacheLookupState::miss,
           "cdn_pipeline_corrupt_png_never_enters_cache");

    expect(cdn_media_pipeline_lookup_static_png(&pipeline, media_asset_sticker(503, 2), 0).state ==
                   CdnStaticPngState::invalid &&
               cdn_media_pipeline_admit_static_png(&pipeline, media_asset_sticker(504, 1),
                                                   CdnResponseResult{}, {})
                       .state == CdnStaticPngState::http_rejected,
           "cdn_pipeline_static_png_and_http_contract");

    const CdnStaticPngResult wrong_validator = cdn_media_pipeline_admit_static_png(
        &pipeline, key, not_modified, {}, byte_literal("\"stale-other-request\""));
    expect(wrong_validator.state == CdnStaticPngState::uncorrelated_not_modified,
           "cdn_pipeline_304_exact_request_etag_correlation");

    const CdnResponseResult webp_policy =
        validate_static_image_response(CdnImageType::webp, byte_literal("HTTP/1.1 200 OK\r\n"
                                                   "Content-Type: image/webp\r\n"
                                                   "Content-Length: 62\r\n"
                                                   "ETag: \"webp-1\"\r\n"
                                                   "Cache-Control: max-age=60\r\n\r\n"),
                                      sizeof(kWebpGradient), 2'000);
    const MediaAssetKey webp_key = media_asset_custom_emoji(600, false);
    const CdnStaticWebpResult webp_admitted = cdn_media_pipeline_admit_static_webp(
        &pipeline, webp_key, webp_policy, ConstSlice<u8>{kWebpGradient, sizeof(kWebpGradient)});
    const u64 admitted_hash =
        hash64_bytes(kHash64Seed, webp_admitted.rgba8.data, webp_admitted.rgba8.count);
    const CdnStaticWebpResult webp_hit = cdn_media_pipeline_lookup_static_webp(&pipeline, webp_key, 2'010);
    expect(webp_admitted.state == CdnStaticWebpState::decoded_response && webp_admitted.store.stored() &&
               webp_admitted.image.width == 16 && webp_admitted.image.height == 16 &&
               admitted_hash == 0x0C28A89B429BABA5ull &&
               webp_hit.state == CdnStaticWebpState::decoded_cache_hit && webp_hit.decode.ok() &&
               hash64_bytes(kHash64Seed, webp_hit.rgba8.data, webp_hit.rgba8.count) ==
                   0x0C28A89B429BABA5ull &&
               bytes_equal(webp_hit.etag, byte_literal("\"webp-1\"")),
           "cdn_pipeline_vp8l_decode_before_cache_and_fresh_hit");

    const CdnResponseResult lossy_webp_policy =
        validate_static_image_response(CdnImageType::webp, byte_literal("HTTP/1.1 200 OK\r\n"
                                                   "Content-Type: image/webp\r\n"
                                                   "Content-Length: 92\r\n"
                                                   "ETag: \"webp-lossy-1\"\r\n"
                                                   "Cache-Control: max-age=60\r\n\r\n"),
                                      sizeof(kWebpLossy4x4), 2'000);
    const MediaAssetKey lossy_webp_key = media_asset_custom_emoji(603, false);
    const CdnStaticWebpResult lossy_webp_admitted = cdn_media_pipeline_admit_static_webp(
        &pipeline, lossy_webp_key, lossy_webp_policy, ConstSlice<u8>{kWebpLossy4x4, sizeof(kWebpLossy4x4)});
    const CdnStaticWebpResult lossy_webp_hit =
        cdn_media_pipeline_lookup_static_webp(&pipeline, lossy_webp_key, 2'010);
    expect(lossy_webp_admitted.state == CdnStaticWebpState::decoded_response &&
               lossy_webp_admitted.store.stored() && lossy_webp_admitted.lossy_decode.ok() &&
               lossy_webp_admitted.image.codec == WebpCodec::vp8_lossy &&
               lossy_webp_admitted.vp8_frame.width == 4 && lossy_webp_admitted.vp8_frame.height == 4 &&
               lossy_webp_admitted.rgba8.count == 0 && lossy_webp_admitted.alpha.count == 0 &&
               lossy_webp_hit.state == CdnStaticWebpState::decoded_cache_hit &&
               lossy_webp_hit.lossy_decode.ok() && lossy_webp_hit.vp8_header.loop_filter.level == 5 &&
               g_lossy_webp_workspace.modes.macroblock_count == 1 &&
               g_lossy_webp_workspace.coefficients.macroblock_count == 1 &&
               bytes_equal(lossy_webp_hit.etag, byte_literal("\"webp-lossy-1\"")),
           "cdn_pipeline_vp8_decode_before_cache_and_fresh_hit");

    u8 corrupt_webp[sizeof(kWebpOne)]{};
    copy_bytes(corrupt_webp, kWebpOne, sizeof(corrupt_webp));
    corrupt_webp[0] ^= 1;
    const CdnResponseResult corrupt_webp_policy =
        validate_static_image_response(CdnImageType::webp, byte_literal("HTTP/1.1 200 OK\r\n"
                                                   "Content-Type: image/webp\r\n"
                                                   "Content-Length: 32\r\n\r\n"),
                                      sizeof(corrupt_webp), 0);
    const MediaAssetKey corrupt_webp_key = media_asset_custom_emoji(601, false);
    const CdnStaticWebpResult rejected_webp =
        cdn_media_pipeline_admit_static_webp(&pipeline, corrupt_webp_key, corrupt_webp_policy,
                                             ConstSlice<u8>{corrupt_webp, sizeof(corrupt_webp)});
    expect(rejected_webp.state == CdnStaticWebpState::decode_rejected &&
               encoded_media_cache_find(&cache, corrupt_webp_key, 0).state ==
                   EncodedMediaCacheLookupState::miss &&
               cdn_media_pipeline_lookup_static_webp(&pipeline, media_asset_custom_emoji(602, true), 0)
                       .state == CdnStaticWebpState::invalid,
           "cdn_pipeline_corrupt_or_animated_webp_rejected");

    JpegFixtureResult jpeg_fixture{};
    expect(
        build_baseline_jpeg_fixture(Slice<u8>{g_png_encoded, sizeof(g_png_encoded)}, true, &jpeg_fixture),
        "cdn_pipeline_jpeg_fixture_build");
    const CdnResponseResult jpeg_policy =
        accepted_cdn_response(byte_literal("\"jpeg-1\""), 2'100, jpeg_fixture.encoded_bytes);
    const MediaAssetKey jpeg_key = media_asset_attachment(650);
    const CdnStaticJpegResult jpeg_requires_workspace = cdn_media_pipeline_admit_static_jpeg(
        &pipeline, jpeg_key, jpeg_policy, ConstSlice<u8>{g_png_encoded, jpeg_fixture.encoded_bytes});
    const bool jpeg_not_published_without_workspace =
        encoded_media_cache_find(&cache, jpeg_key, 2'000).state == EncodedMediaCacheLookupState::miss;
    expect(jpeg_requires_workspace.state == CdnStaticJpegState::jpeg_workspace_required &&
               jpeg_not_published_without_workspace,
           "cdn_pipeline_jpeg_lazy_workspace_before_publication");
    pipeline.jpeg_workspace = &g_jpeg_workspace;
    const CdnStaticJpegResult jpeg_admitted = cdn_media_pipeline_admit_static_jpeg(
        &pipeline, jpeg_key, jpeg_policy, ConstSlice<u8>{g_png_encoded, jpeg_fixture.encoded_bytes});
    const CdnStaticJpegResult jpeg_hit = cdn_media_pipeline_lookup_static_jpeg(&pipeline, jpeg_key, 2'010);
    expect(jpeg_admitted.state == CdnStaticJpegState::decoded_response && jpeg_admitted.store.stored() &&
               jpeg_admitted.decode.ok() && jpeg_admitted.image.width == 16 &&
               jpeg_admitted.image.height == 8 &&
               jpeg_admitted.coefficients == &g_jpeg_workspace.coefficients &&
               jpeg_hit.state == CdnStaticJpegState::decoded_cache_hit &&
               jpeg_hit.decode.decoded_blocks == 6 && jpeg_hit.coefficients->blocks[3].values[0] == -256 &&
               bytes_equal(jpeg_hit.etag, byte_literal("\"jpeg-1\"")),
           "cdn_pipeline_jpeg_decode_before_cache_and_fresh_hit");

    const MediaAssetKey progressive_key = media_asset_attachment(651);
    const CdnResponseResult progressive_policy = accepted_cdn_response(
        byte_literal("\"jpeg-progressive-1\""), 2'100, kJpegProgressive420ReferenceBytes);
    const CdnStaticJpegResult progressive = cdn_media_pipeline_admit_static_jpeg(
        &pipeline, progressive_key, progressive_policy,
        ConstSlice<u8>{reinterpret_cast<const u8*>(kJpegProgressive420Reference),
                       kJpegProgressive420ReferenceBytes});
    const CdnStaticJpegResult progressive_hit =
        cdn_media_pipeline_lookup_static_jpeg(&pipeline, progressive_key, 2'010);
    expect(progressive.state == CdnStaticJpegState::decoded_response && progressive.decode.ok() &&
               progressive.decode.parse.scans == 10 && progressive.decode.decoded_scans == 10 &&
               progressive.image.progressive && progressive.image.width == 16 &&
               progressive.image.height == 16 &&
               progressive_hit.state == CdnStaticJpegState::decoded_cache_hit &&
               progressive_hit.decode.decoded_scans == 10 &&
               bytes_equal(progressive_hit.etag, byte_literal("\"jpeg-progressive-1\"")),
           "cdn_pipeline_progressive_jpeg_decode_publish_and_hit");

    u8 malformed_progressive[kJpegProgressive420ReferenceBytes]{};
    for (u32 index = 0; index < sizeof(malformed_progressive); ++index) {
        malformed_progressive[index] = static_cast<u8>(kJpegProgressive420Reference[index]);
    }
    malformed_progressive[sizeof(malformed_progressive) - 3] ^= 1;
    const MediaAssetKey malformed_progressive_key = media_asset_attachment(652);
    const CdnStaticJpegResult malformed_progressive_result = cdn_media_pipeline_admit_static_jpeg(
        &pipeline, malformed_progressive_key, progressive_policy,
        ConstSlice<u8>{malformed_progressive, sizeof(malformed_progressive)});
    expect(malformed_progressive_result.state == CdnStaticJpegState::decode_rejected &&
               malformed_progressive_result.decode.error == JpegError::entropy &&
               encoded_media_cache_find(&cache, malformed_progressive_key, 2'010).state ==
                   EncodedMediaCacheLookupState::miss,
           "cdn_pipeline_progressive_jpeg_reject_before_publish");

    ApngFixtureResult apng_fixture{};
    expect(build_rgba8_apng_fixture(Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                    Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &apng_fixture),
           "cdn_pipeline_apng_fixture_build");
    const CdnResponseResult apng_policy =
        validate_static_image_response(CdnImageType::png, byte_literal("HTTP/1.1 200 OK\r\n"
                                                  "Content-Type: image/png\r\n"
                                                  "Transfer-Encoding: chunked\r\n"
                                                  "ETag: \"apng-1\"\r\n"
                                                  "Cache-Control: max-age=60\r\n\r\n"),
                                     apng_fixture.encoded_bytes, 3'000);
    const MediaAssetKey apng_key = media_asset_sticker(700, 2);
    const CdnApngResult apng_admitted = cdn_media_pipeline_admit_apng(
        &pipeline, apng_key, apng_policy, ConstSlice<u8>{g_png_encoded, apng_fixture.encoded_bytes});
    const CdnApngResult apng_frame_one =
        cdn_media_pipeline_decode_apng_frame(&pipeline, apng_key, apng_admitted.cache_generation, 1, 3'010);
    bool apng_frame_one_exact = apng_frame_one.decoded();
    for (u32 y = 0; y < 2 && apng_frame_one_exact; ++y) {
        const u8* row = apng_frame_one.scanlines.data + y * apng_frame_one.image.row_stride + 1;
        for (u32 x = 0; x < 2; ++x) {
            const u8* pixel = row + x * 4;
            const u32 actual = static_cast<u32>(pixel[0]) | static_cast<u32>(pixel[1]) << 8 |
                               static_cast<u32>(pixel[2]) << 16 | static_cast<u32>(pixel[3]) << 24;
            apng_frame_one_exact = apng_frame_one_exact && actual == apng_fixture_source_rgba(1, x, y);
        }
    }
    expect(apng_policy.ok() && apng_admitted.decoded() && apng_admitted.store.stored() &&
               apng_admitted.cache_generation != 0 && apng_admitted.frame_count == 4 &&
               apng_admitted.play_count == 2 && apng_admitted.canvas_width == 4 &&
               apng_admitted.canvas_height == 4 && apng_admitted.decode.frame_index == 0 &&
               apng_frame_one_exact && apng_frame_one.decode.frame_index == 1 &&
               apng_frame_one.decode.duration_us == 10'000,
           "cdn_pipeline_apng_admit_and_exact_generation_frame");
    const u16 old_generation = apng_admitted.cache_generation;
    const EncodedMediaCacheStoreResult replaced_apng = encoded_media_cache_store(
        &cache, apng_key, ConstSlice<u8>{g_png_encoded, apng_fixture.encoded_bytes},
        EncodedMediaCachePolicy{byte_literal("\"apng-2\""), 5'000, true, false});
    expect(replaced_apng.stored() && replaced_apng.generation != 0 &&
               replaced_apng.generation != old_generation &&
               cdn_media_pipeline_decode_apng_frame(&pipeline, apng_key, old_generation, 2, 3'020).state ==
                   CdnApngState::generation_mismatch &&
               cdn_media_pipeline_decode_apng_frame(&pipeline, apng_key, replaced_apng.generation, 3, 9'000)
                   .decoded() &&
               cdn_media_pipeline_decode_apng_frame(&pipeline, apng_key, replaced_apng.generation, 0, 9'000)
                   .decoded() &&
               cdn_media_pipeline_lookup_apng(&pipeline, media_asset_sticker(701, 1), 3'000).state ==
                   CdnApngState::invalid,
           "cdn_pipeline_apng_generation_and_stale_playback_contract");

    ApngFixtureResult excluded_fixture{};
    expect(build_rgba8_apng_default_fixture(Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                            Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &excluded_fixture),
           "cdn_pipeline_apng_excluded_default_fixture_build");
    const CdnResponseResult excluded_policy =
        validate_static_image_response(CdnImageType::png, byte_literal("HTTP/1.1 200 OK\r\n"
                                                  "Content-Type: image/png\r\n"
                                                  "Transfer-Encoding: chunked\r\n"
                                                  "Cache-Control: max-age=60\r\n\r\n"),
                                     excluded_fixture.encoded_bytes, 4'000);
    const u32 excluded_idat =
        test_find_png_chunk(ConstSlice<u8>{g_png_encoded, excluded_fixture.encoded_bytes}, 0x49444154u, 0);
    expect(excluded_idat != 0xFFFFFFFFu, "cdn_pipeline_apng_excluded_default_idat_offset");
    if (excluded_idat != 0xFFFFFFFFu) {
        g_png_encoded[excluded_idat + 8] ^= 1;
        test_rewrite_png_chunk_crc(Slice<u8>{g_png_encoded, excluded_fixture.encoded_bytes}, excluded_idat);
    }
    const MediaAssetKey excluded_key = media_asset_sticker(702, 2);
    const CdnApngResult excluded_rejected =
        cdn_media_pipeline_admit_apng(&pipeline, excluded_key, excluded_policy,
                                      ConstSlice<u8>{g_png_encoded, excluded_fixture.encoded_bytes});
    expect(excluded_policy.ok() && excluded_rejected.state == CdnApngState::decode_rejected &&
               encoded_media_cache_find(&cache, excluded_key, 4'000).state ==
                   EncodedMediaCacheLookupState::miss,
           "cdn_pipeline_apng_excluded_default_validated_before_cache");

    GifFixtureResult gif_fixture{};
    expect(build_indexed_gif_fixture(Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                     Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &gif_fixture),
           "cdn_pipeline_gif_fixture_build");
    const CdnResponseResult gif_policy =
        validate_static_image_response(CdnImageType::gif, byte_literal("HTTP/1.1 200 OK\r\n"
                                                  "Content-Type: image/gif\r\n"
                                                  "Transfer-Encoding: chunked\r\n"
                                                  "ETag: \"gif-1\"\r\n"
                                                  "Cache-Control: max-age=60\r\n\r\n"),
                                     gif_fixture.encoded_bytes, 5'000);
    const MediaAssetKey gif_key = media_asset_sticker(800, 4);
    const CdnGifResult gif_admitted = cdn_media_pipeline_admit_gif(
        &pipeline, gif_key, gif_policy, ConstSlice<u8>{g_png_encoded, gif_fixture.encoded_bytes});
    const CdnGifResult gif_frame_one =
        cdn_media_pipeline_decode_gif_frame(&pipeline, gif_key, gif_admitted.cache_generation, 1, 5'010);
    bool gif_frame_exact = gif_frame_one.decoded() && gif_frame_one.indices.count == 4;
    for (u32 index = 0; index < 4 && gif_frame_exact; ++index) {
        gif_frame_exact = gif_frame_one.indices[index] == gif_fixture_source_index(1, index % 2, index / 2);
    }
    expect(gif_policy.ok() && gif_admitted.decoded() && gif_admitted.store.stored() &&
               gif_admitted.cache_generation != 0 && gif_admitted.frame_count == 4 &&
               gif_admitted.play_count == 3 && gif_admitted.canvas_width == 4 &&
               gif_admitted.canvas_height == 4 && gif_admitted.decode.frame_index == 0 && gif_frame_exact &&
               gif_frame_one.decode.duration_us == 100'000,
           "cdn_pipeline_gif_admit_and_exact_generation_frame");
    const u16 gif_old_generation = gif_admitted.cache_generation;
    const EncodedMediaCacheStoreResult gif_replaced =
        encoded_media_cache_store(&cache, gif_key, ConstSlice<u8>{g_png_encoded, gif_fixture.encoded_bytes},
                                  EncodedMediaCachePolicy{byte_literal("\"gif-2\""), 7'000, true, false});
    expect(
        gif_replaced.stored() && gif_replaced.generation != gif_old_generation &&
            cdn_media_pipeline_decode_gif_frame(&pipeline, gif_key, gif_old_generation, 2, 5'020).state ==
                CdnGifState::generation_mismatch &&
            cdn_media_pipeline_decode_gif_frame(&pipeline, gif_key, gif_replaced.generation, 3, 9'000)
                .decoded() &&
            cdn_media_pipeline_decode_gif_frame(&pipeline, gif_key, gif_replaced.generation, 0, 9'000)
                .decoded() &&
            cdn_media_pipeline_lookup_gif(&pipeline, media_asset_sticker(801, 2), 5'000).state ==
                CdnGifState::invalid,
        "cdn_pipeline_gif_generation_and_stale_playback_contract");

    GifImage corrupt_gif_image{};
    pipeline.gif_content_identity = 0;
    expect(gif_parse(ConstSlice<u8>{g_png_encoded, gif_fixture.encoded_bytes}, &corrupt_gif_image,
                     &gif_workspace)
               .ok(),
           "cdn_pipeline_gif_corrupt_frame_offset");
    const u32 corrupt_payload = corrupt_gif_image.frames[3].data_offset + 1;
    g_png_encoded[corrupt_payload] = static_cast<u8>((g_png_encoded[corrupt_payload] & 0xF8u) | 1u);
    const MediaAssetKey corrupt_gif_key = media_asset_sticker(802, 4);
    const CdnGifResult corrupt_gif = cdn_media_pipeline_admit_gif(
        &pipeline, corrupt_gif_key, gif_policy, ConstSlice<u8>{g_png_encoded, gif_fixture.encoded_bytes});
    const CdnGifResult corrupt_later = cdn_media_pipeline_decode_gif_frame(
        &pipeline, corrupt_gif_key, corrupt_gif.cache_generation, 3, 5'000);
    expect(corrupt_gif.decoded() && corrupt_later.state == CdnGifState::decode_rejected && corrupt_later.decode.frame_index == 3 &&
               corrupt_later.decode.error == GifError::lzw_missing_clear &&
               !pipeline.gif_content_identity &&
               encoded_media_cache_find(&cache, corrupt_gif_key, 5'000).state ==
                   EncodedMediaCacheLookupState::miss,
           "cdn_pipeline_gif_later_frame_validated_on_demand_and_bad_cache_evicted");

    AnimatedWebpFixtureResult animated_fixture{};
    expect(build_lossless_animated_webp_fixture(Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                                &animated_fixture),
           "cdn_pipeline_animated_webp_fixture_build");
    const CdnResponseResult animated_policy =
        validate_static_image_response(CdnImageType::webp, byte_literal("HTTP/1.1 200 OK\r\n"
                                                   "Content-Type: image/webp\r\n"
                                                   "Transfer-Encoding: chunked\r\n"
                                                   "ETag: \"animated-webp-1\"\r\n"
                                                   "Cache-Control: max-age=60\r\n\r\n"),
                                      animated_fixture.encoded_bytes, 6'000);
    const MediaAssetKey animated_key = media_asset_custom_emoji(900, true);
    const CdnAnimatedWebpResult animated_admitted = cdn_media_pipeline_admit_animated_webp(
        &pipeline, animated_key, animated_policy,
        ConstSlice<u8>{g_png_encoded, animated_fixture.encoded_bytes});
    const CdnAnimatedWebpResult animated_frame_one = cdn_media_pipeline_decode_animated_webp_frame(
        &pipeline, animated_key, animated_admitted.cache_generation, 1, 6'010);
    bool animated_frame_exact = animated_frame_one.decoded() && animated_frame_one.rgba8.count == 16;
    for (u32 pixel = 0; pixel < 4 && animated_frame_exact; ++pixel) {
        const u8* rgba = animated_frame_one.rgba8.data + pixel * 4;
        const u32 actual = static_cast<u32>(rgba[0]) | static_cast<u32>(rgba[1]) << 8 |
                           static_cast<u32>(rgba[2]) << 16 | static_cast<u32>(rgba[3]) << 24;
        animated_frame_exact = actual == animated_webp_fixture_source_rgba(1);
    }
    expect(animated_policy.ok() && animated_admitted.decoded() && animated_admitted.store.stored() &&
               animated_admitted.cache_generation != 0 && animated_admitted.frame_count == 4 &&
               animated_admitted.play_count == 2 && animated_admitted.canvas_width == 4 &&
               animated_admitted.canvas_height == 4 && animated_admitted.background_rgba == 0x80302010u &&
               animated_admitted.decode.frame.blend == AnimatedWebpBlend::source && animated_frame_exact &&
               animated_frame_one.decode.duration_us == 10'000 &&
               animated_frame_one.decode.frame.dispose == AnimatedWebpDispose::background,
           "cdn_pipeline_animated_webp_all_frame_admit_and_generation");

    const u16 animated_old_generation = animated_admitted.cache_generation;
    const EncodedMediaCacheStoreResult animated_replaced = encoded_media_cache_store(
        &cache, animated_key, ConstSlice<u8>{g_png_encoded, animated_fixture.encoded_bytes},
        EncodedMediaCachePolicy{byte_literal("\"animated-webp-2\""), 8'000, true, false});
    expect(
        animated_replaced.stored() && animated_replaced.generation != animated_old_generation &&
            cdn_media_pipeline_decode_animated_webp_frame(&pipeline, animated_key, animated_old_generation,
                                                          2, 6'020)
                    .state == CdnAnimatedWebpState::generation_mismatch &&
            cdn_media_pipeline_decode_animated_webp_frame(&pipeline, animated_key,
                                                          animated_replaced.generation, 3, 9'000)
                .decoded() &&
            cdn_media_pipeline_decode_animated_webp_frame(&pipeline, animated_key,
                                                          animated_replaced.generation, 0, 9'000)
                .decoded() &&
            cdn_media_pipeline_lookup_animated_webp(&pipeline, media_asset_custom_emoji(901, false), 6'000)
                    .state == CdnAnimatedWebpState::invalid,
        "cdn_pipeline_animated_webp_replacement_and_stale_contract");

    AnimatedWebpImage corrupt_animation{};
    expect(animated_webp_parse(ConstSlice<u8>{g_png_encoded, animated_fixture.encoded_bytes},
                               &corrupt_animation, &animated_webp_workspace)
               .ok(),
           "cdn_pipeline_animated_webp_corrupt_frame_offset");
    const AnimatedWebpFrame corrupt_frame = corrupt_animation.frames[3];
    for (u32 index = 13; index < corrupt_frame.payload_bytes; ++index) {
        g_png_encoded[corrupt_frame.payload_offset + index] = 0xFF;
    }
    const MediaAssetKey corrupt_animated_key = media_asset_custom_emoji(902, true);
    const CdnAnimatedWebpResult corrupt_animated = cdn_media_pipeline_admit_animated_webp(
        &pipeline, corrupt_animated_key, animated_policy,
        ConstSlice<u8>{g_png_encoded, animated_fixture.encoded_bytes});
    const CdnAnimatedWebpResult corrupt_animated_later = cdn_media_pipeline_decode_animated_webp_frame(
        &pipeline, corrupt_animated_key, corrupt_animated.cache_generation, 3, 6'000);
    expect(corrupt_animated.decoded() && corrupt_animated.cache_generation != 0 &&
               corrupt_animated_later.state == CdnAnimatedWebpState::decode_rejected &&
               corrupt_animated_later.decode.error == AnimatedWebpError::frame_decode &&
               encoded_media_cache_find(&cache, corrupt_animated_key, 6'000).state ==
                   EncodedMediaCacheLookupState::miss,
           "cdn_pipeline_animated_webp_first_frame_ready_then_bad_later_frame_evicted");

    AnimatedWebpFixtureResult mixed_fixture{};
    expect(
        build_mixed_animated_webp_fixture(Slice<u8>{g_png_encoded, sizeof(g_png_encoded)}, &mixed_fixture),
        "cdn_pipeline_mixed_animated_webp_fixture_build");
    const CdnResponseResult mixed_policy =
        accepted_cdn_response(byte_literal("\"animated-webp-mixed\""), 8'000, mixed_fixture.encoded_bytes);
    const MediaAssetKey mixed_key = media_asset_custom_emoji(903, true);
    pipeline.lossy_webp_workspace = nullptr;
    const CdnAnimatedWebpResult mixed_requires_workspace = cdn_media_pipeline_admit_animated_webp(
        &pipeline, mixed_key, mixed_policy, ConstSlice<u8>{g_png_encoded, mixed_fixture.encoded_bytes});
    expect(mixed_requires_workspace.state == CdnAnimatedWebpState::lossy_workspace_required &&
               encoded_media_cache_find(&cache, mixed_key, 6'000).state ==
                   EncodedMediaCacheLookupState::miss,
           "cdn_pipeline_mixed_animated_webp_lazy_workspace");
    pipeline.lossy_webp_workspace = &g_lossy_webp_workspace;
    const CdnAnimatedWebpResult mixed_admitted = cdn_media_pipeline_admit_animated_webp(
        &pipeline, mixed_key, mixed_policy, ConstSlice<u8>{g_png_encoded, mixed_fixture.encoded_bytes});
    const CdnAnimatedWebpResult mixed_lossy = cdn_media_pipeline_decode_animated_webp_frame(
        &pipeline, mixed_key, mixed_admitted.cache_generation, 1, 6'010);
    const CdnAnimatedWebpResult mixed_alpha = cdn_media_pipeline_decode_animated_webp_frame(
        &pipeline, mixed_key, mixed_admitted.cache_generation, 2, 6'020);
    bool mixed_alpha_exact = mixed_alpha.alpha.count == 16;
    for (u32 index = 0; index < mixed_alpha.alpha.count; ++index) {
        mixed_alpha_exact = mixed_alpha_exact && mixed_alpha.alpha[index] == 128;
    }
    expect(mixed_admitted.decoded() && mixed_admitted.store.stored() &&
               mixed_admitted.decode.frame.codec == AnimatedWebpCodec::vp8l_lossless &&
               mixed_admitted.rgba8.count == 64 && mixed_lossy.decoded() &&
               mixed_lossy.decode.frame.codec == AnimatedWebpCodec::vp8_lossy &&
               mixed_lossy.vp8_frame.width == 4 && mixed_lossy.alpha.count == 0 && mixed_alpha.decoded() &&
               mixed_alpha.decode.lossy_decode.ok() && mixed_alpha.decode.frame.has_alpha &&
               mixed_alpha_exact,
           "cdn_pipeline_mixed_animated_webp_all_codec_frames");

    const u32 saved_scanline_capacity = pipeline.scanline_capacity;
    pipeline.scanline_capacity = 32;
    const MediaAssetKey constrained_key = media_asset_custom_emoji(904, true);
    const CdnAnimatedWebpResult constrained =
        cdn_media_pipeline_admit_animated_webp(&pipeline, constrained_key, mixed_policy,
                                               ConstSlice<u8>{g_png_encoded, mixed_fixture.encoded_bytes});
    pipeline.scanline_capacity = saved_scanline_capacity;
    expect(constrained.state == CdnAnimatedWebpState::decode_rejected &&
               constrained.decode.error == AnimatedWebpError::output_capacity &&
               encoded_media_cache_find(&cache, constrained_key, 6'000).state ==
                   EncodedMediaCacheLookupState::miss,
           "cdn_pipeline_mixed_animated_webp_scanline_capacity");
}

namespace {

u32 test_gif_delay_offset(ConstSlice<u8> encoded) noexcept {
    for (u32 index = 0; index + 8 <= encoded.count; ++index)
        if (encoded[index] == 0x21 && encoded[index + 1] == 0xF9 && encoded[index + 2] == 4)
            return index + 4;
    return ~0u;
}

} // namespace

void test_cdn_gif_metadata_reuse() noexcept {
    GifFixtureResult fixture{};
    if (!build_indexed_gif_fixture({g_png_encoded, sizeof(g_png_encoded)},
                                    {g_png_zlib, sizeof(g_png_zlib)}, &fixture)) {
        expect(false, "gif_metadata_fixture"); return;
    }
    const ConstSlice<u8> bytes{g_png_encoded, fixture.encoded_bytes};
    const u32 delay = test_gif_delay_offset(bytes);
    if (delay == ~0u) { expect(false, "gif_metadata_control"); return; }
    EncodedMediaCacheEntry entries[4]{}; u16 owners[4]{};
    u8 storage[4 * kEncodedMediaCachePageBytes]{}, output[64]{};
    u32 buckets[8]{};
    EncodedMediaCache cache{}; CdnMediaPipeline pipeline{}; GifWorkspace workspace{};
    if (!encoded_media_cache_initialize(&cache, {entries, 4}, {owners, 4}, {storage, sizeof(storage)}, {buckets, 8}) ||
        !cdn_media_pipeline_initialize(&pipeline, &cache, {output, sizeof(output)}) ||
        !cdn_media_pipeline_bind_gif_workspace(&pipeline, &workspace)) {
        expect(false, "gif_metadata_initialize"); return;
    }
    pipeline.gif_workspace = &workspace;
    const auto a = media_asset_sticker(88001, 4), b = media_asset_sticker(88002, 4);
    const auto alias = media_asset_sticker(88003, 4);
    const EncodedMediaCachePolicy policy{byte_literal("\"gif-reuse\""), 2000, true, false};
    auto response = validate_static_image_response(CdnImageType::gif, byte_literal("HTTP/1.1 200 OK\r\nContent-Type: image/gif\r\nTransfer-Encoding: chunked\r\n"
                     "ETag: \"gif-reuse\"\r\nCache-Control: max-age=60\r\n\r\n"), bytes.count, 1000);
    const auto admitted = cdn_media_pipeline_admit_gif(&pipeline, a, response, bytes);
    const bool response_unkeyed = admitted.decoded() && !pipeline.gif_content_identity;
    const auto first = cdn_media_pipeline_lookup_gif(&pipeline, a, 1000);
    const auto resident = encoded_media_cache_find(&cache, a, 1000);
    const u64 identity_a = resident.content_identity;
    const auto second = cdn_media_pipeline_decode_gif_frame(&pipeline, a, resident.generation, 1, 1000);
    bool exact = second.decoded() && second.indices.count == 4;
    for (u32 pixel = 0; pixel < second.indices.count && exact; ++pixel)
        exact = second.indices[pixel] == gif_fixture_source_index(1, pixel % 2, pixel / 2);
    expect(response_unkeyed && first.decoded() && identity_a && exact &&
               pipeline.gif_content_identity == identity_a && pipeline.gif_image.frames == workspace.frames &&
               first.decode.parse.blocks == second.decode.parse.blocks &&
               first.decode.parse.extensions == second.decode.parse.extensions &&
               first.decode.parse.data_subblocks == second.decode.parse.data_subblocks &&
               first.decode.parse.error == second.decode.parse.error,
           "gif_metadata_response_unkeyed_then_leased_frame_reuse");
    const auto alias_store = encoded_media_cache_store(&cache, alias, bytes, policy);
    const auto alias_lookup = encoded_media_cache_find(&cache, alias, 1000);
    const auto alias_frame = cdn_media_pipeline_decode_gif_frame(&pipeline, alias, alias_store.generation, 0, 1000);
    expect(alias_lookup.content_identity == identity_a && alias_lookup.bytes.data != resident.bytes.data &&
               alias_frame.decoded() && pipeline.gif_content_identity == identity_a,
           "gif_metadata_equal_content_alias_uses_current_lease_bytes");

    g_png_encoded[delay] = 7;
    const auto b_store = encoded_media_cache_store(&cache, b, bytes, policy);
    const auto b_lookup = encoded_media_cache_find(&cache, b, 1000);
    const auto b_frame = cdn_media_pipeline_decode_gif_frame(&pipeline, b, b_store.generation, 0, 1000);
    const bool switched_b = b_frame.decoded() && b_frame.decode.duration_us == 70000 &&
                            pipeline.gif_content_identity == b_lookup.content_identity;
    const auto back_a = cdn_media_pipeline_decode_gif_frame(&pipeline, a, resident.generation, 0, 1000);
    expect(b_store.generation == resident.generation && b_lookup.content_identity != identity_a &&
               switched_b && back_a.decoded() && back_a.decode.duration_us == 50000 &&
               pipeline.gif_content_identity == identity_a,
           "gif_metadata_interleaved_sources_do_not_use_generation_as_identity");
    g_png_encoded[delay] = 5;

    auto not_modified = response;
    not_modified.status_code = 304; not_modified.body_bytes = 0;
    not_modified.disposition = CdnResponseDisposition::not_modified;
    not_modified.expires_at_seconds = 3000;
    const auto refreshed = cdn_media_pipeline_admit_gif(&pipeline, a, not_modified, {}, policy.etag);
    expect(refreshed.decoded() && pipeline.gif_content_identity == identity_a &&
               refreshed.decode.duration_us == 50000,
           "gif_metadata_correlated_304_reuses_exact_leased_identity");
    not_modified.store_allowed = false;
    const auto forbidden = cdn_media_pipeline_admit_gif(&pipeline, a, not_modified, {}, policy.etag);
    expect(forbidden.decoded() && !forbidden.cache_generation && !pipeline.gif_content_identity &&
               !encoded_media_cache_find(&cache, a, 0).found(),
           "gif_metadata_store_forbidden_304_clears_identity");

    encoded_media_cache_erase(&cache, alias);
    encoded_media_cache_erase(&cache, b);
    const auto inserted = encoded_media_cache_store(&cache, a, bytes, policy);
    const auto inserted_lookup = encoded_media_cache_find(&cache, a, 0);
    const auto inserted_frame = cdn_media_pipeline_decode_gif_frame(&pipeline, a, inserted.generation, 0, 1000);
    const u64 old_identity = pipeline.gif_content_identity;
    encoded_media_cache_erase(&cache, a);
    g_png_encoded[delay] = 7;
    const auto reinserted = encoded_media_cache_store(&cache, a, bytes, policy);
    const auto reinserted_lookup = encoded_media_cache_find(&cache, a, 0);
    const auto reinserted_frame = cdn_media_pipeline_decode_gif_frame(&pipeline, a, reinserted.generation, 0, 1000);
    expect(inserted_frame.decoded() && old_identity == inserted_lookup.content_identity &&
               reinserted_lookup.content_identity != old_identity && reinserted_frame.decoded() &&
               reinserted_frame.decode.duration_us == 70000 &&
               pipeline.gif_content_identity == reinserted_lookup.content_identity,
           "gif_metadata_evicted_source_reinsert_never_reuses_old_metadata");
    response.store_allowed = false;
    g_png_encoded[delay] = 5;
    const auto transient = cdn_media_pipeline_admit_gif(&pipeline, b, response, bytes);
    expect(transient.decoded() && transient.state == CdnGifState::decoded_transient &&
               !pipeline.gif_content_identity && transient.decode.duration_us == 50000,
           "gif_metadata_transient_body_cannot_reuse_or_publish_identity");
    const auto hot_again = cdn_media_pipeline_decode_gif_frame(&pipeline, a, reinserted.generation, 0, 1000);
    response.store_allowed = true;
    const auto bad_body = cdn_media_pipeline_admit_gif(&pipeline, b, response,
                                                      {g_png_encoded, bytes.count - 1});
    expect(hot_again.decoded() && bad_body.state == CdnGifState::invalid &&
               pipeline.gif_content_identity == reinserted_lookup.content_identity,
           "gif_metadata_rejected_body_without_parser_keeps_workspace_valid");
    response.body_bytes = bytes.count - 1;
    const auto malformed = cdn_media_pipeline_admit_gif(&pipeline, b, response,
                                                       {g_png_encoded, bytes.count - 1});
    expect(malformed.state == CdnGifState::decode_rejected && !pipeline.gif_content_identity,
           "gif_metadata_failed_parse_clears_partly_overwritten_workspace");
    const auto recovered = cdn_media_pipeline_decode_gif_frame(&pipeline, a, reinserted.generation, 0, 1000);
    pipeline.scanline_capacity = 1;
    const auto failed_decode = cdn_media_pipeline_decode_gif_frame(&pipeline, a, reinserted.generation, 0, 1000);
    expect(recovered.decoded() && failed_decode.state == CdnGifState::decode_rejected &&
               failed_decode.decode.error == GifError::output_capacity && !pipeline.gif_content_identity &&
               !encoded_media_cache_find(&cache, a, 0).found(),
           "gif_metadata_warm_decode_failure_clears_identity_and_erases_encoded_source");
    pipeline.scanline_capacity = sizeof(output);
    const auto retry_store = encoded_media_cache_store(&cache, a, bytes, policy);
    const auto retry = cdn_media_pipeline_decode_gif_frame(&pipeline, a, retry_store.generation, 0, 1000);
    const auto invalid_frame = cdn_media_pipeline_decode_gif_frame(&pipeline, a, retry_store.generation, 4, 1000);
    expect(retry.decoded() && invalid_frame.state == CdnGifState::decode_rejected &&
               !pipeline.gif_content_identity && !encoded_media_cache_find(&cache, a, 0).found(),
           "gif_metadata_invalid_frame_clears_retained_state_and_preserves_cache_erase");
}

void test_account_media_cached_frame() noexcept {
    PngFixtureResult fixture{};
    expect(build_rgba8_png_fixture(32, 32, Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                   Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &fixture),
           "account_media_png_fixture_build");
    AccountMedia media{};
    const AccountMediaRequestResult initialized = account_media_initialize(&media);
    expect(initialized.ok() && media.started && media.allocation && !media.client_started &&
               !media.webp_workspace && !media.jpeg_workspace &&
               account_media_notification_handle(&media) == nullptr && media.owns_cache,
           "account_media_account_scoped_initialize");
    if (!media.started) {
        return;
    }
    const MediaAssetKey key = media_asset_sticker(8'000, 1);
    const CdnResponseResult policy{byte_literal("\"account-media\""),
                                   {},
                                   2'000,
                                   fixture.encoded_bytes,
                                   200,
                                   CdnResponseDisposition::accept_body,
                                   CdnHttpError::none,
                                   true,
                                   false};
    const CdnStaticPngResult admitted = cdn_media_pipeline_admit_static_png(
        &media.pipeline, key, policy, ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes});
    const AccountMediaRequestResult requested =
        account_media_request_static_asset(&media, key, 1'000, 1'100);
    AccountMediaFrame frame{};
    const bool taken = account_media_take_frame(&media, &frame);
    expect(admitted.decoded() && requested.ok() && requested.state == AccountMediaRequestState::ready &&
               taken && frame.image && frame.image->width == 32 && frame.image->height == 32 &&
               frame.bytes.count == fixture.filtered_bytes &&
               frame.kind == AccountMediaFrameKind::png_scanlines && frame.width == 32 &&
               frame.height == 32 && frame.row_pitch == frame.image->row_stride &&
               media_asset_key_equal(frame.key, key) && frame.request_id == 0 && !media.client_started &&
               account_media_pump(&media, 1'001, 1'101),
           "account_media_cache_hit_borrowed_gpu_frame");

    const MediaAssetKey emoji_key = media_asset_custom_emoji(8'001, false);
    const EncodedMediaCacheStoreResult emoji_stored = encoded_media_cache_store(
        &media.cache, emoji_key, ConstSlice<u8>{kWebpGradient, sizeof(kWebpGradient)},
        EncodedMediaCachePolicy{byte_literal("\"account-webp\""), 2'000, true, false});
    const AccountMediaRequestResult emoji_requested =
        account_media_request_static_asset(&media, emoji_key, 1'002, 1'102);
    AccountMediaFrame emoji_frame{};
    const bool emoji_taken = account_media_take_frame(&media, &emoji_frame);
    expect(emoji_stored.stored() && emoji_requested.ok() &&
               emoji_requested.state == AccountMediaRequestState::ready && media.webp_workspace &&
               !media.lossy_webp_workspace && emoji_taken && !emoji_frame.image &&
               emoji_frame.kind == AccountMediaFrameKind::straight_rgba8 && emoji_frame.width == 16 &&
               emoji_frame.height == 16 && emoji_frame.row_pitch == 64 &&
               hash64_bytes(kHash64Seed, emoji_frame.bytes.data, emoji_frame.bytes.count) ==
                   0x0C28A89B429BABA5ull &&
               media_asset_key_equal(emoji_frame.key, emoji_key) && !media.client_started,
           "account_media_lazy_vp8l_cache_hit_frame");

    constexpr Utf8View guild_icon_hash = utf8_literal("0123456789abcdef0123456789abcdef");
    const MediaAssetKey guild_icon_key =
        media_asset_guild_icon(8'009, cdn_guild_icon_variant(guild_icon_hash), false);
    const EncodedMediaCacheStoreResult guild_icon_stored = encoded_media_cache_store(
        &media.cache, guild_icon_key, ConstSlice<u8>{kWebpGradient, sizeof(kWebpGradient)},
        EncodedMediaCachePolicy{byte_literal("\"account-guild-webp\""), 2'000, true, false});
    const AccountMediaGuildIconRequest guild_icon_request{guild_icon_hash, guild_icon_key, {0, 0}};
    const AccountMediaRequestResult guild_icon_requested =
        account_media_request_guild_icon(&media, guild_icon_request, 1'002, 1'102);
    AccountMediaFrame guild_icon_frame{};
    const bool guild_icon_taken = account_media_take_frame(&media, &guild_icon_frame);
    expect(guild_icon_stored.stored(), "account_media_guild_icon_cache_store");
    expect(guild_icon_requested.ok() && guild_icon_requested.state == AccountMediaRequestState::ready,
           "account_media_guild_icon_cache_request");
    expect(guild_icon_taken && guild_icon_frame.kind == AccountMediaFrameKind::straight_rgba8 &&
               media_asset_key_equal(guild_icon_frame.key, guild_icon_key),
           "account_media_guild_icon_cache_hit_frame");
    // The rail's still of an a_ icon: a static key naming the animated hash
    // (the CDN's .webp without animated=true); an animated key still needs a_.
    constexpr Utf8View animated_icon_hash = utf8_literal("a_0123456789abcdef0123456789abcdef");
    const MediaAssetKey still_icon_key =
        media_asset_guild_icon(8'011, cdn_guild_icon_variant(animated_icon_hash), false);
    const EncodedMediaCacheStoreResult still_icon_stored = encoded_media_cache_store(
        &media.cache, still_icon_key, ConstSlice<u8>{kWebpGradient, sizeof(kWebpGradient)},
        EncodedMediaCachePolicy{byte_literal("\"account-guild-still\""), 2'000, true, false});
    const AccountMediaRequestResult still_icon_requested = account_media_request_guild_icon(
        &media, AccountMediaGuildIconRequest{animated_icon_hash, still_icon_key, {0, 0}}, 1'002, 1'102);
    AccountMediaFrame still_icon_frame{};
    const bool still_icon_taken = account_media_take_frame(&media, &still_icon_frame);
    const AccountMediaRequestResult mismatched_icon = account_media_request_guild_icon(
        &media,
        AccountMediaGuildIconRequest{guild_icon_hash,
                                     media_asset_guild_icon(8'012, cdn_guild_icon_variant(guild_icon_hash), true),
                                     {0, 0}},
        1'002, 1'102);
    expect(still_icon_stored.stored() && still_icon_requested.state == AccountMediaRequestState::ready &&
               still_icon_taken && media_asset_key_equal(still_icon_frame.key, still_icon_key) &&
               mismatched_icon.state == AccountMediaRequestState::failed,
           "account_media_guild_icon_still_key_accepts_animated_hash");

    const MediaAssetKey avatar_key =
        media_asset_user_avatar(8'010, user_avatar_variant(guild_icon_hash), false);
    const EncodedMediaCacheStoreResult avatar_stored = encoded_media_cache_store(
        &media.cache, avatar_key, ConstSlice<u8>{kWebpGradient, sizeof(kWebpGradient)},
        EncodedMediaCachePolicy{byte_literal("\"account-avatar-webp\""), 2'000, true, false});
    const AccountMediaGuildIconRequest avatar_request{guild_icon_hash, avatar_key, {0, 0}};
    const AccountMediaRequestResult avatar_requested =
        account_media_request_guild_icon(&media, avatar_request, 1'002, 1'102);
    AccountMediaFrame avatar_frame{};
    const bool avatar_taken = account_media_take_frame(&media, &avatar_frame);
    expect(avatar_stored.stored() && avatar_requested.ok() &&
               avatar_requested.state == AccountMediaRequestState::ready && avatar_taken &&
               avatar_frame.kind == AccountMediaFrameKind::straight_rgba8 &&
               media_asset_key_equal(avatar_frame.key, avatar_key),
           "account_media_user_avatar_cache_hit_frame");

    const MediaAssetKey group_icon_key =
        media_asset_channel_icon(8'011, user_avatar_variant(guild_icon_hash), false);
    const EncodedMediaCacheStoreResult group_icon_stored = encoded_media_cache_store(
        &media.cache, group_icon_key, ConstSlice<u8>{kWebpGradient, sizeof(kWebpGradient)},
        EncodedMediaCachePolicy{byte_literal("\"account-group-icon-webp\""), 2'000, true, false});
    const AccountMediaGuildIconRequest group_icon_bad_size{guild_icon_hash, group_icon_key, {48, 0}};
    const AccountMediaGuildIconRequest group_icon_request{guild_icon_hash, group_icon_key, {64, 0}};
    const AccountMediaRequestResult group_icon_rejected =
        account_media_request_guild_icon(&media, group_icon_bad_size, 1'002, 1'102);
    const AccountMediaRequestResult group_icon_requested =
        account_media_request_guild_icon(&media, group_icon_request, 1'002, 1'102);
    AccountMediaFrame group_icon_frame{};
    const bool group_icon_taken = account_media_take_frame(&media, &group_icon_frame);
    expect(group_icon_stored.stored() && group_icon_rejected.state == AccountMediaRequestState::invalid &&
               group_icon_requested.ok() && group_icon_requested.state == AccountMediaRequestState::ready &&
               group_icon_taken && group_icon_frame.kind == AccountMediaFrameKind::straight_rgba8 &&
               media_asset_key_equal(group_icon_frame.key, group_icon_key),
           "account_media_group_dm_icon_sized_request_and_cache_hit_frame");

    const MediaAssetKey lossy_emoji_key = media_asset_custom_emoji(8'005, false);
    const EncodedMediaCacheStoreResult lossy_emoji_stored = encoded_media_cache_store(
        &media.cache, lossy_emoji_key, ConstSlice<u8>{kWebpLossy4x4, sizeof(kWebpLossy4x4)},
        EncodedMediaCachePolicy{byte_literal("\"account-webp-lossy\""), 2'000, true, false});
    const AccountMediaRequestResult lossy_emoji_requested =
        account_media_request_static_asset(&media, lossy_emoji_key, 1'003, 1'103);
    AccountMediaFrame lossy_emoji_frame{};
    const bool lossy_emoji_taken = account_media_take_frame(&media, &lossy_emoji_frame);
    expect(lossy_emoji_stored.stored() && lossy_emoji_requested.ok() &&
               lossy_emoji_requested.state == AccountMediaRequestState::ready &&
               media.lossy_webp_workspace && lossy_emoji_taken &&
               lossy_emoji_frame.kind == AccountMediaFrameKind::vp8_keyframe &&
               lossy_emoji_frame.vp8_frame && lossy_emoji_frame.vp8_header &&
               lossy_emoji_frame.vp8_modes == &media.lossy_webp_workspace->modes &&
               lossy_emoji_frame.vp8_coefficients == &media.lossy_webp_workspace->coefficients &&
               lossy_emoji_frame.vp8_frame->width == 4 && lossy_emoji_frame.vp8_frame->height == 4 &&
               lossy_emoji_frame.bytes.count == 0 && lossy_emoji_frame.width == 4 &&
               lossy_emoji_frame.height == 4 && lossy_emoji_frame.row_pitch == 4 && !media.client_started,
           "account_media_lazy_vp8_cache_hit_gpu_records");

    const MediaAssetKey png_attachment_key =
        media_asset_attachment(8'008, MediaAttachmentFormat::png, MediaResolutionTier::px256);
    const EncodedMediaCacheStoreResult png_attachment_stored = encoded_media_cache_store(
        &media.cache, png_attachment_key, ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes},
        EncodedMediaCachePolicy{byte_literal("\"account-attachment-png\""), 2'000, true, false});
    const AccountMediaAttachmentRequest png_attachment{
        utf8_literal("https://cdn.discordapp.com/attachments/99/8008/a.webp"
                     "?ex=1000&is=900&hm="
                     "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"),
        png_attachment_key,
        7'003,
        10,
        0,
        0,
        CdnImageType::png,
        {0, 0, 0}};
    const AccountMediaRequestResult png_attachment_requested =
        account_media_request_attachment(&media, png_attachment, 1'004, 1'104);
    AccountMediaFrame png_attachment_frame{};
    const bool png_attachment_taken = account_media_take_frame(&media, &png_attachment_frame);
    expect(png_attachment_stored.stored() && png_attachment_requested.ok() &&
               png_attachment_requested.state == AccountMediaRequestState::ready && png_attachment_taken &&
               png_attachment_frame.kind == AccountMediaFrameKind::png_scanlines &&
               png_attachment_frame.image && png_attachment_frame.width == 32 &&
               png_attachment_frame.height == 32 && png_attachment_frame.source_message_id == 7'003 &&
               png_attachment_frame.source_message_revision == 10 && !media.client_started,
           "account_media_png_attachment_cache_hit_preserves_owner");

    const MediaAssetKey png_embed_key =
        media_asset_embed(7'005, 12, 0, MediaEmbedResource::thumbnail, MediaAttachmentFormat::png, false,
                          MediaResolutionTier::px512);
    const EncodedMediaCacheStoreResult png_embed_stored = encoded_media_cache_store(
        &media.cache, png_embed_key, ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes},
        EncodedMediaCachePolicy{byte_literal("\"account-embed-png\""), 2'000, true, false});
    const AccountMediaAttachmentRequest png_embed{
        utf8_literal("https://cdn.discordapp.com/avatars/12/hash.png?size=1024"),
        png_embed_key,
        7'005,
        12,
        512,
        512,
        CdnImageType::png,
        {0, 0, 0}};
    const AccountMediaRequestResult png_embed_requested =
        account_media_request_attachment(&media, png_embed, 1'004, 1'104);
    AccountMediaFrame png_embed_frame{};
    const bool png_embed_taken = account_media_take_frame(&media, &png_embed_frame);
    expect(png_embed_stored.stored() && png_embed_requested.ok() &&
               png_embed_requested.state == AccountMediaRequestState::ready && png_embed_taken &&
               media_asset_key_equal(png_embed_frame.key, png_embed_key) &&
               png_embed_frame.source_message_id == 7'005 && png_embed_frame.source_message_revision == 12,
           "account_media_embed_png_key_reaches_static_decoder");

    const MediaAssetKey rejected_proxy_key =
        media_asset_embed(7'006, 13, 0, MediaEmbedResource::image, MediaAttachmentFormat::png);
    const AccountMediaAttachmentRequest rejected_proxy{
        utf8_literal("https://example.com/external/token/https/"
                     "cdn.discordapp.com/avatars/12/hash.png"),
        rejected_proxy_key,
        7'006,
        13,
        512,
        512,
        CdnImageType::png,
        {0, 0, 0}};
    const AccountMediaRequestResult rejected_proxy_first =
        account_media_request_attachment(&media, rejected_proxy, 1'006, 1'106);
    const AccountMediaRequestResult rejected_proxy_repeat =
        account_media_request_attachment(&media, rejected_proxy, 1'007, 1'107);
    expect(rejected_proxy_first.state == AccountMediaRequestState::failed &&
               rejected_proxy_first.error == AccountMediaError::target &&
               rejected_proxy_first.media_url_error == MediaUrlError::invalid_host &&
               rejected_proxy_first.retry_at_ms != 0 &&
               rejected_proxy_repeat.state == AccountMediaRequestState::retry_wait &&
               rejected_proxy_repeat.retry_at_ms == rejected_proxy_first.retry_at_ms,
           "account_media_invalid_embed_proxy_is_diagnosed_and_rate_limited");

    const MediaAssetKey webp_attachment_key = media_asset_attachment(8'009, MediaAttachmentFormat::webp);
    const EncodedMediaCacheStoreResult webp_attachment_stored = encoded_media_cache_store(
        &media.cache, webp_attachment_key, ConstSlice<u8>{kWebpLossy4x4, sizeof(kWebpLossy4x4)},
        EncodedMediaCachePolicy{byte_literal("\"account-attachment-webp\""), 2'000, true, false});
    const AccountMediaAttachmentRequest webp_attachment{
        utf8_literal("https://cdn.discordapp.com/attachments/99/8009/a.webp"
                     "?ex=1000&is=900&hm="
                     "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"),
        webp_attachment_key,
        7'004,
        11,
        0,
        0,
        CdnImageType::webp,
        {0, 0, 0}};
    const AccountMediaRequestResult webp_attachment_requested =
        account_media_request_attachment(&media, webp_attachment, 1'004, 1'104);
    AccountMediaFrame webp_attachment_frame{};
    const bool webp_attachment_taken = account_media_take_frame(&media, &webp_attachment_frame);
    expect(webp_attachment_stored.stored() && webp_attachment_requested.ok() &&
               webp_attachment_requested.state == AccountMediaRequestState::ready &&
               webp_attachment_taken && webp_attachment_frame.kind == AccountMediaFrameKind::vp8_keyframe &&
               webp_attachment_frame.width == 4 && webp_attachment_frame.height == 4 &&
               webp_attachment_frame.source_message_id == 7'004 &&
               webp_attachment_frame.source_message_revision == 11 && !media.client_started,
           "account_media_webp_attachment_cache_hit_preserves_owner");

    const MediaAssetKey uncached_jpeg_key = media_asset_attachment(8'006);
    const AccountMediaRequestResult uncached_jpeg =
        account_media_request_static_asset(&media, uncached_jpeg_key, 1'004, 1'104);
    const AccountMediaAttachmentRequest expired_attachment{
        utf8_literal("https://cdn.discordapp.com/attachments/99/8006/a.jpg"
                     "?ex=400&is=300&hm="
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"),
        uncached_jpeg_key,
        7'001,
        3,
        0,
        0,
        CdnImageType::jpeg,
        {0, 0, 0}};
    const AccountMediaRequestResult expired_jpeg =
        account_media_request_attachment(&media, expired_attachment, 1'004, 1'104);
    const AccountMediaAttachmentRequest unsigned_refresh_attachment{
        utf8_literal("https://cdn.discordapp.com/attachments/99/8006/a.jpg"),
        uncached_jpeg_key,
        7'001,
        3,
        0,
        0,
        CdnImageType::jpeg,
        {0, 0, 0}};
    const AccountMediaRequestResult unsigned_refresh =
        account_media_request_attachment(&media, unsigned_refresh_attachment, 1'004, 1'104);
    AccountMediaAttachmentRequest mismatched_attachment = expired_attachment;
    mismatched_attachment.signed_url =
        utf8_literal("https://cdn.discordapp.com/attachments/99/8007/a.jpg"
                     "?ex=800&is=700&hm="
                     "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
    const AccountMediaRequestResult mismatched_jpeg =
        account_media_request_attachment(&media, mismatched_attachment, 1'004, 1'104);
    AccountMediaAttachmentRequest mismatched_format = expired_attachment;
    mismatched_format.image_type = CdnImageType::png;
    const AccountMediaRequestResult mismatched_format_result =
        account_media_request_attachment(&media, mismatched_format, 1'004, 1'104);
    expect(uncached_jpeg.state == AccountMediaRequestState::unsupported && expired_jpeg.ok() &&
               expired_jpeg.state == AccountMediaRequestState::refresh_required && unsigned_refresh.ok() &&
               unsigned_refresh.state == AccountMediaRequestState::refresh_required &&
               mismatched_jpeg.state == AccountMediaRequestState::failed &&
               mismatched_jpeg.error == AccountMediaError::target &&
               mismatched_jpeg.attachment_url_error == AttachmentUrlError::attachment_mismatch &&
               mismatched_format_result.state == AccountMediaRequestState::invalid &&
               !media.jpeg_workspace && !media.client_started,
           "account_media_attachment_miss_requires_signed_url_owner");

    const MediaAssetKey large_jpeg_key =
        media_asset_attachment(8'010, MediaAttachmentFormat::jpeg, MediaResolutionTier::px1024);
    static u8 large_jpeg_encoded[64 * 1024];
    JpegFixtureResult large_jpeg_fixture{};
    const bool large_jpeg_built =
        build_marked_jpeg_fixture({large_jpeg_encoded, sizeof(large_jpeg_encoded)}, 1'024, 576, &large_jpeg_fixture);
    const EncodedMediaCacheStoreResult large_jpeg_stored = encoded_media_cache_store(
        &media.cache, large_jpeg_key, ConstSlice<u8>{large_jpeg_encoded, large_jpeg_fixture.encoded_bytes},
        EncodedMediaCachePolicy{byte_literal("\"account-attachment-large-jpeg\""), 2'000, true, false});
    AccountMediaAttachmentRequest large_jpeg{
        utf8_literal("https://cdn.discordapp.com/attachments/99/8010/a.jpg"
                     "?ex=1000&is=900&hm="
                     "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee"),
        large_jpeg_key,
        7'007,
        14,
        1'024,
        576,
        CdnImageType::jpeg,
        {0, 0, 0}};
    const AccountMediaRequestResult large_jpeg_requested =
        account_media_request_attachment(&media, large_jpeg, 1'004, 1'104);
    AccountMediaFrame large_jpeg_frame{};
    const bool large_jpeg_taken = account_media_take_frame(&media, &large_jpeg_frame);
    large_jpeg.image_type = CdnImageType::png;
    const AccountMediaRequestResult jpeg_as_png =
        account_media_request_attachment(&media, large_jpeg, 1'004, 1'104);
    expect(large_jpeg_built && large_jpeg_stored.stored() &&
               large_jpeg_requested.state == AccountMediaRequestState::ready && large_jpeg_taken &&
               large_jpeg_frame.kind == AccountMediaFrameKind::straight_rgba8 && large_jpeg_frame.width == 1'024 &&
               large_jpeg_frame.height == 576 && media_asset_key_equal(large_jpeg_frame.key, large_jpeg_key) &&
               jpeg_as_png.state == AccountMediaRequestState::invalid,
           "account_media_large_jpeg_rendition_decodes_on_the_cpu");

    JpegFixtureResult jpeg_fixture{};
    expect(
        build_baseline_jpeg_fixture(Slice<u8>{g_png_encoded, sizeof(g_png_encoded)}, true, &jpeg_fixture),
        "account_media_jpeg_fixture_build");
    const MediaAssetKey jpeg_key = media_asset_attachment(8'007);
    const EncodedMediaCacheStoreResult jpeg_stored = encoded_media_cache_store(
        &media.cache, jpeg_key, ConstSlice<u8>{g_png_encoded, jpeg_fixture.encoded_bytes},
        EncodedMediaCachePolicy{byte_literal("\"account-jpeg\""), 2'000, true, false});
    const AccountMediaAttachmentRequest cached_attachment{
        utf8_literal("https://cdn.discordapp.com/attachments/99/8007/a.jpg"
                     "?ex=1000&is=900&hm="
                     "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"),
        jpeg_key,
        7'002,
        9,
        0,
        0,
        CdnImageType::jpeg,
        {0, 0, 0}};
    const AccountMediaRequestResult jpeg_requested =
        account_media_request_attachment(&media, cached_attachment, 1'005, 1'105);
    AccountMediaFrame jpeg_frame{};
    const bool jpeg_taken = account_media_take_frame(&media, &jpeg_frame);
    expect(
        jpeg_stored.stored() && jpeg_requested.ok() &&
            jpeg_requested.state == AccountMediaRequestState::ready && media.jpeg_workspace &&
            jpeg_taken && !jpeg_frame.image && jpeg_frame.jpeg_image &&
            jpeg_frame.jpeg_coefficients == &media.jpeg_workspace->coefficients &&
            jpeg_frame.kind == AccountMediaFrameKind::jpeg_coefficients && jpeg_frame.width == 16 &&
            jpeg_frame.height == 8 && jpeg_frame.row_pitch == 64 && jpeg_frame.source_message_id == 7'002 &&
            jpeg_frame.source_message_revision == 9 && jpeg_frame.bytes.count == 0 &&
            jpeg_frame.jpeg_image->tables == &media.jpeg_workspace->tables &&
            jpeg_frame.jpeg_coefficients->blocks[3].values[0] == -256 && !media.client_started,
        "account_media_lazy_jpeg_cache_hit_gpu_records");
    ApngFixtureResult apng_fixture{};
    expect(build_rgba8_apng_fixture(Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                    Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &apng_fixture),
           "account_media_apng_fixture_build");
    const MediaAssetKey apng_key = media_asset_sticker(8'002, 2);
    const EncodedMediaCacheStoreResult apng_stored = encoded_media_cache_store(
        &media.cache, apng_key, ConstSlice<u8>{g_png_encoded, apng_fixture.encoded_bytes},
        EncodedMediaCachePolicy{byte_literal("\"account-apng\""), 2'000, true, false});
    const AccountMediaRequestResult apng_requested =
        account_media_request_static_asset(&media, apng_key, 1'004, 1'104);
    AccountMediaFrame apng_frame_zero{};
    const bool apng_zero_taken = account_media_take_frame(&media, &apng_frame_zero);
    const AccountMediaRequestResult apng_advanced =
        account_media_advance_animation(&media, apng_key, apng_stored.generation, 1, 1'104);
    AccountMediaFrame apng_frame_one{};
    const bool apng_one_taken = account_media_take_frame(&media, &apng_frame_one);
    const AccountMediaRequestResult apng_looped =
        account_media_advance_animation(&media, apng_key, apng_stored.generation, 0, 1'104);
    AccountMediaFrame apng_loop_frame{};
    const bool apng_loop_taken = account_media_take_frame(&media, &apng_loop_frame);
    expect(apng_stored.stored() && apng_requested.ok() &&
               apng_requested.state == AccountMediaRequestState::ready && media.apng_workspace &&
               apng_zero_taken && apng_frame_zero.image &&
               apng_frame_zero.kind == AccountMediaFrameKind::apng_scanlines &&
               apng_frame_zero.frame_index == 0 && apng_frame_zero.frame_count == 4 &&
               apng_frame_zero.play_count == 2 &&
               apng_frame_zero.cache_generation == apng_stored.generation &&
               apng_frame_zero.canvas_width == 4 && apng_frame_zero.canvas_height == 4 &&
               apng_advanced.ok() && apng_advanced.state == AccountMediaRequestState::ready &&
               apng_one_taken && apng_frame_one.frame_index == 1 && apng_frame_one.duration_us == 10'000 &&
               apng_frame_one.apng_frame.blend == ApngBlend::over &&
               apng_frame_one.apng_frame.dispose == ApngDispose::previous && apng_looped.ok() &&
               apng_loop_taken && apng_loop_frame.frame_index == 0,
           "account_media_apng_cache_generation_advance");

    GifFixtureResult gif_fixture{};
    expect(build_indexed_gif_fixture(Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                     Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &gif_fixture),
           "account_media_gif_fixture_build");
    const MediaAssetKey gif_key = media_asset_sticker(8'003, 4);
    const EncodedMediaCacheStoreResult gif_stored = encoded_media_cache_store(
        &media.cache, gif_key, ConstSlice<u8>{g_png_encoded, gif_fixture.encoded_bytes},
        EncodedMediaCachePolicy{byte_literal("\"account-gif\""), 2'000, true, false});
    const AccountMediaRequestResult gif_requested =
        account_media_request_static_asset(&media, gif_key, 1'004, 1'105);
    AccountMediaFrame gif_frame_zero{};
    const bool gif_zero_taken = account_media_take_frame(&media, &gif_frame_zero);
    const AccountMediaRequestResult gif_advanced =
        account_media_advance_animation(&media, gif_key, gif_stored.generation, 1, 1'106);
    AccountMediaFrame gif_frame_one{};
    const bool gif_one_taken = account_media_take_frame(&media, &gif_frame_one);
    const AccountMediaRequestResult gif_looped =
        account_media_advance_animation(&media, gif_key, gif_stored.generation, 0, 1'106);
    AccountMediaFrame gif_loop_frame{};
    const bool gif_loop_taken = account_media_take_frame(&media, &gif_loop_frame);
    expect(gif_stored.stored() && gif_requested.ok() &&
               gif_requested.state == AccountMediaRequestState::ready && media.gif_workspace &&
               gif_zero_taken && !gif_frame_zero.image && gif_frame_zero.gif_palette &&
               gif_frame_zero.kind == AccountMediaFrameKind::gif_indices &&
               gif_frame_zero.frame_index == 0 && gif_frame_zero.frame_count == 4 &&
               gif_frame_zero.play_count == 3 && gif_frame_zero.cache_generation == gif_stored.generation &&
               gif_frame_zero.canvas_width == 4 && gif_frame_zero.canvas_height == 4 &&
               gif_frame_zero.row_pitch == 4 && gif_advanced.ok() &&
               gif_advanced.state == AccountMediaRequestState::ready && gif_one_taken &&
               gif_frame_one.gif_palette && gif_frame_one.frame_index == 1 &&
               gif_frame_one.duration_us == 100'000 &&
               gif_frame_one.gif_frame.dispose == GifDispose::previous && gif_frame_one.bytes.count == 4 &&
               gif_looped.ok() && gif_loop_taken && gif_loop_frame.frame_index == 0 &&
               gif_loop_frame.gif_palette && gif_loop_frame.gif_palette->count == 4,
           "account_media_gif_cache_generation_advance");

    const u64 before_thumbnail_identity = media.pipeline.gif_content_identity;
    const u32 thumbnail_delay = test_gif_delay_offset({g_png_encoded, gif_fixture.encoded_bytes});
    if (thumbnail_delay != ~0u) {
        g_png_encoded[thumbnail_delay] = 7;
        AccountMediaLocalPreviewRequest thumbnail_request{};
        thumbnail_request.snapshot = {{1, 1, 1, 0}, gif_fixture.encoded_bytes};
        thumbnail_request.path = {L"gif", 3};
        thumbnail_request.key = media_asset_local_draft_preview(88004, 1u | kLocalDraftPreviewThumbnailVariant,
                                                                 MediaAttachmentFormat::gif);
        thumbnail_request.owner_scope = 1; thumbnail_request.owner_generation = 1;
        const auto thumbnail = account_media_decode_local_preview(
            &media, thumbnail_request, {g_png_encoded, gif_fixture.encoded_bytes});
        const bool thumbnail_invalidated = !media.pipeline.gif_content_identity;
        AccountMediaFrame thumbnail_frame{};
        const bool thumbnail_taken = account_media_take_frame(&media, &thumbnail_frame);
        const auto resumed = account_media_advance_animation(&media, gif_key, gif_stored.generation, 0, 1106);
        AccountMediaFrame resumed_frame{};
        const bool resumed_taken = account_media_take_frame(&media, &resumed_frame);
        expect(before_thumbnail_identity && thumbnail.state == AccountMediaRequestState::ready &&
                   thumbnail_invalidated && thumbnail_taken &&
                   thumbnail_frame.kind == AccountMediaFrameKind::straight_rgba8 &&
                   resumed.state == AccountMediaRequestState::ready && resumed_taken &&
                   resumed_frame.duration_us == 50000 &&
                   media.pipeline.gif_content_identity == before_thumbnail_identity,
               "account_media_local_gif_thumbnail_invalidates_animation_metadata_before_workspace_overwrite");
        g_png_encoded[thumbnail_delay] = 5;
    } else {
        expect(false, "account_media_gif_thumbnail_control");
    }

    AnimatedWebpFixtureResult animated_fixture{};
    expect(build_lossless_animated_webp_fixture(Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                                &animated_fixture),
           "account_media_animated_webp_fixture_build");
    const MediaAssetKey animated_key = media_asset_custom_emoji(8'004, true);
    const EncodedMediaCacheStoreResult animated_stored = encoded_media_cache_store(
        &media.cache, animated_key, ConstSlice<u8>{g_png_encoded, animated_fixture.encoded_bytes},
        EncodedMediaCachePolicy{byte_literal("\"account-animated-webp\""), 2'000, true, false});
    const AccountMediaRequestResult animated_requested =
        account_media_request_static_asset(&media, animated_key, 1'005, 1'107);
    AccountMediaFrame animated_frame_zero{};
    const bool animated_zero_taken = account_media_take_frame(&media, &animated_frame_zero);
    const AccountMediaRequestResult animated_advanced =
        account_media_advance_animation(&media, animated_key, animated_stored.generation, 1, 1'108);
    AccountMediaFrame animated_frame_one{};
    const bool animated_one_taken = account_media_take_frame(&media, &animated_frame_one);
    const AccountMediaRequestResult animated_looped =
        account_media_advance_animation(&media, animated_key, animated_stored.generation, 0, 1'108);
    AccountMediaFrame animated_loop_frame{};
    const bool animated_loop_taken = account_media_take_frame(&media, &animated_loop_frame);
    expect(animated_stored.stored() && animated_requested.ok() &&
               animated_requested.state == AccountMediaRequestState::ready && media.webp_workspace &&
               media.animated_webp_workspace && animated_zero_taken && !animated_frame_zero.image &&
               animated_frame_zero.kind == AccountMediaFrameKind::animated_webp_rgba8 &&
               animated_frame_zero.frame_index == 0 && animated_frame_zero.frame_count == 4 &&
               animated_frame_zero.play_count == 2 &&
               animated_frame_zero.cache_generation == animated_stored.generation &&
               animated_frame_zero.canvas_width == 4 && animated_frame_zero.canvas_height == 4 &&
               animated_frame_zero.animated_webp_frame.blend == AnimatedWebpBlend::source &&
               animated_frame_zero.bytes.count == 64 && animated_advanced.ok() && animated_one_taken &&
               animated_frame_one.frame_index == 1 && animated_frame_one.duration_us == 10'000 &&
               animated_frame_one.bytes.count == 16 &&
               animated_frame_one.animated_webp_frame.dispose == AnimatedWebpDispose::background &&
               animated_looped.ok() && animated_loop_taken && animated_loop_frame.frame_index == 0,
           "account_media_animated_webp_cache_generation_advance");
    // The fixture's ANIM chunk names an opaque-ish background (0x80302010);
    // like browsers, the canvas it composes onto stays transparent.
    expect(animated_frame_zero.background_rgba == 0 && animated_frame_one.background_rgba == 0 &&
               animated_loop_frame.background_rgba == 0,
           "account_media_animated_webp_canvas_ignores_anim_background_colour");
    expect(account_media_stop(&media, 5'000) && !media.started && !media.allocation &&
               !media.webp_workspace && !media.animated_webp_workspace && !media.apng_workspace &&
               !media.gif_workspace,
           "account_media_stop_releases_worker_and_cache");
}

namespace {

bool stop_media_worker_fixture(AccountMediaWorker* worker, const char* label) noexcept {
    if (account_media_worker_stop(worker, 5'000))
        return true;
    expect(false, label);
    // Live decode lanes still own this stack fixture when joining fails.
    ExitProcess(1);
}

bool start_media_worker_fixture(AccountMediaWorker* worker, const char* label) noexcept {
    if (account_media_worker_initialize(worker).ok())
        return true;
    expect(false, label);
    return false;
}

} // namespace

void test_account_media_worker_decode_boundary() noexcept {
    GifFixtureResult fixture{};
    expect(build_indexed_gif_fixture(Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                     Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &fixture),
           "account_media_worker_gif_fixture_build");

    AccountMediaWorker worker{};
    if (!start_media_worker_fixture(&worker, "account_media_worker_initialize")) return;

    const MediaAssetKey key = media_asset_sticker(8'100, 4);
    const EncodedMediaCacheStoreResult stored = encoded_media_cache_store(
        &worker.lanes[0].media.cache, key, ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes},
        EncodedMediaCachePolicy{byte_literal("\"worker-gif\""), 2'000, true, false});
    const AccountMediaRequestResult queued =
        account_media_worker_request_static_asset(&worker, key, 1'000, 1'100);
    const DWORD first_wait = WaitForSingleObject(account_media_worker_notification_handle(&worker), 2'000);
    const AccountMediaRequestResult ready =
        account_media_worker_request_static_asset(&worker, key, 1'001, 1'101);
    AccountMediaFrame first{};
    const bool first_taken = account_media_worker_take_frame(&worker, &first);

    const AccountMediaRequestResult advance_queued =
        account_media_worker_advance_animation(&worker, key, stored.generation, 1, 1'102);
    const DWORD second_wait = WaitForSingleObject(account_media_worker_notification_handle(&worker), 2'000);
    const AccountMediaRequestResult advance_ready =
        account_media_worker_advance_animation(&worker, key, stored.generation, 1, 1'103);
    AccountMediaFrame second{};
    const bool second_taken = account_media_worker_take_frame(&worker, &second);

    u32 single_hits = 0, serial_hits = 0, observed_advances = 0;
    const auto measured_advance = [&](MediaAssetKey target, u16 generation, u32 frame_index,
                                       u64 duration, u32* hits) noexcept {
        u64 previous[kAccountMediaDecodeLaneCount]{};
        for (u32 lane = 0; lane < kAccountMediaDecodeLaneCount; ++lane) {
            const auto& pipeline = worker.lanes[lane].media.pipeline;
            if (pipeline.gif_workspace && pipeline.gif_image.frames == pipeline.gif_workspace->frames)
                previous[lane] = pipeline.gif_content_identity;
        }
        const u64 identity = encoded_media_cache_find(&worker.lanes[0].media.cache, target, 0).content_identity;
        const auto submitted = account_media_worker_advance_animation(&worker, target, generation, frame_index, 1104);
        if (submitted.state != AccountMediaRequestState::queued ||
            WaitForSingleObject(account_media_worker_notification_handle(&worker), 2000) != WAIT_OBJECT_0)
            return false;
        AccountMediaFrame frame{};
        if (!account_media_worker_take_frame(&worker, &frame) || !media_asset_key_equal(frame.key, target) ||
            frame.frame_index != frame_index || frame.duration_us != duration || frame.content_identity != identity)
            return false;
        for (u32 lane = 0; lane < kAccountMediaDecodeLaneCount; ++lane) {
            if (ReadAcquire(&worker.lanes[lane].phase) != static_cast<LONG>(AccountMediaWorkerPhase::leased))
                continue;
            ++observed_advances;
            if (identity && previous[lane] == identity) ++*hits;
            return worker.lanes[lane].media.pipeline.gif_content_identity == identity && identity;
        }
        return false;
    };
    bool reuse_sequences = first_taken && second_taken &&
        measured_advance(key, stored.generation, 2, 100000, &single_hits) &&
        measured_advance(key, stored.generation, 3, 20000, &single_hits) &&
        measured_advance(key, stored.generation, 0, 50000, &single_hits);
    const u32 serial_delay = test_gif_delay_offset({g_png_encoded, fixture.encoded_bytes});
    EncodedMediaCacheStoreResult serial_store{};
    const auto serial_key = media_asset_sticker(8102, 4);
    if (reuse_sequences && serial_delay != ~0u) {
        g_png_encoded[serial_delay] = 7;
        serial_store = encoded_media_cache_store(&worker.lanes[0].media.cache, serial_key,
            {g_png_encoded, fixture.encoded_bytes}, {byte_literal("\"serial-gif\""), 2000, true, false});
        g_png_encoded[serial_delay] = 5;
        for (u32 step = 0; step < 4 && reuse_sequences; ++step)
            reuse_sequences = measured_advance(step % 2 ? key : serial_key,
                step % 2 ? stored.generation : serial_store.generation, 0, step % 2 ? 50000 : 70000, &serial_hits);
    } else {
        reuse_sequences = false;
    }
    // Alternating sources keep to their lanes: only the new source's first frame re-parses.
    expect(reuse_sequences && serial_store.stored() && observed_advances == 7 && single_hits == 3 &&
               serial_hits == 3,
           "account_media_worker_gif_metadata_actual_assignment_single_3_hits_serial_3_hits");
    const bool decoded_off_ui = worker.lanes[0].thread_id != 0 && worker.lanes[0].thread_id != GetCurrentThreadId();
    const bool worker_stopped = stop_media_worker_fixture(&worker, "account_media_worker_decode_stop");

    expect(stored.stored() && decoded_off_ui &&
               queued.state == AccountMediaRequestState::queued && first_wait == WAIT_OBJECT_0 &&
               ready.state == AccountMediaRequestState::ready && first_taken &&
               first.kind == AccountMediaFrameKind::gif_indices && first.frame_index == 0 &&
               first.content_identity != 0 && second.content_identity == first.content_identity &&
               first.gif_palette && advance_queued.state == AccountMediaRequestState::queued &&
               second_wait == WAIT_OBJECT_0 && advance_ready.state == AccountMediaRequestState::ready &&
               second_taken && second.kind == AccountMediaFrameKind::gif_indices &&
               second.frame_index == 1 && worker_stopped,
           "account_media_worker_cache_and_animation_decode_off_ui_thread");
}

void test_account_media_worker_failure_is_edge_triggered() noexcept {
    AccountMediaWorker worker{};
    if (!start_media_worker_fixture(&worker, "account_media_worker_failure_initialize")) return;

    const MediaAssetKey key = media_asset_custom_emoji(8'101, false);
    const u8 invalid_webp[]{'R', 'I', 'F', 'F', 4, 0, 0, 0, 'W', 'E', 'B', 'P'};
    const EncodedMediaCacheStoreResult stored = encoded_media_cache_store(
        &worker.lanes[0].media.cache, key,
        ConstSlice<u8>{invalid_webp, static_cast<u32>(sizeof(invalid_webp))},
        EncodedMediaCachePolicy{byte_literal("\"invalid-webp\""), 2'000, true, false});
    const AccountMediaRequestResult queued =
        account_media_worker_request_static_asset(&worker, key, 1'000, 1'100);
    const DWORD waited =
        WaitForSingleObject(account_media_worker_notification_handle(&worker), 2'000);
    const AccountMediaRequestResult failure =
        account_media_worker_request_static_asset(&worker, key, 1'001, 1'101);
    const AccountMediaRequestResult retained =
        account_media_worker_request_static_asset(&worker, key, 1'002, 1'102);
    const u64 retained_wake = account_media_worker_next_wake_at_ms(&worker);
    account_media_worker_expire_retries(&worker, retained_wake - 1);
    const bool before_due = account_media_worker_next_wake_at_ms(&worker) == retained_wake;
    account_media_worker_expire_retries(&worker, retained_wake);
    const bool no_stale_wake = account_media_worker_next_wake_at_ms(&worker) == 0;
    account_media_worker_expire_retries(&worker, retained_wake + 60'000);
    expect(before_due && no_stale_wake && account_media_worker_next_wake_at_ms(&worker) == 0,
           "account_media_worker_invisible_failure_deadline_consumed_once_without_polling");

    const bool stopped = stop_media_worker_fixture(&worker, "account_media_worker_failure_stop");
    expect(stored.stored() && queued.state == AccountMediaRequestState::queued &&
               waited == WAIT_OBJECT_0 && failure.state == AccountMediaRequestState::failed &&
               failure.error == AccountMediaError::decode && failure.retry_at_ms > 1'002 &&
               retained.state == AccountMediaRequestState::retry_wait &&
               retained.retry_at_ms == failure.retry_at_ms && retained_wake == failure.retry_at_ms &&
               stopped,
           "account_media_worker_failure_reported_once_then_retained_as_retry_wait");
}

void test_account_media_worker_reclaimed_failure_cooldown() noexcept {
    PngFixtureResult png{};
    if (!build_rgba8_png_fixture(16, 16, {g_png_encoded, sizeof(g_png_encoded)},
                                  {g_png_zlib, sizeof(g_png_zlib)}, &png)) {
        expect(false, "account_media_worker_reclaimed_failure_fixture");
        return;
    }
    AccountMediaWorker worker{};
    if (!start_media_worker_fixture(&worker, "account_media_worker_reclaimed_failure_initialize")) return;
    const MediaAssetKey bad_key = media_asset_custom_emoji(8'103, false);
    const MediaAssetKey good_key = media_asset_sticker(8'104, 1);
    const u8 invalid_webp[]{'R', 'I', 'F', 'F', 4, 0, 0, 0, 'W', 'E', 'B', 'P'};
    const auto store_bad = [&]() noexcept {
        return encoded_media_cache_store(&worker.lanes[0].media.cache, bad_key,
            {invalid_webp, sizeof(invalid_webp)},
            EncodedMediaCachePolicy{byte_literal("\"reclaimed-bad\""), 2'000, true, false});
    };
    const auto bad_stored = store_bad();
    const auto good_stored = encoded_media_cache_store(&worker.lanes[0].media.cache, good_key,
        {g_png_encoded, png.encoded_bytes},
        EncodedMediaCachePolicy{byte_literal("\"reclaimed-good\""), 2'000, true, false});
    const auto bad_queued = account_media_worker_request_static_asset(&worker, bad_key, 1'000, 1'100);
    const DWORD bad_wait = WaitForSingleObject(account_media_worker_notification_handle(&worker), 2'000);
    // Deliberately visit another asset before querying the failure. Its request
    // reclaims the failed output lane, as Main's ordered visibility scan does.
    const auto good_queued = account_media_worker_request_static_asset(&worker, good_key, 1'001, 1'100);
    const u64 cooldown = account_media_worker_next_wake_at_ms(&worker);
    const u64 queued_before_retry = worker.counters.queued;
    bool held = cooldown > 1'002 && cooldown != ~u64{0};
    for (u32 index = 0; index < 4; ++index) {
        const auto retry = account_media_worker_request_static_asset(&worker, bad_key, 1'002, 1'100);
        held = held && retry.state == AccountMediaRequestState::retry_wait && retry.retry_at_ms == cooldown;
    }
    const DWORD good_wait = WaitForSingleObject(account_media_worker_notification_handle(&worker), 2'000);
    AccountMediaFrame good_frame{};
    const bool good_taken = account_media_worker_take_frame(&worker, &good_frame);
    const u64 queued_while_held = worker.counters.queued;
    // Reinsert corrupt bytes so the eligible retry remains a cold codec test
    // with no network dependency, even if the decoder erased the first entry.
    const auto bad_restored = store_bad();
    account_media_worker_expire_retries(&worker, cooldown);
    const auto eligible = account_media_worker_request_static_asset(&worker, bad_key, cooldown, 1'100);
    const DWORD retry_wait = WaitForSingleObject(account_media_worker_notification_handle(&worker), 2'000);
    const auto reported = account_media_worker_request_static_asset(&worker, bad_key, cooldown + 1, 1'100);
    const u64 queued_after_expiry = worker.counters.queued;
    const bool stopped = stop_media_worker_fixture(&worker, "account_media_worker_reclaimed_failure_stop");
    expect(bad_stored.stored() && good_stored.stored() && bad_restored.stored() &&
        bad_queued.state == AccountMediaRequestState::queued && bad_wait == WAIT_OBJECT_0 &&
        good_queued.state == AccountMediaRequestState::queued && good_wait == WAIT_OBJECT_0 &&
        good_taken && media_asset_key_equal(good_frame.key, good_key) &&
        held && queued_before_retry == 2 && queued_while_held == queued_before_retry &&
        eligible.state == AccountMediaRequestState::queued && retry_wait == WAIT_OBJECT_0 &&
        reported.state == AccountMediaRequestState::failed && reported.error == AccountMediaError::decode &&
        queued_after_expiry == 3 && stopped,
        "account_media_worker_reclaimed_failure_keeps_cooldown_without_completion_storm");
}

void test_account_media_worker_later_frame_failure_cooldown() noexcept {
    GifFixtureResult fixture{};
    GifWorkspace workspace{};
    GifImage parsed{};
    if (!build_indexed_gif_fixture(Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                  Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &fixture) ||
        !gif_parse(ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes}, &parsed, &workspace).ok()) {
        expect(false, "account_media_worker_later_frame_fixture");
        return;
    }
    const u32 payload = parsed.frames[3].data_offset + 1;
    g_png_encoded[payload] = static_cast<u8>((g_png_encoded[payload] & 0xF8u) | 1u);

    AccountMediaWorker worker{};
    if (!start_media_worker_fixture(&worker, "account_media_worker_later_frame_initialize")) return;
    const MediaAssetKey key = media_asset_sticker(8'102, 4);
    const EncodedMediaCacheStoreResult stored = encoded_media_cache_store(
        &worker.lanes[0].media.cache, key, ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes},
        EncodedMediaCachePolicy{byte_literal("\"bad-later-frame\""), 2'000, true, false});
    const AccountMediaRequestResult queued = account_media_worker_request_static_asset(
        &worker, key, clock_monotonic_milliseconds(), 1'100);
    const DWORD first_wait = WaitForSingleObject(account_media_worker_notification_handle(&worker), 2'000);
    const AccountMediaRequestResult ready = account_media_worker_request_static_asset(
        &worker, key, clock_monotonic_milliseconds(), 1'101);
    AccountMediaFrame first{};
    const bool first_taken = account_media_worker_take_frame(&worker, &first);
    const AccountMediaRequestResult advance =
        account_media_worker_advance_animation(&worker, key, stored.generation, 3, 1'102);
    const DWORD second_wait = WaitForSingleObject(account_media_worker_notification_handle(&worker), 2'000);
    const AccountMediaRequestResult failed =
        account_media_worker_advance_animation(&worker, key, stored.generation, 3, 1'103);
    const u64 queued_before_retry = worker.counters.queued;
    const u64 now_ms = clock_monotonic_milliseconds();
    const AccountMediaRequestResult restart =
        account_media_worker_request_static_asset(&worker, key, now_ms, 1'104);
    const AccountMediaRequestResult retained =
        account_media_worker_advance_animation(&worker, key, stored.generation, 3, 1'105);
    const u64 wake = account_media_worker_next_wake_at_ms(&worker);
    const u64 queued_after_retry = worker.counters.queued;
    const bool stopped = stop_media_worker_fixture(&worker, "account_media_worker_later_frame_failure_stop");
    expect(stored.stored() && queued.state == AccountMediaRequestState::queued &&
               first_wait == WAIT_OBJECT_0 && ready.state == AccountMediaRequestState::ready &&
               first_taken && first.frame_index == 0 && advance.state == AccountMediaRequestState::queued &&
               second_wait == WAIT_OBJECT_0 && failed.state == AccountMediaRequestState::failed &&
               failed.error == AccountMediaError::decode &&
               restart.state == AccountMediaRequestState::retry_wait && restart.retry_at_ms > now_ms &&
               retained.state == AccountMediaRequestState::retry_wait && wake == restart.retry_at_ms &&
               queued_before_retry == 2 && queued_after_retry == queued_before_retry && stopped,
           "account_media_worker_later_frame_failure_blocks_redownload_across_lanes");
}

void test_account_media_worker_shared_parallel_lanes() noexcept {
    GifFixtureResult fixture{};
    expect(build_indexed_gif_fixture(Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                     Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &fixture),
           "account_media_worker_parallel_gif_fixture_build");
    const u32 parallel_delay = test_gif_delay_offset({g_png_encoded, fixture.encoded_bytes});
    if (parallel_delay == ~0u) { expect(false, "account_media_worker_parallel_gif_control"); return; }

    AccountMediaWorker worker{};
    if (!start_media_worker_fixture(&worker, "account_media_worker_parallel_initialize")) return;

    MediaAssetKey keys[kAccountMediaDecodeLaneCount]{};
    u16 generations[kAccountMediaDecodeLaneCount]{};
    u64 identities[kAccountMediaDecodeLaneCount]{};
    bool initial_queued = true;
    bool shared_cache = true;
    for (u32 index = 0; index < kAccountMediaDecodeLaneCount; ++index) {
        keys[index] = media_asset_sticker(8'200 + index, 4);
        if (index % 2) {
            keys[index] = media_asset_embed(8'200 + index, 1, 0, MediaEmbedResource::image,
                                           MediaAttachmentFormat::gif, true, MediaResolutionTier::px512);
            keys[index].kind = MediaAssetKind::profile_image;
        }
        g_png_encoded[parallel_delay] = static_cast<u8>(5 + index);
        const EncodedMediaCacheStoreResult stored = encoded_media_cache_store(
            &worker.lanes[0].media.cache, keys[index], ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes},
            EncodedMediaCachePolicy{byte_literal("\"parallel-gif\""), 2'000, true, false});
        generations[index] = stored.generation;
        identities[index] = encoded_media_cache_find(&worker.lanes[0].media.cache, keys[index], 0).content_identity;
        const AccountMediaAttachmentRequest profile_request{
            utf8_literal("https://cdn.discordapp.com/banners/123/a_fixture.gif?size=512"),
            keys[index], 0, 0, 0, 0, CdnImageType::gif, {0, 0, 0}, nullptr};
        const auto queued = keys[index].kind == MediaAssetKind::profile_image
            ? account_media_worker_request_attachment(&worker, profile_request, 1'000 + index, 1'100)
            : account_media_worker_request_static_asset(&worker, keys[index], 1'000 + index, 1'100);
        initial_queued = initial_queued && stored.stored() && queued.state == AccountMediaRequestState::queued;
        shared_cache =
            shared_cache && worker.lanes[index].media.pipeline.cache == &worker.lanes[0].media.cache &&
            (index == 0 ? worker.lanes[index].media.owns_cache : !worker.lanes[index].media.owns_cache);
    }
    g_png_encoded[parallel_delay] = 5;

    bool seen_initial[kAccountMediaDecodeLaneCount]{};
    u32 initial_frames = 0;
    const u64 initial_deadline = clock_monotonic_milliseconds() + 2'000;
    while (initial_frames < kAccountMediaDecodeLaneCount &&
           clock_monotonic_milliseconds() < initial_deadline) {
        AccountMediaFrame frame{};
        if (account_media_worker_take_frame(&worker, &frame)) {
            for (u32 index = 0; index < kAccountMediaDecodeLaneCount; ++index) {
                if (!seen_initial[index] && media_asset_key_equal(frame.key, keys[index]) &&
                    frame.frame_index == 0 && frame.duration_us == (5u + index) * 10000u &&
                    frame.content_identity == identities[index]) {
                    seen_initial[index] = true;
                    ++initial_frames;
                    break;
                }
            }
            continue;
        }
        const u64 now_ms = clock_monotonic_milliseconds();
        const DWORD remaining =
            initial_deadline > now_ms ? static_cast<DWORD>(initial_deadline - now_ms) : 1;
        WaitForSingleObject(account_media_worker_notification_handle(&worker), remaining);
    }

    bool advances_queued = initial_frames == kAccountMediaDecodeLaneCount;
    u64 retained_before[kAccountMediaDecodeLaneCount]{};
    if (advances_queued)
        for (u32 lane = 0; lane < kAccountMediaDecodeLaneCount; ++lane)
            retained_before[lane] = worker.lanes[lane].media.pipeline.gif_content_identity;
    for (u32 index = 0; index < kAccountMediaDecodeLaneCount; ++index) {
        advances_queued = advances_queued && account_media_worker_advance_animation(
                                                 &worker, keys[index], generations[index], 1, 1'200)
                                                     .state == AccountMediaRequestState::queued;
    }
    bool seen_advance[kAccountMediaDecodeLaneCount]{};
    u32 advanced_frames = 0;
    const u64 advance_deadline = clock_monotonic_milliseconds() + 2'000;
    while (advanced_frames < kAccountMediaDecodeLaneCount &&
           clock_monotonic_milliseconds() < advance_deadline) {
        AccountMediaFrame frame{};
        if (account_media_worker_take_frame(&worker, &frame)) {
            for (u32 index = 0; index < kAccountMediaDecodeLaneCount; ++index) {
                if (!seen_advance[index] && media_asset_key_equal(frame.key, keys[index]) &&
                    frame.frame_index == 1 && frame.content_identity == identities[index]) {
                    seen_advance[index] = true;
                    ++advanced_frames;
                    break;
                }
            }
            continue;
        }
        const u64 now_ms = clock_monotonic_milliseconds();
        const DWORD remaining =
            advance_deadline > now_ms ? static_cast<DWORD>(advance_deadline - now_ms) : 1;
        WaitForSingleObject(account_media_worker_notification_handle(&worker), remaining);
    }

    u32 batch_hits = 0;
    bool batch_metadata = advanced_frames == kAccountMediaDecodeLaneCount;
    if (batch_metadata)
        for (u32 lane = 0; lane < kAccountMediaDecodeLaneCount; ++lane)
            for (u32 index = 0; index < kAccountMediaDecodeLaneCount; ++index)
                if (media_asset_key_equal(worker.lanes[lane].command.key, keys[index])) {
                    if (identities[index] && retained_before[lane] == identities[index]) ++batch_hits;
                    batch_metadata = batch_metadata &&
                        worker.lanes[lane].media.pipeline.gif_content_identity == identities[index];
                }
    expect(batch_metadata && batch_hits == kAccountMediaDecodeLaneCount,
           "account_media_worker_gif_metadata_distinct_identity_batch_actual_lane_hits");
    bool distinct_threads = true;
    for (u32 index = 0; index < kAccountMediaDecodeLaneCount; ++index) {
        distinct_threads = distinct_threads && worker.lanes[index].thread_id != 0 &&
                           worker.lanes[index].thread_id != GetCurrentThreadId();
        for (u32 peer = index + 1; peer < kAccountMediaDecodeLaneCount; ++peer) {
            distinct_threads =
                distinct_threads && worker.lanes[index].thread_id != worker.lanes[peer].thread_id;
        }
    }
    const bool counters = InterlockedCompareExchange64(&worker.counters.queued, 0, 0) >= 8 &&
                          worker.lanes[0].media.cache.counters.lease_acquires >= 8 &&
                          worker.lanes[0].media.cache.counters.lease_acquires ==
                              worker.lanes[0].media.cache.counters.lease_releases;
    const bool stopped = stop_media_worker_fixture(&worker, "account_media_worker_parallel_lanes_stop");
    expect(initial_queued && shared_cache && initial_frames == kAccountMediaDecodeLaneCount &&
               advances_queued && advanced_frames == kAccountMediaDecodeLaneCount && distinct_threads &&
               counters && stopped,
           "account_media_worker_four_parallel_lanes_share_leased_cache");
}

void test_account_media_worker_animation_priority_reserve() noexcept {
    PngFixtureResult png{};
    expect(build_rgba8_png_fixture(16, 16, Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                   Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &png),
           "account_media_worker_priority_png_fixture_build");
    AccountMediaWorker worker{};
    if (!start_media_worker_fixture(&worker, "account_media_worker_priority_initialize")) return;

    MediaAssetKey stills[kAccountMediaDecodeLaneCount]{};
    bool stored = true;
    for (u32 index = 0; index < kAccountMediaDecodeLaneCount; ++index) {
        stills[index] = media_asset_sticker(8'300 + index, 1);
        stored = stored && encoded_media_cache_store(&worker.lanes[0].media.cache, stills[index],
                                                     ConstSlice<u8>{g_png_encoded, png.encoded_bytes},
                                                     EncodedMediaCachePolicy{{}, 2'000, true, false})
                               .stored();
    }
    bool stills_queued=true;
    for(u32 index=0;index<kAccountMediaDecodeLaneCount-1;++index)
        stills_queued=account_media_worker_request_static_asset(&worker,stills[index],1000+index,1100).state==
            AccountMediaRequestState::queued && stills_queued;
    const auto final_still=account_media_worker_request_static_asset(&worker,stills[kAccountMediaDecodeLaneCount-1],1009,1100);

    GifFixtureResult gif{};
    expect(build_indexed_gif_fixture(Slice<u8>{g_png_encoded, sizeof(g_png_encoded)},
                                     Slice<u8>{g_png_zlib, sizeof(g_png_zlib)}, &gif),
           "account_media_worker_priority_gif_fixture_build");
    const MediaAssetKey animated = media_asset_sticker(8'310, 4);
    const EncodedMediaCacheStoreResult animated_stored = encoded_media_cache_store(
        &worker.lanes[0].media.cache, animated, ConstSlice<u8>{g_png_encoded, gif.encoded_bytes},
        EncodedMediaCachePolicy{{}, 2'000, true, false});
    const AccountMediaRequestResult animated_queued =
        account_media_worker_request_static_asset(&worker, animated, 1'004, 1'100);

    bool seen_animated = false;
    u32 frames = 0;
    const u64 deadline = clock_monotonic_milliseconds() + 2'000;
    while (frames < kAccountMediaDecodeLaneCount && clock_monotonic_milliseconds() < deadline) {
        AccountMediaFrame frame{};
        if (account_media_worker_take_frame(&worker, &frame)) {
            ++frames;
            seen_animated = seen_animated || media_asset_key_equal(frame.key, animated);
            continue;
        }
        const u64 now_ms = clock_monotonic_milliseconds();
        WaitForSingleObject(account_media_worker_notification_handle(&worker),
                            deadline > now_ms ? static_cast<DWORD>(deadline - now_ms) : 1);
    }
    const bool animation_owned_reserved_lane =
        media_asset_key_equal(worker.lanes[0].command.key, animated) ||
        media_asset_key_equal(worker.lanes[0].completed.key, animated);
    const bool stopped = stop_media_worker_fixture(&worker, "account_media_worker_priority_reserve_stop");
    expect(stored && stills_queued && final_still.state == AccountMediaRequestState::busy &&
               animated_stored.stored() && animated_queued.state == AccountMediaRequestState::queued &&
               frames == kAccountMediaDecodeLaneCount && seen_animated && animation_owned_reserved_lane &&
               stopped,
           "account_media_worker_animation_lane_survives_seven_large_stills");
}

void test_account_media_mixed_animated_webp() noexcept {
    AnimatedWebpFixtureResult fixture{};
    expect(build_mixed_animated_webp_fixture(Slice<u8>{g_png_encoded, sizeof(g_png_encoded)}, &fixture),
           "account_media_mixed_animated_webp_fixture_build");
    AccountMedia media{};
    const AccountMediaRequestResult initialized = account_media_initialize(&media);
    if (!initialized.ok()) {
        expect(false, "account_media_mixed_animated_webp_initialize");
        return;
    }
    const MediaAssetKey key = media_asset_custom_emoji(8'005, true);
    const EncodedMediaCacheStoreResult stored = encoded_media_cache_store(
        &media.cache, key, ConstSlice<u8>{g_png_encoded, fixture.encoded_bytes},
        EncodedMediaCachePolicy{byte_literal("\"account-mixed-animated-webp\""), 2'000, true, false});
    const bool lazy_before = !media.lossy_webp_workspace;
    const AccountMediaRequestResult requested =
        account_media_request_static_asset(&media, key, 1'010, 1'110);
    AccountMediaFrame frame_zero{};
    const bool zero_taken = account_media_take_frame(&media, &frame_zero);
    const AccountMediaRequestResult advanced_one =
        account_media_advance_animation(&media, key, stored.generation, 1, 1'111);
    AccountMediaFrame frame_one{};
    const bool one_taken = account_media_take_frame(&media, &frame_one);
    const AccountMediaRequestResult advanced_two =
        account_media_advance_animation(&media, key, stored.generation, 2, 1'112);
    AccountMediaFrame frame_two{};
    const bool two_taken = account_media_take_frame(&media, &frame_two);
    bool alpha_exact = frame_two.bytes.count == 16;
    for (u32 index = 0; index < frame_two.bytes.count; ++index) {
        alpha_exact = alpha_exact && frame_two.bytes[index] == 128;
    }
    const AccountMediaRequestResult looped =
        account_media_advance_animation(&media, key, stored.generation, 0, 1'113);
    AccountMediaFrame loop_frame{};
    const bool loop_taken = account_media_take_frame(&media, &loop_frame);
    expect(
        stored.stored() && lazy_before && requested.ok() &&
            requested.state == AccountMediaRequestState::ready && media.lossy_webp_workspace &&
            zero_taken && frame_zero.kind == AccountMediaFrameKind::animated_webp_rgba8 &&
            frame_zero.bytes.count == 64 &&
            frame_zero.frame_count == 3 && frame_zero.play_count == 1 && advanced_one.ok() && one_taken &&
            frame_one.kind == AccountMediaFrameKind::animated_webp_vp8 && frame_one.vp8_frame &&
            frame_one.vp8_header && frame_one.vp8_modes == &media.lossy_webp_workspace->modes &&
            frame_one.vp8_coefficients == &media.lossy_webp_workspace->coefficients &&
            frame_one.bytes.count == 0 &&
            frame_one.animated_webp_frame.dispose == AnimatedWebpDispose::background && advanced_two.ok() &&
            two_taken && frame_two.kind == AccountMediaFrameKind::animated_webp_vp8 &&
            frame_two.animated_webp_frame.has_alpha && alpha_exact && looped.ok() && loop_taken &&
            loop_frame.kind == AccountMediaFrameKind::animated_webp_rgba8,
        "account_media_mixed_animated_webp_codec_switch_and_ownership");
    expect(account_media_stop(&media, 5'000) && !media.lossy_webp_workspace &&
               !media.animated_webp_workspace,
           "account_media_mixed_animated_webp_stop");
}

// Unsized profile JPEGs: only i.scdn.co takes the album-art parser; CDN
// splashes and banners keep the ordinary CDN parser.
void test_account_media_profile_jpeg_hosts() noexcept {
    AccountMedia media{};
    if (!account_media_initialize(&media).ok()) {
        expect(false, "account_media_profile_jpeg_hosts_initialize");
        return;
    }
    const auto request = [&](u64 id, Utf8View url) noexcept {
        MediaAssetKey key = media_asset_embed(id, 1, 0, MediaEmbedResource::image, MediaAttachmentFormat::jpeg, false,
                                              MediaResolutionTier::px512);
        key.kind = MediaAssetKind::profile_image;
        const AccountMediaAttachmentRequest r{url, key, 0, 0, 0, 0, CdnImageType::jpeg, {0, 0, 0}, nullptr};
        return account_media_request_attachment(&media, r, 1'000, 1'100);
    };
    const auto splash = request(9'101, utf8_literal("https://cdn.discordapp.com/discovery-splashes/10/"
                                                    "0123456789abcdef0123456789abcdef.jpg?size=480"));
    const auto art = request(9'102, utf8_literal("https://i.scdn.co/image/ab67616d0000b2730123456789abcdef01234567"));
    const auto other = request(9'103, utf8_literal("https://example.com/image.jpg"));
    expect(splash.error != AccountMediaError::target && art.error != AccountMediaError::target &&
               other.error == AccountMediaError::target && other.media_url_error == MediaUrlError::invalid_host,
           "account_media_profile_jpeg_cdn_banner_not_routed_to_album_art_parser");
    (void)account_media_stop(&media, 5'000);
}

void test_account_media_long_animated_webp() noexcept {
    auto* encoded = static_cast<u8*>(VirtualAlloc(nullptr, kAnimatedWebpEncodedMaximumBytes,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    expect(encoded != nullptr, "long_webp_fixture_allocation");
    if (!encoded) return;
    auto load32 = [](const u8* bytes) noexcept {
        return u32(bytes[0]) | (u32(bytes[1]) << 8) | (u32(bytes[2]) << 16) | (u32(bytes[3]) << 24);
    };
    auto store32 = [](u8* bytes, u32 value) noexcept {
        for (u32 i = 0; i < 4; ++i) bytes[i] = static_cast<u8>(value >> (8 * i));
    };
    for (u32 mixed = 0; mixed < 2; ++mixed) {
        u8 source[512]{};
        AnimatedWebpFixtureResult fixture{};
        const bool built = mixed ? build_mixed_animated_webp_fixture({source, sizeof(source)}, &fixture)
                                 : build_lossless_animated_webp_fixture({source, sizeof(source)}, &fixture);
        expect(built, "long_webp_source_fixture");
        if (!built) continue;
        AnimatedWebpWorkspace reference_workspace{};
        AnimatedWebpImage reference{};
        expect(animated_webp_parse({source, fixture.encoded_bytes}, &reference,
                                  &reference_workspace).ok(), "long_webp_reference_controls");
        u32 offsets[4]{};
        u32 sizes[4]{};
        u32 cursor = 44; // RIFF, VP8X, ANIM; retain every original ANMF verbatim.
        for (u32 i = 0; i < fixture.frame_count; ++i) {
            offsets[i] = cursor;
            const u32 payload = load32(source + cursor + 4);
            sizes[i] = 8 + payload + (payload & 1u);
            cursor += sizes[i];
        }
        AccountMedia media{};
        if (!account_media_initialize(&media).ok()) {
            expect(false, "long_webp_media_initialize");
            continue;
        }
        const u32 counts[]{256, 257, 513, kAnimatedWebpFrameCapacity, kAnimatedWebpFrameCapacity + 1};
        AnimatedWebpFrame* stable_table = nullptr;
        for (u32 count : counts) {
            // The frame-count limit is codec independent; probe it with one source.
            if (mixed && count > 513) continue;
            copy_bytes(encoded, source, 44);
            cursor = 44;
            for (u32 i = 0; i < count; ++i) {
                const u32 source_frame = i % fixture.frame_count;
                copy_bytes(encoded + cursor, source + offsets[source_frame], sizes[source_frame]);
                cursor += sizes[source_frame];
            }
            store32(encoded + 4, cursor - 8);
            if (count > 513) {
                AnimatedWebpImage parsed{};
                const auto result = animated_webp_parse({encoded, cursor}, &parsed,
                                                       media.animated_webp_workspace);
                expect(count == kAnimatedWebpFrameCapacity
                           ? result.ok() && parsed.frame_count == count
                           : result.error == AnimatedWebpError::frame_count,
                       "long_webp_playback_limit_boundary");
                continue;
            }
            MediaAssetKey key = media_asset_embed(90'000 + count, 1, 0, MediaEmbedResource::image,
                MediaAttachmentFormat::webp, true, MediaResolutionTier::px512);
            key.kind = MediaAssetKind::profile_image;
            const auto stored = encoded_media_cache_store(&media.cache, key, {encoded, cursor},
                EncodedMediaCachePolicy{byte_literal("\"long-banner\""), 2'000, true, false});
            const AccountMediaAttachmentRequest request{
                utf8_literal("https://cdn.discordapp.com/banners/123/a_fixture.webp?size=512&animated=true"),
                key, 0, 0, 0, 0, CdnImageType::webp, {0, 0, 0}, nullptr};
            const auto requested = account_media_request_attachment(&media, request, 1'000, 1'100);
            AccountMediaFrame first{};
            bool exact = stored.stored() && requested.state == AccountMediaRequestState::ready &&
                         account_media_take_frame(&media, &first) && first.frame_count == count;
            expect(exact, "long_webp_profile_banner_first_frame_without_gif_fetch");
            if (!exact) continue;
            auto* workspace = media.animated_webp_workspace;
            expect(count == 256 ? !workspace->extended_frames.data
                                : workspace->extended_frames.count >= count &&
                                  workspace->extended_frames.count <= count * 2,
                   "long_webp_metadata_grows_only_when_needed");
            auto* table = workspace->extended_frames.data;
            expect(!stable_table || table == stable_table, "long_webp_arena_grows_in_place");
            stable_table = table;
            const u32 indices[]{0, 255, count - 1, 256, 511, 0};
            for (u32 index : indices) {
                if (index >= count) continue;
                const auto advanced = account_media_advance_animation(&media, key, stored.generation,
                                                                       index, 1'101);
                AccountMediaFrame frame{};
                const auto& expected = reference.frames[index % fixture.frame_count];
                exact = exact && advanced.ok() && account_media_take_frame(&media, &frame) &&
                    frame.frame_index == index && frame.frame_count == count &&
                    frame.animated_webp_frame.width == expected.width &&
                    frame.animated_webp_frame.x_offset == expected.x_offset &&
                    frame.animated_webp_frame.dispose == expected.dispose &&
                    frame.animated_webp_frame.codec == expected.codec &&
                    frame.duration_us == maximum(kAnimatedWebpFrameFloorUs, u64(expected.duration_ms) * 1000);
            }
            expect(exact && table == workspace->extended_frames.data && !media.client_started,
                   "long_webp_late_frames_and_loop_reuse_metadata_without_network");
            if (count == 257) {
                auto grow = workspace->grow_frames;
                const u32 capacity = workspace->extended_frames.count;
                workspace->grow_frames = nullptr;
                workspace->extended_frames.count = 256;
                AnimatedWebpImage parsed{};
                expect(animated_webp_parse({encoded, cursor}, &parsed, workspace).error ==
                           AnimatedWebpError::output_capacity,
                       "long_webp_insufficient_caller_storage_rejected_safely");
                workspace->grow_frames = [](AnimatedWebpWorkspace*, u32) noexcept { return false; };
                expect(animated_webp_parse({encoded, cursor}, &parsed, workspace).error ==
                           AnimatedWebpError::output_capacity,
                       "long_webp_arena_growth_failure_rejected_safely");
                workspace->grow_frames = grow;
                workspace->extended_frames.count = capacity;
                encoded[cursor - sizes[(count - 1) % fixture.frame_count] + 8 + 15] |= 0x80;
                expect(animated_webp_parse({encoded, cursor}, &parsed, workspace).error ==
                           AnimatedWebpError::unsupported_feature,
                       "long_webp_invalid_frame_beyond_old_limit_still_rejected");
            }
        }
        void* extended = media.animated_webp_workspace
                             ? media.animated_webp_workspace->extended_frames.data : nullptr;
        expect(account_media_stop(&media, 5'000), "long_webp_media_shutdown");
        MEMORY_BASIC_INFORMATION memory{};
        expect(extended && VirtualQuery(extended, &memory, sizeof(memory)) && memory.State == MEM_FREE,
               "long_webp_extended_metadata_released");
    }
    VirtualFree(encoded, 0, MEM_RELEASE);
}

namespace {

struct MessageMediaTestCapture {
    MediaAssetKey keys[16];
    bool forces[16];
    u32 count;
    bool fail;
    bool stop;
};

bool message_media_test_capture_asset(void* context, MediaAssetKey key, bool force_request,
                                      bool* stop_scan) noexcept {
    MessageMediaTestCapture* capture = static_cast<MessageMediaTestCapture*>(context);
    if (!capture || !stop_scan || capture->count >= 16) {
        return false;
    }
    capture->keys[capture->count] = key;
    capture->forces[capture->count] = force_request;
    ++capture->count;
    *stop_scan = capture->stop;
    return !capture->fail;
}

bool message_media_test_capture_avatar(void* context, const UserRecord& user, bool force_request,
                                       bool* stop_scan) noexcept {
    const Utf8View hash = user_store_avatar_hash(user);
    return message_media_test_capture_asset(context, user_avatar_key(user.id, hash), force_request, stop_scan);
}

bool message_media_test_ignore_attachment(void*, const MessageRecord&, const MessageRichAttachment&, float,
                                          float, bool*) noexcept {
    return true;
}

bool message_media_test_capture_attachment(void* context, const MessageRecord&,
                                           const MessageRichAttachment&, float, float,
                                           bool* stop_scan) noexcept {
    MessageMediaTestCapture* capture = static_cast<MessageMediaTestCapture*>(context);
    if (!capture || !stop_scan || capture->count >= 16) {
        return false;
    }
    ++capture->count;
    *stop_scan = false;
    return true;
}

bool message_media_test_ignore_embed(void*, const MessageRecord&, const MessageRichRecord&, u32, float,
                                     float, bool*) noexcept {
    return true;
}

bool message_media_test_captured(const MessageMediaTestCapture& capture, MediaAssetKey key,
                                 bool expected_force) noexcept {
    for (u32 index = 0; index < capture.count; ++index) {
        if (media_asset_key_equal(capture.keys[index], key)) {
            return capture.forces[index] == expected_force;
        }
    }
    return false;
}

} // namespace

void test_message_media_controller() noexcept {
    MessageRecord message_records[2]{};
    u32 message_order[2]{};
    u8 message_text[256]{};
    MessageStore messages{};
    const bool messages_ready =
        message_store_initialize(&messages, Slice<MessageRecord>{message_records, 2},
                                 Slice<u32>{message_order, 2}, Slice<u8>{message_text, 256});
    const MessageStoreResult first_message =
        messages_ready
            ? message_store_upsert(&messages,
                                   MessageInput{100, 7, 10, 1'000, 0,
                                                utf8_literal("<:still:111> <a:wave:222> <:still:111>"), 0})
            : MessageStoreResult{};
    const MessageStoreResult second_message =
        first_message.ok() ? message_store_upsert(&messages, MessageInput{200, 7, 20, 2'000, 0,
                                                                          utf8_literal("<a:wave:222>"), 0})
                           : MessageStoreResult{};

    MessageRichRecord rich_records[2]{};
    u16 rich_order[2]{};
    u8 rich_text[512]{};
    MessageRichStore rich_store{};
    const bool rich_ready =
        message_rich_store_initialize(&rich_store, Slice<MessageRichRecord>{rich_records, 2},
                                      Slice<u16>{rich_order, 2}, Slice<u8>{rich_text, 512});
    GatewayMessageReaction reaction{};
    reaction.emoji_id = 222;
    reaction.name = utf8_literal("wave");
    reaction.count = 3;
    reaction.normal_count = 3;
    reaction.animated = true;
    GatewayMessageSticker sticker{};
    sticker.id = 333;
    sticker.name = utf8_literal("still");
    sticker.format_type = 1;
    GatewayMessageAttachment attachment{};
    attachment.id = 444;
    attachment.filename = utf8_literal("viewport.png");
    attachment.content_type = utf8_literal("image/png");
    attachment.url = utf8_literal("https://cdn.discordapp.com/attachments/7/444/viewport.png");
    attachment.width = 64;
    attachment.height = 64;
    attachment.has_width = true;
    attachment.has_height = true;
    GatewayMessageComponent button{};
    button.type = gateway_component_button;
    button.style = 2;
    button.label = utf8_literal("Download");
    button.custom_id = utf8_literal("download");
    button.emoji_id = 555;
    button.emoji_name = utf8_literal("blueprint");
    button.parent = 0xFFFFu;
    GatewayMessageEvent rich_event{};
    rich_event.id = 100;
    rich_event.fields = gateway_message_field_message_reference | gateway_message_field_referenced_message |
                        gateway_message_field_reactions | gateway_message_field_sticker_items |
                        gateway_message_field_attachments | gateway_message_field_components;
    rich_event.components = ConstSlice<GatewayMessageComponent>{&button, 1};
    rich_event.reference.author_id = 20;
    rich_event.reference.message_id = 90;
    rich_event.reference.channel_id = 7;
    rich_event.reference.state = GatewayMessageReferenceState::available;
    rich_event.reactions = ConstSlice<GatewayMessageReaction>{&reaction, 1};
    rich_event.stickers = ConstSlice<GatewayMessageSticker>{&sticker, 1};
    rich_event.attachments = ConstSlice<GatewayMessageAttachment>{&attachment, 1};
    const MessageRecord* first_record =
        first_message.ok() ? message_store_resolve(messages, first_message.handle) : nullptr;
    const MessageRichStoreResult rich_applied =
        rich_ready && first_record
            ? message_rich_store_apply(&rich_store, first_record->id, first_record->revision, rich_event)
            : MessageRichStoreResult{};

    UserRecord user_records[2]{};
    u32 user_order[2]{};
    u8 user_names[128]{};
    UserStore users{};
    const bool users_ready =
        user_store_initialize(&users, Slice<UserRecord>{user_records, 2},
                                      Slice<u32>{user_order, 2}, Slice<u8>{user_names, 128});
    const Utf8View static_hash = utf8_literal("0123456789abcdef0123456789abcdef");
    const Utf8View animated_hash = utf8_literal("a_fedcba9876543210fedcba9876543210");
    const bool users_seeded = users_ready &&
                              user_store_upsert(&users, 10, utf8_literal("one")).ok() &&
                              user_store_upsert(&users, 20, utf8_literal("two")).ok() &&
                              user_store_set_avatar(&users, 10, static_hash) &&
                              user_store_set_avatar(&users, 20, animated_hash);

    MessageSceneRowWidget scene_rows[2]{};
    scene_rows[0].source_index = 0;
    scene_rows[0].message_id = 100;
    scene_rows[1].source_index = 1;
    scene_rows[1].message_id = 200;
    MessageScene scene{};
    scene.rows = Slice<MessageSceneRowWidget>{scene_rows, 2};
    scene.first = 0;
    scene.count = 2;

    AnimationTimelineEntry animation_entries[8]{};
    u32 animation_buckets[16]{};
    AnimationTimeline animations{};
    const bool animations_ready =
        animation_timeline_initialize(&animations, Slice<AnimationTimelineEntry>{animation_entries, 8},
                                      Slice<u32>{animation_buckets, 16});

    MediaAtlasEntry atlas_entries[16]{};
    u32 atlas_buckets[32]{};
    GlyphAtlasEntry atlas_allocations[16]{};
    GlyphAtlasRow atlas_rows[4]{};
    MediaAtlas atlas{};
    const bool atlas_ready = media_atlas_initialize(
        &atlas, 64, 64, 0, Slice<MediaAtlasEntry>{atlas_entries, 16}, Slice<u32>{atlas_buckets, 32},
        Slice<GlyphAtlasEntry>{atlas_allocations, 16}, Slice<GlyphAtlasRow>{atlas_rows, 4});

    MessageMediaVisibleRow visible_rows[2]{};
    MessageMediaAssetMark asset_marks[16]{};
    MessageMediaController controller{};
    const bool controller_ready =
        message_media_controller_initialize(&controller, Slice<MessageMediaVisibleRow>{visible_rows, 2},
                                            Slice<MessageMediaAssetMark>{asset_marks, 16});
    // Revisits replay each message's parsed inline emoji (counted as parses).
    static MessageMediaEmojiMemoBank emoji_memo{};
    controller.emoji_memo = &emoji_memo;
    MessageMediaTestCapture capture{};
    const MessageMediaControllerCallbacks callbacks{
        &capture, message_media_test_ignore_attachment, message_media_test_ignore_embed,
        message_media_test_capture_avatar, message_media_test_capture_asset};

    message_media_controller_begin_pump(&controller);
    const MessageMediaControllerResult collected = message_media_controller_collect(
        &controller, messages, rich_store, scene, UiVariableRange{0, 2, 0});
    const MessageMediaControllerResult primary =
        message_media_controller_visit_primary(&controller, rich_store, scene, callbacks);
    animation_timeline_begin_visibility(&animations);
    const MessageMediaControllerResult decorated = message_media_controller_visit_decorations(
        &controller, messages, users, &animations, &atlas, 50'000, callbacks);
    animation_timeline_end_visibility(&animations, 50'000);

    const u32 static_variant = user_avatar_variant(static_hash);
    const u32 animated_variant = user_avatar_variant(animated_hash);
    expect(messages_ready && first_message.ok() && second_message.ok() && rich_ready && rich_applied.ok() &&
               users_seeded && animations_ready && atlas_ready && controller_ready && collected.ok() &&
               collected.visited == 2 && collected.candidates == 2 && controller.row_count == 2 &&
               controller.collections == 1 && controller.source_rows_visited == 2 &&
               controller.rows_collected == 2 && primary.ok() && primary.candidates == 0 &&
               decorated.ok() && decorated.candidates == 6 && capture.count == 6 &&
               message_media_test_captured(capture, media_asset_custom_emoji(555, false), false) &&
               message_media_test_captured(capture, media_asset_user_avatar(10, static_variant, false),
                                           false) &&
               message_media_test_captured(capture, media_asset_user_avatar(20, animated_variant, true),
                                           true) &&
               message_media_test_captured(capture, media_asset_custom_emoji(111, false), false) &&
               message_media_test_captured(capture, media_asset_custom_emoji(222, true), true) &&
               message_media_test_captured(capture, media_asset_sticker(333, 1), false) &&
               controller.duplicate_assets_skipped == 4 && emoji_memo.parses == 2,
           "message_media_controller_single_snapshot_and_asset_dedupe");

    // A virtualized row may be taller than the viewport. Its off-screen rich
    // children must not enter the preview-atlas working set merely because a
    // different part of the parent row is visible.
    UiWidget primary_widgets[1]{};
    UiLayoutNode primary_layout[1]{};
    scene.widgets.storage = Slice<UiWidget>{primary_widgets, 1};
    scene.widgets.layout.storage = Slice<UiLayoutNode>{primary_layout, 1};
    scene.widgets.count = 1;
    scene.widgets.generation = 1;
    scene.widgets.layout.count = 1;
    scene.widgets.layout.generation = 1;
    primary_layout[0].generation = 1;
    primary_layout[0].rectangle = UiRect{0.0f, 120.0f, 64.0f, 184.0f};
    primary_layout[0].content_rectangle = primary_layout[0].rectangle;
    scene.viewport = UiRect{0.0f, 0.0f, 100.0f, 100.0f};
    scene_rows[0].attachment_item_count = 1;
    scene_rows[0].rich_items[kMessageSceneAttachmentItemOffset] = UiWidgetHandle{0, 1};
    MessageMediaTestCapture primary_capture{};
    const MessageMediaControllerCallbacks primary_callbacks{
        &primary_capture, message_media_test_capture_attachment, message_media_test_ignore_embed,
        message_media_test_capture_avatar, message_media_test_capture_asset};
    const MessageMediaControllerResult outside_primary =
        message_media_controller_visit_primary(&controller, rich_store, scene, primary_callbacks);
    primary_layout[0].rectangle = UiRect{0.0f, 80.0f, 64.0f, 144.0f};
    primary_layout[0].content_rectangle = primary_layout[0].rectangle;
    const MessageMediaControllerResult intersecting_primary =
        message_media_controller_visit_primary(&controller, rich_store, scene, primary_callbacks);
    expect(outside_primary.ok() && outside_primary.candidates == 0 &&
               intersecting_primary.ok() && intersecting_primary.candidates == 1 &&
               primary_capture.count == 1,
           "message_media_controller_rich_children_require_viewport_intersection");

    const auto visible_not_prefetched = message_media_controller_visit_primary(
        &controller, rich_store, scene, primary_callbacks, true);
    primary_layout[0].rectangle = UiRect{0.0f, 120.0f, 64.0f, 184.0f};
    const auto nearby_primary = message_media_controller_visit_primary(
        &controller, rich_store, scene, primary_callbacks, true);
    primary_layout[0].rectangle = UiRect{0.0f, 400.0f, 64.0f, 464.0f};
    const auto distant_primary = message_media_controller_visit_primary(
        &controller, rich_store, scene, primary_callbacks, true);
    primary_layout[0].rectangle = UiRect{0.0f, -364.0f, 64.0f, -300.0f};
    const auto above_primary = message_media_controller_visit_primary(
        &controller, rich_store, scene, primary_callbacks, true);
    primary_layout[0].rectangle.bottom = -299.0f;
    const auto nearby_above = message_media_controller_visit_primary(
        &controller, rich_store, scene, primary_callbacks, true);
    expect(visible_not_prefetched.ok() && visible_not_prefetched.candidates == 0 &&
               nearby_primary.ok() && nearby_primary.candidates == 1 &&
               distant_primary.ok() && distant_primary.candidates == 0 &&
               above_primary.ok() && above_primary.candidates == 0 &&
               nearby_above.ok() && nearby_above.candidates == 1,
           "message_media_preload_300px_both_directions_excludes_visible_and_far_media");

    const MediaAtlasResult resident =
        media_atlas_allocate(&atlas, media_asset_custom_emoji(111, false), 1, 1);
    capture = MessageMediaTestCapture{};
    message_media_controller_begin_pump(&controller);
    const MessageMediaControllerResult second_collect = message_media_controller_collect(
        &controller, messages, rich_store, scene, UiVariableRange{0, 2, 0});
    animation_timeline_begin_visibility(&animations);
    const MessageMediaControllerResult second_decorated = message_media_controller_visit_decorations(
        &controller, messages, users, &animations, &atlas, 60'000, callbacks);
    animation_timeline_end_visibility(&animations, 60'000);
    expect(resident.ok() && second_collect.ok() && second_decorated.ok() &&
               second_decorated.candidates == 5 && capture.count == 5 &&
               !message_media_test_captured(capture, media_asset_custom_emoji(111, false), false) &&
               controller.collections == 2 && controller.source_rows_visited == 4 &&
               controller.duplicate_assets_skipped == 8 && emoji_memo.parses == 2,
           "message_media_controller_resident_filter_and_epoch_reuse");

    // The official client animates an author avatar only while its message group (or the
    // avatar itself) is hovered; elsewhere a resident animated avatar holds
    // its frame, so a static channel schedules no animation wake.
    const MediaAssetKey animated_avatar = media_asset_user_avatar(20, animated_variant, true);
    const AnimationTimelineResult avatar_entry = animation_timeline_publish(
        &animations, AnimationTimelineDescriptor{animated_avatar, 80'000, 1, 4, 0, true}, 60'000);
    const MediaAtlasResult avatar_resident = media_atlas_allocate(&atlas, animated_avatar, 1, 1);
    const auto avatar_pass = [&](u64 group, u64 now) noexcept {
        capture = MessageMediaTestCapture{};
        message_media_controller_set_avatar_interaction(&controller, group);
        message_media_controller_begin_pump(&controller);
        animation_timeline_begin_visibility(&animations);
        const bool visited =
            message_media_controller_collect(&controller, messages, rich_store, scene, UiVariableRange{0, 2, 0})
                .ok() &&
            message_media_controller_visit_decorations(&controller, messages, users, &animations, &atlas, now,
                                                       callbacks)
                .ok();
        animation_timeline_end_visibility(&animations, now);
        return visited;
    };
    const auto avatar_plays = [&]() noexcept {
        return avatar_entry.ok() &&
               (animations.entries[avatar_entry.slot].flags & animation_timeline_entry_visible) != 0;
    };
    const bool idle_pass = avatar_pass(0, 70'000);
    const bool idle_held = !avatar_plays() && animation_timeline_next_wake_at_us(&animations) == 0 &&
                           !message_media_test_captured(capture, animated_avatar, true);
    const bool hovered_pass = avatar_pass(200, 80'000);
    const bool hovered_plays = avatar_plays() && animation_timeline_next_wake_at_us(&animations) != 0;
    // Hovering the other group leaves this author's avatar (and the reply
    // preview's copy of it) still.
    const bool other_pass = avatar_pass(100, 90'000);
    const bool other_held = !avatar_plays() && animation_timeline_next_wake_at_us(&animations) == 0;
    expect(avatar_entry.ok() && avatar_resident.ok() && idle_pass && idle_held && hovered_pass && hovered_plays &&
               other_pass && other_held,
           "message_media_controller_animated_avatar_plays_only_for_its_hovered_group");

    MessageMediaVisibleRow small_rows[1]{};
    MessageMediaAssetMark small_marks[8]{};
    MessageMediaController small_controller{};
    const bool small_ready =
        message_media_controller_initialize(&small_controller, Slice<MessageMediaVisibleRow>{small_rows, 1},
                                            Slice<MessageMediaAssetMark>{small_marks, 8});
    message_media_controller_begin_pump(&small_controller);
    const MessageMediaControllerResult capacity = message_media_controller_collect(
        &small_controller, messages, rich_store, scene, UiVariableRange{0, 2, 0});
    message_media_controller_observe_retry(&small_controller, 900);
    message_media_controller_observe_retry(&small_controller, 700);
    const bool budget_before = message_media_controller_upload_budget_reached(&small_controller, 3);
    const bool budget_after = message_media_controller_upload_budget_reached(&small_controller, 4);
    const bool first_log = message_media_controller_should_log_capacity(&small_controller, 1'000, 500);
    const bool throttled_log = message_media_controller_should_log_capacity(&small_controller, 1'200, 500);
    const bool next_log = message_media_controller_should_log_capacity(&small_controller, 1'500, 500);
    expect(small_ready && capacity.error == MessageMediaControllerError::capacity &&
               small_controller.rejected == 1 && small_controller.retry_at_ms == 700 &&
               !budget_before && budget_after && small_controller.scan_pending && first_log &&
               !throttled_log && next_log,
           "message_media_controller_capacity_retry_budget_and_throttle");

    MessageMediaController owned{};
    const bool owned_ready = message_media_controller_initialize_owned(&owned, 8, 32);
    expect(owned_ready && owned.owns_storage && owned.rows && owned.asset_marks && owned.row_capacity == 8 &&
               owned.asset_mark_capacity == 32,
           "message_media_controller_owned_storage_initializes");
    message_media_controller_release(&owned);
    expect(!owned.initialized && owned.rows == nullptr && owned.owned_rows.arena.base == nullptr,
           "message_media_controller_owned_storage_releases");
}

// A message author and the author a reply quotes, both without an avatar hash, each ask for their default avatar:
// the shared key of the CDN picture their id selects, still never a per-user hashed avatar.
void test_message_media_default_avatar_candidates() noexcept {
    constexpr u64 author = 30, quoted = 30 + (1ull << 22);
    MessageRecord message_records[1]{};
    u32 message_order[1]{};
    u8 message_text[64]{};
    MessageStore messages{};
    const bool messages_ready = message_store_initialize(&messages, Slice<MessageRecord>{message_records, 1},
                                                         Slice<u32>{message_order, 1}, Slice<u8>{message_text, 64});
    const MessageStoreResult message =
        messages_ready ? message_store_upsert(&messages, MessageInput{100, 7, author, 1'000, 0, utf8_literal("hi"), 0})
                       : MessageStoreResult{};
    MessageRichRecord rich_records[1]{};
    u16 rich_order[1]{};
    u8 rich_text[128]{};
    MessageRichStore rich_store{};
    const bool rich_ready = message_rich_store_initialize(&rich_store, Slice<MessageRichRecord>{rich_records, 1},
                                                          Slice<u16>{rich_order, 1}, Slice<u8>{rich_text, 128});
    GatewayMessageEvent rich_event{};
    rich_event.id = 100;
    rich_event.fields = gateway_message_field_message_reference | gateway_message_field_referenced_message;
    rich_event.reference.author_id = quoted;
    rich_event.reference.message_id = 90;
    rich_event.reference.channel_id = 7;
    rich_event.reference.state = GatewayMessageReferenceState::available;
    const MessageRecord* record = message.ok() ? message_store_resolve(messages, message.handle) : nullptr;
    const MessageRichStoreResult rich_applied =
        rich_ready && record ? message_rich_store_apply(&rich_store, record->id, record->revision, rich_event)
                             : MessageRichStoreResult{};

    UserRecord user_records[2]{};
    u32 user_order[2]{};
    u8 user_names[128]{};
    UserStore users{};
    const bool users_seeded =
        user_store_initialize(&users, Slice<UserRecord>{user_records, 2}, Slice<u32>{user_order, 2},
                              Slice<u8>{user_names, 128}) &&
        user_store_upsert(&users, author, utf8_literal("one")).ok() &&
        user_store_upsert(&users, quoted, utf8_literal("two")).ok();

    MessageSceneRowWidget scene_rows[1]{};
    scene_rows[0].source_index = 0;
    scene_rows[0].message_id = 100;
    MessageScene scene{};
    scene.rows = Slice<MessageSceneRowWidget>{scene_rows, 1};
    scene.first = 0;
    scene.count = 1;

    AnimationTimelineEntry animation_entries[4]{};
    u32 animation_buckets[8]{};
    AnimationTimeline animations{};
    MediaAtlasEntry atlas_entries[4]{};
    u32 atlas_buckets[8]{};
    GlyphAtlasEntry atlas_allocations[4]{};
    GlyphAtlasRow atlas_rows[2]{};
    MediaAtlas atlas{};
    MessageMediaVisibleRow visible_rows[1]{};
    MessageMediaAssetMark asset_marks[8]{};
    MessageMediaController controller{};
    const bool ready =
        animation_timeline_initialize(&animations, Slice<AnimationTimelineEntry>{animation_entries, 4},
                                      Slice<u32>{animation_buckets, 8}) &&
        media_atlas_initialize(&atlas, 64, 64, 0, Slice<MediaAtlasEntry>{atlas_entries, 4},
                               Slice<u32>{atlas_buckets, 8}, Slice<GlyphAtlasEntry>{atlas_allocations, 4},
                               Slice<GlyphAtlasRow>{atlas_rows, 2}) &&
        message_media_controller_initialize(&controller, Slice<MessageMediaVisibleRow>{visible_rows, 1},
                                            Slice<MessageMediaAssetMark>{asset_marks, 8});
    static MessageMediaEmojiMemoBank emoji_memo{};
    controller.emoji_memo = &emoji_memo;
    MessageMediaTestCapture capture{};
    const MessageMediaControllerCallbacks callbacks{&capture, message_media_test_ignore_attachment,
                                                    message_media_test_ignore_embed, message_media_test_capture_avatar,
                                                    message_media_test_capture_asset};
    message_media_controller_begin_pump(&controller);
    const MessageMediaControllerResult collected =
        message_media_controller_collect(&controller, messages, rich_store, scene, UiVariableRange{0, 1, 0});
    animation_timeline_begin_visibility(&animations);
    const MessageMediaControllerResult decorated = message_media_controller_visit_decorations(
        &controller, messages, users, &animations, &atlas, 50'000, callbacks);
    animation_timeline_end_visibility(&animations, 50'000);
    expect(messages_ready && message.ok() && rich_ready && rich_applied.ok() && users_seeded && ready &&
               collected.ok() && decorated.ok() && decorated.candidates == 2 && capture.count == 2 &&
               message_media_test_captured(capture, default_avatar_key(default_avatar_index(author)), false) &&
               message_media_test_captured(capture, default_avatar_key(default_avatar_index(quoted)), false) &&
               !media_asset_key_equal(default_avatar_key(default_avatar_index(author)),
                                      default_avatar_key(default_avatar_index(quoted))),
           "message_media_controller_requests_default_avatars_for_authors_without_a_hash");
}

void test_media_presentation_policy() noexcept {
    expect(media_asset_key_equal(media_asset_attachment(70,MediaAttachmentFormat::mp4,MediaResolutionTier::px128),
                                 media_asset_attachment(70,MediaAttachmentFormat::mp4,MediaResolutionTier::px512))&&
           media_asset_key_equal(media_asset_embed(80,12,3,MediaEmbedResource::video,MediaAttachmentFormat::mp4,true,MediaResolutionTier::px128),
                                 media_asset_embed(80,12,3,MediaEmbedResource::video,MediaAttachmentFormat::mp4,true,MediaResolutionTier::px512)),
           "native_mp4_source_identity_survives_presentation_resize");
    const MediaPresentationRequest compact = media_presentation_request(1'600, 900, 120.0f, 67.5f, 96, 512);
    const MediaPresentationRequest hidpi = media_presentation_request(1'600, 900, 120.0f, 67.5f, 192, 512);
    const MediaPresentationRequest capped =
        media_presentation_request(4'000, 1'000, 900.0f, 225.0f, 192, 512);
    const MediaPresentationRequest no_upscale =
        media_presentation_request(40, 20, 500.0f, 250.0f, 192, 512);
    expect(compact.resolution == MediaResolutionTier::px128 && compact.width == 128 &&
               compact.height == 72 && hidpi.resolution == MediaResolutionTier::px256 &&
               hidpi.width == 256 && hidpi.height == 144 &&
               capped.resolution == MediaResolutionTier::px512 && capped.width == 512 &&
               capped.height == 128 && no_upscale.resolution == MediaResolutionTier::px512 &&
               no_upscale.width == 40 && no_upscale.height == 20,
           "media_presentation_policy_aspect_scale_and_caps");
    // A 1920x1080 JPEG screenshot in a 550 DIP wide preview.
    const MediaPresentationRequest screenshot = media_presentation_request(
        1'920, 1'080, 550.0f, 309.375f, 96, media_preview_extent_cap(MediaAttachmentFormat::jpeg));
    expect(media_preview_extent_cap(MediaAttachmentFormat::jpeg) == 2048 &&
               media_preview_extent_cap(MediaAttachmentFormat::webp) == 2048 &&
               media_preview_extent_cap(MediaAttachmentFormat::webp, true) == 512 &&
               screenshot.resolution == MediaResolutionTier::px1024 && screenshot.width == 1024 &&
               screenshot.height == 576,
           "media_presentation_large_jpeg_previews_share_the_png_cap");

    const MediaAssetKey attachment =
        media_asset_attachment(70, MediaAttachmentFormat::png, compact.resolution);
    const MediaAssetKey attachment_upgrade =
        media_asset_attachment(70, MediaAttachmentFormat::png, hidpi.resolution);
    const MediaAssetKey embed =
        media_asset_embed(80, 12, 3, MediaEmbedResource::thumbnail, MediaAttachmentFormat::png, false,
                          MediaResolutionTier::px256);
    expect(media_asset_key_valid(attachment) &&
               media_asset_attachment_format(attachment) == MediaAttachmentFormat::png &&
               media_asset_resolution_tier(attachment) == MediaResolutionTier::px128 &&
               !media_asset_key_equal(attachment, attachment_upgrade) && media_asset_key_valid(embed) &&
               media_asset_embed_index(embed) == 3 &&
               media_asset_embed_resource(embed) == MediaEmbedResource::thumbnail &&
               media_asset_embed_format(embed) == MediaAttachmentFormat::png &&
               media_asset_resolution_tier(embed) == MediaResolutionTier::px256,
           "media_presentation_scale_participates_in_exact_identity");
}

void test_media_atlas_allocator() noexcept {
    MediaAtlasEntry entries[4]{};
    u32 buckets[8]{};
    GlyphAtlasEntry allocations[4]{};
    GlyphAtlasRow rows[2]{};
    MediaAtlas atlas{};
    expect(media_atlas_initialize(&atlas, 16, 16, 0, Slice<MediaAtlasEntry>{entries, 4},
                                  Slice<u32>{buckets, 8}, Slice<GlyphAtlasEntry>{allocations, 4},
                                  Slice<GlyphAtlasRow>{rows, 2}),
           "media_atlas_initialize");

    const MediaAssetKey emoji = media_asset_custom_emoji(44, true);
    const MediaAssetKey sticker = media_asset_sticker(44, 2);
    const MediaAtlasResult first = media_atlas_allocate(&atlas, emoji, 8, 8);
    const MediaAtlasResult second = media_atlas_allocate(&atlas, sticker, 8, 8);
    const MediaAtlasResult third = media_atlas_allocate(&atlas, media_asset_attachment(70), 8, 8);
    const MediaAtlasResult fourth = media_atlas_allocate(&atlas, media_asset_sticker(88, 1), 8, 8);
    expect(first.ok() && second.ok() && third.ok() && fourth.ok() &&
               first.handle.slot != second.handle.slot && media_atlas_find(&atlas, emoji).cached &&
               media_atlas_validate(atlas, first.handle) && atlas.entry_count == 4,
           "media_atlas_exact_key_pack_and_validate");

    media_atlas_begin_visibility(&atlas);
    MediaAtlasResult resolved_visible{};
    const bool resolved = media_atlas_resolve_visible(&atlas, first.handle, &resolved_visible);
    const MediaAtlasEntry& resolved_entry = atlas.entries[first.handle.slot];
    expect(resolved && resolved_visible.handle.slot == first.handle.slot &&
               resolved_visible.handle.generation == first.handle.generation &&
               resolved_entry.allocation.slot < atlas.allocations.entry_capacity &&
               glyph_atlas_is_pinned(atlas.allocations, resolved_entry.allocation),
           "media_atlas_generation_handle_visible_resolve");

    media_atlas_begin_visibility(&atlas);
    expect(media_atlas_find(&atlas, media_asset_attachment(70)).ok(), "media_atlas_lru_touch");
    const MediaAtlasResult replacement =
        media_atlas_allocate(&atlas, media_asset_custom_emoji(99, false), 8, 8);
    // A full atlas gives the least recently used slot that is not visible (the sticker) to the request.
    expect(replacement.ok() && replacement.evicted_entries == 1 &&
               media_atlas_find(&atlas, sticker).error == MediaAtlasError::not_found &&
               media_atlas_find(&atlas, emoji).ok() && media_atlas_find(&atlas, media_asset_attachment(70)).ok() &&
               !media_atlas_validate(atlas, second.handle) && atlas.entry_count == 4,
           "media_atlas_slot_lru_eviction_and_stale_handle");

    MediaAtlasEntry protected_entries[4]{};
    u32 protected_buckets[8]{};
    GlyphAtlasEntry protected_allocations[4]{};
    GlyphAtlasRow protected_rows[2]{};
    MediaAtlas protected_atlas{};
    const bool protected_ready = media_atlas_initialize(
        &protected_atlas, 16, 16, 0, Slice<MediaAtlasEntry>{protected_entries, 4},
        Slice<u32>{protected_buckets, 8}, Slice<GlyphAtlasEntry>{protected_allocations, 4},
        Slice<GlyphAtlasRow>{protected_rows, 2});
    const MediaAtlasResult protected_old =
        protected_ready ? media_atlas_allocate(&protected_atlas, media_asset_attachment(100), 16, 8)
                        : MediaAtlasResult{};
    const MediaAtlasResult protected_live =
        protected_old.ok() ? media_atlas_allocate(&protected_atlas, media_asset_attachment(101), 16, 8)
                           : MediaAtlasResult{};
    media_atlas_begin_visibility(&protected_atlas);
    const MediaAtlasResult protected_visible =
        media_atlas_find(&protected_atlas, media_asset_attachment(101));
    const MediaAtlasResult protected_pressure =
        protected_visible.ok() ? media_atlas_allocate(&protected_atlas, media_asset_attachment(102), 16, 16)
                               : MediaAtlasResult{};
    expect(protected_ready && protected_old.ok() && protected_live.ok() && protected_visible.ok() &&
               protected_pressure.error == MediaAtlasError::no_capacity &&
               !protected_pressure.atlas_reset && protected_atlas.allocations.full_resets == 0 &&
               media_atlas_validate(protected_atlas, protected_old.handle) &&
               media_atlas_validate(protected_atlas, protected_live.handle),
           "media_atlas_visible_assets_survive_capacity_pressure");

    MediaAtlasEntry identity_entries[1]{};
    u32 identity_buckets[2]{};
    GlyphAtlasEntry identity_allocations[1]{};
    GlyphAtlasRow identity_rows[2]{};
    MediaAtlas identity_atlas{};
    const bool identity_ready = media_atlas_initialize(
        &identity_atlas, 16, 16, 0, Slice<MediaAtlasEntry>{identity_entries, 1},
        Slice<u32>{identity_buckets, 2}, Slice<GlyphAtlasEntry>{identity_allocations, 1},
        Slice<GlyphAtlasRow>{identity_rows, 2});
    const MediaAtlasResult identity_live =
        identity_ready ? media_atlas_allocate(&identity_atlas, media_asset_user_avatar(500, 1, false), 8, 8)
                       : MediaAtlasResult{};
    const MediaAtlasResult identity_pressure =
        identity_live.ok() ? media_atlas_allocate(&identity_atlas, media_asset_attachment(501), 8, 8)
                           : MediaAtlasResult{};
    expect(identity_ready && identity_live.ok() &&
               identity_pressure.error == MediaAtlasError::no_capacity && !identity_pressure.atlas_reset &&
               identity_atlas.allocations.full_resets == 0 &&
               media_atlas_validate(identity_atlas, identity_live.handle),
           "media_atlas_identity_capacity_never_resets_visible_assets");

    expect(media_atlas_allocate(&atlas, media_asset_attachment(70), 4, 8).error ==
                   MediaAtlasError::invalid_input &&
               media_atlas_allocate(&atlas, MediaAssetKey{1, 0, MediaAssetKind::none, 0}, 1, 1).error ==
                   MediaAtlasError::invalid_input &&
               media_atlas_allocate(&atlas, MediaAssetKey{1, 0, MediaAssetKind::sticker, 2}, 1, 1).error ==
                   MediaAtlasError::invalid_input,
           "media_atlas_dimension_key_and_flag_bounds");

    expect(media_atlas_allocate(&atlas, MediaAssetKey{1, 0, MediaAssetKind::sticker, 0}, 1, 1).error ==
                   MediaAtlasError::invalid_input &&
               media_atlas_allocate(&atlas, MediaAssetKey{1, 9, MediaAssetKind::custom_emoji, 0}, 1, 1)
                       .error == MediaAtlasError::invalid_input &&
               media_atlas_allocate(
                   &atlas, MediaAssetKey{1, 0, MediaAssetKind::attachment, media_asset_flag_animated}, 1, 1)
                       .error == MediaAtlasError::invalid_input,
           "media_atlas_kind_specific_key_validation");

    const MediaAtlasHandle before_reset = third.handle;
    media_atlas_reset(&atlas);
    const MediaAtlasResult after_reset = media_atlas_allocate(&atlas, media_asset_attachment(70), 8, 8);
    expect(after_reset.ok() && atlas.entry_count == 1 && !media_atlas_validate(atlas, before_reset) &&
               media_atlas_validate(atlas, after_reset.handle),
           "media_atlas_reset_generation");

    static MediaAtlasEntry scale_entries[4'096]{};
    static u32 scale_buckets[8'192]{};
    static GlyphAtlasEntry scale_allocations[4'096]{};
    static GlyphAtlasRow scale_rows[64]{};
    MediaAtlas scale_atlas{};
    bool scale_ok = media_atlas_initialize(
        &scale_atlas, 64, 64, 0, Slice<MediaAtlasEntry>{scale_entries, 4'096},
        Slice<u32>{scale_buckets, 8'192}, Slice<GlyphAtlasEntry>{scale_allocations, 4'096},
        Slice<GlyphAtlasRow>{scale_rows, 64});
    for (u32 index = 0; index < 4'096 && scale_ok; ++index) {
        scale_ok =
            media_atlas_allocate(&scale_atlas, media_asset_custom_emoji(100'000 + index, false), 1, 1).ok();
    }
    for (u32 index = 0; index < 4'096 && scale_ok; ++index) {
        const MediaAtlasResult found =
            media_atlas_find(&scale_atlas, media_asset_custom_emoji(100'000 + index, false));
        scale_ok = found.ok();
    }
    for (u32 index = 0; index < 4'096 && scale_ok; ++index) {
        scale_ok = media_atlas_find(&scale_atlas, media_asset_custom_emoji(1'000'000 + index, false)).error ==
                   MediaAtlasError::not_found;
    }
    expect(scale_ok && scale_atlas.entry_count == 4'096, "media_atlas_4096_hit_and_absent_hash_lookup");
}

void test_presentation_damage_tracker() noexcept {
    PresentationDamageTracker tracker{};
    presentation_damage_reset(&tracker);
    const MediaAtlasHandle first{7, 3};
    const MediaAtlasHandle second{19, 4};
    expect(presentation_damage_empty(tracker) &&
               !presentation_damage_note_media(
                   &tracker, MediaAtlasHandle{static_cast<u16>(kPresentationDamageMediaCapacity), 1}) &&
               presentation_damage_note_media(&tracker, first) &&
               presentation_damage_media_pending(tracker, first) &&
               !presentation_damage_current(tracker, 100.0f, 80.0f).valid,
           "presentation_damage_generation_handle_note_is_bounded");

    const PresentationDamageRect first_rectangle{10.0f, 10.0f, 30.0f, 30.0f};
    const PresentationDamageRect first_clip{15.0f, 5.0f, 25.0f, 25.0f};
    expect(presentation_damage_add_media(&tracker, first, first_rectangle, first_clip),
           "presentation_damage_visible_media_accumulate");
    PresentationDamage damage = presentation_damage_current(tracker, 100.0f, 80.0f);
    expect(damage.valid && !damage.full && damage.rectangle.left == 15.0f &&
               damage.rectangle.top == 10.0f && damage.rectangle.right == 25.0f &&
               damage.rectangle.bottom == 25.0f,
           "presentation_damage_intersects_destination_and_clip");

    expect(presentation_damage_note_media(&tracker, second) &&
               presentation_damage_add_media(&tracker, second,
                                             PresentationDamageRect{70.0f, 60.0f, 120.0f, 100.0f},
                                             PresentationDamageRect{0.0f, 0.0f, 90.0f, 70.0f}),
           "presentation_damage_second_visible_media_accumulate");
    damage = presentation_damage_current(tracker, 100.0f, 80.0f);
    expect(damage.valid && !damage.full && damage.rectangle.left == 15.0f &&
               damage.rectangle.top == 10.0f && damage.rectangle.right == 90.0f &&
               damage.rectangle.bottom == 70.0f &&
               presentation_damage_current(tracker, 100.0f, 80.0f).valid,
           "presentation_damage_union_survives_until_commit");

    presentation_damage_commit(&tracker);
    expect(presentation_damage_empty(tracker) && !presentation_damage_media_pending(tracker, first) &&
               !presentation_damage_current(tracker, 100.0f, 80.0f).valid,
           "presentation_damage_successful_present_commit");

    presentation_damage_invalidate_full(&tracker);
    damage = presentation_damage_current(tracker, 100.0f, 80.0f);
    expect(damage.valid && damage.full && damage.rectangle.left == 0.0f && damage.rectangle.top == 0.0f &&
               damage.rectangle.right == 100.0f && damage.rectangle.bottom == 80.0f,
           "presentation_damage_conservative_full_fallback");

    presentation_damage_reset(&tracker);
    expect(presentation_damage_note_media(&tracker, first) &&
               presentation_damage_note_media(&tracker, first) && tracker.dirty_media_count == 1 &&
               presentation_damage_media_pending(tracker, first),
           "presentation_damage_duplicate_note_coalesces");
}

void test_animation_frame_cache() noexcept {
    AnimationFrameCacheEntry entries[4]{};
    u32 buckets[8]{};
    GlyphAtlasEntry allocations[4]{};
    GlyphAtlasRow rows[2]{};
    AnimationFrameCache cache{};
    expect(animation_frame_cache_initialize(&cache, 16, 16, 0, Slice<AnimationFrameCacheEntry>{entries, 4},
                                            Slice<u32>{buckets, 8}, Slice<GlyphAtlasEntry>{allocations, 4},
                                            Slice<GlyphAtlasRow>{rows, 2}),
           "animation_frame_cache_initialize");

    const MediaAssetKey emoji = media_asset_custom_emoji(701, true);
    AnimationFrameCacheDescriptor descriptors[4]{};
    for (u32 index = 0; index < 4; ++index) {
        descriptors[index].key = AnimationFrameCacheKey{emoji, index, 3, 0};
        descriptors[index].control.gif = GifFrame{};
        descriptors[index].control.gif.data_offset = index;
        descriptors[index].control.gif.data_end = index * 8;
        descriptors[index].control.gif.width = 8;
        descriptors[index].control.gif.height = 8;
        descriptors[index].control.gif.x_offset = static_cast<u16>(257 + index);
        descriptors[index].control.gif.y_offset = static_cast<u16>(513 + index);
        descriptors[index].control.gif.delay_cs = 5;
        descriptors[index].control.gif.dispose = GifDispose::none;
        descriptors[index].duration_us = 16'667 + index;
        descriptors[index].width = 8;
        descriptors[index].height = 8;
        descriptors[index].kind = AnimationFrameCacheKind::gif;
        expect(animation_frame_cache_allocate(&cache, descriptors[index]).ok(),
               "animation_frame_cache_capture_frame");
    }
    const AnimationFrameCacheResult exact =
        animation_frame_cache_find(&cache, AnimationFrameCacheKey{emoji, 2, 3, 0});
    expect(exact.ok() && exact.cached && exact.frame_index == 2 && exact.source_generation == 3 &&
               exact.duration_us == 16'669 && exact.control.gif.delay_cs == 5 &&
               exact.control.gif.x_offset == 259 && exact.control.gif.y_offset == 515 &&
               exact.allocation.width == 8 && exact.allocation.height == 8 &&
               animation_frame_cache_find(&cache, AnimationFrameCacheKey{emoji, 2, 4, 0}).error ==
                   AnimationFrameCacheError::not_found,
           "animation_frame_cache_exact_frame_generation_identity");

    animation_frame_cache_begin_visibility(&cache);
    expect(animation_frame_cache_find(&cache, AnimationFrameCacheKey{emoji, 2, 3, 0}).ok(),
           "animation_frame_cache_visible_frame_pin");
    AnimationFrameCacheDescriptor pressure = descriptors[0];
    pressure.key = AnimationFrameCacheKey{media_asset_custom_emoji(702, true), 0, 1, 0};
    const AnimationFrameCacheResult replacement = animation_frame_cache_allocate(&cache, pressure);
    expect(replacement.ok() && cache.counters.evictions == 1 &&
               animation_frame_cache_find(&cache, AnimationFrameCacheKey{emoji, 0, 3, 0}).error ==
                   AnimationFrameCacheError::not_found &&
               animation_frame_cache_find(&cache, AnimationFrameCacheKey{emoji, 2, 3, 0}).ok(),
           "animation_frame_cache_slot_eviction_preserves_visible_frame");

    AnimationFrameCacheEntry grow_entries[8]{};
    u32 grow_buckets[16]{};
    GlyphAtlasEntry grow_allocations[8]{};
    GlyphAtlasRow grow_rows[4]{};
    AnimationFrameCache grow{};
    bool grow_ok = animation_frame_cache_initialize(
        &grow, 32, 32, 0, Slice<AnimationFrameCacheEntry>{grow_entries, 2}, Slice<u32>{grow_buckets, 4},
        Slice<GlyphAtlasEntry>{grow_allocations, 2}, Slice<GlyphAtlasRow>{grow_rows, 4});
    for (u32 index = 0; index < 2 && grow_ok; ++index) {
        grow_ok = animation_frame_cache_allocate(&grow, descriptors[index]).ok();
    }
    grow_ok = grow_ok && animation_frame_cache_grow_storage(
                             &grow, Slice<AnimationFrameCacheEntry>{grow_entries, 8},
                             Slice<u32>{grow_buckets, 16}, Slice<GlyphAtlasEntry>{grow_allocations, 8});
    for (u32 index = 0; index < 2 && grow_ok; ++index) {
        grow_ok = animation_frame_cache_find(&grow, descriptors[index].key).ok();
    }
    for (u32 index = 2; index < 4 && grow_ok; ++index) {
        grow_ok = animation_frame_cache_allocate(&grow, descriptors[index]).ok();
    }
    expect(grow_ok && grow.entry_capacity == 8 && grow.bucket_capacity == 16 &&
               grow.allocations.entry_capacity == 8 && grow.entry_count == 4,
           "animation_frame_cache_stable_storage_growth_preserves_entries");
    auto shared = descriptors[0];
    shared.key.content_identity = 500;
    const auto shared_first = animation_frame_cache_allocate(&grow, shared);
    const AnimationFrameCacheKey shared_alias{media_asset_custom_emoji(799, true), 0, 99, 0, 500};
    const auto shared_hit = animation_frame_cache_find(&grow, shared_alias);
    expect(shared_first.ok() && shared_hit.ok() && shared_hit.allocation.handle.slot == shared_first.allocation.handle.slot &&
               !animation_frame_cache_find(&grow, {shared_alias.asset, 0, 99, 0, 501}).ok() &&
               !animation_frame_cache_find(&grow, {shared_alias.asset, 0, 99, 0, 0}).ok(),
           "animation_frame_cache_shared_content_preserves_frame_and_source_identity");

    static AnimationFrameCacheEntry scale_entries[4'096]{};
    static u32 scale_buckets[8'192]{};
    static GlyphAtlasEntry scale_allocations[4'096]{};
    static GlyphAtlasRow scale_rows[64]{};
    AnimationFrameCache scale{};
    bool scale_ok = animation_frame_cache_initialize(
        &scale, 64, 64, 0, Slice<AnimationFrameCacheEntry>{scale_entries, 4'096},
        Slice<u32>{scale_buckets, 8'192}, Slice<GlyphAtlasEntry>{scale_allocations, 4'096},
        Slice<GlyphAtlasRow>{scale_rows, 64});
    for (u32 index = 0; index < 4'096 && scale_ok; ++index) {
        AnimationFrameCacheDescriptor descriptor{};
        descriptor.key = AnimationFrameCacheKey{media_asset_custom_emoji(800'000 + index, true), index & 7u,
                                                static_cast<u16>((index >> 3) + 1), 0};
        descriptor.control.gif.delay_cs = 2;
        descriptor.duration_us = 20'000;
        descriptor.width = 1;
        descriptor.height = 1;
        descriptor.kind = AnimationFrameCacheKind::gif;
        const AnimationFrameCacheResult inserted = animation_frame_cache_allocate(&scale, descriptor);
        scale_ok = inserted.ok();
    }
    animation_frame_cache_begin_visibility(&scale);
    for (u32 index = 0; index < 4'096 && scale_ok; ++index) {
        const AnimationFrameCacheResult found = animation_frame_cache_find(
            &scale, AnimationFrameCacheKey{media_asset_custom_emoji(800'000 + index, true), index & 7u,
                                           static_cast<u16>((index >> 3) + 1), 0});
        scale_ok = found.ok();
    }
    expect(scale_ok && scale.entry_count == 4'096, "animation_frame_cache_4096_capture_lookup");

    // Row evictions erase buckets by backward shift (no tombstones). Under churn
    // every resident entry must stay reachable from its home bucket, every
    // bucket must name a resident entry, and lookups must match residency.
    AnimationFrameCacheEntry churn_entries[16]{};
    u32 churn_buckets[33]{}; // Exercise non-power-of-two probe wrapping.
    GlyphAtlasEntry churn_allocations[16]{};
    GlyphAtlasRow churn_rows[8]{};
    AnimationFrameCache churn{};
    bool churn_ok = animation_frame_cache_initialize(
        &churn, 32, 32, 0, Slice<AnimationFrameCacheEntry>{churn_entries, 16}, Slice<u32>{churn_buckets, 33},
        Slice<GlyphAtlasEntry>{churn_allocations, 16}, Slice<GlyphAtlasRow>{churn_rows, 8});
    const auto churn_key = [](u32 id) noexcept {
        return AnimationFrameCacheKey{media_asset_custom_emoji(900'000 + id / 4, true), id % 4, 1, 0};
    };
    const auto churn_consistent = [&]() noexcept {
        u32 resident = 0, used_buckets = 0;
        for (u32 bucket = 0; bucket < churn.bucket_capacity; ++bucket) {
            if (!churn_buckets[bucket])
                continue;
            ++used_buckets;
            const u32 slot = churn_buckets[bucket] - 1;
            if (slot >= churn.entry_capacity || !churn_entries[slot].occupied)
                return false;
        }
        for (u32 slot = 0; slot < churn.entry_capacity; ++slot) {
            if (!churn_entries[slot].occupied)
                continue;
            ++resident;
            bool reached = false;
            for (u32 step = 0, bucket = static_cast<u32>(churn_entries[slot].home_bucket);
                 step < churn.bucket_capacity && churn_buckets[bucket] && !reached;
                 ++step, bucket = (bucket + 1) % churn.bucket_capacity)
                reached = churn_buckets[bucket] == slot + 1;
            if (!reached)
                return false;
        }
        return resident == churn.entry_count && used_buckets == resident;
    };
    u32 churn_seed = 0x2545F491u, churn_checks = 0;
    for (u32 step = 0; step < 1'000 && churn_ok; ++step) {
        churn_seed = churn_seed * 1'103'515'245u + 12'345u;
        const u32 id = (churn_seed >> 8) % 96;
        if (!animation_frame_cache_find(&churn, churn_key(id)).ok()) {
            AnimationFrameCacheDescriptor descriptor{};
            descriptor.key = churn_key(id);
            descriptor.duration_us = 20'000;
            descriptor.width = static_cast<u16>(4 + (churn_seed >> 20) % 13);
            descriptor.height = static_cast<u16>(4 + (churn_seed >> 24) % 5);
            descriptor.kind = AnimationFrameCacheKind::gif;
            const auto inserted = animation_frame_cache_allocate(&churn, descriptor);
            churn_ok = inserted.ok() || inserted.error == AnimationFrameCacheError::no_capacity;
        }
        if (step % 4 == 3)
            animation_frame_cache_begin_visibility(&churn);
        churn_ok = churn_ok && churn_consistent();
        if (churn_ok && step % 250 == 249) {
            animation_frame_cache_begin_visibility(&churn);
            for (u32 other = 0; other < 96 && churn_ok; ++other) {
                const auto key = churn_key(other);
                bool resident = false;
                for (u32 slot = 0; slot < churn.entry_capacity; ++slot)
                    resident = resident || (churn_entries[slot].occupied && churn_entries[slot].key.asset.id == key.asset.id &&
                                            churn_entries[slot].key.frame_index == key.frame_index &&
                                            glyph_atlas_validate(churn.allocations, churn_entries[slot].allocation));
                churn_ok = animation_frame_cache_find(&churn, key).ok() == resident;
            }
            animation_frame_cache_begin_visibility(&churn);
            ++churn_checks;
        }
    }
    expect(churn_ok && churn_checks == 4 && churn.counters.evictions > 100,
           "animation_frame_cache_backward_shift_erase_keeps_every_entry_reachable");
}

} // namespace maicord
