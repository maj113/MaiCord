// twin: src/render/shaders/glyph_draw.hlsl d70cf2a82edc2432
// Text quads and text effects: glyph_vertex (STAGE_VS) and glyph_pixel (STAGE_PS). The atlases are paged surfaces
// (see media_draw.glsl): each draw binds the page its quads sample, and their v is in that page's rows.

struct TextEffect {
    vec4 colors[8];
    float left, width, kind, count;
    float animate, reserved_x, reserved_y, reserved_z;
};

layout(location = 0) V2F vec2 v_uv;
layout(location = 1) V2F vec4 v_color;
layout(location = 2) flat V2F vec4 v_bounds;
// xy: atlas scale; z: glyph-uniform flash or travel, evaluated per corner.
layout(location = 3) flat V2F vec3 v_glyph;

#if defined(STAGE_VS)

layout(std140, binding = 0) uniform UiFrameVs {
    vec2 coordinate_scale;
    vec2 coordinate_translation;
    float group_opacity;
};
// A block both stages declare carries an instance name: a program holds one global name per member.
layout(std140, binding = 1) uniform TextEffectsVs {
    float effect_seconds, effect_scale;
    vec2 effect_reserved_unused_;
    TextEffect effects[128];
} text_fx;

layout(location = 0) in vec4 rect;
layout(location = 1) in vec4 uv_rect;
layout(location = 2) in vec4 color;

// Cubic-bezier easing through control points (p.x,p.y) and (p.z,p.w); Newton iterations solve the curve parameter for
// t; Horner form reduces each step.
float curve_ease(float t, vec4 p) {
    vec2 a = 1.0 + 3.0 * (p.xy - p.zw), b = 3.0 * p.zw - 6.0 * p.xy, c = 3.0 * p.xy;
    float u = clamp(t, 0.0, 1.0);
    for (int i = 0; i < 4; i++) {
        float x = ((a.x * u + b.x) * u + c.x) * u;
        float d = (3.0 * a.x * u + 2.0 * b.x) * u + c.x;
        u = clamp(u - (x - t) / max(d, 0.001), 0.0, 1.0);
    }
    return ((a.y * u + b.y) * u + c.y) * u;
}
const vec4 kProfileCurve = vec4(0.44, 0.29, 0.48, 1.0);
const vec4 kNeonCurve = vec4(0.24, 0.31, 0.36, 0.93);
// A move is (from,to,t): its value is mix(from,to,curve_ease(t)); a held value has from==to. A vertex solves at most
// two curves, one per call site below.
float eased(vec3 move, vec4 curve) { return mix(move.x, move.y, curve_ease(move.z, curve)); }
vec3 pop_move(float cycle, bool shadow) {
    vec3 offsets = shadow ? vec3(0.08, 0.13, 0.0) : vec3(0.0, -0.05, 0.08);
    if (cycle >= 0.5) return vec3(offsets.x, offsets.x, 0.0);
    if (cycle < 0.18) return vec3(offsets.xy, cycle / 0.18);
    if (cycle < 0.35) return vec3(offsets.yz, (cycle - 0.18) / 0.17);
    return vec3(offsets.zx, (cycle - 0.35) / 0.15);
}
// Four-second cycle shared by the independent front and extrusion transforms.
float profile_cycle(float phase_offset) { return fract((text_fx.effect_seconds - phase_offset + 60.0) / 4.0); }

vec3 gummy_move(float cycle) {
    if (cycle >= 0.35) return vec3(1.0, 1.0, 0.0);
    if (cycle < 0.07) return vec3(1.0, 1.28, cycle / 0.07);
    if (cycle < 0.14) return vec3(1.28, 0.82, (cycle - 0.07) / 0.07);
    if (cycle < 0.21) return vec3(0.82, 1.1, (cycle - 0.14) / 0.07);
    if (cycle < 0.28) return vec3(1.1, 0.96, (cycle - 0.21) / 0.07);
    return vec3(0.96, 1.0, (cycle - 0.28) / 0.07);
}
const float kNeonStops[10] = float[10](0.15, 0.16, 0.18, 0.20, 0.22, 0.23, 0.25, 0.28, 0.50, 0.51);
const float kNeonValues[10] = float[10](0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0, 1.0, 0.0, 0.0);
vec3 neon_move(float cycle) {
    vec3 move = vec3(0.0);
    for (uint i = 0u; i < 9u; i++)
        if (cycle >= kNeonStops[i] && cycle < kNeonStops[i + 1u])
            move = vec3(kNeonValues[i], kNeonValues[i + 1u],
                        clamp((cycle - kNeonStops[i]) / (kNeonStops[i + 1u] - kNeonStops[i]), 0.0, 1.0));
    return move;
}

void main() {
    vec2 corner = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    vec4 rectangle = rect, uv = uv_rect;
    bool color_glyph = uv.x >= 2.0;
    if (color_glyph) uv.xz -= 2.0;
    // Preserve the atlas-kind tag here; color sampling returns before using bounds.
    v_bounds = uv_rect;
    vec3 glyph = vec3(0.0);
    uint index = min(127u, uint(-color.r - 1.0));
    if (color.r < 0.0 && uv_rect.x >= 0.0 && !color_glyph) {
        if (text_fx.effects[index].kind == 3.0) {
            glyph.xy = (uv.zw - uv.xy) / max(rectangle.zw - rectangle.xy, vec2(0.001));
            float radius = 4.0 + 0.12 * text_fx.effects[index].reserved_y;
            rectangle += vec4(-radius, -radius, radius, radius);
            uv += vec4(-glyph.xy, glyph.xy) * radius;
        }
    }
    vec2 position = mix(rectangle.xy, rectangle.zw, corner);
    if (color.r < 0.0 && !color_glyph) {
        float kind = text_fx.effects[index].kind, animate = text_fx.effects[index].animate;
        float phase_offset = text_fx.effects[index].reserved_x;
        bool neon = kind == 3.0 && uv_rect.x >= 0.0;
        if (neon || color.b < 0.0 || kind == 4.0) {
            vec3 move = vec3(0.0);
            if (!neon) {
                float progress = color.b < 0.0 ? profile_cycle(phase_offset) * 2.0 : (profile_cycle(phase_offset) - 0.05) / 0.5;
                move = vec3(0.0, 1.0, clamp(progress, 0.0, 1.0));
            } else {
                move = neon_move(profile_cycle(phase_offset));
            }
            glyph.z = animate * eased(move, neon ? kNeonCurve : kProfileCurve);
        }
        bool gummy = kind == 8.0 && animate > 0.0 && uv_rect.x >= 0.0;
        if (gummy || kind == 5.0) {
            vec3 move;
            if (gummy)
                move = gummy_move(fract((text_fx.effect_seconds - phase_offset + 60.0 - color.g * 0.05) / 4.0));
            else
                move = pop_move(animate > 0.0 ? profile_cycle(phase_offset) : 0.75, color.b < 0.0 && color.g > 1.5);
            float amount = eased(move, kProfileCurve);
            if (gummy) {
                vec2 center = vec2(color.b, text_fx.effects[index].reserved_z + text_fx.effects[index].reserved_y * 0.65);
                position = center + (position - center) * vec2(amount, 2.0 - amount);
            } else {
                position.y += amount * text_fx.effects[index].reserved_y;
            }
        }
    }
    vec2 clip = position * coordinate_scale + coordinate_translation;
    gl_Position = vec4(clip.x, -clip.y, -1.0, 1.0);
    v_uv = mix(uv.xy, uv.zw, corner);
    v_color = vec4(color.rgb, color.a * group_opacity);
    v_glyph = glyph;
}

#else

layout(std140, binding = 9) uniform TextEffectsPs {
    float effect_seconds, effect_scale;
    vec2 effect_reserved_unused_;
    TextEffect effects[128];
} text_fx;

layout(binding = 0) uniform highp sampler2D glyph_atlas; //! t0 s0
layout(binding = 1) uniform highp sampler2D color_atlas; //! t1 s0

layout(location = 0) out vec4 out_color;

// Every sample reads mip 0: the sampler's MaxLOD is 0, so an explicit level is exact and spares the pixel quad its
// implicit-derivative (LOD) work inside the divergent paths below.
float coverage_sample(vec2 uv) {
    return textureLod(glyph_atlas, uv, 0.0).r;
}
vec4 color_sample(vec2 uv) {
    return textureLod(color_atlas, uv, 0.0);
}

// Keep expanded halo samples within this glyph's allocation, including at atlas edges.
float glyph_mask(vec2 uv, vec4 bounds) {
    if (any(lessThan(uv, bounds.xy)) || any(greaterThan(uv, bounds.zw))) return 0.0;
    return coverage_sample(uv);
}

const float kToonStops[10] = float[10](0.0, 0.08, 0.15, 0.25, 0.45, 0.55, 0.75, 0.83, 0.90, 1.0);
const uint kToonIndices[10] = uint[10](7u, 2u, 1u, 0u, 2u, 0u, 7u, 2u, 1u, 0u);
vec3 toon_color(uint index, float t) {
    vec3 a = text_fx.effects[index].colors[0].rgb, b = a;
    float blend = 0.0;
    for (uint i = 0u; i < 9u; i++)
        if (t >= kToonStops[i] && t <= kToonStops[i + 1u]) {
            a = kToonIndices[i] == 7u ? vec3(1.0) : text_fx.effects[index].colors[kToonIndices[i]].rgb;
            b = kToonIndices[i + 1u] == 7u ? vec3(1.0) : text_fx.effects[index].colors[kToonIndices[i + 1u]].rgb;
            blend = clamp((t - kToonStops[i]) / (kToonStops[i + 1u] - kToonStops[i]), 0.0, 1.0);
        }
    return mix(a, b, blend);
}

const vec2 kDirections[8] = vec2[8](vec2(1.0, 0.0), vec2(-1.0, 0.0), vec2(0.0, 1.0), vec2(0.0, -1.0),
                                    vec2(0.707, 0.707), vec2(-0.707, 0.707), vec2(0.707, -0.707), vec2(-0.707, -0.707));

vec4 shade() {
    if (v_bounds.x >= 2.0) return color_sample(v_uv) * v_color.a;
    float coverage = 1.0;
    if (v_uv.x <= -2.0) {
        vec2 local = vec2(v_uv.x + 3.0, v_uv.y) - 0.5;
        coverage = 1.0 - smoothstep(0.16, 0.25, dot(local, local));
    } else if (v_bounds.x >= 0.0) {
        // Only a neon effect expands the quad past its allocation; a plain glyph's interpolated coordinate never
        // leaves its own rectangle.
        if (v_color.r < 0.0) coverage = glyph_mask(v_uv, v_bounds);
        else coverage = coverage_sample(v_uv);
    }
    vec3 color = v_color.rgb;
    if (v_color.r < 0.0) {
        // Published materials have palette counts in [1,8] and widths >= 1.
        uint index = min(127u, uint(-v_color.r - 1.0));
        float kind = text_fx.effects[index].kind, count = text_fx.effects[index].count, left = text_fx.effects[index].left;
        float reserved_y = text_fx.effects[index].reserved_y, reserved_z = text_fx.effects[index].reserved_z;
        if (kind == 3.0 && v_bounds.x >= 0.0) {
            float stroke = coverage, glow = 0.0;
            float radius = 4.0 + 0.12 * reserved_y, edge = 0.5 + 0.02 * reserved_y;
            for (uint d = 0u; d < 8u; d++) {
                vec2 delta = kDirections[d] * v_glyph.xy;
                stroke = max(stroke, glyph_mask(v_uv + delta * edge, v_bounds));
                glow += glyph_mask(v_uv + delta * radius * 0.25, v_bounds) * 0.5;
                glow += glyph_mask(v_uv + delta * radius * 0.5, v_bounds) * 0.3;
                glow += glyph_mask(v_uv + delta * radius * 0.75, v_bounds) * 0.2;
            }
            glow = clamp(glow * 0.2, 0.0, 1.0);
            float flash = v_glyph.z;
            vec3 face = mix(vec3(1.0), text_fx.effects[index].colors[0].rgb, flash);
            float alpha = coverage + (1.0 - coverage) * stroke + (1.0 - coverage) * (1.0 - stroke) * glow;
            vec3 rgb = face * coverage + text_fx.effects[index].colors[6].rgb * (1.0 - coverage) * stroke +
                       text_fx.effects[index].colors[0].rgb * (1.0 - coverage) * (1.0 - stroke) * glow;
            return vec4(rgb, alpha) * v_color.a;
        }
        // Resolve the final material first. These paths overwrite the generic gradient, so its divisions and palette
        // lookup are unnecessary.
        if (v_color.b < 0.0 && v_color.g <= 1.5)
            color = text_fx.effects[index].colors[kind == 5.0 ? 4 : 5].rgb;
        else if (v_color.b >= 0.0 && kind == 1.0) color = text_fx.effects[index].colors[0].rgb;
        else if (v_color.b >= 0.0 && kind == 5.0) color = vec3(1.0);
        else if (v_color.b >= 0.0 && kind == 8.0)
            color = text_fx.effects[index].colors[uint(v_color.g) % uint(count)].rgb;
        else if (v_color.b >= 0.0 && kind == 9.0) {
            // Palette cycle: c1,c2,c1 (or c1,c2,c3,c1) every 100 DIPs.
            float segment = fract((gl_FragCoord.x / text_fx.effect_scale - left) / 100.0) * count;
            uint a = min(uint(segment), 7u), b = a + 1u < uint(count) ? a + 1u : 0u;
            color = mix(text_fx.effects[index].colors[a].rgb, text_fx.effects[index].colors[b].rgb, fract(segment));
        } else {
            float y = clamp((gl_FragCoord.y / text_fx.effect_scale - reserved_z) / max(reserved_y, 1.0), 0.0, 1.0);
            if (v_color.b >= 0.0 && kind == 4.0) {
                color = toon_color(index, clamp(y * 0.25 + v_glyph.z * 0.75, 0.0, 1.0));
            } else {
                float x = clamp((gl_FragCoord.x / text_fx.effect_scale - left) / text_fx.effects[index].width, 0.0, 1.0);
                if (v_color.b < 0.0) {
                    color = mix(text_fx.effects[index].colors[0].rgb, text_fx.effects[index].colors[1].rgb,
                                clamp((x + y + v_glyph.z) * 0.5, 0.0, 1.0));
                } else {
                    float phase = text_fx.effects[index].animate * (text_fx.effect_seconds - text_fx.effects[index].reserved_x + 60.0);
                    float diagonal = clamp(((x + y) * 0.5 - 0.1) / 0.8, 0.0, 1.0);
                    float prism_x = (gl_FragCoord.x / text_fx.effect_scale - left) / max(text_fx.effects[index].colors[7].a, 38.0);
                    float segment = kind == 7.0 ? fract(prism_x + phase * 0.5) * count : diagonal * (count - 1.0);
                    uint a = min(uint(segment), 7u), b = min(a + 1u, uint(count - 1.0));
                    if (kind == 7.0) b = a + 1u < uint(count) ? a + 1u : 0u;
                    color = mix(text_fx.effects[index].colors[a].rgb, text_fx.effects[index].colors[b].rgb, fract(segment));
                }
            }
        }
    }
    return vec4(color * v_color.a, v_color.a) * coverage;
}

void main() {
    out_color = shade();
}

#endif
