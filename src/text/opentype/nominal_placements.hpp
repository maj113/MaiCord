#pragma once
#include "maicord/text/scripts/ascii_run.hpp"
#include "maicord/sys/simd.hpp"

namespace maicord {
// Advance of a glyph in font units; the plan's ASCII table (static fonts only)
// answers the affine cmap prefix without the metric callback.
inline SfntStatus nominal_advance(const NativeNominalPlan& nominal,const SfntShapePlan& plan,u16 glyph,double* advance) noexcept {
    const u32 index=static_cast<u32>(glyph)-nominal.ascii_first;
    if(nominal.ascii_advances&&index<nominal.ascii_count){*advance=nominal.ascii_advances[index];return SfntStatus::ok;}
    return plan.advance(plan.metric_context,glyph,advance);
}
// The common run in one pass: every character in the plan's affine prefix, no glyph taking part in
// substitution or positioning, advances from the plan's table (static fonts only). Returns the
// characters it wrote. The loop has its own frame: it keeps its state in registers.
__declspec(noinline) inline u32 nominal_ascii_prefix(Utf8View text,const NativeNominalPlan& nominal,__m128 scale,u16* glyphs,SfntPlacement* placements,u32* sources) noexcept {
    const u64* const bits=nominal.participation.count?nominal.participation.data:nullptr;
    const u16* const advances=nominal.ascii_advances;const u32 first=nominal.ascii_first,count=nominal.ascii_count;u32 i=0;
    for(;i<text.bytes;++i){const u32 offset=static_cast<u8>(text.data[i])-32u;if(offset>=count)break;
        const u16 glyph=static_cast<u16>(first+offset);if(bits&&(bits[glyph/64]&(1ull<<(glyph%64))))break;
        glyphs[i]=glyph;placements[i]={_mm_cvtss_f32(_mm_mul_ss(_mm_set_ss(static_cast<float>(advances[offset])),scale)),0,0};sources[i]=i;}
    return i;
}
// A run of printable ASCII that only the font's pair and single positioning changes (NativeKerningPlan): the
// nominal glyphs and advances, the plan's positioning lookups that start at these glyphs, then the legacy kern
// table. The output is the output of sfnt_shape_run and compact_publish for the run, flags included, but the
// operation count is this route's own. It needs a plan of the font without independent letters and a static font.
// False, with nothing published, when a character or the storage does not qualify or a step fails: the caller
// then takes the general path, which reports the errors.
bool kerned_ascii_run(const SfntShapePlan& plan,const NativeNominalPlan& nominal,const NativeKerningPlan& kerning,Utf8View text,float em,
    const NativeAsciiStorage& s,NativeAsciiResult* out,const SfntShapeLimits& limits) noexcept;
inline SfntStatus nominal_placements(const SfntShapePlan& plan,ConstSlice<SfntClusterGlyph> mapped,
    Slice<u16> glyphs,Slice<SfntPlacement> placements,Slice<u32> scratch,__m128 scale) noexcept {
    if(plan.cache_advances&&mapped.count>=8&&scratch.data&&scratch.count>=32){
        const u32 mask=scratch.count>=64?63:31;u64 occupied=0;
        for(u32 i=0;i<mapped.count;++i){const u16 glyph=mapped[i].glyph.glyph;const u32 slot=glyph&mask;const u64 bit=1ull<<slot;
            float value=0;
            if((occupied&bit)&&glyphs[scratch[slot]]==glyph)value=placements[scratch[slot]].advance;
            else{double advance=0;const auto status=plan.advance(plan.metric_context,glyph,&advance);if(status!=SfntStatus::ok)return status;
                value=_mm_cvtss_f32(_mm_mul_ss(_mm_set_ss(static_cast<float>(advance)),scale));scratch[slot]=i;occupied|=bit;}
            glyphs[i]=glyph;placements[i]={value,0,0};
        }
    }else for(u32 i=0;i<mapped.count;++i){const u16 glyph=mapped[i].glyph.glyph;double advance=0;
        const auto status=plan.advance(plan.metric_context,glyph,&advance);if(status!=SfntStatus::ok)return status;
        glyphs[i]=glyph;placements[i]={_mm_cvtss_f32(_mm_mul_ss(_mm_set_ss(static_cast<float>(advance)),scale)),0,0};
    }
    return SfntStatus::ok;
}
}
