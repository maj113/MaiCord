#include "maicord/protocol/media_catalog.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/protocol/rest_route.hpp"
#include "maicord/protocol/json_ids.hpp"
#include "maicord/core/byte_range.hpp"

namespace maicord {
namespace {
using E = MediaCatalogError;
bool sticker_operation(MediaCatalogOperation operation) noexcept {
    return operation == MediaCatalogOperation::sticker_packs ||
           operation == MediaCatalogOperation::guild_stickers;
}
u32 item_limit(MediaCatalogOperation operation) noexcept {
    return sticker_operation(operation)                          ? kMediaCatalogMaximumStickerItems
           : operation == MediaCatalogOperation::gif_suggestions ? kMediaCatalogMaximumSuggestions
                                                                 : kMediaCatalogMaximumPageItems;
}
bool text_valid(Utf8View text, u32 maximum, bool required, bool whitespace = false) noexcept {
    if ((!text.data && text.bytes) || text.bytes > maximum || (required && !text.bytes) ||
        !utf8_validate(text).valid)
        return false;
    for (u32 i = 0; i < text.bytes; ++i)
        if ((static_cast<u8>(text.data[i]) < 32 &&
             !(whitespace && (text.data[i] == '\t' || text.data[i] == '\n' || text.data[i] == '\r'))) ||
            text.data[i] == 127)
            return false;
    return true;
}
bool locale_valid(Utf8View locale) noexcept {
    if (!text_valid(locale, 32, true))
        return false;
    for (u32 i = 0; i < locale.bytes; ++i) {
        const char c = locale.data[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-'))
            return false;
    }
    return locale.data[0] != '-' && locale.data[locale.bytes - 1] != '-';
}
// Schema validation only. This does not grant the media worker admission to a
// host. Scheme-relative official/favorite sources normalize to HTTPS.
bool url_valid(Utf8View url, bool* relative) noexcept {
    u32 first = 0;
    if (url.bytes >= 8 && utf8_equal({url.data, 8}, utf8_literal("https://")))
        first = 8;
    else if (url.bytes >= 2 && url.data[0] == '/' && url.data[1] == '/')
        first = 2;
    else
        return false;
    *relative = first == 2;
    u32 stop = first;
    while (stop < url.bytes && url.data[stop] != '/' && url.data[stop] != '?' && url.data[stop] != '#') {
        const char c = url.data[stop++];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' ||
              c == '-'))
            return false;
    }
    if (stop == first || stop - first > 253 || url.data[first] == '.' || url.data[first] == '-' ||
        url.data[stop - 1] == '.')
        return false;
    for (u32 i = stop; i < url.bytes; ++i) {
        const u8 c = static_cast<u8>(url.data[i]);
        if (c <= 32 || c >= 127 || c == '\\' || c == '"' || c == '<' || c == '>')
            return false;
    }
    return true;
}
struct WireItem {
    JsonString id, name, title, src, gif, url, description, tags;
    u64 sticker_id, guild_id, pack_id;
    u32 width, height, format, seen;
    bool available;
    E error;
};
struct Context {
    JsonIndexedDocument document;
    Slice<MediaCatalogItem> items;
    Slice<char> text;
    MediaCatalogDecodeResult result;
    MediaCatalogItemKind kind;
    u32 depth, limit, first, selected, used;
    u64 pack_id, cover_id;
    JsonString pack_name;
    Utf8View pack_label;
    bool pack_name_copied;
    bool write, page_closed;
};
bool fail(Context& c, E error, u32 offset = 0) noexcept {
    c.result.error = error;
    c.result.error_offset = offset;
    return false;
}
bool read_failed(Context& c, const JsonReadResult& read, u32 offset) noexcept {
    if (read.ok())
        return true;
    if (c.result.error == E::none)
        fail(c, E::json, offset);
    c.result.read_error = read.error;
    return false;
}
bool member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& w = *static_cast<WireItem*>(opaque);
    u32 bit = 0;
    JsonString* string = nullptr;
    if (json_string_equals_ascii(name, utf8_literal("id")))
        bit = 1;
    else if (json_string_equals_ascii(name, utf8_literal("name"))) {
        bit = 2;
        string = &w.name;
    } else if (json_string_equals_ascii(name, utf8_literal("title"))) {
        bit = 4;
        string = &w.title;
    } else if (json_string_equals_ascii(name, utf8_literal("src"))) {
        bit = 8;
        string = &w.src;
    } else if (json_string_equals_ascii(name, utf8_literal("gif_src"))) {
        bit = 16;
        string = &w.gif;
    } else if (json_string_equals_ascii(name, utf8_literal("url"))) {
        bit = 32;
        string = &w.url;
    } else if (json_string_equals_ascii(name, utf8_literal("width")))
        bit = 64;
    else if (json_string_equals_ascii(name, utf8_literal("height")))
        bit = 128;
    else if (json_string_equals_ascii(name, utf8_literal("format_type")))
        bit = 256;
    else if (json_string_equals_ascii(name, utf8_literal("available")))
        bit = 512;
    else if (json_string_equals_ascii(name, utf8_literal("guild_id")))
        bit = 1024;
    else if (json_string_equals_ascii(name, utf8_literal("pack_id")))
        bit = 2048;
    else if (json_string_equals_ascii(name, utf8_literal("description"))) {
        bit = 4096;
        string = &w.description;
    } else if (json_string_equals_ascii(name, utf8_literal("tags"))) {
        bit = 8192;
        string = &w.tags;
    } else
        return true;
    if (w.seen & bit) {
        w.error = E::schema;
        return false;
    }
    w.seen |= bit;
    bool okay = true;
    if (string) {
        if ((bit == 16 || bit == 4096) && value.kind == JsonValueKind::null_value)
            return true;
        okay = json_value_as_string(value, string);
    } else if (bit == 1) {
        if (value.kind == JsonValueKind::string) {
            okay = json_value_as_string(value, &w.id);
            json_string_to_u64(w.id, &w.sticker_id);
        } else {
            okay = json_value_to_u64(value, &w.sticker_id);
            if (okay)
                w.id = {value.source, value.source_offset, false};
        }
    } else if (bit == 512)
        okay = json_value_to_bool(value, &w.available);
    else if (bit == 1024)
        okay = json_value_to_snowflake(value, &w.guild_id);
    else if (bit == 2048)
        okay = json_value_to_snowflake(value, &w.pack_id);
    else {
        u64 n = 0;
        okay = json_value_to_u64(value, &n) && n <= ~u32{0};
        if (bit == 64)
            w.width = static_cast<u32>(n);
        else if (bit == 128)
            w.height = static_cast<u32>(n);
        else
            w.format = static_cast<u32>(n);
    }
    if (!okay)
        w.error = E::schema;
    return okay;
}
bool string(Context& c, JsonString input, Utf8View* output, u32 maximum, bool required, bool url,
            bool publish, bool whitespace = false) noexcept {
    if (!input.encoded.data)
        return required ? fail(c, E::schema, input.source_offset) : true;
    char decoded[kMediaCatalogMaximumUrlBytes]{};
    const auto read = json_string_decode_utf8(input, {decoded, sizeof(decoded)});
    if (!read.ok() || read.bytes > maximum || (required && !read.bytes))
        return fail(c, E::schema, input.source_offset);
    bool relative = false;
    const Utf8View value{decoded, read.bytes};
    if (!(url ? (!read.bytes || url_valid(value, &relative))
              : text_valid(value, maximum, required, whitespace)))
        return fail(c, E::schema, input.source_offset);
    const u32 bytes = read.bytes + (relative ? 6 : 0);
    if (bytes > maximum)
        return fail(c, E::schema, input.source_offset);
    if (!publish)
        return true;
    if (bytes > c.text.count - c.used)
        return fail(c, E::text_capacity, input.source_offset);
    if (c.write && bytes) {
        if (relative)
            copy_bytes(c.text.data + c.used, "https:", 6);
        copy_bytes(c.text.data + c.used + (relative ? 6 : 0), decoded, read.bytes);
        *output = {c.text.data + c.used, bytes};
    }
    c.used += bytes;
    return true;
}
bool item(void* opaque, u32, JsonValue value) noexcept {
    auto& c = *static_cast<Context*>(opaque);
    WireItem w{};
    w.available = true;
    if (c.kind == MediaCatalogItemKind::suggestion) {
        if (!json_value_as_string(value, &w.name))
            return fail(c, E::schema, value.source_offset);
    } else {
        if (value.kind != JsonValueKind::object)
            return fail(c, E::schema, value.source_offset);
        const auto read = json_visit_indexed_object(c.document, value, c.depth, member, &w);
        if (!read.ok()) {
            if (w.error != E::none)
                fail(c, w.error, value.source_offset);
            return read_failed(c, read, value.source_offset);
        }
        const u32 required = c.kind == MediaCatalogItemKind::category ? 2 | 8
                             : c.kind == MediaCatalogItemKind::gif    ? 1 | 8 | 32 | 64 | 128
                                                                      : 1 | 2 | 256;
        if ((w.seen & required) != required || w.width > kMediaCatalogMaximumDimension ||
            w.height > kMediaCatalogMaximumDimension ||
            (c.kind == MediaCatalogItemKind::gif && (!w.width || !w.height)) ||
            (c.kind == MediaCatalogItemKind::sticker &&
             (!w.sticker_id || !w.format || w.format > 4 ||
              (c.pack_id && w.pack_id && c.pack_id != w.pack_id))))
            return fail(c, E::schema, value.source_offset);
    }
    const u32 source_index = c.result.source_count++;
    const bool publish = source_index >= c.first && c.selected < c.limit && !c.page_closed;
    const u32 saved = c.used;
    MediaCatalogItem output{};
    output.kind = c.kind;
    output.sticker_id = c.kind == MediaCatalogItemKind::sticker ? w.sticker_id : 0;
    output.guild_id = w.guild_id;
    output.pack_id = w.pack_id ? w.pack_id : c.pack_id;
    output.width = w.width;
    output.height = w.height;
    output.sticker_format = static_cast<u8>(w.format);
    output.available = w.available;
    output.cover = c.cover_id && w.sticker_id == c.cover_id;
    // All records, including valid records beyond this local page, receive
    // schema validation. Publication remains a prefix of the requested range.
    const auto strings = [&](bool copying) noexcept {
        if (!string(c, c.pack_name, &output.pack_name, kMediaCatalogMaximumNameBytes, false, false, false))
            return false;
        if (copying && c.pack_name.encoded.data && !c.pack_name_copied) {
            if (!string(c, c.pack_name, &c.pack_label, kMediaCatalogMaximumNameBytes, true, false, true))
                return false;
            c.pack_name_copied = true;
        }
        if (copying)
            output.pack_name = c.pack_label;
        return string(c, w.id, &output.provider_id, kMediaCatalogMaximumIdBytes,
                      c.kind == MediaCatalogItemKind::gif || c.kind == MediaCatalogItemKind::sticker, false,
                      copying) &&
               string(
                   c, w.name.encoded.data ? w.name : w.title, &output.name, kMediaCatalogMaximumNameBytes,
                   c.kind == MediaCatalogItemKind::category || c.kind == MediaCatalogItemKind::suggestion ||
                       c.kind == MediaCatalogItemKind::sticker,
                   false, copying) &&
               string(c, w.src, &output.preview_url, kMediaCatalogMaximumUrlBytes,
                      c.kind == MediaCatalogItemKind::gif || c.kind == MediaCatalogItemKind::category, true,
                      copying) &&
               string(c, w.gif, &output.gif_url, kMediaCatalogMaximumUrlBytes, false, true, copying) &&
               string(c, w.url, &output.send_url, kMediaCatalogMaximumUrlBytes,
                      c.kind == MediaCatalogItemKind::gif, true, copying) &&
               // Sticker tags contain tabs and descriptions may contain line
               // breaks; the owned strings keep them.
               string(c, w.description, &output.description, 512, false, false, copying, true) &&
               string(c, w.tags, &output.tags, 1024, false, false, copying, true);
    };
    if (!strings(false))
        return false;
    if (publish && !strings(true)) {
        if (c.result.error != E::text_capacity || !c.selected || c.write)
            return false;
        c.result.error = E::none;
        c.result.error_offset = 0;
        c.used = saved;
        c.page_closed = true;
        return true;
    }
    if (publish) {
        if (c.write)
            c.items[c.selected] = output;
        ++c.selected;
    }
    return true;
}
bool array(Context& c, JsonValue value, MediaCatalogItemKind kind) noexcept {
    if (value.kind != JsonValueKind::array)
        return fail(c, E::schema, value.source_offset);
    c.kind = kind;
    return read_failed(c, json_visit_indexed_array(c.document, value, c.depth, item, &c),
                       value.source_offset);
}
bool trending_preview(void* opaque, u32 index, JsonValue value) noexcept {
    auto& c = *static_cast<Context*>(opaque);
    if (!index)
        return item(&c, index, value);
    // gifs[0] is the one navigation tile. The other provider records are still
    // fully validated without changing the gallery's indices.
    Context validation = c;
    validation.limit = 0;
    validation.write = false;
    if (item(&validation, index, value))
        return true;
    c.result = validation.result;
    return false;
}
struct Pack {
    Context* context;
    JsonValue stickers;
    JsonString name;
    u64 id, cover;
    u32 seen;
};
bool pack_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<Pack*>(opaque);
    const u32 bit = json_string_equals_ascii(name, utf8_literal("id"))                 ? 1
                    : json_string_equals_ascii(name, utf8_literal("stickers"))         ? 2
                    : json_string_equals_ascii(name, utf8_literal("name"))             ? 4
                    : json_string_equals_ascii(name, utf8_literal("cover_sticker_id")) ? 8
                                                                                       : 0;
    if (!bit)
        return true;
    if (p.seen & bit)
        return fail(*p.context, E::schema, value.source_offset);
    p.seen |= bit;
    if (bit == 1)
        return json_value_to_snowflake(value, &p.id) || fail(*p.context, E::schema, value.source_offset);
    if (bit == 4)
        return json_value_as_string(value, &p.name) || fail(*p.context, E::schema, value.source_offset);
    if (bit == 8)
        return value.kind == JsonValueKind::null_value || json_value_to_snowflake(value, &p.cover) ||
               fail(*p.context, E::schema, value.source_offset);
    p.stickers = value;
    return true;
}
bool pack(void* opaque, u32, JsonValue value) noexcept {
    auto& c = *static_cast<Context*>(opaque);
    if (value.kind != JsonValueKind::object)
        return fail(c, E::schema, value.source_offset);
    Pack p{&c};
    const auto read = json_visit_indexed_object(c.document, value, c.depth, pack_member, &p);
    if (!read_failed(c, read, value.source_offset))
        return false;
    if ((p.seen & 7) != 7 || !p.id)
        return fail(c, E::schema, value.source_offset);
    c.pack_id = p.id;
    c.cover_id = p.cover;
    c.pack_name = p.name;
    reset_value(&c.pack_label);
    c.pack_name_copied = false;
    const bool okay = array(c, p.stickers, MediaCatalogItemKind::sticker);
    c.pack_id = c.cover_id = 0;
    reset_value(&c.pack_name);
    reset_value(&c.pack_label);
    c.pack_name_copied = false;
    return okay;
}
struct Root {
    Context* context;
    MediaCatalogOperation operation;
    JsonValue payload;
    JsonValue gifs;
    u32 seen;
};
bool root_member(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& root = *static_cast<Root*>(opaque);
    const bool payload = json_string_equals_ascii(
        name, root.operation == MediaCatalogOperation::gif_categories ? utf8_literal("categories")
                                                                      : utf8_literal("sticker_packs"));
    const bool gifs = root.operation == MediaCatalogOperation::gif_categories &&
                      json_string_equals_ascii(name, utf8_literal("gifs"));
    if (!payload && !gifs)
        return true;
    const u32 bit = payload ? 1u : 2u;
    if (root.seen & bit)
        return fail(*root.context, E::schema, value.source_offset);
    root.seen |= bit;
    (payload ? root.payload : root.gifs) = value;
    return true;
}
bool visit(Context& c, MediaCatalogOperation operation) noexcept {
    if (operation == MediaCatalogOperation::gif_categories ||
        operation == MediaCatalogOperation::sticker_packs) {
        Root root{&c, operation};
        const auto read =
            json_visit_root_object(c.document.input, c.document.structurals, c.depth, root_member, &root);
        if (!read_failed(c, read, read.error_offset))
            return false;
        if (!(root.seen & 1) || root.payload.kind != JsonValueKind::array)
            return fail(c, E::schema, root.payload.source_offset);
        if (operation == MediaCatalogOperation::gif_categories) {
            // The provider supplies the Trending tile's preview separately
            // from the named categories. Validate both arrays before publish.
            if (root.seen & 2) {
                if (root.gifs.kind != JsonValueKind::array)
                    return fail(c, E::schema, root.gifs.source_offset);
                c.kind = MediaCatalogItemKind::gif;
                if (!read_failed(
                        c, json_visit_indexed_array(c.document, root.gifs, c.depth, trending_preview, &c),
                        root.gifs.source_offset))
                    return false;
            }
            return array(c, root.payload, MediaCatalogItemKind::category);
        }
        return read_failed(c, json_visit_indexed_array(c.document, root.payload, c.depth, pack, &c),
                           root.payload.source_offset);
    }
    c.kind = operation == MediaCatalogOperation::gif_suggestions  ? MediaCatalogItemKind::suggestion
             : operation == MediaCatalogOperation::guild_stickers ? MediaCatalogItemKind::sticker
                                                                  : MediaCatalogItemKind::gif;
    const auto read = json_visit_root_array(c.document.input, c.document.structurals, c.depth, item, &c);
    return read_failed(c, read, read.error_offset);
}
} // namespace

StickerSendability guild_sticker_sendability(u64 sticker_guild_id, u64 channel_guild_id, bool available,
                                             bool external_permission, u8 premium_type) noexcept {
    if (!sticker_guild_id)
        return StickerSendability::hidden;
    // Another server's stickers stay out of a channel without Use External Stickers.
    if (sticker_guild_id != channel_guild_id && channel_guild_id && !external_permission)
        return StickerSendability::hidden;
    if (!available)
        return StickerSendability::unavailable;
    if (sticker_guild_id == channel_guild_id)
        return StickerSendability::sendable;
    // Nitro (2) and Nitro Basic (3) carry stickers everywhere; Classic (1) does not.
    return premium_type == 2 || premium_type == 3 ? StickerSendability::sendable : StickerSendability::premium;
}

MediaCatalogWriteResult media_catalog_write_target(const MediaCatalogQuery& query,
                                                   Slice<u8> output) noexcept {
    const bool result_page = query.operation == MediaCatalogOperation::gif_search ||
                             query.operation == MediaCatalogOperation::gif_trending;
    const bool search = query.operation == MediaCatalogOperation::gif_search ||
                        query.operation == MediaCatalogOperation::gif_suggestions;
    if (((query.operation != MediaCatalogOperation::guild_stickers || query.locale.bytes) &&
         !locale_valid(query.locale)) ||
        !text_valid(query.query, kMediaCatalogMaximumQueryBytes, search) ||
        (!search && query.query.bytes) ||
        (result_page && query.limit &&
         (query.limit < kMediaCatalogMinimumGifPageItems || query.limit > kMediaCatalogMaximumPageItems)) ||
        (query.operation == MediaCatalogOperation::gif_suggestions &&
         (!query.limit || query.limit > kMediaCatalogMaximumSuggestions)) ||
        (query.operation == MediaCatalogOperation::gif_categories &&
         query.limit > kMediaCatalogMaximumPageItems) ||
        ((query.operation == MediaCatalogOperation::guild_stickers) != (query.guild_id != 0)))
        return {0, E::invalid_query};
    const ConstSlice<u8> target{output.data, output.count},
        q{reinterpret_cast<const u8*>(query.query.data), query.query.bytes},
        locale{reinterpret_cast<const u8*>(query.locale.data), query.locale.bytes};
    if (!byte_range_valid(target) || byte_ranges_overlap(target, q) || byte_ranges_overlap(target, locale))
        return {0, E::invalid_input};
    u8 temporary[kMediaCatalogMaximumTargetBytes];
    RouteWriter w{};
    route_writer_initialize(&w, {temporary, sizeof(temporary)});
    // The target is a complete origin-form path; the REST transport adds no API prefix.
    route_append(&w, utf8_literal("/api/v10"));
    switch (query.operation) {
    case MediaCatalogOperation::gif_categories:
        route_append(&w, utf8_literal("/gifs/trending"));
        break;
    case MediaCatalogOperation::gif_trending:
        route_append(&w, utf8_literal("/gifs/trending-gifs"));
        break;
    case MediaCatalogOperation::gif_search:
        route_append(&w, utf8_literal("/gifs/search"));
        break;
    case MediaCatalogOperation::gif_suggestions:
        route_append(&w, utf8_literal("/gifs/suggest"));
        break;
    case MediaCatalogOperation::sticker_packs:
        route_append(&w, utf8_literal("/sticker-packs"));
        break;
    case MediaCatalogOperation::guild_stickers:
        route_append(&w, utf8_literal("/guilds/"));
        route_append_u64(&w, query.guild_id);
        route_append(&w, utf8_literal("/stickers"));
        break;
    }
    if (query.operation != MediaCatalogOperation::guild_stickers) {
        route_append(&w, utf8_literal("?locale="));
        route_append_path_component(&w, query.locale);
        if (search) {
            route_append(&w, utf8_literal("&q="));
            route_append_path_component(&w, query.query);
        }
        if (query.operation <= MediaCatalogOperation::gif_search) {
            route_append(&w, utf8_literal("&media_format="));
            route_append(&w, query.format == MediaCatalogFormat::tiny_webp ? utf8_literal("tinywebp")
                                                                           : utf8_literal("webm"));
        }
        // Zero omits the limit (the server default), as the official client does.
        if ((result_page || query.operation == MediaCatalogOperation::gif_suggestions) && query.limit) {
            route_append(&w, utf8_literal("&limit="));
            route_append_u64(&w, query.limit);
        }
    }
    if (!w.valid || w.bytes > output.count)
        return {w.bytes, E::capacity};
    if (w.bytes)
        copy_bytes(output.data, temporary, w.bytes);
    return {w.bytes, E::none};
}

MediaCatalogDecodeResult media_catalog_measure(MediaCatalogOperation operation, Utf8View input,
                                               Slice<u32> scratch, u32 depth, u32 limit,
                                               u32 first) noexcept {
    MediaCatalogDecodeResult invalid{};
    invalid.error = E::invalid_input;
    const bool stickers = sticker_operation(operation);
    if (!limit)
        limit = item_limit(operation);
    if (!input.data || !input.bytes ||
        input.bytes >
            (stickers ? kMediaCatalogMaximumStickerResponseBytes : kMediaCatalogMaximumResponseBytes) ||
        !scratch.data || !scratch.count || scratch.count > kMediaCatalogStructuralCapacity || !depth ||
        depth > 32 || limit > item_limit(operation))
        return invalid;
    const ConstSlice<u8> source{reinterpret_cast<const u8*>(input.data), input.bytes},
        index{reinterpret_cast<const u8*>(scratch.data), scratch.count * static_cast<u32>(sizeof(u32))};
    if (!byte_range_valid(source) || !byte_range_valid(index) || byte_ranges_overlap(source, index))
        return invalid;
    const auto scanned = json_scan_structurals_avx1(input, scratch);
    if (!scanned.ok()) {
        invalid.error = E::scan;
        invalid.scan_error = scanned.error;
        invalid.error_offset = scanned.error_offset;
        return invalid;
    }
    Context c{};
    c.document = {input, {scratch.data, scanned.count}};
    c.depth = depth;
    c.limit = limit;
    c.first = first;
    c.text.count = stickers ? kMediaCatalogMaximumStickerTextBytes : kMediaCatalogMaximumTextBytes;
    if (!visit(c, operation))
        return c.result;
    c.result.item_count = c.selected;
    c.result.text_bytes = c.used;
    c.result.next_index = first < c.result.source_count ? first + c.selected : c.result.source_count;
    c.result.truncated = c.result.next_index < c.result.source_count;
    return c.result;
}

MediaCatalogDecodeResult media_catalog_decode(MediaCatalogOperation operation, Utf8View input,
                                              Slice<u32> scratch, u32 depth, Slice<MediaCatalogItem> items,
                                              Slice<char> text, u32 limit, u32 first) noexcept {
    MediaCatalogDecodeResult invalid{};
    invalid.error = E::invalid_input;
    const bool stickers = sticker_operation(operation);
    if (!limit)
        limit = item_limit(operation);
    if (!input.data || !input.bytes ||
        input.bytes >
            (stickers ? kMediaCatalogMaximumStickerResponseBytes : kMediaCatalogMaximumResponseBytes) ||
        !scratch.data || !scratch.count || scratch.count > kMediaCatalogStructuralCapacity || !depth ||
        depth > 32 || (!items.data && items.count) || items.count > item_limit(operation) ||
        (!text.data && text.count) ||
        text.count > (stickers ? kMediaCatalogMaximumStickerTextBytes : kMediaCatalogMaximumTextBytes) ||
        limit > item_limit(operation))
        return invalid;
    const ConstSlice<u8> ranges[]{
        {reinterpret_cast<const u8*>(input.data), input.bytes},
        {reinterpret_cast<const u8*>(scratch.data), scratch.count * static_cast<u32>(sizeof(u32))},
        {reinterpret_cast<const u8*>(items.data), items.count * static_cast<u32>(sizeof(MediaCatalogItem))},
        {reinterpret_cast<const u8*>(text.data), text.count}};
    for (u32 i = 0; i < 4; ++i) {
        if (!byte_range_valid(ranges[i]))
            return invalid;
        for (u32 j = 0; j < i; ++j)
            if (byte_ranges_overlap(ranges[i], ranges[j]))
                return invalid;
    }
    const auto scanned = json_scan_structurals_avx1(input, scratch);
    if (!scanned.ok()) {
        invalid.error = E::scan;
        invalid.scan_error = scanned.error;
        invalid.error_offset = scanned.error_offset;
        return invalid;
    }
    Context c{};
    c.document = {input, {scratch.data, scanned.count}};
    c.items = items;
    c.text = text;
    c.depth = depth;
    c.limit = minimum(limit, items.count);
    c.first = first;
    if (!visit(c, operation))
        return c.result;
    const auto measured_count = c.selected, measured_bytes = c.used, total = c.result.source_count;
    reset_value(&c.result);
    c.selected = c.used = 0;
    c.page_closed = false;
    c.write = true;
    c.limit = measured_count;
    if (!visit(c, operation))
        return c.result;
    c.result.item_count = measured_count;
    c.result.text_bytes = measured_bytes;
    c.result.next_index = first < total ? first + measured_count : total;
    c.result.truncated = c.result.next_index < total;
    return c.result;
}
} // namespace maicord
