#include "maicord/ui/widgets/kit.hpp"

namespace maicord {
namespace {
constexpr float kChipPadding = 8;
}

bool ui_card_hairline_paint(PaintStream* paint, const UiSettingsTokens& t, const UiRect& row) noexcept {
    return paint_stream_push(paint, row.left + t.metric.card_inset, row.bottom - 1, row.right - t.metric.card_inset,
                             row.bottom, t.color.hairline);
}

float ui_chip_width(float label_width) noexcept {
    return kChipPadding * 2 + label_width;
}
UiRect ui_chip_label_rect(const UiRect& chip) noexcept {
    const float left = chip.left + kChipPadding;
    return {left, chip.top, maximum(left, chip.right - kChipPadding), chip.bottom};
}
Color ui_chip_ink(const UiSettingsTokens& t, UiChipTone tone) noexcept {
    const auto& c = t.color;
    switch (tone) {
    case UiChipTone::positive:
        return c.positive;
    case UiChipTone::warning:
        return c.warning;
    case UiChipTone::dev:
        return c.critical_text;
    case UiChipTone::brand:
        return c.on_brand;
    default:
        return c.text_subtle;
    }
}
bool ui_chip_paint(PaintStream* paint, const UiSettingsTokens& t, const UiRect& chip, UiChipTone tone) noexcept {
    const auto& c = t.color;
    const Color fill = tone == UiChipTone::positive ? c.positive_tint
                       : tone == UiChipTone::warning ? c.warning_tint
                       : tone == UiChipTone::dev     ? c.critical_tint
                       : tone == UiChipTone::brand   ? c.brand
                                                     : c.mod_subtle;
    return paint_stream_push_rounded(paint, chip, fill, ui_rect_height(chip) * .5f);
}

} // namespace maicord
