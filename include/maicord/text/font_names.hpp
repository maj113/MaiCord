#pragma once
#include "maicord/text/sfnt.hpp"
namespace maicord {
// Read-only views over a mapped name table; keep the mapping alive. Opening
// validates record/string spans, not string encoding. Unsorted records are
// accepted without trusting binary-search hints. No allocation or OS services.
// SfntNames must come from a successful open and must not be modified.
struct SfntNames {ConstSlice<u8> table;u16 records,storage,language_tags,version;};
struct SfntName {
    ConstSlice<u8> bytes,language_tag;
    u16 platform,encoding,language,id;
};
struct SfntFontAttributes {u16 weight,width,selection,version;};
SfntStatus sfnt_names_open(ConstSlice<u8> table,SfntNames* output) noexcept;
SfntStatus sfnt_name_record(const SfntNames&,u32 index,SfntName* output) noexcept;
// English metadata policy: en-US, other English, Unicode-neutral, then first
// supported language. Windows wins a platform tie; record order wins remaining
// ties. Unknown version-1 language-tag references are ignored. This chooses a
// name record, not a WWS family or font face. Missing names return ok/found=false.
SfntStatus sfnt_name_english(const SfntNames&,u16 id,SfntName* output,bool* found) noexcept;
// Windows Unicode/symbol and Unicode-platform strings are UTF-16BE. Decode to
// caller-owned UTF-16 units, without a terminator, preserving supplementary
// pairs. Other encodings return unsupported. Source/output must be
// disjoint. Validate the complete encoding and capacity before writing output.
SfntStatus sfnt_name_decode(const SfntName&,Slice<u16> output,u32* units) noexcept;
// Raw default-instance OS/2 fields. This does not infer family grouping,
// synthesized styles, named variable instances or fallback selection.
SfntStatus sfnt_font_attributes(const SfntFace&,SfntFontAttributes* output) noexcept;
static_assert(sizeof(SfntNames)==24);
static_assert(sizeof(SfntFontAttributes)==8);
}
