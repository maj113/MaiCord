#include "application_state.hpp"

// Client-only definitions of the CDN URL functions (protocol/cdn_url.hpp). The
// account module owns the only copy (AccountModuleCdnUrls); the client parses
// media, download and picker URLs only for work a loaded module serves.
// Without a module each entry fails like invalid input, which every caller
// already handles. guild_icon_hash_valid is not forwarded: the client checks
// it per Gateway user record, so both images keep that small function.
namespace maicord {
namespace {

const AccountModuleCdnUrls* cdn_urls() noexcept {
    const AccountHost& host = app_detail::g_app.account;
    return host.module ? &host.api.cdn_urls : nullptr;
}

constexpr MediaUrlResult media_unavailable() noexcept {
    return {0, CdnHost::none, MediaUrlError::invalid_input};
}

constexpr CdnTargetResult target_unavailable() noexcept {
    return {0, CdnHost::none, CdnTargetError::invalid_input};
}

} // namespace

AttachmentUrlResult parse_signed_attachment_url(Utf8View url, u64 attachment_id,
                                                Slice<u8> target_output) noexcept {
    if (const auto* c = cdn_urls())
        return c->parse_signed_attachment_url(url, attachment_id, target_output);
    AttachmentUrlResult result{};
    result.error = AttachmentUrlError::invalid_input;
    return result;
}

MediaUrlResult parse_media_url(Utf8View url, Slice<u8> target_output) noexcept {
    const auto* c = cdn_urls();
    return c ? c->parse_media_url(url, target_output) : media_unavailable();
}

bool catalog_provider_target_valid(CdnHost host, ConstSlice<u8> target) noexcept {
    const auto* c = cdn_urls();
    return c && c->catalog_provider_target_valid(host, target);
}

bool sticker_json_target_valid(ConstSlice<u8> target) noexcept {
    const auto* c = cdn_urls();
    return c && c->sticker_json_target_valid(target);
}

bool soundboard_target_valid(ConstSlice<u8> target) noexcept {
    const auto* c = cdn_urls();
    return c && c->soundboard_target_valid(target);
}

MediaUrlResult parse_catalog_preview_url(Utf8View url, Slice<u8> target_output,
                                         MediaAttachmentFormat format, u32 width, u32 height) noexcept {
    const auto* c = cdn_urls();
    return c ? c->parse_catalog_preview_url(url, target_output, format, width, height)
             : media_unavailable();
}

MediaAttachmentFormat catalog_favorite_format(Utf8View src) noexcept {
    const auto* c = cdn_urls();
    return c ? c->catalog_favorite_format(src) : MediaAttachmentFormat::webp;
}

u32 media_url_log_fields(Utf8View url, Slice<char> output) noexcept {
    const auto* c = cdn_urls();
    return c ? c->media_url_log_fields(url, output) : 0;
}

CdnTargetResult write_media_proxy_target(ConstSlice<u8> source_target, CdnHost source_host,
                                         u32 target_width, u32 target_height, CdnMediaFormat format,
                                         Slice<u8> output, bool animated) noexcept {
    const auto* c = cdn_urls();
    return c ? c->write_media_proxy_target(source_target, source_host, target_width, target_height, format,
                                           output, animated)
             : target_unavailable();
}

CdnTargetResult write_custom_emoji_cdn_target(u64 emoji_id, bool animated, u32 size,
                                              Slice<u8> output) noexcept {
    const auto* c = cdn_urls();
    return c ? c->write_custom_emoji_cdn_target(emoji_id, animated, size, output) : target_unavailable();
}

u32 cdn_guild_icon_variant(Utf8View icon_hash) noexcept {
    const auto* c = cdn_urls();
    return c ? c->cdn_guild_icon_variant(icon_hash) : 0;
}

CdnTargetResult write_guild_icon_cdn_target(u64 guild_id, Utf8View icon_hash, u32 size, Slice<u8> output,
                                            bool animate) noexcept {
    const auto* c = cdn_urls();
    return c ? c->write_guild_icon_cdn_target(guild_id, icon_hash, size, output, animate)
             : target_unavailable();
}

CdnTargetResult write_user_avatar_cdn_target(u64 user_id, Utf8View avatar_hash, u32 size,
                                             Slice<u8> output) noexcept {
    const auto* c = cdn_urls();
    return c ? c->write_user_avatar_cdn_target(user_id, avatar_hash, size, output) : target_unavailable();
}

CdnTargetResult write_channel_icon_cdn_target(u64 channel_id, Utf8View icon_hash, u32 size,
                                              Slice<u8> output) noexcept {
    const auto* c = cdn_urls();
    return c ? c->write_channel_icon_cdn_target(channel_id, icon_hash, size, output) : target_unavailable();
}

CdnTargetResult write_sticker_cdn_target(u64 sticker_id, u32 format_type, Slice<u8> output) noexcept {
    const auto* c = cdn_urls();
    return c ? c->write_sticker_cdn_target(sticker_id, format_type, output) : target_unavailable();
}

Utf8View cdn_host_name(CdnHost host) noexcept {
    const auto* c = cdn_urls();
    return c ? c->cdn_host_name(host) : Utf8View{};
}

} // namespace maicord
