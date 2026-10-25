#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/regional_flag.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/reaction_viewer.hpp"

namespace maicord {
namespace {

constexpr Utf8View kTitle = utf8_literal("Reactions");
constexpr Utf8View kClose = utf8_literal("×");
constexpr Utf8View kLoading = utf8_literal("Loading reactions…");
constexpr Utf8View kFailed = utf8_literal("Couldn’t load reactions");
constexpr Utf8View kEmpty = utf8_literal("No reactions");
constexpr Utf8View kTruncated = utf8_literal("Some reactions aren't shown");
constexpr Utf8View kApp = utf8_literal("APP");

bool push(PaintStream* paint, UiRect rectangle, const Color& fill, const Color& border, float radius,
          float border_width) noexcept {
    return paint_stream_push_rounded_border(paint, rectangle.left, rectangle.top, rectangle.right,
                                            rectangle.bottom, fill, border, radius, border_width);
}

bool text_command(const ReactionViewer& viewer, UiTextOutput* text, Utf8View value, UiRect rectangle,
                  UiRect clip, const Color& color, u32 style = text_style_none,
                  UiTextAlignment horizontal = UiTextAlignment::start) noexcept {
    if (!ui_rect_has_area(ui_rect_intersection(rectangle, clip)))
        return true;
    return ui_text_output_push(text, UiTextCommand{value, viewer.text_owner, rectangle, clip, color, style,
                                                   horizontal, UiTextAlignment::center,
                                                   UiTextWrapping::no_wrap, UiTextOverflow::ellipsis});
}

} // namespace

bool reaction_viewer_presentation_build(const ReactionViewer& viewer, const Theme& theme,
                                        PaintStream* paint) noexcept {
    if (!viewer.active) {
        return true;
    }
    if (!paint || !ui_rect_has_area(viewer.viewport) || !ui_rect_has_area(viewer.panel) ||
        !paint_stream_push_clip(paint, viewer.viewport.left, viewer.viewport.top, viewer.viewport.right,
                                viewer.viewport.bottom)) {
        return false;
    }
    const Color scrim = theme.tokens.overlay.dense;
    bool success = paint_stream_push(paint, viewer.viewport.left, viewer.viewport.top,
                                     viewer.viewport.right, viewer.viewport.bottom, scrim) &&
                   ui_surface_paint(paint, viewer.panel, theme, UiElevation::dialog, theme.scale.radius_md,
                                    nullptr, &theme.interactive);
    if (success) {
        const float y = viewer.header.bottom;
        success =
            paint_stream_push(paint, viewer.panel.left, y, viewer.panel.right, y + 1.0f, theme.interactive);
    }
    if (!paint_stream_push_clip(paint, viewer.body.left, viewer.body.top, viewer.body.right,
                                viewer.body.bottom)) {
        paint_stream_pop_clip(paint);
        return false;
    }
    const Color hover = ui_background_hover_color(theme, theme.surface_high);
    for (u32 index = viewer.first_aggregate; success && index < viewer.aggregate_count; ++index) {
        const UiRect row = reaction_viewer_aggregate_rectangle(viewer, index);
        if (!ui_rect_has_area(row)) {
            break;
        }
        if ((index == viewer.selected_aggregate || index == viewer.hovered_aggregate) &&
            !push(paint, row, hover, hover, 4.0f, 0.0f)) {
            success = false;
        }
    }
    for (u32 index = viewer.first_user; success && index < viewer.user_count; ++index) {
        const UiRect row = reaction_viewer_user_rectangle(viewer, index);
        if (!ui_rect_has_area(row)) {
            break;
        }
        if (index == viewer.hovered_user && !push(paint, row, hover, hover, 4.0f, 0.0f)) {
            success = false;
            break;
        }
        const float avatar_extent = 32.0f;
        const float avatar_top = (row.top + row.bottom - avatar_extent) * 0.5f;
        const UiRect avatar{row.left, avatar_top, row.left + avatar_extent, avatar_top + avatar_extent};
        if (!push(paint, avatar, theme.interactive, theme.interactive, avatar_extent * 0.5f, 0.0f)) {
            success = false;
            break;
        }
        if ((viewer.users[index].flags & reaction_viewer_user_bot) != 0) {
            const UiRect badge{row.right - 42.0f, (row.top + row.bottom - 18.0f) * 0.5f, row.right,
                               (row.top + row.bottom + 18.0f) * 0.5f};
            if (!push(paint, badge, theme.tokens.emphasis.fill, theme.tokens.emphasis.fill, 3.0f, 0.0f)) {
                success = false;
            }
        }
    }
    const bool body_popped = paint_stream_pop_clip(paint);
    return paint_stream_pop_clip(paint) && body_popped && success;
}

bool reaction_viewer_text_presentation_build_output(const ReactionViewer& viewer, const Theme& theme,
                                                    UiTextOutput* text) noexcept {
    if (!viewer.active) {
        return true;
    }
    if (!ui_text_output_valid(text) ||
        !text_command(viewer, text, kTitle,
                      UiRect{viewer.header.left + 24.0f, viewer.header.top, viewer.close_button.left - 8.0f,
                             viewer.header.bottom},
                      viewer.panel, theme.text_primary, text_style_heading_3 | text_style_bold) ||
        !text_command(viewer, text, kClose, viewer.close_button, viewer.panel, theme.text_primary,
                      text_style_heading_3, UiTextAlignment::center)) {
        return false;
    }
    for (u32 index = viewer.first_aggregate; index < viewer.aggregate_count; ++index) {
        const UiRect row = reaction_viewer_aggregate_rectangle(viewer, index);
        if (!ui_rect_has_area(row)) {
            break;
        }
        const ReactionViewerAggregate& aggregate = viewer.aggregates[index];
        const Utf8View name = reaction_viewer_aggregate_name(viewer, index);
        const UiRect emoji{row.left + 4.0f, row.top, row.left + 36.0f, row.bottom};
        const UiRect count{row.left + 40.0f, row.top, row.right - 4.0f, row.bottom};
        if (name.bytes == 0 ||
            (aggregate.emoji_id == 0 && !regional_flag_sequence(name) &&
             !text_command(viewer, text, name, emoji, viewer.body, theme.text_primary,
                           text_style_picker_emoji, UiTextAlignment::center)) ||
            !text_command(viewer, text, Utf8View{aggregate.count_text, aggregate.count_text_bytes}, count,
                          viewer.body, ui_background_muted_text_color(theme), text_style_subtext)) {
            return false;
        }
    }
    for (u32 index = viewer.first_user; index < viewer.user_count; ++index) {
        const UiRect row = reaction_viewer_user_rectangle(viewer, index);
        if (!ui_rect_has_area(row)) {
            break;
        }
        const ReactionViewerUser& user = viewer.users[index];
        const float text_left = row.left + 44.0f;
        const float text_right =
            (user.flags & reaction_viewer_user_bot) != 0 ? row.right - 50.0f : row.right;
        const UiRect display{text_left, row.top + 2.0f, text_right, row.top + 24.0f};
        const UiRect username{text_left, row.top + 22.0f, text_right, row.bottom - 1.0f};
        if (!text_command(viewer, text, reaction_viewer_user_name(viewer, user), display, viewer.body,
                          theme.text_primary, text_style_bold) ||
            !text_command(viewer, text, reaction_viewer_user_username(viewer, user), username, viewer.body,
                          ui_background_muted_text_color(theme), text_style_subtext)) {
            return false;
        }
        if ((user.flags & reaction_viewer_user_bot) != 0) {
            const UiRect badge{row.right - 42.0f, row.top, row.right, row.bottom};
            if (!text_command(viewer, text, kApp, badge, viewer.body, kDefaultTheme.text_primary,
                              text_style_subtext | text_style_bold, UiTextAlignment::center)) {
                return false;
            }
        }
    }
    const Utf8View status = viewer.failed                               ? kFailed
                            : viewer.loading                            ? kLoading
                            : viewer.truncated                          ? kTruncated
                            : viewer.complete && viewer.user_count == 0 ? kEmpty
                                                                        : Utf8View{};
    if (status.bytes != 0) {
        const float right_left =
            ui_rect_width(viewer.panel) < 400.0f ? viewer.panel.left + 112.0f : viewer.panel.left + 138.0f;
        const UiRect rectangle{right_left, viewer.body.top + 4.0f, viewer.panel.right - 24.0f,
                               viewer.body.top + 44.0f};
        return text_command(viewer, text, status, rectangle, viewer.body,
                            viewer.failed ? theme.notification : ui_background_muted_text_color(theme),
                            text_style_subtext, UiTextAlignment::center);
    }
    return true;
}
} // namespace maicord
