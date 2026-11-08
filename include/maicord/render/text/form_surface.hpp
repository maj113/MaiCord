#pragma once
#include "maicord/ui/text/form.hpp"
#include "maicord/render/text/edit_layout.hpp"

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
// One edit field (a form's, a forum post's title or body): its layout over `rectangle` and the geometry drawn from it,
// with selections and composition marks in the caller's arrays; *composition_complete goes false when the marks do not
// fit. A single-line field is centred vertically and its text area reaches the field's right edge; another wraps.
// False when the layout cannot be measured.
bool text_form_field_update(TextEditLayoutOwner* layout, TextEditMetric* metrics, UiRect* selections,
                            TextFormCompositionMark* composition, TextFormEditGeometry* geometry,
                            bool* composition_complete, const TextSystem& text, UiRect rectangle, Utf8View value,
                            u64 signature, u32 anchor_utf16, u32 caret_utf16,
                            ConstSlice<TextCompositionSpan> spans, bool single_line) noexcept;

} // namespace maicord
