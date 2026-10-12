#pragma once
#include "maicord/ui/text.hpp"
#include "maicord/text/font_prepare.hpp"
#include "maicord/text/font_catalog.hpp"
namespace maicord {
struct NativeParagraphHitStorage;
struct NativeParagraphCluster;
// Pre-merge presentation span capacity for mixed-font metrics.
// Zero means invalid input or no storage needed for an unstyled single face.
u32 native_text_candidate_style_capacity(Utf8View,ConstSlice<TextStyleSpan>) noexcept;
// After an editor attempt, the prepared shape may need more metric slots than
// the source-only estimate. Grow only that storage and retry the shape.
u32 native_text_candidate_hit_style_capacity(const TextSystem&) noexcept;
// Unstyled ASCII/resolved Latin and eligible mixed text/emoji retained geometry;
// storage belongs to the caller and must be disjoint from candidate scratch.
// Mixed font metrics need native_text_candidate_style_capacity entries.
bool native_text_candidate_hits(const TextSystem&,Utf8View,float width,float height,
    TextWrapping,NativeParagraphHitStorage,TextMeasureResult*) noexcept;
// Failure status for the current public shape attempt. The adapter clears
// it before starting and when reporting failure; scratch/profile errors never set it.
bool native_text_candidate_take_output_failure(const TextSystem&) noexcept;
// Public ellipsis dispatch: all fitting/styled/multiline alternatives share one
// staged generation retry. A failed alternative cannot consume another retry.
bool native_text_candidate_ellipsis(const TextSystem&, Utf8View, ConstSlice<TextStyleSpan>, float width,
                                    float height, ShapedText*, TextWrapping,
                                    ConstSlice<TextHitRange>) noexcept;
bool native_text_context_initialize(NativeTextContext**, NativeFontSession*) noexcept;
void native_text_context_release(NativeTextContext**) noexcept;
// Slow-path transactional rebuild. The callback borrows a staged context and must not
// keep its context pointer. Commit preserves the context address and format/scope
// handles, assigns new format identities, and keeps independently owned runs.
// Failure leaves the original context intact. Nested rebuilds are rejected.
using NativeTextRebuild = bool (*)(const TextSystem&, void*) noexcept;
bool native_text_context_rebuild(const TextSystem&, NativeTextRebuild, void*) noexcept;

// Immutable LTR/en-US MaiCord format. Family storage is copied; handle lifetime
// is the native context's lifetime. Scope zero is the native system catalog.
struct NativeTextFormatDescription {
    ConstSlice<u16> family;
    u32 scope;
    NativeFontProperties properties;
    float em_size, tab_stop;
    bool word_wrap;
};
bool native_text_format_create(NativeTextContext*, const NativeTextFormatDescription&,
                               u32* handle) noexcept;
// Configure once, before any shape/measure binding, using a context-owned format.
// Recovery preserves this handle alongside other immutable declarations.
bool native_text_context_set_code_format(NativeTextContext*, u32 handle) noexcept;
// Exact-family preparation; it does not fall back when the family is missing.
bool native_text_format_prepare(NativeTextContext*, u32 handle) noexcept;
// Opaque native identity is never reused across context lifetimes; zero
// without a native context or format.
u64 native_text_format_identity(const TextSystem&) noexcept;
// Registers a private face-zero/default-axis font file and returns its scope.
bool native_text_register_font_path(NativeTextContext*,const wchar_t*,u32* scope) noexcept;
struct NativeTextCandidateCounts {u64 shaped,measured,arabic_shaped,arabic_measured,paragraphs_shaped,paragraphs_measured,latin_shaped,latin_measured;};
NativeTextCandidateCounts native_text_candidate_counts(const TextSystem&) noexcept;
struct NativeTextStorageCounts {u64 reserved,committed,peak,retries,catalog_build_ticks;};
NativeTextStorageCounts native_text_candidate_storage_counts(const TextSystem&) noexcept;
// Read-only diagnostic view; invalidated by the next native text operation.
ConstSlice<NativeParagraphCluster> native_text_candidate_prepared_clusters(const TextSystem&) noexcept;
// The native format supplies scope/family/WWS properties; all glyph mapping,
// shaping and line measurement use the native font plan.
// Input and caller capacities were validated by the shared text entry point.
// Success produces an ASCII/resolved Latin paragraph, eligible mixed Latin/
// Common and overridden emoji, or a fitting homogeneous Arabic line. Emoji follows the app's
// system Segoe UI Emoji/normal weight/style policy with no added tracking or
// independent typography. Mixed items require actual font cuts, resolved LTR
// Latin/Common ordinary text and supported cluster boundaries. Shared-font
// script resolution and general fallback remain transition boundaries.
// Latin requires original-scalar coverage in one face,
// resolved LTR/en-US items and the supported newline/tab controls.
// Arabic requires an initial Arabic letter, no outer spaces,
// and only Arabic letters, recognized marks, join controls and internal spaces.
// Arabic digits, general bidi, fallback and truncating ellipsis remain on the general
// path. False leaves output alone
// for the current general layout path; no partially shaped run is retained.
// A non-null output requests shaping, otherwise measurement is returned.
bool native_text_candidate_line(const TextSystem&,Utf8View,float maximum_width,
    float maximum_height,ShapedText* output,TextMeasureResult* measure,TextWrapping wrapping=TextWrapping::wrap,TextOverflow overflow=TextOverflow::clip) noexcept;
// A retained hit request uses the editor's font-property-only run policy and
// caller-owned/disjoint hit and style-metric buffers. It requires measurement
// output and no ShapedText output; successful geometry keeps no font object.
// General multi-line truncation (ellipsis) builder over retained source clusters.
// Reuse the successful general path for the same retained shape. A dimension
// change can still fall back to the fitting/single-line path.
bool native_text_candidate_prefers_multiline(const TextSystem&,Utf8View,ConstSlice<TextStyleSpan>,TextWrapping) noexcept;
bool native_text_candidate_trimmed(const TextSystem&,Utf8View,ConstSlice<TextStyleSpan>,float width,float height,
    ShapedText*,TextWrapping,ConstSlice<TextHitRange> hits={}) noexcept;
bool native_text_candidate_styled(const TextSystem&,Utf8View,ConstSlice<TextStyleSpan>,float width,float height,
    ShapedText*,TextMeasureResult*,TextWrapping,TextOverflow=TextOverflow::clip,ConstSlice<TextHitRange> hits={},NativeParagraphHitStorage* retained=nullptr) noexcept;
}
