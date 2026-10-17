#pragma once
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/text_style.hpp"

namespace maicord {
inline constexpr u32 kUiMentionBadgeMaximumTextBytes = 3;
inline constexpr float kUiMentionBadgeHeight = 16.0f;

// Public numeric badges display exact counts below 1000, then 1k+ through
// 9k+. The model retains its complete count independently of this label.
inline Utf8View ui_mention_badge_label(u32 count, Slice<char> output) noexcept {
    const u32 bytes = count < 10 ? 1u : count < 100 ? 2u : 3u;
    if (!count || !output.data || output.count < bytes) return {};
    if (count >= 1000) {
        output.data[0] = static_cast<char>('0' + minimum(count / 1000, 9u));
        output.data[1] = 'k'; output.data[2] = '+';
    } else {
        for (u32 index = bytes; index; --index) {
            output.data[index - 1] = static_cast<char>('0' + count % 10);
            count /= 10;
        }
    }
    return {output.data, bytes};
}
inline float ui_mention_badge_width(u32 count) noexcept {
    return count < 10 ? 16.0f : count < 100 ? 22.0f : 30.0f;
}
inline UiRect ui_mention_badge_guild_rectangle(UiRect row, u32 count) noexcept {
    return {row.right + 2.0f - ui_mention_badge_width(count), row.bottom - 14.0f,
            row.right + 2.0f, row.bottom + 2.0f};
}
inline bool ui_mention_badge_text_output(u32 count, const UiWidgetManager& manager,
    UiWidgetHandle owner, UiRect rectangle, UiRect clip, UiTextOutput* stream) noexcept {
    if (!count) return true;
    char bytes[kUiMentionBadgeMaximumTextBytes];
    const auto label = ui_mention_badge_label(count, {bytes, sizeof(bytes)});
    return ui_text_output_push_owned(stream,
        {label, {&manager, owner}, rectangle, clip, {1, 1, 1, 1},
         text_style_subtext | text_style_bold,
         UiTextAlignment::center, UiTextAlignment::center,
         UiTextWrapping::no_wrap, UiTextOverflow::clip});
}
} // namespace maicord
