#pragma once
#include "maicord/app/read_scope.hpp"
#include "maicord/model/chat_state.hpp"
#include "maicord/ui/popup_scene.hpp"

namespace maicord {
// Includes visible categories/voice channels with plugin-only menus. Native
// action availability is separate from context-target eligibility.
bool channel_context_target_valid(const ChatStateStore&, u64 channel_id) noexcept;
bool channel_context_target_current(const ChatStateStore&, u64 channel_id, u16 generation,
                                    u32 revision) noexcept;
// Returns Mark as Read, optional unread/retry actions, Close DM for type 1,
// then optional Edit Channel for resolved manageable guild text channels.
// Supply retry_unread_action only for a scope-owned failed unread intent.
// Zero optional action IDs preserve ordinary callers.
// Unknown/hidden channels and insufficient storage leave output unchanged.
// Action IDs are supplied by the caller; this policy owns no application state.
u32 channel_context_policy_build(const ChatStateStore& state, u64 channel_id, u32 mark_read_action,
                                 u32 close_dm_action, Slice<PopupItem> output, u32 edit_action = 0,
                                 u32 mark_unread_action = 0, u32 retry_unread_action = 0,
                                 UiScopedReadMenuState scoped = {}) noexcept;
} // namespace maicord
