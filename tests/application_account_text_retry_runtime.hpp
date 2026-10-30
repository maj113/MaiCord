#pragma once
#include "../src/app/application_account_presentation.hpp"
#include "maicord/ui/private_channel_rows.hpp"
#include "maicord/render/renderer_settings_visibility.hpp"
#include "../src/app/account_ui_runtime.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "../src/app/application_account_text_retry.hpp"
#include "../src/app/application_composer_input.hpp"
#include "../src/app/application_attachments.hpp"
#include "../src/app/application_dm_profile.hpp"

namespace maicord::account_text_retry_runtime_test {
inline bool active{}, reject{}, settings{}, friends{}, last_force{}, pins{}, threads{};
inline u32 attempts{}, accepted_width{}, shell_syncs{}, shell_layouts{};
} // namespace maicord::account_text_retry_runtime_test
namespace maicord::app_detail {
const UiPrivateRowSource* application_private_channel_rows() noexcept {
    return nullptr;
}
bool application_settings_active() noexcept {
    return account_text_retry_runtime_test::settings;
}
bool application_friends_active() noexcept {
    return account_text_retry_runtime_test::friends;
}
bool application_settings_synchronize() noexcept {
    return true;
}
bool application_theme_editor_active() noexcept {
    return renderer_theme_editor_visible(g_app.renderer);
}
bool application_friends_synchronize() noexcept {
    return true;
}
Utf8View application_account_notice() noexcept {
    return {};
}
MessageComposerAttachmentInput application_attachment_input() noexcept {
    return {};
}
bool synchronize_shell_layout() noexcept {
    ++account_text_retry_runtime_test::shell_syncs;
    ++account_text_retry_runtime_test::shell_layouts;
    return true;
}
bool application_pinned_messages_available() noexcept {
    return account_text_retry_runtime_test::pins;
}
bool application_thread_browser_available(u64) noexcept {
    return account_text_retry_runtime_test::threads;
}
bool synchronize_shell_widget_scenes() noexcept {
    ++account_text_retry_runtime_test::shell_syncs;
    return true;
}
// No DM profile panel in these fixtures.
void application_dm_profile_synchronize(bool) noexcept {}
bool refresh_active_autocomplete() noexcept {
    return true;
}
bool rebuild_account_text_cache(bool, bool) noexcept {
    auto& count = account_text_retry_runtime_test::attempts;
    ++count;
    if (account_text_retry_runtime_test::reject)
        return false;
    account_text_retry_runtime_test::accepted_width = g_app.renderer.width;
    g_app.account_ui->message_text.next_timestamp_update_unix_seconds =
        clock_unix_milliseconds() / 1'000 + 60;
    return true;
}
} // namespace maicord::app_detail
namespace maicord {
bool renderer_rebuild_content_text(D3D11Renderer*, const Theme&) noexcept {
    return true;
}
} // namespace maicord

// Compile the exact production freshness/backoff body against this target's
// surrounding host adapters. No alternative retry algorithm lives in the test.
#define MAICORD_ACCOUNT_TEXT_REFRESH_RUNTIME_TEST 1
#define refresh_account_text refresh_account_text_runtime_impl
#define refresh_account_frame refresh_account_frame_runtime_impl
#include "../src/app/application_account_presentation.cpp"
#undef refresh_account_frame
#undef refresh_account_text
#undef MAICORD_ACCOUNT_TEXT_REFRESH_RUNTIME_TEST

namespace maicord::account_text_retry_runtime_test {
inline void run(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace app_detail;
    auto& ui = *g_app.account_ui;
    const auto old_retry = ui.text_rebuild_retry_at_ms;
    const auto old_failures = ui.text_rebuild_failures;
    const auto old_timestamp = ui.message_text.next_timestamp_update_unix_seconds;
    const auto old_instance = g_app.account.instance;
    const bool old_failed = g_app.account.stop_failed, old_login = g_app.login.visible,
               old_scene = g_app.message_scene.active, old_pending = g_app.frame_pending;
    const auto old_width = g_app.renderer.width;
    const auto old_typing = ui.typing_store;
    TypingRecord typing_records[1]{};
    check(typing_store_initialize(&ui.typing_store, {typing_records, 1}),
          "text retry: independent empty typing store initializes without unrelated expiry wakes");
    g_app.account.instance = reinterpret_cast<void*>(1);
    g_app.account.stop_failed = g_app.login.visible = false;
    g_app.message_scene.active = true;
    g_app.renderer.width = 801;
    g_app.frame_pending = false;
    active = reject = true;
    settings = friends = false;
    attempts = accepted_width = 0;
    ui.text_rebuild_retry_at_ms = 0;
    ui.text_rebuild_failures = 0;
    ui.message_text.next_timestamp_update_unix_seconds = clock_unix_milliseconds() / 1'000 - 1;
    // All account/model source revisions are zero and equal to their rendered
    // revisions. Only forced layout presentation can admit the first attempt.
    check(refresh_account_text(true) && attempts == 1 && ui.text_rebuild_retry_at_ms &&
              ui.text_rebuild_failures == 1 && !accepted_width,
          "text retry: failed forced layout creates bounded recovery authority without source mutations");
    const auto now = clock_monotonic_milliseconds();
    ui.text_rebuild_retry_at_ms = now + 60'000;
    reject = false;
    for (u32 i = 0; i < 64; ++i) {
        refresh_account_text(false);
        check(application_refresh_relative_timestamps_if_due(),
              "text retry: overdue timestamp tick acknowledges pending backoff");
    }
    check(attempts == 1 && !g_app.frame_pending &&
              application_wait_timeout(now, now * 1000, true) == 60'000 &&
              !application_nonvisual_wake_due(now),
          "text retry: overdue timestamp and repeated freshness honor backoff without zero wait or frames");
    ui.text_rebuild_retry_at_ms = clock_monotonic_milliseconds() - 1;
    const auto overdue = clock_monotonic_milliseconds();
    for (u32 cover = 0; cover < 3; ++cover) {
        settings = cover == 0;
        friends = cover == 1;
        g_app.login.visible = cover == 2;
        check(application_wait_timeout(overdue, overdue * 1000, true) == INFINITE &&
                  !application_nonvisual_wake_due(overdue) && refresh_account_text(false) &&
                  application_refresh_relative_timestamps_if_due() && attempts == 1 &&
                  ui.text_rebuild_retry_at_ms && !g_app.frame_pending,
              "text retry: covered overdue projection retains authority without a hot wake or retry");
    }
    settings = friends = g_app.login.visible = false;
    g_app.message_scene.active = false;
    check(application_wait_timeout(overdue, overdue * 1000, true) == INFINITE &&
              !application_nonvisual_wake_due(overdue) &&
              application_refresh_relative_timestamps_if_due() && attempts == 1 && !g_app.frame_pending,
          "text retry: inactive chat has no overdue recovery wake");
    g_app.message_scene.active = true;
    check(application_wait_timeout(overdue, overdue * 1000, true) == 0 &&
              application_nonvisual_wake_due(overdue),
          "text retry: uncover independently admits due recovery without a network signal");
    check(application_pump_account() && !last_force && attempts == 2 && accepted_width == 801 &&
              !ui.text_rebuild_retry_at_ms && !ui.text_rebuild_failures && g_app.frame_pending &&
              ui.message_text.next_timestamp_update_unix_seconds > clock_unix_milliseconds() / 1'000,
          "text retry: unchanged-source recovery accepts current width and publishes a real pending frame");
    g_app.frame_pending = false;
    ui.message_text.next_timestamp_update_unix_seconds = clock_unix_milliseconds() / 1'000 - 1;
    check(
        application_wait_timeout(overdue, overdue * 1000, true) == 0 &&
            application_refresh_relative_timestamps_if_due() && last_force && attempts == 3 &&
            g_app.frame_pending &&
            ui.message_text.next_timestamp_update_unix_seconds > clock_unix_milliseconds() / 1'000,
        "text retry: uncovered due relative timestamp still forces semantic refresh and advances its wake");
    g_app.frame_pending = false;
    ui.message_text.next_timestamp_update_unix_seconds = clock_unix_milliseconds() / 1'000 - 1;
    for (u32 cover = 0; cover < 3; ++cover) {
        settings = cover == 0;
        friends = cover == 1;
        g_app.login.visible = cover == 2;
        check(application_wait_timeout(overdue, overdue * 1000, true) == INFINITE &&
                  application_refresh_relative_timestamps_if_due() && attempts == 3 &&
                  !g_app.frame_pending && !ui.text_rebuild_retry_at_ms,
              "text retry: covered overdue semantic timestamp alone cannot wake or request frames");
    }
    settings = friends = g_app.login.visible = false;
    UiThemeEditorScene preview{};
    const auto* old_editor = g_app.renderer.theme_editor_scene;
    const bool old_renderer_login = g_app.renderer.login_visible;
    const bool preview_ready =
        ui_theme_editor_update(&preview, background_theme_defaults(), 0, {0, 0, 801, 600});
    g_app.renderer.theme_editor_scene = preview_ready ? &preview : nullptr;
    g_app.renderer.login_visible = false;
    settings = true;
    check(preview_ready && application_settings_active() && application_account_text_retry_eligible() &&
              refresh_account_text(true) && attempts == 4,
          "text retry: current live theme preview preserves Settings authority while admitting one "
          "forced foreground projection refresh");
    g_app.renderer.theme_editor_scene = old_editor;
    g_app.renderer.login_visible = old_renderer_login;
    ui_theme_editor_release(&preview);
    settings = false;
    ui.text_rebuild_retry_at_ms = overdue;
    g_app.account.stop_failed = true;
    check(application_wait_timeout(overdue, overdue * 1000, true) == INFINITE &&
              !application_nonvisual_wake_due(overdue),
          "text retry: failed host teardown cannot retain a hot retry wake");
    g_app.account.stop_failed = false;
    ui.text_rebuild_retry_at_ms = 0;
    ui.text_rebuild_failures = 0;
    const u32 scroll_attempts = attempts, scroll_syncs = shell_syncs;
    check(refresh_account_scroll() && attempts == scroll_attempts + 1 && shell_syncs == scroll_syncs,
          "scroll refresh: current sources reproject message text without shell-scene synchronization");
    ++ui.typing_store.revision;
    check(refresh_account_scroll() && attempts == scroll_attempts + 2 && shell_syncs == scroll_syncs + 1 &&
              ui.rendered_typing_revision == ui.typing_store.revision,
          "scroll refresh: a stale account source takes the full refresh");
    reject = true;
    check(refresh_account_scroll() && attempts == scroll_attempts + 4 && ui.text_rebuild_retry_at_ms &&
              ui.text_rebuild_failures == 1,
          "scroll refresh: a rejected projection falls back to the full path and its retry backoff");
    reject = false;
    check(refresh_account_scroll() && attempts == scroll_attempts + 4,
          "scroll refresh: pending retry backoff is honored instead of the scroll-only path");
    // Resizing publishes new row/media rectangles before text is rebuilt.
    // A delay for the previous projection must not preserve its glyph positions.
    const u32 resize_attempts = attempts;
    ui.text_rebuild_retry_at_ms = clock_monotonic_milliseconds() + 60'000;
    g_app.renderer.width = 937;
    check(refresh_account_text_for_layout() && attempts == resize_attempts + 1 && last_force &&
              accepted_width == 937 && !ui.text_rebuild_retry_at_ms && !ui.text_rebuild_failures,
          "text retry: changed layout rebuilds at its new width before the previous retry deadline");
    ui.text_rebuild_retry_at_ms = 0;
    ui.text_rebuild_failures = 0;
    // The scheduler wakes at the earliest typing expiry, so an expired record
    // must leave the store behind every presentation early-out (Settings,
    // Friends, unchanged text); otherwise each later wait is a zero timeout.
    for (u32 cover = 0; cover < 3; ++cover) {
        settings = cover == 0;
        friends = cover == 1;
        const auto typed = typing_store_start(&ui.typing_store, 77, 0, 5,
                                              clock_monotonic_milliseconds() - kTypingLifetimeMs - 1);
        // The typing row is already on screen: no text source is stale.
        ui.rendered_typing_revision = ui.typing_store.revision;
        const u64 at = clock_monotonic_milliseconds();
        check(typed.ok() && ui.typing_store.count == 1 && refresh_account_text(false) &&
                  ui.typing_store.count == 0 && typing_store_next_wake_at(ui.typing_store) == 0 &&
                  application_wait_timeout(at, at * 1000, true) != 0 && !application_nonvisual_wake_due(at),
              "typing expiry: an expired record leaves no overdue wake behind covered or unchanged text");
    }
    settings = friends = false;
    // Launch restores a channel before READY: its header lays out without Pins
    // or Threads. Permissions arriving later must lay the header out again.
    ui.text_rebuild_retry_at_ms = 0;
    UiShellLayoutMetrics metrics = kGuildChannelShellLayoutMetrics;
    pins = threads = false;
    const bool laid_out = ui_scene_initialize_bootstrap(&g_app.shell_scene) &&
                          ui_scene_layout_with_metrics(&g_app.shell_scene, UiShellView::guild_channel, metrics, 1280, 800);
    const u32 layouts = shell_layouts;
    ++ui.chat_state.permission_mutations;
    check(laid_out && refresh_account_text(false) && shell_layouts == layouts &&
              !ui_scene_node(g_app.shell_scene, UiSemantic::pins_toggle),
          "header layout: unchanged channel rights keep the header");
    pins = threads = true;
    ++ui.chat_state.permission_mutations;
    check(refresh_account_text(false) && shell_layouts == layouts + 1,
          "header layout: Pins and Threads arriving after the first layout re-lay the header out");
    metrics.flags = static_cast<u16>(metrics.flags | shell_header_flags(false));
    metrics.revision = 2;
    ++ui.chat_state.permission_mutations;
    check(ui_scene_layout_with_metrics(&g_app.shell_scene, UiShellView::guild_channel, metrics, 1280, 800) &&
              ui_scene_node(g_app.shell_scene, UiSemantic::pins_toggle) &&
              ui_scene_node(g_app.shell_scene, UiSemantic::threads_toggle) &&
              refresh_account_text(false) && shell_layouts == layouts + 1,
          "header layout: the re-laid header shows both buttons and settles");
    ui_scene_release(&g_app.shell_scene);
    pins = threads = false;
    auto* old_ui = g_app.account_ui;
    g_app.account_ui = nullptr;
    check(application_wait_timeout(overdue, overdue * 1000, true) == INFINITE &&
              !application_nonvisual_wake_due(overdue),
          "text retry: retired account owner contributes no stale deadline");
    g_app.account_ui = old_ui;
    ui.text_rebuild_retry_at_ms = old_retry;
    ui.text_rebuild_failures = old_failures;
    ui.message_text.next_timestamp_update_unix_seconds = old_timestamp;
    g_app.account.instance = old_instance;
    g_app.account.stop_failed = old_failed;
    g_app.login.visible = old_login;
    g_app.message_scene.active = old_scene;
    g_app.frame_pending = old_pending;
    g_app.renderer.width = old_width;
    ui.typing_store = old_typing;
    active = reject = settings = friends = false;
}
} // namespace maicord::account_text_retry_runtime_test
