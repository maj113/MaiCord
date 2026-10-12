#pragma once

#include "maicord/core/slice.hpp"
#include "maicord/core/utf.hpp"

namespace maicord { struct AccountMediaFrame; }

namespace maicord::app_detail {

struct Application;

// A successful local GPU upload retains its decoder until an actual Present.
void application_media_poster_uploaded(Application&, const AccountMediaFrame&) noexcept;
void application_media_posters_presented(Application&, bool succeeded, u64 before, u64 after) noexcept;

struct ApplicationMediaPumpResult {
    bool repaint;
    bool text_changed;
    bool history_requested;
    bool ok;
};

ApplicationMediaPumpResult application_pump_visible_media(Application* application) noexcept;
// Forum previews remain live over a browser whose chat/read projections are blocked.
ApplicationMediaPumpResult application_pump_forum_preview_media(Application* application) noexcept;
// The thread browser covers only the chat column. Guild rail, channel list,
// footer and voice avatars stay on screen, so their animations keep playing:
// a visibility-only pass (chat counts as hidden and pauses with its remaining
// frame time) plus due animation advances and their codec completions. No new
// asset admission, chat text, geometry, history or read work.
ApplicationMediaPumpResult application_pump_covered_media(Application* application) noexcept;
// Desktop toast avatar: the author's atlas-resident avatar written once per
// user and avatar hash under %LOCALAPPDATA%/MaiCord/notification-avatars.
// Returns its file:/// URI in storage, or empty when the avatar is not resident.
Utf8View application_notification_avatar_uri(u64 user_id, Utf8View avatar_hash, Slice<char> storage) noexcept;

} // namespace maicord::app_detail
