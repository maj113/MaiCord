#pragma once
#include "maicord/protocol/attachment_upload.hpp"
#include "maicord/app/message_attachment_drafts.hpp"
#include "maicord/core/win32_types.hpp"

namespace maicord {
struct UiRect;

struct MessageComposerAttachmentInput;
struct UiComposerAttachmentHit;

struct RestClientResponse;
} // namespace maicord
namespace maicord::app_detail {
bool application_pick_attachments() noexcept;
// Upload a file or Create poll; the file picker directly when polls are unavailable.
bool application_attachment_add_menu() noexcept;
// Consumes the WM_DROPFILES handle on every path, including rejection.
bool application_drop_attachments(HANDLE drop) noexcept;
bool application_paste_attachments() noexcept;
u64 application_attachment_limit(u64 channel_id) noexcept;
void application_sync_attachment_drop_target() noexcept;
bool application_manage_attachments() noexcept;
bool application_attachment_action(u32 action) noexcept;
MessageComposerAttachmentInput application_attachment_input() noexcept;
// Upload strip: pointer activation of a tile part, keyboard while the strip
// has focus (Left/Right/Home/End select; D, Delete or Backspace remove;
// Ctrl+Backspace clears all; E, Enter or Space modify; S spoiler; R retry;
// Up or Escape return to the editor) and shortcut characters it swallows.
bool application_attachment_strip_activate(UiComposerAttachmentHit hit) noexcept;
bool application_attachment_strip_key(u32 key) noexcept;
bool application_attachment_strip_character(u32 value) noexcept;
// Per-draft actions shared by tiles, keys and menus.
bool application_attachment_remove(u64 id) noexcept;
bool application_attachment_toggle_spoiler(u64 id) noexcept;
bool application_attachment_retry(u64 id) noexcept;
bool application_attachment_modify(u64 id, UiRect anchor) noexcept;
bool application_pump_attachments() noexcept;
HANDLE application_attachment_notification() noexcept;
u32 application_attachment_wait_timeout(u64 now_ms) noexcept;
void application_cancel_attachments() noexcept;
// Removes the active channel's unsent drafts; true when anything was removed.
bool application_clear_channel_attachments() noexcept;
bool application_release_attachments() noexcept;
bool application_attachments_defer_close() noexcept;
bool application_attachment_url_pending(u64 now_ms) noexcept;
bool application_attachment_submit_url(u64 request_id, u64 now_ms, bool* submitted) noexcept;
bool application_attachment_accept_url(const RestClientResponse& response, Utf8View body) noexcept;
void application_attachment_fail_url() noexcept;
bool application_attachment_prepare_send(u64 channel_id, Slice<UploadedAttachment> references, Slice<u64> ids, u32* count,
    Slice<u64> sizes = {}) noexcept;
void application_attachment_did_queue(ConstSlice<u64> ids, u64 nonce) noexcept;
// Recorded voice messages share the URL/upload pipeline as unlisted drafts.
// Files attached to, or being prepared for, the channel's draft.
bool application_attachment_channel_pending(u64 channel_id) noexcept;
bool application_attachment_add_voice(u64 channel_id, UploadSource content, Utf8View waveform, u32 duration_ms,
                                      u64* id) noexcept;
// False once the draft is gone (removed, or completed after its send).
bool application_attachment_voice_progress(u64 id, MessageAttachmentDraftPhase* phase, u64* uploaded_bytes,
                                           u64* total_bytes) noexcept;
bool application_attachment_voice_reference(u64 id, UploadedAttachment* reference, u64* bytes) noexcept;
bool application_attachment_voice_retry(u64 id) noexcept;
void application_attachment_voice_remove(u64 id) noexcept;
void application_attachment_complete(u64 nonce) noexcept;
}
