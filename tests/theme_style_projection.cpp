#include "maicord/ui/theme_style_projection.hpp"
#include "maicord/ui/text_form.hpp"
#include "maicord/ui/message_scene.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
bool measure(void*, const UiTextMeasureRequest& request, UiTextMeasureValue* value) noexcept {
    const auto validation = utf8_validate(request.text);
    if (!validation.valid || request.constraint.width <= 0)
        return false;
    const float width = static_cast<float>(validation.codepoints) * 8;
    const u32 columns = maximum(1u, static_cast<u32>(request.constraint.width / 8));
    const u32 lines = request.no_wrap ? 1u : maximum(1u, (validation.codepoints + columns - 1) / columns);
    *value = {{request.no_wrap ? width : minimum(width, request.constraint.width),
               static_cast<float>(lines) * 20},
              16,
              lines};
    return true;
}
} // namespace
void test_theme_style_projection(void (*check)(bool, const char*) noexcept) noexcept {
    UiResolvedThemeStyles bank{};
    check(ui_theme_styles_apply(&bank, {}, {}).ok(), "Theme component bank resolves defaults");
    const auto audio = ui_theme_audio_style(&bank);
    const auto poll = ui_theme_poll_style(&bank);
    const auto form = ui_theme_settings_style(&bank);
    const auto forum = ui_theme_forum_style(&bank);
    const auto editor = ui_theme_editor_style(&bank);
    check(audio.padding == 8 && audio.target == 32 && audio.title_height == 20 &&
              audio.minimum_width == 160 && audio.maximum_width == 480 && audio.corner_radius == 8 &&
              poll.padding == 16 && poll.target == 38 && poll.radius == 8 && poll.maximum_width == 472 &&
              bytes_equal(&form, &kUiDefaultSettingsStyle, sizeof(form)) && forum.maximum_width == 560 &&
              forum.inset == 16 && forum.padding == 20 && forum.gap == 10 && forum.title_height == 44 &&
              forum.body_height == 176 && editor.width == 420 && editor.height == 640 &&
              editor.padding == 20 && editor.gap == 12,
          "Theme resolved component defaults preserve native geometry exactly");
    UiResolvedThemeStyles minimum_footer = bank;
    auto minimum_metrics = bank.metrics;
    minimum_metrics.form_footer_height = 40;
    check(ui_theme_styles_apply(&minimum_footer, {}, minimum_metrics).ok() &&
              ui_theme_settings_style(&minimum_footer).footer_height == 64 &&
              ui_settings_style_valid(ui_theme_settings_style(&minimum_footer)),
          "Theme legal compact footer token retains safe native footer geometry");
    auto metrics = bank.metrics;
    metrics.audio_padding = 10;
    metrics.audio_gap = 8;
    metrics.audio_target = 36;
    metrics.audio_radius = 0;
    metrics.audio_time_width = 140;
    metrics.audio_volume_width = 72;
    metrics.poll_padding = 16;
    metrics.poll_target = 40;
    metrics.poll_marker = 24;
    metrics.form_width = 720;
    metrics.form_field_padding = 16;
    metrics.form_single_height = 48;
    metrics.form_multiline_height = 192;
    metrics.form_group_gap = 32;
    metrics.message_left = 22;
    metrics.message_right = 30;
    metrics.message_avatar = 48;
    metrics.message_avatar_gap = 18;
    metrics.message_top = 20;
    UiThemeTypography typography{};
    typography.body_size = 18;
    check(ui_theme_styles_apply(&bank, typography, metrics).ok(),
          "Theme asymmetric metrics publish atomically");
    const auto custom_audio = ui_theme_audio_style(&bank);
    UiAudioCardGeometry geometry{};
    check(custom_audio.padding == 10 && custom_audio.target == 36 && custom_audio.corner_radius == 0 &&
              ui_audio_card_geometry({0, 0, 500, 180}, custom_audio, &geometry, false) &&
              ui_rect_width(geometry.play_pause) == 36 && geometry.title.top == 10 &&
              geometry.time.right - geometry.time.left == 140,
          "Theme audio geometry and square corners consume actual resolved metrics");
    const auto custom_poll = ui_theme_poll_style(&bank);
    check(custom_poll.padding == 16 && custom_poll.target == 40 && custom_poll.marker == 24 &&
              ui_theme_forum_style(&bank).title_height == 52 &&
              ui_theme_settings_style(&bank).multiline_height == 192,
          "Theme poll and form factories share the published bank");

    UiTextMeasureRecord records[64]{};
    UiTextMeasureCache cache{};
    TextFormScene scene{};
    TextFormContent content{utf8_literal("Custom native form"),
                            utf8_literal("The owner and focus survive a style change."),
                            utf8_literal("Message"),
                            {},
                            utf8_literal("Cancel"),
                            utf8_literal("Save"),
                            utf8_literal("Draft"),
                            5,
                            5,
                            true,
                            true};
    const UiRect viewport{0, 0, 800, 600};
    bool valid = ui_text_measure_cache_initialize(&cache, {records, 64}, measure, nullptr) &&
                 text_form_update(&scene, content, viewport, &cache) &&
                 text_form_focus(&scene, TextFormElement::edit);
    const auto edit = scene.elements[static_cast<u32>(TextFormElement::edit)];
    valid = valid && text_form_bind_theme_styles(&scene, &bank) &&
            text_form_bind_measure_revision(&scene, 17) &&
            text_form_update(&scene, content, viewport, &cache);
    const auto* node = text_form_node(scene, TextFormElement::edit);
    check(valid && node && ui_rect_height(node->rectangle) == 192 && node->spec.style.padding.left == 16 &&
              text_form_focused(scene) == TextFormElement::edit &&
              scene.elements[static_cast<u32>(TextFormElement::edit)].generation == edit.generation,
          "Theme form reflows retained fields while preserving owner and focus");
    const u32 layout = scene.widgets.layout.revision, calls = cache.backend_calls,
              revision = scene.revision;
    for (u32 frame = 0; valid && frame < 64; ++frame)
        valid = !text_form_bind_theme_styles(&scene, &bank) &&
                !text_form_bind_measure_revision(&scene, 17) &&
                text_form_update(&scene, content, viewport, &cache);
    check(valid && scene.widgets.layout.revision == layout && cache.backend_calls == calls &&
              scene.revision == revision,
          "Theme no-op form publication retains 64 layout and measurement frames");
    metrics.compact_radius = 9;
    check(ui_theme_styles_apply(&bank, typography, metrics).ok() &&
              !text_form_bind_theme_styles(&scene, &bank) &&
              text_form_update(&scene, content, viewport, &cache) &&
              scene.widgets.layout.revision == layout,
          "Theme paint-only radius change retains form layout and cache");
    text_form_release(&scene);

    UiPage pages[1]{};
    UiPageRegistry registry{};
    const Utf8View choices[]{utf8_literal("Dark"), utf8_literal("Light")};
    UiSettingsControl choice{
        19, utf8_literal("Appearance"), {}, UiSettingsControlKind::choice, true, false, {choices, 2}, 0};
    UiSettingsPageContent page{utf8_literal("Style controls"), {}, {&choice, 1}, 1};
    page.style = &kUiDefaultSettingsStyle;
    UiSettingsScene settings{};
    valid = ui_page_registry_initialize(&registry, {pages, 1}) &&
            ui_page_registry_register(&registry, {{1, 4}, utf8_literal("Appearance"), &page}).ok() &&
            ui_settings_scene_update(&settings, &registry, viewport);
    UiSettingsControlGeometry previous{}, current{};
    const UiSettingsTarget target{UiSettingsTargetKind::control, {1, 4}, 19};
    valid = valid && ui_settings_scene_control_geometry(settings, target, &previous) &&
            ui_settings_scene_bind_theme_styles(&settings, &bank) &&
            ui_settings_scene_update(&settings, &registry, viewport) &&
            ui_settings_scene_control_geometry(settings, target, &current);
    check(valid && ui_rect_height(current.field) == 48 && settings.style.field_padding == 16 &&
              !ui_rect_equal(previous.field, current.field),
          "Theme settings field height uses bank and retires previous hit geometry");
    const auto settings_layout = settings.widgets.layout.revision;
    const auto settings_generation = settings.widgets.generation;
    for (u32 frame = 0; valid && frame < 64; ++frame)
        valid = !ui_settings_scene_bind_theme_styles(&settings, &bank) &&
                ui_settings_scene_update(&settings, &registry, viewport);
    check(valid && settings.widgets.layout.revision == settings_layout &&
              settings.widgets.generation == settings_generation,
          "Theme no-op settings bank retains 64 widget and layout frames");
    ui_settings_scene_release(&settings);

    MessageRecord message[1]{};
    u32 order[1]{};
    u8 text[64]{};
    MessageStore store{};
    u32 heights[1]{};
    u64 fenwick[1]{};
    UiVariableList list{};
    MessageTimeline timeline{};
    MessageScene messages{};
    MessageSceneRowWidget rows[1]{};
    UiWidget widgets[kMessageSceneWidgetsPerRow + 1]{};
    UiLayoutNode nodes[kMessageSceneWidgetsPerRow + 1]{};
    valid =
        message_store_initialize(&store, {message, 1}, {order, 1}, {text, sizeof(text)}) &&
        message_store_upsert(&store, {700, 9, 11, 0, 0, utf8_literal("Native text"), 0}).ok() &&
        ui_variable_list_initialize(&list, {heights, 1}, {fenwick, 1}, 1, ui_extent_units(100)) &&
        message_timeline_initialize(&timeline, &store, &list, ui_extent_units(100), ui_extent_units(600),
                                    false) &&
        message_scene_initialize(&messages, &timeline, {rows, 1}, {widgets, kMessageSceneWidgetsPerRow + 1},
                                 {nodes, kMessageSceneWidgetsPerRow + 1}) &&
        message_scene_update(&messages, {0, 0, 500, 600}).ok();
    valid = valid && message_scene_bind_theme_styles(&messages, &bank) &&
            message_scene_update(&messages, {0, 0, 500, 600}).ok();
    const auto* avatar = valid ? message_scene_layout_node(messages, rows[0].avatar) : nullptr;
    const auto* body = valid ? message_scene_layout_node(messages, rows[0].content) : nullptr;
    check(valid && avatar && body && ui_rect_width(avatar->rectangle) == 48 &&
              avatar->rectangle.left == 22 && body->rectangle.left == 22 + 48 + 18 &&
              body->rectangle.right == 500 - 30,
          "Theme message avatar and asymmetric padding update actual retained layout");
    const auto passes = messages.layout_passes;
    for (u32 frame = 0; valid && frame < 64; ++frame)
        valid = !message_scene_bind_theme_styles(&messages, &bank) &&
                message_scene_update(&messages, {0, 0, 500, 600}).ok();
    check(valid && messages.layout_passes == passes, "Theme no-op message bank preserves 64 layout frames");
    message_scene_release(&messages);
}
} // namespace maicord
