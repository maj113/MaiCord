// Manual: --filter-cost measures lookup-flag filtering; the registered run is the check.
#include "maicord/text/gdef.hpp"
#include "maicord/core/byte_order.hpp"
#include <Windows.h>

namespace maicord {namespace {
u8 seen[65536];
volatile u32 guard_observations=0;
void observe(SfntStatus status) noexcept {guard_observations=guard_observations+static_cast<u32>(status)+1;}
void print(Utf8View s) noexcept {DWORD n=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s.data,s.bytes,&n,nullptr);}
void number(u64 v) noexcept {char s[32];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n/2;++i){char c=s[i];s[i]=s[n-i-1];s[n-i-1]=c;}print({s,n});}
void put(u8* p,u32 at,u16 v) noexcept {p[at]=static_cast<u8>(v>>8);p[at+1]=static_cast<u8>(v);}
void fixture(u8* p) noexcept {
    clear_bytes(p,64);put(p,0,1);put(p,2,2);put(p,4,14);put(p,10,30);put(p,12,40);
    put(p,14,1);put(p,18,5);for(u16 i=0;i<5;++i)put(p,20+i*2,i);
    put(p,30,1);put(p,32,3);put(p,34,2);put(p,36,2);put(p,38,1);
    put(p,40,1);put(p,42,1);byte_store_be32(p+44,8);put(p,48,1);put(p,50,1);put(p,52,3);
}
void full_fixture(u8* p) noexcept {
    clear_bytes(p,256);put(p,0,1);put(p,2,3);put(p,4,18);put(p,6,44);put(p,8,64);put(p,10,34);put(p,12,128);byte_store_be32(p+14,158);
    put(p,18,1);put(p,22,5);for(u16 i=0;i<5;++i)put(p,24+i*2,i);put(p,34,1);put(p,36,3);put(p,38,2);put(p,40,2);put(p,42,1);
    for(u32 start=44;start<=64;start+=20){put(p,start,6);put(p,start+2,1);put(p,start+4,12);put(p,start+6,1);put(p,start+8,1);put(p,start+10,2);}
    put(p,56,3);put(p,58,1);put(p,60,5);put(p,62,9);
    put(p,76,5);const u16 offsets[]{12,16,20,34,40};for(u32 i=0;i<5;++i)put(p,78+i*2,offsets[i]);
    put(p,88,1);put(p,90,65506);put(p,92,2);put(p,94,5);put(p,96,3);put(p,98,100);put(p,100,6);put(p,102,16);put(p,104,16);put(p,106,1);put(p,108,0xc000);
    put(p,110,3);put(p,112,300);put(p,116,3);put(p,118,200);put(p,120,6);put(p,126,0x8000);
    put(p,128,1);put(p,130,2);byte_store_be32(p+132,12);byte_store_be32(p+136,20);put(p,140,1);put(p,142,2);put(p,144,3);put(p,146,4);put(p,148,2);put(p,150,1);put(p,152,3);put(p,154,3);
    put(p,158,1);byte_store_be32(p+160,12);put(p,164,1);byte_store_be32(p+166,22);put(p,170,1);put(p,172,1);put(p,176,16384);put(p,178,16384);put(p,180,1);put(p,182,1);put(p,184,1);put(p,188,40);
}
bool units() noexcept {
    u8 p[64];fixture(p);SfntGdef g{};SfntLookupFilter f{};SfntCoverage c{};
    if(sfnt_gdef_open({p,54},5,&g)!=SfntStatus::ok || sfnt_gdef_mark_set(g,0,&c)!=SfntStatus::ok)return false;
    if(sfnt_lookup_filter_prepare(g,0x110,0,&f)!=SfntStatus::ok || sfnt_lookup_ignores(g,f,3,SfntGlyphClass::mark) || sfnt_lookup_ignores(g,f,4,SfntGlyphClass::component))return false;
    if(sfnt_lookup_filter_prepare(g,0x118,0,&f)!=SfntStatus::ok || !sfnt_lookup_ignores(g,f,3,SfntGlyphClass::mark))return false;
    if(sfnt_lookup_filter_prepare(g,0x100,0,&f)!=SfntStatus::ok || !sfnt_lookup_ignores(g,f,3,SfntGlyphClass::mark))return false;
    if(sfnt_lookup_filter_prepare(g,0x200,0,&f)!=SfntStatus::ok || sfnt_lookup_ignores(g,f,3,SfntGlyphClass::mark))return false;
    if(sfnt_lookup_filter_prepare(g,0x20,0,&f)!=SfntStatus::unsupported || f.flags)return false;
    if(sfnt_gdef_mark_set(g,1,&c)!=SfntStatus::glyph_out_of_range || c.format)return false;
    if(sfnt_lookup_filter_prepare({},8,0,&f)!=SfntStatus::ok || !sfnt_lookup_ignores({},f,3,SfntGlyphClass::mark))return false;
    for(u32 len=0;len<14;++len)if(sfnt_gdef_open({p,len},5,&g)==SfntStatus::ok || g.table.data)return false;
    put(p,28,5);if(sfnt_gdef_open({p,54},5,&g)!=SfntStatus::malformed || g.table.data)return false;
    fixture(p);put(p,4,2);if(sfnt_gdef_open({p,54},5,&g)!=SfntStatus::malformed)return false;
    fixture(p);put(p,2,1);if(sfnt_gdef_open({p,54},5,&g)!=SfntStatus::unsupported)return false;
    fixture(p);byte_store_be32(p+44,0);if(sfnt_gdef_open({p,54},5,&g)!=SfntStatus::ok || sfnt_gdef_mark_set(g,0,&c)!=SfntStatus::malformed)return false;
    // The mark-set offset is 32 bits and can legitimately exceed 64 KiB.
    auto* wide=static_cast<u8*>(VirtualAlloc(nullptr,70000,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!wide)return false;
    fixture(wide);byte_store_be32(wide+44,66000);put(wide,66040,1);put(wide,66042,1);put(wide,66044,3);
    bool ok=sfnt_gdef_open({wide,70000},5,&g)==SfntStatus::ok && sfnt_gdef_mark_set(g,0,&c)==SfntStatus::ok && sfnt_coverage_index(c,3)==0;VirtualFree(wide,0,MEM_RELEASE);
    // GDEF 1.3 store: one axis, one region and a design-unit delta of 40.
    clear_bytes(p,64);put(p,0,1);put(p,2,3);byte_store_be32(p+14,18);put(p,18,1);byte_store_be32(p+20,12);put(p,24,1);byte_store_be32(p+26,22);
    put(p,30,1);put(p,32,1);put(p,36,16384);put(p,38,16384);put(p,40,1);put(p,42,1);put(p,44,1);put(p,48,40);
    ItemVariationStore store{};VariationInstance instance{};double coordinate=1,scalar=0,delta=0;
    ok=ok && sfnt_gdef_open({p,50},5,&g)==SfntStatus::ok && sfnt_gdef_variation_store(g,&store)==SfntStatus::ok &&
       variation_instance_prepare(store,{&coordinate,1},{&scalar,1},&instance)==SfntStatus::ok && item_variation_delta(instance,0,&delta)==SfntStatus::ok && delta==40;
    coordinate=0;ok=ok && variation_instance_prepare(store,{&coordinate,1},{&scalar,1},&instance)==SfntStatus::ok && item_variation_delta(instance,0,&delta)==SfntStatus::ok && delta==0;
    return ok;
}
bool guards() noexcept {
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;
    auto* mem=static_cast<u8*>(VirtualAlloc(nullptr,page*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!mem)return false;
    DWORD old=0;if(!VirtualProtect(mem+page,page,PAGE_NOACCESS,&old)){VirtualFree(mem,0,MEM_RELEASE);return false;}
    u8 seed[256];full_fixture(seed);u32 random=0xb682fe19;
    for(u32 i=0;i<20000;++i){random^=random<<13;random^=random>>17;random^=random<<5;const u32 size=i<191?i:random%191;u8* p=mem+page-size;
        for(u32 j=0;j<size;++j)p[j]=seed[j];if(i>=191 && size)p[(random>>8)%size]^=static_cast<u8>(random>>16);
        SfntGdef g{};if(sfnt_gdef_open({p,size},5,&g)!=SfntStatus::ok)continue;
        for(u16 gid=0;gid<5;++gid)guard_observations=guard_observations+static_cast<u32>(sfnt_gdef_glyph_class(g,gid))+sfnt_class_value(g.mark_classes,gid);
        for(u16 k=0;k<64;++k){SfntCoverage cov{};const auto status=sfnt_gdef_mark_set(g,k,&cov);observe(status);if(status==SfntStatus::glyph_out_of_range)break;}
        ItemVariationStore store{};observe(sfnt_gdef_variation_store(g,&store,1000));
    }
    VirtualFree(mem,0,MEM_RELEASE);return true;
}
struct Reader {const u8* data;u32 size,at;
    bool word(u32* v) noexcept {if(size-at<4)return false;*v=byte_load_le32(data+at);at+=4;return true;}
    bool take(u32 n,const u8** v) noexcept {if(n>size-at)return false;*v=data+at;at+=n;return true;}
};
bool corpus(const wchar_t* path) noexcept {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;
    LARGE_INTEGER len{};if(!GetFileSizeEx(file,&len) || len.QuadPart<12 || len.QuadPart>0xffffffffll){CloseHandle(file);return false;}
    HANDLE mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);if(!mapping){CloseHandle(file);return false;}
    auto* bytes=static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));Reader r{bytes,static_cast<u32>(len.QuadPart),8};u32 total=0,failures=0;u64 properties=0,memberships=0,filters=0,stores=0;
    bool ok=bytes && bytes_equal(bytes,"MCGDEF2",8) && r.word(&total) && total>0;
    for(u32 record=0;record<total && ok;++record){
        u32 limit=0,size=0,flags=0,sc=0;const u8* raw=nullptr;
        ok=r.word(&limit)&&r.word(&size)&&r.word(&flags)&&r.word(&sc)&&limit>0&&limit<=65536&&flags<8&&sc<=65535&&r.take(size,&raw);if(!ok)break;
        SfntGdef g{};bool match=(flags&1)?sfnt_gdef_open({raw,size},limit,&g)==SfntStatus::ok:size==0;
        const u8* classes=nullptr;ok=r.take(limit*4,&classes);if(!ok)break;
        if(match)for(u32 gid=0;gid<limit;++gid){if(static_cast<u16>(sfnt_gdef_glyph_class(g,static_cast<u16>(gid)))!=byte_load_le16(classes+gid*4) || sfnt_class_value(g.mark_classes,static_cast<u16>(gid))!=byte_load_le16(classes+gid*4+2))match=false;properties+=2;}
        SfntCoverage none{};const auto ss=sfnt_gdef_mark_set(g,static_cast<u16>(sc),&none);if(ss!=((flags&2)?SfntStatus::glyph_out_of_range:SfntStatus::missing_table))match=false;
        for(u32 i=0;i<sc && ok;++i){u32 n=0;ok=r.word(&n)&&n<=limit;if(!ok)break;clear_bytes(seen,sizeof(seen));SfntCoverage cov{};const bool valid=(flags&2) && sfnt_gdef_mark_set(g,static_cast<u16>(i),&cov)==SfntStatus::ok;if(!valid || cov.covered_count!=n)match=false;
            for(u32 k=0;k<n && ok;++k){u32 gid=0;ok=r.word(&gid)&&gid<limit&&!seen[gid];if(ok){seen[gid]=1;u16 actual=0;if(valid && (sfnt_coverage_glyph(cov,k,&actual)!=SfntStatus::ok || actual!=gid))match=false;}}
            if(valid)for(u32 gid=0;gid<limit;++gid){if((sfnt_coverage_index(cov,static_cast<u16>(gid))!=sfnt_not_covered)!=(seen[gid]!=0))match=false;++memberships;}}
        u32 axes=0,regions=0,data=0;ok=ok&&r.word(&axes)&&r.word(&regions)&&r.word(&data);ItemVariationStore store{};const auto vs=sfnt_gdef_variation_store(g,&store);
        if(vs!=((flags&4)?SfntStatus::ok:SfntStatus::missing_table) || store.axis_count!=axes || store.region_count!=regions || store.offsets.count/4!=data)match=false;if(flags&4)++stores;
        u32 configs=0;ok=ok&&r.word(&configs)&&configs<=1000000;
        for(u32 i=0;i<configs && ok;++i){u32 f=0,index=0;const u8* bits=nullptr;ok=r.word(&f)&&r.word(&index)&&f<=65535&&index<=65535&&r.take((limit+7)/8,&bits);if(!ok)break;
            SfntLookupFilter filter{};if(sfnt_lookup_filter_prepare(g,static_cast<u16>(f),static_cast<u16>(index),&filter)!=SfntStatus::ok){match=false;continue;}
            for(u32 gid=0;gid<limit;++gid){if(sfnt_lookup_ignores(g,filter,static_cast<u16>(gid),sfnt_gdef_glyph_class(g,static_cast<u16>(gid)))!=(((bits[gid/8]>>(gid%8))&1)!=0))match=false;++filters;}}
        if(!match){++failures;print(utf8_literal("mismatch_record="));number(record);print(utf8_literal("\n"));}
    }
    ok=ok && r.at==r.size;
    print(utf8_literal("records="));number(total);print(utf8_literal("\nproperty_checks="));number(properties);print(utf8_literal("\nmark_memberships="));number(memberships);print(utf8_literal("\nfilter_checks="));number(filters);print(utf8_literal("\nvariation_stores="));number(stores);print(utf8_literal("\nfailures="));number(failures);print(utf8_literal("\ncomplete="));number(ok);print(utf8_literal("\n"));
    if(bytes)UnmapViewOfFile(bytes);CloseHandle(mapping);CloseHandle(file);return ok&&!failures;
}
#include "lookup_filter_units.inc"
int run() noexcept {
    if(!units()){print(utf8_literal("FAIL GDEF units\n"));return 1;}if(!guards()){print(utf8_literal("FAIL GDEF guards\n"));return 2;}
    if(!filter_membership_units()){print(utf8_literal("FAIL filter membership\n"));return 5;}
    const wchar_t* cmd=GetCommandLineW();bool quote=false;while(*cmd){if(*cmd=='"')quote=!quote;else if(!quote && (*cmd==' ' || *cmd=='\t'))break;++cmd;}while(*cmd==' ' || *cmd=='\t')++cmd;
    if(*cmd){if(lstrcmpW(cmd,L"--filter-cost")==0)return filter_cost()?0:6;
        wchar_t path[32768];u32 n=0;quote=*cmd=='"';if(quote)++cmd;while(*cmd && !(quote?*cmd=='"':(*cmd==' ' || *cmd=='\t'))){if(n+1>=32768)return 3;path[n++]=*cmd++;}if(quote){if(*cmd!='"')return 3;++cmd;}while(*cmd==' ' || *cmd=='\t')++cmd;if(*cmd || !n)return 3;path[n]=0;if(!corpus(path))return 4;}
    print(utf8_literal("PASS native GDEF\n"));return 0;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_gdef_tests_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
