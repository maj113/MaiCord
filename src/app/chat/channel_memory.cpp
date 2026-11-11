#include "maicord/app/chat/channel_memory.hpp"
#include "maicord/app/net/file_persistence.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/model/chat/chat_state.hpp"

namespace maicord {
namespace {
constexpr u8 kMagic[] = {'M', 'C', 'C', 'M'};
constexpr u8 kVersion = 1, kTabsVersion = 2;

bool entry_valid(const ChannelMemoryEntry& entry) noexcept {
    return entry.channel_id != 0 && entry.parent_id != entry.channel_id;
}
} // namespace

bool channel_memory_remember(ChannelMemory* memory, u64 guild_id, u64 channel_id, u64 parent_id) noexcept {
    const ChannelMemoryEntry entry{guild_id, channel_id, parent_id};
    if (!memory || memory->count > kChannelMemoryCapacity || !entry_valid(entry))
        return false;
    u32 found = memory->count;
    for (u32 i = 0; i < memory->count; ++i)
        if (memory->entries[i].guild_id == guild_id)
            found = i;
    if (found == 0 && memory->entries[0].channel_id == channel_id && memory->entries[0].parent_id == parent_id)
        return false;
    // Shift the newer entries down over the old slot, or over the least recent one when full.
    const u32 last = found < memory->count ? found : minimum<u32>(memory->count, kChannelMemoryCapacity - 1);
    for (u32 i = last; i > 0; --i)
        memory->entries[i] = memory->entries[i - 1];
    memory->entries[0] = entry;
    if (found == memory->count && memory->count < kChannelMemoryCapacity)
        ++memory->count;
    ++memory->revision;
    return true;
}

const ChannelMemoryEntry* channel_memory_find(const ChannelMemory& memory, u64 guild_id) noexcept {
    for (u32 i = 0; i < memory.count && i < kChannelMemoryCapacity; ++i)
        if (memory.entries[i].guild_id == guild_id)
            return &memory.entries[i];
    return nullptr;
}

const ChannelMemoryEntry* channel_memory_latest(const ChannelMemory& memory) noexcept {
    return memory.count && memory.count <= kChannelMemoryCapacity ? &memory.entries[0] : nullptr;
}

bool channel_memory_observe(ChannelMemory* memory, const ChatStateStore& state) noexcept {
    if (!state.selected_channel_id || state.restore_channel_id)
        return false;
    const ChannelRecord* channel = chat_state_store_find_channel(state, state.selected_channel_id);
    return channel && channel_memory_remember(memory, channel->guild_id, channel->id,
                                              channel_is_thread(channel->type) ? channel->parent_id : 0);
}

bool channel_memory_set_tabs(ChannelMemory* memory, ConstSlice<ChannelMemoryTab> tabs) noexcept {
    if (!memory || tabs.count > kChannelMemoryTabCapacity)
        return false;
    bool same = tabs.count == memory->tab_count;
    for (u32 i = 0; i < tabs.count; ++i) {
        for (u32 j = 0; j < i; ++j)
            if (tabs[j].channel_id == tabs[i].channel_id)
                return false;
        if (!tabs[i].channel_id || tabs[i].label_bytes > kChannelMemoryTabLabelBytes)
            return false;
        const ChannelMemoryTab& old = memory->tabs[i];
        same = same && old.channel_id == tabs[i].channel_id && old.guild_id == tabs[i].guild_id &&
               old.label_bytes == tabs[i].label_bytes && bytes_equal(old.label, tabs[i].label, old.label_bytes);
    }
    if (same)
        return false;
    clear_bytes(memory->tabs, sizeof(memory->tabs));
    copy_bytes(memory->tabs, tabs.data, tabs.count * sizeof(ChannelMemoryTab));
    memory->tab_count = static_cast<u8>(tabs.count);
    ++memory->revision;
    return true;
}

void channel_memory_seed_restore(const ChannelMemory& memory, ChatStateStore* state) noexcept {
    if (const ChannelMemoryEntry* latest = channel_memory_latest(memory)) {
        state->restore_channel_id = latest->channel_id;
        state->restore_guild_id = latest->guild_id;
        state->home_selected = latest->guild_id == 0;
    }
    if (const ChannelMemoryEntry* home = channel_memory_find(memory, 0))
        state->last_private_channel_id = home->channel_id;
}

u32 channel_memory_encode(const ChannelMemory& memory, Slice<u8> output) noexcept {
    const u32 entries = 6 + memory.count * kChannelMemoryEntryBytes;
    u32 body = entries;
    for (u32 i = 0; i < memory.tab_count && i < kChannelMemoryTabCapacity; ++i) {
        if (memory.tabs[i].label_bytes > kChannelMemoryTabLabelBytes)
            return 0;
        body += (i ? 0 : 1) + 17 + memory.tabs[i].label_bytes;
    }
    if (memory.count > kChannelMemoryCapacity || memory.tab_count > kChannelMemoryTabCapacity || !output.data ||
        output.count < body + 4)
        return 0;
    const u8 header[] = {kMagic[0], kMagic[1], kMagic[2], kMagic[3], memory.tab_count ? kTabsVersion : kVersion,
                         memory.count};
    copy_bytes(output.data, header, sizeof(header));
    for (u32 i = 0; i < memory.count; ++i) {
        u8* at = output.data + 6 + i * kChannelMemoryEntryBytes;
        byte_store_le64(at, memory.entries[i].guild_id);
        byte_store_le64(at + 8, memory.entries[i].channel_id);
        byte_store_le64(at + 16, memory.entries[i].parent_id);
    }
    u32 at = entries;
    if (memory.tab_count)
        output.data[at++] = memory.tab_count;
    for (u32 i = 0; i < memory.tab_count; ++i) {
        const ChannelMemoryTab& tab = memory.tabs[i];
        byte_store_le64(output.data + at, tab.channel_id);
        byte_store_le64(output.data + at + 8, tab.guild_id);
        output.data[at + 16] = tab.label_bytes;
        copy_bytes(output.data + at + 17, tab.label, tab.label_bytes);
        at += 17 + tab.label_bytes;
    }
    const u32 hash = hash32_bytes(output.data, body);
    byte_store_le32(output.data + body, hash);
    return body + 4;
}

bool channel_memory_decode(ConstSlice<u8> input, ChannelMemory* memory) noexcept {
    if (!memory || !input.data || input.count < 10 || input.count > kChannelMemoryFileBytes ||
        !bytes_equal(input.data, kMagic, 4) || (input[4] != kVersion && input[4] != kTabsVersion) ||
        input[5] > kChannelMemoryCapacity)
        return false;
    const u32 entries = 6 + input[5] * kChannelMemoryEntryBytes;
    const u32 body = input.count - 4;
    if (entries > body || byte_load_le32(input.data + body) != hash32_bytes(input.data, body))
        return false;
    ChannelMemory decoded{};
    for (u32 i = 0; i < input[5]; ++i) {
        const u8* at = input.data + 6 + i * kChannelMemoryEntryBytes;
        const ChannelMemoryEntry entry{byte_load_le64(at), byte_load_le64(at + 8), byte_load_le64(at + 16)};
        if (!entry_valid(entry) || channel_memory_find(decoded, entry.guild_id))
            return false;
        decoded.entries[i] = entry;
        decoded.count = static_cast<u8>(i + 1);
    }
    // Version 2 appends the tab count and the tabs, which the saved selections do not depend on: a tab that cannot be
    // used (no channel, a repeated one, a label that is not UTF-8) is skipped, and a record that runs out of the file
    // ends the list, so one bad tab costs that tab and nothing else.
    ChannelMemoryTab tabs[kChannelMemoryTabCapacity]{};
    u32 count = 0;
    if (input[4] == kVersion) {
        if (entries != body)
            return false;
    } else {
        u32 at = entries;
        const u32 saved = at < body ? input[at++] : 0;
        for (u32 i = 0; i < saved && count < kChannelMemoryTabCapacity && at + 17 <= body; ++i) {
            ChannelMemoryTab tab{};
            tab.channel_id = byte_load_le64(input.data + at);
            tab.guild_id = byte_load_le64(input.data + at + 8);
            tab.label_bytes = input[at + 16];
            const char* label = reinterpret_cast<const char*>(input.data + at + 17);
            if (at + 17 + tab.label_bytes > body)
                break;
            at += 17 + tab.label_bytes;
            bool usable = tab.channel_id && tab.label_bytes <= kChannelMemoryTabLabelBytes &&
                          utf8_validate({label, tab.label_bytes}).valid;
            for (u32 j = 0; j < count; ++j)
                usable = usable && tabs[j].channel_id != tab.channel_id;
            if (!usable)
                continue;
            copy_bytes(tab.label, label, tab.label_bytes);
            tabs[count++] = tab;
        }
    }
    if (count && !channel_memory_set_tabs(&decoded, {tabs, count}))
        return false;
    decoded.revision = memory->revision + 1;
    *memory = decoded;
    return true;
}

bool channel_memory_path(u64 profile_id, wchar_t* path, u32 capacity) noexcept {
    return profile_data_path(profile_id, path, capacity, L"accounts\\channels-", L".last");
}

bool channel_memory_load_file(const wchar_t* path, ChannelMemory* memory) noexcept {
    if (!path)
        return false;
    const u32 revision = memory->revision + 1;
    clear_bytes(memory, sizeof(*memory));
    memory->revision = revision;
    u8 record[kChannelMemoryFileBytes];
    const FileReadResult read = file_persistence_read_file(path, {record, sizeof(record)});
    return read.error == FileReadError::missing || (read.ok() && channel_memory_decode({record, read.bytes}, memory));
}

bool channel_memory_save_file(const wchar_t* path, const ChannelMemory& memory) noexcept {
    u8 record[kChannelMemoryFileBytes];
    const u32 bytes = channel_memory_encode(memory, {record, sizeof(record)});
    const u32 length = wide_length(path, 32768);
    return bytes && length && file_persistence_write_atomic(path, length, {record, bytes}).ok();
}

} // namespace maicord
