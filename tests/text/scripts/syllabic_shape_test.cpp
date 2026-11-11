// The Khmer, Myanmar and Universal Shaping Engine shapers and the Hangul plan (MaiCordSyllabicShapeTests), checked
// against HarfBuzz (tests/generated/syllabic_shape_fixture.inc, tools/font_reference/syllabic_fixtures.py). Without
// MAICORD_SYLLABIC_OWNED the test runs the argument and Hangul plan units and replays the cluster-start corpora of
// Sinhala, Khmer, Myanmar and Tibetan. With
// MAICORD_SYLLABIC_OWNED=<script> it shapes that script on its Windows system font: plan preparation accounting and
// failure atomicity, the compact output aliasing every input, malformed UTF-8 and limits, HarfBuzz's output for the
// sample strings, and a font HarfBuzz shapes the script in with its default shaper or without the script's glyphs.
#include "maicord/text/scripts/syllabic_shape.hpp"
#include "maicord/text/shaping/cluster_run.hpp"
#include "maicord/text/opentype/sfnt.hpp"
#include "maicord/text/fonts/store.hpp"
#include "maicord/core/arena.hpp"
#include "maicord/core/byte_order.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "../../support/test_output.hpp"
namespace maicord {namespace {
#include "../../generated/syllabic_shape_fixture.inc"
u32 checks=0;
bool failed(u32 line) noexcept {test_write_text("FAIL syllabic line ");test_write_u64(line);test_write_text("\n");return false;}
#define CHECK(x) do {++checks;if(!(x))return failed(__LINE__);} while(false)
#include "../../support/script_shape_owner.inc"
struct Corpus {SfntSyllabicScript script;const char* name;const u32* words;u32 count,cases;};
template<u32 N> constexpr Corpus corpus(SfntSyllabicScript script,const char* name,const u32 (&words)[N],u32 cases) noexcept {return {script,name,words,N,cases};}
constexpr Corpus corpora[]{corpus(SfntSyllabicScript::sinhala,"Sinhala",sinhala_clusters,sinhala_cluster_cases),
    corpus(SfntSyllabicScript::khmer,"Khmer",khmer_clusters,khmer_cluster_cases),
    corpus(SfntSyllabicScript::myanmar,"Myanmar",myanmar_clusters,myanmar_cluster_cases),
    corpus(SfntSyllabicScript::tibetan,"Tibetan",tibetan_clusters,tibetan_cluster_cases)};
// Each vector's cluster starts equal HarfBuzz's, except the pinned divergent vectors, which must still differ.
bool cluster_corpus(const Corpus& c) noexcept {
    u32 scalars[256]{};u8 starts[256]{};u32 first=0,count=0,done=0,pinned=0,wrong=0;
    for(u32 i=0;i<c.count;++i){const u32 word=c.words[i];CHECK(count<256);scalars[count++]=word&0x1fffffu;
        if(!(word&0x40000000u))continue;
        CHECK(sfnt_syllabic_boundaries(c.script,{scalars,count},{starts,count})==SfntStatus::ok);
        bool same=true;for(u32 j=0;j<count;++j)same&=starts[j]==(c.words[first+j]>>31);
        // Each wrong case: its index, then scalar:native start:HarfBuzz start, in decimal.
        if(same==((word&0x20000000u)!=0)&&++wrong<=40){test_write_text(c.name);test_write_text(same?" pinned divergent case agrees, ":" cluster case ");
            test_write_u64(done);for(u32 j=0;j<count;++j){test_write_text(" ");test_write_u64(scalars[j]);test_write_text(":");test_write_u64(starts[j]);
                test_write_text(":");test_write_u64(c.words[first+j]>>31);}test_write_text("\n");}
        pinned+=!same;++done;count=0;first=i+1;}
    CHECK(!count&&done==c.cases&&!wrong);
    test_write_text("PASS ");test_write_text(c.name);test_write_text(" HarfBuzz cluster starts, ");test_write_u64(done);
    test_write_text(" vectors, ");test_write_u64(pinned);test_write_text(" pinned divergent\n");return true;
}
bool boundary_units() noexcept {
    const u32 ka[]{0x1780,0x17d2,0x179a,0x17bb};u8 starts[4]{};
    CHECK(sfnt_syllabic_boundaries(static_cast<SfntSyllabicScript>(4),{ka,4},{starts,4})==SfntStatus::invalid_input);
    CHECK(sfnt_syllabic_boundaries(SfntSyllabicScript::khmer,{ka,4},{starts,3})==SfntStatus::output_too_small);
    CHECK(sfnt_syllabic_boundaries(SfntSyllabicScript::khmer,{ka,4},{starts,4},63)==SfntStatus::resource_limit);
    const u32 surrogate[]{0xd800};CHECK(sfnt_syllabic_boundaries(SfntSyllabicScript::khmer,{surrogate,1},{starts,1})==SfntStatus::invalid_input);
    CHECK(sfnt_syllabic_boundaries(SfntSyllabicScript::khmer,{ka,4},{starts,4})==SfntStatus::ok&&starts[0]==1&&!starts[1]&&!starts[2]&&!starts[3]);
    test_write_text("PASS syllabic boundary argument units\n");return true;
}
void put(u8* p,u32 at,u16 v) noexcept {p[at]=static_cast<u8>(v>>8);p[at+1]=static_cast<u8>(v);}
// hb-ot-shaper-hangul's plan on a first-party GSUB (hang: ccmp {3,1}, ljmo {2,1}, tjmo {0,2}, vjmo {3,2}): one stage
// after rvrn; a lookup is masked by the union of its masked features unless an unmasked feature (or the required one,
// here tjmo) shares it. Budget sweeps and short output leave the output untouched.
bool hangul_plan_units() noexcept {
    u8 b[110]{};put(b,0,1);put(b,4,10);put(b,6,36);put(b,8,94);
    put(b,10,1);byte_store_be32(b+12,sfnt_tag('h','a','n','g'));put(b,16,8);put(b,18,4);put(b,24,0xffff);put(b,26,4);
    const u32 tags[]{sfnt_tag('c','c','m','p'),sfnt_tag('l','j','m','o'),sfnt_tag('t','j','m','o'),sfnt_tag('v','j','m','o')};
    const u16 refs[]{3,1,2,1,0,2,3,2};put(b,36,4);
    for(u32 i=0;i<4;++i){put(b,28+i*2,static_cast<u16>(i));byte_store_be32(b+38+i*6,tags[i]);put(b,42+i*6,static_cast<u16>(26+i*8));
        put(b,64+i*8,2);put(b,66+i*8,refs[i*2]);put(b,68+i*8,refs[i*2+1]);put(b,96+i*2,10);}
    put(b,94,4);put(b,104,1);SfntLayout layout{};
    CHECK(sfnt_layout_open({b,sizeof(b)},SfntLayoutKind::substitution,&layout)==SfntStatus::ok);
    const SfntLookupStep masked[]{{0,sfnt_hangul_tjmo,1},{1,0,1},{2,sfnt_hangul_ljmo|sfnt_hangul_vjmo|sfnt_hangul_tjmo,1},{3,0,1}},
        required[]{{0,0,1},{1,0,1},{2,0,1},{3,0,1}};
    for(u32 mode=0;mode<4;++mode){put(b,24,mode&2?2:0xffff);const auto* want=mode&2?required:masked;
        u16 scratch[4];SfntLookupStep output[4];SfntLookupPlanResult result{};
        const auto run=[&](u64 budget,u32 capacity) noexcept {return sfnt_hangul_lookup_plan(layout,{},(mode&1)!=0,{scratch,4},{output,capacity},&result,budget);};
        CHECK(run(1000000,4)==SfntStatus::ok&&result.lookup_count==4&&result.script_tag==sfnt_tag('h','a','n','g')&&bytes_equal(output,want,sizeof(masked)));
        const u64 full=result.operations;CHECK(full<10000);
        for(u64 budget=0;budget<=full;++budget){for(auto& step:output)step={0xeeee,0xee,0xee};const auto status=run(budget,4);
            CHECK(result.operations<=budget&&status==(budget<full?SfntStatus::resource_limit:SfntStatus::ok));
            if(status!=SfntStatus::ok)for(const auto& step:output)CHECK(step.lookup==0xeeee);}
        for(auto& step:output)step={0xeeee,0xee,0xee};
        CHECK(run(1000000,3)==SfntStatus::output_too_small&&result.lookup_count==4);for(const auto& step:output)CHECK(step.lookup==0xeeee);
    }
    test_write_text("PASS Hangul lookup plan units\n");return true;
}
template<class T> Slice<T> allocate(Arena& a,u32 n) noexcept {return {arena_allocate<T>(&a,n),n};}
SfntShapeStorage storage(Arena& a) noexcept {
    constexpr u32 n=1024;
    return {allocate<SfntComponentGlyph>(a,n),allocate<SfntComponentNode>(a,2*n),allocate<u64>(a,n/16),
        allocate<u32>(a,n),allocate<u32>(a,2*n),allocate<SfntMarkAssociation>(a,n),allocate<double>(a,n),
        allocate<u8>(a,n),allocate<SfntPosition>(a,n),allocate<SfntPlacement>(a,n),
        {allocate<SfntGsubExecutionFrame>(a,64),allocate<u32>(a,n),allocate<u32>(a,n),allocate<u32>(a,n),
         allocate<u8>(a,n*sizeof(SfntComponentGlyph)),allocate<SfntComponentUndo>(a,4*n)},
        {allocate<SfntGposExecutionFrame>(a,64),allocate<u32>(a,n),allocate<u64>(a,n/16),allocate<double>(a,n),allocate<SfntCursiveChainNode>(a,n)}};
}
i32 rounded(float v) noexcept {return v<0?static_cast<i32>(v-0.5f):static_cast<i32>(v+0.5f);}
u32 head_checksum(const ScriptShapeOwner& owner) noexcept {
    const auto* instance=native_font_instance_get(owner.instance_store,owner.instance);if(!instance)return 0;
    const auto* font=native_font_get(owner.fonts,instance->face);if(!font)return 0;ConstSlice<u8> head{};
    if(sfnt_table(font->geometry.face,sfnt_tag('h','e','a','d'),&head)!=SfntStatus::ok||head.count<12)return 0;
    return byte_load_be32(head.data+8);
}
// HarfBuzz's glyphs, positions and clusters for each sample, through the compact output (the client's path). Glyph
// identity and positions need the font HarfBuzz shaped; another font version still checks glyph and unit counts.
template<u32 N> bool samples(NativeClusterScript script,const ScriptShapeOwner& owner,const NativeClusterPlan& plan,const SyllabicSample (&cases)[N],
    u32 checksum,const SfntShapeStorage& s,const char* label) noexcept {
    const bool same=head_checksum(owner)==checksum;const float em=static_cast<float>(native_run_plan_get(owner.store,owner.run)->units_per_em);
    u16 glyphs[64];SfntPlacement placements[64];u32 characters[64];SfntClusterGlyph mapped[64];
    for(const auto& c:cases){Utf8View text{c.text,c_string_length(c.text)};
        NativeAsciiStorage compact{{mapped,64},s,{glyphs,64},{placements,64},{characters,64}};NativeAsciiResult result{};
        CHECK(native_cluster_run(script,owner.store,plan,text,em,compact,&result)==SfntStatus::ok);
        CHECK(result.glyph_count==c.glyph_count&&result.source_units==c.unit_count);if(!same)continue;
        for(u32 i=0;i<c.glyph_count;++i){const i32* g=c.glyphs+i*4;
            if(glyphs[i]!=g[0]||rounded(placements[i].advance)!=g[1]||rounded(placements[i].advance_offset)!=g[2]||rounded(placements[i].ascender_offset)!=g[3]){
                test_write_text(label);test_write_text(" sample ");test_write_u64(static_cast<u64>(&c-cases));test_write_text(" glyph ");test_write_u64(i);
                test_write_text(": ");test_write_u64(glyphs[i]);test_write_text(" advance ");test_write_u64(static_cast<u64>(rounded(placements[i].advance)));
                test_write_text(" x ");test_write_u64(static_cast<u64>(rounded(placements[i].advance_offset)+100000));test_write_text(" y ");test_write_u64(static_cast<u64>(rounded(placements[i].ascender_offset)+100000));
                test_write_text(" (offsets +100000)\n");return failed(__LINE__);}}
        for(u32 i=0;i<c.unit_count;++i)CHECK(characters[i]==c.characters[i]);}
    test_write_text("PASS ");test_write_text(label);test_write_text(same?" HarfBuzz samples, ":" sample counts (font differs from the fixture's), ");
    test_write_u64(N);test_write_text(" strings\n");return true;
}
bool path_of(const wchar_t* leaf,wchar_t* path) noexcept {
    const u32 directory=GetWindowsDirectoryW(path,900);CHECK(directory&&directory<900);
    const wchar_t fonts[]=L"\\Fonts\\";u32 n=directory;for(u32 i=0;fonts[i];++i)path[n++]=fonts[i];
    for(u32 i=0;leaf[i];++i){CHECK(n<1000);path[n++]=leaf[i];}path[n]=0;return true;
}
template<u32 A,u32 B> bool owned_script(NativeClusterScript script,const char* name,const wchar_t* font,const wchar_t* fallback_font,
    const SyllabicSample (&cases)[A],u32 checksum,const SyllabicSample (&fallback_cases)[B],u32 fallback_checksum,bool default_shaper) noexcept {
    Arena memory{};CHECK(arena_initialize(&memory,64u*1024*1024,1024*1024));struct Release {Arena& a;~Release() noexcept {arena_release(&a);}} release{memory};
    Arena state{};CHECK(arena_initialize(&state,64u*1024*1024,65536));Release release_state{state};
    const auto s=storage(memory);CHECK(s.glyphs.data);
    wchar_t path[1024];CHECK(path_of(font,path));ScriptShapeOwner owner;CHECK(owner.open(path));
    auto& scratch=owner.scratch;arena_reset(&scratch);auto* sentinel=arena_allocate<u32>(&scratch);CHECK(sentinel);*sentinel=0x12345678;
    const auto before_state=arena_mark(&state),before_scratch=arena_mark(&scratch);const u32 refs=owner.plans[0].references;NativeClusterPlan prepared{};
    const auto unchanged=[&]() noexcept {return state.offset==before_state.offset&&scratch.offset==before_scratch.offset&&*sentinel==0x12345678&&
        owner.plans[0].references==refs&&!prepared.run.slot&&!prepared.cmap&&!prepared.substitution_count&&!prepared.positioning_count&&!prepared.fallback;};
    CHECK(native_cluster_plan_prepare(script,owner.store,owner.run,false,&state,&scratch,&prepared,0)==SfntStatus::resource_limit&&unchanged());
    CHECK(native_cluster_plan_prepare(script,owner.store,owner.run,false,&state,&state,&prepared)==SfntStatus::invalid_input&&unchanged());
    CHECK(native_cluster_plan_prepare(static_cast<NativeClusterScript>(4),owner.store,owner.run,false,&state,&scratch,&prepared)==SfntStatus::invalid_input&&unchanged());
    auto stale=owner.run;++stale.generation;
    CHECK(native_cluster_plan_prepare(script,owner.store,stale,false,&state,&scratch,&prepared)==SfntStatus::invalid_input&&unchanged());
    Arena tiny{};CHECK(arena_initialize(&tiny,4096,4096,4096));Release release_tiny{tiny};
    CHECK(arena_allocate(&tiny,4095,1));const auto mark=arena_mark(&tiny);
    CHECK(native_cluster_plan_prepare(script,owner.store,owner.run,false,&tiny,&scratch,&prepared)==SfntStatus::resource_limit&&unchanged()&&tiny.offset==mark.offset);
    CHECK(native_cluster_plan_prepare(script,owner.store,owner.run,false,&state,&tiny,&prepared)==SfntStatus::output_too_small&&unchanged()&&tiny.offset==mark.offset);
    for(u32 mode=0;mode<2;++mode){const usize before=state.offset;
        CHECK(native_cluster_plan_prepare(script,owner.store,owner.run,mode!=0,&state,&scratch,&prepared)==SfntStatus::ok);
        CHECK(prepared.independent_letters==(mode!=0)&&!prepared.fallback&&prepared.substitution_count);
        CHECK(scratch.offset==before_scratch.offset&&*sentinel==0x12345678&&owner.plans[0].references==refs&&state.offset>before);
        if(!mode&&!samples(script,owner,prepared,cases,checksum,s,name))return false;
        auto invalid=prepared;++invalid.run.generation;SfntShapeRunResult result{};
        CHECK(native_cluster_run(script,owner.store,invalid,{},1000,SfntShapeStorage{},&result)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
        const Utf8View pair{cases[0].text,c_string_length(cases[0].text)};const u32 pair_glyphs=cases[0].glyph_count,pair_units=cases[0].unit_count;
        CHECK(native_cluster_run(script,owner.store,prepared,pair,1000,s,&result)==SfntStatus::ok&&result.source_units==pair_units&&result.glyph_count==pair_glyphs);
        u16 expected[64];SfntPlacement placements[64];u32 sources[64];CHECK(pair_glyphs<=64&&pair_units<=64);
        for(u32 i=0;i<pair_glyphs;++i)expected[i]=s.glyphs[i].glyph.glyph;
        copy_bytes(placements,s.placements.data,pair_glyphs*sizeof(SfntPlacement));copy_bytes(sources,s.character_glyphs.data,pair_units*sizeof(u32));
        u16 glyphs[64];SfntPlacement placed[64];u32 characters[64];
        for(u32 alias=0;alias<8;++alias){NativeAsciiStorage compact{{},s,
            alias&1?Slice<u16>{reinterpret_cast<u16*>(s.glyphs.data),s.glyphs.count}:Slice<u16>{glyphs,64},
            alias&2?s.placements:Slice<SfntPlacement>{placed,64},alias&4?s.character_glyphs:Slice<u32>{characters,64}};NativeAsciiResult published{};
            CHECK(native_cluster_run(script,owner.store,prepared,pair,1000,compact,&published)==SfntStatus::ok&&published.glyph_count==pair_glyphs&&published.source_units==pair_units&&published.shaped_ranges==1);
            CHECK(bytes_equal(compact.glyphs.data,expected,pair_glyphs*sizeof(u16))&&bytes_equal(compact.placements.data,placements,pair_glyphs*sizeof(SfntPlacement))&&
                bytes_equal(compact.character_glyphs.data,sources,pair_units*sizeof(u32)));}
        NativeAsciiStorage compact{{},s,{glyphs,pair_glyphs-1},{placed,64},{characters,64}};NativeAsciiResult published{};
        CHECK(native_cluster_run(script,owner.store,prepared,pair,1000,compact,&published)==SfntStatus::output_too_small&&!published.glyph_count&&!published.source_units);
        // A character the font lacks shapes to .notdef, which the compact output rejects so the client can fall back.
        compact.glyphs.count=64;char text[16];u32 bytes=utf8_encode(0x1f600,text);
        CHECK(native_cluster_run(script,owner.store,prepared,{text,bytes},1000,compact,&published)==SfntStatus::unsupported_sequence&&!published.glyph_count&&!published.source_units);
        bytes=utf8_encode(0x20,text);bytes+=utf8_encode(0x1f600,text+bytes);bytes+=utf8_encode(0x20,text+bytes);
        CHECK(native_cluster_run(script,owner.store,prepared,{text,bytes},1000,s,&result)==SfntStatus::ok&&result.source_units==4&&result.glyph_count==3);
        const u32 supplementary[]{0,1,1,2};CHECK(bytes_equal(s.character_glyphs.data,supplementary,sizeof(supplementary)));
        const u8 malformed[][4]{{0xe0,0x80,0x80,0},{0xed,0xa0,0x80,0},{0xf4,0x90,0x80,0x80},{0x80,0,0,0},{0xe0,0xa0,0,0}};
        const u32 lengths[]{3,3,4,1,2};
        for(u32 i=0;i<5;++i)CHECK(native_cluster_run(script,owner.store,prepared,{reinterpret_cast<const char*>(malformed[i]),lengths[i]},1000,s,&result)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
        auto short_storage=s;short_storage.components.count=0;
        CHECK(native_cluster_run(script,owner.store,prepared,pair,1000,short_storage,&result)==SfntStatus::output_too_small&&!result.glyph_count);
        CHECK(native_cluster_run(script,owner.store,prepared,pair,1000,s,&result,{1000000,100000,pair.bytes-1})==SfntStatus::resource_limit&&!result.glyph_count);
        CHECK(native_cluster_run(script,owner.store,prepared,{},1000,compact,&published)==SfntStatus::ok&&!published.glyph_count&&!published.source_units&&!published.shaped_ranges);
    }
    CHECK(owner.close());SfntShapeRunResult result{};
    CHECK(native_cluster_run(script,owner.store,prepared,{},1000,SfntShapeStorage{},&result)==SfntStatus::invalid_input&&!result.glyph_count&&!result.source_units);
    // A font HarfBuzz shapes the script in with its default shaper (the Common alphabetic plan) or the script's shaper.
    CHECK(path_of(fallback_font,path));ScriptShapeOwner fallback;CHECK(fallback.open(path));NativeClusterPlan other{};
    CHECK(native_cluster_plan_prepare(script,fallback.store,fallback.run,false,&state,&fallback.scratch,&other)==SfntStatus::ok&&(other.fallback!=nullptr)==default_shaper);
    if(default_shaper)CHECK(native_cluster_run(script,fallback.store,other,{cases[0].text,c_string_length(cases[0].text)},1000,s,&result)==SfntStatus::unsupported_sequence&&!result.glyph_count);
    if(!samples(script,fallback,other,fallback_cases,fallback_checksum,s,default_shaper?"default-shaper font":"font without the script"))return false;
    CHECK(fallback.close());
    test_write_text("PASS ");test_write_text(name);test_write_text(" owned units; assertions=");test_write_u64(checks);test_write_text("\n");
    return true;
}
bool run() noexcept {
    wchar_t mode[16]{};const DWORD n=GetEnvironmentVariableW(L"MAICORD_SYLLABIC_OWNED",mode,16);
    if(!n){bool ok=boundary_units()&&hangul_plan_units();for(const auto& c:corpora)ok=cluster_corpus(c)&&ok;return ok;}
    CHECK(n<16);
    const auto is=[&](const wchar_t* option) noexcept {u32 i=0;while(option[i]&&option[i]==mode[i])++i;return !option[i]&&!mode[i];};
    if(is(L"sinhala"))return owned_script(NativeClusterScript::sinhala,"Sinhala",L"Nirmala.ttc",L"segoeui.ttf",sinhala_samples,sinhala_samples_checksum,
        sinhala_fallback_samples,sinhala_fallback_checksum,sinhala_fallback_default_shaper);
    if(is(L"khmer"))return owned_script(NativeClusterScript::khmer,"Khmer",L"LeelawUI.ttf",L"segoeui.ttf",khmer_samples,khmer_samples_checksum,
        khmer_fallback_samples,khmer_fallback_checksum,khmer_fallback_default_shaper);
    if(is(L"myanmar"))return owned_script(NativeClusterScript::myanmar,"Myanmar",L"mmrtext.ttf",L"segoeui.ttf",myanmar_samples,myanmar_samples_checksum,
        myanmar_fallback_samples,myanmar_fallback_checksum,myanmar_fallback_default_shaper);
    if(is(L"tibetan"))return owned_script(NativeClusterScript::tibetan,"Tibetan",L"himalaya.ttf",L"segoeui.ttf",tibetan_samples,tibetan_samples_checksum,
        tibetan_fallback_samples,tibetan_fallback_checksum,tibetan_fallback_default_shaper);
    return failed(__LINE__);
}
}}
MAICORD_TEST_ENTRY(maicord_syllabic_shape_entry) {maicord::test_process_exit(maicord::run()?0u:1u);}
