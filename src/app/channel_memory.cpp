#include "maicord/app/channel_memory.hpp"
#include "maicord/app/file_persistence.hpp"
#include "maicord/core/data_path.hpp"
#include "maicord/core/hash.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/win32_process.hpp"

#include "maicord/core/win32_kernel.hpp"
#include "maicord/model/chat_state.hpp"

namespace maicord {
namespace {
constexpr u8 kMagic[] = {'M', 'C', 'C', 'M'};
constexpr u8 kVersion = 1;

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

void channel_memory_seed_restore(const ChannelMemory& memory, ChatStateStore* state) noexcept {
    if (!state)
        return;
    if (const ChannelMemoryEntry* latest = channel_memory_latest(memory)) {
        state->restore_channel_id = latest->channel_id;
        state->restore_guild_id = latest->guild_id;
        state->home_selected = latest->guild_id == 0;
    }
    if (const ChannelMemoryEntry* home = channel_memory_find(memory, 0))
        state->last_private_channel_id = home->channel_id;
}

u32 channel_memory_encode(const ChannelMemory& memory, Slice<u8> output) noexcept {
    const u32 body = 6 + memory.count * kChannelMemoryEntryBytes;
    if (memory.count > kChannelMemoryCapacity || !output.data || output.count < body + 4)
        return 0;
    const u8 header[] = {kMagic[0], kMagic[1], kMagic[2], kMagic[3], kVersion, memory.count};
    copy_bytes(output.data, header, sizeof(header));
    for (u32 i = 0; i < memory.count; ++i) {
        u8* at = output.data + 6 + i * kChannelMemoryEntryBytes;
        byte_store_le64(at, memory.entries[i].guild_id);
        byte_store_le64(at + 8, memory.entries[i].channel_id);
        byte_store_le64(at + 16, memory.entries[i].parent_id);
    }
    const u32 hash = hash32_bytes(output.data, body);
    byte_store_le32(output.data + body, hash);
    return body + 4;
}

bool channel_memory_decode(ConstSlice<u8> input, ChannelMemory* memory) noexcept {
    if (!memory || !input.data || input.count < 10 || input.count > kChannelMemoryFileBytes ||
        !bytes_equal(input.data, kMagic, 4) || input[4] != kVersion || input[5] > kChannelMemoryCapacity ||
        input.count != 10 + input[5] * kChannelMemoryEntryBytes)
        return false;
    const u32 body = input.count - 4;
    const u32 stored = byte_load_le32(input.data + body);
    if (stored != hash32_bytes(input.data, body))
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
    decoded.revision = memory->revision + 1;
    *memory = decoded;
    return true;
}

bool channel_memory_path(u64 profile_id, wchar_t* path, u32 capacity) noexcept {
    if (!profile_id)
        return false;
    const DataPathResult built = data_path(path, capacity, L"accounts\\channels-");
    u32 length = built.length;
    return built.ok() && wide_append_hex(path, capacity, &length, profile_id) &&
           wide_append(path, capacity, &length, L".last");
}

bool channel_memory_load_file(const wchar_t* path, ChannelMemory* memory) noexcept {
    if (!path || !memory)
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
