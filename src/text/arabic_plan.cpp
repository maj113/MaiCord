#include "substitution_selection.hpp"
#include "maicord/text/arabic_plan.hpp"
#include "layout_language.hpp"
#include "maicord/text/base.hpp"

namespace maicord {namespace {
bool spend(u64& work,u64 amount=1) noexcept {if(amount>work)return false;work-=amount;return true;}
u32 allowance(u64 work) noexcept {return static_cast<u32>(minimum<u64>(work,0xffffffffu));}
struct FeatureChoice {u16 bit;u8 stage,mask;};
FeatureChoice choice(SfntLayoutKind kind,u32 tag,bool independent) noexcept {
    if(kind==SfntLayoutKind::substitution)switch(tag) {
        case sfnt_tag('c','c','m','p'):return {1,0,16};case sfnt_tag('l','o','c','l'):return {2,0,16};
        case sfnt_tag('i','s','o','l'):return {4,1,1};case sfnt_tag('f','i','n','a'):return {8,1,2};
        case sfnt_tag('m','e','d','i'):return {16,1,4};case sfnt_tag('i','n','i','t'):return {32,1,8};
        case sfnt_tag('r','l','i','g'):return {64,2,16};case sfnt_tag('r','c','l','t'):return {128,2,16};
        case sfnt_tag('c','a','l','t'):return {256,2,16};case sfnt_tag('l','i','g','a'):return independent?FeatureChoice{}:FeatureChoice{512,2,16};
        case sfnt_tag('m','s','e','t'):return {1024,2,16};default:return {};
    }
    switch(tag) {
        case sfnt_tag('c','u','r','s'):return {1,0,16};case sfnt_tag('d','i','s','t'):return {2,0,16};
        case sfnt_tag('k','e','r','n'):return {4,0,16};case sfnt_tag('m','a','r','k'):return {8,0,16};
        case sfnt_tag('m','k','m','k'):return {16,0,16};default:return {};
    }
}
struct SelectedFeature {SfntFeature feature;u8 stage,mask;};
struct Builder {
    const SfntLayout& layout;const SfntFeatureSubstitutions& variations;Slice<u8> scratch;Slice<SfntLookupStep> output;
    SfntLookupPlanResult& result;u64 work;SelectedFeature features[12]{};u32 count=0;
    SfntStatus add(u16 index,u8 stage,u8 mask) noexcept {
        if(count==12)return SfntStatus::resource_limit;
        u32 tag=0,used=0;auto& selected=features[count];
        const auto s=sfnt_layout_feature(layout,index,variations,&tag,&selected.feature,allowance(work),&used);work-=used;
        if(s!=SfntStatus::ok)return s;selected.stage=stage;selected.mask=mask;++count;return SfntStatus::ok;
    }
    SfntStatus pass(bool write) noexcept {
        u32 emitted=0;const u32 stages=layout.kind==SfntLayoutKind::substitution?3:1;
        for(u32 stage=0;stage<stages;++stage) {
            if(!spend(work,layout.lookup_count))return SfntStatus::resource_limit;
            for(u32 i=0;i<layout.lookup_count;++i)scratch[i]=0;
            for(u32 f=0;f<count;++f) {
                if(!spend(work))return SfntStatus::resource_limit;const auto& selected=features[f];
                if(selected.stage!=stage && selected.stage!=255)continue;
                const auto indices=selected.feature.lookup_indices;
                if(!spend(work,indices.count/2))return SfntStatus::resource_limit;
                for(u32 i=0;i<indices.count/2;++i)scratch[byte_load_be16(indices.data+i*2)]|=selected.mask;
            }
            if(!spend(work,layout.lookup_count))return SfntStatus::resource_limit;
            for(u32 i=0;i<layout.lookup_count;++i)if(scratch[i]) {
                if(write){if(!spend(work))return SfntStatus::resource_limit;output[emitted]={static_cast<u16>(i),static_cast<u8>((scratch[i]&16)?0:scratch[i]),static_cast<u8>(stage)};}
                ++emitted;
            }
        }
        if(!write)result.lookup_count=emitted;return SfntStatus::ok;
    }
    SfntStatus run(bool independent) noexcept {
        constexpr u32 scripts[]{sfnt_tag('a','r','a','b'),sfnt_tag('D','F','L','T'),sfnt_tag('l','a','t','n')};
        SfntLanguageSelection selection{};u64 remaining=work;auto s=sfnt_layout_select_language_prepared(layout,{scripts,3},sfnt_tag('E','N','G',' '),selection,remaining);work=remaining;result.script_tag=selection.script_tag;
        if(s==SfntStatus::missing_table)return SfntStatus::ok;if(s!=SfntStatus::ok)return s;
        result.language_found=true;result.language_tag=selection.language_tag;const auto& selected=selection.language;
        if(scratch.count<layout.lookup_count)return SfntStatus::output_too_small;
        if(selected.required_feature!=0xffff){s=add(selected.required_feature,255,16);if(s!=SfntStatus::ok)return s;}
        u16 seen=0;
        for(u32 i=0;i<selected.feature_indices.count/2;++i) {
            if(!spend(work))return SfntStatus::resource_limit;const u16 f=byte_load_be16(selected.feature_indices.data+i*2);
            const auto c=choice(layout.kind,byte_load_be32(layout.features.data+2+static_cast<u32>(f)*6),independent);
            if(!c.bit || (seen&c.bit))continue;seen|=c.bit;if(f==selected.required_feature)continue;
            s=add(f,c.stage,c.mask);if(s!=SfntStatus::ok)return s;
        }
        const u64 before=work;s=pass(false);if(s!=SfntStatus::ok)return s;
        if(output.count<result.lookup_count)return SfntStatus::output_too_small;
        if(before-work+result.lookup_count>work)return SfntStatus::resource_limit;
        return pass(true);
    }
};
}
SfntStatus sfnt_arabic_lookup_plan(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,bool independent,
    Slice<u8> scratch,Slice<SfntLookupStep> output,SfntLookupPlanResult* result,u64 work) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    if(!layout.table.data || static_cast<u8>(layout.kind)>1 || (scratch.count && !scratch.data) || (output.count && !output.data))return SfntStatus::invalid_input;
    Builder builder{layout,variations,scratch,output,*result,work};const auto s=builder.run(independent);result->operations=work-builder.work;return s;
}
bool arabic_substitution_feature(u32 tag) noexcept {return choice(SfntLayoutKind::substitution,tag,false).bit!=0;}
} // namespace maicord
