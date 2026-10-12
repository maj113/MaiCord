struct FreshEngineCase {const char* name;Utf8View text;const wchar_t* family;u8 mode;};
const FreshEngineCase fresh_engine_cases[]{
    {"ascii",utf8_literal("AV office ordinary words and numbers 0123456789."),L"Segoe UI",0},
    {"latin-combining",utf8_literal("Café naïve é Å office ffi AV To."),L"Segoe UI",0},
    {"greek",utf8_literal("Ελληνικά κείμενο άλφα βήτα κόσμος."),L"Segoe UI",0},
    {"cyrillic",utf8_literal("Кириллица русский українська текст."),L"Segoe UI",0},
    {"hebrew",utf8_literal("שלום עולם אבג 123 (abc) שָׁלוֹם"),L"Segoe UI",0},
    {"arabic",utf8_literal("العربية السلام عليكم 123 لا مُحَمَّد"),L"Segoe UI",0},
    {"bidi-mixed",utf8_literal("AV אבג (123) العربية café 😀 xyz"),L"Segoe UI",0},
    {"bidi-isolates",utf8_literal("A ⁧אבג 123⁩ B ⁦abc⁩ العربية"),L"Segoe UI",0},
    {"hiragana",utf8_literal("ひらがな にほんご あいうえお がぎぐげご"),L"Yu Gothic UI",0},
    {"katakana",utf8_literal("カタカナ 日本語 アイウエオ ガギグゲゴ ｶﾀｶﾅ"),L"Yu Gothic UI",0},
    {"japanese-mixed",utf8_literal("日本語 ひらがな カタカナ AV 123。句読点！"),L"Yu Gothic UI",0},
    {"han-simplified",utf8_literal("简体中文文本测试，换行与标点。你好世界！"),L"Microsoft YaHei UI",0},
    {"han-traditional",utf8_literal("繁體中文文字測試，換行與標點。你好世界！"),L"Microsoft JhengHei UI",0},
    {"hangul",utf8_literal("한국어 한글 테스트 가나다 한글"),L"Malgun Gothic",0},
    {"thai",utf8_literal("ภาษาไทย สวัสดีครับ ข้อความทดสอบ"),L"Leelawadee UI",0},
    {"lao",utf8_literal("ພາສາລາວ ສະບາຍດີ ຂໍ້ຄວາມ"),L"Leelawadee UI",0},
    {"devanagari",utf8_literal("हिन्दी क्षेत्र नमस्ते र्कि क्ष श्रद्धा"),L"Nirmala UI",0},
    {"tamil",utf8_literal("தமிழ் மொழி வணக்கம் கி கொ கௌ ஸ்ரீ"),L"Nirmala UI",0},
    {"bengali",utf8_literal("বাংলা ভাষা নমস্কার কি কো কৌ শ্রদ্ধা"),L"Nirmala UI",0},
    {"gujarati",utf8_literal("ગુજરાતી ભાષા નમસ્તે કિ કો કૌ શ્રદ્ધા"),L"Nirmala UI",0},
    {"gurmukhi",utf8_literal("ਪੰਜਾਬੀ ਭਾਸ਼ਾ ਸਤਿ ਸ੍ਰੀ ਅਕਾਲ ਕਿ ਕੌ"),L"Nirmala UI",0},
    {"odia",utf8_literal("ଓଡ଼ିଆ ଭାଷା ନମସ୍କାର କି କୋ କୌ"),L"Nirmala UI",0},
    {"telugu",utf8_literal("తెలుగు భాష నమస్కారం కి కై క్షేత్రం"),L"Nirmala UI",0},
    {"kannada",utf8_literal("ಕನ್ನಡ ಭಾಷೆ ನಮಸ್ಕಾರ ಕಿ ಕೈ ಕ್ಷೇತ್ರ"),L"Nirmala UI",0},
    {"malayalam",utf8_literal("മലയാളം ഭാഷ നമസ്കാരം കി കൊ കൗ"),L"Nirmala UI",0},
    {"sinhala",utf8_literal("සිංහල භාෂාව ආයුබෝවන්"),L"Nirmala UI",0},
    {"khmer",utf8_literal("ភាសាខ្មែរ សួស្តី ពិភពលោក"),L"Leelawadee UI",0},
    {"myanmar",utf8_literal("မြန်မာဘာသာ မင်္ဂလာပါ"),L"Myanmar Text",0},
    {"tibetan",utf8_literal("བོད་ཡིག བཀྲ་ཤིས་བདེ་ལེགས"),L"Microsoft Himalaya",0},
    {"ethiopic",utf8_literal("አማርኛ ሰላም ዓለም"),L"Nyala",0},
    {"emoji-basic",utf8_literal("😀 😃 ❤️ ☀️ ✨ 🎉"),L"Segoe UI",0},
    {"emoji-sequences",utf8_literal("👨‍👩‍👧‍👦 👩🏽‍💻 🏳️‍🌈 🇸🇮 1️⃣"),L"Segoe UI",0},
    {"mixed-fallback",utf8_literal("AV café 日本語 한국어 אבג العربية 😀"),L"Segoe UI",0},
    {"styled-decorations",utf8_literal("AV styled text office café 😀 wraps across lines."),L"Segoe UI",1},
    {"ellipsis-latin",utf8_literal("AV office and a long ordinary sentence with emoji 😀"),L"Segoe UI",2},
    {"ellipsis-rtl",utf8_literal("אבג العربية 123 a long mixed paragraph with emoji 😀"),L"Segoe UI",2},
    {"ellipsis-cjk",utf8_literal("日本語 ひらがな カタカナ 中文文字 😀"),L"Yu Gothic UI",2},
    {"tabs-newlines",utf8_literal("A\tB\tC\nline two AV office\nthird line 😀"),L"Segoe UI",0},
    {"tracking",utf8_literal("AV office cafe אבג العربية 😀"),L"Segoe UI",3},
    {"armenian",utf8_literal("Հայերեն բարեւ աշխարհ տեքստ"),L"Segoe UI",0},
    {"georgian",utf8_literal("ქართული გამარჯობა მსოფლიო"),L"Segoe UI",0},
    {"cherokee",utf8_literal("ᏣᎳᎩ ᎣᏏᏲ ᎡᎶᎯ"),L"Gadugi",0},
    {"serif-latin",utf8_literal("AV office café ffi To naïve Å"),L"Georgia",0},
    {"monospace-latin",utf8_literal("AV office cafe 012345 {} [] => !="),L"Consolas",0},
    {"lao-context",utf8_literal("ເຝົ້າ ເຟົ້າ ເໜົ້າ"),L"Leelawadee UI",0},
    {"styled-paint-mixed",utf8_literal("AV colored links office café 😀 wraps across lines."),L"Segoe UI",4},
    {"styled-size-mixed",utf8_literal("AV sized text office café 😀 wraps across lines."),L"Segoe UI",5},
    {"styled-fixed-mixed",utf8_literal("AV fixed text office café 😀 wraps across lines."),L"Segoe UI",6},
    {"styled-paint-pure-emoji",utf8_literal("😀😃❤️☀️✨🎉"),L"Segoe UI",7},
    {"styled-paint-bidi",utf8_literal("AV colored אבג العربية café 😀 wraps across lines."),L"Segoe UI",4},
    {"egyptian",utf8_literal("AV 𓀀𓃀𓋹 𓐯 𓀀\u200d𓃀 😀"),L"Segoe UI",0},
    {"runic",utf8_literal("ᚠᚢᚦ ᚠ\u0308 ᚢ\u0301 אבג (ᚱᚲ) 123"),L"Segoe UI",0},
    {"han-default",utf8_literal("简体中文文本测试，换行与标点。你好世界！"),L"Segoe UI",0},
    {"han-monospace",utf8_literal("繁體中文文字測試，換行與標點。你好世界！"),L"Consolas",0},
};
bool native_fresh_engine_benchmark(const wchar_t* argument) noexcept {
    const auto parse=[&](u32* out) noexcept {while(*argument==' ')++argument;*out=0;if(*argument<'0'||*argument>'9')return false;
        while(*argument>='0'&&*argument<='9'){if(*out>10000)return false;*out=*out*10+*argument++-'0';}return true;};
    u32 index=0,side=0;CHECK(parse(&index)&&parse(&side)&&!*argument&&index<sizeof(fresh_engine_cases)/sizeof(fresh_engine_cases[0])&&side<2);
    const auto& sample=fresh_engine_cases[index];
    Output* outputs[]{&native_output,&reference_output};for(auto* out:outputs){CHECK(shaped_text_initialize(&out->text,{out->glyphs,1024},{out->runs,256},{out->clusters,256}));CHECK(shaped_text_bind_decorations(&out->text,{out->decorations,256}));CHECK(shaped_text_bind_hit_regions(&out->text,{out->hits,4096}));}
    const auto create=[&](DWriteTextSystem& system,u32 selected) noexcept {
        if(!selected)return text_system_initialize(static_cast<TextSystem*>(&system),sample.family,16,L"en-US").ok();
        const auto result=text_compatibility_objects_initialize(&system,sample.family,16,L"en-US");system.em_size=16;return SUCCEEDED(result);};
    TextStyleSpan styles[]{{0,2,text_style_bold|text_style_underline},{3,6,text_style_italic|text_style_strikethrough}};
    if(sample.mode==4){styles[0].flags=text_style_link|text_style_underline;styles[1].flags=text_style_strikethrough;}
    if(sample.mode==5){styles[0].flags=text_style_heading_1;styles[1].flags=text_style_subtext;}
    if(sample.mode==6){styles[0].flags=text_style_bold|text_style_underline;styles[1].flags=text_style_bold|text_style_strikethrough;}
    if(sample.mode==7)styles[0]={0,sample.text.bytes,text_style_underline};
    const auto shape=[&](DWriteTextSystem& system,Utf8View text,Output& output) noexcept {
        system.tracking=sample.mode==3?.03f:0;system.fixed_weight=sample.mode==6;
        return text_shape_utf8_styled(&system,text,{styles,sample.mode==7?1u:(sample.mode==1||sample.mode>=4)?2u:0u},sample.mode==2?70.f:160.f,1000,{utf16,256},&output.text,
            sample.mode==2?TextWrapping::no_wrap:TextWrapping::wrap,sample.mode==2?TextOverflow::ellipsis:TextOverflow::clip);};
    const auto memory=[](PROCESS_MEMORY_COUNTERS_EX& value) noexcept {value={};value.cb=sizeof(value);return GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&value),sizeof(value))!=FALSE;};
    PROCESS_MEMORY_COUNTERS_EX mem[5]{};CHECK(memory(mem[0]));LARGE_INTEGER frequency{};CHECK(QueryPerformanceFrequency(&frequency));
    DWriteTextSystem measured{};struct Release {DWriteTextSystem& s;~Release() noexcept {shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);text_system_release(&s);}} release{measured};
    u64 ticks[3]{},cycles[3]{};LARGE_INTEGER first{},last{};ULONG64 start_cycles=0,end_cycles=0;
    CHECK(QueryThreadCycleTime(GetCurrentThread(),&start_cycles)&&QueryPerformanceCounter(&first));const bool initialized=create(measured,side);
    CHECK(QueryPerformanceCounter(&last)&&QueryThreadCycleTime(GetCurrentThread(),&end_cycles));ticks[0]=last.QuadPart-first.QuadPart;cycles[0]=end_cycles-start_cycles;CHECK(initialized&&memory(mem[1]));
    CHECK(side?(!measured.native_text&&!measured.native_fonts&&measured.factory&&measured.format&&measured.emoji_font_policy):(!measured.factory&&!measured.format&&!measured.emoji_font_policy&&measured.native_format));
    auto& output=side?reference_output:native_output;
    CHECK(QueryThreadCycleTime(GetCurrentThread(),&start_cycles)&&QueryPerformanceCounter(&first));const auto shaped=shape(measured,sample.text,output);
    CHECK(QueryPerformanceCounter(&last)&&QueryThreadCycleTime(GetCurrentThread(),&end_cycles));ticks[1]=last.QuadPart-first.QuadPart;cycles[1]=end_cycles-start_cycles;CHECK(memory(mem[2]));
    char alternate[1024]{};CHECK(sample.text.bytes+1<sizeof(alternate));copy_bytes(alternate,sample.text.data,sample.text.bytes);alternate[sample.text.bytes]='!';
    bool changed_ok=shaped.ok();
    if(changed_ok){CHECK(QueryThreadCycleTime(GetCurrentThread(),&start_cycles)&&QueryPerformanceCounter(&first));
        for(u32 i=0;i<32;++i){const auto result=shape(measured,i&1?sample.text:Utf8View{alternate,sample.text.bytes+1},output);if(!result.ok()){changed_ok=false;break;}}
        CHECK(QueryPerformanceCounter(&last)&&QueryThreadCycleTime(GetCurrentThread(),&end_cycles));ticks[2]=last.QuadPart-first.QuadPart;cycles[2]=end_cycles-start_cycles;}
    const auto scratch=native_text_candidate_storage_counts(measured);const auto fonts=native_font_session_stats(measured.native_fonts);CHECK(memory(mem[3]));
    shaped_text_reset(&output.text);text_system_release(&measured);CHECK(memory(mem[4]));
    // Only after measurements and release, construct both providers for parity.
    DWriteTextSystem candidate{};Release candidate_release{candidate};CHECK(create(candidate,0)&&create(owner,1));
    const auto a=shape(candidate,sample.text,native_output),b=shape(owner,sample.text,reference_output);
    // Different adjacent run partitions are permitted only when every glyph
    // retains exact font, absolute origin, bidi/style attributes and source map.
    bool exact=a.ok()&&b.ok()&&same(native_output,reference_output,true);
    if(exact){const auto aa=shape(candidate,{alternate,sample.text.bytes+1},native_output),bb=shape(owner,{alternate,sample.text.bytes+1},reference_output);exact=aa.ok()&&bb.ok()&&same(native_output,reference_output,true);}
    const auto parity_catalog=native_text_candidate_storage_counts(candidate);
    oracle_faces_release();
    print("{\"kind\":\"fresh_engine\",\"case\":");number(index);print(",\"name\":\"");print(sample.name);print("\",\"side\":");number(side);print(",\"frequency\":");number(frequency.QuadPart);
    print(",\"ticks\":[");for(u32 i=0;i<3;++i){if(i)print(",");number(ticks[i]);}print("],\"cycles\":[");for(u32 i=0;i<3;++i){if(i)print(",");number(cycles[i]);}
    print("],\"memory\":[");for(u32 i=0;i<5;++i){if(i)print(",");print("[");number(mem[i].PrivateUsage);print(",");number(mem[i].WorkingSetSize);print(",");number(mem[i].PeakWorkingSetSize);print(",");number(mem[i].PeakPagefileUsage);print("]");}
    print("],\"native_storage\":[");number(scratch.reserved);print(",");number(scratch.committed);print(",");number(scratch.peak);print(",");number(fonts.metadata_committed);print(",");number(fonts.instance_committed);print(",");number(fonts.live.mapped_bytes);
    print("],\"catalog_build_ticks\":");number(scratch.catalog_build_ticks);
    // Untimed parity-owner catalog work, kept distinct from the measured owner.
    print(",\"parity_catalog_build_ticks\":");number(parity_catalog.catalog_build_ticks);
    print(",\"shape_error\":");number(static_cast<u32>(shaped.error));print(",\"native_error\":");number(static_cast<u32>(a.error));print(",\"reference_error\":");number(static_cast<u32>(b.error));print(",\"changed_ok\":");print(changed_ok?"true":"false");print(",\"exact\":");print(exact?"true":"false");print("}\n");
    return true;
}
