#include "maicord/media/regional_flag_svg.hpp"
#include "maicord/core/byte_range.hpp"

namespace maicord { namespace {
struct Source {const RegionalFlagSvg* image; u32 index=0; bool child=false;};
ColrPaintElement leaf(const RegionalFlagSvgLayer& layer,u32 index) noexcept {
    ColrPaintElement element{};element.kind=ColrPaintKind::solid_glyph;
    element.paint.solidGlyph.glyphIndex=index;element.paint.solidGlyph.color.value=layer.color;return element;
}
} // namespace

bool regional_flag_svg_compile(const RegionalFlagSvg& image, ColrPaintProgram* program,float pixels_per_unit) noexcept {
    const ConstSlice<u8> image_range{reinterpret_cast<const u8*>(&image),sizeof(image)};
    const ConstSlice<u8> program_range{reinterpret_cast<const u8*>(program),sizeof(ColrPaintProgram)};
    if(!program||!byte_range_valid(image_range)||!byte_range_valid(program_range)||
        byte_ranges_overlap(image_range,program_range)||!image.count||image.count>kRegionalFlagSvgMaximumLayers||
        !(pixels_per_unit>0&&pixels_per_unit<=64)||program->storage.reserved>8*1024*1024)return false;
    const ConstSlice<u8> arena_range{program->storage.base,static_cast<u32>(program->storage.reserved)};
    if(!byte_range_valid(arena_range)||byte_ranges_overlap(image_range,arena_range)||byte_ranges_overlap(program_range,arena_range))return false;
    for(u32 i=0;i<image.count;++i){const auto& layer=image.layers[i];
        const ConstSlice<u8> path{reinterpret_cast<const u8*>(layer.path.data),layer.path.bytes};
        if(!byte_range_valid(path)||path.count>kRegionalFlagSvgMaximumBytes||
            byte_ranges_overlap(path,program_range)||byte_ranges_overlap(path,arena_range))return false;
        const float channels[]{layer.color.r,layer.color.g,layer.color.b,layer.color.a};
        for(float channel:channels)if(!(channel>=0&&channel<=1))return false;
    }
    Source source{&image};
    ColrPaintSource callbacks{&source,
        +[](void* p,ColrPaintElement* e) noexcept {auto& s=*static_cast<Source*>(p);if(s.child)return false;s.index=0;s.child=true;*e=leaf(s.image->layers[0],0);return true;},
        +[](void* p,ColrPaintElement* e) noexcept {auto& s=*static_cast<Source*>(p);if(!s.child||s.index+1>=s.image->count)return false;++s.index;*e=leaf(s.image->layers[s.index],s.index);return true;},
        +[](void* p) noexcept {auto& s=*static_cast<Source*>(p);if(!s.child)return false;s.child=false;return true;},
        +[](void*,Slice<ColrGradientStop>) noexcept {return false;},
        +[](void* p,u16 id,GlyphOutline* out) noexcept {const auto& s=*static_cast<Source*>(p);return id<s.image->count&&regional_flag_svg_outline(s.image->layers[id],out);},
        +[](void* p,u16 id) noexcept {const auto& s=*static_cast<Source*>(p);
            if(id>=s.image->count)return 0u;const auto& layer=s.image->layers[id];
            // Every added line needs at least two input bytes; an arc's seven
            // parameters cover its at-most-four cubic pieces. Includes figure
            // starts and implicit fill closure, capped by kRegionalFlagSvgMaximumCommands.
            return layer.path.bytes?minimum(kRegionalFlagSvgMaximumCommands,layer.path.bytes/2+8):8u;}};
    ColrPaintElement root_element{};root_element.kind=ColrPaintKind::layers;root_element.paint.layers.childCount=image.count;
    return colr_program_build(program,callbacks,root_element,{0,0,36,36},pixels_per_unit);
}
} // namespace maicord
