#pragma once

#include "maicord/ui/paint.hpp"
#include "maicord/ui/reaction_viewer.hpp"
#include "maicord/ui/text_presentation.hpp"

namespace maicord {

bool reaction_viewer_presentation_build(const ReactionViewer& viewer,
                                        const Theme& theme,
                                        PaintStream* paint) noexcept;
bool reaction_viewer_text_presentation_build(
    const ReactionViewer& viewer, const Theme& theme,
    UiTextCommandStream* text) noexcept;

} // namespace maicord

namespace maicord {
bool reaction_viewer_text_presentation_build_output(
    const ReactionViewer& viewer, const Theme& theme,
    UiTextOutput* text) noexcept;
} // namespace maicord
