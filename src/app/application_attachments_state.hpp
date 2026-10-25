#pragma once
#include "application_attachments.hpp"
#include "maicord/app/message_attachment_drafts.hpp"
#include "application_upload_lane.hpp"
#include "maicord/ui/message_composer_scene.hpp"
namespace maicord::app_detail {
struct ApplicationAttachments {
    MessageAttachmentDraftStore drafts;
    NativeFilePicker picker;
    u64 profile_id;
    u64 picker_channel;
    u64 url_id;
    u64 url_request_id;
    u64 active_id;
    ApplicationUploadLease upload_lease;
    u64 detail_id;
    u64 menu_ids[kAttachmentUploadMaximumCount];
    u32 menu_count;
    u32 uploaded_filename_bytes;
    u32 notice_bytes;
    bool picker_consumed;
    bool picker_cancel_requested;
    bool finishing;
    bool closing;
    bool close_window;
    char label[kMessageComposerReplyLabelBytes];
    char notice[256];
    char menu_labels[kAttachmentUploadMaximumCount][256];
    char url[kHttpsUploadMaximumUrlBytes];
    char uploaded_filename[kAttachmentUploadMaximumUploadedFilenameBytes];
    // Upload-strip projection: drafts plus an optional preparing tile.
    UiComposerAttachmentTile tiles[kAttachmentUploadMaximumCount + 1];
    u32 tile_count;
    u32 preview_revision;
};
}
