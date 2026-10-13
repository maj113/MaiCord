#include "maicord/text/font_catalog_coverage.hpp"
#include "maicord/text/font_catalog_builder.hpp"
#include "maicord/core/byte_order.hpp"
#include <Windows.h>
namespace maicord {namespace {
#include "generated/font_family_fixture.inc"
u32 checks=0;bool cleanup_ok=true;
void print(const char* p) noexcept {u32 n=0;while(p[n])++n;DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),p,n,&written,nullptr);}
void number(u32 value) noexcept {char digits[16],out[16];u32 n=0;do{digits[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);for(u32 i=0;i<n;++i)out[i]=digits[n-i-1];out[n]=0;print(out);}
#define CHECK(x) do{++checks;if(!(x)){print("FAIL line=");number(__LINE__);print("\n");return false;}}while(0)
bool same(NativeFontHandle a,NativeFontHandle b) noexcept{return a.slot==b.slot&&a.generation==b.generation;}
bool write(const wchar_t* path,ConstSlice<u8> bytes) noexcept {HANDLE h=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);if(h==INVALID_HANDLE_VALUE)return false;
    DWORD n=0;const bool okay=WriteFile(h,bytes.data,bytes.count,&n,nullptr)&&n==bytes.count;CloseHandle(h);return okay;}
struct Temporary {
    wchar_t paths[2][MAX_PATH]{};
    bool open() noexcept {wchar_t dir[MAX_PATH];const u32 n=GetTempPathW(MAX_PATH,dir);if(!n||n>=MAX_PATH)return false;for(auto& p:paths)if(!GetTempFileNameW(dir,L"mfc",0,p))return false;return true;}
    ~Temporary() noexcept{for(auto& p:paths)if(*p)cleanup_ok=DeleteFileW(p)&&cleanup_ok;}
};
constexpr u32 held_faces=1024,collection_faces=held_faces+1;
struct Owner {
    NativeFontCatalogBuilder catalog{},alias{};NativeFontSession* session=nullptr;NativeFontCoverageCache cache{};
    NativeFontCoverageSlot slots[3]{};NativeFontHandle retained[held_faces]{};NativeFontInstanceHandle instance{};
    ~Owner() noexcept {
        if(session){auto* fonts=native_font_session_fonts(session);for(auto h:retained)if(h.slot)cleanup_ok=native_font_release(fonts,h)&&cleanup_ok;
            if(instance.slot)cleanup_ok=native_font_instance_release(native_font_session_instances(session),instance)&&cleanup_ok;}
        cleanup_ok=native_font_coverage_release(&cache)&&cleanup_ok;
        if(session)cleanup_ok=native_font_session_release(&session)&&cleanup_ok;
        native_font_catalog_builder_release(&alias);native_font_catalog_builder_release(&catalog);
    }
};
u8 collection[65536];
bool run() noexcept {
    CHECK(!GetModuleHandleW(L"dwrite.dll")&&!GetModuleHandleW(L"DWriteCore.dll"));Temporary temp;CHECK(temp.open());Owner owner;
    const ConstSlice<u8> font{font_family_fixture+24,byte_load_le32(font_family_fixture+12)};
    // Distinct TTC face indices share one fixture's table data.
    byte_store_be32(collection,sfnt_tag('t','t','c','f'));byte_store_be32(collection+4,0x10000);byte_store_be32(collection+8,collection_faces);
    const u32 offset=12+collection_faces*4,size=offset+font.count;CHECK(size<=sizeof(collection));
    for(u32 i=0;i<collection_faces;++i)byte_store_be32(collection+12+i*4,offset);
    copy_bytes(collection+offset,font.data,font.count);
    for(u32 j=0;j<byte_load_be16(font.data+4);++j){auto* field=collection+offset+12+j*16+8;byte_store_be32(field,byte_load_be32(field)+offset);}
    CHECK(write(temp.paths[0],{collection,size})&&write(temp.paths[1],font));
    // Make the later restored-write-time test independent of clock granularity.
    HANDLE dated=CreateFileW(temp.paths[1],FILE_READ_ATTRIBUTES|FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);CHECK(dated!=INVALID_HANDLE_VALUE);
    FILE_BASIC_INFO clock{},older{};bool dated_ok=GetFileInformationByHandleEx(dated,FileBasicInfo,&clock,sizeof(clock))!=FALSE;
    older.ChangeTime.QuadPart=clock.ChangeTime.QuadPart-100000000;
    dated_ok=dated_ok&&SetFileInformationByHandle(dated,FileBasicInfo,&older,sizeof(older));CloseHandle(dated);CHECK(dated_ok);
    CHECK(native_font_catalog_builder_initialize(&owner.catalog)&&native_font_catalog_builder_initialize(&owner.alias));u16 names[256];u32 id=0;
    CHECK(native_font_catalog_builder_add(&owner.catalog,temp.paths[0],{names,256},&id).ok());
    CHECK(native_font_catalog_builder_add(&owner.catalog,temp.paths[1],{names,256},&id).ok());
    CHECK(native_font_catalog_builder_add(&owner.alias,temp.paths[0],{names,256},&id).ok());
    const auto& c=owner.catalog.catalog;const auto& alias=owner.alias.catalog;CHECK(c.count.faces==collection_faces+1&&c.count.candidates==collection_faces+1&&alias.count.faces==collection_faces);
    CHECK(c.count.files==2&&bytes_equal(&c.files[1].change_time,&older.ChangeTime,8));
    CHECK(native_font_session_create(&owner.session));auto*& session=owner.session;auto& cache=owner.cache;auto* fonts=native_font_session_fonts(session);
    const auto initial=native_font_session_stats(session);CHECK(initial.references==1&&!initial.live.files);
    CHECK(!native_font_coverage_initialize(nullptr,{owner.slots,3},&cache));CHECK(!native_font_coverage_initialize(session,{},&cache));
    CHECK(!native_font_coverage_initialize(session,{owner.slots,65},&cache));CHECK(!native_font_coverage_initialize(session,{owner.slots,3},nullptr));
    owner.slots[2].stamp=1;CHECK(!native_font_coverage_initialize(session,{owner.slots,3},&cache));owner.slots[2]={};
    CHECK(native_font_coverage_initialize(session,{owner.slots,3},&cache));CHECK(!native_font_coverage_initialize(session,{owner.slots,3},&cache));
    const auto ready=native_font_session_stats(session);CHECK(ready.references==2&&ready.metadata_committed==initial.metadata_committed&&!ready.live.files&&!cache.acquisitions);
    bool covered=true;CHECK(native_font_coverage_probe(&cache,c,0,65,&covered).ok()&&covered&&cache.acquisitions==1);
    const auto first=owner.slots[0].handle;const auto* stable=native_font_get(*fonts,first);CHECK(stable&&native_font_session_stats(session).live.references==1);
    for(u32 cp=0;cp<1024;++cp){CHECK(native_font_coverage_probe(&cache,c,0,cp,&covered).ok()&&covered==(cp==65));}
    CHECK(cache.acquisitions==1&&!cache.evictions&&native_font_get(*fonts,first)==stable);
    const auto references=native_font_session_stats(session).live.references;
    const NativeFontFace* borrowed=nullptr;
    CHECK(native_font_coverage_probe(&cache,c,0,65,&covered,&borrowed).ok()&&covered&&borrowed==stable);
    CHECK(native_font_session_stats(session).live.references==references);
    CHECK(native_font_coverage_probe(&cache,c,0,66,&covered,&borrowed).ok()&&!covered&&borrowed==stable);
    CHECK(native_font_coverage_probe(&cache,c,0xffffffffu,65,&covered,&borrowed).error==NativeFontError::invalid_input&&!covered&&!borrowed);
    borrowed=stable;CHECK(native_font_coverage_probe(&cache,c,0,0xd800,&covered,&borrowed).error==NativeFontError::invalid_input&&!covered&&!borrowed);
    borrowed=stable;CHECK(native_font_coverage_probe(&cache,c,0,65,nullptr,&borrowed).error==NativeFontError::invalid_input&&!borrowed);
    CHECK(native_font_coverage_probe(&cache,c,0xffffffffu,65,&covered).error==NativeFontError::invalid_input&&!covered);
    const u32 invalid[]{0xd800,0xdfff,0x110000,0xffffffffu};for(u32 cp:invalid){covered=true;CHECK(native_font_coverage_probe(&cache,c,0,cp,&covered).error==NativeFontError::invalid_input&&!covered);}
    CHECK(native_font_coverage_probe(nullptr,c,0,65,&covered).error==NativeFontError::invalid_input&&!covered);
    CHECK(native_font_coverage_probe(&cache,c,0,65,nullptr).error==NativeFontError::invalid_input);
    CHECK(native_font_coverage_acquire(&cache,c,0,nullptr).error==NativeFontError::invalid_input);
    owner.retained[0]={1,1};CHECK(native_font_coverage_acquire(&cache,c,0xffffffffu,owner.retained).error==NativeFontError::invalid_input&&!owner.retained[0].slot);
    CHECK(native_font_coverage_acquire(&cache,c,0,owner.retained).ok()&&same(owner.retained[0],first));
    CHECK(native_font_session_instance(session,first,{},false,{},&owner.instance).ok());
    CHECK(native_font_coverage_probe(&cache,alias,0,65,&covered).ok()&&covered&&cache.acquisitions==2&&same(owner.slots[1].handle,first));
    CHECK(native_font_coverage_probe(&cache,c,1,65,&covered).ok()&&covered&&cache.acquisitions==3);
    CHECK(native_font_session_stats(session).live.files==1&&native_font_session_stats(session).live.faces==2);
    // Touch the first entry; the separate catalog entry must be evicted first.
    CHECK(native_font_coverage_probe(&cache,c,0,65,&covered).ok());
    CHECK(native_font_coverage_probe(&cache,c,2,65,&covered).ok()&&cache.evictions==1&&owner.slots[1].catalog==&c&&owner.slots[1].face==2);
    CHECK(native_font_get(*fonts,owner.retained[0])==stable&&native_font_instance_get(*native_font_session_instances(session),owner.instance));
    // Clock normalization preserves relative recency, including the next victim.
    cache.clock=0xffffffffu;CHECK(native_font_coverage_probe(&cache,c,0,65,&covered).ok()&&cache.clock==4);
    CHECK(native_font_coverage_probe(&cache,c,3,65,&covered).ok()&&cache.evictions==2&&owner.slots[2].face==3);
    NativeFontHandle revalidated{};const auto live=native_font_session_stats(session).live.references;
    for(u32 i=0;i<3;++i){const auto acquired=cache.acquisitions;
        CHECK(native_font_coverage_acquire(&cache,c,0,&revalidated,true).ok()&&same(revalidated,first)&&cache.acquisitions==acquired+1);
        CHECK(native_font_release(fonts,revalidated)&&native_font_session_stats(session).live.references==live);
        CHECK(native_font_coverage_probe(&cache,c,0,65,&covered).ok()&&covered&&cache.acquisitions==acquired+1&&cache.evictions==2);
    }
    const auto stamp=c.files[0].change_time;c.files[0].change_time.dwLowDateTime^=1;
    const auto rejected=native_font_coverage_acquire(&cache,c,0,&revalidated,true);c.files[0].change_time=stamp;
    CHECK(rejected.native_error==ERROR_RETRY&&!revalidated.slot&&native_font_get(*fonts,first)==stable);
    CHECK(native_font_session_stats(session).live.references==live-1);
    CHECK(native_font_coverage_remember(&cache,c,1,first).native_error==ERROR_RETRY);
    CHECK(native_font_session_stats(session).live.references==live-1);
    CHECK(native_font_coverage_remember(&cache,c,0,first).ok()&&native_font_session_stats(session).live.references==live);
    const auto remembered=cache.acquisitions;
    CHECK(native_font_coverage_probe(&cache,c,0,65,&covered).ok()&&covered&&cache.acquisitions==remembered);
    CHECK(native_font_coverage_remember(&cache,c,0,{}).error==NativeFontError::invalid_input);
    CHECK(native_font_coverage_reset(&cache)&&!cache.clock&&native_font_session_stats(session).live.faces==1);
    for(auto slot:owner.slots)CHECK(!slot.catalog&&!slot.handle.slot&&!slot.handle.generation&&!slot.face&&!slot.stamp);
    CHECK(native_font_get(*fonts,owner.retained[0])==stable);CHECK(native_font_release(fonts,owner.retained[0]));owner.retained[0]={};
    CHECK(native_font_instance_release(native_font_session_instances(session),owner.instance));owner.instance={};CHECK(!native_font_session_stats(session).live.files);
    // A stale catalog is an error, and failure must not leak a temporary handle.
    HANDLE changed=CreateFileW(temp.paths[1],FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);CHECK(changed!=INVALID_HANDLE_VALUE);
    FILETIME old_time{},new_time{};const bool got=GetFileTime(changed,nullptr,nullptr,&old_time)!=0;new_time=old_time;new_time.dwHighDateTime^=1;
    const bool set=got&&SetFileTime(changed,nullptr,nullptr,&new_time);CloseHandle(changed);CHECK(set);
    const auto stale=native_font_coverage_probe(&cache,c,collection_faces,65,&covered);CHECK(stale.error==NativeFontError::io&&stale.native_error==ERROR_RETRY&&!covered&&!native_font_session_stats(session).live.files);
    changed=CreateFileW(temp.paths[1],FILE_WRITE_ATTRIBUTES,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);CHECK(changed!=INVALID_HANDLE_VALUE);
    const bool restored=SetFileTime(changed,nullptr,nullptr,&old_time)!=0;CloseHandle(changed);CHECK(restored);
    CHECK(native_font_coverage_probe(&cache,c,collection_faces,65,&covered).native_error==ERROR_RETRY&&!covered&&!native_font_session_stats(session).live.files);
    NativeFontCatalogBuilder refreshed{};struct RefreshRelease {NativeFontCatalogBuilder& b;~RefreshRelease() noexcept {native_font_catalog_builder_release(&b);}} refreshed_release{refreshed};CHECK(native_font_catalog_builder_initialize(&refreshed));
    CHECK(native_font_catalog_builder_add(&refreshed,temp.paths[1],{names,256},&id).ok());
    CHECK(native_font_coverage_probe(&cache,refreshed.catalog,0,65,&covered).ok()&&covered);CHECK(native_font_coverage_reset(&cache));native_font_catalog_builder_release(&refreshed);
    CHECK(write(temp.paths[1],{collection,4}));const auto malformed=native_font_coverage_probe(&cache,c,collection_faces,65,&covered);
    CHECK(malformed.error==NativeFontError::font&&!covered&&!native_font_session_stats(session).live.files);
    CHECK(DeleteFileW(temp.paths[1]));const auto missing=native_font_coverage_probe(&cache,c,collection_faces,65,&covered);
    CHECK(missing.error==NativeFontError::io&&missing.native_error==ERROR_FILE_NOT_FOUND&&!covered);CHECK(write(temp.paths[1],font));
    // Every physical face slot is independently owned elsewhere: coverage
    // cannot hide this capacity failure as an ordinary missing glyph.
    for(u32 i=0;i<held_faces;++i)CHECK(native_font_session_face(session,temp.paths[0],i,owner.retained+i).ok());
    const auto full=native_font_session_stats(session);CHECK(full.live.files==1&&full.live.faces==held_faces&&full.live.references==held_faces);
    const auto capacity=native_font_coverage_probe(&cache,c,held_faces,65,&covered);CHECK(capacity.error==NativeFontError::capacity&&!covered&&native_font_session_stats(session).live.references==held_faces);
    CHECK(native_font_coverage_probe(&cache,c,0,65,&covered).ok()&&covered&&native_font_session_stats(session).live.references==held_faces+1);
    CHECK(native_font_coverage_reset(&cache));for(auto& h:owner.retained){CHECK(native_font_release(fonts,h));h={};}
    CHECK(!native_font_session_stats(session).live.files);
    // The cache's session reference may be the last one alive.
    CHECK(native_font_coverage_probe(&cache,c,0,65,&covered).ok()&&covered);CHECK(native_font_session_release(&session)&&!session);
    CHECK(native_font_coverage_probe(&cache,c,1,65,&covered).ok()&&covered);CHECK(native_font_coverage_release(&cache)&&!cache.session&&!cache.slots.data);
    CHECK(native_font_coverage_release(&cache)&&!native_font_coverage_release(nullptr));CHECK(!native_font_coverage_reset(&cache));return true;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_font_catalog_coverage_entry() noexcept {using namespace maicord;
    DWORD before=0,after=0;const bool got=GetProcessHandleCount(GetCurrentProcess(),&before)!=0;bool okay=run();
    okay=GetProcessHandleCount(GetCurrentProcess(),&after)&&got&&before==after&&cleanup_ok&&okay;
    print(okay?"PASS native catalog coverage checks=":"FAIL native catalog coverage checks=");number(checks);print("\n");ExitProcess(okay?0:1);
}
