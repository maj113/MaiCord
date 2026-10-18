#pragma once
#include "maicord/core/arena.hpp"
#include "maicord/text/outline.hpp"
#include <stddef.h>

namespace maicord {
struct ColrMatrix { float m11,m12,m21,m22,dx,dy; };
struct ColrRect { float left,top,right,bottom; };
struct ColrColor { float r,g,b,a; };
struct ColrGradientStop { float position; ColrColor color; };
enum class ColrPaintKind : u32 {
    none,layers,solid_glyph,solid,linear_gradient,radial_gradient,sweep_gradient,
    glyph,color_glyph,transform,composite
};
struct ColrPaintColor { ColrColor value; };
// Geometry is in em units, with Y down. Colors are straight-alpha sRGB.
// Sweep angles use counter-clockwise degrees from +X; extend and composite
// modes carry the COLRv1 numeric values. Children preserve paint order.
struct ColrPaintElement {
    ColrPaintKind kind;
    union {
        struct { u32 childCount; } layers;
        struct { u32 glyphIndex; ColrPaintColor color; } solidGlyph;
        ColrPaintColor solid;
        struct { u32 extendMode,gradientStopCount; float x0,y0,x1,y1,x2,y2; } linearGradient;
        struct { u32 extendMode,gradientStopCount; float x0,y0,radius0,x1,y1,radius1; } radialGradient;
        struct { u32 extendMode,gradientStopCount; float centerX,centerY,startAngle,endAngle; } sweepGradient;
        struct { u32 glyphIndex; } glyph;
        struct { u32 glyphIndex; ColrRect clipBox; } colorGlyph;
        ColrMatrix transform;
        struct { u32 mode; } composite;
    } paint;
};

// Postfix program records uploaded to the COLR compute shader; the layout is a GPU contract.
struct ColrPaintNode {
    u32 next,kind,data,count;
    u32 mode,reserved;
    float inverse[6],values[6];
};
struct ColrMask {
    u32 segments,segment_count,fill,rows;
    i32 left,top;
    u32 width,height;
    float bounds[4];
    u32 glyph,cache_next;
    ColrMatrix transform;
};
static_assert(sizeof(ColrMask)==80);
static_assert(offsetof(ColrMask,rows)==12);
static_assert(offsetof(ColrMask,left)==16);
static_assert(offsetof(ColrMask,bounds)==32);
struct ColrPaintProgram {
    Arena storage;
    u32 first,last,nodes,stack,peak_stack;
    GlyphOutline bounds;
    u32 paint_types;
    u32 mask_count,mask_pixels;
};
static_assert(sizeof(ColrPaintNode)==72);
static_assert(offsetof(ColrPaintNode,inverse)==24);
static_assert(offsetof(ColrPaintNode,values)==48);
static_assert(offsetof(ColrPaintNode,mode)==16);
static_assert(offsetof(ColrPaintNode,reserved)==20);
static_assert(sizeof(ColrGradientStop)==20);

// A traversal borrows one source, initially positioned at the supplied root.
// Stops belong to the current leaf. Outline output storage is caller-owned.
// Callbacks must not mutate the program arena or re-enter the same source.
struct ColrPaintSource {
    void* context;
    bool (*first_child)(void*,ColrPaintElement*) noexcept;
    bool (*next_sibling)(void*,ColrPaintElement*) noexcept;
    bool (*parent)(void*) noexcept;
    bool (*stops)(void*,Slice<ColrGradientStop>) noexcept;
    bool (*outline)(void*,u16,GlyphOutline*) noexcept;
    // Optional conservative command bound. Zero means unknown; the compiler
    // preserves its full 2,048-command capacity. This does not validate a glyph.
    u32 (*outline_capacity)(void*,u16) noexcept = nullptr;
};
// Failed output must not be submitted; the same program arena can be reset and reused.
bool colr_program_build(ColrPaintProgram* program,const ColrPaintSource& source,
                        const ColrPaintElement& root,ColrRect clip,float pixels_per_em) noexcept;
} // namespace maicord
