// twin: src/render/shaders/background_fx.hlsl fbe73921641adc91
// Background effect pass. FX_VERTEX: fx_vertex (STAGE_VS); FX_TARGET: fx_target_vertex (STAGE_VS);
// FX_COMPOSITE_VERTEX: fx_composite_vertex (STAGE_VS); FX_COMPOSITE_PIXEL: fx_composite_pixel (STAGE_PS);
// none: fx_builtin (STAGE_PS).
#if defined(FX_COMPOSITE_PIXEL)
#extension GL_EXT_blend_func_extended : require
#endif

#if defined(FX_VERTEX)

// Star: (radius, brightness, 1 / edge width, 0). Comet: (axis, fade, 1).
layout(location = 0) V2F vec2 v_local; // DIP offset from the star centre or comet head
layout(location = 1) flat V2F vec4 v_shape;

layout(std140, binding = 0) uniform UiFrameVs {
    vec2 coordinate_scale;
    vec2 coordinate_translation;
};

// The pixel stage sees these rows at b0, the vertex stage at b2: BackgroundFxConstants (rows 0..11) plus the canvas
// (DIPs) and the sprite grid (first star cell, cells per row, comet lanes).
layout(std140, binding = 2) uniform McFxVertex {
    float vertex_time;
    float pad0_unused_, pad1_unused_, pad2_unused_;
    vec2 vertex_resolution;
    vec2 pad3_unused_;
    vec4 gap_unused_[8];
    vec4 vertex_parameters[2];
    vec4 vertex_canvas;
    vec4 vertex_grid;
};

// PCG3D (Jarzynski and Olano, JCGT 2020): three decorrelated values in [0, 1) from three integers. Pure 32-bit
// integer arithmetic: the same stars on every GPU.
vec3 fx_hash(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z;
    v.y += v.z * v.x;
    v.z += v.x * v.y;
    return vec3(v >> 8u) * (1.0 / 16777216.0);
}

// Bounds beyond which neither a star's glow nor a comet's trail reaches 0.5 of an 8-bit level at full strength.
const float kStarReach = 6.0;
const float kCometTail = 36.0, kCometHead = 5.0, kCometWidth = 4.0;

void main() {
    // Triangle strip: (0,0) (1,0) (0,1) (1,1).
    vec2 corner = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    uint instance = uint(gl_InstanceID);
    float speed = vertex_parameters[0].x;
    float density = clamp(vertex_parameters[0].y, 0.0, 1.0);
    float time = vertex_time * speed;
    // Comet lanes come first, then one star per grid cell. Either hashes its key to seed a clock whose window then
    // selects the second hash.
    bool comet = instance < uint(vertex_grid.w);
    uint columns = max(uint(vertex_grid.z), 1u);
    uint index = instance - uint(vertex_grid.w);
    ivec2 cell_index = ivec2(int(vertex_grid.x) + int(index % columns), int(vertex_grid.y) + int(index / columns));
    uvec2 key = comet ? uvec2(instance, 0x7fffffffu) : uvec2(cell_index);
    vec3 base = fx_hash(uvec3(key, 0u));
    // Per-lane periods differ so comets do not fire in lockstep.
    float clock = comet ? time / (7.0 + base.x * 8.0) + 0.16 + base.x * 0.45
                        : time / (2.5 + base.x * 5.0) + base.x * 11.0;
    float window = floor(clock), progress = fract(clock);
    vec2 event = fx_hash(uvec3(key, uint(window) + 1u)).xy;
    vec2 low, high, anchor;
    if (comet) {
        // Density admits one to four staggered paths across the window. Each time window draws a new origin and slope.
        float slope = (event.x < 0.5 ? -1.0 : 1.0) * (0.18 + event.y * 0.24);
        vec2 extent = vertex_parameters[1].xy;
        vec2 path = vec2((extent.y + 96.0) * slope, extent.y + 96.0);
        vec2 axis = normalize(path);
        anchor = vec2(extent.x * (0.225 + event.x * 0.55) - path.x * 0.5, -48.0) + path * progress;
        float fade = smoothstep(0.03, 0.13, progress) * (1.0 - smoothstep(0.82, 0.96, progress));
        vec2 tail_end = anchor - axis * kCometTail, head_end = anchor + axis * kCometHead;
        vec2 reach = vec2(abs(axis.y), abs(axis.x)) * kCometWidth;
        low = min(tail_end, head_end) - reach;
        high = fade > 0.0 ? max(tail_end, head_end) + reach : low;
        v_shape = vec4(axis, fade, 1.0);
    } else {
        // One fixed star per grid cell with its own pulse schedule; window hashes alter pulse timing, not position.
        float pitch = mix(180.0, 48.0, density);
        vec2 cell = vec2(cell_index);
        anchor = (cell + (0.15 + base.yz * 0.7)) * pitch;
        float offset = (progress - (0.2 + event.x * 0.6)) / (0.08 + event.y * 0.12);
        float pulse = exp2(-offset * offset * 4.0) * smoothstep(0.0, 0.08, progress) *
                      (1.0 - smoothstep(0.92, 1.0, progress));
        // Edge ramp: one device pixel (about 1.2 DIPs across a diagonal), at least half a DIP.
        float pixels_per_dip = vertex_resolution.x / (vertex_canvas.z - vertex_canvas.x);
        low = max(anchor - kStarReach, cell * pitch);
        high = min(anchor + kStarReach, (cell + 1.0) * pitch);
        v_shape = vec4(1.1 + base.x * 0.8, 0.07 + pulse * 0.93, min(pixels_per_dip * (1.0 / 1.2), 2.0), 0.0);
    }
    low = max(low, vertex_canvas.xy);
    high = max(min(high, vertex_canvas.zw), low);
    vec2 position = mix(low, high, corner);
    v_local = position - anchor;
    vec2 clip = position * coordinate_scale + coordinate_translation;
    gl_Position = vec4(clip.x, -clip.y, -1.0, 1.0);
}

#elif defined(FX_TARGET)

// The whole effect target as a triangle strip: (0,0) (1,0) (0,1) (1,1), top-left origin.
void main() {
    vec2 corner = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    gl_Position = vec4(corner.x * 2.0 - 1.0, corner.y * 2.0 - 1.0, -1.0, 1.0);
}

#elif defined(FX_COMPOSITE_VERTEX)

layout(location = 0) V2F vec2 v_uv;
layout(location = 1) V2F vec2 v_dip;

layout(std140, binding = 0) uniform UiFrameVs {
    vec2 coordinate_scale;
    vec2 coordinate_translation;
};

// One canvas in DIPs, and the map from a DIP to a position in the effect target: uv = dip * scale + offset.
layout(std140, binding = 2) uniform McFxComposite {
    vec4 composite_canvas;
    vec4 composite_uv;
};

void main() {
    vec2 corner = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    vec2 position = mix(composite_canvas.xy, composite_canvas.zw, corner);
    vec2 clip = position * coordinate_scale + coordinate_translation;
    gl_Position = vec4(clip.x, -clip.y, -1.0, 1.0);
    v_uv = position * composite_uv.xy + composite_uv.zw;
    v_dip = position;
}

#elif defined(FX_COMPOSITE_PIXEL)

layout(location = 0) V2F vec2 v_uv;
layout(location = 1) V2F vec2 v_dip;

layout(std140, binding = 8) uniform McFxCompositePixel {
    vec4 composite_skip[2];
    vec4 composite_clamp; // the used part of the target, inset half a texel
    float composite_opacity;
    float composite_radius; // a tile's corner radius in DIPs; 0 fills the canvas rectangle
    float composite_light;  // the light adaptation's amount (effect_light_composite)
    float composite_layer;  // the paint layer's opacity (normal scales by it, else the blend factor)
    float composite_blend;  // EffectBlend: 0 normal, 1 screen, 2 add
};

layout(binding = 0) uniform highp sampler2D fx_target; //! t0 s0 (the pipeline's bilinear clamp sampler)

// The colour, and the weight of what is behind it: only the adapted screen and add's blend state reads the weight.
layout(location = 0, index = 0) out vec4 out_color;
layout(location = 0, index = 1) out vec4 out_weight;

void main() {
    // The target holds premultiplied colour; the user's opacity scales all four channels, and in normal blend the
    // paint layer's opacity scales them too, so a fading layer composes over what is behind and leaves its alpha whole.
    vec4 color = textureLod(fx_target, clamp(v_uv, composite_clamp.xy, composite_clamp.zw), 0.0) * composite_opacity;
    // A tile is a rounded rectangle: coverage from the signed distance to its outline, one pixel wide.
    if (composite_radius > 0.0) {
        vec2 half_size = (composite_skip[0].zw - composite_skip[0].xy) * 0.5;
        vec2 q = abs(v_dip - (composite_skip[0].xy + half_size)) - (half_size - composite_radius);
        float edge = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - composite_radius;
        color *= clamp(0.5 - edge / max(fwidth(edge), 0.0001), 0.0, 1.0);
    }
    out_color = color;
    out_weight = vec4(1.0);
    // A light appearance: the picture's BT.709 luma flips (y -> 1 - y) with its chroma kept. Normal flips the
    // premultiplied colour (by alpha - 2y); screen and add multiply what is behind by the flipped light instead of
    // adding it. composite_light cross-fades from the plain composite.
    if (composite_light > 0.0) {
        color = clamp(color, 0.0, 1.0);
        float luma = dot(color.rgb, vec3(0.2126, 0.7152, 0.0722));
        if (composite_blend == 0.0) {
            out_color.rgb = mix(color.rgb, clamp(color.rgb + (color.a - 2.0 * luma), 0.0, color.a), composite_light);
        } else {
            vec3 multiply = 1.0 - composite_layer * (1.0 - clamp(1.0 + color.rgb - 2.0 * luma, 0.0, 1.0));
            vec3 plain = composite_blend == 1.0 ? 1.0 - color.rgb : vec3(1.0);
            out_weight.rgb = mix(plain, multiply, composite_light);
            // Settled, nothing is added: a plain multiply blend takes the weight as the colour.
            out_color.rgb = composite_light < 1.0 ? color.rgb * (1.0 - composite_light) : out_weight.rgb;
        }
    }
    if (composite_blend == 0.0)
        out_color *= composite_layer;
}

#else

layout(location = 0) V2F vec2 v_local;
layout(location = 1) flat V2F vec4 v_shape;

layout(std140, binding = 8) uniform McFx {
    float time_unused_, delta_unused_, intensity_unused_;
    float fx_dark;
    vec4 resolution_origin_unused_;
    vec4 palette_unused_[8];
    vec4 fx_parameters[2];
};

layout(location = 0) out vec4 out_color;

void main() {
    float squared = dot(v_local, v_local);
    float detail;
    if (v_shape.w == 0.0) {
        // Core with a one-pixel ramp, plus a compact glow, (1 - d^2 / 64)^6, standing in for exp2(-0.14 d^2).
        float falloff = clamp(1.0 - squared * (1.0 / 64.0), 0.0, 1.0);
        float cubed = falloff * falloff * falloff;
        detail = (clamp((v_shape.x - sqrt(squared)) * v_shape.z, 0.0, 1.0) + cubed * cubed * 0.12) * v_shape.y;
    } else {
        float along = dot(v_local, v_shape.xy);
        float across = dot(v_local, vec2(-v_shape.y, v_shape.x));
        float tail = clamp(1.0 + along * (1.0 / 36.0), 0.0, 1.0) * step(along, 0.0) * exp2(-across * across * 0.7);
        detail = (tail * 0.8 + exp2(-squared * 0.5)) * v_shape.z;
    }
    // Ink follows the foreground mode: additive light ink on a dark theme, a lerp toward dark ink on a light one.
    float amount = clamp(detail, 0.0, 1.0) * clamp(fx_parameters[0].z, 0.0, 1.0) * 0.38;
    if (fx_dark > 0.5)
        out_color = vec4(vec3(0.82, 0.9, 1.0) * amount, 0.0);
    else
        out_color = vec4(vec3(0.1, 0.18, 0.3) * amount, amount);
}

#endif
