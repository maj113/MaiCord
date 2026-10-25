#pragma once
#include "maicord/render/text_form_surface.hpp"

namespace maicord {
struct ForumPostScene;

struct ForumPostSurface {
    const ForumPostScene* scene;
    TextEditLayoutOwner layouts[2];
    TextEditMetric metrics[2][kTextEditSelectionCapacity];
    UiRect selections[2][kTextEditSelectionCapacity];
    TextFormCompositionMark composition[2][kTextCompositionSpanCapacity];
    TextFormEditGeometry geometry[2];
    bool composition_complete[2];
};
bool forum_post_surface_update(ForumPostSurface*, const TextSystem&) noexcept;
void forum_post_surface_release(ForumPostSurface*) noexcept;
} // namespace maicord
