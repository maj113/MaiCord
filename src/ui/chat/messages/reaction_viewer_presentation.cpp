#include "maicord/ui/theme/surface_elevation.hpp"
#include "maicord/ui/text/regional_flag.hpp"
#include "maicord/ui/kit/scene_kit.hpp"
#include "maicord/ui/chat/messages/reaction_viewer.hpp"

namespace maicord {
namespace {

constexpr Utf8View kTitle = utf8_literal("Reactions");
constexpr Utf8View kClose = utf8_literal("×");
constexpr Utf8View kLoading = utf8_literal("Loading reactions…");
constexpr Utf8View kFailed = utf8_literal("Couldn’t load reactions");
constexpr Utf8View kEmpty = utf8_literal("No reactions");
constexpr Utf8View kTruncated = utf8_literal("Some reactions aren't shown");
constexpr Utf8View kApp = utf8_literal("APP");

} // namespace

bool reaction_viewer_presentation_build(const ReactionViewer& viewer, const Theme& theme,
                                        PaintStream* paint) noexcept {
    if (!viewer.active) {
        return true;
    }
    if (!paint || !ui_rect_has_area(viewer.viewport) || !ui_rect_has_area(viewer.panel)) {
        return false;
    }
    UiPaint p{paint, theme};
    p.clip(viewer.viewport)
        .fill(viewer.viewport, theme.tokens.overlay.dense)
        .use<ui_surface_paint>(viewer.panel, theme, UiElevation::dialog, theme.scale.radius_md, nullptr,
                               &theme.interactive)
        .separator({viewer.panel.left, viewer.header.bottom, viewer.panel.right, viewer.header.bottom + 1.0f})
        .clip(viewer.body);
    const Color hover = ui_background_hover_color(theme, theme.surface_high);
    for (u32 index = viewer.first_aggregate; p && index < viewer.aggregate_count; ++index) {
        const UiRect row = reaction_viewer_aggregate_rectangle(viewer, index);
        if (!ui_rect_has_area(row)) {
            break;
        }
        if (index == viewer.selected_aggregate || index == viewer.hovered_aggregate)
            p.rounded_border(row, hover, hover, 4.0f, 0.0f);
    }
    for (u32 index = viewer.first_user; p && index < viewer.user_count; ++index) {
        const UiRect row = reaction_viewer_user_rectangle(viewer, index);
        if (!ui_rect_has_area(row)) {
            break;
        }
        if (index == viewer.hovered_user)
            p.rounded_border(row, hover, hover, 4.0f, 0.0f);
        const float avatar_extent = 32.0f;
        const float avatar_top = (row.top + row.bottom - avatar_extent) * 0.5f;
        p.rounded_border({row.left, avatar_top, row.left + avatar_extent, avatar_top + avatar_extent}, theme.interactive,
                         theme.interactive, avatar_extent * 0.5f, 0.0f);
        if ((viewer.users[index].flags & reaction_viewer_user_bot) != 0) {
            // The APP tag: 16 DIPs of the brand fill (badge.hpp).
            p.rounded({row.right - 42.0f, (row.top + row.bottom) * 0.5f - 8.0f, row.right,
                       (row.top + row.bottom) * 0.5f + 8.0f},
                      theme.tokens.accent.app_badge, theme.scale.radius_sm);
        }
    }
    return p.end();
}

bool reaction_viewer_text_presentation_build_output(const ReactionViewer& viewer, const Theme& theme,
                                                    UiTextOutput* output) noexcept {
    if (!viewer.active) {
        return true;
    }
    if (!viewer.text_owner.manager) {
        return false;
    }
    UiTextPen<UiTextClip::label> text{output, *viewer.text_owner.manager, kUiNoRect};
    const auto owner = viewer.text_owner.handle;
    text.in(viewer.panel)(owner, kTitle,
                          UiRect{viewer.header.left + 24.0f, viewer.header.top, viewer.close_button.left - 8.0f,
                                 viewer.header.bottom},
                          theme.text_primary, text_style_heading_3 | text_style_bold)(
        owner, kClose, viewer.close_button, theme.text_primary, text_style_heading_3, ui_text_center);
    auto body = text.in(viewer.body);
    for (u32 index = viewer.first_aggregate; text.ok() && index < viewer.aggregate_count; ++index) {
        const UiRect row = reaction_viewer_aggregate_rectangle(viewer, index);
        if (!ui_rect_has_area(row)) {
            break;
        }
        const ReactionViewerAggregate& aggregate = viewer.aggregates[index];
        const Utf8View name = reaction_viewer_aggregate_name(viewer, index);
        if (name.bytes == 0) {
            text.fail();
            break;
        }
        if (aggregate.emoji_id == 0 && !regional_flag_sequence(name))
            body(owner, name, UiRect{row.left + 4.0f, row.top, row.left + 36.0f, row.bottom}, theme.text_primary,
                 text_style_picker_emoji, ui_text_center);
        body(owner, Utf8View{aggregate.count_text, aggregate.count_text_bytes},
             UiRect{row.left + 40.0f, row.top, row.right - 4.0f, row.bottom}, ui_background_muted_text_color(theme),
             text_style_subtext);
    }
    for (u32 index = viewer.first_user; text.ok() && index < viewer.user_count; ++index) {
        const UiRect row = reaction_viewer_user_rectangle(viewer, index);
        if (!ui_rect_has_area(row)) {
            break;
        }
        const ReactionViewerUser& user = viewer.users[index];
        const float text_left = row.left + 44.0f;
        const bool bot = (user.flags & reaction_viewer_user_bot) != 0;
        const float text_right = bot ? row.right - 50.0f : row.right;
        body(owner, reaction_viewer_user_name(viewer, user), UiRect{text_left, row.top + 2.0f, text_right, row.top + 24.0f},
             theme.text_primary, text_style_bold);
        body(owner, reaction_viewer_user_username(viewer, user),
             UiRect{text_left, row.top + 22.0f, text_right, row.bottom - 1.0f}, ui_background_muted_text_color(theme),
             text_style_subtext);
        if (bot)
            body(owner, kApp, UiRect{row.right - 42.0f, row.top, row.right, row.bottom}, theme.tokens.text.on_accent,
                 text_style_subtext | text_style_bold, ui_text_center);
    }
    const Utf8View status = viewer.failed                               ? kFailed
                            : viewer.loading                            ? kLoading
                            : viewer.truncated                          ? kTruncated
                            : viewer.complete && viewer.user_count == 0 ? kEmpty
                                                                        : Utf8View{};
    if (status.bytes != 0) {
        const float right_left =
            ui_rect_width(viewer.panel) < 400.0f ? viewer.panel.left + 112.0f : viewer.panel.left + 138.0f;
        body(owner, status,
             UiRect{right_left, viewer.body.top + 4.0f, viewer.panel.right - 24.0f, viewer.body.top + 44.0f},
             viewer.failed ? theme.notification : ui_background_muted_text_color(theme), text_style_subtext,
             ui_text_center);
    }
    return text.ok();
}
} // namespace maicord
