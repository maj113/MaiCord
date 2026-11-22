// The people a guild view shows (src/app/account/author_hydration.cpp) are followed and asked for through the
// running account only: a logout or account switch keeps the account UI for a while with no instance, and the next
// frame must not reach a module entry then.
#include "../../support/account_module_fake.hpp"
#include "../../support/test_output.hpp"
#include "account/account_ui_runtime.hpp"
#include "account/application_account_module.hpp"
#include "account/friends.hpp"
#include "guild/hub.hpp"
#include "shell/state.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {
namespace app_detail {
// The Server Hub shows one member, whose records are not resident.
u32 application_guild_hub_people(u64, u64* ids, u32 count, u32 capacity) noexcept {
    if (count < capacity)
        ids[count++] = 77;
    return count;
}
bool application_friends_active() noexcept {
    return false;
}
} // namespace app_detail
namespace author_hydration_test {
using namespace app_detail;
u32 failures, calls, null_calls, empty_follows;
void check(bool value, const char* label) noexcept {
    test_check(&failures, value, label);
}
bool subscribe(void* instance, const GatewayGuildSubscription* subscription) noexcept {
    ++calls;
    null_calls += instance ? 0 : 1;
    empty_follows += subscription->member_count ? 0 : 1;
    return true;
}
bool members(void* instance, const GatewayGuildMemberRequest*) noexcept {
    ++calls;
    null_calls += instance ? 0 : 1;
    return true;
}
void run() noexcept {
    auto* ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    check(ui != nullptr, "author hydration: account owner allocates");
    if (!ui)
        return;
    GuildRecord guilds[1]{};
    ChannelRecord channels[1]{};
    u16 guild_order[1]{}, channel_order[1]{};
    u8 names[256]{};
    check(chat_state_store_initialize(&ui->chat_state, {guilds, 1}, {guild_order, 1}, {channels, 1},
                                      {channel_order, 1}, {names, sizeof(names)}) &&
              chat_state_store_upsert_guild(&ui->chat_state, {20, utf8_literal("Guild"), GuildMutationKind::create, 9})
                  .ok() &&
              chat_state_store_upsert_channel(&ui->chat_state, {200, 20, 0, utf8_literal("general"), 0, 0, 0}).ok(),
          "author hydration: one guild channel is selected");
    ui->chat_state.selected_channel_id = ui->active_channel_id = 200;
    g_account_fake.guild_subscribe = subscribe;
    g_account_fake.guild_members_request = members;
    g_app.account_ui = ui;
    g_app.account.instance = reinterpret_cast<void*>(1);
    synchronize_visible_author_hydration(1000);
    check(calls >= 2 && !null_calls && !empty_follows,
          "author hydration: the running account follows and asks for the member the view shows");

    // Logout: the stop moves the host away while the account UI and its scenes stay for a while.
    application_author_hydration_reset();
    g_app.account.instance = nullptr;
    calls = 0;
    ++ui->guild_member_store.user_pin_epoch;
    synchronize_visible_author_hydration(2000);
    synchronize_visible_author_hydration(70000);
    check(!calls && !null_calls, "author hydration: a frame without an account reaches no module entry");

    // The next account starts without the last one's followed people: nobody is unfollowed on its gateway.
    g_app.account.instance = reinterpret_cast<void*>(2);
    ++ui->guild_member_store.user_pin_epoch;
    synchronize_visible_author_hydration(3000);
    check(calls && !null_calls && !empty_follows,
          "author hydration: the next account only follows its own people");
    g_account_fake.guild_subscribe = nullptr;
    g_account_fake.guild_members_request = nullptr;
    g_app.account.instance = nullptr;
    g_app.account_ui = nullptr;
    VirtualFree(ui, 0, MEM_RELEASE);
}
} // namespace author_hydration_test
} // namespace maicord
extern "C" __declspec(noreturn) void WINAPI maicord_author_hydration_entry() noexcept {
    maicord::author_hydration_test::run();
    maicord::test_exit(maicord::author_hydration_test::failures, "author hydration");
}
