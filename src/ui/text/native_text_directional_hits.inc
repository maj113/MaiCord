bool native_directional_hits(const NativeTextContext& owner,const NativeParagraphLayout& layout,NativeParagraphHitStorage& storage) noexcept {
    if(storage.required_visual_runs)*storage.required_visual_runs=layout.run_count;
    if(layout.run_count>65535||layout.run_count>storage.visual_runs.count||(layout.run_count&&!storage.visual_runs.data))return false;
    auto& hits=*storage.output;u32 logical_run=0,shape_first=0,span=0;DecorationGraphemeCursor boundaries{};
    const auto& clusters=owner.paragraph_storage.clusters;const bool mixed=(owner.last_mode&32)!=0;
    for(u32 line=0;line<hits.lines.count;++line){auto& l=storage.lines[line];if(!l.cluster_count)continue;
        const u32 hit_end=static_cast<u32>(l.first_cluster)+l.cluster_count;if(hit_end>hits.clusters.count)return false;
        const u32 source_end=storage.clusters[hit_end-1].source_end;u32 shape_end=shape_first;
        while(shape_end<owner.paragraph_shape.cluster_count&&clusters[shape_end].source_end<=source_end)++shape_end;
        u32 trailing=shape_end;while(trailing>shape_first&&owner.styled_reset[trailing-1])--trailing;
        const u32 trailing_source=trailing<shape_end?clusters[trailing].source_first:source_end;
        const u32 first_run=logical_run;u32 hit=l.first_cluster,highest=0,lowest=255;
        while(logical_run<layout.run_count&&owner.paragraph_runs[logical_run].source_first<source_end){const auto& run=owner.paragraph_runs[logical_run];
            while(span+1<owner.styled_spans.count&&owner.styled_spans[span].end_unit<=run.source_first)++span;
            const u32 level=run.source_first>=trailing_source?0:mixed?owner.styled_analysis[span]&255u:1;
            const u32 first=hit,limit=static_cast<u32>(run.source_first)+run.source_count;
            while(hit<hit_end&&storage.clusters[hit].source_first<limit){auto& c=storage.clusters[hit];
                const u32 style=c.style;if(style>=owner.styled_spans.count)return false;
                if(c.source_end>owner.styled_spans[style].end_unit){u32 last=style+1;
                    while(last<owner.styled_spans.count&&owner.styled_spans[last].end_unit<c.source_end)++last;
                    if(last==owner.styled_spans.count)return false;
                    if(owner.styled_spans[last].end_unit==c.source_end||decoration_terminal_boundary(owner,c.source_end,boundaries)==1)c.advance_flags|=native_hit_terminal_region;
                }
                ++hit;
            }
            if(first==hit||storage.clusters[first].source_first!=run.source_first||storage.clusters[hit-1].source_end!=limit)return false;
            float width=run.glyph_count?0:run.width;
            for(u32 g=0;g<run.glyph_count;++g)width=decoration_add(width,owner.paragraph_storage.placements[run.glyph_first+g].advance);
            storage.visual_runs[logical_run++]={static_cast<u16>(first),static_cast<u16>(hit-first),0,width,level};
            highest=maximum(highest,level);if(level&1)lowest=minimum(lowest,level);
        }
        if(hit!=hit_end)return false;
        // Reverse the run records per level (UAX #9 L2) in place; no
        // per-cluster visual permutation or index array is kept.
        for(u32 level=highest;level>=lowest;--level)for(u32 i=first_run;i<logical_run;){
            if(storage.visual_runs[i].bidi_level<level){++i;continue;}const u32 begin=i;
            while(i<logical_run&&storage.visual_runs[i].bidi_level>=level)++i;
            for(u32 a=begin,b=i-1;a<b;++a,--b){const auto value=storage.visual_runs[a];storage.visual_runs[a]=storage.visual_runs[b];storage.visual_runs[b]=value;}
        }
        float pen=0;
        for(u32 i=first_run;i<logical_run;++i){auto& run=storage.visual_runs[i];run.left=pen;pen=decoration_add(pen,run.width);float prefix=0,point_pen=(run.bidi_level&1)?pen:run.left;
            const u32 limit=static_cast<u32>(run.first_cluster)+run.cluster_count;
            for(u32 c=run.first_cluster;c<limit;++c){auto& item=storage.clusters[c];
                // In the directional view left stores the logical leading
                // edge: the right edge for RTL. Queries derive physical boxes.
                item.left=(run.bidi_level&1)?decoration_sub(pen,prefix):decoration_add(run.left,prefix);
                item.style=(item.style&65535u)|(i<<16);
                point_pen=(run.bidi_level&1)?decoration_sub(point_pen,item.width):decoration_add(point_pen,item.width);item.point_edge=point_pen;
                ConstSlice<float> advances{};if(!native_paragraph_hit_advances(hits,c,&advances))return false;
                for(u32 g=0;g<advances.count;++g)prefix=decoration_add(prefix,advances[g]);
            }
        }l.caret_end=pen;shape_first=shape_end;
    }
    if(logical_run!=layout.run_count)return false;hits.visual_runs={storage.visual_runs.data,layout.run_count};return true;
}
