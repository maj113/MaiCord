#pragma once
#include "application_forum_attachments.hpp"
#include "application_upload_lane.hpp"
#include "maicord/protocol/gateway_message_events.hpp"
namespace maicord::app_detail {
struct AccountUiRuntime;
struct ApplicationForumAttachments {
    ApplicationForumPostTicket ticket;
    MessageAttachmentDraftStore drafts;
    NativeFilePicker picker;
    ApplicationUploadLease lease;
    const void* instance;
    const AccountUiRuntime* ui;
    u64 scope, profile, self, guild, active_id, url_id;
    u16 parent_generation;
    u8 parent_type;
    bool closing, picker_consumed, picker_cancelled, finishing, locked, error, close_window;
    u32 revision, status_bytes, uploaded_bytes;
    char status[256];
    char url[kHttpsUploadMaximumUrlBytes];
    char uploaded_filename[kAttachmentUploadMaximumUploadedFilenameBytes];
    ApplicationForumAttachmentRow rows[kAttachmentUploadMaximumCount];
    ForumPostMediaSnapshot* receipt;
    ForumPostMediaSnapshot* transaction;
    GatewayMessageAttachment receipt_views[kAttachmentUploadMaximumCount];
    u32 receipt_bytes;
};
} // namespace maicord::app_detail
