#pragma once

#include "ui_surface_motion_measurement.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/ui/text_form.hpp"

namespace maicord {
namespace channel_overview_probe_detail {

inline UiPoint center(UiRect r) noexcept {
    return {(r.left + r.right) * .5f, (r.top + r.bottom) * .5f};
}
inline bool ink(const surface_motion_probe_detail::Readback& rb, u32 image, UiRect r, float scale,
                Color color, u32 minimum_pixels = 8) noexcept {
    const auto clip = raster_clip_rectangle(r, scale, rb.width, rb.height);
    const u8 expected[]{static_cast<u8>(color.blue * 255.f + .5f),
                        static_cast<u8>(color.green * 255.f + .5f),
                        static_cast<u8>(color.red * 255.f + .5f)};
    u32 pixels = 0;
    for (i32 y = maximum(clip.top, 0); y < clip.bottom; ++y)
        for (i32 x = maximum(clip.left, 0); x < clip.right; ++x) {
            const auto* p = rb.images + image * rb.bytes + (y * rb.width + x) * 4;
            if (surface_motion_probe_detail::distance(p, expected) <= 18 && ++pixels >= minimum_pixels)
                return true;
        }
    return false;
}
inline bool glyphs_in(const D3D11Renderer& r, u32 first, u32 end, UiRect area) noexcept {
    if (!r.content_text || first >= end || end > r.content_text->batch_count)
        return false;
    for (u32 i = first; i < end; ++i) {
        const auto& batch = r.content_text->batches[i];
        for (u32 j = 0; j < batch.quad_count; ++j) {
            const auto& q = r.content_text->quads[batch.first_quad + j];
            if (ui_rect_has_area(ui_rect_intersection(
                    ui_rect_intersection({q.left, q.top, q.right, q.bottom}, batch.clip), area)))
                return true;
        }
    }
    return false;
}

} // namespace channel_overview_probe_detail

// Original, offline models only. This exercises the same scene, shared native
// editor bank, glyph renderer and modal surface used by the production host.
inline bool channel_overview_ui_probe(D3D11Renderer& renderer, UiTextMeasureCache& cache) noexcept {
    using namespace channel_overview_probe_detail;
    using surface_motion_probe_detail::Readback;
    u32 stage = 1;
    const auto fail = [&]() noexcept {
        log_u64(LogLevel::error, "probe", "channel_overview_failure_stage", stage);
        return false;
    };
    if (!renderer.content_text || !renderer.glyph_text.ready)
        return fail();
    UiSettingsScene scene{};
    UiPageRegistry registry{};
    UiPage page{};
    TextFormScene form{};
    TextFormSurface surface{};
    const auto* old_settings = renderer.settings_scene;
    const auto* old_popup = renderer.popup_scene;
    const auto* old_focus = renderer.focus_owner;
    auto* old_form = renderer.text_form;
    struct Restore {
        D3D11Renderer& renderer;
        UiSettingsScene& scene;
        TextFormScene& form;
        TextFormSurface& surface;
        const UiSettingsScene* settings;
        const PopupScene* popup;
        const UiWidgetManager* focus;
        TextFormSurface* previous_form;
        ~Restore() noexcept {
            renderer_bind_text_form(&renderer, previous_form);
            renderer_bind_settings_scene(&renderer, settings);
            renderer_bind_popup_scene(&renderer, popup);
            renderer_bind_focus_owner(&renderer, focus);
            renderer.retained_presentation.valid = false;
            text_edit_layout_invalidate(&renderer.media_picker_layout);
            renderer.media_picker_edit_geometry = {};
            text_form_surface_release(&surface);
            text_form_release(&form);
            ui_settings_scene_release(&scene);
        }
    } restore{renderer, scene, form, surface, old_settings, old_popup, old_focus, old_form};
    renderer_bind_text_form(&renderer, nullptr);
    renderer_bind_popup_scene(&renderer, nullptr);
    const float scale = static_cast<float>(renderer.dpi) / 96.f;
    const UiRect viewport{0, 0, renderer.width / scale, renderer.height / scale};
    UiSettingsEditor name{
        {utf8_literal("native-garden"), 0, 0, true, false, false}, nullptr, utf8_literal("Channel name")};
    UiSettingsEditor topic{{utf8_literal("An original space for sketches and small discoveries.\n"
                                         "Keep notes kind, clear, and easy to find."),
                            0, 0, true, false, false},
                           nullptr,
                           utf8_literal("Add a topic")};
    const Utf8View choices[]{utf8_literal("Off"), utf8_literal("5 seconds"), utf8_literal("30 seconds")};
    UiSettingsControl controls[]{
        {1,
         utf8_literal("Channel name"),
         utf8_literal("Give this space a clear, memorable name."),
         UiSettingsControlKind::edit,
         true,
         false,
         {},
         0,
         {},
         &name},
        {2,
         utf8_literal("Topic"),
         utf8_literal("Describe what belongs here. Your draft stays local until saved."),
         UiSettingsControlKind::multiline,
         true,
         false,
         {},
         0,
         {},
         &topic},
        {3,
         utf8_literal("Slowmode"),
         utf8_literal("Choose the interval between messages."),
         UiSettingsControlKind::choice,
         true,
         false,
         {choices, 3},
         1,
         {},
         nullptr},
        {4,
         utf8_literal("Age-restricted channel"),
         utf8_literal("Require an age check before entering."),
         UiSettingsControlKind::toggle,
         true,
         false,
         {},
         0,
         {},
         nullptr}};
    UiSettingsStyle style = kUiDefaultSettingsStyle;
    UiSettingsFooter footer{utf8_literal("No unsaved changes"),
                            utf8_literal("Save changes"),
                            utf8_literal("Reset"),
                            true,
                            false,
                            false,
                            false};
    UiSettingsPageContent content{utf8_literal("Overview"),
                                  utf8_literal("Channel settings / original offline fixture"),
                                  {controls, 4},
                                  1,
                                  kUiSettingsPageFormat,
                                  &style,
                                  &footer};
    content.navigation_label = utf8_literal("#native-garden");
    const UiPageKey key{0xc071, 1};
    const UiSettingsTarget name_target{UiSettingsTargetKind::control, key, 1};
    const UiSettingsTarget topic_target{UiSettingsTargetKind::control, key, 2};
    const UiSettingsTarget save_target{UiSettingsTargetKind::save, key, 0};
    const UiSettingsTarget reset_target{UiSettingsTargetKind::reset, key, 0};
    if (!ui_page_registry_initialize(&registry, {&page, 1}) ||
        !ui_page_registry_register(&registry, {key, utf8_literal("Overview"), &content}).ok() ||
        !ui_settings_scene_update(&scene, &registry, viewport))
        return fail();
    renderer_bind_settings_scene(&renderer, &scene);
    renderer_bind_focus_owner(&renderer, &scene.widgets);
    UiSettingsControlGeometry name_geometry{}, topic_geometry{}, hit{};
    UiSettingsActivation activation{};
    if (!ui_settings_scene_control_geometry(scene, name_target, &name_geometry) ||
        !ui_settings_scene_control_geometry(scene, topic_target, &topic_geometry) ||
        ui_rect_height(name_geometry.field) != 40 || ui_rect_height(topic_geometry.field) != 160 ||
        ui_rect_width(name_geometry.field) > 680 ||
        name_geometry.text.left != name_geometry.field.left + 12 ||
        ui_settings_scene_activation(scene, save_target, false, &activation))
        return fail();
    const auto point = center(topic_geometry.clip);
    const auto scrollbar = ui_settings_scene_scrollbar(scene, false);
    const auto* body = ui_widget_manager_layout_node(scene.widgets, scene.body);
    const auto* status = ui_widget_manager_layout_node(scene.widgets, scene.footer_status);
    const auto* footer_box =
        status ? ui_widget_manager_layout_node(scene.widgets,
                                               scene.widgets.storage[scene.footer_status.slot].parent)
               : nullptr;
    const auto* save = ui_widget_manager_layout_node(scene.widgets, scene.footer_save);
    // Fields and the footer bar share the form column; the scrolling body
    // starts at that column and runs to the panel edge, so its scrollbar lane
    // lies right of every field.
    if (!body || !status || !footer_box || !save ||
        body->rectangle.left != topic_geometry.field.left ||
        body->rectangle.right != scene.layout.panel.right - kUiSettingsBodyEdgeInset ||
        body->rectangle.right <= topic_geometry.field.right ||
        footer_box->rectangle.left != topic_geometry.field.left ||
        footer_box->rectangle.right != topic_geometry.field.right ||
        status->rectangle.left != footer_box->rectangle.left + kUiSettingsFooterLeadingInset ||
        save->rectangle.right != footer_box->rectangle.right - kUiSettingsFooterInset ||
        (scrollbar.visible &&
         (scrollbar.track.left < topic_geometry.field.right ||
          ui_settings_scene_editor_hit(scene, scrollbar.track.left + 1, point.y, &hit))))
        return fail();
    if (!ui_settings_scene_editor_hit(scene, point.x, point.y, &hit) ||
        !ui_settings_target_equal(hit.target, topic_target) ||
        ui_settings_scene_editor_hit(scene, point.x, topic_geometry.field.top - 1, &hit) ||
        ui_settings_scene_editor_hit(scene, topic_geometry.field.right, point.y, &hit))
        return fail();
    const auto allocation = scene.storage.arena.base;
    u16 generation = scene.widgets.generation;
    u32 layout_revision = scene.widgets.layout.revision;
    const auto stable_layout = [&]() noexcept {
        return scene.storage.arena.base == allocation && scene.widgets.generation == generation &&
               scene.widgets.layout.revision == layout_revision;
    };
    // Enabled state is part of the layout signature (a disabled row is not a
    // focusable widget), so toggling every control rebuilds the widget tree;
    // it must stay in the scene's retained storage. Later steps compare
    // against the rebuilt tree.
    const auto rebuilt_in_place = [&]() noexcept {
        if (scene.storage.arena.base != allocation)
            return false;
        generation = scene.widgets.generation;
        layout_revision = scene.widgets.layout.revision;
        return true;
    };
    Readback readback;
    if (!readback.initialize(renderer))
        return fail();
    const auto render = [&](u32 image, const wchar_t* filename) noexcept {
        if (!renderer_rebuild_content_text(&renderer, kDefaultTheme) ||
            !probe_present(&renderer, kDefaultTheme) || !probe_present(&renderer, kDefaultTheme))
            return false;
        const auto retained = surface_motion_probe_detail::snapshot(renderer);
        const u32 editor_builds = renderer.media_picker_layout.builds;
        if (!surface_motion_probe_detail::draw_fixed(renderer) || !readback.read(renderer, image) ||
            !surface_motion_probe_detail::same(retained, surface_motion_probe_detail::snapshot(renderer)) ||
            renderer.media_picker_layout.builds != editor_builds)
            return false;
        return !filename || readback.capture(image, filename);
    };
    stage = 2;
    if (!render(0, L"channel-overview-default.bmp") || !stable_layout() ||
        !ink(readback, 0, name_geometry.text, scale, kDefaultTheme.text_primary))
        return fail();

    // Caret at the last line makes the real native edit owner scroll a tall
    // topic into its 136-DIP text viewport; selection is a logical byte range.
    const auto long_topic =
        utf8_literal("Garden notes, line 01\nGarden notes, line 02\nGarden notes, line 03\n"
                     "Garden notes, line 04\nGarden notes, line 05\nGarden notes, line 06\n"
                     "Garden notes, line 07\nGarden notes, line 08\nGarden notes, line 09\n"
                     "Garden notes, line 10\nGarden notes, line 11\nGarden notes, line 12\n"
                     "Garden notes, line 13\nGarden notes, line 14\nGarden notes, line 15\n"
                     "Garden notes, line 16\nGarden notes, line 17\nGarden notes, line 18\n"
                     "Select this final line");
    stage = 3;
    topic.view = {long_topic, long_topic.bytes - 22, long_topic.bytes, true, true, true};
    topic.geometry = &renderer.media_picker_edit_geometry;
    footer.status = utf8_literal("Unsaved changes");
    footer.can_save = true;
    ++content.revision;
    if (!ui_settings_scene_update(&scene, &registry, viewport) ||
        !ui_settings_scene_focus(&scene, topic_target))
        return fail();
    ui_settings_scene_set_focus_visible(&scene, true);
    if (!render(1, L"channel-overview-topic-selection.bmp") || !stable_layout() ||
        !renderer.media_picker_layout.active || renderer.media_picker_layout.scroll_y <= 0 ||
        !renderer.media_picker_edit_geometry.selection_count ||
        !ui_settings_scene_control_geometry(scene, topic_target, &topic_geometry) ||
        !ui_settings_scene_activation(scene, save_target, false, &activation))
        return fail();
    const auto& edit = renderer.media_picker_edit_geometry;
    bool selection = false;
    bool selection_pixels = false;
    const Color selection_color{kDefaultTheme.accent.red * .38f + kDefaultTheme.base_lower.red * .62f,
                                kDefaultTheme.accent.green * .38f + kDefaultTheme.base_lower.green * .62f,
                                kDefaultTheme.accent.blue * .38f + kDefaultTheme.base_lower.blue * .62f, 1};
    for (u32 i = 0; i < renderer.paint_stream.count; ++i) {
        const auto& q = renderer.paint_stream.quads[i];
        if (q.fill.alpha == .38f && q.fill.red == kDefaultTheme.accent.red) {
            if (q.left < topic_geometry.text.left || q.top < topic_geometry.text.top ||
                q.right > topic_geometry.text.right || q.bottom > topic_geometry.text.bottom)
                return fail();
            selection = true;
            selection_pixels = selection_pixels ||
                               ink(readback, 1, {q.left, q.top, q.right, q.bottom}, scale, selection_color);
        }
    }
    u32 caret_position = 0;
    const auto caret = center(ui_rect_intersection(edit.caret, topic_geometry.text));
    if (!selection || !selection_pixels || edit.text.top >= topic_geometry.text.top ||
        !text_edit_layout_hit_test(renderer.media_picker_layout, caret.x, caret.y, &caret_position) ||
        caret_position != long_topic.bytes ||
        !ink(readback, 1, topic_geometry.text, scale, kDefaultTheme.text_primary))
        return fail();

    // Pending rejects both actions and revokes editor focus. Failure restores
    // manual retry authority without changing the draft or reallocating the scene.
    stage = 4;
    footer.saving = true;
    footer.status = utf8_literal("Saving…");
    topic.view.focused = topic.view.caret_visible = false;
    topic.geometry = nullptr;
    for (auto& control : controls)
        control.enabled = false;
    ++content.revision;
    if (!ui_settings_scene_update(&scene, &registry, viewport) || !rebuilt_in_place() ||
        ui_settings_scene_activation(scene, save_target, false, &activation) ||
        ui_settings_scene_activation(scene, reset_target, false, &activation) ||
        ui_settings_scene_focused(scene).kind != UiSettingsTargetKind::close || !render(2, nullptr) ||
        renderer.media_picker_layout.active)
        return fail();
    // Footer colors come from the settings tokens: a primary (brand) save
    // button, strong status text, and critical text for a failed save.
    const auto settings_colors = ui_settings_tokens(kDefaultTheme).color;
    const auto* save_node = ui_widget_manager_layout_node(scene.widgets, scene.footer_save);
    const auto* footer_node = ui_widget_manager_layout_node(scene.widgets, scene.footer_status);
    if (!save_node || !footer_node ||
        ink(readback, 2, save_node->rectangle, scale, settings_colors.brand, 24) ||
        !ink(readback, 2, footer_node->content_rectangle, scale, settings_colors.text_strong))
        return fail();
    stage = 5;
    footer.saving = false;
    footer.failed = true;
    footer.status = utf8_literal("Could not save. Your draft is still here. Try again.");
    for (auto& control : controls)
        control.enabled = true;
    ++content.revision;
    if (!ui_settings_scene_update(&scene, &registry, viewport) || !rebuilt_in_place() ||
        !utf8_equal(topic.view.text, long_topic) ||
        !ui_settings_scene_activation(scene, save_target, false, &activation) ||
        !render(2, L"channel-overview-failed-footer.bmp"))
        return fail();
    save_node = ui_widget_manager_layout_node(scene.widgets, scene.footer_save);
    footer_node = ui_widget_manager_layout_node(scene.widgets, scene.footer_status);
    if (!save_node || !footer_node ||
        !ink(readback, 2, footer_node->content_rectangle, scale, settings_colors.critical_text) ||
        !ink(readback, 2, save_node->rectangle, scale, settings_colors.brand, 24))
        return fail();

    stage = 6;
    TextFormContent confirmation{};
    confirmation.title = utf8_literal("Discard unsaved changes?");
    confirmation.context =
        utf8_literal("Your channel name and topic draft have not been saved. "
                     "Keep editing to preserve this original draft, or discard it to leave Overview.");
    confirmation.cancel = utf8_literal("Keep editing");
    confirmation.accept = utf8_literal("Discard changes");
    confirmation.can_accept = confirmation.focus_visible = confirmation.confirmation_only = true;
    if (!text_form_update(&form, confirmation, viewport, &cache) ||
        !text_form_focus(&form, TextFormElement::cancel))
        return fail();
    surface.scene = &form;
    renderer_bind_text_form(&renderer, &surface);
    renderer_bind_focus_owner(&renderer, &form.widgets);
    if (!render(3, L"channel-overview-discard-confirmation.bmp") || !stable_layout() ||
        surface.layout.active || renderer.media_picker_layout.active ||
        renderer.focus_owner != &form.widgets || text_form_focused(form) != TextFormElement::cancel ||
        renderer.form_paint_first_batch == 0 ||
        renderer.form_paint_first_batch >= renderer.paint_stream.batch_count ||
        renderer.form_text_first_batch == 0 ||
        renderer.form_text_first_batch >= renderer.content_text->batch_count ||
        renderer.settings_choice_text_end_batch != renderer.form_text_first_batch)
        return fail();
    const TextFormElement text_elements[]{TextFormElement::title, TextFormElement::context,
                                          TextFormElement::cancel, TextFormElement::accept};
    for (auto element : text_elements) {
        const auto* node = text_form_node(form, element);
        if (!node ||
            !glyphs_in(renderer, renderer.form_text_first_batch, renderer.content_text->batch_count,
                       node->content_rectangle) ||
            !ink(readback, 3, node->content_rectangle, scale, kDefaultTheme.text_primary))
            return fail();
    }
    const auto* cancel = text_form_node(form, TextFormElement::cancel);
    const auto button = center(cancel->rectangle);
    if (text_form_hit_test(form, button.x, button.y) != TextFormElement::cancel ||
        !utf8_equal(topic.view.text, long_topic))
        return fail();
    log_write(LogLevel::info, "probe", "channel_overview_native_editor_footer_modal_retention_passed");
    return true;
}
} // namespace maicord
