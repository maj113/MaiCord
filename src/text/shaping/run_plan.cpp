#include "maicord/text/shaping/run_plan.hpp"
#include "maicord/core/memory.hpp"
#include "run_profile.hpp"
#include "../opentype/sfnt_read.hpp"
#include "maicord/text/opentype/metric_variations.hpp"
#include "maicord/text/opentype/nominal_participation.hpp"
#include "maicord/text/opentype/gpos_execute.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/text/base.hpp"

#include "maicord/core/virtual_memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_process.hpp"

namespace maicord {namespace {
struct PlanData {
    SfntShapePlan plans[2];
    SfntGsubProgram substitutions;
    SfntHorizontalMetrics metrics;
    SfntHvar hvar;
    VariationInstance hvar_instance,gdef_instance;
    bool variable;
    NativeRunPlanKind kind;
    NativeNominalPlan nominal;
    NativeKerningPlan kerning;
    SfntFontLayoutMetrics layout_metrics;
    NativeRunPlanHandle parent;
    const FontGeometryInstance* bold;
    const SfntShapePlan* parent_plan;
    u64* ink;
    u32 ink_glyphs;
};
inline NativeFontResult failure(SfntStatus status) noexcept {return {NativeFontError::font,status,0};}
#include "run_substitutions.inc"
NativeRunPlanSlot* slot(const NativeRunPlanStore& s,NativeRunPlanHandle h) noexcept {
    if(!s.instances||!s.slots.data||!h.slot||h.slot>s.slots.count||!h.generation)return nullptr;
    auto& p=s.slots[h.slot-1];return p.references&&p.generation==h.generation?&p:nullptr;
}
SfntStatus advance(void* context,u16 glyph,double* output) noexcept {
    // Plan preparation validates the complete immutable hmtx table.
    const auto& metrics=*static_cast<const SfntHorizontalMetrics*>(context);*output=0;
    if(glyph>=metrics.glyph_count)return SfntStatus::glyph_out_of_range;
    *output=byte_load_be16(metrics.table.data+minimum<u32>(glyph,metrics.long_metrics-1u)*4u);return SfntStatus::ok;
}
SfntStatus hvar_advance(void* context,u16 glyph,double* output) noexcept {
    const auto& p=*static_cast<const PlanData*>(context);u16 value=0;
    const auto status=sfnt_hvar_design_advance(p.hvar,p.hvar_instance.scalars,p.metrics,glyph,&value);*output=value;return status;
}
SfntStatus gvar_advance(void* context,u16 glyph,double* output) noexcept {
    const auto& instance=*static_cast<const FontGeometryInstance*>(context);u16 value=0;
    const auto status=sfnt_gvar_design_advance(instance.font->outline.truetype,*instance.variation,glyph,&value);*output=value;return status;
}
// A bold glyph with ink is (units per em + 25) / 50 units wider, as DirectWrite draws it. An empty glyph keeps its
// advance. TrueType reads the ink box from glyf. CFF and CFF2 read it once for each glyph from the first path commands
// (cff_glyph_ink) and add their parent's advance, which follows the variation instance.
SfntStatus bold_advance(void* context,u16 glyph,double* output) noexcept {
    *output=0;
    const auto& p=*static_cast<const PlanData*>(context);double value=0;bool inked=false;
    if(p.bold->font->kind==FontGeometryKind::truetype){SfntGlyphMetrics metrics{};
        auto status=sfnt_horizontal_metrics(p.metrics,glyph,&metrics);if(status!=SfntStatus::ok)return status;
        value=metrics.advance;
        if(value){const auto& f=p.bold->font->outline.truetype;const u32 stride=f.loca_stride;
            if((stride!=2&&stride!=4)||!f.loca.data||f.loca.count/stride<=static_cast<u32>(glyph)+1)return SfntStatus::malformed;
            const u32 first=sfnt_read::loca_offset(f.loca,stride,glyph),end=sfnt_read::loca_offset(f.loca,stride,static_cast<u32>(glyph)+1);if(first>end||end>f.glyf.count)return SfntStatus::malformed;
            if(first<end){if(!f.glyf.data||end-first<10)return SfntStatus::malformed;const auto* box=f.glyf.data+first+2;
                const i32 width=static_cast<i16>(byte_load_be16(box+4))-static_cast<i16>(byte_load_be16(box));
                const i32 height=static_cast<i16>(byte_load_be16(box+6))-static_cast<i16>(byte_load_be16(box+2));
                if(width<0||height<0)return SfntStatus::malformed;inked=width&&height;}}
    }else{auto status=p.parent_plan->advance(p.parent_plan->metric_context,glyph,&value);if(status!=SfntStatus::ok)return status;
        if(value){u64* const word=glyph<p.ink_glyphs?p.ink+glyph/32:nullptr;const u32 shift=glyph%32*2;
            if(word&&((*word>>shift)&1))inked=((*word>>shift)&2)!=0;
            else{const auto* variation=p.bold->variation;
                status=cff_glyph_ink(p.bold->font->outline.cff,glyph,variation?variation->coordinates:ConstSlice<double>{},&inked);if(status!=SfntStatus::ok)return status;
                if(word)*word|=(inked?3ull:1ull)<<shift;}}
    }
    *output=value+(inked?(static_cast<u32>(p.metrics.units_per_em)+25)/50:0);return SfntStatus::ok;
}
// Plan preparation runs once per font instance and kind (not per run): compiled for size.
#pragma clang attribute push(__attribute__((minsize)), apply_to = function)
// Whether a prepared GPOS lookup can start at glyph: one of its first-input coverages holds the glyph.
bool starts_at(const SfntGposExecutableLookup& lookup,u16 glyph) noexcept {
    if(sfnt_lookup_bounds_exclude(lookup.bounds,glyph))return false;
    const u32 count=lookup.type<=2?lookup.tables.basic.count:lookup.type<=6?lookup.tables.attachment.count:lookup.tables.context.count;
    for(u32 i=0;i<count;++i){const auto& coverage=lookup.type<=2?lookup.tables.basic[i].coverage:lookup.type<=6?lookup.tables.attachment[i].coverage[0]:
            sfnt_context_first_coverage(lookup.tables.context[i]);
        if(sfnt_coverage_index(coverage,glyph)!=sfnt_not_covered)return true;}
    return false;
}
// The kerning plan of a static text plan. substituted holds the ASCII offsets whose glyphs take part in GSUB.
SfntStatus prepare_kerning(PlanData& p,const u64* substituted,Arena& state) noexcept {
    const auto& plan=p.plans[0];auto& kerning=p.kerning;
    const auto ready=[&](u32 i) noexcept {return ((kerning.ready[i/64]>>(i%64))&1)!=0;};
    for(u32 i=0;i<p.nominal.ascii_count;++i){const u16 glyph=static_cast<u16>(p.nominal.ascii_first+i);const auto type=sfnt_gdef_glyph_class(plan.gdef,glyph);
        if(((substituted[i/64]>>(i%64))&1)||glyph==plan.zwj||glyph==plan.zwnj||(type!=SfntGlyphClass::unassigned&&type!=SfntGlyphClass::base))continue;
        bool other=false;
        for(u32 r=0;r<plan.positioning_roots.count&&!other;++r){const auto* lookup=sfnt_gpos_lookup(*plan.positioning,plan.positioning_roots[r]);
            other=!lookup||(lookup->type>2&&starts_at(*lookup,glyph));}
        if(other)continue;
        kerning.ready[i/64]|=1ull<<(i%64);if(type==SfntGlyphClass::base)kerning.base[i/64]|=1ull<<(i%64);
    }
    // Two passes over the roots: the first counts the lookups that start at a ready glyph, the second keeps them.
    u32 count=0;Slice<u16> roots{};
    for(u32 pass=0;pass<2;++pass){count=0;
        for(u32 r=0;r<plan.positioning_roots.count;++r){const auto* lookup=sfnt_gpos_lookup(*plan.positioning,plan.positioning_roots[r]);if(!lookup||lookup->type>2)continue;
            bool starts=false;for(u32 i=0;i<p.nominal.ascii_count&&!starts;++i)starts=ready(i)&&starts_at(*lookup,static_cast<u16>(p.nominal.ascii_first+i));
            if(starts){if(pass)roots[count]=plan.positioning_roots[r];++count;}}
        if(!pass){roots=take<u16>(state,count);if(!filled(roots))return SfntStatus::resource_limit;}}
    kerning.roots={roots.data,roots.count};return SfntStatus::ok;
}
SfntStatus prepare(const NativeFontInstance& instance,const NativeFontFace& native_font,PlanData& p,Arena& state,Arena& scratch,u32 work,usize state_limit,Arena*& substitutions) noexcept {
    const auto& cmap=native_font.cmap;const auto& font=*instance.geometry.font;const auto& face=font.face;ConstSlice<u8> bytes{};
    auto status=sfnt_horizontal_metrics_open(face,&p.metrics);if(status!=SfntStatus::ok)return status;
    const bool emoji=p.kind==NativeRunPlanKind::emoji;
    const u32 nominal_words=emoji?0:(p.metrics.glyph_count+63u)/64;p.nominal.glyph_limit=emoji?0:p.metrics.glyph_count;
    for(u32 i=0;!emoji&&i<minimum<u32>(95,work);++i){u16 glyph=0;
        if(sfnt_cmap_glyph(cmap,32+i,0,&glyph)!=SfntStatus::ok||!glyph||glyph>=p.metrics.glyph_count)break;
        if(!i)p.nominal.ascii_first=glyph;
        else if(glyph!=static_cast<u32>(p.nominal.ascii_first)+i)break;
        ++p.nominal.ascii_count;
    }
    auto nominal_sources=take<u64>(scratch,nominal_words),participation=take<u64>(scratch,nominal_words);
    if(!filled(nominal_sources)||!filled(participation))return SfntStatus::output_too_small;
    SfntNominalSourceCache source_cache{&cmap,nominal_sources,false};
    for(u32 i=0;i<nominal_words;++i)participation[i]=0;
    SfntGdef gdef{};status=sfnt_table(face,sfnt_tag('G','D','E','F'),&bytes);
    if(status==SfntStatus::ok){status=sfnt_gdef_open(bytes,p.metrics.glyph_count,&gdef);if(status!=SfntStatus::ok)return status;}
    else if(status!=SfntStatus::missing_table)return status;
    const auto coordinates=instance.geometry.variation?instance.geometry.variation->coordinates:ConstSlice<double>{};
    auto feature_coordinates=take<i16>(scratch,coordinates.count);
    if(coordinates.count&&!feature_coordinates.data)return SfntStatus::output_too_small;
    for(u32 i=0;i<coordinates.count;++i){const double value=coordinates[i]*16384;feature_coordinates[i]=static_cast<i16>(value+(value<0?-.5:.5));}
    p.variable=instance.geometry.variation!=nullptr;
    if(p.variable){
        status=sfnt_table(face,sfnt_tag('H','V','A','R'),&bytes);
        if(status==SfntStatus::ok){status=sfnt_hvar_open(bytes,&p.hvar,work);if(status!=SfntStatus::ok)return status;
            auto scalars=take<double>(state,p.hvar.store.region_count);if(!filled(scalars))return SfntStatus::resource_limit;
            status=variation_instance_prepare(p.hvar.store,coordinates,scalars,&p.hvar_instance,work,VariationCoordinatePrecision::fixed_16);if(status!=SfntStatus::ok)return status;}
        else if(status!=SfntStatus::missing_table)return status;
        ItemVariationStore variations{};status=sfnt_gdef_variation_store(gdef,&variations,work);
        if(status==SfntStatus::ok){auto scalars=take<double>(state,variations.region_count);if(!filled(scalars))return SfntStatus::resource_limit;status=variation_instance_prepare(variations,coordinates,scalars,&p.gdef_instance,work,VariationCoordinatePrecision::fixed_16);if(status!=SfntStatus::ok)return status;}
        else if(status!=SfntStatus::missing_table)return status;
    }
    SfntMvar mvar{};VariationInstance mvar_instance{};
    if(p.variable){status=sfnt_table(face,sfnt_tag('M','V','A','R'),&bytes);
        if(status==SfntStatus::ok){status=sfnt_mvar_open(bytes,&mvar,work);if(status!=SfntStatus::ok)return status;
            if(mvar.record_count){auto scalars=take<double>(scratch,mvar.store.region_count);if(!filled(scalars))return SfntStatus::output_too_small;
                status=variation_instance_prepare(mvar.store,coordinates,scalars,&mvar_instance,work,VariationCoordinatePrecision::fixed_16);if(status!=SfntStatus::ok)return status;}}
        else if(status!=SfntStatus::missing_table)return status;}
    status=sfnt_font_layout_metrics(face,p.metrics,p.variable?&mvar:nullptr,mvar_instance.scalars,&p.layout_metrics);if(status!=SfntStatus::ok)return status;
    for(u32 mode=0;mode<2;++mode){auto& plan=p.plans[mode];plan.substitutions=&p.substitutions;
        plan.gdef=gdef;plan.metric_context=p.hvar.store.table.data?static_cast<void*>(&p):&p.metrics;
        plan.advance=p.hvar.store.table.data?hvar_advance:advance;plan.units_per_em=p.metrics.units_per_em;
        if(font.gvar.table.data&&!p.hvar.store.table.data){plan.metric_context=const_cast<FontGeometryInstance*>(&instance.geometry);plan.advance=gvar_advance;}
        plan.cache_advances=p.variable;
        plan.position_instance={p.gdef_instance.store.table.data?&p.gdef_instance:nullptr,0,0,p.variable,VariationArithmetic::float_32};}
    if(!p.variable&&p.nominal.ascii_count){auto table=take<u16>(state,p.nominal.ascii_count);if(!filled(table))return SfntStatus::resource_limit;
        for(u32 i=0;i<table.count;++i){double value=0;status=p.plans[0].advance(p.plans[0].metric_context,static_cast<u16>(p.nominal.ascii_first+i),&value);if(status!=SfntStatus::ok)return status;table[i]=static_cast<u16>(value);}
        p.nominal.ascii_advances=table.data;}
    bool gpos_kern=false,reverse=false;u16 zwj=0,zwnj=0;u64 substituted[2]{};
    status=sfnt_cmap_glyph(cmap,0x200d,0,&zwj);if(status!=SfntStatus::ok)return status;
    status=sfnt_cmap_glyph(cmap,0x200c,0,&zwnj);if(status!=SfntStatus::ok)return status;
    for(auto& plan:p.plans){plan.zwj=zwj;plan.zwnj=zwnj;}
    for(u32 table=0;table<2;++table){
        status=sfnt_table(face,table?sfnt_tag('G','P','O','S'):sfnt_tag('G','S','U','B'),&bytes);
        if(status==SfntStatus::missing_table)continue;if(status!=SfntStatus::ok)return status;
        SfntLayout layout{};status=sfnt_layout_open(bytes,table?SfntLayoutKind::positioning:SfntLayoutKind::substitution,&layout);if(status!=SfntStatus::ok)return status;
        for(u32 f=0;table&&f<layout.feature_count;++f)gpos_kern|=byte_load_be32(layout.features.data+2+f*6)==sfnt_tag('k','e','r','n');
        SfntFeatureSubstitutions variations{};u32 variation_work=0;
        if(layout.variations){u32 record=0;status=sfnt_layout_variation_select(layout,{feature_coordinates.data,feature_coordinates.count},&variations,&record,work,&variation_work);if(status!=SfntStatus::ok)return status;}
        // HarfBuzz's default shaper matches joiners manually in mark and mkmk (F_GLOBAL_MANUAL_JOINERS).
        {constexpr SfntJoinerFeature manual_features[]{{sfnt_tag('m','a','r','k'),3},{sfnt_tag('m','k','m','k'),3}};ConstSlice<u64> manual{};
            status=native_manual_joiners(layout,variations,{manual_features,2},state,manual,work);if(status!=SfntStatus::ok)return status;
            for(auto& plan:p.plans)(table?plan.positioning_manual:plan.substitution_manual)=manual;}
        if(emoji){auto& plan=p.plans[0];
            status=native_lookup_roots_prepare(layout,variations,0,false,state,scratch,
                table?plan.positioning_roots:plan.substitution_roots,work);if(status!=SfntStatus::ok)return status;
            p.plans[1].substitution_roots=plan.substitution_roots;p.plans[1].positioning_roots=plan.positioning_roots;
        }else{
            auto nominal_scratch=take<u64>(scratch,(layout.feature_count+63u)/64+(layout.lookup_count+63u)/64);
            if(!filled(nominal_scratch))return SfntStatus::output_too_small;
            SfntNominalParticipationResult participation_result;
            status=sfnt_nominal_participation_add(layout,p.metrics.glyph_count,participation,nominal_scratch,&participation_result,work,{},&source_cache,variations);
            if(status!=SfntStatus::ok)return status;
            for(u32 i=0;!table&&i<p.nominal.ascii_count;++i){const u32 glyph=p.nominal.ascii_first+i;
                if((participation[glyph/64]>>(glyph%64))&1)substituted[i/64]|=1ull<<(i%64);}
        }
        if(!table){status=deferred_substitutions(layout,gdef,p.metrics.glyph_count,work,state_limit,substitutions,p.substitutions);if(status!=SfntStatus::ok)return status;}
        for(u32 mode=0;!emoji&&mode<2;++mode){auto& plan=p.plans[mode];status=native_lookup_roots_prepare(layout,variations,sfnt_tag('l','a','t','n'),mode!=0,state,scratch,
            table?plan.positioning_roots:plan.substitution_roots,work);if(status!=SfntStatus::ok)return status;}
        // The participation of the standard features leaves out reverse chaining substitution.
        for(u32 i=0;!emoji&&!table&&i<p.plans[0].substitution_roots.count;++i){SfntLookup lookup{};
            status=sfnt_layout_lookup(layout,p.plans[0].substitution_roots[i],&lookup);if(status!=SfntStatus::ok)return status;reverse|=lookup.type==8;}
        if(table){const auto first=p.plans[0].positioning_roots,second=p.plans[1].positioning_roots;auto roots=take<u16>(scratch,first.count+second.count);
            if(!filled(roots))return SfntStatus::output_too_small;
            copy_bytes(roots.data,first.data,first.count*sizeof(u16));if(second.count)copy_bytes(roots.data+first.count,second.data,second.count*sizeof(u16));
            status=native_positioning_prepare(native_font,p.plans[0],{roots.data,roots.count},state,scratch,&p.plans[0].positioning,work);if(status!=SfntStatus::ok)return status;
            p.plans[1].positioning=p.plans[0].positioning;}
    }
    // Without a GPOS kern feature HarfBuzz kerns with the legacy kern table (hb-ot-shape.cc, apply_kern); independent letters do not kern.
    if(!emoji&&!gpos_kern){status=sfnt_table(face,sfnt_tag('k','e','r','n'),&bytes);
        if(status==SfntStatus::ok){if(sfnt_legacy_kern_open(bytes,participation))p.plans[0].legacy_kern=bytes;}
        else if(status!=SfntStatus::missing_table)return status;}
    if(!emoji&&!reverse&&p.nominal.ascii_advances){status=prepare_kerning(p,substituted,state);if(status!=SfntStatus::ok)return status;}
    bool any=false;for(u32 i=0;i<nominal_words;++i)any=any||participation[i]!=0;
    if(any){auto retained=take<u64>(state,nominal_words);if(!filled(retained))return SfntStatus::resource_limit;
        copy_bytes(retained.data,participation.data,nominal_words*sizeof(u64));p.nominal.participation={retained.data,nominal_words};}
    if(p.substitutions.source){auto& owner=*static_cast<GsubOwner*>(p.substitutions.source);
        if(state.committed>state_limit||owner.used>state_limit-state.committed)return SfntStatus::resource_limit;
        owner.limit=state_limit-state.committed;}
    return SfntStatus::ok;
}
}
NativeFontResult native_run_plan_acquire(NativeRunPlanStore* store,NativeFontInstanceHandle instance,
    Arena* scratch,NativeRunPlanHandle* output,u32 work,usize limit,u32 simulations,NativeRunPlanKind kind) noexcept {
    reset_value(output);if(!store->instances||!store->slots.data||!store->slots.count||!scratch->base)
        return {NativeFontError::invalid_input,SfntStatus::invalid_input,0};
    const auto* font=native_font_instance_get(*store->instances,instance);if(!font)return {NativeFontError::invalid_input,SfntStatus::invalid_input,0};
    const auto* face=native_font_get(*store->instances->fonts,font->face);if(!face)return {NativeFontError::invalid_input,SfntStatus::invalid_input,0};
    const bool bold=(simulations&1)!=0&&!font_ignores_bold(face->geometry);
    if(bold&&face->geometry.kind==FontGeometryKind::truetype&&face->geometry.gvar.table.data)return failure(SfntStatus::unsupported);
    u32 available=store->slots.count;
    if(store->slots.count>work)return failure(SfntStatus::resource_limit);
    for(u32 i=0;i<store->slots.count;++i){auto& p=store->slots[i];
        if(!p.references){if(available==store->slots.count&&p.generation!=0xffffffffu)available=i;continue;}
        if(p.instance.slot==instance.slot&&p.instance.generation==instance.generation&&reinterpret_cast<const PlanData*>(p.state.base)->kind==kind&&
           (reinterpret_cast<const PlanData*>(p.state.base)->parent.slot!=0)==bold){
            if(p.references==0xffffffffu)return {NativeFontError::capacity,SfntStatus::resource_limit,0};
            ++p.references;*output={i+1,p.generation};return {NativeFontError::none,SfntStatus::ok,0};}
    }
    if(available==store->slots.count)return {NativeFontError::capacity,SfntStatus::resource_limit,0};
    const usize page=pages_page_size();
    if(!page||(page&(page-1)))return failure(SfntStatus::invalid_input);limit&=~(page-1);
    if(limit<page||limit<sizeof(PlanData))return failure(SfntStatus::resource_limit);
    // A bold CFF or CFF2 plan keeps two bits for each glyph on pages after its metric view: whether bold_advance read
    // the ink of the glyph, and the ink. These pages stay writable. Plans change on the owning thread only.
    const u32 ink_glyphs=bold&&face->geometry.kind==FontGeometryKind::cff?face->geometry.outline.cff.glyph_count:0;
    const usize ink=ink_glyphs?((ink_glyphs+31u)/32u*8u+page-1)&~(page-1):0;
    if(bold&&limit<page+ink)return failure(SfntStatus::resource_limit);
    NativeRunPlanHandle parent{};
    struct ParentRelease {NativeRunPlanStore* s;NativeRunPlanHandle& h;~ParentRelease() noexcept{if(h.slot)native_run_plan_release(s,h);}} parent_release{store,parent};
    if(bold){const auto result=native_run_plan_acquire(store,instance,scratch,&parent,work,limit-page-ink,0,kind);if(!result.ok())return result;
        available=0;while(available<store->slots.count&&(store->slots[available].references||store->slots[available].generation==0xffffffffu))++available;
        if(available==store->slots.count)return {NativeFontError::capacity,SfntStatus::resource_limit,0};}
    Arena state{};struct Cleanup {Arena& state;~Cleanup() noexcept {arena_release(&state);}} cleanup{state};
    if(!arena_initialize(&state,bold?page+ink:limit,0,page,ArenaTag::text))return {NativeFontError::capacity,SfntStatus::resource_limit,thread_last_error()};
    auto* data=arena_allocate_zero<PlanData>(&state);if(!data)return failure(SfntStatus::resource_limit);data->kind=kind;
    SubstitutionScope substitutions{store->substitutions};
    if(bold){const auto& base=*reinterpret_cast<const PlanData*>(store->slots[parent.slot-1].state.base);
        data->metrics=base.metrics;data->nominal=base.nominal;data->nominal.ascii_advances=nullptr;data->layout_metrics=base.layout_metrics;data->parent=parent;
        data->bold=&font->geometry;data->parent_plan=&base.plans[0];
        if(ink){data->ink=static_cast<u64*>(arena_bump_zero(&state,ink,page));data->ink_glyphs=ink_glyphs;if(!data->ink)return failure(SfntStatus::resource_limit);}
        for(u32 mode=0;mode<2;++mode){data->plans[mode]=base.plans[mode];data->plans[mode].metric_context=data;data->plans[mode].advance=bold_advance;data->plans[mode].cache_advances=true;}}
    else{arena_reset(scratch);const auto status=prepare(*font,*face,*data,state,*scratch,work,limit,store->substitutions);if(status!=SfntStatus::ok)return failure(status);}
    if(!pages_protect(state.base,ink?page:state.committed,PageAccess::read))return {NativeFontError::io,SfntStatus::ok,thread_last_error()};
    if(!bold&&!native_font_instance_retain(store->instances,instance))return {NativeFontError::capacity,SfntStatus::resource_limit,0};
    auto& p=store->slots[available];p.instance=instance;p.state=state;reset_value(&state);p.references=1;++p.generation;
    substitutions.keep=true;reset_value(&parent);*output={available+1,p.generation};return {NativeFontError::none,SfntStatus::ok,0};
}
#pragma clang attribute pop
const SfntShapePlan* native_run_plan_get(const NativeRunPlanStore& store,NativeRunPlanHandle h,bool independent) noexcept {
    auto* p=slot(store,h);return p?&reinterpret_cast<const PlanData*>(p->state.base)->plans[independent?1:0]:nullptr;
}
const NativeKerningPlan* native_run_plan_kerning(const NativeRunPlanStore& store,NativeRunPlanHandle h) noexcept {
    auto* p=slot(store,h);return p?&reinterpret_cast<const PlanData*>(p->state.base)->kerning:nullptr;
}
NativeNominalPlan native_run_plan_nominal(const NativeRunPlanStore& store,NativeRunPlanHandle h) noexcept {
    auto* p=slot(store,h);return p?reinterpret_cast<const PlanData*>(p->state.base)->nominal:NativeNominalPlan{};
}
bool native_run_plan_retain(NativeRunPlanStore* store,NativeRunPlanHandle h) noexcept {
    auto* p=slot(*store,h);if(!p||p->references==0xffffffffu)return false;++p->references;return true;
}
const SfntFontLayoutMetrics* native_run_plan_metrics(const NativeRunPlanStore& store,NativeRunPlanHandle h) noexcept {
    auto* p=slot(store,h);return p?&reinterpret_cast<const PlanData*>(p->state.base)->layout_metrics:nullptr;
}
bool native_run_plan_release(NativeRunPlanStore* store,NativeRunPlanHandle h) noexcept {
    auto* p=slot(*store,h);if(!p)return false;if(--p->references)return true;
    const auto instance=p->instance;const auto parent=reinterpret_cast<const PlanData*>(p->state.base)->parent;const u32 generation=p->generation;arena_release(&p->state);reset_value(p);p->generation=generation;
    const bool released=parent.slot?native_run_plan_release(store,parent):native_font_instance_release(store->instances,instance);
    bool live=false;for(u32 i=0;i<store->slots.count;++i)live=live||store->slots[i].references!=0;
    if(!live)release_substitutions(store->substitutions);return released;
}
}
