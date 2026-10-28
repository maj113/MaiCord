#include "maicord/text/truetype_outline.hpp"
#include "maicord/text/font_axes.hpp"
#include "maicord/text/font_geometry.hpp"
#include "maicord/core/byte_order.hpp"
#include "text_test_support.hpp"
namespace maicord {namespace {
#include "generated/gvar_outline_fixture.inc"
struct Reader {ConstSlice<u8> bytes;u32 at=0;bool word(u32* v) noexcept {if(bytes.count-at<4)return false;*v=byte_load_le32(bytes.data+at);at+=4;return true;}bool take(u32 n,ConstSlice<u8>* v) noexcept {if(n>bytes.count-at)return false;*v={bytes.data+at,n};at+=n;return true;}};
SfntOutlinePoint points[512];u32 ends[128];u16 active[64];GlyphPathCommand commands[1024];
u32 shared_points[512],private_points[512];i32 dx[512],dy[512];SfntGvarDelta deltas[1024];double shared_scalars[4096];
u64 fonts=0,outlines=0,command_checks=0,bound_checks=0,budgets=0,guards=0,invalid=0,geometry_checks=0;
bool fail(u32 f,u32 glyph,u32 code) noexcept {print(utf8_literal("FAIL font="));number(f);print(utf8_literal(" glyph="));number(glyph);print(utf8_literal(" code="));number(code);print(utf8_literal("\n"));return false;}
bool guard_table(ConstSlice<u8> table,u16 axis_count,u16 glyph_count,const SfntTrueType& font) noexcept {
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;if(table.count>page)return false;
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!memory)return false;DWORD old=0;if(!VirtualProtect(memory+page,page,PAGE_NOACCESS,&old)){VirtualFree(memory,0,MEM_RELEASE);return false;}
    const SfntOutlinePoint source[]{{10,20,1,{}},{31,20,1,{}},{80,70,1,{}},{80,120,1,{}},{50,180,1,{}},{-20,130,1,{}},{-20,70,1,{}},{-1,30,1,{}}};const u32 contour=7;const double coord=.5;
    u32 random=0x7138491;for(u32 i=0;i<table.count+2048;++i){const u32 size=i<table.count?i:table.count;auto* p=memory+page-size;if(size)copy_bytes(p,table.data,size);
        if(i>=table.count){random^=random<<13;random^=random>>17;random^=random<<5;p[random%size]^=static_cast<u8>((random>>16)|1);}
        SfntGvar g{};if(sfnt_gvar_open({p,size},axis_count,glyph_count,&g,4096)==SfntStatus::ok){SfntGvarInstance instance{};
            if(sfnt_gvar_instance_prepare(g,{&coord,1},{shared_scalars,4096},&instance,4096)==SfntStatus::ok){
                sfnt_gvar_deltas(instance,0,{source,8},{&contour,1},false,{{shared_points,512},{private_points,512},{dx,512},{dy,512}},{deltas,1024},4096);
                sfnt_gvar_deltas(instance,2,{nullptr,1},{},true,{{shared_points,512},{private_points,512},{dx,512},{dy,512}},{deltas,1024},4096);
                u16 advance=0;sfnt_gvar_design_advance(font,instance,0,&advance,4096);sfnt_gvar_design_advance(font,instance,2,&advance,4096);
            }}++guards;
    }VirtualFree(memory,0,MEM_RELEASE);return true;
}
bool corpus() noexcept {
    Reader reader{{gvar_outline_fixture,sizeof(gvar_outline_fixture)},8};u32 count=0;if(!bytes_equal(reader.bytes.data,"MCGVAR1\0",8) || !reader.word(&count))return false;
    for(u32 f=0;f<count;++f){u32 size=0,cases=0;ConstSlice<u8> bytes{};SfntFace face{};SfntTrueType font{};FontAxes axes{};FontAxisMappings mappings{};AxisSegmentMap maps[16];
        if(!reader.word(&size)||!reader.word(&cases)||!reader.take(size,&bytes)||sfnt_open(bytes,0,&face)!=SfntStatus::ok || sfnt_truetype_open(face,&font,true)!=SfntStatus::ok || font_axes_open(face,&axes)!=SfntStatus::ok || axes.axis_count>16 || font_axis_mappings_open(face,axes.axis_count,{maps,16},&mappings)!=SfntStatus::ok)return fail(f,0,1);
        SfntTrueType rejected{};if(sfnt_truetype_open(face,&rejected)!=SfntStatus::unsupported)return fail(f,0,2);++invalid;
        SfntGvar gvar{};u32 used=0;if(sfnt_gvar_open(font.gvar,axes.axis_count,font.metrics.glyph_count,&gvar,1000000,&used)!=SfntStatus::ok)return fail(f,0,3);
        const u32 full=used;for(u32 work=0;work<=full;++work){const auto status=sfnt_gvar_open(font.gvar,axes.axis_count,font.metrics.glyph_count,&gvar,work,&used);++budgets;if(status!=(work<full?SfntStatus::resource_limit:SfntStatus::ok)||used>work)return fail(f,0,4);}
        if(!guard_table(font.gvar,axes.axis_count,font.metrics.glyph_count,font))return fail(f,0,5);++fonts;
        FontGeometry geometry{};if(font_geometry_open(face,&geometry)!=SfntStatus::ok)return fail(f,0,20);
        for(u32 c=0;c<cases;++c){u32 weight=0,glyphs=0;if(!reader.word(&weight)||!reader.word(&glyphs))return false;const FontAxisSetting setting{sfnt_tag('w','g','h','t'),static_cast<double>(weight)};double normalized[16];
            if(font_axis_coordinates(axes,mappings,{&setting,1},{normalized,16})!=SfntStatus::ok)return false;SfntGvarInstance instance{};
            if(sfnt_gvar_instance_prepare(gvar,{normalized,axes.axis_count},{shared_scalars,4096},&instance,1000000,&used)!=SfntStatus::ok)return false;const u32 required=used;
            for(u32 work=0;work<=required;++work){const auto status=sfnt_gvar_instance_prepare(gvar,{normalized,axes.axis_count},{shared_scalars,4096},&instance,work,&used);++budgets;if(status!=(work<required?SfntStatus::resource_limit:SfntStatus::ok)||used>work)return fail(f,0,6);}
            const SfntOutlineVariation variation{&instance,{{shared_points,512},{private_points,512},{dx,512},{dy,512}},{deltas,1024}};
            double geometry_coordinates[16],geometry_scalars[4096];SfntGvarInstance geometry_variation{};FontGeometryInstance prepared{};
            const FontGeometryInstanceStorage storage{{maps,16},{geometry_coordinates,16},{geometry_scalars,4096},&geometry_variation};
            if(font_geometry_instance(geometry,{&setting,1},storage,&prepared,1000000,&used)!=SfntStatus::ok)return fail(f,0,21);const u32 geometry_work=used;
            for(u32 work=0;work<=geometry_work;++work){const auto status=font_geometry_instance(geometry,{&setting,1},storage,&prepared,work,&used);++budgets;if(status!=(work<geometry_work?SfntStatus::resource_limit:SfntStatus::ok) || used>work || (status!=SfntStatus::ok && prepared.font))return fail(f,0,22);}
            for(u32 i=0;i<glyphs;++i){u32 id=0,n=0,fill=0,has_bounds=0,bounds[4];ConstSlice<u8> expected{};if(!reader.word(&id)||!reader.word(&n)||!reader.word(&fill)||!reader.word(&has_bounds)||n>1024)return false;for(u32& b:bounds)if(!reader.word(&b))return false;if(!reader.take(n*28,&expected))return false;
                SfntOutlineScratch scratch{{points,512},{ends,128},{active,64},0,0,1000000};GlyphOutline outline{};outline.commands=commands;outline.capacity=1024;
                if(sfnt_truetype_outline(font,static_cast<u16>(id),&scratch,&outline,SfntOutlineMode::text,&variation)!=SfntStatus::ok || outline.count!=n || static_cast<u32>(outline.fill_rule)!=fill || outline.has_bounds!=(has_bounds!=0) || !bytes_equal(commands,expected.data,n*28))return fail(f,id,7);
                const float actual[]{outline.minimum_x,outline.minimum_y,outline.maximum_x,outline.maximum_y};if(!bytes_equal(actual,bounds,16))return fail(f,id,8);++outlines;command_checks+=n;bound_checks+=4;
                FontGeometryScratch geometry_scratch{scratch,variation.scratch,variation.deltas};
                if(font_geometry_outline(prepared,static_cast<u16>(id),&geometry_scratch,&outline)!=SfntStatus::ok || outline.count!=n || !bytes_equal(commands,expected.data,n*28))return fail(f,id,23);
                const float geometry_bounds[]{outline.minimum_x,outline.minimum_y,outline.maximum_x,outline.maximum_y};if(!bytes_equal(geometry_bounds,bounds,16))return fail(f,id,24);++geometry_checks;
                if(font_geometry_outline({},static_cast<u16>(id),&geometry_scratch,&outline)!=SfntStatus::invalid_input || outline.count || outline.commands!=commands)return fail(f,id,25);++invalid;
                if(font_geometry_outline(prepared,static_cast<u16>(id),nullptr,&outline)!=SfntStatus::invalid_input || outline.count)return fail(f,id,26);++invalid;
                // Find the successful work threshold, then exercise every lower
                // budget. Failure must publish no partial outline or point list.
                u32 low=0,high=1000000;while(low<high){const u32 middle=low+(high-low)/2;scratch.work_limit=middle;const auto s=sfnt_truetype_outline(font,static_cast<u16>(id),&scratch,&outline,SfntOutlineMode::text,&variation);if(s==SfntStatus::ok)high=middle;else if(s==SfntStatus::resource_limit)low=middle+1;else return fail(f,id,9);}
                for(u32 work=0;work<=low;++work){scratch.work_limit=work;const auto s=sfnt_truetype_outline(font,static_cast<u16>(id),&scratch,&outline,SfntOutlineMode::text,&variation);++budgets;if(s!=(work<low?SfntStatus::resource_limit:SfntStatus::ok) || (s!=SfntStatus::ok && (outline.count || scratch.point_count || scratch.contour_count)))return fail(f,id,10);}
                scratch.work_limit=1000000;outline.capacity=n-1;if(sfnt_truetype_outline(font,static_cast<u16>(id),&scratch,&outline,SfntOutlineMode::text,&variation)!=SfntStatus::output_too_small || outline.count)return fail(f,id,11);++invalid;outline.capacity=1024;
                auto short_variation=variation;short_variation.deltas.count=0;if(sfnt_truetype_outline(font,static_cast<u16>(id),&scratch,&outline,SfntOutlineMode::text,&short_variation)!=SfntStatus::output_too_small || outline.count)return fail(f,id,12);++invalid;
            }
        }
    }return reader.at==reader.bytes.count;
}
#include "gvar_metrics_units.inc"
int run() noexcept {const bool ok=gvar_metrics_units()&&gvar_metrics_guards()&&corpus();print(utf8_literal("fonts="));number(fonts);print(utf8_literal("\noutlines="));number(outlines);print(utf8_literal("\ncommands="));number(command_checks);print(utf8_literal("\nbounds="));number(bound_checks);print(utf8_literal("\ngeometry_instances="));number(geometry_checks);print(utf8_literal("\nbudgets="));number(budgets);print(utf8_literal("\nguards="));number(guards);print(utf8_literal("\ninvalid="));number(invalid);print(ok?utf8_literal("\nPASS native gvar outlines\n"):utf8_literal("\nFAIL native gvar outlines\n"));return ok?0:1;}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_gvar_outline_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
