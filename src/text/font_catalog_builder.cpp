#include "maicord/text/font_catalog_builder.hpp"
#include "font_catalog_append.hpp"

#include <Windows.h>

namespace maicord {namespace {
template<class T> bool grow(Arena& arena,Slice<T>& array,u32 required,u32 limit) noexcept {
    if(required<=array.count)return true;if(required>limit)return false;
    const u32 target=static_cast<u32>(minimum<u64>(limit,maximum<u64>(required,array.count?static_cast<u64>(array.count)*2:8)));
    auto* extra=arena_allocate<T>(&arena,target-array.count);if(!extra)return false;if(!array.data)array.data=extra;array.count=target;return true;
}
template<class T> bool copy(Arena& arena,Slice<T> source,u32 count,Slice<T>* output) noexcept {
    if(count>source.count||count>0xffffffffu/sizeof(T)||(count&&!source.data))return false;
    auto* target=count?arena_allocate<T>(&arena,count):nullptr;if(count&&!target)return false;
    if(count)copy_bytes(target,source.data,count*sizeof(T));*output={target,count};return true;
}
}
bool native_font_catalog_builder_initialize(NativeFontCatalogBuilder* b,NativeFontCatalogCounts limits) noexcept {
    if(!b||b->files.base||b->faces.base||b->candidates.base||b->settings.base||b->strings.base||!limits.files||!limits.faces||!limits.candidates||!limits.strings)return false;
    SYSTEM_INFO info{};GetSystemInfo(&info);const usize page=info.dwPageSize;
    if(!page||(page&(page-1)))return false;
    const u32 counts[]{limits.files,limits.faces,limits.candidates,limits.settings,limits.strings};
    constexpr u32 sizes[]{sizeof(NativeFontCatalogFile),sizeof(NativeFontCatalogFace),sizeof(NativeFontCatalogCandidate),sizeof(FontAxisSetting),sizeof(u16)};
    usize bytes[5]{},total=0;
    for(u32 i=0;i<5;++i){if(counts[i]>0xffffffffu/sizes[i])return false;
        const usize size=static_cast<usize>(counts[i])*sizes[i];if(size>~usize{0}-(page-1))return false;
        bytes[i]=(size+page-1)&~(page-1);if(bytes[i]>~usize{0}-total)return false;total+=bytes[i];}
    auto* base=static_cast<u8*>(VirtualAlloc(nullptr,total,MEM_RESERVE,PAGE_READWRITE));if(!base)return false;
    Arena* pools[]{&b->files,&b->faces,&b->candidates,&b->settings,&b->strings};
    // Stable, disjoint slices share one reservation. Unused bytes retain OS zero
    // initialization; individual pools never rewind or release storage.
    for(u32 i=0;i<5;++i){if(bytes[i])*pools[i]={base,bytes[i],0,0,0,page,0};base+=bytes[i];}
    b->limits=limits;return true;
}
void native_font_catalog_builder_release(NativeFontCatalogBuilder* b) noexcept {
    if(!b)return;if(b->files.base)VirtualFree(b->files.base,0,MEM_RELEASE);*b={};
}
NativeFontResult native_font_catalog_builder_add(NativeFontCatalogBuilder* b,const wchar_t* path,Slice<u16> scratch,u32* file_index,u32 maximum_faces,u32 work_per_face,bool default_face_only,NativeFontFileStamp* stamp,bool* no_reparse) noexcept {
    if(no_reparse)*no_reparse=false;
    if(file_index)*file_index=0xffffffffu;if(!b||!b->files.base||!file_index)return {NativeFontError::invalid_input,SfntStatus::invalid_input,0};
    Slice<u16> names=scratch;Slice<u8> metadata{};
    if(scratch.data&&scratch.count>8192&&scratch.count<=32768){names.count-=8192;metadata={reinterpret_cast<u8*>(scratch.data+names.count),16384};}
    const auto extend=[](void* context,NativeFontCatalogCounts needed) noexcept {
        auto& b=*static_cast<NativeFontCatalogBuilder*>(context);
        return grow(b.files,b.catalog.files,needed.files,b.limits.files)&&grow(b.faces,b.catalog.faces,needed.faces,b.limits.faces)&&
            grow(b.candidates,b.catalog.candidates,needed.candidates,b.limits.candidates)&&grow(b.settings,b.catalog.settings,needed.settings,b.limits.settings)&&
            grow(b.strings,b.catalog.strings,needed.strings,b.limits.strings);
    };
    // Extend stable arrays during the same append. Capacity growth must not
    // reopen the file, query its identity again, or reparse completed faces.
    for(;;){const auto result=native_font_catalog_append(&b->catalog,path,names,file_index,maximum_faces,work_per_face,nullptr,default_face_only,metadata,{b,extend},stamp,no_reparse);
        if(metadata.count&&result.error==NativeFontError::font&&result.font_status==SfntStatus::output_too_small){names=scratch;metadata={};continue;}
        return result;
    }

}
NativeFontResult native_font_catalog_publish(const NativeFontCatalog& source,Arena* arena,NativeFontCatalog* output) noexcept {
    if(output)*output={};if(!output||!arena||!arena->base)return {NativeFontError::invalid_input,SfntStatus::invalid_input,0};
    const auto mark=arena_mark(arena);NativeFontCatalog c{};c.count=source.count;
    if(!copy(*arena,source.files,c.count.files,&c.files)||!copy(*arena,source.faces,c.count.faces,&c.faces)||!copy(*arena,source.candidates,c.count.candidates,&c.candidates)||!copy(*arena,source.settings,c.count.settings,&c.settings)||!copy(*arena,source.strings,c.count.strings,&c.strings)){
        arena_rewind(arena,mark);return {NativeFontError::capacity,SfntStatus::output_too_small,0};}
    *output=c;return {NativeFontError::none,SfntStatus::ok,0};
}
}
