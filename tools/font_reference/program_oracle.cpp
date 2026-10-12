#include "program_oracle.hpp"
#include "maicord/ui/text.hpp"
#include "maicord/text/truetype_outline.hpp"
#include "maicord/text/cff_outline.hpp"
#include "maicord/render/colr_source.hpp"
#include <d2d1.h>

namespace maicord {
namespace {
struct Source { IDWritePaintReader* reader; SfntTrueType tt; CffFont cff; bool is_tt,is_cff; };
SfntOutlinePoint points[65536];u32 ends[32768];u16 active[64];
double region_scalars[32767];ColrResolvedColorStop resolved_stops[65535];ColrSourceFrame source_frames[65];
ColrColor color(DWRITE_COLOR_F c) noexcept {return {c.r,c.g,c.b,c.a};}
bool convert(const DWRITE_PAINT_ELEMENT& in,ColrPaintElement* out) noexcept {
    *out={};out->kind=static_cast<ColrPaintKind>(in.paintType);auto& p=out->paint;const auto& q=in.paint;
    switch(in.paintType) {
    case DWRITE_PAINT_TYPE_NONE:break;
    case DWRITE_PAINT_TYPE_LAYERS:p.layers.childCount=q.layers.childCount;break;
    case DWRITE_PAINT_TYPE_SOLID:p.solid.value=color(q.solid.value);break;
    case DWRITE_PAINT_TYPE_SOLID_GLYPH:p.solidGlyph={q.solidGlyph.glyphIndex,{color(q.solidGlyph.color.value)}};break;
    case DWRITE_PAINT_TYPE_LINEAR_GRADIENT:{const auto& v=q.linearGradient;p.linearGradient={v.extendMode,v.gradientStopCount,v.x0,v.y0,v.x1,v.y1,v.x2,v.y2};break;}
    case DWRITE_PAINT_TYPE_RADIAL_GRADIENT:{const auto& v=q.radialGradient;p.radialGradient={v.extendMode,v.gradientStopCount,v.x0,v.y0,v.radius0,v.x1,v.y1,v.radius1};break;}
    case DWRITE_PAINT_TYPE_SWEEP_GRADIENT:{const auto& v=q.sweepGradient;p.sweepGradient={v.extendMode,v.gradientStopCount,v.centerX,v.centerY,v.startAngle,v.endAngle};break;}
    case DWRITE_PAINT_TYPE_GLYPH:p.glyph.glyphIndex=q.glyph.glyphIndex;break;
    case DWRITE_PAINT_TYPE_COLOR_GLYPH:{const auto& v=q.colorGlyph;p.colorGlyph={v.glyphIndex,{v.clipBox.left,v.clipBox.top,v.clipBox.right,v.clipBox.bottom}};break;}
    case DWRITE_PAINT_TYPE_TRANSFORM:{const auto& v=q.transform;p.transform={v.m11,v.m12,v.m21,v.m22,v.dx,v.dy};break;}
    case DWRITE_PAINT_TYPE_COMPOSITE:p.composite.mode=static_cast<u32>(q.composite.mode);break;
    default:return false;
    }
    return true;
}
bool first(void* context,ColrPaintElement* out) noexcept {DWRITE_PAINT_ELEMENT in{};return SUCCEEDED(static_cast<Source*>(context)->reader->MoveToFirstChild(&in)) && convert(in,out);}
bool next(void* context,ColrPaintElement* out) noexcept {DWRITE_PAINT_ELEMENT in{};return SUCCEEDED(static_cast<Source*>(context)->reader->MoveToNextSibling(&in)) && convert(in,out);}
bool parent(void* context) noexcept {return SUCCEEDED(static_cast<Source*>(context)->reader->MoveToParent());}
bool stops(void* context,Slice<ColrGradientStop> output) noexcept {
    D2D1_GRADIENT_STOP buffer[64];auto* reader=static_cast<Source*>(context)->reader;
    for(u32 at=0;at<output.count;at+=64) {
        const u32 count=minimum(64u,output.count-at);
        if(FAILED(reader->GetGradientStops(at,count,buffer)))return false;
        for(u32 i=0;i<count;++i)output[at+i]={buffer[i].position,{buffer[i].color.r,buffer[i].color.g,buffer[i].color.b,buffer[i].color.a}};
    }
    return true;
}
bool outline(void* context,u16 glyph,GlyphOutline* output) noexcept {
    auto* source=static_cast<Source*>(context);
    if(source->is_tt){SfntOutlineScratch scratch{{points,65536},{ends,32768},{active,64},0,0,1000000};return sfnt_truetype_outline(source->tt,glyph,&scratch,output,SfntOutlineMode::colr)==SfntStatus::ok;}
    if(source->is_cff)return cff_outline(source->cff,glyph,output)==SfntStatus::ok;
    return false;
}
u32 children(const ColrPaintElement& e) noexcept {
    switch(e.kind){case ColrPaintKind::layers:return e.paint.layers.childCount;
    case ColrPaintKind::glyph:case ColrPaintKind::color_glyph:case ColrPaintKind::transform:return 1;
    case ColrPaintKind::composite:return 2;default:return 0;}
}
bool elements(Source& old,const ColrPaintSource& source,const ColrPaintElement& a,const ColrPaintElement& b,
              ColrNativeSource& cursor,PaintProgramComparison* totals,u32 depth=0) noexcept {
    if(depth>64)return false;
    ++totals->element_comparisons;
    const auto* pa=reinterpret_cast<const u8*>(&a);const auto* pb=reinterpret_cast<const u8*>(&b);
    for(u32 i=0;i<sizeof(a);++i)if(pa[i]!=pb[i]) {
        if(!totals->element_mismatches){totals->element_kind=static_cast<u32>(a.kind);totals->element_field=i&~3u;
            copy_bytes(&totals->element_original,pa+(i&~3u),4);copy_bytes(&totals->element_candidate,pb+(i&~3u),4);
            totals->element_paint=cursor.frames[cursor.depth].offset;}
        ++totals->element_mismatches;return false;
    }
    const u32 count=children(a);if(!count)return true;
    ColrPaintElement ca{},cb{};
    if(!first(&old,&ca) || !source.first_child(source.context,&cb))return false;
    for(u32 i=0;i<count;++i){if(!elements(old,source,ca,cb,cursor,totals,depth+1))return false;
        if(i+1<count && (!next(&old,&ca) || !source.next_sibling(source.context,&cb)))return false;}
    return parent(&old) && source.parent(source.context);
}
}
void compare_paint_program(const SfntFace& native,IDWritePaintReader* reader,IDWriteFontFace* face,u16 glyph,float size,PaintProgramComparison* totals,bool native_paints,const ColrVariationInstance* selected_instance) noexcept {
    ColrPaintProgram original{},candidate{};DWRITE_PAINT_ELEMENT root{};D2D_RECT_F clip{};ColrPaintElement native_root{};
    Source context{};context.reader=reader;context.is_tt=sfnt_truetype_open(native,&context.tt)==SfntStatus::ok;
    if(!context.is_tt)context.is_cff=cff_open(native,&context.cff)==SfntStatus::ok;
    ColrPaintSource source{&context,first,next,parent,stops,outline};
    bool old_ok=SUCCEEDED(reader->SetTextColor({1,1,1,1})) && SUCCEEDED(reader->SetCurrentGlyph(glyph,&root,&clip)) && colr_paint_build(&original,reader,face,root,clip,size);
    bool new_ok=false;
    if(!native_paints)new_ok=SUCCEEDED(reader->SetCurrentGlyph(glyph,&root,&clip)) && convert(root,&native_root) &&
        colr_program_build(&candidate,source,native_root,{clip.left,clip.top,clip.right,clip.bottom},size);
    else {
        ColrFont font{};ColrVariationData data{};ColrVariationInstance instance{};ColrNativeSource cursor{};ColrRect box{};double coordinates[64]{};
        bool prepared=(context.is_tt || context.is_cff) && colr_open(native,&font)==SfntStatus::ok && colr_variation_data_open(font,&data)==SfntStatus::ok && data.store.axis_count<=64 &&
            colr_variation_instance_prepare(data,{coordinates,data.store.axis_count},{region_scalars,32767},&instance,1000000,ColrVariationMode::paint_reader)==SfntStatus::ok;
        cursor.font=&font;cursor.variation=selected_instance?selected_instance:&instance;cursor.frames={source_frames,65};cursor.stop_storage={resolved_stops,65535};
        cursor.outline_context=&context;cursor.outline_provider=outline;cursor.foreground={1,1,1,1};cursor.palette=0;
        cursor.inverse_em=1.f/(context.is_tt?context.tt.metrics.units_per_em:context.cff.units_per_em);
        new_ok=prepared && colr_source_begin(&cursor,glyph,1000000,&native_root,&box,&source)==SfntStatus::ok;
        if(new_ok){ColrPaintElement previous{};const auto mismatches=totals->element_mismatches;
            const bool compared=SUCCEEDED(reader->SetCurrentGlyph(glyph,&root,&clip)) && convert(root,&previous) && elements(context,source,previous,native_root,cursor,totals);
            if(!compared && mismatches==totals->element_mismatches)++totals->element_failures;
            new_ok=colr_source_begin(&cursor,glyph,1000000,&native_root,&box,&source)==SfntStatus::ok && colr_program_build(&candidate,source,native_root,box,size);}
    }
    ++totals->compared;
    if(!old_ok || !new_ok)++totals->failures;
    else {
        bool different=original.storage.offset!=candidate.storage.offset;u32 first_offset=~0u;
        const usize count=minimum(original.storage.offset,candidate.storage.offset);
        for(usize i=0;i<count;++i)if(original.storage.base[i]!=candidate.storage.base[i]){different=true;first_offset=static_cast<u32>(i);break;}
        const auto* a=reinterpret_cast<const u8*>(&original.first);const auto* b=reinterpret_cast<const u8*>(&candidate.first);
        for(usize i=0;i<sizeof(ColrPaintProgram)-sizeof(Arena);++i)if(a[i]!=b[i])different=true;
        if(different){if(!totals->differences){totals->first_glyph=glyph;totals->first_offset=first_offset;
            if(first_offset!=~0u){const u32 aligned=first_offset&~3u;copy_bytes(&totals->first_original,original.storage.base+aligned,4);copy_bytes(&totals->first_candidate,candidate.storage.base+aligned,4);}
            totals->node_kind=~0u;
            for(u32 at=original.first;at!=~0u;){const auto* n=reinterpret_cast<const ColrPaintNode*>(original.storage.base+at);
                if(first_offset>=at && first_offset<at+sizeof(ColrPaintNode)){totals->node_kind=n->kind;totals->node_field=(first_offset-at)&~3u;break;}
                if((n->kind<=3 || (n->kind>=8 && n->kind<=11)) && n->count && first_offset>=n->data && first_offset<n->data+n->count*20){totals->node_kind=100+n->kind;totals->node_field=(first_offset-n->data)&~3u;break;}
                at=n->next;}
            if(root.paintType==DWRITE_PAINT_TYPE_TRANSFORM && native_root.kind==ColrPaintKind::transform){copy_bytes(totals->root_original,&root.paint.transform,24);copy_bytes(totals->root_candidate,&native_root.paint.transform,24);}
        }++totals->differences;}
        totals->bytes+=original.storage.offset;
    }
    arena_release(&original.storage);arena_release(&candidate.storage);
}
} // namespace maicord
