#include "maicord/text/font_prepare.hpp"
#include "maicord/core/memory.hpp"
namespace maicord {namespace {
template<class T> Slice<T> take(Arena& a,u32 n) noexcept {return {n?arena_allocate<T>(&a,n):nullptr,n};}
NativeFontResult failure(SfntStatus s) noexcept {return {NativeFontError::font,s,0};}
}
NativeFontResult native_font_prepare(NativeFontSession* session,NativeFontHandle face,ConstSlice<FontAxisSetting> settings,u32 simulations,bool outline_instructions,Arena* scratch,NativeFontInstanceHandle* output,bool* oblique,bool* bold) noexcept {
    if(oblique)*oblique=false;if(bold)*bold=false;if(output)reset_value(output);
    if(!session||!scratch||!scratch->base||!output||(settings.count&&!settings.data))return {NativeFontError::invalid_input,SfntStatus::invalid_input,0};
    const auto* selected=native_font_get(*native_font_session_fonts(session),face);if(!selected)return {NativeFontError::invalid_input,SfntStatus::invalid_input,0};
    if(simulations>3||((simulations&2)&&!oblique)||((simulations&1)&&!bold))return failure(SfntStatus::unsupported);
    const auto& font=selected->geometry;if((simulations&1)&&(font.kind!=FontGeometryKind::truetype||font.gvar.table.data))return failure(SfntStatus::unsupported);
    bool instructions=false;TtOutlineRequirements requirements{};
    if(outline_instructions&&font.kind==FontGeometryKind::truetype){SfntStatus status=SfntStatus::ok;
        // Bold prep policy affects widening even when glyphs have no bytecode.
        if(simulations&1){ConstSlice<u8> prep{};status=sfnt_table(font.face,sfnt_tag('p','r','e','p'),&prep);if(status!=SfntStatus::ok&&status!=SfntStatus::missing_table)return failure(status);if(prep.count)instructions=true;}
        if(!instructions){status=sfnt_truetype_has_instructions(font.outline.truetype,&instructions);if(status!=SfntStatus::ok)return failure(status);}
        if(instructions){status=tt_outline_requirements(font.face,&requirements);if(status!=SfntStatus::ok)return failure(status);}}
    NativeFontPrepareScratch prepare{take<AxisSegmentMap>(*scratch,font.axes.axis_count),take<double>(*scratch,font.axes.axis_count),
        take<i32>(*scratch,instructions?maximum<u32>(64,requirements.stack):0),take<TtCallFrame>(*scratch,instructions?64:0)};
    const auto result=native_font_session_instance(session,face,settings,instructions,prepare,output);
    if(result.ok()){if(oblique)*oblique=(simulations&2)!=0;if(bold)*bold=(simulations&1)!=0;}return result;
}
}
