#pragma once

#include "maicord/ui/paint.hpp"
#include "maicord/ui/widget_manager.hpp"

namespace maicord {

enum UiLayoutDebugOverlayFlag : u32 {
    ui_layout_debug_none = 0,
    ui_layout_debug_bounds = 1u << 0,
    ui_layout_debug_content = 1u << 1,
    ui_layout_debug_margins = 1u << 2,
    ui_layout_debug_clips = 1u << 3,
    ui_layout_debug_baselines = 1u << 4,
    ui_layout_debug_diagnostics = 1u << 5,
    ui_layout_debug_anchors = 1u << 6,
    ui_layout_debug_all = ui_layout_debug_bounds | ui_layout_debug_content |
                          ui_layout_debug_margins | ui_layout_debug_clips |
                          ui_layout_debug_baselines |
                          ui_layout_debug_diagnostics | ui_layout_debug_anchors,
};

struct UiLayoutDebugPalette {
    Color bounds[4];
    Color content;
    Color margin;
    Color clip;
    Color baseline;
    Color warning;
    Color warning_fill;
};

extern const UiLayoutDebugPalette kDefaultUiLayoutDebugPalette;

bool ui_widget_manager_debug_paint(
    const UiWidgetManager& manager,
    ConstSlice<UiLayoutDiagnostic> diagnostics, u32 flags,
    const UiLayoutDebugPalette& palette, PaintStream* stream) noexcept;
u32 ui_widget_manager_format_diagnostic(
    const UiWidgetManager& manager, const UiLayoutDiagnostic& diagnostic,
    Slice<char> output) noexcept;
} // namespace maicord
