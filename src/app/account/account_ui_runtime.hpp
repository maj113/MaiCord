#pragma once
#include <stddef.h>

#include "maicord/app/account/account_module_api.hpp"
#include "maicord/app/gateway/client_limits.hpp"
#include "maicord/app/gateway/client_voice.hpp"
#include "maicord/app/chat/composer/message_composer_controller.hpp"
#include "maicord/app/chat/composer/message_draft_session.hpp"
#include "maicord/app/chat/private_conversation.hpp"
#include "maicord/model/guild/member_store.hpp"
#include "maicord/model/user/store.hpp"
#include "maicord/app/chat/visible_author_hydration.hpp"
#include "maicord/app/chat/messages/view_runtime.hpp"
#include "maicord/model/chat/typing_store.hpp"
#include "maicord/model/voice/state_store.hpp"
#include "maicord/model/guild/thread_member_store.hpp"
#include "maicord/model/chat/read_state.hpp"
#include "maicord/model/chat/message_poll_store.hpp"
#include "maicord/model/guild/detail_store.hpp"
#include "maicord/core/stable_storage.hpp"
#include "maicord/render/text/renderer_text_storage.hpp"
#include "maicord/ui/account/profile_media.hpp"
#include "maicord/model/user/private_channel_store.hpp"
#include "maicord/model/chat/reaction_preferences.hpp"
#include "maicord/ui/shell/navigation_view.hpp"
#include "maicord/model/guild/member_list.hpp"
#include "maicord/render/media/atlas.hpp"
#include "maicord/render/text/glyph_atlas.hpp"
#include "maicord/ui/chat/messages/text_presentation.hpp"
#include "maicord/core/base.hpp"

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
inline constexpr u32 kMessageRichTextCapacity = 256 * 1024;
inline constexpr u32 kMediaAtlasEntryCapacity = 4'096;
// Shelves of the general region plus the profile pools' slot rows.
inline constexpr u32 kMediaAtlasRowCapacity = 128;
inline constexpr u32 kMessageDraftCapacity = 64;
inline constexpr u32 kMessageDraftTextCapacity = 64 * 1024;
inline constexpr u32 kMessageRequestTextCapacity = 64 * 1024;

// The REST message decode bank (chat/messages/history_decode.hpp): one reservation of separately page-aligned pools,
// each committed only as far as a body can reach.
inline constexpr u32 kHistoryDecodePoolCount = 17;
struct HistoryDecodeStorage {
    u8* base;
    // Committed bytes of each pool, page multiples from its start.
    u32 committed[kHistoryDecodePoolCount];
};

// The media atlases' geometry and memory policy are the renderer's (MediaAtlasSurface in render/media/atlas.hpp).
inline constexpr u32 kPreviewMediaAtlasEntryCapacity = 512;
inline constexpr u32 kPreviewMediaAtlasRowCapacity = 128;
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
};

static_assert(sizeof(AccountProjectionScratch) == 10'240);

struct AccountTextShapeScratch {
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
};
static_assert(sizeof(AccountProfileDecodeScratch) <= kMessageHistoryPageStructuralCount * sizeof(u32));

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
    u64 poll_sequence;
    bool poll_sequence_known;
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
    // use; history parsing, Gateway decoding and profile commits finish before
    // presentation.
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
    HistoryDecodeStorage history_decode_storage;
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
    // (app/shell/ui_runtime.cpp), so an account pays only for what it uses.
    // Fully committed allocations, such as test owners, may use it directly.
    ChannelRecord channel_record_storage[kChatStateChannelCapacity];
    u16 channel_order_storage[kChatStateChannelCapacity];
    // Indexed by channel slot; committed with the channel records.
    u8 navigation_visibility[kChatStateChannelCapacity];
    u8 chat_state_name_storage[kChatStateNameBytes];
    CustomEmojiRecord emoji_record_storage[kChatStateEmojiCapacity];
    u64 emoji_role_storage[kChatStateEmojiRoleCapacity];
};

// The committed owner ends where the demand tail begins; growth of the lazy decode bank does not
// change it.
inline constexpr usize kAccountUiFixedBytes = offsetof(AccountUiRuntime, channel_record_storage);

} // namespace maicord::app_detail
