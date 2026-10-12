#pragma once
#include "maicord/text/ascii_paragraph.hpp"
namespace maicord {
struct NativeAlphabeticPlan;
// Transient, disjoint scalar and boundary arrays. The caller may reuse storage
// reserved for layout runs: analysis completes before those runs are produced.
struct NativeLatinBoundaryStorage {
    u32* scalar_data;u32 scalar_count;bool deferred;Slice<u8> breaks;
    constexpr NativeLatinBoundaryStorage(Slice<u32> scalars={},Slice<u8> flags={},bool defer=false) noexcept
        :scalar_data(scalars.data),scalar_count(scalars.count),deferred(defer),breaks(flags) {}
};
// The policy is stored in padding, so no additional transient storage is needed.
static_assert(sizeof(NativeLatinBoundaryStorage)==32);
// With deferred=true, paragraph frontends return collected clusters without
// line/space classification. The coordinator must analyze the complete source
// before fitting, committing or hit testing. Boundary calls requested explicitly always run.
// Apply the en-US line policy at complete shaping-cluster boundaries.
// Source coordinates remain UTF-16. Input must already be itemized as a
// supported alphabetic script; dictionary and variation analysis are separate.
SfntStatus native_latin_paragraph_boundaries(Utf8View,Slice<NativeParagraphCluster>,NativeLatinBoundaryStorage) noexcept;
// Single-font frontend with TAB, CR/LF, NEL and Unicode line/paragraph separators.
// Printable segments preserve native_latin_line's original-source coverage and
// All arrays are caller-owned; outputs are committed only on success.
// Optional alphabetic profile extends the printable frontend to an already
// resolved Greek/Cyrillic/Hebrew item; script boundaries and direction belong
// to the caller. RTL shaping does not perform paragraph bidi or visual fitting.
// Optional nominal reports whether every segment took nominal shaping and the
// input had no TAB/newline controls. It is false on failure; whitespace is still
// represented normally and the consumer decides whether a shortcut applies.
SfntStatus native_latin_paragraph_shape(const NativeRunPlanStore&,NativeRunPlanHandle,Utf8View,bool independent,float em,
    const NativeAsciiParagraphStorage&,NativeLatinBoundaryStorage,NativeParagraphShape*,bool* nominal=nullptr,float trailing_spacing=0,const NativeAlphabeticPlan* profile=nullptr,bool force_shaping=false,bool rtl=false) noexcept;
}
