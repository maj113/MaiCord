#include "application_member_moderation.hpp"
#include "application_account_presentation.hpp"
#include "application_composer.hpp"
#include "application_message_interaction.hpp"
#include "application_state.hpp"
#include "application_text_form.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/protocol/guild_admin.hpp"
#include "maicord/ui/member_context_policy.hpp"

#include <Windows.h>

namespace maicord::app_detail {
namespace {

enum class Kind : u8 { none, nickname, timeout, end_timeout, kick, ban };
enum class Step : u8 { idle, choosing, editing, queued, sending, failed, done };

constexpr MemberModerationMenuActions kActions{
    kMemberModerationActionBase + 1, kMemberModerationActionBase + 2, kMemberModerationActionBase + 3,
    kMemberModerationActionBase + 4, kMemberModerationActionBase + 5, kMemberModerationActionBase + 0x100};
constexpr u32 kChoiceCount = sizeof(kModerationBanDeleteChoices) / sizeof(u32);
constexpr u32 kNicknameCodepoints = kGuildAdminNicknameCodepoints;

struct Actor {
    u64 scope, profile, self;
    const void* instance;
};

struct Moderation {
    Actor actor;
    u64 guild_id, user_id, serial, request_id, retry_at;
    // Menu target captured when the member context menu was built.
    u64 menu_guild_id, menu_user_id;
    u32 choice, revision, text_bytes;
    u16 http_status;
    Kind kind;
    Step step;
    bool self_nickname;
    Utf8View status;
    char text[kGuildAdminReasonBytes];
};

Moderation moderation{};
u64 moderation_serial{};

Actor current_actor() noexcept {
    return {g_app.plugin_account_scope_serial, g_app.account.status.profile_id,
            g_app.account_ui ? g_app.account_ui->chat_state.self_user_id : 0, g_app.account.instance};
}

bool actor_current(const Actor& actor) noexcept {
    const Actor now = current_actor();
    return g_app.account_ui && !g_app.login.visible && actor.self && actor.scope == now.scope &&
           actor.profile == now.profile && actor.self == now.self && actor.instance == now.instance;
}

bool connected() noexcept {
    return g_app.account.instance && g_app.account.status.running && g_app.account.status.gateway_ready &&
           !g_app.account.stop_failed && g_app.account.status.required_action == AccountRequirement::none &&
           g_app.account.api.guild_admin;
}

u64 now_seconds() noexcept {
    return clock_unix_milliseconds() / 1000;
}

ModerationCapabilities capabilities(u64 guild_id, u64 user_id) noexcept {
    if (!g_app.account_ui)
        return {};
    return moderation_capabilities(g_app.account_ui->chat_state, g_app.account_ui->guild_member_store, guild_id,
                                   user_id, now_seconds());
}

u8 kind_bit(Kind kind) noexcept {
    switch (kind) {
    case Kind::nickname: return moderation_action_nickname;
    case Kind::timeout: return moderation_action_timeout;
    case Kind::end_timeout: return moderation_action_end_timeout;
    case Kind::kick: return moderation_action_kick;
    case Kind::ban: return moderation_action_ban;
    default: return 0;
    }
}

bool still_allowed() noexcept {
    return actor_current(moderation.actor) &&
           (capabilities(moderation.guild_id, moderation.user_id).allowed & kind_bit(moderation.kind)) != 0;
}

void reset() noexcept {
    SecureZeroMemory(moderation.text, sizeof(moderation.text));
    const u64 menu_guild = moderation.menu_guild_id, menu_user = moderation.menu_user_id;
    moderation = {};
    moderation.menu_guild_id = menu_guild;
    moderation.menu_user_id = menu_user;
}

void changed() noexcept {
    ++moderation.revision;
    application_request_frame();
}

Utf8View target_name(u64 guild_id, u64 user_id) noexcept {
    const auto& ui = *g_app.account_ui;
    if (const auto* member = guild_member_store_find(ui.guild_member_store, guild_id, user_id)) {
        const auto nickname = guild_member_store_nickname(*member);
        if (nickname.bytes)
            return nickname;
    }
    if (const auto* user = user_store_find(ui.user_store, user_id)) {
        const auto name = user_store_name(ui.user_store, *user);
        if (name.bytes)
            return name;
    }
    return utf8_literal("this member");
}

struct Text {
    char data[512];
    u32 bytes;
    void add(Utf8View value) noexcept {
        const u32 room = sizeof(data) - bytes;
        const u32 take = value.bytes < room ? value.bytes : room;
        copy_bytes(data + bytes, value.data, take);
        bytes += take;
    }
    Utf8View view() const noexcept {
        return {data, bytes};
    }
};

Utf8View duration_text(u32 seconds) noexcept {
    switch (seconds) {
    case 60: return utf8_literal("60 seconds");
    case 300: return utf8_literal("5 minutes");
    case 600: return utf8_literal("10 minutes");
    case 3'600: return utf8_literal("1 hour");
    case 86'400: return utf8_literal("1 day");
    default: return utf8_literal("1 week");
    }
}

Utf8View ban_window_text(u32 seconds) noexcept {
    switch (seconds) {
    case 0: return utf8_literal("Their messages stay.");
    case 3'600: return utf8_literal("Messages from the last hour will be deleted.");
    case 21'600: return utf8_literal("Messages from the last 6 hours will be deleted.");
    case 43'200: return utf8_literal("Messages from the last 12 hours will be deleted.");
    case 86'400: return utf8_literal("Messages from the last day will be deleted.");
    case 259'200: return utf8_literal("Messages from the last 3 days will be deleted.");
    default: return utf8_literal("Messages from the last week will be deleted.");
    }
}

Utf8View failure_text(const RestClientResponse& response) noexcept {
    if (response.error != RestClientError::none)
        return utf8_literal("Couldn't reach the server. Try again.");
    switch (response.status_code) {
    case 403: return utf8_literal("You no longer have permission to do this.");
    case 404: return utf8_literal("This member isn't in the server anymore.");
    case 429: return utf8_literal("Too many requests. Wait a moment and try again.");
    case 400: return utf8_literal("The server rejected this change.");
    default: return utf8_literal("Something went wrong. Try again.");
    }
}

TextFormOwnerKey form_owner() noexcept {
    return {moderation.serial, moderation.guild_id, moderation.user_id};
}

bool owner_current(TextFormOwnerKey key) noexcept {
    return moderation.serial && key.scope == moderation.serial && key.collection == moderation.guild_id &&
           key.item == moderation.user_id && moderation.step != Step::idle && actor_current(moderation.actor) &&
           chat_state_store_find_guild(g_app.account_ui->chat_state, moderation.guild_id);
}

bool form_save(TextFormOwnerKey key, Utf8View text) noexcept {
    if (!owner_current(key) || (moderation.step != Step::editing && moderation.step != Step::failed))
        return false;
    const auto validation = utf8_validate(text);
    const bool nickname = moderation.kind == Kind::nickname;
    if (!validation.valid || text.bytes > (nickname ? kNicknameCodepoints * 4 : kGuildAdminReasonBytes) ||
        (nickname && validation.codepoints > kNicknameCodepoints)) {
        moderation.status = utf8_literal("This text is too long.");
        moderation.step = Step::failed;
        changed();
        return false;
    }
    for (u32 i = 0; i < text.bytes; ++i)
        if (static_cast<u8>(text.data[i]) < 0x20) {
            moderation.status = utf8_literal("Use a single line without control characters.");
            moderation.step = Step::failed;
            changed();
            return false;
        }
    if (!still_allowed()) {
        moderation.status = utf8_literal("You can't do this to this member anymore.");
        moderation.step = Step::failed;
        changed();
        return false;
    }
    SecureZeroMemory(moderation.text, sizeof(moderation.text));
    copy_bytes(moderation.text, text.data, text.bytes);
    moderation.text_bytes = text.bytes;
    moderation.step = Step::queued;
    moderation.retry_at = 0;
    moderation.status = {};
    changed();
    return false;
}

ApplicationTextFormProgress form_progress(TextFormOwnerKey key) noexcept {
    ApplicationTextFormProgress view{};
    view.revision = moderation.revision;
    if (!owner_current(key) || moderation.step == Step::done) {
        view.complete = true;
        return view;
    }
    if (moderation.step == Step::queued || moderation.step == Step::sending) {
        view.status = utf8_literal("Sending...");
        view.editable = view.can_accept = view.can_cancel = view.dismissible = false;
        return view;
    }
    view.status = moderation.status;
    if (moderation.step == Step::failed && moderation.http_status)
        view.accept = utf8_literal("Try again");
    return view;
}

void form_closed(TextFormOwnerKey key) noexcept {
    if (key.scope == moderation.serial && moderation.step != Step::sending)
        reset();
}

bool open_form() noexcept {
    const auto name = target_name(moderation.guild_id, moderation.user_id);
    Text title{}, context{};
    Utf8View label = utf8_literal("Reason (optional)");
    Utf8View accept{};
    Utf8View initial{};
    u32 maximum = kGuildAdminReasonBytes;
    switch (moderation.kind) {
    case Kind::nickname: {
        title.add(utf8_literal("Edit nickname"));
        context.add(moderation.self_nickname ? utf8_literal("Shown for you in this server.")
                                             : utf8_literal("Shown for "));
        if (!moderation.self_nickname) {
            context.add(name);
            context.add(utf8_literal(" in this server."));
        }
        context.add(utf8_literal(" Leave it empty to reset."));
        label = utf8_literal("Nickname");
        accept = utf8_literal("Save");
        maximum = kNicknameCodepoints;
        if (const auto* member =
                guild_member_store_find(g_app.account_ui->guild_member_store, moderation.guild_id, moderation.user_id))
            initial = guild_member_store_nickname(*member);
        break;
    }
    case Kind::timeout:
        title.add(utf8_literal("Time out "));
        title.add(name);
        context.add(utf8_literal("For "));
        context.add(duration_text(moderation.choice));
        context.add(utf8_literal(". They can read but can't send messages, react or join voice."));
        accept = utf8_literal("Time out");
        break;
    case Kind::end_timeout:
        title.add(utf8_literal("End timeout for "));
        title.add(name);
        context.add(utf8_literal("They can take part again right away."));
        accept = utf8_literal("End timeout");
        break;
    case Kind::kick:
        title.add(utf8_literal("Remove "));
        title.add(name);
        context.add(utf8_literal("They can rejoin with a new invite."));
        accept = utf8_literal("Remove");
        break;
    case Kind::ban:
        title.add(utf8_literal("Ban "));
        title.add(name);
        context.add(utf8_literal("They can't rejoin unless the ban is lifted. "));
        context.add(ban_window_text(moderation.choice));
        accept = utf8_literal("Ban");
        break;
    default:
        return false;
    }
    moderation.step = Step::editing;
    ApplicationTextFormRequest request{};
    request.title = title.view();
    request.context = context.view();
    request.label = label;
    request.accept = accept;
    request.initial = initial;
    request.owner = form_owner();
    request.valid = owner_current;
    request.save = form_save;
    request.maximum_codepoints = maximum;
    request.maximum_bytes = moderation.kind == Kind::nickname ? kNicknameCodepoints * 4 : kGuildAdminReasonBytes;
    request.single_line = true;
    request.progress = form_progress;
    request.closed = form_closed;
    request.danger = moderation.kind == Kind::timeout || moderation.kind == Kind::kick || moderation.kind == Kind::ban;
    // Removing a timeout records no reason, so it asks for none.
    request.confirmation_only = moderation.kind == Kind::end_timeout;
    if (application_text_form_open(request))
        return true;
    reset();
    return false;
}

bool begin(Kind kind, u64 guild_id, u64 user_id) noexcept {
    if (moderation.step == Step::queued || moderation.step == Step::sending || g_app.text_form)
        return false;
    const auto allowed = capabilities(guild_id, user_id);
    if (!(allowed.allowed & kind_bit(kind)))
        return false;
    reset();
    moderation.actor = current_actor();
    moderation.guild_id = guild_id;
    moderation.user_id = user_id;
    moderation.kind = kind;
    moderation.self_nickname = kind == Kind::nickname && allowed.nickname_self;
    if (++moderation_serial == 0)
        ++moderation_serial;
    moderation.serial = moderation_serial;
    return true;
}

bool open_choices(Kind kind) noexcept {
    PopupItem items[kChoiceCount]{};
    const u32 count = kind == Kind::timeout ? member_context_timeout_choices(kActions, {items, kChoiceCount})
                                            : member_context_ban_choices(kActions, {items, kChoiceCount});
    const UiRect anchor = g_app.popup_scene.anchor;
    if (!count || !dismiss_popup())
        return false;
    if (!popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, moderation.user_id, {items, count}, anchor,
                          application_viewport_dips(), 220.0f)
             .ok()) {
        reset();
        return false;
    }
    moderation.step = Step::choosing;
    set_popup_owner(PopupOwnerKind::member);
    return refresh_popup_text();
}

GuildAdminRequest build_request(MemberPatch* patch, u64 now_ms) noexcept {
    GuildAdminRequest request{};
    request.guild_id = moderation.guild_id;
    request.target_id = moderation.user_id;
    // As the official dialogs send it: kick, ban and timeout always carry the
    // reason box, empty when untouched ("" sends an empty header or
    // "?reason="); nicknames and ending a timeout send none (null).
    request.reason = moderation.kind == Kind::nickname || moderation.kind == Kind::end_timeout
                         ? Utf8View{}
                         : Utf8View{moderation.text, moderation.text_bytes};
    switch (moderation.kind) {
    case Kind::nickname:
        request.operation = moderation.self_nickname ? GuildAdminOperation::member_update_self
                                                     : GuildAdminOperation::member_update;
        if (moderation.self_nickname)
            request.target_id = 0;
        patch->fields = member_patch_nickname;
        patch->nickname = {moderation.text, moderation.text_bytes};
        request.member = patch;
        break;
    case Kind::timeout:
    case Kind::end_timeout:
        request.operation = GuildAdminOperation::member_update;
        patch->fields = member_patch_timeout;
        // Wall-clock milliseconds, as the official client's toISOString() carries.
        patch->communication_disabled_until_ms =
            moderation.kind == Kind::timeout ? now_ms + u64{moderation.choice} * 1000 : 0;
        request.member = patch;
        break;
    case Kind::kick:
        request.operation = GuildAdminOperation::member_kick;
        break;
    case Kind::ban:
        request.operation = GuildAdminOperation::ban_add;
        request.delete_message_seconds = moderation.choice;
        break;
    default:
        break;
    }
    return request;
}

} // namespace

bool application_member_moderation_action(u32 action) noexcept {
    return action > kMemberModerationActionBase && action < kActions.choice_first + kChoiceCount;
}

u32 application_member_moderation_menu_items(u64 guild_id, u64 user_id, Slice<PopupItem> output) noexcept {
    if (!g_app.account_ui || !guild_id || !user_id)
        return 0;
    moderation.menu_guild_id = guild_id;
    moderation.menu_user_id = user_id;
    return member_context_moderation_build(capabilities(guild_id, user_id), kActions, output);
}

bool application_member_moderation_activate(u32 action) noexcept {
    if (!g_app.account_ui || !g_app.popup_scene.active || g_app.popup_owner_kind != PopupOwnerKind::member)
        return dismiss_popup();
    const u64 user = g_app.popup_scene.owner_id;
    if (action >= kActions.choice_first) {
        const u32 index = action - kActions.choice_first;
        const bool timeout = moderation.kind == Kind::timeout;
        const u32 limit = timeout ? sizeof(kModerationTimeoutChoices) / sizeof(u32) : kChoiceCount;
        if (moderation.step != Step::choosing || user != moderation.user_id || index >= limit ||
            (moderation.kind != Kind::timeout && moderation.kind != Kind::ban)) {
            reset();
            return dismiss_popup();
        }
        moderation.choice = timeout ? kModerationTimeoutChoices[index] : kModerationBanDeleteChoices[index];
        if (!dismiss_popup())
            return false;
        if (still_allowed())
            open_form();
        else
            reset();
        return true;
    }
    const u64 guild = moderation.menu_guild_id;
    if (user != moderation.menu_user_id || !guild)
        return dismiss_popup();
    const Kind kind = action == kActions.nickname      ? Kind::nickname
                      : action == kActions.timeout     ? Kind::timeout
                      : action == kActions.end_timeout ? Kind::end_timeout
                      : action == kActions.kick        ? Kind::kick
                      : action == kActions.ban         ? Kind::ban
                                                       : Kind::none;
    if (kind == Kind::none || !begin(kind, guild, user))
        return dismiss_popup();
    if (kind == Kind::timeout || kind == Kind::ban)
        return open_choices(kind);
    if (!dismiss_popup())
        return false;
    open_form();
    return true;
}

bool application_member_moderation_start(u64 guild_id, u64 user_id, u8 action, u32 choice) noexcept {
    const Kind kind = action == moderation_action_nickname      ? Kind::nickname
                      : action == moderation_action_timeout     ? Kind::timeout
                      : action == moderation_action_end_timeout ? Kind::end_timeout
                      : action == moderation_action_kick        ? Kind::kick
                      : action == moderation_action_ban         ? Kind::ban
                                                                : Kind::none;
    bool listed = kind != Kind::timeout && kind != Kind::ban;
    for (const u32 value : kModerationTimeoutChoices)
        listed = listed || (kind == Kind::timeout && value == choice);
    for (const u32 value : kModerationBanDeleteChoices)
        listed = listed || (kind == Kind::ban && value == choice);
    if (kind == Kind::none || !listed || !begin(kind, guild_id, user_id))
        return false;
    moderation.choice = choice;
    return open_form();
}

bool application_member_moderation_pending(u64 now_ms) noexcept {
    return moderation.step == Step::queued && moderation.retry_at <= now_ms && connected() &&
           actor_current(moderation.actor);
}

bool application_member_moderation_submit(u64 request_id, u64 now_ms) noexcept {
    if (!request_id || !application_member_moderation_pending(now_ms))
        return false;
    if (!still_allowed()) {
        moderation.step = Step::failed;
        moderation.status = utf8_literal("You can't do this to this member anymore.");
        changed();
        return false;
    }
    MemberPatch patch{};
    const GuildAdminRequest request = build_request(&patch, clock_unix_milliseconds());
    const auto submitted = g_app.account.api.guild_admin(g_app.account.instance, &request, request_id);
    if (submitted.error == RestClientSubmitError::busy) {
        moderation.retry_at = now_ms + 100;
        return false;
    }
    if (!submitted.ok()) {
        moderation.step = Step::failed;
        moderation.status = utf8_literal("This change couldn't be sent.");
        changed();
        return false;
    }
    moderation.step = Step::sending;
    moderation.request_id = request_id;
    changed();
    return true;
}

bool application_member_moderation_response(const RestClientResponse& response, Utf8View) noexcept {
    if (moderation.step != Step::sending || response.request_id != moderation.request_id)
        return true;
    moderation.request_id = 0;
    moderation.http_status = response.status_code;
    if (response.error == RestClientError::none && response.status_code >= 200 &&
        response.status_code < 300) {
        moderation.step = Step::done;
        SecureZeroMemory(moderation.text, sizeof(moderation.text));
    } else {
        moderation.step = Step::failed;
        moderation.status = failure_text(response);
        if (!moderation.http_status)
            moderation.http_status = 1;
    }
    changed();
    // Closes the form on success; failures stay editable with the reason.
    application_text_form_synchronize();
    return true;
}

u64 application_member_moderation_next_wake() noexcept {
    return moderation.step == Step::queued ? (moderation.retry_at ? moderation.retry_at : 1) : 0;
}

void application_member_moderation_release() noexcept {
    if (moderation.serial)
        application_text_form_cancel_owner(owner_current);
    reset();
    moderation.menu_guild_id = moderation.menu_user_id = 0;
}

} // namespace maicord::app_detail
