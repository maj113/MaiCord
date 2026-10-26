#pragma once
#include "maicord/text/arabic_run_plan.hpp"
#include "maicord/text/ascii_run.hpp"

namespace maicord {
// Shape valid UTF-8 in one font with an already resolved Arabic script and
// complete joining context; itemization, fallback, and font or style splits
// that would lose joining context belong to the caller. Source positions and
// maps are UTF-16 units. Missing cmap entries keep glyph zero.
// The plan must stay alive during the call. Scalars borrow the component
// buffer's byte prefix and mapped glyphs the position buffer's prefix; both are
// consumed before those arrays are reused. All buffers, input and result must
// otherwise be disjoint. A failed call sets no output lengths; scratch may
// change, and the attempt can be retried from the input.
// The forced-LTR path supports backward joining and marks; CGJ and some mixed
// control/mark cases are not handled.
SfntStatus native_arabic_run(const NativeArabicPlanStore& store,NativeArabicPlanHandle plan,
    Utf8View input,bool independent_letters,bool rtl,float em_size,const SfntShapeStorage& storage,
    SfntShapeRunResult* result,SfntShapeLimits limits={}) noexcept;
// Adapt the successful Arabic output to the common compact glyph stream used
// by paragraph layout. The glyph prefix can alias shaping.glyphs,
// as in native_latin_run; source maps and placements may alias their own arrays.
SfntStatus native_arabic_run(const NativeArabicPlanStore&,NativeArabicPlanHandle,
    Utf8View,bool independent,bool rtl,float em,const NativeAsciiStorage&,NativeAsciiResult*,SfntShapeLimits limits={}) noexcept;

}
