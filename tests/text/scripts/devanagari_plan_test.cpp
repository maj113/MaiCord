// Indic shaping (MaiCordDevanagariPlanTests), checked against HarfBuzz. Modes are the suite
// (cmake/test_registry.cmake): none runs the plan, form-probe, syllable and engine units;
// --<script>-input replays that script's HarfBuzz boundary corpus.
#include "../../../src/text/scripts/indic_syllables.hpp"
#include "maicord/text/scripts/indic_plan.hpp"
#include "maicord/text/scripts/indic_clusters.hpp"
#include "maicord/text/scripts/indic_run_plan.hpp"
#include "maicord/text/scripts/detail/indic_form.hpp"
#include "maicord/text/scripts/devanagari_forms.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/text/unicode/unicode.hpp"
#include "maicord/text/shaping/shape_run.hpp"
#include "maicord/core/byte_order.hpp"
#include "../../support/gsub_program_fixture.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/core/win32_user.hpp"
namespace maicord {namespace {
#include "../../generated/devanagari_plan_fixture.inc"
#include "../../generated/devanagari_input_fixture.inc"
#include "../../generated/tamil_input_fixture.inc"
#include "../../generated/bengali_input_fixture.inc"
#include "../../generated/gujarati_input_fixture.inc"
#include "../../generated/gurmukhi_input_fixture.inc"
#include "../../generated/odia_input_fixture.inc"
#include "../../generated/telugu_input_fixture.inc"
#include "../../generated/kannada_input_fixture.inc"
#include "../../generated/malayalam_input_fixture.inc"
void print(const char* text) noexcept {u32 n=0;while(text[n])++n;DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,n,&written,nullptr);}
void number(u64 value) noexcept {char digits[32];u32 n=0;do{digits[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);while(n){const char next[]{digits[--n],0};print(next);}}
bool failed(u32 line) noexcept {print("FAIL Indic line ");number(line);print("\n");return false;}
#define CHECK(x) do {if(!(x))return failed(__LINE__);} while(false)
template<u32 N> ConstSlice<u8> encode(const u16 (&words)[N],u8* bytes) noexcept {
    for(u32 i=0;i<N;++i){bytes[i*2]=static_cast<u8>(words[i]>>8);bytes[i*2+1]=static_cast<u8>(words[i]);}return {bytes,N*2};
}
// QA-only, DWORD-aligned little-endian records mapped from this executable.
ConstSlice<u32> indic_input_fixture(u16 id) noexcept {
    const auto resource=FindResourceW(nullptr,MAKEINTRESOURCEW(id),RT_RCDATA);
    if(!resource)return {};
    const auto size=SizeofResource(nullptr,resource);
    if(!size||size%sizeof(u32))return {};
    const auto data=LockResource(LoadResource(nullptr,resource));
    if(!data)return {};
    return {static_cast<const u32*>(data),static_cast<u32>(size/sizeof(u32))};
}
// Replays a boundary corpus (word = scalar | 0x40000000 vector end | HarfBuzz syllable start << 31,
// tools/indic_boundaries.py).
bool boundary_corpus(SfntIndicProfile profile,u16 resource,u32 words,u32 cases,const char* name) noexcept {
    const auto fixture=indic_input_fixture(resource);
    u32 scalars[256]{};u8 boundaries[256]{};u32 first=0,count=0,done=0;
    CHECK(fixture.count==words);
    for(u32 i=0;i<fixture.count;++i){const u32 entry=fixture[i];CHECK(count<256);scalars[count++]=entry&0x1fffffu;
        if(entry&0x40000000u){CHECK(sfnt_indic_cluster_boundaries(profile,{scalars,count},{boundaries,count})==SfntStatus::ok);
            for(u32 j=0;j<count;++j)if(boundaries[j]!=(fixture[first+j]>>31)){print(name);print(" boundary case ");number(done);print("\n");return false;}
            ++done;count=0;first=i+1;}}
    CHECK(!count&&done==cases);
    print("PASS ");print(name);print(" HarfBuzz syllable boundaries, ");number(done);print(" vectors\n");return true;
}
// HarfBuzz would_substitute on a first-party GSUB: exact input sequences, lookup by lookup.
bool form_units() noexcept {
    const u16 local[]{1,6,2,1,1,1}; // 1 -> 3
    const u16 reph[]{1,8,1,14,1,1,3,1,4,4,2,2}; // 3,2 -> 4
    const u16 below[]{1,8,1,14,1,1,2,1,4,5,2,3}; // 2,3 -> 5
    const u16 expand[]{1,8,1,14,1,1,1,2,6,7}; // 1 -> 6,7
    const u16 combine[]{1,8,1,14,1,1,6,1,4,8,3,7,2}; // 6,7,2 -> 8
    u8 data[5][32]{};const ConstSlice<u8> tables[]{encode(local,data[0]),encode(reph,data[1]),encode(below,data[2]),encode(expand,data[3]),encode(combine,data[4])};
    const u16 types[]{1,4,4,2,4};SfntGsubExecutableSubtable subtables[6]{};SfntGsubExecutableLookup lookups[6]{};
    for(u32 i=0;i<5;++i){CHECK(sfnt_gsub_subtable_open(tables[i],types[i],16,&subtables[i].basic,1000000,nullptr,true)==SfntStatus::ok);
        lookups[i].type=types[i];lookups[i].subtables={&subtables[i],1};}
    // ChainContextSubst format 3: backtrack [9], input [2][3], no lookahead, no actions.
    const u16 chain[]{3,1,16,2,22,28,0,0,1,1,9,1,1,2,1,1,3};u8 chain_bytes[34]{};SfntCoverage coverages[3]{};
    CHECK(sfnt_context_open(encode(chain,chain_bytes),SfntContextKind::chained,16,6,{coverages,3},{},&subtables[5].context)==SfntStatus::ok);
    lookups[5].type=6;lookups[5].subtables={&subtables[5],1};
    SfntGsubProgram program{{lookups,6},{},16};
    const auto would=[&](u16 lookup,ConstSlice<u16> glyphs,bool zero_context=true) noexcept {
        const SfntLookupStep step{lookup,0,0};bool result=false;u64 work=1000;
        return sfnt_gsub_would_substitute(program,{&step,1},glyphs,zero_context,&result,&work)==SfntStatus::ok&&result;};
    const u16 c_h[]{3,2},h_c[]{2,3},raw[]{1,2},one[]{1},three[]{6,7,2};
    CHECK(would(1,{c_h,2})&&!would(1,{raw,2})&&!would(1,{h_c,2})&&would(2,{h_c,2})&&!would(2,{c_h,2}));
    CHECK(would(0,{one,1})&&!would(0,{raw,2})&&would(3,{one,1})&&!would(3,{raw,2}));
    CHECK(would(4,{three,3})&&!would(4,{three,2}));
    // Context rules count only without backtrack and lookahead unless context is allowed (Malayalam).
    CHECK(!would(5,{h_c,2})&&would(5,{h_c,2},false)&&!would(5,{c_h,2},false));
    const SfntLookupStep all[]{{0,0,0},{2,0,0},{5,0,0}};bool result=false;u64 work=1000;
    CHECK(sfnt_gsub_would_substitute(program,{all,3},{h_c,2},true,&result,&work)==SfntStatus::ok&&result);
    work=0;CHECK(sfnt_gsub_would_substitute(program,{all,3},{h_c,2},true,&result,&work)==SfntStatus::resource_limit);
    work=1000;CHECK(sfnt_gsub_would_substitute(program,{all,3},{},true,&result,&work)==SfntStatus::invalid_input);
    const SfntLookupStep missing{6,0,0};CHECK(sfnt_gsub_would_substitute(program,{&missing,1},{h_c,2},true,&result,&work)==SfntStatus::invalid_input);
    print("PASS Indic would-substitute queries\n");return true;
}
// Feature order and masks as HarfBuzz's Indic shaper (hb-ot-shaper-indic collect_features); steps are
// stage 0 before the final reordering and stage 1 after it.
bool plan_units() noexcept {
    // Fixture features: abvs [4,1], akhn [3], calt [4], half [2], locl [6], nukt [5], pres [1,4], rphf [0], test [7].
    const SfntLookupStep expected[]{{6,0,0},{5,0,0},{3,0,0},{0,sfnt_deva_reph,0},{2,sfnt_deva_half,0},{1,0,1},{4,0,1}};
    // The required 'test' feature is an unknown tag: first stage, every glyph.
    const SfntLookupStep required[]{{6,0,0},{7,0,0},{5,0,0},{3,0,0},{0,sfnt_deva_reph,0},{2,sfnt_deva_half,0},{1,0,1},{4,0,1}};
    const ConstSlice<u8> fixtures[]{{deva_plan_fixture,sizeof(deva_plan_fixture)},{deva_required_fixture,sizeof(deva_required_fixture)}};
    u16 scratch[8]{};SfntLookupStep steps[64]{},saved[64]{};
    for(u32 with_required=0;with_required<2;++with_required){SfntLayout layout{};CHECK(sfnt_layout_open(fixtures[with_required],SfntLayoutKind::substitution,&layout)==SfntStatus::ok);
        SfntLookupPlanResult full{};CHECK(sfnt_indic_lookup_plan(layout,{},SfntIndicProfile::devanagari,{scratch,8},{steps,64},&full)==SfntStatus::ok);
        CHECK(full.script_tag==sfnt_tag('d','e','v','2')&&full.language_found&&!full.language_tag);
        if(!with_required)CHECK(full.lookup_count==7&&bytes_equal(steps,expected,sizeof(expected)));
        else CHECK(full.lookup_count==8&&bytes_equal(steps,required,sizeof(required)));
        copy_bytes(saved,steps,full.lookup_count*sizeof(SfntLookupStep));SfntLookupPlanResult result{};
        for(u64 budget=0;budget<=full.operations;++budget){for(auto& entry:steps)entry={65535,255,255};
            const auto status=sfnt_indic_lookup_plan(layout,{},SfntIndicProfile::devanagari,{scratch,8},{steps,64},&result,budget);
            CHECK(result.operations<=budget&&status==(budget==full.operations?SfntStatus::ok:SfntStatus::resource_limit));
            if(status!=SfntStatus::ok)for(const auto& entry:steps)CHECK(entry.lookup==65535&&entry.feature_mask==255&&entry.stage==255);
        }
        for(auto& entry:steps)entry={65535,255,255};
        CHECK(sfnt_indic_lookup_plan(layout,{},SfntIndicProfile::devanagari,{scratch,8},{steps,full.lookup_count-1},&result)==SfntStatus::output_too_small&&result.lookup_count==full.lookup_count);
        for(const auto& entry:steps)CHECK(entry.lookup==65535&&entry.feature_mask==255&&entry.stage==255);
        CHECK(sfnt_indic_lookup_plan(layout,{},SfntIndicProfile::devanagari,{scratch,7},{steps,64},&result)==SfntStatus::output_too_small);
        // Form probes use the feature's own lookups only (HarfBuzz would_substitute).
        CHECK(sfnt_indic_form_lookup_plan(layout,{},SfntIndicProfile::devanagari,sfnt_deva_reph,{scratch,8},{steps,64},&result)==SfntStatus::ok);
        CHECK(result.lookup_count==1&&steps[0].lookup==0&&!steps[0].feature_mask&&!steps[0].stage);
        const u64 form_work=result.operations;
        for(u64 budget=0;budget<=form_work;++budget){for(auto& step:steps)step={65535,255,255};
            const auto status=sfnt_indic_form_lookup_plan(layout,{},SfntIndicProfile::devanagari,sfnt_deva_reph,{scratch,8},{steps,64},&result,budget);
            CHECK(result.operations<=budget&&status==(budget==form_work?SfntStatus::ok:SfntStatus::resource_limit));
            if(status!=SfntStatus::ok)for(const auto& step:steps)CHECK(step.lookup==65535);}
        CHECK(sfnt_indic_form_lookup_plan(layout,{},SfntIndicProfile::devanagari,sfnt_deva_pre,{scratch,8},{steps,64},&result)==SfntStatus::ok&&!result.lookup_count);
        CHECK(sfnt_indic_form_lookup_plan(layout,{},SfntIndicProfile::devanagari,sfnt_deva_half,{scratch,8},{steps,64},&result)==SfntStatus::invalid_input);
    }
    // Only the modern script tag is planned.
    u8 legacy[sizeof(deva_plan_fixture)];copy_bytes(legacy,deva_plan_fixture,sizeof(legacy));
    byte_store_be32(legacy+byte_load_be16(legacy+4)+2,sfnt_tag('d','e','v','a'));SfntLayout layout{};SfntLookupPlanResult result{};
    CHECK(sfnt_layout_open({legacy,sizeof(legacy)},SfntLayoutKind::substitution,&layout)==SfntStatus::ok);
    CHECK(sfnt_indic_lookup_plan(layout,{},SfntIndicProfile::devanagari,{scratch,8},{steps,64},&result)==SfntStatus::ok&&!result.language_found&&!result.lookup_count);
    // A required rphf, absent from the optional FeatureIndex array: its stage, every glyph, and the reph probe.
    copy_bytes(legacy,deva_plan_fixture,sizeof(legacy));const u32 script_list=byte_load_be16(legacy+4);
    const u32 script=script_list+byte_load_be16(legacy+script_list+6),language=script+byte_load_be16(legacy+script);
    legacy[language+2]=0;legacy[language+3]=7;legacy[language+6+7*2]=0;legacy[language+7+7*2]=1;
    CHECK(sfnt_layout_open({legacy,sizeof(legacy)},SfntLayoutKind::substitution,&layout)==SfntStatus::ok);
    CHECK(sfnt_indic_form_lookup_plan(layout,{},SfntIndicProfile::devanagari,sfnt_deva_reph,{scratch,8},{steps,64},&result)==SfntStatus::ok);
    CHECK(result.lookup_count==1&&steps[0].lookup==0);
    CHECK(sfnt_indic_lookup_plan(layout,{},SfntIndicProfile::devanagari,{scratch,8},{steps,64},&result)==SfntStatus::ok);
    const SfntLookupStep global_reph[]{{6,0,0},{5,0,0},{3,0,0},{0,0,0},{2,sfnt_deva_half,0},{1,0,1},{4,0,1}};
    CHECK(result.lookup_count==7&&bytes_equal(steps,global_reph,sizeof(global_reph)));
    print("PASS Indic feature stages, masks, required feature, capacity and work limits\n");return true;
}
// Syllable boundaries checked against HarfBuzz 15.0.0 (tools/indic_boundaries.py oracle).
bool syllable_units() noexcept {
    struct Case {SfntIndicProfile profile;u32 count;u32 scalars[12];u8 starts[12];};
    constexpr auto deva=SfntIndicProfile::devanagari;
    const Case cases[]{
        // Ka, virama, ZWNJ ends the syllable; matras and a Vedic tail stay with their base.
        {deva,11,{0x915,0x94d,0x200c,0x915,0x93f,0x941,0x940,0x915,0x951,0x952,0x951},{1,0,0,1,0,0,0,1,0,0,0}},
        // Vowel constraints: a dotted circle joins the matra after an independent vowel.
        {deva,8,{0x905,0x93e,0x906,0x947,0x90f,0x947,0x905,0x940},{1,1,1,1,1,1,1,0}},
        // No cluster limit: six consonants with viramas are one syllable.
        {deva,12,{0x915,0x94d,0x915,0x94d,0x915,0x94d,0x915,0x94d,0x915,0x94d,0x915,0x93f},{1,0,0,0,0,0,0,0,0,0,0,0}},
        // A lone sign is a broken cluster; two bindus fit a syllable tail, the third starts again.
        {deva,5,{0x902,0x915,0x902,0x902,0x902},{1,1,0,0,1}},
        // Combining order puts the nuktas before the virama; the third nukta and the virama form a
        // broken cluster that starts at the virama's scalar.
        {deva,5,{0x915,0x94d,0x93c,0x93c,0x93c},{1,1,0,0,0}},
        {SfntIndicProfile::malayalam,4,{0xd4e,0xd15,0xd4d,0xd31},{1,0,0,0}},
        {SfntIndicProfile::kannada,5,{0xcb0,0xccd,0x200d,0xc95,0xcbf},{1,0,0,0,0}},
        {SfntIndicProfile::bengali,3,{0x9a8,0x9cd,0x9af},{1,0,0}},
        {SfntIndicProfile::telugu,4,{0xc12,0xc55,0xc46,0xc55},{1,1,0,1}},
    };
    u8 output[16];
    for(const auto& c:cases)CHECK(sfnt_indic_cluster_boundaries(c.profile,{c.scalars,c.count},{output,16})==SfntStatus::ok&&bytes_equal(output,c.starts,c.count));
    const u32 scalars[]{0x915,0x94d,0x915};
    for(auto& b:output)b=255;
    CHECK(sfnt_indic_cluster_boundaries(deva,{scalars,3},{output,16},47)==SfntStatus::resource_limit);for(auto b:output)CHECK(b==255);
    CHECK(sfnt_indic_cluster_boundaries(deva,{scalars,3},{output,2})==SfntStatus::output_too_small);
    const u32 invalid[]{0x915,0xd800};CHECK(sfnt_indic_cluster_boundaries(deva,{invalid,2},{output,16})==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(static_cast<SfntIndicProfile>(9),{scalars,3},{output,16})==SfntStatus::invalid_input);
    CHECK(sfnt_indic_cluster_boundaries(deva,{},{})==SfntStatus::ok);
    // HarfBuzz recomposes Tamil o + au length mark (a zero-class mark) into au and never decomposes au.
    {const char text[]{"\xe0\xae\x94 \xe0\xae\x92\xe0\xaf\x97"};IndicReader reader{SfntIndicProfile::tamil,{text,sizeof(text)-1},nullptr};
        IndicPiece pieces[8];u32 seen[3]{},count=0;
        for(;;){u32 length=0;IndicSyllable outcome{};CHECK(indic_read_syllable(reader,{pieces,8},&length,&outcome)==SfntStatus::ok);if(!length)break;
            CHECK(count<3&&length==1);seen[count++]=pieces[0].scalar();}
        CHECK(count==3&&seen[0]==0xb94&&seen[1]==0x20&&seen[2]==0xb94);}
    print("PASS Indic syllable units\n");return true;
}
// The engine on a first-party Devanagari font: cmap, GDEF marks (virama, anusvara, rakar, ee), rphf
// ra+virama, half ka+virama and blwf virama+ra, distinct advances, no GPOS. Expected glyphs and
// advances are HarfBuzz 15.0.0's on the same font built with fontTools.
bool engine_units() noexcept {
    // Glyphs: ka 1, ra 2, virama 3, i 4, zwj 5, zwnj 6, space 7, circle 8, ta 9, anusvara 10, aa 11, a 12,
    // reph 13, ka half 14, rakar 15, ee 16.
    const u32 cmap_words[]{0x000c0000,172,0,13,0x20,0x20,7,0x902,0x902,10,0x905,0x905,12,0x915,0x915,1,0x924,0x924,9,
        0x930,0x930,2,0x93e,0x93e,11,0x93f,0x93f,4,0x947,0x947,16,0x94d,0x94d,3,0x200c,0x200c,6,0x200d,0x200d,5,0x25cc,0x25cc,8};
    u8 cmap_bytes[172]{};for(u32 i=0;i<43;++i)byte_store_be32(cmap_bytes+i*4,cmap_words[i]);const SfntCmap cmap{{cmap_bytes,172},{},17,12};
    const u16 rphf[]{1,8,1,14,1,1,2,1,4,13,2,3},half[]{1,8,1,14,1,1,1,1,4,14,2,3},blwf[]{1,8,1,14,1,1,3,1,4,15,2,2};
    const u16 classes[]{1,3,14,3,0,0,0,0,0,0,3,0,0,0,0,3,3};
    u8 bytes[3][32]{},class_bytes[40]{};const ConstSlice<u8> tables[]{encode(rphf,bytes[0]),encode(half,bytes[1]),encode(blwf,bytes[2])};
    SfntGsubExecutableSubtable subtables[3]{};SfntGsubExecutableLookup lookups[3]{};SfntGdef gdef{};
    CHECK(sfnt_class_def_open(encode(classes,class_bytes),17,&gdef.glyph_classes)==SfntStatus::ok);
    for(u32 i=0;i<3;++i){CHECK(sfnt_gsub_subtable_open(tables[i],4,17,&subtables[i].basic)==SfntStatus::ok);lookups[i].type=4;lookups[i].subtables={&subtables[i],1};}
    SfntGsubProgram program{{lookups,3},gdef,17};
    SfntShapePlan plan{};plan.substitutions=&program;plan.gdef=gdef;plan.units_per_em=1000;plan.refresh_substitution_classes=true;
    plan.advance=+[](void*,u16 glyph,double* value) noexcept {
        static constexpr u8 widths[]{100,101,102,50,104,105,106,107,108,109,0,111,112,113,114,0,0};*value=widths[glyph];return SfntStatus::ok;};
    const SfntLookupStep steps[]{{0,sfnt_deva_reph,0},{2,sfnt_deva_below,0},{1,sfnt_deva_half,0}};const u64 syllable_steps=7;
    u8 forms[128]{};forms[0x30]=sfnt_deva_reph|sfnt_deva_below;
    SfntComponentGlyph glyphs[64]{},replacement[64]{};SfntComponentNode nodes[64]{};SfntComponentUndo journal[256]{};
    SfntGsubExecutionFrame frames[8]{};u32 positions[256]{},rebased[256]{},consumed[64]{},characters[64]{},targets[64]{};u64 bits[2]{};
    SfntMarkAssociation associations[64]{};double advances[64]{};u8 flags[64]{};SfntPosition position[64]{};SfntPlacement placement[64]{};
    const SfntShapeStorage storage{{glyphs,64},{nodes,64},{bits,2},{characters,64},{targets,64},{associations,64},{advances,64},{flags,64},{position,64},{placement,64},
        {{frames,8},{positions,256},{rebased,256},{consumed,64},{reinterpret_cast<u8*>(replacement),sizeof(replacement)},{journal,256}},{}};
    struct Case {u32 scalars[8];u32 count;u16 glyphs[8];u8 advances[8];u8 sources[8];};
    const Case cases[]{
        {{0x930,0x94d,0x915},3,{1,13},{101,113},{0,0,0}},                    // reph after the base
        {{0x915,0x93f},2,{4,1},{104,101},{0,0}},                             // left matra
        {{0x93f},1,{4,8},{104,108},{0}},                                     // broken: dotted circle
        {{0x915,0x94d,0x200d},3,{14,7},{114,0},{0,0,0}},                     // explicit half form; ZWJ hidden
        {{0x924,0x94d,0x930},3,{9,15},{109,0},{0,0,0}},                      // below-base ra
        {{0x915,0x94d,0x924,0x93f},4,{4,14,9},{104,114,109},{0,0,0,0}},       // matra stays before a half form
        {{0x905,0x93e},2,{12,8,11},{112,108,111},{0,1}},                      // vowel constraint
        {{0x930,0x94d,0x915,0x93f,0x902},5,{4,1,13,10},{104,101,113,0},{0,0,0,0,0}},
        {{0x915,0x94d,0x200c,0x924,0x93f},5,{1,3,7,4,9},{101,50,0,104,109},{0,0,0,3,3}}, // ZWNJ ends a syllable
        {{0x915,0x200d},2,{1,7},{101,0},{0,0}},
        {{0x915,0x94d,0x924,0x94d,0x930,0x947},6,{14,9,15,16},{114,109,0,0},{0,0,0,0,0,0}},
        {{0x930,0x94d},2,{2,3},{102,50},{0,0}},                              // lone Ra,H: no reph
        {{0x902,0x902,0x902},3,{8,10,10,8,10},{108,0,0,108,0},{0,0,3}},      // two bindus per broken cluster
    };
    for(const auto& c:cases){char text[64];u32 size=0;for(u32 i=0;i<c.count;++i)size+=utf8_encode(c.scalars[i],text+size);SfntShapeRunResult result{};
        CHECK(indic_utf8_run(SfntIndicProfile::devanagari,plan,cmap,{text,size},{forms,128},1000,storage,&result,{steps,3},{&syllable_steps,1},{},false,0)==SfntStatus::ok);
        u32 n=0;while(n<8&&c.glyphs[n])++n;CHECK(result.glyph_count==n&&result.source_units==c.count);
        for(u32 i=0;i<n;++i)CHECK(glyphs[i].glyph.glyph==c.glyphs[i]&&placement[i].advance==c.advances[i]);
        for(u32 i=0;i<c.count;++i)CHECK(characters[i]==c.sources[i]);}
    // No cluster limit: sixteen half forms and a base form one syllable (HarfBuzz: 'i', 16 half ka, ka).
    char text[128];u32 size=0;for(u32 i=0;i<16;++i){size+=utf8_encode(0x915,text+size);size+=utf8_encode(0x94d,text+size);}
    size+=utf8_encode(0x915,text+size);size+=utf8_encode(0x93f,text+size);SfntShapeRunResult result{};
    CHECK(indic_utf8_run(SfntIndicProfile::devanagari,plan,cmap,{text,size},{forms,128},1000,storage,&result,{steps,3},{&syllable_steps,1},{},false,0)==SfntStatus::ok);
    CHECK(result.glyph_count==18&&glyphs[0].glyph.glyph==4&&glyphs[17].glyph.glyph==1);
    for(u32 i=1;i<17;++i)CHECK(glyphs[i].glyph.glyph==14);for(u32 i=0;i<34;++i)CHECK(!characters[i]);
    // Work and capacity limits fail without publishing.
    CHECK(indic_utf8_run(SfntIndicProfile::devanagari,plan,cmap,{text,size},{forms,128},1000,storage,&result,{steps,3},{&syllable_steps,1},{1000000,100000,size-1},false,0)==SfntStatus::resource_limit&&!result.glyph_count);
    auto small=storage;small.advances.count=8;
    CHECK(indic_utf8_run(SfntIndicProfile::devanagari,plan,cmap,{text,size},{forms,128},1000,small,&result,{steps,3},{&syllable_steps,1},{},false,0)==SfntStatus::output_too_small&&!result.glyph_count);
    print("PASS Indic engine on a first-party font\n");return true;
}
bool units() noexcept {return plan_units()&&form_units()&&syllable_units()&&engine_units();}
bool run() noexcept {
    const wchar_t* p=GetCommandLineW();bool quoted=false;while(*p){if(*p=='"')quoted=!quoted;else if(!quoted&&(*p==' '||*p=='\t'))break;++p;}while(*p==' '||*p=='\t')++p;
    if(!*p)return units();quoted=*p=='"';if(quoted)++p;wchar_t mode[64]{};u32 n=0;
    while(*p&&!(quoted?*p=='"':*p==' '||*p=='\t')){CHECK(n+1<64);mode[n++]=*p++;}if(quoted){CHECK(*p=='"');++p;}while(*p==' '||*p=='\t')++p;CHECK(n&&!*p);
    const auto is=[&](const wchar_t* option) noexcept {u32 i=0;while(option[i]&&option[i]==mode[i])++i;return !option[i]&&!mode[i];};
    if(is(L"--devanagari-input"))return boundary_corpus(SfntIndicProfile::devanagari,devanagari_input_fixture_resource,devanagari_input_fixture_words,devanagari_input_fixture_cases,"Devanagari");
    if(is(L"--tamil-input"))return boundary_corpus(SfntIndicProfile::tamil,tamil_input_fixture_resource,tamil_input_fixture_words,tamil_input_fixture_cases,"Tamil");
    if(is(L"--bengali-input"))return boundary_corpus(SfntIndicProfile::bengali,bengali_input_fixture_resource,bengali_input_fixture_words,bengali_input_fixture_cases,"Bengali");
    if(is(L"--gujarati-input"))return boundary_corpus(SfntIndicProfile::gujarati,gujarati_input_fixture_resource,gujarati_input_fixture_words,gujarati_input_fixture_cases,"Gujarati");
    if(is(L"--gurmukhi-input"))return boundary_corpus(SfntIndicProfile::gurmukhi,gurmukhi_input_fixture_resource,gurmukhi_input_fixture_words,gurmukhi_input_fixture_cases,"Gurmukhi");
    if(is(L"--odia-input"))return boundary_corpus(SfntIndicProfile::odia,odia_input_fixture_resource,odia_input_fixture_words,odia_input_fixture_cases,"Odia");
    if(is(L"--telugu-input"))return boundary_corpus(SfntIndicProfile::telugu,telugu_input_fixture_resource,telugu_input_fixture_words,telugu_input_fixture_cases,"Telugu");
    if(is(L"--kannada-input"))return boundary_corpus(SfntIndicProfile::kannada,kannada_input_fixture_resource,kannada_input_fixture_words,kannada_input_fixture_cases,"Kannada");
    if(is(L"--malayalam-input"))return boundary_corpus(SfntIndicProfile::malayalam,malayalam_input_fixture_resource,malayalam_input_fixture_words,malayalam_input_fixture_cases,"Malayalam");
    return failed(__LINE__);
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_devanagari_plan_entry() noexcept {ExitProcess(maicord::run()?0u:1u);}
