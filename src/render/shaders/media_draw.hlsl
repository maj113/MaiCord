cbuffer UiFrame : register(b0) {float2 coordinate_scale;float2 coordinate_translation;float group_opacity;float3 frame_reserved;};
struct MediaVertexInput {float4 rectangle:RECT;float4 texture_rectangle:TEXCOORD0;float4 parameters:TEXCOORD1;float4 rounded_clip:TEXCOORD2;};
struct MediaVertexOutput {
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0;
    float opacity : COLOR0;
    float2 local:TEXCOORD1;
    float3 shape:TEXCOORD2;float4 clip_local:TEXCOORD3;float clip_radius:TEXCOORD4;
    // Spoiler blur: standard deviation in layout units, and the decoded
    // half-texel-inset source rectangle every blur tap stays inside.
    nointerpolation float blur:TEXCOORD5;nointerpolation float4 bounds:TEXCOORD6;
};

// Signs select the atlas: x < 0 is the animation cache, y < 0 the preview atlas.
float2 decode_coordinate(float2 coordinate){
    return coordinate.x<0.0f?float2(-coordinate.x-1.0f,coordinate.y):
           coordinate.y<0.0f?float2(coordinate.x,-coordinate.y-1.0f):coordinate;
}

MediaVertexOutput media_vertex(MediaVertexInput input,uint id:SV_VertexID){
    const float2 corners[6]={float2(0,0),float2(1,0),float2(0,1),float2(0,1),float2(1,0),float2(1,1)};
    MediaVertexOutput output;float2 corner=corners[id],position=lerp(input.rectangle.xy,input.rectangle.zw,corner);
    output.position=float4(position*coordinate_scale+coordinate_translation,0,1);
    // A negative blur parameter carries orientation flags instead (cropped
    // avatars and banners turned in 90 degree steps): 1 swaps the texture
    // axes, 2 mirrors u, 4 mirrors v; such quads are never blurred.
    float2 uv=corner;
    const uint orientation=input.parameters.w<0.0f?(uint)(-input.parameters.w+.5f)&7u:0u;
    uv=(orientation&1u)?uv.yx:uv;uv.x=(orientation&2u)?1.0f-uv.x:uv.x;uv.y=(orientation&4u)?1.0f-uv.y:uv.y;
    output.texture_coordinate=lerp(input.texture_rectangle.xy,input.texture_rectangle.zw,uv);output.opacity=input.parameters.x*group_opacity;
    float2 extent=(input.rectangle.zw-input.rectangle.xy)*.5f;output.local=(corner*2-1)*extent;
    output.shape=float3(extent,input.parameters.y);
    output.clip_local=float4(position-(input.rounded_clip.xy+input.rounded_clip.zw)*.5f,(input.rounded_clip.zw-input.rounded_clip.xy)*.5f);
    output.clip_radius=input.parameters.z;output.blur=max(input.parameters.w,0.0f);
    const float2 first=decode_coordinate(input.texture_rectangle.xy),last=decode_coordinate(input.texture_rectangle.zw);
    output.bounds=float4(min(first,last),max(first,last));return output;
}

Texture2D<float4> media_atlas : register(t0);
Texture2D<float4> animation_frame_atlas : register(t1);
Texture2D<float4> preview_media_atlas : register(t2);
SamplerState media_sampler : register(s0);

float4 sample_level(uint source,float2 coordinate){
    // Each atlas has one mip. Select one texture without implicit derivatives.
    float4 result;
    [branch] if(source==1) {
        result=animation_frame_atlas.SampleLevel(media_sampler,coordinate,0);
    } else {
        [branch] if(source==2) {
            result=preview_media_atlas.SampleLevel(media_sampler,coordinate,0);
        } else {
            result=media_atlas.SampleLevel(media_sampler,coordinate,0);
        }
    }
    return result;
}

// Gaussian-weighted golden-angle disk (radius two deviations), rotated per
// pixel by a white-noise hash so undersampling reads as an even frosted
// grain instead of ghosted copies or structured streaks.
float4 blurred_sample(uint source,float2 center,float2 dx,float2 dy,float deviation,float4 bounds,float2 pixel){
    const uint taps=64;
    const float rotation=6.2831853f*frac(sin(dot(floor(pixel),float2(12.9898f,78.233f)))*43758.5453f);
    const float radius=deviation*2.0f,inverse=-0.5f/(deviation*deviation);
    float4 sum=0;float weight=0;
    [loop] for(uint i=0;i<taps;++i){
        const float r=radius*sqrt((i+.5f)/taps),angle=i*2.39996323f+rotation;
        const float2 offset=r*float2(cos(angle),sin(angle));
        const float2 coordinate=clamp(center+offset.x*dx+offset.y*dy,bounds.xy,bounds.zw);
        const float w=exp(r*r*inverse);
        // Atlas texels are premultiplied, so transparency never darkens the mean.
        sum+=sample_level(source,coordinate)*w;weight+=w;
    }
    return sum/weight;
}

float4 media_pixel(MediaVertexOutput input) : SV_Target0 {
    const bool cached_animation = input.texture_coordinate.x < 0.0f;
    const bool large_preview = !cached_animation &&
                               input.texture_coordinate.y < 0.0f;
    const float2 texture_coordinate = cached_animation
        ? float2(-input.texture_coordinate.x - 1.0f,
                 input.texture_coordinate.y)
        : large_preview
            ? float2(input.texture_coordinate.x,
                     -input.texture_coordinate.y - 1.0f)
            : input.texture_coordinate;
    const float2 dx=ddx(texture_coordinate),dy=ddy(texture_coordinate);
    // Layout units per target pixel, so the deviation follows the DPI scale.
    const float units=max(length(float2(ddx(input.local.x),ddy(input.local.x))),1e-4f);
    float4 sample;
    if(input.blur>0.0f){
        sample=blurred_sample(cached_animation?1:large_preview?2:0,texture_coordinate,dx,dy,input.blur/units,
                              input.bounds,input.position.xy);
    }else{
        sample=sample_level(cached_animation?1:large_preview?2:0,texture_coordinate);
    }
    // Radii are uniform per quad: square media (emoji, video, the viewer)
    // skip both rounded-rectangle distances instead of selecting them away.
    float coverage=1.0f;
    [branch] if(input.shape.z>0.0f){
        float radius=min(input.shape.z,min(input.shape.x,input.shape.y));
        float2 corner=abs(input.local)-input.shape.xy+radius;
        float distance=length(max(corner,0))+min(max(corner.x,corner.y),0)-radius;
        float aa=max(fwidth(distance),.25f);coverage=1-smoothstep(-aa,aa,distance);
    }
    [branch] if(input.clip_radius>0){
        float radius=min(input.clip_radius,min(input.clip_local.z,input.clip_local.w));
        float2 q=abs(input.clip_local.xy)-input.clip_local.zw+radius;
        float distance=length(max(q,0))+min(max(q.x,q.y),0)-radius;
        float aa=max(fwidth(distance),.25f);coverage*=1-smoothstep(-aa,aa,distance);
    }
    return float4(sample.rgb * input.opacity,
                  sample.a * input.opacity)*coverage;
}
