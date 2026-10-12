#pragma once

#include "maicord/model/message_store.hpp"
#include "maicord/ui/unread_banner_scene.hpp"
#include "maicord/ui/virtual_list.hpp"

namespace maicord {

// Distance from the live edge, in viewports, before the jump pill appears.
inline constexpr u32 kUnreadBannerJumpViewports = 3;

// Borrowed, synchronously consumed facts about the selected channel. The
// timeline list index equals the store's ordered index.
struct UnreadBannerInput {
    const MessageStore* store;
    const UiVariableList* list;
    u64 channel_id, last_read_message_id, last_message_id;
    u64 scroll_units, viewport_units;
    bool read_state_known, readable;
    bool newer_detached;   // resident history does not reach the live edge
    bool older_exhausted;  // resident history reaches the channel start
};
struct UnreadBannerProjection {
    UiUnreadBannerView view;
    u64 first_unread_id;   // resident first unread, else zero
};
UnreadBannerProjection unread_banner_project(const UnreadBannerInput&) noexcept;

// Plain Escape in the timeline/composer: what a consumer should do, if anything.
struct UnreadEscapePlan {
    bool mark_read, jump_to_present;
    constexpr bool any() const noexcept { return mark_read || jump_to_present; }
};
UnreadEscapePlan unread_banner_escape_plan(const UnreadBannerInput&) noexcept;

} // namespace maicord
