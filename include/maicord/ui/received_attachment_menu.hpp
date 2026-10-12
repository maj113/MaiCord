#pragma once
#include "maicord/ui/popup_scene.hpp"

namespace maicord {
// A received attachment's actions merge into the message context menu after the
// message sections: the media group (Copy Image, Save) and the link group (Copy
// Link, Open Link), then the developer-mode Copy Message ID. A right-clicked
// sticker image adds only the link group. This model builds only the media and
// link groups. Action ids use the downloads namespace, outside every built-in
// message action.
inline constexpr u32 kReceivedAttachmentActionSave = 0x444C0001u;
inline constexpr u32 kReceivedAttachmentActionCopyImage = 0x444C0002u;
inline constexpr u32 kReceivedAttachmentActionCopyLink = 0x444C0003u;
inline constexpr u32 kReceivedAttachmentActionOpenLink = 0x444C0004u;
constexpr bool received_attachment_menu_action(u32 action) noexcept {
    return action >= kReceivedAttachmentActionSave && action <= kReceivedAttachmentActionOpenLink;
}

enum class ReceivedAttachmentKind : u8 {
    image,          // still image that can be copied (png, jpeg, webp)
    animated_image, // gif: saved, never copied as a bitmap
    video,
    audio,
    voice_message,
    file,
    sticker, // a message sticker image: only its link pair is offered
};

// The link pair is named after the URL: a cdn.discordapp.com still image
// is an "Image Link", any other image/video file name a "Media Link".
enum class ReceivedAttachmentLinkKind : u8 {
    link,
    image,
    media
};
ReceivedAttachmentLinkKind received_attachment_link_kind(Utf8View url) noexcept;

struct ReceivedAttachmentMenuFacts {
    ReceivedAttachmentKind kind;
    Utf8View url;    // read while building only; item labels are literals
    bool copy_image; // decoded pixels are resident for this still image
    bool save;       // the download owner accepts this attachment now
};
inline constexpr u32 kReceivedAttachmentMenuItems = 4;
// Returns the item count (zero when the URL is not an https URL or the output
// is too small). Group starts carry popup_item_separator_before.
u32 received_attachment_menu(const ReceivedAttachmentMenuFacts& facts, Slice<PopupItem> output) noexcept;

// A right-clicked link in message text, an embed or a forward gets the link
// group (Copy Link, Open Link, named after the URL as above), merged into the
// message menu in the same place. Open Link follows the link's click, including
// its leaving-site warning.
inline constexpr u32 kMessageLinkActionCopy = 0x4C4E0001u;
inline constexpr u32 kMessageLinkActionOpen = 0x4C4E0002u;
constexpr bool message_link_menu_action(u32 action) noexcept {
    return action == kMessageLinkActionCopy || action == kMessageLinkActionOpen;
}
inline constexpr u32 kMessageLinkMenuItems = 2;
// Returns the item count (zero for an empty URL or a too-small output).
u32 message_link_menu(Utf8View url, Slice<PopupItem> output) noexcept;
} // namespace maicord
