#include "maicord/ui/shell/navigation_scene.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/chat/messages/mention_badge.hpp"
#include "maicord/ui/layout/widget_scroll.hpp"
#include "maicord/ui/layout/list_navigation.hpp"
#include "maicord/ui/layout/virtual_list.hpp"
#include "maicord/ui/chat/private_channel_rows.hpp"
#include "maicord/ui/core/widget_storage.hpp"

namespace maicord {
namespace {

inline constexpr u32 kNavigationRootWidgetId = 1;
inline constexpr u32 kNavigationSpacerWidgetId = 2;
bool row_focusable(const ChannelViewRow& row) noexcept {
    return row.id && (row.selectable || row.type == 4 || row.type == kNavigationEventsRowType ||
                      row.type == kNavigationRequestsRowType);
}

ShellNavigationSceneResult scene_error(ShellNavigationScene* scene,
                                       ShellNavigationSceneError error) noexcept {
    saturating_increment_if(scene, scene->rejected);
    return ShellNavigationSceneResult{error, scene ? scene->first : 0, scene ? scene->count : 0,
                                      scene ? scene->scroll_dips : 0.0f, false};
}

UiLayoutStyle row_style(const ChannelViewRow& row) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fill();
    style.height = ui_layout_fixed(shell_navigation_row_height(row));
    style.horizontal_alignment = UiLayoutAlignment::stretch;
    return style;
}

UiLayoutStyle icon_style(const ChannelViewRow& row) noexcept {
    UiLayoutStyle style = ui_layout_style();
    const bool category = row.type == 4;
    const bool private_channel = channel_is_private(row.type);
    // Original icon-pack glyphs: 20-DIP channel icons centred in the 30-DIP
    // head. A category's chevron follows its name (painted after measuring
    // it); its slot sits at the name's start.
    const float extent = private_channel ? kUiPrivateRowAvatar : category ? kShellCategoryChevron : kShellChannelIcon;
    style.width = ui_layout_fixed(extent);
    style.height = ui_layout_fixed(extent);
    style.margin.left = (private_channel ? 16.0f : category ? kShellCategoryNameLeft : kShellChannelIconLeft) +
                        static_cast<float>(row.depth) * kShellChannelIndent;
    style.margin.top = category ? kShellCategoryLineTop + (kShellCategoryHeight - kShellCategoryLineTop - extent) * .5f
                                : ((private_channel ? ui_private_row_height() : shell_navigation_head_height()) - extent) *
                                      0.5f;
    return style;
}

UiLayoutStyle label_style(const ChannelViewRow& row) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fill();
    style.height = ui_layout_fill();
    style.horizontal_alignment = UiLayoutAlignment::stretch;
    style.vertical_alignment = UiLayoutAlignment::stretch;
    const bool category = row.type == 4;
    const bool private_channel = channel_is_private(row.type);
    style.margin = UiInsets{
        (private_channel ? 60.0f : category ? kShellCategoryNameLeft : kShellChannelLabelLeft) +
            static_cast<float>(row.depth) * kShellChannelIndent,
        private_channel ? 0.0f : category ? kShellCategoryLineTop : (shell_navigation_head_height() - 20.0f) * .5f,
        row.mention_count != 0 ? ui_mention_badge_width(row.mention_count) + 20.0f
        : private_channel      ? 40.0f
                               : kShellChannelHighlightRight + kShellChannelHighlightLeft,
        0.0f};
    style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_text_content);
    return style;
}

UiLayoutStyle mention_badge_style(u32 count, bool private_channel) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fixed(ui_mention_badge_width(count));
    style.height = ui_layout_fixed(kUiMentionBadgeHeight);
    style.margin.right = private_channel ? 10.0f : kShellChannelHighlightRight + kShellChannelHighlightLeft;
    style.margin.top = ((private_channel ? ui_private_row_height() : shell_navigation_head_height()) -
                        kUiMentionBadgeHeight) * 0.5f;
    style.horizontal_alignment = UiLayoutAlignment::end;
    return style;
}

void clear_materialization(ShellNavigationScene* scene) noexcept {
    ui_widget_manager_reset(&scene->widgets);
    scene->root = ui_widget_invalid_handle();
    scene->first = 0;
    scene->count = 0;
    scene->active = false;
}

ShellNavigationSceneResult teardown_error(ShellNavigationScene* scene, ShellNavigationSceneError error) noexcept {
    clear_materialization(scene);
    return scene_error(scene, error);
}

u64 focused_channel(const ShellNavigationScene& scene) noexcept {
    const ShellNavigationRowWidget* focused = shell_navigation_scene_resolve(scene, scene.widgets.focused);
    return focused ? focused->view.id : scene.focused_channel_id;
}

} // namespace

ShellThreadSpine shell_navigation_thread_spine(UiRect row, float row_height, UiRect icon, bool first,
                                               bool continues) noexcept {
    const float x = (icon.left + icon.right) * 0.5f - kShellChannelIndent;
    const float y = row.top + row_height * 0.5f;
    const float r = kShellThreadSpineRadius;
    // The parent icon ends 5 DIP above this row; leave a 2 DIP gap below it.
    const float top = first ? row.top - 3.0f : row.top;
    return {{x, top}, {x, y - r}, {x + r, y}, {icon.right - 4.0f, y}, continues ? row.bottom : y - r};
}

float shell_navigation_row_height(const ChannelViewRow& row) noexcept {
    if (channel_is_private(row.type))
        return ui_private_row_height();
    if (row.type == 4)
        return kShellCategoryHeight;
    if (row.type == kNavigationEventsRowType)
        return shell_navigation_head_height() + kShellNavigationDividerBand;
    return shell_navigation_voice_top(row) + static_cast<float>(row.voice_count) * kShellNavigationVoiceUserHeight +
           (row.voice_invite ? kShellNavigationVoiceInviteHeight : 0.0f);
}

u8 shell_navigation_row_controls(const ChannelViewRow& row, bool hovered, bool selected) noexcept {
    if (row.type == 4)
        return row.controls & channel_row_create;
    return hovered || selected ? row.controls & (channel_row_invite | channel_row_settings) : 0;
}

UiRect shell_navigation_control_rectangle(const UiRect& row, const ChannelViewRow& view, ChannelRowControl control) noexcept {
    if (!(view.controls & control) || (control == channel_row_create) != (view.type == 4))
        return {};
    if (view.type == 4) {
        const float top = row.top + kShellCategoryLineTop +
                          (kShellCategoryHeight - kShellCategoryLineTop - kShellChannelControl) * .5f;
        const float right = row.right - kShellChannelHighlightLeft;
        return {right - kShellChannelControl, top, right, top + kShellChannelControl};
    }
    // The client's controls end 4 DIPs inside the highlight, or 4 before the
    // badge (which ends 6 inside).
    float right = row.right - kShellChannelHighlightRight - kShellChannelControlGap;
    if (view.mention_count)
        right = row.right - kShellChannelHighlightRight - kShellChannelHighlightLeft -
                ui_mention_badge_width(view.mention_count) - kShellChannelControlGap;
    if (control == channel_row_invite && (view.controls & channel_row_settings))
        right -= kShellChannelControl + kShellChannelControlGap;
    const float top = row.top + (shell_navigation_head_height() - kShellChannelControl) * .5f;
    return {right - kShellChannelControl, top, right, top + kShellChannelControl};
}

float shell_navigation_controls_left(const UiRect& row, const ChannelViewRow& view, u8 shown, float right) noexcept {
    constexpr ChannelRowControl order[]{channel_row_invite, channel_row_settings, channel_row_create};
    for (const auto control : order)
        if (shown & control)
            return minimum(right, shell_navigation_control_rectangle(row, view, control).left - kShellChannelControlGap);
    return right;
}

// Rows plus the 8 DIP insets above and below them (banner inset excluded).
static float shell_navigation_content_height(const NavigationView* view) noexcept {
    if (!view || view->channel_count > view->channel_capacity ||
        (view->channel_count != 0 && !view->channel_rows)) {
        return 0.0f;
    }
    float height = 16.0f;
    for (u32 index = 0; index < view->channel_count; ++index) {
        height += shell_navigation_row_height(view->channel_rows[index]);
    }
    return height;
}

bool shell_navigation_scene_initialize(ShellNavigationScene* scene,
                                       Slice<ShellNavigationRowWidget> row_storage,
                                       Slice<UiWidget> widget_storage,
                                       Slice<UiLayoutNode> layout_storage) noexcept {
    if (!scene || !row_storage.data || row_storage.count == 0 || !widget_storage.data ||
        !layout_storage.data || row_storage.count > (~0u - 2u) / 4u) {
        return false;
    }
    reset_value<ShellNavigationScene>(scene);
    scene->rows = row_storage;
    scene->root = ui_widget_invalid_handle();
    return ui_widget_manager_initialize(&scene->widgets, widget_storage, layout_storage);
}

ShellNavigationSceneResult shell_navigation_scene_update(ShellNavigationScene* scene,
                                                         const NavigationView* view, const UiRect& viewport,
                                                         float requested_scroll_dips) noexcept {
    if (!scene || !scene->rows.data || !scene->widgets.storage.data || !ui_rect_valid_or_empty(viewport) ||
        !ui_coordinate_valid(requested_scroll_dips) ||
        (view && (view->channel_count > view->channel_capacity ||
                  (view->channel_count != 0 && !view->channel_rows)))) {
        return scene_error(scene, ShellNavigationSceneError::invalid_input);
    }

    if (!view || !ui_rect_has_area(viewport)) {
        const bool materialized =
            scene->active || scene->count != 0 || scene->widgets.count != 0 || scene->root.generation != 0;
        const bool changed = materialized || !ui_rect_equal(scene->viewport, viewport);
        if (materialized)
            clear_materialization(scene);
        scene->viewport = viewport;
        scene->source_rows = view ? view->channel_rows : nullptr;
        scene->source_revision = view ? view->channel_revision : 0;
        scene->source_count = view ? view->channel_count : 0;
        scene->content_height = shell_navigation_content_height(view);
        scene->scroll_dips = 0.0f;
        if (!view) {
            scene->focused_channel_id = 0;
        }
        return ShellNavigationSceneResult{ShellNavigationSceneError::none, 0, 0, 0.0f, changed};
    }

    const bool source_changed = scene->source_rows != view->channel_rows ||
                                scene->source_revision != view->channel_revision ||
                                scene->source_count != view->channel_count;
    const float inset = shell_navigation_top_inset(*scene);
    const float content_height =
        source_changed ? shell_navigation_content_height(view) + scene->banner_inset : scene->content_height;
    const float maximum_scroll =
        content_height > ui_rect_height(viewport) ? content_height - ui_rect_height(viewport) : 0.0f;
    const float scroll = minimum(maximum(requested_scroll_dips, 0.0f), maximum_scroll);
    if (!source_changed && scene->active && ui_rect_equal(scene->viewport, viewport) &&
        scene->scroll_dips == scroll) {
        return ShellNavigationSceneResult{ShellNavigationSceneError::none, scene->first, scene->count,
                                          scroll, false};
    }

    float prefix = 0.0f;
    u32 first_visible = 0;
    while (first_visible < view->channel_count) {
        const float height = shell_navigation_row_height(view->channel_rows[first_visible]);
        if (inset + prefix + height > scroll) {
            break;
        }
        prefix += height;
        ++first_visible;
    }
    u32 first = first_visible;
    float first_prefix = prefix;
    for (u32 overscan = 0; overscan < kShellNavigationOverscanRows && first != 0; ++overscan) {
        --first;
        first_prefix -= shell_navigation_row_height(view->channel_rows[first]);
    }

    float row_top = inset + first_prefix - scroll;
    u32 end = first;
    while (end < view->channel_count && row_top < ui_rect_height(viewport)) {
        row_top += shell_navigation_row_height(view->channel_rows[end]);
        ++end;
    }
    end = minimum(view->channel_count, end + kShellNavigationOverscanRows);
    const u32 row_count = end - first;
    const bool include_spacer = first == 0;
    u32 mention_badges = 0;
    for (u32 local = 0; local < row_count; ++local) {
        if (view->channel_rows[first + local].mention_count != 0) {
            ++mention_badges;
        }
    }
    const u32 required_widgets = 1 + (include_spacer ? 1u : 0u) + row_count * 3u + mention_badges;
    if (row_count > scene->rows.count &&
        (!scene->row_storage || !ui_virtual_row_storage_grow(scene->row_storage, row_count))) {
        return scene_error(scene, ShellNavigationSceneError::capacity);
    }
    if (scene->row_storage) {
        scene->rows = ui_virtual_row_storage_rows<ShellNavigationRowWidget>(*scene->row_storage);
    }
    if (row_count > scene->rows.count || !ui_widget_manager_reserve(&scene->widgets, required_widgets)) {
        return scene_error(scene, ShellNavigationSceneError::capacity);
    }

    const float local_scroll = include_spacer ? scroll : maximum(0.0f, scroll - (inset + first_prefix));
    if (!source_changed && scene->active && scene->first == first && scene->count == row_count) {
        if (!ui_widget_manager_set_scroll(&scene->widgets, scene->root, UiPoint{0.0f, local_scroll}) ||
            !ui_widget_manager_layout_rectangle(&scene->widgets, viewport).ok()) {
            return scene_error(scene, ShellNavigationSceneError::layout);
        }
        scene->viewport = viewport;
        scene->content_height = content_height;
        scene->scroll_dips = scroll;
        saturating_increment(scene->layout_passes);
        return ShellNavigationSceneResult{ShellNavigationSceneError::none, first, row_count, scroll, true};
    }

    const u64 restore_focus = focused_channel(*scene);
    ui_widget_manager_reset(&scene->widgets);
    const UiWidgetResult root = ui_widget_manager_add_keyed(
        &scene->widgets, ui_widget_invalid_handle(), kNavigationRootWidgetId, UiWidgetKind::scroll_view,
        ui_widget_enabled, "ChannelNavigation", ui_layout_scroll_root_style());
    if (!root.ok()) {
        return teardown_error(scene, ShellNavigationSceneError::widget);
    }
    scene->root = root.handle;
    if (include_spacer) {
        const UiWidgetResult spacer = ui_widget_manager_add_keyed(
            &scene->widgets, root.handle, kNavigationSpacerWidgetId, UiWidgetKind::container, ui_widget_enabled,
            "TopInset", ui_layout_spacer_style(inset));
        if (!spacer.ok()) {
            return teardown_error(scene, ShellNavigationSceneError::widget);
        }
    }
    for (u32 local = 0; local < row_count; ++local) {
        const u32 source_index = first + local;
        const ChannelViewRow& source = view->channel_rows[source_index];
        const u16 flags = static_cast<u16>(ui_widget_enabled | ui_widget_hit_test |
                                           (row_focusable(source) ? ui_widget_focusable : 0));
        const u32 widget_id = source_index * 4u + 3u;
        const Utf8View row_name = source.type == 4 ? utf8_literal("ChannelCategory") : utf8_literal("ChannelRow");
        const UiWidgetResult added = ui_widget_manager_add_keyed(
            &scene->widgets, root.handle, widget_id, UiWidgetKind::button, flags, row_name.data, row_name.bytes,
            row_style(source));
        if (!added.ok()) {
            return teardown_error(scene, ShellNavigationSceneError::widget);
        }
        const UiWidgetResult icon = ui_widget_manager_add_keyed(
            &scene->widgets, added.handle, widget_id + 1u, UiWidgetKind::image, ui_widget_enabled, "ChannelIcon",
            icon_style(source));
        const UiWidgetResult label = ui_widget_manager_add_keyed(
            &scene->widgets, added.handle, widget_id + 2u, UiWidgetKind::text, ui_widget_enabled, "ChannelLabel",
            label_style(source));
        if (!icon.ok() || !label.ok()) {
            return teardown_error(scene, ShellNavigationSceneError::widget);
        }
        UiWidgetHandle mention_badge = ui_widget_invalid_handle();
        if (source.mention_count != 0) {
            const UiWidgetResult badge = ui_widget_manager_add_keyed(
                &scene->widgets, added.handle, widget_id + 3u, UiWidgetKind::custom, ui_widget_enabled,
                "MentionBadge", mention_badge_style(source.mention_count, channel_is_private(source.type)));
            if (!badge.ok()) {
                return teardown_error(scene, ShellNavigationSceneError::widget);
            }
            mention_badge = badge.handle;
        }
        scene->rows[local] = ShellNavigationRowWidget{source,       added.handle,  icon.handle,
                                                      label.handle, mention_badge, source_index};
    }

    if (!ui_widget_manager_set_scroll(&scene->widgets, root.handle, UiPoint{0.0f, local_scroll}) ||
        !ui_widget_manager_layout_rectangle(&scene->widgets, viewport).ok()) {
        return teardown_error(scene, ShellNavigationSceneError::layout);
    }

    scene->viewport = viewport;
    scene->source_rows = view->channel_rows;
    scene->source_revision = view->channel_revision;
    scene->source_count = view->channel_count;
    scene->content_height = content_height;
    scene->scroll_dips = scroll;
    scene->first = first;
    scene->count = row_count;
    scene->focused_channel_id = restore_focus;
    scene->active = true;
    saturating_increment(scene->rebuilds);
    saturating_increment(scene->layout_passes);
    if (restore_focus != 0) {
        shell_navigation_scene_focus_channel(scene, restore_focus);
    }
    return ShellNavigationSceneResult{ShellNavigationSceneError::none, first, row_count, scroll, true};
}

bool shell_navigation_scene_set_banner(ShellNavigationScene* scene, float inset, bool header_hovered,
                                       u32 revision) noexcept {
    if (!scene || !ui_extent_valid(inset))
        return false;
    const bool changed = scene->banner_inset != inset || scene->banner_header_hovered != header_hovered ||
                         scene->banner_revision != revision;
    if (scene->banner_inset != inset) {
        // The top inset sizes the spacer and the content: rebuild the rows.
        scene->banner_inset = inset;
        scene->source_rows = nullptr;
    }
    scene->banner_header_hovered = header_hovered;
    scene->banner_revision = revision;
    return changed;
}

ShellNavigationSceneResult shell_navigation_scene_scroll(ShellNavigationScene* scene,
                                                         const NavigationView* view,
                                                         float delta_dips) noexcept {
    if (!scene || !ui_coordinate_valid(delta_dips)) {
        return scene_error(scene, ShellNavigationSceneError::invalid_input);
    }
    return shell_navigation_scene_update(scene, view, scene->viewport, scene->scroll_dips + delta_dips);
}

const ShellNavigationRowWidget* shell_navigation_scene_resolve(const ShellNavigationScene& scene,
                                                               UiWidgetHandle widget) noexcept {
    if (!ui_widget_manager_resolve(scene.widgets, widget)) {
        return nullptr;
    }
    for (u32 index = 0; index < scene.count; ++index) {
        const ShellNavigationRowWidget& row = scene.rows[index];
        if (ui_widget_handle_equal(row.widget, widget) || ui_widget_handle_equal(row.icon, widget) ||
            ui_widget_handle_equal(row.label, widget) || ui_widget_handle_equal(row.mention_badge, widget)) {
            return &row;
        }
    }
    return nullptr;
}

const UiLayoutNode* shell_navigation_scene_layout_node(const ShellNavigationScene& scene,
                                                       const ShellNavigationRowWidget& row) noexcept {
    return ui_widget_manager_layout_node(scene.widgets, row.widget);
}

ShellNavigationHit shell_navigation_scene_hit_test(const ShellNavigationScene& scene, float x,
                                                   float y) noexcept {
    const UiWidgetHandle widget = ui_widget_manager_hit_test(scene.widgets, x, y);
    const ShellNavigationRowWidget* row = shell_navigation_scene_resolve(scene, widget);
    return row ? ShellNavigationHit{row->view.id, row->widget, row->source_index, row->view.selectable}
               : ShellNavigationHit{};
}

bool shell_navigation_scene_focus_channel(ShellNavigationScene* scene, u64 channel_id) noexcept {
    if (!scene) {
        return false;
    }
    if (channel_id == 0) {
        scene->focused_channel_id = 0;
        return ui_widget_manager_focus(&scene->widgets, ui_widget_invalid_handle());
    }
    for (u32 index = 0; index < scene->count; ++index) {
        const ShellNavigationRowWidget& row = scene->rows[index];
        if (row.view.id == channel_id && row_focusable(row.view) &&
            ui_widget_manager_focus(&scene->widgets, row.widget)) {
            scene->focused_channel_id = channel_id;
            return true;
        }
    }
    return false;
}

bool shell_navigation_scene_reveal_channel(ShellNavigationScene* scene, const NavigationView& view,
                                           u64 channel_id) noexcept {
    if (!scene || !scene->active || !channel_id || view.channel_count > view.channel_capacity ||
        (view.channel_count && !view.channel_rows))
        return false;
    float top = shell_navigation_top_inset(*scene);
    for (u32 i = 0; i < view.channel_count; ++i) {
        const auto& row = view.channel_rows[i];
        const float extent = shell_navigation_row_height(row);
        if (row.id == channel_id) {
            if (!row_focusable(row))
                return false;
            const float height = ui_rect_height(scene->viewport);
            float scroll = scene->scroll_dips;
            if (!ui_scroll_reveal_offset(top, top + extent, scroll, scroll + height, scroll,
                                         maximum(0.0f, scene->content_height - height),
                                         &scroll))
                return false;
            return shell_navigation_scene_update(scene, &view, scene->viewport, scroll).ok() &&
                   shell_navigation_scene_focus_channel(scene, channel_id);
        }
        top += extent;
    }
    return false;
}

ShellNavigationHit shell_navigation_scene_page(ShellNavigationScene* scene,
                                               const NavigationView& view, bool reverse) noexcept {
    if (!scene || !scene->active || !ui_coordinate_valid(scene->scroll_dips) || scene->scroll_dips < 0 ||
        !ui_rect_valid(scene->viewport) || view.channel_count > view.channel_capacity || !view.channel_rows)
        return {};
    struct Source {
        const NavigationView* view;
        u64 first;
        u32 index;
    } source{&view, ui_extent_units(shell_navigation_top_inset(*scene)), 0};
    u32 current = kUiListNoSelection;
    for (u32 i = 0; i < view.channel_count; ++i)
        if (view.channel_rows[i].id == scene->focused_channel_id) {
            current = i;
            break;
        }
    const auto target = ui_list_page(
        view.channel_count, current, ui_extent_units(scene->scroll_dips),
        ui_extent_units(ui_rect_height(scene->viewport)),
        ui_extent_units(scene->content_height), reverse,
        [](void* context, u32 index) noexcept {
            auto& cursor = *static_cast<Source*>(context);
            const auto& model = *cursor.view;
            while (cursor.index < index)
                cursor.first +=
                    ui_extent_units(shell_navigation_row_height(model.channel_rows[cursor.index++]));
            while (cursor.index > index)
                cursor.first -=
                    ui_extent_units(shell_navigation_row_height(model.channel_rows[--cursor.index]));
            return UiListRowBounds{cursor.first, cursor.first + ui_extent_units(shell_navigation_row_height(
                                                                    model.channel_rows[index]))};
        },
        [](void* context, u32 index) noexcept {
            const auto& row = static_cast<Source*>(context)->view->channel_rows[index];
            return row_focusable(row);
        },
        &source);
    if (!target.ok())
        return {};
    const u64 id = view.channel_rows[target.index].id;
    if (!shell_navigation_scene_update(scene, &view, scene->viewport, ui_extent_dips(target.scroll_units))
             .ok() ||
        !shell_navigation_scene_focus_channel(scene, id))
        return {};
    return {id, scene->widgets.focused, target.index, view.channel_rows[target.index].selectable};
}

} // namespace maicord
