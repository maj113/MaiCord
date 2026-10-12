#pragma once

#include "maicord/core/base.hpp"
#include "maicord/model/message_composer.hpp"

namespace maicord { enum class MessageComposerControl : u8; }

namespace maicord::app_detail {

enum class PopupOwnerKind : u8;

void set_popup_owner(PopupOwnerKind owner) noexcept;
bool refresh_message_composer_ui() noexcept;
bool refresh_active_autocomplete() noexcept;
bool focus_message_composer_control(MessageComposerControl control) noexcept;
// Replaces the active draft with "/<name> " (a clicked command mention) and
// focuses it through one undoable edit. True unless an internal failure.
bool application_composer_fill_command(Utf8View name) noexcept;
void reset_message_composer_caret_timing(bool edit_active) noexcept;
void application_tick_message_composer_caret(u64 now_ms) noexcept;
// Sends the debounced '@' member search once due (also run by the caret tick).
void application_mention_member_query_tick(u64 now_ms) noexcept;
u64 application_mention_member_query_next_wake() noexcept;
// The search bar's from:/mentions: people suggestions ask the same debounced
// gateway member search (opcode 8 query) as the composer's '@'.
void application_member_query_note(u64 guild_id, u64 channel_id, Utf8View query) noexcept;
bool stop_local_typing(u64 channel_id) noexcept;
bool note_local_typing_input() noexcept;
bool composer_apply_result(MessageComposerResult result, bool content_input = false) noexcept;

} // namespace maicord::app_detail
