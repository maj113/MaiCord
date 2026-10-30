// Native text engine against DirectWrite (MaiCordNativeAppLineTests). The modes
// registered in cmake/test_registry.cmake are the suite. Manual modes, not run by it:
//   benchmarks  --devanagari-bench (tools/benchmark_devanagari.py)
//   dumps for external comparison  --inspect (the registered
//     --bidi-range-check is the quiet form of the bidi range dump)
//   exhaustive variants  --matrix-batch (the union of the registered --matrix-profile 0-4, which
//     run --ellipsis-hit-profile, the quick --multiline-hit-profile, --style-profile and
//     --decoration-profile) and the unsampled --multiline-hit-profile, --multiline-trim-profile
//     and --multiline-plan-profile (3-12 s each)
//   slower checks, left out of the suite's 20 s budget  --alphabetic-paragraph (2 s),
//     --arabic-paragraph-profile, --arabic-editor-profile and --cjk-editor-paint-profile (1-2 s each,
//     with --family "Segoe UI" 400)
//   needs external input  --paragraph-expansion <font> <weight> (a font whose letters expand)
//   debug toggles inside the registered corpora  MAICORD_ELLIPSIS_CUT_AUDIT (extra logging) and
//     MAICORD_CJK_SAMPLE, MAICORD_CJK_VARIANT, MAICORD_DEVANAGARI_SAMPLE (select one sample)
#include "native_app_line_test.hpp"
namespace maicord::native_app_line {namespace {
bool input_validation() noexcept {
    DWriteTextSystem native{};
    auto* guarded=static_cast<char*>(VirtualAlloc(nullptr,8192,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
    struct Release {DWriteTextSystem& system;char* memory;~Release() noexcept {
        shaped_text_reset(&native_output.text);text_system_release(&system);
        if(memory)VirtualFree(memory,0,MEM_RELEASE);
    }} release{native,guarded};
    DWORD old=0;CHECK(guarded&&VirtualProtect(guarded+4096,4096,PAGE_NOACCESS,&old));
    CHECK(text_system_initialize(static_cast<TextSystem*>(&native),L"Segoe UI",16).ok());
    auto reference=owner;reference.native_text=nullptr;
    DWriteTextSystem systems[]{native,owner,reference};u32 tested=0;
    const Utf8View samples[]{utf8_literal("A"),utf8_literal("\xc2\x80"),utf8_literal("\xdf\xbf"),
        utf8_literal("\xe0\xa0\x80"),utf8_literal("\xed\x9f\xbf"),utf8_literal("\xee\x80\x80"),
        utf8_literal("\xef\xbf\xbf"),utf8_literal("\xf0\x90\x80\x80"),utf8_literal("\xf4\x8f\xbf\xbf"),
        utf8_literal("\x80"),utf8_literal("\xc0\x80"),utf8_literal("\xe0\x80\x80"),
        utf8_literal("\xed\xa0\x80"),utf8_literal("\xf0\x80\x80\x80"),utf8_literal("\xf4\x90\x80\x80"),
        utf8_literal("\xff"),utf8_literal("A\xe2\x82"),utf8_literal("A\0B")};
    for(u32 side=0;side<3;++side){auto& system=systems[side];
        const auto check=[&](Utf8View input,Slice<wchar_t> scratch,TextShapeError error,u32 units) noexcept {
            const auto a=text_shape_utf8(&system,input,{},10000,10000,scratch,&native_output.text);
            const auto d=text_measure_utf8(&system,input,{},10000,10000,scratch);
            CHECK(a.error==error&&d.error==error);
            CHECK(a.utf16_units==units&&d.utf16_units==units);
            ++tested;return true;
        };
        for(auto sample:samples)for(u32 bytes=0;bytes<=sample.bytes;++bytes){
            char* input=guarded+4096-bytes;copy_bytes(input,sample.data,bytes);
            const int units=bytes?MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,input,bytes,nullptr,0):0;
            const auto error=!bytes?TextShapeError::none:units?TextShapeError::output_too_small:TextShapeError::invalid_utf8;
            CHECK(check({input,bytes},{},error,units>0?static_cast<u32>(units):0));
        }
        CHECK(check({nullptr,1},{},TextShapeError::invalid_input,0));
        CHECK(check(utf8_literal("A"),{nullptr,1},TextShapeError::invalid_input,0));
        const auto mixed=utf8_literal("A\xc3\xa9\xe6\xb0\xb4\xf0\x9f\x98\x80");
        for(u32 capacity=0;capacity<=6;++capacity){wchar_t scratch[8];for(auto& unit:scratch)unit=0x1234;
            CHECK(check(mixed,{scratch+1,capacity},capacity<5?TextShapeError::output_too_small:TextShapeError::none,5));
            CHECK(scratch[0]==0x1234&&scratch[6]==0x1234&&scratch[7]==0x1234);
            if(side==2&&capacity>=5){const wchar_t expected[]{L'A',0xe9,0x6c34,0xd83d,0xde00};CHECK(bytes_equal(scratch+1,expected,sizeof(expected)));}
            else for(auto unit:scratch)CHECK(unit==0x1234);
        }
    }
    print("input_validation_cases=");number(tested);print("\n");return true;
}
bool corpus(IDWriteFontCollection* collection,const wchar_t* family,u32 weight) noexcept {
    if(paragraph_expansion){
        IDWriteTextFormat* format=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTextFormat(family,collection,static_cast<DWRITE_FONT_WEIGHT>(weight?weight:400),DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,16,L"en-US",&format)));
        struct Release {IDWriteTextFormat*& p;~Release() noexcept {com_release(p);}} release{format};auto system=owner;system.format=format;CHECK(native_text_format_capture(&system));
        char text[123];for(u32 i=0;i<123;++i)text[i]='A';
        CHECK(compare(system,{text,40},true));CHECK(native_output.text.glyph_count==160);
        CHECK(compare(system,{text,40},true,39,1));CHECK(native_output.text.glyph_count==160);
        text[40]='\t';text[81]='\r';text[82]='\n';
        CHECK(compare(system,{text,123},true,39,1));CHECK(native_output.text.glyph_count==480);
        CHECK(compare(system,{text,123},true,1000,1000));CHECK(native_output.text.glyph_count==480);
        return true;
    }
    const Utf8View texts[]{utf8_literal("a"),utf8_literal("AV To"),utf8_literal("ffi ffl fi fl"),utf8_literal("  a  "),utf8_literal("   "),utf8_literal("office AV To  "),utf8_literal("Hello, world!"),utf8_literal("AV To office AV To office AV To office AV To office AV To office AV To office AV To office AV To office AV To office AV To office AV To office AV To office ")};
    bool cjk_ok=true;const float sizes[]{14.f,17.5f,1000.f};for(float size:sizes){
        IDWriteTextFormat* format=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTextFormat(family,collection,static_cast<DWRITE_FONT_WEIGHT>(weight?weight:400),DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,size,L"en-US",&format)));
        struct Release{IDWriteTextFormat*& f;~Release() noexcept {com_release(f);}} release{format};
        auto system=owner;system.format=format;CHECK(native_text_format_capture(&system));
        if(simulated_profiles){system.em_size=size;CHECK(native_simulated_profile_corpus(system));continue;}
        if(bidi_styles_profiles){system.em_size=size;system.fixed_weight=collection!=nullptr;CHECK(native_bidi_styles_corpus(system));continue;}
        if(mixed_bidi_profiles){system.em_size=size;system.fixed_weight=collection!=nullptr;CHECK(native_mixed_bidi_corpus(system));continue;}
        if(hebrew_profiles){system.em_size=size;system.fixed_weight=collection!=nullptr;CHECK(native_hebrew_corpus(system));continue;}
        if(system_fallback_profiles){system.em_size=size;CHECK(native_system_fallback_corpus(system,collection!=nullptr,!collection&&CompareStringOrdinal(family,-1,L"Segoe UI",-1,FALSE)==CSTR_EQUAL));continue;}
        if(alphabetic_profiles){u32 n=0;while(family[n])++n;CHECK(alphabetic_corpus(system,!collection&&n==8&&bytes_equal(family,L"Segoe UI",16)));continue;}
        if(multiline_trim_profiles){system.em_size=size;system.fixed_weight=collection!=nullptr;if(!native_multiline_trim_corpus(system))cjk_ok=false;continue;}
        if(height_overflow_profiles){system.em_size=size;system.fixed_weight=collection!=nullptr;CHECK(native_height_overflow_corpus(system));continue;}
        if(ellipsis_sign_profiles){system.em_size=size;system.fixed_weight=collection!=nullptr;CHECK(native_ellipsis_sign_corpus(system));continue;}
        if(ellipsis_cut_profiles||ellipsis_line_profiles){system.em_size=size;system.fixed_weight=collection!=nullptr;if(!native_ellipsis_cut_corpus(system))cjk_ok=false;continue;}
        if(ellipsis_style_profiles||ellipsis_hit_profiles){system.em_size=size;system.fixed_weight=collection!=nullptr;if(!native_ellipsis_style_corpus(system))cjk_ok=false;continue;}
        if(common_punctuation_profiles){system.em_size=size;system.fixed_weight=collection!=nullptr;CHECK(native_common_punctuation_corpus(system));continue;}
        if(hangul_profiles){system.em_size=size;system.fixed_weight=collection!=nullptr;if(!native_hangul_corpus(system))return false;continue;}
        if(thai_profiles){system.em_size=size;system.fixed_weight=collection!=nullptr;if(!native_thai_corpus(system))return false;continue;}
        if(standard_script_profiles){system.em_size=size;system.fixed_weight=collection!=nullptr;if(!native_standard_script_corpus(system))return false;continue;}
        if(devanagari_profiles){system.em_size=size;system.fixed_weight=collection!=nullptr;if(!native_devanagari_corpus(system))return false;continue;}
        if(cjk_editor_paint_profiles){system.em_size=size;system.fixed_weight=collection!=nullptr;if(!native_cjk_editor_paint_corpus(system))cjk_ok=false;continue;}
        if(cjk_paragraph_profiles||cjk_editor_profiles){system.em_size=size;system.fixed_weight=collection!=nullptr;if(!native_cjk_paragraph_corpus(system,cjk_editor_profiles))cjk_ok=false;continue;}
        if(arabic_paragraph_profiles||arabic_editor_profiles){system.em_size=size;system.fixed_weight=collection!=nullptr;CHECK(native_arabic_paragraph_corpus(system,arabic_editor_profiles));continue;}
        if(tracking_profiles){CHECK(native_tracking_corpus(system));continue;}
        if(style_continuity_profiles){CHECK(style_continuity_corpus(system));continue;}
        if(style_cluster_profiles){system.em_size=size;u32 family_length=0;while(family[family_length])++family_length;const bool required=!collection&&weight==400&&family_length==8&&bytes_equal(family,L"Segoe UI",16);
            CHECK(style_cluster_corpus(system,required));continue;}
        if(styled_hit_profiles){u32 family_length=0;while(family[family_length])++family_length;const bool required=!collection&&weight==400&&family_length==8&&bytes_equal(family,L"Segoe UI",16);
            CHECK(styled_hit_corpus(system,required));continue;}
        if(directional_editor_profiles||directional_editor_cuts_profiles){system.em_size=size;system.fixed_weight=collection!=nullptr;CHECK(directional_editor_hit_corpus(system,directional_editor_cuts_profiles));continue;}
        if(directional_large_profiles){system.em_size=size;system.fixed_weight=collection!=nullptr;if(size==1000)CHECK(directional_semantic_large_origin(system));continue;}
        if(directional_semantic_profiles){system.em_size=size;system.fixed_weight=collection!=nullptr;CHECK(directional_semantic_corpus(system));continue;}
        if(semantic_profiles){u32 family_length=0;while(family[family_length])++family_length;const bool required=!collection&&weight==400&&family_length==8&&bytes_equal(family,L"Segoe UI",16);
            CHECK(semantic_corpus(system,required));continue;}
        if(style_profiles||decoration_profiles){
            const Utf8View style_texts[]{utf8_literal("AV office caf\xc3\xa9 fi AV"),utf8_literal("AV office e\xcc\x81 fi AV\n\xc3\xa9 AV"),utf8_literal("AV\toffice\r\n\xc3\xa9 AV\n"),utf8_literal("AV office\n\n"),utf8_literal("AV office\t\t   "),utf8_literal("AV office\r\n\t\t")};
            const u32 font_flags[]{text_style_bold,text_style_italic,text_style_bold|text_style_italic,text_style_heading_1,text_style_heading_2,text_style_heading_3,text_style_subtext,text_style_link,text_style_syntax_keyword,text_style_monospace,text_style_heading_1|text_style_bold,text_style_subtext|text_style_italic};
            const u32 decoration_flags[]{text_style_underline,text_style_strikethrough,text_style_underline|text_style_strikethrough,text_style_underline|text_style_heading_1,text_style_strikethrough|text_style_italic,text_style_monospace,text_style_monospace|text_style_code_block,text_style_spoiler,text_style_mention,text_style_quote,text_style_list|text_style_list_bullet,text_style_list|text_style_list_bullet|text_style_list_depth_1};
            const u32* flags=decoration_profiles?decoration_flags:font_flags;
            const float widths[]{8.f,37.f,95.f,1000000.f};
            u32 family_length=0;while(family[family_length])++family_length;const bool require_native=decoration_profiles&&!collection&&weight==400&&family_length==8&&bytes_equal(family,L"Segoe UI",16);
            for(u32 mode=0;mode<4;++mode){system.independent_letters=(mode&1)!=0;system.fixed_weight=(mode&2)!=0;for(u32 sample=0;sample<(decoration_profiles?6u:4u);++sample)for(u32 flags_index=0;flags_index<12;++flags_index)for(u32 placement=0;placement<(decoration_profiles?7u:5u);++placement){
                const auto text=style_texts[sample];TextStyleSpan spans[3]{};u32 count=1;
                if(placement==0)spans[0]={3,6,flags[flags_index]};else if(placement==1)spans[0]={0,text.bytes,flags[flags_index]};else if(placement==2){spans[0]={0,1,flags[flags_index]};spans[1]={1,1,text_style_link};count=2;}else if(placement==3)spans[0]={9,text.bytes-9,flags[flags_index]};else if(placement==4)spans[0]={2,1,flags[flags_index]};
                else if(placement==5){spans[0]={0,3,flags[flags_index]};spans[1]={3,6,flags[flags_index]|text_style_heading_3};spans[2]={9,text.bytes-9,flags[flags_index]|text_style_bold};count=3;}
                else{spans[0]={0,3,flags[flags_index]};spans[1]={3,text.bytes-3,flags[flags_index]};count=2;}
                for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap){++matrix_seen;++cases;const auto wrap=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;auto reference=system;reference.native_text=nullptr;
                    shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);
                    const bool native=native_text_candidate_styled(system,text,{spans,count},width,1000000,&native_output.text,nullptr,wrap);CHECK(!require_native||native);if(native){++native_cases;CHECK(draw_source(system,native_output.text));}shaped_text_reset(&native_output.text);
                    CHECK(text_shape_utf8(&system,text,{spans,count},width,1000000,{utf16,256},&native_output.text,wrap).ok());
                    CHECK(text_shape_utf8(&reference,text,{spans,count},width,1000000,{utf16,256},&reference_output.text,wrap).ok());
                    const auto a=text_measure_utf8(&system,text,{spans,count},width,1000000,{utf16,256},wrap);
                    TextMeasureResult b{};
                    // The first and last samples independently measure every
                    // font, style, placement, width and wrapping combination.
                    CHECK(reference_shape_measure(reference,text,{spans,count},width,1000000,{utf16,256},wrap,
                        sample==0||sample+1==(decoration_profiles?6u:4u),&b));
                    if(!same(native_output,reference_output)||!a.ok()||!b.ok()||!bytes_equal(&a.width,&b.width,12)||a.utf16_units!=b.utf16_units||a.line_count!=b.line_count){print("style sample/flags/placement/em/width/independent/nowrap=");number(sample);print(",");number(flags_index);print(",");number(placement);print(",");real(size);print(",");real(width);print(",");number(mode);print(",");number(nowrap);print("\n");CHECK(inspect_style_layout(system,text,{spans,count},width,wrap));return false;}
                }
            }}continue;
        }
        if(latin_paragraph_profiles||latin_paragraph_hits){const float widths[]{8.f,37.f,95.f,1000000.f};
            for(u32 mode=0;mode<2;++mode){system.independent_letters=mode!=0;
                const ConstSlice<Utf8View> sets[]{{latin_samples,sizeof(latin_samples)/sizeof(latin_samples[0])},{latin_paragraph_samples,sizeof(latin_paragraph_samples)/sizeof(latin_paragraph_samples[0])}};
                for(u32 set=0;set<2;++set)for(u32 sample=0;sample<sets[set].count;++sample){const auto text=sets[set][sample];
                    shaped_text_reset(&native_output.text);const bool native=native_text_candidate_line(system,text,1000000,1000000,&native_output.text,nullptr);
                    shaped_text_reset(&native_output.text);
                    for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap){const auto wrap=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;const float actual=width==1000000?width:width*(size/16);
                        if(!(latin_paragraph_hits?(!native||compare_hits(system,text,actual,wrap)):compare(system,text,native,actual,1000000,wrap))){
                            print("latin paragraph set/sample/em/width/independent/nowrap=");number(set);print(",");number(sample);print(",");real(size);print(",");real(actual);print(",");number(mode);print(",");number(nowrap);print("\n");return false;}
                    }
                }
            }continue;
        }
        if(latin_profiles){for(u32 mode=0;mode<2;++mode){system.independent_letters=mode!=0;for(const auto text:latin_samples){
            shaped_text_reset(&native_output.text);const bool native=native_text_candidate_line(system,text,1000000,1000000,&native_output.text,nullptr);
            shaped_text_reset(&native_output.text);CHECK(compare(system,text,native));}}continue;}
        if(hit_profiles){const float widths[]{8.f,37.f,95.f,1000000.f};for(u32 mode=0;mode<2;++mode){system.independent_letters=mode!=0;for(const auto text:paragraph_samples)for(float width:widths){
            const float actual=width==1000000?width:width*(size/16);CHECK(compare_hits(system,text,actual,TextWrapping::wrap));CHECK(compare_hits(system,text,actual,TextWrapping::no_wrap));}}continue;}
        if(paragraph_profiles){const float widths[]{8.f,37.f,95.f,1000000.f};for(u32 mode=0;mode<2;++mode){system.independent_letters=mode!=0;for(const auto text:paragraph_samples)for(float width:widths){
            const float actual=width==1000000?width:width*(size/16);CHECK(compare(system,text,true,actual,1));}}continue;}
        for(u32 mode=0;mode<2;++mode){system.independent_letters=mode!=0;for(const auto text:texts){
            auto reference=system;reference.native_text=nullptr;shaped_text_reset(&reference_output.text);
            CHECK(text_shape_utf8(&reference,text,{},1000000,1000000,{utf16,256},&reference_output.text).ok());
            bool simulated=false;for(u32 i=0;i<reference_output.text.run_count;++i)simulated|=reference_run_face(reference_output.text.runs[i])->GetSimulations()!=DWRITE_FONT_SIMULATIONS_NONE;
            if(simulated){CHECK(!native_text_candidate_line(system,text,1000000,1000000,&native_output.text,nullptr));++simulated_cases;}
            CHECK(compare(system,text,!simulated));
        }}
    }
    return cjk_ok;
}
bool custom(const wchar_t* path,u32 weight) noexcept {
    // Packaged desktop processes can redirect LocalAppData to their package
    // cache. Give both sides the physical path of the requested file; otherwise
    // DirectWrite Equals can reject two aliases of that very same file.
    wchar_t physical[32768]{};const HANDLE source=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    CHECK(source!=INVALID_HANDLE_VALUE);const DWORD physical_units=GetFinalPathNameByHandleW(source,physical,32768,FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);CloseHandle(source);
    CHECK(physical_units&&physical_units<32768);path=physical_units>6&&physical[0]=='\\'&&physical[1]=='\\'&&physical[2]=='?'&&physical[3]=='\\'&&physical[5]==':'?physical+4:physical;
    IDWriteFactory3* factory=nullptr;IDWriteFontFile* file=nullptr;IDWriteFontFaceReference* face=nullptr;IDWriteFontSetBuilder* builder=nullptr;IDWriteFontSet* set=nullptr;IDWriteFontCollection1* collection=nullptr;IDWriteFontFamily* family=nullptr;IDWriteLocalizedStrings* names=nullptr;
    struct Release {IDWriteFactory3*& a;IDWriteFontFile*& b;IDWriteFontFaceReference*& c;IDWriteFontSetBuilder*& d;IDWriteFontSet*& e;IDWriteFontCollection1*& f;IDWriteFontFamily*& g;IDWriteLocalizedStrings*& h;~Release() noexcept {com_release(h);com_release(g);com_release(f);com_release(e);com_release(d);com_release(c);com_release(b);com_release(a);}} release{factory,file,face,builder,set,collection,family,names};
    CHECK(SUCCEEDED(owner.factory->QueryInterface(__uuidof(IDWriteFactory3),reinterpret_cast<void**>(&factory))));
    CHECK(SUCCEEDED(factory->CreateFontFileReference(path,nullptr,&file))&&SUCCEEDED(factory->CreateFontFaceReference(file,0,DWRITE_FONT_SIMULATIONS_NONE,&face))&&SUCCEEDED(factory->CreateFontSetBuilder(&builder))&&SUCCEEDED(builder->AddFontFaceReference(face))&&SUCCEEDED(builder->CreateFontSet(&set))&&SUCCEEDED(factory->CreateFontCollectionFromFontSet(set,&collection)));
    CHECK(SUCCEEDED(collection->GetFontFamily(0,&family))&&SUCCEEDED(family->GetFamilyNames(&names)));wchar_t name[256];CHECK(SUCCEEDED(names->GetString(0,name,256)));
    // A private collection must not borrow the system family's selection before
    // registration. Then register from disposable caller storage and retry the
    // same cached format. The corpus verifies actual selected faces/axes afterward.
    IDWriteTextFormat* unknown=nullptr;
    CHECK(SUCCEEDED(owner.factory->CreateTextFormat(name,collection,static_cast<DWRITE_FONT_WEIGHT>(weight?weight:400),DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,16,L"en-US",&unknown)));
    struct ReleaseFormat {IDWriteTextFormat*& f;~ReleaseFormat() noexcept {com_release(f);}} release_format{unknown};
    auto system=owner;system.format=unknown;system.native_format=0;shaped_text_reset(&native_output.text);
    CHECK(!native_text_candidate_line(system,utf8_literal("AV"),1000,1000,&native_output.text,nullptr)&&!native_output.text.run_count);
    wchar_t copied[32768];u32 units=0;while(path[units]){CHECK(units<32767);copied[units]=path[units];++units;}copied[units]=0;
    CHECK(native_text_register_collection(owner.native_text,collection,copied));copied[0]=L'!';
    CHECK(native_text_register_collection(owner.native_text,collection,path));
    CHECK(!native_text_register_collection(owner.native_text,collection,L"Z:\\different-font.ttf"));
    CHECK(!native_text_register_collection(owner.native_text,nullptr,path));
    CHECK(!native_text_register_collection(owner.native_text,collection,L""));
    auto reference=system;reference.native_text=nullptr;shaped_text_reset(&reference_output.text);
    CHECK(text_shape_utf8(&reference,utf8_literal("AV"),{},1000,1000,{utf16,256},&reference_output.text).ok());
    if(reference_output.text.run_count==1&&reference_run_face(reference_output.text.runs[0])->GetSimulations()==DWRITE_FONT_SIMULATIONS_NONE){
        CHECK(native_text_candidate_line(system,utf8_literal("AV"),1000,1000,&native_output.text,nullptr));CHECK(same(native_output,reference_output));}
    shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);
    return corpus(collection,name,weight);
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
        shaped_text_reset(&reference_output.text);
        ellipsis_style_profiles = ellipsis_hit_profiles = false;
        multiline_trim_profiles = multiline_plan_profiles = multiline_hit_profiles = multiline_quick =
            false;
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
        const bool ok = profile == 2 ? custom(private_path, 0) : corpus(nullptr, L"Segoe UI", 400);
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
    shaped_text_reset(&reference_output.text);
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
bool shared_oblique_binding() noexcept {
    shaped_text_reset(&reference_output.text);auto system=owner;system.native_text=nullptr;
    CHECK(text_shape_utf8(&system,utf8_literal("AV"),{},1000,1000,{utf16,256},&reference_output.text).ok());
    CHECK(reference_output.text.run_count==1);auto* selected=reference_run_face(reference_output.text.runs[0]);
    IDWriteFontFace5* base=nullptr;IDWriteFontResource* resource=nullptr;IDWriteFontFace5* oblique=nullptr;Arena scratch{};NativeFontInstanceHandle plain{},styled{},rejected{};
    struct Release {IDWriteFontFace5*& b;IDWriteFontResource*& r;IDWriteFontFace5*& o;Arena& a;NativeFontInstanceHandle& p;NativeFontInstanceHandle& s;
        ~Release() noexcept {auto* store=native_font_session_instances(owner.native_fonts);if(p.slot)native_font_instance_release(store,p);if(s.slot)native_font_instance_release(store,s);arena_release(&a);com_release(o);com_release(r);com_release(b);}} release{base,resource,oblique,scratch,plain,styled};
    CHECK(SUCCEEDED(selected->QueryInterface(__uuidof(IDWriteFontFace5),reinterpret_cast<void**>(&base)))&&SUCCEEDED(base->GetFontResource(&resource)));
    DWRITE_FONT_AXIS_VALUE axes[64];const u32 count=base->GetFontAxisValueCount();CHECK(count<=64&&SUCCEEDED(base->GetFontAxisValues(axes,count)));
    CHECK(SUCCEEDED(resource->CreateFontFace(DWRITE_FONT_SIMULATIONS_OBLIQUE,axes,count,&oblique))&&oblique->GetSimulations()==DWRITE_FONT_SIMULATIONS_OBLIQUE);
    CHECK(arena_initialize(&scratch,4*1024*1024,0,4096));bool is_oblique=true;
    NativeFontSelection source{};
    CHECK(native_font_bind_selected(owner.native_fonts,selected,true,&scratch,&plain,&is_oblique,nullptr,&source).ok()&&!is_oblique&&source.face.slot);
    NativeFontInstanceHandle reused{};arena_reset(&scratch);
    CHECK(native_font_prepare(owner.native_fonts,source.face,{source.settings,source.count},0,true,&scratch,&reused).ok());
    CHECK(reused.slot==plain.slot&&reused.generation==plain.generation&&native_font_instance_release(native_font_session_instances(owner.native_fonts),reused));
    CHECK(native_font_prepare(owner.native_fonts,{}, {},0,true,&scratch,&reused).error==NativeFontError::invalid_input&&!reused.slot);
    CHECK(!native_font_bind_selected(owner.native_fonts,oblique,true,&scratch,&rejected).ok()&&!rejected.slot);
    CHECK(native_font_bind_selected(owner.native_fonts,oblique,true,&scratch,&styled,&is_oblique).ok()&&is_oblique);
    CHECK(plain.slot==styled.slot&&plain.generation==styled.generation);
    const u32 simulations[]{1,3};for(u32 mode:simulations){
        IDWriteFontFace5* bold_face=nullptr;NativeFontInstanceHandle bold_handle{};
        struct ReleaseBold {IDWriteFontFace5*& f;NativeFontInstanceHandle& h;~ReleaseBold() noexcept {if(h.slot)native_font_instance_release(native_font_session_instances(owner.native_fonts),h);com_release(f);}} release_bold{bold_face,bold_handle};
        CHECK(SUCCEEDED(resource->CreateFontFace(static_cast<DWRITE_FONT_SIMULATIONS>(mode),axes,count,&bold_face)));
        CHECK(!native_font_bind_selected(owner.native_fonts,bold_face,true,&scratch,&rejected,&is_oblique).ok()&&!rejected.slot);
        bool is_bold=false;CHECK(native_font_bind_selected(owner.native_fonts,bold_face,true,&scratch,&bold_handle,&is_oblique,&is_bold).ok()&&is_bold&&is_oblique==((mode&2)!=0));
        CHECK(plain.slot==bold_handle.slot&&plain.generation==bold_handle.generation);
    }
    return true;
}
bool retained_run_lifetime() noexcept {
    DWriteTextSystem transient{};NativeFontSession* observer=nullptr;NativeFontInstanceHandle outlined{},bridged{};IDWriteFontFace* reference=nullptr;Arena scratch{};
    struct Release {DWriteTextSystem& t;NativeFontSession*& s;NativeFontInstanceHandle& a;NativeFontInstanceHandle& b;IDWriteFontFace*& f;Arena& scratch;
        ~Release() noexcept {shaped_text_reset(&native_output.text);text_system_release(&t);if(s){auto* store=native_font_session_instances(s);if(a.slot)native_font_instance_release(store,a);if(b.slot)native_font_instance_release(store,b);native_font_session_release(&s);}com_release(f);arena_release(&scratch);}}
        release{transient,observer,outlined,bridged,reference,scratch};
    shaped_text_reset(&native_output.text);CHECK(text_system_initialize(&transient,L"Segoe UI",16).ok());
    CHECK(native_text_candidate_line(transient,utf8_literal("AV  "),1000,1000,&native_output.text,nullptr));
    CHECK(native_output.text.run_count==2&&native_output.text.runs[0].native_font&&native_output.text.runs[1].native_font);
    const auto handle=native_output.text.runs[0].native_face;observer=transient.native_fonts;CHECK(native_font_session_retain(observer));
    CHECK(native_output.text.native_fonts()==observer);
    CHECK(native_run_reference(transient.factory,native_output.text,native_output.text.runs[0],&reference));
    text_system_release(&transient);
    CHECK(native_font_session_stats(observer).references==2&&native_font_run_get(*native_font_session_runs(observer),handle));
    CHECK(arena_initialize(&scratch,4*1024*1024,0,4096));
    CHECK(native_font_run_prepare(native_font_session_runs(observer),handle,&scratch,&outlined).ok());
    CHECK(native_font_bind_selected(observer,reference,true,&scratch,&bridged).ok());
    CHECK(outlined.slot==bridged.slot&&outlined.generation==bridged.generation);
    CHECK(native_font_instance_release(native_font_session_instances(observer),bridged));bridged={};
    shaped_text_reset(&native_output.text);
    CHECK(!native_output.text.native_fonts()&&!native_font_run_get(*native_font_session_runs(observer),handle));
    CHECK(native_font_session_stats(observer).references==1&&native_font_session_stats(observer).instances==1&&native_font_instance_get(*native_font_session_instances(observer),outlined));
    CHECK(!native_font_session_release(&observer)&&observer);
    CHECK(native_font_instance_release(native_font_session_instances(observer),outlined));outlined={};
    CHECK(!native_font_session_stats(observer).live.files&&!native_font_session_stats(observer).instances);
    CHECK(native_font_session_release(&observer)&&!observer);return true;
}
bool inspect_paragraphs(bool compare_native=false) noexcept {
    const Utf8View texts[]{utf8_literal("AV AV"),utf8_literal("AV"),utf8_literal("fi fi"),utf8_literal("ffi"),utf8_literal("a\r\nb"),utf8_literal("a\nb"),utf8_literal("a\n"),utf8_literal("\n"),utf8_literal("a  b"),utf8_literal(" a b"),utf8_literal("a\tb"),utf8_literal(" \t "),utf8_literal("a-b/c"),utf8_literal("a...b"),utf8_literal("(a b) c"),utf8_literal("   "),utf8_literal("a     ")};
    const wchar_t* families[]{L"Segoe UI",L"Georgia",L"Consolas"};const float widths[]{.01f,5.f,10.f,15.f,20.f,30.f,45.f,90.f,200.f},heights[]{5.f,1000.f};
    for(u32 family=0;family<3;++family){IDWriteTextFormat* format=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTextFormat(families[family],nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,16,L"en-US",&format)));
        struct Release {IDWriteTextFormat*& f;~Release() noexcept {com_release(f);}} release{format};
        auto system=owner;system.native_text=nullptr;system.format=format;system.native_format=0;
        for(u32 sample=0;sample<sizeof(texts)/sizeof(texts[0]);++sample)for(float width:widths)for(float height:heights){
            shaped_text_reset(&reference_output.text);const auto result=text_shape_utf8(&system,texts[sample],{},width,height,{utf16,256},&reference_output.text);CHECK(result.ok());const auto& out=reference_output.text;
            print("{\"family\":");number(family);print(",\"sample\":");number(sample);print(",\"width\":");real(width);print(",\"height\":");real(height);print(",\"metrics\":[");real(out.width);print(",");real(out.width_including_trailing_whitespace);print(",");real(out.height);print(",");number(out.line_count);print("],\"glyphs\":[");
            for(u32 i=0;i<out.glyph_count;++i){if(i)print(",");print("[");number(out.glyphs[i].index);print(",");real(out.glyphs[i].advance);print("]");}print("],\"clusters\":[");for(u32 i=0;i<out.cluster_count;++i){if(i)print(",");number(out.clusters[i]);}
            print("],\"runs\":[");for(u32 i=0;i<out.run_count;++i){if(i)print(",");const auto& r=out.runs[i];print("[");number(r.text_position);print(",");number(r.text_length);print(",");number(r.first_glyph);print(",");number(r.glyph_count);print(",");real(r.baseline_x);print(",");real(r.baseline_y);print("]");}
            IDWriteTextLayout* layout=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTextLayout(utf16,result.utf16_units,format,width,height,&layout)));DWRITE_LINE_METRICS lines[256];u32 count=0;const auto hr=layout->GetLineMetrics(lines,256,&count);com_release(layout);CHECK(SUCCEEDED(hr));
            print("],\"lines\":[");for(u32 i=0;i<count;++i){if(i)print(",");const auto& l=lines[i];print("[");number(l.length);print(",");number(l.trailingWhitespaceLength);print(",");number(l.newlineLength);print(",");real(l.height);print(",");real(l.baseline);print("]");}print("]}\n");
            if(compare_native){auto native=owner;native.format=format;CHECK(native_text_format_capture(&native));CHECK(compare(native,texts[sample],true,width,height));}
        }
    }return true;
}
bool native_break(const char* bytes,u32 count,u32 at) noexcept {
    NativeParagraphCluster clusters[80]{};for(u32 i=0;i<count;++i)clusters[i]={i,i+1,0,0,0,0};
    native_ascii_paragraph_flags({bytes,count},{clusters,count});return (clusters[at].flags&native_paragraph_break_before)!=0;
}
bool break_tests() noexcept {
    IDWriteTextAnalyzer* analyzer=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTextAnalyzer(&analyzer)));struct Release {IDWriteTextAnalyzer*& p;~Release() noexcept {com_release(p);}} release{analyzer};AsciiBreakReference reference{};u32 differences=0,total=0;
    for(u32 a=32;a<127;++a)for(u32 b=32;b<127;++b)for(u32 context=0;context<4;++context){char bytes[5];u32 count=0;
        if(context==1){bytes[count++]='(';bytes[count++]=' ';}else if(context==2)bytes[count++]='0';else if(context==3){bytes[count++]='\'';bytes[count++]=' ';}
        bytes[count++]=static_cast<char>(a);bytes[count++]=static_cast<char>(b);if(context==2)bytes[count++]='0';
        CHECK(reference.analyze(analyzer,{bytes,count}));for(u32 at=1;at<count;++at){++total;const bool native=native_break(bytes,count,at),expected=reference.after(at);
            if(native!=expected){++differences;print("{\"bytes\":[");for(u32 i=0;i<count;++i){if(i)print(",");number(static_cast<u8>(bytes[i]));}print("],\"at\":");number(at);print(",\"native\":");number(native);print(",\"reference\":");number(expected);print("}\n");}}
    }print("break_boundaries=");number(total);print(" differences=");number(differences);print("\n");return differences==0;
}
bool paragraph_fuzz() noexcept {
    IDWriteTextAnalyzer* analyzer=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTextAnalyzer(&analyzer)));struct Release {IDWriteTextAnalyzer*& p;~Release() noexcept {com_release(p);}} release{analyzer};AsciiBreakReference breaks{};
    constexpr char alphabet[]="aaab01   '-/\\.,:;()[]{}!?%$+\t\r\n";u32 state=0x39846113u;
    const auto random=[&]() noexcept {state^=state<<13;state^=state>>17;state^=state<<5;return state;};
    const float widths[]{13.f,29.f,61.f};
    for(u32 sample=0;sample<4000;++sample){char bytes[80];const u32 count=1+random()%79;for(u32 i=0;i<count;++i)bytes[i]=alphabet[random()%(sizeof(alphabet)-1)];
        for(float width:widths)if(!compare(owner,{bytes,count},true,width,1)){
            print("fuzz sample=");number(sample);print(" width=");real(width);print(" bytes=[");for(u32 i=0;i<count;++i){if(i)print(",");number(static_cast<u8>(bytes[i]));}print("]\n");CHECK(breaks.analyze(analyzer,{bytes,count}));
            for(u32 at=1;at<count;++at)if(breaks.after(at)!=native_break(bytes,count,at)){print("break difference at=");number(at);print(" ref=");number(breaks.after(at));print(" native=");number(native_break(bytes,count,at));print("\n");}return false;}
    }return true;
}
#include "styled_paragraph_benchmark.inc"
#include "native_decoration_options.inc"
bool latin_paragraph_fuzz() noexcept {
    const u32 alphabet[]{'a','e','f','i','A','V','0','1',' ',' ','\t','\r','\n','(',')','"','-','/',',','.','$','%',0xe9,0x10d,0x153,0x301,0x327,0x34f,0x200c,0x200d,0xa0,0x2002,0x202f,0x2010,0x2011,0x2014,0x2028,0x2029};
    u32 state=0x731e48adu;const auto random=[&]() noexcept {state^=state<<13;state^=state>>17;state^=state<<5;return state;};
    const float widths[]{13.f,37.f,95.f};
    for(u32 sample=0;sample<2000;++sample){char bytes[256];u32 count=0;const u32 length=1+random()%63;
        count+=utf8_encode(0xe9,bytes);for(u32 i=0;i<length;++i)count+=utf8_encode(alphabet[random()%(sizeof(alphabet)/sizeof(alphabet[0]))],bytes+count);
        const Utf8View text{bytes,count};shaped_text_reset(&native_output.text);const bool native=native_text_candidate_line(owner,text,1000000,1000000,&native_output.text,nullptr);shaped_text_reset(&native_output.text);
        for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap)if(!compare(owner,text,native,width,1000000,nowrap?TextWrapping::no_wrap:TextWrapping::wrap)){
            print("latin fuzz sample=");number(sample);print(" width=");real(width);print(" nowrap=");number(nowrap);print(" bytes=[");for(u32 i=0;i<count;++i){if(i)print(",");number(static_cast<u8>(bytes[i]));}print("]\n");return false;}
    }return true;
}
bool paragraph_options() noexcept {
    const float stops[]{3.5f,64.f,91.25f};const float widths[]{5.f,39.f,1000.f};const float heights[]{1.f,1000.f};
    for(float stop:stops){CHECK(SUCCEEDED(owner.format->SetIncrementalTabStop(stop)));
        for(const auto text:paragraph_samples)for(float width:widths)for(float height:heights)for(u32 mode=0;mode<2;++mode){
            const auto wrapping=mode?TextWrapping::no_wrap:TextWrapping::wrap;
            CHECK(compare(owner,text,true,width,height,wrapping,TextOverflow::clip));
            CHECK(compare(owner,text,false,width,height,wrapping,TextOverflow::ellipsis));
        }
    }
    CHECK(SUCCEEDED(owner.format->SetIncrementalTabStop(64.f)));
    // A cached paragraph must reflow after a format mutation and must decline
    // format semantics this specialized line fitter does not implement.
    CHECK(compare(owner,utf8_literal("AV\tAV\nAV AV"),true,30,1000));
    CHECK(SUCCEEDED(owner.format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP)));
    CHECK(compare(owner,utf8_literal("AV\tAV\nAV AV"),false,30,1000));
    CHECK(compare(owner,utf8_literal("AV\tAV\nAV AV"),true,30,1000,TextWrapping::no_wrap));
    CHECK(SUCCEEDED(owner.format->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP)));
    return true;
}
#include "alphabetic_paragraph_cases.inc"
#include "devanagari_memory_bench.inc"
#include "thai_wordbreak_probe.inc"
#include "thai_context_cases.inc"
#include "script_storage_cases.inc"
#include "font_range_replay_cases.inc"
#include "long_text_cases.inc"
#include "tsf_point_geometry.inc"
#include "native_historic_cases.inc"
#include "native_editor_policy_probe.inc"
#include "native_font_selection_cases.inc"
#include "native_fresh_engine_cases.inc"
#include "native_font_context_cases.inc"
#include "native_request_recovery.inc"
bool run() noexcept {
    const wchar_t* p=GetCommandLineW();bool quote=false;while(*p){if(*p=='"')quote=!quote;else if(!quote&&(*p==' '||*p=='\t'))break;++p;}while(*p==' '||*p=='\t')++p;
    static const wchar_t recovery_option[]=L"--request-recovery";u32 recovery_match=0;while(recovery_option[recovery_match]&&p[recovery_match]==recovery_option[recovery_match])++recovery_match;
    if(!recovery_option[recovery_match]&&!p[recovery_match])return native_request_recovery();
    static const wchar_t selection_check[]=L"--font-selection-check";u32 selection_match_check=0;while(selection_check[selection_match_check]&&p[selection_match_check]==selection_check[selection_match_check])++selection_match_check;
    if(!selection_check[selection_match_check]&&!p[selection_match_check])return native_font_selection_cases();
    static const wchar_t contexts_check[]=L"--font-context-layouts";u32 contexts_match=0;while(contexts_check[contexts_match]&&p[contexts_match]==contexts_check[contexts_match])++contexts_match;
    if(!contexts_check[contexts_match]&&!p[contexts_match])return native_font_context_layouts();
    static const wchar_t storage_option[]=L"--script-storage-check";u32 storage_match=0;while(storage_option[storage_match]&&p[storage_match]==storage_option[storage_match])++storage_match;
    if(!storage_option[storage_match]&&!p[storage_match])return native_script_storage_cases();
    CHECK(text_system_initialize(&owner,L"Segoe UI",16).ok());
    Output* outputs[]{&native_output,&reference_output};for(auto* out:outputs){CHECK(shaped_text_initialize(&out->text,{out->glyphs,1024},{out->runs,256},{out->clusters,256},{out->decorations,256},{out->hits,4096}));}
    static const wchar_t matrix_batch_option[]=L"--matrix-batch";
    u32 matrix_batch_match = 0;
    while (matrix_batch_option[matrix_batch_match] &&
           p[matrix_batch_match] == matrix_batch_option[matrix_batch_match])
        ++matrix_batch_match;
    if (!matrix_batch_option[matrix_batch_match] &&
        (p[matrix_batch_match] == L' ' || p[matrix_batch_match] == L'\t'))
        return matrix_batch(p + matrix_batch_match);
    static const wchar_t matrix_profile_option[]=L"--matrix-profile";
    u32 matrix_profile_match = 0;
    while (matrix_profile_option[matrix_profile_match] &&
           p[matrix_profile_match] == matrix_profile_option[matrix_profile_match])
        ++matrix_profile_match;
    if (!matrix_profile_option[matrix_profile_match] &&
        (p[matrix_profile_match] == L' ' || p[matrix_profile_match] == L'\t'))
        return matrix_batch(p + matrix_profile_match, true);
    static const wchar_t alphabetic_paragraph_option[]=L"--alphabetic-paragraph";u32 alphabetic_paragraph_match=0;while(alphabetic_paragraph_option[alphabetic_paragraph_match]&&p[alphabetic_paragraph_match]==alphabetic_paragraph_option[alphabetic_paragraph_match])++alphabetic_paragraph_match;
    if(!alphabetic_paragraph_option[alphabetic_paragraph_match]&&!p[alphabetic_paragraph_match])return native_alphabetic_paragraph_corpus();
    static const wchar_t han_option[]=L"--han-paragraph";u32 han_match=0;while(han_option[han_match]&&p[han_match]==han_option[han_match])++han_match;
    if(!han_option[han_match]&&!p[han_match])return native_han_paragraph_corpus();
    static const wchar_t thai_context_option[]=L"--thai-context-check";u32 thai_context_match=0;while(thai_context_option[thai_context_match]&&p[thai_context_match]==thai_context_option[thai_context_match])++thai_context_match;
    if(!thai_context_option[thai_context_match]&&!p[thai_context_match])return native_thai_context_cases();
    static const wchar_t deva_bench_option[]=L"--devanagari-bench";u32 deva_bench_match=0;while(deva_bench_option[deva_bench_match]&&p[deva_bench_match]==deva_bench_option[deva_bench_match])++deva_bench_match;
    if(!deva_bench_option[deva_bench_match]&&!p[deva_bench_match])return native_devanagari_memory_benchmark();
    static const wchar_t deva_memory_option[]=L"--devanagari-memory-check";u32 deva_memory_match=0;while(deva_memory_option[deva_memory_match]&&p[deva_memory_match]==deva_memory_option[deva_memory_match])++deva_memory_match;
    if(!deva_memory_option[deva_memory_match]&&!p[deva_memory_match])return native_devanagari_memory_benchmark(true);
    static const wchar_t long_editor_option[]=L"--long-editor-check";u32 long_editor_match=0;while(long_editor_option[long_editor_match]&&p[long_editor_match]==long_editor_option[long_editor_match])++long_editor_match;
    static const wchar_t retained_option[]=L"--retained-editor-check";u32 retained_match=0;while(retained_option[retained_match]&&p[retained_match]==retained_option[retained_match])++retained_match;
    if(!retained_option[retained_match]&&p[retained_match]==' '&&p[retained_match+1]>=L'0'&&p[retained_match+1]<=L'5'&&!p[retained_match+2])return retained_editor_capacity(p[retained_match+1]-L'0');
    if(!long_editor_option[long_editor_match]&&!p[long_editor_match])return native_long_text_cases(true);
    static const wchar_t long_option[]=L"--long-text-check";u32 long_match=0;while(long_option[long_match]&&p[long_match]==long_option[long_match])++long_match;
    if(!long_option[long_match]&&!p[long_match])return native_long_text_cases();
    static const wchar_t replay_option[]=L"--font-replay-check";u32 replay_match=0;while(replay_option[replay_match]&&p[replay_match]==replay_option[replay_match])++replay_match;
    if(!replay_option[replay_match]&&!p[replay_match])return native_font_range_replay_cases();
    static const wchar_t hangul_option[]=L"--hangul-profile";u32 hangul_match=0;while(hangul_option[hangul_match]&&p[hangul_match]==hangul_option[hangul_match])++hangul_match;
    if(hangul_match==sizeof(hangul_option)/sizeof(wchar_t)-1&&p[hangul_match]==' '){hangul_profiles=true;p+=hangul_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t thai_shape_option[]=L"--thai-shape-profile";u32 thai_shape_match=0;while(thai_shape_option[thai_shape_match]&&p[thai_shape_match]==thai_shape_option[thai_shape_match])++thai_shape_match;
    if(thai_shape_match==sizeof(thai_shape_option)/sizeof(wchar_t)-1&&p[thai_shape_match]==' '){thai_profiles=thai_shape_only=true;p+=thai_shape_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t thai_option[]=L"--thai-profile";u32 thai_match=0;while(thai_option[thai_match]&&p[thai_match]==thai_option[thai_match])++thai_match;
    if(thai_match==sizeof(thai_option)/sizeof(wchar_t)-1&&p[thai_match]==' '){thai_profiles=true;p+=thai_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t extended_option[]=L"--extended-standard-profile";u32 extended_match=0;while(extended_option[extended_match]&&p[extended_match]==extended_option[extended_match])++extended_match;
    if(extended_match==sizeof(extended_option)/sizeof(wchar_t)-1&&p[extended_match]==' '){standard_script_profiles=extended_standard_profiles=true;p+=extended_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t standard_option[]=L"--standard-script-profile";u32 standard_match=0;while(standard_option[standard_match]&&p[standard_match]==standard_option[standard_match])++standard_match;
    if(standard_match==sizeof(standard_option)/sizeof(wchar_t)-1&&p[standard_match]==' '){standard_script_profiles=true;p+=standard_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t tamil_shape_option[]=L"--tamil-direction-shape-profile";u32 tamil_shape_match=0;while(tamil_shape_option[tamil_shape_match]&&p[tamil_shape_match]==tamil_shape_option[tamil_shape_match])++tamil_shape_match;
    if(!tamil_shape_option[tamil_shape_match]&&p[tamil_shape_match]==' '){devanagari_profiles=tamil_profiles=devanagari_directional=devanagari_tracking=tamil_shape_only=true;p+=tamil_shape_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t tamil_direction_option[]=L"--tamil-direction-profile";u32 tamil_direction_match=0;while(tamil_direction_option[tamil_direction_match]&&p[tamil_direction_match]==tamil_direction_option[tamil_direction_match])++tamil_direction_match;
    if(!tamil_direction_option[tamil_direction_match]&&p[tamil_direction_match]==' '){devanagari_profiles=tamil_profiles=devanagari_directional=devanagari_tracking=true;p+=tamil_direction_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t telugu_option[]=L"--telugu-profile";u32 telugu_match=0;while(telugu_option[telugu_match]&&p[telugu_match]==telugu_option[telugu_match])++telugu_match;
    if(!telugu_option[telugu_match]&&p[telugu_match]==' '){devanagari_profiles=telugu_profiles=true;p+=telugu_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t telugu_tracking_option[]=L"--telugu-tracking-profile";u32 telugu_tracking_match=0;while(telugu_tracking_option[telugu_tracking_match]&&p[telugu_tracking_match]==telugu_tracking_option[telugu_tracking_match])++telugu_tracking_match;
    if(!telugu_tracking_option[telugu_tracking_match]&&p[telugu_tracking_match]==' '){devanagari_profiles=telugu_profiles=devanagari_tracking=true;p+=telugu_tracking_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t kannada_option[]=L"--kannada-profile";u32 kannada_match=0;while(kannada_option[kannada_match]&&p[kannada_match]==kannada_option[kannada_match])++kannada_match;
    if(!kannada_option[kannada_match]&&p[kannada_match]==' '){devanagari_profiles=kannada_profiles=true;p+=kannada_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t kannada_tracking_option[]=L"--kannada-tracking-profile";u32 kannada_tracking_match=0;while(kannada_tracking_option[kannada_tracking_match]&&p[kannada_tracking_match]==kannada_tracking_option[kannada_tracking_match])++kannada_tracking_match;
    if(!kannada_tracking_option[kannada_tracking_match]&&p[kannada_tracking_match]==' '){devanagari_profiles=kannada_profiles=devanagari_tracking=true;p+=kannada_tracking_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t khmer_option[]=L"--khmer-profile";u32 khmer_match=0;while(khmer_option[khmer_match]&&p[khmer_match]==khmer_option[khmer_match])++khmer_match;
    if(!khmer_option[khmer_match]&&p[khmer_match]==' '){devanagari_profiles=khmer_profiles=true;p+=khmer_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t khmer_tracking_option[]=L"--khmer-tracking-profile";u32 khmer_tracking_match=0;while(khmer_tracking_option[khmer_tracking_match]&&p[khmer_tracking_match]==khmer_tracking_option[khmer_tracking_match])++khmer_tracking_match;
    if(!khmer_tracking_option[khmer_tracking_match]&&p[khmer_tracking_match]==' '){devanagari_profiles=khmer_profiles=devanagari_tracking=true;p+=khmer_tracking_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t myanmar_option[]=L"--myanmar-profile";u32 myanmar_match=0;while(myanmar_option[myanmar_match]&&p[myanmar_match]==myanmar_option[myanmar_match])++myanmar_match;
    if(!myanmar_option[myanmar_match]&&p[myanmar_match]==' '){devanagari_profiles=myanmar_profiles=true;p+=myanmar_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t myanmar_tracking_option[]=L"--myanmar-tracking-profile";u32 myanmar_tracking_match=0;while(myanmar_tracking_option[myanmar_tracking_match]&&p[myanmar_tracking_match]==myanmar_tracking_option[myanmar_tracking_match])++myanmar_tracking_match;
    if(!myanmar_tracking_option[myanmar_tracking_match]&&p[myanmar_tracking_match]==' '){devanagari_profiles=myanmar_profiles=devanagari_tracking=true;p+=myanmar_tracking_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t tibetan_option[]=L"--tibetan-profile";u32 tibetan_match=0;while(tibetan_option[tibetan_match]&&p[tibetan_match]==tibetan_option[tibetan_match])++tibetan_match;
    if(!tibetan_option[tibetan_match]&&p[tibetan_match]==' '){devanagari_profiles=tibetan_profiles=true;p+=tibetan_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t tibetan_tracking_option[]=L"--tibetan-tracking-profile";u32 tibetan_tracking_match=0;while(tibetan_tracking_option[tibetan_tracking_match]&&p[tibetan_tracking_match]==tibetan_tracking_option[tibetan_tracking_match])++tibetan_tracking_match;
    if(!tibetan_tracking_option[tibetan_tracking_match]&&p[tibetan_tracking_match]==' '){devanagari_profiles=tibetan_profiles=devanagari_tracking=true;p+=tibetan_tracking_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t sinhala_option[]=L"--sinhala-profile";u32 sinhala_match=0;while(sinhala_option[sinhala_match]&&p[sinhala_match]==sinhala_option[sinhala_match])++sinhala_match;
    if(!sinhala_option[sinhala_match]&&p[sinhala_match]==' '){devanagari_profiles=sinhala_profiles=true;p+=sinhala_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t sinhala_tracking_option[]=L"--sinhala-tracking-profile";u32 sinhala_tracking_match=0;while(sinhala_tracking_option[sinhala_tracking_match]&&p[sinhala_tracking_match]==sinhala_tracking_option[sinhala_tracking_match])++sinhala_tracking_match;
    if(!sinhala_tracking_option[sinhala_tracking_match]&&p[sinhala_tracking_match]==' '){devanagari_profiles=sinhala_profiles=devanagari_tracking=true;p+=sinhala_tracking_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t malayalam_option[]=L"--malayalam-profile";u32 malayalam_match=0;while(malayalam_option[malayalam_match]&&p[malayalam_match]==malayalam_option[malayalam_match])++malayalam_match;
    if(!malayalam_option[malayalam_match]&&p[malayalam_match]==' '){devanagari_profiles=malayalam_profiles=true;p+=malayalam_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t malayalam_tracking_option[]=L"--malayalam-tracking-profile";u32 malayalam_tracking_match=0;while(malayalam_tracking_option[malayalam_tracking_match]&&p[malayalam_tracking_match]==malayalam_tracking_option[malayalam_tracking_match])++malayalam_tracking_match;
    if(!malayalam_tracking_option[malayalam_tracking_match]&&p[malayalam_tracking_match]==' '){devanagari_profiles=malayalam_profiles=devanagari_tracking=true;p+=malayalam_tracking_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t odia_option[]=L"--odia-profile";u32 odia_match=0;while(odia_option[odia_match]&&p[odia_match]==odia_option[odia_match])++odia_match;
    if(!odia_option[odia_match]&&p[odia_match]==' '){devanagari_profiles=odia_profiles=true;p+=odia_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t odia_tracking_option[]=L"--odia-tracking-profile";u32 odia_tracking_match=0;while(odia_tracking_option[odia_tracking_match]&&p[odia_tracking_match]==odia_tracking_option[odia_tracking_match])++odia_tracking_match;
    if(!odia_tracking_option[odia_tracking_match]&&p[odia_tracking_match]==' '){devanagari_profiles=odia_profiles=devanagari_tracking=true;p+=odia_tracking_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t gurmukhi_option[]=L"--gurmukhi-profile";u32 gurmukhi_match=0;while(gurmukhi_option[gurmukhi_match]&&p[gurmukhi_match]==gurmukhi_option[gurmukhi_match])++gurmukhi_match;
    if(!gurmukhi_option[gurmukhi_match]&&p[gurmukhi_match]==' '){devanagari_profiles=gurmukhi_profiles=true;p+=gurmukhi_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t gurmukhi_tracking_option[]=L"--gurmukhi-tracking-profile";u32 gurmukhi_tracking_match=0;while(gurmukhi_tracking_option[gurmukhi_tracking_match]&&p[gurmukhi_tracking_match]==gurmukhi_tracking_option[gurmukhi_tracking_match])++gurmukhi_tracking_match;
    if(!gurmukhi_tracking_option[gurmukhi_tracking_match]&&p[gurmukhi_tracking_match]==' '){devanagari_profiles=gurmukhi_profiles=devanagari_tracking=true;p+=gurmukhi_tracking_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t gujarati_option[]=L"--gujarati-profile";u32 gujarati_match=0;while(gujarati_option[gujarati_match]&&p[gujarati_match]==gujarati_option[gujarati_match])++gujarati_match;
    if(!gujarati_option[gujarati_match]&&p[gujarati_match]==' '){devanagari_profiles=gujarati_profiles=true;p+=gujarati_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t gujarati_tracking_option[]=L"--gujarati-tracking-profile";u32 gujarati_tracking_match=0;while(gujarati_tracking_option[gujarati_tracking_match]&&p[gujarati_tracking_match]==gujarati_tracking_option[gujarati_tracking_match])++gujarati_tracking_match;
    if(!gujarati_tracking_option[gujarati_tracking_match]&&p[gujarati_tracking_match]==' '){devanagari_profiles=gujarati_profiles=devanagari_tracking=true;p+=gujarati_tracking_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t bengali_option[]=L"--bengali-profile";u32 bengali_match=0;while(bengali_option[bengali_match]&&p[bengali_match]==bengali_option[bengali_match])++bengali_match;
    if(!bengali_option[bengali_match]&&p[bengali_match]==' '){devanagari_profiles=bengali_profiles=true;p+=bengali_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t bengali_tracking_option[]=L"--bengali-tracking-profile";u32 bengali_tracking_match=0;while(bengali_tracking_option[bengali_tracking_match]&&p[bengali_tracking_match]==bengali_tracking_option[bengali_tracking_match])++bengali_tracking_match;
    if(!bengali_tracking_option[bengali_tracking_match]&&p[bengali_tracking_match]==' '){devanagari_profiles=bengali_profiles=devanagari_tracking=true;p+=bengali_tracking_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t tamil_tracking_shape_option[]=L"--tamil-tracking-shape-profile";u32 tamil_tracking_shape_match=0;while(tamil_tracking_shape_option[tamil_tracking_shape_match]&&p[tamil_tracking_shape_match]==tamil_tracking_shape_option[tamil_tracking_shape_match])++tamil_tracking_shape_match;
    if(!tamil_tracking_shape_option[tamil_tracking_shape_match]&&p[tamil_tracking_shape_match]==' '){devanagari_profiles=tamil_profiles=devanagari_tracking=tamil_shape_only=true;p+=tamil_tracking_shape_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t tamil_tracking_option[]=L"--tamil-tracking-profile";u32 tamil_tracking_match=0;while(tamil_tracking_option[tamil_tracking_match]&&p[tamil_tracking_match]==tamil_tracking_option[tamil_tracking_match])++tamil_tracking_match;
    if(!tamil_tracking_option[tamil_tracking_match]&&p[tamil_tracking_match]==' '){devanagari_profiles=tamil_profiles=devanagari_tracking=true;p+=tamil_tracking_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t tamil_option[]=L"--tamil-profile";u32 tamil_match=0;while(tamil_option[tamil_match]&&p[tamil_match]==tamil_option[tamil_match])++tamil_match;
    if(!tamil_option[tamil_match]&&p[tamil_match]==' '){devanagari_profiles=tamil_profiles=true;p+=tamil_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t deva_option[]=L"--devanagari-profile";u32 deva_match=0;while(deva_option[deva_match]&&p[deva_match]==deva_option[deva_match])++deva_match;
    if(deva_match==sizeof(deva_option)/sizeof(wchar_t)-1&&p[deva_match]==' '){devanagari_profiles=true;p+=deva_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t deva_tracking_option[]=L"--devanagari-tracking-profile";u32 deva_tracking_match=0;while(deva_tracking_option[deva_tracking_match]&&p[deva_tracking_match]==deva_tracking_option[deva_tracking_match])++deva_tracking_match;
    if(deva_tracking_match==sizeof(deva_tracking_option)/sizeof(wchar_t)-1&&p[deva_tracking_match]==' '){devanagari_profiles=devanagari_tracking=true;p+=deva_tracking_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t deva_direction_option[]=L"--devanagari-direction-profile";u32 deva_direction_match=0;while(deva_direction_option[deva_direction_match]&&p[deva_direction_match]==deva_direction_option[deva_direction_match])++deva_direction_match;
    if(deva_direction_match==sizeof(deva_direction_option)/sizeof(wchar_t)-1&&p[deva_direction_match]==' '){devanagari_profiles=devanagari_directional=true;p+=deva_direction_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t direction_tracking_option[]=L"--direction-tracking-profile";u32 direction_tracking_match=0;while(direction_tracking_option[direction_tracking_match]&&p[direction_tracking_match]==direction_tracking_option[direction_tracking_match])++direction_tracking_match;
    if(direction_tracking_match==sizeof(direction_tracking_option)/sizeof(wchar_t)-1&&p[direction_tracking_match]==' '){devanagari_profiles=devanagari_directional=devanagari_tracking=true;p+=direction_tracking_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t arabic_context_option[]=L"--arabic-context-profile";u32 arabic_context_match=0;while(arabic_context_option[arabic_context_match]&&p[arabic_context_match]==arabic_context_option[arabic_context_match])++arabic_context_match;
    if(arabic_context_match==sizeof(arabic_context_option)/sizeof(wchar_t)-1&&p[arabic_context_match]==' '){arabic_context_profiles=devanagari_profiles=devanagari_directional=devanagari_tracking=true;p+=arabic_context_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t quick_option[]=L"--quick";u32 quick_match=0;while(quick_option[quick_match]&&p[quick_match]==quick_option[quick_match])++quick_match;
    if(quick_match==7&&p[7]==' '){multiline_quick=true;p+=7;while(*p==' '||*p=='\t')++p;
        const wchar_t* prefixes[]{L"--multiline-",L"--bidi-emoji-profile",L"--mixed-emoji"};bool supported=false;
        for(u32 i=0;i<3;++i){u32 matched=0;while(prefixes[i][matched]&&p[matched]==prefixes[i][matched])++matched;
            supported|=!prefixes[i][matched]&&(!i||!p[matched]||p[matched]==' ');}CHECK(supported);
        print("quick sampled coverage; exhaustive combinations remain available without --quick\n");}
    static const wchar_t editor_policy_option[]=L"--editor-policy-probe";u32 editor_match=0;while(editor_policy_option[editor_match]&&p[editor_match]==editor_policy_option[editor_match])++editor_match;
    static const wchar_t mixed_gates_option[]=L"--mixed-bidi-gates";u32 mixed_gates_match=0;while(mixed_gates_option[mixed_gates_match]&&p[mixed_gates_match]==mixed_gates_option[mixed_gates_match])++mixed_gates_match;
    static const wchar_t bidi_range_check[]=L"--bidi-range-check";u32 bidi_range_match=0;while(bidi_range_check[bidi_range_match]&&p[bidi_range_match]==bidi_range_check[bidi_range_match])++bidi_range_match;
    if(bidi_range_match+1==sizeof(bidi_range_check)/sizeof(wchar_t)&&!p[bidi_range_match])return native_bidi_style_range_probe(true);
    if(mixed_gates_match+1==sizeof(mixed_gates_option)/sizeof(wchar_t)&&!p[mixed_gates_match])return native_mixed_bidi_gates();
    if(editor_match==21&&!p[21])return native_editor_policy_probe();
    static const wchar_t simulated_option[]=L"--simulated-styles";u32 simulated_match=0;while(simulated_option[simulated_match]&&p[simulated_match]==simulated_option[simulated_match])++simulated_match;
    if(simulated_match==18&&!p[18])return native_simulated_style_corpus();
    static const wchar_t simulation_profile[]=L"--simulated-profile";simulated_match=0;while(simulation_profile[simulated_match]&&p[simulated_match]==simulation_profile[simulated_match])++simulated_match;
    if(simulated_match==19&&p[19]==' '){simulated_profiles=true;p+=19;while(*p==' '||*p=='\t')++p;}
    static const wchar_t system_profile[]=L"--system-fallback-profile";u32 system_match=0;while(system_profile[system_match]&&p[system_match]==system_profile[system_match])++system_match;
    static const wchar_t hebrew_profile[]=L"--hebrew-profile";u32 hebrew_match=0;while(hebrew_profile[hebrew_match]&&p[hebrew_match]==hebrew_profile[hebrew_match])++hebrew_match;
    static const wchar_t mixed_bidi_profile[]=L"--mixed-bidi-profile";u32 mixed_bidi_match=0;while(mixed_bidi_profile[mixed_bidi_match]&&p[mixed_bidi_match]==mixed_bidi_profile[mixed_bidi_match])++mixed_bidi_match;
    static const wchar_t symbol_option[]=L"--symbols-profile";u32 symbol_match=0;while(symbol_option[symbol_match]&&p[symbol_match]==symbol_option[symbol_match])++symbol_match;
    if(symbol_match+1==sizeof(symbol_option)/sizeof(wchar_t)&&p[symbol_match]==' '){bidi_styles_profiles=symbol_profiles=true;p+=symbol_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t bidi_emoji_option[]=L"--bidi-emoji-profile";u32 bidi_emoji_match=0;while(bidi_emoji_option[bidi_emoji_match]&&p[bidi_emoji_match]==bidi_emoji_option[bidi_emoji_match])++bidi_emoji_match;
    if(bidi_emoji_match+1==sizeof(bidi_emoji_option)/sizeof(wchar_t)&&p[bidi_emoji_match]==' '){bidi_styles_profiles=bidi_emoji_profiles=true;p+=bidi_emoji_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t bidi_styles_option[]=L"--bidi-styles-profile";u32 bidi_styles_match=0;while(bidi_styles_option[bidi_styles_match]&&p[bidi_styles_match]==bidi_styles_option[bidi_styles_match])++bidi_styles_match;
    if(bidi_styles_match+1==sizeof(bidi_styles_option)/sizeof(wchar_t)&&p[bidi_styles_match]==' '){bidi_styles_profiles=true;p+=bidi_styles_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t mixed_bidi_wrap_option[]=L"--mixed-bidi-wrap-profile";u32 mixed_bidi_wrap_match=0;while(mixed_bidi_wrap_option[mixed_bidi_wrap_match]&&p[mixed_bidi_wrap_match]==mixed_bidi_wrap_option[mixed_bidi_wrap_match])++mixed_bidi_wrap_match;
    if(mixed_bidi_wrap_match+1==sizeof(mixed_bidi_wrap_option)/sizeof(wchar_t)&&p[mixed_bidi_wrap_match]==' '){mixed_bidi_profiles=mixed_bidi_wrap=true;p+=mixed_bidi_wrap_match;while(*p==' '||*p=='\t')++p;}
    if(mixed_bidi_match+1==sizeof(mixed_bidi_profile)/sizeof(wchar_t)&&p[mixed_bidi_match]==' '){mixed_bidi_profiles=true;p+=mixed_bidi_match;while(*p==' '||*p=='\t')++p;}
    static const wchar_t hebrew_wrap_profile[]=L"--hebrew-wrap-profile";u32 hebrew_wrap_match=0;while(hebrew_wrap_profile[hebrew_wrap_match]&&p[hebrew_wrap_match]==hebrew_wrap_profile[hebrew_wrap_match])++hebrew_wrap_match;
    if(hebrew_wrap_match+1==sizeof(hebrew_wrap_profile)/sizeof(wchar_t)&&p[hebrew_wrap_match]==' '){hebrew_profiles=hebrew_wrap_profiles=true;p+=hebrew_wrap_match;while(*p==' '||*p=='\t')++p;}
    if(hebrew_match+1==sizeof(hebrew_profile)/sizeof(wchar_t)&&p[hebrew_match]==' '){hebrew_profiles=true;p+=hebrew_match;while(*p==' '||*p=='\t')++p;}
    if(system_match==25&&p[25]==' '){system_fallback_profiles=true;p+=25;while(*p==' '||*p=='\t')++p;}
    static const wchar_t fallback_option[]=L"--fallback-bindings";u32 fallback_match=0;while(fallback_option[fallback_match]&&p[fallback_match]==fallback_option[fallback_match])++fallback_match;
    if(fallback_match==19&&!p[19])return native_fallback_binding_corpus();
    static const wchar_t alphabetic_option[]=L"--alphabetic-profile";u32 alphabetic_match=0;while(alphabetic_option[alphabetic_match]&&p[alphabetic_match]==alphabetic_option[alphabetic_match])++alphabetic_match;
    if(alphabetic_match==20&&p[20]==' '){alphabetic_profiles=true;p+=20;while(*p==' '||*p=='\t')++p;}
    static const wchar_t mixed_option[]=L"--mixed-emoji";u32 mixed_match=0;while(mixed_option[mixed_match]&&p[mixed_match]==mixed_option[mixed_match])++mixed_match;
    if(mixed_match==13&&(!p[13]||p[13]==' ')){p+=13;while(*p==' '||*p=='\t')++p;return *p?native_emoji_dispatch_corpus(p,true):native_mixed_emoji_corpus();}
    static const wchar_t emoji_option[]=L"--emoji-dispatch";u32 emoji_match=0;while(emoji_option[emoji_match]&&p[emoji_match]==emoji_option[emoji_match])++emoji_match;
    if(emoji_match==16&&(!p[16]||p[16]==' ')){p+=16;while(*p==' '||*p=='\t')++p;return native_emoji_dispatch_corpus(p);}
    static const wchar_t tracking_option[]=L"--tracking-profile";u32 tracking_match=0;while(tracking_option[tracking_match]&&p[tracking_match]==tracking_option[tracking_match])++tracking_match;
    if(tracking_match==18&&p[18]==' '){tracking_profiles=true;p+=18;while(*p==' '||*p=='\t')++p;}
    static const wchar_t native_formats[]=L"--native-formats";u32 format_option=0;while(native_formats[format_option]&&p[format_option]==native_formats[format_option])++format_option;if(format_option==16&&!p[16])return native_format_corpus();
    static const wchar_t cluster_cases[]=L"--style-clusters";u32 matched_hits=0;while(cluster_cases[matched_hits]&&p[matched_hits]==cluster_cases[matched_hits])++matched_hits;if(matched_hits==16&&!p[16])return inspect_style_clusters();
    static const wchar_t cluster_profile[]=L"--style-cluster-profile";matched_hits=0;while(cluster_profile[matched_hits]&&p[matched_hits]==cluster_profile[matched_hits])++matched_hits;if(matched_hits==23&&p[23]==' '){style_cluster_profiles=true;p+=23;while(*p==' '||*p=='\t')++p;}
    static const wchar_t continuity_profile[]=L"--continuity-profile";matched_hits=0;while(continuity_profile[matched_hits]&&p[matched_hits]==continuity_profile[matched_hits])++matched_hits;if(matched_hits==20&&p[20]==' '){style_continuity_profiles=true;p+=20;while(*p==' '||*p=='\t')++p;}
    static const wchar_t hit_tests[]=L"--paragraph-hits";matched_hits=0;while(hit_tests[matched_hits]&&p[matched_hits]==hit_tests[matched_hits])++matched_hits;if(matched_hits==16&&!p[16])return paragraph_hits();
    if (lstrcmpW(p,L"--tsf-points")==0) return tsf_point_geometry();
    if (lstrcmpW(p,L"--historic")==0) return historic_cases();
    static const wchar_t edit_tests[]=L"--edit-hits";matched_hits=0;while(edit_tests[matched_hits]&&p[matched_hits]==edit_tests[matched_hits])++matched_hits;if(matched_hits==11&&!p[11])return edit_hits();
    static const wchar_t directional_edit_cuts_option[]=L"--directional-edit-cuts";matched_hits=0;while(directional_edit_cuts_option[matched_hits]&&p[matched_hits]==directional_edit_cuts_option[matched_hits])++matched_hits;if(matched_hits==sizeof(directional_edit_cuts_option)/sizeof(wchar_t)-1&&!p[matched_hits])return directional_edit_cuts();
    static const wchar_t directional_edit_option[]=L"--directional-edit";matched_hits=0;while(directional_edit_option[matched_hits]&&p[matched_hits]==directional_edit_option[matched_hits])++matched_hits;if(matched_hits==sizeof(directional_edit_option)/sizeof(wchar_t)-1&&!p[matched_hits])return directional_edit();
    static const wchar_t styled_edit_option[]=L"--styled-edit";matched_hits=0;while(styled_edit_option[matched_hits]&&p[matched_hits]==styled_edit_option[matched_hits])++matched_hits;if(matched_hits==13&&!p[13])return styled_edit();
    static const wchar_t inspect[]=L"--inspect";u32 inspected=0;while(inspect[inspected]&&p[inspected]==inspect[inspected])++inspected;if(inspected==9&&!p[9])return inspect_paragraphs();
    static const wchar_t paragraphs[]=L"--paragraphs";inspected=0;while(paragraphs[inspected]&&p[inspected]==paragraphs[inspected])++inspected;if(inspected==12&&!p[12])return inspect_paragraphs(true);
    static const wchar_t fuzz[]=L"--paragraph-fuzz";inspected=0;while(fuzz[inspected]&&p[inspected]==fuzz[inspected])++inspected;if(inspected==16&&!p[16])return paragraph_fuzz();
    static const wchar_t options[]=L"--paragraph-options";inspected=0;while(options[inspected]&&p[inspected]==options[inspected])++inspected;if(inspected==19&&!p[19])return paragraph_options();
    static const wchar_t break_option[]=L"--break-tests";inspected=0;while(break_option[inspected]&&p[inspected]==break_option[inspected])++inspected;if(inspected==13&&!p[13])return break_tests();
    static const wchar_t hit_profile[]=L"--hit-profile";inspected=0;while(hit_profile[inspected]&&p[inspected]==hit_profile[inspected])++inspected;if(inspected==13&&p[13]==' '){hit_profiles=true;p+=13;while(*p==' '||*p=='\t')++p;}
    static const wchar_t latin_profile[]=L"--latin-profile";inspected=0;while(latin_profile[inspected]&&p[inspected]==latin_profile[inspected])++inspected;if(inspected==15&&p[15]==' '){latin_profiles=true;p+=15;while(*p==' '||*p=='\t')++p;}
    static const wchar_t latin_paragraph_option[]=L"--latin-paragraph";inspected=0;while(latin_paragraph_option[inspected]&&p[inspected]==latin_paragraph_option[inspected])++inspected;if(inspected==17&&p[17]==' '){latin_paragraph_profiles=true;p+=17;while(*p==' '||*p=='\t')++p;}
    static const wchar_t latin_hits_option[]=L"--latin-hits";inspected=0;while(latin_hits_option[inspected]&&p[inspected]==latin_hits_option[inspected])++inspected;if(inspected==12&&p[12]==' '){latin_paragraph_hits=true;p+=12;while(*p==' '||*p=='\t')++p;}
    static const wchar_t latin_fuzz_option[]=L"--latin-paragraph-fuzz";inspected=0;while(latin_fuzz_option[inspected]&&p[inspected]==latin_fuzz_option[inspected])++inspected;if(inspected==22&&!p[22])return latin_paragraph_fuzz();
    static const wchar_t directional_semantic_option[]=L"--directional-semantic-profile";inspected=0;while(directional_semantic_option[inspected]&&p[inspected]==directional_semantic_option[inspected])++inspected;
    if(inspected+1==sizeof(directional_semantic_option)/sizeof(wchar_t)&&p[inspected]==' '){directional_semantic_profiles=true;p+=inspected;while(*p==' '||*p=='\t')++p;}
    static const wchar_t large_range_option[]=L"--directional-large-profile";inspected=0;while(large_range_option[inspected]&&p[inspected]==large_range_option[inspected])++inspected;if(inspected==sizeof(large_range_option)/sizeof(wchar_t)-1&&p[inspected]==' '){directional_large_profiles=true;p+=inspected;while(*p==' '||*p=='\t')++p;}
    static const wchar_t directional_editor_cuts_option[]=L"--directional-editor-cuts-profile";inspected=0;while(directional_editor_cuts_option[inspected]&&p[inspected]==directional_editor_cuts_option[inspected])++inspected;if(inspected==sizeof(directional_editor_cuts_option)/sizeof(wchar_t)-1&&p[inspected]==' '){directional_editor_cuts_profiles=true;p+=inspected;while(*p==' '||*p=='\t')++p;}
    static const wchar_t multiline_hit_option[]=L"--multiline-hit-profile";inspected=0;while(multiline_hit_option[inspected]&&p[inspected]==multiline_hit_option[inspected])++inspected;if(inspected==sizeof(multiline_hit_option)/sizeof(wchar_t)-1&&p[inspected]==' '){multiline_trim_profiles=multiline_hit_profiles=true;p+=inspected;while(*p==' '||*p=='\t')++p;}
    static const wchar_t multiline_plan_option[]=L"--multiline-plan-profile";inspected=0;while(multiline_plan_option[inspected]&&p[inspected]==multiline_plan_option[inspected])++inspected;if(inspected==sizeof(multiline_plan_option)/sizeof(wchar_t)-1&&p[inspected]==' '){multiline_trim_profiles=multiline_plan_profiles=true;p+=inspected;while(*p==' '||*p=='\t')++p;}
    static const wchar_t multiline_trim_option[]=L"--multiline-trim-profile";inspected=0;while(multiline_trim_option[inspected]&&p[inspected]==multiline_trim_option[inspected])++inspected;if(inspected==sizeof(multiline_trim_option)/sizeof(wchar_t)-1&&p[inspected]==' '){multiline_trim_profiles=true;p+=inspected;while(*p==' '||*p=='\t')++p;}
    static const wchar_t ellipsis_line_option[]=L"--ellipsis-line-profile";inspected=0;while(ellipsis_line_option[inspected]&&p[inspected]==ellipsis_line_option[inspected])++inspected;if(inspected==sizeof(ellipsis_line_option)/sizeof(wchar_t)-1&&p[inspected]==' '){ellipsis_line_profiles=true;p+=inspected;while(*p==' '||*p=='\t')++p;}
    static const wchar_t ellipsis_style_option[]=L"--ellipsis-style-profile";inspected=0;while(ellipsis_style_option[inspected]&&p[inspected]==ellipsis_style_option[inspected])++inspected;if(inspected==sizeof(ellipsis_style_option)/sizeof(wchar_t)-1&&p[inspected]==' '){ellipsis_style_profiles=true;p+=inspected;while(*p==' '||*p=='\t')++p;}
    static const wchar_t ellipsis_hit_option[]=L"--ellipsis-hit-profile";inspected=0;while(ellipsis_hit_option[inspected]&&p[inspected]==ellipsis_hit_option[inspected])++inspected;if(inspected==sizeof(ellipsis_hit_option)/sizeof(wchar_t)-1&&p[inspected]==' '){ellipsis_hit_profiles=true;p+=inspected;while(*p==' '||*p=='\t')++p;}
    static const wchar_t ellipsis_cut_option[]=L"--ellipsis-cut-profile";inspected=0;while(ellipsis_cut_option[inspected]&&p[inspected]==ellipsis_cut_option[inspected])++inspected;if(inspected==sizeof(ellipsis_cut_option)/sizeof(wchar_t)-1&&p[inspected]==' '){ellipsis_cut_profiles=true;p+=inspected;while(*p==' '||*p=='\t')++p;}
    static const wchar_t common_punctuation_option[]=L"--common-punctuation-profile";inspected=0;while(common_punctuation_option[inspected]&&p[inspected]==common_punctuation_option[inspected])++inspected;if(inspected==sizeof(common_punctuation_option)/sizeof(wchar_t)-1&&p[inspected]==' '){common_punctuation_profiles=true;p+=inspected;while(*p==' '||*p=='\t')++p;}
    static const wchar_t ellipsis_sign_option[]=L"--ellipsis-sign-profile";inspected=0;while(ellipsis_sign_option[inspected]&&p[inspected]==ellipsis_sign_option[inspected])++inspected;if(inspected==sizeof(ellipsis_sign_option)/sizeof(wchar_t)-1&&p[inspected]==' '){ellipsis_sign_profiles=true;p+=inspected;while(*p==' '||*p=='\t')++p;}
    static const wchar_t height_overflow_option[]=L"--height-overflow-profile";inspected=0;while(height_overflow_option[inspected]&&p[inspected]==height_overflow_option[inspected])++inspected;if(inspected==sizeof(height_overflow_option)/sizeof(wchar_t)-1&&p[inspected]==' '){height_overflow_profiles=true;p+=inspected;while(*p==' '||*p=='\t')++p;}
    static const wchar_t cjk_paragraph_option[]=L"--cjk-paragraph-profile";inspected=0;while(cjk_paragraph_option[inspected]&&p[inspected]==cjk_paragraph_option[inspected])++inspected;if(inspected==sizeof(cjk_paragraph_option)/sizeof(wchar_t)-1&&p[inspected]==' '){cjk_paragraph_profiles=true;p+=inspected;while(*p==' '||*p=='\t')++p;}
    static const wchar_t cjk_editor_option[]=L"--cjk-editor-profile";inspected=0;while(cjk_editor_option[inspected]&&p[inspected]==cjk_editor_option[inspected])++inspected;if(inspected==sizeof(cjk_editor_option)/sizeof(wchar_t)-1&&p[inspected]==' '){cjk_editor_profiles=true;p+=inspected;while(*p==' '||*p=='\t')++p;}
    static const wchar_t cjk_editor_paint_option[]=L"--cjk-editor-paint-profile";inspected=0;while(cjk_editor_paint_option[inspected]&&p[inspected]==cjk_editor_paint_option[inspected])++inspected;if(inspected==sizeof(cjk_editor_paint_option)/sizeof(wchar_t)-1&&p[inspected]==' '){cjk_editor_paint_profiles=true;p+=inspected;while(*p==' '||*p=='\t')++p;}
    static const wchar_t arabic_editor_option[]=L"--arabic-editor-profile";inspected=0;while(arabic_editor_option[inspected]&&p[inspected]==arabic_editor_option[inspected])++inspected;if(inspected==sizeof(arabic_editor_option)/sizeof(wchar_t)-1&&p[inspected]==' '){arabic_editor_profiles=true;p+=inspected;while(*p==' '||*p=='\t')++p;}
    static const wchar_t arabic_paragraph_option[]=L"--arabic-paragraph-profile";inspected=0;while(arabic_paragraph_option[inspected]&&p[inspected]==arabic_paragraph_option[inspected])++inspected;if(inspected==sizeof(arabic_paragraph_option)/sizeof(wchar_t)-1&&p[inspected]==' '){arabic_paragraph_profiles=true;p+=inspected;while(*p==' '||*p=='\t')++p;}
    static const wchar_t directional_editor_option[]=L"--directional-editor-profile";inspected=0;while(directional_editor_option[inspected]&&p[inspected]==directional_editor_option[inspected])++inspected;if(inspected==sizeof(directional_editor_option)/sizeof(wchar_t)-1&&p[inspected]==' '){directional_editor_profiles=true;p+=inspected;while(*p==' '||*p=='\t')++p;}
    static const wchar_t semantic_option[]=L"--semantic-profile";inspected=0;while(semantic_option[inspected]&&p[inspected]==semantic_option[inspected])++inspected;if(inspected==18&&p[18]==' '){semantic_profiles=true;p+=18;while(*p==' '||*p=='\t')++p;}
    static const wchar_t styled_hit_option[]=L"--styled-hit-profile";inspected=0;while(styled_hit_option[inspected]&&p[inspected]==styled_hit_option[inspected])++inspected;if(inspected==20&&p[20]==' '){styled_hit_profiles=true;p+=20;while(*p==' '||*p=='\t')++p;}
    static const wchar_t semantic_options_option[]=L"--semantic-options";inspected=0;while(semantic_options_option[inspected]&&p[inspected]==semantic_options_option[inspected])++inspected;if(inspected==18&&!p[18])return semantic_options();
    static const wchar_t style_option[]=L"--style-profile";inspected=0;while(style_option[inspected]&&p[inspected]==style_option[inspected])++inspected;if(inspected==15&&p[15]==' '){style_profiles=true;p+=15;while(*p==' '||*p=='\t')++p;}
    static const wchar_t decoration_option[]=L"--decoration-profile";inspected=0;while(decoration_option[inspected]&&p[inspected]==decoration_option[inspected])++inspected;if(inspected==20&&p[20]==' '){decoration_profiles=true;p+=20;while(*p==' '||*p=='\t')++p;}
    static const wchar_t decoration_options_option[]=L"--decoration-options";inspected=0;while(decoration_options_option[inspected]&&p[inspected]==decoration_options_option[inspected])++inspected;if(inspected==20&&!p[20])return decoration_options();
    static const wchar_t smoke_option[]=L"--smoke ";inspected=0;while(smoke_option[inspected]&&p[inspected]==smoke_option[inspected])++inspected;if(!smoke_option[inspected]){benchmark_smoke=true;p+=inspected;while(*p==' '||*p=='\t')++p;}
    static const wchar_t telugu_bench_option[]=L"--telugu-bench";inspected=0;while(telugu_bench_option[inspected]&&p[inspected]==telugu_bench_option[inspected])++inspected;if(!telugu_bench_option[inspected]&&!p[inspected])return styled_paragraph_benchmark(true,false,false);
    static const wchar_t arabic_bench_option[]=L"--arabic-bench";inspected=0;while(arabic_bench_option[inspected]&&p[inspected]==arabic_bench_option[inspected])++inspected;if(!arabic_bench_option[inspected]&&!p[inspected])return styled_paragraph_benchmark(false,false,true);
    static const wchar_t kannada_bench_option[]=L"--kannada-bench";inspected=0;while(kannada_bench_option[inspected]&&p[inspected]==kannada_bench_option[inspected])++inspected;if(!kannada_bench_option[inspected]&&!p[inspected])return styled_paragraph_benchmark(false,true,false);
    static const wchar_t odia_bench_option[]=L"--odia-bench";inspected=0;while(odia_bench_option[inspected]&&p[inspected]==odia_bench_option[inspected])++inspected;if(inspected==12&&!p[12])return styled_paragraph_benchmark(false,false,false);
    static const wchar_t profile[]=L"--paragraph-profile";inspected=0;while(profile[inspected]&&p[inspected]==profile[inspected])++inspected;if(inspected==19&&(p[19]==0||p[19]==' ')){paragraph_profiles=true;p+=19;while(*p==' '||*p=='\t')++p;}
    static const wchar_t expansion[]=L"--paragraph-expansion";inspected=0;while(expansion[inspected]&&p[inspected]==expansion[inspected])++inspected;if(inspected==21&&p[21]==' '){paragraph_expansion=true;p+=21;while(*p==' '||*p=='\t')++p;}
    static const wchar_t family_option[]=L"--family";u32 matched=0;while(family_option[matched]&&p[matched]==family_option[matched])++matched;
    const bool system_family=matched==8&&(p[8]==0||p[8]==' '||p[8]=='\t');if(system_family){p+=8;while(*p==' '||*p=='\t')++p;}
    if(*p){wchar_t path[32768];u32 n=0;quote=*p=='"';if(quote)++p;while(*p&&!(quote?*p=='"':*p==' '||*p=='\t')){CHECK(n+1<32768);path[n++]=*p++;}path[n]=0;if(quote&&*p=='"')++p;while(*p==' '||*p=='\t')++p;u32 weight=0;while(*p>='0'&&*p<='9'){CHECK(weight<10000);weight=weight*10+*p++-'0';}return system_family?corpus(nullptr,path,weight):custom(path,weight);}
    CHECK(input_validation());
    CHECK(corpus(nullptr,L"Segoe UI",400));
    CHECK(shared_oblique_binding());
    CHECK(retained_run_lifetime());
    CHECK(compare(owner,utf8_literal("AV To office"),false,30));
    CHECK(compare(owner,utf8_literal("AV To office"),true,30,1000000,TextWrapping::no_wrap,TextOverflow::ellipsis));
    CHECK(compare(owner,utf8_literal("a\nb"),false));CHECK(compare(owner,utf8_literal("Hello \xf0\x9f\x98\x80"),false));
    CHECK(compare(owner,utf8_literal("A\r\nB"),false));
    CHECK(reference_output.text.cluster_count==4&&reference_output.text.clusters[1]==0xffff&&reference_output.text.clusters[2]==0xffff);
    CHECK(compare(owner,utf8_literal("A \xf0\x9f\x8f\xb4\xf3\xa0\x81\xa7\xf3\xa0\x81\xa2\xf3\xa0\x81\xa5\xf3\xa0\x81\xae\xf3\xa0\x81\xa7\xf3\xa0\x81\xbf B"),false));
    CHECK(reference_output.text.cluster_count==18&&reference_output.text.glyph_count>0);
    for(u32 i=4;i<16;++i)CHECK(reference_output.text.clusters[i]==0xffff);
    auto tracked=owner;tracked.tracking=.03f;CHECK(compare(tracked,utf8_literal("AV To office"),false));
    CHECK(SUCCEEDED(owner.format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER)));CHECK(compare(owner,utf8_literal("AV To office"),false));
    CHECK(SUCCEEDED(owner.format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING)));CHECK(compare(owner,{},false));
    return true;
}
}}
extern "C" __declspec(noreturn) void WINAPI maicord_native_app_line_entry() noexcept {
    using namespace maicord::native_app_line;const bool ok=run()&&draw_scratch_ready;
    shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);reference_edit_queries_release();draw_bound_release();oracle_faces_release();const bool draw_retained_ok=draw_retained_release();text_system_release(&owner);arena_release(&draw_scratch);
    if(invisible_offset_differences){print("zero_advance_empty_space_offset_diagnostics=");number(invisible_offset_differences);print("\n");}
    if(hit_point_requests){print("hit_point_requests=");number(hit_point_requests);print(" hit_point_reused=");number(hit_point_reused);print(" repeated_caret_boxes=");number(repeated_hit_boxes);print("\n");}
    reference_edit_queries_report();
    oracle_draw_report();
    if(repeated_matrix_cases){print("repeated_matrix_cases=");number(repeated_matrix_cases);print(" (identical native/oracle requests counted, executed once)\n");}
    if(reference_measure_controls||reference_measure_reuses){print("reference_measure_controls=");number(reference_measure_controls);print(" reference_measure_shape_reuses=");number(reference_measure_reuses);print("\n");}
    if(draw_scratch_uses){print("draw_scratch_uses=");number(draw_scratch_uses);print(" draw_scratch_reservations=");number(draw_scratch_reservations);print(" leases_released=");number(draw_scratch_ready);print(" retained_instance_reuses=");number(draw_retained_reuses);print(" bind_controls=");number(draw_bind_controls);print(" bind_reuses=");number(draw_bind_reuses);print("\n");}
    print("cases=");number(cases);print(" native_cases=");number(native_cases);print(" simulated_cases=");number(simulated_cases);print(" checks=");number(checks);print(ok?" PASS native app lines\n":" FAIL native app lines\n");ExitProcess(ok&&draw_retained_ok&&print_ok?0:1);
}
