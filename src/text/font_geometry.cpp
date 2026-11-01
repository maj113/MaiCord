#include "maicord/text/font_geometry.hpp"
#include "maicord/core/memory.hpp"
#include "sfnt_read.hpp"
namespace maicord {
using namespace sfnt_read;
SfntStatus font_geometry_open(const SfntFace& face,FontGeometry* output) noexcept {
    if(!output)return SfntStatus::invalid_input;reset_value(output);auto& font=*output;font.face=face;
    const bool cff=face.version==sfnt_tag('O','T','T','O');font.kind=cff?FontGeometryKind::cff:FontGeometryKind::truetype;
    auto status=cff?cff_open(face,&font.outline.cff):sfnt_truetype_open(face,&font.outline.truetype,true);
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
    if(bold&&font.kind==FontGeometryKind::cff){clear(output);output->rejected=1;return SfntStatus::unsupported;}
    if(font.kind==FontGeometryKind::cff)return cff_outline(font.outline.cff,glyph,output,work,cff_metrics,oblique);
    scratch->truetype.work_limit=work;
    SfntOutlineVariation variation{instance.variation,scratch->variation,scratch->deltas};
    return sfnt_truetype_outline(font.outline.truetype,glyph,&scratch->truetype,output,SfntOutlineMode::text,font.gvar.table.data?&variation:nullptr,scratch->instructions,oblique,bold);
}
} // namespace maicord
