#include "probe_present.hpp"
#include "maicord/ui/settings_choice.hpp"
#include "maicord/ui/popup_presentation.hpp"
#include "maicord/core/log.hpp"
#include "maicord/ui/settings_scene.hpp"

namespace maicord {
namespace {
// Descriptors are required by the real 1024-choice control. Only the current
// 32 labels are numbered; the remaining borrowed labels share immutable text.
struct ChoiceFixture {
    Utf8View choices[kUiSettingsChoiceMaximum];
    char labels[kUiSettingsChoiceWindow][11];
    PopupItem items[kUiSettingsChoiceMaximumItems];
    UiSettingsScene scene;
    UiPageRegistry registry;
    UiPage page;
    UiSettingsControl control;
    UiSettingsPageContent content;
};
static ChoiceFixture fixture;

void labels_for(ChoiceFixture& f, u32 first) noexcept {
    for (auto& choice : f.choices) choice = utf8_literal("Native choice");
    f.choices[1023] = utf8_literal("Choice 1023");
    for (u32 i = 0; i < kUiSettingsChoiceWindow; ++i) {
        auto& label = f.labels[i];
        const char prefix[] = "Choice ";
        for (u32 j = 0; j < 7; ++j) label[j] = prefix[j];
        u32 value = first + i;
        for (u32 j = 11; j > 7; --j) { label[j - 1] = static_cast<char>('0' + value % 10); value /= 10; }
        f.choices[first + i] = {label, 11};
    }
    ++f.content.revision;
}

bool page_geometry(ChoiceFixture& f, PopupScene& popup, u32 first) noexcept {
    const u32 count = ui_settings_choice_items(f.control, first, {f.items, kUiSettingsChoiceMaximumItems});
    const u32 expected = first == 512 ? 34 : 33;
    if (count != expected || popup.item_count != count || popup.scroll_max <= 0) return false;
    u32 checked = 0;
    for (u32 i = 0; i < count; ++i) {
        checked += (popup.items[i].flags & popup_item_checked) != 0;
        if (!utf8_equal(popup.items[i].label, f.items[i].label) || popup.items[i].action != f.items[i].action) return false;
    }
    if (checked != (first == 992 ? 1u : 0u)) return false;
    u32 next = 0;
    if (first < 992 && (!ui_settings_choice_page(1024, first, false, &next) || next != first + 32)) return false;
    if (first && (!ui_settings_choice_page(1024, first, true, &next) || next != first - 32)) return false;
    bool changed = false;
    const u32 selected = first == 992 ? count - 1 : first ? 1 : 0;
    if (!popup_scene_select_index(&popup, selected, &changed)) return false;
    PopupItemLayout row{};
    if (!popup_scene_resolve_item_layout(popup, selected, &row) || !row.visible) return false;
    const auto r = row.visible_rectangle;
    const auto hit = popup_scene_hit_test(popup, (r.left + r.right) * .5f, (r.top + r.bottom) * .5f);
    u32 absolute = 0;
    if (!hit.inside || !hit.enabled || !ui_settings_choice_selected(f.control, first, hit.action, &absolute) ||
        absolute != (first == 992 ? 1023 : first)) return false;
    UiTextCommand commands[34]{}; UiTextCommandStream text{};
    UiTextOutput text_output{&text, nullptr};
    if (!ui_text_command_stream_initialize(&text, {commands, 34}) ||
        !popup_text_presentation_build_output(popup, kDefaultTheme, &text_output)) return false;
    bool found = false;
    for (u32 i = 0; i < text.count; ++i) {
        const auto& command = commands[i];
        if (command.owner.handle.slot == popup.rows[selected].label.slot) {
            found = utf8_equal(command.text, f.choices[absolute]) && ui_rect_has_area(ui_rect_intersection(command.rectangle, command.clip));
        }
    }
    return found;
}
}

bool settings_choice_ui_probe(D3D11Renderer& renderer, PopupScene& popup,
    bool (*capture)(const wchar_t*) noexcept) noexcept {
    if (!capture || renderer.popup_scene != &popup || popup.active || renderer.login_visible || !renderer.dpi) return false;
    auto& f = fixture;
    f.control = {1, utf8_literal("Native choice list"),
        utf8_literal("1024 choices, 32 per page. Current choice: 1023."),
        UiSettingsControlKind::choice, true, false, {f.choices, 1024}, 1023, {}};
    f.content = {utf8_literal("Choice fixture"), utf8_literal("Native page and popup projection; no live settings are changed."), {&f.control, 1}, 1};
    const auto* old_scene = renderer.settings_scene;
    const u32 width = renderer.width, height = renderer.height;
    struct Restore {
        D3D11Renderer& renderer; PopupScene& popup; ChoiceFixture& f;
        const UiSettingsScene* old_scene; u32 width, height;
        ~Restore() noexcept { popup_scene_close(&popup); renderer_bind_settings_scene(&renderer, old_scene);
            ui_settings_scene_release(&f.scene); renderer_resize(&renderer, width, height); }
    } restore{renderer, popup, f, old_scene, width, height};
    bool valid = ui_page_registry_initialize(&f.registry, {&f.page, 1}) &&
        ui_page_registry_register(&f.registry, {{1, 9}, utf8_literal("Choices"), &f.content}).ok();
    const u32 pages[] = {0, 512, 992};
    const wchar_t* names[] = {L"settings-choice-first.bmp", L"settings-choice-middle-compact.bmp", L"settings-choice-last.bmp"};
    u32 stage = __LINE__;
    for (u32 i = 0; valid && i < 3; ++i) {
        stage = __LINE__;
        popup_scene_close(&popup);
        const float pixels = static_cast<float>(renderer.dpi) / 96.f;
        valid = renderer_resize(&renderer, i == 1 ? static_cast<u32>(640 * pixels) : width,
            i == 1 ? static_cast<u32>(480 * pixels) : height);
        labels_for(f, pages[i]);
        const UiRect viewport{0, 0, renderer.width / pixels, renderer.height / pixels};
        UiRect anchor{};
        valid = valid && ui_settings_scene_update(&f.scene, &f.registry, viewport) &&
            ui_settings_scene_control_anchor(f.scene, {UiSettingsTargetKind::control, {1, 9}, 1}, &anchor);
        if (!valid) break;
        renderer_bind_settings_scene(&renderer, &f.scene);
        const u32 count = ui_settings_choice_items(f.control, pages[i], {f.items, 34});
        valid = count && popup_scene_open(&popup, PopupKind::settings_choice, 1, {f.items, count}, anchor, viewport, 240).ok() &&
            page_geometry(f, popup, pages[i]);
        // Hover a different visible row, forcing a widget interaction change. Then
        // restore the checked/first choice before capturing keyboard highlight.
        if (valid) {
            const u32 hover = pages[i] == 992 ? count - 2 : pages[i] ? 2 : 1;
            PopupItemLayout row{}; bool changed = false;
            valid = renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme);
            const u32 before_hover = renderer.paint_stream_builds;
            valid = valid && popup_scene_resolve_item_layout(popup, hover, &row) && row.visible &&
                popup_scene_set_hover(&popup, (row.visible_rectangle.left + row.visible_rectangle.right) * .5f,
                    (row.visible_rectangle.top + row.visible_rectangle.bottom) * .5f, &changed) && changed &&
                renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme) &&
                renderer.paint_stream_builds > before_hover && renderer.interaction_text_popup_revision == popup.revision &&
                page_geometry(f, popup, pages[i]);
        }
        valid = valid && renderer_rebuild_content_text(&renderer, kDefaultTheme) && probe_present(&renderer, kDefaultTheme) &&
            renderer.interaction_text_popup_revision == popup.revision && capture(names[i]);
        if (valid) { const u32 builds = renderer.paint_stream_builds;
            valid = probe_present(&renderer, kDefaultTheme) && renderer.paint_stream_builds == builds; }
    }
    popup_scene_close(&popup);
    renderer_bind_settings_scene(&renderer, old_scene);
    valid = renderer_resize(&renderer, width, height) && renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
        probe_present(&renderer, kDefaultTheme) && valid;
    if (!valid) log_u64(LogLevel::error, "probe", "settings_choice_failure_line", stage);
    log_u64(LogLevel::info, "probe", "settings_choice_dpi", renderer.dpi);
    log_write(valid ? LogLevel::info : LogLevel::error, "probe", valid ?
        "settings_choice_1024_pages_clipping_hover_and_retention_passed" : "settings_choice_hardware_failed");
    return valid;
}
} // namespace maicord
