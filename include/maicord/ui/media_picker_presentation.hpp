#pragma once
#include "maicord/ui/media_picker.hpp"
namespace maicord {
bool ui_media_picker_scene_paint(const UiMediaPickerScene&, const Theme&, PaintStream*,
                                 const UiEditGeometry*, u32 dpi) noexcept;
bool ui_media_picker_scene_paint_foreground(const UiMediaPickerScene&, const Theme&, PaintStream*) noexcept;
bool ui_media_picker_scene_text_output(const UiMediaPickerScene&, const Theme&, UiTextOutput*,
                                       const UiEditGeometry*) noexcept;
} // namespace maicord
