#include "../src/app/application_attachments_state.hpp"
#include "../src/app/application_state.hpp"
#include "../src/app/application_text_form.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/render/forum_post_surface.hpp"
#include "maicord/ui/forum_post_scene.hpp"
#include "../src/app/account_ui_runtime.hpp"
#include "file_drop_fixture.hpp"

#include "maicord/sys/shell.hpp"
#include "maicord/ui/attachment_menu.hpp"

// Drive the shipping attachment owner with a deterministic account boundary.
// No file dialog, DNS, credentials, or external service participates here.
namespace maicord::app_detail {
ApplicationTextFormRequest captured_form{};
bool captured_form_open = false;
bool application_text_form_open(const ApplicationTextFormRequest& request) noexcept {
    captured_form = request; captured_form_open = true; return true;
}
void application_text_form_cancel_owner(TextFormOwnerValid validator) noexcept {
    if (captured_form.valid == validator) { captured_form = {}; captured_form_open = false; }
}
bool refresh_account_frame(bool) noexcept { return true; }
bool refresh_message_composer_ui() noexcept { return true; }
u32 history_pump_calls = 0;
bool pump_message_history() noexcept { ++history_pump_calls; return true; }
UiRect application_viewport_dips() noexcept { return {0, 0, 800, 600}; }
void set_popup_owner(PopupOwnerKind owner) noexcept { g_app.popup_owner_kind = owner; }
bool application_poll_create_available(u64) noexcept { return false; }
bool application_poll_create_open(u64) noexcept { return false; }
}
namespace maicord {
namespace {
struct FakeUpload {
    AccountUploadResult result;
    u64 channel, request, active;
    u32 file_id, starts, cancels;
    bool release_ready;
    bool source_correct;
};
RestClientSubmitResult WINAPI request_url(void* context, u64 channel, u32 id, Utf8View name,
    u64 bytes, u64 request) noexcept {
    auto& fake = *static_cast<FakeUpload*>(context);
    fake.channel = channel; fake.file_id = id; fake.request = request;
    fake.source_correct = utf8_equal(name, utf8_literal("a.txt")) && bytes == 8;
    return {};
}
AccountUploadResult WINAPI upload(void* context, AccountUploadOperation operation, u64 id, const AccountUploadRequest* request) noexcept {
    auto& fake = *static_cast<FakeUpload*>(context);
    if (operation == AccountUploadOperation::start) {
        ++fake.starts; fake.active = id;
        fake.source_correct = fake.source_correct && request && request->path.count == 8 &&
            request->source.bytes == 8 && request->source.identity.index == 9 &&
            utf8_equal(request->url, utf8_literal("https://upload.example.com/file?signature=fixture"));
    } else if (operation == AccountUploadOperation::cancel) ++fake.cancels;
    auto result = fake.result; result.id = id;
    result.owns_transfer = fake.active == id;
    if (operation == AccountUploadOperation::release) {
        result.released = fake.release_ready;
        if (result.released) { fake.active = 0; result.owns_transfer = false; }
    }
    return result;
}
Utf8View response_body(u32 id, Slice<u8> buffer) noexcept {
    ProtocolWriter writer{buffer, 0};
    protocol_writer_append_literal(&writer, "{\"attachments\":[{\"id\":\"");
    protocol_writer_append_u64(&writer, id);
    protocol_writer_append_literal(&writer, "\",\"upload_url\":\"https://upload.example.com/file?signature=fixture\",\"upload_filename\":\"fixture/a.txt\"}]}");
    return {reinterpret_cast<char*>(buffer.data), writer.used};
}
// Joins the drop worker instead of polling the nonblocking release.
void release_attachments() noexcept {
    using namespace app_detail;
    while (!application_release_attachments()) WaitForSingleObject(g_app.attachments->picker.thread, INFINITE);
}
}
void test_application_attachments(void (*check)(bool, const char*) noexcept) noexcept {
    using namespace app_detail;
    FakeUpload fake{};
    g_app.account_ui = static_cast<AccountUiRuntime*>(VirtualAlloc(nullptr, sizeof(AccountUiRuntime), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    g_app.attachments = static_cast<ApplicationAttachments*>(VirtualAlloc(nullptr, sizeof(ApplicationAttachments), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!g_app.account_ui || !g_app.attachments) { check(false, "attachment application fixture allocation"); return; }
    auto* state = g_app.attachments;
    PopupItem popup_items[16]{}; PopupRowWidget popup_rows[16]{};
    UiWidget popup_widgets[34]{}; UiLayoutNode popup_nodes[34]{};
    check(popup_scene_initialize(&g_app.popup_scene, {popup_items, 16}, {popup_rows, 16},
        {popup_widgets, 34}, {popup_nodes, 34}), "attachment menus share the native popup scene");
    g_app.account.instance = &fake; g_app.account.status.profile_id = 77; g_app.account.status.running = true;
    g_app.account.api.attachment_upload = request_url; g_app.account.api.upload = upload;
    g_app.account_ui->active_channel_id = 7; state->profile_id = 77;
    const wchar_t path[] = L"C:\\a.txt";
    const MessageAttachmentFile file{{path, 8}, {{9, 10, 11, 0}, 8}};
    u64 id = 0;
    check(message_attachment_drafts_initialize(&state->drafts) &&
        message_attachment_drafts_add(&state->drafts, 7, {&file, 1}, {&id, 1}) == MessageAttachmentDraftError::none,
        "attachment application stages original file identity");
    copy_bytes(state->notice, "Note", 4); state->notice_bytes = 4;
    check(message_attachment_drafts_describe(&state->drafts, id, utf8_literal("A diagram"), false) &&
        application_manage_attachments() && g_app.popup_scene.items[0].action == popup_action_attachment_title &&
        !(g_app.popup_scene.items[0].flags & popup_item_enabled) && application_attachment_action(popup_action_attachment_file_first) &&
        state->detail_id == id && state->drafts.count == 1 && g_app.popup_scene.item_count == kAttachmentFileMenuItems,
        "selecting a staged file opens its controls without removing it");
    state->notice_bytes = 0;
    const u32 committed_before_toggle = state->drafts.committed_bytes;
    check(application_attachment_action(popup_action_attachment_spoiler) &&
        message_attachment_drafts_get(&state->drafts, id)->spoiler && g_app.popup_scene.active &&
        g_app.popup_owner_kind == PopupOwnerKind::attachments && (g_app.popup_scene.items[1].flags & popup_item_checked) &&
        state->drafts.committed_bytes == committed_before_toggle && !fake.starts,
        "spoiler control updates checked state before upload without allocation or transfer");
    const u32 spoiler_revision = state->drafts.revision;
    check(message_attachment_drafts_set_spoiler(&state->drafts, id, true) && state->drafts.revision == spoiler_revision,
        "unchanged spoiler state does not invalidate the draft store");
    check(application_attachment_action(popup_action_attachment_description) && captured_form_open &&
        !g_app.popup_scene.active && !state->detail_id && captured_form.owner.item == id &&
        captured_form.maximum_codepoints == 1024 && utf8_equal(captured_form.initial, utf8_literal("A diagram")),
        "description action prepopulates the generic form and closes its menu");
    const auto description_request = captured_form;
    g_app.account_ui->active_channel_id = 8;
    check(!description_request.valid(description_request.owner) &&
        !description_request.save(description_request.owner, utf8_literal("wrong channel")),
        "description save rejects channel changes");
    g_app.account_ui->active_channel_id = 7; g_app.account.status.profile_id = 78;
    check(!description_request.valid(description_request.owner) &&
        !description_request.save(description_request.owner, utf8_literal("wrong profile")),
        "description save rejects profile changes");
    g_app.account.status.profile_id = 77;
    check(description_request.save(description_request.owner, utf8_literal("A diagram\nTwo views \xF0\x9F\x98\x80")) &&
        utf8_equal(message_attachment_drafts_description(&state->drafts, id), utf8_literal("A diagram\nTwo views \xF0\x9F\x98\x80")) &&
        message_attachment_drafts_get(&state->drafts, id)->spoiler && !fake.starts,
        "description save retains multiline Unicode and spoiler without restarting upload");
    check(description_request.save(description_request.owner, {}) && !message_attachment_drafts_description(&state->drafts, id).bytes &&
        description_request.save(description_request.owner, utf8_literal("A diagram")), "description save supports clearing and replacing metadata");
    application_text_form_cancel_owner(description_request.valid);
    check(!captured_form_open && utf8_equal(message_attachment_drafts_description(&state->drafts, id), utf8_literal("A diagram")),
        "description cancellation preserves saved metadata");
    check(application_manage_attachments() && application_attachment_action(popup_action_attachment_file_first),
        "attachment controls reopen after description editing");
    {
        // Upload strip projection and per-tile actions.
        const auto strip = application_attachment_input();
        check(strip.tiles.count == 1 && strip.tiles[0].id == id && strip.tiles[0].spoiler && strip.tiles[0].described &&
                  strip.tiles[0].phase == UiComposerAttachmentPhase::waiting &&
                  strip.tiles[0].kind == UiAttachmentFileKind::document && strip.tiles[0].bytes == 8 &&
                  utf8_equal(strip.tiles[0].filename, utf8_literal("a.txt")) && !strip.tiles[0].preview_ready &&
                  !strip.tiles[0].can_retry && !strip.tiles[0].animated &&
                  message_composer_attachment_row(strip) == message_composer_attachment_row_strip,
              "upload strip projects each staged draft as a tile with its metadata");
        check(application_attachment_toggle_spoiler(id) && !message_attachment_drafts_get(&state->drafts, id)->spoiler &&
                  application_attachment_toggle_spoiler(id) && message_attachment_drafts_get(&state->drafts, id)->spoiler &&
                  !fake.starts,
              "strip spoiler action toggles the draft without starting a transfer");
        check(!application_attachment_retry(id), "strip retry ignores drafts that have not failed");
        const UiRect anchor{10, 10, 38, 38};
        check(application_attachment_modify(id, anchor) && g_app.popup_scene.active &&
                  g_app.popup_scene.item_count == kAttachmentModifyMenuItems && state->detail_id == id &&
                  g_app.popup_scene.items[1].action == popup_action_attachment_rename &&
                  g_app.popup_scene.items[2].action == popup_action_attachment_description,
              "strip modify action offers filename and description (alt text)");
        check(application_attachment_action(popup_action_attachment_rename) && captured_form_open &&
                  !g_app.popup_scene.active && captured_form.single_line &&
                  utf8_equal(captured_form.initial, utf8_literal("a.txt")) &&
                  captured_form.maximum_bytes == kAttachmentUploadMaximumFilenameBytes,
              "rename opens a single-line form prefilled with the filename");
        const auto rename_request = captured_form;
        check(!rename_request.save(rename_request.owner, utf8_literal("dir/evil.txt")) &&
                  !rename_request.save(rename_request.owner, {}) &&
                  rename_request.save(rename_request.owner, utf8_literal("notes Ã©tÃ©.txt")) &&
                  utf8_equal(message_attachment_drafts_filename(&state->drafts, id), utf8_literal("notes Ã©tÃ©.txt")) &&
                  utf8_equal(message_attachment_drafts_description(&state->drafts, id), utf8_literal("A diagram")),
              "rename rejects separators and empty names and keeps the description");
        check(rename_request.save(rename_request.owner, utf8_literal("a.txt")), "rename restores the original filename");
        application_text_form_cancel_owner(rename_request.valid);
        const wchar_t second_path[] = L"C:\\b.png";
        const MessageAttachmentFile second{{second_path, 8}, {{12, 13, 14, 0}, 4096}};
        u64 second_id = 0;
        check(message_attachment_drafts_add(&state->drafts, 7, {&second, 1}, {&second_id, 1}) == MessageAttachmentDraftError::none &&
                  application_attachment_input().tiles.count == 2 &&
                  application_attachment_input().tiles[1].kind == UiAttachmentFileKind::image &&
                  application_attachment_remove(second_id) && state->drafts.count == 1 &&
                  application_attachment_input().tiles.count == 1 && !application_attachment_remove(second_id),
              "strip remove drops exactly one tile and rejects a stale id");
        g_app.account_ui->active_channel_id = 8;
        check(!application_attachment_toggle_spoiler(id) && !application_attachment_remove(id) &&
                  state->drafts.count == 1, "strip actions cannot mutate another channel's draft");
        g_app.account_ui->active_channel_id = 7;
    }
    check(application_attachment_input().label.bytes && !application_attachment_input().ready &&
        application_attachment_wait_timeout(0) == 0, "attachment application exposes waiting upload");
    g_app.message_history_request_owner = MessageHistoryRequestOwner::message_send;
    check(application_attachment_wait_timeout(0) == INFINITE, "attachment wait does not spin behind another REST request");
    g_app.message_history_request_owner = MessageHistoryRequestOwner::none;
    g_app.account_ui->active_channel_id = 8;
    check(application_attachment_action(popup_action_attachment_spoiler) && !g_app.popup_scene.active &&
        message_attachment_drafts_get(&state->drafts, id)->spoiler,
        "stale file controls cannot mutate another channel's attachment");
    bool submitted = false;
    check(application_attachment_submit_url(101, 0, &submitted) && submitted && fake.channel == 7 &&
        !application_attachment_input().label.bytes, "attachment URL request retains channel across navigation");
    RestClientResponse response{}; response.request_id = 101; response.status_code = 429; response.retry_at_ms = 300;
    check(application_attachment_accept_url(response, {}) && !application_attachment_url_pending(299) &&
        application_attachment_wait_timeout(299) == 1 && application_attachment_url_pending(300),
        "attachment URL rate limit waits for server deadline");
    check(application_attachment_submit_url(102, 300, &submitted) && submitted, "attachment URL retries after deadline");
    u8 body[512]{};
    response.request_id = 102; response.status_code = 200;
    check(application_attachment_accept_url(response, response_body(fake.file_id, {body, sizeof(body)})) &&
        fake.starts == 1 && fake.active == state->upload_lease.id && fake.source_correct && state->url[0] == 0,
        "attachment response starts matching immutable source and erases signed URL");
    fake.result.transfer.phase = HttpsFileUploadPhase::complete; fake.result.transfer.status_code = 200;
    fake.result.transfer.committed_bytes = 8;
    check(application_pump_attachments() && state->active_id == id && state->finishing,
        "attachment completion retains owner until worker join succeeds");
    fake.release_ready = true;
    check(application_pump_attachments() && !state->active_id && !state->uploaded_filename[0],
        "attachment worker join releases transient upload metadata");
    g_app.account_ui->active_channel_id = 7;
    check(application_attachment_modify(id, {}) && application_attachment_action(popup_action_attachment_rename) &&
              captured_form_open && captured_form.save(captured_form.owner, utf8_literal("diagram.txt")),
          "a ready upload can still be renamed before sending");
    application_text_form_cancel_owner(captured_form.valid);
    UploadedAttachment reference{}; u64 prepared_id = 0; u32 count = 0;
    u64 prepared_size = 0;
    check(application_attachment_input().ready && application_attachment_prepare_send(7, {&reference, 1}, {&prepared_id, 1}, &count, {&prepared_size, 1}) &&
        prepared_size == 8 &&
        count == 1 && prepared_id == id && reference.spoiler && utf8_equal(reference.description, utf8_literal("A diagram")) &&
        utf8_equal(reference.uploaded_filename, utf8_literal("fixture/a.txt")) &&
        utf8_equal(reference.filename, utf8_literal("diagram.txt")),
        "attachment ready file feeds message submission");
    {
        // Capture the submitted attachments array exactly as the REST body carries it.
        u8 submitted_body[512]{};
        const u32 bytes = attachment_upload_write_references({&reference, 1}, {submitted_body, sizeof(submitted_body)});
        check(bytes && utf8_equal({reinterpret_cast<const char*>(submitted_body), bytes},
                  utf8_literal("[{\"id\":\"0\",\"filename\":\"diagram.txt\",\"uploaded_filename\":\"fixture/a.txt\","
                               "\"description\":\"A diagram\",\"is_spoiler\":true}]")),
              "submission body carries the renamed filename, alt text and is_spoiler without a SPOILER_ rewrite");
    }
    application_attachment_did_queue({&id, 1}, 987);
    check(!message_attachment_drafts_set_spoiler(&state->drafts, id, false) &&
        message_attachment_drafts_get(&state->drafts, id)->spoiler,
        "queued attachment metadata remains immutable");
    check(!application_attachment_input().label.bytes && state->drafts.count == 1,
        "attachment queued send leaves composer and retains source for failure");
    application_attachment_complete(987);
    check(!state->drafts.count && !state->drafts.committed_bytes, "attachment accepted nonce releases staged file pages");

    const u64 previous_file = id;
    check(!description_request.valid(description_request.owner) &&
        !description_request.save(description_request.owner, utf8_literal("late save")),
        "description callback cannot mutate a delivered attachment");
    message_attachment_drafts_add(&state->drafts, 7, {&file, 1}, {&id, 1});
    state->detail_id = previous_file;
    check(application_attachment_action(popup_action_attachment_remove) && state->drafts.count == 1 &&
        message_attachment_drafts_get(&state->drafts, id) && id != previous_file,
        "stale file controls cannot remove a new generation in the reused slot");
    application_attachment_submit_url(103, 400, &submitted);
    check(application_manage_attachments() && application_attachment_action(popup_action_attachment_file_first) &&
        application_attachment_action(popup_action_attachment_remove) && !g_app.popup_scene.active,
        "per-file removal during URL request closes the shared popup");
    response.request_id = 103;
    check(application_attachment_accept_url(response, response_body(fake.file_id, {body, sizeof(body)})) &&
        fake.starts == 1 && !state->url_id, "attachment late URL response cannot resurrect removed file");

    message_attachment_drafts_add(&state->drafts, 7, {&file, 1}, {&id, 1});
    application_attachment_submit_url(104, 500, &submitted);
    response.request_id = 104;
    application_attachment_accept_url(response, response_body(fake.file_id, {body, sizeof(body)}));
    fake.result.transfer.error = HttpsFileUploadError::source_changed;
    application_pump_attachments();
    state->menu_ids[0] = id; state->menu_count = 1;
    check(application_attachment_action(popup_action_attachment_retry) && !application_attachment_url_pending(600) &&
        message_attachment_drafts_get(&state->drafts, id)->failure == MessageAttachmentDraftFailure::source_changed && state->notice_bytes == 0,
        "changed source cannot retry against stale identity");
    message_attachment_drafts_get(&state->drafts, id)->failure = MessageAttachmentDraftFailure::upload;
    check(application_attachment_action(popup_action_attachment_retry) && application_attachment_url_pending(600),
        "ordinary failed upload can request fresh URL");
    application_attachment_submit_url(105, 600, &submitted);
    state->url[0] = 'x'; state->uploaded_filename[0] = 'y'; response.request_id = 105;
    check(application_attachment_accept_url(response, utf8_literal("{}")) && !state->url[0] && !state->uploaded_filename[0],
        "invalid upload response erases partial sensitive fields");

    auto* failed_row = message_attachment_drafts_get(&state->drafts, id);
    failed_row->phase = MessageAttachmentDraftPhase::waiting_url;
    fake.result = {}; fake.result.submit_error = HttpsFileUploadError::memory;
    fake.release_ready = false;
    check(application_attachment_submit_url(106, 600, &submitted) && submitted,
        "attachment retained failed-start fixture admits typed URL");
    response.request_id = 106;
    check(application_attachment_accept_url(response, response_body(fake.file_id, {body, sizeof(body)})) &&
        state->active_id == id && state->finishing && !application_upload_lane_available() &&
        application_attachment_wait_timeout(600) != INFINITE,
        "attachment retained failed start keeps exact lease and timed reap owner");
    check(application_pump_attachments() && state->active_id == id,
        "attachment failed-start reap preserves callback storage before join");
    fake.release_ready = true;
    check(application_pump_attachments() && !state->active_id && application_upload_lane_available() &&
        message_attachment_drafts_get(&state->drafts, id)->phase == MessageAttachmentDraftPhase::failed,
        "attachment failed-start join releases transfer and preserves local draft");
    fake.result = {};

    check(application_upload_lane_acquire(ApplicationUploadOwner::composer, &state->upload_lease), "attachment shutdown acquires exact upload lane");
    application_upload_lane_operation(state->upload_lease, AccountUploadOperation::start);
    state->active_id = id; fake.release_ready = false;
    check(application_manage_attachments() && application_attachment_action(popup_action_attachment_file_first) &&
        application_attachment_action(popup_action_attachment_description) && captured_form_open,
        "description form can edit a failed attachment before shutdown");
    application_cancel_attachments();
    check(!captured_form_open, "attachment shutdown cancels its description form before releasing drafts");
    check(!application_release_attachments() && g_app.attachments == state && fake.cancels &&
        !application_attachment_input().label.bytes, "attachment shutdown retains live callback storage");
    fake.release_ready = true;
    check(application_release_attachments() && !g_app.attachments, "attachment shutdown reaps and releases complete owner");
    ChannelViewRow channels[2]{};
    channels[0].id = 7; channels[1].id = 8;
    channels[0].actions = channels[1].actions = channel_action_send_messages | channel_action_attach_files;
    g_app.account_ui->navigation_view.channel_rows = channels;
    g_app.account_ui->navigation_view.channel_count = 2;
    g_app.account_ui->active_channel_id = 7;
    g_app.window = CreateWindowExW(0, L"STATIC", L"MaiCord drop owner fixture", WS_OVERLAPPED,
        0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    wchar_t dropped_path[MAX_PATH]{}; u32 dropped_units = 0; FileSnapshot dropped_snapshot{};
    const bool drop_ready = g_app.window && file_drop_fixture::seed(dropped_path, &dropped_units, &dropped_snapshot);
    check(drop_ready, "attachment drop application fixture initializes");
    if (drop_ready) {
        const ConstSlice<wchar_t> dropped_file{dropped_path, dropped_units};
        application_sync_attachment_drop_target();
        check((GetWindowLongW(g_app.window, GWL_EXSTYLE) & WS_EX_ACCEPTFILES) != 0,
            "attachment drop registration follows active channel permissions");
        g_app.login.visible = true;
        application_sync_attachment_drop_target();
        HDROP dropped = file_drop_fixture::make({&dropped_file, 1});
        check(application_drop_attachments(dropped) && !g_app.attachments &&
            GlobalFlags(reinterpret_cast<HGLOBAL>(dropped)) == GMEM_INVALID_HANDLE &&
            (GetWindowLongW(g_app.window, GWL_EXSTYLE) & WS_EX_ACCEPTFILES) == 0,
            "login rejects and releases dropped file without allocating upload state");
        g_app.login.visible = false;
        ForumPostScene covered_scene{};
        ForumPostSurface covered_surface{};
        covered_scene.active = true;
        covered_surface.scene = &covered_scene;
        auto* previous_forum = g_app.renderer.forum_form;
        g_app.renderer.forum_form = &covered_surface;
        application_sync_attachment_drop_target();
        dropped = file_drop_fixture::make({&dropped_file, 1});
        check(application_drop_attachments(dropped) && !g_app.attachments &&
                  GlobalFlags(reinterpret_cast<HGLOBAL>(dropped)) == GMEM_INVALID_HANDLE &&
                  (GetWindowLongW(g_app.window, GWL_EXSTYLE) & WS_EX_ACCEPTFILES) == 0,
              "forum lifetime rejects covered composer drops without allocating upload state");
        g_app.renderer.forum_form = previous_forum;
        dropped = file_drop_fixture::make({&dropped_file, 1});
        check(application_drop_attachments(dropped) && g_app.attachments &&
                  g_app.attachments->picker_channel == 7 && application_attachment_input().label.bytes &&
                  !application_attachment_input().ready &&
                  !application_attachment_prepare_send(7, {&reference, 1}, {&prepared_id, 1}, &count) &&
                  (GetWindowLongW(g_app.window, GWL_EXSTYLE) & WS_EX_ACCEPTFILES) == 0,
              "accepted drop captures channel and temporarily disables additional drops");
        if (g_app.attachments) {
            const HANDLE original_worker = g_app.attachments->picker.thread;
            dropped = file_drop_fixture::make({&dropped_file, 1});
            check(application_drop_attachments(dropped) && g_app.attachments->picker.thread == original_worker &&
                GlobalFlags(reinterpret_cast<HGLOBAL>(dropped)) == GMEM_INVALID_HANDLE,
                "busy drop owner keeps original worker and releases a second delivered handle");
            g_app.account_ui->active_channel_id = 8;
            check(file_drop_fixture::await(&g_app.attachments->picker) && application_pump_attachments(),
                "application consumes real drop worker completion after channel switch");
            u64 dropped_id = 0;
            const u32 staged = message_attachment_drafts_channel(&g_app.attachments->drafts, 7, {&dropped_id, 1});
            const auto* source = message_attachment_drafts_get(&g_app.attachments->drafts, dropped_id);
            check(staged == 1 && source && source->source.bytes == 8 && file_identity_equal(source->source.identity, dropped_snapshot.identity) &&
                !application_attachment_input().label.bytes,
                "real dropped file stages on original channel with captured identity");
            release_attachments();
        }
        g_app.account_ui->active_channel_id = 7;
        dropped = file_drop_fixture::make({&dropped_file, 1});
        check(application_drop_attachments(dropped) && g_app.attachments &&
            application_attachment_action(popup_action_attachment_cancel_selection) &&
            utf8_equal(application_attachment_input().label, utf8_literal("Cancelling…")),
            "file preparation cancellation is visible without waiting for provider");
        if (g_app.attachments) {
            check(file_drop_fixture::await(&g_app.attachments->picker) && application_pump_attachments() &&
                !g_app.attachments->drafts.count,
                "user cancellation rejects a completed drop batch before application consumption");
            release_attachments();
        }
        dropped = file_drop_fixture::make({&dropped_file, 1});
        check(application_drop_attachments(dropped) && g_app.attachments, "drop starts before account changes");
        check(application_manage_attachments() && g_app.popup_scene.active, "attachment menu opens during preparation");
        const u32 starts = fake.starts;
        g_app.account.status.profile_id = 78;
        application_pump_attachments();
        if (g_app.attachments && WaitForSingleObject(g_app.attachments->picker.thread, 10000) == WAIT_OBJECT_0)
            application_pump_attachments();
        check(!g_app.attachments && fake.starts == starts && !g_app.popup_scene.active,
            "drop finishing after account change is drained without staging or uploading");
        release_attachments();
        g_app.account.status.profile_id = 77;
        check((GetWindowLongW(g_app.window, GWL_EXSTYLE) & WS_EX_ACCEPTFILES) == 0,
            "attachment cancellation unregisters the window drop target");
    }
    if (dropped_path[0]) DeleteFileW(dropped_path);
    if (g_app.window) DestroyWindow(g_app.window);
    g_app.window = nullptr;
    popup_scene_release(&g_app.popup_scene); g_app.popup_scene = {};
    VirtualFree(g_app.account_ui, 0, MEM_RELEASE); g_app.account_ui = nullptr; g_app.account = {};
}
}
