#pragma once

#include "maicord/app/update_manifest.hpp"

namespace maicord {

// The channel changelog published beside each manifest (tools/release):
//   {"format":1,"product":"MaiCord","channel":"stable"|"preview","version":V,
//    "base":B,"entries":[{"version":v,"date":unix,"commit":"<40 hex>"|"",
//    "text":"..."}]}
// It lists every change in (base, version], newest first: release notes for
// stable, commit subjects numbered by the master build that first had them
// for preview. Signed like the manifest under its own domain.
inline constexpr u32 kUpdateChangelogMaximumBytes = 65536;
inline constexpr u32 kUpdateChangelogMaximumEntries = 100;
inline constexpr u32 kUpdateChangelogTextMaximumBytes = 8192;
inline constexpr u32 kUpdateChangelogStructurals = 4096;
inline constexpr Utf8View kUpdateChangelogSignatureDomain = utf8_literal("MaiCord update changelog v1\n");

struct UpdateChangelogEntry {
    u64 version, date_unix_seconds;
    u32 text_offset, text_bytes;
    char commit[40];
    bool has_commit;
};
struct UpdateChangelog {
    u64 version, base;
    UpdateChannel channel;
    u32 count, text_bytes;
    UpdateChangelogEntry entries[kUpdateChangelogMaximumEntries];
    // Decoded entry text: valid UTF-8 without control characters other than
    // line feeds, nor bidirectional overrides.
    char text[kUpdateChangelogMaximumBytes];
};
enum class UpdateChangelogError : u8 { none, invalid_input, signature, json, field, missing_field, bounds };

// Verifies the publisher signature, then the strict schema and bounds: at most
// 100 entries in non-increasing version order within (base, version], each
// text 1-8192 bytes. On failure output->count and version are zero. The key
// comes from the application's trusted inventory. Scratch holds the
// structural index (kUpdateChangelogStructurals is enough for any document
// the publisher writes).
UpdateChangelogError update_changelog_verify(Utf8View document, ConstSlice<u8> signature, const P256Key& trusted_key,
    KdfSha256Workspace& workspace, Slice<u32> structural_scratch, UpdateChangelog* output) noexcept;

inline Utf8View update_changelog_text(const UpdateChangelog& log, const UpdateChangelogEntry& entry) noexcept {
    return {log.text + entry.text_offset, entry.text_bytes};
}

// Plain display text from the publisher's Markdown subset: "#" headings lose
// their markers, "- "/"* " items become "• " bullets, [text](target) keeps
// only the text, emphasis and code markers are dropped and runs of blank lines
// collapse. Nothing becomes a link. Returns the bytes written; output is
// truncated at a character boundary when too small.
u32 update_changelog_display_text(Utf8View text, Slice<char> output) noexcept;

// "Oct 19, 2026", or "Oct 19" without the year (UTC); 0 when it does not fit.
u32 update_changelog_date_text(u64 unix_seconds, bool year, Slice<char> output) noexcept;

} // namespace maicord
