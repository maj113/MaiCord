// Families whose regular face is drawn emboldened or slanted for semibold/italic requests. A request the engine
// declines (another family answers part of the text) is observed as declined.
bool native_simulated_style_corpus() noexcept {
    const wchar_t* families[]{L"Segoe UI Historic",L"MV Boli",L"Microsoft Himalaya",L"Mongolian Baiti",L"Segoe UI Symbol",L"Segoe Print",L"Bahnschrift"};
    const Utf8View texts[]{utf8_literal("AV To office"),utf8_literal("AV\tTo\r\nAV  "),utf8_literal("A\xcc\x81" "e\xcc\x81 To"),utf8_literal("AV \xf0\x9f\x98\x80 To")};
    const float sizes[]{14,17.5f,1000},widths[]{8,37,95,1000000};const u32 flags[]{0,text_style_link,text_style_underline,text_style_italic,text_style_bold,text_style_bold|text_style_italic};
    for(u32 family=0;family<7;++family){AppSystem system{};CHECK(text_system_initialize(static_cast<TextSystem*>(&system),families[family],16).ok());
        struct Release {AppSystem& s;~Release() noexcept{shaped_text_reset(&native_output.text);text_system_release(&s);}} release{system};
        for(u32 mode=0;mode<4;++mode)for(float em:sizes){
            auto current=system;current.em_size=em;CHECK(set_format(current,families[family],em,(mode&1)?600:400,(mode&2)?2:0));
            for(u32 sample=0;sample<4;++sample)for(u32 flag:flags)for(float w:widths)for(u32 nowrap=0;nowrap<2;++nowrap){const auto text=texts[sample];const float width=w==1000000?w:w*(em/16);
                const TextStyleSpan style{0,flag==text_style_link||flag==text_style_italic?1u:text.bytes,flag};const ConstSlice<TextStyleSpan> styles{&style,flag?1u:0u};const auto wrapping=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;++cases;
                const auto diagnostic=[&]() noexcept{print("simulation family/mode/em/sample/flag/width/nowrap=");number(family);print("/");number(mode);print("/");real(em);print("/");number(sample);print("/");number(flag);print("/");real(width);print("/");number(nowrap);print("\n");};
                const auto shaped=text_shape_utf8(&current, {text,styles,width,1000000,wrapping}, &native_output.text);observe(shaped);
                const bool native=shaped.ok()&&native_output.text.native_fonts();if(!native)continue;++native_cases;
                observe(native_output.text,text,current);
                const auto a=text_measure_utf8(&current, {text,styles,width,1000000,wrapping});observe_measure(a);
                if(!a.ok()||!same_metrics(a,native_output.text)||!compare_hits(current,text,width,wrapping,styles)){diagnostic();return false;}
                if(!flag&&w==1000000&&!nowrap)CHECK(draw_source(current,native_output.text));
            }
        }
    }return true;
}
bool native_simulated_profile_corpus(AppSystem system) noexcept {
    const Utf8View texts[]{utf8_literal("AV To office"),utf8_literal("AV To\nffi AV\tTo  "),utf8_literal("A V  fi fi")};const float widths[]{8,37,95,1000000};
    const u32 flags[]{text_style_italic,text_style_bold|text_style_italic|text_style_underline};
    // Bundled profile weights are fixed in the app. This checks its actual
    // italic policy without requesting synthetic CFF bold or private variable
    // instances that the profile selector does not expose.
    system.fixed_weight=true;
    for(u32 mode=0;mode<2;++mode){system.independent_letters=mode!=0;
        for(const auto text:texts)for(u32 flag:flags)for(float w:widths)for(u32 nowrap=0;nowrap<2;++nowrap){const auto wrapping=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;
            const float width=w==1000000?w:w*(system.em_size/16);const TextStyleSpan style{0,text.bytes,flag};++cases;
            CHECK(layout(system,text,{&style,1},width,wrapping));++native_cases;
            CHECK(compare_hits(system,text,width,wrapping,{&style,1}));if(!mode&&w==1000000&&!nowrap)CHECK(draw_source(system,native_output.text));
        }
    }return true;
}
