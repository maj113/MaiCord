#include "maicord/app/packages/mcfx.hpp"
#include "maicord/app/packages/link.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {

constexpr u32 kHeaderBytes = 16, kEntryBytes = 16, kSectionCount = 6;
constexpr u32 kDepth = 6;
constexpr u32 kSectionCap[kSectionCount]{kMcfxManifestMaximumBytes, kMcfxBytecodeMaximumBytes,
                                         kMcfxTextureMaximumBytes,  kMcfxTextureMaximumBytes,
                                         kMcfxSourceMaximumBytes,   kMcfxPreviewMaximumBytes};

u32 le32(const u8* p) noexcept { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<u32>(p[3]) << 24; }
u32 be32(const u8* p) noexcept { return p[3] | p[2] << 8 | p[1] << 16 | static_cast<u32>(p[0]) << 24; }
bool tag(const u8* p, const char* ascii) noexcept { return bytes_equal(p, ascii, 4); }

// ---- DXBC: a container whose pixel-shader chunk carries the ps_5_0 version token ---------------------------
bool bytecode_valid(ConstSlice<u8> code) noexcept {
    const u8* p = code.data;
    if (code.count < 32 || !tag(p, "DXBC") || le32(p + 24) != code.count)
        return false;
    const u32 chunks = le32(p + 28);
    if (!chunks || chunks > 64 || 32 + chunks * 4 > code.count)
        return false;
    bool version = false;
    for (u32 i = 0; i < chunks; ++i) {
        const u32 at = le32(p + 32 + i * 4);
        if (at < 32 + chunks * 4 || at > code.count - 8 || (at & 3) || le32(p + at + 4) > code.count - at - 8)
            return false;
        if (tag(p + at, "SHEX") || tag(p + at, "SHDR")) {
            // The version token: pixel shader (type 0), 5.0.
            if (version || le32(p + at + 4) < 4 || le32(p + at + 8) != 0x50)
                return false;
            version = true;
        }
    }
    return version;
}

// ---- Text -------------------------------------------------------------------------------------------------
// No controls, bidi controls or zero-width characters (the catalog's text rule).
bool text_clean(const char* s, u32 n) noexcept {
    for (u32 i = 0; i < n; ++i) {
        const u8 c = static_cast<u8>(s[i]);
        if (c < 0x20 || c == 0x7f)
            return false;
        if (c == 0xe2 && i + 2 < n) {
            const u8 b = static_cast<u8>(s[i + 1]), d = static_cast<u8>(s[i + 2]);
            if ((b == 0x80 && ((d >= 0x8b && d <= 0x8f) || (d >= 0xaa && d <= 0xae))) || (b == 0x81 && d >= 0xa0 && d <= 0xa9))
                return false;
        }
        if (c == 0xef && i + 2 < n && static_cast<u8>(s[i + 1]) == 0xbb && static_cast<u8>(s[i + 2]) == 0xbf)
            return false;
        if (c == 0xc2 && i + 1 < n && static_cast<u8>(s[i + 1]) >= 0x80 && static_cast<u8>(s[i + 1]) <= 0x9f)
            return false;
    }
    return true;
}

bool in_set(char c, const char* set) noexcept {
    for (; *set; ++set)
        if (c == *set)
            return true;
    return false;
}
bool charset(const char* s, u32 n, bool lower, bool digit, bool upper, const char* extra) noexcept {
    for (u32 i = 0; i < n; ++i) {
        const char c = s[i];
        if (!((lower && c >= 'a' && c <= 'z') || (digit && c >= '0' && c <= '9') || (upper && c >= 'A' && c <= 'Z') ||
              in_set(c, extra)))
            return false;
    }
    return true;
}

struct Parse {
    JsonIndexedDocument document;
    McfxPackage* out;
    bool failed;
    u32 offset;
    u32 seen;
    u32 uses_texture_mask;
    u32 row, lane, scalars, colours, choice_knobs;
    // The knob being read.
    u32 knob_seen;
    JsonValue minimum, maximum, fallback, choices, type_value;
    McfxKnob knob;
    bool fail(u32 at) noexcept {
        failed = true;
        offset = at;
        return false;
    }
    bool read(JsonReadResult result) noexcept { return result.ok() || (failed ? false : fail(result.error_offset)); }
};

// Decodes a string value into `out` (NUL-terminated), 1..capacity bytes, clean text.
bool text_value(Parse& p, JsonValue value, char* out, u32 capacity, bool allow_empty = false) noexcept {
    JsonString s{};
    if (!json_value_as_string(value, &s))
        return p.fail(value.source_offset);
    const auto decoded = json_string_decode_utf8(s, {out, capacity});
    if (!decoded.ok() || (!decoded.bytes && !allow_empty) || !text_clean(out, decoded.bytes))
        return p.fail(value.source_offset);
    out[decoded.bytes] = 0;
    return true;
}

bool number_value(Parse& p, JsonValue value, float* out) noexcept {
    if (value.kind != JsonValueKind::number || value.source.bytes > 40)
        return p.fail(value.source_offset);
    const char* t = value.source.data;
    u32 i = 0;
    const bool negative = t[0] == '-';
    i += negative;
    double mantissa = 0;
    i32 scale = 0;
    bool fraction = false;
    for (; i < value.source.bytes && t[i] != 'e' && t[i] != 'E'; ++i) {
        if (t[i] == '.') {
            fraction = true;
            continue;
        }
        mantissa = mantissa * 10 + (t[i] - '0');
        scale -= fraction;
    }
    if (i < value.source.bytes) {
        ++i;
        const bool minus = t[i] == '-';
        i += (t[i] == '-' || t[i] == '+');
        i32 e = 0;
        for (; i < value.source.bytes; ++i)
            e = minimum(e * 10 + (t[i] - '0'), 400);
        scale += minus ? -e : e;
    }
    if (scale > 8 || scale < -40)
        return p.fail(value.source_offset);
    for (; scale > 0; --scale)
        mantissa *= 10;
    for (; scale < 0; ++scale)
        mantissa /= 10;
    const float result = static_cast<float>(negative ? -mantissa : mantissa);
    if (!is_finite(result) || result > 1e6f || result < -1e6f)
        return p.fail(value.source_offset);
    *out = result;
    return true;
}

bool whole(float v) noexcept { return v == static_cast<float>(static_cast<i32>(v)); }

bool hex_color(Parse& p, JsonValue value, u32* rgb) noexcept {
    char t[8];
    if (!text_value(p, value, t, 7) || c_string_length(t) != 7 || t[0] != '#')
        return p.fail(value.source_offset);
    u32 v = 0;
    for (u32 i = 1; i < 7; ++i) {
        const char c = t[i];
        const u32 d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 16;
        if (d == 16)
            return p.fail(value.source_offset);
        v = v << 4 | d;
    }
    *rgb = v;
    return true;
}

// ---- Knobs ------------------------------------------------------------------------------------------------
enum KnobMember : u32 { k_id, k_type, k_label, k_min, k_max, k_default, k_choices, k_hint };

bool choice_entry(void* context, u32 index, JsonValue value) noexcept {
    auto& p = *static_cast<Parse*>(context);
    if (index >= kMcfxChoiceMaximum)
        return p.fail(value.source_offset);
    if (!text_value(p, value, p.out->choices[p.choice_knobs][index], kMcfxChoiceBytes))
        return false;
    ++p.knob.choice_count;
    return true;
}

bool knob_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<Parse*>(context);
    static constexpr const char* names[]{"id", "type", "label", "min", "max", "default", "choices", "hint"};
    u32 id = 8;
    for (u32 i = 0; i < 8; ++i)
        if (json_string_equals_ascii_bytes(name.encoded.data, name.encoded.bytes, names[i], c_string_length(names[i])))
            id = i;
    if (id == 8 || (p.knob_seen & 1u << id))
        return p.fail(name.source_offset);
    p.knob_seen |= 1u << id;
    auto& k = p.knob;
    switch (id) {
    case k_id:
        return text_value(p, value, k.id, kMcfxIdBytes) && k.id[0] >= 'a' && k.id[0] <= 'z' &&
                       charset(k.id, c_string_length(k.id), true, true, false, "_")
                   ? true
                   : p.fail(value.source_offset);
    case k_label: return text_value(p, value, k.label, kMcfxLabelBytes);
    case k_hint: return text_value(p, value, k.hint, kMcfxHintBytes);
    case k_type: p.type_value = value; return true;
    case k_min: p.minimum = value; return true;
    case k_max: p.maximum = value; return true;
    case k_default: p.fallback = value; return true;
    default: p.choices = value; return true;
    }
}

bool finish_knob(Parse& p, u32 at) noexcept {
    auto& k = p.knob;
    JsonString type{};
    if (!(p.knob_seen & 1u << k_type) || !json_value_as_string(p.type_value, &type))
        return p.fail(at);
    static constexpr const char* names[]{"float", "int", "toggle", "color", "choice"};
    u32 t = 5;
    for (u32 i = 0; i < 5; ++i)
        if (json_string_equals_ascii_bytes(type.encoded.data, type.encoded.bytes, names[i], c_string_length(names[i])))
            t = i;
    if (t == 5)
        return p.fail(at);
    k.type = static_cast<McfxKnobType>(t);
    constexpr u32 base = 1u << k_id | 1u << k_type | 1u << k_label | 1u << k_default;
    const u32 need = t <= 1 ? base | 1u << k_min | 1u << k_max : t == 4 ? base | 1u << k_choices : base;
    if ((p.knob_seen & ~(1u << k_hint)) != need)
        return p.fail(at);
    if (t <= 1) {
        if (!number_value(p, p.minimum, &k.minimum) || !number_value(p, p.maximum, &k.maximum) ||
            !number_value(p, p.fallback, &k.fallback) || !(k.minimum < k.maximum) || k.fallback < k.minimum ||
            k.fallback > k.maximum || (t == 1 && !(whole(k.minimum) && whole(k.maximum) && whole(k.fallback))))
            return p.fail(at);
    } else if (t == 2) {
        bool on{};
        if (!json_value_to_bool(p.fallback, &on))
            return p.fail(at);
        k.minimum = 0;
        k.maximum = 1;
        k.fallback = on;
    } else if (t == 3) {
        if (!hex_color(p, p.fallback, &k.rgb))
            return false;
    } else {
        if (p.choices.kind != JsonValueKind::array || p.choice_knobs == kMcfxChoiceKnobMaximum ||
            !p.read(json_visit_indexed_array(p.document, p.choices, kDepth, choice_entry, &p)) || k.choice_count < 2)
            return p.failed ? false : p.fail(at);
        u64 index{};
        if (!json_value_to_u64(p.fallback, &index) || index >= k.choice_count)
            return p.fail(at);
        k.choice_index = static_cast<u8>(p.choice_knobs++);
        k.minimum = 0;
        k.maximum = static_cast<float>(k.choice_count - 1);
        k.fallback = static_cast<float>(index);
    }
    // Layout: scalars pack four to a row in declaration order; a colour takes the next row's .rgb.
    if (t == 3) {
        if (p.lane)
            ++p.row, p.lane = 0;
        k.slot = static_cast<u8>(p.row * 4);
        ++p.row;
        ++p.colours;
    } else {
        k.slot = static_cast<u8>(p.row * 4 + p.lane);
        if (++p.lane == 4)
            ++p.row, p.lane = 0;
        p.scalars += t != 4;
    }
    if (p.row + (p.lane != 0) > kEffectKnobRows || p.scalars > 16 || p.colours > 4)
        return p.fail(at);
    auto& list = *p.out;
    for (u32 i = 0; i < list.knob_count; ++i)
        if (c_string_length(list.knobs[i].id) == c_string_length(k.id) &&
            bytes_equal(list.knobs[i].id, k.id, c_string_length(k.id)))
            return p.fail(at);
    list.knobs[list.knob_count++] = k;
    return true;
}

bool knob_entry(void* context, u32 index, JsonValue value) noexcept {
    auto& p = *static_cast<Parse*>(context);
    if (index >= kMcfxKnobMaximum || value.kind != JsonValueKind::object)
        return p.fail(value.source_offset);
    p.knob = {};
    p.knob_seen = 0;
    const u32 choices_before = p.choice_knobs;
    static_cast<void>(choices_before);
    return p.read(json_visit_indexed_object(p.document, value, kDepth, knob_member, &p)) &&
           finish_knob(p, value.source_offset);
}

// ---- Manifest members -------------------------------------------------------------------------------------
bool author_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<Parse*>(context);
    char text[kMcfxCreditNameBytes + 1];
    if (json_string_equals_ascii(name, utf8_literal("name"))) {
        if (p.seen & 0x10000 || !text_value(p, value, text, kMcfxCreditNameBytes))
            return p.failed ? false : p.fail(value.source_offset);
        p.seen |= 0x10000;
        auto& out = *p.out;
        const u32 have = c_string_length(out.authors), add = c_string_length(text), sep = have ? 2 : 0;
        if (have + sep + add > kMcfxAuthorsBytes)
            return p.fail(value.source_offset);
        if (sep)
            copy_bytes(out.authors + have, ", ", 2);
        copy_bytes(out.authors + have + sep, text, add + 1);
        return true;
    }
    if (json_string_equals_ascii(name, utf8_literal("github")) && !(p.seen & 0x20000)) {
        p.seen |= 0x20000;
        char login[40];
        return text_value(p, value, login, 39) && charset(login, c_string_length(login), true, true, true, "-")
                   ? true
                   : p.fail(value.source_offset);
    }
    return p.fail(name.source_offset);
}
bool author_entry(void* context, u32 index, JsonValue value) noexcept {
    auto& p = *static_cast<Parse*>(context);
    if (index >= 4 || value.kind != JsonValueKind::object)
        return p.fail(value.source_offset);
    p.seen &= ~0x30000u;
    return p.read(json_visit_indexed_object(p.document, value, kDepth, author_member, &p)) && (p.seen & 0x10000
                                                                                               ? true
                                                                                               : p.fail(value.source_offset));
}
bool credit_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<Parse*>(context);
    auto& c = p.out->derived_from;
    u32 bit = 0;
    if (json_string_equals_ascii(name, utf8_literal("url"))) bit = 1;
    else if (json_string_equals_ascii(name, utf8_literal("author"))) bit = 2;
    else if (json_string_equals_ascii(name, utf8_literal("license"))) bit = 4;
    if (!bit || (p.seen & bit << 20))
        return p.fail(name.source_offset);
    p.seen |= bit << 20;
    if (bit == 1)
        return text_value(p, value, c.url, kMcfxCreditUrlBytes) && c.url[0] == 'h' && c.url[1] == 't' && c.url[2] == 't' &&
                       c.url[3] == 'p' && c.url[4] == 's' && c.url[5] == ':' && c.url[6] == '/' && c.url[7] == '/'
                   ? true
                   : p.fail(value.source_offset);
    return bit == 2 ? text_value(p, value, c.author, kMcfxCreditNameBytes) : text_value(p, value, c.license, kMcfxLicenseBytes);
}
bool use_entry(void* context, u32 index, JsonValue value) noexcept {
    auto& p = *static_cast<Parse*>(context);
    JsonString s{};
    static constexpr const char* names[]{"noise", "glyphs", "blue_noise", "pointer", "texture0", "texture1"};
    if (index >= 6 || !json_value_as_string(value, &s))
        return p.fail(value.source_offset);
    for (u32 i = 0; i < 6; ++i)
        if (json_string_equals_ascii_bytes(s.encoded.data, s.encoded.bytes, names[i], c_string_length(names[i]))) {
            if (p.out->uses & 1u << i)
                return p.fail(value.source_offset);
            p.out->uses |= 1u << i; // EffectUse's bits are 1 << index in this order
            return true;
        }
    return p.fail(value.source_offset);
}
static_assert(effect_use_noise == 1 && effect_use_glyphs == 2 && effect_use_blue_noise == 4 && effect_use_pointer == 8 &&
              effect_use_texture0 == 16 && effect_use_texture1 == 32);

enum RootMember : u32 { r_interface, r_slug, r_name, r_version, r_authors, r_license, r_derived, r_blend, r_uses, r_resolution, r_fps, r_knobs };
constexpr u32 kRootRequired = 1u << r_interface | 1u << r_slug | 1u << r_name | 1u << r_version | 1u << r_authors |
                              1u << r_license | 1u << r_blend | 1u << r_uses | 1u << r_knobs;

bool root_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<Parse*>(context);
    auto& out = *p.out;
    static constexpr const char* names[]{"interface", "slug", "name", "version", "authors", "license",
                                          "derived_from", "blend", "uses", "resolution", "fps", "knobs"};
    u32 id = 12;
    for (u32 i = 0; i < 12; ++i)
        if (json_string_equals_ascii_bytes(name.encoded.data, name.encoded.bytes, names[i], c_string_length(names[i])))
            id = i;
    if (id == 12 || (p.seen & 1u << id))
        return p.fail(name.source_offset);
    p.seen |= 1u << id;
    switch (id) {
    case r_interface: {
        u64 v{};
        return json_value_to_u64(value, &v) && v == 1 ? true : p.fail(value.source_offset);
    }
    case r_slug:
        return text_value(p, value, out.slug, kMcfxSlugBytes) && package_slug_valid({out.slug, c_string_length(out.slug)})
                   ? true
                   : p.fail(value.source_offset);
    case r_name: return text_value(p, value, out.name, kMcfxNameBytes);
    case r_version:
        return text_value(p, value, out.version, kMcfxVersionBytes) &&
                       charset(out.version, c_string_length(out.version), true, true, true, ".+-")
                   ? true
                   : p.fail(value.source_offset);
    case r_license:
        return text_value(p, value, out.license, kMcfxLicenseBytes) &&
                       charset(out.license, c_string_length(out.license), true, true, true, ".+-")
                   ? true
                   : p.fail(value.source_offset);
    case r_authors:
        return value.kind == JsonValueKind::array && p.read(json_visit_indexed_array(p.document, value, kDepth, author_entry, &p)) &&
                       out.authors[0]
                   ? true
                   : p.failed ? false : p.fail(value.source_offset);
    case r_derived:
        p.seen &= ~(7u << 20);
        out.has_credit = true;
        return value.kind == JsonValueKind::object && p.read(json_visit_indexed_object(p.document, value, kDepth, credit_member, &p)) &&
                       (p.seen >> 20 & 7) == 7
                   ? true
                   : p.failed ? false : p.fail(value.source_offset);
    case r_blend: {
        JsonString s{};
        static constexpr const char* b[]{"normal", "screen", "add"};
        if (!json_value_as_string(value, &s))
            return p.fail(value.source_offset);
        for (u32 i = 0; i < 3; ++i)
            if (json_string_equals_ascii_bytes(s.encoded.data, s.encoded.bytes, b[i], c_string_length(b[i]))) {
                out.blend = static_cast<EffectBlend>(i);
                return true;
            }
        return p.fail(value.source_offset);
    }
    case r_uses:
        return value.kind == JsonValueKind::array && p.read(json_visit_indexed_array(p.document, value, kDepth, use_entry, &p));
    case r_resolution:
    case r_fps: {
        u64 v{};
        const bool resolution = id == r_resolution;
        const u8* table = resolution ? kEffectResolutionPercent : kEffectFramesPerSecond;
        if (!json_value_to_u64(value, &v))
            return p.fail(value.source_offset);
        for (u32 i = 0; i < 5; ++i)
            if (table[i] == v) {
                (resolution ? out.requested_resolution : out.requested_frame_rate) = static_cast<u8>(i + 1);
                return true;
            }
        return p.fail(value.source_offset);
    }
    default:
        return value.kind == JsonValueKind::array && p.read(json_visit_indexed_array(p.document, value, kDepth, knob_entry, &p));
    }
}

McfxResult manifest_parse(ConstSlice<u8> text, McfxWorkspace* w) noexcept {
    const Utf8View input{reinterpret_cast<const char*>(text.data), text.count};
    const auto scanned = json_scan_structurals_avx1(input, {w->structurals, kMcfxManifestMaximumBytes});
    if (!scanned.ok())
        return {McfxError::manifest, scanned.error_offset};
    Parse p{};
    p.document = {input, {w->structurals, scanned.count}};
    p.out = &w->package;
    const auto read = json_visit_root_object(input, p.document.structurals, kDepth, root_member, &p);
    if (!read.ok())
        return {McfxError::manifest, p.failed ? p.offset : read.error_offset};
    if ((p.seen & kRootRequired) != kRootRequired)
        return {McfxError::manifest, 0};
    w->package.rows = static_cast<u8>(p.row + (p.lane != 0));
    return {};
}

// ---- PNG --------------------------------------------------------------------------------------------------
bool png_header(ConstSlice<u8> png, u32* width, u32* height, u32* depth, u32* colour) noexcept {
    static constexpr u8 signature[8]{0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
    if (png.count < 33 || !bytes_equal(png.data, signature, 8) || be32(png.data + 8) != 13 || !tag(png.data + 12, "IHDR"))
        return false;
    *width = be32(png.data + 16);
    *height = be32(png.data + 20);
    *depth = png.data[24];
    *colour = png.data[25];
    return *width && *height;
}

} // namespace

bool mcfx_png_extent(ConstSlice<u8> png, u32* width, u32* height) noexcept {
    u32 depth{}, colour{};
    return png_header(png, width, height, &depth, &colour) && *width <= kEffectTextureMaximumExtent &&
           *height <= kEffectTextureMaximumExtent;
}

McfxResult mcfx_parse(ConstSlice<u8> file, McfxWorkspace* w) noexcept {
    if (!w || (!file.data && file.count))
        return {McfxError::invalid_input, 0};
    clear_bytes(&w->package, sizeof(w->package));
    if (file.count > kMcfxMaximumBytes)
        return {McfxError::oversize, 0};
    if (file.count < kHeaderBytes)
        return {McfxError::truncated, 0};
    const u8* f = file.data;
    const u32 count = f[6] | f[7] << 8;
    if (!tag(f, "MCFX") || (f[4] | f[5] << 8) != 1 || !count || count > kSectionCount || le32(f + 8) != file.count ||
        le32(f + 12) != 0)
        return {McfxError::header, 0};
    const u32 table_end = kHeaderBytes + count * kEntryBytes;
    if (file.count < table_end)
        return {McfxError::truncated, kHeaderBytes};
    ConstSlice<u8> part[kSectionCount]{};
    u32 at = table_end, previous = 0;
    for (u32 i = 0; i < count; ++i) {
        const u8* e = f + kHeaderBytes + i * kEntryBytes;
        const u32 type = le32(e), offset = le32(e + 4), bytes = le32(e + 8);
        if (le32(e + 12) != 0 || type < 1 || type > kSectionCount || type <= previous)
            return {McfxError::section, kHeaderBytes + i * kEntryBytes};
        if (bytes > kSectionCap[type - 1] || !bytes)
            return {McfxError::oversize, kHeaderBytes + i * kEntryBytes};
        if (offset != at)
            return {McfxError::section, kHeaderBytes + i * kEntryBytes + 4};
        if (bytes > file.count - at)
            return {McfxError::truncated, offset};
        part[type - 1] = {f + offset, bytes};
        at += bytes;
        previous = type;
    }
    if (at != file.count)
        return {McfxError::section, at};
    if (!part[0].data || !part[1].data)
        return {McfxError::section, 0};
    if (!bytecode_valid(part[1]))
        return {McfxError::bytecode, 0};
    for (u32 i = 0; i < 2; ++i)
        if (part[2 + i].data) {
            u32 width{}, height{}, depth{}, colour{};
            if (!png_header(part[2 + i], &width, &height, &depth, &colour) || width > kEffectTextureMaximumExtent ||
                height > kEffectTextureMaximumExtent || depth != 8 || (colour != 2 && colour != 6) ||
                part[2 + i].data[28] > 1)
                return {McfxError::texture, i};
        }
    if (part[5].data) {
        u32 width{}, height{}, depth{}, colour{};
        if (!png_header(part[5], &width, &height, &depth, &colour) || width > 2048 || height > 2048)
            return {McfxError::preview, 0};
    }
    const auto manifest = manifest_parse(part[0], w);
    if (!manifest.ok())
        return manifest;
    auto& package = w->package;
    // The texture inputs and sections go together.
    if (!!(package.uses & effect_use_texture0) != !!part[2].data || !!(package.uses & effect_use_texture1) != !!part[3].data)
        return {McfxError::manifest, 0};
    package.bytecode = part[1];
    package.texture[0] = part[2];
    package.texture[1] = part[3];
    package.source = part[4];
    package.preview = part[5];
    package.file_bytes = file.count;
    return {};
}

bool mcfx_knob_value_valid(const McfxKnob& knob, const McfxKnobValue& value) noexcept {
    switch (knob.type) {
    case McfxKnobType::color: return value.rgb <= 0xffffffu;
    case McfxKnobType::number: return is_finite(value.number) && value.number >= knob.minimum && value.number <= knob.maximum;
    default:
        return is_finite(value.number) && value.number >= knob.minimum && value.number <= knob.maximum && whole(value.number);
    }
}

void mcfx_knob_defaults(const McfxPackage& package, McfxKnobValue (&values)[kMcfxKnobMaximum]) noexcept {
    for (u32 i = 0; i < kMcfxKnobMaximum; ++i)
        values[i] = i < package.knob_count ? McfxKnobValue{package.knobs[i].fallback, package.knobs[i].rgb} : McfxKnobValue{};
}

void mcfx_knob_pack(const McfxPackage& package, const McfxKnobValue (&values)[kMcfxKnobMaximum],
                    float (&rows)[kEffectKnobRows][4]) noexcept {
    clear_bytes(rows, sizeof(rows));
    for (u32 i = 0; i < package.knob_count; ++i) {
        const auto& k = package.knobs[i];
        float* lane = &rows[k.slot / 4][k.slot % 4];
        if (k.type == McfxKnobType::color) {
            lane[0] = static_cast<float>(values[i].rgb >> 16 & 255) * (1.f / 255.f);
            lane[1] = static_cast<float>(values[i].rgb >> 8 & 255) * (1.f / 255.f);
            lane[2] = static_cast<float>(values[i].rgb & 255) * (1.f / 255.f);
        } else {
            lane[0] = values[i].number;
        }
    }
}

} // namespace maicord
