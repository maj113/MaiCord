bool native_long_text_cases(bool editor=false) noexcept {
    constexpr u32 limit=native_text_maximum_bytes;
    struct Large {TextGlyph glyphs[limit+128];TextRun runs[limit];u16 clusters[limit];wchar_t utf16[limit];char input[limit+1];
        NativeParagraphHitCluster hit_clusters[limit];NativeParagraphHitLine hit_lines[limit+1];float advances[limit*5];
        SfntFontLineParts parts[limit];NativeParagraphHitRun visual[limit];};
    auto* storage=static_cast<Large*>(VirtualAlloc(nullptr,sizeof(Large)*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(storage);
    shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);
    struct Restore {Large* memory;ShapedText native,reference;~Restore() noexcept {
        shaped_text_reset(&native_output.text);shaped_text_reset(&reference_output.text);
        native_output.text=native;reference_output.text=reference;VirtualFree(memory,0,MEM_RELEASE);
    }} restore{storage,native_output.text,reference_output.text};
    CHECK(shaped_text_initialize(&native_output.text,{storage[0].glyphs,limit+128},{storage[0].runs,limit},{storage[0].clusters,limit}));
    CHECK(shaped_text_initialize(&reference_output.text,{storage[1].glyphs,limit+128},{storage[1].runs,limit},{storage[1].clusters,limit}));
    CHECK(native_text_format_capture(&owner));auto native=owner,reference=owner;native.factory=nullptr;native.format=nullptr;reference.native_text=nullptr;
    const Utf8View samples[]{utf8_literal("office AV text "),utf8_literal("한글 text "),utf8_literal("abc אבג xyz "),utf8_literal("hello 😀 world "),utf8_literal("á")};
    const u32 lengths[]{4097,8193,16384,32768};const float widths[]{240.f,1000000.f};
    for(u32 sample=0;sample<5;++sample)for(u32 bytes:lengths){if((editor&&bytes<16384)||(sample==4&&bytes!=32768))continue;
        u32 filled=0;if(sample==4)storage[0].input[filled++]='b';while(samples[sample].bytes<=bytes-filled){copy_bytes(storage[0].input+filled,samples[sample].data,samples[sample].bytes);filled+=samples[sample].bytes;}
        while(filled<bytes)storage[0].input[filled++]='a';const Utf8View text{storage[0].input,bytes};
        // Place a font-style boundary beyond each former source-offset limit.
        const u32 cut=bytes>16384?24576:bytes>8192?8192:4096;u32 start=cut;
        while(start<bytes&&(static_cast<u8>(text.data[start])&0xc0)==0x80)++start;
        const TextStyleSpan style{start,bytes-start,text_style_bold};
        for(u32 styled=0;styled<2;++styled)for(float width:widths){const ConstSlice<TextStyleSpan> styles{&style,styled&&bytes>start?1u:0u};++cases;print("long sample/bytes/width=");number(sample);print("/");number(bytes);print("/");real(width);print("\n");
            if(editor){NativeParagraphHits hits{};auto& a=storage[0];NativeParagraphHitStorage buffers{{a.hit_clusters,limit},{a.hit_lines,limit+1},{a.advances,limit*5},&hits,{a.parts,limit},{a.visual,limit}};TextMeasureResult measure{};
                CHECK(styles.count?native_text_candidate_styled(native,text,styles,width,1000000,nullptr,&measure,TextWrapping::wrap,TextOverflow::clip,{},&buffers):
                    native_text_candidate_hits(native,text,width,1000000,TextWrapping::wrap,buffers,&measure));
                u32 units=0;for(u32 at=0;at<bytes;){const auto cp=utf8_decode_one(text.data+at,bytes-at);CHECK(cp.valid);
                    if(cp.codepoint>65535){a.utf16[units++]=static_cast<wchar_t>(0xd800+((cp.codepoint-65536)>>10));a.utf16[units++]=static_cast<wchar_t>(0xdc00+((cp.codepoint-65536)&1023));}
                    else a.utf16[units++]=static_cast<wchar_t>(cp.codepoint);at+=cp.width;}
                IDWriteTextLayout* layout=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTextLayout(a.utf16,units,owner.format,width,1000000,&layout)));
                struct Release {IDWriteTextLayout* p;~Release() noexcept {com_release(p);}} release{layout};
                CHECK(SUCCEEDED(text_apply_layout_styles(owner,layout,text,styles)));
                const u32 positions[]{0,4095,4096,8191,8192,15999,16000,16001,16383,16384,24575,24576,units-1,units};
                for(u32 position:positions)if(position<=units)for(u32 trailing=0;trailing<2;++trailing){float x=0,y=0,nx=0;DWRITE_HIT_TEST_METRICS m{};NativeParagraphHitMetric nm{};
                    CHECK(SUCCEEDED(layout->HitTestTextPosition(position,trailing,&x,&y,&m))&&native_paragraph_hit_position(hits,position,trailing!=0,&nm,&nx)==SfntStatus::ok);
                    if(!same_hit(nm,m)||nx!=x){print("long caret position/trailing/native/reference=");number(position);print("/");number(trailing);print("/");real(nx);print("/");real(x);print("\n");CHECK(false);}}
                ++native_cases;continue;
            }
            CHECK(text_shape_utf8_styled(&native,text,styles,width,1000000,{storage[0].utf16,limit},&native_output.text).ok()&&native_output.text.native_fonts());
            CHECK(text_shape_utf8_styled(&reference,text,styles,width,1000000,{storage[1].utf16,limit},&reference_output.text).ok());
            CHECK(same(native_output,reference_output,true));++native_cases;
        }
    }
    TextMeasureResult rejected{};CHECK(!native_text_candidate_line(native,{storage[0].input,limit+1},240,1000,nullptr,&rejected));
    CHECK(native_text_candidate_style_capacity({storage[0].input,limit+1},{})==0);
    const auto stats=native_text_candidate_storage_counts(owner);print("scratch reserved/committed/peak/retries=");number(stats.reserved);print("/");number(stats.committed);print("/");number(stats.peak);print("/");number(stats.retries);print("\n");return true;
}
