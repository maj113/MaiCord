// The fonts the desktop chose, handed to the text system. The window backend reads the settings portal (org.gnome.desktop.
// interface font-name and monospace-font-name) when it creates the window; default_fonts_set_portal lets those take
// precedence over the configuration files (KDE's kdeglobals, GTK's settings.ini) when the default families resolve.
#include "../shell/window.hpp"
#include "maicord/platform/dbus/desktop_settings.hpp"
#include "maicord/platform/font/default_fonts.hpp"

namespace maicord::app_detail {

void application_platform_desktop_fonts() noexcept {
    dbus::DesktopSettings desktop{};
    // With no portal, the configuration files and fontconfig decide alone.
    if (dbus::desktop_settings_get(&desktop))
        default_fonts_set_portal(desktop.ui_font[0] ? desktop.ui_font : nullptr,
                                 desktop.monospace_font[0] ? desktop.monospace_font : nullptr);
    // Soon after this, the main thread waits for the GPU device, and the text system needs the families right after
    // that wait. So fontconfig resolves them (a few milliseconds) before the wait, not after it.
    default_font_family(DefaultFont::ui);
}

} // namespace maicord::app_detail
