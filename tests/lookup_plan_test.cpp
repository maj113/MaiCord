// Manual: --cost measures lookup planning; the registered run is the check.
#include "maicord/text/lookup_plan.hpp"
#include "maicord/text/nominal_participation.hpp"
#include "maicord/text/nominal_sources.hpp"
#include "maicord/text/gsub_execute.hpp"
#include "maicord/text/gpos_execute.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/text/font_store.hpp"
#include "../src/text/lookup_plan_finish.hpp"
#include <Windows.h>
namespace maicord {namespace {
#include "generated/lookup_plan_fixture.inc"
#include "generated/layout_directory_fixture.inc"
void print(Utf8View s) noexcept {DWORD n=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s.data,s.bytes,&n,nullptr);}
void number(u64 v) noexcept {char s[32];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n/2;++i){const char c=s[i];s[i]=s[n-i-1];s[n-i-1]=c;}print({s,n});}
bool error(u32 font,u32 config,u32 code) noexcept {print(utf8_literal("FAIL font="));number(font);print(utf8_literal(" config="));number(config);print(utf8_literal(" code="));number(code);print(utf8_literal("\n"));return false;}
struct Reader {ConstSlice<u8> data;u32 at=0;bool word(u32* value) noexcept {if(data.count-at<4)return false;*value=byte_load_le32(data.data+at);at+=4;return true;}bool take(u32 n,ConstSlice<u8>* v) noexcept {if(n>data.count-at)return false;*v={data.data+at,n};at+=n;return true;}};
SfntGsubExecutableLookup gs_lookups[16];SfntGsubExecutableSubtable gs_subs[32];SfntCoverage cov[32];SfntClassDef classes[32];
u64 queries=0,lookup_checks=0,placement_checks=0,glyph_checks=0,budget_checks=0,guard_checks=0,app_checks=0;
bool plan_units(const SfntLayout& layout,bool independent,ConstSlice<u16> expected,u64 full,const SfntFeatureSubstitutions& variations={}) noexcept {
    u64 bits[16];u16 out[128];SfntLookupPlanResult result{};
    if(full>10000 || expected.count>128)return false;
    for(u64 budget=0;budget<=full;++budget){for(auto& v:out)v=0xeeee;const auto status=sfnt_latin_lookup_plan(layout,variations,independent,{bits,16},{out,128},&result,budget);++budget_checks;
        if(status!=(budget<full?SfntStatus::resource_limit:SfntStatus::ok) || result.operations>budget)return false;
        if(status==SfntStatus::ok){if(result.lookup_count!=expected.count || !bytes_equal(out,expected.data,expected.count*2))return false;}
        else for(auto v:out)if(v!=0xeeee)return false;
    }
    if(expected.count){for(auto& v:out)v=0xeeee;if(sfnt_latin_lookup_plan(layout,variations,independent,{bits,16},{out,expected.count-1},&result)!=SfntStatus::output_too_small || result.lookup_count!=expected.count)return false;for(auto v:out)if(v!=0xeeee)return false;}
    const u32 selected_words=(static_cast<u32>(layout.lookup_count)+63)/64;
    for(u64 budget=0;budget<=full;++budget){for(auto& value:out)value=0xeeee;
        auto status=sfnt_latin_lookup_plan(layout,variations,independent,{bits,selected_words},{},&result,budget);
        if(status==SfntStatus::ok||status==SfntStatus::output_too_small)
            status=finish_lookup_plan({bits,selected_words},{out,result.lookup_count},result.operations,budget);
        ++budget_checks;
        if(status!=(budget<full?SfntStatus::resource_limit:SfntStatus::ok))return false;
        if(status==SfntStatus::ok){if(result.lookup_count!=expected.count||!bytes_equal(out,expected.data,expected.count*2))return false;}
        else for(auto value:out)if(value!=0xeeee)return false;
    }
    SYSTEM_INFO info{};GetSystemInfo(&info);const u32 page=info.dwPageSize,words=(static_cast<u32>(layout.lookup_count)+63)/64;auto* memory=static_cast<u8*>(VirtualAlloc(nullptr,page*4,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!memory)return false;DWORD old=0;
    bool ok=VirtualProtect(memory+page,page,PAGE_NOACCESS,&old)!=0 && VirtualProtect(memory+page*3,page,PAGE_NOACCESS,&old)!=0;
    if(ok){auto* scratch=reinterpret_cast<u64*>(memory+page-words*8);auto* indices=reinterpret_cast<u16*>(memory+page*3-expected.count*2);ok=sfnt_latin_lookup_plan(layout,variations,independent,{scratch,words},{indices,expected.count},&result)==SfntStatus::ok && result.lookup_count==expected.count && bytes_equal(indices,expected.data,expected.count*2);++guard_checks;
        const auto counted=sfnt_latin_lookup_plan(layout,variations,independent,{scratch,words},{},&result);
        ok=ok&&(counted==SfntStatus::ok||counted==SfntStatus::output_too_small)&&finish_lookup_plan({scratch,words},{indices,result.lookup_count},result.operations,full)==SfntStatus::ok&&bytes_equal(indices,expected.data,expected.count*2);++guard_checks;}
    VirtualFree(memory,0,MEM_RELEASE);return ok;
}
void put(u8* p,u32 at,u16 v) noexcept {p[at]=static_cast<u8>(v>>8);p[at+1]=static_cast<u8>(v);}
bool hangul_stages() noexcept {
    u8 b[110]{};put(b,0,1);put(b,4,10);put(b,6,36);put(b,8,94);
    put(b,10,1);byte_store_be32(b+12,sfnt_tag('h','a','n','g'));put(b,16,8);put(b,18,4);put(b,24,0xffff);put(b,26,4);
    const u32 tags[]{sfnt_tag('c','c','m','p'),sfnt_tag('l','j','m','o'),sfnt_tag('t','j','m','o'),sfnt_tag('v','j','m','o')};
    const u16 refs[]{3,1,2,1,0,2,3,2};put(b,36,4);
    for(u32 i=0;i<4;++i){put(b,28+i*2,static_cast<u16>(i));byte_store_be32(b+38+i*6,tags[i]);put(b,42+i*6,static_cast<u16>(26+i*8));
        put(b,64+i*8,2);put(b,66+i*8,refs[i*2]);put(b,68+i*8,refs[i*2+1]);put(b,96+i*2,10);}
    put(b,94,4);put(b,104,1);SfntLayout layout{};
    if(sfnt_layout_open({b,sizeof(b)},SfntLayoutKind::substitution,&layout)!=SfntStatus::ok)return false;
    const u16 expected[]{1,3,1,2,2,3,0,2},required[]{0,1,2,3,1,2,2,3,0,2};
    for(u32 mode=0;mode<4;++mode){put(b,24,mode&2?2:0xffff);const auto want=mode&2?ConstSlice<u16>{required,10}:ConstSlice<u16>{expected,8};
        u64 bits=0;u16 output[12];SfntLookupPlanResult result{};
        const auto run=[&](u64 budget,u32 capacity) noexcept {return sfnt_alphabetic_lookup_plan(layout,{},sfnt_tag('j','a','m','o'),(mode&1)!=0,{&bits,1},{output,capacity},&result,budget);};
        if(run(1000000,12)!=SfntStatus::ok||result.lookup_count!=want.count||!bytes_equal(output,want.data,want.count*2))return false;
        const u64 full=result.operations;if(full>10000)return false;
        for(u64 budget=0;budget<=full;++budget){for(auto& value:output)value=0xeeee;const auto status=run(budget,12);++budget_checks;
            if(result.operations>budget||status!=(budget<full?SfntStatus::resource_limit:SfntStatus::ok))return false;
            if(status==SfntStatus::ok){if(result.lookup_count!=want.count||!bytes_equal(output,want.data,want.count*2))return false;}
            else for(auto value:output)if(value!=0xeeee)return false;}
        for(auto& value:output)value=0xeeee;
        if(run(1000000,want.count-1)!=SfntStatus::output_too_small||result.lookup_count!=want.count)return false;
        for(auto value:output)if(value!=0xeeee)return false;
    }
    return true;
}
bool boundaries() noexcept {
    if(!hangul_stages()){print(utf8_literal("FAIL Hangul stage order/budget\n"));return false;}
    // Three bitset words, both sides of a word boundary, a partial last word,
    // descending references and repetitions. Lookups share an empty payload.
    u8 b[324]{};put(b,0,1);put(b,4,10);put(b,6,30);put(b,8,54);
    put(b,10,1);byte_store_be32(b+12,sfnt_tag('l','a','t','n'));put(b,16,8);put(b,18,4);put(b,24,0xffff);put(b,26,1);
    put(b,30,1);byte_store_be32(b+32,sfnt_tag('c','c','m','p'));put(b,36,8);put(b,40,6);
    const u16 refs[]{129,64,63,0,64,129};for(u32 i=0;i<6;++i)put(b,42+i*2,refs[i]);put(b,54,130);for(u32 i=0;i<130;++i)put(b,56+i*2,262);put(b,316,1);
    SfntLayout l{};SfntLookupPlanResult r{};u64 bits[3];u16 out[4],expected[]{0,63,64,129};
    if(sfnt_layout_open({b,sizeof(b)},SfntLayoutKind::substitution,&l)!=SfntStatus::ok)return false;
    const bool modes[]{false,true};for(bool independent:modes){
        if(sfnt_latin_lookup_plan(l,{},independent,{bits,3},{out,4},&r)!=SfntStatus::ok || !plan_units(l,independent,{expected,4},r.operations))return false;
        for(auto& v:out)v=0xeeee;
        if(sfnt_latin_lookup_plan(l,{},independent,{bits,2},{out,4},&r)!=SfntStatus::output_too_small)return false;
        for(auto v:out)if(v!=0xeeee)return false;
    }
    const u32 standard_tags[]{sfnt_tag('e','t','h','i'),sfnt_tag('c','h','e','r'),sfnt_tag('a','r','m','n'),sfnt_tag('g','e','o','r'),sfnt_tag('t','h','a','i'),sfnt_tag('l','a','o',' ')};
    for(u32 tag:standard_tags)for(u32 direct=0;direct<2;++direct)for(bool independent:modes){
        const u32 selected=direct?tag:sfnt_tag('l','a','t','n');byte_store_be32(b+12,selected);
        if(sfnt_alphabetic_lookup_plan(l,{},tag,independent,{bits,3},{out,4},&r)!=SfntStatus::ok||r.script_tag!=selected||r.lookup_count!=4||!bytes_equal(out,expected,sizeof(expected)))return false;}
    byte_store_be32(b+12,sfnt_tag('l','a','t','n'));for(auto& v:out)v=0xeeee;
    put(b,52,130);if(sfnt_latin_lookup_plan(l,{},false,{bits,3},{out,4},&r)!=SfntStatus::malformed)return false;for(auto v:out)if(v!=0xeeee)return false;
    const u8 empty[]{0,1,0,0,0,10,0,10,0,10,0,0};
    if(sfnt_layout_open({empty,sizeof(empty)},SfntLayoutKind::substitution,&l)!=SfntStatus::ok || sfnt_latin_lookup_plan(l,{},false,{},{},&r)!=SfntStatus::ok || r.lookup_count || r.script_tag || r.language_found || !plan_units(l,false,{},r.operations))return false;
    // Real FeatureVariations selection feeds the planner: replaced feature,
    // unchanged feature in a nonempty substitution table, and null substitution.
    u8 v[sizeof(layout_directory_fixture)];copy_bytes(v,layout_directory_fixture,sizeof(v));
    if(sfnt_layout_open({v,sizeof(v)},SfntLayoutKind::substitution,&l)!=SfntStatus::ok)return false;
    u32 tag=0;SfntScript script{};if(sfnt_layout_script(l,1,&tag,&script)!=SfntStatus::ok)return false;
    const u32 required=static_cast<u32>(script.table.data-v)+byte_load_be16(script.table.data)+2;
    const i16 samples[]{0,-1,16384};
    for(u32 required_mode=0;required_mode<2;++required_mode){put(v,required,required_mode?0:0xffff);
        for(i16 x:samples){i16 coords[]{x,0};SfntFeatureSubstitutions substitutions{};u32 record=0;
            if(sfnt_layout_variation_select(l,{coords,2},&substitutions,&record)!=SfntStatus::ok || record!=(x==0?2u:x==-1?3u:4u))return false;
            const u16 want=x==0?1:0;
            for(bool independent:modes){const u32 n=independent && !required_mode?0:1;
                if(sfnt_latin_lookup_plan(l,substitutions,independent,{bits,3},{out,4},&r)!=SfntStatus::ok || r.lookup_count!=n || (n && out[0]!=want) || !plan_units(l,independent,{&want,n},r.operations,substitutions))return false;
            }
        }
    }
    if(sfnt_latin_lookup_plan({}, {},false,{},{},&r)!=SfntStatus::invalid_input || sfnt_latin_lookup_plan(l,{},false,{nullptr,1},{},&r)!=SfntStatus::invalid_input || sfnt_latin_lookup_plan(l,{},false,{},{nullptr,1},&r)!=SfntStatus::invalid_input || sfnt_latin_lookup_plan(l,{},false,{},{},nullptr)!=SfntStatus::invalid_input)return false;
    return true;
}
bool corpus(ConstSlice<u8> bytes) noexcept {
    if(bytes.count<12 || !bytes_equal(bytes.data,"MCPLN03\0",8))return false;Reader r{bytes,8};u32 count=0;if(!r.word(&count) || count>10000)return false;
    for(u32 f=0;f<count;++f){u32 size=0,kind=0,script=0,language=0,found=0;ConstSlice<u8> raw{};
        if(!r.word(&size)||!r.word(&kind)||!r.word(&script)||!r.word(&language)||!r.word(&found)||kind>1||found>1||!r.take(size,&raw))return false;
        SfntFace face{};SfntCmap cmap{};SfntHorizontalMetrics metrics{};ConstSlice<u8> table{};SfntGdef gdef{};SfntLayout layout{};
        if(sfnt_open(raw,0,&face)!=SfntStatus::ok || sfnt_cmap_open(face,&cmap)!=SfntStatus::ok || sfnt_horizontal_metrics_open(face,&metrics)!=SfntStatus::ok || sfnt_table(face,sfnt_tag('G','D','E','F'),&table)!=SfntStatus::ok || sfnt_gdef_open(table,metrics.glyph_count,&gdef)!=SfntStatus::ok || sfnt_table(face,kind?sfnt_tag('G','P','O','S'):sfnt_tag('G','S','U','B'),&table)!=SfntStatus::ok || sfnt_layout_open(table,static_cast<SfntLayoutKind>(kind),&layout)!=SfntStatus::ok)return error(f,0,1);
        SfntGsubProgram program{};if(!kind && sfnt_gsub_program_prepare(layout,gdef,metrics.glyph_count,{{gs_lookups,16},{gs_subs,32},{cov,32},{classes,32}},&program)!=SfntStatus::ok)return error(f,0,2);
        u64 participation=0,participation_scratch[32],sources=0;SfntNominalParticipationResult participation_result{};
        if(metrics.glyph_count>64 || sfnt_nominal_sources(cmap,{&sources,1})!=SfntStatus::ok || sfnt_nominal_participation_add(layout,metrics.glyph_count,{&participation,1},{participation_scratch,32},&participation_result,1000000,{&sources,1})!=SfntStatus::ok)return error(f,0,10);
        for(u32 c=0;c<2;++c){u32 independent=0,rtl=0,n=0,want=0,simple=0,layout_want=0;u32 expected_placement[3]{},layout_placement[3]{};ConstSlice<u8> expected_ids{};
            if(!r.word(&independent)||!r.word(&rtl)||!r.word(&n)||!r.word(&want)||!r.word(&expected_placement[0])||!r.word(&expected_placement[1])||!r.word(&expected_placement[2])||!r.word(&simple)||!r.word(&layout_want)||!r.word(&layout_placement[0])||!r.word(&layout_placement[1])||!r.word(&layout_placement[2])||n>128||independent>1||rtl>1||simple>1||!r.take(n*2,&expected_ids))return false;
            u64 bits[16];u16 indices[128],expected[128];SfntLookupPlanResult plan{};auto status=sfnt_latin_lookup_plan(layout,{},independent!=0,{bits,16},{indices,128},&plan);
            if(status!=SfntStatus::ok || plan.lookup_count!=n || plan.script_tag!=script || plan.language_tag!=language || plan.language_found!=(found!=0))return error(f,c,3);
            for(u32 i=0;i<n;++i){expected[i]=static_cast<u16>(expected_ids.data[i*2]|(expected_ids.data[i*2+1]<<8));++lookup_checks;if(indices[i]!=expected[i])return error(f,c,4);}
            if(!rtl && !plan_units(layout,independent!=0,{expected,n},plan.operations))return error(f,c,5);
            u16 id=0;if(sfnt_cmap_glyph(cmap,'a',0,&id)!=SfntStatus::ok)return false;const bool can_nominal=id && !(participation&(1ull<<id));if(can_nominal!=(simple!=0))return error(f,c,11);SfntGsubGlyph glyph{id,sfnt_gdef_glyph_class(gdef,id),0};SfntGlyphBuffer buffer{};
            if(sfnt_glyph_buffer_open({reinterpret_cast<u8*>(&glyph),sizeof(glyph)},sizeof(glyph),1,&buffer)!=SfntStatus::ok)return false;
            SfntGsubExecutionFrame frames[2];u32 positions[8],rebased[8],consumed[8];u8 replacements[32];
            if(!kind)for(u32 i=0;i<n;++i){SfntGsubExecutionResult result{};status=sfnt_gsub_execute_lookup(program,indices[i],&buffer,0,1,0,{{frames,2},{positions,8},{rebased,8},{consumed,8},{replacements,32}},{1000000,100000},&result);if(status!=SfntStatus::ok)return error(f,c,6);}
            ++glyph_checks;if(glyph.glyph!=want)return error(f,c,7);SfntGlyphMetrics m{};if(sfnt_horizontal_metrics(metrics,glyph.glyph,&m)!=SfntStatus::ok)return false;const double nominal=m.advance;const u8 flag=2;SfntPosition state{};SfntPlacement placement{};
            if(sfnt_position_initialize({&nominal,1},{&flag,1},rtl!=0,{&state,1})!=SfntStatus::ok)return false;
            if(kind)for(u32 i=0;i<n;++i){SfntGposSubtable subs[16];SfntGposLookup lookup{};SfntPositionExecution result{};if(sfnt_gpos_lookup_prepare(layout,indices[i],gdef,metrics.glyph_count,{subs,16},&lookup)!=SfntStatus::ok || sfnt_position_execute_lookup(lookup,gdef,sfnt_glyph_buffer_view(buffer),{&state,1},0,1,rtl!=0,{},&result)!=SfntStatus::ok)return error(f,c,8);}
            if(sfnt_position_finalize({&state,1},{&flag,1},1000,metrics.units_per_em,{&placement,1})!=SfntStatus::ok || !bytes_equal(&placement,expected_placement,12))return error(f,c,9);placement_checks+=3;++queries;
            // These fixtures are one mapped ASCII scalar, one font, no spacing
            // or range boundary: font participation fully determines the gate.
            // General text, normalization and layout range eligibility are separate.
            u16 app_glyph=glyph.glyph;SfntPlacement app_placement=placement;
            if(can_nominal && !independent){app_glyph=id;if(sfnt_horizontal_metrics(metrics,id,&m)!=SfntStatus::ok)return false;SfntPosition p{static_cast<double>(m.advance),0,0};if(sfnt_position_finalize({&p,1},{&flag,1},1000,metrics.units_per_em,{&app_placement,1})!=SfntStatus::ok)return false;}
            if(app_glyph!=layout_want || !bytes_equal(&app_placement,layout_placement,12))return error(f,c,12);app_checks+=4;
        }
    }return r.at==bytes.count;
}
#include "lookup_plan_cost.inc"
int run() noexcept {
    const wchar_t* command=GetCommandLineW();bool quoted=false;
    while(*command){if(*command=='"')quoted=!quoted;else if(!quoted&&(*command==' '||*command=='\t'))break;++command;}
    while(*command==' '||*command=='\t')++command;
    if(*command){const wchar_t argument[]=L"--cost";u32 i=0;while(argument[i]&&command[i]==argument[i])++i;
        if(argument[i]||command[i])return 1;return lookup_cost()?0:1;}
    const bool boundary_ok=boundaries();if(!boundary_ok)print(utf8_literal("FAIL plan boundaries\n"));const bool ok=boundary_ok && corpus({plan_fixture,sizeof(plan_fixture)});print(utf8_literal("queries="));number(queries);print(utf8_literal("\nlookup_checks="));number(lookup_checks);print(utf8_literal("\nglyph_checks="));number(glyph_checks);print(utf8_literal("\nplacement_checks="));number(placement_checks);print(utf8_literal("\nbudget_checks="));number(budget_checks);print(utf8_literal("\nguard_checks="));number(guard_checks);print(utf8_literal("\napp_checks="));number(app_checks);print(ok?utf8_literal("\nPASS native lookup plan\n"):utf8_literal("\nFAIL native lookup plan\n"));return ok?0:1;}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_lookup_plan_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
