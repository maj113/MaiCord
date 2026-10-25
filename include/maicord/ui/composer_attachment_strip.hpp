#pragma once

#include "maicord/media/asset_key.hpp"
#include "maicord/ui/geometry.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/theme.hpp"

namespace maicord {
struct UiTextOutput;
enum class UiIcon : u8;

// Composer upload area: a horizontally scrolling row of fixed 200 x 200 tiles
// inside the input surface, each with its media (contain-fit thumbnail or
// file-type icon), filename and a top-right action bar. Pure geometry, paint
// and text; the host owns drafts and thumbnails.
inline constexpr u32 kUiComposerAttachmentMaximum = 10;

// Classifier order: image/video MIME families, then extension classes. Unknown
// names use the generic file card.
enum class UiAttachmentFileKind : u8 {
    unknown,
    image,
    video,
    audio,
    archive,
    code,
    document,
    spreadsheet,
    webcode,
    acrobat,
};
UiAttachmentFileKind ui_attachment_file_kind(Utf8View filename) noexcept;
UiIcon ui_attachment_file_icon(UiAttachmentFileKind kind) noexcept;
// Lower-case ASCII extension (at most 8 bytes) without the dot; zero when absent.
u32 ui_attachment_file_extension(Utf8View filename, Slice<char> output) noexcept;
// "12 B", "3.4 KB", "56 MB", "1.2 GB" (binary units).
u32 ui_attachment_format_size(u64 bytes, Slice<char> output) noexcept;
// "0:07", "12:34", "1:02:03".
u32 ui_attachment_format_duration(u32 milliseconds, Slice<char> output) noexcept;

enum class UiComposerAttachmentPhase : u8 {
    preparing, // The picker/clipboard is still preparing files; click cancels.
    waiting,
    uploading,
    ready,
    failed,
};

enum class UiComposerAttachmentPart : u8 {
    none,
    tile,
    retry,
    spoiler,
    edit,
    remove,
};

struct UiComposerAttachmentTile {
    u64 id;
    Utf8View filename;
    u64 bytes;
    u64 uploaded_bytes;
    // Valid when a thumbnail may be resident in the preview atlas.
    MediaAssetKey preview;
    u32 duration_ms;
    UiAttachmentFileKind kind;
    UiComposerAttachmentPhase phase;
    bool spoiler;
    bool described;
    bool animated;
    bool preview_ready;
    bool can_retry;
};

struct UiComposerAttachmentStripStyle {
    float tile = 200;
    float padding = 8;
    float gap = 24;
    float leading = 10;
    float top = 16;
    float bottom = 8;
    float trailing = 30;
    float name_height = 18;
    float name_gap = 8;
    float action = 28;
    float action_padding = 3;
    float tag_height = 16;
    float progress = 3;
    float icon = 48;
};

constexpr float ui_composer_attachment_strip_height(const UiComposerAttachmentStripStyle& style = {}) noexcept {
    return style.top + style.tile + style.bottom;
}

inline constexpr u32 kUiComposerAttachmentButtons = 4;
inline constexpr u32 kUiComposerAttachmentTags = 4;
struct UiComposerAttachmentTag {
    UiRect rectangle;
    Utf8View text;
};
struct UiComposerAttachmentTileGeometry {
    UiRect card;
    UiRect media;
    UiRect name;
    UiRect size;
    UiRect actions;
    UiRect progress;
    UiRect buttons[kUiComposerAttachmentButtons];
    UiComposerAttachmentPart parts[kUiComposerAttachmentButtons];
    u32 button_count;
};

float ui_composer_attachment_strip_content_width(u32 count, const UiComposerAttachmentStripStyle& style = {}) noexcept;
float ui_composer_attachment_strip_clamp_scroll(UiRect strip, u32 count, float scroll,
                                                const UiComposerAttachmentStripStyle& style = {}) noexcept;
// Smallest scroll change that shows the whole tile (and its action bar).
float ui_composer_attachment_strip_reveal(UiRect strip, u32 count, u32 index, float scroll,
                                          const UiComposerAttachmentStripStyle& style = {}) noexcept;
UiComposerAttachmentTileGeometry ui_composer_attachment_tile_geometry(
    UiRect strip, const UiComposerAttachmentTile& tile, u32 index, float scroll,
    const UiComposerAttachmentStripStyle& style = {}) noexcept;
// Media rule: object-fit contain, centred, never upscaled past 2x.
UiRect ui_composer_attachment_fit(UiRect media, u32 width, u32 height) noexcept;
// Spoilered upload media blur with a 44 px deviation (CSS blur(44px)) and keep
// the SPOILER tag; small thumbnails cap the deviation at a quarter of their
// shorter side so they stay a soft image instead of a flat fill.
inline constexpr float kUiComposerAttachmentSpoilerBlur = 44.f;
// Gaussian standard deviation (DIPs) the renderer applies to a fitted
// thumbnail; zero for tiles that are not spoilered.
float ui_composer_attachment_thumbnail_blur(const UiComposerAttachmentTile& tile, UiRect fitted) noexcept;
// Bottom-left pills: duration or GIF, ALT, SPOILER. Text views point at
// literals or duration_storage, which must outlive their use.
u32 ui_composer_attachment_tile_tags(const UiComposerAttachmentTile& tile, UiRect media,
                                     Slice<UiComposerAttachmentTag> output, Slice<char> duration_storage,
                                     const UiComposerAttachmentStripStyle& style = {}) noexcept;

struct UiComposerAttachmentHit {
    u32 index;
    UiComposerAttachmentPart part;
};
inline constexpr u32 kUiComposerAttachmentNoTile = 0xffffffffu;
UiComposerAttachmentHit ui_composer_attachment_strip_hit(UiRect strip, ConstSlice<UiComposerAttachmentTile> tiles,
                                                         float scroll, float x, float y,
                                                         const UiComposerAttachmentStripStyle& style = {}) noexcept;

struct UiComposerAttachmentStripView {
    ConstSlice<UiComposerAttachmentTile> tiles;
    UiRect strip;
    float scroll;
    u32 hovered;
    UiComposerAttachmentPart hovered_part;
    u32 selected;
    bool focus_visible;
    UiWidgetReference owner;
};
// Cards, media wells, file icons and spoiler veils: drawn below thumbnails.
bool ui_composer_attachment_strip_paint_base(const UiComposerAttachmentStripView& view, const Theme& theme,
                                             PaintStream* paint,
                                             const UiComposerAttachmentStripStyle& style = {}) noexcept;
// Tags, progress, failure outline, selection ring and the action bar: drawn
// above thumbnails and below text.
bool ui_composer_attachment_strip_paint_overlay(const UiComposerAttachmentStripView& view, const Theme& theme,
                                                PaintStream* paint,
                                                const UiComposerAttachmentStripStyle& style = {}) noexcept;
bool ui_composer_attachment_strip_text(const UiComposerAttachmentStripView& view, const Theme& theme,
                                       UiTextOutput* output,
                                       const UiComposerAttachmentStripStyle& style = {}) noexcept;
// Visible media wells that should show a resident thumbnail; spoilered tiles
// show theirs blurred (ui_composer_attachment_thumbnail_blur).
bool ui_composer_attachment_thumbnail_visible(const UiComposerAttachmentStripView& view, u32 index,
                                              UiRect* media, UiRect* clip,
                                              const UiComposerAttachmentStripStyle& style = {}) noexcept;

} // namespace maicord
