#pragma once
#include "maicord/ui/text/text.hpp"
#include "maicord/text/fonts/prepare.hpp"
#include "maicord/text/fonts/catalog.hpp"
namespace maicord {
struct NativeParagraphHitStorage;
struct NativeParagraphCluster;
// Pre-merge presentation span capacity for mixed-font metrics.
// Zero means invalid input or no storage needed for an unstyled single face.
u32 native_text_candidate_style_capacity(Utf8View,ConstSlice<TextStyleSpan>) noexcept;
// Metric slots the prepared shape needs after a failed attempt; it can exceed
// the source-only estimate. Grow only that storage and retry.
u32 native_text_candidate_hit_style_capacity(const TextSystem&) noexcept;
// One adapter layout: validated text, folded style spans and resolved hit
// ranges within a box. The shared text entry point checks every field.
struct NativeTextRequest {
    Utf8View text;
    ConstSlice<TextStyleSpan> styles;
    float width;
    float height;
    TextWrapping wrapping = TextWrapping::wrap;
    TextOverflow overflow = TextOverflow::clip;
    ConstSlice<TextHitRange> hits{};
};
// Retained editor geometry, laid out clipped: unstyled ASCII/resolved Latin
// and eligible mixed text/emoji take the paragraph path, styled text the styled
// one. Caller-owned storage must be disjoint from candidate scratch; mixed
// font metrics need native_text_candidate_style_capacity entries.
bool native_text_candidate_hits(const TextSystem&, const NativeTextRequest&, NativeParagraphHitStorage&,
                                TextMeasureResult*) noexcept;
// Returns and clears the output-capacity failure flag of the current public
// shape attempt; scratch/profile errors never set it.
bool native_text_candidate_take_output_failure(const TextSystem&) noexcept;
// Ellipsis dispatch: the fitting, styled and multiline alternatives share one
// staged generation retry; a failed alternative cannot consume another.
bool native_text_candidate_ellipsis(const TextSystem&, const NativeTextRequest&, ShapedText*) noexcept;
bool native_text_context_initialize(NativeTextContext**, NativeFontSession*) noexcept;
void native_text_context_release(NativeTextContext**) noexcept;
// Transactional rebuild. The callback borrows a staged context and must not keep
// its pointer. Commit preserves the context address and format/scope handles,
// assigns new format identities and keeps independently owned runs. Failure
// leaves the original context intact. Nested rebuilds are rejected.
using NativeTextRebuild = bool (*)(const TextSystem&, void*) noexcept;
bool native_text_context_rebuild(const TextSystem&, NativeTextRebuild, void*) noexcept;

// Immutable LTR/en-US format. Family storage is copied; a handle lives as long
// as the native context. Scope zero is the native system catalog.
struct NativeTextFormatDescription {
    ConstSlice<u16> family;
    u32 scope;
    NativeFontProperties properties;
    float em_size, tab_stop;
    bool word_wrap;
};
bool native_text_format_create(NativeTextContext*, const NativeTextFormatDescription&,
                               u32* handle) noexcept;
// Configure once, before any shape/measure binding, with a context-owned format.
// Rebuild preserves the handle.
bool native_text_context_set_code_format(NativeTextContext*, u32 handle) noexcept;
// Exact-family preparation: false when the family is missing, no fallback.
bool native_text_format_prepare(NativeTextContext*, u32 handle) noexcept;
// Opaque identity, never reused across context lifetimes; zero without a
// native context or format.
u64 native_text_format_identity(const TextSystem&) noexcept;
// Registers a private face-zero/default-axis font file and returns its scope.
bool native_text_register_font_path(NativeTextContext*,const wchar_t*,u32* scope) noexcept;
struct NativeTextCandidateCounts {u64 shaped,measured,arabic_shaped,arabic_measured,paragraphs_shaped,paragraphs_measured,latin_shaped,latin_measured;};
NativeTextCandidateCounts native_text_candidate_counts(const TextSystem&) noexcept;
struct NativeTextStorageCounts {u64 reserved,committed,peak,retries;};
NativeTextStorageCounts native_text_candidate_storage_counts(const TextSystem&) noexcept;
// Read-only diagnostic view, invalidated by the next native text operation.
ConstSlice<NativeParagraphCluster> native_text_candidate_prepared_clusters(const TextSystem&) noexcept;
// Input and capacities must already be validated by the shared text entry point.
// Handles an ASCII/resolved Latin paragraph, eligible mixed Latin/Common text with
// overridden emoji (the platform's emoji family: Segoe UI Emoji, Noto Color Emoji; normal weight/style, no added tracking), or a
// fitting homogeneous Arabic line. Mixed items need actual font cuts, resolved LTR
// Latin/Common text and supported cluster boundaries. Latin needs original-scalar
// coverage in one face, resolved LTR/en-US items and only the supported newline/tab
// controls. Arabic needs an initial Arabic letter, no outer spaces, and only Arabic
// letters, recognized marks, join controls and internal spaces; Arabic digits,
// general bidi, fallback and truncating ellipsis take the general path. False leaves
// output untouched and retains no partially shaped run. A non-null output requests
// shaping, otherwise measurement is returned. The line path reads no styles or hits.
bool native_text_candidate_line(const TextSystem&, const NativeTextRequest&, ShapedText* output,
                                TextMeasureResult* measure) noexcept;
bool native_text_candidate_styled(const TextSystem&, const NativeTextRequest&, ShapedText*, TextMeasureResult*) noexcept;
}
