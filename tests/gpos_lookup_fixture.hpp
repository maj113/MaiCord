#pragma once
// Prepares one GPOS lookup through sfnt_gpos_program_prepare, the path shaping
// uses: a one-lookup selection over a compact directory. The record borrows the
// caller's subtable storage; output is written only on success.
#include "maicord/text/gpos_program.hpp"
#include "maicord/core/memory.hpp"
namespace maicord {
inline SfntStatus gpos_prepare_one(const SfntLayout& layout,u16 index,const SfntGdef& gdef,u32 glyph_limit,
    Slice<SfntGposSubtable> basic,Slice<SfntGposAttachment> attachments,SfntGposExecutableLookup* output) noexcept {
    static u64 bits[1024];static const SfntGposExecutableLookup* entries[65536];
    *output={};if(index>=layout.lookup_count)return SfntStatus::invalid_input;
    const u32 words=(layout.lookup_count+63u)/64;clear_bytes(bits,words*8u);bits[index/64]=1ull<<(index%64);
    SfntGposExecutableLookup record{};SfntGposProgram program{};
    const auto status=sfnt_gpos_program_prepare(layout,gdef,glyph_limit,{{&record,1},basic,attachments,{},{},{},{entries,layout.lookup_count}},&program,1000000,1000000,{bits,words});
    if(status==SfntStatus::ok)*output=record;return status;
}
}
