#include "maicord/text/indic_input.hpp"
#include "maicord/text/unicode.hpp"
#include "indic_cluster.hpp"
#include "indic_forms.hpp"
namespace maicord {
SfntStatus indic_scalar_cluster(SfntIndicProfile profile,const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<u32> input,ConstSlice<u8> font_forms,
    float em,SfntShapeStorage storage,SfntShapeRunResult* result,ConstSlice<SfntLookupStep> steps,SfntShapeLimits limits,
    bool word_initial,bool followed_by_text,float trailing_spacing,bool rtl,bool defer_output) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    if(font_forms.count!=128||!font_forms.data)return SfntStatus::invalid_input;
    if(input.count>31)return SfntStatus::unsupported_sequence;
    // Bengali admits one composite vowel; Odia can admit two in one cluster.
    // Gujarati/Gurmukhi signs stay nominal through this same source-preserving path.
    // Original grouping is kept when expansion would otherwise change parsing.
    // Position scratch is unused until after mapping, so expansion borrows it.
    if(storage.positions.count&&!storage.positions.data)return SfntStatus::invalid_input;
    auto* expanded=reinterpret_cast<SfntIndicInputScalar*>(storage.positions.data);SfntIndicInputResult sizes{};
    const u32 capacity=static_cast<u32>(minimum<u64>(indic_cluster_capacity,static_cast<u64>(storage.positions.count)*sizeof(SfntPosition)/sizeof(SfntIndicInputScalar)));
    // Odia/Telugu/Kannada reserve a slot for the dotted circle before spending the
    // remaining preparation slots on split vowels.
    const u32 extra=(profile==SfntIndicProfile::odia||profile==SfntIndicProfile::telugu||profile==SfntIndicProfile::kannada||profile==SfntIndicProfile::malayalam)?31-input.count-static_cast<u32>(input.count&&input.count<31&&input.data&&input[0]==0x200d):0xffffffffu;
    auto status=sfnt_indic_expand_cluster(profile,input,{expanded,capacity},&sizes,limits.stage_work,extra);if(status!=SfntStatus::ok)return status;
    const u32 block=sfnt_indic_script(profile)->block_first;
    u32 scalars[indic_cluster_capacity]{};u8 forms[indic_cluster_capacity]{};SfntClusterGlyph mapped[indic_cluster_capacity]{};
    for(u32 i=0;i<sizes.scalar_count;++i){const u32 scalar=expanded[i].scalar;scalars[i]=scalar;u16 glyph=0;
        status=sfnt_cmap_glyph(cmap,rtl?unicode_bidi_mirror(scalar):scalar,0,&glyph);if(status!=SfntStatus::ok)return status;
        forms[i]=scalar>=block&&scalar<block+128?font_forms[scalar-block]:0;
        mapped[i]={{glyph,sfnt_gdef_glyph_class(plan.gdef,glyph),0},expanded[i].source};}
    if(profile==SfntIndicProfile::malayalam&&sizes.scalar_count&&(font_forms[0x2f]&indic_default_position))forms[0]|=indic_default_model;
    return indic_shape_cluster(profile,plan,cmap,{mapped,sizes.scalar_count},{scalars,sizes.scalar_count},{forms,sizes.scalar_count},
        sizes.source_units,em,storage,result,steps,limits,followed_by_text,trailing_spacing,rtl,word_initial,defer_output);
}
}
