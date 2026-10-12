#include "maicord/text/tt_orientation.hpp"
#include <emmintrin.h>
#pragma float_control(precise,on,push)
namespace maicord {namespace {
struct Point {double x,y;};
struct IntegerPoint {i64 v[2];};
Point point(const SfntOutlinePoint& p) noexcept {return {static_cast<double>(p.x)*2,static_cast<double>(p.y)*2};}
Point middle(Point a,Point b) noexcept {return {(a.x+b.x)*.5,(a.y+b.y)*.5};}
double min(double a,double b) noexcept {return a<b?a:b;}
double max(double a,double b) noexcept {return a>b?a:b;}
i64 absolute(i64 n) noexcept {return n<0?-n:n;}
i64 floor_ratio(i64 n,i64 d) noexcept {return n<0?-((-n+d-1)/d):n/d;}
bool charge(u32& work) noexcept {if(!work)return false;--work;return true;}
IntegerPoint integer_point(const SfntOutlinePoint& p) noexcept {return {{static_cast<i64>(p.x)*2,static_cast<i64>(p.y)*2}};}
IntegerPoint extremum(ConstSlice<SfntOutlinePoint> points,u32 first,u32 end,u32 i,u32 direction) noexcept {
    const auto b=integer_point(points[i]);if(points[i].flags&1)return b;
    const auto& previous=points[i==first?end:i-1];const auto& following=points[i==end?first:i+1];
    auto a=integer_point(previous),z=integer_point(following);
    for(u32 j=0;j<2;++j){if(!(previous.flags&1))a.v[j]=(a.v[j]+b.v[j])/2;if(!(following.flags&1))z.v[j]=(z.v[j]+b.v[j])/2;}
    const u32 axis=direction/2;const i64 d=a.v[axis]-2*b.v[axis]+z.v[axis],n=a.v[axis]-b.v[axis];if(!d)return b;
    const i64 other=d-n,denominator=d*d;IntegerPoint result{};
    for(u32 j=0;j<2;++j){const i64 value=a.v[j]*other*other+2*b.v[j]*n*other+z.v[j]*n*n;
        if(j==axis)result.v[j]=direction&1?-floor_ratio(-value,denominator):floor_ratio(value,denominator);
        else result.v[j]=(absolute(value)/denominator+1)*(value<0?-1:1);
    }return result;
}
bool local_turn(ConstSlice<SfntOutlinePoint> points,u32 first,u32 end,u32 index,u32& work,u32* turn) noexcept {
    auto previous=integer_point(points[index==first?end:index-1]),current=integer_point(points[index]);
    i64 vx=current.v[0]-previous.v[0],vy=current.v[1]-previous.v[1];u32 next=index;*turn=0;
    for(u32 j=first;j<=end;++j){if(!charge(work))return false;next=next==end?first:next+1;const auto p=integer_point(points[next]);
        const i64 wx=p.v[0]-current.v[0],wy=p.v[1]-current.v[1];if(!wx&&!wy)continue;const i64 cross=vx*wy-vy*wx;
        if(cross){*turn=cross>0?2:0;return true;}current=p;vx=wx;vy=wy;
    }return true;
}
struct Ray {
    Point p,d;double length,lx,hx,ly,hy;
    explicit Ray(IntegerPoint probe,u32 direction) noexcept {
        p={static_cast<double>(probe.v[0]),static_cast<double>(probe.v[1])};Point q{};
        if(direction<2){p.x+=direction==0?1:-1;q={direction==0?-32768.0:32767.0,p.y-1};}
        else{p.y+=direction==2?1:-1;q={p.x-1,direction==2?-32768.0:32767.0};}
        d={q.x-p.x,q.y-p.y};length=d.x*d.x+d.y*d.y;lx=min(p.x,q.x);hx=max(p.x,q.x);ly=min(p.y,q.y);hy=max(p.y,q.y);
    }
    double side(Point a) const noexcept {return d.x*(a.y-p.y)-d.y*(a.x-p.x);}
    // A probe starting on another contour does not cross that contour there.
    bool contains_projection(Point a) const noexcept {const double v=(a.x-p.x)*d.x+(a.y-p.y)*d.y;return v>0&&v<=length;}
    bool excluded(Point a,Point b,Point c) const noexcept {return max(max(a.x,b.x),c.x)<lx||min(min(a.x,b.x),c.x)>hx||max(max(a.y,b.y),c.y)<ly||min(min(a.y,b.y),c.y)>hy;}
    u32 line(Point a,Point b) const noexcept {
        if(excluded(a,b,b))return 0;const double sa=side(a),sb=side(b);if(!((sa<=0&&sb>0)||(sb<=0&&sa>0)))return 0;
        // The reference rounds the edge parameter to 16.16 before testing the
        // ray's dominant coordinate. Exact geometric intersection alone gives
        // different answers when a probe starts on a shared boundary.
        const i64 numerator=absolute(static_cast<i64>(sa))*65536,denominator=absolute(static_cast<i64>(sb-sa));
        i64 t=(numerator+denominator/2)/denominator;if(!t)t=1;
        const bool horizontal=absolute(static_cast<i64>(d.x))>absolute(static_cast<i64>(d.y));
        const i64 offset=static_cast<i64>(horizontal?a.x-p.x:a.y-p.y)*65536+static_cast<i64>(horizontal?b.x-a.x:b.y-a.y)*t;
        const i64 extent=static_cast<i64>(horizontal?d.x:d.y)*65536;
        return extent>0?(offset>0&&offset<=extent):(offset<0&&offset>=extent);
    }
    u32 curve(Point a,Point control,Point b) const noexcept {
        if(excluded(a,control,b))return 0;const double sa=side(a),sb=side(b),sc=side(control),aa=sa-2*sc+sb,bb=2*(sc-sa);double roots[2]{};u32 count=0;
        if(!aa){if(!bb)return 0;roots[count++]=-sa/bb;}
        else{const double discriminant=bb*bb-4*aa*sa;if(discriminant<=0)return 0;
            const double square=_mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(),_mm_set_sd(discriminant))),z=-.5*(bb+(bb<0?-square:square));
            if(z){roots[count++]=z/aa;roots[count++]=sa/z;}else roots[count++]=0;
        }
        u32 crossings=0;for(u32 i=0;i<count;++i){const double t=roots[i];if(t<0||t>1)continue;const double derivative=2*aa*t+bb;
            if((t==0&&derivative<=0)||(t==1&&derivative>=0))continue;const double s=1-t;
            if(contains_projection({s*s*a.x+2*t*s*control.x+t*t*b.x,s*s*a.y+2*t*s*control.y+t*t*b.y}))++crossings;
        }return crossings;
    }
};
bool crossings(ConstSlice<SfntOutlinePoint> points,u32 first,u32 end,const Ray& ray,u32& work,u32* count) noexcept {
    if(end-first<2)return true;
    // Reject whole contours before visiting their curves. No temporary paths.
    Point low=point(points[first]),high=low;
    for(u32 i=first+1;i<=end;++i){if(!charge(work))return false;const auto p=point(points[i]);low={min(low.x,p.x),min(low.y,p.y)};high={max(high.x,p.x),max(high.y,p.y)};}
    if(high.x<ray.lx||low.x>ray.hx||high.y<ray.ly||low.y>ray.hy)return true;
    const bool first_on=(points[first].flags&1)!=0,last_on=(points[end].flags&1)!=0;
    const Point start=first_on?point(points[first]):last_on?point(points[end]):middle(point(points[first]),point(points[end]));
    Point current=start;u32 i=first+(first_on?1:0),limit=end+1-(!first_on&&last_on?1:0);
    while(i<limit){if(!charge(work))return false;const Point p=point(points[i]);
        if(points[i].flags&1){*count+=ray.line(current,p);current=p;++i;}
        else{const bool on=i+1>=limit||(points[i+1].flags&1);const Point following=i+1<limit?point(points[i+1]):start,target=on?following:middle(p,following);
            *count+=ray.curve(current,p,target);current=target;i+=on?2:1;
        }
    }if(!charge(work))return false;*count+=ray.line(current,start);return true;
}
}
namespace {
SfntStatus classify(ConstSlice<SfntOutlinePoint> points,ConstSlice<u32> ends,Slice<u8> directions,SfntOutlinePoint* marked,u32 origin,u32 work,u32* used) noexcept {
    struct Report {u32 initial;u32& work;u32* used;~Report() noexcept {if(used)*used=initial-work;}} report{work,work,used};
    if(origin>points.count||(points.count&&!points.data)||(ends.count&&!ends.data)||(!(points.count-origin))!=(!ends.count))return SfntStatus::invalid_input;
    if(points.count-origin>65535||ends.count>32767)return SfntStatus::unsupported;
    if(!marked&&(directions.count<ends.count||(ends.count&&!directions.data)))return SfntStatus::output_too_small;
    u32 first=origin;for(u32 c=0;c<ends.count;++c){if(!charge(work))return SfntStatus::resource_limit;if(ends[c]<first||ends[c]>=points.count)return SfntStatus::invalid_input;first=ends[c]+1;}
    if(first!=points.count)return SfntStatus::invalid_input;
    for(u32 i=origin;i<points.count;++i){if(!charge(work))return SfntStatus::resource_limit;const auto& p=points[i];
        if(!(p.x>=-32768&&p.x<=32767&&p.y>=-32768&&p.y<=32767))return SfntStatus::unsupported;
        if(static_cast<float>(static_cast<i32>(p.x))!=p.x||static_cast<float>(static_cast<i32>(p.y))!=p.y)return SfntStatus::unsupported;
    }
    first=origin;for(u32 c=0;c<ends.count;++c){const u32 end=ends[c];auto& direction=marked?marked[first].reserved[0]:directions[c];direction=0;if(end-first<2){first=end+1;continue;}
        u32 indices[4]{first,first,first,first};
        for(u32 i=first+1;i<=end;++i){if(!charge(work))return SfntStatus::resource_limit;const auto a=integer_point(points[i]);
            for(u32 d=0;d<4;++d){const u32 axis=d/2,other=1-axis,j=indices[d];const auto b=integer_point(points[j]);
                if((d&1?a.v[axis]>b.v[axis]:a.v[axis]<b.v[axis])||(a.v[axis]==b.v[axis]&&(!(points[j].flags&1)||((points[i].flags&1)&&((d==0||d==3)?a.v[other]<b.v[other]:a.v[other]>b.v[other])))))indices[d]=i;
            }
        }
        const auto left=integer_point(points[indices[0]]);i64 distance[4]{};
        for(u32 d=1;d<4;++d){const auto p=integer_point(points[indices[d]]);distance[d]=absolute(p.v[0]-left.v[0])+absolute(p.v[1]-left.v[1]);}
        const u32 second=distance[2]>distance[3]?2:3,other=second==2?3:2,third=distance[other]>distance[1]?other:1,order[]{0,second,third};u8 votes[3]{};
        for(u32 k=0;k<3;++k){if(!charge(work))return SfntStatus::resource_limit;const u32 d=order[k];const Ray ray(extremum(points,first,end,indices[d],d),d);u32 count=0,begin=origin,turn=0;
            for(u32 j=0;j<ends.count;++j){if(!charge(work))return SfntStatus::resource_limit;if(j!=c&&!crossings(points,begin,ends[j],ray,work,&count))return SfntStatus::resource_limit;begin=ends[j]+1;}
            if(!local_turn(points,first,end,indices[d],work,&turn))return SfntStatus::resource_limit;votes[k]=static_cast<u8>((((count&1)^1)*2)==turn);
            if(k==1&&votes[0]==votes[1]){direction=votes[1];break;}if(k==2)direction=votes[2];
        }first=end+1;
    }return SfntStatus::ok;
}
}
SfntStatus tt_contour_directions(ConstSlice<SfntOutlinePoint> points,ConstSlice<u32> ends,Slice<u8> directions,u32 work,u32* used) noexcept {
    return classify(points,ends,directions,nullptr,0,work,used);
}
SfntStatus tt_mark_contour_directions(Slice<SfntOutlinePoint> points,ConstSlice<u32> ends,u32 origin,u32 work,u32* used) noexcept {
    return classify({points.data,points.count},ends,{},points.data,origin,work,used);
}
}
#pragma float_control(pop)
