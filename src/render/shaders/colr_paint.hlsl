#ifndef COLR_STACK_SIZE
#define COLR_STACK_SIZE 8
#endif
// COLRv1 postfix paint evaluator. Results are premultiplied sRGB and retained
// in an RGBA atlas; no paint-tree work runs when drawing a cached glyph.
cbuffer ColrConstants : register(b0) {
    uint2 destination_origin, destination_size;
    float2 outline_origin;
    float pixels_per_em;
    uint first_node, allocation_padding, node_count;
    uint program_base,plan_offset;
};
ByteAddressBuffer program : register(t0);
RWTexture2D<unorm float4> color_atlas : register(u0);
uint load1(uint at){return program.Load(program_base+at);}
uint2 load2(uint at){return program.Load2(program_base+at);}
uint4 load4(uint at){return program.Load4(program_base+at);}
float4 premultiply(float4 c) { return float4(c.rgb*c.a,c.a); }
float3 clip_color(float3 c) {
    float l=dot(c,float3(.3,.59,.11)),lo=min(c.r,min(c.g,c.b)),hi=max(c.r,max(c.g,c.b));
    if(lo<0)c=l+(c-l)*l/max(l-lo,1e-8);
    if(hi>1)c=l+(c-l)*(1-l)/max(hi-l,1e-8);
    return c;
}
float3 set_lum(float3 c,float l) {return clip_color(c+l-dot(c,float3(.3,.59,.11)));}
float saturation(float3 c) {return max(c.r,max(c.g,c.b))-min(c.r,min(c.g,c.b));}
float3 set_sat(float3 c,float s) {
    float lo=min(c.r,min(c.g,c.b)),hi=max(c.r,max(c.g,c.b));
    return hi>lo?(c-lo)*s/(hi>lo?hi-lo:1):0;
}
float4 composite(float4 s,float4 d,uint mode) {
    if(mode==0)return 0;
    if(mode==1)return s;
    if(mode==2)return d;
    if(mode==3)return s+d*(1-s.a);
    if(mode==4)return d+s*(1-d.a);
    if(mode==5)return s*d.a;
    if(mode==6)return d*s.a;
    if(mode==7)return s*(1-d.a);
    if(mode==8)return d*(1-s.a);
    if(mode==9)return s*d.a+d*(1-s.a);
    if(mode==10)return d*s.a+s*(1-d.a);
    if(mode==11)return s*(1-d.a)+d*(1-s.a);
    if(mode==12)return saturate(s+d);
    float3 a=s.a>0?s.rgb/(s.a>0?s.a:1):0,b=d.a>0?d.rgb/(d.a>0?d.a:1):0,c=0;
    if(mode==13)c=a+b-a*b;
    if(mode==14)c=b<=.5?2*a*b:1-2*(1-a)*(1-b);
    if(mode==15)c=min(a,b);
    if(mode==16)c=max(a,b);
    if(mode==17)c=b<=0?0:(a>=1?1:min(1,b/max(1-a,1e-8)));
    if(mode==18)c=b>=1?1:(a<=0?0:1-min(1,(1-b)/max(a,1e-8)));
    if(mode==19)c=a<=.5?2*a*b:1-2*(1-a)*(1-b);
    if(mode==20) {float3 q=b<=.25?((16*b-12)*b+4)*b:sqrt(b);c=a<=.5?b-(1-2*a)*b*(1-b):b+(2*a-1)*(q-b);}
    if(mode==21)c=abs(b-a);
    if(mode==22)c=a+b-2*a*b;
    if(mode==23)c=a*b;
    if(mode==24)c=set_lum(set_sat(a,saturation(b)),dot(b,float3(.3,.59,.11)));
    if(mode==25)c=set_lum(set_sat(b,saturation(a)),dot(b,float3(.3,.59,.11)));
    if(mode==26)c=set_lum(a,dot(b,float3(.3,.59,.11)));
    if(mode==27)c=set_lum(b,dot(a,float3(.3,.59,.11)));
    return float4(s.rgb*(1-d.a)+d.rgb*(1-s.a)+s.a*d.a*c,s.a+d.a-s.a*d.a);
}
// Match Windows DrawPaintGlyphRun: interpolate straight sRGB stops, then
// premultiply. Premultiplying stops first changes transparent-gradient hues.
float4 gradient(uint offset,uint count,uint extend,float t) {
    if(count==0)return 0;
    float first=asfloat(load1(offset));
    float last=asfloat(load1(offset+(count-1)*20));
    float span=last-first;
    if(span>0) {
        if(extend==1)t=first+frac((t-first)/span)*span;
        if(extend==2)t=first+(1-abs(1-((t-first)/span-2*floor((t-first)/span/2))))*span;
    } else if(extend!=0) return 0;
    float4 previous=asfloat(load4(offset+4));
    if(t<first)return premultiply(previous);
    float previous_t=first;
    [loop] for(uint i=1;i<count;++i) {
        float position=asfloat(load1(offset+i*20));
        float4 color=asfloat(load4(offset+i*20+4));
        if(t<position)return premultiply(lerp(previous,color,saturate((t-previous_t)/max(position-previous_t,1e-12))));
        previous=color;previous_t=position;
    }
    return premultiply(previous);
}
float mask(uint offset,uint indices,uint count,uint fill,float2 p,float4 bounds) {
    float gutter=1/pixels_per_em;
    if(any(p<bounds.xy-gutter)||any(p>bounds.zw+gutter)||count==0)return 0;
    int winding=0;float distance2=1e20;
    [loop] for(uint i=0;i<count;++i) {
        float4 edge=asfloat(load4(offset+load1(indices+i*4)*16));
        float2 a=edge.xy,b=edge.zw,v=b-a;
        float t=saturate(dot(p-a,v)/max(dot(v,v),1e-20));
        float2 delta=p-(a+t*v);distance2=min(distance2,dot(delta,delta));
        if((a.y<=p.y&&b.y>p.y)||(b.y<=p.y&&a.y>p.y))
            if(a.x+(p.y-a.y)*v.x/v.y>p.x)winding+=b.y>a.y?1:-1;
    }
    bool inside=fill!=0?winding!=0:((abs(winding)&1)!=0);
    return saturate(.5+(inside?1:-1)*sqrt(distance2)*pixels_per_em);
}
float shape_coverage(uint data,uint2 pixel) {
    float coverage=0;
    int2 local=int2(outline_origin)+int2(pixel-allocation_padding)-asint(load2(data+16));
    uint2 size=load2(data+24);
    if(all(local>=0)&&all(local<int2(size))) {
        uint4 header=load4(data);
        uint2 row=load2(header.w+local.y*8);
        float2 p=(float2(asint(load2(data+16)))+float2(local)+.5)/pixels_per_em;
        coverage=mask(header.x,row.x,row.y,header.z,p,asfloat(load4(data+32)));
    }
    return coverage;
}
[numthreads(8,8,1)]
void colr_paint(uint3 thread : SV_DispatchThreadID) {
    if(any(thread.xy>=destination_size))return;
    if(any(thread.xy<allocation_padding)||any(thread.xy>=destination_size-allocation_padding)) {
        color_atlas[destination_origin+thread.xy]=0;return;
    }
    float2 p=(outline_origin+float2(thread.xy-allocation_padding)+.5)/pixels_per_em;
#if COLR_LAYERS
    float4 accumulated=0;
    uint at=first_node;
    uint2 plan=uint2(0,node_count);
    if(plan_offset!=0) {
        uint columns=(destination_size.x+7)/8;
        plan=load2(plan_offset+((thread.y/8)*columns+thread.x/8)*8);
    }
    uint paint_count=plan.y;
#else
    float4 values[COLR_STACK_SIZE];uint top=0,at=first_node;
    uint paint_count=node_count;
#endif
    [loop]for(uint index=0;index<paint_count;++index) {
#if COLR_LAYERS
        if(plan_offset!=0)at=load1(plan.x+index*4);
#endif
        uint4 header=load4(at);
        uint kind=header.y,data=header.z,count=header.w;
#if COLR_LAYERS
        // The CPU admits only a left-associated source-over chain with root
        // clips. No general paint stack or composite node evaluation is needed.
        if(kind==6){at=header.x;continue;}
        float4 leaf=0;
#endif
        bool shaped=kind>=8&&kind<=11;float coverage=1;
        if(shaped||kind==4)coverage=shape_coverage(shaped?load1(at+20):data,thread.xy);
        if(shaped) {
            if(coverage==0){
#if !COLR_LAYERS
                values[top++]=0;
#endif
                at=header.x;continue;
            }
            kind-=8;
        }
        float4 m=asfloat(load4(at+24));float2 translation=asfloat(load2(at+40));
        float2 q=float2(p.x*m.x+p.y*m.z,p.x*m.y+p.y*m.w)+translation;
        float4 a=asfloat(load4(at+48));float2 b=asfloat(load2(at+64));
        uint mode=load1(at+16);
#if COLR_LAYERS
        if(kind==0)leaf=premultiply(a);
#else
        if(kind==0)values[top++]=premultiply(a);
#endif
        else if(kind<=3) {
            float t=0;bool valid=mode!=0xffffffff;
            if(kind==1) {
                float2 d=a.zw-a.xy,h=b.xy-a.xy,v=q-a.xy;
                float det=d.x*h.y-d.y*h.x;
                valid=valid&&abs(det)>1e-12;
                t=(v.x*h.y-v.y*h.x)/(valid?det:1);
            } else if(kind==2) {
                float2 d=a.zw-a.xy,v=q-a.xy;float r0=b.x,dr=b.y-b.x;
                float A=dot(d,d)-dr*dr,B=-2*(dot(v,d)+r0*dr),C=dot(v,v)-r0*r0;
                float discriminant=B*B-4*A*C;
                if(abs(A)<1e-10) {
                    valid=valid&&abs(B)>1e-10;t=-C/(valid?B:1);valid=valid&&(r0+t*dr>=0);
                } else {
                    valid=valid&&discriminant>=0;
                    float root=sqrt(max(0,discriminant));float t0=(-B-root)/(2*A),t1=(-B+root)/(2*A);
                    bool v0=r0+t0*dr>=0,v1=r0+t1*dr>=0;
                    valid=valid&&(v0||v1);t=v0&&v1?max(t0,t1):(v0?t0:t1);
                }
            }
            else {
                float2 d=q-a.xy;float angle=atan2(-d.y,d.x)*57.2957795131;
                if(angle<0)angle+=360;
                if(a.w==a.z) {
                    valid=valid&&mode==0;
                    t=angle<a.z?-1e20:1e20;
                } else t=(angle-a.z)/(a.w-a.z);
            }
#if COLR_LAYERS
            leaf=valid?gradient(data,count,mode,t):float4(0,0,0,0);
#else
            values[top++]=valid?gradient(data,count,mode,t):float4(0,0,0,0);
#endif
        } else if(kind==4) {
#if !COLR_LAYERS
            values[top-1]*=coverage;
#endif
        }
        else if(kind==5) {
            // Clip rectangle is in local space, including rotations and shear.
            float2 pixel_extent=.5*float2(length(m.xz),length(m.yw))/pixels_per_em;
            float2 coverage=saturate((min(q-a.xy,a.zw-q)+pixel_extent)/max(2*pixel_extent,1e-12));
#if COLR_LAYERS
            accumulated*=coverage.x*coverage.y;
#else
            values[top-1]*=coverage.x*coverage.y;
#endif
        } else if(kind==6) {
#if !COLR_LAYERS
            float4 upper=values[--top],lower=values[top-1];
            // Select operands before inlining the full composite evaluator.
            values[top-1]=composite(count?upper:lower,count?lower:upper,mode);
#endif
#if COLR_LAYERS
        }
        if(shaped) {
            leaf*=coverage;
            accumulated=index==0?leaf:leaf+accumulated*(1-leaf.a);
        }
#else
        } else values[top++]=0;
        if(shaped)values[top-1]*=coverage;
#endif
        at=header.x;
    }
#if COLR_LAYERS
    color_atlas[destination_origin+thread.xy]=accumulated;
#else
    color_atlas[destination_origin+thread.xy]=top==1?values[0]:float4(0,0,0,0);
#endif
}
