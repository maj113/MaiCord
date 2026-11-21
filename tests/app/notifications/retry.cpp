#include "../../../src/app/notifications/controls.cpp"

namespace maicord::app_detail {
namespace {
u32 retry_rollbacks;
const MessageNotificationStore* retry_store;
} // namespace
const MessageNotificationStore* application_notifications_store(const Application*) noexcept {
    return retry_store;
}
bool application_notifications_project(Application*, u64, u64, const MessageNotificationSetting&) noexcept {
    ++retry_rollbacks;
    return true;
}

// Browse Channels follows: the first one turns opt-in on with the channel at once. With opt-in on, follows of the
// same server gather in one patch that waits a second after the last change. Unfollowing clears the favourite flag.
void test_channel_follows(void (*check)(bool, const char*) noexcept) noexcept {
    MessageNotificationGuildSetting guilds[1]{};
    MessageNotificationOverride overrides[2]{};
    MessageNotificationThreadSetting threads[1]{};
    MessageNotificationStore store{};
    message_notifications_initialize(&store, {guilds, 1}, {overrides, 2}, {threads, 1});
    MessageNotificationGuildSetting guild{10, {0, 1u << 3, notification_flags, MessageNotificationLevel::all}};
    MessageNotificationOverride favourite{10, 21, {0, kUserChannelOptInEnabled | kUserChannelFavorited,
                                                   notification_flags, MessageNotificationLevel::inherit}};
    message_notifications_apply_settings(&store, {&guild, 1}, {&favourite, 1}, true, 0);
    retry_store = &store;
    owner = {};
    owner.account_scope = g_app.plugin_account_scope_serial;
    const bool first = application_notification_controls_follow_channel(10, 20, true);
    const auto& opened = owner.queue[0];
    check(first && owner.count == 1 && opened.patch.fields == user_guild_settings_flags &&
              opened.patch.flags == ((1u << 3) | kUserGuildOptInChannelsOn) && opened.patch.batch_count == 1 &&
              opened.patch.batch_channels[0] == 20 && opened.patch.batch_flags[0] == kUserChannelOptInEnabled &&
              !opened.not_before,
          "notification first follow turns channel opt-in on in the same, immediate patch");

    guild.setting.flags |= kUserGuildOptInChannelsOn;
    message_notifications_apply_settings(&store, {&guild, 1}, {&favourite, 1}, true, 0);
    owner.queue[0].submitted = true; // the first patch is in flight
    const u64 now = clock_monotonic_milliseconds();
    const bool later = application_notification_controls_follow_channel(10, 22, true) &&
                       application_notification_controls_follow_channel(10, 21, false);
    const auto& batch = owner.queue[1];
    check(later && owner.count == 2 && !batch.patch.fields && batch.patch.batch_count == 2 &&
              batch.patch.batch_channels[0] == 22 && batch.patch.batch_flags[0] == kUserChannelOptInEnabled &&
              batch.patch.batch_channels[1] == 21 && batch.patch.batch_flags[1] == 0 &&
              batch.batch_prior[1].flags == (kUserChannelOptInEnabled | kUserChannelFavorited) &&
              batch.not_before >= now + 1'000,
          "notification later follows gather in one patch that waits a second after the last");
    retry_store = nullptr;
    owner = {};
}

void test_notification_retry(void (*check)(bool, const char*) noexcept) noexcept {
    retry_rollbacks = 0;
    for (u32 kind = 0; kind < 5; ++kind) {
        owner = {};
        owner.request_id = 73;
        owner.count = 1;
        owner.queue[0].submitted = true;
        owner.queue[0].attempts = kind == 4 ? 3 : 1;
        RestClientResponse response{};
        response.request_id = 73;
        response.status_code = 429;
        response.rate_limited = kind != 1;
        response.retry_at_ms = kind == 2 ? 0 : 3456789;
        response.rejection.captcha = kind == 0;
        check(application_notification_controls_response(response, {}),
              "notification exact completion consumed");
        if (kind == 3)
            check(owner.count == 1 && !owner.request_id && owner.retry_at == 3456789 &&
                      !owner.queue[0].submitted && owner.queue[0].attempts == 1,
                  "notification genuine 429 retains exact deadline and attempt count");
        else
            check(!owner.count && !owner.request_id && !owner.retry_at,
                  "notification challenge, unknown cooldown or exhausted retry leaves no queued write");
    }
    check(retry_rollbacks == 4, "notification terminal failures restore the prior setting exactly once");
    owner = {};
    test_channel_follows(check);
}
} // namespace maicord::app_detail
