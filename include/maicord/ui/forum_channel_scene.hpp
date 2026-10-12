#pragma once
#include "maicord/ui/geometry.hpp"
#include "maicord/core/slice.hpp"
#include "maicord/core/utf.hpp"

namespace maicord {
struct Theme;
struct PaintStream;
struct UiTextOutput;
struct UiThreadBrowserScene;

// Forum (type 15) and media (type 16) channel post list. It replaces the plain
// row list of the thread browser panel for those parents: the panel keeps its
// header, tabs, status and pagination, and this module lays out,
// paints and labels the post cards and the tag/sort/layout toolbar inside it.
inline constexpr u32 kUiForumTagCapacity = 20;
inline constexpr u32 kUiForumPostTagCapacity = 5;
enum class UiForumSort : u8 { latest_activity, creation_date };
enum class UiForumLayout : u8 { list, gallery };
enum class UiForumTagMatch : u8 { some, all };
struct UiForumTag {
    u64 id;
    Utf8View name;
    bool selected;
};
// One post per UiThreadBrowserView row, same order and thread ID. Text is
// borrowed from the host until the view revision changes.
struct UiForumPost {
    u64 thread_id, author_id;
    Utf8View preview, author_name, activity, avatar_hash;
    u64 tags[kUiForumPostTagCapacity];
    u32 reply_count;
    u8 tag_count;
    bool pinned, has_media, metadata_known;
    // First image of the starter message (has_media); format is a MediaAttachmentFormat.
    Utf8View media_url;
    u64 media_id;
    u32 media_width, media_height;
    u8 media_format;
};
struct UiForumChannelView {
    ConstSlice<UiForumTag> tags;
    ConstSlice<UiForumPost> posts;
    UiForumSort sort;
    UiForumLayout layout;
    UiForumTagMatch match;
    bool media_channel;
};
struct UiForumStyle {
    float toolbar_row_height, control_height, sort_width, layout_width, chip_height, chip_row_height, chip_gap;
    float chip_min_width, chip_max_width, chip_scalar_width, chip_padding;
    float card_gap, list_card_height, list_media, gallery_min_width, gallery_card_height, gallery_media_height;
    float card_inset, card_tag_height, avatar, radius;
};
inline constexpr UiForumStyle kDefaultUiForumStyle{
    40, 30, 176, 76, 26, 32, 6,
    52, 168, 7.5f, 28,
    8, 116, 88, 216, 248, 136,
    12, 18, 20, 8
};
bool ui_forum_view_valid(const UiForumChannelView&) noexcept;

// ---- ordering and filtering (official client parity) ------------------------
// No selection matches every post. Otherwise an untagged post never matches;
// "some" needs any selected tag, "all" needs every selected tag.
bool ui_forum_post_matches(ConstSlice<u64> applied, ConstSlice<u64> selected, UiForumTagMatch) noexcept;
struct UiForumSortKey {
    u64 thread_id, last_message_id;
    u32 index;
    bool pinned;
    u8 reserved[3];
};
// Pinned posts first, then newest first by last activity (last message, else
// the post's own ID) or by creation (the post ID); ties by ID, descending.
void ui_forum_sort(Slice<UiForumSortKey>, UiForumSort) noexcept;

// ---- geometry ----------------------------------------------------------------
struct UiForumToolbar {
    UiRect sort, list, gallery;
    UiRect chips[kUiForumTagCapacity];
    u32 chip_count;
    float height;
};
// Lays the toolbar out from band's left/top/right; height is the band it needs.
void ui_forum_toolbar_layout(UiRect band, const UiForumChannelView&, const UiForumStyle&, UiForumToolbar*) noexcept;
struct UiForumGrid {
    u32 columns;
    float card_width, card_height, gap;
};
UiForumGrid ui_forum_grid(float width, UiForumLayout, const UiForumStyle&) noexcept;
UiRect ui_forum_card_rectangle(UiRect body, UiForumGrid, u32 index, float scroll) noexcept;
float ui_forum_content_height(UiForumGrid, u32 count) noexcept;
struct UiForumCardParts {
    UiRect tags, title, preview, avatar, author, meta, media;
};
UiForumCardParts ui_forum_card_parts(UiRect card, UiForumLayout, const UiForumPost&, const UiForumStyle&) noexcept;

// ---- scene integration (called by the thread browser panel) ------------------
// Avatar and thumbnail placements for media requests; false when culled.
bool ui_forum_channel_avatar(const UiThreadBrowserScene&, u32 index, UiRect* rectangle, UiRect* clip) noexcept;
bool ui_forum_channel_media(const UiThreadBrowserScene&, u32 index, UiRect* rectangle, UiRect* clip) noexcept;
bool ui_forum_channel_paint(const UiThreadBrowserScene&, const Theme&, PaintStream*) noexcept;
bool ui_forum_channel_text_output(const UiThreadBrowserScene&, const Theme&, UiTextOutput*) noexcept;
Utf8View ui_forum_sort_label(UiForumSort) noexcept;
} // namespace maicord
