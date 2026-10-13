#include "maicord/text/gpos.hpp"
#include "maicord/core/byte_order.hpp"
#include <Windows.h>
namespace maicord {namespace {
#include "generated/gpos_fixture.inc"
void print(Utf8View s) noexcept {DWORD n=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s.data,s.bytes,&n,nullptr);}
void number(u64 v) noexcept {char s[32];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n/2;++i){const char c=s[i];s[i]=s[n-i-1];s[n-i-1]=c;}print({s,n});}
void put(u8* p,u32 at,u16 v) noexcept {p[at]=static_cast<u8>(v>>8);p[at+1]=static_cast<u8>(v);}
u64 value_checks=0,device_samples=0,anchor_checks=0,queries=0,gap_queries=0;u32 failures=0;
volatile u32 observations=0;
bool units() noexcept {
    u8 store_bytes[32]{};put(store_bytes,0,1);byte_store_be32(store_bytes+2,12);put(store_bytes,6,1);byte_store_be32(store_bytes+8,22);put(store_bytes,12,1);put(store_bytes,14,1);put(store_bytes,18,16384);put(store_bytes,20,16384);put(store_bytes,22,1);put(store_bytes,24,1);put(store_bytes,26,1);put(store_bytes,30,40);
    ItemVariationStore store{};VariationInstance instance{};double coordinate=.5,scalar=0;if(item_variation_store_open({store_bytes,32},0,&store)!=SfntStatus::ok || variation_instance_prepare(store,{&coordinate,1},{&scalar,1},&instance)!=SfntStatus::ok)return false;
    const i16 design[]{-32768,32767,-1,17},pixels[]{-2,7,-5,0};
    for(u16 mask=0;mask<256;++mask){u8 p[128]{};u32 at=16,end=16;for(u32 bit=0;bit<8;++bit)if(mask&(1u<<bit))end+=2;u32 child=end;
        for(u32 bit=0;bit<8;++bit)if(mask&(1u<<bit)){if(bit<4)put(p,at,static_cast<u16>(design[bit]));else {put(p,at,static_cast<u16>(child));const u32 d=bit-4;
                if(d==3){put(p,child+4,0x8000);child+=6;}else {put(p,child,d==1?9:16);put(p,child+2,d==1?9:16);put(p,child+4,static_cast<u16>(d+1));put(p,child+6,d==0?0x8000:d==1?0x7000:0xfb00);child+=8;}}at+=2;}
        SfntValueRecord record{};SfntPositionValue value{};u32 used=0;if(sfnt_position_value_size(mask)!=end-16 || sfnt_value_record_open({p,child},16,mask,end,&record,100,&used)!=SfntStatus::ok || !used || sfnt_value_record_read(record,&value)!=SfntStatus::ok)return false;
        for(u32 i=0;i<4;++i)if(value.design[i]!=(mask&(1u<<i)?design[i]:0) || value.devices[i].format!=(mask&(16u<<i)?(i==3?0x8000:i+1):0))return false;
        SfntPositionAdjustment adjusted{};if(sfnt_position_value_resolve(value,{nullptr,16,9,false,VariationArithmetic::wide},&adjusted)!=SfntStatus::ok)return false;
        for(u32 i=0;i<4;++i)if(adjusted.design[i]!=value.design[i] || adjusted.pixels[i]!=(mask&(16u<<i)?pixels[i]:0))return false;
        if(sfnt_position_value_resolve(value,{&instance,16,9,true,VariationArithmetic::wide},&adjusted)!=SfntStatus::ok)return false;
        for(u32 i=0;i<4;++i)if(adjusted.pixels[i] || adjusted.design[i]!=value.design[i]+((i==3 && (mask&128))?20:0))return false;
        if(sfnt_position_value_resolve(value,{nullptr,16,9,true,VariationArithmetic::wide},&adjusted)!=((mask&128)?SfntStatus::missing_table:SfntStatus::ok))return false;
        if(sfnt_value_record_open({p,child},16,mask,end,&record,0)!=SfntStatus::resource_limit || record.parent.data)return false;
    }
    if(sfnt_position_value_size(256)!=0xffff)return false;
    SfntPositionValue sentinel{};sentinel.design[0]=23;sentinel.devices[0]={{},65535,65535,0x8000};SfntPositionAdjustment no_delta{};
    if(sfnt_position_value_resolve(sentinel,{nullptr,16,16,true,VariationArithmetic::wide},&no_delta)!=SfntStatus::ok || no_delta.design[0]!=23 || no_delta.pixels[0])return false;
    // The 4.29-billion-cell empty class matrix has zero payload and must not be walked.
    u8 empty[30]{};put(empty,0,2);put(empty,2,16);put(empty,8,22);put(empty,10,26);put(empty,12,65535);put(empty,14,65535);put(empty,16,1);put(empty,18,1);put(empty,20,1);put(empty,22,2);put(empty,26,2);
    SfntGposSubtable matrix{};u32 used=0;if(sfnt_gpos_subtable_open({empty,30},2,5,&matrix,10,&used)!=SfntStatus::ok || used>10)return false;
    SfntGsubGlyph glyphs[]{{1,SfntGlyphClass::base,0},{3,SfntGlyphClass::mark,0},{2,SfntGlyphClass::base,0}};SfntGposMatch match{};
    if(sfnt_gpos_subtable_match_view(matrix,{},{{},8},sfnt_glyph_view({glyphs,3}),0,3,&match)!=SfntStatus::ok || !match.matched || match.second_input!=2 || match.next_input!=2 || match.values[0].bytes || match.values[1].bytes)return false;
    // A present zero second value consumes the pair; absent second value does not.
    for(u32 present=0;present<2;++present){u8 p[26]{};const u16 cov=present?20:18;put(p,0,1);put(p,2,cov);put(p,4,4);put(p,6,present?4:0);put(p,8,1);put(p,10,12);put(p,12,1);put(p,14,2);put(p,16,65526);put(p,cov,1);put(p,cov+2,1);put(p,cov+4,1);
        SfntGposSubtable sub{};if(sfnt_gpos_subtable_open({p,static_cast<u32>(cov)+6},2,5,&sub)!=SfntStatus::ok || sfnt_gpos_subtable_match_view(sub,{},{{},8},sfnt_glyph_view({glyphs,3}),0,3,&match)!=SfntStatus::ok || !match.matched || match.second_input!=2 || match.next_input!=2+present)return false;
        if(sfnt_gpos_subtable_match_view(sub,{},{{},8},sfnt_glyph_view({glyphs,3}),0,2,&match)!=SfntStatus::ok || match.matched)return false;}
    // A matching empty single adjustment stops the lookup before a later adjustment.
    u8 zero[]{0,1,0,6,0,0,0,1,0,1,0,1},nonzero[]{0,1,0,8,0,4,0xff,0xe2,0,1,0,1,0,1};SfntGposSubtable sequence[2];
    if(sfnt_gpos_subtable_open({zero,sizeof(zero)},1,5,&sequence[0])!=SfntStatus::ok || sfnt_gpos_subtable_open({nonzero,sizeof(nonzero)},1,5,&sequence[1])!=SfntStatus::ok || sfnt_gpos_lookup_match_view({{sequence,2},{}},{},sfnt_glyph_view({glyphs,3}),0,3,&match)!=SfntStatus::ok || !match.matched || match.values[0].format)return false;
    for(u16 format=1;format<=3;++format){u8 a[24]{};put(a,0,format);put(a,2,65506);put(a,4,100);put(a,6,format==2?65535:10);if(format==3){put(a,10,16);put(a,12,16);put(a,14,1);put(a,16,0x4000);}
        SfntAnchor anchor{};SfntAnchorValue value{};if(sfnt_anchor_open({a,format==1?6u:format==2?8u:18u},&anchor)!=SfntStatus::ok || sfnt_anchor_read(anchor,&value)!=SfntStatus::ok || value.format!=format || value.x!=-30 || value.y!=100 || value.point!=(format==2?65535:0) || value.devices[0].format!=(format==3?1:0))return false;}
    return true;
}
struct Reader {ConstSlice<u8> data;u32 at=0;
    bool word(u32* v) noexcept {if(data.count-at<4)return false;*v=byte_load_le32(data.data+at);at+=4;return true;}
    bool half(u16* v) noexcept {if(data.count-at<2)return false;*v=static_cast<u16>(data.data[at]|(static_cast<u16>(data.data[at+1])<<8));at+=2;return true;}
    bool take(u32 n,ConstSlice<u8>* v) noexcept {if(n>data.count-at)return false;*v={data.data+at,n};at+=n;return true;}
};
bool check_device(Reader& r,const SfntDevice& actual) noexcept {
    u16 format=0,start=0,end=0;u32 count=0;if(!r.half(&format))return false;if(!format)return actual.format==0;
    if(!r.half(&start)||!r.half(&end)||!r.word(&count)||format!=actual.format||start!=actual.start_or_outer||end!=actual.end_or_inner)return false;
    if(format==0x8000){u32 index=0;return !count && sfnt_device_variation_index(actual,&index) && index==((static_cast<u32>(start)<<16)|end);}
    if(format>3 || start>end || count!=static_cast<u32>(end)-start+1)return false;
    for(u32 i=0;i<count;++i){u16 expected=0;i16 delta=0;if(!r.half(&expected)||sfnt_device_pixels(actual,static_cast<u16>(start+i),&delta)!=SfntStatus::ok||delta!=static_cast<i16>(expected))return false;++device_samples;}
    i16 delta=1;if(start && (sfnt_device_pixels(actual,start-1,&delta)!=SfntStatus::ok || delta))return false;if(end<65535 && (sfnt_device_pixels(actual,end+1,&delta)!=SfntStatus::ok || delta))return false;return true;
}
bool check_value(Reader& r,const SfntPositionValue& actual) noexcept {
    for(u32 i=0;i<4;++i){u16 expected=0;if(!r.half(&expected) || actual.design[i]!=static_cast<i16>(expected))return false;}
    for(u32 i=0;i<4;++i)if(!check_device(r,actual.devices[i]))return false;++value_checks;return true;
}
const u8* value_records[1000000];SfntGposSubtable subtables[1024];SfntGsubGlyph input[8];
struct Cell {SfntGsubGlyph glyph;u32 poison[3];};Cell cells[16];
bool same(const SfntGposMatch& a,const SfntGposMatch& b) noexcept {
    if(a.matched!=b.matched || a.first_input!=b.first_input || a.second_input!=b.second_input || a.next_input!=b.next_input)return false;
    for(u32 i=0;i<2;++i)if(a.values[i].parent.data!=b.values[i].parent.data || a.values[i].parent.count!=b.values[i].parent.count || a.values[i].offset!=b.values[i].offset || a.values[i].format!=b.values[i].format || a.values[i].bytes!=b.values[i].bytes)return false;return true;
}
bool coverage_bound_units() noexcept {
    const u16 ends[]{0,12,60000,65535};
    for(u16 format=1;format<=2;++format)for(u16 last:ends)for(u32 empty=0;empty<2;++empty){
        u8 bytes[24]{};put(bytes,0,1);put(bytes,2,8);put(bytes,4,4);put(bytes,6,20);put(bytes,8,format);
        const u16 count=empty?0:format==1&&last?2:1;put(bytes,10,count);
        if(format==1&&count==2)put(bytes,14,last);else if(format==2)put(bytes,14,last);
        SfntGposSubtable bounded{};
        if(sfnt_gpos_subtable_open({bytes,static_cast<u32>(12+count*(format==1?2:6))},1,65536,&bounded)!=SfntStatus::ok)return false;
        auto scanned=bounded;scanned.last_covered_glyph=65535;
        const u16 probes[]{0,1,11,12,13,59999,60000,60001,65534,65535};
        for(u16 glyph:probes)for(u32 budget=0;budget<=1;++budget){SfntGsubGlyph probe{glyph,SfntGlyphClass::base,0};SfntGposMatch a{},b{};u32 a_work=0,b_work=0;
            const auto x=sfnt_gpos_subtable_match_view(bounded,{},{},sfnt_glyph_view({&probe,1}),0,1,&a,budget,&a_work);
            const auto y=sfnt_gpos_subtable_match_view(scanned,{},{},sfnt_glyph_view({&probe,1}),0,1,&b,budget,&b_work);
            if(x!=y||a_work!=b_work||!same(a,b))return false;
        }
    }
    return true;
}
bool corpus(ConstSlice<u8> bytes) noexcept {
    if(bytes.count<12 || !bytes_equal(bytes.data,"MCGPOS1\0",8))return false;Reader r{bytes,8};u32 tables=0;if(!r.word(&tables) || tables>10000)return false;
    for(u32 table=0;table<tables;++table){u32 limit=0,size=0,gdsize=0,vc=0,ac=0,sc=0;ConstSlice<u8> raw{},gd{};
        if(!r.word(&limit)||!r.word(&size)||!r.word(&gdsize)||!r.word(&vc)||!r.word(&ac)||!r.word(&sc)||!limit||limit>65536||vc>1000000||ac>1000000||sc>100000||!r.take(size,&raw)||!r.take(gdsize,&gd))return false;
        SfntLayout layout{};SfntGdef gdef{};if(sfnt_layout_open(raw,SfntLayoutKind::positioning,&layout)!=SfntStatus::ok || (gdsize && sfnt_gdef_open(gd,limit,&gdef)!=SfntStatus::ok))return false;
        for(u32 i=0;i<vc;++i){value_records[i]=r.data.data+r.at;u32 parent=0,offset=0,format=0,minimum=0;
            if(!r.word(&parent)||!r.word(&offset)||!r.word(&format)||!r.word(&minimum)||parent>size||format>65535)return false;
            SfntValueRecord record{};SfntPositionValue value{};const auto status=sfnt_value_record_open({raw.data+parent,size-parent},offset,static_cast<u16>(format),minimum,&record);
            if(status!=SfntStatus::ok || sfnt_value_record_read(record,&value)!=SfntStatus::ok || !check_value(r,value)){print(utf8_literal("value_table="));number(table);print(utf8_literal(" index="));number(i);print(utf8_literal(" status="));number(static_cast<u32>(status));print(utf8_literal("\n"));return false;}}
        for(u32 i=0;i<ac;++i){u32 at=0;u16 format=0,x=0,y=0,point=0;if(!r.word(&at)||!r.half(&format)||!r.half(&x)||!r.half(&y)||!r.half(&point)||at>=size)return false;
            SfntAnchor anchor{};SfntAnchorValue value{};if(sfnt_anchor_open({raw.data+at,size-at},&anchor)!=SfntStatus::ok || sfnt_anchor_read(anchor,&value)!=SfntStatus::ok || value.format!=format || value.x!=static_cast<i16>(x) || value.y!=static_cast<i16>(y) || value.point!=point || !check_device(r,value.devices[0]) || !check_device(r,value.devices[1]))return false;++anchor_checks;}
        u32 previous=0xffffffffu;SfntGposLookup prepared{};
        for(u32 i=0;i<sc;++i){u32 li=0,si=0,offset=0,type=0,format=0,qc=0;if(!r.word(&li)||!r.word(&si)||!r.word(&offset)||!r.word(&type)||!r.word(&format)||!r.word(&qc)||li>65535||qc>10000000)return false;
            if(previous!=li){const auto s=sfnt_gpos_lookup_prepare(layout,static_cast<u16>(li),gdef,limit,{subtables,1024},&prepared);if(s!=SfntStatus::ok){print(utf8_literal("prepare_table="));number(table);print(utf8_literal(" lookup="));number(li);print(utf8_literal(" status="));number(static_cast<u32>(s));print(utf8_literal("\n"));return false;}previous=li;}
            if(si>=prepared.subtables.count)return false;const auto& sub=prepared.subtables.data[si];if(sub.table.data!=raw.data+offset || sub.type!=type || sub.format!=format)return false;
            for(u32 q=0;q<qc;++q){u32 n=0,start=0,end=0,matched=0,second=0,next=0,refs[2]{};if(!r.word(&n)||!r.word(&start)||!r.word(&end)||!r.word(&matched)||!r.word(&second)||!r.word(&next)||!r.word(&refs[0])||!r.word(&refs[1])||!n||n>8||start>=end||end>n||matched>1)return false;
                ConstSlice<u8> record{};if(!r.take(n*4,&record))return false;copy_bytes(input,record.data,record.count);SfntGposMatch result{};
                const auto status=sfnt_gpos_subtable_match_view(sub,gdef,prepared.filter,sfnt_glyph_view({input,n}),start,end,&result);bool pass=status==SfntStatus::ok && result.matched==(matched!=0);
                if(matched){if(result.first_input!=start || result.second_input!=second || result.next_input!=next)pass=false;
                    for(u32 v=0;v<2;++v){SfntPositionValue value{};if(sfnt_value_record_read(result.values[v],&value)!=SfntStatus::ok)return false;
                        if(refs[v]==0xffffffffu){for(u32 k=0;k<4;++k)if(value.design[k] || value.devices[k].format)pass=false;}
                        else {if(refs[v]>=vc)return false;const u8* expected=value_records[refs[v]];const u32 parent=byte_load_le32(expected),at=byte_load_le32(expected+4),mask=byte_load_le32(expected+8);
                            if(result.values[v].parent.data!=raw.data+parent || result.values[v].offset!=at || result.values[v].format!=mask)pass=false;
                            Reader value_reader{bytes,static_cast<u32>(expected+16-bytes.data)};if(!check_value(value_reader,value))pass=false;}}
                    if(!si){SfntGposMatch whole{};if(sfnt_gpos_lookup_match_view(prepared,gdef,sfnt_glyph_view({input,n}),start,end,&whole)!=SfntStatus::ok || !same(whole,result))pass=false;}}
                for(u32 split=0;split<=n;++split){for(u32 k=0;k<16;++k)cells[k]={{65535,static_cast<SfntGlyphClass>(255),255},{0xdddddddd,0xaaaaaaaa,0xeeeeeeee}};
                    for(u32 k=0;k<n;++k)cells[k<split?k:k+3].glyph=input[k];SfntGposMatch gap{};SfntGlyphView view{&cells[0].glyph,&cells[split+3].glyph,split,n-split,sizeof(Cell)};
                    if(sfnt_gpos_subtable_match_view(sub,gdef,prepared.filter,view,start,end,&gap)!=SfntStatus::ok || !same(result,gap))pass=false;++gap_queries;}
                ++queries;if(!pass){++failures;if(failures<10){print(utf8_literal("query_table="));number(table);print(utf8_literal(" sub="));number(i);print(utf8_literal(" query="));number(q);print(utf8_literal(" status="));number(static_cast<u32>(status));print(utf8_literal("\n"));}}
            }
        }
    }return r.at==r.data.count;
}
bool guards() noexcept {
    const u32 size=byte_load_le32(gpos_fixture+16);const u8* raw=gpos_fixture+36;SfntLayout layout{};if(sfnt_layout_open({raw,size},SfntLayoutKind::positioning,&layout)!=SfntStatus::ok)return false;
    ConstSlice<u8> originals[4];u16 types[4];for(u16 i=0;i<4;++i){SfntLookup lookup{};if(sfnt_layout_lookup(layout,i,&lookup)!=SfntStatus::ok || sfnt_lookup_subtable(lookup,0,&originals[i])!=SfntStatus::ok)return false;types[i]=lookup.type;}
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;auto* mem=static_cast<u8*>(VirtualAlloc(nullptr,page*2,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!mem)return false;DWORD old=0;if(!VirtualProtect(mem+page,page,PAGE_NOACCESS,&old)){VirtualFree(mem,0,MEM_RELEASE);return false;}
    SfntGsubGlyph glyphs[]{{1,SfntGlyphClass::base,0},{3,SfntGlyphClass::mark,0},{2,SfntGlyphClass::base,0}};u32 random=0x317d9821;bool ok=true;
    for(u32 i=0;i<10000;++i){const auto original=originals[i%4];if(original.count>page){ok=false;break;}random^=random<<13;random^=random>>17;random^=random<<5;const u32 length=i<4?original.count:random%(original.count+1);u8* data=mem+page-length;copy_bytes(data,original.data,length);if(i>=4 && length)data[(random>>8)%length]^=static_cast<u8>(random>>16);
        SfntGposSubtable sub{};auto s=sfnt_gpos_subtable_open({data,length},types[i%4],5,&sub,1000);observations=observations+static_cast<u32>(s)+1;
        if(s==SfntStatus::ok){SfntGposMatch m{};s=sfnt_gpos_subtable_match_view(sub,{},{{},8},sfnt_glyph_view({glyphs,3}),0,3,&m,1000);observations=observations+static_cast<u32>(s)+m.next_input;
            if(s==SfntStatus::ok && m.matched)for(u32 j=0;j<2;++j){SfntPositionValue v{};s=sfnt_value_record_read(m.values[j],&v);observations=observations+static_cast<u32>(s)+static_cast<u16>(v.design[0]);}}
        SfntAnchor anchor{};s=sfnt_anchor_open({data,length},&anchor,1000);observations=observations+static_cast<u32>(s)+1;if(s==SfntStatus::ok){SfntAnchorValue a{};s=sfnt_anchor_read(anchor,&a);observations=observations+static_cast<u32>(s)+a.point;}
    }
    VirtualFree(mem,0,MEM_RELEASE);return ok;
}
bool file_corpus(const wchar_t* path) noexcept {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;LARGE_INTEGER size{};if(!GetFileSizeEx(file,&size) || size.QuadPart<12 || size.QuadPart>0xffffffffll){CloseHandle(file);return false;}
    HANDLE map=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);if(!map){CloseHandle(file);return false;}const auto* data=static_cast<const u8*>(MapViewOfFile(map,FILE_MAP_READ,0,0,0));const bool ok=data && corpus({data,static_cast<u32>(size.QuadPart)});if(data)UnmapViewOfFile(data);CloseHandle(map);CloseHandle(file);return ok;
}
int run() noexcept {
    if(!units()||!coverage_bound_units()){print(utf8_literal("FAIL GPOS units\n"));return 3;}if(!guards()){print(utf8_literal("FAIL GPOS guards\n"));return 4;}
    const wchar_t* command=GetCommandLineW();bool quote=false;while(*command){if(*command=='"')quote=!quote;else if(!quote && (*command==' ' || *command=='\t'))break;++command;}while(*command==' ' || *command=='\t')++command;
    bool ok=false;if(!*command)ok=corpus({gpos_fixture,sizeof(gpos_fixture)});
    else {wchar_t path[32768];u32 n=0;quote=*command=='"';if(quote)++command;while(*command && !(quote?*command=='"':(*command==' ' || *command=='\t'))){if(n+1>=32768)return 2;path[n++]=*command++;}if(quote){if(*command!='"')return 2;++command;}while(*command==' ' || *command=='\t')++command;if(*command || !n)return 2;path[n]=0;ok=file_corpus(path);}
    print(utf8_literal("value_checks="));number(value_checks);print(utf8_literal("\ndevice_samples="));number(device_samples);print(utf8_literal("\nanchor_checks="));number(anchor_checks);print(utf8_literal("\nqueries="));number(queries);print(utf8_literal("\ngap_queries="));number(gap_queries);print(utf8_literal("\nguard_observations="));number(observations);print(utf8_literal("\nfailures="));number(failures);print(utf8_literal("\n"));print(ok && !failures?utf8_literal("PASS native GPOS basic matching\n"):utf8_literal("FAIL native GPOS basic matching\n"));return ok && !failures?0:1;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_gpos_tests_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
