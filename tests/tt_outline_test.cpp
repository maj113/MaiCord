#include "maicord/text/tt_outline.hpp"
#include "maicord/text/font_geometry.hpp"
#include "maicord/core/byte_order.hpp"
#include "test_output.hpp"
namespace maicord {namespace {
namespace bold_oblique_fixture {
#include "generated/tt_bold_oblique_fixture.inc"
}
namespace bold_fixture {
#include "generated/tt_bold_fixture.inc"
}
namespace oblique_fixture {
#include "generated/tt_oblique_fixture.inc"
}
namespace position_fixture {
#include "generated/tt_position_fixture.inc"
}
namespace normal_fixture {
#include "generated/tt_outline_fixture.inc"
#include "generated/tt_recovery_fixture.inc"
}
static_assert(sizeof(SfntOutlineScratch)==64);
static_assert(sizeof(TtOutlineInstance)==240,"Preparation recovery must fit the existing instance footprint.");
enum class OutlineMode : u8 { normal,bold,bold_oblique,oblique,position };
bool simulate_oblique=false,simulate_bold=false;
template<usize Count> bool mode_name(const wchar_t* value,u32 count,const wchar_t (&name)[Count]) noexcept {
    if(count!=Count-1)return false;
    for(u32 i=0;i<count;++i)if(value[i]!=name[i])return false;
    return true;
}
bool outline_mode(OutlineMode* mode) noexcept {
    const wchar_t* command=GetCommandLineW();bool quoted=false;
    while(*command){if(*command=='"')quoted=!quoted;else if(!quoted&&(*command==' '||*command=='\t'))break;++command;}
    if(quoted)return false;
    while(*command==' '||*command=='\t')++command;
    *mode=OutlineMode::normal;if(!*command)return true;
    quoted=*command=='"';if(quoted)++command;const wchar_t* begin=command;
    while(*command&&!(quoted?*command=='"':(*command==' '||*command=='\t')))++command;
    const u32 count=static_cast<u32>(command-begin);
    if(quoted){if(*command!='"')return false;++command;}
    while(*command==' '||*command=='\t')++command;
    if(*command)return false;
    if(mode_name(begin,count,L"normal"))*mode=OutlineMode::normal;
    else if(mode_name(begin,count,L"bold"))*mode=OutlineMode::bold;
    else if(mode_name(begin,count,L"bold-oblique"))*mode=OutlineMode::bold_oblique;
    else if(mode_name(begin,count,L"oblique"))*mode=OutlineMode::oblique;
    else if(mode_name(begin,count,L"position"))*mode=OutlineMode::position;
    else return false;
    return true;
}
bool fail(u32 item,u32 code) noexcept {test_write_text("FAIL font=");test_write_u64(item);test_write_text(" code=");test_write_u64(code);test_write_text("\n");return false;}
struct Reader {ConstSlice<u8> bytes;u32 at=0;bool word(u32* v) noexcept {if(bytes.count-at<4)return false;*v=byte_load_le32(bytes.data+at);at+=4;return true;}bool take(u32 n,ConstSlice<u8>* v) noexcept {if(n>bytes.count-at)return false;*v={bytes.data+at,n};at+=n;return true;}};
struct Expected {u32 id,count,fill,bounds[4],has_bounds;ConstSlice<u8> commands;};
i32 stack[256],prep_storage[128],prep_cvt[128],storage[128],cvt[128];TtFunction functions[128];TtCallFrame frames[32];TtPoint twilight[64],prep_twilight[64],vm_points[512];u32 vm_ends[128];
SfntOutlinePoint points[512];u32 ends[128];u16 active[64];GlyphPathCommand commands[1024];
u64 fonts=0,outlines=0,command_checks=0,bound_checks=0,budgets=0,invalid=0,vertical_checks=0;
u8 changed_font[4096];bool rejected_program_checked=false;
SfntStatus prepare(const FontGeometry& geometry,TtOutlineInstance* output,TtExecution* execution,u32 work=1000000,u32* used=nullptr) noexcept {
    TtInterpreter s{};s.stack={stack,256};s.storage={prep_storage,128};s.cvt={prep_cvt,128};s.functions={functions,128};s.frames={frames,32};s.twilight={prep_twilight,64};
    return tt_outline_prepare(geometry,&s,output,execution,work,used);
}
TtOutlineContext context(const TtOutlineInstance* instance) noexcept {TtOutlineContext c{};c.instance=instance;c.scratch.stack={stack,256};c.scratch.storage={storage,128};c.scratch.cvt={cvt,128};c.scratch.frames={frames,32};c.scratch.twilight={twilight,64};c.scratch.glyph={vm_points,512};c.contour_ends={vm_ends,128};return c;}
SfntStatus decode_outline(const FontGeometryInstance& instance,u16 glyph,FontGeometryScratch* scratch,GlyphOutline* output,u32 work=1000000) noexcept {
    return font_geometry_outline(instance,glyph,scratch,output,work,nullptr,simulate_oblique,simulate_bold);
}
bool equal(const GlyphOutline& actual,const Expected& expected) noexcept {const float bounds[]{actual.minimum_x,actual.minimum_y,actual.maximum_x,actual.maximum_y};return actual.count==expected.count && static_cast<u32>(actual.fill_rule)==expected.fill && actual.has_bounds==(expected.has_bounds!=0) && bytes_equal(bounds,expected.bounds,16) && bytes_equal(commands,expected.commands.data,expected.commands.count);}
bool vertical_policy(SfntFace face,const Expected (&expected)[4],bool reject) noexcept {
    u8 directory[512]{};const u32 tag=sfnt_tag('v','m','t','x');u32 count=0;bool inserted=false;
    if(face.directory.count+16>sizeof(directory))return false;
    for(u32 at=0;at<face.directory.count;at+=16){
        const u32 current=byte_load_be32(face.directory.data+at);
        if(current==tag)inserted=true;
        if(!inserted&&current>tag){byte_store_be32(directory+count,tag);byte_store_be32(directory+count+12,1);count+=16;inserted=true;}
        copy_bytes(directory+count,face.directory.data+at,16);count+=16;
    }
    if(!inserted){byte_store_be32(directory+count,tag);byte_store_be32(directory+count+12,1);count+=16;}
    face.directory={directory,count};TtOutlineInstance prepared{};TtExecution execution{};FontGeometry font{};FontGeometryInstance instance{};
    if(font_geometry_open(face,&font)!=SfntStatus::ok || prepare(font,&prepared,&execution)!=SfntStatus::ok || prepared.reject_glyph_programs!=reject || font_geometry_instance(font,{}, {},&instance)!=SfntStatus::ok)return false;
    auto ctx=context(&prepared);FontGeometryScratch scratch{{{points,512},{ends,128},{active,64},0,0,1000000},{},{},&ctx};GlyphOutline output{};output.commands=commands;output.capacity=1024;
    for(const auto& e:expected){
        scratch.instructions=nullptr;scratch.truetype.require_instructions=true;
        const auto discovered=decode_outline(instance,static_cast<u16>(e.id),&scratch,&output);
        if(discovered!=SfntStatus::ok&&discovered!=SfntStatus::instructions_required)return false;
        const bool instructed=discovered==SfntStatus::instructions_required;
        if(instructed&&(output.count||scratch.truetype.point_count||scratch.truetype.contour_count))return false;
        scratch.instructions=&ctx;
        const auto status=decode_outline(instance,static_cast<u16>(e.id),&scratch,&output);
        if(instructed&&reject){if(status!=SfntStatus::unsupported || output.count || scratch.truetype.point_count || scratch.truetype.contour_count)return false;}
        else if(status!=SfntStatus::ok || !equal(output,e))return false;
        ++vertical_checks;
    }
    return true;
}
bool corpus(ConstSlice<u8> fixture) noexcept {
    Reader r{fixture,8};u32 count=0;if(!bytes_equal(r.bytes.data,"MCTTGL1\0",8)||!r.word(&count))return false;
    for(u32 f=0;f<count;++f){u32 size=0,n=0;ConstSlice<u8> bytes{};SfntFace face{};FontGeometry font{};FontGeometryInstance instance{};
        if(!r.word(&size)||!r.word(&n)||n!=4||!r.take(size,&bytes)||sfnt_open(bytes,0,&face)!=SfntStatus::ok || font_geometry_open(face,&font)!=SfntStatus::ok || font_geometry_instance(font,{}, {},&instance)!=SfntStatus::ok)return fail(f,1);
        Expected expected[4]{};for(auto& e:expected){if(!r.word(&e.id)||!r.word(&e.count)||!r.word(&e.fill)||!r.word(&e.has_bounds)||e.count>1024)return fail(f,2);for(auto& b:e.bounds)if(!r.word(&b))return false;if(!r.take(e.count*28,&e.commands))return fail(f,3);}
        TtOutlineInstance prepared{};TtExecution execution{};u32 used=0;if(prepare(font,&prepared,&execution,1000000,&used)!=SfntStatus::ok)return fail(f,4);const u32 full=used;
        for(u32 work=0;work<=full;++work){const auto s=prepare(font,&prepared,&execution,work,&used);++budgets;if(used>work || s!=(work<full?SfntStatus::resource_limit:SfntStatus::ok) || (s!=SfntStatus::ok && prepared.font.glyf.data))return fail(f,5);}
        const bool recovered=execution.missing_function;
        if(prepared.prep_recovered_missing_function!=recovered || (recovered&&!(prepared.graphics.instruction_control&1)))return fail(f,16);
        auto ctx=context(&prepared);FontGeometryScratch scratch{{{points,512},{ends,128},{active,64},0,0,1000000},{},{},&ctx};GlyphOutline output{};output.commands=commands;output.capacity=1024;
        i32 original_cvt[128],original_storage[128];TtPoint original_twilight[64];copy_bytes(original_cvt,prep_cvt,sizeof(prep_cvt));copy_bytes(original_storage,prep_storage,sizeof(prep_storage));copy_bytes(original_twilight,prep_twilight,sizeof(prep_twilight));
        // Reverse order, then forward order, checks that root glyph mutations
        // never contaminate another root or the immutable prepared arrays.
        for(u32 pass=0;pass<2;++pass)for(u32 i=0;i<4;++i){const auto& e=expected[pass?i:3-i];
            scratch.instructions=nullptr;scratch.truetype.require_instructions=true;
            const auto discovered=decode_outline(instance,static_cast<u16>(e.id),&scratch,&output);
            if(discovered!=SfntStatus::ok&&discovered!=SfntStatus::instructions_required)return fail(f,19);
            if(discovered==SfntStatus::instructions_required&&(output.count||scratch.truetype.point_count||scratch.truetype.contour_count))return fail(f,20);
            scratch.instructions=&ctx;
            if(decode_outline(instance,static_cast<u16>(e.id),&scratch,&output)!=SfntStatus::ok || !equal(output,e))return fail(f,6);++outlines;command_checks+=e.count;bound_checks+=4;
            if(prepared.prep_recovered_missing_function!=recovered)return fail(f,17);
            if(!bytes_equal(original_cvt,prep_cvt,sizeof(prep_cvt)) || !bytes_equal(original_storage,prep_storage,sizeof(prep_storage)) || !bytes_equal(original_twilight,prep_twilight,sizeof(prep_twilight)))return fail(f,7);
            if(pass)continue;u32 low=0,high=1000000;while(low<high){const u32 work=low+(high-low)/2;const auto s=decode_outline(instance,static_cast<u16>(e.id),&scratch,&output,work);if(s==SfntStatus::ok)high=work;else if(s==SfntStatus::resource_limit)low=work+1;else return fail(f,8);}
            for(u32 work=0;work<=low;++work){const auto s=decode_outline(instance,static_cast<u16>(e.id),&scratch,&output,work);++budgets;if(s!=(work<low?SfntStatus::resource_limit:SfntStatus::ok) || (s!=SfntStatus::ok && (output.count||scratch.truetype.point_count||scratch.truetype.contour_count)))return fail(f,9);}
            output.capacity=e.count-1;if(decode_outline(instance,static_cast<u16>(e.id),&scratch,&output)!=SfntStatus::output_too_small || output.count)return fail(f,10);++invalid;output.capacity=1024;
        }
        auto other=prepared;other.font.glyf.count-=1;ctx.instance=&other;if(decode_outline(instance,0,&scratch,&output)!=SfntStatus::invalid_input || output.count)return fail(f,11);++invalid;ctx.instance=&prepared;
        ctx.scratch.cvt=prepared.cvt;if(prepared.cvt.count && (decode_outline(instance,0,&scratch,&output)!=SfntStatus::invalid_input || output.count))return fail(f,12);++invalid;
        if(!rejected_program_checked){
            const auto& tt=font.outline.truetype;const u32 glyph_begin=tt.loca_stride==4?byte_load_be32(tt.loca.data):byte_load_be16(tt.loca.data)*2u;
            const u8* glyph=tt.glyf.data+glyph_begin;const u32 at=10+byte_load_be16(glyph)*2u;
            if(byte_load_be16(glyph+at)){
                if(bytes.count>sizeof(changed_font))return fail(f,13);copy_bytes(changed_font,bytes.data,bytes.count);changed_font[static_cast<u32>(glyph-bytes.data)+at+2]=0x83;
                SfntFace changed_face{};FontGeometry changed_geometry{};FontGeometryInstance changed_instance{};TtOutlineInstance changed_prepared{};
                if(sfnt_open({changed_font,bytes.count},0,&changed_face)!=SfntStatus::ok || font_geometry_open(changed_face,&changed_geometry)!=SfntStatus::ok || font_geometry_instance(changed_geometry,{}, {},&changed_instance)!=SfntStatus::ok || prepare(changed_geometry,&changed_prepared,&execution)!=SfntStatus::ok)return fail(f,14);
                ctx=context(&changed_prepared);
                if(decode_outline(changed_instance,0,&scratch,&output)!=SfntStatus::unsupported || output.count || scratch.truetype.point_count || scratch.truetype.contour_count || ctx.execution.opcode!=0x83)return fail(f,15);
                ++invalid;rejected_program_checked=true;
            }
        }
        if(!vertical_policy(face,expected,prepared.reject_glyph_programs||!(prepared.graphics.instruction_control&1)))return fail(f,18);
        ++fonts;
    }return r.at==r.bytes.count;
}
int run() noexcept {
    OutlineMode mode{};if(!outline_mode(&mode)){test_write_line("FAIL outline mode");return 2;}
    ConstSlice<u8> fixture{normal_fixture::tt_outline_fixture,sizeof(normal_fixture::tt_outline_fixture)};
    switch(mode){
    case OutlineMode::normal:break;
    case OutlineMode::bold:fixture={bold_fixture::tt_outline_fixture,sizeof(bold_fixture::tt_outline_fixture)};break;
    case OutlineMode::bold_oblique:fixture={bold_oblique_fixture::tt_outline_fixture,sizeof(bold_oblique_fixture::tt_outline_fixture)};break;
    case OutlineMode::oblique:fixture={oblique_fixture::tt_outline_fixture,sizeof(oblique_fixture::tt_outline_fixture)};break;
    case OutlineMode::position:fixture={position_fixture::tt_outline_fixture,sizeof(position_fixture::tt_outline_fixture)};break;
    }
    simulate_oblique=mode==OutlineMode::oblique||mode==OutlineMode::bold_oblique;
    simulate_bold=mode==OutlineMode::bold||mode==OutlineMode::bold_oblique;
    bool ok=corpus(fixture);
    if(ok&&mode==OutlineMode::normal)ok=corpus({normal_fixture::tt_recovery_fixture,sizeof(normal_fixture::tt_recovery_fixture)});
    test_write_metric("fonts=",fonts);test_write_metric("outlines=",outlines);test_write_metric("commands=",command_checks);test_write_metric("bounds=",bound_checks);test_write_metric("budgets=",budgets);test_write_metric("invalid=",invalid);test_write_metric("vertical=",vertical_checks);test_write_line(ok?"PASS native instructed outlines":"FAIL native instructed outlines");return ok?0:1;}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_tt_outline_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
