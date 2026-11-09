#pragma once
#include "maicord/app/net/rest_client.hpp"
#include "maicord/model/chat/message_store.hpp"
#include "maicord/ui/chat/messages/context_menu.hpp"
#include "maicord/ui/shell/popup_scene.hpp"

namespace maicord::app_detail {
// The shared user context menu (member list, message authors, DM list, voice
// rows, profile cards) with its Invite to server, Roles and Apps follow-ups;
// message menu extras (Apps, Remove embeds, Speak, Report); user notes; and
// the requests behind them on the DETAILS lane (hosted by channel overview).
enum class UserMenuSource : u8 { member_list, message, private_channel, voice, profile };
// guild_id is the menu's server (zero: the selected one). Extra rows (a voice
// row's Mute, for instance) come first; their actions stay with the caller.
bool application_user_menu_open(u64 user_id, UiRect anchor, u64 guild_id, UserMenuSource source,
                                ConstSlice<PopupItem> leading = {}, const UiPopupFeedback* feedback = nullptr) noexcept;
// The other person's rows for a 1:1 DM row's conversation menu.
u32 application_user_actions_private_channel_items(u64 channel_id, Slice<PopupItem> output) noexcept;
bool application_user_menu_activate(u32 action) noexcept;

MessageContextMenuExtras application_user_actions_message_extras(const MessageRecord&) noexcept;
// Apps, Remove embeds, Speak message and Report message; false when the
// action is not one of them.
bool application_user_actions_message_activate(u64 message_id, u32 action) noexcept;
// The command index for the active channel changed: an open Apps list refreshes.
void application_user_actions_commands_changed() noexcept;
// Says `text` aloud at Settings > Accessibility's Text-to-Speech rate (Speak message and the rate's Preview);
// false when no speech engine is available.
bool application_user_actions_speak(Utf8View text) noexcept;

// Notes: the text a profile shows (draft while typing), loading on demand.
Utf8View application_user_note(u64 user_id, bool* loading) noexcept;
bool application_user_note_edit(u64 user_id, Utf8View text) noexcept;
void application_user_note_flush(u64 user_id) noexcept;
// The note's last save was refused (its retries ran out); the next edit tries again.
bool application_user_note_failed(u64 user_id) noexcept;
// USER_NOTE_UPDATE data (verbatim JSON).
void application_user_actions_note_gateway(Utf8View data) noexcept;

u64 application_user_actions_next_wake() noexcept;
bool application_user_actions_submit(u64 now) noexcept;
bool application_user_actions_owns_response(u64 request_id) noexcept;
bool application_user_actions_response(const RestClientResponse&, Utf8View body) noexcept;
void application_user_actions_release() noexcept;
} // namespace maicord::app_detail
