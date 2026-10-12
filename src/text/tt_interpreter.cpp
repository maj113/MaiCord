#include "maicord/text/tt_interpreter.hpp"
#include "maicord/core/byte_order.hpp"
#include "tt_math.hpp"
namespace maicord {namespace {
bool valid(ConstSlice<u8> b) noexcept {return !b.count || b.data;}
bool fits(i64 v) noexcept {return v>=-2147483648ll && v<=2147483647ll;}
i64 magnitude(i64 v) noexcept {return v<0?-v:v;}
i64 quotient(i64 numerator,i64 denominator) noexcept {const bool negative=(numerator<0)!=(denominator<0);const i64 n=magnitude(numerator),d=magnitude(denominator);const i64 q=(n+d/2)/d;return negative?-q:q;}
// Fixed-point projection multiplication adds a positive half before shifting;
// its negative ties differ from TrueType's explicit grid-rounding instructions.
i64 projected_product(i64 numerator,i64 denominator) noexcept {return numerator<0?-((-numerator+(denominator-1)/2)/denominator):(numerator+denominator/2)/denominator;}
bool multiply_divide(i64 a,i64 b,i64 denominator,i64* out) noexcept {
    if(!denominator)return false;const bool negative=(a<0)^(b<0)^(denominator<0);const u64 x=static_cast<u64>(magnitude(a)),y=static_cast<u64>(magnitude(b)),d=static_cast<u64>(magnitude(denominator));
    if(y && x>0xffffffffffffffffull/y)return false;const u64 product=x*y,q=product/d,round=product%d>=(d+1)/2?1:0;
    if(q>0x7fffffffffffffffull-round)return false;*out=negative?-static_cast<i64>(q+round):static_cast<i64>(q+round);return true;
}
using tt_math::normalized_vector;
i64 rounded(i32 v,const TtGraphicsState& g) noexcept {
    const i64 a=magnitude(static_cast<i64>(v));i64 value=a;
    switch(g.round){case TtRound::grid:value=(a+32)&~63ll;break;case TtRound::half_grid:value=(a&~63ll)+32;break;case TtRound::double_grid:value=(a+16)&~31ll;break;case TtRound::down:value=a&~63ll;break;case TtRound::up:value=(a+63)&~63ll;break;case TtRound::off:break;
    case TtRound::super:value=((a-g.round_phase+g.round_threshold)&-static_cast<i64>(g.round_period))+g.round_phase;if(value<0&&v)value=g.round_phase;break;}
    return v<0?-value:value;
}
struct Machine {
    TtInterpreter& s;TtExecution& result;TtPhase phase;u32 work,depth=0;ConstSlice<u8> code;u32 at=0;
    bool spend(u32 n=1) noexcept {if(n>work)return false;work-=n;result.operations+=n;return true;}
    bool push(i32 v) noexcept {if(s.stack_count>=s.stack.count)return false;s.stack[s.stack_count++]=v;return true;}
    bool pop(i32* v) noexcept {if(!s.stack_count)return false;*v=s.stack[--s.stack_count];return true;}
    bool two(i32* a,i32* b) noexcept {return pop(b) && pop(a);}
    SfntStatus skip(u8* op) noexcept {
        if(!spend())return SfntStatus::resource_limit;if(at>=code.count)return SfntStatus::truncated;*op=code[at++];u32 n=0,width=1;
        if(*op==0x40 || *op==0x41){if(at==code.count)return SfntStatus::truncated;n=code[at++];width=*op==0x41?2:1;}
        else if(*op>=0xb0 && *op<=0xbf){n=(*op&7)+1;width=*op>=0xb8?2:1;}
        if(n*width>code.count-at)return SfntStatus::truncated;at+=n*width;return SfntStatus::ok;
    }
    SfntStatus branch(bool allow_else) noexcept {u32 nesting=0;while(at<code.count){u8 op=0;const auto status=skip(&op);if(status!=SfntStatus::ok)return status;
        if(op==0x58)++nesting;else if(op==0x59){if(!nesting)return SfntStatus::ok;--nesting;}else if(op==0x1b && !nesting && allow_else)return SfntStatus::ok;}
        return SfntStatus::malformed;
    }
    TtPoint* point(u32 zone,i32 index) noexcept {if(zone>1 || index<0)return nullptr;const auto points=zone?s.glyph:s.twilight;return static_cast<u32>(index)<points.count?points.data+index:nullptr;}
    bool project(const TtPoint& p,bool original,i32* value) noexcept {const auto& g=s.graphics;const i16 x=original?g.dual_x:g.projection_x,y=original?g.dual_y:g.projection_y;
        const i64 v=projected_product(static_cast<i64>(original?p.original_x:p.x)*x,16384)+projected_product(static_cast<i64>(original?p.original_y:p.y)*y,16384);if(!fits(v))return false;*value=static_cast<i32>(v);return true;}
    SfntStatus move(TtPoint& p,i64 distance,bool projection) noexcept {auto& g=s.graphics;
        const i64 dot=projection?projected_product(static_cast<i64>(g.freedom_x)*g.projection_x,16384)+projected_product(static_cast<i64>(g.freedom_y)*g.projection_y,16384):16384;
        if(!dot)return SfntStatus::unsupported;i64 x=0,y=0;
        if(!multiply_divide(distance,g.freedom_x,dot,&x)||!multiply_divide(distance,g.freedom_y,dot,&y))return SfntStatus::malformed;
        if(x<-2147483648ll-p.x || x>2147483647ll-p.x || y<-2147483648ll-p.y || y>2147483647ll-p.y)return SfntStatus::malformed;p.x=static_cast<i32>(p.x+x);p.y=static_cast<i32>(p.y+y);if(g.freedom_x)p.touched|=1;if(g.freedom_y)p.touched|=2;return SfntStatus::ok;
    }
    bool distance(const TtPoint& a,const TtPoint& b,bool original,i64* value,bool unscaled=true,bool current_vector=false) noexcept {
        const i64 x=static_cast<i64>(original?a.original_x:a.x)-(original?b.original_x:b.x),y=static_cast<i64>(original?a.original_y:a.y)-(original?b.original_y:b.y);const auto& g=s.graphics;
        const i64 scale=original && unscaled?64:1;
        const i16 vx=original&&!current_vector?g.dual_x:g.projection_x,vy=original&&!current_vector?g.dual_y:g.projection_y;
        *value=(projected_product(x*vx,16384*scale)+projected_product(y*vy,16384*scale))*scale;return true;
    }
    SfntStatus vector_line(u8 op) noexcept {
        i32 a=0,b=0;if(!two(&a,&b))return SfntStatus::malformed;auto& g=s.graphics;const auto* p1=point(g.zp[2],b);const auto* p2=point(g.zp[1],a);if(!p1||!p2)return SfntStatus::malformed;
        i64 x=static_cast<i64>(p2->x)-p1->x,y=static_cast<i64>(p2->y)-p1->y;if(op&1){const i64 old=x;x=-y;y=old;}i16 vx=0,vy=0;normalized_vector(x,y,&vx,&vy);
        if(op==8 || op==9){g.freedom_x=vx;g.freedom_y=vy;}else{g.projection_x=g.dual_x=vx;g.projection_y=g.dual_y=vy;
            if(op>=0x86){x=static_cast<i64>(p2->original_x)-p1->original_x;y=static_cast<i64>(p2->original_y)-p1->original_y;if(op&1){const i64 old=x;x=-y;y=old;}normalized_vector(x,y,&g.dual_x,&g.dual_y);}}
        return SfntStatus::ok;
    }
    i64 single_width(i64 value) noexcept {const auto& g=s.graphics;return magnitude(magnitude(value)-g.single_width)<g.single_width_cut_in?(value<0?-static_cast<i64>(g.single_width):g.single_width):value;}
    SfntStatus relative(u8 op) noexcept {
        auto& g=s.graphics;i32 index=0,cvt=0;if(op>=0xe0 && !pop(&cvt))return SfntStatus::malformed;if(!pop(&index))return SfntStatus::malformed;
        auto* p=point(g.zp[1],index);auto* reference=point(g.zp[0],static_cast<i32>(g.rp[0]));if(!p||!reference)return SfntStatus::malformed;
        i64 target=0,original=0,current=0;
        if(op>=0xe0){if(cvt<0||static_cast<u32>(cvt)>=s.cvt.count)return SfntStatus::malformed;target=single_width(s.cvt[cvt]);
            if(!g.zp[1]){if(g.projection_x!=g.freedom_x || g.projection_y!=g.freedom_y)return SfntStatus::unsupported;
                const i64 x=reference->original_x+quotient(target*g.freedom_x,16384),y=reference->original_y+quotient(target*g.freedom_y,16384);if(!fits(x)||!fits(y))return SfntStatus::malformed;
                p->x=p->original_x=static_cast<i32>(x);p->y=p->original_y=static_cast<i32>(y);}
        }
        if(!distance(*p,*reference,true,&original,op<0xe0 && g.zp[0] && g.zp[1])||!distance(*p,*reference,false,&current))return SfntStatus::unsupported;
        if(op<0xe0)target=single_width(original);else{if(g.auto_flip && ((original<0)!=(target<0)))target=-target;if((op&4) && g.zp[0]==g.zp[1] && magnitude(target-original)>g.cvt_cut_in)target=original;}
        if(op&4){if(!fits(target))return SfntStatus::unsupported;target=rounded(static_cast<i32>(target),g);}
        if(op&8)target=original<0?minimum(target,-static_cast<i64>(g.minimum_distance)):maximum(target,static_cast<i64>(g.minimum_distance));
        const auto status=move(*p,target-current,true);if(status!=SfntStatus::ok)return status;g.rp[1]=g.rp[0];g.rp[2]=static_cast<u32>(index);if(op&16)g.rp[0]=static_cast<u32>(index);return SfntStatus::ok;
    }
    SfntStatus shift(u8 op) noexcept {
        auto& g=s.graphics;const u32 zone=op&1?g.zp[0]:g.zp[1],index=op&1?g.rp[1]:g.rp[2];auto* reference=point(zone,static_cast<i32>(index));if(!reference)return SfntStatus::malformed;
        i64 delta=0;if(!distance(*reference,TtPoint{reference->original_x,reference->original_y,reference->original_x,reference->original_y,0,0,0},false,&delta))return SfntStatus::unsupported;
        if(op<=0x33){if(g.loop>s.stack_count)return SfntStatus::malformed;if(!spend(g.loop))return SfntStatus::resource_limit;
            for(u32 i=0;i<g.loop;++i){i32 p=0;pop(&p);auto* target=point(g.zp[2],p);if(!target)return SfntStatus::malformed;const auto status=move(*target,delta,true);if(status!=SfntStatus::ok)return status;}g.loop=1;return SfntStatus::ok;}
        i32 item=0;if(!pop(&item)||item<0)return SfntStatus::malformed;u32 first=0,last=0,target_zone=g.zp[2];
        if(op<=0x35){if(target_zone!=1 || static_cast<u32>(item)>=s.contour_ends.count)return SfntStatus::malformed;first=item?s.contour_ends[item-1]+1:0;last=s.contour_ends[item]+1;if(first>last || last>s.glyph.count)return SfntStatus::malformed;}
        else{if(item>1)return SfntStatus::malformed;target_zone=static_cast<u32>(item);last=target_zone?(s.contour_ends.count?s.contour_ends[s.contour_ends.count-1]+1:0):s.twilight.count;if(target_zone && last>s.glyph.count)return SfntStatus::malformed;}
        if(!spend(last-first))return SfntStatus::resource_limit;
        for(u32 i=first;i<last;++i){if(target_zone==zone && i==index)continue;auto* p=point(target_zone,static_cast<i32>(i));if(!p)return SfntStatus::malformed;const u8 touched=p->touched;const auto status=move(*p,delta,true);if(status!=SfntStatus::ok)return status;if(op>=0x36)p->touched=touched;}return SfntStatus::ok;
    }
    SfntStatus iup(u32 axis) noexcept {
        if(s.graphics.zp[2]!=1)return SfntStatus::unsupported;const u8 mask=axis?2:1;u32 first=0;
        for(u32 contour=0;contour<s.contour_ends.count;++contour){if(!spend())return SfntStatus::resource_limit;const u32 last=s.contour_ends[contour];if(last<first || last>=s.glyph.count)return SfntStatus::malformed;
            u32 initial=last+1;for(u32 i=first;i<=last;++i){if(!spend())return SfntStatus::resource_limit;if(s.glyph[i].touched&mask){initial=i;break;}}
            if(initial<=last){u32 a=initial;do{u32 b=a==last?first:a+1;while(b!=initial && !(s.glyph[b].touched&mask)){if(!spend())return SfntStatus::resource_limit;b=b==last?first:b+1;}
                const auto pa=s.glyph[a],pb=s.glyph[b];i64 oa=axis?pa.original_y:pa.original_x,ob=axis?pb.original_y:pb.original_x,da=(axis?pa.y:pa.x)-oa,db=(axis?pb.y:pb.x)-ob;
                if(oa>ob){const i64 p=oa;oa=ob;ob=p;const i64 d=da;da=db;db=d;}
                // The ratio maps original font units to F26Dot6 points using
                // 16.16 precision. Our originals carry six extra fraction bits,
                // hence the equivalent 2^22 scale. Prepare once per interval.
                const i64 span=(ob+db)-(oa+da);
                const bool short_span=span>=-32768 && span<=32767;
                const i64 scale=oa==ob || short_span?0:quotient(span*4194304,ob-oa);
                for(u32 i=a==last?first:a+1;i!=b;i=i==last?first:i+1){if(!spend())return SfntStatus::resource_limit;auto& p=s.glyph[i];const i64 old=axis?p.original_y:p.original_x;
                    // The outline profile uses the following contour
                    // anchor's shift when both original coordinates are equal.
                    // With one touched anchor Windows shifts current points;
                    // with two anchors it interpolates from original points.
                    // The short signed-word interval path interpolates directly;
                    // larger intervals use the prepared fixed-point ratio.
                    // Both add a positive half and divide toward zero, including
                    // reversed current spans; this differs from projection ties.
                    const i64 value=a==b?(axis?p.y:p.x)+db:oa==ob?old+db:old<=oa?old+da:old>=ob?old+db:
                        oa+da+(short_span?(span*(old-oa)+(ob-oa)/2)/(ob-oa):(scale*(old-oa)+2097152)/4194304);
                    if(!fits(value))return SfntStatus::malformed;if(axis)p.y=static_cast<i32>(value);else p.x=static_cast<i32>(value);}
                a=b;}while(a!=initial);}
            first=last+1;
        }return SfntStatus::ok;
    }
    SfntStatus run() noexcept {
        while(true){
            if(at==code.count){if(!depth)return SfntStatus::ok;if(!spend())return SfntStatus::resource_limit;auto& frame=s.frames[depth-1];if(frame.remaining>1){--frame.remaining;at=0;continue;}code=frame.code;at=frame.offset;--depth;continue;}
            if(at>code.count)return SfntStatus::malformed;result.byte_offset=at;result.opcode=code[at];result.call_depth=depth;
            if(!spend())return SfntStatus::resource_limit;const u8 op=code[at++];i32 a=0,b=0;auto& g=s.graphics;
            if(op==0x40 || op==0x41 || (op>=0xb0 && op<=0xbf)){
                u32 n=(op&7u)+1,width=op>=0xb8?2:1;if(op==0x40 || op==0x41){if(at==code.count)return SfntStatus::truncated;n=code[at++];width=op==0x41?2:1;}
                if(n*width>code.count-at)return SfntStatus::truncated;if(n>s.stack.count-s.stack_count)return SfntStatus::output_too_small;if(!spend(n))return SfntStatus::resource_limit;
                for(u32 i=0;i<n;++i){push(width==1?code[at]:static_cast<i16>(byte_load_be16(code.data+at)));at+=width;}continue;
            }
            if(op<=5){const i16 x=op&1?16384:0,y=op&1?0:16384;if(op<4){g.projection_x=g.dual_x=x;g.projection_y=g.dual_y=y;}if(op<2 || op>=4){g.freedom_x=x;g.freedom_y=y;}continue;}
            if(op>=0x10 && op<=0x12){if(!pop(&a)||a<0)return SfntStatus::malformed;g.rp[op-0x10]=static_cast<u32>(a);continue;}
            if(op>=0x13 && op<=0x16){if(!pop(&a)||a<0||a>1)return SfntStatus::malformed;if(op==0x16)g.zp[0]=g.zp[1]=g.zp[2]=static_cast<u32>(a);else g.zp[op-0x13]=static_cast<u32>(a);continue;}
            if(op>=0x50 && op<=0x55){if(!two(&a,&b))return SfntStatus::malformed;const bool v=op==0x50?a<b:op==0x51?a<=b:op==0x52?a>b:op==0x53?a>=b:op==0x54?a==b:a!=b;if(!push(v))return SfntStatus::output_too_small;continue;}
            if(op>=0x68 && op<=0x6f){if(!pop(&a))return SfntStatus::malformed;const i64 value=op<0x6c?rounded(a,g):a;if(!fits(value))return SfntStatus::malformed;if(!push(static_cast<i32>(value)))return SfntStatus::output_too_small;continue;}
            if(op>=0xc0){const auto status=relative(op);if(status!=SfntStatus::ok)return status;continue;}
            if(op>=0x32 && op<=0x37){const auto status=shift(op);if(status!=SfntStatus::ok)return status;continue;}
            switch(op){
            case 0x06:case 0x07:case 0x08:case 0x09:case 0x86:case 0x87:{const auto status=vector_line(op);if(status!=SfntStatus::ok)return status;break;}
            case 0x0a:case 0x0b:{if(!two(&a,&b))return SfntStatus::malformed;const i16 x=static_cast<i16>(a),y=static_cast<i16>(b);if(op==0x0a){g.projection_x=g.dual_x=x;g.projection_y=g.dual_y=y;}else{g.freedom_x=x;g.freedom_y=y;}break;}
            case 0x0c:case 0x0d:if(!push(op==0x0c?g.projection_x:g.freedom_x)||!push(op==0x0c?g.projection_y:g.freedom_y))return SfntStatus::output_too_small;break;
            case 0x0e:g.freedom_x=g.projection_x;g.freedom_y=g.projection_y;break;
            case 0x17:if(!pop(&a)||a<0)return SfntStatus::malformed;g.loop=static_cast<u32>(a);break;
            case 0x18:g.round=TtRound::grid;break;case 0x19:g.round=TtRound::half_grid;break;case 0x3d:g.round=TtRound::double_grid;break;case 0x7a:g.round=TtRound::off;break;case 0x7c:g.round=TtRound::up;break;case 0x7d:g.round=TtRound::down;break;
            case 0x1a:case 0x1d:case 0x1e:case 0x1f:if(!pop(&a))return SfntStatus::malformed;if(op==0x1a)g.minimum_distance=a;else if(op==0x1d)g.cvt_cut_in=a;else if(op==0x1e)g.single_width_cut_in=a;else{if(!fits(static_cast<i64>(a)*64))return SfntStatus::malformed;g.single_width=a*64;}break;
            case 0x1b:{const auto status=branch(false);if(status!=SfntStatus::ok)return status;break;}
            case 0x1c:case 0x78:case 0x79:{if(op==0x1c){if(!pop(&a))return SfntStatus::malformed;b=1;}else if(!two(&a,&b))return SfntStatus::malformed;
                if(op==0x1c || (op==0x78?b!=0:b==0)){const i64 destination=static_cast<i64>(at)-1+a;if(destination<0 || destination>code.count)return SfntStatus::malformed;at=static_cast<u32>(destination);}break;}
            case 0x20:if(!s.stack_count)return SfntStatus::malformed;if(!push(s.stack[s.stack_count-1]))return SfntStatus::output_too_small;break;
            case 0x21:if(!pop(&a))return SfntStatus::malformed;break;case 0x22:s.stack_count=0;break;
            case 0x23:if(s.stack_count<2)return SfntStatus::malformed;a=s.stack[s.stack_count-1];s.stack[s.stack_count-1]=s.stack[s.stack_count-2];s.stack[s.stack_count-2]=a;break;
            case 0x24:if(!push(static_cast<i32>(s.stack_count)))return SfntStatus::output_too_small;break;
            case 0x25:case 0x26:if(!pop(&a)||a<=0||static_cast<u32>(a)>s.stack_count)return SfntStatus::malformed;b=s.stack[s.stack_count-a];if(op==0x26){if(!spend(static_cast<u32>(a)))return SfntStatus::resource_limit;for(u32 i=s.stack_count-a;i+1<s.stack_count;++i)s.stack[i]=s.stack[i+1];--s.stack_count;}if(!push(b))return SfntStatus::output_too_small;break;
            case 0x2a:case 0x2b:{if(!pop(&a)||a<0)return SfntStatus::malformed;
                if(static_cast<u32>(a)>=s.functions.count||!s.functions[a].defined){result.missing_function=true;return SfntStatus::malformed;}
                if(!valid(s.functions[a].code))return SfntStatus::malformed;b=1;if(op==0x2a && (!pop(&b)||b<0))return SfntStatus::malformed;if(!b)break;if(depth>=s.frames.count)return SfntStatus::resource_limit;s.frames[depth++]={code,at,static_cast<u32>(b)};code=s.functions[a].code;at=0;break;}
            case 0x2c:{if(phase==TtPhase::glyph)return SfntStatus::unsupported;if(!pop(&a)||a<0)return SfntStatus::malformed;if(static_cast<u32>(a)>=s.functions.count)return SfntStatus::output_too_small;const u32 first=at;u8 found=0;while(at<code.count){const auto status=skip(&found);if(status!=SfntStatus::ok)return status;if(found==0x2c || found==0x89)return SfntStatus::malformed;if(found==0x2d)break;}if(found!=0x2d)return SfntStatus::malformed;s.functions[a]={{code.data+first,at-first-1},true};break;}
            case 0x2d:return SfntStatus::malformed;
            case 0x29:{if(!pop(&a))return SfntStatus::malformed;auto* p=point(g.zp[0],a);if(!p)return SfntStatus::malformed;const u8 mask=static_cast<u8>((g.freedom_x?1:0)|(g.freedom_y?2:0));p->touched&=static_cast<u8>(~mask);break;}
            case 0x2e:case 0x2f:{if(!pop(&a))return SfntStatus::malformed;auto* p=point(g.zp[0],a);if(!p)return SfntStatus::malformed;i32 position=0;if(!project(*p,false,&position))return SfntStatus::unsupported;const auto status=move(*p,(op&1)?rounded(position,g)-position:0,true);if(status!=SfntStatus::ok)return status;g.rp[0]=g.rp[1]=static_cast<u32>(a);break;}
            case 0x30:case 0x31:{const auto status=iup(op==0x30?1:0);if(status!=SfntStatus::ok)return status;break;}
            case 0x39:{auto* p1=point(g.zp[0],static_cast<i32>(g.rp[1]));auto* p2=point(g.zp[1],static_cast<i32>(g.rp[2]));if(!p1||!p2||g.loop>s.stack_count)return SfntStatus::malformed;
                const bool unscaled=g.zp[0] && g.zp[1] && g.zp[2];i64 old_range=0,new_range=0;if(!distance(*p2,*p1,true,&old_range,unscaled)||!distance(*p2,*p1,false,&new_range))return SfntStatus::unsupported;if(!spend(g.loop))return SfntStatus::resource_limit;
                // The normalized-outline profile uses the dual vector
                // for the original reference span, but the current projection
                // vector for each original glyph-point distance.
                for(u32 i=0;i<g.loop;++i){pop(&a);auto* p=point(g.zp[2],a);if(!p)return SfntStatus::malformed;i64 old=0,current=0,target=0;if(!distance(*p,*p1,true,&old,unscaled,unscaled)||!distance(*p,*p1,false,&current))return SfntStatus::unsupported;
                    // The normalized Windows path's zero-range fallback uses
                    // the unscaled design-unit distance as a fixed-point delta.
                    if(old_range){if(!multiply_divide(old,new_range,old_range,&target))return SfntStatus::malformed;}else target=old/64;
                    if(target<-17179869180ll || target>17179869180ll)return SfntStatus::malformed;
                    const auto status=move(*p,target-current,true);if(status!=SfntStatus::ok)return status;}g.loop=1;break;}
            case 0x3c:{auto* ref=point(g.zp[0],static_cast<i32>(g.rp[0]));if(!ref||g.loop>s.stack_count)return SfntStatus::malformed;if(!spend(g.loop))return SfntStatus::resource_limit;
                for(u32 i=0;i<g.loop;++i){pop(&a);auto* p=point(g.zp[1],a);if(!p)return SfntStatus::malformed;i64 current=0;if(!distance(*p,*ref,false,&current))return SfntStatus::unsupported;const auto status=move(*p,-current,true);if(status!=SfntStatus::ok)return status;}g.loop=1;break;}
            case 0x3a:case 0x3b:{if(!two(&a,&b))return SfntStatus::malformed;auto* p=point(g.zp[1],a);auto* ref=point(g.zp[0],static_cast<i32>(g.rp[0]));if(!p||!ref)return SfntStatus::malformed;
                if(!g.zp[1]){const i64 x=ref->original_x+quotient(static_cast<i64>(b)*g.freedom_x,16384),y=ref->original_y+quotient(static_cast<i64>(b)*g.freedom_y,16384);if(!fits(x)||!fits(y))return SfntStatus::malformed;p->x=p->original_x=static_cast<i32>(x);p->y=p->original_y=static_cast<i32>(y);}
                i64 current=0;if(!distance(*p,*ref,false,&current))return SfntStatus::unsupported;const auto status=move(*p,static_cast<i64>(b)-current,true);if(status!=SfntStatus::ok)return status;g.rp[1]=g.rp[0];g.rp[2]=static_cast<u32>(a);if(op&1)g.rp[0]=static_cast<u32>(a);break;}
            case 0x3e:case 0x3f:{if(!two(&a,&b)||b<0||static_cast<u32>(b)>=s.cvt.count)return SfntStatus::malformed;auto* p=point(g.zp[0],a);if(!p)return SfntStatus::malformed;i64 target=s.cvt[b];
                if(!g.zp[0]){const i64 x=quotient(target*g.freedom_x,16384),y=quotient(target*g.freedom_y,16384);if(!fits(x)||!fits(y))return SfntStatus::malformed;p->x=p->original_x=static_cast<i32>(x);p->y=p->original_y=static_cast<i32>(y);}
                i32 current=0;if(!project(*p,false,&current))return SfntStatus::unsupported;if(op&1){if(magnitude(target-current)>g.cvt_cut_in)target=current;target=rounded(static_cast<i32>(target),g);}
                const auto status=move(*p,target-current,true);if(status!=SfntStatus::ok)return status;g.rp[0]=g.rp[1]=static_cast<u32>(a);break;}
            case 0x38:{if(!pop(&b))return SfntStatus::malformed;if(g.loop>s.stack_count)return SfntStatus::malformed;if(!spend(g.loop))return SfntStatus::resource_limit;for(u32 i=0;i<g.loop;++i){pop(&a);auto* p=point(g.zp[2],a);if(!p)return SfntStatus::malformed;const auto status=move(*p,b,false);if(status!=SfntStatus::ok)return status;}g.loop=1;break;}
            case 0x42:case 0x44:case 0x70:{if(!two(&a,&b)||a<0)return SfntStatus::malformed;const auto target=op==0x42?s.storage:s.cvt;if(static_cast<u32>(a)>=target.count)return SfntStatus::malformed;const i64 value=op==0x70?static_cast<i64>(b)*64:b;if(!fits(value))return SfntStatus::malformed;target[a]=static_cast<i32>(value);break;}
            case 0x43:case 0x45:{if(!pop(&a)||a<0)return SfntStatus::malformed;const auto target=op==0x43?s.storage:s.cvt;if(static_cast<u32>(a)>=target.count)return SfntStatus::malformed;if(!push(target[a]))return SfntStatus::output_too_small;break;}
            case 0x46:case 0x47:{if(!pop(&a))return SfntStatus::malformed;auto* p=point(g.zp[2],a);if(!p)return SfntStatus::malformed;if(!project(*p,op==0x47,&b))return SfntStatus::unsupported;if(!push(b))return SfntStatus::output_too_small;break;}
            case 0x48:{if(!two(&a,&b))return SfntStatus::malformed;auto* p=point(g.zp[2],a);i32 old=0;if(!p)return SfntStatus::malformed;if(!project(*p,false,&old))return SfntStatus::unsupported;const auto status=move(*p,static_cast<i64>(b)-old,true);if(status!=SfntStatus::ok)return status;break;}
            case 0x4b:case 0x4c:if(!push(op==0x4b?static_cast<i32>(s.units_per_em):0))return SfntStatus::output_too_small;break;
            case 0x49:case 0x4a:{if(!two(&a,&b))return SfntStatus::malformed;auto* p1=point(g.zp[0],a);auto* p2=point(g.zp[1],b);if(!p1||!p2)return SfntStatus::malformed;i64 d=0;if(!distance(*p1,*p2,op==0x4a,&d,g.zp[0] && g.zp[1])||!fits(d))return SfntStatus::unsupported;if(!push(static_cast<i32>(d)))return SfntStatus::output_too_small;break;}
            case 0x4d:g.auto_flip=true;break;case 0x4e:g.auto_flip=false;break;
            case 0x56:case 0x57:if(!pop(&a))return SfntStatus::malformed;if(!push(((magnitude(rounded(a,g))/64)&1)==(op==0x56?1:0)))return SfntStatus::output_too_small;break;
            case 0x58:if(!pop(&a))return SfntStatus::malformed;if(!a){const auto status=branch(true);if(status!=SfntStatus::ok)return status;}break;case 0x59:break;
            case 0x5a:case 0x5b:if(!two(&a,&b))return SfntStatus::malformed;if(!push(op==0x5a?(a!=0 && b!=0):(a!=0 || b!=0)))return SfntStatus::output_too_small;break;
            case 0x5c:if(!pop(&a))return SfntStatus::malformed;if(!push(!a))return SfntStatus::output_too_small;break;
            case 0x5d:case 0x71:case 0x72:case 0x73:case 0x74:case 0x75:{
                if(!pop(&a)||a<0||static_cast<u32>(a)>s.stack_count/2 || g.delta_shift>6)return SfntStatus::malformed;
                const u32 count=static_cast<u32>(a),band=op==0x5d?0:op>=0x73?op-0x73:op-0x70;const bool control=op>=0x73;
                if(!spend(count))return SfntStatus::resource_limit;
                for(u32 i=0;i<count;++i){if(!two(&a,&b)||a<0||a>255||b<0)return SfntStatus::malformed;
                    if(static_cast<u64>(g.delta_base)+band*16+static_cast<u32>(a>>4)!=s.units_per_em)continue;
                    const i32 steps=(a&15)<8?(a&15)-8:(a&15)-7,delta=steps*(64>>g.delta_shift);
                    if(control){if(static_cast<u32>(b)>=s.cvt.count || !fits(static_cast<i64>(s.cvt[b])+delta))return SfntStatus::malformed;s.cvt[b]+=delta;}
                    else{auto* p=point(g.zp[0],b);if(!p)return SfntStatus::malformed;const auto status=move(*p,delta,true);if(status!=SfntStatus::ok)return status;}
                }break;}
            case 0x5e:case 0x5f:if(!pop(&a)||a<0)return SfntStatus::malformed;if(op==0x5e)g.delta_base=static_cast<u32>(a);else{if(a>6)return SfntStatus::malformed;g.delta_shift=static_cast<u32>(a);}break;
            case 0x76:{if(!pop(&a))return SfntStatus::malformed;a&=255;if((a>>6)==3)return SfntStatus::malformed;const i32 period=32<<(a>>6);g.round=TtRound::super;g.round_period=static_cast<i16>(period);g.round_phase=static_cast<i16>(((a>>4)&3)*period/4);g.round_threshold=static_cast<i16>((a&15)?((a&15)-4)*period/8:period-1);break;}
            case 0x60:case 0x61:case 0x62:case 0x63:{if(!two(&a,&b))return SfntStatus::malformed;i64 value=0;if(op==0x60)value=static_cast<i32>(static_cast<u32>(a)+static_cast<u32>(b));else if(op==0x61)value=static_cast<i32>(static_cast<u32>(a)-static_cast<u32>(b));else if(op==0x62){if(!b)return SfntStatus::malformed;value=quotient(static_cast<i64>(a)*64,b);}else value=quotient(static_cast<i64>(a)*b,64);if(!fits(value))return SfntStatus::malformed;if(!push(static_cast<i32>(value)))return SfntStatus::output_too_small;break;}
            case 0x64:case 0x65:case 0x66:case 0x67:{if(!pop(&a))return SfntStatus::malformed;const i64 value=op==0x64?magnitude(static_cast<i64>(a)):op==0x65?-static_cast<i64>(a):op==0x66?(static_cast<i64>(a)&~63ll):((static_cast<i64>(a)+63)&~63ll);if(!fits(value))return SfntStatus::malformed;if(!push(static_cast<i32>(value)))return SfntStatus::output_too_small;break;}
            case 0x85:case 0x8d:if(!pop(&a))return SfntStatus::malformed;if(op==0x85)g.scan_control=static_cast<u32>(a);else g.scan_type=static_cast<u32>(a);break;
            case 0x88:if(!pop(&a))return SfntStatus::malformed;if(!push(static_cast<i32>(tt_outline_information(static_cast<u32>(a)))))return SfntStatus::output_too_small;break;
            case 0x8a:if(s.stack_count<3)return SfntStatus::malformed;a=s.stack[s.stack_count-3];s.stack[s.stack_count-3]=s.stack[s.stack_count-2];s.stack[s.stack_count-2]=s.stack[s.stack_count-1];s.stack[s.stack_count-1]=a;break;
            case 0x8b:case 0x8c:if(!two(&a,&b))return SfntStatus::malformed;if(!push(op==0x8b?maximum(a,b):minimum(a,b)))return SfntStatus::output_too_small;break;
            case 0x8e:if(!two(&a,&b)||b<1||b>3 || (a!=0 && a!=(1<<(b-1))))return SfntStatus::malformed;g.instruction_control=(g.instruction_control&~(1u<<(b-1)))|static_cast<u32>(a);break;
            default:return SfntStatus::unsupported;
            }
        }
    }
};
}
u32 tt_outline_information(u32 selector) noexcept {return (selector&1?42u:0u)|(selector&8?1024u:0u)|(selector&16?2048u:0u);}
SfntStatus tt_execute(TtInterpreter* state,ConstSlice<u8> program,TtPhase phase,TtExecution* result,u32 work) noexcept {
    if(!result)return SfntStatus::invalid_input;*result={};
    if(!state || !valid(program) || !state->units_per_em || state->stack_count>state->stack.count || (state->stack.count && !state->stack.data) || (state->storage.count && !state->storage.data) || (state->cvt.count && !state->cvt.data) || (state->functions.count && !state->functions.data) || (state->frames.count && !state->frames.data) || (state->glyph.count && !state->glyph.data) || (state->twilight.count && !state->twilight.data) || (state->contour_ends.count && !state->contour_ends.data))return SfntStatus::invalid_input;
    if(phase==TtPhase::glyph && (state->graphics.instruction_control&1))return SfntStatus::ok;
    Machine machine{*state,*result,phase,work,0,program,0};const auto status=machine.run();result->stack_count=state->stack_count;result->call_depth=machine.depth;return status;
}
} // namespace maicord
