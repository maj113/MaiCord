#include "maicord/text/scripts/emoji_run.hpp"
#include "maicord/core/memory.hpp"
#include "../shaping/run_profile.hpp"
#include "maicord/text/unicode/unicode.hpp"
#include "maicord/text/base.hpp"
#include "maicord/sys/simd.hpp"
#include "native_direction.hpp"
namespace maicord {namespace {
// A root cannot execute without a covered first glyph. A glyph uncovered by
// every selected root bypasses substitution and positioning; an unknown result
// or exhausted budget keeps full shaping. The four-entry cache of proven
// misses lives only for one run.
bool uncovered(const SfntCoverage& coverage,u16 glyph,u64* work) noexcept {
    if(*work<17)return false;*work-=17;return sfnt_coverage_index(coverage,glyph)==sfnt_not_covered;
}
bool inert(const SfntShapePlan& plan,u16 glyph,u64* work) noexcept {
    for(u32 i=0;i<plan.substitution_roots.count;++i){if(!*work)return false;--*work;
        if(!plan.substitutions||plan.substitution_roots[i]>=plan.substitutions->lookups.count)return false;
        const SfntGsubExecutableLookup* prepared=nullptr;if(sfnt_gsub_lookup_prepare(*plan.substitutions,plan.substitution_roots[i],&prepared)!=SfntStatus::ok)return false;
        const auto& lookup=*prepared;if(sfnt_lookup_bounds_disjoint(lookup.bounds,glyph,glyph))continue;
        for(u32 j=0;j<lookup.subtables.count;++j){const auto& sub=lookup.subtables[j];
            if(!uncovered(lookup.type<=4?sub.basic.coverage:lookup.type==8?sub.reverse.coverages[0]:sfnt_context_first_coverage(sub.context),glyph,work))return false;}}
    for(u32 i=0;i<plan.positioning_roots.count;++i){if(!*work)return false;--*work;
        if(!plan.positioning||plan.positioning_roots[i]>=plan.positioning->lookups.count)return false;
        const auto* prepared=sfnt_gpos_lookup(*plan.positioning,plan.positioning_roots[i]);if(!prepared||!prepared->type)return false;
        const auto& lookup=*prepared;if(sfnt_lookup_bounds_disjoint(lookup.bounds,glyph,glyph))continue;
        const u32 count=lookup.type<=2?lookup.tables.basic.count:lookup.type<=6?lookup.tables.attachment.count:lookup.tables.context.count;
        for(u32 j=0;j<count;++j)if(!uncovered(lookup.type<=2?lookup.tables.basic[j].coverage:lookup.type<=6?lookup.tables.attachment[j].coverage[0]:sfnt_context_first_coverage(lookup.tables.context[j]),glyph,work))return false;
    }return true;
}
// Per source unit: control kind << 16 | nominal glyph.
struct Controls {const SfntGdef* gdef;const u32* sources;u32 units;u16 space;SfntComponentForest components;SfntClusterMap clusters;bool arabic;};
// HarfBuzz hides the default ignorables that substitution left alone (hb_ot_hide_default_ignorables;
// _hb_glyph_info_is_default_ignorable excludes substituted glyphs), after positioning: a hidden glyph keeps its
// own glyph through GPOS, marked by reserved bit 0, and becomes the space glyph once shaped (native_emoji_run).
SfntStatus finish_controls(void* opaque,Slice<SfntComponentGlyph> glyphs,Slice<u8> zero,u64 work) noexcept {
    auto& p=*static_cast<Controls*>(opaque);if(glyphs.count>work)return SfntStatus::resource_limit;if(zero.count<glyphs.count)return SfntStatus::output_too_small;work-=glyphs.count;
    for(u32 i=0;i<glyphs.count;++i){auto& glyph=glyphs[i];if(glyph.source>=p.units)return SfntStatus::invalid_input;
        // Arabic's control ownership follows the restored logical source,
        // including GSUB decomposition. Common emoji follows surviving
        // component ownership instead.
        const u32 kind=glyph.glyph.glyph==static_cast<u16>(p.sources[glyph.source])?p.sources[glyph.source]>>16:0;bool control=kind&&p.arabic;
        if(kind&&!p.arabic&&glyph.component!=0xffffffffu){u32 root=0,ordinal=0;u64 used=0;
            const auto status=sfnt_component_location(p.components,glyph.component,&root,&ordinal,work,&used);work-=used;if(status!=SfntStatus::ok)return status;
            control=root!=0xffffffffu&&p.components.nodes[root].count==1;}
        glyph.glyph.reserved=static_cast<u8>(control);
        if(control){
            if(kind==1&&glyph.source&&!p.arabic){SfntClusterMerge merge{};u64 used=0;
                const auto status=sfnt_cluster_map_plan(p.clusters,glyph.source-1,glyph.source,&merge,work,&used);work-=used;if(status!=SfntStatus::ok)return status;
                if(merge.commit_work>work)return SfntStatus::resource_limit;work-=merge.commit_work;sfnt_cluster_map_commit(&p.clusters,merge);}}
        glyph.glyph.glyph_class=sfnt_gdef_glyph_class(*p.gdef,glyph.glyph.glyph);zero[i]=static_cast<u8>(control||glyph.glyph.glyph_class==SfntGlyphClass::mark);
    }return SfntStatus::ok;
}
}
SfntStatus native_emoji_plan_prepare(const NativeRunPlanStore& store,NativeRunPlanHandle run,Arena* state,Arena* scratch,NativeEmojiPlan* output,u32 work) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    NativePlanSource source{};auto status=native_plan_source(store,run,false,state,scratch,source);if(status!=SfntStatus::ok)return status;
    const auto* parent=source.parent;const auto* instance=source.instance;const auto* font=source.font;
    ArenaScope saved_state{state,arena_mark(state)},saved_scratch{scratch,arena_mark(scratch)};NativeEmojiPlan plan{};plan.run=run;plan.cmap=&font->cmap;
    status=sfnt_cmap_glyph(font->cmap,32,0,&plan.space_glyph);if(status!=SfntStatus::ok)return status;
    status=native_profile_roots_prepare(*instance,*font,0,false,*state,*scratch,plan.substitution_roots,plan.positioning_roots,work);if(status!=SfntStatus::ok)return status;
    status=native_positioning_prepare(*font,*parent,plan.positioning_roots,*state,*scratch,&plan.positioning_program,work);if(status!=SfntStatus::ok)return status;
    saved_state.keep=true;*output=plan;return SfntStatus::ok;
}
SfntStatus native_emoji_run(const NativeRunPlanStore& store,const NativeEmojiPlan& prepared,Utf8View text,float em,const NativeAsciiStorage& s,NativeAsciiResult* out,SfntShapeLimits limits,bool coverage,bool rtl,bool arabic,bool paragraph_start) noexcept {
    if(!out)return SfntStatus::invalid_input;reset_value(out);u32 em_bits=0;copy_bytes(&em_bits,&em,4);const auto* parent=native_run_plan_get(store,prepared.run);
    if(!parent||!prepared.cmap||!filled(prepared.substitution_roots)||!filled(prepared.positioning_roots)||(text.bytes&&!text.data)||!em_bits||em_bits>=0x7f800000u||em>65536||!filled(s.mapped)||!filled(s.glyphs)||!filled(s.placements)||!filled(s.character_glyphs)||
       !filled(s.shaping.advances)||!filled(s.shaping.targets))return SfntStatus::invalid_input;
    if(text.bytes>limits.stage_work)return SfntStatus::resource_limit;u32 mapped=0,units=0,boundaries=0;bool controls=false,simple=true;u16 misses[4]{};u32 cached=0,next_miss=0;u64 proof_work=limits.stage_work;
    // An Arabic-script item laid out left to right is shaped as HarfBuzz does (hb_ensure_native_direction): its
    // graphemes reversed and shaped right to left, then published left to right. Only an item of one grapheme
    // takes it (a ZWJ sequence keeps its glyphs in HarfBuzz's visual order, as one cluster); several graphemes
    // would leave the paragraph's left-to-right cluster order.
    const bool reversed=arabic&&!rtl&&native_direction::reverses(text)&&[&]() noexcept {
        u32 previous=0;for(u32 at=0;at<text.bytes;){const auto decoded=utf8_decode_one(text.data+at,text.bytes-at);if(!decoded.valid)return false;
            if(at&&!native_direction::continues(decoded.codepoint,previous))return false;previous=decoded.codepoint;at+=decoded.width;}
        return true;}();
    SfntShapePlan plan;native_profile_shape_plan(plan,*parent,prepared.positioning_program,prepared.substitution_roots,prepared.positioning_roots);plan.position_instance.round_variable_anchors=true;
    auto* provenance=reinterpret_cast<u32*>(s.shaping.advances.data);const u64 provenance_units=static_cast<u64>(s.shaping.advances.count)*sizeof(double)/sizeof(u32);
    u32 previous_scalar=0;
    for(u32 at=0;at<text.bytes;){const auto scalar=utf8_decode_one(text.data+at,text.bytes-at);if(!scalar.valid)return SfntStatus::invalid_input;
        const u32 cp=scalar.codepoint;const auto* properties=unicode_properties(cp);const auto category=properties->general_category();
        // A selector the font does not map with its base stays a separate hidden glyph (as in latin_run.cpp). So does any
        // other default ignorable that is not a format or control character (CGJ, U+17B4/U+17B5), as HarfBuzz hides
        // it (hb_ot_hide_default_ignorables); its Hangul fillers are not default ignorable (is_default_ignorable).
        const bool selector=properties->has(UnicodeFlag::variation_selector);
        const bool format=category==UnicodeGeneralCategory::cf||category==UnicodeGeneralCategory::cc||category==UnicodeGeneralCategory::zl||category==UnicodeGeneralCategory::zp;
        const bool hidden=!format&&properties->has(UnicodeFlag::default_ignorable_code_point)&&cp!=0x115f&&cp!=0x1160&&cp!=0x3164&&cp!=0xffa0;
        const u8 control=cp==0x200d?1u:cp==0x200c?2u:selector?3u:hidden?4u:0u;
        if(!control&&format)return SfntStatus::unsupported_sequence;
        const auto next=at+scalar.width<text.bytes?utf8_decode_one(text.data+at+scalar.width,text.bytes-at-scalar.width):Utf8DecodeResult{};
        const u32 mapped_cp=rtl&&properties->has(UnicodeFlag::bidi_mirrored)?unicode_bidi_mirror(cp):cp;
        u16 glyph=0;bool variation=false;
        if(!selector&&next.valid&&unicode_layout_variation_selector(next.codepoint)){const auto status=sfnt_cmap_glyph(*prepared.cmap,mapped_cp,next.codepoint,&glyph);
            if(status!=SfntStatus::ok&&status!=SfntStatus::unsupported_sequence)return status;variation=status==SfntStatus::ok&&glyph;}
        if(!variation){const auto status=sfnt_cmap_glyph(*prepared.cmap,mapped_cp,0,&glyph);if(status!=SfntStatus::ok)return status;}
        if(coverage&&!glyph&&!control)return SfntStatus::unsupported_sequence;
        if(mapped==s.mapped.count||units>=provenance_units)return SfntStatus::output_too_small;
        // HarfBuzz continues the cluster of a ZWJ into the Extended_Pictographic after it (hb_set_unicode_props).
        const bool attached=(!arabic&&control==1)||category==UnicodeGeneralCategory::mn||category==UnicodeGeneralCategory::mc||category==UnicodeGeneralCategory::me||
            (previous_scalar==0x200d&&properties->has(UnicodeFlag::extended_pictographic));
        if(!units||!attached){if(boundaries==s.shaping.targets.count)return SfntStatus::output_too_small;s.shaping.targets[boundaries++]=units;}
        // A mark that begins its paragraph follows a dotted circle when the font maps one (HarfBuzz HB_BUFFER_FLAG_BOT),
        // except a default-ignorable one (as in latin_run.cpp).
        if(!units&&paragraph_start&&(category==UnicodeGeneralCategory::mn||category==UnicodeGeneralCategory::mc||category==UnicodeGeneralCategory::me)&&
           !properties->has(UnicodeFlag::default_ignorable_code_point)){
            u16 circle=0;const auto status=sfnt_cmap_glyph(*prepared.cmap,0x25cc,0,&circle);if(status!=SfntStatus::ok)return status;
            if(circle){if(mapped+1>=s.mapped.count)return SfntStatus::output_too_small;s.mapped[mapped++]={{circle,sfnt_gdef_glyph_class(plan.gdef,circle),0},0};simple=false;}}
        const auto glyph_class=sfnt_gdef_glyph_class(plan.gdef,glyph);provenance[units]=control?static_cast<u32>(control)<<16|glyph:0u;controls=controls||control!=0;s.mapped[mapped++]={{glyph,glyph_class,0},units};
        simple=simple&&glyph&&!control&&!attached&&glyph_class!=SfntGlyphClass::mark;
        if(simple){bool found=false;for(u32 i=0;i<cached;++i)found=found||misses[i]==glyph;
            if(!found){simple=inert(plan,glyph,&proof_work);if(simple){misses[next_miss]=glyph;next_miss=(next_miss+1)&3u;cached=minimum<u32>(4,cached+1);}}}
        units+=(cp>65535?2u:1u)+(variation?(next.codepoint>65535?2u:1u):0u);at+=scalar.width+(variation?next.width:0u);previous_scalar=variation?next.codepoint:cp;
    }
    if(units>s.character_glyphs.count)return SfntStatus::output_too_small;if(!mapped)return SfntStatus::ok;
    // One grapheme reversed is itself; HarfBuzz keeps it one cluster.
    if(reversed){simple=false;boundaries=1;}
    if(simple){if(!filled(s.shaping.flags))return SfntStatus::invalid_input;if(mapped>s.glyphs.count||mapped>s.placements.count||mapped>s.shaping.flags.count)return SfntStatus::output_too_small;
        const auto scale=_mm_div_ss(_mm_set_ss(em),_mm_set_ss(static_cast<float>(plan.units_per_em)));u32 previous=0xffffffffu;float advance=0;
        for(u32 i=0;i<mapped;++i){const auto& g=s.mapped[i];if(previous!=g.glyph.glyph){double metric=0;const auto status=plan.advance(plan.metric_context,g.glyph.glyph,&metric);if(status!=SfntStatus::ok)return status;
                advance=_mm_cvtss_f32(_mm_mul_ss(_mm_set_ss(static_cast<float>(metric)),scale));previous=g.glyph.glyph;}
            s.glyphs[i]=g.glyph.glyph;s.placements[i]={advance,0,0};s.shaping.flags[i]=sfnt_position_cluster_start;
            const u32 end=i+1<mapped?s.mapped[i+1].source:units;for(u32 at=g.source;at<end;++at)s.character_glyphs[at]=i;
        }
        *out={mapped,units,1,0,0};return SfntStatus::ok;
    }
    Controls policy{&plan.gdef,provenance,units,prepared.space_glyph,{{s.shaping.components.data,mapped},false,false},sfnt_cluster_map_frame(s.shaping.cluster_bits.data,units),arabic};
    SfntShapeRunResult result{};auto status=sfnt_shape_run(plan,{s.mapped.data,mapped},units,{s.shaping.targets.data,boundaries},rtl||reversed,em,s.shaping,&result,limits,{}, {},controls?SfntShapeGlyphPolicy{&policy,finish_controls}:SfntShapeGlyphPolicy{});
    out->lookup_operations=result.lookup_operations;if(status!=SfntStatus::ok)return status;
    // Hidden controls become the space glyph without horizontal offset after positioning (finish_controls).
    if(controls)for(u32 i=0;i<result.glyph_count;++i)if(s.shaping.glyphs[i].glyph.reserved&1){s.shaping.glyphs[i].glyph.glyph=policy.space;s.shaping.placements[i].advance_offset=0;}
    if(reversed){status=native_direction::publish_left_to_right(text,units,s.shaping,result.glyph_count,limits.stage_work);if(status!=SfntStatus::ok)return status;}
    if(result.glyph_count>s.glyphs.count||result.glyph_count>s.placements.count)return SfntStatus::output_too_small;
    if(s.character_glyphs.data!=s.shaping.character_glyphs.data)copy_bytes(s.character_glyphs.data,s.shaping.character_glyphs.data,static_cast<usize>(units)*sizeof(u32));
    if(s.placements.data!=s.shaping.placements.data)copy_bytes(s.placements.data,s.shaping.placements.data,static_cast<usize>(result.glyph_count)*sizeof(SfntPlacement));
    for(u32 i=0;i<result.glyph_count;++i)s.glyphs[i]=s.shaping.glyphs[i].glyph.glyph;
    *out={result.glyph_count,units,0,1,result.lookup_operations};return SfntStatus::ok;
}
}
