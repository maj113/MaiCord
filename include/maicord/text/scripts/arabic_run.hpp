#pragma once
#include "maicord/text/scripts/arabic_run_plan.hpp"
#include "maicord/text/scripts/arabic_joining.hpp"
#include "maicord/text/scripts/ascii_run.hpp"

namespace maicord {
// Shape valid UTF-8 in one font with an already resolved Arabic script; context
// gives the joining of the text around it (see arabic_joining_context), so a run cut
// from a word keeps its forms, and paragraph_start says whether the run begins its
// paragraph (a leading mark then follows a dotted circle). Source positions and maps
// are UTF-16 units. Missing cmap entries keep glyph zero.
// The plan must stay alive during the call. Scalars borrow the component
// buffer's byte prefix and mapped glyphs the position buffer's prefix; both are
// consumed before those arrays are reused. All buffers, input and result must
// otherwise be disjoint. A failed call sets no output lengths; scratch may
// change, and the attempt can be retried from the input.
// A left-to-right run with letters is shaped as HarfBuzz does (hb_ensure_native_direction):
// its graphemes reversed and shaped right to left, then published left to right.
SfntStatus native_arabic_run(const NativeArabicPlanStore& store,NativeArabicPlanHandle plan,
    Utf8View input,bool independent_letters,bool rtl,float em_size,const SfntShapeStorage& storage,
    SfntShapeRunResult* result,SfntShapeLimits limits={},ArabicJoiningContext context={},bool paragraph_start=true) noexcept;
// Adapt the successful Arabic output to the common compact glyph stream used
// by paragraph layout. The glyph prefix can alias shaping.glyphs,
// as in native_latin_run; source maps and placements may alias their own arrays.
SfntStatus native_arabic_run(const NativeArabicPlanStore&,NativeArabicPlanHandle,
    Utf8View,bool independent,bool rtl,float em,const NativeAsciiStorage&,NativeAsciiResult*,SfntShapeLimits limits={},ArabicJoiningContext context={},bool paragraph_start=true) noexcept;

}
