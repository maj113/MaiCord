#include "menu_row_ui_probe.hpp"
#include "probe_present.hpp"
#include "maicord/ui/message_context_menu.hpp"
#include "maicord/ui/popup_presentation.hpp"
#include "maicord/protocol/application_commands.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "../../src/render/d3d11_renderer_interaction_state.hpp"

namespace maicord {
bool menu_row_ui_probe(D3D11Renderer& renderer, bool (*capture)(const wchar_t*) noexcept) noexcept {
    if (!renderer.message_scene || !renderer.message_scene->timeline ||
        !renderer.message_scene->timeline->store || !renderer.message_scene->count)
        return false;
    const auto& messages = *renderer.message_scene;
    const auto* message = message_store_resolve(*messages.timeline->store, messages.rows[0].message);
    if (!message)
        return false;

    PopupItem storage[16]{};
    PopupRowWidget rows[16]{};
    UiWidget widgets[34]{};
    UiLayoutNode nodes[34]{};
    PopupScene menu{};
    const auto* original = renderer.popup_scene;
    struct Restore {
        D3D11Renderer& renderer;
        PopupScene& menu;
        const PopupScene* original;
        ~Restore() noexcept {
            renderer_bind_popup_scene(&renderer, original);
            popup_scene_release(&menu);
        }
    } restore{renderer, menu, original};
    if (!popup_scene_initialize(&menu, {storage, 16}, {rows, 16}, {widgets, 34}, {nodes, 34}))
        return false;
    const UiRect viewport{0, 0, static_cast<float>(renderer.width) * 96.f / renderer.dpi,
                          static_cast<float>(renderer.height) * 96.f / renderer.dpi};
    const UiRect anchor{messages.viewport.left + 32, messages.viewport.top + 48,
                        messages.viewport.left + 33, messages.viewport.top + 49};
    MessageContextMenuModel context{};
    bool valid = message_context_menu_build(&context, *message, true, true, false, nullptr, true).ok() &&
                 popup_scene_open(&menu, PopupKind::context_menu, message->id,
                                  {context.items, context.count}, anchor, viewport, 280)
                     .ok() &&
                 renderer_bind_popup_scene(&renderer, &menu);
    u32 context_icons = 0;
    for (u32 index = 0; valid && index < menu.item_count; ++index) {
        if (!popup_item_menu_row(menu.items[index]))
            continue;
        UiMenuRowGeometry geometry{};
        valid = popup_scene_menu_row_geometry(menu, index, &geometry) && ui_rect_has_area(geometry.icon) &&
                geometry.icon.right <= geometry.primary.left &&
                !menu.items[index].presentation.trailing_icon;
        ++context_icons;
    }
    valid = valid && context_icons != 0 && renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
            probe_present(&renderer, kDefaultTheme) &&
            d3d11_renderer_interaction_text_state_current(renderer) &&
            (!capture || capture(L"shared-context-menu-original-icons.bmp"));

    // Existing typed application-command data exercises the same two-line
    // suggestion adapter as the command composer. The index owns its labels
    // and descriptions until after the popup is retired below.
    constexpr auto body = utf8_literal(
        R"({"applications":[{"id":"10","name":"First"}],"application_commands":[{"id":"100","application_id":"10","version":"7","name":"bump","description":"Bump first","type":1},{"id":"300","application_id":"10","version":"9","name":"admin","description":"Admin","type":1}]})");
    u32 scratch[256]{};
    CommandIndex commands{};
    valid = valid && command_index_decode(body, {scratch, 256}, &commands) && commands.command_count == 2;
    PopupItem suggestions[3]{};
    char labels[2][64]{};
    suggestions[0] = {utf8_literal("All"), 1, popup_item_enabled | popup_item_command_category};
    for (u32 index = 0; valid && index < commands.command_count; ++index) {
        const auto& command = commands.commands[index];
        if (command.name.bytes >= sizeof(labels[index])) {
            valid = false;
            break;
        }
        labels[index][0] = '/';
        copy_bytes(labels[index] + 1, command.name.data, command.name.bytes);
        suggestions[index + 1] = {{labels[index], command.name.bytes + 1},
                                  index + 2,
                                  popup_item_enabled | popup_item_command_row};
        suggestions[index + 1].detail = command.description;
    }
    const auto* composer = renderer.message_composer_scene
                               ? ui_widget_manager_layout_node(renderer.message_composer_scene->widgets,
                                                               renderer.message_composer_scene->surface)
                               : nullptr;
    valid = valid && composer &&
            popup_scene_open(&menu, PopupKind::autocomplete, message->channel_id, {suggestions, 3},
                             composer->rectangle, viewport, 360)
                .ok();
    UiMenuRowGeometry suggestion{};
    valid = valid && popup_scene_menu_row_geometry(menu, 1, &suggestion) &&
            suggestion.icon.right <= suggestion.primary.left &&
            suggestion.primary.bottom == suggestion.detail.top &&
            popup_item_menu_presentation(menu.items[1]).secondary_line &&
            renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
            probe_present(&renderer, kDefaultTheme) &&
            d3d11_renderer_interaction_text_state_current(renderer) &&
            (!capture || capture(L"shared-command-suggestions-two-lines.bmp"));

    PopupItem rtl{utf8_literal("שלום, טקסט בדיקה ארוך עם עברית ואותיות Latin לבדיקת גבולות הטקסט"), 1,
                  popup_item_enabled, popup_shortcut_copy};
    rtl.detail = utf8_literal("Original helper text remains in its second-line slot");
    rtl.presentation = ui_menu_row_presentation(UiIcon::command, true);
    valid = valid &&
            popup_scene_open(&menu, PopupKind::context_menu, message->id, {&rtl, 1}, anchor, viewport, 220)
                .ok();
    UiMenuRowGeometry rtl_geometry{};
    valid = valid && popup_scene_menu_row_geometry(menu, 0, &rtl_geometry) &&
            renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
            probe_present(&renderer, kDefaultTheme);
    const UiRect slots[]{ui_menu_row_slot_clip(rtl_geometry, rtl_geometry.primary),
                         ui_menu_row_slot_clip(rtl_geometry, rtl_geometry.detail),
                         ui_menu_row_slot_clip(rtl_geometry, rtl_geometry.shortcut)};
    bool slot_batches[3]{};
    for (u32 index = renderer.overlay_text_first_batch; valid && index < renderer.content_text->batch_count;
         ++index) {
        const auto& batch = renderer.content_text->batches[index];
        for (u32 slot = 0; slot < 3; ++slot)
            if (batch.quad_count && ui_rect_equal(batch.clip, slots[slot]))
                slot_batches[slot] = true;
    }
    valid = valid && slot_batches[0] && slot_batches[1] && slot_batches[2] &&
            d3d11_renderer_interaction_text_state_current(renderer) &&
            (!capture || capture(L"shared-menu-narrow-rtl-slot-clips.bmp"));

    renderer_bind_popup_scene(&renderer, original);
    popup_scene_close(&menu);
    command_index_release(&commands);
    const bool restored =
        renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme);
    log_u64(LogLevel::info, "probe", "shared_menu_popup_item_bytes", sizeof(PopupItem));
    log_u64(LogLevel::info, "probe", "shared_menu_projection_bytes", sizeof(UiMenuRowGeometry));
    log_u64(LogLevel::info, "probe", "shared_menu_context_original_icons", context_icons);
    log_u64(LogLevel::info, "probe", "shared_menu_native_slot_clips",
            slot_batches[0] + slot_batches[1] + slot_batches[2]);
    log_write(valid && restored ? LogLevel::info : LogLevel::error, "probe",
              valid && restored ? "shared_menu_rows_native_passed" : "shared_menu_rows_native_failed");
    return valid && restored;
}
} // namespace maicord
