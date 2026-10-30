#include "maicord/text/font_store.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/base.hpp"
#include "font_path_stat.hpp"
#include "maicord/core/win32_kernel.hpp"
namespace maicord {namespace {
bool valid(const NativeFontStore& s) noexcept {return s.files.data && s.files.count && s.faces.data && s.faces.count;}
NativeFontFaceSlot* slot(const NativeFontStore& s,NativeFontHandle h) noexcept {
    if(!h.slot||h.slot>s.faces.count||!h.generation)return nullptr;
    auto& f=s.faces[h.slot-1];return f.references&&f.generation==h.generation?&f:nullptr;
}
void close(NativeFontFileSlot& f) noexcept {
    if(f.bytes.data)UnmapViewOfFile(f.bytes.data);
    if(f.file && f.file!=INVALID_HANDLE_VALUE)CloseHandle(f.file);
    reset_value(&f);
}
NativeFontResult result(NativeFontError error,SfntStatus font=SfntStatus::ok,u32 native=0) noexcept {return {error,font,native};}
}
NativeFontResult native_font_acquire(NativeFontStore* store,const wchar_t* path,u32 index,NativeFontHandle* output,NativeFontStoreGrow grow,void* context) noexcept {
    if(output)reset_value(output);
    if(!store||!valid(*store)||!path||!*path||!output)return result(NativeFontError::invalid_input);
    NativeFontFileSlot opened{};
    struct Cleanup {NativeFontFileSlot& f;~Cleanup() noexcept {close(f);}} cleanup{opened};
    // Keep the read handle alive with no write/delete sharing. All borrowed
    // table spans refer to these same bytes until the last face is released.
    opened.file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(opened.file==INVALID_HANDLE_VALUE)return result(NativeFontError::io,SfntStatus::ok,thread_last_error());
    NativeFontFileStamp stamp{};
    if(!font_path_stat::read_handle(opened.file,&stamp)){LARGE_INTEGER length{};
        if(GetFileType(opened.file)!=FILE_TYPE_DISK)return result(NativeFontError::io,SfntStatus::ok,ERROR_FILE_INVALID);
        if(!GetFileSizeEx(opened.file,&length)||!GetFileInformationByHandleEx(opened.file,FileIdInfo,&stamp.identity,sizeof(stamp.identity)))
            return result(NativeFontError::io,SfntStatus::ok,thread_last_error());
        stamp.bytes=static_cast<u64>(length.QuadPart);} // A negative size fails the limit below.
    if(!stamp.bytes || stamp.bytes>0xffffffffull)return result(NativeFontError::font,SfntStatus::unsupported);
    opened.identity=stamp.identity;
    u32 file_index=store->files.count,free_file=store->files.count,free_face=store->faces.count;
    for(u32 i=0;i<store->files.count;++i){const auto& f=store->files[i];
        if(!f.faces){if(free_file==store->files.count)free_file=i;continue;}
        if(bytes_equal(&f.identity,&opened.identity,sizeof(opened.identity))){file_index=i;break;}}
    for(u32 i=0;i<store->faces.count;++i){auto& f=store->faces[i];
        if(!f.references){if(free_face==store->faces.count && f.generation!=0xffffffffu)free_face=i;continue;}
        if(f.file_slot==file_index && f.face.geometry.face.face_index==index){
            if(f.references==0xffffffffu)return result(NativeFontError::capacity);
            ++f.references;*output={i+1,f.generation};return result(NativeFontError::none);}}
    // Preserve the old end indices: growth appends the first usable slots there.
    // Identity lookup and the opened read handle stay valid across that growth.
    const bool new_file=file_index==store->files.count;
    const bool need_file=new_file&&free_file==store->files.count,need_face=free_face==store->faces.count;
    if(need_file||need_face){
        if(!grow||!grow(context,need_file,need_face)||free_face>=store->faces.count||
            (new_file&&free_file>=store->files.count))return result(NativeFontError::capacity);
    }
    ConstSlice<u8> bytes{};
    if(new_file){
        const HANDLE mapping=CreateFileMappingW(opened.file,nullptr,PAGE_READONLY,0,0,nullptr);
        if(!mapping)return result(NativeFontError::io,SfntStatus::ok,thread_last_error());
        const auto* data=static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));
        const u32 error=data?0:thread_last_error();CloseHandle(mapping);
        if(!data)return result(NativeFontError::io,SfntStatus::ok,error);
        opened.bytes={data,static_cast<u32>(stamp.bytes)};bytes=opened.bytes;
    }else bytes=store->files[file_index].bytes;
    auto& target=store->faces[free_face];SfntFace face{};auto status=sfnt_open(bytes,index,&face); // The slot's face is zero; failure clears it again.
    if(status==SfntStatus::ok)status=font_geometry_open(face,&target.face.geometry);
    if(status==SfntStatus::ok)status=sfnt_cmap_open(face,&target.face.cmap);
    if(status!=SfntStatus::ok){reset_value(&target.face);return result(NativeFontError::font,status);}
    if(new_file){file_index=free_file;store->files[file_index]=opened;reset_value(&opened);}
    target.file_slot=file_index;target.references=1;++target.generation;++store->files[file_index].faces;*output={free_face+1,target.generation};return result(NativeFontError::none);
}
bool native_font_retain(NativeFontStore* s,NativeFontHandle h) noexcept {
    if(!s)return false;auto* f=slot(*s,h);if(!f||f->references==0xffffffffu)return false;++f->references;return true;
}
bool native_font_release(NativeFontStore* s,NativeFontHandle h) noexcept {
    if(!s)return false;auto* f=slot(*s,h);if(!f)return false;
    if(--f->references)return true;
    auto& file=s->files[f->file_slot];const u32 generation=f->generation;reset_value(f);f->generation=generation;
    if(!--file.faces)close(file);return true;
}
const NativeFontFace* native_font_get(const NativeFontStore& s,NativeFontHandle h) noexcept {
    const auto* f=slot(s,h);return f?&f->face:nullptr;
}
NativeFontStoreStats native_font_stats(const NativeFontStore& s) noexcept {
    NativeFontStoreStats stats{};if(!valid(s))return stats;
    for(u32 i=0;i<s.files.count;++i){const auto& f=s.files[i];if(f.faces){++stats.files;stats.mapped_bytes+=f.bytes.count;}}
    for(u32 i=0;i<s.faces.count;++i){const auto& f=s.faces[i];if(f.references){++stats.faces;stats.references=f.references>0xffffffffu-stats.references?0xffffffffu:stats.references+f.references;}}
    return stats;
}
} // namespace maicord
