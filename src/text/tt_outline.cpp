#include "maicord/text/tt_outline.hpp"
#include "maicord/text/font_geometry.hpp"
#include "maicord/core/byte_order.hpp"
namespace maicord {namespace {
bool charge(u32* work,u64 cost) noexcept {if(cost>*work)return false;*work-=static_cast<u32>(cost);return true;}
template<class T> bool room(Slice<T> array,u32 count) noexcept {return array.count>=count && (!count || array.data);}
SfntStatus optional(const SfntFace& f,u32 tag,ConstSlice<u8>* bytes) noexcept {const auto s=sfnt_table(f,tag,bytes);return s==SfntStatus::missing_table?SfntStatus::ok:s;}
bool fixed(float value,i32* out) noexcept {const double v=static_cast<double>(value)*64;if(!(v>=-2147483648.0 && v<=2147483647.0))return false;*out=static_cast<i32>(v);return static_cast<double>(*out)==v;}
TtGraphicsState glyph_state(TtGraphicsState state) noexcept {
    const u32 control=state.instruction_control;if(control&2)state={};state.instruction_control=control;
    state.projection_x=state.freedom_x=state.dual_x=16384;state.projection_y=state.freedom_y=state.dual_y=0;
    state.rp[0]=state.rp[1]=state.rp[2]=0;state.zp[0]=state.zp[1]=state.zp[2]=1;state.loop=1;return state;
}
}
SfntStatus tt_outline_requirements(const SfntFace& face,TtOutlineRequirements* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};ConstSlice<u8> maxp{},cvt{};
    auto status=sfnt_table(face,sfnt_tag('m','a','x','p'),&maxp);if(status!=SfntStatus::ok)return status;
    status=optional(face,sfnt_tag('c','v','t',' '),&cvt);if(status!=SfntStatus::ok)return status;
    if(maxp.count<32)return SfntStatus::truncated;if(byte_load_be32(maxp.data)!=0x10000 || cvt.count%2)return SfntStatus::malformed;
    *output={byte_load_be16(maxp.data+24),byte_load_be16(maxp.data+18),cvt.count/2,byte_load_be16(maxp.data+20),byte_load_be16(maxp.data+16)};return SfntStatus::ok;
}
SfntStatus tt_outline_prepare(const FontGeometry& geometry,TtInterpreter s,TtOutlineInstance* output,TtExecution* execution,u32 work,u32* used) noexcept {
    struct Report {u32 initial;u32& left;u32* used;~Report() noexcept {if(used)*used=initial-left;}} report{work,work,used};
    if(output)*output={};if(execution)*execution={};if(!output || !execution)return SfntStatus::invalid_input;TtOutlineInstance result{};
    if(!geometry.face.file.data)return SfntStatus::invalid_input;
    if(geometry.kind!=FontGeometryKind::truetype)return SfntStatus::unsupported;
    const auto& face=geometry.face;result.font=geometry.outline.truetype;auto status=SfntStatus::ok;
    ConstSlice<u8> maxp{},cvt{},fpgm{},prep{},os2{},vertical{},cvar{};
    if((status=sfnt_table(face,sfnt_tag('m','a','x','p'),&maxp))!=SfntStatus::ok || (status=optional(face,sfnt_tag('c','v','t',' '),&cvt))!=SfntStatus::ok ||
       (status=optional(face,sfnt_tag('f','p','g','m'),&fpgm))!=SfntStatus::ok || (status=optional(face,sfnt_tag('p','r','e','p'),&prep))!=SfntStatus::ok ||
       (status=optional(face,sfnt_tag('v','m','t','x'),&vertical))!=SfntStatus::ok || (status=optional(face,sfnt_tag('c','v','a','r'),&cvar))!=SfntStatus::ok || (status=sfnt_table(face,sfnt_tag('O','S','/','2'),&os2))!=SfntStatus::ok)return status;
    if(cvar.data)return SfntStatus::unsupported;if(maxp.count<32 || os2.count<74)return SfntStatus::truncated;
    if(byte_load_be32(maxp.data)!=0x10000 || cvt.count%2)return SfntStatus::malformed;
    if(byte_load_be16(maxp.data+22))return SfntStatus::unsupported;
    const u32 twilight=byte_load_be16(maxp.data+16),storage=byte_load_be16(maxp.data+18),functions=byte_load_be16(maxp.data+20),stack=byte_load_be16(maxp.data+24),values=cvt.count/2;
    if(!room(s.stack,stack)||!room(s.storage,storage)||!room(s.cvt,values)||!room(s.functions,functions)||!room(s.twilight,twilight))return SfntStatus::output_too_small;
    if(!charge(&work,static_cast<u64>(storage)+values+functions+twilight))return SfntStatus::resource_limit;
    // Some required fonts declare zero stack while prep sets scan controls.
    // Use the bounded caller-provided preparation stack; glyph execution still
    // uses the declared capacity stored in the prepared instance below.
    s.storage.count=storage;s.cvt.count=values;s.functions.count=functions;s.twilight.count=twilight;s.glyph={};s.contour_ends={};s.units_per_em=result.font.metrics.units_per_em;s.graphics={};s.stack_count=0;
    for(u32 i=0;i<values;++i)s.cvt[i]=static_cast<i16>(byte_load_be16(cvt.data+i*2))*64;
    for(u32 i=0;i<storage;++i)s.storage[i]=0;for(u32 i=0;i<functions;++i)s.functions[i]={};for(u32 i=0;i<twilight;++i)s.twilight[i]={};
    status=tt_execute(&s,fpgm,TtPhase::font,execution,work);work-=execution->operations;if(status!=SfntStatus::ok)return status;
    s.graphics={};s.stack_count=0;for(u32 i=0;i<twilight;++i)s.twilight[i]={};
    status=tt_execute(&s,prep,TtPhase::prep,execution,work);work-=execution->operations;
    // The Windows outline path disables glyph programs after prep calls an
    // undefined function (including Mongolian Baiti's absent fpgm). First-party
    // probes distinguish this from an absent but unused font program. Discard
    // the partial graphics state; unsupported opcodes and exhausted resources
    // remain errors, so missing engine support cannot select raw outlines.
    if(status==SfntStatus::malformed&&execution->missing_function){s.graphics={};s.graphics.instruction_control=1;result.prep_recovered_missing_function=true;}
    else if(status!=SfntStatus::ok)return status;
    if(vertical.data && !(s.graphics.instruction_control&1)){
        result.reject_glyph_programs=true;s.graphics.instruction_control|=1;
    }
    if(result.font.gvar.data && !(s.graphics.instruction_control&1))return SfntStatus::unsupported;
    result.cvt=s.cvt;result.storage=s.storage;result.functions=s.functions;result.twilight=s.twilight;result.graphics=s.graphics;result.stack_capacity=static_cast<u16>(stack);
    result.ascender=static_cast<i16>(byte_load_be16(os2.data+68));result.descender=static_cast<i16>(byte_load_be16(os2.data+70));*output=result;return SfntStatus::ok;
}
SfntStatus tt_outline_begin(TtOutlineContext* context,const SfntTrueType& font,u32* work) noexcept {
    if(!context||!context->instance||!work)return SfntStatus::invalid_input;context->execution={};const auto& instance=*context->instance;auto& s=context->scratch;
    if(font.glyf.data!=instance.font.glyf.data || font.glyf.count!=instance.font.glyf.count || font.loca.data!=instance.font.loca.data || font.loca.count!=instance.font.loca.count || font.loca_stride!=instance.font.loca_stride || font.bearing_equals_minimum!=instance.font.bearing_equals_minimum || font.metrics.table.data!=instance.font.metrics.table.data || font.metrics.table.count!=instance.font.metrics.table.count || font.metrics.glyph_count!=instance.font.metrics.glyph_count || font.metrics.long_metrics!=instance.font.metrics.long_metrics || font.metrics.units_per_em!=instance.font.metrics.units_per_em)return SfntStatus::invalid_input;
    if(!room(s.stack,instance.stack_capacity)||!room(s.storage,instance.storage.count)||!room(s.cvt,instance.cvt.count)||!room(s.twilight,instance.twilight.count))return SfntStatus::output_too_small;
    if((s.storage.data && s.storage.data==instance.storage.data)||(s.cvt.data && s.cvt.data==instance.cvt.data)||(s.twilight.data && s.twilight.data==instance.twilight.data))return SfntStatus::invalid_input;
    if(instance.graphics.instruction_control&1)return SfntStatus::ok;
    if(!charge(work,static_cast<u64>(instance.storage.count)+instance.cvt.count+instance.twilight.count))return SfntStatus::resource_limit;
    for(u32 i=0;i<instance.storage.count;++i)s.storage[i]=instance.storage[i];for(u32 i=0;i<instance.cvt.count;++i)s.cvt[i]=instance.cvt[i];for(u32 i=0;i<instance.twilight.count;++i)s.twilight[i]=instance.twilight[i];return SfntStatus::ok;
}
SfntStatus tt_outline_apply(TtOutlineContext* context,u16 glyph,ConstSlice<u8> program,Slice<SfntOutlinePoint> points,ConstSlice<u32> contours,u32 point_origin,float* origin,float vertical_offset,u32* work) noexcept {
    if(!program.count)return SfntStatus::ok;if(!context||!context->instance||!origin||!work)return SfntStatus::invalid_input;const auto& instance=*context->instance;
    if(instance.reject_glyph_programs)return SfntStatus::unsupported;
    if(instance.graphics.instruction_control&1)return SfntStatus::ok;auto s=context->scratch;
    if(points.count>0xfffffffb || !room(s.glyph,points.count+4)||!room(context->contour_ends,contours.count))return SfntStatus::output_too_small;
    if(!charge(work,static_cast<u64>(points.count)*2+contours.count+4))return SfntStatus::resource_limit;
    s.glyph.count=points.count+4;s.cvt.count=instance.cvt.count;s.storage.count=instance.storage.count;s.twilight.count=instance.twilight.count;s.functions=instance.functions;s.stack.count=instance.stack_capacity;s.stack_count=0;s.units_per_em=instance.font.metrics.units_per_em;s.graphics=glyph_state(instance.graphics);
    for(u32 i=0;i<points.count;++i){i32 x=0,y=0;if(!fixed(points[i].x,&x)||!fixed(points[i].y,&y))return SfntStatus::unsupported;s.glyph[i]={x,y,x,y,0,static_cast<u8>(points[i].flags&1),0};}
    for(u32 i=0;i<contours.count;++i){if(contours[i]<point_origin || contours[i]-point_origin>=points.count)return SfntStatus::malformed;context->contour_ends[i]=contours[i]-point_origin;}
    s.contour_ends={context->contour_ends.data,contours.count};SfntGlyphMetrics metrics{};auto status=sfnt_horizontal_metrics(instance.font.metrics,glyph,&metrics);if(status!=SfntStatus::ok)return status;
    i32 left=0;if(!fixed(-*origin,&left))return SfntStatus::unsupported;const i64 right=static_cast<i64>(left)+metrics.advance*64,center=static_cast<i64>(left)+(metrics.advance/2)*64;
    if(right>2147483647 || center>2147483647)return SfntStatus::malformed;
    s.glyph[points.count]={left,0,left,0,0,1,0};s.glyph[points.count+1]={static_cast<i32>(right),0,static_cast<i32>(right),0,0,1,0};
    for(u32 i=0;i<2;++i){i32 y=0;if(!fixed((i?instance.descender:instance.ascender)-vertical_offset,&y))return SfntStatus::unsupported;s.glyph[points.count+2+i]={static_cast<i32>(center),y,static_cast<i32>(center),y,0,1,0};}
    status=tt_execute(&s,program,TtPhase::glyph,&context->execution,*work);*work-=context->execution.operations;if(status!=SfntStatus::ok)return status;
    for(u32 i=0;i<points.count;++i){points[i].x=s.glyph[i].x/64.f;points[i].y=s.glyph[i].y/64.f;points[i].flags=static_cast<u8>((points[i].flags&~1u)|s.glyph[i].on_curve);}
    *origin=-(s.glyph[points.count].x/64.f);return SfntStatus::ok;
}
} // namespace maicord
