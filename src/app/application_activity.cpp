#include "application_activity.hpp"
#include "application_state.hpp"
#include "account_ui_runtime.hpp"
#include "maicord/app/presence_idle.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {
namespace {
constexpr u64 kScanIntervalMs = 20'000;
constexpr u64 kChoiceRefreshMs = 5'000;
constexpr u32 kChoiceMaximum = 24;
constexpr u32 kAddGameRow = 0xff;

struct ActivityState {
    u64 account_scope;
    PresenceIdle idle;
    u64 idle_check_at, scan_at;
    // The detected game; started_ms survives rescans of the same game.
    char game_name[kGatewayLocalGameNameBytes];
    u64 game_application_id, game_started_ms;
    u8 game_name_bytes;
    bool game;
    GatewayLocalPresence published;
    bool published_valid;
    RegisteredGames games;
    bool games_loaded;
    // "Add a game" choices: running windowed programs.
    char choice_text[kChoiceMaximum * kRegisteredGameExecutableBytes];
    char choice_names[kChoiceMaximum][kRegisteredGameNameBytes];
    Utf8View choices[kChoiceMaximum];
    Utf8View choice_executables[kChoiceMaximum];
    u32 choice_count;
    u64 choices_at;
    // Share my activity request lane (PRIMARY).
    u64 request_id, retry_at, profile_id, self_user_id;
    u8 attempts;
    bool desired_share, queued, failed;
} g;
// Plugin rich presence outlives account scopes; the plugin clears its own.
struct PluginActivity {
    u64 owner_id, sequence, application_id, start_ms, end_ms;
    char name[kGatewayLocalGameNameBytes];
    char details[kGatewayLocalActivityTextBytes];
    char state[kGatewayLocalActivityTextBytes];
    u8 name_bytes, details_bytes, state_bytes, type;
};
constexpr u32 kPluginActivities = 8;
PluginActivity g_plugin_activities[kPluginActivities];
u64 g_plugin_activity_sequence;

const PluginActivity* plugin_activity() noexcept {
    const PluginActivity* newest = nullptr;
    for (const auto& activity : g_plugin_activities)
        if (activity.owner_id && (!newest || activity.sequence > newest->sequence))
            newest = &activity;
    return newest;
}

// Drops the account state but keeps the loaded registered games.
void reset() noexcept {
    const RegisteredGames games = g.games;
    const bool loaded = g.games_loaded;
    reset_value(&g);
    g.games = games;
    g.games_loaded = loaded;
}
void scope() noexcept {
    if (g.account_scope == g_app.plugin_account_scope_serial)
        return;
    if (g.request_id)
        return; // The primary lane still owns this completion.
    reset();
    g.account_scope = g_app.plugin_account_scope_serial;
}
bool module() noexcept {
    return g_app.account.instance && g_app.account.api.local_presence;
}
void load_games() noexcept {
    if (g.games_loaded)
        return;
    g.games_loaded = true;
    if (!registered_games_load(&g.games)) {
        reset_value(&g.games);
        log_write(LogLevel::warning, "activity", "registered_games_load_failed");
    }
}
GatewayActivitySharing sharing() noexcept {
    GatewayActivitySharing value{};
    if (module())
        g_app.account.api.local_presence(g_app.account.instance, nullptr, &value);
    return value;
}
bool shared() noexcept {
    const auto value = sharing();
    return g.queued ? g.desired_share : value.known && value.share;
}
u64 input_idle_ms() noexcept {
    LASTINPUTINFO info{};
    info.cbSize = sizeof(info);
    if (!GetLastInputInfo(&info))
        return 0;
    return static_cast<u32>(GetTickCount() - info.dwTime); // Wraps with the 32-bit tick.
}
bool in_call() noexcept {
    const auto* ui = g_app.account_ui;
    return ui && ui->voice_session.channel_id && ui->voice_session.phase != VoiceSetupPhase::idle;
}
void detect(u64 now_ms) noexcept {
    if (!g_app.preferences.game_detection) {
        g.game = false;
        g.scan_at = 0;
        return;
    }
    if (g.scan_at && now_ms < g.scan_at)
        return;
    g.scan_at = deadline_after(now_ms, kScanIntervalMs);
    load_games();
    char storage[16 * 1024];
    Utf8View names[512];
    const u32 count = game_detection_snapshot({storage, sizeof(storage)}, {names, 512}, false);
    const auto match = game_detection_match(g.games, game_detection_builtin(), {names, count});
    if (!match.found) {
        g.game = false;
        return;
    }
    const u32 bytes = minimum<u32>(match.name.bytes, kGatewayLocalGameNameBytes);
    if (!g.game || g.game_name_bytes != bytes || !bytes_equal(g.game_name, match.name.data, bytes))
        g.game_started_ms = clock_unix_milliseconds();
    copy_bytes(g.game_name, match.name.data, bytes);
    g.game_name_bytes = static_cast<u8>(bytes);
    g.game_application_id = match.application_id;
    g.game = true;
}
void refresh_choices(u64 now_ms) noexcept {
    if (g.choices_at && now_ms < g.choices_at)
        return;
    g.choices_at = deadline_after(now_ms, kChoiceRefreshMs);
    load_games();
    char storage[8 * 1024];
    Utf8View names[256];
    const u32 count = game_detection_snapshot({storage, sizeof(storage)}, {names, 256}, true);
    u32 text = 0;
    g.choice_count = 0;
    for (u32 i = 0; i < count && g.choice_count < kChoiceMaximum; ++i) {
        bool known = false;
        for (u32 k = 0; k < g.games.count && !known; ++k)
            known = utf8_equal(names[i], {g.games.items[k].executable, g.games.items[k].executable_bytes});
        if (known || names[i].bytes > sizeof(g.choice_text) - text)
            continue;
        copy_bytes(g.choice_text + text, names[i].data, names[i].bytes);
        const u32 slot = g.choice_count++;
        g.choice_executables[slot] = {g.choice_text + text, names[i].bytes};
        text += names[i].bytes;
        const u32 label = game_detection_display_name(names[i], {g.choice_names[slot], kRegisteredGameNameBytes});
        g.choices[slot] = {g.choice_names[slot], label};
    }
}
} // namespace

void application_activity_tick(u64 now_ms) noexcept {
    scope();
    if (!module())
        return;
    if (!g.idle_check_at || now_ms >= g.idle_check_at) {
        const PresenceIdleInput input{input_idle_ms(), clock_unix_milliseconds(), kPresenceIdleTimeoutMs, in_call(),
                                      false};
        presence_idle_update(&g.idle, input);
        g.idle_check_at = deadline_after(now_ms, presence_idle_next_check_ms(g.idle, input));
    }
    detect(now_ms);
    GatewayLocalPresence local{};
    local.idle = g.idle.idle;
    local.idle_since_ms = g.idle.idle ? g.idle.since_unix_ms : 0;
    if (const auto* plugin = plugin_activity()) {
        local.game = true;
        local.game_type = plugin->type;
        local.game_application_id = plugin->application_id;
        local.game_started_ms = plugin->start_ms;
        local.game_ends_ms = plugin->end_ms;
        copy_bytes(local.game_name, plugin->name, plugin->name_bytes);
        local.game_name_bytes = plugin->name_bytes;
        copy_bytes(local.game_details, plugin->details, plugin->details_bytes);
        local.game_details_bytes = plugin->details_bytes;
        copy_bytes(local.game_state, plugin->state, plugin->state_bytes);
        local.game_state_bytes = plugin->state_bytes;
    } else if (g.game) {
        local.game = true;
        local.game_application_id = g.game_application_id;
        local.game_started_ms = g.game_started_ms;
        copy_bytes(local.game_name, g.game_name, g.game_name_bytes);
        local.game_name_bytes = g.game_name_bytes;
    }
    if (g.published_valid && gateway_local_presence_equal(g.published, local))
        return;
    if (g_app.account.api.local_presence(g_app.account.instance, &local, nullptr)) {
        copy_value(&g.published, local);
        g.published_valid = true;
        application_request_frame(); // Settings show the playing game.
    }
}
u64 application_activity_next_wake() noexcept {
    if (!module())
        return 0;
    u64 wake = g.idle_check_at;
    if (g_app.preferences.game_detection)
        wake = earliest_deadline(wake, g.scan_at ? g.scan_at : 1);
    if (g.queued && !g.request_id && g.retry_at &&
        g_app.message_history_request_owner == MessageHistoryRequestOwner::none)
        wake = earliest_deadline(wake, g.retry_at);
    return wake;
}
void application_activity_clear() noexcept {
    if (!g.request_id)
        reset();
}
bool application_activity_set_plugin(u64 owner_id, const ApplicationPluginActivity* activity) noexcept {
    if (!owner_id)
        return false;
    PluginActivity* slot = nullptr;
    for (auto& candidate : g_plugin_activities)
        if (candidate.owner_id == owner_id)
            slot = &candidate;
    if (!activity) {
        if (slot)
            reset_value(slot);
    } else {
        if (!activity->name.bytes || (activity->type != 0 && activity->type != 2 && activity->type != 3 &&
                                      activity->type != 5))
            return false;
        for (auto& candidate : g_plugin_activities)
            if (!slot && !candidate.owner_id)
                slot = &candidate;
        if (!slot)
            return false;
        reset_value(slot);
        slot->owner_id = owner_id;
        slot->sequence = ++g_plugin_activity_sequence;
        slot->type = activity->type;
        slot->application_id = activity->application_id;
        slot->start_ms = activity->start_ms;
        slot->end_ms = activity->end_ms >= activity->start_ms ? activity->end_ms : 0;
        slot->name_bytes = static_cast<u8>(utf8_copy_truncated(slot->name, activity->name));
        slot->details_bytes = static_cast<u8>(utf8_copy_truncated(slot->details, activity->details));
        slot->state_bytes = static_cast<u8>(utf8_copy_truncated(slot->state, activity->state));
    }
    // Publish now rather than at the next idle check.
    application_activity_tick(clock_monotonic_milliseconds());
    return true;
}

bool application_activity_sharing_pending(u64 now_ms) noexcept {
    scope();
    return g.queued && !g.request_id && g.retry_at && g.retry_at <= now_ms && g_app.account.instance &&
           g_app.account.api.activity_sharing && sharing().known;
}
bool application_activity_sharing_submit(u64 request_id, u64 now_ms) noexcept {
    if (!request_id || !application_activity_sharing_pending(now_ms) || !g_app.account.api.self_status_snapshot)
        return false;
    const auto self = g_app.account.api.self_status_snapshot(g_app.account.instance);
    const auto submitted = g_app.account.api.activity_sharing(g_app.account.instance, self.profile_id,
                                                              self.self_user_id, g.desired_share, request_id);
    if (submitted.error == RestClientSubmitError::busy) {
        g.retry_at = deadline_after(now_ms, 50);
        return false;
    }
    if (!submitted.ok()) {
        log_u64(LogLevel::warning, "activity", "sharing_submit_error", static_cast<u64>(submitted.error));
        g.queued = false;
        g.failed = true;
        g.retry_at = 0;
        return false;
    }
    g.request_id = request_id;
    g.profile_id = self.profile_id;
    g.self_user_id = self.self_user_id;
    g.retry_at = 0;
    ++g.attempts;
    return true;
}
bool application_activity_sharing_response(const RestClientResponse& response, Utf8View body) noexcept {
    if (!g.request_id || response.request_id != g.request_id)
        return false;
    const bool consumed = g_app.account.instance && g_app.account.api.self_status_complete &&
                          g_app.account.api.self_status_complete(g_app.account.instance, g.profile_id,
                                                                 g.self_user_id, g.request_id, &response, body);
    g.request_id = 0;
    const bool ok = consumed && response.transport_ok() && response.status_code >= 200 && response.status_code < 300;
    if (!ok && response.transport_ok() && response.status_code == 429 && response.rate_limited &&
        response.retry_at_ms && !api_rejection_requires_action(response.rejection) && g.attempts < 3) {
        g.retry_at = response.retry_at_ms;
    } else {
        if (!ok)
            log_u64(LogLevel::warning, "activity", "sharing_response_status", response.status_code);
        g.queued = false;
        g.failed = !ok;
    }
    application_request_frame();
    return true;
}

bool application_activity_settings_project(SettingBinding binding, UiSettingsControl* control) noexcept {
    if (!control)
        return false;
    scope();
    switch (binding) {
    case SettingBinding::share_activity: {
        const auto value = sharing();
        control->checked = g.queued ? g.desired_share : value.share;
        control->enabled = value.known && !g.request_id && g_app.account.api.activity_sharing;
        if (g.failed)
            control->description = utf8_literal("Couldn't save this change. Try again.");
        else if (!value.known)
            control->description = utf8_literal("Available once MaiCord connects.");
        return true;
    }
    case SettingBinding::game_detection:
        control->checked = g_app.preferences.game_detection;
        return true;
    case SettingBinding::registered_games:
        return true;
    case SettingBinding::add_game:
        refresh_choices(clock_monotonic_milliseconds());
        control->selected_choice = kUiSettingsNoChoice;
        if (!g.choice_count) {
            // A dropdown needs at least one option; with nothing running the
            // row is a plain hint.
            control->kind = UiSettingsControlKind::notice;
            reset_value(&control->choices);
            control->description = utf8_literal("Open the game, then come back here to add it.");
            return true;
        }
        control->choices = {g.choices, g.choice_count};
        control->enabled = g.games.count < kRegisteredGameMaximum;
        return true;
    default:
        return false;
    }
}
bool application_activity_settings_activate(SettingBinding binding, bool checked, u32 choice, bool* result) noexcept {
    if (!result)
        return false;
    scope();
    switch (binding) {
    case SettingBinding::share_activity:
        *result = sharing().known && g_app.account.api.activity_sharing;
        if (*result) {
            g.desired_share = checked;
            g.queued = true;
            g.failed = false;
            g.attempts = 0;
            if (!g.request_id)
                g.retry_at = 1;
            application_request_frame();
        }
        return true;
    case SettingBinding::add_game: {
        *result = false;
        if (choice >= g.choice_count)
            return true;
        load_games();
        *result = registered_games_add(&g.games, g.choices[choice], g.choice_executables[choice]) &&
                  registered_games_save(g.games);
        g.choices_at = 0;
        g.scan_at = 0; // Detect the added game on the next tick.
        application_request_frame();
        return true;
    }
    default:
        return false;
    }
}
u32 application_activity_settings_list(Slice<UiSettingsControl> controls,
                                       Slice<UiSettingsControlPresentation> presentation) noexcept {
    if (!controls.data || !presentation.data)
        return 0;
    load_games();
    u32 count = 0;
    const Utf8View playing = g.game ? Utf8View{g.game_name, g.game_name_bytes} : Utf8View{};
    // The detected game leads the list, as the official "Now playing" card does.
    if (playing.bytes && count < controls.count && count < presentation.count) {
        bool custom = false;
        for (u32 i = 0; i < g.games.count && !custom; ++i)
            custom = utf8_equal(playing, {g.games.items[i].name, g.games.items[i].name_bytes});
        if (!custom) {
            controls[count] = {settings_list_control_id(SettingBinding::registered_games, kAddGameRow - 1), playing,
                               shared() ? utf8_literal("Playing now. Shown on your profile.")
                                        : utf8_literal("Playing now. Not shared."),
                               UiSettingsControlKind::notice, true, false, {}, 0, {}, nullptr};
            presentation[count++] = {UiSettingsVariant::standard, 0, kUiSettingsNoDefault};
        }
    }
    for (u32 i = 0; i < g.games.count && count < controls.count && count < presentation.count; ++i) {
        const auto& game = g.games.items[i];
        const Utf8View name{game.name, game.name_bytes};
        controls[count] = {settings_list_control_id(SettingBinding::registered_games, i), name,
                           utf8_equal(name, playing) ? utf8_literal("Playing now")
                                                     : Utf8View{game.executable, game.executable_bytes},
                           UiSettingsControlKind::action, true, false, {}, 0, utf8_literal("Remove"), nullptr};
        presentation[count++] = {UiSettingsVariant::critical, 0, kUiSettingsNoDefault};
    }
    if (!count && count < controls.count && count < presentation.count) {
        controls[count] = {settings_list_control_id(SettingBinding::registered_games, kAddGameRow),
                           utf8_literal("No games added"),
                           g_app.preferences.game_detection
                               ? utf8_literal("Detected games appear here while they run.")
                               : utf8_literal("Turn on game detection to show what you're playing."),
                           UiSettingsControlKind::notice, true, false, {}, 0, {}, nullptr};
        presentation[count++] = {UiSettingsVariant::standard, 0, kUiSettingsNoDefault};
    }
    return count;
}
bool application_activity_settings_row(u64 control_id) noexcept {
    if (control_id < kSettingsListFirst || control_id >= kSettingsListFirst + 0x10000)
        return false;
    return static_cast<SettingBinding>((control_id - kSettingsListFirst) >> 8) == SettingBinding::registered_games;
}
bool application_activity_settings_activate_row(u64 control_id) noexcept {
    if (!application_activity_settings_row(control_id))
        return false;
    const u32 index = static_cast<u32>((control_id - kSettingsListFirst) & 0xff);
    load_games();
    if (index >= g.games.count)
        return false;
    const bool removed = registered_games_remove(&g.games, index) && registered_games_save(g.games);
    g.scan_at = 0;
    g.choices_at = 0;
    application_request_frame();
    return removed;
}
} // namespace maicord::app_detail
