#include "probe_present.hpp"
#include "maicord/core/log.hpp"
#include "maicord/model/read_state.hpp"
#include "maicord/ui/channel_context_policy.hpp"
#include "maicord/ui/mention_badge.hpp"
#include "maicord/ui/shell_text_presentation.hpp"

namespace maicord {
namespace {
constexpr u64 kGuild = 92001, kUnreadGuild = 92002, kMentionGuild = 92003;
constexpr u64 kUnread = 92100, kMentions = 92101, kThread = 92102, kSelected = 92103;
constexpr u64 kOtherUnread = 92200, kOtherMentions = 92300;
constexpr u32 kMarkAction = 701, kCloseAction = 702;
bool check(bool valid, const char* failure) noexcept {
    if (!valid) log_write(LogLevel::error, "read_state", failure);
    return valid;
}
struct ReadStateFixture {
    ChatStateStore state{};
    GuildRecord guilds[3]{};
    u16 guild_order[3]{};
    ChannelRecord channels[8]{};
    u16 channel_order[8]{};
    u8 names[4096]{};
    ReadStateStore reads{};
    ReadStateEntry pending[8]{};
    NavigationView view{};
    GuildViewRow guild_rows[3]{};
    ChannelViewRow channel_rows[8]{};
    u16 channel_scratch[8]{};
    u8 visibility[8]{};
    ChannelSortKey sort_keys[8]{};
    GuildRailScene rail{};
    GuildRailRowWidget rail_rows[4]{};
    UiWidget rail_widgets[9]{};
    UiLayoutNode rail_nodes[9]{};
    ShellNavigationScene navigation{};
    ShellNavigationRowWidget navigation_rows[8]{};
    UiWidget navigation_widgets[65]{};
    UiLayoutNode navigation_nodes[65]{};
    PopupScene popup{};
    PopupItem popup_items[2]{};
    PopupRowWidget popup_rows[2]{};
    UiWidget popup_widgets[6]{};
    UiLayoutNode popup_nodes[6]{};
    MessageStore messages{};
    MessageRecord message_records[1]{};
    u32 message_order[1]{};
    u8 message_bytes[128]{};
    UiVariableList message_list{};
    u32 message_units[1]{};
    u64 message_fenwick[1]{};
    MessageTimeline timeline{};
    MessageScene message_scene{};
    MessageSceneRowWidget message_rows[1]{};
    UiWidget message_widgets[4]{};
    UiLayoutNode message_nodes[4]{};
};
bool channel(ReadStateFixture& f, u64 id, u64 guild, Utf8View name, u8 type,
    u32 position, u64 last_message, u64 read_message, u32 mentions, u64 parent = 0) noexcept {
    if (!check(id < read_message && read_message <= last_message, "fixture_snowflake_chronology_invalid")) return false;
    ChannelInput input{};
    input.id = id; input.guild_id = guild; input.parent_id = parent; input.name = name;
    input.type = type; input.position = position; input.last_message_id = last_message; input.last_message_known = true;
    if (!check(chat_state_store_upsert_channel(&f.state, input).ok(), "fixture_channel_upsert_failed")) {
        log_u64(LogLevel::error, "read_state", "fixture_channel_id", id);
        return false;
    }
    const auto* inserted = chat_state_store_find_channel(f.state, id);
    if (!check(inserted != nullptr, "fixture_inserted_channel_missing")) return false;
    auto& record = f.channels[inserted - f.channels];
    record.permissions_resolved = true;
    record.effective_permissions = kPermissionViewChannel | kPermissionReadMessageHistory;
    record.actions = channel_action_view | channel_action_read_history;
    ReadStateEntry entry{};
    entry.channel_id = id; entry.message_id = read_message; entry.mention_count = mentions;
    entry.fields = read_state_cursor | read_state_mentions;
    return check(read_state_upsert(&f.state, entry, 1), "fixture_authoritative_read_upsert_failed");
}
bool initialize(ReadStateFixture& f) noexcept {
    return check(chat_state_store_initialize(&f.state, {f.guilds, 3}, {f.guild_order, 3},
            {f.channels, 8}, {f.channel_order, 8}, {f.names, 4096}), "fixture_state_initialize_failed") &&
        check(read_state_initialize(&f.reads, {f.pending, 8}), "fixture_read_owner_initialize_failed") &&
        check(read_state_bind(&f.state, &f.reads), "fixture_read_owner_bind_failed") &&
        check(chat_state_store_upsert_guild(&f.state, {kGuild, utf8_literal("Read-state QA"), GuildMutationKind::create, 0}).ok(), "fixture_qa_guild_upsert_failed") &&
        check(chat_state_store_upsert_guild(&f.state, {kUnreadGuild, utf8_literal("Unread only"), GuildMutationKind::create, 0}).ok(), "fixture_unread_guild_upsert_failed") &&
        check(chat_state_store_upsert_guild(&f.state, {kMentionGuild, utf8_literal("Mentions only"), GuildMutationKind::create, 0}).ok(), "fixture_mentions_guild_upsert_failed") &&
        channel(f, kUnread, kGuild, utf8_literal("unread"), 0, 0, 1'000'000, 999'900, 0) &&
        channel(f, kMentions, kGuild, utf8_literal("mentions only"), 0, 1, 1'100'000, 1'100'000, 1200) &&
        channel(f, kThread, kGuild, utf8_literal("thread unread"), 11, 0, 1'200'000, 1'100'000, 1, kUnread) &&
        channel(f, kSelected, kGuild, utf8_literal("already read"), 0, 2, 1'300'000, 1'300'000, 0) &&
        channel(f, kOtherUnread, kUnreadGuild, utf8_literal("other unread"), 0, 0, 2'000'000, 1'999'900, 0) &&
        channel(f, kOtherMentions, kMentionGuild, utf8_literal("other mentions"), 0, 0, 3'000'000, 3'000'000, 142) &&
        check(chat_state_store_select_channel(&f.state, kSelected).ok(), "fixture_selected_channel_failed") &&
        check(navigation_view_initialize(&f.view, {f.guild_rows, 3}, {f.channel_rows, 8},
            {f.channel_scratch, 8}, {f.visibility, 8}, {f.sort_keys, 8}), "fixture_navigation_initialize_failed") &&
        check(guild_rail_scene_initialize(&f.rail, {f.rail_rows, 4}, {f.rail_widgets, 9}, {f.rail_nodes, 9}), "fixture_guild_scene_initialize_failed") &&
        check(shell_navigation_scene_initialize(&f.navigation, {f.navigation_rows, 8},
            {f.navigation_widgets, 65}, {f.navigation_nodes, 65}), "fixture_channel_scene_initialize_failed") &&
        check(popup_scene_initialize(&f.popup, {f.popup_items, 2}, {f.popup_rows, 2},
            {f.popup_widgets, 6}, {f.popup_nodes, 6}), "fixture_popup_initialize_failed") &&
        check(message_store_initialize(&f.messages, {f.message_records, 1}, {f.message_order, 1}, {f.message_bytes, 128}), "fixture_message_store_initialize_failed") &&
        check(ui_variable_list_initialize(&f.message_list, {f.message_units, 1}, {f.message_fenwick, 1}, 0, 1), "fixture_message_list_initialize_failed") &&
        check(message_timeline_initialize(&f.timeline, &f.messages, &f.message_list, 1, 1, true), "fixture_message_timeline_initialize_failed") &&
        check(message_scene_initialize(&f.message_scene, &f.timeline, {f.message_rows, 1}, {f.message_widgets, 4}, {f.message_nodes, 4}), "fixture_message_scene_initialize_failed");
}
bool state_contract(const ReadStateFixture& f, bool read) noexcept {
    const auto* unread = navigation_view_find_channel(f.view, kUnread);
    const auto* mentions = navigation_view_find_channel(f.view, kMentions);
    const auto* thread = navigation_view_find_channel(f.view, kThread);
    const auto* guild = navigation_view_find_guild(f.view, kGuild);
    const auto* other_unread = navigation_view_find_guild(f.view, kUnreadGuild);
    const auto* other_mentions = navigation_view_find_guild(f.view, kMentionGuild);
    if (!unread || !mentions || !thread || !guild || !other_unread || !other_mentions) return check(false, "expected_projection_rows_missing");
    return check(unread->unread == !read && !unread->mention_count, "white_unread_state_incorrect") &&
        check(!mentions->unread && mentions->mention_count == (read ? 0u : 1200u), "independent_channel_mentions_incorrect") &&
        check(thread->unread == !read && thread->mention_count == (read ? 0u : 1u), "thread_read_state_incorrect") &&
        check(guild->unread == !read && guild->mention_count == (read ? 0u : 1201u), "guild_activity_incorrect") &&
        check(other_unread->unread == !read && !other_unread->mention_count, "independent_guild_white_unread_incorrect") &&
        check(!other_mentions->unread && other_mentions->mention_count == (read ? 0u : 142u), "independent_guild_mentions_incorrect");
}
bool text_contract(const D3D11Renderer& renderer, const ReadStateFixture& f, bool read) noexcept {
    UiTextCommand commands[kMaximumShellChromeTextCommands]{};
    char bytes[4096]{};
    UiTextCommandStream stream{};
    if (!ui_text_command_stream_initialize(&stream, {commands, kMaximumShellChromeTextCommands}, {bytes, 4096}) ||
        !shell_text_presentation_build(*renderer.ui_scene, f.state, f.view, kDefaultTheme, f.navigation,
            nullptr, nullptr, nullptr, &stream, nullptr, &f.rail)) return check(false, "shell_text_build_failed");
    u32 found = 0, badges = 0, capped = 0, three_digits = 0, single_digit = 0;
    for (u32 i = 0; i < stream.count; ++i) {
        const auto& command = commands[i];
        bool badge = false;
        u32 raw_count = 0;
        UiRect expected_rectangle{};
        if (command.owner.manager == &f.rail.widgets) {
            for (u32 j = 0; j < f.rail.count; ++j) {
                const auto& row = f.rail.rows[j];
                if (command.owner.handle.slot != row.widget.slot || command.owner.handle.generation != row.widget.generation) continue;
                const auto* layout = guild_rail_scene_layout_node(f.rail, row);
                if (!check(layout != nullptr, "guild_badge_owner_layout_missing")) return false;
                // The rail row also owns its server acronym; only the badge slot is a badge.
                expected_rectangle = ui_mention_badge_guild_rectangle(layout->rectangle, row.view.mention_count);
                badge = row.view.mention_count && ui_rect_equal(command.rectangle, expected_rectangle);
                raw_count = row.view.mention_count;
            }
        } else if (command.owner.manager == &f.navigation.widgets) {
            for (u32 j = 0; j < f.navigation.count; ++j) {
                const auto& row = f.navigation.rows[j];
                if (!row.mention_badge.generation || command.owner.handle.slot != row.mention_badge.slot ||
                    command.owner.handle.generation != row.mention_badge.generation) continue;
                const auto* layout = ui_widget_manager_layout_node(f.navigation.widgets, row.mention_badge);
                if (!check(layout != nullptr, "channel_badge_owner_layout_missing")) return false;
                badge = true; raw_count = row.view.mention_count; expected_rectangle = layout->rectangle;
            }
        }
        if (badge) {
            char label[kUiMentionBadgeMaximumTextBytes]{};
            const auto expected_label = ui_mention_badge_label(raw_count, {label, sizeof(label)});
            if (!check(!read && raw_count && utf8_equal(command.text, expected_label), "native_numeric_badge_label_incorrect") ||
                !check(command.color.red == 1 && command.color.green == 1 && command.color.blue == 1 && command.color.alpha == 1 &&
                    command.style_flags == (text_style_bold | text_style_subtext) &&
                    command.horizontal_alignment == UiTextAlignment::center && command.vertical_alignment == UiTextAlignment::center &&
                    command.wrapping == UiTextWrapping::no_wrap && ui_rect_equal(command.rectangle, expected_rectangle),
                    "native_numeric_badge_style_or_geometry_incorrect")) return false;
            ++badges;
            capped += static_cast<u32>(utf8_equal(command.text, utf8_literal("1k+")));
            three_digits += static_cast<u32>(utf8_equal(command.text, utf8_literal("142")));
            single_digit += static_cast<u32>(utf8_equal(command.text, utf8_literal("1")));
        }
        if (command.owner.manager != &f.navigation.widgets) continue;
        const bool unread_label = utf8_equal(command.text, utf8_literal("unread")) ||
            utf8_equal(command.text, utf8_literal("thread unread"));
        const bool mentions_label = utf8_equal(command.text, utf8_literal("mentions only"));
        if (!unread_label && !mentions_label) continue;
        const u64 channel_id = mentions_label ? kMentions :
            utf8_equal(command.text, utf8_literal("thread unread")) ? kThread : kUnread;
        const auto* row = navigation_view_find_channel(f.view, channel_id);
        if (!check(row && row->selectable && (row->actions & channel_action_view),
                   "readable_channel_label_destination_authority_missing")) return false;
        // Admitted read-only destinations retain the standard navigation text
        // color after their independent unread marker and mention badge clear.
        const auto expected = unread_label && !read ? kDefaultTheme.text_primary : kDefaultTheme.text_secondary;
        if (!check(command.color.red == expected.red && command.color.green == expected.green &&
            command.color.blue == expected.blue && command.color.alpha == expected.alpha, "channel_label_read_color_incorrect")) return false;
        ++found;
    }
    return check(found == 3, "channel_label_text_missing") &&
        check(badges == (read ? 0u : 4u) && capped == (read ? 0u : 2u) &&
            three_digits == (read ? 0u : 1u) && single_digit == (read ? 0u : 1u), "native_numeric_badge_commands_missing");
}
bool mark_read(ReadStateFixture& f) noexcept {
    const u64 ids[]{kUnread, kMentions, kThread, kOtherUnread, kOtherMentions};
    for (const auto id : ids) {
        const auto* record = chat_state_store_find_channel(f.state, id);
        if (!record || !read_state_mark_local(&f.state, id, record->last_message_id, 1, id == kThread ? 2 : 1)) return false;
    }
    return true;
}
bool paint_contract(const D3D11Renderer& renderer, bool read) noexcept {
    const auto& stream = renderer.paint_stream;
    if (!stream.quads || stream.count > stream.quad_capacity || stream.count > kMaxPaintQuads) return false;
    u32 white = 0, red = 0;
    for (u32 i = 0; i < stream.count; ++i) {
        const auto& quad = stream.quads[i];
        const auto color = quad.fill;
        const auto& notification = kDefaultTheme.notification;
        // Rail badges add a rail-coloured cutout ring around the same 16-DIP pill.
        const bool pill = quad.radius == 8.f || (quad.border_width > 0 && quad.radius == 8.f + quad.border_width);
        if (pill && color.red == notification.red && color.green == notification.green &&
            color.blue == notification.blue && color.alpha == notification.alpha) ++red;
        const auto& primary = kDefaultTheme.text_primary;
        const float width = quad.right - quad.left;
        const float height = quad.bottom - quad.top;
        // The selected guild's animated 8-DIP pill is also white. Count only
        // the guild's square unread dot and the narrower channel markers.
        if (((quad.radius == 3.f && width == 6.f) ||
             (quad.radius == 4.f && width == 8.f && height == 8.f)) &&
            color.red == primary.red && color.green == primary.green && color.blue == primary.blue &&
            color.alpha == primary.alpha) ++white;
    }
    return check(white == (read ? 0u : 3u), "native_white_unread_markers_incorrect") &&
        check(red == (read ? 0u : 4u), "native_independent_mention_badges_incorrect");
}
} // namespace

// This is native presentation QA, not an authenticated account or ACK request.
bool read_state_probe(D3D11Renderer& renderer, UiTextMeasureCache& measure,
    bool (*capture)(const wchar_t*) noexcept) noexcept {
    (void)measure;
    if (!capture || !renderer.ui_scene || !renderer.chat_state || !renderer.navigation_view ||
        !renderer.guild_rail_scene || !renderer.shell_navigation_scene) return false;
    const auto* guild_pane = ui_scene_resolve(*renderer.ui_scene, ui_scene_find(*renderer.ui_scene, UiSemantic::guild_navigation));
    const auto* channel_pane = ui_scene_resolve(*renderer.ui_scene, ui_scene_find(*renderer.ui_scene, UiSemantic::channel_navigation));
    const auto* root = ui_scene_resolve(*renderer.ui_scene, ui_scene_find(*renderer.ui_scene, UiSemantic::root));
    const auto* messages = ui_scene_resolve(*renderer.ui_scene, ui_scene_find(*renderer.ui_scene, UiSemantic::message_view));
    if (!guild_pane || !channel_pane || !root || !messages || !ui_rect_has_area(guild_pane->rectangle) || !ui_rect_has_area(channel_pane->rectangle)) return false;
    ReadStateFixture f{};
    const auto* previous_state = renderer.chat_state;
    const auto* previous_view = renderer.navigation_view;
    const auto* previous_rail = renderer.guild_rail_scene;
    const auto* previous_navigation = renderer.shell_navigation_scene;
    const auto* previous_popup = renderer.popup_scene;
    const auto* previous_messages = renderer.message_scene;
    const auto* previous_message_text = renderer.message_text;
    const auto* previous_action_bar = renderer.message_action_bar;
    const auto* previous_composer = renderer.message_composer_scene;
    const auto* previous_members = renderer.member_list_scene;
    const auto* previous_focus = renderer.focus_owner;
    const UiRect previous_scrollbar = renderer.scrollbar_viewport;
    bool valid = check(initialize(f), "fixture_initialize_failed");
    if (valid) {
        renderer.chat_state = &f.state; renderer.navigation_view = &f.view;
        renderer.message_text = nullptr; renderer.message_action_bar = nullptr;
        renderer.message_composer_scene = nullptr; renderer.member_list_scene = nullptr; renderer.focus_owner = nullptr;
        renderer.scrollbar_viewport = {};
        valid = renderer_bind_guild_rail_scene(&renderer, &f.rail) &&
            renderer_bind_shell_navigation_scene(&renderer, &f.navigation) && renderer_bind_popup_scene(&renderer, &f.popup) &&
            message_scene_update(&f.message_scene, messages->rectangle).ok() && renderer_bind_message_scene(&renderer, &f.message_scene);
    }
    for (u32 pass = 0; valid && pass < 2; ++pass) {
        const bool read = pass != 0;
        if (read) { popup_scene_close(&f.popup); valid = check(mark_read(f), "local_read_mutation_failed"); }
        valid = valid && navigation_view_rebuild(&f.view, f.state).ok() &&
            guild_rail_scene_update(&f.rail, &f.view, guild_pane->rectangle, 0).ok() &&
            shell_navigation_scene_update(&f.navigation, &f.view, channel_pane->rectangle, 0).ok() &&
            state_contract(f, read) && text_contract(renderer, f, read);
        if (valid && !read) {
            const ShellNavigationRowWidget* row = nullptr;
            for (u32 i = 0; i < f.navigation.count; ++i) if (f.navigation.rows[i].view.id == kUnread) row = &f.navigation.rows[i];
            const auto* layout = row ? shell_navigation_scene_layout_node(f.navigation, *row) : nullptr;
            PopupItem items[2]{};
            const u32 count = channel_context_policy_build(f.state, kUnread, kMarkAction, kCloseAction, {items, 2});
            // A pointer anchor at the unread row's right edge keeps the popup
            // beside the pane so both independent activity cues stay visible.
            const UiRect anchor = layout ? UiRect{layout->rectangle.right - 2.f, layout->rectangle.top + 15.f,
                layout->rectangle.right, layout->rectangle.top + 16.f} : UiRect{};
            valid = check(layout && count == 1 && items[0].action == kMarkAction, "production_menu_policy_failed") &&
                popup_scene_open(&f.popup, PopupKind::context_menu, kUnread, {items, count}, anchor,
                    root->rectangle, 184.f).ok();
            if (valid) {
                PopupItemLayout item{};
                valid = check(popup_scene_resolve_item_layout(f.popup, 0, &item) && item.visible && popup_scene_hit_test(f.popup,
                    (item.visible_rectangle.left + item.visible_rectangle.right) * .5f,
                    (item.visible_rectangle.top + item.visible_rectangle.bottom) * .5f).action == kMarkAction,
                    "native_mark_read_menu_hit_incorrect");
            }
        }
        valid = valid && renderer_rebuild_shell_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme) &&
            paint_contract(renderer, read) &&
            capture(read ? L"read-state-read.bmp" : L"read-state-unread.bmp");
    }
    // Rebuild while owned fixture backing is alive; no borrowed scene or text
    // pointer may survive its teardown, including on a failed assertion.
    renderer.chat_state = previous_state; renderer.navigation_view = previous_view;
    renderer.guild_rail_scene = previous_rail; renderer.shell_navigation_scene = previous_navigation;
    renderer.popup_scene = previous_popup; renderer.message_scene = previous_messages;
    renderer.message_text = previous_message_text; renderer.message_action_bar = previous_action_bar;
    renderer.message_composer_scene = previous_composer; renderer.member_list_scene = previous_members;
    renderer.focus_owner = previous_focus; renderer.scrollbar_viewport = previous_scrollbar;
    popup_scene_release(&f.popup);
    message_scene_release(&f.message_scene);
    const bool restored = renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme);
    log_write(valid && restored ? LogLevel::info : LogLevel::error, "probe",
        valid && restored ? "read_state_native_hardware_passed" : "read_state_native_hardware_failed");
    return valid && restored;
}
} // namespace maicord
