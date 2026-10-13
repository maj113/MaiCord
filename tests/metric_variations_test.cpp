#include "maicord/text/metric_variations.hpp"
#include "maicord/text/font_axes.hpp"
#include "maicord/core/byte_order.hpp"
#include <Windows.h>
namespace maicord {namespace {
#include "generated/metric_variations_fixture.inc"
void print(Utf8View s) noexcept {DWORD n=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s.data,s.bytes,&n,nullptr);}
void number(i64 value) noexcept {if(value<0){print(utf8_literal("-"));value=-value;}u64 v=static_cast<u64>(value);char s[32];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n/2;++i){const char c=s[i];s[i]=s[n-i-1];s[n-i-1]=c;}print({s,n});}
struct Reader {ConstSlice<u8> bytes;u32 at=0;bool word(u32* v) noexcept {if(bytes.count-at<4)return false;*v=byte_load_le32(bytes.data+at);at+=4;return true;}bool take(u32 n,ConstSlice<u8>* v) noexcept {if(n>bytes.count-at)return false;*v={bytes.data+at,n};at+=n;return true;}};
u64 fonts=0,cases=0,advances_checked=0,decorations_checked=0,budgets=0,guards=0,malformed=0,raw_checks=0;
double hscalars[32767],mscalars[32767];u16 advances[65536];i32 decorations[4];
bool fail(u32 font,u32 code) noexcept {print(utf8_literal("FAIL font="));number(font);print(utf8_literal(" code="));number(code);print(utf8_literal("\n"));return false;}
// QA boundary corresponding to Windows design-unit fields. Production layout
// integration still decides when to use these integer fields versus raw deltas.
i32 nearest(double value) noexcept {const double shifted=value+.5;const i32 i=static_cast<i32>(shifted);return i-(i>shifted?1:0);}
bool resolve(const SfntFace& face,u32 weight,u32* glyph_count) noexcept {
    FontAxes axes{};FontAxisMappings mappings{};AxisSegmentMap maps[16];double coordinates[16];SfntHorizontalMetrics metrics{};ConstSlice<u8> htable{},mtable{},post{},os2{};SfntHvar hvar{};SfntMvar mvar{};
    if(font_axes_open(face,&axes)!=SfntStatus::ok || axes.axis_count>16 || font_axis_mappings_open(face,axes.axis_count,{maps,16},&mappings)!=SfntStatus::ok)return false;
    const FontAxisSetting setting{sfnt_tag('w','g','h','t'),static_cast<double>(weight)};
    if(font_axis_coordinates(axes,mappings,{&setting,1},{coordinates,16})!=SfntStatus::ok || sfnt_horizontal_metrics_open(face,&metrics)!=SfntStatus::ok || sfnt_table(face,sfnt_tag('H','V','A','R'),&htable)!=SfntStatus::ok || sfnt_hvar_open(htable,&hvar)!=SfntStatus::ok || sfnt_table(face,sfnt_tag('M','V','A','R'),&mtable)!=SfntStatus::ok || sfnt_mvar_open(mtable,&mvar)!=SfntStatus::ok)return false;
    VariationInstance hi{},mi{};
    if(variation_instance_prepare(hvar.store,{coordinates,axes.axis_count},{hscalars,32767},&hi)!=SfntStatus::ok)return false;
    if(mvar.record_count && variation_instance_prepare(mvar.store,{coordinates,axes.axis_count},{mscalars,32767},&mi)!=SfntStatus::ok)return false;
    *glyph_count=metrics.glyph_count;
    for(u32 i=0;i<metrics.glyph_count;++i)if(sfnt_hvar_design_advance(hvar,hi.scalars,metrics,static_cast<u16>(i),advances+i)!=SfntStatus::ok)return false;
    if(sfnt_table(face,sfnt_tag('p','o','s','t'),&post)!=SfntStatus::ok || post.count<12 || sfnt_table(face,sfnt_tag('O','S','/','2'),&os2)!=SfntStatus::ok || os2.count<30)return false;
    const i16 base[]{static_cast<i16>(byte_load_be16(post.data+8)),static_cast<i16>(byte_load_be16(post.data+10)),static_cast<i16>(byte_load_be16(os2.data+28)),static_cast<i16>(byte_load_be16(os2.data+26))};
    constexpr u32 tags[]{sfnt_tag('u','n','d','o'),sfnt_tag('u','n','d','s'),sfnt_tag('s','t','r','o'),sfnt_tag('s','t','r','s')};
    for(u32 i=0;i<4;++i){double delta=0;if(sfnt_mvar_delta(mvar,mi.scalars,tags[i],&delta)!=SfntStatus::ok || !(delta>=-2147000000 && delta<=2147000000))return false;const i32 value=nearest(base[i]+delta);decorations[i]=(i&1)?static_cast<u16>(value):static_cast<i16>(value);}
    return true;
}
bool boundaries(const SfntFace& face) noexcept {
    for(u32 kind=0;kind<2;++kind){ConstSlice<u8> table{};if(sfnt_table(face,kind?sfnt_tag('M','V','A','R'):sfnt_tag('H','V','A','R'),&table)!=SfntStatus::ok)return false;
        SfntHvar h{};SfntMvar m{};u32 full=0,used=0;const auto status=kind?sfnt_mvar_open(table,&m,1000000,&full):sfnt_hvar_open(table,&h,1000000,&full);if(status!=SfntStatus::ok)return false;
        for(u32 work=0;work<=full;++work){const auto s=kind?sfnt_mvar_open(table,&m,work,&used):sfnt_hvar_open(table,&h,work,&used);++budgets;if(used>work || s!=(work<full?SfntStatus::resource_limit:SfntStatus::ok))return false;}
        SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;if(table.count>page)return false;
        auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!memory)return false;DWORD old=0;if(!VirtualProtect(memory+page,page,PAGE_NOACCESS,&old)){VirtualFree(memory,0,MEM_RELEASE);return false;}
        for(u32 size=0;size<=table.count;++size){auto* start=memory+page-size;if(size)copy_bytes(start,table.data,size);const auto s=kind?sfnt_mvar_open({start,size},&m):sfnt_hvar_open({start,size},&h);++guards;if(size==table.count && s!=SfntStatus::ok){VirtualFree(memory,0,MEM_RELEASE);return false;}}
        VirtualFree(memory,0,MEM_RELEASE);
    }return true;
}
bool raw(const SfntFace& face,u32 mode) noexcept {
    ConstSlice<u8> ht{},mt{};SfntHvar h{};SfntMvar m{};const double scalar=.5;
    if(sfnt_table(face,sfnt_tag('H','V','A','R'),&ht)!=SfntStatus::ok || sfnt_table(face,sfnt_tag('M','V','A','R'),&mt)!=SfntStatus::ok || sfnt_hvar_open(ht,&h)!=SfntStatus::ok || sfnt_mvar_open(mt,&m)!=SfntStatus::ok)return false;
    for(u32 i=0;i<23;++i){const u32 index=mode==1?i%3:i;const i32 value=mode==3 || mode==4?static_cast<i32>(index%7)-3:static_cast<i32>(index*5+1)*(mode==2?-1:1);double delta=0;
        if(sfnt_hvar_advance_delta(h,{&scalar,1},static_cast<u16>(i),&delta)!=SfntStatus::ok || delta!=value*.5)return false;++raw_checks;}
    constexpr u32 tags[]{sfnt_tag('s','t','r','o'),sfnt_tag('s','t','r','s'),sfnt_tag('u','n','d','o'),sfnt_tag('u','n','d','s')};constexpr i32 normal[]{13,17,19,23},fraction[]{-3,-1,1,3};
    for(u32 i=0;i<4;++i){double delta=0;const double expected=mode==5?0:(mode==3 || mode==4?fraction[i]:normal[i]*(mode==2?-1:1))*.5;
        if(sfnt_mvar_delta(m,{&scalar,1},tags[i],&delta)!=SfntStatus::ok || delta!=expected)return false;++raw_checks;}
    double delta=1;if(sfnt_mvar_delta(m,{&scalar,1},0,&delta)!=SfntStatus::ok || delta!=0 || sfnt_mvar_delta(m,{&scalar,1},0xffffffff,&delta)!=SfntStatus::ok || delta!=0)return false;raw_checks+=2;
    // Corrupt only record indices/order and map formats, keeping valid store
    // bytes so these exercise the new wrappers rather than a truncated store.
    u8 changed[4096];if(ht.count>sizeof(changed) || mt.count>sizeof(changed))return false;
    copy_bytes(changed,ht.data,ht.count);const u32 map=byte_load_be32(changed+8);
    if(map){changed[map]=1;if(sfnt_hvar_open({changed,ht.count},&h)!=SfntStatus::unsupported)return false;++malformed;}
    if(m.record_count){copy_bytes(changed,mt.data,mt.count);copy_bytes(changed+20,changed+12,4);if(sfnt_mvar_open({changed,mt.count},&m)!=SfntStatus::malformed)return false;++malformed;
        copy_bytes(changed,mt.data,mt.count);changed[18]=0x7f;changed[19]=0xff;if(sfnt_mvar_open({changed,mt.count},&m)!=SfntStatus::ok || sfnt_mvar_delta(m,{&scalar,1},tags[0],&delta)!=SfntStatus::malformed)return false;++malformed;
        // Larger compatible value records: move each record and the unchanged
        // relative-addressed store, and verify actual delta resolution.
        const u32 old_store=byte_load_be16(mt.data+10),count=byte_load_be16(mt.data+8),new_store=12+count*12,new_size=new_store+mt.count-old_store;
        if(new_size>sizeof(changed))return false;copy_bytes(changed,mt.data,12);changed[7]=12;changed[10]=static_cast<u8>(new_store>>8);changed[11]=static_cast<u8>(new_store);
        for(u32 i=0;i<count;++i){copy_bytes(changed+12+i*12,mt.data+12+i*8,8);for(u32 j=0;j<4;++j)changed[20+i*12+j]=0xee;}
        copy_bytes(changed+new_store,mt.data+old_store,mt.count-old_store);
        if(sfnt_mvar_open({changed,new_size},&m)!=SfntStatus::ok || sfnt_mvar_delta(m,{&scalar,1},tags[3],&delta)!=SfntStatus::ok || delta!=(mode==3 || mode==4?3:23*(mode==2?-1:1))*.5)return false;++raw_checks;
    }
    return true;
}
bool corpus() noexcept {
    Reader r{{metric_variations_fixture,sizeof(metric_variations_fixture)},8};u32 count=0;if(!bytes_equal(r.bytes.data,"MCVMET1\0",8) || !r.word(&count))return false;
    for(u32 f=0;f<count;++f){u32 size=0,n=0;ConstSlice<u8> data{};SfntFace face{};if(!r.word(&size)||!r.word(&n)||!r.take(size,&data)||sfnt_open(data,0,&face)!=SfntStatus::ok)return fail(f,1);if(!boundaries(face))return fail(f,2);if(!raw(face,f))return fail(f,4);++fonts;
        for(u32 c=0;c<n;++c){u32 weight=0,glyphs=0,actual=0;if(!r.word(&weight)||!r.word(&glyphs)||!resolve(face,weight,&actual)||actual!=glyphs)return fail(f,3);
            for(u32 i=0;i<4;++i){u32 expected=0;if(!r.word(&expected)||static_cast<i32>(expected)!=decorations[i])return fail(f,10+i);++decorations_checked;}
            for(u32 i=0;i<glyphs;++i){u32 expected=0;if(!r.word(&expected)||expected!=advances[i])return fail(f,100+i);++advances_checked;}++cases;}
    }return r.at==r.bytes.count;
}
bool invalid() noexcept {
    u8 bytes[128]{};SfntHvar h{};SfntMvar m{};u32 used=99;
    if(sfnt_hvar_open({},nullptr,0,&used)!=SfntStatus::invalid_input || used)return false;++malformed;
    if(sfnt_mvar_open({},nullptr,0,&used)!=SfntStatus::invalid_input || used)return false;++malformed;
    bytes[1]=2;if(sfnt_hvar_open({bytes,20},&h)!=SfntStatus::unsupported || sfnt_mvar_open({bytes,12},&m)!=SfntStatus::unsupported)return false;malformed+=2;
    bytes[1]=1;if(sfnt_hvar_open({bytes,20},&h)!=SfntStatus::malformed || sfnt_mvar_open({bytes,12},&m)!=SfntStatus::ok)return false;malformed+=2;
    bytes[7]=8;if(sfnt_mvar_open({bytes,12},&m)!=SfntStatus::ok)return false;
    double d=1;if(sfnt_mvar_delta(m,{},sfnt_tag('s','t','r','s'),&d)!=SfntStatus::ok || d!=0)return false;++malformed;
    bytes[11]=12;if(sfnt_mvar_open({bytes,12},&m)!=SfntStatus::malformed)return false;++malformed;
    bytes[9]=1;if(sfnt_mvar_open({bytes,20},&m)!=SfntStatus::malformed)return false;++malformed;
    // Query output contracts and malformed variation-index references.
    if(sfnt_hvar_advance_delta({}, {}, 0, &d)!=SfntStatus::invalid_input || d!=0 || sfnt_hvar_advance_delta({}, {}, 0, nullptr)!=SfntStatus::invalid_input || sfnt_mvar_delta({}, {}, 0, nullptr)!=SfntStatus::invalid_input)return false;malformed+=3;
    return true;
}
bool argument(const wchar_t** command,wchar_t* out,u32 capacity) noexcept {auto* p=*command;while(*p==' '||*p=='\t')++p;const bool quoted=*p=='"';if(quoted)++p;u32 n=0;while(*p && !(quoted?*p=='"':*p==' '||*p=='\t')){if(n+1==capacity)return false;out[n++]=*p++;}if(quoted){if(*p!='"')return false;++p;}out[n]=0;*command=p;return n!=0;}
int file(const wchar_t* path) noexcept {
    HANDLE handle=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(handle==INVALID_HANDLE_VALUE)return 2;LARGE_INTEGER size{};if(!GetFileSizeEx(handle,&size)||size.QuadPart<=0||size.QuadPart>0xffffffffll){CloseHandle(handle);return 2;}
    HANDLE mapping=CreateFileMappingW(handle,nullptr,PAGE_READONLY,0,0,nullptr);auto* data=mapping?static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0)):nullptr;SfntFace face{};bool ok=data && sfnt_open({data,static_cast<u32>(size.QuadPart)},0,&face)==SfntStatus::ok;
    print(utf8_literal("["));for(u32 i=0;i<2 && ok;++i){u32 n=0;const u32 weight=i?600:400;ok=resolve(face,weight,&n);if(!ok)break;if(i)print(utf8_literal(","));print(utf8_literal("{\"weight\":"));number(weight);print(utf8_literal(",\"decorations\":["));for(u32 j=0;j<4;++j){if(j)print(utf8_literal(","));number(decorations[j]);}print(utf8_literal("],\"advances\":["));for(u32 j=0;j<n;++j){if(j)print(utf8_literal(","));number(advances[j]);}print(utf8_literal("]}"));}print(utf8_literal("]\n"));
    if(data)UnmapViewOfFile(data);if(mapping)CloseHandle(mapping);CloseHandle(handle);return ok?0:1;
}
int run() noexcept {const wchar_t* command=GetCommandLineW();wchar_t path[32768];if(!argument(&command,path,32768))return 2;while(*command==' '||*command=='\t')++command;if(*command){if(!argument(&command,path,32768))return 2;while(*command==' '||*command=='\t')++command;if(*command)return 2;return file(path);}
    const bool ok=corpus() && invalid();print(utf8_literal("fonts="));number(fonts);print(utf8_literal("\ncases="));number(cases);print(utf8_literal("\nadvances="));number(advances_checked);print(utf8_literal("\ndecorations="));number(decorations_checked);print(utf8_literal("\nraw_deltas="));number(raw_checks);print(utf8_literal("\nbudgets="));number(budgets);print(utf8_literal("\nguards="));number(guards);print(utf8_literal("\ninvalid="));number(malformed);print(ok?utf8_literal("\nPASS native metric variations\n"):utf8_literal("\nFAIL native metric variations\n"));return ok?0:1;}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_metric_variations_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
