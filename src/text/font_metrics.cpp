#include "maicord/text/font_metrics.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/base.hpp"
#include "layout_float.hpp"
namespace maicord {namespace {
u16 word(ConstSlice<u8> bytes,u32 offset) noexcept {return byte_load_be16(bytes.data+offset);}
i16 signed_word(ConstSlice<u8> bytes,u32 offset) noexcept {return static_cast<i16>(word(bytes,offset));}
#pragma float_control(precise,on,push)
// Nearest integer of base plus its MVAR delta, ties upward. Without variations
// that is base itself. The first failure is kept in status and later calls are no-ops.
i32 varied(i32 base,u32 tag,const SfntMvar* mvar,ConstSlice<double> scalars,SfntStatus& status) noexcept {
    if(!mvar||status!=SfntStatus::ok)return base;
    double delta=0;status=sfnt_mvar_delta(*mvar,scalars,tag,&delta);if(status!=SfntStatus::ok)return base;
    const double value=base+delta+.5;
    if(!(value>-2147483647.0&&value<2147483647.0)){status=SfntStatus::malformed;return base;}
    const i32 truncated=static_cast<i32>(value);return truncated-(truncated>value?1:0);
}
#pragma float_control(pop)
}
SfntStatus sfnt_font_layout_metrics(const SfntFace& face,const SfntHorizontalMetrics& h,
    const SfntMvar* mvar,ConstSlice<double> scalars,SfntFontLayoutMetrics* output) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);if(!h.units_per_em)return SfntStatus::invalid_input;
    ConstSlice<u8> os2{},post{};auto status=sfnt_table(face,sfnt_tag('O','S','/','2'),&os2);if(status!=SfntStatus::ok)return status;
    status=sfnt_table(face,sfnt_tag('p','o','s','t'),&post);if(status!=SfntStatus::ok)return status;
    if(os2.count<78||post.count<12)return SfntStatus::truncated;
    const auto adjust=[&](i32 base,u32 tag) noexcept {return varied(base,tag,mvar,scalars,status);};
    i32 ascent,descent,gap;
    if(word(os2,62)&128){
        ascent=adjust(signed_word(os2,68),sfnt_tag('h','a','s','c'));
        descent=adjust(signed_word(os2,70),sfnt_tag('h','d','s','c'));
        gap=adjust(signed_word(os2,72),sfnt_tag('h','l','g','p'));
        ascent=maximum(ascent,0);descent=maximum(-descent,0);
    }else{
        const i32 win_ascent=word(os2,74),win_descent=word(os2,76);
        if(!win_ascent&&!win_descent){ascent=maximum<i32>(h.ascender,0);descent=maximum<i32>(-static_cast<i32>(h.descender),0);gap=h.line_gap;}
        else{
            // The compatibility gap is derived from base fields, before MVAR.
            gap=maximum<i32>(0,static_cast<i32>(h.ascender)-h.descender+h.line_gap-win_ascent-win_descent);
            ascent=adjust(win_ascent,sfnt_tag('h','c','l','a'));
            descent=adjust(win_descent,sfnt_tag('h','c','l','d'));
        }
    }
    const i32 raw_underline=word(post,10),raw_strike=word(os2,26),fallback=(h.units_per_em+7)/14;
    const i32 underline_position=adjust(signed_word(post,8),sfnt_tag('u','n','d','o'));
    const i32 underline_size=raw_underline?adjust(raw_underline,sfnt_tag('u','n','d','s')):fallback;
    const i32 strike_position=adjust(signed_word(os2,28),sfnt_tag('s','t','r','o'));
    const i32 strike_size=adjust(raw_strike?raw_strike:raw_underline?raw_underline:fallback,
        raw_strike?sfnt_tag('s','t','r','s'):sfnt_tag('u','n','d','s'));
    if(status!=SfntStatus::ok)return status;
    *output={h.units_per_em,static_cast<u16>(ascent),static_cast<u16>(descent),static_cast<i16>(gap),
        static_cast<i16>(underline_position),static_cast<u16>(underline_size),static_cast<i16>(strike_position),static_cast<u16>(strike_size)};
    return SfntStatus::ok;
}
SfntStatus sfnt_font_line_parts(const SfntFontLayoutMetrics& m,float em,SfntFontLineParts* output) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);u32 bits=0;copy_bytes(&bits,&em,4);
    if(!m.units_per_em||!bits||bits>=0x7f800000u||em>65536)return SfntStatus::invalid_input;
    // Default layout rounds the product before dividing. Keep this order at
    // fractional heading sizes even under /fp:fast; double scaling differs.
    const auto scaled=[&](i32 units) noexcept {return _mm_cvtss_f32(_mm_div_ss(_mm_mul_ss(_mm_set_ss(static_cast<float>(units)),_mm_set_ss(em)),_mm_set_ss(static_cast<float>(m.units_per_em))));};
    *output={scaled(static_cast<i32>(m.ascent)+m.line_gap),scaled(m.descent)};return SfntStatus::ok;
}
SfntStatus sfnt_single_font_line(const SfntFontLayoutMetrics& m,float em,SfntSingleFontLine* output) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);SfntFontLineParts parts{};const auto status=sfnt_font_line_parts(m,em,&parts);if(status!=SfntStatus::ok)return status;
    *output={layout_add(parts.baseline,parts.descent),parts.baseline};return SfntStatus::ok;
}
}
