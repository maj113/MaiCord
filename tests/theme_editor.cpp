#include "maicord/ui/theme_editor.hpp"
#include "maicord/ui/button.hpp"
#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/widget_scroll.hpp"
namespace maicord {
void test_theme_editor(void (*check)(bool, const char*) noexcept) noexcept {
    UiThemeEditorScene scene{};
    BackgroundTheme out{};
    struct Release {
        UiThemeEditorScene& scene;
        ~Release() noexcept {
            ui_theme_editor_release(&scene);
        }
    } release{scene};
    auto draft = background_theme_defaults();
    auto invalid = draft;
    invalid.color_count = 0;
    check(!ui_theme_editor_update(&scene, invalid, 0, {0, 0, 1000, 720}) && !scene.storage.arena.base,
          "theme editor: invalid borrowed configuration cannot allocate dormant scene");
    check(ui_theme_editor_update(&scene, draft, 0, {0, 0, 1000, 720}) && ui_theme_editor_current(scene) &&
              scene.widgets.count == kUiThemeEditorElements,
          "theme editor: original right drawer uses one bounded fixed native widget bank");
    if (!ui_theme_editor_current(scene))
        return;
    const auto hex_handle = scene.elements[static_cast<u32>(UiThemeEditorElement::hex)];
    const auto* hex_widget = ui_widget_manager_resolve(scene.widgets, hex_handle);
    const bool hex_focus = ui_theme_editor_focus(&scene, UiThemeEditorElement::hex);
    const auto* hex_node = ui_theme_editor_node(scene, UiThemeEditorElement::hex);
    const auto hex_bounds =
        hex_node ? ui_rect_intersection(hex_node->rectangle, hex_node->clip_rectangle) : UiRect{};
    const UiPoint hex_point{(hex_bounds.left + hex_bounds.right) * .5f,
                            (hex_bounds.top + hex_bounds.bottom) * .5f};
    const auto hex_press = ui_button_pointer(&scene.widgets, UiButtonPointerEvent::press, hex_point);
    const auto hex_release = ui_button_pointer(&scene.widgets, UiButtonPointerEvent::release, hex_point);
    check(hex_widget && hex_widget->kind == UiWidgetKind::button && hex_focus &&
              ui_rect_has_area(hex_bounds) && hex_press.handled && !hex_press.activate &&
              hex_release.activate && hex_release.target.slot == hex_handle.slot &&
              !scene.widgets.captured.generation,
          "theme editor: hex launcher admits actual native press/release instead of masquerading as an "
          "unbound text field");
    ui_button_pointer(&scene.widgets, UiButtonPointerEvent::cancel, {});
    ui_theme_editor_focus(&scene, UiThemeEditorElement::sv);
    const auto storage = scene.storage.arena.base;
    const auto generation = scene.widgets.generation;
    const auto layout = scene.widgets.layout.revision;
    const auto lifetime = scene.generation;
    const auto* sv = ui_theme_editor_node(scene, UiThemeEditorElement::sv);
    const auto r = sv->rectangle;
    bool retained = true;
    for (u32 i = 0; i < 64; ++i) {
        ui_theme_editor_pointer(
            &scene, UiThemeEditorElement::sv,
            {r.left + ui_rect_width(r) * (.1f + i * .01f), r.top + ui_rect_height(r) * .2f}, &out);
        retained = retained && scene.storage.arena.base == storage &&
                   scene.widgets.generation == generation && scene.widgets.layout.revision == layout &&
                   scene.generation == lifetime && background_theme_valid(scene.draft);
    }
    check(retained && scene.saturation > .72f && scene.saturation < .74f && scene.value > .79f &&
              scene.value < .81f,
          "theme editor: 64 true float color drags retain layout storage handles and opening lifetime");
    check(ui_theme_editor_hsv_rgb(0, 1, 1) == 0xff0000 &&
              ui_theme_editor_hsv_rgb(1.f / 3, 1, 1) == 0x00ff00 &&
              ui_theme_editor_hsv_rgb(2.f / 3, 1, 1) == 0x0000ff &&
              ui_theme_editor_hsv_rgb(1, 0, 1) == 0xffffff,
          "theme editor: native HSV primaries hue seam and neutral white are exact");
    check(!ui_theme_editor_focus(&scene, UiThemeEditorElement::speed) &&
              !ui_theme_editor_key(&scene, UiThemeEditorElement::density, 0x23, &out),
          "theme editor effects: None keeps motion ranges disabled without false input authority");
    const auto effects_storage = scene.storage.arena.base;
    const auto effects_generation = scene.widgets.generation;
    check(ui_theme_editor_action(&scene, UiThemeEditorElement::effect_stars, 0, &out) &&
              out.effect == BackgroundEffect::stars &&
              out.strength == background_effect_settings_defaults(BackgroundEffect::stars).strength &&
              ui_theme_editor_focus(&scene, UiThemeEditorElement::strength),
          "theme editor effects: explicit Stars segment loads its retained defaults and reveals native "
          "slider");
    const auto* strength = ui_theme_editor_node(scene, UiThemeEditorElement::strength);
    bool effect_drag = strength != nullptr;
    const auto strength_rect = strength ? strength->rectangle : UiRect{};
    const auto retained_effect_layout = scene.widgets.layout.revision;
    for (u32 i = 0; i < 64 && effect_drag; ++i) {
        ui_theme_editor_pointer(&scene, UiThemeEditorElement::strength,
                                {strength_rect.left + 8 + static_cast<float>(i), strength_rect.bottom - 14},
                                &out);
        effect_drag = scene.storage.arena.base == effects_storage &&
                      scene.widgets.generation == effects_generation &&
                      scene.widgets.layout.revision == retained_effect_layout;
    }
    const u8 saved_strength = scene.draft.strength;
    check(effect_drag &&
              saved_strength != background_effect_settings_defaults(BackgroundEffect::stars).strength &&
              scene.effect_profiles[0].strength == saved_strength &&
              ui_theme_editor_action(&scene, UiThemeEditorElement::effect_comets, 0, &out) &&
              out.strength == background_effect_settings_defaults(BackgroundEffect::comets).strength &&
              ui_theme_editor_action(&scene, UiThemeEditorElement::effect_stars, 0, &out) &&
              out.strength == saved_strength,
          "theme editor effects: sixty-four exact slider previews retain storage and independent "
          "Stars/Comets profiles");
    const auto* both_node = ui_theme_editor_node(scene, UiThemeEditorElement::effect_both);
    const auto* both_speed = ui_theme_editor_node(scene, UiThemeEditorElement::speed);
    const auto both_generation = scene.widgets.generation;
    const auto both_defaults = background_effect_settings_defaults(BackgroundEffect::stars_and_comets);
    check(both_node && both_speed && both_node->rectangle.bottom < both_speed->rectangle.top &&
              ui_theme_editor_action(&scene, UiThemeEditorElement::effect_both, 0, &out) &&
              out.effect == BackgroundEffect::stars_and_comets && out.speed == both_defaults.speed &&
              out.strength == both_defaults.strength && out.density == both_defaults.density &&
              ui_theme_editor_focus(&scene, UiThemeEditorElement::effect_both) &&
              ui_theme_editor_move_focus(&scene, false) &&
              scene.widgets.focused.slot ==
                  scene.elements[static_cast<u32>(UiThemeEditorElement::speed)].slot,
          "theme editor effects: the last card has a separate row and native logical focus reaches speed");
    check(ui_theme_editor_focus(&scene, UiThemeEditorElement::strength) &&
              ui_theme_editor_key(&scene, UiThemeEditorElement::strength, 0x27, &out) &&
              out.strength == both_defaults.strength + 1 &&
              ui_theme_editor_action(&scene, UiThemeEditorElement::effect_stars, 0, &out) &&
              out.strength == saved_strength &&
              ui_theme_editor_action(&scene, UiThemeEditorElement::effect_both, 0, &out) &&
              out.strength == both_defaults.strength + 1 &&
              scene.effect_profiles[2].strength == both_defaults.strength + 1 &&
              scene.storage.arena.base == effects_storage && scene.widgets.generation == both_generation &&
              ui_theme_editor_action(&scene, UiThemeEditorElement::effect_stars, 0, &out),
          "theme editor effects: independent retained profile restores without storage or widget churn");
    // The editor exposes exactly None plus the three supported effects.
    bool catalog_elements = true;
    for (u32 i = 0; i < kUiThemeEditorElements; ++i) {
        const auto e = static_cast<UiThemeEditorElement>(i);
        const bool effect_card = e >= UiThemeEditorElement::effect_none && e <= UiThemeEditorElement::effect_both;
        catalog_elements = catalog_elements && (!effect_card || i - static_cast<u32>(UiThemeEditorElement::effect_none) <
                                                                    kBackgroundEffectCount);
    }
    check(catalog_elements && static_cast<u32>(UiThemeEditorElement::effect_both) -
                                      static_cast<u32>(UiThemeEditorElement::effect_none) + 1 ==
                                  kBackgroundEffectCount,
          "theme editor effects: card bank is exactly None, Stars, Comets and Stars and comets");
    BackgroundEffectSettings bad_profiles[kBackgroundEffectProfileCount];
    for (u32 i = 0; i < kBackgroundEffectProfileCount; ++i)
        bad_profiles[i] = scene.effect_profiles[i];
    bad_profiles[2].density = 101;
    const auto valid_before_profiles = scene.draft;
    check(!ui_theme_editor_update(&scene, scene.draft, scene.selected_stop, scene.viewport, nullptr,
                                  bad_profiles) &&
              background_theme_equal(scene.draft, valid_before_profiles),
          "theme editor effects: invalid per-effect source rejects atomically without changing current "
          "draft");
    check(ui_theme_editor_focus(&scene, UiThemeEditorElement::density) &&
              ui_theme_editor_move_focus(&scene, false) &&
              scene.widgets.focused.slot ==
                  scene.elements[static_cast<u32>(UiThemeEditorElement::surprise)].slot,
          "theme editor effects: logical Tab order reaches sticky actions after the final motion range");
    const auto scrollbar = ui_theme_editor_scrollbar(scene);
    const auto* density = ui_theme_editor_node(scene, UiThemeEditorElement::density);
    check(density && scrollbar.visible && density->rectangle.right <= scrollbar.track.left &&
              !ui_theme_editor_focus(&scene, UiThemeEditorElement::scrollbar),
          "theme editor effects: scrollbar has a separate body gutter without becoming a keyboard field");
    check(ui_theme_editor_update(&scene, draft, 0, scene.viewport) &&
              ui_theme_editor_action(&scene, UiThemeEditorElement::reset, 0, &out) &&
              scene.effect_profiles[0].strength ==
                  background_effect_settings_defaults(BackgroundEffect::stars).strength &&
              background_theme_equal(out, draft),
          "theme editor effects: Reset publishes inactive customized profiles even when visible draft is "
          "default");
    // Return to the original visible color fixture before its geometry checks.
    check(ui_theme_editor_update(&scene, draft, 0, {0, 0, 1000, 720}) &&
              ui_theme_editor_focus(&scene, UiThemeEditorElement::sv),
          "theme editor effects: color focus restores through the same scroll owner");
    const bool bar_revealed = ui_theme_editor_focus(&scene, UiThemeEditorElement::gradient);
    const auto* bar = ui_theme_editor_node(scene, UiThemeEditorElement::gradient);
    const auto bar_clip = bar ? ui_rect_intersection(bar->rectangle, bar->clip_rectangle) : UiRect{};
    const float bar_y = (bar_clip.top + bar_clip.bottom) * .5f;
    check(bar_revealed && bar && ui_rect_has_area(bar_clip) &&
              ui_theme_editor_pointer(&scene, UiThemeEditorElement::gradient,
                                      {(bar_clip.left + bar_clip.right) * .5f, bar_y}, &out) &&
              scene.selected_stop == 1 &&
              ui_theme_editor_pointer(&scene, UiThemeEditorElement::gradient, {bar_clip.right - 1, bar_y},
                                      &out) &&
              scene.selected_stop == 2,
          "theme editor: gradient bar selects the actual nearest bounded color stop");
    check(ui_theme_editor_action(&scene, UiThemeEditorElement::add, 0, &out) &&
              scene.draft.color_count == 4 &&
              ui_theme_editor_action(&scene, UiThemeEditorElement::add, 0, &out) &&
              scene.draft.color_count == 5 &&
              !ui_theme_editor_action(&scene, UiThemeEditorElement::add, 0, &out),
          "theme editor: add stops preserves bounded one-to-five color contract");
    for (u32 i = 0; i < 4; ++i)
        ui_theme_editor_action(&scene, UiThemeEditorElement::remove, 0, &out);
    check(scene.draft.color_count == 1 && !scene.draft.colors[1] && !scene.draft.colors[2] &&
              !scene.draft.colors[3] && !scene.draft.colors[4] &&
              !ui_theme_editor_action(&scene, UiThemeEditorElement::remove, 0, &out),
          "theme editor: remove stops canonicalizes unused colors and keeps one real color");
    const auto one_color = scene.draft;
    check(!ui_theme_editor_focus(&scene, UiThemeEditorElement::direction) &&
              !ui_theme_editor_key(&scene, UiThemeEditorElement::direction, 0x23, &out) &&
              !ui_theme_editor_pointer(&scene, UiThemeEditorElement::direction, {900, 600}, &out) &&
              background_theme_equal(scene.draft, one_color),
          "theme editor: solid direction rejects focus keys and pointer without mutation");
    const auto solid_layout = scene.widgets.layout.revision;
    check(ui_theme_editor_action(&scene, UiThemeEditorElement::add, 0, &out) && out.color_count == 2 &&
              out.colors[0] == one_color.colors[0] && scene.selected_stop == 1 &&
              ui_widget_manager_focus(&scene.widgets,
                                      scene.elements[static_cast<u32>(UiThemeEditorElement::direction)]) &&
              ui_theme_editor_action(&scene, UiThemeEditorElement::remove, 0, &out) &&
              out.color_count == 1 && !out.colors[1] && scene.storage.arena.base == storage &&
              scene.widgets.layout.revision == solid_layout && scene.generation == lifetime &&
              scene.widgets.focused.slot !=
                  scene.elements[static_cast<u32>(UiThemeEditorElement::direction)].slot,
          "theme editor: actual one-to-two-to-one actions preserve bank layout and retire disabled focus");
    PaintStream solid_paint{};
    bool solid_draw = paint_stream_initialize(&solid_paint, 1) &&
                      paint_stream_reset(&solid_paint, 300, 100) &&
                      paint_stream_push_palette_gradient(&solid_paint, {10, 10, 290, 90}, out, 8);
    check(solid_draw && solid_paint.count == 1 && solid_paint.quads[0].border_width == 0 &&
              solid_paint.quads[0].fill.alpha == 1 && solid_paint.quads[0].radius == 8 &&
              solid_paint.quads[0].fill.red == ((out.colors[0] >> 16) & 255) / 255.f &&
              solid_paint.quads[0].fill.green == ((out.colors[0] >> 8) & 255) / 255.f &&
              solid_paint.quads[0].fill.blue == (out.colors[0] & 255) / 255.f && !solid_paint.rejected,
          "theme editor: single palette color emits exactly one original solid rounded quad");
    paint_stream_release(&solid_paint);
    const auto old = scene.draft;
    check(ui_theme_editor_action(&scene, UiThemeEditorElement::surprise, 491, &out) &&
              background_theme_valid(out) && (out.color_count == 2 || out.color_count == 3) &&
              out.effect == old.effect && out.speed == old.speed && out.strength == old.strength &&
              out.density == old.density && out.colors[0] != out.colors[1] && out.angle_degrees % 45 == 0 &&
              out.angle_degrees < 360 && out.intensity >= 20 && out.intensity <= 80 &&
              out.intensity % 20 == 0 && scene.selected_stop < out.color_count &&
              scene.storage.arena.base == storage,
          "theme editor: shared Surprise publishes two or three harmony stops with discrete intensity "
          "direction and retained effect authority");
    check(ui_theme_editor_focus(&scene, UiThemeEditorElement::direction),
          "theme editor: randomized real gradient re-enables direction");
    const auto* direction_node = ui_theme_editor_node(scene, UiThemeEditorElement::direction);
    if (direction_node) {
        const auto direction_rectangle = direction_node->rectangle;
        const float left = direction_rectangle.left + 8, width = ui_rect_width(direction_rectangle) - 16;
        const u16 snapped = scene.draft.angle_degrees == 90 ? 180 : 90;
        check(ui_theme_editor_pointer(
                  &scene, UiThemeEditorElement::direction,
                  {left + width * ((snapped - 1.f) / 360.f), direction_rectangle.bottom - 14}, &out) &&
                  out.angle_degrees == snapped &&
                  ui_theme_editor_key(&scene, UiThemeEditorElement::direction, 0x27, &out) &&
                  out.angle_degrees == snapped + 1,
              "theme editor: pointer direction snaps near cardinal while keyboard preserves exact "
              "one-degree edit");
    } else
        check(false, "theme editor: current direction geometry exists for native pointer snap");
    check(ui_theme_editor_update(&scene, scene.draft, scene.selected_stop, {0, 0, 500, 360}) &&
              scene.generation == lifetime && scene.widgets.generation != generation &&
              ui_theme_editor_focus(&scene, UiThemeEditorElement::intensity),
          "theme editor: compact drawer scroll-reveals controls while resize invalidates capture not "
          "opening lifetime");
    const auto* intensity = ui_theme_editor_node(scene, UiThemeEditorElement::intensity);
    const auto visible = ui_rect_intersection(intensity->rectangle, intensity->clip_rectangle);
    check(ui_rect_has_area(visible) &&
              ui_theme_editor_hit_test(scene, {visible.left + 16, visible.top + 12}) ==
                  UiThemeEditorElement::intensity,
          "theme editor: native field hits use the same exact clipped scroll projection as paint");
    check(
        ui_theme_editor_key(&scene, UiThemeEditorElement::intensity, 0x23, &out) && out.intensity == 100 &&
            ui_theme_editor_key(&scene, UiThemeEditorElement::intensity, 0x24, &out) && out.intensity == 0,
        "theme editor: accessible Home End reach exact native slider endpoints");
    PaintStream paint{};
    auto paint_draft = scene.draft;
    paint_draft.color_count = 5;
    paint_draft.colors[2] = 0x16836f;
    paint_draft.colors[3] = 0x164d80;
    paint_draft.colors[4] = 0x6935b8;
    check(ui_theme_editor_update(&scene, paint_draft, 0, {0, 0, 1000, 720}) &&
              ui_theme_editor_focus(&scene, UiThemeEditorElement::sv),
          "theme editor: complete five-stop paint fixture reveals real SV surface");
    const auto* paint_sv = ui_theme_editor_node(scene, UiThemeEditorElement::sv);
    ui_theme_editor_pointer(&scene, UiThemeEditorElement::sv,
                            {paint_sv->rectangle.right, paint_sv->rectangle.top}, &out);
    check(
        paint_stream_initialize(&paint) && paint_stream_reset(&paint, 1000, 720) &&
            ui_theme_editor_paint(scene, kDefaultTheme, &paint) && paint.count < 100 && !paint.rejected,
        "theme editor: clipped SV hue and all-stop native palette remain within retained primitive budget");
    u32 marker_borders = 0;
    bool marker_inside = true;
    for (u32 i = 0; i < paint.count; ++i) {
        const auto& quad = paint.quads[i];
        if ((quad.radius == 5 && quad.border_width == 2) ||
            (quad.radius == 4 && quad.border_width == 1.5f)) {
            ++marker_borders;
            marker_inside = marker_inside && quad.left > paint_sv->rectangle.left &&
                            quad.top > paint_sv->rectangle.top && quad.right < paint_sv->rectangle.right &&
                            quad.bottom < paint_sv->rectangle.bottom;
        }
    }
    check(marker_borders == 2 && marker_inside && scene.saturation == 1 && scene.value == 1,
          "theme editor: both contrast rings stay inside SV edges while true HSV endpoints remain exact");
    paint_stream_release(&paint);
    UiTextCommand commands[24]{};
    UiTextCommandStream text{};
    UiTextOutput output{&text, nullptr};
    check(ui_text_command_stream_initialize(&text, {commands, 24}) &&
              ui_theme_editor_text_output(scene, kDefaultTheme, &output) && text.count > 5 &&
              text.count < 24 && !text.rejected,
          "theme editor: exact widget references and clipped original labels use shared text output");
    scene.light_mode = true;
    const auto primary_ink = ui_settings_button_text(ui_settings_tokens(kLightTheme),
                                                     UiSettingsButtonVariant::primary, {});
    bool light_ink = ui_theme_editor_focus(&scene, UiThemeEditorElement::light) &&
                     ui_text_command_stream_initialize(&text, {commands, 24}) &&
                     ui_theme_editor_text_output(scene, kLightTheme, &output);
    u32 white_labels = 0;
    for (u32 i = 0; light_ink && i < text.count; ++i) {
        const auto& command = text.storage[i];
        if (command.owner.handle.slot ==
                scene.elements[static_cast<u32>(UiThemeEditorElement::light)].slot ||
            command.owner.handle.slot ==
                scene.elements[static_cast<u32>(UiThemeEditorElement::apply)].slot) {
            ++white_labels;
            light_ink = command.color.red == primary_ink.red && command.color.green == primary_ink.green &&
                        command.color.blue == primary_ink.blue && command.color.alpha == primary_ink.alpha;
        }
    }
    check(light_ink && white_labels == 2,
          "theme editor: selected Light and primary Apply use the Settings primary button ink");
    Theme light_gradient = kLightTheme;
    light_gradient.background = background_theme_defaults();
    light_gradient.background.intensity = 100;
    const auto gradient_ink = ui_settings_button_text(ui_settings_tokens(light_gradient),
                                                      UiSettingsButtonVariant::primary, {});
    constexpr UiThemeEditorElement accent_targets[]{UiThemeEditorElement::light,
                                                    UiThemeEditorElement::apply};
    constexpr UiButtonPointerEvent accent_events[]{UiButtonPointerEvent::move, UiButtonPointerEvent::press};
    bool accent_contrast = paint_stream_initialize(&paint);
    for (const auto target : accent_targets) {
        accent_contrast = accent_contrast && ui_theme_editor_focus(&scene, target);
        const auto* node = ui_theme_editor_node(scene, target);
        if (!node) {
            accent_contrast = false;
            break;
        }
        const auto rect = node->rectangle;
        for (const auto event : accent_events) {
            ui_button_pointer(&scene.widgets, UiButtonPointerEvent::cancel, {});
            const auto pointer = ui_button_pointer(
                &scene.widgets, event, {(rect.left + rect.right) * .5f, (rect.top + rect.bottom) * .5f});
            accent_contrast = accent_contrast && pointer.handled && paint_stream_reset(&paint, 1000, 720) &&
                              ui_theme_editor_paint(scene, light_gradient, &paint) &&
                              ui_text_command_stream_initialize(&text, {commands, 24}) &&
                              ui_theme_editor_text_output(scene, light_gradient, &output);
            bool opaque_accent = false, white_ink = false;
            for (u32 i = 0; accent_contrast && i < paint.count; ++i) {
                const auto& quad = paint.quads[i];
                if (quad.left == rect.left && quad.right == rect.right && quad.top == rect.top &&
                    quad.bottom == rect.bottom) {
                    const auto fill = quad.fill;
                    opaque_accent =
                        opaque_accent || (fill.alpha == 1 && fill.blue > fill.red &&
                                          fill.blue > fill.green && fill.red <= light_gradient.accent.red &&
                                          fill.green <= light_gradient.accent.green &&
                                          fill.blue <= light_gradient.accent.blue);
                }
            }
            for (u32 i = 0; accent_contrast && i < text.count; ++i) {
                const auto& command = text.storage[i];
                if (command.owner.handle.slot == scene.elements[static_cast<u32>(target)].slot)
                    white_ink = command.color.red == gradient_ink.red &&
                                command.color.green == gradient_ink.green &&
                                command.color.blue == gradient_ink.blue &&
                                command.color.alpha == gradient_ink.alpha;
            }
            accent_contrast = accent_contrast && opaque_accent && white_ink;
        }
    }
    ui_button_pointer(&scene.widgets, UiButtonPointerEvent::cancel, {});
    paint_stream_release(&paint);
    check(accent_contrast, "theme editor: actual Light Apply and selected segment pointer hover and "
                           "capture keep the opaque accent and the Settings primary label ink");
    // Editor buttons resolve normal, hover and pressed exactly like the Settings button primitive.
    {
        const Theme* style_themes[]{&kDefaultTheme, &light_gradient};
        constexpr UiThemeEditorElement style_targets[]{UiThemeEditorElement::reset, UiThemeEditorElement::apply,
                                                       UiThemeEditorElement::close};
        PaintStream editor_paint{}, reference{};
        bool matches = paint_stream_initialize(&editor_paint) && paint_stream_initialize(&reference) &&
                       ui_theme_editor_update(&scene, scene.draft, scene.selected_stop, {0, 0, 1000, 720});
        for (const auto* style_theme : style_themes)
            for (const auto target : style_targets)
                for (u32 step = 0; matches && step < 3; ++step) {
                    const auto* node = ui_theme_editor_node(scene, target);
                    if (!node) {
                        matches = false;
                        break;
                    }
                    const auto rect = node->rectangle;
                    const UiPoint center{(rect.left + rect.right) * .5f, (rect.top + rect.bottom) * .5f};
                    ui_button_pointer(&scene.widgets, UiButtonPointerEvent::cancel, {});
                    if (step)
                        ui_button_pointer(&scene.widgets,
                                          step == 1 ? UiButtonPointerEvent::move : UiButtonPointerEvent::press,
                                          center);
                    auto tokens = ui_settings_tokens(*style_theme);
                    tokens.metric.control_radius = scene.style.control_radius;
                    const auto variant = target == UiThemeEditorElement::apply ? UiSettingsButtonVariant::primary
                                                                               : UiSettingsButtonVariant::secondary;
                    matches = matches && paint_stream_reset(&editor_paint, 1000, 720) &&
                              ui_theme_editor_paint(scene, *style_theme, &editor_paint) &&
                              paint_stream_reset(&reference, 1000, 720) &&
                              ui_settings_button_paint(&reference, tokens, rect, variant,
                                                       {true, step != 0, step == 2, false}) &&
                              reference.count == 1;
                    bool found = false;
                    for (u32 i = 0; matches && i < editor_paint.count; ++i) {
                        const auto& quad = editor_paint.quads[i];
                        if (quad.left == rect.left && quad.top == rect.top && quad.right == rect.right &&
                            quad.bottom == rect.bottom)
                            found = found || bytes_equal(&quad, &reference.quads[0], sizeof(PaintQuad));
                    }
                    matches = matches && found;
                }
        ui_button_pointer(&scene.widgets, UiButtonPointerEvent::cancel, {});
        paint_stream_release(&editor_paint);
        paint_stream_release(&reference);
        check(matches, "theme editor: Reset, Apply and close paint the Settings button fill, border, radius "
                       "and normal/hover/pressed states in themed and gradient themes");
    }
    const auto old_handle = scene.elements[static_cast<u32>(UiThemeEditorElement::hex)];
    ui_theme_editor_close(&scene);
    check(!ui_theme_editor_current(scene) && !ui_widget_manager_resolve(scene.widgets, old_handle) &&
              ui_theme_editor_hit_test(scene, {490, 20}) == UiThemeEditorElement::count,
          "theme editor: closing invalidates native input text owner and stale hits before release");
}
} // namespace maicord
