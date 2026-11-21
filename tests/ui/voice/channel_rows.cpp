#include "maicord/ui/voice/channel_rows.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/ui/chat/messages/mention_badge.hpp"
#include "maicord/ui/shell/navigation_scene.hpp"

// The "Invite to Voice" countdown and the geometry of a voice channel row.
namespace maicord {
namespace {
using Check = void (*)(bool, const char*) noexcept;

void invite_cases(Check check) noexcept {
    UiVoiceInviteCountdown invite{};
    check(!ui_voice_invite_observe(&invite, 0) && !ui_voice_invite_channel(invite),
          "not connected, no row");
    check(ui_voice_invite_observe(&invite, 7) && ui_voice_invite_channel(invite) == 7 &&
              !ui_voice_invite_next_wake(invite),
          "a new connection shows the row without a countdown until it is on screen");
    check(!ui_voice_invite_tick(&invite, 1'000, true) && ui_voice_invite_next_wake(invite) == 11'000,
          "on screen the row counts down 10 s");
    check(!ui_voice_invite_tick(&invite, 6'000, false) && !ui_voice_invite_next_wake(invite) &&
              !ui_voice_invite_tick(&invite, 7'000, true) && ui_voice_invite_next_wake(invite) == 17'000,
          "hovering or leaving the screen restarts the full 10 s");
    check(!ui_voice_invite_tick(&invite, 16'999, true) && ui_voice_invite_tick(&invite, 17'000, true) &&
              !ui_voice_invite_channel(invite) && !ui_voice_invite_next_wake(invite),
          "the countdown dismisses the row");
    check(!ui_voice_invite_observe(&invite, 7) && !ui_voice_invite_channel(invite),
          "staying connected keeps it dismissed");
    check(ui_voice_invite_observe(&invite, 0) == false && ui_voice_invite_observe(&invite, 7) &&
              ui_voice_invite_channel(invite) == 7,
          "a new connection to the channel shows it again");
    check(ui_voice_invite_dismiss(&invite) && !ui_voice_invite_channel(invite) &&
              !ui_voice_invite_dismiss(&invite),
          "the close button dismisses it once");
    check(ui_voice_invite_observe(&invite, 8) && ui_voice_invite_channel(invite) == 8,
          "moving to another channel shows it for that channel");
    check(ui_voice_invite_observe(&invite, 0) && !ui_voice_invite_channel(invite),
          "leaving voice hides it");
}

void layout_cases(Check check) noexcept {
    ChannelViewRow row{};
    row.id = 7;
    row.type = 2;
    row.voice_count = 2;
    const float plain = shell_navigation_row_height(row);
    row.voice_subtitle = true;
    row.voice_invite = true;
    check(shell_navigation_row_height(row) ==
              plain + kShellNavigationStageTopicHeight + kShellNavigationVoiceInviteHeight,
          "a status line and the invite row add their heights");
    const UiRect box{0, 100, 240, 100 + shell_navigation_row_height(row)};
    UiVoiceInviteRow invite{};
    const UiRect status = ui_voice_status_rectangle(row, box);
    check(ui_voice_invite_row(row, box, &invite) &&
              status.top >= box.top + shell_navigation_head_height() - 4 &&
              invite.row.top >= status.bottom + 2 * kShellNavigationVoiceUserHeight - 2 &&
              invite.row.bottom <= box.bottom && invite.close.right <= box.right &&
              invite.label.left > invite.icon.right,
          "the invite row follows the users inside the channel row");
    row.voice_invite = false;
    check(!ui_voice_invite_row(row, box, &invite), "no invite row without the flag");
}

bool overlaps(const UiRect& a, const UiRect& b) noexcept {
    return ui_rect_has_area(ui_rect_intersection(a, b));
}
bool contains(const UiRect& outer, const UiRect& inner) noexcept {
    return inner.left >= outer.left && inner.top >= outer.top && inner.right <= outer.right &&
           inner.bottom <= outer.bottom;
}

// The connected channel's head line: Create Invite and Edit Channel, then a mention badge, each in its own
// space inside the row, and the name ending before the first of them. Nothing else is drawn at its right end.
void trailing_cases(Check check) noexcept {
    ChannelViewRow row{};
    row.id = 7;
    row.type = 2;
    row.voice_count = 1;
    row.voice_subtitle = true;
    row.controls = channel_row_invite | channel_row_settings;
    const UiRect box{0, 100, 240, 100 + shell_navigation_row_height(row)};
    const UiRect head{box.left, box.top, box.right, box.top + shell_navigation_head_height()};
    const u32 counts[]{0, 3, 42, 700};
    bool inside = true, disjoint = true, named = true, shown = true;
    for (const u32 count : counts) {
        row.mention_count = count;
        const UiRect invite = shell_navigation_control_rectangle(box, row, channel_row_invite);
        const UiRect settings = shell_navigation_control_rectangle(box, row, channel_row_settings);
        const float badge_right = box.right - kShellChannelHighlightRight - kShellChannelHighlightLeft;
        const UiRect badge = count ? UiRect{badge_right - ui_mention_badge_width(count), head.top, badge_right, head.bottom}
                                   : UiRect{};
        shown = shown && ui_rect_has_area(invite) && ui_rect_has_area(settings) &&
                shell_navigation_row_controls(row, true, false) == row.controls;
        inside = inside && contains(head, invite) && contains(head, settings) && (!count || contains(head, badge));
        disjoint = disjoint && !overlaps(invite, settings) && !overlaps(invite, badge) && !overlaps(settings, badge);
        named = named && shell_navigation_controls_left(box, row, row.controls, box.right) <= invite.left;
    }
    check(shown, "a hovered connected channel shows Create Invite and Edit Channel");
    check(inside && disjoint, "the controls and the mention badge are disjoint and inside the channel row");
    check(named, "the channel name ends before the leftmost control");
}

// The user limit pill (the official "00 | 02") and the full channel rule.
void limit_cases(Check check) noexcept {
    const auto reads = [](u32 value, const char* expected) noexcept {
        char text[4];
        const u32 bytes = shell_navigation_limit_digits(value, text);
        return bytes == c_string_length(expected) && bytes_equal(text, expected, bytes);
    };
    check(reads(0, "00") && reads(2, "02") && reads(99, "99") && reads(150, "150") && reads(9'999, "9999"),
          "a pill count has at least two digits");
    ChannelViewRow row{};
    row.id = 7;
    row.type = 2;
    row.user_limit = 2;
    row.actions = channel_action_connect;
    const UiRect box{0, 100, 240, 100 + shell_navigation_row_height(row)};
    const float badge_right = box.right - kShellChannelHighlightRight - kShellChannelHighlightLeft;
    ShellLimitPill pill{};
    // Inside the 1-DIP border: a 28-DIP users part, then a 20-DIP limit part.
    check(shell_navigation_limit_pill(box, row, false, false, &pill) && pill.box.right == badge_right &&
              ui_rect_width(pill.box) == 50 && ui_rect_height(pill.box) == 20 && pill.split - pill.box.left == 29 &&
              pill.box.top - box.top == box.top + shell_navigation_head_height() - pill.box.bottom,
          "an empty channel with a limit of 2 shows a 50 x 20 pill where a mention badge ends, centred on the head");
    check(!shell_navigation_limit_pill(box, row, true, false, &pill) &&
              !shell_navigation_limit_pill(box, row, false, true, &pill),
          "a hovered or selected row shows its controls instead");
    row.mention_count = 1;
    check(!shell_navigation_limit_pill(box, row, false, false, &pill), "a mention badge takes its place");
    row.mention_count = 0;
    row.actions = 0;
    check(!shell_navigation_limit_pill(box, row, false, false, &pill), "no pill without Connect");
    row.actions = channel_action_connect;
    row.user_limit = 0;
    check(!shell_navigation_limit_pill(box, row, false, false, &pill), "no pill without a limit");
    row.type = 13;
    row.user_limit = 10'000;
    check(!shell_navigation_limit_pill(box, row, false, false, &pill), "a stage's maximum is not a limit");
    row.user_limit = 500;
    row.voice_count = 99;
    check(shell_navigation_limit_pill(box, row, false, false, &pill) && pill.box.right - 1 - pill.split == 27 &&
              pill.split - pill.box.left == 29 && pill.box.right == badge_right,
          "a stage limit of 500 widens only its own part");

    ChannelRecord channel{};
    channel.type = 2;
    channel.user_limit = 2;
    channel.actions = channel_action_connect;
    check(!channel_voice_full(channel, 1) && channel_voice_full(channel, 2) && channel_voice_full(channel, 3),
          "a channel is full when its users reach the limit");
    channel.actions |= channel_action_move_members;
    check(!channel_voice_full(channel, 2), "Move Members joins a full channel");
    channel.actions = channel_action_connect;
    channel.user_limit = 0;
    check(!channel_voice_full(channel, 50), "a channel without a limit is never full");
}

void duration_cases(Check check) noexcept {
    const auto says = [](u64 seconds, const char* expected) noexcept {
        char text[kUiVoiceDurationBytes];
        const u32 bytes = ui_voice_duration_text(seconds, text, sizeof(text));
        return bytes == c_string_length(expected) && bytes_equal(text, expected, bytes);
    };
    check(says(0, "0 seconds") && says(1, "1 second") && says(59, "59 seconds"), "a short call reads in seconds");
    check(says(60, "1 minute") && says(125, "2 minutes, 5 seconds") && says(3'599, "59 minutes, 59 seconds"),
          "minutes lose a zero seconds part");
    check(says(3'600, "1 hour") && says(3'723, "1 hour, 2 minutes, 3 seconds") && says(7'200 + 5, "2 hours, 5 seconds"),
          "hours read singular and plural, skipping zero parts");
    char small[8];
    check(ui_voice_duration_text(5, small, sizeof(small)) == 0, "a short buffer writes nothing");
}
} // namespace

void test_voice_channel_rows(Check check) noexcept {
    invite_cases(check);
    layout_cases(check);
    trailing_cases(check);
    limit_cases(check);
    duration_cases(check);
}
} // namespace maicord
