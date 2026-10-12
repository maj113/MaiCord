#include "maicord/model/invite_card_cache.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

bool same_code(const InviteCardEntry& entry, Utf8View code) noexcept {
    return entry.state != InviteCardState::empty && entry.code_bytes == code.bytes &&
           bytes_equal(entry.code, code.data, code.bytes);
}

InviteCardEntry* find(InviteCardCache* cache, Utf8View code) noexcept {
    for (auto& entry : cache->entries)
        if (same_code(entry, code))
            return &entry;
    return nullptr;
}

} // namespace

const InviteCardEntry* invite_card_cache_find(const InviteCardCache& cache, Utf8View code) noexcept {
    for (const auto& entry : cache.entries)
        if (same_code(entry, code))
            return &entry;
    return nullptr;
}

const InviteCardEntry* invite_card_cache_touch(InviteCardCache* cache, Utf8View code, u64 now_ms) noexcept {
    if (!cache || !guild_invite_code_valid(code.data, code.bytes))
        return nullptr;
    if (InviteCardEntry* entry = find(cache, code)) {
        entry->used_ms = now_ms;
        const bool stale = entry->state == InviteCardState::resolved && now_ms - entry->updated_ms >= kInviteCardFreshMs;
        const bool retry = entry->state == InviteCardState::failed && now_ms - entry->updated_ms >= kInviteCardRetryMs;
        if (stale || retry)
            entry->state = InviteCardState::queued; // keeps resolved data visible until replaced
        return entry;
    }
    InviteCardEntry* slot = nullptr;
    for (auto& entry : cache->entries) {
        if (entry.state == InviteCardState::empty) {
            slot = &entry;
            break;
        }
        // Never evict an in-flight request: its completion must find it.
        if (entry.state != InviteCardState::pending && (!slot || entry.used_ms < slot->used_ms))
            slot = &entry;
    }
    if (!slot)
        return nullptr;
    *slot = InviteCardEntry{};
    copy_bytes(slot->code, code.data, code.bytes);
    slot->code_bytes = static_cast<u8>(code.bytes);
    slot->state = InviteCardState::queued;
    slot->used_ms = now_ms;
    ++cache->revision;
    return slot;
}

const InviteCardEntry* invite_card_cache_next(const InviteCardCache& cache) noexcept {
    const InviteCardEntry* next = nullptr;
    for (const auto& entry : cache.entries)
        if (entry.state == InviteCardState::queued && (!next || entry.used_ms < next->used_ms))
            next = &entry;
    return next;
}

bool invite_card_cache_begin(InviteCardCache* cache, Utf8View code) noexcept {
    InviteCardEntry* entry = cache ? find(cache, code) : nullptr;
    if (!entry || entry->state != InviteCardState::queued)
        return false;
    entry->state = InviteCardState::pending;
    return true;
}

bool invite_card_cache_complete(InviteCardCache* cache, Utf8View code, GuildInviteOutcome outcome,
                                const GuildInvite* invite, u64 now_ms) noexcept {
    InviteCardEntry* entry = cache ? find(cache, code) : nullptr;
    if (!entry || entry->state != InviteCardState::pending)
        return false;
    entry->updated_ms = now_ms;
    switch (outcome) {
    case GuildInviteOutcome::resolved:
        if (!invite)
            return false;
        entry->invite = *invite;
        entry->state = InviteCardState::resolved;
        break;
    case GuildInviteOutcome::unknown:
    case GuildInviteOutcome::banned:
        entry->state = InviteCardState::invalid;
        break;
    default:
        // A previously resolved entry keeps its data across a failed refresh.
        entry->state = entry->invite.guild_id ? InviteCardState::resolved : InviteCardState::failed;
        break;
    }
    ++cache->revision;
    return true;
}

void invite_card_cache_requeue(InviteCardCache* cache) noexcept {
    if (!cache)
        return;
    for (auto& entry : cache->entries)
        if (entry.state == InviteCardState::pending)
            entry.state = InviteCardState::queued;
}

void invite_card_cache_clear(InviteCardCache* cache) noexcept {
    if (!cache)
        return;
    const u32 revision = cache->revision + 1u;
    *cache = {};
    cache->revision = revision;
}

bool invite_card_cache_set_joined(InviteCardCache* cache, u32 index, bool joined) noexcept {
    if (!cache || index >= kInviteCardCacheEntries || cache->entries[index].joined == joined)
        return false;
    cache->entries[index].joined = joined;
    ++cache->revision;
    return true;
}

} // namespace maicord
