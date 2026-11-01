#pragma once

#include "maicord/model/chat/message_rich_store.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/stable_storage.hpp"
#include "maicord/model/chat/sorted_search.hpp"

namespace maicord {
namespace message_rich_store_detail {

inline constexpr u32 kReferenceFields =
    gateway_message_field_message_reference | gateway_message_field_referenced_message;
inline constexpr u32 kRichFields =
    kReferenceFields | gateway_message_field_reactions | gateway_message_field_attachments |
    gateway_message_field_embeds | gateway_message_field_sticker_items | gateway_message_field_components |
    gateway_message_field_interaction | gateway_message_field_message_snapshots | gateway_message_field_thread |
    gateway_message_field_mentions | gateway_message_field_call;

using StringVisitor = bool (*)(void*, MessageRichString*) noexcept;

inline constexpr u32 kItemBytes[kMessageRichSideKinds] = {
    sizeof(MessageRichReaction), sizeof(MessageRichAttachment), sizeof(MessageRichEmbed),
    sizeof(MessageRichEmbedField), sizeof(MessageRichSticker), sizeof(MessageRichComponent)};

struct SearchResult {
    u32 order_index;
    u16 slot;
    bool found;
};

void* block_of(const MessageRichRecord& record, u32 kind) noexcept;
void set_block(MessageRichRecord* record, u32 kind, void* block) noexcept;
u32 count_of(const MessageRichRecord& record, u32 kind) noexcept;
bool walk_item(u32 kind, void* item, StringVisitor visitor, void* context) noexcept;
bool walk_head_strings(MessageRichRecord* record, u32 fields, StringVisitor visitor, void* context) noexcept;
bool walk_strings(MessageRichRecord* record, u32 fields, StringVisitor visitor, void* context) noexcept;
void* fit_block(MessageRichSide* side, u32 kind, void* current, u32 count, bool* fresh) noexcept;
void reset_slot(MessageRichStore* store, MessageRichRecord* record, u16 generation) noexcept;
u32 bank_capacity(const MessageRichStore& store) noexcept;
u32 bank_base(const MessageRichStore& store) noexcept;
SearchResult find_record(const MessageRichStore& store, u64 message_id) noexcept;
Utf8View stored_text(const MessageRichStore& store, MessageRichString value) noexcept;
u32 free_slot(const MessageRichStore& store, u32 from, u32 skip) noexcept;
bool compact_excluding(MessageRichStore* store, u16 target_slot, bool has_target,
                       u32 replaced_fields) noexcept;
bool count_wasted(void* opaque, MessageRichString* value) noexcept;
bool erase_slot(MessageRichStore* store, u32 order_index, u16 slot) noexcept;
bool initialize_store(MessageRichStore* store, MessageRichRecord* records, StableStorage* record_storage,
                      u32 capacity, u32 record_extent, Slice<u16> order, Slice<u8> text,
                      u32 physical_capacity, bool sparse_text = false, bool keep_side = false) noexcept;
bool extend_records(MessageRichStore* store) noexcept;

} // namespace message_rich_store_detail
} // namespace maicord
