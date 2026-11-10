#include "../../shell/click_count.hpp"
#include "../../shell/content_point.hpp"
#include "../../shell/overlay_input.hpp"
#include "text_form_state.hpp"
#include "../../shell/cursor.hpp"
#include "text_input.hpp"
#include "text_geometry.hpp"
#include "../../shell/state.hpp"
#include "../../account/account_presentation.hpp"
#include "composer.hpp"
#include "input.hpp"
#include "../messages/interaction.hpp"
#include "../../shell/scrolling.hpp"
#include "../../shell/input.hpp"
#include "../threads/forum_form.hpp"
#include "../../account/login/ui.hpp"
#include "maicord/platform/window/clipboard.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/scratch_pages.hpp"
#include "../../account/account_ui_runtime.hpp"

#include "maicord/platform/window/key.hpp"
#include "maicord/platform/desktop/screen_color.hpp"

namespace maicord::app_detail {
namespace {
// The desktop could not pick a colour from the screen once: the eyedropper stays hidden for the session.
bool g_eyedropper_unavailable;
struct LoginFormMeasureStorage {
    UiTextMeasureCache cache;
    UiTextMeasureRecord records[kTextFormElements * kUiTextMeasureCacheWays];
};
bool owner_context_valid(const ApplicationTextFormRequest& request) noexcept {
    if (request.login_owner)
        return request.confirmation_only && g_app.login.visible && !g_app.account_ui &&
               !request.forum_parent.serial && !application_forum_form_active();
    return g_app.account_ui && !g_app.login.visible;
}
bool valid(const ApplicationTextForm* form) noexcept {
    return form && g_app.renderer_ready && owner_context_valid(form->request) &&
           (!form->request.forum_parent.serial ||
            (application_forum_form_active() &&
             application_forum_post_current(form->request.forum_parent))) &&
           form->request.valid && form->request.valid(form->request.owner);
}
bool user_dismissible(const ApplicationTextForm& form) noexcept {
    return form.can_cancel && form.dismissible;
}
void end_capture(ApplicationTextForm* form) noexcept {
    form->dragging = false;
    form->color_drag = TextFormElement::count;
    form->pointer_button = ui_widget_invalid_handle();
    ui_widget_manager_capture(&form->scene.widgets, ui_widget_invalid_handle());
    application_release_capture();
}
void reset_caret(ApplicationTextForm* form) noexcept {
    if (form->request.confirmation_only || !form->editable) {
        form->caret_visible = false;
        form->blink_at_ms = 0;
        return;
    }
    form->caret_visible =
        application_window_keyboard_focus() && text_form_focused(form->scene) == TextFormElement::edit;
    form->blink_at_ms = form->caret_visible && form->blink_ms != INFINITE
                            ? clock_monotonic_milliseconds() + form->blink_ms
                            : 0;
}
bool progress(ApplicationTextForm* form, bool* changed, bool* complete) noexcept {
    *changed = *complete = false;
    if (!form->request.progress)
        return true;
    const auto current = form->request.progress(form->request.owner);
    if (!utf8_validate(current.status).valid || !utf8_validate(current.accept).valid ||
        current.status.bytes > 2048 || current.accept.bytes > 128)
        return false;
    *complete = current.complete;
    if (current.revision == form->progress_revision && current.editable == form->editable &&
        current.can_accept == form->can_accept && current.can_cancel == form->can_cancel &&
        current.dismissible == form->dismissible && utf8_equal(current.accept, form->progress_accept))
        return true;
    *changed = true;
    form->progress_revision = current.revision;
    form->progress_accept = current.accept;
    form->status = current.status;
    const bool controls_changed = form->can_accept != current.can_accept ||
                                  form->can_cancel != current.can_cancel ||
                                  form->dismissible != current.dismissible;
    const bool editability_changed = form->editable != current.editable;
    // Publish the snapshot before releasing OS capture, which may synchronously
    // route WM_CAPTURECHANGED back through this same progress projection.
    form->can_accept = current.can_accept;
    form->can_cancel = current.can_cancel;
    form->dismissible = current.dismissible;
    form->editable = current.editable;
    if (controls_changed || editability_changed) {
        end_capture(form);
        ui_button_keyboard_cancel(&form->keyboard);
    }
    if (editability_changed) {
        if (!current.editable) {
            if (!form->request.confirmation_only) {
                secure_text_store_unbind(&form->store);
                secure_edit_cancel_composition(&form->edit);
            }
            reset_value(&form->composition);
            if (form->scene.active && form->can_cancel)
                text_form_focus(&form->scene, TextFormElement::cancel);
        }
        reset_caret(form);
    }
    return true;
}
bool project(ApplicationTextForm* form) noexcept {
    text_form_bind_theme_styles(&form->scene, application_theme().styles);
    text_form_bind_measure_revision(&form->scene, renderer_text_measure_revision(g_app.renderer));
    const auto& request = form->request;
    const bool confirmation = request.confirmation_only;
    Utf8View shown{};
    SecureEditUtf16Selection selection{};
    if (!confirmation) {
        // Only ordinary fields may lend their stable model bytes to presentation; a secret field shows its bullets.
        // The scene/renderer are detached before this model is released.
        if (form->edit.password != request.secret || form->edit.storage.bytes > form->edit.storage.capacity ||
            form->edit.storage.capacity > kApplicationTextFormBytes)
            return false;
        if (request.secret) {
            SecureEditRenderWindow window{};
            if (secure_edit_render_window(form->edit, kApplicationTextFormSecretCodepoints,
                                          {form->mask, sizeof(form->mask)}, &shown.bytes,
                                          &window) != SecureEditResult::unchanged)
                return false;
            shown.data = form->mask;
            selection = {window.anchor, window.caret};
        } else {
            const auto text = secure_edit_view(form->edit);
            if (text.count != form->edit.storage.bytes || (!text.data && text.count) ||
                secure_edit_utf16_selection(form->edit, &selection) != SecureEditResult::unchanged)
                return false;
            shown = {reinterpret_cast<const char*>(text.data), text.count};
            secure_text_store_composition(form->store, &form->composition);
        }
    }
    TextFormContent content{request.title,
                            request.context,
                            request.label,
                            form->status,
                            confirmation && request.cancel.bytes ? request.cancel : utf8_literal("Cancel"),
                            form->progress_accept.bytes ? form->progress_accept : request.accept,
                            shown,
                            form->editable ? selection.anchor : selection.caret,
                            selection.caret,
                            form->can_accept,
                            !confirmation && form->caret_visible,
                            {confirmation ? nullptr : form->composition.spans,
                             confirmation ? 0u : form->composition.span_count},
                            g_app.keyboard_focus_visible,
                            confirmation,
                            form->editable,
                            request.single_line,
                            form->can_cancel,
                            request.danger};
    if (confirmation) {
        content.option = request.option;
        content.option_checked = form->option_checked;
        content.detail = request.detail;
        content.single_action = request.single_action;
    } else {
        content.choices_label = request.choices_label;
        content.choices = {form->choices, request.choices.count};
        content.choice_selected = form->choice_selected;
        content.choice_editor = request.choice_editor - 1;
        content.select_label = request.select_label;
        content.select_options = {form->options, request.select_options.count};
        content.select_selected = form->select_selected;
        content.select_expanded = form->select_expanded;
        content.placeholder = request.placeholder;
        content.error = form->error;
        content.swatch = request.swatch;
        content.swatch_rgba = form->swatch_rgba;
        content.picker = request.picker;
        content.qr = request.qr;
        if (request.picker) {
            content.color = form->color;
            content.color.eyedropper = !g_eyedropper_unavailable;
            content.color.picking = form->color_picking;
        }
    }
    return text_form_update(&form->scene, content, application_viewport_dips(), form->measure);
}
// Runs the request's check on the current text, remembering it while the
// check accepts it. Returns whether it was accepted.
bool check_text(ApplicationTextForm* form) noexcept {
    if (!form->request.check || form->request.confirmation_only)
        return true;
    const auto view = secure_edit_view(form->edit);
    const Utf8View text{reinterpret_cast<const char*>(view.data), view.count};
    u32 rgba = form->swatch_rgba;
    if (!form->request.check(form->request.owner, text, &rgba))
        return false;
    form->swatch_rgba = rgba;
    if (form->request.picker) {
        // Typed text moves the square and the hue bar; the picker's own writes keep the hue it set.
        if (!form->color_writing)
            form->color.hsv = color_picker_hsv(rgba >> 8, form->color.hsv);
        if (form->request.color_changed && form->request.valid(form->request.owner))
            form->request.color_changed(form->request.owner, rgba >> 8);
    }
    if (text.bytes <= sizeof(form->accepted)) {
        copy_bytes(form->accepted, text.data, text.bytes);
        form->accepted_bytes = static_cast<u8>(text.bytes);
    }
    return true;
}
bool refresh(ApplicationTextForm* form) noexcept {
    if (form->refreshing)
        return true;
    form->refreshing = true;
    const bool result =
        project(form) && renderer_rebuild_content_text(&g_app.renderer, application_theme());
    if (!form->request.confirmation_only) {
        secure_text_store_synchronize(&form->store);
        secure_text_store_notify_layout(&form->store);
    }
    form->refreshing = false;
    application_request_frame();
    return result;
}
// A colour picker's own change (square, hue bar, keys, a swatch, the eyedropper): the field shows the colour as
// #RRGGBB and the check previews it, keeping the hue the picker set.
void set_color(ApplicationTextForm* form, ColorPickerHsv hsv) noexcept {
    form->color.hsv = hsv;
    char hex[8];
    const u32 bytes = color_hex_format(color_picker_rgb(hsv) << 8 | 0xFF, false, {hex, sizeof(hex)});
    form->color_writing = true;
    secure_edit_assign(&form->edit, {hex, bytes});
    reset_value(&form->status);
    form->error = false;
    check_text(form);
    form->color_writing = false;
    refresh(form);
}
void pick_settled(void*) noexcept {
    application_request_frame();
}
void end_pick(ApplicationTextForm* form) noexcept {
    screen_color_pick_end();
    form->color_picking = false;
    reset_value(&form->status);
}
// The eyedropper: starts a pick, or ends the one under way.
void toggle_pick(ApplicationTextForm* form) noexcept {
    if (form->color_picking) {
        end_pick(form);
    } else if (screen_color_pick_start(pick_settled, nullptr)) {
        form->color_picking = true;
        form->error = false;
#if defined(_WIN32)
        form->status = utf8_literal("Click anywhere on your screen to pick a color. Right-click or press Esc to cancel.");
#else
        form->status = utf8_literal("Pick a color anywhere on your screen.");
#endif
    } else {
        g_eyedropper_unavailable = true;
        form->status = utf8_literal("Colors can't be picked from the screen here.");
        form->error = true;
    }
    refresh(form);
}
// The eyedropper's answer, once it settled.
void poll_pick(ApplicationTextForm* form) noexcept {
    u32 rgb = 0;
    const auto state = form->color_picking ? screen_color_pick_state(&rgb) : ScreenColorState::idle;
    if (state == ScreenColorState::idle || state == ScreenColorState::picking)
        return;
    end_pick(form);
    if (state == ScreenColorState::picked) {
        set_color(form, color_picker_hsv(rgb, form->color.hsv));
        return;
    }
    if (state == ScreenColorState::failed) {
        g_eyedropper_unavailable = true;
        form->status = utf8_literal("Colors can't be picked from the screen here.");
        form->error = true;
    }
    refresh(form);
}
// Applies a pending progress update. False when the form closed instead.
bool settle(ApplicationTextForm* form) noexcept {
    bool changed = false, complete = false;
    if (!progress(form, &changed, &complete) || complete) {
        application_text_form_close(false);
        return false;
    }
    if (changed)
        refresh(form);
    return true;
}
bool geometry(void* opaque, SecureTextStoreGeometry* output) noexcept {
    auto* form = static_cast<ApplicationTextForm*>(opaque);
    if (form != g_app.text_form)
        return false;
    const auto* field = text_form_node(form->scene, TextFormElement::edit);
    if (!field)
        return false;
    return text_store_geometry(field->content_rectangle, output);
}
bool point_geometry(void* opaque, SecureTextPoint client, bool round, u32* position, bool* inside) noexcept {
    auto* form = static_cast<ApplicationTextForm*>(opaque);
    if (form != g_app.text_form || !form->surface.layout.active ||
        form->surface.layout.text_signature !=
            text_edit_signature(
                {reinterpret_cast<const char*>(form->edit.storage.data), form->edit.storage.bytes}))
        return false;
    return text_store_point_geometry(form->surface.layout, client, round, position, inside);
}
void changed(void* opaque) noexcept {
    auto* form = static_cast<ApplicationTextForm*>(opaque);
    if (form != g_app.text_form || form->refreshing || !form->editable)
        return;
    reset_value(&form->status);
    form->error = false;
    form->typed = true;
    check_text(form);
    reset_caret(form);
    refresh(form);
}
bool focus_field(ApplicationTextForm* form, TextFormElement element) noexcept {
    if ((form->request.confirmation_only || !form->editable) && element == TextFormElement::edit)
        return false;
    if (!form->request.confirmation_only)
        secure_edit_break_history_run(&form->edit);
    if (!text_form_focus(&form->scene, element))
        return false;
    ui_button_keyboard_cancel(&form->keyboard);
    if (form->request.confirmation_only) {
        reset_caret(form);
        return refresh(form);
    }
    if (element == TextFormElement::edit) {
        if (!secure_text_store_bind(&form->store, &form->edit, SecureTextInputScope::text))
            return false;
        secure_text_store_set_window_focus(&form->store, application_window_keyboard_focus());
    } else
        secure_text_store_unbind(&form->store);
    reset_caret(form);
    return refresh(form);
}
bool activate(ApplicationTextForm* form, TextFormElement element) noexcept {
    if (!valid(form)) {
        application_text_form_close(false);
        return true;
    }
    if (element == TextFormElement::cancel) {
        if (user_dismissible(*form))
            application_text_form_close();
        return true;
    }
    if (element == TextFormElement::option) {
        if (form->request.option.bytes && form->editable) {
            form->option_checked = !form->option_checked;
            refresh(form);
        }
        return true;
    }
    if (element == TextFormElement::eyedropper) {
        if (form->editable)
            toggle_pick(form);
        return true;
    }
    if (element == TextFormElement::color_swatches) {
        if (form->editable && form->color.focused_swatch < form->color.suggested.count)
            set_color(form, color_picker_hsv(form->suggested[form->color.focused_swatch], form->color.hsv));
        return true;
    }
    if (element == TextFormElement::color_field || element == TextFormElement::color_hue)
        return true;
    u32 at = 0;
    if (text_form_choice_index(element, &at)) {
        // A new choice starts the field over, empty and untyped.
        if (form->editable && at < form->request.choices.count && at != form->choice_selected) {
            form->choice_selected = at;
            form->typed = false;
            secure_edit_assign(&form->edit, {});
            reset_value(&form->status);
            refresh(form);
            if (at + 1 == form->request.choice_editor)
                focus_field(form, TextFormElement::edit);
        }
        return true;
    }
    if (element == TextFormElement::select) {
        if (form->editable) {
            form->select_expanded = !form->select_expanded;
            refresh(form);
        }
        return true;
    }
    if (text_form_select_index(element, &at)) {
        if (form->editable && at < form->request.select_options.count) {
            form->select_selected = at;
            form->select_expanded = false;
            refresh(form);
            focus_field(form, TextFormElement::select);
        }
        return true;
    }
    if (element != TextFormElement::accept)
        return false;
    if (!form->can_accept)
        return true;
    if (!check_text(form)) {
        // Rejected text reverts to the last accepted text with an error.
        secure_edit_assign(&form->edit, {form->accepted, form->accepted_bytes});
        form->status = form->request.rejected.bytes ? form->request.rejected : utf8_literal("Enter a valid value.");
        form->error = true;
        reset_caret(form);
        refresh(form);
        return true;
    }
    const auto text = form->request.confirmation_only ? ConstSlice<u8>{} : secure_edit_view(form->edit);
    const Utf8View typed{reinterpret_cast<const char*>(text.data), text.count};
    const bool saved =
        form->request.save_choices
            ? form->request.save_choices(form->request.owner, typed, form->typed, form->choice_selected,
                                         form->select_selected)
        : form->request.save_option ? form->request.save_option(form->request.owner, form->option_checked)
                                    : form->request.save(form->request.owner, typed);
    if (g_app.text_form != form)
        return true;
    if (saved) {
        application_text_form_close();
    } else if (form->request.progress) {
        bool changed = false, complete = false;
        if (!progress(form, &changed, &complete) || complete)
            application_text_form_close(false);
        else
            refresh(form);
    } else {
        form->status =
            form->request.confirmation_only
                ? utf8_literal("Something went wrong. Try again.")
                : utf8_literal("Couldn't save. Try again.");
        form->error = true;
        refresh(form);
    }
    return true;
}
bool apply(ApplicationTextForm* form, SecureEditResult result) noexcept {
    if (result == SecureEditResult::capacity)
        form->status = utf8_literal("This text is too long."), form->error = true;
    else if (result == SecureEditResult::invalid_input || result == SecureEditResult::invalid_utf8 ||
             result == SecureEditResult::invalid_utf16)
        form->status = utf8_literal("This text contains an unsupported character."), form->error = true;
    else if (result == SecureEditResult::changed)
        form->status = {}, form->error = false;
    if (result == SecureEditResult::pending_surrogate)
        return true;
    if (result == SecureEditResult::unchanged && form->surface.layout.active) {
        const auto& layout = form->surface.layout;
        const float delta = layout.caret.top < layout.viewport_top
                                ? layout.caret.top - layout.viewport_top
                                : maximum(0.0f, layout.caret.top + layout.caret.height -
                                                    layout.viewport_top - layout.height);
        text_edit_layout_scroll(&form->surface.layout, delta);
    }
    reset_caret(form);
    refresh(form);
    return true;
}
bool pointer_position(ApplicationTextForm* form, float x, float y, bool extend) noexcept {
    SecureEditResult result;
    return !form->request.confirmation_only && form->editable &&
           secure_edit_pointer_select(&form->edit, form->surface.layout, x, y, extend, &result) && apply(form, result);
}
} // namespace

namespace {
// Moves the caret, and the anchor unless extending, to a UTF-16 position.
bool select_to(SecureEdit* edit, u32 position, bool extend, SecureEditResult* result) noexcept {
    SecureEditUtf16Selection selection{};
    if (secure_edit_utf16_selection(*edit, &selection) != SecureEditResult::unchanged)
        return false;
    if (!extend)
        selection.anchor = position;
    selection.caret = position;
    *result = secure_edit_set_utf16_selection(edit, selection);
    return true;
}
// Its own frame: the clipboard buffer must not weigh on every input event.
__declspec(noinline) SecureEditResult paste_clipboard(SecureEdit* edit) noexcept {
    char pasted[8000];
    const auto read = clipboard_read_utf8({pasted, minimum<u32>(edit->storage.capacity, sizeof(pasted))});
    const auto result = read.ok() ? secure_edit_insert_utf8(edit, {pasted, read.bytes})
                        : read.error == ClipboardTextError::capacity ? SecureEditResult::capacity
                                                                     : SecureEditResult::unchanged;
    SecureZeroMemory(pasted, sizeof(pasted));
    return result;
}
} // namespace

bool text_store_point_geometry(const TextEditLayoutOwner& layout, SecureTextPoint client, bool round, u32* position,
                               bool* inside) noexcept {
    if (!g_app.renderer.dpi)
        return false;
    POINT point{client.x, client.y};
    application_client_to_content(&point);
    const float scale = 96.0f / g_app.renderer.dpi;
    return text_edit_layout_point_geometry(layout, point.x * scale, point.y * scale, round, position, inside);
}

bool secure_edit_pointer_select(SecureEdit* edit, const TextEditLayoutOwner& layout, float x, float y, bool extend,
                                SecureEditResult* result) noexcept {
    u32 position = 0;
    return text_edit_layout_hit_test(layout, x, y, &position) && select_to(edit, position, extend, result);
}

bool secure_edit_command_key(SecureEdit* edit, const TextEditLayoutOwner& layout, u32 code, bool control, bool shift,
                             bool history, bool clipboard, SecureEditResult* result) noexcept {
    if (control && (code == 'Z' || code == 'Y')) {
        // A text service owns its active composition. Let it finish before replaying model history; consumed
        // native keys never reach this path.
        if (!history)
            return false;
        *result = code == 'Y' || shift ? secure_edit_redo(edit) : secure_edit_undo(edit);
        return true;
    }
    if (control && code == 'A') {
        *result = secure_edit_select_all(edit);
        return true;
    }
    if (control && (code == 'C' || code == 'X')) {
        if (!clipboard)
            return false;
        secure_edit_break_history_run(edit);
        const auto first = minimum(edit->anchor, edit->caret), last = maximum(edit->anchor, edit->caret);
        if (first == last || code != 'X' ||
            !clipboard_write_utf8({reinterpret_cast<const char*>(edit->storage.data + first), last - first}))
            return false;
        *result = secure_edit_delete_forward(edit);
        return true;
    }
    if (control && code == 'V') {
        *result = paste_clipboard(edit);
        return true;
    }
    switch (code) {
    case key::backspace:
        *result = secure_edit_backspace(edit, clock_monotonic_milliseconds());
        return true;
    case key::del:
        *result = secure_edit_delete_forward(edit, clock_monotonic_milliseconds());
        return true;
    case key::left:
        *result = secure_edit_move(edit, SecureEditMove::previous, shift);
        return true;
    case key::right:
        *result = secure_edit_move(edit, SecureEditMove::next, shift);
        return true;
    case key::home:
        *result = secure_edit_move(edit, control ? SecureEditMove::home : SecureEditMove::line_home, shift);
        return true;
    case key::end:
        *result = secure_edit_move(edit, control ? SecureEditMove::end : SecureEditMove::line_end, shift);
        return true;
    case key::up:
    case key::down: {
        u32 position = 0;
        return text_edit_layout_move_vertical(layout, code == key::down, &position) &&
               select_to(edit, position, shift, result);
    }
    default:
        return false;
    }
}

bool application_text_form_open(const ApplicationTextFormRequest& request) noexcept {
    const bool parent = request.forum_parent.serial != 0;
    const bool forum = application_forum_form_active();
    if ((forum && (!parent || !application_forum_post_current(request.forum_parent))) ||
        (parent && (!forum || !application_forum_post_current(request.forum_parent))))
        return false;
    if (g_app.text_form || !request.valid || (!request.save && !request.save_option && !request.save_choices) ||
        !request.valid(request.owner) || !g_app.renderer_ready || !owner_context_valid(request))
        return false;
    const bool choices = request.choices.count || request.select_options.count;
    if (request.maximum_bytes > kApplicationTextFormBytes ||
        ((request.option.bytes || request.save_option) && !request.confirmation_only) ||
        (choices && (request.confirmation_only || !request.save_choices)) || (request.save_choices && !choices) ||
        request.choices.count > kTextFormChoices || request.select_options.count > kTextFormSelectOptions ||
        (!request.choices.data && request.choices.count) ||
        (!request.select_options.data && request.select_options.count) ||
        request.choice_editor > request.choices.count ||
        (request.select_options.count && request.select_selected >= request.select_options.count) ||
        ((request.check || request.swatch) && request.confirmation_only) || (request.swatch && !request.check) ||
        ((request.detail.bytes || request.single_action) && !request.confirmation_only) ||
        request.detail.bytes > kApplicationTextFormDetailBytes || !utf8_validate(request.detail).valid ||
        (request.secret && (request.confirmation_only || !request.single_line || request.check ||
                            request.maximum_codepoints - 1 >= kApplicationTextFormSecretCodepoints)) ||
        (request.picker && (!request.swatch || !request.check || request.secret ||
                            request.suggested_colors.count > kColorPickerSuggestedColors ||
                            (!request.suggested_colors.data && request.suggested_colors.count))))
        return false;
    const Utf8View labels[] = {request.title,  request.context, request.label,       request.accept,
                               request.cancel, request.option,  request.placeholder, request.rejected};
    u32 label_bytes = 0;
    for (const auto label : labels) {
        if (!utf8_validate(label).valid || label.bytes > 2048 - label_bytes)
            return false;
        label_bytes += label.bytes;
    }
    const auto extra_label = [&](Utf8View label) noexcept {
        const bool fits = utf8_validate(label).valid && label.bytes <= 2048 - label_bytes;
        label_bytes += fits ? label.bytes : 0;
        return fits;
    };
    bool extra = extra_label(request.choices_label) && extra_label(request.select_label);
    for (u32 i = 0; extra && i < request.choices.count; ++i)
        extra = extra_label(request.choices[i]);
    for (u32 i = 0; extra && i < request.select_options.count; ++i)
        extra = extra_label(request.select_options[i]);
    if (!extra)
        return false;
    if (request.login_owner && !renderer_prepare_login_form_text(&g_app.renderer)) {
        renderer_release_login_form_text(&g_app.renderer);
        return false;
    }
    const usize owner_bytes =
        sizeof(ApplicationTextForm) + (request.login_owner ? sizeof(LoginFormMeasureStorage) : 0);
    auto* form = static_cast<ApplicationTextForm*>(
        commit_pages(owner_bytes));
    if (!form) {
        if (request.login_owner)
            renderer_release_login_form_text(&g_app.renderer);
        return false;
    }
    copy_value(&form->request, request);
    if (request.login_owner) {
        auto* measurement = reinterpret_cast<LoginFormMeasureStorage*>(form + 1);
        if (!ui_text_measure_cache_initialize(
                &measurement->cache, {measurement->records, kTextFormElements * kUiTextMeasureCacheWays},
                renderer_text_measure_backend, &g_app.renderer)) {
            pages_release(form);
            renderer_release_login_form_text(&g_app.renderer);
            return false;
        }
        form->measure = &measurement->cache;
    } else {
        form->measure = &g_app.account_ui->text_measure_cache;
    }
    form->editable = form->can_accept = form->can_cancel = form->dismissible = true;
    form->option_checked = request.option.bytes && request.option_checked;
    Utf8View* copied[] = {&form->request.title,  &form->request.context, &form->request.label,
                          &form->request.accept, &form->request.cancel,  &form->request.option,
                          &form->request.placeholder, &form->request.rejected};
    static_assert(sizeof(copied) / sizeof(copied[0]) == sizeof(labels) / sizeof(labels[0]));
    u32 offset = 0;
    const auto copy_label = [&](Utf8View source) noexcept {
        copy_bytes(form->labels + offset, source.data, source.bytes);
        const Utf8View copy{form->labels + offset, source.bytes};
        offset += source.bytes;
        return copy;
    };
    for (u32 i = 0; i < sizeof(copied) / sizeof(copied[0]); ++i)
        *copied[i] = copy_label(labels[i]);
    form->request.choices_label = copy_label(request.choices_label);
    form->request.select_label = copy_label(request.select_label);
    for (u32 i = 0; i < request.choices.count; ++i)
        form->choices[i] = copy_label(request.choices[i]);
    for (u32 i = 0; i < request.select_options.count; ++i)
        form->options[i] = copy_label(request.select_options[i]);
    form->request.choices = {form->choices, request.choices.count};
    form->request.select_options = {form->options, request.select_options.count};
    copy_bytes(form->detail, request.detail.data, request.detail.bytes);
    form->request.detail = {form->detail, request.detail.bytes};
    form->choice_selected = ~0u;
    form->select_selected = request.select_selected;
    if (request.picker) {
        copy_bytes(form->suggested, request.suggested_colors.data,
                   static_cast<u32>(request.suggested_colors.count * sizeof(u32)));
        form->request.suggested_colors = {form->suggested, request.suggested_colors.count};
        form->color.suggested = form->request.suggested_colors;
        form->color_drag = TextFormElement::count;
    }
    bool initialized = true;
    if (!request.confirmation_only) {
        initialized = secure_edit_initialize(
            &form->edit, request.maximum_bytes ? request.maximum_bytes : kApplicationTextFormBytes, request.secret,
            {request.maximum_codepoints, !request.single_line});
        if (initialized) {
            const auto assigned = secure_edit_assign(&form->edit, request.initial);
            initialized = assigned == SecureEditResult::changed || assigned == SecureEditResult::unchanged;
            if (initialized)
                check_text(form);
        }
        initialized = initialized && (request.secret || secure_edit_enable_history(&form->edit, 32 * 1024));
        form->blink_ms = application_caret_blink_ms();
        if (!form->blink_ms)
            form->blink_ms = 500;
        form->caret_visible = true;
    }
    reset_value(&form->request.initial);
    const auto discard = [&]() noexcept {
        text_form_release(&form->scene);
        if (!request.confirmation_only)
            secure_edit_release(&form->edit);
        pages_release(form);
        if (request.login_owner)
            renderer_release_login_form_text(&g_app.renderer);
        return false;
    };
    bool progress_changed = false, completed = false;
    if (!initialized || !progress(form, &progress_changed, &completed) || completed || !project(form))
        return discard();
    auto* prior = request.login_owner
                      ? &g_app.login_scene.widgets
                      : (g_app.context_focus ? g_app.context_focus : &g_app.message_composer_scene.widgets);
    if (parent && !application_forum_form_suspend_child(request.forum_parent))
        return discard();
    form->previous_focus = {prior, prior->focused};
    ui_button_keyboard_cancel(&g_app.button_keyboard);
    if (request.login_owner) {
        g_app.login.pointer_selection = LoginControl::none;
        ui_widget_manager_capture(&g_app.login_scene.widgets, ui_widget_invalid_handle());
        if (g_app.login.text_store.implementation) {
            secure_text_store_unbind(&g_app.login.text_store);
            secure_text_store_set_window_focus(&g_app.login.text_store, false);
        }
    } else {
        application_composer_text_input_suspend();
        form->previous_composer_focus = g_app.message_composer.focused;
        ui_scroll_host_cancel(&g_app.scroll_host);
        end_message_text_selection();
        end_message_composer_pointer_selection();
        end_scrollbar_drag();
        end_pane_resize();
        stop_local_typing(g_app.message_composer.channel_id);
        g_app.message_composer.focused = false;
        ++g_app.message_composer.revision;
        reset_message_composer_caret_timing(false);
        popup_scene_close(&g_app.popup_scene);
        set_popup_owner(PopupOwnerKind::none);
    }
    g_app.text_form = form;
    if (application_key_down(key::enter))
        g_app.text_form_held_key = key::enter;
    g_app.context_focus = &form->scene.widgets;
    form->surface.scene = &form->scene;
    renderer_bind_text_form(&g_app.renderer, &form->surface);
    if (request.login_owner)
        end_capture(form);
    if (request.login_owner && !application_refresh_login_view()) {
        application_text_form_close(false);
        return false;
    }
    if (!request.confirmation_only) {
        if (!secure_text_store_initialize(
                &form->store, {g_app.window, geometry, changed, form, nullptr, point_geometry}) ||
            (form->editable &&
             !secure_text_store_bind(&form->store, &form->edit, SecureTextInputScope::text)) ||
            !secure_text_store_attach(&form->store)) {
            application_text_form_close();
            return false;
        }
        secure_text_store_set_window_focus(&form->store, application_window_keyboard_focus());
    }
    reset_caret(form);
    if (!refresh(form)) {
        application_text_form_close();
        return false;
    }
    if (request.confirmation_only && request.accept_focused && form->can_accept &&
        !focus_field(form, TextFormElement::accept)) {
        application_text_form_close();
        return false;
    }
    return true;
}

namespace {
// A colour picker's Apply: the field's colour to the owner's save.
bool save_color(TextFormOwnerKey key, Utf8View text) noexcept {
    u32 rgb = 0;
    const auto* form = g_app.text_form;
    return form && color_hex_parse_rgb24(text, &rgb) && form->request.color_save(key, rgb);
}
} // namespace

bool application_color_picker_open(const ApplicationColorPickerRequest& picker) noexcept {
    if (!picker.save || picker.rgb > 0xFFFFFF)
        return false;
    char hex[8];
    const u32 bytes = color_hex_format(picker.rgb << 8 | 0xFF, false, {hex, sizeof(hex)});
    ApplicationTextFormRequest request{};
    request.title = picker.title;
    request.label = picker.label;
    request.accept = utf8_literal("Apply");
    request.initial = {hex, bytes};
    request.owner = picker.owner;
    request.valid = picker.valid;
    request.save = save_color;
    request.closed = picker.closed;
    request.maximum_codepoints = 7;
    request.single_line = true;
    request.placeholder = utf8_literal("#RRGGBB");
    request.check = application_text_form_color_check;
    request.rejected = utf8_literal("That isn't a hex color. Use #RGB or #RRGGBB.");
    request.swatch = true;
    request.picker = true;
    request.suggested_colors = picker.suggested;
    request.color_changed = picker.preview;
    request.color_save = picker.save;
    return application_text_form_open(request);
}

bool application_text_form_set_slider(UiPoint point, double value) noexcept {
    auto* form = g_app.text_form;
    if (!form || !valid(form) || !form->editable)
        return false;
    const auto element = text_form_hit_test(form->scene, point.x, point.y);
    const bool hue = element == TextFormElement::color_hue;
    if (!hue && element != TextFormElement::color_field)
        return false;
    const float at = maximum(0.f, minimum(static_cast<float>(value / (hue ? 360 : 100)), 1.f));
    auto hsv = form->color.hsv;
    (hue ? hsv.hue : hsv.saturation) = at;
    set_color(form, hsv);
    return true;
}

void application_text_form_close(bool restore_focus, bool rebuild) noexcept {
    auto* form = g_app.text_form;
    if (!form)
        return;
    const bool restore = restore_focus && valid(form);
    const auto previous = form->previous_focus;
    const auto closed = form->request.closed;
    const auto owner = form->request.owner;
    const auto forum_parent = form->request.forum_parent;
    const bool login_owner = form->request.login_owner;
    const bool composer = form->previous_composer_focus;
    if (application_key_down(key::enter))
        g_app.text_form_held_key = key::enter;
    else if (application_key_down(key::space))
        g_app.text_form_held_key = key::space;
    g_app.text_form = nullptr;
    g_app.context_focus = nullptr;
    renderer_bind_text_form(&g_app.renderer, nullptr);
    if (login_owner)
        renderer_release_login_form_text(&g_app.renderer);
    end_capture(form);
    if (form->color_picking)
        screen_color_pick_end();
    if (!form->request.confirmation_only)
        secure_text_store_release(&form->store);
    text_form_surface_release(&form->surface);
    text_form_release(&form->scene);
    if (!form->request.confirmation_only)
        secure_edit_release(&form->edit);
    SecureZeroMemory(form, sizeof(*form));
    pages_release(form);
    if (restore && previous.manager) {
        auto* manager = const_cast<UiWidgetManager*>(previous.manager);
        if (ui_widget_manager_focus(manager, previous.handle) ||
            ui_widget_manager_input_enabled(*manager, manager->focused)) {
            g_app.context_focus = manager;
            if (composer && manager == &g_app.message_composer_scene.widgets) {
                g_app.message_composer.focused = true;
                ++g_app.message_composer.revision;
                reset_message_composer_caret_timing(true);
            }
        }
    }
    if (rebuild && !forum_parent.serial && g_app.renderer_ready && g_app.account_ui)
        refresh_account_text(true);
    if (closed)
        closed(owner);
    if (forum_parent.serial)
        application_forum_form_resume_child(forum_parent);
    application_request_frame();
}

bool application_text_form_forum_child(const ApplicationForumPostTicket& ticket) noexcept {
    if (!ticket.serial || !g_app.text_form)
        return false;
    const auto& parent = g_app.text_form->request.forum_parent;
    return parent.serial == ticket.serial && parent.parent_id == ticket.parent_id &&
           parent.publication_revision == ticket.publication_revision;
}
void application_text_form_cancel_owner(TextFormOwnerValid validator) noexcept {
    if (g_app.text_form && g_app.text_form->request.valid == validator)
        application_text_form_close(false);
}
void application_text_form_theme_changed() noexcept {
    auto* form = g_app.text_form;
    if (!form)
        return;
    if (form->request.login_owner) {
        auto* owned = reinterpret_cast<LoginFormMeasureStorage*>(form + 1);
        if (form->measure == &owned->cache)
            ui_text_measure_cache_invalidate(&owned->cache);
    }
    // Shared account caches are invalidated by the publishing theme owner.
    // Keep editing and scrolling intact; the next visible projection relayouts.
    form->scene.layout_signature = 0;
}
bool application_text_form_synchronize() noexcept {
    auto* form = g_app.text_form;
    if (!form)
        return true;
    if (!valid(form)) {
        application_text_form_close(false);
        return true;
    }
    const bool styles_changed = text_form_bind_theme_styles(&form->scene, application_theme().styles);
    const bool measure_changed = text_form_bind_measure_revision(&form->scene,
                                                                renderer_text_measure_revision(g_app.renderer));
    bool changed = false, complete = false;
    if (!progress(form, &changed, &complete) || complete) {
        application_text_form_close(false);
        return true;
    }
    if (form->color_picking) {
        poll_pick(form);
        if (g_app.text_form != form)
            return true;
    }
    if (changed || styles_changed || measure_changed || !form->scene.layout_signature)
        return refresh(form);
    if (form->scene.content.focus_visible != g_app.keyboard_focus_visible ||
        !ui_rect_equal(application_viewport_dips(), form->scene.viewport))
        return refresh(form);
    return true;
}
bool application_text_form_input(UiInputKind kind, u32 code, bool repeated) noexcept {
    if (ui_input_consume_key_tail(&g_app.text_form_held_key, {kind, code, repeated}))
        return true;
    auto* form = g_app.text_form;
    if (!form)
        return false;
    if (!valid(form)) {
        application_text_form_close(false);
        return true;
    }
    if (!settle(form))
        return true;
    if (kind == UiInputKind::system_key_down)
        return code != key::f4 && code != key::space;
    if (kind == UiInputKind::system_character)
        return code != ' ';
    const bool ctrl = application_control_down();
    const bool shift = application_shift_down();
    const auto focused = text_form_focused(form->scene);
    if (kind == UiInputKind::key_down && code == key::escape) {
        g_app.text_form_held_key = code;
        // Escape first ends a pick under way, then closes.
        if (form->color_picking) {
            end_pick(form);
            refresh(form);
        } else if (user_dismissible(*form))
            application_text_form_close();
        return true;
    }
    if (kind == UiInputKind::key_down && form->editable &&
        (focused == TextFormElement::color_field || focused == TextFormElement::color_hue ||
         focused == TextFormElement::color_swatches)) {
        auto hsv = form->color.hsv;
        if (focused == TextFormElement::color_swatches) {
            const auto* row = text_form_node(form->scene, TextFormElement::color_swatches);
            if (row && color_picker_swatch_key(row->rectangle, form->color.suggested.count,
                                               &form->color.focused_swatch, code)) {
                refresh(form);
                return true;
            }
        } else if (focused == TextFormElement::color_field ? color_picker_field_key(&hsv, code, shift)
                                                           : color_picker_hue_key(&hsv, code, shift)) {
            set_color(form, hsv);
            return true;
        }
    }
    if (kind == UiInputKind::key_down && code == key::tab) {
        if (text_form_move_focus(&form->scene, shift))
            focus_field(form, text_form_focused(form->scene));
        return true;
    }
    if (focused != TextFormElement::edit || form->request.confirmation_only || !form->editable) {
        if (kind == UiInputKind::key_down || kind == UiInputKind::key_up) {
            const auto key = code == key::enter  ? UiButtonKey::enter
                             : code == key::space ? UiButtonKey::space
                                                : UiButtonKey::other;
            const auto action = ui_button_keyboard_key(&form->keyboard, &form->scene.widgets, key,
                                                       kind == UiInputKind::key_down, repeated);
            if (action.activate) {
                if (kind == UiInputKind::key_down)
                    g_app.text_form_held_key = code;
                activate(form, focused);
                if (!g_app.text_form && kind == UiInputKind::key_down)
                    g_app.text_form_held_key = code;
            }
        }
        return true;
    }
    if (kind == UiInputKind::character) {
        if (ctrl || code == '\b' || code == '\t' || code == 27)
            return true;
        return code <= 0xFFFF
                   ? apply(form, secure_edit_insert_utf16(&form->edit,
                                                          static_cast<u16>(code == '\r' ? '\n' : code),
                                                          clock_monotonic_milliseconds()))
                   : true;
    }
    if (kind != UiInputKind::key_down)
        return true;
    if (code == key::enter && form->request.single_line) {
        if (!repeated && !form->composition.active) {
            g_app.text_form_held_key = code;
            activate(form, TextFormElement::accept);
        }
        return true;
    }
    if (ctrl && code == key::enter) {
        if (!repeated) {
            g_app.text_form_held_key = code;
            activate(form, TextFormElement::accept);
            if (!g_app.text_form)
                g_app.text_form_held_key = code;
        }
        return true;
    }
    // Editing keys; a secret field keeps no history and no clipboard copy, a composition owns undo.
    SecureEditResult result;
    if (secure_edit_command_key(&form->edit, form->surface.layout, code, ctrl, shift,
                                !form->composition.active && !form->request.secret, !form->request.secret, &result))
        apply(form, result);
    return true;
}

bool application_text_form_filter(const WindowEvent& event) noexcept {
    UiInputEvent held{};
    if (application_held_key_event(g_app.text_form_held_key, event, &held))
        return application_text_form_input(held.kind, held.code, held.repeated);
    auto* form = g_app.text_form;
    if (!form)
        return false;
    if (!valid(form)) {
        application_text_form_close(false);
        return false;
    }
    if (!settle(form))
        return false;
    return form->editable && !form->request.confirmation_only && secure_text_store_filter_key(&form->store, event);
}

bool application_text_form_window_event(const WindowEvent& event) noexcept {
    if (input_focus_lost(event) || event.type == EventType::cancel_input)
        g_app.text_form_held_key = 0;
    auto* form = g_app.text_form;
    if (!form)
        return false;
    if (!valid(form)) {
        application_text_form_close(false);
        return false;
    }
    if (!settle(form))
        return true;
    // Content-pixel position of a pointer event, in DIPs.
    const float x = input_pointer(event) ? client_dip_x(event) : 0.f;
    const float y = input_pointer(event) ? client_dip_y(event) : 0.f;
    if (input_focus_lost(event)) {
        // The dialog stays open, as the official client's do; the window's focus returning resumes the field.
        end_capture(form);
        ui_button_keyboard_cancel(&form->keyboard);
        if (!form->request.confirmation_only) {
            secure_text_store_set_window_focus(&form->store, false);
            secure_text_store_unbind(&form->store);
            reset_value(&form->composition);
            secure_edit_cancel_composition(&form->edit);
        }
        form->caret_visible = false;
        form->blink_at_ms = 0;
        refresh(form);
        return true; // Consume this dismissal; the parent overlay remains alive.
    }
    if (event.type == EventType::focus_changed) {
        if (!form->request.confirmation_only) {
            secure_edit_break_history_run(&form->edit);
            if (form->editable && text_form_focused(form->scene) == TextFormElement::edit)
                secure_text_store_bind(&form->store, &form->edit, SecureTextInputScope::text);
            secure_text_store_set_window_focus(&form->store, true);
        }
        reset_caret(form);
        refresh(form);
        return false;
    }
    if (event.type == EventType::capture_lost || event.type == EventType::cancel_input) {
        const bool cancelled = event.type == EventType::cancel_input;
        if (cancelled)
            end_capture(form);
        else {
            form->dragging = false;
            form->color_drag = TextFormElement::count;
            form->pointer_button = ui_widget_invalid_handle();
            ui_widget_manager_capture(&form->scene.widgets, ui_widget_invalid_handle());
        }
        ui_button_keyboard_cancel(&form->keyboard);
        if (cancelled && !form->request.confirmation_only) {
            secure_text_store_unbind(&form->store);
            reset_value(&form->composition);
            secure_edit_cancel_composition(&form->edit);
            if (form->editable && text_form_focused(form->scene) == TextFormElement::edit) {
                secure_text_store_bind(&form->store, &form->edit, SecureTextInputScope::text);
                secure_text_store_set_window_focus(&form->store, application_window_keyboard_focus());
            }
            reset_caret(form);
            refresh(form);
        }
        return !user_dismissible(*form);
    }
    switch (event.type) {
    case EventType::pointer_down: {
        // The right button's press is swallowed; other buttons but the primary one pass.
        if (input_press(event, PointerButton::right))
            return true;
        if (!input_down(event))
            return false;
        const auto* panel = text_form_node(form->scene, TextFormElement::panel);
        if (panel && !ui_rect_contains(panel->rectangle, x, y)) {
            if (user_dismissible(*form))
                application_text_form_close();
            return true;
        }
        application_focus_window();
        const auto element = text_form_hit_test(form->scene, x, y);
        if (form->editable && (element == TextFormElement::color_field || element == TextFormElement::color_hue ||
                               element == TextFormElement::color_swatches)) {
            // The square and the hue bar follow the pointer until release; a swatch applies at once.
            if (!focus_field(form, element))
                return true;
            const auto* node = text_form_node(form->scene, element);
            if (element == TextFormElement::color_swatches) {
                const u32 at = color_picker_swatch_at(node->rectangle, form->color.suggested.count, x, y);
                if (at < form->color.suggested.count) {
                    form->color.focused_swatch = at;
                    set_color(form, color_picker_hsv(form->suggested[at], form->color.hsv));
                }
                return true;
            }
            set_color(form, element == TextFormElement::color_field
                                ? color_picker_field_at(form->color.hsv, node->rectangle, x, y)
                                : color_picker_hue_at(form->color.hsv, node->rectangle, x));
            if (application_capture_widget(&form->scene.widgets, form->scene.elements[static_cast<u32>(element)]))
                form->color_drag = element;
            return true;
        }
        u32 at = 0;
        if (element == TextFormElement::edit || element == TextFormElement::cancel ||
            element == TextFormElement::accept || element == TextFormElement::option ||
            element == TextFormElement::select || element == TextFormElement::eyedropper ||
            text_form_choice_index(element, &at) || text_form_select_index(element, &at)) {
            if (!focus_field(form, element))
                return true;
            const auto handle = form->scene.elements[static_cast<u32>(element)];
            if (element == TextFormElement::edit && !form->request.confirmation_only) {
                // Double click: word; a third press within the double-click
                // time/distance: line. An IME composition keeps its selection.
                const u8 clicks = application_click_count(event, &form->scene, x, y, input_double(event));
                const bool composing = secure_text_store_composing(form->store);
                if (!composing) {
                    pointer_position(form, x, y, clicks == 1 && input_shift(event));
                    if (clicks == 2)
                        apply(form, secure_edit_select_word(&form->edit, form->edit.caret));
                    else if (clicks == 3)
                        apply(form, secure_edit_select_line(&form->edit, form->edit.caret));
                }
                form->dragging = clicks == 1 && !composing;
            } else
                form->pointer_button = handle;
            if (!application_capture_widget(&form->scene.widgets, handle))
                end_capture(form);
        }
        return true;
    }
    case EventType::pointer_move: {
        if (form->dragging && input_held(event))
            pointer_position(form, x, y, true);
        if ((form->color_drag == TextFormElement::color_field || form->color_drag == TextFormElement::color_hue) &&
            input_held(event)) {
            const auto* node = text_form_node(form->scene, form->color_drag);
            if (node) {
                const auto hsv = form->color_drag == TextFormElement::color_field
                                     ? color_picker_field_at(form->color.hsv, node->rectangle, x, y)
                                     : color_picker_hue_at(form->color.hsv, node->rectangle, x);
                if (hsv.hue != form->color.hsv.hue || hsv.saturation != form->color.hsv.saturation ||
                    hsv.value != form->color.hsv.value)
                    set_color(form, hsv);
            }
            return true;
        }
        // Footer buttons and the option row light up only under the pointer.
        const auto element = text_form_hit_test(form->scene, x, y);
        u32 at = 0;
        const auto hovered = element == TextFormElement::cancel || element == TextFormElement::accept ||
                                     element == TextFormElement::option || element == TextFormElement::select ||
                                     element == TextFormElement::eyedropper ||
                                     text_form_choice_index(element, &at) || text_form_select_index(element, &at)
                                 ? form->scene.elements[static_cast<u32>(element)]
                                 : ui_widget_invalid_handle();
        const auto previous = form->scene.widgets.hovered;
        if (!ui_widget_handle_equal(previous, hovered)) {
            if (!ui_widget_manager_hover(&form->scene.widgets, hovered))
                ui_widget_manager_hover(&form->scene.widgets, ui_widget_invalid_handle());
            refresh(form);
        }
        return true;
    }
    case EventType::pointer_leave:
        if (form->scene.widgets.hovered.generation) {
            ui_widget_manager_hover(&form->scene.widgets, ui_widget_invalid_handle());
            refresh(form);
        }
        return false;
    case EventType::pointer_up: {
        if (input_release(event)) {
            const auto button = form->pointer_button;
            const auto element = text_form_hit_test(form->scene, x, y);
            const bool clicked =
                element != TextFormElement::count &&
                ui_widget_handle_matches(button, form->scene.elements[static_cast<u32>(element)]);
            end_capture(form);
            if (clicked)
                activate(form, element);
            return true;
        }
        // The right button's release is swallowed; the other buttons' pass.
        return input_release(event, PointerButton::right);
    }
    case EventType::wheel: {
        if (input_horizontal_wheel(event))
            return true;
        const UiPoint point = client_wheel_dip(event);
        const float delta = -input_wheel_notches(event) * 48;
        if (!form->request.confirmation_only &&
            text_form_hit_test(form->scene, point.x, point.y) == TextFormElement::edit)
            text_edit_layout_scroll(&form->surface.layout, delta);
        else
            text_form_scroll(&form->scene, delta);
        refresh(form);
        return true;
    }
    case EventType::context_menu:
        return true;
    case EventType::cursor_query: {
        // Buttons are hands and the editor an I-beam, from the widgets themselves.
        UiPoint point{};
        application_set_cursor(client_cursor_dip(event, &point) ? ui_widget_manager_cursor(form->scene.widgets, point.x, point.y)
                                                                : UiCursor::arrow);
        return true;
    }
    default:
        return false;
    }
}
void application_text_form_tick(u64 now_ms) noexcept {
    auto* form = g_app.text_form;
    if (!form)
        return;
    if (!valid(form)) {
        application_text_form_close(false);
        return;
    }
    if (form->blink_at_ms && now_ms >= form->blink_at_ms) {
        form->caret_visible = !form->caret_visible;
        form->blink_at_ms = now_ms + form->blink_ms;
        refresh(form);
    }
}
u64 application_text_form_next_wake() noexcept {
    return g_app.text_form ? g_app.text_form->blink_at_ms : 0;
}
} // namespace maicord::app_detail
