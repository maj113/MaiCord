#include "maicord/render/lottie_program.hpp"
#include "maicord/core/json_reader.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/win32_kernel.hpp"
#include "maicord/sys/d2d1.hpp"
#include "maicord/sys/crt.hpp"
namespace maicord {
namespace {
constexpr u32 maximum_leaves = 1024;
// Array decoding assigns each live entry before use. Byte storage avoids both
// zero-filling unused entries and JsonValue's default validated-bit writes.
// JsonValue is an implicit-lifetime aggregate; the byte array provides storage.
template<u32 Count> struct ValueScratch {
    alignas(JsonValue) u8 storage[sizeof(JsonValue) * Count];
    operator JsonValue*() noexcept { return reinterpret_cast<JsonValue*>(storage); }
    operator const JsonValue*() const noexcept { return reinterpret_cast<const JsonValue*>(storage); }
};
static_assert(__is_trivially_copyable(JsonValue));

float clamped(float value, float low, float high) noexcept {
    return minimum(maximum(value, low), high);
}
float absf(float value) noexcept {
    return value < 0 ? -value : value;
}
using Matrix = D2D1_MATRIX_3X2_F;
constexpr Matrix identity{1, 0, 0, 1, 0, 0};
Matrix compose(const Matrix& a, const Matrix& b) noexcept {
    return {a._11 * b._11 + a._12 * b._21,         a._11 * b._12 + a._12 * b._22,
            a._21 * b._11 + a._22 * b._21,         a._21 * b._12 + a._22 * b._22,
            a._31 * b._11 + a._32 * b._21 + b._31, a._31 * b._12 + a._32 * b._22 + b._32};
}
float number(const JsonValue& v, float fallback = 0) noexcept {
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
// Signed area (exact for cubics), an upper bound of the length (L1 norm of the control polygon) and of
// the winding of a simplified outline; open figures are closed as a fill does.
struct AreaSink final : ID2D1SimplifiedGeometrySink {
    static constexpr i32 kBins = 256;
    double area = 0, length = 0;
    float top, scale;
    i32 depth[kBins + 1]{};
    explicit AreaSink(const D2D1_RECT_F& bounds) noexcept
        : top(bounds.top), scale(bounds.bottom > bounds.top ? kBins / (bounds.bottom - bounds.top) : 0) {
    }
    i32 bin(float y) const noexcept {
        return lottie_height_bin(y, top, scale, kBins);
    }
    // A line segment crosses a horizontal line once and a cubic at most three times, and only where its
    // control points reach that height; bins widen the span by one on each side against rounding.
    void record(float a, float b, float c, float d, i32 crossings) noexcept {
        const i32 first = maximum(bin(minimum(minimum(a, b), minimum(c, d))) - 1, 0),
                  last = minimum(bin(maximum(maximum(a, b), maximum(c, d))) + 1, kBins - 1);
        depth[first] += crossings;
        depth[last + 1] -= crossings;
    }
    // The most any point can wind: half the most crossings of any horizontal line.
    u32 winding() const noexcept {
        i32 sum = 0, most = 0;
        for (i32 i = 0; i < kBins; ++i) {
            sum += depth[i];
            most = maximum(most, sum);
        }
        return static_cast<u32>(maximum(most / 2, 1));
    }
    D2D1_POINT_2F start{}, current{};
    static double distance(D2D1_POINT_2F a, D2D1_POINT_2F b) noexcept {
        const double x = static_cast<double>(b.x) - a.x, y = static_cast<double>(b.y) - a.y;
        return (x < 0 ? -x : x) + (y < 0 ? -y : y);
    }
    void line(D2D1_POINT_2F to) noexcept {
        area += (static_cast<double>(current.x) * to.y - static_cast<double>(to.x) * current.y) * .5;
        length += distance(current, to);
        record(current.y, current.y, to.y, to.y, 1);
        current = to;
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
    void STDMETHODCALLTYPE SetFillMode(D2D1_FILL_MODE) noexcept override {
    }
    void STDMETHODCALLTYPE SetSegmentFlags(D2D1_PATH_SEGMENT) noexcept override {
    }
    void STDMETHODCALLTYPE BeginFigure(D2D1_POINT_2F p, D2D1_FIGURE_BEGIN) noexcept override {
        start = current = p;
    }
    void STDMETHODCALLTYPE AddLines(const D2D1_POINT_2F* p, UINT32 n) noexcept override {
        for (u32 i = 0; i < n; ++i)
            line(p[i]);
    }
    void STDMETHODCALLTYPE AddBeziers(const D2D1_BEZIER_SEGMENT* b, UINT32 n) noexcept override {
        // Three-point Gauss-Legendre is exact for the degree-five integrand x dy - y dx.
        static constexpr double nodes[]{.1127016653792583, .5, .8872983346207417}, weights[]{5. / 18, 4. / 9, 5. / 18};
        for (u32 i = 0; i < n; ++i) {
            const double x0 = current.x, y0 = current.y, x1 = b[i].point1.x, y1 = b[i].point1.y, x2 = b[i].point2.x,
                         y2 = b[i].point2.y, x3 = b[i].point3.x, y3 = b[i].point3.y;
            for (u32 k = 0; k < 3; ++k) {
                const double t = nodes[k], u = 1 - t;
                const double x = u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3,
                             y = u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3;
                const double dx = 3 * u * u * (x1 - x0) + 6 * u * t * (x2 - x1) + 3 * t * t * (x3 - x2),
                             dy = 3 * u * u * (y1 - y0) + 6 * u * t * (y2 - y1) + 3 * t * t * (y3 - y2);
                area += weights[k] * (x * dy - y * dx) * .5;
            }
            length += distance(current, b[i].point1) + distance(b[i].point1, b[i].point2) +
                      distance(b[i].point2, b[i].point3);
            record(current.y, b[i].point1.y, b[i].point2.y, b[i].point3.y, 3);
            current = b[i].point3;
        }
    }
    void STDMETHODCALLTYPE EndFigure(D2D1_FIGURE_END) noexcept override {
        line(start);
    }
    HRESULT STDMETHODCALLTYPE Close() noexcept override {
        return S_OK;
    }
};
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
    // Active matte/mask region in world space (everything outside it when
    // clip_outside), and the combine tolerance there.
    ID2D1Geometry* clip = nullptr;
    float tolerance = .1f;
    bool clip_outside = false;
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
    JsonValue get(const JsonValue& object,const char* name) noexcept {
        if(object.kind!=JsonValueKind::object || !object.source.bytes)return {};
        u32 token=locate(object.source_offset)+1,n=0;while(name[n])++n;
        JsonValue result{};bool found=false;
        while(json.input.data[json.structurals[token]]!='}') {
            const u32 start=json.structurals[token]+1,end=json.structurals[token+1];
            const JsonString key{{json.input.data+start,end-start},start,false};
            token+=3;u32 at=json.structurals[token-1]+1;
            const auto entry=value(at,token);
            if(json_name_is(key,{name,n})) {if(found)return {};result=entry;found=true;}
            if(json.input.data[json.structurals[token]]==',')++token;
        }
        return result;
    }
    bool array(const JsonValue& input,Slice<JsonValue> output,u32* count) noexcept {
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
    // Element-by-element walk of an array: the tokens array() visits, without a materialized copy.
    struct Elements { u32 token, at; bool more; };
    Elements elements(const JsonValue& input) noexcept {
        if(input.kind!=JsonValueKind::array || !input.source.bytes)return {0,0,false};
        return {locate(input.source_offset)+1,input.source_offset+1,true};
    }
    bool step(Elements& e,JsonValue* item) noexcept {
        if(!e.more)return false;
        if(json.input.data[json.structurals[e.token]]==']' && e.at>=json.structurals[e.token]){e.more=false;return false;}
        whitespace(e.at);
        if(json.input.data[e.at]==']'){e.more=false;return false;}
        *item=value(e.at,e.token);
        if(json.input.data[json.structurals[e.token]]==','){e.at=json.structurals[e.token]+1;++e.token;}
        else e.more=false;
        return true;
    }
    enum class Shape : u8 { other, group, path, ellipse, rectangle, fill, stroke, gradient_fill, gradient_stroke,
                            transform, trim, repeater, merge };
    // The item's "ty" code, decoded once instead of compared name by name.
    __declspec(noinline) Shape shape_kind(const JsonValue& value) noexcept {
        static constexpr u16 codes[]{'g' << 8 | 'r', 's' << 8 | 'h', 'e' << 8 | 'l', 'r' << 8 | 'c',
                                     'f' << 8 | 'l', 's' << 8 | 't', 'g' << 8 | 'f', 'g' << 8 | 's',
                                     't' << 8 | 'r', 't' << 8 | 'm', 'r' << 8 | 'p', 'm' << 8 | 'm'};
        JsonString text{};
        char name[3]{};
        if (!string_field(value, "ty", &text))
            return Shape::other;
        const auto decoded = json_string_decode_utf8(text, {name, 3});
        if (!decoded.ok() || decoded.bytes != 2)
            return Shape::other;
        const u16 code = static_cast<u16>(static_cast<u8>(name[0]) << 8 | static_cast<u8>(name[1]));
        for (u32 i = 0; i < sizeof(codes) / sizeof(codes[0]); ++i)
            if (codes[i] == code)
                return static_cast<Shape>(i + 1);
        return Shape::other;
    }
    bool hidden(const JsonValue& value) noexcept {
        return flag_field(value, "hd");
    }
    struct Sample { JsonValue start,end,key; float progress=0; };
    Sample sampled(JsonValue property) noexcept {
        if(property.kind==JsonValueKind::object){const auto k=get(property,"k");if(k.source.bytes)property=k;}
        Sample result{property,property,{},0};
        if(property.kind!=JsonValueKind::array)return result;
        // Keyframes are walked in order, keeping only the selected one and its neighbours.
        JsonValue previous{},current{},upcoming{},item{};
        u32 count=0,selected=0;bool plain=false,advancing=true;
        for(auto walk=elements(property);step(walk,&item);++count){
            if(count==512)return {};
            if(!count){current=item;plain=item.kind!=JsonValueKind::object || !get(item,"t").source.bytes;}
            else if(!plain && advancing){
                if(field(item,"t")<=frame){previous=current;current=item;++selected;}
                else{upcoming=item;advancing=false;}
            }
        }
        if(!count)return {};
        if(plain)return result;
        result.key=current;result.start=get(current,"s");
        if(!result.start.source.bytes && selected)result.start=get(previous,"e");
        if(!result.start.source.bytes && selected)result.start=get(previous,"s");
        result.end=result.start;
        if(!advancing && field(current,"h")!=1){
            const float begin=field(current,"t"),end=field(upcoming,"t");
            result.end=get(current,"e");
            if(!result.end.source.bytes)result.end=get(upcoming,"s");
            if(!result.end.source.bytes)result.end=result.start;
            result.progress=clamped(end>begin?(frame-begin)/(end-begin):0,0,1);
        }
        return result;
    }
    float component(JsonValue value,u32 index,float fallback=0) noexcept {
        if(value.kind==JsonValueKind::array){
            JsonValue chosen{},last{},item{};u32 n=0;
            for(auto walk=elements(value);step(walk,&item);++n){
                if(n==256)return fallback;
                if(n==index)chosen=item;
                last=item;
            }
            if(!n)return fallback;
            value=index<n?chosen:last;
        }
        return number(value,fallback);
    }
    static float bezier(float t,float a,float b) noexcept {
        const float u=1-t;return 3*u*u*t*a+3*u*t*t*b+t*t*t;
    }
    float progress(const Sample& value,u32 dimension=0) noexcept {
        if(value.progress==0 || value.progress==1)return value.progress;
        const auto in=get(value.key,"i"),out=get(value.key,"o");
        if(!in.source.bytes||!out.source.bytes)return value.progress;
        const float x1=clamped(component_field(out,"x",dimension),0,1),
                    x2=clamped(component_field(in,"x",dimension,1),0,1);
        float low=0,high=1,t=value.progress;
        for(u32 i=0;i<16;++i){t=(low+high)*.5f;
            if(bezier(t,x1,x2)<value.progress)low=t;else high=t;}
        return bezier(t,component_field(out,"y",dimension),component_field(in,"y",dimension,1));
    }
    float interpolated(const Sample& value,u32 dimension,float fallback=0) noexcept {
        const float a=component(value.start,dimension,fallback),b=component(value.end,dimension,a);
        return a+(b-a)*progress(value,dimension);
    }
    struct GradientComponents {
        float starts[256];
        ValueScratch<256> ends;
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
    float scalar(const JsonValue& property,float fallback=0) noexcept {return interpolated(sampled(property),0,fallback);}
    // An object member read as a number, constant or animated at the current frame.
    __declspec(noinline) float field(const JsonValue& object, const char* key, float fallback = 0) noexcept {
        return number(get(object, key), fallback);
    }
    __declspec(noinline) float animated_field(const JsonValue& object, const char* key, float fallback = 0) noexcept {
        return scalar(get(object, key), fallback);
    }
    __declspec(noinline) bool flag_field(const JsonValue& object, const char* key) noexcept {
        bool flag = false;
        json_value_to_bool(get(object, key), &flag);
        return flag;
    }
    __declspec(noinline) bool string_field(const JsonValue& object, const char* key, JsonString* output) noexcept {
        return json_value_as_string(get(object, key), output);
    }
    __declspec(noinline) float component_field(const JsonValue& object, const char* key, u32 index,
                                               float fallback = 0) noexcept {
        return component(get(object, key), index, fallback);
    }
    __declspec(noinline) D2D1_POINT_2F point_field(const JsonValue& object, const char* key,
                                                   D2D1_POINT_2F fallback = {}) noexcept {
        return point(get(object, key), fallback);
    }
    __declspec(noinline) D2D1_POINT_2F point_array_components(const JsonValue& value, D2D1_POINT_2F fallback) noexcept {
        JsonValue first{}, second{}, item{};
        u32 count = 0;
        for (auto walk = elements(value); step(walk, &item); ++count) {
            if (count == 256)
                return fallback;
            if (count < 2)
                (count ? second : first) = item;
        }
        if (!count)
            return fallback;
        return {number(first, fallback.x), number(count > 1 ? second : first, fallback.y)};
    }
    D2D1_POINT_2F point_components(const JsonValue& value, D2D1_POINT_2F fallback) noexcept {
        if (value.kind != JsonValueKind::array)
            return {number(value, fallback.x), number(value, fallback.y)};
        return point_array_components(value, fallback);
    }
    D2D1_POINT_2F point(const JsonValue& property,D2D1_POINT_2F fallback={}) noexcept {
        const auto x=get(property,"x");
        if(x.source.bytes)return {scalar(x),animated_field(property,"y")};
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
    Matrix transform(const JsonValue& value) noexcept {
        const auto p = point_field(value, "p"), anchor = point_field(value, "a"),
                   scale = point_field(value, "s", {100, 100});
        float angle = animated_field(value, "r") * 3.14159265358979323846f / 180;
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
    ColrColor color(const JsonValue& style, float opacity) noexcept {
        ColrColor c{0,0,0,opacity*animated_field(style,"o",100)*.01f};
        const auto value=sampled(get(style,"c"));
        c.r=interpolated(value,0);c.g=interpolated(value,1);c.b=interpolated(value,2);
        c.r = clamped(c.r, 0.f, 1.f);
        c.g = clamped(c.g, 0.f, 1.f);
        c.b = clamped(c.b, 0.f, 1.f);
        c.a = clamped(c.a, 0.f, 1.f);
        return c;
    }
    bool append(ID2D1Geometry* geometry, const Matrix& m, const ColrColor& c, bool gradient = false) noexcept {
        if (!geometry)
            return false;
        if (c.a <= 0) {
            geometry->Release();
            return true;
        }
        const Matrix* placement = &m;
        if (clip) {
            // Mattes and masks clip in world space; the leaf keeps world coordinates.
            ID2D1TransformedGeometry* placed = nullptr;
            ID2D1Geometry* clipped = nullptr;
            const bool okay = SUCCEEDED(factory->CreateTransformedGeometry(geometry, &m, &placed)) &&
                              combine(placed, clip,
                                      clip_outside ? D2D1_COMBINE_MODE_EXCLUDE : D2D1_COMBINE_MODE_INTERSECT,
                                      &clipped, tolerance);
            if (placed)
                placed->Release();
            geometry->Release();
            if (!okay)
                return false;
            geometry = clipped;
            placement = &identity;
        }
        // Direct2D's area flattens and resolves the whole outline (tens of microseconds). A leaf whose exact
        // signed area, divided by the most any point can wind, still exceeds the threshold plus twice the
        // flattening error along the outline (tolerance times length) is kept without it.
        D2D1_RECT_F bounds{};
        const bool bounded = SUCCEEDED(geometry->GetBounds(placement, &bounds));
        AreaSink outline(bounds);
        const u32 winding =
            bounded && SUCCEEDED(geometry->Simplify(D2D1_GEOMETRY_SIMPLIFICATION_OPTION_CUBICS_AND_LINES, placement,
                                                    .0001f, &outline))
                ? outline.winding()
                : 0;
        const bool large = winding && (outline.area < 0 ? -outline.area : outline.area) >
                                          winding * (.00001 + .0002 * outline.length);
        float area = 0;
        if (!large && FAILED(geometry->ComputeArea(placement, .0001f, &area))) {
            geometry->Release();
            return false;
        }
        if (!large && !(area > .00001f)) {
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
        leaf.transform = *placement;
        leaf.color = c;
        if (gradient) {
            new (storage + sizeof(Leaf)) Gradient;
            ++gradient_count;
        }
        ++leaf_count;
        return true;
    }
    bool append_style(ID2D1Geometry* geometry, const Matrix& m, const JsonValue& style, float opacity) noexcept {
        const Shape kind = shape_kind(style);
        if (kind != Shape::gradient_fill && kind != Shape::gradient_stroke)
            return append(geometry, m, color(style, opacity));
        const u32 previous = leaf_count;
        if (!append(geometry, m, {1, 1, 1, opacity * animated_field(style, "o", 100) * .01f}, true))
            return false;
        if (previous == leaf_count)
            return true;
        auto& leaf = get_leaf(previous);
        const auto gradient = get(style, "g");
        const u32 colors = static_cast<u32>(field(gradient, "p"));
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
        const auto a0 = point_field(style, "s"), b0 = point_field(style, "e");
        const auto start = transform_point(a0), end = transform_point(b0);
        if (field(style, "t", 1) == 1) {
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
    // A new path geometry with its sink open; null on failure.
    __declspec(noinline) ID2D1PathGeometry* open_path(ID2D1GeometrySink** sink) noexcept {
        ID2D1PathGeometry* geometry = nullptr;
        if (FAILED(factory->CreatePathGeometry(&geometry)) || FAILED(geometry->Open(sink))) {
            if (geometry)
                geometry->Release();
            return nullptr;
        }
        return geometry;
    }
    // Closes and releases the sink; a failed build or close also releases the geometry.
    __declspec(noinline) static bool finish_path(ID2D1PathGeometry* geometry, ID2D1GeometrySink* sink,
                                                 HRESULT built = S_OK) noexcept {
        const HRESULT closed = sink->Close();
        sink->Release();
        if (FAILED(built) || FAILED(closed)) {
            geometry->Release();
            return false;
        }
        return true;
    }
    // One "v"/"i"/"o" vertex list of a path keyframe, morphed toward the end keyframe's list by `blend`.
    __declspec(noinline) bool vertices(const JsonValue& start, const JsonValue& end, float blend, const char* key,
                                       D2D1_POINT_2F* output, u32* count) noexcept {
        *count = 0;
        const JsonValue list = get(start, key);
        if (list.kind != JsonValueKind::array || !list.source.bytes)
            return false;
        const JsonValue end_list = blend ? get(end, key) : JsonValue{};
        if (blend && (end_list.kind != JsonValueKind::array || !end_list.source.bytes))
            return false;
        auto walk = elements(list), end_walk = elements(end_list);
        JsonValue a{}, b{};
        while (step(walk, &a)) {
            if (*count == 512 || (blend && !step(end_walk, &b)))
                return false;
            const auto p = point(a), q = blend ? point(b) : p;
            output[(*count)++] = D2D1_POINT_2F{p.x + (q.x - p.x) * blend, p.y + (q.y - p.y) * blend};
        }
        return !blend || !step(end_walk, &b);
    }
    // A keyframe's "s"/"e" shape list (at most two entries, the first used).
    bool shape_unwrap(JsonValue* list) noexcept {
        if(list->kind!=JsonValueKind::array)return true;
        JsonValue first{},item{};u32 n=0;
        for(auto walk=elements(*list);step(walk,&item);++n){
            if(n==2)return false;
            if(!n)first=item;
        }
        if(!n)return false;
        *list=first;
        return true;
    }
    bool path(const JsonValue& shape, ID2D1Geometry** output, const char* key = "ks") noexcept {
        *output = nullptr;
        const auto animated=sampled(get(shape,key));
        auto value=animated.start,end=animated.end;
        if(!shape_unwrap(&value)||!shape_unwrap(&end))return false;
        D2D1_POINT_2F vertex[512],tangent_in[512],tangent_out[512];
        u32 nv=0,ni=0,no=0;
        const float blend=progress(animated);
        if(!vertices(value,end,blend,"v",vertex,&nv)||!vertices(value,end,blend,"i",tangent_in,&ni)||
            !vertices(value,end,blend,"o",tangent_out,&no)||!nv||nv!=ni||nv!=no)return false;
        ID2D1GeometrySink* sink = nullptr;
        ID2D1PathGeometry* geometry = open_path(&sink);
        if (!geometry)
            return false;
        const bool closed = flag_field(value, "c");
        sink->SetFillMode(D2D1_FILL_MODE_WINDING);
        sink->BeginFigure(vertex[0], D2D1_FIGURE_BEGIN_FILLED);
        for (u32 i = 0; i < (closed ? nv : nv - 1); ++i) {
            const u32 next = (i + 1) % nv;
            const auto a=vertex[i],b=vertex[next],o=tangent_out[i],t=tangent_in[next];
            const D2D1_BEZIER_SEGMENT segment{{a.x + o.x, a.y + o.y}, {b.x + t.x, b.y + t.y}, b};
            sink->AddBezier(segment);
        }
        sink->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        if (!finish_path(geometry, sink))
            return false;
        *output = geometry;
        return true;
    }
    // Creates the ellipse or rounded-rectangle geometry of a primitive shape item.
    bool primitive(const JsonValue& shape, Shape kind, ID2D1Geometry** output) noexcept {
        const auto p = point_field(shape, "p"), s = point_field(shape, "s");
        if (kind == Shape::ellipse) {
            ID2D1EllipseGeometry* ellipse = nullptr;
            if (FAILED(factory->CreateEllipseGeometry({p, s.x * .5f, s.y * .5f}, &ellipse)))
                return false;
            *output = ellipse;
            return true;
        }
        const float r = animated_field(shape, "r");
        ID2D1RoundedRectangleGeometry* rectangle = nullptr;
        if (FAILED(factory->CreateRoundedRectangleGeometry(
                {{p.x - s.x * .5f, p.y - s.y * .5f, p.x + s.x * .5f, p.y + s.y * .5f}, r, r}, &rectangle)))
            return false;
        *output = rectangle;
        return true;
    }
    // Appends the outline of `geometry` widened by the stroke item's width and caps.
    bool stroked(ID2D1Geometry* geometry, const Matrix& world, const JsonValue& stroke, float opacity) noexcept {
        ID2D1GeometrySink* sink = nullptr;
        ID2D1PathGeometry* widened = open_path(&sink);
        if (!widened)
            return false;
        D2D1_STROKE_STYLE_PROPERTIES style{};
        const u32 cap = static_cast<u32>(field(stroke, "lc", 1)),
                  join = static_cast<u32>(field(stroke, "lj", 1));
        style.startCap = style.endCap = style.dashCap = cap == 2   ? D2D1_CAP_STYLE_ROUND
                                                        : cap == 3 ? D2D1_CAP_STYLE_SQUARE
                                                                   : D2D1_CAP_STYLE_FLAT;
        style.lineJoin = join == 2   ? D2D1_LINE_JOIN_ROUND
                         : join == 3 ? D2D1_LINE_JOIN_BEVEL
                                     : D2D1_LINE_JOIN_MITER;
        style.miterLimit = maximum(1.f, field(stroke, "ml", 4));
        ID2D1StrokeStyle* native_style = nullptr;
        const HRESULT created = factory->CreateStrokeStyle(style, nullptr, 0, &native_style);
        const HRESULT hr = SUCCEEDED(created)
                               ? geometry->Widen(animated_field(stroke, "w"), native_style, nullptr, .1f, sink)
                               : created;
        if (native_style)
            native_style->Release();
        return finish_path(widened, sink, hr) && append_style(widened, world, stroke, opacity);
    }
    // Appends a second reference to `geometry` painted with a fill item.
    bool filled(ID2D1Geometry* geometry, const Matrix& world, const JsonValue& fill, float opacity) noexcept {
        geometry->AddRef();
        return append_style(geometry, world, fill, opacity);
    }
    bool groups(const JsonValue& items, Matrix world, float opacity, u32 depth, bool repeated = false,
                const Matrix& repetition = identity, const JsonValue& inherited_fill = {},
                const JsonValue& inherited_stroke = {}) noexcept {
        if (depth > 24)
            return false;
        ValueScratch<512> shapes;
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
                const Shape kind = shape_kind(shapes[i]);
                if (kind == Shape::transform) {
                    world = compose(transform(shapes[i]), world);
                    opacity *= animated_field(shapes[i], "o", 100) * .01f;
                } else if (kind == Shape::fill || kind == Shape::gradient_fill) {
                    fill = shapes[i];
                    fill_index = i;
                } else if (kind == Shape::stroke || kind == Shape::gradient_stroke) {
                    stroke = shapes[i];
                    stroke_index = i;
                } else if (kind == Shape::merge)
                    merged = true;
                else if (kind == Shape::repeater)
                    repeater = shapes[i];
            }
        world = compose(repetition, world);
        for (u32 i = count; i--;) {
            const auto shape = shapes[i];
            if (hidden(shape))
                continue;
            const Shape kind = merged ? Shape::other : shape_kind(shape);
            ID2D1Geometry* geometry = nullptr;
            if (merged) {
                if (i != count - 1)
                    continue;
                if (!geometry_scope(items, &geometry, depth, false))
                    return false;
            } else if (kind == Shape::group) {
                if (!groups(get(shape, "it"), world, opacity, depth + 1, false, identity, fill, stroke))
                    return false;
                continue;
            } else if (kind == Shape::path) {
                if (!path(shape, &geometry))
                    return false;
            } else if (kind == Shape::ellipse || kind == Shape::rectangle) {
                if (!primitive(shape, kind, &geometry))
                    return false;
            } else if (kind >= Shape::fill && kind <= Shape::repeater)
                continue;
            else
                return false;
            const bool has_fill = fill.source.bytes != 0,
                       fill_first = has_fill && (!stroke.source.bytes || fill_index > stroke_index);
            bool okay = !fill_first || filled(geometry, world, fill, opacity);
            if (okay && stroke.source.bytes && animated_field(stroke, "w") > 0)
                okay = stroked(geometry, world, stroke, opacity);
            if (okay && has_fill && !fill_first)
                okay = filled(geometry, world, fill, opacity);
            geometry->Release();
            if (!okay)
                return false;
        }
        if (!repeated && repeater.source.bytes) {
            const float copies = animated_field(repeater, "c");
            if (!(copies >= 0 && copies <= 32) || animated_field(repeater, "o") != 0)
                return false;
            const auto tr = get(repeater, "tr");
            const Matrix step = transform(tr);
            Matrix m = identity;
            for (u32 copy = 1; copy < static_cast<u32>(copies); ++copy) {
                m = compose(m, step);
                const float t = static_cast<float>(copy) / maximum(1.f, copies - 1);
                const float alpha =
                    (animated_field(tr, "so", 100) * (1 - t) + animated_field(tr, "eo", 100) * t) * .01f;
                if (!groups(items, inherited_world, inherited_opacity * alpha, depth + 1, true, m,
                            inherited_fill, inherited_stroke))
                    return false;
            }
        }
        return true;
    }
    bool combine(ID2D1Geometry* a, ID2D1Geometry* b, D2D1_COMBINE_MODE mode, ID2D1Geometry** output,
                 float flattening = .1f) noexcept {
        ID2D1GeometrySink* sink = nullptr;
        ID2D1PathGeometry* merged = open_path(&sink);
        if (!merged)
            return false;
        if (!finish_path(merged, sink, a->CombineWithGeometry(b, mode, nullptr, flattening, sink)))
            return false;
        *output = merged;
        return true;
    }
    // Shape modifiers consume geometry, including otherwise unstyled child groups.
    bool geometry_scope(const JsonValue& items, ID2D1Geometry** output, u32 depth,
                        bool apply_transform = true, bool* has_shapes = nullptr) noexcept {
        *output = nullptr;
        if(has_shapes)*has_shapes=false;
        if (depth > 24)
            return false;
        ValueScratch<512> shapes;
        u32 n = 0;
        if (!array(items, {shapes, 512}, &n))
            return false;
        Matrix matrix = identity;
        D2D1_COMBINE_MODE mode = D2D1_COMBINE_MODE_UNION;
        for (u32 i = 0; i < n; ++i) {
            if (hidden(shapes[i]))
                continue;
            const Shape kind = shape_kind(shapes[i]);
            if (kind == Shape::transform)
                matrix = transform(shapes[i]);
            if (kind == Shape::merge) {
                const auto mm = static_cast<u32>(field(shapes[i], "mm", 1));
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
            const Shape kind = shape_kind(shape);
            if (kind == Shape::path) {
                if (!path(shape, &part))
                    return false;
            } else if (kind == Shape::group) {
                bool child_shapes=false;
                if (!geometry_scope(get(shape, "it"), &part, depth + 1, true, &child_shapes))
                    return false;
                // A transform-only group has no path operand. A real operand
                // whose boolean result is empty still participates in merges.
                if(!child_shapes){part->Release();continue;}
            } else if (kind == Shape::ellipse || kind == Shape::rectangle) {
                if (!primitive(shape, kind, &part))
                    return false;
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
        if (!collected && !empty_geometry(&collected))
            return false;
        ID2D1TransformedGeometry* transformed = nullptr;
        if (!apply_transform)
            matrix = identity;
        if (FAILED(factory->CreateTransformedGeometry(collected, &matrix, &transformed)))
            return false;
        *output = transformed;
        if(has_shapes)*has_shapes=any_shapes;
        return true;
    }
    Matrix parent_transform(const JsonValue& layer, const ConstSlice<JsonValue>& layers, u32 depth) noexcept {
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
            if (field(layers[i], "ind", -2) == index)
                return compose(own, parent_transform(layers[i], layers, depth + 1));
        invalid_parent = true;
        return own;
    }
    // One layer's leaves. A matte source draws even when hidden.
    bool layer(const JsonValue& value, const ConstSlice<JsonValue>& siblings, const Matrix& world, float opacity, u32 depth,
               bool matte = false) noexcept {
        if ((!matte && hidden(value)) || frame < field(value, "ip", -100000) ||
            frame >= field(value, "op", 100000))
            return true;
        const float ty = field(value, "ty", -1);
        if (ty == 3)
            return true;
        const Matrix local = compose(parent_transform(value, siblings, 0), world);
        const float alpha = opacity * animated_field(get(value, "ks"), "o", 100) * .01f;
        if (alpha <= 0)
            return true;
        ID2D1Geometry* masked = nullptr;
        bool masked_outside = false;
        ID2D1Geometry* outer = clip;
        const bool outer_outside = clip_outside;
        if (!mask(value, local, &masked, &masked_outside) ||
            (masked && !push_clip(masked, masked_outside)))
            return false;
        bool okay = true;
        if (ty == 4) {
            okay = groups(get(value, "shapes"), local, alpha, 0);
        } else if (ty == 1) {
            // Solid layer: a filled sw x sh rectangle in layer space.
            const D2D1_RECT_F bounds{0, 0, field(value, "sw"), field(value, "sh")};
            ID2D1RectangleGeometry* rectangle = nullptr;
            ColrColor c{0, 0, 0, alpha};
            okay = solid_color(get(value, "sc"), &c) &&
                   SUCCEEDED(factory->CreateRectangleGeometry(&bounds, &rectangle)) && append(rectangle, local, c);
        } else if (ty == 0) {
            JsonString reference{};
            okay = string_field(value, "refId", &reference) && assets.kind == JsonValueKind::array && assets.source.bytes;
            JsonValue asset{}, item{};
            u32 n = 0;
            for (auto walk = elements(assets); okay && step(walk, &item); ++n) {
                JsonString name{};
                okay = n != 256; // an asset table this long is rejected whole
                if (okay && !asset.source.bytes && string_field(item, "id", &name) &&
                    name.encoded.bytes == reference.encoded.bytes &&
                    bytes_equal(name.encoded.data, reference.encoded.data, name.encoded.bytes))
                    asset = item;
            }
            if (okay && asset.source.bytes) {
                const float previous = frame;
                frame = (frame - field(value, "st")) / maximum(.0001f, field(value, "sr", 1));
                okay = layers(get(asset, "layers"), local, alpha, depth + 1);
                frame = previous;
            } else
                okay = false;
        } else
            okay = false;
        if (clip != outer) {
            clip->Release();
            clip = outer;
            clip_outside = outer_outside;
        }
        return okay;
    }
    bool empty_geometry(ID2D1Geometry** output) noexcept {
        ID2D1GeometrySink* sink = nullptr;
        ID2D1PathGeometry* empty = open_path(&sink);
        if (!empty || !finish_path(empty, sink))
            return false;
        *output = empty;
        return true;
    }
    // The world-space union of a matte source's painted geometry.
    bool matte_region(const JsonValue& source, const ConstSlice<JsonValue>& siblings, const Matrix& world, float opacity, u32 depth,
                      ID2D1Geometry** output) noexcept {
        *output = nullptr;
        const u32 mark = leaf_count;
        const u16 gradients = gradient_count;
        ID2D1Geometry* const outer = clip;
        const bool outer_outside = clip_outside;
        clip = nullptr;
        bool okay = layer(source, siblings, world, opacity, depth, true);
        clip = outer;
        clip_outside = outer_outside;
        ID2D1Geometry* region = nullptr;
        okay = okay && empty_geometry(&region);
        for (u32 i = mark; i < leaf_count; ++i) {
            auto& leaf = get_leaf(i);
            ID2D1TransformedGeometry* placed = nullptr;
            ID2D1Geometry* joined = nullptr;
            if (okay && SUCCEEDED(factory->CreateTransformedGeometry(leaf.geometry, &leaf.transform, &placed)) &&
                combine(region, placed, D2D1_COMBINE_MODE_UNION, &joined, tolerance)) {
                region->Release();
                region = joined;
            } else
                okay = false;
            if (placed)
                placed->Release();
            leaf.geometry->Release();
        }
        leaf_count = mark;
        gradient_count = gradients;
        if (!okay) {
            if (region)
                region->Release();
            return false;
        }
        *output = region;
        return true;
    }
    // Region algebra over (region, outside) pairs, where outside means
    // everything except the region; no unbounded geometry is ever built.
    // `operand` is consumed; the result replaces *region.
    bool region_combine(ID2D1Geometry** region, bool* outside, ID2D1Geometry* operand, bool operand_outside,
                        char op) noexcept {
        // op: 'a' union, 's' subtract, 'i' intersect. Rows: region and operand each inside or outside;
        // an entry holds the combine mode (bits 0-1), whether the operands swap (bit 2) and whether
        // the result stays an outside (bit 3).
        static_assert(D2D1_COMBINE_MODE_UNION == 0 && D2D1_COMBINE_MODE_INTERSECT == 1 &&
                      D2D1_COMBINE_MODE_EXCLUDE == 3);
        static constexpr u8 plan[]{0, 3, 1, 15, 1, 3, 11, 8, 7, 9, 7, 8};
        const u32 step = plan[((*outside ? 2u : 0u) + (operand_outside ? 1u : 0u)) * 3 +
                              (op == 'a' ? 0u : op == 's' ? 1u : 2u)];
        const auto mode = static_cast<D2D1_COMBINE_MODE>(step & 3);
        const bool swap = (step & 4) != 0, result_outside = (step & 8) != 0;
        ID2D1Geometry* next = nullptr;
        const bool okay = swap ? combine(operand, *region, mode, &next, tolerance)
                               : combine(*region, operand, mode, &next, tolerance);
        operand->Release();
        if (!okay)
            return false;
        (*region)->Release();
        *region = next;
        *outside = result_outside;
        return true;
    }
    // Installs the active clip intersected with (region, outside); region is
    // consumed. The caller restores the previous clip.
    bool push_clip(ID2D1Geometry* region, bool outside) noexcept {
        if (!clip) {
            clip = region;
            clip_outside = outside;
            return true;
        }
        clip->AddRef();
        ID2D1Geometry* combined = clip;
        bool combined_outside = clip_outside;
        if (!region_combine(&combined, &combined_outside, region, outside, 'i')) {
            combined->Release();
            return false;
        }
        clip = combined;
        clip_outside = combined_outside;
        return true;
    }
    // Layer masks (masksProperties) as one world-space region; null when none apply.
    bool mask(const JsonValue& owner, const Matrix& local, ID2D1Geometry** output, bool* outside) noexcept {
        *output = nullptr;
        *outside = false;
        const auto property = get(owner, "masksProperties");
        if (!property.source.bytes)
            return true;
        ValueScratch<64> masks;
        u32 count = 0;
        if (!array(property, {masks, 64}, &count))
            return false;
        ID2D1Geometry* region = nullptr;
        bool region_outside = false, okay = true;
        for (u32 i = 0; okay && i < count; ++i) {
            JsonString mode{};
            const char op = string_field(masks[i], "mode", &mode) && mode.encoded.bytes == 1
                                ? mode.encoded.data[0]
                                : 'a';
            if (op != 'a' && op != 's' && op != 'i')
                continue;
            ID2D1Geometry* shape = nullptr;
            ID2D1TransformedGeometry* placed = nullptr;
            okay = path(masks[i], &shape, "pt") && SUCCEEDED(factory->CreateTransformedGeometry(shape, &local, &placed));
            if (shape)
                shape->Release();
            if (!okay)
                break;
            const bool inverted = flag_field(masks[i], "inv");
            // Adding starts from nothing; subtracting or intersecting from everything.
            if (!region) {
                okay = empty_geometry(&region);
                region_outside = op != 'a';
                if (!okay) {
                    placed->Release();
                    break;
                }
            }
            okay = region_combine(&region, &region_outside, placed, inverted, op);
        }
        if (!okay) {
            if (region)
                region->Release();
            return false;
        }
        *output = region;
        *outside = region_outside;
        return true;
    }
    static bool hex_digit(char c, u32* value) noexcept {
        *value = c >= '0' && c <= '9'   ? static_cast<u32>(c - '0')
                 : c >= 'a' && c <= 'f' ? static_cast<u32>(c - 'a' + 10)
                 : c >= 'A' && c <= 'F' ? static_cast<u32>(c - 'A' + 10)
                                        : 16u;
        return *value < 16;
    }
    bool solid_color(const JsonValue& value, ColrColor* c) noexcept {
        JsonString text{};
        if (!json_value_as_string(value, &text) || text.encoded.bytes != 7 || text.encoded.data[0] != '#')
            return false;
        float* channels[]{&c->r, &c->g, &c->b};
        for (u32 i = 0; i < 3; ++i) {
            u32 high = 0, low = 0;
            if (!hex_digit(text.encoded.data[1 + i * 2], &high) || !hex_digit(text.encoded.data[2 + i * 2], &low))
                return false;
            *channels[i] = static_cast<float>(high * 16 + low) / 255.f;
        }
        return true;
    }
    bool layers(const JsonValue& array_value, const Matrix& world, float opacity, u32 depth) noexcept {
        if (depth > 16)
            return false;
        ValueScratch<512> values;
        u32 count = 0;
        if (!array(array_value, {values, 512}, &count))
            return false;
        const ConstSlice<JsonValue> siblings{values, count};
        for (u32 i = count; i--;) {
            const auto current = values[i];
            // A track matte source only shapes the layer bound to it.
            if (field(current, "td") != 0)
                continue;
            const u32 matte_type = static_cast<u32>(field(current, "tt"));
            if (matte_type < 1 || matte_type > 4) {
                if (!layer(current, siblings, world, opacity, depth))
                    return false;
                continue;
            }
            // tp names the source by index; older files bind the layer above.
            const auto bound = get(current, "tp");
            JsonValue source{};
            if (bound.source.bytes) {
                const float index = number(bound, -1);
                for (u32 j = 0; j < count; ++j)
                    if (field(values[j], "ind", -2) == index)
                        source = values[j];
            } else if (i)
                source = values[i - 1];
            ID2D1Geometry* region = nullptr;
            if (!source.source.bytes || !matte_region(source, siblings, world, opacity, depth, &region))
                return false;
            // Luma mattes use coverage like alpha mattes; inverted kinds keep the outside.
            ID2D1Geometry* const previous = clip;
            const bool previous_outside = clip_outside;
            if (!push_clip(region, matte_type == 2 || matte_type == 4))
                return false;
            const bool okay = layer(current, siblings, world, opacity, depth);
            clip->Release();
            clip = previous;
            clip_outside = previous_outside;
            if (!okay)
                return false;
        }
        return true;
    }
};
static_assert(offsetof(Document, leaf_records) == 104);
static_assert(offsetof(Document, leaf_pool) == 2152);
static_assert(sizeof(Document) == 747640);
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
                     float frame, float pixels) noexcept {
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
    doc->tolerance = .1f / extent;
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
    return colr_program_build(program, source, root_paint, {0, 0, w / extent, h / extent}, pixels);
}
} // namespace
bool lottie_program_compile_frame(ConstSlice<u8> encoded, u32 frame_index, float pixels,
                                  LottieTiming* timing, LottieProgramAcquire acquire, void* context) noexcept {
    if (!timing || !acquire || !encoded.data || !encoded.count || encoded.count > kLottieMaximumBytes)
        return false;
    reset_value(timing);
    ScannedJson source;
    if (!source.read(encoded) || !timing_from_index(source.json, timing) || frame_index >= timing->frames)
        return false;
    auto* program = acquire(context);
    const float frame = timing->first + frame_index;
    return compile_parameters_valid(program, frame, pixels) &&
           compile_indexed(source.json, program, frame, pixels);
}
} // namespace maicord
