#include "probe_present.hpp"
#include "maicord/ui/channel_context_policy.hpp"
#include "maicord/ui/guild_context_policy.hpp"
#include "maicord/ui/popup_presentation.hpp"
#include "maicord/core/log.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/ui/popup_scene.hpp"

namespace maicord {
namespace {
// A native menu shows at most eight plugin rows (mc.ui.menus@2).
constexpr u32 kRows = 8;
constexpr u32 kExtraFirst = 0xF400, kMarkRead = 0xF301, kCloseDm = 0xF302, kCopyGuild = 0xF303;
constexpr Utf8View kLabels[2][kRows]{
    {utf8_literal("Open Channel Tools"), utf8_literal("Inspect Channel ID"),
     utf8_literal("Unavailable Channel Action"), utf8_literal("Open Channel Settings"),
     utf8_literal("Show Channel Details"), utf8_literal("Open Native Panel"),
     utf8_literal("Show Channel Diagnostics"), utf8_literal("Open Extension Details")},
    {utf8_literal("Open Server Tools"), utf8_literal("Inspect Server ID"),
     utf8_literal("Unavailable Server Action"), utf8_literal("Open Server Settings"),
     utf8_literal("Show Server Details"), utf8_literal("Open Native Panel"),
     utf8_literal("Show Server Diagnostics"), utf8_literal("Open Extension Details")}};
struct Fixture {
    ChatStateStore state;
    GuildRecord guild;
    ChannelRecord channel;
    RoleRecord role;
    PermissionOverwriteRecord overwrite;
    u16 guild_order[1], channel_order[1];
    PopupItem items[2 + kRows];
};
bool initialize(Fixture& f) noexcept {
    f.guild.id = 7; f.guild.occupied = true; f.guild.generation = 1; f.guild.revision = 1;
    f.channel.id = 8; f.channel.guild_id = 7; f.channel.occupied = true;
    f.channel.generation = 1; f.channel.revision = 1; f.channel.permissions_resolved = true;
    f.channel.effective_permissions = kPermissionViewChannel | kPermissionReadMessageHistory;
    f.channel.actions = channel_action_view | channel_action_read_history;
    f.state.guilds = &f.guild; f.state.guild_order = f.guild_order;
    f.state.guild_capacity = f.state.guild_count = 1;
    f.state.channels = &f.channel; f.state.channel_order = f.channel_order;
    f.state.channel_capacity = f.state.channel_count = 1;
    f.state.roles = &f.role; f.state.overwrites = &f.overwrite;
    return true;
}
u32 project(Fixture& f, u32 placement, u32 native_count) noexcept {
    // Host-copied labels; the third row is a disabled item.
    for (u32 index = 0; index < kRows; ++index)
        f.items[native_count + index] = {kLabels[placement][index], kExtraFirst + index,
            static_cast<u16>((index != 2 ? popup_item_enabled : 0) |
                (!index && native_count ? popup_item_separator_before : 0))};
    return native_count + kRows;
}
bool geometry(PopupScene& popup, u32 placement, u32 native_count, UiRect viewport) noexcept {
    const auto* panel = popup_scene_layout_node(popup, popup.panel);
    if (!panel || panel->rectangle.left < viewport.left || panel->rectangle.top < viewport.top ||
        panel->rectangle.right > viewport.right || panel->rectangle.bottom > viewport.bottom ||
        popup.item_count != native_count + kRows) return false;
    if ((popup.items[native_count].flags & popup_item_separator_before) !=
        (native_count ? popup_item_separator_before : 0)) return false;
    UiTextCommand commands[2 + kRows]{};
    UiTextCommandStream text{};
    UiTextOutput text_output{&text, nullptr};
    if (!ui_text_command_stream_initialize(&text, {commands, 2 + kRows})) return false;
    for (u32 index = 0; index < kRows; ++index) {
        bool changed = false;
        const u32 reveal = native_count + (index == 2 ? 3 : index);
        if (!popup_scene_select_index(&popup, reveal, &changed)) return false;
        PopupItemLayout layout{};
        if (!popup_scene_resolve_item_layout(popup, native_count + index, &layout) || !layout.visible ||
            !ui_rect_has_area(layout.label->content_rectangle)) return false;
        const auto r = layout.visible_rectangle;
        const auto hit = popup_scene_hit_test(popup, (r.left + r.right) * .5f, (r.top + r.bottom) * .5f);
        if (!hit.inside || hit.action != kExtraFirst + index || hit.enabled != (index != 2)) return false;
        ui_text_command_stream_reset(&text);
        if (!popup_text_presentation_build_output(popup, kDefaultTheme, &text_output)) return false;
        bool found = false;
        for (u32 command = 0; command < text.count; ++command) {
            const auto& value = commands[command];
            if (value.owner.manager != &popup.widgets || value.owner.handle.slot != popup.rows[native_count + index].label.slot)
                continue;
            const auto expected = index == 2 ? kDefaultTheme.text_secondary : kDefaultTheme.text_primary;
            found = utf8_equal(value.text, kLabels[placement][index]) && value.wrapping == UiTextWrapping::no_wrap &&
                value.overflow == UiTextOverflow::ellipsis && value.color.red == expected.red &&
                value.color.green == expected.green && value.color.blue == expected.blue &&
                value.rectangle.left >= layout.row->rectangle.left && value.rectangle.right <= layout.row->rectangle.right;
        }
        if (!found) return false;
    }
    bool changed = false;
    return popup_scene_select_index(&popup, native_count + 1, &changed) &&
        popup_scene_move_selection(&popup, 1, &changed) && changed &&
        popup_scene_selected_action(popup) == kExtraFirst + 3;
}
bool display(D3D11Renderer& renderer, PopupScene& popup, Fixture& f, u32 placement,
    u32 native_count, UiRect viewport, const wchar_t* filename,
    bool (*capture)(const wchar_t*) noexcept) noexcept {
    const u32 count = project(f, placement, native_count);
    const UiRect edge_anchor{viewport.right - 1, viewport.bottom - 1, viewport.right, viewport.bottom};
    if (!count || !popup_scene_open(&popup, PopupKind::context_menu, placement ? 7 : 8,
        {f.items, count}, edge_anchor, viewport, 264).ok() ||
        !geometry(popup, placement, native_count, viewport) ||
        !renderer_rebuild_content_text(&renderer, kDefaultTheme) ||
        !probe_present(&renderer, kDefaultTheme) || renderer.interaction_text_popup_revision != popup.revision ||
        (filename && !capture(filename))) return false;
    const u32 builds = renderer.paint_stream_builds;
    return probe_present(&renderer, kDefaultTheme) && renderer.paint_stream_builds == builds;
}
} // namespace

// Projection-only fixture: shared native policy plus eight copied plugin rows
// and caller popup/device. App/SDK routing is tested elsewhere.
bool plugin_navigation_ui_probe(D3D11Renderer& renderer, PopupScene& popup,
    bool (*capture)(const wchar_t*) noexcept) noexcept {
    if (!capture || renderer.popup_scene != &popup || popup.active || !renderer.dpi) return false;
    Fixture f{};
    const float scale = 96.f / static_cast<float>(renderer.dpi);
    const UiRect viewport{0, 0, renderer.width * scale, renderer.height * scale};
    u32 stage = __LINE__;
    bool valid = initialize(f) && channel_context_target_valid(f.state, 8) && guild_context_target_valid(f.state, 7);
    if (valid) {
        stage = __LINE__;
        const u32 native = channel_context_policy_build(f.state, 8, kMarkRead, kCloseDm, {f.items, 2});
        valid = native == 1 && f.items[0].action == kMarkRead && utf8_equal(f.items[0].label, utf8_literal("Mark as read")) &&
            display(renderer, popup, f, 0, native, viewport, L"plugin-navigation-channel.bmp", capture);
    }
    popup_scene_close(&popup);
    if (valid) {
        stage = __LINE__;
        // Eligible forum containers have plugin-only menus, no native read
        // action and no orphan separator (voice channels carry chat and read
        // like text channels). No extra screenshots are written.
        constexpr u8 plugin_only_types[]{15};
        for (const u8 type : plugin_only_types) {
            f.channel.type = type;
            valid = channel_context_target_valid(f.state, 8) &&
                !channel_context_policy_build(f.state, 8, kMarkRead, kCloseDm, {f.items, 2}) &&
                display(renderer, popup, f, 0, 0, viewport, nullptr, capture);
            popup_scene_close(&popup);
            if (!valid) break;
        }
        f.channel.type = 0;
    }
    if (valid) {
        stage = __LINE__;
        const u32 native = guild_context_policy_build(f.state, 7, kCopyGuild, {f.items, 2});
        valid = native == 1 && f.items[0].action == kCopyGuild && utf8_equal(f.items[0].label, utf8_literal("Copy server ID")) &&
            display(renderer, popup, f, 1, native, viewport, L"plugin-navigation-guild.bmp", capture);
    }
    popup_scene_close(&popup);
    const bool restored = renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme);
    if (!valid) log_u64(LogLevel::error, "probe", "plugin_navigation_failure_line", stage);
    log_u64(LogLevel::info, "probe", "plugin_navigation_fixture_dpi", renderer.dpi);
    log_write(valid && restored ? LogLevel::info : LogLevel::error, "probe",
        valid && restored ? "plugin_navigation_registry_maximum_policy_geometry_retention_passed" : "plugin_navigation_probe_failed");
    return valid && restored;
}
} // namespace maicord
