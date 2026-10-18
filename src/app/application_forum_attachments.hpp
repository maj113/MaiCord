#pragma once
#include "application_forum_post.hpp"
#include "maicord/app/message_attachment_drafts.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/protocol/forum_post.hpp"
#include "maicord/core/win32_types.hpp"
namespace maicord::app_detail {
// Whether the file name ends in `suffix` (lowercase), ignoring ASCII case.
inline bool filename_ends_with(Utf8View name, Utf8View suffix) noexcept {
    return name.bytes >= suffix.bytes &&
           ascii_equal_ignore_case({name.data + name.bytes - suffix.bytes, suffix.bytes}, suffix);
}
struct ApplicationForumAttachmentRow {
    u64 id;
    Utf8View filename, description, status;
    u64 bytes, uploaded_bytes;
    MessageAttachmentDraftPhase phase;
    bool spoiler, can_remove, can_retry;
};
struct ApplicationForumAttachmentView {
    ConstSlice<ApplicationForumAttachmentRow> rows;
    Utf8View status;
    u32 revision;
    bool can_add, busy, ready, error;
};
bool application_forum_attachments_begin(const ApplicationForumPostTicket&) noexcept;
ApplicationForumAttachmentView application_forum_attachments_view(const ApplicationForumPostTicket&) noexcept;
bool application_forum_attachments_current(const ApplicationForumPostTicket&, u64 id = 0) noexcept;
bool application_forum_attachments_pick(const ApplicationForumPostTicket&) noexcept;
// Always consumes a drop, including stale-request/rejected paths.
bool application_forum_attachments_drop(const ApplicationForumPostTicket&, HANDLE) noexcept;
bool application_forum_attachments_paste(const ApplicationForumPostTicket&) noexcept;
bool application_forum_attachments_remove(const ApplicationForumPostTicket&, u64) noexcept;
bool application_forum_attachments_retry(const ApplicationForumPostTicket&, u64) noexcept;
bool application_forum_attachments_spoiler(const ApplicationForumPostTicket&, u64, bool) noexcept;
bool application_forum_attachments_describe(const ApplicationForumPostTicket&, u64, Utf8View) noexcept;
// Borrowed source/path; callers retaining a source must balance retain/release.
bool application_forum_attachments_source(const ApplicationForumPostTicket&, u64,
    UploadSource*, FileSnapshot*, ConstSlice<wchar_t>*) noexcept;
bool application_forum_attachments_pump() noexcept;
bool application_forum_attachments_synchronize() noexcept;
HANDLE application_forum_attachments_notification() noexcept;
u64 application_forum_attachments_next_wake() noexcept;
u32 application_forum_attachments_wait_timeout(u64 now_ms) noexcept;
void application_forum_attachments_retire(const ApplicationForumPostTicket&) noexcept;
bool application_forum_attachments_release() noexcept;
bool application_forum_attachments_defer_close() noexcept;
// The forum host owns the DETAILS response and its cancellation.
bool application_forum_attachments_url_candidate(const ApplicationForumPostTicket&, u64 now_ms,
    ForumPostRequest*, u64* draft_id) noexcept;
bool application_forum_attachments_url_admitted(const ApplicationForumPostTicket&, u64 draft_id) noexcept;
void application_forum_attachments_url_unadmitted(const ApplicationForumPostTicket&, u64 draft_id) noexcept;
void application_forum_attachments_url_response(const ApplicationForumPostTicket&, u64 draft_id,
    const RestClientResponse&, Utf8View) noexcept;
bool application_forum_attachments_prepare(const ApplicationForumPostTicket&,
    Slice<UploadedAttachment>, Slice<u64> sizes, u32*) noexcept;
void application_forum_attachments_lock(const ApplicationForumPostTicket&, bool) noexcept;
void application_forum_attachments_refresh(const ApplicationForumPostTicket&) noexcept;
bool application_forum_attachments_decode_media(Utf8View, const ForumPostRequest&, u64 self,
    Slice<u32>, ForumPostSnapshot*) noexcept;
ConstSlice<GatewayMessageAttachment> application_forum_attachments_receipt() noexcept;
struct ApplicationForumAttachmentMetrics { u32 owner_bytes, committed_bytes, draft_bytes, receipt_bytes; bool upload_active; };
ApplicationForumAttachmentMetrics application_forum_attachments_metrics() noexcept;
} // namespace maicord::app_detail
