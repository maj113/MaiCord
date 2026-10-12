#include "native_source.hpp"
using namespace maicord;
namespace {
bool outline(void* context,u16 glyph,GlyphOutline* output) noexcept {
    auto& self=*static_cast<NativePaintContext*>(context);
    if(self.is_tt) {
        SfntOutlineScratch scratch{{self.points,65536},{self.ends,32768},{self.active,64},0,0,1000000};
        return sfnt_truetype_outline(self.tt,glyph,&scratch,output,SfntOutlineMode::colr_commands)==SfntStatus::ok;
    }
    return cff_outline(self.cff,glyph,output)==SfntStatus::ok;
}
}
bool NativePaintContext::open(const wchar_t* path) noexcept {
    file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER size{};
    if(!GetFileSizeEx(file,&size) || size.QuadPart<=0 || size.QuadPart>0xffffffffll)return false;
    mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);
    if(!mapping)return false;
    bytes=MapViewOfFile(mapping,FILE_MAP_READ,0,0,0);
    if(!bytes || sfnt_open({static_cast<const u8*>(bytes),static_cast<u32>(size.QuadPart)},0,&face)!=SfntStatus::ok)return false;
    is_tt=sfnt_truetype_open(face,&tt)==SfntStatus::ok;
    if(!is_tt && cff_open(face,&cff)!=SfntStatus::ok)return false;
    if(colr_open(face,&color)!=SfntStatus::ok || colr_variation_data_open(color,&variation_data)!=SfntStatus::ok)return false;
    double coordinates[64]{};
    if(variation_data.store.axis_count>64 ||
       colr_variation_instance_prepare(variation_data,{coordinates,variation_data.store.axis_count},{scalars,32767},&instance,1000000,
                                       ColrVariationMode::paint_reader)!=SfntStatus::ok)return false;
    cursor.font=&color;cursor.variation=&instance;cursor.frames={frames,65};cursor.stop_storage={stops,65535};
    cursor.outline_context=this;cursor.outline_provider=outline;cursor.foreground={1,1,1,1};cursor.palette=0;
    cursor.outline_capacity_font=is_tt?&tt:nullptr;
    cursor.inverse_em=1.f/(is_tt?tt.metrics.units_per_em:cff.units_per_em);
    return true;
}
bool NativePaintContext::build(ColrPaintProgram* program,u16 glyph,float em) noexcept {
    ColrPaintElement root{};ColrRect clip{};ColrPaintSource callbacks{};
    return colr_source_begin(&cursor,glyph,1000000,&root,&clip,&callbacks)==SfntStatus::ok &&
           colr_program_build(program,callbacks,root,clip,em);
}
void NativePaintContext::close() noexcept {
    if(bytes)UnmapViewOfFile(bytes);
    if(mapping)CloseHandle(mapping);
    if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);
    bytes=nullptr;mapping=nullptr;file=INVALID_HANDLE_VALUE;
}
