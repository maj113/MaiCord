// The native text engine's application lines (MaiCordNativeAppLineTests) against the committed references in
// app_line_reference.hpp. The modes registered in cmake/test_registry.cmake are the suite. Manual modes, not run by it:
//   exhaustive variants  --matrix-batch (the union of the registered --matrix-profile 0-4, which
//     run --ellipsis-hit-profile, the quick --multiline-hit-profile, --style-profile and
//     --decoration-profile) and the unsampled --multiline-hit-profile and --multiline-trim-profile
//     (3-12 s each)
//   slower checks, left out of the suite's 20 s budget  --alphabetic-paragraph (2 s),
//     --arabic-paragraph-profile, --arabic-editor-profile and --cjk-editor-paint-profile (1-2 s each,
//     with --family "Segoe UI" 400)
//   needs external input  --paragraph-expansion <font> <weight> (a font whose letters expand)
//   debug toggles inside the registered corpora  MAICORD_CJK_SAMPLE, MAICORD_CJK_VARIANT and
//     MAICORD_DEVANAGARI_SAMPLE (select one sample)
// Recording the references: tools/font_reference/text_reference.py (MAICORD_TEXT_REFERENCE_RECORD). A manual mode
// or debug selection without a recorded reference reports so and exits 77.
#include "native_app_line_test.hpp"
#include "maicord/text/fonts/family.hpp"
namespace maicord::native_app_line {namespace {
bool input_validation() noexcept {
    AppSystem fresh{};
    auto* guarded=static_cast<char*>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    struct Release {AppSystem& system;char* memory;~Release() noexcept {
        shaped_text_reset(&native_output.text);text_system_release(&system);
        if(memory)VirtualFree(memory,0,MEM_RELEASE);
    }} release{fresh,guarded};
    DWORD old=0;CHECK(guarded&&VirtualProtect(guarded+4096,4096,PAGE_NOACCESS,&old));
    CHECK(text_system_initialize(static_cast<TextSystem*>(&fresh),L"Segoe UI",16).ok());
    AppSystem systems[]{fresh,owner};u32 tested=0;
    const Utf8View samples[]{utf8_literal("A"),utf8_literal("\xc2\x80"),utf8_literal("\xdf\xbf"),
        utf8_literal("\xe0\xa0\x80"),utf8_literal("\xed\x9f\xbf"),utf8_literal("\xee\x80\x80"),
        utf8_literal("\xef\xbf\xbf"),utf8_literal("\xf0\x90\x80\x80"),utf8_literal("\xf4\x8f\xbf\xbf"),
        utf8_literal("\x80"),utf8_literal("\xc0\x80"),utf8_literal("\xe0\x80\x80"),
        utf8_literal("\xed\xa0\x80"),utf8_literal("\xf0\x80\x80\x80"),utf8_literal("\xf4\x90\x80\x80"),
        utf8_literal("\xff"),utf8_literal("A\xe2\x82"),utf8_literal("A\0B")};
    for(auto& system:systems){
        const auto check=[&](Utf8View input,TextShapeError error,u32 units) noexcept {
            const auto a=text_shape_utf8(&system, {input,{},10000,10000}, &native_output.text);
            const auto d=text_measure_utf8(&system, {input,{},10000,10000});
            // Valid input reaches layout: whatever it makes of the text, validation passed.
            if(error==TextShapeError::none)CHECK(a.error!=TextShapeError::invalid_utf8&&a.error!=TextShapeError::invalid_input&&
                d.error!=TextShapeError::invalid_utf8&&d.error!=TextShapeError::invalid_input);
            else CHECK(a.error==error&&d.error==error);
            CHECK(a.utf16_units==units&&d.utf16_units==units);
            ++tested;return true;
        };
        for(auto sample:samples)for(u32 bytes=0;bytes<=sample.bytes;++bytes){
            char* input=guarded+4096-bytes;copy_bytes(input,sample.data,bytes);
            const int units=bytes?MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,input,bytes,nullptr,0):0;
            const auto error=!bytes||units?TextShapeError::none:TextShapeError::invalid_utf8;
            CHECK(check({input,bytes},error,units>0?static_cast<u32>(units):0));
        }
        CHECK(check({nullptr,1},TextShapeError::invalid_input,0));
        // The output's cluster capacity bounds the UTF-16 units a shape takes.
        const auto mixed=utf8_literal("A\xc3\xa9\xe6\xb0\xb4\xf0\x9f\x98\x80");
        for(u32 capacity=1;capacity<=6;++capacity){TextGlyph glyphs[16];TextRun runs[4];u16 clusters[6];ShapedText bounded{};
            CHECK(shaped_text_initialize(&bounded,{glyphs,16},{runs,4},{clusters,capacity}));
            const auto shaped=text_shape_utf8(&system,{mixed,{},10000,10000},&bounded);shaped_text_reset(&bounded);
            CHECK(shaped.utf16_units==5&&(capacity<5?shaped.error==TextShapeError::output_too_small:shaped.ok()));++tested;
        }
    }
    print("input_validation_cases=");number(tested);print("\n");return true;
}
// True when a native run of the output draws a simulated (emboldened or slanted) face.
bool simulated(const ShapedText& text) noexcept {
    for(u32 i=0;i<text.run_count;++i){const auto& run=text.runs[i];if(!run.native_font)continue;
        const auto* record=native_font_run_get(*native_font_session_runs(text.native_fonts()),run.native_face);if(record&&record->simulations)return true;}
    return false;
}
bool same_family(const wchar_t* family,const wchar_t* expected) noexcept {u32 i=0;while(family[i]&&family[i]==expected[i])++i;return !family[i]&&!expected[i];}
bool corpus(u32 scope,const wchar_t* family,u32 weight) noexcept {
    const u16 requested=static_cast<u16>(weight?weight:400);
    if(paragraph_expansion){
        auto system=owner;system.scope=scope;CHECK(set_format(system,family,16,requested));
        char text[123];for(u32 i=0;i<123;++i)text[i]='A';
        CHECK(compare(system,{text,40},true));CHECK(native_output.text.glyph_count==160);
        CHECK(compare(system,{text,40},true,39,1));CHECK(native_output.text.glyph_count==160);
        text[40]='\t';text[81]='\r';text[82]='\n';
        CHECK(compare(system,{text,123},true,39,1));CHECK(native_output.text.glyph_count==480);
        CHECK(compare(system,{text,123},true,1000,1000));CHECK(native_output.text.glyph_count==480);
        return true;
    }
    const bool segoe=!scope&&!bundled_mode&&same_family(family,L"Segoe UI");
    const Utf8View texts[]{utf8_literal("a"),utf8_literal("AV To"),utf8_literal("ffi ffl fi fl"),utf8_literal("  a  "),utf8_literal("   "),utf8_literal("office AV To  "),utf8_literal("Hello, world!"),utf8_literal("AV To office AV To office AV To office AV To office AV To office AV To office AV To office AV To office AV To office AV To office AV To office AV To office AV To office ")};
    bool cjk_ok=true;const float sizes[]{14.f,17.5f,1000.f};for(float size:sizes){
        auto system=owner;system.scope=scope;CHECK(set_format(system,family,size,requested));
        if(simulated_profiles){system.em_size=size;CHECK(native_simulated_profile_corpus(system));continue;}
        if(bidi_styles_profiles){system.em_size=size;system.fixed_weight=scope!=0;CHECK(native_bidi_styles_corpus(system));continue;}
        if(mixed_bidi_profiles){system.em_size=size;system.fixed_weight=scope!=0;CHECK(native_mixed_bidi_corpus(system));continue;}
        if(hebrew_profiles){system.em_size=size;system.fixed_weight=scope!=0;CHECK(native_hebrew_corpus(system));continue;}
        if(system_fallback_profiles){system.em_size=size;CHECK(native_system_fallback_corpus(system,scope!=0,segoe));continue;}
        if(alphabetic_profiles){system.em_size=size;CHECK(alphabetic_corpus(system,segoe));continue;}
        if(multiline_trim_profiles){system.em_size=size;system.fixed_weight=scope!=0;if(!native_multiline_trim_corpus(system))cjk_ok=false;continue;}
        if(height_overflow_profiles){system.em_size=size;system.fixed_weight=scope!=0;CHECK(native_height_overflow_corpus(system));continue;}
        if(ellipsis_sign_profiles){system.em_size=size;system.fixed_weight=scope!=0;CHECK(native_ellipsis_sign_corpus(system));continue;}
        if(ellipsis_cut_profiles||ellipsis_line_profiles){system.em_size=size;system.fixed_weight=scope!=0;if(!native_ellipsis_cut_corpus(system))cjk_ok=false;continue;}
        if(ellipsis_style_profiles||ellipsis_hit_profiles){system.em_size=size;system.fixed_weight=scope!=0;if(!native_ellipsis_style_corpus(system))cjk_ok=false;continue;}
        if(common_punctuation_profiles){system.em_size=size;system.fixed_weight=scope!=0;CHECK(native_common_punctuation_corpus(system));continue;}
        if(hangul_profiles){system.em_size=size;system.fixed_weight=scope!=0;if(!native_hangul_corpus(system))return false;continue;}
        if(thai_profiles){system.em_size=size;system.fixed_weight=scope!=0;if(!native_thai_corpus(system))return false;continue;}
        if(standard_script_profiles){system.em_size=size;system.fixed_weight=scope!=0;if(!native_standard_script_corpus(system))return false;continue;}
        if(devanagari_profiles){system.em_size=size;system.fixed_weight=scope!=0;if(!native_devanagari_corpus(system))return false;continue;}
        if(cjk_editor_paint_profiles){system.em_size=size;system.fixed_weight=scope!=0;if(!native_cjk_editor_paint_corpus(system))cjk_ok=false;continue;}
        if(cjk_paragraph_profiles||cjk_editor_profiles){system.em_size=size;system.fixed_weight=scope!=0;if(!native_cjk_paragraph_corpus(system,cjk_editor_profiles))cjk_ok=false;continue;}
        if(arabic_paragraph_profiles||arabic_editor_profiles){system.em_size=size;system.fixed_weight=scope!=0;CHECK(native_arabic_paragraph_corpus(system,arabic_editor_profiles));continue;}
        if(tracking_profiles){CHECK(native_tracking_corpus(system));continue;}
        if(style_continuity_profiles){CHECK(style_continuity_corpus(system));continue;}
        if(style_cluster_profiles){system.em_size=size;CHECK(style_cluster_corpus(system,segoe&&weight==400));continue;}
        if(styled_hit_profiles){CHECK(styled_hit_corpus(system,segoe&&weight==400));continue;}
        if(directional_editor_profiles||directional_editor_cuts_profiles){system.em_size=size;system.fixed_weight=scope!=0;CHECK(directional_editor_hit_corpus(system,directional_editor_cuts_profiles));continue;}
        if(directional_large_profiles){system.em_size=size;system.fixed_weight=scope!=0;if(size==1000)CHECK(directional_semantic_large_origin(system));continue;}
        if(directional_semantic_profiles){system.em_size=size;system.fixed_weight=scope!=0;CHECK(directional_semantic_corpus(system));continue;}
        if(semantic_profiles){CHECK(semantic_corpus(system,segoe&&weight==400));continue;}
        if(style_profiles||decoration_profiles){
            const Utf8View style_texts[]{utf8_literal("AV office caf\xc3\xa9 fi AV"),utf8_literal("AV office e\xcc\x81 fi AV\n\xc3\xa9 AV"),utf8_literal("AV\toffice\r\n\xc3\xa9 AV\n"),utf8_literal("AV office\n\n"),utf8_literal("AV office\t\t   "),utf8_literal("AV office\r\n\t\t")};
            const u32 font_flags[]{text_style_bold,text_style_italic,text_style_bold|text_style_italic,text_style_heading_1,text_style_heading_2,text_style_heading_3,text_style_subtext,text_style_link,text_style_syntax_keyword,text_style_monospace,text_style_heading_1|text_style_bold,text_style_subtext|text_style_italic};
            const u32 decoration_flags[]{text_style_underline,text_style_strikethrough,text_style_underline|text_style_strikethrough,text_style_underline|text_style_heading_1,text_style_strikethrough|text_style_italic,text_style_monospace,text_style_monospace|text_style_code_block,text_style_spoiler,text_style_mention,text_style_quote,text_style_list|text_style_list_bullet,text_style_list|text_style_list_bullet|text_style_list_depth_1};
            const u32* flags=decoration_profiles?decoration_flags:font_flags;
            const float widths[]{8.f,37.f,95.f,1000000.f};
            const bool require_native=decoration_profiles&&segoe&&weight==400;
            for(u32 mode=0;mode<4;++mode){system.independent_letters=(mode&1)!=0;system.fixed_weight=(mode&2)!=0;for(u32 sample=0;sample<(decoration_profiles?6u:4u);++sample)for(u32 flags_index=0;flags_index<12;++flags_index)for(u32 placement=0;placement<(decoration_profiles?7u:5u);++placement){
                const auto text=style_texts[sample];TextStyleSpan spans[3]{};u32 count=1;
                if(placement==0)spans[0]={3,6,flags[flags_index]};else if(placement==1)spans[0]={0,text.bytes,flags[flags_index]};else if(placement==2){spans[0]={0,1,flags[flags_index]};spans[1]={1,1,text_style_link};count=2;}else if(placement==3)spans[0]={9,text.bytes-9,flags[flags_index]};else if(placement==4)spans[0]={2,1,flags[flags_index]};
                else if(placement==5){spans[0]={0,3,flags[flags_index]};spans[1]={3,6,flags[flags_index]|text_style_heading_3};spans[2]={9,text.bytes-9,flags[flags_index]|text_style_bold};count=3;}
                else{spans[0]={0,3,flags[flags_index]};spans[1]={3,text.bytes-3,flags[flags_index]};count=2;}
                for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap){++matrix_seen;++cases;const auto wrap=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;
                    shaped_text_reset(&native_output.text);
                    const bool native=native_text_candidate_styled(system,{text,{spans,count},width,1000000,wrap},&native_output.text,nullptr);CHECK(!require_native||native);observe_value(static_cast<u32>(native));
                    if(native){++native_cases;CHECK(draw_source(system,native_output.text));}shaped_text_reset(&native_output.text);
                    const auto shaped=text_shape_utf8(&system,{text,{spans,count},width,1000000,wrap},&native_output.text);
                    const auto measured=text_measure_utf8(&system,{text,{spans,count},width,1000000,wrap});
                    CHECK(shaped.ok()&&measured.ok()&&measured.utf16_units==shaped.utf16_units&&same_metrics(measured,native_output.text));
                    observe(native_output.text,text,system);
                }
            }}continue;
        }
        if(latin_paragraph_profiles||latin_paragraph_hits){const float widths[]{8.f,37.f,95.f,1000000.f};
            for(u32 mode=0;mode<2;++mode){system.independent_letters=mode!=0;
                const ConstSlice<Utf8View> sets[]{{latin_samples,sizeof(latin_samples)/sizeof(latin_samples[0])},{latin_paragraph_samples,sizeof(latin_paragraph_samples)/sizeof(latin_paragraph_samples[0])}};
                for(u32 set=0;set<2;++set)for(u32 sample=0;sample<sets[set].count;++sample){const auto text=sets[set][sample];
                    shaped_text_reset(&native_output.text);const bool native=native_text_candidate_line(system,{text,{},1000000,1000000},&native_output.text,nullptr);
                    shaped_text_reset(&native_output.text);observe_value(static_cast<u32>(native));
                    for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap){const auto wrap=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;const float actual=width==1000000?width:width*(size/16);
                        if(!(latin_paragraph_hits?(!native||compare_hits(system,text,actual,wrap)):compare(system,text,native,actual,1000000,wrap))){
                            print("latin paragraph set/sample/em/width/independent/nowrap=");number(set);print(",");number(sample);print(",");real(size);print(",");real(actual);print(",");number(mode);print(",");number(nowrap);print("\n");return false;}
                    }
                }
            }continue;
        }
        if(latin_profiles){for(u32 mode=0;mode<2;++mode){system.independent_letters=mode!=0;for(const auto text:latin_samples){
            shaped_text_reset(&native_output.text);const bool native=native_text_candidate_line(system,{text,{},1000000,1000000},&native_output.text,nullptr);
            shaped_text_reset(&native_output.text);observe_value(static_cast<u32>(native));CHECK(compare(system,text,native));}}continue;}
        if(hit_profiles){const float widths[]{8.f,37.f,95.f,1000000.f};for(u32 mode=0;mode<2;++mode){system.independent_letters=mode!=0;for(const auto text:paragraph_samples)for(float width:widths){
            const float actual=width==1000000?width:width*(size/16);CHECK(compare_hits(system,text,actual,TextWrapping::wrap));CHECK(compare_hits(system,text,actual,TextWrapping::no_wrap));}}continue;}
        if(paragraph_profiles){const float widths[]{8.f,37.f,95.f,1000000.f};for(u32 mode=0;mode<2;++mode){system.independent_letters=mode!=0;for(const auto text:paragraph_samples)for(float width:widths){
            const float actual=width==1000000?width:width*(size/16);CHECK(compare(system,text,true,actual,1));}}continue;}
        // A simulated face (bold or oblique drawn from a regular one) is declined by the line path.
        for(u32 mode=0;mode<2;++mode){system.independent_letters=mode!=0;for(const auto text:texts){
            shaped_text_reset(&second_output.text);CHECK(text_shape_utf8(&system,{text,{},1000000,1000000},&second_output.text).ok());
            const bool simulation=simulated(second_output.text);shaped_text_reset(&second_output.text);
            if(simulation){CHECK(!native_text_candidate_line(system,{text,{},1000000,1000000},&native_output.text,nullptr));++simulated_cases;}
            CHECK(compare(system,text,!simulation));
        }}
    }
    return cjk_ok;
}
// The WWS family the native catalog files face zero of `path` under (native_font_family).
bool private_family(const wchar_t* path,wchar_t (&name)[256]) noexcept {
    NativeFontHandle face{};CHECK(native_font_session_face(owner.native_fonts,path,0,&face).ok());
    auto* store=native_font_session_fonts(owner.native_fonts);
    struct Release {NativeFontStore* store;NativeFontHandle face;~Release() noexcept {native_font_release(store,face);}} release{store,face};
    const auto* font=native_font_get(*store,face);CHECK(font);
    u16 units[256];NativeFontFamily family{};CHECK(native_font_family(font->geometry.face,{units,255},&family)==SfntStatus::ok&&family.units<256);
    for(u32 i=0;i<family.units;++i)name[i]=static_cast<wchar_t>(units[i]);name[family.units]=0;return true;
}
// --bundled-fonts <directory>: the system families the corpora name become the repository's fonts (set_format); the
// owner draws body text with MuseoModerno and code (monospace spans) with BioRhyme instead of Segoe UI and Consolas,
// so layouts that need no other script are the same on every machine. Fonts without contextual alternates keep each
// drawn run comparable with HarfBuzz. Runs before any binding, as the code format requires.
bool use_bundled_fonts(const wchar_t* directory) noexcept {
    const wchar_t* files[]{L"\\MuseoModerno.ttf",L"\\ZillaSlab.ttf",L"\\BioRhyme.ttf"};
    for(u32 i=0;i<3;++i){wchar_t path[1100];u32 n=0;while(directory[n]){CHECK(n<1024);path[n]=directory[n];++n;}
        for(u32 j=0;files[i][j];++j)path[n++]=files[i][j];path[n]=0;
        CHECK(native_text_register_font_path(owner.native_text,path,&bundled_families[i].scope)&&bundled_families[i].scope);
        CHECK(private_family(path,bundled_families[i].name));}
    bundled_mode=true;auto code=owner;CHECK(set_format(owner,L"Segoe UI",16)&&set_format(code,L"Consolas",16));owner.scope=0;
    CHECK(native_text_context_set_code_format(owner.native_text,code.native_format));return true;
}
bool custom(const wchar_t* path,u32 weight) noexcept {
    // Registration is by path: repeating it returns the same scope, and an empty or missing path is refused.
    wchar_t copied[32768];u32 units=0;while(path[units]){CHECK(units<32767);copied[units]=path[units];++units;}copied[units]=0;
    u32 scope=0,again=0,ignored=0;CHECK(native_text_register_font_path(owner.native_text,copied,&scope)&&scope);copied[0]=L'!';
    CHECK(native_text_register_font_path(owner.native_text,path,&again)&&again==scope);
    CHECK(!native_text_register_font_path(owner.native_text,L"",&ignored));
    wchar_t name[256];CHECK(private_family(path,name));
    auto system=owner;system.scope=scope;CHECK(set_format(system,name,16,static_cast<u16>(weight?weight:400)));
    shaped_text_reset(&second_output.text);CHECK(text_shape_utf8(&system,{utf8_literal("AV"),{},1000,1000},&second_output.text).ok());
    const bool simulation=simulated(second_output.text);shaped_text_reset(&second_output.text);
    if(!simulation){CHECK(native_text_candidate_line(system,{utf8_literal("AV"),{},1000,1000},&native_output.text,nullptr));observe(native_output.text,utf8_literal("AV"),system);}
    shaped_text_reset(&native_output.text);
    return corpus(scope,name,weight);
}
bool matrix_batch(const wchar_t* arguments, bool single_profile = false) noexcept {
    // The batch is the exact union of the former forty-four modulo shards.
    // Retain one font/session owner, but retire each profile's output leases.
    static wchar_t private_path[32768];
    while (*arguments == L' ' || *arguments == L'\t')
        ++arguments;
    u32 selected_profile = 0;
    if (single_profile) {
        CHECK(*arguments >= L'0' && *arguments <= L'4');
        selected_profile = static_cast<u32>(*arguments++ - L'0');
        CHECK(*arguments == L' ' || *arguments == L'\t');
        while (*arguments == L' ' || *arguments == L'\t')
            ++arguments;
    }
    const bool quoted = *arguments == L'"';
    if (quoted)
        ++arguments;
    u32 units = 0;
    while (*arguments && !(quoted ? *arguments == L'"' : *arguments == L' ' || *arguments == L'\t')) {
        CHECK(units + 1 < 32768);
        private_path[units++] = *arguments++;
    }
    CHECK(units);
    if (quoted) {
        CHECK(*arguments == L'"');
        ++arguments;
    }
    while (*arguments == L' ' || *arguments == L'\t')
        ++arguments;
    CHECK(!*arguments);
    private_path[units] = 0;
    constexpr const char* profiles[]{"ellipsis_hits", "multiline_hits", "multiline_private_hits", "styled",
                                     "decorations"};
    constexpr u32 expected[]{31320, 18600, 18600, 23040, 48384};
    const u32 first_profile = single_profile ? selected_profile : 0;
    const u32 profile_limit = single_profile ? selected_profile + 1 : 5;
    for (u32 profile = first_profile; profile < profile_limit; ++profile) {
        shaped_text_reset(&native_output.text);
        ellipsis_style_profiles = ellipsis_hit_profiles = false;
        multiline_trim_profiles = multiline_hit_profiles = multiline_quick = false;
        style_profiles = decoration_profiles = false;
        matrix_seen = 0;
        const u32 first_case = cases, first_native = native_cases, first_checks = checks;
        if (profile == 0)
            ellipsis_hit_profiles = true;
        else if (profile == 1 || profile == 2)
            multiline_trim_profiles = multiline_hit_profiles = multiline_quick = true;
        else if (profile == 3)
            style_profiles = true;
        else
            decoration_profiles = true;
        const bool ok = profile == 2 ? custom(private_path, 0) : corpus(0, L"Segoe UI", 400);
        print("matrix_profile=");
        print(profiles[profile]);
        print(" cases=");
        number(cases - first_case);
        print(" seen=");
        number(matrix_seen);
        print(" native_cases=");
        number(native_cases - first_native);
        print(" checks=");
        number(checks - first_checks);
        print(ok ? " PASS\n" : " FAIL\n");
        CHECK(ok);
        CHECK(matrix_seen == expected[profile] && cases - first_case == expected[profile]);
    }
    shaped_text_reset(&native_output.text);
    ellipsis_hit_profiles = multiline_trim_profiles = multiline_hit_profiles = multiline_quick = false;
    style_profiles = decoration_profiles = false;
    if (single_profile) {
        print("PASS matrix_profile profiles=1 cases=");
        number(expected[selected_profile]);
        print(" sizes=14,17.5,1000\n");
    } else
        print("PASS matrix_batch profiles=5 cases=139944 sizes=14,17.5,1000\n");
    return true;
}
bool retained_run_lifetime() noexcept {
    // A run handle keeps its session's face alive after the text system is released; the outline instance prepared
    // from it outlives the run, and the session's last release needs every handle back.
    TextSystem transient{};NativeFontSession* observer=nullptr;NativeFontInstanceHandle outlined{};Arena scratch{};
    struct Release {TextSystem& t;NativeFontSession*& s;NativeFontInstanceHandle& a;Arena& scratch;
        ~Release() noexcept {shaped_text_reset(&native_output.text);text_system_release(&t);if(s){if(a.slot)native_font_instance_release(native_font_session_instances(s),a);native_font_session_release(&s);}arena_release(&scratch);}}
        release{transient,observer,outlined,scratch};
    shaped_text_reset(&native_output.text);CHECK(text_system_initialize(&transient,L"Segoe UI",16).ok());
    CHECK(native_text_candidate_line(transient,{utf8_literal("AV  "),{},1000,1000},&native_output.text,nullptr));
    CHECK(native_output.text.run_count==2&&native_output.text.runs[0].native_font&&native_output.text.runs[1].native_font);
    const auto handle=native_output.text.runs[0].native_face;observer=transient.native_fonts;CHECK(native_font_session_retain(observer));
    CHECK(native_output.text.native_fonts()==observer);
    text_system_release(&transient);
    CHECK(native_font_session_stats(observer).references==2&&native_font_run_get(*native_font_session_runs(observer),handle));
    CHECK(arena_initialize(&scratch,4*1024*1024,0,4096));
    CHECK(native_font_run_prepare(native_font_session_runs(observer),handle,&scratch,&outlined).ok());
    shaped_text_reset(&native_output.text);
    CHECK(!native_output.text.native_fonts()&&!native_font_run_get(*native_font_session_runs(observer),handle));
    CHECK(native_font_session_stats(observer).references==1&&native_font_session_stats(observer).instances==1&&native_font_instance_get(*native_font_session_instances(observer),outlined));
    CHECK(!native_font_session_release(&observer)&&observer);
    CHECK(native_font_instance_release(native_font_session_instances(observer),outlined));outlined={};
    CHECK(!native_font_session_stats(observer).live.files&&!native_font_session_stats(observer).instances);
    CHECK(native_font_session_release(&observer)&&!observer);return true;
}
bool paragraphs() noexcept {
    const Utf8View texts[]{utf8_literal("AV AV"),utf8_literal("AV"),utf8_literal("fi fi"),utf8_literal("ffi"),utf8_literal("a\r\nb"),utf8_literal("a\nb"),utf8_literal("a\n"),utf8_literal("\n"),utf8_literal("a  b"),utf8_literal(" a b"),utf8_literal("a\tb"),utf8_literal(" \t "),utf8_literal("a-b/c"),utf8_literal("a...b"),utf8_literal("(a b) c"),utf8_literal("   "),utf8_literal("a     ")};
    const wchar_t* families[]{L"Segoe UI",L"Georgia",L"Consolas"};const float widths[]{.01f,5.f,10.f,15.f,20.f,30.f,45.f,90.f,200.f},heights[]{5.f,1000.f};
    for(const auto* family:families){auto system=owner;CHECK(set_format(system,family,16));
        for(const auto text:texts)for(float width:widths)for(float height:heights)CHECK(compare(system,text,true,width,height));
    }return true;
}
bool paragraph_fuzz() noexcept {
    constexpr char alphabet[]="aaab01   '-/\\.,:;()[]{}!?%$+\t\r\n";u32 state=0x39846113u;
    const auto random=[&]() noexcept {state^=state<<13;state^=state>>17;state^=state<<5;return state;};
    const float widths[]{13.f,29.f,61.f};
    for(u32 sample=0;sample<4000;++sample){char bytes[80];const u32 count=1+random()%79;for(u32 i=0;i<count;++i)bytes[i]=alphabet[random()%(sizeof(alphabet)-1)];
        for(float width:widths)if(!compare(owner,{bytes,count},true,width,1)){
            print("fuzz sample=");number(sample);print(" width=");real(width);print(" bytes=[");for(u32 i=0;i<count;++i){if(i)print(",");number(static_cast<u8>(bytes[i]));}print("]\n");return false;}
    }return true;
}
#include "styled_paragraph_checks.inc"
#include "native_decoration_options.inc"
bool latin_paragraph_fuzz() noexcept {
    const u32 alphabet[]{'a','e','f','i','A','V','0','1',' ',' ','\t','\r','\n','(',')','"','-','/',',','.','$','%',0xe9,0x10d,0x153,0x301,0x327,0x34f,0x200c,0x200d,0xa0,0x2002,0x202f,0x2010,0x2011,0x2014,0x2028,0x2029};
    u32 state=0x731e48adu;const auto random=[&]() noexcept {state^=state<<13;state^=state>>17;state^=state<<5;return state;};
    const float widths[]{13.f,37.f,95.f};
    for(u32 sample=0;sample<2000;++sample){char bytes[256];u32 count=0;const u32 length=1+random()%63;
        count+=utf8_encode(0xe9,bytes);for(u32 i=0;i<length;++i)count+=utf8_encode(alphabet[random()%(sizeof(alphabet)/sizeof(alphabet[0]))],bytes+count);
        const Utf8View text{bytes,count};shaped_text_reset(&native_output.text);const bool native=native_text_candidate_line(owner,{text,{},1000000,1000000},&native_output.text,nullptr);shaped_text_reset(&native_output.text);
        for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap)if(!compare(owner,text,native,width,1000000,nowrap?TextWrapping::no_wrap:TextWrapping::wrap)){
            print("latin fuzz sample=");number(sample);print(" width=");real(width);print(" nowrap=");number(nowrap);print(" bytes=[");for(u32 i=0;i<count;++i){if(i)print(",");number(static_cast<u8>(bytes[i]));}print("]\n");return false;}
    }return true;
}
bool paragraph_options() noexcept {
    const float stops[]{3.5f,64.f,91.25f};const float widths[]{5.f,39.f,1000.f};const float heights[]{1.f,1000.f};
    for(float stop:stops){auto system=owner;CHECK(set_format(system,L"Segoe UI",16,400,0,stop));
        for(const auto text:paragraph_samples)for(float width:widths)for(float height:heights)for(u32 mode=0;mode<2;++mode){
            const auto wrapping=mode?TextWrapping::no_wrap:TextWrapping::wrap;
            CHECK(compare(system,text,true,width,height,wrapping,TextOverflow::clip));
            CHECK(compare(system,text,false,width,height,wrapping,TextOverflow::ellipsis));
        }
    }
    // A cached paragraph must reflow after a format change, and a format that does not wrap declines the line path's
    // wrapping request.
    auto system=owner;CHECK(compare(system,utf8_literal("AV\tAV\nAV AV"),true,30,1000));
    CHECK(set_format(system,L"Segoe UI",16,400,0,0,false));
    CHECK(compare(system,utf8_literal("AV\tAV\nAV AV"),false,30,1000));
    CHECK(compare(system,utf8_literal("AV\tAV\nAV AV"),true,30,1000,TextWrapping::no_wrap));
    return true;
}
#include "alphabetic_paragraph_cases.inc"
#include "devanagari_memory_check.inc"
#include "thai_wordbreak_probe.inc"
#include "thai_context_cases.inc"
#include "script_storage_cases.inc"
#include "font_range_replay_cases.inc"
#include "long_text_cases.inc"
#include "tsf_point_geometry.inc"
#include "native_historic_cases.inc"
#include "native_font_selection_cases.inc"
#include "native_fresh_engine_cases.inc"
#include "native_font_context_cases.inc"
#include "native_request_recovery.inc"
// The command line is matched option by option, left to right, against what is left of it.
u32 option_length(const wchar_t* p,const wchar_t* option) noexcept {u32 n=0;while(option[n]&&p[n]==option[n])++n;return option[n]?0:n;}
bool is_option(const wchar_t* p,const wchar_t* option) noexcept {const u32 n=option_length(p,option);return n&&!p[n];}
const wchar_t* after_option(const wchar_t* p,const wchar_t* option) noexcept {const u32 n=option_length(p,option);return n&&(p[n]==L' '||p[n]==L'\t')?p+n:nullptr;}
// option followed by a blank (or by the end of the text with end_ok): consumes it and the blanks after it.
bool take(const wchar_t*& p,const wchar_t* option,bool end_ok=false) noexcept {
    const u32 n=option_length(p,option);if(!n||!(p[n]==L' '||p[n]==L'\t'||(end_ok&&!p[n])))return false;
    p+=n;while(*p==L' '||*p==L'\t')++p;return true;
}
template<class... Flags> void profile(const wchar_t*& p,const wchar_t* option,Flags&... flags) noexcept {if(take(p,option))((flags=true),...);}
bool run() noexcept {
    const wchar_t* p=GetCommandLineW();bool quote=false;while(*p){if(*p=='"')quote=!quote;else if(!quote&&(*p==' '||*p=='\t'))break;++p;}while(*p==' '||*p=='\t')++p;
    if(is_option(p,L"--request-recovery"))return native_request_recovery();
    if(is_option(p,L"--font-selection-check"))return native_font_selection_cases();
    if(is_option(p,L"--font-context-layouts"))return native_font_context_layouts();
    if(is_option(p,L"--script-storage-check"))return native_script_storage_cases();
    wchar_t bundled[1024]{};
    if(take(p,L"--bundled-fonts")){u32 n=0;const bool quoted=*p=='"';if(quoted)++p;while(*p&&!(quoted?*p=='"':*p==' '||*p=='\t')){CHECK(n+1<1024);bundled[n++]=*p++;}
        if(quoted&&*p=='"')++p;while(*p==' '||*p=='\t')++p;}
    CHECK(text_system_initialize(static_cast<TextSystem*>(&owner),L"Segoe UI",16).ok());reference_session(owner.native_fonts);
    if(bundled[0])CHECK(use_bundled_fonts(bundled));
    Output* outputs[]{&native_output,&second_output};for(auto* out:outputs){CHECK(shaped_text_initialize(&out->text,{out->glyphs,1024},{out->runs,256},{out->clusters,256},{out->decorations,256},{out->hits,4096}));}
    if(const wchar_t* rest=after_option(p,L"--matrix-batch"))return matrix_batch(rest);
    if(const wchar_t* rest=after_option(p,L"--matrix-profile"))return matrix_batch(rest,true);
    if(is_option(p,L"--alphabetic-paragraph"))return native_alphabetic_paragraph_corpus();
    if(is_option(p,L"--han-paragraph"))return native_han_paragraph_corpus();
    if(is_option(p,L"--thai-context-check"))return native_thai_context_cases();
    if(is_option(p,L"--devanagari-memory-check"))return native_devanagari_memory_check();
    if(const u32 n=option_length(p,L"--retained-editor-check");n&&p[n]==' '&&p[n+1]>=L'0'&&p[n+1]<=L'5'&&!p[n+2])return retained_editor_capacity(p[n+1]-L'0');
    if(is_option(p,L"--long-editor-check"))return native_long_text_cases(true);
    if(is_option(p,L"--long-text-check"))return native_long_text_cases();
    if(is_option(p,L"--font-replay-check"))return native_font_range_replay_cases();
    profile(p,L"--hangul-profile",hangul_profiles);
    profile(p,L"--thai-shape-profile",thai_profiles,thai_shape_only);
    profile(p,L"--thai-profile",thai_profiles);
    profile(p,L"--extended-standard-profile",standard_script_profiles,extended_standard_profiles);
    profile(p,L"--standard-script-profile",standard_script_profiles);
    profile(p,L"--tamil-direction-shape-profile",devanagari_profiles,tamil_profiles,devanagari_directional,devanagari_tracking,tamil_shape_only);
    profile(p,L"--tamil-direction-profile",devanagari_profiles,tamil_profiles,devanagari_directional,devanagari_tracking);
    profile(p,L"--telugu-profile",devanagari_profiles,telugu_profiles);
    profile(p,L"--telugu-tracking-profile",devanagari_profiles,telugu_profiles,devanagari_tracking);
    profile(p,L"--kannada-profile",devanagari_profiles,kannada_profiles);
    profile(p,L"--kannada-tracking-profile",devanagari_profiles,kannada_profiles,devanagari_tracking);
    profile(p,L"--khmer-profile",devanagari_profiles,khmer_profiles);
    profile(p,L"--khmer-tracking-profile",devanagari_profiles,khmer_profiles,devanagari_tracking);
    profile(p,L"--myanmar-profile",devanagari_profiles,myanmar_profiles);
    profile(p,L"--myanmar-tracking-profile",devanagari_profiles,myanmar_profiles,devanagari_tracking);
    profile(p,L"--tibetan-profile",devanagari_profiles,tibetan_profiles);
    profile(p,L"--tibetan-tracking-profile",devanagari_profiles,tibetan_profiles,devanagari_tracking);
    profile(p,L"--sinhala-profile",devanagari_profiles,sinhala_profiles);
    profile(p,L"--sinhala-tracking-profile",devanagari_profiles,sinhala_profiles,devanagari_tracking);
    profile(p,L"--malayalam-profile",devanagari_profiles,malayalam_profiles);
    profile(p,L"--malayalam-tracking-profile",devanagari_profiles,malayalam_profiles,devanagari_tracking);
    profile(p,L"--odia-profile",devanagari_profiles,odia_profiles);
    profile(p,L"--odia-tracking-profile",devanagari_profiles,odia_profiles,devanagari_tracking);
    profile(p,L"--gurmukhi-profile",devanagari_profiles,gurmukhi_profiles);
    profile(p,L"--gurmukhi-tracking-profile",devanagari_profiles,gurmukhi_profiles,devanagari_tracking);
    profile(p,L"--gujarati-profile",devanagari_profiles,gujarati_profiles);
    profile(p,L"--gujarati-tracking-profile",devanagari_profiles,gujarati_profiles,devanagari_tracking);
    profile(p,L"--bengali-profile",devanagari_profiles,bengali_profiles);
    profile(p,L"--bengali-tracking-profile",devanagari_profiles,bengali_profiles,devanagari_tracking);
    profile(p,L"--tamil-tracking-shape-profile",devanagari_profiles,tamil_profiles,devanagari_tracking,tamil_shape_only);
    profile(p,L"--tamil-tracking-profile",devanagari_profiles,tamil_profiles,devanagari_tracking);
    profile(p,L"--tamil-profile",devanagari_profiles,tamil_profiles);
    profile(p,L"--devanagari-profile",devanagari_profiles);
    profile(p,L"--devanagari-tracking-profile",devanagari_profiles,devanagari_tracking);
    profile(p,L"--devanagari-direction-profile",devanagari_profiles,devanagari_directional);
    profile(p,L"--direction-tracking-profile",devanagari_profiles,devanagari_directional,devanagari_tracking);
    profile(p,L"--arabic-context-profile",arabic_context_profiles,devanagari_profiles,devanagari_directional,devanagari_tracking);
    if(take(p,L"--quick")){multiline_quick=true;
        CHECK(option_length(p,L"--multiline-")||(option_length(p,L"--bidi-emoji-profile")&&(!p[20]||p[20]==' '))||(option_length(p,L"--mixed-emoji")&&(!p[13]||p[13]==' ')));
        print("quick sampled coverage; exhaustive combinations remain available without --quick\n");}
    if(is_option(p,L"--bidi-range-check"))return native_bidi_style_range_probe(true);
    if(is_option(p,L"--mixed-bidi-gates"))return native_mixed_bidi_gates();
    if(is_option(p,L"--simulated-styles"))return native_simulated_style_corpus();
    profile(p,L"--simulated-profile",simulated_profiles);
    profile(p,L"--symbols-profile",bidi_styles_profiles,symbol_profiles);
    profile(p,L"--bidi-emoji-profile",bidi_styles_profiles,bidi_emoji_profiles);
    profile(p,L"--bidi-styles-profile",bidi_styles_profiles);
    profile(p,L"--mixed-bidi-wrap-profile",mixed_bidi_profiles,mixed_bidi_wrap);
    profile(p,L"--mixed-bidi-profile",mixed_bidi_profiles);
    profile(p,L"--hebrew-wrap-profile",hebrew_profiles,hebrew_wrap_profiles);
    profile(p,L"--hebrew-profile",hebrew_profiles);
    profile(p,L"--system-fallback-profile",system_fallback_profiles);
    if(is_option(p,L"--fallback-bindings"))return native_fallback_binding_corpus();
    profile(p,L"--alphabetic-profile",alphabetic_profiles);
    if(take(p,L"--mixed-emoji",true))return *p?native_emoji_dispatch_corpus(p,true):native_mixed_emoji_corpus();
    if(take(p,L"--emoji-dispatch",true))return native_emoji_dispatch_corpus(p);
    profile(p,L"--tracking-profile",tracking_profiles);
    if(is_option(p,L"--native-formats"))return native_format_corpus();
    if(is_option(p,L"--style-clusters"))return inspect_style_clusters();
    profile(p,L"--style-cluster-profile",style_cluster_profiles);
    profile(p,L"--continuity-profile",style_continuity_profiles);
    if(is_option(p,L"--paragraph-hits"))return paragraph_hits();
    if(is_option(p,L"--tsf-points"))return tsf_point_geometry();
    if(is_option(p,L"--historic"))return historic_cases();
    if(is_option(p,L"--edit-hits"))return edit_hits();
    if(is_option(p,L"--directional-edit-cuts"))return directional_edit_cuts();
    if(is_option(p,L"--directional-edit"))return directional_edit();
    if(is_option(p,L"--styled-edit"))return styled_edit();
    if(is_option(p,L"--paragraphs"))return paragraphs();
    if(is_option(p,L"--paragraph-fuzz")){reference_shaping(false);return paragraph_fuzz();}
    if(is_option(p,L"--paragraph-options"))return paragraph_options();
    profile(p,L"--hit-profile",hit_profiles);
    profile(p,L"--latin-profile",latin_profiles);
    profile(p,L"--latin-paragraph",latin_paragraph_profiles);
    profile(p,L"--latin-hits",latin_paragraph_hits);
    if(is_option(p,L"--latin-paragraph-fuzz")){reference_shaping(false);return latin_paragraph_fuzz();}
    profile(p,L"--directional-semantic-profile",directional_semantic_profiles);
    profile(p,L"--directional-large-profile",directional_large_profiles);
    profile(p,L"--directional-editor-cuts-profile",directional_editor_cuts_profiles);
    profile(p,L"--multiline-hit-profile",multiline_trim_profiles,multiline_hit_profiles);
    profile(p,L"--multiline-trim-profile",multiline_trim_profiles);
    profile(p,L"--ellipsis-line-profile",ellipsis_line_profiles);
    profile(p,L"--ellipsis-style-profile",ellipsis_style_profiles);
    profile(p,L"--ellipsis-hit-profile",ellipsis_hit_profiles);
    profile(p,L"--ellipsis-cut-profile",ellipsis_cut_profiles);
    profile(p,L"--common-punctuation-profile",common_punctuation_profiles);
    profile(p,L"--ellipsis-sign-profile",ellipsis_sign_profiles);
    profile(p,L"--height-overflow-profile",height_overflow_profiles);
    profile(p,L"--cjk-paragraph-profile",cjk_paragraph_profiles);
    profile(p,L"--cjk-editor-profile",cjk_editor_profiles);
    profile(p,L"--cjk-editor-paint-profile",cjk_editor_paint_profiles);
    profile(p,L"--arabic-editor-profile",arabic_editor_profiles);
    profile(p,L"--arabic-paragraph-profile",arabic_paragraph_profiles);
    profile(p,L"--directional-editor-profile",directional_editor_profiles);
    profile(p,L"--semantic-profile",semantic_profiles);
    profile(p,L"--styled-hit-profile",styled_hit_profiles);
    if(is_option(p,L"--semantic-options"))return semantic_options();
    profile(p,L"--style-profile",style_profiles);
    profile(p,L"--decoration-profile",decoration_profiles);
    if(is_option(p,L"--decoration-options"))return decoration_options();
    if(is_option(p,L"--telugu-styled"))return styled_paragraph_checks(true,false,false);
    if(is_option(p,L"--arabic-styled"))return styled_paragraph_checks(false,false,true);
    if(is_option(p,L"--kannada-styled"))return styled_paragraph_checks(false,true,false);
    if(is_option(p,L"--odia-styled"))return styled_paragraph_checks(false,false,false);
    if(take(p,L"--paragraph-profile",true))paragraph_profiles=true;
    profile(p,L"--paragraph-expansion",paragraph_expansion);
    const bool system_family=take(p,L"--family",true);
    if(*p){wchar_t path[32768];u32 n=0;quote=*p=='"';if(quote)++p;while(*p&&!(quote?*p=='"':*p==' '||*p=='\t')){CHECK(n+1<32768);path[n++]=*p++;}path[n]=0;if(quote&&*p=='"')++p;while(*p==' '||*p=='\t')++p;u32 weight=0;while(*p>='0'&&*p<='9'){CHECK(weight<10000);weight=weight*10+*p++-'0';}return system_family?corpus(0,path,weight):custom(path,weight);}
    CHECK(input_validation());
    CHECK(corpus(0,L"Segoe UI",400));
    CHECK(retained_run_lifetime());
    CHECK(compare(owner,utf8_literal("AV To office"),false,30));
    CHECK(compare(owner,utf8_literal("AV To office"),true,30,1000000,TextWrapping::no_wrap,TextOverflow::ellipsis));
    CHECK(compare(owner,utf8_literal("a\nb"),false));CHECK(compare(owner,utf8_literal("Hello \xf0\x9f\x98\x80"),false));
    CHECK(compare(owner,utf8_literal("A\r\nB"),false));CHECK(native_output.text.cluster_count==4);
    CHECK(compare(owner,utf8_literal("A \xf0\x9f\x8f\xb4\xf3\xa0\x81\xa7\xf3\xa0\x81\xa2\xf3\xa0\x81\xa5\xf3\xa0\x81\xae\xf3\xa0\x81\xa7\xf3\xa0\x81\xbf B"),false));
    CHECK(native_output.text.cluster_count==18&&native_output.text.glyph_count>0);
    auto tracked=owner;tracked.tracking=.03f;CHECK(compare(tracked,utf8_literal("AV To office"),false));
    CHECK(compare(owner,{},false));
    return true;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_native_app_line_entry() noexcept {
    using namespace maicord::native_app_line;
    const bool started=reference_begin();const bool ok=started&&run()&&draw_scratch_ready;
    shaped_text_reset(&native_output.text);shaped_text_reset(&second_output.text);reference_release_sessions();const bool retained=draw_retained_release();
    text_system_release(&owner);arena_release(&draw_scratch);
    print("cases=");number(cases);print(" native_cases=");number(native_cases);print(" simulated_cases=");number(simulated_cases);print(" checks=");number(checks);print(ok?" PASS native app lines\n":" FAIL native app lines\n");
    const maicord::u32 status=reference_end(ok&&retained&&print_ok);ExitProcess(status);
}
