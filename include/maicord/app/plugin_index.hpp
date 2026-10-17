#pragma once

#include "maicord/app/plugin_manifest.hpp"
#include "maicord/platform/hkdf_sha256.hpp"
#include "maicord/platform/p256.hpp"

namespace maicord {

// Plugin catalog index (v1/index.json) and its detached signature
// (v1/index.json.sig, 64 bytes, P-256/P1363 over SHA-256 of this domain
// followed by the document bytes). The domain keeps update-manifest and index
// signatures apart even under a shared key. tools/release/plugin_index.py is
// the reference writer; tests/fixtures/plugin_index holds shared vectors.
inline constexpr Utf8View kPluginIndexSignatureDomain = utf8_literal("MaiCord plugin index v1\n");
inline constexpr u32 kPluginIndexMaximumBytes = 256 * 1024;
// Structural offsets a valid index can need; denser documents are refused.
inline constexpr u32 kPluginIndexStructuralCapacity = kPluginIndexMaximumBytes / 2;
// Root, plugins, plugin, versions, version, files, file, github_release.
inline constexpr u32 kPluginIndexMaximumDepth = 8;
inline constexpr u32 kPluginIndexMaximumPlugins = 128;
inline constexpr u32 kPluginIndexMaximumVersions = 4; // per plugin, previews included
inline constexpr u32 kPluginIndexMaximumListedVersions = kPluginIndexMaximumPlugins * kPluginIndexMaximumVersions;
inline constexpr u32 kPluginIndexMaximumFiles = 2; // per version, one per architecture
inline constexpr u32 kPluginIndexMaximumAuthors = 4;
inline constexpr u32 kPluginIndexMaximumCategories = 3;
inline constexpr u32 kPluginIndexMaximumRevoked = 64;
inline constexpr u32 kPluginIndexMaximumBlocked = 64;
inline constexpr u32 kPluginIndexDetailsMaximumBytes = 16 * 1024;
inline constexpr u64 kPluginIndexFileMaximumBytes = 64ull * 1024 * 1024; // kNativePluginDllMaximumBytes
inline constexpr u32 kPluginIndexMaximumHostAbi = 0xFFFF;
// Indexes are signed for 30 days; the client refuses longer validity, an
// issue time more than the clock skew ahead of now, and expiry at or before now.
inline constexpr u64 kPluginIndexMaximumValiditySeconds = 35ull * 86400;
inline constexpr u64 kPluginIndexClockSkewSeconds = 600;
// Byte limits of the UTF-8 text after JSON unescaping.
inline constexpr u32 kPluginIndexProviderIdMaximumBytes = 40;
inline constexpr u32 kPluginIndexProviderNameMaximumBytes = 64;
inline constexpr u32 kPluginIndexSlugMaximumBytes = 40;
inline constexpr u32 kPluginIndexNameMaximumBytes = kNativePluginNameCapacity - 1;           // 95
inline constexpr u32 kPluginIndexSummaryMaximumBytes = kNativePluginDescriptionCapacity - 1; // 255
inline constexpr u32 kPluginIndexPublisherMaximumBytes = kNativePluginPublisherCapacity - 1; // 63
inline constexpr u32 kPluginIndexAuthorMaximumBytes = 64;
inline constexpr u32 kPluginIndexGithubLoginMaximumBytes = 39;
inline constexpr u32 kPluginIndexLicenseMaximumBytes = 32;
inline constexpr u32 kPluginIndexUrlMaximumBytes = 256;
inline constexpr u32 kPluginIndexChangelogMaximumBytes = 512;
inline constexpr u32 kPluginIndexReasonMaximumBytes = 128;
inline constexpr u32 kPluginIndexPathMaximumBytes = 128;
inline constexpr u32 kPluginIndexTagMaximumBytes = 64;
inline constexpr u32 kPluginIndexAssetMaximumBytes = 128;
// Fixed vocabularies, bit i of PluginIndexPlugin::categories / disclosures.
inline constexpr u32 kPluginIndexCategoryCount = 10;  // chat appearance notifications privacy utility voice
                                                      // accessibility moderation developer fun
inline constexpr u32 kPluginIndexDisclosureCount = 4; // network files processes input
Utf8View plugin_index_category_name(u32 index) noexcept;

// official: the compiled provider; its index says id "maicord", carries no
// key, names a commit for every version and derives file locations.
// custom_signed: a TOFU-pinned key; the index carries that key and gives
// each file a path or github_release location. custom_unsigned: Developer
// Mode only, no signature and no key.
enum class PluginProviderKind : u8 { official, custom_signed, custom_unsigned };
enum class PluginIndexArch : u8 { x64, arm64 };
enum class PluginIndexFileSource : u8 { derived, path, github_release };
enum class PluginRevocationSeverity : u8 { malicious, broken, withdrawn };

enum class PluginIndexError : u8 {
    none,
    invalid_input,   // arguments, never document content
    size,            // empty or above kPluginIndexMaximumBytes (checked before hashing)
    signature,       // not exactly 64 bytes or not by the trusted key under this domain
    json,            // malformed JSON, a byte order mark or more structure than the scratch holds
    depth,           // nested deeper than kPluginIndexMaximumDepth
    unknown_field,   // a member the schema does not define
    duplicate_field, // a member named twice in one object
    missing_field,   // an always-required member is absent
    type,            // wrong JSON type, or a number that is not an unsigned 64-bit integer
    format,          // format is not 1
    provider,        // provider id/key, commit or blocklist rules of the provider kind
    key_changed,     // a custom index names a key other than the pinned one
    sequence,        // sequence is 0
    time,            // issued, expires, released or since out of range, or validity too long
    expired,         // expires at or before now (policy.accept_expired reads it anyway)
    limit,           // a list is empty where entries are required, or holds too many
    text,            // display text: invalid UTF-8, too long or empty, controls, bidi or zero-width
    identifier,      // provider id, slug, license or GitHub login grammar
    duplicate,       // repeated slug, owner, permission, category, disclosure, arch or list entry
    owner,           // owner_id 0 or 1 (the native settings owner)
    version,         // version is not MAJOR.MINOR.PATCH[-preview.N] naming version_code
    order,           // version codes do not strictly decrease
    abi,             // abi, min_host_abi or max_host_abi out of range or order
    permission,      // a permission name outside the manifest table
    choice,          // a category, disclosure, arch or severity outside its fixed set
    digest,          // sha256 is not 64 lowercase hex digits, commit not 40
    file,            // a file or details byte count out of range
    location,        // file location missing, forbidden for the kind, or malformed
    url,             // homepage is not an https URL on a DNS host
};
struct PluginIndexResult {
    PluginIndexError error;
    constexpr bool ok() const noexcept { return error == PluginIndexError::none; }
};

struct PluginIndexPolicy {
    u64 now_unix;          // trusted wall clock, nonzero
    PluginProviderKind kind;
    bool accept_expired;   // reloading a cached index read-only: parse it and set expired
    u8 pinned_key[65];     // custom_signed: the provider.key the index must name
};

// A JSON string body inside the verified document (still escaped). Decode
// with plugin_index_text_decode against the same document bytes.
struct PluginIndexText {
    u32 offset, bytes;
};
struct PluginIndexFile {
    u8 sha256[32];
    u32 bytes;
    PluginIndexArch arch;
    PluginIndexFileSource source;
    PluginIndexText path;                   // source path: relative to the provider base
    PluginIndexText repository, tag, asset; // source github_release
};
struct PluginIndexVersion {
    u64 version_code; // update_version_number; "version" is its text
    u64 released;
    u64 permissions;  // MC_PERM_* mask
    u32 abi, min_host_abi, max_host_abi; // max_host_abi 0: no bound
    u32 file_count;
    PluginIndexText changelog;
    u8 commit[20];
    bool has_commit;
    PluginIndexFile files[kPluginIndexMaximumFiles];
};
struct PluginIndexAuthor {
    PluginIndexText name, github; // github.bytes 0 when absent
};
struct PluginIndexPlugin {
    u64 owner_id;
    PluginIndexText slug, name, summary, publisher, license, homepage;
    PluginIndexAuthor authors[kPluginIndexMaximumAuthors];
    u32 author_count;
    u32 categories, disclosures; // bit masks over the fixed vocabularies
    // Listed versions, newest first: versions[first_version .. + version_count).
    // Preview versions are validated, then ignored (index.ignored_versions).
    u32 first_version, version_count;
    u32 details_bytes; // 0 when the plugin has no details document
    u8 details_sha256[32];
};
struct PluginIndexRevocation {
    u64 owner_id, since;
    u8 sha256[32];
    PluginRevocationSeverity severity;
    PluginIndexText reason;
};
// Official only: SHA-256 values blocked whichever provider installed them.
struct PluginIndexBlock {
    u64 since;
    u8 sha256[32];
    PluginIndexText reason;
};
// About 140 KiB; callers allocate it per check.
struct PluginIndex {
    u64 sequence, issued, expires;
    u8 digest[32]; // SHA-256(domain || document): the acceptance identity
    u8 key[65];    // provider.key of a custom signed index
    bool has_key;
    bool expired;  // only with policy.accept_expired
    PluginIndexText provider_id, provider_name, provider_homepage;
    u32 plugin_count, version_count, revoked_count, blocked_count, ignored_versions;
    PluginIndexPlugin plugins[kPluginIndexMaximumPlugins];
    PluginIndexVersion versions[kPluginIndexMaximumListedVersions];
    PluginIndexRevocation revoked[kPluginIndexMaximumRevoked];
    PluginIndexBlock blocked[kPluginIndexMaximumBlocked];
};

// Strict format 1: unknown, duplicate or missing members are refused, every
// limit is checked, and display text is the manifest's text rule plus a ban
// on bidi controls (U+061C, U+200E-200F, U+202A-202E, U+2066-2069) and
// zero-width characters (U+200B-200D, U+2060-2064, U+FEFF). The size is
// checked before hashing; the signature before parsing. The trusted key comes
// from the compiled inventory (official) or the provider pin, never from the
// document. No allocation, network or file access. Document, signature,
// scratch, workspace, key, policy and output must not overlap. On failure
// *output is zeroed, never partially filled. kind is official or custom_signed.
PluginIndexResult plugin_index_verify(Utf8View document, ConstSlice<u8> signature, const P256Key& trusted_key,
    KdfSha256Workspace& workspace, const PluginIndexPolicy& policy, Slice<u32> structural_scratch,
    PluginIndex* output) noexcept;
// The same checks without a signature, for a Developer Mode provider the user
// accepted as unsigned (policy.kind custom_unsigned).
PluginIndexResult plugin_index_parse_unsigned(Utf8View document, KdfSha256Workspace& workspace,
    const PluginIndexPolicy& policy, Slice<u32> structural_scratch, PluginIndex* output) noexcept;

// The display-text rule above for one decoded string: valid UTF-8 of at most
// maximum bytes (nonempty when required).
bool plugin_index_display_text_valid(Utf8View text, u32 maximum, bool required) noexcept;

// provider.key of a document whose signature has NOT been checked: only the
// candidate key a first-use or key-change flow verifies the same document
// with. Never makes a document trusted. malformed: not JSON (or a BOM), a
// repeated provider or key member, or not a 130-hex uncompressed point. key
// is written only when present.
enum class PluginIndexKeyPeek : u8 { absent, present, malformed };
PluginIndexKeyPeek plugin_index_unverified_key(Utf8View document, Slice<u32> structural_scratch, u8* key) noexcept;

// Buffers of one check, probe or cache load (about 910 KiB), allocated per
// operation. record is the cache file layout: the 64-byte signature (zero when
// unsigned) then the document, so the pair cannot tear.
struct PluginIndexWork {
    u8 record[64 + kPluginIndexMaximumBytes];
    u32 document_bytes;
    u32 scratch[kPluginIndexStructuralCapacity];
    PluginIndex index;
};
inline Utf8View plugin_index_work_document(const PluginIndexWork& work) noexcept {
    return {reinterpret_cast<const char*>(work.record + 64), work.document_bytes};
}

// Unescapes one text field of the document the index was parsed from.
JsonStringDecodeResult plugin_index_text_decode(Utf8View document, PluginIndexText text, Slice<char> output) noexcept;

// The schema-3 manifest the client writes for one listed version: owner, ABI,
// min_host_abi, name, summary as description, publisher, permissions, the
// version text and the managed DLL name <owner16>-<sha64>.dll of the file for
// arch. Fails with `file` when the version has no file for arch and `abi` when
// a manifest cannot describe it (abi other than 5). output is unchanged on failure.
PluginIndexResult plugin_index_manifest(const PluginIndex& index, Utf8View document, u32 plugin, u32 version,
    PluginIndexArch arch, NativePluginManifest* output) noexcept;

} // namespace maicord
