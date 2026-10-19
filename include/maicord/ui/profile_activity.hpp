#pragma once
#include "maicord/model/user_activity.hpp"
#include "maicord/model/user_profile.hpp"

namespace maicord {
// Rich presence cards of the profile popout and the full profile: their order,
// text, timer chip, progress bar and actions, by the official client's card
// rules. Custom status is not a card (the status bubble shows it).

// Card order: competing, streaming, playing, then the other types; rich
// presence before plain activities, then the newer first. At most one playing
// activity survives, and one card per application ID or name. Writes item
// indices into order; returns the card count.
u32 profile_activity_order(const UserActivitySet& set, Slice<u8> order) noexcept;

// Timer chips: "m:ss", or "h:mm:ss" from an hour.
u32 profile_activity_clock_text(u64 seconds, Slice<char> output) noexcept;
// Progress bar ends: "mm:ss", or "hh:mm:ss" from an hour.
u32 profile_activity_track_text(u64 seconds, Slice<char> output) noexcept;

enum class ProfileActivityTimer : u8 {
    none,
    elapsed,
    countdown
};
struct ProfileActivityClock {
    ProfileActivityTimer timer;
    u64 seconds;
};
// The timer chip at now. An end later than the activity's creation counts
// down (not while listening) until it passes; otherwise the time since the
// start (or creation) shows, stopped at the end. None when a progress bar
// shows or nothing gives a start.
ProfileActivityClock profile_activity_clock(const UserActivity&, u64 unix_now_ms) noexcept;

// Listening and watching activities with both timestamps show a progress bar:
// whole seconds elapsed (clamped to the duration) and the fraction played.
struct ProfileActivityProgress {
    u64 elapsed, duration;
    float fraction;
};
bool profile_activity_progress(const UserActivity&, u64 unix_now_ms, ProfileActivityProgress* output) noexcept;

enum class ProfileActivityGlyph : u8 {
    game,
    music,
    screen,
    timer,
    party,
    episode,
    stream,
    competition
};
// The artwork slot's glyph while (or when) no image shows.
ProfileActivityGlyph profile_activity_placeholder(UserActivityType) noexcept;
enum class ProfileActivityAction : u8 {
    none,
    // The activity's own button labels; their URLs come from its metadata.
    custom,
    // A Twitch or YouTube stream's page.
    watch,
    // "Ask to Join": the game must run here, which this client cannot check,
    // so the button shows disabled with the official "<name> is not detected".
    join
};
struct ProfileActivityContent {
    ProfileString<96> heading;
    // The heading-sized first line, then up to two text lines.
    ProfileString<128> title;
    ProfileString<192> lines[2];
    u8 line_count;
    // Chips under the lines: the party ("state (2 of 4)") and a watched
    // episode ("S1E2"); empty when absent. The timer chip comes from
    // profile_activity_clock.
    ProfileString<192> party;
    ProfileString<16> episode;
    bool chips, progress, spotify, artwork;
    ProfileActivityGlyph clock_glyph;
    ProfileActivityAction action;
    // The join button's tooltip.
    ProfileString<160> hint;
};
// bot: application accounts show no artwork or chips. self: no actions.
bool profile_activity_content(const UserActivity&, bool bot, bool self, ProfileActivityContent* output) noexcept;

// The party chip: "state (count of max)", "state (count)", the count alone or
// the state alone; 0 when neither is shown.
u32 profile_activity_party_text(Utf8View state, u32 size, u32 max, Slice<char> output) noexcept;
// "S<season>E<episode>" from large text like "Season 2, Episode 5"; 0 otherwise.
u32 profile_activity_episode_text(Utf8View large_text, Slice<char> output) noexcept;
} // namespace maicord
