#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/model/reaction_preferences.hpp"

namespace maicord::app_detail {
inline constexpr u32 kApplicationEmojiUseCapacity = 16;
struct ApplicationEmojiUse {
    ReactionEmojiRef emoji[kApplicationEmojiUseCapacity];
    u32 count;
};
// Settings sync for the account's ReactionPreferenceStore (favorites plus
// reaction and composer frecency in settings-proto/2).
bool application_emoji_preferences_toggle_favorite(ReactionEmojiRef) noexcept;
void application_emoji_preferences_track_reaction(ReactionEmojiRef) noexcept;
// Unique custom and Unicode emoji in outgoing content, scanned before the
// composer is cleared and recorded once the send was queued.
ApplicationEmojiUse application_emoji_preferences_scan(Utf8View content) noexcept;
void application_emoji_preferences_track_message(const ApplicationEmojiUse&) noexcept;
// Picker open: refresh a stale snapshot.
void application_emoji_preferences_open() noexcept;
void application_emoji_preferences_synchronize() noexcept;
bool application_emoji_preferences_pending(u64 now_ms) noexcept;
bool application_emoji_preferences_submit(u64 request_id, u64 now_ms) noexcept;
bool application_emoji_preferences_response(const RestClientResponse&, Utf8View body) noexcept;
void application_emoji_preferences_response_drained() noexcept;
Slice<u8> application_emoji_preferences_response_buffer() noexcept;
u64 application_emoji_preferences_next_wake() noexcept;
// Call after pending responses were handled or the account worker joined.
void application_emoji_preferences_release() noexcept;
// Key mapping: custom IDs as decimal, Unicode by its primary shortcode. Only
// entries this client adds use these keys; observed keys are sent back as
// received. Returns 0 bytes when the emoji has no settings key.
u32 application_emoji_preference_key(ReactionEmojiRef, Slice<char> output) noexcept;
ReactionEmojiRef application_emoji_preference_ref(Utf8View key) noexcept;
} // namespace maicord::app_detail
