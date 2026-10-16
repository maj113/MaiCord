#pragma once
#include "application_account_presentation.hpp"
#include "application_friends.hpp"
#include "application_settings.hpp"
#include "application_state.hpp"

namespace maicord::app_detail {
// A new layout cannot present the previous projection's glyph positions.
// Its first rebuild must not inherit a delay from the rejected geometry.
inline bool refresh_account_text_for_layout() noexcept {
    if (g_app.account_ui)
        g_app.account_ui->text_rebuild_retry_at_ms = 0;
    return refresh_account_text(true);
}
inline bool application_account_text_retry_eligible() noexcept {
    return g_app.account_ui && g_app.account.instance && !g_app.account.stop_failed &&
           !g_app.login.visible && g_app.message_scene.active &&
           (!application_settings_active() || renderer_theme_editor_visible(g_app.renderer)) &&
           !application_friends_active();
}
inline u64 application_account_text_retry_wake() noexcept {
    return g_app.account_ui && g_app.account_ui->text_rebuild_retry_at_ms &&
                   application_account_text_retry_eligible()
               ? g_app.account_ui->text_rebuild_retry_at_ms
               : 0;
}
inline bool application_account_timestamp_refresh_eligible(u64 now_ms) noexcept {
    return application_account_text_retry_eligible() &&
           (!g_app.account_ui->text_rebuild_retry_at_ms ||
            now_ms >= g_app.account_ui->text_rebuild_retry_at_ms);
}
} // namespace maicord::app_detail
