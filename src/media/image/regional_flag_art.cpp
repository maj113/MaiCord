#include "maicord/media/image/regional_flag_art.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/core/byte_range.hpp"
#include "maicord/sys/simd.hpp"

namespace maicord { namespace {
constexpr double pi = 3.14159265358979323846;
double root(double value) noexcept {return _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(value)));}
double absolute(double value) noexcept {return value < 0 ? -value : value;}
void sincos(double angle, double& sine, double& cosine) noexcept {
    angle -= static_cast<i64>(angle / (2 * pi)) * (2 * pi);
    if (angle > pi) angle -= 2 * pi;
    if (angle < -pi) angle += 2 * pi;
    double term = angle; sine = angle;
    for (u32 n = 1; n < 12; ++n) {term *= -angle * angle / ((2 * n) * (2 * n + 1)); sine += term;}
    term = 1; cosine = 1;
    for (u32 n = 1; n < 12; ++n) {term *= -angle * angle / ((2 * n - 1) * (2 * n)); cosine += term;}
}
constexpr u8 kRecordEnd = 0xFF;
struct Reader {
    const u8* at; const u8* end; bool ok = true;
    u8 byte() noexcept {if (at < end) return *at++; ok = false; return 0;}
    u32 varint() noexcept {
        u32 value = 0;
        for (u32 shift = 0; shift < 32; shift += 7) {const u8 b = byte(); value |= u32{b & 127u} << shift; if (b < 128) return value;}
        ok = false; return 0;
    }
    // Every number takes at least one byte.
    bool fits(u32 count) const noexcept {return count <= static_cast<usize>(end - at);}
};
// A zigzag varint in `unit` hundredths, read as decimal text is: the whole
// units, then each digit d of the fraction as d / 10^k. That order is what the
// SVG sources' numbers mean to a decimal parser, so every outline point keeps
// the exact value it had when flags were drawn from SVG text.
double decimal(Reader& r, u32 unit) noexcept {
    const u32 zigzag = r.varint(), magnitude = (zigzag >> 1) + (zigzag & 1);
    if (magnitude > 409600 / unit) {r.ok = false; return 0;}
    const u32 hundredths = magnitude * unit;
    double value = hundredths / 100;
    value += (hundredths / 10 % 10) / 10.0;
    value += (hundredths % 10) / 100.0;
    return zigzag & 1 ? -value : value;
}
// The outline commands one path command adds at most: a move may close the
// previous figure first, and arc_piece splits an arc at most five levels deep.
u32 command_bound(u8 letter, u32 count) noexcept {
    switch (letter | 32) {
    case 'z': return 1;
    case 'm': return count / 2 + 1;
    case 'l': return count / 2;
    case 'h': case 'v': return count;
    case 'c': return count / 6;
    case 's': case 'q': return count / 4;
    case 'a': return count / 7 * 32;
    default: return 0;
    }
}
struct Point {double x, y;};
Point add(Point a, Point b) noexcept {return {a.x + b.x, a.y + b.y};}
Point reflect(Point control, Point around) noexcept {return {2 * around.x - control.x, 2 * around.y - control.y};}
struct Path {
    GlyphOutline* out; Point current{}, start{}, control{}; bool figure = false; char previous = 0;
    bool emit(GlyphPathCommandKind kind, Point a = {}, Point b = {}, Point d = {}) noexcept {
        if (out->count >= out->capacity) {++out->rejected; return false;}
        const Point points[]{a,b,d}; const u32 count = kind == GlyphPathCommandKind::cubic ? 3 : 1;
        if (kind != GlyphPathCommandKind::end_closed)
            for (u32 i = 0; i < count; ++i)
                if (!(points[i].x >= -16384 && points[i].x <= 16384 &&
                      points[i].y >= -16384 && points[i].y <= 16384)) return false;
        out->commands[out->count++] = {static_cast<float>(a.x), static_cast<float>(a.y),
            static_cast<float>(b.x), static_cast<float>(b.y), static_cast<float>(d.x), static_cast<float>(d.y), kind, {}};
        if (kind == GlyphPathCommandKind::end_closed) return true;
        for (u32 i = 0; i < count; ++i) {
            const float x = static_cast<float>(points[i].x), y = static_cast<float>(points[i].y);
            if (!out->has_bounds) {out->minimum_x = out->maximum_x = x; out->minimum_y = out->maximum_y = y; out->has_bounds = true;}
            else {out->minimum_x = minimum(out->minimum_x,x); out->maximum_x = maximum(out->maximum_x,x);
                out->minimum_y = minimum(out->minimum_y,y); out->maximum_y = maximum(out->maximum_y,y);}
        } return true;
    }
    bool close() noexcept {if (!figure) return true; figure = false; current = start; return emit(GlyphPathCommandKind::end_closed);}
    bool move(Point point) noexcept {if (!close() || !emit(GlyphPathCommandKind::begin_filled, point)) return false; current = start = point; figure = true; return true;}
    bool line(Point point) noexcept {if (!figure || !emit(GlyphPathCommandKind::line, point)) return false; current = point; return true;}
    bool cubic(Point a, Point b, Point point) noexcept {if (!figure || !emit(GlyphPathCommandKind::cubic,a,b,point)) return false; control = b; current = point; return true;}
};
Point ellipse(Point unit, Point center, double rx, double ry, double sine, double cosine) noexcept {
    return {center.x + rx * unit.x * cosine - ry * unit.y * sine, center.y + rx * unit.x * sine + ry * unit.y * cosine};
}
bool arc_piece(Path& path, Point a, Point b, Point center, double rx, double ry,
               double sine, double cosine, bool sweep, u32 depth = 0) noexcept {
    if (depth > 5) return false;
    const double dot = a.x*b.x + a.y*b.y, cross = a.x*b.y - a.y*b.x;
    if (dot >= 0 && (sweep ? cross >= 0 : cross <= 0)) {
        const double k = (4.0 / 3.0) * cross / (1 + dot + root(maximum(0.0,2 + 2 * dot)));
        const Point p{a.x-k*a.y,a.y+k*a.x}, q{b.x+k*b.y,b.y-k*b.x};
        return path.cubic(ellipse(p,center,rx,ry,sine,cosine),ellipse(q,center,rx,ry,sine,cosine),ellipse(b,center,rx,ry,sine,cosine));
    }
    Point middle{a.x+b.x,a.y+b.y}; const double length = root(middle.x*middle.x+middle.y*middle.y);
    if (length < 1e-10) middle = sweep ? Point{-a.y,a.x} : Point{a.y,-a.x};
    else {const double sign = (sweep ? cross < 0 : cross > 0) ? -1 : 1; middle = {middle.x/length*sign,middle.y/length*sign};}
    return arc_piece(path,a,middle,center,rx,ry,sine,cosine,sweep,depth+1) &&
           arc_piece(path,middle,b,center,rx,ry,sine,cosine,sweep,depth+1);
}
bool arc(Path& path, double rx, double ry, double rotation, bool large, bool sweep, Point end) noexcept {
    if (path.current.x == end.x && path.current.y == end.y) return true;
    rx = absolute(rx); ry = absolute(ry); if (!rx || !ry) return path.line(end);
    double sine=0,cosine=0; sincos(rotation*pi/180,sine,cosine);
    const double dx=(path.current.x-end.x)*.5,dy=(path.current.y-end.y)*.5;
    const double x=cosine*dx+sine*dy,y=-sine*dx+cosine*dy;
    const double ratio=x*x/(rx*rx)+y*y/(ry*ry); if (ratio > 1) {const double scale=root(ratio); rx*=scale;ry*=scale;}
    const double denominator=rx*rx*y*y+ry*ry*x*x;
    if (!(denominator > 0)) return false;
    const double factor=(large == sweep ? -1 : 1)*root(maximum(0.0,(rx*rx*ry*ry-denominator)/denominator));
    const double cx=factor*rx*y/ry,cy=-factor*ry*x/rx;
    const Point center{cosine*cx-sine*cy+(path.current.x+end.x)*.5,sine*cx+cosine*cy+(path.current.y+end.y)*.5};
    const Point a{(x-cx)/rx,(y-cy)/ry},b{(-x-cx)/rx,(-y-cy)/ry};
    if (!arc_piece(path,a,b,center,rx,ry,sine,cosine,sweep)) return false; path.current=end; return true;
}
// Per command: its letter, the count of its numbers, then the numbers.
bool path_outline(ConstSlice<u8> input, Path& path) noexcept {
    Reader r{input.data, input.data + input.count};
    while (r.at < r.end) {
        char command = static_cast<char>(r.byte());
        const u32 count = r.varint();
        if (!r.ok || !r.fits(count)) return false;
        const char letter = command >= 'a' && command <= 'z' ? static_cast<char>(command - 32) : command;
        if (letter == 'Z') {if (count || !path.close()) return false; path.previous = letter; continue;}
        u32 step = 0;
        switch (letter) {case 'M': case 'L': step = 2; break; case 'H': case 'V': step = 1; break;
            case 'C': step = 6; break; case 'S': case 'Q': step = 4; break; case 'A': step = 7; break; default: return false;}
        if (!count || count % step) return false;
        for (u32 first = 0; first < count; first += step) {
            double v[7]{};
            for (u32 i = 0; i < step; ++i) v[i] = decimal(r, kRegionalFlagPathUnit);
            if (!r.ok) return false;
            // Pairs after a move's first are lines.
            const bool relative = command >= 'a' && command <= 'z';
            const char upper = relative ? static_cast<char>(command - 32) : command;
            const Point origin = path.current;
            const auto point = [&](u32 i) noexcept {const Point p{v[i], v[i + 1]}; return relative ? add(origin, p) : p;};
            bool ok = false;
            if (upper == 'M') {ok = path.move(point(0)); command = relative ? 'l' : 'L';}
            else if (upper == 'L') ok = path.line(point(0));
            else if (upper == 'H') ok = path.line({relative ? origin.x + v[0] : v[0], origin.y});
            else if (upper == 'V') ok = path.line({origin.x, relative ? origin.y + v[0] : v[0]});
            else if (upper == 'C') ok = path.cubic(point(0), point(2), point(4));
            else if (upper == 'S') ok = path.cubic(path.previous == 'C' || path.previous == 'S' ? reflect(path.control, origin) : origin, point(0), point(2));
            else if (upper == 'Q') {const auto q = point(0), end = point(2);
                ok = path.cubic({origin.x + (q.x - origin.x) * 2 / 3, origin.y + (q.y - origin.y) * 2 / 3},
                                {end.x + (q.x - end.x) * 2 / 3, end.y + (q.y - end.y) * 2 / 3}, end); path.control = q;}
            else {
                if ((v[3] != 0 && v[3] != 1) || (v[4] != 0 && v[4] != 1)) return false;
                ok = arc(path, v[0], v[1], v[2], v[3] != 0, v[4] != 0, point(5));
            }
            if (!ok) return false;
            path.previous = upper;
        }
    }
    return path.close();
}
} // namespace

bool regional_flag_record_parse(ConstSlice<u8> record, RegionalFlagArt* output) noexcept {
    const ConstSlice<u8> destination{reinterpret_cast<const u8*>(output), sizeof(RegionalFlagArt)};
    if (!output || !byte_range_valid(destination) || !byte_range_valid(record) || !record.count ||
        byte_ranges_overlap(record, destination)) return false;
    output->count = 0;
    Reader r{record.data, record.data + record.count};
    for (u8 header = r.byte(); r.ok && header != kRecordEnd; header = r.byte()) {
        // Bits 0-1 the kind (path, circle, ellipse), bit 2 even-odd, bit 3 the ellipse rotation.
        const u32 kind = header & 3;
        if (header > 15 || kind == 3 || ((header & 8) && kind != 2) || output->count == kRegionalFlagMaximumLayers)
            return false;
        RegionalFlagLayer& layer = output->layers[output->count++];
        reset_value(&layer);
        const u8 red = r.byte(), green = r.byte(), blue = r.byte();
        layer.color = {red / 255.f, green / 255.f, blue / 255.f, 1};
        layer.fill_rule = header & 4 ? GlyphFillRule::alternate : GlyphFillRule::winding;
        layer.rotation = header & 8 ? -76.714f : 0;
        if (kind == 0) {
            const u8* first = r.at;
            u64 bound = 1; // The closing of the last figure.
            for (u8 letter = r.byte(); r.ok && letter; letter = r.byte()) {
                const u32 count = r.varint();
                if (!r.fits(count)) return false;
                for (u32 i = 0; i < count; ++i) r.varint();
                bound += command_bound(letter, count);
            }
            if (!r.ok || r.at - 1 == first) return false;
            layer.path = {first, static_cast<u32>(r.at - 1 - first)};
            layer.command_bound = static_cast<u32>(minimum<u64>(bound, kRegionalFlagMaximumCommands));
        } else {
            float* const geometry[]{&layer.cx, &layer.cy, &layer.rx, &layer.ry};
            for (u32 i = 0; i < (kind == 1 ? 3u : 4u); ++i) *geometry[i] = static_cast<float>(decimal(r, 1));
            if (kind == 1) layer.ry = layer.rx;
            if (!r.ok || !(layer.rx > 0) || !(layer.ry > 0)) return false;
            layer.command_bound = 8; // A figure of four quarter arcs.
        }
    }
    return r.ok && r.at == r.end && output->count;
}

bool regional_flag_layer_outline(const RegionalFlagLayer& layer, GlyphOutline* output) noexcept {
    const ConstSlice<u8> layer_range{reinterpret_cast<const u8*>(&layer),sizeof(layer)};
    const ConstSlice<u8> result_range{reinterpret_cast<const u8*>(output),sizeof(GlyphOutline)};
    const ConstSlice<u8> path_range=layer.path;
    if(!output||!byte_range_valid(layer_range)||!byte_range_valid(result_range)||
        !byte_range_valid(path_range)||layer.path.count>kRegionalFlagScratchBytes||
        byte_ranges_overlap(layer_range,result_range)||byte_ranges_overlap(path_range,result_range)||
        !output->commands||!output->capacity||output->capacity>kRegionalFlagMaximumCommands)return false;
    const ConstSlice<u8> commands_range{reinterpret_cast<const u8*>(output->commands),output->capacity*static_cast<u32>(sizeof(GlyphPathCommand))};
    if(!byte_range_valid(commands_range)||byte_ranges_overlap(commands_range,path_range)||
        byte_ranges_overlap(commands_range,layer_range)||byte_ranges_overlap(commands_range,result_range))return false;
    const float geometry[]{layer.cx,layer.cy,layer.rx,layer.ry};
    for(float value:geometry)if(!(value>=-4096&&value<=4096))return false;
    if((layer.rotation!=0&&layer.rotation!=-76.714f)||
        (layer.fill_rule!=GlyphFillRule::winding&&layer.fill_rule!=GlyphFillRule::alternate))return false;
    auto* commands=output->commands;const auto capacity=output->capacity;*output={commands,capacity};output->fill_rule=layer.fill_rule;
    Path path{output};
    if(layer.path.count)return path_outline(layer.path,path)&&output->has_bounds;
    if(!(layer.rx>0)||!(layer.ry>0))return false;
    double sine=0,cosine=0;sincos(layer.rotation*pi/180,sine,cosine);
    Point center{layer.cx,layer.cy};
    if(layer.rotation) {const double x=center.x-13.001,y=center.y-18.001;
        center={13.001+x*cosine-y*sine,18.001+x*sine+y*cosine};}
    const Point points[]={{1,0},{0,1},{-1,0},{0,-1},{1,0}};
    if(!path.move(ellipse(points[0],center,layer.rx,layer.ry,sine,cosine)))return false;
    for(u32 i=0;i<4;++i)if(!arc_piece(path,points[i],points[i+1],center,layer.rx,layer.ry,sine,cosine,true))return false;
    return path.close();
}

} // namespace maicord
