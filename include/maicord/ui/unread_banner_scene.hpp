#pragma once

#include "maicord/ui/paint.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/transition.hpp"

namespace maicord {

inline constexpr u32 kUiUnreadBannerLabelBytes = 128;
inline constexpr u32 kUiUnreadBannerMotionMs = 150;

// Host-projected channel facts; the scene owns no read-state model.
struct UiUnreadBannerView {
    u64 channel_id;
    u64 since_unix_ms;     // first unread message time; zero omits "since"
    u32 count;             // unread messages known to be newer than the cursor
    bool count_estimated;  // older unread history is not resident
    bool banner;           // the channel has unread messages
    bool jump;             // the viewport is detached or far from the live edge
};
enum class UiUnreadBannerAction : u8 { none, jump_to_unread, mark_read, jump_to_present, notice_primary, notice_secondary };

// A conversation notice card above the timeline: a DM safety warning or a
// message request. Text is copied; a zero id hides the card. The unread bar
// slides below it while both show.
inline constexpr u32 kUiBannerNoticeTitleBytes = 96;
inline constexpr u32 kUiBannerNoticeTextBytes = 192;
inline constexpr u32 kUiBannerNoticeActionBytes = 32;
struct UiBannerNotice {
    u64 id;
    Utf8View title, description, primary, secondary; // primary may be empty
    bool warning; // Warning tone and shield icon; otherwise neutral with an inbox icon.
    bool busy;    // Actions wait for the request in flight.
};

struct UiUnreadBannerStyle {
    float notice_height = 68, notice_gap = 8, notice_radius = 8, notice_button = 84;
    float banner_height = 32, banner_inset = 16, banner_radius = 8, action_width = 120;
    float pill_height = 40, pill_width = 380, pill_bottom = 12, pill_radius = 16, pill_action = 136, icon = 16;
};
struct UiUnreadBannerScene {
    UiWidgetReference text_owner;
    UiUnreadBannerStyle style;
    UiUnreadBannerView view;
    UiRect viewport, banner, banner_label, banner_action, pill;
    UiMotionTransition banner_motion, pill_motion;
    char label[kUiUnreadBannerLabelBytes];
    u32 revision;
    u16 label_bytes;
    UiUnreadBannerAction hovered;
    // Notice card (ui_unread_banner_set_notice).
    UiRect notice, notice_icon, notice_title_rect, notice_text_rect, notice_primary_rect, notice_secondary_rect;
    UiMotionTransition notice_motion;
    u64 notice_id;
    char notice_title[kUiBannerNoticeTitleBytes], notice_text[kUiBannerNoticeTextBytes];
    char notice_primary[kUiBannerNoticeActionBytes], notice_secondary[kUiBannerNoticeActionBytes];
    u8 notice_title_bytes, notice_text_bytes, notice_primary_bytes, notice_secondary_bytes;
    bool notice_warning, notice_busy;
};

// "12 new messages since 3:04 PM"; estimated counts gain a "+" suffix.
// since is preformatted ("3:04 PM" or "3:04 PM on October 5, 2026").
// Writes nothing and returns zero when output is too small.
u32 ui_unread_banner_format_label(u32 count, bool estimated, Utf8View since, Slice<char> output) noexcept;
// Copies the view and label, ticks both 150 ms slide/fade transitions and
// projects geometry against the message viewport. Returns paint change.
bool ui_unread_banner_update(UiUnreadBannerScene*, const UiUnreadBannerView&, Utf8View label, UiRect viewport,
                             UiWidgetReference text_owner, u64 now_ms, bool animate) noexcept;
// Replaces the notice card (null or a zero id hides it) before the next
// update; returns true when its content changed. Text is truncated to fit.
bool ui_unread_banner_set_notice(UiUnreadBannerScene*, const UiBannerNotice*) noexcept;
u64 ui_unread_banner_next_wake(const UiUnreadBannerScene&, u64 now_ms, bool animate) noexcept;
bool ui_unread_banner_visible(const UiUnreadBannerScene&) noexcept;
// Painted surfaces that sit above timeline content this frame, clipped to the
// viewport: [0] the unread banner (top), [1] the jump pill (bottom). Hidden
// surfaces are empty. Hover toolbars clip themselves against these.
void ui_unread_banner_occluders(const UiUnreadBannerScene&, UiRect (&occluders)[2]) noexcept;
// Only settled-in, visible surfaces accept pointer actions.
UiUnreadBannerAction ui_unread_banner_hit_test(const UiUnreadBannerScene&, float x, float y) noexcept;
bool ui_unread_banner_set_hover(UiUnreadBannerScene*, UiUnreadBannerAction) noexcept;
bool ui_unread_banner_paint(const UiUnreadBannerScene&, const Theme&, PaintStream*) noexcept;
bool ui_unread_banner_text_output(const UiUnreadBannerScene&, const Theme&, UiTextOutput*) noexcept;
void ui_unread_banner_clear(UiUnreadBannerScene*) noexcept;

} // namespace maicord
