#ifndef COLR_STACK_SIZE
#define COLR_STACK_SIZE 8
#endif
// COLRv1 postfix paint evaluator. Results are premultiplied sRGB and retained
// in an RGBA atlas; no paint-tree work runs when drawing a cached glyph.
#define COLR_JOB_CAPACITY 64
struct ColrJob {
    uint2 destination_origin, destination_size;
    float2 outline_origin;
    float pixels_per_em;
    uint first_node, allocation_padding, node_count;
    uint program_base,plan_offset;
};
// One dispatch paints up to COLR_JOB_CAPACITY glyphs: group z selects the job.
cbuffer ColrConstants : register(b0) { ColrJob jobs[COLR_JOB_CAPACITY]; }
static uint2 destination_origin, destination_size;
static float2 outline_origin;
static float pixels_per_em;
static uint first_node, allocation_padding, node_count, program_base, plan_offset;
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
    if(mode>=24) {
        // Non-separable modes share one set_sat and set_lum: hue and saturation
        // keep a (24) or b (25) with the other's saturation; color and
        // luminosity swap which side supplies the luminosity.
        float3 x=(mode==25||mode==27)?b:a;
        if(mode<26)x=set_sat(x,saturation(mode==24?b:a));
        c=set_lum(x,dot(mode==27?a:b,float3(.3,.59,.11)));
    }
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
// One outline edge: its distance only when within a pixel of p (a farther edge
// cannot change the saturated coverage) and its winding crossing.
void edge_step(float4 edge,float2 p,float gutter2,inout int winding,inout float distance2) {
    float2 a=edge.xy,v=edge.zw-edge.xy,d=p-a;
    // Bounds gap from the edge's centre and half extent (cheaper than min/max of both ends).
    float2 gap=max(abs(d-.5*v)-.5*abs(v),0);
    if(dot(gap,gap)<gutter2) {
        float t=saturate(dot(d,v)*rcp(max(dot(v,v),1e-20)));
        float2 delta=d-t*v;distance2=min(distance2,dot(delta,delta));
    }
    // Crossing right of p without a division: the sign of the side test against
    // the edge direction replaces comparing the crossing abscissa.
    if((a.y<=p.y)!=(edge.w<=p.y)) {
        float side=v.x*d.y-d.x*v.y;
        if(side*v.y>0)winding+=v.y>0?1:-1;
    }
}
float4 load_edge(uint offset,uint index) { return asfloat(load4(offset+index*16)); }
float mask(uint offset,uint indices,uint count,uint fill,float2 p) {
    float gutter2=1/(pixels_per_em*pixels_per_em);
    int winding=0;float distance2=1e20;
    // Two edges per step: both indices, then both edges, load together instead
    // of one dependent pair per edge. A missing second edge is one far from p,
    // which adds neither distance nor winding.
    [loop] for(uint i=0;i<count;i+=2) {
        uint2 pair=load2(indices+i*4);
        float4 first=load_edge(offset,pair.x),second=load_edge(offset,pair.y);
        if(i+1==count)second=float4(1e9,1e9,1e9,1e9);
        edge_step(first,p,gutter2,winding,distance2);
        edge_step(second,p,gutter2,winding,distance2);
    }
    bool inside=fill!=0?winding!=0:((abs(winding)&1)!=0);
    return saturate(.5+(inside?1:-1)*sqrt(distance2)*pixels_per_em);
}
float shape_coverage(uint data,uint2 pixel,float2 p) {
    // Placement, header and bounds are independent loads of one record.
    const uint4 placement=load4(data+16),header=load4(data);
    const float4 bounds=asfloat(load4(data+32));
    const int2 local=int2(outline_origin)+int2(pixel-allocation_padding)-asint(placement.xy);
    const float gutter=1/pixels_per_em;
    if(any(local<0)||any(local>=int2(placement.zw))||any(p<bounds.xy-gutter)||any(p>bounds.zw+gutter))return 0;
    const uint2 row=load2(header.w+local.y*8);
    return row.y==0?0:mask(header.x,row.x,row.y,header.z,p);
}
#if COLR_LAYERS
// Each pixel's plan is cut into COLR_SLICES consecutive runs evaluated by
// separate threads: a pixel's layers are a serial chain of dependent loads.
#ifndef COLR_SLICES
#define COLR_SLICES 8
#endif
groupshared float4 slice_color[COLR_SLICES][64];
groupshared float slice_scale[COLR_SLICES][64];
[numthreads(8,8,COLR_SLICES)]
void colr_paint(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID) {
    const uint slice=local.z,lane=local.y*8+local.x;
    const uint3 thread=uint3(group.xy*8+local.xy,group.z);
#else
[numthreads(8,8,1)]
void colr_paint(uint3 thread : SV_DispatchThreadID) {
#endif
    const ColrJob job=jobs[thread.z];
    destination_origin=job.destination_origin;destination_size=job.destination_size;outline_origin=job.outline_origin;
    pixels_per_em=job.pixels_per_em;first_node=job.first_node;allocation_padding=job.allocation_padding;
    node_count=job.node_count;program_base=job.program_base;plan_offset=job.plan_offset;
#if COLR_LAYERS
    // Every thread of the group reaches the barrier below.
    const bool inside=all(thread.xy<destination_size);
    const bool body=inside&&all(thread.xy>=allocation_padding)&&all(thread.xy<destination_size-allocation_padding);
    float4 accumulated=0;float scale=1;
    [branch] if(body) {
    const float2 p=(outline_origin+float2(thread.xy-allocation_padding)+.5)/pixels_per_em;
    uint at=first_node;
    uint2 plan=uint2(0,node_count);
    if(plan_offset!=0) {
        uint columns=(destination_size.x+7)/8;
        plan=load2(plan_offset+((thread.y/8)*columns+thread.x/8)*8);
    }
    const uint paint_count=plan.y;
    // Without a plan the nodes chain by link and slice 0 walks them all.
    const uint first_index=plan_offset!=0?slice*paint_count/COLR_SLICES:0;
    const uint end_index=plan_offset!=0?(slice+1)*paint_count/COLR_SLICES:(slice==0?paint_count:0);
#else
    if(any(thread.xy>=destination_size))return;
    if(any(thread.xy<allocation_padding)||any(thread.xy>=destination_size-allocation_padding)) {
        color_atlas[destination_origin+thread.xy]=0;return;
    }
    const float2 p=(outline_origin+float2(thread.xy-allocation_padding)+.5)/pixels_per_em;
    float4 values[COLR_STACK_SIZE];uint top=0,at=first_node;
    const uint first_index=0,end_index=node_count;
#endif
    [loop]for(uint index=first_index;index<end_index;++index) {
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
        // The node's own fields load with its header, ahead of the mask lookup.
        const uint2 extra=load2(at+16);
        const float4 m=asfloat(load4(at+48)),a=asfloat(load4(at+24));
        const float2 translation=asfloat(load2(at+64)),b=asfloat(load2(at+40));
        if(shaped||kind==4)coverage=shape_coverage(shaped?extra.y:data,thread.xy,p);
        if(shaped) {
            if(coverage==0){
#if !COLR_LAYERS
                values[top++]=0;
#endif
                at=header.x;continue;
            }
            kind-=8;
        }
        float2 q=float2(p.x*m.x+p.y*m.z,p.x*m.y+p.y*m.w)+translation;
        uint mode=extra.x;
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
            const float clip=coverage.x*coverage.y;
            accumulated*=clip;scale*=clip;
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
            accumulated=leaf+accumulated*(1-leaf.a);
            scale*=1-leaf.a;
        }
#else
        } else values[top++]=0;
        if(shaped)values[top-1]*=coverage;
#endif
        at=header.x;
    }
#if COLR_LAYERS
    }
    slice_color[slice][lane]=accumulated;
    slice_scale[slice][lane]=scale;
    GroupMemoryBarrierWithGroupSync();
    if(slice==0&&inside) {
        // Source-over and root clips are affine in the colour below them
        // (below*scale + color), so the slices combine in layer order.
        float4 result=slice_color[0][lane];
        [unroll]for(uint other=1;other<COLR_SLICES;++other)result=result*slice_scale[other][lane]+slice_color[other][lane];
        color_atlas[destination_origin+thread.xy]=body?result:0;
    }
#else
    color_atlas[destination_origin+thread.xy]=top==1?values[0]:float4(0,0,0,0);
#endif
}
