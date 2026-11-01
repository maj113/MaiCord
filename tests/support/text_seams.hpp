#pragma once
// Argument-checked wrappers over the engine's validated internal entry points,
// kept out of the shipped library: only the test programs call them.
#include "maicord/core/memory.hpp"
#include "maicord/text/unicode/bidi.hpp"
#include "maicord/text/opentype/context.hpp"
#include "maicord/text/fonts/geometry.hpp"
#include "maicord/text/opentype/gpos.hpp"
#include "maicord/text/opentype/gpos_execute.hpp"
#include "maicord/text/opentype/gsub.hpp"
#include "../../src/text/opentype/context_lookups.hpp"
#include "../../src/text/opentype/gpos_validated.hpp"
#include "../../src/text/opentype/gsub_validated.hpp"
#include "../../src/text/opentype/sfnt_read.hpp"

namespace maicord {
inline SfntGlyphView sfnt_glyph_view(ConstSlice<SfntGsubGlyph> glyphs) noexcept {return {glyphs.data,nullptr,glyphs.count,0,sizeof(SfntGsubGlyph)};}

// UAX #9 classes of scalars as working cells.
inline UnicodeBidiStatus unicode_bidi_classify(ConstSlice<u32> scalars,Slice<UnicodeBidiCell> cells) noexcept {
    if((scalars.count && !scalars.data) || (cells.count && !cells.data))return UnicodeBidiStatus::invalid_input;
    if(cells.count<scalars.count)return UnicodeBidiStatus::output_too_small;
    for(u32 i=0;i<scalars.count;++i)if(!unicode_is_scalar(scalars[i]))return UnicodeBidiStatus::invalid_input;
    for(u32 i=0;i<scalars.count;++i) {
        const auto type=unicode_properties(scalars[i])->bidi_class();cells[i]={scalars[i],~0u,~0u,type,type,0,0};
    }
    return UnicodeBidiStatus::ok;
}

// Input is logical order. start<input_end<=glyphs.count bounds input matching;
// backtrack/lookahead may extend outside that range within the run. The first input
// glyph is not filtered; backtrack order is nearest-first. Actions keep their encoded
// order and refer to matched input sequence positions, not raw run offsets, so the
// nested executor must rebase after prior edits. A rule with no actions still
// matches; no match returns ok with matched=false. On output_too_small only
// input_count is set, to the required scratch capacity. feature_mask restricts the
// full input span (including ignored glyphs) as in GSUB basic matching; backtrack
// and lookahead need no eligibility bits. A non-sentinel first_coverage_index is the
// already queried first-input coverage index for this context and glyph.
inline SfntStatus sfnt_context_match_view(const SfntContext& c,const SfntGdef& gdef,const SfntLookupFilter& filter,
    SfntGlyphView glyphs,u32 start,u32 input_end,Slice<u32> positions,SfntContextMatch* output,
    u32 work=1000000,u32* work_used=nullptr,u8 mask=0,u32 first_coverage_index=sfnt_not_covered) noexcept {
    const sfnt_read::WorkReport report{work,work,work_used};
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(!sfnt_glyph_view_valid(glyphs) || start>=input_end || input_end>sfnt_glyph_count(glyphs) || (positions.count && !positions.data))return SfntStatus::invalid_input;
    return context_match(c,gdef,filter,glyphs,start,input_end,positions,output,&work,mask,first_coverage_index);
}
struct SfntSequenceAction {u16 sequence_index,lookup_index;};
inline SfntStatus sfnt_context_action(const SfntContextMatch& match,u32 index,SfntSequenceAction* output) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(!match.matched || index>=match.actions.count/4)return SfntStatus::invalid_input;
    *output={byte_load_be16(match.actions.data+index*4),byte_load_be16(match.actions.data+index*4+2)};return SfntStatus::ok;
}

inline SfntStatus sfnt_gsub_subtable_match_view(const SfntGsubSubtable& sub,const SfntGdef& gdef,const SfntLookupFilter& filter,
    SfntGlyphView glyphs,u32 start,u32 alternate,Slice<u32> positions,SfntGsubMatch* output,
    u32 work=1000000,u32* work_used=nullptr,u8 mask=0) noexcept {
    const sfnt_read::WorkReport report{work,work,work_used};
    if(!output)return SfntStatus::invalid_input;reset_value(output);
    if(!sfnt_glyph_view_valid(glyphs) || start>=sfnt_glyph_count(glyphs) || (positions.count && !positions.data))return SfntStatus::invalid_input;
    return gsub_subtable_match(sub,gdef,filter,glyphs,start,alternate,positions,output,&work,mask);
}
inline SfntStatus sfnt_gsub_replacement(const SfntGsubMatch& match,u16 index,u16* glyph) noexcept {
    if(!glyph)return SfntStatus::invalid_input;*glyph=0;if(!match.matched || index>=match.output_count)return SfntStatus::invalid_input;
    *glyph=match.replacements.data?byte_load_be16(match.replacements.data+static_cast<u32>(index)*2):match.single;return SfntStatus::ok;
}

inline SfntStatus sfnt_gpos_subtable_match_view(const SfntGposSubtable& sub,const SfntGdef& gdef,const SfntLookupFilter& filter,
    SfntGlyphView glyphs,u32 start,u32 end,SfntGposMatch* output,u32 work=1000000,u32* used=nullptr) noexcept {
    const sfnt_read::WorkReport report{work,work,used};if(!output)return SfntStatus::invalid_input;reset_value(output);
    if(!sfnt_glyph_view_valid(glyphs) || start>=end || end>sfnt_glyph_count(glyphs))return SfntStatus::invalid_input;
    return gpos_subtable_match(sub,gdef,filter,glyphs,start,end,output,&work);
}

// Flags are supplied by the shaping layer, not inferred from Unicode or GDEF.
// The first glyph must begin a cluster. Storage must not overlap other inputs.
inline SfntStatus sfnt_position_initialize(ConstSlice<double> advances,ConstSlice<u8> flags,bool rtl,Slice<SfntPosition> output,u64 work=1000000) noexcept {
    if(advances.count!=flags.count || (advances.count && (!advances.data || !flags.data)))return SfntStatus::invalid_input;if(output.count<advances.count)return SfntStatus::output_too_small;
    if(advances.count && !output.data)return SfntStatus::invalid_input;if(advances.count>work)return SfntStatus::resource_limit;
    for(u32 i=0;i<advances.count;++i)if(!is_finite(advances.data[i]) || (flags.data[i]&~3u) || (!i && !(flags.data[i]&sfnt_position_cluster_start)))return SfntStatus::invalid_input;
    for(u32 i=0;i<advances.count;++i){const bool zero=(flags.data[i]&sfnt_position_zero_advance)!=0;output.data[i]=sfnt_position_nominal(advances.data[i],zero,rtl);}return SfntStatus::ok;
}

// Instances without fvar axes need no variation storage and ignore axis settings.
// Maps are temporary preparation scratch. Coordinates, scalars and the variation
// object must stay unchanged for the resulting instance.
struct FontGeometryInstanceStorage {
    Slice<AxisSegmentMap> maps;
    Slice<double> coordinates,shared_scalars;
    SfntGvarInstance* variation;
};
inline SfntStatus font_geometry_instance(const FontGeometry& font,ConstSlice<FontAxisSetting> settings,
    FontGeometryInstanceStorage storage,FontGeometryInstance* output,u32 work=1000000,u32* used=nullptr) noexcept {
    const sfnt_read::WorkReport report{work,work,used};
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(!font.face.file.data || (settings.count && !settings.data))return SfntStatus::invalid_input;
    if(!font.axes.axis_count){*output={&font,nullptr};return SfntStatus::ok;}
    if(!storage.variation)return SfntStatus::output_too_small;reset_value(&(*storage.variation));
    FontAxisMappings mappings{};u32 child=0;auto s=font_axis_mappings_open(font.face,font.axes.axis_count,storage.maps,&mappings,work,&child);work-=child;if(s!=SfntStatus::ok)return s;
    s=font_axis_coordinates(font.axes,mappings,settings,storage.coordinates,{},FontAxisPrecision::fixed16,work,&child);work-=child;if(s!=SfntStatus::ok)return s;
    if(font.gvar.table.data){s=sfnt_gvar_instance_prepare(font.gvar,{storage.coordinates.data,font.axes.axis_count},storage.shared_scalars,storage.variation,work,&child);work-=child;if(s!=SfntStatus::ok)return s;}
    else *storage.variation={{},{storage.coordinates.data,font.axes.axis_count},{}};
    *output={&font,storage.variation};return SfntStatus::ok;
}
}
