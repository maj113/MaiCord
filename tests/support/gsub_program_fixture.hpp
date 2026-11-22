#pragma once
// Builds a dense GSUB directory by preparing every lookup eagerly through
// sfnt_gsub_prepare_lookup, the per-lookup entry shaping defers to. Payloads
// are packed into the caller's storage in lookup order.
#include "maicord/text/opentype/gsub_execute.hpp"
namespace maicord {
struct GsubProgramStorage {Slice<SfntGsubExecutableLookup> lookups;Slice<SfntGsubExecutableSubtable> subtables;Slice<SfntCoverage> coverages;Slice<SfntClassDef> classes;};
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
// Plain glyph records (4 bytes each) run as shaping runs them, in the context sfnt_gsub_execute_lookup requires: one
// source unit and one component node each, a cluster map of one cluster per unit and a component forest.
template<u32 N> struct GsubRecords {
    SfntComponentGlyph glyphs[N];SfntComponentNode nodes[N];SfntComponentUndo journal[N];u64 bits[N/64+2];
    SfntClusterMap clusters;SfntComponentForest forest;SfntGlyphBuffer buffer;
    // n records from input with room for capacity, the buffer's gap at split.
    bool open(const void* input,u32 n,u32 capacity,u32 split) noexcept {
        if(n>capacity||capacity>N)return false;
        for(u32 i=0;i<n;++i){glyphs[i]={};copy_bytes(&glyphs[i].glyph,static_cast<const u8*>(input)+i*4,4);glyphs[i].source=i;}
        return sfnt_cluster_map_open(n,{},{bits,N/64+2},&clusters)==SfntStatus::ok&&sfnt_components_open({glyphs,n},{nodes,N},&forest)==SfntStatus::ok&&
            sfnt_glyph_buffer_open({reinterpret_cast<u8*>(glyphs),capacity*static_cast<u32>(sizeof(SfntComponentGlyph))},sizeof(SfntComponentGlyph),n,&buffer)==SfntStatus::ok&&
            sfnt_glyph_buffer_seek(&buffer,split)==SfntStatus::ok;
    }
    bool open(const void* input,u32 n,u32 capacity) noexcept {return open(input,n,capacity,n);}
    SfntStatus execute(const SfntGsubProgram& program,u16 lookup,u32 start,u32 end,u32 alternate,SfntGsubExecutionScratch scratch,
        SfntGsubExecutionLimits limits,SfntGsubExecutionResult* result,u8 mask=0,ConstSlice<u32> segments={}) noexcept {
        scratch.component_journal={journal,N};return sfnt_gsub_execute_lookup(program,lookup,&buffer,start,end,alternate,scratch,limits,result,&clusters,&forest,mask,segments);
    }
    // Whether the first n stored records hold the glyphs of input.
    bool holds(const void* input,u32 n) const noexcept {
        for(u32 i=0;i<n;++i)if(!bytes_equal(&glyphs[i].glyph,static_cast<const u8*>(input)+i*4,4))return false;
        return true;
    }
    u32 count() const noexcept {return sfnt_glyph_count(sfnt_glyph_buffer_view(buffer));}
    u16 glyph(u32 i) const noexcept {return sfnt_glyph_at(sfnt_glyph_buffer_view(buffer),i).glyph;}
};
}
