#include "maicord/text/gsub_execute.hpp"
#include "maicord/core/byte_order.hpp"

#include "glyph_view_check.hpp"
#include "text_test_support.hpp"

namespace maicord {namespace {
volatile u32 observations=0;
void observe(SfntStatus s) noexcept {observations=observations+static_cast<u32>(s)+1;}
void put(u8* p,u32 at,u16 v) noexcept {p[at]=static_cast<u8>(v>>8);p[at+1]=static_cast<u8>(v);}
u32 words(u8* p,ConstSlice<u16> values) noexcept {for(u32 i=0;i<values.count;++i)put(p,i*2,values.data[i]);return values.count*2;}
u32 ligature_fixture(u8* p) noexcept {const u16 data[]{1,8,1,14,1,1,1,3,8,16,22,7,3,2,3,6,2,2,5,1};return words(p,{data,20});}
u32 lookup_fixture(u8* p,bool extension) noexcept {
    clear_bytes(p,80);put(p,0,1);put(p,4,10);put(p,6,10);put(p,8,12);put(p,12,1);put(p,14,4);put(p,16,extension?7:1);put(p,20,2);put(p,22,10);put(p,24,extension?30:22);
    for(u32 i=0;i<2;++i){u32 at=26+i*(extension?20:12);if(extension){put(p,at,1);put(p,at+2,1);byte_store_be32(p+at+4,8);at+=8;}put(p,at,1);put(p,at+2,6);put(p,at+4,static_cast<u16>(1+i));put(p,at+6,1);put(p,at+8,1);put(p,at+10,1);}
    return extension?66:50;
}
bool units() noexcept {
    u8 b[80]{};const u16 one[]{1,6,65535,1,2,0,2};u32 size=words(b,{one,7});SfntGsubSubtable sub{};SfntGsubMatch result{};u32 positions[8]{};u16 out=0;
    if(sfnt_gsub_subtable_open({b,size},1,65536,&sub)!=SfntStatus::ok)return false;
    SfntGsubGlyph input[]{{0,SfntGlyphClass::mark,0},{4,SfntGlyphClass::mark,0},{2,SfntGlyphClass::base,0},{3,SfntGlyphClass::base,0}};SfntLookupFilter filter{{},8};
    if(sfnt_gsub_subtable_match_view(sub,{},filter,sfnt_glyph_view({input,1}),0,0,{positions,8},&result)!=SfntStatus::ok || !result.matched || sfnt_gsub_replacement(result,0,&out)!=SfntStatus::ok || out!=65535)return false;
    if(sfnt_gsub_subtable_open({b,size},1,4,&sub)!=SfntStatus::malformed || sub.table.data)return false;
    // A compact coverage range validates modular output without expanding it.
    const u16 range[]{1,6,2,2,1,65534,65535,0};size=words(b,{range,8});input[0].glyph=65535;
    if(sfnt_gsub_subtable_open({b,size},1,65536,&sub)!=SfntStatus::ok || sfnt_gsub_subtable_match_view(sub,{},filter,sfnt_glyph_view({input,1}),0,0,{positions,8},&result)!=SfntStatus::ok || result.single!=1)return false;
    const u16 two[]{2,10,2,2,3,1,2,1,4};size=words(b,{two,9});input[0].glyph=4;
    if(sfnt_gsub_subtable_open({b,size},1,8,&sub)!=SfntStatus::ok || sfnt_gsub_subtable_match_view(sub,{},filter,sfnt_glyph_view({input,1}),0,0,{positions,8},&result)!=SfntStatus::ok || result.single!=3)return false;
    const u16 sequence[]{1,8,1,14,1,1,1,3,2,3,4};size=words(b,{sequence,11});input[0].glyph=1;
    if(sfnt_gsub_subtable_open({b,size},2,8,&sub)!=SfntStatus::ok || sfnt_gsub_subtable_match_view(sub,{},filter,sfnt_glyph_view({input,1}),0,0,{positions,8},&result)!=SfntStatus::ok || result.output_count!=3)return false;
    for(u16 i=0;i<3;++i)if(sfnt_gsub_replacement(result,i,&out)!=SfntStatus::ok || out!=i+2)return false;
    if(sfnt_gsub_subtable_open({b,size},3,8,&sub)!=SfntStatus::ok || sfnt_gsub_subtable_match_view(sub,{},filter,sfnt_glyph_view({input,1}),0,1,{positions,8},&result)!=SfntStatus::ok || result.single!=3)return false;
    if(sfnt_gsub_subtable_match_view(sub,{},filter,sfnt_glyph_view({input,1}),0,3,{positions,8},&result)!=SfntStatus::ok || result.matched)return false;
    put(b,14,0);if(sfnt_gsub_subtable_open({b,size},2,8,&sub)!=SfntStatus::malformed || sfnt_gsub_subtable_open({b,size},3,8,&sub)!=SfntStatus::ok || sfnt_gsub_subtable_match_view(sub,{},filter,sfnt_glyph_view({input,1}),0,0,{positions,8},&result)!=SfntStatus::ok || result.matched)return false;
    SfntGsubSubtable peers[2]{};u8 second[32];copy_bytes(second,b,size);put(second,14,1);put(second,16,5);
    if(sfnt_gsub_subtable_open({b,size},2,8,peers,1000000,nullptr,true)!=SfntStatus::ok||sfnt_gsub_subtable_match_view(peers[0],{},filter,sfnt_glyph_view({input,1}),0,0,{positions,8},&result)!=SfntStatus::ok||result.matched)return false;
    if(sfnt_gsub_subtable_open({second,size},2,8,peers+1)!=SfntStatus::ok)return false;
    if(sfnt_gsub_subtable_match_view(peers[1],{},filter,sfnt_glyph_view({input,1}),0,0,{positions,8},&result)!=SfntStatus::ok||!result.matched||result.output_count!=1||sfnt_gsub_replacement(result,0,&out)!=SfntStatus::ok||out!=5)return false;
    size=ligature_fixture(b);if(sfnt_gsub_subtable_open({b,size},4,8,&sub)!=SfntStatus::ok)return false;
    if(sfnt_gsub_subtable_match_view(sub,{},filter,sfnt_glyph_view({input,4}),0,0,{positions,8},&result)!=SfntStatus::ok || !result.matched || result.single!=7 || result.input_count!=3 || result.last_input!=3 || positions[0]!=0 || positions[1]!=2 || positions[2]!=3)return false;
    if(sfnt_gsub_subtable_match_view(sub,{},filter,sfnt_glyph_view({input,4}),0,0,{positions,2},&result)!=SfntStatus::output_too_small || result.matched || result.input_count!=3 || result.replacements.data)return false;
    if(sfnt_gsub_subtable_match_view(sub,{},filter,sfnt_glyph_view({input,4}),0,0,{positions,8},&result,2)!=SfntStatus::resource_limit || result.matched)return false;
    filter.flags=0;if(sfnt_gsub_subtable_match_view(sub,{},filter,sfnt_glyph_view({input,4}),0,0,{positions,8},&result)!=SfntStatus::ok || result.single!=5 || result.input_count!=1)return false;
    // Prefer the first matching record, even when a later record is longer.
    put(b,16,16);put(b,18,8);filter.flags=8;if(sfnt_gsub_subtable_open({b,size},4,8,&sub)!=SfntStatus::ok || sfnt_gsub_subtable_match_view(sub,{},filter,sfnt_glyph_view({input,4}),0,0,{positions,8},&result)!=SfntStatus::ok || result.single!=6 || result.input_count!=2 || result.last_input!=2)return false;
    size=ligature_fixture(b);if(sfnt_gsub_subtable_open({b,size},4,8,&sub)!=SfntStatus::ok)return false;
    u8 gd[36]{};put(gd,0,1);put(gd,2,2);put(gd,10,14);put(gd,12,22);put(gd,14,1);put(gd,16,4);put(gd,18,1);put(gd,20,2);put(gd,22,1);put(gd,24,1);byte_store_be32(gd+26,8);put(gd,30,1);put(gd,32,1);put(gd,34,3);SfntGdef gdef{};
    if(sfnt_gdef_open({gd,36},8,&gdef)!=SfntStatus::ok)return false;
    const u16 flags[]{0x100,0x200,0x210,0x208};const u16 wanted[]{7,5,7,7};
    for(u32 i=0;i<4;++i)if(sfnt_lookup_filter_prepare(gdef,flags[i],0,&filter)!=SfntStatus::ok || sfnt_gsub_subtable_match_view(sub,gdef,filter,sfnt_glyph_view({input,4}),0,0,{positions,8},&result)!=SfntStatus::ok || result.single!=wanted[i])return false;
    // Lookup preparation resolves extension subtables; the first subtable maps glyph 1.
    for(u32 extension=0;extension<2;++extension){size=lookup_fixture(b,extension!=0);SfntLayout layout{};SfntGsubExecutableSubtable storage[2];SfntGsubExecutableLookup lookup{};
        if(sfnt_layout_open({b,size},SfntLayoutKind::substitution,&layout)!=SfntStatus::ok || sfnt_gsub_prepare_lookup(layout,0,{},8,{{storage,1},{},{}},&lookup)!=SfntStatus::output_too_small || lookup.type)return false;
        if(sfnt_gsub_prepare_lookup(layout,0,{},8,{{storage,2},{},{}},&lookup)!=SfntStatus::ok || lookup.subtables.count!=2 || sfnt_gsub_subtable_match_view(lookup.subtables[0].basic,{},lookup.filter,sfnt_glyph_view({input,1}),0,0,{positions,8},&result)!=SfntStatus::ok || result.single!=2)return false;
        if(sfnt_gsub_subtable_match_view(lookup.subtables[0].basic,{},lookup.filter,sfnt_glyph_view({input,1}),0,0,{},&result)!=SfntStatus::output_too_small || result.input_count!=1)return false;
        // An identity substitution is still a match.
        put(b,extension?38:30,0);if(sfnt_layout_open({b,size},SfntLayoutKind::substitution,&layout)!=SfntStatus::ok || sfnt_gsub_prepare_lookup(layout,0,{},8,{{storage,2},{},{}},&lookup)!=SfntStatus::ok || sfnt_gsub_subtable_match_view(lookup.subtables[0].basic,{},lookup.filter,sfnt_glyph_view({input,1}),0,0,{positions,8},&result)!=SfntStatus::ok || !result.matched || result.single!=1)return false;
    }
    size=ligature_fixture(b);put(b,24,0);if(sfnt_gsub_subtable_open({b,size},4,8,&sub)!=SfntStatus::malformed)return false;
    size=ligature_fixture(b);put(b,28,8);if(sfnt_gsub_subtable_open({b,size},4,8,&sub)!=SfntStatus::malformed)return false;
    size=ligature_fixture(b);return sfnt_gsub_subtable_open({b,size},4,8,&sub,0)==SfntStatus::resource_limit && !sub.table.data;
}
bool prefix_units() noexcept {
    u8 bytes[80];const auto size=ligature_fixture(bytes);SfntGsubSubtable sub{};
    if(sfnt_gsub_subtable_open({bytes,size},4,8,&sub)!=SfntStatus::ok)return false;
    const auto check=[&](ConstSlice<SfntGsubGlyph> input,u8 mask,u16 glyph,u32 required,u32 last,u32 cost) noexcept {
        for(u32 capacity=0;capacity<=4;++capacity)for(u32 budget=0;budget<=cost+1;++budget){
            u32 positions[4]{0xffffffffu,0xffffffffu,0xffffffffu,0xffffffffu},used=0;SfntGsubMatch result{};
            const auto status=sfnt_gsub_subtable_match_view(sub,{},{{},8},sfnt_glyph_view(input),0,0,{positions,capacity},&result,budget,&used,mask);
            if(used!=minimum(budget,cost))return false;
            if(budget<cost){if(status!=SfntStatus::resource_limit||result.matched)return false;}
            else if(capacity<required){if(status!=SfntStatus::output_too_small||result.matched||result.input_count!=required)return false;}
            else if(status!=SfntStatus::ok||!result.matched||result.single!=glyph||result.input_count!=required||result.last_input!=last||positions[0]||(required>1&&positions[1]!=last))return false;
        }return true;
    };
    // Failed long rules share a valid filtered prefix with a later short rule.
    const SfntGsubGlyph mismatch[]{{1,SfntGlyphClass::base,0x10},{4,SfntGlyphClass::mark,0x10},{2,SfntGlyphClass::base,0x10},{4,SfntGlyphClass::base,0x10}};
    if(!check({mismatch,4},0,6,2,2,8))return false;
    const SfntGsubGlyph short_input[]{{1,SfntGlyphClass::base,0},{2,SfntGlyphClass::base,0}};
    if(!check({short_input,2},0,6,2,1,5))return false;
    // End-of-input and an ineligible ignored glyph stop the same filtered stream
    // for every candidate, but a later one-component rule can still match.
    const SfntGsubGlyph exhausted[]{{1,SfntGlyphClass::base,0},{4,SfntGlyphClass::mark,0},{5,SfntGlyphClass::mark,0}};
    if(!check({exhausted,3},0,5,1,0,8))return false;
    auto blocked=mismatch[1];blocked.reserved=0;const SfntGsubGlyph masked[]{mismatch[0],blocked,mismatch[2],mismatch[3]};
    if(!check({masked,4},0x10,5,1,0,6))return false;
    // An earlier rule must still detect bad input before considering a shorter
    // alternative. A leading one-component rule must not inspect that input.
    const SfntGsubGlyph invalid[]{mismatch[0],mismatch[1],mismatch[2],{8,SfntGlyphClass::base,0}};
    u32 positions[4],used=0;SfntGsubMatch result{};
    if(sfnt_gsub_subtable_match_view(sub,{},{{},8},sfnt_glyph_view(ConstSlice<SfntGsubGlyph>{invalid,4}),0,0,{positions,4},&result,100,&used,0)!=SfntStatus::invalid_input||used!=5||result.matched)return false;
    put(bytes,16,22);put(bytes,20,8);if(sfnt_gsub_subtable_open({bytes,size},4,8,&sub)!=SfntStatus::ok||!check({invalid,4},0,5,1,0,2))return false;
    return true;
}
bool guards() noexcept {
    Win32SystemInfo info{};GetSystemInfo(info.native());const u32 page=info.dwPageSize;auto* mem=static_cast<u8*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));if(!mem)return false;DWORD old=0;if(!VirtualProtect(mem+page,page,PAGE_NOACCESS,&old)){VirtualFree(mem,0,MEM_RELEASE);return false;}
    u8 seeds[3][80]{};const u16 single[]{2,10,2,2,3,1,2,1,4},sequence[]{1,8,1,14,1,1,1,3,2,3,4};
    const u32 sizes[]{words(seeds[0],{single,9}),words(seeds[1],{sequence,11}),ligature_fixture(seeds[2])};u32 random=0xf3629ee3;SfntGsubGlyph input[]{{1,SfntGlyphClass::base,0},{4,SfntGlyphClass::mark,0},{2,SfntGlyphClass::base,0},{3,SfntGlyphClass::base,0}};u32 positions[4];SfntLookupFilter filter{{},8};
    for(u32 i=0;i<20000;++i){random^=random<<13;random^=random>>17;random^=random<<5;const u32 seed=i%3,total=sizes[seed],size=i<123?minimum(i/3,total):random%(total+1);u8* b=mem+page-size;for(u32 j=0;j<size;++j)b[j]=seeds[seed][j];if(i>=123 && size)b[(random>>8)%size]^=static_cast<u8>(random>>16);
        for(u16 type=1;type<=4;++type){SfntGsubSubtable sub{};const auto status=sfnt_gsub_subtable_open({b,size},type,8,&sub,1000);observe(status);if(status==SfntStatus::ok){SfntGsubMatch result{};observe(sfnt_gsub_subtable_match_view(sub,{},filter,sfnt_glyph_view({input,4}),0,random%4,{positions,4},&result,100));if(result.matched)for(u16 k=0;k<result.output_count;++k){u16 glyph=0;observe(sfnt_gsub_replacement(result,k,&glyph));observations=observations+glyph;}}}
    }
    VirtualFree(mem,0,MEM_RELEASE);return true;
}
struct Reader {const u8* data;u32 size,at;
    bool word(u32* v) noexcept {if(size-at<4)return false;*v=byte_load_le32(data+at);at+=4;return true;}
    bool take(u32 n,const u8** v) noexcept {if(n>size-at)return false;*v=data+at;at+=n;return true;}
};
u32 mapping[65536];SfntGsubGlyph glyphs[65536];u32 consumed[65536];SfntGsubExecutableSubtable lookup_storage[65535];
bool corpus(const wchar_t* path) noexcept {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;Win32LargeInteger len{};
    if(!GetFileSizeEx(file,len.native()) || len.QuadPart<12 || len.QuadPart>0xffffffffll){CloseHandle(file);return false;}HANDLE map=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);if(!map){CloseHandle(file);return false;}
    auto* bytes=static_cast<const u8*>(MapViewOfFile(map,FILE_MAP_READ,0,0,0));Reader r{bytes,static_cast<u32>(len.QuadPart),8};u32 fonts=0,failures=0,type_counts[5]{};u64 queries=0,replacements=0,ligature_queries=0,prepared_lookups=0,coverage_checks=0;
    bool ok=bytes && bytes_equal(bytes,"MCGSUB1",8) && r.word(&fonts) && fonts>0;
    for(u32 font=0;font<fonts && ok;++font){u32 limit=0,size=0,gdef_size=0,count=0;const u8 *raw=nullptr,*gd=nullptr;ok=r.word(&limit)&&r.word(&size)&&r.word(&gdef_size)&&r.word(&count)&&limit>0&&limit<=65536&&count<=1000000&&r.take(size,&raw)&&r.take(gdef_size,&gd);if(!ok)break;
        SfntLayout layout{};SfntGdef gdef{};bool match=sfnt_layout_open({raw,size},SfntLayoutKind::substitution,&layout)==SfntStatus::ok && (!gdef_size || sfnt_gdef_open({gd,gdef_size},limit,&gdef)==SfntStatus::ok);if(!match && !count)++failures;u32 last_lookup=0xffffffff;SfntGsubExecutableLookup prepared{};
        for(u32 record=0;record<count && ok;++record){u32 type=0,offset=0,li=0,si=0,flags=0,markset=0,n=0;ok=r.word(&type)&&r.word(&offset)&&r.word(&li)&&r.word(&si)&&r.word(&flags)&&r.word(&markset)&&r.word(&n)&&type>=1&&type<=4&&offset<size&&li<=65535&&si<=65535&&flags<=65535&&markset<=65535&&n<=limit;if(!ok)break;
            for(u32 gid=0;gid<limit;++gid)mapping[gid]=0xffffffff;
            for(u32 i=0;i<n && ok;++i){u32 gid=0,outputs=0;ok=r.word(&gid)&&gid<limit&&mapping[gid]==0xffffffff;if(!ok)break;mapping[gid]=r.at;
                if(type<4){ok=r.word(&outputs)&&outputs<=65535&&(type!=1 || outputs==1)&&(type!=2 || outputs>0);const u8* values=nullptr;ok=ok&&r.take(outputs*4,&values);if(ok)for(u32 j=0;j<outputs;++j)if(byte_load_le32(values+j*4)>=limit)ok=false;}}
            SfntGsubSubtable sub{};const auto status=sfnt_gsub_subtable_open({raw+offset,size-offset},static_cast<u16>(type),limit,&sub);if(status!=SfntStatus::ok || sub.coverage.covered_count!=n)match=false;
            SfntLookupFilter filter{};if(sfnt_lookup_filter_prepare(gdef,static_cast<u16>(flags),static_cast<u16>(markset),&filter)!=SfntStatus::ok)match=false;
            if(li!=last_lookup){if(sfnt_gsub_prepare_lookup(layout,static_cast<u16>(li),gdef,limit,{{lookup_storage,65535},{},{}},&prepared)!=SfntStatus::ok)match=false;last_lookup=li;++prepared_lookups;}
            if(si>=prepared.subtables.count || prepared.subtables.data[si].basic.table.data!=raw+offset || prepared.subtables.data[si].basic.type!=type || prepared.filter.flags!=flags)match=false;
            if(status==SfntStatus::ok)for(u32 gid=0;gid<limit;++gid){const bool covered=mapping[gid]!=0xffffffff;if((sfnt_coverage_index(sub.coverage,static_cast<u16>(gid))!=sfnt_not_covered)!=covered)match=false;++coverage_checks;if(type==4 && covered)continue;const u32 outputs=covered?byte_load_le32(bytes+mapping[gid]):0;
                const u32 choices=type==3 && covered?outputs+1:1;glyphs[0]={static_cast<u16>(gid),SfntGlyphClass::unassigned,0};
                for(u32 choice=0;choice<choices;++choice){SfntGsubMatch result{};const bool expected_match=covered && (type!=3 || choice<outputs);const u32 expected_count=expected_match?(type==2?outputs:1):0;
                    if(qa_gsub_match(sub,gdef,filter,{glyphs,1},0,choice,{consumed,65536},&result)!=SfntStatus::ok || result.matched!=expected_match || result.output_count!=expected_count || result.input_count!=(expected_match?1u:0u) || (expected_match && (consumed[0]!=0 || result.last_input!=0)))match=false;
                    for(u32 j=0;j<expected_count;++j){u16 value=0;const u32 at=mapping[gid]+4+(type==3?choice:j)*4;if(sfnt_gsub_replacement(result,static_cast<u16>(j),&value)!=SfntStatus::ok || value!=byte_load_le32(bytes+at))match=false;++replacements;}++queries;
                }}
            u32 probes=0;ok=ok&&r.word(&probes)&&probes<=1000000;
            for(u32 i=0;i<probes && ok;++i){u32 start=0,input_count=0,consumed_count=0,output_count=0;ok=r.word(&start)&&r.word(&input_count)&&r.word(&consumed_count)&&r.word(&output_count)&&input_count>0&&input_count<=65536&&start<input_count&&consumed_count<=input_count&&output_count<=65535;if(!ok)break;
                for(u32 k=0;k<input_count && ok;++k){u32 gid=0,cl=0;ok=r.word(&gid)&&r.word(&cl)&&gid<limit&&cl<=4;glyphs[k]={static_cast<u16>(gid),static_cast<SfntGlyphClass>(cl),0};}
                const u8 *expected_positions=nullptr,*expected_glyphs=nullptr;ok=ok&&r.take(consumed_count*4,&expected_positions)&&r.take(output_count*4,&expected_glyphs);if(!ok)break;
                SfntGsubMatch result{};if(status!=SfntStatus::ok || qa_gsub_match(sub,gdef,filter,{glyphs,input_count},start,0,{consumed,65536},&result)!=SfntStatus::ok || result.matched!=(consumed_count!=0) || result.input_count!=consumed_count || result.output_count!=output_count)match=false;
                if(consumed_count && result.last_input!=byte_load_le32(expected_positions+(consumed_count-1)*4))match=false;
                for(u32 k=0;k<consumed_count;++k)if(consumed[k]!=byte_load_le32(expected_positions+k*4))match=false;
                for(u32 k=0;k<output_count;++k){u16 value=0;if(sfnt_gsub_replacement(result,static_cast<u16>(k),&value)!=SfntStatus::ok || value!=byte_load_le32(expected_glyphs+k*4))match=false;++replacements;}++ligature_queries;
            }
            ++type_counts[type];if(!match){++failures;print(utf8_literal("mismatch_font="));number(font);print(utf8_literal(" lookup="));number(li);print(utf8_literal(" subtable="));number(si);print(utf8_literal(" status="));number(static_cast<u32>(status));print(utf8_literal("\n"));match=true;}
        }
    }
    ok=ok&&r.at==r.size;print(utf8_literal("fonts="));number(fonts);for(u32 t=1;t<=4;++t){print(utf8_literal("\ntype_"));number(t);print(utf8_literal("="));number(type_counts[t]);}print(utf8_literal("\nprepared_lookups="));number(prepared_lookups);print(utf8_literal("\ncoverage_checks="));number(coverage_checks);print(utf8_literal("\nsingle_glyph_queries="));number(queries);print(utf8_literal("\nligature_probes="));number(ligature_queries);print(utf8_literal("\nreplacement_checks="));number(replacements);print(utf8_literal("\nfailures="));number(failures);print(utf8_literal("\ncomplete="));number(ok);print(utf8_literal("\n"));
    print(utf8_literal("gap_view_queries="));number(qa_gap_queries);print(utf8_literal("\n"));
    if(bytes)UnmapViewOfFile(bytes);CloseHandle(map);CloseHandle(file);return ok&&!failures;
}
int run() noexcept {
    if(!units()){print(utf8_literal("FAIL GSUB units\n"));return 1;}if(!prefix_units()){print(utf8_literal("FAIL GSUB prefix matching\n"));return 5;}if(!guards()){print(utf8_literal("FAIL GSUB guards\n"));return 2;}
    const wchar_t* cmd=test_arguments();
    if(*cmd){wchar_t path[32768];if(!test_path_argument(cmd,path))return 3;if(!corpus(path))return 4;}
    print(utf8_literal("PASS native GSUB basic matching\n"));return 0;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_gsub_tests_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
