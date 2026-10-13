// Indic shaping plans, profiles and corpora (MaiCordDevanagariPlanTests). The built-in
// --<script>-plan/-profile/-input/-shape modes are the suite (cmake/test_registry.cmake).
// Manual QA modes, not run by it: --clusters and --<script>-clusters (stdin cluster streams
// for the Python oracles), --malayalam-font-profile, a font path as the only argument,
// MAICORD_*_BATCH / MAICORD_*_RUN / MAICORD_*_COMPACT / MAICORD_DEVANAGARI_RTL (stdin batches over
// system fonts), MAICORD_*_FONT=<font file>, and the benchmarks MAICORD_INDIC_RUN_BENCH and
// MAICORD_DEVANAGARI_MEMO_BENCH (tools/benchmark_indic_window.py, benchmark_devanagari_memo.py).
#include "../src/text/indic_forms.hpp"
#include "maicord/text/devanagari_plan.hpp"
#include "maicord/text/indic_plan.hpp"
#include "maicord/text/indic_clusters.hpp"
#include "maicord/text/tamil_input.hpp"
#include "maicord/text/tamil_shape_run.hpp"
#include "maicord/text/bengali_shape_cluster.hpp"
#include "maicord/text/gujarati_shape_cluster.hpp"
#include "maicord/text/gurmukhi_shape_cluster.hpp"
#include "maicord/text/odia_shape_cluster.hpp"
#include "maicord/text/telugu_shape_cluster.hpp"
#include "maicord/text/kannada_shape_cluster.hpp"
#include "maicord/text/malayalam_shape_cluster.hpp"
#include "maicord/text/malayalam_run.hpp"
#include "maicord/text/malayalam_shape_run.hpp"
#include "maicord/text/kannada_shape_run.hpp"
#include "maicord/text/kannada_run.hpp"
#include "maicord/text/telugu_shape_run.hpp"
#include "maicord/text/telugu_run.hpp"
#include "maicord/text/odia_shape_run.hpp"
#include "maicord/text/odia_run.hpp"
#include "maicord/text/gurmukhi_shape_run.hpp"
#include "maicord/text/gurmukhi_run.hpp"
#include "maicord/text/detail/indic_form.hpp"
#include "maicord/text/gujarati_run.hpp"
#include "maicord/text/gujarati_shape_run.hpp"
#include "maicord/text/bengali_shape_run.hpp"
#include "maicord/text/bengali_run.hpp"
#include "maicord/text/tamil_run.hpp"
#include "maicord/text/devanagari_forms.hpp"
#include "maicord/text/devanagari_clusters.hpp"
#include "maicord/text/devanagari_order.hpp"
#include "maicord/text/devanagari_shape_cluster.hpp"
#include "maicord/text/devanagari_run.hpp"
#include "maicord/text/devanagari_run_plan.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/text/unicode.hpp"
#include "maicord/text/shape_run.hpp"
#include "maicord/core/arena.hpp"
#include "maicord/core/byte_order.hpp"
#include <Windows.h>
namespace maicord {namespace {
#include "generated/devanagari_plan_fixture.inc"
#include "generated/devanagari_cluster_fixture.inc"
#include "generated/devanagari_shape_samples.inc"
void print(const char* text) noexcept {u32 n=0;while(text[n])++n;DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,n,&written,nullptr);}
void number(u64 value) noexcept {char digits[32];u32 n=0;do{digits[n++]=static_cast<char>('0'+value%10);value/=10;}while(value);while(n){const char next[]{digits[--n],0};print(next);}}
bool failed(u32 line) noexcept {print("FAIL Devanagari plan line ");number(line);print("\n");return false;}
#define CHECK(x) do {if(!(x))return failed(__LINE__);} while(false)
#include "profile_positioning_storage.inc"
// QA-only, DWORD-aligned little-endian records mapped from this executable.
// Avoid parsing millions of integer tokens or allocating/copying the corpus.
ConstSlice<u32> indic_input_fixture(u16 id) noexcept {
    const auto resource=FindResourceW(nullptr,MAKEINTRESOURCEW(id),RT_RCDATA);
    if(!resource)return {};
    const auto size=SizeofResource(nullptr,resource);
    if(!size||size%sizeof(u32))return {};
    const auto data=LockResource(LoadResource(nullptr,resource));
    if(!data)return {};
    return {static_cast<const u32*>(data),size/sizeof(u32)};
}
#include "tamil_plan_units.inc"
#include "bengali_plan_units.inc"
#include "tamil_input_units.inc"
#include "bengali_input_units.inc"
#include "gujarati_input_units.inc"
#include "gurmukhi_input_units.inc"
#include "odia_input_units.inc"
#include "telugu_input_units.inc"
#include "kannada_input_units.inc"
#include "malayalam_input_units.inc"
bool tamil_profile=false,bengali_profile=false,gujarati_profile=false,gurmukhi_profile=false,odia_profile=false,telugu_profile=false,kannada_profile=false,malayalam_profile=false;
bool tamil_owned_profile=false;
template<u32 N> ConstSlice<u8> encode(const u16 (&words)[N],u8* bytes) noexcept {
    for(u32 i=0;i<N;++i){bytes[i*2]=static_cast<u8>(words[i]>>8);bytes[i*2+1]=static_cast<u8>(words[i]);}return {bytes,N*2};
}
#include "bengali_shape_units.inc"
#include "gujarati_shape_units.inc"
#include "gurmukhi_shape_units.inc"
#include "odia_shape_units.inc"
#include "telugu_shape_units.inc"
#include "kannada_shape_units.inc"
#include "malayalam_shape_units.inc"
#include "malayalam_presentation_units.inc"
bool form_units() noexcept {
    const u16 local[]{1,6,2,1,1,1}; // 1 -> 3
    const u16 half[]{1,8,1,14,1,1,3,1,4,4,2,2}; // 3,2 -> 4
    const u16 below[]{1,8,1,14,1,1,2,1,4,5,2,3}; // 2,3 -> 5
    const u16 erase[]{1,8,1,14,1,1,2,0}; // empty sequence, ignored compatibility path
    const u16 expand[]{1,8,1,14,1,1,1,2,6,7}; // 1 -> 6,7
    const u16 combine[]{1,8,1,14,1,1,6,1,4,8,3,7,2}; // 6,7,2 -> 8
    u8 data[6][32]{};const ConstSlice<u8> tables[]{encode(local,data[0]),encode(half,data[1]),encode(below,data[2]),encode(erase,data[3]),encode(expand,data[4]),encode(combine,data[5])};
    const u16 types[]{1,4,4,2,2,4};SfntGsubExecutableSubtable subtables[6]{};SfntGsubExecutableLookup lookups[6]{};
    for(u32 i=0;i<6;++i){CHECK(sfnt_gsub_subtable_open(tables[i],types[i],16,&subtables[i].basic,1000000,nullptr,true)==SfntStatus::ok);
        lookups[i].type=types[i];lookups[i].subtables={&subtables[i],1};}
    SfntGsubProgram program{{lookups,6},{},16};SfntComponentGlyph glyphs[8]{},replacement[8]{};
    SfntGsubExecutionFrame frames[8]{};u32 positions[32]{},rebased[32]{},consumed[8]{};SfntComponentUndo journal[64]{};
    SfntGsubExecutionScratch storage{{frames,8},{positions,32},{rebased,32},{consumed,8},{reinterpret_cast<u8*>(replacement),sizeof(replacement)},{journal,64}};
    const SfntLookupStep half_steps[]{{0,0,0},{1,0,0}},below_steps[]{{0,0,0},{2,0,0}},deletion[]{{3,0,0}},expansion[]{{4,0,0},{5,0,0}};
    SfntDevanagariFormResult result{};
    const auto probe=[&](ConstSlice<SfntLookupStep> steps,SfntDevanagariForm form,u64 work=1000000,u32 capacity=8) noexcept {
        return sfnt_devanagari_probe_form(program,steps,1,2,form,{glyphs,capacity},storage,&result,{work,100000});};
    CHECK(probe({half_steps,2},sfnt_deva_half)==SfntStatus::ok&&result.formed&&result.glyph==4);const u64 total=result.operations;
    for(u64 work=0;work<=total;++work){const auto status=probe({half_steps,2},sfnt_deva_half,work);
        CHECK(result.operations<=work&&status==(work==total?SfntStatus::ok:SfntStatus::resource_limit));
        CHECK(status==SfntStatus::ok?result.formed:!result.formed&&!result.glyph);}
    CHECK(probe({below_steps,2},sfnt_deva_below)==SfntStatus::ok&&result.formed&&result.glyph==5);
    CHECK(probe({below_steps,2},sfnt_deva_pre)==SfntStatus::ok&&result.formed);
    CHECK(probe({below_steps,2},sfnt_deva_post)==SfntStatus::ok&&result.formed);
    CHECK(probe({half_steps,2},sfnt_deva_reph)==SfntStatus::ok&&result.formed);
    CHECK(probe({half_steps,2},sfnt_deva_below)==SfntStatus::ok&&!result.formed);
    CHECK(probe({half_steps,1},sfnt_deva_half)==SfntStatus::ok&&!result.formed);
    CHECK(probe({deletion,1},sfnt_deva_half)==SfntStatus::ok&&!result.formed);
    CHECK(probe({expansion,2},sfnt_deva_half)==SfntStatus::ok&&result.formed&&result.glyph==8);
    CHECK(probe({expansion,2},sfnt_deva_half,1000000,2)==SfntStatus::output_too_small&&!result.formed);
    CHECK(probe({},sfnt_deva_half)==SfntStatus::ok&&!result.formed);
    CHECK(probe({half_steps,2},sfnt_deva_nukta)==SfntStatus::invalid_input&&!result.formed);
    const SfntLookupStep reversed[]{{1,0,0},{0,0,0}},masked[]{{0,1,0}},staged[]{{0,0,1}};
    CHECK(probe({reversed,2},sfnt_deva_half)==SfntStatus::invalid_input);
    CHECK(probe({masked,1},sfnt_deva_half)==SfntStatus::invalid_input);
    CHECK(probe({staged,1},sfnt_deva_half)==SfntStatus::invalid_input);
    // A consumed mark must own its own original source in the final ligature.
    const u16 classes[]{1,2,1,3};u8 class_bytes[8]{};
    CHECK(sfnt_class_def_open(encode(classes,class_bytes),16,&program.gdef.glyph_classes)==SfntStatus::ok);
    CHECK(probe({half_steps,2},sfnt_deva_half)==SfntStatus::ok&&result.formed);
    CHECK(probe({below_steps,2},sfnt_deva_below)==SfntStatus::ok&&result.formed);
    return true;
}
bool cluster_units() noexcept {
    u32 corpus[256]{};u8 corpus_output[256]{};u32 start=0,count=0,cases=0;
    for(u32 i=0;i<sizeof(deva_cluster_fixture)/sizeof(u32);++i){const u32 entry=deva_cluster_fixture[i];CHECK(count<256);corpus[count++]=entry&0x1fffffu;
        if(entry&0x40000000u){CHECK(sfnt_devanagari_cluster_boundaries({corpus,count},{corpus_output,count})==SfntStatus::ok);
            for(u32 j=0;j<count;++j)if(corpus_output[j]!=(deva_cluster_fixture[start+j]>>31)){print("cluster fixture case ");number(cases);print("\n");CHECK(false);}
            ++cases;count=0;start=i+1;}}
    CHECK(!count&&cases==deva_cluster_fixture_cases);
    const u32 scalars[]{0x915,0x94d,0x200c,0x915,0x93f,0x941,0x940,0x915,0x951,0x952,0x951};
    const u8 expected[]{1,0,0,1,0,0,0,1,0,0,1};u8 output[11]{};
    CHECK(sfnt_devanagari_cluster_boundaries({scalars,11},{output,11})==SfntStatus::ok&&bytes_equal(output,expected,11));
    for(u64 work=0;work<44;++work){for(auto& b:output)b=255;
        CHECK(sfnt_devanagari_cluster_boundaries({scalars,11},{output,11},work)==SfntStatus::resource_limit);
        for(auto b:output)CHECK(b==255);}
    CHECK(sfnt_devanagari_cluster_boundaries({scalars,11},{output,11},44)==SfntStatus::ok);
    for(auto& b:output)b=255;const u32 invalid[]{0x915,0xd800};
    CHECK(sfnt_devanagari_cluster_boundaries({invalid,2},{output,11})==SfntStatus::invalid_input);
    CHECK(sfnt_devanagari_cluster_boundaries({scalars,11},{output,10})==SfntStatus::output_too_small);
    for(auto b:output)CHECK(b==255);
    CHECK(sfnt_devanagari_cluster_boundaries({}, {})==SfntStatus::ok);
    const u32 vowel_matras[]{0x905,0x93e,0x906,0x947,0x90f,0x947,0x905,0x940};const u8 vowel_expected[]{1,1,1,1,1,1,1,0};
    CHECK(sfnt_devanagari_cluster_boundaries({vowel_matras,8},{output,11})==SfntStatus::ok&&bytes_equal(output,vowel_expected,8));
    CHECK(sfnt_devanagari_cluster_boundaries({scalars,11},{reinterpret_cast<u8*>(const_cast<u32*>(scalars)),11})==SfntStatus::invalid_input);
    // Larger than the Windows observer's input capacity: exercise the same
    // verified period without retaining a per-character auxiliary buffer.
    static u32 long_input[32768];static u8 long_output[32768];
    for(u32 i=0;i<32768;++i)long_input[i]=i%2?0x94du:0x915u;
    CHECK(sfnt_devanagari_cluster_boundaries({long_input,32768},{long_output,32768},32768*4)==SfntStatus::ok);
    for(u32 i=0;i<32768;++i)CHECK(long_output[i]==(i%10==0));
    return true;
}
bool cluster_transfer(bool write,void* bytes,u32 count) noexcept {
    auto* p=static_cast<u8*>(bytes);HANDLE stream=GetStdHandle(write?STD_OUTPUT_HANDLE:STD_INPUT_HANDLE);
    while(count){DWORD done=0;if(!(write?WriteFile(stream,p,count,&done,nullptr):ReadFile(stream,p,count,&done,nullptr))||!done)return false;p+=done;count-=done;}return true;
}
bool cluster_stream(SfntIndicProfile profile=SfntIndicProfile::devanagari,ConstSlice<u8> forms={}) noexcept {
    char magic[8];u32 rows=0;CHECK(cluster_transfer(false,magic,8)&&bytes_equal(magic,"MDVCL1\0\0",8));
    CHECK(cluster_transfer(false,&rows,4)&&rows<=200000);CHECK(cluster_transfer(true,magic,8)&&cluster_transfer(true,&rows,4));
    u32 scalars[4096];u8 boundaries[4096];
    for(u32 row=0;row<rows;++row){u32 count=0;CHECK(cluster_transfer(false,&count,4)&&count&&count<=4096);
        CHECK(cluster_transfer(false,scalars,count*4));CHECK(sfnt_indic_cluster_boundaries(profile,{scalars,count},{boundaries,count},1000000,forms)==SfntStatus::ok);
        CHECK(cluster_transfer(true,boundaries,count));}return true;
}
bool cluster_shape_units() noexcept {
    // First-party font views: Ra=1, halant=2, pre-matra=3, reph=4, space=5,
    // dotted circle=6, visarga=7, Ka=8. No installed font or platform shaper.
    const u32 cmap_words[]{0x000c0000,124,0,9,32,32,5,0x915,0x915,8,0x926,0x926,10,0x930,0x930,1,
        0x93c,0x93c,9,0x93f,0x93f,3,0x94d,0x94d,2,0x25cc,0x25cc,6,0x10000,0x10000,11};u8 cmap_bytes[124]{};
    for(u32 i=0;i<31;++i)byte_store_be32(cmap_bytes+i*4,cmap_words[i]);const SfntCmap cmap{{cmap_bytes,124},{},16,12};
    const u16 ligature[]{1,8,1,14,1,1,1,1,4,4,2,2},classes[]{1,2,6,3,1,3,1,1,3};u8 data[32]{},class_data[32]{};
    SfntGsubExecutableSubtable subtable{};CHECK(sfnt_gsub_subtable_open(encode(ligature,data),4,16,&subtable.basic)==SfntStatus::ok);
    SfntGdef gdef{};CHECK(sfnt_class_def_open(encode(classes,class_data),16,&gdef.glyph_classes)==SfntStatus::ok);
    SfntGsubExecutableLookup lookup{};lookup.type=4;lookup.subtables={&subtable,1};SfntGsubProgram program{{&lookup,1},gdef,16};
    SfntShapePlan plan{};plan.substitutions=&program;plan.gdef=gdef;plan.units_per_em=1000;plan.refresh_substitution_classes=true;
    plan.advance=+[](void*,u16,double* value) noexcept {*value=100;return SfntStatus::ok;};
    SfntComponentGlyph glyphs[16]{},replacement[16]{};SfntComponentNode nodes[16]{};SfntComponentUndo journal[128]{};
    SfntGsubExecutionFrame frames[8]{};u32 positions[32]{},rebased[32]{},consumed[16]{},characters[16]{},targets[32]{};u64 bits[1]{};
    SfntMarkAssociation associations[16]{};double advances[16]{};u8 flags[16]{};SfntPosition position[16]{};SfntPlacement placement[16]{};
    SfntShapeStorage storage{{glyphs,16},{nodes,16},{bits,1},{characters,16},{targets,32},{associations,16},{advances,16},{flags,16},{position,16},{placement,16},
        {{frames,8},{positions,32},{rebased,32},{consumed,16},{reinterpret_cast<u8*>(replacement),sizeof(replacement)},{journal,128}},{}};
    const u32 scalars[]{0x930,0x94d,0x915,0x93f};const u8 forms[]{sfnt_deva_reph,0,0,0};
    const SfntClusterGlyph input[]{{{1,SfntGlyphClass::base,0},0},{{2,SfntGlyphClass::mark,0},1},{{8,SfntGlyphClass::base,0},2},{{3,SfntGlyphClass::base,0},3}};
    const SfntLookupStep step{0,sfnt_deva_reph,static_cast<u8>(SfntDevanagariStage::reph)};SfntShapeRunResult result{};
    const auto shape=[&](u64 work=1000000) noexcept {return sfnt_devanagari_shape_cluster(plan,cmap,{input,4},{scalars,4},{forms,4},4,1000,storage,&result,{&step,1},{work,100000,1000000});};
    CHECK(shape()==SfntStatus::ok&&result.glyph_count==3&&glyphs[0].glyph.glyph==3&&glyphs[1].glyph.glyph==8&&glyphs[2].glyph.glyph==4);
    CHECK(placement[0].advance==100&&placement[1].advance==100&&placement[2].advance==0);for(u32 i=0;i<4;++i)CHECK(!characters[i]);
    CHECK(nodes[glyphs[2].component].count==2);const u64 full=result.lookup_operations;
    for(u64 work=0;work<=full;++work){const auto status=shape(work);CHECK(result.lookup_operations<=work);
        CHECK(status==(work==full?SfntStatus::ok:SfntStatus::resource_limit));if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units);}
    const u32 orphan_scalar=0x93f;const u8 orphan_form=0;const SfntClusterGlyph orphan{{3,SfntGlyphClass::base,0},0};
    CHECK(sfnt_devanagari_shape_cluster(plan,cmap,{&orphan,1},{&orphan_scalar,1},{&orphan_form,1},1,1000,storage,&result,{&step,1})==SfntStatus::ok);
    CHECK(result.glyph_count==2&&glyphs[0].glyph.glyph==3&&glyphs[1].glyph.glyph==6&&!characters[0]);
    const u32 joiner_scalar=0x200d;const SfntClusterGlyph joiner{{5,SfntGlyphClass::base,0},0};
    CHECK(sfnt_devanagari_shape_cluster(plan,cmap,{&joiner,1},{&joiner_scalar,1},{&orphan_form,1},1,1000,storage,&result,{&step,1},{},true)==SfntStatus::ok);
    CHECK(result.glyph_count==1&&glyphs[0].glyph.glyph==5&&placement[0].advance==0);
    const u32 grouped_scalars[]{0x926,0x94d,0x915,0x93f,0x93c};const u8 grouped_forms[5]{};
    const SfntClusterGlyph grouped_input[]{{{10,SfntGlyphClass::base,0},0},{{2,SfntGlyphClass::mark,0},1},{{8,SfntGlyphClass::base,0},2},{{3,SfntGlyphClass::base,0},3},{{9,SfntGlyphClass::mark,0},4}};
    const auto group_shape=[&](u64 work=1000000) noexcept {return sfnt_devanagari_shape_cluster(plan,cmap,{grouped_input,5},{grouped_scalars,5},{grouped_forms,5},5,1000,storage,&result,{&step,1},{work,100000,1000000});};
    CHECK(group_shape()==SfntStatus::ok&&result.glyph_count==5);
    const u16 grouped_glyphs[]{10,2,3,9,8};for(u32 i=0;i<5;++i)CHECK(glyphs[i].glyph.glyph==grouped_glyphs[i]&&!characters[i]);
    const u64 group_work=result.lookup_operations;
    for(u64 work=0;work<=group_work;++work){const auto status=group_shape(work);CHECK(result.lookup_operations<=work&&status==(work==group_work?SfntStatus::ok:SfntStatus::resource_limit));
        if(status!=SfntStatus::ok)CHECK(!result.glyph_count&&!result.source_units);}
    {
        u16 output_glyphs[1024]{};SfntPlacement output_placements[1024]{};u32 output_characters[1024]{};u8 font_forms[128]{};
        font_forms[0x30]=sfnt_deva_reph;
        NativeDevanagariRunStorage run_storage{storage,{output_glyphs,1024},{output_placements,1024},{output_characters,1024}};
        const u32 run_scalars[]{0x930,0x94d,0x915,0x93f,0x20,0x10000,0x915};char text[32]{};u32 bytes=0;
        for(auto scalar:run_scalars)bytes+=utf8_encode(scalar,text+bytes);
        NativeAsciiResult run{};const auto shape_run=[&](u64 work=1000000) noexcept {
            return native_devanagari_run(plan,cmap,{font_forms,128},{&step,1},{text,bytes},1000,run_storage,&run,{work,100000,1000000});};
        CHECK(shape_run()==SfntStatus::ok&&run.glyph_count==6&&run.source_units==8&&run.shaped_ranges==4);
        const u16 wanted_glyphs[]{3,8,4,5,11,8};const u32 wanted_characters[]{0,0,0,0,3,4,4,5};
        CHECK(bytes_equal(output_glyphs,wanted_glyphs,sizeof(wanted_glyphs))&&bytes_equal(output_characters,wanted_characters,sizeof(wanted_characters)));
        const u64 run_work=run.lookup_operations;
        const auto separate=run_storage;
        run_storage={storage,{reinterpret_cast<u16*>(glyphs),16},{placement,16},{characters,16}};
        CHECK(shape_run()==SfntStatus::ok&&run.glyph_count==6&&run.source_units==8&&run.lookup_operations==run_work);
        CHECK(bytes_equal(run_storage.glyphs.data,wanted_glyphs,sizeof(wanted_glyphs))&&bytes_equal(characters,wanted_characters,sizeof(wanted_characters)));
        CHECK(placement[0].advance==100&&placement[2].advance==0&&placement[5].advance==100);
        run_storage=separate;
        CHECK(native_devanagari_run(plan,cmap,{font_forms,128},{&step,1},{text,bytes},1000,run_storage,&run,{},7)==SfntStatus::ok);
        CHECK(output_placements[0].advance==100&&output_placements[1].advance==100&&output_placements[2].advance==7&&output_placements[2].advance_offset==0);
        CHECK(output_placements[3].advance==107&&output_placements[4].advance==107&&output_placements[5].advance==107);
        CHECK(bytes_equal(output_characters,wanted_characters,sizeof(wanted_characters)));
        char controlled[8];const u32 controlled_bytes=utf8_encode(0x915,controlled)+utf8_encode(0x200c,controlled+3);
        CHECK(native_devanagari_run(plan,cmap,{font_forms,128},{&step,1},{controlled,controlled_bytes},1000,run_storage,&run,{},7)==SfntStatus::ok);
        CHECK(run.glyph_count==2&&output_glyphs[0]==8&&output_glyphs[1]==5&&output_placements[0].advance==107&&output_placements[1].advance==0&&output_placements[1].advance_offset==-7);
        const u32 invalid_spacing[]{0xbf800000u,0x7f800000u,0x7fc00000u};
        for(auto bits_value:invalid_spacing){float spacing=0;copy_bytes(&spacing,&bits_value,4);
            CHECK(native_devanagari_run(plan,cmap,{font_forms,128},{&step,1},{text,bytes},1000,run_storage,&run,{},spacing)==SfntStatus::invalid_input&&!run.glyph_count&&!run.source_units);}
        for(u64 work=0;work<=run_work;++work){const auto status=shape_run(work);CHECK(run.lookup_operations<=work);
            CHECK(status==(work==run_work?SfntStatus::ok:SfntStatus::resource_limit));if(status!=SfntStatus::ok)CHECK(!run.glyph_count&&!run.source_units&&!run.shaped_ranges);}
        run_storage.glyphs.count=5;CHECK(shape_run()==SfntStatus::output_too_small&&!run.glyph_count&&!run.source_units);run_storage.glyphs.count=1024;
        run_storage.character_glyphs.count=7;CHECK(shape_run()==SfntStatus::output_too_small&&!run.glyph_count&&!run.source_units);run_storage.character_glyphs.count=1024;
        run_storage.placements.count=5;CHECK(shape_run()==SfntStatus::output_too_small&&!run.glyph_count&&!run.source_units);run_storage.placements.count=1024;
        text[bytes++]=static_cast<char>(0xf0);CHECK(shape_run()==SfntStatus::invalid_input&&!run.glyph_count&&!run.source_units);--bytes;
        CHECK(native_devanagari_run(plan,cmap,{font_forms,128},{&step,1},{text,bytes},1000,run_storage,&run,{1000000,100000,bytes-1})==SfntStatus::resource_limit);
        CHECK(native_devanagari_run(plan,cmap,{font_forms,128},{&step,1},{},1000,run_storage,&run)==SfntStatus::ok&&!run.source_units&&!run.glyph_count&&!run.lookup_operations);
        // More than twenty decoder windows with only sixteen glyph/component
        // scratch slots; the run output never aliases that reused scratch.
        char long_text[2048]{};u32 long_bytes=0;
        for(u32 i=0;i<129;++i)for(u32 j=0;j<5;++j)long_bytes+=utf8_encode(run_scalars[j],long_text+long_bytes);
        CHECK(native_devanagari_run(plan,cmap,{font_forms,128},{&step,1},{long_text,long_bytes},1000,run_storage,&run)==SfntStatus::ok);
        CHECK(run.glyph_count==516&&run.source_units==645&&run.shaped_ranges==258);
        for(u32 i=0;i<129;++i){for(u32 j=0;j<4;++j)CHECK(output_glyphs[i*4+j]==wanted_glyphs[j]);
            for(u32 j=0;j<5;++j)CHECK(output_characters[i*5+j]==i*4+(j==4?3:0));}
        const u64 uncached_work=run.lookup_operations;
        CHECK(native_devanagari_run(plan,cmap,{font_forms,128},{&step,1},{long_text,long_bytes},1000,run_storage,&run,{},0,true)==SfntStatus::ok);
        CHECK(run.glyph_count==516&&run.source_units==645&&run.shaped_ranges==258&&run.lookup_operations<uncached_work);
        for(u32 i=0;i<129;++i){for(u32 j=0;j<4;++j)CHECK(output_glyphs[i*4+j]==wanted_glyphs[j]);
            for(u32 j=0;j<5;++j)CHECK(output_characters[i*5+j]==i*4+(j==4?3:0));}
        // Replays must preserve surrogate maps, final-control context, spacing,
        // exact-prefix aliases and output atomicity at every work boundary.
        const auto memo_case=[&](ConstSlice<u32> input,float em,float spacing,bool rtl=false) noexcept {
            char source[128];u32 size=0;for(u32 i=0;i<input.count;++i)size+=utf8_encode(input[i],source+size);
            const auto invoke=[&](bool reuse,u64 work=1000000) noexcept {return native_devanagari_run(plan,cmap,{font_forms,128},{&step,1},
                {source,size},em,run_storage,&run,{work,100000,1000000},spacing,reuse,rtl);};
            run_storage=separate;CHECK(invoke(false)==SfntStatus::ok&&run.glyph_count<64&&run.source_units<=64);
            const auto wanted=run;u16 ids[64];SfntPlacement placed[64];u32 maps[64];
            copy_bytes(ids,output_glyphs,run.glyph_count*2);copy_bytes(placed,output_placements,run.glyph_count*sizeof(SfntPlacement));copy_bytes(maps,output_characters,run.source_units*4);
            const auto equal=[&]() noexcept {return run.glyph_count==wanted.glyph_count&&run.source_units==wanted.source_units&&run.shaped_ranges==wanted.shaped_ranges&&
                bytes_equal(run_storage.glyphs.data,ids,run.glyph_count*2)&&bytes_equal(run_storage.placements.data,placed,run.glyph_count*sizeof(SfntPlacement))&&
                bytes_equal(run_storage.character_glyphs.data,maps,run.source_units*4);};
            CHECK(invoke(true)==SfntStatus::ok&&equal());const u64 work=run.lookup_operations;
            for(u64 budget=0;budget<=work;++budget){const auto status=invoke(true,budget);CHECK(run.lookup_operations<=budget);
                CHECK(status==(budget==work?SfntStatus::ok:SfntStatus::resource_limit));if(status!=SfntStatus::ok)CHECK(!run.glyph_count&&!run.source_units&&!run.shaped_ranges);}
            for(u32 alias=1;alias<8&&wanted.glyph_count<16&&wanted.source_units<=16;++alias){run_storage=separate;
                if(alias&1)run_storage.glyphs={reinterpret_cast<u16*>(glyphs),16};
                if(alias&2)run_storage.placements={placement,16};
                if(alias&4)run_storage.character_glyphs={characters,16};
                CHECK(invoke(true)==SfntStatus::ok&&equal());}
            for(u32 short_buffer=0;short_buffer<3;++short_buffer){run_storage=separate;
                if(!short_buffer)run_storage.glyphs.count=wanted.glyph_count-1;
                else if(short_buffer==1)run_storage.placements.count=wanted.glyph_count-1;
                else run_storage.character_glyphs.count=wanted.source_units-1;
                CHECK(invoke(true)==SfntStatus::output_too_small&&!run.glyph_count&&!run.source_units&&!run.shaped_ranges);}
            run_storage=separate;return true;};
        const u32 repeated[]{0x930,0x94d,0x915,0x93f,0x20,0x10000,0x930,0x94d,0x915,0x93f,0x20,0x10000};
        const u32 controls[]{0x200d,0x20,0x200d,0x20,0x200d};
        const u32 evicted[]{0x915,0x926,0x930,0x20,0x10000,0x915,0x926,0x930,0x20,0x10000};
        const float spacings[]{0,7};for(float spacing:spacings){CHECK(memo_case({repeated,12},1000,spacing));CHECK(memo_case({controls,5},17.5f,spacing));CHECK(memo_case({evicted,10},14,spacing));}
        CHECK(memo_case({repeated,12},1000,0,true));CHECK(memo_case({controls,5},17.5f,0,true));CHECK(memo_case({evicted,10},14,0,true));
        CHECK(memo_case({repeated,12},17.5f,7,true));CHECK(memo_case({controls,5},14,7,true));CHECK(memo_case({evicted,10},1000,7,true));
        // Equal full 32-bit hash and byte length, but distinct source/glyphs.
        const u32 collisions[2][8]{{0x91c,0x94d,0x919,0x94d,0x91f,0x94d,0x915,0x93f},
                                   {0x934,0x94d,0x91b,0x94d,0x91a,0x94d,0x916,0x93f}};
        u32 collision_text[36];for(u32 side=0;side<2;++side){u32 hash=2166136261u;for(auto scalar:collisions[side])hash=(hash^scalar)*16777619u;CHECK(hash==0x0cd97f14u);}
        for(u32 i=0;i<4;++i){copy_bytes(collision_text+i*9,collisions[i&1],8*sizeof(u32));collision_text[i*9+8]=0x20;}
        CHECK(memo_case({collision_text,36},17.5f,7));
        CHECK(memo_case({collision_text,36},17.5f,0,true));
    }
    storage.glyphs.count=1;CHECK(sfnt_devanagari_shape_cluster(plan,cmap,{&orphan,1},{&orphan_scalar,1},{&orphan_form,1},1,1000,storage,&result,{&step,1})==SfntStatus::output_too_small);
    CHECK(!result.glyph_count&&!result.source_units);return true;
}
bool units() noexcept {
    CHECK(form_units()&&cluster_units()&&cluster_shape_units());
    {
        const u32 scalars[]{0x930,0x94d,0x915,0x94d,0x930,0x93f};
        const u8 forms[]{sfnt_deva_reph,0,sfnt_deva_half,0,sfnt_deva_below,0};u32 order[6]{};u8 masks[6]{};SfntDevanagariOrderResult result{};
        CHECK(sfnt_devanagari_initial_order({scalars,6},{forms,6},{order,6},{masks,6},&result)==SfntStatus::ok);
        const u32 wanted[]{0,1,5,2,3,4};CHECK(bytes_equal(order,wanted,sizeof(wanted))&&result.base==2&&result.reph_end==2&&result.pre_matra==5&&!result.needs_dotted_circle);
        CHECK((masks[0]&sfnt_deva_reph)&&(masks[1]&sfnt_deva_reph)&&!(masks[2]&sfnt_deva_half)&&(masks[3]&sfnt_deva_below));
        for(u64 work=0;work<6*64;++work){for(auto& x:order)x=65535;for(auto& x:masks)x=255;
            CHECK(sfnt_devanagari_initial_order({scalars,6},{forms,6},{order,6},{masks,6},&result,work)==SfntStatus::resource_limit);
            for(auto x:order)CHECK(x==65535);for(auto x:masks)CHECK(x==255);}
        const u32 marks[]{0x915,0x94d,0x93c};const u8 empty[]{0,0,0};
        CHECK(sfnt_devanagari_initial_order({marks,3},{empty,3},{order,6},{masks,6},&result)==SfntStatus::ok&&order[1]==2&&order[2]==1);
        CHECK(sfnt_devanagari_initial_order({scalars+5,1},{empty,1},{order,6},{masks,6},&result)==SfntStatus::ok&&result.needs_dotted_circle);
        const u32 separate[]{0x915,0x915};CHECK(sfnt_devanagari_initial_order({separate,2},{empty,2},{order,6},{masks,6},&result)==SfntStatus::unsupported_sequence);
    }
    const SfntLookupStep expected[]{{6,0,0},{5,1,1},{3,0,2},{0,2,3},{2,8,7},{1,0,12},{4,0,12}};
    const ConstSlice<u8> fixtures[]{{deva_plan_fixture,sizeof(deva_plan_fixture)},{deva_required_fixture,sizeof(deva_required_fixture)}};
    u16 scratch[8]{};SfntLookupStep steps[64]{},saved[64]{};
    for(u32 required=0;required<2;++required){SfntLayout layout{};CHECK(sfnt_layout_open(fixtures[required],SfntLayoutKind::substitution,&layout)==SfntStatus::ok);
        SfntLookupPlanResult full{};CHECK(sfnt_devanagari_lookup_plan(layout,{},false,{scratch,8},{steps,64},&full)==SfntStatus::ok);
        CHECK(full.script_tag==sfnt_tag('d','e','v','2')&&full.language_found&&!full.language_tag&&full.lookup_count==(required?19u:7u));
        if(!required)CHECK(bytes_equal(steps,expected,sizeof(expected)));
        else{u32 at=0;for(u32 stage=0;stage<13;++stage){if(stage==11)continue;
            for(const auto& wanted:expected)if(wanted.stage==stage){CHECK(at<full.lookup_count&&bytes_equal(&steps[at],&wanted,sizeof(wanted)));++at;}
            CHECK(at<full.lookup_count&&steps[at].lookup==7&&steps[at].stage==stage);++at;}CHECK(at==full.lookup_count);}
        copy_bytes(saved,steps,full.lookup_count*sizeof(SfntLookupStep));SfntLookupPlanResult result{};
        CHECK(sfnt_devanagari_lookup_plan(layout,{},true,{scratch,8},{steps,64},&result)==SfntStatus::ok&&result.lookup_count==full.lookup_count&&bytes_equal(steps,saved,full.lookup_count*sizeof(SfntLookupStep)));
        for(u64 budget=0;budget<=full.operations;++budget){for(auto& entry:steps)entry={65535,255,255};
            const auto status=sfnt_devanagari_lookup_plan(layout,{},false,{scratch,8},{steps,64},&result,budget);
            CHECK(result.operations<=budget&&status==(budget==full.operations?SfntStatus::ok:SfntStatus::resource_limit));
            if(status!=SfntStatus::ok)for(const auto& entry:steps)CHECK(entry.lookup==65535&&entry.feature_mask==255&&entry.stage==255);
        }
        for(auto& entry:steps)entry={65535,255,255};
        CHECK(sfnt_devanagari_lookup_plan(layout,{},false,{scratch,8},{steps,full.lookup_count-1},&result)==SfntStatus::output_too_small&&result.lookup_count==full.lookup_count);
        for(const auto& entry:steps)CHECK(entry.lookup==65535&&entry.feature_mask==255&&entry.stage==255);
        CHECK(sfnt_devanagari_lookup_plan(layout,{},false,{scratch,7},{steps,64},&result)==SfntStatus::output_too_small);
        CHECK(sfnt_devanagari_form_lookup_plan(layout,{},sfnt_deva_half,{scratch,8},{steps,64},&result)==SfntStatus::ok);
        CHECK(result.lookup_count==2&&steps[0].lookup==2&&steps[1].lookup==6&&!steps[0].feature_mask&&!steps[0].stage);
        const u64 form_work=result.operations;
        for(u64 budget=0;budget<=form_work;++budget){for(auto& step:steps)step={65535,255,255};
            const auto status=sfnt_devanagari_form_lookup_plan(layout,{},sfnt_deva_half,{scratch,8},{steps,64},&result,budget);
            CHECK(result.operations<=budget&&status==(budget==form_work?SfntStatus::ok:SfntStatus::resource_limit));
            if(status!=SfntStatus::ok)for(const auto& step:steps)CHECK(step.lookup==65535);}
        CHECK(sfnt_devanagari_form_lookup_plan(layout,{},sfnt_deva_pre,{scratch,8},{steps,64},&result)==SfntStatus::ok&&!result.lookup_count);
    }
    u8 legacy[sizeof(deva_plan_fixture)];copy_bytes(legacy,deva_plan_fixture,sizeof(legacy));
    byte_store_be32(legacy+byte_load_be16(legacy+4)+2,sfnt_tag('d','e','v','a'));SfntLayout layout{};SfntLookupPlanResult result{};
    CHECK(sfnt_layout_open({legacy,sizeof(legacy)},SfntLayoutKind::substitution,&layout)==SfntStatus::ok);
    CHECK(sfnt_devanagari_lookup_plan(layout,{},false,{scratch,8},{steps,64},&result)==SfntStatus::ok&&!result.language_found&&!result.lookup_count);
    // Required half is absent from the optional FeatureIndex array. Its form
    // classification must still include it; unrelated required tags stay out.
    copy_bytes(legacy,deva_plan_fixture,sizeof(legacy));const u32 script_list=byte_load_be16(legacy+4);
    const u32 script=script_list+byte_load_be16(legacy+script_list+6),language=script+byte_load_be16(legacy+script);
    legacy[language+2]=0;legacy[language+3]=3;legacy[language+6+3*2]=0;legacy[language+7+3*2]=2;
    CHECK(sfnt_layout_open({legacy,sizeof(legacy)},SfntLayoutKind::substitution,&layout)==SfntStatus::ok);
    CHECK(sfnt_devanagari_form_lookup_plan(layout,{},sfnt_deva_half,{scratch,8},{steps,64},&result)==SfntStatus::ok);
    CHECK(result.lookup_count==2&&steps[0].lookup==2&&steps[1].lookup==6);
    print("PASS Devanagari stages, eligibility, reuse, capacity and work limits\n");return true;
}
template<class T> Slice<T> allocate(Arena& arena,u32 count) noexcept {return {arena_allocate<T>(&arena,count),count};}
bool shape_batch_mode() noexcept {wchar_t value[2]{};return GetEnvironmentVariableW(L"MAICORD_DEVANAGARI_BATCH",value,2)==1&&value[0]=='1';}
bool tamil_shape_mode() noexcept {wchar_t value[2]{};return GetEnvironmentVariableW(L"MAICORD_TAMIL_BATCH",value,2)==1&&value[0]=='1';}
bool tamil_run_mode() noexcept {wchar_t value[2]{};return GetEnvironmentVariableW(L"MAICORD_TAMIL_RUN",value,2)==1&&value[0]=='1';}
bool gujarati_run_mode() noexcept {wchar_t value[2]{};return GetEnvironmentVariableW(L"MAICORD_GUJARATI_RUN",value,2)==1&&value[0]=='1';}
bool gurmukhi_run_mode() noexcept {wchar_t value[2]{};return GetEnvironmentVariableW(L"MAICORD_GURMUKHI_RUN",value,2)==1&&value[0]=='1';}
bool gurmukhi_shape_mode() noexcept {wchar_t value[2]{};return GetEnvironmentVariableW(L"MAICORD_GURMUKHI_BATCH",value,2)==1&&value[0]=='1';}
bool telugu_run_mode() noexcept {wchar_t value[2]{};return GetEnvironmentVariableW(L"MAICORD_TELUGU_RUN",value,2)==1&&value[0]=='1';}
bool kannada_run_mode() noexcept {wchar_t value[2]{};return GetEnvironmentVariableW(L"MAICORD_KANNADA_RUN",value,2)==1&&value[0]=='1';}
bool malayalam_run_mode() noexcept {wchar_t value[2]{};return GetEnvironmentVariableW(L"MAICORD_MALAYALAM_RUN",value,2)==1&&value[0]=='1';}
bool malayalam_shape_mode() noexcept {wchar_t value[2]{};return GetEnvironmentVariableW(L"MAICORD_MALAYALAM_BATCH",value,2)==1&&value[0]=='1';}
bool kannada_shape_mode() noexcept {wchar_t value[2]{};return GetEnvironmentVariableW(L"MAICORD_KANNADA_BATCH",value,2)==1&&value[0]=='1';}
bool telugu_shape_mode() noexcept {wchar_t value[2]{};return GetEnvironmentVariableW(L"MAICORD_TELUGU_BATCH",value,2)==1&&value[0]=='1';}
bool odia_shape_mode() noexcept {wchar_t value[2]{};return GetEnvironmentVariableW(L"MAICORD_ODIA_BATCH",value,2)==1&&value[0]=='1';}
bool odia_run_mode() noexcept {wchar_t value[2]{};return GetEnvironmentVariableW(L"MAICORD_ODIA_RUN",value,2)==1&&value[0]=='1';}
bool gujarati_shape_mode() noexcept {wchar_t value[2]{};return GetEnvironmentVariableW(L"MAICORD_GUJARATI_BATCH",value,2)==1&&value[0]=='1';}
bool bengali_shape_mode() noexcept {wchar_t value[2]{};return GetEnvironmentVariableW(L"MAICORD_BENGALI_BATCH",value,2)==1&&value[0]=='1';}
bool bengali_run_mode() noexcept {wchar_t value[2]{};return GetEnvironmentVariableW(L"MAICORD_BENGALI_RUN",value,2)==1&&value[0]=='1';}
bool bengali_compact_mode() noexcept {wchar_t value[2]{};return GetEnvironmentVariableW(L"MAICORD_BENGALI_COMPACT",value,2)==1&&value[0]=='1';}
bool shape_right_to_left=false;
bool indic_run_benchmark_mode() noexcept {wchar_t value[2]{};return GetEnvironmentVariableW(L"MAICORD_INDIC_RUN_BENCH",value,2)==1&&value[0]=='1';}
bool memo_benchmark_mode() noexcept {wchar_t value[2]{};return GetEnvironmentVariableW(L"MAICORD_DEVANAGARI_MEMO_BENCH",value,2)==1&&value[0]=='1';}
struct TextOutput {u16 glyphs[1024];SfntPlacement placements[1024];u32 clusters[256];u32 count;};
struct OwnedProfile {
    NativeFontFileSlot files[1]{};NativeFontFaceSlot faces[1]{};NativeFontInstanceSlot instances[1]{};NativeRunPlanSlot plans[1]{};
    NativeFontStore fonts{{files,1},{faces,1}};NativeFontInstanceStore instance_store{&fonts,{instances,1}};NativeRunPlanStore store{&instance_store,{plans,1}};
    NativeFontHandle face{};NativeFontInstanceHandle instance{};NativeRunPlanHandle run{};NativeDevanagariPlan profile{};NativeIndicPlan indic{};bool tamil=false;Arena scratch{};
    ~OwnedProfile() noexcept {if(run.slot)native_run_plan_release(&store,run);if(instance.slot)native_font_instance_release(&instance_store,instance);
        if(face.slot)native_font_release(&fonts,face);arena_release(&scratch);}
    bool prepare_parent(const wchar_t* path,u32 index) noexcept {
        CHECK(arena_initialize(&scratch,16*1024*1024,65536));CHECK(native_font_acquire(&fonts,path,index,&face).ok());
        CHECK(native_font_instance_acquire(&instance_store,face,{},false,{},&instance).ok());
        CHECK(native_run_plan_acquire(&store,instance,&scratch,&run).ok());return true;
    }
    bool prepare_tamil(Arena& state,SfntShapeStorage probe) noexcept {
        tamil=true;arena_reset(&scratch);auto* sentinel=arena_allocate<u32>(&scratch);CHECK(sentinel);*sentinel=0x12345678;
        const auto before_state=arena_mark(&state),before_scratch=arena_mark(&scratch);const u32 references=plans[0].references;
        const auto unchanged=[&]() noexcept {return state.offset==before_state.offset&&scratch.offset==before_scratch.offset&&*sentinel==0x12345678&&
            plans[0].references==references&&!indic.run.slot&&!indic.cmap&&!indic.substitution_count&&!indic.positioning_count;};
        CHECK(native_indic_plan_prepare(store,run,SfntIndicProfile::tamil,&state,&scratch,probe,&indic,{}, {1000000,100000,0})==SfntStatus::resource_limit&&unchanged());
        CHECK(native_indic_plan_prepare(store,run,static_cast<SfntIndicProfile>(255),&state,&scratch,probe,&indic)==SfntStatus::invalid_input&&unchanged());
        CHECK(native_indic_plan_prepare(store,run,SfntIndicProfile::tamil,&state,&state,probe,&indic)==SfntStatus::invalid_input&&unchanged());
        u8 sentinel_form=255;
        CHECK(native_indic_plan_prepare(store,run,SfntIndicProfile::tamil,&state,&scratch,probe,&indic,{&sentinel_form,1})==SfntStatus::invalid_input&&unchanged()&&sentinel_form==255);
        Arena tiny{};CHECK(arena_initialize(&tiny,4096,4096,4096));struct Release {Arena* arena;~Release() noexcept {arena_release(arena);}} release{&tiny};
        CHECK(arena_allocate(&tiny,4095,1));const auto mark=arena_mark(&tiny);
        CHECK(native_indic_plan_prepare(store,run,SfntIndicProfile::tamil,&tiny,&scratch,probe,&indic)==SfntStatus::resource_limit&&unchanged()&&tiny.offset==mark.offset);
        CHECK(native_indic_plan_prepare(store,run,SfntIndicProfile::tamil,&state,&tiny,probe,&indic)==SfntStatus::output_too_small&&unchanged()&&tiny.offset==mark.offset);
        CHECK(native_indic_plan_prepare(store,run,SfntIndicProfile::tamil,&state,&scratch,{},&indic)==SfntStatus::ok);
        CHECK(indic.profile==SfntIndicProfile::tamil&&scratch.offset==before_scratch.offset&&*sentinel==0x12345678&&plans[0].references==references);
        CHECK(profile_positioning_storage(store,run,indic.substitutions(),indic.positioning(),state,before_state.offset,indic.positioning_program));
        auto invalid=indic;invalid.profile=SfntIndicProfile::devanagari;invalid.cmap=reinterpret_cast<const SfntCmap*>(1);SfntShapeRunResult result{};
        CHECK(native_tamil_run(store,invalid,{},1000,SfntShapeStorage{},&result)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
        invalid=indic;++invalid.run.generation;invalid.cmap=reinterpret_cast<const SfntCmap*>(1);
        CHECK(native_tamil_run(store,invalid,{},1000,SfntShapeStorage{},&result)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
        char text[8]{};u32 bytes=utf8_encode(0xb95,text);bytes+=utf8_encode(0xbca,text+bytes);
        CHECK(native_tamil_run(store,indic,{text,bytes},1000,probe,&result)==SfntStatus::ok&&result.glyph_count==3&&result.source_units==2);
        u16 expected_glyphs[3]{};SfntPlacement expected_placements[3]{};u32 expected_sources[2]{};
        for(u32 i=0;i<3;++i)expected_glyphs[i]=probe.glyphs[i].glyph.glyph;
        copy_bytes(expected_placements,probe.placements.data,sizeof(expected_placements));copy_bytes(expected_sources,probe.character_glyphs.data,sizeof(expected_sources));
        u16 compact_glyphs[8]{};SfntPlacement compact_placements[8]{};u32 compact_sources[8]{};
        for(u32 aliases=0;aliases<8;++aliases){NativeAsciiStorage compact{{},probe,
            aliases&1?Slice<u16>{reinterpret_cast<u16*>(probe.glyphs.data),probe.glyphs.count}:Slice<u16>{compact_glyphs,8},
            aliases&2?probe.placements:Slice<SfntPlacement>{compact_placements,8},
            aliases&4?probe.character_glyphs:Slice<u32>{compact_sources,8}};NativeAsciiResult published{};
            CHECK(native_tamil_run(store,indic,{text,bytes},1000,compact,&published)==SfntStatus::ok&&published.glyph_count==3&&published.source_units==2&&published.shaped_ranges==1);
            CHECK(bytes_equal(compact.glyphs.data,expected_glyphs,sizeof(expected_glyphs))&&bytes_equal(compact.placements.data,expected_placements,sizeof(expected_placements))&&bytes_equal(compact.character_glyphs.data,expected_sources,sizeof(expected_sources)));}
        NativeAsciiStorage compact{{},probe,{compact_glyphs,1},{compact_placements,8},{compact_sources,8}};NativeAsciiResult published{};
        CHECK(native_tamil_run(store,indic,{text,bytes},1000,compact,&published)==SfntStatus::output_too_small&&!published.glyph_count&&!published.source_units);
        compact.glyphs.count=8;bytes=utf8_encode(0x10ffff,text);
        CHECK(native_tamil_run(store,indic,{text,bytes},1000,compact,&published)==SfntStatus::unsupported_sequence&&!published.glyph_count&&!published.source_units);
        CHECK(native_tamil_run(store,indic,{},1000,compact,&published)==SfntStatus::ok&&!published.glyph_count&&!published.source_units&&!published.shaped_ranges);
        return true;
    }
    bool prepare(const wchar_t* path,u32 index,Arena& state,SfntShapeStorage probe,bool use_tamil=false) noexcept {
        CHECK(prepare_parent(path,index));
        if(use_tamil)return prepare_tamil(state,probe);
        // Caller prefixes survive all rollback paths; preparation may not reset
        // an unrelated arena prefix or silently retain another parent reference.
        arena_reset(&scratch);auto* sentinel=arena_allocate<u32>(&scratch);CHECK(sentinel);*sentinel=0x12345678;
        const auto before_state=arena_mark(&state),before_scratch=arena_mark(&scratch);const u32 references=plans[0].references;
        const auto unchanged=[&]() noexcept {return state.offset==before_state.offset&&scratch.offset==before_scratch.offset&&*sentinel==0x12345678&&plans[0].references==references&&!profile.run.slot&&!profile.cmap&&!profile.substitutions.count&&!profile.positioning.count;};
        CHECK(native_devanagari_plan_prepare(store,run,&state,&scratch,probe,&profile,{1000000,100000,0})==SfntStatus::resource_limit&&unchanged());
        CHECK(native_devanagari_plan_prepare(store,run,&state,&scratch,probe,&profile,{1,100000,1000000})==SfntStatus::resource_limit&&unchanged());
        auto short_probe=probe;short_probe.glyphs.count=1;
        CHECK(native_devanagari_plan_prepare(store,run,&state,&scratch,short_probe,&profile)==SfntStatus::output_too_small&&unchanged());
        CHECK(native_devanagari_plan_prepare(store,run,&state,&state,probe,&profile)==SfntStatus::invalid_input&&unchanged());
        Arena tiny{};CHECK(arena_initialize(&tiny,4096,4096,4096));struct Release {Arena* arena;~Release() noexcept {arena_release(arena);}} release{&tiny};
        CHECK(arena_allocate(&tiny,4095,1));const auto tiny_mark=arena_mark(&tiny);
        CHECK(native_devanagari_plan_prepare(store,run,&tiny,&scratch,probe,&profile)==SfntStatus::resource_limit&&unchanged()&&tiny.offset==tiny_mark.offset);
        CHECK(native_devanagari_plan_prepare(store,run,&state,&tiny,probe,&profile)==SfntStatus::output_too_small&&unchanged()&&tiny.offset==tiny_mark.offset);
        CHECK(native_devanagari_plan_prepare(store,run,&state,&scratch,probe,&profile)==SfntStatus::ok);
        CHECK(scratch.offset==before_scratch.offset&&*sentinel==0x12345678&&plans[0].references==references);
        CHECK(profile_positioning_storage(store,run,profile.substitutions,profile.positioning,state,before_state.offset,profile.positioning_program));
        auto stale=profile;++stale.run.generation;NativeAsciiResult result{};
        CHECK(native_devanagari_run(store,stale,{},1000,{},&result)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
        return true;
    }
    bool close() noexcept {
        CHECK(native_run_plan_release(&store,run));run={};NativeAsciiResult result{};
        if(tamil)CHECK(native_tamil_run(store,indic,{},1000,NativeAsciiStorage{},&result)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
        CHECK(native_devanagari_run(store,profile,{},1000,{},&result)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
        CHECK(native_font_instance_release(&instance_store,instance));instance={};CHECK(native_font_release(&fonts,face));face={};
        const auto stats=native_font_stats(fonts);CHECK(!stats.files&&!stats.faces&&!stats.references&&!stats.mapped_bytes);return true;
    }
};
#include "bengali_owned_plan.inc"
SfntStatus shape_text(ConstSlice<u32> scalars,SfntShapeStorage storage,TextOutput& output,const OwnedProfile& owned) noexcept {
    output.count=0;if(!scalars.count||scalars.count>256)return SfntStatus::invalid_input;
    char text[1024];u32 bytes=0,source_units=0,characters[512]{};
    for(u32 i=0;i<scalars.count;++i){output.clusters[i]=0;char encoded[4];const u8 width=utf8_encode(scalars[i],encoded);
        if(!width)return SfntStatus::invalid_input;copy_bytes(text+bytes,encoded,width);bytes+=width;}
    NativeAsciiResult result{};const auto status=native_devanagari_run(owned.store,owned.profile,{text,bytes},1000,
        {storage,{output.glyphs,1024},{output.placements,1024},{characters,512}},&result,{},0,shape_right_to_left);
    if(status!=SfntStatus::ok)return status;
    for(u32 i=0;i<scalars.count;++i){output.clusters[i]=characters[source_units];source_units+=scalars[i]>0xffff?2u:1u;}
    output.count=result.glyph_count;return SfntStatus::ok;
}
bool shape_stream(SfntShapeStorage storage,const OwnedProfile& owned) noexcept {
    char magic[8];u32 rows=0;CHECK(cluster_transfer(false,magic,8)&&bytes_equal(magic,"MDSHP1\0\0",8));
    CHECK(cluster_transfer(false,&rows,4)&&rows<=200000);CHECK(cluster_transfer(true,magic,8)&&cluster_transfer(true,&rows,4));
    u32 scalars[256];TextOutput output{};
    for(u32 row=0;row<rows;++row){u32 count=0;CHECK(cluster_transfer(false,&count,4)&&count&&count<=256);CHECK(cluster_transfer(false,scalars,count*4));
        u32 status=static_cast<u32>(shape_text({scalars,count},storage,output,owned));
        CHECK(cluster_transfer(true,&status,4)&&cluster_transfer(true,&output.count,4)&&cluster_transfer(true,output.glyphs,output.count*2));
        CHECK(cluster_transfer(true,output.clusters,count*4)&&cluster_transfer(true,output.placements,output.count*12));}
    return true;
}
#include "devanagari_memo_bench.inc"
bool tamil_shape_stream(const OwnedProfile& owned,SfntShapeStorage storage) noexcept {
    char magic[8];u32 rows=0;CHECK(cluster_transfer(false,magic,8)&&bytes_equal(magic,"MDSHP1\0\0",8));
    CHECK(cluster_transfer(false,&rows,4)&&rows<=200000);CHECK(cluster_transfer(true,magic,8)&&cluster_transfer(true,&rows,4));
    u32 scalars[256]{},clusters[256]{};u16 glyphs[1024]{};const bool run=tamil_run_mode();
    for(u32 row=0;row<rows;++row){u32 count=0;CHECK(cluster_transfer(false,&count,4)&&count&&count<=(run?256u:31u));CHECK(cluster_transfer(false,scalars,count*4));
        char text[1024]{};u32 length=0;
        for(u32 i=0;i<count;++i){char encoded[4];const u8 bytes=utf8_encode(scalars[i],encoded);CHECK(bytes&&length+bytes<=1024);copy_bytes(text+length,encoded,bytes);length+=bytes;}
        SfntShapeRunResult result{};const auto status=native_tamil_run(owned.store,owned.indic,{text,length},1000,storage,&result);
        u32 code=static_cast<u32>(status);CHECK(result.glyph_count<=1024);u32 units=0;
        for(u32 i=0;i<count;++i){clusters[i]=status==SfntStatus::ok?storage.character_glyphs[units]:0;units+=scalars[i]>0xffff?2u:1u;}
        for(u32 i=0;i<result.glyph_count;++i)glyphs[i]=storage.glyphs[i].glyph.glyph;
        CHECK(cluster_transfer(true,&code,4)&&cluster_transfer(true,&result.glyph_count,4)&&cluster_transfer(true,glyphs,result.glyph_count*2));
        CHECK(cluster_transfer(true,clusters,count*4)&&cluster_transfer(true,storage.placements.data,result.glyph_count*12));}
    return true;
}
bool bengali_shape_stream(const SfntShapePlan& plan,const SfntCmap& cmap,ConstSlice<u8> forms,ConstSlice<SfntLookupStep> steps,SfntShapeStorage storage,
    const NativeRunPlanStore* owner=nullptr,const NativeBengaliPlan* prepared=nullptr,bool gujarati=false,bool gurmukhi=false,bool odia=false,bool telugu=false,bool kannada=false,bool malayalam=false) noexcept {
    char magic[8];u32 rows=0;CHECK(cluster_transfer(false,magic,8)&&bytes_equal(magic,"MDSHP1\0\0",8));
    CHECK(cluster_transfer(false,&rows,4)&&rows<=200000);CHECK(cluster_transfer(true,magic,8)&&cluster_transfer(true,&rows,4));
    u32 scalars[256]{},clusters[256]{};u16 glyphs[1024]{};const bool run=malayalam?malayalam_run_mode():kannada?kannada_run_mode():telugu?telugu_run_mode():odia?odia_run_mode():gurmukhi?gurmukhi_run_mode():gujarati?gujarati_run_mode():bengali_run_mode(),compact=bengali_compact_mode();
    for(u32 row=0;row<rows;++row){u32 count=0;CHECK(cluster_transfer(false,&count,4)&&count&&count<=(run?256u:31u));CHECK(cluster_transfer(false,scalars,count*4));
        char text[1024]{};u32 bytes=0;if(run){CHECK(owner&&prepared);for(u32 i=0;i<count;++i){const u8 width=utf8_encode(scalars[i],text+bytes);CHECK(width);bytes+=width;}}
        SfntShapeRunResult result{};SfntStatus status=SfntStatus::ok;
        if(run&&compact){NativeAsciiResult published{};NativeAsciiStorage output{{},storage,
                {reinterpret_cast<u16*>(storage.glyphs.data),storage.glyphs.count},storage.placements,storage.character_glyphs};
            status=malayalam?native_malayalam_run(*owner,*prepared,{text,bytes},1000,output,&published,{},shape_right_to_left):kannada?native_kannada_run(*owner,*prepared,{text,bytes},1000,output,&published,{},shape_right_to_left):telugu?native_telugu_run(*owner,*prepared,{text,bytes},1000,output,&published,{},shape_right_to_left):odia?native_odia_run(*owner,*prepared,{text,bytes},1000,output,&published,{},shape_right_to_left):gurmukhi?native_gurmukhi_run(*owner,*prepared,{text,bytes},1000,output,&published,{},shape_right_to_left):gujarati?native_gujarati_run(*owner,*prepared,{text,bytes},1000,output,&published,{},shape_right_to_left):native_bengali_run(*owner,*prepared,{text,bytes},1000,output,&published,{},shape_right_to_left);
            result.glyph_count=published.glyph_count;result.source_units=published.source_units;
        }else if(malayalam)status=run?native_malayalam_run(*owner,*prepared,{text,bytes},1000,storage,&result,{},shape_right_to_left):sfnt_malayalam_shape_cluster(plan,cmap,{scalars,count},forms,1000,storage,&result,steps,{},false,0,shape_right_to_left);
        else if(kannada)status=run?native_kannada_run(*owner,*prepared,{text,bytes},1000,storage,&result,{},shape_right_to_left):sfnt_kannada_shape_cluster(plan,cmap,{scalars,count},forms,1000,storage,&result,steps,{},false,0,shape_right_to_left);
        else if(telugu)status=run?native_telugu_run(*owner,*prepared,{text,bytes},1000,storage,&result,{},shape_right_to_left):sfnt_telugu_shape_cluster(plan,cmap,{scalars,count},forms,1000,storage,&result,steps,{},false,0,shape_right_to_left);
        else if(odia)status=run?native_odia_run(*owner,*prepared,{text,bytes},1000,storage,&result,{},shape_right_to_left):sfnt_odia_shape_cluster(plan,cmap,{scalars,count},forms,1000,storage,&result,steps,{},false,0,shape_right_to_left);
        else if(gurmukhi)status=run?native_gurmukhi_run(*owner,*prepared,{text,bytes},1000,storage,&result,{},shape_right_to_left):sfnt_gurmukhi_shape_cluster(plan,cmap,{scalars,count},forms,1000,storage,&result,steps,{},false,0,shape_right_to_left);
        else if(gujarati)status=run?native_gujarati_run(*owner,*prepared,{text,bytes},1000,storage,&result,{},shape_right_to_left):sfnt_gujarati_shape_cluster(plan,cmap,{scalars,count},forms,1000,storage,&result,steps,{},false,0,shape_right_to_left);
        else status=run?native_bengali_run(*owner,*prepared,{text,bytes},1000,storage,&result,{},shape_right_to_left):sfnt_bengali_shape_cluster(plan,cmap,{scalars,count},forms,1000,storage,&result,steps,{},true,false,0,shape_right_to_left);
        u32 code=static_cast<u32>(status),units=0;CHECK(result.glyph_count<=1024);
        for(u32 i=0;i<count;++i){clusters[i]=status==SfntStatus::ok?storage.character_glyphs[units]:0;units+=scalars[i]>0xffff?2u:1u;}
        for(u32 i=0;i<result.glyph_count;++i)glyphs[i]=run&&compact?reinterpret_cast<const u16*>(storage.glyphs.data)[i]:storage.glyphs[i].glyph.glyph;
        CHECK(cluster_transfer(true,&code,4)&&cluster_transfer(true,&result.glyph_count,4)&&cluster_transfer(true,glyphs,result.glyph_count*2));
        CHECK(cluster_transfer(true,clusters,count*4)&&cluster_transfer(true,storage.placements.data,result.glyph_count*12));}
    return true;
}
#include "indic_run_bench.inc"
bool font_case(ConstSlice<u8> bytes,u32 index,Arena& arena,const wchar_t* path,bool profile_only) noexcept {
    if(tamil_profile)return tamil_font_plan(bytes,index);
    wchar_t cluster_mode[2]{};const bool telugu_cluster_mode=GetEnvironmentVariableW(L"MAICORD_TELUGU_CLUSTER_STREAM",cluster_mode,2)==1&&cluster_mode[0]==L'1';
    const bool kannada_cluster_mode=GetEnvironmentVariableW(L"MAICORD_KANNADA_CLUSTER_STREAM",cluster_mode,2)==1&&cluster_mode[0]==L'1';
    const bool malayalam_cluster_mode=GetEnvironmentVariableW(L"MAICORD_MALAYALAM_CLUSTER_STREAM",cluster_mode,2)==1&&cluster_mode[0]==L'1';
    const bool malayalam=malayalam_shape_mode(),kannada=kannada_shape_mode()||kannada_run_mode(),telugu=telugu_shape_mode()||telugu_run_mode(),odia=odia_shape_mode()||odia_run_mode(),gurmukhi=gurmukhi_shape_mode()||gurmukhi_run_mode(),gujarati=gujarati_shape_mode()||gujarati_run_mode(),batch=shape_batch_mode(),tamil=tamil_shape_mode()||tamil_run_mode()||tamil_owned_profile,bengali=bengali_shape_mode()||bengali_run_mode(),benchmark=memo_benchmark_mode(),quiet=malayalam||malayalam_cluster_mode||kannada||kannada_cluster_mode||telugu||batch||profile_only||telugu_cluster_mode||benchmark||tamil||bengali||gujarati||gurmukhi||odia;
    const auto profile=malayalam_profile||malayalam?SfntIndicProfile::malayalam:kannada_profile||kannada?SfntIndicProfile::kannada:telugu_profile||telugu?SfntIndicProfile::telugu:odia_profile||odia?SfntIndicProfile::odia:gurmukhi_profile||gurmukhi?SfntIndicProfile::gurmukhi:gujarati_profile||gujarati?SfntIndicProfile::gujarati:bengali_profile||bengali?SfntIndicProfile::bengali:tamil?SfntIndicProfile::tamil:SfntIndicProfile::devanagari;
    arena_reset(&arena);SfntFace face{};SfntCmap cmap{};SfntHorizontalMetrics metrics{};SfntGdef gdef{};ConstSlice<u8> data{};
    CHECK(sfnt_open(bytes,index,&face)==SfntStatus::ok&&sfnt_cmap_open(face,&cmap)==SfntStatus::ok&&sfnt_horizontal_metrics_open(face,&metrics)==SfntStatus::ok);
    CHECK(sfnt_table(face,sfnt_tag('G','D','E','F'),&data)==SfntStatus::ok&&sfnt_gdef_open(data,metrics.glyph_count,&gdef)==SfntStatus::ok);
    SfntGsubProgram substitutions{};SfntGposProgram positioning{};SfntLayout substitution_layout{};SfntLookupStep steps[4096]{};u32 count=0;u16 roots[4096]{},scratch[4096]{};u32 root_count=0;
    for(u32 kind=0;kind<2;++kind){SfntLayout layout{};SfntLookupPlanResult result{};SfntLookupStep selected[4096];
        CHECK(sfnt_table(face,kind?sfnt_tag('G','P','O','S'):sfnt_tag('G','S','U','B'),&data)==SfntStatus::ok);
        CHECK(sfnt_layout_open(data,kind?SfntLayoutKind::positioning:SfntLayoutKind::substitution,&layout)==SfntStatus::ok);
        CHECK(sfnt_indic_lookup_plan(layout,{},profile,false,{scratch,4096},{selected,4096},&result)==SfntStatus::ok&&result.language_found);
        if(!quiet){print("{\"face\":");number(index);print(",\"kind\":");number(kind);print(",\"steps\":[");
        for(u32 i=0;i<result.lookup_count;++i){if(i)print(",");print("[");number(selected[i].lookup);print(",");number(selected[i].feature_mask);print(",");number(selected[i].stage);print("]");}print("]}\n");}
        if(!kind){substitution_layout=layout;count=result.lookup_count;copy_bytes(steps,selected,count*sizeof(SfntLookupStep));SfntGsubProgramRequirements n{};
            CHECK(sfnt_gsub_program_requirements(layout,&n)==SfntStatus::ok);
            CHECK(sfnt_gsub_program_prepare(layout,gdef,metrics.glyph_count,{allocate<SfntGsubExecutableLookup>(arena,n.lookups),allocate<SfntGsubExecutableSubtable>(arena,n.subtables),allocate<SfntCoverage>(arena,n.coverages),allocate<SfntClassDef>(arena,n.classes)},&substitutions)==SfntStatus::ok);
        }else{root_count=result.lookup_count;for(u32 i=0;i<root_count;++i)roots[i]=selected[i].lookup;SfntGposProgramRequirements n{};
            CHECK(sfnt_gpos_program_requirements(layout,&n)==SfntStatus::ok);
            CHECK(sfnt_gpos_program_prepare(layout,gdef,metrics.glyph_count,{allocate<SfntGposExecutableLookup>(arena,n.lookups),allocate<SfntGposSubtable>(arena,n.basic),allocate<SfntGposAttachment>(arena,n.attachments),allocate<SfntContext>(arena,n.contexts),allocate<SfntCoverage>(arena,n.coverages),allocate<SfntClassDef>(arena,n.classes)},&positioning)==SfntStatus::ok);}
    }
    SfntShapePlan plan{};plan.substitutions=&substitutions;plan.positioning=&positioning;plan.positioning_roots={roots,root_count};plan.gdef=gdef;plan.units_per_em=metrics.units_per_em;plan.metric_context=&metrics;plan.refresh_substitution_classes=true;
    plan.advance=+[](void* p,u16 glyph,double* value) noexcept {SfntGlyphMetrics m{};const auto status=sfnt_horizontal_metrics(*static_cast<SfntHorizontalMetrics*>(p),glyph,&m);*value=m.advance;return status;};
    SfntComponentGlyph glyphs[32]{},replacement[32]{};SfntComponentNode nodes[64]{};SfntComponentUndo journal[512]{};u64 bits[2]{},chains[2]{};
    u32 characters[32]{},targets[32]{},positions[128]{},rebased[128]{},consumed[32]{},gp_positions[128]{};u8 flags[32]{};
    SfntGsubExecutionFrame frames[16]{};SfntGposExecutionFrame gp_frames[16]{};SfntCursiveChainNode cursive[32]{};
    double advances[32]{},prefix[32]{};SfntPosition states[32]{};SfntPlacement placements[32]{};SfntMarkAssociation associations[32]{};
    SfntShapeStorage storage{{glyphs,32},{nodes,64},{bits,2},{characters,32},{targets,32},{associations,32},{advances,32},{flags,32},{states,32},{placements,32},
        {{frames,16},{positions,128},{rebased,128},{consumed,32},{reinterpret_cast<u8*>(replacement),sizeof(replacement)},{journal,512}},{{gp_frames,16},{gp_positions,128},{chains,2},{prefix,32},{cursive,32}}};
    if(malayalam||kannada||telugu||odia||gurmukhi_run_mode()||tamil_run_mode()||bengali_run_mode()||gujarati_run_mode())storage={allocate<SfntComponentGlyph>(arena,1024),allocate<SfntComponentNode>(arena,2048),allocate<u64>(arena,16),
            allocate<u32>(arena,512),allocate<u32>(arena,1024),allocate<SfntMarkAssociation>(arena,1024),allocate<double>(arena,1024),
            allocate<u8>(arena,1024),allocate<SfntPosition>(arena,1024),allocate<SfntPlacement>(arena,1024),
            {allocate<SfntGsubExecutionFrame>(arena,32),allocate<u32>(arena,4096),allocate<u32>(arena,4096),allocate<u32>(arena,1024),
             allocate<u8>(arena,1024*sizeof(SfntComponentGlyph)),allocate<SfntComponentUndo>(arena,8192)},
            {allocate<SfntGposExecutionFrame>(arena,32),allocate<u32>(arena,4096),allocate<u64>(arena,16),allocate<double>(arena,1024),allocate<SfntCursiveChainNode>(arena,1024)}};
    if(tamil){OwnedProfile owned;CHECK(owned.prepare(path,index,arena,storage,true));
        CHECK(owned.indic.substitution_count==count&&bytes_equal(owned.indic.substitution_steps,steps,count*sizeof(SfntLookupStep)));
        CHECK(owned.indic.positioning_count==root_count&&bytes_equal(owned.indic.positioning_roots,roots,root_count*sizeof(u16)));
        if(profile_only){print("PASS Tamil owned profile face=");number(index);print(" root_bytes=");number(count*sizeof(SfntLookupStep)+root_count*sizeof(u16));print(" profile_bytes=");number(sizeof(NativeIndicPlan));print("\n");return owned.close();}
        CHECK(tamil_shape_stream(owned,storage));return owned.close();}
    SfntShapeStagePolicy policy{nullptr,+[](void*,u16,SfntGlyphBuffer*,SfntClusterMap*,SfntComponentForest*,SfntGsubExecutionScratch,u64,u64* used) noexcept {*used=0;return SfntStatus::ok;},13};
    const SfntDevanagariForm forms[]{sfnt_deva_reph,sfnt_deva_half,sfnt_deva_pre,sfnt_deva_below,sfnt_deva_post};u8 font_forms[128]{};
    const u32 block=sfnt_indic_script(profile)->block_first;
    u16 halant=0;CHECK(sfnt_cmap_glyph(cmap,block+0x4d,0,&halant)==SfntStatus::ok&&halant);
    for(auto form:forms){SfntLookupStep selected[4096]{};SfntLookupPlanResult scheduled{};
        CHECK(sfnt_indic_form_lookup_plan(substitution_layout,{},profile,form,{scratch,4096},{selected,4096},&scheduled)==SfntStatus::ok);
        if(!quiet){print("{\"face\":");number(index);print(",\"form\":");number(form);print(",\"cases\":[");}bool comma=false;
        for(u32 scalar=block;scalar<block+128;++scalar){if(unicode_properties(scalar)->indic_syllabic_category()!=UnicodeIndicSyllabicCategory::consonant)continue;
            u16 glyph=0;CHECK(sfnt_cmap_glyph(cmap,scalar,0,&glyph)==SfntStatus::ok);if(!glyph)continue;SfntDevanagariFormResult result{};
            CHECK(sfnt_devanagari_probe_form(substitutions,{selected,scheduled.lookup_count},glyph,halant,form,{glyphs,32},storage.substitutions,&result)==SfntStatus::ok);
            if(result.formed)font_forms[scalar-block]|=static_cast<u8>(form);
            if(!quiet){if(comma)print(",");comma=true;print("[");number(scalar);print(",");number(result.formed);print(",");number(result.glyph);print(",");number(result.operations);print("]");}}
        if(!quiet)print("]}\n");}
    indic_form_defaults(profile,font_forms);
    if(profile==SfntIndicProfile::malayalam&&(font_forms[0x2f]&indic_default_position)){SfntLookupPlanResult reordered{};
        CHECK(sfnt_indic_lookup_plan(substitution_layout,{},profile,false,{scratch,4096},{steps,4096},&reordered,1000000,true)==SfntStatus::ok);count=reordered.lookup_count;}
    if(kannada_profile&&kannada_cluster_mode){CHECK(cluster_stream(SfntIndicProfile::kannada,{font_forms,128}));return true;}
    if(malayalam_profile&&malayalam_cluster_mode){CHECK(cluster_stream(SfntIndicProfile::malayalam,{font_forms,128}));return true;}
    wchar_t telugu_stream[2]{};if(telugu_profile&&GetEnvironmentVariableW(L"MAICORD_TELUGU_CLUSTER_STREAM",telugu_stream,2)==1&&telugu_stream[0]==L'1'){CHECK(cluster_stream(SfntIndicProfile::telugu,{font_forms,128}));return true;}
    if(malayalam){if(!malayalam_run_mode())return bengali_shape_stream(plan,cmap,{font_forms,128},{steps,count},storage,nullptr,nullptr,false,false,false,false,false,true);
        OwnedProfile owned;CHECK(owned.prepare_parent(path,index));NativeMalayalamPlan prepared{};
        CHECK(native_malayalam_plan_prepare(owned.store,owned.run,&arena,&owned.scratch,storage,&prepared)==SfntStatus::ok);
        CHECK(bytes_equal(prepared.forms,font_forms,128));
        CHECK(bengali_shape_stream(plan,cmap,{font_forms,128},{steps,count},storage,&owned.store,&prepared,false,false,false,false,false,true));return owned.close();}
    if(kannada){if(!kannada_run_mode())return bengali_shape_stream(plan,cmap,{font_forms,128},{steps,count},storage,nullptr,nullptr,false,false,false,false,true);
        OwnedProfile owned;CHECK(owned.prepare_parent(path,index));NativeKannadaPlan prepared{};
        CHECK(native_kannada_plan_prepare(owned.store,owned.run,&arena,&owned.scratch,storage,&prepared)==SfntStatus::ok);
        CHECK(bytes_equal(prepared.forms,font_forms,128));
        CHECK(bengali_shape_stream(plan,cmap,{font_forms,128},{steps,count},storage,&owned.store,&prepared,false,false,false,false,true));return owned.close();}
    if(telugu){if(!telugu_run_mode())return bengali_shape_stream(plan,cmap,{font_forms,128},{steps,count},storage,nullptr,nullptr,false,false,false,true);
        OwnedProfile owned;CHECK(owned.prepare_parent(path,index));NativeTeluguPlan prepared{};
        CHECK(native_telugu_plan_prepare(owned.store,owned.run,&arena,&owned.scratch,storage,&prepared)==SfntStatus::ok);
        CHECK(bytes_equal(prepared.forms,font_forms,128));
        CHECK(bengali_shape_stream(plan,cmap,{font_forms,128},{steps,count},storage,&owned.store,&prepared,false,false,false,true));return owned.close();}
    if(odia){if(!odia_run_mode())return bengali_shape_stream(plan,cmap,{font_forms,128},{steps,count},storage,nullptr,nullptr,false,false,true);
        OwnedProfile owned;CHECK(owned.prepare_parent(path,index));NativeOdiaPlan prepared{};
        CHECK(native_odia_plan_prepare(owned.store,owned.run,&arena,&owned.scratch,storage,&prepared)==SfntStatus::ok);
        CHECK(bytes_equal(prepared.forms,font_forms,128));
        CHECK(bengali_shape_stream(plan,cmap,{font_forms,128},{steps,count},storage,&owned.store,&prepared,false,false,true));return owned.close();}
    if(gurmukhi){if(!gurmukhi_run_mode())return bengali_shape_stream(plan,cmap,{font_forms,128},{steps,count},storage,nullptr,nullptr,false,true);
        OwnedProfile owned;CHECK(owned.prepare_parent(path,index));NativeGurmukhiPlan prepared{};
        CHECK(native_gurmukhi_plan_prepare(owned.store,owned.run,&arena,&owned.scratch,storage,&prepared)==SfntStatus::ok);
        CHECK(bytes_equal(prepared.forms,font_forms,128));
        CHECK(bengali_shape_stream(plan,cmap,{font_forms,128},{steps,count},storage,&owned.store,&prepared,false,true));return owned.close();}
    if(gujarati){if(!gujarati_run_mode())return bengali_shape_stream(plan,cmap,{font_forms,128},{steps,count},storage,nullptr,nullptr,true);
        OwnedProfile owned;CHECK(owned.prepare_parent(path,index));NativeGujaratiPlan prepared{};
        CHECK(native_gujarati_plan_prepare(owned.store,owned.run,&arena,&owned.scratch,storage,&prepared)==SfntStatus::ok);
        CHECK(bytes_equal(prepared.forms,font_forms,128));
        CHECK(indic_run_benchmark_mode()?indic_run_benchmark(owned.store,prepared,storage):bengali_shape_stream(plan,cmap,{font_forms,128},{steps,count},storage,&owned.store,&prepared,true));return owned.close();}
    if(bengali_profile||gujarati_profile||gurmukhi_profile||odia_profile||telugu_profile||kannada_profile||malayalam_profile)return bengali_owned_plan(path,index,arena,storage,{steps,count},{roots,root_count},font_forms,profile);
    if(bengali){if(!bengali_run_mode())return bengali_shape_stream(plan,cmap,{font_forms,128},{steps,count},storage);
        OwnedProfile owned;CHECK(owned.prepare_parent(path,index));NativeBengaliPlan prepared{};
        CHECK(native_bengali_plan_prepare(owned.store,owned.run,&arena,&owned.scratch,storage,&prepared)==SfntStatus::ok);
        CHECK(bytes_equal(prepared.forms,font_forms,128));
        CHECK(indic_run_benchmark_mode()?indic_run_benchmark(owned.store,prepared,storage):bengali_shape_stream(plan,cmap,{font_forms,128},{steps,count},storage,&owned.store,&prepared));return owned.close();}
    OwnedProfile owned;CHECK(owned.prepare(path,index,arena,storage));
    if(benchmark){CHECK(memo_benchmark(storage,owned));return owned.close();}
    CHECK(owned.profile.substitutions.count==count&&bytes_equal(owned.profile.substitutions.data,steps,count*sizeof(SfntLookupStep)));
    CHECK(owned.profile.positioning.count==root_count&&bytes_equal(owned.profile.positioning.data,roots,root_count*sizeof(u16)));
    CHECK(bytes_equal(owned.profile.forms,font_forms,128));
    if(profile_only){print("PASS Devanagari profile face=");number(index);print(" root_bytes=");number(count*sizeof(SfntLookupStep)+root_count*sizeof(u16));
        print(" profile_bytes=");number(sizeof(NativeDevanagariPlan));print("\n");return owned.close();}
    if(batch){CHECK(shape_stream(storage,owned));return owned.close();}
    for(u32 sample=0;sample<sizeof(deva_shape_samples)/sizeof(deva_shape_samples[0]);++sample){
        u32 scalars[256]{},length=0;const auto* text=deva_shape_samples[sample];while(text[length]){CHECK(length<256);scalars[length]=text[length];++length;}
        TextOutput output{};const auto shaped=shape_text({scalars,length},storage,output,owned);
        const auto& output_glyphs=output.glyphs;const auto& output_placements=output.placements;const auto& output_clusters=output.clusters;const u32 written=output.count;
        print("{\"face\":");number(index);print(",\"ordered\":");number(sample);print(",\"status\":");number(static_cast<u8>(shaped));print(",\"scalars\":[");
        for(u32 i=0;i<length;++i){if(i)print(",");number(scalars[i]);}print("],\"glyphs\":[");
        for(u32 i=0;i<written;++i){if(i)print(",");number(output_glyphs[i]);}print("],\"clusters\":[");
        for(u32 i=0;i<length;++i){if(i)print(",");number(output_clusters[i]);}print("],\"placements\":[");
        for(u32 i=0;i<written;++i){if(i)print(",");u32 values[3];copy_bytes(values,&output_placements[i],12);print("[");for(u32 j=0;j<3;++j){if(j)print(",");number(values[j]);}print("]");}print("]}\n");}
    for(u32 sample=0;sample<2;++sample){const u32 scalars[]{0x915,0x93c};SfntClusterGlyph input[2]{};const u8 mask=static_cast<u8>(sfnt_deva_conjunct|(sample?sfnt_deva_nukta:0));
        for(u32 i=0;i<=sample;++i){u16 glyph=0;CHECK(sfnt_cmap_glyph(cmap,scalars[i],0,&glyph)==SfntStatus::ok&&glyph);input[i]={{glyph,sfnt_gdef_glyph_class(gdef,glyph),mask},i};}
        const u32 boundary[]{0};SfntShapeRunResult result{};CHECK(sfnt_shape_run(plan,{input,sample+1},sample+1,{boundary,1},false,1000,storage,&result,{}, {steps,count},{},{},nullptr,&policy)==SfntStatus::ok);
        print("{\"face\":");number(index);print(",\"sample\":");number(sample);print(",\"glyphs\":[");
        for(u32 i=0;i<result.glyph_count;++i){if(i)print(",");number(glyphs[i].glyph.glyph);}print("],\"placements\":[");
        for(u32 i=0;i<result.glyph_count;++i){if(i)print(",");u32 value[3];copy_bytes(value,&placements[i],12);print("[");for(u32 j=0;j<3;++j){if(j)print(",");number(value[j]);}print("]");}print("]}\n");
    }
    return owned.close();
}
bool font_file(const wchar_t* path,bool profile_only=false) noexcept {
    wchar_t direction[2]{};shape_right_to_left=GetEnvironmentVariableW(L"MAICORD_DEVANAGARI_RTL",direction,2)==1&&direction[0]=='1';
    HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);CHECK(file!=INVALID_HANDLE_VALUE);
    struct Close {HANDLE h;~Close() noexcept {CloseHandle(h);}} close{file};LARGE_INTEGER size{};CHECK(GetFileSizeEx(file,&size)&&size.QuadPart>0&&size.QuadPart<64*1024*1024);
    HANDLE mapping=CreateFileMappingW(file,nullptr,PAGE_READONLY,0,0,nullptr);CHECK(mapping);Close close_mapping{mapping};
    const auto* data=static_cast<const u8*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,0));CHECK(data);struct Unmap {const void* p;~Unmap() noexcept {UnmapViewOfFile(p);}} unmap{data};
    Arena arena{};CHECK(arena_initialize(&arena,32*1024*1024,65536));struct Release {Arena* a;~Release() noexcept {arena_release(a);}} release{&arena};
    const ConstSlice<u8> bytes{data,static_cast<u32>(size.QuadPart)};SfntFace first{};CHECK(sfnt_open(bytes,0,&first)==SfntStatus::ok&&first.face_count<=32);
    if(kannada_run_mode()||kannada_shape_mode()||telugu_run_mode()||telugu_shape_mode()||odia_shape_mode()||odia_run_mode()||gurmukhi_run_mode()||gurmukhi_shape_mode()||gujarati_run_mode()||gujarati_shape_mode()||shape_batch_mode()||tamil_shape_mode()||tamil_run_mode()||bengali_shape_mode()||bengali_run_mode())CHECK(first.face_count==1);
    for(u32 i=0;i<first.face_count;++i)CHECK(font_case(bytes,i,arena,path,profile_only));return true;
}
bool run() noexcept {
    const wchar_t* p=GetCommandLineW();bool quoted=false;while(*p){if(*p=='"')quoted=!quoted;else if(!quoted&&(*p==' '||*p=='\t'))break;++p;}while(*p==' '||*p=='\t')++p;
    if(!*p)return units();quoted=*p=='"';if(quoted)++p;wchar_t path[1024]{};u32 n=0;
    while(*p&&!(quoted?*p=='"':*p==' '||*p=='\t')){CHECK(n+1<1024);path[n++]=*p++;}if(quoted){CHECK(*p=='"');++p;}while(*p==' '||*p=='\t')++p;CHECK(n&&!*p);
    if(bytes_equal(path,L"--clusters",sizeof(L"--clusters")))return cluster_stream();
    if(bytes_equal(path,L"--gujarati-shape",sizeof(L"--gujarati-shape")))return gujarati_shape_units();
    if(bytes_equal(path,L"--gujarati-input",sizeof(L"--gujarati-input")))return gujarati_input_units();
    if(bytes_equal(path,L"--gujarati-clusters",sizeof(L"--gujarati-clusters")))return cluster_stream(SfntIndicProfile::gujarati);
    if(bytes_equal(path,L"--gurmukhi-shape",sizeof(L"--gurmukhi-shape")))return gurmukhi_shape_units();
    if(bytes_equal(path,L"--gurmukhi-input",sizeof(L"--gurmukhi-input")))return gurmukhi_input_units();
    if(bytes_equal(path,L"--gurmukhi-clusters",sizeof(L"--gurmukhi-clusters")))return cluster_stream(SfntIndicProfile::gurmukhi);
    if(bytes_equal(path,L"--gurmukhi-plan",sizeof(L"--gurmukhi-plan")))return tamil_plan_units(SfntIndicProfile::gurmukhi);
    if(bytes_equal(path,L"--telugu-clusters",sizeof(L"--telugu-clusters"))){telugu_profile=true;const auto path_length=GetEnvironmentVariableW(L"MAICORD_TELUGU_FONT",path,1024);CHECK(path_length&&path_length<1024);return font_file(path);}
    if(bytes_equal(path,L"--telugu-shape",sizeof(L"--telugu-shape")))return telugu_shape_units();
    if(bytes_equal(path,L"--kannada-clusters",sizeof(L"--kannada-clusters"))){kannada_profile=true;const auto path_length=GetEnvironmentVariableW(L"MAICORD_KANNADA_FONT",path,1024);CHECK(path_length&&path_length<1024);return font_file(path);}
    if(bytes_equal(path,L"--malayalam-clusters",sizeof(L"--malayalam-clusters"))||bytes_equal(path,L"--malayalam-font-profile",sizeof(L"--malayalam-font-profile"))){malayalam_profile=true;const auto path_length=GetEnvironmentVariableW(L"MAICORD_MALAYALAM_FONT",path,1024);CHECK(path_length&&path_length<1024);return font_file(path);}
    if(bytes_equal(path,L"--malayalam-shape",sizeof(L"--malayalam-shape")))return malayalam_shape_units();
    if(bytes_equal(path,L"--kannada-shape",sizeof(L"--kannada-shape")))return kannada_shape_units();
    if(bytes_equal(path,L"--malayalam-plan",sizeof(L"--malayalam-plan")))return tamil_plan_units(SfntIndicProfile::malayalam)&&malayalam_presentation_units();
    if(bytes_equal(path,L"--malayalam-input",sizeof(L"--malayalam-input")))return malayalam_input_units();
    if(bytes_equal(path,L"--kannada-plan",sizeof(L"--kannada-plan")))return tamil_plan_units(SfntIndicProfile::kannada);
    if(bytes_equal(path,L"--kannada-input",sizeof(L"--kannada-input")))return kannada_input_units();
    if(bytes_equal(path,L"--telugu-plan",sizeof(L"--telugu-plan")))return tamil_plan_units(SfntIndicProfile::telugu);
    if(bytes_equal(path,L"--telugu-input",sizeof(L"--telugu-input")))return telugu_input_units();
    if(bytes_equal(path,L"--odia-plan",sizeof(L"--odia-plan")))return tamil_plan_units(SfntIndicProfile::odia);
    if(bytes_equal(path,L"--odia-input",sizeof(L"--odia-input")))return odia_input_units();
    if(bytes_equal(path,L"--odia-shape",sizeof(L"--odia-shape")))return odia_shape_units();
    if(bytes_equal(path,L"--odia-clusters",sizeof(L"--odia-clusters")))return cluster_stream(SfntIndicProfile::odia);
    if(bytes_equal(path,L"--gujarati-plan",sizeof(L"--gujarati-plan")))return tamil_plan_units(SfntIndicProfile::gujarati);
    if(bytes_equal(path,L"--bengali-clusters",sizeof(L"--bengali-clusters")))return cluster_stream(SfntIndicProfile::bengali);
    if(bytes_equal(path,L"--bengali-input",sizeof(L"--bengali-input")))return bengali_input_units();
    if(bytes_equal(path,L"--bengali-shape",sizeof(L"--bengali-shape")))return bengali_shape_units();
    if(bytes_equal(path,L"--bengali-plan",sizeof(L"--bengali-plan")))return bengali_plan_units();
    if(bytes_equal(path,L"--tamil-plan",sizeof(L"--tamil-plan")))return tamil_plan_units();
    if(bytes_equal(path,L"--tamil-input",sizeof(L"--tamil-input")))return tamil_input_units();
    tamil_profile=bytes_equal(path,L"--tamil-profile",sizeof(L"--tamil-profile"));
    bengali_profile=bytes_equal(path,L"--bengali-profile",sizeof(L"--bengali-profile"));
    gurmukhi_profile=bytes_equal(path,L"--gurmukhi-profile",sizeof(L"--gurmukhi-profile"));
    malayalam_profile=bytes_equal(path,L"--malayalam-profile",sizeof(L"--malayalam-profile"));
    kannada_profile=bytes_equal(path,L"--kannada-profile",sizeof(L"--kannada-profile"));
    telugu_profile=bytes_equal(path,L"--telugu-profile",sizeof(L"--telugu-profile"));
    odia_profile=bytes_equal(path,L"--odia-profile",sizeof(L"--odia-profile"));
    gujarati_profile=bytes_equal(path,L"--gujarati-profile",sizeof(L"--gujarati-profile"));
    tamil_owned_profile=bytes_equal(path,L"--tamil-owned-profile",sizeof(L"--tamil-owned-profile"));
    if(tamil_profile||tamil_owned_profile||bengali_profile||gujarati_profile||gurmukhi_profile||odia_profile||telugu_profile||kannada_profile||malayalam_profile||bytes_equal(path,L"--profile",sizeof(L"--profile"))){const wchar_t suffix[]=L"/Fonts/Nirmala.ttc";const u32 length=GetWindowsDirectoryW(path,1024);
        CHECK(length&&length<1024-sizeof(suffix)/sizeof(wchar_t));copy_bytes(path+length,suffix,sizeof(suffix));return font_file(path,!bengali_profile&&!gujarati_profile&&!gurmukhi_profile&&!odia_profile&&!telugu_profile&&!kannada_profile&&!malayalam_profile);}
    return font_file(path);
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_devanagari_plan_entry() noexcept {ExitProcess(maicord::run()?0u:1u);}
