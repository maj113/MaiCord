#include "maicord/ui/unread_banner_scene.hpp"
#include "maicord/ui/surface_elevation.hpp"
#include "maicord/ui/icon.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
constexpr Utf8View kMarkRead = utf8_literal("Mark as read");
constexpr Utf8View kJumpPresent = utf8_literal("Jump to present");
constexpr Utf8View kOlderMessages = utf8_literal("You're viewing older messages");
constexpr UiMotionStyle kMotion{kUiUnreadBannerMotionMs, UiMotionCurve::ease_out};

bool append(Slice<char> output, u32* used, Utf8View text) noexcept {
    if (text.bytes > output.count - *used)
        return false;
    copy_bytes(output.data + *used, text.data, text.bytes);
    *used += text.bytes;
    return true;
}
bool append_u32(Slice<char> output, u32* used, u32 value) noexcept {
    char digits[10];
    u32 count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % 10);
        value /= 10;
    } while (value);
    if (count > output.count - *used)
        return false;
    while (count)
        output.data[(*used)++] = digits[--count];
    return true;
}
Color faded(Color color, float level) noexcept {
    color.alpha *= level;
    return color;
}
// Notice card text: a bold title line over the wrapped description (up to
// four lines). Narrow cards move the actions to their own row below.
constexpr float kNoticeTitleLine = 22, kNoticeTextLine = 18, kNoticePadding = 10, kNoticeTextLeft = 46,
                kNoticeActionHeight = 32, kNoticeStackWidth = 520, kNoticeTextEm = 13.5f;
float notice_width(const UiUnreadBannerScene& s) noexcept {
    const float inset = minimum(s.style.banner_inset, ui_rect_width(s.viewport) * .1f);
    return maximum(0.f, ui_rect_width(s.viewport) - inset * 2);
}
bool notice_actions(const UiUnreadBannerScene& s) noexcept {
    return s.notice_primary_bytes || s.notice_secondary_bytes;
}
bool notice_stacked(const UiUnreadBannerScene& s) noexcept {
    return notice_actions(s) && notice_width(s) < kNoticeStackWidth;
}
float notice_button(const UiUnreadBannerScene& s) noexcept {
    return minimum(s.style.notice_button, notice_width(s) * .22f);
}
// Right edge of the text column, relative to the card's left edge.
float notice_text_right(const UiUnreadBannerScene& s) noexcept {
    const float width = notice_width(s), button = notice_button(s);
    if (notice_stacked(s) || !notice_actions(s))
        return width - 12;
    const u32 buttons = (s.notice_primary_bytes ? 1u : 0u) + (s.notice_secondary_bytes ? 1u : 0u);
    return width - 12 - button * static_cast<float>(buttons) - 8 * static_cast<float>(buttons - 1) - 12;
}
u32 notice_lines(const UiUnreadBannerScene& s) noexcept {
    if (!s.notice_text_bytes)
        return 0;
    const u32 lines = ui_settings_text_lines({s.notice_text, s.notice_text_bytes}, kNoticeTextEm,
                                             maximum(1.f, notice_text_right(s) - kNoticeTextLeft));
    return lines < 1 ? 1 : lines > 4 ? 4 : lines;
}
float notice_height(const UiUnreadBannerScene& s) noexcept {
    const float text = kNoticePadding * 2 + kNoticeTitleLine + kNoticeTextLine * static_cast<float>(notice_lines(s));
    return maximum(s.style.notice_height, text + (notice_stacked(s) ? kNoticeActionHeight + kNoticePadding : 0));
}
// Geometry slides by the remaining distance; interaction waits for settle.
float notice_extent(const UiUnreadBannerScene& s) noexcept {
    return s.notice_id || s.notice_motion.value > 0 ? (notice_height(s) + s.style.notice_gap * 2) * s.notice_motion.value
                                                    : 0;
}
UiRect notice_rectangle(const UiUnreadBannerScene& s) noexcept {
    const auto& st = s.style;
    const float level = s.notice_motion.value, height = notice_height(s);
    const float inset = minimum(st.banner_inset, ui_rect_width(s.viewport) * .1f);
    const float top = s.viewport.top + st.notice_gap - (height + st.notice_gap) * (1.f - level);
    return {s.viewport.left + inset, top, s.viewport.right - inset, top + height};
}
u8 copy_text(char* output, u32 capacity, Utf8View text) noexcept {
    if (!text.data || !utf8_validate(text).valid)
        return 0;
    u32 bytes = text.bytes < capacity ? text.bytes : capacity;
    while (bytes && bytes < text.bytes && (static_cast<u8>(text.data[bytes]) & 0xC0) == 0x80)
        --bytes;
    copy_bytes(output, text.data, bytes);
    return static_cast<u8>(bytes);
}
UiRect banner_rectangle(const UiUnreadBannerScene& s) noexcept {
    const auto& st = s.style;
    const float level = s.banner_motion.value;
    const float top = s.viewport.top + notice_extent(s) - st.banner_height * (1.f - level);
    const float inset = minimum(st.banner_inset, ui_rect_width(s.viewport) * .1f);
    return {s.viewport.left + inset, top, s.viewport.right - inset, top + st.banner_height};
}
UiRect pill_rectangle(const UiUnreadBannerScene& s) noexcept {
    const auto& st = s.style;
    const float level = s.pill_motion.value;
    const float width = minimum(st.pill_width, ui_rect_width(s.viewport) - 32);
    const float center = (s.viewport.left + s.viewport.right) * .5f;
    const float bottom = s.viewport.bottom - st.pill_bottom * level + st.pill_height * (1.f - level);
    return {center - width * .5f, bottom - st.pill_height, center + width * .5f, bottom};
}
bool owner_valid(UiWidgetReference owner) noexcept {
    return owner.manager && ui_widget_manager_resolve(*owner.manager, owner.handle);
}
} // namespace

u32 ui_unread_banner_format_label(u32 count, bool estimated, Utf8View since, Slice<char> output) noexcept {
    char text[kUiUnreadBannerLabelBytes];
    const Slice<char> scratch{text, sizeof(text)};
    u32 used = 0;
    const bool written =
        count && append_u32(scratch, &used, count) && (!estimated || append(scratch, &used, utf8_literal("+"))) &&
        append(scratch, &used, count == 1 && !estimated ? utf8_literal(" new message") : utf8_literal(" new messages")) &&
        (!since.bytes || (append(scratch, &used, utf8_literal(" since ")) && append(scratch, &used, since)));
    if (!written || !output.data || used > output.count)
        return 0;
    copy_bytes(output.data, text, used);
    return used;
}

bool ui_unread_banner_set_notice(UiUnreadBannerScene* scene, const UiBannerNotice* notice) noexcept {
    if (!scene)
        return false;
    const UiBannerNotice none{};
    const auto& n = notice && notice->id && notice->title.bytes ? *notice : none;
    char title[kUiBannerNoticeTitleBytes], text[kUiBannerNoticeTextBytes];
    char primary[kUiBannerNoticeActionBytes], secondary[kUiBannerNoticeActionBytes];
    const u8 title_bytes = copy_text(title, sizeof(title), n.title);
    const u8 text_bytes = copy_text(text, sizeof(text), n.description);
    const u8 primary_bytes = copy_text(primary, sizeof(primary), n.primary);
    const u8 secondary_bytes = copy_text(secondary, sizeof(secondary), n.secondary);
    // An outgoing card keeps its text while it slides away.
    if (!n.id && scene->notice_motion.value > 0) {
        const bool changed = scene->notice_id != 0;
        scene->notice_id = 0;
        if (changed)
            ++scene->revision;
        return changed;
    }
    const bool changed = scene->notice_id != n.id || scene->notice_warning != n.warning || scene->notice_busy != n.busy ||
                         scene->notice_title_bytes != title_bytes || scene->notice_text_bytes != text_bytes ||
                         scene->notice_primary_bytes != primary_bytes || scene->notice_secondary_bytes != secondary_bytes ||
                         !bytes_equal(scene->notice_title, title, title_bytes) ||
                         !bytes_equal(scene->notice_text, text, text_bytes) ||
                         !bytes_equal(scene->notice_primary, primary, primary_bytes) ||
                         !bytes_equal(scene->notice_secondary, secondary, secondary_bytes);
    if (!changed)
        return false;
    scene->notice_id = n.id;
    scene->notice_warning = n.warning;
    scene->notice_busy = n.busy;
    copy_bytes(scene->notice_title, title, title_bytes);
    copy_bytes(scene->notice_text, text, text_bytes);
    copy_bytes(scene->notice_primary, primary, primary_bytes);
    copy_bytes(scene->notice_secondary, secondary, secondary_bytes);
    scene->notice_title_bytes = title_bytes;
    scene->notice_text_bytes = text_bytes;
    scene->notice_primary_bytes = primary_bytes;
    scene->notice_secondary_bytes = secondary_bytes;
    ++scene->revision;
    return true;
}

void ui_unread_banner_clear(UiUnreadBannerScene* scene) noexcept {
    if (!scene)
        return;
    const u32 revision = scene->revision + 1;
    const auto style = scene->style;
    *scene = {};
    scene->style = style;
    scene->revision = revision;
}

bool ui_unread_banner_update(UiUnreadBannerScene* scene, const UiUnreadBannerView& view, Utf8View label,
                             UiRect viewport, UiWidgetReference text_owner, u64 now_ms, bool animate) noexcept {
    if (!scene)
        return false;
    const bool usable = ui_rect_valid(viewport) && ui_rect_width(viewport) >= 160 &&
                        ui_rect_height(viewport) >= 96 && owner_valid(text_owner) && view.channel_id;
    UiUnreadBannerView next = usable ? view : UiUnreadBannerView{};
    if (!label.bytes || label.bytes > kUiUnreadBannerLabelBytes || !utf8_validate(label).valid)
        next.banner = false;
    // A channel switch starts from hidden instead of sliding stale content.
    const UiMotionScope scope{next.channel_id ? next.channel_id : 1, 1};
    if (scene->view.channel_id != next.channel_id) {
        ui_motion_transition_reset(&scene->banner_motion, scope, 0);
        ui_motion_transition_reset(&scene->pill_motion, scope, 0);
        ui_motion_transition_reset(&scene->notice_motion, scope, 0);
    }
    const bool notice = usable && scene->notice_id != 0;
    bool changed_notice = ui_motion_transition_tick(&scene->notice_motion, scope, notice ? 1.f : 0.f, now_ms, kMotion,
                                                    !animate, true);
    bool changed = ui_motion_transition_tick(&scene->banner_motion, scope, next.banner ? 1.f : 0.f, now_ms, kMotion,
                                             !animate, true) || changed_notice;
    changed = ui_motion_transition_tick(&scene->pill_motion, scope, next.jump ? 1.f : 0.f, now_ms, kMotion,
                                        !animate, true) || changed;
    // Keep the outgoing label while the banner slides away.
    const bool keep_label = !next.banner && scene->banner_motion.value > 0;
    if (!keep_label) {
        const u16 bytes = next.banner ? static_cast<u16>(label.bytes) : 0;
        if (bytes != scene->label_bytes || !bytes_equal(scene->label, label.data, bytes)) {
            copy_bytes(scene->label, label.data, bytes);
            scene->label_bytes = bytes;
            changed = true;
        }
    }
    // Compare the stored (possibly cleared) forms. Comparing the raw inputs and
    // byte-comparing the padded view reported a change on every frame while the
    // banner was unusable (login, settings), so each present requested another.
    const UiRect stored_viewport = usable ? viewport : UiRect{};
    const UiWidgetReference stored_owner = usable ? text_owner : UiWidgetReference{};
    changed = changed || !ui_rect_equal(scene->viewport, stored_viewport) ||
              scene->text_owner.manager != stored_owner.manager ||
              scene->text_owner.handle.slot != stored_owner.handle.slot ||
              scene->text_owner.handle.generation != stored_owner.handle.generation ||
              scene->view.channel_id != next.channel_id || scene->view.since_unix_ms != next.since_unix_ms ||
              scene->view.count != next.count || scene->view.count_estimated != next.count_estimated ||
              scene->view.banner != next.banner || scene->view.jump != next.jump;
    const u64 since = keep_label ? scene->view.since_unix_ms : next.since_unix_ms;
    scene->view = next;
    scene->view.since_unix_ms = since;
    scene->viewport = stored_viewport;
    scene->text_owner = stored_owner;
    if (!usable) {
        scene->banner = scene->banner_label = scene->banner_action = scene->pill = {};
        scene->notice = scene->notice_icon = scene->notice_title_rect = scene->notice_text_rect = {};
        scene->notice_primary_rect = scene->notice_secondary_rect = {};
    } else {
        scene->notice = notice_rectangle(*scene);
        const auto r = scene->notice;
        const bool stacked = notice_stacked(*scene);
        const float half = kNoticeActionHeight * .5f;
        // Side actions centre on the card; stacked actions share the row under the text.
        const float middle = stacked ? r.bottom - kNoticePadding - half : (r.top + r.bottom) * .5f;
        float button = notice_button(*scene);
        if (stacked) {
            const float row = r.right - 12 - (r.left + kNoticeTextLeft);
            button = scene->notice_primary_bytes && scene->notice_secondary_bytes ? (row - 8) * .5f : row;
        }
        scene->notice_secondary_rect = scene->notice_secondary_bytes
                                           ? UiRect{r.right - 12 - button, middle - half, r.right - 12, middle + half}
                                           : UiRect{};
        const float primary_right = scene->notice_secondary_bytes ? scene->notice_secondary_rect.left - 8 : r.right - 12;
        scene->notice_primary_rect = scene->notice_primary_bytes
                                         ? UiRect{primary_right - button, middle - half, primary_right, middle + half}
                                         : UiRect{};
        const float text_right = r.left + notice_text_right(*scene);
        const float title_top = r.top + kNoticePadding, text_top = title_top + kNoticeTitleLine;
        const float icon_middle = stacked ? title_top + kNoticeTitleLine * .5f : (r.top + r.bottom) * .5f;
        scene->notice_icon = {r.left + 14, icon_middle - 10, r.left + 34, icon_middle + 10};
        scene->notice_title_rect = {r.left + kNoticeTextLeft, title_top, text_right, text_top};
        scene->notice_text_rect = {r.left + kNoticeTextLeft, text_top, text_right,
                                   text_top + kNoticeTextLine * static_cast<float>(maximum(1u, notice_lines(*scene)))};
        const auto& st = scene->style;
        scene->banner = banner_rectangle(*scene);
        const float action = minimum(st.action_width, ui_rect_width(scene->banner) * .4f);
        scene->banner_action = {scene->banner.right - action, scene->banner.top, scene->banner.right,
                                scene->banner.bottom};
        scene->banner_label = {scene->banner.left + 12, scene->banner.top, scene->banner_action.left - 8,
                               scene->banner.bottom};
        scene->pill = pill_rectangle(*scene);
    }
    if (!ui_unread_banner_visible(*scene))
        scene->hovered = UiUnreadBannerAction::none;
    if (changed)
        ++scene->revision;
    return changed;
}

u64 ui_unread_banner_next_wake(const UiUnreadBannerScene& scene, u64 now_ms, bool animate) noexcept {
    const u64 a = ui_motion_transition_next_wake(scene.banner_motion, scene.banner_motion.scope, now_ms, !animate, true);
    const u64 b = ui_motion_transition_next_wake(scene.pill_motion, scene.pill_motion.scope, now_ms, !animate, true);
    const u64 c = ui_motion_transition_next_wake(scene.notice_motion, scene.notice_motion.scope, now_ms, !animate, true);
    const u64 ab = !a ? b : !b ? a : minimum(a, b);
    return !ab ? c : !c ? ab : minimum(ab, c);
}

bool ui_unread_banner_visible(const UiUnreadBannerScene& scene) noexcept {
    return owner_valid(scene.text_owner) && ui_rect_has_area(scene.viewport) &&
           (scene.banner_motion.value > 0 || scene.pill_motion.value > 0 || scene.notice_motion.value > 0);
}

void ui_unread_banner_occluders(const UiUnreadBannerScene& scene, UiRect (&occluders)[2]) noexcept {
    occluders[0] = occluders[1] = UiRect{};
    if (!ui_unread_banner_visible(scene))
        return;
    if (scene.banner_motion.value > 0 && scene.label_bytes)
        occluders[0] = ui_rect_intersection(scene.banner, scene.viewport);
    if (scene.notice_motion.value > 0 && scene.notice_title_bytes) {
        // The card and the bar below it stack; one occluder spans both.
        const auto card = ui_rect_intersection(scene.notice, scene.viewport);
        occluders[0] = ui_rect_has_area(occluders[0]) ? UiRect{minimum(card.left, occluders[0].left), card.top,
                                                               maximum(card.right, occluders[0].right),
                                                               maximum(card.bottom, occluders[0].bottom)}
                                                      : card;
    }
    if (scene.pill_motion.value > 0)
        occluders[1] = ui_rect_intersection(scene.pill, scene.viewport);
    for (auto& occluder : occluders)
        if (!ui_rect_has_area(occluder))
            occluder = UiRect{};
}

UiUnreadBannerAction ui_unread_banner_hit_test(const UiUnreadBannerScene& scene, float x, float y) noexcept {
    if (!ui_unread_banner_visible(scene) || !ui_rect_contains(scene.viewport, x, y))
        return UiUnreadBannerAction::none;
    if (scene.notice_id && scene.notice_motion.value >= 1 && ui_rect_contains(scene.notice, x, y)) {
        if (scene.notice_busy)
            return UiUnreadBannerAction::none;
        if (scene.notice_primary_bytes && ui_rect_contains(scene.notice_primary_rect, x, y))
            return UiUnreadBannerAction::notice_primary;
        if (scene.notice_secondary_bytes && ui_rect_contains(scene.notice_secondary_rect, x, y))
            return UiUnreadBannerAction::notice_secondary;
        return UiUnreadBannerAction::none;
    }
    if (scene.view.banner && scene.banner_motion.value >= 1 && ui_rect_contains(scene.banner, x, y))
        return ui_rect_contains(scene.banner_action, x, y) ? UiUnreadBannerAction::mark_read
                                                           : UiUnreadBannerAction::jump_to_unread;
    if (scene.view.jump && scene.pill_motion.value >= 1 && ui_rect_contains(scene.pill, x, y))
        return UiUnreadBannerAction::jump_to_present;
    return UiUnreadBannerAction::none;
}

bool ui_unread_banner_set_hover(UiUnreadBannerScene* scene, UiUnreadBannerAction action) noexcept {
    if (!scene || scene->hovered == action)
        return false;
    scene->hovered = action;
    ++scene->revision;
    return true;
}

bool ui_unread_banner_paint(const UiUnreadBannerScene& scene, const Theme& theme, PaintStream* paint) noexcept {
    if (!ui_unread_banner_visible(scene))
        return true;
    const auto v = scene.viewport;
    if (!paint || !paint_stream_push_clip(paint, v.left, v.top, v.right, v.bottom))
        return false;
    bool ok = true;
    const auto& st = scene.style;
    if (scene.notice_motion.value > 0 && scene.notice_title_bytes) {
        const float level = scene.notice_motion.value;
        const auto r = scene.notice;
        const auto surface = ui_surface_style(theme, UiElevation::banner);
        const Color fill = faded(surface.fill, level), border = faded(surface.border, level);
        const Color tone = faded(scene.notice_warning ? theme.tokens.accent.warning : theme.tokens.emphasis.outline,
                                 level);
        ok = paint_stream_record_content(paint, scene.text_owner, r, UiLayoutContentKind::control,
                                         ui_layout_content_bounds_allow_overlap) &&
             paint_stream_push_rounded(paint, r.left, r.top + 2, r.right, r.bottom + 2,
                                       ui_theme_color_alpha(theme.tokens.overlay.shadow_soft, level), st.notice_radius) &&
             ui_surface_paint(paint, r, theme, UiElevation::banner, st.notice_radius, &fill, &border) &&
             // A tone stripe along the leading edge marks warnings.
             paint_stream_push_rounded(paint, r.left, r.top, r.left + 4, r.bottom, tone, theme.scale.radius_xs) &&
             ui_icon_paint(paint, scene.notice_warning ? UiIcon::shield_alert : UiIcon::inbox, scene.notice_icon, tone);
        const bool busy = scene.notice_busy;
        if (ok && scene.notice_primary_bytes) {
            const auto b = scene.notice_primary_rect;
            Color button = faded(theme.tokens.emphasis.fill, level * (busy ? .5f : 1.f));
            ok = paint_stream_push_rounded(paint, b.left, b.top, b.right, b.bottom, button, theme.scale.radius_sm) &&
                 (scene.hovered != UiUnreadBannerAction::notice_primary ||
                  paint_stream_push_rounded(paint, b.left, b.top, b.right, b.bottom,
                                            faded(theme.tokens.state.hover, level), theme.scale.radius_sm));
        }
        if (ok && scene.notice_secondary_bytes) {
            const auto b = scene.notice_secondary_rect;
            ok = paint_stream_push_rounded(paint, b.left, b.top, b.right, b.bottom,
                                           faded(theme.tokens.fill.subtle, level * (busy ? .5f : 1.f)),
                                           theme.scale.radius_sm) &&
                 (scene.hovered != UiUnreadBannerAction::notice_secondary ||
                  paint_stream_push_rounded(paint, b.left, b.top, b.right, b.bottom,
                                            faded(theme.tokens.state.hover, level), theme.scale.radius_sm));
        }
    }
    if (ok && scene.banner_motion.value > 0 && scene.label_bytes) {
        const float level = scene.banner_motion.value;
        const auto r = scene.banner;
        // The unread bar is an emphasis fill: gradient-derived on gradient themes.
        const Color fill = faded(theme.tokens.emphasis.fill, level);
        const Color text = faded(theme.tokens.emphasis.text, level);
        const float icon_top = (r.top + r.bottom - st.icon) * .5f;
        const UiRect icon{scene.banner_action.right - 10 - st.icon, icon_top, scene.banner_action.right - 10,
                          icon_top + st.icon};
        ok = paint_stream_record_content(paint, scene.text_owner, r, UiLayoutContentKind::decoration,
                                         ui_layout_content_bounds_allow_overlap) &&
             // Square top edge tucks under the channel header; only the bottom is rounded.
             paint_stream_push(paint, r.left, r.top, r.right, r.top + st.banner_radius, fill) &&
             paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom, fill, st.banner_radius) &&
             (scene.hovered != UiUnreadBannerAction::mark_read ||
              paint_stream_push_rounded(paint, scene.banner_action.left, r.top, r.right, r.bottom,
                                        {text.red, text.green, text.blue, .14f * level}, st.banner_radius)) &&
             ui_icon_paint(paint, UiIcon::check, icon, text);
    }
    if (ok && scene.pill_motion.value > 0) {
        const float level = scene.pill_motion.value;
        const auto r = scene.pill;
        const bool hovered = scene.hovered == UiUnreadBannerAction::jump_to_present;
        // The jump pill floats over the timeline: banner elevation material,
        // with the shared hover highlight above it.
        const auto surface = ui_surface_style(theme, UiElevation::banner);
        const Color fill = faded(surface.fill, level), border = faded(surface.border, level);
        const float icon_top = (r.top + r.bottom - st.icon) * .5f;
        const UiRect icon{r.right - 14 - st.icon, icon_top, r.right - 14, icon_top + st.icon};
        ok = paint_stream_record_content(paint, scene.text_owner, r, UiLayoutContentKind::control,
                                         ui_layout_content_bounds_allow_overlap) &&
             paint_stream_push_rounded(paint, r.left, r.top + 2, r.right, r.bottom + 2, ui_theme_color_alpha(theme.tokens.overlay.shadow_soft, level),
                                       st.pill_radius) &&
             ui_surface_paint(paint, r, theme, UiElevation::banner, st.pill_radius, &fill, &border) &&
             (!hovered || paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom,
                                                    faded(theme.tokens.state.hover, level), st.pill_radius)) &&
             ui_icon_paint(paint, UiIcon::arrow_down, icon, faded(theme.tokens.emphasis.outline, level));
    }
    return paint_stream_pop_clip(paint) && ok;
}

bool ui_unread_banner_text_output(const UiUnreadBannerScene& scene, const Theme& theme,
                                  UiTextOutput* output) noexcept {
    if (!ui_unread_banner_visible(scene))
        return true;
    const auto& st = scene.style;
    if (scene.notice_motion.value > 0 && scene.notice_title_bytes) {
        const float level = scene.notice_motion.value;
        const auto clip = ui_rect_intersection(scene.viewport, scene.notice);
        const Color primary_text = faded(theme.tokens.emphasis.text, level * (scene.notice_busy ? .6f : 1.f));
        const Color secondary_text = faded(theme.text_primary, level * (scene.notice_busy ? .6f : 1.f));
        if (!ui_text_output_push(output, {{scene.notice_title, scene.notice_title_bytes}, scene.text_owner,
                                          scene.notice_title_rect, clip, faded(theme.text_primary, level),
                                          text_style_bold, UiTextAlignment::start, UiTextAlignment::center,
                                          UiTextWrapping::no_wrap, UiTextOverflow::ellipsis}) ||
            (scene.notice_text_bytes &&
             !ui_text_output_push(output, {{scene.notice_text, scene.notice_text_bytes}, scene.text_owner,
                                           scene.notice_text_rect, clip, faded(theme.text_secondary, level),
                                           text_style_subtext, UiTextAlignment::start, UiTextAlignment::start,
                                           UiTextWrapping::wrap, UiTextOverflow::ellipsis})) ||
            (scene.notice_primary_bytes &&
             !ui_text_output_push(output, {{scene.notice_primary, scene.notice_primary_bytes}, scene.text_owner,
                                           scene.notice_primary_rect, clip, primary_text,
                                           text_style_subtext | text_style_bold, UiTextAlignment::center,
                                           UiTextAlignment::center, UiTextWrapping::no_wrap,
                                           UiTextOverflow::ellipsis})) ||
            (scene.notice_secondary_bytes &&
             !ui_text_output_push(output, {{scene.notice_secondary, scene.notice_secondary_bytes}, scene.text_owner,
                                           scene.notice_secondary_rect, clip, secondary_text,
                                           text_style_subtext | text_style_bold, UiTextAlignment::center,
                                           UiTextAlignment::center, UiTextWrapping::no_wrap,
                                           UiTextOverflow::ellipsis})))
            return false;
    }
    if (scene.banner_motion.value > 0 && scene.label_bytes) {
        const float level = scene.banner_motion.value;
        const Color text = faded(theme.tokens.emphasis.text, level);
        const auto action = scene.banner_action;
        if (!ui_text_output_push(output, {{scene.label, scene.label_bytes}, scene.text_owner, scene.banner_label,
                                          scene.viewport, text, text_style_subtext | text_style_bold,
                                          UiTextAlignment::start, UiTextAlignment::center, UiTextWrapping::no_wrap,
                                          UiTextOverflow::ellipsis}) ||
            !ui_text_output_push(output, {kMarkRead, scene.text_owner,
                                          {action.left, action.top, action.right - 16 - st.icon, action.bottom},
                                          scene.viewport, text, text_style_subtext | text_style_bold,
                                          UiTextAlignment::end, UiTextAlignment::center, UiTextWrapping::no_wrap,
                                          UiTextOverflow::ellipsis}))
            return false;
    }
    if (scene.pill_motion.value > 0) {
        const auto r = scene.pill;
        const float level = scene.pill_motion.value;
        const float action = minimum(st.pill_action, ui_rect_width(r) * .5f);
        return ui_text_output_push(output, {kOlderMessages, scene.text_owner,
                                            {r.left + 16, r.top, r.right - action - 8, r.bottom}, scene.viewport,
                                            faded(theme.text_secondary, level), text_style_subtext,
                                            UiTextAlignment::start, UiTextAlignment::center,
                                            UiTextWrapping::no_wrap, UiTextOverflow::ellipsis}) &&
               ui_text_output_push(output, {kJumpPresent, scene.text_owner,
                                            {r.right - action, r.top, r.right - 18 - st.icon, r.bottom},
                                            scene.viewport, faded(theme.text_primary, level),
                                            text_style_subtext | text_style_bold, UiTextAlignment::end,
                                            UiTextAlignment::center, UiTextWrapping::no_wrap,
                                            UiTextOverflow::ellipsis});
    }
    return true;
}

} // namespace maicord
