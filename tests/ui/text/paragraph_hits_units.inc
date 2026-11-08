// Paragraph hit geometry of the native editor path, observed: the caret at every position and side, the points of a
// grid around each distinct caret box, and every range. A range's counted query must report the regions it needs.
inline NativeParagraphHitCluster hit_clusters[256];inline NativeParagraphHitLine hit_lines[257];inline float hit_advances[1280];inline NativeParagraphHitRun hit_visual_runs[256];
inline bool compare_hits(const AppSystem& system,Utf8View text,float width,TextWrapping wrapping,ConstSlice<TextStyleSpan> styles) noexcept {
    ++cases;NativeParagraphHits native{};NativeParagraphHitStorage storage{{hit_clusters,256},{hit_lines,257},{hit_advances,1280},&native};TextMeasureResult measured{};
    SfntFontLineParts parts[513];storage.styles={parts,513};storage.visual_runs={hit_visual_runs,256};
    CHECK(native_text_candidate_hits(system,{text,styles,width,1000,wrapping},storage,&measured));++native_cases;observe_measure(measured);
    u32 units=0;CHECK(utf8_utf16_position_for_byte_offset(text,text.bytes,&units));
    // A trailing caret, or a position inside the same cluster, repeats the previous box, whose points were just observed.
    u32 previous_box[4]{};bool have_box=false;
    for(u32 p=0;p<=units;++p)for(u32 trailing=0;trailing<2;++trailing){float x=0;NativeParagraphHitMetric m{};
        CHECK(native_paragraph_hit_position(native,p,trailing!=0,&m,&x)==SfntStatus::ok);observe_value(m);observe_value(x);
        u32 box[4];copy_bytes(box,&m.left,4);copy_bytes(box+1,&m.width,4);copy_bytes(box+2,&m.top,4);copy_bytes(box+3,&m.height,4);
        if(have_box&&bytes_equal(box,previous_box,sizeof(box)))continue;
        copy_bytes(previous_box,box,sizeof(box));have_box=true;
        const float xs[]{m.left-.001f,m.left,m.left+m.width*.5f,m.left+m.width,m.left+m.width+.001f,-1.f,1000000.f};
        const float ys[]{m.top-.001f,m.top,m.top+m.height*.5f,m.top+m.height,m.top+m.height+.001f,-1.f,1000000.f};
        for(float py:ys)for(float px:xs){NativeParagraphHitPoint point{};
            CHECK(native_paragraph_hit_point(native,px,py,&point)==SfntStatus::ok);const u32 value[2]{point.source_position,point.inside};observe_value(value);}
    }
    for(u32 a=0;a<=units;++a)for(u32 b=a;b<=units;++b){NativeParagraphHitMetric regions[256];u32 count=0,required=0;
        CHECK(native_paragraph_hit_range(native,a,b-a,3.125f,-17.3f,{regions,256},&count)==SfntStatus::ok);
        observe_value(count);observe_bytes(regions,count*sizeof(*regions));
        const auto status=native_paragraph_hit_range(native,a,b-a,0,0,{},&required);CHECK(required==count&&status==SfntStatus::output_too_small);
    }
    return true;
}
inline bool hit_guards() noexcept {
    {
        const auto split_text=utf8_literal("a\r\nb");
        NativeParagraphCluster split[]{{0,1,0,1,7,0},{1,2,1,0,0,native_paragraph_newline|(1u<<native_paragraph_style_shift)},
            {2,3,1,0,0,native_paragraph_newline|(2u<<native_paragraph_style_shift)},{3,4,1,1,7,3u<<native_paragraph_style_shift}};
        u32 scalars[4];u8 breaks[5];SfntPlacement advances[]{{7,0,0},{7,0,0}};
        SfntFontLineParts parts[]{{15,5},{15,5},{15,5},{15,5}};NativeParagraphRun split_runs[4];NativeParagraphLayout fitted{};
        const NativeParagraphShape prepared{4,2,4,{20,15}};
        CHECK(native_latin_paragraph_boundaries(split_text,{split,4},{{scalars,4},{breaks,5}})==SfntStatus::ok);
        CHECK((split[1].flags&native_paragraph_crlf_prefix)&&!(split[1].flags&native_paragraph_newline));
        CHECK(native_prepared_styled_paragraph_layout(prepared,{split,4},{advances,2},{parts,4},1,64,true,{split_runs,4},&fitted)==SfntStatus::ok);
        CHECK(fitted.line_count==2&&fitted.run_count==4&&fitted.height==40&&fitted.width==7);
        CHECK(split_runs[1].source_first==1&&split_runs[1].source_count==1&&split_runs[2].source_first==2&&split_runs[2].source_count==1&&split_runs[1].baseline==split_runs[2].baseline);
    }
    NativeParagraphShape shape{3,1,2,{20,15}};
    NativeParagraphCluster clusters[2]{{0,1,0,1,7,0},{1,3,1,0,0,native_paragraph_newline}};
    SfntPlacement placement{7,0,0};NativeParagraphRun runs[3];NativeParagraphLayout layout{};NativeParagraphHits hits{};
    NativeParagraphHitCluster hc[3];NativeParagraphHitLine hl[2];float ha[1];
    NativeParagraphHitStorage storage{{hc,3},{hl,2},{ha,1},&hits};
    const auto build=[&](NativeParagraphHitStorage* s,float width=40) noexcept {return native_prepared_paragraph_layout(shape,{clusters,2},{&placement,1},width,64,true,{runs,3},&layout,s);};
    CHECK(build(&storage)==SfntStatus::ok&&hits.clusters.count==3&&hits.lines.count==2&&hits.advances.count==1);
    {
        ConstSlice<float> values{};CHECK(native_paragraph_hit_advances(hits,0,&values)&&values.count==1&&values[0]==7);
        CHECK(native_paragraph_hit_advances(hits,1,&values)&&!values.count&&native_paragraph_hit_advances(hits,2,&values)&&!values.count);
        CHECK(!native_paragraph_hit_advances(hits,3,&values)&&!values.count&&!native_paragraph_hit_advances(hits,0,nullptr));
        const u32 saved=hc[1].advance_flags;hc[1].advance_flags=(saved&~native_hit_advance_mask)|2u;
        CHECK(!native_paragraph_hit_advances(hits,0,&values)&&!values.count);hc[1].advance_flags=saved;
        const u32 head=hc[0].advance_flags;hc[0].advance_flags=(head&~native_hit_advance_mask)|2u;
        CHECK(!native_paragraph_hit_advances(hits,0,&values)&&!values.count);hc[0].advance_flags=head;
    }
    for(u32 mode=0;mode<3;++mode){auto short_storage=storage;if(mode==0)short_storage.clusters.count=2;else if(mode==1)short_storage.lines.count=1;else short_storage.advances.count=0;
        CHECK(build(&short_storage)==SfntStatus::output_too_small&&!hits.lines.count&&!layout.line_count);}
    auto missing=storage;missing.output=nullptr;CHECK(build(&missing)==SfntStatus::invalid_input);
    missing=storage;missing.clusters.data=nullptr;CHECK(build(&missing)==SfntStatus::invalid_input);
    const u32 invalid_bits[]{0x7fc00000u,0x7f800000u,0xff800000u};
    for(u32 bits:invalid_bits){float bad=0;copy_bytes(&bad,&bits,4);CHECK(build(&storage,bad)==SfntStatus::invalid_input&&!hits.lines.count);}
    CHECK(build(&storage)==SfntStatus::ok);NativeParagraphHitMetric metric{};NativeParagraphHitPoint point{};float x=0;u32 count=0;
    CHECK(native_paragraph_hit_position(hits,4,false,&metric,&x)==SfntStatus::invalid_input);
    CHECK(native_paragraph_hit_position(hits,0,false,nullptr,&x)==SfntStatus::invalid_input);
    CHECK(native_paragraph_hit_position(hits,0,false,&metric,nullptr)==SfntStatus::invalid_input);
    CHECK(native_paragraph_hit_point(hits,0,0,nullptr)==SfntStatus::invalid_input);
    CHECK(native_paragraph_hit_range(hits,0,4,0,0,{&metric,1},&count)==SfntStatus::invalid_input);
    CHECK(native_paragraph_hit_range(hits,0,1,0,0,{nullptr,1},&count)==SfntStatus::invalid_input);
    CHECK(native_paragraph_hit_range(hits,0,1,0,0,{&metric,1},nullptr)==SfntStatus::invalid_input);
    for(u32 bits:invalid_bits){float bad=0;copy_bytes(&bad,&bits,4);
        CHECK(native_paragraph_hit_point(hits,bad,0,&point)==SfntStatus::invalid_input&&native_paragraph_hit_point(hits,0,bad,&point)==SfntStatus::invalid_input);
        CHECK(native_paragraph_hit_range(hits,0,1,bad,0,{&metric,1},&count)==SfntStatus::invalid_input);}
    NativeParagraphHits empty{};CHECK(native_paragraph_hit_position(empty,0,false,&metric,&x)==SfntStatus::invalid_input);
    auto excessive=shape;excessive.glyph_count=native_hit_advance_mask+1;
    CHECK(native_prepared_paragraph_layout(excessive,{clusters,2},{&placement,excessive.glyph_count},40,64,true,{runs,3},&layout,&storage)==SfntStatus::resource_limit&&!hits.lines.count);
    // Layouts with no glyphs still expose one typographic line. Editor-level
    // empty-input caret height remains the established separate UI policy.
    shape={0,0,0,{20,15}};CHECK(native_prepared_paragraph_layout(shape,{},{},40,64,true,{},&layout,&storage)==SfntStatus::ok);
    CHECK(hits.lines.count==1&&!hits.clusters.count&&native_paragraph_hit_position(hits,0,false,&metric,&x)==SfntStatus::ok&&metric.height==20&&x==0);
    return true;
}
inline bool paragraph_hits() noexcept {
    CHECK(hit_guards());
    const wchar_t* families[]{L"Segoe UI",L"Georgia",L"Consolas"};const float sizes[]{14.f,17.5f,1000.f},widths[]{8.f,37.f,95.f,1000000.f};
    for(u32 family=0;family<3;++family)for(float em:sizes){auto system=owner;CHECK(set_format(system,families[family],em));
        for(u32 independent=0;independent<2;++independent){system.independent_letters=independent!=0;
            for(u32 sample=0;sample<sizeof(paragraph_samples)/sizeof(paragraph_samples[0]);++sample)for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap)
                if(!compare_hits(system,paragraph_samples[sample],width==1000000?width:width*(em/16),nowrap?TextWrapping::no_wrap:TextWrapping::wrap,{})){
                    print("family/sample/em/width/independent/nowrap=");number(family);print(",");number(sample);print(",");real(em);print(",");real(width);print(",");number(independent);print(",");number(nowrap);print("\n");return false;}
        }
    }return true;
}
