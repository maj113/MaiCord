#include "maicord/text/latin_run.hpp"
#include "nominal_placements.hpp"
#include "maicord/text/alphabetic_run.hpp"
#include "maicord/text/lookup_plan.hpp"
#include "maicord/text/emoji_run.hpp"
#include "maicord/text/hebrew_shape_run.hpp"
#include "maicord/text/nominal_sources.hpp"
#include "maicord/text/grapheme.hpp"
#include "maicord/core/utf.hpp"
#include <emmintrin.h>
namespace maicord {namespace {
#include "generated/alphabetic_composition_data.inc"
u32 compose(u32 first,u32 last) noexcept {
    if((first|last)>65535)return 0;const u32 key=(first<<16)|last;u32 lo=0,hi=sizeof(alphabetic_composition_keys)/sizeof(u32);
    while(lo<hi){const u32 mid=lo+(hi-lo)/2;if(alphabetic_composition_keys[mid]<key)lo=mid+1;else hi=mid;}
    return lo<sizeof(alphabetic_composition_keys)/sizeof(u32)&&alphabetic_composition_keys[lo]==key?alphabetic_composition_values[lo]:0;
}
bool attachment(u32 cp) noexcept {if(cp<0x300)return false;const auto* p=unicode_properties(cp);return p->combining_class()!=0&&!p->has(UnicodeFlag::default_ignorable_code_point);}
bool lao_consonant_range(u32 cp) noexcept {return cp-0xe81u<46||cp-0xedcu<4;}
u8 thai_shape_class(u32 cp) noexcept {
    if(cp-0xe01u<46)return 2;
    if(cp==0xe30||cp==0xe32||cp-0xe40u<6)return 1;
    if(cp==0xe31||cp-0xe34u<4||cp-0xe47u<8)return 5;
    if(cp-0xe38u<2)return 3;
    if(cp==0xe3a)return 4;
    if(cp==0xe33)return 6;
    if(cp-0x200cu<2)return 7;
    if(cp==0x2d||cp-0x2012u<4||cp==0xd7||cp==0xa0||cp==0x2022||cp==0x25cc||cp-0x25fbu<4)return 2;
    return 0;
}
struct Controls {const SfntGdef* gdef;const u8* sources;u32 units;u16 space;bool expansion_attachments;};
// Preserve the origin of inserted dotted circles in existing component padding.
// A missing synthetic base is permitted; a missing user character is not.
SfntStatus remember_synthetic_bases(void*,SfntGlyphBuffer* buffer,SfntClusterMap*,SfntComponentForest* forest,SfntGsubExecutionScratch,u64 work) noexcept {
    const auto view=sfnt_glyph_buffer_view(*buffer);const u32 count=sfnt_glyph_count(view);
    if(count>work)return SfntStatus::resource_limit;if(count!=forest->nodes.count)return SfntStatus::invalid_input;
    for(u32 i=0;i<count;++i){auto& glyph=const_cast<SfntGsubGlyph&>(sfnt_glyph_at(view,i));
        forest->nodes[i].reserved=static_cast<u16>((glyph.reserved&1)?0x100:0);glyph.reserved=0;}
    return SfntStatus::ok;
}
SfntStatus finish_controls(void* opaque,Slice<SfntComponentGlyph> glyphs,Slice<u8> zero,u64 work) noexcept {
    const auto& p=*static_cast<const Controls*>(opaque);if(glyphs.count>work)return SfntStatus::resource_limit;if(zero.count<glyphs.count)return SfntStatus::output_too_small;
    u32 previous=0xffffffffu;
    for(u32 i=0;i<glyphs.count;++i){auto& g=glyphs[i];if(g.source>=p.units)return SfntStatus::invalid_input;const bool control=p.sources&&p.sources[g.source]==1;
        if(control)g.glyph.glyph=p.space;g.glyph.glyph_class=sfnt_gdef_glyph_class(*p.gdef,g.glyph.glyph);const bool mark=g.glyph.glyph_class==SfntGlyphClass::mark;
        if(p.expansion_attachments&&mark&&g.component==0xffffffffu)g.attachment=previous;
        if(!mark)previous=g.component;zero[i]=static_cast<u8>(control||mark);}
    return SfntStatus::ok;
}
struct EgyptianControls {Controls common;u16 glyphs[4];};
// USE suppresses matching control glyph IDs, including absent mappings to zero.
SfntStatus finish_egyptian_controls(void* opaque,Slice<SfntComponentGlyph> glyphs,Slice<u8> zero,u64 work) noexcept {
    auto& p=*static_cast<EgyptianControls*>(opaque);if(glyphs.count>work/2)return SfntStatus::resource_limit;
    const auto status=finish_controls(&p.common,glyphs,zero,work-glyphs.count);if(status!=SfntStatus::ok)return status;
    for(u32 i=0;i<glyphs.count;++i){auto& g=glyphs[i];const auto id=g.glyph.glyph;
        if(id==p.glyphs[0]||id==p.glyphs[1]||id==p.glyphs[2]||id==p.glyphs[3]){
            g.glyph.glyph=p.common.space;g.glyph.glyph_class=sfnt_gdef_glyph_class(*p.common.gdef,p.common.space);zero[i]=1;}}
    return SfntStatus::ok;
}
SfntStatus alphabetic_run(const NativeRunPlanStore& store,NativeRunPlanHandle handle,Utf8View text,bool independent,float em,const NativeAsciiStorage& s,NativeAsciiResult* out,SfntShapeLimits limits,bool require_source_coverage,const NativeAlphabeticPlan* profile,bool force_shaping,bool rtl=false,SfntShapeRunResult* raw_output=nullptr,bool khmer_fallback=false) noexcept {
    if(!out)return SfntStatus::invalid_input;*out={};u32 em_bits=0;copy_bytes(&em_bits,&em,4);
    const auto* plan=native_run_plan_get(store,handle,independent);
    if(!plan||(text.bytes&&!text.data)||!em_bits||em_bits>=0x7f800000u||em>65536||
       (s.mapped.count&&!s.mapped.data)||(s.shaping.components.count&&!s.shaping.components.data)||(s.shaping.targets.count&&!s.shaping.targets.data)||
       (s.glyphs.count&&!s.glyphs.data)||(s.placements.count&&!s.placements.data)||(s.character_glyphs.count&&!s.character_glyphs.data))return SfntStatus::invalid_input;
    if(text.bytes>limits.stage_work)return SfntStatus::resource_limit;
    const auto* instance=native_font_instance_get(*store.instances,store.slots[handle.slot-1].instance);if(!instance)return SfntStatus::invalid_input;
    const auto* font=native_font_get(*store.instances->fonts,instance->face);if(!font)return SfntStatus::invalid_input;
    auto* scalars=reinterpret_cast<u32*>(s.shaping.components.data);const u64 capacity=static_cast<u64>(s.shaping.components.count)*sizeof(SfntComponentNode)/sizeof(u32);
    const bool jamo=profile&&profile->script_tag==sfnt_tag('j','a','m','o');
    const bool korean=jamo||(profile&&profile->script_tag==sfnt_tag('h','a','n','g'));
    const bool egyptian=profile&&profile->script_tag==sfnt_tag('e','g','y','p');
    const bool thai_script=profile&&profile->script_tag==sfnt_tag('t','h','a','i');
    const bool thai=thai_script||(profile&&profile->script_tag==sfnt_tag('l','a','o',' '));
    bool sara_am=false,thai_below=false,thai_base=false;u8 thai_base_class=0,thai_previous_class=0;
    // Temporary input metadata lives in the mapped glyph padding. It is cleared
    // before GSUB; only synthetic-base provenance survives in component nodes.
    u32 count=0,units=0,boundary_count=0;bool controls=false,tones=false,synthetic_bases=false;UnicodeGraphemeState graphemes{};
    for(u32 at=0;at<text.bytes;){const auto cp=utf8_decode_one(text.data+at,text.bytes-at);if(!cp.valid)return SfntStatus::invalid_input;
        if(count>=capacity||count>=s.mapped.count)return SfntStatus::output_too_small;
        bool boundary=false;if(!unicode_grapheme_next(&graphemes,cp.codepoint,&boundary))return SfntStatus::invalid_input;
        // The rejected-font generic provider does not apply Khmer conjunct
        // joining. This changes shaping groups only, not editor graphemes.
        if(khmer_fallback&&cp.codepoint>=0x1780&&cp.codepoint<=0x17b3)boundary=true;
        bool invalid_thai=false;
        // A synthetic dotted circle does not make a missing consonant valid
        // for the next source mark (notably across a font cut before Sara Am).
        if(thai_script){
            u8 kind=thai_shape_class(cp.codepoint);
            if(!count)thai_base_class=thai_previous_class=kind;
            if(kind<=2){thai_base_class=kind;boundary=true;}
            else if(kind==3){
                invalid_thai=thai_base_class!=2||thai_previous_class==5;
                if(!invalid_thai&&thai_previous_class==4)kind=4;
            }else if(kind==4)invalid_thai=thai_base_class==0||thai_base_class==8||thai_previous_class==4||thai_previous_class==5;
            else if(kind==5)invalid_thai=thai_base_class!=2;
            else if(kind==6){invalid_thai=thai_base_class!=2;thai_base_class=6;}
            // Synthetic circles never become a valid base. Two class bytes
            // retain the reference's validity state; existing mapped metadata
            // handles lower-mark/phinthu reordering and Sara Am expansion.
            if(invalid_thai){boundary=true;thai_base_class=8;}
            thai_previous_class=kind;
        }else if(thai){const auto category=unicode_properties(cp.codepoint)->general_category();
            const bool mark=category==UnicodeGeneralCategory::mn||category==UnicodeGeneralCategory::mc||category==UnicodeGeneralCategory::me||cp.codepoint==0xe33||cp.codepoint==0xeb3;
            if(mark){invalid_thai=!thai_base||(cp.codepoint==0xebc&&count&&!lao_consonant_range(scalars[count-1])&&scalars[count-1]!=0x200d);if(invalid_thai){boundary=true;thai_base=false;}}
            else if(cp.codepoint!=0x200c&&cp.codepoint!=0x200d)thai_base=cp.codepoint==0x25cc||(cp.codepoint>=0xe01&&cp.codepoint<=0xe2e)||(lao_consonant_range(cp.codepoint)&&category==UnicodeGeneralCategory::lo);
        }
        const bool repeated_tone=korean&&(cp.codepoint==0x302e||cp.codepoint==0x302f)&&count&&(scalars[count-1]==0x302e||scalars[count-1]==0x302f);
        const bool jamo_tone=korean&&!jamo&&(cp.codepoint==0x302e||cp.codepoint==0x302f)&&count&&
            ((scalars[count-1]>=0x1100&&scalars[count-1]<=0x11ff)||(scalars[count-1]>=0x3130&&scalars[count-1]<=0x318f)||(scalars[count-1]>=0xffa0&&scalars[count-1]<=0xffdc));
        if(repeated_tone||jamo_tone)boundary=true;
        // Windows keeps halfwidth kana sound marks as separate initial shape
        // clusters. Font ligatures can still merge them during GSUB. This is
        // a script shaping policy; Unicode grapheme/editor boundaries stay
        // independent and must not be changed to obtain this cluster map.
        if((cp.codepoint==0x200c&&!thai)||(egyptian&&cp.codepoint==0x200d)||(profile&&profile->script_tag==sfnt_tag('k','a','n','a')&&
            (cp.codepoint==0xff9e||cp.codepoint==0xff9f)))boundary=true;
        // The app's current en-US Hangul path retains scalar shape boundaries,
        // including conjoining Jamo. Grapheme/editor boundaries stay separate.
        if(profile&&profile->script_tag==sfnt_tag('h','a','n','g')&&cp.codepoint!=0x302e&&cp.codepoint!=0x302f&&unicode_properties(cp.codepoint)->script()==UnicodeScript::hang)boundary=true;
        if(boundary){if(boundary_count==s.shaping.targets.count)return SfntStatus::output_too_small;s.shaping.targets[boundary_count++]=units;}
        // Unicode 17 selectors begin with E1, EF or F3 in UTF-8. Ordinary
        // text avoids a second decode/property lookup; the property still
        // validates the complete candidate before consuming a selector.
        const u8 lead=at+cp.width<text.bytes?static_cast<u8>(text.data[at+cp.width]):0;
        const auto next=(lead==0xe1||lead==0xef||lead==0xf3)?utf8_decode_one(text.data+at+cp.width,text.bytes-at-cp.width):Utf8DecodeResult{};
        const bool variation=next.valid&&unicode_properties(next.codepoint)->has(UnicodeFlag::variation_selector)&&!unicode_properties(cp.codepoint)->has(UnicodeFlag::variation_selector);
        // The not-yet-mapped glyph field temporarily holds the selector. Its
        // compact code covers both BMP and supplementary selectors without
        // adding storage. UTF-16 sources include both consumed scalars.
        if((korean||thai)&&(!count||repeated_tone||jamo_tone||invalid_thai)){const auto* properties=unicode_properties(cp.codepoint);const auto category=properties->general_category();
            if((category==UnicodeGeneralCategory::mn||category==UnicodeGeneralCategory::mc||category==UnicodeGeneralCategory::me||(thai&&(cp.codepoint==0xe33||cp.codepoint==0xeb3)))&&!properties->has(UnicodeFlag::default_ignorable_code_point)){
                if(count+1>=capacity||count+1>=s.mapped.count)return SfntStatus::output_too_small;
                scalars[count]=0x25cc;s.mapped[count].source=units;s.mapped[count].glyph.reserved=1;s.mapped[count++].glyph.glyph=0;synthetic_bases=true;}}
        const bool am=thai&&(cp.codepoint==0x0e33||cp.codepoint==0x0eb3);
        if(am){if(count+1>=capacity||count+1>=s.mapped.count)return SfntStatus::output_too_small;
            if(require_source_coverage){u16 original=0;const auto status=sfnt_cmap_glyph(font->cmap,cp.codepoint,0,&original);if(status!=SfntStatus::ok)return status;if(!original)return SfntStatus::unsupported_sequence;}
            scalars[count]=cp.codepoint==0x0e33?0x0e4d:0x0ecd;s.mapped[count++]={{0,SfntGlyphClass{},2},units};sara_am=true;}
        scalars[count]=am?cp.codepoint-1:cp.codepoint;s.mapped[count].source=units;if(korean||thai)s.mapped[count].glyph.reserved=0;
        if(thai){const u32 cpmark=cp.codepoint&~0x80u;if(cpmark==0xe31||(cpmark>=0xe34&&cpmark<=0xe37)||(cpmark>=0xe47&&cpmark<=0xe4e)||cp.codepoint==0xebb)s.mapped[count].glyph.reserved=4;
            if(cp.codepoint==0xe3a){s.mapped[count].glyph.reserved=8;thai_below=true;}else if(cp.codepoint==0xe38||cp.codepoint==0xe39)s.mapped[count].glyph.reserved=16;}
        s.mapped[count].glyph.glyph=variation?static_cast<u16>(next.codepoint>65535?next.codepoint-0xe0000:next.codepoint):0;++count;
        units+=(cp.codepoint>65535?2u:1u)+(variation?(next.codepoint>65535?2u:1u):0u);at+=cp.width+(variation?next.width:0u);
        if(variation&&!unicode_grapheme_next(&graphemes,next.codepoint,&boundary))return SfntStatus::invalid_input;
        if(cp.codepoint>=0xad&&!(jamo&&(cp.codepoint==0x115f||cp.codepoint==0x1160)))controls|=unicode_properties(cp.codepoint)->has(UnicodeFlag::default_ignorable_code_point);
        tones|=korean&&(cp.codepoint==0x302e||cp.codepoint==0x302f);
    }
    if(units>s.character_glyphs.count)return SfntStatus::output_too_small;if(!count)return SfntStatus::ok;
    auto* control_sources=reinterpret_cast<u8*>(s.shaping.advances.data);
    if(controls||tones){if(!control_sources||static_cast<u64>(s.shaping.advances.count)*sizeof(double)<units)return SfntStatus::output_too_small;for(u32 i=0;i<units;++i)control_sources[i]=0;}
    const bool hebrew=profile&&profile->hebrew_preparation;
    const auto nominal=native_run_plan_nominal(store,handle);if(!nominal.glyph_limit)return SfntStatus::invalid_input;bool simple=!independent&&!force_shaping&&!hebrew;u32 written=0;
    // Reuse the plan's validated affine ASCII cmap prefix inside Latin text.
    // Accented scalars and fonts without that prefix keep ordinary cmap lookup.
    const auto map=[&](u32 scalar,u16* glyph,u32 selector=0) noexcept {
        if(rtl)scalar=unicode_bidi_mirror(scalar);
        if(selector&&selector<0x200)selector+=0xe0000;
        const u32 offset=scalar-32u;if(!selector&&offset<nominal.ascii_count){*glyph=static_cast<u16>(nominal.ascii_first+offset);return SfntStatus::ok;}
        const auto status=sfnt_cmap_glyph(font->cmap,scalar,selector,glyph);
        return status==SfntStatus::unsupported_sequence&&selector?sfnt_cmap_glyph(font->cmap,scalar,0,glyph):status;
    };
    const bool greek=profile&&profile->script_tag==sfnt_tag('g','r','e','k');
    for(u32 i=0;i<count;){u32 end=i+1;while(!hebrew&&end<count&&(!greek||end-i<4)&&attachment(scalars[end]))++end;
        u32 candidate=scalars[i];
        for(u32 j=i;candidate&&j<end;++j){
            if(s.mapped[j].glyph.glyph){u16 selected=0,base_glyph=0;
                auto status=map(scalars[j],&selected,s.mapped[j].glyph.glyph);if(status!=SfntStatus::ok)return status;
                status=map(scalars[j],&base_glyph);if(status!=SfntStatus::ok)return status;if(selected!=base_glyph)candidate=0;}
            if(j!=i&&candidate)candidate=compose(candidate,scalars[j]);
        }
        u16 composed=0;if(end>i+1&&candidate){const auto status=map(candidate,&composed);if(status!=SfntStatus::ok)return status;}
        if(composed&&require_source_coverage)for(u32 j=i;j<end;++j){u16 original=0;const auto status=map(scalars[j],&original);
            if(status!=SfntStatus::ok)return status;if(!original)return SfntStatus::unsupported_sequence;}
        const u32 limit=composed?i+1:end;
        for(u32 j=i;j<limit;++j){u16 glyph=composed;const u32 cp=composed?candidate:scalars[j];const u32 source=s.mapped[j].source;
            const u8 metadata=korean||thai?s.mapped[j].glyph.reserved:0;
            if(!composed){const auto status=map(cp,&glyph,s.mapped[j].glyph.glyph);if(status!=SfntStatus::ok)return status;}
            const u32 flags=cp>=0xad?unicode_properties(cp)->flags():0;
            constexpr u32 selector=static_cast<u32>(UnicodeFlag::variation_selector),ignorable=static_cast<u32>(UnicodeFlag::default_ignorable_code_point);
            if(require_source_coverage&&!glyph&&!(metadata&1)&&!(flags&selector))return SfntStatus::unsupported_sequence;
            const bool control=(flags&(ignorable|selector))==ignorable&&!(jamo&&(cp==0x115f||cp==0x1160))&&!(khmer_fallback&&(cp==0x17b4||cp==0x17b5));if(controls&&control)control_sources[source]=1;
            if(tones&&(cp==0x302e||cp==0x302f))control_sources[source]=2;
            simple=simple&&!composed&&!control&&glyph&&nominal_scalar_is_simple(cp)&&(!nominal.participation.count||!(nominal.participation[glyph/64]&(1ull<<(glyph%64))));
            s.mapped[written++]={{glyph,sfnt_gdef_glyph_class(plan->gdef,glyph),metadata},source};
        }i=end;
    }
    if(simple){if(written>s.glyphs.count||written>s.placements.count)return SfntStatus::output_too_small;
        const auto scale=_mm_div_ss(_mm_set_ss(em),_mm_set_ss(static_cast<float>(plan->units_per_em)));
        if(plan->cache_advances){const auto status=nominal_placements(*plan,{s.mapped.data,written},s.glyphs,s.placements,s.shaping.targets,scale);if(status!=SfntStatus::ok)return status;
            for(u32 i=0;i<written;++i){const auto& g=s.mapped[i];
                const u32 end=i+1<written?s.mapped[i+1].source:units;for(u32 source=g.source;source<end;++source)s.character_glyphs[source]=i;}
        }else for(u32 i=0;i<written;++i){const auto& g=s.mapped[i];double advance=0;const auto status=plan->advance(plan->metric_context,g.glyph.glyph,&advance);if(status!=SfntStatus::ok)return status;
            s.glyphs[i]=g.glyph.glyph;s.placements[i]={_mm_cvtss_f32(_mm_mul_ss(_mm_set_ss(static_cast<float>(advance)),scale)),0,0};
            const u32 end=i+1<written?s.mapped[i+1].source:units;for(u32 source=g.source;source<end;++source)s.character_glyphs[source]=i;}
        *out={written,units,1,0,0};return SfntStatus::ok;
    }
    const bool extended=profile&&profile->script_tag!=sfnt_tag('l','a','t','n');
    ConstSlice<u32> input_order{};
    if(tones){if(s.shaping.character_glyphs.count<written)return SfntStatus::output_too_small;auto* order=s.shaping.character_glyphs.data;u64 work=limits.stage_work;
        if(written>work)return SfntStatus::resource_limit;work-=written;for(u32 i=0;i<written;++i)order[i]=i;
        for(u32 i=0;i<written;++i)if(control_sources[s.mapped[i].source]==2){double advance=0;const auto status=plan->advance(plan->metric_context,s.mapped[i].glyph.glyph,&advance);if(status!=SfntStatus::ok)return status;
            if(!(advance>0))continue;u32 lo=0,hi=boundary_count;while(lo<hi){if(!work)return SfntStatus::resource_limit;--work;const u32 mid=lo+(hi-lo)/2;if(s.shaping.targets[mid]<=s.mapped[i].source)lo=mid+1;else hi=mid;}
            const u32 first=lo?s.shaping.targets[lo-1]:0;u32 position=i;
            while(position&&s.mapped[order[position-1]].source>=first){if(!work)return SfntStatus::resource_limit;--work;order[position]=order[position-1];--position;}order[position]=i;
        }input_order={order,written};}
    // Sara Am contributes a nikhahit below preceding upper marks. Thai phinthu
    // follows lower vowels in the shaping order; original source indices stay
    // intact. Reuse the existing input-permutation scratch for both operations.
    if(sara_am||thai_below){if(s.shaping.character_glyphs.count<written)return SfntStatus::output_too_small;auto* order=s.shaping.character_glyphs.data;u64 work=limits.stage_work;
        if(written>work)return SfntStatus::resource_limit;work-=written;for(u32 i=0;i<written;++i)order[i]=i;
        for(u32 i=0;i<written;++i)if(s.mapped[i].glyph.reserved&(2|16)){u32 position=i;const u8 previous=(s.mapped[i].glyph.reserved&2)?4:8;
            while(position&&(s.mapped[order[position-1]].glyph.reserved&previous)){if(!work)return SfntStatus::resource_limit;--work;order[position]=order[position-1];--position;}order[position]=i;}
        input_order={order,written};}
    Controls policy{&plan->gdef,controls||tones?control_sources:nullptr,units,0,extended};if(controls||egyptian){const auto status=sfnt_cmap_glyph(font->cmap,32,0,&policy.space);if(status!=SfntStatus::ok)return status;}
    EgyptianControls hieroglyphs;
    if(egyptian){hieroglyphs.common=policy;for(u32 i=0;i<4;++i){const auto status=sfnt_cmap_glyph(font->cmap,0x200c+i,0,hieroglyphs.glyphs+i);if(status!=SfntStatus::ok)return status;}}
    auto latin=*plan;latin.refresh_substitution_classes=true;latin.position_instance.round_variable_anchors=true;
    if(profile){if(profile->positioning_program)latin.positioning=profile->positioning_program;latin.substitution_roots=profile->substitution_roots;latin.positioning_roots=profile->positioning_roots;}latin.compensate_consecutive_zeroes=extended;
    latin.retain_zero_positioning=profile&&profile->script_tag==sfnt_tag('h','e','b','r')&&!hebrew;
    const SfntShapeInputPolicy initial{nullptr,remember_synthetic_bases};
    SfntShapeRunResult result{};const auto status=hebrew?sfnt_hebrew_shape_run(latin,font->cmap,{s.mapped.data,written},{scalars,count},units,rtl,em,s.shaping,&result,limits):
        sfnt_shape_run(latin,{s.mapped.data,written},units,{s.shaping.targets.data,boundary_count},rtl,em,s.shaping,&result,limits,{},input_order,egyptian?SfntShapeGlyphPolicy{&hieroglyphs,finish_egyptian_controls}:controls||extended?SfntShapeGlyphPolicy{&policy,finish_controls}:SfntShapeGlyphPolicy{},synthetic_bases||thai?&initial:nullptr);
    out->lookup_operations=result.lookup_operations;if(status!=SfntStatus::ok)return status;
    if(require_source_coverage){u32 byte=0,source=0;u64 work=limits.stage_work;
        if(result.glyph_count>work)return SfntStatus::resource_limit;work-=result.glyph_count;
        for(u32 i=0;i<result.glyph_count;++i)if(!s.shaping.glyphs[i].glyph.glyph){
            const u32 component=s.shaping.glyphs[i].component;
            if(synthetic_bases&&component<written&&s.shaping.components[component].count==1&&s.shaping.components[component].reserved==0x100)continue;
            const u32 origin=s.shaping.glyphs[i].source;if(origin<source){byte=source=0;}
            while(source<origin&&byte<text.bytes){if(!work)return SfntStatus::resource_limit;--work;const auto cp=utf8_decode_one(text.data+byte,text.bytes-byte);source+=cp.codepoint>65535?2u:1u;byte+=cp.width;}
            const auto cp=utf8_decode_one(text.data+byte,text.bytes-byte);
            if(source!=origin||!cp.valid||!unicode_properties(cp.codepoint)->has(UnicodeFlag::variation_selector))return SfntStatus::unsupported_sequence;
        }
    }
    if(raw_output){*raw_output=result;return SfntStatus::ok;}
    if(result.glyph_count>s.glyphs.count||result.glyph_count>s.placements.count)return SfntStatus::output_too_small;
    if(s.character_glyphs.data!=s.shaping.character_glyphs.data)copy_bytes(s.character_glyphs.data,s.shaping.character_glyphs.data,static_cast<usize>(units)*sizeof(u32));
    if(s.placements.data!=s.shaping.placements.data)copy_bytes(s.placements.data,s.shaping.placements.data,static_cast<usize>(result.glyph_count)*sizeof(SfntPlacement));
    for(u32 i=0;i<result.glyph_count;++i)s.glyphs[i]=s.shaping.glyphs[i].glyph.glyph;
    *out={result.glyph_count,units,0,1,result.lookup_operations};return SfntStatus::ok;
}
}
SfntStatus native_latin_shape_run(const NativeRunPlanStore& store,NativeRunPlanHandle handle,Utf8View text,bool independent,bool rtl,float em,SfntShapeStorage storage,SfntShapeRunResult* out,SfntShapeLimits limits) noexcept {
    if(!out)return SfntStatus::invalid_input;*out={};
    const u64 capacity=static_cast<u64>(storage.positions.count)*sizeof(SfntPosition)/sizeof(SfntClusterGlyph);
    NativeAsciiStorage s{{reinterpret_cast<SfntClusterGlyph*>(storage.positions.data),static_cast<u32>(minimum(capacity,u64{0xffffffffu}))},storage,{},storage.placements,storage.character_glyphs};
    NativeAsciiResult compact{};return alphabetic_run(store,handle,text,independent,em,s,&compact,limits,false,nullptr,true,rtl,out);
}
SfntStatus native_khmer_fallback_shape_run(const NativeRunPlanStore& store,NativeRunPlanHandle handle,Utf8View text,bool independent,bool rtl,float em,
    SfntShapeStorage storage,SfntShapeRunResult* out,ConstSlice<u16> substitutions,ConstSlice<u16> positioning,SfntShapeLimits limits) noexcept {
    if(!out)return SfntStatus::invalid_input;*out={};
    const u64 capacity=static_cast<u64>(storage.positions.count)*sizeof(SfntPosition)/sizeof(SfntClusterGlyph);
    NativeAsciiStorage s{{reinterpret_cast<SfntClusterGlyph*>(storage.positions.data),static_cast<u32>(minimum(capacity,u64{0xffffffffu}))},storage,{},storage.placements,storage.character_glyphs};
    const NativeAlphabeticPlan profile{handle,substitutions,positioning,sfnt_tag('l','a','t','n'),independent,false};
    NativeAsciiResult compact{};return alphabetic_run(store,handle,text,independent,em,s,&compact,limits,false,&profile,true,rtl,out,true);
}
SfntStatus native_latin_run(const NativeRunPlanStore& store,NativeRunPlanHandle handle,Utf8View text,bool independent,float em,const NativeAsciiStorage& s,NativeAsciiResult* out,SfntShapeLimits limits,bool coverage,bool force_shaping) noexcept {
    return alphabetic_run(store,handle,text,independent,em,s,out,limits,coverage,nullptr,force_shaping);
}
SfntStatus native_alphabetic_run(const NativeRunPlanStore& store,const NativeAlphabeticPlan& profile,Utf8View text,float em,const NativeAsciiStorage& s,NativeAsciiResult* out,SfntShapeLimits limits,bool coverage,bool force_shaping,bool rtl) noexcept {
    if(!profile.script_tag){
        if(out)*out={};const auto* plan=native_run_plan_get(store,profile.run,profile.independent);if(!plan)return SfntStatus::invalid_input;
        const auto* instance=native_font_instance_get(*store.instances,store.slots[profile.run.slot-1].instance);
        const auto* font=instance?native_font_get(*store.instances->fonts,instance->face):nullptr;if(!font)return SfntStatus::invalid_input;
        NativeEmojiPlan common{profile.run,&font->cmap,profile.substitution_roots,profile.positioning_roots,0,profile.positioning_program};
        const auto status=sfnt_cmap_glyph(font->cmap,32,0,&common.space_glyph);if(status!=SfntStatus::ok)return status;
        const auto result=native_emoji_run(store,common,text,em,s,out,limits,coverage,rtl);
        // Proved-inert glyphs can bypass lookup execution while source/script
        // policy still requires shaped (sequential) paragraph accumulation.
        if(result==SfntStatus::ok&&out&&force_shaping&&out->nominal_ranges){out->nominal_ranges=0;out->shaped_ranges=1;}
        return result;
    }
    if((profile.script_tag!=sfnt_tag('l','a','t','n')&&!sfnt_alphabetic_script_supported(profile.script_tag))||
        (profile.substitution_roots.count&&!profile.substitution_roots.data)||(profile.positioning_roots.count&&!profile.positioning_roots.data)){if(out)*out={};return SfntStatus::invalid_input;}
    return alphabetic_run(store,profile.run,text,profile.independent,em,s,out,limits,coverage,&profile,force_shaping,rtl);
}
}
