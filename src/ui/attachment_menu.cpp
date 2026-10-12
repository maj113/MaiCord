#include "maicord/ui/attachment_menu.hpp"
namespace maicord {
u32 attachment_file_menu(Utf8View filename, bool spoiler, Slice<PopupItem> output) noexcept {
    if (!filename.data || !filename.bytes || !output.data || output.count < kAttachmentFileMenuItems) return 0;
    output[0] = {filename, popup_action_attachment_title, 0, 0};
    output[1] = {spoiler ? utf8_literal("Remove spoiler") : utf8_literal("Mark as spoiler"),
        popup_action_attachment_spoiler, static_cast<u16>(popup_item_enabled | (spoiler ? popup_item_checked : 0)), 0};
    output[2] = {utf8_literal("Edit description"), popup_action_attachment_description, popup_item_enabled, 0};
    output[3] = {utf8_literal("Remove attachment"), popup_action_attachment_remove,
        static_cast<u16>(popup_item_enabled | popup_item_danger), 0};
    output[4] = {utf8_literal("Back to attachments"), popup_action_attachment_back,
        static_cast<u16>(popup_item_enabled | popup_item_separator_before), 0};
    return kAttachmentFileMenuItems;
}
u32 attachment_modify_menu(Utf8View filename, bool described, Slice<PopupItem> output) noexcept {
    if (!filename.data || !filename.bytes || !output.data || output.count < kAttachmentModifyMenuItems) return 0;
    output[0] = {filename, popup_action_attachment_title, 0, 0};
    output[1] = {utf8_literal("Rename file"), popup_action_attachment_rename, popup_item_enabled, 0};
    output[1].presentation = ui_menu_row_presentation(UiIcon::edit);
    output[2] = {described ? utf8_literal("Edit description (alt text)") : utf8_literal("Add description (alt text)"),
        popup_action_attachment_description, popup_item_enabled, 0};
    output[2].presentation = ui_menu_row_presentation(UiIcon::image);
    return kAttachmentModifyMenuItems;
}
}
