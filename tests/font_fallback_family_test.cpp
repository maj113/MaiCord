#include "maicord/text/font_fallback_catalog.hpp"
#include "maicord/text/font_fallback_range.hpp"
#include "maicord/text/font_catalog_builder.hpp"
#include "maicord/text/font_catalog_coverage.hpp"
#include "maicord/core/byte_order.hpp"
namespace maicord {namespace {
u32 checks=0,cases=0,groups=0,range_cases=0;u64 probes=0;
void print(const char* s) noexcept {u32 n=0;while(s[n])++n;DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s,n,&done,nullptr);}
void number(u64 v) noexcept {char s[32],out[32];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n;++i)out[i]=s[n-i-1];out[n]=0;print(out);}
#define CHECK(x) do{++checks;if(!(x)){print("FAIL line=");number(__LINE__);print(" group=");number(groups);print(" case=");number(cases);print("\n");return false;}}while(0)
struct Mock {u32 mask=0,called=0,fail=0xffffffffu;};
SfntStatus mock_coverage(void* context,u32 token,u32,bool* out) noexcept {auto& m=*static_cast<Mock*>(context);++m.called;*out=false;if(token==m.fail)return SfntStatus::malformed;*out=(m.mask&(1u<<token))!=0;return SfntStatus::ok;}
bool memory_guards() noexcept {
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;CHECK(page>=4096);
    auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,static_cast<SIZE_T>(page)*4,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(memory);
    struct Cleanup {u8* memory;~Cleanup() noexcept{VirtualFree(memory,0,MEM_RELEASE);}} cleanup{memory};
    auto* input=reinterpret_cast<NativeFontProperties*>(memory+page)-4;const NativeFontProperties source[]{{400,5,0},{300,5,0},{700,5,0},{400,5,2}};copy_bytes(input,source,sizeof(source));
    auto* output=reinterpret_cast<u32*>(memory+page*3)-4;output[-1]=0x197ac05d;DWORD old=0;
    CHECK(VirtualProtect(memory,page,PAGE_READONLY,&old)&&VirtualProtect(memory+page,page,PAGE_NOACCESS,&old)&&VirtualProtect(memory+page*3,page,PAGE_NOACCESS,&old));
    u32 count=99,used=0;CHECK(native_font_fallback_order({input,4},{600,5,0},{output,4},&count,100,&used)==SfntStatus::ok&&count==4);
    const u32 expected[]{2,1,0,3};CHECK(bytes_equal(output,expected,sizeof(expected))&&output[-1]==0x197ac05d&&bytes_equal(input,source,sizeof(source)));
    CHECK(native_font_fallback_order({input,4},{600,5,0},{output,4},&count,used-1,&used)==SfntStatus::resource_limit&&!count&&output[-1]==0x197ac05d);return true;
}
bool guards() noexcept {
    CHECK(memory_guards());
    const NativeFontProperties input[]{{400,5,0},{300,5,0},{700,5,0},{400,5,2}};u32 output[4]{},count=99,used=0;
    CHECK(native_font_fallback_order({input,4},{600,5,0},{output,4},&count,100,&used)==SfntStatus::ok&&count==4);
    const u32 expected[]{2,1,0,3};CHECK(bytes_equal(output,expected,sizeof(expected)));const u32 needed=used;
    for(u32 budget=0;budget<needed;++budget)CHECK(native_font_fallback_order({input,4},{600,5,0},{output,4},&count,budget,&used)==SfntStatus::resource_limit&&!count&&used==budget);
    CHECK(native_font_fallback_order({input,4},{400,5,0},{output,3},&count)==SfntStatus::output_too_small&&!count);
    CHECK(native_font_fallback_order({nullptr,4},{400,5,0},{output,4},&count)==SfntStatus::invalid_input&&!count);
    CHECK(native_font_fallback_order({input,4},{400,5,0},{nullptr,4},&count)==SfntStatus::invalid_input&&!count);
    CHECK(native_font_fallback_order({input,4},{400,5,0},{output,4},nullptr)==SfntStatus::invalid_input);
    CHECK(native_font_fallback_order({},{400,5,0},{},&count,0,&used)==SfntStatus::unsupported_sequence&&!count&&!used);
    const NativeFontProperties invalid[]{{0,5,0},{1001,5,0},{400,0,0},{400,10,0},{400,5,3}};
    for(auto p:invalid){CHECK(native_font_fallback_order({&p,1},{400,5,0},{output,4},&count)==SfntStatus::invalid_input&&!count);CHECK(native_font_fallback_order({input,4},p,{output,4},&count)==SfntStatus::invalid_input&&!count);}
    NativeFontCatalogCandidate candidates[4]{};NativeFontCatalogFace faces[4]{};u16 names[]{'P','r','o','b','e',0};
    for(u32 i=0;i<4;++i){candidates[i]={input[i],i,0xffffffffu,0,0};faces[i]={0,i,0,5,i,1};}
    NativeFontCatalog catalog{{},{faces,4},{candidates,4},{},{names,6},{0,4,4,0,6}};NativeFontProperties scratch[4];u32 indices[4];
    CHECK(native_font_catalog_fallback_order(catalog,{names,5},{600,5,0},{scratch,4},{indices,4},{output,4},&count)==SfntStatus::ok&&count==4&&bytes_equal(output,expected,sizeof(expected)));
    const u16 lower[]{'p','r','o','b','e'};
    CHECK(native_font_catalog_fallback_order(catalog,{lower,5},{600,5,0},{scratch,4},{indices,4},{output,4},&count,100,100,&used)==SfntStatus::ok&&count==4&&bytes_equal(output,expected,sizeof(expected)));
    const u32 catalog_work=used;for(u32 budget=0;budget<catalog_work;++budget)CHECK(native_font_catalog_fallback_order(catalog,{names,5},{600,5,0},{scratch,4},{indices,4},{output,4},&count,100,budget,&used)==SfntStatus::resource_limit&&!count&&used==budget);
    CHECK(native_font_catalog_fallback_order(catalog,{names,5},{600,5,0},{scratch,4},{indices,4},{output,4},&count,100,catalog_work,&used)==SfntStatus::ok&&count==4&&used==catalog_work);
    Mock mock{};NativeFontCatalogMatch result{};
    for(u32 mask=0;mask<16;++mask){mock={mask};CHECK(native_font_catalog_fallback_select(catalog,{output,count},{600,5,0},65,mock_coverage,&mock,&result)==SfntStatus::ok);
        const u32 chosen=mask&4?2u:mask&2?1u:mask&1?0u:mask&8?3u:native_fallback_missing;CHECK(result.candidate==chosen);
        if(chosen!=native_fallback_missing)CHECK(result.properties.weight==700&&result.simulations==(chosen==2?0u:1u));}
    mock={0,0,2};CHECK(native_font_catalog_fallback_select(catalog,{output,count},{600,5,0},65,mock_coverage,&mock,&result)==SfntStatus::malformed&&result.candidate==native_fallback_missing&&mock.called==1);
    mock={2};CHECK(native_font_catalog_fallback_select(catalog,{output,count},{600,5,0},65,mock_coverage,&mock,&result,1,&used)==SfntStatus::resource_limit&&result.candidate==native_fallback_missing&&used==1);
    CHECK(native_font_catalog_fallback_select(catalog,{output,count},{600,5,0},0xd800,mock_coverage,&mock,&result)==SfntStatus::invalid_input);
    CHECK(native_font_catalog_fallback_select(catalog,{output,count},{600,5,0},0x110000,mock_coverage,&mock,&result)==SfntStatus::invalid_input);
    CHECK(native_font_catalog_fallback_select(catalog,{output,count},{600,5,0},65,nullptr,&mock,&result)==SfntStatus::invalid_input);
    CHECK(native_font_catalog_fallback_select(catalog,{output,count},{600,5,0},65,mock_coverage,&mock,nullptr)==SfntStatus::invalid_input);
    u32 bad=99;CHECK(native_font_catalog_fallback_select(catalog,{&bad,1},{600,5,0},65,mock_coverage,&mock,&result)==SfntStatus::invalid_input&&result.candidate==native_fallback_missing);
    CHECK(native_font_catalog_fallback_order(catalog,{names,5},{600,5,0},{scratch,4},{indices,4},{output,4},&count,3)==SfntStatus::resource_limit&&!count);
    const u16 missing[]{'N','o'};CHECK(native_font_catalog_fallback_order(catalog,{missing,2},{600,5,0},{scratch,4},{indices,4},{output,4},&count)==SfntStatus::unsupported_sequence&&!count);
    CHECK(native_font_catalog_fallback_order(catalog,{names,5},{600,5,0},{scratch,3},{indices,4},{output,4},&count)==SfntStatus::output_too_small&&!count);
    CHECK(native_font_catalog_fallback_order(catalog,{names,5},{600,5,0},{scratch,4},{indices,3},{output,4},&count)==SfntStatus::output_too_small&&!count);
    const auto saved=faces[3];faces[3].first_candidate=4;CHECK(native_font_catalog_fallback_order(catalog,{names,5},{600,5,0},{scratch,4},{indices,4},{output,4},&count)==SfntStatus::invalid_input&&!count);faces[3]=saved;
    candidates[3].face=0;CHECK(native_font_catalog_fallback_order(catalog,{names,5},{600,5,0},{scratch,4},{indices,4},{output,4},&count)==SfntStatus::invalid_input&&!count);candidates[3].face=3;
    CHECK(!GetModuleHandleW(L"dwrite.dll")&&!GetModuleHandleW(L"DWriteCore.dll"));return true;
}
struct File {
    HANDLE handle=INVALID_HANDLE_VALUE;const u8* data=nullptr;u32 size=0;
    bool open(const wchar_t* path) noexcept {handle=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);if(handle==INVALID_HANDLE_VALUE)return false;LARGE_INTEGER n{};
        if(!GetFileSizeEx(handle,&n)||n.QuadPart<=0||n.QuadPart>0xffffffffll)return false;size=static_cast<u32>(n.QuadPart);HANDLE map=CreateFileMappingW(handle,nullptr,PAGE_READONLY,0,0,nullptr);if(!map)return false;
        data=static_cast<const u8*>(MapViewOfFile(map,FILE_MAP_READ,0,0,0));CloseHandle(map);return data!=nullptr;}
    ~File() noexcept{if(data)UnmapViewOfFile(data);if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
};
struct Reader {const u8* data;u32 size,at=0;
    bool read(void* out,u32 bytes) noexcept{if(bytes>size-at)return false;copy_bytes(out,data+at,bytes);at+=bytes;return true;}
    bool integer(u32* out) noexcept{return read(out,4);}
    bool text(wchar_t* out,u32* units) noexcept {if(!integer(units)||*units>=32768||!read(out,*units*2))return false;out[*units]=0;return true;}
};
struct Expected {u32 file,index,simulations;NativeFontProperties properties;u32 axes,tags[2],bits[2];};
struct Coverage {const NativeFontCatalog* catalog;NativeFontSession* session;NativeFontCoverageCache cache;NativeFontCoverageSlot slots[16];NativeFontResult failure{};};
SfntStatus actual_coverage(void* context,u32 candidate,u32 scalar,bool* covered) noexcept {
    auto& c=*static_cast<Coverage*>(context);*covered=false;++probes;
    c.failure=native_font_coverage_probe(&c.cache,*c.catalog,candidate,scalar,covered);
    return c.failure.ok()?SfntStatus::ok:c.failure.error==NativeFontError::font?c.failure.font_status:SfntStatus::malformed;
}
struct RangeContext {const NativeFontCatalog* catalog;ConstSlice<u32> order;NativeFontProperties request;Coverage* coverage;u32 policy;bool active=true;ConstSlice<u32> base;};
u32 route_policy(u32 mode,u32 cp) noexcept {
    if((mode>=1&&mode<=3)||mode==6)return cp<0x80?0u:cp<0x300?1u:cp<0x370?2u:3u;
    if(mode==4||mode==10)return cp<0x80?0u:1u;
    if(mode==5){const u32 starts[]{0x180b,0x180e,0x180f,0x1810,0xfe00,0xfe10,0xe0100,0xe01f0};u32 i=0;while(i<8&&starts[i]<=cp)++i;return i;}
    if(mode==7)return cp<0x61?0u:cp==0x61?1u:2u;return 0;
}
SfntStatus range_route(void* context,u32 scalar,u32* output) noexcept {*output=route_policy(static_cast<RangeContext*>(context)->policy,scalar);return SfntStatus::ok;}
SfntStatus range_font(void* context,u32 scalar,u32* output) noexcept {
    auto& c=*static_cast<RangeContext*>(context);*output=native_fallback_missing;if(!c.active)return SfntStatus::ok;
    NativeFontCatalogMatch match{};const auto status=native_font_catalog_fallback_select(*c.catalog,c.order,c.request,scalar,actual_coverage,c.coverage,&match);
    if(status==SfntStatus::ok)*output=match.candidate;return status;
}
SfntStatus range_base(void* context,u32 scalar,u32* output) noexcept {
    auto& c=*static_cast<RangeContext*>(context);*output=native_fallback_missing;if(!c.base.count)return SfntStatus::ok;
    NativeFontCatalogMatch match{};const auto status=native_font_catalog_fallback_select(*c.catalog,c.base,c.request,scalar,actual_coverage,c.coverage,&match);
    if(status==SfntStatus::ok)*output=match.candidate;return status;
}
SfntStatus range_select(void* context,u32 scalar,NativeFallbackChoice* output) noexcept {
    auto& c=*static_cast<RangeContext*>(context);NativeFontCatalogMatch match{};
    if(c.policy==4&&scalar>=0x80){*output={native_fallback_missing,native_fallback_missing};return SfntStatus::ok;}
    const auto status=native_font_catalog_fallback_select(*c.catalog,c.order,c.request,scalar,actual_coverage,c.coverage,&match);
    const u32 interval=!c.policy?0u:scalar<0x80?0u:scalar<0x300?1u:scalar<0x370?2u:3u;
    if(status==SfntStatus::ok)*output={match.candidate,interval};return status;
}
SfntStatus range_coverage(void* context,u32 candidate,u32 scalar,bool* covered) noexcept {
    return actual_coverage(static_cast<RangeContext*>(context)->coverage,candidate,scalar,covered);
}
bool group(Reader& reader,bool ranges) noexcept {
    u32 flags=0,nfiles=0,nexpected=0;CHECK(reader.integer(&flags)&&((flags&0x300u)?(flags&~0x300u)<=21:flags<=9)&&reader.integer(&nfiles)&&nfiles&&nfiles<=256&&reader.integer(&nexpected)&&(nexpected||ranges)&&nexpected<=1024);
    NativeFontCatalogBuilder builder{};Arena arena{};Coverage coverage{};
    struct Cleanup {NativeFontCatalogBuilder& b;Arena& a;Coverage& c;~Cleanup() noexcept {
        native_font_coverage_release(&c.cache);if(c.session)native_font_session_release(&c.session);native_font_catalog_builder_release(&b);arena_release(&a);}} cleanup{builder,arena,coverage};
    CHECK(native_font_catalog_builder_initialize(&builder)&&arena_initialize(&arena,4*1024*1024,0,4096));
    u16 scratch_name[32768];wchar_t path[32768],family[32768];u32 files[256],units=0;
    for(u32 i=0;i<nfiles;++i){CHECK(reader.text(path,&units));const auto added=native_font_catalog_builder_add(&builder,path,{scratch_name,32768},files+i,65536,1000000,(flags&1)!=0);
        if(!added.ok()){print("add error=");number(static_cast<u32>(added.error));print(" status=");number(static_cast<u32>(added.font_status));print("\n");}CHECK(added.ok());}
    CHECK(reader.text(family,&units)&&units);const u32 family_units=units;wchar_t base_family[32768];u32 base_units=0;
    if(flags&0x200u)CHECK(reader.text(base_family,&base_units));Expected expected[1024]{};
    for(u32 i=0;i<nexpected;++i){auto& e=expected[i];CHECK(reader.integer(&e.file)&&e.file<nfiles&&reader.integer(&e.index)&&reader.integer(&e.simulations)&&reader.read(&e.properties,4)&&reader.integer(&e.axes)&&e.axes<=2);
        for(u32 j=0;j<e.axes;++j)CHECK(reader.integer(e.tags+j)&&reader.integer(e.bits+j));}
    const auto& catalog=builder.catalog;coverage.catalog=&catalog;const auto sizes=catalog.count;
    CHECK(native_font_session_create(&coverage.session)&&native_font_coverage_initialize(coverage.session,{coverage.slots,16},&coverage.cache));
    auto* props=arena_allocate<NativeFontProperties>(&arena,sizes.candidates);auto* indices=arena_allocate<u32>(&arena,sizes.candidates);auto* order=arena_allocate<u32>(&arena,sizes.candidates);CHECK(props&&indices&&order);
    auto* base_order=arena_allocate<u32>(&arena,sizes.candidates);CHECK(base_order);u32 queries=0;CHECK(reader.integer(&queries));char range_text[4096];
    for(u32 q=0;q<queries;++q){NativeFontProperties request{};u32 n=0,count=0;CHECK(reader.read(&request,4)&&reader.integer(&n));
        CHECK(native_font_catalog_fallback_order(catalog,{reinterpret_cast<const u16*>(family),family_units},request,{props,sizes.candidates},{indices,sizes.candidates},{order,sizes.candidates},&count)==SfntStatus::ok&&count);
        u32 base_count=0;if(base_units){const auto found=native_font_catalog_fallback_order(catalog,{reinterpret_cast<const u16*>(base_family),base_units},request,{props,sizes.candidates},{indices,sizes.candidates},{base_order,sizes.candidates},&base_count);
            CHECK(found==SfntStatus::ok||found==SfntStatus::unsupported_sequence);}
        u32 mode=0,prefix=0,suffix=0;if(ranges){CHECK(reader.integer(&mode)&&mode<=1);if(mode)CHECK(n==0x110000&&reader.integer(&prefix)&&reader.integer(&suffix));}
        for(u32 j=0;j<n;++j){u32 scalar=0,id=0,mapped=0,bytes=0;NativeFontCatalogMatch match{};SfntStatus status{};
            if(ranges){
                if(mode){scalar=j;CHECK(reader.integer(&id)&&reader.integer(&mapped));if(j>=0xd800&&j<=0xdfff){CHECK(id==native_fallback_missing&&!mapped);continue;}
                    if(prefix)bytes+=utf8_encode(prefix,range_text+bytes);bytes+=utf8_encode(j,range_text+bytes);if(suffix)bytes+=utf8_encode(suffix,range_text+bytes);}
                else{CHECK(reader.integer(&bytes)&&bytes&&bytes<=sizeof(range_text)&&reader.integer(&id)&&reader.integer(&mapped)&&reader.read(range_text,bytes));scalar=utf8_decode_one(range_text,bytes).codepoint;}
                CHECK(id<nexpected||id==native_fallback_missing);const u32 policy=(flags&0xffu)>>1;
                RangeContext context{&catalog,{order,count},request,&coverage,policy};NativeFallbackRangeResult result{};
                if(flags&0x200u){context.active=policy!=9&&policy!=6&&(policy!=4||scalar<0x80)&&(policy!=5||(route_policy(policy,scalar)&1));context.base={base_order,base_count};
                    status=native_font_fallback_map({range_text,bytes},base_units?range_base:nullptr,range_route,range_font,range_coverage,&context,&result);}
                else if(flags&0x100u){context.active=policy!=9&&policy!=6&&(policy!=4||scalar<0x80)&&(policy!=5||(route_policy(policy,scalar)&1));
                    status=native_font_fallback_routed_range({range_text,bytes},range_route,range_font,range_coverage,&context,&result);}
                else status=native_font_fallback_range({range_text,bytes},range_select,range_coverage,&context,&result);
                if(status!=SfntStatus::ok||result.units!=mapped){print("range bytes=");number(bytes);print(" units=");number(result.units);print(" expected=");number(mapped);print(" scalar=");number(scalar);print("\n");}
                u32 expected_bytes=0;CHECK(utf8_byte_offset_for_utf16_position({range_text,bytes},mapped,&expected_bytes));
                CHECK(status==SfntStatus::ok&&result.units==mapped&&result.bytes==expected_bytes);
                if(result.choice.font==native_fallback_missing)match={native_fallback_missing,{},0};
                else{CHECK(result.choice.font<catalog.count.candidates);NativeFontMatch selected{};
                    CHECK(native_font_match({&catalog.candidates[result.choice.font].properties,1},request,&selected)==SfntStatus::ok);
                    match={result.choice.font,selected.properties,selected.simulations};}++range_cases;
            }else{CHECK(reader.integer(&scalar)&&reader.integer(&id)&&id<nexpected);status=native_font_catalog_fallback_select(catalog,{order,count},request,scalar,actual_coverage,&coverage,&match);}
            if(id==native_fallback_missing){CHECK(status==SfntStatus::ok&&match.candidate==native_fallback_missing);++cases;continue;}
            if(status!=SfntStatus::ok||match.candidate==native_fallback_missing){print("scalar=");number(scalar);print(" status=");number(static_cast<u32>(status));print(" native_error=");number(coverage.failure.native_error);print("\n");}
            CHECK(status==SfntStatus::ok&&match.candidate!=native_fallback_missing);NativeFontCatalogEntry entry{};CHECK(native_font_catalog_entry(catalog,match.candidate,&entry).ok());const auto& e=expected[id];
            const bool same=entry.file==catalog.files.data+files[e.file]&&entry.face_index==e.index&&match.simulations==e.simulations&&bytes_equal(&match.properties,&e.properties,4)&&entry.settings.count==e.axes;
            if(!same){print("scalar=");number(scalar);print(" requested=");number(request.weight);print("/");number(request.style);print(" actual_candidate=");number(match.candidate);print(" weight=");number(match.properties.weight);print(" simulations=");number(match.simulations);print(" expected_weight=");number(e.properties.weight);print(" expected_simulations=");number(e.simulations);print("\n");}
            CHECK(same);for(u32 k=0;k<e.axes;++k){const float value=static_cast<float>(entry.settings[k].value);u32 bits=0;copy_bytes(&bits,&value,4);CHECK(entry.settings[k].tag==e.tags[k]&&bits==e.bits[k]);}++cases;
        }
    }CHECK(native_font_coverage_release(&coverage.cache)&&!native_font_session_stats(coverage.session).live.files&&native_font_session_release(&coverage.session));++groups;return true;
}
#include "font_fallback_policy_cases.inc"
bool fixture(const wchar_t* path) noexcept {File file;CHECK(file.open(path));Reader reader{file.data,file.size};u32 magic=0,n=0;CHECK(reader.integer(&magic)&&(magic==0x3146464d||magic==0x3152464d||magic==0x3146504d));
    if(magic==0x3146504d)CHECK(policy_fixture(reader));
    else{CHECK(reader.integer(&n));for(u32 i=0;i<n;++i)CHECK(group(reader,magic==0x3152464d));}
    CHECK(reader.at==reader.size);CHECK(!GetModuleHandleW(L"dwrite.dll")&&!GetModuleHandleW(L"DWriteCore.dll"));return true;}
bool argument(const wchar_t** command,wchar_t* out) noexcept {const wchar_t* p=*command;while(*p==' '||*p=='\t')++p;const bool quoted=*p=='"';if(quoted)++p;u32 n=0;
    while(*p&&!(quoted?*p=='"':*p==' '||*p=='\t')){if(n==32767)return false;out[n++]=*p++;}if(quoted){if(*p!='"')return false;++p;}out[n]=0;*command=p;return n!=0;}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_font_fallback_family_entry() noexcept {using namespace maicord;bool ok=guards();const wchar_t* command=GetCommandLineW();wchar_t path[32768];
    if(ok&&argument(&command,path)&&argument(&command,path))ok=fixture(path);print(ok?"PASS native fallback families groups=":"FAIL native fallback families groups=");number(groups);print(" cases=");number(cases);print(" checks=");number(checks);print(" probes=");number(probes);print(" ranges=");number(range_cases);print("\n");ExitProcess(ok?0:1);}
