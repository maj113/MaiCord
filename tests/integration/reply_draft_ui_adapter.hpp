#pragma once
#include "maicord/app/message_composer_controller.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/model/user_store.hpp"
#include "maicord/ui/message_composer_scene.hpp"
#include "maicord/ui/popup_scene.hpp"
#include <windows.h>

namespace maicord {
inline constexpr u64 kReplyDraftProbeGuild = 97000;
inline constexpr u64 kReplyDraftProbeChannelA = 97100;
inline constexpr u64 kReplyDraftProbeChannelB = 97101;
inline constexpr u64 kReplyDraftProbeSelf = 97005;
inline constexpr u64 kReplyDraftProbeAuthor = 97006;
inline constexpr u64 kReplyDraftProbeMessage = 97200;
inline constexpr u64 kReplyDraftProbeOwnMessage = 97201;

// Only native presentation is projected separately. App reply authority,
// draft/session persistence and mouse/keyboard actions use production code.
bool reply_draft_ui_adapter_begin(HWND, u32 dpi, bool (*refresh)(void*) noexcept, void*) noexcept;
void reply_draft_ui_adapter_end() noexcept;
bool reply_draft_ui_adapter_active() noexcept;
ChatStateStore* reply_draft_ui_adapter_state() noexcept;
UserStore* reply_draft_ui_adapter_users() noexcept;
MessageComposer* reply_draft_ui_adapter_composer() noexcept;
MessageComposerScene* reply_draft_ui_adapter_scene() noexcept;
MessageComposerController* reply_draft_ui_adapter_controller() noexcept;
MessageTimeline* reply_draft_ui_adapter_timeline() noexcept;
PopupScene* reply_draft_ui_adapter_popup() noexcept;
MessageComposerReplySnapshot reply_draft_ui_adapter_reply() noexcept;
Utf8View reply_draft_ui_adapter_label(Slice<char>) noexcept;
bool reply_draft_ui_adapter_editing() noexcept;
bool reply_draft_ui_adapter_begin_reply(u64) noexcept;
bool reply_draft_ui_adapter_select(u64) noexcept;
bool reply_draft_ui_adapter_text(Utf8View) noexcept;
bool reply_draft_ui_adapter_restart() noexcept;
bool reply_draft_ui_adapter_profile(bool second) noexcept;
bool reply_draft_ui_adapter_evict_source() noexcept;
bool reply_draft_ui_adapter_author(Utf8View) noexcept;
bool reply_draft_ui_adapter_delete_rest(u64) noexcept;
bool reply_draft_ui_adapter_gateway(Utf8View full_envelope) noexcept;
bool reply_draft_ui_adapter_edit(bool begin) noexcept;
bool reply_draft_ui_adapter_mouse(MessageComposerControl, u32 dpi) noexcept;
bool reply_draft_ui_adapter_key(MessageComposerControl, u32 key) noexcept;
bool reply_draft_ui_adapter_refresh() noexcept;
bool reply_draft_ui_adapter_failed_menu(UiRect anchor, UiRect viewport) noexcept;
bool reply_draft_ui_adapter_deleted_menu() noexcept;
void reply_draft_ui_adapter_close_menu() noexcept;
} // namespace maicord
