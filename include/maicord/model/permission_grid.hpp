#pragma once

#include "maicord/model/chat_state.hpp"
#include "maicord/model/permission_bits.hpp"
#include "maicord/protocol/guild_admin.hpp"

namespace maicord {

// Editable permission catalog shared by the role editor (guild level) and
// channel permission overrides. Wording is original and kept short.
enum class PermissionGroup : u8 {
    general,
    membership,
    text,
    voice,
    apps,
    stage,
    events,
    advanced,
    count
};

enum PermissionScope : u8 {
    permission_scope_guild = 1u << 0,
    permission_scope_text = 1u << 1,
    permission_scope_voice = 1u << 2,
    permission_scope_stage = 1u << 3,
    permission_scope_channels = permission_scope_text | permission_scope_voice | permission_scope_stage,
};

struct PermissionGridEntry {
    PermissionSet bit;
    // Label, description, channel label and channel description, NUL-separated;
    // read through permission_grid_label/description. Channel overrides rename
    // a few guild permissions; an empty channel field keeps the guild wording.
    const char* text;
    PermissionGroup group;
    u8 scopes;
};

ConstSlice<PermissionGridEntry> permission_grid_entries() noexcept;
Utf8View permission_group_label(PermissionGroup) noexcept;
// Scope of a channel type; categories show every channel permission.
u8 permission_grid_channel_scope(u8 channel_type) noexcept;
bool permission_grid_applies(const PermissionGridEntry&, u8 scope) noexcept;
Utf8View permission_grid_label(const PermissionGridEntry&, u8 scope) noexcept;
Utf8View permission_grid_description(const PermissionGridEntry&, u8 scope) noexcept;

// Override value of one bit. Allow wins when a malformed pair sets both.
enum class PermissionTriState : u8 {
    deny,
    neutral,
    allow
};
PermissionTriState permission_tri_get(PermissionSet allow, PermissionSet deny, PermissionSet bit) noexcept;
void permission_tri_set(PermissionSet* allow, PermissionSet* deny, PermissionSet bit, PermissionTriState) noexcept;

// Editable copy of one channel's overrides. Order follows insertion; equality
// ignores order. Type 0 is a role, 1 a member.
inline constexpr u32 kPermissionOverwriteDraftMaximum = kGuildAdminMaximumOverwrites;
struct PermissionOverwriteDraft {
    PermissionOverwriteRecord entries[kPermissionOverwriteDraftMaximum];
    u32 count;
};
bool permission_overwrite_draft_load(PermissionOverwriteDraft*, ConstSlice<PermissionOverwriteRecord>) noexcept;
// Index of an override or ~0u.
u32 permission_overwrite_draft_find(const PermissionOverwriteDraft&, u64 id) noexcept;
bool permission_overwrite_draft_add(PermissionOverwriteDraft*, u64 id, u8 type) noexcept;
bool permission_overwrite_draft_remove(PermissionOverwriteDraft*, u64 id) noexcept;
bool permission_overwrite_draft_set(PermissionOverwriteDraft*, u64 id, PermissionSet bit, PermissionTriState) noexcept;
bool permission_overwrite_draft_equal(const PermissionOverwriteDraft&,
                                      ConstSlice<PermissionOverwriteRecord>) noexcept;
// Wire form for a channel PATCH; returns the number written or ~0u when output is short.
u32 permission_overwrite_draft_write(const PermissionOverwriteDraft&, Slice<PermissionOverwriteWrite>) noexcept;
// Channel's overrides as stored, or an empty slice.
ConstSlice<PermissionOverwriteRecord> chat_state_store_channel_overwrites(const ChatStateStore&,
                                                                                  const ChannelRecord&) noexcept;

} // namespace maicord
