#include "maicord/text/fonts/geometry.hpp"
#include "maicord/core/memory.hpp"
#include "../opentype/sfnt_read.hpp"
namespace maicord {
using namespace sfnt_read;
// Once per face: compiled for size. Outline emission below stays /O2.
__attribute__((minsize)) SfntStatus font_geometry_open(const SfntFace& face,FontGeometry* output) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);auto& font=*output;font.face=face;
    const bool cff=face.version==sfnt_tag('O','T','T','O');font.kind=cff?FontGeometryKind::cff:FontGeometryKind::truetype;
    auto status=cff?cff_open(face,&font.outline.cff):sfnt_truetype_open(face,&font.outline.truetype,true);
#if !defined(_WIN32)
    // A colour bitmap font (CBLC/CBDT: Noto Color Emoji on Debian, Ubuntu and nixpkgs) has no outlines; its cmap, layout
    // tables and horizontal metrics are everything the text engine reads of it, and bitmap_color.hpp the images.
    if(!cff&&status==SfntStatus::missing_table){ConstSlice<u8> locations{},data{};
        if(sfnt_table(face,sfnt_tag('C','B','L','C'),&locations)==SfntStatus::ok&&sfnt_table(face,sfnt_tag('C','B','D','T'),&data)==SfntStatus::ok){
            reset_value(&font.outline.truetype);font.kind=FontGeometryKind::bitmap;status=sfnt_horizontal_metrics_open(face,&font.outline.truetype.metrics);}}
#endif
    // Paint/layout variation axes exist independently of outline deltas.
    if(status==SfntStatus::ok)status=font_axes_open(face,&font.axes);
    if(status==SfntStatus::ok&&!cff&&font.outline.truetype.gvar.data)
        status=sfnt_gvar_open(font.outline.truetype.gvar,font.axes.axis_count,font.outline.truetype.metrics.glyph_count,&font.gvar);
    if(status!=SfntStatus::ok)reset_value(output);
    return status;
}
namespace {
// Clears a result but keeps the caller's command storage.
void clear(GlyphOutline* output) noexcept {auto* commands=output->commands;const u32 capacity=output->capacity;reset_value(output);output->commands=commands;output->capacity=capacity;}
}
SfntStatus font_geometry_outline(const FontGeometryInstance& instance,u16 glyph,
    FontGeometryScratch* scratch,GlyphOutline* output,u32 work,CffGlyphMetrics* cff_metrics,bool oblique,bool bold) noexcept {
    if(cff_metrics)reset_value(cff_metrics);
    if(!instance.font || !output || (!scratch && instance.font->kind==FontGeometryKind::truetype)){if(output)clear(output);return SfntStatus::invalid_input;}
    const auto& font=*instance.font;
#if !defined(_WIN32)
    if(font.kind==FontGeometryKind::bitmap){clear(output);return SfntStatus::unsupported;}
#endif
    if(bold&&font.kind==FontGeometryKind::cff){clear(output);output->rejected=1;return SfntStatus::unsupported;}
    if(font.kind==FontGeometryKind::cff)return cff_outline(font.outline.cff,glyph,output,work,cff_metrics,oblique);
    scratch->truetype.work_limit=work;
    SfntOutlineVariation variation{instance.variation,scratch->variation,scratch->deltas};
    return sfnt_truetype_outline(font.outline.truetype,glyph,&scratch->truetype,output,SfntOutlineMode::text,font.gvar.table.data?&variation:nullptr,scratch->instructions,oblique,bold);
}
} // namespace maicord
