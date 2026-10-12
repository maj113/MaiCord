#pragma once
#include "maicord/text/font_fallback.hpp"
#include "maicord/core/utf.hpp"
namespace maicord {
enum NativeFallbackClass:u8 {
    native_fallback_extending=1,native_fallback_joining=2,
    native_fallback_selector=4,native_fallback_invalid=8
};
// en-US fallback attachment, separate from grapheme/caret boundaries.
// Extending characters may keep a preceding font if it covers them. The
// joining bit independently lets the next scalar keep that font.
// Selectors keep a preceding font without nominal coverage. Within one policy,
// a missing choice ends when the family covers a selector. A selector crossing
// policy boundaries stays with the preceding choice; routed fallback uses the
// separate function below and a family list bound to the initial route.
u8 native_font_fallback_class(u32 scalar) noexcept;
// Opaque effective-font and mapping-interval identities. Policy distinguishes
// interval/scale boundaries even when a mapping list and font are unchanged. The caller manages
// their metadata and keeps identities stable for this query. A missing font is
// native_fallback_missing; normal missing choices still need a policy identity.
struct NativeFallbackChoice {u32 font,policy;};
using NativeFallbackSelect=SfntStatus(*)(void*,u32 scalar,NativeFallbackChoice*) noexcept;
struct NativeFallbackRangeResult {NativeFallbackChoice choice;u32 bytes,units;};
// Map a nonempty UTF-8 prefix, stopping before a different font/policy choice.
// Selection is independent for ordinary scalars. Attachment can retain the prior
// choice across routing boundaries. The coverage and selection callbacks hold all
// font state; this does no allocation, IO or glyph shaping and keeps no decoded-text buffer.
// Empty input succeeds with zero length and missing identities. Failure clears
// output; callback side effects are the caller's concern. All arguments are disjoint.
// Budget counts decoded scalars and callback attempts, not callback internals.
// Invalid UTF-8 encountered while finding the boundary is an error. The caller
// validates any remaining input. One query does not inherit a prior range's font.
SfntStatus native_font_fallback_range(Utf8View,NativeFallbackSelect,NativeFallbackCoverage,void*,
    NativeFallbackRangeResult*,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
using NativeFallbackIndexSelect=SfntStatus(*)(void*,u32 scalar,u32* index) noexcept;
// Fallback routing and in-family coverage are independent constraints. Bind
// select_font to the initial route's fixed family preference list before calling.
// select_route returns stable interval identities, including unmapped gaps;
// it must not change the family binding. select_font returns a preferred covering
// effective-font identity or native_fallback_missing. Coverage uses that identity.
// Routing has additional interval boundaries at U+E000 and U+F900. One attached
// selector shares its preceding token's first scalar routing state; consecutive
// selectors pair from the left. Font preference still
// processes each scalar independently, as in the in-family walker above.
// This does not supply a system policy, initial family binding, scale or refresh.
// Empty/error, ownership, UTF-8 and work-budget contracts follow the walker above.
// A coverage result needed by both constraints is queried only once per scalar.
SfntStatus native_font_fallback_routed_range(Utf8View,NativeFallbackIndexSelect select_route,
    NativeFallbackIndexSelect select_font,NativeFallbackCoverage,void*,NativeFallbackRangeResult*,
    u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
// Coordinate a requested base family with a fallback route's fixed target list.
// A covering initial base choice uses in-family extent and bypasses routing.
// Otherwise routed fallback gives the base family first preference on ordinary
// scalars; the selected fallback font can still keep attached text. All font
// identities share the coverage callback's namespace. A missing base family can
// use a null base callback. Bind fallback selection to the initial scalar's route
// before calling; no system mapping data, catalog ownership or scale is supplied.
// Base results have policy=native_fallback_missing; fallback keeps route IDs.
// No allocation or state kept between calls. Error/empty/budget contracts match the walkers.
SfntStatus native_font_fallback_map(Utf8View,NativeFallbackIndexSelect select_base,
    NativeFallbackIndexSelect select_route,NativeFallbackIndexSelect select_fallback,
    NativeFallbackCoverage,void*,NativeFallbackRangeResult*,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
static_assert(sizeof(NativeFallbackChoice)==8&&sizeof(NativeFallbackRangeResult)==16);
}
