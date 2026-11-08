#include "maicord/text/scripts/arabic_plan.hpp"
#include "maicord/core/memory.hpp"
#include "../opentype/layout_language.hpp"
#include "maicord/text/base.hpp"

namespace maicord {namespace {
u32 allowance(u64 work) noexcept {return static_cast<u32>(minimum<u64>(work,0xffffffffu));}
struct FeatureChoice {u32 bit;u8 stage,mask;};
// HarfBuzz's right-to-left Arabic features (hb_ot_shape_collect_features with collect_features_arabic), by GSUB
// stage: rvrn | rtla rand trak rtlm | ccmp locl | isol | fina | medi | init | rlig | calt | rclt liga clig mset abvm blwm
// mark mkmk curs dist kern; calt joins the last stage when the language system has rclt (has_feature, HarfBuzz
// issue 1573). GPOS applies the global ones in one stage; positioning by joining form is not supported. The forms
// are masked, the rest global; independent letters disable liga, clig and kern.
constexpr u32 gsub_stages=10;
FeatureChoice choice(SfntLayoutKind kind,u32 tag,bool independent,bool rclt) noexcept {
    const bool gsub=kind==SfntLayoutKind::substitution;
    const auto pick=[gsub](u32 bit,u8 stage,u8 mask=16) noexcept {return gsub||mask==16?FeatureChoice{1u<<bit,static_cast<u8>(gsub?stage:0),mask}:FeatureChoice{};};
    switch(tag) {
        case sfnt_tag('r','v','r','n'):return pick(0,0);
        case sfnt_tag('r','t','l','a'):return pick(1,1);case sfnt_tag('r','a','n','d'):return pick(2,1);case sfnt_tag('t','r','a','k'):return pick(3,1);
        case sfnt_tag('r','t','l','m'):return pick(23,1,sfnt_arabic_rtlm_mask);
        case sfnt_tag('c','c','m','p'):return pick(4,2);case sfnt_tag('l','o','c','l'):return pick(5,2);
        case sfnt_tag('i','s','o','l'):return pick(6,3,1);case sfnt_tag('f','i','n','a'):return pick(7,4,2);
        case sfnt_tag('m','e','d','i'):return pick(8,5,4);case sfnt_tag('i','n','i','t'):return pick(9,6,8);
        case sfnt_tag('r','l','i','g'):return pick(10,7);case sfnt_tag('c','a','l','t'):return pick(11,rclt?9:8);
        case sfnt_tag('r','c','l','t'):return pick(12,9);case sfnt_tag('m','s','e','t'):return pick(13,9);
        case sfnt_tag('a','b','v','m'):return pick(14,9);case sfnt_tag('b','l','w','m'):return pick(15,9);
        case sfnt_tag('m','a','r','k'):return pick(16,9);case sfnt_tag('m','k','m','k'):return pick(17,9);
        case sfnt_tag('c','u','r','s'):return pick(18,9);case sfnt_tag('d','i','s','t'):return pick(19,9);
        case sfnt_tag('l','i','g','a'):return independent?FeatureChoice{}:pick(20,9);
        case sfnt_tag('c','l','i','g'):return independent?FeatureChoice{}:pick(21,9);
        case sfnt_tag('k','e','r','n'):return independent?FeatureChoice{}:pick(22,9);
        default:return {};
    }
}
struct SelectedFeature {SfntFeature feature;u8 stage,mask;};
struct Builder {
    const SfntLayout& layout;const SfntFeatureSubstitutions& variations;Slice<u8> scratch;Slice<SfntLookupStep> output;
    SfntLookupPlanResult& result;u64 work;SelectedFeature features[24]{};u32 count=0;
    SfntStatus add(u16 index,u8 stage,u8 mask) noexcept {
        if(count==24)return SfntStatus::resource_limit;
        u32 tag=0,used=0;auto& selected=features[count];
        const auto s=sfnt_layout_feature(layout,index,variations,&tag,&selected.feature,allowance(work),&used);work-=used;
        if(s!=SfntStatus::ok)return s;selected.stage=stage;selected.mask=mask;++count;return SfntStatus::ok;
    }
    SfntStatus pass(bool write) noexcept {
        u32 emitted=0;const u32 stages=layout.kind==SfntLayoutKind::substitution?gsub_stages:1;
        for(u32 stage=0;stage<stages;++stage) {
            if(!spend(&work,layout.lookup_count))return SfntStatus::resource_limit;
            for(u32 i=0;i<layout.lookup_count;++i)scratch[i]=0;
            for(u32 f=0;f<count;++f) {
                if(!spend(&work))return SfntStatus::resource_limit;const auto& selected=features[f];
                if(selected.stage!=stage)continue;
                const auto indices=selected.feature.lookup_indices;
                if(!spend(&work,indices.count/2))return SfntStatus::resource_limit;
                for(u32 i=0;i<indices.count/2;++i)scratch[byte_load_be16(indices.data+i*2)]|=selected.mask;
            }
            if(!spend(&work,layout.lookup_count))return SfntStatus::resource_limit;
            // Published phases: 0 before the joining forms, 1 the forms, 2 after them.
            const u8 phase=static_cast<u8>(stage<3?0:stage<7?1:2);
            for(u32 i=0;i<layout.lookup_count;++i)if(scratch[i]) {
                if(write){if(!spend(&work))return SfntStatus::resource_limit;output[emitted]={static_cast<u16>(i),static_cast<u8>((scratch[i]&16)?0:scratch[i]),phase};}
                ++emitted;
            }
        }
        if(!write)result.lookup_count=emitted;return SfntStatus::ok;
    }
    SfntStatus run(bool independent) noexcept {
        static constexpr u32 scripts[]{sfnt_tag('a','r','a','b'),sfnt_tag('D','F','L','T'),sfnt_tag('d','f','l','t'),sfnt_tag('l','a','t','n')};
        SfntLanguageSelection selection{};u64 remaining=work;auto s=sfnt_layout_select_language_prepared(layout,{scripts,4},sfnt_tag('E','N','G',' '),selection,remaining);work=remaining;result.script_tag=selection.script_tag;
        if(s==SfntStatus::missing_table)return SfntStatus::ok;if(s!=SfntStatus::ok)return s;
        result.language_found=true;result.language_tag=selection.language_tag;const auto& selected=selection.language;
        if(scratch.count<layout.lookup_count)return SfntStatus::output_too_small;
        const auto tag_of=[&](u16 f) noexcept {return byte_load_be32(layout.features.data+2+static_cast<u32>(f)*6);};
        bool rclt=false;
        if(!spend(&work,selected.feature_indices.count/2))return SfntStatus::resource_limit;
        for(u32 i=0;i<selected.feature_indices.count/2;++i)rclt|=tag_of(byte_load_be16(selected.feature_indices.data+i*2))==sfnt_tag('r','c','l','t');
        // The required feature applies unmasked in the stage of its tag's feature, else the first (HarfBuzz's
        // required_feature_stage).
        if(selected.required_feature!=0xffff){s=add(selected.required_feature,choice(layout.kind,tag_of(selected.required_feature),false,rclt).stage,16);if(s!=SfntStatus::ok)return s;}
        u32 seen=0;
        for(u32 i=0;i<selected.feature_indices.count/2;++i) {
            if(!spend(&work))return SfntStatus::resource_limit;const u16 f=byte_load_be16(selected.feature_indices.data+i*2);
            const auto c=choice(layout.kind,tag_of(f),independent,rclt);
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
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    if(!layout.table.data || static_cast<u8>(layout.kind)>1 || (scratch.count && !scratch.data) || (output.count && !output.data))return SfntStatus::invalid_input;
    Builder builder{layout,variations,scratch,output,*result,work};const auto s=builder.run(independent);result->operations=work-builder.work;return s;
}
} // namespace maicord
