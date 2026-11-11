#pragma once
#include "maicord/ui/account/profile_activity.hpp"
#include "maicord/ui/core/geometry.hpp"
#include "maicord/ui/kit/scene_kit.hpp"

namespace maicord {
// The activity card: one layout of a rich presence for the profile popout, the
// full profile and Active Now. The content follows profile_activity_content;
// the layout places it as items relative to the card's top-left, which the
// host turns into its widgets (the profile) or paints (Active Now). A card
// owns every string its items show, so it must stay where it was laid out.
enum class UiActivityCardSize : u8 {
    // 8 DIP padding, 60 DIP art, one-line text (a cut line's tooltip is its
    // full text), buttons full width under the body.
    popout,
    // 12 DIP padding, 100 DIP art, wrapping text, buttons under the text.
    full,
    // Active Now's section: no padding or heading, 32 DIP art, one-line text
    // and Ask to Join in a column on the right.
    compact
};
enum class UiActivityCardRole : u8 {
    heading,
    title,
    line,
    // A chip's glyph (icon) and label; the timer chip's are clock_glyph and
    // clock and tick.
    glyph,
    chip,
    clock_glyph,
    clock,
    // The progress bar: its ends (elapsed ticks) and its track.
    elapsed,
    total,
    bar,
    // The artwork (which image: UiActivityCard::art.large; the placeholder's
    // glyph in icon) and the small image on its corner.
    art,
    badge,
    // A button: its plate (the target; its label in text for accessibility),
    // then its leading glyph and its label.
    button,
    button_glyph,
    button_label
};
enum class UiActivityCardAction : u8 {
    none,
    // The activity's own button (index): its URL comes from the activity's
    // metadata.
    button,
    // A Twitch or YouTube stream's page.
    watch,
    // Ask to Join, disabled ("<name> is not detected").
    join,
    // Spotify: the track's page (Play on Spotify, the track title).
    spotify_track,
    // Spotify: the album's page (the album art; its ID is in the metadata).
    spotify_album
};
enum class UiActivityCardTone : u8 {
    normal,
    strong,
    subtle,
    positive
};
struct UiActivityCardItem {
    UiRect rect;
    Utf8View text;
    // Tooltip: a cut line's full text, an image's hover text, a disabled
    // button's reason.
    Utf8View hint;
    u32 flags; // text style
    UiActivityCardRole role;
    UiActivityCardTone tone;
    UiActivityCardAction action;
    u8 index;
    UiIcon icon; // UiIcon::count: none
    bool wrap;   // the text takes more than one line
    bool disabled;
};
// The most items a card lays out (a full or popout card: the heading, the art and its badge, the title, two lines,
// three chips of two items, the progress bar's three and two buttons of at most two items, or one of three).
inline constexpr u32 kUiActivityCardItems = 19;
inline constexpr u8 kUiActivityCardNone = 0xFF;
// Text size in DIPs: one line's width, or with wrap the height at width.
using UiActivityCardMeasure = UiSize (*)(void* context, Utf8View text, u32 flags, float width, bool wrap) noexcept;
struct UiActivityCardInput {
    UiActivityCardSize size;
    float width;
    u64 now_ms;
    // bot: no artwork or chips. self: no actions. interactive: buttons show.
    bool bot, self, interactive;
    // The application's icon is known (it stands in for missing art).
    bool application_icon;
    // Null estimates widths and never wraps or cuts.
    UiActivityCardMeasure measure;
    void* context;
};
struct UiActivityCard {
    ProfileActivityContent content;
    // The timer chip, the progress bar's ends; the images' hover text and the
    // activity's button labels.
    ProfileString<16> time[3];
    ProfileString<128> art_hint, badge_hint;
    ProfileString<96> labels[2];
    UiActivityCardItem items[kUiActivityCardItems];
    u32 count;
    float height;
    // Played fraction of the progress bar.
    float fraction;
    UserActivityArtwork art;
    // The ticking items (kUiActivityCardNone when absent).
    u8 clock, clock_glyph, bar;
};
// False for a custom status (not a card).
bool ui_activity_card_layout(const UserActivity&, const UiActivityCardInput&, UiActivityCard*) noexcept;
// Advances the timer chip and progress bar to now in place. relayout: a time
// changed its length or a timer ended.
enum class UiActivityCardTick : u8 {
    unchanged,
    changed,
    relayout
};
UiActivityCardTick ui_activity_card_tick(UiActivityCard*, const UserActivity&, u64 now_ms) noexcept;
bool ui_activity_card_timed(const UiActivityCard&) noexcept;

// Scenes without widgets of their own (Active Now) paint a card at origin:
// the plates, glyphs, progress track and art placeholders (the host draws the
// decoded images over the art and badge items), then the text through a pen.
bool ui_activity_card_paint(const UiActivityCard&, UiPoint origin, const Theme&, PaintStream*) noexcept;
bool ui_activity_card_text(const UiActivityCard&, UiPoint origin, const Theme&, UiTextPen<>& pen,
                           UiWidgetHandle owner, const UiRect& clip) noexcept;
} // namespace maicord
