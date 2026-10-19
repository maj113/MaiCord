#include "maicord/text/lookup_plan.hpp"
#include "maicord/text/simple_script.hpp"
#include "maicord/text/khmer_plan.hpp"
#include "layout_language.hpp"
#include "lookup_plan_finish.hpp"
#include "maicord/text/base.hpp"
#include "maicord/sys/intrin.hpp"
namespace maicord {namespace {
bool spend(u64* work,u64 amount=1) noexcept {if(amount>*work)return false;*work-=amount;return true;}
u32 allowance(u64 work) noexcept {return static_cast<u32>(minimum<u64>(work,0xffffffffu));}
u16 feature_bit(SfntLayoutKind kind,u32 tag,bool independent) noexcept {
    if(independent){switch(tag){
        case sfnt_tag('c','c','m','p'):return 1;case sfnt_tag('l','o','c','l'):return 2;case sfnt_tag('r','l','i','g'):return 4;
        case sfnt_tag('r','c','l','t'):return 8;case sfnt_tag('r','v','r','n'):return 16;
        case sfnt_tag('d','i','s','t'):return 32;case sfnt_tag('m','a','r','k'):return 64;case sfnt_tag('m','k','m','k'):return 128;
        default:return 0;}}
    if(kind==SfntLayoutKind::substitution){switch(tag){
        case sfnt_tag('c','c','m','p'):return 1;case sfnt_tag('l','o','c','l'):return 2;case sfnt_tag('r','l','i','g'):return 4;
        case sfnt_tag('r','c','l','t'):return 8;case sfnt_tag('r','v','r','n'):return 16;
        case sfnt_tag('l','i','g','a'):return 32;case sfnt_tag('c','l','i','g'):return 64;case sfnt_tag('c','a','l','t'):return 128;
        default:return 0;}}
    switch(tag){case sfnt_tag('d','i','s','t'):return 1;case sfnt_tag('m','a','r','k'):return 2;case sfnt_tag('m','k','m','k'):return 4;case sfnt_tag('k','e','r','n'):return 8;default:return 0;}
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
        if(jamo&&layout.kind==SfntLayoutKind::substitution){
            const u32 tags[]{sfnt_tag('c','c','m','p'),sfnt_tag('l','j','m','o'),sfnt_tag('v','j','m','o'),sfnt_tag('t','j','m','o')};
            for(u32 pass=0;pass<2;++pass){const u64 before=work;result.lookup_count=0;u32 written=0;
                for(u32 stage=0;stage<4;++stage){if(!spend(&work,words))return SfntStatus::resource_limit;for(u32 i=0;i<words;++i)bits[i]=0;
                    if(!stage&&selected.required_feature!=0xffff){s=add(selected.required_feature);if(s!=SfntStatus::ok)return s;}
                    for(u32 i=0;i<selected.feature_indices.count/2;++i){if(!spend(&work))return SfntStatus::resource_limit;const u16 f=byte_load_be16(selected.feature_indices.data+i*2);
                        if(byte_load_be32(layout.features.data+2+static_cast<u32>(f)*6)!=tags[stage])continue;
                        if(stage||f!=selected.required_feature){s=add(f);if(s!=SfntStatus::ok)return s;}break;}
                    if(pass){if(!spend(&work,words))return SfntStatus::resource_limit;
                        for(u32 i=0;i<words;++i){u64 word=bits[i];while(word){if(!spend(&work))return SfntStatus::resource_limit;unsigned long bit=0;_BitScanForward64(&bit,word);output[written++]=static_cast<u16>(i*64+bit);word&=word-1;}}}
                }
                if(output.count<result.lookup_count)return SfntStatus::output_too_small;
                // The immutable replay has the same scan cost plus emission.
                // Reserve enough work before publishing any lookup index.
                if(!pass&&work<before-work+static_cast<u64>(words)*4+result.lookup_count)return SfntStatus::resource_limit;
            }return SfntStatus::ok;
        }
        for(u32 i=0;i<words;++i)bits.data[i]=0;
        if(selected.required_feature!=0xffff){s=add(selected.required_feature);if(s!=SfntStatus::ok)return s;}
        u16 seen=0;
        for(u32 i=0;i<selected.feature_indices.count/2;++i){if(!spend(&work))return SfntStatus::resource_limit;const u16 f=byte_load_be16(selected.feature_indices.data+i*2);
            const u32 feature_tag=byte_load_be32(layout.features.data+2+static_cast<u32>(f)*6);const u16 bit=feature_bit(layout.kind,feature_tag,independent);if(!bit || (seen&bit))continue;seen|=bit;
            if(f==selected.required_feature)continue;s=add(f);if(s!=SfntStatus::ok)return s;
        }
        if(output.count<result.lookup_count)return SfntStatus::output_too_small;
        const u64 emission=static_cast<u64>(words)+result.lookup_count;
        if(!spend(&work,emission))return SfntStatus::resource_limit;
        return finish_lookup_plan({bits.data,words},{output.data,result.lookup_count},0,emission);
    }
};
bool unusable(const SfntLayout& layout,Slice<u64> scratch,Slice<u16> indices) noexcept {
    return !layout.table.data||static_cast<u8>(layout.kind)>1||(scratch.count&&!scratch.data)||(indices.count&&!indices.data);
}
SfntStatus build(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,Slice<u64> scratch,Slice<u16> indices,SfntLookupPlanResult& result,u64 work,bool independent,bool common,u32 script) noexcept {
    Builder builder{layout,variations,scratch,indices,result,work};const auto status=builder.run(independent,common,script);result.operations=work-builder.work;return status;
}
}
__declspec(noinline) SfntStatus finish_lookup_plan(ConstSlice<u64> bits,Slice<u16> indices,u64 selected_work,u64 work) noexcept {
    if(!indices.count)return SfntStatus::ok;
    if(selected_work>work||static_cast<u64>(bits.count)+indices.count>work-selected_work)return SfntStatus::resource_limit;
    u32 written=0;
    for(u32 i=0;i<bits.count;++i){u64 word=bits[i];while(word){unsigned long bit=0;
        _BitScanForward64(&bit,word);indices[written++]=static_cast<u16>(i*64+bit);word&=word-1;}}
    return SfntStatus::ok;
}
SfntStatus sfnt_latin_lookup_plan(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,bool independent,Slice<u64> scratch,Slice<u16> indices,SfntLookupPlanResult* result,u64 work) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};if(unusable(layout,scratch,indices))return SfntStatus::invalid_input;
    return build(layout,variations,scratch,indices,*result,work,independent,false,0);
}
SfntStatus sfnt_common_lookup_plan(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,Slice<u64> scratch,Slice<u16> indices,SfntLookupPlanResult* result,u64 work,bool independent) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};if(unusable(layout,scratch,indices))return SfntStatus::invalid_input;
    return build(layout,variations,scratch,indices,*result,work,independent,true,0);
}
SfntStatus sfnt_khmer_fallback_substitutions(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,Slice<u64> scratch,Slice<u16> indices,SfntLookupPlanResult* result,u64 work) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    if(unusable(layout,scratch,indices)||layout.kind!=SfntLayoutKind::substitution)return SfntStatus::invalid_input;
    return build(layout,variations,scratch,indices,*result,work,true,false,sfnt_tag('k','h','m','r'));
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
    if(!result)return SfntStatus::invalid_input;*result={};
    if(script==sfnt_tag('l','a','t','n'))return sfnt_latin_lookup_plan(layout,variations,independent,scratch,indices,result,work);
    if(!sfnt_alphabetic_script_supported(script)||unusable(layout,scratch,indices))return SfntStatus::invalid_input;
    return build(layout,variations,scratch,indices,*result,work,independent,false,script);
}
}
