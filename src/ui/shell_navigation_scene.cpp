#include "maicord/ui/shell_navigation_scene.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/mention_badge.hpp"
#include "maicord/ui/widget_scroll.hpp"
#include "maicord/ui/list_navigation.hpp"
#include "maicord/ui/virtual_list.hpp"
#include "maicord/ui/private_channel_rows.hpp"

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
    // Original icon-pack glyphs: 16-DIP channel icons and a 12-DIP category
    // chevron, centred in the 32-DIP head and 28-DIP category rows.
    const float extent = private_channel ? kUiPrivateRowAvatar : category ? 12.0f : 16.0f;
    style.width = ui_layout_fixed(extent);
    style.height = ui_layout_fixed(extent);
    style.margin.left = (private_channel ? 16.0f : category ? 16.0f : 12.0f) + static_cast<float>(row.depth) * 12.0f;
    style.margin.top = ((category ? 28.0f : private_channel ? kUiPrivateRowHeight : kShellNavigationVoiceHeadHeight) -
                        extent) * 0.5f;
    return style;
}

UiLayoutStyle label_style(const ChannelViewRow& row) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fill();
    style.height = ui_layout_fill();
    style.horizontal_alignment = UiLayoutAlignment::stretch;
    style.vertical_alignment = UiLayoutAlignment::stretch;
    style.margin = UiInsets{
        (channel_is_private(row.type) ? 60.0f : 34.0f) + static_cast<float>(row.depth) * 12.0f,
        channel_is_private(row.type) ? 0.0f : 5.0f,
        row.mention_count != 0 ? ui_mention_badge_width(row.mention_count) + 18.0f
        : channel_is_private(row.type) ? 40.0f : 12.0f, 0.0f};
    style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_text_content);
    return style;
}

UiLayoutStyle mention_badge_style(u32 count, bool private_channel) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fixed(ui_mention_badge_width(count));
    style.height = ui_layout_fixed(kUiMentionBadgeHeight);
    style.margin.right = 10.0f;
    style.margin.top = private_channel ? (kUiPrivateRowHeight - kUiMentionBadgeHeight) * 0.5f : 8.0f;
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

u64 focused_channel(const ShellNavigationScene& scene) noexcept {
    const ShellNavigationRowWidget* focused = shell_navigation_scene_resolve(scene, scene.widgets.focused);
    return focused ? focused->view.id : scene.focused_channel_id;
}

} // namespace

ShellThreadSpine shell_navigation_thread_spine(UiRect row, float row_height, UiRect icon, bool first,
                                               bool continues) noexcept {
    const float x = (icon.left + icon.right) * 0.5f - 12.0f;
    const float y = row.top + row_height * 0.5f;
    const float r = kShellThreadSpineRadius;
    // The parent icon ends 8 DIP above this row; leave a 2 DIP gap below it.
    const float top = first ? row.top - 6.0f : row.top;
    return {{x, top}, {x, y - r}, {x + r, y}, {icon.right - 4.0f, y}, continues ? row.bottom : y - r};
}

float shell_navigation_row_height(const ChannelViewRow& row) noexcept {
    if (channel_is_private(row.type))
        return kUiPrivateRowHeight;
    return row.type == 4 ? 28.0f
                         : shell_navigation_voice_top(row) +
                               static_cast<float>(row.voice_count) * kShellNavigationVoiceUserHeight +
                               (row.voice_invite ? kShellNavigationVoiceInviteHeight : 0.0f);
}

float shell_navigation_content_height(const NavigationView* view) noexcept {
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
                                                         const NavigationView* view, UiRect viewport,
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
        const UiWidget* retained_root = ui_widget_manager_resolve(scene->widgets, scene->root);
        if (!retained_root ||
            !ui_layout_engine_set_scroll(&scene->widgets.layout, retained_root->layout,
                                         UiPoint{0.0f, local_scroll}) ||
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
    const UiWidgetResult root = ui_widget_manager_add_prevalidated(
        &scene->widgets, ui_widget_invalid_handle(),
        UiWidgetDesc{kNavigationRootWidgetId, UiWidgetKind::scroll_view, ui_widget_enabled,
                     UiLayoutNodeSpec{utf8_literal("ChannelNavigation"), ui_layout_scroll_root_style(), UiSize{}, 0.0f,
                                      kNavigationRootWidgetId}});
    if (!root.ok()) {
        clear_materialization(scene);
        return scene_error(scene, ShellNavigationSceneError::widget);
    }
    scene->root = root.handle;
    if (include_spacer) {
        const UiWidgetResult spacer = ui_widget_manager_add_prevalidated(
            &scene->widgets, root.handle,
            UiWidgetDesc{kNavigationSpacerWidgetId, UiWidgetKind::container, ui_widget_enabled,
                         UiLayoutNodeSpec{utf8_literal("TopInset"), ui_layout_spacer_style(inset), UiSize{}, 0.0f,
                                          kNavigationSpacerWidgetId}});
        if (!spacer.ok()) {
            clear_materialization(scene);
            return scene_error(scene, ShellNavigationSceneError::widget);
        }
    }
    for (u32 local = 0; local < row_count; ++local) {
        const u32 source_index = first + local;
        const ChannelViewRow& source = view->channel_rows[source_index];
        const u16 flags = static_cast<u16>(ui_widget_enabled | ui_widget_hit_test |
                                           (row_focusable(source) ? ui_widget_focusable : 0));
        const u32 widget_id = source_index * 4u + 3u;
        const UiWidgetResult added = ui_widget_manager_add_prevalidated(
            &scene->widgets, root.handle,
            UiWidgetDesc{widget_id, UiWidgetKind::button, flags,
                         UiLayoutNodeSpec{source.type == 4 ? utf8_literal("ChannelCategory")
                                                           : utf8_literal("ChannelRow"),
                                          row_style(source), UiSize{}, 0.0f, widget_id}});
        if (!added.ok()) {
            clear_materialization(scene);
            return scene_error(scene, ShellNavigationSceneError::widget);
        }
        const UiWidgetResult icon = ui_widget_manager_add_prevalidated(
            &scene->widgets, added.handle,
            UiWidgetDesc{widget_id + 1u, UiWidgetKind::image, ui_widget_enabled,
                         UiLayoutNodeSpec{utf8_literal("ChannelIcon"), icon_style(source), UiSize{}, 0.0f,
                                          widget_id + 1u}});
        const UiWidgetResult label = ui_widget_manager_add_prevalidated(
            &scene->widgets, added.handle,
            UiWidgetDesc{widget_id + 2u, UiWidgetKind::text, ui_widget_enabled,
                         UiLayoutNodeSpec{utf8_literal("ChannelLabel"), label_style(source), UiSize{}, 0.0f,
                                          widget_id + 2u}});
        if (!icon.ok() || !label.ok()) {
            clear_materialization(scene);
            return scene_error(scene, ShellNavigationSceneError::widget);
        }
        UiWidgetHandle mention_badge = ui_widget_invalid_handle();
        if (source.mention_count != 0) {
            const UiWidgetResult badge = ui_widget_manager_add_prevalidated(
                &scene->widgets, added.handle,
                UiWidgetDesc{widget_id + 3u, UiWidgetKind::custom, ui_widget_enabled,
                             UiLayoutNodeSpec{utf8_literal("MentionBadge"),
                                              mention_badge_style(source.mention_count, channel_is_private(source.type)), UiSize{}, 0.0f,
                                              widget_id + 3u}});
            if (!badge.ok()) {
                clear_materialization(scene);
                return scene_error(scene, ShellNavigationSceneError::widget);
            }
            mention_badge = badge.handle;
        }
        scene->rows[local] = ShellNavigationRowWidget{source,       added.handle,  icon.handle,
                                                      label.handle, mention_badge, source_index};
    }

    const UiWidget* root_widget = ui_widget_manager_resolve(scene->widgets, root.handle);
    if (!root_widget ||
        !ui_layout_engine_set_scroll(&scene->widgets.layout, root_widget->layout,
                                     UiPoint{0.0f, local_scroll}) ||
        !ui_widget_manager_layout_rectangle(&scene->widgets, viewport).ok()) {
        clear_materialization(scene);
        return scene_error(scene, ShellNavigationSceneError::layout);
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
        const bool row_match = row.widget.slot == widget.slot && row.widget.generation == widget.generation;
        const bool icon_match = row.icon.slot == widget.slot && row.icon.generation == widget.generation;
        const bool label_match = row.label.slot == widget.slot && row.label.generation == widget.generation;
        const bool badge_match =
            row.mention_badge.slot == widget.slot && row.mention_badge.generation == widget.generation;
        if (row_match || icon_match || label_match || badge_match) {
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
