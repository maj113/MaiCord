#include "maicord/text/devanagari_forms.hpp"
namespace maicord {
SfntStatus sfnt_devanagari_probe_form(const SfntGsubProgram& program,ConstSlice<SfntLookupStep> steps,
    u16 consonant,u16 halant,SfntDevanagariForm form,Slice<SfntComponentGlyph> glyphs,
    SfntGsubExecutionScratch scratch,SfntDevanagariFormResult* result,SfntGsubExecutionLimits limits) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};bool reverse=false;
    switch(form){case sfnt_deva_reph:case sfnt_deva_half:break;
        case sfnt_deva_pre:case sfnt_deva_below:case sfnt_deva_post:reverse=true;break;default:return SfntStatus::invalid_input;}
    if((steps.count&&!steps.data)||(glyphs.count&&!glyphs.data)||!program.glyph_limit||program.glyph_limit>65536||
       consonant>=program.glyph_limit||halant>=program.glyph_limit||
       (program.lookups.count&&!sfnt_gsub_directory_valid(program)))return SfntStatus::invalid_input;
    if(glyphs.count<2||glyphs.count>0xffffffffu/sizeof(SfntComponentGlyph))return SfntStatus::output_too_small;
    u64 work=limits.operations;
    const auto spend=[&](u64 count) noexcept {if(count>work)return false;work-=count;result->operations+=count;return true;};
    if(!spend(steps.count))return SfntStatus::resource_limit;
    for(u32 i=0;i<steps.count;++i)if(steps[i].lookup>=program.lookups.count||steps[i].stage||steps[i].feature_mask||
        (i&&steps[i-1].lookup>=steps[i].lookup))return SfntStatus::invalid_input;
    if(!steps.count)return SfntStatus::ok;
    // Fixed cost of the two-source setup (map initialization and component open).
    if(!spend(16))return SfntStatus::resource_limit;
    const u16 pair[]{reverse?halant:consonant,reverse?consonant:halant};
    for(u32 i=0;i<2;++i)glyphs[i]={{pair[i],sfnt_gdef_glyph_class(program.gdef,pair[i]),0},i,0,0};
    SfntComponentNode nodes[2]{};SfntComponentForest forest{};u64 bits=0;const u32 boundary=0;SfntClusterMap clusters{};SfntGlyphBuffer buffer{};
    auto status=sfnt_components_open({glyphs.data,2},{nodes,2},&forest,16);if(status!=SfntStatus::ok)return status;
    status=sfnt_cluster_map_open(2,{&boundary,1},{&bits,1},&clusters,16);if(status!=SfntStatus::ok)return status;
    status=sfnt_glyph_buffer_open({reinterpret_cast<u8*>(glyphs.data),glyphs.count*static_cast<u32>(sizeof(SfntComponentGlyph))},sizeof(SfntComponentGlyph),2,&buffer);
    if(status!=SfntStatus::ok)return status;
    bool refresh=false;
    for(u32 i=0;i<steps.count;++i){
        const auto view=sfnt_glyph_buffer_view(buffer);const u32 count=sfnt_glyph_count(view);
        if(refresh){if(!spend(count))return SfntStatus::resource_limit;
            for(u32 j=0;j<count;++j){auto& glyph=const_cast<SfntGsubGlyph&>(sfnt_glyph_at(view,j));glyph.glyph_class=sfnt_gdef_glyph_class(program.gdef,glyph.glyph);}}
        SfntGsubExecutionResult execution{};
        status=sfnt_gsub_execute_lookup(program,steps[i].lookup,&buffer,0,count,0,scratch,{work,limits.match_work},&execution,&clusters,&forest);
        work-=execution.operations;result->operations+=execution.operations;if(status!=SfntStatus::ok)return status;refresh=execution.edits!=0;
    }
    if(!spend(1))return SfntStatus::resource_limit;
    const auto view=sfnt_glyph_buffer_view(buffer);if(sfnt_glyph_count(view)!=1)return SfntStatus::ok;
    const auto& glyph=*reinterpret_cast<const SfntComponentGlyph*>(&sfnt_glyph_at(view,0));
    u32 first=0,last=0;u64 used=0;status=sfnt_component_source_bounds(forest,glyph.component,&first,&last,work,&used);
    work-=used;result->operations+=used;if(status!=SfntStatus::ok)return status;
    if(first==0&&last==1){result->formed=true;result->glyph=glyph.glyph.glyph;}return SfntStatus::ok;
}
}
