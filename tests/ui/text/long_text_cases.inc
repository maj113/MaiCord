bool native_long_text_cases(bool editor=false) noexcept {
    constexpr u32 limit=native_text_maximum_bytes;
    struct Large {TextGlyph glyphs[limit+128];TextRun runs[limit];u16 clusters[limit];char input[limit+1];
        NativeParagraphHitCluster hit_clusters[limit];NativeParagraphHitLine hit_lines[limit+1];float advances[limit*5];
        SfntFontLineParts parts[limit];NativeParagraphHitRun visual[limit];};
    auto* storage=static_cast<Large*>(VirtualAlloc(nullptr,sizeof(Large),MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));CHECK(storage);
    shaped_text_reset(&native_output.text);
    struct Restore {Large* memory;ShapedText native;~Restore() noexcept {
        shaped_text_reset(&native_output.text);native_output.text=native;VirtualFree(memory,0,MEM_RELEASE);
    }} restore{storage,native_output.text};
    CHECK(shaped_text_initialize(&native_output.text,{storage->glyphs,limit+128},{storage->runs,limit},{storage->clusters,limit}));
    auto native=owner;
    const Utf8View samples[]{utf8_literal("office AV text "),utf8_literal("한글 text "),utf8_literal("abc אבג xyz "),utf8_literal("hello 😀 world "),utf8_literal("á")};
    const u32 lengths[]{4097,8193,16384,32768};const float widths[]{240.f,1000000.f};
    for(u32 sample=0;sample<5;++sample)for(u32 bytes:lengths){if((editor&&bytes<16384)||(sample==4&&bytes!=32768))continue;
        u32 filled=0;if(sample==4)storage->input[filled++]='b';while(samples[sample].bytes<=bytes-filled){copy_bytes(storage->input+filled,samples[sample].data,samples[sample].bytes);filled+=samples[sample].bytes;}
        while(filled<bytes)storage->input[filled++]='a';const Utf8View text{storage->input,bytes};
        // Place a font-style boundary beyond each former source-offset limit.
        const u32 cut=bytes>16384?24576:bytes>8192?8192:4096;u32 start=cut;
        while(start<bytes&&(static_cast<u8>(text.data[start])&0xc0)==0x80)++start;
        const TextStyleSpan style{start,bytes-start,text_style_bold};
        for(u32 styled=0;styled<2;++styled)for(float width:widths){const ConstSlice<TextStyleSpan> styles{&style,styled&&bytes>start?1u:0u};++cases;print("long sample/bytes/width=");number(sample);print("/");number(bytes);print("/");real(width);print("\n");
            if(editor){NativeParagraphHits hits{};auto& a=*storage;NativeParagraphHitStorage buffers{{a.hit_clusters,limit},{a.hit_lines,limit+1},{a.advances,limit*5},&hits,{a.parts,limit},{a.visual,limit}};TextMeasureResult measure{};
                CHECK(native_text_candidate_hits(native,{text,styles,width,1000000},buffers,&measure));observe_measure(measure);
                const u32 units=hits.source_units;
                const u32 positions[]{0,4095,4096,8191,8192,15999,16000,16001,16383,16384,24575,24576,units-1,units};
                for(u32 position:positions)if(position<=units)for(u32 trailing=0;trailing<2;++trailing){float x=0;NativeParagraphHitMetric metric{};
                    CHECK(native_paragraph_hit_position(hits,position,trailing!=0,&metric,&x)==SfntStatus::ok);observe_value(metric);observe_value(x);}
                ++native_cases;continue;
            }
            CHECK(text_shape_utf8(&native, {text,styles,width,1000000}, &native_output.text).ok()&&native_output.text.native_fonts());
            observe(native_output.text,text,native);++native_cases;
        }
    }
    TextMeasureResult rejected{};CHECK(!native_text_candidate_line(native,{{storage->input,limit+1},{},240,1000},nullptr,&rejected));
    CHECK(native_text_candidate_style_capacity({storage->input,limit+1},{})==0);
    const auto stats=native_text_candidate_storage_counts(owner);print("scratch reserved/committed/peak/retries=");number(stats.reserved);print("/");number(stats.committed);print("/");number(stats.peak);print("/");number(stats.retries);print("\n");return true;
}
