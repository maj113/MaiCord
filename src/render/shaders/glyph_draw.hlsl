cbuffer UiFrame : register(b0) {
    float2 coordinate_scale;
    float2 coordinate_translation;
    float group_opacity;
    float3 frame_reserved;
};
struct TextEffectMaterial {
    float4 colors[8];
    float left, width, kind, count;
    float animate; float3 reserved;
};
cbuffer TextEffects : register(b1) {
    float effect_seconds, effect_scale; float2 effect_reserved;
    TextEffectMaterial effects[128];
};

struct GlyphVertexInput {
    float4 rectangle : RECT;
    float4 texture_rectangle : TEXCOORD0;
    float4 color : COLOR0;
};

struct GlyphVertexOutput {
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
    float4 color : COLOR0;
    nointerpolation float4 texture_bounds : TEXCOORD1;
    // xy: atlas scale; z: glyph-uniform flash or travel, evaluated per corner.
    nointerpolation float3 glyph_parameters : TEXCOORD2;
};

#if defined(MAICORD_GLYPH_TRIANGLE_LIST_REFERENCE)
static const float2 kGlyphCorners[6] = {
    float2(0.0f, 0.0f),
    float2(1.0f, 0.0f),
    float2(0.0f, 1.0f),
    float2(0.0f, 1.0f),
    float2(1.0f, 0.0f),
    float2(1.0f, 1.0f),
};
#else
// One instanced strip preserves both triangles without shading shared corners twice.
static const float2 kGlyphCorners[4] = {
    float2(0.0f, 0.0f), float2(1.0f, 0.0f),
    float2(0.0f, 1.0f), float2(1.0f, 1.0f),
};
#endif

// Independent front/extrusion transforms share a four-second clock and rest
// after the first half-cycle. Geometry remains retained across all frames.
float curve_ease(float t,float4 p) {
    float u=saturate(t);
    [unroll] for(int i=0;i<4;i++) {
        float v=1-u;
        float x=3*p.x*v*v*u+3*p.z*v*u*u+u*u*u;
        float d=3*p.x*v*v+6*(p.z-p.x)*v*u+3*(1-p.z)*u*u;
        u=saturate(u-(x-t)/max(d,.001));
    }
    return 3*p.y*(1-u)*(1-u)*u+3*p.w*(1-u)*u*u+u*u*u;
}
float profile_ease(float t) { return curve_ease(t,float4(.44,.29,.48,1));
}
float pop_offset(float cycle,bool shadow) {
    float3 offsets=shadow?float3(.08,.13,0):float3(0,-.05,.08);
    if(cycle>=.5)return offsets.x;
    float2 endpoints;float t;
    if(cycle<.18){endpoints=offsets.xy;t=cycle/.18;}
    else if(cycle<.35){endpoints=offsets.yz;t=(cycle-.18)/.17;}
    else {endpoints=offsets.zx;t=(cycle-.35)/.15;}
    return lerp(endpoints.x,endpoints.y,profile_ease(t));
}
float profile_cycle(TextEffectMaterial effect) { return frac((effect_seconds-effect.reserved.x+60)/4); }

float gummy_scale(float cycle) {
    if(cycle>=.35)return 1;
    float2 endpoints;float t;
    if(cycle<.07){endpoints=float2(1,1.28);t=cycle/.07;}
    else if(cycle<.14){endpoints=float2(1.28,.82);t=(cycle-.07)/.07;}
    else if(cycle<.21){endpoints=float2(.82,1.1);t=(cycle-.14)/.07;}
    else if(cycle<.28){endpoints=float2(1.1,.96);t=(cycle-.21)/.07;}
    else {endpoints=float2(.96,1);t=(cycle-.28)/.07;}
    return lerp(endpoints.x,endpoints.y,profile_ease(t));
}
float neon_flash(float cycle);
GlyphVertexOutput glyph_vertex(GlyphVertexInput input, uint vertex_id : SV_VertexID) {
    GlyphVertexOutput output;
    const float2 corner = kGlyphCorners[vertex_id];
    float4 rectangle=input.rectangle, uv=input.texture_rectangle;
    const bool color_glyph=uv.x>=2;
    if(color_glyph)uv.xz-=2;
    // Preserve the atlas-kind tag here; color sampling returns before using bounds.
    output.texture_bounds=input.texture_rectangle;
    output.glyph_parameters=0;
    if(input.color.r<0 && input.texture_rectangle.x>=0 && !color_glyph) {
        const TextEffectMaterial effect=effects[min(127u,(uint)(-input.color.r-1))];
        if(effect.kind==3) {
            output.glyph_parameters.xy=(uv.zw-uv.xy)/max(rectangle.zw-rectangle.xy,float2(.001,.001));
            float radius=4+.12*effect.reserved.y;
            rectangle+=float4(-radius,-radius,radius,radius);
            uv+=float4(-output.glyph_parameters.xy,output.glyph_parameters.xy)*radius;
        }
    }
    float2 position=lerp(rectangle.xy,rectangle.zw,corner);
    if(input.color.r<0 && !color_glyph) {
        const TextEffectMaterial effect=effects[min(127u,(uint)(-input.color.r-1))];
        if(effect.kind==3 && input.texture_rectangle.x>=0)
            output.glyph_parameters.z=effect.animate*neon_flash(profile_cycle(effect));
        else if(input.color.b<0 || effect.kind==4) {
            float progress=input.color.b<0?profile_cycle(effect)*2:(profile_cycle(effect)-.05)/.5;
            output.glyph_parameters.z=profile_ease(saturate(progress))*effect.animate;
        }
        if(effect.kind==8 && effect.animate>0 && input.texture_rectangle.x>=0) {
            float cycle=frac((effect_seconds-effect.reserved.x+60-input.color.g*.05)/4);
            float scale=gummy_scale(cycle);
            float2 center=float2(input.color.b,effect.reserved.z+effect.reserved.y*.65);
            position=center+(position-center)*float2(scale,2-scale);
        }
        if(effect.kind==5) position.y+=pop_offset(effect.animate>0?profile_cycle(effect):.75,
            input.color.b<0&&input.color.g>1.5)*effect.reserved.y;
    }
    output.position=float4(position*coordinate_scale+coordinate_translation,0,1);
    output.texture_coordinate=lerp(uv.xy,uv.zw,corner);
    output.color = input.color;
    output.color.a *= group_opacity;
    return output;
}

Texture2D<float> glyph_atlas : register(t0);
Texture2D<float4> color_glyph_atlas : register(t1);
SamplerState glyph_sampler : register(s0);

// Keep expanded halo samples within this glyph's allocation, including at atlas edges.
float glyph_mask(float2 uv,float4 bounds) {
    if(any(uv<bounds.xy)||any(uv>bounds.zw))return 0;
    return glyph_atlas.Sample(glyph_sampler,uv);
}
float3 toon_color(TextEffectMaterial effect,float t) {
    const float stops[10]={0,.08,.15,.25,.45,.55,.75,.83,.90,1};
    const uint indices[10]={7,2,1,0,2,0,7,2,1,0};
    float3 a=effect.colors[0].rgb,b=a;float blend=0;
    [unroll]for(uint i=0;i<9;i++)if(t>=stops[i]&&t<=stops[i+1]) {
        a=indices[i]==7?float3(1,1,1):effect.colors[indices[i]].rgb;
        b=indices[i+1]==7?float3(1,1,1):effect.colors[indices[i+1]].rgb;
        blend=saturate((t-stops[i])/(stops[i+1]-stops[i]));
    }
    return lerp(a,b,blend);
}
float neon_flash(float cycle) {
    const float stops[10]={.15,.16,.18,.20,.22,.23,.25,.28,.50,.51};
    const float values[10]={0,1,0,0,1,0,0,1,0,0};
    float2 endpoints=0;float t=0;
    [unroll]for(uint i=0;i<9;i++)if(cycle>=stops[i]&&cycle<stops[i+1]) {
        endpoints=float2(values[i],values[i+1]);
        t=saturate((cycle-stops[i])/(stops[i+1]-stops[i]));
    }
    if(endpoints.x==endpoints.y)return endpoints.x;
    return lerp(endpoints.x,endpoints.y,curve_ease(t,float4(.24,.31,.36,.93)));
}
float4 glyph_pixel(GlyphVertexOutput input) : SV_Target0 {
    if(input.texture_bounds.x>=2)return color_glyph_atlas.Sample(glyph_sampler,input.texture_coordinate)*input.color.a;
    float coverage=1;
    if(input.texture_coordinate.x<=-2) {
        float2 local=float2(input.texture_coordinate.x+3,input.texture_coordinate.y)-.5;
        coverage=1-smoothstep(.16,.25,dot(local,local));
    } else if(input.texture_bounds.x>=0)coverage=glyph_mask(input.texture_coordinate,input.texture_bounds);
    float3 color=input.color.rgb;
    if(input.color.r<0) {
        const TextEffectMaterial effect=effects[min(127u,(uint)(-input.color.r-1))];
        if(effect.kind==3 && input.texture_bounds.x>=0) {
            const float2 directions[8]={float2(1,0),float2(-1,0),float2(0,1),float2(0,-1),
                float2(.707,.707),float2(-.707,.707),float2(.707,-.707),float2(-.707,-.707)};
            float stroke=coverage,glow=0;
            float radius=4+.12*effect.reserved.y,edge=.5+.02*effect.reserved.y;
            [unroll]for(uint d=0;d<8;d++) {
                float2 delta=directions[d]*input.glyph_parameters.xy;
                stroke=max(stroke,glyph_mask(input.texture_coordinate+delta*edge,input.texture_bounds));
                glow+=glyph_mask(input.texture_coordinate+delta*radius*.25,input.texture_bounds)*.5;
                glow+=glyph_mask(input.texture_coordinate+delta*radius*.5,input.texture_bounds)*.3;
                glow+=glyph_mask(input.texture_coordinate+delta*radius*.75,input.texture_bounds)*.2;
            }
            glow=saturate(glow*.2);
            float flash=input.glyph_parameters.z;
            float3 face=lerp(float3(1,1,1),effect.colors[0].rgb,flash);
            float alpha=coverage+(1-coverage)*stroke+(1-coverage)*(1-stroke)*glow;
            float3 rgb=face*coverage+effect.colors[6].rgb*(1-coverage)*stroke+
                effect.colors[0].rgb*(1-coverage)*(1-stroke)*glow;
            return float4(rgb,alpha)*input.color.a;
        }
        // Resolve the final material first. These paths overwrite the generic
        // gradient, so its divisions and palette lookup are unnecessary.
        if(input.color.b<0 && input.color.g<=1.5)
            color=effect.colors[effect.kind==5?4:5].rgb;
        else if(input.color.b>=0 && effect.kind==1)color=effect.colors[0].rgb;
        else if(input.color.b>=0 && effect.kind==5)color=float3(1,1,1);
        else if(input.color.b>=0 && effect.kind==8)
            color=effect.colors[(uint)input.color.g%(uint)max(effect.count,1)].rgb;
        else if(input.color.b>=0 && effect.kind==9) {
            // Enhanced role colours: c1,c2,c1 (or c1,c2,c3,c1) every 100 DIPs.
            float segment=frac((input.position.x/effect_scale-effect.left)/100)*max(effect.count,1);
            uint a=min((uint)segment,7u),b=a+1u<(uint)max(effect.count,1)?a+1u:0u;
            color=lerp(effect.colors[a].rgb,effect.colors[b].rgb,frac(segment));
        }
        else {
            float y=saturate((input.position.y/effect_scale-effect.reserved.z)/max(effect.reserved.y,1));
            if(input.color.b>=0 && effect.kind==4)
                color=toon_color(effect,saturate(y*.25+input.glyph_parameters.z*.75));
            else {
                float x=saturate((input.position.x/effect_scale-effect.left)/max(effect.width,1));
                if(input.color.b<0)
                    color=lerp(effect.colors[0].rgb,effect.colors[1].rgb,saturate((x+y+input.glyph_parameters.z)*.5));
                else {
                    float phase=effect.animate*(effect_seconds-effect.reserved.x+60);
                    float diagonal=saturate(((x+y)*.5-.1)/.8);
                    float prism_x=(input.position.x/effect_scale-effect.left)/max(effect.colors[7].a,38);
                    float segment=effect.kind==7?frac(prism_x+phase*.5)*effect.count:diagonal*max(effect.count-1,0);
                    uint a=min((uint)segment,7u),b=min(a+1u,(uint)max(effect.count-1,0));
                    // The renderer supplies an integer palette count in [1,8].
                    if(effect.kind==7)b=a+1u<(uint)max(effect.count,1)?a+1u:0u;
                    color=lerp(effect.colors[a].rgb,effect.colors[b].rgb,frac(segment));
                }
            }
        }
    }
    return float4(color*input.color.a,input.color.a)*coverage;
}
