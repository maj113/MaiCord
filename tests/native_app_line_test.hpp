// State and comparison helpers shared by the native app line driver's units:
// native_app_line_test.cpp (modes, run()), native_app_line_scripts.cpp and
// native_app_line_editing.cpp (corpora).
#pragma once
#include "maicord/text/detail/thai_lexical.hpp"
#include "maicord/text/detail/thai_syllable.hpp"
#include "../src/ui/native_text_trim_plan.hpp"
#include "../src/ui/native_text_span.hpp"
#include "../src/ui/native_text_script.hpp"
#include "maicord/text/detail/thai_word_break.hpp"
#include "maicord/text/detail/line_break_tailored.hpp"
#include "maicord/text/grapheme.hpp"
#include "maicord/ui/native_text_candidate.hpp"
#include "maicord/ui/emoji_presentation.hpp"
#include "maicord/ui/native_text_catalog.hpp"
#include "../src/ui/native_text_catalog_owner.hpp"
#include "maicord/render/text_edit_layout.hpp"
#include "../tools/font_reference/dwrite_text_system.hpp"
#include "../tools/font_reference/native_run_reference.hpp"
#include "../tools/font_reference/ascii_break_reference.hpp"
#include "maicord/text/ascii_paragraph.hpp"
#include "maicord/text/detail/paragraph_layout.hpp"
#include "maicord/text/latin_paragraph.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/sys/dwrite.hpp"
#include "maicord/sys/simd.hpp"
#include "maicord/core/win32_exception.hpp"
namespace maicord::native_app_line {
inline u32 checks=0,cases=0,native_cases=0,simulated_cases=0;inline bool print_ok=true;
inline u64 matrix_seen=0;
inline u32 invisible_offset_differences=0;
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
bool native_arabic_paragraph_corpus(DWriteTextSystem,bool) noexcept;
inline bool cjk_paragraph_profiles=false,cjk_editor_profiles=false,cjk_editor_paint_profiles=false,height_overflow_profiles=false;
bool native_height_overflow_corpus(DWriteTextSystem) noexcept;
inline bool ellipsis_sign_profiles=false;
inline bool ellipsis_cut_profiles=false;
inline bool ellipsis_line_profiles=false;
inline bool ellipsis_style_profiles=false;
inline bool ellipsis_hit_profiles=false;
inline bool multiline_trim_profiles=false,multiline_plan_profiles=false,multiline_hit_profiles=false,multiline_quick=false;
bool native_multiline_trim_corpus(DWriteTextSystem) noexcept;
bool native_ellipsis_style_corpus(DWriteTextSystem) noexcept;
inline bool common_punctuation_profiles=false;
bool native_ellipsis_sign_corpus(DWriteTextSystem) noexcept;
bool native_ellipsis_cut_corpus(DWriteTextSystem) noexcept;
bool native_common_punctuation_corpus(DWriteTextSystem) noexcept;
bool native_cjk_paragraph_corpus(DWriteTextSystem,bool) noexcept;
bool native_cjk_editor_paint_corpus(DWriteTextSystem) noexcept;
inline bool hangul_profiles=false;
bool native_hangul_corpus(DWriteTextSystem) noexcept;
inline bool thai_profiles=false,thai_shape_only=false;
bool native_thai_corpus(DWriteTextSystem) noexcept;
inline bool standard_script_profiles=false,extended_standard_profiles=false;
bool native_standard_script_corpus(DWriteTextSystem) noexcept;
inline bool tamil_profiles=false,tamil_shape_only=false;
inline bool bengali_profiles=false,gujarati_profiles=false,gurmukhi_profiles=false,odia_profiles=false,telugu_profiles=false,kannada_profiles=false,malayalam_profiles=false,sinhala_profiles=false,khmer_profiles=false,myanmar_profiles=false,tibetan_profiles=false;
inline bool devanagari_profiles=false,devanagari_tracking=false,devanagari_directional=false,arabic_context_profiles=false;
bool native_devanagari_corpus(DWriteTextSystem) noexcept;
inline bool hebrew_wrap_profiles=false;
inline bool mixed_bidi_profiles=false,mixed_bidi_wrap=false;
inline bool bidi_styles_profiles=false,bidi_emoji_profiles=false,symbol_profiles=false;
bool native_bidi_styles_corpus(DWriteTextSystem) noexcept;
bool native_mixed_bidi_corpus(DWriteTextSystem) noexcept;
bool native_hebrew_corpus(DWriteTextSystem) noexcept;
bool native_system_fallback_corpus(DWriteTextSystem,bool,bool) noexcept;
bool native_simulated_profile_corpus(DWriteTextSystem) noexcept;
bool alphabetic_corpus(DWriteTextSystem,bool) noexcept;
#include "latin_line_cases.inc"
#include "latin_paragraph_cases.inc"
inline bool compare_hits(DWriteTextSystem,Utf8View,float,TextWrapping,ConstSlice<TextStyleSpan> styles={},bool detached=false,bool tracked_layout=false,bool captured_format=false) noexcept;
bool directional_editor_hit_corpus(DWriteTextSystem,bool cuts=false) noexcept;
bool styled_hit_corpus(DWriteTextSystem,bool) noexcept;
bool style_cluster_corpus(DWriteTextSystem,bool) noexcept;
bool style_continuity_corpus(DWriteTextSystem) noexcept;
bool native_tracking_corpus(DWriteTextSystem) noexcept;
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
inline Output native_output{},reference_output{};inline wchar_t utf16[256];inline DWriteTextSystem owner{};
struct OracleFace {FILE_ID_INFO identity;u32 file_bytes,face_index,axis_count,simulations;DWRITE_FONT_AXIS_VALUE axes[2];IDWriteFactory* factory;IDWriteFontFace5* face;u32 draw_requests;bool draw_checked;};
inline OracleFace oracle_faces[64]{};inline u32 oracle_next=0;
inline u64 oracle_draw_reuses=0,oracle_draw_controls=0,oracle_draw_builds=0,oracle_draw_aliases=0;
inline u64 reference_measure_controls=0,reference_measure_reuses=0,repeated_matrix_cases=0;
inline Arena draw_scratch{};inline u64 draw_scratch_uses=0,draw_scratch_reservations=0;inline bool draw_scratch_ready=true;
inline void oracle_faces_release() noexcept {for(auto& value:oracle_faces){com_release(value.face);com_release(value.factory);value={};}oracle_next=0;}
// Draw controls rebind every native run through both its run handle and the
// selected DirectWrite face. Retain a few prepared owner instances so repeated
// controls reuse the already executed TrueType font/CVT programs instead of
// rebuilding an identical instance per case. The run-handle path is still
// prepared and checked on every call.
struct DrawRetained {NativeFontSession* session;NativeFontInstanceHandle handle;};
inline DrawRetained draw_retained[8]{};inline u32 draw_retained_next=0;inline u64 draw_retained_reuses=0;
inline bool draw_retain(NativeFontSession* session,NativeFontInstanceHandle handle) noexcept {
    for(const auto& r:draw_retained)if(r.session==session&&r.handle.slot==handle.slot&&r.handle.generation==handle.generation){++draw_retained_reuses;return true;}
    auto& r=draw_retained[draw_retained_next++&7u];
    if(r.session&&!native_font_instance_release(native_font_session_instances(r.session),r.handle))return false;
    r={};if(!native_font_instance_retain(native_font_session_instances(session),handle))return false;r={session,handle};return true;
}
inline bool draw_retained_release() noexcept {
    bool ok=true;for(auto& r:draw_retained){if(r.session)ok=native_font_instance_release(native_font_session_instances(r.session),r.handle)&&ok;r={};}return ok;
}
// Binding a selected DirectWrite face reopens its file by path to prove the
// physical identity. The same cached oracle face bound against the same
// retained instance is an identical request: bind it on first use, after any
// face/instance change and on every 64th repeat as an independent control.
struct DrawBound {IDWriteFontFace* face;NativeFontInstanceHandle instance;u32 repeats;bool oblique,bold;};
inline DrawBound draw_bound[8]{};inline u32 draw_bound_next=0;inline u64 draw_bind_controls=0,draw_bind_reuses=0;
inline void draw_bound_release() noexcept {for(auto& b:draw_bound){com_release(b.face);b={};}}
inline void print_face_file(IDWriteFontFace* face) noexcept {
    u32 count=1;IDWriteFontFile* file=nullptr;if(FAILED(face->GetFiles(&count,&file))||count!=1||!file)return;
    IDWriteFontFileLoader* loader=nullptr;IDWriteLocalFontFileLoader* local=nullptr;const void* key=nullptr;u32 size=0,length=0;wchar_t path[1024]{};
    if(SUCCEEDED(file->GetLoader(&loader))&&SUCCEEDED(loader->QueryInterface(__uuidof(IDWriteLocalFontFileLoader),reinterpret_cast<void**>(&local)))&&
       SUCCEEDED(file->GetReferenceKey(&key,&size))&&SUCCEEDED(local->GetFilePathLengthFromKey(key,size,&length))&&length<1024&&SUCCEEDED(local->GetFilePathFromKey(key,size,path,1024))){
        print("font file=");for(u32 i=0;i<length;++i){const char c[2]{static_cast<char>(path[i]<128?path[i]:'?'),0};print(c);}print(" index/simulation=");number(face->GetIndex());print("/");number(face->GetSimulations());print("\n");}
    com_release(local);com_release(loader);com_release(file);
}
inline bool oracle_live_recipe(IDWriteFactory* factory,const ShapedText& text,const TextRun& run,OracleFace* recipe) noexcept {
    // Validate the current run/font generations and live mapped file on every
    // call. Only the immutable physical construction recipe identifies a face.
    CHECK(factory&&recipe&&run.native_font&&text.native_fonts());
    const auto* record=native_font_run_get(*native_font_session_runs(text.native_fonts()),run.native_face);
    const auto* store=native_font_session_fonts(text.native_fonts());
    const auto* font=record&&store?native_font_get(*store,record->source.face):nullptr;CHECK(record&&font);
    const u32 file_slot=store->faces[record->source.face.slot-1].file_slot;CHECK(file_slot<store->files.count);
    const auto& physical=store->files[file_slot];
    CHECK(physical.faces&&physical.file&&physical.file!=INVALID_HANDLE_VALUE&&physical.bytes.data&&physical.bytes.count);
    CHECK(record->source.count<=2);*recipe={};recipe->factory=factory;copy_bytes(&recipe->identity,&physical.identity,sizeof(physical.identity));recipe->file_bytes=physical.bytes.count;
    recipe->face_index=font->geometry.face.face_index;recipe->axis_count=record->source.count;recipe->simulations=record->simulations;
    for(u32 i=0;i<record->source.count;++i){const auto setting=record->source.settings[i];const u32 tag=setting.tag;
        recipe->axes[i]={static_cast<DWRITE_FONT_AXIS_TAG>(((tag&255)<<24)|((tag&0xff00)<<8)|((tag>>8)&0xff00)|(tag>>24)),static_cast<float>(setting.value)};}
    return true;
}
inline OracleFace* oracle_find_recipe(const OracleFace& recipe) noexcept {
    for(auto& value:oracle_faces)if(value.face&&value.factory==recipe.factory&&value.file_bytes==recipe.file_bytes&&value.face_index==recipe.face_index&&
        value.axis_count==recipe.axis_count&&value.simulations==recipe.simulations&&
        bytes_equal(&value.identity,&recipe.identity,sizeof(value.identity))&&bytes_equal(value.axes,recipe.axes,recipe.axis_count*sizeof(*recipe.axes)))return &value;
    return nullptr;
}
inline bool oracle_construct_face(IDWriteFactory* factory,const ShapedText& text,const TextRun& run,IDWriteFontFace5** output) noexcept {
    CHECK(output);*output=nullptr;IDWriteFontFace* selected=nullptr;
    CHECK(native_run_reference(factory,text,run,&selected));
    const auto queried=selected->QueryInterface(__uuidof(IDWriteFontFace5),reinterpret_cast<void**>(output));com_release(selected);
    CHECK(SUCCEEDED(queried)&&*output);return true;
}
inline OracleFace* oracle_store_face(const OracleFace& recipe,OracleFace* cached,IDWriteFontFace5* face) noexcept {
    auto& value=cached?*cached:oracle_faces[oracle_next++%64];com_release(value.face);com_release(value.factory);
    value=recipe;value.factory->AddRef();value.face=face;return &value;
}
inline bool same_native_face(const ShapedText& text,const TextRun& run,IDWriteFontFace* reference) noexcept {
    OracleFace recipe{};OracleFace* cached=nullptr;
    if(run.native_font){CHECK(oracle_live_recipe(owner.factory,text,run,&recipe));cached=oracle_find_recipe(recipe);
        // Compare to the independently shaped current reference on every run.
        if(cached&&cached->face->Equals(reference)!=FALSE)return true;}
    // A non-equal path alias still reconstructs from the current live path.
    IDWriteFontFace5* face=nullptr;CHECK(oracle_construct_face(owner.factory,text,run,&face));
    const bool equal=face->Equals(reference)!=FALSE;
    if(!equal){print_face_file(face);print_face_file(reference);}
    if(run.native_font)oracle_store_face(recipe,cached,face);else com_release(face);return equal;
}
inline bool oracle_draw_face(IDWriteFactory* factory,const ShapedText& text,const TextRun& run,IDWriteFontFace** output) noexcept {
    CHECK(output);*output=nullptr;OracleFace recipe{};CHECK(oracle_live_recipe(factory,text,run,&recipe));
    auto* cached=oracle_find_recipe(recipe);
    if(!cached){IDWriteFontFace5* fresh=nullptr;CHECK(oracle_construct_face(factory,text,run,&fresh));
        cached=oracle_store_face(recipe,nullptr,fresh);++oracle_draw_builds;}
    // First draw for each recipe and periodic hits retain the real current-path
    // constructor as an independent control, refreshing non-equal aliases.
    if(!cached->draw_checked||(++cached->draw_requests&255u)==0){
        IDWriteFontFace5* fresh=nullptr;CHECK(oracle_construct_face(factory,text,run,&fresh));++oracle_draw_controls;
        if(cached->face->Equals(fresh)==FALSE){com_release(cached->face);cached->face=fresh;fresh=nullptr;++oracle_draw_aliases;}
        com_release(fresh);cached->draw_checked=true;
    }else ++oracle_draw_reuses;
    cached->face->AddRef();*output=cached->face;return true;
}
inline void oracle_draw_report() noexcept {
    if(!oracle_draw_controls&&!oracle_draw_reuses)return;
    print("oracle_draw_reuses=");number(oracle_draw_reuses);print(" oracle_draw_controls=");number(oracle_draw_controls);
    print(" oracle_draw_builds=");number(oracle_draw_builds);print(" oracle_draw_aliases=");number(oracle_draw_aliases);print("\n");
}
inline bool reference_shape_measure(DWriteTextSystem& reference,Utf8View input,ConstSlice<TextStyleSpan> styles,float width,float height,
    Slice<wchar_t> scratch,TextWrapping wrapping,bool control,TextMeasureResult* output) noexcept {
    CHECK(output&&!reference.native_text&&!reference_output.text.native_fonts());
    // The independent shape already captured DirectWrite GetMetrics for this
    // exact request. Keep every native measure; avoid rebuilding its oracle.
    const auto& shaped=reference_output.text;
    *output={S_OK,shaped.width,shaped.width_including_trailing_whitespace,shaped.height,shaped.cluster_count,shaped.line_count,TextShapeError::none};
    // Caller controls cover font/em/style/typography/wrapping cells and cut
    // boundaries; additional controls are spread across the entire matrix.
    control=control||((reference_measure_controls+reference_measure_reuses)&255u)==0;
    if(control){++reference_measure_controls;const auto measured=text_measure_utf8(&reference,input,styles,width,height,scratch,wrapping);
        CHECK(measured.ok()&&bytes_equal(&measured.width,&output->width,12)&&measured.utf16_units==output->utf16_units&&measured.line_count==output->line_count);
        *output=measured;
    }else ++reference_measure_reuses;
    return true;
}
inline bool draw_source(const DWriteTextSystem& system,const ShapedText& text) noexcept {
    CHECK(text.native_fonts()==system.native_fonts&&text.run_count&&text.runs[0].native_font);
    const auto font=text.runs[0].native_face;IDWriteFontFace* selected=nullptr;
    CHECK(oracle_draw_face(system.factory,text,text.runs[0],&selected));
    struct FaceRelease {IDWriteFontFace*& f;~FaceRelease() noexcept {com_release(f);}} face_release{selected};
    CHECK(draw_scratch_ready);if(!draw_scratch.base){CHECK(arena_initialize(&draw_scratch,4*1024*1024,0,4096));++draw_scratch_reservations;}
    auto& scratch=draw_scratch;++draw_scratch_uses;NativeFontInstanceHandle reused{},bridged{};
    struct Release {Arena& a;NativeFontSession* session;NativeFontInstanceHandle& x;NativeFontInstanceHandle& y;~Release() noexcept {auto* s=native_font_session_instances(session);
        const bool rx=!x.slot||native_font_instance_release(s,x),ry=!y.slot||native_font_instance_release(s,y);
        if(rx&&ry)arena_reset(&a);else draw_scratch_ready=false;}} release{scratch,system.native_fonts,reused,bridged};
    bool a_oblique=false,a_bold=false,b_oblique=false,b_bold=false;
    CHECK(native_font_run_prepare(native_font_session_runs(system.native_fonts),font,&scratch,&reused,&a_oblique,&a_bold).ok());
    const bool retained=system.native_fonts==owner.native_fonts;if(retained)CHECK(draw_retain(system.native_fonts,reused));
    DrawBound* bound=nullptr;
    if(retained)for(auto& b:draw_bound)if(b.face==selected&&b.instance.slot==reused.slot&&b.instance.generation==reused.generation)bound=&b;
    if(bound&&(++bound->repeats&63u)){b_oblique=bound->oblique;b_bold=bound->bold;++draw_bind_reuses;}
    else{++draw_bind_controls;
        CHECK(native_font_bind_selected(system.native_fonts,selected,true,&scratch,&bridged,&b_oblique,&b_bold).ok());
        CHECK(reused.slot==bridged.slot&&reused.generation==bridged.generation);
        CHECK(!bound||(b_oblique==bound->oblique&&b_bold==bound->bold));
        if(retained&&!bound){bound=&draw_bound[draw_bound_next++&7u];com_release(bound->face);selected->AddRef();
            *bound={selected,reused,0,b_oblique,b_bold};}}
    CHECK(a_oblique==b_oblique&&a_bold==b_bold);
    NativeFontInstanceHandle rejected{};
    CHECK(!native_font_run_prepare(nullptr,font,&scratch,&rejected).ok()&&!rejected.slot);
    CHECK(!native_font_run_prepare(native_font_session_runs(system.native_fonts),{font.slot,font.generation+1},&scratch,&rejected).ok()&&!rejected.slot);
    return true;
}
inline bool same_visual_runs(Output&,Output&) noexcept;
inline bool describe_run_font(const ShapedText& text,u32 glyph) noexcept {
    u32 run=0;while(run<text.run_count&&text.runs[run].first_glyph+text.runs[run].glyph_count<=glyph)++run;CHECK(run<text.run_count);
    IDWriteFontFace* selected=nullptr;CHECK(native_run_reference(owner.factory,text,text.runs[run],&selected));IDWriteFontFace3* face=nullptr;
    const auto status=selected->QueryInterface(__uuidof(IDWriteFontFace3),reinterpret_cast<void**>(&face));com_release(selected);CHECK(SUCCEEDED(status));
    print("font weight/style/simulation=");number(face->GetWeight());print("/");number(face->GetStyle());print("/");number(face->GetSimulations());
    IDWriteLocalizedStrings* names=nullptr;const auto named=face->GetFamilyNames(&names);com_release(face);CHECK(SUCCEEDED(named));
    wchar_t wide[128]{};const auto found=names->GetString(0,wide,128);com_release(names);CHECK(SUCCEEDED(found));char narrow[128];u32 i=0;
    while(i<127&&wide[i]){narrow[i]=wide[i]<128?static_cast<char>(wide[i]):'?';++i;}narrow[i]=0;print(" family=");print(narrow);print("\n");return true;
}
inline bool native_empty_glyph(const ShapedText& text,u32 glyph) noexcept {
    u32 run=0;while(run<text.run_count&&text.runs[run].first_glyph+text.runs[run].glyph_count<=glyph)++run;
    CHECK(run<text.run_count&&text.runs[run].native_font&&text.native_fonts());
    const auto* record=native_font_run_get(*native_font_session_runs(text.native_fonts()),text.runs[run].native_face);CHECK(record);
    const auto* font=native_font_get(*native_font_session_fonts(text.native_fonts()),record->source.face);CHECK(font);const u16 index=text.glyphs[glyph].index;
    if(font->geometry.kind==FontGeometryKind::truetype){const auto& tt=font->geometry.outline.truetype;
        CHECK(index<tt.metrics.glyph_count&&(static_cast<u32>(index)+2)*tt.loca_stride<=tt.loca.count);
        const auto offset=[&](u32 at) noexcept {const auto* p=tt.loca.data+at*tt.loca_stride;
            return tt.loca_stride==4?(static_cast<u32>(p[0])<<24)|(static_cast<u32>(p[1])<<16)|(static_cast<u32>(p[2])<<8)|p[3]:((static_cast<u32>(p[0])<<8)|p[1])*2;};
        const u32 begin=offset(index),end=offset(static_cast<u32>(index)+1);CHECK(begin<=end&&end<=tt.glyf.count);
        // An absent glyf record has no contours for instructions, variations,
        // bold or oblique simulation to transform into visible geometry.
        return begin==end;
    }
    GlyphPathCommand commands[1]{};GlyphOutline outline{};CHECK(glyph_outline_initialize(&outline,{commands,1}));
    const FontGeometryInstance instance{&font->geometry,nullptr};
    return font_geometry_outline(instance,index,nullptr,&outline)==SfntStatus::ok&&outline.count==0&&!outline.has_bounds;
}
inline bool same(Output& a,Output& b,bool mixed=false,bool blank_offsets=false) noexcept {
    const auto& x=a.text;const auto& y=b.text;
    if(x.glyph_count!=y.glyph_count){const ShapedText* outputs[]{&x,&y};
        for(u32 side=0;side<2;++side){print(side?"reference glyph IDs:":"native glyph IDs:");
            for(u32 i=0;i<minimum(outputs[side]->glyph_count,64u);++i){print(" ");number(outputs[side]->glyphs[i].index);}print("\n");}}
    if(x.glyph_count!=y.glyph_count||x.cluster_count!=y.cluster_count||(!mixed&&x.run_count!=y.run_count)){print("native glyph/cluster/run=");number(x.glyph_count);print("/");number(x.cluster_count);print("/");number(x.run_count);print(" reference=");number(y.glyph_count);print("/");number(y.cluster_count);print("/");number(y.run_count);print("\n");
        const ShapedText* values[]{&x,&y};for(u32 value=0;value<2;++value)for(u32 i=0;i<values[value]->run_count;++i){const auto& r=values[value]->runs[i];print(value?"ref run=":"native run=");number(r.first_glyph);print("/");number(r.glyph_count);print("/");number(r.text_position);print("/");number(r.text_length);print(" x/y=");real(r.baseline_x);print("/");real(r.baseline_y);print("\n");}}
    CHECK(x.glyph_count==y.glyph_count&&x.cluster_count==y.cluster_count&&(mixed||x.run_count==y.run_count));
    bool glyphs_equal=true;
    for(u32 i=0;i<x.glyph_count;++i)if(!bytes_equal(x.glyphs+i,y.glyphs+i,sizeof(TextGlyph))){const auto& a_g=x.glyphs[i];const auto& b_g=y.glyphs[i];
        if(blank_offsets&&a_g.index==b_g.index&&a_g.advance==0&&b_g.advance==0){
            auto adjusted=a_g;adjusted.advance_offset=b_g.advance_offset;adjusted.ascender_offset=b_g.ascender_offset;
            if(bytes_equal(&adjusted,&b_g,sizeof(adjusted))){u32 run=0;while(run<y.run_count&&y.runs[run].first_glyph+y.runs[run].glyph_count<=i)++run;
                CHECK(run<y.run_count);DWRITE_GLYPH_METRICS metric{};u16 space=0;const u32 scalar=32;
                CHECK(SUCCEEDED(reference_run_face(y.runs[run])->GetGlyphIndicesW(&scalar,1,&space))&&SUCCEEDED(reference_run_face(y.runs[run])->GetDesignGlyphMetrics(&b_g.index,1,&metric)));
                if(b_g.index==space&&static_cast<i64>(metric.advanceWidth)==static_cast<i64>(metric.leftSideBearing)+metric.rightSideBearing&&
                    static_cast<i64>(metric.advanceHeight)==static_cast<i64>(metric.topSideBearing)+metric.bottomSideBearing){
                    // Offsets cannot affect an empty outline with zero advance.
                    // Reject color/bitmap fonts: empty monochrome bounds alone
                    // do not prove their glyph has no visible paint.
                    IDWriteFontFace4* formats=nullptr;
                    CHECK(SUCCEEDED(reference_run_face(y.runs[run])->QueryInterface(__uuidof(IDWriteFontFace4),reinterpret_cast<void**>(&formats))));
                    const auto images=static_cast<u32>(formats->GetGlyphImageFormats());com_release(formats);
                    const u32 outlines=DWRITE_GLYPH_IMAGE_FORMATS_TRUETYPE|DWRITE_GLYPH_IMAGE_FORMATS_CFF;
                    GlyphPathCommand command[1]{};GlyphOutline outline{};CHECK(glyph_outline_initialize(&outline,{command,1}));
                    const auto result=text_outline_glyph(reference_run_face(y.runs[run]),b_g.index,&outline);
                    if(images&&(images&~outlines)==0&&result.ok()&&result.command_count==0&&!outline.has_bounds&&native_empty_glyph(x,i)){++invisible_offset_differences;continue;}
                }}}
        glyphs_equal=false;
        print("native ");CHECK(describe_run_font(x,i));print("reference ");CHECK(describe_run_font(y,i));
        const ShapedText* values[]{&x,&y};for(u32 value=0;value<2;++value)for(u32 r=0;r<values[value]->run_count;++r){const auto& run=values[value]->runs[r];
            if(i>=run.first_glyph&&i<run.first_glyph+run.glyph_count){print(value?"reference run source/count/level=":"native run source/count/level=");number(run.text_position);print("/");number(run.text_length);print("/");number(run.bidi_level);print("\n");break;}}
        print("glyph=");number(i);print(" native/ref index=");number(a_g.index);print("/");number(b_g.index);print(" advance=");real(a_g.advance);print("/");real(b_g.advance);print(" offsets=");real(a_g.advance_offset);print("/");real(b_g.advance_offset);print(",");real(a_g.ascender_offset);print("/");real(b_g.ascender_offset);print("\n");}
    if(!bytes_equal(x.clusters,y.clusters,x.cluster_count*2)){print("cluster maps native/reference=");for(u32 i=0;i<x.cluster_count;++i){number(x.clusters[i]);print("/");number(y.clusters[i]);print(" ");}print("\n");}
    CHECK(glyphs_equal&&bytes_equal(x.clusters,y.clusters,x.cluster_count*2));
    if(mixed)CHECK(same_visual_runs(a,b));
    if(!bytes_equal(&x.width,&y.width,12)||x.line_count!=y.line_count){print("metrics native=");real(x.width);print(",");real(x.width_including_trailing_whitespace);print(",");real(x.height);print(",");number(x.line_count);print(" reference=");real(y.width);print(",");real(y.width_including_trailing_whitespace);print(",");real(y.height);print(",");number(y.line_count);print("\n");}
    if(!bytes_equal(&x.width,&y.width,12)||x.decoration_count!=y.decoration_count||!bytes_equal(x.decorations,y.decorations,minimum(x.decoration_count,y.decoration_count)*sizeof(TextDecoration))){
        const auto prepared=native_text_candidate_prepared_clusters(owner);for(u32 i=0;i<prepared.count;++i){const auto& c=prepared[i];print("native cluster first/end/glyph/count/width/flags=");number(c.source_first);print("/");number(c.source_end);print("/");number(c.glyph_first);print("/");number(c.glyph_count);print("/");real(c.width);print("/");number(c.flags);print("\n");}
        for(u32 i=0;i<x.run_count;++i){print("native/reference run level=");number(x.runs[i].bidi_level);print("/");if(i<y.run_count)number(y.runs[i].bidi_level);print("\n");}
        const ShapedText* values[]{&x,&y};for(u32 value=0;value<2;++value)for(u32 i=0;i<values[value]->decoration_count;++i){const auto& d=values[value]->decorations[i];print(value?"ref decoration=":"native decoration=");number(i);print(" kind/flags=");number(static_cast<u8>(d.kind));print("/");number(d.style_flags);print(" box=");real(d.left);print(",");real(d.top);print(",");real(d.right);print(",");real(d.bottom);print("\n");}}
    if(x.hit_region_count!=y.hit_region_count){const ShapedText* values[]{&x,&y};for(u32 value=0;value<2;++value){print(value?"reference hit count=":"native hit count=");number(values[value]->hit_region_count);print("\n");
        for(u32 i=0;i<values[value]->hit_region_count;++i){const auto& h=values[value]->hit_regions[i];print("hit bounds=");real(h.left);print("/");real(h.top);print("/");real(h.right);print("/");real(h.bottom);print("\n");}}}
    CHECK(bytes_equal(&x.width,&y.width,12)&&x.line_count==y.line_count&&x.decoration_count==y.decoration_count&&x.hit_region_count==y.hit_region_count&&bytes_equal(x.decorations,y.decorations,x.decoration_count*sizeof(TextDecoration)));
    if(!bytes_equal(x.hit_regions,y.hit_regions,x.hit_region_count*sizeof(TextHitRegion))){for(u32 i=0;i<x.hit_region_count;++i){const auto& a_hit=x.hit_regions[i];const auto& b_hit=y.hit_regions[i];
        if(!bytes_equal(&a_hit,&b_hit,sizeof(a_hit))){print("hit=");number(i);print(" semantic=");number(a_hit.semantic_index);print(" native=");real(a_hit.left);print(",");real(a_hit.top);print(",");real(a_hit.right);print(",");real(a_hit.bottom);print(" reference=");real(b_hit.left);print(",");real(b_hit.top);print(",");real(b_hit.right);print(",");real(b_hit.bottom);print("\n");break;}}}
    CHECK(bytes_equal(x.hit_regions,y.hit_regions,x.hit_region_count*sizeof(TextHitRegion)));
    if(mixed)return true;
    for(u32 i=0;i<x.run_count;++i){const auto& ar=x.runs[i];const auto& br=y.runs[i];
        if(ar.bidi_level!=br.bidi_level||ar.text_position!=br.text_position||ar.text_length!=br.text_length||ar.style_flags!=br.style_flags){
            print("run metadata ");number(i);print(" native/reference level/source/length/style=");number(ar.bidi_level);print("/");number(br.bidi_level);print(" ");number(ar.text_position);print("/");number(br.text_position);print(" ");number(ar.text_length);print("/");number(br.text_length);print(" ");number(ar.style_flags);print("/");number(br.style_flags);print("\n");}
        if(ar.baseline_x!=br.baseline_x||ar.baseline_y!=br.baseline_y){print("run position ");number(i);print(" native=");real(ar.baseline_x);print(",");real(ar.baseline_y);print(" ref=");real(br.baseline_x);print(",");real(br.baseline_y);print("\n");}
        CHECK(ar.baseline_x==br.baseline_x&&ar.baseline_y==br.baseline_y&&ar.em_size==br.em_size&&ar.first_glyph==br.first_glyph&&ar.glyph_count==br.glyph_count&&ar.text_position==br.text_position&&ar.text_length==br.text_length&&ar.style_flags==br.style_flags&&ar.bidi_level==br.bidi_level&&ar.sideways==br.sideways);
        CHECK(same_native_face(x,ar,reference_run_face(br)));
    }
    return true;
}
inline bool same_visual_runs(Output& a,Output& b) noexcept {
    // Equivalent run partitions may still draw identical output. Keep exact
    // glyph/cluster/metric/decoration comparisons above; here compare each
    // glyph's absolute origin, font and rendering attributes without rounding.
    const auto& x=a.text;const auto& y=b.text;u32 ar=0,br=0,first=0;float ax=0,bx=0;
    const auto add=[](float a,float b) noexcept {return _mm_cvtss_f32(_mm_add_ss(_mm_set_ss(a),_mm_set_ss(b)));};
    for(u32 glyph=0;glyph<x.glyph_count;++glyph){
        while(ar<x.run_count&&x.runs[ar].first_glyph+x.runs[ar].glyph_count<=glyph){++ar;ax=0;}
        while(br<y.run_count&&y.runs[br].first_glyph+y.runs[br].glyph_count<=glyph){++br;bx=0;}
        CHECK(ar<x.run_count&&br<y.run_count);const auto& u=x.runs[ar];const auto& v=y.runs[br];
        CHECK(glyph>=u.first_glyph&&glyph>=v.first_glyph&&u.em_size==v.em_size&&u.style_flags==v.style_flags&&u.bidi_level==v.bidi_level&&u.sideways==v.sideways);
        if(add(u.baseline_x,ax)!=add(v.baseline_x,bx)||u.baseline_y!=v.baseline_y){print("visual glyph/run/reference-run/x/reference-x/y/reference-y=");number(glyph);print("/");number(ar);print("/");number(br);print("/");real(add(u.baseline_x,ax));print("/");real(add(v.baseline_x,bx));print("/");real(u.baseline_y);print("/");real(v.baseline_y);print("\n");
            print("native ");CHECK(describe_run_font(x,glyph));print("reference ");CHECK(describe_run_font(y,glyph));
            const ShapedText* values[]{&x,&y};for(u32 value=0;value<2;++value)for(u32 i=0;i<values[value]->run_count;++i){const auto& run=values[value]->runs[i];
                print(value?"reference source/count/glyphs/level/x/y=":"native source/count/glyphs/level/x/y=");number(run.text_position);print("/");number(run.text_length);print("/");number(run.glyph_count);print("/");number(run.bidi_level);print("/");real(run.baseline_x);print("/");real(run.baseline_y);print("\n");}}
        CHECK(add(u.baseline_x,ax)==add(v.baseline_x,bx)&&u.baseline_y==v.baseline_y);
        if(glyph==first){
            // Reuse the exact physical-file and construction-recipe oracle face
            // used by strict run comparison; every visual face still compares.
            CHECK(same_native_face(x,u,reference_run_face(v)));
            first=minimum(u.first_glyph+u.glyph_count,v.first_glyph+v.glyph_count);}
        ax=add(ax,x.glyphs[glyph].advance);bx=add(bx,y.glyphs[glyph].advance);
    }
    return true;
}
#include "semantic_ranges_cases.inc"
inline bool compare(DWriteTextSystem system,Utf8View input,bool require_native,float width=1000000,float height=1000000,TextWrapping wrapping=TextWrapping::wrap,TextOverflow overflow=TextOverflow::clip) noexcept {
    ++cases;shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);
    if(require_native){CHECK(native_text_candidate_line(system,input,width,height,&native_output.text,nullptr,wrapping,overflow));CHECK(draw_source(system,native_output.text));++native_cases;shaped_text_reset(&native_output.text);}
    const auto a=text_shape_utf8(&system,input,{},width,height,{utf16,256},&native_output.text,wrapping,overflow);
    auto reference=system;reference.native_text=nullptr;
    const auto b=text_shape_utf8(&reference,input,{},width,height,{utf16,256},&reference_output.text,wrapping,overflow);
    CHECK(a.ok()&&b.ok()&&a.utf16_units==b.utf16_units&&same(native_output,reference_output));
    const auto am=text_measure_utf8(&system,input,{},width,height,{utf16,256},wrapping);
    TextMeasureResult bm{};
    if(overflow==TextOverflow::clip){
        CHECK(reference_shape_measure(reference,input,{},width,height,{utf16,256},wrapping,input.bytes<=1,&bm));
    }else{
        // Measurement does not apply the shaping request's ellipsis trimming.
        bm=text_measure_utf8(&reference,input,{},width,height,{utf16,256},wrapping);
    }
    CHECK(am.ok()&&bm.ok()&&bytes_equal(&am.width,&bm.width,12)&&am.utf16_units==bm.utf16_units&&am.line_count==bm.line_count);
    return true;
}
inline bool inspect_style_layout(DWriteTextSystem system,Utf8View text,ConstSlice<TextStyleSpan> styles,float width,TextWrapping wrapping) noexcept {
    if(IsDebuggerPresent())__debugbreak();
    Output snapshot{};CHECK(shaped_text_initialize(&snapshot.text,{snapshot.glyphs,1024},{snapshot.runs,256},{snapshot.clusters,256},{snapshot.decorations,256}));
    struct SnapshotRelease {ShapedText& text;~SnapshotRelease() noexcept {shaped_text_reset(&text);}} snapshot_release{snapshot.text};
    auto reference=system;reference.native_text=nullptr;
    CHECK(text_shape_utf8(&reference,text,styles,width,1000000,{utf16,256},&snapshot.text,wrapping).ok());
    IDWriteTextLayout* layout=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTextLayout(utf16,snapshot.text.cluster_count,system.format,width,1000000,&layout)));
    struct Release {IDWriteTextLayout*& p;~Release() noexcept {com_release(p);}} release{layout};
    CHECK(SUCCEEDED(layout->SetWordWrapping(wrapping==TextWrapping::wrap?DWRITE_WORD_WRAPPING_WRAP:DWRITE_WORD_WRAPPING_NO_WRAP))&&SUCCEEDED(text_apply_layout_styles(system,layout,text,styles)));
    if(system.independent_letters){IDWriteTypography* typography=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTypography(&typography)));
        const DWRITE_FONT_FEATURE_TAG tags[]{DWRITE_FONT_FEATURE_TAG_STANDARD_LIGATURES,DWRITE_FONT_FEATURE_TAG_CONTEXTUAL_LIGATURES,DWRITE_FONT_FEATURE_TAG_KERNING};HRESULT configured=S_OK;
        for(auto tag:tags)if(SUCCEEDED(configured))configured=typography->AddFontFeature({tag,0});
        if(SUCCEEDED(configured))configured=layout->SetTypography(typography,{0,snapshot.text.cluster_count});com_release(typography);CHECK(SUCCEEDED(configured));}
    if(system.tracking){IDWriteTextLayout1* spacing=nullptr;CHECK(SUCCEEDED(layout->QueryInterface(__uuidof(IDWriteTextLayout1),reinterpret_cast<void**>(&spacing))));float size=system.em_size;
        auto configured=layout->GetFontSize(0,&size);if(SUCCEEDED(configured))configured=spacing->SetCharacterSpacing(0,size*system.tracking,0,{0,snapshot.text.cluster_count});com_release(spacing);CHECK(SUCCEEDED(configured));}
    CHECK(SUCCEEDED(text_apply_layout_styles(system,layout,text,styles)));
    DWRITE_TEXT_METRICS without_effect{},with_effect{};CHECK(SUCCEEDED(layout->GetMetrics(&without_effect)));
    for(u32 i=0;i<styles.count;++i){IDWriteTypography* effect=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTypography(&effect)));u32 first=0,last=0;
        CHECK(utf8_utf16_position_for_byte_offset(text,styles[i].byte_offset,&first)&&utf8_utf16_position_for_byte_offset(text,styles[i].byte_offset+styles[i].byte_count,&last));
        const HRESULT applied=layout->SetDrawingEffect(effect,{first,last-first});com_release(effect);CHECK(SUCCEEDED(applied));}
    CHECK(SUCCEEDED(layout->GetMetrics(&with_effect)));print("effect widths=");real(without_effect.width);print(",");real(with_effect.width);print("\n");
    DWRITE_CLUSTER_METRICS clusters[256];DWRITE_LINE_METRICS lines[256];u32 nc=0,nl=0;CHECK(SUCCEEDED(layout->GetClusterMetrics(clusters,256,&nc))&&SUCCEEDED(layout->GetLineMetrics(lines,256,&nl)));
    print("{\"cluster_metrics\":[");for(u32 i=0;i<nc;++i){if(i)print(",");print("[");number(clusters[i].length);print(",");real(clusters[i].width);print(",");number(clusters[i].isWhitespace);print(",");number(clusters[i].isRightToLeft);print("]");}
    print("],\"lines\":[");for(u32 i=0;i<nl;++i){if(i)print(",");print("[");number(lines[i].length);print(",");real(lines[i].height);print(",");real(lines[i].baseline);print("]");}
    print("],\"runs\":[");const auto& out=snapshot.text;
    CHECK(out.width==with_effect.width&&out.width_including_trailing_whitespace==with_effect.widthIncludingTrailingWhitespace&&out.height==with_effect.height&&out.line_count==with_effect.lineCount);
    for(u32 i=0;i<out.run_count;++i){if(i)print(",");const auto& r=out.runs[i];DWRITE_FONT_METRICS m{};reference_run_face(r)->GetMetrics(&m);
        print("[");number(r.text_position);print(",");number(r.text_length);print(",");real(r.em_size);print(",");real(r.baseline_x);print(",");real(r.baseline_y);print(",");number(m.designUnitsPerEm);print(",");number(m.ascent);print(",");number(m.descent);print(",");number(static_cast<u16>(m.lineGap));print("]");}
    print("],\"advances\":[");for(u32 i=0;i<out.glyph_count;++i){if(i)print(",");real(out.glyphs[i].advance);}
    print("],\"source_map\":[");for(u32 i=0;i<out.cluster_count;++i){if(i)print(",");number(out.clusters[i]);}print("]}\n");
    if(bidi_styles_profiles){print("{\"prefix_hits\":[");
        for(u32 end=0;end<=out.cluster_count;++end){DWRITE_HIT_TEST_METRICS metrics[256];u32 actual=0;
            CHECK(SUCCEEDED(layout->HitTestTextRange(0,end,0,0,metrics,256,&actual)));if(end)print(",");print("[");number(end);
            for(u32 i=0;i<actual;++i){const auto& m=metrics[i];print(",[");number(m.textPosition);print(",");number(m.length);print(",");real(m.left);print(",");real(m.top);print(",");real(m.width);print("]");}print("]");}
        print("]}\n");}
    return true;
}
#include "paragraph_hits_units.inc"
#include "edit_hits_units.inc"
} // namespace maicord::native_app_line
