#include "maicord/text/truetype_outline.hpp"
#include "maicord/core/memory.hpp"
#include "maicord/text/tt_orientation.hpp"
#include "maicord/text/tt_bold_points.hpp"
#include "maicord/text/tt_outline.hpp"
#include "sfnt_read.hpp"

namespace maicord {
namespace {

using namespace sfnt_read;
#pragma float_control(precise,on,push)
float varied_coordinate(double value) noexcept {
    // gvar coordinates round to integral design units, with positive half
    // ties. Static component transforms keep their 1/64 grid.
    const double shifted=value+.5;const i64 i=static_cast<i64>(shifted);
    return static_cast<float>(i-(static_cast<double>(i)>shifted?1:0));
}
#pragma float_control(pop)

float component_product(float coordinate, float factor) noexcept {
    // The unhinted Windows scaler resolves component coordinates on a 1/64
    // design-unit grid, independent of pixel hinting/ROUND_XY.
    const double value = static_cast<double>(coordinate) * factor;
    const double magnitude = value < 0 ? -value : value;
    const i64 integer = static_cast<i64>(magnitude * 64.0 + 0.5);
    return static_cast<float>(value < 0 ? -integer : integer) / 64.f;
}

u32 location(const SfntTrueType& font, u32 glyph) noexcept { return loca_offset(font.loca, font.loca_stride, glyph); }

struct Reader {
    ConstSlice<u8> data;
    u32 at;
    bool take(u32 count, const u8** out) noexcept {
        if (at > data.count || count > data.count - at) return false;
        *out = data.data + at; at += count; return true;
    }
};

void reset(GlyphOutline* out) noexcept {
    auto* commands = out->commands;
    const u32 capacity = out->capacity;
    reset_value(out);
    out->commands = commands; out->capacity = capacity; out->fill_rule = GlyphFillRule::winding;
}

// Raw design points are kept until composites and point attachments are
// resolved; only the final glyph is normalized.
struct Decoder {
    const SfntTrueType& font;
    SfntOutlineScratch& scratch;
    u32 work;
    const SfntOutlineVariation* variation=nullptr;
    u32 delta_count=0;
    TtOutlineContext* instructions=nullptr;
    bool leaf_origin_set=false;
    float leaf_vertical_offset=0;
    bool bold=false;

    SfntStatus instruct(u16 id,ConstSlice<u8> program,u32 first,u32 first_contour,float* origin,float vertical_offset) noexcept {
        if(!instructions)return program.count&&scratch.require_instructions?SfntStatus::instructions_required:SfntStatus::ok;
        return tt_outline_apply(instructions,id,program,{scratch.points.data+first,scratch.point_count-first},
            {scratch.contour_ends.data+first_contour,scratch.contour_count-first_contour},first,origin,vertical_offset,&work);
    }

    SfntStatus variations(u16 id,u32 first,u32 first_contour,u32 count,bool composite,SfntGvarDelta** output) noexcept {
        if(count+4>variation->deltas.count-delta_count || !variation->deltas.data)return SfntStatus::output_too_small;
        *output=variation->deltas.data+delta_count;u32 used=0;
        const ConstSlice<SfntOutlinePoint> points{composite?nullptr:scratch.points.data+first,count};
        const ConstSlice<u32> ends=composite?ConstSlice<u32>{}:ConstSlice<u32>{scratch.contour_ends.data+first_contour,scratch.contour_count-first_contour};
        const auto s=sfnt_gvar_deltas(*variation->instance,id,points,ends,composite,variation->scratch,{*output,count+4},work,&used,first);work-=used;return s;
    }

    SfntStatus simple(ConstSlice<u8> data, u32 contours,ConstSlice<u8>* program) noexcept {
        reset_value(program);
        if (!contours) {if(data.count==10)return SfntStatus::ok;if(data.count<12)return SfntStatus::truncated;const u32 n=be16(data.data+10);if(n>data.count-12)return SfntStatus::truncated;*program={data.data+12,n};return SfntStatus::ok;}
        if (contours > (data.count - 10u) / 2u) return SfntStatus::truncated;
        const u32 points = be16(data.data + 10 + (contours - 1) * 2) + 1u;
        if (points > scratch.points.count - scratch.point_count ||
            contours > scratch.contour_ends.count - scratch.contour_count)
            return SfntStatus::output_too_small;
        if (points > work) return SfntStatus::resource_limit;
        work -= points;
        const u32 first = scratch.point_count;
        u32 previous = 0;
        for (u32 i = 0; i < contours; ++i) {
            const u32 end = be16(data.data + 10 + i * 2);
            if (i && end <= previous) return SfntStatus::malformed;
            previous = end;
            scratch.contour_ends.data[scratch.contour_count++] = first + end;
        }
        Reader reader{data, 10 + contours * 2};
        const u8* p = nullptr;
        if (!reader.take(2, &p)) return SfntStatus::truncated;
        const u32 instruction_count=be16(p);if (!reader.take(instruction_count, &p)) return SfntStatus::truncated;*program={p,instruction_count};
        for (u32 i = 0; i < points;) {
            if (!reader.take(1, &p)) return SfntStatus::truncated;
            const u8 flags = *p;
            u32 repeat = 1;
            if (flags & 8u) {
                if (!reader.take(1, &p)) return SfntStatus::truncated;
                repeat += *p;
            }
            if (repeat > points - i) return SfntStatus::malformed;
            while (repeat--) scratch.points.data[first + i++] = {0, 0, flags, {0, 0, 0}};
        }
        for (u32 axis = 0; axis < 2; ++axis) {
            i32 coordinate = 0;
            const u8 short_bit = axis ? 4 : 2, same_bit = axis ? 32 : 16;
            for (u32 i = 0; i < points; ++i) {
                auto& point = scratch.points.data[first + i];
                i32 delta = 0;
                if (point.flags & short_bit) {
                    if (!reader.take(1, &p)) return SfntStatus::truncated;
                    delta = point.flags & same_bit ? *p : -static_cast<i32>(*p);
                } else if (!(point.flags & same_bit)) {
                    if (!reader.take(2, &p)) return SfntStatus::truncated;
                    delta = s16(p);
                }
                // At most 65536 signed-16 deltas stay within i32.
                coordinate += delta;
                if (axis) point.y = static_cast<float>(coordinate);
                else point.x = static_cast<float>(coordinate);
            }
        }
        scratch.point_count += points;
        return SfntStatus::ok;
    }

    SfntStatus glyph(u16 id, u32 depth, float* metric_origin,float vertical_offset=0,float* metric_origin_y=nullptr) noexcept {
        struct Restore {u32& count;u32 saved;~Restore() noexcept {count=saved;}} restore{delta_count,delta_count};
        *metric_origin = 0;
        if(metric_origin_y)*metric_origin_y=0;
        if (id >= font.metrics.glyph_count) return SfntStatus::glyph_out_of_range;
        if (depth >= scratch.active_glyphs.count || depth >= 64 || !work)
            return SfntStatus::resource_limit;
        --work;
        for (u32 i = 0; i < depth; ++i)
            if (scratch.active_glyphs.data[i] == id) return SfntStatus::malformed;
        scratch.active_glyphs.data[depth] = id;
        const u32 begin = location(font, id), end = location(font, id + 1u);
        if (begin > end || end > font.glyf.count) return SfntStatus::malformed;
        if (begin == end) return SfntStatus::ok;
        const ConstSlice<u8> data{font.glyf.data + begin, end - begin};
        if (data.count < 10) return SfntStatus::truncated;
        if (!font.bearing_equals_minimum) {
            SfntGlyphMetrics metrics{};
            const auto status = sfnt_horizontal_metrics(font.metrics, id, &metrics);
            if (status != SfntStatus::ok) return status;
            *metric_origin = static_cast<float>(metrics.bearing - s16(data.data + 2));
        }
        const i16 contours = s16(data.data);
        if (contours >= 0) {
            ConstSlice<u8> program{};const u32 first=scratch.point_count,first_contour=scratch.contour_count;auto status=simple(data,static_cast<u32>(contours),&program);
            if(status!=SfntStatus::ok)return status;
            if(bold){u32 used=0;status=tt_mark_contour_directions({scratch.points.data,scratch.point_count},{scratch.contour_ends.data+first_contour,scratch.contour_count-first_contour},first,work,&used);work-=used;if(status!=SfntStatus::ok)return status;}
            if(variation){const u32 count=scratch.point_count-first;SfntGvarDelta* deltas=nullptr;
            status=variations(id,first,first_contour,count,false,&deltas);if(status!=SfntStatus::ok)return status;
            if(count>work)return SfntStatus::resource_limit;work-=count;
            for(u32 i=0;i<count;++i){auto& point=scratch.points[first+i];point.x=varied_coordinate(point.x+deltas[i].x);point.y=varied_coordinate(point.y+deltas[i].y);}
            *metric_origin-=varied_coordinate(deltas[count].x);}
            // In the normalized-outline profile, simple component
            // programs share the first leaf's vertical phantom origin for this
            // root decode. Parent programs use their own translated origin.
            if(!leaf_origin_set){leaf_vertical_offset=vertical_offset;leaf_origin_set=true;}
            return instruct(id,program,first,first_contour,metric_origin,leaf_vertical_offset);
        }
        const u32 first = scratch.point_count;
        const u32 first_contour=scratch.contour_count;
        SfntGvarDelta* component_deltas=nullptr;u32 component_count=0,component_index=0;
        if(variation){Reader scan{data,10};u16 f=0;do{if(!work)return SfntStatus::resource_limit;--work;const u8* p=nullptr;if(!scan.take(4,&p))return SfntStatus::truncated;f=be16(p);
                const u32 words=composite_transform_words(f);if(words==0xff)return SfntStatus::malformed;
                if(!scan.take((f&1?4u:2u)+words*2,&p))return SfntStatus::truncated;++component_count;
            }while(f&0x20);
            auto status=variations(id,0,0,component_count,true,&component_deltas);if(status!=SfntStatus::ok)return status;
            delta_count+=component_count+4;*metric_origin-=varied_coordinate(component_deltas[component_count].x);
        }
        Reader reader{data, 10};
        u16 flags = 0;
        bool has_instructions = false;
        do {
            const u8* p = nullptr;
            if (!reader.take(4, &p)) return SfntStatus::truncated;
            flags = be16(p);
            const u16 child = be16(p + 2);
            has_instructions = has_instructions || (flags & 0x100u);
            const bool words = (flags & 1u) != 0, xy = (flags & 2u) != 0;
            if (!reader.take(words ? 4u : 2u, &p)) return SfntStatus::truncated;
            const i32 a = words ? (xy ? s16(p) : be16(p))
                                : (xy ? static_cast<signed char>(p[0]) : p[0]);
            const i32 b = words ? (xy ? s16(p + 2) : be16(p + 2))
                                : (xy ? static_cast<signed char>(p[1]) : p[1]);
            float xx = 1, xy_matrix = 0, yx = 0, yy = 1;
            const u32 values = composite_transform_words(flags);
            if (values == 0xff) return SfntStatus::malformed;
            if (values) {
                if (!reader.take(values * 2, &p)) return SfntStatus::truncated;
                xx = s16(p) / 16384.f;
                yy = values == 1 ? xx : s16(p + (values == 2 ? 2 : 6)) / 16384.f;
                if (values == 4) { xy_matrix = s16(p + 2) / 16384.f; yx = s16(p + 4) / 16384.f; }
            }
            const u32 child_first = scratch.point_count;
            const u32 child_first_contour=scratch.contour_count;
            float child_origin = 0,child_origin_y=0;
            // Transformed/point-attached instruction contexts need their own
            // Windows scaler transform; reject rather than apply root vectors.
            if(instructions && !(instructions->instance->graphics.instruction_control&1) && (values || !xy))return SfntStatus::unsupported;
            auto status = glyph(child, depth + 1, &child_origin,vertical_offset+(xy?static_cast<float>(b):0),&child_origin_y);
            if (status != SfntStatus::ok) return status;
            if(bold){const i64 determinant=static_cast<i64>(xx*16384)*static_cast<i64>(yy*16384)-static_cast<i64>(xy_matrix*16384)*static_cast<i64>(yx*16384);
                if(!determinant)return SfntStatus::unsupported;
                if(determinant<0){const u32 count=scratch.contour_count-child_first_contour;if(count>work)return SfntStatus::resource_limit;work-=count;
                    u32 first_point=child_first;for(u32 c=child_first_contour;c<scratch.contour_count;++c){scratch.points[first_point].reserved[0]^=1;first_point=scratch.contour_ends[c]+1;}}
            }
            if(flags&0x200u){*metric_origin=variation?component_product(child_origin,xx)+component_product(child_origin_y,yx):child_origin;
                if(variation&&metric_origin_y)*metric_origin_y=component_product(child_origin,xy_matrix)+component_product(child_origin_y,yy);}
            const u32 child_points = scratch.point_count - child_first;
            if (child_points > work / 2u) return SfntStatus::resource_limit;
            work -= child_points * 2u;
            // Child transforms are resolved before parent point attachments.
            for (u32 i = child_first; i < scratch.point_count; ++i) {
                auto& point = scratch.points.data[i];
                const float x = point.x, y = point.y;
                point.x = component_product(x, xx) + component_product(y, yx);
                point.y = component_product(x, xy_matrix) + component_product(y, yy);
            }
            float dx = static_cast<float>(a), dy = static_cast<float>(b);
            if(xy && variation){dx=varied_coordinate(dx+component_deltas[component_index].x);dy=varied_coordinate(dy+component_deltas[component_index].y);}
            if (!xy) {
                const u32 parent_count = child_first - first;
                const u32 child_count = scratch.point_count - child_first;
                if (static_cast<u32>(a) >= parent_count || static_cast<u32>(b) >= child_count) {
                    // The four phantom points are valid references but need phantom
                    // metrics this decoder lacks: unsupported, not corrupt data.
                    return static_cast<u64>(a) < static_cast<u64>(parent_count) + 4u &&
                           static_cast<u64>(b) < static_cast<u64>(child_count) + 4u
                               ? SfntStatus::unsupported : SfntStatus::malformed;
                }
                dx = scratch.points.data[first + a].x - scratch.points.data[child_first + b].x;
                dy = scratch.points.data[first + a].y - scratch.points.data[child_first + b].y;
            } else if ((flags & 0x1800u) == 0x800u) {
                // Match the Windows scaler's legacy offset scale: magnitude
                // of the largest coefficient for each input axis. The offset
                // is not rotated/reflected as an ordinary point would be.
                const float x_scale = maximum(xx < 0 ? -xx : xx, xy_matrix < 0 ? -xy_matrix : xy_matrix);
                const float y_scale = maximum(yx < 0 ? -yx : yx, yy < 0 ? -yy : yy);
                dx = component_product(dx, x_scale);
                dy = component_product(dy, y_scale);
            }
            // ROUND_XY_TO_GRID applies after scaling/grid fitting, not to an
            // unhinted design outline. The default component offset is unscaled.
            for (u32 i = child_first; i < scratch.point_count; ++i) {
                auto& point = scratch.points.data[i]; point.x += dx; point.y += dy;
                if (!(point.x >= -1.e12f && point.x <= 1.e12f &&
                      point.y >= -1.e12f && point.y <= 1.e12f)) return SfntStatus::malformed;
            }
            ++component_index;
        } while (flags & 0x20u);
        if (has_instructions) {
            const u8* p = nullptr;
            if (!reader.take(2, &p))return SfntStatus::truncated;const u32 n=be16(p);if(!reader.take(n,&p))return SfntStatus::truncated;
            return instruct(id,{p,n},first,first_contour,metric_origin,vertical_offset);
        }
        return SfntStatus::ok;
    }
};

// Appends commands and, when asked, grows the bounds over every emitted point.
struct Writer {
    GlyphOutline& out;
    bool bounds;
    __forceinline void include(SfntOutlinePoint p) noexcept {
        if (!out.has_bounds) { out.minimum_x = out.maximum_x = p.x; out.minimum_y = out.maximum_y = p.y; out.has_bounds = true; return; }
        out.minimum_x = minimum(out.minimum_x, p.x); out.maximum_x = maximum(out.maximum_x, p.x);
        out.minimum_y = minimum(out.minimum_y, p.y); out.maximum_y = maximum(out.maximum_y, p.y);
    }
    __forceinline bool append(GlyphPathCommandKind kind, SfntOutlinePoint a = {},
                  SfntOutlinePoint b = {}, SfntOutlinePoint c = {}) noexcept {
        if (out.count == out.capacity) return false;
        out.commands[out.count++] = {a.x, a.y, b.x, b.y, c.x, c.y, kind, {0, 0, 0}};
        if (bounds && kind != GlyphPathCommandKind::end_closed) {
            include(a);
            if (kind == GlyphPathCommandKind::cubic) { include(b); include(c); }
        }
        return true;
    }
};

#pragma float_control(precise, on, push)
SfntOutlinePoint middle(SfntOutlinePoint a, SfntOutlinePoint b) noexcept {
    return {(a.x + b.x) * .5f, (a.y + b.y) * .5f, 1, {0, 0, 0}};
}

SfntOutlinePoint normalized(SfntOutlinePoint point, float origin, float scale) noexcept {
    point.x = (point.x + origin) * scale;
    point.y = point.y == 0.f ? 0.f : -point.y * scale;
    return point;
}

SfntStatus emit(const SfntOutlineScratch& scratch, GlyphOutline& output, float origin, float scale,bool color,bool bounds) noexcept {
    Writer writer{output,bounds};
    u32 first = 0;
    for (u32 contour = 0; contour < scratch.contour_count; ++contour) {
        const u32 last = scratch.contour_ends.data[contour];
        // DirectWrite's outline sink omits isolated single-point contours.
        if (!color && last == first) { first = last + 1; continue; }
        u32 i = first, limit = last + 1;
        const auto* points = scratch.points.data;
        SfntOutlinePoint raw_start{};
        if (points[first].flags & 1u) raw_start = points[i++];
        else if (points[last].flags & 1u) { raw_start = points[last]; --limit; }
        else raw_start = middle(points[first], points[last]);
        const auto start = color?raw_start:normalized(raw_start, origin, scale);
        if (!writer.append(GlyphPathCommandKind::begin_filled, start)) return SfntStatus::output_too_small;
        auto current = start;
        while (i < limit) {
            const auto point = color?points[i]:normalized(points[i], origin, scale);
            if (point.flags & 1u) {
                if (!writer.append(GlyphPathCommandKind::line, point)) return SfntStatus::output_too_small;
                current = point; ++i;
            } else {
                const auto next = i + 1 < limit ? points[i + 1] : raw_start;
                const bool on = (next.flags & 1u) != 0;
                const auto raw_target=on?next:middle(points[i],next);
                const auto target = color?raw_target:normalized(raw_target, origin, scale);
                constexpr float third = 1.f / 3.f;
                // The color path uses 2/3 as a folded constant (as /fp:fast folds
                // (1 - 1/3)); text needs the precise subtraction used by the
                // Windows outline sink.
                const float two_thirds = color?2.f/3.f:1.f-third;
                const SfntOutlinePoint a{current.x * third + point.x * two_thirds,
                                         current.y * third + point.y * two_thirds, 0, {0, 0, 0}};
                const SfntOutlinePoint b{target.x * third + point.x * two_thirds,
                                         target.y * third + point.y * two_thirds, 0, {0, 0, 0}};
                if (!writer.append(GlyphPathCommandKind::cubic, a, b, target)) return SfntStatus::output_too_small;
                current = target; i += on ? 2u : 1u;
            }
        }
        if (!writer.append(GlyphPathCommandKind::end_closed)) return SfntStatus::output_too_small;
        first = last + 1;
    }
    return SfntStatus::ok;
}
#pragma float_control(pop)

} // namespace

u32 sfnt_truetype_outline_capacity(const SfntTrueType& font,u16 glyph) noexcept {
    if(glyph>=font.metrics.glyph_count||!font.loca.data||!font.glyf.data||
       (font.loca_stride!=2&&font.loca_stride!=4)||font.loca.count<(static_cast<u32>(glyph)+2)*font.loca_stride)return 0;
    const u32 begin=location(font,glyph),end=location(font,glyph+1u);
    if(begin>end||end>font.glyf.count)return 0;
    if(begin==end)return 1;
    if(end-begin<10)return 0;
    const auto* data=font.glyf.data+begin;const i32 contours=s16(data);
    if(contours<0)return 0;
    if(!contours)return 1;
    if(static_cast<u32>(contours)>(end-begin-10)/2)return 0;
    // Each loop iteration consumes at least one explicit point and emits at
    // most one line/cubic, plus begin/end commands for each contour.
    return static_cast<u32>(be16(data+10+(contours-1)*2))+1+static_cast<u32>(contours)*2;
}

SfntStatus sfnt_truetype_open(const SfntFace& face, SfntTrueType* font,bool allow_variations) noexcept {
    if (!font) return SfntStatus::invalid_input;
    reset_value(font);
    if (face.version == sfnt_tag('O','T','T','O')) return SfntStatus::unsupported;
    ConstSlice<u8> gvar{};
    const auto variation = sfnt_table(face, sfnt_tag('g','v','a','r'), &gvar);
    if (variation == SfntStatus::ok && !allow_variations) return SfntStatus::unsupported;
    if (variation != SfntStatus::missing_table && variation!=SfntStatus::ok) return variation;
    SfntTrueType result{};
    result.gvar=gvar;
    auto status = sfnt_horizontal_metrics_open(face, &result.metrics);
    if (status != SfntStatus::ok) return status;
    status = sfnt_table(face, sfnt_tag('l','o','c','a'), &result.loca);
    if (status != SfntStatus::ok) return status;
    status = sfnt_table(face, sfnt_tag('g','l','y','f'), &result.glyf);
    if (status != SfntStatus::ok) return status;
    ConstSlice<u8> head{};
    status = sfnt_table(face, sfnt_tag('h','e','a','d'), &head);
    if (status != SfntStatus::ok) return status;
    const u16 format = be16(head.data + 50);
    if (format > 1 || be16(head.data + 52)) return SfntStatus::unsupported;
    result.loca_stride = format ? 4 : 2;
    result.bearing_equals_minimum = (be16(head.data + 16) & 2u) != 0;
    const u32 count = static_cast<u32>(result.metrics.glyph_count) + 1u;
    if (count > result.loca.count / result.loca_stride) return SfntStatus::truncated;
    *font = result;
    return SfntStatus::ok;
}

SfntStatus sfnt_truetype_has_instructions(const SfntTrueType& font,bool* present,u32 work,u32* used) noexcept {
    WorkReport report{work,work,used};
    if(!present)return SfntStatus::invalid_input;*present=false;if(!font.loca.data||!font.glyf.data||(font.loca_stride!=2&&font.loca_stride!=4)||font.loca.count/font.loca_stride<=font.metrics.glyph_count)return SfntStatus::invalid_input;
    for(u32 glyph=0;glyph<font.metrics.glyph_count;++glyph){if(!work)return SfntStatus::resource_limit;--work;const u32 begin=location(font,glyph),end=location(font,glyph+1);
        if(begin>end||end>font.glyf.count)return SfntStatus::malformed;if(begin==end)continue;const u32 size=end-begin;if(size<10)return SfntStatus::truncated;
        const auto* data=font.glyf.data+begin;const i16 contours=s16(data);u32 at=10;
        if(contours>=0){if(!contours&&size==10)continue;at+=static_cast<u32>(contours)*2;}
        else{bool instructions=false;u16 flags=0;do{if(!work)return SfntStatus::resource_limit;--work;if(at>size||size-at<4)return SfntStatus::truncated;
                flags=be16(data+at);at+=4;instructions|=(flags&0x100)!=0;const u32 words=composite_transform_words(flags);
                if(words==0xff)return SfntStatus::malformed;const u32 bytes=(flags&1?4u:2u)+words*2;if(bytes>size-at)return SfntStatus::truncated;at+=bytes;
            }while(flags&0x20);if(!instructions)continue;}
        if(at>size||size-at<2)return SfntStatus::truncated;const u32 count=be16(data+at);if(count>size-at-2)return SfntStatus::truncated;
        if(count){*present=true;return SfntStatus::ok;}
    }return SfntStatus::ok;
}
SfntStatus sfnt_truetype_outline(const SfntTrueType& font, u16 glyph,
                                  SfntOutlineScratch* scratch, GlyphOutline* output,SfntOutlineMode mode,const SfntOutlineVariation* variation,TtOutlineContext* instructions,bool oblique,bool bold) noexcept {
    if (!output) return SfntStatus::invalid_input;
    reset(output);
    if (!scratch || !scratch->points.data || !scratch->contour_ends.data || !scratch->active_glyphs.data ||
        !output->commands || !font.loca.data || !font.glyf.data || !font.metrics.units_per_em)
        return SfntStatus::invalid_input;
    scratch->point_count = scratch->contour_count = 0;
    if(bold&&(mode!=SfntOutlineMode::text||variation))return SfntStatus::unsupported;
    if(font.gvar.data && (!variation || !variation->instance))return SfntStatus::unsupported;
    if(variation && (!variation->instance || variation->instance->font.table.data!=font.gvar.data || variation->instance->font.table.count!=font.gvar.count || variation->instance->font.glyph_count!=font.metrics.glyph_count))return SfntStatus::invalid_input;
    Decoder decoder{font, *scratch, scratch->work_limit,variation,0,instructions};
    decoder.bold=bold;
    float origin = 0,origin_y=0;
    auto status=instructions?tt_outline_begin(instructions,font,&decoder.work):SfntStatus::ok;
    if(status==SfntStatus::ok)status=decoder.glyph(glyph,0,&origin,0,&origin_y);
    if(status==SfntStatus::ok&&origin_y){
        if(scratch->point_count>decoder.work)status=SfntStatus::resource_limit;
        else{decoder.work-=scratch->point_count;for(u32 i=0;i<scratch->point_count;++i)scratch->points[i].y+=origin_y;}
    }
    if(status==SfntStatus::ok&&bold){u32 used=0;
        status=tt_bold_marked_points({scratch->points.data,scratch->point_count},{scratch->contour_ends.data,scratch->contour_count},font.metrics.units_per_em,
            instructions&&instructions->instance->prep_recovered_missing_function,decoder.work,&used);decoder.work-=used;
    }
    if(status==SfntStatus::ok&&oblique){
        if(mode!=SfntOutlineMode::text)status=SfntStatus::unsupported;
        else if(scratch->point_count>decoder.work)status=SfntStatus::resource_limit;
        // Match the Windows scaler's 1/64-unit transform, with half ties away
        // from zero, after attachments/instructions and before implied points.
        else {decoder.work-=scratch->point_count;
            for(u32 i=0;i<scratch->point_count;++i)scratch->points[i].x+=component_product(scratch->points[i].y,87.f/256.f);}
    }
    if (status == SfntStatus::ok && scratch->point_count) {
        const float scale = 1.f / font.metrics.units_per_em;
        // The color path computes implied points after scaling and keeps isolated
        // contours. It applies only to simple glyphs within 2048 points and
        // contours; composites use the text order.
        const u32 begin=location(font,glyph),end=location(font,glyph+1);
        const bool color=(mode==SfntOutlineMode::colr||mode==SfntOutlineMode::colr_commands) && end-begin>=10 && s16(font.glyf.data+begin)>=0 &&
            scratch->point_count<=2048 && scratch->contour_count<=2048;
        if(color)for(u32 i=0;i<scratch->point_count;++i)scratch->points[i]=normalized(scratch->points[i],origin,scale);
        status = emit(*scratch, *output, origin, scale,color,mode!=SfntOutlineMode::colr_commands);
    }
    if (status != SfntStatus::ok) {
        reset(output); output->rejected = 1;
        scratch->point_count = scratch->contour_count = 0;
    }
    return status;
}

} // namespace maicord
