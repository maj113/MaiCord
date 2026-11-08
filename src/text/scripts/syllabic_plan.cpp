#include "maicord/text/scripts/syllabic_shape.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/memory.hpp"
#include "../opentype/layout_language.hpp"
#include "maicord/text/base.hpp"
namespace maicord {namespace {
// One feature of a shaper's plan: its stage, its mask (zero: every glyph) and whether HarfBuzz applies it within
// each syllable (F_PER_SYLLABLE) or turns it off for independent letters (liga, clig, kern).
struct Feature {u32 tag;u8 stage,mask,flags;};
constexpr u8 syllable=1,optional=2;
constexpr u32 tag(const char (&t)[5]) noexcept {return sfnt_tag(t[0],t[1],t[2],t[3]);}
// The presentation stage's global features from hb-ot-shape.cc (common and horizontal features) that each shaper
// keeps: rlig, calt, clig, liga and rclt; independent letters turn off clig and liga.
#define MAICORD_GLOBALS(stage) {tag("rlig"),stage,0,0},{tag("calt"),stage,0,0},{tag("clig"),stage,0,optional},{tag("rclt"),stage,0,0}
// hb-ot-shaper-khmer: rvrn; the basic features in one stage after the reordering; the presentation features. Its
// override_features runs after the user's features: clig is always on (even for independent letters), liga off.
constexpr Feature khmer[]{
    {tag("rvrn"),0,0,0},
    {tag("locl"),1,0,syllable},{tag("ccmp"),1,0,syllable},{tag("pref"),1,sfnt_khmer_pref,syllable},
    {tag("blwf"),1,sfnt_khmer_post,syllable},{tag("abvf"),1,sfnt_khmer_post,syllable},
    {tag("pstf"),1,sfnt_khmer_post,syllable},{tag("cfar"),1,sfnt_khmer_cfar,syllable},
    {tag("pres"),2,0,0},{tag("abvs"),2,0,0},{tag("blws"),2,0,0},{tag("psts"),2,0,0},
    {tag("rlig"),2,0,0},{tag("calt"),2,0,0},{tag("clig"),2,0,0},{tag("rclt"),2,0,0}};
// hb-ot-shaper-myanmar: rvrn; locl and ccmp; rphf, pref, blwf and pstf one stage each; the presentation features.
constexpr Feature myanmar[]{
    {tag("rvrn"),0,0,0},{tag("locl"),1,0,syllable},{tag("ccmp"),1,0,syllable},
    {tag("rphf"),2,0,syllable},{tag("pref"),3,0,syllable},{tag("blwf"),4,0,syllable},{tag("pstf"),5,0,syllable},
    {tag("pres"),6,0,0},{tag("abvs"),6,0,0},{tag("blws"),6,0,0},{tag("psts"),6,0,0},MAICORD_GLOBALS(6),{tag("liga"),6,0,optional}};
// hb-ot-shaper-use: rvrn; the default glyph pre-processing group; rphf; pref; the orthographic unit group; the
// topographical forms (by cluster, across clusters); the standard typographic presentation.
constexpr Feature use[]{
    {tag("rvrn"),0,0,0},
    {tag("locl"),1,0,syllable},{tag("ccmp"),1,0,syllable},{tag("nukt"),1,0,syllable},{tag("akhn"),1,0,syllable},
    {tag("rphf"),2,sfnt_use_rphf,syllable},{tag("pref"),3,0,syllable},
    {tag("rkrf"),4,0,syllable},{tag("abvf"),4,0,syllable},{tag("blwf"),4,0,syllable},{tag("half"),4,0,syllable},
    {tag("pstf"),4,0,syllable},{tag("vatu"),4,0,syllable},{tag("cjct"),4,0,syllable},
    {tag("isol"),5,sfnt_use_isol,0},{tag("init"),5,sfnt_use_init,0},{tag("medi"),5,sfnt_use_medi,0},{tag("fina"),5,sfnt_use_fina,0},
    {tag("abvs"),6,0,0},{tag("blws"),6,0,0},{tag("haln"),6,0,0},{tag("pres"),6,0,0},{tag("psts"),6,0,0},
    MAICORD_GLOBALS(6),{tag("liga"),6,0,optional}};
#undef MAICORD_GLOBALS
// hb-ot-shaper-hangul: rvrn; ljmo, vjmo, tjmo and calt masked, with the default features (hb-ot-shape.cc).
constexpr Feature hangul[]{
    {tag("rvrn"),0,0,0},{tag("ljmo"),1,sfnt_hangul_ljmo,0},{tag("vjmo"),1,sfnt_hangul_vjmo,0},{tag("tjmo"),1,sfnt_hangul_tjmo,0},
    {tag("calt"),1,sfnt_hangul_calt,0},{tag("rand"),1,0,0},{tag("trak"),1,0,0},{tag("abvm"),1,0,0},{tag("blwm"),1,0,0},
    {tag("ccmp"),1,0,0},{tag("locl"),1,0,0},{tag("mark"),1,0,0},{tag("mkmk"),1,0,0},{tag("rlig"),1,0,0},
    {tag("clig"),1,0,optional},{tag("curs"),1,0,0},{tag("dist"),1,0,0},{tag("kern"),1,0,optional},{tag("liga"),1,0,optional},
    {tag("rclt"),1,0,0}};
// GPOS: the common and horizontal positioning features every shaper keeps.
constexpr Feature positioning[]{
    {tag("kern"),0,0,optional},{tag("dist"),0,0,0},{tag("abvm"),0,0,0},{tag("blwm"),0,0,0},{tag("mark"),0,0,0},
    {tag("mkmk"),0,0,0},{tag("curs"),0,0,0}};
static_assert(sizeof(use)/sizeof(Feature)<=32);
constexpr SfntJoinerFeature khmer_manual[]{{tag("pref"),3},{tag("blwf"),3},{tag("abvf"),3},{tag("pstf"),3},{tag("cfar"),3},
    {tag("pres"),3},{tag("abvs"),3},{tag("blws"),3},{tag("psts"),3}};
constexpr SfntJoinerFeature myanmar_manual[]{{tag("rphf"),1},{tag("pref"),1},{tag("blwf"),1},{tag("pstf"),1},
    {tag("pres"),1},{tag("abvs"),1},{tag("blws"),1},{tag("psts"),1}};
constexpr SfntJoinerFeature use_manual[]{{tag("akhn"),1},{tag("rphf"),1},{tag("pref"),1},{tag("rkrf"),1},{tag("abvf"),1},
    {tag("blwf"),1},{tag("half"),1},{tag("pstf"),1},{tag("vatu"),1},{tag("cjct"),1},{tag("abvs"),1},{tag("blws"),1},
    {tag("haln"),1},{tag("pres"),1},{tag("psts"),1}};
template<class T,u32 N> constexpr ConstSlice<T> all(const T (&values)[N]) noexcept {return {values,N};}
constexpr u16 unmasked=256,global=512;
struct Selected {SfntFeature feature;u8 stage,mask;bool global;};
struct Builder {
    const SfntLayout& layout;const SfntFeatureSubstitutions& variations;ConstSlice<Feature> features;bool independent;
    Slice<u16> scratch;Slice<SfntLookupStep> output;SfntLookupPlanResult& result;u64 work;Slice<u64> per_syllable;
    Selected selected[32]{};u32 count=0;u8 stages=1;
    SfntStatus add(u16 index,u8 stage,u8 mask,bool all_syllables) noexcept {
        if(count==32)return SfntStatus::resource_limit;u32 found=0,used=0;auto& entry=selected[count];
        const auto status=sfnt_layout_feature(layout,index,variations,&found,&entry.feature,static_cast<u32>(minimum<u64>(work,0xffffffffu)),&used);
        work-=used;if(status!=SfntStatus::ok)return status;entry.stage=stage;entry.mask=mask;entry.global=all_syllables;++count;return SfntStatus::ok;
    }
    SfntStatus pass(bool write) noexcept {
        u32 emitted=0;
        if(write)for(u32 i=0;i<per_syllable.count;++i)per_syllable[i]=0;
        for(u8 stage=0;stage<stages;++stage){
            if(!spend(&work,layout.lookup_count))return SfntStatus::resource_limit;
            for(u32 i=0;i<layout.lookup_count;++i)scratch[i]=0;
            for(u32 f=0;f<count;++f){
                if(!spend(&work))return SfntStatus::resource_limit;const auto& s=selected[f];if(s.stage!=stage)continue;
                const auto indices=s.feature.lookup_indices;if(!spend(&work,indices.count/2))return SfntStatus::resource_limit;
                for(u32 i=0;i<indices.count/2;++i){const u16 lookup=byte_load_be16(indices.data+i*2);
                    if(lookup<layout.lookup_count)scratch[lookup]|=(s.mask?s.mask:unmasked)|(s.global?global:0);}
            }
            if(!spend(&work,layout.lookup_count))return SfntStatus::resource_limit;
            for(u32 i=0;i<layout.lookup_count;++i)if(scratch[i]){
                if(write){if(!spend(&work))return SfntStatus::resource_limit;
                    output[emitted]={static_cast<u16>(i),static_cast<u8>(scratch[i]&unmasked?0:scratch[i]),stage};
                    if(per_syllable.count&&!(scratch[i]&global))per_syllable[emitted>>6]|=1ull<<(emitted&63);}
                ++emitted;
            }
        }
        if(!write)result.lookup_count=emitted;return SfntStatus::ok;
    }
    SfntStatus run(ConstSlice<u32> scripts) noexcept {
        SfntLanguageSelection selection{};
        auto status=sfnt_layout_select_language_prepared(layout,scripts,sfnt_tag('E','N','G',' '),selection,work);result.script_tag=selection.script_tag;
        if(status==SfntStatus::missing_table)return SfntStatus::ok;if(status!=SfntStatus::ok)return status;
        result.language_found=true;result.language_tag=selection.language_tag;const auto& language=selection.language;
        if(scratch.count<layout.lookup_count)return SfntStatus::output_too_small;
        for(u32 i=0;i<features.count;++i)stages=maximum<u8>(stages,static_cast<u8>(features[i].stage+1));
        const auto tag_of=[&](u16 index) noexcept {return byte_load_be32(layout.features.data+2+static_cast<u32>(index)*6);};
        const auto find=[&](u32 wanted) noexcept {for(u32 i=0;i<features.count;++i)if(features[i].tag==wanted)return i;return features.count;};
        // The required feature applies to every glyph across syllables, in its tag's stage (the first for other tags).
        if(language.required_feature!=65535){const u32 at=find(tag_of(language.required_feature));
            status=add(language.required_feature,at<features.count?features[at].stage:0,0,true);if(status!=SfntStatus::ok)return status;}
        u32 seen=0;
        for(u32 i=0;i<language.feature_indices.count/2;++i){
            if(!spend(&work))return SfntStatus::resource_limit;const u16 index=byte_load_be16(language.feature_indices.data+i*2);
            if(index==language.required_feature)continue;
            const u32 at=find(tag_of(index));if(at==features.count||(seen>>at&1u))continue;seen|=1u<<at;
            const auto& f=features[at];if(independent&&(f.flags&optional))continue;
            status=add(index,f.stage,f.mask,!(f.flags&syllable));if(status!=SfntStatus::ok)return status;
        }
        const u64 before=work;status=pass(false);if(status!=SfntStatus::ok)return status;
        if(output.count<result.lookup_count||(per_syllable.count&&per_syllable.count<(result.lookup_count+63)/64))return SfntStatus::output_too_small;
        if(work<before-work+result.lookup_count)return SfntStatus::resource_limit;
        return pass(true);
    }
};
}
ConstSlice<SfntJoinerFeature> sfnt_syllabic_manual_joiners(SfntSyllabicScript script) noexcept {
    switch(script){case SfntSyllabicScript::khmer:return all(khmer_manual);case SfntSyllabicScript::myanmar:return all(myanmar_manual);default:return all(use_manual);}
}
SfntStatus sfnt_syllabic_lookup_plan(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,SfntSyllabicScript script,bool independent,
    Slice<u16> scratch,Slice<SfntLookupStep> output,SfntLookupPlanResult* result,u64 work,Slice<u64> per_syllable) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    if(static_cast<u8>(script)>3||!layout.table.data||static_cast<u8>(layout.kind)>1||(scratch.count&&!scratch.data)||(output.count&&!output.data)||
       (per_syllable.count&&!per_syllable.data))return SfntStatus::invalid_input;
    // HarfBuzz's script tags for the script (hb_ot_tags_from_script_and_language), then its fallbacks.
    const u32 first=script==SfntSyllabicScript::khmer?tag("khmr"):script==SfntSyllabicScript::myanmar?tag("mym2"):
        script==SfntSyllabicScript::tibetan?tag("tibt"):tag("sinh");
    const bool two=script==SfntSyllabicScript::myanmar;
    const u32 scripts[]{first,two?tag("mymr"):tag("DFLT"),two?tag("DFLT"):tag("dflt"),two?tag("dflt"):tag("latn"),tag("latn")};
    const auto features=layout.kind==SfntLayoutKind::positioning?all(positioning):
        script==SfntSyllabicScript::khmer?all(khmer):script==SfntSyllabicScript::myanmar?all(myanmar):all(use);
    Builder builder{layout,variations,features,independent,scratch,output,*result,work,per_syllable};
    const auto status=builder.run({scripts,two?5u:4u});result->operations=work-builder.work;return status;
}
SfntStatus sfnt_hangul_lookup_plan(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,bool independent,
    Slice<u16> scratch,Slice<SfntLookupStep> output,SfntLookupPlanResult* result,u64 work) noexcept {
    if(!result)return SfntStatus::invalid_input;reset_value(result);
    if(!layout.table.data||static_cast<u8>(layout.kind)>1||(scratch.count&&!scratch.data)||(output.count&&!output.data))return SfntStatus::invalid_input;
    const u32 scripts[]{tag("hang"),tag("DFLT"),tag("dflt"),tag("latn")};
    Builder builder{layout,variations,all(hangul),independent,scratch,output,*result,work,{}};
    const auto status=builder.run({scripts,4});result->operations=work-builder.work;return status;
}
}
