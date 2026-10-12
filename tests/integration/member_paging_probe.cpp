#include "probe_present.hpp"
#include "maicord/core/log.hpp"
#include "maicord/ui/scrollbar.hpp"
#include "maicord/ui/shell_text_presentation.hpp"
#include "maicord/app/member_list_channel_binding.hpp"

namespace maicord {
namespace {
bool paging_check(bool valid, const char* failure) noexcept {
    if (!valid) log_write(LogLevel::error, "member_paging", failure);
    return valid;
}
const MemberListRowWidget* wire_row(const MemberListScene& scene, u32 wire) noexcept {
    for (u32 i = 0; i < scene.count; ++i)
        if (scene.rows[i].source_index == wire) return &scene.rows[i];
    return nullptr;
}
bool thumb_painted(const D3D11Renderer& renderer, const UiScrollbarGeometry& geometry) noexcept {
    const auto& stream = renderer.paint_stream;
    if (!geometry.visible || !stream.quads || stream.count > stream.quad_capacity ||
        stream.count > kMaxPaintQuads) return false;
    for (u32 i = 0; i < stream.count; ++i) {
        const auto& quad = stream.quads[i];
        if (ui_rect_equal({quad.left, quad.top, quad.right, quad.bottom}, geometry.thumb) &&
            quad.radius == 4.f && quad.fill.red == kDefaultTheme.interactive.red &&
            quad.fill.green == kDefaultTheme.interactive.green &&
            quad.fill.blue == kDefaultTheme.interactive.blue &&
            quad.fill.alpha == kDefaultTheme.interactive.alpha) return true;
    }
    return false;
}
bool text_contract(const D3D11Renderer& renderer, const MemberListScene& scene,
                   bool arrived) noexcept {
    UiTextCommand commands[kMaximumShellChromeTextCommands]{};
    char bytes[8192]{};
    UiTextCommandStream stream{};
    if (!paging_check(ui_text_command_stream_initialize(&stream, {commands, kMaximumShellChromeTextCommands},
                                           {bytes, 8192}), "text_stream_initialize_failed") ||
        !paging_check(shell_text_presentation_build(*renderer.ui_scene, *renderer.chat_state,
            *renderer.navigation_view, kDefaultTheme, *renderer.shell_navigation_scene,
            renderer.message_composer_scene, &scene, &stream), "shell_text_build_failed")) return false;
    bool group = false, known = false, arrival = false;
    const auto* gap = wire_row(scene, 50002);
    if (!paging_check(gap != nullptr, "text_wire_50002_missing")) return false;
    for (u32 i = 0; i < stream.count; ++i) {
        const auto& command = commands[i];
        group |= utf8_equal(command.text, utf8_literal("ONLINE \xE2\x80\x94 75,000"));
        known |= utf8_equal(command.text, utf8_literal("Resident at wire 50001"));
        arrival |= utf8_equal(command.text, utf8_literal("Arrived at wire 50002"));
        if (!arrived && command.owner.handle.slot == gap->label.slot &&
            command.owner.manager == &scene.widgets)
            return paging_check(false, "placeholder_emitted_text");
    }
    return paging_check(group, "authoritative_group_text_missing") &&
        paging_check(known, "resident_member_text_missing") &&
        paging_check(arrival == arrived, "arrival_text_state_incorrect");
}
} // namespace

// Caller supplies a guild-mode shell with a visible member_sidebar. All
// fixture backing is bounded independently of the 100000 logical wire rows.
bool member_paging_probe(D3D11Renderer& renderer, UiTextMeasureCache& measure,
                         bool (*capture)(const wchar_t*) noexcept) noexcept {
    if (!capture || !renderer.ui_scene || !renderer.chat_state ||
        !renderer.navigation_view || !renderer.shell_navigation_scene) return false;
    const auto* pane = ui_scene_resolve(*renderer.ui_scene,
        ui_scene_find(*renderer.ui_scene, UiSemantic::member_sidebar));
    if (!pane || !ui_rect_has_area(pane->rectangle)) return false;
    log_u64(LogLevel::info, "member_paging", "member_store_bytes", sizeof(GuildMemberStore));
    log_u64(LogLevel::info, "member_paging", "member_record_bytes", sizeof(GuildMemberRecord));
    log_u64(LogLevel::info, "member_paging", "user_record_bytes", sizeof(UserRecord));
    log_u64(LogLevel::info, "member_paging", "projection_bytes", sizeof(MemberListView));
    log_u64(LogLevel::info, "member_paging", "projection_row_bytes", sizeof(MemberListRow));
    log_u64(LogLevel::info, "member_paging", "scene_bytes", sizeof(MemberListScene));
    log_u64(LogLevel::info, "member_paging", "scene_row_bytes", sizeof(MemberListRowWidget));
    log_u64(LogLevel::info, "member_paging", "subscription_bytes", sizeof(MemberListSubscriptionRetry));
    MemberListRowWidget rows[64]{};
    UiWidget widgets[257]{};
    UiLayoutNode nodes[257]{};
    MemberListScene scene{};
    UserRecord user_records[2]{};
    u32 user_order[2]{};
    u8 user_names[1024]{};
    UserStore users{};
    GuildMemberRecord member_records[2]{};
    u32 member_order[2]{};
    GuildMemberStore members{};
    members.records = member_records; members.order = member_order;
    members.capacity = 2; members.count = 1; members.revision = 1;
    member_records[0].guild_id = 90001; member_records[0].list_id = 90002;
    member_records[0].user_id = 90003; member_records[0].generation = 1;
    member_records[0].list_index = 50001; member_records[0].occupied = true;
    member_records[0].status = GuildMemberStatus::online;
    MemberListRow resident[3]{};
    resident[0].kind = MemberListRowKind::group;
    resident[0].group_role_id = kMemberListOnlineGroup;
    resident[0].logical_index = 50000; resident[0].group_count = 75000;
    resident[1].kind = MemberListRowKind::member;
    resident[1].user_id = 90003; resident[1].member_slot = 0;
    resident[1].member_generation = 1; resident[1].logical_index = 50001;
    resident[1].status = GuildMemberStatus::online;
    MemberListView source{};
    source.rows = {resident, 3}; source.count = 100000; source.member_count = 99998;
    source.resident_count = 2; source.authoritative = true; source.revision = 1;
    source.guild_id = 90001; source.list_id = 90002;
    source.source_members = &members; source.source_member_revision = members.revision;
    const auto* previous = renderer.member_list_scene;
    const UiRect previous_scrollbar_viewport = renderer.scrollbar_viewport;
    u32 stage = __LINE__;
    bool valid = user_store_initialize(&users, {user_records, 2}, {user_order, 2}, {user_names, 1024}) &&
        user_store_upsert(&users, 90003, utf8_literal("Resident at wire 50001")).ok() &&
        user_store_upsert(&users, 90004, utf8_literal("Arrived at wire 50002")).ok() &&
        member_list_scene_initialize(&scene, {rows, 64}, {widgets, 257}, {nodes, 257});
    scene.measure = &measure;
    const float scroll = 50000.f * kMemberListRowHeight;
    UiRect gap_rectangle{}, known_rectangle{};
    UiScrollbarGeometry scrollbar{};
    if (valid) {
        stage = __LINE__;
        valid = member_list_scene_update(&scene, &source, &members, &users,
            renderer.chat_state, pane->rectangle, scroll).ok();
    }
    if (valid) {
        stage = __LINE__;
        const auto* gap = wire_row(scene, 50002);
        const auto* known = wire_row(scene, 50001);
        const auto* gap_layout = gap ? member_list_scene_layout_node(scene, gap->widget) : nullptr;
        const auto* known_layout = known ? member_list_scene_layout_node(scene, known->widget) : nullptr;
        valid = paging_check(gap != nullptr, "gap_wire_missing") &&
            paging_check(known != nullptr, "resident_wire_missing") &&
            paging_check(gap_layout != nullptr, "gap_layout_missing") &&
            paging_check(known_layout != nullptr, "resident_layout_missing") &&
            paging_check(!gap->resident, "gap_unexpectedly_resident") &&
            paging_check(known->resident, "known_wire_not_resident");
        if (valid) {
            gap_rectangle = gap_layout->rectangle; known_rectangle = known_layout->rectangle;
            scrollbar = ui_scrollbar_geometry(scene.viewport, scene.scroll_dips, scene.content_height);
            const auto* widget = ui_widget_manager_resolve(scene.widgets, gap->widget);
            // Select the native member scrollbar just as hovering this pane
            // does in the application; restore the caller's owner below.
            renderer.scrollbar_viewport = scene.viewport;
            log_u64(LogLevel::info, "member_paging", "gap_top_millidips", static_cast<u64>(gap_rectangle.top * 1000));
            log_u64(LogLevel::info, "member_paging", "resident_top_millidips", static_cast<u64>(known_rectangle.top * 1000));
            log_u64(LogLevel::info, "member_paging", "scene_first_wire", scene.first);
            log_u64(LogLevel::info, "member_paging", "scene_materialized_rows", scene.count);
            valid = paging_check(widget != nullptr, "gap_widget_missing") &&
                paging_check(!(widget->flags & (ui_widget_hit_test | ui_widget_focusable)), "gap_widget_interactive") &&
                paging_check(member_list_scene_hit_test(scene, (gap_rectangle.left + gap_rectangle.right) * .5f,
                    (gap_rectangle.top + gap_rectangle.bottom) * .5f).user_id == 0, "gap_hit_resolves_member") &&
                paging_check(gap_rectangle.top - known_rectangle.top == kMemberListRowHeight, "wire_row_spacing_incorrect") &&
                paging_check(scrollbar.visible, "logical_scrollbar_missing") &&
                paging_check(scene.source_count == 100000, "logical_count_incorrect") &&
                paging_check(scene.count < 64, "materialization_unbounded") &&
                text_contract(renderer, scene, false) &&
                paging_check(renderer_bind_member_list_scene(&renderer, &scene), "bind_scene_failed") &&
                paging_check(renderer_rebuild_content_text(&renderer, kDefaultTheme), "gap_renderer_text_failed") &&
                paging_check(probe_present(&renderer, kDefaultTheme), "gap_hardware_present_failed") &&
                paging_check(thumb_painted(renderer, scrollbar), "gap_scrollbar_thumb_not_painted") &&
                paging_check(capture(L"member-paging-gaps.bmp"), "gap_capture_failed");
        }
    }
    if (valid) {
        stage = __LINE__;
        member_records[1] = member_records[0];
        member_records[1].user_id = 90004; member_records[1].list_index = 50002;
        members.count = 2; ++members.revision;
        resident[2] = resident[1]; resident[2].user_id = 90004;
        resident[2].member_slot = 1; resident[2].logical_index = 50002;
        source.resident_count = 3; source.source_member_revision = members.revision; ++source.revision;
        member_order[1] = 1;
        valid = member_list_scene_update(&scene, &source, &members, &users,
            renderer.chat_state, pane->rectangle, scroll).ok();
        const auto* filled = wire_row(scene, 50002);
        const auto* known = wire_row(scene, 50001);
        const auto* filled_layout = filled ? member_list_scene_layout_node(scene, filled->widget) : nullptr;
        const auto* known_layout = known ? member_list_scene_layout_node(scene, known->widget) : nullptr;
        const auto after = ui_scrollbar_geometry(scene.viewport, scene.scroll_dips, scene.content_height);
        valid = paging_check(valid, "arrival_scene_update_failed") &&
            paging_check(filled != nullptr, "arrival_wire_missing") &&
            paging_check(filled->resident, "arrival_wire_not_resident") &&
            paging_check(filled_layout != nullptr, "arrival_layout_missing") &&
            paging_check(known_layout != nullptr, "arrival_neighbor_layout_missing") &&
            paging_check(ui_rect_equal(filled_layout->rectangle, gap_rectangle), "arrival_moved_wire_position") &&
            paging_check(ui_rect_equal(known_layout->rectangle, known_rectangle), "arrival_moved_neighbor") &&
            paging_check(ui_rect_equal(after.track, scrollbar.track), "arrival_moved_scrollbar_track") &&
            paging_check(ui_rect_equal(after.thumb, scrollbar.thumb), "arrival_moved_scrollbar_thumb") &&
            paging_check(after.maximum_scroll == scrollbar.maximum_scroll, "arrival_changed_scroll_extent") &&
            paging_check(member_list_scene_hit_test(scene, (gap_rectangle.left + gap_rectangle.right) * .5f,
                (gap_rectangle.top + gap_rectangle.bottom) * .5f).user_id == 90004, "arrival_hit_incorrect") &&
            text_contract(renderer, scene, true) &&
            paging_check(renderer_rebuild_content_text(&renderer, kDefaultTheme), "arrival_renderer_text_failed") &&
            paging_check(probe_present(&renderer, kDefaultTheme), "arrival_hardware_present_failed") &&
            paging_check(thumb_painted(renderer, after), "arrival_scrollbar_thumb_not_painted") &&
            paging_check(capture(L"member-paging-arrival.bmp"), "arrival_capture_failed");
    }
    // Rebuild owned renderer text while fixture backing still exists, removing
    // every borrowed scene/text pointer before automatic backing teardown.
    renderer.member_list_scene = previous;
    renderer.scrollbar_viewport = previous_scrollbar_viewport;
    const bool restored = renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
        probe_present(&renderer, kDefaultTheme);
    if (!valid) log_u64(LogLevel::error, "probe", "member_paging_failure_line", stage);
    log_write(valid && restored ? LogLevel::info : LogLevel::error, "probe",
        valid && restored ? "member_paging_sparse_hardware_passed" : "member_paging_sparse_hardware_failed");
    return valid && restored;
}
} // namespace maicord
