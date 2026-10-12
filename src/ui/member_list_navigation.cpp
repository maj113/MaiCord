#include "maicord/ui/member_list_scene.hpp"
#include "maicord/ui/virtual_list.hpp"

namespace maicord {
namespace {
bool source_valid(const MemberListScene* scene) noexcept {
    return scene && scene->active && scene->source && scene->source->rows.data &&
        (!scene->source->authoritative ? scene->source->count <= scene->source->rows.count : scene->source->resident_count <= scene->source->rows.count) && scene->members && scene->members->records &&
        scene->scroll_dips == scene->scroll_dips && scene->scroll_dips >= 0 && scene->scroll_dips <= 1'000'000'000.0f &&
        ui_rect_valid(scene->viewport);
}
bool eligible(void* context, u32 index) noexcept {
    const auto& scene = *static_cast<const MemberListScene*>(context);
    const auto* resolved = member_list_view_row(*scene.source, index);
    if (!resolved) return false;
    const auto& row = *resolved;
    return row.kind == MemberListRowKind::member && row.user_id &&
        ((scene.source->thread_mode && row.member_slot == ~0u && scene.users &&
          user_store_find(*scene.users, row.user_id)) ||
         (row.member_slot < scene.members->capacity && scene.members->records[row.member_slot].occupied &&
          scene.members->records[row.member_slot].user_id == row.user_id));
}
u32 focused_index(const MemberListScene& scene) noexcept {
    const auto* row = member_list_scene_resolve(scene, scene.widgets.focused);
    if (row && row->source_index < scene.source->count &&
        row->user_id == scene.focused_user_id) return row->source_index;
    if (scene.focused_user_id) {
        const u32 resident = scene.source->authoritative ? scene.source->resident_count : scene.source->count;
        for (u32 i = 0; i < resident; ++i)
            if (scene.source->rows[i].user_id == scene.focused_user_id)
                return scene.source->authoritative ? scene.source->rows[i].logical_index : i;
    }
    if (scene.source->authoritative && scene.focused_source_index < scene.source_count)
        return scene.focused_source_index;
    return kUiListNoSelection;
}
bool reveal_index(MemberListScene* scene, u32 index) noexcept {
    if (index >= scene->source_count) return false;
    const bool known_member = eligible(scene, index);
    if (!known_member && !scene->source->authoritative) return false;
    const auto* row = member_list_view_row(*scene->source, index);
    const u64 user_id = known_member ? row->user_id : 0;
    scene->focused_source_index = index;
    const u64 extent = ui_extent_units(kMemberListRowHeight);
    const u64 offset = static_cast<u64>(static_cast<double>(scene->scroll_dips) * kUiExtentUnitsPerDip);
    u64 revealed = 0;
    if (!ui_scroll_reveal_units(index * extent, extent, scene->source->count * extent, offset,
        ui_extent_units(ui_rect_height(scene->viewport)), &revealed)) return false;
    const float scroll = static_cast<float>(static_cast<double>(revealed) / kUiExtentUnitsPerDip);
    return member_list_scene_update(scene, scene->source, scene->members, scene->users, scene->state,
        scene->viewport, scroll).ok() && member_list_scene_focus_user(scene, user_id);
}
} // namespace

bool member_list_scene_reveal_user(MemberListScene* scene, u64 user_id) noexcept {
    if (!source_valid(scene) || !user_id) return false;
    const u32 resident = scene->source->authoritative ? scene->source->resident_count : scene->source->count;
    for (u32 i = 0; i < resident; ++i) {
        const auto& row = scene->source->rows[i];
        const u32 index = scene->source->authoritative ? row.logical_index : i;
        if (row.user_id == user_id && eligible(scene, index)) return reveal_index(scene, index);
    }
    return false;
}

MemberListHit member_list_scene_navigate(MemberListScene* scene, UiListMove move) noexcept {
    if (!source_valid(scene)) return {};
    u32 current = focused_index(*scene);
    if (current == kUiListNoSelection && move == UiListMove::restore && scene->scroll_dips >= 0)
        current = static_cast<u32>(scene->scroll_dips / kMemberListRowHeight);
    if (scene->source->authoritative && scene->source_count) {
        if (current == kUiListNoSelection) current = minimum(scene->source_count - 1,
            static_cast<u32>(scene->scroll_dips / kMemberListRowHeight));
        if ((move == UiListMove::previous && !current) ||
            (move == UiListMove::next && current == scene->source_count - 1)) return {};
        u32 index = current;
        if (move == UiListMove::first) index = 0;
        else if (move == UiListMove::last) index = scene->source_count - 1;
        else if (move == UiListMove::previous && index) --index;
        else if (move == UiListMove::next && index < scene->source_count - 1) ++index;
        const bool reverse = move == UiListMove::previous || move == UiListMove::last;
        for (u32 checked = 0; checked < scene->source->resident_count; ++checked) {
            const auto* row = member_list_view_row(*scene->source, index);
            if (!row || row->kind == MemberListRowKind::member) break;
            if (reverse) { if (!index) break; --index; }
            else { if (index == scene->source_count - 1) break; ++index; }
        }
        if (!reveal_index(scene, index)) return {};
        return {scene->focused_user_id, scene->widgets.focused, index};
    }
    const u32 index = ui_list_move(scene->source->count, current, move, eligible, scene);
    if (index == kUiListNoSelection || !reveal_index(scene, index)) return {};
    return {scene->focused_user_id, scene->widgets.focused, index};
}

MemberListHit member_list_scene_page(MemberListScene* scene, bool reverse) noexcept {
    if (!source_valid(scene)) return {};
    if (scene->source->authoritative && scene->source_count) {
        const float delta = ui_rect_height(scene->viewport) * (reverse ? -1.0f : 1.0f);
        const float scroll = maximum(0.0f, scene->scroll_dips + delta);
        u32 current = focused_index(*scene);
        if (current == kUiListNoSelection) current = minimum(scene->source_count - 1,
            static_cast<u32>(scene->scroll_dips / kMemberListRowHeight));
        const u32 page = maximum(1u, static_cast<u32>(ui_rect_height(scene->viewport) / kMemberListRowHeight));
        const u32 index = reverse ? (current > page ? current - page : 0) :
            static_cast<u32>(minimum(static_cast<u64>(current) + page, static_cast<u64>(scene->source_count - 1)));
        scene->focused_source_index = index;
        if (!member_list_scene_update(scene, scene->source, scene->members, scene->users, scene->state,
            scene->viewport, scroll).ok()) return {};
        const auto* row = member_list_view_row(*scene->source, index);
        const u64 id = eligible(scene, index) && row ? row->user_id : 0;
        if (!member_list_scene_focus_user(scene, id)) return {};
        return {id, scene->widgets.focused, index};
    }
    const u64 extent = ui_extent_units(kMemberListRowHeight);
    const auto target = ui_list_page(scene->source->count, focused_index(*scene),
        static_cast<u64>(static_cast<double>(scene->scroll_dips) * kUiExtentUnitsPerDip),
        ui_extent_units(ui_rect_height(scene->viewport)), scene->source->count * extent, reverse,
        [](void*, u32 index) noexcept {
            const u64 stride = ui_extent_units(kMemberListRowHeight);
            return UiListRowBounds{index * stride, (static_cast<u64>(index) + 1) * stride};
        }, eligible, scene);
    if (!target.ok()) return {};
    const u64 id = scene->source->rows[target.index].user_id;
    const float scroll = static_cast<float>(static_cast<double>(target.scroll_units) / kUiExtentUnitsPerDip);
    if (!member_list_scene_update(scene, scene->source, scene->members, scene->users, scene->state,
        scene->viewport, scroll).ok() || !member_list_scene_focus_user(scene, id)) return {};
    return {id, scene->widgets.focused, target.index};
}
} // namespace maicord
