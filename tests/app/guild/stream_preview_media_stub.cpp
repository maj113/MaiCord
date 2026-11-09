// Media-pump fixtures without the Go Live client: no stream previews are shown.
#include "../../../src/app/voice/stream.hpp"
#include "../../../src/app/guild/card.hpp"
#include "../../../src/app/guild/banner.hpp"
#include "../../../src/app/media/expression_card.hpp"
#include "../../../src/app/chat/composer/commands.hpp"
#include "../../../src/app/guild/settings/settings.hpp"
#include "../../../src/app/chat/game_lookup.hpp"
#include "../../../src/app/chat/composer/game_search.hpp"

namespace maicord::app_detail {
bool application_stream_preview_key_current(const MediaAssetKey&) noexcept {
    return false;
}
u32 application_stream_preview_media(StreamPreviewMedia*, u32) noexcept {
    return 0;
}
// Nor is the server tag popout.
u32 application_guild_card_media(GuildCardMedia*, u32) noexcept {
    return 0;
}
u32 application_guild_card_emoji_media(MediaAssetKey*, u32) noexcept {
    return 0;
}
bool application_guild_card_key_current(MediaAssetKey) noexcept {
    return false;
}
void application_guild_card_media_failed(MediaAssetKey, const AccountMediaRequestResult&) noexcept {}
// Nor the emoji/sticker popout.
u32 application_expression_card_media(MediaAssetKey*, u32) noexcept {
    return 0;
}
const GuildRecord* application_expression_card_guild() noexcept {
    return nullptr;
}
// Nor a server banner over the channel list.
u32 application_guild_banner_media(GuildBannerMedia*, u32) noexcept {
    return 0;
}
bool application_guild_banner_key_current(MediaAssetKey) noexcept {
    return false;
}
void application_guild_banner_media_failed(MediaAssetKey) noexcept {}
bool application_guild_banner_take_retired(MediaAssetKey*) noexcept {
    return false;
}
// Nor the command picker's app icons.
u32 application_commands_icon_url(u64, Slice<char>) noexcept {
    return 0;
}
// Nor Server Settings' profile preview.
bool application_guild_settings_preview_media(u32, MediaAssetKey*, Utf8View*) noexcept {
    return false;
}
bool application_guild_settings_preview_key_current(const MediaAssetKey&) noexcept {
    return false;
}
// Nor game mentions: no game is ever cached or searched.
const GameStore* application_games() noexcept {
    return nullptr;
}
void game_lookup_want(u64) noexcept {}
u32 application_game_search_icon_url(u64, Slice<char>) noexcept {
    return 0;
}
} // namespace maicord::app_detail
