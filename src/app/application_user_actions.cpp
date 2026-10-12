#include "application_user_actions.hpp"
#include "application_account_presentation.hpp"
#include "application_commands.hpp"
#include "application_composer.hpp"
#include "application_friends.hpp"
#include "application_member_moderation.hpp"
#include "application_message_delete.hpp"
#include "application_message_history.hpp"
#include "application_message_interaction.hpp"
#include "application_private_channel.hpp"
#include "application_private_conversation.hpp"
#include "application_profile.hpp"
#include "application_report.hpp"
#include "application_state.hpp"
#include "application_text_form.hpp"
#include "maicord/app/private_conversation.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/role_policy.hpp"
#include "maicord/model/user_note_store.hpp"
#include "maicord/platform/speech.hpp"
#include "maicord/protocol/application_commands.hpp"
#include "maicord/protocol/guild_admin.hpp"
#include "maicord/ui/clipboard.hpp"
#include "application_plugins.hpp"
#include "maicord/ui/guild_context_policy.hpp"
#include "maicord/ui/user_context_menu.hpp"

#include <Windows.h>

namespace maicord::app_detail {
namespace {
using A = UserContextAction;
constexpr u32 kModerationCapacity = 6;
constexpr u32 kPrivateMemberCapacity = 4;
constexpr u32 kLeadingCapacity = 4;
constexpr u32 kMenuCapacity = kLeadingCapacity + kUserContextMaximumItems + kModerationCapacity +
                              kPrivateMemberCapacity + 2 + kApplicationPluginMenuRows;
constexpr u32 kInviteSentMemory = 16;
// Invite to server: the official client creates a one-use invite and sends its link in DM.
constexpr u32 kInviteMaxAgeSeconds = 86'400;

enum class FollowUp : u8 { none, invite, roles, apps };
enum class Work : u8 { none, invite_create, invite_dm, role, flags, note_load, note_save };
enum class InvitePhase : u8 { idle, create, creating, dm, opening };

struct InviteJob {
    u64 user_id, guild_id, channel_id;
    InvitePhase phase;
    u8 code_bytes;
    char code[kGuildAdminInviteCodeBytes];
};

struct State {
    u64 scope;
    // Captured with the open menu; every follow-up and action revalidates it.
    u64 menu_user, menu_guild, menu_message, menu_dm_channel;
    UserMenuSource source;
    FollowUp follow;
    PopupItem items[kUserContextGuildItems + 2];
    u64 invite_guilds[kUserContextGuildItems];
    u32 invite_count;
    u64 sent_user, sent_guilds[kInviteSentMemory];
    u32 sent_count;
    u64 role_ids[kUserContextRoleItems];
    u32 role_count;
    ApplicationContextCommand apps[kUserContextAppItems];
    u32 app_count;
    u8 app_type;
    // Requests.
    InviteJob invite;
    u64 role_user, role_guild, role_id;
    bool role_add, role_queued;
    // The last toggle until the member update shows it (cleared on failure).
    u64 role_override_id;
    bool role_override_add;
    u64 flags_channel, flags_message;
    u32 flags_value;
    bool flags_queued;
    u64 request_id, request_user, retry_at;
    Work work;
    u64 note_form_user;
    u64 note_form_serial;
    UserNoteStore* notes;
    Speech speech;
    char speak[2048];
    u32 structurals[8192];
};
State* g_state;

bool available() noexcept {
    return g_app.account_ui && !g_app.login.visible && g_app.account.instance;
}
ChatStateStore& chat() noexcept {
    return g_app.account_ui->chat_state;
}
u64 self_id() noexcept {
    return g_app.account_ui ? g_app.account_ui->chat_state.self_user_id : 0;
}
State* ensure() noexcept {
    if (!g_state) {
        g_state = static_cast<State*>(VirtualAlloc(nullptr, sizeof(State), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!g_state)
            return nullptr;
        g_state->scope = g_app.plugin_account_scope_serial;
    }
    if (g_state->scope != g_app.plugin_account_scope_serial) {
        // Another account: drop captured menus, queued work and cached notes.
        auto* notes = g_state->notes;
        const auto speech = g_state->speech;
        clear_bytes(g_state, static_cast<u32>(offsetof(State, notes)));
        g_state->scope = g_app.plugin_account_scope_serial;
        g_state->speech = speech;
        if (notes)
            user_note_reset(notes);
        g_state->notes = notes;
    }
    return g_state;
}
UserNoteStore* notes() noexcept {
    auto* s = ensure();
    if (!s)
        return nullptr;
    if (!s->notes)
        s->notes = static_cast<UserNoteStore*>(
            VirtualAlloc(nullptr, sizeof(UserNoteStore), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    return s->notes;
}
void wake() noexcept {
    if (g_app.window)
        PostMessageW(g_app.window, WM_NULL, 0, 0);
}
u32 format_id(u64 value, char* out) noexcept {
    return message_context_format_snowflake(value, {out, 20});
}

// Facts ---------------------------------------------------------------------

bool is_bot(u64 user_id) noexcept {
    const auto* appearance = user_appearance_find(g_app.account_ui->user_store.appearance, user_id);
    return appearance && (appearance->fields & appearance_bot) && appearance->bot;
}
u64 menu_guild(u64 guild_id) noexcept {
    return guild_id ? guild_id : chat().selected_guild_id;
}
bool composer_accepts_mention() noexcept {
    const u64 channel_id = g_app.account_ui->active_channel_id;
    const auto* channel = channel_id ? chat_state_store_find_channel(chat(), channel_id) : nullptr;
    return channel && g_app.message_composer.channel_id == channel_id && !g_app.message_edit.editor_active &&
           (channel->actions & channel_action_send_messages);
}
// Servers this user can be invited to, in rail order, skipping the menu's own.
u32 collect_invite_guilds(u64 exclude, u64* output, u32 capacity) noexcept {
    const auto& view = g_app.account_ui->navigation_view;
    u32 count = 0;
    for (u32 i = 0; i < view.guild_count && count < capacity; ++i) {
        const auto& row = view.guild_rows[i];
        if (!guild_view_row_is_guild(row) || row.unavailable || row.id == exclude)
            continue;
        if (guild_context_invite_channel(chat(), row.id, 0))
            output[count++] = row.id;
    }
    return count;
}
// Editable roles of the guild (highest first) and whether the member holds them.
u32 collect_roles(u64 guild_id, u64 user_id, u64* ids, bool* held, u32 capacity) noexcept {
    const auto& state = chat();
    const auto policy = role_edit_policy(state, guild_id);
    if (!policy.allowed)
        return 0;
    const auto* member = guild_member_store_find(g_app.account_ui->guild_member_store, guild_id, user_id);
    if (!member || !guild_member_store_role_ids_known(*member))
        return 0;
    const auto member_roles = guild_member_store_role_ids(*member);
    const RoleRecord* found[kUserContextRoleItems];
    u32 count = 0;
    for (u32 i = 0; i < state.role_count && count < capacity && count < kUserContextRoleItems; ++i) {
        const auto& role = state.roles[i];
        if (role.guild_id != guild_id || role.id == guild_id || role.managed || !role_editable(policy, role))
            continue;
        found[count++] = &role;
    }
    for (u32 i = 1; i < count; ++i)
        for (u32 j = i; j > 0 && (found[j]->position > found[j - 1]->position ||
                                  (found[j]->position == found[j - 1]->position && found[j]->id < found[j - 1]->id));
             --j) {
            const auto* swap = found[j];
            found[j] = found[j - 1];
            found[j - 1] = swap;
        }
    for (u32 i = 0; i < count; ++i) {
        ids[i] = found[i]->id;
        held[i] = false;
        for (u32 r = 0; r < member_roles.count; ++r)
            held[i] = held[i] || member_roles[r] == found[i]->id;
    }
    return count;
}
bool role_busy() noexcept {
    return g_state && (g_state->role_queued || g_state->work == Work::role);
}

// Menu ------------------------------------------------------------------------

bool menu_current(u64 user_id) noexcept {
    if (!g_state || g_state->scope != g_app.plugin_account_scope_serial || g_state->menu_user != user_id ||
        !g_app.popup_scene.active)
        return false;
    if (g_app.popup_owner_kind == PopupOwnerKind::private_channel)
        return g_state->menu_dm_channel && g_app.popup_scene.owner_id == g_state->menu_dm_channel;
    return g_app.popup_scene.owner_id == user_id && g_app.popup_owner_kind == PopupOwnerKind::member;
}
bool reopen(ConstSlice<PopupItem> items, u64 owner, PopupOwnerKind kind, float width) noexcept {
    const UiRect anchor = g_app.popup_scene.anchor;
    application_plugins_menu_dismissed();
    if (!popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, owner, items, anchor,
                          application_viewport_dips(), width)
             .ok())
        return false;
    set_popup_owner(kind);
    g_app.context_focus = &g_app.popup_scene.widgets;
    return refresh_popup_text();
}
bool open_invites() noexcept {
    auto& s = *g_state;
    s.invite_count = collect_invite_guilds(s.menu_guild, s.invite_guilds, kUserContextGuildItems);
    if (!s.invite_count)
        return dismiss_popup();
    UserContextChoice choices[kUserContextGuildItems];
    for (u32 i = 0; i < s.invite_count; ++i) {
        const auto* guild = chat_state_store_find_guild(chat(), s.invite_guilds[i]);
        bool sent = false;
        if (s.sent_user == s.menu_user)
            for (u32 j = 0; j < s.sent_count; ++j)
                sent = sent || s.sent_guilds[j] == s.invite_guilds[i];
        choices[i] = {guild ? chat_state_store_guild_name(chat(), *guild) : utf8_literal("Server"), {}, sent};
    }
    const u32 count = user_context_invites_build({choices, s.invite_count}, {s.items, kUserContextGuildItems});
    s.follow = FollowUp::invite;
    return count && (reopen({s.items, count}, s.menu_user, PopupOwnerKind::member, 240.0f) || dismiss_popup());
}
bool open_roles() noexcept {
    auto& s = *g_state;
    bool held[kUserContextRoleItems]{};
    s.role_count = collect_roles(s.menu_guild, s.menu_user, s.role_ids, held, kUserContextRoleItems);
    if (!s.role_count)
        return dismiss_popup();
    UserContextRole roles[kUserContextRoleItems];
    for (u32 i = 0; i < s.role_count; ++i) {
        if (s.role_override_id == s.role_ids[i] && s.role_user == s.menu_user)
            held[i] = s.role_override_add;
        const auto* role = chat_state_store_find_role(chat(), s.menu_guild, s.role_ids[i]);
        roles[i] = {role ? chat_state_store_role_name(chat(), *role) : Utf8View{}, role ? role->color : 0, held[i],
                    !role_busy()};
    }
    const u32 count = user_context_roles_build({roles, s.role_count}, {s.items, kUserContextRoleItems});
    s.follow = FollowUp::roles;
    return count && (reopen({s.items, count}, s.menu_user, PopupOwnerKind::member, 240.0f) || dismiss_popup());
}
bool open_apps(u8 type, u64 owner, PopupOwnerKind kind) noexcept {
    auto& s = *g_state;
    bool loading = false;
    s.app_type = type;
    s.app_count = application_commands_context(type, {s.apps, kUserContextAppItems}, &loading);
    UserContextChoice choices[kUserContextAppItems];
    for (u32 i = 0; i < s.app_count; ++i)
        choices[i] = {s.apps[i].name, s.apps[i].application_name, false};
    const u32 count = user_context_apps_build({choices, s.app_count}, loading, {s.items, kUserContextAppItems});
    s.follow = FollowUp::apps;
    return count && (reopen({s.items, count}, owner, kind, 260.0f) || dismiss_popup());
}

// Notes -------------------------------------------------------------------------

bool note_form_valid(TextFormOwnerKey key) noexcept {
    return g_state && available() && key.scope == g_app.plugin_account_scope_serial &&
           key.collection == g_state->note_form_serial && key.item && key.item != self_id();
}
bool note_form_save(TextFormOwnerKey key, Utf8View text) noexcept {
    if (!note_form_valid(key) || !user_note_valid(text))
        return false;
    auto* store = notes();
    const u64 now = clock_monotonic_milliseconds();
    if (!store || !user_note_edit(store, key.item, text, now))
        return false;
    user_note_flush(store, key.item, now);
    wake();
    return true;
}
bool open_note_form(u64 user_id) noexcept {
    auto& s = *g_state;
    s.note_form_user = 0;
    if (++s.note_form_serial == 0)
        ++s.note_form_serial;
    const auto* user = user_store_find(g_app.account_ui->user_store, user_id);
    static char context[192];
    u32 used = 0;
    const auto append = [&](Utf8View text) noexcept {
        const u32 take = minimum<u32>(text.bytes, sizeof(context) - used);
        copy_bytes(context + used, text.data, take);
        used += take;
    };
    append(utf8_literal("Only you can see notes about "));
    append(user ? user_store_name(g_app.account_ui->user_store, *user) : utf8_literal("this user"));
    append(utf8_literal("."));
    ApplicationTextFormRequest request{};
    request.title = utf8_literal("Note");
    request.context = {context, used};
    request.label = utf8_literal("Note");
    request.accept = utf8_literal("Save");
    request.initial = notes() ? user_note_text(*notes(), user_id) : Utf8View{};
    request.owner = {g_app.plugin_account_scope_serial, s.note_form_serial, user_id};
    request.valid = note_form_valid;
    request.save = note_form_save;
    request.maximum_codepoints = kUserNoteMaximumCodepoints;
    request.maximum_bytes = kUserNoteMaximumBytes;
    return application_text_form_open(request);
}
// Notes open only once the saved text is known, so Save never overwrites a
// note the user has not seen.
bool begin_note(u64 user_id) noexcept {
    auto* store = notes();
    if (!store || !user_note_request(store, user_id))
        return false;
    const auto* entry = user_note_find(*store, user_id);
    if (entry && entry->state == UserNoteState::loaded)
        return open_note_form(user_id);
    g_state->note_form_user = user_id;
    wake();
    return true;
}

// Invite to server ---------------------------------------------------------------

bool begin_invite(u64 guild_id) noexcept {
    auto& s = *g_state;
    if (s.invite.phase != InvitePhase::idle)
        return false;
    const u64 channel = guild_context_invite_channel(chat(), guild_id, 0);
    if (!channel)
        return false;
    s.invite = {s.menu_user, guild_id, channel, InvitePhase::create, 0, {}};
    if (s.sent_user != s.menu_user) {
        s.sent_user = s.menu_user;
        s.sent_count = 0;
    }
    if (s.sent_count < kInviteSentMemory)
        s.sent_guilds[s.sent_count++] = guild_id;
    wake();
    return true;
}
// Sends the invite link in the DM through the ordinary send queue.
bool send_invite_link(u64 channel_id) noexcept {
    auto& s = *g_state;
    char text[64];
    constexpr auto prefix = utf8_literal("https://discord.gg/");
    copy_bytes(text, prefix.data, prefix.bytes);
    copy_bytes(text + prefix.bytes, s.invite.code, s.invite.code_bytes);
    const u32 bytes = prefix.bytes + s.invite.code_bytes;
    char storage[96];
    MessageComposer composer{};
    if (!message_composer_initialize(&composer, {storage, sizeof(storage)}) ||
        !message_composer_restore(&composer, channel_id, {text, bytes}, bytes, bytes))
        return false;
    auto& ui = *g_app.account_ui;
    const auto queued = message_composer_controller_queue_send(&ui.message_composer_controller, true, self_id(),
                                                               clock_unix_milliseconds(),
                                                               clock_monotonic_milliseconds(), 0, {}, {}, &composer);
    pump_message_history();
    return queued.ok();
}
void finish_invite(bool ok) noexcept {
    if (!ok)
        log_write(LogLevel::warning, "user_menu", "invite_failed");
    g_state->invite = {};
}
u64 read_channel_id(Utf8View body) noexcept {
    struct Find {
        u64 id;
    } find{};
    const auto scan = json_scan_structurals_avx1(body, {g_state->structurals, 8192});
    if (!scan.ok() || !json_visit_root_object(
                          body, {g_state->structurals, scan.count}, 8,
                          [](void* p, JsonString name, JsonValue value) noexcept {
                              JsonString text{};
                              if (json_string_equals_ascii(name, utf8_literal("id")) &&
                                  json_value_as_string(value, &text))
                                  json_string_to_u64(text, &static_cast<Find*>(p)->id);
                              return true;
                          },
                          &find)
                          .ok())
        return 0;
    return find.id;
}

// Lane -------------------------------------------------------------------------------

bool lane_free() noexcept {
    return g_app.details_request_owner == MessageHistoryRequestOwner::none;
}
u64 next_id() noexcept {
    u64 id = ++g_app.account_ui->message_history_request_id;
    if (!id)
        id = ++g_app.account_ui->message_history_request_id;
    return id;
}
bool connected() noexcept {
    return available() && g_app.account.status.gateway_ready &&
           g_app.account.status.required_action == AccountRequirement::none;
}
bool claim(u64 id, Work work, u64 user) noexcept {
    auto& s = *g_state;
    s.request_id = id;
    s.work = work;
    s.request_user = user;
    s.retry_at = 0;
    g_app.details_request_owner = MessageHistoryRequestOwner::channel_overview;
    g_app.details_request_id = id;
    return true;
}
} // namespace

// Public: user menu -------------------------------------------------------------------

bool application_user_menu_open(u64 user_id, UiRect anchor, u64 guild_id, UserMenuSource source,
                                ConstSlice<PopupItem> leading, const UiPopupFeedback* feedback) noexcept {
    if (!g_app.account_ui || g_app.login.visible || !user_id || leading.count > kLeadingCapacity || !ensure())
        return false;
    auto& s = *g_state;
    const u64 guild = source == UserMenuSource::private_channel ? 0 : menu_guild(guild_id);
    const bool self = user_id == self_id();
    UserContextFacts facts{};
    u8 relation = 0;
    facts.relations_known = application_friends_user_relation(user_id, &relation);
    facts.relation = static_cast<UserContextRelation>(relation);
    facts.self = self;
    facts.bot = is_bot(user_id);
    facts.can_mention = composer_accepts_mention();
    facts.can_message = true;
    const u64 dm = private_conversation_cached(chat(), user_id);
    facts.can_call = dm != 0;
    if (auto* store = notes()) {
        user_note_request(store, user_id);
        facts.has_note = user_note_text(*store, user_id).bytes != 0;
        wake();
    }
    u64 scratch[kUserContextGuildItems];
    facts.can_invite = !self && collect_invite_guilds(guild, scratch, 1) != 0;
    bool held[kUserContextRoleItems];
    facts.can_edit_roles = guild && collect_roles(guild, user_id, scratch, held, 1) != 0;
    if (!self && g_app.account_ui->active_channel_id && g_app.account.api.application_command) {
        ApplicationContextCommand probe[1];
        bool loading = false;
        facts.has_apps = application_commands_context(kCommandTypeUser, {probe, 1}, &loading) || loading;
    }
    facts.can_report = !self && g_app.account.api.user_social;
    facts.developer_mode = false; // Copy user ID joins the copy group below.
    PopupItem items[kMenuCapacity]{};
    u32 count = 0;
    for (u32 i = 0; i < leading.count; ++i)
        items[count++] = leading[i];
    const u32 builtins = user_context_menu_build(facts, {items + count, kUserContextMaximumItems});
    if (!builtins)
        return false;
    if (count)
        items[count].flags |= popup_item_separator_before;
    count += builtins;
    u32 extra = guild ? application_member_moderation_menu_items(guild, user_id, {items + count, kModerationCapacity})
                      : 0;
    if (!extra && !guild)
        extra = application_private_channel_member_items(user_id, {items + count, kPrivateMemberCapacity});
    if (extra)
        items[count].flags |= popup_item_separator_before;
    count += extra;
    items[count++] = {utf8_literal("Copy username"), popup_action_copy_member_name,
                      static_cast<u16>(popup_item_enabled | popup_item_separator_before), 0};
    if (g_app.preferences.developer_mode)
        items[count++] = {utf8_literal("Copy user ID"), popup_action_copy_member_id, popup_item_enabled, 0};
    const u32 native = count;
    count += application_plugins_menu_items(ApplicationPluginMenuSurface::user, user_id,
                                            {items + count, kApplicationPluginMenuRows}, guild);
    auto opened = popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, user_id, {items, count}, anchor,
                                   application_viewport_dips(), 240.0f, false, 0, 0, 0, feedback);
    if (!opened.ok() && count != native) {
        application_plugins_menu_dismissed();
        opened = popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, user_id, {items, native}, anchor,
                                  application_viewport_dips(), 240.0f, false, 0, 0, 0, feedback);
    }
    if (!opened.ok())
        return false;
    s.menu_user = user_id;
    s.menu_guild = guild;
    s.menu_message = 0;
    s.menu_dm_channel = 0;
    s.source = source;
    s.follow = FollowUp::none;
    set_popup_owner(PopupOwnerKind::member);
    if (!application_plugins_menu_bind())
        return false;
    if (source == UserMenuSource::member_list) {
        member_list_scene_focus_user(&g_app.member_list_scene, user_id);
        g_app.context_focus = &g_app.member_list_scene.widgets;
    }
    return refresh_popup_text();
}

u32 application_user_actions_private_channel_items(u64 channel_id, Slice<PopupItem> output) noexcept {
    if (!available() || !ensure() || !chat().private_channels)
        return 0;
    const auto* record = private_channel_store_find(*chat().private_channels, channel_id);
    const auto recipients = record ? private_channel_store_recipients(*chat().private_channels, *record)
                                   : ConstSlice<u64>{};
    if (recipients.count != 1 || !recipients[0] || recipients[0] == self_id())
        return 0;
    const u64 user = recipients[0];
    UserContextFacts facts{};
    u8 relation = 0;
    facts.relations_known = application_friends_user_relation(user, &relation);
    facts.relation = static_cast<UserContextRelation>(relation);
    facts.bot = is_bot(user);
    facts.can_call = true;
    if (auto* store = notes()) {
        user_note_request(store, user);
        facts.has_note = user_note_text(*store, user).bytes != 0;
        wake();
    }
    u64 scratch[1];
    facts.can_invite = collect_invite_guilds(0, scratch, 1) != 0;
    if (g_app.account_ui->active_channel_id == channel_id && g_app.account.api.application_command) {
        ApplicationContextCommand probe[1];
        bool loading = false;
        facts.has_apps = application_commands_context(kCommandTypeUser, {probe, 1}, &loading) || loading;
    }
    facts.can_report = g_app.account.api.user_social != nullptr;
    facts.developer_mode = g_app.preferences.developer_mode;
    const u32 count = user_context_menu_build(facts, output);
    if (!count)
        return 0;
    output[0].flags |= popup_item_separator_before;
    auto& s = *g_state;
    s.menu_user = user;
    s.menu_guild = 0;
    s.menu_message = 0;
    s.menu_dm_channel = channel_id;
    s.source = UserMenuSource::private_channel;
    s.follow = FollowUp::none;
    return count;
}

bool application_user_menu_action(u32 action) noexcept {
    return user_context_action(action);
}

bool application_user_menu_activate(u32 action) noexcept {
    if (!available() || !g_state || !g_app.popup_scene.active)
        return dismiss_popup();
    auto& s = *g_state;
    const u64 owner = g_app.popup_scene.owner_id;
    // Message Apps: the follow-up of a message menu targets the message.
    if (g_app.popup_owner_kind == PopupOwnerKind::message) {
        const u32 first = static_cast<u32>(A::app_first);
        if (s.follow != FollowUp::apps || s.menu_message != owner || action < first || action - first >= s.app_count)
            return dismiss_popup();
        const auto command = s.apps[action - first];
        dismiss_popup();
        return application_commands_context_invoke(kCommandTypeMessage, command.command_id, owner) || true;
    }
    const u64 user = g_app.popup_owner_kind == PopupOwnerKind::private_channel ? s.menu_user : owner;
    if (!menu_current(user))
        return dismiss_popup();
    if (action >= static_cast<u32>(A::invite_first) && action < static_cast<u32>(A::role_first)) {
        const u32 index = action - static_cast<u32>(A::invite_first);
        if (s.follow != FollowUp::invite || index >= s.invite_count)
            return dismiss_popup();
        const u64 guild = s.invite_guilds[index];
        dismiss_popup();
        return begin_invite(guild) || true;
    }
    if (action >= static_cast<u32>(A::role_first) && action < static_cast<u32>(A::app_first) - 1) {
        const u32 index = action - static_cast<u32>(A::role_first);
        if (s.follow != FollowUp::roles || index >= s.role_count || role_busy())
            return dismiss_popup();
        bool held[kUserContextRoleItems]{};
        u64 ids[kUserContextRoleItems];
        const u32 count = collect_roles(s.menu_guild, user, ids, held, kUserContextRoleItems);
        u32 at = count;
        for (u32 i = 0; i < count; ++i)
            if (ids[i] == s.role_ids[index])
                at = i;
        if (at == count)
            return dismiss_popup();
        s.role_user = user;
        s.role_guild = s.menu_guild;
        s.role_id = ids[at];
        if (s.role_override_id == ids[at] && s.role_user == user)
            held[at] = s.role_override_add;
        s.role_add = !held[at];
        s.role_queued = true;
        s.role_override_id = ids[at];
        s.role_override_add = s.role_add;
        wake();
        return open_roles() || true;
    }
    if (action >= static_cast<u32>(A::app_first)) {
        const u32 index = action - static_cast<u32>(A::app_first);
        if (s.follow != FollowUp::apps || index >= s.app_count)
            return dismiss_popup();
        const auto command = s.apps[index];
        dismiss_popup();
        return application_commands_context_invoke(kCommandTypeUser, command.command_id, user) || true;
    }
    const UiRect anchor = g_app.popup_scene.anchor;
    switch (static_cast<A>(action)) {
    case A::profile:
        dismiss_popup();
        // From a profile card this is View Full Profile.
        if (s.source == UserMenuSource::profile && application_profile_expand(user))
            return true;
        return application_profile_open(user, anchor) || true;
    case A::mention: {
        dismiss_popup();
        if (!composer_accepts_mention())
            return true;
        char text[32];
        u32 bytes = 0;
        text[bytes++] = '<';
        text[bytes++] = '@';
        bytes += format_id(user, text + bytes);
        text[bytes++] = '>';
        text[bytes++] = ' ';
        const u32 first = minimum(g_app.message_composer.anchor, g_app.message_composer.caret);
        const u32 last = maximum(g_app.message_composer.anchor, g_app.message_composer.caret);
        return focus_message_composer_control(MessageComposerControl::edit) &&
               composer_apply_result(message_composer_replace_range(&g_app.message_composer, first, last, {text, bytes}),
                                     true);
    }
    case A::message:
        dismiss_popup();
        return queue_private_conversation(PrivateConversationOperation::open, user) || true;
    case A::call: {
        dismiss_popup();
        const u64 dm = private_conversation_cached(chat(), user);
        return !dm || application_private_channel_call(dm) || true;
    }
    case A::add_note:
        dismiss_popup();
        return begin_note(user) || true;
    case A::add_friend:
    case A::accept_friend:
    case A::remove_friend:
    case A::cancel_request:
    case A::block:
    case A::unblock: {
        const auto intent = static_cast<A>(action) == A::add_friend      ? ApplicationFriendsUserAction::add
                            : static_cast<A>(action) == A::accept_friend ? ApplicationFriendsUserAction::accept
                            : static_cast<A>(action) == A::remove_friend ? ApplicationFriendsUserAction::remove
                            : static_cast<A>(action) == A::cancel_request ? ApplicationFriendsUserAction::cancel
                            : static_cast<A>(action) == A::block         ? ApplicationFriendsUserAction::block
                                                                         : ApplicationFriendsUserAction::unblock;
        dismiss_popup();
        if (!application_friends_user_action(user, intent))
            log_u64(LogLevel::warning, "user_menu", "relationship_action_refused", static_cast<u64>(intent));
        return true;
    }
    case A::invite_menu:
        return open_invites();
    case A::roles_menu:
        return open_roles();
    case A::apps_menu:
        return open_apps(kCommandTypeUser, user, PopupOwnerKind::member);
    case A::copy_id: {
        dismiss_popup();
        char id[20];
        const u32 bytes = g_app.preferences.developer_mode ? format_id(user, id) : 0;
        return !bytes || clipboard_write_utf8(g_app.window, {id, bytes}) || true;
    }
    case A::report: {
        dismiss_popup();
        ApplicationReportTarget target{};
        target.name = ReportMenuName::user;
        target.user_id = user;
        target.guild_id = s.menu_guild;
        return application_report_open(target) || true;
    }
    default:
        return dismiss_popup();
    }
}

// Public: message menu ---------------------------------------------------------------------

MessageContextMenuExtras application_user_actions_message_extras(const MessageRecord& record) noexcept {
    MessageContextMenuExtras extras{};
    if (!available() || record.delivery != MessageDeliveryState::authoritative)
        return extras;
    const bool own = record.author_id && record.author_id == self_id();
    const auto* rich = message_rich_store_find(g_app.account_ui->message_rich_store, record.id);
    const auto* row = navigation_view_find_channel(g_app.account_ui->navigation_view, record.channel_id);
    const bool manage = row && (row->actions & channel_action_manage_messages);
    extras.remove_embeds = rich && rich->embed_count && !(record.flags & kMessageFlagSuppressEmbeds) &&
                           (own || manage) && g_app.account.api.user_social;
    if (record.channel_id == g_app.account_ui->active_channel_id && g_app.account.api.application_command) {
        ApplicationContextCommand probe[1];
        bool loading = false;
        extras.apps = application_commands_context(kCommandTypeMessage, {probe, 1}, &loading) || loading;
    }
    extras.speak = record.content_bytes != 0;
    extras.report = !own && record.author_id && g_app.account.api.user_social;
    return extras;
}

bool application_user_actions_message_activate(u64 message_id, u32 action) noexcept {
    const auto value = static_cast<MessageContextAction>(action);
    if (value != MessageContextAction::apps_menu && value != MessageContextAction::remove_embeds &&
        value != MessageContextAction::speak_message && value != MessageContextAction::report_message)
        return false;
    if (!available() || !ensure())
        return dismiss_popup();
    const MessageStoreResult found = message_store_find(g_app.message_view.store, message_id);
    const MessageRecord* record = found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
    if (!record)
        return dismiss_popup();
    const auto extras = application_user_actions_message_extras(*record);
    auto& s = *g_state;
    switch (value) {
    case MessageContextAction::apps_menu:
        if (!extras.apps)
            return dismiss_popup();
        s.menu_message = message_id;
        s.menu_user = 0;
        return open_apps(kCommandTypeMessage, message_id, PopupOwnerKind::message);
    case MessageContextAction::remove_embeds:
        dismiss_popup();
        if (!extras.remove_embeds || s.flags_queued || s.work == Work::flags)
            return true;
        s.flags_channel = record->channel_id;
        s.flags_message = record->id;
        s.flags_value = record->flags | kMessageFlagSuppressEmbeds;
        s.flags_queued = true;
        wake();
        return true;
    case MessageContextAction::speak_message: {
        dismiss_popup();
        const auto* user = user_store_find(g_app.account_ui->user_store, record->author_id);
        const Utf8View name = user ? user_store_name(g_app.account_ui->user_store, *user) : Utf8View{};
        const Utf8View content = message_store_content(g_app.message_view.store, found.handle);
        u32 used = 0;
        const auto append = [&](Utf8View text) noexcept {
            u32 take = minimum<u32>(text.bytes, sizeof(s.speak) - used);
            while (take && take < text.bytes && (static_cast<u8>(text.data[take]) & 0xC0) == 0x80)
                --take;
            copy_bytes(s.speak + used, text.data, take);
            used += take;
        };
        if (name.bytes) {
            append(name);
            append(utf8_literal(" said "));
        }
        append(content);
        if (!speech_speak(&s.speech, {s.speak, used}))
            log_write(LogLevel::warning, "user_menu", "speech_unavailable");
        return true;
    }
    default: {
        dismiss_popup();
        if (!extras.report)
            return true;
        ApplicationReportTarget target{};
        target.name = ReportMenuName::message;
        target.channel_id = record->channel_id;
        target.message_id = record->id;
        target.user_id = record->author_id;
        return application_report_open(target) || true;
    }
    }
}

void application_user_actions_commands_changed() noexcept {
    if (!g_state || g_state->follow != FollowUp::apps || !g_app.popup_scene.active)
        return;
    const u64 owner = g_app.popup_scene.owner_id;
    if (g_app.popup_owner_kind == PopupOwnerKind::message && owner == g_state->menu_message)
        open_apps(kCommandTypeMessage, owner, PopupOwnerKind::message);
    else if (g_app.popup_owner_kind == PopupOwnerKind::member && owner == g_state->menu_user)
        open_apps(kCommandTypeUser, owner, PopupOwnerKind::member);
}

// Public: notes ---------------------------------------------------------------------------------

Utf8View application_user_note(u64 user_id, bool* loading) noexcept {
    if (loading)
        *loading = false;
    auto* store = available() && user_id && user_id != self_id() ? notes() : nullptr;
    if (!store || !user_note_request(store, user_id))
        return {};
    const auto* entry = user_note_find(*store, user_id);
    if (loading)
        *loading = entry && (entry->state == UserNoteState::empty || entry->state == UserNoteState::loading);
    if (entry && entry->load_pending)
        wake();
    return user_note_text(*store, user_id);
}

bool application_user_note_edit(u64 user_id, Utf8View text) noexcept {
    auto* store = available() ? notes() : nullptr;
    const bool edited = store && user_note_edit(store, user_id, text, clock_monotonic_milliseconds());
    if (edited)
        wake();
    return edited;
}

void application_user_note_flush(u64 user_id) noexcept {
    if (auto* store = g_state ? g_state->notes : nullptr) {
        user_note_flush(store, user_id, clock_monotonic_milliseconds());
        wake();
    }
}

void application_user_actions_note_gateway(Utf8View data) noexcept {
    auto* store = g_state ? g_state->notes : nullptr;
    if (!store || !available())
        return;
    UserNoteRecord record{};
    if (!user_note_decode(data, {g_state->structurals, 8192}, true, &record))
        return;
    user_note_gateway(store, record.user_id, {record.text, record.bytes});
    if (g_app.profile && g_app.profile->scene.active && g_app.profile->data.user_id == record.user_id)
        application_profile_note_changed();
}

// Public: lane ------------------------------------------------------------------------------------

u64 application_user_actions_next_wake() noexcept {
    if (!g_state || !connected() || g_state->request_id || !lane_free())
        return 0;
    const auto& s = *g_state;
    if (s.invite.phase == InvitePhase::create || s.invite.phase == InvitePhase::dm || s.role_queued || s.flags_queued)
        return s.retry_at ? s.retry_at : 1;
    if (!s.notes)
        return 0;
    const auto next = user_note_next(*s.notes, clock_monotonic_milliseconds());
    if (next.work != UserNoteWork::none)
        return s.retry_at ? s.retry_at : 1;
    return next.due_ms;
}

bool application_user_actions_submit(u64 now) noexcept {
    if (!g_state || !connected() || g_state->request_id || !lane_free() || g_state->retry_at > now)
        return false;
    auto& s = *g_state;
    auto& api = g_app.account.api;
    const auto busy = [&](RestClientSubmitResult result) noexcept {
        if (result.error == RestClientSubmitError::busy) {
            s.retry_at = now + 100;
            return true;
        }
        return false;
    };
    if (s.invite.phase == InvitePhase::create) {
        const u64 id = next_id();
        InviteCreate options{kInviteMaxAgeSeconds, 1, false, true};
        GuildAdminRequest request{};
        request.operation = GuildAdminOperation::invite_create;
        request.guild_id = s.invite.guild_id;
        request.channel_id = s.invite.channel_id;
        request.invite = &options;
        const auto result = api.guild_admin ? api.guild_admin(g_app.account.instance, &request, id)
                                            : RestClientSubmitResult{RestClientSubmitError::invalid_input};
        if (busy(result))
            return false;
        if (!result.ok()) {
            finish_invite(false);
            return false;
        }
        s.invite.phase = InvitePhase::creating;
        return claim(id, Work::invite_create, s.invite.user_id);
    }
    if (s.invite.phase == InvitePhase::dm) {
        const u64 id = next_id();
        const auto result = api.private_conversation(g_app.account.instance, PrivateConversationOperation::open,
                                                     s.invite.user_id, id);
        if (busy(result))
            return false;
        if (!result.ok()) {
            finish_invite(false);
            return false;
        }
        s.invite.phase = InvitePhase::opening;
        return claim(id, Work::invite_dm, s.invite.user_id);
    }
    if (s.role_queued) {
        s.role_queued = false;
        if (!role_editable(chat(), s.role_guild, s.role_id) || !api.profile_get)
            return false;
        char digits[20];
        const u32 bytes = format_id(s.role_id, digits);
        const u64 id = next_id();
        const auto result = api.profile_get(g_app.account.instance, s.role_user, s.role_guild, id,
                                            s.role_add ? 5 : 6, {digits, bytes});
        if (busy(result)) {
            s.role_queued = true;
            return false;
        }
        return result.ok() && claim(id, Work::role, s.role_user);
    }
    if (s.flags_queued) {
        s.flags_queued = false;
        UserSocialRequest request{};
        request.operation = UserSocialOperation::message_flags;
        request.channel_id = s.flags_channel;
        request.message_id = s.flags_message;
        request.flags = s.flags_value;
        const u64 id = next_id();
        const auto result = api.user_social ? api.user_social(g_app.account.instance, &request, id)
                                            : RestClientSubmitResult{RestClientSubmitError::invalid_input};
        if (busy(result)) {
            s.flags_queued = true;
            return false;
        }
        return result.ok() && claim(id, Work::flags, 0);
    }
    if (!s.notes || !api.user_social)
        return false;
    const auto next = user_note_next(*s.notes, clock_monotonic_milliseconds());
    if (next.work == UserNoteWork::none)
        return false;
    UserSocialRequest request{};
    request.user_id = next.user_id;
    Utf8View sent{};
    if (next.work == UserNoteWork::save) {
        if (!user_note_save_begin(s.notes, next.user_id, &sent))
            return false;
        request.operation = UserSocialOperation::note_put;
        request.text = sent;
    } else
        request.operation = UserSocialOperation::note_get;
    const u64 id = next_id();
    const auto result = api.user_social(g_app.account.instance, &request, id);
    if (!result.ok()) {
        if (next.work == UserNoteWork::save)
            user_note_save_finished(s.notes, next.user_id, false, clock_monotonic_milliseconds());
        if (result.error == RestClientSubmitError::busy)
            s.retry_at = now + 100;
        return false;
    }
    if (next.work == UserNoteWork::load)
        user_note_load_begin(s.notes, next.user_id);
    return claim(id, next.work == UserNoteWork::save ? Work::note_save : Work::note_load, next.user_id);
}

bool application_user_actions_owns_response(u64 request_id) noexcept {
    return g_state && request_id && g_state->request_id == request_id;
}

bool application_user_actions_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!application_user_actions_owns_response(response.request_id))
        return true;
    auto& s = *g_state;
    const Work work = s.work;
    const u64 user = s.request_user;
    s.request_id = 0;
    s.work = Work::none;
    const bool ok = response.error == RestClientError::none && response.status_code >= 200 &&
                    response.status_code < 300;
    const u64 now = clock_monotonic_milliseconds();
    if (response.error == RestClientError::none && response.status_code == 429)
        s.retry_at = maximum(now + 1000, response.retry_at_ms);
    switch (work) {
    case Work::invite_create: {
        GuildAdminInvite invite{};
        if (!ok || !guild_admin_decode_invite(body, {s.structurals, 8192}, &invite).ok() || !invite.code_bytes ||
            invite.code_bytes > sizeof(s.invite.code)) {
            finish_invite(false);
            break;
        }
        copy_bytes(s.invite.code, invite.code, invite.code_bytes);
        s.invite.code_bytes = invite.code_bytes;
        const u64 dm = private_conversation_cached(chat(), s.invite.user_id);
        if (dm)
            finish_invite(send_invite_link(dm));
        else
            s.invite.phase = InvitePhase::dm;
        break;
    }
    case Work::invite_dm: {
        const u64 channel = ok ? read_channel_id(body) : 0;
        finish_invite(channel && send_invite_link(channel));
        break;
    }
    case Work::role:
        if (!ok) {
            log_u64(LogLevel::warning, "user_menu", "role_change_status", response.status_code);
            s.role_override_id = 0;
        }
        // GUILD_MEMBER_UPDATE refreshes the member; an open Roles list follows it.
        if (s.follow == FollowUp::roles && g_app.popup_scene.active && g_app.popup_owner_kind == PopupOwnerKind::member &&
            g_app.popup_scene.owner_id == s.menu_user)
            open_roles();
        break;
    case Work::flags:
        if (!ok)
            log_u64(LogLevel::warning, "user_menu", "suppress_embeds_status", response.status_code);
        break;
    case Work::note_load: {
        UserNoteRecord record{};
        if (ok && user_note_decode(body, {s.structurals, 8192}, false, &record) && record.user_id == user)
            user_note_loaded(s.notes, user, true, {record.text, record.bytes});
        else if (response.error == RestClientError::none && response.status_code == 404)
            user_note_loaded(s.notes, user, true, {});
        else
            user_note_loaded(s.notes, user, false, {});
        if (s.note_form_user == user) {
            const auto* entry = user_note_find(*s.notes, user);
            if (entry && entry->state == UserNoteState::loaded && !g_app.text_form)
                open_note_form(user);
            else
                s.note_form_user = 0;
        }
        if (g_app.profile && g_app.profile->scene.active && g_app.profile->data.user_id == user)
            application_profile_note_changed();
        break;
    }
    case Work::note_save:
        user_note_save_finished(s.notes, user, ok, now);
        if (g_app.profile && g_app.profile->scene.active && g_app.profile->data.user_id == user)
            application_profile_note_changed();
        break;
    default:
        break;
    }
    wake();
    return true;
}

void application_user_actions_release() noexcept {
    if (!g_state)
        return;
    application_text_form_cancel_owner(note_form_valid);
    speech_release(&g_state->speech);
    if (g_state->notes)
        VirtualFree(g_state->notes, 0, MEM_RELEASE);
    VirtualFree(g_state, 0, MEM_RELEASE);
    g_state = nullptr;
}
} // namespace maicord::app_detail
