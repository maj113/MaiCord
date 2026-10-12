bool directional_hit_guards(const DWriteTextSystem& candidate) noexcept {
    NativeParagraphHitCluster clusters[64];NativeParagraphHitLine lines[65];float advances[272];SfntFontLineParts styles[64];NativeParagraphHitRun runs[64];
    NativeParagraphHits hits{};u32 required=0;TextMeasureResult measure{};
    NativeParagraphHitStorage storage{{clusters,64},{lines,65},{advances,272},&hits,{styles,64},{runs,0},&required};
    const auto text=utf8_literal("A \xd7\x90\xd7\x91\xd7\x92 B");
    const auto fit=[&]() noexcept {return native_text_candidate_hits(candidate,text,7,1000,TextWrapping::wrap,storage,&measure);};
    CHECK(!fit()&&required>1&&required<=64&&!hits.clusters.count&&!hits.lines.count&&!hits.visual_runs.count);
    const u32 needed=required;storage.visual_runs.count=needed-1;
    CHECK(!fit()&&required==needed&&!hits.clusters.count&&!hits.lines.count&&!hits.visual_runs.count);
    storage.visual_runs.count=needed;CHECK(fit()&&hits.visual_runs.count==needed);
    NativeParagraphHitMetric exact[64];u32 count=0;CHECK(native_paragraph_hit_range(hits,0,hits.source_units,0,0,{exact,64},&count)==SfntStatus::ok&&count>1);
    NativeParagraphHitMetric short_output[64];u32 short_count=0;CHECK(native_paragraph_hit_range(hits,0,hits.source_units,0,0,{short_output,count-1},&short_count)==SfntStatus::output_too_small&&short_count==count);
    CHECK(bytes_equal(exact,short_output,(count-1)*sizeof(exact[0])));
    storage.visual_runs.data=nullptr;CHECK(!fit()&&!hits.visual_runs.count&&!hits.lines.count);storage.visual_runs.data=runs;
    storage.visual_runs.count=64;CHECK(fit());
    // A property cut inside this complete grapheme retains a terminal region.
    const auto cut=directional_editor_samples[6];const TextStyleSpan underline{8,cut.bytes-8,text_style_underline};
    CHECK(native_text_candidate_styled(candidate,cut,{&underline,1},95,1000,nullptr,&measure,TextWrapping::wrap,TextOverflow::clip,{},&storage)&&hits.lines.count&&hits.visual_runs.count);
    CHECK(compare_hits(owner,cut,95,TextWrapping::wrap,{&underline,1},true));
    const TextStyleSpan invalid{3,cut.bytes-3,text_style_underline};
    CHECK(!native_text_candidate_styled(candidate,cut,{&invalid,1},95,1000,nullptr,&measure,TextWrapping::wrap,TextOverflow::clip,{},&storage)&&!hits.lines.count&&!hits.visual_runs.count);
    CHECK(fit());return true;
}
bool directional_edit() noexcept {
    CHECK(native_text_format_capture(&owner));auto candidate=owner,reference=owner;candidate.factory=nullptr;candidate.format=nullptr;reference.native_text=nullptr;
    CHECK(directional_hit_guards(candidate));
    DWriteEditLayoutOwner layouts[2]{};TextEditMetric selections[2][48];
    for(u32 i=0;i<2;++i){layouts[i].selections=selections[i];layouts[i].selection_capacity=48;}
    struct Release {DWriteEditLayoutOwner* p;~Release() noexcept {text_edit_layout_release(p);text_edit_layout_release(p+1);}} release{layouts};
    const float widths[]{7,95,1000000};const u32 flags[]{0,text_style_heading_3,text_style_monospace};
    for(u32 sample=0;sample<sizeof(directional_editor_samples)/sizeof(directional_editor_samples[0]);++sample){const auto text=directional_editor_samples[sample];u32 units=0;
        CHECK(utf8_utf16_position_for_byte_offset(text,text.bytes,&units));
        for(float width:widths)for(u32 flag:flags)for(u32 options=0;options<8;++options){const TextStyleSpan style{0,text.bytes,flag};
            TextEditLayoutInput input{text,text_edit_signature(text),0,0,9.125f,32.25f,width,40,1000000,static_cast<u8>(options),{&style,flag?1u:0u}};
            for(u32 a=0;a<=units;++a)for(u32 p=0;p<=units;++p){input.anchor_utf16=a;input.caret_utf16=p;++cases;
                CHECK(text_edit_layout_measure(layouts,input,candidate)&&text_edit_layout_measure(layouts+1,input,reference));
                CHECK(layouts[0].geometry&&!layouts[0].layout&&layouts[1].layout);++native_cases;
                if(!same_edit(layouts[0],layouts[1])){print("retained directional sample/options/flag/width/anchor/caret=");number(sample);print("/");number(options);print("/");number(flag);print("/");real(width);print("/");number(a);print("/");number(p);print("\n");return false;}
            }
            const auto builds=layouts[0].builds;CHECK(text_edit_layout_measure(layouts,input,candidate)&&layouts[0].builds==builds);
            CHECK(text_measure_utf8_styled(&owner,utf8_literal("Other cached paragraph"),{},95,1000,{utf16,256}).ok()&&same_edit(layouts[0],layouts[1]));
            CHECK(text_edit_layout_scroll(layouts,-33)&&text_edit_layout_scroll(layouts+1,-33)&&same_edit(layouts[0],layouts[1]));
            CHECK(text_edit_layout_scroll(layouts,21)&&text_edit_layout_scroll(layouts+1,21)&&same_edit(layouts[0],layouts[1]));
        }
    }
    char long_text[4096];for(u32 i=0;i<4096;i+=4){long_text[i]=static_cast<char>(0xd7);long_text[i+1]=static_cast<char>(0x90);long_text[i+2]='A';long_text[i+3]=' ';}
    const u32 lengths[]{64,1024,4096,16};for(u32 length:lengths){const Utf8View text{long_text,length};const u32 units=length/4*3;
        TextEditLayoutInput input{text,text_edit_signature(text),0,units,9.125f,32.25f,95,40,1000000,7,{}};++cases;
        CHECK(text_edit_layout_measure(layouts,input,candidate)&&layouts[0].geometry&&!layouts[0].layout&&text_edit_layout_measure(layouts+1,input,reference)&&same_edit(layouts[0],layouts[1]));++native_cases;
    }
    print("directional_edit_storage_commit=");number(layouts[0].storage.arena.committed);print(" legacy_utf16_commit=");number(layouts[1].storage.arena.committed);print("\n");return true;
}
