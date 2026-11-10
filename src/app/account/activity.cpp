#include "activity.hpp"
#include "../shell/state.hpp"
#include "../net/rest_rate_retry.hpp"
#include "account_access.hpp"
#include "account_ui_runtime.hpp"
#include "maicord/app/account/presence_idle.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/virtual_memory.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/app/gateway/client.hpp"

namespace maicord::app_detail {
namespace {
// The process snapshot interval. The official client's own interval is inside its native module, outside the
// client research; between scans nothing runs.
constexpr u64 kScanIntervalMs = 20'000;
constexpr u64 kChoiceRefreshMs = 5'000;
constexpr u32 kChoiceMaximum = 24;
constexpr u32 kAddGameRow = 0xff;
constexpr u32 kPlayedTextBytes = 32;
constexpr u32 kRemoveTextBytes = 8 + kRegisteredGameNameBytes;

struct ActivityState {
    u64 account_scope;
    PresenceIdle idle;
    u64 idle_check_at, scan_at;
    // The shown game (the official client's visible running game): its start holds while the same process runs.
    char game_name[kGatewayLocalGameNameBytes];
    u64 game_started_ms, game_application_id;
    u32 game_pid;
    u8 game_name_bytes;
    bool game, game_custom, streamer_tool;
    GatewayLocalPresence published;
    bool published_valid;
    // "Add it!" choices: programs owning a visible window that no game or added program names.
    char choice_text[kChoiceMaximum * kRegisteredGameExecutableBytes];
    char choice_names[kChoiceMaximum][kRegisteredGameNameBytes];
    Utf8View choices[kChoiceMaximum];
    Utf8View choice_executables[kChoiceMaximum];
    u32 choice_count;
    u64 choices_at;
    // Added Games rows: "Last played ..." and "Remove <name>".
    char played_text[kRegisteredGameMaximum][kPlayedTextBytes];
    char remove_text[kRegisteredGameMaximum][kRemoveTextBytes];
    // Share my activity request lane (PRIMARY).
    u64 request_id, retry_at, profile_id, self_user_id;
    u8 attempts;
    bool desired_share, queued, failed;
} g;
// The registered games outlive account scopes; loaded once.
RegisteredGames g_games;
bool g_games_loaded;
// Discord's detectable list (detectables.bin), loaded on the first scan with detection on and replaced when the
// refresh worker brings a new one; it outlives account scopes too.
struct Detectables {
    u8* image;
    u32 bytes;
    DetectableList list;
    bool loaded;
    u64 retry_at; // monotonic ms; a failed refresh waits an hour
} g_detectables;
constexpr u64 kDetectableRetryMs = 60 * 60 * 1000;
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

void reset() noexcept {
    reset_value(&g);
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
    return g_app.account.instance != nullptr;
}
void load_games() noexcept {
    if (g_games_loaded)
        return;
    g_games_loaded = true;
    if (!registered_games_load(&g_games)) {
        reset_value(&g_games);
        log_write(LogLevel::warning, "activity", "registered_games_load_failed");
    }
}
GatewayActivitySharing sharing() noexcept {
    GatewayActivitySharing value{};
    if (module())
        account_module_local_presence(g_app.account.instance, nullptr, &value);
    return value;
}
u64 input_idle_ms() noexcept {
    const u32 idle = platform_settings_read().system_idle_ms;
    return idle == kSystemIdleUnknown ? 0 : idle;
}
bool in_call() noexcept {
    const auto* ui = g_app.account_ui;
    return ui && ui->voice_session.channel_id && ui->voice_session.phase != VoiceSetupPhase::idle;
}
// The entry of the shown game: an added program by name, a detected game by application id (created on first
// sight when `create`).
u32 shown_entry(bool create) noexcept {
    if (!g.game)
        return kRegisteredGameNone;
    for (u32 i = 0; i < g_games.count; ++i) {
        const auto& game = g_games.items[i];
        if (g.game_custom ? (game.flags & registered_game_added) &&
                                utf8_equal({game.name, game.name_bytes}, {g.game_name, g.game_name_bytes})
                          : game.application_id == g.game_application_id)
            return i;
    }
    return g.game_custom || !create
               ? kRegisteredGameNone
               : registered_games_seen(&g_games, {g.game_name, g.game_name_bytes}, g.game_application_id);
}
// Records when the shown game was last played (Added Games' "Last played").
void played() noexcept {
    const u32 entry = shown_entry(true);
    if (entry == kRegisteredGameNone)
        return;
    g_games.items[entry].last_played_ms = clock_unix_milliseconds();
    if (!registered_games_save(g_games))
        log_write(LogLevel::warning, "activity", "registered_games_save_failed");
}
void end_game() noexcept {
    if (g.game)
        played();
    g.game = false;
}
void release_detectables() noexcept {
    detectable_refresh_cancel();
    if (g_detectables.image)
        pages_release(g_detectables.image, g_detectables.bytes);
    g_detectables = {};
}
void adopt_detectables(u8* image, u32 bytes) noexcept {
    DetectableList list{};
    if (!detectable_list_view({image, bytes}, &list)) {
        pages_release(image, bytes);
        log_write(LogLevel::warning, "activity", "detectables_invalid");
        return;
    }
    auto& d = g_detectables;
    if (d.image)
        pages_release(d.image, d.bytes);
    d.image = image;
    d.bytes = bytes;
    d.list = list;
}
// Loads the saved list once, takes a finished refresh, and starts the next one a day after either list's last
// answer (the official client's schedule; an hour after a failed run).
void detectables(u64 now_ms) noexcept {
    auto& d = g_detectables;
    if (!d.loaded) {
        d.loaded = true;
        u32 bytes = 0;
        if (u8* image = detectable_image_load(&bytes))
            adopt_detectables(image, bytes);
    }
    bool finished = false;
    const auto refreshed = detectable_refresh_take(&finished);
    if (refreshed.image)
        adopt_detectables(refreshed.image, refreshed.bytes);
    else if (finished)
        log_write(LogLevel::warning, "activity", "detectables_refresh_failed");
    const auto* h = d.list.header;
    const u64 unix_now = clock_unix_milliseconds();
    const bool due = !h || unix_now >= minimum(h->games_checked_ms, h->exclusions_checked_ms) + kDetectableRefreshMs;
    if (due && now_ms >= d.retry_at && detectable_refresh_start(unix_now))
        d.retry_at = deadline_after(now_ms, kDetectableRetryMs);
}
void detect(u64 now_ms) noexcept {
    const bool games = g_app.preferences.game_detection;
    if (!games && g_detectables.loaded)
        release_detectables();
    if (!games && !g_app.preferences.streamer_auto) {
        end_game();
        g.streamer_tool = false;
        g.scan_at = 0;
        return;
    }
    // The first scan waits an interval, so a process snapshot never lands in startup; 1 asks for one now.
    const bool first = !g.scan_at;
    if (first || now_ms < g.scan_at) {
        if (first)
            g.scan_at = deadline_after(now_ms, kScanIntervalMs);
        return;
    }
    g.scan_at = deadline_after(now_ms, kScanIntervalMs);
    load_games();
    if (games)
        detectables(now_ms);
    // Streamer tools alone when game detection is off: no list, and select is not asked.
    const DetectableList list = games ? g_detectables.list : DetectableList{};
    GameScan scan;
    game_detection_scan(g_games, list, &scan);
    bool tool = false;
    for (u32 i = 0; i < scan.count && !tool; ++i)
        tool = game_detection_streamer_tool(scan.processes[i]);
    g.streamer_tool = tool;
    const auto match = games ? game_detection_select(g_games, list, {scan.processes, scan.count},
                                                     g.game ? g.game_pid : 0, scan.foreground_pid)
                             : GameDetectionMatch{};
    if (!match.found) {
        end_game();
        return;
    }
    char name[kGatewayLocalGameNameBytes];
    const u32 bytes = utf8_copy_truncated(name, kGatewayLocalGameNameBytes, match.name);
    if (g.game && g.game_pid == match.pid && g.game_custom == match.custom &&
        g.game_application_id == match.application_id && utf8_equal({g.game_name, g.game_name_bytes}, {name, bytes}))
        return;
    end_game();
    copy_bytes(g.game_name, name, bytes);
    g.game_name_bytes = static_cast<u8>(bytes);
    g.game_pid = match.pid;
    g.game_custom = match.custom;
    g.game_application_id = match.application_id;
    g.game_started_ms = clock_unix_milliseconds();
    g.game = true;
    played();
}
void refresh_choices(u64 now_ms) noexcept {
    if (g.choices_at && now_ms < g.choices_at)
        return;
    g.choices_at = deadline_after(now_ms, kChoiceRefreshMs);
    load_games();
    Utf8View paths[kChoiceMaximum];
    const u32 count = game_detection_choices({g.choice_text, sizeof(g.choice_text)}, {paths, kChoiceMaximum});
    const DetectableList& list = g_detectables.list;
    g.choice_count = 0;
    for (u32 i = 0; i < count; ++i) {
        const GameProcess process{paths[i], {}, 0, true};
        bool known = game_detection_blocked(list, paths[i]) || game_detection_streamer_tool(process) ||
                     detectable_list_names(list, process);
        for (u32 k = 0; k < g_games.count && !known; ++k)
            known = (g_games.items[k].flags & registered_game_added) &&
                    utf8_equal(paths[i], {g_games.items[k].executable, g_games.items[k].executable_bytes});
        if (known)
            continue;
        const u32 slot = g.choice_count++;
        g.choice_executables[slot] = paths[i];
        const u32 label = game_detection_display_name(paths[i], {g.choice_names[slot], kRegisteredGameNameBytes});
        g.choices[slot] = {g.choice_names[slot], label};
    }
}
// "Toggle detection": a game turned off is never shown; the next tick rescans.
bool toggle_detection(u32 entry) noexcept {
    if (entry >= g_games.count)
        return false;
    g_games.items[entry].flags ^= registered_game_detection_off;
    g.scan_at = 1;
    application_request_frame();
    return registered_games_save(g_games);
}
// "Last played 3 days ago", with the official client's relative-time thresholds (moment's fromNow).
Utf8View played_text(u64 played_ms, char (&output)[kPlayedTextBytes]) noexcept {
    struct Span {
        u64 below, unit;
        Utf8View text; // After the rounded count when `unit` is above 1.
    };
    constexpr u64 kDay = 86'400;
    constexpr Span kSpans[]{
        {45, 1, utf8_literal("a few seconds ago")}, {90, 1, utf8_literal("a minute ago")},
        {45 * 60, 60, utf8_literal(" minutes ago")}, {90 * 60, 1, utf8_literal("an hour ago")},
        {22 * 3600, 3600, utf8_literal(" hours ago")}, {36 * 3600, 1, utf8_literal("a day ago")},
        {26 * kDay, kDay, utf8_literal(" days ago")}, {45 * kDay, 1, utf8_literal("a month ago")},
        {320 * kDay, 304 * kDay / 10, utf8_literal(" months ago")}, {548 * kDay, 1, utf8_literal("a year ago")},
        {~0ull, 365 * kDay, utf8_literal(" years ago")},
    };
    const u64 now_ms = clock_unix_milliseconds();
    const u64 seconds = now_ms > played_ms ? (now_ms - played_ms) / 1000 : 0;
    constexpr Utf8View kLead = utf8_literal("Last played ");
    copy_bytes(output, kLead.data, kLead.bytes);
    u32 used = kLead.bytes;
    for (const auto& span : kSpans) {
        if (seconds >= span.below)
            continue;
        if (span.unit > 1)
            used += decimal_write(output + used, (seconds + span.unit / 2) / span.unit);
        copy_bytes(output + used, span.text.data, span.text.bytes);
        return {output, used + span.text.bytes};
    }
    return {};
}
} // namespace

void application_activity_tick(u64 now_ms) noexcept {
    scope();
    if (!module())
        return;
    if (!g.idle_check_at || now_ms >= g.idle_check_at) {
        const PresenceIdleInput input{input_idle_ms(), clock_unix_milliseconds(), in_call()};
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
    if (account_module_local_presence(g_app.account.instance, &local, nullptr)) {
        copy_value(&g.published, local);
        g.published_valid = true;
        application_request_frame(); // Settings show the playing game.
    }
}
u64 application_activity_next_wake() noexcept {
    if (!module())
        return 0;
    u64 wake = g.idle_check_at;
    if (g_app.preferences.game_detection || g_app.preferences.streamer_auto)
        wake = earliest_deadline(wake, g.scan_at ? g.scan_at : 1);
    if (g.queued && !g.request_id && g.retry_at &&
        g_app.message_history_request_owner == MessageHistoryRequestOwner::none)
        wake = earliest_deadline(wake, g.retry_at);
    return wake;
}
void application_activity_clear() noexcept {
    // Account stop and exit: the refresh worker joins and the scan buffers go.
    release_detectables();
    game_detection_release();
    if (!g.request_id)
        reset();
}
bool application_activity_streamer_tool_running() noexcept {
    return g.streamer_tool;
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
           sharing().known;
}
bool application_activity_sharing_submit(u64 request_id, u64 now_ms) noexcept {
    if (!request_id || !application_activity_sharing_pending(now_ms))
        return false;
    const auto self = account_module_self_status_snapshot(g_app.account.instance);
    const auto submitted = account_module_activity_sharing(g_app.account.instance, self.profile_id,
                                                              self.self_user_id, g.desired_share, request_id);
    if (submit_busy(submitted, g.retry_at, now_ms, 50))
        return false;
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
    const bool consumed = g_app.account.instance &&
                          account_module_self_status_complete(g_app.account.instance, g.profile_id,
                                                                 g.self_user_id, g.request_id, &response, body);
    g.request_id = 0;
    const bool ok = consumed && response.succeeded();
    if (const u64 retry = rest_rate_retry_deadline(response); !ok && retry && g.attempts < 3) {
        g.retry_at = retry;
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
        control->enabled = value.known && !g.request_id;
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
    case SettingBinding::current_game:
        // Registered Games' Current Game: the shown game with its detection switch, else "No game detected".
        if (!g.game) {
            control->kind = UiSettingsControlKind::notice;
            control->label = utf8_literal("No game detected");
            control->description = {};
            return true;
        }
        control->label = {g.game_name, g.game_name_bytes};
        control->description = utf8_literal("Now playing!");
        control->checked = true;
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
        *result = sharing().known;
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
    case SettingBinding::current_game:
        load_games();
        *result = g.game && toggle_detection(shown_entry(true));
        return true;
    case SettingBinding::add_game: {
        *result = false;
        if (choice >= g.choice_count)
            return true;
        load_games();
        *result = registered_games_add(&g_games, g.choices[choice], g.choice_executables[choice]) &&
                  registered_games_save(g_games);
        g.choices_at = 0;
        g.scan_at = 1; // Detect the added game on the next tick.
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
    // Added Games, played most recently first: each game's detection switch, then Remove (not while it plays).
    u32 order[kRegisteredGameMaximum];
    for (u32 i = 0; i < g_games.count; ++i) {
        u32 at = i;
        for (; at && g_games.items[order[at - 1]].last_played_ms < g_games.items[i].last_played_ms; --at)
            order[at] = order[at - 1];
        order[at] = i;
    }
    const u32 playing = shown_entry(false);
    const u32 capacity = minimum<u32>(controls.count, presentation.count);
    u32 count = 0;
    for (u32 n = 0; n < g_games.count && count < capacity; ++n) {
        const u32 i = order[n];
        const auto& game = g_games.items[i];
        const Utf8View name{game.name, game.name_bytes};
        const Utf8View status = i == playing          ? utf8_literal("Now playing!")
                                : game.last_played_ms ? played_text(game.last_played_ms, g.played_text[i])
                                                      : Utf8View{};
        controls[count] = {settings_list_control_id(SettingBinding::registered_games, 2 * i), name, status,
                           UiSettingsControlKind::toggle, true, !(game.flags & registered_game_detection_off), {}, 0,
                           {}, nullptr};
        presentation[count++] = {UiSettingsVariant::standard, 0, kUiSettingsNoDefault};
        if (i == playing || count == capacity)
            continue;
        constexpr Utf8View kRemove = utf8_literal("Remove ");
        copy_bytes(g.remove_text[i], kRemove.data, kRemove.bytes);
        copy_bytes(g.remove_text[i] + kRemove.bytes, name.data, name.bytes);
        controls[count] = {settings_list_control_id(SettingBinding::registered_games, 2 * i + 1),
                           {g.remove_text[i], kRemove.bytes + name.bytes}, {}, UiSettingsControlKind::action, true,
                           false, {}, 0, utf8_literal("Remove"), nullptr};
        presentation[count++] = {UiSettingsVariant::critical, ui_settings_presentation_compact, kUiSettingsNoDefault};
    }
    if (!count && capacity) {
        controls[count] = {settings_list_control_id(SettingBinding::registered_games, kAddGameRow),
                           utf8_literal("No games added"),
                           g_app.preferences.game_detection
                               ? utf8_literal("Games you play appear here.")
                               : utf8_literal("Turn on game detection to show what you're playing."),
                           UiSettingsControlKind::notice, true, false, {}, 0, {}, nullptr};
        presentation[count++] = {UiSettingsVariant::standard, 0, kUiSettingsNoDefault};
    }
    return count;
}
bool application_activity_settings_row(u64 control_id) noexcept {
    return settings_list_control(control_id) && settings_list_binding(control_id) == SettingBinding::registered_games;
}
bool application_activity_settings_activate_row(u64 control_id) noexcept {
    if (!application_activity_settings_row(control_id))
        return false;
    const u32 row = settings_list_index(control_id);
    const u32 index = row / 2;
    load_games();
    if (index >= g_games.count)
        return false;
    if (!(row & 1))
        return toggle_detection(index);
    if (index == shown_entry(false))
        return false; // The playing game has no Remove.
    const bool removed = registered_games_remove(&g_games, index) && registered_games_save(g_games);
    g.scan_at = 1;
    g.choices_at = 0;
    application_request_frame();
    return removed;
}
} // namespace maicord::app_detail
