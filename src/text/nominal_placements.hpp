#pragma once
#include "maicord/text/shape_run.hpp"
#include <emmintrin.h>

namespace maicord {
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
