#pragma once
// Builds a dense GSUB directory by preparing every lookup eagerly through
// sfnt_gsub_prepare_lookup, the per-lookup entry shaping defers to. Payloads
// are packed into the caller's storage in lookup order.
#include "maicord/text/gsub_execute.hpp"
namespace maicord {
struct GsubProgramStorage {Slice<SfntGsubExecutableLookup> lookups;Slice<SfntGsubExecutableSubtable> subtables;Slice<SfntCoverage> coverages;Slice<SfntClassDef> classes;};
inline SfntStatus gsub_program_needs(const SfntLayout& layout,SfntGsubProgramRequirements* output) noexcept {
    *output={};SfntGsubProgramRequirements total{layout.lookup_count,0,0,0};
    for(u32 i=0;i<layout.lookup_count;++i){SfntGsubProgramRequirements need{};const auto status=sfnt_gsub_lookup_requirements(layout,static_cast<u16>(i),&need);if(status!=SfntStatus::ok)return status;
        total.subtables+=need.subtables;total.coverages+=need.coverages;total.classes+=need.classes;}
    *output=total;return SfntStatus::ok;
}
inline SfntStatus prepare_gsub_program(const SfntLayout& layout,const SfntGdef& gdef,u32 glyph_limit,GsubProgramStorage storage,SfntGsubProgram* output,
    bool ignore_empty_sequences=false,bool ignore_unreachable_single_substitutions=false,u32 payload_work_limit=1000000) noexcept {
    *output={};if(layout.lookup_count>storage.lookups.count)return SfntStatus::output_too_small;u32 subtables=0,coverages=0,classes=0;
    for(u32 i=0;i<layout.lookup_count;++i){SfntGsubProgramRequirements need{};auto status=sfnt_gsub_lookup_requirements(layout,static_cast<u16>(i),&need);if(status!=SfntStatus::ok)return status;
        if(need.subtables>storage.subtables.count-subtables||need.coverages>storage.coverages.count-coverages||need.classes>storage.classes.count-classes)return SfntStatus::output_too_small;
        const SfntGsubLookupStorage slot{{storage.subtables.data+subtables,need.subtables},{storage.coverages.data+coverages,need.coverages},{storage.classes.data+classes,need.classes}};
        status=sfnt_gsub_prepare_lookup(layout,static_cast<u16>(i),gdef,glyph_limit,slot,&storage.lookups.data[i],payload_work_limit,1000000,ignore_empty_sequences,ignore_unreachable_single_substitutions);
        if(status!=SfntStatus::ok)return status;subtables+=need.subtables;coverages+=need.coverages;classes+=need.classes;}
    *output={{storage.lookups.data,layout.lookup_count},gdef,glyph_limit};return SfntStatus::ok;
}
}
