#pragma once

#include "maicord/core/base.hpp"

// The desktop's appearance and input settings from the settings portal (org.freedesktop.portal.Settings: ReadAll once,
// then the SettingChanged signal), kept current on the session bus (session.hpp).
//
// The window backend maps the portable part onto PlatformSettings (platform_settings_read) and raises settings_changed;
// the rest (accent colour, contrast, fonts, cursor, caption buttons) is read here directly. A value the desktop does not
// publish keeps its default. The keys read:
//
//   org.freedesktop.appearance          color-scheme (u: 1 dark, 2 light), accent-color ((ddd), each 0..1),
//                                       contrast (u: 1 more), reduced-motion (u: 1 reduced)
//   org.gnome.desktop.interface         enable-animations, cursor-blink, cursor-blink-time, cursor-size, cursor-theme,
//                                       font-name, document-font-name, monospace-font-name, text-scaling-factor,
//                                       color-scheme (when the portal's own is absent)
//   org.gnome.desktop.wm.preferences    button-layout, titlebar-font
//   org.gnome.desktop.peripherals.mouse double-click
//   org.kde.kdeglobals.KDE              DoubleClickInterval

namespace maicord::dbus {

enum class ColorScheme : u8 { no_preference = 0, dark = 1, light = 2 };

// The areas a change touched. The low four values are the window events' SettingsArea (settings_pointer, settings_caret,
// settings_motion, settings_scheme); the others are added by this module and ride in the same bit set.
enum DesktopSettingsArea : u32 {
    desktop_pointer = 1,
    desktop_caret = 2,
    desktop_motion = 4,
    desktop_scheme = 8,
    desktop_accent = 0x10,
    desktop_contrast = 0x20,
    desktop_fonts = 0x40,
    desktop_cursor = 0x80,
    desktop_caption = 0x100,
    desktop_all_areas = 0x1FF,
};

enum CaptionButton : u8 { caption_none = 0, caption_close = 1, caption_minimize = 2, caption_maximize = 3, caption_menu = 4 };

// The buttons of a caption bar, from the GNOME button-layout string: those left of the ':' and those right of it, in
// order.
struct CaptionButtons {
    u8 left[4];
    u8 left_count;
    u8 right[4];
    u8 right_count;
};

// Parses a GTK/GNOME button-layout ("appmenu:minimize,maximize,close"): appmenu is the window menu; spacers and names
// that are not caption buttons are skipped; a button appears once. Returns false for an empty layout string.
bool caption_buttons_parse(const char* layout, CaptionButtons* buttons) noexcept;

struct DesktopSettings {
    bool loaded;  // the portal answered (otherwise every field is its default)
    ColorScheme color_scheme;
    bool accent_valid;
    float accent[3];  // red, green, blue, 0..1
    bool more_contrast;
    bool reduced_motion;  // appearance reduced-motion
    bool animations;      // enable-animations; true when unknown
    u32 double_click_ms;  // 0 when unknown
    bool cursor_blink;
    u32 cursor_blink_ms;  // GTK's cursor-blink-time: one full on-off cycle
    float text_scale;     // 1.0 when unknown
    u32 cursor_size;      // 0 when unknown
    char cursor_theme[64];
    char ui_font[96];  // "Cantarell 11": family and size in points
    char document_font[96];
    char monospace_font[96];
    char titlebar_font[96];
    bool caption_known;
    CaptionButtons caption;  // meaningful when caption_known
};

void desktop_settings_defaults(DesktopSettings* settings) noexcept;

// Called after a read or a SettingChanged with the DesktopSettingsArea bits that changed (UI thread, inside
// session_bus_process).
using DesktopSettingsHandler = void (*)(void* context, u32 areas) noexcept;
void desktop_settings_set_handler(DesktopSettingsHandler handler, void* context) noexcept;

// Opens the session bus when needed, subscribes to SettingChanged and reads everything; idempotent. False without a bus.
bool desktop_settings_start() noexcept;
// Pumps the session bus until the first read finished (or failed) or `timeout_ms` passed; true when settings are loaded.
bool desktop_settings_wait_loaded(u32 timeout_ms) noexcept;
// A consistent copy of the current settings, from any thread; true when loaded (otherwise `*settings` is the defaults).
bool desktop_settings_get(DesktopSettings* settings) noexcept;

} // namespace maicord::dbus
