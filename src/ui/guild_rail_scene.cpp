#include "maicord/ui/guild_rail_scene.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "maicord/ui/widget_scroll.hpp"
#include "maicord/ui/list_navigation.hpp"
#include "maicord/ui/virtual_list.hpp"

namespace maicord {
namespace {

inline constexpr u32 kGuildRailRootWidgetId = 1;
inline constexpr u32 kGuildRailSpacerWidgetId = 2;
// Row widgets use source index + 3; the add button sits above any row ID.
inline constexpr u32 kGuildRailAddWidgetId = 0x7FFF0000u;
inline constexpr float kGuildRailTopInset = 12.0f;
inline constexpr float kGuildRailRowExtent = 40.0f;
inline constexpr float kGuildRailRowGap = kGuildRailRowGapDips;
inline constexpr float kGuildRailRowStride = kGuildRailRowExtent + kGuildRailRowGap;
inline constexpr float kGuildRailOverscanDips = kGuildRailOverscanRows * kGuildRailRowStride;
inline constexpr UiMotionStyle kFolderMotion{kGuildRailFolderMotionMs, UiMotionCurve::css_ease_out};

GuildRailSceneResult scene_error(GuildRailScene* scene, GuildRailSceneError error) noexcept {
    saturating_increment_if(scene, scene->rejected);
    return GuildRailSceneResult{error, scene ? scene->first : 0, scene ? scene->count : 0,
                                scene ? scene->scroll_dips : 0.0f, false};
}

UiLayoutStyle row_style(float reveal) noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fixed(kGuildRailRowExtent);
    style.height = ui_layout_fixed(kGuildRailRowExtent * reveal);
    style.margin.left = 16.0f;
    style.margin.bottom = kGuildRailRowGap * reveal;
    return style;
}

UiMotionTransition transition_of(const GuildRailFolderMotion& slot) noexcept {
    return {{slot.folder_id, 1}, slot.started_ms, slot.sampled_ms, slot.value, slot.from, slot.target,
            kFolderMotion, slot.active};
}
void store_transition(GuildRailFolderMotion* slot, const UiMotionTransition& t) noexcept {
    slot->started_ms = t.started_ms;
    slot->sampled_ms = t.sampled_ms;
    slot->value = t.value;
    slot->from = t.from;
    slot->target = t.target;
    slot->active = t.active;
}
const GuildRailFolderMotion* folder_slot(const GuildRailScene& scene, u64 folder_id) noexcept {
    for (const auto& slot : scene.folder_motion)
        if (slot.folder_id == folder_id && folder_id)
            return &slot;
    return nullptr;
}

// Settled extents use only the view; the scene adds running transitions.
float reveal_of(const GuildRailScene* scene, const GuildViewRow& row) noexcept {
    if (row.folder_role != GuildFolderRole::member)
        return 1.0f;
    if (scene)
        if (const auto* slot = folder_slot(*scene, row.folder_id); slot && slot->active)
            return slot->value;
    return row.folder_expanded ? 1.0f : 0.0f;
}

float content_height(const GuildRailScene* scene, const NavigationView* view) noexcept {
    if (!view)
        return 0.0f;
    float height = kGuildRailTopInset;
    for (u32 i = 0; i < view->guild_count; ++i)
        height += reveal_of(scene, view->guild_rows[i]) * kGuildRailRowStride;
    return height + kGuildRailRowStride; // "Add a server"
}

// Content-space top of a source row, plus its stride.
float row_top(const GuildRailScene* scene, const NavigationView& view, u32 index) noexcept {
    float top = kGuildRailTopInset;
    for (u32 i = 0; i < index && i < view.guild_count; ++i)
        top += reveal_of(scene, view.guild_rows[i]) * kGuildRailRowStride;
    return top;
}

struct VisibleRange {
    u32 first, count;
    float spacer;
    bool add; // the trailing add button intersects the overscanned viewport
};
VisibleRange visible_range(const GuildRailScene& scene, const NavigationView& view, float scroll,
                           float height) noexcept {
    VisibleRange range{0, 0, kGuildRailTopInset, false};
    const float low = scroll - kGuildRailOverscanDips, high = scroll + height + kGuildRailOverscanDips;
    float top = kGuildRailTopInset;
    bool started = false;
    for (u32 i = 0; i < view.guild_count; ++i) {
        const float stride = reveal_of(&scene, view.guild_rows[i]) * kGuildRailRowStride;
        if (stride > 0 && top + stride > low && top < high) {
            if (!started) {
                started = true;
                range.first = i;
                range.spacer = top;
            }
            ++range.count;
        } else if (started && top >= high) {
            return range;
        }
        top += stride;
    }
    range.add = top + kGuildRailRowStride > low && top < high;
    if (range.add && !started) {
        // No row is visible: the spacer reaches the button itself.
        range.first = view.guild_count;
        range.spacer = top;
    }
    return range;
}

// Materialized rows skip zero-extent (collapsed) members.
template <typename Visit> void each_visible(const GuildRailScene& scene, const NavigationView& view,
                                            VisibleRange range, Visit visit) noexcept {
    u32 local = 0;
    for (u32 i = range.first; i < view.guild_count && local < range.count; ++i)
        if (reveal_of(&scene, view.guild_rows[i]) > 0)
            visit(local++, i);
}

void clear_materialization(GuildRailScene* scene) noexcept {
    ui_widget_manager_reset(&scene->widgets);
    scene->root = ui_widget_invalid_handle();
    scene->add_button = ui_widget_invalid_handle();
    scene->first = 0;
    scene->count = 0;
    scene->active = false;
}

u64 focused_guild(const GuildRailScene& scene) noexcept {
    const GuildRailRowWidget* focused = guild_rail_scene_resolve(scene, scene.widgets.focused);
    return focused ? focused->view.id : scene.focused_guild_id;
}

bool materialization_matches(const GuildRailScene& scene, const NavigationView& view,
                             VisibleRange range) noexcept {
    if (!scene.active || scene.first != range.first || scene.count != range.count ||
        scene.spacer_height != range.spacer || (scene.add_button.generation != 0) != range.add)
        return false;
    bool matches = true;
    each_visible(scene, view, range, [&](u32 local, u32 index) noexcept {
        const GuildRailRowWidget& retained = scene.rows[local];
        const GuildViewRow& source = view.guild_rows[index];
        matches = matches && retained.source_index == index && retained.view.id == source.id &&
                  retained.view.generation == source.generation &&
                  retained.reveal == reveal_of(&scene, source);
    });
    return matches;
}

u16 row_flags(const GuildViewRow& source) noexcept {
    return static_cast<u16>(ui_widget_enabled | ui_widget_hit_test |
                            (!source.unavailable ? ui_widget_focusable : 0));
}

void update_retained_rows(GuildRailScene* scene, const NavigationView& view, VisibleRange range) noexcept {
    each_visible(*scene, view, range, [&](u32 local, u32 index) noexcept {
        GuildRailRowWidget& retained = scene->rows[local];
        const GuildViewRow& source = view.guild_rows[index];
        retained.view = source;
        retained.source_index = index;
        UiWidget* widget = ui_widget_manager_resolve(&scene->widgets, retained.widget);
        if (widget)
            widget->flags = row_flags(source);
        if (source.unavailable && scene->widgets.focused.slot == retained.widget.slot &&
            scene->widgets.focused.generation == retained.widget.generation) {
            ui_widget_manager_focus(&scene->widgets, ui_widget_invalid_handle());
            scene->focused_guild_id = 0;
        }
    });
}

GuildRailHit hit_of(const GuildRailRowWidget& row) noexcept {
    const bool folder = row.view.folder_role == GuildFolderRole::header;
    const bool conversation = row.view.folder_role == GuildFolderRole::conversation;
    return {row.view.id, row.widget, row.source_index, !row.view.unavailable && !folder && !conversation, folder,
            false, conversation};
}

} // namespace

bool guild_rail_scene_initialize(GuildRailScene* scene, Slice<GuildRailRowWidget> row_storage,
                                 Slice<UiWidget> widget_storage,
                                 Slice<UiLayoutNode> layout_storage) noexcept {
    if (!scene || !row_storage.data || row_storage.count == 0 || !widget_storage.data ||
        !layout_storage.data) {
        return false;
    }
    *scene = GuildRailScene{};
    scene->rows = row_storage;
    scene->root = ui_widget_invalid_handle();
    return ui_widget_manager_initialize(&scene->widgets, widget_storage, layout_storage);
}

GuildRailSceneResult guild_rail_scene_update(GuildRailScene* scene, const NavigationView* view,
                                             UiRect viewport, float requested_scroll_dips) noexcept {
    if (!scene || !scene->rows.data || !scene->widgets.storage.data || !ui_rect_valid_or_empty(viewport) ||
        !ui_coordinate_valid(requested_scroll_dips) ||
        (view &&
         (view->guild_count > view->guild_capacity || (view->guild_count != 0 && !view->guild_rows)))) {
        return scene_error(scene, GuildRailSceneError::invalid_input);
    }

    if (!view || !ui_rect_has_area(viewport)) {
        const bool materialized =
            scene->active || scene->count != 0 || scene->widgets.count != 0 || scene->root.generation != 0;
        const bool changed = materialized || !ui_rect_equal(scene->viewport, viewport);
        if (materialized)
            clear_materialization(scene);
        scene->viewport = viewport;
        scene->source_rows = view ? view->guild_rows : nullptr;
        scene->source_revision = view ? view->guild_revision : 0;
        scene->source_count = view ? view->guild_count : 0;
        scene->content_height = content_height(scene, view);
        scene->scroll_dips = 0.0f;
        if (!view) {
            scene->focused_guild_id = 0;
        }
        return GuildRailSceneResult{GuildRailSceneError::none, 0, 0, 0.0f, changed};
    }

    const bool source_changed = scene->source_rows != view->guild_rows ||
                                scene->source_revision != view->guild_revision ||
                                scene->source_count != view->guild_count;
    // Folder transitions change extents without a source revision.
    const float height = content_height(scene, view);
    const float maximum_scroll =
        height > ui_rect_height(viewport) ? height - ui_rect_height(viewport) : 0.0f;
    const float scroll = minimum(maximum(requested_scroll_dips, 0.0f), maximum_scroll);
    if (!source_changed && scene->active && ui_rect_equal(scene->viewport, viewport) &&
        scene->scroll_dips == scroll && scene->content_height == height) {
        return GuildRailSceneResult{GuildRailSceneError::none, scene->first, scene->count, scroll, false};
    }

    VisibleRange range = visible_range(*scene, *view, scroll, ui_rect_height(viewport));
    if (range.count > scene->rows.count &&
        (!scene->row_storage || !ui_virtual_row_storage_grow(scene->row_storage, range.count))) {
        return scene_error(scene, GuildRailSceneError::capacity);
    }
    if (scene->row_storage) {
        scene->rows = ui_virtual_row_storage_rows<GuildRailRowWidget>(*scene->row_storage);
    }
    // Rows come first: storage sized only for them leaves out the add button.
    if (range.add && !ui_widget_manager_reserve(&scene->widgets, 3 + range.count))
        range.add = false;
    if (range.count > scene->rows.count || !ui_widget_manager_reserve(&scene->widgets, 2 + range.count)) {
        return scene_error(scene, GuildRailSceneError::capacity);
    }

    if (materialization_matches(*scene, *view, range)) {
        const bool geometry_changed = !ui_rect_equal(scene->viewport, viewport) || scene->scroll_dips != scroll;
        update_retained_rows(scene, *view, range);
        if (geometry_changed) {
            const UiWidget* retained_root = ui_widget_manager_resolve(scene->widgets, scene->root);
            if (!retained_root ||
                !ui_layout_engine_set_scroll(&scene->widgets.layout, retained_root->layout,
                                             UiPoint{0.0f, scroll}) ||
                !ui_widget_manager_layout_rectangle(&scene->widgets, viewport).ok()) {
                return scene_error(scene, GuildRailSceneError::layout);
            }
            saturating_increment(scene->layout_passes);
        }
        scene->viewport = viewport;
        scene->source_rows = view->guild_rows;
        scene->source_revision = view->guild_revision;
        scene->source_count = view->guild_count;
        scene->content_height = height;
        scene->scroll_dips = scroll;
        saturating_increment_if(source_changed, scene->data_updates);
        return GuildRailSceneResult{GuildRailSceneError::none, range.first, range.count, scroll, true};
    }

    u64 restore_focus = focused_guild(*scene);
    if (restore_focus == 0) {
        restore_focus = view->selected_guild_id;
    }
    // Indicator motion belongs to a guild, not a widget slot.
    GuildRailIndicator indicators[64]{};
    u64 indicator_ids[64]{};
    const u32 kept = minimum<u32>(scene->active ? scene->count : 0, 64);
    for (u32 i = 0; i < kept; ++i) {
        indicators[i] = scene->rows[i].indicator;
        indicator_ids[i] = scene->rows[i].view.id;
    }
    ui_widget_manager_reset(&scene->widgets);
    const UiWidgetResult root = ui_widget_manager_add_prevalidated(
        &scene->widgets, ui_widget_invalid_handle(),
        UiWidgetDesc{kGuildRailRootWidgetId, UiWidgetKind::scroll_view, ui_widget_enabled,
                     UiLayoutNodeSpec{utf8_literal("GuildRail"), ui_layout_scroll_root_style(), UiSize{}, 0.0f,
                                      kGuildRailRootWidgetId}});
    if (!root.ok()) {
        clear_materialization(scene);
        return scene_error(scene, GuildRailSceneError::widget);
    }
    scene->root = root.handle;
    const UiWidgetResult spacer = ui_widget_manager_add_prevalidated(
        &scene->widgets, root.handle,
        UiWidgetDesc{kGuildRailSpacerWidgetId, UiWidgetKind::container, ui_widget_enabled,
                     UiLayoutNodeSpec{utf8_literal("TopInset"), ui_layout_spacer_style(range.spacer), UiSize{}, 0.0f,
                                      kGuildRailSpacerWidgetId}});
    if (!spacer.ok()) {
        clear_materialization(scene);
        return scene_error(scene, GuildRailSceneError::widget);
    }
    bool added_all = true;
    each_visible(*scene, *view, range, [&](u32 local, u32 index) noexcept {
        if (!added_all)
            return;
        const GuildViewRow& source = view->guild_rows[index];
        const float reveal = reveal_of(scene, source);
        const u32 widget_id = index + 3;
        const UiWidgetResult added = ui_widget_manager_add_prevalidated(
            &scene->widgets, root.handle,
            UiWidgetDesc{widget_id, UiWidgetKind::button, row_flags(source),
                         UiLayoutNodeSpec{utf8_literal("GuildRow"), row_style(reveal), UiSize{}, 0.0f, widget_id}});
        if (!added.ok()) {
            added_all = false;
            return;
        }
        GuildRailIndicator indicator{};
        for (u32 k = 0; k < kept; ++k)
            if (indicator_ids[k] == source.id)
                indicator = indicators[k];
        scene->rows[local] = GuildRailRowWidget{source, added.handle, index, indicator, reveal};
    });
    if (!added_all) {
        clear_materialization(scene);
        return scene_error(scene, GuildRailSceneError::widget);
    }
    scene->add_button = ui_widget_invalid_handle();
    if (range.add) {
        const UiWidgetResult add = ui_widget_manager_add_prevalidated(
            &scene->widgets, root.handle,
            UiWidgetDesc{kGuildRailAddWidgetId, UiWidgetKind::button,
                         static_cast<u16>(ui_widget_enabled | ui_widget_hit_test),
                         UiLayoutNodeSpec{utf8_literal("AddServer"), row_style(1.0f), UiSize{}, 0.0f,
                                          kGuildRailAddWidgetId}});
        if (!add.ok()) {
            clear_materialization(scene);
            return scene_error(scene, GuildRailSceneError::widget);
        }
        scene->add_button = add.handle;
    }

    const UiWidget* root_widget = ui_widget_manager_resolve(scene->widgets, root.handle);
    if (!root_widget ||
        !ui_layout_engine_set_scroll(&scene->widgets.layout, root_widget->layout, UiPoint{0.0f, scroll}) ||
        !ui_widget_manager_layout_rectangle(&scene->widgets, viewport).ok()) {
        clear_materialization(scene);
        return scene_error(scene, GuildRailSceneError::layout);
    }

    scene->viewport = viewport;
    scene->source_rows = view->guild_rows;
    scene->source_revision = view->guild_revision;
    scene->source_count = view->guild_count;
    scene->content_height = height;
    scene->scroll_dips = scroll;
    scene->first = range.first;
    scene->count = range.count;
    scene->spacer_height = range.spacer;
    scene->focused_guild_id = restore_focus;
    scene->active = true;
    saturating_increment(scene->rebuilds);
    saturating_increment(scene->layout_passes);
    if (restore_focus != 0) {
        guild_rail_scene_focus_guild(scene, restore_focus);
    }
    return GuildRailSceneResult{GuildRailSceneError::none, range.first, range.count, scroll, true};
}

GuildRailSceneResult guild_rail_scene_scroll(GuildRailScene* scene, const NavigationView* view,
                                             float delta_dips) noexcept {
    if (!scene || !ui_coordinate_valid(delta_dips)) {
        return scene_error(scene, GuildRailSceneError::invalid_input);
    }
    return guild_rail_scene_update(scene, view, scene->viewport, scene->scroll_dips + delta_dips);
}

const GuildRailRowWidget* guild_rail_scene_resolve(const GuildRailScene& scene,
                                                   UiWidgetHandle widget) noexcept {
    if (!ui_widget_manager_resolve(scene.widgets, widget)) {
        return nullptr;
    }
    for (u32 index = 0; index < scene.count; ++index) {
        const GuildRailRowWidget& row = scene.rows[index];
        if (row.widget.slot == widget.slot && row.widget.generation == widget.generation) {
            return &row;
        }
    }
    return nullptr;
}

const UiLayoutNode* guild_rail_scene_layout_node(const GuildRailScene& scene,
                                                 const GuildRailRowWidget& row) noexcept {
    return ui_widget_manager_layout_node(scene.widgets, row.widget);
}

GuildRailHit guild_rail_scene_hit_test(const GuildRailScene& scene, float x, float y) noexcept {
    const UiWidgetHandle widget = ui_widget_manager_hit_test(scene.widgets, x, y);
    const GuildRailRowWidget* row = guild_rail_scene_resolve(scene, widget);
    if (!row && scene.add_button.generation && widget.slot == scene.add_button.slot &&
        widget.generation == scene.add_button.generation)
        return {0, widget, 0, false, false, true};
    return row ? hit_of(*row) : GuildRailHit{};
}

UiRect guild_rail_scene_add_rectangle(const GuildRailScene& scene) noexcept {
    const UiLayoutNode* node =
        scene.add_button.generation ? ui_widget_manager_layout_node(scene.widgets, scene.add_button) : nullptr;
    return node ? node->rectangle : UiRect{};
}

bool guild_rail_scene_focus_guild(GuildRailScene* scene, u64 guild_id) noexcept {
    if (!scene) {
        return false;
    }
    if (guild_id == 0) {
        scene->focused_guild_id = 0;
        return ui_widget_manager_focus(&scene->widgets, ui_widget_invalid_handle());
    }
    for (u32 index = 0; index < scene->count; ++index) {
        const GuildRailRowWidget& row = scene->rows[index];
        if (row.view.id == guild_id && !row.view.unavailable &&
            ui_widget_manager_focus(&scene->widgets, row.widget)) {
            scene->focused_guild_id = guild_id;
            return true;
        }
    }
    return false;
}

bool guild_rail_scene_reveal_guild(GuildRailScene* scene, const NavigationView& view, u64 guild_id) noexcept {
    if (!scene || !scene->active || !guild_id || view.guild_count > view.guild_capacity ||
        (view.guild_count && !view.guild_rows)) return false;
    for (u32 i = 0; i < view.guild_count; ++i) {
        if (view.guild_rows[i].id != guild_id) continue;
        if (view.guild_rows[i].unavailable) return false;
        // A guild inside a collapsed folder is represented by its header.
        u32 target = i;
        if (guild_view_row_hidden(view.guild_rows[i]))
            while (target && view.guild_rows[target].folder_role != GuildFolderRole::header)
                --target;
        const float top = row_top(scene, view, target);
        const float height = ui_rect_height(scene->viewport);
        float scroll = scene->scroll_dips;
        if (!ui_scroll_reveal_offset(top, top + kGuildRailRowExtent, scroll, scroll + height, scroll,
                maximum(0.0f, content_height(scene, &view) - height), &scroll)) return false;
        return guild_rail_scene_update(scene, &view, scene->viewport, scroll).ok() &&
            guild_rail_scene_focus_guild(scene, view.guild_rows[target].id);
    }
    return false;
}

GuildRailHit guild_rail_scene_page(GuildRailScene* scene, const NavigationView& view,
                                  bool reverse, bool from_home) noexcept {
    constexpr u32 kTops = 1024;
    if (!scene || !scene->active || !ui_coordinate_valid(scene->scroll_dips) || scene->scroll_dips < 0 ||
        !ui_rect_valid(scene->viewport) || view.guild_count > view.guild_capacity || !view.guild_rows ||
        view.guild_count > kTops) return {};
    struct Source {
        const NavigationView* view;
        u64 tops[kTops], extents[kTops];
    };
    ScratchPages<Source> scratch;
    if (!scratch.data) return {};
    Source& source = *scratch.data;
    source.view = &view;
    float top = kGuildRailTopInset;
    for (u32 i = 0; i < view.guild_count; ++i) {
        const float reveal = reveal_of(scene, view.guild_rows[i]);
        source.tops[i] = ui_extent_units(top);
        source.extents[i] = ui_extent_units(kGuildRailRowExtent * reveal);
        top += kGuildRailRowStride * reveal;
    }
    u32 current = kUiListNoSelection;
    if (!from_home)
        for (u32 i = 0; i < view.guild_count; ++i)
            if (view.guild_rows[i].id == scene->focused_guild_id) { current = i; break; }
    const auto target = ui_list_page(view.guild_count, current, ui_extent_units(scene->scroll_dips),
        ui_extent_units(ui_rect_height(scene->viewport)), ui_extent_units(content_height(scene, &view)), reverse,
        [](void* context, u32 index) noexcept {
            const auto& s = *static_cast<Source*>(context);
            return UiListRowBounds{s.tops[index], s.tops[index] + s.extents[index]};
        }, [](void* context, u32 index) noexcept {
            const auto& s = *static_cast<Source*>(context);
            const auto& row = s.view->guild_rows[index];
            return row.id && !row.unavailable && s.extents[index] != 0;
        }, &source);
    if (!target.ok()) return {};
    const u64 id = view.guild_rows[target.index].id;
    if (!guild_rail_scene_update(scene, &view, scene->viewport, ui_extent_dips(target.scroll_units)).ok() ||
        !guild_rail_scene_focus_guild(scene, id)) return {};
    const auto* row = guild_rail_scene_resolve(*scene, scene->widgets.focused);
    return row ? hit_of(*row) : GuildRailHit{id, scene->widgets.focused, target.index, true, false};
}

bool guild_rail_scene_folder_toggled(GuildRailScene* scene, u64 folder_id, bool expanded, u64 now_ms,
                                     bool animate) noexcept {
    if (!scene || !folder_id)
        return false;
    GuildRailFolderMotion* slot = nullptr;
    for (auto& candidate : scene->folder_motion)
        if (candidate.folder_id == folder_id)
            slot = &candidate;
    const float from = slot && slot->active ? slot->value : expanded ? 0.0f : 1.0f;
    if (!slot)
        for (auto& candidate : scene->folder_motion)
            if (!candidate.folder_id || !candidate.active) {
                slot = &candidate;
                break;
            }
    if (!slot)
        return false;
    const UiMotionScope scope{folder_id, 1};
    UiMotionTransition transition = slot->folder_id == folder_id ? transition_of(*slot) : UiMotionTransition{};
    if (slot->folder_id != folder_id) {
        slot->folder_id = folder_id;
        ui_motion_transition_reset(&transition, scope, from);
    }
    // Ticking with an unchanged sample time starts the deadline at now_ms.
    ui_motion_transition_tick(&transition, scope, expanded ? 1.0f : 0.0f, now_ms, kFolderMotion, !animate, true);
    store_transition(slot, transition);
    if (!slot->active)
        slot->folder_id = 0;
    scene->content_height = -1.0f; // force the next update to re-layout
    return true;
}

bool guild_rail_scene_tick(GuildRailScene* scene, u64 now_ms, bool animate) noexcept {
    if (!scene)
        return false;
    bool changed = false;
    for (auto& slot : scene->folder_motion) {
        if (!slot.folder_id)
            continue;
        const UiMotionScope scope{slot.folder_id, 1};
        auto transition = transition_of(slot);
        changed = ui_motion_transition_tick(&transition, scope, slot.target, now_ms, kFolderMotion, !animate, true) ||
                  changed;
        store_transition(&slot, transition);
        if (!slot.active) {
            slot.folder_id = 0;
            changed = true;
        }
    }
    return changed;
}

u64 guild_rail_scene_next_wake(const GuildRailScene& scene, u64 now_ms) noexcept {
    u64 wake = 0;
    for (const auto& slot : scene.folder_motion) {
        if (!slot.folder_id)
            continue;
        const u64 deadline = ui_motion_transition_next_wake(transition_of(slot), {slot.folder_id, 1}, now_ms, false, true);
        if (deadline && (!wake || deadline < wake))
            wake = deadline;
    }
    return wake;
}

} // namespace maicord
