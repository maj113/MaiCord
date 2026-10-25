// The linked account module's API entries, called without an account owner:
// each refuses before creating an account, login, worker or transport.
#include "maicord/app/account_host.hpp"
#include "maicord/app/account_bulk_message_ack.hpp"
#include "maicord/app/account_channel_overview.hpp"
#include "maicord/app/account_download.hpp"
#include "maicord/app/account_forum_post.hpp"
#include "maicord/app/account_logout.hpp"
#include "maicord/app/account_notification_settings.hpp"
#include "maicord/app/account_poll_create.hpp"
#include "maicord/app/account_private_channel.hpp"
#include "maicord/app/account_relationships.hpp"
#include "maicord/app/account_thread_browser.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/protocol/guild_assets.hpp"
#include "maicord/protocol/self_status_settings.hpp"
#include "maicord/protocol/stage_channel.hpp"

namespace maicord {

void test_account_module_dormant(void (*check)(bool, const char*) noexcept) noexcept {
    AccountHost host{};
    check(account_host_load(&host).ok(), "dormant module: the linked module loads without profile or credential");
    if (!host.module)
        return;
    constexpr auto invalid = RestClientSubmitError::invalid_input;
    const AccountModuleApi& api = host.api;

    const AccountModuleApi complete = host.api;
    host.api.thread_browser = nullptr;
    check(account_host_start(&host, L"unused-profile", L"unused-session", 1, 1).error == AccountModuleError::invalid_input &&
              !host.instance,
          "dormant module: host refuses an incomplete API before creating an account owner");
    host.api = complete;
    host.api.version = 88;
    check(account_host_start(&host, L"unused-profile", L"unused-session", 1, 1).error == AccountModuleError::invalid_input &&
              !host.instance,
          "dormant module: host refuses a previous ABI version before creating an account owner");
    host.api = complete;

    const BulkMessageAckEntry entry{1, 2, 0};
    check(api.bulk_message_ack(nullptr, &entry, 1, 1).error == invalid && api.bulk_message_ack(nullptr, nullptr, 0, 1).error == invalid,
          "dormant module: bulk read acknowledgement");
    check(api.channel_overview(nullptr, ChannelOverviewOperation::fetch, 20, nullptr, 1).error == invalid,
          "dormant module: channel overview");
    ForumPostRequest forum{};
    forum.parent_id = 20;
    forum.guild_id = 10;
    forum.operation = ForumPostOperation::parent_metadata;
    check(api.forum_post(nullptr, &forum, 1).error == invalid && api.forum_post(nullptr, nullptr, 1).error == invalid,
          "dormant module: forum posts");
    check(api.guild_admin(nullptr, nullptr, 1).error == invalid && api.guild_moderation(nullptr, nullptr, 1).error == invalid,
          "dormant module: guild administration and moderation");
    GuildAssetRequest probe{};
    probe.operation = GuildAssetOperation::image_probe;
    probe.image = {reinterpret_cast<const u8*>("x"), 1};
    check(api.guild_assets(nullptr, nullptr, 1).error == invalid && api.guild_assets(nullptr, &probe, 0).error == invalid,
          "dormant module: guild assets");
    StageRequest stage{};
    stage.operation = StageOperation::lower_hand;
    stage.guild_id = 10;
    stage.channel_id = 20;
    check(api.stage(nullptr, nullptr, 1).error == invalid && api.stage(nullptr, &stage, 0).error == invalid,
          "dormant module: stage requests");
    GatewayClientVoiceVideoSnapshot video{};
    video.count = 7;
    check(!api.voice_video_snapshot(nullptr, &video) && video.count == 7 && !api.voice_video_wanted(nullptr, 42, false),
          "dormant module: voice video");
    AccountLogoutResponse logout{777};
    bool logout_refused = true;
    constexpr AccountLogoutOperation logout_operations[]{AccountLogoutOperation::begin, AccountLogoutOperation::cancel, AccountLogoutOperation::take};
    for (const auto operation : logout_operations)
        logout_refused = logout_refused && api.logout(nullptr, operation, 1, &logout).submit_error == invalid && logout.request_id == 777;
    check(logout_refused, "dormant module: logout leaves the response untouched");
    SelfCustomStatus status{};
    const UserGuildSettingsPatch patch{10, 0, 0, 0, user_guild_settings_suppress_roles};
    const MentionsQuery mentions{0, 0, kMentionsPageLimit, true, true};
    check(!api.custom_status_snapshot(nullptr, &status) && api.custom_status_submit(nullptr, 1, 2, &status, 3).error == invalid &&
              api.guild_settings(nullptr, &patch, 1).error == invalid && api.mentions(nullptr, &mentions, 1).error == invalid,
          "dormant module: custom status, guild notification settings and mentions");
    PollCreate poll{};
    poll.question = utf8_literal("Lunch?");
    poll.answers[0] = {utf8_literal("Pizza"), 0, {}};
    poll.answer_count = 1;
    poll.duration_hours = 24;
    check(api.poll_create(nullptr, 20, 77, &poll, 1).error == invalid && api.poll_create(nullptr, 20, 77, nullptr, 1).error == invalid,
          "dormant module: poll creation");
    PrivateChannelActionRequest leave{};
    leave.kind = PrivateChannelActionKind::leave;
    leave.channel_id = 10;
    check(api.private_channel(nullptr, &leave, 1).error == invalid && api.private_channel(nullptr, nullptr, 1).error == invalid,
          "dormant module: private channel actions");
    check(api.download(nullptr, AccountDownloadOperation::start, 1, nullptr).submit_error == HttpsFileDownloadError::invalid_input,
          "dormant module: attachment downloads");
    bool relationships_refused = true;
    for (u32 op = 0; op <= static_cast<u32>(RelationshipOperation::clear_hidden); ++op)
        relationships_refused = relationships_refused &&
            api.relationships(nullptr, static_cast<RelationshipOperation>(op), op > 1 && op < 8 ? 42 : 0,
                              op == 1 ? utf8_literal("friend.name") : Utf8View{}, 108).error == invalid;
    check(relationships_refused, "dormant module: every relationship operation");
    constexpr auto ack_body = utf8_literal("{\"token\":null}");
    check(api.message_ack(nullptr, 7, 99, {reinterpret_cast<const u8*>(ack_body.data), ack_body.bytes}, 201).error == invalid,
          "dormant module: message acknowledgement");
    ThreadBrowserRequest browse{20, 10, {}, 0, ThreadBrowserMode::archived, {}};
    check(api.thread_browser(nullptr, &browse, 1).error == invalid && api.thread_browser(nullptr, nullptr, 1).error == invalid,
          "dormant module: thread browser");
    ThreadCreationRequest thread{20, 10, 30, utf8_literal("Thread"), 1440, 11, ThreadCreationOperation::from_message, {}};
    check(api.thread_creation(nullptr, &thread, 1).error == invalid && api.thread_creation(nullptr, nullptr, 1).error == invalid,
          "dormant module: thread creation");
    check(!host.instance && !host.login_instance, "dormant module: no call created an account or login actor");
    check(account_host_release(&host).ok() && !host.module, "dormant module: release clears the host");
}

} // namespace maicord
