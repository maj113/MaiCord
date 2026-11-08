#include "maicord/text/scripts/indic_plan.hpp"
#include "maicord/core/memory.hpp"
#include "../opentype/layout_language.hpp"
#include "maicord/text/base.hpp"
namespace maicord {namespace {
constexpr u16 unmasked=256,global=512;
constexpr u8 stage_count=static_cast<u8>(SfntDevanagariStage::count);
struct Choice {u8 identity,stage,mask=0;};
// HarfBuzz hb-ot-shaper-indic feature order and masks, plus the default features it keeps for Indic.
Choice choice(SfntLayoutKind kind,u32 tag) noexcept {
    constexpr u8 present=static_cast<u8>(SfntDevanagariStage::presentation);
    if(kind==SfntLayoutKind::substitution)switch(tag){
        case sfnt_tag('r','v','r','n'):return {1,0};case sfnt_tag('l','o','c','l'):return {2,0};case sfnt_tag('c','c','m','p'):return {3,0};
        case sfnt_tag('n','u','k','t'):return {4,1};case sfnt_tag('a','k','h','n'):return {5,2};
        case sfnt_tag('r','p','h','f'):return {6,3,sfnt_deva_reph};case sfnt_tag('r','k','r','f'):return {7,4};
        case sfnt_tag('p','r','e','f'):return {8,5,sfnt_deva_pre};case sfnt_tag('b','l','w','f'):return {9,6,sfnt_deva_below};
        case sfnt_tag('a','b','v','f'):return {10,7,sfnt_deva_post};case sfnt_tag('h','a','l','f'):return {11,8,sfnt_deva_half};
        case sfnt_tag('p','s','t','f'):return {12,9,sfnt_deva_post};case sfnt_tag('v','a','t','u'):return {13,10};
        case sfnt_tag('c','j','c','t'):return {14,11};case sfnt_tag('i','n','i','t'):return {15,present,sfnt_deva_init};
        case sfnt_tag('p','r','e','s'):return {16,present};case sfnt_tag('a','b','v','s'):return {17,present};
        case sfnt_tag('b','l','w','s'):return {18,present};case sfnt_tag('p','s','t','s'):return {19,present};
        case sfnt_tag('h','a','l','n'):return {20,present};case sfnt_tag('r','l','i','g'):return {21,present};
        case sfnt_tag('c','a','l','t'):return {22,present};case sfnt_tag('c','l','i','g'):return {23,present};
        case sfnt_tag('r','c','l','t'):return {24,present};default:return {};
    }
    switch(tag){
        case sfnt_tag('k','e','r','n'):return {1,0};case sfnt_tag('d','i','s','t'):return {2,0};
        case sfnt_tag('a','b','v','m'):return {3,0};case sfnt_tag('b','l','w','m'):return {4,0};
        case sfnt_tag('m','a','r','k'):return {5,0};case sfnt_tag('m','k','m','k'):return {6,0};
        case sfnt_tag('c','u','r','s'):return {7,0};default:return {};
    }
}
// HarfBuzz applies these GSUB features across syllables (F_GLOBAL without F_PER_SYLLABLE); every other
// Indic feature, locl and ccmp included, stays inside each syllable.
constexpr bool global_feature(u32 tag) noexcept {
    return tag==sfnt_tag('r','v','r','n')||tag==sfnt_tag('r','l','i','g')||tag==sfnt_tag('c','a','l','t')||
        tag==sfnt_tag('c','l','i','g')||tag==sfnt_tag('r','c','l','t');
}
struct Selected {SfntFeature feature;u8 stage,mask;bool global;};
struct Builder {
    const SfntLayout& layout;const SfntFeatureSubstitutions& variations;Slice<u16> scratch;Slice<SfntLookupStep> output;
    SfntLookupPlanResult& result;u64 work;Slice<u64> per_syllable{};Selected features[32]{};u32 count=0;u8 form=0;
    SfntStatus add(u16 index,u8 stage,u8 mask) noexcept {
        if(count==32)return SfntStatus::resource_limit;u32 tag=0,used=0;auto& entry=features[count];
        const auto status=sfnt_layout_feature(layout,index,variations,&tag,&entry.feature,static_cast<u32>(minimum<u64>(work,0xffffffffu)),&used);
        work-=used;if(status!=SfntStatus::ok)return status;entry.stage=stage;entry.mask=mask;
        // A required feature of a tag outside the Indic set applies to the whole run.
        entry.global=global_feature(tag)||!choice(layout.kind,tag).identity;++count;return SfntStatus::ok;
    }
    SfntStatus pass(bool write) noexcept {
        u32 emitted=0;const u8 stages=layout.kind==SfntLayoutKind::substitution&&!form?stage_count:1;
        if(write)for(u32 i=0;i<per_syllable.count;++i)per_syllable[i]=0;
        for(u8 stage=0;stage<stages;++stage){
            if(stage==static_cast<u8>(SfntDevanagariStage::final_reorder))continue;
            if(!spend(&work,layout.lookup_count))return SfntStatus::resource_limit;
            for(u32 i=0;i<layout.lookup_count;++i)scratch[i]=0;
            for(u32 f=0;f<count;++f){
                if(!spend(&work))return SfntStatus::resource_limit;const auto& selected=features[f];if(selected.stage!=stage)continue;
                const auto indices=selected.feature.lookup_indices;if(!spend(&work,indices.count/2))return SfntStatus::resource_limit;
                for(u32 i=0;i<indices.count/2;++i)scratch[byte_load_be16(indices.data+i*2)]|=(selected.mask?selected.mask:unmasked)|(selected.global?global:0);
            }
            if(!spend(&work,layout.lookup_count))return SfntStatus::resource_limit;
            for(u32 i=0;i<layout.lookup_count;++i)if(scratch[i]){
                if(write){if(!spend(&work))return SfntStatus::resource_limit;
                    output[emitted]={static_cast<u16>(i),static_cast<u8>(scratch[i]&unmasked?0:scratch[i]),static_cast<u8>(stage>static_cast<u8>(SfntDevanagariStage::final_reorder))};
                    if(per_syllable.count&&!(scratch[i]&global))per_syllable[emitted>>6]|=1ull<<(emitted&63);}
                ++emitted;
            }
        }
        if(!write)result.lookup_count=emitted;return SfntStatus::ok;
    }
    SfntStatus run(u32 script_tag) noexcept {
        const u32 script[]{script_tag};SfntLanguageSelection selection{};
        auto status=sfnt_layout_select_language_prepared(layout,{script,1},sfnt_tag('E','N','G',' '),selection,work);result.script_tag=selection.script_tag;
        if(status==SfntStatus::missing_table)return SfntStatus::ok;if(status!=SfntStatus::ok)return status;
        result.language_found=true;result.language_tag=selection.language_tag;const auto& language=selection.language;
        if(scratch.count<layout.lookup_count)return SfntStatus::output_too_small;
        const auto tag_of=[&](u16 index) noexcept {return byte_load_be32(layout.features.data+2+static_cast<u32>(index)*6);};
        // The required feature applies to every glyph in its tag's stage (stage 0 for other tags).
        if(language.required_feature!=65535){const auto required=choice(layout.kind,tag_of(language.required_feature));
            if(!form||(required.identity&&required.stage==form)){status=add(language.required_feature,form?0:required.stage,0);if(status!=SfntStatus::ok)return status;}}
        u32 seen=0;
        for(u32 i=0;i<language.feature_indices.count/2;++i){
            if(!spend(&work))return SfntStatus::resource_limit;const u16 index=byte_load_be16(language.feature_indices.data+i*2);
            if(index==language.required_feature)continue;
            const auto selected=choice(layout.kind,tag_of(index));if(!selected.identity)continue;
            if(form&&selected.stage!=form)continue;
            const u32 bit=1u<<selected.identity;if(seen&bit)continue;seen|=bit;
            status=add(index,form?0:selected.stage,form?0:selected.mask);if(status!=SfntStatus::ok)return status;
        }
        const u64 before=work;status=pass(false);if(status!=SfntStatus::ok)return status;
        if(output.count<result.lookup_count||(per_syllable.count&&per_syllable.count<(result.lookup_count+63)/64))return SfntStatus::output_too_small;
        if(work<before-work+result.lookup_count)return SfntStatus::resource_limit;
        return pass(true);
    }
};
}
SfntStatus sfnt_indic_lookup_plan(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,SfntIndicProfile profile,
    Slice<u16> scratch,Slice<SfntLookupStep> output,SfntLookupPlanResult* result,u64 work,Slice<u64> per_syllable) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    const auto* info=sfnt_indic_script(profile);const u32 script=info?info->tag:0;
    if(!script||!layout.table.data||static_cast<u8>(layout.kind)>1||(scratch.count&&!scratch.data)||(output.count&&!output.data)||
       (per_syllable.count&&!per_syllable.data))return SfntStatus::invalid_input;
    Builder builder{layout,variations,scratch,output,*result,work,per_syllable};const auto status=builder.run(script);result->operations=work-builder.work;return status;
}
SfntStatus sfnt_indic_form_lookup_plan(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,SfntIndicProfile profile,SfntIndicForm form,
    Slice<u16> scratch,Slice<SfntLookupStep> output,SfntLookupPlanResult* result,u64 work) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);u8 stage=0;
    switch(form){case sfnt_deva_reph:stage=3;break;case sfnt_deva_pre:stage=5;break;case sfnt_deva_below:stage=6;break;
        case sfnt_deva_post:stage=9;break;case sfnt_deva_vatu:stage=10;break;default:return SfntStatus::invalid_input;}
    const auto* info=sfnt_indic_script(profile);const u32 script=info?info->tag:0;
    if(!script||!layout.table.data||layout.kind!=SfntLayoutKind::substitution||(scratch.count&&!scratch.data)||(output.count&&!output.data))return SfntStatus::invalid_input;
    Builder builder{layout,variations,scratch,output,*result,work};builder.form=stage;
    const auto status=builder.run(script);result->operations=work-builder.work;return status;
}
}
