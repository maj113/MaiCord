#include "state.hpp"
#include "maicord/core/memory.hpp"
#include "../settings/theme_publication.hpp"

#include "maicord/core/virtual_memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"

namespace maicord::app_detail {

// Keep the large startup owner zero-filled in the PE. The preference loader
// establishes user defaults before notifications initialize; embedding volume
// 100 here would serialize this entire aggregate into initialized .data.
// Any nonzero default member initializer reachable from Application (here the
// taskbar, tray and spellcheck toggles and the viewer's motion style) does the
// same: ~260 KB of file. List every ClientPreferences member here; the PE
// audit (tools/audit_pe.ps1 -MaximumInitializedDataBytes) fails the build if
// a new nonzero default brings the aggregate back into initialized .data.
// The viewer reveal is reset to the default style before first use.
constinit Application g_app{.preferences = {ClientTheme::dark,
                                            false,
                                            false,
                                            false,
                                            false,
                                            false,
                                            false,
                                            0,
                                            NotificationSoundpack::classic,
                                            {},
                                            {},
                                            false, // developer_mode
                                            false, // taskbar_flash
                                            false, // unread_badge
                                            false, // minimize_to_tray
                                            false, // start_minimized
                                            false, // spellcheck
                                            false, // game_detection
                                            false, // dm_profile_visible
                                            false, // role_colors_everywhere
                                            false, // reply_timestamps
                                            false, // folders_close_others
                                            false, // folders_close_on_home
                                            false, // folders_follow_selection
                                            false, // show_hidden_channels
                                            false, // image_zoom
                                            false, // image_zoom_square
                                            0,     // image_zoom_factor
                                            0,     // image_zoom_size
                                            0,     // message_group_spacing
                                            0,     // channel_pane_width
                                            0,     // member_pane_width
                                            0},    // expression_picker_width
                            .image_viewer = {.reveal = {.style = {0, UiMotionCurve::linear}}}};
constinit ThemePublication theme_publication{};

const Theme& application_theme() noexcept {
    auto& publication = theme_publication;
    // The custom owner publishes a copied snapshot through the transaction
    // below. Its effective document may be overwritten by a rejected preview;
    // no frame ever borrows or re-hashes that document here.
    if (!publication.custom &&
        (!publication.ready || publication.previous_kind != g_app.preferences.theme ||
         !background_theme_equal(publication.previous_gradient, g_app.preferences.gradient))) {
        Theme& theme = publication.set_theme(client_theme_projection(g_app.preferences));
        theme.styles = publication.styles;
        theme.style_revision = publication.styles ? publication.styles->revision : 0;
        remember_preferences(publication, g_app.preferences);
        publication.ready = true;
    }
    return publication.theme();
}

// Call after renderer and scene owners retire their borrowed Theme/styles.
void application_theme_release() noexcept {
    if (theme_publication.styles)
        pages_release(theme_publication.styles);
    reset_value(&theme_publication);
}

bool application_request_frame() noexcept {
    g_app.frame_pending = true;
    // The normal loop observes this flag and waits for a DXGI display slot.
    // Windows owns dispatch during modal sizing, so retain a paint wakeup there.
    if (g_app.renderer.live_resize && g_app.window)
        window_invalidate(g_app.window);
    return true;
}

} // namespace maicord::app_detail
