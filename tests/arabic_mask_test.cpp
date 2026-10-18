#include "maicord/text/arabic_joining.hpp"
#include "gsub_program_fixture.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/win32_kernel.hpp"

namespace maicord {namespace {
struct ArabicMaskCase {ConstSlice<u8> gsub,gdef;ConstSlice<u32> scalars;ConstSlice<u16> expected;u8 mask;};
#include "generated/arabic_mask_fixture.inc"
SfntGsubExecutableLookup lookups[16];SfntGsubExecutableSubtable subtables[32];SfntCoverage coverages[128];SfntClassDef classes[32];
SfntGsubExecutionFrame frames[16];u32 positions[128],rebased[128],consumed[32];SfntGsubGlyph replacements[32],glyphs[64];
void print(Utf8View text) noexcept {DWORD done=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text.data,text.bytes,&done,nullptr);}
void number(u64 v) noexcept {char s[32];u32 n=0;do{s[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n/2;++i){char t=s[i];s[i]=s[n-i-1];s[n-i-1]=t;}print({s,n});}
bool vectors() noexcept {
    u32 case_index=0,queries=0,checks=0;
    for(const auto& c:arabic_mask_cases) {
        SfntLayout layout{};SfntGdef gdef{};SfntGsubProgram program{};
        if(c.scalars.count>64 || sfnt_layout_open(c.gsub,SfntLayoutKind::substitution,&layout)!=SfntStatus::ok ||
           sfnt_gdef_open(c.gdef,37,&gdef)!=SfntStatus::ok ||
           prepare_gsub_program(layout,gdef,37,{{lookups,16},{subtables,32},{coverages,128},{classes,32}},&program)!=SfntStatus::ok)return false;
        for(u32 gate=0;gate<2;++gate) {
            for(u32 i=0;i<program.lookups.count;++i)lookups[i].bounds.ready=static_cast<u16>(gate);
            for(u32 split=0;split<=c.scalars.count;++split) {
                for(u32 i=0;i<c.scalars.count;++i) {
                    const auto cp=c.scalars[i];if(cp!=0x628 && cp!=0x64e)return false;
                    glyphs[i]={static_cast<u16>(cp==0x628?2:6),cp==0x64e?SfntGlyphClass::mark:SfntGlyphClass::base,0};
                }
                if(arabic_joining_forms(c.scalars,{&glyphs[0].reserved,sizeof(glyphs)-3},sizeof(glyphs[0]))!=ArabicJoiningStatus::ok)return false;
                SfntGlyphBuffer buffer{};if(sfnt_glyph_buffer_open({reinterpret_cast<u8*>(glyphs),sizeof(glyphs)},sizeof(glyphs[0]),c.scalars.count,&buffer)!=SfntStatus::ok || sfnt_glyph_buffer_seek(&buffer,split)!=SfntStatus::ok)return false;
                SfntGsubExecutionResult result{};const auto status=sfnt_gsub_execute_lookup(program,0,&buffer,0,c.scalars.count,0,
                    {{frames,16},{positions,128},{rebased,128},{consumed,32},{reinterpret_cast<u8*>(replacements),sizeof(replacements)}},
                    {1000000,100000},&result,nullptr,nullptr,c.mask);
                const auto view=sfnt_glyph_buffer_view(buffer);bool ok=status==SfntStatus::ok && sfnt_glyph_count(view)==c.expected.count && result.input_end==c.expected.count;
                for(u32 i=0;i<minimum(sfnt_glyph_count(view),c.expected.count);++i){ok=ok && sfnt_glyph_at(view,i).glyph==c.expected[i];++checks;}
                ++queries;if(!ok){print(utf8_literal("FAIL case="));number(case_index);print(utf8_literal(" split="));number(split);print(utf8_literal(" status="));number(static_cast<u32>(status));print(utf8_literal(" actual="));for(u32 i=0;i<sfnt_glyph_count(view);++i){number(sfnt_glyph_at(view,i).glyph);print(utf8_literal(" "));}print(utf8_literal("\n"));return false;}
            }
        }
        ++case_index;
    }
    print(utf8_literal("mask_cases="));number(case_index);print(utf8_literal(" queries="));number(queries);print(utf8_literal(" glyph_checks="));number(checks);print(utf8_literal("\n"));return true;
}
int run() noexcept {if(!vectors())return 1;print(utf8_literal("PASS native Arabic form eligibility and GSUB execution\n"));return 0;}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_arabic_mask_tests_entry() noexcept {ExitProcess(static_cast<UINT>(maicord::run()));}
