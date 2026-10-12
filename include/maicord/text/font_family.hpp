#pragma once
#include "maicord/text/font_candidates.hpp"
namespace maicord {
struct NativeFontFamily {NativeFontProperties properties;u32 units;};
// Resolve the English WWS family and properties from font metadata. The caller
// provides mutable UTF-16 scratch/output; no terminator is added. Result fields
// are written only on success; scratch may change on error. Inputs/output are
// disjoint. Numeric/abbreviated legacy face-name schemes are not implemented.
SfntStatus native_font_family(const SfntFace&,Slice<u16> name,NativeFontFamily*,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
// Prepare file-backed candidates with the resolved WWS properties. Mapping and
// family-name storage stay with the caller; no OS service or allocation.
SfntStatus native_font_family_candidates(const SfntFace&,Slice<u16> name,NativeFontFamily*,NativeFontCandidates*,u32 work_limit=1000000) noexcept;
}
