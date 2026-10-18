#include "maicord/text/font_metrics.hpp"
#include "maicord/text/base.hpp"
#include "maicord/sys/simd.hpp"
namespace maicord {namespace {
u16 word(ConstSlice<u8> bytes,u32 offset) noexcept {return byte_load_be16(bytes.data+offset);}
i16 signed_word(ConstSlice<u8> bytes,u32 offset) noexcept {return static_cast<i16>(word(bytes,offset));}
#pragma float_control(precise,on,push)
SfntStatus varied(i32 base,u32 tag,const SfntMvar* mvar,ConstSlice<double> scalars,i32* output) noexcept {
    double delta=0;if(mvar){const auto status=sfnt_mvar_delta(*mvar,scalars,tag,&delta);if(status!=SfntStatus::ok)return status;}
    const double value=base+delta+.5;
    if(!(value>-2147483647.0&&value<2147483647.0))return SfntStatus::malformed;
    const i32 truncated=static_cast<i32>(value);*output=truncated-(truncated>value?1:0);return SfntStatus::ok;
}
#pragma float_control(pop)
}
SfntStatus sfnt_font_layout_metrics(const SfntFace& face,const SfntHorizontalMetrics& h,
    const SfntMvar* mvar,ConstSlice<double> scalars,SfntFontLayoutMetrics* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};if(!h.units_per_em)return SfntStatus::invalid_input;
    ConstSlice<u8> os2{},post{};auto status=sfnt_table(face,sfnt_tag('O','S','/','2'),&os2);if(status!=SfntStatus::ok)return status;
    status=sfnt_table(face,sfnt_tag('p','o','s','t'),&post);if(status!=SfntStatus::ok)return status;
    if(os2.count<78||post.count<12)return SfntStatus::truncated;
    i32 ascent=0,descent=0,gap=0;
    const auto adjust=[&](i32 base,u32 tag,i32* value) noexcept {return varied(base,tag,mvar,scalars,value);};
    if(word(os2,62)&128){
        status=adjust(signed_word(os2,68),sfnt_tag('h','a','s','c'),&ascent);if(status!=SfntStatus::ok)return status;
        status=adjust(signed_word(os2,70),sfnt_tag('h','d','s','c'),&descent);if(status!=SfntStatus::ok)return status;
        status=adjust(signed_word(os2,72),sfnt_tag('h','l','g','p'),&gap);if(status!=SfntStatus::ok)return status;
        ascent=maximum(ascent,0);descent=maximum(-descent,0);
    }else{
        const i32 win_ascent=word(os2,74),win_descent=word(os2,76);
        if(!win_ascent&&!win_descent){ascent=maximum<i32>(h.ascender,0);descent=maximum<i32>(-static_cast<i32>(h.descender),0);gap=h.line_gap;}
        else{
            // The compatibility gap is derived from base fields, before MVAR.
            gap=maximum<i32>(0,static_cast<i32>(h.ascender)-h.descender+h.line_gap-win_ascent-win_descent);
            status=adjust(win_ascent,sfnt_tag('h','c','l','a'),&ascent);if(status!=SfntStatus::ok)return status;
            status=adjust(win_descent,sfnt_tag('h','c','l','d'),&descent);if(status!=SfntStatus::ok)return status;
        }
    }
    const i32 raw_underline=word(post,10),raw_strike=word(os2,26),fallback=(h.units_per_em+7)/14;
    i32 underline_position=0,underline_size=fallback,strike_position=0,strike_size=0;
    status=adjust(signed_word(post,8),sfnt_tag('u','n','d','o'),&underline_position);if(status!=SfntStatus::ok)return status;
    if(raw_underline){status=adjust(raw_underline,sfnt_tag('u','n','d','s'),&underline_size);if(status!=SfntStatus::ok)return status;}
    status=adjust(signed_word(os2,28),sfnt_tag('s','t','r','o'),&strike_position);if(status!=SfntStatus::ok)return status;
    status=adjust(raw_strike?raw_strike:raw_underline?raw_underline:fallback,
        raw_strike?sfnt_tag('s','t','r','s'):sfnt_tag('u','n','d','s'),&strike_size);if(status!=SfntStatus::ok)return status;
    *output={h.units_per_em,static_cast<u16>(ascent),static_cast<u16>(descent),static_cast<i16>(gap),
        static_cast<i16>(underline_position),static_cast<u16>(underline_size),static_cast<i16>(strike_position),static_cast<u16>(strike_size)};
    return SfntStatus::ok;
}
SfntStatus sfnt_font_line_parts(const SfntFontLayoutMetrics& m,float em,SfntFontLineParts* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};u32 bits=0;copy_bytes(&bits,&em,4);
    if(!m.units_per_em||!bits||bits>=0x7f800000u||em>65536)return SfntStatus::invalid_input;
    // Default layout rounds the product before dividing. Keep this order at
    // fractional heading sizes even under /fp:fast; double scaling differs.
    const auto size=_mm_set_ss(em),units=_mm_set_ss(static_cast<float>(m.units_per_em));
    const auto baseline=_mm_div_ss(_mm_mul_ss(_mm_set_ss(static_cast<float>(static_cast<i32>(m.ascent)+m.line_gap)),size),units);
    const auto descent=_mm_div_ss(_mm_mul_ss(_mm_set_ss(static_cast<float>(m.descent)),size),units);
    *output={_mm_cvtss_f32(baseline),_mm_cvtss_f32(descent)};return SfntStatus::ok;
}
SfntStatus sfnt_single_font_line(const SfntFontLayoutMetrics& m,float em,SfntSingleFontLine* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};SfntFontLineParts parts{};const auto status=sfnt_font_line_parts(m,em,&parts);if(status!=SfntStatus::ok)return status;
    *output={_mm_cvtss_f32(_mm_add_ss(_mm_set_ss(parts.baseline),_mm_set_ss(parts.descent))),parts.baseline};return SfntStatus::ok;
}
SfntStatus sfnt_simulate_metrics(const SfntDesignGlyphMetrics& base,u16 units,
    FontSimulation simulation,SfntDesignGlyphMetrics* output) noexcept {
    if(!output)return SfntStatus::invalid_input;
    const auto input=base;*output={};
    if(units<16||units>16384||static_cast<u8>(simulation)>3)return SfntStatus::invalid_input;
    const i64 width=static_cast<i64>(input.advance_width)-input.left_bearing-input.right_bearing;
    const i64 height=static_cast<i64>(input.advance_height)-input.top_bearing-input.bottom_bearing;
    if(width<0||height<0)return SfntStatus::malformed;
    i64 left=input.left_bearing,advance=input.advance_width,right=input.right_bearing;
    i64 top=input.top_bearing,vertical_advance=input.advance_height,origin=input.vertical_origin;
    if(width&&height){
        if(static_cast<u8>(simulation)&1){
            const i64 amount=(static_cast<u32>(units)+25)/50;
            if(advance)advance+=amount;else right-=amount;
            if(vertical_advance){vertical_advance+=amount;origin+=amount;}else top-=amount;
        }
        if(static_cast<u8>(simulation)&2){
            const auto shear=[](i64 y) noexcept {const i64 value=y*87;return value/256-(value%256<0?1:0);};
            left+=shear(origin+input.bottom_bearing-vertical_advance);
            right-=shear(origin-top);
        }
    }
    const auto signed_field=[](i64 v) noexcept {return v>=-2147483648ll&&v<=2147483647ll;};
    if(!signed_field(left)||!signed_field(right)||!signed_field(top)||!signed_field(origin)||
       advance>0xffffffffll||vertical_advance>0xffffffffll)return SfntStatus::malformed;
    *output={static_cast<i32>(left),static_cast<u32>(advance),static_cast<i32>(right),static_cast<i32>(top),
        static_cast<u32>(vertical_advance),input.bottom_bearing,static_cast<i32>(origin)};
    return SfntStatus::ok;
}
}
