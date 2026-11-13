#pragma once

#include "maicord/core/os_text.hpp"
#include "maicord/platform/crypto/hkdf_sha256.hpp"
#include "maicord/platform/crypto/p256.hpp"

// The official catalog's index, format 2, in four signed files, each with its detached 64-byte .sig (P-256/P1363
// over SHA-256 of the domain followed by the document): index-4.json, which the Linux client reads, lists every item
// with an effect's interface 4 package (the one that carries the GLSL section the GLES backend runs); index-3.json,
// which the Windows client reads, lists every item with its DXBC package; index-full.json (0.6.3) leaves out the
// themes and plugins that name an interface; index.json (0.6.0 to 0.6.2) also the effects that do. This reader
// ignores members it doesn't know and items of kinds it doesn't know, so later fields join index-3.json and
// index-4.json without another split. tools/release/plugin_index.py is the reference writer and verifier;
// tests/fixtures/plugin_index/format2 holds the shared vectors.
namespace maicord {

// The index file this client reads next to the catalog's address, and its signature.
inline constexpr Utf8View kCatalogIndexFile = MAICORD_OS_TEXT("index-3.json", "index-4.json");
inline constexpr Utf8View kCatalogIndexSignatureFile = MAICORD_OS_TEXT("index-3.json.sig", "index-4.json.sig");
inline constexpr Utf8View kCatalogIndexSignatureDomain = utf8_literal("MaiCord plugin index v2\n");
inline constexpr u32 kCatalogIndexMaximumBytes = 256 * 1024;
// Structural offsets a valid index can need; denser documents are refused.
inline constexpr u32 kCatalogIndexStructuralCapacity = kCatalogIndexMaximumBytes / 2;
inline constexpr u32 kCatalogIndexMaximumDepth = 8;
inline constexpr u32 kCatalogIndexMaximumItems = 128;
inline constexpr u32 kCatalogIndexMaximumVersions = 4; // per plugin; themes and effects have one
inline constexpr u32 kCatalogIndexMaximumListedVersions = kCatalogIndexMaximumItems * kCatalogIndexMaximumVersions;
inline constexpr u32 kCatalogIndexMaximumAuthors = 4;
inline constexpr u32 kCatalogIndexMaximumCategories = 3;
inline constexpr u32 kCatalogIndexMaximumPreviews = 4;
inline constexpr u32 kCatalogIndexMaximumHosts = 8; // MC_NETWORK_HOSTS_MAX
inline constexpr u32 kCatalogIndexMaximumRevoked = 64;
inline constexpr u32 kCatalogIndexMaximumBlocked = 64;
// Per effect: 16 scalar or toggle knobs, 4 colours, 8 choices of 2..16 entries.
inline constexpr u32 kCatalogIndexMaximumScalarKnobs = 16;
inline constexpr u32 kCatalogIndexMaximumColorKnobs = 4;
inline constexpr u32 kCatalogIndexMaximumChoiceKnobs = 8;
inline constexpr u32 kCatalogIndexMaximumChoices = 16;
// Totals over the whole index; a document past them fails with `limit`.
inline constexpr u32 kCatalogIndexMaximumKnobs = 1024;
inline constexpr u32 kCatalogIndexMaximumChoiceTexts = 2048;
inline constexpr u32 kCatalogIndexMaximumHostTexts = kCatalogIndexMaximumListedVersions * kCatalogIndexMaximumHosts;
inline constexpr u32 kCatalogIndexDetailsMaximumBytes = 16 * 1024;
inline constexpr u32 kCatalogIndexPreviewMaximumBytes = 256 * 1024;
inline constexpr u32 kCatalogIndexLoopMaximumBytes = 2 * 1024 * 1024;
inline constexpr u32 kCatalogIndexPluginMaximumBytes = 8 * 1024 * 1024;
inline constexpr u32 kCatalogIndexThemeMaximumBytes = 64 * 1024;
inline constexpr u32 kCatalogIndexEffectMaximumBytes = 1536 * 1024;
// Indexes are signed for 30 days; the client refuses longer validity, an issue
// time more than the clock skew ahead of now, and expiry at or before now.
inline constexpr u64 kCatalogIndexMaximumValiditySeconds = 35ull * 86400;
inline constexpr u64 kCatalogIndexClockSkewSeconds = 600;
inline constexpr u32 kCatalogSlugMaximumBytes = 40;
inline constexpr u32 kCatalogVersionTextMaximumBytes = 23;
// Knob numbers are fixed point: thousandths.
inline constexpr i64 kCatalogKnobScale = 1000;

enum class CatalogKind : u8 { plugin, theme, effect };
inline constexpr u32 kCatalogKindCount = 3;
enum class CatalogThemeBase : u8 { dark, light, amoled };
enum class CatalogCost : u8 { light, medium, heavy };
enum class CatalogBlend : u8 { normal, screen, add };
enum class CatalogKnobType : u8 { float_value, int_value, toggle, color, choice };
enum class CatalogRevocationSeverity : u8 { none, malicious, broken, withdrawn };

// The error names of tools/release/plugin_index.py (ERRORS), plus invalid_input for bad arguments.
enum class CatalogIndexError : u8 {
    none,
    invalid_input,
    size,            // empty or above kCatalogIndexMaximumBytes (checked before hashing)
    signature,       // not exactly 64 bytes, or by no trusted key under this domain
    json,            // malformed JSON, a byte order mark or more structure than the scratch holds
    depth,           // nested deeper than kCatalogIndexMaximumDepth
    unknown_field,   // a member the schema does not define for this object (or this kind)
    duplicate_field, // a member named twice in one object
    missing_field,
    type,            // wrong JSON type, or a number of the wrong shape
    format,          // format is not 2
    sequence,        // sequence is 0
    time,            // issued, expires, released or since out of range, or validity too long
    expired,         // expires at or before now (accept_expired reads it anyway)
    limit,           // a count or number out of range
    text,            // display text or a colour
    identifier,      // slug, category, license, GitHub login or knob id grammar
    duplicate,       // a repeated entry where entries are unique
    version,         // not MAJOR.MINOR.PATCH (each 0..999, no leading zeros)
    order,           // versions not newest first, permissions not in bit order
    api_level,
    permission,      // an unknown permission, or network listed without hosts (or the reverse)
    choice,          // a value outside its fixed set
    digest,          // sha256 is not 64 lowercase hex digits, commit not 40
    file,            // a file, details, preview or loop byte count out of range
    url,             // not an https URL on a DNS host
    host,            // a network host the plugin loader would refuse
};
struct CatalogIndexResult {
    CatalogIndexError error;
    constexpr bool ok() const noexcept { return error == CatalogIndexError::none; }
};

// Decoded UTF-8 in CatalogIndex::text; bytes 0 when absent.
struct CatalogString {
    u32 offset, bytes;
};
struct CatalogBlob {
    u8 sha256[32];
    u32 bytes;
};
struct CatalogImage {
    CatalogBlob blob;
    u32 width, height;
};
struct CatalogAuthor {
    CatalogString name, github;
};
// A plugin's listed version, or the one version of a theme or effect (permissions and hosts zero). api_level is a
// plugin version's MC_API_LEVEL.
struct CatalogVersion {
    u64 code; // major * 1000000 + minor * 1000 + patch
    u64 released;
    u64 permissions; // MC_PERM_* (maicord_plugin.h)
    CatalogString text, changelog;
    u32 api_level;
    u32 first_host, host_count; // into CatalogIndex::hosts
    CatalogBlob file;
    u8 commit[20];
    bool has_commit;
};
struct CatalogKnob {
    CatalogString id, label;
    CatalogKnobType type;
    bool percent; // a float knob shown as a percentage (0.75 reads "75 %")
    // float and int: thousandths; toggle: value 0 or 1; color: value 0xRRGGBB; choice: value is the index.
    i64 minimum, maximum, value;
};
struct CatalogItem {
    CatalogKind kind;
    // What the item needs of the client, 0 when the index leaves it out (1): a plugin's catalog entry level (1), a
    // theme's document interface (kClientThemeInterfaceMaximum) or an effect's .mcfx interface (kMcfxInterfaceMaximum).
    // Above the client's, the item shows as needing a newer MaiCord.
    u32 interface_level;
    CatalogThemeBase base;
    CatalogCost cost;
    CatalogBlend blend;
    CatalogString slug, name, summary, license, source, homepage;
    CatalogAuthor authors[kCatalogIndexMaximumAuthors];
    u32 author_count;
    CatalogString categories[kCatalogIndexMaximumCategories];
    u32 category_count;
    CatalogBlob details;
    CatalogImage previews[kCatalogIndexMaximumPreviews];
    u32 preview_count;
    u32 first_version, version_count; // newest first, into CatalogIndex::versions
    // Themes.
    CatalogString effect;
    u32 swatches[4]; // 0xRRGGBB: background, surface, text, accent
    bool has_swatches;
    // Effects.
    bool has_loop;
    CatalogImage loop;
    u32 first_knob, knob_count;
};
struct CatalogRevocation {
    u8 sha256[32];
    CatalogRevocationSeverity severity; // malicious for the blocklist
    CatalogString reason;
};
// About 520 KiB. Every string is decoded once into text.
struct CatalogIndex {
    u64 sequence, issued, expires;
    u8 digest[32]; // SHA-256(domain || document): the acceptance identity
    bool expired;  // only with accept_expired
    u32 item_count, version_count, knob_count, host_count, choice_count, revoked_count, blocked_count, text_bytes;
    CatalogItem items[kCatalogIndexMaximumItems];
    CatalogVersion versions[kCatalogIndexMaximumListedVersions];
    CatalogKnob knobs[kCatalogIndexMaximumKnobs];
    CatalogString hosts[kCatalogIndexMaximumHostTexts];
    CatalogRevocation revoked[kCatalogIndexMaximumRevoked];
    CatalogRevocation blocked[kCatalogIndexMaximumBlocked];
    char text[kCatalogIndexMaximumBytes];
};
inline Utf8View catalog_text(const CatalogIndex& index, CatalogString s) noexcept {
    return {index.text + s.offset, s.bytes};
}

// Format 2: unknown members (also for the item's kind) and items of an unknown kind are skipped; duplicate or
// missing members are refused, every limit is checked, display text bans controls,
// bidi controls and zero-width characters. The size is checked before
// hashing; the signature (by any of keys) before parsing. No allocation,
// network or file access. On failure *output is zeroed.
CatalogIndexResult catalog_index_verify(Utf8View document, ConstSlice<u8> signature, ConstSlice<const P256Key*> keys,
                                        KdfSha256Workspace& workspace, u64 now_unix, bool accept_expired,
                                        Slice<u32> structural_scratch, CatalogIndex* output) noexcept;

// A catalog slug: 1..kCatalogSlugMaximumBytes of lower-case letters, digits and '-', not starting with '-'.
bool catalog_slug_valid(Utf8View text) noexcept;
// "MAJOR.MINOR.PATCH" (each 0..999, no leading zeros) as major*1e6+minor*1e3+patch; false otherwise.
bool catalog_version_code(Utf8View text, u64* code) noexcept;
// "files/<sha256>.dll|json|mcfx" (a path below the catalog base); returns the bytes written.
u32 catalog_file_name(CatalogKind kind, const u8 (&sha256)[32], Slice<char> output) noexcept;
// The index of slug among items of kind, or ~0u.
u32 catalog_index_find(const CatalogIndex& index, CatalogKind kind, const Utf8View& slug) noexcept;
// What the index says about a file: none, or the strongest severity listed for sha256 (the blocklist is malicious).
CatalogRevocationSeverity catalog_index_revocation(const CatalogIndex& index, const u8* sha256,
                                                   const CatalogRevocation** entry = nullptr) noexcept;

} // namespace maicord
