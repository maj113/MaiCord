#include "maicord/app/packages/mcfx.hpp"
#include "maicord/app/packages/link.hpp"
#include "maicord/core/color_hex.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/core/json_scan.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/name_list.hpp"

namespace maicord {
namespace {

constexpr u32 kHeaderBytes = 16, kEntryBytes = 16, kSectionCount = 9;
constexpr u32 kDepth = 6;
constexpr u32 kSectionCap[kSectionCount]{kMcfxManifestMaximumBytes, kMcfxBytecodeMaximumBytes,
                                         kMcfxTextureMaximumBytes,  kMcfxTextureMaximumBytes,
                                         kMcfxSourceMaximumBytes,   kMcfxPreviewMaximumBytes,
                                         kMcfxTextureMaximumBytes,  kMcfxTextureMaximumBytes,
                                         kMcfxShadersMaximumBytes};
// TEXTURE0..3: their sections (part index) and inputs.
constexpr u8 kTexturePart[kEffectTextureCount]{2, 3, 6, 7};
constexpr u32 kTextureUse[kEffectTextureCount]{effect_use_texture0, effect_use_texture1, effect_use_texture2,
                                               effect_use_texture3};
// Shader model 5.0 version tokens: program type in the high half (0 pixel, 1 vertex).
constexpr u32 kPixel50 = 0x50, kVertex50 = 0x10050;

u32 le32(const u8* p) noexcept { return p[0] | p[1] << 8 | p[2] << 16 | static_cast<u32>(p[3]) << 24; }
u32 be32(const u8* p) noexcept { return p[3] | p[2] << 8 | p[1] << 16 | static_cast<u32>(p[0]) << 24; }
bool tag(const u8* p, const char* ascii) noexcept { return bytes_equal(p, ascii, 4); }

// ---- DXBC: a container with one shader chunk; its version token, or 0 when the container is not sound ------
u32 bytecode_version(ConstSlice<u8> code) noexcept {
    const u8* p = code.data;
    if (code.count < 32 || !tag(p, "DXBC") || le32(p + 24) != code.count)
        return 0;
    const u32 chunks = le32(p + 28);
    if (!chunks || chunks > 64 || 32 + chunks * 4 > code.count)
        return 0;
    u32 version = 0;
    for (u32 i = 0; i < chunks; ++i) {
        const u32 at = le32(p + 32 + i * 4);
        if (at < 32 + chunks * 4 || at > code.count - 8 || (at & 3) || le32(p + at + 4) > code.count - at - 8)
            return 0;
        if (tag(p + at, "SHEX") || tag(p + at, "SHDR")) {
            if (version || le32(p + at + 4) < 4 || !le32(p + at + 8))
                return 0;
            version = le32(p + at + 8);
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

// The position of a JSON name among packed names (name_list.hpp), or their count when it is none.
u32 name_index(const JsonString& name, const char* names) noexcept {
    u32 index = 0;
    for (NameList list{names}; list; list.next(), ++index)
        if (json_string_equals_ascii(name, list.name()))
            break;
    return index;
}
// The position of a string value among packed names; 0xff when the value is not a string.
u32 value_index(JsonValue value, const char* names) noexcept {
    JsonString s{};
    return json_value_as_string(value, &s) ? name_index(s, names) : 0xff;
}

struct Parse {
    JsonIndexedDocument document;
    McfxPackage* out;
    bool failed;
    u32 offset;
    u32 seen;
    u32 row, lane, scalars, colours, choice_knobs;
    u32 interface_version;
    // The members seen of the knob, target or pass being read.
    u32 knob_seen;
    JsonValue minimum, maximum, fallback, choices, type_value;
    McfxKnob knob;
    // A number or a small array of them (numbers_value).
    u16 numbers[4];
    u32 number_count;
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
    if (!text_value(p, value, t, 7) || c_string_length(t) != 7 || t[0] != '#' || !color_hex_parse_rgb24({t, 7}, rgb))
        return p.fail(value.source_offset);
    return true;
}

// ---- Knobs ------------------------------------------------------------------------------------------------
enum KnobMember : u32 { k_id, k_type, k_label, k_min, k_max, k_default, k_choices, k_hint, k_percent };

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
    const u32 id = name_index(name, "id\0type\0label\0min\0max\0default\0choices\0hint\0percent\0");
    if (id == 9 || (p.knob_seen & 1u << id))
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
    case k_percent: return json_value_to_bool(value, &k.percent) ? true : p.fail(value.source_offset);
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
    const u32 t = name_index(type, "float\0int\0toggle\0color\0choice\0");
    if (t == 5)
        return p.fail(at);
    k.type = static_cast<McfxKnobType>(t);
    constexpr u32 base = 1u << k_id | 1u << k_type | 1u << k_label | 1u << k_default;
    const u32 need = t <= 1 ? base | 1u << k_min | 1u << k_max : t == 4 ? base | 1u << k_choices : base;
    if ((p.knob_seen & ~(1u << k_hint | 1u << k_percent)) != need || (k.percent && t != 0))
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
        return text_value(p, value, c.url, kMcfxCreditUrlBytes) && bytes_equal(c.url, "https://", 8) ? true : p.fail(value.source_offset);
    return bit == 2 ? text_value(p, value, c.author, kMcfxCreditNameBytes) : text_value(p, value, c.license, kMcfxLicenseBytes);
}
bool use_entry(void* context, u32 index, JsonValue value) noexcept {
    auto& p = *static_cast<Parse*>(context);
    const u32 i = value_index(value, "noise\0glyphs\0blue_noise\0pointer\0texture0\0texture1\0clock\0appearance\0"
                                     "texture2\0texture3\0");
    if (index >= 10 || i >= 10 || (p.out->uses & 1u << i))
        return p.fail(value.source_offset);
    p.out->uses |= 1u << i; // EffectUse's bits are 1 << index in this order
    return true;
}
static_assert(effect_use_noise == 1 && effect_use_glyphs == 2 && effect_use_blue_noise == 4 && effect_use_pointer == 8 &&
              effect_use_texture0 == 16 && effect_use_texture1 == 32 && effect_use_clock == 64 &&
              effect_use_appearance == 128 && effect_use_texture2 == 256 && effect_use_texture3 == 512);

// ---- Pass graph (interface 3) -----------------------------------------------------------------------------
// "targets": [{"format": "rgba8|rgba16f|rgba32f", "size": [w, h] | "scale": percent, "persistent": bool}]
// "passes": [{"ps": i, "vs": i, "grid": [w, h] | "quads": n, "target": i, "inputs": [i, ...],
//             "blend": "opaque|alpha|add|premultiplied", "clear": bool}]; no "target": the output.
// Whole numbers up to 65535 into p.numbers: one, or an array of up to four.
bool number_entry(void* context, u32 index, JsonValue value) noexcept {
    auto& p = *static_cast<Parse*>(context);
    u64 v{};
    if (index >= 4 || !json_value_to_u64(value, &v) || v > 0xffff)
        return p.fail(value.source_offset);
    p.numbers[index] = static_cast<u16>(v);
    p.number_count = index + 1;
    return true;
}
bool numbers_value(Parse& p, JsonValue value) noexcept {
    p.number_count = 0;
    return value.kind == JsonValueKind::array ? p.read(json_visit_indexed_array(p.document, value, kDepth, number_entry, &p))
                                              : number_entry(&p, 0, value);
}

bool target_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<Parse*>(context);
    auto& t = p.out->graph.targets[p.out->graph.target_count];
    const u32 id = name_index(name, "format\0size\0scale\0persistent\0");
    if (id == 4 || (p.knob_seen & 1u << id))
        return p.fail(name.source_offset);
    p.knob_seen |= 1u << id;
    bool ok;
    if (id == 0) {
        const u32 format = value_index(value, "rgba8\0rgba16f\0rgba32f\0");
        t.format = static_cast<EffectTargetFormat>(format);
        ok = format < 3;
    } else if (id == 3) {
        ok = json_value_to_bool(value, &t.persistent);
    } else {
        // size: [1..512, 1..512]; scale: 1..100 percent of the output.
        const bool size = id == 1;
        ok = numbers_value(p, value) && (value.kind == JsonValueKind::array) == size && p.number_count == 1u + size &&
             p.numbers[0] && p.numbers[size] && p.numbers[0] <= (size ? kEffectGraphExtent : 100) &&
             p.numbers[size] <= kEffectGraphExtent;
        if (size) {
            t.width = p.numbers[0];
            t.height = p.numbers[1];
        } else {
            t.scale = static_cast<u8>(p.numbers[0]);
        }
    }
    return ok || (p.failed ? false : p.fail(value.source_offset));
}
bool target_entry(void* context, u32 index, JsonValue value) noexcept {
    auto& p = *static_cast<Parse*>(context);
    if (index >= kEffectGraphTargets || value.kind != JsonValueKind::object)
        return p.fail(value.source_offset);
    p.knob_seen = 0;
    if (!p.read(json_visit_indexed_object(p.document, value, kDepth, target_member, &p)))
        return false;
    // A format, and a size or a scale.
    const u32 extent = p.knob_seen & 6;
    if (!(p.knob_seen & 1) || (extent != 2 && extent != 4))
        return p.fail(value.source_offset);
    ++p.out->graph.target_count;
    return true;
}

enum PassMember : u32 { q_vs, q_ps, q_grid, q_quads, q_target, q_inputs, q_blend, q_clear };
bool pass_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<Parse*>(context);
    auto& q = p.out->graph.passes[p.out->graph.pass_count];
    const u32 id = name_index(name, "vs\0ps\0grid\0quads\0target\0inputs\0blend\0clear\0");
    if (id == 8 || (p.knob_seen & 1u << id))
        return p.fail(name.source_offset);
    p.knob_seen |= 1u << id;
    bool ok;
    if (id == q_blend) {
        const u32 blend = value_index(value, "opaque\0alpha\0add\0premultiplied\0");
        q.blend = static_cast<EffectPassBlend>(blend);
        ok = blend < 4;
    } else if (id == q_clear) {
        ok = json_value_to_bool(value, &q.clear);
    } else {
        if (!numbers_value(p, value))
            return false;
        const u32 a = p.numbers[0], b = p.numbers[1], n = p.number_count;
        ok = (value.kind == JsonValueKind::array) == (id == q_grid || id == q_inputs);
        switch (id) {
        case q_vs:
        case q_ps:
            ok = ok && a < kEffectGraphShaders;
            (id == q_vs ? q.vertex : q.pixel) = static_cast<u8>(a);
            break;
        case q_grid:
        case q_quads: {
            const bool grid = id == q_grid;
            ok = ok && !(p.knob_seen & 1u << (grid ? q_quads : q_grid)) && n == 1u + grid &&
                 (grid ? a >= 2 && b >= 2 && a <= kEffectGridExtent && b <= kEffectGridExtent
                       : a >= 1 && a <= kEffectQuadsMaximum);
            q.draw = grid ? EffectPassDraw::grid : EffectPassDraw::quads;
            q.count[0] = static_cast<u16>(a);
            q.count[1] = static_cast<u16>(grid ? b : 1);
            break;
        }
        case q_target:
            ok = ok && a < kEffectGraphTargets;
            q.target = static_cast<u8>(a);
            break;
        default:
            for (u32 i = 0; ok && i < n; ++i) {
                ok = p.numbers[i] < kEffectGraphTargets && !(q.inputs & 1u << p.numbers[i]);
                q.inputs = static_cast<u8>(q.inputs | 1u << p.numbers[i]);
            }
        }
    }
    return ok || (p.failed ? false : p.fail(value.source_offset));
}
bool pass_entry(void* context, u32 index, JsonValue value) noexcept {
    auto& p = *static_cast<Parse*>(context);
    if (index >= kEffectGraphPasses || value.kind != JsonValueKind::object)
        return p.fail(value.source_offset);
    p.knob_seen = 0;
    p.out->graph.passes[index].target = kEffectGraphOutput;
    if (!p.read(json_visit_indexed_object(p.document, value, kDepth, pass_member, &p)))
        return false;
    if (!(p.knob_seen & 1u << q_ps))
        return p.fail(value.source_offset);
    ++p.out->graph.pass_count;
    return true;
}

// What the members alone cannot check: shader indices and stages, targets and inputs, every shader used, the
// output written, and the memory caps (manifest), then each shader's stage (bytecode).
McfxError graph_check(McfxPackage& package) noexcept {
    auto& g = package.graph;
    u32 fixed = 0, scaled = 0, used = 0, vertex = 0;
    bool output = false;
    for (u32 i = 0; i < g.target_count; ++i) {
        const auto& t = g.targets[i];
        const u32 bytes = kEffectTargetTexelBytes[static_cast<u32>(t.format)] * (1u + t.persistent);
        fixed += t.scale ? 0 : bytes * t.width * t.height;
        scaled += bytes * t.scale * t.scale;
    }
    // scaled is in ten-thousandths of a byte per output pixel.
    if (fixed > kEffectGraphFixedBytes || scaled > kEffectGraphScaledBytes * 10000)
        return McfxError::manifest;
    for (u32 i = 0; i < g.pass_count; ++i) {
        const auto& q = g.passes[i];
        // A vertex shader draws geometry and geometry needs one; shader 0 is a pixel shader, so vertex 0 is none.
        if ((q.vertex != 0) != (q.draw != EffectPassDraw::screen) || q.vertex >= g.shader_count ||
            q.pixel >= g.shader_count || q.inputs >> g.target_count ||
            (q.target != kEffectGraphOutput &&
             (q.target >= g.target_count || ((q.inputs >> q.target & 1) && !g.targets[q.target].persistent))))
            return McfxError::manifest;
        output = output || q.target == kEffectGraphOutput;
        used |= 1u << q.pixel | 1u << q.vertex;
        vertex |= q.vertex ? 1u << q.vertex : 0u;
    }
    if (!output || used != (1u << g.shader_count) - 1)
        return McfxError::manifest;
    g.vertex_shaders = static_cast<u16>(vertex);
    for (u32 i = 0; i < g.shader_count; ++i)
        if (bytecode_version(package.shaders[i]) != (vertex >> i & 1 ? kVertex50 : kPixel50))
            return McfxError::bytecode;
    return McfxError::none;
}

enum RootMember : u32 {
    r_interface, r_slug, r_name, r_version, r_authors, r_license, r_derived, r_blend, r_uses, r_resolution, r_fps,
    r_knobs, r_targets, r_passes
};
constexpr u32 kRootRequired = 1u << r_interface | 1u << r_slug | 1u << r_name | 1u << r_version | 1u << r_authors |
                              1u << r_license | 1u << r_blend | 1u << r_uses | 1u << r_knobs;

bool root_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<Parse*>(context);
    auto& out = *p.out;
    const u32 id = name_index(name, "interface\0slug\0name\0version\0authors\0license\0derived_from\0blend\0uses\0resolution\0"
                                    "fps\0knobs\0targets\0passes\0");
    if (id == 14 || (p.seen & 1u << id))
        return p.fail(name.source_offset);
    p.seen |= 1u << id;
    switch (id) {
    case r_interface: {
        // 1; 2 for mc_ambient (clock, appearance); 3 for the pass graph and its textures. An older client refuses
        // a number it does not know here.
        u64 v{};
        if (!json_value_to_u64(value, &v) || v < 1 || v > kMcfxInterfaceMaximum)
            return p.fail(value.source_offset);
        p.interface_version = static_cast<u32>(v);
        return true;
    }
    case r_targets:
    case r_passes:
        return value.kind == JsonValueKind::array &&
                       p.read(json_visit_indexed_array(p.document, value, kDepth, id == r_targets ? target_entry : pass_entry, &p))
                   ? true
                   : p.failed ? false : p.fail(value.source_offset);
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
        const u32 i = value_index(value, "normal\0screen\0add\0");
        if (i >= 3)
            return p.fail(value.source_offset);
        out.blend = static_cast<EffectBlend>(i);
        return true;
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
    // The ambient inputs are interface 2 names: a package that declares them says so, so older clients refuse it.
    if ((w->package.uses & kEffectAmbientUses) && p.interface_version < 2)
        return {McfxError::manifest, 0};
    w->package.interface_version = static_cast<u8>(p.interface_version);
    w->package.rows = static_cast<u8>(p.row + (p.lane != 0));
    return {};
}

// ---- PNG --------------------------------------------------------------------------------------------------
bool png_header(const ConstSlice<u8>& png, u32* width, u32* height, u32* depth, u32* colour) noexcept {
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

bool mcfx_float_texture(ConstSlice<u8> texture, u32* width, u32* height) noexcept {
    if (texture.count < 8 || !tag(texture.data, "MCFT"))
        return false;
    *width = texture.data[4] | texture.data[5] << 8;
    *height = texture.data[6] | texture.data[7] << 8;
    return true;
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
    // Shader 0 is the DXBC section's pixel shader; SHADERS holds a graph's others, whole containers back to back.
    auto& package = w->package;
    const ConstSlice<u8> more = part[8];
    package.shaders[0] = part[1];
    u32 shaders = 1;
    for (u32 offset = 0; offset < more.count; offset += package.shaders[shaders++].count) {
        const u32 bytes = more.count - offset >= 32 ? le32(more.data + offset + 24) : 0;
        if (shaders == kEffectGraphShaders || bytes < 32 || bytes > more.count - offset || bytes > kMcfxBytecodeMaximumBytes)
            return {McfxError::bytecode, offset};
        package.shaders[shaders] = {more.data + offset, bytes};
    }
    package.graph.shader_count = static_cast<u8>(shaders);
    if (bytecode_version(part[1]) != kPixel50)
        return {McfxError::bytecode, 0};
    for (u32 i = 1; i < shaders; ++i)
        if (!bytecode_version(package.shaders[i]))
            return {McfxError::bytecode, i};
    bool data = false;
    for (u32 i = 0; i < kEffectTextureCount; ++i) {
        const ConstSlice<u8> texture = part[kTexturePart[i]];
        if (!texture.data)
            continue;
        u32 width{}, height{}, depth = 8, colour = 2;
        const bool floats = mcfx_float_texture(texture, &width, &height);
        data = data || floats;
        if ((!floats && (!png_header(texture, &width, &height, &depth, &colour) || texture.data[28] > 1)) || !width ||
            !height || width > kEffectTextureMaximumExtent || height > kEffectTextureMaximumExtent || depth != 8 ||
            (colour != 2 && colour != 6) || (floats && texture.count != 8 + width * height * 16))
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
    // The texture inputs and sections go together; the graph, TEXTURE2/3 and float data need interface 3.
    for (u32 i = 0; i < kEffectTextureCount; ++i)
        if (!!(package.uses & kTextureUse[i]) != !!part[kTexturePart[i]].data)
            return {McfxError::manifest, 0};
    const bool graph = package.graph.pass_count || package.graph.target_count || shaders > 1;
    if ((graph || data || (package.uses & kEffectGraphUses)) && package.interface_version < 3)
        return {McfxError::manifest, 0};
    if (graph) {
        const McfxError error = graph_check(package);
        if (error != McfxError::none)
            return {error, 0};
    }
    for (u32 i = 0; i < kEffectTextureCount; ++i)
        package.texture[i] = part[kTexturePart[i]];
    package.source = part[4];
    package.preview = part[5];
    package.file_bytes = file.count;
    return {};
}

bool mcfx_knob_value_valid(const McfxKnob& knob, const McfxKnobValue& value) noexcept {
    if (knob.type == McfxKnobType::color)
        return value.rgb <= 0xffffffu;
    return is_finite(value.number) && value.number >= knob.minimum && value.number <= knob.maximum &&
           (knob.type == McfxKnobType::number || whole(value.number));
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
