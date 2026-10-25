#include "maicord/app/mls_group.hpp"
#include "mls_wire.hpp"

#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
constexpr u32 kOffsetBits = 20;
constexpr u32 kOffsetMask = (1u << kOffsetBits) - 1;
constexpr u8 kSenderExternal = 2;
constexpr u8 kContentProposal = 2;

u32 entry_offset(const MlsProposalCacheEntry& entry) noexcept {
    return entry.location & kOffsetMask;
}
u32 entry_bytes(const MlsProposalCacheEntry& entry) noexcept {
    return (entry.location >> kOffsetBits) + 1;
}
bool find(const MlsProposalCache& cache, const u8* reference, u32* index) noexcept {
    for (u32 i = 0; i != cache.entry_count; ++i)
        if (mls_equal(cache.entries[i].reference, reference, kMlsHashBytes)) {
            if (index)
                *index = i;
            return true;
        }
    return false;
}
MlsStatus insert(MlsProposalCache& cache, ConstSlice<u8> message, const MlsProposal& proposal) noexcept {
    if (!message.count || message.count > kMlsMaxProposalBytes || proposal.group_id != cache.group_id ||
        proposal.epoch != cache.epoch)
        return MlsStatus::malformed;
    u32 existing = 0;
    if (find(cache, proposal.reference, &existing)) {
        // A redelivery is idempotent only for the exact authenticated message.
        const auto& entry = cache.entries[existing];
        return entry_bytes(entry) == message.count &&
                       mls_equal(cache.storage + entry_offset(entry), message.data, message.count)
                   ? MlsStatus::ok
                   : MlsStatus::crypto_failure;
    }
    if (cache.entry_count >= cache.entry_capacity ||
        message.count > cache.storage_capacity - cache.storage_used)
        return MlsStatus::buffer_too_small;
    auto& entry = cache.entries[cache.entry_count++];
    entry.location = cache.storage_used | ((message.count - 1) << kOffsetBits);
    copy_bytes(entry.reference, proposal.reference, kMlsHashBytes);
    copy_bytes(cache.storage + cache.storage_used, message.data, message.count);
    cache.storage_used += message.count;
    return MlsStatus::ok;
}
void remove(MlsProposalCache& cache, u32 index) noexcept {
    for (u32 i = index; i + 1 < cache.entry_count; ++i)
        cache.entries[i] = cache.entries[i + 1];
    --cache.entry_count;
    secure_clear_bytes(&cache.entries[cache.entry_count], sizeof(MlsProposalCacheEntry));
    u32 used = 0;
    for (u32 i = 0; i != cache.entry_count; ++i)
        used = maximum(used, entry_offset(cache.entries[i]) + entry_bytes(cache.entries[i]));
    if (used < cache.storage_used)
        secure_clear_bytes(cache.storage + used, cache.storage_used - used);
    cache.storage_used = used;
}
MlsStatus parse_entry(KdfSha256Workspace& kdf, const MlsProposalCache& cache, u32 index,
                      MlsProposal* proposal) noexcept {
    const auto& entry = cache.entries[index];
    u32 consumed = 0;
    auto status = mls_proposal_parse(kdf, {cache.storage + entry_offset(entry), entry_bytes(entry)},
                                     nullptr, proposal, &consumed);
    if (status == MlsStatus::ok &&
        (consumed != entry_bytes(entry) || proposal->group_id != cache.group_id ||
         proposal->epoch != cache.epoch || !mls_equal(proposal->reference, entry.reference, kMlsHashBytes)))
        status = MlsStatus::malformed;
    return status;
}
} // namespace

MlsStatus mls_proposal_parse(KdfSha256Workspace& kdf, ConstSlice<u8> input, const MlsExternalSender* sender,
                             MlsProposal* proposal, u32* consumed) noexcept {
    if (!input.data || !proposal || !consumed || !input.count || (sender && !sender->signature_key))
        return MlsStatus::invalid_argument;
    reset_value(proposal);
    *consumed = 0;
    input.count = minimum(input.count, kMlsMaxProposalBytes);
    MlsReader reader{input};
    MlsProposal candidate{};
    const auto version = reader.integer(2);
    const auto format = reader.integer(2);
    const auto group_id = reader.vector();
    candidate.epoch = reader.integer(8);
    const auto sender_type = reader.integer(1);
    const auto sender_index = reader.integer(4);
    const auto authenticated_data = reader.vector();
    const auto content_type = reader.integer(1);
    candidate.type = static_cast<u16>(reader.integer(2));
    if (reader.error != MlsWireError::none || version != 1 || format != 1 ||
        group_id.count != kMlsGroupIdBytes || sender_type != kSenderExternal || sender_index != 0 ||
        authenticated_data.count || content_type != kContentProposal)
        return MlsStatus::malformed;
    candidate.group_id = mls_load_be64(group_id.data);
    if (candidate.type == kMlsProposalAdd) {
        u32 package_bytes = 0;
        if (const auto status =
                mls_key_package_parse(kdf, {input.data + reader.used, input.count - reader.used},
                                      sender != nullptr, &candidate.added, &package_bytes);
            status != MlsStatus::ok)
            return status;
        reader.take(package_bytes);
    } else if (candidate.type == kMlsProposalRemove)
        candidate.removed_leaf = static_cast<u32>(reader.integer(4));
    else
        return MlsStatus::malformed;
    const u32 tbs_bytes = reader.used;
    const auto signature = reader.vector();
    if (reader.error != MlsWireError::none || signature.count < 8 || signature.count > kMlsSignatureMaximum)
        return MlsStatus::malformed;
    if (sender)
        if (const auto status = mls_verify_point(kdf, sender->signature_key, text_bytes("FramedContentTBS"),
                                                 {input.data, tbs_bytes}, {}, signature);
            status != MlsStatus::ok)
            return status;
    // RFC 9420 5.2: the reference hashes AuthenticatedContent, which starts
    // after the MLSMessage protocol version.
    if (!mls_reference_hash(kdf, text_bytes("MLS 1.0 Proposal Reference"),
                            {input.data + 2, reader.used - 2}, {candidate.reference, kMlsHashBytes})
             .ok())
        return MlsStatus::crypto_failure;
    candidate.verified = true;
    *proposal = candidate;
    *consumed = reader.used;
    return MlsStatus::ok;
}

MlsStatus mls_proposal_cache_init(MlsProposalCache* cache, u64 group_id, u64 epoch, Slice<u8> storage,
                                  Slice<MlsProposalCacheEntry> entries) noexcept {
    if (!cache || !storage.data || !entries.data || !storage.count || !entries.count ||
        storage.count > (1u << kOffsetBits))
        return MlsStatus::invalid_argument;
    clear_bytes(storage.data, storage.count);
    clear_bytes(entries.data, entries.count * static_cast<u32>(sizeof(MlsProposalCacheEntry)));
    *cache = {storage.data, entries.data, group_id, epoch, storage.count, 0, entries.count, 0};
    return MlsStatus::ok;
}
void mls_proposal_cache_reset(MlsProposalCache* cache, u64 group_id, u64 epoch) noexcept {
    if (!cache || !cache->storage || !cache->entries)
        return;
    secure_clear_bytes(cache->storage, cache->storage_used);
    secure_clear_bytes(cache->entries, cache->entry_count * sizeof(MlsProposalCacheEntry));
    cache->storage_used = cache->entry_count = 0;
    cache->group_id = group_id;
    cache->epoch = epoch;
}
void mls_proposal_cache_clear(MlsProposalCache* cache) noexcept {
    if (!cache)
        return;
    if (cache->storage)
        secure_clear_bytes(cache->storage, cache->storage_used);
    if (cache->entries)
        secure_clear_bytes(cache->entries, cache->entry_count * sizeof(MlsProposalCacheEntry));
    reset_value(cache);
}
MlsStatus mls_proposal_cache_apply(KdfSha256Workspace& kdf, MlsProposalCache* cache, ConstSlice<u8> input,
                                   const MlsExternalSender& sender, ConstSlice<u64> recognized) noexcept {
    if (!cache || !cache->storage || !cache->entries || !input.data || !sender.signature_key)
        return MlsStatus::invalid_argument;
    // DAVE opcode 27: u8 operation (0 append, 1 revoke) then one vector of
    // proposal messages or of proposal references.
    MlsReader reader{input};
    const auto revoke = reader.integer(1);
    const auto items = reader.vector();
    if (!mls_done(reader) || revoke > 1)
        return MlsStatus::malformed;
    if (revoke) {
        // Every reference must be known and unique before anything is removed.
        MlsReader check{items};
        while (check.error == MlsWireError::none && check.used < items.count) {
            const auto reference = check.vector();
            if (check.error != MlsWireError::none || reference.count != kMlsHashBytes ||
                !find(*cache, reference.data, nullptr))
                return MlsStatus::malformed;
            MlsReader prior{{items.data, check.used - mls_varint_size(kMlsHashBytes) - kMlsHashBytes}};
            while (prior.used < prior.input.count) {
                const auto earlier = prior.vector();
                if (prior.error != MlsWireError::none)
                    return MlsStatus::malformed;
                if (mls_equal(earlier.data, reference.data, kMlsHashBytes))
                    return MlsStatus::malformed;
            }
        }
        if (check.error != MlsWireError::none)
            return MlsStatus::malformed;
        MlsReader apply{items};
        while (apply.used < items.count) {
            const auto reference = apply.vector();
            u32 index = 0;
            if (!find(*cache, reference.data, &index))
                return MlsStatus::malformed;
            remove(*cache, index);
        }
        return MlsStatus::ok;
    }
    const u32 saved_used = cache->storage_used, saved_count = cache->entry_count;
    auto status = MlsStatus::ok;
    for (u32 offset = 0; status == MlsStatus::ok && offset < items.count;) {
        MlsProposal proposal{};
        u32 consumed = 0;
        status = mls_proposal_parse(kdf, {items.data + offset, items.count - offset}, &sender, &proposal,
                                    &consumed);
        if (status != MlsStatus::ok)
            break;
        if (proposal.type == kMlsProposalAdd && recognized.data) {
            bool known = false;
            for (u32 i = 0; i != recognized.count && !known; ++i)
                known = recognized[i] == proposal.added.user_id;
            if (!known) {
                status = MlsStatus::member_removed;
                break;
            }
        }
        status = insert(*cache, {items.data + offset, consumed}, proposal);
        offset += consumed;
    }
    if (status != MlsStatus::ok) {
        secure_clear_bytes(cache->storage + saved_used, cache->storage_used - saved_used);
        secure_clear_bytes(cache->entries + saved_count,
                         (cache->entry_count - saved_count) * sizeof(MlsProposalCacheEntry));
        cache->storage_used = saved_used;
        cache->entry_count = saved_count;
    }
    return status;
}
MlsStatus mls_proposal_cache_resolve(KdfSha256Workspace& kdf, const MlsProposalCache& cache,
                                     ConstSlice<u8> references, Slice<MlsProposal> proposals,
                                     u32* count) noexcept {
    if (!cache.storage || !cache.entries || !count || (!proposals.data && proposals.count))
        return MlsStatus::invalid_argument;
    *count = 0;
    u32 required = 0;
    for (u32 offset = 0;;) {
        const u8* reference = nullptr;
        bool has = false;
        if (const auto status = mls_commit_reference_next(references, &offset, &reference, &has);
            status != MlsStatus::ok)
            return status;
        if (!has)
            break;
        ++required;
    }
    if (required > proposals.count) {
        *count = required;
        return MlsStatus::buffer_too_small;
    }
    u32 used = 0;
    auto status = MlsStatus::ok;
    for (u32 offset = 0; status == MlsStatus::ok;) {
        const u8* reference = nullptr;
        bool has = false;
        if (status = mls_commit_reference_next(references, &offset, &reference, &has);
            status != MlsStatus::ok || !has)
            break;
        u32 index = 0;
        if (!find(cache, reference, &index)) {
            status = MlsStatus::malformed;
            break;
        }
        for (u32 i = 0; i != used; ++i)
            if (mls_equal(proposals[i].reference, reference, kMlsHashBytes))
                status = MlsStatus::malformed;
        if (status == MlsStatus::ok)
            status = parse_entry(kdf, cache, index, &proposals[used]);
        if (status == MlsStatus::ok)
            ++used;
    }
    if (status != MlsStatus::ok) {
        secure_clear_bytes(proposals.data, used * sizeof(MlsProposal));
        return status;
    }
    *count = used;
    return MlsStatus::ok;
}
MlsStatus mls_proposal_cache_resolve_all(KdfSha256Workspace& kdf, const MlsProposalCache& cache,
                                         Slice<MlsProposal> proposals, u32* count) noexcept {
    if (!cache.storage || !cache.entries || !count || (!proposals.data && proposals.count))
        return MlsStatus::invalid_argument;
    *count = cache.entry_count;
    if (proposals.count < cache.entry_count)
        return MlsStatus::buffer_too_small;
    *count = 0;
    for (u32 i = 0; i != cache.entry_count; ++i)
        if (const auto status = parse_entry(kdf, cache, i, &proposals[i]); status != MlsStatus::ok) {
            secure_clear_bytes(proposals.data, i * sizeof(MlsProposal));
            return status;
        }
    *count = cache.entry_count;
    return MlsStatus::ok;
}
} // namespace maicord
