#pragma once
#include "maicord/model/chat_state.hpp"

namespace maicord {
inline constexpr u32 kReadScopeTargetMaximum = 4096;
enum class ReadScopeKind : u8 {
    guild,
    category
};
struct ReadScopeSource {
    u64 id, guild_id;
    u32 revision, guild_revision;
    u16 generation, guild_generation;
    ReadScopeKind kind;
    u64 effective_permissions;
    u32 actions;
};
struct ReadScopeTarget {
    u64 channel_id, parent_id, message_id, parent_parent_id;
    u64 effective_permissions, parent_effective_permissions;
    u32 actions, parent_actions;
    u16 generation, parent_generation;
    u8 type, parent_type;
    bool outbound;
};
struct ReadScopePlanResult {
    ReadScopeSource source;
    u32 count, outbound_count;
    bool valid;
};
bool read_scope_source(const ChatStateStore&, ReadScopeKind, u64 id, ReadScopeSource*) noexcept;
bool read_scope_source_current(const ChatStateStore&, const ReadScopeSource&) noexcept;
// Count-only admission uses the identical captured action clock and filters.
// Caller can commit just this prefix, then fill before optimistic mutation.
ReadScopePlanResult read_scope_plan_size(const ChatStateStore&, ReadScopeKind, u64 id,
                                         u64 action_time_ms) noexcept;
// Full Core inventory, sorted unique IDs. Null cursors are local-clear targets
// only. Admission failures (including alias/capacity) leave output unchanged.
ReadScopePlanResult read_scope_plan(const ChatStateStore&, ReadScopeKind, u64 id, u64 action_time_ms,
                                    Slice<ReadScopeTarget>) noexcept;
// Validates retained authority/structure without widening the captured cursor
// when a newer message arrives. Parent movement and generation reuse fail closed.
bool read_scope_target_current(const ChatStateStore&, const ReadScopeSource&,
                               const ReadScopeTarget&) noexcept;

// Host-owned availability/progress; default keeps existing menus unchanged.
struct UiScopedReadMenuState {
    u32 mark_action = 0, retry_action = 0;
    bool available = false, pending = false, retry_available = false;
};
} // namespace maicord
