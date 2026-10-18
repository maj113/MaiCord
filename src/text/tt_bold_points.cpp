#include "maicord/text/tt_bold_points.hpp"
#include "tt_math.hpp"
namespace maicord {namespace {
struct Point {i64 x,y;};
bool same(Point a,Point b) noexcept {return a.x==b.x&&a.y==b.y;}
Point add(Point a,Point b) noexcept {return {a.x+b.x,a.y+b.y};}
Point sub(Point a,Point b) noexcept {return {a.x-b.x,a.y-b.y};}
i64 floor_div(i64 n,i64 d) noexcept {return n<0?-((-n+d-1)/d):n/d;}
bool mul_div(i64 a,i64 b,i64 d,i64* out) noexcept {
    if(!d)return false;
    const u64 x=static_cast<u64>(absolute_value<i64>(a)),y=static_cast<u64>(absolute_value<i64>(b)),z=static_cast<u64>(absolute_value<i64>(d));
    if(y&&x>0xffffffffffffffffull/y)return false;
    const u64 p=x*y,q=p/z,r=p%z>=(z+1)/2?1:0;
    if(q>0x7fffffffffffffffull-r)return false;
    *out=((a<0)^(b<0)^(d<0))?-static_cast<i64>(q+r):static_cast<i64>(q+r);return true;
}
bool read(const SfntOutlinePoint& p,Point* out) noexcept {
    const double x=static_cast<double>(p.x)*64,y=static_cast<double>(p.y)*64;
    if(!(x>=-2147483648.0&&x<=2147483647.0&&y>=-2147483648.0&&y<=2147483647.0))return false;
    out->x=static_cast<i64>(x);out->y=static_cast<i64>(y);
    return x==static_cast<double>(out->x)&&y==static_cast<double>(out->y);
}
bool meeting(Point a0,Point a1,Point b0,Point b1,Point* out) noexcept {
    const auto a=sub(a1,a0),b=sub(b1,b0);i64 n=0,d=0,t=0;
    if(!a.y){if(!b.x){*out={b0.x,a0.y};return true;}n=b0.y-a0.y;d=-b.y;}
    else if(!a.x){if(!b.y){*out={a0.x,b0.y};return true;}n=b0.x-a0.x;d=-b.x;}
    else if(absolute_value<i64>(a.x)<absolute_value<i64>(a.y)){
        if(!mul_div(b0.y-a0.y,a.x,a.y,&t))return false;n=a0.x-b0.x+t;
        if(!mul_div(b.y,a.x,a.y,&t))return false;d=b.x-t;
    }else{
        if(!mul_div(b0.x-a0.x,a.y,a.x,&t))return false;n=b0.y-a0.y-t;
        if(!mul_div(b.x,a.y,a.x,&t))return false;d=t-b.y;
    }
    if(absolute_value<i64>(d)<=16){*out={floor_div(a1.x+b0.x,2),floor_div(a1.y+b0.y,2)};return true;}
    i64 x=0,y=0;if(!mul_div(n,b.x,d,&x)||!mul_div(n,b.y,d,&y))return false;
    // Only the clamped displacement is needed; keep the following sums bounded.
    if(absolute_value<i64>(x)>0x3fffffffffffffffll||absolute_value<i64>(y)>0x3fffffffffffffffll)return false;
    *out={b0.x+x,b0.y+y};return true;
}
struct Widen {
    i64 strength,xn,xp,yn,yp,sign;
    Point offset(Point v) const noexcept {
        if(strength==1)return {xn+(-sign*v.y>0?xp:0),yn};
        i16 nx=0,ny=0;tt_math::normalized_vector(-sign*v.y,sign*v.x,&nx,&ny);
        const i64 x=floor_div(nx,256),y=floor_div(ny,256);
        return {xn+floor_div(x*(x>=0?xp:xn)+32,64),yn+floor_div(y*(y>=0?yp:yn)+32,64)};
    }
};
}
namespace {
SfntStatus widen(Slice<SfntOutlinePoint> points,ConstSlice<u32> ends,
    ConstSlice<u8> directions,bool marked,u16 units,bool fractional_halves,u32 work_limit,u32* work_used) noexcept {
    if(work_used)*work_used=0;
    if(units<16||units>16384||(points.count&&!points.data)||(ends.count&&!ends.data)||
        (!marked&&((directions.count&&!directions.data)||ends.count!=directions.count))||(!points.count)!=(!ends.count))return SfntStatus::invalid_input;
    const u64 cost=static_cast<u64>(points.count)*3+ends.count;
    if(cost>work_limit)return SfntStatus::resource_limit;
    if(work_used)*work_used=static_cast<u32>(cost);
    u32 start=0;
    for(u32 c=0;c<ends.count;++c){if(ends[c]<start||ends[c]>=points.count)return SfntStatus::invalid_input;if((marked?points[start].reserved[0]:directions[c])>1)return SfntStatus::invalid_input;start=ends[c]+1;}
    if(start!=points.count)return SfntStatus::invalid_input;
    for(u32 i=0;i<points.count;++i){Point p{};if(!read(points[i],&p))return SfntStatus::malformed;}
    const i64 s=(units+49)/50,xn=fractional_halves?s*32:(s/2)*64,yp=fractional_halves?(s-1)*32:((s-1)/2)*64;
    Widen w{s,xn,s*64-xn,(s-1)*64-yp,yp,1};start=0;
    for(u32 c=0;c<ends.count;++c){const u32 end=ends[c];w.sign=(marked?points[start].reserved[0]:directions[c])?-1:1;
        if(end-start<2){start=end+1;continue;}
        Point first{},previous{};read(points[start],&first);read(points[end],&previous);
        for(u32 i=start;i<=end;){Point p{};read(points[i],&p);u32 last=i;
            while(last<end){Point q{};read(points[last+1],&q);if(!same(p,q))break;++last;}
            Point next=first;if(last<end)read(points[last+1],&next);
            const Point a=w.offset(sub(p,previous)),b=w.offset(sub(next,p));Point delta=a;
            if(!same(a,b)){Point hit{};const Point center{w.xn,w.yn};
                if(!meeting(add(previous,s==1?center:a),add(p,a),add(p,b),add(next,s==1?center:b),&hit))return SfntStatus::malformed;
                delta=sub(hit,p);
            }
            if(delta.x<0)delta.x=0;else if(delta.x>s*64)delta.x=s*64;
            if(delta.y<0)delta.y=0;else if(delta.y>(s-1)*64)delta.y=2*w.yn;
            const Point result=add(p,delta);
            if(result.x>2147483647ll||result.y>2147483647ll)return SfntStatus::malformed;
            for(u32 j=i;j<=last;++j){points[j].x=static_cast<float>(static_cast<double>(result.x)/64);points[j].y=static_cast<float>(static_cast<double>(result.y)/64);}
            previous=p;i=last+1;
        }start=end+1;
    }return SfntStatus::ok;
}
}
SfntStatus tt_bold_points(Slice<SfntOutlinePoint> points,ConstSlice<u32> ends,ConstSlice<u8> directions,u16 units,bool fractional,u32 work,u32* used) noexcept {
    return widen(points,ends,directions,false,units,fractional,work,used);
}
SfntStatus tt_bold_marked_points(Slice<SfntOutlinePoint> points,ConstSlice<u32> ends,u16 units,bool fractional,u32 work,u32* used) noexcept {
    return widen(points,ends,{},true,units,fractional,work,used);
}
}
