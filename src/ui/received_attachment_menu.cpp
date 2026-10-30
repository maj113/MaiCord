#include "maicord/ui/received_attachment_menu.hpp"

namespace maicord {
namespace {
bool equal_ascii_case(const char* data, u32 bytes, const char* literal) noexcept {
    u32 index = 0;
    for (; index < bytes && literal[index]; ++index) {
        char c = data[index];
        if (c >= 'A' && c <= 'Z')
            c = static_cast<char>(c - 'A' + 'a');
        if (c != literal[index])
            return false;
    }
    return index == bytes && !literal[index];
}
// Index of the final path extension, or count when no extension matches.
u32 path_extension(Utf8View path, const char* const* extensions, u32 count) noexcept {
    u32 dot = path.bytes;
    for (u32 index = path.bytes; index-- > 0;) {
        if (path.data[index] == '/')
            break;
        if (path.data[index] == '.') {
            dot = index;
            break;
        }
    }
    if (dot == path.bytes)
        return count;
    for (u32 index = 0; index < count; ++index)
        if (equal_ascii_case(path.data + dot + 1, path.bytes - dot - 1, extensions[index]))
            return index;
    return count;
}
// The first seven entries are still-image formats with special CDN wording.
constexpr u32 kImageExtensionCount = 7;
constexpr const char* kMediaExtensions[]{"png", "jpg", "jpeg", "webp", "avif", "bmp",
                                         "svg", "gif", "mp4",  "webm", "mov"};
template <u32 N> constexpr u32 count_of(const char* const (&)[N]) noexcept {
    return N;
}
Utf8View link_label(ReceivedAttachmentLinkKind link, bool open) noexcept {
    static constexpr Utf8View labels[2][3]{
        {utf8_literal("Copy Link"), utf8_literal("Copy Image Link"), utf8_literal("Copy Media Link")},
        {utf8_literal("Open Link"), utf8_literal("Open Image Link"), utf8_literal("Open Media Link")}};
    return labels[open][static_cast<u32>(link)];
}
} // namespace

ReceivedAttachmentLinkKind received_attachment_link_kind(Utf8View url) noexcept {
    constexpr Utf8View scheme = utf8_literal("https://");
    if (!url.data || url.bytes <= scheme.bytes || !equal_ascii_case(url.data, scheme.bytes, "https://"))
        return ReceivedAttachmentLinkKind::link;
    u32 host_end = scheme.bytes;
    while (host_end < url.bytes && url.data[host_end] != '/' && url.data[host_end] != '?' &&
           url.data[host_end] != '#')
        ++host_end;
    u32 path_end = host_end;
    while (path_end < url.bytes && url.data[path_end] != '?' && url.data[path_end] != '#')
        ++path_end;
    const Utf8View path{url.data + host_end, path_end - host_end};
    const u32 extension = path_extension(path, kMediaExtensions, count_of(kMediaExtensions));
    if (extension == count_of(kMediaExtensions))
        return ReceivedAttachmentLinkKind::link;
    return extension < kImageExtensionCount &&
                   equal_ascii_case(url.data + scheme.bytes, host_end - scheme.bytes, "cdn.discordapp.com")
               ? ReceivedAttachmentLinkKind::image
               : ReceivedAttachmentLinkKind::media;
}

u32 received_attachment_menu(const ReceivedAttachmentMenuFacts& facts, Slice<PopupItem> output) noexcept {
    if (!output.data || output.count < kReceivedAttachmentMenuItems || !facts.url.data ||
        facts.url.bytes <= 8 || !equal_ascii_case(facts.url.data, 8, "https://"))
        return 0;
    u32 count = 0;
    const auto add = [&](Utf8View label, u32 action, UiIcon icon, bool group_start) noexcept {
        PopupItem item{
            label, action,
            static_cast<u16>(popup_item_enabled | (group_start ? popup_item_separator_before : 0)), 0};
        item.presentation = ui_menu_row_presentation(icon);
        output[count++] = item;
    };
    const bool still = facts.kind == ReceivedAttachmentKind::image;
    const bool image = still || facts.kind == ReceivedAttachmentKind::animated_image;
    if (still && facts.copy_image)
        add(utf8_literal("Copy Image"), kReceivedAttachmentActionCopyImage, UiIcon::image, true);
    if (facts.save && facts.kind != ReceivedAttachmentKind::sticker)
        add(image                                         ? utf8_literal("Save Image")
            : facts.kind == ReceivedAttachmentKind::video ? utf8_literal("Save Video")
                                                          : utf8_literal("Save File"),
            kReceivedAttachmentActionSave, image ? UiIcon::image : UiIcon::download, count == 0);
    const auto link = received_attachment_link_kind(facts.url);
    add(link_label(link, false), kReceivedAttachmentActionCopyLink, UiIcon::link, true);
    add(link_label(link, true), kReceivedAttachmentActionOpenLink, UiIcon::external_link, false);
    return count;
}

namespace {
u32 link_pair(const Utf8View (&labels)[kMessageLinkMenuItems], const u32 (&actions)[kMessageLinkMenuItems],
              const UiIcon (&icons)[kMessageLinkMenuItems], Slice<PopupItem> output) noexcept {
    if (!output.data || output.count < kMessageLinkMenuItems)
        return 0;
    for (u32 i = 0; i < kMessageLinkMenuItems; ++i) {
        PopupItem item{labels[i], actions[i],
                       static_cast<u16>(popup_item_enabled | (i == 0 ? popup_item_separator_before : 0)), 0};
        item.presentation = ui_menu_row_presentation(icons[i]);
        output[i] = item;
    }
    return kMessageLinkMenuItems;
}
} // namespace

u32 message_link_menu(Utf8View url, Slice<PopupItem> output) noexcept {
    if (!url.data || !url.bytes)
        return 0;
    const auto link = received_attachment_link_kind(url);
    const Utf8View labels[]{link_label(link, false), link_label(link, true)};
    static constexpr u32 actions[]{kMessageLinkActionCopy, kMessageLinkActionOpen};
    static constexpr UiIcon icons[]{UiIcon::link, UiIcon::external_link};
    return link_pair(labels, actions, icons, output);
}

u32 attachment_link_menu(Slice<PopupItem> output) noexcept {
    static constexpr Utf8View labels[]{utf8_literal("Copy Media Link"), utf8_literal("Copy Text")};
    static constexpr u32 actions[]{kMessageLinkActionCopy, kMessageLinkActionCopyText};
    static constexpr UiIcon icons[]{UiIcon::link, UiIcon::copy};
    return link_pair(labels, actions, icons, output);
}
} // namespace maicord
