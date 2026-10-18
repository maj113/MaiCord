#include "application_forum_attachments_state.hpp"
#include "application_state.hpp"
#include "application_attachments.hpp"
#include "application_forum_preview.hpp"
#include "maicord/core/deadline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/time.hpp"
#include <Windows.h>
#include <shellapi.h>
namespace maicord::app_detail {
namespace {
ApplicationForumAttachments* forum_attachments{};
static_assert(sizeof(ApplicationForumAttachments) <= 24576);
bool key(const ApplicationForumAttachments& owner, const ApplicationForumPostTicket& ticket) noexcept {
    return owner.ticket == ticket;
}
const ChannelRecord* parent(const ApplicationForumPostTicket& ticket) noexcept {
    return g_app.account_ui ? chat_state_store_find_channel(g_app.account_ui->chat_state, ticket.parent_id) : nullptr;
}
bool source(const ApplicationForumAttachments& owner) noexcept {
    const auto* p = parent(owner.ticket);
    return !owner.closing && application_forum_post_current(owner.ticket) && g_app.account_ui == owner.ui &&
        g_app.account.instance == owner.instance && g_app.plugin_account_scope_serial == owner.scope &&
        g_app.account.status.profile_id == owner.profile && g_app.account_ui->chat_state.self_user_id == owner.self &&
        p && p->occupied && p->generation == owner.parent_generation && p->type == owner.parent_type && p->guild_id == owner.guild;
}
bool authorized(const ApplicationForumPostTicket& ticket) noexcept {
    const auto* p = parent(ticket);
    constexpr u32 needed = channel_action_view | channel_action_read_history |
        channel_action_send_messages | channel_action_attach_files;
    return application_forum_post_current(ticket) && p && p->permissions_resolved &&
        !(p->flags & kChannelObfuscatedFlag) && (p->actions & needed) == needed &&
        g_app.account.status.running && !g_app.account.stop_failed &&
        g_app.account.status.required_action == AccountRequirement::none;
}
void changed(ApplicationForumAttachments& owner) noexcept {
    ++owner.revision; application_forum_post_attachment_changed(owner.ticket); application_request_frame();
}
void status(ApplicationForumAttachments& owner, Utf8View text, bool error = true) noexcept {
    owner.status_bytes = minimum(text.bytes, static_cast<u32>(sizeof(owner.status)));
    copy_bytes(owner.status, text.data, owner.status_bytes); owner.error = error; changed(owner);
}
void failed(ApplicationForumAttachments& owner, u64 id, MessageAttachmentDraftFailure failure, u16 code = 0) noexcept {
    auto* row = message_attachment_drafts_get(&owner.drafts, id);
    if (!row) return;
    if (row->phase == MessageAttachmentDraftPhase::failed && row->failure == failure && row->http_status == code) return;
    row->phase = MessageAttachmentDraftPhase::failed; row->failure = failure; row->http_status = code;
    ++owner.drafts.revision; changed(owner);
}
bool editable(const ApplicationForumAttachments& owner) noexcept {
    return source(owner) && !owner.locked && application_forum_post_editable(owner.ticket);
}
void cancel(ApplicationForumAttachments& owner) noexcept {
    owner.picker_cancelled = true;
    native_file_picker_cancel(&owner.picker);
    if (owner.active_id) application_upload_lane_cancel(owner.lease);
}
bool reap(ApplicationForumAttachments& owner) noexcept {
    const bool picker = native_file_picker_release(&owner.picker, 0);
    const bool upload = application_upload_lane_release(&owner.lease);
    if (!picker || !upload) return false;
    const bool close = owner.close_window;
    message_attachment_drafts_release(&owner.drafts);
    if (owner.receipt) { SecureZeroMemory(owner.receipt, owner.receipt_bytes); VirtualFree(owner.receipt, 0, MEM_RELEASE); }
    SecureZeroMemory(&owner, sizeof(owner)); VirtualFree(&owner, 0, MEM_RELEASE); forum_attachments = nullptr;
    if (close && g_app.window) PostMessageW(g_app.window, WM_CLOSE, 0, 0);
    return true;
}
bool picked_start(ApplicationForumAttachments& owner, NativeFilePickerError result) noexcept {
    if (result != NativeFilePickerError::none) { status(owner, utf8_literal("Could not prepare these attachments.")); return false; }
    owner.picker_consumed = owner.picker_cancelled = false; owner.error = false; owner.status_bytes = 0;
    changed(owner); return true;
}
Utf8View row_status(const MessageAttachmentDraft& row) noexcept {
    if (row.phase == MessageAttachmentDraftPhase::ready) return utf8_literal("Ready");
    if (row.phase == MessageAttachmentDraftPhase::uploading) return utf8_literal("Uploading…");
    if (row.phase == MessageAttachmentDraftPhase::waiting_url) return utf8_literal("Waiting to upload…");
    if (row.failure == MessageAttachmentDraftFailure::source_changed) return utf8_literal("File changed. Remove and select it again.");
    if (row.failure == MessageAttachmentDraftFailure::size_limit) return utf8_literal("File is too large.");
    return utf8_literal("Upload failed. Retry or remove this file.");
}
}
bool application_forum_attachments_begin(const ApplicationForumPostTicket& ticket) noexcept {
    if (!application_forum_post_current(ticket)) return false;
    if (forum_attachments && !key(*forum_attachments, ticket)) {
        application_forum_preview_retire(forum_attachments->ticket);
        forum_attachments->closing = true; cancel(*forum_attachments);
        if (!reap(*forum_attachments)) return false;
    }
    if (forum_attachments) return source(*forum_attachments);
    auto* owner = static_cast<ApplicationForumAttachments*>(VirtualAlloc(nullptr, sizeof(ApplicationForumAttachments), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!owner) return false;
    if (!message_attachment_drafts_initialize(&owner->drafts)) { VirtualFree(owner, 0, MEM_RELEASE); return false; }
    const auto* p = parent(ticket);
    owner->ticket = ticket; owner->instance = g_app.account.instance; owner->ui = g_app.account_ui;
    owner->scope = g_app.plugin_account_scope_serial; owner->profile = g_app.account.status.profile_id;
    owner->self = g_app.account_ui->chat_state.self_user_id; owner->guild = p->guild_id;
    owner->parent_generation = p->generation; owner->parent_type = p->type;
    owner->revision = 1; forum_attachments = owner; return true;
}
bool application_forum_attachments_current(const ApplicationForumPostTicket& ticket, u64 id) noexcept {
    return forum_attachments && key(*forum_attachments, ticket) && source(*forum_attachments) &&
        (!id || message_attachment_drafts_get(&forum_attachments->drafts, id));
}
ApplicationForumAttachmentView application_forum_attachments_view(const ApplicationForumPostTicket& ticket) noexcept {
    const bool host_edit = application_forum_post_editable(ticket);
    const bool allowed = host_edit && authorized(ticket);
    if (!forum_attachments) return {{}, {}, 0, allowed, false, true, false};
    auto& owner = *forum_attachments;
    if (!key(owner, ticket)) return {{}, {}, owner.revision, false, true, true, false};
    if (!source(owner)) return {{}, {}, owner.revision, false, true, false, false};
    u64 ids[kAttachmentUploadMaximumCount]{};
    const u32 count = message_attachment_drafts_channel(&owner.drafts, ticket.parent_id, {ids, kAttachmentUploadMaximumCount});
    bool ready = !owner.picker.runtime && !owner.active_id && !owner.url_id && (!count || authorized(ticket));
    for (u32 i = 0; i < count; ++i) {
        const auto* row = message_attachment_drafts_get(&owner.drafts, ids[i]);
        const bool can_edit = host_edit && !owner.locked;
        owner.rows[i] = {ids[i], message_attachment_drafts_filename(&owner.drafts, ids[i]),
            message_attachment_drafts_description(&owner.drafts, ids[i]), row_status(*row), row->source.bytes,
            row->uploaded_bytes, row->phase, row->spoiler, can_edit,
            can_edit && allowed && row->phase == MessageAttachmentDraftPhase::failed && row->failure != MessageAttachmentDraftFailure::source_changed};
        ready = ready && row->phase == MessageAttachmentDraftPhase::ready && row->source.bytes <= application_attachment_limit(ticket.parent_id);
    }
    return {{owner.rows, count}, {owner.status, owner.status_bytes}, owner.revision + owner.drafts.revision,
        allowed && !owner.locked && !owner.picker.runtime && count < kAttachmentUploadMaximumCount,
        owner.picker.runtime || owner.active_id || owner.url_id, ready, owner.error};
}
bool application_forum_attachments_pick(const ApplicationForumPostTicket& ticket) noexcept {
    if (!authorized(ticket) || !application_forum_attachments_begin(ticket) || !application_forum_attachments_view(ticket).can_add) return false;
    auto& owner = *forum_attachments;
    const auto count = message_attachment_drafts_channel(&owner.drafts, ticket.parent_id, {});
    return picked_start(owner, native_file_picker_start(&owner.picker, g_app.window, kAttachmentUploadMaximumCount - count, utf8_literal("Attach files to this post")));
}
bool application_forum_attachments_drop(const ApplicationForumPostTicket& ticket, HANDLE drop) noexcept {
    if (!drop) return true;
    if (!authorized(ticket) || !application_forum_attachments_begin(ticket) || !application_forum_attachments_view(ticket).can_add) {
        DragFinish(static_cast<HDROP>(drop)); return true;
    }
    auto& owner = *forum_attachments;
    const auto count = message_attachment_drafts_channel(&owner.drafts, ticket.parent_id, {});
    const auto started = native_file_picker_take_drop(&owner.picker, drop, kAttachmentUploadMaximumCount - count);
    if (started != NativeFilePickerError::none) DragFinish(static_cast<HDROP>(drop));
    return picked_start(owner, started);
}
bool application_forum_attachments_paste(const ApplicationForumPostTicket& ticket) noexcept {
    if (!authorized(ticket) || !application_forum_attachments_begin(ticket) || !application_forum_attachments_view(ticket).can_add) return false;
    auto& owner = *forum_attachments;
    ClipboardAttachmentData input{};
    if (!clipboard_attachment_capture(&input)) { status(owner, utf8_literal("Could not read the clipboard attachment.")); return false; }
    const auto count = message_attachment_drafts_channel(&owner.drafts, ticket.parent_id, {});
    const auto result = native_file_picker_take_clipboard(&owner.picker, &input, kAttachmentUploadMaximumCount - count, application_attachment_limit(ticket.parent_id));
    clipboard_attachment_release(&input); return picked_start(owner, result);
}
bool application_forum_attachments_remove(const ApplicationForumPostTicket& ticket, u64 id) noexcept {
    if (!application_forum_attachments_current(ticket, id) || !editable(*forum_attachments)) return false;
    auto& owner = *forum_attachments;
    if (owner.url_id == id && !application_forum_post_cancel_upload(ticket, id)) return false;
    if (owner.active_id == id) application_upload_lane_cancel(owner.lease);
    application_forum_preview_retire(ticket, id);
    const bool removed = message_attachment_drafts_remove(&owner.drafts, id);
    if (removed) { owner.error = false; owner.status_bytes = 0; changed(owner); }
    return removed;
}
bool application_forum_attachments_retry(const ApplicationForumPostTicket& ticket, u64 id) noexcept {
    if (!application_forum_attachments_current(ticket, id) || !editable(*forum_attachments) || !authorized(ticket)) return false;
    auto& owner = *forum_attachments;
    auto* row = message_attachment_drafts_get(&owner.drafts, id);
    if (row->phase != MessageAttachmentDraftPhase::failed || row->failure == MessageAttachmentDraftFailure::source_changed ||
        row->source.bytes > application_attachment_limit(ticket.parent_id)) return false;
    row->phase = MessageAttachmentDraftPhase::waiting_url; row->failure = MessageAttachmentDraftFailure::none;
    row->retry_at_ms = 0; row->uploaded_bytes = 0;
    owner.error = false; owner.status_bytes = 0; ++owner.drafts.revision; changed(owner); return true;
}
bool application_forum_attachments_spoiler(const ApplicationForumPostTicket& ticket, u64 id, bool value) noexcept {
    if (!application_forum_attachments_current(ticket, id) || !editable(*forum_attachments)) return false;
    const bool result = message_attachment_drafts_set_spoiler(&forum_attachments->drafts, id, value);
    if (result) changed(*forum_attachments); return result;
}
bool application_forum_attachments_describe(const ApplicationForumPostTicket& ticket, u64 id, Utf8View value) noexcept {
    if (!application_forum_attachments_current(ticket, id) || !editable(*forum_attachments)) return false;
    const auto* row = message_attachment_drafts_get(&forum_attachments->drafts, id);
    const bool result = message_attachment_drafts_describe(&forum_attachments->drafts, id, value, row->spoiler);
    if (result) changed(*forum_attachments); return result;
}
bool application_forum_attachments_source(const ApplicationForumPostTicket& ticket, u64 id,
    UploadSource* content, FileSnapshot* snapshot, ConstSlice<wchar_t>* path) noexcept {
    if (!content || !snapshot || !path || !application_forum_attachments_current(ticket, id)) return false;
    const auto* row = message_attachment_drafts_get(&forum_attachments->drafts, id);
    *content = row->content; *snapshot = row->source; *path = message_attachment_drafts_path(&forum_attachments->drafts, id); return true;
}
bool application_forum_attachments_synchronize() noexcept {
    if (!forum_attachments) return true;
    auto& owner = *forum_attachments;
    if (!source(owner)) { application_forum_preview_retire(owner.ticket); owner.closing = true; cancel(owner); reap(owner); return true; }
    if (!authorized(owner.ticket)) {
        cancel(owner);
        if (owner.active_id) failed(owner, owner.active_id, MessageAttachmentDraftFailure::request);
        if (owner.url_id) failed(owner, owner.url_id, MessageAttachmentDraftFailure::request);
        if (!owner.error) status(owner, utf8_literal("You no longer have permission to attach files here."));
    }
    if (owner.active_id && !owner.finishing) {
        const auto* row = message_attachment_drafts_get(&owner.drafts, owner.active_id);
        if (row && row->source.bytes > application_attachment_limit(owner.ticket.parent_id)) {
            application_upload_lane_cancel(owner.lease);
            failed(owner, owner.active_id, MessageAttachmentDraftFailure::size_limit);
            owner.finishing = true;
        }
    }
    return true;
}
bool application_forum_attachments_pump() noexcept {
    application_forum_attachments_synchronize();
    if (!forum_attachments || forum_attachments->closing) return true;
    auto& owner = *forum_attachments;
    const auto picked = native_file_picker_status(&owner.picker);
    if (picked.phase == NativeFilePickerPhase::complete && !owner.picker_consumed) {
        owner.picker_consumed = true;
        if (picked.ok() && !owner.picker_cancelled && authorized(owner.ticket)) {
            MessageAttachmentFile files[kAttachmentUploadMaximumCount]{}; u64 ids[kAttachmentUploadMaximumCount]{};
            bool valid = picked.count && picked.count <= kAttachmentUploadMaximumCount;
            const auto limit = application_attachment_limit(owner.ticket.parent_id);
            for (u32 i = 0; valid && i < picked.count; ++i) {
                files[i].path = native_file_picker_path(&owner.picker, i); files[i].filename = native_file_picker_filename(&owner.picker, i);
                files[i].content = native_file_picker_source(&owner.picker, i);
                valid = native_file_picker_snapshot(&owner.picker, i, &files[i].snapshot) && files[i].snapshot.bytes <= limit;
            }
            if (!valid || message_attachment_drafts_add(&owner.drafts, owner.ticket.parent_id, {files, picked.count}, {ids, kAttachmentUploadMaximumCount}) != MessageAttachmentDraftError::none)
                status(owner, utf8_literal("Couldn't attach these files. Posts can have up to 10 files."));
        } else if (!owner.picker_cancelled && picked.error != NativeFilePickerError::cancelled)
            status(owner, utf8_literal("Could not read the selected attachments."));
        changed(owner);
    }
    if (owner.picker_consumed && owner.picker.runtime && native_file_picker_release(&owner.picker, 0)) changed(owner);
    if (owner.active_id) {
        const auto progress = application_upload_lane_operation(owner.lease, AccountUploadOperation::status);
        auto* row = message_attachment_drafts_get(&owner.drafts, owner.active_id);
        if (row && row->uploaded_bytes != progress.transfer.committed_bytes) { row->uploaded_bytes = progress.transfer.committed_bytes; changed(owner); }
        if (!owner.finishing && (progress.transfer.phase == HttpsFileUploadPhase::complete || progress.submit_error != HttpsFileUploadError::none)) {
            if (row && progress.submit_error == HttpsFileUploadError::none && progress.transfer.ok() && authorized(owner.ticket) &&
                row->source.bytes <= application_attachment_limit(owner.ticket.parent_id)) {
                if (!message_attachment_drafts_uploaded(&owner.drafts, owner.active_id, {owner.uploaded_filename, owner.uploaded_bytes})) failed(owner, owner.active_id, MessageAttachmentDraftFailure::memory);
            } else if (row) failed(owner, owner.active_id,
                progress.transfer.error == HttpsFileUploadError::source_changed || progress.transfer.error == HttpsFileUploadError::file_size ?
                    MessageAttachmentDraftFailure::source_changed : MessageAttachmentDraftFailure::upload, progress.transfer.status_code);
            owner.finishing = true; changed(owner);
        }
        if (owner.finishing && application_upload_lane_release(&owner.lease)) {
            owner.active_id = 0; owner.finishing = false; owner.uploaded_bytes = 0;
            SecureZeroMemory(owner.uploaded_filename, sizeof(owner.uploaded_filename)); changed(owner);
        }
    }
    return true;
}
bool application_forum_attachments_url_candidate(const ApplicationForumPostTicket& ticket, u64 now,
    ForumPostRequest* output, u64* draft_id) noexcept {
    if (!output || !draft_id || !application_forum_attachments_current(ticket) || !authorized(ticket)) return false;
    auto& owner = *forum_attachments;
    if (owner.locked || owner.url_id || owner.active_id || !application_upload_lane_available()) return false;
    for (u32 i = 0; i < owner.drafts.count; ++i) {
        const u32 slot = owner.drafts.order[i]; const auto& row = owner.drafts.records[slot];
        if (!row.occupied || row.phase != MessageAttachmentDraftPhase::waiting_url || row.retry_at_ms > now) continue;
        const u64 id = (u64{row.generation} << 32) | (slot + 1);
        if (row.source.bytes > application_attachment_limit(ticket.parent_id)) { failed(owner, id, MessageAttachmentDraftFailure::size_limit); continue; }
        *output = {}; output->parent_id = ticket.parent_id; output->guild_id = owner.guild;
        output->operation = ForumPostOperation::attachment_url;
        output->upload_filename = message_attachment_drafts_filename(&owner.drafts, id);
        output->upload_bytes = row.source.bytes; output->upload_id = static_cast<u32>(id);
        *draft_id = id; return true;
    }
    return false;
}
bool application_forum_attachments_url_admitted(const ApplicationForumPostTicket& ticket, u64 id) noexcept {
    if (!application_forum_attachments_current(ticket, id) || !authorized(ticket)) return false;
    auto& owner = *forum_attachments;
    if (!application_upload_lane_acquire(ApplicationUploadOwner::forum, &owner.lease)) {
        return false;
    }
    owner.url_id = id; changed(owner); return true;
}
void application_forum_attachments_url_unadmitted(const ApplicationForumPostTicket& ticket, u64 id) noexcept {
    if (!forum_attachments || !key(*forum_attachments, ticket) || forum_attachments->url_id != id) return;
    forum_attachments->url_id = 0; application_upload_lane_release(&forum_attachments->lease);
}
void application_forum_attachments_url_response(const ApplicationForumPostTicket& ticket, u64 id,
    const RestClientResponse& response, Utf8View body) noexcept {
    if (!forum_attachments || !key(*forum_attachments, ticket)) return;
    auto& owner = *forum_attachments;
    if (owner.url_id != id) return;
    owner.url_id = 0;
    auto* row = message_attachment_drafts_get(&owner.drafts, id);
    const bool valid = row && source(owner) && authorized(ticket) && row->source.bytes <= application_attachment_limit(ticket.parent_id);
    const auto decoded = valid && response.transport_ok() && response.status_code >= 200 && response.status_code < 300 &&
        body.data && body.bytes && body.bytes == response.body_bytes && body.bytes <= kForumPostMaximumResponseBytes ?
        attachment_upload_parse_response(body, static_cast<u32>(id), {owner.url, sizeof(owner.url)}, {owner.uploaded_filename, sizeof(owner.uploaded_filename)}) : AttachmentUploadResponse{};
    if (!decoded.ok) {
        failed(owner, id, MessageAttachmentDraftFailure::request, response.status_code);
        if (response.status_code == 429 && row) row->retry_at_ms = response.retry_at_ms;
        application_upload_lane_release(&owner.lease);
    } else {
        const AccountUploadRequest request{{owner.url, decoded.url_bytes}, message_attachment_drafts_path(&owner.drafts, id), row->source, row->content};
        const auto started = application_upload_lane_operation(owner.lease, AccountUploadOperation::start, &request);
        if (started.submit_error == HttpsFileUploadError::none) {
            owner.active_id = id; owner.uploaded_bytes = decoded.uploaded_filename_bytes;
            row->phase = MessageAttachmentDraftPhase::uploading; owner.finishing = false;
        } else {
            failed(owner, id, MessageAttachmentDraftFailure::upload);
            if (!application_upload_lane_release(&owner.lease)) { owner.active_id = id; owner.finishing = true; }
        }
    }
    SecureZeroMemory(owner.url, sizeof(owner.url)); changed(owner);
}
bool application_forum_attachments_prepare(const ApplicationForumPostTicket& ticket,
    Slice<UploadedAttachment> refs, Slice<u64> sizes, u32* count) noexcept {
    if (!count) return false; *count = 0;
    if (!forum_attachments) return true;
    if (!key(*forum_attachments, ticket)) return true;
    if (!application_forum_attachments_current(ticket)) return false;
    auto& owner = *forum_attachments;
    u64 ids[kAttachmentUploadMaximumCount]{};
    const auto found = message_attachment_drafts_channel(&owner.drafts, ticket.parent_id, {ids, kAttachmentUploadMaximumCount});
    if (found > refs.count || found > sizes.count || owner.picker.runtime || owner.active_id || owner.url_id || (found && !authorized(ticket))) return false;
    for (u32 i = 0; i < found; ++i) {
        const auto* row = message_attachment_drafts_get(&owner.drafts, ids[i]);
        if (row->source.bytes > application_attachment_limit(ticket.parent_id) || !message_attachment_drafts_reference(&owner.drafts, ids[i], i, &refs[i])) return false;
        sizes[i] = row->source.bytes;
    }
    *count = found; return true;
}
void application_forum_attachments_lock(const ApplicationForumPostTicket& ticket, bool value) noexcept {
    if (!forum_attachments || !key(*forum_attachments, ticket)) return;
    forum_attachments->locked = value; changed(*forum_attachments);
}
void application_forum_attachments_refresh(const ApplicationForumPostTicket& ticket) noexcept {
    if (!application_forum_attachments_current(ticket) || !authorized(ticket)) return;
    auto& owner = *forum_attachments;
    owner.locked = false;
    for (auto& row : owner.drafts.records) if (row.occupied && row.phase == MessageAttachmentDraftPhase::ready) {
        row.phase = MessageAttachmentDraftPhase::waiting_url; row.uploaded_bytes = 0; row.uploaded_filename_bytes = 0;
    }
    ++owner.drafts.revision; changed(owner);
}
bool application_forum_attachments_decode_media(Utf8View body, const ForumPostRequest& request,
    u64 self, Slice<u32> scratch, ForumPostSnapshot* output) noexcept {
    if (!output || !forum_attachments) return false;
    auto& owner = *forum_attachments;
    if (!owner.receipt) {
        owner.receipt_bytes = (2u * sizeof(ForumPostMediaSnapshot) + 4095u) & ~4095u;
        owner.receipt = static_cast<ForumPostMediaSnapshot*>(VirtualAlloc(nullptr, owner.receipt_bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!owner.receipt) { owner.receipt_bytes = 0; return false; }
        // Two disjoint decoder banks share this single allocation and release.
        owner.transaction = owner.receipt + 1;
    }
    if (!forum_post_media_decode(body, request, self, scratch, owner.transaction, owner.receipt).ok()) return false;
    for (u32 i = 0; i < owner.receipt->attachment_count; ++i) owner.receipt_views[i] = forum_post_attachment_view(*owner.receipt, i);
    *output = owner.receipt->post; return true;
}
ConstSlice<GatewayMessageAttachment> application_forum_attachments_receipt() noexcept {
    return forum_attachments && forum_attachments->receipt ? ConstSlice<GatewayMessageAttachment>{forum_attachments->receipt_views, forum_attachments->receipt->attachment_count} : ConstSlice<GatewayMessageAttachment>{};
}
HANDLE application_forum_attachments_notification() noexcept { return forum_attachments && !forum_attachments->picker_consumed ? forum_attachments->picker.notification : nullptr; }
u64 application_forum_attachments_next_wake() noexcept {
    if (!forum_attachments) return 0;
    const auto now = clock_monotonic_milliseconds();
    const auto& owner = *forum_attachments;
    if (owner.closing || owner.active_id || (owner.picker.runtime && owner.picker_consumed)) return deadline_after(now, owner.closing ? 10 : 100);
    return 0;
}
u32 application_forum_attachments_wait_timeout(u64 now) noexcept {
    const auto wake = application_forum_attachments_next_wake();
    return wake ? static_cast<u32>(minimum<u64>(wake > now ? wake - now : 0, INFINITE - 1)) : INFINITE;
}
void application_forum_attachments_retire(const ApplicationForumPostTicket& ticket) noexcept {
    if (!forum_attachments || !key(*forum_attachments, ticket)) return;
    application_forum_preview_retire(ticket);
    forum_attachments->closing = true; cancel(*forum_attachments);
}
bool application_forum_attachments_release() noexcept {
    if (!forum_attachments) return true;
    application_forum_preview_retire(forum_attachments->ticket);
    forum_attachments->closing = true; cancel(*forum_attachments); return reap(*forum_attachments);
}
bool application_forum_attachments_defer_close() noexcept {
    if (!forum_attachments) return false;
    forum_attachments->close_window = true; return !application_forum_attachments_release();
}
ApplicationForumAttachmentMetrics application_forum_attachments_metrics() noexcept {
    if (!forum_attachments) return {};
    return {sizeof(ApplicationForumAttachments), (sizeof(ApplicationForumAttachments) + 4095u) & ~4095u,
        forum_attachments->drafts.committed_bytes, forum_attachments->receipt_bytes, forum_attachments->active_id != 0};
}
} // namespace maicord::app_detail
