#include "maicord/media/image/regional_flag_art.hpp"

namespace maicord { namespace {
struct Source {const RegionalFlagArt* art; u32 index=0; bool child=false;};
ColrPaintElement leaf(const RegionalFlagLayer& layer,u32 index) noexcept {
    ColrPaintElement element{};element.kind=ColrPaintKind::solid_glyph;
    element.paint.solidGlyph.glyphIndex=index;element.paint.solidGlyph.color.value=layer.color;return element;
}
} // namespace

bool regional_flag_compile(const RegionalFlagArt& art, ColrPaintProgram* program) noexcept {
    Source source{&art};
    ColrPaintSource callbacks{&source,
        +[](void* p,ColrPaintElement* e) noexcept {auto& s=*static_cast<Source*>(p);if(s.child)return false;s.index=0;s.child=true;*e=leaf(s.art->layers[0],0);return true;},
        +[](void* p,ColrPaintElement* e) noexcept {auto& s=*static_cast<Source*>(p);if(!s.child||s.index+1>=s.art->count)return false;++s.index;*e=leaf(s.art->layers[s.index],s.index);return true;},
        +[](void* p) noexcept {auto& s=*static_cast<Source*>(p);if(!s.child)return false;s.child=false;return true;},
        +[](void*,Slice<ColrGradientStop>) noexcept {return false;},
        +[](void* p,u16 id,GlyphOutline* out) noexcept {const auto& s=*static_cast<Source*>(p);return id<s.art->count&&regional_flag_layer_outline(s.art->layers[id],out);},
        +[](void* p,u16 id) noexcept {const auto& s=*static_cast<Source*>(p);return id<s.art->count?s.art->layers[id].command_bound:0u;}};
    ColrPaintElement root_element{};root_element.kind=ColrPaintKind::layers;root_element.paint.layers.childCount=art.count;
    return colr_program_build(program,callbacks,root_element,{0,0,36,36},kRegionalFlagPixelsPerUnit);
}
} // namespace maicord
