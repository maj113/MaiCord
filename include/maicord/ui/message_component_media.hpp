#pragma once

#include "maicord/media/asset.hpp"
#include "maicord/model/message_rich_store.hpp"
#include "maicord/protocol/guild_invite.hpp"
#include "maicord/protocol/guild_community.hpp"

namespace maicord {

// Thumbnail and media gallery images are fetched by URL, like profile art:
// the key is a hash of the (proxied) URL, so identical media share one decode.
struct MessageComponentMedia {
    MediaAssetKey key;
    Utf8View url;
    u8 format; // MediaAttachmentFormat
};

MediaAttachmentFormat message_component_media_format(Utf8View url) noexcept;
MediaAssetKey message_component_media_key(u64 url_hash, MediaAttachmentFormat format) noexcept;
// False for components without fetchable HTTPS media (attachment:// names and
// unsupported formats).
bool message_component_media_resolve(const MessageRichStore& store, const MessageRichComponent& component,
                                     MessageComponentMedia* output) noexcept;

// CDN URL of an invite's server icon ("https://cdn.discordapp.com/icons/{guild}/{hash}.png?size=128");
// returns its length, 0 without an icon. Its media key is
// message_component_media_key(hash of the URL, png).
u32 message_invite_icon_url(const GuildInvite& invite, Slice<char> output) noexcept;
// A scheduled event's cover ("https://cdn.discordapp.com/guild-events/{id}/{hash}.png?size=256");
// 0 without a cover. Keyed like invite icons.
u32 message_event_cover_url(const ScheduledEvent& event, Slice<char> output) noexcept;

} // namespace maicord
