#pragma once
#include "maicord/app/rest_client.hpp"
#include "maicord/protocol/message_reaction.hpp"
#include "maicord/ui/tooltip.hpp"

namespace maicord::app_detail {
// Hovering a reaction pill shows its emoji and ":name: reacted by" its first
// reactors, fetched once per reaction and count.
inline constexpr u32 kReactionTooltipNames = 3;

// The reaction pill under the pointer as a tooltip target; marks its first
// reactors as needed when they are not known yet.
bool application_reaction_tooltip_target(UiPoint point, u64 scope, UiTooltipTarget* target) noexcept;
// The pointer left the pills: a fetch not yet submitted is dropped.
void application_reaction_tooltip_leave() noexcept;
// Admits a needed reactor fetch to the history request lane.
void application_reaction_tooltip_pump() noexcept;
bool application_reaction_tooltip_pending() noexcept;

struct ReactionTooltipQuery {
    Utf8View emoji_name;
    u64 channel_id, message_id, emoji_id, epoch;
    MessageReactionType type;
};
// The lane is free: the fetch to submit, if one is needed.
bool application_reaction_tooltip_query(ReactionTooltipQuery* query) noexcept;
void application_reaction_tooltip_submitted(u64 epoch) noexcept;
void application_reaction_tooltip_failed() noexcept;
void application_reaction_tooltip_response(const RestClientResponse& response, ConstSlice<u8> body,
                                           Slice<u32> page_scratch, Slice<u32> object_scratch) noexcept;
} // namespace maicord::app_detail
