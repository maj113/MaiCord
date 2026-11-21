#include "maicord/text/opentype/context.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/virtual_memory.hpp"
#include "../../support/test_output.hpp"
#include "../../support/text_seams.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "../../support/text_test_support.hpp"

namespace maicord {namespace {
#include "../../generated/context_fixture.inc"
volatile u32 observations=0;
void observe(SfntStatus s) noexcept {observations=observations+static_cast<u32>(s)+1;}
void put(u8* p,u32 at,u16 v) noexcept {p[at]=static_cast<u8>(v>>8);p[at+1]=static_cast<u8>(v);}
bool units() noexcept {
    SfntGdef gdef{};if(sfnt_gdef_open({context_gdef_fixture,sizeof(context_gdef_fixture)},10,&gdef)!=SfntStatus::ok)return false;SfntLookupFilter filter{};if(sfnt_lookup_filter_prepare(gdef,8,0,&filter)!=SfntStatus::ok)return false;
    SfntCoverage coverage[16];SfntClassDef classes[3];u32 positions[4];SfntGsubGlyph input[11],reverse_input[9];const u16 ids[]{2,9,1,9,3,9,4,9,5,9,6},rev_ids[]{2,9,1,9,3,9,5,9,6};
    for(u32 i=0;i<11;++i)input[i]={ids[i],sfnt_gdef_glyph_class(gdef,ids[i]),0};for(u32 i=0;i<9;++i)reverse_input[i]={rev_ids[i],sfnt_gdef_glyph_class(gdef,rev_ids[i]),0};
    for(const auto& f:context_fixtures){const ConstSlice<u8> bytes{f.data,f.size};
        if(f.mode==2){u32 count=0;SfntReverseSubstitution r{};SfntGsubMatch result{};
            if(sfnt_gsub_reverse_requirements(bytes,&count)!=SfntStatus::ok || count!=5 || sfnt_gsub_reverse_open(bytes,10,{coverage,4},&r)!=SfntStatus::output_too_small || r.table.data)return false;
            if(sfnt_gsub_reverse_open(bytes,10,{coverage,16},&r)!=SfntStatus::ok || sfnt_gsub_reverse_match_view(r,gdef,filter,sfnt_glyph_view({reverse_input,9}),4,&result)!=SfntStatus::ok || !result.matched || result.single!=8 || result.last_input!=4 || result.input_count!=1)return false;
            if(sfnt_gsub_reverse_match_view(r,gdef,filter,sfnt_glyph_view({reverse_input,8}),4,&result)!=SfntStatus::ok || result.matched)return false;
            if(sfnt_gsub_reverse_match_view(r,gdef,filter,sfnt_glyph_view({reverse_input,9}),4,&result,0)!=SfntStatus::resource_limit || result.matched)return false;
            continue;
        }
        const auto kind=static_cast<SfntContextKind>(f.mode);SfntContextRequirements req{};SfntContext c{};SfntContextMatch result{};
        if(sfnt_context_requirements(bytes,kind,&req)!=SfntStatus::ok || req.coverages!=(f.format==3?(f.mode?6u:2u):1u) || req.classes!=(f.format==2?(f.mode?3u:1u):0u))return false;
        if(sfnt_context_open(bytes,kind,10,3,{}, {},&c)!=SfntStatus::output_too_small || c.table.data)return false;
        if(sfnt_context_open(bytes,kind,10,3,{coverage,16},{classes,3},&c)!=SfntStatus::ok || c.max_inputs!=2 || sfnt_context_match_view(c,gdef,filter,sfnt_glyph_view({input,11}),4,7,{positions,4},&result)!=SfntStatus::ok || !result.matched || result.input_count!=2 || result.last_input!=6 || positions[0]!=4 || positions[1]!=6 || result.actions.count!=12)return false;
        const auto& first=c.coverages[c.format==3&&c.kind==SfntContextKind::chained?byte_load_be16(c.table.data+2):0];
        const u32 first_index=sfnt_coverage_index(first,input[4].glyph);
        if(first_index==sfnt_not_covered||sfnt_context_match_view(c,gdef,filter,sfnt_glyph_view({input,11}),4,7,{positions,4},&result,1000000,nullptr,0,first_index)!=SfntStatus::ok||!result.matched||result.input_count!=2||result.last_input!=6||positions[0]!=4||positions[1]!=6||result.actions.count!=12)return false;
        SfntContextMatch rejected{};
        if(sfnt_context_match_view(c,gdef,filter,sfnt_glyph_view({input,11}),4,7,{positions,4},&rejected,1000000,nullptr,0,first.covered_count)!=SfntStatus::invalid_input||rejected.matched||rejected.input_count)return false;
        const u16 seq[]{1,0,1},lookup[]{2,1,0};for(u32 i=0;i<3;++i){SfntSequenceAction action{};if(sfnt_context_action(result,i,&action)!=SfntStatus::ok || action.sequence_index!=seq[i] || action.lookup_index!=lookup[i])return false;}
        const u32 actions_at=static_cast<u32>(result.actions.data-f.data);
        if(sfnt_context_match_view(c,gdef,filter,sfnt_glyph_view({input,11}),4,7,{positions,1},&result)!=SfntStatus::output_too_small || result.input_count!=2 || result.matched || result.actions.data)return false;
        if(sfnt_context_match_view(c,gdef,filter,sfnt_glyph_view({input,11}),4,5,{positions,4},&result)!=SfntStatus::ok || result.matched!=(f.format!=3) || result.actions.count || result.input_count!=(f.format==3?0u:1u))return false;
        if(sfnt_context_match_view(c,gdef,filter,sfnt_glyph_view({input,11}),4,7,{positions,4},&result,0)!=SfntStatus::resource_limit || result.matched)return false;
        u8 changed[512];if(f.size>sizeof(changed))return false;for(u32 i=0;i<f.size;++i)changed[i]=f.data[i];put(changed,actions_at,2);
        if(sfnt_context_open({changed,f.size},kind,10,3,{coverage,16},{classes,3},&c)!=SfntStatus::malformed || c.table.data)return false;
        put(changed,actions_at,1);put(changed,actions_at+2,3);if(sfnt_context_open({changed,f.size},kind,10,3,{coverage,16},{classes,3},&c)!=SfntStatus::malformed)return false;
    }
    // Ignore surplus format-1 sets, including an unused out-of-range offset.
    u8 extra[512];const auto& f=context_fixtures[0];for(u32 i=0;i<6;++i)extra[i]=f.data[i];put(extra,2,static_cast<u16>(byte_load_be16(f.data+2)+2));put(extra,4,2);put(extra,6,10);put(extra,8,65535);for(u32 i=8;i<f.size;++i)extra[i+2]=f.data[i];
    SfntContext c{};SfntContextMatch result{};if(sfnt_context_open({extra,f.size+2},SfntContextKind::sequence,10,3,{coverage,16},{classes,3},&c)!=SfntStatus::ok || sfnt_context_match_view(c,gdef,filter,sfnt_glyph_view({input,11}),4,7,{positions,4},&result)!=SfntStatus::ok || !result.matched)return false;
    // Ignore surplus coverage entries when no corresponding rule set exists.
    for(u32 i=0;i<f.size;++i)extra[i]=f.data[i];const u32 cov=byte_load_be16(extra+2);put(extra,cov+2,2);put(extra,cov+6,7);input[4].glyph=7;input[4].glyph_class=SfntGlyphClass::base;
    return sfnt_context_open({extra,f.size+2},SfntContextKind::sequence,10,3,{coverage,16},{classes,3},&c)==SfntStatus::ok && sfnt_context_match_view(c,gdef,filter,sfnt_glyph_view({input,11}),4,7,{positions,4},&result)==SfntStatus::ok && !result.matched;
}
bool guards() noexcept {
    Win32SystemInfo info{};GetSystemInfo(info.native());const u32 page=info.dwPageSize;auto* mem=static_cast<u8*>(pages_allocate(page*2));if(!mem)return false;if(!pages_protect(mem+page, page, PageAccess::none)){pages_release(mem);return false;}
    SfntCoverage coverages[128];SfntClassDef classes[3];u32 positions[16];SfntGsubGlyph glyphs[11],reverse_glyphs[9];const u16 ids[]{2,9,1,9,3,9,4,9,5,9,6},reverse_ids[]{2,9,1,9,3,9,5,9,6};for(u32 i=0;i<11;++i)glyphs[i]={ids[i],ids[i]==9 || ids[i]==3?SfntGlyphClass::mark:SfntGlyphClass::base,0};for(u32 i=0;i<9;++i)reverse_glyphs[i]={reverse_ids[i],reverse_ids[i]==9 || reverse_ids[i]==3?SfntGlyphClass::mark:SfntGlyphClass::base,0};SfntLookupFilter filter{{},8};u32 random=0x825ceb77;
    for(u32 i=0;i<20000;++i){const auto& f=context_fixtures[i%7];random^=random<<13;random^=random>>17;random^=random<<5;const u32 size=i<1024?minimum(i/7,f.size):random%(f.size+1);u8* b=mem+page-size;for(u32 j=0;j<size;++j)b[j]=f.data[j];if(i>=1024 && size)b[(random>>8)%size]^=static_cast<u8>(random>>16);
        if(f.mode==2){u32 count=0;const auto need=sfnt_gsub_reverse_requirements({b,size},&count);observe(need);if(need!=SfntStatus::ok || count>128)continue;SfntReverseSubstitution r{};const auto s=sfnt_gsub_reverse_open({b,size},10,{coverages,128},&r,1000);observe(s);if(s==SfntStatus::ok){SfntGsubMatch m{};observe(sfnt_gsub_reverse_match_view(r,{},filter,sfnt_glyph_view({reverse_glyphs,9}),4,&m,100));observations=observations+m.single;}continue;}
        const auto kind=static_cast<SfntContextKind>(f.mode);SfntContextRequirements req{};const auto need=sfnt_context_requirements({b,size},kind,&req);observe(need);if(need!=SfntStatus::ok || req.coverages>128 || req.classes>3)continue;SfntContext c{};const auto s=sfnt_context_open({b,size},kind,10,3,{coverages,128},{classes,3},&c,1000);observe(s);if(s==SfntStatus::ok){SfntContextMatch m{};observe(sfnt_context_match_view(c,{},filter,sfnt_glyph_view({glyphs,11}),4,7,{positions,16},&m,100));if(m.matched)for(u32 j=0;j<m.actions.count/4;++j){SfntSequenceAction action{};observe(sfnt_context_action(m,j,&action));observations=observations+action.sequence_index+action.lookup_index;}}
    }
    pages_release(mem);return true;
}
int run() noexcept {
    if(!units()){print(utf8_literal("FAIL context units\n"));return 1;}if(!guards()){print(utf8_literal("FAIL context guards\n"));return 2;}
    print(utf8_literal("PASS native contextual matching\n"));return 0;
}
}}
MAICORD_TEST_ENTRY(maicord_context_tests_entry) {::maicord::test_process_exit(static_cast<UINT>(maicord::run()));}
