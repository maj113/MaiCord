#include "maicord/text/layout_tables.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {namespace {
u32 expected[65536];u8 seen[65536];
void print(Utf8View text) noexcept {DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text.data,text.bytes,&done,nullptr);}
void number(u64 value) noexcept {char s[32];u32 n=0;do{s[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);for(u32 i=0;i<n/2;++i){char t=s[i];s[i]=s[n-i-1];s[n-i-1]=t;}print({s,n});}
void put(u8* bytes,u32 at,u16 value) noexcept {bytes[at]=static_cast<u8>(value>>8);bytes[at+1]=static_cast<u8>(value);}
bool units() noexcept {
    u8 data[40]{};SfntCoverage coverage{};SfntClassDef classes{};SfntDevice device{};u16 glyph=0;i16 delta=0;u32 index=0;
    put(data,0,1);put(data,2,3);put(data,4,0);put(data,6,3);put(data,8,7);
    if(sfnt_coverage_open({data,10},8,&coverage)!=SfntStatus::ok || coverage.covered_count!=3 || sfnt_coverage_index(coverage,3)!=1 || sfnt_coverage_index(coverage,2)!=sfnt_not_covered ||
       sfnt_coverage_glyph(coverage,2,&glyph)!=SfntStatus::ok || glyph!=7 || sfnt_coverage_glyph(coverage,3,&glyph)!=SfntStatus::glyph_out_of_range || glyph)return false;
    put(data,8,3);if(sfnt_coverage_open({data,10},8,&coverage)!=SfntStatus::malformed || coverage.format)return false;
    put(data,0,2);put(data,2,2);put(data,4,1);put(data,6,3);put(data,8,0);put(data,10,6);put(data,12,7);put(data,14,3);
    if(sfnt_coverage_open({data,16},8,&coverage)!=SfntStatus::ok || coverage.covered_count!=5 || sfnt_coverage_index(coverage,7)!=4 ||
       sfnt_coverage_glyph(coverage,3,&glyph)!=SfntStatus::ok || glyph!=6)return false;
    put(data,14,4);if(sfnt_coverage_open({data,16},8,&coverage)!=SfntStatus::malformed)return false;
    put(data,14,3);put(data,10,3);if(sfnt_coverage_open({data,16},8,&coverage)!=SfntStatus::malformed)return false;
    put(data,2,1);put(data,4,0);put(data,6,65535);put(data,8,0);
    if(sfnt_coverage_open({data,10},65536,&coverage)!=SfntStatus::ok || coverage.covered_count!=65536 || sfnt_coverage_index(coverage,65535)!=65535 ||
       sfnt_coverage_glyph(coverage,65535,&glyph)!=SfntStatus::ok || glyph!=65535 || sfnt_coverage_open({data,10},65535,&coverage)!=SfntStatus::malformed)return false;
    put(data,0,1);put(data,2,5);put(data,4,3);put(data,6,2);put(data,8,0);put(data,10,65535);
    if(sfnt_class_def_open({data,12},8,&classes)!=SfntStatus::ok || classes.max_class!=65535 || sfnt_class_value(classes,4)!=0 || sfnt_class_value(classes,5)!=2 ||
       sfnt_class_value(classes,6)!=0 || sfnt_class_value(classes,7)!=65535 || sfnt_class_value(classes,65535)!=0)return false;
    if(sfnt_class_def_open({data,12},7,&classes)!=SfntStatus::malformed || classes.format)return false;
    put(data,0,2);put(data,2,2);put(data,4,1);put(data,6,3);put(data,8,4);put(data,10,6);put(data,12,7);put(data,14,2);
    if(sfnt_class_def_open({data,16},8,&classes)!=SfntStatus::ok || classes.max_class!=4 || sfnt_class_value(classes,2)!=4 || sfnt_class_value(classes,5)!=0 || sfnt_class_value(classes,7)!=2)return false;
    put(data,10,3);if(sfnt_class_def_open({data,16},8,&classes)!=SfntStatus::malformed)return false;
    put(data,0,10);put(data,2,18);put(data,4,1);put(data,6,0x6c6c);put(data,8,0xbfff);
    const i16 samples[]{1,-2,-1,0,1,-2,-1,0,-2};
    if(sfnt_device_open({data,10},&device)!=SfntStatus::ok)return false;
    for(u32 ppem=0;ppem<65536;++ppem)if(sfnt_device_pixels(device,static_cast<u16>(ppem),&delta)!=SfntStatus::ok || delta!=(ppem>=10 && ppem<=18?samples[ppem-10]:0))return false;
    put(data,0,0);put(data,2,4);put(data,4,2);put(data,6,0x178f);put(data,8,0xa123);
    const i16 four[]{1,7,-8,-1,-6};
    if(sfnt_device_open({data,10},&device)!=SfntStatus::ok)return false;
    for(u32 i=0;i<5;++i)if(sfnt_device_pixels(device,static_cast<u16>(i),&delta)!=SfntStatus::ok || delta!=four[i])return false;
    put(data,0,65533);put(data,2,65535);put(data,4,3);put(data,6,0x7f80);put(data,8,0xffa5);
    const i16 eight[]{127,-128,-1};
    if(sfnt_device_open({data,10},&device)!=SfntStatus::ok)return false;
    for(u32 i=0;i<3;++i)if(sfnt_device_pixels(device,static_cast<u16>(65533+i),&delta)!=SfntStatus::ok || delta!=eight[i])return false;
    put(data,0,65535);put(data,2,2);put(data,4,0x8000);
    if(sfnt_device_open({data,6},&device)!=SfntStatus::ok || !sfnt_device_variation_index(device,&index) || index!=0xffff0002 ||
       sfnt_device_pixels(device,16,&delta)!=SfntStatus::unsupported || delta)return false;
    put(data,4,4);if(sfnt_device_open({data,10},&device)!=SfntStatus::unsupported || device.format)return false;
    put(data,0,1);put(data,2,0);
    if(sfnt_coverage_open({data,4},8,&coverage)!=SfntStatus::ok || coverage.covered_count || sfnt_coverage_index(coverage,0)!=sfnt_not_covered)return false;
    put(data,0,2);
    if(sfnt_class_def_open({data,4},8,&classes)!=SfntStatus::ok || classes.max_class || sfnt_class_value(classes,7))return false;
    return sfnt_coverage_open({},8,&coverage)==SfntStatus::invalid_input && sfnt_class_def_open({data,4},0,&classes)==SfntStatus::invalid_input &&
           sfnt_device_open({},&device)==SfntStatus::invalid_input && sfnt_class_value({},123)==0;
}
bool guards() noexcept {
    Win32SystemInfo info{};GetSystemInfo(info.native());const u32 page=info.dwPageSize;
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!memory)return false;
    DWORD old=0;if(!VirtualProtect(memory+page,page,PAGE_NOACCESS,&old)){VirtualFree(memory,0,MEM_RELEASE);return false;}
    u8 base[32]{};put(base,0,2);put(base,2,2);put(base,4,1);put(base,6,3);put(base,8,0);put(base,10,6);put(base,12,7);put(base,14,3);
    bool ok=true;u32 random=0x07c0a117;
    for(u32 iteration=0;iteration<20000 && ok;++iteration) {
        random^=random<<13;random^=random>>17;random^=random<<5;const u32 size=iteration<16?iteration:random%33;
        u8* bytes=memory+page-size;for(u32 i=0;i<size;++i)bytes[i]=base[i];
        if(iteration>=16 && size){const u32 at=(random>>8)%size;bytes[at]^=static_cast<u8>(random>>16);}
        SfntCoverage coverage{};SfntClassDef classes{};SfntDevice device{};const ConstSlice<u8> view{bytes,size};
        if(sfnt_coverage_open(view,128,&coverage)==SfntStatus::ok) {
            for(u32 glyph=0;glyph<128;++glyph) {const u32 index=sfnt_coverage_index(coverage,static_cast<u16>(glyph));if(index!=sfnt_not_covered){u16 found=0;if(index>=coverage.covered_count || sfnt_coverage_glyph(coverage,index,&found)!=SfntStatus::ok || found!=glyph)ok=false;}}
        }
        if(sfnt_class_def_open(view,128,&classes)==SfntStatus::ok)for(u32 glyph=0;glyph<128;++glyph)if(sfnt_class_value(classes,static_cast<u16>(glyph))>classes.max_class)ok=false;
        if(sfnt_device_open(view,&device)==SfntStatus::ok){i16 delta=0;const auto status=sfnt_device_pixels(device,device.start_or_outer,&delta);if(status!=SfntStatus::ok && status!=SfntStatus::unsupported)ok=false;}
    }
    VirtualFree(memory,0,MEM_RELEASE);return ok;
}
struct Reader {const u8* data;u32 size,at;
    bool word(u32* value) noexcept {if(size-at<4)return false;*value=byte_load_le32(data+at);at+=4;return true;}
    bool take(u32 count,const u8** value) noexcept {if(count>size-at)return false;*value=data+at;at+=count;return true;}
};
bool corpus(const wchar_t* path) noexcept {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;
    Win32LargeInteger length{};if(!GetFileSizeEx(file,length.native()) || length.QuadPart<12 || length.QuadPart>0xffffffffll){CloseHandle(file);return false;}
    HANDLE mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);if(!mapping){CloseHandle(file);return false;}
    auto* bytes=static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));Reader reader{bytes,static_cast<u32>(length.QuadPart),8};u32 total=0,counts[5]{},failures=0;u64 queries=0,reverse=0;
    bool ok=bytes && bytes_equal(bytes,"MCOTCM1",8) && reader.word(&total) && total>0;
    for(u32 record=0;record<total && ok;++record) {
        u32 kind=0,limit=0,size=0,count=0,aux=0,ident=0;const u8* raw=nullptr;
        ok=reader.word(&kind)&&reader.word(&limit)&&reader.word(&size)&&reader.word(&count)&&reader.word(&aux)&&reader.word(&ident)&&kind>=1&&kind<=4&&limit>0&&limit<=65536&&count<=65536&&reader.take(size,&raw);if(!ok)break;
        for(u32 i=0;i<65536;++i){expected[i]=kind==1?sfnt_not_covered:0;seen[i]=0;}
        for(u32 i=0;i<count && ok;++i){u32 key=0,value=0;ok=reader.word(&key)&&reader.word(&value)&&key<65536&&!seen[key]&&(kind>2 || key<limit);if(ok){expected[key]=value;seen[key]=1;if((kind==1 && value!=i) || (kind==2 && value>65535))ok=false;}}
        if(!ok)break;bool match=true;const ConstSlice<u8> data{raw,size};
        if(kind==1 || kind==2) {
            SfntCoverage coverage{};SfntClassDef classes{};const auto status=kind==1?sfnt_coverage_open(data,limit,&coverage):sfnt_class_def_open(data,limit,&classes);
            match=status==SfntStatus::ok;
            if(match && kind==1)match=coverage.covered_count==count && count==aux;
            if(match && kind==2)match=classes.max_class==aux;
            if(match)for(u32 glyph=0;glyph<limit;++glyph){const u32 value=kind==1?sfnt_coverage_index(coverage,static_cast<u16>(glyph)):sfnt_class_value(classes,static_cast<u16>(glyph));if(value!=expected[glyph])match=false;++queries;}
            if(status==SfntStatus::ok && kind==1)for(u32 i=0;i<coverage.covered_count;++i){u16 glyph=0;if(sfnt_coverage_glyph(coverage,i,&glyph)!=SfntStatus::ok || expected[glyph]!=i)match=false;++reverse;}
            if(status==SfntStatus::ok && limit<65536){const u16 glyph=65535;const u32 value=kind==1?sfnt_coverage_index(coverage,glyph):sfnt_class_value(classes,glyph);if(value!=(kind==1?sfnt_not_covered:0))match=false;++queries;}
        } else {
            SfntDevice device{};match=sfnt_device_open(data,&device)==SfntStatus::ok;
            if(match && kind==4){u32 index=0;i16 delta=42;match=!count && sfnt_device_variation_index(device,&index) && index==aux && sfnt_device_pixels(device,16,&delta)==SfntStatus::unsupported && !delta;++queries;}
            else if(match){if(device.format==0x8000 || count!=static_cast<u32>(device.end_or_inner)-device.start_or_outer+1)match=false;
                if(match)for(u32 p=0;p<65536;++p){i16 delta=0;if(sfnt_device_pixels(device,static_cast<u16>(p),&delta)!=SfntStatus::ok || delta!=static_cast<i32>(expected[p]))match=false;++queries;}}
        }
        ++counts[kind];if(!match){++failures;if(failures<=10){print(utf8_literal("layout mismatch record="));number(ident);print(utf8_literal(" kind="));number(kind);print(utf8_literal("\n"));}}
    }
    if(ok)ok=reader.at==reader.size;
    for(u32 kind=1;kind<=4;++kind){print(utf8_literal("kind_"));number(kind);print(utf8_literal("="));number(counts[kind]);print(utf8_literal("\n"));}
    print(utf8_literal("queries="));number(queries);print(utf8_literal("\nreverse_queries="));number(reverse);print(utf8_literal("\nfailures="));number(failures);print(utf8_literal("\ncomplete="));number(ok?1:0);print(utf8_literal("\n"));
    if(bytes)UnmapViewOfFile(bytes);CloseHandle(mapping);CloseHandle(file);return ok && !failures;
}
int run() noexcept {
    if(!units()){print(utf8_literal("FAIL layout units\n"));return 1;}if(!guards()){print(utf8_literal("FAIL layout guards\n"));return 2;}
    const wchar_t* command=GetCommandLineW();bool quote=false;while(*command){if(*command=='"')quote=!quote;else if(!quote && (*command==' ' || *command=='\t'))break;++command;}
    while(*command==' ' || *command=='\t')++command;
    if(*command){wchar_t path[32768];u32 count=0;quote=*command=='"';if(quote)++command;while(*command && !(quote?*command=='"':(*command==' ' || *command=='\t'))){if(count+1>=32768)return 3;path[count++]=*command++;}
        if(quote){if(*command!='"')return 3;++command;}while(*command==' ' || *command=='\t')++command;if(*command || !count)return 3;path[count]=0;if(!corpus(path))return 4;}
    print(utf8_literal("PASS native layout common tables\n"));return 0;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_layout_tables_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
