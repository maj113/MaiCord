#pragma once
#include "maicord/text/truetype_outline.hpp"
#include "maicord/text/cff_outline.hpp"
#include "maicord/text/font_axes.hpp"
namespace maicord {
enum class FontGeometryKind : u8 { truetype, cff };
struct FontGeometry {
    SfntFace face;
    union Outline {SfntTrueType truetype;CffFont cff;} outline;
    FontAxes axes;
    SfntGvar gvar;
    FontGeometryKind kind;
};
struct FontGeometryInstance {
    const FontGeometry* font;
    // Nonnull keeps the normalized fvar coordinates, including paint/layout-only
    // variation. Its SfntGvar payload is empty when outlines have no deltas;
    // only font->gvar.table.data allows calling outline-variation routines.
    const SfntGvarInstance* variation;
};
struct FontGeometryInstanceStorage {
    // Maps are temporary preparation scratch. Coordinates, scalars and the
    // variation object must stay unchanged for the resulting instance.
    Slice<AxisSegmentMap> maps;
    Slice<double> coordinates,shared_scalars;
    SfntGvarInstance* variation;
};
struct FontGeometryScratch {
    SfntOutlineScratch truetype;
    SfntGvarScratch variation;
    Slice<SfntGvarDelta> deltas;
    TtOutlineContext* instructions=nullptr;
};
// FontGeometry borrows the mapped font bytes, which are read-only. The mapping
// and this object must outlive every instance and every shaping/rendering user.
// Parse once per file face, prepare once per chosen axis instance. Base instances
// are shared by draw styles with equivalent preparation requirements.
SfntStatus font_geometry_open(const SfntFace& face,FontGeometry* output) noexcept;
// Instances without fvar axes need no variation storage and ignore axis settings.
SfntStatus font_geometry_instance(const FontGeometry& font,ConstSlice<FontAxisSetting> settings,
    FontGeometryInstanceStorage storage,FontGeometryInstance* output,u32 work_limit=1000000,u32* work_used=nullptr) noexcept;
// Output is normalized one-em raw/varied text contours; no rasterization or
// shaping. Scratch belongs to one active decode and can be reused across
// faces/instances. An optional prepared instruction context in scratch applies
// the implemented bytecode operations; the instance alone does not guarantee
// instruction/context compatibility. Optional oblique simulation transforms
// design geometry before emission. CFF metrics remain the base charstring
// metrics; GlyphOutline bounds describe the transformed ink.
// Bold is supported for static TrueType text outlines, before oblique; it needs
// prepared instructions when the font's prep policy affects widening. CFF and
// variable bold remain unsupported.
SfntStatus font_geometry_outline(const FontGeometryInstance& instance,u16 glyph,
    FontGeometryScratch* scratch,GlyphOutline* output,u32 work_limit=1000000,
    CffGlyphMetrics* cff_metrics=nullptr,bool oblique=false,bool bold=false) noexcept;
static_assert(sizeof(FontGeometryInstance)==16);
} // namespace maicord
