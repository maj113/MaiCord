#include "maicord/render/lottie_program.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/d2d1.hpp"
#include "maicord/sys/crt.hpp"
namespace maicord {
namespace {
constexpr u32 maximum_leaves = 1024;
float clamped(float value, float low, float high) noexcept {
    return minimum(maximum(value, low), high);
}
float absf(float value) noexcept {
    return value < 0 ? -value : value;
}
using Matrix = D2D1_MATRIX_3X2_F;
constexpr Matrix identity{1, 0, 0, 1, 0, 0};
Matrix compose(Matrix a, Matrix b) noexcept {
    return {a._11 * b._11 + a._12 * b._21,         a._11 * b._12 + a._12 * b._22,
            a._21 * b._11 + a._22 * b._21,         a._21 * b._12 + a._22 * b._22,
            a._31 * b._11 + a._32 * b._21 + b._31, a._31 * b._12 + a._32 * b._22 + b._32};
}
float number(JsonValue v, float fallback = 0) noexcept {
    if (v.kind != JsonValueKind::number || !v.source.data || !v.source.bytes || v.source.bytes > 64)
        return fallback;
    u32 at = 0;
    bool negative = v.source.data[at] == '-';
    at += negative;
    double value = 0, scale = 1;
    while (at < v.source.bytes && v.source.data[at] >= '0' && v.source.data[at] <= '9')
        value = value * 10 + v.source.data[at++] - '0';
    if (at < v.source.bytes && v.source.data[at] == '.') {
        ++at;
        while (at < v.source.bytes && v.source.data[at] >= '0' && v.source.data[at] <= '9') {
            scale *= .1;
            value += (v.source.data[at++] - '0') * scale;
        }
    }
    if (at < v.source.bytes && (v.source.data[at] == 'e' || v.source.data[at] == 'E')) {
        ++at;
        bool minus = false;
        if (at < v.source.bytes && (v.source.data[at] == '+' || v.source.data[at] == '-'))
            minus = v.source.data[at++] == '-';
        u32 power = 0;
        while (at < v.source.bytes && v.source.data[at] >= '0' && v.source.data[at] <= '9') {
            power = power * 10 + v.source.data[at++] - '0';
            if (power > 30)
                return fallback;
        }
        while (power--)
            value *= minus ? .1 : 10;
    }
    if (at != v.source.bytes || !(value >= 0 && value <= 10000000))
        return fallback;
    return static_cast<float>(negative ? -value : value);
}
struct Leaf {
    ID2D1Geometry* geometry;
    Matrix transform;
    ColrColor color;
};
struct Gradient {
    ColrPaintElement paint;
    u32 stop_count;
    ColrGradientStop stops[32];
};
constexpr u16 gradient_flag = 0x8000;
constexpr u16 gradient_prefix_mask = gradient_flag - 1;
constexpr u32 leaf_pool_bytes = maximum_leaves * (sizeof(Leaf) + sizeof(Gradient));
static_assert(sizeof(Leaf) == 48 && sizeof(Gradient) == 680);
static_assert(sizeof(Leaf) % alignof(Leaf) == 0 && sizeof(Gradient) % alignof(Leaf) == 0);
static_assert(alignof(Leaf) >= alignof(Gradient) && sizeof(Leaf) % alignof(Gradient) == 0);
static_assert(maximum_leaves <= gradient_prefix_mask);
static_assert((maximum_leaves - 1) * (sizeof(Leaf) + sizeof(Gradient)) +
                  sizeof(Leaf) + sizeof(Gradient) == leaf_pool_bytes);
struct Document {
    JsonIndexedDocument json;
    JsonValue assets;
    ID2D1Factory* factory = nullptr;
    u32 leaf_count = 0, leaf_index = 0, level = 0;
    float frame = 0;
    bool invalid_parent = false;
    u16 gradient_count = 0;
    u32* ends;
    // The low bits count prior gradient records. The high bit marks a gradient.
    u16 leaf_records[maximum_leaves];
    alignas(Leaf) u8 leaf_pool[leaf_pool_bytes];
    u32 leaf_offset(u32 index) const noexcept {
        return static_cast<u32>(index * sizeof(Leaf) +
                                (leaf_records[index] & gradient_prefix_mask) * sizeof(Gradient));
    }
    Leaf& get_leaf(u32 index) noexcept {
        return *reinterpret_cast<Leaf*>(leaf_pool + leaf_offset(index));
    }
    const Leaf& get_leaf(u32 index) const noexcept {
        return *reinterpret_cast<const Leaf*>(leaf_pool + leaf_offset(index));
    }
    Gradient& get_gradient(u32 index) noexcept {
        return *reinterpret_cast<Gradient*>(leaf_pool + leaf_offset(index) + sizeof(Leaf));
    }
    bool has_gradient(u32 index) const noexcept {
        return (leaf_records[index] & gradient_flag) != 0;
    }
    u32 locate(u32 offset) const noexcept {
        u32 first=0,last=json.structurals.count;
        while(first<last){const u32 mid=first+(last-first)/2;
            if(json.structurals[mid]<offset)first=mid+1;else last=mid;}
        return first;
    }
    void whitespace(u32& at) const noexcept {
        while(at<json.input.bytes && (json.input.data[at]==' ' || json.input.data[at]=='\r' ||
            json.input.data[at]=='\n' || json.input.data[at]=='\t'))++at;
    }
    // The complete document is validated once. Matched delimiters let property
    // reads skip subtrees without recursively revalidating their keyframes.
    JsonValue value(u32& at,u32& token) const noexcept {
        whitespace(at);const u32 begin=at;JsonValueKind kind{};
        const char c=json.input.data[at];
        if(c=='{' || c=='[' || c=='"') {
            kind=c=='{'?JsonValueKind::object:c=='['?JsonValueKind::array:JsonValueKind::string;
            const u32 end=ends[token];at=json.structurals[end]+1;token=end+1;
        } else {
            kind=c=='n'?JsonValueKind::null_value:(c=='t'||c=='f')?JsonValueKind::boolean:JsonValueKind::number;
            at=json.structurals[token];
            while(at>begin && (json.input.data[at-1]==' ' || json.input.data[at-1]=='\r' ||
                json.input.data[at-1]=='\n' || json.input.data[at-1]=='\t'))--at;
        }
        return {{json.input.data+begin,at-begin},begin,kind};
    }
    JsonValue get(JsonValue object,const char* name) noexcept {
        if(object.kind!=JsonValueKind::object || !object.source.bytes)return {};
        u32 token=locate(object.source_offset)+1,n=0;while(name[n])++n;
        JsonValue result{};bool found=false;
        while(json.input.data[json.structurals[token]]!='}') {
            const u32 start=json.structurals[token]+1,end=json.structurals[token+1];
            const JsonString key{{json.input.data+start,end-start},start,false};
            token+=3;u32 at=json.structurals[token-1]+1;
            const auto entry=value(at,token);
            if(json_string_equals_ascii(key,{name,n})) {if(found)return {};result=entry;found=true;}
            if(json.input.data[json.structurals[token]]==',')++token;
        }
        return result;
    }
    bool array(JsonValue input,Slice<JsonValue> output,u32* count) noexcept {
        *count=0;if(input.kind!=JsonValueKind::array || !input.source.bytes)return false;
        u32 token=locate(input.source_offset)+1,at=input.source_offset+1;
        while(json.input.data[json.structurals[token]]!=']' || at<json.structurals[token]) {
            whitespace(at);if(json.input.data[at]==']')break;
            if(*count==output.count)return false;
            output[(*count)++]=value(at,token);
            if(json.input.data[json.structurals[token]]==','){at=json.structurals[token]+1;++token;}
            else break;
        }
        return true;
    }
    bool type(JsonValue value, const char* name) noexcept {
        JsonString text{};
        u32 n = 0;
        while (name[n])
            ++n;
        return json_value_as_string(get(value, "ty"), &text) && json_string_equals_ascii(text, {name, n});
    }
    bool hidden(JsonValue value) noexcept {
        bool hd = false;
        json_value_to_bool(get(value, "hd"), &hd);
        return hd;
    }
    struct Sample { JsonValue start,end,key; float progress=0; };
    Sample sampled(JsonValue property) noexcept {
        if(property.kind==JsonValueKind::object){const auto k=get(property,"k");if(k.source.bytes)property=k;}
        Sample result{property,property,{},0};
        if(property.kind!=JsonValueKind::array)return result;
        JsonValue values[512]{};u32 count=0;
        if(!array(property,{values,512},&count)||!count)return {};
        if(values[0].kind!=JsonValueKind::object || !get(values[0],"t").source.bytes)return result;
        u32 selected=0;
        while(selected+1<count && number(get(values[selected+1],"t"))<=frame)++selected;
        result.key=values[selected];result.start=get(result.key,"s");
        if(!result.start.source.bytes && selected)result.start=get(values[selected-1],"e");
        if(!result.start.source.bytes && selected)result.start=get(values[selected-1],"s");
        result.end=result.start;
        if(selected+1<count && number(get(result.key,"h"))!=1){
            const float begin=number(get(result.key,"t")),end=number(get(values[selected+1],"t"));
            result.end=get(result.key,"e");
            if(!result.end.source.bytes)result.end=get(values[selected+1],"s");
            if(!result.end.source.bytes)result.end=result.start;
            result.progress=clamped(end>begin?(frame-begin)/(end-begin):0,0,1);
        }
        return result;
    }
    float component(JsonValue value,u32 index,float fallback=0) noexcept {
        if(value.kind==JsonValueKind::array){JsonValue a[256]{};u32 n=0;
            if(!array(value,{a,256},&n)||!n)return fallback;value=a[minimum(index,n-1)];}
        return number(value,fallback);
    }
    static float bezier(float t,float a,float b) noexcept {
        const float u=1-t;return 3*u*u*t*a+3*u*t*t*b+t*t*t;
    }
    float progress(const Sample& value,u32 dimension=0) noexcept {
        if(value.progress==0 || value.progress==1)return value.progress;
        const auto in=get(value.key,"i"),out=get(value.key,"o");
        if(!in.source.bytes||!out.source.bytes)return value.progress;
        const float x1=clamped(component(get(out,"x"),dimension),0,1),
                    x2=clamped(component(get(in,"x"),dimension,1),0,1);
        float low=0,high=1,t=value.progress;
        for(u32 i=0;i<16;++i){t=(low+high)*.5f;
            if(bezier(t,x1,x2)<value.progress)low=t;else high=t;}
        return bezier(t,component(get(out,"y"),dimension),component(get(in,"y"),dimension,1));
    }
    float interpolated(const Sample& value,u32 dimension,float fallback=0) noexcept {
        const float a=component(value.start,dimension,fallback),b=component(value.end,dimension,a);
        return a+(b-a)*progress(value,dimension);
    }
    struct GradientComponents {
        float starts[256];
        JsonValue ends[256];
        u32 count, end_count;
        JsonValue scalar_end;
    };
    bool gradient_components(const Sample& value, GradientComponents* output) noexcept {
        if (!array(value.start, {output->ends, 256}, &output->count) || !output->count)
            return false;
        for (u32 i = 0; i < output->count; ++i)
            output->starts[i] = number(output->ends[i]);
        output->scalar_end = value.end;
        output->end_count = 0;
        if (value.end.kind == JsonValueKind::array &&
            !array(value.end, {output->ends, 256}, &output->end_count))
            output->end_count = 0;
        return true;
    }
    float gradient_component(const Sample& value, const GradientComponents& components,
                             u32 dimension) noexcept {
        const float a = components.starts[minimum(dimension, components.count - 1)];
        const JsonValue end = components.end_count
                                  ? components.ends[minimum(dimension, components.end_count - 1)]
                                  : components.scalar_end;
        const float b = number(end, a);
        return a + (b - a) * progress(value, dimension);
    }
    float scalar(JsonValue property,float fallback=0) noexcept {return interpolated(sampled(property),0,fallback);}
    __declspec(noinline) D2D1_POINT_2F point_array_components(JsonValue value, D2D1_POINT_2F fallback) noexcept {
        JsonValue components[256]; u32 count = 0;
        if (!array(value, {components, 256}, &count) || !count)
            return fallback;
        return {number(components[0], fallback.x), number(components[minimum(1u, count - 1)], fallback.y)};
    }
    D2D1_POINT_2F point_components(JsonValue value, D2D1_POINT_2F fallback) noexcept {
        if (value.kind != JsonValueKind::array)
            return {number(value, fallback.x), number(value, fallback.y)};
        return point_array_components(value, fallback);
    }
    D2D1_POINT_2F point(JsonValue property,D2D1_POINT_2F fallback={}) noexcept {
        if(get(property,"x").source.bytes)return {scalar(get(property,"x")),scalar(get(property,"y"))};
        const auto value=sampled(property);
        const auto start = point_components(value.start, fallback);
        const auto end = point_components(value.end, start);
        const auto to=get(value.key,"to"),ti=get(value.key,"ti");
        if(to.source.bytes && ti.source.bytes){const float t=progress(value),u=1-t;
            return {u*u*u*start.x+3*u*u*t*(start.x+component(to,0))+3*u*t*t*(end.x+component(ti,0))+t*t*t*end.x,
                    u*u*u*start.y+3*u*u*t*(start.y+component(to,1))+3*u*t*t*(end.y+component(ti,1))+t*t*t*end.y};}
        return {start.x + (end.x - start.x) * progress(value, 0),
                start.y + (end.y - start.y) * progress(value, 1)};
    }
    Matrix transform(JsonValue value) noexcept {
        const auto p = point(get(value, "p")), anchor = point(get(value, "a")),
                   scale = point(get(value, "s"), {100, 100});
        float angle = scalar(get(value, "r")) * 3.14159265358979323846f / 180;
        angle -= static_cast<i32>(angle / (2 * 3.14159265358979323846f)) * (2 * 3.14159265358979323846f);
        if (angle > 3.14159265358979323846f)
            angle -= 2 * 3.14159265358979323846f;
        if (angle < -3.14159265358979323846f)
            angle += 2 * 3.14159265358979323846f;
        const float z = angle * angle;
        const float sine =
            angle *
            (1 + z * (-1.f / 6 +
                      z * (1.f / 120 + z * (-1.f / 5040 + z * (1.f / 362880 + z * (-1.f / 39916800))))));
        const float cosine =
            1 + z * (-.5f + z * (1.f / 24 + z * (-1.f / 720 + z * (1.f / 40320 +
                                                                   z * (-1.f / 3628800 + z / 479001600)))));
        Matrix m{cosine * scale.x * .01f,
                 sine * scale.x * .01f,
                 -sine * scale.y * .01f,
                 cosine * scale.y * .01f,
                 0,
                 0};
        m._31 = p.x - anchor.x * m._11 - anchor.y * m._21;
        m._32 = p.y - anchor.x * m._12 - anchor.y * m._22;
        return m;
    }
    ColrColor color(JsonValue style, float opacity) noexcept {
        ColrColor c{0,0,0,opacity*scalar(get(style,"o"),100)*.01f};
        const auto value=sampled(get(style,"c"));
        c.r=interpolated(value,0);c.g=interpolated(value,1);c.b=interpolated(value,2);
        c.r = clamped(c.r, 0.f, 1.f);
        c.g = clamped(c.g, 0.f, 1.f);
        c.b = clamped(c.b, 0.f, 1.f);
        c.a = clamped(c.a, 0.f, 1.f);
        return c;
    }
    bool append(ID2D1Geometry* geometry, Matrix m, ColrColor c, bool gradient = false) noexcept {
        if (!geometry)
            return false;
        if (c.a <= 0) {
            geometry->Release();
            return true;
        }
        float area = 0;
        if (FAILED(geometry->ComputeArea(&m, .0001f, &area))) {
            geometry->Release();
            return false;
        }
        if (!(area > .00001f)) {
            geometry->Release();
            return true;
        }
        if (leaf_count == maximum_leaves) {
            geometry->Release();
            return false;
        }
        leaf_records[leaf_count] = gradient_count;
        auto* storage = leaf_pool + leaf_offset(leaf_count);
        auto& leaf = *new (storage) Leaf;
        leaf.geometry = geometry;
        leaf.transform = m;
        leaf.color = c;
        if (gradient) {
            new (storage + sizeof(Leaf)) Gradient;
            ++gradient_count;
        }
        ++leaf_count;
        return true;
    }
    bool append_style(ID2D1Geometry* geometry, Matrix m, JsonValue style, float opacity) noexcept {
        if (!type(style, "gf") && !type(style, "gs"))
            return append(geometry, m, color(style, opacity));
        const u32 previous = leaf_count;
        if (!append(geometry, m, {1, 1, 1, opacity * scalar(get(style, "o"), 100) * .01f}, true))
            return false;
        if (previous == leaf_count)
            return true;
        auto& leaf = get_leaf(previous);
        const auto gradient = get(style, "g");
        const u32 colors = static_cast<u32>(number(get(gradient, "p")));
        const auto animated=sampled(get(gradient,"k"));
        GradientComponents components;
        if (!colors || colors > 32 || !gradient_components(animated, &components) || components.count < colors * 4)
            return false;
        const u32 n = components.count;
        const auto at = [&](u32 dimension) noexcept {
            return gradient_component(animated, components, dimension);
        };
        auto& data = get_gradient(previous);
        data.stop_count = colors;
        leaf_records[previous] |= gradient_flag;
        for (u32 i = 0; i < colors; ++i) {
            auto& stop = data.stops[i];
            stop.position = at(i * 4);
            stop.color = {at(i * 4 + 1), at(i * 4 + 2), at(i * 4 + 3), leaf.color.a};
            if (stop.position < 0 || stop.position > 1 || (i && stop.position < data.stops[i - 1].position))
                return false;
            if (n >= colors * 4 + 2) {
                u32 j = colors * 4;
                while (j + 3 < n && at(j + 2) <= stop.position)
                    j += 2;
                const float start = at(j), end = j + 3 < n ? at(j + 2) : start;
                const float x = clamped(end > start ? (stop.position - start) / (end - start) : 0, 0, 1);
                stop.color.a *=
                    at(j + 1) * (1 - x) + (j + 3 < n ? at(j + 3) : at(j + 1)) * x;
            }
        }
        const auto transform_point = [m](D2D1_POINT_2F p) noexcept {
            return D2D1_POINT_2F{p.x * m._11 + p.y * m._21 + m._31, p.x * m._12 + p.y * m._22 + m._32};
        };
        const auto a0 = point(get(style, "s")), b0 = point(get(style, "e"));
        const auto start = transform_point(a0), end = transform_point(b0);
        if (number(get(style, "t"), 1) == 1) {
            const auto perpendicular = transform_point({a0.x - (b0.y - a0.y), a0.y + (b0.x - a0.x)});
            data.paint.kind = ColrPaintKind::linear_gradient;
            data.paint.paint.linearGradient = {0,     colors, start.x,         start.y,
                                                  end.x, end.y,  perpendicular.x, perpendicular.y};
        } else {
            const float dx = end.x - start.x, dy = end.y - start.y;
            float radius = maximum(absf(dx), absf(dy));
            if (radius > 0)
                for (u32 i = 0; i < 8; ++i)
                    radius = (radius + (dx * dx + dy * dy) / radius) * .5f;
            data.paint.kind = ColrPaintKind::radial_gradient;
            data.paint.paint.radialGradient = {0, colors, start.x, start.y, 0, start.x, start.y, radius};
        }
        return true;
    }
    bool path(JsonValue shape, ID2D1Geometry** output) noexcept {
        *output = nullptr;
        const auto animated=sampled(get(shape,"ks"));
        auto value=animated.start,end=animated.end;
        if(value.kind==JsonValueKind::array){JsonValue a[2]{};u32 n=0;
            if(!array(value,{a,2},&n)||!n)return false;value=a[0];}
        if(end.kind==JsonValueKind::array){JsonValue a[2]{};u32 n=0;
            if(!array(end,{a,2},&n)||!n)return false;end=a[0];}
        JsonValue v[512]{},in[512]{},out[512]{},v_end[512]{},in_end[512]{},out_end[512]{};
        u32 nv=0,ni=0,no=0,nve=0,nie=0,noe=0;
        if(!array(get(value,"v"),{v,512},&nv)||!array(get(value,"i"),{in,512},&ni)||
            !array(get(value,"o"),{out,512},&no)||!nv||nv!=ni||nv!=no)return false;
        const float blend=progress(animated);
        if(blend && (!array(get(end,"v"),{v_end,512},&nve)||!array(get(end,"i"),{in_end,512},&nie)||
            !array(get(end,"o"),{out_end,512},&noe)||nve!=nv||nie!=nv||noe!=nv))return false;
        const auto morph=[this,blend](JsonValue a,JsonValue b) noexcept {const auto p=point(a),q=blend?point(b):p;
            return D2D1_POINT_2F{p.x+(q.x-p.x)*blend,p.y+(q.y-p.y)*blend};};
        ID2D1PathGeometry* geometry = nullptr;
        ID2D1GeometrySink* sink = nullptr;
        if (FAILED(factory->CreatePathGeometry(&geometry)) || FAILED(geometry->Open(&sink))) {
            if (geometry)
                geometry->Release();
            return false;
        }
        bool closed = false;
        json_value_to_bool(get(value, "c"), &closed);
        sink->SetFillMode(D2D1_FILL_MODE_WINDING);
        sink->BeginFigure(morph(v[0],v_end[0]), D2D1_FIGURE_BEGIN_FILLED);
        for (u32 i = 0; i < (closed ? nv : nv - 1); ++i) {
            const u32 next = (i + 1) % nv;
            const auto a=morph(v[i],v_end[i]),b=morph(v[next],v_end[next]),o=morph(out[i],out_end[i]),t=morph(in[next],in_end[next]);
            const D2D1_BEZIER_SEGMENT segment{{a.x + o.x, a.y + o.y}, {b.x + t.x, b.y + t.y}, b};
            sink->AddBezier(segment);
        }
        sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        const HRESULT result = sink->Close();
        sink->Release();
        if (FAILED(result)) {
            geometry->Release();
            return false;
        }
        *output = geometry;
        return true;
    }
    bool groups(JsonValue items, Matrix world, float opacity, u32 depth, bool repeated = false,
                Matrix repetition = identity, JsonValue inherited_fill = {},
                JsonValue inherited_stroke = {}) noexcept {
        if (depth > 24)
            return false;
        JsonValue shapes[512]{};
        u32 count = 0;
        if (!array(items, {shapes, 512}, &count))
            return false;
        const Matrix inherited_world = world;
        const float inherited_opacity = opacity;
        JsonValue fill = inherited_fill, stroke = inherited_stroke, repeater{};
        u32 fill_index = count + 1, stroke_index = count + 2;
        bool merged = false;
        for (u32 i = 0; i < count; ++i)
            if (!hidden(shapes[i])) {
                if (type(shapes[i], "tr")) {
                    world = compose(transform(shapes[i]), world);
                    opacity *= scalar(get(shapes[i], "o"), 100) * .01f;
                } else if (type(shapes[i], "fl") || type(shapes[i], "gf")) {
                    fill = shapes[i];
                    fill_index = i;
                } else if (type(shapes[i], "st") || type(shapes[i], "gs")) {
                    stroke = shapes[i];
                    stroke_index = i;
                } else if (type(shapes[i], "mm"))
                    merged = true;
                else if (type(shapes[i], "rp"))
                    repeater = shapes[i];
            }
        world = compose(repetition, world);
        for (u32 i = count; i--;) {
            const auto shape = shapes[i];
            if (hidden(shape))
                continue;
            if (type(shape, "gr") && !merged) {
                if (!groups(get(shape, "it"), world, opacity, depth + 1, false, identity, fill, stroke))
                    return false;
                continue;
            }
            ID2D1Geometry* geometry = nullptr;
            if (merged) {
                if (i != count - 1)
                    continue;
                if (!geometry_scope(items, &geometry, depth, false))
                    return false;
            } else if (type(shape, "sh")) {
                if (!path(shape, &geometry))
                    return false;
            } else if (type(shape, "el")) {
                const auto p = point(get(shape, "p")), s = point(get(shape, "s"));
                ID2D1EllipseGeometry* ellipse = nullptr;
                if (FAILED(factory->CreateEllipseGeometry({p, s.x * .5f, s.y * .5f}, &ellipse)))
                    return false;
                geometry = ellipse;
            } else if (type(shape, "rc")) {
                const auto p = point(get(shape, "p")), s = point(get(shape, "s"));
                const float r = scalar(get(shape, "r"));
                ID2D1RoundedRectangleGeometry* rectangle = nullptr;
                if (FAILED(factory->CreateRoundedRectangleGeometry(
                        {{p.x - s.x * .5f, p.y - s.y * .5f, p.x + s.x * .5f, p.y + s.y * .5f}, r, r},
                        &rectangle)))
                    return false;
                geometry = rectangle;
            } else if (type(shape, "fl") || type(shape, "st") || type(shape, "gf") || type(shape, "gs") ||
                       type(shape, "tr") || type(shape, "tm") || type(shape, "rp"))
                continue;
            else
                return false;
            if (fill.source.bytes && (!stroke.source.bytes || fill_index > stroke_index)) {
                geometry->AddRef();
                if (!append_style(geometry, world, fill, opacity)) {
                    geometry->Release();
                    return false;
                }
            }
            if (stroke.source.bytes && scalar(get(stroke, "w")) > 0) {
                ID2D1PathGeometry* widened = nullptr;
                ID2D1GeometrySink* sink = nullptr;
                if (FAILED(factory->CreatePathGeometry(&widened)) || FAILED(widened->Open(&sink))) {
                    if (widened)
                        widened->Release();
                    geometry->Release();
                    return false;
                }
                D2D1_STROKE_STYLE_PROPERTIES style{};
                const u32 cap = static_cast<u32>(number(get(stroke, "lc"), 1)),
                          join = static_cast<u32>(number(get(stroke, "lj"), 1));
                style.startCap = style.endCap = style.dashCap = cap == 2   ? D2D1_CAP_STYLE_ROUND
                                                                : cap == 3 ? D2D1_CAP_STYLE_SQUARE
                                                                           : D2D1_CAP_STYLE_FLAT;
                style.lineJoin = join == 2   ? D2D1_LINE_JOIN_ROUND
                                 : join == 3 ? D2D1_LINE_JOIN_BEVEL
                                             : D2D1_LINE_JOIN_MITER;
                style.miterLimit = maximum(1.f, number(get(stroke, "ml"), 4));
                ID2D1StrokeStyle* native_style = nullptr;
                const HRESULT created = factory->CreateStrokeStyle(style, nullptr, 0, &native_style);
                const HRESULT hr = SUCCEEDED(created) ? geometry->Widen(scalar(get(stroke, "w")),
                                                                        native_style, nullptr, .1f, sink)
                                                      : created;
                if (native_style)
                    native_style->Release();
                const HRESULT end = sink->Close();
                sink->Release();
                if (FAILED(hr) || FAILED(end) || !append_style(widened, world, stroke, opacity)) {
                    if (FAILED(hr) || FAILED(end))
                        widened->Release();
                    geometry->Release();
                    return false;
                }
            }
            if (fill.source.bytes && stroke.source.bytes && fill_index <= stroke_index) {
                geometry->AddRef();
                if (!append_style(geometry, world, fill, opacity)) {
                    geometry->Release();
                    return false;
                }
            }
            geometry->Release();
        }
        if (!repeated && repeater.source.bytes) {
            const float copies = scalar(get(repeater, "c"));
            if (!(copies >= 0 && copies <= 32) || scalar(get(repeater, "o")) != 0)
                return false;
            const auto tr = get(repeater, "tr");
            const Matrix step = transform(tr);
            Matrix m = identity;
            for (u32 copy = 1; copy < static_cast<u32>(copies); ++copy) {
                m = compose(m, step);
                const float t = static_cast<float>(copy) / maximum(1.f, copies - 1);
                const float alpha =
                    (scalar(get(tr, "so"), 100) * (1 - t) + scalar(get(tr, "eo"), 100) * t) * .01f;
                if (!groups(items, inherited_world, inherited_opacity * alpha, depth + 1, true, m,
                            inherited_fill, inherited_stroke))
                    return false;
            }
        }
        return true;
    }
    bool combine(ID2D1Geometry* a, ID2D1Geometry* b, D2D1_COMBINE_MODE mode,
                 ID2D1Geometry** output) noexcept {
        ID2D1PathGeometry* merged = nullptr;
        ID2D1GeometrySink* sink = nullptr;
        if (FAILED(factory->CreatePathGeometry(&merged)) || FAILED(merged->Open(&sink))) {
            if (merged)
                merged->Release();
            return false;
        }
        const HRESULT hr = a->CombineWithGeometry(b, mode, nullptr, .1f, sink), end = sink->Close();
        sink->Release();
        if (FAILED(hr) || FAILED(end)) {
            merged->Release();
            return false;
        }
        *output = merged;
        return true;
    }
    // Shape modifiers consume geometry, including otherwise unstyled child groups.
    bool geometry_scope(JsonValue items, ID2D1Geometry** output, u32 depth,
                        bool apply_transform = true, bool* has_shapes = nullptr) noexcept {
        *output = nullptr;
        if(has_shapes)*has_shapes=false;
        if (depth > 24)
            return false;
        JsonValue shapes[512]{};
        u32 n = 0;
        if (!array(items, {shapes, 512}, &n))
            return false;
        Matrix matrix = identity;
        D2D1_COMBINE_MODE mode = D2D1_COMBINE_MODE_UNION;
        for (u32 i = 0; i < n; ++i) {
            if (hidden(shapes[i]))
                continue;
            if (type(shapes[i], "tr"))
                matrix = transform(shapes[i]);
            if (type(shapes[i], "mm")) {
                const auto mm = static_cast<u32>(number(get(shapes[i], "mm"), 1));
                mode = mm == 4   ? D2D1_COMBINE_MODE_INTERSECT
                       : mm == 3 ? D2D1_COMBINE_MODE_EXCLUDE
                       : mm == 5 ? D2D1_COMBINE_MODE_XOR
                                 : D2D1_COMBINE_MODE_UNION;
            }
        }
        ID2D1Geometry* collected = nullptr;
        bool any_shapes=false;
        struct Release {
            ID2D1Geometry*& p;
            ~Release() noexcept {
                if (p)
                    p->Release();
            }
        } release{collected};
        for (u32 i = 0; i < n; ++i) {
            const auto shape = shapes[i];
            if (hidden(shape))
                continue;
            ID2D1Geometry* part = nullptr;
            if (type(shape, "sh")) {
                if (!path(shape, &part))
                    return false;
            } else if (type(shape, "gr")) {
                bool child_shapes=false;
                if (!geometry_scope(get(shape, "it"), &part, depth + 1, true, &child_shapes))
                    return false;
                // A transform-only group has no path operand. A real operand
                // whose boolean result is empty still participates in merges.
                if(!child_shapes){part->Release();continue;}
            } else if (type(shape, "el")) {
                const auto p = point(get(shape, "p")), s = point(get(shape, "s"));
                ID2D1EllipseGeometry* ellipse = nullptr;
                if (FAILED(factory->CreateEllipseGeometry({p, s.x * .5f, s.y * .5f}, &ellipse)))
                    return false;
                part = ellipse;
            } else if (type(shape, "rc")) {
                const auto p = point(get(shape, "p")), s = point(get(shape, "s"));
                const float r = scalar(get(shape, "r"));
                ID2D1RoundedRectangleGeometry* rect = nullptr;
                if (FAILED(factory->CreateRoundedRectangleGeometry(
                        {{p.x - s.x * .5f, p.y - s.y * .5f, p.x + s.x * .5f, p.y + s.y * .5f}, r, r},
                        &rect)))
                    return false;
                part = rect;
            } else
                continue;
            any_shapes=true;
            if (!collected) {
                collected = part;
                continue;
            }
            ID2D1Geometry* joined = nullptr;
            const bool okay = combine(collected, part, mode, &joined);
            part->Release();
            if (!okay)
                return false;
            collected->Release();
            collected = joined;
        }
        if (!collected) {
            ID2D1PathGeometry* empty = nullptr;
            ID2D1GeometrySink* sink = nullptr;
            if (FAILED(factory->CreatePathGeometry(&empty)) || FAILED(empty->Open(&sink))) {
                if (empty)
                    empty->Release();
                return false;
            }
            const HRESULT hr = sink->Close();
            sink->Release();
            if (FAILED(hr)) {
                empty->Release();
                return false;
            }
            collected = empty;
        }
        ID2D1TransformedGeometry* transformed = nullptr;
        if (!apply_transform)
            matrix = identity;
        if (FAILED(factory->CreateTransformedGeometry(collected, &matrix, &transformed)))
            return false;
        *output = transformed;
        if(has_shapes)*has_shapes=any_shapes;
        return true;
    }
    Matrix parent_transform(JsonValue layer, ConstSlice<JsonValue> layers, u32 depth) noexcept {
        Matrix own = transform(get(layer, "ks"));
        const auto parent = get(layer, "parent");
        if (!parent.source.bytes)
            return own;
        if (depth >= 24) {
            invalid_parent = true;
            return own;
        }
        const float index = number(parent, -1);
        for (u32 i = 0; i < layers.count; ++i)
            if (number(get(layers[i], "ind"), -2) == index)
                return compose(own, parent_transform(layers[i], layers, depth + 1));
        invalid_parent = true;
        return own;
    }
    bool layers(JsonValue array_value, Matrix world, float opacity, u32 depth) noexcept {
        if (depth > 16)
            return false;
        JsonValue values[512]{};
        u32 count = 0;
        if (!array(array_value, {values, 512}, &count))
            return false;
        for (u32 i = count; i--;) {
            const auto layer = values[i];
            if (hidden(layer) || frame < number(get(layer, "ip"), -100000) ||
                frame >= number(get(layer, "op"), 100000))
                continue;
            const float ty = number(get(layer, "ty"), -1);
            if (ty == 3)
                continue;
            const Matrix local = compose(parent_transform(layer, {values, count}, 0), world);
            const float alpha = opacity * scalar(get(get(layer, "ks"), "o"), 100) * .01f;
            if (alpha <= 0)
                continue;
            if (ty == 4) {
                if (!groups(get(layer, "shapes"), local, alpha, 0))
                    return false;
            } else if (ty == 0) {
                JsonString reference{};
                if (!json_value_as_string(get(layer, "refId"), &reference))
                    return false;
                JsonValue a[256]{};
                u32 n = 0;
                if (!array(assets, {a, 256}, &n))
                    return false;
                bool found = false;
                for (u32 j = 0; j < n; ++j) {
                    JsonString name{};
                    if (json_value_as_string(get(a[j], "id"), &name) &&
                        name.encoded.bytes == reference.encoded.bytes &&
                        bytes_equal(name.encoded.data, reference.encoded.data, name.encoded.bytes)) {
                        found = true;
                        const float previous = frame;
                        frame = (frame - number(get(layer, "st"))) /
                                maximum(.0001f, number(get(layer, "sr"), 1));
                        const bool okay = layers(get(a[j], "layers"), local, alpha, depth + 1);
                        frame = previous;
                        if (!okay)
                            return false;
                        break;
                    }
                }
                if (!found)
                    return false;
            } else
                return false;
        }
        return true;
    }
};
static_assert(offsetof(Document, leaf_records) == 96);
static_assert(offsetof(Document, leaf_pool) == 2144);
static_assert(sizeof(Document) == 747616);
class OutlineSink final : public ID2D1SimplifiedGeometrySink {
    GlyphOutline* out;
    void push(GlyphPathCommand command) noexcept {
        if (out->count == out->capacity) {
            ++out->rejected;
            return;
        }
        out->commands[out->count++] = command;
        const u32 points = command.kind == GlyphPathCommandKind::cubic ? 3
                           : command.kind == GlyphPathCommandKind::line ||
                                   command.kind == GlyphPathCommandKind::begin_filled
                               ? 1
                               : 0;
        const float xy[]{command.x1, command.y1, command.x2, command.y2, command.x3, command.y3};
        for (u32 i = 0; i < points; ++i) {
            const float x = xy[i * 2], y = xy[i * 2 + 1];
            if (!out->has_bounds) {
                out->minimum_x = out->maximum_x = x;
                out->minimum_y = out->maximum_y = y;
                out->has_bounds = true;
            } else {
                out->minimum_x = minimum(out->minimum_x, x);
                out->maximum_x = maximum(out->maximum_x, x);
                out->minimum_y = minimum(out->minimum_y, y);
                out->maximum_y = maximum(out->maximum_y, y);
            }
        }
    }

  public:
    explicit OutlineSink(GlyphOutline* output) noexcept : out(output) {
        out->count = out->rejected = 0;
        out->has_bounds = false;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** value) noexcept override {
        if (!value)
            return E_POINTER;
        *value = nullptr;
        if (id == __uuidof(IUnknown) || id == __uuidof(ID2D1SimplifiedGeometrySink)) {
            *value = this;
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        return 1;
    }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        return 1;
    }
    void STDMETHODCALLTYPE SetFillMode(D2D1_FILL_MODE mode) noexcept override {
        out->fill_rule =
            mode == D2D1_FILL_MODE_ALTERNATE ? GlyphFillRule::alternate : GlyphFillRule::winding;
    }
    void STDMETHODCALLTYPE SetSegmentFlags(D2D1_PATH_SEGMENT) noexcept override {
    }
    void STDMETHODCALLTYPE BeginFigure(D2D1_POINT_2F p, D2D1_FIGURE_BEGIN) noexcept override {
        push({p.x, p.y, 0, 0, 0, 0, GlyphPathCommandKind::begin_filled, {}});
    }
    void STDMETHODCALLTYPE AddLines(const D2D1_POINT_2F* p, UINT32 n) noexcept override {
        for (u32 i = 0; i < n; ++i)
            push({p[i].x, p[i].y, 0, 0, 0, 0, GlyphPathCommandKind::line, {}});
    }
    void STDMETHODCALLTYPE AddBeziers(const D2D1_BEZIER_SEGMENT* p, UINT32 n) noexcept override {
        for (u32 i = 0; i < n; ++i)
            push({p[i].point1.x,
                  p[i].point1.y,
                  p[i].point2.x,
                  p[i].point2.y,
                  p[i].point3.x,
                  p[i].point3.y,
                  GlyphPathCommandKind::cubic,
                  {}});
    }
    void STDMETHODCALLTYPE EndFigure(D2D1_FIGURE_END) noexcept override {
        // Fill implicitly closes open subpaths. Stroke geometry was already
        // widened from the original open path, so its caps/joins stay intact.
        push({0,
              0,
              0,
              0,
              0,
              0,
              GlyphPathCommandKind::end_closed,
              {}});
    }
    HRESULT STDMETHODCALLTYPE Close() noexcept override {
        return out->rejected ? E_OUTOFMEMORY : S_OK;
    }
};
ColrPaintElement leaf(const Document& doc) noexcept {
    ColrPaintElement e{};
    if (doc.has_gradient(doc.leaf_index)) {
        e.kind = ColrPaintKind::glyph;
        e.paint.glyph.glyphIndex = doc.leaf_index;
    } else {
        e.kind = ColrPaintKind::solid_glyph;
        e.paint.solidGlyph = {doc.leaf_index, {doc.get_leaf(doc.leaf_index).color}};
    }
    return e;
}
struct ScannedJson {
    JsonIndexedDocument json{};
    u32* index = nullptr;
    ~ScannedJson() noexcept {
        if (index)
            VirtualFree(index, 0, MEM_RELEASE);
    }
    bool read(ConstSlice<u8> encoded) noexcept {
        index = static_cast<u32*>(VirtualAlloc(nullptr, encoded.count * sizeof(u32),
                                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        if (!index)
            return false;
        index = new (index) u32[encoded.count];
        const Utf8View text{reinterpret_cast<const char*>(encoded.data), encoded.count};
        const auto scan = json_scan_structurals_avx1(text, {index, encoded.count});
        json = {text, {index, scan.count}};
        return scan.ok();
    }
};
bool timing_from_index(const JsonIndexedDocument& json, LottieTiming* timing) noexcept {
    struct Header { float w=0,h=0,first=0,last=0,rate=0; bool layers=false; } h{};
    const bool okay=json_visit_root_object(json.input,json.structurals,64,
        +[](void* p,JsonString key,JsonValue value) noexcept {
            auto& h=*static_cast<Header*>(p);
            if(json_string_equals_ascii(key,utf8_literal("w")))h.w=number(value);
            else if(json_string_equals_ascii(key,utf8_literal("h")))h.h=number(value);
            else if(json_string_equals_ascii(key,utf8_literal("ip")))h.first=number(value);
            else if(json_string_equals_ascii(key,utf8_literal("op")))h.last=number(value);
            else if(json_string_equals_ascii(key,utf8_literal("fr")))h.rate=number(value);
            else if(json_string_equals_ascii(key,utf8_literal("layers")))h.layers=value.kind==JsonValueKind::array;
            return true;
        },&h).ok() && h.w>0 && h.w<=4096 && h.h>0 && h.h<=4096 && h.layers;
    if(!okay)return false;
    if(h.first<0 || h.last-h.first>10000 || h.rate>120)return false;
    timing->first=h.first;timing->rate=h.rate;
    if(h.rate>0 && h.last>h.first){const float count=h.last-h.first;
        timing->frames=static_cast<u32>(count)+(count>static_cast<u32>(count));
        timing->duration_us=static_cast<u64>(1000000.0/h.rate+.5);}
    return true;
}
bool compile_parameters_valid(ColrPaintProgram* program, float frame, float pixels) noexcept {
    return program && pixels > 0 && pixels <= 512 && frame >= 0 && frame < 100000;
}
bool compile_indexed(const JsonIndexedDocument& json, ColrPaintProgram* program,
                     float frame, float pixels, float* source_width) noexcept {
    const usize ends_bytes = json.structurals.count * sizeof(u32);
    const usize doc_offset = (ends_bytes + alignof(Document) - 1) & ~(alignof(Document) - 1);
    void* block = VirtualAlloc(nullptr, doc_offset + sizeof(Document),
                              MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!block)
        return false;
    // The hot ends table precedes the header and first leaf records.
    auto* doc = new (static_cast<u8*>(block) + doc_offset) Document;
    struct Release {
        Document* doc;
        void* block;
        ~Release() noexcept {
            for (u32 i = 0; i < doc->leaf_count; ++i)
                doc->get_leaf(i).geometry->Release();
            if (doc->factory)
                doc->factory->Release();
            VirtualFree(block, 0, MEM_RELEASE);
        }
    } release{doc, block};
    const auto text = json.input;
    const auto index = json.structurals;
    doc->json = json;
    doc->ends = new (block) u32[json.structurals.count];
    u32 stack[64]{},depth=0;
    for(u32 i=0;i<index.count;++i){const char c=text.data[index[i]];
        if(c=='"'){doc->ends[i]=i+1;++i;}
        else if(c=='{'||c=='['){if(depth==64)return false;stack[depth++]=i;}
        else if(c=='}'||c==']'){if(!depth)return false;doc->ends[stack[--depth]]=i;}
    }
    doc->frame = frame;
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr,
                                 reinterpret_cast<void**>(&doc->factory))))
        return false;
    const JsonValue root{text, 0, JsonValueKind::object};
    doc->assets = doc->get(root, "assets");
    const float w = number(doc->get(root, "w")), h = number(doc->get(root, "h"));
    if (!(w > 0 && w <= 4096 && h > 0 && h <= 4096))
        return false;
    const float extent = maximum(w, h);
    const Matrix normalized{1.f / extent, 0, 0, 1.f / extent, 0, 0};
    if (!doc->layers(doc->get(root, "layers"), normalized, 1, 0) || doc->invalid_parent) {
        return false;
    }
    ColrPaintSource source{doc,
                           +[](void* p, ColrPaintElement* e) noexcept {
                               auto& d = *static_cast<Document*>(p);
                               if (d.level == 0) {
                                   d.level = 1;
                                   d.leaf_index = 0;
                                   *e = leaf(d);
                                   return true;
                               }
                               if (d.level == 1 && d.has_gradient(d.leaf_index)) {
                                   d.level = 2;
                                   *e = d.get_gradient(d.leaf_index).paint;
                                   return true;
                               }
                               return false;
                           },
                           +[](void* p, ColrPaintElement* e) noexcept {
                               auto& d = *static_cast<Document*>(p);
                               if (d.level != 1 || d.leaf_index + 1 >= d.leaf_count)
                                   return false;
                               ++d.leaf_index;
                               *e = leaf(d);
                               return true;
                           },
                           +[](void* p) noexcept {
                               auto& d = *static_cast<Document*>(p);
                               if (!d.level)
                                   return false;
                               --d.level;
                               return true;
                           },
                           +[](void* p, Slice<ColrGradientStop> out) noexcept {
                               auto& d = *static_cast<Document*>(p);
                               const auto& gradient = d.get_gradient(d.leaf_index);
                               if (out.count != gradient.stop_count)
                                   return false;
                               copy_bytes(out.data, gradient.stops, out.count * sizeof(*out.data));
                               return true;
                           },
                           +[](void* p, u16 id, GlyphOutline* out) noexcept {
                               auto& d = *static_cast<Document*>(p);
                               if (id >= d.leaf_count)
                                   return false;
                               const auto& leaf = d.get_leaf(id);
                               OutlineSink sink(out);
                               return SUCCEEDED(leaf.geometry->Simplify(
                                          D2D1_GEOMETRY_SIMPLIFICATION_OPTION_CUBICS_AND_LINES,
                                          &leaf.transform, .0001f, &sink)) &&
                                      !out->rejected;
                           }};
    ColrPaintElement root_paint{};
    root_paint.kind = ColrPaintKind::layers;
    root_paint.paint.layers.childCount = doc->leaf_count;
    if (source_width)
        *source_width = 1;
    return colr_program_build(program, source, root_paint, {0, 0, w / extent, h / extent}, pixels);
}
} // namespace
bool lottie_program_compile_frame(ConstSlice<u8> encoded, u32 frame_index, float pixels,
                                  LottieTiming* timing, LottieProgramAcquire acquire, void* context,
                                  float* source_width) noexcept {
    if (!timing || !acquire || !encoded.data || !encoded.count || encoded.count > kLottieMaximumBytes)
        return false;
    *timing = {};
    ScannedJson source;
    if (!source.read(encoded) || !timing_from_index(source.json, timing) || frame_index >= timing->frames)
        return false;
    auto* program = acquire(context);
    const float frame = timing->first + frame_index;
    return compile_parameters_valid(program, frame, pixels) &&
           compile_indexed(source.json, program, frame, pixels, source_width);
}
} // namespace maicord
