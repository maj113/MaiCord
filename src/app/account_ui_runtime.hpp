#pragma once
#include <stddef.h>

#include "maicord/app/account_paths.hpp"
#include "maicord/app/account_module_api.hpp"
#include "maicord/app/message_composer_controller.hpp"
#include "maicord/app/message_draft_session.hpp"
#include "maicord/app/message_history_sync.hpp"
#include "maicord/app/private_conversation.hpp"
#include "maicord/app/visible_author_hydration.hpp"
#include "maicord/app/message_view_runtime.hpp"
#include "maicord/app/reaction_viewer_controller.hpp"
#include "maicord/model/typing_store.hpp"
#include "maicord/model/voice_state_store.hpp"
#include "maicord/model/thread_member_store.hpp"
#include "maicord/model/read_state.hpp"
#include "maicord/model/message_poll_store.hpp"
#include "maicord/model/guild_detail_store.hpp"
#include "maicord/core/stable_storage.hpp"
#include "maicord/render/d3d11_renderer.hpp"
#include "maicord/ui/profile_media.hpp"
#include "maicord/model/private_channel_store.hpp"
#include "maicord/model/reaction_preferences.hpp"

namespace maicord::app_detail {

inline constexpr u32 kChatStateGuildCapacity = 256;
// Channels, roles and overwrites are bounded by their 16-bit indexes; their
// storage is committed only as accounts use it (see the demand tail below).
inline constexpr u32 kChatStateChannelCapacity = 65'535;
inline constexpr u32 kChatStateRoleCapacity = 65'535;
inline constexpr u32 kChatStateOverwriteCapacity = 65'535;
// Room for 200 servers of 500 emojis, committed as servers bring them.
inline constexpr u32 kChatStateEmojiCapacity = 131'072;
inline constexpr u32 kChatStateEmojiRoleCapacity = 262'144;
// Two banks of guild, channel and role names, committed as they fill.
inline constexpr u32 kChatStateNameBytes = 16 * 1024 * 1024;
// The channel list shows one server (or the conversation list) at a time.
inline constexpr u32 kNavigationChannelRows = 4'096;
inline constexpr u32 kPrivateChannelCapacity = 16'384;
// Descriptions and vanity codes of every guild plus the feature names of
// those the account manages.
inline constexpr u32 kGuildDetailTextBytes = 64 * 1024;
inline constexpr u32 kUserCapacity = 4'096;
inline constexpr u32 kUserNameBytes = 128 * 1024;
inline constexpr u32 kGuildMemberCapacity = 512;
inline constexpr u32 kMemberListViewCapacity = kGuildMemberCapacity + 256;
inline constexpr u32 kMessageHistoryRequestLimit = 25;
inline constexpr u32 kMessageHistoryNamedUserCapacity =
    kMessageHistoryRequestLimit * kGatewayMessageMentionUserMaximum;
inline constexpr u32 kMessageHistoryMemberRoleCapacity =
    kMessageHistoryRequestLimit * kGatewayMessageMemberRoleMaximumIds;
inline constexpr u32 kMessageRichTextCapacity = 256 * 1024;
inline constexpr u32 kMediaAtlasEntryCapacity = 4'096;
inline constexpr u32 kMediaAtlasRowCapacity = 64;
inline constexpr u32 kMessageDraftCapacity = 64;
inline constexpr u32 kMessageDraftTextCapacity = 64 * 1024;
inline constexpr u32 kMessageRequestTextCapacity = 64 * 1024;

// Identity media has a 16 MiB budget. The lazy preview texture has 64 MiB
// capacity, enough for 2048-pixel PNGs plus atlas padding independently.
// Both textures start small and grow by whole steps: 4 MiB + 2 MiB
// steps for identity media, 8 MiB + 8 MiB steps for previews and call video.
inline constexpr u16 kMediaAtlasExtent = 2'048;
inline constexpr u16 kPreviewMediaAtlasExtent = 4'096;
inline constexpr u16 kMediaAtlasInitialHeight = 512;
inline constexpr u16 kMediaAtlasGrowthStep = 256;
inline constexpr u16 kPreviewMediaAtlasInitialHeight = 512;
inline constexpr u16 kPreviewMediaAtlasGrowthStep = 512;
inline constexpr u32 kPreviewMediaAtlasEntryCapacity = 512;
inline constexpr u32 kPreviewMediaAtlasRowCapacity = 128;
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
inline constexpr u32 kMessageHistoryPageStructuralCount = 64 * 1024;
inline constexpr u32 kMessageHistoryObjectStructuralCount = 4 * 1024;
inline constexpr u32 kMessageTextByteStyleOffset = 0;
inline constexpr u32 kRendererDecorationOffset =
    kMessageTextByteStyleOffset + kMessageTextByteStyleScratchCapacity * sizeof(u32);
inline constexpr u32 kRendererHitSpanOffset =
    (kRendererDecorationOffset + kRendererTextDecorationCapacity * sizeof(TextDecoration) + 7u) & ~7u;
inline constexpr u32 kRendererHitRegionOffset =
    kRendererHitSpanOffset + kTextHitSpanMaximum * sizeof(TextHitSpan);
inline constexpr u32 kRendererTransientScratchBytes =
    kRendererHitRegionOffset + kRendererContentHitRegionCapacity * sizeof(TextHitRegion);

// One ordinary Gateway record, or (between pops) the renderer's transient
// text scratch. Oversized guild records use the pump's on-demand reservation.
inline constexpr u32 kAccountUiEventScratchBytes =
    ((kRendererTransientScratchBytes > kGatewayClientDispatchPayloadCapacity ? kRendererTransientScratchBytes
                                                                              : kGatewayClientDispatchPayloadCapacity) +
     4095u) & ~4095u;
static_assert(kAccountUiEventScratchBytes >= kGatewayClientDispatchPayloadCapacity &&
              kAccountUiEventScratchBytes >= kRendererTransientScratchBytes);

struct AccountProjectionScratch {
    u32 member_list[kGuildMemberCapacity];
    u16 navigation_channels[kNavigationChannelRows];
};

// How much of the demand tail of AccountUiRuntime is committed.
struct AccountDemandCommit {
    u32 channel_slots;
    u32 name_bytes[2];
    u32 emoji_records;
    u32 emoji_roles;
    u32 unused;
};

static_assert(sizeof(AccountProjectionScratch) == 10'240);

struct AccountTextShapeScratch {
    wchar_t utf16[kRendererContentTextUnitCapacity];
    TextGlyph glyphs[kRendererContentTextUnitCapacity];
    TextRun runs[kRendererContentRunCapacity];
    u16 clusters[kRendererContentTextUnitCapacity];
};

union AccountResponseShapeScratch {
    AccountTextShapeScratch shape;
    u8 response[kAccountHistoryResponseBytes];
};
static_assert(sizeof(AccountTextShapeScratch) <= kAccountHistoryResponseBytes);

struct AccountProfileDecodeScratch {
    u32 structurals[16 * 1024];
    UserProfile temporary;

    bool decode_into(Utf8View body, u64 user, u64 guild, UserProfile& retained) noexcept {
        if (!user_profile_decode(body, {structurals, 16 * 1024}, user, guild, &temporary))
            return false;
        retained = temporary;
        return true;
    }
};
static_assert(sizeof(AccountProfileDecodeScratch) <= kMessageHistoryPageStructuralCount * sizeof(u32));

// UI-thread-only REST transaction storage. Reserve its address with the account,
// commit on the first decode, and retain it until account teardown.
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

struct AccountUiRuntime {
    VisibleAuthorHydration author_hydration;
    PrivateConversationRequest private_conversation;
    ChatStateStore chat_state;
    GuildDetailStore guild_details;
    ReadStateStore read_state_store;
    StableStorage read_state_bank;
    StableStorage role_storage;
    StableStorage overwrite_storage;
    AccountDemandCommit demand_commit;
    PrivateChannelStore private_channel_store;
    UserStore user_store;
    GuildMemberStore guild_member_store;
    ThreadMemberStore thread_member_store;
    StableStorage thread_member_banks[2];
    TypingStore typing_store;
    VoiceStateStore voice_states;
    GatewayClientVoiceSnapshot voice_session;
    MemberListView member_list;
    NavigationView navigation_view;
    MessageTextPresentation message_text;
    UiTextMeasureCache text_measure_cache;
    RendererTextStorage renderer_text;
    MessageRichStore message_rich_store;
    MessagePollStore message_poll_store;
    MessageScenePollBinding message_poll_binding;
    u64 poll_gateway_serial;
    u64 message_history_poll_serial;
    u64 poll_source_sequences[8];
    u8 poll_source_sequence_mask;
    MediaAtlas media_atlas;
    MediaAtlas preview_media_atlas;
    MessageDraftStore message_drafts;
    ReactionPreferenceStore reaction_preferences;
    MessageComposerController message_composer_controller;
    GuildRecord guild_record_storage[kChatStateGuildCapacity];
    GuildDetailRecord guild_detail_records[kChatStateGuildCapacity];
    char guild_detail_text[kGuildDetailTextBytes];
    UserRecord user_record_storage[kUserCapacity];
    GuildMemberRecord guild_member_record_storage[kGuildMemberCapacity];
    TypingRecord typing_record_storage[kTypingRecordCapacity];
    VoiceState voice_state_records[kVoiceStateCapacity];
    u16 guild_order_storage[kChatStateGuildCapacity];
    u32 user_order_storage[kUserCapacity];
    u32 guild_member_order_storage[kGuildMemberCapacity];
    MemberListRow member_list_rows[kMemberListViewCapacity];
    // Bound to the member list scene while this account is presented.
    MemberMediaCache member_media_cache;
    // Folder headers share the rail with guilds, at most one per guild.
    GuildViewRow navigation_guild_rows[kChatStateGuildCapacity * 2];
    ChannelViewRow navigation_channel_rows[kNavigationChannelRows];
    GlyphQuad renderer_text_quads[kRendererContentGlyphCapacity];
    GlyphDrawBatch renderer_text_batches[kRendererContentBatchCapacity];
    MessageTextCommand message_text_commands[kMaximumMessageTextCommands];
    char message_text_utf8[kMaximumMessageTextUtf8Bytes];
    TextStyleSpan message_text_styles[kMaximumMessageTextStyleSpans];
    MessageMarkdownSemantic message_text_semantics[kMaximumMessageTextSemantics];
    MessageTextMeasurement message_text_measurements[kMaximumMessageTextMeasurements];
    UiTextMeasureRecord message_text_measure_cache_records[kMaximumMessageTextMeasureCacheRecords];
    MessageTextSpoilerReveal message_text_spoiler_reveals[kMaximumMessageTextSpoilerReveals];
    MessageTextSemantic renderer_content_semantics[kRendererContentSemanticCapacity];
    TextHitRegion renderer_content_hit_regions[kRendererContentHitRegionCapacity];
    u8 user_name_storage[kUserNameBytes];
    alignas(8) u8 event_payload_scratch[kAccountUiEventScratchBytes];
    // READY guild IDs and bulk-delete IDs are alternative event payloads.
    union {
        u64 guild_id_scratch[kChatStateGuildCapacity];
        u64 message_bulk_ids[kGatewayBulkDeleteMaximumIds];
    };
    // REST decoding/application completes before presentation begins. Shaping
    // releases its font runs before returning; only quads and hit regions persist.
    AccountResponseShapeScratch response_shape_scratch;
    // UI-thread-only transaction scratch. Navigation keys are rebuilt before
    // use; history parsing and profile commits finish before presentation.
    union {
        ChannelSortKey navigation_sort_keys[kNavigationChannelRows];
        u32 message_history_page_structurals[kMessageHistoryPageStructuralCount];
        AccountProfileDecodeScratch profile_decode;
    };
    // Layout builders and history object decoding are UI-thread-only and
    // consume their scratch before committing retained rows or messages.
    union {
        AccountProjectionScratch projection_scratch;
        u32 message_history_object_structurals[kMessageHistoryObjectStructuralCount];
    };
    StableStorage history_decode_storage;
    StableStorage message_rich_record_storage;
    u16 message_rich_order[kMessageViewItemCapacity];
    u8 message_rich_text[kMessageRichTextCapacity];
    MediaAtlasEntry media_atlas_entries[kMediaAtlasEntryCapacity];
    u32 media_atlas_buckets[kMediaAtlasEntryCapacity * 2];
    GlyphAtlasEntry media_atlas_allocations[kMediaAtlasEntryCapacity];
    GlyphAtlasRow media_atlas_rows[kMediaAtlasRowCapacity];
    MediaAtlasEntry preview_media_atlas_entries[kPreviewMediaAtlasEntryCapacity];
    u32 preview_media_atlas_buckets[kPreviewMediaAtlasEntryCapacity * 2];
    GlyphAtlasEntry preview_media_atlas_allocations[kPreviewMediaAtlasEntryCapacity];
    GlyphAtlasRow preview_media_atlas_rows[kPreviewMediaAtlasRowCapacity];
    MessageDraftRecord message_draft_records[kMessageDraftCapacity];
    u8 message_draft_text[kMessageDraftTextCapacity];
    u8 message_request_text[kMessageRequestTextCapacity];
    MessageDraftSession message_draft_session;
    u64 active_channel_id;
    u64 message_history_request_id;
    u32 observed_selection_revision;
    u32 rendered_navigation_revision;
    u32 rendered_message_created;
    u32 rendered_message_updated;
    u32 rendered_message_deleted;
    u32 rendered_user_revision;
    u32 rendered_member_revision;
    u32 rendered_private_revision;
    u32 rendered_emoji_mutations;
    u32 rendered_typing_revision;
    u32 rendered_permission_mutations;
    u32 rendered_composer_revision;
    u64 rendered_channel_id;
    u64 text_rebuild_retry_at_ms;
    u32 text_rebuild_failures;
    u32 rendered_poll_revision;
    MessageMarkdownCache message_markdown_cache;
    MessageMarkdownCacheEntry message_markdown_cache_entries[kMessageMarkdownCacheSlots];
    alignas(8) u8 message_markdown_cache_arena[kMessageMarkdownCacheArenaBytes];
    // Demand tail: the host reserves it and commits ranges as the stores grow
    // (application_ui_runtime.cpp), so an account pays only for what it uses.
    // Fully committed allocations, such as test owners, may use it directly.
    ChannelRecord channel_record_storage[kChatStateChannelCapacity];
    u16 channel_order_storage[kChatStateChannelCapacity];
    // Indexed by channel slot; committed with the channel records.
    u8 navigation_visibility[kChatStateChannelCapacity];
    u8 chat_state_name_storage[kChatStateNameBytes];
    CustomEmojiRecord emoji_record_storage[kChatStateEmojiCapacity];
    u64 emoji_role_storage[kChatStateEmojiRoleCapacity];
};

// Includes fixed-capacity identity, member, message, glyph and media stores.
// Permission, history decode and rich records are independently demand-committed
// and are not part of this committed allocation.
inline constexpr u32 kAccountUiMemberPagingBytes =
    sizeof(GuildMemberStore) - 48 + sizeof(MemberListView) - 80 +
    kMemberListViewCapacity * (sizeof(MemberListRow) - 32);
inline constexpr u32 kAccountUiThreadMemberBytes =
    sizeof(ThreadMemberStore) + 2 * sizeof(StableStorage);
// Known-channel read metadata shares the channel records (sizeof(ChannelRecord)
// - 96 bytes each, committed with them in the demand tail). Only late channel
// identities use separately demand-committed pending storage.
inline constexpr u32 kAccountUiReadStateBytes =
    sizeof(ReadStateStore*) + sizeof(ReadStateStore) + sizeof(StableStorage);
static_assert(kAccountUiReadStateBytes == 152);
// The current reply reuses the composer's channel; its remaining snapshot
// fields occupy 16 bytes, plus eight added bytes in each retained draft record.
inline constexpr u32 kAccountUiReplyDraftBytes = kMessageDraftCapacity * (sizeof(MessageDraftRecord) - 40) +
                                                 sizeof(MessageComposerReplySnapshot) - sizeof(u64);
static_assert(kAccountUiReplyDraftBytes == 528);
// The Markdown cache: its presentation binding (16 bytes), slots and ring.
inline constexpr u32 kAccountUiMarkdownCacheBytes = 16 + sizeof(MessageMarkdownCache) +
                                                    kMessageMarkdownCacheSlots * sizeof(MessageMarkdownCacheEntry) +
                                                    kMessageMarkdownCacheArenaBytes;
// The committed owner ends where the demand tail begins; growth of the lazy decode bank does not
// change it.
inline constexpr usize kAccountUiFixedBytes = offsetof(AccountUiRuntime, channel_record_storage);
// The literal base predates these members: a text measurement of 808 bytes, a guild record of 120,
// appearance and activity stores of two StableStorage + 16 each, a 288-byte chat state store, and
// the channel, name, emoji and emoji-role storage that has since moved to the demand tail.
static_assert(kAccountUiFixedBytes ==
              4'261'168 - 128 * 1024 + kAccountUiEventScratchBytes + kAccountUiMarkdownCacheBytes +
                  sizeof(MemberMediaCache) +
                  kMaximumMessageTextMeasurements * (sizeof(MessageTextMeasurement) - 808) +
                  kChatStateGuildCapacity * (sizeof(GuildRecord) - 120) +
                  (sizeof(UserAppearanceStore) - 2 * sizeof(StableStorage) - 16) +
                  (sizeof(UserActivityStore) - 2 * sizeof(StableStorage) - 16) +
                  sizeof(MemberListPositionTable) + sizeof(GuildDetailStore) +
                  kChatStateGuildCapacity * sizeof(GuildDetailRecord) + kGuildDetailTextBytes -
                  4'096 * (sizeof(ChannelRecord) + sizeof(u16) + sizeof(CustomEmojiRecord)) - 256 * 1024 -
                  16'384 * sizeof(u64) + sizeof(AccountDemandCommit) + (sizeof(ChatStateStore) - 288));

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
inline bool application_message_body_may_have_snapshot(Utf8View body) noexcept {
    return (application_message_body_optional_fields(body) & 2) != 0;
}

inline bool application_message_decode_scratch(AccountUiRuntime& ui, MessageHistoryDecodeScratch* output,
                                               Utf8View body = {}) noexcept {
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
        {ui.message_history_object_structurals, kMessageHistoryObjectStructuralCount},
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
