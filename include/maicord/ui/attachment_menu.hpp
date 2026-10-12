#pragma once
#include "maicord/ui/popup_scene.hpp"

namespace maicord {
enum AttachmentMenuAction : u32 {
    popup_action_attachment_file_first = 2'000,
    popup_action_attachment_retry = 2'010,
    popup_action_attachment_dismiss = 2'011,
    popup_action_attachment_cancel_selection = 2'012,
    popup_action_attachment_spoiler = 2'013,
    popup_action_attachment_remove = 2'014,
    popup_action_attachment_back = 2'015,
    popup_action_attachment_title = 2'016,
    popup_action_attachment_description = 2'017,
    // The composer's add button menu.
    popup_action_attachment_upload = 2'018,
    popup_action_attachment_create_poll = 2'019,
    // The upload tile's Modify Attachment menu.
    popup_action_attachment_rename = 2'020,
};
inline constexpr u32 kAttachmentFileMenuItems = 5;
// File-name storage belongs to the caller and must outlive the open popup.
// This view has no upload, account or filesystem dependencies.
u32 attachment_file_menu(Utf8View filename, bool spoiler, Slice<PopupItem> output) noexcept;
inline constexpr u32 kAttachmentModifyMenuItems = 3;
// The official client's Modify Attachment fields: filename and description (alt text).
u32 attachment_modify_menu(Utf8View filename, bool described, Slice<PopupItem> output) noexcept;
}
