// Native font catalog (MaiCordFontCatalogTests). No argument runs the suite. Manual modes:
// --handle-stat-check (the default run already includes it) and
// --handle-stat-fallback-check, which asserts the fallback after a debugger (CDB) forces the
// handle query to fail and therefore fails without that injection.
#include "maicord/text/font_session.hpp"
#include "maicord/text/font_catalog.hpp"
#include "../src/text/font_path_stat.hpp"
#include <Windows.h>
namespace maicord {namespace {
#include "generated/font_family_fixture.inc"
#include "font_catalog_fixture.inc"
u32 checks=0;
void print(const char* s) noexcept {u32 n=0;while(s[n])++n;DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&done,nullptr);}
void number(u32 v) noexcept {char d[16],out[16];u32 n=0;do{d[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n;++i)out[i]=d[n-i-1];out[n]=0;print(out);}
#define CHECK(x) do{++checks;if(!(x)){print("FAIL line=");number(__LINE__);print("\n");return false;}}while(0)
u32 length(const wchar_t* p) noexcept {u32 n=0;while(p[n])++n;return n;}
ConstSlice<u16> wide(const wchar_t* p) noexcept {return {reinterpret_cast<const u16*>(p),length(p)};}
bool equal(ConstSlice<u16> a,ConstSlice<u16> b) noexcept {return a.count==b.count&&bytes_equal(a.data,b.data,a.count*2);}
struct Temporary {
    wchar_t reservation[MAX_PATH]{},directory[MAX_PATH]{},paths[4][MAX_PATH]{};bool reserved=false,created=false;
    bool open() noexcept {wchar_t temp[MAX_PATH];const u32 n=GetTempPathW(MAX_PATH,temp);if(!n||n>=MAX_PATH)return false;
        reserved=GetTempFileNameW(temp,L"mcc",0,reservation)!=0;if(!reserved)return false;const u32 size=length(reservation);if(size+5>=MAX_PATH)return false;
        copy_bytes(directory,reservation,size*2);copy_bytes(directory+size,L".dir",10);created=CreateDirectoryW(directory,nullptr)!=0;if(!created)return false;
        for(auto& path:paths)if(!GetTempFileNameW(directory,L"c",0,path))return false;return true;}
    ~Temporary() noexcept {for(auto& p:paths)if(*p)DeleteFileW(p);if(created)RemoveDirectoryW(directory);if(reserved)DeleteFileW(reservation);}
};
bool write(const wchar_t* path,ConstSlice<u8> bytes) noexcept {HANDLE h=CreateFileW(path,GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,0,nullptr);if(h==INVALID_HANDLE_VALUE)return false;DWORD n=0;const bool ok=WriteFile(h,bytes.data,bytes.count,&n,nullptr)&&n==bytes.count;CloseHandle(h);return ok;}
bool views() noexcept {
    const ConstSlice<u8> font{font_family_fixture+24,byte_load_le32(font_family_fixture+12)};CHECK(font.count+24<sizeof(font_family_fixture));
    Temporary temp;CHECK(temp.open());CHECK(write(temp.paths[0],font));CatalogFixture fixture;u32 index=0;CHECK(fixture.add(temp.paths[0],&index));const auto& c=fixture.catalog;
    CHECK(equal(native_font_catalog_path(c,0),wide(temp.paths[0]))&&equal(native_font_catalog_name(c,0),wide(L"Probe"))&&c.candidates[0].properties.weight==700);
    CHECK(!native_font_catalog_settings(c,0).count&&!native_font_catalog_path(c,1).count&&!native_font_catalog_name(c,1).count&&!native_font_catalog_settings(c,1).count);
    auto broken=c;broken.files.count=0;CHECK(!native_font_catalog_path(broken,0).count&&!native_font_catalog_name(broken,0).count);
    return true;
}
bool entries() noexcept {
    const ConstSlice<u8> font{font_family_fixture+24,byte_load_le32(font_family_fixture+12)};Temporary temp;CHECK(temp.open());
    u8 collection[16384]{};byte_store_be32(collection,sfnt_tag('t','t','c','f'));byte_store_be32(collection+4,0x10000);byte_store_be32(collection+8,10);u32 size=52;
    for(u32 i=0;i<10;++i){const u32 offset=size;CHECK(offset+font.count+4<sizeof(collection));byte_store_be32(collection+12+i*4,offset);copy_bytes(collection+offset,font.data,font.count);
        for(u32 j=0;j<byte_load_be16(font.data+4);++j){auto* field=collection+offset+12+j*16+8;byte_store_be32(field,byte_load_be32(field)+offset);}size=(offset+font.count+3)&~3u;}
    CHECK(write(temp.paths[0],{collection,size}));CatalogFixture fixture;u32 index=0;CHECK(fixture.add(temp.paths[0],&index)&&fixture.catalog.count.candidates==10);auto& c=fixture.catalog;
    NativeFontSession* session=nullptr;NativeFontHandle face{};
    struct Release {NativeFontSession*& session;NativeFontHandle& face;~Release() noexcept {if(session){if(face.slot)native_font_release(native_font_session_fonts(session),face);native_font_session_release(&session);}}} release{session,face};
    CHECK(native_font_session_create(&session)&&native_font_session_face(session,temp.paths[0],0,&face).ok());auto* fonts=native_font_session_fonts(session);
    NativeFontCatalogEntry entry{};CHECK(native_font_catalog_entry(c,1,&entry).ok()&&entry.face_index==1&&entry.path.count&&entry.file&&!entry.settings.count);
    CHECK(native_font_catalog_validate(entry,*fonts,face).native_error==ERROR_RETRY);
    CHECK(native_font_catalog_validate({},*fonts,face).error==NativeFontError::invalid_input);
    CHECK(native_font_catalog_entry(c,0,&entry).ok()&&native_font_catalog_validate(entry,*fonts,face).ok());
    // Every part of the catalog version is required, even for an already live face.
    for(u32 field=0;field<5;++field){auto wrong_file=*entry.file;auto wrong=entry;wrong.file=&wrong_file;
        if(field==0)++wrong_file.bytes;else if(field==1)wrong_file.identity.VolumeSerialNumber^=1;
        else if(field==2)wrong_file.identity.FileId.Identifier[0]^=1;
        else if(field==3)wrong_file.last_write.dwLowDateTime^=1;else wrong_file.change_time.dwLowDateTime^=1;
        CHECK(native_font_catalog_validate(wrong,*fonts,face).native_error==ERROR_RETRY);}
    const u32 saved=c.candidates[0].first_setting;c.candidates[0].first_setting=1;
    CHECK(native_font_catalog_entry(c,0,&entry).error==NativeFontError::invalid_input&&!entry.file);
    c.candidates[0].first_setting=saved;CHECK(native_font_catalog_entry(c,0,nullptr).error==NativeFontError::invalid_input);
    CHECK(native_font_catalog_entry(c,10,&entry).error==NativeFontError::invalid_input);
    CHECK(native_font_release(fonts,face));face={};return true;
}
bool argument(const wchar_t** command,wchar_t* out,u32 available) noexcept {const wchar_t* p=*command;while(*p==L' '||*p==L'\t')++p;const bool quoted=*p==L'"';if(quoted)++p;u32 n=0;
    while(*p&&!(quoted?*p==L'"':*p==L' '||*p==L'\t')){if(n+1>=available)return false;out[n++]=*p++;}if(quoted){if(*p!=L'"')return false;++p;}out[n]=0;*command=p;return n!=0;}
#include "font_handle_stat_units.inc"
bool run() noexcept {
    static wchar_t arg[32768];const wchar_t* command=GetCommandLineW();CHECK(argument(&command,arg,32768));while(*command==L' '||*command==L'\t')++command;
    if(!*command)return views()&&entries()&&handle_stat_tests::run();
    CHECK(argument(&command,arg,32768));
    if(equal(wide(arg),wide(L"--handle-stat-check")))return handle_stat_tests::run();
    CHECK(equal(wide(arg),wide(L"--handle-stat-fallback-check")));return handle_stat_tests::run(true);
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_font_catalog_entry() noexcept {
    using namespace maicord;const bool ok=run();print("checks=");number(checks);print(ok?"\nPASS native font catalog\n":"\nFAIL native font catalog\n");ExitProcess(ok?0:1);
}
