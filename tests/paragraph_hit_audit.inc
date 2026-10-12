struct HitAudit {
    u32 positions=0,ranges=0,points=0,pixel_points=0,coordinates=0,semantics=0,point_positions=0,point_inside=0,pixel_positions=0,pixel_inside=0;
    u32 boundary_midpoint_ties=0,pixel_midpoint_ties=0;
    float maximum_delta=0;
    void coordinate(float a,float b) noexcept {
        if(a==b)return;++coordinates;const float delta=a>b?a-b:b-a;maximum_delta=maximum(maximum_delta,delta);
    }
    void metric(const NativeParagraphHitMetric& a,const DWRITE_HIT_TEST_METRICS& b) noexcept {
        semantics+=a.source_first!=b.textPosition||a.source_count!=b.length||a.bidi_level!=b.bidiLevel||!b.isText||b.isTrimmed;
        coordinate(a.left,b.left);coordinate(a.top,b.top);coordinate(a.width,b.width);coordinate(a.height,b.height);
    }
    bool point(const NativeParagraphHits& h,IDWriteTextLayout* layout,float x,float y,bool pixel) noexcept {
        if(repeated_hit_point(x,y))return true;
        BOOL trailing=FALSE,inside=FALSE;DWRITE_HIT_TEST_METRICS expected{};NativeParagraphHitPoint actual{};
        CHECK(SUCCEEDED(layout->HitTestPoint(x,y,&trailing,&inside,&expected))&&native_paragraph_hit_point(h,x,y,&actual)==SfntStatus::ok);
        const bool position=actual.source_position!=expected.textPosition+(trailing?expected.length:0),bounds=actual.inside!=(inside!=FALSE);
        const float middle=_mm_cvtss_f32(_mm_add_ss(_mm_set_ss(expected.left),_mm_mul_ss(_mm_set_ss(expected.width),_mm_set_ss(.5f))));
        const bool tie=position&&x==middle&&actual.source_position==expected.textPosition+(trailing?0:expected.length);
        if(pixel)pixel_midpoint_ties+=tie;else boundary_midpoint_ties+=tie;
        if(pixel&&position&&!pixel_positions){print("pixel caret x/y/native/reference=");real(x);print("/");real(y);print("/");number(actual.source_position);print("/");number(expected.textPosition+(trailing?expected.length:0));print("\n");}
        if(pixel){++pixel_points;pixel_positions+=position;pixel_inside+=bounds;}
        else{++points;point_positions+=position;point_inside+=bounds;}
        return true;
    }
    bool report() const noexcept {
        HitObservationWriter w;w.text("{\"kind\":\"hit_audit\",\"case\":");w.number(cases);
        const u32 values[]{positions,ranges,points,pixel_points,coordinates,semantics,point_positions,point_inside,pixel_positions,pixel_inside,boundary_midpoint_ties,pixel_midpoint_ties};
        w.text(",\"counts\":[");for(u32 i=0;i<12;++i){if(i)w.text(",");w.number(values[i]);}
        w.text("],\"maximum_delta_bits\":");w.real(maximum_delta);w.text("}\n");w.flush();return w.ok;
    }
    bool exact() const noexcept {return !coordinates&&!semantics&&!point_positions&&!point_inside&&!pixel_positions&&!pixel_inside;}
};
bool audit_hit_geometry(const NativeParagraphHits& h,IDWriteTextLayout* layout,u32 units,bool* audited) noexcept {
    wchar_t option[2]{};*audited=GetEnvironmentVariableW(L"MAICORD_HIT_AUDIT",option,2)==1&&option[0]=='1';if(!*audited)return true;
    HitAudit audit;begin_hit_point_probes();
    for(u32 position=0;position<=units;++position)for(u32 trailing=0;trailing<2;++trailing){
        float x=0,y=0,nx=0;DWRITE_HIT_TEST_METRICS m{};NativeParagraphHitMetric actual{};
        CHECK(SUCCEEDED(layout->HitTestTextPosition(position,trailing,&x,&y,&m))&&native_paragraph_hit_position(h,position,trailing!=0,&actual,&nx)==SfntStatus::ok);
        ++audit.positions;audit.metric(actual,m);audit.coordinate(nx,x);
        const float xs[]{m.left-.001f,m.left,m.left+m.width*.5f,m.left+m.width,m.left+m.width+.001f,-1.f,1000000.f};
        const float ys[]{y-.001f,y,y+m.height*.5f,y+m.height,y+m.height+.001f,-1.f,1000000.f};
        for(float py:ys)for(float px:xs)CHECK(audit.point(h,layout,px,py,false));
    }
    begin_hit_point_probes();
    for(u32 position=0;position<=units;++position){float x=0,y=0;DWRITE_HIT_TEST_METRICS m{};
        CHECK(SUCCEEDED(layout->HitTestTextPosition(position,FALSE,&x,&y,&m)));
        const float xs[]{m.left,m.left+m.width*.5f,m.left+m.width},ys[]{y,y+m.height*.5f,y+m.height};
        const float scales[]{1,1.25f,1.5f,2};
        const float origins[][2]{{0,0},{3.125f,-17.3f},{.25f,.5f},{.5f,.25f}};
        for(float scale:scales)for(const auto& origin:origins)for(float cx:xs)for(float cy:ys){
            CHECK(cx>-1000000&&cx<1000000&&cy>-1000000&&cy<1000000);
            const i32 ix=static_cast<i32>((cx+origin[0])*scale),iy=static_cast<i32>((cy+origin[1])*scale);
            for(i32 dx=-2;dx<=2;++dx)for(i32 dy=-1;dy<=1;++dy)CHECK(audit.point(h,layout,(ix+dx)/scale-origin[0],(iy+dy)/scale-origin[1],true));
        }
    }
    for(u32 first=0;first<=units;++first)for(u32 end=first;end<=units;++end){
        DWRITE_HIT_TEST_METRICS expected[256];NativeParagraphHitMetric actual[256];u32 count=0,ncount=0;
        CHECK(SUCCEEDED(layout->HitTestTextRange(first,end-first,3.125f,-17.3f,expected,256,&count))&&native_paragraph_hit_range(h,first,end-first,3.125f,-17.3f,{actual,256},&ncount)==SfntStatus::ok);
        ++audit.ranges;audit.semantics+=count!=ncount;
        for(u32 i=0;i<minimum(count,ncount);++i)audit.metric(actual[i],expected[i]);
        u32 required=0;const auto status=native_paragraph_hit_range(h,first,end-first,0,0,{},&required);
        CHECK(required==ncount&&status==SfntStatus::output_too_small);
    }
    CHECK(audit.report());if(audit.pixel_positions)describe_hit_runs(h,layout);return audit.exact();
}
