#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/app/gateway_client.hpp"
#include "maicord/protocol/soundboard.hpp"
#include "maicord/ui/geometry.hpp"

namespace maicord::app_detail {
// Soundboard on the client: the sound catalog (default and server sounds,
// kept current by Gateway events), playing received sounds through the
// account module, the call bar's picker, effect labels on call tiles and the
// soundboard REST queue (DETAILS lane, one request at a time).

// Gateway mailbox observer: READY, guild deletes and the soundboard dispatches.
void application_soundboard_gateway(const GatewayClientEvent& event, ConstSlice<u8> payload) noexcept;

// The current call (channel_id) offers the soundboard (guild voice channels).
bool application_soundboard_available(u64 channel_id) noexcept;
bool application_soundboard_picker_open() noexcept;
// Opens (or closes, when open) the picker above the call bar button.
bool application_soundboard_toggle(u64 channel_id, UiRect anchor) noexcept;
// Picker popup: actions, Shift-click previews; type-to-search keys.
bool application_soundboard_menu_action(u32 action) noexcept;
bool application_soundboard_menu_activate(u32 action, bool preview) noexcept;
bool application_soundboard_menu_key(u32 key) noexcept;

// Label ("emoji name") for a user who just played a sound in the current
// call; empty when none. Borrowed until that user's label changes or expires.
Utf8View application_soundboard_effect(u64 user_id) noexcept;

// Settings → Soundboard page (Manage Expressions).
struct SoundboardGuildView {
    const SoundboardSound* sounds;
    u32 count;
    bool loaded, loading, busy;
    Utf8View notice; // Last failure for this server; empty for none.
};
SoundboardGuildView application_soundboard_guild(u64 guild_id) noexcept;
// Changes whenever the catalog, a queued edit or a notice changes.
u32 application_soundboard_revision() noexcept;
// Loads the server's sounds once (again after a failure).
void application_soundboard_request_guild(u64 guild_id) noexcept;
const SoundboardSound* application_soundboard_find(u64 sound_id) noexcept;
// Checks a picked file in the account module (format, size, duration).
bool application_soundboard_probe(ConstSlice<u8> file, SoundboardFileInfo* info, Utf8View* error) noexcept;
// Queues an upload; the bytes are copied. False when the queue is full.
bool application_soundboard_create(u64 guild_id, Utf8View name, Utf8View emoji, u16 volume,
                                   ConstSlice<u8> file) noexcept;
bool application_soundboard_update(u64 guild_id, u64 sound_id, Utf8View name, Utf8View emoji, u64 emoji_id,
                                   u16 volume) noexcept;
bool application_soundboard_remove(u64 guild_id, u64 sound_id) noexcept;
bool application_soundboard_preview(u64 sound_id, u16 volume) noexcept;

// DETAILS lane.
bool application_soundboard_pending(u64 now_ms) noexcept;
bool application_soundboard_submit(u64 request_id, u64 now_ms) noexcept;
bool application_soundboard_response(const RestClientResponse& response, Utf8View body) noexcept;
// lane_idle: no DETAILS request is in flight. Also expires effect labels.
u64 application_soundboard_next_wake(bool lane_idle) noexcept;
void application_soundboard_release() noexcept;
} // namespace maicord::app_detail
