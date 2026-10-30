#pragma once

#include "maicord/ui/paint.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/media/asset.hpp"

namespace maicord {
struct PopupScene;
struct UiEditGeometry;
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
    // Corner radius of an avatar quad; zero keeps the round avatar mask.
    float corner_radius;
};
using PopupEditGeometry = UiEditGeometry;

// measure (optional) lays out the command picker's usage chips.
bool popup_presentation_build(const PopupScene& scene, const Theme& theme, PaintStream* paint,
                              UiTextMeasureCache* measure = nullptr) noexcept;
PopupMediaPresentationStatus popup_media_presentation_resolve(const PopupScene& scene, u32 index,
                                                              PopupMediaCommand* command) noexcept;
// The media key of a resolved popup picture (request, residency and draw share it).
MediaAssetKey popup_media_asset_key(const PopupMediaCommand& command) noexcept;
// A picture fetched by URL (kPopupItemMediaUrl; media_variant is its
// MediaAttachmentFormat): its key from the URL's hash, as profile media key URLs.
MediaAssetKey popup_media_url_key(u64 url_hash, MediaAttachmentFormat format) noexcept;
bool popup_text_presentation_build_output(const PopupScene& scene, const Theme& theme,
                                          UiTextOutput* text, UiTextMeasureCache* measure = nullptr) noexcept;
} // namespace maicord
