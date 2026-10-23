#include "maicord/model/message_composer.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/ui/profile_dialog_scene.hpp"
#include "maicord/ui/button.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"

// Geometry of the profile editor page (the official Profiles settings layout:
// tab bar, customization sections, captioned colour swatches, button groups
// and the 300 DIP preview column) and of its crop / picker dialogs, at the
// window sizes the client supports.
namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;
bool close_to(float a, float b, float epsilon = .01f) noexcept {
    return a - b < epsilon && b - a < epsilon;
}
bool palette(void*, u64, u32 choice, BackgroundTheme* out) noexcept {
    *out = {};
    out->colors[0] = out->colors[1] = choice ? 0x5865F2u : 0x112233u;
    out->color_count = 2;
    return true;
}
struct Page {
    UiPage pages[2]{};
    UiPageRegistry registry{};
    Utf8View tabs[2]{utf8_literal("Main Profile"), utf8_literal("Per-server Profiles")};
    Utf8View avatar[2]{utf8_literal("Change Avatar"), utf8_literal("Remove Avatar")};
    Utf8View theme[2]{utf8_literal("Primary"), utf8_literal("Accent")};
    MessageComposer models[2]{};
    char name[64]{}, bio[256]{};
    UiSettingsEditor editors[2]{};
    UiSettingsControl controls[8]{};
    UiSettingsControlPresentation presentation[8]{};
    UiSettingsPageContent content{};
};
void build(Page* p) noexcept {
    message_composer_initialize(&p->models[0], {p->name, sizeof(p->name)});
    message_composer_initialize(&p->models[1], {p->bio, sizeof(p->bio)});
    p->editors[0] = {{message_composer_text(p->models[0]), 0, 0, false, false, true}, nullptr, utf8_literal("ada")};
    p->editors[1] = {{message_composer_text(p->models[1]), 0, 0, false, false, true}, nullptr, {}};
    p->controls[0] = {1, utf8_literal("Profiles"), {}, UiSettingsControlKind::tabs, true, false, {p->tabs, 2}, 0};
    p->controls[1] = {2, utf8_literal("Display Name"), {}, UiSettingsControlKind::edit, true, false, {}, 0, {},
                      &p->editors[0]};
    p->controls[2] = {3, utf8_literal("Must be 32 or fewer in length."), {}, UiSettingsControlKind::error, true, false};
    p->controls[3] = {4, utf8_literal("Avatar"), {}, UiSettingsControlKind::buttons, true, false, {p->avatar, 2},
                      kUiSettingsNoChoice};
    p->controls[4] = {5, utf8_literal("Profile Theme"), {}, UiSettingsControlKind::colors, true, false, {p->theme, 2},
                      kUiSettingsNoChoice, utf8_literal("Reset Theme")};
    p->controls[5] = {6, utf8_literal("Bio"), utf8_literal("You can use markdown and links if you'd like."),
                      UiSettingsControlKind::multiline, true, false, {}, 0, utf8_literal("190"), &p->editors[1]};
    for (auto& pr : p->presentation)
        pr = {UiSettingsVariant::standard, 0, kUiSettingsNoDefault};
    p->content = {utf8_literal("Profiles"), {}, {p->controls, 6}, 1, kUiSettingsPageFormat, &kUiDefaultSettingsStyle};
    p->content.presentation = p->presentation;
    p->content.sectioned = true;
    p->content.aside_width = 300;
    p->content.aside_height = 420;
    p->content.aside_row = 1;
    p->content.aside_label = utf8_literal("Preview");
}
// Scrolls the body until the control's field is fully visible and returns its
// field (index ~0u) or option rectangle in content coordinates (the page at
// scroll offset 0), so stacked layouts in short windows are measured too.
constexpr u32 kField = ~0u;
UiRect find(UiSettingsScene* scene, u64 control, u32 index = kField) noexcept {
    const UiSettingsTarget target{UiSettingsTargetKind::control, {1, 1}, control};
    ui_settings_scene_scroll(scene, false, -100000);
    for (u32 step = 0; step < 400; ++step) {
        UiSettingsControlGeometry g{};
        if (ui_settings_scene_control_geometry(*scene, target, &g) &&
            ui_rect_height(g.clip) >= ui_rect_height(g.field) - .5f) {
            UiRect r = g.field;
            if (index != kField && !ui_settings_scene_choice_rectangle(*scene, target, index, &r))
                return {};
            r.top += scene->content_offset;
            r.bottom += scene->content_offset;
            return r;
        }
        if (!ui_settings_scene_scroll(scene, false, 8))
            break;
    }
    return {};
}
UiRect visible(UiRect content, const UiSettingsScene& scene) noexcept {
    content.top -= scene.content_offset;
    content.bottom -= scene.content_offset;
    return content;
}

void test_page(Check check) noexcept {
    // 1280x720 at 100%, 1920x1080 at 150% and 2560x1440 at 200% (1280x720
    // DIP), 1366x768 at 125% (1093x614 DIP), the 632x480 DIP minimum window
    // and a large 2560x1440 window at 100%.
    const UiRect viewports[] = {{0, 0, 1280, 720}, {0, 0, 1093, 614}, {0, 0, 632, 480}, {0, 0, 2560, 1440}};
    for (const auto viewport : viewports) {
        static Page p{};
        p = {};
        build(&p);
        UiSettingsScene scene{};
        int context = 0;
        const bool built = ui_page_registry_initialize(&p.registry, {p.pages, 2}) &&
                           ui_page_registry_register(&p.registry, {{1, 1}, utf8_literal("Profiles"), &p.content}).ok() &&
                           ui_settings_scene_set_presentation(&scene, UiSettingsPresentation::modal) &&
                           ui_settings_scene_bind_palette(&scene, {&context, palette, 1}) &&
                           ui_settings_scene_update(&scene, &p.registry, viewport);
        check(built, "profile editor page: the sectioned page with an aside builds at every window size");
        if (!built)
            continue;
        UiRect heading{}, card{}, clip{};
        bool beside = false;
        const bool aside = ui_settings_scene_aside(scene, &heading, &card, &clip, &beside);
        const float column = ui_rect_width(scene.layout.column);
        const auto tabs = find(&scene, 1);
        const auto name = find(&scene, 2);
        const auto first_tab = find(&scene, 1, 0), second_tab = find(&scene, 1, 1);
        check(aside && close_to(ui_rect_width(card), minimum(300.f, column)) && close_to(ui_rect_height(card), 420) &&
                  close_to(heading.bottom + 8, card.top) && beside == (column >= 300 + 36 + 280),
              "profile editor page: the preview column is 300 DIP wide under its heading, beside the form when it fits");
        check(close_to(first_tab.left, scene.layout.column.left) && second_tab.left >= first_tab.right + 31 &&
                  close_to(ui_rect_width(tabs), column) && close_to(ui_rect_height(tabs), 37),
              "profile editor page: the tab bar spans the page with 32 DIP between tabs");
        if (beside)
            check(name.right <= card.left - 36 + .5f && card.top >= tabs.bottom + 16 - .5f,
                  "profile editor page: form fields stop 36 DIP before the preview, which starts below the tabs");
        else
            check(card.top >= tabs.bottom && name.top >= card.bottom + 24 - .5f && close_to(ui_rect_width(name), column),
                  "profile editor page: narrow windows stack the preview above full-width sections");
        const auto change = find(&scene, 4, 0), remove = find(&scene, 4, 1);
        const auto tokens = ui_settings_tokens(kDefaultTheme);
        check(close_to(change.left, name.left) && close_to(remove.left, change.right + 4) &&
                  close_to(ui_rect_height(change), tokens.metric.button_small) && close_to(change.top, remove.top),
              "profile editor page: Change and Remove sit side by side, 4 DIP apart, under the section title");
        const auto primary = find(&scene, 5, 0), accent = find(&scene, 5, 1);
        check(close_to(ui_rect_width(primary), 69) && close_to(ui_rect_height(primary), 50) &&
                  close_to(accent.left, primary.right + 16),
              "profile editor page: theme swatches are 69x50 and 16 DIP apart");
        const auto bio = find(&scene, 6);
        check(bio.top > accent.bottom && ui_rect_height(bio) >= 96,
              "profile editor page: the bio box follows the theme section");
        UiSettingsActivation hit{}, tab_hit{};
        find(&scene, 4);
        const auto shown_remove = visible(remove, scene);
        const bool remove_hit = ui_settings_scene_option_pointer(
            scene, {UiSettingsTargetKind::control, {1, 1}, 4}, (shown_remove.left + shown_remove.right) * .5f,
            (shown_remove.top + shown_remove.bottom) * .5f, &hit);
        find(&scene, 1);
        const auto shown_tab = visible(second_tab, scene);
        check(remove_hit && hit.selected_choice == 1 &&
                  ui_settings_scene_option_pointer(scene, {UiSettingsTargetKind::control, {1, 1}, 1},
                                                   (shown_tab.left + shown_tab.right) * .5f,
                                                   (shown_tab.top + shown_tab.bottom) * .5f, &tab_hit) &&
                  tab_hit.selected_choice == 1,
              "profile editor page: pointer hits resolve the pressed button and tab");
        PaintStream paint{};
        UiTextCommand commands[256]{};
        UiTextCommandStream text{};
        UiTextOutput text_output{&text, nullptr};
        check(paint_stream_initialize(&paint) &&
                  paint_stream_reset(&paint, viewport.right, viewport.bottom) &&
                  ui_settings_scene_paint(scene, kDefaultTheme, &paint) && paint.rejected == 0 &&
                  ui_text_command_stream_initialize(&text, {commands, 256}) &&
                  ui_settings_scene_text_output(scene, kDefaultTheme, &text_output) && !text.rejected,
              "profile editor page: tabs, sections, swatches, error and counter paint and emit text");
        paint_stream_release(&paint);
        ui_settings_scene_release(&scene);
        for (auto& model : p.models)
            message_composer_release(&model);
    }
}

void test_dialogs(Check check) noexcept {
    static UiProfileDialogScene scene{};
    // Short windows (800 DIP tall or less) use the official 200 DIP editor.
    const UiRect views[] = {{0, 0, 1280, 720}, {0, 0, 1920, 1200}};
    for (const auto viewport : views) {
        scene = {};
        const auto editor = ui_profile_dialog_crop_editor(viewport);
        auto crop = image_crop_begin(ImageCropUse::avatar, 1600, 1200, ui_rect_width(editor), ui_rect_height(editor));
        UiProfileDialogContent content{};
        content.kind = UiProfileDialogKind::crop;
        content.title = utf8_literal("Edit Image");
        content.apply_label = utf8_literal("Apply");
        content.image = media_asset_local_draft_preview(1, 1, MediaAttachmentFormat::png);
        content.crop = crop;
        content.image_ready = true;
        content.can_apply = true;
        const bool built = ui_profile_dialog_update(&scene, content, viewport);
        check(built && close_to(ui_rect_width(scene.rectangle), 480) && close_to(ui_rect_width(editor), 430) &&
                  close_to(ui_rect_height(editor), viewport.bottom <= 800 ? 200.f : 350.f) &&
                  close_to(ui_rect_width(scene.slider_track), 128) &&
                  close_to((scene.rectangle.left + scene.rectangle.right) * .5f, viewport.right * .5f),
              "profile crop dialog: a 480 DIP modal with a 430 DIP editor and a 128 DIP zoom slider");
        ProfileMedia dim{}, bright{};
        dim.rotation = bright.rotation = 270;
        dim.playback_epoch = bright.playback_epoch = 123;
        dim.circular = bright.circular = true;
        const bool media = ui_profile_dialog_media(scene, 0, &dim) && ui_profile_dialog_media(scene, 1, &bright);
        check(media && dim.rotation == 0 && bright.rotation == 0 && dim.playback_epoch == 0 &&
                  bright.playback_epoch == 0 && !dim.circular && !bright.circular,
              "profile crop dialog: resolver overwrites reused output");
        const auto mask = image_crop_mask_rect(crop);
        check(media && dim.domain == 1 && dim.opacity < 1 && close_to(ui_rect_width(bright.mask), mask.right - mask.left) &&
                  close_to(bright.mask_radius, ui_rect_width(bright.mask) * .5f) &&
                  bright.rectangle.left <= bright.mask.left + .01f && bright.rectangle.right >= bright.mask.right - .01f,
              "profile crop dialog: the image covers the round mask, dimmed outside it");
        // Rotation keeps the visual rectangle and flags the turn.
        image_crop_rotate(&crop);
        content.crop = crop;
        ui_profile_dialog_update(&scene, content, viewport);
        ProfileMedia turned{};
        check(ui_profile_dialog_media(scene, 1, &turned) && turned.rotation == 90 &&
                  close_to(ui_rect_height(turned.rectangle) / ui_rect_width(turned.rectangle), 1600.f / 1200.f, .01f),
              "profile crop dialog: a turned image is drawn in its turned shape");
        u32 source = 0;
        const auto zoom_hit = ui_profile_dialog_hit_test(scene, scene.slider_track.left + 64,
                                                         (scene.slider_track.top + scene.slider_track.bottom) * .5f);
        check(ui_profile_dialog_action(scene, zoom_hit, &source) == UiProfileDialogAction::zoom &&
                  close_to(ui_profile_dialog_zoom_at(scene, scene.slider_track.left + 64), .5f),
              "profile crop dialog: the slider maps its track to zoom");
        ui_profile_dialog_close(&scene);
    }
    UiProfileDialogItem items[5]{};
    items[1].sku = 11;
    items[1].label = utf8_literal("Cat Ears");
    items[2].sku = 12;
    items[3].sku = 13;
    items[4].sku = 14;
    UiProfileDialogContent picker{};
    picker.kind = UiProfileDialogKind::decoration;
    picker.title = utf8_literal("Change Avatar Decoration");
    picker.apply_label = utf8_literal("Apply");
    picker.items = {items, 5};
    picker.selected = 1;
    picker.preview_label = utf8_literal("Cat Ears");
    scene = {};
    const bool built = ui_profile_dialog_update(&scene, picker, {0, 0, 1280, 800});
    u32 tiles = 0;
    UiRect first{}, second{};
    for (u32 i = 0; i < scene.part_count; ++i) {
        u32 source = 0;
        if (ui_profile_dialog_action(scene, scene.parts[i].widget, &source) != UiProfileDialogAction::item)
            continue;
        const auto* node = ui_widget_manager_layout_node(scene.widgets, scene.parts[i].widget);
        if (node && tiles == 0)
            first = node->rectangle;
        if (node && tiles == 1)
            second = node->rectangle;
        ++tiles;
    }
    check(built && tiles == 5 && close_to(ui_rect_height(first), 80) && close_to(second.left, first.right + 12) &&
              ui_rect_width(scene.grid_viewport) < ui_rect_width(scene.rectangle) - 280,
          "profile picker: 80 DIP tiles in 12 DIP gutters beside the 280 DIP preview panel");
    // The host rebuilds the dialog every frame, between a press and its release.
    const auto clicked = [&](UiProfileDialogAction wanted) noexcept {
        UiRect r{};
        for (u32 i = 0; i < scene.part_count; ++i)
            if (scene.parts[i].action == wanted)
                if (const auto* node = ui_widget_manager_layout_node(scene.widgets, scene.parts[i].widget))
                    r = node->rectangle;
        const float x = (r.left + r.right) * .5f, y = (r.top + r.bottom) * .5f;
        ui_button_pointer_target(&scene.widgets, UiButtonPointerEvent::press, ui_profile_dialog_hit_test(scene, x, y));
        const auto content = scene.content;
        if (!ui_profile_dialog_update(&scene, content, {0, 0, 1280, 800}))
            return false;
        const auto released = ui_button_pointer_target(&scene.widgets, UiButtonPointerEvent::release,
                                                       ui_profile_dialog_hit_test(scene, x, y));
        return released.activate && ui_profile_dialog_action(scene, released.target) == wanted;
    };
    check(clicked(UiProfileDialogAction::cancel) && clicked(UiProfileDialogAction::close) &&
              clicked(UiProfileDialogAction::item),
          "profile picker: Cancel, X and tiles complete a click across a rebuild");
    ui_profile_dialog_close(&scene);
}
} // namespace

void test_profile_editor_layout(void (*check)(bool, const char*) noexcept) noexcept {
    test_page(check);
    test_dialogs(check);
}
} // namespace maicord
