#pragma once

namespace maicord {
struct PaintStream;
struct ReactionViewer;
struct Theme;
struct UiTextOutput;

bool reaction_viewer_presentation_build(const ReactionViewer& viewer,
                                        const Theme& theme,
                                        PaintStream* paint) noexcept;
bool reaction_viewer_text_presentation_build_output(
    const ReactionViewer& viewer, const Theme& theme,
    UiTextOutput* text) noexcept;
} // namespace maicord
