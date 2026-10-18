#include "cff_internal.hpp"
#include "maicord/text/base.hpp"
#include "maicord/sys/simd.hpp"

namespace maicord {
namespace {

#pragma float_control(precise, on, push)
bool bounded(double value) noexcept { return value >= -1.e12 && value <= 1.e12; }
bool integer(double value, i32* out) noexcept {
    if (!(value >= -2147483648.0 && value <= 2147483647.0)) return false;
    *out = static_cast<i32>(value); return *out == value;
}
float positive_zero(float value) noexcept {
    const __m128 packed=_mm_set_ss(value);
    return _mm_cvtss_f32(_mm_andnot_ps(_mm_cmpeq_ss(packed,_mm_setzero_ps()),packed));
}

#include "cff_bold.inc"

template<bool Bold> struct VirtualMachine {
    const CffFont& font;
    CffIndex local_subrs;
    GlyphOutline& out;
    u32& work;
    CffGlyphMetrics* metrics;
    double matrix[6]{};
    double design_scale = 0;
    float em_scale = 0;
    double stack[48]{};
    double transient[32]{};
    double x = 0, y = 0, translate_x = 0, translate_y = 0;
    double accent_x = 0, accent_y = 0;
    double advance = 0, nominal_width = 0;
    u32 count = 0, stems = 0, random = 1;
    u16 base_code = 0, accent_code = 0;
    bool open = false, drawn = false, ever_drawn = false;
    bool width_seen = false, ended = false, composite = false;
    bool flex_points = false;
    double oblique_scale = 0;
    static constexpr bool bold=Bold;

    bool flex_coordinate(double value, double* result) const noexcept {
        // Windows' unhinted flex path round-trips output controls through a 16.16
        // inverse transform; ordinary cubics do not. The charstring current point
        // stays unchanged, otherwise error drifts between flexes.
        *result = value;
        if (!design_scale) return true;
        const double scaled = value * design_scale;
        if (!(scaled >= -9.e18 && scaled <= 9.e18)) return false;
        const double inverse_scale = static_cast<double>(static_cast<i64>(4294967296.0 / design_scale + .5));
        const double transformed = static_cast<double>(static_cast<i64>(scaled + (scaled < 0 ? -.5 : .5)));
        const double restored = transformed * inverse_scale / 65536.0;
        if (!(restored >= -9.e18 && restored <= 9.e18)) return false;
        *result = static_cast<i64>(restored + (restored < 0 ? -.5 : .5)) / 65536.0;
        return true;
    }
    // Flex round-trips through the complete fixed-point transform and inverse.
    // The shear couples x to y; independent scalar inverses lose low bits at
    // nonintegral FontMatrix scales. Products round before they are added.
    bool flex_oblique(double* a,double* b) const noexcept {
        const auto rounded=[](double value) noexcept {return static_cast<double>(static_cast<i64>(value+(value<0?-.5:.5)));};
        const double ax=*a*design_scale,by=*b*design_scale,skew=*b*oblique_scale;
        if(!(ax>=-9.e18&&ax<=9.e18&&by>=-9.e18&&by<=9.e18&&skew>=-9.e18&&skew<=9.e18))return false;
        const double transformed_x=rounded(ax)+rounded(skew),transformed_y=rounded(by);
        const double inverse=rounded(4294967296.0/design_scale);
        const double inverse_skew=rounded(-oblique_scale*4294967296.0/(design_scale*design_scale));
        const double rx=transformed_x*inverse/65536.0,ry=transformed_y*inverse/65536.0,rs=transformed_y*inverse_skew/65536.0;
        if(!(rx>=-9.e18&&rx<=9.e18&&ry>=-9.e18&&ry<=9.e18&&rs>=-9.e18&&rs<=9.e18))return false;
        *a=(rounded(rx)+rounded(rs))/65536.0;*b=rounded(ry)/65536.0;return true;
    }

    bool append(GlyphPathCommandKind kind, double x1 = 0, double y1 = 0,
                  double x2 = 0, double y2 = 0, double x3 = 0, double y3 = 0) noexcept {
        if (out.count == out.capacity) return false;
        GlyphPathCommand command{}; command.kind = kind;
        const double points[]{x1,y1,x2,y2,x3,y3};
        float coordinates[6]{};
        const u32 size = kind == GlyphPathCommandKind::cubic ? 3u :
                         kind == GlyphPathCommandKind::end_closed ? 0u : 1u;
        for (u32 i = 0; i < size; ++i) {
            double a = points[i*2] + translate_x, b = points[i*2+1] + translate_y;
            double metric_a=a,metric_b=b;
            if (flex_points && kind == GlyphPathCommandKind::cubic){
                if(metrics&&oblique_scale&&(!flex_coordinate(a,&metric_a)||!flex_coordinate(b,&metric_b)))return false;
                if(oblique_scale){if(!flex_oblique(&a,&b))return false;}
                else if(!flex_coordinate(a,&a)||!flex_coordinate(b,&b))return false;
            }
            float tx = static_cast<float>(a) * static_cast<float>(matrix[0]) +
                             static_cast<float>(b) * static_cast<float>(matrix[2]) + static_cast<float>(matrix[4]);
            float ty = -(static_cast<float>(a) * static_cast<float>(matrix[1]) +
                              static_cast<float>(b) * static_cast<float>(matrix[3]) + static_cast<float>(matrix[5]));
            double design_x = 0, design_y = 0;
            CffBoldPoint staged{};
            if (design_scale) {
                const double ax=a*design_scale, by=b*design_scale;
                if (!(ax>=-9.e18 && ax<=9.e18 && by>=-9.e18 && by<=9.e18)) return false;
                double rx=static_cast<i64>(ax+(ax<0?-.5:.5))/65536.0;
                const double ry=static_cast<i64>(by+(by<0?-.5:.5))/65536.0;
                design_x=rx; design_y=ry;
                if(metrics&&oblique_scale&&flex_points&&kind==GlyphPathCommandKind::cubic){
                    const double mx=metric_a*design_scale,my=metric_b*design_scale;
                    if(!(mx>=-9.e18&&mx<=9.e18&&my>=-9.e18&&my<=9.e18))return false;
                    design_x=static_cast<i64>(mx+(mx<0?-.5:.5))/65536.0;design_y=static_cast<i64>(my+(my<0?-.5:.5))/65536.0;
                }
                if(oblique_scale){const double shift=b*oblique_scale;if(!(shift>=-9.e18&&shift<=9.e18))return false;rx+=static_cast<i64>(shift+(shift<0?-.5:.5))/65536.0;}
                if(bold){
                    const double fx=rx*65536.0,fy=ry*65536.0;
                    if(!(fx>=-2147483648.0&&fx<=2147483647.0&&fy>=-2147483648.0&&fy<=2147483647.0))return false;
                    staged={static_cast<i32>(fx),static_cast<i32>(fy)};
                }
                tx=static_cast<float>(rx)*em_scale;
                ty=-static_cast<float>(ry)*em_scale;
            } else if (metrics) {
                design_x=(a*matrix[0]+b*matrix[2]+matrix[4])*font.units_per_em;
                design_y=(a*matrix[1]+b*matrix[3]+matrix[5])*font.units_per_em;
            }
            if (!bounded(tx) || !bounded(ty)) return false;
            const float px = positive_zero(tx);
            const float py = positive_zero(ty);
            if(bold)copy_bytes(coordinates+i*2,&staged,sizeof(staged));
            else{coordinates[i*2] = px; coordinates[i*2+1] = py;}
            if (metrics) {
                if (!bounded(design_x) || !bounded(design_y)) return false;
                if (!metrics->has_bounds) {
                    metrics->minimum_x=metrics->maximum_x=design_x;
                    metrics->minimum_y=metrics->maximum_y=design_y;
                    metrics->has_bounds=true;
                } else {
                    metrics->minimum_x=minimum(metrics->minimum_x,design_x);
                    metrics->minimum_y=minimum(metrics->minimum_y,design_y);
                    metrics->maximum_x=maximum(metrics->maximum_x,design_x);
                    metrics->maximum_y=maximum(metrics->maximum_y,design_y);
                }
            }
            if(bold)continue;
            if (!out.has_bounds) {
                out.minimum_x = out.maximum_x = px; out.minimum_y = out.maximum_y = py;
                out.has_bounds = true;
            } else {
                out.minimum_x = minimum(out.minimum_x, px); out.maximum_x = maximum(out.maximum_x, px);
                out.minimum_y = minimum(out.minimum_y, py); out.maximum_y = maximum(out.maximum_y, py);
            }
        }
        command.x1=coordinates[0]; command.y1=coordinates[1]; command.x2=coordinates[2];
        command.y2=coordinates[3]; command.x3=coordinates[4]; command.y3=coordinates[5];
        out.commands[out.count++] = command;
        return true;
    }

    bool close() noexcept {
        if (!open) return true;
        open = false;
        return !drawn || append(GlyphPathCommandKind::end_closed);
    }
    bool begin() noexcept {
        if (!open) return false;
        if (drawn) return true;
        drawn = true; ever_drawn = true;
        return append(GlyphPathCommandKind::begin_filled, x, y);
    }
    bool move(double dx, double dy) noexcept {
        if (!close()) return false;
        x += dx; y += dy; open = true; drawn = false;
        return bounded(x) && bounded(y);
    }
    bool line(double dx, double dy) noexcept {
        if (!begin()) return false;
        x += dx; y += dy;
        return bounded(x) && bounded(y) && append(GlyphPathCommandKind::line, x, y);
    }
    bool curve(double dx1, double dy1, double dx2, double dy2, double dx3, double dy3) noexcept {
        if (!begin()) return false;
        const double ax = x + dx1, ay = y + dy1, bx = ax + dx2, by = ay + dy2;
        x = bx + dx3; y = by + dy3;
        return bounded(x) && bounded(y) && append(GlyphPathCommandKind::cubic, ax, ay, bx, by, x, y);
    }
    bool push(double value) noexcept {
        if (count == 48 || !bounded(value)) return false;
        stack[count++] = value; return true;
    }
    bool width(u32 required) noexcept {
        if (!width_seen && count == required + 1) {
            advance = nominal_width + stack[0];
            if (!bounded(advance)) return false;
            for (u32 i = 1; i < count; ++i) stack[i-1] = stack[i];
            --count;
        }
        width_seen = true;
        return count == required;
    }
    SfntStatus geometry_error() const noexcept {
        return out.count == out.capacity ? SfntStatus::output_too_small : SfntStatus::malformed;
    }

    SfntStatus arithmetic(u8 op) noexcept {
        double a = 0, b = 0;
        i32 index = 0;
        if (op == 0) return count ? SfntStatus::malformed : SfntStatus::ok; // deprecated dotsection
        if (op == 23) {
            random ^= random << 13; random ^= random >> 17; random ^= random << 5;
            return push((static_cast<double>(random) + 1) / 4294967296.0) ? SfntStatus::ok : SfntStatus::malformed;
        }
        if (!count) return SfntStatus::malformed;
        switch (op) {
        case 5: stack[count-1] = stack[count-1] == 0 ? 1 : 0; break;
        case 9: stack[count-1] = absolute_value<double>(stack[count-1]); break;
        case 14: stack[count-1] = -stack[count-1]; break;
        case 18: --count; break;
        case 26:
            if (stack[count-1] < 0) return SfntStatus::malformed;
            stack[count-1] = _mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(stack[count-1]))); break;
        case 27: if (!push(stack[count-1])) return SfntStatus::malformed; break;
        case 21:
            if (!integer(stack[count-1], &index) || index < 0 || index >= 32) return SfntStatus::malformed;
            stack[count-1] = transient[index]; break;
        case 29:
            if (!integer(stack[--count], &index)) return SfntStatus::malformed;
            if (index < 0) index = 0;
            if (static_cast<u32>(index) >= count || !push(stack[count-1-index])) return SfntStatus::malformed;
            break;
        case 22:
            if (count < 4) return SfntStatus::malformed;
            a = stack[count-2] <= stack[count-1] ? stack[count-4] : stack[count-3];
            count -= 4; push(a); break;
        case 30: {
            if (count < 2) return SfntStatus::malformed;
            i32 size = 0, shift = 0;
            if (!integer(stack[count-2], &size) || !integer(stack[count-1], &shift) || size < 0 ||
                static_cast<u32>(size) > count-2) return SfntStatus::malformed;
            count -= 2;
            if (size) {
                shift %= size; if (shift < 0) shift += size;
                double temporary[48];
                for (i32 i = 0; i < size; ++i) temporary[(i+shift)%size] = stack[count-size+i];
                for (i32 i = 0; i < size; ++i) stack[count-size+i] = temporary[i];
            }
            break;
        }
        default:
            if (count < 2) return SfntStatus::malformed;
            b = stack[count-1]; a = stack[count-2];
            switch (op) {
            case 3: a = a != 0 && b != 0 ? 1 : 0; break;
            case 4: a = a != 0 || b != 0 ? 1 : 0; break;
            case 10: a += b; break;
            case 11: a -= b; break;
            case 12: if (b == 0) return SfntStatus::malformed; a /= b; break;
            case 15: a = a == b ? 1 : 0; break;
            case 20:
                if (!integer(b, &index) || index < 0 || index >= 32) return SfntStatus::malformed;
                transient[index] = a; count -= 2; return SfntStatus::ok;
            case 24: a *= b; break;
            case 28: stack[count-1] = a; stack[count-2] = b; return SfntStatus::ok;
            default: return SfntStatus::unsupported;
            }
            if (!bounded(a)) return SfntStatus::malformed;
            --count; stack[count-1] = a;
        }
        return SfntStatus::ok;
    }

    SfntStatus flex(u8 op) noexcept {
        const double* s = stack;
        bool ok = false;
        flex_points = true;
        if (op == 34 && count == 7) {
            ok = curve(s[0],0,s[1],s[2],s[3],0) && curve(s[4],0,s[5],-s[2],s[6],0);
        } else if (op == 35 && count == 13) {
            ok = curve(s[0],s[1],s[2],s[3],s[4],s[5]) && curve(s[6],s[7],s[8],s[9],s[10],s[11]);
        } else if (op == 36 && count == 9) {
            ok = curve(s[0],s[1],s[2],s[3],s[4],0) && curve(s[5],0,s[6],s[7],s[8],-(s[1]+s[3]+s[7]));
        } else if (op == 37 && count == 11) {
            const double dx = s[0]+s[2]+s[4]+s[6]+s[8], dy = s[1]+s[3]+s[5]+s[7]+s[9];
            ok = curve(s[0],s[1],s[2],s[3],s[4],s[5]) &&
                 curve(s[6],s[7],s[8],s[9],absolute_value<double>(dx)>absolute_value<double>(dy)?s[10]:-dx,absolute_value<double>(dx)>absolute_value<double>(dy)?-dy:s[10]);
        } else return SfntStatus::malformed;
        flex_points = false;
        count = 0;
        return ok ? SfntStatus::ok : geometry_error();
    }

    SfntStatus run(ConstSlice<u8> program, u32 depth) noexcept {
        if (depth > 10) return SfntStatus::resource_limit;
        u32 at = 0;
        while (at < program.count && !ended) {
            if (!work) return SfntStatus::resource_limit;
            --work;
            const u8 byte = program.data[at++];
            if (byte >= 32 || byte == 28) {
                double value = 0;
                if (byte <= 246 && byte != 28) value = static_cast<i32>(byte)-139;
                else if (byte >= 247 && byte <= 254) {
                    if (at == program.count) return SfntStatus::truncated;
                    const u8 next = program.data[at++];
                    value = byte <= 250 ? (byte-247)*256+next+108 : -(byte-251)*256-next-108;
                } else {
                    const u32 size = byte == 28 ? 2u : 4u;
                    if (size > program.count-at) return SfntStatus::truncated;
                    value = byte == 28 ? static_cast<i16>(byte_load_be16(program.data+at))
                                      : static_cast<i32>(byte_load_be32(program.data+at))/65536.0;
                    at += size;
                }
                if (!push(value)) return SfntStatus::malformed;
                continue;
            }
            if (byte == 12) {
                if (at == program.count) return SfntStatus::truncated;
                const u8 op = program.data[at++];
                const auto status = op >= 34 && op <= 37 ? flex(op) : arithmetic(op);
                if (status != SfntStatus::ok) return status;
                continue;
            }
            if (byte == 10 || byte == 29) {
                i32 index = 0;
                if (!count || !integer(stack[--count], &index)) return SfntStatus::malformed;
                const auto& subrs = byte == 10 ? local_subrs : font.global_subrs;
                const i64 actual = static_cast<i64>(index) + (subrs.count < 1240 ? 107 : subrs.count < 33900 ? 1131 : 32768);
                if (actual < 0 || static_cast<u64>(actual) >= subrs.count) return SfntStatus::malformed;
                const auto status = run(cff_detail::index_object(subrs, static_cast<u32>(actual)), depth+1);
                if (status != SfntStatus::ok) return status;
                continue;
            }
            if (byte == 11) return depth ? SfntStatus::ok : SfntStatus::malformed;
            if (byte == 14) {
                if (!width_seen && (count == 1 || count == 5)) {
                    advance=nominal_width+stack[0];
                    if (!bounded(advance)) return SfntStatus::malformed;
                    for (u32 i=1;i<count;++i) stack[i-1]=stack[i]; --count;
                }
                if (count == 4) {
                    i32 base = 0, accent = 0;
                    if (font.cid || ever_drawn || !integer(stack[2],&base) || !integer(stack[3],&accent) ||
                        base < 0 || base > 255 || accent < 0 || accent > 255) return SfntStatus::malformed;
                    composite = true; accent_x=stack[0]; accent_y=stack[1];
                    base_code=static_cast<u16>(base); accent_code=static_cast<u16>(accent);
                } else if (count) return SfntStatus::malformed;
                ended = true;
                return close() ? SfntStatus::ok : geometry_error();
            }
            if (byte == 1 || byte == 3 || byte == 18 || byte == 23 || byte == 19 || byte == 20) {
                if (!width_seen && (count & 1u)) {
                    advance=nominal_width+stack[0];
                    if (!bounded(advance)) return SfntStatus::malformed;
                    for (u32 i=1;i<count;++i) stack[i-1]=stack[i]; --count;
                }
                width_seen = true;
                if (count & 1u) return SfntStatus::malformed;
                stems += count/2; count=0;
                if (stems > 96) return SfntStatus::malformed;
                if (byte == 19 || byte == 20) {
                    const u32 mask=(stems+7)/8;
                    if (mask > program.count-at) return SfntStatus::truncated;
                    at += mask;
                }
                continue;
            }
            bool ok = true;
            const auto* s = stack;
            switch (byte) {
            case 4: if (!width(1)) return SfntStatus::malformed; ok=move(0,s[0]); break;
            case 21: if (!width(2)) return SfntStatus::malformed; ok=move(s[0],s[1]); break;
            case 22: if (!width(1)) return SfntStatus::malformed; ok=move(s[0],0); break;
            case 5:
                if (!count || count%2) return SfntStatus::malformed;
                for (u32 i=0;ok && i<count;i+=2) ok=line(s[i],s[i+1]); break;
            case 6: case 7:
                if (!count) return SfntStatus::malformed;
                for (u32 i=0;ok && i<count;++i) {
                    const bool horizontal=(byte==6) != ((i&1u)!=0);
                    ok=line(horizontal?s[i]:0,horizontal?0:s[i]);
                }
                break;
            case 8:
                if (!count || count%6) return SfntStatus::malformed;
                for (u32 i=0;ok && i<count;i+=6) ok=curve(s[i],s[i+1],s[i+2],s[i+3],s[i+4],s[i+5]); break;
            case 24:
                if (count<8 || (count-2)%6) return SfntStatus::malformed;
                for (u32 i=0;ok && i<count-2;i+=6) ok=curve(s[i],s[i+1],s[i+2],s[i+3],s[i+4],s[i+5]);
                if (ok) ok=line(s[count-2],s[count-1]); break;
            case 25:
                if (count<8 || (count-6)%2) return SfntStatus::malformed;
                for (u32 i=0;ok && i<count-6;i+=2) ok=line(s[i],s[i+1]);
                if (ok) ok=curve(s[count-6],s[count-5],s[count-4],s[count-3],s[count-2],s[count-1]); break;
            case 26: case 27: {
                if (count<4 || (count%4>1)) return SfntStatus::malformed;
                u32 i=count&1u; double extra=i?s[0]:0;
                for (;ok && i<count;i+=4) {
                    ok=byte==26 ? curve(extra,s[i],s[i+1],s[i+2],0,s[i+3])
                                : curve(s[i],extra,s[i+1],s[i+2],s[i+3],0);
                    extra=0;
                }
                break;
            }
            case 30: case 31: {
                if (count<4 || (count%4>1)) return SfntStatus::malformed;
                bool horizontal=byte==31;
                for (u32 i=0;ok && i+3<count;i+=4) {
                    const double extra=count-i==5?s[i+4]:0;
                    ok=horizontal ? curve(s[i],0,s[i+1],s[i+2],extra,s[i+3])
                                  : curve(0,s[i],s[i+1],s[i+2],s[i+3],extra);
                    horizontal=!horizontal;
                }
                break;
            }
            default: return SfntStatus::unsupported;
            }
            count=0;
            if (!ok) return geometry_error();
        }
        return ended ? SfntStatus::ok : SfntStatus::truncated;
    }
};

template<bool Bold> SfntStatus decode(const CffFont& font, u16 glyph, double dx, double dy,
                    GlyphOutline& output, u32& work, u32 depth, CffGlyphMetrics* metrics,bool oblique) noexcept {
    if (glyph >= font.glyph_count) return SfntStatus::glyph_out_of_range;
    if (depth > 8) return SfntStatus::resource_limit;
    VirtualMachine<Bold> vm{font, {}, output, work, metrics};
    auto status=cff_detail::select_dict(font,glyph,&vm.local_subrs,vm.matrix,&vm.random,&vm.advance,&vm.nominal_width);
    if (status!=SfntStatus::ok) return status;
    if (vm.matrix[0]>0 && vm.matrix[0]==vm.matrix[3] && !vm.matrix[1] && !vm.matrix[2] && !vm.matrix[4] && !vm.matrix[5]) {
        // Windows quantizes uniform scaling in design space to 16.16 before the
        // final single-precision em conversion; a rounded reciprocal alone differs.
        const double scaled=vm.matrix[0]*font.units_per_em*65536.0;
        if (scaled<=2147483647.0) {
            vm.design_scale=static_cast<i32>(scaled+.5);
            vm.em_scale=1.0f/font.units_per_em;
        }
    }
    if (!vm.random) vm.random=0x7384a119u;
    if((oblique||Bold)&&!vm.design_scale)return SfntStatus::unsupported;
    // The shear is resolved once per glyph in the same 16.16 matrix as the font scale.
    if(oblique)vm.oblique_scale=static_cast<i32>(vm.design_scale*(87.0/256.0)+.5);
    vm.translate_x=dx; vm.translate_y=dy;
    status=vm.run(cff_detail::index_object(font.charstrings,glyph),0);
    if (status==SfntStatus::ok && metrics && !depth) {
        metrics->advance_x=vm.design_scale?vm.advance*vm.design_scale/65536.0:vm.advance*vm.matrix[0]*font.units_per_em;
        metrics->advance_y=vm.design_scale?0:vm.advance*vm.matrix[1]*font.units_per_em;
        if (!bounded(metrics->advance_x) || !bounded(metrics->advance_y)) return SfntStatus::malformed;
    }
    if (status!=SfntStatus::ok || !vm.composite) return status;
    u16 base=0,accent=0;
    status=cff_detail::standard_glyph(font,vm.base_code,&base); if(status!=SfntStatus::ok)return status;
    status=cff_detail::standard_glyph(font,vm.accent_code,&accent); if(status!=SfntStatus::ok)return status;
    status=decode<Bold>(font,accent,dx+vm.accent_x,dy+vm.accent_y,output,work,depth+1,metrics,oblique); if(status!=SfntStatus::ok)return status;
    return decode<Bold>(font,base,dx,dy,output,work,depth+1,metrics,oblique);
}
#pragma float_control(pop)

template<bool Bold> SfntStatus outline(const CffFont& font, u16 glyph, GlyphOutline* output, u32 work_limit, CffGlyphMetrics* metrics,bool oblique) noexcept {
    if (metrics) *metrics={};
    if (!output) return SfntStatus::invalid_input;
    auto* commands=output->commands; const u32 capacity=output->capacity;
    *output={}; output->commands=commands; output->capacity=capacity; output->fill_rule=GlyphFillRule::winding;
    if (!commands || !font.table.data || !font.charstrings.count) return SfntStatus::invalid_input;
    auto status=decode<Bold>(font,glyph,0,0,*output,work_limit,0,metrics,oblique);
    if constexpr (Bold) { if(status==SfntStatus::ok)status=cff_bolden(font,*output,work_limit,oblique); }
    if(status!=SfntStatus::ok) {
        *output={}; output->commands=commands; output->capacity=capacity;
        output->fill_rule=GlyphFillRule::winding; output->rejected=1;
        if (metrics) *metrics={};
    }
    return status;
}

} // namespace

SfntStatus cff_outline(const CffFont& font, u16 glyph, GlyphOutline* output, u32 work_limit, CffGlyphMetrics* metrics,bool oblique) noexcept {
    return outline<false>(font,glyph,output,work_limit,metrics,oblique);
}
SfntStatus cff_outline_bold(const CffFont& font, u16 glyph, GlyphOutline* output, u32 work_limit, CffGlyphMetrics* metrics,bool oblique) noexcept {
    return outline<true>(font,glyph,output,work_limit,metrics,oblique);
}

} // namespace maicord
