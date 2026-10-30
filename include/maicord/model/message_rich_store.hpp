#pragma once

#include "maicord/core/stable_storage.hpp"
#include "maicord/protocol/gateway_message_events.hpp"
#include "maicord/protocol/message_reaction.hpp"

namespace maicord {
enum class MediaAttachmentFormat : u32;

struct MessageRichString {
    u32 offset;
    u16 bytes;
    u16 reserved;
};

struct MessageRichReference {
    u64 message_id;
    u64 channel_id;
    u64 guild_id;
    u64 author_id;
    MessageRichString author_name;
    MessageRichString author_avatar_hash;
    MessageRichString content;
    GatewayMessageReferenceState state;
    u8 type;
    u8 content_flags; // GatewayMessageReferenceContent
    u8 message_type;
    u8 quote_flags; // GatewayMessageReferenceQuote
    u8 reserved[3];
};

// The reply preview's trailing icon, in priority order: sticker, slash
// command, microphone, then image for attachments/embeds (never on a poll
// result). The "Click to see attachment" placeholder carries the image icon too.
enum class MessageReplyIcon : u8 { none, image, sticker, command, voice };
inline constexpr u8 kMessageTypePinned = 6;
inline constexpr u8 kMessageTypePollResult = 46;
inline constexpr u8 kMessageTypeUserJoin = 7;
inline constexpr u8 kMessageTypeRoleSubscriptionPurchase = 25;
inline constexpr u8 kMessageTypeGuildApplicationPremium = 32;
inline constexpr u16 kMessageTypeChatInputCommand = 20;
inline constexpr u16 kMessageTypeInteractionPremiumUpsell = 26;
// An interaction header shows slash commands (and the premium upsell) as a
// chip; every other command is plain link-coloured text.
constexpr bool message_interaction_chip(u16 message_type) noexcept {
    return message_type == kMessageTypeChatInputCommand || message_type == kMessageTypeInteractionPremiumUpsell;
}
// What the 16-DIP slot before a quoted message's preview shows besides the
// author's avatar: a join notice's arrow, an app subscription's ticket, a
// poll result's chart.
enum class MessageReplyLeading : u8 { avatar, join_arrow, ticket, poll };
constexpr MessageReplyLeading message_rich_reply_leading(u8 message_type) noexcept {
    return message_type == kMessageTypeUserJoin || message_type == kMessageTypeRoleSubscriptionPurchase
               ? MessageReplyLeading::join_arrow
           : message_type == kMessageTypeGuildApplicationPremium ? MessageReplyLeading::ticket
           : message_type == kMessageTypePollResult              ? MessageReplyLeading::poll
                                                                 : MessageReplyLeading::avatar;
}
// The poll icon fills the 16-DIP avatar slot; the other marks are 12 DIPs and
// give their text 4 DIPs less room.
constexpr bool message_reply_leading_small(MessageReplyLeading leading) noexcept {
    return leading == MessageReplyLeading::join_arrow || leading == MessageReplyLeading::ticket;
}
// A quoted poll result leads with its chart only when the poll's name is
// known; any quote that is not loaded leads with the avatar slot.
inline MessageReplyLeading message_rich_reply_leading(const MessageRichReference& reference) noexcept {
    if (reference.state != GatewayMessageReferenceState::available)
        return MessageReplyLeading::avatar;
    const MessageReplyLeading leading = message_rich_reply_leading(reference.message_type);
    return leading == MessageReplyLeading::poll && reference.content.bytes == 0 ? MessageReplyLeading::avatar : leading;
}
constexpr MessageReplyIcon message_rich_reply_icon(u8 content_flags, u8 message_type, bool text_empty,
                                                   bool poll) noexcept {
    if (content_flags & gateway_reference_stickers)
        return MessageReplyIcon::sticker;
    if (content_flags & gateway_reference_interaction)
        return MessageReplyIcon::command;
    if (content_flags & gateway_reference_voice_message)
        return MessageReplyIcon::voice;
    if ((content_flags & (gateway_reference_attachments | gateway_reference_embeds)) &&
        message_type != kMessageTypePollResult)
        return MessageReplyIcon::image;
    return text_empty && !poll && message_type != kMessageTypePinned && message_type != kMessageTypePollResult &&
                   !(content_flags & gateway_reference_components_v2)
               ? MessageReplyIcon::image
               : MessageReplyIcon::none;
}

struct MessageRichSnapshot {
    MessageRichString content;
    u64 timestamp_ms;
    u64 edited_timestamp_ms;
    u32 flags;
    u16 type;
    bool present;
};

struct MessageRichReaction {
    u64 emoji_id;
    MessageRichString name;
    u32 count;
    u32 normal_count;
    u32 burst_count;
    u8 flags;
    u8 reserved[3];
};

struct MessageRichInteraction {
    u64 id;
    u64 user_id;
    MessageRichString user_name;
    MessageRichString user_avatar_hash;
    MessageRichString command_name;
    u8 type;
    u8 reserved[7];
};

// Thread started from this message; id 0 when none is known.
struct MessageRichThread {
    u64 id;
    u64 last_message_id;
    MessageRichString name;
    u32 message_count;
    u32 member_count;
    bool archived;
    bool locked;
    u8 reserved[6];
};

struct MessageRichAttachment {
    u64 id;
    u64 size;
    MessageRichString filename;
    MessageRichString title;
    MessageRichString description;
    MessageRichString content_type;
    MessageRichString url;
    u32 width;
    u32 height;
    u32 flags;
    u8 state;
    u8 reserved[3];
    MessageRichString waveform;
    u32 duration_ms;
};

struct MessageRichEmbedField {
    MessageRichString name;
    MessageRichString value;
    u16 embed_index;
    bool inline_field;
    u8 reserved;
};

struct MessageRichEmbedMedia {
    MessageRichString url;
    MessageRichString proxy_url;
    MessageRichString content_type;
    MessageRichString description;
    u32 width;
    u32 height;
    u32 flags;
    u8 state;
    u8 reserved[3];
};

enum class MessageRichEmbedMediaSlot : u8 {
    image,
    thumbnail,
    video
};

struct MessageRichEmbed {
    MessageRichString title;
    MessageRichString description;
    MessageRichString url;
    MessageRichString author_name;
    MessageRichString footer_text;
    MessageRichString author_url;
    MessageRichString author_icon; // author.proxy_icon_url
    MessageRichString footer_icon; // footer.proxy_icon_url
    MessageRichString provider_name;
    MessageRichString provider_url;
    u64 timestamp_ms;
    MessageRichEmbedMedia image;
    MessageRichEmbedMedia thumbnail;
    MessageRichEmbedMedia video;
    u32 color;
    u16 field_begin;
    u16 field_count;
    GatewayMessageEmbedType type;
    bool has_color;
    bool has_title;
    bool has_author;
    u8 flags; // embed flags low byte (kGatewayMediaExplicitFlag)
};

struct MessageRichSticker {
    u64 id;
    MessageRichString name;
    u8 format_type;
    u8 reserved[7];
};

// Field reuse by type follows GatewayMessageComponent.
struct MessageRichComponent {
    MessageRichString label;
    MessageRichString content;
    MessageRichString custom_id;
    MessageRichString url;
    MessageRichString emoji_name;
    u64 emoji_id;
    u32 id;
    u32 accent_color;
    u16 parent;
    u16 order;
    u16 type;
    u16 style;
    bool disabled;
    bool has_accent_color;
    u8 min_values;
    u8 max_values;
    u8 flags; // GatewayMessageComponentFlag
    u8 reserved[3];
};

// System-message data that is not part of the base record: the target of a
// recipient add/remove (mentions[0]) and the CALL object.
struct MessageRichSystem {
    u64 target_user_id;
    MessageRichString target_name;
    u64 call_ended_ms;
    bool call_present;
    bool call_ended;
    bool call_participated;
    u8 reserved;
    u16 message_type; // retained from the last event that carried the type
    u8 reserved_tail[2];
};

struct MessageRichRecord {
    u64 message_id;
    u32 message_revision;
    u32 truncations;
    union {
        MessageRichReference reference;
        MessageRichSnapshot snapshot; // valid only for a tagged snapshot child
    };
    MessageRichInteraction interaction;
    // Item arrays: blocks owned by the store's side slabs (see MessageRichSide),
    // valid for indexes below the matching count and until the next store
    // mutation. An empty array points at shared zero items, never null in a
    // store-owned record.
    MessageRichReaction* reactions;
    MessageRichAttachment* attachments;
    MessageRichEmbed* embeds;
    MessageRichEmbedField* embed_fields;
    MessageRichSticker* stickers;
    MessageRichComponent* components;
    u16 generation;
    u8 reaction_count;
    u8 attachment_count;
    u8 embed_count;
    u8 embed_field_count;
    u8 sticker_count;
    u8 component_count;
    bool occupied;
    u16 forwarded_snapshot_slot;
    bool forwarded_snapshot_child;
    bool has_forwarded_snapshot;
    bool voice_message;
    // Message flag IS_COMPONENTS_V2: top-level components stack 8 apart
    // instead of the 6 between legacy action rows.
    bool components_v2;
    MessageRichThread thread;
    MessageRichSystem system;
};

constexpr bool message_rich_record_voice(const MessageRichRecord& record) noexcept {
    return record.voice_message;
}

static_assert(sizeof(MessageRichRecord) == 264);

// Zeroes a record and points its arrays at the shared empty items. Store slots
// that are not in use are exactly a cleared record plus their generation.
void message_rich_record_clear(MessageRichRecord* record) noexcept;

inline bool message_rich_has_interaction(const MessageRichRecord& rich) noexcept {
    return rich.interaction.user_id != 0 && rich.interaction.command_name.bytes != 0;
}

inline bool message_rich_has_header(const MessageRichRecord& rich) noexcept {
    // System rows use their references as actions (pins, threads, polls).
    if (rich.forwarded_snapshot_child || message_type_is_system(rich.system.message_type))
        return false;
    return (rich.reference.type != 1 && rich.reference.state != GatewayMessageReferenceState::none) ||
           message_rich_has_interaction(rich);
}

inline u64 message_rich_header_user(const MessageRichRecord& rich) noexcept {
    if (rich.forwarded_snapshot_child || message_type_is_system(rich.system.message_type))
        return 0;
    return rich.reference.type != 1 && rich.reference.state != GatewayMessageReferenceState::none
               ? rich.reference.author_id
               : rich.interaction.user_id;
}

// Fixed-size blocks for the item arrays, in two classes each (a few items, or
// the protocol maximum), so a record pays only for the arrays it fills.
inline constexpr u32 kMessageRichSideKinds = 6;
struct MessageRichSlab {
    StableStorage storage;
    u32 block_bytes;
    u32 free_head; // block index + 1, linked through the first word of each free block
    u32 top;       // blocks handed out so far
};
struct MessageRichSide {
    MessageRichSlab slabs[kMessageRichSideKinds][2]; // [kind][0] small class, [1] large class
    u32 initialized;
};

struct MessageRichStore {
    MessageRichSide side;
    MessageRichRecord* records;
    StableStorage* record_storage;
    u16* order;
    u8* text;
    u32 capacity;
    u32 record_extent;
    u32 count;
    u32 text_capacity;
    u32 text_used;
    u32 text_wasted;
    u32 rejected;
    u32 updated;
    u32 deleted;
    u32 compactions;
    u8 active_bank;
    bool sparse_text;    // caller commits text prefixes before writes
    u32 record_capacity; // physical outer + child slots; capacity bounds indexed outer records
    u32 snapshot_count;
};

enum class MessageRichStoreError : u8 {
    none,
    invalid_input,
    invalid_utf8,
    record_capacity,
    corrupt_state,
};

struct MessageRichStoreResult {
    const MessageRichRecord* record;
    MessageRichStoreError error;
    bool inserted;
    bool storage_truncated;

    constexpr bool ok() const noexcept {
        return error == MessageRichStoreError::none;
    }
};

enum class MessageRichReactionMutationKind : u8 {
    add,
    remove,
    remove_all,
    remove_emoji,
};

struct MessageRichReactionMutation {
    u64 emoji_id;
    Utf8View name;
    MessageReactionType type;
    MessageRichReactionMutationKind kind;
    bool current_user;
    bool animated;
};

bool message_rich_store_initialize(MessageRichStore* store, Slice<MessageRichRecord> records,
                                   Slice<u16> order, Slice<u8> text) noexcept;
// For caller-owned sparse virtual text reservations. Initialize and reset write
// only text[0] and text[text.count / 2], which must already be committed.
// Before apply/reaction mutation/compaction, the caller must commit both banks'
// entire prefixes that the operation may use; unused tails stay untouched.
// Release frees owned records, never the caller's text reservation.
bool message_rich_store_initialize_owned_sparse(MessageRichStore* store, StableStorage* record_storage,
                                                u32 record_capacity, Slice<u16> order,
                                                Slice<u8> reserved_text) noexcept;
void message_rich_store_release(MessageRichStore* store) noexcept;
MessageRichStoreResult message_rich_store_apply(MessageRichStore* store, u64 message_id,
                                                u32 message_revision,
                                                const GatewayMessageEvent& input) noexcept;
MessageRichStoreResult message_rich_store_mutate_reaction(
    MessageRichStore* store, u64 message_id, u32 message_revision,
    const MessageRichReactionMutation& mutation) noexcept;
const MessageRichRecord* message_rich_store_find(const MessageRichStore& store, u64 message_id) noexcept;
// Delivery-only base-message changes preserve current rich metadata. Stale
// metadata is never promoted to match a newer base revision.
bool message_rich_store_advance_revision(MessageRichStore* store, u64 message_id, u32 expected_revision,
                                         u32 next_revision) noexcept;
// Metadata selects a bounded standalone decoder; encoded bytes still require
// validation. Unknown MIME types never become MP3/WAV merely by filename.
MediaAttachmentFormat message_rich_attachment_audio_format(
    const MessageRichStore& store, const MessageRichAttachment& attachment) noexcept;
bool message_rich_attachment_audio_supported(const MessageRichStore& store,
                                             const MessageRichAttachment& attachment) noexcept;

bool message_rich_store_validate_attachments(ConstSlice<GatewayMessageAttachment> attachments) noexcept;
bool message_rich_store_set_attachment_dimensions(MessageRichStore* store, u64 message_id,
                                                  u32 message_revision, u64 attachment_id, u32 width,
                                                  u32 height, bool* changed,
                                                  bool snapshot = false) noexcept;
bool message_rich_store_set_embed_dimensions(MessageRichStore* store, u64 message_id, u32 message_revision,
                                             u32 embed_index, MessageRichEmbedMediaSlot slot, u32 width,
                                             u32 height, bool* changed, bool snapshot = false) noexcept;
Utf8View message_rich_store_string(const MessageRichStore& store, MessageRichString value) noexcept;
const MessageRichRecord* message_rich_store_snapshot(const MessageRichStore& store,
                                                     u64 outer_message_id) noexcept;
bool message_rich_store_delete(MessageRichStore* store, u64 message_id) noexcept;
bool message_rich_store_reset(MessageRichStore* store) noexcept;

// Snapshot helpers. A packed message (outer record plus forwarded child) carries
// its own strings, so it is independent of text-bank and slot layout. *written
// is 0 when the message has no rich record; an empty output only sizes it.
bool message_rich_store_pack(const MessageRichStore& store, u64 message_id, Slice<u8> output,
                             u32* written) noexcept;
// Inserts a packed message that is not yet resident, bound to message_revision.
// A bulk caller passes slot_hint (initially 0) so free slots are found in order.
bool message_rich_store_unpack(MessageRichStore* store, u64 message_id, u32 message_revision,
                               ConstSlice<u8> packed, u32* slot_hint = nullptr) noexcept;

static_assert(sizeof(MessageRichString) == 8);

} // namespace maicord
