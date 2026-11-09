#include "private_header.hpp"
#include "private_channel.hpp"
#include "../account/account_presentation.hpp"
#include "../account/profile.hpp"
#include "../account/account_ui_runtime.hpp"
#include "../settings/settings.hpp"
#include "../shell/cursor.hpp"
#include "../shell/input.hpp"
#include "../shell/state.hpp"
#include "../shell/window_events.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/media/core/asset.hpp"
#include "maicord/model/guild/member_store.hpp"
#include "maicord/ui/chat/names_card.hpp"
#include "maicord/ui/chat/private_channel_rows.hpp"
#include "maicord/ui/chat/private_channel_title.hpp"
#include "maicord/ui/accessibility/accessibility.hpp"
#include "maicord/ui/core/cursor.hpp"

namespace maicord::app_detail {
namespace {
// The popout stays this long after the pointer leaves the identity and the popout.
constexpr u64 kCloseMs = 250;
struct HeaderUi {
    UiNamesCardScene card;
    u64 user, channel_id, leave_ms;
    // The popout was opened by keyboard focus (it closes when focus leaves, not when the pointer does).
    bool by_focus;
    UiRect anchor;
    // The initials the card's rows borrow while it projects.
    char initials[kUiNamesCardRows][kUiNamesCardIcons][8];
};
// Zero-filled in the PE (.bss), as the stream card: the entrance is reset with its scope before it ticks.
HeaderUi header{.card = {{.entrance = {.style = {0, UiMotionCurve::linear}}}}};

u64 now_ms() noexcept {
    return clock_monotonic_milliseconds();
}
// Sign-in, settings, popups and modal surfaces own the pointer.
bool blocked() noexcept {
    return !g_app.account_ui || g_app.login.visible || g_app.popup_scene.active || g_app.text_form ||
           g_app.image_viewer.scene.active || application_settings_active() ||
           (g_app.renderer.profile_scene && g_app.renderer.profile_scene->active);
}
void close_card() noexcept {
    header.user = header.channel_id = header.leave_ms = 0;
    if (header.card.active) {
        ui_names_card_close(&header.card);
        application_repaint();
    }
    if (g_app.renderer.names_card_scene == &header.card)
        g_app.renderer.names_card_scene = nullptr;
}
bool identity_at(float x, float y, UiPrivateHeaderIdentity* identity) noexcept {
    const auto* rows = application_private_channel_rows();
    const u64 channel = g_app.account_ui->chat_state.selected_channel_id;
    if (!rows || !channel || !ui_private_header_identity(g_app.shell_scene, *rows, channel, identity))
        return false;
    return ui_rect_contains(identity->avatar, x, y) || ui_rect_contains(identity->name, x, y);
}
// The other nicknames of header.user in the servers both of you are in, one row per nickname (the server
// icons it is used in); the popout's rows, skipping the display name itself.
bool project(u64 now) noexcept {
    auto& ui = *g_app.account_ui;
    const auto* user = user_store_find(ui.user_store, header.user);
    const auto* appearance = user_appearance_find(ui.user_store.appearance, header.user);
    if (!user || (appearance && appearance->system))
        return false;
    const Utf8View display = user_store_name(ui.user_store, *user);
    UiNamesCardContent content{};
    content.username = user_store_username(ui.user_store, *user);
    for (u32 g = 0; g < ui.chat_state.guild_count; ++g) {
        const GuildRecord& guild = ui.chat_state.guilds[g];
        const auto* member = guild.occupied ? guild_member_store_find(ui.guild_member_store, guild.id, header.user)
                                            : nullptr;
        const Utf8View nickname = member ? guild_member_store_nickname(*member) : Utf8View{};
        if (!nickname.bytes || utf8_equal(nickname, display))
            continue;
        u32 row = 0;
        while (row < content.row_count && !utf8_equal(content.rows[row].nickname, nickname))
            ++row;
        if (row == content.row_count) {
            if (row == kUiNamesCardRows)
                continue;
            content.rows[content.row_count++].nickname = nickname;
        }
        auto& r = content.rows[row];
        if (r.server_count < kUiNamesCardIcons) {
            const u32 icon = r.server_count;
            const bool picture = guild.icon_variant && chat_state_store_guild_icon_hash(guild).bytes;
            r.icons[icon] = picture ? media_asset_guild_icon(guild.id, guild.icon_variant, false) : MediaAssetKey{};
            // A server without a picture shows its name's first character.
            const Utf8View name = chat_state_store_guild_name(ui.chat_state, guild);
            const auto first = name.bytes ? utf8_decode_one(name.data, name.bytes) : Utf8DecodeResult{};
            r.initials[icon] = {header.initials[row][icon], first.valid ? first.width : 0u};
            if (first.valid)
                copy_bytes(header.initials[row][icon], name.data, first.width);
        }
        ++r.server_count;
    }
    if (!content.row_count)
        return false;
    const u32 revision = header.card.revision;
    if (!ui_names_card_show(&header.card, header.user, content, header.anchor, application_viewport_dips(), now,
                            application_reduced_motion(), &ui.text_measure_cache)) {
        close_card();
        return false;
    }
    g_app.renderer.names_card_scene = &header.card;
    if (revision != header.card.revision)
        application_repaint();
    return true;
}
bool focused_identity() noexcept {
    const auto handle = g_app.shell_scene.semantics[static_cast<u32>(UiSemantic::private_identity)];
    return handle.generation && g_app.context_focus == &g_app.shell_scene.widgets &&
           ui_widget_handle_equal(g_app.shell_scene.widgets.focused, handle);
}
bool current_identity(UiPrivateHeaderIdentity* identity) noexcept {
    const auto* rows = application_private_channel_rows();
    const u64 channel = g_app.account_ui ? g_app.account_ui->chat_state.selected_channel_id : 0;
    return rows && channel && ui_private_header_identity(g_app.shell_scene, *rows, channel, identity);
}
void open_card(const UiPrivateHeaderIdentity& identity, u64 now) noexcept {
    header.user = identity.user;
    header.channel_id = g_app.account_ui->chat_state.selected_channel_id;
    header.anchor = identity.avatar;
    header.leave_ms = 0;
    project(now);
}
} // namespace

void application_private_header_pump() noexcept {
    if (g_app.account_ui) {
        // Focus shows the popout as hover does and releases it with a grace when focus moves on.
        const u64 now = now_ms();
        UiPrivateHeaderIdentity identity{};
        if (focused_identity() && !blocked()) {
            if (!header.by_focus && current_identity(&identity)) {
                header.by_focus = true;
                open_card(identity, now);
            } else if (header.by_focus) {
                header.leave_ms = 0;
            }
        } else if (header.by_focus) {
            header.by_focus = false;
            if (!header.card.active)
                header.user = 0;
            else if (!header.leave_ms)
                header.leave_ms = now;
        }
    }
    if (!header.card.active && !header.user)
        return;
    if (blocked() || g_app.account_ui->chat_state.selected_channel_id != header.channel_id) {
        close_card();
        return;
    }
    const u64 now = now_ms();
    if (header.leave_ms && now >= header.leave_ms + kCloseMs) {
        close_card();
        return;
    }
    if (header.card.active && ui_names_card_tick(&header.card, now, application_reduced_motion()))
        application_repaint();
}

u64 application_private_header_next_wake() noexcept {
    u64 wake = 0;
    if (header.card.active && header.leave_ms)
        wake = earliest_deadline(wake, header.leave_ms + kCloseMs);
    if (header.card.active)
        wake = earliest_deadline(wake, ui_names_card_next_wake(header.card, application_reduced_motion()));
    return wake;
}

bool application_private_header_window_event(const WindowEvent& event) noexcept {
    if (!g_app.account_ui)
        return false;
    if (event.type == EventType::pointer_leave) {
        if (header.card.active && !header.leave_ms && !header.by_focus)
            header.leave_ms = now_ms();
        return false;
    }
    if (input_vertical_wheel(event)) {
        close_card();
        return false;
    }
    const bool move = event.type == EventType::pointer_move, cursor = event.type == EventType::cursor_query;
    const bool press = input_press(event);
    if (!move && !cursor && !press && !input_release(event))
        return false;
    if (blocked()) {
        if (header.card.active)
            close_card();
        return false;
    }
    UiPoint point{};
    if (cursor) {
        if (!client_cursor_dip(event, &point))
            return false;
    } else {
        point = {client_dip_x(event), client_dip_y(event)};
    }
    const bool over_card = ui_names_card_contains(header.card, point.x, point.y);
    UiPrivateHeaderIdentity identity{};
    const bool over = !over_card && identity_at(point.x, point.y, &identity);
    if (cursor) {
        if (over)
            application_set_cursor(ui_action_cursor(true));
        return over;
    }
    if (move) {
        const u64 now = now_ms();
        if (over_card) {
            header.leave_ms = 0;
            return true;
        }
        if (over) {
            header.leave_ms = 0;
            if (header.user != identity.user) {
                header.user = identity.user;
                header.channel_id = g_app.account_ui->chat_state.selected_channel_id;
                header.anchor = identity.avatar;
                project(now);
            }
        } else if (header.card.active && !header.leave_ms && !header.by_focus) {
            header.leave_ms = now;
            application_request_frame();
        } else if (!header.card.active) {
            header.user = 0; // Nothing to show: look again on the next visit.
        }
        return false;
    }
    if (press && over) {
        const u64 user = identity.user;
        const UiRect anchor = identity.avatar;
        close_card();
        return application_profile_view_full(user, anchor);
    }
    return over_card;
}
bool application_private_header_focused() noexcept {
    return g_app.account_ui && focused_identity();
}

bool application_private_header_activate() noexcept {
    UiPrivateHeaderIdentity identity{};
    if (!g_app.account_ui || !current_identity(&identity))
        return false;
    close_card();
    return application_profile_view_full(identity.user, identity.avatar);
}

void application_private_header_popout(bool show) noexcept {
    UiPrivateHeaderIdentity identity{};
    if (!show)
        close_card();
    else if (g_app.account_ui && !blocked() && current_identity(&identity))
        open_card(identity, now_ms());
}

void application_private_header_access(UiAccessWidgetInfo* info) noexcept {
    // "<name>, view profile": the name as the header shows it.
    static char text[560];
    u32 used = 0;
    const auto* rows = application_private_channel_rows();
    const auto* channel = rows && g_app.account_ui
                              ? chat_state_store_find_channel(g_app.account_ui->chat_state,
                                                              g_app.account_ui->chat_state.selected_channel_id)
                              : nullptr;
    if (channel) {
        const Utf8View title = private_channel_title(g_app.account_ui->chat_state, rows->users, *channel,
                                                     {text, 512});
        // A group name or nickname is returned in place: take it into the buffer either way.
        used = utf8_truncated_length(title, 512);
        if (title.data != text)
            copy_bytes(text, title.data, used);
    }
    constexpr auto suffix = utf8_literal(", view profile");
    copy_bytes(text + used, suffix.data, suffix.bytes);
    info->role = UiAccessRole::button;
    info->name = {text, used + suffix.bytes};
    info->help = utf8_literal("Opens the full profile. Focus or hover to see the other names.");
    info->actions = ui_access_action_invoke | ui_access_action_expand;
    info->state |= header.card.active ? ui_access_expanded : ui_access_collapsed;
    info->item = kPrivateHeaderAccessItem;
    info->leaf = true;
}
} // namespace maicord::app_detail
