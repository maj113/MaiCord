#pragma once

#include "maicord/protocol/guild_community.hpp"

namespace maicord {

// Scheduled events of the account's servers, fed by READY/GUILD_CREATE
// guild_scheduled_events, GUILD_SCHEDULED_EVENT_* dispatches and the REST
// list. Bounded: when full, finished events go first, then the latest start.
inline constexpr u32 kScheduledEventStoreCapacity = 64;

struct ScheduledEventStore {
    ScheduledEvent entries[kScheduledEventStoreCapacity];
    u32 count;
    u32 revision; // bumps on every visible change
};

void scheduled_event_store_reset(ScheduledEventStore*) noexcept;
const ScheduledEvent* scheduled_event_store_find(const ScheduledEventStore&, u64 event_id) noexcept;
// Inserts or replaces an event. A replacement without a user count or an
// interest observation keeps the stored one (Gateway updates carry neither).
// Completed and canceled events are removed instead.
bool scheduled_event_store_upsert(ScheduledEventStore*, const ScheduledEvent&) noexcept;
bool scheduled_event_store_remove(ScheduledEventStore*, u64 event_id) noexcept;
bool scheduled_event_store_clear_guild(ScheduledEventStore*, u64 guild_id) noexcept;
// GUILD_SCHEDULED_EVENT_USER_ADD/REMOVE: a known count moves by one, and the
// current account's own row sets its interest.
bool scheduled_event_store_user(ScheduledEventStore*, u64 event_id, u64 user_id, u64 self_user_id,
                                bool added) noexcept;
// Interest from /users/@me/scheduled-events or a confirmed PUT/DELETE.
bool scheduled_event_store_set_interest(ScheduledEventStore*, u64 event_id, bool interested,
                                        bool adjust_count) noexcept;
// Marks every event of a guild as having a known interest (a complete RSVP
// list arrived): those not named afterwards are not interested.
void scheduled_event_store_reset_interest(ScheduledEventStore*, u64 guild_id) noexcept;
// Upcoming or live events of a guild: live first, then by start time.
u32 scheduled_event_store_guild_events(const ScheduledEventStore&, u64 guild_id,
                                       Slice<const ScheduledEvent*> output) noexcept;
u32 scheduled_event_store_guild_count(const ScheduledEventStore&, u64 guild_id) noexcept;
constexpr bool scheduled_event_listed(const ScheduledEvent& event) noexcept {
    return event.status != ScheduledEventStatus::completed && event.status != ScheduledEventStatus::canceled;
}

} // namespace maicord
