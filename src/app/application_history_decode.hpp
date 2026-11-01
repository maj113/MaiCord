#pragma once
#include <stddef.h>

#include "account_ui_runtime.hpp"
#include "maicord/app/message_history_sync.hpp"

// REST history/profile decode scratch carved out of AccountUiRuntime. Only the
// callers that decode REST bodies include this; the runtime struct does not need it.
namespace maicord::app_detail {

inline bool application_profile_decode_into(AccountProfileDecodeScratch& scratch, Utf8View body, u64 user,
                                            u64 guild, UserProfile& retained) noexcept {
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
struct HistorySnapshotDecodeBank {
    GatewayMessageAttachment attachments[kMessageHistoryAttachmentCapacity];
    GatewayMessageEmbed embeds[kMessageHistoryEmbedCapacity];
    GatewayMessageEmbedField embed_fields[kMessageHistoryEmbedFieldCapacity];
    GatewayMessageSticker stickers[kMessageHistoryStickerCapacity];
    GatewayMessageComponent components[kMessageHistoryComponentCapacity];
    GatewayMessageNamedUser named_users[kMessageHistoryNamedUserCapacity];
};

struct HistoryDecodeBank {
    GatewayMessageEvent messages[kMessageHistoryRequestLimit];
    GatewayMessageNamedUser named_users[kMessageHistoryNamedUserCapacity];
    u64 member_roles[kMessageHistoryMemberRoleCapacity];
    GatewayMessageReaction reactions[kMessageHistoryReactionCapacity];
    GatewayMessageAttachment attachments[kMessageHistoryAttachmentCapacity];
    GatewayMessageEmbed embeds[kMessageHistoryEmbedCapacity];
    GatewayMessageEmbedField embed_fields[kMessageHistoryEmbedFieldCapacity];
    GatewayMessageSticker stickers[kMessageHistoryStickerCapacity];
    GatewayMessageComponent components[kMessageHistoryComponentCapacity];
    GatewayMessagePollAnswer poll_answers[kMessageHistoryRequestLimit * kGatewayMessagePollMaximumAnswers];
    GatewayMessagePollAnswerCount
        poll_counts[kMessageHistoryRequestLimit * kGatewayMessagePollMaximumAnswers];
    HistorySnapshotDecodeBank snapshot;
};
inline constexpr u32 kHistoryDecodeBaseBytes = offsetof(HistoryDecodeBank, poll_answers);
inline constexpr u32 kHistoryDecodePollBytes = offsetof(HistoryDecodeBank, snapshot);
inline constexpr u32 kHistoryDecodeStorageUnitBytes = alignof(HistoryDecodeBank);
static_assert(sizeof(HistoryDecodeBank) % kHistoryDecodeStorageUnitBytes == 0);
static_assert(kHistoryDecodeBaseBytes % kHistoryDecodeStorageUnitBytes == 0);
inline bool application_history_decode_storage_initialize(AccountUiRuntime& ui) noexcept {
    return stable_storage_initialize(&ui.history_decode_storage,
                                     sizeof(HistoryDecodeBank) / kHistoryDecodeStorageUnitBytes, 0,
                                     kHistoryDecodeStorageUnitBytes, alignof(HistoryDecodeBank));
}

inline void application_history_decode_storage_release(AccountUiRuntime& ui) noexcept {
    stable_storage_release(&ui.history_decode_storage);
}

// Failure leaves the output and all decode/model state unchanged. No bank
// pointer is exposed until the entire typed transaction scratch is writable.
// This is a conservative check, not JSON interpretation. Unicode escapes
// can spell a property name, so any such escape enables the optional region.
// The real decoder still validates the property and its nested structure.
inline u8 application_message_body_optional_fields(Utf8View body) noexcept {
    constexpr auto snapshot_key = utf8_literal("\"message_snapshots\"");
    constexpr auto poll_key = utf8_literal("\"poll\"");
    if (!body.data)
        return 0;
    u8 fields = 0;
    for (u32 i = 0; i < body.bytes; ++i) {
        if (body.data[i] == '\\' && i + 1 < body.bytes && body.data[i + 1] == 'u')
            return 3;
        if (body.data[i] != '"')
            continue;
        if (body.bytes - i >= snapshot_key.bytes &&
            bytes_equal(body.data + i, snapshot_key.data, snapshot_key.bytes))
            fields |= 2;
        if (body.bytes - i >= poll_key.bytes && bytes_equal(body.data + i, poll_key.data, poll_key.bytes))
            fields |= 1;
    }
    return fields;
}

inline bool application_message_decode_scratch(AccountUiRuntime& ui, MessageHistoryDecodeScratch* output,
                                               const Utf8View& body = {}) noexcept {
    const u8 optional = application_message_body_optional_fields(body);
    const bool snapshot = (optional & 2) != 0;
    const bool polls = (optional & 1) != 0;
    const u32 bytes = snapshot ? sizeof(HistoryDecodeBank)
                      : polls  ? kHistoryDecodePollBytes
                               : kHistoryDecodeBaseBytes;
    if (!output ||
        !stable_storage_reserve(&ui.history_decode_storage, bytes / kHistoryDecodeStorageUnitBytes))
        return false;
    auto& bank = *stable_storage_data<HistoryDecodeBank>(ui.history_decode_storage);
    *output = MessageHistoryDecodeScratch{
        {ui.message_history_page_structurals, kMessageHistoryPageStructuralCount},
        {reinterpret_cast<char*>(ui.event_payload_scratch), kGatewayClientDispatchPayloadCapacity},
        {bank.messages, kMessageHistoryRequestLimit},
        {bank.named_users, kMessageHistoryNamedUserCapacity},
        {{bank.reactions, kMessageHistoryReactionCapacity},
         {bank.attachments, kMessageHistoryAttachmentCapacity},
         {bank.embeds, kMessageHistoryEmbedCapacity},
         {bank.embed_fields, kMessageHistoryEmbedFieldCapacity},
         {bank.stickers, kMessageHistoryStickerCapacity},
         {bank.components, kMessageHistoryComponentCapacity},
         {bank.member_roles, kMessageHistoryMemberRoleCapacity},
         snapshot
             ? GatewayMessageSnapshotScratch{{bank.snapshot.attachments, kMessageHistoryAttachmentCapacity},
                                             {bank.snapshot.embeds, kMessageHistoryEmbedCapacity},
                                             {bank.snapshot.embed_fields,
                                              kMessageHistoryEmbedFieldCapacity},
                                             {bank.snapshot.stickers, kMessageHistoryStickerCapacity},
                                             {bank.snapshot.components, kMessageHistoryComponentCapacity},
                                             {bank.snapshot.named_users, kMessageHistoryNamedUserCapacity}}
             : GatewayMessageSnapshotScratch{},
         polls ? GatewayMessagePollScratch{{bank.poll_answers, kMessageHistoryRequestLimit *
                                                                   kGatewayMessagePollMaximumAnswers},
                                           {bank.poll_counts, kMessageHistoryRequestLimit *
                                                                  kGatewayMessagePollMaximumAnswers}}
               : GatewayMessagePollScratch{}},
        64};
    return true;
}

} // namespace maicord::app_detail
