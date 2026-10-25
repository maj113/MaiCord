#include "maicord/model/private_channel_store.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/heap_sort.hpp"
#include "maicord/sys/crt.hpp"
#pragma intrinsic(_alloca)

namespace maicord {
namespace {
u32 lower_bound(const PrivateChannelStore& store, u64 id) noexcept {
    u32 first = 0, last = store.count;
    while (first < last) {
        const u32 middle = first + (last - first) / 2;
        if (store.records[middle].id < id) first = middle + 1;
        else last = middle;
    }
    return first;
}
bool valid(const PrivateChannelStore& store) noexcept {
    return store.records.data && store.recipients.data && store.count <= store.records.count &&
        store.recipient_count <= store.recipients.count;
}
bool icon_valid(Utf8View icon) noexcept {
    if (!icon.bytes) return true;
    if (!icon.data || (icon.bytes != 32 && icon.bytes != 34)) return false;
    const u32 first = icon.bytes == 34 ? 2 : 0;
    if (first && (icon.data[0] != 'a' || icon.data[1] != '_')) return false;
    for (u32 i = first; i < icon.bytes; ++i)
        if (!((icon.data[i] >= '0' && icon.data[i] <= '9') || (icon.data[i] >= 'a' && icon.data[i] <= 'f'))) return false;
    return true;
}
void erase_recipients(PrivateChannelStore* store, u32 index) noexcept {
    auto& record = store->records[index];
    if (!record.recipient_count) return;
    const u32 end = record.recipient_offset + record.recipient_count;
    for (u32 i = end; i < store->recipient_count; ++i) store->recipients[i - record.recipient_count] = store->recipients[i];
    store->recipient_count -= record.recipient_count;
    for (u32 i = 0; i < store->count; ++i)
        if (i != index && store->records[i].recipient_offset >= end) store->records[i].recipient_offset -= record.recipient_count;
    record.recipient_count = 0;
}
} // namespace

bool private_channel_store_initialize(PrivateChannelStore* store,
    Slice<PrivateChannelRecord> records, Slice<u64> recipients, Slice<PrivateNicknameRecord> nicknames) noexcept {
    if (!store || !records.data || !records.count || !recipients.data || !recipients.count ||
        store->record_backing.base || store->recipient_backing.base || store->nickname_backing.base ||
        (!nicknames.data && nicknames.count)) return false;
    reset_value(store);
    store->records = records;
    store->recipients = recipients;
    store->nicknames = nicknames;
    return true;
}
bool private_channel_store_initialize_owned(PrivateChannelStore* store, u32 records, u32 recipients) noexcept {
    if (!store || !records || !recipients || records > 65'535 || recipients > 1'000'000 ||
        store->record_backing.base || store->recipient_backing.base || store->nickname_backing.base) return false;
    reset_value(store);
    if (!arena_initialize(&store->record_backing, static_cast<usize>(records) * sizeof(PrivateChannelRecord), 0, 4096) ||
        !arena_initialize(&store->recipient_backing, static_cast<usize>(recipients) * sizeof(u64), 0, 4096) ||
        !arena_initialize(&store->nickname_backing, static_cast<usize>(recipients) * sizeof(PrivateNicknameRecord), 0, 4096)) {
        private_channel_store_release(store);
        return false;
    }
    store->records = {reinterpret_cast<PrivateChannelRecord*>(store->record_backing.base), records};
    store->recipients = {reinterpret_cast<u64*>(store->recipient_backing.base), recipients};
    store->nicknames = {reinterpret_cast<PrivateNicknameRecord*>(store->nickname_backing.base), recipients};
    return true;
}
void private_channel_store_release(PrivateChannelStore* store) noexcept {
    if (!store) return;
    arena_release(&store->record_backing);
    arena_release(&store->recipient_backing);
    arena_release(&store->nickname_backing);
    reset_value(store);
}
void private_channel_store_clear(PrivateChannelStore* store) noexcept {
    if (!store) return;
    store->count = store->recipient_count = 0;
    store->nickname_count = 0;
    store->sort_sequence = 0;
    saturating_increment(store->revision);
}
bool private_channel_store_touch_sort(PrivateChannelStore* store, u64 id) noexcept {
    const auto* found = store ? private_channel_store_find(*store, id) : nullptr;
    if (!found) return false;
    if (store->sort_sequence != ~0ull) ++store->sort_sequence;
    auto& record = store->records[static_cast<u32>(found - store->records.data)];
    record.sort_sequence = store->sort_sequence;
    saturating_increment(record.revision);
    saturating_increment(store->revision);
    return true;
}
const PrivateChannelRecord* private_channel_store_find(const PrivateChannelStore& store, u64 id) noexcept {
    if (!valid(store) || !id) return nullptr;
    const u32 index = lower_bound(store, id);
    return index < store.count && store.records[index].id == id ? &store.records[index] : nullptr;
}
static bool private_channel_store_can_replace(const PrivateChannelStore& store,
                                              const PrivateChannelInput& input) noexcept {
    if (!valid(store) || !input.id || (input.icon_known && !icon_valid(input.icon)) ||
        input.recipients.count > kPrivateRecipientMaximum ||
        (input.recipients.count && !input.recipients.data)) return false;
    const auto* old = private_channel_store_find(store, input.id);
    if (old && (old->recipient_offset > store.recipient_count ||
        old->recipient_count > store.recipient_count - old->recipient_offset)) return false;
    if (!old && store.count == store.records.count) return false;
    if (!input.recipients_known) return true;
    if (input.recipients.count > store.recipients.count - (store.recipient_count - (old ? old->recipient_count : 0))) return false;
    for (u32 i = 0; i < input.recipients.count; ++i) {
        if (!input.recipients[i]) return false;
        for (u32 j = 0; j < i; ++j) if (input.recipients[i] == input.recipients[j]) return false;
    }
    return true;
}
bool private_channel_store_prepare(PrivateChannelStore* store, const PrivateChannelInput& input) noexcept {
    if (!store || !private_channel_store_can_replace(*store, input)) return false;
    const auto* old = private_channel_store_find(*store, input.id);
    const usize record_bytes = static_cast<usize>(store->count + (old ? 0 : 1)) * sizeof(PrivateChannelRecord);
    const usize recipient_bytes = static_cast<usize>(store->recipient_count +
        (input.recipients_known ? input.recipients.count : 0) -
        (old && input.recipients_known ? old->recipient_count : 0)) * sizeof(u64);
    if (store->record_backing.base && record_bytes > store->record_backing.offset &&
        !arena_allocate(&store->record_backing, record_bytes - store->record_backing.offset, 1)) return false;
    if (store->recipient_backing.base && recipient_bytes > store->recipient_backing.offset &&
        !arena_allocate(&store->recipient_backing, recipient_bytes - store->recipient_backing.offset, 1)) return false;
    return true;
}
bool private_channel_store_replace(PrivateChannelStore* store, const PrivateChannelInput& source) noexcept {
    if (!private_channel_store_prepare(store, source)) return false;
    PrivateChannelInput input = source;
    u64 ids[kPrivateRecipientMaximum];
    char icon[34];
    if (input.recipients_known) {
        for (u32 i = 0; i < input.recipients.count; ++i) ids[i] = input.recipients[i];
        input.recipients.data = ids;
    }
    if (input.icon_known) {
        copy_bytes(icon, input.icon.data, input.icon.bytes);
        input.icon.data = icon;
    }
    const u32 index = lower_bound(*store, input.id);
    bool changed = false;
    if (index == store->count || store->records[index].id != input.id) {
        for (u32 i = store->count; i > index; --i) store->records[i] = store->records[i - 1];
        reset_value(&store->records[index]);
        store->records[index].id = input.id;
        ++store->count;
        changed = true;
    }
    auto& record = store->records[index];
    if (input.recipients_known) {
        bool equal = record.recipient_count == input.recipients.count;
        for (u32 i = 0; equal && i < input.recipients.count; ++i)
            equal = store->recipients[record.recipient_offset + i] == input.recipients[i];
        if (!equal) {
            erase_recipients(store, index);
            record.recipient_offset = store->recipient_count;
            record.recipient_count = static_cast<u16>(input.recipients.count);
            for (u32 i = 0; i < input.recipients.count; ++i) store->recipients[store->recipient_count++] = input.recipients[i];
            changed = true;
        }
    }
    if (input.owner_known && record.owner_id != input.owner_id) { record.owner_id = input.owner_id; changed = true; }
    if (input.request_known && record.request_flags != input.request_flags) {
        record.request_flags = input.request_flags;
        changed = true;
    }
    if (input.safety_known && (record.safety_warning != input.safety_warning ||
                               record.safety_warning_id != input.safety_warning_id)) {
        record.safety_warning = input.safety_warning_id ? input.safety_warning : PrivateSafetyWarning::none;
        record.safety_warning_id = record.safety_warning == PrivateSafetyWarning::none ? 0 : input.safety_warning_id;
        changed = true;
    }
    if (input.icon_known && (record.icon_bytes != input.icon.bytes || !bytes_equal(record.icon, input.icon.data, input.icon.bytes))) {
        record.icon_bytes = static_cast<u8>(input.icon.bytes);
        copy_bytes(record.icon, input.icon.data, input.icon.bytes);
        changed = true;
    }
    if (changed) { saturating_increment(record.revision); saturating_increment(store->revision); }
    return true;
}
bool private_channel_store_set_request(PrivateChannelStore* store, u64 id, u8 request_flags) noexcept {
    const auto* found = store ? private_channel_store_find(*store, id) : nullptr;
    if (!found) return false;
    auto& record = store->records[static_cast<u32>(found - store->records.data)];
    if (record.request_flags == request_flags) return true;
    record.request_flags = request_flags;
    saturating_increment(record.revision);
    saturating_increment(store->revision);
    return true;
}
bool private_channel_store_clear_safety_warning(PrivateChannelStore* store, u64 id, u64 warning_id) noexcept {
    const auto* found = store ? private_channel_store_find(*store, id) : nullptr;
    if (!found) return false;
    auto& record = store->records[static_cast<u32>(found - store->records.data)];
    if (!record.safety_warning_id || (warning_id && record.safety_warning_id != warning_id)) return true;
    record.safety_warning_id = 0;
    record.safety_warning = PrivateSafetyWarning::none;
    saturating_increment(record.revision);
    saturating_increment(store->revision);
    return true;
}
u32 private_channel_store_request_count(const PrivateChannelStore& store, bool spam) noexcept {
    if (!valid(store)) return 0;
    u32 count = 0;
    for (u32 i = 0; i < store.count; ++i) {
        const u8 flags = store.records[i].request_flags;
        count += spam ? (flags & private_request_spam) != 0
                      : (flags & private_request_pending) && !(flags & private_request_spam);
    }
    return count;
}
bool private_channel_store_remove(PrivateChannelStore* store, u64 id) noexcept {
    const auto* found = store ? private_channel_store_find(*store, id) : nullptr;
    if (!found) return false;
    const u32 index = static_cast<u32>(found - store->records.data);
    if (store->nickname_count && !private_channel_store_replace_nicknames(store, id, {})) return false;
    erase_recipients(store, index);
    for (u32 i = index + 1; i < store->count; ++i) store->records[i - 1] = store->records[i];
    --store->count;
    saturating_increment(store->revision);
    return true;
}
ConstSlice<u64> private_channel_store_recipients(const PrivateChannelStore& store,
    const PrivateChannelRecord& record) noexcept {
    if (!valid(store) || record.recipient_offset > store.recipient_count ||
        record.recipient_count > store.recipient_count - record.recipient_offset) return {};
    return {store.recipients.data + record.recipient_offset, record.recipient_count};
}
namespace {
u32 nickname_first(const PrivateChannelStore& store, u64 channel_id, u64 user_id = 0) noexcept {
    u32 first = 0, last = store.nickname_count;
    while (first < last) {
        const u32 middle = first + (last - first) / 2;
        const auto& item = store.nicknames[middle];
        if (item.channel_id < channel_id || (item.channel_id == channel_id && item.user_id < user_id)) first = middle + 1;
        else last = middle;
    }
    return first;
}
}
Utf8View private_channel_store_nickname(const PrivateChannelStore& store, u64 channel_id, u64 user_id) noexcept {
    if (!channel_id || !user_id || store.nickname_count > store.nicknames.count ||
        (store.nickname_count && !store.nicknames.data)) return {};
    const u32 index = nickname_first(store, channel_id, user_id);
    if (index == store.nickname_count) return {};
    const auto& record = store.nicknames[index];
    return record.channel_id == channel_id && record.user_id == user_id && record.bytes <= 128 ?
        Utf8View{record.name, record.bytes} : Utf8View{};
}
bool private_channel_store_prepare_nicknames(PrivateChannelStore* store, u64 channel_id,
    ConstSlice<PrivateNicknameInput> names) noexcept {
    if (!store || !valid(*store) || !channel_id || names.count > kPrivateRecipientMaximum ||
        (!names.data && names.count) || store->nickname_count > store->nicknames.count ||
        (!store->nicknames.data && (names.count || store->nickname_count))) return false;
    for (u32 i = 0; i < names.count; ++i) {
        if (!names[i].user_id || !names[i].name.data || !names[i].name.bytes || names[i].name.bytes > 128 ||
            !utf8_validate(names[i].name).valid) return false;
        for (u32 j = 0; j < i; ++j) if (names[i].user_id == names[j].user_id) return false;
    }
    const u32 first = nickname_first(*store, channel_id);
    u32 end = first;
    while (end < store->nickname_count && store->nicknames[end].channel_id == channel_id) ++end;
    if (names.count > store->nicknames.count - (store->nickname_count - (end - first))) return false;
    const usize bytes = static_cast<usize>(store->nickname_count - (end - first) + names.count) * sizeof(PrivateNicknameRecord);
    return !store->nickname_backing.base || bytes <= store->nickname_backing.offset ||
        arena_allocate(&store->nickname_backing, bytes - store->nickname_backing.offset, 1);
}
bool private_channel_store_nicknames(const PrivateChannelStore& store, u64 channel_id,
    Slice<PrivateNicknameInput> output, u32* count) noexcept {
    if (!count || !channel_id || store.nickname_count > store.nicknames.count ||
        (store.nickname_count && !store.nicknames.data)) return false;
    *count = 0;
    for (u32 i = nickname_first(store, channel_id); i < store.nickname_count && store.nicknames[i].channel_id == channel_id; ++i) {
        if (!output.data || *count == output.count) return false;
        const auto& item = store.nicknames[i];
        output[(*count)++] = {item.user_id, {item.name, item.bytes}};
    }
    return true;
}
bool private_channel_store_replace_nicknames(PrivateChannelStore* store, u64 channel_id,
    ConstSlice<PrivateNicknameInput> names) noexcept {
    if (!private_channel_store_prepare_nicknames(store, channel_id, names)) return false;
    const auto* channel = private_channel_store_find(*store, channel_id);
    if (!channel) return false;
    auto* staged = names.count ? static_cast<PrivateNicknameRecord*>(
        _alloca(sizeof(PrivateNicknameRecord) * names.count)) : nullptr;
    for (u32 i = 0; i < names.count; ++i) {
        ::new (static_cast<void*>(staged + i)) PrivateNicknameRecord{};
        staged[i].channel_id = channel_id;
        staged[i].user_id = names[i].user_id;
        staged[i].bytes = static_cast<u8>(names[i].name.bytes);
        copy_bytes(staged[i].name, names[i].name.data, names[i].name.bytes);
    }
    heap_sort(staged, names.count, [](const auto& a, const auto& b) noexcept { return a.user_id < b.user_id; });
    const u32 first = nickname_first(*store, channel_id);
    u32 end = first;
    while (end < store->nickname_count && store->nicknames[end].channel_id == channel_id) ++end;
    bool equal = end - first == names.count;
    for (u32 i = 0; equal && i < names.count; ++i) {
        const auto& old = store->nicknames[first + i];
        equal = old.user_id == staged[i].user_id && old.bytes == staged[i].bytes && bytes_equal(old.name, staged[i].name, old.bytes);
    }
    if (equal) return true;
    const u32 old_count = end - first;
    if (names.count > old_count) {
        for (u32 i = store->nickname_count; i > end; --i) store->nicknames[i - 1 + names.count - old_count] = store->nicknames[i - 1];
    } else {
        for (u32 i = end; i < store->nickname_count; ++i) store->nicknames[i - old_count + names.count] = store->nicknames[i];
    }
    for (u32 i = 0; i < names.count; ++i) store->nicknames[first + i] = staged[i];
    store->nickname_count = store->nickname_count - old_count + names.count;
    saturating_increment(store->records[static_cast<u32>(channel - store->records.data)].revision);
    saturating_increment(store->revision);
    return true;
}
} // namespace maicord
