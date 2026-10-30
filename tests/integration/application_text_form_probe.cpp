#include "probe_present.hpp"
#include "reaction_intent_ui_adapter.hpp"
#include "thread_membership_ui_adapter.hpp"
#include "reply_draft_ui_adapter.hpp"
#include "thread_creation_ui_adapter.hpp"
#include "../../src/app/application_profile.hpp"
#include "../../src/app/application_text_form_state.hpp"
#include "../../src/app/application_composer_text_input_state.hpp"
#include "../../src/app/application_state.hpp"
#include "../../src/app/application_button_input.hpp"
#include "../../src/app/application_account_presentation.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/log.hpp"
#include "maicord/ui/settings_scene.hpp"
#include "../../src/app/account_ui_runtime.hpp"

#include "maicord/core/win32_user.hpp"
#include "maicord/sys/winsock.hpp"
#include "maicord/ui/friends_scene.hpp"

namespace maicord::app_detail {
u32 probe_button_actions;
// This fixture owns the shared native editor host, without linking the account
// Friends controller. These bounded switches model only its owner contract.
MessageComposer* probe_friends_search;
bool probe_friends_visible;
bool probe_friends_requests_visible;
bool application_friends_active() noexcept {
    return probe_friends_visible;
}
MessageComposer* application_friends_search_model() noexcept {
    return probe_friends_search;
}
bool application_friends_search_focused() noexcept {
    return probe_friends_visible && !probe_friends_requests_visible && probe_friends_search &&
           probe_friends_search->focused;
}
bool application_friends_search_changed(bool) noexcept {
    return true;
}
TextInputTarget application_friends_search_target() noexcept {
    return application_friends_search_focused() ? message_composer_input_target(probe_friends_search)
                                                : TextInputTarget{};
}
void application_friends_search_cancel_native_group(const TextInputTarget& target) noexcept {
    if (probe_friends_search && target == message_composer_input_target(probe_friends_search)) {
        text_input_end_group(target);
        text_input_cancel(target);
    }
}
bool handle_composer_click(LPARAM) noexcept {
    ++probe_button_actions;
    return true;
}
// No voice message recording in this probe.
void application_voice_message_pointer(UiButtonPointerEvent, bool, bool) noexcept {
}
bool handle_shell_click(LPARAM) noexcept {
    ++probe_button_actions;
    return true;
}
float client_dip_x(LPARAM value) noexcept {
    return static_cast<short>(LOWORD(value)) * 96.0f / g_app.renderer.dpi;
}
float client_dip_y(LPARAM value) noexcept {
    return static_cast<short>(HIWORD(value)) * 96.0f / g_app.renderer.dpi;
}
// The shipping form host runs against the probe's native renderer/window.
// These unrelated conversation side effects are outside this form fixture.
bool stop_local_typing(u64) noexcept {
    return true;
}
bool composer_apply_result(MessageComposerResult, bool) noexcept {
    return true;
}
bool application_profile_refresh_message() noexcept {
    return true;
}
MessageComposer* application_profile_edit_model() noexcept {
    return g_app.profile ? &g_app.profile->message : nullptr;
}
bool application_profile_edit_focused() noexcept {
    return g_app.profile && g_app.profile->message.focused && !g_app.profile->message_pending;
}
void reset_message_composer_caret_timing(bool visible) noexcept {
    g_app.message_composer_caret_visible = visible;
}
void end_message_text_selection() noexcept {
}
void end_message_composer_pointer_selection() noexcept {
}
void end_scrollbar_drag() noexcept {
}
void end_pane_resize() noexcept {
}
void set_popup_owner(PopupOwnerKind owner) noexcept {
    g_app.popup_owner_kind = owner;
}
UiRect application_viewport_dips() noexcept {
    const float scale = 96.0f / static_cast<float>(g_app.renderer.dpi);
    return {0, 0, g_app.renderer.width * scale, g_app.renderer.height * scale};
}
bool refresh_account_frame(bool force) noexcept {
    return refresh_account_text(force);
}
bool application_refresh_login_view() noexcept {
    // This account/editor fixture has no login display owner. Startup recovery
    // rendering is covered by the dedicated account logout native fixture.
    return false;
}
bool refresh_account_text(bool) noexcept {
    if (thread_creation_ui_adapter_active())
        return thread_creation_ui_adapter_refresh();
    if (reply_draft_ui_adapter_active())
        return reply_draft_ui_adapter_refresh();
    if (thread_membership_ui_adapter_active())
        return thread_membership_ui_adapter_refresh();
    if (reaction_intent_ui_adapter_active())
        return reaction_intent_ui_adapter_refresh();
    return renderer_rebuild_content_text(&g_app.renderer, kDefaultTheme);
}
bool refresh_message_composer_ui() noexcept {
    return refresh_account_text(true);
}
} // namespace maicord::app_detail
namespace maicord {
namespace {
struct Fixture {
    bool valid, reject;
    u32 saves, bytes;
    char saved[4096];
    app_detail::ApplicationTextFormProgress progress;
};
Fixture* fixture;
bool owner_valid(app_detail::TextFormOwnerKey key) noexcept {
    return fixture && fixture->valid && key.scope == 1 && key.collection == 2 && key.item == 3;
}
app_detail::ApplicationTextFormProgress owner_progress(app_detail::TextFormOwnerKey) noexcept {
    return fixture->progress;
}
bool save(app_detail::TextFormOwnerKey key, Utf8View text) noexcept {
    if (!owner_valid(key))
        return false;
    ++fixture->saves;
    if (fixture->reject || text.bytes > sizeof(fixture->saved))
        return false;
    copy_bytes(fixture->saved, text.data, text.bytes);
    fixture->bytes = text.bytes;
    return true;
}
bool fixture_input(UiInputKind kind, u32 code, bool repeated, bool ctrl = false,
                   bool shift = false) noexcept {
    BYTE original[256]{}, state[256]{};
    if (!GetKeyboardState(original))
        return false;
    state[VK_CONTROL] = ctrl ? 0x80 : 0;
    state[VK_SHIFT] = shift ? 0x80 : 0;
    if (!SetKeyboardState(state))
        return false;
    const bool handled = app_detail::application_text_form_input(kind, code, repeated);
    const bool restored = SetKeyboardState(original) != FALSE;
    return handled && restored;
}
bool history_key(u32 key, bool shift = false) noexcept {
    return fixture_input(UiInputKind::key_down, key, false, true, shift);
}
bool fixture_open(const app_detail::ApplicationTextFormRequest& request) noexcept {
    BYTE original[256]{}, state[256]{};
    if (!GetKeyboardState(original) || !SetKeyboardState(state))
        return false;
    const bool opened = app_detail::application_text_form_open(request);
    const bool restored = SetKeyboardState(original) != FALSE;
    return opened && restored;
}
} // namespace
bool application_text_form_probe(D3D11Renderer& renderer, UiTextMeasureCache& cache, HWND window) noexcept {
    using namespace app_detail;
    auto* ui = static_cast<AccountUiRuntime*>(
        VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!ui)
        return false;
    // Synthetic key events need a deterministic thread keyboard state. A real
    // held Shift/Ctrl/Enter must not change the meaning of a fixture command.
    BYTE original_keyboard[256]{}, fixture_keyboard[256]{};
    if (!GetKeyboardState(original_keyboard) || !SetKeyboardState(fixture_keyboard)) {
        VirtualFree(ui, 0, MEM_RELEASE);
        return false;
    }
    // Move renderer ownership for this synchronous fixture, then restore it.
    // Rebase self-referential geometry pointers on each transfer.
    g_app.renderer = renderer;
    renderer = {};
    g_app.renderer.composer_layout.selections = g_app.renderer.edit_selections;
    g_app.renderer.login_edit.selections = g_app.renderer.edit_selections;
    g_app.renderer.edit_selection_owner = &g_app.renderer.composer_layout;
    ui->text_measure_cache = cache;
    ui->text_measure_cache.backend_context = &g_app.renderer;
    g_app.account_ui = ui;
    g_app.window = window;
    g_app.renderer_ready = true;
    Fixture local{true, false};
    fixture = &local;
    UiWidget widgets[2]{};
    UiLayoutNode nodes[2]{};
    auto& previous = g_app.message_composer_scene.widgets;
    bool valid = ui_widget_manager_initialize(&previous, {widgets, 2}, {nodes, 2});
    auto style = ui_layout_style();
    style.width = ui_layout_fill();
    style.height = ui_layout_fixed(30);
    const auto old = ui_widget_manager_add(&previous, ui_widget_invalid_handle(),
                                           {1,
                                            UiWidgetKind::text_field,
                                            ui_widget_enabled | ui_widget_hit_test | ui_widget_focusable,
                                            {utf8_literal("Previous focus"), style}});
    valid = valid && old.ok() && ui_widget_manager_layout_rectangle(&previous, {0, 0, 300, 40}).ok() &&
            ui_widget_manager_focus(&previous, old.handle);
    g_app.context_focus = &previous;
    g_app.message_composer.focused = true;
    char initial[] = "before", name[] = "photo.png";
    ApplicationTextFormRequest request{utf8_literal("Description"),
                                       {name, 9},
                                       utf8_literal("Describe the attachment"),
                                       utf8_literal("Save"),
                                       {initial, 6},
                                       {1, 2, 3},
                                       owner_valid,
                                       save,
                                       1024};
    valid = valid && fixture_open(request) && g_app.text_form;
    if (!valid)
        log_write(LogLevel::error, "probe", "application_text_form_open_failed");
    initial[0] = 'X';
    name[0] = 'X';
    valid = valid && g_app.text_form && g_app.text_form->edit.storage.data[0] == 'b' &&
            g_app.text_form->request.context.data[0] == 'p' &&
            g_app.context_focus == &g_app.text_form->scene.widgets;
    valid = valid && fixture_input(UiInputKind::character, '!', false) &&
            fixture_input(UiInputKind::character, '\r', false) &&
            fixture_input(UiInputKind::character, 0xD83D, false) &&
            fixture_input(UiInputKind::character, 0xDE00, false) && local.saves == 0 &&
            probe_present(&g_app.renderer, kDefaultTheme);
    valid = valid && g_app.text_form && g_app.text_form->edit.storage.bytes == 12 && history_key('Z') &&
            g_app.text_form->edit.storage.bytes == 8 && history_key('Y') &&
            g_app.text_form->edit.storage.bytes == 12 && history_key('Z') && history_key('Z', true) &&
            g_app.text_form->edit.storage.bytes == 12 && local.saves == 0;
    if (!valid) {
        log_write(LogLevel::error, "probe", "application_text_form_unicode_history_failed");
        if (g_app.text_form) {
            log_u64(LogLevel::error, "probe", "form_failure_bytes", g_app.text_form->edit.storage.bytes);
            log_u64(LogLevel::error, "probe", "form_failure_undo",
                    secure_edit_history_costs(g_app.text_form->edit).undo_steps);
            log_u64(LogLevel::error, "probe", "form_failure_redo",
                    secure_edit_history_costs(g_app.text_form->edit).redo_steps);
            log_u64(LogLevel::error, "probe", "form_failure_composition",
                    g_app.text_form->composition.active);
            log_u64(LogLevel::error, "probe", "form_failure_ctrl",
                    static_cast<u16>(GetKeyState(VK_CONTROL)));
        }
    }
    valid = valid && fixture_input(UiInputKind::character, 'A', false) &&
            fixture_input(UiInputKind::character, 'B', false) &&
            g_app.text_form->edit.storage.bytes == 14 && history_key('Z') &&
            g_app.text_form->edit.storage.bytes == 12 && history_key('Y') &&
            g_app.text_form->edit.storage.bytes == 14 && history_key('Z') &&
            g_app.text_form->edit.storage.bytes == 12;
    valid = valid && fixture_input(UiInputKind::key_down, VK_ESCAPE, false) && !g_app.text_form &&
            local.saves == 0 && g_app.context_focus == &previous && g_app.message_composer.focused &&
            fixture_input(UiInputKind::key_up, VK_ESCAPE, false);
    // The shipping host consumes one outside/focus dismissal, preserving its parent.
    valid = valid && fixture_open(request);
    LRESULT dismissed = 0;
    const u32 before_dismiss_saves = local.saves;
    valid = valid &&
            application_text_form_window_message(WM_LBUTTONDOWN, 0, MAKELPARAM(1, 1), &dismissed) &&
            !g_app.text_form && local.saves == before_dismiss_saves && g_app.context_focus == &previous;
    valid = valid && fixture_open(request) &&
            application_text_form_window_message(WM_KILLFOCUS, 0, 0, &dismissed) && !g_app.text_form &&
            local.saves == before_dismiss_saves;
    initial[0] = 'b';
    name[0] = 'p';
    local.reject = true;
    if (!valid)
        log_write(LogLevel::error, "probe", "application_text_form_restore_focus_failed");
    valid = valid && fixture_open(request) && fixture_input(UiInputKind::character, 'z', false) &&
            fixture_input(UiInputKind::key_down, VK_TAB, false) &&
            fixture_input(UiInputKind::key_down, VK_TAB, false) &&
            fixture_input(UiInputKind::key_down, VK_RETURN, false) && g_app.text_form &&
            g_app.text_form->status.bytes && local.saves == 1;
    valid = valid && fixture_input(UiInputKind::key_down, VK_RETURN, true) && local.saves == 1 &&
            fixture_input(UiInputKind::key_up, VK_RETURN, false);
    local.reject = false;
    valid = valid && fixture_input(UiInputKind::key_down, VK_RETURN, false) && !g_app.text_form &&
            local.saves == 2 && local.bytes == 7 && bytes_equal(local.saved, "beforez", 7) &&
            fixture_input(UiInputKind::key_down, VK_RETURN, true) &&
            fixture_input(UiInputKind::character, '\r', false) &&
            fixture_input(UiInputKind::key_up, VK_RETURN, false) &&
            !fixture_input(UiInputKind::key_down, VK_RETURN, false);
    char lines[256]{};
    if (!valid) {
        log_write(LogLevel::error, "probe", "application_text_form_save_retry_failed");
        log_u64(LogLevel::error, "probe", "form_failure_saves", local.saves);
        log_u64(LogLevel::error, "probe", "form_failure_saved_bytes", local.bytes);
        log_u64(LogLevel::error, "probe", "form_failure_held", g_app.text_form_held_key);
        log_u64(LogLevel::error, "probe", "form_failure_shift", static_cast<u16>(GetKeyState(VK_SHIFT)));
        log_u64(LogLevel::error, "probe", "form_failure_ctrl", static_cast<u16>(GetKeyState(VK_CONTROL)));
        if (g_app.text_form) {
            log_u64(LogLevel::error, "probe", "form_failure_focus",
                    static_cast<u32>(text_form_focused(g_app.text_form->scene)));
            log_u64(LogLevel::error, "probe", "form_failure_bytes", g_app.text_form->edit.storage.bytes);
        }
    }
    for (u32 i = 0; i < sizeof(lines); ++i)
        lines[i] = i % 2 ? '\n' : 'a';
    request.initial = {lines, sizeof(lines)};
    valid = valid && fixture_open(request);
    if (valid && g_app.text_form) {
        auto* form = g_app.text_form;
        const auto* field = text_form_node(form->scene, TextFormElement::edit);
        const float scale = static_cast<float>(g_app.renderer.dpi) / 96;
        POINT point{static_cast<LONG>((field->content_rectangle.left + 8) * scale),
                    static_cast<LONG>((field->content_rectangle.top + 8) * scale)};
        ClientToScreen(window, &point);
        const float before_scroll = form->surface.layout.scroll_y;
        const u32 before_caret = form->edit.caret;
        LRESULT result = 0;
        valid = before_scroll > 0 &&
                application_text_form_window_message(WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA),
                                                     MAKELPARAM(point.x, point.y), &result) &&
                form->surface.layout.scroll_y < before_scroll && form->edit.caret == before_caret &&
                fixture_input(UiInputKind::key_down, VK_END, false) &&
                form->surface.layout.scroll_y == before_scroll;
    }
    local.valid = false;
    if (!valid)
        log_write(LogLevel::error, "probe", "application_text_form_scroll_failed");
    valid =
        fixture_input(UiInputKind::character, 'Q', false) && !g_app.text_form && local.saves == 2 && valid;
    local.valid = true;
    request.initial = utf8_literal("before");
    request.maximum_codepoints = 6;
    valid = valid && fixture_open(request);
    if (valid && g_app.text_form) {
        const auto revision = g_app.text_form->edit.revision;
        valid = fixture_input(UiInputKind::character, 'x', false) &&
                g_app.text_form->edit.revision == revision && g_app.text_form->edit.storage.bytes == 6 &&
                g_app.text_form->status.bytes;
    }
    application_text_form_close(false, false);
    if (!valid)
        log_write(LogLevel::error, "probe", "application_text_form_invalidation_or_capacity_failed");
    {
        // Exercise the ordinary native editor's transient retirement as well
        // as the editor-free committed confirmation runtime fixture.
        request.progress = owner_progress;
        local.reject = true;
        local.progress = {{}, {}, 1, true, true, false, true, true};
        valid = valid && fixture_open(request) && fixture_input(UiInputKind::character, 0xD83D, false);
        auto* form = g_app.text_form;
        if (valid && form) {
            valid = form->edit.pending_high_surrogate == 0xD83D && valid;
            local.progress = {utf8_literal("Finishing action…"),
                              utf8_literal("Retry"),
                              2,
                              false,
                              false,
                              false,
                              false,
                              false};
            valid = application_text_form_synchronize() && g_app.text_form == form &&
                    !form->edit.pending_high_surrogate && !form->composition.active && !form->can_cancel &&
                    !form->dismissible && text_form_focused(form->scene) == TextFormElement::count && valid;
            const auto revision = form->edit.revision;
            const u32 saves = local.saves;
            LRESULT consumed = 0;
            valid = fixture_input(UiInputKind::key_down, VK_ESCAPE, false) &&
                    fixture_input(UiInputKind::key_up, VK_ESCAPE, false) &&
                    application_text_form_window_message(WM_LBUTTONDOWN, 0, MAKELPARAM(1, 1), &consumed) &&
                    application_text_form_window_message(WM_LBUTTONUP, 0, MAKELPARAM(1, 1), &consumed) &&
                    application_text_form_window_message(WM_KILLFOCUS, 0, 0, &consumed) &&
                    application_text_form_window_message(WM_CANCELMODE, 0, 0, &consumed) &&
                    g_app.text_form == form && fixture_input(UiInputKind::character, 'x', false) &&
                    form->edit.revision == revision && form->edit.storage.bytes == 6 &&
                    local.saves == saves && !form->keyboard.pressed.manager &&
                    !form->pointer_button.generation && !form->scene.widgets.captured.generation && valid;
            local.progress.can_accept = true;
            local.progress.status = utf8_literal("Could not finish. Retry to continue.");
            ++local.progress.revision;
            valid = application_text_form_synchronize() &&
                    text_form_focused(form->scene) == TextFormElement::accept &&
                    fixture_input(UiInputKind::key_down, VK_RETURN, true) && local.saves == saves &&
                    fixture_input(UiInputKind::key_up, VK_RETURN, false) &&
                    fixture_input(UiInputKind::key_down, VK_RETURN, false) && local.saves == saves + 1 &&
                    fixture_input(UiInputKind::key_down, VK_RETURN, true) && local.saves == saves + 1 &&
                    fixture_input(UiInputKind::key_up, VK_RETURN, false) && g_app.text_form == form &&
                    valid;
            local.valid = false;
            valid = application_text_form_synchronize() && !g_app.text_form && valid;
            local.valid = true;
        }
        application_text_form_close(false, false);
        request.progress = nullptr;
        local.reject = false;
        if (!valid)
            log_write(LogLevel::error, "probe", "application_text_form_locked_native_lifecycle_failed");
    }
    {
        // Exercise the shipping native input owner on the probe's HWND. No
        // account requests are issued by these synthetic editor fixtures.
        message_composer_scene_release(&g_app.message_composer_scene);
        char draft[64]{};
        valid = message_composer_initialize(&g_app.message_composer, {draft, sizeof(draft)}) &&
                message_composer_scene_initialize_owned(&g_app.message_composer_scene) && valid;
        message_composer_reset(&g_app.message_composer, 123, false);
        message_composer_insert_utf8(&g_app.message_composer, utf8_literal("native draft"));
        g_app.message_composer.focused = true;
        valid = message_composer_scene_update(&g_app.message_composer_scene, &g_app.message_composer,
                                              application_viewport_dips(), true)
                    .ok() &&
                message_composer_scene_focus_edit(&g_app.message_composer_scene) && valid;
        const HWND prior_focus = SetFocus(window);
        valid = GetFocus() == window && application_composer_text_input_synchronize() &&
                g_app.composer_text_input && secure_text_store_attached(g_app.composer_text_input->store) &&
                !g_app.composer_text_input->failed && valid;
        request.maximum_codepoints = 1024;
        valid = fixture_open(request) && g_app.composer_text_input &&
                !secure_text_store_attached(g_app.composer_text_input->store) &&
                !g_app.composer_text_input->target && valid;
        application_text_form_close();
        valid = application_composer_text_input_synchronize() && g_app.composer_text_input &&
                secure_text_store_attached(g_app.composer_text_input->store) && valid;
        if (g_app.composer_text_input) {
            g_app.composer_text_input->held_key = VK_RETURN;
            MSG repeat{};
            repeat.hwnd = window;
            repeat.message = WM_KEYDOWN;
            repeat.wParam = VK_RETURN;
            repeat.lParam = 1ll << 30;
            valid = application_composer_text_input_filter(repeat) && valid;
            repeat.message = WM_KEYUP;
            valid = application_composer_text_input_filter(repeat) &&
                    !g_app.composer_text_input->held_key && valid;
            const auto old_target = g_app.composer_text_input->target;
            message_composer_restore(&g_app.message_composer, 123, utf8_literal("replacement"), 11, 11);
            valid = !text_input_ready(old_target) && application_composer_text_input_synchronize() &&
                    text_input_ready(g_app.composer_text_input->target) &&
                    !application_composer_text_input_composing() && valid;
        }
        auto* profile = static_cast<ApplicationProfile*>(
            VirtualAlloc(nullptr, sizeof(ApplicationProfile), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        valid = profile && valid;
        if (profile) {
            g_app.profile = profile;
            valid = message_composer_initialize_owned(&profile->message) && valid;
            profile->message.focused = true;
            profile->scene.active = true;
            profile->scene.edit.active = true;
            profile->scene.edit_content = {20, 20, 300, 60};
            valid = application_composer_text_input_synchronize() && g_app.composer_text_input &&
                    g_app.composer_text_input->target == message_composer_input_target(&profile->message) &&
                    valid;
            const u16 typed[] = {'t', 'e', 's', 't', ' ', 0xD83D, 0xDE00};
            TextInputChange change{};
            if (g_app.composer_text_input)
                valid = text_input_replace(g_app.composer_text_input->target, 0, 0, {typed, 7}, &change) ==
                            TextInputResult::changed &&
                        profile->message.bytes == 9 &&
                        utf8_equal(message_composer_text(g_app.message_composer),
                                   utf8_literal("replacement")) &&
                        valid;
            application_composer_text_input_suspend();
            profile->scene.active = false;
            message_composer_release(&profile->message);
            g_app.profile = nullptr;
            VirtualFree(profile, 0, MEM_RELEASE);
            valid = application_composer_text_input_synchronize() && g_app.composer_text_input &&
                    g_app.composer_text_input->target ==
                        message_composer_input_target(&g_app.message_composer) &&
                    valid;
        }
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "profile_native_text_target_handoff_passed"
                        : "profile_native_text_target_handoff_failed");
        {
            char search_bytes[128]{};
            MessageComposer search{};
            valid = message_composer_initialize(&search, {search_bytes, sizeof(search_bytes)}) && valid;
            search.focused = true;
            probe_friends_search = &search;
            probe_friends_visible = true;
            valid = application_composer_text_input_synchronize() && g_app.composer_text_input &&
                    g_app.composer_text_input->target == message_composer_input_target(&search) && valid;
            const u16 unicode[] = {0x017D, 'i', 'v', 'a', ' ', 0xD83D, 0xDE00};
            TextInputChange change{};
            if (g_app.composer_text_input) {
                const auto old_target = g_app.composer_text_input->target;
                valid = text_input_replace(old_target, 0, 0, {unicode, 7}, &change) ==
                            TextInputResult::changed &&
                        search.bytes == 10 &&
                        utf8_equal(message_composer_text(g_app.message_composer),
                                   utf8_literal("replacement")) &&
                        valid;
                message_composer_clear(&search);
                valid = !text_input_ready(old_target) && application_composer_text_input_synchronize() &&
                        text_input_ready(g_app.composer_text_input->target) && valid;
                const auto grouped = g_app.composer_text_input->target;
                valid =
                    message_composer_enable_history(&search, 2048) && text_input_begin_group(grouped) &&
                    text_input_replace(grouped, 0, 0, {unicode, 7}, &change) == TextInputResult::changed &&
                    valid;
                // Eligibility can disappear before native detach. The shared
                // owner must still close the old identity's history group.
                search.focused = false;
                valid = application_composer_text_input_synchronize() &&
                        !g_app.composer_text_input->target &&
                        message_composer_undo(&search) == MessageComposerResult::changed && !search.bytes &&
                        valid;
                search.focused = true;
                valid = application_composer_text_input_synchronize() && valid;
            }
            valid = fixture_open(request) && g_app.composer_text_input &&
                    !g_app.composer_text_input->target && valid;
            application_text_form_close();
            valid = application_composer_text_input_synchronize() && g_app.composer_text_input &&
                    g_app.composer_text_input->target == message_composer_input_target(&search) && valid;
            {
                // Bind the actual second native scene while the shared TSF
                // host still owns a search group. Modal eligibility changes
                // before detach; old identity cleanup must still run.
                UiFriendsScene main_scene{}, requests_scene{};
                struct RestoreScenes {
                    UiFriendsScene& main;
                    UiFriendsScene& requests;
                    const UiFriendsScene* old_main;
                    const UiFriendsScene* old_requests;
                    UiWidgetManager* old_context;
                    const UiWidgetManager* old_renderer_focus;
                    bool old_composer_focus;
                    ~RestoreScenes() noexcept {
                        application_composer_text_input_suspend();
                        probe_friends_requests_visible = false;
                        g_app.context_focus = old_context;
                        g_app.renderer.focus_owner = old_renderer_focus;
                        g_app.message_composer.focused = old_composer_focus;
                        renderer_bind_friends_requests_scene(&g_app.renderer, old_requests);
                        renderer_bind_friends_scene(&g_app.renderer, old_main);
                        ui_friends_scene_release(&requests);
                        ui_friends_scene_release(&main);
                    }
                } restore_scenes{main_scene,
                                 requests_scene,
                                 g_app.renderer.friends_scene,
                                 g_app.renderer.friends_requests_scene,
                                 g_app.context_focus,
                                 g_app.renderer.focus_owner,
                                 g_app.message_composer.focused};
                const auto checkpoint = [&](bool success, const char* label) noexcept {
                    if (!success)
                        log_write(LogLevel::error, "probe", label);
                    valid = success && valid;
                };
                UiFriendsSource empty{nullptr, 0, 1,
                                      +[](void*, u32, UiFriendsRow*) noexcept { return false; }};
                UiFriendsSections hidden{nullptr, 0, 1, true, true, 0};
                UiFriendsContent content{
                    {message_composer_text(search), search.anchor, search.caret, true, true, false}, 0, 1};
                checkpoint(ui_friends_scene_update(&main_scene, &empty, UiFriendsTab::pending,
                                                   UiFriendsPhase::ready, {}, application_viewport_dips(),
                                                   &content) &&
                               ui_friends_scene_update(&requests_scene, &empty, UiFriendsTab::pending,
                                                       UiFriendsPhase::ready, {},
                                                       application_viewport_dips(), nullptr, &hidden) &&
                               ui_friends_scene_focus(
                                   &main_scene, {UiFriendsAction::search, 0, UiFriendsTab::pending}) &&
                               ui_friends_scene_focus(&requests_scene, {UiFriendsAction::toggle_hidden, 0,
                                                                        UiFriendsTab::pending}),
                           "requests_tsf_scene_owners_failed");
                renderer_bind_friends_scene(&g_app.renderer, &main_scene);
                g_app.context_focus = &main_scene.widgets;
                g_app.renderer.focus_owner = &main_scene.widgets;
                checkpoint(application_composer_text_input_synchronize() && g_app.composer_text_input &&
                               g_app.composer_text_input->target == message_composer_input_target(&search),
                           "requests_tsf_search_attach_failed");
                if (g_app.composer_text_input) {
                    const auto old_search = g_app.composer_text_input->target;
                    const u16 modal_text[]{'m', 'o', 'd', 'a', 'l'};
                    TextInputChange requests_change{};
                    checkpoint(text_input_begin_group(old_search) &&
                                   text_input_replace(old_search, 0, 0, {modal_text, 5},
                                                      &requests_change) == TextInputResult::changed,
                               "requests_tsf_search_group_failed");
                    probe_friends_requests_visible = true;
                    renderer_bind_friends_requests_scene(&g_app.renderer, &requests_scene);
                    g_app.context_focus = &requests_scene.widgets;
                    g_app.renderer.focus_owner = &requests_scene.widgets;
                    checkpoint(application_composer_text_input_synchronize() &&
                                   !g_app.composer_text_input->target &&
                                   !secure_text_store_attached(g_app.composer_text_input->store) &&
                                   !application_friends_search_target() && search.focused,
                               "requests_tsf_modal_detach_failed");
                    checkpoint(message_composer_undo(&search) == MessageComposerResult::changed &&
                                   !search.bytes,
                               "requests_tsf_closed_group_undo_failed");
                    content.search.focused = false;
                    ++content.revision;
                    checkpoint(ui_friends_scene_update(&main_scene, &empty, UiFriendsTab::pending,
                                                       UiFriendsPhase::ready, {},
                                                       application_viewport_dips(), &content) &&
                                   renderer_rebuild_content_text(&g_app.renderer, kDefaultTheme) &&
                                   !g_app.renderer.composer_layout.active,
                               "requests_tsf_hidden_layout_failed");
                    const u32 hidden_chat_revision = g_app.message_composer.text_revision;
                    auto confirmation = request;
                    confirmation.title = utf8_literal("Clear other requests");
                    confirmation.context = utf8_literal("Clear ignored and spam requests?");
                    confirmation.label = {};
                    confirmation.initial = {};
                    confirmation.maximum_codepoints = 0;
                    confirmation.confirmation_only = true;
                    checkpoint(fixture_open(confirmation) && g_app.text_form && g_app.composer_text_input &&
                                   !g_app.composer_text_input->target &&
                                   !g_app.text_form->surface.layout.active,
                               "requests_tsf_confirmation_cover_failed");
                    application_text_form_close();
                    checkpoint(application_composer_text_input_synchronize() &&
                                   !g_app.composer_text_input->target &&
                                   g_app.message_composer.text_revision == hidden_chat_revision &&
                                   g_app.context_focus == &requests_scene.widgets,
                               "requests_tsf_modal_restore_failed");
                    renderer_bind_friends_requests_scene(&g_app.renderer, nullptr);
                    probe_friends_requests_visible = false;
                    g_app.context_focus = &main_scene.widgets;
                    g_app.renderer.focus_owner = &main_scene.widgets;
                    checkpoint(application_composer_text_input_synchronize() &&
                                   g_app.composer_text_input->target ==
                                       message_composer_input_target(&search),
                               "requests_tsf_search_reattach_failed");
                }
                log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                          valid ? "friends_requests_native_scene_tsf_detach_group_and_cover_passed"
                                : "friends_requests_native_scene_tsf_detach_group_and_cover_failed");
            }
            application_composer_text_input_suspend();
            probe_friends_visible = false;
            probe_friends_search = nullptr;
            message_composer_release(&search);
            valid = application_composer_text_input_synchronize() && g_app.composer_text_input &&
                    g_app.composer_text_input->target ==
                        message_composer_input_target(&g_app.message_composer) &&
                    valid;
            log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                      valid ? "friends_native_text_target_identity_modal_handoff_passed"
                            : "friends_native_text_target_identity_modal_handoff_failed");
        }
        application_composer_text_input_release();
        valid = !g_app.composer_text_input && !g_app.message_composer_scene.composition.count && valid;
        const auto* button_node =
            message_composer_scene_layout_node(g_app.message_composer_scene, MessageComposerControl::emoji);
        const float scale = g_app.renderer.dpi / 96.0f;
        const LPARAM pointer =
            button_node ? MAKELPARAM(static_cast<u16>((button_node->rectangle.left + 16) * scale),
                                     static_cast<u16>((button_node->rectangle.top + 16) * scale))
                        : 0;
        probe_button_actions = 0;
        valid = button_node && application_button_pointer(WM_LBUTTONDOWN, pointer) &&
                probe_button_actions == 0 && GetCapture() == window &&
                application_button_pointer(WM_LBUTTONUP, pointer) && probe_button_actions == 1 &&
                GetCapture() != window && !g_app.button_pointer_owner && valid;
        {
            UiSettingsScene cover{};
            cover.active = true;
            const auto* previous_settings = g_app.renderer.settings_scene;
            g_app.renderer.settings_scene = &cover;
            const auto* edit = message_composer_scene_layout_node(g_app.message_composer_scene,
                                                                  MessageComposerControl::edit);
            const LPARAM text_pointer =
                edit ? MAKELPARAM(static_cast<u16>((edit->rectangle.left + 16) * scale),
                                  static_cast<u16>((edit->rectangle.top + 16) * scale))
                     : 0;
            valid = edit && application_button_pointer(WM_MOUSEMOVE, pointer) &&
                    application_button_pointer(WM_LBUTTONDOWN, pointer) &&
                    application_button_pointer(WM_LBUTTONUP, pointer) &&
                    application_button_pointer(WM_LBUTTONDOWN, text_pointer) &&
                    application_button_pointer(WM_LBUTTONUP, text_pointer) && probe_button_actions == 1 &&
                    !g_app.button_pointer_owner && GetCapture() != window && valid;
            g_app.renderer.settings_scene = previous_settings;
            valid = application_button_pointer(WM_LBUTTONDOWN, pointer) && valid;
            g_app.renderer.settings_scene = &cover;
            valid = application_button_pointer(WM_LBUTTONUP, pointer) && probe_button_actions == 1 &&
                    !g_app.button_pointer_owner && GetCapture() != window && valid;
            g_app.renderer.settings_scene = previous_settings;
            log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                      valid ? "settings_native_cover_blocks_buttons_text_and_prior_capture_passed"
                            : "settings_native_cover_blocks_buttons_text_and_prior_capture_failed");
        }
        valid = application_button_pointer(WM_LBUTTONDOWN, pointer) && valid;
        auto* emoji = ui_widget_manager_resolve(&g_app.message_composer_scene.widgets,
                                                g_app.message_composer_scene.emoji);
        if (emoji)
            emoji->flags &= ~ui_widget_enabled;
        ui_widget_manager_reconcile_input(&g_app.message_composer_scene.widgets);
        valid = application_button_pointer(WM_LBUTTONUP, pointer) && probe_button_actions == 1 &&
                GetCapture() != window && !g_app.button_pointer_owner && valid;
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "shipping_button_host_release_and_disabled_capture_passed"
                        : "shipping_button_host_failed");
        message_composer_scene_release(&g_app.message_composer_scene);
        message_composer_release(&g_app.message_composer);
        SetFocus(prior_focus);
        log_write(valid ? LogLevel::info : LogLevel::error, "probe",
                  valid ? "composer_native_owner_modal_handoff_passed"
                        : "composer_native_owner_modal_handoff_failed");
    }
    fixture = nullptr;
    cache = ui->text_measure_cache;
    cache.backend_context = &renderer;
    renderer = g_app.renderer;
    g_app.renderer = {};
    renderer.composer_layout.selections = renderer.edit_selections;
    renderer.login_edit.selections = renderer.edit_selections;
    renderer.edit_selection_owner = &renderer.composer_layout;
    VirtualFree(ui, 0, MEM_RELEASE);
    g_app.account_ui = nullptr;
    g_app.window = nullptr;
    g_app.renderer_ready = false;
    g_app.context_focus = nullptr;
    g_app.message_composer_scene = {};
    g_app.message_composer = {};
    g_app.text_form_held_key = 0;
    valid = SetKeyboardState(original_keyboard) != FALSE && valid;
    valid = renderer_rebuild_content_text(&renderer, kDefaultTheme) &&
            probe_present(&renderer, kDefaultTheme) && valid;
    log_write(valid ? LogLevel::info : LogLevel::error, "probe",
              valid ? "application_text_form_passed" : "application_text_form_failed");
    return valid;
}
} // namespace maicord
