// Fixed-point staging borrows the coordinate bytes of the caller's output (no
// second point array); floats are written only after widening.
struct CffBoldPoint { i32 x,y; };
bool same(CffBoldPoint a,CffBoldPoint b) noexcept {return a.x==b.x&&a.y==b.y;}
u32 cff_command_points(GlyphPathCommandKind kind) noexcept {
    return kind==GlyphPathCommandKind::cubic?3u:kind==GlyphPathCommandKind::end_closed?0u:1u;
}
struct CffBoldPath {
    GlyphOutline& out;
    u32& work;
    bool spend() noexcept {if(!work)return false;--work;return true;}
    CffBoldPoint get(u32 index) const noexcept {
        CffBoldPoint p;copy_bytes(&p,reinterpret_cast<const u8*>(out.commands+(index>>2))+(index&3)*8,8);return p;
    }
    void put(u32 index,CffBoldPoint p) noexcept {
        copy_bytes(reinterpret_cast<u8*>(out.commands+(index>>2))+(index&3)*8,&p,8);
    }
    u32 next(u32 index) const noexcept {
        return (index&3)+1<cff_command_points(out.commands[index>>2].kind)?index+1:(index&~3u)+4;
    }
    bool contour(u32 first,u32* limit) noexcept {
        u32 end=first>>2;
        while(end<out.count&&out.commands[end].kind!=GlyphPathCommandKind::end_closed){if(!spend())return false;++end;}
        if(end==out.count)return false;*limit=end<<2;return true;
    }
};
struct CffBoldExtrema {
    i32 value[4]{};
    bool vote[4]{};
    bool any=false;
    void consider(CffBoldPoint a,CffBoldPoint b,CffBoldPoint c) noexcept {
        const bool direction=(static_cast<double>(c.x)-b.x)*(static_cast<double>(a.y)-b.y)-
            (static_cast<double>(c.y)-b.y)*(static_cast<double>(a.x)-b.x)>0;
        const bool replace[]{!any||b.x>value[0],!any||b.x<value[1],!any||b.y>value[2],!any||b.y<value[3]};
        for(u32 i=0;i<4;++i)if(replace[i]){value[i]=i<2?b.x:b.y;vote[i]=direction;}any=true;
    }
};
struct CffBoldEdge {double ax,ay,bx,by;};
CffBoldEdge cff_bold_edge(CffBoldPoint a,CffBoldPoint b,i32 wx,i32 wy,bool forward) noexcept {
    const i64 dx=(static_cast<i64>(b.x)-a.x)*(forward?1:-1),dy=(static_cast<i64>(b.y)-a.y)*(forward?1:-1);
    const i32 sx=dy>0?wx:dy<0?-wx:0,sy=dx>0?-wy:dx<0?wy:0;
    return {static_cast<double>(a.x)+sx,static_cast<double>(a.y)+sy,static_cast<double>(b.x)+sx,static_cast<double>(b.y)+sy};
}
CffBoldPoint cff_bold_join(CffBoldPoint previous,CffBoldPoint current,CffBoldPoint next,i32 wx,i32 wy,bool forward) noexcept {
    const auto a=cff_bold_edge(previous,current,wx,wy,forward),b=cff_bold_edge(current,next,wx,wy,forward);
    double x=b.ax,y=b.ay;
    if(a.bx!=b.ax||a.by!=b.ay){
        const double dx=a.bx-a.ax,dy=a.by-a.ay,ex=b.bx-b.ax,ey=b.by-b.ay,det=dx*ey-dy*ex;
        if(det){const double t=((a.ay-b.ay)*ex-(a.ax-b.ax)*ey)/det;x=dx*t+a.ax;y=dy*t+a.ay;}
        // Clamp before the truncating integer conversion; intersections can lie
        // beyond the fixed-point range.
        x=minimum(static_cast<double>(current.x)+wx,maximum(static_cast<double>(current.x)-wx,x));
        y=minimum(static_cast<double>(current.y)+wy,maximum(static_cast<double>(current.y)-wy,y));
    }
    return {static_cast<i32>(x)+wx,static_cast<i32>(y)+wy};
}
SfntStatus cff_bolden(const CffFont& font,GlyphOutline& out,u32& work,bool oblique) noexcept {
    const double m=font.matrix[0];
    if(font.cid||!(m>0&&m<2&&m==font.matrix[3])||font.matrix[1]||font.matrix[2]||font.matrix[4]||font.matrix[5])return SfntStatus::unsupported;
    const double matrix=static_cast<double>(static_cast<i64>(m*1073741824.0+.5));
    if(matrix<1||matrix>2147483647.0)return SfntStatus::unsupported;
    const double inverse=70368744177664.0/matrix;
    if(inverse>2147483647.0)return SfntStatus::unsupported;
    const double scaled=m*font.units_per_em*65536.0;
    if(scaled>2147483646.0)return SfntStatus::unsupported;
    const i64 inv=static_cast<i64>(inverse+.5),scale=static_cast<i64>(scaled+.5);
    if(scale<=0||scale>2147483647)return SfntStatus::unsupported;
    const auto multiply=[](i64 a,i64 b) noexcept {return (a*b+32768)>>16;};
    const i64 horizontal=multiply(inv,scale),vertical=horizontal+(oblique?multiply(multiply(inv,static_cast<i64>(scale*(87.0/256.0)+.5)),0x561e):0);
    const i64 wide_x=multiply(horizontal,655),wide_y=multiply(vertical,655);
    if(wide_x<0||wide_y<0||wide_x>0x1fffffff||wide_y>0x1fffffff||out.count>0x3fffffffu)return SfntStatus::resource_limit;
    const i32 wx=static_cast<i32>(wide_x),wy=static_cast<i32>(wide_y);
    CffBoldPath path{out,work};CffBoldExtrema extrema;
    // Each nonzero edge's endpoint votes at most once per strict extremum.
    // Duplicate runs are scanned once here and once by their preceding edge.
    for(u32 first=0;first>>2<out.count;){
        u32 limit=0;if(!path.contour(first,&limit))return SfntStatus::resource_limit;
        for(u32 i=first;i<limit;i=path.next(i)){
            if(!path.spend())return SfntStatus::resource_limit;
            const auto a=path.get(i);
            if(a.x<(-2147483647-1)+2*wx||a.x>2147483647-2*wx||a.y<(-2147483647-1)+2*wy||a.y>2147483647-2*wy)return SfntStatus::resource_limit;
            u32 j=path.next(i);if(j==limit)j=first;const auto b=path.get(j);if(same(a,b))continue;
            u32 k=j;CffBoldPoint c{};
            do{if(!path.spend())return SfntStatus::resource_limit;k=path.next(k);if(k==limit)k=first;c=path.get(k);}while(same(b,c));
            extrema.consider(a,b,c);
        }
        first=limit+4;
    }
    const bool forward=static_cast<u32>(extrema.vote[0])+extrema.vote[1]+extrema.vote[2]+extrema.vote[3]>=2;
    for(u32 first=0;first>>2<out.count;){
        u32 limit=0;if(!path.contour(first,&limit))return SfntStatus::resource_limit;
        const auto original_first=path.get(first);CffBoldPoint previous=original_first;u32 logical_end=first;
        // Locate the last distinct point before writing anything. Any trailing
        // copies of the first point receive the already-computed first result.
        for(u32 i=first;i<limit;i=path.next(i)){
            if(!path.spend())return SfntStatus::resource_limit;
            const auto p=path.get(i);if(!same(p,original_first)){previous=p;logical_end=path.next(i);}
        }
        CffBoldPoint first_result=original_first;
        for(u32 i=first;i<logical_end;){
            const auto current=path.get(i);u32 j=path.next(i);
            while(j<logical_end&&same(path.get(j),current)){if(!path.spend())return SfntStatus::resource_limit;j=path.next(j);}
            const auto next=j<logical_end?path.get(j):original_first;
            const auto result=cff_bold_join(previous,current,next,wx,wy,forward);
            if(i==first)first_result=result;
            do{if(!path.spend())return SfntStatus::resource_limit;path.put(i,result);i=path.next(i);}while(i<j);
            previous=current;
        }
        if(logical_end!=first)for(u32 i=logical_end;i<limit;i=path.next(i)){if(!path.spend())return SfntStatus::resource_limit;path.put(i,first_result);}
        first=limit+4;
    }
    // Bounds are rebuilt from the final emboldened outline; raw CFF metrics stay separate.
    const float em=1.0f/font.units_per_em;out.has_bounds=false;
    for(u32 command=0;command<out.count;++command)for(u32 part=0;part<cff_command_points(out.commands[command].kind);++part){
        if(!path.spend())return SfntStatus::resource_limit;
        const auto p=path.get((command<<2)+part);
        const float coordinates[]{positive_zero(static_cast<float>(p.x/65536.0)*em),positive_zero(static_cast<float>(-static_cast<double>(p.y)/65536.0)*em)};
        copy_bytes(reinterpret_cast<u8*>(out.commands+command)+part*8,coordinates,8);
        if(!out.has_bounds){out.minimum_x=out.maximum_x=coordinates[0];out.minimum_y=out.maximum_y=coordinates[1];out.has_bounds=true;}
        else{out.minimum_x=minimum(out.minimum_x,coordinates[0]);out.maximum_x=maximum(out.maximum_x,coordinates[0]);out.minimum_y=minimum(out.minimum_y,coordinates[1]);out.maximum_y=maximum(out.maximum_y,coordinates[1]);}
    }
    return SfntStatus::ok;
}
