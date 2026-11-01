#pragma once
// Caller-storage initializers for UI scenes. The shipping client builds its popup and message scenes
// from owned arenas, so the fixed-capacity variants the tests rely on live here, not in the UI library.
#include "maicord/core/memory.hpp"
#include "maicord/ui/message_scene.hpp"
#include "maicord/ui/popup_scene.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/ui/unread_divider_style.hpp"

namespace maicord {
inline bool popup_scene_initialize(PopupScene* scene, Slice<PopupItem> items, Slice<PopupRowWidget> rows,
                                   Slice<UiWidget> widgets, Slice<UiLayoutNode> layout_nodes) noexcept {
    if (!scene || !items.data || items.count == 0 || !rows.data || rows.count < items.count ||
        !widgets.data || widgets.count < 2 || !layout_nodes.data || layout_nodes.count < widgets.count) {
        return false;
    }
    reset_value<PopupScene>(scene);
    scene->items = items;
    scene->rows = rows;
    scene->selected_index = ~0u;
    return ui_widget_manager_initialize(&scene->widgets, widgets, layout_nodes);
}

inline bool message_scene_initialize(MessageScene* scene, MessageTimeline* timeline,
                                     Slice<MessageSceneRowWidget> row_storage, Slice<UiWidget> widget_storage,
                                     Slice<UiLayoutNode> layout_storage) noexcept {
    if (!scene || !timeline || !timeline->store || !timeline->list || !row_storage.data ||
        row_storage.count == 0 || !widget_storage.data || !layout_storage.data ||
        widget_storage.count == 0 || layout_storage.count == 0) {
        return false;
    }
    reset_value<MessageScene>(scene);
    scene->rows = row_storage;
    scene->timeline = timeline;
    scene->root = ui_widget_invalid_handle();
    scene->unread_divider_style = kDefaultUiUnreadDividerStyle;
    scene->unread_prepared_height = kDefaultUiUnreadDividerStyle.height;
    return ui_widget_manager_initialize(&scene->widgets, widget_storage, layout_storage);
}

// Fixed-storage hosts supply the child rows of forwarded snapshots; the owned host uses its row allocator.
inline bool message_scene_bind_snapshot_rows(MessageScene* scene, Slice<MessageSceneRowWidget> rows) noexcept {
    if (!scene || scene->active || scene->owns_rows || (!rows.data && rows.count) ||
        rows.count > kInvalidUiWidgetSlot)
        return false;
    scene->snapshot_rows = rows;
    return true;
}

// Opens a registered settings page: selects it, rebuilds the scene over its content and focuses its row.
inline bool ui_settings_scene_select(UiSettingsScene* scene, UiPageKey page) noexcept {
    if (!scene || !scene->active || !scene->registry || !ui_page_registry_select(scene->registry, page).ok())
        return false;
    if (!ui_settings_scene_update(scene, scene->registry, scene->viewport))
        return false;
    return ui_settings_scene_focus(scene, {UiSettingsTargetKind::page, page, 0});
}
} // namespace maicord
