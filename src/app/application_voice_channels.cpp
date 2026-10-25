#include "application_voice_channels.hpp"
#include "application_guild_lifecycle.hpp"
#include "application_stage.hpp"
#include "application_state.hpp"
#include "application_text_form.hpp"
#include "application_voice.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/time.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/model/voice_channel_info_store.hpp"
#include "maicord/protocol/stage_channel.hpp"
#include "maicord/protocol/voice_channel_info.hpp"
#include "maicord/ui/shell_navigation_scene.hpp"

#include "maicord/core/memory.hpp"
#include "maicord/render/renderer_settings_visibility.hpp"
#include "account_ui_runtime.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {
namespace {
struct StatusForm {
    u64 serial, channel_id, guild_id, account_scope, revision;
    Utf8View status;
    bool sending, done;
};
struct VoiceChannels {
    VoiceChannelInfoStore info;
    UiVoiceChannelExtras extras;
    UiVoiceTimerClock clock;
    UiVoiceInviteCountdown invite;
    StatusForm form;
    u64 form_serial;
    // The selected guild last asked for its values and in which session; a
    // guild without voice channels, skipped until its channels change.
    u64 asked_guild, skipped_guild;
    u32 session, asked_session, skipped_session, skipped_mutations, info_revision;
    char text[kVoiceChannelStatusMaximumBytes];
};
VoiceChannels vc{};

enum class Target : u8 {
    none,
    status,
    invite,
    invite_close
};
struct Hit {
    Target target;
    u64 channel_id;
};

ChatStateStore* chat() noexcept {
    return g_app.account_ui ? &g_app.account_ui->chat_state : nullptr;
}
const ChannelRecord* voice_channel(u64 channel_id) noexcept {
    const auto* state = chat();
    const auto* channel = state && channel_id ? chat_state_store_find_channel(*state, channel_id) : nullptr;
    return channel && channel->type == 2 && channel->guild_id ? channel : nullptr;
}
bool can(const ChannelRecord& channel, PermissionSet bits) noexcept {
    return channel.permissions_resolved && (channel.effective_permissions & bits) == bits;
}
void bump() noexcept {
    ++vc.extras.revision;
    application_repaint();
}
void settle_info() noexcept {
    if (vc.info.revision != vc.info_revision) {
        vc.info_revision = vc.info.revision;
        bump();
    }
}

struct Apply {
    VoiceChannelInfoStore* store;
};
bool apply_entry(void* opaque, const VoiceChannelInfoEntry& entry) noexcept {
    auto* store = static_cast<Apply*>(opaque)->store;
    if (entry.has_start)
        voice_channel_info_store_set_start(store, entry.guild_id, entry.channel_id, entry.start_s);
    if (entry.has_status)
        voice_channel_info_store_set_status(store, entry.guild_id, entry.channel_id, entry.status);
    return true;
}
void decode(VoiceChannelInfoEvent kind, ConstSlice<u8> payload) noexcept {
    if (!payload.data || !payload.count || !g_app.account_ui)
        return;
    const Utf8View data{reinterpret_cast<const char*>(payload.data), payload.count};
    const Slice<u32> structurals{g_app.account_ui->message_history_page_structurals,
                                 kMessageHistoryPageStructuralCount};
    // The answer replaces what the guild had, as in the official client.
    if (kind == VoiceChannelInfoEvent::channel_info) {
        const auto probe = voice_channel_info_decode(kind, data, structurals, {vc.text, sizeof(vc.text)},
                                                     nullptr, nullptr);
        if (!probe.ok()) {
            log_u64(LogLevel::warning, "voice", "channel_info_rejected", static_cast<u64>(probe.error));
            return;
        }
        voice_channel_info_store_clear_guild(&vc.info, probe.guild_id);
    }
    Apply apply{&vc.info};
    const auto result =
        voice_channel_info_decode(kind, data, structurals, {vc.text, sizeof(vc.text)}, apply_entry, &apply);
    if (!result.ok())
        log_u64(LogLevel::warning, "voice", "channel_info_update_rejected", static_cast<u64>(result.error));
}

// Asks for the selected guild's statuses and start times once per session;
// the worker drops repeats within a session.
void request_selected_guild() noexcept {
    const auto* state = chat();
    const u64 guild = state && !state->home_selected ? state->selected_guild_id : 0;
    if (!guild || (guild == vc.asked_guild && vc.asked_session == vc.session))
        return;
    // A guild without voice channels is looked at again after channel changes.
    if (guild == vc.skipped_guild && vc.skipped_session == vc.session &&
        vc.skipped_mutations == state->channel_mutations)
        return;
    auto& host = g_app.account;
    const auto* record = chat_state_store_find_guild(*state, guild);
    if (!record || record->unavailable || !host.instance || !host.api.channel_info)
        return;
    bool voice = false;
    for (u32 i = 0; i < state->channel_count && !voice; ++i) {
        const auto& channel = state->channels[state->channel_order[i]];
        voice = channel.guild_id == guild && channel.type == 2;
    }
    if (!voice) {
        vc.skipped_guild = guild;
        vc.skipped_session = vc.session;
        vc.skipped_mutations = state->channel_mutations;
        return;
    }
    host.api.channel_info(host.instance, guild);
    vc.asked_guild = guild;
    vc.asked_session = vc.session;
}

void follow_connection() noexcept {
    const auto view = application_voice_view();
    const auto* joined = view.guild_id ? voice_channel(view.channel_id) : nullptr;
    const bool connected = joined && view.state == UiVoicePanelState::connected;
    ui_voice_invite_observe(&vc.invite, connected ? joined->id : 0);
    const u64 status = joined && can(*joined, kPermissionSetVoiceChannelStatus) ? joined->id : 0;
    const u64 invite =
        connected && can(*joined, kPermissionCreateInvite) ? ui_voice_invite_channel(vc.invite) : 0;
    if (status == vc.extras.status_channel_id && invite == vc.extras.invite_channel_id)
        return;
    vc.extras.status_channel_id = status;
    vc.extras.invite_channel_id = invite;
    bump();
}

UiRect head_rectangle(UiRect row) noexcept {
    return {row.left, row.top, row.right, row.top + kShellNavigationVoiceHeadHeight};
}
bool on_screen(UiRect rectangle) noexcept {
    return ui_rect_has_area(ui_rect_intersection(rectangle, g_app.shell_navigation_scene.viewport));
}
bool row_timer(const ChannelViewRow& row) noexcept {
    const auto* info =
        row.type == 2 && !row.mention_count ? voice_channel_info_store_find(vc.info, row.id) : nullptr;
    return info && info->start_s;
}

Hit hit(float x, float y) noexcept {
    const auto& scene = g_app.shell_navigation_scene;
    if (!scene.active || !g_app.account_ui || !ui_rect_contains(scene.viewport, x, y))
        return {};
    for (u32 i = 0; i < scene.count; ++i) {
        const auto& row = scene.rows[i].view;
        const bool status = row.voice_subtitle && row.id == vc.extras.status_channel_id;
        if (!status && !row.voice_invite)
            continue;
        const auto* layout = shell_navigation_scene_layout_node(scene, scene.rows[i]);
        if (!layout)
            continue;
        if (status && ui_rect_contains(ui_voice_status_rectangle(row, layout->rectangle), x, y))
            return {Target::status, row.id};
        UiVoiceInviteRow invite{};
        if (ui_voice_invite_row(row, layout->rectangle, &invite) && ui_rect_contains(invite.row, x, y))
            return {ui_rect_contains(invite.close, x, y) ? Target::invite_close : Target::invite, row.id};
    }
    return {};
}

// Status editor ---------------------------------------------------------------
bool form_valid(TextFormOwnerKey key) noexcept {
    const auto& f = vc.form;
    return f.serial && key.scope == f.serial && key.collection == f.channel_id &&
           voice_channel(f.channel_id) && f.account_scope == g_app.plugin_account_scope_serial &&
           !g_app.login.visible && vc.extras.status_channel_id == f.channel_id;
}
Utf8View current_status(u64 channel_id) noexcept {
    const auto* info = voice_channel_info_store_find(vc.info, channel_id);
    return info ? voice_channel_info_status(vc.info, *info) : Utf8View{};
}
void form_notice(Utf8View text) noexcept {
    vc.form.status = text;
    ++vc.form.revision;
    application_repaint();
}
bool form_save(TextFormOwnerKey key, Utf8View text) noexcept {
    auto& f = vc.form;
    if (!form_valid(key) || f.sending)
        return false;
    if (!stage_voice_status_valid(text)) {
        form_notice(utf8_literal("A status is one line of up to 500 characters."));
        return false;
    }
    if (utf8_equal(text, current_status(f.channel_id)))
        return true;
    if (!application_stage_queue_voice_status(f.guild_id, f.channel_id, text)) {
        form_notice(utf8_literal("Couldn't set the status. Try again."));
        return false;
    }
    f.sending = true;
    form_notice({});
    return false; // Closes once the PUT succeeds.
}
ApplicationTextFormProgress form_progress(TextFormOwnerKey key) noexcept {
    ApplicationTextFormProgress view{};
    view.revision = vc.form.revision;
    view.complete = vc.form.done || !form_valid(key);
    view.status = vc.form.status;
    view.editable = view.can_accept = !vc.form.sending;
    if (vc.form.sending)
        view.accept = utf8_literal("Saving");
    return view;
}
void form_closed(TextFormOwnerKey key) noexcept {
    if (key.scope == vc.form.serial)
        reset_value(&vc.form);
}
bool open_status_form(u64 channel_id) noexcept {
    const auto* channel = voice_channel(channel_id);
    if (!channel || g_app.text_form)
        return false;
    reset_value(&vc.form);
    vc.form.serial = ++vc.form_serial ? vc.form_serial : ++vc.form_serial;
    vc.form.channel_id = channel->id;
    vc.form.guild_id = channel->guild_id;
    vc.form.account_scope = g_app.plugin_account_scope_serial;
    ApplicationTextFormRequest request{};
    request.title = utf8_literal("Channel status");
    request.context = utf8_literal("Everyone sees it under the channel name.");
    request.label = utf8_literal("Status");
    request.accept = utf8_literal("Set status");
    request.placeholder = utf8_literal("What's happening here?");
    request.initial = current_status(channel->id);
    request.owner = {vc.form.serial, channel->id, 1};
    request.valid = form_valid;
    request.save = form_save;
    request.progress = form_progress;
    request.closed = form_closed;
    request.maximum_codepoints = kStageVoiceStatusMaximumCodepoints;
    request.maximum_bytes = kStageVoiceStatusMaximumBytes;
    request.single_line = true;
    if (application_text_form_open(request))
        return true;
    reset_value(&vc.form);
    return false;
}
} // namespace

void application_voice_channels_gateway(const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept {
    vc.extras.info = &vc.info;
    if (event.kind == GatewayClientEventKind::ready || event.kind == GatewayClientEventKind::resumed) {
        ++vc.session; // The worker may ask every guild again.
        return;
    }
    if (event.kind != GatewayClientEventKind::dispatch)
        return;
    switch (event.dispatch_kind) {
    case GatewayClientDispatchKind::guild_create:
        if (event.state.id == vc.asked_guild)
            vc.asked_guild = 0;
        break;
    case GatewayClientDispatchKind::guild_delete:
        voice_channel_info_store_clear_guild(&vc.info, event.state.id);
        if (event.state.id == vc.asked_guild)
            vc.asked_guild = 0;
        break;
    case GatewayClientDispatchKind::channel_delete:
        voice_channel_info_store_remove(&vc.info, event.state.id);
        break;
    case GatewayClientDispatchKind::channel_info:
        decode(VoiceChannelInfoEvent::channel_info, payload);
        break;
    case GatewayClientDispatchKind::voice_channel_start_time_update:
        decode(VoiceChannelInfoEvent::start_time_update, payload);
        break;
    case GatewayClientDispatchKind::voice_channel_status_update:
        decode(VoiceChannelInfoEvent::status_update, payload);
        break;
    default:
        return;
    }
    settle_info();
}

void application_voice_channels_pump() noexcept {
    vc.extras.info = &vc.info;
    if (!g_app.account_ui)
        return;
    request_selected_guild();
    follow_connection();
    settle_info();
}

const UiVoiceChannelExtras& application_voice_channels_extras() noexcept {
    vc.extras.info = &vc.info;
    return vc.extras;
}

bool application_voice_channels_click(float x, float y) noexcept {
    const auto target = hit(x, y);
    switch (target.target) {
    case Target::status:
        return open_status_form(target.channel_id) || true;
    case Target::invite_close:
        if (ui_voice_invite_dismiss(&vc.invite)) {
            vc.extras.invite_channel_id = 0;
            vc.extras.invite_hovered = vc.extras.invite_close_hovered = false;
            bump();
        }
        return true;
    case Target::invite:
        if (const auto* channel = voice_channel(target.channel_id))
            application_guild_lifecycle_open_invite(channel->guild_id, channel->id);
        return true;
    default:
        return false;
    }
}

bool application_voice_channels_hover(float x, float y) noexcept {
    const auto target = hit(x, y);
    const bool status = target.target == Target::status;
    const bool invite = target.target == Target::invite || target.target == Target::invite_close;
    const bool close = target.target == Target::invite_close;
    auto& e = vc.extras;
    if (status != e.status_hovered || invite != e.invite_hovered || close != e.invite_close_hovered) {
        e.status_hovered = status;
        e.invite_hovered = invite;
        e.invite_close_hovered = close;
        application_repaint();
    }
    return target.target != Target::none;
}

bool application_voice_channels_hand_cursor(float x, float y) noexcept {
    return hit(x, y).target != Target::none;
}

void application_voice_channels_status_result(u64 channel_id, bool ok, u32 status_code) noexcept {
    auto& f = vc.form;
    if (!f.serial || !f.sending || f.channel_id != channel_id)
        return;
    f.sending = false;
    if (ok) {
        f.done = true;
        form_notice({});
        return;
    }
    form_notice(status_code == 403   ? utf8_literal("You don't have permission to do this.")
                : status_code == 429 ? utf8_literal("Slow down. Try again in a moment.")
                                     : utf8_literal("Couldn't set the status. Try again."));
}

u64 application_voice_timer_next_wake() noexcept {
    const u64 timer = ui_voice_timer_next_wake(vc.clock), invite = ui_voice_invite_next_wake(vc.invite);
    return !timer ? invite : !invite ? timer : minimum(timer, invite);
}

void application_voice_timer_tick(u64 now_ms) noexcept {
    vc.extras.info = &vc.info;
    // Runs every loop turn: without start times or an invite row nothing can show.
    if (!vc.info.count && !ui_voice_invite_channel(vc.invite)) {
        vc.clock.visible = false;
        vc.clock.wake_ms = 0;
        return;
    }
    const bool shown = g_app.window && !IsIconic(g_app.window) && IsWindowVisible(g_app.window) &&
                       !renderer_settings_hides_shell(g_app.renderer);
    const auto& scene = g_app.shell_navigation_scene;
    bool timer = false, invite = false;
    for (u32 i = 0; shown && scene.active && g_app.account_ui && i < scene.count; ++i) {
        const auto& row = scene.rows[i].view;
        const bool has_timer = row_timer(row);
        if (!has_timer && !row.voice_invite)
            continue;
        const auto* layout = shell_navigation_scene_layout_node(scene, scene.rows[i]);
        if (!layout)
            continue;
        timer = timer || (has_timer && on_screen(head_rectangle(layout->rectangle)));
        UiVoiceInviteRow rows{};
        invite = invite || (ui_voice_invite_row(row, layout->rectangle, &rows) && on_screen(rows.row));
    }
    const u64 unix_ms = clock_unix_milliseconds();
    const bool focused = GetForegroundWindow() == g_app.window && !g_app.preferences.reduced_motion;
    if (ui_voice_timer_tick(&vc.clock, now_ms, unix_ms, timer, focused)) {
        vc.extras.clock_s = vc.clock.shown_s;
        application_repaint();
    } else if (!timer) {
        // Off screen the label follows the clock without wakes, so a timer
        // that comes into view draws the current time in its first frame.
        vc.extras.clock_s = unix_ms / 1'000;
    }
    // Reduced motion keeps the row until it is dismissed, as in the official client.
    const bool running = invite && !vc.extras.invite_hovered && !g_app.preferences.reduced_motion;
    if (ui_voice_invite_tick(&vc.invite, now_ms, running)) {
        vc.extras.invite_channel_id = 0;
        vc.extras.invite_hovered = vc.extras.invite_close_hovered = false;
        bump();
    }
}

void application_voice_channels_release() noexcept {
    const u64 serial = vc.form_serial;
    const u32 session = vc.session;
    secure_clear_bytes(&vc, sizeof(vc));
    vc.form_serial = serial;
    vc.session = session + 1;
    vc.extras.info = &vc.info;
}
} // namespace maicord::app_detail
