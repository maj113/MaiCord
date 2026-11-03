#include "maicord/app/plugins/catalog_index.hpp"
#include "maicord/app/plugins/http.hpp"
#include "maicord/app/update/fetch.hpp"
#include "maicord/core/ascii.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/name_list.hpp"
#include "maicord/sdk/maicord_plugin.h"

namespace maicord {
namespace {
using E = CatalogIndexError;
constexpr char kKinds[] = "plugin\0theme\0effect\0";
// MC_PERM_* in bit order.
constexpr char kPermissions[] = "message_send\0read_messages\0message_history\0read_activity\0network\0composer\0"
                                "act_messages\0act_presence\0clipboard\0automated_actions\0persistent_log\0";
constexpr u32 kPermissionCount = 11;
static_assert(name_list_count(kPermissions) == kPermissionCount && (1ull << kPermissionCount) - 1 == MC_PERM_KNOWN &&
              MC_PERM_NETWORK == 1ull << 4 && MC_PERM_PERSISTENT_LOG == 1ull << 10);
constexpr u32 kNetworkBit = 4;
constexpr char kSeverities[] = "malicious\0broken\0withdrawn\0";
constexpr char kBases[] = "dark\0light\0amoled\0";
constexpr char kCosts[] = "light\0medium\0heavy\0";
constexpr char kBlends[] = "normal\0screen\0add\0";
constexpr char kUses[] = "noise\0glyphs\0blue_noise\0pointer\0texture0\0texture1\0";
constexpr char kKnobTypes[] = "float\0int\0toggle\0color\0choice\0";
constexpr u64 kTimeMaximum = 1ull << 40;
constexpr i64 kKnobScalarMaximum = 1'000'000'000;
constexpr u32 kImageMaximumExtent = 4096;
constexpr u32 kCategoryMaximumBytes = 24;
constexpr u32 kNameMaximumBytes = 95, kSummaryMaximumBytes = 255, kAuthorMaximumBytes = 64, kGithubMaximumBytes = 39;
constexpr u32 kLicenseMaximumBytes = 32, kUrlMaximumBytes = 256, kChangelogMaximumBytes = 512, kAltMaximumBytes = 255;
constexpr u32 kReasonMaximumBytes = 128, kKnobIdMaximumBytes = 24, kKnobLabelMaximumBytes = 48, kChoiceMaximumBytes = 24;

struct Parse {
    CatalogIndex* out;
    JsonIndexedDocument document;
    E error;
};
bool fail(Parse& p, E error) noexcept {
    if (p.error == E::none) p.error = error;
    return false;
}
// The index of name among names, marking it in seen. An unknown name, or one
// allowed lacks, is unknown_field; a repeated one duplicate_field.
u32 member(Parse& p, JsonString name, const char* names, u32 allowed, u32* seen) noexcept {
    u32 i = 0;
    for (NameList list{names}; list; list.next(), ++i) {
        if (!json_string_equals_ascii(name, list.name())) continue;
        if (!(allowed & (1u << i))) break;
        if (*seen & (1u << i)) { fail(p, E::duplicate_field); return ~0u; }
        *seen |= 1u << i;
        return i;
    }
    fail(p, E::unknown_field);
    return ~0u;
}
bool object(Parse& p, JsonValue value, JsonMemberVisitor visitor, void* context) noexcept {
    if (value.kind != JsonValueKind::object) return fail(p, E::type);
    return json_visit_indexed_object(p.document, value, kCatalogIndexMaximumDepth, visitor, context).ok() || fail(p, E::json);
}
// Visits an array of at most maximum entries (more: limit) and at least minimum.
bool array(Parse& p, JsonValue value, JsonArrayVisitor visitor, void* context, u32 minimum) noexcept {
    if (value.kind != JsonValueKind::array) return fail(p, E::type);
    const auto read = json_visit_indexed_array(p.document, value, kCatalogIndexMaximumDepth, visitor, context);
    if (!read.ok()) return fail(p, E::json);
    return read.member_count >= minimum || fail(p, E::limit);
}
bool required(Parse& p, u32 seen, u32 mask) noexcept { return (seen & mask) == mask || fail(p, E::missing_field); }

// ---- numbers -------------------------------------------------------------------------------
// A JSON integer: no fraction or exponent and at most 20 digits; anything else is a type error.
struct Integer {
    bool negative, overflow;
    u64 magnitude;
};
bool integer(Parse& p, JsonValue value, Integer* out) noexcept {
    if (value.kind != JsonValueKind::number) return fail(p, E::type);
    const Utf8View t = value.source;
    u32 at = t.bytes && t.data[0] == '-' ? 1u : 0u;
    if (t.bytes - at > 20 || at == t.bytes) return fail(p, E::type);
    *out = {at == 1, false, 0};
    for (; at < t.bytes; ++at) {
        const char c = t.data[at];
        if (c < '0' || c > '9') return fail(p, E::type);
        const u64 digit = static_cast<u64>(c - '0');
        if (out->magnitude > (~0ull - digit) / 10) out->overflow = true;
        out->magnitude = out->magnitude * 10 + digit;
    }
    return true;
}
bool unsigned_in(Parse& p, JsonValue value, u64 low, u64 high, E code, u64* out) noexcept {
    Integer n{};
    if (!integer(p, value, &n)) return false;
    if (n.overflow || (n.negative && n.magnitude) || n.magnitude < low || n.magnitude > high) return fail(p, code);
    *out = n.magnitude;
    return true;
}
bool u32_in(Parse& p, JsonValue value, u32 low, u32 high, E code, u32* out) noexcept {
    u64 n = 0;
    if (!unsigned_in(p, value, low, high, code, &n)) return false;
    *out = static_cast<u32>(n);
    return true;
}
bool signed_in(Parse& p, JsonValue value, i64 low, i64 high, E code, i64* out) noexcept {
    Integer n{};
    if (!integer(p, value, &n)) return false;
    if (n.overflow || n.magnitude > static_cast<u64>(kKnobScalarMaximum) * 10) return fail(p, code);
    const i64 v = n.negative ? -static_cast<i64>(n.magnitude) : static_cast<i64>(n.magnitude);
    if (v < low || v > high) return fail(p, code);
    *out = v;
    return true;
}
// An integer or -?(0|[1-9][0-9]*)(.[0-9]{1,3})? within +-1e9, in thousandths.
bool decimal(Parse& p, JsonValue value, i64* out) noexcept {
    if (value.kind != JsonValueKind::number) return fail(p, E::type);
    const Utf8View t = value.source;
    u32 at = t.bytes && t.data[0] == '-' ? 1u : 0u;
    const u32 digits_start = at;
    u64 whole = 0;
    bool huge = false;
    while (at < t.bytes && t.data[at] >= '0' && t.data[at] <= '9') {
        huge = huge || whole > 1'000'000'000'000ull;
        whole = whole * 10 + static_cast<u64>(t.data[at++] - '0');
    }
    const u32 digits = at - digits_start;
    if (!digits || (digits > 1 && t.data[digits_start] == '0')) return fail(p, E::type);
    u64 fraction = 0;
    if (at < t.bytes) {
        if (t.data[at++] != '.') return fail(p, E::type); // an exponent
        u32 places = 0;
        while (at < t.bytes && t.data[at] >= '0' && t.data[at] <= '9' && places < 4) {
            fraction = fraction * 10 + static_cast<u64>(t.data[at++] - '0');
            ++places;
        }
        if (!places || places > 3 || at != t.bytes) return fail(p, E::type);
        for (; places < 3; ++places) fraction *= 10;
    } else if (digits > 20) {
        return fail(p, E::type);
    }
    if (huge || whole > static_cast<u64>(kKnobScalarMaximum) || (whole == static_cast<u64>(kKnobScalarMaximum) && fraction))
        return fail(p, E::limit);
    const i64 v = static_cast<i64>(whole) * kCatalogKnobScale + static_cast<i64>(fraction);
    *out = digits_start ? -v : v;
    return true;
}

// ---- text ----------------------------------------------------------------------------------
// One string decoded at the end of the text pool; keep() makes it part of the index.
struct Text {
    CatalogString ref;
    Utf8View view;
};
bool decode(Parse& p, JsonValue value, Text* text) noexcept {
    JsonString string{};
    if (!json_value_as_string(value, &string)) return fail(p, E::type);
    CatalogIndex& out = *p.out;
    const auto decoded = json_string_decode_utf8(string, {out.text + out.text_bytes, kCatalogIndexMaximumBytes - out.text_bytes});
    if (!decoded.ok()) return fail(p, E::json);
    text->ref = {out.text_bytes, decoded.bytes};
    text->view = {out.text + out.text_bytes, decoded.bytes};
    return true;
}
// target null: the text was only checked, not kept.
bool keep(Parse& p, const Text& text, CatalogString* target) noexcept {
    if (!target) return true;
    *target = text.ref;
    p.out->text_bytes += text.ref.bytes;
    return true;
}
bool banned(u32 c) noexcept {
    return c < 32 || (c >= 127 && c <= 159) || c == 0x061C || (c >= 0x200B && c <= 0x200F) ||
        (c >= 0x202A && c <= 0x202E) || (c >= 0x2060 && c <= 0x2064) || (c >= 0x2066 && c <= 0x2069) || c == 0xFEFF;
}
bool display_text(Utf8View text, u32 maximum, bool required) noexcept {
    if ((!text.data && text.bytes) || text.bytes > maximum || (required && !text.bytes)) return false;
    for (u32 at = 0; at < text.bytes;) {
        const u8 c = static_cast<u8>(text.data[at]);
        if (c >= 32 && c < 127) { ++at; continue; }
        const auto scalar = utf8_decode_one(text.data + at, text.bytes - at);
        if (!scalar.valid || banned(scalar.codepoint)) return false;
        at += scalar.width;
    }
    return true;
}
// A display text member kept in the index.
bool display(Parse& p, JsonValue value, u32 maximum, bool needed, CatalogString* target) noexcept {
    Text t{};
    if (!decode(p, value, &t)) return false;
    return display_text(t.view, maximum, needed) ? keep(p, t, target) : fail(p, E::text);
}
bool lower_alnum(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }
bool slug_valid(Utf8View t, u32 maximum) noexcept {
    if (!t.bytes || t.bytes > maximum || !lower_alnum(t.data[0])) return false;
    for (u32 i = 1; i < t.bytes; ++i) if (!lower_alnum(t.data[i]) && t.data[i] != '-') return false;
    return true;
}
bool license_valid(Utf8View t) noexcept {
    if (!t.bytes || t.bytes > kLicenseMaximumBytes) return false;
    for (u32 i = 0; i < t.bytes; ++i)
        if (!ascii_is_alnum(t.data[i]) && t.data[i] != '.' && t.data[i] != '+' && t.data[i] != '-') return false;
    return true;
}
bool github_valid(Utf8View t) noexcept {
    if (!t.bytes || t.bytes > kGithubMaximumBytes || t.data[0] == '-' || t.data[t.bytes - 1] == '-') return false;
    for (u32 i = 0; i < t.bytes; ++i) if (!ascii_is_alnum(t.data[i]) && t.data[i] != '-') return false;
    return true;
}
bool knob_id_valid(Utf8View t) noexcept {
    if (!t.bytes || t.bytes > kKnobIdMaximumBytes || t.data[0] < 'a' || t.data[0] > 'z') return false;
    for (u32 i = 1; i < t.bytes; ++i) if (!lower_alnum(t.data[i]) && t.data[i] != '_') return false;
    return true;
}
bool url_valid(Utf8View t) noexcept {
    constexpr Utf8View scheme = utf8_literal("https://");
    if (t.bytes <= scheme.bytes || t.bytes > kUrlMaximumBytes || !bytes_equal(t.data, scheme.data, scheme.bytes)) return false;
    u32 end = scheme.bytes;
    while (end < t.bytes && t.data[end] != '/' && t.data[end] != '?' && t.data[end] != '#') ++end;
    if (!update_origin_host_valid({t.data + scheme.bytes, end - scheme.bytes})) return false;
    for (u32 i = end; i < t.bytes; ++i) {
        const char c = t.data[i];
        bool allowed = ascii_is_alnum(c);
        for (const char* a = "-._~!$&'()*+,;=:@/?%#"; *a && !allowed; ++a) allowed = c == *a;
        if (!allowed) return false;
    }
    return true;
}
i32 hex_digit(char c, bool upper) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return upper && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
bool hex_bytes(Utf8View t, u8* output, u32 bytes) noexcept {
    if (t.bytes != bytes * 2) return false;
    for (u32 i = 0; i < t.bytes; ++i) {
        const i32 nibble = hex_digit(t.data[i], false);
        if (nibble < 0) return false;
        output[i / 2] = static_cast<u8>(i % 2 ? (output[i / 2] << 4) | nibble : nibble);
    }
    return true;
}
bool color(Utf8View t, u32* rgb) noexcept {
    if (t.bytes != 7 || t.data[0] != '#') return false;
    u32 value = 0;
    for (u32 i = 1; i < 7; ++i) {
        const i32 nibble = hex_digit(t.data[i], true);
        if (nibble < 0) return false;
        value = value << 4 | static_cast<u32>(nibble);
    }
    *rgb = value;
    return true;
}
// A string that must satisfy valid, kept in the index; code otherwise.
bool grammar(Parse& p, JsonValue value, bool (*valid)(Utf8View), E code, CatalogString* target) noexcept {
    Text t{};
    if (!decode(p, value, &t)) return false;
    return valid(t.view) ? keep(p, t, target) : fail(p, code);
}
bool category_member(Utf8View t) noexcept { return slug_valid(t, kCategoryMaximumBytes); }
// The position of a string among names, or code (choice) when it is not one.
bool lookup(Parse& p, JsonValue value, const char* names, u32* index) noexcept {
    Text t{};
    if (!decode(p, value, &t)) return false;
    u32 i = 0;
    for (NameList list{names}; list; list.next(), ++i)
        if (utf8_equal(t.view, list.name())) { *index = i; return true; }
    return fail(p, E::choice);
}
bool digest(Parse& p, JsonValue value, u8* output, u32 bytes) noexcept {
    Text t{};
    if (!decode(p, value, &t)) return false;
    return hex_bytes(t.view, output, bytes) || fail(p, E::digest);
}
bool version_text(Parse& p, JsonValue value, CatalogVersion* v) noexcept {
    Text t{};
    if (!decode(p, value, &t)) return false;
    if (t.view.bytes > kCatalogVersionTextMaximumBytes || !catalog_version_code(t.view, &v->code)) return fail(p, E::version);
    return keep(p, t, &v->text);
}
bool time(Parse& p, JsonValue value, u64* out) noexcept { return unsigned_in(p, value, 1, kTimeMaximum, E::time, out); }

// ---- {sha256, bytes} -------------------------------------------------------------------------
struct BlobContext {
    Parse* p;
    CatalogBlob* blob;
    u32 maximum, seen;
};
bool blob_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<BlobContext*>(context);
    const u32 field = member(*c.p, name, "sha256\0bytes\0", 3, &c.seen);
    if (field == ~0u) return false;
    if (field == 0) return digest(*c.p, value, c.blob->sha256, 32);
    return u32_in(*c.p, value, 1, c.maximum, E::file, &c.blob->bytes);
}
bool blob(Parse& p, JsonValue value, u32 maximum, CatalogBlob* out) noexcept {
    BlobContext c{&p, out, maximum, 0};
    return object(p, value, blob_member, &c) && required(p, c.seen, 3);
}

// ---- previews and the loop -------------------------------------------------------------------
enum ImageField : u32 { im_sha256, im_bytes, im_width, im_height, im_alt, im_fps, im_seconds };
constexpr char kImageNames[] = "sha256\0bytes\0width\0height\0alt\0fps\0seconds\0";
constexpr u32 kPreviewFields = 0x1F, kLoopFields = 0x6F;
struct ImageContext {
    Parse* p;
    CatalogImage* image;
    u32 allowed, seen;
};
bool image_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<ImageContext*>(context);
    Parse& p = *c.p;
    CatalogImage& im = *c.image;
    u32 range_only = 0; // fps and seconds are validated but nothing reads them
    switch (member(p, name, kImageNames, c.allowed, &c.seen)) {
    case im_sha256: return digest(p, value, im.blob.sha256, 32);
    case im_bytes:
        return u32_in(p, value, 1, c.allowed == kLoopFields ? kCatalogIndexLoopMaximumBytes : kCatalogIndexPreviewMaximumBytes,
                      E::file, &im.blob.bytes);
    case im_width: return u32_in(p, value, 1, kImageMaximumExtent, E::limit, &im.width);
    case im_height: return u32_in(p, value, 1, kImageMaximumExtent, E::limit, &im.height);
    case im_alt: return display(p, value, kAltMaximumBytes, true, nullptr);
    case im_fps: return u32_in(p, value, 1, 60, E::limit, &range_only);
    case im_seconds: return u32_in(p, value, 1, 10, E::limit, &range_only);
    default: return false;
    }
}
bool image(Parse& p, JsonValue value, u32 fields, CatalogImage* out) noexcept {
    ImageContext c{&p, out, fields, 0};
    return object(p, value, image_member, &c) && required(p, c.seen, fields);
}
bool preview_entry(void* context, u32 index, JsonValue value) noexcept {
    auto& item = *static_cast<CatalogItem*>(static_cast<void**>(context)[1]);
    Parse& p = *static_cast<Parse*>(static_cast<void**>(context)[0]);
    if (index >= kCatalogIndexMaximumPreviews) return fail(p, E::limit);
    if (!image(p, value, kPreviewFields, &item.previews[index])) return false;
    item.preview_count = index + 1;
    return true;
}

// ---- authors and derived_from ----------------------------------------------------------------
struct AuthorContext {
    Parse* p;
    CatalogAuthor* author;
    u32 seen;
};
bool author_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<AuthorContext*>(context);
    const u32 field = member(*c.p, name, "name\0github\0", 3, &c.seen);
    if (field == ~0u) return false;
    if (field == 0) return display(*c.p, value, kAuthorMaximumBytes, true, &c.author->name);
    return grammar(*c.p, value, github_valid, E::identifier, &c.author->github);
}
bool author_entry(void* context, u32 index, JsonValue value) noexcept {
    Parse& p = *static_cast<Parse*>(static_cast<void**>(context)[0]);
    auto& item = *static_cast<CatalogItem*>(static_cast<void**>(context)[1]);
    if (index >= kCatalogIndexMaximumAuthors) return fail(p, E::limit);
    AuthorContext a{&p, &item.authors[index], 0};
    if (!object(p, value, author_member, &a) || !required(p, a.seen, 1)) return false;
    item.author_count = index + 1;
    return true;
}
struct DerivedContext {
    Parse* p;
    u32 seen;
};
// derived_from is validated; no reader needs it kept.
bool derived_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<DerivedContext*>(context);
    switch (member(*c.p, name, "url\0author\0license\0", 7, &c.seen)) {
    case 0: return grammar(*c.p, value, url_valid, E::url, nullptr);
    case 1: return display(*c.p, value, kAuthorMaximumBytes, true, nullptr);
    case 2: return grammar(*c.p, value, license_valid, E::identifier, nullptr);
    default: return false;
    }
}
bool category_entry(void* context, u32 index, JsonValue value) noexcept {
    Parse& p = *static_cast<Parse*>(static_cast<void**>(context)[0]);
    auto& item = *static_cast<CatalogItem*>(static_cast<void**>(context)[1]);
    if (index >= kCatalogIndexMaximumCategories) return fail(p, E::limit);
    CatalogString& category = item.categories[index];
    if (!grammar(p, value, category_member, E::identifier, &category)) return false;
    for (u32 i = 0; i < index; ++i)
        if (utf8_equal(catalog_text(*p.out, item.categories[i]), catalog_text(*p.out, category))) return fail(p, E::duplicate);
    item.category_count = index + 1;
    return true;
}

// ---- plugin versions -----------------------------------------------------------------------------
struct PermissionContext {
    Parse* p;
    u64* mask;
    u32 next; // the lowest bit still allowed (bit order)
};
bool permission_entry(void* context, u32 index, JsonValue value) noexcept {
    auto& c = *static_cast<PermissionContext*>(context);
    Parse& p = *c.p;
    if (index >= 32) return fail(p, E::limit);
    Text t{};
    if (!decode(p, value, &t)) return false;
    u32 bit = 0;
    for (NameList list{kPermissions}; list && !utf8_equal(t.view, list.name()); list.next()) ++bit;
    if (bit == kPermissionCount) return fail(p, E::permission);
    if (*c.mask & (1ull << bit)) return fail(p, E::duplicate);
    if (bit < c.next) return fail(p, E::order);
    *c.mask |= 1ull << bit;
    c.next = bit + 1;
    return true;
}
bool host_entry(void* context, u32 index, JsonValue value) noexcept {
    Parse& p = *static_cast<Parse*>(static_cast<void**>(context)[0]);
    auto& v = *static_cast<CatalogVersion*>(static_cast<void**>(context)[1]);
    CatalogIndex& out = *p.out;
    if (index >= kCatalogIndexMaximumHosts) return fail(p, E::limit);
    Text t{};
    if (!decode(p, value, &t)) return false;
    if (!plugin_network_pattern_valid(t.view)) return fail(p, E::host);
    for (u32 i = 0; i < index; ++i) if (utf8_equal(catalog_text(out, out.hosts[v.first_host + i]), t.view)) return fail(p, E::duplicate);
    keep(p, t, &out.hosts[out.host_count++]);
    v.host_count = index + 1;
    return true;
}
enum VersionField : u32 { ve_version, ve_released, ve_changelog, ve_commit, ve_api_level, ve_permissions, ve_network, ve_file };
constexpr char kVersionNames[] = "version\0released\0changelog\0commit\0api_level\0permissions\0network\0file\0";
constexpr u32 kVersionRequired = 0xF3; // all but changelog and commit
struct VersionContext {
    Parse* p;
    CatalogVersion* version;
    u32 seen;
};
bool version_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<VersionContext*>(context);
    Parse& p = *c.p;
    CatalogVersion& v = *c.version;
    void* pair[]{&p, &v};
    switch (member(p, name, kVersionNames, 0xFF, &c.seen)) {
    case ve_version: return version_text(p, value, &v);
    case ve_released: return time(p, value, &v.released);
    case ve_changelog: return display(p, value, kChangelogMaximumBytes, false, &v.changelog);
    case ve_commit: return digest(p, value, v.commit, 20) && (v.has_commit = true);
    case ve_api_level: return u32_in(p, value, 1, 0xFFFF, E::api_level, &v.api_level);
    case ve_permissions: {
        PermissionContext permissions{&p, &v.permissions, 0};
        return array(p, value, permission_entry, &permissions, 0);
    }
    case ve_network:
        v.first_host = p.out->host_count;
        return array(p, value, host_entry, pair, 0);
    case ve_file: return blob(p, value, kCatalogIndexPluginMaximumBytes, &v.file);
    default: return false;
    }
}
struct VersionsContext {
    Parse* p;
    CatalogItem* item;
};
bool version_entry(void* context, u32 index, JsonValue value) noexcept {
    auto& c = *static_cast<VersionsContext*>(context);
    Parse& p = *c.p;
    CatalogIndex& out = *p.out;
    if (index >= kCatalogIndexMaximumVersions) return fail(p, E::limit);
    CatalogVersion& v = out.versions[out.version_count];
    VersionContext vc{&p, &v, 0};
    if (!object(p, value, version_member, &vc) || !required(p, vc.seen, kVersionRequired)) return false;
    if (((v.permissions >> kNetworkBit) & 1) != (v.host_count != 0)) return fail(p, E::permission);
    if (index) {
        const CatalogVersion& newer = out.versions[out.version_count - 1];
        if (v.code >= newer.code || v.released > newer.released) return fail(p, E::order);
    }
    ++out.version_count;
    c.item->version_count = index + 1;
    return true;
}

// ---- effect knobs ----------------------------------------------------------------------------------
enum KnobField : u32 { kn_id, kn_type, kn_label, kn_min, kn_max, kn_default, kn_choices, kn_percent };
constexpr char kKnobNames[] = "id\0type\0label\0min\0max\0default\0choices\0percent\0";
constexpr u32 kKnobFields[]{0x3F, 0x3F, 0x27, 0x27, 0x67}; // by CatalogKnobType; required, and allowed
constexpr u32 kKnobPercent = 1u << kn_percent;           // also allowed on a float knob
struct KnobContext {
    Parse* p;
    CatalogKnob* knob;
    u32 seen, choice_count;
    CatalogString choices[kCatalogIndexMaximumChoices]; // kept to refuse repeats within the knob
};
bool choice_entry(void* context, u32 index, JsonValue value) noexcept {
    auto& c = *static_cast<KnobContext*>(context);
    Parse& p = *c.p;
    CatalogIndex& out = *p.out;
    if (index >= kCatalogIndexMaximumChoices) return fail(p, E::limit);
    if (out.choice_count == kCatalogIndexMaximumChoiceTexts) return fail(p, E::limit);
    Text t{};
    if (!decode(p, value, &t)) return false;
    if (!display_text(t.view, kChoiceMaximumBytes, true)) return fail(p, E::text);
    for (u32 i = 0; i < index; ++i)
        if (utf8_equal(catalog_text(out, c.choices[i]), t.view)) return fail(p, E::duplicate);
    keep(p, t, &c.choices[index]);
    ++out.choice_count;
    c.choice_count = index + 1;
    return true;
}
bool knob_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<KnobContext*>(context);
    Parse& p = *c.p;
    CatalogKnob& k = *c.knob;
    const auto type = k.type;
    const bool number = type == CatalogKnobType::float_value || type == CatalogKnobType::int_value;
    i64* bound = nullptr;
    switch (member(p, name, kKnobNames,
                   kKnobFields[static_cast<u32>(type)] | (type == CatalogKnobType::float_value ? kKnobPercent : 0u), &c.seen)) {
    case kn_id: return grammar(p, value, knob_id_valid, E::identifier, &k.id);
    case kn_type: return true; // read before the visit
    case kn_percent:
        if (value.kind != JsonValueKind::boolean || !json_value_to_bool(value, &k.percent)) return fail(p, E::type);
        return true;
    case kn_label: return display(p, value, kKnobLabelMaximumBytes, true, &k.label);
    case kn_min: bound = &k.minimum; break;
    case kn_max: bound = &k.maximum; break;
    case kn_default:
        if (number) { bound = &k.value; break; }
        if (type == CatalogKnobType::toggle) {
            bool on = false;
            if (value.kind != JsonValueKind::boolean || !json_value_to_bool(value, &on)) return fail(p, E::type);
            k.value = on;
            return true;
        }
        if (type == CatalogKnobType::choice) return signed_in(p, value, 0, kCatalogIndexMaximumChoices - 1, E::limit, &k.value);
        {
            Text t{};
            u32 rgb = 0;
            if (!decode(p, value, &t)) return false;
            if (!color(t.view, &rgb)) return fail(p, E::text);
            k.value = rgb;
            return true;
        }
    case kn_choices:
        return array(p, value, choice_entry, &c, 2);
    default: return false;
    }
    if (type == CatalogKnobType::float_value) {
        if (!decimal(p, value, bound)) return false;
    } else {
        if (!signed_in(p, value, -kKnobScalarMaximum, kKnobScalarMaximum, E::limit, bound)) return false;
        *bound *= kCatalogKnobScale;
    }
    return true;
}
// The position of the string member name holds among names; choice when it is something
// else, missing_field when absent. A repeated name is duplicate_field.
struct Peek {
    Parse* p;
    Utf8View name;
    const char* names;
    u32 index, seen;
};
bool peek_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& k = *static_cast<Peek*>(context);
    if (!json_string_equals_ascii(name, k.name)) return true;
    if (k.seen++) return fail(*k.p, E::duplicate_field);
    char text[16];
    JsonString string{};
    if (!json_value_as_string(value, &string)) return fail(*k.p, E::choice);
    const auto decoded = json_string_decode_utf8(string, {text, sizeof(text)});
    u32 i = 0;
    for (NameList list{k.names}; decoded.ok() && list; list.next(), ++i)
        if (utf8_equal({text, decoded.bytes}, list.name())) { k.index = i; return true; }
    return fail(*k.p, E::choice);
}
bool peek(Parse& p, JsonValue value, Utf8View name, const char* names, u32* index) noexcept {
    if (value.kind != JsonValueKind::object) return fail(p, E::type);
    Peek k{&p, name, names, 0, 0};
    if (!json_visit_indexed_object(p.document, value, kCatalogIndexMaximumDepth, peek_member, &k).ok()) return fail(p, E::json);
    if (!k.seen) return fail(p, E::missing_field);
    *index = k.index;
    return true;
}
struct KnobsContext {
    Parse* p;
    CatalogItem* item;
};
bool knob_entry(void* context, u32 index, JsonValue value) noexcept {
    auto& c = *static_cast<KnobsContext*>(context);
    Parse& p = *c.p;
    CatalogIndex& out = *p.out;
    if (index >= kCatalogIndexMaximumScalarKnobs + kCatalogIndexMaximumColorKnobs + kCatalogIndexMaximumChoiceKnobs ||
        out.knob_count == kCatalogIndexMaximumKnobs)
        return fail(p, E::limit);
    u32 type = 0;
    if (!peek(p, value, utf8_literal("type"), kKnobTypes, &type)) return false;
    CatalogKnob& k = out.knobs[out.knob_count];
    k.type = static_cast<CatalogKnobType>(type);
    KnobContext kc{&p, &k, 0};
    if (!object(p, value, knob_member, &kc) || !required(p, kc.seen, kKnobFields[type])) return false;
    const bool number = k.type == CatalogKnobType::float_value || k.type == CatalogKnobType::int_value;
    if (number && (k.minimum >= k.maximum || k.value < k.minimum || k.value > k.maximum)) return fail(p, E::limit);
    if (k.type == CatalogKnobType::choice && static_cast<u64>(k.value) >= kc.choice_count) return fail(p, E::limit);
    ++out.knob_count;
    c.item->knob_count = index + 1;
    return true;
}
bool knobs(Parse& p, JsonValue value, CatalogItem& item) noexcept {
    KnobsContext c{&p, &item};
    item.first_knob = p.out->knob_count;
    if (!array(p, value, knob_entry, &c, 0)) return false;
    const CatalogKnob* list = p.out->knobs + item.first_knob;
    u32 scalars = 0, colors = 0, choices = 0;
    for (u32 i = 0; i < item.knob_count; ++i) {
        for (u32 j = 0; j < i; ++j)
            if (utf8_equal(catalog_text(*p.out, list[i].id), catalog_text(*p.out, list[j].id))) return fail(p, E::duplicate);
        scalars += list[i].type <= CatalogKnobType::toggle;
        colors += list[i].type == CatalogKnobType::color;
        choices += list[i].type == CatalogKnobType::choice;
    }
    return (scalars <= kCatalogIndexMaximumScalarKnobs && colors <= kCatalogIndexMaximumColorKnobs &&
            choices <= kCatalogIndexMaximumChoiceKnobs) || fail(p, E::limit);
}
struct UseContext {
    Parse* p;
    u32 seen;
};
bool use_entry(void* context, u32 index, JsonValue value) noexcept {
    auto& c = *static_cast<UseContext*>(context);
    Parse& p = *c.p;
    if (index >= 6) return fail(p, E::limit);
    u32 use = 0;
    if (!lookup(p, value, kUses, &use)) return false;
    if (c.seen & (1u << use)) return fail(p, E::duplicate);
    c.seen |= 1u << use;
    return true;
}
struct SwatchContext {
    Parse* p;
    u32* swatches;
    u32 seen;
};
bool swatch_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<SwatchContext*>(context);
    const u32 field = member(*c.p, name, "background\0surface\0text\0accent\0", 0xF, &c.seen);
    if (field == ~0u) return false;
    Text t{};
    if (!decode(*c.p, value, &t)) return false;
    return color(t.view, &c.swatches[field]) || fail(*c.p, E::text);
}

// ---- items -------------------------------------------------------------------------------------
enum ItemField : u32 { it_kind, it_slug, it_name, it_summary, it_authors, it_license, it_source, it_homepage, it_categories,
    it_derived, it_details, it_previews, it_versions, it_version, it_released, it_changelog, it_file, it_base, it_effect,
    it_swatches, it_cost, it_blend, it_uses, it_knobs, it_loop };
constexpr char kItemNames[] = "kind\0slug\0name\0summary\0authors\0license\0source\0homepage\0categories\0derived_from\0"
                              "details\0previews\0versions\0version\0released\0changelog\0file\0base\0effect\0swatches\0cost\0"
                              "blend\0uses\0knobs\0loop\0";
constexpr u32 bit(ItemField f) noexcept { return 1u << f; }
constexpr u32 kCommonFields = (1u << (it_previews + 1)) - 1;
constexpr u32 kCommonRequired = kCommonFields & ~(bit(it_homepage) | bit(it_derived));
constexpr u32 kDataFields = bit(it_version) | bit(it_released) | bit(it_changelog) | bit(it_file);
constexpr u32 kDataRequired = kDataFields & ~bit(it_changelog);
constexpr u32 kItemFields[]{
    kCommonFields | bit(it_versions),
    kCommonFields | kDataFields | bit(it_base) | bit(it_effect) | bit(it_swatches),
    kCommonFields | kDataFields | bit(it_cost) | bit(it_blend) | bit(it_uses) | bit(it_knobs) | bit(it_loop)};
constexpr u32 kItemRequired[]{
    kCommonRequired | bit(it_versions),
    kCommonRequired | kDataRequired | bit(it_base),
    kCommonRequired | kDataRequired | bit(it_cost) | bit(it_blend) | bit(it_uses) | bit(it_knobs)};
constexpr u32 kFileMaximum[]{kCatalogIndexPluginMaximumBytes, kCatalogIndexThemeMaximumBytes, kCatalogIndexEffectMaximumBytes};
struct ItemContext {
    Parse* p;
    CatalogItem* item;
    u32 seen;
};
bool item_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<ItemContext*>(context);
    Parse& p = *c.p;
    CatalogItem& item = *c.item;
    const u32 kind = static_cast<u32>(item.kind);
    CatalogVersion& data = p.out->versions[item.first_version]; // themes and effects
    void* pair[]{&p, &item};
    u32 index = 0;
    switch (member(p, name, kItemNames, kItemFields[kind], &c.seen)) {
    case it_kind: return true; // read before the visit
    case it_slug: return grammar(p, value, catalog_slug_valid, E::identifier, &item.slug);
    case it_name: return display(p, value, kNameMaximumBytes, true, &item.name);
    case it_summary: return display(p, value, kSummaryMaximumBytes, true, &item.summary);
    case it_authors: return array(p, value, author_entry, pair, 1);
    case it_license: return grammar(p, value, license_valid, E::identifier, &item.license);
    case it_source: return grammar(p, value, url_valid, E::url, &item.source);
    case it_homepage: return grammar(p, value, url_valid, E::url, &item.homepage);
    case it_categories: return array(p, value, category_entry, pair, 0);
    case it_derived: {
        DerivedContext d{&p, 0};
        return object(p, value, derived_member, &d) && required(p, d.seen, 7);
    }
    case it_details: return blob(p, value, kCatalogIndexDetailsMaximumBytes, &item.details);
    case it_previews: return array(p, value, preview_entry, pair, item.kind == CatalogKind::plugin ? 0u : 1u);
    case it_versions: {
        VersionsContext versions{&p, &item};
        return array(p, value, version_entry, &versions, 1);
    }
    case it_version: return version_text(p, value, &data);
    case it_released: return time(p, value, &data.released);
    case it_changelog: return display(p, value, kChangelogMaximumBytes, false, &data.changelog);
    case it_file: return blob(p, value, kFileMaximum[kind], &data.file);
    case it_base:
        if (!lookup(p, value, kBases, &index)) return false;
        item.base = static_cast<CatalogThemeBase>(index);
        return true;
    case it_effect: return grammar(p, value, catalog_slug_valid, E::identifier, &item.effect);
    case it_swatches: {
        SwatchContext s{&p, item.swatches, 0};
        return object(p, value, swatch_member, &s) && required(p, s.seen, 0xF) && (item.has_swatches = true);
    }
    case it_cost:
        if (!lookup(p, value, kCosts, &index)) return false;
        item.cost = static_cast<CatalogCost>(index);
        return true;
    case it_blend:
        if (!lookup(p, value, kBlends, &index)) return false;
        item.blend = static_cast<CatalogBlend>(index);
        return true;
    case it_uses: {
        UseContext uses{&p, 0};
        return array(p, value, use_entry, &uses, 0);
    }
    case it_knobs: return knobs(p, value, item);
    case it_loop: return image(p, value, kLoopFields, &item.loop) && (item.has_loop = true);
    default: return false;
    }
}
bool item_entry(void* context, u32 index, JsonValue value) noexcept {
    Parse& p = *static_cast<Parse*>(context);
    CatalogIndex& out = *p.out;
    if (index >= kCatalogIndexMaximumItems) return fail(p, E::limit);
    u32 kind = 0;
    if (!peek(p, value, utf8_literal("kind"), kKinds, &kind)) return false;
    CatalogItem& item = out.items[index];
    item.kind = static_cast<CatalogKind>(kind);
    item.first_version = out.version_count;
    if (item.kind != CatalogKind::plugin) {
        item.version_count = 1;
        ++out.version_count;
    }
    ItemContext c{&p, &item, 0};
    if (!object(p, value, item_member, &c) || !required(p, c.seen, kItemRequired[kind])) return false;
    const Utf8View slug = catalog_text(out, item.slug);
    for (u32 i = 0; i < index; ++i)
        if (out.items[i].kind == item.kind && utf8_equal(catalog_text(out, out.items[i].slug), slug)) return fail(p, E::duplicate);
    out.item_count = index + 1;
    return true;
}

// ---- revoked and blocklist ---------------------------------------------------------------------
struct RevokedContext {
    Parse* p;
    CatalogRevocation* entry;
    u32 seen;
    bool blocklist;
};
bool revoked_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<RevokedContext*>(context);
    Parse& p = *c.p;
    CatalogRevocation& e = *c.entry;
    u32 severity = 0;
    u64 since = 0; // validated; nothing reads it
    switch (member(p, name, "sha256\0reason\0since\0severity\0", c.blocklist ? 7u : 0xFu, &c.seen)) {
    case 0: return digest(p, value, e.sha256, 32);
    case 1: return display(p, value, kReasonMaximumBytes, false, &e.reason);
    case 2: return time(p, value, &since);
    case 3:
        if (!lookup(p, value, kSeverities, &severity)) return false;
        e.severity = static_cast<CatalogRevocationSeverity>(severity + 1);
        return true;
    default: return false;
    }
}
template <bool Blocklist>
bool revoked_entry(void* context, u32 index, JsonValue value) noexcept {
    Parse& p = *static_cast<Parse*>(context);
    CatalogIndex& out = *p.out;
    CatalogRevocation* list = Blocklist ? out.blocked : out.revoked;
    u32& count = Blocklist ? out.blocked_count : out.revoked_count;
    if (index >= (Blocklist ? kCatalogIndexMaximumBlocked : kCatalogIndexMaximumRevoked)) return fail(p, E::limit);
    RevokedContext c{&p, &list[index], 0, Blocklist};
    if (!object(p, value, revoked_member, &c) || !required(p, c.seen, Blocklist ? 7u : 0xFu)) return false;
    if (Blocklist) list[index].severity = CatalogRevocationSeverity::malicious;
    for (u32 i = 0; i < index; ++i) if (bytes_equal(list[i].sha256, list[index].sha256, 32)) return fail(p, E::duplicate);
    count = index + 1;
    return true;
}

// ---- root ------------------------------------------------------------------------------------------
enum RootField : u32 { r_format, r_sequence, r_issued, r_expires, r_items, r_revoked, r_blocklist };
constexpr char kRootNames[] = "format\0sequence\0issued\0expires\0items\0revoked\0blocklist\0";
struct RootContext {
    Parse* p;
    u32 seen;
};
bool root_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<RootContext*>(context);
    Parse& p = *c.p;
    CatalogIndex& out = *p.out;
    u64 format = 0;
    switch (member(p, name, kRootNames, 0x7F, &c.seen)) {
    case r_format: return (json_value_to_u64(value, &format) && format == 2) || fail(p, E::format);
    case r_sequence: return unsigned_in(p, value, 1, ~0ull, E::sequence, &out.sequence);
    case r_issued: return time(p, value, &out.issued);
    case r_expires: return time(p, value, &out.expires);
    case r_items: return array(p, value, item_entry, &p, 0);
    case r_revoked: return array(p, value, revoked_entry<false>, &p, 0);
    case r_blocklist: return array(p, value, revoked_entry<true>, &p, 0);
    default: return false;
    }
}
CatalogIndexResult finish(CatalogIndex* output, E error) noexcept {
    if (error != E::none) clear_bytes(output, sizeof(*output));
    return {error};
}
} // namespace

CatalogIndexResult catalog_index_verify(Utf8View document, ConstSlice<u8> signature, ConstSlice<const P256Key*> keys,
                                        KdfSha256Workspace& workspace, u64 now, bool accept_expired, Slice<u32> scratch,
                                        CatalogIndex* output) noexcept {
    if (!output || !document.data || !workspace.storage.data || !scratch.data || !scratch.count ||
        scratch.count > ~u32{0} / sizeof(u32) || !now || !keys.count || !keys.data)
        return {E::invalid_input};
    const ConstSlice<u8> ranges[]{
        {reinterpret_cast<const u8*>(document.data), document.bytes},
        {reinterpret_cast<const u8*>(scratch.data), scratch.count * static_cast<u32>(sizeof(u32))},
        {reinterpret_cast<const u8*>(output), static_cast<u32>(sizeof(*output))},
        {workspace.storage.data, workspace.storage.capacity},
        signature};
    for (u32 i = 0; i < 5; ++i) {
        if (!byte_range_valid(ranges[i])) return {E::invalid_input};
        for (u32 j = 0; j < i; ++j) if (byte_ranges_overlap(ranges[i], ranges[j])) return {E::invalid_input};
    }
    for (u32 i = 0; i < keys.count; ++i)
        if (!keys[i] || !keys[i]->handle || keys[i]->kind != P256Kind::ecdsa) return {E::invalid_input};
    clear_bytes(output, sizeof(*output));
    if (!document.bytes || document.bytes > kCatalogIndexMaximumBytes) return {E::size};
    const ConstSlice<u8> parts[]{
        {reinterpret_cast<const u8*>(kCatalogIndexSignatureDomain.data), kCatalogIndexSignatureDomain.bytes},
        {reinterpret_cast<const u8*>(document.data), document.bytes}};
    u8 digest[32]{};
    if (!sha256(workspace, {parts, 2}, {digest, 32}).ok()) return {E::invalid_input};
    bool trusted = false;
    for (u32 i = 0; i < keys.count && !trusted && signature.count == 64; ++i)
        trusted = p256_verify_digest(*keys[i], {digest, 32}, signature).ok();
    if (!trusted) return {E::signature};
    // The reader skips a byte order mark; the format has none.
    if (document.bytes >= 3 && static_cast<u8>(document.data[0]) == 0xEF && static_cast<u8>(document.data[1]) == 0xBB &&
        static_cast<u8>(document.data[2]) == 0xBF) return {E::json};
    const auto scan = json_scan_structurals_avx1(document, scratch);
    if (!scan.ok()) return {E::json};
    Parse p{output, {document, {scratch.data, scan.count}}, E::none};
    RootContext root{&p, 0};
    const auto visited =
        json_visit_root_object(document, {scratch.data, scan.count}, kCatalogIndexMaximumDepth, root_member, &root);
    if (!visited.ok())
        return finish(output, p.error != E::none ? p.error : visited.error == JsonReadError::depth_limit ? E::depth : E::json);
    if ((root.seen & 0x1F) != 0x1F) return finish(output, E::missing_field);
    CatalogIndex& out = *output;
    if (out.expires <= out.issued || out.expires - out.issued > kCatalogIndexMaximumValiditySeconds ||
        out.issued > now + kCatalogIndexClockSkewSeconds)
        return finish(output, E::time);
    if (out.expires <= now) {
        if (!accept_expired) return finish(output, E::expired);
        out.expired = true;
    }
    copy_bytes(out.digest, digest, 32);
    return {E::none};
}

bool catalog_slug_valid(Utf8View text) noexcept { return slug_valid(text, kCatalogSlugMaximumBytes); }

bool catalog_version_code(Utf8View text, u64* code) noexcept {
    u64 parts[3]{};
    u32 at = 0;
    for (u32 part = 0; part < 3; ++part) {
        const u32 start = at;
        while (at < text.bytes && text.data[at] >= '0' && text.data[at] <= '9' && at - start < 4)
            parts[part] = parts[part] * 10 + static_cast<u64>(text.data[at++] - '0');
        const u32 digits = at - start;
        if (!digits || digits > 3 || (digits > 1 && text.data[start] == '0')) return false;
        if (part < 2 && (at == text.bytes || text.data[at++] != '.')) return false;
    }
    if (at != text.bytes) return false;
    *code = parts[0] * 1'000'000 + parts[1] * 1'000 + parts[2];
    return true;
}

u32 catalog_file_name(CatalogKind kind, const u8 (&sha256)[32], Slice<char> output) noexcept {
    static constexpr char alphabet[] = "0123456789abcdef";
    static constexpr char extensions[] = "dll\0json\0mcfx\0";
    const Utf8View extension = name_list_at(extensions, static_cast<u32>(kind));
    const u32 bytes = 6 + 64 + 1 + extension.bytes;
    if (!output.data || output.count < bytes || !extension.bytes) return 0;
    copy_bytes(output.data, "files/", 6);
    for (u32 i = 0; i < 32; ++i) {
        output.data[6 + i * 2] = alphabet[sha256[i] >> 4];
        output.data[7 + i * 2] = alphabet[sha256[i] & 15];
    }
    output.data[70] = '.';
    copy_bytes(output.data + 71, extension.data, extension.bytes);
    return bytes;
}

u32 catalog_index_find(const CatalogIndex& index, CatalogKind kind, const Utf8View& slug) noexcept {
    for (u32 i = 0; i < index.item_count; ++i)
        if (index.items[i].kind == kind && utf8_equal(catalog_text(index, index.items[i].slug), slug)) return i;
    return ~0u;
}

CatalogRevocationSeverity catalog_index_revocation(const CatalogIndex& index, const u8* sha256,
                                                   const CatalogRevocation** entry) noexcept {
    const CatalogRevocation* found = nullptr;
    for (u32 i = 0; i < index.blocked_count && !found; ++i)
        if (bytes_equal(index.blocked[i].sha256, sha256, 32)) found = &index.blocked[i];
    for (u32 i = 0; i < index.revoked_count && !found; ++i)
        if (bytes_equal(index.revoked[i].sha256, sha256, 32)) found = &index.revoked[i];
    if (entry) *entry = found;
    return found ? found->severity : CatalogRevocationSeverity::none;
}

Utf8View catalog_permission_name(u32 bit) noexcept { return name_list_at(kPermissions, bit); }
Utf8View catalog_kind_name(CatalogKind kind) noexcept { return name_list_at(kKinds, static_cast<u32>(kind)); }

} // namespace maicord
