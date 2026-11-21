#include "maicord/text/opentype/lookup_plan.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/shaping/simple_script.hpp"
#include "layout_language.hpp"
#include "maicord/text/base.hpp"
#include "maicord/sys/intrin.hpp"
namespace maicord {namespace {
// HarfBuzz's horizontal default-shaper features, applied from either table (hb_ot_shape_collect_features): rvrn,
// rand, trak, abvm, blwm, ccmp, locl, mark, mkmk, rlig, calt, clig, curs, dist, kern, liga and rclt. Independent
// letters turn off liga, clig and kern only, as HarfBuzz features. The direction features (ltra, ltrm, rtla, rtlm)
// are not applied. A tag's bit is its index here; the last three are the ones independent letters turn off.
constexpr u32 default_features[]{sfnt_tag('r','v','r','n'),sfnt_tag('r','a','n','d'),sfnt_tag('t','r','a','k'),sfnt_tag('a','b','v','m'),
    sfnt_tag('b','l','w','m'),sfnt_tag('c','c','m','p'),sfnt_tag('l','o','c','l'),sfnt_tag('m','a','r','k'),sfnt_tag('m','k','m','k'),
    sfnt_tag('r','l','i','g'),sfnt_tag('c','a','l','t'),sfnt_tag('c','u','r','s'),sfnt_tag('d','i','s','t'),sfnt_tag('r','c','l','t'),
    sfnt_tag('c','l','i','g'),sfnt_tag('k','e','r','n'),sfnt_tag('l','i','g','a')};
// A multiplicative hash that gives each of the 17 tags its own one of 32 slots. A slot keeps its tag and its index
// plus one (zero for an empty slot).
constexpr u32 default_slot(u32 tag) noexcept {return (tag*0x26a2e176u)>>27;}
struct DefaultFeatureSlots {u32 tags[32];u8 indices[32];};
constexpr DefaultFeatureSlots default_slots=[]{DefaultFeatureSlots t{};
    for(u32 i=0;i<17;++i){t.tags[default_slot(default_features[i])]=default_features[i];t.indices[default_slot(default_features[i])]=static_cast<u8>(i+1);}return t;}();
static_assert([]{u32 used=0;for(const u32 tag:default_features)used|=1u<<default_slot(tag);return __builtin_popcount(used)==17;}());
}
u32 sfnt_default_feature_bit(u32 tag,bool independent) noexcept {
    const u32 slot=default_slot(tag),index=default_slots.tags[slot]==tag?default_slots.indices[slot]:0u;
    return index&&!(independent&&index>14)?1u<<(index-1):0u;
}
namespace {
struct Builder {
    const SfntLayout& layout;const SfntFeatureSubstitutions& variations;Slice<u64> bits;Slice<u16> output;SfntLookupPlanResult& result;u64 work;
    SfntStatus add(u16 index) noexcept {
        SfntFeature feature{};u32 tag=0,used=0;auto s=sfnt_layout_feature(layout,index,variations,&tag,&feature,work_allowance(work),&used);work-=used;if(s!=SfntStatus::ok)return s;
        if(!spend(&work,feature.lookup_indices.count/2))return SfntStatus::resource_limit;
        for(u32 i=0;i<feature.lookup_indices.count/2;++i){const u16 lookup=byte_load_be16(feature.lookup_indices.data+i*2);const u64 bit=1ull<<(lookup%64);auto& word=bits.data[lookup/64];if(!(word&bit)){word|=bit;++result.lookup_count;}}
        return SfntStatus::ok;
    }
    SfntStatus run(bool independent,bool common=false,u32 script=0) noexcept {
        const u32 first=common?sfnt_tag('D','F','L','T'):sfnt_tag('l','a','t','n'),second=common?sfnt_tag('l','a','t','n'):sfnt_tag('D','F','L','T');
        const u32 candidates[]{script,first,second};SfntLanguageSelection selection{};u64 remaining=work;
        auto s=sfnt_layout_select_language_prepared(layout,{candidates+(script?0:1),script?3u:2u},sfnt_tag('E','N','G',' '),selection,remaining);work=remaining;result.script_tag=selection.script_tag;
        if(s==SfntStatus::missing_table)return SfntStatus::ok;if(s!=SfntStatus::ok)return s;
        result.language_found=true;result.language_tag=selection.language_tag;const auto& selected=selection.language;
        const u32 words=(static_cast<u32>(layout.lookup_count)+63)/64;if(bits.count<words)return SfntStatus::output_too_small;if(!spend(&work,words))return SfntStatus::resource_limit;
        // Stages run in order, each emitting its lookups ascending: rvrn and then every other default feature
        // (HarfBuzz pauses after rvrn). The required feature runs in its tag's stage, else the first (HarfBuzz's
        // required_feature_stage).
        constexpr u32 stages=2;
        const auto tag_of=[&](u16 f) noexcept {return byte_load_be32(layout.features.data+2+static_cast<u32>(f)*6);};
        const auto stage_bit=[](u32 bit) noexcept -> u32 {return bit==1u?0u:1u;}; // rvrn (bit 0) runs alone, first
        const auto stage_of=[&](u32 tag) noexcept -> u32 {const u32 bit=sfnt_default_feature_bit(tag,independent);return bit?stage_bit(bit):stages;};
        const u32 required=selected.required_feature,required_stage=required==0xffff||stage_of(tag_of(required))==stages?0u:stage_of(tag_of(required));
        for(u32 pass=0;pass<2;++pass){const u64 before=work;result.lookup_count=0;u32 written=0;
            for(u32 stage=0;stage<stages;++stage){if(!spend(&work,words))return SfntStatus::resource_limit;for(u32 i=0;i<words;++i)bits[i]=0;
                if(required!=0xffff&&stage==required_stage){s=add(required);if(s!=SfntStatus::ok)return s;}
                u32 seen=0;
                for(u32 i=0;i<selected.feature_indices.count/2;++i){if(!spend(&work))return SfntStatus::resource_limit;const u16 f=byte_load_be16(selected.feature_indices.data+i*2);
                    const u32 bit=sfnt_default_feature_bit(tag_of(f),independent);if(!bit||stage_bit(bit)!=stage)continue;
                    if(seen&bit)continue;seen|=bit;
                    if(f==required)continue;s=add(f);if(s!=SfntStatus::ok)return s;}
                if(pass){if(!spend(&work,words))return SfntStatus::resource_limit;
                    for(u32 i=0;i<words;++i){u64 word=bits[i];while(word){if(!spend(&work))return SfntStatus::resource_limit;unsigned long bit=0;_BitScanForward64(&bit,word);output[written++]=static_cast<u16>(i*64+bit);word&=word-1;}}}
            }
            if(output.count<result.lookup_count)return SfntStatus::output_too_small;
            // The immutable replay has the same scan cost plus emission.
            // Reserve enough work before publishing any lookup index.
            if(!pass&&work<before-work+static_cast<u64>(words)*stages+result.lookup_count)return SfntStatus::resource_limit;
        }return SfntStatus::ok;
    }
};
inline bool unusable(const SfntLayout& layout,const Slice<u64>& scratch,const Slice<u16>& indices) noexcept {
    return !layout.table.data||static_cast<u8>(layout.kind)>1||(scratch.count&&!scratch.data)||(indices.count&&!indices.data);
}
SfntStatus build(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,const Slice<u64>& scratch,const Slice<u16>& indices,SfntLookupPlanResult& result,u64 work,bool independent,bool common,u32 script) noexcept {
    Builder builder{layout,variations,scratch,indices,result,work};const auto status=builder.run(independent,common,script);result.operations=work-builder.work;return status;
}
}
SfntStatus sfnt_latin_lookup_plan(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,bool independent,Slice<u64> scratch,Slice<u16> indices,SfntLookupPlanResult* result,u64 work) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);if(unusable(layout,scratch,indices))return SfntStatus::invalid_input;
    return build(layout,variations,scratch,indices,*result,work,independent,false,0);
}
SfntStatus sfnt_common_lookup_plan(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,Slice<u64> scratch,Slice<u16> indices,SfntLookupPlanResult* result,u64 work,bool independent) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);if(unusable(layout,scratch,indices))return SfntStatus::invalid_input;
    return build(layout,variations,scratch,indices,*result,work,independent,true,0);
}
bool sfnt_alphabetic_script_supported(u32 script) noexcept {
    switch(script){
        case sfnt_tag('l','a','t','n'):case sfnt_tag('g','r','e','k'):case sfnt_tag('c','y','r','l'):
        case sfnt_tag('h','e','b','r'):case sfnt_tag('h','a','n','i'):case sfnt_tag('k','a','n','a'):
        case sfnt_tag('h','a','n','g'):case sfnt_tag('a','r','m','n'):
        case sfnt_tag('g','e','o','r'):case sfnt_tag('t','h','a','i'):case sfnt_tag('l','a','o',' '):
        case sfnt_tag('e','t','h','i'):case sfnt_tag('c','h','e','r'):
        case sfnt_tag('e','g','y','p'):case sfnt_tag('r','u','n','r'):return true;
        default:for(const u32 tag:simple_script_tags.values)if(tag==script)return true;return false;
    }
}
SfntStatus sfnt_alphabetic_lookup_plan(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,u32 script,bool independent,Slice<u64> scratch,Slice<u16> indices,SfntLookupPlanResult* result,u64 work) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    if(script==sfnt_tag('l','a','t','n'))return sfnt_latin_lookup_plan(layout,variations,independent,scratch,indices,result,work);
    if(!sfnt_alphabetic_script_supported(script)||unusable(layout,scratch,indices))return SfntStatus::invalid_input;
    return build(layout,variations,scratch,indices,*result,work,independent,false,script);
}
__attribute__((minsize)) SfntStatus sfnt_manual_joiner_lookups(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,ConstSlice<SfntJoinerFeature> features,
    Slice<u64> bits,bool* any,u64 work) noexcept {
    if(!any||!layout.table.data||(features.count&&!features.data))return SfntStatus::invalid_input;
    const u32 words=(2u*layout.lookup_count+63u)/64u;if(bits.count<words||(words&&!bits.data))return SfntStatus::invalid_input;
    for(u32 f=0;f<layout.feature_count;++f){if(!spend(&work,1+features.count))return SfntStatus::resource_limit;
        const u32 tag=byte_load_be32(layout.features.data+2+f*6);u8 manual=0;
        for(u32 i=0;i<features.count;++i)if(features[i].tag==tag)manual|=static_cast<u8>(features[i].manual&3u);
        if(!manual)continue;
        u32 found=0;SfntFeature feature{};u32 used=0;const auto status=sfnt_layout_feature(layout,static_cast<u16>(f),variations,&found,&feature,static_cast<u32>(minimum<u64>(work,0xffffffffu)),&used);
        work-=used;if(status!=SfntStatus::ok)return status;
        const u32 count=feature.lookup_indices.count/2;if(!spend(&work,count))return SfntStatus::resource_limit;
        for(u32 i=0;i<count;++i){const u32 lookup=byte_load_be16(feature.lookup_indices.data+i*2);if(lookup>=layout.lookup_count)continue;
            bits[2*lookup/64]|=static_cast<u64>(manual)<<(2*lookup%64);*any=true;}
    }
    return SfntStatus::ok;
}
SfntStatus sfnt_staged_lookup_plan(const SfntLayout& layout,ConstSlice<SfntStagedFeature> features,u32 stages,u32 skipped,Slice<u16> scratch,
    Slice<SfntLookupStep> output,Slice<u64> per_syllable,SfntLookupPlanResult& result,u64& work) noexcept {
    const u64 before=work;
    for(u32 write=0;write<2;++write){u32 emitted=0;
        if(write){if(output.count<result.lookup_count||(per_syllable.count&&per_syllable.count<(result.lookup_count+63)/64))return SfntStatus::output_too_small;
            if(work<before-work+result.lookup_count)return SfntStatus::resource_limit;
            for(u32 i=0;i<per_syllable.count;++i)per_syllable[i]=0;}
        for(u32 stage=0;stage<stages;++stage){
            if(stage==skipped)continue;
            if(!spend(&work,layout.lookup_count))return SfntStatus::resource_limit;
            for(u32 i=0;i<layout.lookup_count;++i)scratch[i]=0;
            for(u32 f=0;f<features.count;++f){
                if(!spend(&work))return SfntStatus::resource_limit;const auto& s=features[f];if(s.stage!=stage)continue;
                const auto indices=s.feature.lookup_indices;if(!spend(&work,indices.count/2))return SfntStatus::resource_limit;
                for(u32 i=0;i<indices.count/2;++i){const u16 lookup=byte_load_be16(indices.data+i*2);
                    if(lookup<layout.lookup_count)scratch[lookup]|=(s.mask?s.mask:sfnt_plan_unmasked)|(s.global?sfnt_plan_global:0);}
            }
            if(!spend(&work,layout.lookup_count))return SfntStatus::resource_limit;
            for(u32 i=0;i<layout.lookup_count;++i)if(scratch[i]){
                if(write){if(!spend(&work))return SfntStatus::resource_limit;
                    output[emitted]={static_cast<u16>(i),static_cast<u8>(scratch[i]&sfnt_plan_unmasked?0:scratch[i]),static_cast<u8>(skipped==255?stage:stage>skipped)};
                    if(per_syllable.count&&!(scratch[i]&sfnt_plan_global))per_syllable[emitted>>6]|=1ull<<(emitted&63);}
                ++emitted;
            }
        }
        if(!write)result.lookup_count=emitted;}
    return SfntStatus::ok;
}
}
