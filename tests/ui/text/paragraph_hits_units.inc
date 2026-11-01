// Included by the native application line QA harness. DirectWrite is an oracle
// here only; none of these calls are part of the production geometry engine.
struct HitPointProbe {u32 x,y,generation;};
inline HitPointProbe hit_point_probes[8192]{};inline u32 hit_point_generation=0;
inline u64 hit_point_requests=0,hit_point_reused=0,repeated_hit_boxes=0;
inline void begin_hit_point_probes() noexcept {
    if(!++hit_point_generation){clear_bytes(hit_point_probes,sizeof(hit_point_probes));hit_point_generation=1;}
}
inline bool repeated_hit_point(float x,float y) noexcept {
    ++hit_point_requests;u32 xb=0,yb=0;copy_bytes(&xb,&x,4);copy_bytes(&yb,&y,4);
    u32 hash=xb*0x9e3779b9u+yb;hash^=hash>>16;hash*=0x7feb352du;hash^=hash>>15;
    // Exact coordinates and a layout-local generation are mandatory. A bounded
    // collision search falls back to performing the query, never skipping it.
    for(u32 probe=0;probe<8;++probe){auto& entry=hit_point_probes[(hash+probe)&8191u];
        if(entry.generation!=hit_point_generation){entry={xb,yb,hit_point_generation};return false;}
        if(entry.x==xb&&entry.y==yb){++hit_point_reused;return true;}}
    return false;
}
inline void hit_metric(const DWRITE_HIT_TEST_METRICS& m) noexcept {
    print("[");number(m.textPosition);print(",");number(m.length);print(",");real(m.left);print(",");real(m.top);print(",");real(m.width);print(",");real(m.height);print(",");number(m.bidiLevel);print(",");number(m.isText);print(",");number(m.isTrimmed);print("]");
}
inline NativeParagraphHitCluster hit_clusters[256];inline NativeParagraphHitLine hit_lines[257];inline float hit_advances[1280];inline NativeParagraphHitRun hit_visual_runs[256];
struct HitRunCapture final : IDWriteTextRenderer {
    bool comma=false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(!bytes_equal(&iid,&__uuidof(IUnknown),sizeof(GUID))&&!bytes_equal(&iid,&__uuidof(IDWriteTextRenderer),sizeof(GUID))&&!bytes_equal(&iid,&__uuidof(IDWritePixelSnapping),sizeof(GUID)))return E_NOINTERFACE;
        *out=static_cast<IDWriteTextRenderer*>(this);return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return 1;} ULONG STDMETHODCALLTYPE Release() override {return 1;}
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void*,BOOL* value) override {*value=TRUE;return S_OK;}
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(void*,DWRITE_MATRIX* value) override {*value={1,0,0,1,0,0};return S_OK;}
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*,FLOAT* value) override {*value=1;return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(void*,FLOAT x,FLOAT y,DWRITE_MEASURING_MODE,const DWRITE_GLYPH_RUN* run,const DWRITE_GLYPH_RUN_DESCRIPTION* desc,IUnknown*) override {
        if(!run||!desc)return E_INVALIDARG;
        IDWriteFontFace3* face=nullptr;IDWriteLocalizedStrings* names=nullptr;wchar_t family[256]{};
        if(SUCCEEDED(run->fontFace->QueryInterface(__uuidof(IDWriteFontFace3),reinterpret_cast<void**>(&face)))&&SUCCEEDED(face->GetFamilyNames(&names))&&SUCCEEDED(names->GetString(0,family,256))){char text[256];u32 i=0;while(i<255&&family[i]){text[i]=family[i]<128?static_cast<char>(family[i]):'?';++i;}text[i]=0;print(text);print(" ");}com_release(names);com_release(face);
        print("draw source=");number(desc->textPosition);print("..");number(desc->textPosition+desc->stringLength);print(" x/y/em=");real(x);print("/");real(y);print("/");real(run->fontEmSize);print(" level=");number(run->bidiLevel);print(" advances=");
        for(u32 i=0;i<run->glyphCount;++i){real(run->glyphAdvances[i]);print(",");}print(" glyphs=");
        for(u32 i=0;i<run->glyphCount;++i){number(run->glyphIndices[i]);print(",");}print("\n");return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(void*,FLOAT,FLOAT,const DWRITE_UNDERLINE*,IUnknown*) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(void*,FLOAT,FLOAT,const DWRITE_STRIKETHROUGH*,IUnknown*) override {return S_OK;}
    HRESULT STDMETHODCALLTYPE DrawInlineObject(void*,FLOAT,FLOAT,IDWriteInlineObject*,BOOL,BOOL,IUnknown*) override {return S_OK;}
};
inline void describe_hit_runs(const NativeParagraphHits& h,IDWriteTextLayout* layout) noexcept {
    HitRunCapture capture;layout->Draw(nullptr,&capture,0,0);
    for(u32 i=0;i<h.visual_runs.count;++i){const auto& r=h.visual_runs[i];const u32 first=h.clusters[r.first_cluster].source_first,end=h.clusters[r.first_cluster+r.cluster_count-1].source_end;
        print("run=");number(first);print("..");number(end);print(" left/width/level=");real(r.left);print("/");real(r.width);print("/");number(r.bidi_level);print(" ref=");
        DWRITE_HIT_TEST_METRICS m[256];u32 count=0;if(SUCCEEDED(layout->HitTestTextRange(first,end-first,0,0,m,256,&count)))for(u32 j=0;j<count;++j)hit_metric(m[j]);print("\n");}
}
inline bool same_hit(const NativeParagraphHitMetric& a,const DWRITE_HIT_TEST_METRICS& b) noexcept {
    if(a.source_first!=b.textPosition||a.source_count!=b.length||a.left!=b.left||a.top!=b.top||a.width!=b.width||a.height!=b.height||a.bidi_level!=b.bidiLevel||!b.isText||b.isTrimmed){
        print("native hit=[");number(a.source_first);print(",");number(a.source_count);print(",");real(a.left);print(",");real(a.top);print(",");real(a.width);print(",");real(a.height);print(",");number(a.bidi_level);print("] ref=");hit_metric(b);print("\n");return false;}return true;
}
inline bool compare_hits(DWriteTextSystem system,Utf8View text,float width,TextWrapping wrapping,ConstSlice<TextStyleSpan> styles,bool detached,bool tracked_layout,bool captured_format) noexcept {
    ++cases;NativeParagraphHits native{};NativeParagraphHitStorage storage{{hit_clusters,256},{hit_lines,257},{hit_advances,1280},&native};TextMeasureResult measured{};
    SfntFontLineParts parts[513];storage.styles={parts,513};storage.visual_runs={hit_visual_runs,256};
    auto candidate=system;if(detached){
        // Only the directional caller supplies a held, immutable format already
        // captured by its font/em cell. Every native invocation still validates
        // the live declaration before removing the legacy COM fallback.
        if(captured_format){CHECK(candidate.native_text&&candidate.native_format&&native_text_format_identity(candidate));}
        else {CHECK(native_text_format_capture(&candidate));}
        candidate.factory=nullptr;candidate.format=nullptr;
    }
    if(styles.count){CHECK(native_text_candidate_styled(candidate,text,styles,width,1000,nullptr,&measured,wrapping,TextOverflow::clip,{},&storage));++native_cases;}
    else {CHECK(native_text_candidate_hits(candidate,text,width,1000,wrapping,storage,&measured));if(detached)++native_cases;}
    u32 units=0;for(u32 at=0;at<text.bytes;){const auto cp=utf8_decode_one(text.data+at,text.bytes-at);CHECK(cp.valid&&units+2<=256);
        if(cp.codepoint>65535){utf16[units++]=static_cast<wchar_t>(0xd800+((cp.codepoint-65536)>>10));utf16[units++]=static_cast<wchar_t>(0xdc00+((cp.codepoint-65536)&1023));}
        else utf16[units++]=static_cast<wchar_t>(cp.codepoint);at+=cp.width;}
    IDWriteTextLayout* layout=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTextLayout(utf16,units,system.format,width,1000,&layout)));
    struct Release {IDWriteTextLayout*& p;~Release() noexcept {com_release(p);}} release{layout};
    CHECK(SUCCEEDED(layout->SetWordWrapping(wrapping==TextWrapping::wrap?DWRITE_WORD_WRAPPING_WRAP:DWRITE_WORD_WRAPPING_NO_WRAP))&&SUCCEEDED(text_apply_layout_styles(system,layout,text,styles)));
    // Explicit tracked-layout queries are distinct from the current app edit
    // owner's untracked policy. Match drawing's spacing, then restore the
    // emoji override that clears tracking for emoji-only font ranges.
    if(tracked_layout&&system.tracking){IDWriteTextLayout1* spacing=nullptr;float size=0;
        CHECK(SUCCEEDED(layout->QueryInterface(__uuidof(IDWriteTextLayout1),reinterpret_cast<void**>(&spacing))));
        struct ReleaseSpacing {IDWriteTextLayout1*& p;~ReleaseSpacing() noexcept {com_release(p);}} release_spacing{spacing};
        CHECK(SUCCEEDED(layout->GetFontSize(0,&size))&&SUCCEEDED(spacing->SetCharacterSpacing(0,size*system.tracking,0,{0,units})));
        CHECK(SUCCEEDED(text_apply_layout_styles(system,layout,text,styles)));}
    if(system.independent_letters){IDWriteTypography* typography=nullptr;CHECK(SUCCEEDED(owner.factory->CreateTypography(&typography)));
        struct ReleaseTypography {IDWriteTypography*& p;~ReleaseTypography() noexcept {com_release(p);}} release_typography{typography};
        CHECK(SUCCEEDED(typography->AddFontFeature({DWRITE_FONT_FEATURE_TAG_STANDARD_LIGATURES,0}))&&SUCCEEDED(typography->AddFontFeature({DWRITE_FONT_FEATURE_TAG_CONTEXTUAL_LIGATURES,0}))&&SUCCEEDED(typography->AddFontFeature({DWRITE_FONT_FEATURE_TAG_KERNING,0}))&&SUCCEEDED(layout->SetTypography(typography,{0,units})));}
    // The immutable layout receives many identical coordinates from adjacent
    // positions and leading/trailing carets. Verify each distinct point once.
    begin_hit_point_probes();
    // Every grid point derives only from the caret box (left, width, y,
    // height). A trailing caret, or a position inside the same cluster,
    // repeats the previous box, whose points were all just verified.
    u32 previous_box[4]{};bool have_box=false;
    for(u32 p=0;p<=units;++p)for(u32 trailing=0;trailing<2;++trailing){float x=0,y=0,nx=0;DWRITE_HIT_TEST_METRICS m{};NativeParagraphHitMetric nm{};
        CHECK(SUCCEEDED(layout->HitTestTextPosition(p,trailing,&x,&y,&m))&&native_paragraph_hit_position(native,p,trailing!=0,&nm,&nx)==SfntStatus::ok);
        if(!same_hit(nm,m)||nx!=x){print("position=");number(p);print(" trailing=");number(trailing);print(" x=");real(nx);print(" ref=");real(x);print("\n");describe_hit_runs(native,layout);CHECK(false);}
        u32 box[4];copy_bytes(box,&m.left,4);copy_bytes(box+1,&m.width,4);copy_bytes(box+2,&y,4);copy_bytes(box+3,&m.height,4);
        if(have_box&&bytes_equal(box,previous_box,sizeof(box))){++repeated_hit_boxes;continue;}
        copy_bytes(previous_box,box,sizeof(box));have_box=true;
        const float xs[]{m.left-.001f,m.left,m.left+m.width*.5f,m.left+m.width,m.left+m.width+.001f,-1.f,1000000.f};
        const float ys[]{y-.001f,y,y+m.height*.5f,y+m.height,y+m.height+.001f,-1.f,1000000.f};
        for(float py:ys)for(float px:xs){BOOL t=FALSE,inside=FALSE;DWRITE_HIT_TEST_METRICS pm{};NativeParagraphHitPoint np{};
            if(repeated_hit_point(px,py))continue;
            CHECK(SUCCEEDED(layout->HitTestPoint(px,py,&t,&inside,&pm))&&native_paragraph_hit_point(native,px,py,&np)==SfntStatus::ok);
            if(np.source_position!=pm.textPosition+(t?pm.length:0)||np.inside!=(inside!=0)){
                print("point x/y=");real(px);print(",");real(py);print(" position/inside native=");number(np.source_position);print(",");number(np.inside);print(" ref=");number(pm.textPosition+(t?pm.length:0));print(",");number(inside);print(" metric=");hit_metric(pm);print("\n");
                for(u32 diagnostic=0;diagnostic<native.clusters.count;++diagnostic){const auto& cluster=native.clusters[diagnostic];NativeParagraphHitMetric observed{};float caret=0;DWRITE_HIT_TEST_METRICS expected{};float rx=0,ry=0;
                    native_paragraph_hit_position(native,cluster.source_first,false,&observed,&caret);layout->HitTestTextPosition(cluster.source_first,FALSE,&rx,&ry,&expected);
                    print("point cluster source/leading/width=");number(cluster.source_first);print("/");real(cluster.left);print("/");real(cluster.width);print(" native left=");real(observed.left);print(" ref=");hit_metric(expected);print("\n");}
                describe_hit_runs(native,layout);CHECK(false);}
        }
    }
    for(u32 a=0;a<=units;++a)for(u32 b=a;b<=units;++b){DWRITE_HIT_TEST_METRICS m[256];NativeParagraphHitMetric nm[256];u32 count=0,ncount=0;
        CHECK(SUCCEEDED(layout->HitTestTextRange(a,b-a,3.125f,-17.3f,m,256,&count))&&native_paragraph_hit_range(native,a,b-a,3.125f,-17.3f,{nm,256},&ncount)==SfntStatus::ok);
        if(count!=ncount){print("range count at=");number(a);print(",");number(b);print(" native/ref=");number(ncount);print("/");number(count);print("\n");for(u32 i=0;i<count;++i){hit_metric(m[i]);print("\n");}}
        if(count!=ncount){for(u32 i=0;i<native.clusters.count;++i){const auto& c=native.clusters[i];print("cluster ");number(c.source_first);print("..");number(c.source_end);print(" style=");number(c.style);print(" advances=");number(c.advance_flags);print("\n");}
            for(u32 p=0;p<=units;++p){u32 got=0;CHECK(SUCCEEDED(layout->HitTestTextRange(0,p,0,0,m,256,&got)));print("prefix=");number(p);print(" ");for(u32 j=0;j<got;++j)hit_metric(m[j]);print("\n");}}
        CHECK(count==ncount);for(u32 i=0;i<count;++i)if(!same_hit(nm[i],m[i])){print("range=");number(a);print(",");number(b);print(" region=");number(i);print("\n");describe_hit_runs(native,layout);CHECK(false);}
        u32 required=0;const auto status=native_paragraph_hit_range(native,a,b-a,0,0,{},&required);CHECK(required==count&&status==SfntStatus::output_too_small);
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
    for(u32 family=0;family<3;++family)for(float em:sizes){IDWriteTextFormat* format=nullptr;
        CHECK(SUCCEEDED(owner.factory->CreateTextFormat(families[family],nullptr,DWRITE_FONT_WEIGHT_NORMAL,DWRITE_FONT_STYLE_NORMAL,DWRITE_FONT_STRETCH_NORMAL,em,L"en-US",&format)));
        struct Release {IDWriteTextFormat*& p;~Release() noexcept {com_release(p);}} release{format};auto system=owner;system.format=format;CHECK(native_text_format_capture(&system));
        for(u32 independent=0;independent<2;++independent){system.independent_letters=independent!=0;
            for(u32 sample=0;sample<sizeof(paragraph_samples)/sizeof(paragraph_samples[0]);++sample)for(float width:widths)for(u32 nowrap=0;nowrap<2;++nowrap)
                if(!compare_hits(system,paragraph_samples[sample],width==1000000?width:width*(em/16),nowrap?TextWrapping::no_wrap:TextWrapping::wrap)){
                    print("family/sample/em/width/independent/nowrap=");number(family);print(",");number(sample);print(",");real(em);print(",");real(width);print(",");number(independent);print(",");number(nowrap);print("\n");return false;}
        }
    }return true;
}
