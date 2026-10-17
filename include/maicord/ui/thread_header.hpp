#pragma once
#include "maicord/ui/button.hpp"
#include "maicord/ui/popup_scene.hpp"
#include "maicord/ui/text_presentation.hpp"

namespace maicord {
struct UiScene;

enum class UiThreadMembership : u8 { unknown, joined, not_joined };
enum class UiThreadAction : u8 { none, check_status, join, leave, unarchive, retry, cancel, menu };
constexpr u32 ui_thread_action_bit(UiThreadAction action) noexcept {
    return action > UiThreadAction::none && action < UiThreadAction::menu
        ? 1u << (static_cast<u32>(action) - 1u) : 0;
}

// No default member initializers, so application globals stay zero-filled;
// callers opt in through ui_thread_header_style().
struct UiThreadHeaderStyle {
    float height, feedback_height, inset, gap, control_height;
    float primary_width, overflow_width, cancel_width;
};
constexpr UiThreadHeaderStyle ui_thread_header_style() noexcept {
    return {44.f, kUiMenuRowHeight, 12.f, kUiMenuRowGap, 32.f, 90.f, 32.f, 64.f};
}

// The host owns membership, archive/locked permission policy,
// operation lifetime, and persistent status strings. A roster is not input.
struct UiThreadHeaderView {
    u64 thread_id;
    u64 source_publication;
    u32 enabled_actions;
    UiThreadMembership membership;
    UiThreadAction operation;
    bool visible, archive_known, archived, locked_known, locked, busy, status_error;
    Utf8View status;
};
struct UiThreadHeaderWidgets {
    UiWidgetHandle root, status, primary, overflow, feedback, cancel;
};
struct UiThreadHeaderGeometry {
    UiRect rectangle, status, primary, overflow, feedback, cancel, clip;
};
struct UiThreadHeaderTarget {
    u64 thread_id, source_publication;
    UiWidgetHandle widget;
    UiThreadAction action;
};
struct UiThreadHeader {
    UiWidgetManager* manager;
    UiScene* shell;
    UiThreadHeaderWidgets widgets;
    UiThreadHeaderView view;
    UiThreadHeaderStyle style;
    UiThreadHeaderGeometry geometry;
    u64 captured_publication;
    u32 revision, projections;
    bool active, published;
};
struct UiThreadHeaderPointerResult {
    UiThreadHeaderTarget target;
    bool handled, activate, changed;
};
struct UiThreadMenuActions {
    u32 check_status, join, leave, unarchive, retry, cancel;
};

float ui_thread_header_height(const UiThreadHeaderView&, const UiThreadHeaderStyle&) noexcept;
UiThreadAction ui_thread_header_primary_action(const UiThreadHeaderView&) noexcept;
Utf8View ui_thread_action_label(UiThreadAction) noexcept;
bool ui_thread_header_project(const UiThreadHeaderView&, const UiThreadHeaderStyle&, UiRect rectangle,
    UiRect clip, UiThreadHeaderGeometry*) noexcept;
// Bind six caller-created shell widgets. No allocation or independent manager.
bool ui_thread_header_bind(UiThreadHeader*, UiWidgetManager*, UiThreadHeaderWidgets) noexcept;
bool ui_thread_header_bind_shell(UiThreadHeader*, UiScene*) noexcept;
// Call after shell layout. Updates only these six native nodes; ordinary
// message/composer nodes are untouched. Equal revisions skip the update.
bool ui_thread_header_update(UiThreadHeader*, const UiThreadHeaderView&, UiRect rectangle, UiRect clip,
    const UiThreadHeaderStyle&) noexcept;
bool ui_thread_header_current(const UiThreadHeader&, const UiThreadHeaderTarget&) noexcept;
UiThreadHeaderTarget ui_thread_header_widget_target(const UiThreadHeader&, UiWidgetHandle) noexcept;
UiThreadHeaderPointerResult ui_thread_header_pointer(UiThreadHeader*, UiButtonPointerEvent,
    UiPoint) noexcept;
bool ui_thread_header_paint(const UiThreadHeader&, const Theme&, PaintStream*) noexcept;
bool ui_thread_header_text(const UiThreadHeader&, const Theme&, UiTextOutput*) noexcept;
// Copies at most six standard native menu rows. Zero IDs omit actions, and
// insufficient capacity / colliding supplied IDs leave output unchanged.
u32 ui_thread_menu_build(const UiThreadHeaderView&, UiThreadMenuActions, Slice<PopupItem>) noexcept;

static_assert(__is_trivial(UiThreadHeader) && __is_standard_layout(UiThreadHeader));
static_assert(sizeof(UiThreadHeader) < 512);
} // namespace maicord
