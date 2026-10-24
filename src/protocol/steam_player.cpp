#include "maicord/protocol/embed_player.hpp"
#include "maicord/protocol/writer.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/byte_range.hpp"

namespace maicord {
u32 steam_id(Utf8View value) noexcept {
    if (!value.data || !value.bytes || value.bytes > 10 || value.data[0] == '0') return 0;
    u32 result = 0;
    for (u32 i = 0; i < value.bytes; ++i) {
        const u32 digit = static_cast<u8>(value.data[i]) - '0';
        if (digit > 9 || result > (~u32{0} - digit) / 10) return 0;
        result = result * 10 + digit;
    }
    return result;
}
u32 steam_video_target(Utf8View app, Slice<u8> output) noexcept {
    if (!steam_id(app)) return 0;
    ProtocolWriter w{output, 0};
    return protocol_writer_append_literal(&w, "/api/appdetails?appids=") &&
        protocol_writer_append_bytes(&w, reinterpret_cast<const u8*>(app.data), app.bytes) &&
        protocol_writer_append_literal(&w, "&filters=movies") ? w.used : 0;
}
namespace {
bool location(Utf8View url, bool manifest, Utf8View app, EmbedStreamLocation* out) noexcept {
    constexpr auto origin = utf8_literal("https://video.akamai.steamstatic.com");
    constexpr auto prefix = utf8_literal("/store_trailers/");
    if (!url.data || url.bytes > kEmbedStreamUrlBytes || !utf8_starts_with(url, origin) ||
        !utf8_starts_with({url.data + origin.bytes, url.bytes - origin.bytes}, prefix)) return false;
    u32 at = origin.bytes + prefix.bytes;
    Utf8View parts[4]{};
    for (auto& part : parts) {
        const u32 begin = at;
        while (at < url.bytes && url.data[at] != '/') ++at;
        if (at == url.bytes || at == begin) return false;
        part = {url.data + begin, at++ - begin};
    }
    if (!steam_id(parts[0]) || !steam_id(parts[1]) || !steam_id(parts[3]) ||
        (app.bytes && !utf8_equal(parts[0], app)) || parts[2].bytes < 32 || parts[2].bytes > 64) return false;
    for (u32 i = 0; i < parts[2].bytes; ++i) if (!ascii_is_hex_digit(parts[2].data[i])) return false;
    auto leaf = Utf8View{url.data + at, url.bytes - at};
    if (manifest) {
        constexpr auto name = utf8_literal("dash_h264.mpd");
        if (!utf8_starts_with(leaf, name)) return false;
        if (leaf.bytes != name.bytes) {
            leaf = {leaf.data + name.bytes, leaf.bytes - name.bytes};
            if (!utf8_starts_with(leaf, utf8_literal("?t=")) || leaf.bytes < 4 || leaf.bytes > 23) return false;
            for (u32 i = 3; i < leaf.bytes; ++i) if (!ascii_is_digit(leaf.data[i])) return false;
        }
    } else {
        constexpr auto init = utf8_literal("dash_h264/init-stream"), media = utf8_literal("dash_h264/chunk-stream");
        const bool initialization = utf8_starts_with(leaf, init);
        const u32 begin = initialization ? init.bytes : utf8_starts_with(leaf, media) ? media.bytes : 0;
        if (!begin) return false;
        u32 cursor = begin;
        while (cursor < leaf.bytes && ascii_is_digit(leaf.data[cursor])) ++cursor;
        if (cursor == begin || cursor - begin > 10) return false;
        if (!initialization) {
            if (cursor == leaf.bytes || leaf.data[cursor++] != '-') return false;
            const u32 number = cursor;
            while (cursor < leaf.bytes && ascii_is_digit(leaf.data[cursor])) ++cursor;
            if (cursor == number || cursor - number > 10) return false;
        }
        if (!utf8_equal({leaf.data + cursor, leaf.bytes - cursor}, utf8_literal(".m4s"))) return false;
    }
    if (out) *out = {{origin.data + 8, origin.bytes - 8}, {url.data + origin.bytes, url.bytes - origin.bytes}};
    return true;
}
struct Field { Utf8View name; JsonValue value{}; bool seen{}; };
struct Fields { Field* data; u32 count; };
bool member(void* context, JsonString name, JsonValue value) noexcept {
    auto& fields = *static_cast<Fields*>(context);
    for (u32 i = 0; i < fields.count; ++i) if (json_name_is(name, fields.data[i].name)) {
        if (fields.data[i].seen) return false;
        fields.data[i].seen = true; fields.data[i].value = value; break;
    }
    return true;
}
template<usize N> bool fields(const JsonIndexedDocument& doc, JsonValue object, Field (&entries)[N]) noexcept {
    Fields list{entries, static_cast<u32>(N)};
    return json_visit_indexed_object(doc, object, 16, member, &list).ok();
}
}
bool steam_manifest_location(Utf8View url, Utf8View app, EmbedStreamLocation* out) noexcept { return location(url, true, app, out); }
bool steam_media_location(Utf8View url, EmbedStreamLocation* out) noexcept { return location(url, false, {}, out); }
EmbedPlayerError steam_video_manifest(Utf8View json, Utf8View app, Utf8View movie, Slice<u32> scratch, EmbedStream* output) noexcept {
    using E = EmbedPlayerError;
    if (!output || !steam_id(app) || (movie.bytes && !steam_id(movie)) || !json.data || !json.bytes || json.bytes > 128 * 1024 ||
        !byte_range_valid(json.data, json.bytes) || !byte_range_valid(output, sizeof(*output)) ||
        byte_ranges_overlap(json.data, json.bytes, output, sizeof(*output))) return E::invalid_input;
    const auto scan = json_scan_structurals_avx1(json, scratch);
    if (!scan.ok()) return scan.error == JsonScanError::output_overflow ? E::capacity : E::malformed;
    const JsonIndexedDocument doc{json, {scratch.data, scan.count}};
    Field root[]{{app}};
    if (!fields(doc, {json, 0, JsonValueKind::object}, root) || !root[0].seen) return E::malformed;
    Field product[]{{utf8_literal("success")}, {utf8_literal("data")}}; bool success{};
    if (!fields(doc, root[0].value, product) || !product[0].seen || !json_value_to_bool(product[0].value, &success)) return E::malformed;
    if (!success) return E::unavailable;
    Field data[]{{utf8_literal("movies")}};
    if (!product[1].seen || !fields(doc, product[1].value, data)) return E::malformed;
    if (!data[0].seen) return E::unavailable;
    struct Selection { const JsonIndexedDocument* doc; u32 wanted; JsonString url{}; bool selected{}, supported{}; } selected{&doc, steam_id(movie)};
    const auto visited = json_visit_indexed_array(doc, data[0].value, 16,
        +[](void* context, u32 index, JsonValue value) noexcept {
            auto& s = *static_cast<Selection*>(context);
            if (index >= 64) return false;
            Field record[]{{utf8_literal("id")}, {utf8_literal("dash_h264")}}; u64 id{};
            if (!fields(*s.doc, value, record) || !record[0].seen || !json_value_to_u64(record[0].value, &id) || !id || id > ~u32{0}) return false;
            if (s.wanted ? id != s.wanted : index != 0) return true;
            if (s.selected) return false;
            s.selected = true;
            if (record[1].seen) {
                if (!json_value_as_string(record[1].value, &s.url)) return false;
                s.supported = true;
            }
            return true;
        }, &selected);
    if (!visited.ok()) return E::malformed;
    if (!selected.selected) return E::unavailable;
    if (!selected.supported) return E::unsupported;
    EmbedStream staged{};
    const auto decoded = json_string_decode_utf8(selected.url, {staged.url, sizeof(staged.url)});
    if (!decoded.ok() || !steam_manifest_location({staged.url, decoded.bytes}, app)) return E::malformed;
    staged.url_bytes = decoded.bytes; *output = staged;
    return E::none;
}
} // namespace maicord
