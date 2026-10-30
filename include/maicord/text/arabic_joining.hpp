#pragma once
#include "maicord/text/unicode.hpp"

namespace maicord {
enum class ArabicJoiningStatus : u8 {ok,invalid_input,output_too_small,work_limit};
enum class ArabicJoiningForm : u8 {none=0,isolated=1,final=2,medial=4,initial=8};
// Arabic joining-form eligibility in logical scalar order, before substitutions.
// The caller supplies a complete joining context, including transparent marks
// and join controls; a font/style boundary is not necessarily a joining boundary.
// Nonjoining, transparent and join-causing characters receive none, though
// join-causing ones still affect their neighbours. No normalization or control
// removal.
// Writes one byte at i*stride; other bytes are untouched, so existing glyph
// metadata can hold the forms. The output span must not overlap scalars.
// Invalid arguments, short storage and insufficient work (2*scalar_count units)
// leave output unchanged. The scalars must be valid Unicode scalar values.
// reverse scans the joining context backward for forced-LTR Arabic while keeping
// output indexed by original scalar; ZWJ then also carries form eligibility
// before the later control-suppression pass.
ArabicJoiningStatus arabic_joining_forms(ConstSlice<u32> scalars,Slice<u8> output,
                                       u32 stride=1,u64 work_limit=1000000,bool reverse=false) noexcept;
} // namespace maicord
