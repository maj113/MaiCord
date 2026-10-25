#include "maicord/app/update_changelog.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/decimal.hpp"

namespace maicord {
namespace {
enum Field : u32 { format, product, channel, version, base, entries, field_count };
enum EntryField : u32 { entry_version, entry_date, entry_commit, entry_text, entry_field_count };
struct Parse {
    JsonIndexedDocument document;
    UpdateChangelog* log;
    JsonValue entries;
    u32 seen;
    UpdateChangelogError error;
};
struct EntryParse {
    Parse* parse;
    UpdateChangelogEntry entry;
    u32 seen;
};
bool fail(Parse& parse, UpdateChangelogError error) noexcept {
    if (parse.error == UpdateChangelogError::none) parse.error = error;
    return false;
}
bool plain_string(JsonValue value, Utf8View* output) noexcept {
    JsonString parsed{};
    if (!json_value_as_string(value, &parsed) || parsed.has_escapes) return false;
    *output = parsed.encoded;
    return true;
}
bool hex_digit(char c) noexcept { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }
// Keeps valid UTF-8 without C0/C1 controls (except '\n'; tabs become spaces),
// DEL or bidirectional embeddings, overrides and isolates. Returns the length.
u32 sanitize(char* text, u32 bytes) noexcept {
    u32 out = 0;
    for (u32 at = 0; at < bytes;) {
        const auto decoded = utf8_decode_one(text + at, bytes - at);
        if (!decoded.valid || !decoded.width) { ++at; continue; }
        const u32 c = decoded.codepoint;
        const bool control = (c < 0x20 && c != '\n' && c != '\t') || (c >= 0x7f && c < 0xa0);
        const bool bidi = (c >= 0x202a && c <= 0x202e) || (c >= 0x2066 && c <= 0x2069) || c == 0x200e || c == 0x200f;
        if (c == '\t') text[out++] = ' ';
        else if (!control && !bidi)
            for (u32 i = 0; i < decoded.width; ++i) text[out++] = text[at + i];
        at += decoded.width;
    }
    return out;
}
bool visit_entry(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& e = *static_cast<EntryParse*>(opaque);
    auto& parse = *e.parse;
    static constexpr Utf8View names[]{utf8_literal("version"), utf8_literal("date"), utf8_literal("commit"), utf8_literal("text")};
    u32 field = entry_field_count;
    for (u32 i = 0; i < entry_field_count; ++i)
        if (json_string_equals_ascii(name, names[i])) { field = i; break; }
    if (field == entry_field_count || (e.seen & (1u << field))) return fail(parse, UpdateChangelogError::field);
    e.seen |= 1u << field;
    switch (field) {
    case entry_version: return json_value_to_u64(value, &e.entry.version) || fail(parse, UpdateChangelogError::field);
    case entry_date: return json_value_to_u64(value, &e.entry.date_unix_seconds) || fail(parse, UpdateChangelogError::field);
    case entry_commit: {
        Utf8View text{};
        if (!plain_string(value, &text) || (text.bytes != 0 && text.bytes != 40)) return fail(parse, UpdateChangelogError::field);
        for (u32 i = 0; i < text.bytes; ++i)
            if (!hex_digit(text.data[i])) return fail(parse, UpdateChangelogError::field);
        copy_bytes(e.entry.commit, text.data, text.bytes);
        e.entry.has_commit = text.bytes == 40;
        return true;
    }
    default: {
        JsonString text{};
        if (!json_value_as_string(value, &text)) return fail(parse, UpdateChangelogError::field);
        auto& log = *parse.log;
        const auto size = json_string_decode_utf8(text, {});
        if (size.error != JsonStringDecodeError::none && size.error != JsonStringDecodeError::output_too_small)
            return fail(parse, UpdateChangelogError::field);
        if (!size.bytes || size.bytes > kUpdateChangelogTextMaximumBytes || size.bytes > sizeof(log.text) - log.text_bytes)
            return fail(parse, UpdateChangelogError::bounds);
        const auto decoded = json_string_decode_utf8(text, {log.text + log.text_bytes, size.bytes});
        if (!decoded.ok()) return fail(parse, UpdateChangelogError::field);
        const u32 kept = sanitize(log.text + log.text_bytes, decoded.bytes);
        if (!kept) return fail(parse, UpdateChangelogError::bounds);
        e.entry.text_offset = log.text_bytes;
        e.entry.text_bytes = kept;
        log.text_bytes += kept;
        return true;
    }
    }
}
bool visit_entries(void* opaque, u32, JsonValue value) noexcept {
    auto& parse = *static_cast<Parse*>(opaque);
    auto& log = *parse.log;
    if (value.kind != JsonValueKind::object) return fail(parse, UpdateChangelogError::field);
    if (log.count == kUpdateChangelogMaximumEntries) return fail(parse, UpdateChangelogError::bounds);
    EntryParse e{&parse, {}, 0};
    if (!json_visit_indexed_object(parse.document, value, 2, visit_entry, &e).ok()) return fail(parse, UpdateChangelogError::json);
    if (e.seen != (1u << entry_field_count) - 1u) return fail(parse, UpdateChangelogError::missing_field);
    const u64 newer = log.count ? log.entries[log.count - 1].version : log.version;
    if (e.entry.version <= log.base || e.entry.version > newer) return fail(parse, UpdateChangelogError::bounds);
    log.entries[log.count++] = e.entry;
    return true;
}
bool visit_root(void* opaque, JsonString name, JsonValue value) noexcept {
    auto& parse = *static_cast<Parse*>(opaque);
    auto& log = *parse.log;
    static constexpr Utf8View names[]{utf8_literal("format"), utf8_literal("product"), utf8_literal("channel"),
        utf8_literal("version"), utf8_literal("base"), utf8_literal("entries")};
    u32 field = field_count;
    for (u32 i = 0; i < field_count; ++i)
        if (json_string_equals_ascii(name, names[i])) { field = i; break; }
    if (field == field_count || (parse.seen & (1u << field))) return fail(parse, UpdateChangelogError::field);
    parse.seen |= 1u << field;
    Utf8View text{};
    u64 number = 0;
    bool valid = true;
    switch (field) {
    case format: valid = json_value_to_u64(value, &number) && number == 1; break;
    case product: valid = plain_string(value, &text) && utf8_equal(text, utf8_literal("MaiCord")); break;
    case channel:
        valid = plain_string(value, &text);
        if (valid && utf8_equal(text, utf8_literal("stable"))) log.channel = UpdateChannel::stable;
        else if (valid && utf8_equal(text, utf8_literal("preview"))) log.channel = UpdateChannel::preview;
        else valid = false;
        break;
    case version: valid = json_value_to_u64(value, &log.version) && log.version; break;
    case base: valid = json_value_to_u64(value, &log.base); break;
    default: valid = value.kind == JsonValueKind::array; parse.entries = value; break;
    }
    return valid || fail(parse, UpdateChangelogError::field);
}
} // namespace

UpdateChangelogError update_changelog_verify(Utf8View document, ConstSlice<u8> signature, const P256Key& trusted_key,
    KdfSha256Workspace& workspace, Slice<u32> scratch, UpdateChangelog* output) noexcept {
    if (!output) return UpdateChangelogError::invalid_input;
    output->count = 0; output->version = 0; output->text_bytes = 0;
    if (!document.data || !document.bytes || document.bytes > kUpdateChangelogMaximumBytes || signature.count != 64 ||
        !trusted_key.handle || trusted_key.kind != P256Kind::ecdsa || !workspace.storage.data || !scratch.data || !scratch.count)
        return UpdateChangelogError::invalid_input;
    const ConstSlice<u8> parts[]{
        {reinterpret_cast<const u8*>(kUpdateChangelogSignatureDomain.data), kUpdateChangelogSignatureDomain.bytes},
        {reinterpret_cast<const u8*>(document.data), document.bytes}};
    u8 hashed[32]{};
    if (!sha256(workspace, {parts, 2}, {hashed, sizeof(hashed)}).ok() ||
        !p256_verify_digest(trusted_key, {hashed, sizeof(hashed)}, signature).ok())
        return UpdateChangelogError::signature;
    const auto scan = json_scan_structurals_avx1(document, scratch);
    if (!scan.ok()) return UpdateChangelogError::json;
    Parse parse{{document, {scratch.data, scan.count}}, output, {}, 0, UpdateChangelogError::none};
    const auto read = json_visit_root_object(document, {scratch.data, scan.count}, 4, visit_root, &parse);
    UpdateChangelogError error = parse.error;
    if (error == UpdateChangelogError::none && !read.ok()) error = UpdateChangelogError::json;
    if (error == UpdateChangelogError::none && parse.seen != (1u << field_count) - 1u) error = UpdateChangelogError::missing_field;
    if (error == UpdateChangelogError::none && output->base >= output->version) error = UpdateChangelogError::bounds;
    if (error == UpdateChangelogError::none &&
        !json_visit_indexed_array(parse.document, parse.entries, 3, visit_entries, &parse).ok())
        error = parse.error == UpdateChangelogError::none ? UpdateChangelogError::json : parse.error;
    if (error != UpdateChangelogError::none) { output->count = 0; output->version = 0; output->text_bytes = 0; }
    return error;
}

namespace {
struct Out {
    Slice<char> out;
    u32 used;
    void put(const char* text, u32 bytes) noexcept {
        // Never split a character: drop a sequence that doesn't fit whole.
        if (bytes > out.count - used) return;
        copy_bytes(out.data + used, text, bytes);
        used += bytes;
    }
    void put(Utf8View text) noexcept { put(text.data, text.bytes); }
};
bool starts(Utf8View line, u32 at, const char* prefix, u32 bytes) noexcept {
    return line.bytes - at >= bytes && bytes_equal(line.data + at, prefix, bytes);
}
// Inline Markdown: links keep their text, emphasis and code markers go.
void inline_text(Out& o, Utf8View line, u32 at) noexcept {
    while (at < line.bytes) {
        const char c = line.data[at];
        if (c == '`' || (c == '*' && at + 1 < line.bytes && line.data[at + 1] == '*') ||
            (c == '_' && at + 1 < line.bytes && line.data[at + 1] == '_')) {
            at += c == '`' ? 1 : 2;
            continue;
        }
        if (c == '[') {
            u32 close = at + 1;
            while (close < line.bytes && line.data[close] != ']') ++close;
            if (close + 1 < line.bytes && line.data[close + 1] == '(') {
                u32 end = close + 2;
                while (end < line.bytes && line.data[end] != ')') ++end;
                if (end < line.bytes) {
                    inline_text(o, {line.data, close}, at + 1);
                    at = end + 1;
                    continue;
                }
            }
        }
        const auto decoded = utf8_decode_one(line.data + at, line.bytes - at);
        const u32 width = decoded.valid && decoded.width ? decoded.width : 1u;
        o.put(line.data + at, width);
        at += width;
    }
}
} // namespace

u32 update_changelog_display_text(Utf8View text, Slice<char> output) noexcept {
    if (!output.data || !text.data) return 0;
    Out o{output, 0};
    bool blank = true, any = false;
    for (u32 start = 0; start <= text.bytes;) {
        u32 end = start;
        while (end < text.bytes && text.data[end] != '\n') ++end;
        Utf8View line{text.data + start, end - start};
        while (line.bytes && line.data[line.bytes - 1] == ' ') --line.bytes;
        u32 at = 0;
        while (at < line.bytes && line.data[at] == ' ') ++at;
        if (at == line.bytes) {
            blank = true;
        } else {
            if (any) o.put(blank ? "\n\n" : "\n", blank ? 2 : 1);
            blank = false; any = true;
            if (line.data[at] == '#') {
                while (at < line.bytes && line.data[at] == '#') ++at;
                while (at < line.bytes && line.data[at] == ' ') ++at;
            } else if (starts(line, at, "- ", 2) || starts(line, at, "* ", 2)) {
                o.put(utf8_literal("\xE2\x80\xA2 "));
                at += 2;
            }
            inline_text(o, line, at);
        }
        start = end + 1;
    }
    return o.used;
}

u32 update_changelog_date_text(u64 unix_seconds, bool year, Slice<char> output) noexcept {
    // Civil date from days since 1970-01-01 (Howard Hinnant's algorithm).
    const i64 days = static_cast<i64>(unix_seconds / 86400) + 719468;
    const i64 era = days / 146097;
    const i64 day_of_era = days - era * 146097;
    const i64 year_of_era = (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    const i64 day_of_year = day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const i64 month_index = (5 * day_of_year + 2) / 153;
    const u32 day = static_cast<u32>(day_of_year - (153 * month_index + 2) / 5 + 1);
    const u32 month = static_cast<u32>(month_index < 10 ? month_index + 3 : month_index - 9);
    const u64 civil_year = static_cast<u64>(year_of_era + era * 400 + (month <= 2 ? 1 : 0));
    static constexpr char names[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char text[24];
    u32 used = 0;
    copy_bytes(text, names + (month - 1) * 3, 3);
    used = 3;
    text[used++] = ' ';
    used += decimal_write(text + used, day);
    if (year) {
        text[used++] = ',';
        text[used++] = ' ';
        used += decimal_write(text + used, civil_year);
    }
    if (!output.data || used > output.count) return 0;
    copy_bytes(output.data, text, used);
    return used;
}

} // namespace maicord
