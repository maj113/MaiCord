#include "application_click_count.hpp"
#include "application_content_point.hpp"
#include "application_text_form_state.hpp"
#include "application_cursor.hpp"
#include "application_composer_text_input.hpp"
#include "application_state.hpp"
#include "application_account_presentation.hpp"
#include "application_composer.hpp"
#include "application_composer_input.hpp"
#include "application_message_interaction.hpp"
#include "application_scrolling.hpp"
#include "application_shell_input.hpp"
#include "application_forum_form.hpp"
#include "application_login_ui.hpp"
#include "maicord/ui/clipboard.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/memory.hpp"
#include "account_ui_runtime.hpp"

#include "maicord/core/win32_user.hpp"
#include "maicord/sys/winsock.hpp"

namespace maicord::app_detail {
namespace {
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
    form->pointer_button = ui_widget_invalid_handle();
    ui_widget_manager_capture(&form->scene.widgets, ui_widget_invalid_handle());
    if (GetCapture() == g_app.window)
        ReleaseCapture();
}
void reset_caret(ApplicationTextForm* form) noexcept {
    if (form->request.confirmation_only || !form->editable) {
        form->caret_visible = false;
        form->blink_at_ms = 0;
        return;
    }
    form->caret_visible =
        GetFocus() == g_app.window && text_form_focused(form->scene) == TextFormElement::edit;
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
    const Utf8View cancel = request.cancel.bytes ? request.cancel : utf8_literal("Cancel");
    if (request.confirmation_only) {
        TextFormContent content{request.title,
                                request.context,
                                {},
                                form->status,
                                cancel,
                                form->progress_accept.bytes ? form->progress_accept : request.accept,
                                {},
                                0,
                                0,
                                form->can_accept,
                                false,
                                {},
                                g_app.keyboard_focus_visible,
                                true,
                                form->editable,
                                request.single_line,
                                form->can_cancel,
                                request.danger};
        content.option = request.option;
        content.option_checked = form->option_checked;
        return text_form_update(&form->scene, content, application_viewport_dips(), form->measure);
    }
    // Only ordinary fields may lend their stable model bytes to presentation.
    // The scene/renderer are detached before this model is released.
    if (form->edit.password || form->edit.storage.bytes > form->edit.storage.capacity ||
        form->edit.storage.capacity > kApplicationTextFormBytes)
        return false;
    const auto text = secure_edit_view(form->edit);
    SecureEditUtf16Selection selection{};
    if (text.count != form->edit.storage.bytes || (!text.data && text.count) ||
        secure_edit_utf16_selection(form->edit, &selection) != SecureEditResult::unchanged)
        return false;
    secure_text_store_composition(form->store, &form->composition);
    TextFormContent content{request.title,
                             request.context,
                             request.label,
                             form->status,
                             utf8_literal("Cancel"),
                             form->progress_accept.bytes ? form->progress_accept : request.accept,
                             {reinterpret_cast<const char*>(text.data), text.count},
                             form->editable ? selection.anchor : selection.caret,
                             selection.caret,
                             form->can_accept,
                             form->caret_visible,
                             {form->composition.spans, form->composition.span_count},
                             g_app.keyboard_focus_visible,
                             false,
                             form->editable,
                             request.single_line,
                             form->can_cancel,
                             request.danger,
                             {},
                             false};
    content.choices_label = request.choices_label;
    content.choices = {form->choices, request.choices.count};
    content.choice_selected = form->choice_selected;
    content.choice_editor = request.choice_editor;
    content.select_label = request.select_label;
    content.select_options = {form->options, request.select_options.count};
    content.select_selected = form->select_selected;
    content.select_expanded = form->select_expanded;
    content.placeholder = request.placeholder;
    content.error = form->error;
    content.swatch = request.swatch;
    content.swatch_rgba = form->swatch_rgba;
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
    if (form != g_app.text_form || !output || !g_app.window)
        return false;
    const auto* field = text_form_node(form->scene, TextFormElement::edit);
    if (!field)
        return false;
    const float scale = static_cast<float>(g_app.renderer.dpi) / 96;
    const auto bounds = field->content_rectangle;
    POINT origin{};
    if (!application_content_to_screen(&origin) ||
        !GetWindowRect(g_app.window, output->window_screen.native()))
        return false;
    output->text_screen = {origin.x + static_cast<LONG>(bounds.left * scale),
                           origin.y + static_cast<LONG>(bounds.top * scale),
                           origin.x + static_cast<LONG>(bounds.right * scale),
                           origin.y + static_cast<LONG>(bounds.bottom * scale)};
    return true;
}
bool point_geometry(void* opaque, POINT point, bool round, u32* position, bool* inside) noexcept {
    auto* form = static_cast<ApplicationTextForm*>(opaque);
    if (!form || form != g_app.text_form || !g_app.renderer.dpi || !form->surface.layout.active ||
        form->surface.layout.text_signature !=
            text_edit_signature(
                {reinterpret_cast<const char*>(form->edit.storage.data), form->edit.storage.bytes}) ||
        !application_screen_to_content(&point))
        return false;
    const float scale = 96.0f / g_app.renderer.dpi;
    return text_edit_layout_point_geometry(form->surface.layout, point.x * scale, point.y * scale, round,
                                             position, inside);
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
        secure_text_store_set_window_focus(&form->store, GetFocus() == g_app.window);
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
    u32 at = 0;
    if (text_form_choice_index(element, &at)) {
        // A new choice starts the field over, empty and untyped.
        if (form->editable && at < form->request.choices.count && at != form->choice_selected) {
            form->choice_selected = at;
            form->typed = false;
            secure_edit_assign(&form->edit, {});
            reset_value(&form->status);
            refresh(form);
            if (at == form->request.choice_editor)
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
    if (form->request.confirmation_only || !form->editable)
        return false;
    u32 position = 0;
    SecureEditUtf16Selection selection{};
    if (!text_edit_layout_hit_test(form->surface.layout, x, y, &position) ||
        secure_edit_utf16_selection(form->edit, &selection) != SecureEditResult::unchanged)
        return false;
    if (!extend)
        selection.anchor = position;
    selection.caret = position;
    return apply(form, secure_edit_set_utf16_selection(&form->edit, selection));
}
} // namespace

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
        (request.choice_selected != ~0u && request.choice_selected >= request.choices.count) ||
        (request.choice_editor != ~0u && request.choice_editor >= request.choices.count) ||
        (request.select_options.count && request.select_selected >= request.select_options.count) ||
        ((request.check || request.swatch) && request.confirmation_only) || (request.swatch && !request.check))
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
        VirtualAlloc(nullptr, owner_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
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
            VirtualFree(form, 0, MEM_RELEASE);
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
    form->choice_selected = request.choice_selected;
    form->select_selected = request.select_selected;
    bool initialized = true;
    if (!request.confirmation_only) {
        initialized = secure_edit_initialize(
            &form->edit, request.maximum_bytes ? request.maximum_bytes : kApplicationTextFormBytes, false,
            {request.maximum_codepoints, !request.single_line});
        if (initialized) {
            const auto assigned = secure_edit_assign(&form->edit, request.initial);
            initialized = assigned == SecureEditResult::changed || assigned == SecureEditResult::unchanged;
            if (initialized)
                check_text(form);
        }
        initialized = initialized && secure_edit_enable_history(&form->edit, 32 * 1024);
        form->blink_ms = GetCaretBlinkTime();
        if (!form->blink_ms)
            form->blink_ms = 500;
        form->caret_visible = true;
    }
    reset_value(&form->request.initial);
    const auto discard = [&]() noexcept {
        text_form_release(&form->scene);
        if (!request.confirmation_only)
            secure_edit_release(&form->edit);
        VirtualFree(form, 0, MEM_RELEASE);
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
    if ((GetKeyState(VK_RETURN) & 0x8000) != 0)
        g_app.text_form_held_key = VK_RETURN;
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
        secure_text_store_set_window_focus(&form->store, GetFocus() == g_app.window);
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
    if ((GetKeyState(VK_RETURN) & 0x8000) != 0)
        g_app.text_form_held_key = VK_RETURN;
    else if ((GetKeyState(VK_SPACE) & 0x8000) != 0)
        g_app.text_form_held_key = VK_SPACE;
    g_app.text_form = nullptr;
    g_app.context_focus = nullptr;
    renderer_bind_text_form(&g_app.renderer, nullptr);
    if (login_owner)
        renderer_release_login_form_text(&g_app.renderer);
    end_capture(form);
    if (!form->request.confirmation_only)
        secure_text_store_release(&form->store);
    text_form_surface_release(&form->surface);
    text_form_release(&form->scene);
    if (!form->request.confirmation_only)
        secure_edit_release(&form->edit);
    SecureZeroMemory(form, sizeof(*form));
    VirtualFree(form, 0, MEM_RELEASE);
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
        return code != VK_F4 && code != VK_SPACE;
    if (kind == UiInputKind::system_character)
        return code != ' ';
    const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    const auto focused = text_form_focused(form->scene);
    if (kind == UiInputKind::key_down && code == VK_ESCAPE) {
        g_app.text_form_held_key = code;
        if (user_dismissible(*form))
            application_text_form_close();
        return true;
    }
    if (kind == UiInputKind::key_down && code == VK_TAB) {
        if (text_form_move_focus(&form->scene, shift))
            focus_field(form, text_form_focused(form->scene));
        return true;
    }
    if (focused != TextFormElement::edit || form->request.confirmation_only || !form->editable) {
        if (kind == UiInputKind::key_down || kind == UiInputKind::key_up) {
            const auto key = code == VK_RETURN  ? UiButtonKey::enter
                             : code == VK_SPACE ? UiButtonKey::space
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
    if (code == VK_RETURN && form->request.single_line) {
        if (!repeated && !form->composition.active) {
            g_app.text_form_held_key = code;
            activate(form, TextFormElement::accept);
        }
        return true;
    }
    if (ctrl && code == VK_RETURN) {
        if (!repeated) {
            g_app.text_form_held_key = code;
            activate(form, TextFormElement::accept);
            if (!g_app.text_form)
                g_app.text_form_held_key = code;
        }
        return true;
    }
    if (ctrl && (code == 'Z' || code == 'Y')) {
        // A text service owns its active composition. Let it finish before
        // replaying model history; consumed native keys never reach this path.
        if (form->composition.active)
            return true;
        return apply(form,
                     code == 'Y' || shift ? secure_edit_redo(&form->edit) : secure_edit_undo(&form->edit));
    }
    if (ctrl && code == 'A')
        return apply(form, secure_edit_select_all(&form->edit));
    if (ctrl && (code == 'C' || code == 'X')) {
        secure_edit_break_history_run(&form->edit);
        const auto first = minimum(form->edit.anchor, form->edit.caret),
                   last = maximum(form->edit.anchor, form->edit.caret);
        if (first != last &&
            clipboard_write_utf8(
                g_app.window,
                {reinterpret_cast<const char*>(form->edit.storage.data + first), last - first}) &&
            code == 'X')
            return apply(form, secure_edit_delete_forward(&form->edit));
        return true;
    }
    if (ctrl && code == 'V') {
        char pasted[kApplicationTextFormBytes]{};
        const auto read = clipboard_read_utf8(g_app.window, {pasted, sizeof(pasted)});
        const auto result = read.ok() ? secure_edit_insert_utf8(&form->edit, {pasted, read.bytes})
                            : read.error == ClipboardTextError::capacity ? SecureEditResult::capacity
                                                                         : SecureEditResult::unchanged;
        SecureZeroMemory(pasted, sizeof(pasted));
        return apply(form, result);
    }
    switch (code) {
    case VK_BACK:
        return apply(form, secure_edit_backspace(&form->edit, clock_monotonic_milliseconds()));
    case VK_DELETE:
        return apply(form, secure_edit_delete_forward(&form->edit, clock_monotonic_milliseconds()));
    case VK_LEFT:
        return apply(form, secure_edit_move(&form->edit, SecureEditMove::previous, shift));
    case VK_RIGHT:
        return apply(form, secure_edit_move(&form->edit, SecureEditMove::next, shift));
    case VK_HOME:
        return apply(
            form,
            secure_edit_move(&form->edit, ctrl ? SecureEditMove::home : SecureEditMove::line_home, shift));
    case VK_END:
        return apply(form, secure_edit_move(&form->edit,
                                            ctrl ? SecureEditMove::end : SecureEditMove::line_end, shift));
    case VK_UP:
    case VK_DOWN: {
        u32 position = 0;
        SecureEditUtf16Selection selection{};
        if (text_edit_layout_move_vertical(form->surface.layout, code == VK_DOWN, &position) &&
            secure_edit_utf16_selection(form->edit, &selection) == SecureEditResult::unchanged) {
            if (!shift)
                selection.anchor = position;
            selection.caret = position;
            return apply(form, secure_edit_set_utf16_selection(&form->edit, selection));
        }
        return true;
    }
    default:
        return true;
    }
}

bool application_text_form_filter_message(const MSG& message) noexcept {
    if (message.hwnd != g_app.window)
        return false;
    if (g_app.text_form_held_key && (message.message == WM_KEYDOWN || message.message == WM_KEYUP) &&
        message.wParam == g_app.text_form_held_key)
        return application_text_form_input(
            message.message == WM_KEYDOWN ? UiInputKind::key_down : UiInputKind::key_up,
            static_cast<u32>(message.wParam), (message.lParam & (1ll << 30)) != 0);
    auto* form = g_app.text_form;
    if (!form)
        return false;
    if (!valid(form)) {
        application_text_form_close(false);
        return false;
    }
    if (!settle(form))
        return false;
    return form->editable && !form->request.confirmation_only &&
           secure_text_store_filter_message(&form->store, message);
}

bool application_text_form_window_message(UINT message, WPARAM wparam, LPARAM lparam,
                                          LRESULT* result) noexcept {
    if (message == WM_KILLFOCUS || message == WM_CANCELMODE)
        g_app.text_form_held_key = 0;
    auto* form = g_app.text_form;
    if (!form || !result)
        return false;
    if (!valid(form)) {
        application_text_form_close(false);
        return false;
    }
    if (!settle(form))
        return true;
    *result = 0;
    const float scale = 96.0f / static_cast<float>(g_app.renderer.dpi);
    const float x = static_cast<float>(static_cast<short>(LOWORD(lparam))) * scale;
    const float y = static_cast<float>(static_cast<short>(HIWORD(lparam))) * scale;
    if (message == WM_KILLFOCUS) {
        if (user_dismissible(*form)) {
            application_text_form_close(false);
            return true;
        }
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
    if (message == WM_SETFOCUS) {
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
    if (message == WM_CAPTURECHANGED || message == WM_CANCELMODE) {
        if (message == WM_CANCELMODE)
            end_capture(form);
        else {
            form->dragging = false;
            form->pointer_button = ui_widget_invalid_handle();
            ui_widget_manager_capture(&form->scene.widgets, ui_widget_invalid_handle());
        }
        ui_button_keyboard_cancel(&form->keyboard);
        if (message == WM_CANCELMODE && !form->request.confirmation_only) {
            secure_text_store_unbind(&form->store);
            reset_value(&form->composition);
            secure_edit_cancel_composition(&form->edit);
            if (form->editable && text_form_focused(form->scene) == TextFormElement::edit) {
                secure_text_store_bind(&form->store, &form->edit, SecureTextInputScope::text);
                secure_text_store_set_window_focus(&form->store, GetFocus() == g_app.window);
            }
            reset_caret(form);
            refresh(form);
        }
        return !user_dismissible(*form);
    }
    switch (message) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const auto* panel = text_form_node(form->scene, TextFormElement::panel);
        if (panel && !ui_rect_contains(panel->rectangle, x, y)) {
            if (user_dismissible(*form))
                application_text_form_close();
            return true;
        }
        SetFocus(g_app.window);
        const auto element = text_form_hit_test(form->scene, x, y);
        u32 at = 0;
        if (element == TextFormElement::edit || element == TextFormElement::cancel ||
            element == TextFormElement::accept || element == TextFormElement::option ||
            element == TextFormElement::select || text_form_choice_index(element, &at) ||
            text_form_select_index(element, &at)) {
            if (!focus_field(form, element))
                return true;
            const auto handle = form->scene.elements[static_cast<u32>(element)];
            if (element == TextFormElement::edit && !form->request.confirmation_only) {
                // Double click: word; a third press within the double-click
                // time/distance: line. An IME composition keeps its selection.
                const u8 clicks = application_click_count(&form->scene, x, y, message == WM_LBUTTONDBLCLK);
                SecureTextComposition composition{};
                const bool composing =
                    secure_text_store_composition(form->store, &composition) && composition.active;
                if (!composing) {
                    pointer_position(form, x, y, clicks == 1 && (wparam & MK_SHIFT) != 0);
                    if (clicks == 2)
                        apply(form, secure_edit_select_word(&form->edit, form->edit.caret));
                    else if (clicks == 3)
                        apply(form, secure_edit_select_line(&form->edit, form->edit.caret));
                }
                form->dragging = clicks == 1 && !composing;
            } else
                form->pointer_button = handle;
            if (ui_widget_manager_capture(&form->scene.widgets, handle))
                SetCapture(g_app.window);
            if (GetCapture() != g_app.window)
                end_capture(form);
        }
        return true;
    }
    case WM_MOUSEMOVE: {
        if (form->dragging && (wparam & MK_LBUTTON))
            pointer_position(form, x, y, true);
        // Footer buttons and the option row light up only under the pointer.
        const auto element = text_form_hit_test(form->scene, x, y);
        u32 at = 0;
        const auto hovered = element == TextFormElement::cancel || element == TextFormElement::accept ||
                                     element == TextFormElement::option || element == TextFormElement::select ||
                                     text_form_choice_index(element, &at) || text_form_select_index(element, &at)
                                 ? form->scene.elements[static_cast<u32>(element)]
                                 : ui_widget_invalid_handle();
        const auto previous = form->scene.widgets.hovered;
        if (!ui_widget_handle_equal(previous, hovered)) {
            if (!ui_widget_manager_hover(&form->scene.widgets, hovered))
                ui_widget_manager_hover(&form->scene.widgets, ui_widget_invalid_handle());
            if (hovered.generation) {
                TRACKMOUSEEVENT tracking{static_cast<DWORD>(sizeof(TRACKMOUSEEVENT)), TME_LEAVE, g_app.window, 0};
                TrackMouseEvent(&tracking);
            }
            refresh(form);
        }
        return true;
    }
    case WM_MOUSELEAVE:
        if (form->scene.widgets.hovered.generation) {
            ui_widget_manager_hover(&form->scene.widgets, ui_widget_invalid_handle());
            refresh(form);
        }
        return false;
    case WM_LBUTTONUP: {
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
    case WM_MOUSEWHEEL: {
        POINT point{static_cast<short>(LOWORD(lparam)), static_cast<short>(HIWORD(lparam))};
        application_screen_to_content(&point);
        const float delta = -static_cast<float>(static_cast<short>(HIWORD(wparam))) / WHEEL_DELTA * 48;
        if (!form->request.confirmation_only &&
            text_form_hit_test(form->scene, point.x * scale, point.y * scale) == TextFormElement::edit)
            text_edit_layout_scroll(&form->surface.layout, delta);
        else
            text_form_scroll(&form->scene, delta);
        refresh(form);
        return true;
    }
    case WM_MOUSEHWHEEL:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
        return true;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) {
            // Buttons are hands and the editor an I-beam, from the widgets themselves.
            UiPoint point{};
            application_set_cursor(application_cursor_point(g_app.window, g_app.renderer.dpi, lparam, &point)
                                       ? ui_widget_manager_cursor(form->scene.widgets, point.x, point.y)
                                       : UiCursor::arrow);
            *result = TRUE;
            return true;
        }
        return false;
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
