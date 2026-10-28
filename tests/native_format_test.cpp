#include "maicord/ui/native_text_candidate.hpp"
#include "maicord/ui/native_text_catalog.hpp"
#include "../src/ui/native_text_catalog_owner.hpp"
#include "maicord/text/font_session.hpp"
#include "maicord/text/font_run.hpp"
#include "maicord/text/paragraph_hits.hpp"
#include "maicord/core/win32_kernel.hpp"
#include <stddef.h>
namespace maicord {namespace {
bool released=true;
void print(const char* text) noexcept {u32 n=0;while(text[n])++n;DWORD written=0;WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,n,&written,nullptr);}
#define CHECK(x) do{if(!(x)){print("FAIL native format line " #x "\n");return false;}}while(0)
TextGlyph glyphs[256];TextRun runs[256];u16 clusters[256];TextDecoration decorations[256];TextHitRegion regions[256];
NativeParagraphHitCluster hit_clusters[256];NativeParagraphHitLine hit_lines[257];float advances[1280];SfntFontLineParts parts[16];
ShapedText output() noexcept {ShapedText out{};out.glyphs=glyphs;out.glyph_capacity=256;out.runs=runs;out.run_capacity=256;out.clusters=clusters;out.cluster_capacity=256;
    out.decorations=decorations;out.decoration_capacity=256;out.hit_regions=regions;out.hit_region_capacity=256;return out;}
void release_output(ShapedText& out) noexcept {
    auto* session=out.native_fonts();if(session){for(u32 i=0;i<out.run_count;++i)released=native_font_run_release(native_font_session_runs(session),out.runs[i].native_face)&&released;released=native_font_session_release(&session)&&released;}out={};
}
bool exercise(TextSystem system) noexcept {
    CHECK(system.native_format);
    const auto text=utf8_literal("AV office caf\xc3\xa9\nAV\tTo");auto draw=output();
    struct Release {ShapedText& value;~Release() noexcept {release_output(value);}} release{draw};
    CHECK(native_text_candidate_line(system,text,95,1000,&draw,nullptr));CHECK(draw.native_fonts()&&draw.glyph_count&&draw.line_count>1);release_output(draw);draw=output();
    const TextStyleSpan style{3,6,text_style_link|text_style_underline};const TextHitRange hit{0,18,65535,0};
    CHECK(native_text_candidate_styled(system,text,{&style,1},95,1000,&draw,nullptr,TextWrapping::wrap,TextOverflow::clip,{&hit,1}));
    CHECK(draw.native_fonts()&&draw.decoration_count&&draw.hit_region_count);
    NativeParagraphHits geometry{};NativeParagraphHitStorage storage{{hit_clusters,256},{hit_lines,257},{advances,1280},&geometry,{parts,16}};TextMeasureResult measure{};
    CHECK(native_text_candidate_styled(system,text,{&style,1},95,1000,nullptr,&measure,TextWrapping::wrap,TextOverflow::clip,{},&storage));
    CHECK(measure.ok()&&geometry.source_units==20&&geometry.lines.count>1);
    NativeParagraphHitMetric metric{};float x=0;CHECK(native_paragraph_hit_position(geometry,10,false,&metric,&x)==SfntStatus::ok);
    const Utf8View emoji[]{utf8_literal("\xf0\x9f\x98\x80"),utf8_literal("\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7"),
        utf8_literal("\xf0\x9f\x8f\xb4\xf3\xa0\x81\xa7\xf3\xa0\x81\xa2\xf3\xa0\x81\xa5\xf3\xa0\x81\xae\xf3\xa0\x81\xa7\xf3\xa0\x81\xbf"),
        utf8_literal("A\xf0\x9f\x98\x80" "B")};
    for(const auto input:emoji){release_output(draw);draw=output();
        CHECK(native_text_candidate_line(system,input,37,1000000,&draw,nullptr));CHECK(draw.native_fonts()&&draw.glyph_count&&draw.run_count);
        const TextStyleSpan effect{0,input.bytes,text_style_underline|(input.data[0]=='A'?text_style_link:text_style_bold|text_style_italic)};release_output(draw);draw=output();
        CHECK(native_text_candidate_styled(system,input,{&effect,1},37,1000000,&draw,nullptr,TextWrapping::wrap));
        CHECK(native_text_candidate_hits(system,input,37,1000000,TextWrapping::wrap,storage,&measure));
        CHECK(measure.ok()&&measure.utf16_units==draw.cluster_count&&native_paragraph_hit_position(geometry,1,false,&metric,&x)==SfntStatus::ok);}
    CHECK(!GetModuleHandleW(L"dwrite.dll")&&!GetModuleHandleW(L"DWriteCore.dll"));return true;
}
bool braille(TextSystem system) noexcept {
    // Exercise every braille cell, including U+2800, through native coverage,
    // measurement, styled shaping and cached reuse. No platform text fallback.
    for(u32 half=0;half<2;++half){char text[391]{};u32 bytes=0;
        for(u32 i=0;i<128;++i){if(i&&!(i%16))text[bytes++]='\n';const u32 cp=0x2800+half*128+i;
            text[bytes++]=static_cast<char>(0xe0|(cp>>12));text[bytes++]=static_cast<char>(0x80|((cp>>6)&63));text[bytes++]=static_cast<char>(0x80|(cp&63));}
        const Utf8View input{text,bytes};auto draw=output();
        struct Release {ShapedText& value;~Release() noexcept {release_output(value);}} release{draw};
        TextMeasureResult measure{};
        CHECK(native_text_candidate_line(system,input,1000,1000,nullptr,&measure));
        CHECK(measure.ok()&&measure.utf16_units==135&&measure.line_count==8);
        for(u32 warm=0;warm<2;++warm){CHECK(native_text_candidate_line(system,input,1000,1000,&draw,nullptr));
            CHECK(draw.native_fonts()&&draw.cluster_count==135&&draw.line_count==8&&draw.glyph_count>=128);
            for(u32 i=0;i<draw.glyph_count;++i)CHECK(draw.glyphs[i].index);
            release_output(draw);draw=output();}
        const TextStyleSpan style{0,bytes,text_style_bold|text_style_underline};
        CHECK(native_text_candidate_styled(system,input,{&style,1},1000,1000,&draw,nullptr,TextWrapping::wrap));
        CHECK(draw.native_fonts()&&draw.line_count==8&&draw.decoration_count);
    }
    const auto mixed=utf8_literal("A\xe2\xa0\x80\xe2\xa3\xbf\nZ");TextMeasureResult measure{};
    CHECK(native_text_candidate_line(system,mixed,1000,1000,nullptr,&measure)&&measure.ok()&&measure.line_count==2);
    CHECK(!GetModuleHandleW(L"dwrite.dll")&&!GetModuleHandleW(L"DWriteCore.dll"));return true;
}
bool family_cache(NativeFontSession* session) noexcept {
    NativeTextCatalogs catalogs{};Arena scratch{};
    CHECK(native_text_catalogs_initialize(&catalogs)&&arena_initialize(&scratch,4*1024*1024,0,4096));
    struct Release {NativeTextCatalogs& c;Arena& a;~Release() noexcept{native_text_catalogs_release(&c);arena_release(&a);}} release{catalogs,scratch};
    const u16 family[]{'G','e','o','r','g','i','a'},lower[]{'g','e','o','r','g','i','a'};
    NativeTextFontQuery query{0,{family,7},{400,5,0}};NativeTextFontRange normal{},bold{},again{};
    CHECK(native_text_catalog_range(&catalogs,query,session,&scratch,utf8_literal("a\xcb\x91"),&normal)&&normal.bytes==1&&normal.units==1);
    CHECK(native_text_catalog_range(&catalogs,query,session,&scratch,utf8_literal("\xcb\x91\xcc\x81" "a"),&bold)&&bold.bytes==4&&bold.units==2&&normal.match.candidate!=bold.match.candidate&&!bold.match.simulations);
    const auto acquisitions=catalogs.coverage.acquisitions;const auto* order=catalogs.families;
    const auto mark=arena_mark(&scratch);auto* sentinel=arena_allocate<u64>(&scratch);CHECK(sentinel);*sentinel=0x7391257ee9234411ull;
    query.family={lower,7};
    for(u32 i=0;i<64;++i)CHECK(native_text_catalog_range(&catalogs,query,session,&scratch,utf8_literal("\xcb\x91\xcc\x81" "a"),&again)&&again.bytes==bold.bytes&&again.units==bold.units&&again.match.candidate==bold.match.candidate);
    CHECK(catalogs.families==order&&catalogs.coverage.acquisitions==acquisitions&&*sentinel==0x7391257ee9234411ull&&arena_rewind(&scratch,mark));
    CHECK(!native_text_catalog_range(&catalogs,query,session,&scratch,utf8_literal("\xff"),&again)&&!again.bytes&&!again.units&&again.match.candidate==native_fallback_missing);
    NativeFontInstanceHandle instance{};NativeFontSelection selected{};
    CHECK(native_text_catalog_select_face(&catalogs,0,bold.match.candidate,0,session,&scratch,&instance,&selected)&&instance.slot);
    CHECK(native_font_instance_release(native_font_session_instances(session),instance));
    CHECK(native_text_catalog_select_face(&catalogs,0,bold.match.candidate,2,session,&scratch,&instance,&selected)&&instance.slot);
    CHECK(native_font_instance_release(native_font_session_instances(session),instance));
    CHECK(!native_text_catalog_select_face(&catalogs,0,bold.match.candidate,4,session,&scratch,&instance,&selected)&&!instance.slot&&!selected.count);
    return true;
}
bool simulated_formats(NativeFontSession* session) noexcept {
    TextSystem system{};system.native_fonts=session;system.em_size=16;CHECK(native_text_context_initialize(&system.native_text,session));
    struct Release {TextSystem& s;~Release() noexcept{native_text_context_release(&s.native_text);}} release{system};
    const u16 family[]{'S','e','g','o','e',' ','U','I',' ','H','i','s','t','o','r','i','c'};NativeFontInstanceHandle common{};
    const auto text=utf8_literal("AV A\xcc\x81\tTo\r\nAV");
    for(u32 mode=0;mode<4;++mode){NativeTextFormatDescription description{{family,17},0,{static_cast<u16>((mode&1)?600:400),5,static_cast<u8>((mode&2)?2:0)},16,64,true};
        CHECK(native_text_format_create(system.native_text,description,&system.native_format));auto draw=output();
        CHECK(native_text_candidate_line(system,text,37,1000,&draw,nullptr)&&draw.native_fonts()&&draw.run_count);
        const auto* font=native_font_run_get(*native_font_session_runs(session),draw.runs[0].native_face);CHECK(font&&font->simulations==mode);
        if(!mode)common=font->instance;else CHECK(font->instance.slot==common.slot&&font->instance.generation==common.generation);
        release_output(draw);NativeParagraphHits geometry{};TextMeasureResult measure{};
        NativeParagraphHitStorage storage{{hit_clusters,256},{hit_lines,257},{advances,1280},&geometry,{parts,16}};
        CHECK(native_text_candidate_hits(system,text,37,1000,TextWrapping::wrap,storage,&measure)&&measure.ok()&&geometry.lines.count);
    }CHECK(!GetModuleHandleW(L"dwrite.dll")&&!GetModuleHandleW(L"DWriteCore.dll"));return true;
}
bool system_cache(NativeFontSession* session,const wchar_t* path) noexcept {
    NativeTextCatalogs catalogs{};Arena scratch{};
    CHECK(native_text_catalogs_initialize(&catalogs)&&arena_initialize(&scratch,4*1024*1024,0,4096));
    struct Release {NativeTextCatalogs& c;Arena& a;~Release() noexcept{native_text_catalogs_release(&c);arena_release(&a);}} release{catalogs,scratch};
    u32 scope=0;CHECK(native_text_catalog_register_path(&catalogs,path,&scope)&&scope);
    const u16 family[]{'C','o','m','p','a','g','n','o','n'};NativeTextFontQuery query{scope,{family,9},{400,5,0}};
    NativeTextFontRange base{},other{},again{};
    CHECK(native_text_catalog_range(&catalogs,query,session,&scratch,utf8_literal("A\xce\xb1"),&base)&&base.scope==scope&&base.bytes==1&&base.units==1);
    CHECK(native_text_catalog_range(&catalogs,query,session,&scratch,utf8_literal("\xce\xb1" "A"),&other)&&other.scope==0&&other.bytes==2&&other.units==1);
    const auto* held_catalog=&catalogs.scopes[scope].catalog;u32 extra_scope=0;
    CHECK(native_text_catalog_register_path(&catalogs,L"unused-stability-scope",&extra_scope)&&extra_scope!=scope);
    CHECK(&catalogs.scopes[scope].catalog==held_catalog);
    const auto acquisitions=catalogs.coverage.acquisitions;const auto* families=catalogs.families;
    const auto mark=arena_mark(&scratch);auto* sentinel=arena_allocate<u64>(&scratch);CHECK(sentinel);*sentinel=0x751eb32ea039c291ull;
    for(u32 i=0;i<64;++i)CHECK(native_text_catalog_range(&catalogs,query,session,&scratch,utf8_literal("\xce\xb1" "A"),&again)&&bytes_equal(&again,&other,sizeof(other)));
    CHECK(catalogs.coverage.acquisitions==acquisitions&&catalogs.families==families&&*sentinel==0x751eb32ea039c291ull&&arena_rewind(&scratch,mark));
    NativeFontInstanceHandle instance{};NativeFontSelection selection{};u32 selected_scope=0xffffffffu;
    CHECK(native_text_catalog_select_face(&catalogs,other.scope,other.match.candidate,other.match.simulations,session,&scratch,&instance,&selection)&&instance.slot);
    CHECK(native_font_instance_release(native_font_session_instances(session),instance));
    bool missing_family=true;
    CHECK(native_text_catalog_select_query(&catalogs,query,session,&scratch,&instance,&selection,nullptr,nullptr,&selected_scope,&missing_family)&&instance.slot&&selected_scope==scope&&!missing_family);
    CHECK(native_font_instance_release(native_font_session_instances(session),instance));
    u16 missing[]{'M','a','i','C','o','r','d','-','N','o','-','S','u','c','h','-','F','a','m','i','l','y'};query.family={missing,22};
    CHECK(native_text_catalog_select_query(&catalogs,query,session,&scratch,&instance,&selection,nullptr,nullptr,&selected_scope,&missing_family)&&instance.slot&&selected_scope==0&&missing_family);
    CHECK(native_font_instance_release(native_font_session_instances(session),instance));
    CHECK(native_text_catalog_range(&catalogs,query,session,&scratch,utf8_literal("A"),&again)&&again.scope==0&&again.bytes==1);
    // The negative family entry owns its name, including in a private scope.
    const u16 original[]{'M','a','i','C','o','r','d','-','N','o','-','S','u','c','h','-','F','a','m','i','l','y'};const auto* cached=catalogs.families;
    missing[0]='X';query.family={original,22};
    CHECK(native_text_catalog_range(&catalogs,query,session,&scratch,utf8_literal("A"),&again)&&again.scope==0&&catalogs.families==cached);
    query.family={family,9};
    CHECK(native_text_catalog_range(&catalogs,query,session,&scratch,utf8_literal("A"),&again)&&again.scope==scope&&again.match.candidate==base.match.candidate);
    query.scope=0xffffffffu;
    CHECK(!native_text_catalog_select_query(&catalogs,query,session,&scratch,&instance,&selection,nullptr,nullptr,&selected_scope,&missing_family)&&!instance.slot&&!missing_family);
    CHECK(!native_text_catalog_range(&catalogs,query,session,&scratch,utf8_literal("A"),&again)&&!again.bytes&&again.scope==native_fallback_missing&&again.match.candidate==native_fallback_missing);
    CHECK(!GetModuleHandleW(L"dwrite.dll")&&!GetModuleHandleW(L"DWriteCore.dll"));return true;
}
// No font discovery is needed to exercise inline/arena transitions and index pages.
bool format_boundaries(NativeFontSession* session) noexcept {
    const u32 lengths[]{64,65,255};for(const u32 length:lengths){
        TextSystem system{};system.native_fonts=session;
        CHECK(native_text_context_initialize(&system.native_text,session));
        struct Release {TextSystem& s;~Release() noexcept{native_text_context_release(&s.native_text);}} release{system};
        u16 family[256];for(u32 i=0;i<256;++i)family[i]=static_cast<u16>('A'+i%26);
        NativeTextFormatDescription description{{family,256},0,{400,5,0},16,64,true};
        CHECK(!native_text_format_create(system.native_text,description,&system.native_format)&&!system.native_format);
        description.family.count=length;u64 identities[130]{};
        for(u32 i=0;i<130;++i){description.em_size=16+static_cast<float>(i);
            CHECK(native_text_format_create(system.native_text,description,&system.native_format)&&system.native_format==i+1);
            identities[i]=native_text_format_identity(system);CHECK(identities[i]&&(!i||identities[i]!=identities[i-1]));
            // Mutating the caller buffer must not mutate any previously saved format.
            u16 original[256];copy_bytes(original,family,length*2);family[0]='Z';
            auto copied=description;copied.family.data=original;u32 same=0;
            CHECK(native_text_format_create(system.native_text,copied,&same)&&same==i+1);family[0]='A';}
        for(u32 i=130;i--;){description.em_size=16+static_cast<float>(i);
            CHECK(native_text_format_create(system.native_text,description,&system.native_format)&&system.native_format==i+1);
            CHECK(native_text_format_identity(system)==identities[i]);}
    }
    NativeTextCatalogs catalogs{};Arena scratch{};
    CHECK(native_text_catalogs_initialize(&catalogs)&&arena_initialize(&scratch,4096,0,4096));
    struct Release {NativeTextCatalogs& c;Arena& a;~Release() noexcept{native_text_catalogs_release(&c);arena_release(&a);}} release{catalogs,scratch};
    // Allocation failure publishes neither a path nor a scope. Existing scope
    // addresses remain stable as later registrations commit more arena pages.
    NativeTextCatalogScope* held[32]{&catalogs.scopes[0]};
    const usize reserved=catalogs.storage.reserved;catalogs.storage.reserved=0;u32 registered=0;
    CHECK(!native_text_catalog_register_path(&catalogs,L"unallocated",&registered)&&registered==0xffffffffu&&catalogs.count==1&&!catalogs.storage.offset);
    catalogs.storage.reserved=reserved;
    for(u32 i=1;i<32;++i){wchar_t path[]{L'x',static_cast<wchar_t>(L'A'+i),0};
        CHECK(native_text_catalog_register_path(&catalogs,path,&registered)&&registered==i&&catalogs.count==i+1);
        held[i]=&catalogs.scopes[i];path[1]=L'!';CHECK(held[i]->path[1]==static_cast<wchar_t>(L'A'+i));
        for(u32 j=0;j<=i;++j)CHECK(&catalogs.scopes[j]==held[j]);}
    const usize full_offset=catalogs.storage.offset;
    CHECK(native_text_catalog_register_path(&catalogs,L"xB",&registered)&&registered==1&&catalogs.storage.offset==full_offset);
    CHECK(!native_text_catalog_register_path(&catalogs,L"over-capacity",&registered)&&registered==0xffffffffu&&catalogs.count==32&&catalogs.storage.offset==full_offset);
    // The highest scope bit must cache failure independently from the system bit.
    catalogs.scopes[31].path=L"";
    const u16 family[]{'A'};NativeTextFontQuery query{31,{family,1},{400,5,0}};NativeTextFontRange result{};
    CHECK(!native_text_catalog_range(&catalogs,query,session,&scratch,utf8_literal("A"),&result));
    CHECK(catalogs.attempted==0x80000000u&&catalogs.builds==1&&catalogs.failures==1);
    CHECK(!native_text_catalog_range(&catalogs,query,session,&scratch,utf8_literal("A"),&result));
    CHECK(catalogs.attempted==0x80000000u&&catalogs.builds==1&&catalogs.failures==1);
    return true;
}
volatile u32 stale_observation;
__declspec(noinline) void stale_marker(const NativeTextCatalogs* owner,u32 scope,u32 phase) noexcept {stale_observation=owner->stale_scopes^scope^phase;}
static_assert(offsetof(NativeTextCatalogs,stale_scopes)==436);
// Only copied catalog descriptors are faulted; no installed/private font bytes
// or shared snapshot arrays are modified. Existing instances must remain valid.
bool stale_scope_signal(NativeFontSession* session,const wchar_t* path,u32 scope_index) noexcept {
    NativeTextCatalogs catalogs{};Arena scratch{},copies{};NativeFontInstanceHandle retained{};
    struct Release {NativeTextCatalogs& c;Arena& a;Arena& b;NativeFontSession* session;NativeFontInstanceHandle& held;
        ~Release() noexcept {if(held.slot)native_font_instance_release(native_font_session_instances(session),held);native_text_catalogs_release(&c);arena_release(&a);arena_release(&b);}}
        release{catalogs,scratch,copies,session,retained};
    CHECK(native_text_catalogs_initialize(&catalogs)&&arena_initialize(&scratch,4*1024*1024,0,4096)&&arena_initialize(&copies,1024*1024,0,4096));
    for(u32 i=1;i<scope_index;++i){wchar_t unused[]{L'u',static_cast<wchar_t>(L'A'+i),0};u32 index=0;CHECK(native_text_catalog_register_path(&catalogs,unused,&index)&&index==i);}
    if(scope_index){u32 index=0;CHECK(native_text_catalog_register_path(&catalogs,path,&index)&&index==scope_index);}
    const u16 system_name[]{'S','e','g','o','e',' ','U','I'},private_name[]{'C','o','m','p','a','g','n','o','n'};
    const NativeTextFontQuery query{scope_index,scope_index?ConstSlice<u16>{private_name,9}:ConstSlice<u16>{system_name,8},{400,5,0}};
    NativeTextFontRange range{};CHECK(native_text_catalog_range(&catalogs,query,session,&scratch,utf8_literal("A"),&range)&&range.match.candidate!=native_fallback_missing);
    CHECK(scope_index||catalogs.files);NativeFontSelection selected{};
    CHECK(native_text_catalog_select_query(&catalogs,query,session,&scratch,&retained,&selected)&&retained.slot&&!catalogs.stale_scopes);
    const auto* held=native_font_instance_get(*native_font_session_instances(session),retained);CHECK(held);
    auto& catalog=scope_index?catalogs.scopes[scope_index].catalog:catalogs.files->catalog;const auto original=catalog.files;
    auto* files=arena_allocate<NativeFontCatalogFile>(&copies,catalog.count.files);CHECK(files);
    copy_bytes(files,original.data,catalog.count.files*sizeof(*files));
    const u32 file=catalog.faces[catalog.candidates[range.match.candidate].face].file;
    auto* changed=reinterpret_cast<u8*>(&files[file].change_time);*changed^=1;
    CHECK(native_font_coverage_reset(&catalogs.coverage));catalog.files={files,catalog.count.files};
    struct Restore {NativeFontCatalog& c;Slice<NativeFontCatalogFile> original;~Restore() noexcept {c.files=original;}} restore{catalog,original};
    const u32 candidate=range.match.candidate;const u32 bit=1u<<scope_index;NativeFontInstanceHandle rejected{};
    stale_marker(&catalogs,scope_index,0);
    CHECK(!native_text_catalog_select_query(&catalogs,query,session,&scratch,&rejected,&selected)&&!rejected.slot&&!selected.face.slot&&catalogs.stale_scopes==bit);
    stale_marker(&catalogs,scope_index,1);
    CHECK(native_font_instance_get(*native_font_session_instances(session),retained)==held);
    catalogs.stale_scopes=0;
    CHECK(!native_text_catalog_range(&catalogs,query,session,&scratch,utf8_literal("A"),&range)&&!range.bytes&&catalogs.stale_scopes==bit);
    stale_marker(&catalogs,scope_index,2);
    catalogs.stale_scopes=0;
    CHECK(!native_text_catalog_select_face(&catalogs,scope_index,candidate,0,session,&scratch,&rejected,&selected)&&!rejected.slot&&!selected.face.slot&&catalogs.stale_scopes==bit);
    stale_marker(&catalogs,scope_index,3);
    // Invalid requests must not request a catalog rebuild.
    catalogs.stale_scopes=0;
    CHECK(!native_text_catalog_range(&catalogs,query,session,&scratch,utf8_literal("\xff"),&range)&&!catalogs.stale_scopes);
    CHECK(!native_text_catalog_select_face(&catalogs,scope_index,candidate,4,session,&scratch,&rejected,&selected)&&!catalogs.stale_scopes);
    catalog.files=original;
    CHECK(native_text_catalog_range(&catalogs,query,session,&scratch,utf8_literal("A"),&range)&&!catalogs.stale_scopes);
    catalogs.stale_scopes=bit;
    CHECK(native_text_catalog_range(&catalogs,query,session,&scratch,utf8_literal("A"),&range)&&catalogs.stale_scopes==bit);
    CHECK(native_font_instance_get(*native_font_session_instances(session),retained)==held);
    return true;
}
#include "native_generation_recovery.inc"
#include "native_recovery_pressure.inc"
bool run() noexcept {
    const wchar_t* arg=GetCommandLineW();bool quote=false;while(*arg){if(*arg=='"')quote=!quote;else if(!quote&&(*arg==' '||*arg=='\t'))break;++arg;}while(*arg==' '||*arg=='\t')++arg;
    const wchar_t option[]=L"--stale-signal ";u32 matched=0;while(option[matched]&&arg[matched]==option[matched])++matched;const bool stale_only=!option[matched];if(stale_only)arg+=matched;
    const wchar_t generation_option[]=L"--generation-recovery ";u32 generation_match=0;while(generation_option[generation_match]&&arg[generation_match]==generation_option[generation_match])++generation_match;const bool generation_only=!generation_option[generation_match];if(generation_only)arg+=generation_match;
    const wchar_t pressure_option[]=L"--recovery-pressure ";u32 pressure_match=0;while(pressure_option[pressure_match]&&arg[pressure_match]==pressure_option[pressure_match])++pressure_match;const bool pressure_only=!pressure_option[pressure_match];if(pressure_only)arg+=pressure_match;
    wchar_t path[1024];u32 length=0;quote=*arg=='"';if(quote)++arg;while(*arg&&(!quote||*arg!='"')&&(quote||(*arg!=' '&&*arg!='\t'))){CHECK(length<1023);path[length++]=*arg++;}path[length]=0;CHECK(length);

    CHECK(!GetModuleHandleW(L"dwrite.dll")&&!GetModuleHandleW(L"DWriteCore.dll"));
    TextSystem system{};CHECK(native_font_session_create(&system.native_fonts));CHECK(native_text_context_initialize(&system.native_text,system.native_fonts));
    struct Release {TextSystem& s;~Release() noexcept {native_text_context_release(&s.native_text);native_font_session_release(&s.native_fonts);}} release{system};
    if(stale_only){
        CHECK(stale_scope_signal(system.native_fonts,path,0));
        CHECK(stale_scope_signal(system.native_fonts,path,1));
        CHECK(stale_scope_signal(system.native_fonts,path,31));
        CHECK(!GetModuleHandleW(L"dwrite.dll")&&!GetModuleHandleW(L"DWriteCore.dll"));
        print("PASS stale scope signal: system file, private file, scope 31, retained instance, sticky bits\n");return true;
    }
    if(generation_only){CHECK(recovery_format_sharing());CHECK(generation_recovery(path));CHECK(automatic_generation_recovery(path));CHECK(released);return true;}
    if(pressure_only){CHECK(recovery_session_pressure(path));CHECK(released);return true;}
    CHECK(format_boundaries(system.native_fonts));
    CHECK(family_cache(system.native_fonts));
    CHECK(simulated_formats(system.native_fonts));
    u16 family[]{'S','e','g','o','e',' ','U','I'};NativeTextFormatDescription description{{family,8},0,{400,5,0},16,64,true};
    CHECK(native_text_format_create(system.native_text,description,&system.native_format));system.em_size=16;
    u32 same=0;CHECK(native_text_format_create(system.native_text,description,&same)&&same==system.native_format);
    family[0]='X';CHECK(exercise(system));family[0]='S';CHECK(braille(system));
    const Utf8View alphabetic[]{utf8_literal("A \xce\xb1\xce\xb2 \xd0\x90\xd0\x91"),utf8_literal("\xce\xb1\xcc\x93\xcc\x81\t\xd0\x98\xcc\x86\nAV")};
    for(const auto text:alphabetic){auto shaped=output();CHECK(native_text_candidate_line(system,text,37,1000000,&shaped,nullptr)&&shaped.native_fonts());
        CHECK(shaped.cluster_count&&shaped.line_count);release_output(shaped);NativeParagraphHits geometry{};TextMeasureResult measure{};
        NativeParagraphHitStorage storage{{hit_clusters,256},{hit_lines,257},{advances,1280},&geometry,{parts,16}};
        CHECK(native_text_candidate_hits(system,text,37,1000000,TextWrapping::wrap,storage,&measure)&&measure.ok()&&geometry.source_units);
        CHECK(!GetModuleHandleW(L"dwrite.dll")&&!GetModuleHandleW(L"DWriteCore.dll"));}
    const u32 first_format=system.native_format;const u64 first_identity=native_text_format_identity(system);
    const auto instances=native_font_session_stats(system.native_fonts).instances;
    for(u32 i=0;i<96;++i){description.em_size=8+static_cast<float>(i)*.5f;description.tab_stop=description.em_size*4;
        CHECK(native_text_format_create(system.native_text,description,&system.native_format));system.em_size=description.em_size;CHECK(exercise(system));}
    CHECK(native_font_session_stats(system.native_fonts).instances==instances);
    system.native_format=first_format;system.em_size=16;CHECK(native_text_format_identity(system)==first_identity&&exercise(system));
    // In-family fallback retains the selected physical face without a platform
    // text object. Its coverage-cache references must also be released with the
    // context while independently retained runs remain alive.
    const u16 georgia[]{'G','e','o','r','g','i','a'};description={{georgia,7},0,{400,5,0},16,64,true};
    CHECK(native_text_format_create(system.native_text,description,&system.native_format));system.em_size=16;
    const auto fallback_text=utf8_literal("a\xcb\x91\xcc\x81" "a\t\xcb\x91\n");auto fallback_draw=output();
    CHECK(native_text_candidate_line(system,fallback_text,37,1000,&fallback_draw,nullptr)&&fallback_draw.native_fonts());
    CHECK(fallback_draw.run_count>2);release_output(fallback_draw);
    NativeParagraphHits fallback_geometry{};TextMeasureResult fallback_measure{};
    NativeParagraphHitStorage fallback_storage{{hit_clusters,256},{hit_lines,257},{advances,1280},&fallback_geometry,{parts,16}};
    CHECK(native_text_candidate_hits(system,fallback_text,37,1000,TextWrapping::wrap,fallback_storage,&fallback_measure)&&fallback_measure.ok());
    const u32 normal_format=system.native_format;u32 bold_format=0;description.properties.weight=600;
    CHECK(native_text_format_create(system.native_text,description,&bold_format));
    for(u32 i=0;i<6;++i){system.native_format=(i&1)?bold_format:normal_format;auto draw=output();
        CHECK(native_text_candidate_line(system,utf8_literal("a\xcb\x91" "a"),1000000,1000,&draw,nullptr)&&draw.native_fonts());
        CHECK(draw.run_count==((i&1)?1u:3u));release_output(draw);}
    CHECK(!GetModuleHandleW(L"dwrite.dll")&&!GetModuleHandleW(L"DWriteCore.dll"));
    // Private face-zero registration is also independent of COM collections.
    CHECK(system_cache(system.native_fonts,path));
    u32 scope=0,again=0;CHECK(native_text_register_font_path(system.native_text,path,&scope));CHECK(native_text_register_font_path(system.native_text,path,&again)&&again==scope);
    const u16 private_family[]{'C','o','m','p','a','g','n','o','n'};description={{private_family,9},scope,{400,5,0},16,64,true};
    CHECK(native_text_format_create(system.native_text,description,&system.native_format));system.em_size=16;CHECK(exercise(system));
    auto invalid=system;invalid.native_format=0xffffffffu;TextMeasureResult measured{};CHECK(!native_text_candidate_line(invalid,utf8_literal("AV"),95,1000,nullptr,&measured));
    CHECK(!native_text_format_identity(invalid));CHECK(!GetModuleHandleW(L"dwrite.dll")&&!GetModuleHandleW(L"DWriteCore.dll"));
    // Published runs and copied editor geometry outlive all format metadata.
    auto retained=output();const auto text=utf8_literal("A \xce\xb1\xf0\x9f\x98\x80\xd0\x91 To");
    CHECK(native_text_candidate_line(system,text,1000000,1000,&retained,nullptr));
    NativeParagraphHits geometry{};NativeParagraphHitStorage storage{{hit_clusters,256},{hit_lines,257},{advances,1280},&geometry,{parts,16}};
    const TextStyleSpan style{0,text.bytes,text_style_underline};
    CHECK(native_text_candidate_styled(system,text,{&style,1},95,1000,nullptr,&measured,TextWrapping::wrap,TextOverflow::clip,{},&storage));
    NativeParagraphHitMetric before{},after{};float before_x=0,after_x=0;
    CHECK(native_paragraph_hit_position(geometry,3,false,&before,&before_x)==SfntStatus::ok);
    native_text_context_release(&system.native_text);CHECK(!system.native_text&&native_font_session_release(&system.native_fonts));
    CHECK(native_paragraph_hit_position(geometry,3,false,&after,&after_x)==SfntStatus::ok&&bytes_equal(&before,&after,sizeof(before))&&before_x==after_x);
    auto* session=retained.native_fonts();CHECK(session&&native_font_run_get(*native_font_session_runs(session),retained.runs[0].native_face));
    Arena scratch{};CHECK(arena_initialize(&scratch,4*1024*1024,0,4096));
    for(u32 i=0;i<retained.run_count;++i){NativeFontInstanceHandle instance{};
        CHECK(native_font_run_prepare(native_font_session_runs(session),retained.runs[i].native_face,&scratch,&instance).ok()&&instance.slot);
        CHECK(native_font_instance_release(native_font_session_instances(session),instance));}
    arena_release(&scratch);
    CHECK(native_font_session_stats(session).references==1);release_output(retained);CHECK(released);
    print("PASS native format ownership without DirectWrite\n");return true;
}
}}
extern "C" void maicord_native_format_entry(){ExitProcess(maicord::run()?0:1);}
