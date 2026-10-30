bool native_simulated_style_corpus() noexcept {
    u32 other_family_cases=0;
    const wchar_t* families[]{L"Segoe UI Historic",L"MV Boli",L"Microsoft Himalaya",L"Mongolian Baiti",L"Segoe UI Symbol",L"Segoe Print",L"Bahnschrift"};
    const Utf8View texts[]{utf8_literal("AV To office"),utf8_literal("AV\tTo\r\nAV  "),utf8_literal("A\xcc\x81" "e\xcc\x81 To"),utf8_literal("AV \xf0\x9f\x98\x80 To")};
    const float sizes[]{14,17.5f,1000},widths[]{8,37,95,1000000};const u32 flags[]{0,text_style_link,text_style_underline,text_style_italic,text_style_bold,text_style_bold|text_style_italic};
    for(u32 family=0;family<7;++family){DWriteTextSystem system{};CHECK(text_system_initialize(&system,families[family],16).ok());
        struct Release {DWriteTextSystem& s;~Release() noexcept{shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);text_system_release(&s);}} release{system};
        for(u32 mode=0;mode<4;++mode)for(float em:sizes){IDWriteTextFormat* format=nullptr;
            CHECK(SUCCEEDED(system.factory->CreateTextFormat(families[family],nullptr,(mode&1)?DWRITE_FONT_WEIGHT_SEMI_BOLD:DWRITE_FONT_WEIGHT_NORMAL,(mode&2)?DWRITE_FONT_STYLE_ITALIC:DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,em,L"en-US",&format)));
            struct FormatRelease {IDWriteTextFormat*& p;~FormatRelease() noexcept{com_release(p);}} format_release{format};
            auto current=system;current.format=format;current.em_size=em;CHECK(native_text_format_capture(&current));auto reference=current;reference.native_text=nullptr;current.factory=nullptr;current.format=nullptr;
            for(u32 sample=0;sample<4;++sample)for(u32 flag:flags)for(float w:widths)for(u32 nowrap=0;nowrap<2;++nowrap){const auto text=texts[sample];const float width=w==1000000?w:w*(em/16);
                const TextStyleSpan style{0,flag==text_style_link||flag==text_style_italic?1u:text.bytes,flag};const ConstSlice<TextStyleSpan> styles{&style,flag?1u:0u};const auto wrapping=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;++cases;
                const auto diagnostic=[&]() noexcept{print("simulation family/mode/em/sample/flag/width/nowrap=");number(family);print("/");number(mode);print("/");real(em);print("/");number(sample);print("/");number(flag);print("/");real(width);print("/");number(nowrap);print("\n");};
                CHECK(text_shape_utf8(&reference,text,styles,width,1000000,{utf16,256},&reference_output.text,wrapping).ok());bool outside=false;
                for(u32 r=0;r<reference_output.text.run_count;++r){IDWriteFontFace3* face=nullptr;IDWriteLocalizedStrings* names=nullptr;
                    CHECK(SUCCEEDED(reference_run_face(reference_output.text.runs[r])->QueryInterface(__uuidof(IDWriteFontFace3),reinterpret_cast<void**>(&face)))&&SUCCEEDED(face->GetFamilyNames(&names)));bool found=false;
                    for(u32 name=0;name<names->GetCount();++name){wchar_t value[256];u32 length=0;CHECK(SUCCEEDED(names->GetStringLength(name,&length))&&length<256&&SUCCEEDED(names->GetString(name,value,256)));
                        found=found||CompareStringOrdinal(value,-1,families[family],-1,TRUE)==CSTR_EQUAL||CompareStringOrdinal(value,-1,L"Segoe UI Emoji",-1,TRUE)==CSTR_EQUAL;}
                    outside=outside||!found;com_release(names);com_release(face);}
                const bool native=text_shape_utf8(&current,text,styles,width,1000000,{utf16,256},&native_output.text,wrapping).ok()&&native_output.text.native_fonts();
                if(!native&&!outside){diagnostic();CHECK(false);}if(native)++native_cases;
                if(outside)++other_family_cases;
                auto actual=current;if(!native){actual.factory=system.factory;actual.format=format;CHECK(text_shape_utf8(&actual,text,styles,width,1000000,{utf16,256},&native_output.text,wrapping).ok());}
                if(!same(native_output,reference_output,true)){diagnostic();return false;}
                const auto a=text_measure_utf8(&actual,text,styles,width,1000000,{utf16,256},wrapping),b=text_measure_utf8(&reference,text,styles,width,1000000,{utf16,256},wrapping);
                CHECK(a.ok()&&b.ok()&&bytes_equal(&a.width,&b.width,12)&&a.line_count==b.line_count);
                auto hit=current;hit.factory=system.factory;hit.format=format;if(native&&!compare_hits(hit,text,width,wrapping,styles)){diagnostic();return false;}
                if(native&&!flag&&w==1000000&&!nowrap)CHECK(draw_source(hit,native_output.text));
            }
        }
    }print("other_family_reference_cases=");number(other_family_cases);print("\n");return true;
}
bool native_simulated_profile_corpus(DWriteTextSystem system) noexcept {
    const Utf8View texts[]{utf8_literal("AV To office"),utf8_literal("AV To\nffi AV\tTo  "),utf8_literal("A V  fi fi")};const float widths[]{8,37,95,1000000};
    const u32 flags[]{text_style_italic,text_style_bold|text_style_italic|text_style_underline};
    // Bundled profile weights are fixed in the app. This checks its actual
    // italic policy without requesting synthetic CFF bold or private variable
    // instances that the profile selector does not expose.
    system.fixed_weight=true;IDWriteFontCollection* collection=nullptr;wchar_t family[256];u32 index=0;BOOL exists=FALSE;
    CHECK(SUCCEEDED(system.format->GetFontCollection(&collection))&&SUCCEEDED(system.format->GetFontFamilyName(family,256)));
    const auto found=collection->FindFamilyName(family,&index,&exists);com_release(collection);CHECK(SUCCEEDED(found));
    if(!exists)print("native_missing_base_family=1\n");
    for(u32 mode=0;mode<2;++mode){system.independent_letters=mode!=0;auto native=system;native.factory=nullptr;native.format=nullptr;auto reference=system;reference.native_text=nullptr;
        for(const auto text:texts)for(u32 flag:flags)for(float w:widths)for(u32 nowrap=0;nowrap<2;++nowrap){const auto wrapping=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;
            const float width=w==1000000?w:w*(system.em_size/16);const TextStyleSpan style{0,text.bytes,flag};++cases;
            const bool used_native=text_shape_utf8(&native,text,{&style,1},width,1000000,{utf16,256},&native_output.text,wrapping).ok()&&native_output.text.native_fonts();
            CHECK(used_native);++native_cases;
            CHECK(text_shape_utf8(&reference,text,{&style,1},width,1000000,{utf16,256},&reference_output.text,wrapping).ok());CHECK(same(native_output,reference_output,true));
            auto measuring=used_native?native:system;const auto a=text_measure_utf8(&measuring,text,{&style,1},width,1000000,{utf16,256},wrapping),b=text_measure_utf8(&reference,text,{&style,1},width,1000000,{utf16,256},wrapping);
            CHECK(a.ok()&&b.ok()&&bytes_equal(&a.width,&b.width,12)&&a.line_count==b.line_count);
            if(used_native){CHECK(compare_hits(system,text,width,wrapping,{&style,1}));if(!mode&&w==1000000&&!nowrap)CHECK(draw_source(system,native_output.text));}
        }
    }return true;
}
