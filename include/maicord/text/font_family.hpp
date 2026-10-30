#pragma once
#include "maicord/text/font_candidates.hpp"
namespace maicord {
struct NativeFontFamily {NativeFontProperties properties;u32 units;};
// Resolve the English WWS family and properties from font metadata. The caller
// provides mutable UTF-16 scratch/output; no terminator is added. Result fields
// are written only on success; scratch may change on error. Inputs/output are
// disjoint. Numeric/abbreviated legacy face-name schemes are not implemented.
// The work is linear in the name table plus a bounded number of passes over the
// name, which output.count bounds.
SfntStatus native_font_family(const SfntFace&,Slice<u16> name,NativeFontFamily*) noexcept;
// Prepare file-backed candidates with the resolved WWS properties. Mapping and
// family-name storage stay with the caller; no OS service or allocation.
SfntStatus native_font_family_candidates(const SfntFace&,Slice<u16> name,NativeFontFamily*,NativeFontCandidates*) noexcept;
}
