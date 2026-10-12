#pragma once
#include "maicord/ui/widget_storage.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/text_presentation.hpp"

namespace maicord {
// Keyboard shortcuts overlay (Ctrl+/): a centered dialog over a scrim with a
// query line and the shortcut list, grouped under headings and balanced over
// two columns when the dialog is wide enough. Each row shows its label and up
// to two chords drawn as key caps.
inline constexpr u32 kUiShortcutsMaximumRows = 64;
inline constexpr u32 kUiShortcutsChordKeys = 4;
struct UiShortcutsRow {
    Utf8View label;
    Utf8View keys[2][kUiShortcutsChordKeys];
    u8 key_counts[2];
    bool header;
};
// Borrowed until the next update; publish changes through revision.
struct UiShortcutsContent {
    Utf8View title, query, placeholder, empty;
    ConstSlice<UiShortcutsRow> rows;
    u32 revision;
};
enum class UiShortcutsTarget : u8 {
    none,
    close,   // The close button or the scrim outside the dialog.
    search,  // The query line.
    body     // The list.
};
struct UiShortcutsScene {
    UiFixedWidgetStorageOwner storage;
    UiWidgetManager widgets;
    const UiShortcutsContent* content{};
    UiRect viewport{}, panel{}, search{}, body{}, close_rectangle{};
    // Row rectangles relative to the unscrolled body origin.
    UiRect rows[kUiShortcutsMaximumRows]{};
    UiWidgetHandle root{}, close{};
    float scroll{}, content_height{};
    u32 row_count{}, columns{}, content_revision{}, revision{};
    bool active{};
};
bool ui_shortcuts_scene_update(UiShortcutsScene*, const UiShortcutsContent*, UiRect viewport) noexcept;
void ui_shortcuts_scene_release(UiShortcutsScene*) noexcept;
bool ui_shortcuts_scene_current(const UiShortcutsScene&) noexcept;
UiShortcutsTarget ui_shortcuts_scene_hit_test(const UiShortcutsScene&, float x, float y) noexcept;
// Scrolls the list by delta DIPs within its content; true when it moved.
bool ui_shortcuts_scene_scroll(UiShortcutsScene*, float delta) noexcept;
bool ui_shortcuts_scene_set_hover(UiShortcutsScene*, float x, float y, bool* changed) noexcept;
// Visible rectangle of a row; false when the row is scrolled out of the list.
bool ui_shortcuts_scene_row(const UiShortcutsScene&, u32 index, UiRect* rectangle) noexcept;
// Estimated key cap width for a key label.
float ui_shortcuts_key_width(Utf8View key) noexcept;
bool ui_shortcuts_scene_paint(const UiShortcutsScene&, const Theme&, PaintStream*) noexcept;
bool ui_shortcuts_scene_text_output(const UiShortcutsScene&, const Theme&, UiTextOutput*) noexcept;
} // namespace maicord
