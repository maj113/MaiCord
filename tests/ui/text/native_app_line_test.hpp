// State and helpers shared by the native app line driver's units: native_app_line_test.cpp (modes, run()),
// native_app_line_scripts.cpp and native_app_line_editing.cpp (corpora). Every layout, measure, hit and editor result
// the corpora make is observed into the committed references (app_line_reference.hpp); the native engine's own
// invariants (dispatch, capacities, recovery, shape against measure) are checked directly.
#pragma once
#include "maicord/text/unicode/detail/thai_lexical.hpp"
#include "maicord/text/unicode/detail/thai_syllable.hpp"
#include "../../../src/ui/text/native_text_span.hpp"
#include "../../../src/ui/text/native_text_script.hpp"
#include "maicord/text/unicode/detail/thai_word_break.hpp"
#include "maicord/text/unicode/detail/line_break.hpp"
#include "maicord/text/unicode/grapheme.hpp"
#include "maicord/ui/text/native_text_candidate.hpp"
#include "maicord/ui/media/emoji_presentation.hpp"
#include "maicord/ui/text/native_text_catalog.hpp"
#include "../../../src/ui/text/native_text_catalog_owner.hpp"
#include "maicord/render/text/edit_layout.hpp"
#include "maicord/text/fonts/run.hpp"
#include "maicord/text/fonts/session.hpp"
#include "maicord/text/scripts/ascii_paragraph.hpp"
#include "maicord/text/layout/detail/paragraph_layout.hpp"
#include "maicord/text/scripts/latin_paragraph.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "app_line_reference.hpp"
namespace maicord::native_app_line {
inline u32 checks=0,cases=0,native_cases=0,simulated_cases=0;inline bool print_ok=true;
inline u64 matrix_seen=0;
inline bool paragraph_profiles=false,paragraph_expansion=false;
inline bool hit_profiles=false;
inline bool latin_profiles=false;
inline bool latin_paragraph_profiles=false,latin_paragraph_hits=false;
inline bool style_profiles=false,decoration_profiles=false;
inline bool semantic_profiles=false,directional_semantic_profiles=false,directional_large_profiles=false,directional_editor_profiles=false,directional_editor_cuts_profiles=false;
inline bool styled_hit_profiles=false;
inline bool style_cluster_profiles=false;
inline bool style_continuity_profiles=false,tracking_profiles=false;
inline bool alphabetic_profiles=false;
inline bool simulated_profiles=false;
inline bool system_fallback_profiles=false;
inline bool hebrew_profiles=false,arabic_paragraph_profiles=false,arabic_editor_profiles=false;
// The system under test: the production text system plus the catalog scope its formats name a family in (zero is
// the system catalog, otherwise a font file registered with native_text_register_font_path).
struct AppSystem : TextSystem {u32 scope;};
bool native_arabic_paragraph_corpus(AppSystem,bool) noexcept;
inline bool cjk_paragraph_profiles=false,cjk_editor_profiles=false,cjk_editor_paint_profiles=false,height_overflow_profiles=false;
bool native_height_overflow_corpus(AppSystem) noexcept;
inline bool ellipsis_sign_profiles=false;
inline bool ellipsis_cut_profiles=false;
inline bool ellipsis_line_profiles=false;
inline bool ellipsis_style_profiles=false;
inline bool ellipsis_hit_profiles=false;
inline bool multiline_trim_profiles=false,multiline_hit_profiles=false,multiline_quick=false;
bool native_multiline_trim_corpus(AppSystem) noexcept;
bool native_ellipsis_style_corpus(AppSystem) noexcept;
inline bool common_punctuation_profiles=false;
bool native_ellipsis_sign_corpus(AppSystem) noexcept;
bool native_ellipsis_cut_corpus(AppSystem) noexcept;
bool native_common_punctuation_corpus(AppSystem) noexcept;
bool native_cjk_paragraph_corpus(AppSystem,bool) noexcept;
bool native_cjk_editor_paint_corpus(AppSystem) noexcept;
inline bool hangul_profiles=false;
bool native_hangul_corpus(AppSystem) noexcept;
inline bool thai_profiles=false,thai_shape_only=false;
bool native_thai_corpus(AppSystem) noexcept;
inline bool standard_script_profiles=false,extended_standard_profiles=false;
bool native_standard_script_corpus(AppSystem) noexcept;
inline bool tamil_profiles=false,tamil_shape_only=false;
inline bool bengali_profiles=false,gujarati_profiles=false,gurmukhi_profiles=false,odia_profiles=false,telugu_profiles=false,kannada_profiles=false,malayalam_profiles=false,sinhala_profiles=false,khmer_profiles=false,myanmar_profiles=false,tibetan_profiles=false;
inline bool devanagari_profiles=false,devanagari_tracking=false,devanagari_directional=false,arabic_context_profiles=false;
bool native_devanagari_corpus(AppSystem) noexcept;
inline bool hebrew_wrap_profiles=false;
inline bool mixed_bidi_profiles=false,mixed_bidi_wrap=false;
inline bool bidi_styles_profiles=false,bidi_emoji_profiles=false,symbol_profiles=false;
bool native_bidi_styles_corpus(AppSystem) noexcept;
bool native_mixed_bidi_corpus(AppSystem) noexcept;
bool native_hebrew_corpus(AppSystem) noexcept;
bool native_system_fallback_corpus(AppSystem,bool,bool) noexcept;
bool native_simulated_profile_corpus(AppSystem) noexcept;
bool alphabetic_corpus(AppSystem,bool) noexcept;
#include "latin_line_cases.inc"
#include "latin_paragraph_cases.inc"
inline bool compare_hits(const AppSystem&,Utf8View,float,TextWrapping,ConstSlice<TextStyleSpan> styles={}) noexcept;
bool directional_editor_hit_corpus(AppSystem,bool cuts=false) noexcept;
bool styled_hit_corpus(AppSystem,bool) noexcept;
bool style_cluster_corpus(AppSystem,bool) noexcept;
bool style_continuity_corpus(AppSystem) noexcept;
bool native_tracking_corpus(AppSystem) noexcept;
// Corpora run() selects, defined in native_app_line_scripts.cpp and native_app_line_editing.cpp.
bool native_han_paragraph_corpus() noexcept;
bool native_mixed_bidi_gates() noexcept;
bool native_bidi_style_range_probe(bool check) noexcept;
bool inspect_style_clusters() noexcept;
bool retained_editor_capacity(u32 sample) noexcept;
bool directional_edit() noexcept;
bool directional_edit_cuts() noexcept;
bool styled_edit() noexcept;
bool native_format_corpus() noexcept;
bool native_emoji_dispatch_corpus(const wchar_t* path,bool mixed=false) noexcept;
bool native_mixed_emoji_corpus() noexcept;
bool native_fallback_binding_corpus() noexcept;
bool native_simulated_style_corpus() noexcept;
inline const Utf8View paragraph_samples[]{utf8_literal("AV AV"),utf8_literal("AV"),utf8_literal("fi fi"),utf8_literal("ffi"),utf8_literal("a\r\nb"),utf8_literal("a\nb"),utf8_literal("a\n"),utf8_literal("\n"),utf8_literal("a  b"),utf8_literal(" a b"),utf8_literal("a\tb"),utf8_literal(" \t "),utf8_literal("a-b/c"),utf8_literal("a...b"),utf8_literal("(a b) c"),utf8_literal("   "),utf8_literal("a     "),utf8_literal("Hello world\nThis is an ordinary wrapped paragraph."),utf8_literal(" a abcdefghijklmnopqrstuvwxyz 0123456789"),utf8_literal("a\t\tb\r\n\n\t\n")};
inline void print(const char* text) noexcept {u32 n=0;while(text[n])++n;DWORD written=0;print_ok=print_ok&&WriteFile(GetStdHandle(STD_OUTPUT_HANDLE),text,n,&written,nullptr)&&written==n;}
inline void number(u64 v) noexcept {char text[24];u32 n=0;do{text[n++]=static_cast<char>('0'+v%10);v/=10;}while(v);for(u32 i=0;i<n/2;++i){const char c=text[i];text[i]=text[n-i-1];text[n-i-1]=c;}text[n]=0;print(text);}
inline void real(float value) noexcept {u32 bits=0;copy_bytes(&bits,&value,4);number(bits);}
#define CHECK(x) do{++checks;if(!(x)){print("FAIL line=");number(__LINE__);print(" case=");number(cases);print("\n");return false;}}while(0)
struct Output {TextGlyph glyphs[1024];TextRun runs[256];u16 clusters[256];TextDecoration decorations[256];TextHitRegion hits[4096];ShapedText text;};
inline Output native_output{},second_output{};inline AppSystem owner{};
inline Arena draw_scratch{};inline bool draw_scratch_ready=true;
// Draw preparations retain a few prepared owner instances, so repeated preparations reuse the already executed
// TrueType font/CVT programs instead of rebuilding an identical instance per case.
struct DrawRetained {NativeFontSession* session;NativeFontInstanceHandle handle;};
inline DrawRetained draw_retained[8]{};inline u32 draw_retained_next=0;
inline bool draw_retain(NativeFontSession* session,NativeFontInstanceHandle handle) noexcept {
    for(const auto& r:draw_retained)if(r.session==session&&r.handle.slot==handle.slot&&r.handle.generation==handle.generation)return true;
    auto& r=draw_retained[draw_retained_next++&7u];
    if(r.session&&!native_font_instance_release(native_font_session_instances(r.session),r.handle))return false;
    r={};if(!native_font_instance_retain(native_font_session_instances(session),handle))return false;r={session,handle};return true;
}
inline bool draw_retained_release() noexcept {
    bool ok=true;for(auto& r:draw_retained){if(r.session)ok=native_font_instance_release(native_font_session_instances(r.session),r.handle)&&ok;r={};}return ok;
}
// The renderer prepares a native run's outlines from its run handle alone: the first run prepares (with the
// simulations its record permits, observed), a stale generation and a missing store are refused.
inline bool draw_source(const TextSystem& system,const ShapedText& text) noexcept {
    CHECK(text.native_fonts()==system.native_fonts&&text.run_count&&text.runs[0].native_font);
    const auto font=text.runs[0].native_face;
    CHECK(draw_scratch_ready);if(!draw_scratch.base)CHECK(arena_initialize(&draw_scratch,4*1024*1024,0,4096));
    NativeFontInstanceHandle prepared{},rejected{};bool oblique=false,bold=false;
    struct Release {Arena& a;NativeFontSession* session;NativeFontInstanceHandle& x;~Release() noexcept {
        if(!x.slot||native_font_instance_release(native_font_session_instances(session),x))arena_reset(&a);else draw_scratch_ready=false;}} release{draw_scratch,system.native_fonts,prepared};
    CHECK(native_font_run_prepare(native_font_session_runs(system.native_fonts),font,&draw_scratch,&prepared,&oblique,&bold).ok());
    if(system.native_fonts==owner.native_fonts)CHECK(draw_retain(system.native_fonts,prepared));
    observe_value(static_cast<u32>(oblique|bold<<1));
    CHECK(!native_font_run_prepare(nullptr,font,&draw_scratch,&rejected).ok()&&!rejected.slot);
    CHECK(!native_font_run_prepare(native_font_session_runs(system.native_fonts),{font.slot,font.generation+1},&draw_scratch,&rejected).ok()&&!rejected.slot);
    return true;
}
// With --bundled-fonts, the system families the corpora name map to the repository's fonts in their private scopes
// (Segoe UI to MuseoModerno, Georgia to Zilla Slab, Consolas to BioRhyme), so their layouts are the same everywhere.
struct BundledFamily {const wchar_t* system;wchar_t name[256];u32 scope;};
inline BundledFamily bundled_families[3]{{L"Segoe UI",{},0},{L"Georgia",{},0},{L"Consolas",{},0}};
inline bool bundled_mode;
// Points `system` at a new format of `family` in its scope (zero: the system catalog) at `em` pixels, with weight,
// style (0 normal, 2 italic), the incremental tab stop (zero: four em, the default) and wrapping. em_size, the size
// styled spans scale from, is the caller's.
inline bool set_format(AppSystem& system,const wchar_t* family,float em,u16 weight=400,u8 style=0,float tab=0,bool wrap=true) noexcept {
    if(bundled_mode)for(const auto& b:bundled_families){u32 i=0;while(b.system[i]&&b.system[i]==family[i])++i;
        if(!b.system[i]&&!family[i]){family=b.name;system.scope=b.scope;break;}}
    u16 name[256];u32 n=0;while(family[n]){if(n==255)return false;name[n]=static_cast<u16>(family[n]);++n;}
    return native_text_format_create(system.native_text,{{name,n},system.scope,{weight,5,style},em,tab?tab:em*4,wrap},&system.native_format);
}
inline void observe(const ShapedText& text,Utf8View source,const TextSystem& system) noexcept {
    observe_shape(text,source,system.independent_letters,system.tracking);
}
inline void observe(TextShapeResult result) noexcept {const u32 words[2]{result.utf16_units,static_cast<u32>(result.error)};observe_bytes(words,8);}
inline bool same_metrics(const TextMeasureResult& measure,const ShapedText& text) noexcept {
    return bytes_equal(&measure.width,&text.width,12)&&measure.line_count==text.line_count;
}
// One layout through the shipping entry points, observed: optionally the specialized line path first (which must
// accept it and draw), then shape and measure, which agree unless the shape trimmed with an ellipsis.
inline bool compare(TextSystem system,Utf8View input,bool require_native,float width=1000000,float height=1000000,TextWrapping wrapping=TextWrapping::wrap,TextOverflow overflow=TextOverflow::clip) noexcept {
    ++cases;shaped_text_reset(&native_output.text);
    if(require_native){CHECK(native_text_candidate_line(system,{input,{},width,height,wrapping,overflow},&native_output.text,nullptr));CHECK(draw_source(system,native_output.text));++native_cases;shaped_text_reset(&native_output.text);}
    const auto shaped=text_shape_utf8(&system,{input,{},width,height,wrapping,overflow},&native_output.text);
    observe(shaped);if(shaped.ok())observe(native_output.text,input,system);
    const auto measured=text_measure_utf8(&system,{input,{},width,height,wrapping});observe_measure(measured);
    CHECK(measured.error==shaped.error&&measured.utf16_units==shaped.utf16_units);
    if(shaped.ok()&&overflow==TextOverflow::clip)CHECK(same_metrics(measured,native_output.text));
    return true;
}
// A styled shape through the shipping entry point, observed; false unless it succeeds natively (any run with an
// engine font: empty text has none).
inline bool shape_observed(TextSystem system,Utf8View text,ConstSlice<TextStyleSpan> styles,float width,TextWrapping wrapping,float height=1000000,TextOverflow overflow=TextOverflow::clip) noexcept {
    const auto shaped=text_shape_utf8(&system,{text,styles,width,height,wrapping,overflow},&native_output.text);
    if(!shaped.ok()||(native_output.text.run_count&&!native_output.text.native_fonts()))return false;
    observe(native_output.text,text,system);return true;
}
// The same shape plus its measure, which agrees on the clipped layout's metrics.
inline bool layout(TextSystem system,Utf8View text,ConstSlice<TextStyleSpan> styles,float width,TextWrapping wrapping,float height=1000000) noexcept {
    CHECK(shape_observed(system,text,styles,width,wrapping,height));
    const auto measured=text_measure_utf8(&system,{text,styles,width,height,wrapping});observe_measure(measured);
    CHECK(measured.ok()&&measured.utf16_units==native_output.text.cluster_count&&same_metrics(measured,native_output.text));
    return true;
}
#include "semantic_ranges_cases.inc"
#include "paragraph_hits_units.inc"
#include "edit_hits_units.inc"
} // namespace maicord::native_app_line
