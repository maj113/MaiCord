#pragma once
#include "maicord/ui/text_form.hpp"
#include "maicord/render/text_edit_layout.hpp"

namespace maicord {

// Caller-owned, instantiated only while a form needs presentation. Unbind from
// the renderer before release. Scene/text storage outlives every rebuild/draw.
struct TextFormSurface {
    const TextFormScene* scene;
    TextEditLayoutOwner layout;
    TextEditMetric metrics[kTextEditSelectionCapacity];
    UiRect selections[kTextEditSelectionCapacity];
    TextFormEditGeometry geometry;
    TextFormCompositionMark composition[kTextCompositionSpanCapacity];
    bool composition_complete;
};
bool text_form_surface_update(TextFormSurface* surface, const TextSystem& text) noexcept;
void text_form_surface_release(TextFormSurface* surface) noexcept;

} // namespace maicord
