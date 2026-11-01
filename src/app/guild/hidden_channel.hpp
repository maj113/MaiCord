#pragma once
#include "maicord/ui/core/geometry.hpp"

namespace maicord::app_detail {
// "Show hidden channels": a listed channel the account cannot view only explains
// itself. Selecting or right-clicking its row opens a read-only popup with the
// channel's age and the roles and members that can see it, computed from the
// cached permission data. Nothing is requested: no history, subscription, typing,
// read state or voice join ever concerns such a channel.

// The channel is a guild channel the account cannot view (permissions resolved).
bool application_channel_hidden(u64 channel_id) noexcept;
// Opens the explanation popup next to anchor. False when the channel is not hidden.
bool application_hidden_channel_open(u64 channel_id, UiRect anchor) noexcept;
} // namespace maicord::app_detail
