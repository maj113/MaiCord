#include "probe_present.hpp"
#include "maicord/ui/message_context_controller.hpp"
#include "maicord/ui/popup_presentation.hpp"
#include "maicord/core/log.hpp"

namespace maicord {
namespace {
inline constexpr u32 kExtraActionFirst = 0xF200;
inline constexpr Utf8View kExtraLabels[] = {
    utf8_literal("Open Plugin Settings"), utf8_literal("Inspect Semantic IDs"),
    utf8_literal("Unavailable Action"), utf8_literal("Open Label Gallery"),
    utf8_literal("Show Session Tools"), utf8_literal("Open Audit Page"),
    utf8_literal("Show Native Controls"), utf8_literal("Open Extension Details"),
};

bool extra_geometry(PopupScene& popup, u32 first) noexcept {
    UiTextCommand commands[kMessageContextMaximumPopupItems]{};
    UiTextCommandStream text{};
    if (!ui_text_command_stream_initialize(&text, {commands, kMessageContextMaximumPopupItems})) return false;
    for (u32 index = 0; index < kMessageContextMaximumExtraItems; ++index) {
        bool changed = false;
        // Reveal the next enabled row to bring the disabled row immediately
        // above it into view without giving that row keyboard focus.
        const u32 reveal_index = first + (index == 2 ? 3 : index);
        if (!popup_scene_select_index(&popup, reveal_index, &changed)) return false;
        PopupItemLayout layout{};
        if (!popup_scene_resolve_item_layout(popup, first + index, &layout) || !layout.visible ||
            !ui_rect_has_area(layout.visible_rectangle) || !ui_rect_has_area(layout.label->content_rectangle)) return false;
        const auto visible = layout.visible_rectangle;
        const auto hit = popup_scene_hit_test(popup, (visible.left + visible.right) * .5f,
            (visible.top + visible.bottom) * .5f);
        if (!hit.inside || hit.action != kExtraActionFirst + index || hit.enabled != (index != 2)) return false;
        ui_text_command_stream_reset(&text);
        if (!popup_text_presentation_build(popup, kDefaultTheme, &text)) return false;
        bool found = false;
        for (u32 command = 0; command < text.count; ++command) {
            const auto& value = commands[command];
            if (value.owner.handle.slot != popup.rows[first + index].label.slot) continue;
            const Color expected = index == 2 ? kDefaultTheme.text_secondary : kDefaultTheme.text_primary;
            found = utf8_equal(value.text, kExtraLabels[index]) && value.overflow == UiTextOverflow::ellipsis &&
                value.wrapping == UiTextWrapping::no_wrap && value.color.red == expected.red &&
                value.color.green == expected.green && value.color.blue == expected.blue &&
                value.rectangle.left >= layout.row->rectangle.left && value.rectangle.right <= layout.row->rectangle.right;
        }
        if (!found) return false;
    }
    bool changed = false;
    if (!popup_scene_select_index(&popup, first + 1, &changed) ||
        !popup_scene_move_selection(&popup, 1, &changed) || !changed ||
        popup_scene_selected_action(popup) != kExtraActionFirst + 3) return false;
    return popup_scene_select_index(&popup, first + 7, &changed);
}
bool feedback_geometry(D3D11Renderer& renderer, UiRect viewport,
    bool (*capture)(const wchar_t*) noexcept) noexcept {
    UiSettingsScene scene{};
    UiPageRegistry registry{};
    UiPage page{};
    UiSettingsControl control{1, utf8_literal("Load plugin"),
        utf8_literal("Couldn't open this plugin page. Try again."),
        UiSettingsControlKind::action, true, false, {}, 0, utf8_literal("Choose DLL")};
    UiSettingsPageContent content{utf8_literal("Plugins"),
        utf8_literal("Add features to MaiCord."), {&control, 1}, 1};
    const auto* previous = renderer.settings_scene;
    struct Restore {
        D3D11Renderer& renderer; UiSettingsScene& scene; const UiSettingsScene* previous;
        ~Restore() noexcept { renderer_bind_settings_scene(&renderer, previous); ui_settings_scene_release(&scene); }
    } restore{renderer, scene, previous};
    if (!ui_page_registry_initialize(&registry, {&page, 1}) ||
        !ui_page_registry_register(&registry, {{1, 4}, utf8_literal("Plugins"), &content}).ok() ||
        !ui_settings_scene_update(&scene, &registry, viewport)) return false;
    renderer_bind_settings_scene(&renderer, &scene);
    UiTextCommand commands[16]{}; UiTextCommandStream text{};
    if (!ui_text_command_stream_initialize(&text, {commands, 16}) ||
        !ui_settings_scene_text(scene, kDefaultTheme, &text)) return false;
    bool readable = false;
    for (u32 i = 0; i < text.count; ++i) if (utf8_equal(commands[i].text, control.description)) {
        const auto visible = ui_rect_intersection(commands[i].rectangle, commands[i].clip);
        readable = ui_rect_has_area(visible) && ui_rect_width(visible) > 200;
    }
    return readable && renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
        probe_present(&renderer, kDefaultTheme) && capture(L"plugin-ui-feedback.bmp");
}
} // namespace

// Uses the caller's existing popup backing and capture path. No separate scene,
// renderer, worker or allocation owner is added by these fixtures.
bool plugin_ui_probe(D3D11Renderer& renderer, PopupScene& popup,
    bool (*capture)(const wchar_t*) noexcept) noexcept {
    if (!capture || renderer.popup_scene != &popup || popup.active || !renderer.dpi) return false;
    const float scale = 96.f / static_cast<float>(renderer.dpi);
    const UiRect viewport{0, 0, renderer.width * scale, renderer.height * scale};
    const UiRect anchor{viewport.right * .5f, 24, viewport.right * .5f + 1, 25};
    PopupItem extras[kMessageContextMaximumExtraItems]{};
    for (u32 index = 0; index < kMessageContextMaximumExtraItems; ++index)
        extras[index] = {kExtraLabels[index], kExtraActionFirst + index,
            static_cast<u16>((index != 2 ? popup_item_enabled : 0) | (!index ? popup_item_separator_before : 0))};
    PopupItem friends[4 + kMessageContextMaximumExtraItems]{};
    friends[0] = {utf8_literal("Profile"), 0xF101, popup_item_enabled};
    friends[1] = {utf8_literal("Remove friend"), 0xF102,
        static_cast<u16>(popup_item_enabled | popup_item_danger | popup_item_separator_before)};
    friends[2] = {utf8_literal("Block"), 0xF103, static_cast<u16>(popup_item_enabled | popup_item_danger)};
    friends[3] = {utf8_literal("Ignore"), 0xF104, popup_item_enabled};
    for (u32 index = 0; index < kMessageContextMaximumExtraItems; ++index) friends[4 + index] = extras[index];
    u32 stage = __LINE__;
    bool valid = popup_scene_open(&popup, PopupKind::context_menu, 30, {friends, 12}, anchor, viewport, 200).ok() &&
        popup.item_count == 12 && extra_geometry(popup, 4) &&
        renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme) &&
        renderer.interaction_text_popup_revision == popup.revision && capture(L"plugin-ui-friends.bmp");
    if (valid) {
        stage = __LINE__;
        const u32 builds = renderer.paint_stream_builds;
        valid = probe_present(&renderer, kDefaultTheme) && renderer.paint_stream_builds == builds;
    }
    popup_scene_close(&popup);
    MessageRecord message{};
    message.id = 40; message.channel_id = 50; message.author_id = 30;
    MessageContextController controller{};
    if (valid) {
        stage = __LINE__;
        valid = message_context_controller_open(&controller, &popup, message,
            true, true, true, nullptr, anchor, viewport, true, {extras, 8}).ok() && popup.item_count == 21;
    }
    if (valid) {
        stage = __LINE__;
        PopupItemLayout quick{}, first_extra{}, last_builtin{};
        valid = popup_scene_resolve_item_layout(popup, 0, &quick) &&
            ui_rect_width(quick.row->rectangle) == kPopupQuickReactionExtent &&
            popup_scene_resolve_item_layout(popup, 12, &last_builtin) &&
            popup_scene_resolve_item_layout(popup, 13, &first_extra) &&
            first_extra.row->rectangle.top - last_builtin.row->rectangle.bottom == kPopupContextSeparatorGap &&
            extra_geometry(popup, 13) && renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
            probe_present(&renderer, kDefaultTheme) && renderer.interaction_text_popup_revision == popup.revision &&
            capture(L"plugin-ui-message.bmp");
    }
    if (valid) {
        stage = __LINE__;
        const u32 builds = renderer.paint_stream_builds;
        valid = probe_present(&renderer, kDefaultTheme) && renderer.paint_stream_builds == builds;
    }
    popup_scene_close(&popup);
    message_context_controller_close(&controller);
    const bool restored = renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme);
    if (valid && restored) {
        stage = __LINE__;
        valid = feedback_geometry(renderer, viewport, capture) &&
            renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme);
    }
    if (!valid) log_u64(LogLevel::error, "probe", "plugin_ui_failure_line", stage);
    log_write(valid && restored ? LogLevel::info : LogLevel::error, "probe",
        valid && restored ? "plugin_ui_maximum_rows_hardware_and_retention_passed" : "plugin_ui_maximum_rows_hardware_failed");
    return valid && restored;
}
} // namespace maicord
