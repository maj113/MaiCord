#include "maicord/text/use_plan.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/khmer_plan.hpp"
#include "maicord/text/base.hpp"
#include "layout_language.hpp"
namespace maicord {namespace {
struct Feature {u32 tag;u8 stage;};
constexpr u8 optional=128,conditional=64,substitution_stages=0x6f;
constexpr Feature substitution[]{
    {sfnt_tag('l','o','c','l'),0},{sfnt_tag('c','c','m','p'),0},
    {sfnt_tag('n','u','k','t'),0},{sfnt_tag('a','k','h','n'),0},
    {sfnt_tag('r','p','h','f'),1},{sfnt_tag('p','r','e','f'),2},
    {sfnt_tag('r','k','r','f'),3},{sfnt_tag('a','b','v','f'),3},
    {sfnt_tag('b','l','w','f'),3},{sfnt_tag('h','a','l','f'),3},
    {sfnt_tag('p','s','t','f'),3},{sfnt_tag('v','a','t','u'),3},
    {sfnt_tag('c','j','c','t'),3},{sfnt_tag('i','s','o','l'),5},
    {sfnt_tag('a','b','v','s'),6},{sfnt_tag('b','l','w','s'),6},
    {sfnt_tag('c','a','l','t'),6|optional},{sfnt_tag('c','l','i','g'),6|optional},
    {sfnt_tag('h','a','l','n'),6},{sfnt_tag('l','i','g','a'),6|optional},
    {sfnt_tag('p','r','e','s'),6},{sfnt_tag('p','s','t','s'),6},
    {sfnt_tag('r','c','l','t'),6},{sfnt_tag('r','l','i','g'),6},{sfnt_tag('r','v','r','n'),6}
};
constexpr Feature positioning[]{
    {sfnt_tag('c','u','r','s'),optional},{sfnt_tag('d','i','s','t'),0},
    {sfnt_tag('k','e','r','n'),optional},{sfnt_tag('m','a','r','k'),0},
    {sfnt_tag('a','b','v','m'),0},{sfnt_tag('b','l','w','m'),0},{sfnt_tag('m','k','m','k'),0}
};
constexpr Feature khmer_substitution[]{
    {sfnt_tag('l','o','c','l'),0},{sfnt_tag('c','c','m','p'),0},
    {sfnt_tag('p','r','e','f'),0},{sfnt_tag('b','l','w','f'),0},
    {sfnt_tag('a','b','v','f'),0},{sfnt_tag('p','s','t','f'),0},
    {sfnt_tag('c','f','a','r'),1|conditional},{sfnt_tag('p','r','e','s'),1},
    {sfnt_tag('b','l','w','s'),1},{sfnt_tag('a','b','v','s'),1},
    {sfnt_tag('p','s','t','s'),1},{sfnt_tag('c','l','i','g'),1},{sfnt_tag('r','c','l','t'),1}
};
constexpr Feature khmer_positioning[]{
    {sfnt_tag('d','i','s','t'),0},{sfnt_tag('b','l','w','m'),0},
    {sfnt_tag('a','b','v','m'),0},{sfnt_tag('m','k','m','k'),0}
};
static_assert(sizeof(substitution)/sizeof(Feature)<=32&&sizeof(khmer_substitution)/sizeof(Feature)<=32);
bool spend(u64& work,u64 count) noexcept {if(count>work)return false;work-=count;return true;}
enum class Profile:u8 {sinhala,khmer,myanmar,tibetan};
// Share execution for the six-stage profiles. Only Khmer needs a different
// membership representation; script/language policy runs once before scheduling.
struct Builder {
    const SfntLayout& layout;const SfntFeatureSubstitutions& variations;bool independent;
    Slice<u8> scratch;Slice<SfntLookupStep> output;SfntLookupPlanResult& result;u64 work;bool modern;Profile profile;
    SfntStatus add(u16 index,u8 stages) noexcept {
        SfntFeature feature{};u32 tag=0,used=0;
        const auto status=sfnt_layout_feature(layout,index,variations,&tag,&feature,
            static_cast<u32>(minimum<u64>(work,0xffffffffu)),&used);
        work-=used;if(status!=SfntStatus::ok)return status;
        const auto indices=feature.lookup_indices;
        if(!spend(work,indices.count/2))return SfntStatus::resource_limit;
        for(u32 i=0;i<indices.count/2;++i)scratch[byte_load_be16(indices.data+i*2)]|=stages;
        return SfntStatus::ok;
    }
    SfntStatus run() noexcept {
        const bool khmer=profile==Profile::khmer,myanmar=profile==Profile::myanmar,tibetan=profile==Profile::tibetan;
        const bool gpos=layout.kind==SfntLayoutKind::positioning;
        const u32 first=khmer?sfnt_tag('k','h','m','r'):myanmar?(modern?sfnt_tag('m','y','m','2'):sfnt_tag('m','y','m','r')):tibetan?sfnt_tag('t','i','b','t'):sfnt_tag('s','i','n','h');
        if(myanmar&&gpos){
            // The Windows common-font gate recognizes script records, without
            // requiring a language. Legacy mymr alone does not enable GPOS.
            bool enabled=false;
            for(u32 i=0;i<layout.script_count;++i){
                if(!spend(work,1))return SfntStatus::resource_limit;
                const u32 tag=byte_load_be32(layout.scripts.data+2+i*6);
                if(tag==sfnt_tag('m','y','m','2')||tag==sfnt_tag('l','a','t','n')||tag==sfnt_tag('D','F','L','T')){enabled=true;break;}
            }
            if(!enabled)return SfntStatus::ok;
        }
        const bool fallback=myanmar||tibetan;
        const u32 scripts[]{first,fallback&&gpos?sfnt_tag('l','a','t','n'):sfnt_tag('D','F','L','T'),sfnt_tag('D','F','L','T')};SfntLanguageSelection selection{};
        auto status=sfnt_layout_select_language_prepared(layout,{scripts,fallback?(gpos?3u:2u):1u},khmer||tibetan?0:sfnt_tag('E','N','G',' '),selection,work);
        result.script_tag=selection.script_tag;
        if(status==SfntStatus::missing_table)return SfntStatus::ok;
        if(status!=SfntStatus::ok)return status;
        result.language_found=true;result.language_tag=selection.language_tag;
        if(scratch.count<layout.lookup_count)return SfntStatus::output_too_small;
        if(!spend(work,layout.lookup_count))return SfntStatus::resource_limit;
        for(u32 i=0;i<layout.lookup_count;++i)scratch[i]=0;
        const bool gsub=layout.kind==SfntLayoutKind::substitution;
        const auto features=khmer?(gsub?ConstSlice<Feature>{khmer_substitution,sizeof(khmer_substitution)/sizeof(Feature)}:
            ConstSlice<Feature>{khmer_positioning,sizeof(khmer_positioning)/sizeof(Feature)}):gsub?ConstSlice<Feature>{substitution,sizeof(substitution)/sizeof(Feature)}:
            ConstSlice<Feature>{positioning,sizeof(positioning)/sizeof(Feature)};
        const auto& language=selection.language;
        if(language.required_feature!=65535){
            status=add(language.required_feature,gsub?(khmer?3:substitution_stages):1);
            if(status!=SfntStatus::ok)return status;
        }
        u32 seen=0;
        for(u32 i=0;i<language.feature_indices.count/2;++i){
            if(!spend(work,1))return SfntStatus::resource_limit;
            const u16 index=byte_load_be16(language.feature_indices.data+i*2);
            const u32 tag=byte_load_be32(layout.features.data+2+static_cast<u32>(index)*6);
            for(u32 f=0;f<features.count;++f){
                if(!spend(work,1))return SfntStatus::resource_limit;
                if(features[f].tag!=tag)continue;
                const u32 bit=1u<<f;const u8 stage=features[f].stage;
                if(seen&bit)break;seen|=bit;
                if(index==language.required_feature||(independent&&(stage&optional)))break;
                status=add(index,static_cast<u8>((stage&conditional)?4u:1u<<(stage&63)));
                if(status!=SfntStatus::ok)return status;break;
            }
        }
        const u8 stages=gsub?(khmer?3:substitution_stages):1;
        const u8 stage_count=khmer?2:static_cast<u8>(SfntUseStage::count);
        const u64 scan=static_cast<u64>(layout.lookup_count)*(gsub?(khmer?2u:6u):1u);
        // Khmer's bit 2 is a conditional presentation membership; bit 1
        // dominates it when the same lookup is also enabled unconditionally.
        const auto members=[&](u8 stage) noexcept ->u8 {return khmer&&stage==1?6:static_cast<u8>(1u<<stage);};
        if(!spend(work,scan))return SfntStatus::resource_limit;
        for(u8 stage=0;stage<stage_count;++stage)if(stages&(1u<<stage))
            for(u32 i=0;i<layout.lookup_count;++i)if(scratch[i]&members(stage))++result.lookup_count;
        if(output.count<result.lookup_count)return SfntStatus::output_too_small;
        // Reserve the entire write phase before touching caller output.
        if(!spend(work,scan+result.lookup_count))return SfntStatus::resource_limit;
        u32 at=0;
        for(u8 stage=0;stage<stage_count;++stage)if(stages&(1u<<stage))
            for(u32 i=0;i<layout.lookup_count;++i)if(scratch[i]&members(stage))
                output[at++]={static_cast<u16>(i),static_cast<u8>(khmer&&stage==1&&!(scratch[i]&2)?sfnt_khmer_cfar_mask:0),stage};
        return SfntStatus::ok;
    }
};
SfntStatus plan(Profile profile,const SfntLayout& layout,const SfntFeatureSubstitutions& variations,bool independent,
    Slice<u8> scratch,Slice<SfntLookupStep> output,SfntLookupPlanResult* result,u64 work,bool modern=true) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    if(!layout.table.data||static_cast<u8>(layout.kind)>1||(scratch.count&&!scratch.data)||(output.count&&!output.data))return SfntStatus::invalid_input;
    Builder builder{layout,variations,independent,scratch,output,*result,work,modern,profile};
    const auto status=builder.run();result->operations=work-builder.work;return status;
}
}
SfntStatus sfnt_sinhala_lookup_plan(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,bool independent,
    Slice<u8> scratch,Slice<SfntLookupStep> output,SfntLookupPlanResult* result,u64 work) noexcept {
    return plan(Profile::sinhala,layout,variations,independent,scratch,output,result,work);
}
SfntStatus sfnt_myanmar_lookup_plan(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,bool independent,
    Slice<u8> scratch,Slice<SfntLookupStep> output,SfntLookupPlanResult* result,u64 work,bool modern) noexcept {
    return plan(Profile::myanmar,layout,variations,independent,scratch,output,result,work,modern);
}
SfntStatus sfnt_khmer_lookup_plan(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,
    Slice<u8> scratch,Slice<SfntLookupStep> output,SfntLookupPlanResult* result,u64 work) noexcept {
    return plan(Profile::khmer,layout,variations,false,scratch,output,result,work);
}
SfntStatus sfnt_tibetan_lookup_plan(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,bool independent,
    Slice<u8> scratch,Slice<SfntLookupStep> output,SfntLookupPlanResult* result,u64 work) noexcept {
    return plan(Profile::tibetan,layout,variations,independent,scratch,output,result,work);
}
}
