#include "application_attachments_state.hpp"
#include "application_state.hpp"
#include "application_account_presentation.hpp"
#include "application_composer.hpp"
#include "application_message_history.hpp"
#include "application_text_form.hpp"
#include "application_poll_create.hpp"
#include "application_composer_preview.hpp"
#include "application_attachment_probe.hpp"
#include "maicord/app/message_attachment_drafts.hpp"
#include "maicord/app/voice_message.hpp"
#include "maicord/core/log.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/sys/shell.hpp"
#include "maicord/sys/winsock.hpp"

namespace maicord::app_detail {
namespace {
bool current(const ApplicationAttachments* state) noexcept {
    return state && !state->closing && g_app.account_ui && g_app.account.instance &&
        state->profile_id == g_app.account.status.profile_id;
}
bool can_attach() noexcept {
    if (!g_app.account_ui || !g_app.account.instance || !g_app.account.status.running ||
        g_app.login.visible ||
        (g_app.renderer.forum_form && g_app.renderer.forum_form->scene &&
         g_app.renderer.forum_form->scene->active) ||
        (g_app.renderer.settings_scene && g_app.renderer.settings_scene->active) ||
        g_app.message_edit.editor_active || (g_app.attachments && g_app.attachments->closing))
        return false;
    const auto* channel = navigation_view_find_channel(g_app.account_ui->navigation_view,
                                                               g_app.account_ui->active_channel_id);
    constexpr u32 actions = channel_action_send_messages | channel_action_attach_files;
    return channel && (channel->actions & actions) == actions;
}
bool description_owner_valid(TextFormOwnerKey key) noexcept {
    const auto* state = g_app.attachments;
    if (!current(state) || state->profile_id != key.scope || g_app.account_ui->active_channel_id != key.collection) return false;
    const auto* record = message_attachment_drafts_get(&state->drafts, key.item);
    return record && record->channel_id == key.collection && record->phase != MessageAttachmentDraftPhase::queued;
}
bool save_description(TextFormOwnerKey key, Utf8View text) noexcept {
    if (!description_owner_valid(key)) return false;
    auto* state = g_app.attachments;
    const auto* record = message_attachment_drafts_get(&state->drafts, key.item);
    return message_attachment_drafts_describe(&state->drafts, key.item, text, record->spoiler);
}
bool save_filename(TextFormOwnerKey key, Utf8View text) noexcept {
    if (!description_owner_valid(key)) return false;
    // Path separators would only be stripped by the service; keep the name exact.
    for (u32 i = 0; i < text.bytes; ++i)
        if (text.data[i] == '/' || text.data[i] == '\\') return false;
    return message_attachment_drafts_rename(&g_app.attachments->drafts, key.item, text);
}
bool record_editable(const ApplicationAttachments* state, u64 id) noexcept {
    const auto* record = message_attachment_drafts_get(&state->drafts, id);
    return record && record->channel_id == g_app.account_ui->active_channel_id &&
           record->phase != MessageAttachmentDraftPhase::queued;
}
UiComposerAttachmentPhase tile_phase(MessageAttachmentDraftPhase phase) noexcept {
    switch (phase) {
    case MessageAttachmentDraftPhase::uploading: return UiComposerAttachmentPhase::uploading;
    case MessageAttachmentDraftPhase::ready: return UiComposerAttachmentPhase::ready;
    case MessageAttachmentDraftPhase::failed: return UiComposerAttachmentPhase::failed;
    default: return UiComposerAttachmentPhase::waiting;
    }
}
bool retryable(const MessageAttachmentDraft& record) noexcept {
    return record.phase == MessageAttachmentDraftPhase::failed &&
           record.failure != MessageAttachmentDraftFailure::source_changed &&
           record.failure != MessageAttachmentDraftFailure::size_limit;
}
// The picker is still preparing files for channel_id.
bool preparing(const ApplicationAttachments* state, u64 channel_id) noexcept {
    return state->picker.runtime && !state->picker_consumed && state->picker_channel == channel_id;
}
// Sentinel id for the picker's preparing tile; draft ids never use it.
inline constexpr u64 kPreparingTileId = ~0ull;
ApplicationAttachments* ensure() noexcept {
    if (g_app.attachments) return current(g_app.attachments) ? g_app.attachments : nullptr;
    auto* state = static_cast<ApplicationAttachments*>(VirtualAlloc(nullptr, sizeof(ApplicationAttachments), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!state) return nullptr;
    if (!message_attachment_drafts_initialize(&state->drafts)) { VirtualFree(state, 0, MEM_RELEASE); return nullptr; }
    state->profile_id = g_app.account.status.profile_id;
    g_app.attachments = state;
    return state;
}
void notice(ApplicationAttachments* state, Utf8View text) noexcept {
    state->notice_bytes = minimum(text.bytes, static_cast<u32>(sizeof(state->notice)));
    copy_bytes(state->notice, text.data, state->notice_bytes);
}
void limit_notice(ApplicationAttachments* state, u64 limit) noexcept {
    ProtocolWriter writer{{reinterpret_cast<u8*>(state->notice), sizeof(state->notice)}, 0};
    protocol_writer_append_literal(&writer, "Max file size is ");
    protocol_writer_append_u64(&writer, limit / kUploadMiB);
    protocol_writer_append_literal(&writer, " MB in this channel.");
    state->notice_bytes = writer.used;
}
void fail(ApplicationAttachments* state, u64 id, MessageAttachmentDraftFailure failure, u16 status = 0) noexcept {
    if (auto* record = message_attachment_drafts_get(&state->drafts, id)) {
        record->phase = MessageAttachmentDraftPhase::failed; record->failure = failure; record->http_status = status;
        ++state->drafts.revision;
    }
}
void append_name(ProtocolWriter* writer, Utf8View name, u32 maximum_bytes) noexcept {
    u32 bytes = 0;
    while (bytes < name.bytes) {
        const auto scalar = utf8_decode_one(name.data + bytes, name.bytes - bytes);
        if (!scalar.valid || bytes + scalar.width > maximum_bytes) break;
        bytes += scalar.width;
    }
    protocol_writer_append_bytes(writer, reinterpret_cast<const u8*>(name.data), bytes);
}
void close_menu() noexcept {
    if (g_app.popup_owner_kind == PopupOwnerKind::attachments) {
        popup_scene_close(&g_app.popup_scene); set_popup_owner(PopupOwnerKind::none);
    }
    if (g_app.attachments) g_app.attachments->detail_id = 0;
}
bool open_menu(ConstSlice<PopupItem> items) noexcept {
    const auto* anchor = message_composer_scene_layout_node(g_app.message_composer_scene, MessageComposerControl::attachments);
    if (!anchor || !ui_rect_has_area(anchor->rectangle)) anchor = message_composer_scene_layout_node(g_app.message_composer_scene, MessageComposerControl::add_attachment);
    const UiRect rectangle = anchor ? anchor->rectangle : ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::title_bar);
    if (!popup_scene_open(&g_app.popup_scene, PopupKind::popover, g_app.account_ui->active_channel_id ? g_app.account_ui->active_channel_id : 1,
        items, rectangle, application_viewport_dips(), 360.0f, true).ok()) return false;
    set_popup_owner(PopupOwnerKind::attachments);
    return refresh_account_frame(true);
}
bool show_file(ApplicationAttachments* state, u64 id) noexcept {
    const auto* record = message_attachment_drafts_get(&state->drafts, id);
    if (!record || record->channel_id != g_app.account_ui->active_channel_id || record->phase == MessageAttachmentDraftPhase::queued) {
        close_menu(); return refresh_message_composer_ui();
    }
    ProtocolWriter writer{{reinterpret_cast<u8*>(state->menu_labels[0]), sizeof(state->menu_labels[0])}, 0};
    append_name(&writer, message_attachment_drafts_filename(&state->drafts, id), 240);
    PopupItem items[kAttachmentFileMenuItems];
    const u32 count = attachment_file_menu({state->menu_labels[0], writer.used}, record->spoiler, {items, kAttachmentFileMenuItems});
    state->detail_id = id;
    return count && open_menu({items, count});
}
u64 next_url(const ApplicationAttachments* state, u64 now_ms) noexcept {
    if (!current(state) || state->url_id || state->active_id) return 0;
    for (u32 i = 0; i < state->drafts.count; ++i) {
        const u32 slot = state->drafts.order[i];
        const auto& record = state->drafts.records[slot];
        if (record.phase == MessageAttachmentDraftPhase::waiting_url && record.retry_at_ms <= now_ms)
            return (static_cast<u64>(record.generation) << 32) | (slot + 1);
    }
    return 0;
}
bool reap(ApplicationAttachments* state) noexcept {
    const bool picker_done = native_file_picker_release(&state->picker, 0);
    if (state->active_id) {
        if (application_upload_lane_release(&state->upload_lease)) state->active_id = 0;
    }
    if (!picker_done || state->active_id) return false;
    application_attachment_probe_release();
    message_attachment_drafts_release(&state->drafts);
    SecureZeroMemory(state, sizeof(*state)); VirtualFree(state, 0, MEM_RELEASE); g_app.attachments = nullptr;
    return true;
}
}
MessageComposerAttachmentInput application_attachment_input() noexcept {
    MessageComposerAttachmentInput input{}; input.can_attach = can_attach();
    auto* state = g_app.attachments;
    if (!current(state) || g_app.message_edit.editor_active) return input;
    u64 ids[kAttachmentUploadMaximumCount]{};
    const u32 count = minimum(kAttachmentUploadMaximumCount, message_attachment_drafts_channel(&state->drafts,
        g_app.account_ui->active_channel_id, {ids, kAttachmentUploadMaximumCount}));
    state->tile_count = 0;
    for (u32 i = 0; i < count; ++i) {
        const auto* record = message_attachment_drafts_get(&state->drafts, ids[i]);
        if (!record) continue;
        const Utf8View filename = message_attachment_drafts_filename(&state->drafts, ids[i]);
        const auto preview = application_composer_preview_view(ids[i]);
        auto& tile = state->tiles[state->tile_count++];
        reset_value(&tile);
        tile.id = ids[i];
        tile.filename = filename;
        tile.bytes = record->source.bytes;
        tile.uploaded_bytes = record->uploaded_bytes;
        tile.preview = preview.ready ? preview.key : MediaAssetKey{};
        tile.preview_ready = preview.ready && media_asset_key_valid(preview.key);
        tile.kind = ui_attachment_file_kind(filename);
        tile.phase = tile_phase(record->phase);
        tile.spoiler = record->spoiler;
        tile.described = record->description_bytes != 0;
        tile.animated = application_composer_preview_format(filename) == MediaAttachmentFormat::gif;
        tile.can_retry = retryable(*record);
        tile.duration_ms = application_attachment_probe_duration(ids[i]);
    }
    if (preparing(state, g_app.account_ui->active_channel_id) &&
        state->tile_count < kUiComposerAttachmentMaximum) {
        auto& tile = state->tiles[state->tile_count++];
        reset_value(&tile);
        tile.id = kPreparingTileId;
        tile.phase = UiComposerAttachmentPhase::preparing;
        tile.filename = state->picker_cancel_requested ? utf8_literal("Cancelling…") : utf8_literal("Preparing files…");
        input.label = state->picker_cancel_requested ? utf8_literal("Cancelling…") :
            utf8_literal("Preparing files… Click to cancel.");
        input.tiles = {state->tiles, state->tile_count};
        return input;
    }
    if (!count) return input;
    u32 ready = 0, failed = 0, changed_sources = 0; double bytes = 0, total = 0;
    for (u32 i = 0; i < count; ++i) {
        const auto* record = message_attachment_drafts_get(&state->drafts, ids[i]);
        ready += record->phase == MessageAttachmentDraftPhase::ready;
        failed += record->phase == MessageAttachmentDraftPhase::failed;
        changed_sources += record->phase == MessageAttachmentDraftPhase::failed && record->failure == MessageAttachmentDraftFailure::source_changed;
        bytes += static_cast<double>(record->uploaded_bytes); total += static_cast<double>(record->source.bytes);
    }
    ProtocolWriter writer{{reinterpret_cast<u8*>(state->label), sizeof(state->label)}, 0};
    if (count == 1) {
        if (message_attachment_drafts_get(&state->drafts, ids[0])->spoiler) protocol_writer_append_literal(&writer, "Spoiler: ");
        append_name(&writer, message_attachment_drafts_filename(&state->drafts, ids[0]), 96);
    }
    else { protocol_writer_append_u64(&writer, count); protocol_writer_append_literal(&writer, " attachments"); }
    if (changed_sources) protocol_writer_append_literal(&writer, " · Changed. Remove and add it again.");
    else if (failed) protocol_writer_append_literal(&writer, " · Upload failed. Click to retry.");
    else if (ready == count) protocol_writer_append_literal(&writer, " · Ready");
    else {
        protocol_writer_append_literal(&writer, " - uploading ");
        // Avoid overflow for large selected files; percentage is diagnostic UI.
        const u64 percent = total ? static_cast<u64>(static_cast<double>(bytes) * 100.0 / static_cast<double>(total)) : 0;
        protocol_writer_append_u64(&writer, minimum<u64>(percent, 100)); protocol_writer_append_literal(&writer, "%");
    }
    input.label = {state->label, writer.used}; input.ready = ready == count;
    input.tiles = {state->tiles, state->tile_count};
    return input;
}
void application_sync_attachment_drop_target() noexcept {
    if (!g_app.window) return;
    const auto* state = g_app.attachments;
    const bool enabled = can_attach() && (!state || (!state->picker.runtime &&
        message_attachment_drafts_channel(&state->drafts, g_app.account_ui->active_channel_id, {}) < kAttachmentUploadMaximumCount));
    const LONG_PTR style = GetWindowLongPtrW(g_app.window, GWL_EXSTYLE);
    const bool registered = (style & WS_EX_ACCEPTFILES) != 0;
    if (registered != enabled)
        SetWindowLongPtrW(g_app.window, GWL_EXSTYLE,
                         enabled ? style | WS_EX_ACCEPTFILES : style & ~LONG_PTR{WS_EX_ACCEPTFILES});
}
bool application_drop_attachments(HANDLE drop) noexcept {
    if (!drop) return true;
    auto discard = [drop]() noexcept { DragFinish(static_cast<HDROP>(drop)); };
    if (!can_attach()) { discard(); return true; }
    auto* state = ensure();
    if (!state) { discard(); return false; }
    const u32 count = message_attachment_drafts_channel(&state->drafts, g_app.account_ui->active_channel_id, {});
    if (state->picker.runtime || count >= kAttachmentUploadMaximumCount) { discard(); return true; }
    state->picker_channel = g_app.account_ui->active_channel_id; state->picker_consumed = false;
    state->picker_cancel_requested = false;
    const auto started = native_file_picker_take_drop(&state->picker, drop, kAttachmentUploadMaximumCount - count);
    if (started != NativeFilePickerError::none) {
        discard(); notice(state, utf8_literal("Couldn't add these files."));
        return application_manage_attachments();
    }
    if (g_app.popup_scene.active) { popup_scene_close(&g_app.popup_scene); set_popup_owner(PopupOwnerKind::none); }
    application_sync_attachment_drop_target();
    return refresh_message_composer_ui();
}
bool application_paste_attachments() noexcept {
    if (!can_attach()) return true;
    auto* state = ensure(); if (!state) return false;
    const u32 count = message_attachment_drafts_channel(&state->drafts, g_app.account_ui->active_channel_id, {});
    if (state->picker.runtime) return true;
    if (count >= kAttachmentUploadMaximumCount) {
        notice(state, utf8_literal("You can only upload 10 files at a time."));
        return application_manage_attachments();
    }
    ClipboardAttachmentData input{};
    if (!clipboard_attachment_capture(&input)) {
        notice(state, utf8_literal("Could not read the clipboard attachment. Try pasting again."));
        return application_manage_attachments();
    }
    state->picker_channel = g_app.account_ui->active_channel_id;
    state->picker_consumed = false; state->picker_cancel_requested = false;
    const auto started = native_file_picker_take_clipboard(&state->picker, &input, kAttachmentUploadMaximumCount - count,
        application_attachment_limit(state->picker_channel));
    clipboard_attachment_release(&input);
    if (started != NativeFilePickerError::none) {
        notice(state, utf8_literal("Could not prepare the clipboard attachment.")); return application_manage_attachments();
    }
    application_sync_attachment_drop_target(); return refresh_message_composer_ui();
}
bool application_pick_attachments() noexcept {
    if (!can_attach()) return true;
    auto* state = ensure(); if (!state) return false;
    if (state->picker.runtime) return true;
    const u32 count = message_attachment_drafts_channel(&state->drafts, g_app.account_ui->active_channel_id, {});
    if (count >= kAttachmentUploadMaximumCount) {
        notice(state, utf8_literal("You can only upload 10 files at a time."));
        return application_manage_attachments();
    }
    state->picker_channel = g_app.account_ui->active_channel_id; state->picker_consumed = false;
    state->picker_cancel_requested = false;
    const auto opened = native_file_picker_start(&state->picker, g_app.window, kAttachmentUploadMaximumCount - count, utf8_literal("Attach files"));
    if (opened != NativeFilePickerError::none) {
        notice(state, utf8_literal("Could not open the file picker.")); return application_manage_attachments();
    }
    if (g_app.popup_scene.active) { popup_scene_close(&g_app.popup_scene); set_popup_owner(PopupOwnerKind::none); }
    application_sync_attachment_drop_target();
    return refresh_message_composer_ui();
}
bool application_manage_attachments() noexcept {
    auto* state = g_app.attachments; if (!current(state)) return true;
    state->detail_id = 0;
    PopupItem items[kAttachmentUploadMaximumCount + 4]{}; u32 used = 0;
    if (state->notice_bytes) items[used++] = {{state->notice, state->notice_bytes}, popup_action_attachment_title, 0, 0};
    state->menu_count = message_attachment_drafts_channel(&state->drafts, g_app.account_ui->active_channel_id,
        {state->menu_ids, kAttachmentUploadMaximumCount});
    for (u32 i = 0; i < state->menu_count; ++i) {
        ProtocolWriter writer{{reinterpret_cast<u8*>(state->menu_labels[i]), sizeof(state->menu_labels[i])}, 0};
        if (message_attachment_drafts_get(&state->drafts, state->menu_ids[i])->spoiler)
            protocol_writer_append_literal(&writer, "Spoiler: ");
        append_name(&writer, message_attachment_drafts_filename(&state->drafts, state->menu_ids[i]), 220);
        items[used++] = {{state->menu_labels[i], writer.used}, popup_action_attachment_file_first + i, popup_item_enabled, 0};
    }
    items[used++] = {utf8_literal("Retry failed uploads"), popup_action_attachment_retry, popup_item_enabled, 0};
    if (preparing(state, g_app.account_ui->active_channel_id))
        items[used++] = {utf8_literal("Cancel"), popup_action_attachment_cancel_selection, popup_item_enabled, 0};
    items[used++] = {utf8_literal("Dismiss"), popup_action_attachment_dismiss, popup_item_enabled, 0};
    return open_menu({items, used});
}
bool application_attachment_add_menu() noexcept {
    if (!g_app.account_ui) return false;
    const u64 channel = g_app.account_ui->active_channel_id;
    if (!application_poll_create_available(channel)) return application_pick_attachments();
    PopupItem items[2]{};
    items[0] = {utf8_literal("Upload a file"), popup_action_attachment_upload,
        static_cast<u16>(can_attach() ? popup_item_enabled : 0), 0};
    items[0].presentation = ui_menu_row_presentation(UiIcon::upload);
    items[1] = {utf8_literal("Create poll"), popup_action_attachment_create_poll, popup_item_enabled, 0};
    items[1].presentation = ui_menu_row_presentation(UiIcon::poll);
    const auto* anchor = message_composer_scene_layout_node(g_app.message_composer_scene, MessageComposerControl::add_attachment);
    const UiRect rectangle = anchor ? anchor->rectangle : ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::title_bar);
    if (!popup_scene_open(&g_app.popup_scene, PopupKind::context_menu, channel ? channel : 1, {items, 2}, rectangle,
        application_viewport_dips(), 220.0f, true).ok()) return false;
    set_popup_owner(PopupOwnerKind::attachments);
    return refresh_account_frame(true);
}
bool application_attachment_action(u32 action) noexcept {
    if (action == popup_action_attachment_upload || action == popup_action_attachment_create_poll) {
        close_menu();
        if (action == popup_action_attachment_upload) return application_pick_attachments();
        return !g_app.account_ui || application_poll_create_open(g_app.account_ui->active_channel_id) ||
               refresh_message_composer_ui();
    }
    auto* state = g_app.attachments; if (!current(state)) return false;
    if (action >= popup_action_attachment_file_first && action < popup_action_attachment_file_first + state->menu_count)
        return show_file(state, state->menu_ids[action - popup_action_attachment_file_first]);
    if (action == popup_action_attachment_back) return application_manage_attachments();
    if (action == popup_action_attachment_rename) {
        const TextFormOwnerKey key{state->profile_id, g_app.account_ui->active_channel_id, state->detail_id};
        if (!description_owner_valid(key)) { close_menu(); return refresh_message_composer_ui(); }
        const Utf8View current_name = message_attachment_drafts_filename(&state->drafts, key.item);
        ApplicationTextFormRequest request{utf8_literal("Rename file"), current_name, utf8_literal("Filename"),
            utf8_literal("Save"), current_name, key, description_owner_valid, save_filename, 255};
        request.maximum_bytes = kAttachmentUploadMaximumFilenameBytes;
        request.single_line = true;
        close_menu();
        return application_text_form_open(request);
    }
    if (action == popup_action_attachment_description) {
        const TextFormOwnerKey key{state->profile_id, g_app.account_ui->active_channel_id, state->detail_id};
        if (!description_owner_valid(key)) { close_menu(); return refresh_message_composer_ui(); }
        const ApplicationTextFormRequest request{utf8_literal("Description (alt text)"),
            message_attachment_drafts_filename(&state->drafts, key.item), utf8_literal("Add a description (up to 1024 characters)"),
            utf8_literal("Save description"), message_attachment_drafts_description(&state->drafts, key.item),
            key, description_owner_valid, save_description, 1024};
        close_menu();
        return application_text_form_open(request);
    }
    if (action == popup_action_attachment_spoiler || action == popup_action_attachment_remove) {
        const u64 id = state->detail_id;
        const auto* record = message_attachment_drafts_get(&state->drafts, id);
        if (!record || record->channel_id != g_app.account_ui->active_channel_id || record->phase == MessageAttachmentDraftPhase::queued) {
            close_menu(); return refresh_message_composer_ui();
        }
        if (action == popup_action_attachment_spoiler) {
            if (!message_attachment_drafts_set_spoiler(&state->drafts, id, !record->spoiler)) return false;
            return show_file(state, id) && refresh_message_composer_ui();
        }
        if (id == state->active_id) application_upload_lane_cancel(state->upload_lease);
        application_composer_preview_retire(id);
        if (!message_attachment_drafts_remove(&state->drafts, id)) return false;
    } else if (action == popup_action_attachment_retry) {
        for (u32 i = 0; i < state->menu_count; ++i) {
            auto* record = message_attachment_drafts_get(&state->drafts, state->menu_ids[i]);
            if (record && record->channel_id == g_app.account_ui->active_channel_id &&
                record->phase == MessageAttachmentDraftPhase::failed && record->failure != MessageAttachmentDraftFailure::source_changed) {
                record->phase = MessageAttachmentDraftPhase::waiting_url; record->retry_at_ms = 0; record->failure = MessageAttachmentDraftFailure::none;
                record->uploaded_bytes = 0;
            }
        }
    } else if (action == popup_action_attachment_cancel_selection) {
        if (preparing(state, g_app.account_ui->active_channel_id)) {
            state->picker_cancel_requested = true; native_file_picker_cancel(&state->picker);
        }
    } else if (action != popup_action_attachment_dismiss) return false;
    close_menu();
    state->notice_bytes = 0;
    return refresh_message_composer_ui() && pump_message_history();
}
bool application_attachment_url_pending(u64 now_ms) noexcept {
    return application_upload_lane_available() && next_url(g_app.attachments, now_ms) != 0;
}
bool application_attachment_submit_url(u64 request_id, u64 now_ms, bool* submitted) noexcept {
    *submitted = false; auto* state = g_app.attachments;
    const u64 id = next_url(state, now_ms); if (!id || !application_upload_lane_available()) return true;
    auto* record = message_attachment_drafts_get(&state->drafts, id);
    const u64 limit = application_attachment_limit(record->channel_id);
    if (record->source.bytes > limit) {
        fail(state, id, MessageAttachmentDraftFailure::size_limit); limit_notice(state, limit);
        return refresh_message_composer_ui();
    }
    if (!application_upload_lane_acquire(ApplicationUploadOwner::composer, &state->upload_lease)) return true;
    const auto result = g_app.account.api.attachment_upload(g_app.account.instance, record->channel_id, static_cast<u32>(id),
        message_attachment_drafts_filename(&state->drafts, id), record->source.bytes, request_id);
    if (result.error == RestClientSubmitError::busy) { application_upload_lane_release(&state->upload_lease); record->retry_at_ms = now_ms + 100; return true; }
    if (!result.ok()) { application_upload_lane_release(&state->upload_lease); fail(state, id, MessageAttachmentDraftFailure::request); return refresh_message_composer_ui(); }
    state->url_id = id; state->url_request_id = request_id; *submitted = true; return true;
}
void application_attachment_fail_url() noexcept {
    if (auto* state = g_app.attachments) {
        fail(state, state->url_id, MessageAttachmentDraftFailure::request); state->url_id = state->url_request_id = 0;
        application_upload_lane_release(&state->upload_lease);
    }
}
bool application_attachment_accept_url(const RestClientResponse& response, Utf8View body) noexcept {
    auto* state = g_app.attachments;
    if (!current(state) || !state->url_id || response.request_id != state->url_request_id) return true;
    const u64 id = state->url_id; state->url_id = state->url_request_id = 0;
    auto* record = message_attachment_drafts_get(&state->drafts, id);
    if (!record) { application_upload_lane_release(&state->upload_lease); return true; }
    if (response.transport_ok() && response.status_code == 429 && response.retry_at_ms) {
        record->retry_at_ms = response.retry_at_ms; application_upload_lane_release(&state->upload_lease); return true;
    }
    const auto decoded = response.transport_ok() && response.status_code >= 200 && response.status_code < 300 ?
        attachment_upload_parse_response(body, static_cast<u32>(id), {state->url, sizeof(state->url)},
            {state->uploaded_filename, sizeof(state->uploaded_filename)}) : AttachmentUploadResponse{};
    if (!decoded.ok) {
        SecureZeroMemory(state->url, sizeof(state->url));
        SecureZeroMemory(state->uploaded_filename, sizeof(state->uploaded_filename));
        fail(state, id, MessageAttachmentDraftFailure::request, response.status_code);
        application_upload_lane_release(&state->upload_lease);
        return refresh_message_composer_ui();
    }
    const u64 limit = application_attachment_limit(record->channel_id);
    if (record->source.bytes > limit) {
        SecureZeroMemory(state->url, sizeof(state->url));
        SecureZeroMemory(state->uploaded_filename, sizeof(state->uploaded_filename));
        fail(state, id, MessageAttachmentDraftFailure::size_limit); limit_notice(state, limit);
        application_upload_lane_release(&state->upload_lease);
        return refresh_message_composer_ui();
    }
    const AccountUploadRequest request{{state->url, decoded.url_bytes}, message_attachment_drafts_path(&state->drafts, id), record->source, record->content,
        record->voice ? HttpsUploadContentType::audio_ogg : HttpsUploadContentType::octet_stream};
    const auto started = application_upload_lane_operation(state->upload_lease, AccountUploadOperation::start, &request);
    SecureZeroMemory(state->url, sizeof(state->url));
    if (started.submit_error != HttpsFileUploadError::none) {
        fail(state, id, MessageAttachmentDraftFailure::upload); SecureZeroMemory(state->uploaded_filename, sizeof(state->uploaded_filename));
        if (!application_upload_lane_release(&state->upload_lease)) { state->active_id = id; state->finishing = true; }
    } else {
        state->active_id = id; state->finishing = false; state->uploaded_filename_bytes = decoded.uploaded_filename_bytes;
        record->phase = MessageAttachmentDraftPhase::uploading;
    }
    return refresh_message_composer_ui();
}
bool application_pump_attachments() noexcept {
    auto* state = g_app.attachments; if (!state) return true;
    if (!current(state)) {
        application_cancel_attachments(); const bool close = state->close_window;
        if (reap(state) && close && g_app.window) PostMessageW(g_app.window, WM_CLOSE, 0, 0);
        return true;
    }
    bool changed = false, show_notice = false;
    const auto picked = native_file_picker_status(&state->picker);
    if (picked.phase == NativeFilePickerPhase::complete && !state->picker_consumed) {
        state->picker_consumed = true;
        if (picked.ok() && !state->picker_cancel_requested) {
            MessageAttachmentFile files[kAttachmentUploadMaximumCount]{}; u64 ids[kAttachmentUploadMaximumCount]{};
            bool valid = picked.count <= kAttachmentUploadMaximumCount;
            bool too_large = false;
            const u64 limit = application_attachment_limit(state->picker_channel);
            for (u32 i = 0; valid && i < picked.count; ++i) {
                files[i].path = native_file_picker_path(&state->picker, i);
                files[i].filename = native_file_picker_filename(&state->picker, i);
                files[i].content = native_file_picker_source(&state->picker, i);
                valid = native_file_picker_snapshot(&state->picker, i, &files[i].snapshot);
                too_large = too_large || files[i].snapshot.bytes > limit;
            }
            const auto added = valid && !too_large ? message_attachment_drafts_add(&state->drafts, state->picker_channel,
                {files, picked.count}, {ids, kAttachmentUploadMaximumCount}) : MessageAttachmentDraftError::invalid_input;
            if (added != MessageAttachmentDraftError::none) {
                if (too_large) limit_notice(state, limit);
                else
                notice(state, added == MessageAttachmentDraftError::empty_file ? utf8_literal("Files cannot be empty.") :
                    utf8_literal("Couldn't attach these files.")); show_notice = true;
            }
            changed = true;
        } else if (!state->picker_cancel_requested && picked.error != NativeFilePickerError::cancelled) {
            if (picked.native_error == ERROR_FILE_TOO_LARGE) limit_notice(state, application_attachment_limit(state->picker_channel));
            else notice(state, picked.error == NativeFilePickerError::capacity ?
                utf8_literal("You can only upload 10 files at a time.") :
                utf8_literal("Could not read the selected files.")); show_notice = true;
        }
    }
    if (state->picker_consumed && state->picker.runtime && native_file_picker_release(&state->picker, 0)) changed = true;
    if (state->active_id) {
        const auto status = application_upload_lane_operation(state->upload_lease, AccountUploadOperation::status);
        auto* record = message_attachment_drafts_get(&state->drafts, state->active_id);
        if (record && !state->finishing && record->uploaded_bytes != status.transfer.committed_bytes) {
            record->uploaded_bytes = status.transfer.committed_bytes; changed = true;
        }
        if (!state->finishing && (status.transfer.phase == HttpsFileUploadPhase::complete || status.submit_error != HttpsFileUploadError::none)) {
            if (record) {
                if (status.submit_error == HttpsFileUploadError::none && status.transfer.ok()) {
                    if (!message_attachment_drafts_uploaded(&state->drafts, state->active_id, {state->uploaded_filename, state->uploaded_filename_bytes}))
                        fail(state, state->active_id, MessageAttachmentDraftFailure::memory);
                } else {
                    fail(state, state->active_id, status.transfer.error == HttpsFileUploadError::source_changed || status.transfer.error == HttpsFileUploadError::file_size ?
                        MessageAttachmentDraftFailure::source_changed : MessageAttachmentDraftFailure::upload, status.transfer.status_code);
                    if (record->failure == MessageAttachmentDraftFailure::source_changed)
                        notice(state, utf8_literal("A selected file changed. Remove it and select it again."));
                }
            }
            state->finishing = true; changed = true;
        }
        if (state->finishing) {
            if (application_upload_lane_release(&state->upload_lease)) {
                state->active_id = 0; state->finishing = false; state->uploaded_filename_bytes = 0;
                SecureZeroMemory(state->uploaded_filename, sizeof(state->uploaded_filename));
            }
        }
    }
    if (application_attachment_probe_synchronize()) changed = true;
    if (state->preview_revision != application_composer_preview_revision()) {
        state->preview_revision = application_composer_preview_revision(); changed = true;
    }
    if (changed && !refresh_message_composer_ui()) return false;
    if (show_notice && !application_manage_attachments()) return false;
    return !application_attachment_url_pending(clock_monotonic_milliseconds()) || pump_message_history();
}
HANDLE application_attachment_notification() noexcept {
    return g_app.attachments && !g_app.attachments->picker_consumed ? g_app.attachments->picker.notification : nullptr;
}
u32 application_attachment_wait_timeout(u64 now_ms) noexcept {
    const auto* state = g_app.attachments; if (!state) return INFINITE;
    u32 timeout = state->active_id ? 100 : INFINITE;
    if (state->closing || (state->picker_consumed && state->picker.runtime)) timeout = 10;
    if (current(state) && application_upload_lane_available() && !state->url_id && !state->active_id &&
        g_app.message_history_request_owner == MessageHistoryRequestOwner::none) {
        for (const auto& record : state->drafts.records) {
            if (record.occupied && record.phase == MessageAttachmentDraftPhase::waiting_url)
                timeout = minimum(timeout, record.retry_at_ms > now_ms ? static_cast<u32>(minimum<u64>(record.retry_at_ms - now_ms, INFINITE - 1)) : 0u);
        }
    }
    return timeout;
}
bool application_clear_channel_attachments() noexcept {
    auto* state = g_app.attachments;
    if (!current(state) || !g_app.account_ui) return false;
    u64 ids[kAttachmentUploadMaximumCount];
    const u32 count = message_attachment_drafts_channel(&state->drafts, g_app.account_ui->active_channel_id,
        {ids, kAttachmentUploadMaximumCount});
    bool removed = false;
    for (u32 i = 0; i < count; ++i) {
        const auto* record = message_attachment_drafts_get(&state->drafts, ids[i]);
        // Queued drafts already belong to a send.
        if (!record || record->phase == MessageAttachmentDraftPhase::queued) continue;
        if (ids[i] == state->active_id) application_upload_lane_cancel(state->upload_lease);
        application_composer_preview_retire(ids[i]);
        removed = message_attachment_drafts_remove(&state->drafts, ids[i]) || removed;
    }
    if (removed) state->notice_bytes = 0;
    return removed;
}
void application_cancel_attachments() noexcept {
    application_text_form_cancel_owner(description_owner_valid);
    close_menu();
    if (auto* state = g_app.attachments) {
        state->closing = true; state->url_id = state->url_request_id = 0;
        native_file_picker_cancel(&state->picker);
        if (state->active_id) application_upload_lane_cancel(state->upload_lease);
        else application_upload_lane_release(&state->upload_lease);
    }
    if (g_app.window) {
        const LONG_PTR style = GetWindowLongPtrW(g_app.window, GWL_EXSTYLE);
        SetWindowLongPtrW(g_app.window, GWL_EXSTYLE, style & ~LONG_PTR{WS_EX_ACCEPTFILES});
    }
}
bool application_release_attachments() noexcept {
    application_cancel_attachments(); return !g_app.attachments || reap(g_app.attachments);
}
bool application_attachments_defer_close() noexcept {
    if (!g_app.attachments) return false;
    g_app.attachments->close_window = true;
    return !application_release_attachments();
}
bool application_attachment_prepare_send(u64 channel_id, Slice<UploadedAttachment> references, Slice<u64> ids, u32* count, Slice<u64> sizes) noexcept {
    if (!count) return false; *count = 0;
    auto* state = g_app.attachments; if (!current(state)) return true;
    if (preparing(state, channel_id)) return false;
    const u32 found = message_attachment_drafts_channel(&state->drafts, channel_id, ids);
    if (found > references.count || found > ids.count || (sizes.count && (!sizes.data || found > sizes.count))) return false;
    for (u32 i = 0; i < found; ++i) {
        if (!message_attachment_drafts_reference(&state->drafts, ids[i], i, &references[i])) return false;
        if (sizes.count) sizes[i] = message_attachment_drafts_get(&state->drafts, ids[i])->source.bytes;
    }
    *count = found; return true;
}
void application_attachment_did_queue(ConstSlice<u64> ids, u64 nonce) noexcept {
    if (g_app.attachments && ids.count && !message_attachment_drafts_mark_queued(&g_app.attachments->drafts, ids, nonce))
        log_write(LogLevel::error, "attachment", "queue_ownership_mismatch");
    for (u32 i = 0; g_app.attachments && i < ids.count; ++i) application_composer_preview_retire(ids[i]);
    if (ids.count) close_menu();
}
bool application_attachment_channel_pending(u64 channel_id) noexcept {
    const auto* state = g_app.attachments;
    return current(state) && channel_id &&
           (preparing(state, channel_id) ||
            message_attachment_drafts_channel(&state->drafts, channel_id, {}) != 0);
}
bool application_attachment_add_voice(u64 channel_id, UploadSource content, Utf8View waveform, u32 duration_ms,
                                      u64* id) noexcept {
    auto* state = ensure();
    if (!state || !id)
        return false;
    const ConstSlice<wchar_t> filename{kVoiceMessageFilenameWide, sizeof(kVoiceMessageFilenameWide) / sizeof(wchar_t) - 1};
    return message_attachment_drafts_add_voice(&state->drafts, channel_id, filename, content, waveform, duration_ms,
                                               id) == MessageAttachmentDraftError::none;
}
bool application_attachment_voice_progress(u64 id, MessageAttachmentDraftPhase* phase, u64* uploaded_bytes,
                                           u64* total_bytes) noexcept {
    const auto* state = g_app.attachments;
    const auto* record = current(state) ? message_attachment_drafts_get(&state->drafts, id) : nullptr;
    if (!record || !record->voice)
        return false;
    *phase = record->phase;
    *uploaded_bytes = record->uploaded_bytes;
    *total_bytes = record->source.bytes;
    return true;
}
bool application_attachment_voice_reference(u64 id, UploadedAttachment* reference, u64* bytes) noexcept {
    const auto* state = g_app.attachments;
    const auto* record = current(state) ? message_attachment_drafts_get(&state->drafts, id) : nullptr;
    if (!record || !record->voice || !message_attachment_drafts_reference(&state->drafts, id, 0, reference))
        return false;
    *bytes = record->source.bytes;
    return true;
}
bool application_attachment_voice_retry(u64 id) noexcept {
    auto* state = g_app.attachments;
    auto* record = current(state) ? message_attachment_drafts_get(&state->drafts, id) : nullptr;
    if (!record || !record->voice || !retryable(*record))
        return false;
    record->phase = MessageAttachmentDraftPhase::waiting_url;
    record->retry_at_ms = 0;
    record->failure = MessageAttachmentDraftFailure::none;
    record->uploaded_bytes = 0;
    ++state->drafts.revision;
    return true;
}
void application_attachment_voice_remove(u64 id) noexcept {
    auto* state = g_app.attachments;
    const auto* record = state ? message_attachment_drafts_get(&state->drafts, id) : nullptr;
    if (!record || !record->voice || record->phase == MessageAttachmentDraftPhase::queued)
        return;
    if (id == state->active_id)
        application_upload_lane_cancel(state->upload_lease);
    message_attachment_drafts_remove(&state->drafts, id);
}
void application_attachment_complete(u64 nonce) noexcept {
    if (g_app.attachments) message_attachment_drafts_remove_nonce(&g_app.attachments->drafts, nonce);
}
namespace {
ApplicationAttachments* strip_state() noexcept {
    auto* state = g_app.attachments;
    return current(state) && !g_app.message_edit.editor_active ? state : nullptr;
}
u64 tile_id(u32 index) noexcept {
    const auto tiles = message_composer_scene_attachment_tiles(g_app.message_composer_scene);
    return index < tiles.count ? tiles[index].id : 0;
}
UiRect tile_button(u32 index, UiComposerAttachmentPart part) noexcept {
    const auto view = message_composer_scene_attachment_view(g_app.message_composer_scene, false);
    if (index >= view.tiles.count) return {};
    const auto geometry = ui_composer_attachment_tile_geometry(view.strip, view.tiles[index], index, view.scroll);
    for (u32 i = 0; i < geometry.button_count; ++i)
        if (geometry.parts[i] == part) return geometry.buttons[i];
    return geometry.card;
}
bool cancel_preparing(ApplicationAttachments* state) noexcept {
    if (preparing(state, g_app.account_ui->active_channel_id)) {
        state->picker_cancel_requested = true; native_file_picker_cancel(&state->picker);
    }
    return refresh_message_composer_ui();
}
bool focus_strip(u32 index) noexcept {
    if (!focus_message_composer_control(MessageComposerControl::attachments)) return false;
    message_composer_scene_attachment_select(&g_app.message_composer_scene, index);
    return refresh_message_composer_ui();
}
} // namespace
bool application_attachment_remove(u64 id) noexcept {
    auto* state = strip_state();
    if (!state || !record_editable(state, id)) return false;
    if (state->detail_id == id) close_menu();
    if (id == state->active_id) application_upload_lane_cancel(state->upload_lease);
    application_composer_preview_retire(id);
    if (!message_attachment_drafts_remove(&state->drafts, id)) return false;
    state->notice_bytes = 0;
    application_sync_attachment_drop_target();
    return refresh_message_composer_ui() && pump_message_history();
}
bool application_attachment_toggle_spoiler(u64 id) noexcept {
    auto* state = strip_state();
    if (!state || !record_editable(state, id)) return false;
    const auto* record = message_attachment_drafts_get(&state->drafts, id);
    return message_attachment_drafts_set_spoiler(&state->drafts, id, !record->spoiler) && refresh_message_composer_ui();
}
bool application_attachment_retry(u64 id) noexcept {
    auto* state = strip_state();
    if (!state || !record_editable(state, id)) return false;
    auto* record = message_attachment_drafts_get(&state->drafts, id);
    if (!retryable(*record)) return false;
    record->phase = MessageAttachmentDraftPhase::waiting_url; record->retry_at_ms = 0;
    record->failure = MessageAttachmentDraftFailure::none; record->uploaded_bytes = 0; ++state->drafts.revision;
    return refresh_message_composer_ui() && pump_message_history();
}
bool application_attachment_modify(u64 id, UiRect anchor) noexcept {
    auto* state = strip_state();
    if (!state || !record_editable(state, id)) return false;
    ProtocolWriter writer{{reinterpret_cast<u8*>(state->menu_labels[0]), sizeof(state->menu_labels[0])}, 0};
    append_name(&writer, message_attachment_drafts_filename(&state->drafts, id), 240);
    PopupItem items[kAttachmentModifyMenuItems];
    const u32 count = attachment_modify_menu({state->menu_labels[0], writer.used},
        message_attachment_drafts_get(&state->drafts, id)->description_bytes != 0, {items, kAttachmentModifyMenuItems});
    if (!count) return false;
    if (!ui_rect_has_area(anchor)) {
        const auto* node = message_composer_scene_layout_node(g_app.message_composer_scene, MessageComposerControl::attachments);
        anchor = node ? node->rectangle : ui_scene_content_rectangle(g_app.shell_scene, UiSemantic::title_bar);
    }
    if (!popup_scene_open(&g_app.popup_scene, PopupKind::popover, g_app.account_ui->active_channel_id ? g_app.account_ui->active_channel_id : 1,
        {items, count}, anchor, application_viewport_dips(), 280.0f, true).ok()) return false;
    set_popup_owner(PopupOwnerKind::attachments);
    state->detail_id = id;
    return refresh_account_frame(true);
}
bool application_attachment_strip_activate(UiComposerAttachmentHit hit) noexcept {
    auto* state = strip_state();
    const u64 id = tile_id(hit.index);
    if (!state || !id) return true;
    if (id == kPreparingTileId) return cancel_preparing(state);
    switch (hit.part) {
    case UiComposerAttachmentPart::retry: return application_attachment_retry(id) || refresh_message_composer_ui();
    case UiComposerAttachmentPart::spoiler: return application_attachment_toggle_spoiler(id) || refresh_message_composer_ui();
    case UiComposerAttachmentPart::edit: return application_attachment_modify(id, tile_button(hit.index, hit.part)) || refresh_message_composer_ui();
    case UiComposerAttachmentPart::remove: return application_attachment_remove(id) || refresh_message_composer_ui();
    case UiComposerAttachmentPart::tile: return focus_strip(hit.index);
    default: return true;
    }
}
bool application_attachment_strip_key(u32 key) noexcept {
    auto* state = strip_state();
    auto& scene = g_app.message_composer_scene;
    const auto tiles = message_composer_scene_attachment_tiles(scene);
    if (!state || !tiles.count) return false;
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0 && (GetKeyState(VK_MENU) & 0x8000) == 0;
    u32 index = message_composer_scene_attachment_selected(scene);
    const bool selected = index < tiles.count;
    if (!selected) index = 0;
    const u64 id = tiles[index].id;
    const auto select = [&](u32 next) noexcept {
        message_composer_scene_attachment_select(&scene, next);
        return refresh_message_composer_ui();
    };
    switch (key) {
    case VK_LEFT: return select(selected && index ? index - 1 : 0);
    case VK_RIGHT: return select(selected ? minimum(index + 1, tiles.count - 1) : 0);
    case VK_HOME: return select(0);
    case VK_END: return select(tiles.count - 1);
    case VK_UP:
    case VK_ESCAPE:
        message_composer_scene_attachment_select(&scene, kUiComposerAttachmentNoTile);
        return focus_message_composer_control(MessageComposerControl::edit) && refresh_message_composer_ui();
    case VK_BACK:
        if (control) {
            if (application_clear_channel_attachments()) {
                application_sync_attachment_drop_target();
                return focus_message_composer_control(MessageComposerControl::edit) &&
                       refresh_message_composer_ui() && pump_message_history();
            }
            return true;
        }
        [[fallthrough]];
    case VK_DELETE:
    case 'D': {
        if (id == kPreparingTileId) return cancel_preparing(state);
        if (!application_attachment_remove(id)) return true;
        const auto remaining = message_composer_scene_attachment_tiles(scene);
        if (!remaining.count)
            return focus_message_composer_control(MessageComposerControl::edit) && refresh_message_composer_ui();
        return select(minimum(index, remaining.count - 1));
    }
    case VK_RETURN:
    case VK_SPACE:
    case VK_F2:
    case 'E':
        if (id == kPreparingTileId) return cancel_preparing(state);
        return application_attachment_modify(id, tile_button(index, UiComposerAttachmentPart::edit)) || true;
    case 'S':
        return id == kPreparingTileId || application_attachment_toggle_spoiler(id) || true;
    case 'R':
        return id == kPreparingTileId || application_attachment_retry(id) || true;
    default:
        return false;
    }
}
bool application_attachment_strip_character(u32 value) noexcept {
    if (!strip_state() || !message_composer_scene_attachment_tiles(g_app.message_composer_scene).count) return false;
    switch (value | 0x20u) {
    case ' ': case 'd': case 'e': case 's': case 'r': return true;
    default: return value == '\b' || value == '\r' || value == 0x1b;
    }
}
}
