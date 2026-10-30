#include "thread_creation_ui_adapter.hpp"
#include "application_state.hpp"
#include "application_account_presentation.hpp"
#include "maicord/core/time.hpp"
#include "maicord/ui/message_context_controller.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
#include "maicord/model/permission_bits.hpp"
#include "../../src/app/account_ui_runtime.hpp"
namespace maicord {
namespace {
using namespace app_detail;
struct CreationAdapter {
    D3D11Renderer* renderer;
    UiTextMeasureCache* measure;
    ThreadCreationRequest flight;
    char name[kThreadCreationNameMaximumBytes];
    u64 flight_id;
    LPARAM pressed;
    u32 submissions, cancellations;
    bool transferred;
    bool (*refresh)(void*) noexcept;
    void* context;
};
CreationAdapter* creation_adapter;
void rebase(D3D11Renderer& r, bool login) noexcept {
    r.composer_layout.selections = r.edit_selections;
    r.login_edit.selections = r.edit_selections;
    r.edit_selection_owner = login ? &r.login_edit : &r.composer_layout;
}
bool call(bool (*operation)(void*) noexcept, void* context = nullptr) noexcept {
    if (!creation_adapter) return false;
    if (creation_adapter->transferred) return operation(context);
    auto& r = *creation_adapter->renderer;
    const bool login = r.edit_selection_owner == &r.login_edit;
    g_app.renderer = r;
    r = {};
    rebase(g_app.renderer, login);
    creation_adapter->transferred = true;
    creation_adapter->measure->backend_context = &g_app.renderer;
    g_app.account_ui->text_measure_cache.backend_context = &g_app.renderer;
    const bool result = operation(context);
    r = g_app.renderer;
    rebase(r, g_app.renderer.edit_selection_owner == &g_app.renderer.login_edit);
    g_app.renderer = {};
    g_app.renderer.width = r.width; g_app.renderer.height = r.height; g_app.renderer.dpi = r.dpi;
    g_app.renderer.text_form = r.text_form;
    g_app.account_ui->text_measure_cache.backend_context = &r;
    creation_adapter->transferred = false;
    creation_adapter->measure->backend_context = &r;
    return result;
}
RestClientSubmitResult WINAPI submit_creation(void*, const ThreadCreationRequest* request, u64 id) noexcept {
    if (!creation_adapter || !id) return {RestClientSubmitError::invalid_input};
    if (!request) {
        if (id != creation_adapter->flight_id) return {RestClientSubmitError::busy};
        ++creation_adapter->cancellations; return {};
    }
    if (creation_adapter->flight_id) return {RestClientSubmitError::busy};
    if (!thread_creation_request_valid(*request)) return {RestClientSubmitError::invalid_input};
    creation_adapter->flight = *request;
    copy_bytes(creation_adapter->name, request->name.data, request->name.bytes);
    creation_adapter->flight.name = {creation_adapter->name, request->name.bytes};
    creation_adapter->flight_id = id; ++creation_adapter->submissions; return {};
}
bool permission(bool allowed) noexcept {
    auto* state = reply_draft_ui_adapter_state();
    constexpr u64 base = kPermissionViewChannel | kPermissionReadMessageHistory |
        kPermissionSendMessages | kPermissionManageMessages;
    if (!state || !chat_state_store_upsert_role(state, {kReplyDraftProbeGuild, kReplyDraftProbeGuild,
        base | (allowed ? kPermissionCreatePublicThreads : 0), utf8_literal("Members")})) return false;
    chat_state_store_recompute_guild_permissions(state, kReplyDraftProbeGuild);
    return true;
}
} // namespace
bool thread_creation_ui_adapter_begin(HWND window, D3D11Renderer& renderer, UiTextMeasureCache& cache,
    bool (*refresh)(void*) noexcept, void* context) noexcept {
    if (creation_adapter || application_thread_creation_metrics().owner_bytes) return false;
    auto* value = static_cast<CreationAdapter*>(VirtualAlloc(nullptr, sizeof(CreationAdapter), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!value) return false;
    value->renderer = &renderer; value->measure = &cache; value->refresh = refresh; value->context = context;
    if (!reply_draft_ui_adapter_begin(window, renderer.dpi, refresh, context)) { VirtualFree(value, 0, MEM_RELEASE); return false; }
    creation_adapter = value;
    g_app.renderer.width = renderer.width; g_app.renderer.height = renderer.height; g_app.renderer.dpi = renderer.dpi;
    g_app.renderer_ready = true;
    g_app.account.api.thread_creation = submit_creation;
    g_app.account_ui->text_measure_cache = cache;
    g_app.account_ui->text_measure_cache.backend_context = &renderer;
    if (!permission(true)) { thread_creation_ui_adapter_end(); return false; }
    return true;
}
void thread_creation_ui_adapter_end() noexcept {
    if (!creation_adapter) return;
    auto* value = creation_adapter;
    call(+[](void*) noexcept { application_text_form_close(false, false); return true; });
    if (value->flight_id) thread_creation_ui_adapter_response(0, {}, RestClientError::cancelled);
    call(+[](void*) noexcept { application_thread_creation_release(); return true; });
    reply_draft_ui_adapter_end();
    creation_adapter = nullptr; VirtualFree(value, 0, MEM_RELEASE);
}
bool thread_creation_ui_adapter_active() noexcept { return creation_adapter != nullptr; }
bool thread_creation_ui_adapter_refresh() noexcept { return creation_adapter && creation_adapter->refresh(creation_adapter->context); }
D3D11Renderer& thread_creation_ui_adapter_renderer(D3D11Renderer& fallback) noexcept {
    return creation_adapter && creation_adapter->transferred ? g_app.renderer : fallback;
}
void thread_creation_ui_adapter_viewport(D3D11Renderer& r, const UiScene& shell) noexcept {
    if (!creation_adapter) return;
    g_app.shell_scene = shell;
    if (!creation_adapter->transferred) {
        g_app.renderer.width = r.width; g_app.renderer.height = r.height; g_app.renderer.dpi = r.dpi;
    }
    auto* surface = g_app.text_form ? &g_app.text_form->surface : nullptr;
    if (r.text_form != surface) renderer_bind_text_form(&r, surface);
}
bool thread_creation_ui_adapter_open() noexcept {
    return call(+[](void*) noexcept { return application_thread_creation_open(kReplyDraftProbeMessage); });
}
bool thread_creation_ui_adapter_menu() noexcept {
    return call(+[](void*) noexcept {
        const auto found = message_store_find(g_app.message_view.store, kReplyDraftProbeMessage);
        const auto* record = found.ok() ? message_store_resolve(g_app.message_view.store, found.handle) : nullptr;
        if (!record || !application_thread_creation_available(record->id)) return false;
        const auto result = message_context_controller_open(&g_app.message_context, &g_app.popup_scene, *record,
            false, false, false, nullptr, {640, 260, 650, 270}, application_viewport_dips(),
            false, {}, false, false, false, false, true);
        if (!result.ok()) return false;
        bool creation_row = false;
        for (u32 item = 0; item < g_app.message_context.menu.count; ++item)
            creation_row |= g_app.message_context.menu.items[item].action == static_cast<u32>(MessageContextAction::create_thread);
        if (!creation_row) return false;
        g_app.popup_owner_kind = PopupOwnerKind::message;
        renderer_bind_popup_scene(&g_app.renderer, &g_app.popup_scene);
        return thread_creation_ui_adapter_refresh();
    });
}
bool thread_creation_ui_adapter_input(UiInputKind kind, u32 code, bool repeated, bool control, bool shift) noexcept {
    struct Input { UiInputKind kind; u32 code; bool repeated, control, shift; } input{kind, code, repeated, control, shift};
    return call(+[](void* opaque) noexcept {
        const auto& input = *static_cast<Input*>(opaque);
        BYTE prior[256]{}, keys[256]{};
        if (!GetKeyboardState(prior)) return false;
        keys[VK_CONTROL] = input.control ? 0x80 : 0; keys[VK_SHIFT] = input.shift ? 0x80 : 0;
        if (!SetKeyboardState(keys)) return false;
        const bool accepted = application_text_form_input(input.kind, input.code, input.repeated);
        if (input.kind == UiInputKind::key_down) application_text_form_input(UiInputKind::key_up, input.code, false);
        return SetKeyboardState(prior) != FALSE && accepted;
    }, &input);
}
bool thread_creation_ui_adapter_pointer(TextFormElement element) noexcept {
    return call(+[](void* opaque) noexcept {
        if (!g_app.text_form) return false;
        const auto* node = text_form_node(g_app.text_form->scene, *static_cast<TextFormElement*>(opaque));
        if (!node) return false;
        const auto r = node->rectangle;
        const auto p = MAKELPARAM(static_cast<u16>((r.left + r.right) * .5f * g_app.renderer.dpi / 96.f),
            static_cast<u16>((r.top + r.bottom) * .5f * g_app.renderer.dpi / 96.f));
        LRESULT result = 0;
        return application_text_form_window_message(WM_LBUTTONDOWN, MK_LBUTTON, p, &result) &&
            application_text_form_window_message(WM_LBUTTONUP, 0, p, &result);
    }, &element);
}
bool thread_creation_ui_adapter_pointer_down(TextFormElement element) noexcept {
    return call(+[](void* opaque) noexcept {
        if (!g_app.text_form) return false;
        const auto* node = text_form_node(g_app.text_form->scene, *static_cast<TextFormElement*>(opaque));
        if (!node) return false;
        const auto r = node->rectangle;
        creation_adapter->pressed = MAKELPARAM(static_cast<u16>((r.left + r.right) * .5f * g_app.renderer.dpi / 96.f),
            static_cast<u16>((r.top + r.bottom) * .5f * g_app.renderer.dpi / 96.f));
        LRESULT result = 0;
        return application_text_form_window_message(WM_LBUTTONDOWN, MK_LBUTTON, creation_adapter->pressed, &result);
    }, &element);
}
bool thread_creation_ui_adapter_pointer_up() noexcept {
    return call(+[](void*) noexcept {
        LRESULT result = 0;
        return application_text_form_window_message(WM_LBUTTONUP, 0, creation_adapter->pressed, &result);
    });
}
bool thread_creation_ui_adapter_synchronize() noexcept {
    return call(+[](void*) noexcept { return application_thread_creation_synchronize() && application_text_form_synchronize(); });
}
bool thread_creation_ui_adapter_submit(u64 id) noexcept {
    return call(+[](void* opaque) noexcept { return application_thread_creation_submit(*static_cast<u64*>(opaque), clock_monotonic_milliseconds()); }, &id);
}
bool thread_creation_ui_adapter_response(u16 status, Utf8View body, RestClientError error, u64 exact_id) noexcept {
    if (!creation_adapter || !creation_adapter->flight_id) return false;
    struct Receipt { RestClientResponse response; Utf8View body; } receipt{};
    receipt.response.request_id = exact_id ? exact_id : creation_adapter->flight_id;
    receipt.response.status_code = status; receipt.response.error = error; receipt.response.body_bytes = body.bytes; receipt.body = body;
    if (receipt.response.request_id == creation_adapter->flight_id) creation_adapter->flight_id = 0;
    return call(+[](void* opaque) noexcept { const auto& r = *static_cast<Receipt*>(opaque);
        return application_thread_creation_response(r.response, r.body) && application_text_form_synchronize(); }, &receipt);
}
bool thread_creation_ui_adapter_flight(ThreadCreationRequest* request, u64* id) noexcept {
    if (!creation_adapter || !request || !id || !creation_adapter->flight_id) return false;
    *request = creation_adapter->flight; *id = creation_adapter->flight_id; return true;
}
u32 thread_creation_ui_adapter_submissions() noexcept { return creation_adapter ? creation_adapter->submissions : 0; }
u32 thread_creation_ui_adapter_cancellations() noexcept { return creation_adapter ? creation_adapter->cancellations : 0; }
ApplicationTextForm* thread_creation_ui_adapter_form() noexcept { return creation_adapter ? g_app.text_form : nullptr; }
bool thread_creation_ui_adapter_retire(u8 reason) noexcept {
    return call(+[](void* opaque) noexcept {
        const u8 reason = *static_cast<u8*>(opaque);
        if (reason == 0) { if (!permission(false)) return false; }
        else if (reason == 1) { ++g_app.plugin_account_scope_serial; ++g_app.account.status.profile_id; }
        else if (!message_store_delete(&g_app.message_view.store, kReplyDraftProbeMessage)) return false;
        return application_thread_creation_synchronize() && application_text_form_synchronize();
    }, &reason);
}
namespace app_detail {
// The probe has no slowmode state: thread creation is never throttled.
bool application_thread_create_cooldown_active(u64) noexcept { return false; }
void application_thread_create_note_created(u64) noexcept {}
} // namespace app_detail
} // namespace maicord
