#pragma once
#include "maicord/model/user_store.hpp"
#include "maicord/ui/geometry.hpp"
#include "maicord/ui/icon.hpp"

namespace maicord {
// The status line under a user's name in the DM list and the member list.
enum class UiActivityGlyph : u8 { none, game, music, screen };
struct UiActivityStatus {
    UiActivityGlyph glyph;
    // The custom status emoji: Unicode text, or a custom emoji's ID.
    Utf8View emoji;
    u64 emoji_id;
    Utf8View text;
};
UiActivityGlyph ui_activity_glyph(UserActivityType) noexcept;
// The official rule: the first non-custom activity's glyph and short text
// (user_activity_status_text), then the custom status, whose text replaces the
// activity's while the glyph stays. Without rich activities the appearance's
// line (custom status or "Playing X") shows alone. Bots get no glyph; the
// caller hides the line for offline users. `primary` is the first activity
// (null when none). False when there is nothing to show; text points into
// storage or the appearance record.
bool ui_activity_status(const UserActivity* primary, const UserAppearanceRecord* appearance, bool bot,
                        Slice<char> storage, UiActivityStatus* output) noexcept;
// The same from the store, with `appearance` the record that carries the line
// (a server's, else the global one). Without storage only the glyph and the
// custom status resolve, and no activity record is decoded.
bool ui_user_activity_status(const UserStore& users, u64 user_id, const UserAppearanceRecord* appearance,
                             Slice<char> storage, UiActivityStatus* output) noexcept;
UiIcon ui_activity_glyph_icon(UiActivityGlyph) noexcept;
// The glyph's box at the start of a status line, and the text after it.
inline constexpr float kUiActivityGlyphExtent = 12.f, kUiActivityGlyphGap = 4.f;
UiRect ui_activity_glyph_rectangle(UiRect line) noexcept;
UiRect ui_activity_glyph_text(UiRect line, UiActivityGlyph) noexcept;
} // namespace maicord
