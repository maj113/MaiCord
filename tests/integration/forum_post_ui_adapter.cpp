#include "forum_post_ui_adapter.hpp"
#include "forum_media_ui_adapter.hpp"
#include "clipboard_attachment_ui_fixture.hpp"
#include "application_forum_preview.hpp"
#include "application_text_form_state.hpp"
#include "maicord/app/account_media_worker.hpp"
#include "../media_fixture.hpp"
#include "maicord/sys/shell.hpp"
#include "maicord/sys/tsf.hpp"
#include "application_forum_form_state.hpp"
#include "application_state.hpp"
#include "application_thread_browser.hpp"
#include "maicord/core/time.hpp"
#include "maicord/core/log.hpp"
#include "maicord/protocol/writer.hpp"
#include "../tsf_composition_fixture.hpp"
#include "../file_drop_fixture.hpp"
#include "maicord/app/account_upload.hpp"
#include "maicord/model/permission_bits.hpp"
#include "../../src/app/account_ui_runtime.hpp"
#include "maicord/ui/thread_browser_scene.hpp"
namespace maicord {
namespace {
using namespace app_detail;
struct ForumAdapter {
    D3D11Renderer* renderer;
    UiTextMeasureCache* measure;
    ForumPostRequest flight;
    char title[400], body[8000];
    u8 response[16384];
    u64 flight_id;
    LPARAM pressed;
    u32 submissions, cancellations;
    bool transferred, manage_threads;
    bool media_enabled;
    AccountMediaWorker* media_worker;
    AccountUploadResult upload;
    wchar_t media_file[MAX_PATH];
    HGLOBAL pending_drop;
};
ForumAdapter* forum_adapter;
IDataObject* clipboard_fixture_provider;
void rebase(D3D11Renderer& r, bool login) noexcept {
    r.composer_layout.selections = r.edit_selections;
    r.login_edit.selections = r.edit_selections;
    r.edit_selection_owner = login ? &r.login_edit : &r.composer_layout;
}
bool call(bool (*operation)(void*) noexcept, void* context = nullptr) noexcept {
    if (!forum_adapter)
        return false;
    if (forum_adapter->transferred)
        return operation(context);
    auto& r = *forum_adapter->renderer;
    const bool login = r.edit_selection_owner == &r.login_edit;
    g_app.renderer = r;
    r = {};
    rebase(g_app.renderer, login);
    forum_adapter->transferred = true;
    forum_adapter->measure->backend_context = &g_app.renderer;
    g_app.account_ui->text_measure_cache.backend_context = &g_app.renderer;
    const bool result = operation(context);
    r = g_app.renderer;
    rebase(r, g_app.renderer.edit_selection_owner == &g_app.renderer.login_edit);
    g_app.renderer = {};
    g_app.renderer.width = r.width;
    g_app.renderer.height = r.height;
    g_app.renderer.dpi = r.dpi;
    forum_adapter->transferred = false;
    forum_adapter->measure->backend_context = &r;
    g_app.account_ui->text_measure_cache.backend_context = &r;
    return result;
}
RestClientSubmitResult WINAPI submit_forum(void*, const ForumPostRequest* request,
                                           u64 id) noexcept {
    if (!forum_adapter || !id)
        return {RestClientSubmitError::invalid_input};
    if (!request) {
        if (id != forum_adapter->flight_id)
            return {RestClientSubmitError::busy};
        ++forum_adapter->cancellations;
        return {};
    }
    if (forum_adapter->flight_id)
        return {RestClientSubmitError::busy};
    if (!forum_post_request_valid(*request))
        return {RestClientSubmitError::invalid_input};
    forum_adapter->flight = *request;
    copy_bytes(forum_adapter->title, request->title.data, request->title.bytes);
    copy_bytes(forum_adapter->body, request->content.data, request->content.bytes);
    forum_adapter->flight.title = {forum_adapter->title, request->title.bytes};
    forum_adapter->flight.content = {forum_adapter->body, request->content.bytes};
    forum_adapter->flight_id = id;
    ++forum_adapter->submissions;
    return {};
}
bool permissions(bool allowed, bool manage) noexcept {
    auto* state = reply_draft_ui_adapter_state();
    const u64 base = kPermissionViewChannel | kPermissionReadMessageHistory |
                     kPermissionSendMessagesInThreads |
                     (forum_adapter && forum_adapter->media_enabled ? kPermissionAttachFiles : 0);
    if (!state ||
        !chat_state_store_upsert_role(
            state,
            {kReplyDraftProbeGuild, kReplyDraftProbeGuild,
             base | (allowed ? kPermissionSendMessages | kPermissionCreatePublicThreads : 0) |
                 (manage ? kPermissionManageThreads : 0),
             utf8_literal("Members")}))
        return false;
    chat_state_store_recompute_guild_permissions(state, kReplyDraftProbeGuild);
    return true;
}
} // namespace
bool forum_post_ui_adapter_begin(HWND window, D3D11Renderer& r, UiTextMeasureCache& measure,
                                 bool (*refresh)(void*) noexcept, void* context) noexcept {
    if (forum_adapter || application_forum_post_metrics().owner_bytes || application_forum_form_active())
        return false;
    auto* value = static_cast<ForumAdapter*>(
        VirtualAlloc(nullptr, sizeof(ForumAdapter), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!value)
        return false;
    value->renderer = &r;
    value->measure = &measure;
    if (!reply_draft_ui_adapter_begin(window, r.dpi, refresh, context)) {
        VirtualFree(value, 0, MEM_RELEASE);
        return false;
    }
    forum_adapter = value;
    g_app.renderer.width = r.width;
    g_app.renderer.height = r.height;
    g_app.renderer.dpi = r.dpi;
    g_app.renderer_ready = true;
    g_app.account.api.forum_post = submit_forum;
    g_app.account_ui->text_measure_cache = measure;
    g_app.account_ui->text_measure_cache.backend_context = &r;
    auto* state = reply_draft_ui_adapter_state();
    const ChannelInput parent{
        kForumPostProbeParent, kReplyDraftProbeGuild, 0, utf8_literal("product-forum"), 2, 0, 15};
    if (!permissions(true, false) || !chat_state_store_upsert_channel(state, parent).ok() ||
        !chat_state_store_set_thread_listing_synced(state, parent.id, state->self_user_id)) {
        forum_post_ui_adapter_end();
        return false;
    }
    chat_state_store_recompute_guild_permissions(state, kReplyDraftProbeGuild);
    return true;
}
void forum_post_ui_adapter_end() noexcept {
    if (!forum_adapter)
        return;
    auto* value = forum_adapter;
    call(+[](void*) noexcept {
        application_forum_form_close(false, false);
        application_thread_browser_close();
        return true;
    });
    if (value->flight_id)
        forum_post_ui_adapter_response(0, {}, RestClientError::cancelled);
    call(+[](void*) noexcept {
        application_forum_post_release();
        application_forum_form_release();
        application_thread_browser_release();
        return true;
    });
    if (value->media_enabled) {
        call(+[](void*) noexcept {
            application_forum_preview_release();
            return application_forum_attachments_release();
        });
        if (value->media_worker) {
            while (!account_media_worker_stop(value->media_worker, 100)) {
            }
            call(+[](void*) noexcept {
                application_forum_preview_release();
                return true;
            });
            VirtualFree(value->media_worker, 0, MEM_RELEASE);
        }
        if (value->media_file[0])
            DeleteFileW(value->media_file);
    }
    reply_draft_ui_adapter_end();
    forum_adapter = nullptr;
    VirtualFree(value, 0, MEM_RELEASE);
}
bool forum_post_ui_adapter_active() noexcept {
    return forum_adapter != nullptr;
}
bool forum_post_ui_adapter_navigate(u64 channel, u64 message) noexcept {
    if (!forum_adapter || !g_app.account_ui || channel != kForumPostProbeThread ||
        message != kForumPostProbeMessage ||
        !chat_state_store_select_channel(&g_app.account_ui->chat_state, channel).ok() ||
        !message_composer_controller_select_channel(&g_app.account_ui->message_composer_controller, channel)
             .ok() ||
        !message_timeline_reset(&g_app.message_view.timeline))
        return false;
    g_app.account_ui->active_channel_id = channel;
    g_app.pending_message_jump_channel_id = channel;
    g_app.pending_message_jump_message_id = message;
    g_app.context_focus = nullptr;
    g_app.message_composer.focused = false;
    // The shipping forum host publishes its validated starter event after this
    // channel/history navigation seam. This fixture never inserts that message.
    return true;
}
D3D11Renderer& forum_post_ui_adapter_renderer(D3D11Renderer& r) noexcept {
    return forum_adapter && forum_adapter->transferred ? g_app.renderer : r;
}
void forum_post_ui_adapter_viewport(D3D11Renderer& r, const UiScene& shell) noexcept {
    if (!forum_adapter)
        return;
    g_app.shell_scene = shell;
    if (!forum_adapter->transferred) {
        g_app.renderer.width = r.width;
        g_app.renderer.height = r.height;
        g_app.renderer.dpi = r.dpi;
    }
    renderer_bind_forum_form(&r, application_forum_form_surface());
    auto* child = g_app.text_form ? &g_app.text_form->surface : nullptr;
    if (r.text_form != child)
        renderer_bind_text_form(&r, child);
    renderer_bind_thread_browser_scene(&r, application_thread_browser_scene());
}
bool forum_post_ui_adapter_browser() noexcept {
    return call(+[](void*) noexcept { return application_thread_browser_open(kForumPostProbeParent); });
}
bool forum_post_ui_adapter_browser_create() noexcept {
    return call(+[](void*) noexcept {
        auto* scene = application_thread_browser_scene();
        if (!scene)
            return false;
        UiWidgetHandle widget = ui_widget_invalid_handle();
        for (u32 slot = 0; slot < scene->widgets.count; ++slot) {
            const UiWidgetHandle candidate{static_cast<u16>(slot), scene->widgets.generation};
            if (ui_thread_browser_scene_target(*scene, candidate).action ==
                UiThreadBrowserAction::create_post) {
                widget = candidate;
                break;
            }
        }
        const auto target = ui_thread_browser_scene_target(*scene, widget);
        if (!ui_thread_browser_scene_target_current(*scene, target))
            return false;
        const auto* node = ui_widget_manager_layout_node(scene->widgets, widget);
        if (!node)
            return false;
        const auto rectangle = node->rectangle;
        const auto point = MAKELPARAM(
            static_cast<u16>((rectangle.left + rectangle.right) * .5f * g_app.renderer.dpi / 96.f),
            static_cast<u16>((rectangle.top + rectangle.bottom) * .5f * g_app.renderer.dpi / 96.f));
        return application_thread_browser_window_message(WM_LBUTTONDOWN, MK_LBUTTON, point) &&
               application_thread_browser_window_message(WM_LBUTTONUP, 0, point) &&
               application_forum_form_active();
    });
}
bool forum_post_ui_adapter_open() noexcept {
    return call(+[](void*) noexcept { return application_forum_post_open(kForumPostProbeParent); });
}
bool forum_post_ui_adapter_input(UiInputKind kind, u32 code, bool repeated, bool control,
                                 bool shift) noexcept {
    struct Input {
        UiInputKind kind;
        u32 code;
        bool repeated, control, shift;
    } input{kind, code, repeated, control, shift};
    return call(
        +[](void* opaque) noexcept {
            const auto& value = *static_cast<Input*>(opaque);
            BYTE prior[256]{}, keys[256]{};
            if (!GetKeyboardState(prior))
                return false;
            keys[VK_CONTROL] = value.control ? 0x80 : 0;
            keys[VK_SHIFT] = value.shift ? 0x80 : 0;
            if (!SetKeyboardState(keys))
                return false;
            const bool child = application_text_form_forum_child(application_forum_post_view().ticket);
            const bool accepted =
                child ? application_text_form_input(value.kind, value.code, value.repeated)
                      : application_forum_form_input(value.kind, value.code, value.repeated);
            // Keep the native release paired with every injected down, including
            // Return's consumed modal tail after validation/async submission.
            const bool released =
                value.kind != UiInputKind::key_down ||
                (child ? application_text_form_input(UiInputKind::key_up, value.code, false)
                       : application_forum_form_input(UiInputKind::key_up, value.code, false));
            return SetKeyboardState(prior) != FALSE && accepted && released;
        },
        &input);
}
namespace {
const UiLayoutNode* forum_target_node(ForumPostScene& scene, const ForumPostTarget& target) noexcept {
    return target.element == ForumPostElement::attachments
               ? ui_widget_manager_layout_node(
                     scene.widgets,
                     ui_attachment_draft_list_handle(scene.attachment_list, target.attachment_id,
                                                     target.attachment_action))
           : target.element == ForumPostElement::tags && target.tag < scene.content.tags.count
               ? ui_widget_manager_layout_node(scene.widgets, scene.tags[target.tag])
               : forum_post_node(scene, target.element);
}
} // namespace
bool forum_post_ui_adapter_pointer_down(ForumPostTarget target) noexcept {
    return call(
        +[](void* opaque) noexcept {
            const auto target = *static_cast<ForumPostTarget*>(opaque);
            auto* scene = application_forum_form_scene();
            if (!scene)
                return false;
            const UiLayoutNode* node = forum_target_node(*scene, target);
            if (!node)
                return false;
            auto rect = ui_rect_intersection(node->rectangle, node->clip_rectangle);
            if (!ui_rect_has_area(rect)) {
                // Use the shipping keyboard traversal to reveal clipped controls;
                // it also detaches/rebinds the real TSF field owner.
                bool revealed = false;
                for (u32 step = 0; step < kForumPostTagCapacity + 7 + kUiAttachmentDraftMaximum * 4;
                     ++step) {
                    if (!application_forum_form_input(UiInputKind::key_down, VK_TAB, false))
                        return false;
                    application_forum_form_input(UiInputKind::key_up, VK_TAB, false);
                    const auto focused = forum_post_focused(*scene);
                    if (focused.element == target.element && focused.tag == target.tag &&
                        focused.attachment_id == target.attachment_id &&
                        focused.attachment_action == target.attachment_action) {
                        revealed = true;
                        break;
                    }
                }
                if (!revealed)
                    return false;
                node = forum_target_node(*scene, target);
                if (!node)
                    return false;
                rect = ui_rect_intersection(node->rectangle, node->clip_rectangle);
            }
            if (!ui_rect_has_area(rect))
                return false;
            forum_adapter->pressed =
                MAKELPARAM(static_cast<u16>((rect.left + rect.right) * .5f * g_app.renderer.dpi / 96.f),
                           static_cast<u16>((rect.top + rect.bottom) * .5f * g_app.renderer.dpi / 96.f));
            LRESULT result = 0;
            return application_forum_form_window_message(WM_LBUTTONDOWN, MK_LBUTTON, forum_adapter->pressed,
                                                         &result);
        },
        &target);
}
bool forum_post_ui_adapter_pointer_up() noexcept {
    return call(+[](void*) noexcept {
        LRESULT result = 0;
        return application_forum_form_window_message(WM_LBUTTONUP, 0, forum_adapter->pressed, &result);
    });
}
bool forum_post_ui_adapter_pointer(ForumPostTarget target) noexcept {
    return forum_post_ui_adapter_pointer_down(target) && forum_post_ui_adapter_pointer_up();
}
bool forum_post_ui_adapter_synchronize() noexcept {
    if (forum_adapter && forum_adapter->media_enabled)
        return forum_media_ui_adapter_pump();
    return call(+[](void*) noexcept {
        return application_forum_post_synchronize() && application_forum_form_synchronize() &&
               application_thread_browser_synchronize();
    });
}
bool forum_post_ui_adapter_submit(u64 id) noexcept {
    return call(
        +[](void* opaque) noexcept {
            return application_forum_post_submit(*static_cast<u64*>(opaque),
                                                 clock_monotonic_milliseconds()) &&
                   application_forum_form_synchronize();
        },
        &id);
}
bool forum_post_ui_adapter_reject_before_wake(u64 id) noexcept {
    return call(
        +[](void* opaque) noexcept {
            const u64 wake = application_forum_post_next_wake();
            const u32 submits = forum_adapter->submissions;
            return wake > 1 && !application_forum_post_submit(*static_cast<u64*>(opaque), wake - 1) &&
                   forum_adapter->submissions == submits;
        },
        &id);
}
bool forum_post_ui_adapter_response(u16 status, Utf8View body, RestClientError error,
                                    u64 exact) noexcept {
    if (!forum_adapter || !forum_adapter->flight_id)
        return false;
    struct Receipt {
        RestClientResponse response;
        Utf8View body;
    } value{};
    value.response.request_id = exact ? exact : forum_adapter->flight_id;
    value.response.status_code = status;
    value.response.error = error;
    value.response.body_bytes = body.bytes;
    value.body = body;
    if (value.response.request_id == forum_adapter->flight_id)
        forum_adapter->flight_id = 0;
    return call(
        +[](void* opaque) noexcept {
            const auto& value = *static_cast<Receipt*>(opaque);
            return application_forum_post_response(value.response, value.body) &&
                   application_forum_form_synchronize();
        },
        &value);
}
bool forum_post_ui_adapter_flight(ForumPostRequest* request, u64* id) noexcept {
    if (!forum_adapter || !forum_adapter->flight_id || !request || !id)
        return false;
    *request = forum_adapter->flight;
    *id = forum_adapter->flight_id;
    return true;
}
u32 forum_post_ui_adapter_submissions() noexcept {
    return forum_adapter ? forum_adapter->submissions : 0;
}
u32 forum_post_ui_adapter_cancellations() noexcept {
    return forum_adapter ? forum_adapter->cancellations : 0;
}
ForumPostScene* forum_post_ui_adapter_scene() noexcept {
    return forum_adapter ? application_forum_form_scene() : nullptr;
}
ForumPostSurface* forum_post_ui_adapter_surface() noexcept {
    return forum_adapter ? application_forum_form_surface() : nullptr;
}
SecureEdit* forum_post_ui_adapter_edit(u32 field) noexcept {
    return forum_adapter && field < 2
               ? application_forum_form_edit(static_cast<ApplicationForumField>(field))
               : nullptr;
}
bool forum_post_ui_adapter_costs(ForumPostProbeCosts* costs) noexcept {
    auto* form = application_forum_form_state();
    if (!forum_adapter || !form || !costs)
        return false;
    const auto metrics = application_forum_form_metrics();
    *costs = {};
    MEMORY_BASIC_INFORMATION form_memory{};
    if (!VirtualQuery(form, &form_memory, sizeof(form_memory)) || form_memory.State != MEM_COMMIT ||
        form_memory.RegionSize != metrics.committed_bytes)
        return false;
    costs->form_bytes = metrics.owner_bytes;
    costs->form_committed_bytes = static_cast<u32>(form_memory.RegionSize);
    costs->form_revision = metrics.revision;
    costs->form_serial = metrics.serial;
    costs->scene_committed_bytes = static_cast<u32>(form->scene.storage.arena.committed);
    costs->scene_commit_count = form->scene.storage.arena.commit_count;
    costs->scene_revision = form->scene.revision;
    costs->layout_revision = form->scene.widgets.layout.revision;
    costs->measure_calls = form->scene.measure->backend_calls;
    for (u32 field = 0; field < 2; ++field) {
        MEMORY_BASIC_INFORMATION memory{};
        if (!VirtualQuery(form->fields[field].edit.storage.data, &memory, sizeof(memory)) ||
            memory.State != MEM_COMMIT)
            return false;
        costs->edit_committed_bytes[field] = static_cast<u32>(memory.RegionSize);
        costs->history[field] = secure_edit_history_costs(form->fields[field].edit);
        if (!VirtualQuery(form->fields[field].edit.history, &memory, sizeof(memory)) ||
            memory.State != MEM_COMMIT)
            return false;
        costs->history_owner_committed_bytes[field] = static_cast<u32>(memory.RegionSize);
    }
    return true;
}
bool forum_post_ui_adapter_manage_threads(bool manage) noexcept {
    return call(
        +[](void* opaque) noexcept {
            const bool value = *static_cast<bool*>(opaque);
            forum_adapter->manage_threads = value;
            return permissions(true, value) && application_forum_post_synchronize() &&
                   application_forum_form_synchronize();
        },
        &manage);
}
bool forum_post_ui_adapter_retire(u8 reason) noexcept {
    return call(
        +[](void* opaque) noexcept {
            const auto reason = *static_cast<u8*>(opaque);
            if (reason == 0) {
                if (!permissions(false, false))
                    return false;
            } else if (reason == 1) {
                ++g_app.plugin_account_scope_serial;
                ++g_app.account.status.profile_id;
            } else {
                if (!chat_state_store_remove_channel(&g_app.account_ui->chat_state,
                                                        kForumPostProbeParent))
                    return false;
                if (!chat_state_store_upsert_channel(&g_app.account_ui->chat_state,
                                                        {kForumPostProbeParent, kReplyDraftProbeGuild, 0,
                                                         utf8_literal("replacement-forum"), 2, 0, 15})
                         .ok())
                    return false;
                chat_state_store_recompute_guild_permissions(&g_app.account_ui->chat_state,
                                                                kReplyDraftProbeGuild);
            }
            return application_forum_post_synchronize() && application_forum_form_synchronize();
        },
        &reason);
}

bool forum_post_ui_adapter_metadata(bool required, Utf8View* output, bool unnamed) noexcept {
    if (!forum_adapter || !output)
        return false;
    ProtocolWriter writer{{forum_adapter->response, sizeof(forum_adapter->response)}, 0};
    if (!protocol_writer_append_literal(
            &writer,
            "{\"id\":\"97400\",\"guild_id\":\"97000\",\"type\":15,\"name\":\"product-forum\",\"flags\":") ||
        !protocol_writer_append_u64(&writer, required ? 16 : 0) ||
        !protocol_writer_append_literal(&writer,
                                        ",\"default_auto_archive_duration\":4320,\"available_tags\":["))
        return false;
    const Utf8View names[]{utf8_literal("Discussion"), utf8_literal("Feedback"), utf8_literal("日本語"),
                           utf8_literal("Planning"),   utf8_literal("Release"),  utf8_literal("Question"),
                           utf8_literal("Moderated")};
    for (u32 tag = 0; tag < 7; ++tag) {
        if ((tag && !protocol_writer_append_byte(&writer, ',')) ||
            !protocol_writer_append_literal(&writer, "{\"id\":\"") ||
            !protocol_writer_append_u64(&writer, 97501 + tag) ||
            !protocol_writer_append_literal(&writer, "\",\"name\":") ||
            !protocol_writer_append_json_string(&writer, unnamed && tag == 1 ? Utf8View{} : names[tag]) ||
            !protocol_writer_append_literal(&writer, ",\"moderated\":") ||
            !protocol_writer_append_c_string(&writer, tag == 6 ? "true}" : "false}"))
            return false;
    }
    if (!protocol_writer_append_literal(&writer, "]}"))
        return false;
    *output = {reinterpret_cast<const char*>(forum_adapter->response), writer.used};
    return true;
}
bool forum_post_ui_adapter_success(Utf8View* output) noexcept {
    if (!forum_adapter || !output || forum_adapter->flight.operation != ForumPostOperation::create)
        return false;
    const auto& request = forum_adapter->flight;
    ProtocolWriter writer{{forum_adapter->response, sizeof(forum_adapter->response)}, 0};
    if (!protocol_writer_append_literal(
            &writer, "{\"id\":\"97450\",\"guild_id\":\"97000\",\"parent_id\":\"97400\",\"type\":11,\"owner_"
                     "id\":\"97005\",\"last_message_id\":\"97451\",\"name\":") ||
        !protocol_writer_append_json_string(&writer, request.title) ||
        !protocol_writer_append_literal(
            &writer, ",\"thread_metadata\":{\"archived\":false,\"locked\":false,\"archive_timestamp\":"
                     "\"2026-10-05T12:00:00.000Z\",\"auto_archive_duration\":4320},\"applied_tags\":["))
        return false;
    for (u32 tag = 0; tag < request.tag_count; ++tag)
        if ((tag && !protocol_writer_append_byte(&writer, ',')) ||
            !protocol_writer_append_byte(&writer, '"') ||
            !protocol_writer_append_u64(&writer, request.applied_tag_ids[tag]) ||
            !protocol_writer_append_byte(&writer, '"'))
            return false;
    if (!protocol_writer_append_literal(
            &writer,
            "],\"member\":{\"id\":\"97450\",\"user_id\":\"97005\",\"join_timestamp\":\"2026-10-05T12:00:00."
            "000Z\",\"flags\":0},\"message\":{\"id\":\"97451\",\"channel_id\":\"97450\",\"author\":{\"id\":"
            "\"97005\"},\"type\":0,\"flags\":0,\"timestamp\":\"2026-10-05T12:00:00.000Z\",\"content\":") ||
        !protocol_writer_append_json_string(&writer, request.content) ||
        !protocol_writer_append_literal(&writer, "}}"))
        return false;
    *output = {reinterpret_cast<const char*>(forum_adapter->response), writer.used};
    return true;
}
namespace {
struct ForumTextSink final : ITextStoreACPSink {
    ULONG references = 1;
    ITextStoreACP* store{};
    ITfContextOwnerCompositionSink* owner{};
    tsf_test::Composition composition{};
    u32 phase{};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** output) override {
        if (!output)
            return E_POINTER;
        *output = nullptr;
        if (!bytes_equal(&id, &__uuidof(IUnknown), sizeof(GUID)) &&
            !bytes_equal(&id, &__uuidof(ITextStoreACPSink), sizeof(GUID)))
            return E_NOINTERFACE;
        *output = static_cast<ITextStoreACPSink*>(this);
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++references;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        return --references;
    }
    HRESULT STDMETHODCALLTYPE OnLockGranted(DWORD) override {
        TS_TEXTCHANGE change{};
        if (!phase++) {
            const WCHAR text[] = L"First line 日本語\nSecond line 🚀\nTSF paste k";
            if (store->InsertTextAtSelection(TS_IAS_NOQUERY, text,
                                             static_cast<ULONG>(sizeof(text) / sizeof(text[0]) - 1),
                                             nullptr, nullptr, &change) != S_OK)
                return E_FAIL;
            LONG end = 0;
            if (store->GetEndACP(&end) != S_OK || !end)
                return E_FAIL;
            composition.range.start = end - 1;
            composition.range.length = 1;
            BOOL accepted = FALSE;
            return owner->OnStartComposition(&composition, &accepted) == S_OK && accepted ? S_OK : E_FAIL;
        }
        const WCHAR text[]{0x304b};
        if (store->SetText(0, composition.range.start, composition.range.start + 1, text, 1, &change) !=
                S_OK ||
            owner->OnUpdateComposition(&composition, &composition.range) != S_OK)
            return E_FAIL;
        return owner->OnEndComposition(&composition);
    }
    HRESULT STDMETHODCALLTYPE OnTextChange(DWORD, const TS_TEXTCHANGE*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnSelectionChange() override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnLayoutChange(TsLayoutCode, TsViewCookie) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnStatusChange(DWORD) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnAttrsChange(LONG, LONG, ULONG, const TS_ATTRID*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnStartEditTransaction() override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnEndEditTransaction() override {
        return S_OK;
    }
};
} // namespace
bool forum_post_ui_adapter_drop_files() noexcept {
    return call(+[](void*) noexcept {
        auto* form = application_forum_form_state();
        if (!form)
            return false;
        const wchar_t filename[] = LR"(D:/MaiCord-build/qa/forum-post-20261005/covered-upload.txt)";
        const ConstSlice<wchar_t> path{filename,
                                       static_cast<u32>(sizeof(filename) / sizeof(filename[0]) - 1)};
        const HDROP drop = file_drop_fixture::make({&path, 1});
        if (!drop)
            return false;
        if (DragQueryFileW(drop, 0xffffffffu, nullptr, 0) != 1) {
            DragFinish(drop);
            return false;
        }
        const auto* attachments = g_app.attachments;
        const u32 composer_revision = g_app.message_composer.revision;
        const u64 channel = g_app.account_ui->active_channel_id;
        const u32 title_bytes = form->fields[0].edit.storage.bytes,
                  body_bytes = form->fields[1].edit.storage.bytes;
        LRESULT result = 123;
        const bool handled =
            application_forum_form_window_message(WM_DROPFILES, reinterpret_cast<WPARAM>(drop), 0, &result);
        const bool freed = GlobalFlags(reinterpret_cast<HGLOBAL>(drop)) == GMEM_INVALID_HANDLE;
        if (!freed)
            DragFinish(drop);
        return handled && result == 0 && freed && application_forum_form_state() == form &&
               g_app.attachments == attachments && g_app.message_composer.revision == composer_revision &&
               g_app.account_ui->active_channel_id == channel &&
               form->fields[0].edit.storage.bytes == title_bytes &&
               form->fields[1].edit.storage.bytes == body_bytes;
    });
}
bool forum_post_ui_adapter_escape_tails() noexcept {
    return call(+[](void*) noexcept {
        return application_forum_form_input(UiInputKind::key_down, VK_ESCAPE, false) &&
               !application_forum_form_active() &&
               application_forum_form_input(UiInputKind::key_down, VK_ESCAPE, true) &&
               application_forum_form_input(UiInputKind::character, VK_ESCAPE, false) &&
               application_forum_form_input(UiInputKind::key_up, VK_ESCAPE, false) &&
               !application_forum_form_input(UiInputKind::character, 'x', false);
    });
}
bool forum_post_ui_adapter_tsf_paste_composition() noexcept {
    return call(+[](void*) noexcept {
        auto* form = application_forum_form_state();
        if (!form || !form->editable)
            return false;
        auto& field = form->fields[1];
        const bool attached = secure_text_store_attached(field.store);
        secure_text_store_detach(&field.store);
        if (!secure_text_store_process_deferred())
            return false;
        ForumTextSink sink{};
        sink.store = static_cast<ITextStoreACP*>(secure_text_store_test_interface(&field.store));
        if (!sink.store)
            return false;
        const bool queried = sink.store->QueryInterface(__uuidof(ITfContextOwnerCompositionSink),
                                                        reinterpret_cast<void**>(&sink.owner)) == S_OK;
        const bool advised =
            queried && sink.store->AdviseSink(__uuidof(ITextStoreACPSink), &sink, TS_AS_ALL_SINKS) == S_OK;
        HRESULT first = E_FAIL, second = E_FAIL;
        bool valid = advised && sink.store->RequestLock(TS_LF_READWRITE | TS_LF_SYNC, &first) == S_OK &&
                     first == S_OK;
        const u32 submissions = forum_adapter->submissions;
        valid = valid && field.composition.active &&
                forum_post_ui_adapter_input(UiInputKind::key_down, VK_RETURN, false, true) &&
                forum_adapter->submissions == submissions &&
                application_forum_post_view().phase == ApplicationForumPostPhase::editing;
        if (advised && sink.phase == 1)
            valid = sink.store->RequestLock(TS_LF_READWRITE | TS_LF_SYNC, &second) == S_OK &&
                    second == S_OK && valid;
        if (!valid) {
            // Retire the actual store's reference to this stack composition
            // while its view and sink still exist, including failed lock paths.
            if (sink.owner)
                sink.owner->OnEndComposition(&sink.composition);
            secure_text_store_unbind(&field.store);
            secure_text_store_bind(&field.store, &field.edit, SecureTextInputScope::text);
        }
        if (advised)
            sink.store->UnadviseSink(&sink);
        if (sink.owner)
            sink.owner->Release();
        if (attached)
            valid = secure_text_store_attach(&field.store) && valid;
        return valid && !field.composition.active && application_forum_form_synchronize() &&
               utf8_equal(application_forum_post_view().body,
                          utf8_literal("First line 日本語\nSecond line 🚀\nTSF paste か"));
    });
}
namespace {
struct ClipboardDispatch {
    u32 remaining = 32;
    bool interrupted = false;
};
bool ole_window(HWND window) noexcept {
    wchar_t name[64]{};
    constexpr wchar_t expected[] = L"OleMainThreadWndClass";
    const int units = GetClassNameW(window, name, 64);
    if (units != static_cast<int>(sizeof(expected) / sizeof(expected[0]) - 1))
        return false;
    for (int i = 0; i < units; ++i)
        if (name[i] != expected[i])
            return false;
    return true;
}
void dispatch_clipboard_window(HWND window, ClipboardDispatch* state) noexcept {
    if (!window || window == g_app.window ||
        GetWindowThreadProcessId(window, nullptr) != GetCurrentThreadId() || !ole_window(window))
        return;
    MSG message{};
    while (state->remaining && PeekMessageW(&message, window, 0, 0, PM_REMOVE)) {
        --state->remaining;
        if (message.message == WM_QUIT) {
            PostQuitMessage(static_cast<int>(message.wParam));
            state->interrupted = true;
            return;
        }
        // Never invoke the probe WndProc while its renderer is transferred.
        // Only OLE's own RPC windows may receive this fixture pump.
        DispatchMessageW(&message);
    }
}
BOOL CALLBACK clipboard_window_visit(HWND window, LPARAM opaque) noexcept {
    auto* state = reinterpret_cast<ClipboardDispatch*>(opaque);
    dispatch_clipboard_window(window, state);
    return state->remaining && !state->interrupted;
}
bool dispatch_clipboard_calls() noexcept {
    ClipboardDispatch state{};
    EnumThreadWindows(GetCurrentThreadId(), clipboard_window_visit, reinterpret_cast<LPARAM>(&state));
    for (HWND window = nullptr; state.remaining && !state.interrupted;) {
        window = FindWindowExW(HWND_MESSAGE, window, L"OleMainThreadWndClass", nullptr);
        if (!window)
            break;
        dispatch_clipboard_window(window, &state);
    }
    return !state.interrupted;
}
AccountUploadResult WINAPI media_upload(void*, AccountUploadOperation operation, u64 id,
                                        const AccountUploadRequest* request) noexcept {
    if (!forum_adapter)
        return {};
    auto& result = forum_adapter->upload;
    if (operation == AccountUploadOperation::start) {
        if (!request || !request->source.bytes)
            return {{}, id, HttpsFileUploadError::invalid_input, false, false};
        result = {
            {0, request->source.bytes, 0, 0, 0, HttpsFileUploadError::none, HttpsFileUploadPhase::sending},
            id,
            HttpsFileUploadError::none,
            false,
            true};
    } else if (operation == AccountUploadOperation::cancel) {
        result.transfer.phase = HttpsFileUploadPhase::complete;
        result.transfer.error = HttpsFileUploadError::cancelled;
    } else if (operation == AccountUploadOperation::release) {
        result.released = true;
        result.owns_transfer = false;
    }
    return result;
}
AccountMediaRequestResult WINAPI media_preview(void*, AccountLocalPreviewOperation operation,
                                               const AccountMediaLocalPreviewRequest* request, u64 now,
                                               u64 unix_now) noexcept {
    if (!forum_adapter || !forum_adapter->media_worker)
        return {};
    if (operation == AccountLocalPreviewOperation::presented)
        return !request && !now && !unix_now
                   ? account_media_worker_local_previews_presented(forum_adapter->media_worker)
                   : AccountMediaRequestResult{};
    if (!request) return {};
    if (operation == AccountLocalPreviewOperation::uploaded)
        return !now && !unix_now
                   ? account_media_worker_local_preview_uploaded(forum_adapter->media_worker, *request)
                   : AccountMediaRequestResult{};
    if (operation == AccountLocalPreviewOperation::retire)
        return account_media_worker_retire_local_preview(forum_adapter->media_worker, *request);
    if (operation == AccountLocalPreviewOperation::request)
        return account_media_worker_request_local_preview(forum_adapter->media_worker, *request, now, unix_now);
    return {};
}
} // namespace
bool forum_media_ui_adapter_enable() noexcept {
    return call(+[](void*) noexcept {
        if (!forum_adapter || forum_adapter->media_enabled)
            return false;
        forum_adapter->media_worker = static_cast<AccountMediaWorker*>(
            VirtualAlloc(nullptr, sizeof(AccountMediaWorker), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!forum_adapter->media_worker)
            return false;
        if (!account_media_worker_initialize(forum_adapter->media_worker).ok()) {
            while (!account_media_worker_stop(forum_adapter->media_worker, 100)) {
            }
            VirtualFree(forum_adapter->media_worker, 0, MEM_RELEASE);
            forum_adapter->media_worker = nullptr;
            return false;
        }
        forum_adapter->media_enabled = true;
        g_app.account.api.upload = media_upload;
        g_app.account.api.media_local_preview = media_preview;
        return permissions(true, false);
    });
}
bool forum_media_ui_adapter_seed_drop() noexcept {
    return call(+[](void*) noexcept {
        const auto failure = [](u32 stage, DWORD native = ERROR_SUCCESS) noexcept {
            log_u64(LogLevel::error, "probe", "forum_media_drop_failure_stage", stage);
            log_u64(LogLevel::error, "probe", "forum_media_drop_native_error", native);
            return false;
        };
        wchar_t directory[MAX_PATH]{}, temporary[MAX_PATH]{};
        if (!GetTempPathW(MAX_PATH, directory) || !GetTempFileNameW(directory, L"mcm", 0, temporary))
            return failure(1, GetLastError());
        u32 units = 0;
        while (temporary[units])
            ++units;
        if (units + 4 >= MAX_PATH) {
            DeleteFileW(temporary);
            return failure(2, ERROR_INSUFFICIENT_BUFFER);
        }
        copy_bytes(forum_adapter->media_file, temporary, units * sizeof(wchar_t));
        DeleteFileW(temporary);
        const wchar_t suffix[] = L".png";
        copy_bytes(forum_adapter->media_file + units, suffix, sizeof(suffix));
        units += 4;
        const HANDLE file = CreateFileW(forum_adapter->media_file, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                        CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
        if (file == INVALID_HANDLE_VALUE)
            return failure(3, GetLastError());
        u8 encoded[8192]{}, scratch[8192]{};
        PngFixtureResult image{};
        DWORD written = 0;
        const bool seeded = build_rgba8_png_fixture(
                                32, 32, {encoded, sizeof(encoded)}, {scratch, sizeof(scratch)}, &image,
                                +[](u32, u32, u32, u32) noexcept { return 0xffff00ffu; }) &&
                            WriteFile(file, encoded, image.encoded_bytes, &written, nullptr) &&
                            written == image.encoded_bytes;
        CloseHandle(file);
        if (!seeded)
            return failure(4, GetLastError());
        const ConstSlice<wchar_t> path{forum_adapter->media_file, units};
        const auto drop = file_drop_fixture::make({&path, 1});
        if (!drop)
            return failure(5, GetLastError());
        LRESULT result = 1;
        const auto channel = g_app.account_ui->active_channel_id;
        const auto* composer_attachments = g_app.attachments;
        const bool handled =
            application_forum_form_window_message(WM_DROPFILES, reinterpret_cast<WPARAM>(drop), 0, &result);
        // Successful admission transfers the handle to the native worker. It
        // calls DragFinish before publishing completion, so an immediate free
        // assertion races that worker and must never free its borrowed handle.
        if (!handled) {
            DragFinish(drop);
            return failure(6);
        }
        forum_adapter->pending_drop = reinterpret_cast<HGLOBAL>(drop);
        if (result != 0 || channel != g_app.account_ui->active_channel_id ||
            composer_attachments != g_app.attachments)
            return failure(7);
        return true;
    });
}
bool forum_media_ui_adapter_drop_consumed() noexcept {
    if (!forum_adapter)
        return false;
    if (forum_adapter->pending_drop && GlobalFlags(forum_adapter->pending_drop) == GMEM_INVALID_HANDLE)
        forum_adapter->pending_drop = nullptr;
    return !forum_adapter->pending_drop;
}
bool forum_media_ui_adapter_pump() noexcept {
    return call(+[](void*) noexcept {
        if (application_forum_attachments_view(application_forum_post_view().ticket).busy &&
            !dispatch_clipboard_calls())
            return false;
        if (!application_forum_attachments_pump())
            return false;
        if (!application_forum_form_synchronize())
            return false;
        if (forum_adapter->media_enabled) {
            if (!application_forum_preview_request(clock_monotonic_milliseconds(), 1759667400ull))
                return false;
            AccountMediaFrame frame{};
            while (account_media_worker_take_frame(forum_adapter->media_worker, &frame)) {
                if (!application_forum_preview_frame_current(frame))
                    continue;
                if (frame.kind != AccountMediaFrameKind::png_scanlines || !frame.image ||
                    !g_app.renderer.preview_media_atlas)
                    return false;
                const auto upload =
                    renderer_upload_png_scanlines(&g_app.renderer, frame.key, *frame.image, frame.bytes);
                if (!upload.ok())
                    return false;
                application_forum_preview_frame_uploaded(frame);
            }
        }
        return application_forum_post_synchronize() && application_text_form_synchronize() &&
               application_forum_form_synchronize();
    });
}
IDataObject* clipboard_attachment_ui_fixture_provider() noexcept {
    return clipboard_fixture_provider;
}
bool forum_media_ui_adapter_paste_file() noexcept {
    const DWORD sequence = GetClipboardSequenceNumber();
    const bool captured = call(+[](void*) noexcept {
        const auto failure = [](u32 stage, HRESULT result = S_OK) noexcept {
            log_u64(LogLevel::error, "probe", "forum_media_clipboard_failure_stage", stage);
            log_u64(LogLevel::error, "probe", "forum_media_clipboard_hresult", static_cast<u32>(result));
            return false;
        };
        if (!forum_adapter || !forum_adapter->media_file[0] || clipboard_fixture_provider)
            return failure(1);
        const auto ticket = application_forum_post_view().ticket;
        const u64 deadline = GetTickCount64() + 10000;
        while (!application_forum_attachments_view(ticket).can_add && GetTickCount64() < deadline) {
            if (!forum_media_ui_adapter_pump())
                return failure(2);
            Sleep(1);
        }
        if (!application_forum_attachments_view(ticket).can_add)
            return failure(3);
        // A Shell provider is injected only into UiProbe's capture boundary.
        // The common capture still marshals it into the real native worker.
        const HRESULT initialized = OleInitialize(nullptr);
        if (FAILED(initialized))
            return failure(4, initialized);
        struct OleLifetime {
            ~OleLifetime() noexcept {
                OleUninitialize();
            }
        } ole;
        struct SourceLifetime {
            IShellItem* item = nullptr;
            IDataObject* object = nullptr;
            ~SourceLifetime() noexcept {
                if (clipboard_fixture_provider == object)
                    clipboard_fixture_provider = nullptr;
                com_release(object);
                com_release(item);
            }
        } source;
        HRESULT native =
            SHCreateItemFromParsingName(forum_adapter->media_file, nullptr, IID_PPV_ARGS(&source.item));
        if (FAILED(native))
            return failure(5, native);
        native = source.item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&source.object));
        if (FAILED(native))
            return failure(6, native);
        clipboard_fixture_provider = source.object;
        if (!clipboard_attachment_available())
            return failure(7);
        BYTE prior[256]{}, keys[256]{}, restored[256]{};
        if (!GetKeyboardState(prior))
            return failure(8);
        keys[VK_CONTROL] = 0x80;
        u32 stage = 9;
        bool ok = SetKeyboardState(keys) != FALSE;
        if (ok) {
            stage = 10;
            const MSG message{g_app.window, WM_KEYDOWN, 'V', 0, 0, {}};
            ok = application_forum_form_filter_message(message);
            application_forum_form_input(UiInputKind::key_up, 'V', false);
            if (ok && !application_forum_attachments_view(ticket).busy) {
                stage = 11;
                ok = false;
            }
        }
        // Report intake and restoration independently so a cleanup failure
        // cannot hide the primary diagnosis. Restore before pumping the worker.
        if (!ok)
            failure(stage);
        bool keyboard_restored = SetKeyboardState(prior) != FALSE && GetKeyboardState(restored) != FALSE;
        if (keyboard_restored)
            for (u32 i = 0; i < 256; ++i)
                if (prior[i] != restored[i]) {
                    keyboard_restored = false;
                    break;
                }
        if (!keyboard_restored) {
            log_u64(LogLevel::error, "probe", "forum_media_clipboard_keyboard_restore_failed", 1);
            return false;
        }
        if (!ok)
            return false;
        // Keep the borrowed provider alive until the real picker has reaped its
        // marshalled work. Only OLE RPC windows are dispatched during the wait.
        const u64 prepare_deadline = GetTickCount64() + 10000;
        while (application_forum_attachments_view(ticket).busy && GetTickCount64() < prepare_deadline) {
            if (!forum_media_ui_adapter_pump())
                return failure(12);
            Sleep(1);
        }
        const auto prepared = application_forum_attachments_view(ticket);
        if (prepared.busy || !prepared.can_add || prepared.rows.count != 2)
            return failure(13);
        return true;
    });
    if (GetClipboardSequenceNumber() != sequence) {
        log_u64(LogLevel::error, "probe", "forum_media_clipboard_sequence_changed", 1);
        return false;
    }
    return captured;
}
bool forum_media_ui_adapter_upload(u64 committed, bool complete, bool failed) noexcept {
    if (!forum_adapter || !forum_adapter->upload.id)
        return false;
    auto& result = forum_adapter->upload;
    result.transfer.committed_bytes = minimum(committed, result.transfer.file_bytes);
    result.transfer.phase = complete ? HttpsFileUploadPhase::complete : HttpsFileUploadPhase::sending;
    result.transfer.status_code = complete ? (failed ? 500 : 200) : 0;
    result.transfer.error = failed ? HttpsFileUploadError::http : HttpsFileUploadError::none;
    return forum_media_ui_adapter_pump();
}
bool forum_media_ui_adapter_url(Utf8View* body) noexcept {
    if (!forum_adapter || !body ||
        forum_adapter->flight.operation != ForumPostOperation::attachment_url)
        return false;
    ProtocolWriter writer{{forum_adapter->response, sizeof(forum_adapter->response)}, 0};
    if (!protocol_writer_append_literal(&writer, "{\"attachments\":[{\"id\":") ||
        !protocol_writer_append_u64(&writer, forum_adapter->flight.upload_id) ||
        !protocol_writer_append_literal(&writer,
                                        ",\"upload_url\":\"https://uploads.example.com/"
                                        "native-probe\",\"upload_filename\":\"probe/native-image.png\"}]}"))
        return false;
    *body = {reinterpret_cast<const char*>(writer.output.data), writer.used};
    return true;
}
bool forum_media_ui_adapter_preview_ready() noexcept {
    const auto view = application_forum_attachments_view(application_forum_post_view().ticket);
    if (!view.rows.count)
        return false;
    return application_forum_preview_view(application_forum_post_view().ticket, view.rows[0].id).ready;
}
bool forum_media_ui_adapter_reveal_narrow(u64 draft_id) noexcept {
    return call(
        +[](void* opaque) noexcept {
            const auto id = *static_cast<u64*>(opaque);
            auto* scene = application_forum_form_scene();
            if (!scene || !application_forum_attachments_current(application_forum_post_view().ticket, id))
                return false;
            const auto* heading = forum_post_node(*scene, ForumPostElement::heading);
            const auto* actions = forum_post_node(*scene, ForumPostElement::actions);
            if (!heading || !actions)
                return false;
            const auto footer = actions->rectangle;
            const auto scrollbar = forum_post_scrollbar(*scene);
            if (!scrollbar.visible)
                return false;
            const auto fully_visible = [&](UiWidgetHandle handle) noexcept {
                const auto* node = ui_widget_manager_layout_node(scene->widgets, handle);
                return node && ui_rect_has_area(node->rectangle) && node->rectangle.bottom <= footer.top &&
                       ui_rect_equal(ui_rect_intersection(node->rectangle, node->clip_rectangle),
                                     node->rectangle);
            };
            const UiAttachmentDraftAction controls[]{
                UiAttachmentDraftAction::remove, UiAttachmentDraftAction::retry,
                UiAttachmentDraftAction::spoiler, UiAttachmentDraftAction::describe};
            const auto ready = [&]() noexcept {
                for (const auto control : controls) {
                    const auto handle =
                        ui_attachment_draft_list_handle(scene->attachment_list, id, control);
                    const auto* node = ui_widget_manager_layout_node(scene->widgets, handle);
                    if (!node ||
                        ((node->spec.style.flags & ui_layout_node_visible) && !fully_visible(handle)))
                        return false;
                }
                if (!fully_visible(scene->elements[static_cast<u32>(ForumPostElement::tags_hint)]))
                    return false;
                for (u32 tag = 0; tag < scene->content.tags.count; ++tag)
                    if (!fully_visible(scene->tags[tag]))
                        return false;
                return true;
            };
            POINT point{static_cast<LONG>((heading->rectangle.left + heading->rectangle.right) * .5f *
                                          g_app.renderer.dpi / 96.f),
                        static_cast<LONG>((heading->rectangle.top + heading->rectangle.bottom) * .5f *
                                          g_app.renderer.dpi / 96.f)};
            if (!ClientToScreen(g_app.window, &point))
                return false;
            for (u32 step = 0; !ready() && step < 32; ++step) {
                LRESULT result = 1;
                if (!application_forum_form_window_message(
                        WM_MOUSEWHEEL, MAKEWPARAM(0, static_cast<WORD>(-WHEEL_DELTA)),
                        MAKELPARAM(static_cast<short>(point.x), static_cast<short>(point.y)), &result) ||
                    result != 0)
                    return false;
            }
            if (!ready() ||
                !ui_rect_equal(footer, forum_post_node(*scene, ForumPostElement::actions)->rectangle))
                return false;
            for (const auto control : controls) {
                const auto handle = ui_attachment_draft_list_handle(scene->attachment_list, id, control);
                if (!ui_widget_manager_input_enabled(scene->widgets, handle))
                    continue;
                const auto* node = ui_widget_manager_layout_node(scene->widgets, handle);
                const auto hit =
                    forum_post_hit_test(*scene, (node->rectangle.left + node->rectangle.right) * .5f,
                                        (node->rectangle.top + node->rectangle.bottom) * .5f);
                if (hit.element != ForumPostElement::attachments || hit.attachment_id != id ||
                    hit.attachment_action != control || hit.attachment_widget.slot != handle.slot ||
                    hit.attachment_widget.generation != handle.generation)
                    return false;
            }
            const auto scrolled = forum_post_scrollbar(*scene);
            if (!scrolled.visible || scrolled.thumb.top <= scrollbar.thumb.top ||
                scrolled.track.bottom > footer.top)
                return false;
            log_write(LogLevel::info, "probe",
                      "forum_media_actual_narrow_wheel_scroll_full_row_actions_tags_and_persistent_footer_"
                      "passed");
            return true;
        },
        &draft_id);
}
bool forum_media_ui_adapter_child() noexcept {
    return application_text_form_forum_child(application_forum_post_view().ticket);
}
UiWidgetManager* forum_media_ui_adapter_focus() noexcept {
    return g_app.text_form                  ? &g_app.text_form->scene.widgets
           : application_forum_form_scene() ? &application_forum_form_scene()->widgets
                                            : nullptr;
}
bool forum_media_ui_adapter_description_saved(u64 id, Utf8View text) noexcept {
    const auto rows = application_forum_attachments_view(application_forum_post_view().ticket).rows;
    for (u32 i = 0; i < rows.count; ++i)
        if (rows[i].id == id)
            return utf8_equal(rows[i].description, text);
    return false;
}
} // namespace maicord
