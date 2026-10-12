#pragma once
#include "maicord/text/arabic_run_plan.hpp"
#include "maicord/text/ascii_run.hpp"

namespace maicord {
// Shape valid UTF-8 in one selected font with an already resolved Arabic script
// and complete joining context. Font/style splits must not discard joining
// context; general itemization/fallback and partial-context runs belong to the
// caller. This is not a general multilingual or paragraph entry point.
// Output source positions/maps use UTF-16 units, including surrogate pairs.
// Missing cmap entries keep glyph zero; choosing a fallback face is separate.
// Keep the plan alive during the call. No allocation, I/O or OS shaping call.
// Scalars borrow the component buffer's byte prefix; mapped glyphs borrow the
// position buffer's prefix. They are consumed before those arrays are reused.
// Grapheme starts borrow targets; Arabic preparation can refine those clusters.
// No extra per-character arrays are required. All provided buffers, input and
// result must otherwise be disjoint. A failed call sets no output lengths;
// scratch bytes can change and the whole attempt can be retried from the input.
// The forced-LTR path supports backward joining and marks; CGJ and some mixed
// control/mark cases are not handled. This adapter does not perform fallback.
SfntStatus native_arabic_run(const NativeArabicPlanStore& store,NativeArabicPlanHandle plan,
    Utf8View input,bool independent_letters,bool rtl,float em_size,SfntShapeStorage storage,
    SfntShapeRunResult* result,SfntShapeLimits limits={}) noexcept;
// Adapt the successful Arabic output to the common compact glyph stream used
// by paragraph layout. The glyph prefix can alias shaping.glyphs,
// as in native_latin_run; source maps and placements may alias their own arrays.
SfntStatus native_arabic_run(const NativeArabicPlanStore&,NativeArabicPlanHandle,
    Utf8View,bool independent,bool rtl,float em,const NativeAsciiStorage&,NativeAsciiResult*,SfntShapeLimits limits={}) noexcept;

}
