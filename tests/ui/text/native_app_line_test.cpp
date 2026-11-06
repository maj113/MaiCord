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
    CHECK(text_system_initialize(&owner,L"Segoe UI",16).ok());
    Output* outputs[]{&native_output,&reference_output};for(auto* out:outputs){CHECK(shaped_text_initialize(&out->text,{out->glyphs,1024},{out->runs,256},{out->clusters,256},{out->decorations,256},{out->hits,4096}));}
    if(const wchar_t* rest=after_option(p,L"--matrix-batch"))return matrix_batch(rest);
    if(const wchar_t* rest=after_option(p,L"--matrix-profile"))return matrix_batch(rest,true);
    if(is_option(p,L"--alphabetic-paragraph"))return native_alphabetic_paragraph_corpus();
    if(is_option(p,L"--han-paragraph"))return native_han_paragraph_corpus();
    if(is_option(p,L"--thai-context-check"))return native_thai_context_cases();
    if(is_option(p,L"--devanagari-bench"))return native_devanagari_memory_benchmark();
    if(is_option(p,L"--devanagari-memory-check"))return native_devanagari_memory_benchmark(true);
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
    if(is_option(p,L"--editor-policy-probe"))return native_editor_policy_probe();
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
    if(is_option(p,L"--inspect"))return inspect_paragraphs();
    if(is_option(p,L"--paragraphs"))return inspect_paragraphs(true);
    if(is_option(p,L"--paragraph-fuzz"))return paragraph_fuzz();
    if(is_option(p,L"--paragraph-options"))return paragraph_options();
    if(is_option(p,L"--break-tests"))return break_tests();
    profile(p,L"--hit-profile",hit_profiles);
    profile(p,L"--latin-profile",latin_profiles);
    profile(p,L"--latin-paragraph",latin_paragraph_profiles);
    profile(p,L"--latin-hits",latin_paragraph_hits);
    if(is_option(p,L"--latin-paragraph-fuzz"))return latin_paragraph_fuzz();
    profile(p,L"--directional-semantic-profile",directional_semantic_profiles);
    profile(p,L"--directional-large-profile",directional_large_profiles);
    profile(p,L"--directional-editor-cuts-profile",directional_editor_cuts_profiles);
    profile(p,L"--multiline-hit-profile",multiline_trim_profiles,multiline_hit_profiles);
    profile(p,L"--multiline-plan-profile",multiline_trim_profiles,multiline_plan_profiles);
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
    profile(p,L"--smoke",benchmark_smoke);
    if(is_option(p,L"--telugu-bench"))return styled_paragraph_benchmark(true,false,false);
    if(is_option(p,L"--arabic-bench"))return styled_paragraph_benchmark(false,false,true);
    if(is_option(p,L"--kannada-bench"))return styled_paragraph_benchmark(false,true,false);
    if(is_option(p,L"--odia-bench"))return styled_paragraph_benchmark(false,false,false);
    if(take(p,L"--paragraph-profile",true))paragraph_profiles=true;
    profile(p,L"--paragraph-expansion",paragraph_expansion);
    const bool system_family=take(p,L"--family",true);
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
