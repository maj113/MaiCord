#include "maicord/ui/theme/style_projection.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/core/transition.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/ui/theme/styles.hpp"
#include "scene_internal.hpp"
#include "maicord/ui/widgets/icon.hpp"

namespace maicord {
using namespace settings_scene_detail;
namespace {
// Keyboard zones in Tab order.
constexpr UiSettingsFocusZone kFocusZones[]{UiSettingsFocusZone::search, UiSettingsFocusZone::sidebar,
                                            UiSettingsFocusZone::panel, UiSettingsFocusZone::close};
// The official client's segmented control has no transition (its plate jumps); MaiCord slides it on the hover
// fade's duration with an ease-out curve.
constexpr UiMotionStyle kSegmentMotion{170, UiMotionCurve::css_ease_out};
constexpr UiMotionScope kSegmentScope{1, 1};
constexpr u8 kSegmentNone = 0xFF;
// Remembers which option each plain segments row of the page shows selected. A row whose option changed since the
// last projection starts its plate sliding from the one it left (one row at a time; none under reduced motion).
__forceinline void track_segments(UiSettingsScene* scene, bool keep) noexcept {
    auto& segment = scene->segment;
    const auto& controls = scene->content.controls;
    u64 ids[kUiSettingsSegmentMemory];
    u8 choices[kUiSettingsSegmentMemory];
    u32 count = 0;
    if (!keep)
        segment.control_id = 0;
    for (u32 i = 0; i < controls.count && count < kUiSettingsSegmentMemory; ++i) {
        const auto& c = controls[i];
        if (c.kind != UiSettingsControlKind::segments || has_flag(scene->content, i, ui_settings_presentation_tristate))
            continue;
        const u8 choice = c.selected_choice < minimum<u32>(c.choices.count, kSegmentNone)
                              ? static_cast<u8>(c.selected_choice)
                              : kSegmentNone;
        for (u32 k = 0; keep && k < segment.memory_count; ++k)
            if (segment.memory_ids[k] == c.id) {
                const u8 before = segment.memory_choices[k];
                if (before != choice && before != kSegmentNone && choice != kSegmentNone && !scene->reduced_motion) {
                    segment.control_id = c.id;
                    segment.from = before;
                    ui_motion_transition_reset(&segment.transition, kSegmentScope, 0);
                }
                break;
            }
        ids[count] = c.id;
        choices[count++] = choice;
    }
    copy_bytes(segment.memory_ids, ids, count * static_cast<u32>(sizeof(u64)));
    copy_bytes(segment.memory_choices, choices, count);
    segment.memory_count = static_cast<u8>(count);
}
__forceinline bool option_kind(UiSettingsControlKind kind) noexcept {
    return kind == UiSettingsControlKind::segments || kind == UiSettingsControlKind::swatches ||
           kind == UiSettingsControlKind::tabs || kind == UiSettingsControlKind::buttons ||
           kind == UiSettingsControlKind::colors;
}
// The part of a list row under a point (UiSettingsListPart), or ~0u.
__forceinline u32 list_part(const UiSettingsLayoutTokens& t, const UiSettingsPageContent& content, u32 index, const UiRect& row, float x,
              float y) noexcept {
    const auto* item = list_item(content, index);
    if (!item || !ui_rect_contains(row, x, y))
        return ~0u;
    if (item->style != UiSettingsListStyle::row)
        return ui_settings_list_row;
    const auto parts = list_parts(t, content, index, row);
    for (u32 i = 0; i < kUiSettingsListActions; ++i)
        if (item->actions[i] != UiIcon::count && ui_rect_contains(parts.actions[i], x, y))
            return ui_settings_list_action + i;
    const auto& c = content.controls[index];
    if ((item->count_icon != UiIcon::count || (c.choices.count && c.choices[0].bytes)) &&
        ui_rect_contains(parts.column, x, y))
        return ui_settings_list_count;
    return ui_settings_list_row;
}
} // namespace

UiSettingsModalLayout ui_settings_modal_layout(const UiSettingsLayoutTokens& tokens, const UiRect& viewport,
                                               UiSettingsPresentation presentation, bool wide) noexcept {
    const auto& m = tokens.metric;
    UiSettingsModalLayout l{};
    const float width = ui_rect_width(viewport), height = ui_rect_height(viewport);
    if (presentation == UiSettingsPresentation::dialog) {
        // One centred column; ui_settings_dialog_fit shrinks it to the content.
        l.compact = width < m.modal_compact_width || height < 420;
        l.modal = viewport;
        if (!l.compact) {
            const float dialog_width = minimum(kUiSettingsDialogWidth, width - m.modal_inset_side * 2);
            const float left = viewport.left + (width - dialog_width) * .5f;
            const float top = viewport.top + m.modal_inset_top;
            l.modal = {left, top, left + dialog_width,
                       top + maximum(1.f, height - m.modal_inset_top - m.modal_inset_bottom)};
        }
        l.sidebar = {l.modal.left, l.modal.top, l.modal.left, l.modal.bottom};
        l.panel = l.modal;
        const float pad = kUiSettingsDialogPadding;
        // A plain close glyph sits in the top-right corner, level with the title.
        l.close = ui_settings_close_rect(tokens, l.panel.right - pad + 4 - m.close_size * .5f, l.panel.top + pad - 6);
        l.column = {l.panel.left + pad, l.panel.top + pad, maximum(l.panel.left + pad + 1, l.panel.right - pad),
                    l.panel.bottom};
        return l;
    }
    if (presentation == UiSettingsPresentation::modal) {
        l.compact = width < m.modal_compact_width || height < 420;
        l.modal = viewport;
        if (!l.compact) {
            const float available_width = width - m.modal_inset_side * 2;
            const float available_height = height - m.modal_inset_top - m.modal_inset_bottom;
            float modal_width = available_width;
            if (width >= m.modal_ratio_threshold)
                modal_width = maximum(width * m.modal_viewport_ratio, m.modal_ratio_threshold - m.modal_inset_side * 2);
            modal_width = minimum(minimum(modal_width, available_width), m.modal_max_width);
            const float left = viewport.left + (width - modal_width) * .5f;
            const float top = viewport.top + m.modal_inset_top;
            l.modal = {left, top, left + modal_width, top + maximum(1.f, available_height)};
        }
        const float sidebar = minimum(m.sidebar_width, ui_rect_width(l.modal) * (l.compact ? .32f : .36f));
        l.sidebar = {l.modal.left, l.modal.top, l.modal.left + sidebar, l.modal.bottom};
        l.panel = {l.sidebar.right, l.modal.top, l.modal.right, l.modal.bottom};
        const float close_right = l.panel.right - 20;
        // The ghost close button sits level with the 24/30 title.
        l.close = ui_settings_close_rect(tokens, close_right - m.close_size * .5f, l.panel.top + m.panel_padding_top - 3);
        // Small windows halve the panel's side padding so the form keeps
        // room for a label beside its control.
        const float left = l.panel.left + (l.compact ? m.panel_padding_x * .5f : m.panel_padding_x);
        const float right_limit = l.close.left - 16;
        // Store pages take the room up to the close button (at most kUiSettingsWideMaximum).
        const float column_width = wide ? kUiSettingsWideMaximum : m.panel_max_width;
        l.column = {left, l.panel.top + m.panel_padding_top, maximum(left + 1, minimum(right_limit, left + column_width)),
                    l.panel.bottom};
        return l;
    }
    // Layer: growing sidebar region with a right-aligned column, then content.
    // Below sidebar + 640 DIP the sidebar, padding and close gap shrink
    // together (down to the 624 DIP minimum window) so the form column
    // keeps at least ~360 DIP instead of collapsing beside a full sidebar.
    l.modal = viewport;
    const float extra = maximum(0.f, width - m.sidebar_width - 800);
    const float squeeze = minimum(1.f, maximum(0.f, m.sidebar_width + 640 - width) / 256.f);
    const float sidebar = maximum(minimum(width * .4f, kUiSettingsLayerMinimumSidebar),
                                  minimum(width * .4f, m.sidebar_width + extra * .5f) -
                                      (m.sidebar_width - kUiSettingsLayerMinimumSidebar) * squeeze);
    l.sidebar = {viewport.left, viewport.top, viewport.left + sidebar, viewport.bottom};
    l.panel = {l.sidebar.right, viewport.top, viewport.right, viewport.bottom};
    const float padding = m.panel_padding_x * (1 - .5f * squeeze), close_gap = 24 - 8 * squeeze;
    const float left = l.panel.left + padding;
    // A wide page (a form beside its preview) takes the room up to the close button, at most kUiSettingsWideMaximum.
    const float right = minimum(l.panel.right - padding - m.close_size - close_gap,
                                left + (wide ? kUiSettingsWideMaximum : m.panel_max_width));
    l.column = {left, viewport.top + 56, maximum(left + 1, right), viewport.bottom};
    l.close = ui_settings_close_rect(tokens, l.column.right + close_gap + m.close_size * .5f, viewport.top + 56);
    return l;
}

void ui_settings_dialog_fit(UiSettingsModalLayout* layout, const UiRect& viewport, float content_height) noexcept {
    if (!layout || layout->compact || !(content_height > 0))
        return;
    const float spare = ui_rect_height(layout->modal) - content_height;
    if (spare <= 0)
        return;
    // Centre within the viewport, inside the insets of the full-height dialog.
    const float centred = viewport.top + (ui_rect_height(viewport) - content_height) * .5f;
    const float top = minimum(layout->modal.top + spare, maximum(layout->modal.top, centred));
    const float shift = top - layout->modal.top;
    const auto move = [&](UiRect& r, bool bottom_follows_modal) noexcept {
        r.top += shift;
        r.bottom = bottom_follows_modal ? top + content_height : r.bottom + shift;
    };
    move(layout->modal, true);
    move(layout->sidebar, true);
    move(layout->panel, true);
    move(layout->column, true);
    move(layout->close, false);
}

bool ui_settings_scene_bind_theme_styles(UiSettingsScene* scene, const UiResolvedThemeStyles* styles) noexcept {
    if (!scene)
        return false;
    const u32 revision = styles && styles->ready ? styles->geometry_revision : 0;
    if (scene->theme_styles == styles && scene->theme_geometry_revision == revision)
        return false;
    scene->theme_styles = styles;
    scene->theme_geometry_revision = revision;
    scene->layout_signature = 0;
    return true;
}

bool ui_settings_scene_set_presentation(UiSettingsScene* scene, UiSettingsPresentation presentation) noexcept {
    if (!scene || presentation > UiSettingsPresentation::dialog || scene->presentation == presentation)
        return false;
    scene->presentation = presentation;
    scene->layout_signature = 0;
    return true;
}

bool ui_settings_scene_bind_navigation(UiSettingsScene* scene, ConstSlice<UiSettingsNavEntry> navigation,
                                       u32 revision) noexcept {
    if (!scene || !valid_navigation(navigation))
        return false;
    if (scene->navigation.data == navigation.data && scene->navigation.count == navigation.count &&
        scene->navigation_revision == revision)
        return false;
    scene->navigation = navigation;
    scene->navigation_revision = revision;
    scene->layout_signature = 0;
    return true;
}
bool ui_settings_scene_set_navigation_note(UiSettingsScene* scene, Utf8View note) noexcept {
    if (!scene || !valid_text(note))
        return false;
    if (scene->navigation_note.data == note.data && scene->navigation_note.bytes == note.bytes)
        return false;
    scene->navigation_note = note;
    scene->layout_signature = 0;
    return true;
}

bool ui_settings_scene_set_search(UiSettingsScene* scene, const UiSettingsEditor& search) noexcept {
    if (!scene || !valid_text(search.view.text) || !valid_text(search.placeholder) || !geometry_valid(search.geometry))
        return false;
    const auto& a = scene->search;
    if (a.view.text.data == search.view.text.data && a.view.text.bytes == search.view.text.bytes &&
        a.view.anchor == search.view.anchor && a.view.caret == search.view.caret &&
        a.view.focused == search.view.focused && a.view.caret_visible == search.view.caret_visible &&
        a.geometry == search.geometry && a.placeholder.data == search.placeholder.data)
        return false;
    const bool clear_changed = (a.view.text.bytes != 0) != (search.view.text.bytes != 0);
    scene->search = search;
    if (clear_changed && scene->active)
        synchronize_input(*scene);
    ++scene->revision;
    return true;
}

bool ui_settings_scene_search_geometry(const UiSettingsScene& scene, UiSettingsControlGeometry* output) noexcept {
    if (output)
        reset_value(output);
    const auto* node = output && current(scene) && scene.search_field.generation
                           ? ui_widget_manager_layout_node(scene.widgets, scene.search_field)
                           : nullptr;
    if (!node)
        return false;
    const UiSettingsTarget target{UiSettingsTargetKind::search, {}, 0};
    *output = {target,
               {&scene.widgets, scene.search_field},
               node->rectangle,
               node->rectangle,
               ui_settings_search_text_rect(node->rectangle),
               node->clip_rectangle,
               true,
               false};
    return ui_rect_has_area(output->clip);
}

bool ui_settings_scene_set_flash(UiSettingsScene* scene, u64 control_id, float strength) noexcept {
    if (!scene)
        return false;
    strength = strength > 0 ? minimum(strength, 1.f) : 0.f;
    if (!strength)
        control_id = 0;
    if (scene->flash_control == control_id && scene->flash == strength)
        return false;
    scene->flash_control = control_id;
    scene->flash = strength;
    ++scene->motion_revision;
    return true;
}

bool ui_settings_scene_animate_toggle(UiSettingsScene* scene, const UiSettingsTarget& target, bool checked,
                                      u64 now_ms) noexcept {
    if (!scene || target.kind != UiSettingsTargetKind::control || !now_ms)
        return false;
    if (!ui_page_key_equal(target.page, scene->selected_page))
        return false;
    scene->animation = {target.control_id, now_ms, checked};
    scene->clock_ms = now_ms;
    ++scene->motion_revision;
    return true;
}

bool ui_settings_scene_tick(UiSettingsScene* scene, u64 now_ms, bool reduced_motion) noexcept {
    if (!scene)
        return false;
    scene->reduced_motion = reduced_motion;
    bool animating = false;
    const SceneTokens t(*scene);
    const u32 duration = reduced_motion ? t.duration.switch_reduced_ms : t.duration.switch_ms;
    if (scene->animation.started_ms) {
        if (now_ms >= scene->animation.started_ms + duration + kUiTransitionFrameMs)
            reset_value(&scene->animation);
        else
            animating = true;
    }
    if (scene->clock_ms != now_ms && (animating || scene->clock_ms)) {
        scene->clock_ms = animating ? now_ms : 0;
        ++scene->motion_revision;
    }
    // A segments row's plate slides to its new option; settled (or reduced motion) it rests there at once. The
    // labels' weight swaps at the halfway point and rests at the settle: those two ticks rebuild the text, the
    // others only repaint.
    if (auto& segment = scene->segment; segment.control_id) {
        const float before = segment.transition.value;
        const bool moved = ui_motion_transition_tick(&segment.transition, kSegmentScope, 1.f, now_ms, kSegmentMotion,
                                                     reduced_motion, true);
        const bool settled = !segment.transition.active;
        if (settled)
            segment.control_id = 0;
        if (settled || (before < .5f && segment.transition.value >= .5f)) {
            ++scene->revision;
            ++scene->motion_revision;
        } else if (moved)
            ++scene->motion_revision;
    }
    // Button plates ease to and from their hover fill (paint only).
    if (ui_motion_hover_tick(&scene->hover_motion, hover_motion_id(scene->widgets.hovered), now_ms, kUiMotionHover,
                             reduced_motion))
        ++scene->motion_revision;
    return animating || ui_settings_scene_animating(*scene);
}

bool ui_settings_scene_animating(const UiSettingsScene& scene) noexcept {
    return scene.animation.started_ms != 0 || scene.segment.control_id != 0 || scene.hover_motion.in.active ||
           scene.hover_motion.out.active;
}

bool ui_settings_page_content_valid(const UiSettingsPageContent& content) noexcept {
    return valid_content(content);
}
bool ui_settings_scene_update(UiSettingsScene* scene, UiPageRegistry* registry, const UiRect& viewport_view) noexcept {
    const UiRect viewport = viewport_view; // the scene is written below
    if (!scene || !registry || !ui_rect_valid(viewport) || ui_rect_width(viewport) < 320 ||
        ui_rect_height(viewport) < 200 || ui_rect_height(viewport) > 8192 || registry->count > 16384)
        return false;
    const auto* page = ui_page_registry_selected(*registry);
    if (page && !page->context)
        return false;
    // The selected page's content (borrowed), or the placeholder of a registry with none selected.
    const UiSettingsPageContent none{utf8_literal("Settings"), {}, {}, 0};
    const auto& content = page ? *static_cast<const UiSettingsPageContent*>(page->context) : none;
    const UiPageKey selected = page ? page->key : UiPageKey{};
    const bool unchanged =
        scene->active && scene->registry == registry && scene->registry_revision == registry->revision &&
        ui_page_key_equal(selected, scene->selected_page) && scene->content_revision == content.revision &&
        ui_rect_equal(scene->viewport, viewport) &&
        scene->projected_theme_geometry_revision == scene->theme_geometry_revision &&
        scene->projected_navigation_revision == scene->navigation_revision && scene->layout_signature;
    if (unchanged)
        return true;
    if (!valid_content(content))
        return false;
    UiSettingsStyle style = content.style ? *content.style : kUiDefaultSettingsStyle;
    if (scene->theme_styles && scene->theme_styles->ready)
        style = ui_theme_settings_style(scene->theme_styles);
    const bool changed_page = scene->registry != registry || !ui_page_key_equal(selected, scene->selected_page);
    if ((!scene->active || changed_page) && scene->page_generation == ~u64{0})
        return false;
    // Validate once before touching retained state, then reuse the same geometry.
    const SceneTokens tokens(*scene);
    const auto layout = ui_settings_modal_layout(tokens, viewport, scene->presentation, content.wide);
    const float width = maximum(1.f, ui_rect_width(layout.column));
    // Markdown blocks live only while their page is shown.
    if (changed_page)
        ui_markdown_bank_release(&scene->markdown);
    const auto context = row_context(*scene);
    float rows = 0;
    for (u32 i = 0; i < content.controls.count; ++i) {
        rows += row_layout(tokens, style, content, i, 0, 0, width, context).height;
        if (rows > kMaximumUiCoordinate - 8192)
            return false;
    }
    const bool previous_active = scene->active;
    const auto* previous_registry = scene->registry;
    const u64 previous_signature = scene->layout_signature;
    const u32 previous_theme = scene->projected_theme_geometry_revision;
    scene->registry = registry;
    copy_value(&scene->content, content);
    scene->style = style;
    scene->footer = content.footer ? *content.footer : UiSettingsFooter{};
    scene->viewport = viewport;
    scene->layout = layout;
    if (scene->presentation == UiSettingsPresentation::dialog) {
        // Fit the dialog to its title, rows and footer band.
        const auto f = frame(*scene, tokens.metric);
        ui_settings_dialog_fit(&scene->layout, viewport,
                               f.body.top - scene->layout.modal.top + rows + kUiSettingsDialogBodyEnd +
                                   (scene->footer.visible ? dialog_footer_height(tokens.metric)
                                                          : kUiSettingsDialogPadding));
    }
    const u64 signature = layout_signature(*scene, content, style, viewport, selected, registry->revision);
    const bool retain_layout = previous_active && previous_registry == registry && previous_signature == signature &&
                               previous_theme == scene->theme_geometry_revision;
    if (!scene->active || changed_page)
        ++scene->page_generation;
    if (!scene->active)
        ++scene->opens;
    scene->registry_revision = registry->revision;
    scene->content_revision = content.revision;
    scene->projected_theme_geometry_revision = scene->theme_geometry_revision;
    scene->projected_navigation_revision = scene->navigation_revision;
    scene->layout_signature = signature;
    scene->selected_page = selected;
    track_segments(scene, previous_active && !changed_page);
    if (changed_page) {
        scene->content_offset = 0;
        scene->flash_control = 0;
        scene->flash = 0;
        reset_value(&scene->animation);
    }
    // A page that opens, or changes by keyboard or a link, scrolls its sidebar entry into view.
    const UiSettingsTarget entry{UiSettingsTargetKind::page, selected, 0};
    const bool reveal = changed_page || !previous_active;
    if (retain_layout) {
        synchronize_input(*scene);
        ++scene->revision;
        if (reveal)
            ui_settings_scene_reveal(scene, entry);
        return true;
    }
    if (!build(scene)) {
        ui_settings_scene_release(scene);
        return false;
    }
    scene->active = true;
    if (reveal)
        ui_settings_scene_reveal(scene, entry);
    return true;
}
void ui_settings_scene_release(UiSettingsScene* scene) noexcept {
    if (!scene)
        return;
    const u32 revision = scene->revision + 1;
    const u64 page_generation = scene->page_generation;
    const auto presentation = scene->presentation;
    const u64 opens = scene->opens;
    const auto* measure = scene->measure;
    ui_fixed_widget_storage_release(&scene->storage);
    ui_markdown_bank_release(&scene->markdown);
    reset_value(scene);
    scene->revision = revision;
    scene->page_generation = page_generation;
    scene->presentation = presentation;
    scene->opens = opens;
    scene->measure = measure;
}
bool ui_settings_scene_bind_measure(UiSettingsScene* scene, const UiKitMeasure* measure) noexcept {
    if (!scene || scene->measure == measure)
        return false;
    scene->measure = measure;
    scene->layout_signature = 0;
    return true;
}
bool ui_settings_scene_host_rect(const UiSettingsScene& scene, UiRect* page, UiRect* clip) noexcept {
    if (!current(scene) || !scene.content.host)
        return false;
    const auto* node = ui_widget_manager_layout_node(scene.widgets, scene.body);
    if (!node)
        return false;
    // The page starts at the body's content edge; the clip keeps the body's focus ring gutter past it.
    if (page)
        *page = {scene.layout.column.left, node->content_rectangle.top, scene.layout.column.right, node->rectangle.bottom};
    if (clip)
        *clip = ui_rect_overlap(node->rectangle, node->clip_rectangle);
    return true;
}
UiSettingsTarget ui_settings_scene_hit_test(const UiSettingsScene& scene, float x, float y) noexcept {
    if (!current(scene))
        return {};
    const auto handle = ui_widget_manager_hit_test(scene.widgets, x, y);
    if (!handle.generation)
        return {};
    const auto target = entries(scene)[handle.slot].target;
    if (target.kind == UiSettingsTargetKind::search && scene.search.view.text.bytes) {
        const auto* node = ui_widget_manager_layout_node(scene.widgets, handle);
        if (node && ui_rect_contains(ui_settings_search_clear_rect(node->rectangle), x, y))
            return {UiSettingsTargetKind::search_clear, {}, 0};
    }
    return target;
}
UiSettingsTarget ui_settings_scene_focused(const UiSettingsScene& scene) noexcept {
    return current(scene) ? scene.focus : UiSettingsTarget{};
}
UiSettingsFocusZone ui_settings_scene_focus_zone(const UiSettingsScene& scene) noexcept {
    return current(scene) ? zone_of(scene.focus) : UiSettingsFocusZone::none;
}
bool ui_settings_scene_reveal(UiSettingsScene* scene, const UiSettingsTarget& target) noexcept {
    if (!scene || !current(*scene))
        return false;
    const SceneTokens t(*scene);
    const auto& m = t.metric;
    if (target.kind == UiSettingsTargetKind::page || target.kind == UiSettingsTargetKind::action) {
        float height = 0;
        const float top = nav_offset_of(*scene, m, target, &height);
        // Pinned pages stand in the foot and are always in view.
        if (top == -2)
            return true;
        if (top < 0)
            return false;
        const float previous = scene->sidebar_offset;
        reveal_range(scene, true, top, top + height, m);
        return apply_scroll(scene, true, previous);
    }
    u32 row = 0;
    if (target.kind != UiSettingsTargetKind::control || !find_control(*scene, target, &row))
        return target.kind != UiSettingsTargetKind::none;
    const float previous = scene->content_offset;
    reveal_range(scene, false, row_start(*scene, row), row_start(*scene, row + 1), m);
    return apply_scroll(scene, false, previous);
}
bool ui_settings_scene_focus(UiSettingsScene* scene, UiSettingsTarget target) noexcept {
    if (!scene || !current(*scene) || !target_exists(*scene, target))
        return false;
    if (target.kind == UiSettingsTargetKind::search_clear)
        target = {UiSettingsTargetKind::search, {}, 0};
    if (!ui_settings_scene_reveal(scene, target))
        return false;
    const auto handle = target_handle(*scene, target);
    if (handle.generation && !ui_widget_manager_focus(&scene->widgets, handle))
        return false;
    if (!ui_settings_target_equal(scene->focus, target)) {
        scene->focus = target;
        ++scene->revision;
    }
    find_control(*scene, target, &scene->focus_row);
    if (!handle.generation)
        return build(scene);
    return true;
}
bool ui_settings_scene_move_focus(UiSettingsScene* scene, bool reverse) noexcept {
    if (!scene || !current(*scene))
        return false;
    // Walks the (first 256) focusable targets in order; the second walk picks the one to move to.
    u32 count = 0, position = ~0u, ahead = ~0u;
    u32 parked_row = 0;
    const bool parked = focus_parked(*scene, &parked_row);
    const auto walk = [&](u32 pick, UiSettingsTarget* picked) noexcept {
        count = 0;
        for (const auto zone : kFocusZones)
            visit_targets(*scene, zone, [&](UiSettingsTarget target) noexcept {
                if (count < 256 && target_exists(*scene, target)) {
                    if (picked && count == pick)
                        *picked = target;
                    if (ui_settings_target_equal(target, scene->focus))
                        position = count;
                    // A parked focus continues from its slot: `ahead` is the first target past it.
                    if (parked && ahead == ~0u && after_parked_focus(*scene, target, parked_row))
                        ahead = count;
                    ++count;
                }
                return false;
            });
    };
    walk(0, nullptr);
    if (!count)
        return false;
    u32 next = position == ~0u ? (reverse ? count - 1 : 0)
               : reverse       ? (position ? position - 1 : count - 1)
                               : (position + 1 == count ? 0 : position + 1);
    if (parked && ahead != ~0u)
        next = reverse ? (ahead ? ahead - 1 : count - 1) : ahead;
    UiSettingsTarget chosen{};
    walk(next, &chosen);
    return ui_settings_scene_focus(scene, chosen);
}
bool ui_settings_scene_move_zone(UiSettingsScene* scene, bool reverse) noexcept {
    if (!scene || !current(*scene))
        return false;
    const auto zone = zone_of(scene->focus);
    u32 position = 0;
    for (u32 i = 0; i < 4; ++i)
        if (kFocusZones[i] == zone)
            position = i;
    for (u32 step = 1; step <= 4; ++step) {
        const auto candidate = kFocusZones[reverse ? (position + 4 - step) % 4 : (position + step) % 4];
        UiSettingsTarget chosen{};
        if (candidate == UiSettingsFocusZone::sidebar) {
            const UiSettingsTarget selected{UiSettingsTargetKind::page, scene->selected_page, 0};
            if (target_exists(*scene, selected))
                chosen = selected;
        }
        if (chosen.kind == UiSettingsTargetKind::none)
            visit_targets(*scene, candidate, [&](UiSettingsTarget target) noexcept {
                if (!target_exists(*scene, target))
                    return false;
                // The panel resumes at the first row still in view.
                if (candidate == UiSettingsFocusZone::panel && target.kind == UiSettingsTargetKind::control) {
                    u32 row = 0;
                    if (find_control(*scene, target, &row) && row_start(*scene, row + 1) <= scene->content_offset)
                        return false;
                }
                chosen = target;
                return true;
            });
        if (chosen.kind != UiSettingsTargetKind::none)
            return ui_settings_scene_focus(scene, chosen);
    }
    return false;
}
bool ui_settings_scene_move_within(UiSettingsScene* scene, bool reverse) noexcept {
    if (!scene || !current(*scene))
        return false;
    const auto zone = zone_of(scene->focus);
    if (zone != UiSettingsFocusZone::sidebar && zone != UiSettingsFocusZone::panel)
        return false;
    UiSettingsTarget previous{}, chosen{};
    bool found = false;
    u32 parked_row = 0;
    const bool parked = focus_parked(*scene, &parked_row);
    visit_targets(*scene, zone, [&](UiSettingsTarget target) noexcept {
        if (!target_exists(*scene, target))
            return false;
        if (found) {
            if (!reverse) {
                chosen = target;
                return true;
            }
            return false;
        }
        // A parked focus moves from its slot: to the first row past it, or the last row before it.
        if (parked && after_parked_focus(*scene, target, parked_row)) {
            found = true;
            chosen = reverse ? previous : target;
            return true;
        }
        if (ui_settings_target_equal(target, scene->focus)) {
            found = true;
            if (reverse) {
                chosen = previous;
                return true;
            }
            return false;
        }
        previous = target;
        return false;
    });
    if (parked && !found && reverse)
        chosen = previous;
    return chosen.kind != UiSettingsTargetKind::none && ui_settings_scene_focus(scene, chosen);
}
void ui_settings_scene_set_focus_visible(UiSettingsScene* scene, bool visible) noexcept {
    if (!scene || scene->focus_visible == visible)
        return;
    scene->focus_visible = visible;
    ++scene->revision;
}
bool ui_settings_scene_scroll(UiSettingsScene* scene, bool sidebar, float delta) noexcept {
    if (!scene || !current(*scene) || !ui_coordinate_valid(delta))
        return false;
    const SceneTokens t(*scene);
    auto& offset = sidebar ? scene->sidebar_offset : scene->content_offset;
    const float previous = offset;
    offset = clamp_offset(*scene, sidebar, offset + delta, t.metric);
    return apply_scroll(scene, sidebar, previous);
}
bool ui_settings_scene_scroll_viewport(const UiSettingsScene& scene, bool sidebar, UiRect* viewport) noexcept {
    if (!current(scene))
        return false;
    const auto* node = ui_widget_manager_layout_node(scene.widgets, sidebar ? scene.sidebar : scene.body);
    if (!node)
        return false;
    *viewport = ui_rect_overlap(node->rectangle, node->clip_rectangle);
    // The sidebar's scrollbar sits in its right padding, beside the rows.
    if (sidebar)
        viewport->right = minimum(scene.layout.sidebar.right, viewport->right + 12);
    return true;
}
UiScrollbarGeometry ui_settings_scene_scrollbar(const UiSettingsScene& scene, bool sidebar) noexcept {
    UiRect viewport{};
    if (!ui_settings_scene_scroll_viewport(scene, sidebar, &viewport))
        return {};
    const SceneTokens t(scene);
    return ui_scrollbar_geometry(viewport, sidebar ? scene.sidebar_offset : scene.content_offset,
                                 sidebar ? nav_extent(scene, t.metric) : content_extent(scene, t.metric));
}
bool ui_settings_scene_activation(const UiSettingsScene& scene, const UiSettingsTarget& target, bool reverse,
                                  UiSettingsActivation* activation) noexcept {
    if (!activation || !current(scene) || !target_exists(scene, target))
        return false;
    *activation = {target, UiSettingsControlKind::action, false, 0};
    if (target.kind != UiSettingsTargetKind::control)
        return true;
    const auto& c = *find_control(scene, target);
    activation->kind = c.kind;
    activation->checked = !c.checked;
    activation->selected_choice = c.selected_choice;
    const u32 count = c.choices.count;
    // Button groups and colour swatches press their first option.
    if (c.kind == UiSettingsControlKind::buttons || c.kind == UiSettingsControlKind::colors) {
        activation->selected_choice = c.selected_choice < count ? c.selected_choice : 0;
        return count != 0;
    }
    if (c.kind == UiSettingsControlKind::choice || c.kind == UiSettingsControlKind::radio ||
        c.kind == UiSettingsControlKind::palette || c.kind == UiSettingsControlKind::segments ||
        c.kind == UiSettingsControlKind::swatches || c.kind == UiSettingsControlKind::tabs) {
        const bool none = c.selected_choice >= count;
        activation->selected_choice = none        ? (reverse ? count - 1 : 0)
                                      : reverse   ? (c.selected_choice ? c.selected_choice - 1 : count - 1)
                                                  : (c.selected_choice + 1 == count ? 0 : c.selected_choice + 1);
    }
    if (c.kind == UiSettingsControlKind::slider)
        activation->selected_choice =
            reverse ? (c.selected_choice ? c.selected_choice - 1 : 0) : minimum(c.selected_choice + 1, count - 1);
    return true;
}
const UiSettingsControl* ui_settings_scene_control(const UiSettingsScene& scene, const UiSettingsTarget& target,
                                                   u32* index) noexcept {
    return current(scene) ? find_control(scene, target, index) : nullptr;
}
bool ui_settings_scene_control_anchor(const UiSettingsScene& scene, const UiSettingsTarget& target, UiRect* anchor) noexcept {
    const auto* control = ui_settings_scene_control(scene, target);
    if (!control || !anchor || !control->enabled)
        return false;
    UiSettingsControlGeometry geometry;
    if (!ui_settings_scene_control_geometry(scene, target, &geometry))
        return false;
    *anchor = geometry.clip;
    return true;
}
bool ui_settings_scene_control_geometry(const UiSettingsScene& scene, const UiSettingsTarget& target,
                                        UiSettingsControlGeometry* output) noexcept {
    if (output)
        reset_value(output);
    u32 index = 0;
    const auto* control = ui_settings_scene_control(scene, target, &index);
    if (!output || !control)
        return false;
    const auto handle = target_handle(scene, target);
    const SceneTokens t(scene);
    RowLayout row;
    UiRect clip;
    if (!row_layout_for(scene, t, handle, index, &row, &clip))
        return false;
    UiRect field = ui_rect_has_area(row.control) ? row.control : row.row;
    UiRect text_rectangle = field;
    if (editor_kind(control->kind)) {
        const float pad = scene.style.field_padding;
        const float vertical = control->kind == UiSettingsControlKind::multiline
                                   ? pad
                                   : minimum(pad, maximum(0.f, (ui_rect_height(field) - 24.f) * .5f));
        text_rectangle = {field.left + pad, field.top + vertical, field.right - pad, field.bottom - vertical};
        // An edit cell's text runs between its emoji and clear buttons.
        if (more(scene.content, index, ui_settings_more_cell) && ui_rect_has_area(row.lead)) {
            text_rectangle.left = row.lead.right + t.metric.chip_padding;
            text_rectangle.right = (ui_rect_has_area(row.value) ? row.value.left : field.right) - t.metric.chip_padding;
        }
    } else if (control->kind == UiSettingsControlKind::choice)
        text_rectangle = ui_select_text_rect(t, field);
    const auto visible = ui_rect_overlap(field, clip);
    if (!ui_rect_has_area(visible) || !ui_rect_has_area(text_rectangle))
        return false;
    *output = {target,
               {&scene.widgets, handle},
               row.row,
               field,
               text_rectangle,
               visible,
               control->enabled,
               control->kind == UiSettingsControlKind::multiline};
    return true;
}
bool ui_settings_scene_editor_hit(const UiSettingsScene& scene, float x, float y,
                                  UiSettingsControlGeometry* output) noexcept {
    if (output)
        reset_value(output);
    const auto target = ui_settings_scene_hit_test(scene, x, y);
    u32 index = 0;
    const auto* control = ui_settings_scene_control(scene, target, &index);
    if (!(control && control->enabled && editor_kind(control->kind) &&
          ui_settings_scene_control_geometry(scene, target, output) && ui_rect_contains(output->clip, x, y)))
        return false;
    // An edit cell's buttons are not part of its text.
    if (more(scene.content, index, ui_settings_more_cell)) {
        RowLayout row;
        if (row_layout_for(scene, SceneTokens(scene), target_handle(scene, target), index, &row) &&
            (ui_rect_contains(row.lead, x, y) || ui_rect_contains(row.value, x, y))) {
            reset_value(output);
            return false;
        }
    }
    return true;
}
bool ui_settings_scene_bind_palette(UiSettingsScene* scene, UiSettingsPaletteSource source) noexcept {
    if (!scene || (source.resolve && (!source.context || !source.revision)) ||
        (!source.resolve && (source.context || source.revision)))
        return false;
    if (scene->palette.context == source.context && scene->palette.resolve == source.resolve &&
        scene->palette.revision == source.revision)
        return true;
    scene->palette = source;
    scene->palette_hovered_index = ~u32{0};
    ++scene->revision;
    return true;
}
namespace {
// Fills the semantic request of choice `choice` on a control.
bool report(const UiSettingsTarget& target, const UiSettingsControl& c, u32 choice, UiSettingsActivation* output) noexcept {
    *output = {target, c.kind, c.checked, choice};
    return true;
}
// The enabled control of `kind` the pointer is on, with its geometry.
const UiSettingsControl* pointer_control(const UiSettingsScene& scene, const UiSettingsTarget& target,
                                         UiSettingsControlKind kind, float x, float y,
                                         UiSettingsControlGeometry* geometry) noexcept {
    const auto* c = ui_settings_scene_control(scene, target);
    return ui_coordinate_valid(x) && ui_coordinate_valid(y) && c && c->kind == kind && c->enabled &&
                   ui_settings_scene_control_geometry(scene, target, geometry) &&
                   ui_rect_contains(geometry->clip, x, y)
               ? c
               : nullptr;
}
} // namespace
bool ui_settings_scene_slider_geometry(const UiSettingsScene& scene, const UiSettingsTarget& target,
                                       UiSettingsSliderGeometry* output) noexcept {
    u32 index = 0;
    const auto* c = ui_settings_scene_control(scene, target, &index);
    UiSettingsControlGeometry geometry;
    if (!output || !c || c->kind != UiSettingsControlKind::slider ||
        !ui_settings_scene_control_geometry(scene, target, &geometry))
        return false;
    const SceneTokens t(scene);
    const auto slider = ui_slider_geometry(t, geometry.field, c->choices.count, c->selected_choice,
                                           marks(scene.content, index));
    const UiRect track{slider.track_left, slider.bar.top, slider.track_right, slider.bar.bottom};
    if (!ui_rect_has_area(track) || !ui_rect_intersects(slider.bar, geometry.clip))
        return false;
    *output = {geometry, track, slider.grabber};
    return true;
}
bool ui_settings_scene_slider_pointer(const UiSettingsScene& scene, const UiSettingsTarget& target, float x,
                                      UiSettingsActivation* output) noexcept {
    UiSettingsSliderGeometry geometry;
    if (!output || !ui_coordinate_valid(x) || !ui_settings_scene_slider_geometry(scene, target, &geometry) ||
        !geometry.control.enabled)
        return false;
    const auto* c = ui_settings_scene_control(scene, target);
    const float fraction = maximum(0.f, minimum(1.f, (x - geometry.track.left) / ui_rect_width(geometry.track)));
    return report(target, *c, static_cast<u32>(fraction * (c->choices.count - 1) + .5f), output);
}
bool ui_settings_scene_slider_hit(const UiSettingsScene& scene, const UiSettingsTarget& target, float x, float y) noexcept {
    UiSettingsSliderGeometry geometry;
    if (!ui_coordinate_valid(x) || !ui_coordinate_valid(y) ||
        !ui_settings_scene_slider_geometry(scene, target, &geometry) || !geometry.control.enabled)
        return false;
    const float middle = (geometry.track.top + geometry.track.bottom) * .5f;
    return ui_rect_contains(ui_rect_intersection({geometry.track.left - 10, middle - 14, geometry.track.right + 10,
                                                  middle + 14},
                                                 geometry.control.clip),
                            x, y);
}
bool ui_settings_scene_slider_key(const UiSettingsScene& scene, const UiSettingsTarget& target, UiSettingsSliderKey key,
                                  UiSettingsActivation* output) noexcept {
    const auto* c = ui_settings_scene_control(scene, target);
    if (!output || !c || !c->enabled || !target_exists(scene, target) ||
        (c->kind != UiSettingsControlKind::slider && c->kind != UiSettingsControlKind::palette &&
         c->kind != UiSettingsControlKind::radio && c->kind != UiSettingsControlKind::segments &&
         c->kind != UiSettingsControlKind::swatches) ||
        key > UiSettingsSliderKey::page_increase)
        return false;
    const u32 last = c->choices.count - 1;
    const bool up = key == UiSettingsSliderKey::increase || key == UiSettingsSliderKey::page_increase;
    const bool page = key == UiSettingsSliderKey::page_decrease || key == UiSettingsSliderKey::page_increase;
    const u32 step = page && c->kind == UiSettingsControlKind::slider ? ui_slider_page_step(c->choices.count) : 1u;
    const u32 selected = c->selected_choice > last ? (up ? ~0u : last + 1) : c->selected_choice;
    const u32 value = key == UiSettingsSliderKey::home  ? 0
                      : key == UiSettingsSliderKey::end ? last
                      : !up ? (selected > step ? minimum(selected - step, last) : 0)
                            : (selected == ~0u ? 0 : minimum(selected + step, last));
    return report(target, *c, value, output);
}
bool ui_settings_scene_palette_pointer(const UiSettingsScene& scene, const UiSettingsTarget& target, float x, float y,
                                       UiSettingsActivation* output) noexcept {
    UiSettingsControlGeometry geometry;
    const auto* c = output ? pointer_control(scene, target, UiSettingsControlKind::palette, x, y, &geometry) : nullptr;
    if (!c)
        return false;
    const SceneTokens t(scene);
    const u32 columns = ui_swatch_columns(t, ui_rect_width(geometry.field));
    for (u32 i = 0; i < c->choices.count; ++i) {
        auto swatch = ui_swatch_rect(t, geometry.field, i, columns);
        swatch.bottom += ui_swatch_label_height();
        if (ui_rect_contains(swatch, x, y))
            return report(target, *c, i, output);
    }
    return false;
}
bool ui_settings_scene_radio_pointer(const UiSettingsScene& scene, const UiSettingsTarget& target, float x, float y,
                                     UiSettingsActivation* output) noexcept {
    UiSettingsControlGeometry geometry;
    const auto* c = output ? pointer_control(scene, target, UiSettingsControlKind::radio, x, y, &geometry) : nullptr;
    const u32 choice = c ? ui_radio_hit(SceneTokens(scene), geometry.field, c->choices.count, x, y) : ~0u;
    return choice != ~0u && report(target, *c, choice, output);
}
bool ui_settings_scene_button_hit(const UiSettingsScene& scene, const UiSettingsTarget& target, float x, float y) noexcept {
    u32 index = 0;
    const auto* c = ui_settings_scene_control(scene, target, &index);
    const bool cell = c && more(scene.content, index, ui_settings_more_cell);
    if (c && c->kind == UiSettingsControlKind::edit && (c->action_label.bytes || cell) && c->enabled) {
        RowLayout row;
        UiRect clip;
        return row_layout_for(scene, SceneTokens(scene), target_handle(scene, target), index, &row, &clip) &&
               (ui_rect_contains(ui_rect_overlap(row.value, clip), x, y) ||
                (cell && ui_rect_contains(ui_rect_overlap(row.lead, clip), x, y)));
    }
    UiSettingsControlGeometry geometry;
    return c && (c->kind == UiSettingsControlKind::notice || c->kind == UiSettingsControlKind::action ||
                 c->kind == UiSettingsControlKind::status || c->kind == UiSettingsControlKind::footnote ||
                 c->kind == UiSettingsControlKind::markdown) &&
           ui_settings_scene_control_geometry(scene, target, &geometry) && ui_rect_contains(geometry.clip, x, y);
}
namespace {
__forceinline u32 option_hit(const UiSettingsLayoutTokens& t, const UiSettingsControl& c, const UiRect& field, float x, float y,
               const UiKitMeasure* measure) noexcept {
    const u32 count = c.choices.count + (c.kind == UiSettingsControlKind::colors && c.action_label.bytes ? 1u : 0u);
    for (u32 i = 0; i < count; ++i)
        if (ui_rect_contains(option_rect(t, c, field, i, measure), x, y))
            return i;
    return ~0u;
}
} // namespace
bool ui_settings_scene_option_pointer(const UiSettingsScene& scene, const UiSettingsTarget& target, float x, float y,
                                      UiSettingsActivation* output) noexcept {
    u32 index = 0;
    const auto* c = ui_settings_scene_control(scene, target, &index);
    UiSettingsControlGeometry geometry;
    if (!output || !ui_coordinate_valid(x) || !ui_coordinate_valid(y) || !c || !c->enabled)
        return false;
    const bool edit_cell = c->kind == UiSettingsControlKind::edit && more(scene.content, index, ui_settings_more_cell);
    if ((list_item(scene.content, index) && c->kind == UiSettingsControlKind::action) ||
        (c->kind == UiSettingsControlKind::edit && (c->action_label.bytes || edit_cell))) {
        // List rows report their part; an edit row its button as choice 0 (an edit cell its leading button as 1).
        const SceneTokens t(scene);
        RowLayout row;
        UiRect clip;
        if (!row_layout_for(scene, t, target_handle(scene, target), index, &row, &clip) ||
            !ui_rect_contains(clip, x, y))
            return false;
        const u32 part = c->kind == UiSettingsControlKind::edit
                             ? (ui_rect_contains(row.value, x, y)                ? 0u
                                : edit_cell && ui_rect_contains(row.lead, x, y) ? 1u
                                                                                : ~0u)
                             : list_part(t, scene.content, index, row.row, x, y);
        if (part == ~0u)
            return false;
        return report(target, *c, part, output);
    }
    if (!option_kind(c->kind) || !ui_settings_scene_control_geometry(scene, target, &geometry) ||
        !ui_rect_contains(geometry.clip, x, y))
        return false;
    const SceneTokens t(scene);
    const bool tristate = has_flag(scene.content, index, ui_settings_presentation_tristate);
    const u32 choice = c->kind == UiSettingsControlKind::segments
                           ? (tristate ? ui_tristate_hit(t, geometry.field, x, y)
                                       : ui_segmented_hit(t, geometry.field, c->choices.count, x, y))
                       : c->kind == UiSettingsControlKind::swatches
                           ? (more(scene.content, index, ui_settings_more_banner_tiles)
                                  ? banner_tile_hit(t, geometry.field, c->choices.count, x, y)
                              : more(scene.content, index, ui_settings_more_glyphs)
                                  ? glyph_tile_hit(t, geometry.field, c->choices.count, x, y)
                                  : ui_color_grid_hit(t, geometry.field, c->choices.count, x, y))
                           : option_hit(t, *c, geometry.field, x, y, scene.measure);
    if (choice == ~0u)
        return false;
    return report(target, *c, choice, output);
}
bool ui_settings_scene_choice_rectangle(const UiSettingsScene& scene, const UiSettingsTarget& target, u32 choice,
                                        UiRect* output) noexcept {
    u32 index = 0;
    const auto* c = ui_settings_scene_control(scene, target, &index);
    UiSettingsControlGeometry geometry;
    if (output && c && list_style(scene.content, index, UiSettingsListStyle::row)) {
        const SceneTokens t(scene);
        RowLayout row;
        if (!row_layout_for(scene, t, target_handle(scene, target), index, &row))
            return false;
        const auto parts = list_parts(t, scene.content, index, row.row);
        *output = choice == ui_settings_list_row     ? row.row
                  : choice == ui_settings_list_count ? parts.column
                  : choice - ui_settings_list_action < kUiSettingsListActions
                      ? parts.actions[choice - ui_settings_list_action]
                      : UiRect{};
        return ui_rect_has_area(*output);
    }
    if (!output || !c || choice >= c->choices.count ||
        (c->kind != UiSettingsControlKind::radio && c->kind != UiSettingsControlKind::segments &&
         c->kind != UiSettingsControlKind::tabs && c->kind != UiSettingsControlKind::buttons &&
         c->kind != UiSettingsControlKind::colors) ||
        !ui_settings_scene_control_geometry(scene, target, &geometry))
        return false;
    const SceneTokens t(scene);
    const bool tristate = has_flag(scene.content, index, ui_settings_presentation_tristate);
    *output = c->kind == UiSettingsControlKind::radio ? ui_radio_option(t, geometry.field, choice)
              : c->kind == UiSettingsControlKind::segments
                  ? (tristate ? ui_tristate_option(t, geometry.field, choice)
                              : ui_segmented_option(t, geometry.field, c->choices.count, choice))
                  : option_rect(t, *c, geometry.field, choice, scene.measure);
    return ui_rect_has_area(*output);
}
bool ui_settings_scene_grip_hit(const UiSettingsScene& scene, const UiSettingsTarget& target, float x, float y) noexcept {
    u32 index = 0;
    const auto* c = ui_settings_scene_control(scene, target, &index);
    if (!c || !c->enabled || !has_flag(scene.content, index, ui_settings_presentation_grip))
        return false;
    const SceneTokens t(scene);
    RowLayout row;
    UiRect clip;
    if (!row_layout_for(scene, t, target_handle(scene, target), index, &row, &clip))
        return false;
    const UiRect grip{row.lead.left - 6, row.row.top, row.lead.left + t.metric.grip_width + 4, row.row.bottom};
    return ui_rect_contains(ui_rect_intersection(grip, clip), x, y);
}
bool ui_settings_scene_drop_target(const UiSettingsScene& scene, float y, u64* control_id, bool* after) noexcept {
    if (!control_id || !after || !current(scene) || !ui_coordinate_valid(y))
        return false;
    const SceneTokens t(scene);
    float best = kMaximumUiCoordinate;
    bool found = false;
    for (u32 i = 0; i < scene.widgets.count; ++i) {
        const auto& entry = entries(scene)[i];
        if (entry.role != EntryRole::row)
            continue;
        RowLayout row;
        UiRect clip;
        if (!row_layout_for(scene, t, {static_cast<u16>(i), scene.widgets.generation}, entry.source_index,
                            &row, &clip) ||
            !ui_rect_intersects(row.row, clip))
            continue;
        const float middle = (row.row.top + row.row.bottom) * .5f;
        const float distance = y < row.row.top ? row.row.top - y : y > row.row.bottom ? y - row.row.bottom : 0;
        if (distance < best) {
            best = distance;
            *control_id = scene.content.controls[entry.source_index].id;
            *after = y >= middle;
            found = true;
        }
    }
    return found;
}
bool ui_settings_scene_set_drag(UiSettingsScene* scene, u64 dragged, u64 drop_control, bool after) noexcept {
    if (!scene)
        return false;
    const auto row = [&](u64 id) noexcept -> u16 {
        for (u32 i = 0; id && i < scene->content.controls.count && i < 0xfffe; ++i)
            if (scene->content.controls[i].id == id)
                return static_cast<u16>(i + 1);
        return 0;
    };
    const u16 drag = row(dragged), drop = row(drop_control);
    after = after && drop;
    if (scene->drag_row == drag && scene->drop_row == drop && scene->drop_after == after)
        return false;
    scene->drag_row = drag;
    scene->drop_row = drop;
    scene->drop_after = after;
    ++scene->motion_revision;
    return true;
}
bool ui_settings_scene_set_footer_motion(UiSettingsScene* scene, float shift, float alert) noexcept {
    if (!scene || !ui_coordinate_valid(shift))
        return false;
    shift = maximum(-64.f, minimum(shift, 64.f));
    alert = alert > 0 ? minimum(alert, 1.f) : 0.f;
    const i16 shift_q = static_cast<i16>(shift * 16.f);
    const u8 alert_q = static_cast<u8>(alert * 255.f + .5f);
    if (scene->footer_shift_q == shift_q && scene->footer_alert_q == alert_q)
        return false;
    scene->footer_shift_q = shift_q;
    scene->footer_alert_q = alert_q;
    // Footer text moves with the bar, so text is rebuilt as well as paint.
    ++scene->motion_revision;
    ++scene->revision;
    return true;
}
bool ui_settings_scene_palette_hover(UiSettingsScene* scene, const UiSettingsTarget& target, float x, float y) noexcept {
    if (!scene)
        return false;
    UiSettingsActivation activation;
    const u32 index = ui_settings_scene_palette_pointer(*scene, target, x, y, &activation) ||
                              ui_settings_scene_radio_pointer(*scene, target, x, y, &activation) ||
                              ui_settings_scene_option_pointer(*scene, target, x, y, &activation)
                          ? activation.selected_choice
                          : ~u32{0};
    if (scene->palette_hovered_index == index)
        return false;
    scene->palette_hovered_index = index;
    ++scene->revision;
    return true;
}
bool ui_settings_scene_aside(const UiSettingsScene& scene, UiRect* heading, UiRect* card, UiRect* clip,
                             bool* beside) noexcept {
    if (!current(scene))
        return false;
    const auto aside = aside_layout(scene);
    if (!aside.active)
        return false;
    const SceneTokens t(scene);
    const auto f = frame(scene, t.metric);
    const auto& content = scene.content;
    const float label = content.aside_label.bytes ? kUiSettingsLabelLine + kAsideHeadingGap : 0;
    float left = scene.layout.column.left, top = 0;
    if (aside.beside) {
        // Sticky 24 DIP below the scroller's top while its rows scroll by.
        left = scene.layout.column.left + aside.form + kAsideGap;
        const float start = f.body.top + row_start(scene, content.aside_row) - scene.content_offset;
        const float end = f.body.top + content_extent(scene, t.metric) - t.metric.panel_padding_bottom -
                          scene.content_offset - aside.block;
        top = maximum(start, minimum(f.body.top + kSectionGap, end));
    } else
        top = f.body.top + row_start(scene, content.aside_row) - aside.block - kSectionGap - scene.content_offset;
    const UiRect head{left, top, left + aside.width, top + (label ? kUiSettingsLabelLine : 0)};
    const UiRect body{left, top + label, left + aside.width, top + label + content.aside_height};
    const UiRect visible = ui_rect_overlap(f.body, scene.layout.panel);
    if (!ui_rect_has_area(ui_rect_overlap({left, top, left + aside.width, top + aside.block}, visible)))
        return false;
    if (heading)
        *heading = head;
    if (card)
        *card = body;
    if (clip)
        *clip = visible;
    if (beside)
        *beside = aside.beside;
    return true;
}
bool ui_settings_scene_page_owner(const UiSettingsScene& scene, UiWidgetReference owner) noexcept {
    if (!current(scene) || owner.manager != &scene.widgets || !ui_widget_manager_resolve(scene.widgets, owner.handle))
        return false;
    if (ui_widget_handle_equal(owner.handle, scene.title) || ui_widget_handle_equal(owner.handle, scene.description))
        return true;
    const auto& entry = entries(scene)[owner.handle.slot];
    return entry.role == EntryRole::row || entry.role == EntryRole::footer_status ||
           entry.target.kind == UiSettingsTargetKind::save || entry.target.kind == UiSettingsTargetKind::reset;
}
bool ui_settings_scene_spinner_visible(const UiSettingsScene& scene) noexcept {
    if (!scene.active)
        return false;
    for (u32 i = 0; i < scene.content.controls.count; ++i) {
        const auto& c = scene.content.controls[i];
        if ((c.kind == UiSettingsControlKind::notice && c.checked) || (c.kind == UiSettingsControlKind::status && c.checked) ||
            (scene.content.meters && scene.content.meters[i] == kUiSettingsIndeterminateMeter))
            return true;
    }
    return false;
}

// The cursor of ui_settings_scene_next_avatar: a row's entry index shifted up kMediaPhaseBits over the phase inside
// it, 0 the row's avatar and 1 + i the picture of its glyph tile i (a choice of a glyph swatch row, or the name tag
// row's chip).
constexpr u32 kMediaPhaseBits = 8, kMediaPhaseMask = (1u << kMediaPhaseBits) - 1;

bool ui_settings_scene_next_avatar(const UiSettingsScene& scene, u32* cursor,
                                   UiSettingsAvatarGeometry* output) noexcept {
    const auto& content = scene.content;
    if (!cursor || !output || !current(scene) || (!content.avatar_media && !content.glyph_tiles && !content.preview))
        return false;
    const SceneTokens t(scene);
    u32 phase = *cursor & kMediaPhaseMask;
    for (u32 at = *cursor >> kMediaPhaseBits; at < scene.widgets.count; ++at, phase = 0) {
        const auto& entry = entries(scene)[at];
        const u32 index = entry.source_index;
        if (entry.role != EntryRole::row || index >= content.controls.count)
            continue;
        const auto& c = content.controls[index];
        const bool tiles = content.glyph_tiles && c.kind == UiSettingsControlKind::swatches &&
                           more(content, index, ui_settings_more_glyphs);
        const bool chip = content.glyph_tiles && c.kind == UiSettingsControlKind::action &&
                          more(content, index, ui_settings_more_name_tag);
        const bool avatar_row = avatar_backed(content, index);
        const bool preview = c.kind == UiSettingsControlKind::preview && content.preview && content.preview->avatars;
        if (!avatar_row && !tiles && !chip && !preview)
            continue;
        const UiWidgetHandle handle{static_cast<u16>(at), scene.widgets.generation};
        RowLayout r;
        UiRect clip;
        if (!row_layout_for(scene, t, handle, index, &r, &clip))
            continue;
        if (preview) {
            // Accessibility's preview: your avatar at each cozy message, then the status column's three, one phase each.
            const auto& s = *content.preview;
            for (u32 part = phase; part < 5; ++part) {
                const bool column = part > 1;
                const auto& media = s.avatars[column ? part - 1 : 0];
                const UiRect avatar = column ? preview_status_avatar(r, part - 2) : preview_avatar(r, part);
                const UiRect visible = ui_rect_overlap(avatar, clip);
                if ((s.compact && !column) || !avatar_media_ready(media) || !ui_rect_has_area(visible))
                    continue;
                *output = {&media, {&scene.widgets, handle}, avatar, visible, false, false, 0.f, 1.f};
                *cursor = at << kMediaPhaseBits | (part + 1);
                return true;
            }
            continue;
        }
        if (avatar_row && !phase) {
            // A banner row's upload tile (its last) shows the picture covering it.
            const bool upload = more(content, index, ui_settings_more_upload_tile);
            if (upload ? c.choices.count : ui_rect_has_area(r.lead)) {
                // Same circle as paint_row's loading placeholder.
                const UiRect avatar = upload ? banner_tile_rect(t, r.control, c.choices.count - 1)
                                             : avatar_rect(t.metric, content, index, r.lead);
                const UiRect visible = ui_rect_overlap(avatar, clip);
                if (ui_rect_has_area(visible)) {
                    // Game tiles round their covers; a trait's custom emoji keeps its own shape.
                    const bool tile = upload || (more(content, index, ui_settings_more_cell) &&
                                                 c.kind == UiSettingsControlKind::action);
                    *output = {&content.avatar_media[index], {&scene.widgets, handle}, avatar, visible, tile, upload,
                               tile ? t.metric.control_radius : 0.f, 1.f};
                    *cursor = at << kMediaPhaseBits | 1;
                    return true;
                }
            }
        }
        const u32 count = tiles ? c.choices.count : chip && ui_rect_has_area(r.tag) ? 1u : 0u;
        for (u32 i = phase ? phase - 1 : 0; i < count && i + 2 <= kMediaPhaseMask; ++i) {
            const auto* glyph = glyph_tile(content, index, i);
            if (!glyph_image(glyph))
                continue;
            const UiRect picture = tiles ? glyph_image_rect(glyph_tile_rect(t, r.control, i))
                                         : glyph_chip_image_rect(r.tag);
            const UiRect visible = ui_rect_overlap(picture, clip);
            if (!ui_rect_has_area(visible))
                continue;
            *output = {&glyph->media, {&scene.widgets, handle}, picture, visible, true, false, 0.f,
                       tiles && !c.enabled ? t.disabled_alpha : 1.f};
            *cursor = at << kMediaPhaseBits | (i + 2);
            return true;
        }
    }
    *cursor = scene.widgets.count << kMediaPhaseBits;
    return false;
}

} // namespace maicord
