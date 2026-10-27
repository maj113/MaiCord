#include "maicord/ui/settings_choice.hpp"
#include "maicord/ui/page_registry.hpp"
#include "maicord/ui/settings_scene.hpp"

namespace maicord {
void test_settings_choice(void (*check)(bool, const char*) noexcept) noexcept {
    static Utf8View labels[kUiSettingsChoiceMaximum];
    for (auto& label : labels)
        label = utf8_literal("An immutable native choice");
    UiSettingsControl control{77,
                              utf8_literal("Choices"),
                              {},
                              UiSettingsControlKind::choice,
                              true,
                              false,
                              {labels, kUiSettingsChoiceMaximum},
                              513};
    PopupItem items[kUiSettingsChoiceMaximumItems]{};
    u32 reached = 0, checked = 0, first = 0;
    bool complete = true;
    do {
        const u32 count = ui_settings_choice_items(control, first, {items, kUiSettingsChoiceMaximumItems});
        if (count != 32 + static_cast<u32>(first != 0) + static_cast<u32>(first + 32 < 1024)) {
            complete = false;
            break;
        }
        for (u32 index = 0; index < count; ++index) {
            const auto& item = items[index];
            if (item.action == kUiSettingsChoicePrevious || item.action == kUiSettingsChoiceNext)
                continue;
            u32 selected = ~0u;
            if (!ui_settings_choice_selected(control, first, item.action, &selected) ||
                selected != reached || item.label.data != labels[selected].data ||
                !(item.flags & popup_item_enabled))
                complete = false;
            if (item.flags & popup_item_checked) {
                ++checked;
                complete = complete && selected == 513;
            }
            ++reached;
        }
    } while (ui_settings_choice_page(control.choices.count, first, false, &first));
    check(complete && reached == 1024 && checked == 1 && first == 992,
          "choice: every maximum-budget choice has an absolute action and exactly the current value is "
          "checked");
    for (u32 page = 31; page; --page)
        complete = complete && ui_settings_choice_page(1024, first, true, &first);
    check(complete && first == 0,
          "choice: previous windows reach the first choice without a descriptor cache");
    u32 selected = 19;
    check(!ui_settings_choice_selected(control, 0, kUiSettingsChoiceActionFirst + 32, &selected) &&
              selected == 19 &&
              !ui_settings_choice_selected(control, 0, kUiSettingsChoicePrevious, &selected) &&
              selected == 19,
          "choice: selection rejects action IDs from other windows and pagination commands");
    u32 next = 17;
    check(!ui_settings_choice_page(1024, 0, true, &next) && next == 17 &&
              !ui_settings_choice_page(1024, 992, false, &next) && next == 17 &&
              !ui_settings_choice_page(1024, 1, false, &next),
          "choice: paging is bounded and preserves failure output");
    items[0].action = 987;
    labels[31] = {"\xC0", 1};
    check(!ui_settings_choice_items(control, 0, {items, kUiSettingsChoiceMaximumItems}) &&
              items[0].action == 987,
          "choice: invalid last label preflights before publishing the first descriptor");
    labels[31] = utf8_literal("An immutable native choice");
    check(!ui_settings_choice_items(control, 0, {items, 32}) && items[0].action == 987,
          "choice: pagination capacity is preflighted atomically");
    auto saved = labels[0];
    labels[0] = {reinterpret_cast<const char*>(items), 1};
    check(!ui_settings_choice_items(control, 0, {items, 34}) && items[0].action == 987,
          "choice: borrowed strings cannot alias destination descriptors");
    labels[0] = saved;
    control.enabled = false;
    check(!ui_settings_choice_items(control, 0, {items, 34}),
          "choice: disabled control cannot project a live menu");
    control.enabled = true;
    control.choices.count = 33;
    control.selected_choice = 32;
    check(ui_settings_choice_items(control, 32, {items, 34}) == 2 &&
              items[0].action == kUiSettingsChoicePrevious &&
              items[1].action == kUiSettingsChoiceActionFirst + 32 && (items[1].flags & popup_item_checked),
          "choice: short last window contains only its choices and preceding-page action");

    UiPage pages[1]{};
    UiPageRegistry registry{};
    UiSettingsScene scene{};
    UiSettingsPageContent content{utf8_literal("Choice settings"), {}, {&control, 1}, 1};
    UiRect anchor{};
    u32 index = ~0u;
    const UiSettingsTarget target{UiSettingsTargetKind::control, {1, 1}, 77};
    check(ui_page_registry_initialize(&registry, {pages, 1}) &&
              ui_page_registry_register(&registry, {{1, 1}, utf8_literal("Choices"), &content}).ok() &&
              ui_settings_scene_update(&scene, &registry, {0, 0, 800, 600}) &&
              ui_settings_scene_control(scene, target, &index) == &control && index == 0 &&
              ui_settings_scene_control_anchor(scene, target, &anchor) && ui_rect_has_area(anchor),
          "choice: current semantic control resolves its visible native value anchor");
    ++content.revision;
    check(!ui_settings_scene_control(scene, target) &&
              !ui_settings_scene_control_anchor(scene, target, &anchor),
          "choice: revised descriptors cannot be borrowed through an old settings scene");
    ui_settings_scene_release(&scene);

    PopupScene popup{};
    PopupRowWidget rows[kUiSettingsChoiceMaximumItems]{};
    UiWidget widgets[2 + 2 * kUiSettingsChoiceMaximumItems]{};
    UiLayoutNode nodes[2 + 2 * kUiSettingsChoiceMaximumItems]{};
    control.choices.count = 1024;
    control.selected_choice = 0;
    const u32 count = ui_settings_choice_items(control, 0, {items, kUiSettingsChoiceMaximumItems});
    bool changed = false;
    PopupItemLayout last{};
    const bool opened = count &&
                        popup_scene_initialize(&popup, {items, kUiSettingsChoiceMaximumItems},
                                               {rows, kUiSettingsChoiceMaximumItems},
                                               {widgets, 2 + 2 * kUiSettingsChoiceMaximumItems},
                                               {nodes, 2 + 2 * kUiSettingsChoiceMaximumItems}) &&
                        popup_scene_open(&popup, PopupKind::settings_choice, 77, {items, count},
                                         {20, 20, 40, 40}, {0, 0, 320, 180}, 240)
                            .ok();
    check(opened && popup.kind == PopupKind::settings_choice && popup.scroll_max > 0 &&
              popup_scene_select_index(&popup, count - 1, &changed) &&
              popup_scene_resolve_item_layout(popup, count - 1, &last) && last.visible &&
              popup.scroll_offset > 0 && popup_scene_page_selection(&popup, true, &changed) &&
              popup_scene_scroll(&popup, -popup.scroll_max, &changed) && popup.scroll_offset == 0,
          "choice: dedicated popup kind reveals clipped selections and supports page and scroll geometry");
    PopupItem quick[5]{};
    for (u32 i = 0; i < 4; ++i)
        quick[i] = {utf8_literal("Reaction"), i + 1, popup_item_enabled | popup_item_quick_reaction};
    quick[4] = {utf8_literal("Action"), 5, popup_item_enabled};
    const u32 opens = popup.opens, revision = popup.revision;
    check(opened &&
              !popup_scene_open(&popup, PopupKind::settings_choice, 77, {quick, 5}, {20, 20, 40, 40},
                                {0, 0, 320, 180}, 240)
                   .ok() &&
              popup.opens == opens && popup.revision == revision &&
              popup.kind == PopupKind::settings_choice,
          "choice: dedicated popup rejects reaction strips before replacing its retained list");
    popup_scene_release(&popup);
}
} // namespace maicord
