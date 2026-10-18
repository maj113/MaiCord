#include "maicord/text/use_plan.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/win32_kernel.hpp"
namespace maicord {namespace {
#include "generated/devanagari_plan_fixture.inc"
// One schedule contract suite runs against the Sinhala, Myanmar and Tibetan
// profiles; --myanmar or --tibetan selects the profile (default Sinhala).
bool myanmar=false,tibetan=false;u32 profile_tag=sfnt_tag('s','i','n','h');
SfntStatus lookup_plan(const SfntLayout& layout,const SfntFeatureSubstitutions& variations,bool independent,
    Slice<u8> scratch,Slice<SfntLookupStep> output,SfntLookupPlanResult* result,u64 work_limit=1000000) noexcept {
    return myanmar?sfnt_myanmar_lookup_plan(layout,variations,independent,scratch,output,result,work_limit):
        tibetan?sfnt_tibetan_lookup_plan(layout,variations,independent,scratch,output,result,work_limit):
        sfnt_sinhala_lookup_plan(layout,variations,independent,scratch,output,result,work_limit);
}
u32 checks=0;
void print(const char* s) noexcept {u32 n=0;while(s[n])++n;DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&written,nullptr);}
void number(u64 v) noexcept {char b[32];u32 n=0;do{b[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);while(n){char c[]{b[--n],0};print(c);}}
void store16(u8* p,u16 v) noexcept {p[0]=static_cast<u8>(v>>8);p[1]=static_cast<u8>(v);}
bool fail(u32 line) noexcept {print("FAIL USE plan line ");number(line);print("\n");return false;}
#define CHECK(x) do {++checks;if(!(x))return fail(__LINE__);} while(false)
bool untouched(const SfntLookupStep (&steps)[32]) noexcept {
    for(const auto& s:steps)if(s.lookup!=65535||s.feature_mask!=255||s.stage!=255)return false;return true;
}
void poison(SfntLookupStep (&steps)[32]) noexcept {for(auto& s:steps)s={65535,255,255};}
bool units() noexcept {
    const ConstSlice<u8> fixtures[]{{deva_plan_fixture,sizeof(deva_plan_fixture)},{deva_required_fixture,sizeof(deva_required_fixture)}};
    constexpr SfntLookupStep expected[]{{3,0,0},{5,0,0},{6,0,0},{0,0,1},{2,0,3},{1,0,6},{4,0,6}};
    for(u32 required=0;required<2;++required){
        u8 bytes[1024],scratch[8];copy_bytes(bytes,fixtures[required].data,fixtures[required].count);
        const u32 tag_at=byte_load_be16(bytes+4)+2;byte_store_be32(bytes+tag_at,profile_tag);
        SfntLayout layout{};SfntLookupPlanResult full{},result{};SfntLookupStep steps[32],saved[32];
        CHECK(sfnt_layout_open({bytes,fixtures[required].count},SfntLayoutKind::substitution,&layout)==SfntStatus::ok);
        CHECK(lookup_plan(layout,{},false,{scratch,8},{steps,32},&full)==SfntStatus::ok);
        CHECK(full.language_found&&full.script_tag==profile_tag&&!full.language_tag&&full.lookup_count==(required?13u:7u));
        if(!required)CHECK(bytes_equal(steps,expected,sizeof(expected)));
        else {u32 at=0;for(u8 stage=0;stage<7;++stage){if(stage==4)continue;
            for(const auto& s:expected)if(s.stage==stage){CHECK(bytes_equal(steps+at,&s,sizeof(s)));++at;}
            CHECK(steps[at].lookup==7&&steps[at].stage==stage&&!steps[at].feature_mask);++at;
        }CHECK(at==full.lookup_count);}
        copy_bytes(saved,steps,full.lookup_count*sizeof(SfntLookupStep));
        CHECK(lookup_plan(layout,{},true,{scratch,8},{steps,32},&result)==SfntStatus::ok&&
            result.lookup_count==full.lookup_count&&bytes_equal(saved,steps,full.lookup_count*sizeof(SfntLookupStep)));
        for(u64 budget=0;budget<=full.operations;++budget){poison(steps);
            const auto status=lookup_plan(layout,{},false,{scratch,8},{steps,32},&result,budget);
            CHECK(result.operations<=budget&&status==(budget==full.operations?SfntStatus::ok:SfntStatus::resource_limit));
            if(status!=SfntStatus::ok)CHECK(untouched(steps));
        }
        poison(steps);
        CHECK(lookup_plan(layout,{},false,{scratch,8},{steps,full.lookup_count-1},&result)==SfntStatus::output_too_small&&result.lookup_count==full.lookup_count&&untouched(steps));
        CHECK(lookup_plan(layout,{},false,{scratch,7},{steps,32},&result)==SfntStatus::output_too_small&&untouched(steps));
        CHECK(lookup_plan(layout,{},false,{nullptr,8},{steps,32},&result)==SfntStatus::invalid_input&&untouched(steps));
        CHECK(lookup_plan(layout,{},false,{scratch,8},{nullptr,32},&result)==SfntStatus::invalid_input);
        CHECK(lookup_plan(layout,{},false,{scratch,8},{steps,32},nullptr)==SfntStatus::invalid_input);
        // Each required feature must validate before any caller output is written.
        if(required){const u32 feature_at=byte_load_be16(bytes+6);const u32 record=feature_at+2+8*6;
            const u32 body=feature_at+byte_load_be16(bytes+record+4);const u16 original=byte_load_be16(bytes+body+4);
            store16(bytes+body+4,65535);
            CHECK(lookup_plan(layout,{},false,{scratch,8},{steps,32},&result)==SfntStatus::malformed&&untouched(steps));
            store16(bytes+body+4,original);
        }
        // A selected variation replaces locl's lookup 6 with lookup 7. Both
        // the alternate and its corruption must preserve atomic output.
        u8 alternate[]{0,1,0,0,0,1,0,4,0,0,0,12,0,0,0,1,0,7};
        const SfntFeatureSubstitutions variations{{alternate,sizeof(alternate)},1};
        CHECK(lookup_plan(layout,variations,false,{scratch,8},{steps,32},&result)==SfntStatus::ok);
        CHECK(result.lookup_count==full.lookup_count-(required?1u:0u)&&steps[2].lookup==7&&steps[2].stage==0);
        poison(steps);alternate[16]=255;alternate[17]=255;
        CHECK(lookup_plan(layout,variations,false,{scratch,8},{steps,32},&result)==SfntStatus::malformed&&untouched(steps));
        // Exact-size caller buffers end immediately before inaccessible pages.
        Win32SystemInfo info{};GetSystemInfo(info.native());const u32 page=info.dwPageSize;
        auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*4,MEM_RESERVE,PAGE_NOACCESS));CHECK(memory);
        const bool committed=VirtualAlloc(memory,page,MEM_COMMIT,PAGE_READWRITE)&&VirtualAlloc(memory+page*2,page,MEM_COMMIT,PAGE_READWRITE);
        bool guarded=false;
        if(committed){auto* roots=reinterpret_cast<SfntLookupStep*>(memory+page*3-full.lookup_count*sizeof(SfntLookupStep));
            guarded=lookup_plan(layout,{},false,{memory+page-layout.lookup_count,layout.lookup_count},{roots,full.lookup_count},&result)==SfntStatus::ok&&
                result.lookup_count==full.lookup_count&&bytes_equal(saved,roots,full.lookup_count*sizeof(SfntLookupStep));}
        CHECK(VirtualFree(memory,0,MEM_RELEASE));CHECK(committed&&guarded);
        const u32 alternatives[]{myanmar?sfnt_tag('m','y','m','r'):tibetan?sfnt_tag('l','a','t','n'):sfnt_tag('D','F','L','T'),sfnt_tag('d','e','v','2')};for(u32 tag:alternatives){
            byte_store_be32(bytes+tag_at,tag);
            CHECK(lookup_plan(layout,{},false,{scratch,8},{steps,32},&result)==SfntStatus::ok&&!result.language_found&&!result.lookup_count&&untouched(steps));
        }
        if(myanmar||tibetan){byte_store_be32(bytes+tag_at,sfnt_tag('D','F','L','T'));
            CHECK(lookup_plan(layout,{},false,{scratch,8},{steps,32},&result)==SfntStatus::ok&&result.language_found&&
                result.script_tag==sfnt_tag('D','F','L','T')&&result.lookup_count==full.lookup_count&&bytes_equal(steps,saved,full.lookup_count*sizeof(SfntLookupStep)));}
        byte_store_be32(bytes+tag_at,profile_tag);
        if(myanmar){byte_store_be32(bytes+tag_at,sfnt_tag('m','y','m','r'));
            CHECK(sfnt_myanmar_lookup_plan(layout,{},false,{scratch,8},{steps,32},&result,1000000,false)==SfntStatus::ok&&
                result.lookup_count==full.lookup_count&&bytes_equal(saved,steps,full.lookup_count*sizeof(SfntLookupStep)));
            byte_store_be32(bytes+tag_at,profile_tag);}
        // Two equal feature tags keep the first LangSys-listed record. The
        // later half lookup must not leak into any selected group.
        const u32 feature_at=byte_load_be16(bytes+6);
        byte_store_be32(bytes+feature_at+2+3*6,sfnt_tag('a','b','v','s'));
        CHECK(lookup_plan(layout,{},false,{scratch,8},{steps,32},&result)==SfntStatus::ok);
        for(u32 i=0;i<result.lookup_count;++i)CHECK(steps[i].lookup!=2);
        // GPOS retains required roots while optional kern/curs are suppressed.
        const u32 tags[]{sfnt_tag('a','b','v','m'),sfnt_tag('b','l','w','m'),sfnt_tag('c','u','r','s'),
            sfnt_tag('d','i','s','t'),sfnt_tag('k','e','r','n'),sfnt_tag('m','a','r','k'),
            sfnt_tag('m','k','m','k'),sfnt_tag('t','e','s','t'),sfnt_tag('z','z','z','z')};
        for(u32 i=0;i<9;++i)byte_store_be32(bytes+feature_at+2+i*6,tags[i]);
        CHECK(sfnt_layout_open({bytes,fixtures[required].count},SfntLayoutKind::positioning,&layout)==SfntStatus::ok);
        for(u32 mode=0;mode<2;++mode){
            CHECK(lookup_plan(layout,{},mode!=0,{scratch,8},{steps,32},&full)==SfntStatus::ok);
            CHECK(full.lookup_count==6-mode+required);
            for(u32 i=0;i<full.lookup_count;++i)CHECK(steps[i].lookup==(required&&i==full.lookup_count-1?7u:i+1)&&!steps[i].stage&&!steps[i].feature_mask);
            for(u64 budget=0;budget<full.operations;++budget){poison(steps);
                CHECK(lookup_plan(layout,{},mode!=0,{scratch,8},{steps,32},&result,budget)==SfntStatus::resource_limit&&result.operations<=budget&&untouched(steps));}
        }
        if(myanmar){byte_store_be32(bytes+tag_at,sfnt_tag('m','y','m','r'));poison(steps);
            CHECK(sfnt_myanmar_lookup_plan(layout,{},false,{scratch,8},{steps,32},&result,1000000,false)==SfntStatus::ok&&
                !result.lookup_count&&!result.language_found&&untouched(steps));
            CHECK(sfnt_myanmar_lookup_plan(layout,{},false,{scratch,8},{steps,32},&result,0,false)==SfntStatus::resource_limit&&untouched(steps));
            const u32 fallbacks[]{sfnt_tag('l','a','t','n'),sfnt_tag('D','F','L','T')};
            for(u32 tag:fallbacks){byte_store_be32(bytes+tag_at,tag);
                CHECK(sfnt_myanmar_lookup_plan(layout,{},false,{scratch,8},{steps,32},&result,1000000,false)==SfntStatus::ok&&
                    result.script_tag==tag&&result.language_found&&result.lookup_count==6+required);}}
    }
    print(tibetan?"PASS Tibetan schedule, required features, script isolation and atomic limits; checks=":myanmar?"PASS Myanmar schedule, required features, script isolation and atomic limits; checks=":"PASS USE schedule, required features, script isolation and atomic limits; checks=");number(checks);print("\n");return true;
}
bool plans(ConstSlice<u8> bytes) noexcept {
    SfntFace face{};CHECK(sfnt_open(bytes,0,&face)==SfntStatus::ok);
    u8 scratch[4096];SfntLookupStep steps[8192];
    for(u32 kind=0;kind<2;++kind){ConstSlice<u8> table{};SfntLayout layout{};
        const auto status=sfnt_table(face,kind?sfnt_tag('G','P','O','S'):sfnt_tag('G','S','U','B'),&table);
        if(status==SfntStatus::missing_table)continue;CHECK(status==SfntStatus::ok);
        CHECK(sfnt_layout_open(table,kind?SfntLayoutKind::positioning:SfntLayoutKind::substitution,&layout)==SfntStatus::ok);
        for(u32 mode=0;mode<2;++mode){SfntLookupPlanResult result{};
            CHECK(lookup_plan(layout,{},mode!=0,{scratch,4096},{steps,8192},&result)==SfntStatus::ok&&(myanmar||tibetan||result.language_found));
            print("{\"kind\":");number(kind);print(",\"independent\":");number(mode);print(",\"steps\":[");
            for(u32 i=0;i<result.lookup_count;++i){if(i)print(",");print("[");number(steps[i].lookup);print(",");number(steps[i].feature_mask);print(",");number(steps[i].stage);print("]");}
            print("]}\n");
        }
    }return true;
}
bool option(const wchar_t*& command,const wchar_t* flag) noexcept {
    u32 i=0;while(flag[i]&&command[i]==flag[i])++i;
    if(flag[i]||(command[i]&&command[i]!=' '&&command[i]!='\t'))return false;
    command+=i;while(*command==' '||*command=='\t')++command;return true;
}
int run() noexcept {
    const wchar_t* command=GetCommandLineW();bool quote=false;
    while(*command){if(*command=='"')quote=!quote;else if(!quote&&(*command==' '||*command=='\t'))break;++command;}
    while(*command==' '||*command=='\t')++command;
    if(option(command,L"--myanmar")){myanmar=true;profile_tag=sfnt_tag('m','y','m','2');}
    else if(option(command,L"--tibetan")){tibetan=true;profile_tag=sfnt_tag('t','i','b','t');}
    if(!*command)return units()?0:1;
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
extern "C" __declspec(noreturn) void WINAPI maicord_use_plan_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
