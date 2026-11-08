#pragma once
#include "maicord/ui/text/native_text_candidate.hpp"
#include "maicord/text/layout/detail/paragraph_layout.hpp"
namespace maicord {
// Prepares or reuses the complete native shape and fits every logical line into the
// caller-owned, transient plan (no font handles or glyph arrays are copied). Widths
// and visible cluster prefixes include width/height omission decisions; hard-break
// suffixes stay in cluster_count. This feeds omission publication, not drawing:
// layout.run_count is the fitter's ordinary run view, before prefix/control/sign
// publication. The plan is the ellipsis layout's: request overflow and hits are ignored.
bool native_text_prepare_trim_plan(const TextSystem&,const NativeTextRequest&,Slice<NativeParagraphLineFit>,
    NativeParagraphLayout*) noexcept;
}
