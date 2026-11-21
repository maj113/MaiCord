#pragma once

#include "../../account/account_ui_runtime.hpp"
#include "maicord/app/chat/messages/history_sync.hpp"
#include "maicord/app/net/rest_client.hpp"
#include "maicord/core/intrinsics.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "maicord/sys/simd.hpp"

// REST history/profile decode scratch carved out of AccountUiRuntime. Only the
// callers that decode REST bodies include this; the runtime struct does not need it.
namespace maicord::app_detail {

inline bool application_profile_decode_into(AccountProfileDecodeScratch& scratch, const Utf8View& body,
                                            u64 user, u64 guild, UserProfile& retained) noexcept {
    if (!user_profile_decode(body, {scratch.structurals, 16 * 1024}, user, guild, &scratch.temporary))
        return false;
    retained = scratch.temporary;
    return true;
}

inline constexpr u32 kMessageHistoryNamedUserCapacity =
    kMessageHistoryRequestLimit * kGatewayMessageMentionUserMaximum;
// Shared by a page; an author whose roles no longer fit is left unknown.
inline constexpr u32 kMessageHistoryMemberRoleCapacity = kMessageHistoryRequestLimit * 100;
inline constexpr u32 kMessageHistoryReactionCapacity =
    kMessageHistoryRequestLimit * kGatewayMessageReactionMaximum;
inline constexpr u32 kMessageHistoryAttachmentCapacity =
    kMessageHistoryRequestLimit * kGatewayMessageAttachmentMaximum;
inline constexpr u32 kMessageHistoryEmbedCapacity =
    kMessageHistoryRequestLimit * kGatewayMessageEmbedMaximum;
inline constexpr u32 kMessageHistoryEmbedFieldCapacity =
    kMessageHistoryRequestLimit * kGatewayMessageEmbedFieldMaximum;
inline constexpr u32 kMessageHistoryStickerCapacity =
    kMessageHistoryRequestLimit * kGatewayMessageStickerMaximum;
inline constexpr u32 kMessageHistoryComponentCapacity =
    kMessageHistoryRequestLimit * kGatewayMessageComponentMaximum;
// The largest page body, the cap that account_module_history_submit asks for: 25 messages at Discord's limits (4,000
// characters of content, 6,000 of embeds, 10 attachments, and the same again for a replied message) are about 80 KB
// each.
inline constexpr u32 kMessageHistoryMaximumBodyBytes = kRestClientMaximumResponseBytes;
// A page that outgrows the shared index and text arena decodes in the storage's own: an index of one entry for each
// two body bytes and text of the body's size (decoded text is never longer than its JSON).
inline constexpr u32 kMessageHistoryLargeStructuralCapacity = kMessageHistoryMaximumBodyBytes / 2 + 1;
// The decode bank's pools, each in its own page-aligned region of HistoryDecodeStorage's reservation.
enum class HistoryDecodePool : u32 {
    messages,
    named_users,
    member_roles,
    reactions,
    attachments,
    embeds,
    embed_fields,
    stickers,
    components,
    poll_answers,
    poll_counts,
    snapshot_attachments,
    snapshot_embeds,
    snapshot_embed_fields,
    snapshot_stickers,
    snapshot_components,
    snapshot_named_users,
    structurals,
    text,
    count,
};
static_assert(static_cast<u32>(HistoryDecodePool::count) == kHistoryDecodePoolCount);
inline constexpr u32 kHistoryDecodePageBytes = 4096;
struct HistoryDecodePoolShape {
    u32 element_bytes;
    u32 capacity;
};
inline constexpr HistoryDecodePoolShape kHistoryDecodePools[kHistoryDecodePoolCount] = {
    {sizeof(GatewayMessageEvent), kMessageHistoryRequestLimit},
    {sizeof(GatewayMessageNamedUser), kMessageHistoryNamedUserCapacity},
    {sizeof(u64), kMessageHistoryMemberRoleCapacity},
    {sizeof(GatewayMessageReaction), kMessageHistoryReactionCapacity},
    {sizeof(GatewayMessageAttachment), kMessageHistoryAttachmentCapacity},
    {sizeof(GatewayMessageEmbed), kMessageHistoryEmbedCapacity},
    {sizeof(GatewayMessageEmbedField), kMessageHistoryEmbedFieldCapacity},
    {sizeof(GatewayMessageSticker), kMessageHistoryStickerCapacity},
    {sizeof(GatewayMessageComponent), kMessageHistoryComponentCapacity},
    {sizeof(GatewayMessagePollAnswer), kMessageHistoryRequestLimit * kGatewayMessagePollMaximumAnswers},
    {sizeof(GatewayMessagePollAnswerCount), kMessageHistoryRequestLimit * kGatewayMessagePollMaximumAnswers},
    {sizeof(GatewayMessageAttachment), kMessageHistoryAttachmentCapacity},
    {sizeof(GatewayMessageEmbed), kMessageHistoryEmbedCapacity},
    {sizeof(GatewayMessageEmbedField), kMessageHistoryEmbedFieldCapacity},
    {sizeof(GatewayMessageSticker), kMessageHistoryStickerCapacity},
    {sizeof(GatewayMessageComponent), kMessageHistoryComponentCapacity},
    {sizeof(GatewayMessageNamedUser), kMessageHistoryNamedUserCapacity},
    {sizeof(u32), kMessageHistoryLargeStructuralCapacity},
    {1, kMessageHistoryMaximumBodyBytes},
};
// The reserved bytes of a pool: its full capacity, rounded up to whole pages.
constexpr u32 history_decode_pool_bytes(u32 pool) noexcept {
    return (kHistoryDecodePools[pool].element_bytes * kHistoryDecodePools[pool].capacity + kHistoryDecodePageBytes - 1) &
           ~(kHistoryDecodePageBytes - 1);
}
constexpr u32 history_decode_pool_offset(u32 pool) noexcept {
    u32 offset = 0;
    for (u32 index = 0; index < pool; ++index)
        offset += history_decode_pool_bytes(index);
    return offset;
}
inline constexpr u32 kHistoryDecodeReservedBytes = history_decode_pool_offset(kHistoryDecodePoolCount);

inline bool application_history_decode_storage_initialize(AccountUiRuntime& ui) noexcept {
    HistoryDecodeStorage& storage = ui.history_decode_storage;
    if (storage.base)
        return false;
    auto* base = static_cast<u8*>(pages_reserve(kHistoryDecodeReservedBytes));
    if (!base)
        return false;
    reset_value(&storage);
    storage.base = base;
    arena_account_add(ArenaTag::storage, 1, kHistoryDecodeReservedBytes, 0);
    return true;
}

inline void application_history_decode_storage_release(AccountUiRuntime& ui) noexcept {
    HistoryDecodeStorage& storage = ui.history_decode_storage;
    if (storage.base) {
        pages_release(storage.base, kHistoryDecodeReservedBytes);
        i64 committed = 0;
        for (const u32 bytes : storage.committed)
            committed += bytes;
        arena_account_add(ArenaTag::storage, -1, -i64{kHistoryDecodeReservedBytes}, -committed);
    }
    reset_value(&storage);
}

// Commits `pool` through `elements` elements (held to its capacity) in whole pages. The elements its committed pages
// hold, or 0 when the system refuses the commit.
inline u32 history_decode_pool_reserve(HistoryDecodeStorage& storage, u32 pool, u32 elements) noexcept {
    const HistoryDecodePoolShape shape = kHistoryDecodePools[pool];
    const u32 wanted = minimum(elements, shape.capacity) * shape.element_bytes;
    const u32 target = (wanted + kHistoryDecodePageBytes - 1) & ~(kHistoryDecodePageBytes - 1);
    u32& committed = storage.committed[pool];
    if (target > committed) {
        if (!pages_commit(storage.base + history_decode_pool_offset(pool) + committed, target - committed))
            return 0;
        arena_account_add(ArenaTag::storage, 0, 0, target - committed);
        committed = target;
    }
    return minimum(shape.capacity, committed / shape.element_bytes);
}

// What admission needs of a body: its '{' count and its optional fields (1 polls, 2 message snapshots). This is a
// conservative check, not JSON interpretation. Unicode escapes can spell a property name, so any such escape enables
// both optional regions. The real decoder still validates the property and its nested structure.
struct HistoryDecodeBodyShape {
    u32 objects;
    u8 optional;
};
inline HistoryDecodeBodyShape application_message_body_shape(Utf8View body) noexcept {
    constexpr auto snapshot_key = utf8_literal("\"message_snapshots\"");
    constexpr auto poll_key = utf8_literal("\"poll\"");
    HistoryDecodeBodyShape shape{};
    if (!body.data)
        return shape;
    const auto equal = [](const char* bytes, char value) noexcept {
        return static_cast<u32>(_mm_movemask_epi8(
            _mm_cmpeq_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i_u*>(bytes)), _mm_set1_epi8(value))));
    };
    // 16 bytes for each step. A step also reads the byte after them, so the last step reads a copy of the end
    // of the body with zero bytes after it.
    char end[32]{};
    for (u32 i = 0; i < body.bytes; i += 16) {
        const char* block = body.data + i;
        if (body.bytes - i < 17) {
            copy_bytes(end, block, body.bytes - i);
            block = end;
        }
        shape.objects += population_count(equal(block, '{'));
        if (equal(block, '\\') & equal(block + 1, 'u'))
            shape.optional = 3;
        // Only a quote before 'm' or 'p' can start one of the two keys.
        for (u32 keys = equal(block, '"') & (equal(block + 1, 'm') | equal(block + 1, 'p')); keys; keys &= keys - 1) {
            const u32 at = i + lowest_set_bit(keys);
            const bool poll = body.data[at + 1] == 'p';
            const Utf8View key = poll ? poll_key : snapshot_key;
            if (body.bytes - at >= key.bytes && bytes_equal(body.data + at, key.data, key.bytes))
                shape.optional |= poll ? 1 : 2;
        }
    }
    return shape;
}

// The first `counts[pool]` elements of a pool, or an empty slice when the pool was not admitted.
template <typename T> Slice<T> history_decode_pool_slice(const HistoryDecodeStorage& storage, const u32* counts,
                                                         HistoryDecodePool pool) noexcept {
    const u32 index = static_cast<u32>(pool);
    return {counts[index] ? reinterpret_cast<T*>(storage.base + history_decode_pool_offset(index)) : nullptr,
            counts[index]};
}

// Failure leaves the output and all decode/model state unchanged; no pool pointer is exposed until every pool the body
// needs is writable. Every pooled element but a member role is decoded from a JSON object of its own, so the body's
// '{' count bounds each pool (plus one element a decoder may write before it rejects a value); role IDs are strings,
// so that pool commits whole. Each slice ends at its committed elements: a decode cannot reach an uncommitted page.
// A body larger than the shared index and text arena gets the storage's large index and text until
// application_message_decode_release_large.
inline bool application_message_decode_scratch(AccountUiRuntime& ui, MessageHistoryDecodeScratch* output,
                                               const Utf8View& body = {}) noexcept {
    HistoryDecodeStorage& storage = ui.history_decode_storage;
    if (!storage.base)
        return false;
    const HistoryDecodeBodyShape shape = application_message_body_shape(body);
    const bool snapshot = (shape.optional & 2) != 0;
    const bool polls = (shape.optional & 1) != 0;
    const u32 bound = shape.objects + 1;
    u32 counts[kHistoryDecodePoolCount]{};
    for (u32 pool = 0; pool < static_cast<u32>(HistoryDecodePool::structurals); ++pool) {
        const bool needed = pool >= static_cast<u32>(HistoryDecodePool::snapshot_attachments) ? snapshot
                            : pool >= static_cast<u32>(HistoryDecodePool::poll_answers)    ? polls
                                                                                            : true;
        if (!needed)
            continue;
        counts[pool] = history_decode_pool_reserve(
            storage, pool, pool == static_cast<u32>(HistoryDecodePool::member_roles) ? kMessageHistoryMemberRoleCapacity
                                                                                      : bound);
        if (!counts[pool])
            return false;
    }
    Slice<u32> structurals{ui.message_history_page_structurals, kMessageHistoryPageStructuralCount};
    Slice<char> text{reinterpret_cast<char*>(ui.event_payload_scratch), kGatewayClientDispatchPayloadCapacity};
    const u32 entries = body.bytes / 2 + 1;
    if (entries > structurals.count || body.bytes > text.count) {
        constexpr u32 index_pool = static_cast<u32>(HistoryDecodePool::structurals);
        constexpr u32 text_pool = static_cast<u32>(HistoryDecodePool::text);
        if (history_decode_pool_reserve(storage, index_pool, entries) < entries ||
            history_decode_pool_reserve(storage, text_pool, body.bytes) < body.bytes)
            return false;
        structurals = {reinterpret_cast<u32*>(storage.base + history_decode_pool_offset(index_pool)),
                       storage.committed[index_pool] / u32{sizeof(u32)}};
        text = {reinterpret_cast<char*>(storage.base + history_decode_pool_offset(text_pool)),
                storage.committed[text_pool]};
    }
    using P = HistoryDecodePool;
    const HistoryDecodeStorage& s = storage;
    *output = MessageHistoryDecodeScratch{
        structurals,
        text,
        history_decode_pool_slice<GatewayMessageEvent>(s, counts, P::messages),
        history_decode_pool_slice<GatewayMessageNamedUser>(s, counts, P::named_users),
        {history_decode_pool_slice<GatewayMessageReaction>(s, counts, P::reactions),
         history_decode_pool_slice<GatewayMessageAttachment>(s, counts, P::attachments),
         history_decode_pool_slice<GatewayMessageEmbed>(s, counts, P::embeds),
         history_decode_pool_slice<GatewayMessageEmbedField>(s, counts, P::embed_fields),
         history_decode_pool_slice<GatewayMessageSticker>(s, counts, P::stickers),
         history_decode_pool_slice<GatewayMessageComponent>(s, counts, P::components),
         history_decode_pool_slice<u64>(s, counts, P::member_roles),
         {history_decode_pool_slice<GatewayMessageAttachment>(s, counts, P::snapshot_attachments),
          history_decode_pool_slice<GatewayMessageEmbed>(s, counts, P::snapshot_embeds),
          history_decode_pool_slice<GatewayMessageEmbedField>(s, counts, P::snapshot_embed_fields),
          history_decode_pool_slice<GatewayMessageSticker>(s, counts, P::snapshot_stickers),
          history_decode_pool_slice<GatewayMessageComponent>(s, counts, P::snapshot_components),
          history_decode_pool_slice<GatewayMessageNamedUser>(s, counts, P::snapshot_named_users)},
         {history_decode_pool_slice<GatewayMessagePollAnswer>(s, counts, P::poll_answers),
          history_decode_pool_slice<GatewayMessagePollAnswerCount>(s, counts, P::poll_counts)}},
        64};
    return true;
}

// Decommits the large index and text that a long body committed. Call it when the body's decode, and every copy of
// its text, are done.
inline void application_message_decode_release_large(AccountUiRuntime& ui) noexcept {
    HistoryDecodeStorage& storage = ui.history_decode_storage;
    for (u32 pool = static_cast<u32>(HistoryDecodePool::structurals); pool < kHistoryDecodePoolCount; ++pool) {
        const u32 bytes = storage.committed[pool];
        if (!bytes)
            continue;
        pages_decommit(storage.base + history_decode_pool_offset(pool), bytes);
        arena_account_add(ArenaTag::storage, 0, 0, -i64{bytes});
        storage.committed[pool] = 0;
    }
}

// Publishes the polls of one decoded history page. A poll that the Gateway changed after the request stays.
// The message pool bounds a page to kMessageHistoryRequestLimit messages.
inline bool application_message_history_publish_polls(AccountUiRuntime& ui,
                                                      ConstSlice<GatewayMessageEvent> messages) noexcept {
    MessagePollPageInput inputs[kMessageHistoryRequestLimit];
    MessagePollResultTicket tickets[kMessageHistoryRequestLimit];
    u32 polls = 0;
    for (u32 index = 0; index < messages.count && polls < kMessageHistoryRequestLimit; ++index) {
        const GatewayMessageEvent& message = messages[index];
        if (!(message.fields & gateway_message_field_poll))
            continue;
        const auto* existing = message_poll_store_find(ui.message_poll_store, message.id);
        if (existing && existing->last_gateway_serial > ui.message_history_poll_serial)
            continue;
        tickets[polls] =
            message_poll_store_result_ticket(ui.message_poll_store, message.id, ui.message_history_poll_serial);
        inputs[polls] = {message.id, message.channel_id, &message.poll, 0, &tickets[polls]};
        ++polls;
    }
    return message_poll_store_apply_page(&ui.message_poll_store, {inputs, polls}).ok();
}

} // namespace maicord::app_detail
