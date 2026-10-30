#pragma once

#include "maicord/ui/text_presentation.hpp"

namespace maicord {

// Placement bits of ui_scene_text: wrapped lines, a scene-defined bit that selects
// the body region instead of the panel, and the horizontal alignment
// (UiTextAlignment) in bits 2-3.
inline constexpr u32 ui_text_wrap = 1;
inline constexpr u32 ui_text_in_body = 2;
inline constexpr u32 ui_text_center = 1u << 2;
inline constexpr u32 ui_text_end = 2u << 2;

// Pushes one clipped text command; empty text or an empty clip pushes nothing.
__declspec(noinline) inline bool ui_scene_text(UiTextOutput* out, const UiWidgetManager& widgets,
                                               UiWidgetHandle owner, const Utf8View& value, const UiRect& rectangle,
                                               const UiRect& region, const Color& color, u32 flags,
                                               u32 mode) noexcept {
    const auto clip = ui_rect_intersection(rectangle, region);
    const bool wrap = mode & ui_text_wrap;
    return !value.bytes || !ui_rect_has_area(clip) ||
           ui_text_output_push(out, {value, {&widgets, owner}, rectangle, clip, color, flags,
                                     static_cast<UiTextAlignment>(mode >> 2),
                                     wrap ? UiTextAlignment::start : UiTextAlignment::center,
                                     wrap ? TextWrapping::wrap : TextWrapping::no_wrap, TextOverflow::ellipsis,
                                     UiTextFace::normal});
}

// Like ui_scene_text for a caller-chosen clip that is pushed as given; text is
// always vertically centred.
__declspec(noinline) inline bool ui_scene_label(UiTextOutput* out, const UiWidgetManager& widgets,
                                                UiWidgetHandle owner, const Utf8View& value,
                                                const UiRect& rectangle, const UiRect& clip, const Color& color,
                                                u32 flags, u32 mode) noexcept {
    if (!value.bytes || !ui_rect_intersects(rectangle, clip))
        return true;
    return ui_text_output_push(out, {value, {&widgets, owner}, rectangle, clip, color, flags,
                                     static_cast<UiTextAlignment>(mode >> 2), UiTextAlignment::center,
                                     mode & ui_text_wrap ? TextWrapping::wrap : TextWrapping::no_wrap,
                                     TextOverflow::ellipsis, UiTextFace::normal});
}

} // namespace maicord
