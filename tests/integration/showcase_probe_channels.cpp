// The showcase's Channels & Roles page on its Browse Channels tab: channels_roles.cpp's own page builder over the
// probe's world (showcase_probe_hub.cpp sets the account's stores). What the community module and the rest of the
// application would answer is defined here.
#include "../../src/app/guild/channels_roles.cpp"
#include "showcase_probe_internal.hpp"

namespace maicord::app_detail {
namespace community_detail {
Background* background;
ChatStateStore& state() noexcept {
    return g_app.account_ui->chat_state;
}
Slice<u32> json_scratch() noexcept {
    static u32 scratch[1024];
    return {scratch, 1024};
}
bool ensure_background() noexcept {
    return false;
}
bool enqueue(GuildCommunityOperation, u64, u64) noexcept {
    return false;
}
const GuildRecord* guild(u64 id) noexcept {
    return chat_state_store_find_guild(state(), id);
}
} // namespace community_detail

bool synchronize_navigation_view() noexcept {
    return true;
}
bool application_voice_join(u64, u64) noexcept {
    return false;
}
bool select_content_guild(u64) noexcept {
    return false;
}
bool application_notification_controls_follow_channel(u64, u64, bool) noexcept {
    return false;
}
bool application_guild_settings_create_channel(u64, u64, u8) noexcept {
    return false;
}
} // namespace maicord::app_detail

namespace maicord::showcase_probe_detail {
using namespace app_detail;

// As application_guild_channels_roles_open shows a community server with onboarding questions, then a click on the
// Browse Channels tab. The introduction stays open, as on a first visit.
const ContentPageOwner* showcase_channels_page(const ShowcaseWorld& world) noexcept {
    if (!showcase_account_ui(world) || (!page && !(page = owner_allocate<Page>())))
        return nullptr;
    auto& p = *page;
    p.guild_id = world.guild;
    p.account_scope = g_app.plugin_account_scope_serial;
    p.profile_id = g_app.account.status.profile_id;
    p.browse = true;
    p.browse_only = false;
    p.phase = Phase::ready;
    p.shown = true;
    return &kRolesPage;
}
} // namespace maicord::showcase_probe_detail
