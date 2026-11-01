#pragma once

#include "maicord/core/text_bank.hpp"
#include "maicord/model/chat/chat_state.hpp"
#include "maicord/model/user/store.hpp"
#include "maicord/ui/shell/popup_scene.hpp"

namespace maicord {

inline constexpr u32 kHiddenChannelMenuRows = 3 + 1 + 5 + 3 + 1;
inline constexpr u32 kHiddenChannelMenuTextBytes = 1024;

// "Show hidden channels": the read-only rows of the popup that explains a locked
// channel. They hold its name, when it was created and last had a message, and
// who can see it, worked out from the cached roles and overwrites alone: the
// roles (highest first, bots' managed roles left out) that still resolve View
// Channel there and the members an overwrite names. Row text lives in `bank`
// (kHiddenChannelMenuTextBytes suffice) and names in the stores; both must
// outlive the popup. Returns the row count, zero when the channel is not hidden
// or `output` holds fewer than kHiddenChannelMenuRows.
u32 hidden_channel_menu_build(const ChatStateStore& state, const UserStore& users, const ChannelRecord& channel,
                              u64 now_unix_seconds, TextBank* bank, Slice<PopupItem> output) noexcept;

} // namespace maicord
