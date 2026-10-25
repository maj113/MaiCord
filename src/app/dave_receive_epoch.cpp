#include "maicord/app/dave_receive_epoch.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"
#include "byte_views.hpp"


namespace maicord {
namespace {
constexpr u64 never = ~u64{0};
constexpr u32 key_lifetime_ms = 10000, maximum_gap = 250;
struct Entry {
    DaveFrameKey key;
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
    // Transfer Entry ownership, not the CNG allocations themselves. Handles
    // point into separate locked buffers, never into this movable entry array.
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
        if (data[i].key.cipher.handle && now > data[i].expiry_ms)
            dave_frame_key_release(&data[i].key);
        if (data[i].key.cipher.handle)
            epoch.next_key_expiry_ms = minimum(epoch.next_key_expiry_ms, data[i].expiry_ms);
    }
    u32 removed = 0;
    while (removed != epoch.count && epoch.oldest_generation < epoch.newest_generation &&
           !data[removed].key.cipher.handle) {
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
bool protected_span(ConstSlice<u8> span, DaveReceiveEpoch& epoch, KdfSha256Workspace& workspace) noexcept {
    return dave_receive_epoch_aliases(epoch, span) || aliases_workspace(workspace, span);
}
DaveEpochOpenResult fail(DaveEpochError error, u32 generation = 0) noexcept {
    DaveEpochOpenResult result{};
    result.error = error;
    result.generation = generation;
    return result;
}
} // namespace
bool dave_receive_epoch_aliases(const DaveReceiveEpoch& epoch, ConstSlice<u8> span) noexcept {
    if (byte_ranges_overlap(span, object_bytes(epoch)) ||
        byte_ranges_overlap(span, {epoch.storage.data, epoch.storage.capacity}))
        return true;
    if (!epoch.storage.data)
        return false;
    const auto* data = reinterpret_cast<const Entry*>(epoch.storage.data + 32);
    for (u32 i = 0; i != epoch.count; ++i)
        if (byte_ranges_overlap(span,
                                {data[i].key.cipher.storage.data, data[i].key.cipher.storage.capacity}))
            return true;
    return false;
}
KdfResult dave_receive_epoch_initialize(DaveReceiveEpoch* epoch, ConstSlice<u8> seed, u64 now,
                                        Gcm64Backend backend) noexcept {
    if (!epoch || epoch->storage.data || epoch->storage.locked || seed.count != 16 ||
        !byte_range_valid(seed) || byte_ranges_overlap(seed, object_bytes(*epoch)) ||
        (backend != Gcm64Backend::automatic && backend != Gcm64Backend::portable &&
         backend != Gcm64Backend::pclmul))
        return {0, 0, KdfError::invalid_input};
    if (!grow(*epoch, 1))
        return {0, 0, KdfError::storage};
    copy_bytes(epoch->storage.data, seed.data, 16);
    epoch->seed_bytes = 16;
    epoch->created_ms = epoch->last_now_ms = now;
    epoch->expiry_ms = epoch->next_key_expiry_ms = never;
    epoch->backend = backend;
    return {};
}
void dave_receive_epoch_release(DaveReceiveEpoch* epoch) noexcept {
    if (!epoch)
        return;
    if (epoch->storage.data) {
        auto* data = entries(*epoch);
        for (u32 i = 0; i != epoch->count; ++i)
            dave_frame_key_release(&data[i].key);
        secure_buffer_release(&epoch->storage);
    }
    reset_value(epoch);
}
DaveEpochOpenResult dave_receive_epoch_open(DaveReceiveEpoch& epoch, KdfSha256Workspace& workspace,
                                            const DaveFrameView& view, u64 now, Slice<u8> scratch,
                                            Slice<u8> output) noexcept {
    if (!epoch.storage.data || !epoch.storage.locked || !workspace.storage.data ||
        !workspace.storage.locked)
        return fail(DaveEpochError::not_initialized);
    if (!byte_range_valid(view.frame) || view.clear_ranges.count > kDaveMaximumClearRanges ||
        !byte_range_valid({scratch.data, scratch.count}) || !byte_range_valid({output.data, output.count}))
        return fail(DaveEpochError::invalid_input);
    const ConstSlice<u8> ranges{reinterpret_cast<const u8*>(view.clear_ranges.data),
                                view.clear_ranges.count * static_cast<u32>(sizeof(DaveClearRange))};
    if (!byte_range_valid(ranges))
        return fail(DaveEpochError::invalid_input);
    if (view.frame.count < 12 || view.body_bytes > view.frame.count - 12 ||
        view.clear_bytes > view.body_bytes) {
        auto result = fail(DaveEpochError::frame);
        result.frame.error = DaveOpenError::invalid_frame;
        result.frame.frame_error = DaveFrameError::invalid_input;
        return result;
    }
    const u32 needed = view.clear_bytes || output.data == view.frame.data ? view.body_bytes : 0;
    if (output.count < view.body_bytes || scratch.count < needed) {
        auto result = fail(DaveEpochError::frame);
        result.frame.error = output.count < view.body_bytes ? DaveOpenError::output_capacity
                                                            : DaveOpenError::scratch_capacity;
        result.frame.scratch_bytes = needed;
        return result;
    }
    const ConstSlice<u8> spans[]{
        object_bytes(view), view.frame, ranges, {scratch.data, scratch.count}, {output.data, output.count}};
    for (auto span : spans)
        if (protected_span(span, epoch, workspace))
            return fail(DaveEpochError::invalid_input);
    if (byte_ranges_overlap(object_bytes(workspace), object_bytes(epoch)) ||
        byte_ranges_overlap(object_bytes(workspace), {epoch.storage.data, epoch.storage.capacity}))
        return fail(DaveEpochError::invalid_input);
    if (now < epoch.last_now_ms)
        return fail(DaveEpochError::invalid_time);
    epoch.last_now_ms = now;
    if (now > epoch.expiry_ms)
        return fail(DaveEpochError::expired);
    // Resolve before cleanup, as libdave does, so an expiring low generation
    // is rejected instead of reinterpreted as a different generation mid-call.
    DaveFrameCounter counter{};
    if (!dave_frame_counter(epoch.oldest_generation, view.nonce, &counter))
        return fail(DaveEpochError::generation_gap);
    if (!dave_replay_can_accept(epoch.replay, counter.value)) {
        auto result = fail(DaveEpochError::frame, counter.generation);
        result.frame.error = DaveOpenError::replay_rejected;
        return result;
    }
    cleanup(epoch, now);
    const u32 generation = counter.generation;
    if (generation < epoch.oldest_generation ||
        u64{generation} > u64{epoch.newest_generation} + maximum_gap)
        return fail(DaveEpochError::generation_gap, generation);
    // Milliseconds -> whole seconds first. Even UINT64_MAX milliseconds *
    // 170/1000 fits uint64; no floating point or overflowing uint32 addition.
    const u64 lifetime_generations = (((now - epoch.created_ms) / 1000) * 170) >> 24;
    if (generation > lifetime_generations)
        return fail(DaveEpochError::generation_lifetime, generation);
    const u64 required = u64{generation} - epoch.oldest_generation + 1;
    if (required > ~u32{0} || !grow(epoch, static_cast<u32>(required)))
        return fail(DaveEpochError::storage, generation);
    auto* data = entries(epoch);
    while (epoch.next_generation <= generation) {
        auto& entry = data[epoch.count];
        const auto derived = dave_ratchet_step(workspace, {epoch.storage.data, epoch.seed_bytes},
                                               static_cast<u32>(epoch.next_generation),
                                               {entry.material, 16}, {epoch.storage.data, 32});
        if (!derived.ok()) {
            auto result = fail(DaveEpochError::derivation, generation);
            result.derivation = derived;
            return result;
        }
        ++epoch.count;
        ++epoch.next_generation; // 64-bit terminal value prevents generation wrap.
        epoch.seed_bytes = 32;
    }
    auto& entry = data[generation - epoch.oldest_generation];
    if (!entry.key.cipher.handle) {
        const auto imported =
            dave_frame_key_initialize(&entry.key, generation, {entry.material, 16}, epoch.backend);
        if (!imported.ok()) {
            auto result = fail(DaveEpochError::key_import, generation);
            result.key_import = imported;
            return result;
        }
        entry.expiry_ms = generation < epoch.newest_generation ? deadline(now) : never;
        epoch.next_key_expiry_ms = minimum(epoch.next_key_expiry_ms, entry.expiry_ms);
    }
    auto result = fail(DaveEpochError::none, generation);
    result.frame = dave_frame_open(view, entry.key, &epoch.replay, scratch, output);
    if (!result.frame.ok()) {
        result.error = DaveEpochError::frame;
        return result;
    }
    if (generation > epoch.newest_generation) {
        epoch.newest_generation = generation;
        const u64 expires = deadline(now);
        for (u32 i = 0; i != epoch.count && u64{epoch.oldest_generation} + i < generation; ++i)
            if (data[i].key.cipher.handle) {
                data[i].expiry_ms = minimum(data[i].expiry_ms, expires);
                epoch.next_key_expiry_ms = minimum(epoch.next_key_expiry_ms, data[i].expiry_ms);
            }
        epoch.cleanup_pending = true;
    }
    return result;
}
} // namespace maicord
