// Media-pump fixtures without the Go Live client: no stream previews are shown.
#include "../src/app/application_stream.hpp"
#include "../src/app/application_guild_card.hpp"
#include "../src/app/application_guild_banner.hpp"

namespace maicord::app_detail {
bool application_stream_preview_key_current(MediaAssetKey) noexcept {
    return false;
}
u32 application_stream_preview_media(StreamPreviewMedia*, u32) noexcept {
    return 0;
}
// Nor is the server tag popout.
u32 application_guild_card_media(GuildCardMedia*, u32) noexcept {
    return 0;
}
bool application_guild_card_key_current(MediaAssetKey) noexcept {
    return false;
}
void application_guild_card_media_failed(MediaAssetKey, const AccountMediaRequestResult&) noexcept {}
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
} // namespace maicord::app_detail
