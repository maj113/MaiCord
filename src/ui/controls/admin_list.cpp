#include "maicord/ui/controls/settings_controls.hpp"
#include "maicord/core/utf.hpp"
#include "maicord/ui/controls/settings_tokens.hpp"
#include "maicord/ui/icon.hpp"

namespace maicord {
namespace {
// Chip labels draw as description text (13 DIP); the estimate errs wide so
// short chord labels such as "Ctrl+Alt+M" never ellipsize.
constexpr float kChipEm = 13.f, kChipEstimateSlack = 1.12f;
} // namespace

bool ui_admin_chip_dot(Utf8View label, bool dots) noexcept {
    return dots && !(label.bytes && label.data[0] == '+');
}

float ui_admin_chip_width(const UiSettingsTokens& tokens, Utf8View label, bool dot) noexcept {
    const float text = minimum(160.f, ui_settings_text_width(label, kChipEm) * kChipEstimateSlack);
    return text + tokens.metric.chip_padding * 2 + (dot ? tokens.metric.accent_dot - 2 + 6 : 0);
}

UiRect ui_admin_chip_rect(const UiSettingsTokens& tokens, ConstSlice<Utf8View> labels, bool dots, UiRect area,
                          u32 index) noexcept {
    const auto& m = tokens.metric;
    float x = area.left, y = area.top;
    for (u32 i = 0; i < labels.count; ++i) {
        const bool dot = ui_admin_chip_dot(labels[i], dots);
        const float width = minimum(ui_admin_chip_width(tokens, labels[i], dot), ui_rect_width(area));
        if (x > area.left && x + width > area.right) {
            x = area.left;
            y += m.chip_height + m.chip_gap;
        }
        if (i == index)
            return {x, y, x + width, y + m.chip_height};
        x += width + m.chip_gap;
    }
    return {};
}

u32 ui_admin_chip_rows(const UiSettingsTokens& tokens, ConstSlice<Utf8View> labels, bool dots, float width) noexcept {
    if (!labels.count)
        return 0;
    const auto last = ui_admin_chip_rect(tokens, labels, dots, {0, 0, maximum(1.f, width), 0}, labels.count - 1);
    return static_cast<u32>(last.top / (tokens.metric.chip_height + tokens.metric.chip_gap) + .5f) + 1;
}

float ui_admin_chips_height(const UiSettingsTokens& tokens, u32 rows) noexcept {
    return rows ? rows * tokens.metric.chip_height + (rows - 1) * tokens.metric.chip_gap : 0;
}

bool ui_admin_chip_paint(PaintStream* paint, const UiSettingsTokens& tokens, UiRect chip, Color dot, bool has_dot,
                         float alpha) noexcept {
    const auto& c = tokens.color;
    const float radius = ui_rect_height(chip) * .5f;
    bool ok = paint_stream_push_rounded_border(paint, chip.left, chip.top, chip.right, chip.bottom,
                                               ui_settings_color_alpha(c.mod_subtle, alpha),
                                               ui_settings_color_alpha(c.border_subtle, alpha), radius, 1);
    if (ok && has_dot) {
        const float size = tokens.metric.accent_dot - 2, cy = (chip.top + chip.bottom) * .5f;
        const float left = chip.left + tokens.metric.chip_padding - 2;
        ok = paint_stream_push_rounded(paint, left, cy - size * .5f, left + size, cy + size * .5f,
                                       ui_settings_color_alpha(dot, alpha), size * .5f);
    }
    return ok;
}

UiRect ui_admin_chip_label_rect(const UiSettingsTokens& tokens, UiRect chip, bool dot) noexcept {
    const float left = chip.left + tokens.metric.chip_padding + (dot ? tokens.metric.accent_dot - 2 + 4 : 0);
    return {left, chip.top, chip.right - tokens.metric.chip_padding + 2, chip.bottom};
}

bool ui_admin_grip_paint(PaintStream* paint, const UiSettingsTokens& tokens, UiRect lead, bool hovered,
                         bool enabled) noexcept {
    const float size = 18, cx = lead.left + tokens.metric.grip_width * .5f, cy = (lead.top + lead.bottom) * .5f;
    const Color color = hovered ? tokens.color.text_default : tokens.color.text_subtle;
    return ui_icon_paint(paint, UiIcon::grip_vertical, {cx - size * .5f, cy - size * .5f, cx + size * .5f, cy + size * .5f},
                         ui_settings_color_alpha(color, enabled ? (hovered ? 1.f : .7f) : .3f));
}

bool ui_admin_dot_paint(PaintStream* paint, const UiSettingsTokens& tokens, UiRect lead, Color color,
                        float alpha) noexcept {
    const float size = tokens.metric.accent_dot, cy = (lead.top + lead.bottom) * .5f;
    return paint_stream_push_rounded(paint, lead.left, cy - size * .5f, lead.left + size, cy + size * .5f,
                                     ui_settings_color_alpha(color, alpha), size * .5f);
}

bool ui_admin_avatar_paint(PaintStream* paint, const UiSettingsTokens& tokens, UiRect avatar, Color fill,
                           float alpha) noexcept {
    const float radius = ui_rect_width(avatar) * .5f;
    return paint_stream_push_rounded_border(paint, avatar.left, avatar.top, avatar.right, avatar.bottom,
                                            ui_settings_color_alpha(fill, alpha),
                                            ui_settings_color_alpha(tokens.color.border_subtle, alpha), radius, 1);
}

Utf8View ui_admin_initials(Utf8View label, Slice<char> storage) noexcept {
    u32 used = 0, words = 0;
    bool boundary = true;
    for (u32 offset = 0; offset < label.bytes && words < 2;) {
        const auto scalar = utf8_decode_one(label.data + offset, label.bytes - offset);
        if (!scalar.valid)
            break;
        const bool space = scalar.codepoint == ' ' || scalar.codepoint == '-' || scalar.codepoint == '_';
        // A lowercase second word reads as a sentence, not a name: keep one initial.
        if (boundary && !space && words == 1 && scalar.codepoint >= 'a' && scalar.codepoint <= 'z')
            break;
        if (boundary && !space) {
            if (used + scalar.width > storage.count)
                break;
            for (u32 i = 0; i < scalar.width; ++i)
                storage[used + i] = label.data[offset + i];
            if (scalar.width == 1 && storage[used] >= 'a' && storage[used] <= 'z')
                storage[used] = static_cast<char>(storage[used] - 32);
            used += scalar.width;
            ++words;
        }
        boundary = space;
        offset += scalar.width;
    }
    return {storage.data, used};
}

} // namespace maicord
