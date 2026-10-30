#pragma once
#include "maicord/text/font_fallback.hpp"
namespace maicord {
enum NativeFallbackClass:u8 {
    native_fallback_extending=1,native_fallback_joining=2,
    native_fallback_selector=4,native_fallback_invalid=8
};
// en-US fallback attachment, separate from grapheme/caret boundaries. Extending
// characters may keep a preceding font if it covers them; the joining bit
// independently lets the next scalar keep that font. Selectors keep a preceding
// font without nominal coverage. Within one policy a missing choice ends when the
// family covers a selector; a selector crossing policy boundaries stays with the
// preceding choice. Routed fallback uses native_font_fallback_routed_range.
u8 native_font_fallback_class(u32 scalar) noexcept;
// Opaque effective-font and mapping-interval identities. Policy distinguishes
// interval/scale boundaries even when a mapping list and font are unchanged. The caller manages
// their metadata and keeps identities stable for this query. A missing font is
// native_fallback_missing; normal missing choices still need a policy identity.
struct NativeFallbackChoice {u32 font,policy;};
using NativeFallbackSelect=SfntStatus(*)(void*,u32 scalar,NativeFallbackChoice*) noexcept;
struct NativeFallbackRangeResult {NativeFallbackChoice choice;u32 bytes,units;};
// Maps a UTF-8 prefix, stopping before a different font/policy choice.
// Selection is independent for ordinary scalars; attachment can retain the prior
// choice across routing boundaries. The coverage and selection callbacks hold all
// font state. Empty input succeeds with zero length and missing identities. The
// output is written on success only; callback side effects are the caller's
// concern. Invalid UTF-8 encountered while finding the boundary is an error; the
// caller validates any remaining input. A query does not inherit a prior range's
// font. All callbacks and the output are required.
SfntStatus native_font_fallback_range(Utf8View,NativeFallbackSelect,NativeFallbackCoverage,void*,NativeFallbackRangeResult*) noexcept;
using NativeFallbackIndexSelect=SfntStatus(*)(void*,u32 scalar,u32* index) noexcept;
// Fallback routing and in-family coverage are independent constraints. Bind
// select_font to the initial route's fixed family preference list before calling.
// select_route returns stable interval identities, including unmapped gaps, and must
// not change the family binding. select_font returns a preferred covering
// effective-font identity or native_fallback_missing; coverage uses that identity.
// Routing has extra interval boundaries at U+E000 and U+F900. One attached selector
// shares its preceding token's first-scalar routing state; consecutive selectors pair
// from the left. Font preference still processes each scalar independently, as in
// the walker above, which also defines the empty/error, ownership and UTF-8
// contracts. A coverage result needed by both constraints is queried once per scalar.
SfntStatus native_font_fallback_routed_range(Utf8View,NativeFallbackIndexSelect select_route,
    NativeFallbackIndexSelect select_font,NativeFallbackCoverage,void*,NativeFallbackRangeResult*) noexcept;
// Coordinates a requested base family with a fallback route's fixed target list
// for a nonempty prefix. A covering initial base choice uses in-family extent and
// bypasses routing. Otherwise routed fallback gives the base family first
// preference on ordinary scalars; the selected fallback font can still keep
// attached text. All font identities share the coverage callback's namespace. An
// uncovered base family selects native_fallback_missing. Bind fallback selection
// to the initial scalar's route before calling. Base results have
// policy=native_fallback_missing; fallback keeps route IDs. Error/empty contracts
// match the walkers.
SfntStatus native_font_fallback_map(Utf8View,NativeFallbackIndexSelect select_base,
    NativeFallbackIndexSelect select_route,NativeFallbackIndexSelect select_fallback,
    NativeFallbackCoverage,void*,NativeFallbackRangeResult*) noexcept;
static_assert(sizeof(NativeFallbackChoice)==8&&sizeof(NativeFallbackRangeResult)==16);
}
