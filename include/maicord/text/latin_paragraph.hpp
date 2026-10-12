#pragma once
#include "maicord/text/ascii_paragraph.hpp"
namespace maicord {
struct NativeAlphabeticPlan;
// Transient, disjoint scalar and boundary arrays. The caller may reuse storage
// reserved for layout runs: analysis completes before those runs are produced.
// With deferred=true, paragraph frontends return collected clusters without
// line/space classification; the coordinator must analyze the complete source
// before fitting, committing or hit testing.
struct NativeLatinBoundaryStorage {
    u32* scalar_data;u32 scalar_count;bool deferred;Slice<u8> breaks;
    constexpr NativeLatinBoundaryStorage(Slice<u32> scalars={},Slice<u8> flags={},bool defer=false) noexcept
        :scalar_data(scalars.data),scalar_count(scalars.count),deferred(defer),breaks(flags) {}
};
// The policy is stored in padding, so no additional transient storage is needed.
static_assert(sizeof(NativeLatinBoundaryStorage)==32);
// Apply the en-US line policy at complete shaping-cluster boundaries; an explicit
// call runs even when the storage is deferred. Source coordinates remain UTF-16.
// Input must already be itemized as a supported alphabetic script.
SfntStatus native_latin_paragraph_boundaries(Utf8View,Slice<NativeParagraphCluster>,NativeLatinBoundaryStorage) noexcept;
// Single-font frontend with TAB, CR/LF, NEL and Unicode line/paragraph separators.
// Printable segments follow native_latin_line, including its original-source
// coverage requirement. All arrays are caller-owned; outputs are committed only
// on success. Optional alphabetic profile extends the frontend to an already
// resolved Greek/Cyrillic/Hebrew item; script boundaries and direction belong
// to the caller, and RTL shaping does no paragraph bidi or visual fitting.
// Optional nominal is true only when every segment took nominal shaping and the
// input had no TAB, newline or nonvisual items; it is false on failure.
// Whitespace is represented normally; the consumer decides whether a shortcut
// applies.
SfntStatus native_latin_paragraph_shape(const NativeRunPlanStore&,NativeRunPlanHandle,Utf8View,bool independent,float em,
    const NativeAsciiParagraphStorage&,NativeLatinBoundaryStorage,NativeParagraphShape*,bool* nominal=nullptr,float trailing_spacing=0,const NativeAlphabeticPlan* profile=nullptr,bool force_shaping=false,bool rtl=false) noexcept;
}
