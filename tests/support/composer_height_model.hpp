#pragma once
// A model of the composer's height and width boundaries, for tests that lay out a shell or a composer viewport before
// a scene has measured its text (the client sizes the composer from the measured row,
// message_composer_scene_cached_height). Text is a 20 px line per 8 px column (at least 120 px wide), at most eight
// lines; the row, toolbar and chrome follow the scene's rules.
#include "maicord/core/utf.hpp"
#include "maicord/model/chat/message_composer.hpp"
#include "maicord/ui/chat/composer/message_composer_scene.hpp"
#include "maicord/ui/theme/style_projection.hpp"
#include "maicord/ui/theme/styles.hpp"

namespace maicord {

// The default theme bank's boundaries without plugin buttons: below the inline width the GIF and sticker buttons fold
// into the emoji picker, below the compact width the trailing actions wrap into toolbar rows.
// tests/ui/shell/composer_geometry.cpp checks both against the scene.
inline constexpr float kComposerInlineWidth = 380.f;
inline constexpr float kComposerCompactWidth = 264.f;

inline float message_composer_preferred_height(const MessageComposer& composer, float available_text_width) noexcept {
    const float usable_width = maximum(available_text_width, 120.0f);
    const u32 columns = maximum(1u, static_cast<u32>(usable_width / 8.0f));
    u32 lines = 1;
    u32 column = 0;
    u32 offset = 0;
    while (offset < composer.bytes) {
        const Utf8DecodeResult decoded = utf8_decode_one(composer.text + offset, composer.bytes - offset);
        if (!decoded.valid)
            break;
        if (decoded.codepoint == '\n' || ++column >= columns) {
            ++lines;
            column = 0;
        }
        offset += decoded.width;
    }
    const float height = 20.0f + static_cast<float>(minimum(lines, 8u)) * 20.0f;
    return minimum(maximum(height, kMessageComposerMinimumHeight), kMessageComposerMaximumHeight);
}

// attachments is a MessageComposerAttachmentRow. The typing strip above the surface is always part of the height.
inline float message_composer_scene_preferred_height(const MessageComposer& model, float composer_width,
                                                     bool reply_visible, u8 attachments_visible = 0,
                                                     const UiResolvedThemeStyles* styles = nullptr) noexcept {
    const auto editor_padding = [](const UiThemeMetrics& metrics) noexcept {
        return metrics.composer_padding + metrics.composer_padding + metrics.menu_padding + metrics.menu_padding;
    };
    const auto& metrics = ui_theme_metrics(styles);
    const float natural =
        message_composer_preferred_height(model, message_composer_scene_text_width(composer_width, styles)) +
        editor_padding(metrics) - editor_padding(ui_theme_metrics(nullptr));
    const float minimum_row = maximum(kMessageComposerMinimumHeight,
                                      maximum(32.f, metrics.control_height) + 2.f * metrics.composer_padding);
    return minimum(kMessageComposerMaximumHeight, maximum(minimum_row, natural)) +
           message_composer_scene_toolbar_height(composer_width, styles) +
           message_composer_scene_chrome_height(reply_visible, attachments_visible, styles);
}

} // namespace maicord
