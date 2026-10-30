#include "maicord/ui/profile_appearance.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/ui/member_list_scene.hpp"
#include "maicord/ui/profile_media.hpp"
#include "maicord/model/thread_member_store.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/decimal.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/model/guild_member_store.hpp"
#include "maicord/ui/plugin_slots.hpp"
#include "maicord/ui/scene.hpp"
#include "maicord/ui/activity_status.hpp"

namespace maicord {
namespace {

inline constexpr u32 kMemberListRootId = 1;

UiLayoutStyle row_style() noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fill();
    style.height = ui_layout_fixed(kMemberListRowHeight);
    style.horizontal_alignment = UiLayoutAlignment::stretch;
    return style;
}

UiLayoutStyle avatar_style() noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fixed(32.0f);
    style.height = ui_layout_fixed(32.0f);
    style.margin = UiInsets{8.0f, 6.0f, 0.0f, 0.0f};
    return style;
}

UiLayoutStyle label_style() noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fill();
    style.height = ui_layout_fill();
    style.margin = UiInsets{50.0f, 8.0f, 18.0f, 8.0f};
    style.flags = static_cast<u16>(ui_layout_node_visible | ui_layout_node_text_content);
    return style;
}

UiLayoutStyle group_label_style() noexcept {
    UiLayoutStyle style = label_style();
    style.margin = UiInsets{8.0f, 9.0f, 8.0f, 9.0f};
    return style;
}

UiLayoutStyle status_style() noexcept {
    UiLayoutStyle style = ui_layout_style();
    style.width = ui_layout_fixed(10.0f);
    style.height = ui_layout_fixed(10.0f);
    style.margin = UiInsets{31.0f, 28.0f, 0.0f, 0.0f};
    return style;
}

MemberListSceneResult failure(MemberListScene* scene, MemberListSceneError error) noexcept {
    saturating_increment_if(scene, scene->rejected);
    return MemberListSceneResult{error, scene ? scene->first : 0, scene ? scene->count : 0,
                                 scene ? scene->scroll_dips : 0.0f, false};
}

float clamp_scroll(const MemberListView& source, const UiRect& viewport,
                   float requested) noexcept {
    const float content = static_cast<float>(member_list_view_count(source)) * kMemberListRowHeight;
    const float maximum_scroll = maximum(0.0f, content - ui_rect_height(viewport));
    return minimum(maximum(requested, 0.0f), maximum_scroll);
}

UiVirtualRange member_virtual_range(u32 count, float scroll, float viewport_height) noexcept {
    if (count == 0 || viewport_height <= 0.0f) {
        return UiVirtualRange{};
    }
    const u32 first_visible = minimum(count - 1, static_cast<u32>(scroll / kMemberListRowHeight));
    const u32 first =
        first_visible > kMemberListSceneOverscanRows ? first_visible - kMemberListSceneOverscanRows : 0;
    const float visible_rows_f = viewport_height / kMemberListRowHeight;
    const u32 visible_rows = static_cast<u32>(visible_rows_f) + 2u;
    const u32 requested = visible_rows + kMemberListSceneOverscanRows * 2u;
    return UiVirtualRange{first, minimum(requested, count - first),
                          static_cast<float>(first) * kMemberListRowHeight - scroll};
}

void clear_materialization(MemberListScene* scene) noexcept {
    scene->source = nullptr;
    scene->members = nullptr;
    scene->users = nullptr;
    scene->state = nullptr;
    scene->source_revision = 0;
    scene->source_count = 0;
    scene->first = 0;
    scene->count = 0;
    scene->active = false;
    ui_widget_manager_reset(&scene->widgets);
    member_media_cache_reset(scene->media_cache);
}

bool source_current(const MemberListView* source,
                    const GuildMemberStore* members, const UserStore* users,
                    const ChatStateStore* state) noexcept {
    if (!source) return true;
    if (source->source_user_revision != users->revision ||
        source->source_permission_mutations != state->permission_mutations ||
        (source->guild_id && source->source_member_revision != members->revision)) return false;
    return !source->thread_mode || (source->source_threads &&
        source->source_thread_revision == source->source_threads->revision &&
        source->source_thread_generation == source->source_threads->generation &&
        thread_member_store_owns(*source->source_threads, source->guild_id, source->list_id));
}

} // namespace

bool member_list_scene_initialize(MemberListScene* scene, Slice<MemberListRowWidget> rows,
                                  Slice<UiWidget> widgets, Slice<UiLayoutNode> layout_nodes) noexcept {
    if (!scene || !rows.data || rows.count == 0 || !widgets.data || widgets.count == 0 ||
        !layout_nodes.data || layout_nodes.count < widgets.count) {
        return false;
    }
    reset_value<MemberListScene>(scene);
    scene->focused_source_index = kUiListNoSelection;
    scene->rows = rows;
    return ui_widget_manager_initialize(&scene->widgets, widgets, layout_nodes);
}

MemberListSceneResult member_list_scene_update(MemberListScene* scene,
                                               const MemberListView* source,
                                               const GuildMemberStore* members,
                                               const UserStore* users,
                                               const ChatStateStore* state, UiRect viewport,
                                               float requested_scroll_dips) noexcept {
    if (!scene || !scene->rows.data || !ui_rect_valid_or_empty(viewport) ||
        requested_scroll_dips != requested_scroll_dips || requested_scroll_dips < -1'000'000'000.0f ||
        requested_scroll_dips > 1'000'000'000.0f ||
        (source && (!source->rows.data || (!source->authoritative && source->count > source->rows.count) || (source->authoritative && source->resident_count > source->rows.count))) ||
        (source && source->guild_id && members && (!members->records || !members->order)) ||
        (users && (!users->records || !users->order || !users->names)) ||
        (source && (!members || !users || !state))) {
        return failure(scene, MemberListSceneError::invalid_input);
    }
    if (!source || !ui_rect_has_area(viewport)) {
        const bool changed =
            scene->active || scene->source != source || scene->members != members ||
            scene->users != users || scene->state != state ||
            scene->source_revision != (source ? source->revision : 0) ||
            scene->source_count != (source ? member_list_view_count(*source) : 0) ||
            !ui_rect_equal(scene->viewport, viewport) || !source_current(source, members, users, state);
        if (!changed)
            return MemberListSceneResult{MemberListSceneError::none, 0, 0, 0.f, false};
        clear_materialization(scene);
        // Remember the hidden publication without retaining materialized rows.
        // A repeated synchronization need not reset handles or renew interest.
        scene->source = source;
        scene->members = members;
        scene->users = users;
        scene->state = state;
        scene->source_revision = source ? source->revision : 0;
        scene->source_count = source ? member_list_view_count(*source) : 0;
        scene->scroll_dips = 0;
        scene->viewport = viewport;
        if (scene->viewport_changed) scene->viewport_changed(scene->viewport_context);
        return MemberListSceneResult{MemberListSceneError::none, 0, 0, 0.0f, changed};
    }
    const float scroll = clamp_scroll(*source, viewport, requested_scroll_dips);
    const UiVirtualRange range = member_virtual_range(member_list_view_count(*source), scroll, ui_rect_height(viewport));
    const u32 required_widgets = 1 + range.count * kMemberListWidgetsPerRow;
    if (range.count > scene->rows.count &&
        (!scene->row_storage || !ui_virtual_row_storage_grow(scene->row_storage, range.count))) {
        return failure(scene, MemberListSceneError::capacity);
    }
    if (scene->row_storage) {
        scene->rows = ui_virtual_row_storage_rows<MemberListRowWidget>(*scene->row_storage);
    }
    if (range.count > scene->rows.count || !ui_widget_manager_reserve(&scene->widgets, required_widgets)) {
        return failure(scene, MemberListSceneError::capacity);
    }
    const bool retained = scene->active && scene->source == source &&
                          scene->members == members && scene->users == users && scene->state == state &&
                          scene->source_revision == source->revision && scene->first == range.first &&
                          scene->count == range.count && scene->source_count == member_list_view_count(*source) &&
                          source_current(source, members, users, state) &&
                          (!source->authoritative || !source->source_members ||
                           source->source_member_revision == source->source_members->revision);
    const float local_scroll =
        maximum(0.0f, scroll - static_cast<float>(range.first) * kMemberListRowHeight);
    if (retained) {
        if (ui_rect_equal(scene->viewport, viewport) && scene->scroll_dips == scroll)
            return MemberListSceneResult{MemberListSceneError::none, range.first, range.count, scroll, false};
        const UiWidget* root = ui_widget_manager_resolve(scene->widgets, scene->root);
        if (!root ||
            !ui_layout_engine_set_scroll(&scene->widgets.layout, root->layout,
                                         UiPoint{0.0f, local_scroll}) ||
            !ui_widget_manager_layout_rectangle(&scene->widgets, viewport).ok()) {
            return failure(scene, MemberListSceneError::layout);
        }
        scene->viewport = viewport;
        scene->scroll_dips = scroll;
        ++scene->layout_passes;
        if (scene->viewport_changed) scene->viewport_changed(scene->viewport_context);
        return MemberListSceneResult{MemberListSceneError::none, range.first, range.count, scroll, true};
    }

    u64 restore_focus = scene->focused_user_id;
    if (source->authoritative && scene->focused_source_index < member_list_view_count(*source)) {
        const auto* focused = member_list_view_row(*source, scene->focused_source_index);
        if (focused && focused->kind == MemberListRowKind::member) restore_focus = focused->user_id;
    }
    ui_widget_manager_reset(&scene->widgets);
    const UiWidgetResult root = ui_widget_manager_add_keyed(
        &scene->widgets, ui_widget_invalid_handle(), kMemberListRootId, UiWidgetKind::scroll_view, ui_widget_enabled,
        "MemberList", ui_layout_scroll_root_style());
    if (!root.ok()) {
        clear_materialization(scene);
        return failure(scene, MemberListSceneError::widget);
    }
    scene->root = root.handle;
    for (u32 local = 0; local < range.count; ++local) {
        const u32 source_index = range.first + local;
        const auto* resolved = member_list_view_row(*source, source_index);
        const bool resident = resolved &&
            (resolved->kind != MemberListRowKind::member || !source->guild_id ||
             (source->thread_mode && resolved->member_slot == ~0u &&
              user_store_find(*users, resolved->user_id)) ||
             (resolved->member_slot < members->capacity && members->records[resolved->member_slot].occupied &&
              members->records[resolved->member_slot].user_id == resolved->user_id));
        const MemberListRow source_row = resident ? *resolved : MemberListRow{};
        const bool member_row = resident && source_row.kind == MemberListRowKind::member;
        const u32 slot = source_row.member_slot;
        const u32 id = 2u + source_index * kMemberListWidgetsPerRow;
        const UiWidgetResult row = ui_widget_manager_add_keyed(
            &scene->widgets, root.handle, id, UiWidgetKind::button,
            member_row ? static_cast<u16>(ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable)
                       : static_cast<u16>(ui_widget_enabled),
            "MemberRow", row_style());
        const UiWidgetResult avatar =
            row.ok() ? ui_widget_manager_add_keyed(&scene->widgets, row.handle, id + 1, UiWidgetKind::image,
                                                   ui_widget_enabled, "MemberAvatar", avatar_style())
                     : UiWidgetResult{};
        const UiWidgetResult label =
            avatar.ok() ? ui_widget_manager_add_keyed(
                              &scene->widgets, row.handle, id + 2, UiWidgetKind::text, ui_widget_enabled,
                              "MemberName", (member_row || !resident) ? label_style() : group_label_style())
                        : UiWidgetResult{};
        const UiWidgetResult status =
            label.ok() ? ui_widget_manager_add_keyed(&scene->widgets, row.handle, id + 3, UiWidgetKind::custom,
                                                     ui_widget_enabled, "MemberStatus", status_style())
                       : UiWidgetResult{};
        if (!row.ok() || !avatar.ok() || !label.ok() || !status.ok()) {
            clear_materialization(scene);
            return failure(scene, MemberListSceneError::widget);
        }
        scene->rows[local] =
            MemberListRowWidget{row.handle,     avatar.handle,      label.handle,
                                status.handle,  source_row.user_id, source_row.group_role_id,
                                slot,           source_index,       source_row.role_color,
                                source_row.kind};
        scene->rows[local].source_row = source_row;
        scene->rows[local].resident = resident;
        if (resident && !member_row) {
            auto& text = scene->rows[local].group_label;
            const auto group_name = member_list_row_label(source_row, *members, *users, *state);
            text.bytes = static_cast<u16>(utf8_truncated_length(group_name, 128));
            // Group heading format: "ONLINE — 75,000" (ASCII letters uppercased,
            // em dash, grouped digits).
            for (u32 i = 0; i < text.bytes; ++i) {
                const char c = group_name.data[i];
                text.data[i] = c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c;
            }
            const char separator[] = " \xE2\x80\x94 ";
            copy_bytes(text.data + text.bytes, separator, sizeof(separator) - 1);
            text.bytes = static_cast<u16>(text.bytes + sizeof(separator) - 1);
            u32 count = source->authoritative ? source_row.group_count : 0;
            if (!source->authoritative)
                for (u32 i = source_index + 1;
                     i < source->count && source->rows[i].kind == MemberListRowKind::member; ++i)
                    ++count;
            text.bytes = static_cast<u16>(text.bytes + decimal_write_grouped(text.data + text.bytes, count));
            if (source->thread_mode && source->incomplete) {
                static constexpr char partial[] = " (partial)";
                copy_bytes(text.data + text.bytes, partial, sizeof(partial) - 1);
                text.bytes = static_cast<u16>(text.bytes + sizeof(partial) - 1);
            }
        }
    }
    const UiWidget* root_widget = ui_widget_manager_resolve(scene->widgets, root.handle);
    if (!root_widget ||
        !ui_layout_engine_set_scroll(&scene->widgets.layout, root_widget->layout,
                                     UiPoint{0.0f, local_scroll}) ||
        !ui_widget_manager_layout_rectangle(&scene->widgets, viewport).ok()) {
        clear_materialization(scene);
        return failure(scene, MemberListSceneError::layout);
    }
    scene->source = source;
    scene->members = members;
    scene->users = users;
    scene->state = state;
    scene->viewport = viewport;
    scene->focused_user_id = restore_focus;
    scene->content_height = static_cast<float>(member_list_view_count(*source)) * kMemberListRowHeight;
    scene->scroll_dips = scroll;
    scene->source_revision = source->revision;
    scene->source_count = member_list_view_count(*source);
    scene->first = range.first;
    scene->count = range.count;
    scene->active = true;
    ++scene->rebuilds;
    ++scene->layout_passes;
    if (restore_focus != 0) {
        member_list_scene_focus_user(scene, restore_focus);
    }
    if (scene->viewport_changed) scene->viewport_changed(scene->viewport_context);
    return MemberListSceneResult{MemberListSceneError::none, range.first, range.count, scroll, true};
}

MemberListSceneResult member_list_scene_scroll(MemberListScene* scene, float delta_dips) noexcept {
    if (!scene || !ui_coordinate_valid(delta_dips)) {
        return failure(scene, MemberListSceneError::invalid_input);
    }
    return member_list_scene_update(scene, scene->source, scene->members, scene->users, scene->state,
                                    scene->viewport, scene->scroll_dips + delta_dips);
}

const MemberListRowWidget* member_list_scene_resolve(const MemberListScene& scene,
                                                     UiWidgetHandle widget) noexcept {
    if (!ui_widget_manager_resolve(scene.widgets, widget)) {
        return nullptr;
    }
    for (u32 index = 0; index < scene.count; ++index) {
        const MemberListRowWidget& row = scene.rows[index];
        const UiWidgetHandle handles[] = {row.widget, row.avatar, row.label, row.status};
        for (u32 handle = 0; handle < 4; ++handle) {
            if (handles[handle].slot == widget.slot && handles[handle].generation == widget.generation) {
                return &row;
            }
        }
    }
    return nullptr;
}

const UiLayoutNode* member_list_scene_layout_node(const MemberListScene& scene,
                                                  UiWidgetHandle widget) noexcept {
    return ui_widget_manager_layout_node(scene.widgets, widget);
}

MemberListHit member_list_scene_hit_test(const MemberListScene& scene, float x, float y) noexcept {
    const UiWidgetHandle hit = ui_widget_manager_hit_test(scene.widgets, x, y);
    const MemberListRowWidget* row = member_list_scene_resolve(scene, hit);
    return row && member_list_scene_row_resident(scene, static_cast<u32>(row - scene.rows.data)) &&
                   row->kind == MemberListRowKind::member
               ? MemberListHit{row->user_id, row->widget, row->source_index}
               : MemberListHit{};
}

bool member_list_scene_row_resident(const MemberListScene& scene, u32 index) noexcept {
    if (!scene.source || index >= scene.count || !scene.rows[index].resident) return false;
    const auto& row = scene.rows[index];
    const auto* resolved = member_list_view_row(*scene.source, row.source_index);
    return resolved && resolved->kind == row.kind && resolved->user_id == row.user_id;
}

bool member_list_scene_focus_user(MemberListScene* scene, u64 user_id) noexcept {
    if (!scene) {
        return false;
    }
    if (user_id == 0) {
        scene->focused_user_id = 0;
        return ui_widget_manager_focus(&scene->widgets, ui_widget_invalid_handle());
    }
    for (u32 index = 0; index < scene->count; ++index) {
        if (member_list_scene_row_resident(*scene, index) && scene->rows[index].kind == MemberListRowKind::member &&
            scene->rows[index].user_id == user_id &&
            ui_widget_manager_focus(&scene->widgets, scene->rows[index].widget)) {
            scene->focused_user_id = user_id;
            scene->focused_source_index = scene->rows[index].source_index;
            return true;
        }
    }
    scene->focused_user_id = user_id;
    return true;
}

bool member_list_row_status(const MemberListScene& s, u32 index, Slice<char> storage,
                            UiActivityStatus* output) noexcept {
    if (!output)
        return false;
    reset_value(output);
    if (index >= s.count || !s.users || !s.source)
        return false;
    const auto& row = s.rows[index];
    if (row.kind != MemberListRowKind::member || row.source_row.status == GuildMemberStatus::offline)
        return false;
    const auto* global = user_appearance_find(s.users->appearance, row.user_id);
    const auto* guild = user_appearance_find(s.users->appearance, row.user_id, s.source->guild_id);
    return ui_user_activity_status(*s.users, row.user_id,
                                   guild && (guild->fields & appearance_activity) ? guild : global, storage, output);
}
UiRect member_list_row_name_line(UiRect label) noexcept {
    label.top -= 4;
    label.bottom = label.top + 20;
    return label;
}
UiRect member_list_row_status_line(UiRect label) noexcept {
    return {label.left, label.top + 16, label.right, label.bottom + 4};
}
UiBadgeWidget member_list_badge_resolve(const MemberListScene& s, u32 index) noexcept {
    if (index >= s.count || !s.users || !s.source)
        return {};
    const auto& row = s.rows[index];
    if (!member_list_scene_row_resident(s, index) || row.kind != MemberListRowKind::member)
        return {};
    const auto* a = user_appearance_find(s.users->appearance, row.user_id);
    if (!a || (!a->bot && (!a->primary_enabled || !a->primary_tag.bytes)))
        return {};
    const auto* label = member_list_scene_layout_node(s, row.label);
    if (!label)
        return {};
    auto line = label->content_rectangle;
    const auto* guild = user_appearance_find(s.users->appearance, row.user_id, s.source->guild_id);
    char status_storage[kAppearanceActivityBytes];
    UiActivityStatus status{};
    if (member_list_row_status(s, index, {status_storage, sizeof(status_storage)}, &status))
        line = member_list_row_name_line(line);
    UiBadgeWidget b{};
    b.owner = row.label;
    if (a->bot)
        ui_app_badge(&b, a->public_flags);
    else {
        b.leading = true;
        b.style = text_style_subtext | text_style_bold;
        b.label = a->primary_tag.view();
    }
    const bool hovered =
        s.widgets.hovered.slot == row.widget.slot && s.widgets.hovered.generation == row.widget.generation;
    b.opacity = row.source_row.status == GuildMemberStatus::offline && !hovered
                    ? .4f
                    : 1.f;
    const auto natural = ui_badge_widget_layout(s.measure, b, line);
    const float width = ui_rect_width(natural.outer);
    const auto name =
        member_list_row_label(row.source_row, *s.members, *s.users, *s.state);
    const auto* name_style = guild && guild->name.font_id ? guild : a;
    UiTextMeasureValue measured{};
    const float maximum_name = maximum(1.f, ui_rect_width(line) - width - 6);
    float name_width = minimum(maximum_name, name.bytes * 8.f);
    if (s.measure && ui_text_measure_cache_measure(
                         s.measure,
                         {{0x4e414d45, hash64_bytes(kHash64Seed, name.data, name.bytes), 0, 0, 0},
                          name,
                          {},
                          {maximum_name, 40},
                          true,
                          profile_name_face(name_style->name.font_id)},
                         &measured)
                         .ok())
        name_width = minimum(maximum_name, measured.size.width);
    // 6 after the name: the server tag ends there plus its width, the APP tag
    // starts there.
    if (b.compact)
        line.left += name_width + 6;
    else
        line.right = minimum(line.right, line.left + name_width + 6 + width);
    b.rectangle = ui_badge_widget_layout(s.measure, b, line).outer;
    return b;
}
UiPluginSlotBadge member_list_plugin_badge(const MemberListScene& s, u32 index) noexcept {
    UiPluginSlotBadge plugin{};
    if (!g_ui_plugin_slot_query || index >= s.count || !s.users || !s.source) return plugin;
    const auto& row = s.rows[index];
    if (!member_list_scene_row_resident(s, index) || row.kind != MemberListRowKind::member) return plugin;
    const auto* label = member_list_scene_layout_node(s, row.label);
    if (!label) return plugin;
    auto line = label->content_rectangle;
    line.top = (line.top + line.bottom) * .5f - 10;
    line.bottom = line.top + 20;
    if (!ui_plugin_slot_line_badge(kUiPluginSlotMemberBadge, 0, 0, row.user_id, row.label, line, s.measure, &plugin))
        return plugin;
    const auto tag = member_list_badge(s, index);
    if (tag.label.bytes && plugin.badge.rectangle.left < tag.rectangle.right + 4) reset_value(&plugin);
    return plugin;
}
UiPluginSlotBadge member_list_plugin_hit(const MemberListScene& s, float x, float y, u64* user_id) noexcept {
    const auto* row = member_list_scene_resolve(s, ui_widget_manager_hit_test(s.widgets, x, y));
    if (!row) return {};
    const auto plugin = member_list_plugin_badge(s, static_cast<u32>(row - s.rows.data));
    if (!plugin.badge.label.bytes || !ui_rect_contains(plugin.badge.rectangle, x, y)) return {};
    if (user_id) *user_id = row->user_id;
    return plugin;
}
UiRect member_list_app_check_hit(const MemberListScene& s, float x, float y, u64* user_id,
                                 UiWidgetHandle* row_widget) noexcept {
    const auto* row = member_list_scene_resolve(s, ui_widget_manager_hit_test(s.widgets, x, y));
    if (!row) return {};
    const auto badge = member_list_badge(s, static_cast<u32>(row - s.rows.data));
    const auto check = ui_app_badge_check(badge);
    if (!ui_rect_contains(check, x, y)) return {};
    if (user_id) *user_id = row->user_id;
    if (row_widget) *row_widget = row->widget;
    return check;
}

namespace {
constexpr u8 kMemberMediaHovered = 1, kMemberMediaOffline = 2;
constexpr u32 kMemberMediaCacheLoad = kMemberMediaCacheCapacity * 3 / 4;
static_assert((kMemberMediaCacheCapacity & (kMemberMediaCacheCapacity - 1)) == 0);

UiRect member_media_offset(UiRect r, float x, float y) noexcept {
    return {r.left + x, r.top + y, r.right + x, r.bottom + y};
}

// Every input of member_media_resolve and member_list_badge_resolve that is
// not per row: a change re-resolves every cached row.
u64 member_media_stamp(const MemberListScene& s) noexcept {
    u64 h = hash64_mix(kHash64Seed, reinterpret_cast<usize>(s.users));
    h = hash64_mix(h, reinterpret_cast<usize>(s.members));
    h = hash64_mix(h, reinterpret_cast<usize>(s.state));
    h = hash64_mix(h, reinterpret_cast<usize>(s.measure));
    h = hash64_mix(h, s.source ? s.source->guild_id : 0);
    h = hash64_mix(h, (static_cast<u64>(s.users->revision) << 32) | s.users->appearance.revision);
    h = hash64_mix(h, (static_cast<u64>(s.users->compactions) << 32) | s.users->evictions);
    // An activity status line moves the badge onto the name line.
    h = hash64_mix(h, s.users->activities.revision);
    h = hash64_mix(h, s.members ? s.members->revision : 0);
    h = hash64_mix(h, s.measure ? s.measure->generation : 0);
    return h | 1;
}

// The row's layout node, or null when it has no member media.
const UiLayoutNode* member_media_row_node(const MemberListScene& s, u32 index) noexcept {
    if (!s.active || !s.users || !s.source || index >= s.count)
        return nullptr;
    const auto& row = s.rows[index];
    if (row.kind != MemberListRowKind::member || !row.user_id || !member_list_scene_row_resident(s, index))
        return nullptr;
    return member_list_scene_layout_node(s, row.widget);
}

const MemberMediaCacheEntry* member_media_entry(const MemberListScene& s, u32 index,
                                                const UiLayoutNode** row_node) noexcept {
    *row_node = nullptr;
    MemberMediaCache* cache = s.media_cache;
    if (!cache || !s.active || !s.users || !s.source || index >= s.count)
        return nullptr;
    const auto& row = s.rows[index];
    const u64 stamp = member_media_stamp(s);
    if (cache->stamp != stamp) {
        member_media_cache_reset(cache);
        cache->stamp = stamp;
    }
    // Consumers look up a row's layers back to back: its residency, node and
    // entry hold while nothing the lookup reads has moved.
    u64 key = hash64_mix(stamp, index);
    key = hash64_mix(key, (static_cast<u64>(row.widget.slot) << 32) | row.widget.generation);
    key = hash64_mix(key, (static_cast<u64>(s.widgets.hovered.slot) << 32) | s.widgets.hovered.generation);
    key = hash64_mix(key, (static_cast<u64>(s.layout_passes) << 32) | s.rebuilds);
    key = hash64_mix(key, (static_cast<u64>(s.source->revision) << 32) | s.first) | 1;
    const u32 memory = index & (kMemberMediaRowMemory - 1);
    if (cache->row_keys[memory] == key) {
        *row_node = cache->row_nodes[memory];
        return cache->row_slots[memory] == ~0u ? nullptr : &cache->entries[cache->row_slots[memory]];
    }
    const UiLayoutNode* node = member_media_row_node(s, index);
    cache->row_keys[memory] = key;
    cache->row_nodes[memory] = node;
    cache->row_slots[memory] = ~0u;
    *row_node = node;
    if (!node)
        return nullptr;
    // member_media_resolve animates a hovered row's nameplate.
    const u32 hovered = s.widgets.hovered.generation ? s.widgets.hovered.slot : ~0u;
    const bool row_hovered = (row.widget.slot == hovered || row.avatar.slot == hovered ||
                              row.label.slot == hovered || row.status.slot == hovered) &&
                             ui_widget_manager_resolve(s.widgets, s.widgets.hovered);
    const u8 state = static_cast<u8>((row_hovered ? kMemberMediaHovered : 0) |
                                     (row.source_row.status == GuildMemberStatus::offline ? kMemberMediaOffline : 0));
    const float width = ui_rect_width(node->rectangle);
    constexpr u32 mask = kMemberMediaCacheCapacity - 1;
    u32 slot = static_cast<u32>(hash64_mix(kHash64Seed, row.user_id)) & mask;
    for (;; slot = (slot + 1) & mask) {
        const MemberMediaCacheEntry& entry = cache->entries[slot];
        if (!entry.user_id)
            break;
        if (entry.user_id == row.user_id) {
            if (entry.state == state && entry.width == width) {
                saturating_increment(cache->hits);
                cache->row_slots[memory] = slot;
                return &entry;
            }
            break;
        }
    }
    if (!cache->entries[slot].user_id) {
        if (cache->count >= kMemberMediaCacheLoad) {
            member_media_cache_reset(cache);
            cache->stamp = stamp;
            slot = static_cast<u32>(hash64_mix(kHash64Seed, row.user_id)) & mask;
        }
        ++cache->count;
    }
    MemberMediaCacheEntry& entry = cache->entries[slot];
    reset_value<MemberMediaCacheEntry>(&entry);
    entry.user_id = row.user_id;
    entry.width = width;
    entry.state = state;
    const float x = -node->rectangle.left, y = -node->rectangle.top;
    const UiBadgeWidget badge = member_list_badge_resolve(s, index);
    // A label too long for the copy stays uncached (0xFF) and keeps resolving.
    entry.badge_bytes = badge.owner.generation ? 0xFF : 0;
    if (badge.owner.generation && badge.label.bytes <= sizeof(entry.badge_text)) {
        entry.badge = true;
        entry.badge_leading = badge.leading;
        entry.badge_accent = badge.accent;
        entry.badge_bytes = static_cast<u8>(badge.label.bytes);
        copy_bytes(entry.badge_text, badge.label.data, badge.label.bytes);
        entry.badge_rectangle = member_media_offset(badge.rectangle, x, y);
    }
    for (u32 layer = 0; layer < kMemberMediaLayerCount; ++layer) {
        ProfileMedia media{};
        if (!member_media_resolve(s, index, static_cast<MemberMedia>(layer), &media, false))
            continue;
        entry.layer_mask = static_cast<u8>(entry.layer_mask | (1u << layer));
        entry.circular_mask = static_cast<u8>(entry.circular_mask | (media.circular ? 1u << layer : 0u));
        entry.keys[layer] = media.key;
        entry.rectangles[layer] = member_media_offset(media.rectangle, x, y);
    }
    constexpr u32 avatar = static_cast<u32>(MemberMedia::avatar);
    MemberAvatarMedia plain{};
    if (!(entry.layer_mask & (1u << avatar)) && member_plain_avatar_resolve(s, index, &plain, false)) {
        entry.plain_avatar = true;
        entry.layer_mask = static_cast<u8>(entry.layer_mask | (1u << avatar));
        entry.circular_mask = static_cast<u8>(entry.circular_mask | (1u << avatar));
        entry.keys[avatar] = plain.key;
        entry.rectangles[avatar] = member_media_offset(plain.rectangle, x, y);
    }
    saturating_increment(cache->resolves);
    // A load reset above cleared the remembered rows.
    cache->row_keys[memory] = key;
    cache->row_nodes[memory] = node;
    cache->row_slots[memory] = slot;
    return &entry;
}
} // namespace

void member_media_cache_reset(MemberMediaCache* cache) noexcept {
    if (!cache)
        return;
    const u32 resolves = cache->resolves, hits = cache->hits;
    clear_bytes(cache, sizeof(*cache));
    cache->resolves = resolves;
    cache->hits = hits;
}

bool member_media_place(const MemberListScene& s, u32 index, MemberMedia layer,
                        MemberMediaPlacement* out) noexcept {
    if (!out)
        return false;
    if (!s.media_cache) {
        ProfileMedia media{};
        if (!member_media_resolve(s, index, layer, &media))
            return false;
        *out = {media.key, media.rectangle, media.clip, media.circular};
        return true;
    }
    const UiLayoutNode* node = nullptr;
    const auto* entry = member_media_entry(s, index, &node);
    const u32 bit = 1u << static_cast<u32>(layer);
    if (!entry || !(entry->layer_mask & bit) || (layer == MemberMedia::avatar && entry->plain_avatar) ||
        !ui_rect_intersects(node->rectangle, s.viewport))
        return false;
    *out = {entry->keys[static_cast<u32>(layer)],
            member_media_offset(entry->rectangles[static_cast<u32>(layer)], node->rectangle.left,
                                node->rectangle.top),
            s.viewport, (entry->circular_mask & bit) != 0};
    return true;
}

bool member_plain_avatar_place(const MemberListScene& s, u32 index, MemberMediaPlacement* out) noexcept {
    if (!out)
        return false;
    if (!s.media_cache) {
        MemberAvatarMedia media{};
        if (!member_plain_avatar_resolve(s, index, &media))
            return false;
        *out = {media.key, media.rectangle, media.clip, true};
        return true;
    }
    const UiLayoutNode* node = nullptr;
    const auto* entry = member_media_entry(s, index, &node);
    if (!entry || !entry->plain_avatar)
        return false;
    constexpr u32 avatar = static_cast<u32>(MemberMedia::avatar);
    const UiRect rectangle =
        member_media_offset(entry->rectangles[avatar], node->rectangle.left, node->rectangle.top);
    if (!ui_rect_intersects(rectangle, s.viewport))
        return false;
    *out = {entry->keys[avatar], rectangle, s.viewport, true};
    return true;
}

UiBadgeWidget member_list_badge(const MemberListScene& s, u32 index) noexcept {
    if (!s.media_cache)
        return member_list_badge_resolve(s, index);
    const UiLayoutNode* node = nullptr;
    const auto* entry = member_media_entry(s, index, &node);
    if (entry && !entry->badge && entry->badge_bytes == 0xFF)
        return member_list_badge_resolve(s, index);
    if (!entry || !entry->badge)
        return {};
    const auto& row = s.rows[index];
    UiBadgeWidget b{};
    b.owner = row.label;
    b.leading = entry->badge_leading;
    // Only the APP tag is an accent badge, and it is compact.
    b.accent = b.compact = entry->badge_accent;
    b.style = text_style_subtext | text_style_bold;
    b.label = {entry->badge_text, entry->badge_bytes};
    const bool hovered =
        s.widgets.hovered.slot == row.widget.slot && s.widgets.hovered.generation == row.widget.generation;
    b.opacity = row.source_row.status == GuildMemberStatus::offline && !hovered ? .4f : 1.f;
    b.rectangle = member_media_offset(entry->badge_rectangle, node->rectangle.left, node->rectangle.top);
    return b;
}
} // namespace maicord
