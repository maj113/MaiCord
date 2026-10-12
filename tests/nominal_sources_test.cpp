#include "maicord/text/nominal_sources.hpp"
#include "maicord/core/byte_order.hpp"
namespace maicord {namespace {
#include "generated/nominal_scalar_fixture.inc"
void print(Utf8View s) noexcept {DWORD n=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s.data,s.bytes,&n,nullptr);}
void number(u64 v) noexcept {char s[32];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n/2;++i){const char c=s[i];s[i]=s[n-i-1];s[n-i-1]=c;}print({s,n});}
u64 scalar_checks=0,map_checks=0,budget_checks=0,guard_checks=0;volatile u64 observation=0;
void p16(u8* p,u32 at,u16 v) noexcept {p[at]=static_cast<u8>(v>>8);p[at+1]=static_cast<u8>(v);}
u8 file[2048];
u8* fixture(u16 format,u32 size,u16 glyphs) noexcept {
    clear_bytes(file,sizeof(file));byte_store_be32(file,0x10000);p16(file,4,2);
    byte_store_be32(file+12,sfnt_tag('c','m','a','p'));byte_store_be32(file+20,64);byte_store_be32(file+24,12+size);
    byte_store_be32(file+28,sfnt_tag('m','a','x','p'));byte_store_be32(file+36,48);byte_store_be32(file+40,6);byte_store_be32(file+48,0x5000);p16(file,52,glyphs);
    p16(file,66,1);p16(file,68,format==13?0:3);p16(file,70,format==13?6:format>=10?10:1);byte_store_be32(file+72,12);
    u8* p=file+76;p16(p,0,format);if(format>=10)byte_store_be32(p+4,size);else p16(p,2,static_cast<u16>(size));return p;
}
bool observed(u32 cp) noexcept {return cp<65536 && ((nominal_scalar_fixture[cp/8]>>(cp%8))&1)!=0;}
bool verify(ConstSlice<u8> bytes={file,sizeof(file)},bool exhaustive=true,bool timed=true) noexcept {
    SfntFace face{};SfntCmap cmap{};if(sfnt_open(bytes,0,&face)!=SfntStatus::ok || sfnt_cmap_open(face,&cmap)!=SfntStatus::ok)return false;
    u64 expected[1024]{},actual[1024]{},work=0;const u32 words=(cmap.glyph_count+63u)/64;
    // Independent slow inverse via the existing scalar cmap query, restricted
    // by uncompressed Windows observations, validates the new range traversal.
    for(u32 cp=0;cp<65536;++cp){if(!observed(cp))continue;u16 glyph=0;if(sfnt_cmap_glyph(cmap,cp,0,&glyph)!=SfntStatus::ok)return false;if(glyph)expected[glyph/64]|=1ull<<(glyph%64);}
    if(sfnt_nominal_sources(cmap,{actual,1024},&work)!=SfntStatus::ok || !bytes_equal(expected,actual,words*8) || (exhaustive&&work>10000))return false;map_checks+=cmap.glyph_count;
    if(!exhaustive&&timed){LARGE_INTEGER begin{},end{},frequency{};QueryPerformanceFrequency(&frequency);QueryPerformanceCounter(&begin);
        for(u32 i=0;i<1024;++i){if(sfnt_nominal_sources(cmap,{actual,1024},&work)!=SfntStatus::ok)return false;observation=observation^actual[i%words];}
        QueryPerformanceCounter(&end);print(utf8_literal("source_ticks="));number(end.QuadPart-begin.QuadPart);print(utf8_literal("\nsource_frequency="));number(frequency.QuadPart);print(utf8_literal("\nsource_repeats=1024\nsource_work="));number(work);print(utf8_literal("\n"));}
    const u64 full=work;for(u64 i=0;i<(exhaustive?full+1:3);++i){const u64 budget=exhaustive?i:i==0?0:i==1?full-1:full;const auto status=sfnt_nominal_sources(cmap,{actual,1024},&work,budget);++budget_checks;if(status!=(budget<full?SfntStatus::resource_limit:SfntStatus::ok) || work>budget || (status==SfntStatus::ok && !bytes_equal(actual,expected,words*8)))return false;}
    actual[0]=0xaaaaaaaaaaaaaaaaull;if(sfnt_nominal_sources(cmap,{actual,words-1},&work)!=SfntStatus::output_too_small || actual[0]!=0xaaaaaaaaaaaaaaaaull)return false;
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize,storage=((words*8+page-1)/page)*page;auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,storage+page,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!memory)return false;DWORD old=0;
    bool ok=VirtualProtect(memory+storage,page,PAGE_NOACCESS,&old)!=0;auto* out=reinterpret_cast<u64*>(memory+storage-words*8);if(ok)ok=sfnt_nominal_sources(cmap,{out,words},&work)==SfntStatus::ok && bytes_equal(out,expected,words*8);++guard_checks;VirtualFree(memory,0,MEM_RELEASE);return ok;
}
bool units() noexcept {
    for(u32 cp=0;cp<=0x110100;++cp){if(nominal_scalar_is_simple(cp)!=observed(cp))return false;++scalar_checks;}if(nominal_scalar_is_simple(0xffffffff))return false;
    u8* p=fixture(0,262,256);for(u32 cp=0;cp<256;++cp)p[6+cp]=static_cast<u8>(cp);if(!verify())return false;
    for(u16 format=6;format<=10;format+=4){const u32 header=format==6?10:20;p=fixture(format,header+260,256);if(format==6){p16(p,6,0x280);p16(p,8,130);}else{byte_store_be32(p+12,0x280);byte_store_be32(p+16,130);}for(u32 i=0;i<130;++i)p16(p,header+i*2,static_cast<u16>(i+1));if(!verify())return false;}
    p=fixture(10,24,256);byte_store_be32(p+12,0x10000);byte_store_be32(p+16,2);p16(p,20,1);p16(p,22,2);if(!verify())return false;
    p=fixture(4,46,256);p16(p,6,6);p16(p,14,'C');p16(p,16,'c');p16(p,18,65535);p16(p,22,'A');p16(p,24,'a');p16(p,26,65535);p16(p,28,static_cast<u16>(3-'A'));p16(p,30,2);p16(p,32,1);p16(p,36,4);p16(p,40,10);p16(p,42,0);p16(p,44,20);if(!verify())return false;
    // Delta wraparound must preserve glyph zero and reach the last valid glyph.
    p=fixture(4,32,65535);p16(p,6,4);p16(p,14,'C');p16(p,16,65535);p16(p,20,'A');p16(p,22,65535);p16(p,24,static_cast<u16>(65534-'A'));p16(p,26,1);
    // Skip the intermediate out-of-range glyph 65535 by using only A.
    p16(p,14,'A');if(!verify())return false;
    for(u16 format=12;format<=13;++format){p=fixture(format,64,65535);byte_store_be32(p+12,4);const u32 groups[][3]{{0x280,0x381,63},{0xfffc,0xffff,1},{0x10000,0x10001,4},{0x1f600,0x1f601,6}};for(u32 i=0;i<4;++i)for(u32 j=0;j<3;++j)byte_store_be32(p+16+i*12+j*4,groups[i][j]);if(!verify())return false;}
    for(u16 format=12;format<=13;++format){const u32 scalars[]{0,31,32,63,64,65,127,128,0x2ff,0x300,0x36f,0x370,0xd7ff,0xe000,0xfffc,0xffff,0x10000};
        p=fixture(format,16+sizeof(scalars)*3,256);byte_store_be32(p+12,sizeof(scalars)/sizeof(scalars[0]));
        for(u32 i=0;i<sizeof(scalars)/sizeof(scalars[0]);++i){byte_store_be32(p+16+i*12,scalars[i]);byte_store_be32(p+20+i*12,scalars[i]);byte_store_be32(p+24+i*12,i%4?i*7:0);}
        if(!verify())return false;}
    p=fixture(13,28,256);byte_store_be32(p+12,1);byte_store_be32(p+16,0x300);byte_store_be32(p+20,0x36f);byte_store_be32(p+24,2);if(!verify())return false;
    p=fixture(13,28,256);byte_store_be32(p+12,1);byte_store_be32(p+16,0);byte_store_be32(p+20,0xffff);byte_store_be32(p+24,0);if(!verify())return false;
    for(u32 alignment=0;alignment<64;++alignment)for(u32 scenario=0;scenario<4;++scenario)for(u16 format=4;format<=12;format+=8){
        const u32 first=0x280+((scenario&1)?alignment:0),last=first+(scenario==2?63:191),base=scenario==2?65440:scenario==3?0:alignment;
        const u16 glyphs=scenario==2?65535:512;
        if(format==4){p=fixture(4,32,glyphs);p16(p,6,4);p16(p,14,static_cast<u16>(last));p16(p,16,65535);p16(p,20,static_cast<u16>(first));p16(p,22,65535);p16(p,24,static_cast<u16>(base-first));p16(p,26,1);}
        else{p=fixture(12,28,glyphs);byte_store_be32(p+12,1);byte_store_be32(p+16,first);byte_store_be32(p+20,last);byte_store_be32(p+24,base);}
        if(!verify({file,sizeof(file)},false,false))return false;
    }
    {
        p=fixture(4,32,256);p16(p,6,4);p16(p,14,0x3bf);p16(p,16,65535);p16(p,20,0x280);p16(p,22,65535);p16(p,24,static_cast<u16>(128-0x280));p16(p,26,1);
        SfntFace face{};SfntCmap cmap{};u64 bits[5]{},used=0;bits[4]=0x123456789abcdef0ull;
        if(sfnt_open({file,sizeof(file)},0,&face)!=SfntStatus::ok||sfnt_cmap_open(face,&cmap)!=SfntStatus::ok||
           sfnt_nominal_sources(cmap,{bits,4},&used)!=SfntStatus::glyph_out_of_range||bits[4]!=0x123456789abcdef0ull)return false;
        const u64 full=used;
        for(u64 budget=0;budget<=full;++budget){const auto status=sfnt_nominal_sources(cmap,{bits,4},&used,budget);++budget_checks;
            if(status!=(budget<full?SfntStatus::resource_limit:SfntStatus::glyph_out_of_range)||used>budget||bits[4]!=0x123456789abcdef0ull)return false;}
    }
    u64 word=0,used=1;if(sfnt_nominal_sources({}, {&word,1},&used)!=SfntStatus::invalid_input || used)return false;SfntFace face{};SfntCmap cmap{};if(sfnt_open({file,sizeof(file)},0,&face)!=SfntStatus::ok || sfnt_cmap_open(face,&cmap)!=SfntStatus::ok || sfnt_nominal_sources(cmap,{nullptr,1})!=SfntStatus::invalid_input)return false;
    return true;
}
bool file_check(const wchar_t* path) noexcept {
    HANDLE handle=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(handle==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER size{};if(!GetFileSizeEx(handle,&size)||size.QuadPart<12||size.QuadPart>0xffffffffll){CloseHandle(handle);return false;}
    HANDLE mapping=CreateFileMappingW(handle,nullptr,PAGE_READONLY,0,0,nullptr);if(!mapping){CloseHandle(handle);return false;}
    auto* bytes=static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));const bool ok=bytes&&verify({bytes,static_cast<u32>(size.QuadPart)},false);
    if(bytes)UnmapViewOfFile(bytes);CloseHandle(mapping);CloseHandle(handle);return ok;
}
int run() noexcept {
    const wchar_t* command=GetCommandLineW();bool quoted=false;while(*command){if(*command=='"')quoted=!quoted;else if(!quoted&&(*command==' '||*command=='\t'))break;++command;}while(*command==' '||*command=='\t')++command;
    bool ok=false;if(!*command)ok=units();
    else {wchar_t path[32768];u32 n=0;quoted=*command=='"';if(quoted)++command;
        while(*command&&!(quoted?*command=='"':(*command==' '||*command=='\t'))){if(n+1>=32768)return 2;path[n++]=*command++;}
        if(quoted){if(*command!='"')return 2;++command;}while(*command==' '||*command=='\t')++command;if(*command||!n)return 2;path[n]=0;ok=file_check(path);}
print(utf8_literal("scalar_checks="));number(scalar_checks);print(utf8_literal("\nmap_checks="));number(map_checks);print(utf8_literal("\nbudget_checks="));number(budget_checks);print(utf8_literal("\nguards="));number(guard_checks);print(ok?utf8_literal("\nPASS native nominal sources\n"):utf8_literal("\nFAIL native nominal sources\n"));return ok?0:1;}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_nominal_sources_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
