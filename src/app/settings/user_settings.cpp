#include "user_settings.hpp"
#include "../guild/folders.hpp"
#include "../account/account_access.hpp"
#include "../shell/owner.hpp"
#include "account_broker.hpp"
#include "../shell/state.hpp"
#include "../chat/composer/text_form.hpp"
#include "../chat/application_link_trust.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/text/timestamp_format.hpp"
#include "maicord/ui/chat/messages/scene.hpp"
#include "maicord/ui/layout/density.hpp"

#include "maicord/protocol/user/account_settings.hpp"
#include "../account/account_ui_runtime.hpp"
#include "maicord/app/gateway/client_event.hpp"
#include "maicord/protocol/user/settings_proto.hpp"

namespace maicord::app_detail {
namespace {
using K = UserSettingsKind;
constexpr UserSettingsField kCompact{6, 17, 0, K::boolean}, kNsfwCommands{6, 23, 0, K::boolean},
    kSpam{6, 27, 0, K::enumeration}, kExplicitGuilds{6, 29, 1, K::enumeration},
    kExplicitFriends{6, 29, 2, K::enumeration}, kExplicitOthers{6, 29, 3, K::enumeration},
    kProfileUpdates{7, 24, 0, K::boolean}, kDmRestricted{8, 4, 0, K::plain_bool},
    kDmRestrictedV2{8, 27, 0, K::boolean}, kMessageRequests{8, 17, 0, K::boolean}, // default_message_request_restricted
    kFriendFlags{8, 11, 0, K::uint32}, kVisibility{8, 32, 0, K::enumeration},
    kHourCycle{13, 9, 0, K::enumeration}, kSpoilers{6, 4, 0, K::string}, kInlineAttachment{6, 9, 0, K::boolean},
    kInlineEmbed{6, 10, 0, K::boolean}, kGifAutoPlay{6, 11, 0, K::boolean}, kRenderEmbeds{6, 12, 0, K::boolean},
    kRenderReactions{6, 13, 0, K::boolean}, kAnimateEmoji{6, 14, 0, K::boolean},
    kAnimateStickers{6, 15, 0, K::uint32}, kConvertEmoticons{6, 21, 0, K::boolean};
constexpr UserSettingsField kLegacyExplicit{6, 19, 0, K::uint32};
// Messaging Permissions, Data & Privacy and Activity Privacy rows.
// hide_friend_request_notes, drops_opted_out and quests_3p_data_opted_out
// are shown inverted, as in the official client.
constexpr UserSettingsField kNsfwGuilds{6, 20, 0, K::boolean}, kHideRequestNotes{8, 33, 0, K::boolean},
    kDropsOptedOut{8, 18, 0, K::boolean}, kQuests3pOptedOut{8, 30, 0, K::boolean},
    kNotifyOnline{7, 25, 0, K::boolean}, kNotifyGoLive{7, 26, 0, K::boolean},
    kActivityServers{8, 29, 0, K::enumeration}, kJoinFriends{8, 1, 0, K::boolean}, kJoinVoice{8, 2, 0, K::boolean};
// Language, the Notifications page's "Notify me when..." rows, Mobile Notification Delay (afk_timeout, seconds),
// Chat Box's game suggestions and Developer Mode.
constexpr UserSettingsField kLocale{12, 1, 0, K::string}, kStreamNotifications{5, 7, 0, K::boolean},
    kAfkTimeout{5, 6, 0, K::uint32}, kFriendOnline{7, 12, 0, K::boolean}, kProfileUpdateNotices{7, 16, 0, K::boolean},
    kAnniversaries{7, 14, 0, K::boolean}, kReactionNotifications{7, 7, 0, K::enumeration},
    kGameMentions{6, 39, 0, K::boolean}, kDeveloperMode{13, 2, 0, K::plain_bool};
// Show image descriptions, the /tts command, sticker suggestions, compact avatars and UI Density.
constexpr UserSettingsField kImageDescriptions{6, 7, 0, K::boolean}, kTtsCommand{6, 16, 0, K::boolean},
    kStickerAutocomplete{6, 28, 0, K::boolean}, kCompactAvatars{6, 41, 0, K::boolean},
    kUiDensity{13, 12, 0, K::enumeration};
// UI Density's choices (Compact, Default, Spacious) as UIDensity values; unset reads as Default.
constexpr u8 kUiDensityValues[]{1, 4, 2};
// Discord's locales, in the order of catalog_data.inc's `languages`.
constexpr Utf8View kLocales[]{
    utf8_literal("id"),    utf8_literal("da"),    utf8_literal("de"),    utf8_literal("en-GB"), utf8_literal("en-US"),
    utf8_literal("es-ES"), utf8_literal("es-419"), utf8_literal("fr"),   utf8_literal("hr"),    utf8_literal("it"),
    utf8_literal("lt"),    utf8_literal("hu"),    utf8_literal("nl"),    utf8_literal("no"),    utf8_literal("pl"),
    utf8_literal("pt-BR"), utf8_literal("ro"),    utf8_literal("fi"),    utf8_literal("sv-SE"), utf8_literal("vi"),
    utf8_literal("tr"),    utf8_literal("cs"),    utf8_literal("el"),    utf8_literal("bg"),    utf8_literal("ru"),
    utf8_literal("uk"),    utf8_literal("hi"),    utf8_literal("th"),    utf8_literal("zh-CN"), utf8_literal("zh-TW"),
    utf8_literal("ja"),    utf8_literal("ko"),    utf8_literal("ar")};
constexpr u32 kLocaleUnitedStates = 4;
constexpr Utf8View kExplicitChoices[]{utf8_literal("Show"), utf8_literal("Blur"), utf8_literal("Block")};
// privacy.restricted_guild_ids, message_request_restricted_guild_ids,
// activity_restricted_guild_ids, activity_joining_restricted_guild_ids.
// Membership is absolute, not an exception to a default: the defaults only
// seed servers joined later, as in the official client.
constexpr UserSettingsField kGuildLists[]{{8, 3, 0, K::id_member},
                                          {8, 16, 0, K::id_member},
                                          {8, 13, 0, K::id_member},
                                          {8, 15, 0, K::id_member}};
// render_spoilers strings by MessageSpoilerReveal; unset or unknown is ON_CLICK.
constexpr Utf8View kSpoilerValues[]{utf8_literal("ON_CLICK"), utf8_literal("IF_MODERATOR"),
                                    utf8_literal("ALWAYS")};
// The select lists On click, Always, On servers I moderate.
constexpr u8 kSpoilerChoices[]{0, 2, 1};
// friend_source_flags: MUTUAL_FRIENDS 2, MUTUAL_GUILDS 4, NO_RELATION 8. Everyone
// means all three (14); turning it off keeps both narrower sources (6).
constexpr u32 kFriendsOfFriends = 2, kServerMembers = 4, kEveryone = 8, kFriendDefault = 14;
constexpr u64 kInvalidUserSettingsData = 50105;
// Radio/choice index <-> proto enum.
constexpr u8 kSpamValues[]{3, 2, 1};          // filter all, non-friends, off
constexpr u8 kVisibilityValues[]{3, 2, 1};    // all servers, small servers, friends only
// default_guilds_activity_restricted_v2: OFF 1 (share everywhere), ON_FOR_LARGE_GUILDS 2, ON 3.
constexpr u8 kActivityServerValues[]{1, 2, 3};

struct Bank {
    UserSettingsStore store;
    u8 partial[kUserSettingsPartialMaximumBytes];
    u8 response[kUserSettingsProtoArenaBytes];
};
struct Owner {
    Bank* bank;
    u64 profile_id, self_user_id, request_id, retry_at_ms, applied_revision;
    Utf8View status;
    u8 attempts;
    bool fetching, failed;
    u64 explicit_revision;
    AccountContentPolicy explicit_policy;
    MessageExplicitFilter explicit_filters[3];
    bool explicit_cached;
};
Owner g_owner{};
// The message scene's display: the account's compact flag and clock shape with
// the local group spacing, built-in extras and Accessibility's role colours and links.
MessageSceneDisplay scene_display(bool compact, bool compact_avatars) noexcept {
    const auto& p = g_app.preferences;
    return {p.message_group_spacing, compact, static_cast<MessageSceneClock>(timestamp_clock_shape()),
            p.reply_timestamps, p.role_colors_everywhere, p.role_style,
            static_cast<u8>(p.desaturate_user_colors ? p.desaturation : 0), p.underline_links, compact_avatars};
}

UserSettingsStore* store() noexcept {
    return g_owner.bank ? &g_owner.bank->store : nullptr;
}
bool loaded() noexcept {
    return store() && store()->loaded;
}
AccountContentPolicy content_policy() noexcept {
    return g_app.account_ui ? g_app.account_ui->chat_state.content_policy : AccountContentPolicy{};
}
bool content_confirmation_valid(TextFormOwnerKey key) noexcept {
    return account_online() && key.scope == reinterpret_cast<u64>(g_app.account.instance) &&
           key.collection == g_app.account.status.profile_id &&
           key.item == g_app.account_ui->chat_state.self_user_id &&
           account_content_confirmation_required(content_policy());
}
bool content_confirmation_open(TextFormOwnerKey key, Utf8View) noexcept {
    if (!content_confirmation_valid(key))
        return false;
    application_text_form_close();
    return application_link_open(utf8_literal("https://discord.com/settings/account-status"), true);
}
void release_bank() noexcept {
    owner_release(g_owner.bank);
}
__forceinline bool ensure_bank() noexcept {
    if (g_owner.bank)
        return true;
    if (!owner_ensure(g_owner.bank))
        return false;
    user_settings_store_reset(&g_owner.bank->store);
    return true;
}
// Discards everything owned by a previous profile or user.
void scope() noexcept {
    const u64 profile = g_app.account.status.profile_id;
    const u64 self = g_app.account_ui ? g_app.account_ui->chat_state.self_user_id : 0;
    if (g_owner.profile_id == profile && g_owner.self_user_id == self)
        return;
    application_text_form_cancel_owner(content_confirmation_valid);
    application_cancel_request(&AccountModuleApi::account_settings, g_owner.request_id);
    release_bank();
    const u64 applied = g_owner.applied_revision;
    reset_value(&g_owner);
    g_owner.applied_revision = applied + 1;
    g_owner.profile_id = profile;
    g_owner.self_user_id = self;
}
u64 read_number(UserSettingsField field, u64 fallback) noexcept {
    UserSettingsValue value;
    return store() && user_settings_store_read(*store(), field, &value) ? value.number : fallback;
}
struct ExplicitOptions {
    u32 first, count;
    bool enabled;
};
ExplicitOptions explicit_options(SettingBinding binding) noexcept {
    const auto policy = content_policy();
    const bool restricted = policy.eligibility == AccountContentEligibility::restricted;
    const bool guild = binding == SettingBinding::explicit_guilds;
    return {restricted ? 1u : 0u, guild ? (restricted ? 1u : 2u) : (restricted ? 2u : 3u),
            !(policy.flags & account_content_parent_managed) && !(guild && restricted)};
}
__forceinline MessageMediaContext explicit_context(SettingBinding binding) noexcept {
    return binding == SettingBinding::explicit_friends ? MessageMediaContext::friend_dm
           : binding == SettingBinding::explicit_others ? MessageMediaContext::other_dm
                                                        : MessageMediaContext::guild;
}
__forceinline UserSettingsField explicit_field(SettingBinding binding) noexcept {
    return binding == SettingBinding::explicit_friends ? kExplicitFriends
           : binding == SettingBinding::explicit_others ? kExplicitOthers : kExplicitGuilds;
}
bool present(UserSettingsField field) noexcept {
    UserSettingsValue value;
    return store() && user_settings_store_read(*store(), field, &value);
}
bool write(UserSettingsField field, const UserSettingsValue& value) noexcept {
    auto* s = store();
    if (!s || !user_settings_store_set(s, field, value, clock_monotonic_milliseconds()))
        return false;
    g_owner.failed = false;
    reset_value(&g_owner.status);
    application_user_settings_apply_display();
    application_request_frame();
    return true;
}
bool write_number(UserSettingsField field, u64 number) noexcept {
    return write(field, user_settings_number(number));
}
u32 index_of(const u8* values, u32 count, u64 value, u32 fallback) noexcept {
    for (u32 i = 0; i < count; ++i)
        if (values[i] == value)
            return i;
    return fallback;
}
bool dm_restricted() noexcept {
    return present(kDmRestrictedV2) ? read_number(kDmRestrictedV2, 0) != 0 : read_number(kDmRestricted, 0) != 0;
}
bool restricted_account() noexcept {
    return content_policy().eligibility == AccountContentEligibility::restricted;
}
// dm_spam_filter_v2 when unset: restricted accounts filter all; others follow
// the legacy explicit_content_filter (0 off, 1 non-friends, 2 all; unset 1).
__forceinline u64 spam_filter() noexcept {
    const u64 value = read_number(kSpam, 0);
    if (value >= 1 && value <= 3)
        return value;
    if (restricted_account())
        return 3;
    const u64 legacy = read_number(kLegacyExplicit, 1);
    return legacy <= 2 ? legacy + 1 : 2;
}
// default_guilds_activity_restricted_v2 when unset: shared everywhere, or in
// small servers only for restricted accounts.
u64 activity_servers() noexcept {
    const u64 value = read_number(kActivityServers, 0);
    return value >= 1 && value <= 3 ? value : restricted_account() ? 2 : 1;
}
u32 spoiler_choice() noexcept {
    UserSettingsValue value;
    if (store() && user_settings_store_read(*store(), kSpoilers, &value))
        for (u32 i = 0; i < 3; ++i)
            if (utf8_equal({value.text, value.text_bytes}, kSpoilerValues[i]))
                return i;
    return 0;
}
// Synced switches with their value when unset; inverted ones show !value.
struct SyncedToggle {
    UserSettingsField field;
    bool fallback, inverted;
};
bool synced_toggle(SettingBinding binding, SyncedToggle* toggle) noexcept {
    switch (binding) {
    case SettingBinding::nsfw_guilds: *toggle = {kNsfwGuilds, false, false}; return true;
    case SettingBinding::friend_request_notes: *toggle = {kHideRequestNotes, restricted_account(), true}; return true;
    case SettingBinding::sponsored_activity: *toggle = {kDropsOptedOut, false, true}; return true;
    case SettingBinding::sponsored_third_party: *toggle = {kQuests3pOptedOut, false, true}; return true;
    case SettingBinding::share_online: *toggle = {kNotifyOnline, true, false}; return true;
    case SettingBinding::share_streaming: *toggle = {kNotifyGoLive, true, false}; return true;
    case SettingBinding::join_friends: *toggle = {kJoinFriends, true, false}; return true;
    case SettingBinding::join_voice: *toggle = {kJoinVoice, true, false}; return true;
    case SettingBinding::notify_go_live: *toggle = {kStreamNotifications, true, false}; return true;
    case SettingBinding::notify_friend_online: *toggle = {kFriendOnline, true, false}; return true;
    case SettingBinding::notify_profile_updates: *toggle = {kProfileUpdateNotices, true, false}; return true;
    case SettingBinding::notify_anniversaries: *toggle = {kAnniversaries, true, false}; return true;
    case SettingBinding::game_mentions_autocomplete: *toggle = {kGameMentions, true, false}; return true;
    case SettingBinding::image_descriptions: *toggle = {kImageDescriptions, false, false}; return true;
    case SettingBinding::sticker_autocomplete: *toggle = {kStickerAutocomplete, false, false}; return true;
    case SettingBinding::tts_command: *toggle = {kTtsCommand, true, false}; return true;
    default: return false;
    }
}
// Unset fields keep the service defaults: everything shown and animated.
__forceinline MessageRichDisplay chat_display() noexcept {
    MessageRichDisplay display{};
    if (!loaded())
        return display;
    display.attachment_media = read_number(kInlineAttachment, 1) != 0;
    display.embed_media = read_number(kInlineEmbed, 1) != 0;
    display.embeds = read_number(kRenderEmbeds, 1) != 0;
    display.reactions = read_number(kRenderReactions, 1) != 0;
    display.gifs = read_number(kGifAutoPlay, 1) ? MessageMediaAnimation::always : MessageMediaAnimation::interaction;
    display.emoji = read_number(kAnimateEmoji, 1) ? MessageMediaAnimation::always : MessageMediaAnimation::interaction;
    const u64 stickers = read_number(kAnimateStickers, 0);
    display.stickers = stickers <= 2 ? static_cast<MessageMediaAnimation>(stickers) : MessageMediaAnimation::always;
    display.spoilers = static_cast<MessageSpoilerReveal>(spoiler_choice());
    display.convert_emoticons = read_number(kConvertEmoticons, 1) != 0;
    display.image_descriptions = read_number(kImageDescriptions, 0) != 0;
    return display;
}
bool chat_toggle(SettingBinding binding, UserSettingsField* field) noexcept {
    switch (binding) {
    case SettingBinding::inline_attachment_media: *field = kInlineAttachment; return true;
    case SettingBinding::inline_embed_media: *field = kInlineEmbed; return true;
    case SettingBinding::render_embeds: *field = kRenderEmbeds; return true;
    case SettingBinding::render_reactions: *field = kRenderReactions; return true;
    case SettingBinding::gif_autoplay: *field = kGifAutoPlay; return true;
    case SettingBinding::animate_emoji: *field = kAnimateEmoji; return true;
    case SettingBinding::convert_emoticons: *field = kConvertEmoticons; return true;
    default: return false;
    }
}
void mark_failed(Utf8View status) noexcept {
    g_owner.failed = true;
    g_owner.status = status;
    log_write(LogLevel::warning, "user_settings", "save_failed");
}
} // namespace

void application_user_settings_gateway(const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    if (event.kind != GatewayClientEventKind::dispatch || event.dispatch_kind != GatewayClientDispatchKind::user_settings)
        return;
    scope();
    const bool partial = user_settings_proto_partial(payload);
    if (partial && !loaded())
        return; // a partial update cannot seed an empty store
    if (!ensure_bank() || !user_settings_store_apply(store(), payload, partial)) {
        log_write(LogLevel::warning, "user_settings", "gateway_rejected");
        return;
    }
    application_user_settings_apply_display();
    application_request_frame();
}

void application_user_settings_synchronize() noexcept {
    scope();
    if (auto* s = store())
        s->live = account_online();
    const u64 revision = store() ? store()->revision : 0;
    if (revision != g_owner.applied_revision)
        application_user_settings_apply_display();
}

bool application_user_settings_pending(u64 now_ms) noexcept {
    auto* s = store();
    if (!s || g_owner.request_id || !account_online() || !g_app.account.api.account_settings || g_owner.retry_at_ms > now_ms)
        return false;
    s->live = true;
    return s->refetch || user_settings_store_due(*s, now_ms);
}

bool application_user_settings_submit(u64 request_id, u64 now_ms) noexcept {
    if (!request_id || !application_user_settings_pending(now_ms))
        return false;
    auto* s = store();
    AccountSettingsRequest request{};
    if (s->refetch) {
        request.operation = AccountSettingsOperation::settings_fetch;
        const auto admitted = g_app.account.api.account_settings(g_app.account.instance, &request, request_id);
        if (!admitted.ok()) {
            g_owner.retry_at_ms = now_ms + 1000;
            return false;
        }
        s->refetch = false;
        g_owner.fetching = true;
        g_owner.request_id = request_id;
        return true;
    }
    u32 required = 0;
    bool pinned = false;
    const u32 bytes =
        user_settings_store_begin(s, {g_owner.bank->partial, kUserSettingsPartialMaximumBytes}, &required, &pinned);
    if (!bytes) {
        // Unserializable (oversized) groups: drop the edits rather than loop.
        clear_bytes(s->dirty, sizeof(s->dirty));
        s->dirty_count = 0;
        s->due_ms = 0;
        mark_failed(utf8_literal("Couldn't save your settings."));
        application_user_settings_apply_display();
        return false;
    }
    request.operation = AccountSettingsOperation::settings_patch;
    request.payload = {g_owner.bank->partial, bytes};
    request.required_data_version = required;
    request.required_data_version_present = pinned;
    const auto admitted = g_app.account.api.account_settings(g_app.account.instance, &request, request_id);
    secure_clear_bytes(g_owner.bank->partial, bytes);
    if (!admitted.ok()) {
        // Busy lanes retry the same edits shortly; nothing is rolled back.
        user_settings_store_complete(s, UserSettingsOutcome::rate_limited, {}, now_ms + 500);
        return false;
    }
    g_owner.fetching = false;
    g_owner.request_id = request_id;
    return true;
}

bool application_user_settings_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (application_guild_folders_owns_response(response.request_id))
        return application_guild_folders_response(response, body);
    if (!g_owner.request_id || response.request_id != g_owner.request_id)
        return false;
    g_owner.request_id = 0;
    auto* s = store();
    if (!s)
        return true;
    const u64 now_ms = clock_monotonic_milliseconds();
    const bool ok = response.succeeded();
    UserSettingsResponse decoded{};
    if (ok)
        decoded = user_settings_proto_decode_response(body, kUserSettingsStoredGroups,
                                                      {g_owner.bank->response, kUserSettingsProtoArenaBytes});
    const ConstSlice<u8> extracted{g_owner.bank->response, decoded.extracted.ok() ? decoded.extracted.bytes : 0};
    if (g_owner.fetching) {
        g_owner.fetching = false;
        if (decoded.extracted.ok() && decoded.settings_present && user_settings_store_apply(s, extracted, false)) {
            g_owner.attempts = 0;
        } else {
            s->refetch = ++g_owner.attempts < 3 &&
                (response.status_code != 429 || (response.transport_ok() && response.rate_limited &&
                                                 response.retry_at_ms != 0));
            g_owner.retry_at_ms = response.status_code == 429 ? response.retry_at_ms : now_ms + 5000;
        }
        application_user_settings_apply_display();
        application_request_frame();
        return true;
    }
    UserSettingsOutcome outcome = UserSettingsOutcome::failed;
    if (ok && decoded.extracted.ok() && decoded.settings_present)
        outcome = decoded.out_of_date ? UserSettingsOutcome::out_of_date : UserSettingsOutcome::saved;
    else if (response.transport_ok() && response.rate_limited && response.status_code == 429 &&
             response.retry_at_ms != 0 && ++g_owner.attempts < 4)
        outcome = UserSettingsOutcome::rate_limited;
    else if (response.transport_ok() && response.status_code == 400) {
        u32 structurals[64];
        if (account_settings_error_code(body, {structurals, 64}) == kInvalidUserSettingsData)
            outcome = UserSettingsOutcome::invalid_data;
    }
    const auto completion = user_settings_store_complete(
        s, outcome, outcome == UserSettingsOutcome::saved || outcome == UserSettingsOutcome::out_of_date ? extracted
                                                                                                        : ConstSlice<u8>{},
        response.retry_at_ms);
    if (outcome != UserSettingsOutcome::rate_limited)
        g_owner.attempts = 0;
    if (outcome == UserSettingsOutcome::out_of_date)
        mark_failed(utf8_literal("Your settings changed on another device. Showing the latest."));
    else if (completion.rolled_back)
        mark_failed(utf8_literal("Couldn't save your settings. Your change was undone."));
    else if (outcome == UserSettingsOutcome::saved) {
        g_owner.failed = false;
        reset_value(&g_owner.status);
    }
    application_user_settings_apply_display();
    application_request_frame();
    return true;
}

u64 application_user_settings_next_wake() noexcept {
    const auto* s = store();
    if (!s || g_owner.request_id)
        return 0;
    if (s->refetch)
        return g_owner.retry_at_ms ? g_owner.retry_at_ms : 1;
    return s->dirty_count && !s->inflight_active ? maximum(s->due_ms, g_owner.retry_at_ms) : 0;
}

void application_user_settings_release() noexcept {
    application_text_form_cancel_owner(content_confirmation_valid);
    release_bank();
    reset_value(&g_owner);
    timestamp_set_hour_cycle(HourCycle::automatic);
    g_ui_density = UiDensity::standard;
    message_scene_set_display(&g_app.message_scene, scene_display(false, false));
    if (message_rich_set_display({}))
        message_scene_invalidate_geometry(&g_app.message_scene);
}

void application_user_settings_apply_display() noexcept {
    const bool compact = loaded() && read_number(kCompact, 0) != 0;
    const bool compact_avatars = loaded() && read_number(kCompactAvatars, 0) != 0;
    const u64 cycle = loaded() ? read_number(kHourCycle, 0) : 0;
    // UIDensity COMPACT 1, COZY 2 (Spacious); unset, DEFAULT and RESPONSIVE lay out as Default.
    const u64 density_value = loaded() ? read_number(kUiDensity, 4) : 4;
    const UiDensity density = density_value == 1   ? UiDensity::compact
                              : density_value == 2 ? UiDensity::spacious
                                                   : UiDensity::standard;
    const bool density_changed = density != g_ui_density;
    g_ui_density = density;
    g_owner.applied_revision = store() ? store()->revision : g_owner.applied_revision;
    // Developer Mode is the account's (appearance.developer_mode); the local copy serves before sign-in.
    if (loaded())
        g_app.preferences.developer_mode = read_number(kDeveloperMode, 0) != 0;
    // The cycle first: it decides the timestamp column's width.
    const bool time = timestamp_set_hour_cycle(cycle <= 2 ? static_cast<HourCycle>(cycle)
                                                                  : HourCycle::automatic);
    const bool display = message_scene_set_display(&g_app.message_scene, scene_display(compact, compact_avatars));
    const MessageRichDisplay chat = chat_display();
    const MessageRichDisplay previous = message_rich_display();
    const bool chat_changed = !message_rich_display_equal(chat, previous);
    // Spoiler reveal restyles text; geometry changes remeasure every row.
    const bool chat_geometry = message_rich_set_display(chat) || chat.spoilers != previous.spoilers;
    if (chat_geometry)
        message_scene_invalidate_geometry(&g_app.message_scene);
    if (chat_changed)
        g_app.message_media.scan_pending = true;
    if ((display || time || chat_changed || density_changed) && g_app.renderer_ready)
        application_refresh_theme_consumers(false, display || chat_geometry || density_changed);
}

bool application_user_settings_set_developer_mode(bool on) noexcept {
    return loaded() && write_number(kDeveloperMode, on);
}

bool application_user_settings_game_mentions_autocomplete() noexcept {
    return !loaded() || read_number(kGameMentions, 1) != 0;
}
bool application_user_settings_sticker_autocomplete() noexcept {
    return loaded() && read_number(kStickerAutocomplete, 0) != 0;
}
bool application_user_settings_tts_command() noexcept {
    return !loaded() || read_number(kTtsCommand, 1) != 0;
}

bool application_user_settings_reveal_spoilers(u64 channel_id) noexcept {
    switch (message_rich_display().spoilers) {
    case MessageSpoilerReveal::always:
        return true;
    case MessageSpoilerReveal::moderator: {
        const ChannelViewRow* channel =
            g_app.account_ui && channel_id
                ? navigation_view_find_channel(g_app.account_ui->navigation_view, channel_id)
                : nullptr;
        return channel && (channel->actions & channel_action_manage_messages) != 0;
    }
    default:
        return false;
    }
}

MessageExplicitFilter application_user_settings_explicit_filter(MessageMediaContext context) noexcept {
    scope();
    const u32 index = static_cast<u32>(context);
    if (index >= 3)
        return MessageExplicitFilter::block;
    const u64 revision = store() ? store()->revision : 0;
    const auto policy = content_policy();
    if (!g_owner.explicit_cached || g_owner.explicit_revision != revision ||
        !bytes_equal(&g_owner.explicit_policy, &policy, sizeof(policy))) {
        const u64 legacy = read_number(kLegacyExplicit, 1);
        g_owner.explicit_filters[0] = message_explicit_filter_resolve(
            MessageMediaContext::guild, read_number(kExplicitGuilds, 0), legacy, policy);
        g_owner.explicit_filters[1] = message_explicit_filter_resolve(
            MessageMediaContext::friend_dm, read_number(kExplicitFriends, 0), legacy, policy);
        g_owner.explicit_filters[2] = message_explicit_filter_resolve(
            MessageMediaContext::other_dm, read_number(kExplicitOthers, 0), legacy, policy);
        g_owner.explicit_revision = revision;
        g_owner.explicit_policy = policy;
        g_owner.explicit_cached = true;
    }
    return g_owner.explicit_filters[index];
}

bool application_user_settings_content_confirmation_required() noexcept {
    return account_content_confirmation_required(content_policy());
}

void application_user_settings_request_content_confirmation() noexcept {
    if (!account_online() || !application_user_settings_content_confirmation_required())
        return;
    ApplicationTextFormRequest request{};
    request.title = utf8_literal("Confirm your age in Discord");
    request.context = utf8_literal("To show sensitive media, open Discord and go to User Settings > Account Status, "
                                   "then choose Confirm Age. Return here after Discord updates your account.");
    request.accept = utf8_literal("Open Discord account status");
    request.cancel = utf8_literal("Close");
    request.owner = {reinterpret_cast<u64>(g_app.account.instance), g_app.account.status.profile_id,
                     g_app.account_ui->chat_state.self_user_id};
    request.valid = content_confirmation_valid;
    request.save = content_confirmation_open;
    request.confirmation_only = true;
    application_text_form_open(request);
}

u64 application_user_settings_revision() noexcept {
    return store() ? store()->revision : 0;
}
Utf8View application_user_settings_session_hash() noexcept {
    const auto* s = store();
    return s ? Utf8View{s->session_hash, s->session_hash_bytes} : Utf8View{};
}
bool application_user_settings_ready() noexcept {
    scope();
    return loaded();
}
Utf8View application_user_settings_failure() noexcept {
    return g_owner.failed ? g_owner.status : Utf8View{};
}
bool application_user_settings_guild_restricted(GuildPrivacyList list, u64 guild_id) noexcept {
    const u32 index = static_cast<u32>(list);
    return guild_id && index < 4 && loaded() && user_settings_store_contains(*store(), kGuildLists[index], guild_id);
}
bool application_user_settings_set_guild_restricted(GuildPrivacyList list, u64 guild_id, bool restricted) noexcept {
    const u32 index = static_cast<u32>(list);
    return guild_id && index < 4 && loaded() &&
           write(kGuildLists[index], user_settings_member(guild_id, restricted));
}

bool application_user_settings_project(SettingBinding binding, UiSettingsControl* control,
                                       UiSettingsControlPresentation* presentation) noexcept {
    scope();
    const bool ready = loaded();
    const auto notice = [&]() noexcept {
        if (ready && !g_owner.failed)
            return false;
        control->description = g_owner.failed      ? g_owner.status
                               : !account_online() ? utf8_literal("Connect to Discord to change these settings.")
                                                   : utf8_literal("Loading your Discord settings…");
        control->label = g_owner.failed ? utf8_literal("Settings not saved") : utf8_literal("Synced settings");
        if (g_owner.failed)
            presentation->variant = UiSettingsVariant::critical;
        return true;
    };
    control->enabled = ready;
    switch (binding) {
    case SettingBinding::user_settings_notice:
    case SettingBinding::privacy_notice:
    case SettingBinding::language_notice:
    case SettingBinding::chat_notice:
    case SettingBinding::activity_notice:
    case SettingBinding::accessibility_notice:
    case SettingBinding::notifications_notice:
        return notice();
    case SettingBinding::language: {
        // Unset is the default locale, en-US; one this list doesn't name selects nothing.
        UserSettingsValue locale;
        control->selected_choice = kLocaleUnitedStates;
        if (store() && user_settings_store_read(*store(), kLocale, &locale) && locale.present) {
            control->selected_choice = kUiSettingsNoChoice;
            for (u32 i = 0; i < sizeof(kLocales) / sizeof(kLocales[0]); ++i)
                if (utf8_equal({locale.text, locale.text_bytes}, kLocales[i]))
                    control->selected_choice = i;
        }
        return true;
    }
    case SettingBinding::notify_reactions: {
        // NOTIFICATIONS_ENABLED 0, ONLY_DMS 1, NOTIFICATIONS_DISABLED 2: the choices' order.
        const u64 value = read_number(kReactionNotifications, 0);
        control->selected_choice = value <= 2 ? static_cast<u32>(value) : 0;
        return true;
    }
    case SettingBinding::mobile_push_delay: {
        // afk_timeout in seconds (unset: 60), shown as the whole minute 1..10 at or below it.
        const u64 minutes = read_number(kAfkTimeout, 60) / 60;
        control->selected_choice = minutes >= 1 && minutes <= 10 ? static_cast<u32>(minutes - 1) : 0;
        return true;
    }
    case SettingBinding::compact_avatars:
        // Cozy messages always show avatars: the switch reads on and only works in compact mode.
        control->checked = read_number(kCompact, 0) == 0 || read_number(kCompactAvatars, 0) != 0;
        control->enabled = ready && read_number(kCompact, 0) != 0;
        if (read_number(kCompact, 0) == 0)
            control->description =
                utf8_literal("To hide avatars, go to Accessibility and select Compact under Chat Message Display.");
        return true;
    case SettingBinding::ui_density:
        control->selected_choice = index_of(kUiDensityValues, 3, read_number(kUiDensity, 4), 1);
        return true;
    case SettingBinding::animate_stickers: {
        const u64 value = read_number(kAnimateStickers, 0);
        control->selected_choice = value <= 2 ? static_cast<u32>(value) : 0;
        return true;
    }
    case SettingBinding::render_spoilers:
        control->selected_choice = index_of(kSpoilerChoices, 3, spoiler_choice(), 0);
        return true;
    case SettingBinding::dm_from_members:
        control->checked = !dm_restricted();
        return true;
    case SettingBinding::message_requests:
        // ON is default_message_request_restricted false (or unset). Requests
        // need DMs: while DMs are restricted the switch reads off, disabled.
        control->checked = !dm_restricted() && read_number(kMessageRequests, 0) == 0;
        control->enabled = ready && !dm_restricted();
        return true;
    case SettingBinding::nsfw_commands:
    case SettingBinding::nsfw_guilds:
        // Age-restricted access is for adults; restricted accounts see it off.
        control->checked = !restricted_account() &&
                           read_number(binding == SettingBinding::nsfw_commands ? kNsfwCommands : kNsfwGuilds, 0) != 0;
        control->enabled = ready && !restricted_account();
        return true;
    case SettingBinding::dm_spam_filter:
        control->selected_choice = index_of(kSpamValues, 3, spam_filter(), 1);
        return true;
    case SettingBinding::activity_servers:
        control->selected_choice = index_of(kActivityServerValues, 3, activity_servers(), 0);
        return true;
    case SettingBinding::explicit_friends:
    case SettingBinding::explicit_others:
    case SettingBinding::explicit_guilds: {
        const auto options = explicit_options(binding);
        const auto value = application_user_settings_explicit_filter(explicit_context(binding));
        const u32 selected = static_cast<u32>(value);
        control->choices = {kExplicitChoices + options.first, options.count};
        control->enabled = ready && options.enabled;
        control->selected_choice = selected >= options.first && selected - options.first < options.count
                                       ? selected - options.first : kUiSettingsNoChoice;
        if (content_policy().flags & account_content_parent_managed)
            control->description = value == MessageExplicitFilter::show
                ? utf8_literal("Current setting: Show. Managed by your parent or guardian.")
                : value == MessageExplicitFilter::blur
                    ? utf8_literal("Current setting: Blur. Managed by your parent or guardian.")
                    : utf8_literal("Current setting: Block. Managed by your parent or guardian.");
        else if (control->selected_choice == kUiSettingsNoChoice)
            control->description = binding == SettingBinding::explicit_guilds &&
                                           content_policy().eligibility == AccountContentEligibility::restricted
                ? (value == MessageExplicitFilter::show
                       ? utf8_literal("Current setting: Show. Changes to server filtering are restricted for this account.")
                       : utf8_literal("Current setting: Block. Changes to server filtering are restricted for this account."))
                : value == MessageExplicitFilter::show
                    ? utf8_literal("Current setting: Show. This account can choose Blur or Block.")
                    : utf8_literal("Current setting: Block. Server channels support Show or Blur.");
        return true;
    }
    case SettingBinding::friend_everyone:
    case SettingBinding::friend_mutual_friends:
    case SettingBinding::friend_mutual_guilds: {
        const u64 flags = read_number(kFriendFlags, kFriendDefault);
        control->checked = binding == SettingBinding::friend_everyone ? (flags & kFriendDefault) == kFriendDefault
                           : binding == SettingBinding::friend_mutual_friends ? (flags & kFriendsOfFriends) != 0
                                                                              : (flags & kServerMembers) != 0;
        return true;
    }
    case SettingBinding::profile_visibility:
        control->selected_choice = index_of(kVisibilityValues, 3, read_number(kVisibility, 3), 0);
        return true;
    case SettingBinding::profile_updates:
        control->checked = read_number(kProfileUpdates, 1) != 0;
        return true;
    case SettingBinding::message_display:
        control->selected_choice = read_number(kCompact, 0) ? 1 : 0;
        return true;
    case SettingBinding::time_format: {
        const u64 cycle = read_number(kHourCycle, 0);
        control->selected_choice = cycle <= 2 ? static_cast<u32>(cycle) : 0;
        return true;
    }
    default: {
        SyncedToggle toggle{};
        if (synced_toggle(binding, &toggle)) {
            control->checked = (read_number(toggle.field, toggle.fallback ? 1 : 0) != 0) != toggle.inverted;
            return true;
        }
        UserSettingsField field{};
        if (!chat_toggle(binding, &field))
            return false;
        control->checked = read_number(field, 1) != 0;
        return true;
    }
    }
}

bool application_user_settings_activate(SettingBinding binding, bool checked, u32 choice) noexcept {
    if (!loaded())
        return false;
    switch (binding) {
    case SettingBinding::dm_from_members:
        // The official switch writes default_guilds_restricted_v2 alone.
        return write_number(kDmRestrictedV2, !checked);
    case SettingBinding::message_requests:
        return !dm_restricted() && write_number(kMessageRequests, !checked);
    case SettingBinding::nsfw_commands:
    case SettingBinding::nsfw_guilds:
        if (restricted_account())
            return false;
        if (checked && application_user_settings_content_confirmation_required()) {
            application_user_settings_request_content_confirmation();
            return true;
        }
        return write_number(binding == SettingBinding::nsfw_commands ? kNsfwCommands : kNsfwGuilds, checked);
    case SettingBinding::dm_spam_filter:
        return choice < 3 && write_number(kSpam, kSpamValues[choice]);
    case SettingBinding::activity_servers:
        return choice < 3 && write_number(kActivityServers, kActivityServerValues[choice]);
    case SettingBinding::explicit_friends:
    case SettingBinding::explicit_others:
    case SettingBinding::explicit_guilds: {
        const auto options = explicit_options(binding);
        if (!options.enabled || choice >= options.count)
            return false;
        const u32 value = options.first + choice + 1;
        if (value == 1 && application_user_settings_content_confirmation_required()) {
            application_user_settings_request_content_confirmation();
            return true;
        }
        return write_number(explicit_field(binding), value);
    }
    case SettingBinding::friend_everyone:
    case SettingBinding::friend_mutual_friends:
    case SettingBinding::friend_mutual_guilds: {
        u64 flags = read_number(kFriendFlags, kFriendDefault);
        if (binding == SettingBinding::friend_everyone)
            flags = checked ? kFriendDefault : kFriendsOfFriends | kServerMembers;
        else {
            const u64 bit = binding == SettingBinding::friend_mutual_friends ? kFriendsOfFriends : kServerMembers;
            flags = checked ? flags | bit : flags & ~(bit | kEveryone);
        }
        return write_number(kFriendFlags, flags);
    }
    case SettingBinding::profile_visibility:
        return choice < 3 && write_number(kVisibility, kVisibilityValues[choice]);
    case SettingBinding::profile_updates:
        return write_number(kProfileUpdates, checked);
    case SettingBinding::message_display:
        return choice < 2 && write_number(kCompact, choice);
    case SettingBinding::time_format:
        return choice < 3 && write_number(kHourCycle, choice);
    case SettingBinding::animate_stickers:
        return choice < 3 && write_number(kAnimateStickers, choice);
    case SettingBinding::render_spoilers:
        return choice < 3 && write(kSpoilers, user_settings_text(kSpoilerValues[kSpoilerChoices[choice]]));
    case SettingBinding::language:
        return choice < sizeof(kLocales) / sizeof(kLocales[0]) && write(kLocale, user_settings_text(kLocales[choice]));
    case SettingBinding::notify_reactions:
        return choice < 3 && write_number(kReactionNotifications, choice);
    case SettingBinding::mobile_push_delay:
        return choice < 10 && write_number(kAfkTimeout, (choice + 1) * 60);
    case SettingBinding::compact_avatars:
        return read_number(kCompact, 0) != 0 && write_number(kCompactAvatars, checked);
    case SettingBinding::ui_density:
        return choice < 3 && write_number(kUiDensity, kUiDensityValues[choice]);
    default: {
        SyncedToggle toggle{};
        if (synced_toggle(binding, &toggle))
            return write_number(toggle.field, checked != toggle.inverted);
        UserSettingsField field{};
        return chat_toggle(binding, &field) && write_number(field, checked);
    }
    }
}
} // namespace maicord::app_detail
