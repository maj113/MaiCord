#include "maicord/model/guild/permission_grid.hpp"
#include "maicord/core/packed_reader.hpp"
#include "maicord/media/compression/inflated_block.hpp"

namespace maicord {
namespace {
#include <permission_grid_pack.inc>
// The grid's data is src/model/guild/permission_grid_data.inc, shipped as one zlib stream (the format is in
// tools/pack_tables.py) that the first reader inflates into these tables.
PermissionGridText g_channel_text[kPermissionGridChannelTexts];
PermissionGridEntry g_entries[kPermissionGridEntries];
Utf8View g_group_labels[kPermissionGridGroups];
InflatedBlock g_block;

// Runs once on the inflated bytes, before any thread sees them.
void publish(const u8* data) noexcept {
    PackedReader in{data};
    for (auto& text : g_channel_text) {
        text.label = in.text();
        text.description = in.text();
    }
    for (auto& entry : g_entries) {
        entry.bit = PermissionSet{1} << in.byte();
        entry.label = in.text();
        entry.description = in.text();
        const u32 channel = in.byte();
        entry.channel = channel ? g_channel_text + channel - 1 : nullptr;
        entry.group = static_cast<PermissionGroup>(in.byte());
        entry.scopes = static_cast<u8>(in.byte());
    }
    for (auto& label : g_group_labels)
        label = in.text();
}
bool ready() noexcept {
    return g_block.state == 2 || inflated_block_load(g_block, kPermissionGridPacked, sizeof(kPermissionGridPacked),
                                                     kPermissionGridBytes, publish) != nullptr;
}
} // namespace

ConstSlice<PermissionGridEntry> permission_grid_entries() noexcept {
    return ready() ? ConstSlice<PermissionGridEntry>{g_entries, kPermissionGridEntries} : ConstSlice<PermissionGridEntry>{};
}

Utf8View permission_group_label(PermissionGroup group) noexcept {
    return group < PermissionGroup::count && ready() ? g_group_labels[static_cast<u32>(group)] : Utf8View{};
}

u8 permission_grid_channel_scope(u8 type) noexcept {
    switch (type) {
    case 0:
    case 5:
    case 15:
    case 16:
        return permission_scope_text;
    case 2:
        return permission_scope_voice;
    case 13:
        return permission_scope_stage;
    case 4:
        return permission_scope_channels;
    default:
        return 0;
    }
}

bool permission_grid_applies(const PermissionGridEntry& entry, u8 scope) noexcept {
    return (entry.scopes & scope) != 0;
}

Utf8View permission_grid_label(const PermissionGridEntry& entry, u8 scope) noexcept {
    return !(scope & permission_scope_guild) && entry.channel && entry.channel->label.bytes ? entry.channel->label : entry.label;
}

Utf8View permission_grid_description(const PermissionGridEntry& entry, u8 scope) noexcept {
    return !(scope & permission_scope_guild) && entry.channel && entry.channel->description.bytes ? entry.channel->description
                                                                                : entry.description;
}

PermissionTriState permission_tri_get(PermissionSet allow, PermissionSet deny, PermissionSet bit) noexcept {
    return (allow & bit) ? PermissionTriState::allow
           : (deny & bit) ? PermissionTriState::deny
                          : PermissionTriState::neutral;
}

void permission_tri_set(PermissionSet* allow, PermissionSet* deny, PermissionSet bit,
                        PermissionTriState state) noexcept {
    *allow &= ~bit;
    *deny &= ~bit;
    if (state == PermissionTriState::allow)
        *allow |= bit;
    else if (state == PermissionTriState::deny)
        *deny |= bit;
}

bool permission_overwrite_draft_load(PermissionOverwriteDraft* draft,
                                     ConstSlice<PermissionOverwriteRecord> source) noexcept {
    if (!draft || source.count > kPermissionOverwriteDraftMaximum || (source.count && !source.data))
        return false;
    for (u32 i = 0; i < source.count; ++i)
        draft->entries[i] = source[i];
    draft->count = source.count;
    return true;
}

u32 permission_overwrite_draft_find(const PermissionOverwriteDraft& draft, u64 id) noexcept {
    for (u32 i = 0; i < draft.count; ++i)
        if (draft.entries[i].id == id)
            return i;
    return ~0u;
}

bool permission_overwrite_draft_add(PermissionOverwriteDraft* draft, u64 id, u8 type) noexcept {
    if (!draft || !id || type > 1 || draft->count >= kPermissionOverwriteDraftMaximum ||
        permission_overwrite_draft_find(*draft, id) != ~0u)
        return false;
    draft->entries[draft->count++] = {id, 0, 0, type};
    return true;
}

bool permission_overwrite_draft_remove(PermissionOverwriteDraft* draft, u64 id) noexcept {
    const u32 index = draft ? permission_overwrite_draft_find(*draft, id) : ~0u;
    if (index == ~0u)
        return false;
    for (u32 i = index + 1; i < draft->count; ++i)
        draft->entries[i - 1] = draft->entries[i];
    --draft->count;
    return true;
}

bool permission_overwrite_draft_set(PermissionOverwriteDraft* draft, u64 id, PermissionSet bit,
                                    PermissionTriState state) noexcept {
    const u32 index = draft ? permission_overwrite_draft_find(*draft, id) : ~0u;
    if (index == ~0u || !bit || state > PermissionTriState::allow)
        return false;
    // Packed records are copied out and back to avoid unaligned references.
    auto entry = draft->entries[index];
    PermissionSet allow = entry.allow, deny = entry.deny;
    permission_tri_set(&allow, &deny, bit, state);
    entry.allow = allow;
    entry.deny = deny;
    draft->entries[index] = entry;
    return true;
}

bool permission_overwrite_draft_equal(const PermissionOverwriteDraft& draft,
                                      ConstSlice<PermissionOverwriteRecord> source) noexcept {
    if (draft.count != source.count)
        return false;
    for (u32 i = 0; i < source.count; ++i) {
        const u32 index = permission_overwrite_draft_find(draft, source[i].id);
        if (index == ~0u)
            return false;
        const auto entry = draft.entries[index];
        if (entry.allow != source[i].allow || entry.deny != source[i].deny || entry.type != source[i].type)
            return false;
    }
    return true;
}

u32 permission_overwrite_draft_write(const PermissionOverwriteDraft& draft,
                                     Slice<PermissionOverwriteWrite> output) noexcept {
    if (draft.count > output.count || (draft.count && !output.data))
        return ~0u;
    for (u32 i = 0; i < draft.count; ++i) {
        const auto entry = draft.entries[i];
        output[i] = {entry.id, entry.allow, entry.deny, entry.type};
    }
    return draft.count;
}

ConstSlice<PermissionOverwriteRecord> chat_state_store_channel_overwrites(
    const ChatStateStore& store, const ChannelRecord& channel) noexcept {
    if (!store.overwrites || !channel.overwrite_count ||
        channel.overwrite_offset + channel.overwrite_count > store.overwrite_count)
        return {};
    return {store.overwrites + channel.overwrite_offset, channel.overwrite_count};
}

} // namespace maicord
