cbuffer UiFrame : register(b0) {float2 coordinate_scale;float2 coordinate_translation;float group_opacity;};
struct MediaVertexInput {float4 rectangle:RECT;float4 texture_rectangle:TEXCOORD0;float4 parameters:TEXCOORD1;float4 rounded_clip:TEXCOORD2;};
struct MediaVertexOutput {
    float4 position : SV_Position;
    float2 texture_coordinate : TEXCOORD0; // atlas coordinate, already decoded
    float opacity : COLOR0;
    float2 local:TEXCOORD1;
    float3 shape:TEXCOORD2;float4 clip_local:TEXCOORD3;float clip_radius:TEXCOORD4;
    // Spoiler blur: standard deviation in layout units; and the atlas the quad
    // samples (0 primary, 1 animation cache, 2 preview). Then the decoded
    // half-texel-inset source rectangle every blur tap stays inside.
    nointerpolation float2 blur_source:TEXCOORD5;nointerpolation float4 bounds:TEXCOORD6;
};

// Signs select the atlas: x < 0 is the animation cache, y < 0 the preview atlas.
float2 decode_coordinate(float2 coordinate){
    return coordinate.x<0.0f?float2(-coordinate.x-1.0f,coordinate.y):
           coordinate.y<0.0f?float2(coordinate.x,-coordinate.y-1.0f):coordinate;
}

MediaVertexOutput media_vertex(MediaVertexInput input,uint id:SV_VertexID){
    const float2 corners[4]={float2(0,0),float2(1,0),float2(0,1),float2(1,1)};
    MediaVertexOutput output;float2 corner=corners[id],position=lerp(input.rectangle.xy,input.rectangle.zw,corner);
    output.position=float4(position*coordinate_scale+coordinate_translation,0,1);
    // A negative blur parameter carries orientation flags instead (cropped
    // avatars and banners turned in 90 degree steps): 1 swaps the texture
    // axes, 2 mirrors u, 4 mirrors v; such quads are never blurred.
    float2 uv=corner;
    const uint orientation=input.parameters.w<0.0f?(uint)(-input.parameters.w+.5f)&7u:0u;
    uv=(orientation&1u)?uv.yx:uv;uv.x=(orientation&2u)?1.0f-uv.x:uv.x;uv.y=(orientation&4u)?1.0f-uv.y:uv.y;
    const float2 first=decode_coordinate(input.texture_rectangle.xy),last=decode_coordinate(input.texture_rectangle.zw);
    output.texture_coordinate=lerp(first,last,uv);output.opacity=input.parameters.x*group_opacity;
    float2 extent=(input.rectangle.zw-input.rectangle.xy)*.5f;output.local=(corner*2-1)*extent;
    output.shape=float3(extent,input.parameters.y);
    output.clip_local=float4(position-(input.rounded_clip.xy+input.rounded_clip.zw)*.5f,(input.rounded_clip.zw-input.rounded_clip.xy)*.5f);
    output.clip_radius=input.parameters.z;
    output.blur_source=float2(max(input.parameters.w,0.0f),input.texture_rectangle.x<0.0f?1.0f:input.texture_rectangle.y<0.0f?2.0f:0.0f);
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

// Disk of 32 taps (radius two deviations) rotated per pixel by interleaved
// gradient noise, so undersampling reads as an even frosted grain instead of
// ghosted copies or structured streaks. Tap radii are the quantiles of a 2D
// Gaussian truncated at two deviations, so the taps carry equal weight; the
// direction turns by the golden angle per tap.
float4 blur_taps(Texture2D<float4> atlas,float2 center,float2 dx,float2 dy,float deviation,float4 bounds,float2 direction){
    const uint taps=32;
    static const float radii[taps]={
        0.16494f, 0.28766f, 0.37401f, 0.44575f, 0.50921f, 0.56726f, 0.62152f, 0.67301f,
        0.72242f, 0.77026f, 0.81692f, 0.86271f, 0.90788f, 0.95266f, 0.99725f, 1.04182f,
        1.08656f, 1.13163f, 1.17721f, 1.22349f, 1.27065f, 1.31891f, 1.36851f, 1.41974f,
        1.47291f, 1.52844f, 1.58679f, 1.64861f, 1.71469f, 1.78612f, 1.86442f, 1.95184f};
    const float2 turn=float2(-0.737368818f,0.675490360f);
    float4 sum=0;
    [loop] for(uint i=0;i<taps;++i){
        const float2 offset=deviation*radii[i]*direction;
        const float2 coordinate=clamp(center+offset.x*dx+offset.y*dy,bounds.xy,bounds.zw);
        // Atlas texels are premultiplied, so transparency never darkens the mean.
        sum+=atlas.SampleLevel(media_sampler,coordinate,0);
        direction=float2(direction.x*turn.x-direction.y*turn.y,direction.x*turn.y+direction.y*turn.x);
    }
    return sum*(1.0f/taps);
}
// The atlas is uniform per quad: select it once, outside the taps.
float4 blurred_sample(uint source,float2 center,float2 dx,float2 dy,float deviation,float4 bounds,float2 pixel){
    float2 direction;
    sincos(6.2831853f*frac(52.9829189f*frac(dot(floor(pixel),float2(0.06711056f,0.00583715f)))),direction.y,direction.x);
    float4 result;
    [branch] if(source==1) result=blur_taps(animation_frame_atlas,center,dx,dy,deviation,bounds,direction);
    else [branch] if(source==2) result=blur_taps(preview_media_atlas,center,dx,dy,deviation,bounds,direction);
    else result=blur_taps(media_atlas,center,dx,dy,deviation,bounds,direction);
    return result;
}

float4 media_pixel(MediaVertexOutput input) : SV_Target0 {
    const float2 texture_coordinate = input.texture_coordinate;
    const uint source = (uint)input.blur_source.y;
    const float2 dx=ddx(texture_coordinate),dy=ddy(texture_coordinate);
    // Layout units per target pixel, so the deviation follows the DPI scale.
    const float units=max(length(float2(ddx(input.local.x),ddy(input.local.x))),1e-4f);
    float4 sample;
    if(input.blur_source.x>0.0f){
        sample=blurred_sample(source,texture_coordinate,dx,dy,input.blur_source.x/units,
                              input.bounds,input.position.xy);
    }else{
        sample=sample_level(source,texture_coordinate);
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
