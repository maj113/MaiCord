#include "maicord/text/nominal_participation.hpp"
#include "maicord/text/nominal_sources.hpp"
#include "maicord/core/byte_order.hpp"
#include <Windows.h>
namespace maicord {namespace {
#include "generated/nominal_participation_fixture.inc"
void print(Utf8View s) noexcept {DWORD n=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s.data,s.bytes,&n,nullptr);}
void number(u64 v) noexcept {char s[32];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n/2;++i){const char c=s[i];s[i]=s[n-i-1];s[n-i-1]=c;}print({s,n});}
u64 font_checks=0,glyph_checks=0,budget_checks=0,guards=0,lazy_built=0,lazy_skipped=0;
void put(u8* p,u32 at,u16 v) noexcept {p[at]=static_cast<u8>(v>>8);p[at+1]=static_cast<u8>(v);}
bool boundaries() noexcept {
    // Range coverage crossing words and reaching glyph 65535. Includes glyph
    // zero as table data; missing-glyph fallback remains the text caller's policy.
    u8 raw[72]{};put(raw,0,1);put(raw,4,10);put(raw,6,12);put(raw,8,26);
    put(raw,12,1);byte_store_be32(raw+14,sfnt_tag('c','c','m','p'));put(raw,18,8);put(raw,22,1);
    put(raw,26,1);put(raw,28,4);put(raw,30,1);put(raw,34,1);put(raw,36,8);put(raw,38,1);put(raw,40,6);put(raw,44,2);put(raw,46,4);
    const u16 ranges[][3]{{0,0,0},{63,130,1},{511,512,69},{65534,65535,71}};
    for(u32 i=0;i<4;++i)for(u32 j=0;j<3;++j)put(raw,48+i*6+j*2,ranges[i][j]);
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;if(page!=4096)return false;
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*6,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!memory)return false;DWORD old=0;
    bool ok=VirtualProtect(memory+page*2,page,PAGE_NOACCESS,&old)!=0 && VirtualProtect(memory+page*4,page,PAGE_NOACCESS,&old)!=0;
    auto* bitmap=reinterpret_cast<u64*>(memory);auto* scratch=reinterpret_cast<u64*>(memory+page*4-16);SfntLayout layout{};SfntNominalParticipationResult result{};
    ok=ok && sfnt_layout_open({raw,sizeof(raw)},SfntLayoutKind::substitution,&layout)==SfntStatus::ok && sfnt_nominal_participation_add(layout,65536,{bitmap,1024},{scratch,2},&result)==SfntStatus::ok;++guards;
    if(ok)for(u32 glyph=0;glyph<65536;++glyph){const bool expected=glyph==0 || (glyph>=63 && glyph<=130) || (glyph>=511 && glyph<=512) || glyph>=65534;if(((bitmap[glyph/64]>>(glyph%64))&1)!=(expected?1u:0u)){ok=false;break;}++glyph_checks;}
    const u64 full=result.operations;
    for(u64 budget=0;budget<=full && ok;++budget){for(u32 i=0;i<1024;++i)bitmap[i]=0;const auto s=sfnt_nominal_participation_add(layout,65536,{bitmap,1024},{scratch,2},&result,budget);ok=s==(budget<full?SfntStatus::resource_limit:SfntStatus::ok) && result.operations<=budget;++budget_checks;}
    ok=ok && sfnt_nominal_participation_add(layout,65535,{bitmap,1024},{scratch,2},&result)==SfntStatus::malformed;
    ok=ok && sfnt_nominal_participation_add(layout,65536,{bitmap,1023},{scratch,2},&result)==SfntStatus::output_too_small;
    ok=ok && sfnt_nominal_participation_add(layout,0,{bitmap,1024},{scratch,2},&result)==SfntStatus::invalid_input && sfnt_nominal_participation_add(layout,65537,{bitmap,1024},{scratch,2},&result)==SfntStatus::invalid_input;
    ok=ok && sfnt_nominal_participation_add({},10,{bitmap,1024},{scratch,2},&result)==SfntStatus::invalid_input && sfnt_nominal_participation_add(layout,10,{nullptr,1},{scratch,2},&result)==SfntStatus::invalid_input && sfnt_nominal_participation_add(layout,10,{bitmap,1024},{nullptr,1},&result)==SfntStatus::invalid_input && sfnt_nominal_participation_add(layout,10,{bitmap,1024},{scratch,2},nullptr)==SfntStatus::invalid_input;
    const u64 no_sources=0;ok=ok && sfnt_nominal_participation_add(layout,65536,{bitmap,1024},{scratch,2},&result,1000000,{&no_sources,1})==SfntStatus::output_too_small && sfnt_nominal_participation_add(layout,65536,{bitmap,1024},{scratch,2},&result,1000000,{nullptr,1024})==SfntStatus::invalid_input;
    // A version-1.1 table retains base participation. The nominal gate does not
    // read feature replacements; the full-shaping plan selects them separately.
    u8 variable[80]{};copy_bytes(variable,raw,10);put(variable,2,1);for(u32 field=4;field<10;field+=2)put(variable,field,static_cast<u16>(byte_load_be16(raw+field)+4));byte_store_be32(variable+10,76);copy_bytes(variable+14,raw+10,62);put(variable,76,1);
    for(u32 i=0;i<1024;++i)bitmap[i]=0;
    ok=ok && sfnt_layout_open({variable,sizeof(variable)},SfntLayoutKind::substitution,&layout)==SfntStatus::ok && sfnt_nominal_participation_add(layout,65536,{bitmap,1024},{scratch,2},&result)==SfntStatus::ok && bitmap[0]==0x8000000000000001ull && bitmap[1023]==0xc000000000000000ull;
    // Structural mutation/truncation probes end immediately before NOACCESS.
    u32 random=0x94375b21;for(u32 i=0;i<3000 && ok;++i){random^=random<<13;random^=random>>17;random^=random<<5;const u32 size=i<=sizeof(raw)?i:random%(sizeof(raw)+1);auto* bytes=memory+page*2-size;copy_bytes(bytes,raw,size);if(i>sizeof(raw) && size)bytes[(random>>8)%size]^=static_cast<u8>(random>>16);
        if(sfnt_layout_open({bytes,size},SfntLayoutKind::substitution,&layout)==SfntStatus::ok){u64 output[1024]{},temp[2048]{};sfnt_nominal_participation_add(layout,65536,{output,1024},{temp,2048},&result,10000);if(result.operations>10000)ok=false;}++guards;
    }
    VirtualFree(memory,0,MEM_RELEASE);return ok;
}
bool corpus() noexcept {
    ConstSlice<u8> all{participation_fixture,sizeof(participation_fixture)};if(all.count<12 || !bytes_equal(all.data,"MCNOM01\0",8))return false;const u32 count=byte_load_le32(all.data+8);u32 at=12;
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*6,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!memory)return false;DWORD old=0;
    bool ok=VirtualProtect(memory+page,page,PAGE_NOACCESS,&old)!=0 && VirtualProtect(memory+page*3,page,PAGE_NOACCESS,&old)!=0 && VirtualProtect(memory+page*5,page,PAGE_NOACCESS,&old)!=0;
    for(u32 i=0;i<count && ok;++i){if(all.count-at<20){ok=false;break;}const u32 size=byte_load_le32(all.data+at),kind=byte_load_le32(all.data+at+4),limit=byte_load_le32(all.data+at+8);u64 expected=0;copy_bytes(&expected,all.data+at+12,8);at+=20;if(size>all.count-at || kind>1 || !limit || limit>64){ok=false;break;}
        SfntFace face{};SfntLayout layout{};ConstSlice<u8> table{};ok=sfnt_open({all.data+at,size},0,&face)==SfntStatus::ok && sfnt_table(face,kind?sfnt_tag('G','P','O','S'):sfnt_tag('G','S','U','B'),&table)==SfntStatus::ok && sfnt_layout_open(table,static_cast<SfntLayoutKind>(kind),&layout)==SfntStatus::ok;at+=size;if(!ok)break;
        const u32 words=(layout.feature_count+63u)/64+(layout.lookup_count+63u)/64;auto* bitmap=reinterpret_cast<u64*>(memory+page-8);auto* scratch=reinterpret_cast<u64*>(memory+page*3-words*8);auto* sources=reinterpret_cast<u64*>(memory+page*5-8);*bitmap=0;SfntNominalParticipationResult result{};
        SfntCmap cmap{};u64 source_work=0;ok=sfnt_cmap_open(face,&cmap)==SfntStatus::ok && sfnt_nominal_sources(cmap,{sources,1},&source_work)==SfntStatus::ok;if(!ok)break;
        ok=sfnt_nominal_participation_add(layout,limit,{bitmap,1},{scratch,words},&result)==SfntStatus::ok && (*bitmap&~1ull)==expected;++guards;++font_checks;glyph_checks+=limit-1;
        if(!ok){print(utf8_literal("FAIL font="));number(i);print(utf8_literal(" actual="));number(*bitmap);print(utf8_literal(" expected="));number(expected);print(utf8_literal("\n"));break;}
        *bitmap=0;ok=sfnt_nominal_participation_add(layout,limit,{bitmap,1},{scratch,words},&result,1000000,{sources,1})==SfntStatus::ok && (*bitmap&~1ull)==expected;++guards;
        const u64 full=result.operations;if(full>10000){ok=false;break;}
        for(u64 budget=0;budget<=full && ok;++budget){*bitmap=0;const auto s=sfnt_nominal_participation_add(layout,limit,{bitmap,1},{scratch,words},&result,budget,{sources,1});++budget_checks;ok=s==(budget<full?SfntStatus::resource_limit:SfntStatus::ok) && result.operations<=budget;if(s==SfntStatus::ok)ok=ok && (*bitmap&~1ull)==expected;}
        u64 delayed=0xaaaaaaaaaaaaaaaaull;SfntNominalSourceCache lazy{&cmap,{&delayed,1},false};*bitmap=0;
        ok=ok&&sfnt_nominal_participation_add(layout,limit,{bitmap,1},{scratch,words},&result,1000000,{},&lazy)==SfntStatus::ok&&(*bitmap&~1ull)==expected;
        const u64 lazy_work=maximum(full,lazy.ready?source_work:0);
        if(lazy.ready){++lazy_built;ok=ok&&delayed==*sources;}else{++lazy_skipped;ok=ok&&delayed==0xaaaaaaaaaaaaaaaaull;}
        *bitmap=0;ok=ok&&sfnt_nominal_participation_add(layout,limit,{bitmap,1},{scratch,words},&result,1000000,{},&lazy)==SfntStatus::ok&&(*bitmap&~1ull)==expected&&result.operations==full;
        for(u64 budget=0;budget<=lazy_work&&ok;++budget){lazy.ready=false;*bitmap=0;
            const auto status=sfnt_nominal_participation_add(layout,limit,{bitmap,1},{scratch,words},&result,budget,{},&lazy);++budget_checks;
            ok=status==(budget<lazy_work?SfntStatus::resource_limit:SfntStatus::ok)&&result.operations<=budget;
            if(status==SfntStatus::ok)ok=ok&&(*bitmap&~1ull)==expected;
        }
        lazy.bitmap.count=0;ok=ok&&sfnt_nominal_participation_add(layout,limit,{bitmap,1},{scratch,words},&result,1000000,{},&lazy)==SfntStatus::output_too_small;
        lazy.bitmap.count=1;ok=ok&&sfnt_nominal_participation_add(layout,limit,{bitmap,1},{scratch,words},&result,1000000,{sources,1},&lazy)==SfntStatus::invalid_input;
        lazy.cmap=nullptr;ok=ok&&sfnt_nominal_participation_add(layout,limit,{bitmap,1},{scratch,words},&result,1000000,{},&lazy)==SfntStatus::invalid_input;
        SfntLookup first_lookup{};ConstSlice<u8> first_subtable{};
        if(ok && sfnt_layout_lookup(layout,0,&first_lookup)==SfntStatus::ok && first_lookup.type==(kind?8:6) && sfnt_lookup_subtable(first_lookup,0,&first_subtable)==SfntStatus::ok && byte_load_be16(first_subtable.data)==3){
            const u64 saved=*sources;*sources=0;*bitmap=0;ok=sfnt_nominal_participation_add(layout,limit,{bitmap,1},{scratch,words},&result,1000000,{sources,1})==SfntStatus::ok && !*bitmap;
            const u64 pruned_work=result.operations;for(u64 budget=0;budget<=pruned_work && ok;++budget){*bitmap=0;const auto status=sfnt_nominal_participation_add(layout,limit,{bitmap,1},{scratch,words},&result,budget,{sources,1});ok=status==(budget<pruned_work?SfntStatus::resource_limit:SfntStatus::ok) && result.operations<=budget && !*bitmap;++budget_checks;}*sources=saved;
        }
        // Accumulation preserves prior tables, including bits outside this face.
        const u64 retained=1ull<<63;*bitmap=retained;ok=ok && sfnt_nominal_participation_add(layout,limit,{bitmap,1},{scratch,words},&result)==SfntStatus::ok && *bitmap==(retained|expected);
        *bitmap=retained;ok=ok && sfnt_nominal_participation_add(layout,limit,{bitmap,1},{scratch,words-1},&result)==SfntStatus::output_too_small && *bitmap==retained;
        ok=ok && sfnt_nominal_participation_add(layout,limit,{bitmap,0},{scratch,words},&result)==SfntStatus::output_too_small && *bitmap==retained;
    }
    VirtualFree(memory,0,MEM_RELEASE);return ok && at==all.count && lazy_built && lazy_skipped;
}
int font_bitmap(const wchar_t* path) noexcept {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return 2;LARGE_INTEGER len{};
    if(!GetFileSizeEx(file,&len) || len.QuadPart<12 || len.QuadPart>0xffffffffll){CloseHandle(file);return 2;}HANDLE mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);if(!mapping){CloseHandle(file);return 2;}
    auto* bytes=static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));if(!bytes){CloseHandle(mapping);CloseHandle(file);return 2;}
    SfntFace face{};SfntCmap cmap{};u16 count=0;auto s=sfnt_open({bytes,static_cast<u32>(len.QuadPart)},0,&face);if(s==SfntStatus::ok)s=sfnt_glyph_count(face,&count);u64 bits[1024]{},scratch[2048]{},sources[1024]{},source_work=0,work=0,lazy_bits[1024]{},lazy_sources[1024]{},lazy_work=0;SfntNominalSourceCache lazy{&cmap,{lazy_sources,1024},false};
    if(s==SfntStatus::ok)s=sfnt_cmap_open(face,&cmap);if(s==SfntStatus::ok)s=sfnt_nominal_sources(cmap,{sources,1024},&source_work);
    for(u32 kind=0;kind<2 && s==SfntStatus::ok;++kind){ConstSlice<u8> raw{};s=sfnt_table(face,kind?sfnt_tag('G','P','O','S'):sfnt_tag('G','S','U','B'),&raw);if(s==SfntStatus::missing_table){s=SfntStatus::ok;continue;}if(s!=SfntStatus::ok)break;SfntLayout l{};s=sfnt_layout_open(raw,static_cast<SfntLayoutKind>(kind),&l);if(s!=SfntStatus::ok)break;SfntNominalParticipationResult r{};s=sfnt_nominal_participation_add(l,count,{bits,1024},{scratch,2048},&r,1000000,{sources,(count+63u)/64});work+=r.operations;
        if(s==SfntStatus::ok)s=sfnt_nominal_participation_add(l,count,{lazy_bits,1024},{scratch,2048},&r,1000000,{},&lazy);lazy_work+=r.operations;
        if(s==SfntStatus::ok&&!bytes_equal(bits,lazy_bits,((count+63u)/64)*8))s=SfntStatus::malformed;}
    print(utf8_literal("{\"status\":"));number(static_cast<u32>(s));print(utf8_literal(",\"glyph_count\":"));number(count);print(utf8_literal(",\"operations\":"));number(work);print(utf8_literal(",\"source_operations\":"));number(source_work);print(utf8_literal(",\"lazy_operations\":"));number(lazy_work+(lazy.ready?source_work:0));print(utf8_literal(",\"source_built\":"));print(lazy.ready?utf8_literal("true"):utf8_literal("false"));print(utf8_literal(",\"bits\":["));
    if(s==SfntStatus::ok)for(u32 i=0;i<(count+63u)/64;++i){if(i)print(utf8_literal(","));number(bits[i]);}print(utf8_literal("]}\n"));UnmapViewOfFile(bytes);CloseHandle(mapping);CloseHandle(file);return 0;
}
int run() noexcept {
    const wchar_t* command=GetCommandLineW();bool quoted=false;while(*command){if(*command=='"')quoted=!quoted;else if(!quoted && (*command==' ' || *command=='\t'))break;++command;}while(*command==' ' || *command=='\t')++command;
    if(*command){wchar_t path[32768];u32 n=0;quoted=*command=='"';if(quoted)++command;while(*command && !(quoted?*command=='"':(*command==' ' || *command=='\t'))){if(n+1>=32768)return 2;path[n++]=*command++;}if(quoted){if(*command!='"')return 2;++command;}while(*command==' ' || *command=='\t')++command;if(*command || !n)return 2;path[n]=0;return font_bitmap(path);}
    const bool boundary_ok=boundaries();if(!boundary_ok)print(utf8_literal("FAIL participation boundaries\n"));const bool ok=boundary_ok && corpus();print(utf8_literal("fonts="));number(font_checks);print(utf8_literal("\nglyph_checks="));number(glyph_checks);print(utf8_literal("\nbudget_checks="));number(budget_checks);print(utf8_literal("\nguards="));number(guards);print(utf8_literal("\nlazy_built="));number(lazy_built);print(utf8_literal("\nlazy_skipped="));number(lazy_skipped);print(ok?utf8_literal("\nPASS native nominal participation\n"):utf8_literal("\nFAIL native nominal participation\n"));return ok?0:1;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_nominal_participation_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
