#pragma once
#include "maicord/media/core/asset_key.hpp"
#include "maicord/ui/widgets/hover_card.hpp"
#include "maicord/ui/core/paint.hpp"
#include "maicord/ui/text/presentation.hpp"

namespace maicord {
struct UiTextMeasureCache;
// The popout under a one-to-one DM header's avatar and name: the peer's username with "AKA", a divider, then one
// row per nickname they use in the servers you share, with those servers' pictures (up to three, then "+N").
// Pure geometry and paint; the host owns the hover timing and draws the server pictures into ui_names_card_media.
// The card is as wide as its longest line within 180 to 280 DIPs of content, 16 DIPs of padding around it.
inline constexpr float kUiNamesCardPadding = 16, kUiNamesCardGap = 8, kUiNamesCardMinimum = 180,
                       kUiNamesCardMaximum = 280, kUiNamesCardLine = 20, kUiNamesCardRowGap = 4,
                       kUiNamesCardIcon = 20, kUiNamesCardIconGap = 4, kUiNamesCardNicknameGap = 12;
inline constexpr u32 kUiNamesCardRows = 6, kUiNamesCardIcons = 3, kUiNamesCardTextBytes = 128;
inline constexpr u32 kUiNamesCardEntranceMs = 150;
struct UiNamesCardRow {
    Utf8View nickname; // Copied.
    u32 server_count;  // Every server with it; the first kUiNamesCardIcons draw.
    // A server's picture, or an invalid key and its initial (a Unicode character, copied) in a plain disc.
    MediaAssetKey icons[kUiNamesCardIcons];
    Utf8View initials[kUiNamesCardIcons];
};
struct UiNamesCardContent {
    Utf8View username; // Copied.
    UiNamesCardRow rows[kUiNamesCardRows];
    u32 row_count;
};
// identity (UiHoverCard) is the user shown (host value).
struct UiNamesCardScene : UiHoverCard {
    UiRect username, label, divider, nicknames[kUiNamesCardRows], more[kUiNamesCardRows];
    UiRect icons[kUiNamesCardRows * kUiNamesCardIcons];
    char username_text[kUiNamesCardTextBytes], nickname_text[kUiNamesCardRows][kUiNamesCardTextBytes];
    char initial_text[kUiNamesCardRows * kUiNamesCardIcons][8], more_text[kUiNamesCardRows][8];
    MediaAssetKey keys[kUiNamesCardRows * kUiNamesCardIcons];
    u32 username_bytes, nickname_bytes[kUiNamesCardRows], initial_bytes[kUiNamesCardRows * kUiNamesCardIcons],
        more_bytes[kUiNamesCardRows], icon_count[kUiNamesCardRows], row_count;
};

// Shows (or re-projects) the card for identity under anchor inside viewport (above it when there is no room
// below); measure sizes the lines when given. A new identity restarts the entrance.
bool ui_names_card_show(UiNamesCardScene*, u64 identity, const UiNamesCardContent&, UiRect anchor, UiRect viewport,
                        u64 now_ms, bool reduced_motion, UiTextMeasureCache* measure) noexcept;
void ui_names_card_close(UiNamesCardScene*) noexcept;
bool ui_names_card_tick(UiNamesCardScene*, u64 now_ms, bool reduced_motion) noexcept;
u64 ui_names_card_next_wake(const UiNamesCardScene&, bool reduced_motion) noexcept;
bool ui_names_card_contains(const UiNamesCardScene&, float x, float y) noexcept;
// Server picture `index` (row * kUiNamesCardIcons + icon) the host draws round; false past the shown ones and for
// a server without a picture.
struct UiNamesCardMedia {
    MediaAssetKey key;
    UiRect rectangle, clip;
    float opacity;
};
bool ui_names_card_media(const UiNamesCardScene&, u32 index, UiNamesCardMedia* output) noexcept;
bool ui_names_card_paint(const UiNamesCardScene&, const Theme&, PaintStream*) noexcept;
bool ui_names_card_text_output(const UiNamesCardScene&, const Theme&, UiTextOutput*) noexcept;
} // namespace maicord
