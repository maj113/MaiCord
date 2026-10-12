#include "maicord/app/plugin_index.hpp"
#include "maicord/app/update_fetch.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/core/memory.hpp"

namespace maicord {
namespace {
using E = PluginIndexError;
constexpr Utf8View kCategories[kPluginIndexCategoryCount]{utf8_literal("chat"), utf8_literal("appearance"),
    utf8_literal("notifications"), utf8_literal("privacy"), utf8_literal("utility"), utf8_literal("voice"),
    utf8_literal("accessibility"), utf8_literal("moderation"), utf8_literal("developer"), utf8_literal("fun")};
constexpr Utf8View kDisclosures[kPluginIndexDisclosureCount]{utf8_literal("network"), utf8_literal("files"),
    utf8_literal("processes"), utf8_literal("input")};
constexpr Utf8View kArches[]{utf8_literal("x64"), utf8_literal("arm64")};
constexpr Utf8View kSeverities[]{utf8_literal("malicious"), utf8_literal("broken"), utf8_literal("withdrawn")};
constexpr Utf8View kOfficialId = utf8_literal("maicord");
// The longest text field plus one byte, so a decode that fills it is too long.
constexpr u32 kDecodeCapacity = kPluginIndexChangelogMaximumBytes + 1;
constexpr u32 kVersionTextCapacity = 24; // "999.999.999-preview.998" is 23 bytes

struct Parse {
    const PluginIndexPolicy* policy;
    PluginIndex* out;
    JsonIndexedDocument document;
    E error;
    char buffer[kDecodeCapacity];
};
bool fail(Parse& p, E error) noexcept {
    if (p.error == E::none) p.error = error;
    return false;
}
// The index of name among names, marking it seen; count (with the error set)
// when it is unknown or repeated.
u32 member(Parse& p, JsonString name, const Utf8View* names, u32 count, u32* seen) noexcept {
    for (u32 i = 0; i < count; ++i) {
        if (!json_string_equals_ascii(name, names[i])) continue;
        if (*seen & (1u << i)) { fail(p, E::duplicate_field); return count; }
        *seen |= 1u << i;
        return i;
    }
    fail(p, E::unknown_field);
    return count;
}
bool number(Parse& p, JsonValue value, u64* output) noexcept { return json_value_to_u64(value, output) || fail(p, E::type); }
struct Decoded {
    PluginIndexText ref;
    Utf8View text; // into Parse::buffer; empty when it did not fit
    bool fits;
};
bool decode(Parse& p, JsonValue value, Decoded* decoded) noexcept {
    JsonString string{};
    if (!json_value_as_string(value, &string)) return fail(p, E::type);
    decoded->ref = {static_cast<u32>(string.encoded.data - p.document.input.data), string.encoded.bytes};
    const auto result = json_string_decode_utf8(string, {p.buffer, kDecodeCapacity});
    decoded->fits = result.ok();
    decoded->text = decoded->fits ? Utf8View{p.buffer, result.bytes} : Utf8View{};
    return result.ok() || result.error == JsonStringDecodeError::output_too_small || fail(p, E::json);
}
bool object(Parse& p, JsonValue value, JsonMemberVisitor visitor, void* context) noexcept {
    if (value.kind != JsonValueKind::object) return fail(p, E::type);
    // The root visit already bounded the whole document's depth.
    return json_visit_indexed_object(p.document, value, kPluginIndexMaximumDepth, visitor, context).ok() || fail(p, E::json);
}
bool array(Parse& p, JsonValue value, JsonArrayVisitor visitor, void* context, u32* count) noexcept {
    if (value.kind != JsonValueKind::array) return fail(p, E::type);
    const auto read = json_visit_indexed_array(p.document, value, kPluginIndexMaximumDepth, visitor, context);
    if (!read.ok()) return fail(p, E::json);
    *count = read.member_count;
    return true;
}

// ---- value grammars ----------------------------------------------------------
bool lower_alnum(char c) noexcept { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); }
bool alnum(char c) noexcept { return lower_alnum(c) || (c >= 'A' && c <= 'Z'); }
bool banned(u32 c) noexcept {
    return c < 32 || (c >= 127 && c <= 159) || c == 0x061C || (c >= 0x200B && c <= 0x200F) ||
        (c >= 0x202A && c <= 0x202E) || (c >= 0x2060 && c <= 0x2064) || (c >= 0x2066 && c <= 0x2069) || c == 0xFEFF;
}
bool display_text(Utf8View text, u32 maximum, bool required) noexcept {
    if ((!text.data && text.bytes) || text.bytes > maximum || (required && !text.bytes)) return false;
    for (u32 at = 0; at < text.bytes;) {
        const auto scalar = utf8_decode_one(text.data + at, text.bytes - at);
        if (!scalar.valid || banned(scalar.codepoint)) return false;
        at += scalar.width;
    }
    return true;
}
bool display(const Decoded& d, u32 maximum, bool required) noexcept { return d.fits && display_text(d.text, maximum, required); }
bool slug_valid(const Decoded& d) noexcept {
    if (!d.fits || !d.text.bytes || d.text.bytes > kPluginIndexSlugMaximumBytes || !lower_alnum(d.text.data[0])) return false;
    for (u32 i = 1; i < d.text.bytes; ++i) if (!lower_alnum(d.text.data[i]) && d.text.data[i] != '-') return false;
    return true;
}
bool provider_id_valid(const Decoded& d) noexcept {
    if (!d.fits || !d.text.bytes || d.text.bytes > kPluginIndexProviderIdMaximumBytes) return false;
    for (u32 i = 0; i < d.text.bytes; ++i) if (!lower_alnum(d.text.data[i]) && d.text.data[i] != '-') return false;
    return true;
}
bool license_valid(const Decoded& d) noexcept {
    if (!d.fits || !d.text.bytes || d.text.bytes > kPluginIndexLicenseMaximumBytes) return false;
    for (u32 i = 0; i < d.text.bytes; ++i) {
        const char c = d.text.data[i];
        if (!alnum(c) && c != '.' && c != '+' && c != '-') return false;
    }
    return true;
}
bool github_valid(const Decoded& d) noexcept {
    const Utf8View t = d.text;
    if (!d.fits || !t.bytes || t.bytes > kPluginIndexGithubLoginMaximumBytes || t.data[0] == '-' || t.data[t.bytes - 1] == '-') return false;
    for (u32 i = 0; i < t.bytes; ++i) if (!alnum(t.data[i]) && t.data[i] != '-') return false;
    return true;
}
bool hex_digest(const Decoded& d, u8* output, u32 bytes) noexcept {
    if (!d.fits || d.text.bytes != bytes * 2) return false;
    for (u32 i = 0; i < d.text.bytes; ++i) {
        const char c = d.text.data[i];
        const i32 nibble = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
        if (nibble < 0) return false;
        output[i / 2] = static_cast<u8>(i % 2 ? (output[i / 2] << 4) | nibble : nibble);
    }
    return true;
}
bool url_valid(const Decoded& d) noexcept {
    const Utf8View t = d.text;
    constexpr Utf8View scheme = utf8_literal("https://");
    if (!d.fits || t.bytes <= scheme.bytes || t.bytes > kPluginIndexUrlMaximumBytes || !bytes_equal(t.data, scheme.data, scheme.bytes))
        return false;
    u32 end = scheme.bytes;
    while (end < t.bytes && t.data[end] != '/' && t.data[end] != '?' && t.data[end] != '#') ++end;
    if (!update_origin_host_valid({t.data + scheme.bytes, end - scheme.bytes})) return false;
    for (u32 i = end; i < t.bytes; ++i) {
        const char c = t.data[i];
        bool allowed = alnum(c);
        for (const char* a = "-._~!$&'()*+,;=:@/?%#"; *a && !allowed; ++a) allowed = c == *a;
        if (!allowed) return false;
    }
    return true;
}
bool dot_segment(const char* s, u32 n) noexcept { return (n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'); }
// Relative to the provider base: [A-Za-z0-9._-] segments joined by '/', none
// empty, "." or "..".
bool path_valid(const Decoded& d) noexcept {
    const Utf8View t = d.text;
    if (!d.fits || !t.bytes || t.bytes > kPluginIndexPathMaximumBytes) return false;
    u32 start = 0;
    for (u32 i = 0; i <= t.bytes; ++i) {
        if (i == t.bytes || t.data[i] == '/') {
            if (i == start || dot_segment(t.data + start, i - start)) return false;
            start = i + 1;
        } else if (!alnum(t.data[i]) && t.data[i] != '.' && t.data[i] != '_' && t.data[i] != '-') return false;
    }
    return true;
}
// A release tag or asset name: [A-Za-z0-9._+-], not "." or "..".
bool release_name_valid(const Decoded& d, u32 maximum) noexcept {
    const Utf8View t = d.text;
    if (!d.fits || !t.bytes || t.bytes > maximum || dot_segment(t.data, t.bytes)) return false;
    for (u32 i = 0; i < t.bytes; ++i) {
        const char c = t.data[i];
        if (!alnum(c) && c != '.' && c != '_' && c != '+' && c != '-') return false;
    }
    return true;
}
u32 lookup(const Utf8View* names, u32 count, const Decoded& d) noexcept {
    if (!d.fits) return count;
    for (u32 i = 0; i < count; ++i) if (utf8_equal(d.text, names[i])) return i;
    return count;
}
// MAJOR.MINOR.PATCH or MAJOR.MINOR.PATCH-preview.N exactly as
// update_version_text writes version_code (n 1..998 or 999 for stable).
bool version_valid(Utf8View text, u64 code) noexcept {
    if (!code || code % 1000 == 0 || code / 1000000000 >= 1000) return false;
    char expected[kVersionTextCapacity];
    const u32 bytes = update_version_text(code, {expected, sizeof(expected)});
    return bytes && utf8_equal(text, {expected, bytes});
}

// ---- string sets: categories, disclosures, permissions -------------------------
enum class Vocabulary : u8 { categories, disclosures, permissions };
struct SetContext {
    Parse* p;
    u64* mask;
    Vocabulary vocabulary;
    u32 maximum;
};
bool set_entry(void* context, u32 index, JsonValue value) noexcept {
    auto& c = *static_cast<SetContext*>(context);
    Parse& p = *c.p;
    if (index >= c.maximum) return fail(p, E::limit);
    Decoded d{};
    if (!decode(p, value, &d)) return false;
    u32 bit = 0;
    if (c.vocabulary == Vocabulary::permissions) {
        bit = d.fits ? native_plugin_permission_bit(d.text) : kNativePluginPermissionCount;
        if (bit == kNativePluginPermissionCount) return fail(p, E::permission);
    } else {
        const bool categories = c.vocabulary == Vocabulary::categories;
        bit = categories ? lookup(kCategories, kPluginIndexCategoryCount, d) : lookup(kDisclosures, kPluginIndexDisclosureCount, d);
        if (bit == (categories ? kPluginIndexCategoryCount : kPluginIndexDisclosureCount)) return fail(p, E::choice);
    }
    if (*c.mask & (1ull << bit)) return fail(p, E::duplicate);
    *c.mask |= 1ull << bit;
    return true;
}
bool string_set(Parse& p, JsonValue value, Vocabulary vocabulary, u32 maximum, u64* mask) noexcept {
    SetContext c{&p, mask, vocabulary, maximum};
    u32 count = 0;
    return array(p, value, set_entry, &c, &count);
}

// ---- provider ------------------------------------------------------------------
enum Provider : u32 { pr_id, pr_name, pr_homepage, pr_key, provider_count };
constexpr Utf8View kProviderNames[provider_count]{utf8_literal("id"), utf8_literal("name"), utf8_literal("homepage"),
    utf8_literal("key")};
constexpr u32 kProviderRequired = (1u << pr_key) - 1;
struct ObjectContext {
    Parse* p;
    u32 seen;
};
bool provider_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<ObjectContext*>(context);
    Parse& p = *c.p;
    PluginIndex& out = *p.out;
    const u32 field = member(p, name, kProviderNames, provider_count, &c.seen);
    if (field == provider_count) return false;
    Decoded d{};
    if (!decode(p, value, &d)) return false;
    switch (field) {
    case pr_id: {
        if (!provider_id_valid(d)) return fail(p, E::identifier);
        // Only the compiled official provider may call itself "maicord".
        if (utf8_equal(d.text, kOfficialId) != (p.policy->kind == PluginProviderKind::official)) return fail(p, E::provider);
        out.provider_id = d.ref;
        return true;
    }
    case pr_name:
        if (!display(d, kPluginIndexProviderNameMaximumBytes, true)) return fail(p, E::text);
        out.provider_name = d.ref;
        return true;
    case pr_homepage:
        if (!url_valid(d)) return fail(p, E::url);
        out.provider_homepage = d.ref;
        return true;
    default: { // key
        u8 key[65]{};
        if (p.policy->kind != PluginProviderKind::custom_signed || !d.fits || !update_public_key_from_hex(d.text, {key, 65}))
            return fail(p, E::provider);
        if (!bytes_equal(key, p.policy->pinned_key, 65)) return fail(p, E::key_changed);
        copy_bytes(out.key, key, 65);
        out.has_key = true;
        return true;
    }
    }
}

// ---- files ---------------------------------------------------------------------
enum Release : u32 { gr_repository, gr_tag, gr_asset, release_count };
constexpr Utf8View kReleaseNames[release_count]{utf8_literal("repository"), utf8_literal("tag"), utf8_literal("asset")};
struct ReleaseContext {
    Parse* p;
    PluginIndexFile* file;
    u32 seen;
};
bool release_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<ReleaseContext*>(context);
    Parse& p = *c.p;
    const u32 field = member(p, name, kReleaseNames, release_count, &c.seen);
    if (field == release_count) return false;
    Decoded d{};
    if (!decode(p, value, &d)) return false;
    const bool valid = field == gr_repository ? d.fits && update_repository_valid(d.text)
        : release_name_valid(d, field == gr_tag ? kPluginIndexTagMaximumBytes : kPluginIndexAssetMaximumBytes);
    if (!valid) return fail(p, E::location);
    (field == gr_repository ? c.file->repository : field == gr_tag ? c.file->tag : c.file->asset) = d.ref;
    return true;
}
enum FileField : u32 { f_arch, f_sha256, f_bytes, f_path, f_release, file_field_count };
constexpr Utf8View kFileNames[file_field_count]{utf8_literal("arch"), utf8_literal("sha256"), utf8_literal("bytes"),
    utf8_literal("path"), utf8_literal("github_release")};
constexpr u32 kFileRequired = (1u << f_path) - 1;
struct FileContext {
    Parse* p;
    PluginIndexFile* file;
    u32 seen;
};
bool file_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<FileContext*>(context);
    Parse& p = *c.p;
    PluginIndexFile& file = *c.file;
    const u32 field = member(p, name, kFileNames, file_field_count, &c.seen);
    if (field == file_field_count) return false;
    const bool official = p.policy->kind == PluginProviderKind::official;
    Decoded d{};
    switch (field) {
    case f_arch: {
        if (!decode(p, value, &d)) return false;
        const u32 arch = lookup(kArches, 2, d);
        if (arch == 2) return fail(p, E::choice);
        file.arch = static_cast<PluginIndexArch>(arch);
        return true;
    }
    case f_sha256:
        if (!decode(p, value, &d)) return false;
        return hex_digest(d, file.sha256, 32) || fail(p, E::digest);
    case f_bytes: {
        u64 bytes = 0;
        if (!number(p, value, &bytes)) return false;
        if (!bytes || bytes > kPluginIndexFileMaximumBytes) return fail(p, E::file);
        file.bytes = static_cast<u32>(bytes);
        return true;
    }
    case f_path:
        // Official locations are derived from the repository, never read from the index.
        if (official) return fail(p, E::location);
        if (!decode(p, value, &d)) return false;
        if (!path_valid(d)) return fail(p, E::location);
        file.path = d.ref;
        file.source = PluginIndexFileSource::path;
        return true;
    default: { // github_release
        if (official) return fail(p, E::location);
        ReleaseContext r{&p, &file, 0};
        if (!object(p, value, release_member, &r)) return false;
        if (r.seen != (1u << release_count) - 1) return fail(p, E::missing_field);
        file.source = PluginIndexFileSource::github_release;
        return true;
    }
    }
}
struct FilesContext {
    Parse* p;
    PluginIndexVersion* version;
};
bool file_entry(void* context, u32 index, JsonValue value) noexcept {
    auto& c = *static_cast<FilesContext*>(context);
    Parse& p = *c.p;
    if (index >= kPluginIndexMaximumFiles) return fail(p, E::limit);
    PluginIndexFile& file = c.version->files[index];
    FileContext f{&p, &file, 0};
    if (!object(p, value, file_member, &f)) return false;
    if ((f.seen & kFileRequired) != kFileRequired) return fail(p, E::missing_field);
    const bool located = f.seen & ((1u << f_path) | (1u << f_release));
    const bool both = (f.seen & (1u << f_path)) && (f.seen & (1u << f_release));
    if (p.policy->kind != PluginProviderKind::official && (!located || both)) return fail(p, E::location);
    for (u32 i = 0; i < index; ++i) if (c.version->files[i].arch == file.arch) return fail(p, E::duplicate);
    c.version->file_count = index + 1;
    return true;
}

// ---- versions ------------------------------------------------------------------
enum VersionField : u32 { v_version, v_code, v_abi, v_min_host, v_max_host, v_permissions, v_files, v_released, v_commit,
    v_changelog, version_field_count };
constexpr Utf8View kVersionNames[version_field_count]{utf8_literal("version"), utf8_literal("version_code"),
    utf8_literal("abi"), utf8_literal("min_host_abi"), utf8_literal("max_host_abi"), utf8_literal("permissions"),
    utf8_literal("files"), utf8_literal("released"), utf8_literal("commit"), utf8_literal("changelog")};
constexpr u32 kVersionRequired = ((1u << version_field_count) - 1) & ~(1u << v_commit);
struct VersionContext {
    Parse* p;
    PluginIndexVersion* version;
    u32 seen;
    u32 version_bytes;
    char version_text[kVersionTextCapacity];
    u64 abi, min_host, max_host;
};
bool version_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<VersionContext*>(context);
    Parse& p = *c.p;
    PluginIndexVersion& v = *c.version;
    const u32 field = member(p, name, kVersionNames, version_field_count, &c.seen);
    if (field == version_field_count) return false;
    Decoded d{};
    switch (field) {
    case v_version:
        if (!decode(p, value, &d)) return false;
        if (!d.fits || d.text.bytes >= kVersionTextCapacity) return fail(p, E::version);
        copy_bytes(c.version_text, d.text.data, d.text.bytes);
        c.version_bytes = d.text.bytes;
        return true;
    case v_code: return number(p, value, &v.version_code);
    case v_abi: return number(p, value, &c.abi);
    case v_min_host: return number(p, value, &c.min_host);
    case v_max_host: return number(p, value, &c.max_host);
    case v_permissions: return string_set(p, value, Vocabulary::permissions, kNativePluginPermissionCount, &v.permissions);
    case v_files: {
        FilesContext f{&p, &v};
        u32 count = 0;
        if (!array(p, value, file_entry, &f, &count)) return false;
        return count || fail(p, E::limit);
    }
    case v_released:
        if (!number(p, value, &v.released)) return false;
        return v.released || fail(p, E::time);
    case v_commit:
        if (!decode(p, value, &d)) return false;
        if (!hex_digest(d, v.commit, 20)) return fail(p, E::digest);
        v.has_commit = true;
        return true;
    default: // changelog
        if (!decode(p, value, &d)) return false;
        if (!display(d, kPluginIndexChangelogMaximumBytes, false)) return fail(p, E::text);
        v.changelog = d.ref;
        return true;
    }
}
struct VersionsContext {
    Parse* p;
    PluginIndexPlugin* plugin;
    u64 previous_code;
};
bool version_entry(void* context, u32 index, JsonValue value) noexcept {
    auto& c = *static_cast<VersionsContext*>(context);
    Parse& p = *c.p;
    PluginIndex& out = *p.out;
    // Every listed slot fits: at most kPluginIndexMaximumVersions per plugin.
    if (index >= kPluginIndexMaximumVersions || out.version_count >= kPluginIndexMaximumListedVersions) return fail(p, E::limit);
    PluginIndexVersion& v = out.versions[out.version_count];
    VersionContext vc{&p, &v, 0, 0, {}, 0, 0, 0};
    if (!object(p, value, version_member, &vc)) return false;
    if ((vc.seen & kVersionRequired) != kVersionRequired) return fail(p, E::missing_field);
    if (!version_valid({vc.version_text, vc.version_bytes}, v.version_code)) return fail(p, E::version);
    if (!vc.abi || vc.abi > kPluginIndexMaximumHostAbi || vc.min_host < vc.abi || vc.min_host > kPluginIndexMaximumHostAbi ||
        (vc.max_host && (vc.max_host < vc.min_host || vc.max_host > kPluginIndexMaximumHostAbi))) return fail(p, E::abi);
    if (p.policy->kind == PluginProviderKind::official && !v.has_commit) return fail(p, E::provider);
    if (index && v.version_code >= c.previous_code) return fail(p, E::order);
    c.previous_code = v.version_code;
    v.abi = static_cast<u32>(vc.abi); v.min_host_abi = static_cast<u32>(vc.min_host); v.max_host_abi = static_cast<u32>(vc.max_host);
    if (update_version_channel(v.version_code) == UpdateChannel::preview) {
        // Reserved for later clients: validated, never offered.
        v = {};
        ++out.ignored_versions;
        return true;
    }
    ++out.version_count;
    ++c.plugin->version_count;
    return true;
}

// ---- plugins -------------------------------------------------------------------
enum PluginField : u32 { pl_slug, pl_owner, pl_name, pl_summary, pl_publisher, pl_authors, pl_license, pl_homepage,
    pl_categories, pl_disclosures, pl_details, pl_versions, plugin_field_count };
constexpr Utf8View kPluginNames[plugin_field_count]{utf8_literal("slug"), utf8_literal("owner_id"), utf8_literal("name"),
    utf8_literal("summary"), utf8_literal("publisher"), utf8_literal("authors"), utf8_literal("license"),
    utf8_literal("homepage"), utf8_literal("categories"), utf8_literal("disclosures"), utf8_literal("details"),
    utf8_literal("versions")};
constexpr u32 kPluginRequired = ((1u << plugin_field_count) - 1) & ~(1u << pl_details);
enum AuthorField : u32 { au_name, au_github, author_field_count };
constexpr Utf8View kAuthorNames[author_field_count]{utf8_literal("name"), utf8_literal("github")};
enum DetailsField : u32 { de_sha256, de_bytes, details_field_count };
constexpr Utf8View kDetailsNames[details_field_count]{utf8_literal("sha256"), utf8_literal("bytes")};
struct AuthorContext {
    Parse* p;
    PluginIndexAuthor* author;
    u32 seen;
};
bool author_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<AuthorContext*>(context);
    Parse& p = *c.p;
    const u32 field = member(p, name, kAuthorNames, author_field_count, &c.seen);
    if (field == author_field_count) return false;
    Decoded d{};
    if (!decode(p, value, &d)) return false;
    if (field == au_name) {
        if (!display(d, kPluginIndexAuthorMaximumBytes, true)) return fail(p, E::text);
        c.author->name = d.ref;
    } else {
        if (!github_valid(d)) return fail(p, E::identifier);
        c.author->github = d.ref;
    }
    return true;
}
bool author_entry(void* context, u32 index, JsonValue value) noexcept {
    auto& plugin_context = *static_cast<ObjectContext*>(context);
    Parse& p = *plugin_context.p;
    if (index >= kPluginIndexMaximumAuthors) return fail(p, E::limit);
    PluginIndexPlugin& plugin = p.out->plugins[p.out->plugin_count];
    AuthorContext a{&p, &plugin.authors[index], 0};
    if (!object(p, value, author_member, &a)) return false;
    if (!(a.seen & (1u << au_name))) return fail(p, E::missing_field);
    plugin.author_count = index + 1;
    return true;
}
struct DetailsContext {
    Parse* p;
    PluginIndexPlugin* plugin;
    u32 seen;
};
bool details_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<DetailsContext*>(context);
    Parse& p = *c.p;
    const u32 field = member(p, name, kDetailsNames, details_field_count, &c.seen);
    if (field == details_field_count) return false;
    if (field == de_sha256) {
        Decoded d{};
        if (!decode(p, value, &d)) return false;
        return hex_digest(d, c.plugin->details_sha256, 32) || fail(p, E::digest);
    }
    u64 bytes = 0;
    if (!number(p, value, &bytes)) return false;
    if (!bytes || bytes > kPluginIndexDetailsMaximumBytes) return fail(p, E::file);
    c.plugin->details_bytes = static_cast<u32>(bytes);
    return true;
}
bool plugin_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<ObjectContext*>(context);
    Parse& p = *c.p;
    PluginIndexPlugin& plugin = p.out->plugins[p.out->plugin_count];
    const u32 field = member(p, name, kPluginNames, plugin_field_count, &c.seen);
    if (field == plugin_field_count) return false;
    Decoded d{};
    u64 mask = 0;
    u32 count = 0;
    switch (field) {
    case pl_slug:
        if (!decode(p, value, &d)) return false;
        if (!slug_valid(d)) return fail(p, E::identifier);
        plugin.slug = d.ref;
        return true;
    case pl_owner:
        if (!number(p, value, &plugin.owner_id)) return false;
        return plugin.owner_id > 1 || fail(p, E::owner);
    case pl_name:
    case pl_summary:
    case pl_publisher: {
        if (!decode(p, value, &d)) return false;
        const u32 maximum = field == pl_name ? kPluginIndexNameMaximumBytes
            : field == pl_summary ? kPluginIndexSummaryMaximumBytes : kPluginIndexPublisherMaximumBytes;
        if (!display(d, maximum, field == pl_name)) return fail(p, E::text);
        (field == pl_name ? plugin.name : field == pl_summary ? plugin.summary : plugin.publisher) = d.ref;
        return true;
    }
    case pl_authors:
        if (!array(p, value, author_entry, &c, &count)) return false;
        return count || fail(p, E::limit);
    case pl_license:
        if (!decode(p, value, &d)) return false;
        if (!license_valid(d)) return fail(p, E::identifier);
        plugin.license = d.ref;
        return true;
    case pl_homepage:
        if (!decode(p, value, &d)) return false;
        if (!url_valid(d)) return fail(p, E::url);
        plugin.homepage = d.ref;
        return true;
    case pl_categories:
        if (!string_set(p, value, Vocabulary::categories, kPluginIndexMaximumCategories, &mask)) return false;
        plugin.categories = static_cast<u32>(mask);
        return true;
    case pl_disclosures:
        if (!string_set(p, value, Vocabulary::disclosures, kPluginIndexDisclosureCount, &mask)) return false;
        plugin.disclosures = static_cast<u32>(mask);
        return true;
    case pl_details: {
        DetailsContext details{&p, &plugin, 0};
        if (!object(p, value, details_member, &details)) return false;
        return details.seen == (1u << details_field_count) - 1 || fail(p, E::missing_field);
    }
    default: { // versions
        VersionsContext versions{&p, &plugin, 0};
        plugin.first_version = p.out->version_count;
        plugin.version_count = 0;
        if (!array(p, value, version_entry, &versions, &count)) return false;
        return count || fail(p, E::limit);
    }
    }
}
bool same_text(Utf8View document, PluginIndexText a, PluginIndexText b) noexcept {
    char left[kPluginIndexSlugMaximumBytes + 1], right[kPluginIndexSlugMaximumBytes + 1];
    const auto x = plugin_index_text_decode(document, a, {left, sizeof(left)});
    const auto y = plugin_index_text_decode(document, b, {right, sizeof(right)});
    return x.ok() && y.ok() && utf8_equal({left, x.bytes}, {right, y.bytes});
}
bool plugin_entry(void* context, u32 index, JsonValue value) noexcept {
    Parse& p = *static_cast<Parse*>(context);
    PluginIndex& out = *p.out;
    if (index >= kPluginIndexMaximumPlugins) return fail(p, E::limit);
    ObjectContext c{&p, 0};
    if (!object(p, value, plugin_member, &c)) return false;
    if ((c.seen & kPluginRequired) != kPluginRequired) return fail(p, E::missing_field);
    const PluginIndexPlugin& plugin = out.plugins[index];
    for (u32 i = 0; i < index; ++i)
        if (out.plugins[i].owner_id == plugin.owner_id || same_text(p.document.input, out.plugins[i].slug, plugin.slug))
            return fail(p, E::duplicate);
    out.plugin_count = index + 1;
    return true;
}

// ---- revocations and the blocklist ---------------------------------------------
enum RevokedField : u32 { rv_owner, rv_sha256, rv_severity, rv_reason, rv_since, revoked_field_count };
constexpr Utf8View kRevokedNames[revoked_field_count]{utf8_literal("owner_id"), utf8_literal("sha256"),
    utf8_literal("severity"), utf8_literal("reason"), utf8_literal("since")};
enum BlockField : u32 { bl_sha256, bl_reason, bl_since, block_field_count };
constexpr Utf8View kBlockNames[block_field_count]{utf8_literal("sha256"), utf8_literal("reason"), utf8_literal("since")};
struct RevokedContext {
    Parse* p;
    PluginIndexRevocation* entry;
    u32 seen;
};
bool revoked_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<RevokedContext*>(context);
    Parse& p = *c.p;
    PluginIndexRevocation& entry = *c.entry;
    const u32 field = member(p, name, kRevokedNames, revoked_field_count, &c.seen);
    if (field == revoked_field_count) return false;
    Decoded d{};
    switch (field) {
    case rv_owner:
        if (!number(p, value, &entry.owner_id)) return false;
        return entry.owner_id > 1 || fail(p, E::owner);
    case rv_sha256:
        if (!decode(p, value, &d)) return false;
        return hex_digest(d, entry.sha256, 32) || fail(p, E::digest);
    case rv_severity: {
        if (!decode(p, value, &d)) return false;
        const u32 severity = lookup(kSeverities, 3, d);
        if (severity == 3) return fail(p, E::choice);
        entry.severity = static_cast<PluginRevocationSeverity>(severity);
        return true;
    }
    case rv_reason:
        if (!decode(p, value, &d)) return false;
        if (!display(d, kPluginIndexReasonMaximumBytes, false)) return fail(p, E::text);
        entry.reason = d.ref;
        return true;
    default:
        if (!number(p, value, &entry.since)) return false;
        return entry.since || fail(p, E::time);
    }
}
bool revoked_entry(void* context, u32 index, JsonValue value) noexcept {
    Parse& p = *static_cast<Parse*>(context);
    PluginIndex& out = *p.out;
    if (index >= kPluginIndexMaximumRevoked) return fail(p, E::limit);
    RevokedContext c{&p, &out.revoked[index], 0};
    if (!object(p, value, revoked_member, &c)) return false;
    if (c.seen != (1u << revoked_field_count) - 1) return fail(p, E::missing_field);
    for (u32 i = 0; i < index; ++i)
        if (out.revoked[i].owner_id == c.entry->owner_id && bytes_equal(out.revoked[i].sha256, c.entry->sha256, 32))
            return fail(p, E::duplicate);
    out.revoked_count = index + 1;
    return true;
}
struct BlockContext {
    Parse* p;
    PluginIndexBlock* entry;
    u32 seen;
};
bool block_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<BlockContext*>(context);
    Parse& p = *c.p;
    const u32 field = member(p, name, kBlockNames, block_field_count, &c.seen);
    if (field == block_field_count) return false;
    Decoded d{};
    if (field == bl_since) {
        if (!number(p, value, &c.entry->since)) return false;
        return c.entry->since || fail(p, E::time);
    }
    if (!decode(p, value, &d)) return false;
    if (field == bl_sha256) return hex_digest(d, c.entry->sha256, 32) || fail(p, E::digest);
    if (!display(d, kPluginIndexReasonMaximumBytes, false)) return fail(p, E::text);
    c.entry->reason = d.ref;
    return true;
}
bool block_entry(void* context, u32 index, JsonValue value) noexcept {
    Parse& p = *static_cast<Parse*>(context);
    PluginIndex& out = *p.out;
    if (index >= kPluginIndexMaximumBlocked) return fail(p, E::limit);
    BlockContext c{&p, &out.blocked[index], 0};
    if (!object(p, value, block_member, &c)) return false;
    if (c.seen != (1u << block_field_count) - 1) return fail(p, E::missing_field);
    for (u32 i = 0; i < index; ++i) if (bytes_equal(out.blocked[i].sha256, c.entry->sha256, 32)) return fail(p, E::duplicate);
    out.blocked_count = index + 1;
    return true;
}

// ---- root ----------------------------------------------------------------------
enum RootField : u32 { r_format, r_provider, r_sequence, r_issued, r_expires, r_plugins, r_revoked, r_blocklist,
    root_field_count };
constexpr Utf8View kRootNames[root_field_count]{utf8_literal("format"), utf8_literal("provider"), utf8_literal("sequence"),
    utf8_literal("issued"), utf8_literal("expires"), utf8_literal("plugins"), utf8_literal("revoked"),
    utf8_literal("blocklist")};
constexpr u32 kRootRequired = (1u << r_blocklist) - 1;
bool root_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& c = *static_cast<ObjectContext*>(context);
    Parse& p = *c.p;
    PluginIndex& out = *p.out;
    const u32 field = member(p, name, kRootNames, root_field_count, &c.seen);
    if (field == root_field_count) return false;
    u64 n = 0;
    u32 count = 0;
    switch (field) {
    case r_format:
        if (!number(p, value, &n)) return false;
        return n == 1 || fail(p, E::format);
    case r_provider: {
        ObjectContext provider{&p, 0};
        if (!object(p, value, provider_member, &provider)) return false;
        if ((provider.seen & kProviderRequired) != kProviderRequired) return fail(p, E::missing_field);
        return out.has_key == (p.policy->kind == PluginProviderKind::custom_signed) || fail(p, E::provider);
    }
    case r_sequence:
        if (!number(p, value, &out.sequence)) return false;
        return out.sequence || fail(p, E::sequence);
    case r_issued: return number(p, value, &out.issued);
    case r_expires: return number(p, value, &out.expires);
    case r_plugins: return array(p, value, plugin_entry, &p, &count);
    case r_revoked: return array(p, value, revoked_entry, &p, &count);
    default: // blocklist: the official kill switch
        if (p.policy->kind != PluginProviderKind::official) return fail(p, E::provider);
        return array(p, value, block_entry, &p, &count);
    }
}

PluginIndexResult finish(PluginIndex* output, E error) noexcept {
    if (error != E::none) clear_bytes(output, sizeof(*output));
    return {error};
}
PluginIndexResult read(Utf8View document, ConstSlice<u8> signature, const P256Key* key, KdfSha256Workspace& workspace,
    const PluginIndexPolicy& policy, Slice<u32> scratch, PluginIndex* output) noexcept {
    const bool signed_kind = policy.kind == PluginProviderKind::official || policy.kind == PluginProviderKind::custom_signed;
    if (!output || !document.data || !workspace.storage.data || !scratch.data || !scratch.count ||
        scratch.count > ~u32{0} / sizeof(u32) || !policy.now_unix || policy.kind > PluginProviderKind::custom_unsigned ||
        signed_kind != (key != nullptr) || (key && (!key->handle || key->kind != P256Kind::ecdsa)))
        return {E::invalid_input};
    const ConstSlice<u8> ranges[]{
        {reinterpret_cast<const u8*>(document.data), document.bytes},
        {reinterpret_cast<const u8*>(scratch.data), scratch.count * static_cast<u32>(sizeof(u32))},
        {reinterpret_cast<const u8*>(output), static_cast<u32>(sizeof(*output))},
        {reinterpret_cast<const u8*>(&workspace), sizeof(workspace)},
        {workspace.storage.data, workspace.storage.capacity},
        {reinterpret_cast<const u8*>(&policy), sizeof(policy)},
        {reinterpret_cast<const u8*>(key), key ? static_cast<u32>(sizeof(*key)) : 0u},
        signature};
    const u32 range_count = key ? 8u : 6u;
    for (u32 i = 0; i < range_count; ++i) {
        if (!byte_range_valid(ranges[i])) return {E::invalid_input};
        for (u32 j = 0; j < i; ++j) if (byte_ranges_overlap(ranges[i], ranges[j])) return {E::invalid_input};
    }
    clear_bytes(output, sizeof(*output));
    if (!document.bytes || document.bytes > kPluginIndexMaximumBytes) return {E::size};
    if (key && signature.count != 64) return {E::signature};
    const ConstSlice<u8> parts[]{
        {reinterpret_cast<const u8*>(kPluginIndexSignatureDomain.data), kPluginIndexSignatureDomain.bytes},
        {reinterpret_cast<const u8*>(document.data), document.bytes}};
    u8 digest[32]{};
    if (!sha256(workspace, {parts, 2}, {digest, 32}).ok()) return {E::invalid_input};
    if (key && !p256_verify_digest(*key, {digest, 32}, signature).ok()) return {E::signature};
    // The reader skips a byte order mark; the format has none.
    if (document.bytes >= 3 && static_cast<u8>(document.data[0]) == 0xEF && static_cast<u8>(document.data[1]) == 0xBB &&
        static_cast<u8>(document.data[2]) == 0xBF) return {E::json};
    const auto scan = json_scan_structurals_avx1(document, scratch);
    if (!scan.ok()) return {E::json};
    Parse p{&policy, output, {document, {scratch.data, scan.count}}, E::none, {}};
    ObjectContext root{&p, 0};
    const auto visited = json_visit_root_object(document, {scratch.data, scan.count}, kPluginIndexMaximumDepth, root_member, &root);
    if (!visited.ok()) return finish(output, p.error != E::none ? p.error : visited.error == JsonReadError::depth_limit ? E::depth : E::json);
    if ((root.seen & kRootRequired) != kRootRequired) return finish(output, E::missing_field);
    PluginIndex& out = *output;
    if (!out.issued || out.expires <= out.issued || out.expires - out.issued > kPluginIndexMaximumValiditySeconds ||
        out.issued > policy.now_unix + kPluginIndexClockSkewSeconds) return finish(output, E::time);
    if (out.expires <= policy.now_unix) {
        if (!policy.accept_expired) return finish(output, E::expired);
        out.expired = true;
    }
    copy_bytes(out.digest, digest, 32);
    return {E::none};
}
void hex(u64 value, u32 digits, char* out) noexcept {
    constexpr char alphabet[] = "0123456789abcdef";
    for (u32 i = 0; i < digits; ++i) out[i] = alphabet[(value >> ((digits - 1 - i) * 4)) & 15];
}

// ---- unverified provider.key -----------------------------------------------------
struct Peek {
    JsonIndexedDocument document;
    u8 key[65];
    bool provider, found, malformed;
};
bool peek_provider_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<Peek*>(context);
    if (!json_string_equals_ascii(name, utf8_literal("key"))) return true;
    char text[131];
    JsonString string{};
    if (p.found || !json_value_as_string(value, &string)) { p.malformed = true; return false; }
    const auto decoded = json_string_decode_utf8(string, {text, sizeof(text)});
    if (!decoded.ok() || !update_public_key_from_hex({text, decoded.bytes}, {p.key, 65})) { p.malformed = true; return false; }
    p.found = true;
    return true;
}
bool peek_root_member(void* context, JsonString name, JsonValue value) noexcept {
    auto& p = *static_cast<Peek*>(context);
    if (!json_string_equals_ascii(name, utf8_literal("provider"))) return true;
    if (p.provider || value.kind != JsonValueKind::object) { p.malformed = true; return false; }
    p.provider = true;
    return json_visit_indexed_object(p.document, value, kPluginIndexMaximumDepth, peek_provider_member, &p).ok();
}
} // namespace

PluginIndexKeyPeek plugin_index_unverified_key(Utf8View document, Slice<u32> scratch, u8* key) noexcept {
    using Peeked = PluginIndexKeyPeek;
    if (!key || !document.data || !document.bytes || document.bytes > kPluginIndexMaximumBytes || !scratch.data || !scratch.count ||
        scratch.count > ~u32{0} / sizeof(u32))
        return Peeked::malformed;
    const ConstSlice<u8> ranges[]{{reinterpret_cast<const u8*>(document.data), document.bytes},
        {reinterpret_cast<const u8*>(scratch.data), scratch.count * static_cast<u32>(sizeof(u32))}, {key, 65}};
    for (u32 i = 0; i < 3; ++i) {
        if (!byte_range_valid(ranges[i])) return Peeked::malformed;
        for (u32 j = 0; j < i; ++j) if (byte_ranges_overlap(ranges[i], ranges[j])) return Peeked::malformed;
    }
    if (document.bytes >= 3 && static_cast<u8>(document.data[0]) == 0xEF && static_cast<u8>(document.data[1]) == 0xBB &&
        static_cast<u8>(document.data[2]) == 0xBF) return Peeked::malformed;
    const auto scan = json_scan_structurals_avx1(document, scratch);
    if (!scan.ok()) return Peeked::malformed;
    Peek p{{document, {scratch.data, scan.count}}, {}, false, false, false};
    const auto visited = json_visit_root_object(document, {scratch.data, scan.count}, kPluginIndexMaximumDepth, peek_root_member, &p);
    if (!visited.ok() || p.malformed) return Peeked::malformed;
    if (!p.found) return Peeked::absent;
    copy_bytes(key, p.key, 65);
    return Peeked::present;
}

Utf8View plugin_index_category_name(u32 index) noexcept {
    return index < kPluginIndexCategoryCount ? kCategories[index] : Utf8View{};
}
Utf8View plugin_index_disclosure_name(u32 index) noexcept {
    return index < kPluginIndexDisclosureCount ? kDisclosures[index] : Utf8View{};
}

PluginIndexResult plugin_index_verify(Utf8View document, ConstSlice<u8> signature, const P256Key& trusted_key,
    KdfSha256Workspace& workspace, const PluginIndexPolicy& policy, Slice<u32> structural_scratch,
    PluginIndex* output) noexcept {
    if (policy.kind == PluginProviderKind::custom_unsigned) return {E::invalid_input};
    return read(document, signature, &trusted_key, workspace, policy, structural_scratch, output);
}

PluginIndexResult plugin_index_parse_unsigned(Utf8View document, KdfSha256Workspace& workspace,
    const PluginIndexPolicy& policy, Slice<u32> structural_scratch, PluginIndex* output) noexcept {
    if (policy.kind != PluginProviderKind::custom_unsigned) return {E::invalid_input};
    return read(document, {}, nullptr, workspace, policy, structural_scratch, output);
}

bool plugin_index_display_text_valid(Utf8View text, u32 maximum, bool required) noexcept {
    return display_text(text, maximum, required);
}

JsonStringDecodeResult plugin_index_text_decode(Utf8View document, PluginIndexText text, Slice<char> output) noexcept {
    if (!document.data || text.offset > document.bytes || text.bytes > document.bytes - text.offset)
        return {0, JsonStringDecodeError::invalid_input};
    return json_string_decode_utf8({{document.data + text.offset, text.bytes}, text.offset, true}, output);
}

PluginIndexResult plugin_index_manifest(const PluginIndex& index, Utf8View document, u32 plugin, u32 version,
    PluginIndexArch arch, NativePluginManifest* output) noexcept {
    if (!output || plugin >= index.plugin_count || version >= index.plugins[plugin].version_count) return {E::invalid_input};
    const PluginIndexPlugin& entry = index.plugins[plugin];
    if (entry.first_version + version >= index.version_count) return {E::invalid_input};
    const PluginIndexVersion& listed = index.versions[entry.first_version + version];
    const PluginIndexFile* file = nullptr;
    for (u32 i = 0; i < listed.file_count && i < kPluginIndexMaximumFiles; ++i) if (listed.files[i].arch == arch) file = &listed.files[i];
    if (!file) return {E::file};
    NativePluginManifest m{};
    m.owner_id = entry.owner_id;
    m.abi_version = listed.abi;
    m.min_host_abi = listed.min_host_abi;
    m.permissions = listed.permissions;
    const auto name = plugin_index_text_decode(document, entry.name, {m.name, sizeof(m.name) - 1});
    const auto summary = plugin_index_text_decode(document, entry.summary, {m.description, sizeof(m.description) - 1});
    const auto publisher = plugin_index_text_decode(document, entry.publisher, {m.publisher, sizeof(m.publisher) - 1});
    if (!name.ok() || !summary.ok() || !publisher.ok()) return {E::invalid_input};
    m.name_bytes = name.bytes; m.description_bytes = summary.bytes; m.publisher_bytes = publisher.bytes;
    m.version_bytes = update_version_text(listed.version_code, {m.version, sizeof(m.version) - 1});
    hex(entry.owner_id, 16, m.dll);
    m.dll[16] = '-';
    for (u32 i = 0; i < 32; ++i) hex(file->sha256[i], 2, m.dll + 17 + i * 2);
    copy_bytes(m.dll + 81, ".dll", 4);
    m.dll_bytes = 85;
    copy_bytes(m.sha256, file->sha256, 32);
    // Index text is stricter than the manifest's, so only the ABI can fail here.
    if (!native_plugin_manifest_valid(m)) return {E::abi};
    *output = m;
    return {E::none};
}

} // namespace maicord
