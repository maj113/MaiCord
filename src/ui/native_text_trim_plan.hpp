#pragma once
#include "maicord/ui/native_text_candidate.hpp"
#include "../text/paragraph_layout.hpp"
namespace maicord {
// Prepare/reuse the complete native shape and fit every logical line. The
// caller-owned plan is transient; no font handles or glyph arrays are copied.
// Widths and visible cluster prefixes include width/height omission decisions.
// Hard-break suffixes stay represented in cluster_count. This is input to
// omission publication, not a trimmed drawing result. layout.run_count describes
// the fitter's ordinary run view, before prefix/control/sign publication.
bool native_text_prepare_trim_plan(const TextSystem&,Utf8View,ConstSlice<TextStyleSpan>,
    float width,float height,TextWrapping,Slice<NativeParagraphLineFit>,NativeParagraphLayout*) noexcept;
}
