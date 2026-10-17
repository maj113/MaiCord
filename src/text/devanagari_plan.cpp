#include "maicord/text/indic_plan.hpp"
#include "layout_language.hpp"
#include "maicord/text/base.hpp"
namespace maicord {namespace {
constexpr u16 unmasked=256;
constexpr u8 required_stage=255;
constexpr u8 stage_masks[]{0,sfnt_deva_nukta,0,sfnt_deva_reph,0,sfnt_deva_pre,
    sfnt_deva_below,sfnt_deva_half,sfnt_deva_post,sfnt_deva_vatu,sfnt_deva_conjunct,0,0};
static_assert(sizeof(stage_masks)==static_cast<u8>(SfntDevanagariStage::count));
bool spend(u64& work,u64 count=1) noexcept {if(count>work)return false;work-=count;return true;}
struct Choice {u8 identity,stage,mask=0;};
Choice choice(SfntLayoutKind kind,u32 tag,bool initial) noexcept {
    if(kind==SfntLayoutKind::substitution)switch(tag){
        case sfnt_tag('l','o','c','l'):return {1,0};case sfnt_tag('n','u','k','t'):return {2,1};
        case sfnt_tag('a','k','h','n'):return {3,2};case sfnt_tag('r','p','h','f'):return {4,3};
        case sfnt_tag('r','k','r','f'):return {5,4};case sfnt_tag('p','r','e','f'):return {6,5};
        case sfnt_tag('b','l','w','f'):return {7,6};case sfnt_tag('h','a','l','f'):return {8,7};
        case sfnt_tag('p','s','t','f'):return {9,8};case sfnt_tag('v','a','t','u'):return {10,9};
        case sfnt_tag('c','j','c','t'):return {11,10};
        case sfnt_tag('p','r','e','s'):return {12,12};case sfnt_tag('a','b','v','s'):return {13,12};
        case sfnt_tag('b','l','w','s'):return {14,12};case sfnt_tag('p','s','t','s'):return {15,12};
        case sfnt_tag('h','a','l','n'):return {16,12};case sfnt_tag('c','a','l','t'):return {17,12};
        case sfnt_tag('r','c','l','t'):return {18,12};
        case sfnt_tag('i','n','i','t'):return initial?Choice{19,12,sfnt_bengali_initial}:Choice{};default:return {};
    }
    switch(tag){
        case sfnt_tag('k','e','r','n'):return {1,0};case sfnt_tag('d','i','s','t'):return {2,0};
        case sfnt_tag('a','b','v','m'):return {3,0};case sfnt_tag('b','l','w','m'):return {4,0};
        case sfnt_tag('m','a','r','k'):return {5,0};case sfnt_tag('m','k','m','k'):return {6,0};default:return {};
    }
}
struct Selected {SfntFeature feature;u8 stage,mask,identity;};
static_assert(sizeof(Selected)==48);
struct Builder {
    const SfntLayout& layout;const SfntFeatureSubstitutions& variations;Slice<u16> scratch;Slice<SfntLookupStep> output;
    SfntLookupPlanResult& result;u64 work;Selected features[20]{};u32 count=0;u8 form_stage=255;bool initial=false,ordered_presentation=false;
    SfntStatus add(u16 index,u8 stage,u8 mask=0,u8 identity=0) noexcept {
        if(count==20)return SfntStatus::resource_limit;u32 tag=0,used=0;auto& entry=features[count];
        const auto status=sfnt_layout_feature(layout,index,variations,&tag,&entry.feature,static_cast<u32>(minimum<u64>(work,0xffffffffu)),&used);
        work-=used;if(status!=SfntStatus::ok)return status;entry.stage=stage;entry.mask=mask;entry.identity=identity;++count;return SfntStatus::ok;
    }
    SfntStatus pass(bool write) noexcept {
        u32 emitted=0;const u8 stages=layout.kind==SfntLayoutKind::substitution&&form_stage==255?static_cast<u8>(SfntDevanagariStage::count):1;
        for(u8 stage=0;stage<stages;++stage){
            if(stage==static_cast<u8>(SfntDevanagariStage::final_reorder))continue;
            const u32 passes=ordered_presentation&&stage==12?6u:1u;
            for(u32 pass=0;pass<passes;++pass){
            if(!spend(work,layout.lookup_count))return SfntStatus::resource_limit;
            for(u32 i=0;i<layout.lookup_count;++i)scratch[i]=0;
            const u16 mask=stage_masks[stage]?stage_masks[stage]:unmasked;
            for(u32 f=0;f<count;++f){
                if(!spend(work))return SfntStatus::resource_limit;const auto& selected=features[f];
                if(selected.stage!=stage&&selected.stage!=required_stage)continue;
                if(passes>1&&(selected.stage==required_stage?pass!=0:(selected.identity>=17?5u:static_cast<u32>(selected.identity)-12)!=pass))continue;
                const auto indices=selected.feature.lookup_indices;if(!spend(work,indices.count/2))return SfntStatus::resource_limit;
                // Fallback calt/rclt share font order, but a lookup referenced by both
                // executes once per feature. Two spare scratch bits record which
                // feature added it; repeats inside one feature still collapse.
                const u16 context=passes>1&&pass==5?static_cast<u16>(selected.identity==17?512:1024):0;
                for(u32 i=0;i<indices.count/2;++i)scratch[byte_load_be16(indices.data+i*2)]|=(selected.mask?selected.mask:mask)|context;
            }
            if(!spend(work,layout.lookup_count))return SfntStatus::resource_limit;
            for(u32 i=0;i<layout.lookup_count;++i)if(scratch[i]){
                const u32 repeat=(scratch[i]&1536)==1536?2u:1u;
                if(write){if(!spend(work,repeat))return SfntStatus::resource_limit;for(u32 n=0;n<repeat;++n)output[emitted+n]={static_cast<u16>(i),static_cast<u8>(scratch[i]&unmasked?0:scratch[i]),stage};}
                emitted+=repeat;
            }
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
        u32 seen=0;
        if(language.required_feature!=65535){
            if(form_stage==255){status=add(language.required_feature,required_stage);if(status!=SfntStatus::ok)return status;}
            else{const auto required=choice(layout.kind,byte_load_be32(layout.features.data+2+static_cast<u32>(language.required_feature)*6),initial);
                if(required.identity&&(required.stage==0||required.stage==form_stage)){
                    seen|=1u<<required.identity;status=add(language.required_feature,0);if(status!=SfntStatus::ok)return status;}}
        }
        for(u32 i=0;i<language.feature_indices.count/2;++i){
            if(!spend(work))return SfntStatus::resource_limit;const u16 index=byte_load_be16(language.feature_indices.data+i*2);
            const auto selected=choice(layout.kind,byte_load_be32(layout.features.data+2+static_cast<u32>(index)*6),initial);
            if(form_stage!=255&&selected.stage!=0&&selected.stage!=form_stage)continue;
            if(!selected.identity)continue;const u32 bit=1u<<selected.identity;if(seen&bit)continue;seen|=bit;
            if(index==language.required_feature&&form_stage==255)continue;status=add(index,form_stage==255?selected.stage:0,form_stage==255?selected.mask:0,selected.identity);if(status!=SfntStatus::ok)return status;
        }
        if(form_stage!=255&&!(seen&(1u<<(form_stage+1))))count=0;
        const u64 before=work;status=pass(false);if(status!=SfntStatus::ok)return status;
        if(output.count<result.lookup_count)return SfntStatus::output_too_small;
        if(work<before-work+result.lookup_count)return SfntStatus::resource_limit;
        return pass(true);
    }
};

}
SfntStatus sfnt_indic_lookup_plan(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,SfntIndicProfile profile,bool independent,
    Slice<u16> scratch,Slice<SfntLookupStep> output,SfntLookupPlanResult* result,u64 work,bool ordered_presentation) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};(void)independent;
    const auto* info=sfnt_indic_script(profile);const u32 script=info?info->tag:0;
    if(ordered_presentation&&(profile!=SfntIndicProfile::malayalam||layout.kind!=SfntLayoutKind::substitution))return SfntStatus::invalid_input;
    if(!script||!layout.table.data||static_cast<u8>(layout.kind)>1||(scratch.count&&!scratch.data)||(output.count&&!output.data))return SfntStatus::invalid_input;
    Builder builder{layout,variations,scratch,output,*result,work};builder.initial=profile==SfntIndicProfile::bengali;builder.ordered_presentation=ordered_presentation;const auto status=builder.run(script);result->operations=work-builder.work;return status;
}
SfntStatus sfnt_indic_form_lookup_plan(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,SfntIndicProfile profile,SfntIndicForm form,
    Slice<u16> scratch,Slice<SfntLookupStep> output,SfntLookupPlanResult* result,u64 work) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};u8 stage=255;
    switch(form){case sfnt_deva_reph:stage=3;break;case sfnt_deva_half:stage=7;break;case sfnt_deva_pre:stage=5;break;
        case sfnt_deva_below:stage=6;break;case sfnt_deva_post:stage=8;break;default:return SfntStatus::invalid_input;}
    const auto* info=sfnt_indic_script(profile);const u32 script=info?info->tag:0;
    if(!script||!layout.table.data||layout.kind!=SfntLayoutKind::substitution||(scratch.count&&!scratch.data)||(output.count&&!output.data))return SfntStatus::invalid_input;
    Builder builder{layout,variations,scratch,output,*result,work};builder.initial=profile==SfntIndicProfile::bengali;builder.form_stage=stage;
    const auto status=builder.run(script);result->operations=work-builder.work;return status;
}
}
