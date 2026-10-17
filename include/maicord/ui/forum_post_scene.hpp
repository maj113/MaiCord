#pragma once
#include "maicord/ui/text_form.hpp"
#include "maicord/ui/attachment_draft_list.hpp"
#include "maicord/ui/scrollbar.hpp"

namespace maicord {
// Fresh inventory has at most 20 tags; five retained missing selections remain removable.
inline constexpr u32 kForumPostTagCapacity = 25;
inline constexpr u32 kForumPostSelectedTagMaximum = 5;
enum class ForumPostElement : u8 {
    root,
    panel,
    heading,
    context,
    title_label,
    title,
    body_label,
    body,
    hint,
    tags_hint,
    tags,
    status,
    risk,
    actions,
    cancel,
    inspect,
    accept,
    content,
    attachment_toolbar,
    add_files,
    attachment_status,
    attachments,
    count
};
inline constexpr u32 kForumPostElements = static_cast<u32>(ForumPostElement::count);
struct ForumPostTagView {
    u64 id;
    Utf8View name;
    bool selected, enabled;
};
// Display policy only. A legal empty wire label retains its ID and metadata.
constexpr Utf8View forum_post_tag_label(Utf8View name) noexcept {
    return name.bytes ? name : utf8_literal("Unnamed tag");
}
struct ForumPostFieldView {
    Utf8View text;
    u32 anchor_utf16, caret_utf16;
    ConstSlice<TextCompositionSpan> composition;
    bool caret_visible;
};
struct ForumPostStyle {
    float maximum_width = 560, inset = 16, padding = 20, gap = 10;
    float title_height = 44, body_height = 176, corner_radius = 6;
};
// Borrowed labels and model bytes must outlive renderer rebuild and drawing.
struct ForumPostContent {
    u64 owner_serial;
    Utf8View heading, context, title_label, body_label, hint, tags_hint, status;
    Utf8View cancel, inspect, accept, risk;
    ForumPostFieldView fields[2];
    ConstSlice<ForumPostTagView> tags;
    ConstSlice<UiAttachmentDraftView> attachments;
    Utf8View add_files, attachment_status;
    UiAttachmentDraftStyle attachment_style{};
    bool can_add_files;
    bool editable, can_accept, focus_visible, uncertain, risk_acknowledged;
    ForumPostStyle style{};
    u16 measure_revision{1};
};
struct ForumPostTarget {
    ForumPostElement element;
    u32 tag;
    u64 attachment_id;
    UiAttachmentDraftAction attachment_action;
    UiWidgetHandle attachment_widget;
};
struct ForumPostScene {
    UiFixedWidgetStorageOwner storage;
    UiWidgetManager widgets;
    UiWidgetHandle elements[kForumPostElements], tags[kForumPostTagCapacity];
    UiAttachmentDraftList attachment_list;
    UiTextMeasureCache* measure;
    ForumPostContent content;
    UiRect viewport;
    u64 layout_signature, tree_signature, text_signature[2], presentation_signature;
    u32 revision;
    bool active;
};
bool forum_post_update(ForumPostScene*, const ForumPostContent&, UiRect, UiTextMeasureCache*) noexcept;
void forum_post_release(ForumPostScene*) noexcept;
const UiLayoutNode* forum_post_node(const ForumPostScene&, ForumPostElement) noexcept;
ForumPostTarget forum_post_hit_test(const ForumPostScene&, float, float) noexcept;
ForumPostTarget forum_post_focused(const ForumPostScene&) noexcept;
bool forum_post_focus(ForumPostScene*, ForumPostTarget) noexcept;
bool forum_post_move_focus(ForumPostScene*, bool reverse) noexcept;
bool forum_post_scroll(ForumPostScene*, float delta) noexcept;
UiScrollbarGeometry forum_post_scrollbar(const ForumPostScene&) noexcept;
bool forum_post_paint(const ForumPostScene&, const TextFormEditGeometry (&)[2], const Theme&,
                      PaintStream*) noexcept;
bool forum_post_text_output(const ForumPostScene&, const TextFormEditGeometry (&)[2], const Theme&,
                            UiTextOutput*) noexcept;
} // namespace maicord
