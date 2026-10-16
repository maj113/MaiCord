#include "maicord/text/gpos_execute.hpp"
#include "maicord/core/byte_order.hpp"
#include <Windows.h>
#ifdef MAICORD_MARK_POSITION_TESTS
#include "maicord/text/gpos_mark_execute.hpp"
#endif
#ifdef MAICORD_CURSIVE_POSITION_TESTS
#include "maicord/text/gpos_cursive_execute.hpp"
#endif
#ifdef MAICORD_LIGATURE_POSITION_TESTS
#include "maicord/text/gpos_ligature_execute.hpp"
#endif
namespace maicord {namespace {
#ifdef MAICORD_LIGATURE_POSITION_TESTS
u64 ligature_matches=0,ligature_scan_steps=0,ligature_prefix_queries=0,ligature_ordered_steps=0,ligature_missing_components=0,ligature_compatibility_anchors=0;
bool ligature_lookup[8];
#endif
#ifdef MAICORD_MARK_POSITION_TESTS
#ifdef MAICORD_CURSIVE_POSITION_TESTS
#ifdef MAICORD_LIGATURE_POSITION_TESTS
#include "generated/gpos_ligature_execute_fixture.inc"
#else
#include "generated/gpos_cursive_execute_fixture.inc"
#endif
u64 cursive_matches=0,cursive_steps=0,cursive_words=0;bool cursive_lookup[8];
#else
#include "generated/gpos_mark_execute_fixture.inc"
#endif
u64 mark_matches=0,target_steps=0,compatibility_anchors=0;
SfntGposAttachment mark_storage[8][32];SfntGposAttachmentLookup mark_lookups[8];bool mark_lookup[8];
#else
#include "generated/gpos_execute_fixture.inc"
#endif
void print(Utf8View s) noexcept {DWORD n=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s.data,s.bytes,&n,nullptr);}
void number(u64 v) noexcept {char s[32];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n/2;++i){const char c=s[i];s[i]=s[n-i-1];s[n-i-1]=c;}print({s,n});}
u64 configurations=0,executions=0,float_checks=0,budget_checks=0;u32 failures=0;
struct Reader {ConstSlice<u8> data;u32 at=0;
    bool word(u32* v) noexcept {if(data.count-at<4)return false;*v=byte_load_le32(data.data+at);at+=4;return true;}
    bool half(u16* v) noexcept {if(data.count-at<2)return false;*v=static_cast<u16>(data.data[at]|(static_cast<u16>(data.data[at+1])<<8));at+=2;return true;}
    bool take(u32 n,ConstSlice<u8>* v) noexcept {if(n>data.count-at)return false;*v={data.data+at,n};at+=n;return true;}
};
SfntGposSubtable sub_storage[8][32];SfntGposLookup lookups[8];
struct Cell {SfntGsubGlyph glyph;u32 poison[3];};
bool check(ConstSlice<u8> bytes,ConstSlice<SfntPlacement> values,u32 font,u32 config,u32 stage,u32 split) noexcept {
    bool pass=true;for(u32 i=0;i<values.count;++i){u32 bits[3];copy_bytes(bits,&values.data[i],12);
        for(u32 k=0;k<3;++k){const u32 expected=byte_load_le32(bytes.data+i*12+k*4);++float_checks;if(bits[k]!=expected){pass=false;++failures;
            if(failures<10){print(utf8_literal("mismatch_font="));number(font);print(utf8_literal(" config="));number(config);print(utf8_literal(" stage="));number(stage);print(utf8_literal(" split="));number(split);print(utf8_literal(" glyph="));number(i);print(utf8_literal(" component="));number(k);print(utf8_literal(" actual="));number(bits[k]);print(utf8_literal(" expected="));number(expected);print(utf8_literal("\n"));}}}}
    return pass;
}
bool corpus(ConstSlice<u8> bytes) noexcept {
#ifdef MAICORD_LIGATURE_POSITION_TESTS
    const char* magic="MCGLP01\0";
#elif defined(MAICORD_CURSIVE_POSITION_TESTS)
    const char* magic="MCGCU01\0";
#elif defined(MAICORD_MARK_POSITION_TESTS)
    const char* magic="MCGMK01\0";
#else
    const char* magic="MCGPX01\0";
#endif
    if(bytes.count<12 || !bytes_equal(bytes.data,magic,8))return false;Reader r{bytes,8};u32 fonts=0;if(!r.word(&fonts)||fonts>10000)return false;bool tested_pair=false;
    for(u32 f=0;f<fonts;++f){u32 size=0,n=0,lc=0,cc=0;ConstSlice<u8> raw{},records{};
        if(!r.word(&size)||!r.word(&n)||!r.word(&lc)||!r.word(&cc)||!n||n>16||lc>8||cc>10000)return false;
#ifdef MAICORD_LIGATURE_POSITION_TESTS
        u32 has_associations=0;if(!r.word(&has_associations) || has_associations>16)return false;
#endif
        if(!r.take(size,&raw)||!r.take(n*4,&records))return false;
#ifdef MAICORD_LIGATURE_POSITION_TESTS
        SfntMarkAssociation associations[16];u32 target_glyphs[16];
        for(u32 i=0;i<has_associations;++i)if(!r.word(&target_glyphs[i]))return false;
        if(has_associations)for(u32 i=0;i<n;++i)if(!r.word(&associations[i].target)||!r.word(&associations[i].component))return false;
#endif
        SfntFace face{};SfntHorizontalMetrics metrics{};SfntLayout layout{};SfntGdef gdef{};ConstSlice<u8> table{};
        if(sfnt_open(raw,0,&face)!=SfntStatus::ok || sfnt_horizontal_metrics_open(face,&metrics)!=SfntStatus::ok || sfnt_table(face,sfnt_tag('G','P','O','S'),&table)!=SfntStatus::ok || sfnt_layout_open(table,SfntLayoutKind::positioning,&layout)!=SfntStatus::ok || sfnt_table(face,sfnt_tag('G','D','E','F'),&table)!=SfntStatus::ok || sfnt_gdef_open(table,metrics.glyph_count,&gdef)!=SfntStatus::ok)return false;
        SfntGsubGlyph glyphs[16];u8 flags[16];double advances[16];
        for(u32 i=0;i<n;++i){const u8* p=records.data+i*4;glyphs[i]={static_cast<u16>(p[0]|(static_cast<u16>(p[1])<<8)),static_cast<SfntGlyphClass>(p[2]),0};flags[i]=p[3];SfntGlyphMetrics m{};
            if(sfnt_horizontal_metrics(metrics,glyphs[i].glyph,&m)!=SfntStatus::ok)return false;advances[i]=m.advance;}
        SfntFeature feature{};u32 tag=0;if(sfnt_layout_feature(layout,0,{},&tag,&feature)!=SfntStatus::ok || tag!=sfnt_tag('t','e','s','t') || feature.lookup_indices.count!=lc*2)return false;
        for(u32 i=0;i<lc;++i){u16 index=0;if(!r.half(&index)||index!=byte_load_be16(feature.lookup_indices.data+i*2))return false;lookups[i]={};
#ifdef MAICORD_MARK_POSITION_TESTS
            SfntLookup directory{};if(sfnt_layout_lookup(layout,index,&directory)!=SfntStatus::ok)return false;mark_lookup[i]=directory.type==4 || directory.type==6;
#ifdef MAICORD_CURSIVE_POSITION_TESTS
#ifdef MAICORD_LIGATURE_POSITION_TESTS
            ligature_lookup[i]=directory.type==5;
            if(ligature_lookup[i]){if(sfnt_gpos_attachment_lookup_prepare(layout,index,gdef,metrics.glyph_count,{mark_storage[i],32},&mark_lookups[i])!=SfntStatus::ok)return false;continue;}
#endif
            cursive_lookup[i]=directory.type==3;
            if(cursive_lookup[i]){if(sfnt_gpos_attachment_lookup_prepare(layout,index,gdef,metrics.glyph_count,{mark_storage[i],32},&mark_lookups[i])!=SfntStatus::ok)return false;continue;}
#endif
            if(mark_lookup[i]){if(sfnt_gpos_attachment_lookup_prepare(layout,index,gdef,metrics.glyph_count,{mark_storage[i],32},&mark_lookups[i])!=SfntStatus::ok)return false;continue;}
#endif
            if(sfnt_gpos_lookup_prepare(layout,index,gdef,metrics.glyph_count,{sub_storage[i],32},&lookups[i])!=SfntStatus::ok)return false;}
        if(!tested_pair && lc && lookups[0].subtables.count && lookups[0].subtables.data[0].type==2 && n>=2){
            SfntPosition complete[16];SfntPositionExecution completed{};if(sfnt_position_initialize({advances,n},{flags,n},false,{complete,n})!=SfntStatus::ok || sfnt_position_execute_lookup(lookups[0],gdef,sfnt_glyph_view({glyphs,n}),{complete,n},0,2,false,{},&completed)!=SfntStatus::ok || completed.matches!=1)return false;
            for(u32 budget=0;budget<1000;++budget){SfntPosition state[16],before[16];if(sfnt_position_initialize({advances,n},{flags,n},false,{state,n})!=SfntStatus::ok)return false;copy_bytes(before,state,n*sizeof(SfntPosition));SfntPositionExecution result{};
                const auto s=sfnt_position_execute_lookup(lookups[0],gdef,sfnt_glyph_view({glyphs,n}),{state,n},0,2,false,{},&result,budget);++budget_checks;
                if(s==SfntStatus::ok){if(result.matches!=1)return false;tested_pair=true;break;}
                if(s!=SfntStatus::resource_limit || result.matches>1 || result.operations>budget || !bytes_equal(state,result.matches?complete:before,n*sizeof(SfntPosition)))return false;}
            if(!tested_pair)return false;}
        for(u32 c=0;c<cc;++c){u32 em_bits=0,rtl=0;ConstSlice<u8> expected[2];if(!r.word(&em_bits)||!r.word(&rtl)||rtl>1||!r.take(n*12,&expected[0])||!r.take(n*12,&expected[1]))return false;float em=0;copy_bytes(&em,&em_bits,4);++configurations;
            for(u32 split=0;split<=n;++split){Cell cells[20];for(u32 k=0;k<20;++k)cells[k]={{65535,static_cast<SfntGlyphClass>(255),255},{0xdeadbeef,0xdddddddd,0xeeeeeeee}};
                for(u32 k=0;k<n;++k)cells[k<split?k:k+3].glyph=glyphs[k];const SfntGlyphView view{&cells[0].glyph,&cells[split+3].glyph,split,n-split,sizeof(Cell)};
                SfntPosition state[16];SfntPlacement placements[16];if(sfnt_position_initialize({advances,n},{flags,n},rtl!=0,{state,n})!=SfntStatus::ok || sfnt_position_finalize({state,n},{flags,n},em,metrics.units_per_em,{placements,n})!=SfntStatus::ok)return false;
                check(expected[0],{placements,n},f,c,0,split);
                for(u32 i=0;i<lc;++i){
#ifdef MAICORD_LIGATURE_POSITION_TESTS
                    if(ligature_lookup[i]){if(!has_associations)return false;double prefix[17];SfntLigatureExecution result{};
                        const auto status=sfnt_position_execute_ligature_lookup(mark_lookups[i],gdef,view,{advances,n},{associations,n},{target_glyphs,has_associations},{state,n},0,n,rtl!=0,{},SfntNullAnchorPolicy::windows_parent_coordinates,SfntComponentAnchorPolicy::skip_missing,{prefix,has_associations},&result);
                        if(status!=SfntStatus::ok){print(utf8_literal("ligature_execution_status="));number(static_cast<u32>(status));print(utf8_literal(" font="));number(f);print(utf8_literal("\n"));return false;}
                        ligature_matches+=result.matches;ligature_scan_steps+=result.scan_steps;ligature_prefix_queries+=result.prefix_queries;ligature_ordered_steps+=result.ordered_steps;ligature_missing_components+=result.missing_components;ligature_compatibility_anchors+=result.compatibility_anchors;continue;}
#endif
#ifdef MAICORD_CURSIVE_POSITION_TESTS
                    if(cursive_lookup[i]){u64 chain_bits=0xaaaaaaaaaaaaaaaaull;SfntCursiveExecution result{};const auto s=sfnt_position_execute_cursive_lookup(mark_lookups[i],gdef,view,{advances,n},{state,n},0,n,rtl!=0,{}, {&chain_bits,1},&result);
                        if(s!=SfntStatus::ok){print(utf8_literal("cursive_execution_status="));number(static_cast<u32>(s));print(utf8_literal(" font="));number(f);print(utf8_literal("\n"));return false;}cursive_matches+=result.matches;cursive_steps+=result.scan_steps;cursive_words+=result.chain_words;continue;}
#endif
#ifdef MAICORD_MARK_POSITION_TESTS
                    if(mark_lookup[i]){SfntMarkExecution result{};const auto s=sfnt_position_execute_mark_lookup(mark_lookups[i],gdef,view,{advances,n},{flags,n},{state,n},0,n,rtl!=0,{},SfntNullAnchorPolicy::windows_parent_coordinates,&result
#ifdef MAICORD_LIGATURE_POSITION_TESTS
                            ,1000000,nullptr,nullptr,has_associations?ConstSlice<SfntMarkAssociation>{associations,n}:ConstSlice<SfntMarkAssociation>{}
#endif
                        );
                        if(s!=SfntStatus::ok){print(utf8_literal("mark_execution_status="));number(static_cast<u32>(s));print(utf8_literal(" font="));number(f);print(utf8_literal("\n"));return false;}mark_matches+=result.matches;target_steps+=result.search_steps;compatibility_anchors+=result.compatibility_anchors;continue;}
#endif
                    SfntPositionExecution result{};const auto s=sfnt_position_execute_lookup(lookups[i],gdef,view,{state,n},0,n,rtl!=0,{},&result);
                    if(s!=SfntStatus::ok){print(utf8_literal("execution_status="));number(static_cast<u32>(s));print(utf8_literal("\n"));return false;}}
                if(sfnt_position_finalize({state,n},{flags,n},em,metrics.units_per_em,{placements,n})!=SfntStatus::ok)return false;check(expected[1],{placements,n},f,c,1,split);++executions;
            }
        }
    }return r.at==r.data.count && tested_pair;
}
#ifdef MAICORD_LIGATURE_POSITION_TESTS
// The ligature variant owns these common units; every variant keeps its corpus.
void put(u8* p,u32 at,u16 v) noexcept {p[at]=static_cast<u8>(v>>8);p[at+1]=static_cast<u8>(v);}
bool units() noexcept {
    double advance[]{100,200};u8 flags[]{2,2};SfntPosition positions[2]{{1,2,3},{4,5,6}},before[2];copy_bytes(before,positions,sizeof(before));
    if(sfnt_position_initialize({advance,2},{flags,2},false,{positions,2},1)!=SfntStatus::resource_limit || !bytes_equal(positions,before,sizeof(before)))return false;
    u64 nan_bits=0x7ff8000000000000ull;copy_bytes(&advance[1],&nan_bits,8);
    if(sfnt_position_initialize({advance,2},{flags,2},false,{positions,2})!=SfntStatus::invalid_input || !bytes_equal(positions,before,sizeof(before)))return false;advance[1]=200;
    if(sfnt_position_initialize({advance,2},{flags,2},false,{positions,1})!=SfntStatus::output_too_small)return false;flags[0]=0;
    if(sfnt_position_initialize({advance,2},{flags,2},false,{positions,2})!=SfntStatus::invalid_input)return false;flags[0]=2;
    // Both halves of a pair must resolve before either position changes.
    u8 raw[40]{};put(raw,0,1);put(raw,2,12);put(raw,4,1);put(raw,6,16);put(raw,8,1);put(raw,10,18);put(raw,12,1);put(raw,14,1);put(raw,16,1);
    put(raw,18,1);put(raw,20,2);put(raw,22,30);put(raw,24,8);put(raw,30,0x8000);
    SfntGposSubtable sub{};if(sfnt_gpos_subtable_open({raw,32},2,3,&sub)!=SfntStatus::ok)return false;SfntGposLookup lookup{{&sub,1},{}};SfntGsubGlyph glyphs[]{{1,SfntGlyphClass::base,0},{2,SfntGlyphClass::base,0}};SfntPositionExecution result{};
    if(sfnt_position_initialize({advance,2},{flags,2},false,{positions,2})!=SfntStatus::ok)return false;copy_bytes(before,positions,sizeof(before));
    const SfntPositionInstance variable{nullptr,0,0,true,VariationArithmetic::wide};
    if(sfnt_position_execute_lookup(lookup,{},sfnt_glyph_view({glyphs,2}),{positions,2},0,2,false,variable,&result)!=SfntStatus::missing_table || result.matches || !bytes_equal(before,positions,sizeof(before)))return false;
    // The no-variation sentinel does not require an instance.
    put(raw,26,65535);put(raw,28,65535);if(sfnt_gpos_subtable_open({raw,32},2,3,&sub)!=SfntStatus::ok || sfnt_position_execute_lookup(lookup,{},sfnt_glyph_view({glyphs,2}),{positions,2},0,2,false,variable,&result)!=SfntStatus::ok || positions[0].advance_offset!=30 || positions[1].advance_offset!=0)return false;
    u8 store_bytes[32]{};put(store_bytes,0,1);byte_store_be32(store_bytes+2,12);put(store_bytes,6,1);byte_store_be32(store_bytes+8,22);put(store_bytes,12,1);put(store_bytes,14,1);put(store_bytes,18,16384);put(store_bytes,20,16384);put(store_bytes,22,1);put(store_bytes,24,1);put(store_bytes,26,1);put(store_bytes,30,41);
    ItemVariationStore store{};VariationInstance instance{};double coordinate=.5,scalar=0;if(item_variation_store_open({store_bytes,32},0,&store)!=SfntStatus::ok || variation_instance_prepare(store,{&coordinate,1},{&scalar,1},&instance)!=SfntStatus::ok)return false;
    put(raw,26,0);put(raw,28,0);if(sfnt_gpos_subtable_open({raw,32},2,3,&sub)!=SfntStatus::ok)return false;
    for(u32 mode=0;mode<2;++mode){const SfntPositionInstance varied{&instance,0,0,true,static_cast<VariationArithmetic>(mode)};bool succeeded=false;
        for(u32 budget=0;budget<100;++budget){if(sfnt_position_initialize({advance,2},{flags,2},false,{positions,2})!=SfntStatus::ok)return false;copy_bytes(before,positions,sizeof(before));
            const auto s=sfnt_position_execute_lookup(lookup,{},sfnt_glyph_view({glyphs,2}),{positions,2},0,2,false,varied,&result,budget);++budget_checks;
            if(s==SfntStatus::ok){if(result.matches!=1 || positions[0].advance_offset!=30 || positions[1].advance_offset!=20.5)return false;succeeded=true;break;}
            if(s!=SfntStatus::resource_limit || result.matches || !bytes_equal(before,positions,sizeof(before)))return false;}
        if(!succeeded)return false;}
    SfntPlacement output[2];if(sfnt_position_finalize({positions,2},{flags,2},0,1000,{output,2})!=SfntStatus::invalid_input || sfnt_position_finalize({positions,2},{flags,2},16,0,{output,2})!=SfntStatus::invalid_input)return false;
    positions[0]={1e300,0,0};if(sfnt_position_finalize({positions,2},{flags,2},16,1000,{output,2})!=SfntStatus::resource_limit)return false;
    return sfnt_position_initialize({}, {},false,{})==SfntStatus::ok && sfnt_position_finalize({}, {},16,1000,{})==SfntStatus::ok;
}
bool guarded_storage() noexcept {
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize;auto* mem=static_cast<u8*>(VirtualAlloc(nullptr,page*8,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!mem)return false;
    DWORD old=0;for(u32 i=1;i<8;i+=2)if(!VirtualProtect(mem+i*page,page,PAGE_NOACCESS,&old)){VirtualFree(mem,0,MEM_RELEASE);return false;}
    auto* p=reinterpret_cast<SfntPosition*>(mem+page-2*sizeof(SfntPosition));auto* flags=mem+page*3-2;auto* a=reinterpret_cast<double*>(mem+page*5-2*sizeof(double));auto* out=reinterpret_cast<SfntPlacement*>(mem+page*7-2*sizeof(SfntPlacement));
    flags[0]=2;flags[1]=3;a[0]=100;a[1]=200;const bool ok=sfnt_position_initialize({a,2},{flags,2},true,{p,2})==SfntStatus::ok && sfnt_position_finalize({p,2},{flags,2},1000,1000,{out,2})==SfntStatus::ok && out[0].advance==100 && out[1].advance==0 && out[1].advance_offset==-200;
    VirtualFree(mem,0,MEM_RELEASE);return ok;
}
#include "gpos_mark_execute_units.inc"
#include "gpos_cursive_execute_units.inc"
#include "gpos_ligature_execute_units.inc"
#endif
bool file_corpus(const wchar_t* path) noexcept {
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)return false;LARGE_INTEGER size{};if(!GetFileSizeEx(file,&size) || size.QuadPart<12 || size.QuadPart>0xffffffffll){CloseHandle(file);return false;}
    HANDLE map=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);if(!map){CloseHandle(file);return false;}const auto* data=static_cast<const u8*>(MapViewOfFile(map,FILE_MAP_READ,0,0,0));const bool ok=data && corpus({data,static_cast<u32>(size.QuadPart)});if(data)UnmapViewOfFile(data);CloseHandle(map);CloseHandle(file);return ok;
}
int run() noexcept {
#ifdef MAICORD_LIGATURE_POSITION_TESTS
    if(!units() || !guarded_storage()){print(utf8_literal("FAIL position units/guards\n"));return 3;}
    if(!mark_units()){print(utf8_literal("FAIL mark units\n"));return 4;}
    if(!cursive_units()){print(utf8_literal("FAIL cursive units\n"));return 5;}
    if(!ligature_units()){print(utf8_literal("FAIL ligature units\n"));return 6;}
#endif
    const wchar_t* command=GetCommandLineW();bool quote=false;while(*command){if(*command=='"')quote=!quote;else if(!quote && (*command==' ' || *command=='\t'))break;++command;}while(*command==' ' || *command=='\t')++command;
    bool ok=false;if(!*command)ok=corpus({position_fixture,sizeof(position_fixture)});
    else {wchar_t path[32768];u32 n=0;quote=*command=='"';if(quote)++command;while(*command && !(quote?*command=='"':(*command==' ' || *command=='\t'))){if(n+1>=32768)return 2;path[n++]=*command++;}if(quote){if(*command!='"')return 2;++command;}while(*command==' ' || *command=='\t')++command;if(*command || !n)return 2;path[n]=0;ok=file_corpus(path);}
    print(utf8_literal("configurations="));number(configurations);print(utf8_literal("\nexecutions="));number(executions);print(utf8_literal("\nfloat_checks="));number(float_checks);print(utf8_literal("\nbudget_checks="));number(budget_checks);print(utf8_literal("\nfailures="));number(failures);print(utf8_literal("\n"));
#ifdef MAICORD_MARK_POSITION_TESTS
#ifdef MAICORD_LIGATURE_POSITION_TESTS
    print(utf8_literal("linear_search_steps="));number(linear_search_steps);print(utf8_literal("\nlinear_mark_matches="));number(linear_mark_matches);print(utf8_literal("\n"));
#endif
    print(utf8_literal("mark_matches="));number(mark_matches);print(utf8_literal("\ntarget_steps="));number(target_steps);print(utf8_literal("\ncompatibility_anchors="));number(compatibility_anchors);print(utf8_literal("\n"));
#ifndef MAICORD_CURSIVE_POSITION_TESTS
    print(ok&&!failures?utf8_literal("PASS native mark positioning\n"):utf8_literal("FAIL native mark positioning\n"));
#endif
#else
    print(ok&&!failures?utf8_literal("PASS native basic positioning\n"):utf8_literal("FAIL native basic positioning\n"));
#endif
#ifdef MAICORD_CURSIVE_POSITION_TESTS
    print(utf8_literal("cursive_matches="));number(cursive_matches);print(utf8_literal("\ncursive_steps="));number(cursive_steps);print(utf8_literal("\ncursive_words="));number(cursive_words);print(utf8_literal("\n"));
#ifdef MAICORD_LIGATURE_POSITION_TESTS
    print(utf8_literal("long_cursive_scan="));number(long_cursive_scan);print(utf8_literal("\nlong_cursive_edges="));number(long_cursive_edges);print(utf8_literal("\nlong_chain_words="));number(long_chain_words);print(utf8_literal("\ncursive_unresolved_failures="));number(cursive_unresolved_failures);print(utf8_literal("\n"));
#endif
#ifdef MAICORD_LIGATURE_POSITION_TESTS
    print(ok&&!failures?utf8_literal("PASS native ligature positioning\n"):utf8_literal("FAIL native ligature positioning\n"));
#else
    print(ok&&!failures?utf8_literal("PASS native cursive positioning\n"):utf8_literal("FAIL native cursive positioning\n"));
#endif
#endif
#ifdef MAICORD_LIGATURE_POSITION_TESTS
    print(utf8_literal("long_ligature_scan="));number(long_ligature_scan);print(utf8_literal("\nlong_ligature_queries="));number(long_ligature_queries);print(utf8_literal("\nligature_budget_checks="));number(ligature_budget_checks);print(utf8_literal("\n"));
    print(utf8_literal("ligature_matches="));number(ligature_matches);print(utf8_literal("\nligature_scan_steps="));number(ligature_scan_steps);print(utf8_literal("\nligature_prefix_queries="));number(ligature_prefix_queries);print(utf8_literal("\nligature_ordered_steps="));number(ligature_ordered_steps);print(utf8_literal("\nligature_missing_components="));number(ligature_missing_components);print(utf8_literal("\nligature_compatibility_anchors="));number(ligature_compatibility_anchors);print(utf8_literal("\n"));
#endif
    return ok&&!failures?0:1;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_gpos_execute_tests_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
