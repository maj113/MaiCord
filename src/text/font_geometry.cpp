#include "maicord/text/font_geometry.hpp"
#include "sfnt_read.hpp"
namespace maicord {
using namespace sfnt_read;
SfntStatus font_geometry_open(const SfntFace& face,FontGeometry* output) noexcept {
    if(!output)return SfntStatus::invalid_input;*output={};FontGeometry font{};font.face=face;
    if(face.version==sfnt_tag('O','T','T','O')){
        font.kind=FontGeometryKind::cff;const auto s=cff_open(face,&font.outline.cff);if(s!=SfntStatus::ok)return s;
    }else{
        auto s=sfnt_truetype_open(face,&font.outline.truetype,true);if(s!=SfntStatus::ok)return s;
    }
    // Paint/layout variation axes exist independently of outline deltas.
    auto status=font_axes_open(face,&font.axes);if(status!=SfntStatus::ok)return status;
    if(font.kind==FontGeometryKind::truetype&&font.outline.truetype.gvar.data){
        status=sfnt_gvar_open(font.outline.truetype.gvar,font.axes.axis_count,font.outline.truetype.metrics.glyph_count,&font.gvar);if(status!=SfntStatus::ok)return status;}
    *output=font;return SfntStatus::ok;
}
SfntStatus font_geometry_instance(const FontGeometry& font,ConstSlice<FontAxisSetting> settings,
    FontGeometryInstanceStorage storage,FontGeometryInstance* output,u32 work,u32* used) noexcept {
    WorkReport report{work,work,used};
    if(!output)return SfntStatus::invalid_input;*output={};if(!font.face.file.data || (settings.count && !settings.data))return SfntStatus::invalid_input;
    if(!font.axes.axis_count){*output={&font,nullptr};return SfntStatus::ok;}
    if(!storage.variation)return SfntStatus::output_too_small;*storage.variation={};
    FontAxisMappings mappings{};u32 child=0;auto s=font_axis_mappings_open(font.face,font.axes.axis_count,storage.maps,&mappings,work,&child);work-=child;if(s!=SfntStatus::ok)return s;
    s=font_axis_coordinates(font.axes,mappings,settings,storage.coordinates,{},FontAxisPrecision::fixed16,work,&child);work-=child;if(s!=SfntStatus::ok)return s;
    if(font.gvar.table.data){s=sfnt_gvar_instance_prepare(font.gvar,{storage.coordinates.data,font.axes.axis_count},storage.shared_scalars,storage.variation,work,&child);work-=child;if(s!=SfntStatus::ok)return s;}
    else *storage.variation={{},{storage.coordinates.data,font.axes.axis_count},{}};
    *output={&font,storage.variation};return SfntStatus::ok;
}
SfntStatus font_geometry_outline(const FontGeometryInstance& instance,u16 glyph,
    FontGeometryScratch* scratch,GlyphOutline* output,u32 work,CffGlyphMetrics* cff_metrics,bool oblique,bool bold) noexcept {
    if(cff_metrics)*cff_metrics={};
    if(!instance.font || !output || (!scratch && instance.font->kind==FontGeometryKind::truetype)){if(output){auto* commands=output->commands;const u32 capacity=output->capacity;*output={};output->commands=commands;output->capacity=capacity;}return SfntStatus::invalid_input;}
    const auto& font=*instance.font;
    if(bold&&font.kind==FontGeometryKind::cff){auto* commands=output->commands;const u32 capacity=output->capacity;*output={};output->commands=commands;output->capacity=capacity;output->rejected=1;return SfntStatus::unsupported;}
    if(font.kind==FontGeometryKind::cff)return cff_outline(font.outline.cff,glyph,output,work,cff_metrics,oblique);
    scratch->truetype.work_limit=work;
    SfntOutlineVariation variation{instance.variation,scratch->variation,scratch->deltas};
    return sfnt_truetype_outline(font.outline.truetype,glyph,&scratch->truetype,output,SfntOutlineMode::text,font.gvar.table.data?&variation:nullptr,scratch->instructions,oblique,bold);
}
} // namespace maicord
