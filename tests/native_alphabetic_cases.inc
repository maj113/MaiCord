// Independent app comparisons: source text is intentionally unnormalized.
const Utf8View alphabetic_samples[]{
    utf8_literal("\xce\x9a\xce\xb1\xce\xbb\xce\xb7\xce\xbc\xce\xad\xcf\x81\xce\xb1\x20\xce\xba\xcf\x8c\xcf\x83\xce\xbc\xce\xb5"),
    utf8_literal("\xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd0\xb5\xd1\x82\x20\xd0\xbc\xd0\xb8\xd1\x80"),
    utf8_literal("\xd0\xa3\xd0\xba\xd1\x80\xd0\xb0\xd1\x97\xd0\xbd\xd0\xb0\x20\xd1\x97\xd0\xb6\xd0\xb0\xd0\xba"),
    utf8_literal("\xe1\xbc\x84\xce\xbd\xce\xb8\xcf\x81\xcf\x89\xcf\x80\xce\xbf\xcf\x82\x20\xe1\xbe\xa0\xce\xb4\xce\xae"),
    utf8_literal("\x61\xce\xb1\xd0\xb0"),
    utf8_literal("\x47\x72\x65\x65\x6b\x20\xce\x95\xce\xbb\xce\xbb\xce\xb7\xce\xbd\xce\xb9\xce\xba\xce\xac\x20\x43\x79\x72\x69\x6c\x6c\x69\x63\x20\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9\x20\x4c\x61\x74\x69\x6e\x20\x63\x61\x66\xc3\xa9"),
    utf8_literal("\x28\xce\xb1\xce\xb2\x29\x20\x5b\xd0\x90\xd0\x91\x5d\x20\x41\x56\x20\x6f\x66\x66\x69\x63\x65"),
    utf8_literal("\xce\xb1\xcc\x93\xcc\x81\x20\xce\xb9\xcc\x88\xcc\x81"),
    utf8_literal("\xd0\x98\xcc\x86\x20\xd0\x95\xcc\x88\x20\xd0\xb8\xcc\x86\x20\xd0\xb5\xcc\x88"),
    utf8_literal("\xce\xb1\xcc\x93\xcc\x80\xcd\x85\xcc\x81\x20\xce\xb2"),
    utf8_literal("\xce\xb1\xe2\x80\x8c\xce\xb2\x20\xd0\x90\xe2\x80\x8d\xd0\x91"),
    utf8_literal("\xcc\x81\xce\xb1\x20\xce\xb2\xcc\xa7"),
    utf8_literal("\x20\x20\xce\x95\xce\xbb\xce\xbb\xce\xb7\xce\xbd\xce\xb9\xce\xba\xce\xac\x20\x20"),
    utf8_literal("\xce\xb1\x09\xd0\x90\x0d\x0a\x61\x0a\xce\xb2"),
    utf8_literal("\x0a\x20\x20\xce\x91\x0a\x0a\xd0\x91\x09"),
    utf8_literal("\xce\xb1\xe2\x80\xa8\xd0\x91\xe2\x80\xa9\x63"),
    utf8_literal("\xce\xb1\xc2\xa0\xce\xb2\x20\xd0\x90\xe2\x80\xaf\xd0\x91"),
    utf8_literal("\x41\x56\x20\xce\x95\xce\xbb\xce\xbb\xce\xb7\xce\xbd\xce\xb9\xce\xba\xce\xac\x20\x41\x56\x20\xd0\xa0\xd1\x83\xd1\x81\xd1\x81\xd0\xba\xd0\xb8\xd0\xb9\x20\x66\x66\x69"),
    utf8_literal("\x65\xcc\x81\x20\xce\xb1\xcc\x81\x20\xd0\xb8\xcc\x86"),
    utf8_literal("\x41\x20\xce\xb1\x20\xd0\x90\x0d\x0a\x09\x20\x20"),
    utf8_literal("\xce\xb1\xce\xb2\xce\xb3\x20\xd0\xb0\xd0\xb1\xd0\xb2"),
    utf8_literal("\xce\xb1\xcd\x8f\xcc\x81\x20\xd0\x90\xcd\x8f\xcc\x86"),
    utf8_literal("\xce\xb2\x20\xcf\x8a\xcc\x80\x20\xce\xb3"),
    utf8_literal("\xce\xb2\x20\xce\x91\xcc\x93\xcc\x80\xcc\x81\x20\xce\xb3"),
    utf8_literal("\xce\xb2\x20\xc2\xa8\xcd\x82\xe2\x80\x8c\x20\xce\xb3"),
    utf8_literal("\xce\xb2\x20\xce\x91\xcc\x81\xcc\x81\x20\xce\xb3"),
};
bool alphabetic_corpus(DWriteTextSystem system,bool required) noexcept {
    const float widths[]{8,37,95,1000000};const u32 flags[]{0,text_style_link,text_style_underline|text_style_strikethrough,text_style_bold|text_style_italic,text_style_monospace,text_style_heading_2|text_style_underline};
    const float em=system.format->GetFontSize();system.em_size=em;
    for(u32 sample=0;sample<sizeof(alphabetic_samples)/sizeof(alphabetic_samples[0]);++sample){const auto text=alphabetic_samples[sample];
        for(u32 mode=0;mode<4;++mode)for(u32 flag:flags){system.independent_letters=(mode&1)!=0;system.tracking=(mode&2)?.03f:0;
            const TextStyleSpan style{0,text.bytes,flag};const ConstSlice<TextStyleSpan> styles{&style,flag?1u:0u};
            for(float w:widths)for(u32 nowrap=0;nowrap<2;++nowrap){++cases;const float width=w==1000000?w:w*(em/16);const auto wrap=nowrap?TextWrapping::no_wrap:TextWrapping::wrap;
                shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);auto reference=system;reference.native_text=nullptr;
                const bool native=flag?native_text_candidate_styled(system,text,styles,width,1000000,&native_output.text,nullptr,wrap):native_text_candidate_line(system,text,width,1000000,&native_output.text,nullptr,wrap);
                // Segoe UI italic faces lack these controls. Windows font
                // fallback changes the clusters; keep that transition explicit.
                bool fallback=false;if(flag&text_style_italic)for(u32 at=0;at<text.bytes;){const auto cp=utf8_decode_one(text.data+at,text.bytes-at);fallback|=cp.codepoint==0x200c||cp.codepoint==0x200d||cp.codepoint==0x34f;at+=cp.width;}
                if(required&&!fallback&&!native){print("alphabetic declined sample/mode/flag=");number(sample);print("/");number(mode);print("/");number(flag);print("\n");return false;}
                if(native)++native_cases;else CHECK(text_shape_utf8_styled(&system,text,styles,width,1000000,{utf16,256},&native_output.text,wrap).ok());
                CHECK(text_shape_utf8_styled(&reference,text,styles,width,1000000,{utf16,256},&reference_output.text,wrap).ok());
                const auto a=text_measure_utf8_styled(&system,text,styles,width,1000000,{utf16,256},wrap),b=text_measure_utf8_styled(&reference,text,styles,width,1000000,{utf16,256},wrap);
                if(!same(native_output,reference_output,true,true)||!a.ok()||!b.ok()||!bytes_equal(&a.width,&b.width,12)||a.line_count!=b.line_count){
                    print("alphabetic sample/mode/flag/em/width/nowrap=");number(sample);print("/");number(mode);print("/");number(flag);print("/");real(em);print("/");real(width);print("/");number(nowrap);print("\n");return false;}
                if(native&&!mode&&!compare_hits(system,text,width,wrap,styles)){print("alphabetic hit sample/flag/em/width/nowrap=");number(sample);print("/");number(flag);print("/");real(em);print("/");real(width);print("/");number(nowrap);print("\n");return false;}
            }
        }
    }return true;
}
bool alphabetic_benchmark() noexcept {
    CHECK(native_text_format_capture(&owner));const u32 pairs[][2]{{0,3},{1,2},{5,17},{7,8}};LARGE_INTEGER frequency{};CHECK(QueryPerformanceFrequency(&frequency));
    auto reference=owner;reference.native_text=nullptr;
    for(u32 pair=0;pair<4;++pair){for(u32 item=0;item<2;++item){const auto text=alphabetic_samples[pairs[pair][item]];
            shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);
            CHECK(native_text_candidate_line(owner,text,120,1000000,&native_output.text,nullptr));
            CHECK(text_shape_utf8(&reference,text,120,1000000,{utf16,256},&reference_output.text).ok()&&same(native_output,reference_output,true));}
        for(u32 mode=0;mode<2;++mode){print("{\"kind\":\"alphabetic_app\",\"pair\":");number(pair);print(",\"fresh\":");number(mode);print(",\"frequency\":");number(frequency.QuadPart);print(",\"iterations\":1024,\"batches\":[");
            for(u32 batch=0;batch<12;++batch){u64 ticks[2]{};
                for(u32 order=0;order<2;++order){const u32 legacy=order^(batch&1);auto system=legacy?reference:owner;auto* out=legacy?&reference_output.text:&native_output.text;
                    CHECK(text_shape_utf8(&system,alphabetic_samples[pairs[pair][0]],120,1000000,{utf16,256},out).ok());
                    const auto before=native_text_candidate_counts(owner);LARGE_INTEGER start{},end{};CHECK(QueryPerformanceCounter(&start));
                    for(u32 i=0;i<1024;++i)CHECK(text_shape_utf8(&system,alphabetic_samples[pairs[pair][mode?(i&1):0]],120,1000000,{utf16,256},out).ok());
                    CHECK(QueryPerformanceCounter(&end));ticks[legacy]=end.QuadPart-start.QuadPart;const auto after=native_text_candidate_counts(owner);
                    CHECK(after.shaped-before.shaped==(legacy?0u:1024u));}
                if(batch)print(",");print("[");number(ticks[0]);print(",");number(ticks[1]);print("]");}print("]}\n");}
    }return true;
}
