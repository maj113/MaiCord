#pragma once
#include "maicord/media/asset_key.hpp"
#include "maicord/ui/hover_card.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/text_presentation.hpp"

namespace maicord {
struct UiTextMeasureCache;

// The emoji and sticker popout, opened by clicking an emoji or a sticker in a
// message: the official client's 288 DIP card beside it. Pure geometry and
// paint; the host resolves the content, draws the picture, the pack's stickers
// and the server icon into ui_expression_card_media and runs the controls.
inline constexpr float kUiExpressionCardWidth = 288, kUiExpressionCardPadding = 16, kUiExpressionCardGap = 8;
inline constexpr float kUiExpressionCardPicture = 48, kUiExpressionCardPreview = 80, kUiExpressionCardGuildIcon = 32;
inline constexpr u32 kUiExpressionCardTitleBytes = 160, kUiExpressionCardTextBytes = 320,
                     kUiExpressionCardNameBytes = 128, kUiExpressionCardPreviews = 3;
inline constexpr u32 kUiExpressionCardEntranceMs = 200;

enum class UiExpressionCardLayout : u8 {
    loading, // The official client's loader while the sticker is fetched.
    head,    // The picture beside its name and description (an emoji, a server sticker).
    pack,    // A standard sticker: its name, its pack and the pack's stickers.
};
enum class UiExpressionCardControl : u8 {
    none,
    guild,  // The source server's icon or name (opens the server).
    button, // The pack layout's button.
};
struct UiExpressionCardContent {
    UiExpressionCardLayout layout;
    // head: a custom emoji or sticker image, else a Unicode emoji drawn as text.
    MediaAssetKey picture;
    Utf8View glyph;
    // All strings are copied.
    Utf8View title, description;
    // A bold range of the description (the pack's name); empty without one.
    u32 emphasis_offset, emphasis_bytes;
    // pack: up to three of the pack's other stickers, and the button label.
    MediaAssetKey previews[kUiExpressionCardPreviews];
    u32 preview_count;
    Utf8View button;
    // The source server below the head ("This emoji is from"), shown when the
    // heading is set; the icon else its acronym; linked when it can be opened.
    Utf8View guild_heading, guild_name, guild_subtitle, guild_acronym;
    MediaAssetKey guild_icon;
    bool guild_link;
};
struct UiExpressionCardScene : UiHoverCard {
    UiExpressionCardLayout layout;
    UiRect picture, title, description, guild_section, guild_heading, guild_icon, guild_name, guild_subtitle,
        button, skeleton;
    UiRect previews[kUiExpressionCardPreviews];
    MediaAssetKey picture_key, preview_keys[kUiExpressionCardPreviews], guild_icon_key;
    char title_text[kUiExpressionCardTitleBytes], description_text[kUiExpressionCardTextBytes],
        glyph[48], button_text[48], guild_heading_text[48], guild_name_text[kUiExpressionCardNameBytes],
        guild_subtitle_text[64], guild_acronym_text[16];
    u32 title_bytes, description_bytes, glyph_bytes, button_bytes, guild_heading_bytes, guild_name_bytes,
        guild_subtitle_bytes, guild_acronym_bytes, emphasis_offset, emphasis_bytes, preview_count;
    bool guild_link, guild_hovered;
};

// Shows (or re-projects) the card for identity beside anchor inside viewport:
// right of it with an 8 DIP gap, else left, centred on it and kept inside
// the viewport. measure sizes the title and the wrapping text when given.
bool ui_expression_card_show(UiExpressionCardScene*, u64 identity, const UiExpressionCardContent&, UiRect anchor,
                             UiRect viewport, u64 now_ms, bool reduced_motion, UiTextMeasureCache* measure) noexcept;
void ui_expression_card_close(UiExpressionCardScene*) noexcept;
bool ui_expression_card_tick(UiExpressionCardScene*, u64 now_ms, bool reduced_motion) noexcept;
u64 ui_expression_card_next_wake(const UiExpressionCardScene&, u64 now_ms, bool reduced_motion) noexcept;
bool ui_expression_card_contains(const UiExpressionCardScene&, float x, float y) noexcept;
UiExpressionCardControl ui_expression_card_hit(const UiExpressionCardScene&, float x, float y) noexcept;
// True when the hovered control changed.
bool ui_expression_card_set_hover(UiExpressionCardScene*, float x, float y) noexcept;
bool ui_expression_card_set_pressed(UiExpressionCardScene*, bool pressed) noexcept;
// Media the host draws: 0 the picture, 1..3 the pack's stickers, 4 the server icon.
inline constexpr u32 kUiExpressionCardMediaCount = 5;
struct UiExpressionCardMedia {
    MediaAssetKey key;
    UiRect rectangle, clip;
    float radius, opacity;
};
bool ui_expression_card_media(const UiExpressionCardScene&, u32 index, UiExpressionCardMedia* output) noexcept;
bool ui_expression_card_paint(const UiExpressionCardScene&, const Theme&, PaintStream*) noexcept;
bool ui_expression_card_text_output(const UiExpressionCardScene&, const Theme&, UiTextOutput*) noexcept;
} // namespace maicord
