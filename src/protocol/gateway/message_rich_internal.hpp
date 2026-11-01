#pragma once

#include "maicord/protocol/gateway/message_events.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/encoding/json_ids.hpp"
#include "decode.hpp"
#include "../encoding/json_field_lists.hpp"

namespace maicord {
namespace message_rich_detail {

// Gathered values and queued work items are assigned before their counts advance.
template<class T, u32 Count> struct DecodeScratch {
    static_assert(__is_trivially_copyable(T));
    alignas(T) u8 storage[sizeof(T) * Count];
    operator T*() noexcept { return reinterpret_cast<T*>(storage); }
};

using protocol_detail::accept_member;
using protocol_detail::member_bit;
using protocol_detail::member_index;
using protocol_detail::decode_failure;
using protocol_detail::read_failure;
using protocol_detail::read_object;

inline constexpr u32 kRichReferenceContentMaximumBytes = 16 * 1024;
inline constexpr u32 kRichUrlMaximumBytes = 2 * 1024;
inline constexpr u32 kRichAttachmentNameMaximumBytes = 512;
inline constexpr u32 kRichAttachmentDescriptionMaximumBytes = 4 * 1024;
inline constexpr u32 kRichContentTypeMaximumBytes = 128;
inline constexpr u32 kRichEmbedTitleMaximumBytes = 1024;
inline constexpr u32 kRichEmbedDescriptionMaximumBytes = 16 * 1024;
inline constexpr u32 kRichEmbedFooterMaximumBytes = 8 * 1024;
inline constexpr u32 kRichComponentTextMaximumBytes = 4 * 1024;
inline constexpr u32 kRichComponentIdMaximumBytes = 256;

struct RootContext {
    JsonValue message_reference;
    JsonValue referenced_message;
    JsonValue reactions;
    JsonValue attachments;
    JsonValue embeds;
    JsonValue stickers;
    JsonValue components;
    JsonValue interaction;
    JsonValue snapshots;
    JsonValue poll;
    JsonValue thread;
    // The message's own mentions: only a reply that mentions the quoted author shows "@" before the name.
    JsonValue mentions;
    // A clip or media mention of the replying message (see gateway_quote_media_mention).
    JsonValue media_mention;
    u64 seen;
    GatewayEventDecodeError error;
};

struct AuthorContext {
    u64 id;
    JsonString username;
    JsonString global_name;
    JsonString avatar;
    u64 seen;
    GatewayEventDecodeError error;
    bool global_name_null;
    bool avatar_null;
};

struct EmbedFieldContext {
    JsonString name;
    JsonString value;
    bool inline_field;
    u64 seen;
    GatewayEventDecodeError error;
};

struct EmbedContext {
    JsonString title;
    JsonString description;
    JsonString url;
    JsonString type;
    JsonValue author;
    JsonValue footer;
    JsonValue fields;
    JsonValue image;
    JsonValue thumbnail;
    JsonValue video;
    JsonValue provider;
    JsonValue components;
    JsonString timestamp;
    u32 color;
    u32 flags;
    u64 seen;
    GatewayEventDecodeError error;
};

// author {name,url,icon_url,proxy_icon_url}, footer {text,icon_url,
// proxy_icon_url} and provider {name,url}; nulls are absent members.
struct EmbedPartContext {
    JsonString name;
    JsonString text;
    JsonString url;
    JsonString proxy_icon_url;
    u64 seen;
    GatewayEventDecodeError error;
};

bool value_u32(const JsonValue& value, u32* output) noexcept;
bool bounded_string(const JsonString& source, u32 maximum_bytes, const Slice<char>& output_view, u32* used,
                    Utf8View* value, u32* truncations) noexcept;
bool rich_url_valid(Utf8View value, bool attachment_allowed) noexcept;
bool visit_author(void* opaque, JsonString name, JsonValue value) noexcept;
GatewayEventDecodeResult gather_array(const JsonValue& value, const JsonIndexedDocument& scratch,
                                      u32 maximum_depth, JsonValue* values, u32 capacity, u32 truncation,
                                      u32* truncations, u32* count) noexcept;
bool visit_embed_field(void* opaque, JsonString name, JsonValue value) noexcept;
bool visit_embed_part(void* opaque, JsonString name, JsonValue value) noexcept;
bool embed_part_url(const EmbedPartContext& part, u64 bit, JsonString source, Slice<char> text_output,
                    u32* text_used, Utf8View* destination, u32* truncations) noexcept;
GatewayEventDecodeResult decode_reference(const RootContext& root, const JsonIndexedDocument& scratch,
                                          u32 maximum_depth, Slice<char> text_output, u32* text_used,
                                          GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult decode_interaction(const JsonValue& value, const JsonIndexedDocument& scratch, u32 depth,
                                            Slice<char> text, u32* used,
                                            GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult decode_thread(const JsonValue& value, const JsonIndexedDocument& scratch, u32 depth,
                                       Slice<char> text, u32* used, GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult decode_reactions(JsonValue array, const JsonIndexedDocument& scratch,
                                          u32 maximum_depth, Slice<char> text_output, u32* text_used,
                                          GatewayMessageRichScratch output_scratch,
                                          GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult decode_attachments(JsonValue array, const JsonIndexedDocument& scratch,
                                            u32 maximum_depth, Slice<char> text_output, u32* text_used,
                                            GatewayMessageRichScratch output_scratch,
                                            GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult decode_embeds(JsonValue array, const JsonIndexedDocument& scratch,
                                       u32 maximum_depth, Slice<char> text_output, u32* text_used,
                                       GatewayMessageRichScratch output_scratch,
                                       GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult decode_stickers(JsonValue array, const JsonIndexedDocument& scratch,
                                         u32 maximum_depth, Slice<char> text_output, u32* text_used,
                                         GatewayMessageRichScratch output_scratch,
                                         GatewayMessageEvent* output) noexcept;
GatewayEventDecodeResult decode_components(JsonValue array, const JsonIndexedDocument& scratch,
                                           u32 maximum_depth, Slice<char> text_output, u32* text_used,
                                           GatewayMessageRichScratch output_scratch,
                                           GatewayMessageEvent* output) noexcept;

} // namespace message_rich_detail
} // namespace maicord
