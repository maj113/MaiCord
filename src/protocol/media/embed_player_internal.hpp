#pragma once

#include "maicord/protocol/media/embed_player.hpp"
#include "../encoding/json_field_lists.hpp"
#include "maicord/protocol/encoding/writer.hpp"
#include "maicord/protocol/media/embed_provider.hpp"

namespace maicord {
namespace embed_player_detail {

using protocol_detail::FieldSet;
using protocol_detail::MemberName;
using protocol_detail::NameTable;
using Field = protocol_detail::FieldValue;
using protocol_detail::field_named;

struct Candidate {
    JsonString url;
    // `index` is the format's place in the provider's list.
    u32 content, bitrate, width, height, fps, index;
    // The audioQuality of an audio or combined format: 1 low or unnamed, 2 medium, 3 high. 0 without the field.
    u32 audio_quality;
    bool combined, drc;
};
struct Selection {
    JsonIndexedDocument doc;
    // The best adaptive video, the best audio and the best combined format.
    Candidate video, audio, combined;
    u32 maximum_height, count;
    // Receives one record for each format. Null when the caller wants no report.
    EmbedOffer* offer;
    // The best Opus format. `audio` is the best AAC format until the list ends and the client picks one of the two.
    Candidate opus;
};
struct TwitterMedia { JsonIndexedDocument doc; JsonValue selected; u8 index; u32 count; bool found; };
struct TwitterVariants { JsonIndexedDocument doc; EmbedStream selected; u16 cap; u32 count; };

bool id_chars(const Utf8View& id, bool dashes = false) noexcept;
bool video_id(const Utf8View& id) noexcept;
u64 numeric_id(const Utf8View& id) noexcept;
u64 decimal(const Utf8View& digits) noexcept;
u32 id_target(const Utf8View& prefix, const Utf8View& id, const Slice<u8>& output) noexcept;
bool u64_of(const Field& f, u64* out) noexcept;
bool bool_of(const Field& f, bool* out) noexcept;
bool text(const Field& f, JsonString* out) noexcept;
bool equal_chars(const Field& f, const char* value, u32 bytes) noexcept;
bool equal(const Field& f, const Utf8View& value) noexcept;
bool has_prefix_chars(const Utf8View& view, const char* prefix, u32 bytes) noexcept;
bool is_text_chars(const Utf8View& view, const char* value, u32 bytes) noexcept;
bool is_text_nocase_chars(const Utf8View& view, const char* value, u32 bytes) noexcept;
JsonStringDecodeResult decode(const JsonString& value, char* out, u32 capacity) noexcept;
bool visit_array(const JsonIndexedDocument& doc, const JsonValue& value, JsonArrayVisitor visitor, void* context) noexcept;
// The value at the end of a path of member names from `value` (`table` is a NameTable). A value without source text
// when a step is not an object or lacks the member.
JsonValue child_path(const JsonIndexedDocument& doc, JsonValue value, const u8* table, u32 depth) noexcept;
bool number(const Field& f, u32* out) noexcept;
EmbedPlayerError index_json(const Utf8View& json, const Slice<u32>& scratch, JsonIndexedDocument* doc) noexcept;
void sanitize_title(MediaTrackTitle* title) noexcept;
JsonStringDecodeResult decode_title(const JsonString& name, MediaTrackTitle* title) noexcept;
// Appends a track's author (sanitized; empty when unknown) and length after its title (track_info.hpp), when both fit.
void append_track_detail(MediaTrackTitle* title, Utf8View author, u32 seconds) noexcept;
bool track_page_is(const JsonString& page, u64 id, const Utf8View& country, u32 capacity) noexcept;
EmbedPlayerError publish_stream(EmbedStream* staged, u32 url_bytes, bool valid, EmbedStream* output) noexcept;
EmbedPlayerError embedded_json(Utf8View html, Utf8View marker, Utf8View* output, bool array = false) noexcept;
bool better_rendition(u32 height, u32 bitrate, u32 old_height, u32 old_bitrate, u32 cap) noexcept;
u32 short_side(const Candidate& c) noexcept;
bool format(void* opaque, u32, JsonValue value) noexcept;
bool store(const Candidate& candidate, EmbedStream* out) noexcept;
u64 stream_expiry_chars(const Utf8View& url, const char* key_data, u32 key_bytes, bool* present) noexcept;
bool twitter_location(Utf8View url, EmbedStreamLocation* out, u16* width=nullptr, u16* height=nullptr) noexcept;
bool twitter_media(void* context,u32 index,JsonValue value) noexcept;
bool twitter_variant(void* context,u32,JsonValue value) noexcept;
template <MemberName... Names>
__forceinline JsonValue child(const JsonIndexedDocument& doc, const JsonValue& parent, u32 depth = 48) noexcept {
    return child_path(doc, parent, NameTable<Names...>::data.bytes, depth);
}

template <MemberName... Names>
__forceinline bool fields(const JsonIndexedDocument& doc, const JsonValue& value, FieldSet<Names...>& set) noexcept {
    return protocol_detail::fields_at<24>(doc, value, set);
}
template <MemberName... Names>
__forceinline bool fields(const JsonIndexedDocument& doc, FieldSet<Names...>& set) noexcept {
    return protocol_detail::fields_at<24>(doc, set);
}
// Literal operands travel as pointer and length instead of a view built on the stack.
template <usize N> __forceinline bool equal(const Field& f, const char (&value)[N]) noexcept { return equal_chars(f, value, N - 1); }
template <usize N> __forceinline bool has_prefix(const Utf8View& view, const char (&prefix)[N]) noexcept {
    return has_prefix_chars(view, prefix, N - 1);
}
template <usize N> __forceinline bool is_text(const Utf8View& view, const char (&value)[N]) noexcept {
    return is_text_chars(view, value, N - 1);
}
template <usize N> __forceinline bool is_text_nocase(const Utf8View& view, const char (&value)[N]) noexcept {
    return is_text_nocase_chars(view, value, N - 1);
}
template <usize N> __forceinline u64 stream_expiry(const Utf8View& url, const char (&key)[N], bool* present = nullptr) noexcept {
    return stream_expiry_chars(url, key, N - 1, present);
}

} // namespace embed_player_detail
} // namespace maicord
