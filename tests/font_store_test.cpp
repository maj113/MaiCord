#include "maicord/text/font_store.hpp"
#if defined(MAICORD_FONT_INSTANCE_TESTS)
#include "maicord/text/font_instance.hpp"
#endif
#include "maicord/core/byte_order.hpp"
namespace maicord {namespace {
#include "generated/tt_outline_fixture.inc"
void print(const char* p,u32 n) noexcept {DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),p,n,&written,nullptr);}
void number(u64 v) noexcept {char b[32];u32 n=0;do{b[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n/2;++i){const char c=b[i];b[i]=b[n-i-1];b[n-i-1]=c;}print(b,n);}
u32 checks=0;
bool check(bool v,u32 line) noexcept {++checks;if(!v){print("FAIL line=",10);number(line);print("\n",1);}return v;}
#define CHECK(v) do {if(!check((v),__LINE__))return false;}while(false)
NativeFontFileSlot files[2];NativeFontFaceSlot faces[3];NativeFontStore store{{files,2},{faces,3}};
wchar_t paths[5][MAX_PATH],reservation[MAX_PATH],directory[MAX_PATH];bool reserved=false,directory_created=false;u8 collection[32768];
bool write_file(const wchar_t* path,ConstSlice<u8> bytes) noexcept {
    const HANDLE h=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);if(h==INVALID_HANDLE_VALUE)return false;
    DWORD n=0;const bool ok=WriteFile(h,bytes.data,bytes.count,&n,nullptr)&&n==bytes.count;CloseHandle(h);return ok;
}
bool stats(u32 f,u32 faces_count,u32 references) noexcept {const auto s=native_font_stats(store);return s.files==f&&s.faces==faces_count&&s.references==references&&(f?s.mapped_bytes>0:s.mapped_bytes==0);}
bool same(NativeFontHandle a,NativeFontHandle b) noexcept {return a.slot==b.slot&&a.generation==b.generation;}
struct Growth {u32 calls,requested;bool files,faces,success;};
bool grow(void* context,bool need_files,bool need_faces) noexcept {
    auto& g=*static_cast<Growth*>(context);++g.calls;g.requested=(need_files?1u:0u)|(need_faces?2u:0u);
    if(need_files&&g.files&&store.files.count<2)++store.files.count;
    if(need_faces&&g.faces&&store.faces.count<3)++store.faces.count;
    return g.success;
}
bool growth_cases() noexcept {
    store.files.count=store.faces.count=1;NativeFontHandle a{},b{},c{},alias{};
    DWORD before=0,after=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    Growth g{};CHECK(native_font_acquire(&store,paths[2],0,&a,grow,&g).ok()&&!g.calls);
    const auto* view=native_font_get(store,a);
    CHECK(native_font_acquire(&store,paths[2],0,&alias,grow,&g).ok()&&same(a,alias)&&!g.calls);
    CHECK(native_font_release(&store,alias));
    faces[a.slot-1].references=0xffffffffu;
    const bool overflow=native_font_acquire(&store,paths[2],0,&alias,grow,&g).error==NativeFontError::capacity&&!alias.slot&&!g.calls;
    faces[a.slot-1].references=1;CHECK(overflow);
    // Refusal and an ineffective successful callback close the transient handle.
    CHECK(native_font_acquire(&store,paths[2],1,&b,grow,&g).error==NativeFontError::capacity&&!b.slot&&g.calls==1&&g.requested==2);
    g.success=true;
    CHECK(native_font_acquire(&store,paths[0],0,&b,grow,&g).error==NativeFontError::capacity&&!b.slot&&g.calls==2&&g.requested==3);
    // Partial growth remains available, but never publishes an incomplete face.
    g.files=true;
    CHECK(native_font_acquire(&store,paths[0],0,&b,grow,&g).error==NativeFontError::capacity&&!b.slot&&g.calls==3&&g.requested==3&&store.files.count==2&&store.faces.count==1);
    g.faces=true;g.success=false;
    CHECK(native_font_acquire(&store,paths[0],0,&b,grow,&g).error==NativeFontError::capacity&&!b.slot&&g.calls==4&&g.requested==2&&store.faces.count==2);
    CHECK(stats(1,1,1)&&GetProcessHandleCount(GetCurrentProcess(),&after)&&after==before+1&&native_font_get(store,a)==view);
    CHECK(native_font_acquire(&store,paths[0],0,&b,grow,&g).ok()&&g.calls==4);
    g.success=true;
    CHECK(native_font_acquire(&store,paths[2],1,&c,grow,&g).ok()&&g.calls==5&&g.requested==2&&stats(2,3,3));
    CHECK(native_font_get(store,a)==view&&view->geometry.face.file.data==native_font_get(store,c)->geometry.face.file.data);
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&after==before+2);
    CHECK(native_font_release(&store,a)&&native_font_release(&store,b)&&native_font_release(&store,c));
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&after==before&&stats(0,0,0));
    store.files.count=1;g={0,0,true,true,true};
    CHECK(native_font_acquire(&store,paths[0],0,&a,grow,&g).ok()&&!g.calls);
    CHECK(native_font_acquire(&store,paths[1],0,&b,grow,&g).ok()&&g.calls==1&&g.requested==1&&stats(2,2,2));
    CHECK(native_font_release(&store,a)&&native_font_release(&store,b));
    // Exhausted generations are skipped even when the store grows around them.
    faces[0].generation=0xffffffffu;store.files.count=store.faces.count=1;g={0,0,true,true,true};
    CHECK(native_font_acquire(&store,paths[0],0,&b,grow,&g).ok()&&b.slot==2&&g.calls==1&&g.requested==2);
    CHECK(native_font_release(&store,b));store.files.count=2;store.faces.count=3;return true;
}
bool outline(const NativeFontFace* face) noexcept {
    CHECK(face);u16 glyph=0;CHECK(sfnt_cmap_glyph(face->cmap,'a',0,&glyph)==SfntStatus::ok&&glyph);
    FontGeometryInstance instance{};CHECK(font_geometry_instance(face->geometry,{}, {},&instance)==SfntStatus::ok);
    SfntOutlinePoint points[64];u32 ends[16];u16 active[16];GlyphPathCommand commands[128];
    FontGeometryScratch scratch{{{points,64},{ends,16},{active,16},0,0,1000000},{},{}};GlyphOutline out{};out.commands=commands;out.capacity=128;
    CHECK(font_geometry_outline(instance,glyph,&scratch,&out)==SfntStatus::ok&&out.count&&out.has_bounds);return true;
}
#if defined(MAICORD_FONT_INSTANCE_TESTS)
#include "font_instance_cases.hpp"
#endif
bool run() noexcept {
    const ConstSlice<u8> font{tt_outline_fixture+20,byte_load_le32(tt_outline_fixture+12)};
    CHECK(font.count<16000 && font.count+20<sizeof(tt_outline_fixture));
    wchar_t temp[MAX_PATH];const DWORD temp_length=GetTempPathW(MAX_PATH,temp);CHECK(temp_length>0&&temp_length<MAX_PATH);
    reserved=GetTempFileNameW(temp,L"mcf",0,reservation)!=0;CHECK(reserved);
    u32 length=0;while(reservation[length])++length;CHECK(length+5<MAX_PATH);
    copy_bytes(directory,reservation,length*sizeof(wchar_t));copy_bytes(directory+length,L".dir",5*sizeof(wchar_t));
    directory_created=CreateDirectoryW(directory,nullptr)!=0;CHECK(directory_created);
    // Keep the reservation file until cleanup: no parallel test can reuse its
    // basename while we replace a child placeholder with a hard-link alias.
    for(auto& path:paths)CHECK(GetTempFileNameW(directory,L"\u03bbmc",0,path));
    CHECK(write_file(paths[0],font)&&write_file(paths[1],font));
    CHECK(DeleteFileW(paths[4])&&CreateHardLinkW(paths[4],paths[0],nullptr));
    const u32 offsets[]{20,(20+font.count+3)&~3u};const u32 size=offsets[1]+font.count;
    CHECK(size<sizeof(collection));byte_store_be32(collection,sfnt_tag('t','t','c','f'));byte_store_be32(collection+4,0x10000);byte_store_be32(collection+8,2);
    for(u32 i=0;i<2;++i){const u32 offset=offsets[i];byte_store_be32(collection+12+4*i,offset);copy_bytes(collection+offset,font.data,font.count);
        const u32 tables=byte_load_be16(font.data+4);CHECK(12+tables*16<=font.count);
        for(u32 t=0;t<tables;++t){u8* field=collection+offset+12+t*16+8;byte_store_be32(field,byte_load_be32(field)+offset);}}
    CHECK(write_file(paths[2],{collection,size}));
    NativeFontHandle a{},b{},alias{},c{},d{};
    DWORD handles_before=0,handles_after=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&handles_before));
    CHECK(native_font_acquire(&store,paths[0],0,&a).ok()&&stats(1,1,1));
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&handles_after)&&handles_after==handles_before+1);
    faces[a.slot-1].references=0xffffffffu;
    const bool overflow_rejected=!native_font_retain(&store,a)&&native_font_acquire(&store,paths[0],0,&b).error==NativeFontError::capacity&&!b.slot&&faces[a.slot-1].references==0xffffffffu;
    faces[a.slot-1].references=1;CHECK(overflow_rejected);
    const auto* view=native_font_get(store,a);CHECK(outline(view));
    MEMORY_BASIC_INFORMATION memory{};CHECK(VirtualQuery(view->geometry.face.file.data,&memory,sizeof(memory))==sizeof(memory)&&memory.Type==MEM_MAPPED&&memory.Protect==PAGE_READONLY);
    CHECK(native_font_acquire(&store,paths[4],0,&alias).ok()&&same(a,alias)&&stats(1,1,2));
    CHECK(native_font_get(store,alias)==view&&native_font_retain(&store,a)&&stats(1,1,3));
    HANDLE writer=CreateFileW(paths[0],GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    const DWORD write_error=GetLastError();if(writer!=INVALID_HANDLE_VALUE)CloseHandle(writer);CHECK(writer==INVALID_HANDLE_VALUE&&write_error==ERROR_SHARING_VIOLATION);
    CHECK(native_font_release(&store,a)&&native_font_release(&store,alias)&&stats(1,1,1)&&outline(view));
    CHECK(native_font_acquire(&store,paths[1],0,&b).ok()&&!same(a,b)&&stats(2,2,2));
    CHECK(native_font_acquire(&store,paths[2],0,&c).error==NativeFontError::capacity&&!c.slot&&stats(2,2,2));
    CHECK(native_font_release(&store,b)&&!native_font_get(store,b)&&stats(1,1,1));
    CHECK(native_font_acquire(&store,paths[2],0,&c).ok()&&native_font_acquire(&store,paths[2],1,&d).ok()&&stats(2,3,3));
    CHECK(native_font_get(store,c)->geometry.face.file.data==native_font_get(store,d)->geometry.face.file.data&&outline(native_font_get(store,d)));
    CHECK(native_font_acquire(&store,paths[4],0,&alias).ok()&&same(alias,a)&&stats(2,3,4)&&native_font_release(&store,alias));
    const auto stale=d;CHECK(native_font_release(&store,d)&&!native_font_release(&store,stale)&&!native_font_retain(&store,stale));
    const auto bad_face=native_font_acquire(&store,paths[2],2,&d);CHECK(bad_face.error==NativeFontError::font&&bad_face.font_status==SfntStatus::face_out_of_range&&!d.slot&&stats(2,2,2));
    CHECK(native_font_release(&store,c)&&native_font_release(&store,a)&&stats(0,0,0));
    const u8 invalid[]{0,0,0,0};CHECK(write_file(paths[0],{invalid,4}));
    CHECK(native_font_acquire(&store,paths[0],0,&b).error==NativeFontError::font&&!b.slot&&stats(0,0,0));
    CHECK(write_file(paths[0],font)&&native_font_acquire(&store,paths[0],0,&b).ok()&&!same(a,b));
    CHECK(!native_font_get(store,a)&&!native_font_retain(&store,a)&&!native_font_release(&store,a)&&outline(native_font_get(store,b)));
    CHECK(native_font_release(&store,b)&&stats(0,0,0));
    DWORD before=0,after=0;CHECK(GetProcessHandleCount(GetCurrentProcess(),&before));
    for(u32 i=0;i<128;++i){
        CHECK(native_font_acquire(&store,paths[3],0,&b).error==NativeFontError::font&&!b.slot&&stats(0,0,0));
        CHECK(native_font_acquire(&store,paths[0],0,&b).ok()&&native_font_release(&store,b)&&stats(0,0,0));}
    CHECK(GetProcessHandleCount(GetCurrentProcess(),&after)&&before==after);
    CHECK(native_font_acquire(nullptr,paths[0],0,&b).error==NativeFontError::invalid_input);
    CHECK(native_font_acquire(&store,L"",0,&b).error==NativeFontError::invalid_input);
    CHECK(native_font_acquire(&store,paths[0],0,nullptr).error==NativeFontError::invalid_input);
    CHECK(!native_font_get(store,{0,1})&&!native_font_get(store,{1,0})&&!native_font_get(store,{4,1}));
#if defined(MAICORD_FONT_INSTANCE_TESTS)
    CHECK(instance_cases());
#endif
    CHECK(growth_cases());
    for(auto& f:faces)f.generation=0xffffffffu;
    CHECK(native_font_acquire(&store,paths[0],0,&b).error==NativeFontError::capacity&&!b.slot&&stats(0,0,0));
    return true;
}
} }
extern "C" int maicord_font_store_entry() noexcept {
    using namespace maicord;const bool ok=run();
#if defined(MAICORD_FONT_INSTANCE_TESTS)
    for(u32 i=0;i<instance_store.slots.count;++i)while(instance_store.slots[i].references)native_font_instance_release(&instance_store,{i+1,instance_store.slots[i].generation});
    print("instance_fonts=",15);number(instance_fonts);print("\ninstance_outlines=",19);number(instance_outlines);print("\ninstance_budgets=",18);number(instance_budgets);print("\ninstance_slot_bytes=",21);number(sizeof(NativeFontInstanceSlot));print("\n",1);
#endif
    // Always close live test handles before deleting only files we created.
    for(u32 i=0;i<store.faces.count;++i)while(store.faces[i].references)native_font_release(&store,{i+1,store.faces[i].generation});
    bool cleaned=true;for(auto& path:paths)if(path[0]&&!DeleteFileW(path))cleaned=false;
    if(directory_created&&!RemoveDirectoryW(directory))cleaned=false;
    if(reserved&&!DeleteFileW(reservation))cleaned=false;
    print("checks=",7);number(checks);print("\nhandle_bytes=",14);number(sizeof(NativeFontHandle));print("\nfile_slot_bytes=",17);number(sizeof(NativeFontFileSlot));print("\nface_slot_bytes=",17);number(sizeof(NativeFontFaceSlot));
    const bool passed=ok&&cleaned;
#if defined(MAICORD_FONT_INSTANCE_TESTS)
    const char* message=passed?"\nPASS native font instances\n":"\nFAIL native font instances\n";
#else
    const char* message=passed?"\nPASS native font ownership\n":"\nFAIL native font ownership\n";
#endif
    print(message,28);return passed?0:1;
}
