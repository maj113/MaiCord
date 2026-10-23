#include "maicord/ui/native_text_candidate.hpp"
#include "maicord/text/font_session.hpp"
#include "maicord/text/paragraph_hits.hpp"
#include "maicord/core/win32_kernel.hpp"
namespace maicord { namespace {
void print(const char* text) noexcept { u32 n=0; while(text[n])++n; DWORD done=0; WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,n,&done,nullptr); }
#define CHECK(x) do { if (!(x)) { print("FAIL flag native geometry: " #x "\n"); return false; } } while (0)
NativeParagraphHitCluster clusters[256]; NativeParagraphHitLine lines[257]; float advances[1280];
SfntFontLineParts parts[256]; NativeParagraphHitRun runs[256];
bool close(float value,float wanted) noexcept {return value>wanted-.01f&&value<wanted+.01f;}
NativeParagraphHitStorage storage(NativeParagraphHits* output) noexcept {
    return {{clusters,256},{lines,257},{advances,1280},output,{parts,256},{runs,256}};
}
bool atom(const TextSystem& system,Utf8View text,u32 flags,float extent) noexcept {
    TextStyleSpan style{0,text.bytes,flags}; NativeParagraphHits geometry{};TextMeasureResult measured{};
    auto retained = storage(&geometry);
    CHECK(native_text_candidate_styled(system,text,{&style,1},extent,128,nullptr,&measured,
        TextWrapping::wrap,TextOverflow::clip,{},&retained));
    CHECK(measured.ok()&&close(measured.width,extent)&&geometry.source_units==text.bytes/2&&geometry.lines.count==1);
    NativeParagraphHitMetric metric{};u32 count=0;
    CHECK(native_paragraph_hit_range(geometry,0,geometry.source_units,0,0,{&metric,1},&count)==SfntStatus::ok);
    CHECK(count==1&&close(metric.width,extent)&&metric.source_first==0&&metric.source_count==text.bytes/2);
    NativeParagraphHitPoint point{};
    CHECK(native_paragraph_hit_point(geometry,extent*.1f,metric.top+metric.height*.5f,&point)==SfntStatus::ok&&point.source_position==0);
    CHECK(native_paragraph_hit_point(geometry,extent*.9f,metric.top+metric.height*.5f,&point)==SfntStatus::ok&&point.source_position==geometry.source_units);
    return true;
}
bool run() noexcept {
    TextSystem system{};
    CHECK(native_font_session_create(&system.native_fonts)&&native_text_context_initialize(&system.native_text,system.native_fonts));
    struct Release {TextSystem& system;~Release() noexcept {native_text_context_release(&system.native_text);native_font_session_release(&system.native_fonts);}} release{system};
    const u16 family[]{'S','e','g','o','e',' ','U','I'};
    const NativeTextFormatDescription description{{family,8},0,{400,5,0},16,64,true};
    CHECK(native_text_format_create(system.native_text,description,&system.native_format));system.em_size=16;
    const auto tj=utf8_literal("\xF0\x9F\x87\xB9\xF0\x9F\x87\xAF");
    const auto tk=utf8_literal("\xF0\x9F\x87\xB9\xF0\x9F\x87\xB0");
    const auto england=utf8_literal("\xF0\x9F\x8F\xB4\xF3\xA0\x81\xA7\xF3\xA0\x81\xA2\xF3\xA0\x81\xA5\xF3\xA0\x81\xAE\xF3\xA0\x81\xA7\xF3\xA0\x81\xBF");
    CHECK(atom(system,tj,text_style_custom_emoji,16)&&atom(system,tk,text_style_custom_emoji,16)&&atom(system,england,text_style_custom_emoji,16));
    CHECK(atom(system,tj,text_style_custom_emoji|text_style_jumbo_emoji,48));
    // Compaction preserves ordinary clusters between and after projected flags.
    const auto mixed = utf8_literal("A\xF0\x9F\x87\xB9\xF0\x9F\x87\xAF" "B\xF0\x9F\x87\xB9\xF0\x9F\x87\xB0" "C");
    const TextStyleSpan mixed_styles[]{{1,8,text_style_custom_emoji},{10,8,text_style_custom_emoji}};
    NativeParagraphHits mixed_geometry{}; TextMeasureResult mixed_measure{};
    auto mixed_storage = storage(&mixed_geometry);
    CHECK(native_text_candidate_styled(system,mixed,{mixed_styles,2},256,128,nullptr,&mixed_measure,
        TextWrapping::wrap,TextOverflow::clip,{},&mixed_storage));
    CHECK(mixed_geometry.source_units==11&&mixed_geometry.lines.count==1);
    NativeParagraphHitMetric mixed_metric{};u32 mixed_count=0;
    for(u32 first=1;first<=6;first+=5) {
        CHECK(native_paragraph_hit_range(mixed_geometry,first,4,0,0,{&mixed_metric,1},&mixed_count)==SfntStatus::ok);
        CHECK(mixed_count==1&&close(mixed_metric.width,16)&&mixed_metric.source_first==first&&mixed_metric.source_count==4);
    }
    CHECK(native_paragraph_hit_range(mixed_geometry,10,1,0,0,{&mixed_metric,1},&mixed_count)==SfntStatus::ok&&
        mixed_count==1&&mixed_metric.width>0&&mixed_metric.source_first==10);

    const auto adjacent=utf8_literal("\xF0\x9F\x87\xB9\xF0\x9F\x87\xAF\xF0\x9F\x87\xB9\xF0\x9F\x87\xB0");
    const TextStyleSpan style{0,adjacent.bytes,text_style_custom_emoji};NativeParagraphHits geometry{};TextMeasureResult measured{};
    auto retained=storage(&geometry);
    CHECK(native_text_candidate_styled(system,adjacent,{&style,1},16,128,nullptr,&measured,TextWrapping::wrap,TextOverflow::clip,{},&retained));
    CHECK(geometry.source_units==8&&geometry.lines.count==2&&close(measured.width,16));
    NativeParagraphHitMetric metrics[2]{};u32 count=0;
    CHECK(native_paragraph_hit_range(geometry,0,8,0,0,{metrics,2},&count)==SfntStatus::ok&&count==2&&
        close(metrics[0].width,16)&&close(metrics[1].width,16)&&metrics[0].top<metrics[1].top);
    NativeParagraphHitMetric prior{};float prior_x=0;
    CHECK(native_paragraph_hit_position(geometry,4,false,&prior,&prior_x)==SfntStatus::ok);
    // Warm refitting and independent measurement preserve the source atom.
    CHECK(native_text_candidate_styled(system,adjacent,{&style,1},32,128,nullptr,&measured,TextWrapping::wrap)&&close(measured.width,32));
    system.independent_letters=true;
    CHECK(atom(system,tj,text_style_custom_emoji,16));system.independent_letters=false;
    TextStyleSpan code{0,tj.bytes,text_style_monospace};
    CHECK(native_text_candidate_styled(system,tj,{&code,1},128,128,nullptr,&measured,TextWrapping::wrap)&&measured.width>0&&measured.utf16_units==4);
    CHECK(!GetModuleHandleW(L"dwrite.dll")&&!GetModuleHandleW(L"DWriteCore.dll"));
    print("PASS flag native geometry: true RI/tag ranges, em cells, wrap, warm retention and code control\n");return true;
}
} }
extern "C" void maicord_flag_native_geometry_entry(){ExitProcess(maicord::run()?0:1);}
