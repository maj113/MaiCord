#include "maicord/media/regional_flag_svg.hpp"
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
bool equal(Utf8View value, const char* literal) noexcept {
    u32 bytes = 0; while (literal[bytes]) ++bytes;
    if (bytes != value.bytes) return false;
    for (u32 i = 0; i < bytes; ++i) if (value.data[i] != literal[i]) return false;
    return true;
}
bool space(char c) noexcept {return c == ' ' || c == '\t' || c == '\r' || c == '\n';}
struct Cursor {Utf8View input; u32 at = 0;};
void skip(Cursor& c, bool comma = false) noexcept {
    while (c.at < c.input.bytes && (space(c.input.data[c.at]) || (comma && c.input.data[c.at] == ','))) ++c.at;
}
bool number(Cursor& c, double& value) noexcept {
    skip(c, true); bool negative = false;
    if (c.at < c.input.bytes && (c.input.data[c.at] == '-' || c.input.data[c.at] == '+')) negative = c.input.data[c.at++] == '-';
    value = 0; u32 digits = 0;
    while (c.at < c.input.bytes && c.input.data[c.at] >= '0' && c.input.data[c.at] <= '9') {
        if (++digits > 12) return false; value = value * 10 + c.input.data[c.at++] - '0';
    }
    if (c.at < c.input.bytes && c.input.data[c.at] == '.') {
        ++c.at; double divisor = 1;
        while (c.at < c.input.bytes && c.input.data[c.at] >= '0' && c.input.data[c.at] <= '9') {
            if (++digits > 12) return false; divisor *= 10; value += (c.input.data[c.at++] - '0') / divisor;
        }
    }
    if (!digits) return false;
    if (c.at < c.input.bytes && (c.input.data[c.at] == 'e' || c.input.data[c.at] == 'E')) {
        ++c.at; bool minus = false;
        if (c.at < c.input.bytes && (c.input.data[c.at] == '+' || c.input.data[c.at] == '-')) minus = c.input.data[c.at++] == '-';
        u32 exponent = 0, count = 0;
        while (c.at < c.input.bytes && c.input.data[c.at] >= '0' && c.input.data[c.at] <= '9') {
            if (++count > 2) return false; exponent = exponent * 10 + c.input.data[c.at++] - '0';
        }
        if (!count || exponent > 8) return false;
        while (exponent--) value = minus ? value / 10 : value * 10;
    }
    if (value > 4096) return false; if (negative) value = -value; return true;
}
bool scalar(Utf8View input, float& output) noexcept {
    Cursor cursor{input}; double value = 0;
    if (!number(cursor, value)) return false; skip(cursor);
    if (cursor.at != input.bytes) return false; output = static_cast<float>(value); return true;
}
bool name(Cursor& c, Utf8View& output) noexcept {
    const u32 first = c.at;
    while (c.at < c.input.bytes) {const char v = c.input.data[c.at];
        if ((v >= 'a' && v <= 'z') || (v >= 'A' && v <= 'Z') || v == ':' || v == '-') ++c.at; else break;}
    output = {c.input.data + first, c.at - first}; return output.bytes != 0;
}
bool color(Utf8View value, ColrColor& output) noexcept {
    u32 rgb = 0;
    if (equal(value, "red")) rgb = 0xff0000;
    else if (equal(value, "green")) rgb = 0x008000;
    else if (equal(value, "navy")) rgb = 0x000080;
    else {
        if ((value.bytes != 4 && value.bytes != 7) || value.data[0] != '#') return false;
        for (u32 i = 1; i < value.bytes; ++i) {
            const char v = value.data[i]; u32 digit = 0;
            if (v >= '0' && v <= '9') digit = v - '0';
            else if (v >= 'a' && v <= 'f') digit = v - 'a' + 10;
            else if (v >= 'A' && v <= 'F') digit = v - 'A' + 10; else return false;
            rgb = value.bytes == 4 ? (rgb << 8) | (digit * 17) : (rgb << 4) | digit;
        }
    }
    output = {((rgb >> 16) & 255) / 255.f, ((rgb >> 8) & 255) / 255.f, (rgb & 255) / 255.f, 1}; return true;
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
bool flag(Cursor& c, bool& output) noexcept {skip(c,true); if(c.at==c.input.bytes || (c.input.data[c.at]!='0' && c.input.data[c.at]!='1'))return false;output=c.input.data[c.at++]=='1';return true;}
bool path_outline(Utf8View input, Path& path) noexcept {
    Cursor c{input}; char command=0; u32 work=0;
    while (true) {
        skip(c,true); if (c.at==c.input.bytes) return path.close();
        if (++work > kRegionalFlagSvgMaximumCommands) return false;
        const char lead=c.input.data[c.at]; if ((lead>='A'&&lead<='Z')||(lead>='a'&&lead<='z')) {command=lead;++c.at;}
        if (!command) return false;
        const bool relative=command>='a'&&command<='z'; const char upper=relative?static_cast<char>(command-32):command;
        if (upper=='Z') {if(!path.close())return false;path.previous=upper;command=0;continue;}
        double v[7]{}; u32 count=0;
        switch(upper) {case 'M':case 'L':count=2;break;case 'H':case 'V':count=1;break;
            case 'C':count=6;break;case 'S':case 'Q':count=4;break;case 'A':count=3;break;default:return false;}
        for(u32 i=0;i<count;++i)if(!number(c,v[i]))return false;
        const Point origin=path.current;
        const auto point=[&](u32 i) noexcept {const Point p{v[i],v[i+1]};return relative?add(origin,p):p;};
        bool ok=false;
        if(upper=='M') {ok=path.move(point(0));command=relative?'l':'L';}
        else if(upper=='L')ok=path.line(point(0));
        else if(upper=='H')ok=path.line({relative?origin.x+v[0]:v[0],origin.y});
        else if(upper=='V')ok=path.line({origin.x,relative?origin.y+v[0]:v[0]});
        else if(upper=='C')ok=path.cubic(point(0),point(2),point(4));
        else if(upper=='S')ok=path.cubic(path.previous=='C'||path.previous=='S'?reflect(path.control,origin):origin,point(0),point(2));
        else if(upper=='Q') {const auto q=point(0),end=point(2);
            ok=path.cubic({origin.x+(q.x-origin.x)*2/3,origin.y+(q.y-origin.y)*2/3},
                          {end.x+(q.x-end.x)*2/3,end.y+(q.y-end.y)*2/3},end);path.control=q;}
        else {bool large=false,sweep=false;double x=0,y=0;
            if(!flag(c,large)||!flag(c,sweep)||!number(c,x)||!number(c,y))return false;
            ok=arc(path,v[0],v[1],v[2],large,sweep,relative?add(origin,{x,y}):Point{x,y});}
        if(!ok)return false;path.previous=upper;
    }
}
} // namespace

bool regional_flag_svg_parse(ConstSlice<u8> encoded, RegionalFlagSvg* output) noexcept {
    const ConstSlice<u8> destination{reinterpret_cast<const u8*>(output),sizeof(RegionalFlagSvg)};
    if(!output||!byte_range_valid(destination)||!byte_range_valid(encoded)||!encoded.count||
        encoded.count>kRegionalFlagSvgMaximumBytes||byte_ranges_overlap(encoded,destination))return false;
    reset_value(output);
    Cursor c{{reinterpret_cast<const char*>(encoded.data),encoded.count}};
    RegionalFlagSvgLayer inherited[4]{}; inherited[0].color={0,0,0,1};inherited[0].fill_rule=GlyphFillRule::winding;
    u32 depth=0,elements=0;bool root_seen=false,root_closed=false;
    while(true) {
        skip(c);if(c.at==c.input.bytes)return root_seen&&root_closed&&output->count;
        if(root_closed||c.input.data[c.at++]!='<'||++elements>64)return false;
        bool closing=false;if(c.at<c.input.bytes&&c.input.data[c.at]=='/'){closing=true;++c.at;}
        Utf8View tag{};if(!name(c,tag))return false;
        if(closing) {skip(c);if(c.at==c.input.bytes||c.input.data[c.at++]!='>')return false;
            if(equal(tag,"g")){if(!depth)return false;--depth;}
            else if(equal(tag,"svg")){if(depth||!root_seen)return false;root_closed=true;}
            else return false;continue;}
        const bool svg=equal(tag,"svg"),group=equal(tag,"g"),path=equal(tag,"path"),circle=equal(tag,"circle"),ellipse_tag=equal(tag,"ellipse");
        if((!svg&&!group&&!path&&!circle&&!ellipse_tag)||(svg&&root_seen)||(!svg&&!root_seen))return false;
        RegionalFlagSvgLayer layer=inherited[depth];reset_value(&layer.path);layer.cx=layer.cy=layer.rx=layer.ry=layer.rotation=0;
        u32 seen=0;bool self=false;
        while(true) {
            skip(c);if(c.at==c.input.bytes)return false;
            if(c.input.data[c.at]=='>'){++c.at;break;}
            if(c.input.data[c.at]=='/'&&c.at+1<c.input.bytes&&c.input.data[c.at+1]=='>'){c.at+=2;self=true;break;}
            Utf8View attr{},value{};if(!name(c,attr))return false;skip(c);
            if(c.at==c.input.bytes||c.input.data[c.at++]!='=')return false;skip(c);
            if(c.at==c.input.bytes||c.input.data[c.at++]!='"')return false;
            const u32 first=c.at;while(c.at<c.input.bytes&&c.input.data[c.at]!='"'){
                const u8 v=static_cast<u8>(c.input.data[c.at++]);if(v=='&'||v=='<'||v>=128||v==0)return false;}
            if(c.at==c.input.bytes)return false;value={c.input.data+first,c.at-first};++c.at;
            u32 bit=0;
            if(equal(attr,"xmlns")&&svg){bit=1;if(!equal(value,"http://www.w3.org/2000/svg"))return false;}
            else if(equal(attr,"viewBox")&&svg){bit=2;if(!equal(value,"0 0 36 36"))return false;}
            else if(equal(attr,"xml:space")&&svg){bit=4;if(!equal(value,"preserve"))return false;}
            else if(equal(attr,"fill")&&!svg){bit=8;if(!color(value,layer.color))return false;}
            else if(equal(attr,"fill-rule")&&(path||group)){bit=16;if(!equal(value,"evenodd"))return false;layer.fill_rule=GlyphFillRule::alternate;}
            else if(equal(attr,"clip-rule")&&(path||group)){bit=32;if(!equal(value,"evenodd"))return false;}
            else if(equal(attr,"d")&&path){bit=64;layer.path=value;if(!value.bytes)return false;}
            else if(equal(attr,"cx")&&(circle||ellipse_tag)){bit=128;if(!scalar(value,layer.cx))return false;}
            else if(equal(attr,"cy")&&(circle||ellipse_tag)){bit=256;if(!scalar(value,layer.cy))return false;}
            else if(equal(attr,"r")&&circle){bit=512;if(!scalar(value,layer.rx)||!(layer.rx>0))return false;layer.ry=layer.rx;}
            else if(equal(attr,"rx")&&ellipse_tag){bit=512;if(!scalar(value,layer.rx)||!(layer.rx>0))return false;}
            else if(equal(attr,"ry")&&ellipse_tag){bit=1024;if(!scalar(value,layer.ry)||!(layer.ry>0))return false;}
            else if(equal(attr,"transform")&&ellipse_tag){bit=2048;
                if(!equal(value,"rotate(-76.714 13.001 18.001)"))return false;layer.rotation=-76.714f;}
            else return false;
            if(seen&bit)return false;seen|=bit;
        }
        if(svg){if(self||(seen&3)!=3)return false;root_seen=true;}
        else if(group){if(self||depth==3)return false;inherited[++depth]=layer;}
        else {
            if(!self||(path&&!(seen&64))||(circle&&(seen&512)==0)||(ellipse_tag&&(seen&1536)!=1536)||
               output->count==kRegionalFlagSvgMaximumLayers)return false;
            output->layers[output->count++]=layer;
        }
    }
}

bool regional_flag_svg_outline(const RegionalFlagSvgLayer& layer, GlyphOutline* output) noexcept {
    const ConstSlice<u8> layer_range{reinterpret_cast<const u8*>(&layer),sizeof(layer)};
    const ConstSlice<u8> result_range{reinterpret_cast<const u8*>(output),sizeof(GlyphOutline)};
    const ConstSlice<u8> path_range{reinterpret_cast<const u8*>(layer.path.data),layer.path.bytes};
    if(!output||!byte_range_valid(layer_range)||!byte_range_valid(result_range)||
        !byte_range_valid(path_range)||layer.path.bytes>kRegionalFlagSvgMaximumBytes||
        byte_ranges_overlap(layer_range,result_range)||byte_ranges_overlap(path_range,result_range)||
        !output->commands||!output->capacity||output->capacity>kRegionalFlagSvgMaximumCommands)return false;
    const ConstSlice<u8> commands_range{reinterpret_cast<const u8*>(output->commands),output->capacity*static_cast<u32>(sizeof(GlyphPathCommand))};
    if(!byte_range_valid(commands_range)||byte_ranges_overlap(commands_range,path_range)||
        byte_ranges_overlap(commands_range,layer_range)||byte_ranges_overlap(commands_range,result_range))return false;
    const float geometry[]{layer.cx,layer.cy,layer.rx,layer.ry};
    for(float value:geometry)if(!(value>=-4096&&value<=4096))return false;
    if((layer.rotation!=0&&layer.rotation!=-76.714f)||
        (layer.fill_rule!=GlyphFillRule::winding&&layer.fill_rule!=GlyphFillRule::alternate))return false;
    auto* commands=output->commands;const auto capacity=output->capacity;*output={commands,capacity};output->fill_rule=layer.fill_rule;
    Path path{output};
    if(layer.path.bytes)return path_outline(layer.path,path)&&output->has_bounds;
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
