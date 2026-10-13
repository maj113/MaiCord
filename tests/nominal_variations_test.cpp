#include "maicord/text/font_axes.hpp"
#include "maicord/text/lookup_plan.hpp"
#include "maicord/text/nominal_sources.hpp"
#include "maicord/text/nominal_participation.hpp"
#include "maicord/text/gsub_execute.hpp"
#include "maicord/text/gpos_execute.hpp"
#include "maicord/core/byte_order.hpp"
#include <Windows.h>
namespace maicord {namespace {
#include "generated/nominal_variations_fixture.inc"
void print(Utf8View s) noexcept {DWORD n=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),s.data,s.bytes,&n,nullptr);}
void number(u64 v) noexcept {char s[32];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n/2;++i){const char c=s[i];s[i]=s[n-i-1];s[n-i-1]=c;}print({s,n});}
struct Reader {ConstSlice<u8> bytes;u32 at=0;bool word(u32* v) noexcept {if(bytes.count-at<4)return false;*v=byte_load_le32(bytes.data+at);at+=4;return true;}bool take(u32 n,ConstSlice<u8>* v) noexcept {if(n>bytes.count-at)return false;*v={bytes.data+at,n};at+=n;return true;}};
u64 cases=0,glyph_checks=0,placement_checks=0,layout_checks=0,budget_checks=0;
bool fail(u32 f,u32 c,u32 code) noexcept {print(utf8_literal("FAIL font="));number(f);print(utf8_literal(" case="));number(c);print(utf8_literal(" code="));number(code);print(utf8_literal("\n"));return false;}
bool run_corpus() noexcept {
    Reader r{{nominal_variations_fixture,sizeof(nominal_variations_fixture)},8};u32 fonts=0;if(!bytes_equal(r.bytes.data,"MCNVAR1\0",8) || !r.word(&fonts))return false;
    for(u32 f=0;f<fonts;++f){u32 size=0,kind=0,count=0;ConstSlice<u8> raw{};if(!r.word(&size)||!r.word(&kind)||!r.word(&count)||kind>1||!r.take(size,&raw))return false;
        SfntFace face{};SfntCmap cmap{};SfntHorizontalMetrics metrics{};SfntLayout layout{};SfntGdef gdef{};ConstSlice<u8> table{};FontAxes axes{};FontAxisMappings mappings{};AxisSegmentMap map[4];
        if(sfnt_open(raw,0,&face)!=SfntStatus::ok || sfnt_cmap_open(face,&cmap)!=SfntStatus::ok || sfnt_horizontal_metrics_open(face,&metrics)!=SfntStatus::ok || sfnt_table(face,kind?sfnt_tag('G','P','O','S'):sfnt_tag('G','S','U','B'),&table)!=SfntStatus::ok || sfnt_layout_open(table,static_cast<SfntLayoutKind>(kind),&layout)!=SfntStatus::ok || font_axes_open(face,&axes)!=SfntStatus::ok || axes.axis_count!=1 || font_axis_mappings_open(face,1,{map,4},&mappings)!=SfntStatus::ok || sfnt_table(face,sfnt_tag('G','D','E','F'),&table)!=SfntStatus::ok || sfnt_gdef_open(table,metrics.glyph_count,&gdef)!=SfntStatus::ok)return fail(f,0,1);
        u64 sources=0,participation=0,scratch[4];SfntNominalParticipationResult p{};
        if(metrics.glyph_count>64 || sfnt_nominal_sources(cmap,{&sources,1})!=SfntStatus::ok || sfnt_nominal_participation_add(layout,metrics.glyph_count,{&participation,1},{scratch,4},&p,1000000,{&sources,1})!=SfntStatus::ok)return fail(f,0,2);
        SfntGsubExecutableLookup lookups[2];SfntGsubExecutableSubtable subs[2];SfntGsubProgram program{};
        if(!kind && sfnt_gsub_program_prepare(layout,gdef,metrics.glyph_count,{{lookups,2},{subs,2},{},{}},&program)!=SfntStatus::ok)return fail(f,0,3);
        for(u32 c=0;c<count;++c){u32 weight=0,independent=0,n=0,wanted_record=0,simple=0;u32 text[2],wanted[2],wanted_position[6],layout_glyph[2],layout_position[6];
            if(!r.word(&weight)||!r.word(&independent)||!r.word(&n)||!r.word(&wanted_record)||!r.word(&simple)||n<1||n>2||independent>1||simple>1)return false;
            for(u32 i=0;i<n;++i)if(!r.word(text+i))return false;for(u32 i=0;i<n;++i)if(!r.word(wanted+i))return false;for(u32 i=0;i<n*3;++i)if(!r.word(wanted_position+i))return false;for(u32 i=0;i<n;++i)if(!r.word(layout_glyph+i))return false;for(u32 i=0;i<n*3;++i)if(!r.word(layout_position+i))return false;
            const FontAxisSetting setting{sfnt_tag('w','g','h','t'),static_cast<double>(weight)};double normalized=0;i16 condition=0;
            if(font_axis_coordinates(axes,mappings,{&setting,1},{&normalized,1},{&condition,1})!=SfntStatus::ok)return fail(f,c,4);
            SfntFeatureSubstitutions selected{};u32 record=0,used=0;if(sfnt_layout_variation_select(layout,{&condition,1},&selected,&record,1000000,&used)!=SfntStatus::ok || record!=wanted_record)return fail(f,c,5);
            const u32 full=used;for(u32 budget=0;budget<=full;++budget){SfntFeatureSubstitutions checked{};u32 value=0;const auto s=sfnt_layout_variation_select(layout,{&condition,1},&checked,&value,budget,&used);++budget_checks;if(s!=(budget<full?SfntStatus::resource_limit:SfntStatus::ok) || used>budget || (s==SfntStatus::ok && value!=wanted_record))return fail(f,c,6);}
            u16 plan_ids[2];SfntLookupPlanResult plan{};if(sfnt_latin_lookup_plan(layout,selected,independent!=0,{scratch,4},{plan_ids,2},&plan)!=SfntStatus::ok)return fail(f,c,7);
            SfntGsubGlyph glyphs[2];u16 nominal_ids[2];bool all_simple=true;for(u32 i=0;i<n;++i){if(sfnt_cmap_glyph(cmap,text[i],0,nominal_ids+i)!=SfntStatus::ok)return false;glyphs[i]={nominal_ids[i],sfnt_gdef_glyph_class(gdef,nominal_ids[i]),0};const bool one=nominal_scalar_is_simple(text[i]) && nominal_ids[i] && !(participation&(1ull<<nominal_ids[i]));if(!i && one!=(simple!=0))return fail(f,c,8);all_simple=all_simple && one;}
            SfntGlyphBuffer buffer{};if(sfnt_glyph_buffer_open({reinterpret_cast<u8*>(glyphs),sizeof(glyphs)},sizeof(SfntGsubGlyph),n,&buffer)!=SfntStatus::ok)return false;
            SfntGsubExecutionFrame frames[2];u32 positions[4],rebased[4],consumed[4];u8 replacements[16];
            if(!kind)for(u32 i=0;i<plan.lookup_count;++i){SfntGsubExecutionResult result{};if(sfnt_gsub_execute_lookup(program,plan_ids[i],&buffer,0,n,0,{{frames,2},{positions,4},{rebased,4},{consumed,4},{replacements,16}},{100000,10000},&result)!=SfntStatus::ok)return fail(f,c,9);}
            double advances[2];u8 flags[2]{2,2};SfntPosition state[2];SfntPlacement placed[2];
            for(u32 i=0;i<n;++i){SfntGlyphMetrics m{};if(glyphs[i].glyph!=wanted[i] || sfnt_horizontal_metrics(metrics,glyphs[i].glyph,&m)!=SfntStatus::ok)return fail(f,c,10);advances[i]=m.advance;++glyph_checks;}
            if(sfnt_position_initialize({advances,n},{flags,n},false,{state,n})!=SfntStatus::ok)return false;
            if(kind)for(u32 i=0;i<plan.lookup_count;++i){SfntGposSubtable tables[1];SfntGposLookup lookup{};SfntPositionExecution result{};if(sfnt_gpos_lookup_prepare(layout,plan_ids[i],gdef,metrics.glyph_count,{tables,1},&lookup)!=SfntStatus::ok || sfnt_position_execute_lookup(lookup,gdef,sfnt_glyph_buffer_view(buffer),{state,n},0,n,false,{},&result)!=SfntStatus::ok)return fail(f,c,11);}
            if(sfnt_position_finalize({state,n},{flags,n},1000,metrics.units_per_em,{placed,n})!=SfntStatus::ok || !bytes_equal(placed,wanted_position,n*12))return fail(f,c,12);placement_checks+=n*3;
            // These fixtures are one ASCII word in one font/style. This does not
            // define general range segmentation or combining-mark policy.
            if(all_simple && !independent)for(u32 i=0;i<n;++i){SfntGlyphMetrics m{};if(sfnt_horizontal_metrics(metrics,nominal_ids[i],&m)!=SfntStatus::ok)return false;glyphs[i].glyph=nominal_ids[i];state[i]={static_cast<double>(m.advance),0,0};}
            if(all_simple && !independent && sfnt_position_finalize({state,n},{flags,n},1000,metrics.units_per_em,{placed,n})!=SfntStatus::ok)return false;
            for(u32 i=0;i<n;++i)if(glyphs[i].glyph!=layout_glyph[i])return fail(f,c,13);if(!bytes_equal(placed,layout_position,n*12))return fail(f,c,14);layout_checks+=n*4;++cases;
        }
    }return r.at==r.bytes.count;
}
int run() noexcept {const bool ok=run_corpus();print(utf8_literal("cases="));number(cases);print(utf8_literal("\nglyph_checks="));number(glyph_checks);print(utf8_literal("\nplacement_checks="));number(placement_checks);print(utf8_literal("\nlayout_checks="));number(layout_checks);print(utf8_literal("\nbudget_checks="));number(budget_checks);print(ok?utf8_literal("\nPASS native nominal variations\n"):utf8_literal("\nFAIL native nominal variations\n"));return ok?0:1;}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_nominal_variations_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
