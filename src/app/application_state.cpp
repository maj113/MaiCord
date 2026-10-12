#include "application_state.hpp"
#include "application_theme_publication.hpp"

#include <Windows.h>

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
                                            false,
                                            false,
                                            false,
                                            false,
                                            false,
                                            false,
                                            false,
                                            0,
                                            0,
                                            0},
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
        publication.theme = client_theme_projection(g_app.preferences);
        publication.theme.styles = publication.styles;
        publication.theme.style_revision = publication.styles ? publication.styles->revision : 0;
        remember_preferences(publication, g_app.preferences);
        publication.ready = true;
    }
    return publication.theme;
}

// Call after renderer and scene owners retire their borrowed Theme/styles.
void application_theme_release() noexcept {
    if (theme_publication.styles)
        VirtualFree(theme_publication.styles, 0, MEM_RELEASE);
    theme_publication = {};
}

bool application_request_frame() noexcept {
    g_app.frame_pending = true;
    // The normal loop observes this flag and waits for a DXGI display slot.
    // Windows owns dispatch during modal sizing, so retain a paint wakeup there.
    return !g_app.renderer.live_resize || !g_app.window ||
           InvalidateRect(g_app.window, nullptr, FALSE) != FALSE;
}

} // namespace maicord::app_detail
