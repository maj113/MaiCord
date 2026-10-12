bool native_text_candidate_prefers_multiline(const TextSystem& system,Utf8View text,ConstSlice<TextStyleSpan> styles,TextWrapping wrapping) noexcept {
    if(!system.native_text)return false;const auto& owner=*system.native_text;
    if(!owner.last_valid||!owner.prefer_multiline||owner.last_independent!=system.independent_letters||owner.last_tracking!=system.tracking||
       owner.last_text.count!=text.bytes||!text.data||!bytes_equal(owner.last_text.data,text.data,text.bytes))return false;
    if(owner.last_styled){if(owner.last_style_em!=system.em_size||owner.last_fixed_weight!=system.fixed_weight||owner.last_styles.count!=styles.count||
        (styles.count&&(!styles.data||!bytes_equal(owner.last_styles.data,styles.data,styles.count*sizeof(TextStyleSpan)))))return false;}
    else if(styles.count)return false;
    return resolve_format(system,wrapping)==owner.last_format;
}
bool native_text_candidate_trimmed_once(const TextSystem& system,Utf8View text,ConstSlice<TextStyleSpan> styles,float width,float height,
    ShapedText* output,TextWrapping wrapping,ConstSlice<TextHitRange> hits) noexcept {
    CatalogOperation catalog_operation(system.native_text);
    if((styles.count&&!styles.data)||styles.count>kTextStyleSpanMaximum||!output||output->run_count||output->native_fonts()||!output->runs||!output->glyphs||!output->clusters)return false;
    bool geometry=hits.count!=0;for(u32 i=0;i<styles.count;++i)geometry|=(styles[i].flags&native_decoration_flags)!=0;
    NativeTrimPlanScratch scratch{};NativeParagraphLayout layout{};
    if(!prepare_trim_plan(system,text,styles,width,height,wrapping,{},&layout,&scratch,geometry))return false;
    if(layout.line_count<2){system.native_text->prefer_multiline=false;return false;}
    auto& owner=*system.native_text;const auto* format=resolve_format(system,wrapping);if(!format)return false;
    const auto* sign=omission_sign(owner,format);if(!sign)return false;
    const auto& shape=owner.paragraph_shape;const auto& s=owner.paragraph_storage;auto fit=scratch.fit;
    const bool styled=owner.last_styled,directional=styled&&(owner.last_mode&48)!=0;
    if(geometry&&!styled)return false;
    struct RestoreLines {NativeTextContext& owner;Slice<NativeParagraphLine> previous;~RestoreLines() noexcept {owner.styled_lines=previous;}} restore_lines{owner,owner.styled_lines};
    Slice<NativeStyledOmission> omissions{};u32 omission_count=0;
    if(geometry){owner.styled_lines=take<NativeParagraphLine>(owner.scratch,layout.line_count);omissions=take<NativeStyledOmission>(owner.scratch,layout.line_count);if(!owner.styled_lines.data||!omissions.data)return false;}
    auto* plain=styled?nullptr:binding(owner,format);if(!styled&&!plain)return false;
    if(shape.source_units>output->cluster_capacity)return output_capacity_failure(owner);
    const auto span_for=[&](u32 source) noexcept {return styled?decoration_span(owner,source):0u;};
    const auto font_for=[&](u32 run) noexcept {return styled?owner.styled_spans[span_for(owner.paragraph_runs[run].source_first)].binding()->font:plain->font;};
    for(u32 i=0;i<shape.source_units;++i)output->clusters[i]=0xffffu;
    u32 source_run=0,written_runs=0,written_glyphs=0,sign_count=0;
    RtlLineOutput rtl{owner,output};rtl.source_gaps=true;
    for(u32 line_index=0;line_index<layout.line_count;++line_index){auto& line=fit[line_index];
        if(line.flags&native_line_fit_hidden){if(geometry)owner.styled_lines[line_index]={static_cast<u16>(written_runs),0,line.top,0,0};continue;}
        const u32 first=line.first_cluster,end=first+line.cluster_count;
        const u32 source_end=line.cluster_count?s.clusters[end-1].source_end:shape.source_units;
        u32 full_end=source_run;while(full_end<layout.run_count&&owner.paragraph_runs[full_end].source_first<source_end)++full_end;
        u32 content_end=end;while(content_end>first&&(s.clusters[content_end-1].flags&(native_paragraph_newline|native_paragraph_crlf_prefix)))--content_end;
        const u32 newline_source=content_end<end?s.clusters[content_end].source_first:source_end;
        u32 controls=source_run;while(controls<full_end&&owner.paragraph_runs[controls].source_first<newline_source)++controls;
        const u32 visible_end=first+line.visible_clusters;
        NativeParagraphShape prefix=shape;prefix.cluster_count=line.visible_clusters;
        prefix.source_units=line.visible_clusters?s.clusters[visible_end-1].source_end:(first<shape.cluster_count?s.clusters[first].source_first:shape.source_units);
        prefix.glyph_count=line.visible_clusters?s.clusters[visible_end-1].glyph_first+s.clusters[visible_end-1].glyph_count:(first<shape.cluster_count?s.clusters[first].glyph_first:shape.glyph_count);
        if(written_runs>controls)return false;
        NativeParagraphLayout placed{};const bool intact=visible_end==content_end;
        if(intact){placed.run_count=controls-source_run;
            for(u32 i=0;i<placed.run_count;++i)owner.paragraph_runs[written_runs+i]=owner.paragraph_runs[source_run+i];
        }else{
            const NativeParagraphLineSink boundary{&rtl,nullptr,{},directional?split_rtl_fallback_space:nullptr};
            const auto run_space=Slice<NativeParagraphRun>{owner.paragraph_runs.data+written_runs,controls-written_runs};
            const auto status=styled?native_prepared_styled_paragraph_layout(prefix,{s.clusters.data+first,prefix.cluster_count},{s.placements.data,prefix.glyph_count},
                {owner.styled_parts.data,owner.styled_parts.count},width,format->tab_stop,false,run_space,&placed,{},nullptr,boundary):
                native_prepared_paragraph_layout(prefix,{s.clusters.data+first,prefix.cluster_count},{s.placements.data,prefix.glyph_count},width,format->tab_stop,false,run_space,&placed);
            if(status!=SfntStatus::ok)return false;
        }
        const u32 run_first=written_runs;written_runs+=placed.run_count;
        const bool omitted=(line.flags&native_line_fit_sign)!=0;
        const float baseline=decoration_add(line.top,line.baseline);
        if(!intact)for(u32 i=run_first;i<written_runs;++i){auto& r=owner.paragraph_runs[i];const float own=styled?owner.styled_parts[span_for(r.source_first)].baseline:line.baseline;
            r.baseline=decoration_add(decoration_add(baseline,-own),own);}
        const auto* last=placed.run_count?owner.paragraph_runs.data+written_runs-1:nullptr;
        float sign_x=last?decoration_add(last->x,last->width):0;
        for(u32 i=controls;i<full_end;++i){auto control=owner.paragraph_runs[i];if(omitted)control.x=line.extent;owner.paragraph_runs[written_runs++]=control;}
        source_run=full_end;
        if(written_runs>output->run_capacity)return output_capacity_failure(owner);
        if(directional){
            rtl.omission=omitted;rtl.omission_width=sign->width;rtl.omission_x=0;rtl.omission_before=placed.run_count;rtl.zeroes={};
            const u32 span=minimum(span_for(prefix.source_units),owner.styled_spans.count-1);
            rtl.omission_level=owner.last_mode&32?static_cast<u8>(owner.styled_analysis[span]):static_cast<u8>(1);
            if(omitted&&geometry){u32 limit=span;while(limit<owner.styled_spans.count&&owner.styled_spans[limit].first_unit<newline_source)++limit;
                auto zeroes=take<NativeOmissionZero>(owner.scratch,limit-span);if(limit>span&&!zeroes.data)return false;u32 count=0;
                for(u32 i=span;i<limit;++i){const auto& part=owner.styled_spans[i];const u8 level=owner.last_mode&32?static_cast<u8>(owner.styled_analysis[i]):static_cast<u8>(1);
                    const u32 zero_end=minimum(static_cast<u32>(part.end_unit),newline_source);
                    if(count&&zeroes[count-1].level==level)zeroes[count-1].end=zero_end;else zeroes[count++]={maximum(prefix.source_units,static_cast<u32>(part.first_unit)),zero_end,0,level};}
                rtl.zeroes={zeroes.data,count};}
            u32 trailing=visible_end;while(trailing>first&&owner.styled_reset[trailing-1])--trailing;
            const u32 trailing_source=trailing<visible_end?s.clusters[trailing].source_first:prefix.source_units;
            if(publish_rtl_line(&rtl,{owner.paragraph_runs.data+run_first,written_runs-run_first},run_first,trailing_source)!=SfntStatus::ok)return false;
            written_glyphs=rtl.written;if(omitted)sign_x=rtl.omission_x;
            // Mandatory controls keep their independently captured metric origin.
            for(u32 i=run_first+placed.run_count;i<written_runs;++i)output->runs[i].baseline_x=output->runs[i].baseline_y=owner.paragraph_runs[i].x;
        }else for(u32 i=run_first;i<written_runs;++i){const auto& r=owner.paragraph_runs[i];
            if(r.glyph_count>output->glyph_capacity-written_glyphs||written_glyphs+r.glyph_count>65535)return output_capacity_failure(owner);
            for(u32 g=0;g<r.glyph_count;++g){const auto& p=s.placements[r.glyph_first+g];output->glyphs[written_glyphs+g]={p.advance,p.advance_offset,p.ascender_offset,s.glyphs[r.glyph_first+g],0};}
            for(u32 u=0;u<r.source_count;++u){const u32 glyph=s.source_glyphs[r.source_first+u];output->clusters[r.source_first+u]=glyph==0xffffffffu?0xffffu:static_cast<u16>(written_glyphs+glyph-r.glyph_first);}
            output->runs[i].first_glyph=written_glyphs;output->runs[i].glyph_count=r.glyph_count;output->runs[i].baseline_x=r.x;output->runs[i].bidi_level=0;written_glyphs+=r.glyph_count;
        }
        if(omitted&&written_runs>run_first+placed.run_count){float edge=decoration_add(sign_x,sign->width);
            if(directional)for(u32 i=run_first;i<run_first+placed.run_count;++i)edge=maximum(edge,decoration_add(output->runs[i].baseline_y,owner.paragraph_runs[i].width));
            for(u32 i=run_first+placed.run_count;i<written_runs;++i){owner.paragraph_runs[i].x=edge;if(directional)output->runs[i].baseline_y=edge;}
        }
        if(geometry){const auto* last_run=written_runs>run_first?owner.paragraph_runs.data+written_runs-1:nullptr;
            owner.styled_lines[line_index]={static_cast<u16>(run_first),static_cast<u16>(written_runs-run_first),line.top,line.height,last_run?decoration_add(last_run->x,last_run->width):0};
            if(omitted){ConstSlice<NativeOmissionZero> zeroes{rtl.zeroes.data,rtl.zeroes.count};
                if(!directional){auto single=take<NativeOmissionZero>(owner.scratch,1);if(!single.data)return false;single[0]={prefix.source_units,newline_source,decoration_add(sign_x,sign->width),0};zeroes={single.data,1};}
                omissions[omission_count++]={prefix.source_units,sign_x,sign->width,line.height,directional?rtl.omission_visual:placed.run_count,zeroes,line_index,newline_source};}}
        if(omitted){++sign_count;line.width=sign_x;}
    }
    if(sign_count>output->run_capacity-written_runs||static_cast<u64>(sign_count)*sign->glyphs>output->glyph_capacity-written_glyphs||
        static_cast<u64>(written_glyphs)+static_cast<u64>(sign_count)*sign->glyphs>65535)return output_capacity_failure(owner);
    u32 decoration_count=0,hit_count=0;
    if(geometry){auto rendered=layout;rendered.run_count=written_runs;const ConstSlice<NativeStyledOmission> hidden{omissions.data,omission_count};
        if(!native_styled_decorations(owner,rendered,width,format->tab_stop,{output->decorations,output->decoration_capacity},&decoration_count,directional?output:nullptr,nullptr,hidden))return false;
        DecorationGraphemeCursor boundaries{};
        for(u32 i=0;i<hits.count;++i){const auto& hit=hits[i];if(!hit.unit_count||hit.first_unit>shape.source_units||hit.unit_count>shape.source_units-hit.first_unit)return false;
            if(!decoration_range(owner,rendered,format->tab_stop,hit.first_unit,hit.first_unit+hit.unit_count,
                [&](const NativeParagraphLine& line,float left,float span_width) noexcept {
                    if(!(span_width>0)||!(line.height>0))return true;if(!output->hit_regions||hit_count==output->hit_region_capacity)return false;
                    output->hit_regions[hit_count++]={left,line.top,decoration_add(left,span_width),decoration_add(line.top,line.height),hit.semantic_index,0};return true;
                },&boundaries,directional?output:nullptr,nullptr,hidden))return false;
        }
    }
    if(!native_font_session_retain(owner.fonts))return false;
    auto* store=native_font_session_runs(owner.fonts);u32 retained=0,retained_signs=0;
    while(retained<written_runs&&native_font_run_retain(store,font_for(retained)))++retained;
    if(retained==written_runs)while(retained_signs<sign_count&&native_font_run_retain(store,sign->font))++retained_signs;
    if(retained!=written_runs||retained_signs!=sign_count){while(retained){--retained;native_font_run_release(store,font_for(retained));}while(retained_signs){--retained_signs;native_font_run_release(store,sign->font);}auto* session=owner.fonts;native_font_session_release(&session);return false;}
    output->set_native_fonts(owner.fonts);
    for(u32 i=0;i<written_runs;++i){const auto& r=owner.paragraph_runs[i];const auto* style=styled?owner.styled_spans.data+span_for(r.source_first):nullptr;const auto previous=output->runs[i];
        output->runs[i]={font_for(i),previous.baseline_x,r.baseline,style?style->em:format->em,previous.first_glyph,previous.glyph_count,r.source_first,r.source_count,style?style->flags:0,previous.bidi_level,false,true};}
    for(u32 i=0;i<layout.line_count;++i){const auto& line=fit[i];if(!(line.flags&native_line_fit_sign))continue;
        copy_bytes(output->glyphs+written_glyphs,sign+1,sign->glyphs*sizeof(TextGlyph));const float baseline=decoration_add(line.top,line.baseline);
        output->runs[written_runs]={sign->font,line.width,decoration_add(decoration_add(baseline,-sign->baseline),sign->baseline),format->em,written_glyphs,sign->glyphs,0,1,0,0,false,true};
        output->clusters[0]=static_cast<u16>(written_glyphs);written_glyphs+=sign->glyphs;++written_runs;
    }
    output->decoration_count=decoration_count;output->hit_region_count=hit_count;output->glyph_count=written_glyphs;output->run_count=written_runs;output->cluster_count=shape.source_units;output->width=layout.width;output->width_including_trailing_whitespace=layout.extent;output->height=layout.height;output->line_count=layout.line_count;
    owner.prefer_multiline=true;++owner.shaped;++owner.paragraphs_shaped;return true;
}
