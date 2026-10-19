#include "maicord/render/colr_source.hpp"
#include "maicord/text/truetype_outline.hpp"
#include "maicord/core/byte_order.hpp"

namespace maicord {
namespace {
#pragma float_control(precise,on,push)
bool failed(ColrNativeSource& s,SfntStatus status) noexcept {s.status=status;return false;}
bool spend(ColrNativeSource& s) noexcept {if(!s.work)return failed(s,SfntStatus::resource_limit);--s.work;return true;}
float scalar(double value) noexcept {return static_cast<float>(value);}
float negative(float value) noexcept {return value==0?0:-value;}
float distance(const ColrNativeSource& s,double value) noexcept {return scalar(value)*s.inverse_em;}
bool color(ColrNativeSource& s,u16 entry,double alpha,ColrColor* output) noexcept {
    if(entry==0xffff)s.uses_foreground=true;
    *output=s.foreground;
    if(entry!=0xffff) {
        CpalColor c{};const auto status=cpal_color(s.font->palette,s.palette,entry,&c);if(status!=SfntStatus::ok)return failed(s,status);
        constexpr float unit=1.f/255.f;
        *output={c.red*unit,c.green*unit,c.blue*unit,c.alpha*unit};
    }
    output->a*=scalar(alpha);return true;
}
bool clip_box(ColrNativeSource& s,u16 glyph,ColrRect* output) noexcept {
    ColrClip raw{},clip{};auto status=colr_clip(*s.font,glyph,&raw);
    if(status==SfntStatus::ok)status=colr_resolve_clip(*s.variation,raw,&clip);
    if(status!=SfntStatus::ok)return failed(s,status);
    if(!clip.present){*output={};return true;}
    *output={distance(s,clip.minimum_x),-distance(s,clip.maximum_y),distance(s,clip.maximum_x),-distance(s,clip.minimum_y)};return true;
}
// Reduce in half-turn units before evaluating a small-domain Taylor series.
// Windows numeric behavior is reproduced explicitly, not assumed from
// real-number equivalence.
double product_error(double a,double b,double product) noexcept {
    constexpr double split=134217729.0;
    const double ca=split*a,cb=split*b,ah=ca-(ca-a),bh=cb-(cb-b),al=a-ah,bl=b-bh;
    return ((ah*bh-product)+ah*bl+al*bh)+al*bl;
}
void sincos_halfturn(double angle,bool quantized,double* sine,double* cosine,bool short_polynomial=true) noexcept {
    constexpr double pi=3.1415926535897932384626433832795;
    double x=0,sign=1;
    if(quantized) {
        // Windows first converts half turns to float radians. Preserve that
        // rounding even for large varied angles, then reduce accurately.
        const float radians=scalar(angle)*3.1415927410125732421875f;
        if(short_polynomial) {
            // The Windows reader math uses ninth/tenth-degree Taylor kernels
            // on a nearest-quadrant reduction. Higher-degree evaluation can
            // round to a different float near a rounding boundary.
            const double quotient=static_cast<double>(radians)/(pi*.5);
            const i64 quadrant=static_cast<i64>(quotient+(quotient<0?-.5:.5));
            const double n=static_cast<double>(quadrant),product=n*(pi*.5);
            const double r=((static_cast<double>(radians)-product)-product_error(n,pi*.5,product))-n*6.123233995736766035e-17;
            const double z=r*r;
            const double sv=r+(r*z)*(-1.0/6+z*(1.0/120+z*(-1.0/5040+z/362880)));
            const double cv=(1-z*.5)+(z*z)*(1.0/24+z*(-1.0/720+z*(1.0/40320-z/3628800)));
            switch(static_cast<u64>(quadrant)&3u) {
            case 0:*sine=sv;*cosine=cv;break;
            case 1:*sine=cv;*cosine=-sv;break;
            case 2:*sine=-sv;*cosine=-cv;break;
            default:*sine=-cv;*cosine=sv;break;
            }
            return;
        }
        const double quotient=static_cast<double>(radians)/(2*pi);
        const i64 turns=static_cast<i64>(quotient+(quotient<0?-.5:.5));
        const double n=static_cast<double>(turns),product=n*(2*pi);
        x=((static_cast<double>(radians)-product)-product_error(n,2*pi,product))-n*2.449293598294706414e-16;
        if(x>pi*.5){x=pi-x;sign=-1;}else if(x < -pi*.5){x=-pi-x;sign=-1;}
    } else {
        const i64 turns=static_cast<i64>(angle*.5);angle-=static_cast<double>(turns)*2;
        if(angle>1)angle-=2;if(angle < -1)angle+=2;
        if(angle>.5){angle=1-angle;sign=-1;}else if(angle < -.5){angle=-1-angle;sign=-1;}
        x=angle*pi;
    }
    const double xx=x*x;
    double st=x,ct=1,sv=x,cv=1;
    for(u32 i=1;i<=12;++i){st*=-xx/((2*i)*(2*i+1.0));ct*=-xx/((2*i-1.0)*(2*i));sv+=st;cv+=ct;}
    *sine=sv;*cosine=cv*sign;
}
float tangent(double angle,bool quantized) noexcept {double s=0,c=0;sincos_halfturn(angle,quantized,&s,&c,false);return scalar(s/c);}
ColrMatrix multiply(ColrMatrix a,ColrMatrix b) noexcept {
    return {a.m11*b.m11+a.m12*b.m21,a.m11*b.m12+a.m12*b.m22,
            a.m21*b.m11+a.m22*b.m21,a.m21*b.m12+a.m22*b.m22,
            a.dx*b.m11+a.dy*b.m21+b.dx,a.dx*b.m12+a.dy*b.m22+b.dy};
}
ColrMatrix transform(const ColrNativeSource& s,u32 format,const double* v) noexcept {
    ColrMatrix m{1,0,0,1,0,0};float cx=0,cy=0;
    // Full basis conversion preserves signed zeros from the Windows matrix
    // products; manually changing signs is not bit-equivalent.
    if(format==12){const ColrMatrix flip{1,0,0,-1,0,0};const ColrMatrix raw{scalar(v[0]),scalar(v[1]),scalar(v[2]),scalar(v[3]),distance(s,v[4]),distance(s,v[5])};return multiply(multiply(flip,raw),flip);}
    if(format==14){m.dx=distance(s,v[0]);m.dy=-distance(s,v[1]);return m;}
    if(format==16 || format==18){m.m11=scalar(v[0]);m.m22=scalar(v[1]);if(format==18){cx=distance(s,v[2]);cy=-distance(s,v[3]);m.dx=cx*(1-m.m11);m.dy=cy*(1-m.m22);}return m;}
    if(format==20 || format==22){m.m11=m.m22=scalar(v[0]);if(format==22){cx=distance(s,v[1]);cy=-distance(s,v[2]);m.dx=cx*(1-m.m11);m.dy=cy*(1-m.m22);}return m;}
    const bool quantized=s.variation->mode==ColrVariationMode::quantized;
    if(format==24 || format==26){double sn=0,cs=0;sincos_halfturn(v[0],quantized,&sn,&cs);m.m11=m.m22=scalar(cs);m.m12=negative(scalar(sn));m.m21=scalar(sn);if(format==26){cx=distance(s,v[1]);cy=negative(distance(s,v[2]));}}
    if(format==28 || format==30){m.m21=tangent(v[0],quantized);m.m12=negative(tangent(v[1],quantized));if(format==30){cx=distance(s,v[2]);cy=negative(distance(s,v[3]));}}
    m.dx=cx-(cx*m.m11+cy*m.m21);m.dy=cy-(cx*m.m12+cy*m.m22);
    return m;
}
bool resolve(ColrNativeSource& s,ColrSourceFrame& frame,const ColrPaint& raw,ColrPaintElement* out) noexcept {
    if(!spend(s))return false;*out={};auto& p=out->paint;frame.child_count=0;
    frame.offset=raw.offset;frame.format=raw.format;
    if(frame.legacy) {
        if(raw.format==0){frame.first_layer=raw.first_layer;out->kind=ColrPaintKind::layers;frame.child_count=raw.child[0];p.layers.childCount=frame.child_count;return true;}
        out->kind=ColrPaintKind::solid_glyph;p.solidGlyph.glyphIndex=raw.glyph;return color(s,raw.palette,1,&p.solidGlyph.color.value);
    }
    ColrPaintValues values;const double* v=raw.values;
    if(raw.variable || (raw.format==2 && s.variation->mode==ColrVariationMode::standard)) {
        const auto status=colr_resolve_paint_values(*s.variation,raw,&values);
        if(status!=SfntStatus::ok)return failed(s,status);v=values.values;
    }
    const u32 format=raw.variable?raw.format-1:raw.format;
    if(format==1){frame.first_layer=raw.first_layer;out->kind=ColrPaintKind::layers;p.layers.childCount=frame.child_count=raw.layer_count;return true;}
    if(format==2){out->kind=ColrPaintKind::solid;return color(s,raw.palette,v[0],&p.solid.value);}
    if(format==4 || format==6 || format==8) {
        frame.color_line=raw.color_line;frame.variable=raw.variable;
        ColrColorLine line{};const auto status=colr_color_line(*s.font,raw,&line);if(status!=SfntStatus::ok)return failed(s,status);
        frame.stop_count=line.count;
        // OpenType extend values pass through: 0 pad, 1 repeat, 2 reflect.
        const u32 extend=line.extend;
        if(format==4){out->kind=ColrPaintKind::linear_gradient;p.linearGradient={extend,line.count,distance(s,v[0]),-distance(s,v[1]),distance(s,v[2]),-distance(s,v[3]),distance(s,v[4]),-distance(s,v[5])};}
        else if(format==6){
            // Quantized mode sign-extends encoded radius words; standard COLR
            // radii stay unsigned before variations apply.
            const bool quantized=s.variation->mode==ColrVariationMode::quantized;
            const double r0=v[2]-(quantized && raw.values[2]>=32768?65536:0),r1=v[5]-(quantized && raw.values[5]>=32768?65536:0);
            out->kind=ColrPaintKind::radial_gradient;p.radialGradient={extend,line.count,distance(s,v[0]),-distance(s,v[1]),distance(s,r0),distance(s,v[3]),-distance(s,v[4]),distance(s,r1)};
        }
        else {out->kind=ColrPaintKind::sweep_gradient;p.sweepGradient={extend,line.count,distance(s,v[0]),-distance(s,v[1]),scalar((v[2]+1)*180),scalar((v[3]+1)*180)};}
        return true;
    }
    if(format==10) {
        // The parent reader validated the child format byte. Decode a solid
        // here only when fusing it with the glyph; traversal validates every
        // other child before use, without decoding that record twice.
        const u8 child_format=s.font->table[raw.child[0]];
        if(child_format==2) {
            // A static solid is consumed entirely by this fused operation.
            // Validate its complete record before reading palette and alpha.
            if(s.font->table.count-raw.child[0]<5)return failed(s,SfntStatus::truncated);
            const auto* encoded=s.font->table.data+raw.child[0];
            const u16 palette=byte_load_be16(encoded+1);
            if(palette!=0xffff && palette>=s.font->palette.entry_count)return failed(s,SfntStatus::malformed);
            double alpha=static_cast<i16>(byte_load_be16(encoded+3))/16384.0;
            if(s.variation->mode==ColrVariationMode::standard)alpha=alpha<0?0:alpha>1?1:alpha;
            out->kind=ColrPaintKind::solid_glyph;p.solidGlyph.glyphIndex=raw.glyph;
            return color(s,palette,alpha,&p.solidGlyph.color.value);
        }
        if(child_format==3) {
            ColrPaint child;auto status=colr_paint_read(*s.font,raw.child[0],&child);if(status!=SfntStatus::ok)return failed(s,status);
            status=colr_resolve_paint_values(*s.variation,child,&values);if(status!=SfntStatus::ok)return failed(s,status);
            out->kind=ColrPaintKind::solid_glyph;p.solidGlyph.glyphIndex=raw.glyph;return color(s,child.palette,values.values[0],&p.solidGlyph.color.value);
        }
        out->kind=ColrPaintKind::glyph;p.glyph.glyphIndex=raw.glyph;frame.child[0]=raw.child[0];frame.child_count=1;return true;
    }
    if(format==11){frame.child[0]=raw.child[0];out->kind=ColrPaintKind::color_glyph;p.colorGlyph.glyphIndex=raw.glyph;frame.child_count=1;return clip_box(s,raw.glyph,&p.colorGlyph.clipBox);}
    if(format>=12 && format<=30){
        if(raw.variable && s.variation->mode==ColrVariationMode::quantized && format==18) {
            // Windows reader quirk in nonuniform VarScaleAroundCenter:
            // both center deltas modify X; Y retains its unvaried coordinate.
            values.values[2]+=values.values[3]-raw.values[3];values.values[3]=raw.values[3];
        }
        out->kind=ColrPaintKind::transform;p.transform=transform(s,format,v);frame.child[0]=raw.child[0];frame.child_count=1;return true;
    }
    if(format==32){frame.child[0]=raw.child[0];frame.child[1]=raw.child[1];out->kind=ColrPaintKind::composite;p.composite.mode=raw.mode;frame.child_count=2;return true;}
    return failed(s,SfntStatus::unsupported);
}
bool child(ColrNativeSource& s,u32 index,ColrPaintElement* out) noexcept {
    if(s.depth>=64 || s.depth+1>=s.frames.count)return failed(s,SfntStatus::resource_limit);
    const auto& parent=s.frames[s.depth];if(index>=parent.child_count)return failed(s,SfntStatus::invalid_input);
    // Only links survive traversal; decoded numeric fields stay local.
    auto& frame=s.frames[s.depth+1];frame.child_index=index;frame.child_count=0;frame.legacy=parent.legacy;
    ColrPaint raw;
    if(parent.legacy) {
        ColrLegacyLayer layer{};const auto status=colr_legacy_layer(*s.font,parent.first_layer+index,&layer);if(status!=SfntStatus::ok)return failed(s,status);
        raw={};raw.format=2;raw.glyph=layer.glyph;raw.palette=layer.palette;
    } else {
        u32 offset=0;
        if(parent.format==1){const auto status=colr_layer_paint(*s.font,parent.first_layer+index,&offset);if(status!=SfntStatus::ok)return failed(s,status);}
        else offset=parent.child[index];
        for(u32 i=0;i<=s.depth;++i)if(s.frames[i].offset==offset)return failed(s,SfntStatus::malformed);
        const auto status=colr_paint_read(*s.font,offset,&raw);if(status!=SfntStatus::ok)return failed(s,status);
    }
    ++s.depth;return resolve(s,frame,raw,out);
}
bool first(void* context,ColrPaintElement* out) noexcept {auto& s=*static_cast<ColrNativeSource*>(context);return s.status==SfntStatus::ok && child(s,0,out);}
bool next(void* context,ColrPaintElement* out) noexcept {
    auto& s=*static_cast<ColrNativeSource*>(context);if(s.status!=SfntStatus::ok || !s.depth)return false;
    const u32 index=s.frames[s.depth].child_index+1;--s.depth;return child(s,index,out);
}
bool parent(void* context) noexcept {auto& s=*static_cast<ColrNativeSource*>(context);if(s.status!=SfntStatus::ok || !s.depth)return false;--s.depth;return true;}
bool stops(void* context,Slice<ColrGradientStop> output) noexcept {
    auto& s=*static_cast<ColrNativeSource*>(context);if(s.status!=SfntStatus::ok)return false;
    const auto& frame=s.frames[s.depth];
    if(frame.format<4 || frame.format>9)return failed(s,SfntStatus::invalid_input);
    // The immutable font's complete line was validated when this leaf was
    // entered. Reuse its count instead of reconstructing and decoding a paint.
    const ColrColorLine line{{s.font->table.data+frame.color_line+3,frame.stop_count*(frame.variable?10u:6u)},
        static_cast<u16>(frame.stop_count),s.font->table[frame.color_line],frame.variable};
    if(output.count!=line.count || !output.data)return failed(s,SfntStatus::invalid_input);
    if(s.work<line.count)return failed(s,SfntStatus::resource_limit);s.work-=line.count;
    if(line.count>s.stop_storage.count && s.stop_arena) {
        arena_reset(s.stop_arena);
        auto* storage=arena_allocate<ColrResolvedColorStop>(s.stop_arena,line.count);
        if(!storage)return failed(s,SfntStatus::output_too_small);
        s.stop_storage={storage,line.count};
    }
    const auto status=colr_resolve_color_stops(*s.font,*s.variation,line,s.stop_storage);if(status!=SfntStatus::ok)return failed(s,status);
    for(u32 i=0;i<line.count;++i){output[i].position=scalar(s.stop_storage[i].position);if(!color(s,s.stop_storage[i].palette,s.stop_storage[i].alpha,&output[i].color))return false;}
    return true;
}
bool outline(void* context,u16 glyph,GlyphOutline* output) noexcept {
    auto& s=*static_cast<ColrNativeSource*>(context);return s.status==SfntStatus::ok && s.outline_provider(s.outline_context,glyph,output);
}
u32 outline_capacity(void* context,u16 glyph) noexcept {
    const auto& s=*static_cast<const ColrNativeSource*>(context);
    return sfnt_truetype_outline_capacity(*s.outline_capacity_font,glyph);
}
#pragma float_control(pop)
}
SfntStatus colr_source_begin(ColrNativeSource* source,u16 glyph,u32 work,ColrPaintElement* root,ColrRect* clip,ColrPaintSource* callbacks) noexcept {
    if(!source || !root || !clip || !callbacks)return SfntStatus::invalid_input;
    auto& s=*source;*root={};*clip={};*callbacks={};s.status=SfntStatus::ok;s.depth=0;s.work=work;s.uses_foreground=false;
    if(!s.font || !s.variation || !s.frames.count || !s.frames.data || !s.outline_provider || !(s.inverse_em>0) || s.palette>=s.font->palette.palette_count)return s.status=SfntStatus::invalid_input;
    ColrSourceFrame frame{};ColrPaint raw;u32 offset=0;auto status=colr_base_paint(*s.font,glyph,&offset);
    if(status==SfntStatus::unsupported_sequence) {
        ColrLayerRange range{};status=colr_legacy_layers(*s.font,glyph,&range);
        if(status==SfntStatus::ok){frame.legacy=true;raw={};raw.first_layer=range.first;raw.child[0]=range.count;}
    } else if(status==SfntStatus::ok)status=colr_paint_read(*s.font,offset,&raw);
    if(status!=SfntStatus::ok)return s.status=status;
    s.frames[0]=frame;
    if(!resolve(s,s.frames[0],raw,root) || !clip_box(s,glyph,clip))return s.status;
    *callbacks={&s,first,next,parent,stops,outline,s.outline_capacity_font?outline_capacity:nullptr};return SfntStatus::ok;
}
} // namespace maicord
