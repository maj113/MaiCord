#pragma once

#include "maicord/ui/account/profile_scene.hpp"
#include "maicord/ui/account/profile_activity.hpp"
#include "maicord/ui/plugins/slots.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/sys/crt.hpp"

namespace maicord::profile_scene_detail {

constexpr u32 capacity = 320;
// One widget per part and the root.
constexpr u32 kWidgets = capacity + 1;
// Chips a card holds: the bot tag, the server tag and the plugin chips.
constexpr u32 kBadges = 12;
// Text variants as size codes (1rem = the body size).
constexpr u32 kXs = kTextStyleSizeCodeXs, kSm = kTextStyleSizeCodeSm, kMd = kTextStyleSizeCodeMd,
              kLg = text_style_heading_2, kXl = text_style_heading_1;
constexpr float kPopoutWidth = 300, kPopoutBanner = 105, kModalBanner = 140;
// The DM profile panel (the official sidebar redesign): a card inset 8 DIPs
// in its slot with a 2 DIP border (a Nitro theme's gradient), a 105 DIP banner,
// the 80 DIP avatar 16 and 57 from the card's corner, a 44 DIP "View Full
// Profile" bar.
constexpr float kPanelInset = 8, kPanelBorder = 2, kPanelBanner = 105, kPanelAvatarTop = 57 - kPanelBorder,
                kPanelFooter = 44;
constexpr u16 kNoPart = 0xFFFF;
// A card's changing parts (indices into parts): the timer chip's label and
// glyph, the progress bar's ends and track.
struct ActivityTimer {
    u16 clock, glyph, elapsed, total, bar;
    u8 item, card;
};
struct Storage {
    UiWidget widgets[kWidgets];
    UiLayoutNode nodes[kWidgets];
    ProfilePart parts[capacity];
    // Owned labels; other parts borrow profile text.
    ProfileString<64> mutual_text[2], tab_text[2], roles_more, member_since[2];
    // The username line: a bot's "name#1234".
    ProfileString<136> username_tag;
    // The panel's command chips: "/name".
    ProfileString<132> command_label[kProfileCommandMaximum];
    ProfileString<64> connection_line[16];
    char connection_initial[16][2];
    // The laid-out activities and their cards' text (the store changes under
    // them): per card the timer chip and the progress bar's ends.
    UserActivitySet activity_set;
    ProfileActivityContent activity_content[kUserActivityMaximum];
    ProfileString<16> activity_time[kUserActivityMaximum][3];
    ActivityTimer timers[kUserActivityMaximum];
    u32 timer_count;
    // A tick's time while it lays the cards out again (0: the clock).
    u64 tick_ms;
    // Set when a part cannot be added; the layout reports it once, at its end.
    bool failed;
    // The chips of parts that draw one (ProfilePart::badge indexes it).
    UiBadgeWidget badges[kBadges];
    u32 badge_count;
    // The bio parser's workspace.
    struct {
        u32 words[1024];
        TextStyleSpan spans[128];
    } bio_work;
};
static_assert(sizeof(Storage::connection_line) / sizeof(ProfileString<64>) ==
              sizeof(UserProfile::connections) / sizeof(ProfileConnection));
// Rich presence cards: a header line, then the artwork (the small image on its
// corner) beside the title, text lines, chips (party, timer, episode) and the
// progress bar. Buttons follow the card's body in the popout and the text in
// the full profile, whose cards use the larger metrics.
struct CardMetrics {
    float pad, image, small, gap, ring;
};

Storage& labels(ProfileScene* s) noexcept;
const Storage& labels(const ProfileScene& s) noexcept;
bool interactive(const ProfileScene& s) noexcept;
bool modal(const ProfileScene& s) noexcept;
bool panel(const ProfileScene& s) noexcept;
UiSize measure(ProfileScene* s, const Utf8View& text, float width, bool wrap, u32 flags = 0,
               UiTextFace face = UiTextFace::normal) noexcept;
float text_width(ProfileScene* s, const Utf8View& text, float limit, u32 flags,
                 UiTextFace face = UiTextFace::normal) noexcept;
ProfilePart* add(ProfileScene* s, UiWidgetHandle parent, const UiRect& r, const Utf8View& label = {},
                 ProfileAction action = ProfileAction::none, u32 flags = 0,
                 ProfileLook look = ProfileLook::plain, ProfileTone tone = ProfileTone::normal,
                 bool clip = false) noexcept;
ProfilePart* blank(ProfileScene* s, UiWidgetHandle parent, const UiRect& r,
                   ProfileAction action = ProfileAction::none, ProfileLook look = ProfileLook::plain) noexcept;
ProfilePart* icon(ProfileScene* s, UiWidgetHandle parent, const UiRect& r, UiIcon glyph, ProfileAction action,
                  ProfileTone tone = ProfileTone::subtle, ProfileLook look = ProfileLook::icon) noexcept;
ProfilePart* caption(ProfileScene* s, UiWidgetHandle parent, const UiRect& r, const Utf8View& label, u32 flags,
                     ProfileTone tone = ProfileTone::normal) noexcept;
void place(ProfileScene* s, UiWidgetHandle handle, const UiRect& r) noexcept;
void mutual_count(ProfileString<64>* text, u32 n, bool servers) noexcept;
const UserAppearanceRecord* appearance(const ProfileScene& s, u32 field) noexcept;
bool bot(const ProfileScene& s) noexcept;
bool self(const ProfileScene& s) noexcept;
u64 activity_now(ProfileScene& s) noexcept;
CardMetrics card_metrics(bool large) noexcept;
void shift(ProfileScene* s, u32 first, float dy) noexcept;
float activity_card(ProfileScene* s, UiWidgetHandle owner, float left, float top, float right, u32 card, u32 item,
                    u64 now_ms, ProfileAction action) noexcept;
u32 activity_cards(ProfileScene* s, u8* order) noexcept;
float status_bubble(ProfileScene* s, UiWidgetHandle owner, float left, float top, float max_width) noexcept;
bool roles_shown(const ProfileScene& s) noexcept;
float roles(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width, bool clamp) noexcept;
float plugin_section(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width) noexcept;
float name_block(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width, bool large) noexcept;
float button(ProfileScene* s, UiWidgetHandle owner, float left, float top, const Utf8View& label, UiIcon glyph,
             ProfileAction action, bool primary, float width = 0) noexcept;
float notices(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width, bool large) noexcept;
Utf8View peer_name(const ProfileScene& s, bool friends, const ProfilePeer& peer) noexcept;
void peer_row(ProfileScene* s, UiWidgetHandle owner, bool friends, u32 index, const UiRect& row_rect,
              const UiRect& face_rect) noexcept;
float mutuals(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width) noexcept;
float biography(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width, bool clamp,
                bool view_bio = true) noexcept;
void header_geometry(ProfileScene* s, const UiRect& banner, float avatar_left, float avatar_top, float size) noexcept;
void header_parts(ProfileScene* s, ProfileAction avatar_action) noexcept;
void resolve_theme(ProfileScene* s) noexcept;
UiWidgetHandle text_field(ProfileScene* s, const ProfilePart* part) noexcept;
bool can_message(const ProfileScene& s) noexcept;
bool finish(ProfileScene* s, const UiRect& viewport) noexcept;
void header_to_screen(ProfileScene* s, float dx, float dy) noexcept;
void header_buttons(ProfileScene* s, float bx, float by) noexcept;
bool layout_popout(ProfileScene* s, const UiRect& viewport) noexcept;
bool layout_modal(ProfileScene* s, const UiRect& viewport) noexcept;
float commands_section(ProfileScene* s, UiWidgetHandle owner, float left, float top, float width) noexcept;
bool layout_panel(ProfileScene* s, const UiRect& viewport) noexcept;
template <u32 N> bool append(ProfileString<N>* s, const Utf8View& text) noexcept {
    const u32 bytes = utf8_truncated_length(text, N - s->bytes);
    copy_bytes(s->data + s->bytes, text.data, bytes);
    s->bytes += static_cast<u16>(bytes);
    return bytes == text.bytes;
}

} // namespace maicord::profile_scene_detail
