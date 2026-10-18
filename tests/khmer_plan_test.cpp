#include "maicord/text/khmer_plan.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/win32_kernel.hpp"
namespace maicord {namespace {
#include "generated/khmer_plan_fixture.inc"
u32 checks=0;
void print(const char* s) noexcept {u32 n=0;while(s[n])++n;DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&written,nullptr);}
void number(u64 v) noexcept {char b[32];u32 n=0;do{b[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);while(n){char c[]{b[--n],0};print(c);}}
void store16(u8* p,u16 v) noexcept {p[0]=static_cast<u8>(v>>8);p[1]=static_cast<u8>(v);}
bool fail(u32 line) noexcept {print("FAIL Khmer plan line ");number(line);print("\n");return false;}
#define CHECK(x) do {++checks;if(!(x))return fail(__LINE__);} while(false)
bool untouched(const SfntLookupStep (&steps)[32]) noexcept {
    for(const auto& s:steps)if(s.lookup!=65535||s.feature_mask!=255||s.stage!=255)return false;return true;
}
void poison(SfntLookupStep (&steps)[32]) noexcept {for(auto& s:steps)s={65535,255,255};}
bool units() noexcept {
    const ConstSlice<u8> fixtures[]{{khmer_plan_fixture,sizeof(khmer_plan_fixture)},{khmer_required_fixture,sizeof(khmer_required_fixture)}};
    constexpr SfntLookupStep expected[]{{0,0,0},{4,0,0},{5,0,0},{0,0,1},{1,0,1},{2,1,1},{3,0,1}};
    for(u32 required=0;required<2;++required){
        u8 bytes[1024],scratch[8];copy_bytes(bytes,fixtures[required].data,fixtures[required].count);
        const u32 tag_at=byte_load_be16(bytes+4)+2;
        SfntLayout layout{};SfntLookupPlanResult full{},result{};SfntLookupStep steps[32],saved[32];
        CHECK(sfnt_layout_open({bytes,fixtures[required].count},SfntLayoutKind::substitution,&layout)==SfntStatus::ok);
        CHECK(sfnt_khmer_lookup_plan(layout,{},{scratch,8},{steps,32},&full)==SfntStatus::ok);
        CHECK(full.language_found&&full.script_tag==sfnt_tag('k','h','m','r')&&!full.language_tag&&full.lookup_count==(required?9u:7u));
        if(!required)CHECK(bytes_equal(steps,expected,sizeof(expected)));
        else {u32 at=0;for(u8 stage=0;stage<2;++stage){
            for(const auto& s:expected)if(s.stage==stage){CHECK(bytes_equal(steps+at,&s,sizeof(s)));++at;}
            CHECK(steps[at].lookup==6&&steps[at].stage==stage&&!steps[at].feature_mask);++at;
        }CHECK(at==full.lookup_count);}
        copy_bytes(saved,steps,full.lookup_count*sizeof(SfntLookupStep));
        for(u64 budget=0;budget<=full.operations;++budget){poison(steps);
            const auto status=sfnt_khmer_lookup_plan(layout,{},{scratch,8},{steps,32},&result,budget);
            CHECK(result.operations<=budget&&status==(budget==full.operations?SfntStatus::ok:SfntStatus::resource_limit));
            if(status!=SfntStatus::ok)CHECK(untouched(steps));
        }
        poison(steps);
        CHECK(sfnt_khmer_lookup_plan(layout,{},{scratch,8},{steps,full.lookup_count-1},&result)==SfntStatus::output_too_small&&result.lookup_count==full.lookup_count&&untouched(steps));
        CHECK(sfnt_khmer_lookup_plan(layout,{},{scratch,7},{steps,32},&result)==SfntStatus::output_too_small&&untouched(steps));
        CHECK(sfnt_khmer_lookup_plan(layout,{},{nullptr,8},{steps,32},&result)==SfntStatus::invalid_input&&untouched(steps));
        CHECK(sfnt_khmer_lookup_plan(layout,{},{scratch,8},{nullptr,32},&result)==SfntStatus::invalid_input);
        CHECK(sfnt_khmer_lookup_plan(layout,{},{scratch,8},{steps,32},nullptr)==SfntStatus::invalid_input);
        const u32 feature_at=byte_load_be16(bytes+6);
        if(required){const u32 record=feature_at+2+7*6;
            const u32 body=feature_at+byte_load_be16(bytes+record+4);const u16 original=byte_load_be16(bytes+body+4);
            store16(bytes+body+4,65535);
            CHECK(sfnt_khmer_lookup_plan(layout,{},{scratch,8},{steps,32},&result)==SfntStatus::malformed&&untouched(steps));
            store16(bytes+body+4,original);
        }
        // Selected feature variation replaces pref lookup 5 with lookup 7.
        u8 alternate[]{0,1,0,0,0,1,0,5,0,0,0,12,0,0,0,1,0,7};
        const SfntFeatureSubstitutions variations{{alternate,sizeof(alternate)},1};
        CHECK(sfnt_khmer_lookup_plan(layout,variations,{scratch,8},{steps,32},&result)==SfntStatus::ok);
        CHECK(result.lookup_count==full.lookup_count&&steps[required?3:2].lookup==7&&steps[required?3:2].stage==0);
        poison(steps);alternate[16]=255;alternate[17]=255;
        CHECK(sfnt_khmer_lookup_plan(layout,variations,{scratch,8},{steps,32},&result)==SfntStatus::malformed&&untouched(steps));
        Win32SystemInfo info{};GetSystemInfo(info.native());const u32 page=info.dwPageSize;
        auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*4,MEM_RESERVE,PAGE_NOACCESS));CHECK(memory);
        const bool committed=VirtualAlloc(memory,page,MEM_COMMIT,PAGE_READWRITE)&&VirtualAlloc(memory+page*2,page,MEM_COMMIT,PAGE_READWRITE);
        bool guarded=false;
        if(committed){auto* roots=reinterpret_cast<SfntLookupStep*>(memory+page*3-full.lookup_count*sizeof(SfntLookupStep));
            guarded=sfnt_khmer_lookup_plan(layout,{},{memory+page-layout.lookup_count,layout.lookup_count},{roots,full.lookup_count},&result)==SfntStatus::ok&&
                result.lookup_count==full.lookup_count&&bytes_equal(saved,roots,full.lookup_count*sizeof(SfntLookupStep));}
        CHECK(VirtualFree(memory,0,MEM_RELEASE));CHECK(committed&&guarded);
        const u32 alternatives[]{sfnt_tag('D','F','L','T'),sfnt_tag('k','h','m','2'),sfnt_tag('s','i','n','h')};for(u32 tag:alternatives){
            byte_store_be32(bytes+tag_at,tag);
            CHECK(sfnt_khmer_lookup_plan(layout,{},{scratch,8},{steps,32},&result)==SfntStatus::ok&&!result.language_found&&!result.lookup_count&&untouched(steps));
        }
        byte_store_be32(bytes+tag_at,sfnt_tag('k','h','m','r'));
        // Force a default-language absence; no language must be invented.
        const u32 script_at=byte_load_be16(bytes+4)+byte_load_be16(bytes+tag_at+4);
        const u16 default_offset=byte_load_be16(bytes+script_at);store16(bytes+script_at,0);
        CHECK(sfnt_khmer_lookup_plan(layout,{},{scratch,8},{steps,32},&result)==SfntStatus::ok&&!result.language_found&&!result.lookup_count&&untouched(steps));
        store16(bytes+script_at,default_offset);
        // Duplicate tags choose the first LangSys record. clig must not add 3.
        byte_store_be32(bytes+feature_at+2+3*6,sfnt_tag('c','f','a','r'));
        CHECK(sfnt_khmer_lookup_plan(layout,{},{scratch,8},{steps,32},&result)==SfntStatus::ok);
        bool found=false;for(u32 i=0;i<result.lookup_count;++i)if(steps[i].lookup==3){found=true;CHECK(steps[i].feature_mask==sfnt_khmer_cfar_mask);}CHECK(found);
        // One GPOS group; unsupported tags cannot contribute lookups.
        const u32 tags[]{sfnt_tag('a','b','v','m'),sfnt_tag('b','l','w','m'),sfnt_tag('c','u','r','s'),
            sfnt_tag('d','i','s','t'),sfnt_tag('k','e','r','n'),sfnt_tag('m','a','r','k'),
            sfnt_tag('m','k','m','k'),sfnt_tag('t','e','s','t')};
        for(u32 i=0;i<8;++i)byte_store_be32(bytes+feature_at+2+i*6,tags[i]);
        CHECK(sfnt_layout_open({bytes,fixtures[required].count},SfntLayoutKind::positioning,&layout)==SfntStatus::ok);
        CHECK(sfnt_khmer_lookup_plan(layout,{},{scratch,8},{steps,32},&full)==SfntStatus::ok&&full.lookup_count==4+required);
        constexpr u16 positions[]{0,1,3,4,6};
        for(u32 i=0;i<full.lookup_count;++i)CHECK(steps[i].lookup==positions[i]&&!steps[i].stage&&!steps[i].feature_mask);
        for(u64 budget=0;budget<full.operations;++budget){poison(steps);
            CHECK(sfnt_khmer_lookup_plan(layout,{},{scratch,8},{steps,32},&result,budget)==SfntStatus::resource_limit&&result.operations<=budget&&untouched(steps));}
    }
    print("PASS Khmer schedule, conditional union, required features, variation, isolation and atomic limits; checks=");number(checks);print("\n");return true;
}
bool plans(ConstSlice<u8> bytes) noexcept {
    SfntFace face{};CHECK(sfnt_open(bytes,0,&face)==SfntStatus::ok);
    u8 scratch[4096];SfntLookupStep steps[8192];
    for(u32 kind=0;kind<2;++kind){ConstSlice<u8> table{};SfntLayout layout{};
        const auto status=sfnt_table(face,kind?sfnt_tag('G','P','O','S'):sfnt_tag('G','S','U','B'),&table);
        if(status==SfntStatus::missing_table)continue;CHECK(status==SfntStatus::ok);
        CHECK(sfnt_layout_open(table,kind?SfntLayoutKind::positioning:SfntLayoutKind::substitution,&layout)==SfntStatus::ok);
        for(u32 mode=0;mode<2;++mode){SfntLookupPlanResult result{};
            CHECK(sfnt_khmer_lookup_plan(layout,{},{scratch,4096},{steps,8192},&result)==SfntStatus::ok);
            print("{\"kind\":");number(kind);print(",\"independent\":");number(mode);print(",\"steps\":[");
            for(u32 i=0;i<result.lookup_count;++i){if(i)print(",");print("[");number(steps[i].lookup);print(",");number(steps[i].feature_mask);print(",");number(steps[i].stage);print("]");}
            print("]}\n");
        }
    }return true;
}
int run() noexcept {
    const wchar_t* command=GetCommandLineW();bool quote=false;
    while(*command){if(*command=='"')quote=!quote;else if(!quote&&(*command==' '||*command=='\t'))break;++command;}
    while(*command==' '||*command=='\t')++command;if(!*command)return units()?0:1;
    wchar_t path[32768];u32 n=0;quote=*command=='"';if(quote)++command;
    while(*command&&!(quote?*command=='"':(*command==' '||*command=='\t'))){if(n+1>=32768)return 2;path[n++]=*command++;}
    if(quote){if(*command!='"')return 2;++command;}while(*command==' '||*command=='\t')++command;if(*command||!n)return 2;path[n]=0;
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE)return 2;Win32LargeInteger size{};
    if(!GetFileSizeEx(file,size.native())||size.QuadPart<12||size.QuadPart>0xffffffffll){CloseHandle(file);return 2;}
    HANDLE mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);
    const auto* bytes=mapping?static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0)):nullptr;
    const bool ok=bytes&&plans({bytes,static_cast<u32>(size.QuadPart)});
    if(bytes)UnmapViewOfFile(bytes);if(mapping)CloseHandle(mapping);CloseHandle(file);return ok?0:3;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_khmer_plan_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
