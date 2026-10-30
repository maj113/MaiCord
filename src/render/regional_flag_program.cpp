#include "maicord/media/regional_flag_svg.hpp"

namespace maicord { namespace {
struct Source {const RegionalFlagSvg* image; u32 index=0; bool child=false;};
ColrPaintElement leaf(const RegionalFlagSvgLayer& layer,u32 index) noexcept {
    ColrPaintElement element{};element.kind=ColrPaintKind::solid_glyph;
    element.paint.solidGlyph.glyphIndex=index;element.paint.solidGlyph.color.value=layer.color;return element;
}
} // namespace

bool regional_flag_svg_compile(const RegionalFlagSvg& image, ColrPaintProgram* program,float pixels_per_unit) noexcept {
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
