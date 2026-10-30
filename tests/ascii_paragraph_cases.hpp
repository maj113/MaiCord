// Included inside the native run-plan test namespace. These cases check the
// retained-layout contract independently of font files and the Windows oracle.
bool latin_boundary_cases() noexcept {
    {
        // Paragraph shaping consumes cluster widths, not standalone caret or
        // whitespace geometry. Keep identical float reductions and rejection
        // of nonmonotone, undersized and nonfinite shaped output.
        const SfntFontLayoutMetrics metrics{1000,800,200};
        const auto text=utf8_literal("e\xcc\x81\xf0\x9f\x98\x80" "A ");
        u32 map[]{0,0,1,1,2,3};SfntPlacement placements[]{{.1f,0,0},{1.23f,0,0},{3,0,0},{2,0,0}};
        NativeLineCluster complete[4]{},compact[4]{};NativeAsciiLineStorage storage{};
        storage.run.character_glyphs={map,6};storage.run.placements={placements,4};
        const auto collect=[&](bool paragraph,NativeAsciiLineResult& result,u32 capacity=4) noexcept {
            result={};result.run={4,6,1,0,0};storage.clusters={paragraph?compact:complete,capacity};
            return native_unicode_line_geometry(metrics,text,10,storage,&result,paragraph?NativeLineText::paragraph:NativeLineText::unicode);
        };
        NativeAsciiLineResult full{},part{};
        CHECK(collect(false,full)==SfntStatus::ok&&collect(true,part)==SfntStatus::ok);
        CHECK(full.cluster_count==4&&part.cluster_count==4&&bytes_equal(complete,compact,sizeof(complete)));
        CHECK(full.geometry_flags==native_line_ordered_x&&part.geometry_flags==full.geometry_flags);
        CHECK(full.trailing_source_units==1&&!part.trailing_source_units&&full.caret_end>0&&!part.caret_end);
        CHECK(full.vertical.height>0&&!part.vertical.height);
        CHECK(collect(true,part,3)==SfntStatus::output_too_small&&collect(false,full,3)==SfntStatus::output_too_small);
        map[2]=0;map[3]=0;map[4]=2;map[5]=1;
        CHECK(collect(true,part)==SfntStatus::unsupported_sequence&&collect(false,full)==SfntStatus::unsupported_sequence);
        map[2]=map[3]=1;map[4]=2;map[5]=3;placements[0].advance=-1;
        CHECK(collect(true,part)==SfntStatus::ok&&collect(false,full)==SfntStatus::ok&&!part.geometry_flags&&!full.geometry_flags);
        const u32 infinity=0x7f800000u;copy_bytes(&placements[0].advance,&infinity,4);
        CHECK(collect(true,part)==SfntStatus::resource_limit&&collect(false,full)==SfntStatus::resource_limit);
    }
    {
        // Collection can defer analysis without accessing boundary scratch.
        // Final analysis must recover the same CRLF/tab/bidi source policy;
        // malformed input and collection-capacity failures still reject.
        const SfntFontLayoutMetrics metrics{1000,800,200};
        const auto controls=utf8_literal("\r\n\t\xe2\x80\xaa\xe2\x80\xab\xe2\x80\xac\xe2\x80\xa9");
        NativeParagraphCluster a[8]{},b[8]{};u32 am[8]{},bm[8]{},scalars[8]{};u8 flags[9]{};
        NativeAsciiParagraphStorage as{},bs{};as.clusters={a,8};bs.clusters={b,8};as.source_glyphs={am,8};bs.source_glyphs={bm,8};
        NativeParagraphShape expected{},deferred{};bool nominal=true;
        const NativeParagraphLineSource no_printable{nullptr,[](void*,Utf8View,float,const NativeAsciiLineStorage&,NativeAsciiLineResult*) noexcept {return SfntStatus::unsupported_sequence;}};
        const NativeLatinBoundaryStorage complete{{scalars,8},{flags,9}},later{{},{},true};
        CHECK(!complete.deferred&&later.deferred);
        CHECK(native_script_paragraph_shape(metrics,controls,10,as,complete,&expected,no_printable)==SfntStatus::ok);
        CHECK(native_script_paragraph_shape(metrics,controls,10,bs,later,&deferred,no_printable,&nominal)==SfntStatus::ok&&!nominal);
        CHECK(deferred.source_units==expected.source_units&&deferred.cluster_count==expected.cluster_count&&!deferred.glyph_count);
        // Explicit analysis runs even if its storage carries the deferred bit.
        auto final=complete;final.deferred=true;
        CHECK(native_latin_paragraph_boundaries(controls,{b,deferred.cluster_count},final)==SfntStatus::ok);
        CHECK(bytes_equal(a,b,expected.cluster_count*sizeof(*a))&&bytes_equal(am,bm,expected.source_units*sizeof(*am)));
        CHECK(native_script_paragraph_shape(metrics,controls,10,bs,{},&deferred,no_printable)==SfntStatus::output_too_small&&!deferred.source_units);
        CHECK(native_script_paragraph_shape(metrics,utf8_literal("\xc0"),10,bs,later,&deferred,no_printable)==SfntStatus::invalid_input&&!deferred.source_units);
        bs.clusters.count=0;CHECK(native_script_paragraph_shape(metrics,controls,10,bs,later,&deferred,no_printable)==SfntStatus::output_too_small&&!deferred.source_units);
    }
    const auto text=utf8_literal("\xf0\x90\x9e\x80" "e\xcc\x81 \xf0\x90\x9e\x80\r\n");
    NativeParagraphCluster clusters[]{{0,2,0,1,6,0},{2,4,1,1,5,0},{4,5,2,1,2,0},{5,7,3,1,6,0},{7,9,4,0,0,native_paragraph_newline}};
    u32 scalars[8];u8 breaks[9];breaks[8]=71;
    NativeLatinBoundaryStorage storage{{scalars,7},{breaks,8}};
    CHECK(native_latin_paragraph_boundaries(text,{clusters,5},storage)==SfntStatus::ok&&breaks[8]==71);
    CHECK(!clusters[0].flags&&!clusters[1].flags&&clusters[2].flags==native_paragraph_space&&clusters[3].flags==native_paragraph_break_before&&clusters[4].flags==native_paragraph_newline);
    auto small=storage;small.scalar_count=6;CHECK(native_latin_paragraph_boundaries(text,{clusters,5},small)==SfntStatus::output_too_small);
    small=storage;small.breaks.count=7;CHECK(native_latin_paragraph_boundaries(text,{clusters,5},small)==SfntStatus::output_too_small);
    small=storage;small.scalar_data=nullptr;CHECK(native_latin_paragraph_boundaries(text,{clusters,5},small)==SfntStatus::invalid_input);
    CHECK(native_latin_paragraph_boundaries({nullptr,1},{clusters,5},storage)==SfntStatus::invalid_input);
    CHECK(native_latin_paragraph_boundaries(utf8_literal("\xf0\x90"),{clusters,5},storage)==SfntStatus::invalid_input);
    clusters[0].source_end=1;clusters[1].source_first=1;CHECK(native_latin_paragraph_boundaries(text,{clusters,5},storage)==SfntStatus::unsupported_sequence);
    CHECK(native_latin_paragraph_boundaries({}, {},{{},{breaks,1}})==SfntStatus::ok&&breaks[0]==0);
    const auto selectors=utf8_literal(" \xef\xb8\x8f\xef\xb8\x8e" "A\xef\xb8\x8f");
    NativeParagraphCluster split_selectors[]{{0,1,0,1,2,0},{1,2,1,1,7,0},{2,3,2,1,7,0},{3,4,3,1,8,0},{4,5,4,1,7,0}};
    CHECK(native_latin_paragraph_boundaries(selectors,{split_selectors,5},storage)==SfntStatus::ok);
    CHECK((split_selectors[0].flags&native_paragraph_space)&&(split_selectors[1].flags&native_paragraph_space));
    CHECK(!(split_selectors[2].flags&native_paragraph_space)&&!(split_selectors[3].flags&native_paragraph_space)&&!(split_selectors[4].flags&native_paragraph_space));
    CHECK(split_selectors[1].glyph_count==1&&split_selectors[1].width==7);
    return true;
}
bool paragraph_line_sink_cases() noexcept {
    NativeParagraphShape shape{3,2,3,{10,7}};
    const NativeParagraphCluster clusters[]{{0,1,0,1,4,0},{1,2,1,1,2,native_paragraph_space},{2,3,2,0,0,native_paragraph_newline}};
    const SfntPlacement placements[]{{4,0,0},{2,0,0}};const SfntFontLineParts style{7,3};
    NativeParagraphRun runs[4]{};runs[3].source_first=71;NativeParagraphLayout result{};
    NativeParagraphHitCluster hit_clusters[3];NativeParagraphHitLine hit_lines[2];float hit_advances[2];SfntFontLineParts hit_style{};
    NativeParagraphHits hits{};NativeParagraphHitStorage storage{{hit_clusters,3},{hit_lines,2},{hit_advances,2},&hits,{&hit_style,1}};
    struct State {const NativeParagraphRun* base;u32 calls,fail_at,first[2],count[2],trailing[2];bool valid;};
    State state{runs,0,2,{},{},{},true};
    const NativeParagraphLineSink sink{&state,[](void* opaque,ConstSlice<NativeParagraphRun> line_runs,u32 first,u32 trailing) noexcept {
        auto& s=*static_cast<State*>(opaque);if(s.calls==2)return SfntStatus::invalid_input;const u32 call=s.calls++;
        s.first[call]=first;s.count[call]=line_runs.count;s.trailing[call]=trailing;
        s.valid=s.valid&&(!line_runs.count||line_runs.data==s.base+first);
        if(line_runs.count==3)s.valid=s.valid&&line_runs[0].width==4&&line_runs[1].width==2&&line_runs[2].source_first==2;
        return call==s.fail_at?SfntStatus::resource_limit:SfntStatus::ok;}};
    const auto fit=[&](u32 capacity=4) noexcept {return native_prepared_styled_paragraph_layout(shape,{clusters,3},{placements,2},{&style,1},100,16,true,{runs,capacity},&result,{},&storage,sink);};
    CHECK(fit()==SfntStatus::ok&&state.valid&&state.calls==2&&state.first[0]==0&&state.count[0]==3&&state.trailing[0]==1);
    CHECK(state.first[1]==3&&!state.count[1]&&state.trailing[1]==3&&result.line_count==2&&hits.lines.count==2&&runs[3].source_first==71);
    for(u32 failure=0;failure<2;++failure){state.calls=0;state.fail_at=failure;
        CHECK(fit()==SfntStatus::resource_limit&&state.calls==failure+1&&!result.run_count&&!result.line_count&&!hits.lines.count);}
    state.calls=0;state.fail_at=2;CHECK(fit(2)==SfntStatus::output_too_small&&!state.calls&&!result.run_count&&!hits.lines.count);
    CHECK(fit()==SfntStatus::ok&&state.calls==2&&state.valid&&runs[3].source_first==71);
    const u8 reset[]{0,0,1};auto explicit_reset=sink;explicit_reset.trailing_reset={reset,3};state.calls=0;
    CHECK(native_prepared_styled_paragraph_layout(shape,{clusters,3},{placements,2},{&style,1},100,16,true,{runs,4},&result,{},&storage,explicit_reset)==SfntStatus::ok);
    CHECK(state.calls==2&&state.trailing[0]==2&&state.count[0]==2&&runs[0].width==6&&result.width==4&&result.extent==6);
    state.calls=0;explicit_reset.trailing_reset.count=2;
    CHECK(native_prepared_styled_paragraph_layout(shape,{clusters,3},{placements,2},{&style,1},100,16,true,{runs,4},&result,{},&storage,explicit_reset)==SfntStatus::invalid_input&&!state.calls&&!result.line_count&&!hits.lines.count);
    explicit_reset.trailing_reset={nullptr,3};
    CHECK(native_prepared_styled_paragraph_layout(shape,{clusters,3},{placements,2},{&style,1},100,16,true,{runs,4},&result,{},&storage,explicit_reset)==SfntStatus::invalid_input&&!state.calls&&!result.line_count&&!hits.lines.count);
    state.calls=0;shape={0,0,0,{10,7}};
    CHECK(native_prepared_styled_paragraph_layout(shape,{},{},{&style,1},100,16,true,{},&result,{},nullptr,sink)==SfntStatus::ok);
    CHECK(state.calls==1&&!state.first[0]&&!state.count[0]&&!state.trailing[0]&&result.line_count==1&&!result.run_count);
    return true;
}
bool styled_paragraph_cases() noexcept {
    CHECK(paragraph_line_sink_cases());
    const SfntFontLineParts styles[]{{7,3},{10,2},{6,6}};
    NativeParagraphShape shape{4,4,4,{10,7}};NativeParagraphCluster clusters[]{
        {0,1,0,1,4,0},{1,2,1,1,3,1u<<native_paragraph_style_shift},
        {2,3,2,1,2,(2u<<native_paragraph_style_shift)|native_paragraph_space},
        {3,4,3,1,5,(2u<<native_paragraph_style_shift)|native_paragraph_break_before}};
    SfntPlacement placements[]{{4,0,0},{3,0,0},{2,0,0},{5,0,0}};NativeParagraphRun runs[8];NativeParagraphLayout out{};
    const auto build=[&](float width,u32 capacity=8) noexcept {return native_prepared_styled_paragraph_layout(shape,{clusters,4},{placements,4},{styles,3},width,16,true,{runs,capacity},&out);};
    CHECK(build(100)==SfntStatus::ok&&out.height==16&&out.line_count==1&&out.run_count==3&&out.width==14&&out.extent==14);
    CHECK(runs[0].baseline==10&&runs[1].baseline==10&&runs[2].baseline==10&&runs[0].x==0&&runs[1].x==4&&runs[2].x==7);
    CHECK(build(8)==SfntStatus::ok&&out.height==28&&out.line_count==2&&out.run_count==4&&out.width==7&&out.extent==9&&!out.flags);
    CHECK(runs[2].source_count==1&&runs[2].x==7&&runs[3].x==0&&runs[3].baseline==22);
    NativeParagraphLine lines[3];lines[2]={71,72,73,74,75};
    CHECK(native_prepared_styled_paragraph_layout(shape,{clusters,4},{placements,4},{styles,3},8,16,true,{runs,8},&out,{lines,2})==SfntStatus::ok);
    CHECK(lines[0].first_run==0&&lines[0].run_count==3&&lines[0].top==0&&lines[0].height==16&&lines[0].extent==9);
    CHECK(lines[1].first_run==3&&lines[1].run_count==1&&lines[1].top==16&&lines[1].height==12&&lines[1].extent==5&&lines[2].first_run==71&&lines[2].extent==75);
    const NativeParagraphGeometry geometry{{clusters,4},{placements,4},{runs,out.run_count},{lines,out.line_count},4,16};
    NativeParagraphHitMetric ranges[2];u32 range_count=0;
    struct Visitor {u32 calls;float width;bool accept;};Visitor visitor{};
    const NativeParagraphRangeSink sink{&visitor,[](void* opaque,const NativeParagraphHitMetric& value) noexcept {
        auto& state=*static_cast<Visitor*>(opaque);++state.calls;state.width+=value.width;return state.accept;}};
    CHECK(native_paragraph_geometry_visit(geometry,0,4,sink,&range_count)==SfntStatus::output_too_small&&range_count==2&&visitor.calls==1&&visitor.width==9);
    visitor={0,0,true};CHECK(native_paragraph_geometry_visit(geometry,0,4,sink,&range_count)==SfntStatus::ok&&range_count==2&&visitor.calls==2&&visitor.width==14);
    CHECK(native_paragraph_geometry_visit(geometry,0,4,{},&range_count)==SfntStatus::ok&&range_count==2);
    CHECK(native_paragraph_geometry_visit(geometry,3,2,sink,&range_count)==SfntStatus::invalid_input&&!range_count&&visitor.calls==2);
    CHECK(native_paragraph_geometry_visit(geometry,0,4,sink,nullptr)==SfntStatus::invalid_input&&visitor.calls==2);
    struct TerminalRegions {NativeParagraphHitMetric values[3];u32 count;};TerminalRegions terminal{};
    const NativeParagraphRangeSink terminal_sink{&terminal,[](void* opaque,const NativeParagraphHitMetric& value) noexcept {
        auto& state=*static_cast<TerminalRegions*>(opaque);if(state.count==3)return false;state.values[state.count++]=value;return true;}};
    CHECK(native_paragraph_geometry_visit(geometry,0,3,terminal_sink,&range_count,true)==SfntStatus::ok&&range_count==2&&terminal.count==2);
    CHECK(terminal.values[0].source_first==0&&terminal.values[0].source_count==3&&terminal.values[0].width==9);
    CHECK(terminal.values[1].source_first==3&&!terminal.values[1].source_count&&!terminal.values[1].left&&!terminal.values[1].width&&terminal.values[1].top==16&&terminal.values[1].height==12);
    terminal.count=0;
    CHECK(native_paragraph_geometry_visit(geometry,0,2,terminal_sink,&range_count,true)==SfntStatus::ok&&range_count==1&&terminal.count==1&&terminal.values[0].width==7);
    terminal.count=0;
    CHECK(native_paragraph_geometry_visit(geometry,0,4,terminal_sink,&range_count,true)==SfntStatus::ok&&range_count==2&&terminal.count==2&&terminal.values[1].width==5);
    NativeParagraphHitCluster hit_clusters[4];NativeParagraphHitLine hit_lines[2];float hit_advances[4];SfntFontLineParts hit_styles[4];
    hit_styles[3]={71,72};NativeParagraphHits hits{};NativeParagraphHitStorage hit_storage{{hit_clusters,4},{hit_lines,2},{hit_advances,4},&hits,{hit_styles,3}};
    CHECK(native_prepared_styled_paragraph_layout(shape,{clusters,4},{placements,4},{styles,3},8,16,true,{runs,8},&out,{},&hit_storage)==SfntStatus::ok);
    CHECK(hits.style_count==3&&bytes_equal(hit_styles,styles,sizeof(styles))&&hit_styles[3].baseline==71);
    CHECK(hit_lines[0].ascent_style==1&&hit_lines[0].descent_style==2&&hit_clusters[2].style==2);
    NativeParagraphHitMetric caret{};float caret_x=0;
    CHECK(native_paragraph_hit_position(hits,0,false,&caret,&caret_x)==SfntStatus::ok&&caret.top==3&&caret.height==10);
    CHECK(native_paragraph_hit_position(hits,1,false,&caret,&caret_x)==SfntStatus::ok&&caret.top==0&&caret.height==12);
    CHECK(native_paragraph_hit_position(hits,2,false,&caret,&caret_x)==SfntStatus::ok&&caret.top==4&&caret.height==12);
    CHECK(native_paragraph_hit_position(hits,3,false,&caret,&caret_x)==SfntStatus::ok&&caret.top==16&&caret.height==12);
    CHECK(native_paragraph_hit_range(hits,0,3,0,0,{ranges,2},&range_count)==SfntStatus::ok&&range_count==1&&ranges[0].height==16&&ranges[0].top==0);
    hit_storage.styles.count=2;
    CHECK(native_prepared_styled_paragraph_layout(shape,{clusters,4},{placements,4},{styles,3},8,16,true,{runs,8},&out,{},&hit_storage)==SfntStatus::output_too_small&&!out.line_count&&!hits.lines.count);
    hit_storage.styles={nullptr,3};
    CHECK(native_prepared_styled_paragraph_layout(shape,{clusters,4},{placements,4},{styles,3},8,16,true,{runs,8},&out,{},&hit_storage)==SfntStatus::output_too_small&&!hits.lines.count);
    CHECK(native_prepared_styled_paragraph_layout(shape,{clusters,4},{placements,4},{styles,3},8,16,true,{runs,8},&out,{lines,1})==SfntStatus::output_too_small&&!out.line_count);
    CHECK(native_prepared_styled_paragraph_layout(shape,{clusters,4},{placements,4},{styles,3},8,16,true,{runs,8},&out,{nullptr,2})==SfntStatus::invalid_input&&!out.line_count);
    CHECK(build(8,3)==SfntStatus::output_too_small&&!out.run_count&&!out.line_count);
    CHECK(native_prepared_styled_paragraph_layout(shape,{clusters,4},{placements,4},{},100,16,true,{runs,8},&out)==SfntStatus::invalid_input&&!out.run_count);
    const NativeParagraphShape large{65535,1,1,{10,7}};const NativeParagraphCluster wide{0,65535,0,1,4,0};
    CHECK(native_prepared_paragraph_layout(large,{&wide,1},{placements,1},100,16,true,{runs,8},&out)==SfntStatus::ok&&runs[0].source_first==0&&runs[0].source_count==65535&&runs[0].width==4);
    CHECK(native_prepared_paragraph_layout(large,{&wide,1},{placements,1},100,16,true,{runs,8},&out,&hit_storage)==SfntStatus::ok&&hit_clusters[0].source_end==65535&&!hits.style_count);
    CHECK(native_paragraph_hit_position(hits,65535,false,&caret,&caret_x)==SfntStatus::ok&&caret.source_first==65535&&!caret.source_count&&caret_x==4);
    const NativeParagraphShape leading{3,3,3,{10,7}};
    NativeParagraphCluster zeros[]{{0,1,0,1,0,0},{1,2,1,1,0,1u<<native_paragraph_style_shift},{2,3,2,1,4,2u<<native_paragraph_style_shift}};
    const SfntPlacement zero_placements[]{{0,0,0},{0,0,0},{4,0,0}};hit_storage.styles={hit_styles,3};NativeParagraphHitPoint point{};
    const auto leading_hits=[&]() noexcept {return native_prepared_styled_paragraph_layout(leading,{zeros,3},{zero_placements,3},{styles,3},100,16,true,{runs,8},&out,{},&hit_storage);};
    CHECK(leading_hits()==SfntStatus::ok&&native_paragraph_hit_point(hits,-1,5,&point)==SfntStatus::ok&&point.source_position==2&&!point.inside);
    zeros[1].flags=0;zeros[2].flags=0;
    CHECK(leading_hits()==SfntStatus::ok&&native_paragraph_hit_point(hits,-1,5,&point)==SfntStatus::ok&&!point.source_position&&!point.inside);
    return true;
}
bool paragraph_cases() noexcept {
    NativeParagraphShape shape{6,3,5,{10,7}};
    NativeParagraphCluster flagged[6]{};for(u32 i=0;i<6;++i)flagged[i]={i,i+1,0,0,0,0};
    native_ascii_paragraph_flags(utf8_literal("ab \t\r\n"),{flagged,6});
    CHECK(!flagged[0].flags&&!flagged[1].flags&&flagged[2].flags==native_paragraph_space&&flagged[3].flags==native_paragraph_break_before&&!flagged[4].flags&&!flagged[5].flags);
    NativeParagraphCluster clusters[]{
        {0,1,0,1,4,0},{1,2,1,1,4,0},{2,3,2,1,2,native_paragraph_space},
        {3,4,3,0,0,native_paragraph_tab|native_paragraph_break_before},{4,6,3,0,0,native_paragraph_newline}};
    SfntPlacement placements[]{{4,0,0},{4,0,0},{2,0,0}};
    NativeParagraphRun runs[8]{};NativeParagraphLayout out{};
    const auto layout=[&](float width,float stop,bool wrap,u32 capacity=8) noexcept {
        out={99,99,99,99,99,99};return native_prepared_paragraph_layout(shape,{clusters,5},{placements,3},width,stop,wrap,{runs,capacity},&out);};
    CHECK(layout(100,16,true)==SfntStatus::ok&&out.line_count==2&&out.run_count==3&&out.width==16&&out.extent==16&&out.height==20&&!out.flags);
    CHECK(runs[0].source_count==3&&runs[0].glyph_count==3&&runs[1].x==10&&runs[1].source_first==3&&runs[2].x==16&&runs[2].source_count==2);
    CHECK(layout(5,16,true)==SfntStatus::ok&&out.line_count==3&&out.flags==native_paragraph_emergency_break&&out.height==30);
    CHECK(layout(5,16,false)==SfntStatus::ok&&out.line_count==2&&!out.flags);
    CHECK(layout(100,8,true)==SfntStatus::ok&&out.width==16&&out.line_count==2);
    CHECK(layout(100,16,true,2)==SfntStatus::output_too_small&&!out.run_count&&!out.line_count&&!out.height);
    CHECK(layout(100,16,true)==SfntStatus::ok&&out.run_count==3); // Retry after a partial write.
    const u32 bad_bits[]{0x7f800000,0xff800000,0x7fc00000};
    for(u32 bits:bad_bits){float v=0;copy_bytes(&v,&bits,4);
        CHECK(layout(v,16,true)==SfntStatus::invalid_input&&!out.line_count);
        CHECK(layout(100,v,true)==SfntStatus::invalid_input&&!out.line_count);}
    CHECK(layout(0,16,true)==SfntStatus::invalid_input&&!out.line_count);
    CHECK(layout(100,0,true)==SfntStatus::invalid_input&&!out.line_count);
    shape.vertical.height=3.0e38f;CHECK(layout(100,16,true)==SfntStatus::resource_limit&&!out.line_count);shape.vertical.height=10;
    const NativeParagraphShape empty{0,0,0,{10,7}};
    CHECK(native_prepared_paragraph_layout(empty,{},{},100,16,true,{},&out)==SfntStatus::ok&&out.line_count==1&&!out.run_count&&out.height==10);
    CHECK(native_prepared_paragraph_layout(empty,{},{},100,16,true,{},nullptr)==SfntStatus::invalid_input);
    return true;
}
bool ordered_paragraph_cases() noexcept {
    // Abstract shaped output for a supplementary scalar, base+mark, space,
    // supplementary scalar and CRLF. UTF-16 positions are deliberately not
    // glyph indices or UTF-8 byte offsets. No source string reaches the fitter.
    NativeParagraphShape shape{9,5,5,{10,7}};
    NativeParagraphCluster clusters[]{
        {0,2,0,1,6,0},{2,4,1,2,5,0},{4,5,3,1,2,native_paragraph_space},
        {5,7,4,1,7,native_paragraph_break_before},{7,9,5,0,0,native_paragraph_newline}};
    SfntPlacement placements[]{{6,0,0},{5,0,0},{0,-1,2},{2,0,0},{7,0,0}};
    NativeParagraphRun runs[8];NativeParagraphLayout out{};NativeParagraphHits hits{};
    NativeParagraphHitCluster hc[8];NativeParagraphHitLine hl[8];float ha[8];
    NativeParagraphHitStorage storage{{hc,8},{hl,8},{ha,8},&hits};
    const auto build=[&](float width=14,bool wrap=true) noexcept {
        return native_prepared_paragraph_layout(shape,{clusters,5},{placements,5},width,16,wrap,{runs,8},&out,&storage);};
    CHECK(build()==SfntStatus::ok&&out.line_count==3&&out.run_count==4&&!out.flags&&out.width==11&&out.extent==13&&out.height==30);
    CHECK(runs[0].source_first==0&&runs[0].source_count==4&&runs[0].glyph_count==3&&runs[0].baseline==7);
    CHECK(runs[1].source_first==4&&runs[1].source_count==1&&runs[1].x==11);
    CHECK(runs[2].source_first==5&&runs[2].source_count==2&&runs[2].x==0&&runs[2].baseline==17);
    CHECK(hits.source_units==9&&hits.clusters.count==6&&hits.lines.count==3&&hits.advances.count==5);
    NativeParagraphHitMetric metric{};float x=0;
    for(u32 p=0;p<4;++p){CHECK(native_paragraph_hit_position(hits,p,false,&metric,&x)==SfntStatus::ok);
        CHECK(metric.source_first==(p<2?0u:2u)&&metric.source_count==2&&x==(p<2?0.f:6.f));}
    CHECK(native_paragraph_hit_position(hits,6,true,&metric,&x)==SfntStatus::ok&&metric.source_first==5&&metric.source_count==2&&metric.top==10&&x==7);
    CHECK(native_paragraph_hit_position(hits,9,false,&metric,&x)==SfntStatus::ok&&!metric.source_count&&metric.top==20&&x==0);
    NativeParagraphHitPoint point{};
    CHECK(native_paragraph_hit_point(hits,2.9f,5,&point)==SfntStatus::ok&&!point.source_position);
    CHECK(native_paragraph_hit_point(hits,3.f,5,&point)==SfntStatus::ok&&point.source_position==2);
    CHECK(native_paragraph_hit_point(hits,4.f,15,&point)==SfntStatus::ok&&point.source_position==7);
    NativeParagraphHitMetric ranges[4];u32 count=0;
    // A nonempty selection stops at its end position; the final empty caret
    // line contributes height but no selected source units or range rectangle.
    CHECK(native_paragraph_hit_range(hits,0,9,0,0,{ranges,4},&count)==SfntStatus::ok&&count==2);
    CHECK(ranges[0].source_first==0&&ranges[0].source_count==5&&ranges[0].width==13);
    CHECK(ranges[1].source_first==5&&ranges[1].source_count==4&&ranges[1].width==7);
    CHECK(build(1)==SfntStatus::ok&&out.flags==native_paragraph_emergency_break&&out.line_count==4);
    CHECK(hc[0].source_end==2&&hc[1].source_first==2&&hc[1].source_end==4); // Atomic emergency clusters.
    CHECK(build(1,false)==SfntStatus::ok&&!out.flags&&out.line_count==2&&out.width==20);
    const auto saved=storage;storage.clusters.count=5;
    CHECK(build()==SfntStatus::output_too_small&&!out.line_count&&!hits.lines.count);storage=saved;
    CHECK(build()==SfntStatus::ok&&hits.lines.count==3);
    return true;
}
