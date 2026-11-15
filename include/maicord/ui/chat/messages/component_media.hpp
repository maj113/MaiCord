#pragma once

#include "maicord/media/core/asset_key.hpp"
#include "maicord/protocol/guild/invite.hpp"

namespace maicord {
struct MessageRichComponent;
struct MessageRichStore;
struct ScheduledEvent;

// Thumbnail and media gallery images are fetched by URL, like profile art:
// the key is a hash of the (proxied) URL, so identical media share one decode.
struct MessageComponentMedia {
    MediaAssetKey key;
    Utf8View url;
    u8 format; // MediaAttachmentFormat
};

MediaAttachmentFormat message_component_media_format(Utf8View url) noexcept;
// The px2048 tier asks the decoder for a JPEG's own size (a track's 1280 x 720 picture on a stage). A profile image
// at the px512 tier draws small, and a JPEG past 512 px decodes at about 160 px.
MediaAssetKey message_component_media_key(u64 url_hash, MediaAttachmentFormat format,
                                          MediaResolutionTier resolution = MediaResolutionTier::px512) noexcept;
// False for components without fetchable HTTPS media (attachment:// names and
// unsupported formats).
bool message_component_media_resolve(const MessageRichStore& store, const MessageRichComponent& component,
                                     MessageComponentMedia* output) noexcept;

// CDN URL of an invite's server icon ("https://cdn.discordapp.com/icons/{guild}/{hash}.png?size=128");
// returns its length, 0 without an icon. Its media key is
// message_component_media_key(hash of the URL, png).
u32 message_invite_icon_url(const GuildInvite& invite, const Slice<char>& output) noexcept;
// A scheduled event's cover ("https://cdn.discordapp.com/guild-events/{id}/{hash}.png?size=256");
// 0 without a cover. Keyed like invite icons.
u32 message_event_cover_url(const ScheduledEvent& event, const Slice<char>& output) noexcept;

} // namespace maicord
