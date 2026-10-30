#include "maicord/app/dave_receive_epoch.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "byte_views.hpp"

namespace maicord {
namespace {
constexpr u64 never = ~u64{0};
constexpr u32 key_lifetime_ms = 10000, maximum_gap = 250;
struct Entry {
    AesGcm64Key key;
    u64 expiry_ms;
    u8 material[16];
};
Entry* entries(DaveReceiveEpoch& epoch) noexcept {
    return reinterpret_cast<Entry*>(epoch.storage.data + 32);
}
u64 deadline(u64 now) noexcept {
    return now > never - key_lifetime_ms ? never : now + key_lifetime_ms;
}
bool grow(DaveReceiveEpoch& epoch, u32 required) noexcept {
    if (required <= epoch.capacity)
        return true;
    constexpr u32 limit = (~u32{0} - 32) / sizeof(Entry);
    if (required > limit)
        return false;
    const u32 doubled = epoch.capacity > limit / 2 ? limit : epoch.capacity * 2;
    const u32 capacity = maximum(required, maximum(4u, doubled));
    SecureBuffer replacement{};
    if (!secure_buffer_initialize(&replacement, 32 + capacity * sizeof(Entry)).ok())
        return false;
    if (epoch.storage.data)
        copy_bytes(replacement.data, epoch.storage.data, 32 + epoch.count * sizeof(Entry));
    // Transfer Entry ownership, not the key allocations themselves. A key's
    // storage is a separate locked buffer, never part of this movable array.
    secure_buffer_release(&epoch.storage);
    epoch.storage = replacement;
    epoch.capacity = capacity;
    return true;
}
void cleanup(DaveReceiveEpoch& epoch, u64 now) noexcept {
    if (!epoch.cleanup_pending && now <= epoch.next_key_expiry_ms)
        return;
    auto* data = entries(epoch);
    epoch.next_key_expiry_ms = never;
    for (u32 i = 0; i != epoch.count; ++i) {
        if (data[i].key.storage.data && now > data[i].expiry_ms)
            aes_gcm64_release(&data[i].key);
        if (data[i].key.storage.data)
            epoch.next_key_expiry_ms = minimum(epoch.next_key_expiry_ms, data[i].expiry_ms);
    }
    u32 removed = 0;
    while (removed != epoch.count && epoch.oldest_generation < epoch.newest_generation &&
           !data[removed].key.storage.data) {
        ++removed;
        ++epoch.oldest_generation;
    }
    if (removed) {
        epoch.count -= removed;
        copy_bytes(data, data + removed, epoch.count * sizeof(Entry));
        secure_clear_bytes(data + epoch.count, removed * sizeof(Entry));
    }
    epoch.cleanup_pending = false;
}
} // namespace
bool dave_receive_epoch_aliases(const DaveReceiveEpoch& epoch, ConstSlice<u8> span) noexcept {
    if (byte_ranges_overlap(span, {epoch.storage.data, epoch.storage.capacity}))
        return true;
    const auto* data = reinterpret_cast<const Entry*>(epoch.storage.data + 32);
    for (u32 i = 0; i != epoch.count; ++i)
        if (byte_ranges_overlap(span, {data[i].key.storage.data, data[i].key.storage.capacity}))
            return true;
    return false;
}
KdfResult dave_receive_epoch_initialize(DaveReceiveEpoch* epoch, ConstSlice<u8> seed, u64 now) noexcept {
    if (!grow(*epoch, 1))
        return {0, 0, KdfError::storage};
    copy_bytes(epoch->storage.data, seed.data, 16);
    epoch->seed_bytes = 16;
    epoch->created_ms = now;
    epoch->next_key_expiry_ms = never;
    return {};
}
void dave_receive_epoch_release(DaveReceiveEpoch* epoch) noexcept {
    if (epoch->storage.data) {
        auto* data = entries(*epoch);
        for (u32 i = 0; i != epoch->count; ++i)
            aes_gcm64_release(&data[i].key);
        secure_buffer_release(&epoch->storage);
    }
    reset_value(epoch);
}
DaveEpochError dave_receive_epoch_open(DaveReceiveEpoch& epoch, KdfSha256Workspace& workspace,
                                       const DaveFrameView& view, u64 now, u8* scratch,
                                       u8* output) noexcept {
    // Resolve before cleanup, as libdave does, so an expiring low generation
    // is rejected instead of reinterpreted as a different generation mid-call.
    DaveFrameCounter counter{};
    if (!dave_frame_counter(epoch.oldest_generation, view.nonce, &counter))
        return DaveEpochError::generation_gap;
    if (!dave_replay_can_accept(epoch.replay, counter.value))
        return DaveEpochError::replay_rejected;
    cleanup(epoch, now);
    const u32 generation = counter.generation;
    if (generation < epoch.oldest_generation ||
        u64{generation} > u64{epoch.newest_generation} + maximum_gap)
        return DaveEpochError::generation_gap;
    // Milliseconds -> whole seconds first. Even UINT64_MAX milliseconds *
    // 170/1000 fits uint64; no floating point or overflowing uint32 addition.
    const u64 lifetime_generations = (((now - epoch.created_ms) / 1000) * 170) >> 24;
    if (generation > lifetime_generations)
        return DaveEpochError::generation_lifetime;
    const u64 required = u64{generation} - epoch.oldest_generation + 1;
    if (required > ~u32{0} || !grow(epoch, static_cast<u32>(required)))
        return DaveEpochError::storage;
    auto* data = entries(epoch);
    while (epoch.next_generation <= generation) {
        auto& entry = data[epoch.count];
        const auto derived = dave_ratchet_step(workspace, {epoch.storage.data, epoch.seed_bytes},
                                               static_cast<u32>(epoch.next_generation), entry.material,
                                               epoch.storage.data);
        if (!derived.ok())
            return DaveEpochError::derivation;
        ++epoch.count;
        ++epoch.next_generation; // 64-bit terminal value prevents generation wrap.
        epoch.seed_bytes = 32;
    }
    auto& entry = data[generation - epoch.oldest_generation];
    if (!entry.key.storage.data) {
        if (!aes_gcm64_initialize(&entry.key, {entry.material, 16}).ok())
            return DaveEpochError::key_import;
        entry.expiry_ms = generation < epoch.newest_generation ? deadline(now) : never;
        epoch.next_key_expiry_ms = minimum(epoch.next_key_expiry_ms, entry.expiry_ms);
    }
    // The entry at index generation - oldest is this generation's key, so it
    // is the key for the nonce's counter.
    switch (dave_frame_open(view, entry.key, epoch.replay, counter.value, scratch, output)) {
    case AeadError::none:
        break;
    case AeadError::authentication_failed:
        return DaveEpochError::authentication_failed;
    default:
        return DaveEpochError::crypto_failure;
    }
    if (generation > epoch.newest_generation) {
        epoch.newest_generation = generation;
        const u64 expires = deadline(now);
        for (u32 i = 0; i != epoch.count && u64{epoch.oldest_generation} + i < generation; ++i)
            if (data[i].key.storage.data) {
                data[i].expiry_ms = minimum(data[i].expiry_ms, expires);
                epoch.next_key_expiry_ms = minimum(epoch.next_key_expiry_ms, data[i].expiry_ms);
            }
        epoch.cleanup_pending = true;
    }
    return DaveEpochError::none;
}
} // namespace maicord
