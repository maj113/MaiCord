#include "maicord/ui/text_form.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
namespace {
bool measure(void*, const UiTextMeasureRequest& request, UiTextMeasureValue* value) noexcept {
    const auto validation = utf8_validate(request.text);
    if (!validation.valid || request.constraint.width <= 0)
        return false;
    const float natural = static_cast<float>(validation.codepoints) * 8;
    const u32 columns = maximum(1u, static_cast<u32>(request.constraint.width / 8));
    const u32 lines = request.no_wrap ? 1u : maximum(1u, (validation.codepoints + columns - 1) / columns);
    *value = {{request.no_wrap ? natural : minimum(natural, request.constraint.width),
               static_cast<float>(lines) * 20},
              16,
              lines};
    return true;
}
} // namespace
void test_text_form(void (*check)(bool, const char*) noexcept) noexcept {
    TextFormScene scene{};
    UiTextMeasureRecord records[32]{};
    UiTextMeasureCache cache{};
    check(ui_text_measure_cache_initialize(&cache, {records, 32}, measure, nullptr),
          "text_form_measure_initialize");
    TextFormContent content{utf8_literal("Description"),
                            utf8_literal("image.png"),
                            utf8_literal("Describe this attachment"),
                            {},
                            utf8_literal("Cancel"),
                            utf8_literal("Save"),
                            utf8_literal("A\r\n\xF0\x9F\x98\x80"),
                            0,
                            5,
                            true,
                            true};
    const UiRect viewport{100, 50, 900, 750};
    check(text_form_update(&scene, content, viewport, &cache) && scene.active &&
              text_form_focused(scene) == TextFormElement::edit,
          "text_form_open_focus");
    if (!scene.active) {
        text_form_release(&scene);
        return;
    }
    UiLayoutDiagnostic diagnostics[32]{};
    check(ui_layout_engine_validate(scene.widgets.layout, {diagnostics, 32}).clean(),
          "text_form_layout_clean");
    {
        // Modal footer: actions sit at the trailing edge, Cancel before Accept,
        // separated from the body by more than the ordinary field gap.
        const auto* panel = text_form_node(scene, TextFormElement::panel);
        const auto* edit = text_form_node(scene, TextFormElement::edit);
        const auto* status = text_form_node(scene, TextFormElement::status);
        const auto* cancel = text_form_node(scene, TextFormElement::cancel);
        const auto* accept = text_form_node(scene, TextFormElement::accept);
        check(panel && edit && status && cancel && accept &&
                  accept->rectangle.right > panel->content_rectangle.right - .5f &&
                  accept->rectangle.right < panel->content_rectangle.right + .5f &&
                  cancel->rectangle.right <= accept->rectangle.left &&
                  cancel->rectangle.left > panel->content_rectangle.left &&
                  cancel->rectangle.top - edit->rectangle.bottom > panel->spec.style.gap,
              "text_form_actions_trailing_footer_with_separated_rhythm");
    }
    const auto generation = scene.widgets.generation;
    const auto layout_revision = scene.widgets.layout.revision;
    const auto backend_calls = cache.backend_calls;
    const auto storage = scene.storage.arena.base;
    content.anchor_utf16 = content.caret_utf16;
    check(text_form_update(&scene, content, viewport, &cache) && scene.widgets.generation == generation &&
              scene.widgets.layout.revision == layout_revision && cache.backend_calls == backend_calls &&
              scene.storage.arena.base == storage,
          "text_form_selection_update_reuses_layout_and_storage");
    check(text_form_move_focus(&scene, false) && text_form_focused(scene) == TextFormElement::cancel &&
              text_form_move_focus(&scene, false) && text_form_focused(scene) == TextFormElement::accept &&
              text_form_move_focus(&scene, false) && text_form_focused(scene) == TextFormElement::edit &&
              text_form_move_focus(&scene, true) && text_form_focused(scene) == TextFormElement::accept,
          "text_form_tab_wrap_both_directions");
    content.can_accept = false;
    check(text_form_update(&scene, content, viewport, &cache) &&
              text_form_focused(scene) == TextFormElement::edit &&
              !text_form_focus(&scene, TextFormElement::accept),
          "text_form_disabled_accept_reconciles_focus");
    content.editable = false;
    content.status = utf8_literal("Creating thread…");
    check(text_form_update(&scene, content, viewport, &cache) &&
              text_form_focused(scene) == TextFormElement::cancel &&
              !text_form_focus(&scene, TextFormElement::edit) &&
              !text_form_focus(&scene, TextFormElement::accept) &&
              utf8_equal(scene.content.text, content.text) && scene.widgets.generation == generation &&
              scene.storage.arena.base == storage,
          "text_form_pending_owner_retains_name_and_cancel_without_edit_accept_or_widget_rebuild");
    const auto pending_revision = scene.revision, pending_layout = scene.widgets.layout.revision;
    check(text_form_update(&scene, content, viewport, &cache) && scene.revision == pending_revision &&
              scene.widgets.layout.revision == pending_layout,
          "text_form_unchanged_pending_projection_reuses_layout_and_revision");
    const auto cancel_handle = scene.elements[static_cast<u32>(TextFormElement::cancel)];
    const auto* cancel_node = text_form_node(scene, TextFormElement::cancel);
    const UiRect cancel_rectangle = cancel_node ? cancel_node->rectangle : UiRect{};
    check(ui_widget_manager_capture(&scene.widgets, cancel_handle),
          "text_form_cancel_pointer_capture_fixture");
    content.can_cancel = false;
    check(text_form_update(&scene, content, viewport, &cache) && scene.active &&
              text_form_focused(scene) == TextFormElement::count && !scene.widgets.captured.generation &&
              !text_form_focus(&scene, TextFormElement::cancel) &&
              !ui_widget_manager_input_enabled(scene.widgets, cancel_handle) &&
              text_form_hit_test(scene, (cancel_rectangle.left + cancel_rectangle.right) * .5f,
                                 (cancel_rectangle.top + cancel_rectangle.bottom) * .5f) ==
                  TextFormElement::count &&
              scene.widgets.layout.revision == pending_layout && scene.widgets.generation == generation &&
              scene.storage.arena.base == storage,
          "text_form_committed_pending_disables_cancel_focus_hit_capture_without_layout_or_widget_rebuild");
    const Theme cancel_themes[]{kDefaultTheme, kLightTheme};
    for (const auto& theme : cancel_themes) {
        UiTextCommand commands[16]{};
        UiTextCommandStream text{};
        UiTextOutput text_output{&text, nullptr};
        bool disabled_cancel = false;
        const auto* field = text_form_node(scene, TextFormElement::edit);
        if (field && ui_text_command_stream_initialize(&text, {commands, 16}) &&
            text_form_text_output(scene, {field->content_rectangle, {}, {}, {}}, theme, &text_output)) {
            for (u32 i = 0; i < text.count; ++i) {
                const auto& command = text.storage[i];
                if (command.owner.handle.slot == cancel_handle.slot)
                    // Disabled actions keep their fill-derived ink at half opacity.
                    disabled_cancel = command.color.alpha > 0 && command.color.alpha <= .5f;
            }
        }
        check(disabled_cancel, "text_form_disabled_cancel_uses_existing_disabled_action_ink_dark_light");
    }
    const auto locked_revision = scene.revision, locked_measurements = cache.backend_calls;
    bool locked_stable = true;
    for (u32 frame = 0; frame < 64; ++frame)
        locked_stable &=
            text_form_update(&scene, content, viewport, &cache) && scene.revision == locked_revision &&
            scene.widgets.layout.revision == pending_layout && cache.backend_calls == locked_measurements &&
            text_form_focused(scene) == TextFormElement::count;
    check(locked_stable, "text_form_locked_64_updates_reuse_geometry_measurements_and_disabled_focus");
    content.can_accept = true;
    check(text_form_update(&scene, content, viewport, &cache) &&
              text_form_focused(scene) == TextFormElement::accept && text_form_move_focus(&scene, false) &&
              text_form_focused(scene) == TextFormElement::accept && text_form_move_focus(&scene, true) &&
              text_form_focused(scene) == TextFormElement::accept &&
              !text_form_focus(&scene, TextFormElement::cancel),
          "text_form_retry_only_reconciles_focus_and_tab_without_reenabling_cancel");
    content.can_cancel = true;
    content.editable = true;
    content.status = {};
    check(text_form_update(&scene, content, viewport, &cache) &&
              text_form_focus(&scene, TextFormElement::edit),
          "text_form_explicit_editing_recovery_reenables_same_name_field");
    const auto previous_revision = scene.revision;
    content.caret_utf16 = 4;
    check(!text_form_update(&scene, content, viewport, &cache) && scene.revision == previous_revision &&
              scene.active,
          "text_form_rejects_split_surrogate_projection_atomically");
    content.caret_utf16 = 5;
    content.can_accept = true;
    content.title = utf8_literal("A longer translated title for the native text form");
    content.accept = utf8_literal("Save the attachment description");
    content.cancel = utf8_literal("Cancel and return to the attachment");
    const UiRect sizes[] = {{0, 0, 320, 400}, {13, 27, 173, 227}, {0, 0, 96, 64}};
    for (const auto size : sizes) {
        const bool updated = text_form_update(&scene, content, size, &cache);
        check(updated, "text_form_small_viewport_update");
        if (!updated)
            continue;
        check(ui_layout_engine_validate(scene.widgets.layout, {diagnostics, 32}).clean(),
              "text_form_small_viewport_layout_clean");
        check(text_form_focus(&scene, TextFormElement::accept),
              "text_form_reveal_accept_in_short_viewport");
        UiRect anchor{};
        check(ui_widget_manager_context_anchor(scene.widgets, &anchor) &&
                  text_form_hit_test(scene, (anchor.left + anchor.right) * 0.5f,
                                     (anchor.top + anchor.bottom) * 0.5f) == TextFormElement::accept,
              "text_form_revealed_accept_hit_test");
    }
    const auto prior_text = content.text;
    content.text = utf8_literal("日本語😀");
    content.anchor_utf16 = content.caret_utf16 = 5;
    content.single_line = true;
    // The preceding tiny-viewport cases intentionally reveal the footer.
    // Reveal the name field before asserting its visible glyph projection.
    check(text_form_update(&scene, content, viewport, &cache) &&
              text_form_focus(&scene, TextFormElement::edit),
          "text_form_single_line_name_update");
    const auto* name_field = text_form_node(scene, TextFormElement::edit);
    UiTextCommand name_commands[16]{};
    UiTextCommandStream name_text{};
    UiTextOutput name_text_output{&name_text, nullptr};
    bool name_no_wrap = false;
    if (name_field && ui_text_command_stream_initialize(&name_text, {name_commands, 16}) &&
        text_form_text_output(scene, {name_field->content_rectangle, {}, {}, {}}, kDefaultTheme, &name_text_output)) {
        for (u32 i = 0; i < name_text.count; ++i)
            if (name_text.storage[i].owner.handle.slot ==
                scene.elements[static_cast<u32>(TextFormElement::edit)].slot)
                name_no_wrap = name_text.storage[i].wrapping == UiTextWrapping::no_wrap;
    }
    check(name_field && ui_rect_height(name_field->rectangle) == 44.f && name_no_wrap &&
              ui_layout_engine_validate(scene.widgets.layout, {diagnostics, 32}).clean(),
          "text_form_single_line_unicode_name_has_native44dip_field_and_no_wrap_text");
    content.single_line = false;
    content.text = prior_text;
    check(text_form_update(&scene, content, viewport, &cache) && scene.storage.arena.base == storage,
          "text_form_update_reuses_storage");
    text_form_release(&scene);
    check(!scene.storage.arena.base && !scene.active && !scene.widgets.count, "text_form_release");

    // The narrow native probe is 960 physical pixels at 144 DPI: 640 DIPs.
    constexpr float narrow_width = 960.f * 96.f / 144.f;
    constexpr float narrow_height = 900.f * 96.f / 144.f;
    const UiRect action_viewports[] = {
        {0, 0, 800, 700}, {0, 0, narrow_width, narrow_height}, {0, 0, 320, 500}};
    const Theme action_themes[] = {kDefaultTheme, kLightTheme};
    TextFormScene actions{};
    TextFormContent name{};
    name.title = utf8_literal("Create thread");
    name.label = utf8_literal("Thread name");
    name.cancel = utf8_literal("Cancel");
    name.text = utf8_literal("Release 日本語🚀");
    name.single_line = true;
    for (const auto action_viewport : action_viewports) {
        name.accept = utf8_literal("Create");
        name.status = {};
        name.can_accept = name.editable = true;
        const bool initial = text_form_update(&actions, name, action_viewport, &cache);
        check(initial, "text_form_dynamic_action_initial_create");
        if (!initial)
            continue;
        const float create_width =
            ui_rect_width(text_form_node(actions, TextFormElement::accept)->rectangle);
        name.accept = utf8_literal("Check again");
        name.status = utf8_literal("Checking for an existing thread…");
        name.can_accept = name.editable = false;
        const bool updated = text_form_update(&actions, name, action_viewport, &cache);
        check(updated, "text_form_dynamic_disabled_check_thread_update");
        if (!updated)
            continue;
        const auto* accept = text_form_node(actions, TextFormElement::accept);
        const auto* cancel = text_form_node(actions, TextFormElement::cancel);
        UiTextMeasureValue measured{};
        const bool measured_label =
            measure(nullptr,
                    {{1, 1, 1, 1, 1},
                     name.accept,
                     {},
                     {ui_rect_width(accept->content_rectangle), ui_rect_height(accept->content_rectangle)},
                     true},
                    &measured);
        check(measured_label && measured.line_count == 1 &&
                  measured.size.width <= ui_rect_width(accept->content_rectangle) &&
                  measured.size.height <= ui_rect_height(accept->content_rectangle) &&
                  ui_rect_width(accept->rectangle) > create_width &&
                  ui_rect_height(accept->rectangle) == 36 && ui_rect_height(cancel->rectangle) == 36 &&
                  accept->rectangle.top == cancel->rectangle.top &&
                  ui_layout_engine_validate(actions.widgets.layout, {diagnostics, 32}).clean() &&
                  !text_form_focus(&actions, TextFormElement::accept),
              "text_form_dynamic_check_thread_fits_one_line_and_equal36dip_actions_including640dip144dpi");
        for (const auto& theme : action_themes) {
            UiTextCommand commands[16]{};
            UiTextCommandStream text{};
            UiTextOutput text_output{&text, nullptr};
            bool no_wrap_disabled = false;
            const auto* edit = text_form_node(actions, TextFormElement::edit);
            if (ui_text_command_stream_initialize(&text, {commands, 16}) &&
                text_form_text_output(actions, {edit->content_rectangle, {}, {}, {}}, theme, &text_output)) {
                for (u32 i = 0; i < text.count; ++i) {
                    const auto& command = text.storage[i];
                    if (command.owner.handle.slot ==
                        actions.elements[static_cast<u32>(TextFormElement::accept)].slot)
                        no_wrap_disabled = command.wrapping == UiTextWrapping::no_wrap &&
                                           command.overflow == UiTextOverflow::ellipsis &&
                                           command.color.alpha > 0 && command.color.alpha <= .5f;
                }
            }
            check(no_wrap_disabled,
                  "text_form_dynamic_action_dark_light_retains_disabled_style_and_no_wrap");
        }
        const auto action_revision = actions.revision, action_layout = actions.widgets.layout.revision;
        const auto action_measurements = cache.backend_calls;
        bool retained = true;
        for (u32 frame = 0; frame < 64; ++frame)
            retained = text_form_update(&actions, name, action_viewport, &cache) && retained;
        check(retained && actions.revision == action_revision &&
                  actions.widgets.layout.revision == action_layout &&
                  cache.backend_calls == action_measurements,
              "text_form_dynamic_action_64_unchanged_updates_retain_geometry_and_measurements");
        text_form_release(&actions);
    }
    text_form_release(&actions);

    // The ban dialog's shape: a radio group, an editor shown only under its
    // last choice, and a dropdown that lists its options while open.
    TextFormScene choices{};
    const Utf8View reasons[] = {utf8_literal("Spam"), utf8_literal("Rules"), utf8_literal("Other")};
    const Utf8View windows[] = {utf8_literal("None"), utf8_literal("Hour"), utf8_literal("Day")};
    TextFormContent dialog{utf8_literal("Ban @someone?"), {}, {}, {}, utf8_literal("Cancel"), utf8_literal("Ban"),
                           {}, 0, 0, true, false};
    dialog.choices_label = utf8_literal("Reason for Ban");
    dialog.choices = {reasons, 3};
    dialog.choice_editor = 2;
    dialog.select_label = utf8_literal("Delete Message History");
    dialog.select_options = {windows, 3};
    dialog.select_selected = 1;
    const UiRect dialog_viewport{0, 0, 800, 700};
    const bool opened = text_form_update(&choices, dialog, dialog_viewport, &cache);
    const auto* collapsed = text_form_node(choices, TextFormElement::edit);
    check(opened && text_form_focused(choices) == TextFormElement::choice_0 && collapsed &&
              ui_rect_height(collapsed->rectangle) == 0 && !text_form_node(choices, TextFormElement::choice_3) &&
              !text_form_node(choices, TextFormElement::select_0) && !text_form_node(choices, TextFormElement::label) &&
              ui_rect_has_area(text_form_choice_marker(choices, TextFormElement::choice_2)) &&
              !text_form_focus(&choices, TextFormElement::edit),
          "text_form_choices_open_on_the_first_option_with_the_editor_folded");
    dialog.choice_selected = 2;
    dialog.select_expanded = true;
    const bool chosen = text_form_update(&choices, dialog, dialog_viewport, &cache);
    const auto* edit = text_form_node(choices, TextFormElement::edit);
    const auto* select = text_form_node(choices, TextFormElement::select);
    const auto* last = text_form_node(choices, TextFormElement::select_2);
    u32 option = 0;
    check(chosen && edit && ui_rect_height(edit->rectangle) > 0 && text_form_focus(&choices, TextFormElement::edit) &&
              select && last && last->rectangle.top >= select->rectangle.bottom &&
              !text_form_node(choices, TextFormElement::select_3) &&
              text_form_select_index(TextFormElement::select_2, &option) && option == 2 &&
              !text_form_choice_index(TextFormElement::select_0, &option) &&
              ui_layout_engine_validate(choices.widgets.layout, {diagnostics, 32}).clean(),
          "text_form_choices_unfold_the_editor_and_list_the_open_dropdown_beneath_it");
    dialog.choice_selected = 5;
    check(!text_form_update(&choices, dialog, dialog_viewport, &cache), "text_form_choices_refuse_a_missing_option");
    text_form_release(&choices);
}
} // namespace maicord
