#include "maicord/ui/composer_voice_bar.hpp"

#include "maicord/core/decimal.hpp"
#include "maicord/ui/background_presentation.hpp"
#include "maicord/ui/paint.hpp"
#include "maicord/ui/text_presentation.hpp"
#include "maicord/ui/text_style.hpp"

namespace maicord {
namespace {
constexpr float kDot = 10.f, kGap = 8.f, kBar = 3.f, kBarGap = 2.f, kMinimumPeak = 2.f;
u32 time_text(u32 milliseconds, char* out) noexcept {
    const u32 seconds = milliseconds / 1000, minutes = seconds / 60;
    u32 used = 0;
    used += decimal_write(out + used, minimum(minutes, 999u));
    out[used++] = ':';
    out[used++] = static_cast<char>('0' + seconds % 60 / 10);
    out[used++] = static_cast<char>('0' + seconds % 10);
    return used;
}
bool rounded(PaintStream* paint, UiRect r, Color color, float radius) noexcept {
    return paint_stream_push_rounded(paint, r.left, r.top, r.right, r.bottom, color, radius);
}
} // namespace

UiComposerVoiceBarGeometry ui_composer_voice_bar_geometry(UiRect bar, const UiComposerVoiceView& view) noexcept {
    UiComposerVoiceBarGeometry g{};
    if (!ui_rect_has_area(bar))
        return g;
    const float center = (bar.top + bar.bottom) * .5f;
    float left = bar.left + 4.f;
    if (view.mode == UiComposerVoiceMode::recording) {
        g.dot = {left, center - kDot * .5f, left + kDot, center + kDot * .5f};
        left = g.dot.right + kGap;
    }
    const float time_width = view.mode == UiComposerVoiceMode::recording ? 44.f : 84.f;
    g.time = {left, bar.top, minimum(bar.right, left + time_width), bar.bottom};
    float right = bar.right - 4.f;
    if (view.status.bytes) {
        const float width = minimum(260.f, (bar.right - bar.left) * .45f);
        g.status = {maximum(g.time.right, right - width), bar.top, right, bar.bottom};
        right = g.status.left - kGap;
    }
    const float height = minimum(24.f, bar.bottom - bar.top);
    g.waveform = {g.time.right + kGap, center - height * .5f, maximum(g.time.right + kGap, right),
                  center + height * .5f};
    return g;
}

u32 ui_composer_voice_bar_count(float width) noexcept {
    return width > kBar ? static_cast<u32>((width + kBarGap) / (kBar + kBarGap)) : 0;
}

bool ui_composer_voice_bar_paint(const UiComposerVoiceView& view, UiRect bar, const Theme& theme,
                                 PaintStream* paint) noexcept {
    if (!paint)
        return false;
    if (!ui_composer_voice_bar_visible(view.mode) || !ui_rect_has_area(bar))
        return true;
    const auto g = ui_composer_voice_bar_geometry(bar, view);
    if (view.mode == UiComposerVoiceMode::recording &&
        !rounded(paint, g.dot, ui_theme_color_alpha(theme.tokens.accent.danger, view.pulse ? 1.f : .45f), kDot * .5f))
        return false;
    const u32 count = minimum(ui_composer_voice_bar_count(ui_rect_width(g.waveform)), 512u);
    if (!count)
        return true;
    const float center = (g.waveform.top + g.waveform.bottom) * .5f;
    const float peak_height = maximum(kMinimumPeak, ui_rect_height(g.waveform));
    const Color idle = ui_background_muted_text_color(theme);
    const Color live = view.mode == UiComposerVoiceMode::failed ? theme.tokens.text.danger : theme.text_primary;
    // Review shows the whole recording, played bars highlighted; sending fills
    // by upload progress. Recording scrolls the newest levels in from the right.
    const float played = view.mode == UiComposerVoiceMode::sending ? view.progress
                         : view.elapsed_ms && view.mode != UiComposerVoiceMode::recording
                             ? static_cast<float>(minimum(view.position_ms, view.elapsed_ms)) /
                                   static_cast<float>(view.elapsed_ms)
                             : 0.f;
    const u32 levels = minimum(view.level_count, kUiComposerVoiceLevels);
    for (u32 i = 0; i < count; ++i) {
        u8 value = 0;
        Color color = live;
        if (view.mode == UiComposerVoiceMode::recording) {
            // Bar i shows level (levels - count + i); older ones are idle.
            const u32 from_right = count - 1 - i;
            if (from_right < levels)
                value = view.levels[levels - 1 - from_right];
            else
                color = idle;
        } else {
            const u32 first = i * kUiComposerVoiceWaveform / count;
            const u32 end = maximum(first + 1, (i + 1) * kUiComposerVoiceWaveform / count);
            for (u32 s = first; s < end && s < kUiComposerVoiceWaveform; ++s)
                value = view.waveform[s] > value ? view.waveform[s] : value;
            if ((static_cast<float>(i) + .5f) / static_cast<float>(count) > played &&
                view.mode != UiComposerVoiceMode::failed)
                color = idle;
        }
        const float height = maximum(kMinimumPeak, peak_height * static_cast<float>(value) / 255.f);
        const float left = g.waveform.left + static_cast<float>(i) * (kBar + kBarGap);
        if (!rounded(paint, {left, center - height * .5f, left + kBar, center + height * .5f}, color, kBar * .5f))
            return false;
    }
    return true;
}

bool ui_composer_voice_bar_text(const UiComposerVoiceView& view, UiRect bar, UiRect clip, UiWidgetReference owner,
                                const Theme& theme, UiTextOutput* output) noexcept {
    if (!output)
        return false;
    if (!ui_composer_voice_bar_visible(view.mode) || !ui_rect_has_area(bar))
        return true;
    const auto g = ui_composer_voice_bar_geometry(bar, view);
    char time[24];
    u32 bytes = 0;
    if (view.mode == UiComposerVoiceMode::recording) {
        bytes = time_text(view.elapsed_ms, time);
    } else {
        bytes = time_text(minimum(view.position_ms, view.elapsed_ms), time);
        time[bytes++] = ' ';
        time[bytes++] = '/';
        time[bytes++] = ' ';
        bytes += time_text(view.elapsed_ms, time + bytes);
    }
    if (!ui_text_output_push_owned(output, {{time, bytes}, owner, g.time, ui_rect_intersection(clip, g.time),
                                            theme.text_primary, text_style_subtext, UiTextAlignment::start,
                                            UiTextAlignment::center, UiTextWrapping::no_wrap, UiTextOverflow::clip}))
        return false;
    return !view.status.bytes || !ui_rect_has_area(g.status) ||
           ui_text_output_push(output, {view.status, owner, g.status, ui_rect_intersection(clip, g.status),
                                        view.error ? theme.tokens.text.danger : ui_background_muted_text_color(theme),
                                        text_style_subtext, UiTextAlignment::end, UiTextAlignment::center,
                                        UiTextWrapping::no_wrap, UiTextOverflow::ellipsis});
}

} // namespace maicord
