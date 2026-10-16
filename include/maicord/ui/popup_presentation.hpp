#pragma once

#include "maicord/ui/paint.hpp"
#include "maicord/ui/popup_scene.hpp"
#include "maicord/ui/text_presentation.hpp"

namespace maicord {
struct UiTextMeasureCache;

enum class PopupMediaPresentationStatus : u8 {
    skip,
    ready,
    invalid,
};

struct PopupMediaCommand {
    UiRect rectangle;
    UiRect clip;
    u64 media_id;
    u8 media_flags;
    u32 media_variant;
    bool avatar;
    bool guild_icon;
    u16 regional_flag;
};
using PopupEditGeometry = UiEditGeometry;

bool popup_presentation_build(const PopupScene& scene, const Theme& theme, PaintStream* paint) noexcept;
UiRect popup_avatar_rectangle(const UiLayoutNode& row) noexcept;
bool popup_text_presentation_build(const PopupScene& scene, const Theme& theme,
                                   UiTextCommandStream* text, UiTextMeasureCache* measure = nullptr) noexcept;
PopupMediaPresentationStatus popup_media_presentation_resolve(const PopupScene& scene, u32 index,
                                                              PopupMediaCommand* command) noexcept;

} // namespace maicord

namespace maicord {
bool popup_text_presentation_build_output(const PopupScene& scene, const Theme& theme,
                                          UiTextOutput* text, UiTextMeasureCache* measure = nullptr) noexcept;
} // namespace maicord
