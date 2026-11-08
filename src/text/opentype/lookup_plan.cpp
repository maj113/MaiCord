#include "maicord/text/opentype/lookup_plan.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/shaping/simple_script.hpp"
#include "layout_language.hpp"
#include "maicord/text/base.hpp"
#include "maicord/sys/intrin.hpp"
namespace maicord {namespace {
u32 allowance(u64 work) noexcept {return static_cast<u32>(minimum<u64>(work,0xffffffffu));}
// HarfBuzz's horizontal default-shaper features, applied from either table (hb_ot_shape_collect_features): rvrn,
// rand, trak, abvm, blwm, ccmp, locl, mark, mkmk, rlig, calt, clig, curs, dist, kern, liga and rclt. Independent
// letters turn off liga, clig and kern only, as HarfBuzz features. The direction features (ltra, ltrm, rtla, rtlm)
// are not applied, and rvrn shares the one stage.
u32 feature_bit(u32 tag,bool independent) noexcept {
    switch(tag){
        case sfnt_tag('r','v','r','n'):return 1u<<0;case sfnt_tag('r','a','n','d'):return 1u<<1;case sfnt_tag('t','r','a','k'):return 1u<<2;
        case sfnt_tag('a','b','v','m'):return 1u<<3;case sfnt_tag('b','l','w','m'):return 1u<<4;case sfnt_tag('c','c','m','p'):return 1u<<5;
        case sfnt_tag('l','o','c','l'):return 1u<<6;case sfnt_tag('m','a','r','k'):return 1u<<7;case sfnt_tag('m','k','m','k'):return 1u<<8;
        case sfnt_tag('r','l','i','g'):return 1u<<9;case sfnt_tag('c','a','l','t'):return 1u<<10;case sfnt_tag('c','u','r','s'):return 1u<<11;
        case sfnt_tag('d','i','s','t'):return 1u<<12;case sfnt_tag('r','c','l','t'):return 1u<<13;
        case sfnt_tag('c','l','i','g'):return independent?0u:1u<<14;case sfnt_tag('k','e','r','n'):return independent?0u:1u<<15;
        case sfnt_tag('l','i','g','a'):return independent?0u:1u<<16;
        default:return 0;
    }
}
struct Builder {
    const SfntLayout& layout;const SfntFeatureSubstitutions& variations;Slice<u64> bits;Slice<u16> output;SfntLookupPlanResult& result;u64 work;
    SfntStatus add(u16 index) noexcept {
        SfntFeature feature{};u32 tag=0,used=0;auto s=sfnt_layout_feature(layout,index,variations,&tag,&feature,allowance(work),&used);work-=used;if(s!=SfntStatus::ok)return s;
        if(!spend(&work,feature.lookup_indices.count/2))return SfntStatus::resource_limit;
        for(u32 i=0;i<feature.lookup_indices.count/2;++i){const u16 lookup=byte_load_be16(feature.lookup_indices.data+i*2);const u64 bit=1ull<<(lookup%64);auto& word=bits.data[lookup/64];if(!(word&bit)){word|=bit;++result.lookup_count;}}
        return SfntStatus::ok;
    }
    SfntStatus run(bool independent,bool common=false,u32 script=0) noexcept {
        const u32 first=common?sfnt_tag('D','F','L','T'):sfnt_tag('l','a','t','n'),second=common?sfnt_tag('l','a','t','n'):sfnt_tag('D','F','L','T');
        const bool jamo=script==sfnt_tag('j','a','m','o');
        const bool hangul=jamo||script==sfnt_tag('h','a','n','g');
        const u32 candidates[]{jamo?sfnt_tag('h','a','n','g'):script,first,second};SfntLanguageSelection selection{};u64 remaining=work;
        // Explicit independent-letter typography enables the normal fallback
        // feature selection; ordinary Hangul keeps a missing script empty.
        auto s=sfnt_layout_select_language_prepared(layout,{candidates+(script?0:1),hangul&&!independent?1u:script?3u:2u},sfnt_tag('E','N','G',' '),selection,remaining);work=remaining;result.script_tag=selection.script_tag;
        if(s==SfntStatus::missing_table)return SfntStatus::ok;if(s!=SfntStatus::ok)return s;
        result.language_found=true;result.language_tag=selection.language_tag;const auto& selected=selection.language;
        const u32 words=(static_cast<u32>(layout.lookup_count)+63)/64;if(bits.count<words)return SfntStatus::output_too_small;if(!spend(&work,words))return SfntStatus::resource_limit;
        // Stages run in order, each emitting its lookups ascending: rvrn and then every other default feature
        // (HarfBuzz pauses after rvrn). The required feature runs in its tag's stage, else the first (HarfBuzz's
        // required_feature_stage).
        constexpr u32 stages=2;
        const auto tag_of=[&](u16 f) noexcept {return byte_load_be32(layout.features.data+2+static_cast<u32>(f)*6);};
        const auto stage_of=[&](u32 tag) noexcept -> u32 {return feature_bit(tag,independent)?(tag==sfnt_tag('r','v','r','n')?0u:1u):stages;};
        const u32 required=selected.required_feature,required_stage=required==0xffff||stage_of(tag_of(required))==stages?0u:stage_of(tag_of(required));
        for(u32 pass=0;pass<2;++pass){const u64 before=work;result.lookup_count=0;u32 written=0;
            for(u32 stage=0;stage<stages;++stage){if(!spend(&work,words))return SfntStatus::resource_limit;for(u32 i=0;i<words;++i)bits[i]=0;
                if(required!=0xffff&&stage==required_stage){s=add(required);if(s!=SfntStatus::ok)return s;}
                u32 seen=0;
                for(u32 i=0;i<selected.feature_indices.count/2;++i){if(!spend(&work))return SfntStatus::resource_limit;const u16 f=byte_load_be16(selected.feature_indices.data+i*2);
                    const u32 tag=tag_of(f);if(stage_of(tag)!=stage)continue;
                    const u32 bit=feature_bit(tag,independent);if(seen&bit)continue;seen|=bit;
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
bool unusable(const SfntLayout& layout,const Slice<u64>& scratch,const Slice<u16>& indices) noexcept {
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
        case sfnt_tag('h','a','n','g'):case sfnt_tag('j','a','m','o'):case sfnt_tag('a','r','m','n'):
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
}
